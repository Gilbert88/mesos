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

#include <sys/wait.h>

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/reap.hpp>

#include "linux/cgroups.hpp"
#include "linux/ns.hpp"

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using namespace process;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::internal::slave::state::SlaveState;

using mesos::slave::ContainerState;
using mesos::slave::ContainerTermination;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


// class LinuxLauncherTest : public MesosTest
// {
// protected:
//   struct Parameters
//   {
//     vector<string> argv;
//     string path;
//     Subprocess::IO stdin = Subprocess::FD(STDIN_FILENO);
//     Subprocess::IO stdout = Subprocess::FD(STDOUT_FILENO);
//     Subprocess::IO stderr = Subprocess::FD(STDERR_FILENO);
//     slave::MesosContainerizerLaunch::Flags flags;
//   };

//   Try<Parameters> prepare(
//       const slave::Flags& flags,
//       const string& command,
//       const Option<string>& exitStatusCheckpointPath = None())
//   {
//     Parameters parameters;

//     parameters.path = path::join(flags.launcher_dir, "mesos-containerizer");

//     // TODO(benh): Reset `parameters.stdin`, `parameters.stdout`,
//     // `parameters.stderr` if `flags.verbose` is false.

//     parameters.argv.resize(2);
//     parameters.argv[0] = "mesos-containerizer";
//     parameters.argv[1] = slave::MesosContainerizerLaunch::NAME;

//     CommandInfo commandInfo;
//     commandInfo.set_shell(true);
//     commandInfo.set_value(command);

//     parameters.flags.command = JSON::protobuf(commandInfo);

//     Try<string> directory = environment->mkdtemp();
//     if (directory.isError()) {
//       return Error("Failed to create directory: " + directory.error());
//     }

//     parameters.flags.working_directory = directory.get();

//     if (exitStatusCheckpointPath.isSome()) {
//       parameters.flags.exit_status_path = exitStatusCheckpointPath.get();
//     }

//     return parameters;
//   }
// };


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_ForkNoCheckpointExitStatus)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(flags, "exit 0");
//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   AWAIT_EXPECT_WEXITSTATUS_EQ(0, launcher->wait(containerId));

//   AWAIT_READY(launcher->destroy(containerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_Fork)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "exit 0",
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   AWAIT_EXPECT_WEXITSTATUS_EQ(0, launcher->wait(containerId));

//   AWAIT_READY(launcher->destroy(containerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_Recover)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "exit 0",
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   delete launcher;

//   create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   launcher = create.get();

//   AWAIT_READY(launcher->recover({}));

//   Future<ContainerStatus> status = launcher->status(containerId);
//   AWAIT_READY(status);
//   EXPECT_EQ(pid.get(), status->executor_pid());

//   // Since process is just exiting we can `wait` before we `destroy`.
//   AWAIT_EXPECT_WEXITSTATUS_EQ(0, launcher->wait(containerId));

//   AWAIT_READY(launcher->destroy(containerId));
// }


//   // uncheckpointed container (i.e., an unknown launched container)

