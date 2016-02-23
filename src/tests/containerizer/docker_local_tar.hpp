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

#ifndef __TEST_DOCKER_LOCAL_TAR_HPP__
#define __TEST_DOCKER_LOCAL_TAR_HPP__

#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/nothing.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include "tests/containerizer/rootfs.hpp"

namespace mesos {
namespace internal {
namespace tests {

class DockerLocalTar {
public:
  virtual ~DockerLocalTar() {}

  // Create a docker test image tarball in docker registry directory.
  static Try<Nothing> create(
      const std::string& archivesDir,
      const std::string& name) {
    if (!os::exists(archivesDir)) {
      Try<Nothing> mkdir = os::mkdir(archivesDir, true);
      if (mkdir.isError()) {
        return Error("Failed to create docker local registry directory '" +
                     archivesDir + "': " + mkdir.error());
      }
    }

    // Remove the image tarball, because it is possible that the same
    // naming non-testing purpose image tarball exists in archives dir.
    const std::string imageTar = path::join(archivesDir, name + ".tar");
    if (os::exists(imageTar)) {
      Try<Nothing> rm = os::rm(imageTar);
      if (rm.isError()) {
        return Error("Failed to remove docker test image tarball '" +
                     imageTar + "': " + rm.error());
      }
    }

    const std::string imagePath = path::join(archivesDir, name);
    if (os::exists(imagePath)) {
      Try<Nothing> rmdir = os::rmdir(imagePath);
      if (rmdir.isError()) {
        return Error("Failed to remove docker test image directory '" +
                     imagePath + "': " + rmdir.error());
      }
    }

    Try<Nothing> mkdir = os::mkdir(imagePath);
    if (mkdir.isError()) {
      return Error("Failed to create docker test image directory '" +
                   imagePath + "': " + mkdir.error());
    }

    // Create a two-layer docker test image, `layerId1` will be the parent
    // of `layerId2`.
    const std::string layerId1 =
      "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54";
    const std::string layerPath1 = path::join(imagePath, layerId1);

    const std::string layerId2 =
      "d7057cb020844f245031d27b76cb18af05db1cc3a96a29fa7777af75f5ac91a3";
    const std::string layerPath2 = path::join(imagePath, layerId2);

    // Create docker test image `repositories`.
    JSON::Value repositories = JSON::parse(
        "{"
        "  \"" + name + "\": {"
        "    \"latest\": \"" + layerId2 + "\""
        "  }"
        "}").get();

    Try<Nothing> writeRepo = os::write(
        path::join(imagePath, "repositories"),
        stringify(repositories));
    if (writeRepo.isError()) {
      return Error("Failed to save docker test image 'repositories': " +
                   writeRepo.error());
    }

    // Create layer1.
    Try<Nothing> mkdirLayer1 = os::mkdir(layerPath1);
    if (mkdirLayer1.isError()) {
      return Error("Failed to create docker test image layer '" +
                   layerId1 + "': " + mkdirLayer1.error());
    }

    JSON::Value manifest1 = JSON::parse(
        R"~(
        {
            "container": "5f8e0e129ff1e03bbb50a8b6ba7636fa5503c695125b1c392490d8aa113e8cf6",
            "created": "2015-09-21T20:15:47.433616227Z",
            "config": {
                "Hostname": "5f8e0e129ff1",
                "Entrypoint": null,
                "Env": null,
                "OnBuild": null,
                "OpenStdin": false,
                "MacAddress": "",
                "User": "",
                "VolumeDriver": "",
                "AttachStderr": false,
                "AttachStdout": false,
                "PublishService": "",
                "NetworkDisabled": false,
                "StdinOnce": false,
                "Cmd": null,
                "WorkingDir": "",
                "AttachStdin": false,
                "Volumes": null,
                "Tty": false,
                "Domainname": "",
                "Image": "",
                "Labels": null,
                "ExposedPorts": null
            },
            "container_config": {
                "Hostname": "5f8e0e129ff1",
                "Entrypoint": null,
                "Env": null,
                "OnBuild": null,
                "OpenStdin": false,
                "MacAddress": "",
                "User": "",
                "VolumeDriver": "",
                "AttachStderr": false,
                "AttachStdout": false,
                "PublishService": "",
                "NetworkDisabled": false,
                "StdinOnce": false,
                "Cmd": [
                    "/bin/sh",
                    "-c",
                    "#(nop) ADD file:6cccb5f0a3b3947116a0c0f55d071980d94427ba0d6dad17bc68ead832cc0a8f in /"
                ],
                "WorkingDir": "",
                "AttachStdin": false,
                "Volumes": null,
                "Tty": false,
                "Domainname": "",
                "Image": "",
                "Labels": null,
                "ExposedPorts": null
            },
            "architecture": "amd64",
            "docker_version": "1.8.2",
            "os": "linux",
            "id": "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
            "Size": 1095501
        })~").get();

    Try<Nothing> writeManifest1 = os::write(
        path::join(layerPath1, "json"),
        stringify(manifest1));
    if (writeManifest1.isError()) {
      return Error("Failed to save docker test image layer '" + layerId1 +
                   "': " + writeManifest1.error());
    }

    const std::string rootfs1 = path::join(layerPath1, "layer");

    Try<Nothing> mkdirRootfs1 = os::mkdir(rootfs1);
    if (mkdirRootfs1.isError()) {
      return Error("Failed to create layer rootfs directory '" +
                   rootfs1 + "': " + mkdirRootfs1.error());
    }

    // Only create one linux rootfs for the first layer1.
    Try<process::Owned<Rootfs>> rootfs = LinuxRootfs::create(rootfs1);
    if (rootfs.isError()) {
      return Error("Failed to create docker test image rootfs: " +
                   rootfs.error());
    }

    CHECK_NE(os::getcwd(), rootfs1);

    // Tar the rootfs for layer1.
    Try<Nothing> tar = os::tar(rootfs1, path::join(layerPath1, "layer.tar"));
    if (tar.isError()) {
      return Error("Failed to tar layer rootfs: " + tar.error());
    }

    Try<Nothing> rmdir = os::rmdir(rootfs1);
    if (rmdir.isError()) {
      return Error("Failed to remove layer rootfs directory: " + rmdir.error());
    }

    // Not using the layer VERSION now, but still keep it to make consensus.
    Try<Nothing> writeVersion1 = os::write(
        path::join(layerPath1, "VERSION"),
        "1.0");
    if (writeVersion1.isError()) {
      return Error("Failed to save layer version: " + writeVersion1.error());
    }

    // Create layer2.
    Try<Nothing> mkdirLayer2 = os::mkdir(layerPath2);
    if (mkdirLayer2.isError()) {
      return Error("Failed to create docker test image layer '" +
                   layerId2 + "': " + mkdirLayer2.error());
    }

    JSON::Value manifest2 = JSON::parse(
        R"~(
        {
            "container": "7f652467f9e6d1b3bf51172868b9b0c2fa1c711b112f4e987029b1624dd6295f",
            "parent": "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
            "created": "2015-09-21T20:15:47.866196515Z",
            "config": {
                "Hostname": "5f8e0e129ff1",
                "Entrypoint": null,
                "Env": null,
                "OnBuild": null,
                "OpenStdin": false,
                "MacAddress": "",
                "User": "",
                "VolumeDriver": "",
                "AttachStderr": false,
                "AttachStdout": false,
                "PublishService": "",
                "NetworkDisabled": false,
                "StdinOnce": false,
                "Cmd": [
                    "sh"
                ],
                "WorkingDir": "",
                "AttachStdin": false,
                "Volumes": null,
                "Tty": false,
                "Domainname": "",
                "Image": "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
                "Labels": null,
                "ExposedPorts": null
            },
            "container_config": {
                "Hostname": "5f8e0e129ff1",
                "Entrypoint": null,
                "Env": null,
                "OnBuild": null,
                "OpenStdin": false,
                "MacAddress": "",
                "User": "",
                "VolumeDriver": "",
                "AttachStderr": false,
                "AttachStdout": false,
                "PublishService": "",
                "NetworkDisabled": false,
                "StdinOnce": false,
                "Cmd": [
                    "/bin/sh",
                    "-c",
                    "#(nop) CMD [\"sh\"]"
                ],
                "WorkingDir": "",
                "AttachStdin": false,
                "Volumes": null,
                "Tty": false,
                "Domainname": "",
                "Image": "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
                "Labels": null,
                "ExposedPorts": null
            },
            "architecture": "amd64",
            "docker_version": "1.8.2",
            "os": "linux",
            "id": "d7057cb020844f245031d27b76cb18af05db1cc3a96a29fa7777af75f5ac91a3",
            "Size": 0
        })~").get();

    Try<Nothing> writeManifest2 = os::write(
        path::join(layerPath2, "json"),
        stringify(manifest2));
    if (writeManifest2.isError()) {
      return Error("Failed to save docker test image layer '" + layerId2 +
                   "': " + writeManifest2.error());
    }

    const std::string rootfs2 = path::join(layerPath2, "layer");

    Try<Nothing> mkdirRootfs2 = os::mkdir(rootfs2);
    if (mkdirRootfs2.isError()) {
      return Error("Failed to create layer rootfs directory '" +
                   rootfs2 + "': " + mkdirRootfs2.error());
    }

    CHECK_NE(os::getcwd(), rootfs2);

    // Tar the rootfs for layer2.
    tar = os::tar(rootfs2, path::join(layerPath2, "layer.tar"));
    if (tar.isError()) {
      return Error("Failed to tar layer rootfs: " + tar.error());
    }

    rmdir = os::rmdir(rootfs2);
    if (rmdir.isError()) {
      return Error("Failed to remove layer rootfs directory: " + rmdir.error());
    }

    // Not using the layer VERSION now, but still keep it to make consensus.
    Try<Nothing> writeVersion2 = os::write(
        path::join(layerPath2, "VERSION"),
        "1.0");
    if (writeVersion2.isError()) {
      return Error("Failed to save layer version: " + writeVersion2.error());
    }

    CHECK_NE(os::getcwd(), imagePath);

    tar = os::tar(imagePath, imageTar);
    if (tar.isError()) {
      return Error("Failed to create image tarball: " + tar.error());
    }

    rmdir = os::rmdir(imagePath);
    if (rmdir.isError()) {
      return Error("Failed to remove image directory: '" + imagePath + "': " +
                   rmdir.error());
    }

    return Nothing();
  }
};



} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TEST_DOCKER_LOCAL_TAR_HPP__
