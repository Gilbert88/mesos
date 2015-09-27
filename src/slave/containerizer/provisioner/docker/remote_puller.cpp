/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "slave/containerizer/provisioner/docker/remote_puller.hpp"

#include <list>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/provisioner/docker/registry_client.hpp"

using std::list;
using std::string;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;
using process::Promise;
using process::Subprocess;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

using process::http::URL;

using PulledLayerInfo = RemotePuller::PulledLayerInfo;
using PulledImageInfo = RemotePuller::PulledImageInfo;

using FileSystemLayerInfo = registry::FileSystemLayerInfo;
using ManifestResponse = registry::ManifestResponse;
using RegistryClient = registry::RegistryClient;

const Duration DEFAULT_PULL_TIMEOUT_SECS = Seconds(30);
const string DEFAULT_TAG = "latest";
const double MANIFEST_TIMEOUT_PERCENT = 10;

class RemotePullerProcess : public Process<RemotePullerProcess>
{
public:
  static Try<Owned<RemotePullerProcess>> create(
      const URL& registry,
      const URL& authServer);

  process::Future<PulledImageInfo> pull(
      const Image::Name& imageName,
      const Path& downloadDir,
      const Option<Duration>& timeout,
      const Option<size_t>& maxSize);

private:
  explicit RemotePullerProcess(const Owned<RegistryClient>& registry);

  Try<string> getRemoteNameFromCanonicalName(
      const Image::Name& name,
      const Option<string>& tag);

  Future<PulledLayerInfo> downloadLayer(
      const string& remoteName,
      const Path& downloadDir,
      const FileSystemLayerInfo& layer,
      const Duration& timeout,
      const Option<size_t>& maxSize);

  Owned<RegistryClient> _registryClient;
  hashmap<string, Owned<Promise<PulledLayerInfo>>> _downloadTracker;

  RemotePullerProcess(const RemotePullerProcess&) = delete;
  RemotePullerProcess& operator=(const RemotePullerProcess&) = delete;
};


Try<Owned<RemotePuller>> RemotePuller::create(
    const URL& registry,
    const URL& authServer)
{
  Try<Owned<RemotePullerProcess>> process =
    RemotePullerProcess::create(registry, authServer);

  if (process.isError()) {
    return Error(process.error());
  }

  return Owned<RemotePuller>(new RemotePuller(process.get()));
}


RemotePuller::RemotePuller(const Owned<RemotePullerProcess>& process)
  : process_(process)
{
  spawn(CHECK_NOTNULL(process_.get()));
}


RemotePuller::~RemotePuller()
{
  terminate(process_.get());
  process::wait(process_.get());
}


Future<PulledImageInfo> RemotePuller::pull(
    const Image::Name& imageName,
    const Path& downloadDir,
    const Option<Duration>& timeout,
    const Option<size_t>& maxSize)
{
  return dispatch(
      process_.get(),
      &RemotePullerProcess::pull,
      imageName,
      downloadDir,
      timeout,
      maxSize)
    .onAny([imageName](
        const Future<PulledImageInfo>& future) -> Future<PulledImageInfo> {
      if (future.isFailed()) {
        LOG(WARNING) << "Failed to download image '" + imageName.repository() +
          "' : " + future.failure();
      }

      return future;
    });
}


Try<Owned<RemotePullerProcess>> RemotePullerProcess::create(
    const URL& registryServer,
    const URL& authServer)
{
  Try<Owned<RegistryClient>> registry =
    RegistryClient::create(registryServer, authServer, None());

  if (registry.isError()) {
    return Error("Failed to create registry client: " + registry.error());
  }

  return Owned<RemotePullerProcess>(new RemotePullerProcess(registry.get()));
}


RemotePullerProcess::RemotePullerProcess(const Owned<RegistryClient>& registry)
  : _registryClient(registry) {}