//   // orphan cgroup (i.e., a legacy partially launched container)


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_RecoverUncheckpointed)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   // Launch a process in a cgroup that the LinuxLauncher will assume
//   // is an uncheckpointed container.
//   //
//   // NOTE: we need to call `cgroups::hierarchy` this AFTER
//   // `LinuxLauncher::create` so that a `cgroups::prepare` will have
//   // been called already.
//   Result<string> hierarchy = cgroups::hierarchy("freezer");
//   ASSERT_SOME(hierarchy);

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   string cgroup = path::join(
//       flags.cgroups_root,
//       slave::Launcher::buildPathForContainer(containerId));

//   Try<string> directory = environment->mkdtemp();
//   ASSERT_SOME(directory);

//   Try<Subprocess> child = subprocess(
//       "exit 0",
//       Subprocess::FD(STDIN_FILENO),
//       Subprocess::FD(STDOUT_FILENO),
//       Subprocess::FD(STDERR_FILENO),
//       SETSID,
//       None(),
//       None(),
//       {Subprocess::Hook([=](pid_t pid) {
//         return cgroups::isolate(hierarchy.get(), cgroup, pid);
//       })},
//       directory.get());

//   ASSERT_SOME(child);

//   ContainerState state;
//   state.mutable_executor_info()->mutable_executor_id()->set_value("executor"); // NOLINT
//   state.mutable_container_id()->CopyFrom(containerId);
//   state.set_pid(child->pid());
//   state.set_directory(directory.get());

//   // Now make sure we can recover.
//   AWAIT_ASSERT_EQ(hashset<ContainerID>::EMPTY, launcher->recover({state}));

//   Future<ContainerStatus> status = launcher->status(containerId);
//   AWAIT_READY(status);
//   EXPECT_EQ(child->pid(), status->executor_pid());

//   // And now make sure we can wait on it, but not since this is an
//   // uncheckpointed container not launched by the launcher the exit
//   // status will be `None`.
//   Future<Option<int>> wait = launcher->wait(containerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_READY(wait);
//   EXPECT_NONE(wait.get());
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedForkNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "exit 42",
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Check UTS namespace.
//   Try<ino_t> inode = ns::getns(pid.get(), "uts");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "uts"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "uts"));

//   // Check NET namespace.
//   inode = ns::getns(pid.get(), "net");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "net"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "net"));

//   // Check PID namespace.
//   inode = ns::getns(pid.get(), "pid");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "pid"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "pid"));

//   AWAIT_EXPECT_WEXITSTATUS_EQ(42, launcher->wait(nestedContainerId));
//   AWAIT_READY(launcher->destroy(nestedContainerId));

//   AWAIT_READY(launcher->destroy(containerId));
//   AWAIT_EXPECT_WEXITSTATUS_NE(0, launcher->wait(containerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedForkDestroyNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "read", // Keep nested container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Now destroy outer container.
//   Future<Option<int>> wait = launcher->wait(containerId);
//   Future<Option<int>> nestedWait = launcher->wait(nestedContainerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_EXPECT_WEXITSTATUS_NE(0, wait);
//   AWAIT_EXPECT_WEXITSTATUS_NE(0, nestedWait);

//   AWAIT_READY(launcher->destroy(nestedContainerId));
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedForkNestedDestroyNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "read", // Keep nested container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Now destroy nested container.
//   Future<Option<int>> nestedWait = launcher->wait(nestedContainerId);

//   AWAIT_READY(launcher->destroy(nestedContainerId));

//   AWAIT_EXPECT_WEXITSTATUS_NE(0, nestedWait);

//   Future<Option<int>> wait = launcher->wait(containerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, wait);
// }


// TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedRecoverNoNamespaces)
// {
//   slave::Flags flags = CreateSlaveFlags();

//   Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   slave::Launcher* launcher = create.get();

//   ContainerID containerId;
//   containerId.set_value("kevin");

//   Try<Parameters> parameters = prepare(
//       flags,
//       "read", // Keep outer container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(containerId));

//   ASSERT_SOME(parameters);

//   Try<pid_t> pid = launcher->fork(
//       containerId,
//       parameters->path,
//       parameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &parameters->flags,
//       None(),
//       CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

//   ASSERT_SOME(pid);

//   // Now launch nested container.
//   ContainerID nestedContainerId;
//   nestedContainerId.mutable_parent()->CopyFrom(containerId);
//   nestedContainerId.set_value("ben");

//   Try<Parameters> nestedParameters = prepare(
//       flags,
//       "read", // Keep nested container around by blocking on stdin.
//       launcher->getExitStatusCheckpointPath(nestedContainerId));

//   ASSERT_SOME(nestedParameters);

//   Try<pid_t> nestedPid = launcher->fork(
//       nestedContainerId,
//       nestedParameters->path,
//       nestedParameters->argv,
//       parameters->stdin,
//       parameters->stdout,
//       parameters->stderr,
//       &nestedParameters->flags,
//       None(),
//       None());

//   ASSERT_SOME(nestedPid);

//   // Check UTS namespace.
//   Try<ino_t> inode = ns::getns(pid.get(), "uts");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "uts"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "uts"));

//   // Check NET namespace.
//   inode = ns::getns(pid.get(), "net");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "net"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "net"));

//   // Check PID namespace.
//   inode = ns::getns(pid.get(), "pid");
//   ASSERT_SOME(inode);
//   EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "pid"));
//   EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "pid"));

//   delete launcher;

//   create = slave::LinuxLauncher::create(flags);
//   ASSERT_SOME(create);

//   launcher = create.get();

//   AWAIT_READY(launcher->recover({}));

//   Future<ContainerStatus> status = launcher->status(containerId);
//   AWAIT_READY(status);
//   EXPECT_EQ(pid.get(), status->executor_pid());

//   status = launcher->status(nestedContainerId);
//   AWAIT_READY(status);
//   EXPECT_EQ(nestedPid.get(), status->executor_pid());

//   Future<Option<int>> nestedWait = launcher->wait(nestedContainerId);

//   AWAIT_READY(launcher->destroy(nestedContainerId));

//   AWAIT_EXPECT_WEXITSTATUS_NE(0, nestedWait);

//   Future<Option<int>> wait = launcher->wait(containerId);

//   AWAIT_READY(launcher->destroy(containerId));

//   AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, wait);
// }


class MesosContainerizerTest : public MesosTest {};


TEST_F(MesosContainerizerTest, NestedContainerID)
{
  ContainerID id1;
  id1.set_value(UUID::random().toString());

  ContainerID id2;
  id2.set_value(UUID::random().toString());

  EXPECT_EQ(id1, id1);
  EXPECT_NE(id1, id2);

  ContainerID id3 = id1;
  id3.mutable_parent()->CopyFrom(id2);

  EXPECT_EQ(id3, id3);
  EXPECT_NE(id3, id1);

  hashset<ContainerID> ids;
  ids.insert(id2);
  ids.insert(id3);

  EXPECT_TRUE(ids.contains(id2));
  EXPECT_TRUE(ids.contains(id3));
  EXPECT_FALSE(ids.contains(id1));
}


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_Launch)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,cgroups/mem,namespaces/pid";

  Fetcher fetcher;

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  MesosContainerizer* containerizer = create.get();

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value("kevin");

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "exit 42");
  executor.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:32").get());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      SlaveID(),
      map<string, string>(),
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerTermination> wait = containerizer->wait(containerId);

  AWAIT_READY(wait);

  ASSERT_TRUE(wait->has_status());
  int status = wait->status();
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(42, WEXITSTATUS(status));

  delete containerizer;
}


