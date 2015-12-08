// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "slave/containerizer/mesos/provisioner/docker/store.hpp"

#include <list>
#include <vector>

#include <glog/logging.h>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/result.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/subprocess.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/docker/metadata_manager.hpp"
#include "slave/containerizer/mesos/provisioner/docker/paths.hpp"
#include "slave/containerizer/mesos/provisioner/docker/puller.hpp"

#include "slave/flags.hpp"

using namespace process;

using std::list;
using std::pair;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class StoreProcess : public Process<StoreProcess>
{
public:
  StoreProcess(
      const Flags& _flags,
      const Owned<MetadataManager>& _metadataManager,
      const Owned<Puller>& _puller)
    : flags(_flags), metadataManager(_metadataManager), puller(_puller) {}

  ~StoreProcess() {}

  Future<Nothing> recover();

  Future<vector<string>> get(const mesos::Image& image);

private:
  Future<Image> _get(
      const Image::Name& name,
      const Option<Image>& image);

  Future<vector<string>> __get(const Image& image);

  Future<pair<vector<string>, string>> moveLayers(
      const ImageInfo& imageInfo);

  Future<Image> storeImage(
      const Image::Name& name,
      const pair<vector<string>, string>& layerIds);

  Future<Nothing> moveLayer(const pair<string, string>& layerPath);

  const Flags flags;
  Owned<MetadataManager> metadataManager;
  Owned<Puller> puller;
  hashmap<std::string, Owned<Promise<Image>>> pulling;
};


Try<Owned<slave::Store>> Store::create(const Flags& flags)
{
  Try<Owned<Puller>> puller = Puller::create(flags);
  if (puller.isError()) {
    return Error("Failed to create Docker puller: " + puller.error());
  }

  Try<Owned<slave::Store>> store = Store::create(flags, puller.get());
  if (store.isError()) {
    return Error("Failed to create Docker store: " + store.error());
  }

  return store.get();
}


Try<Owned<slave::Store>> Store::create(
    const Flags& flags,
    const Owned<Puller>& puller)
{
  Try<Nothing> mkdir = os::mkdir(flags.docker_store_dir);
  if (mkdir.isError()) {
    return Error("Failed to create Docker store directory: " +
                 mkdir.error());
  }

  mkdir = os::mkdir(paths::getStagingDir(flags.docker_store_dir));
  if (mkdir.isError()) {
    return Error("Failed to create Docker store staging directory: " +
                 mkdir.error());
  }

  Try<Owned<MetadataManager>> metadataManager = MetadataManager::create(flags);
  if (metadataManager.isError()) {
    return Error(metadataManager.error());
  }

  Owned<StoreProcess> process(
      new StoreProcess(flags, metadataManager.get(), puller));

  return Owned<slave::Store>(new Store(process));
}


Store::Store(const Owned<StoreProcess>& _process) : process(_process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


Store::~Store()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<Nothing> Store::recover()
{
  return dispatch(process.get(), &StoreProcess::recover);
}


Future<vector<string>> Store::get(const mesos::Image& image)
{
  return dispatch(process.get(), &StoreProcess::get, image);
}


Future<vector<string>> StoreProcess::get(const mesos::Image& image)
{
  if (image.type() != mesos::Image::DOCKER) {
    return Failure("Docker provisioner store only supports Docker images");
  }

  Image::Name imageName = parseImageName(image.docker().name());

  return metadataManager->get(imageName)
    .then(defer(self(), &Self::_get, imageName, lambda::_1))
    .then(defer(self(), &Self::__get, lambda::_1));
}


Future<Image> StoreProcess::_get(
    const Image::Name& name,
    const Option<Image>& image)
{
  if (image.isSome()) {
    return image.get();
  }

  Try<string> staging =
    os::mkdtemp(paths::getStagingTempDir(flags.docker_store_dir));

  if (staging.isError()) {
    return Failure("Failed to create a staging directory");
  }

  const string imageName = stringify(name);

  if (!pulling.contains(imageName)) {
    Owned<Promise<Image>> promise(new Promise<Image>());

    Future<Image> future = puller->pull(name, Path(staging.get()))
      .then(defer(self(), &Self::moveLayers, lambda::_1))
      .then(defer(self(), &Self::storeImage, name, lambda::_1))
      .onAny(defer(self(), [this, imageName](const Future<Image>&) {
        pulling.erase(imageName);
      }))
      .onAny([staging, imageName]() {
        Try<Nothing> rmdir = os::rmdir(staging.get());
        if (rmdir.isError()) {
          LOG(WARNING) << "Failed to remove staging directory: "
                       << rmdir.error();
        }
      });

    promise->associate(future);
    pulling[imageName] = promise;

    return promise->future();
  }

  return pulling[imageName]->future();
}


Future<vector<string>> StoreProcess::__get(const Image& image)
{
  vector<string> layerDirectories;
  foreach (const string& layer, image.layer_ids()) {
    layerDirectories.push_back(
        paths::getImageLayerRootfsPath(
            flags.docker_store_dir, layer));
  }

  return layerDirectories;
}


Future<Nothing> StoreProcess::recover()
{
  return metadataManager->recover();
}


Future<pair<vector<string>, string>> StoreProcess::moveLayers(
    const ImageInfo& imageInfo)
{
  list<Future<Nothing>> futures;
  foreach (const auto& layerPath, imageInfo.layerPaths) {
    futures.push_back(moveLayer(layerPath));
  }

  return collect(futures)
    .then([imageInfo]() {
        vector<string> layerIds;
        foreach (const auto& layerPath, imageInfo.layerPaths) {
          layerIds.push_back(layerPath.first);
        }

        return pair<vector<string>, string>(layerIds, imageInfo.manifest);
    });
}


Future<Image> StoreProcess::storeImage(
    const Image::Name& name,
    const pair<vector<string>, string>& imageInfo)
{
  return metadataManager->put(name, imageInfo.first, imageInfo.second);
}


Future<Nothing> StoreProcess::moveLayer(const pair<string, string>& layerPath)
{
  if (!os::exists(layerPath.second)) {
    return Failure("Unable to find layer '" + layerPath.first + "' in '" +
                   layerPath.second + "'");
  }

  const string imageLayerPath =
    paths::getImageLayerPath(flags.docker_store_dir, layerPath.first);

  Try<Nothing> mkdir = os::mkdir(imageLayerPath);
  if (mkdir.isError()) {
    return Failure("Failed to create layer path in store for id '" +
                   layerPath.first + "': " + mkdir.error());
  }

  Try<Nothing> status = os::rename(
      layerPath.second,
      paths::getImageLayerRootfsPath(
          flags.docker_store_dir, layerPath.first));

  if (status.isError()) {
    return Failure("Failed to move layer '" + layerPath.first +
                   "' to store directory: " + status.error());
  }

  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
