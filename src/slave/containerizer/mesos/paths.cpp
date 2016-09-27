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

const char SLAVES_DIR[] = "slaves";
const char FRAMEWORKS_DIR[] = "frameworks";
const char EXECUTORS_DIR[] = "executors";
const char EXECUTOR_RUNS_DIR[] = "runs";


string buildPath(
    const ContainerID& containerId,
    const string& prefix)
{
  if (!containerId.has_parent()) {
    return path::join(prefix, containerId.value());
  } else {
    return path::join(
        buildPath(containerId.parent(), prefix),
        prefix,
        containerId.value());
  }
}


string getRuntimePath(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  return path::join(
      runtimeDir,
      buildPath(containerId, CONTAINER_DIRECTORY));
}


Result<pid_t> getContainerPid(
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = path::join(
      getRuntimePath(runtimeDir, containerId),
      PID_FILE);

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
    const string& runtimeDir,
    const ContainerID& containerId)
{
  const string path = path::join(
      getRuntimePath(runtimeDir, containerId),
      STATUS_FILE);

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


Try<vector<ContainerID>> getContainerIds(const string& runtimeDir)
{
  lambda::function<Try<vector<ContainerID>>(const Option<ContainerID>&)> helper;

  helper = [&helper, &runtimeDir](const Option<ContainerID>& parentContainerId)
    -> Try<vector<ContainerID>> {
    // Loop through each container at the path, if it exists.
    const string path = path::join(
        parentContainerId.isSome()
          ? getRuntimePath(runtimeDir, parentContainerId.get())
          : runtimeDir,
        CONTAINER_DIRECTORY);

    if (!os::exists(path)) {
      return vector<ContainerID>();
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
      container.set_value(entry);

      if (parentContainerId.isSome()) {
        container.mutable_parent()->CopyFrom(parentContainerId.get());
      }

      containers.push_back(container);

      // Now recursively build the list of nested containers.
      Try<vector<ContainerID>> children = helper(container);
      if (children.isError()) {
        return Error(children.error());
      }

      if (!children->empty()) {
        containers.insert(containers.end(), children->begin(), children->end());
      }
    }

    return containers;
  };

  return helper(None());
}


string getSandboxPath(
    const string& rootSandboxPath,
    const ContainerID& containerId)
{
  return containerId.has_parent()
    ? path::join(
        getSandboxPath(rootSandboxPath, containerId.parent()),
        "containers",
        containerId.value())
    : rootSandboxPath;
}


Try<SandboxPath> parseSandboxPath(
    const string& _rootDir,
    const string& directory)
{
  // Make sure there's a separator at the end of the `rootdir` so that
  // we don't accidentally slice off part of a directory.
  const string rootDir = path::join(_rootDir, "");

  if (!strings::startsWith(directory, rootDir)) {
    return Error(
        "Directory '" + directory + "' does not fall under "
        "the root directory: " + rootDir);
  }

  vector<string> tokens =
    strings::tokenize(directory.substr(rootDir.size()), "/");

  // A complete executor run path consists of at least 8 tokens, which
  // includes the four named directories and the four IDs.
  if (tokens.size() < 8) {
    return Error(
        "Path after root directory is not long enough to be an "
        "executor run path: " + path::join(tokens));
  }

  // All four named directories much match.
  if (tokens[0] == SLAVES_DIR &&
      tokens[2] == FRAMEWORKS_DIR &&
      tokens[4] == EXECUTORS_DIR &&
      tokens[6] == EXECUTOR_RUNS_DIR) {
    SandboxPath path;

    path.slaveId.set_value(tokens[1]);
    path.frameworkId.set_value(tokens[3]);
    path.executorId.set_value(tokens[5]);
    path.containerId.set_value(tokens[7]);

    // For a nested container x.y.z, the sandbox layout is the following:
    // .../runs/x/containers/y/containers/z
    if (tokens.size() > 8 && tokens[8] == CONTAINER_DIRECTORY) {
      for (size_t i = 8; i < tokens.size(); i++) {
        if (i % 2 == 0) {
          if (tokens[i] != CONTAINER_DIRECTORY) {
            break;
          }
        } else {
          ContainerID id;
          id.set_value(tokens[i]);
          id.mutable_parent()->CopyFrom(path.containerId);
          path.containerId = id;
        }
      }
    }

    return path;
  }

  return Error(
      "Could not parse executor run path from directory: " + directory);
}

} // namespace paths {
} // namespace containerizer {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
