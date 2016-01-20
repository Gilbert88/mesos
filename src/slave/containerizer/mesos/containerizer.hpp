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

#ifndef __MESOS_CONTAINERIZER_HPP__
#define __MESOS_CONTAINERIZER_HPP__

#include <list>
#include <vector>

#include <mesos/slave/container_logger.hpp>
#include <mesos/slave/isolator.hpp>

#include <process/metrics/counter.hpp>

#include <stout/hashmap.hpp>
#include <stout/multihashmap.hpp>

#include "slave/state.hpp"

#include "slave/containerizer/containerizer.hpp"

#include "slave/containerizer/mesos/launcher.hpp"

#include "slave/containerizer/mesos/provisioner/provisioner.hpp"

namespace mesos {
namespace internal {
namespace slave {

extern const char MESOS_CONTAINERIZER[];

// Forward declaration.
class MesosContainerizerProcess;

class MesosContainerizer : public Containerizer
{
public:
  static Try<MesosContainerizer*> create(
      const Flags& flags,
      bool local,
      Fetcher* fetcher);

  MesosContainerizer(
      const Flags& flags,
      bool local,
      Fetcher* fetcher,
      const process::Owned<mesos::slave::ContainerLogger>& logger,
      const process::Owned<Launcher>& launcher,
      const process::Owned<Provisioner>& provisioner,
      const std::vector<process::Owned<mesos::slave::Isolator>>& isolators);

  // Used for testing.
  MesosContainerizer(const process::Owned<MesosContainerizerProcess>& _process);

  virtual ~MesosContainerizer();

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const TaskInfo& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual void destroy(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID>> containers();

private:
  process::Owned<MesosContainerizerProcess> process;
};


class MesosContainerizerProcess
  : public process::Process<MesosContainerizerProcess>
{
public:
  MesosContainerizerProcess(
      const Flags& _flags,
      bool _local,
      Fetcher* _fetcher,
      const process::Owned<mesos::slave::ContainerLogger>& _logger,
      const process::Owned<Launcher>& _launcher,
      const process::Owned<Provisioner>& _provisioner,
      const std::vector<process::Owned<mesos::slave::Isolator>>& _isolators)
    : flags(_flags),
      local(_local),
      fetcher(_fetcher),
      logger(_logger),
      launcher(_launcher),
      provisioner(_provisioner),
      isolators(_isolators) {}

  virtual ~MesosContainerizerProcess() {}

  virtual process::Future<Nothing> recover(
      const Option<state::SlaveState>& state);

  virtual process::Future<bool> launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint);

  virtual process::Future<Nothing> update(
      const ContainerID& containerId,
      const Resources& resources);

  virtual process::Future<ResourceStatistics> usage(
      const ContainerID& containerId);

  virtual process::Future<containerizer::Termination> wait(
      const ContainerID& containerId);

  virtual process::Future<bool> exec(
      const ContainerID& containerId,
      int pipeWrite);

  virtual void destroy(const ContainerID& containerId);

  virtual process::Future<hashset<ContainerID>> containers();

  // Made public for testing.
  void ___recover(
      const ContainerID& containerId,
      const process::Future<std::list<process::Future<Nothing>>>& future);

private:
  process::Future<Nothing> _recover(
      const std::list<mesos::slave::ContainerState>& recoverable,
      const hashset<ContainerID>& orphans);

  process::Future<std::list<Nothing>> recoverIsolators(
      const std::list<mesos::slave::ContainerState>& recoverable,
      const hashset<ContainerID>& orphans);

  process::Future<Nothing> recoverProvisioner(
      const std::list<mesos::slave::ContainerState>& recoverable,
      const hashset<ContainerID>& orphans);

  process::Future<Nothing> __recover(
      const std::list<mesos::slave::ContainerState>& recovered,
      const hashset<ContainerID>& orphans);

  process::Future<std::list<Option<mesos::slave::ContainerLaunchInfo>>>
    prepare(const ContainerID& containerId,
            const Option<TaskInfo>& taskInfo,
            const ExecutorInfo& executorInfo,
            const std::string& directory,
            const Option<std::string>& user,
            const Option<ProvisionInfo>& provisionInfo);

