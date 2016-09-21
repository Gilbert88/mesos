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

#include <stout/lambda.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "slave/containerizer/mesos/paths.hpp"

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace containerizer {
namespace paths {

string buildPathForContainer(
    const ContainerID& containerId,
    const string& prefix)
{
  if (!containerId.has_parent()) {
    return path::join(prefix, containerId.value());
  } else {
    return path::join(
        buildPathForContainer(containerId.parent(), prefix),
        prefix,
        containerId.value());
  }
}


string getRuntimePath(
    const string& runtime_dir,
    const ContainerID& containerId)
{
  return path::join(
      runtime_dir,
      buildPathForContainer(containerId, CONTAINER_DIRECTORY));
}


Result<pid_t> getContainerPid(
    const string& runtime_dir,
    const ContainerID& containerId)
{
  const string path = path::join(
    getRuntimePath(runtime_dir, containerId), PID_FILE);

  if (!os::exists(path)) {
    // This is possible because we don't atomically create the
    // directory and write the 'pid' file and thus we might
    // terminate/restart after we've created the directory but
    // before we've written the file.
    return None();
  }

  Try<string> read = os::read(path);
  if (read.isError()) {
    return Error("Failed to recover pid of container: " + read.error());
  }

  Try<pid_t> pid = numify<pid_t>(read.get());
  if (pid.isError()) {
    return Error(
        "Failed to numify pid '" + read.get() +
        "' of container at '" + path + "': " + pid.error());
  }

  return pid.get();
}


Result<int> getContainerStatus(
    const string& runtime_dir,
    const ContainerID& containerId)
{
  const string path = path::join(
    getRuntimePath(runtime_dir, containerId), STATUS_FILE);

  if (!os::exists(path)) {
    return None();
  }

  Try<string> read = os::read(path);
  if (read.isError()) {
    return Error("Unable to read status for container '" +
                 containerId.value() + "' from checkpoint file '" +
                 path + "': " + read.error());
  }

  if (read.get() != "") {
    Try<int> containerStatus = numify<int>(read.get());
    if (containerStatus.isError()) {
      return Error("Unable to read status for container '" +
                   containerId.value() + "' as integer from '" +
                   path + "': " + read.error());
    }

    return containerStatus.get();
  }

  return None();
}


Result<vector<ContainerID>> getContainerIds(const string& runtimeDir)
{
  lambda::function<Result<vector<ContainerID>>(
      const string&,
      const string&)> helper;

  helper = [&helper](
      const string& runtimeDir,
      const string& directory) -> Result<vector<ContainerID>> {
    // Loop through each container at the path, if it exists.
    const string path = path::join(runtimeDir, directory);

    if (!os::exists(path)) {
      return None();
    }

    Try<list<string>> entries = os::ls(path);
    if (entries.isError()) {
      return Error("Failed to list '" + path + "': " + entries.error());
    }

    // The order always guarantee that a parent container is inserted
    // before its child containers. This is necessary for constructing
    // the hashmap 'containers_' in 'Containerizer::recover()'.
    vector<ContainerID> containers;

    foreach (const string& entry, entries.get()) {
      // We're not expecting anything else but directories here
      // representing each container.
      CHECK(os::stat::isdir(path::join(path, entry)));

      // TODO(benh): Validate that the entry looks like a ContainerID?
      ContainerID container;

      // Determine the ContainerID from 'directory/entry' (we explicitly
      // do not want to use `path` because it contains things that we don't
      // want in our ContainerID and even still we have to skip all
      // instances of 'containers' as well).
      vector<string> tokens =
        strings::tokenize(
            path::join(directory, entry),
            stringify(os::PATH_SEPARATOR));

      foreach (const string& token, tokens) {
        // Skip the directory separator 'containers'.
        if (token == CONTAINER_DIRECTORY) {
          continue;
        }

        ContainerID id;
        id.set_value(token);

        if (container.has_value()) {
          id.mutable_parent()->CopyFrom(container);
        }

        container = id;
      }

      // Validate the ID (there should be at least one level).
      if (!container.has_value()) {
        return Error(
            "Failed to determine ContainerID from path '" + path + "'");
      }

      containers.push_back(container);

      // Now recursively build the list of nested containers.
      Result<vector<ContainerID>> _containers = helper(
          runtimeDir,
          path::join(directory, entry, CONTAINER_DIRECTORY));

      if (_containers.isError()) {
        return Error(_containers.error());
      } else if (_containers.isSome()) {
        containers.insert(
            containers.end(), _containers->begin(), _containers->end());
      }
    }

    return containers;
  };

  return helper(runtimeDir, CONTAINER_DIRECTORY);
}

} // namespace paths {
} // namespace containerizer {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
