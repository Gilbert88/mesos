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

#ifndef __DOCKER_RUNTIME_ISOLATOR_HPP__
#define __DOCKER_RUNTIME_ISOLATOR_HPP__

#include "slave/containerizer/mesos/isolator.hpp"

namespace mesos {
namespace internal {
namespace slave {

// The docker runtime Isolator is responsible for preparing mesos
// container by merging runtime configuration specified by user
// and docker image default configuration.
class DockerRuntimeIsolatorProcess : public MesosIsolatorProcess
{
public:
  static Try<mesos::slave::Isolator*> create(const Flags& flags);

  virtual ~DockerRuntimeIsolatorProcess();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ContainerState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<Option<mesos::slave::ContainerLaunchInfo>> prepare(
      const ContainerID& containerId,
      const mesos::slave::ContainerConfig& containerConfig);

  virtual process::Future<Nothing> isolate(
      const ContainerID& containerId,
      pid_t pid);

  virtual process::Future<mesos::slave::ContainerLimitation> watch(
      const ContainerID& containerId);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<Nothing> cleanup(
      const ContainerID& containerId);

private:
  DockerRuntimeIsolatorProcess(
      const Flags& flags);

  Option<Environment> getEnvironment(
      const mesos::slave::ContainerConfig& containerConfig);

  // The following logic table shows how mesos CommandInfo cooperate
  // with docker image default config. `0` represents `isNone`, while
  // `1` represents `isSome`. Except the first col and the first row,
  // in all other cells, the first item is the executable, and the
  // rest are arguments.
  // +---------+--------------+--------------+--------------+--------------+
  // |         | Entrypoint=0 | Entrypoint=0 | Entrypoint=1 | Entrypoint=1 |
  // |         |     Cmd=0    |     Cmd=1    |     Cmd=0    |     Cmd=1    |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |     Error    |   ./Cmd[0]   | ./Entrypt[0] | ./Entrypt[0] |
  // | value=0 |              |   Cmd[1]..   | Entrypt[1].. | Entrypt[1].. |
  // |  argv=0 |              |              |              |     Cmd..    |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |     Error    |   ./Cmd[0]   | ./Entrypt[0] | ./Entrypt[0] |
  // | value=0 |              |     argv     | Entrypt[1].. | Entrypt[1].. |
  // |  argv=1 |              |              |     argv     |     argv     |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |    ./value   |    ./value   |    ./value   |    ./value   |
  // | value=1 |              |              |              |              |
  // |  argv=0 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=0  |    ./value   |    ./value   |    ./value   |    ./value   |
  // | value=1 |     argv     |     argv     |     argv     |     argv     |
  // |  argv=1 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |     Error    |     Error    |     Error    |     Error    |
  // | value=0 |              |              |              |              |
  // |  argv=0 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |     Error    |     Error    |     Error    |     Error    |
  // | value=0 |              |              |              |              |
  // |  argv=1 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |
  // | value=1 |     value    |     value    |     value    |     value    |
  // |  argv=0 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  // |   sh=1  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |  /bin/sh -c  |
  // | value=1 |     value    |     value    |     value    |     value    |
  // |  argv=1 |              |              |              |              |
  // +---------+--------------+--------------+--------------+--------------+
  Try<CommandInfo> getCommandInfo(
      const mesos::slave::ContainerConfig& containerConfig);

  std::string getWorkingDir(
      const mesos::slave::ContainerConfig& containerConfig,
      CommandInfo& commandInfo);

  const Flags flags;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __DOCKER_RUNTIME_ISOLATOR_HPP__
