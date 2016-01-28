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

#include <list>
#include <string>

#include <glog/logging.h>

#include <mesos/docker/v1.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos/isolators/docker/runtime.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

using mesos::slave::ContainerConfig;
using mesos::slave::ContainerLaunchInfo;
using mesos::slave::ContainerLimitation;
using mesos::slave::ContainerState;
using mesos::slave::Isolator;

namespace mesos {
namespace internal {
namespace slave {

DockerRuntimeIsolatorProcess::DockerRuntimeIsolatorProcess(
    const Flags& _flags)
  : flags(_flags) {}


DockerRuntimeIsolatorProcess::~DockerRuntimeIsolatorProcess() {}


Try<Isolator*> DockerRuntimeIsolatorProcess::create(const Flags& flags)
{
  process::Owned<MesosIsolatorProcess> process(
      new DockerRuntimeIsolatorProcess(flags));

  return new MesosIsolator(process);
}


Future<Nothing> DockerRuntimeIsolatorProcess::recover(
    const list<ContainerState>& states,
    const hashset<ContainerID>& orphans)
{
  return Nothing();
}


Future<Option<ContainerLaunchInfo>> DockerRuntimeIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ContainerConfig& containerConfig)
{
  const ExecutorInfo& executorInfo = containerConfig.executorinfo();

  if (executorInfo.has_container() &&
      executorInfo.container().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare docker runtime for a MESOS contaienr");
  }

  if (containerConfig.has_taskinfo() &&
      containerConfig.taskinfo().has_container() &&
      containerConfig.taskinfo().container().type() != ContainerInfo::MESOS) {
    return Failure("Can only prepare docker runtime for a MESOS container");
  }

  LOG(INFO) << "Preparing docker runtime for container: "
            << stringify(containerId);

  if (!containerConfig.taskinfo().has_container() &&
      !executorInfo.has_container()) {
    return None();
  }

  if (!containerConfig.has_docker()) {
    // No docker image default config available.
    return None();
  }

  // Contains docker image default environment variables, merged
  // command, and default working directory.
  ContainerLaunchInfo launchInfo;

  Option<Environment> environment = getEnvironment(containerConfig);
  if (environment.isSome()) {
    launchInfo.mutable_environment()->CopyFrom(environment.get());
  }

  Try<CommandInfo> info = getCommandInfo(containerConfig);
  if (info.isError()) {
    return Failure("Failed to get CommandInfo: " + info.error());
  }

  // Keep a copy of CommandInfo, in case working directory has
  // to be mutated for command executor.
  CommandInfo commandInfo = info.get();

  launchInfo.set_working_dir(getWorkingDir(containerConfig, commandInfo));
  launchInfo.mutable_command()->CopyFrom(commandInfo);

  return launchInfo;
}


Option<Environment> DockerRuntimeIsolatorProcess::getEnvironment(
    const ContainerConfig& containerConfig)
{
  CHECK(containerConfig.docker().manifest().has_config());

  if (containerConfig.docker().manifest().config().env_size() == 0) {
    return None();
  }

  Environment environment;

  foreach (
      const string& env, containerConfig.docker().manifest().config().env()) {
    const vector<string> tokens = strings::tokenize(env, "=");
    if (tokens.size() != 2) {
      // Do not return an error. Skip any invalid environment variable.
      VLOG(1) << "Skipping invalid environment variable: '"
              << env << "'";

      continue;
    }

    // Keep all environment from runtime isolator. If there exists
    // environment variable duplicate cases, it will be overwrited
    // in mesos containerizer.
    Environment::Variable* variable = environment.add_variables();
    variable->set_name(tokens[0]);
    variable->set_value(tokens[1]);
  }

  return environment;
}


Try<CommandInfo> DockerRuntimeIsolatorProcess::getCommandInfo(
    const ContainerConfig& containerConfig)
{
  CHECK(containerConfig.docker().manifest().has_config());

  // We mutate the CommandInfo following the logic:
  // 1. If shell is specified as true, the commandInfo value (has to
  //    be provided) will be regarded as shell command.
  // 2. If shell is specified as false, use the commandInfo value
  //    as executable (not doing anything here).
  // 3. If shell is specified as false and the commandInfo value is
  //    not set, use the docker image specified runtime configuration.
  //    i. If `Entrypoint` is specified, it is treated as executable,
  //       then `Cmd` get appended as arguments to `Entrypoint`.
  //    ii.If `Entrypoint` is not specified, use the first `Cmd` as
  //       executable and the rest as arguments.
  CommandInfo commandInfo = containerConfig.executorinfo().command();

  if (commandInfo.shell()) {
    if (!commandInfo.has_value()) {
      return Error("Shell specified but no command value provided");
    }
  } else {
    // Only possible for custom executor.
    if (!containerConfig.has_taskinfo() &&
        !commandInfo.has_value()) {
      // We keep the arguments of commandInfo while it does not have
      // a value, so that arguments can be specified by user in custom
      // executor, which is running with default image executable.
      docker::spec::v1::ImageManifest::Config config =
        containerConfig.docker().manifest().config();

      // Filter out executable for commandInfo value.
      if (config.entrypoint_size() > 0) {
        commandInfo.set_value(config.entrypoint(0));

        // Put user defined argv after default entrypoint argv
        // in sequence.
        commandInfo.clear_arguments();

        for (int i = 1; i < config.entrypoint_size(); i++) {
          commandInfo.add_arguments(config.entrypoint(i));
        }

        commandInfo.mutable_arguments()->MergeFrom(
            containerConfig.executorinfo().command().arguments());

        // Overwrite default cmd arguments if CommandInfo arguments
        // are set by user.
        if (commandInfo.arguments_size() == 0) {
          foreach (const string& cmd, config.cmd()) {
            commandInfo.add_arguments(cmd);
          }
        }
      } else if (config.cmd_size() > 0) {
        commandInfo.set_value(config.cmd(0));

        // Overwrite default cmd arguments if CommandInfo arguments
        // are set by user.
        if (commandInfo.arguments_size() == 0) {
          for (int i = 1; i < config.cmd_size(); i++) {
            commandInfo.add_arguments(config.cmd(i));
          }
        }
      } else {
        return Error(
            "No executable found for executor: '" +
            containerConfig.executorinfo().executor_id().value() +
            "'");
      }
    }

    if (!commandInfo.has_value()) {
      Error(
          "No executable found for executor: '" +
          containerConfig.executorinfo().executor_id().value() + "'");
    }

    // Only possible for command executor. Image specified entrypoint
    // or cmd is passed as an argument to command executor's flag.
    if (containerConfig.has_taskinfo() &&
        containerConfig.docker().manifest().has_config()) {
      // Exclude override case.
      foreach (const string& argument, commandInfo.arguments()) {
        if (strings::startsWith(argument, "--override")) {
          return commandInfo;
        }
      }

      CHECK(
          containerConfig.taskinfo().has_command());

      CommandInfo taskCommand =
        containerConfig.taskinfo().command();

      if (taskCommand.has_value()) {
        return commandInfo;
      }

      // Merge image default entrypoing and cmd into task command.
      docker::spec::v1::ImageManifest::Config config =
        containerConfig.docker().manifest().config();

      if (config.entrypoint_size() > 0) {
        taskCommand.set_value(config.entrypoint(0));

        taskCommand.clear_arguments();

        for (int i = 1; i < config.entrypoint_size(); i++) {
          taskCommand.add_arguments(config.entrypoint(i));
        }

        taskCommand.mutable_arguments()->MergeFrom(
            containerConfig.taskinfo().command().arguments());

        if (taskCommand.arguments_size() == 0) {
          foreach (const string& cmd, config.cmd()) {
            taskCommand.add_arguments(cmd);
          }
        }
      } else if (config.cmd_size() > 0) {
        taskCommand.set_value(config.cmd(0));

        if (taskCommand.arguments_size() == 0) {
          for (int i = 1; i < config.cmd_size(); i++) {
            taskCommand.add_arguments(config.cmd(i));
          }
        }
      } else {
        return Error(
            "No executable found for task: '" +
            containerConfig.taskinfo().task_id().value() + "'");
      }

      JSON::Object object = JSON::protobuf(taskCommand);

      // Pass task command as a flag, which will be loaded
      // by command executor.
      commandInfo.add_arguments("--task_command=" + stringify(object));
    }
  }

  return commandInfo;
}


string DockerRuntimeIsolatorProcess::getWorkingDir(
    const ContainerConfig& containerConfig,
    CommandInfo& commandInfo)
{
  const string& directory = containerConfig.has_rootfs()
    ? flags.sandbox_directory
    : containerConfig.directory();

  if (!containerConfig.has_docker()) {
    return directory;
  }

  CHECK(containerConfig.docker().manifest().has_config());

  if (!containerConfig.docker().manifest().config().has_workingdir()) {
    return directory;
  }

  const string& workDir =
    containerConfig.docker().manifest().config().workingdir();

  if (containerConfig.has_taskinfo()) {
    // Command executor.
    for (int i = 0; i < commandInfo.arguments_size(); i++) {
      if (strings::startsWith(commandInfo.arguments(i),
                              "--sandbox_directory")) {
        commandInfo.set_arguments(
            i, path::join(commandInfo.arguments(i), workDir));

        break;
      }
    }
  } else {
    // Custom executor.
    return containerConfig.has_rootfs()
      ? path::join(flags.sandbox_directory, workDir)
      : path::join(containerConfig.directory(), workDir);
  }

  return directory;
}


Future<Nothing> DockerRuntimeIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  return Nothing();
}


Future<ContainerLimitation> DockerRuntimeIsolatorProcess::watch(
    const ContainerID& containerId)
{
  return Future<ContainerLimitation>();
}


Future<Nothing> DockerRuntimeIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  return Nothing();
}


Future<ResourceStatistics> DockerRuntimeIsolatorProcess::usage(
    const ContainerID& containerId)
{
  return ResourceStatistics();
}


Future<Nothing> DockerRuntimeIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
