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

#ifndef __PROVISIONER_DOCKER_REMOTE_PULLER_HPP__
#define __PROVISIONER_DOCKER_REMOTE_PULLER_HPP__

#include <list>
#include <string>
#include <utility>

#include <stout/duration.hpp>
#include <stout/path.hpp>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include "slave/containerizer/provisioner/docker/message.hpp"
#include "slave/containerizer/provisioner/docker/puller.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declarations.
class RemotePullerProcess;

/*
 * Pulls an image from remote registry.
 * TODO(jojy): The interface for Puller is still in another patch. Will
 * incorporate the interface once that patch is available.
 */
class RemotePuller : public Puller
{
public:
  typedef std::pair<std::string, std::string> PulledLayerInfo;
  typedef std::list<PulledLayerInfo> PulledImageInfo;

  /**
   * Factory method for creating RemotePuller.
   *
   * @param registry URL of the registry server.
   * @param authServer URL of the authentication server.
   * @return RemotePuller on success.
   *         Error on failure.
   */
  static Try<process::Owned<RemotePuller>> create(
      const process::http::URL& registry,
      const process::http::URL& authServer);

  ~RemotePuller();

  /**
   * Pulls an image into a download directory.
   *
   * @param imageName canonical name of the image.
   * @param imageTag tag of the image to be pulled.
   * @param downloadDir path to which the layers should be downloaded.
   * @param timeout maximum time for the download process.
   * @param maxSize maximum allowed download size per layer.
   * @return List of layer ids pulled.
   */
  process::Future<PulledImageInfo> pull(
      const Image::Name& imageName,
      const Path& downloadDir,
      const Option<Duration>& timeout,
      const Option<size_t>& maxSize);

private:
  RemotePuller(const process::Owned<RemotePullerProcess>& process);

  process::Owned<RemotePullerProcess> process_;

  RemotePuller(const RemotePuller&) = delete;
  RemotePuller& operator=(const RemotePuller&) = delete;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif //  __PROVISIONER_DOCKER_REMOTE_PULLER_HPP__