  process::Future<Nothing> fetch(
      const ContainerID& containerId,
      const CommandInfo& commandInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId);

  process::Future<bool> _launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint,
      const Option<ProvisionInfo>& provisionInfo);

  process::Future<bool> __launch(
      const ContainerID& containerId,
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const std::string& directory,
      const Option<std::string>& user,
      const SlaveID& slaveId,
      const process::PID<Slave>& slavePid,
      bool checkpoint,
      const Option<ProvisionInfo>& provisionInfo,
      const std::list<Option<mesos::slave::ContainerLaunchInfo>>& launchInfos);

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
      const Option<TaskInfo>& taskInfo,
      const ExecutorInfo& executorInfo,
      const Option<ProvisionInfo>& provisionInfo);

  process::Future<bool> isolate(
      const ContainerID& containerId,
      pid_t _pid);

  // Continues 'destroy()' once isolators has completed.
  void _destroy(const ContainerID& containerId);

  // Continues '_destroy()' once all processes have been killed by the launcher.
  void __destroy(
      const ContainerID& containerId,
      const process::Future<Nothing>& future);

  // Continues '__destroy()' once we get the exit status of the executor.
  void ___destroy(
      const ContainerID& containerId,
      const process::Future<Option<int>>& status,
      const Option<std::string>& message);

  // Continues '___destroy()' once all isolators have completed
  // cleanup.
  void ____destroy(
      const ContainerID& containerId,
      const process::Future<Option<int>>& status,
      const process::Future<std::list<process::Future<Nothing>>>& cleanups,
      Option<std::string> message);

  // Continues '____destroy()' once provisioner have completed destroy.
  void _____destroy(
      const ContainerID& containerId,
      const process::Future<Option<int>>& status,
      const process::Future<bool>& destroy,
      Option<std::string> message);

  // Call back for when an isolator limits a container and impacts the
  // processes. This will trigger container destruction.
  void limited(
      const ContainerID& containerId,
      const process::Future<mesos::slave::ContainerLimitation>& future);

  // Call back for when the executor exits. This will trigger container
  // destroy.
  void reaped(const ContainerID& containerId);

  // TODO(jieyu): Consider introducing an Isolators struct and moving
  // all isolator related operations to that struct.
  process::Future<std::list<process::Future<Nothing>>> cleanupIsolators(
      const ContainerID& containerId);

  const Flags flags;
  const bool local;
  Fetcher* fetcher;
  process::Owned<mesos::slave::ContainerLogger> logger;
  const process::Owned<Launcher> launcher;
  const process::Owned<Provisioner> provisioner;
  const std::vector<process::Owned<mesos::slave::Isolator>> isolators;

  enum State
  {
    PREPARING,
    ISOLATING,
    FETCHING,
    RUNNING,
    DESTROYING
  };

  struct Container
  {
    // Promise for futures returned from wait().
    process::Promise<containerizer::Termination> promise;

    // We need to keep track of the future exit status for each
    // executor because we'll only get a single notification when
    // the executor exits.
    process::Future<Option<int>> status;

    // We keep track of the future that is waiting for all the
    // isolators' prepare futures, so that destroy will only start
    // calling cleanup after all isolators has finished preparing.
    process::Future<std::list<Option<mesos::slave::ContainerLaunchInfo>>>
      launchInfos;

    // We keep track of the future that is waiting for all the
    // isolators' isolate futures, so that destroy will only start
    // calling cleanup after all isolators has finished isolating.
    process::Future<std::list<Nothing>> isolation;

    // We keep track of any limitations received from each isolator so we can
    // determine the cause of an executor termination.
    std::vector<mesos::slave::ContainerLimitation> limitations;

    // We keep track of the resources for each container so we can set the
    // ResourceStatistics limits in usage().
    Resources resources;

    // The executor's working directory on the host.
    std::string directory;

    State state;
  };

  hashmap<ContainerID, process::Owned<Container>> containers_;

  struct Metrics
  {
    Metrics();
    ~Metrics();

    process::metrics::Counter container_destroy_errors;
  } metrics;
};

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_HPP__