TEST_F(MesosContainerizerTest, ROOT_CGROUPS_NestedLaunch)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.launcher = "linux";
  flags.isolation = "cgroups/cpu,cgroups/mem,namespaces/pid";

  Fetcher fetcher;

  Try<MesosContainerizer*> create = MesosContainerizer::create(
      flags,
      false,
      &fetcher);

  ASSERT_SOME(create);

  MesosContainerizer* containerizer = create.get();

  SlaveState state;
  state.id = SlaveID();

  AWAIT_READY(containerizer->recover(state));

  ContainerID containerId;
  containerId.set_value("kevin");

  ExecutorInfo executor = CREATE_EXECUTOR_INFO("executor", "read");
  executor.mutable_resources()->CopyFrom(
      Resources::parse("cpus:1;mem:32").get());

  Try<string> directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      executor,
      directory.get(),
      None(),
      state.id,
      map<string, string>(),
      true); // TODO(benh): Ever want to check not-checkpointing?

  AWAIT_ASSERT_TRUE(launch);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value("ben");

  directory = environment->mkdtemp();
  ASSERT_SOME(directory);

  launch = containerizer->launch(
      nestedContainerId,
      CREATE_COMMAND_INFO("exit 42"),
      None(),
      Resources::parse("cpus:1;mem:32").get(),
      directory.get(),
      None(),
      state.id);

  AWAIT_ASSERT_TRUE(launch);

  Future<ContainerTermination> wait = containerizer->wait(nestedContainerId);

  AWAIT_READY(wait);

  ASSERT_TRUE(wait->has_status());
  int status = wait->status();
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(42, WEXITSTATUS(status));

  wait = containerizer->wait(containerId);

  containerizer->destroy(containerId);

  AWAIT_READY(wait);
  ASSERT_TRUE(wait->has_status());
  status = wait->status();
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));

  delete containerizer;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
