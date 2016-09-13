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

#include <errno.h>
#ifdef __linux__
#include <sched.h>
#include <signal.h>
#endif // __linux__
#include <string.h>

#include <iostream>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/unreachable.hpp>

#include <stout/os/kill.hpp>

#ifdef __linux__
#include "linux/capabilities.hpp"
#include "linux/fs.hpp"
#include "linux/ns.hpp"
#endif

#include "mesos/mesos.hpp"

#include "slave/containerizer/mesos/launch.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

#ifdef __linux__
using mesos::internal::capabilities::Capabilities;
using mesos::internal::capabilities::Capability;
using mesos::internal::capabilities::ProcessCapabilities;
#endif // __linux__

namespace mesos {
namespace internal {
namespace slave {

const string MesosContainerizerLaunch::NAME = "launch";


MesosContainerizerLaunch::Flags::Flags()
{
  add(&Flags::command,
      "command",
      "The command to execute.");

  add(&Flags::working_directory,
      "working_directory",
      "The working directory for the command. It has to be an absolute path \n"
      "w.r.t. the root filesystem used for the command.");

#ifndef __WINDOWS__
  add(&Flags::rootfs,
      "rootfs",
      "Absolute path to the container root filesystem. The command will be \n"
      "interpreted relative to this path");

  add(&Flags::user,
      "user",
      "The user to change to.");
#endif // __WINDOWS__

  add(&Flags::pipe_read,
      "pipe_read",
      "The read end of the control pipe. This is a file descriptor \n"
      "on Posix, or a handle on Windows. It's caller's responsibility \n"
      "to make sure the file descriptor or the handle is inherited \n"
      "properly in the subprocess. It's used to synchronize with the \n"
      "parent process. If not specified, no synchronization will happen.");

  add(&Flags::pipe_write,
      "pipe_write",
      "The write end of the control pipe. This is a file descriptor \n"
      "on Posix, or a handle on Windows. It's caller's responsibility \n"
      "to make sure the file descriptor or the handle is inherited \n"
      "properly in the subprocess. It's used to synchronize with the \n"
      "parent process. If not specified, no synchronization will happen.");

  add(&Flags::pre_exec_commands,
      "pre_exec_commands",
      "The additional preparation commands to execute before\n"
      "executing the command.");

#ifdef __linux__
  add(&Flags::exit_status_path,
      "exit_status_path",
      "The path to write the exit status of the launched process to");

  add(&Flags::unshare_namespace_mnt,
      "unshare_namespace_mnt",
      "Whether to launch the command in a new mount namespace.",
      false);

  add(&capabilities,
      "capabilities",
      "Capabilities of the command can use.");
#endif // __linux__
}


#ifdef __linux__
// When launching the executor with an 'init' process, we need to
// forward all relevant signals to it. The functions below help to
// enable this forwarding.
static pid_t containerPid;


static void signalHandler(int sig)
{
  // We purposefully ignore the error here since we have to remain
  // async signal safe. The only possible error scenario relevant to
  // us is ESRCH, but if that happens that means our pid is already
  // gone and the process will exit soon. So we are safe.
  os::kill(containerPid, sig);
}


static Try<Nothing> forwardSignals(pid_t pid)
{
  containerPid = pid;

  // Forwarding signal handlers for all relevant signals.
  for (int i = 1; i < NSIG; i++) {
    // We don't want to forward the SIGCHLD signal, nor do we want to
    // handle it ourselves because we reap all children inline in the
    // `execute` function.
    if (i == SIGCHLD) {
      continue;
    }

    // We can't catch or ignore these signals, so we shouldn't try
    // to register a handler for them.
    if (i == SIGKILL || i == SIGSTOP) {
      continue;
    }

    if (os::signals::install(i, signalHandler) != 0) {
      // Error out if we cant install a handler for any non real-time
      // signals (i.e. any signal less or equal to `SIGUNUSED`). For
      // the real-time signals, we simply ignore the error and move on
      // to the next signal.
      //
      // NOTE: We can't just use `SIGRTMIN` because its value changes
      // based on signals used internally by glibc.
      if (i <= SIGUNUSED) {
        return ErrnoError("Unable to register signal '" + stringify(i) + "'");
      }
    }
  }

  return Nothing();
}
#endif // __linux__


int MesosContainerizerLaunch::execute()
{
  // Check command line flags.
  if (flags.command.isNone()) {
    cerr << "Flag --command is not specified" << endl;
    return EXIT_FAILURE;
  }

  bool controlPipeSpecified =
    flags.pipe_read.isSome() && flags.pipe_write.isSome();

  if ((flags.pipe_read.isSome() && flags.pipe_write.isNone()) ||
      (flags.pipe_read.isNone() && flags.pipe_write.isSome())) {
    cerr << "Flag --pipe_read and --pipe_write should either be "
         << "both set or both not set" << endl;
    return EXIT_FAILURE;
  }

  // Parse the command.
  Try<CommandInfo> command =
    ::protobuf::parse<CommandInfo>(flags.command.get());

  if (command.isError()) {
    cerr << "Failed to parse the command: " << command.error() << endl;
    return EXIT_FAILURE;
  }

  // Validate the command.
  if (command.get().shell()) {
    if (!command.get().has_value()) {
      cerr << "Shell command is not specified" << endl;
      return EXIT_FAILURE;
    }
  } else {
    if (!command.get().has_value()) {
      cerr << "Executable path is not specified" << endl;
      return EXIT_FAILURE;
    }
  }

  if (controlPipeSpecified) {
    int pipe[2] = { flags.pipe_read.get(), flags.pipe_write.get() };

    // NOTE: On windows we need to pass `HANDLE`s between processes,
    // as file descriptors are not unique across processes. Here we
    // convert back from from the `HANDLE`s we receive to fds that can
    // be used in os-agnostic code.
#ifdef __WINDOWS__
    pipe[0] = os::handle_to_fd(pipe[0], _O_RDONLY | _O_TEXT);
    pipe[1] = os::handle_to_fd(pipe[1], _O_TEXT);
#endif // __WINDOWS__

    Try<Nothing> close = os::close(pipe[1]);
    if (close.isError()) {
      cerr << "Failed to close pipe[1]: " << close.error() << endl;
      return EXIT_FAILURE;
    }

    // Do a blocking read on the pipe until the parent signals us to continue.
    char dummy;
    ssize_t length;
    while ((length = os::read(pipe[0], &dummy, sizeof(dummy))) == -1 &&
           errno == EINTR);

    if (length != sizeof(dummy)) {
       // There's a reasonable probability this will occur during
       // agent restarts across a large/busy cluster.
       cerr << "Failed to synchronize with agent "
            << "(it's probably exited)" << endl;
       return EXIT_FAILURE;
    }

    close = os::close(pipe[0]);
    if (close.isError()) {
      cerr << "Failed to close pipe[0]: " << close.error() << endl;
      return EXIT_FAILURE;
    }
  }

#ifdef __linux__
  // The existence of the `exit_status_path` flag implies that we will
  // fork-exec the command we are launching, rather than simply
  // execing it (so we have the opportunity to checkpoint its exit
  // status). We open the file now, in order to ensure that we can
  // write to it even if we `pivot_root` below.
  Option<int> exitStatusFd = None();

  if (flags.exit_status_path.isSome()) {
    Try<int> open = os::open(
        flags.exit_status_path.get(),
        O_WRONLY | O_CREAT | O_CLOEXEC,
        S_IRUSR | S_IWUSR);

    if (open.isError()) {
      cerr << "Failed to open file for writing the exit status"
           << " '" << flags.exit_status_path.get() << "':"
           << " " << open.error() << endl;
      return EXIT_FAILURE;
    }

    exitStatusFd = open.get();
  }

  if (flags.unshare_namespace_mnt) {
    if (unshare(CLONE_NEWNS) != 0) {
      cerr << "Failed to unshare mount namespace: "
           << os::strerror(errno) << endl;
      return EXIT_FAILURE;
    }
  }
#endif // __linux__

  // Run additional preparation commands. These are run as the same
  // user and with the environment as the agent.
  if (flags.pre_exec_commands.isSome()) {
    // TODO(jieyu): Use JSON::Array if we have generic parse support.
    JSON::Array array = flags.pre_exec_commands.get();
    foreach (const JSON::Value& value, array.values) {
      if (!value.is<JSON::Object>()) {
        cerr << "Invalid JSON format for flag --commands" << endl;
        return EXIT_FAILURE;
      }

      Try<CommandInfo> parse = ::protobuf::parse<CommandInfo>(value);
      if (parse.isError()) {
        cerr << "Failed to parse a preparation command: "
             << parse.error() << endl;
        return EXIT_FAILURE;
      }

      if (!parse.get().has_value()) {
        cerr << "The 'value' of a preparation command is not specified" << endl;
        return EXIT_FAILURE;
      }

      cout << "Executing pre-exec command '" << value << "'" << endl;

      // We use fork/exec here to avoid calling `process:subprocess()`
      // and initializing all of `libprocess` (and subsequently
      // creating a whole bunch of unused threads, etc.) just to run
      // this simple script. In the past, we used `os::system()` here
      // to avoid initializing libprocess, but this caused security
      // issues with allowing arbitrary shell commands to be appended
      // to root-level pre-exec commands that take strings as their
      // last argument (e.g. mount --bind <src> <target>, where target
      // is user supplied and is set to "target_dir; rm -rf /"). We
      // now handle this case by invoking `execvp` directly (which
      // ensures that exactly one command is invoked and that all of
      // the strings passed to it are treated as direct arguments to
      // that one command).
      //
      // TODO(klueska): Refactor `libprocess::subprocess()` to pull
      // the base functionality into stout to avoid initializing
      // libprocess for simple use cases like this one.
      pid_t pid = ::fork();

      if (pid == -1) {
        cerr << "Failed to fork() the pre-exec command: "
             << os::strerror(errno) << endl;
        return EXIT_FAILURE;
      }

      if (pid == 0) {
        if (parse->shell()) {
          // Execute the command using the shell.
          os::execlp(os::Shell::name,
                     os::Shell::arg0,
                     os::Shell::arg1,
                     parse->value().c_str(),
                     (char*) nullptr);
        } else {
          // Use execvp to launch the command.
          os::execvp(parse->value().c_str(),
                 os::raw::Argv(parse->arguments()));
        }

        cerr << "Failed to execute pre-exec command:"
             << " " << os::strerror(errno) << endl;
        UNREACHABLE();
      }

      int exitStatus = 0;
      Result<pid_t> waitpid = os::waitpid(pid, &exitStatus, 0);

      if (waitpid.isError()) {
        cerr << "Failed to os::waitpid() on pre-exec command"
             << " '" << value << "': " << waitpid.error() << endl;
        return EXIT_FAILURE;
      } else if (waitpid.isNone()) {
        cerr << "Calling os::waitpid() with blocking semantics"
             << "returned asynchronously for pre-exec command"
             << " '" << value << "': " << waitpid.error() << endl;
        return EXIT_FAILURE;
      }

      if (exitStatus != 0) {
        cerr << "The pre-exec command '" << value << "'"
             << " failed with exit status " << exitStatus << endl;
        return EXIT_FAILURE;
      }
    }
  }

#ifndef __WINDOWS__
  // NOTE: If 'flags.user' is set, we will get the uid, gid, and the
  // supplementary group ids associated with the specified user before
  // changing the filesystem root. This is because after changing the
  // filesystem root, the current process might no longer have access
  // to /etc/passwd and /etc/group on the host.
  Option<uid_t> uid;
  Option<gid_t> gid;
  vector<gid_t> gids;

  // TODO(gilbert): For the case container user exists, support
  // framework/task/default user -> container user mapping once
  // user namespace and container capabilities is available for
  // mesos container.

  if (flags.user.isSome()) {
    Result<uid_t> _uid = os::getuid(flags.user.get());
    if (!_uid.isSome()) {
      cerr << "Failed to get the uid of user '" << flags.user.get() << "': "
           << (_uid.isError() ? _uid.error() : "not found") << endl;
      return EXIT_FAILURE;
    }

    // No need to change user/groups if the specified user is the same
    // as that of the current process.
    if (_uid.get() != os::getuid().get()) {
      Result<gid_t> _gid = os::getgid(flags.user.get());
      if (!_gid.isSome()) {
        cerr << "Failed to get the gid of user '" << flags.user.get() << "': "
             << (_gid.isError() ? _gid.error() : "not found") << endl;
        return EXIT_FAILURE;
      }

      Try<vector<gid_t>> _gids = os::getgrouplist(flags.user.get());
      if (_gids.isError()) {
        cerr << "Failed to get the supplementary gids of user '"
             << flags.user.get() << "': "
             << (_gids.isError() ? _gids.error() : "not found") << endl;
        return EXIT_FAILURE;
      }

      uid = _uid.get();
      gid = _gid.get();
      gids = _gids.get();
    }
  }
#endif // __WINDOWS__

#ifdef __linux__
  // Initialize capabilities support if necessary.
  Try<Capabilities> capabilitiesManager = Error("Not initialized");

  if (flags.capabilities.isSome()) {
    capabilitiesManager = Capabilities::create();
    if (capabilitiesManager.isError()) {
      cerr << "Failed to initialize capabilities support: "
           << capabilitiesManager.error() << endl;
      return EXIT_FAILURE;
    }

    // Prevent clearing of capabilities on `setuid`.
    if (uid.isSome()) {
      Try<Nothing> keepCaps = capabilitiesManager->setKeepCaps();
      if (keepCaps.isError()) {
        cerr << "Failed to set process control for keeping capabilities "
             << "on potential uid change: " << keepCaps.error() << endl;
        return EXIT_FAILURE;
      }
    }
  }
#endif // __linux__

#ifdef __WINDOWS__
  // Not supported on Windows.
  const Option<std::string> rootfs = None();
#else
  const Option<std::string> rootfs = flags.rootfs;
#endif // __WINDOWS__

  // Change root to a new root, if provided.
  if (rootfs.isSome()) {
    cout << "Changing root to " << rootfs.get() << endl;

    // Verify that rootfs is an absolute path.
    Result<string> realpath = os::realpath(rootfs.get());
    if (realpath.isError()) {
      cerr << "Failed to determine if rootfs is an absolute path: "
           << realpath.error() << endl;
      return EXIT_FAILURE;
    } else if (realpath.isNone()) {
      cerr << "Rootfs path does not exist" << endl;
      return EXIT_FAILURE;
    } else if (realpath.get() != rootfs.get()) {
      cerr << "Rootfs path is not an absolute path" << endl;
      return EXIT_FAILURE;
    }

#ifdef __linux__
    Try<Nothing> chroot = fs::chroot::enter(rootfs.get());
#elif defined(__WINDOWS__)
    Try<Nothing> chroot = Error("`chroot` not supported on Windows");
#else // For any other platform we'll just use POSIX chroot.
    Try<Nothing> chroot = os::chroot(rootfs.get());
#endif // __linux__
    if (chroot.isError()) {
      cerr << "Failed to enter chroot '" << rootfs.get()
           << "': " << chroot.error();
      return EXIT_FAILURE;
    }
  }

  // Change user if provided. Note that we do that after executing the
  // preparation commands so that those commands will be run with the
  // same privilege as the mesos-agent.
#ifndef __WINDOWS__
  if (uid.isSome()) {
    Try<Nothing> setgid = os::setgid(gid.get());
    if (setgid.isError()) {
      cerr << "Failed to set gid to " << gid.get()
           << ": " << setgid.error() << endl;
      return EXIT_FAILURE;
    }

    Try<Nothing> setgroups = os::setgroups(gids, uid);
    if (setgroups.isError()) {
      cerr << "Failed to set supplementary gids: "
           << setgroups.error() << endl;
      return EXIT_FAILURE;
    }

    Try<Nothing> setuid = os::setuid(uid.get());
    if (setuid.isError()) {
      cerr << "Failed to set uid to " << uid.get()
           << ": " << setuid.error() << endl;
      return EXIT_FAILURE;
    }
  }
#endif // __WINDOWS__

#ifdef __linux__
  if (flags.capabilities.isSome()) {
    Try<CapabilityInfo> requestedCapabilities =
      ::protobuf::parse<CapabilityInfo>(flags.capabilities.get());

    if (requestedCapabilities.isError()) {
      cerr << "Failed to parse capabilities: "
           << requestedCapabilities.error() << endl;
      return EXIT_FAILURE;
    }

    Try<ProcessCapabilities> capabilities = capabilitiesManager->get();
    if (capabilities.isError()) {
      cerr << "Failed to get capabilities for the current process: "
           << capabilities.error() << endl;
      return EXIT_FAILURE;
    }

    // After 'setuid', 'effective' set is cleared. Since `SETPCAP` is
    // required in the `effective` set of a process to change the
    // bounding set, we need to restore it first.
    capabilities->add(capabilities::EFFECTIVE, capabilities::SETPCAP);

    Try<Nothing> setPcap = capabilitiesManager->set(capabilities.get());
    if (setPcap.isError()) {
      cerr << "Failed to add SETPCAP to the effective set: "
           << setPcap.error() << endl;
      return EXIT_FAILURE;
    }

    // Set up requested capabilities.
    Set<Capability> target = capabilities::convert(requestedCapabilities.get());

    capabilities->set(capabilities::EFFECTIVE, target);
    capabilities->set(capabilities::PERMITTED, target);
    capabilities->set(capabilities::INHERITABLE, target);
    capabilities->set(capabilities::BOUNDING, target);

    Try<Nothing> set = capabilitiesManager->set(capabilities.get());
    if (set.isError()) {
      cerr << "Failed to set process capabilities: " << set.error() << endl;
      return EXIT_FAILURE;
    }
  }
#endif // __linux__

  if (flags.working_directory.isSome()) {
    Try<Nothing> chdir = os::chdir(flags.working_directory.get());
    if (chdir.isError()) {
      cerr << "Failed to chdir into current working directory "
           << "'" << flags.working_directory.get() << "': "
           << chdir.error() << endl;
      return EXIT_FAILURE;
    }
  }

  // Relay the environment variables.
  // TODO(jieyu): Consider using a clean environment.

#ifdef __linux__
  // If we have `exitStatusFd` set, then we need to fork-exec the
  // command we are launching and write its exit status out to
  // persistent storage. We use fork-exec directly (as opposed to
  // `process::subprocess()`) for the same reasons described above for
  // the pre-exec commands.
  if (exitStatusFd.isSome()) {
    pid_t pid = ::fork();

    if (pid == -1) {
      cerr << "Failed to fork() the command: "
           << os::strerror(errno) << endl;
      return EXIT_FAILURE;
    }

    if (pid == 0) {
      if (command->shell()) {
        // Execute the command using the shell.
        os::execlp(os::Shell::name,
                   os::Shell::arg0,
                   os::Shell::arg1,
                   command->value().c_str(),
                   (char*) nullptr);
      } else {
        // Use execvp to launch the command.
        os::execvp(command->value().c_str(),
               os::raw::Argv(command->arguments()));
      }

      cerr << "Failed to execute command: " << os::strerror(errno) << endl;
      UNREACHABLE();
    }

    // Forward all incoming signals to the newly created process.
    Try<Nothing> signals = forwardSignals(pid);
    if (signals.isError()) {
      cerr << "Failed to forward signals: " << signals.error() << endl;
      return EXIT_FAILURE;
    }

    // Wait for the newly created process to finish.
    int exitStatus = 0;
    Result<pid_t> waitpid = None();

    // Reap all decendants, but only continue once we reap the
    // process we just launched.
    do {
      waitpid = os::waitpid(-1, &exitStatus, 0);

      if (waitpid.isError()) {
        cerr << "Failed to os::waitpid(): " << waitpid.error() << endl;
        return EXIT_FAILURE;
      } else if (waitpid.isNone()) {
        cerr << "Calling os::waitpid() with blocking semantics"
             << "returned asynchronously" << endl;
        return EXIT_FAILURE;
      }
    } while (pid != waitpid.get());

    // Checkpoint the exit status of the command.
    // It's ok to block here, so we just `os::write()` directly.
    Try<Nothing> write = os::write(
        exitStatusFd.get(),
        stringify(exitStatus));

    os::close(exitStatusFd.get());

    if (write.isError()) {
      cerr << "Failed to write the exit status"
           << " '" << stringify(exitStatus) << "' to"
           << " '" << flags.exit_status_path.get() << ":"
           << " " << write.error() << endl;
      return EXIT_FAILURE;
    }

    return exitStatus;
  }
#endif // __linux__

  if (command->shell()) {
    // Execute the command using shell.
    os::execlp(os::Shell::name,
               os::Shell::arg0,
               os::Shell::arg1,
               command->value().c_str(),
               (char*) nullptr);
  } else {
    // Use execvp to launch the command.
    os::execvp(command->value().c_str(),
           os::raw::Argv(command->arguments()));
  }

  // If we get here, the execle call failed.
  cerr << "Failed to execute command: " << os::strerror(errno) << endl;
  UNREACHABLE();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
