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

#ifndef __LINUX_LAUNCHER_HPP__
#define __LINUX_LAUNCHER_HPP__

#include "slave/containerizer/mesos/launcher.hpp"

#include <stout/hashmap.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Launcher for Linux systems with cgroups. Uses a freezer cgroup to
// track pids.
class LinuxLauncher : public Launcher
{
public:
  static Try<Launcher*> create(const Flags& flags);

  // Returns 'true' if prerequisites for using LinuxLauncher are available.
  static bool available();

  virtual ~LinuxLauncher() {}

  virtual process::Future<hashset<ContainerID>> recover(
      const std::list<mesos::slave::ContainerState>& states);

  virtual Try<pid_t> fork(
      const ContainerID& containerId,
      const std::string& path,
      const std::vector<std::string>& argv,
      const process::Subprocess::IO& in,
      const process::Subprocess::IO& out,
      const process::Subprocess::IO& err,
      const flags::FlagsBase* flags,
      const Option<std::map<std::string, std::string>>& environment,
      const Option<int>& namespaces,
      std::vector<process::Subprocess::Hook> parentHooks);

  virtual process::Future<Nothing> destroy(const ContainerID& containerId);

  virtual process::Future<ContainerStatus> status(
      const ContainerID& containerId);

  virtual std::string getExitStatusCheckpointPath(
      const ContainerID& containerId);

private:
  LinuxLauncher(
      const Flags& flags,
      const std::string& freezerHierarchy,
      const Option<std::string>& systemdHierarchy);

  // Helper struct for storing information about each container. A
  // "container" here means a cgroup in the freezer subsystem that is
  // used to represent a collection of processes. This container may
  // also have multiple namespaces associated with it but that is not
  // managed explicitly here.
  struct Container
  {
    ContainerID id;

    // NOTE: this represents 'PID 1', i.e., the "init" of the
    // container that we created (it may be for an executor, or any
    // arbitrary process that has been launched in the event of nested
    // containers). It's optional because while recovering we might
    // recover the "container" (i.e., the freezer cgroup) but not have
    // the checkpointed pid.
    Option<pid_t> pid;
  };

  // Helper for doing recovery on a particular cgroup.
  Try<hashmap<ContainerID, Container>> recover(
      const std::string& cgroup,
      hashmap<ContainerID, Container> containers);

  static const std::string subsystem;
  const Flags flags;
  const std::string freezerHierarchy;
  const Option<std::string> systemdHierarchy;

  // Helper for determining the cgroup for a container (i.e., the
  // "path" in a cgroup subsystem).
  std::string cgroup(const ContainerID& containerId);

  hashmap<ContainerID, Container> containers;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __LINUX_LAUNCHER_HPP__