Future<PulledLayerInfo> RemotePullerProcess::downloadLayer(
    const string& remoteName,
    const Path& downloadDir,
    const FileSystemLayerInfo& layer,
    const Duration& timeout,
    const Option<size_t>& maxSize)
{
  VLOG(1) << "Downloading layer: '" +
    layer.layerId + "' for image '" +
    remoteName + "'";

  Owned<Promise<PulledLayerInfo>> downloadPromise;

  if (!_downloadTracker.contains(remoteName)) {
    downloadPromise =
      Owned<Promise<PulledLayerInfo>>(new Promise<PulledLayerInfo>());

  } else {
    LOG(INFO) << "Download already in progress for image '" + remoteName +
      "' layer: " + layer.layerId;

    downloadPromise = _downloadTracker.at(remoteName);
    return downloadPromise->future();
  }

  const Path downloadFile(path::join(downloadDir, layer.layerId));

  dispatch(self(), [=]() {
      _registryClient->getBlob(
      remoteName,
      layer.checksumInfo,
      downloadFile,
      timeout,
      maxSize)
    .after(timeout, [layer](
        Future<size_t> future) -> Future<size_t> {
      future.discard();
      return Failure(
          "Failed to download blob for layer '" +
          layer.layerId + "' - Blob download timeout");
     })
    .onAny([this, remoteName, downloadPromise](
        const Future<size_t>& future) {
      if(future.isFailed()) {
        downloadPromise->fail(future.failure());
      }

      _downloadTracker.erase(remoteName);
    })
    .then([this, downloadDir, downloadFile, layer, downloadPromise](
        const Future<size_t>& blobSize) -> Future<PulledLayerInfo> {
      if (blobSize.get() <= 0) {
        downloadPromise->fail("Failed to download layer: " + layer.layerId);
      }

      vector<string> argv = {
        "tar",
        "-C",
        downloadDir,
        "-x",
        "-f",
        downloadFile
      };

      Try<Subprocess> s = subprocess(
          "tar",
          argv,
          Subprocess::PATH("/dev/null"),
          Subprocess::PATH("/dev/null"),
          Subprocess::PATH("/dev/null"));

      if (s.isError()) {
        downloadPromise->fail("Failed to create tar subprocess: " + s.error());
      }

      s.get().status()
        .then([this, layer, downloadPromise, downloadFile](
            const Option<int>& status) -> Future<PulledLayerInfo> {
          if (status.isNone()) {
            downloadPromise->fail("Failed to reap subprocess to untar image");
          } else if (!WIFEXITED(status.get()) ||
                WEXITSTATUS(status.get()) != 0) {
              downloadPromise->fail(
                  "Untar failed with exit code: " + WSTRINGIFY(status.get()));
          } else {
              downloadPromise->set(PulledLayerInfo(layer.layerId, downloadFile));
          }

          return downloadPromise->future();
        });

      return downloadPromise->future();
    });
  });

  return downloadPromise->future();
}


Try<string> RemotePullerProcess::getRemoteNameFromCanonicalName(
    const Image::Name& name,
    const Option<string>& tag)
{
  //TODO(jojy): Canonical names could be something like "ubuntu14.04" but the
  //remote name for it could be library/ubuntu14.04. This mapping has to come
  //from a configuration in the near future.

  return "library/" + name.repository();
}


Future<PulledImageInfo> RemotePullerProcess::pull(
    const Image::Name& imageName,
    const Path& downloadDir,
    const Option<Duration>& timeout,
    const Option<size_t>& maxSize)
{
  const string imageTag = imageName.tag();
  Try<string> remoteName = getRemoteNameFromCanonicalName(imageName, imageTag);
  if (remoteName.isError()) {
    return Failure(
        "Failed to get remote name from canonical name for '" +
        imageName.repository() + "'");
  }

  Duration totalTimeout = Seconds(
    timeout.getOrElse(DEFAULT_PULL_TIMEOUT_SECS).secs());

  totalTimeout =
    totalTimeout.secs() <= 0 ? DEFAULT_PULL_TIMEOUT_SECS : totalTimeout;

  const Duration manifestTimeout =
    totalTimeout * (MANIFEST_TIMEOUT_PERCENT / 100);

  const Duration layerTimeout = totalTimeout - manifestTimeout;

  return _registryClient->getManifest(
      remoteName.get(),
      imageTag,
      manifestTimeout)
    .then(defer(self(), [this, downloadDir, maxSize, layerTimeout, remoteName](
        const ManifestResponse& manifest) {
      list<Future<PulledLayerInfo>> downloadFutures;

      foreach(const FileSystemLayerInfo& layer, manifest.fsLayerInfoList) {
        downloadFutures.push_back(downloadLayer(
            remoteName.get(),
            downloadDir,
            layer,
            layerTimeout,
            maxSize));
      }

      return collect(downloadFutures);
    }))
    .onAny([](
        const Future<list<PulledLayerInfo>>& layerFutures)
        -> Future<list<PulledLayerInfo>> {
      if (layerFutures.isDiscarded()) {
        return Failure("Failed to download image layers (future discarded)");
      }

      return layerFutures;
    })
    .then([](
        const Future<list<PulledLayerInfo>>& layerFutures)
        -> Future<PulledImageInfo> {
        PulledImageInfo layers;

        foreach(const PulledLayerInfo& layer, layerFutures.get()) {
          layers.emplace_back(layer);
        }

        return layers;
    });
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
