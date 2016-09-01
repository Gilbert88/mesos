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

#include "linux/ns.hpp"

#include "slave/containerizer/mesos/launch.hpp"
#include "slave/containerizer/mesos/linux_launcher.hpp"

#include "tests/environment.hpp"
#include "tests/mesos.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


class LinuxLauncherTest : public MesosTest
{
protected:
  struct Parameters
  {
    vector<string> argv;
    string path;
    slave::MesosContainerizerLaunch::Flags* flags;
  };

  Try<Parameters> prepare(
      const slave::Flags& flags,
      const string& command,
      const Option<string>& exitStatusCheckpointPath = None())
  {
    Parameters parameters;

    parameters.path = path::join(flags.launcher_dir, "mesos-containerizer");

    parameters.argv.resize(2);
    parameters.argv[0] = "mesos-containerizer";
    parameters.argv[1] = slave::MesosContainerizerLaunch::NAME;

    parameters.flags = new slave::MesosContainerizerLaunch::Flags();

    CommandInfo commandInfo;
    commandInfo.set_shell(true);
    commandInfo.set_value(command);

    parameters.flags->command = JSON::protobuf(commandInfo);

    Try<string> directory = environment->mkdtemp();
    if (directory.isError()) {
      return Error("Failed to create directory: " + directory.error());
    }

    parameters.flags->working_directory = directory.get();

    parameters.flags->pipe_read = open("/dev/zero", O_RDONLY);
    parameters.flags->pipe_write = open("/dev/null", O_WRONLY);

    if (exitStatusCheckpointPath.isSome()) {
      parameters.flags->exit_status_path = exitStatusCheckpointPath.get();
    }

    return parameters;
  }
};


TEST_F(LinuxLauncherTest, ROOT_CGROUPS_ForkNoCheckpointExitStatus)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
  ASSERT_SOME(create);

  slave::Launcher* launcher = create.get();

  ContainerID containerId;
  containerId.set_value("kevin");

  Try<Parameters> parameters = prepare(flags, "exit 0");
  ASSERT_SOME(parameters);

  Parameters parameters_ = parameters.get();

  Try<pid_t> pid = launcher->fork(
      containerId,
      parameters->path,
      parameters->argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO), // TODO(benh): Don't output unnecessarily.
      Subprocess::FD(STDERR_FILENO),
      parameters->flags,
      None(),
      CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

  ASSERT_SOME(pid);

  Future<Option<int>> wait = launcher->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(WIFEXITED(wait.get().get()));
  EXPECT_EQ(0, WEXITSTATUS(wait.get().get()));

  AWAIT_READY(launcher->destroy(containerId));
}


TEST_F(LinuxLauncherTest, ROOT_CGROUPS_Fork)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
  ASSERT_SOME(create);

  slave::Launcher* launcher = create.get();

  ContainerID containerId;
  containerId.set_value("kevin");

  Try<Parameters> parameters = prepare(
      flags,
      "exit 0",
      launcher->getExitStatusCheckpointPath(containerId));

  ASSERT_SOME(parameters);

  Try<pid_t> pid = launcher->fork(
      containerId,
      parameters->path,
      parameters->argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO), // TODO(benh): Don't output unnecessarily.
      Subprocess::FD(STDERR_FILENO),
      parameters->flags,
      None(),
      CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

  ASSERT_SOME(pid);

  Future<Option<int>> wait = launcher->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(WIFEXITED(wait.get().get()));
  EXPECT_EQ(0, WEXITSTATUS(wait.get().get()));

  AWAIT_READY(launcher->destroy(containerId));
}


TEST_F(LinuxLauncherTest, ROOT_CGROUPS_Recover)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
  ASSERT_SOME(create);

  slave::Launcher* launcher = create.get();

  ContainerID containerId;
  containerId.set_value("kevin");

  Try<Parameters> parameters = prepare(
      flags,
      "exit 0",
      launcher->getExitStatusCheckpointPath(containerId));

  ASSERT_SOME(parameters);

  Try<pid_t> pid = launcher->fork(
      containerId,
      parameters->path,
      parameters->argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO), // TODO(benh): Don't output unnecessarily.
      Subprocess::FD(STDERR_FILENO),
      parameters->flags,
      None(),
      CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

  ASSERT_SOME(pid);

  delete launcher;

  create = slave::LinuxLauncher::create(flags);
  ASSERT_SOME(create);

  launcher = create.get();

  AWAIT_READY(launcher->recover({}));

  Future<ContainerStatus> status = launcher->status(containerId);
  AWAIT_READY(status);
  EXPECT_EQ(pid.get(), status->executor_pid());

  Future<Option<int>> wait = launcher->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(WIFEXITED(wait.get().get()));
  EXPECT_EQ(0, WEXITSTATUS(wait.get().get()));

  AWAIT_READY(launcher->destroy(containerId));
}


TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedForkNoNamespaces)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
  ASSERT_SOME(create);

  slave::Launcher* launcher = create.get();

  ContainerID containerId;
  containerId.set_value("kevin");

  Try<Parameters> parameters = prepare(
      flags,
      "sleep 1", // Keep outer container around for entire test.
      launcher->getExitStatusCheckpointPath(containerId));

  ASSERT_SOME(parameters);

  Try<pid_t> pid = launcher->fork(
      containerId,
      parameters->path,
      parameters->argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO), // TODO(benh): Don't output unnecessarily.
      Subprocess::FD(STDERR_FILENO),
      parameters->flags,
      None(),
      CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

  close(parameters->flags->pipe_read.get());
  close(parameters->flags->pipe_write.get());

  ASSERT_SOME(pid);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value("ben");

  Try<Parameters> nestedParameters = prepare(
      flags,
      "exit 42",
      launcher->getExitStatusCheckpointPath(nestedContainerId));

  ASSERT_SOME(nestedParameters);

  Try<pid_t> nestedPid = launcher->fork(
      nestedContainerId,
      nestedParameters->path,
      nestedParameters->argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO), // TODO(benh): Don't output unnecessarily.
      Subprocess::FD(STDERR_FILENO),
      nestedParameters->flags,
      None(),
      None());

  close(nestedParameters->flags->pipe_read.get());
  close(nestedParameters->flags->pipe_write.get());

  ASSERT_SOME(nestedPid);

  // Check UTS namespace.
  Try<ino_t> inode = ns::getns(pid.get(), "uts");
  ASSERT_SOME(inode);
  EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "uts"));
  EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "uts"));

  // Check NET namespace.
  inode = ns::getns(pid.get(), "net");
  ASSERT_SOME(inode);
  EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "net"));
  EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "net"));

  // Check PID namespace.
  inode = ns::getns(pid.get(), "pid");
  ASSERT_SOME(inode);
  EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "pid"));
  EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "pid"));

  Future<Option<int>> wait = launcher->wait(nestedContainerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(WIFEXITED(wait.get().get()));
  EXPECT_EQ(42, WEXITSTATUS(wait.get().get()));
  // EXPECT_EXIT_STATUS(42, launcher->wait(nestedContainerId));

  wait = launcher->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_TRUE(WIFEXITED(wait.get().get()));
  EXPECT_EQ(0, WEXITSTATUS(wait.get().get()));
  // EXPECT_EXIT_STATUS(42, launcher->wait(containerId));

  AWAIT_READY(launcher->destroy(nestedContainerId));
  AWAIT_READY(launcher->destroy(containerId));
}


TEST_F(LinuxLauncherTest, ROOT_CGROUPS_NestedRecoverNoNamespaces)
{
  slave::Flags flags = CreateSlaveFlags();

  Try<slave::Launcher*> create = slave::LinuxLauncher::create(flags);
  ASSERT_SOME(create);

  slave::Launcher* launcher = create.get();

  ContainerID containerId;
  containerId.set_value("kevin");

  Try<Parameters> parameters = prepare(
      flags,
      "sleep 1", // Keep outer container around for entire test.
      launcher->getExitStatusCheckpointPath(containerId));

  ASSERT_SOME(parameters);

  Try<pid_t> pid = launcher->fork(
      containerId,
      parameters->path,
      parameters->argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO), // TODO(benh): Don't output unnecessarily.
      Subprocess::FD(STDERR_FILENO),
      parameters->flags,
      None(),
      CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID);

  close(parameters->flags->pipe_read.get());
  close(parameters->flags->pipe_write.get());

  ASSERT_SOME(pid);

  // Now launch nested container.
  ContainerID nestedContainerId;
  nestedContainerId.mutable_parent()->CopyFrom(containerId);
  nestedContainerId.set_value("ben");

  Try<Parameters> nestedParameters = prepare(
      flags,
      "exit 42",
      launcher->getExitStatusCheckpointPath(nestedContainerId));

  ASSERT_SOME(nestedParameters);

  Try<pid_t> nestedPid = launcher->fork(
      nestedContainerId,
      nestedParameters->path,
      nestedParameters->argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO), // TODO(benh): Don't output unnecessarily.
      Subprocess::FD(STDERR_FILENO),
      nestedParameters->flags,
      None(),
      None());

  close(nestedParameters->flags->pipe_read.get());
  close(nestedParameters->flags->pipe_write.get());

  ASSERT_SOME(nestedPid);

  // Check UTS namespace.
  Try<ino_t> inode = ns::getns(pid.get(), "uts");
  ASSERT_SOME(inode);
  EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "uts"));
  EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "uts"));

  // Check NET namespace.
  inode = ns::getns(pid.get(), "net");
  ASSERT_SOME(inode);
  EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "net"));
  EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "net"));

  // Check PID namespace.
  inode = ns::getns(pid.get(), "pid");
  ASSERT_SOME(inode);
  EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), "pid"));
  EXPECT_SOME_EQ(inode.get(), ns::getns(nestedPid.get(), "pid"));

  delete launcher;

  create = slave::LinuxLauncher::create(flags);
  ASSERT_SOME(create);

  launcher = create.get();

  AWAIT_READY(launcher->recover({}));

  Future<ContainerStatus> status = launcher->status(containerId);
  AWAIT_READY(status);
  EXPECT_EQ(pid.get(), status->executor_pid());

  status = launcher->status(nestedContainerId);
  AWAIT_READY(status);
  EXPECT_EQ(nestedPid.get(), status->executor_pid());

  Future<Option<int>> wait = launcher->wait(nestedContainerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());
  ASSERT_TRUE(WIFEXITED(wait.get().get()));
  EXPECT_EQ(42, WEXITSTATUS(wait.get().get()));
  // EXPECT_EXIT_STATUS(42, launcher->wait(nestedContainerId));

  wait = launcher->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_TRUE(WIFEXITED(wait.get().get()));
  EXPECT_EQ(0, WEXITSTATUS(wait.get().get()));
  // EXPECT_EXIT_STATUS(42, launcher->wait(containerId));

  AWAIT_READY(launcher->destroy(nestedContainerId));
  AWAIT_READY(launcher->destroy(containerId));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
