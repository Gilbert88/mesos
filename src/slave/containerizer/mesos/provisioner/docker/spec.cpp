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

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include "slave/containerizer/mesos/provisioner/docker/registry_client.hpp"
#include "slave/containerizer/mesos/provisioner/docker/spec.hpp"

using std::string;

using namespace mesos::internal::slave::docker::registry;

using FileSystemLayerInfo = RegistryClient::FileSystemLayerInfo;
using ManifestResponse = RegistryClient::ManifestResponse;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace spec {

// Validate if the specified image manifest conforms to the Docker spec.
Option<Error> validateManifest(const DockerImageManifest& manifest)
{
  // Validate required fields are present,
  // e.g., repeated fields that has to be >= 1.
  if (manifest.fslayers_size() <= 0) {
    return Error("'fsLayers' field size must be at least one");
  }

  if (manifest.history_size() <= 0) {
    return Error("'history' field size must be at least one");
  }

  if (manifest.signatures_size() <= 0) {
    return Error("'signatures' field size must be at least one");
  }

  // Verify that blobSum and v1Compatibility numbers are equal.
  if (manifest.fslayers_size() != manifest.history_size()) {
    return Error("There should be equal size of 'fsLayers' "
                 "with corresponding 'history'");
  }

  // FsLayers field validation.
  foreach (const DockerImageManifest::FsLayers& fslayer,
           manifest.fslayers()) {
    const string& blobSum = fslayer.blobsum();
    if (!strings::contains(blobSum, ":")) {
      return Error("Incorrect 'blobSum' format: " + blobSum);
    }
  }

  return None();
}


Try<DockerImageManifest> parse(const JSON::Object& json)
{
  Try<DockerImageManifest> manifest =
    protobuf::parse<DockerImageManifest>(json);

  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validateManifest(manifest.get());
  if (error.isSome()) {
    return Error("Docker Image Manifest Validation failed: " +
                 error.get().message);
  }

  return manifest.get();
}

Try<docker::ManifestResponse> parseManifestResponse(
    const Manifest& manifest)
{
  docker::ManifestResponse manifestResponse;
  manifestResponse.set_name(manifest.name);

  auto createLayerInfo = [](
      const string& checksumInfo,
      const string& layerId)
      -> Try<Manifest::FileSystemLayerInfo> {
    docker::ManifestResponse::FileSystemLayerInfo layerInfo;
    layerInfo.set_checksuminfo(checksumInfo);
    layerInfo.set_layerid(layerId);
    return layerInfo;
  };

  foreach(const FileSystemLayerInfo& fsLayerInfo,
          manifest.fsLayerInfos) {
    Try<docker::Manifest::FileSystemLayerInfo> layerInfo =
      createLayerInfo(fsLayerInfo.checksumInfo, fsLayerInfo.layerId);

    if (!layerInfo.isSome()) {
      return Error(
          "Fail to create file system layer info for layerId: "
          + fsLayerInfo.layerId);
    }

    manifestResponse.add_filesystemlayerinfo()->CopyFrom(layerInfo.get());
  }

  return manifestResponse;
}

} // namespace spec {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
