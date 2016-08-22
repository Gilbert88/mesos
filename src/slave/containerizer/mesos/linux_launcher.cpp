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

#include <sched.h>
#include <unistd.h>

#include <linux/sched.h>

#include <vector>

#include <process/collect.hpp>

#include <stout/abort.hpp>
#include <stout/check.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/stat.hpp>

#include "linux/cgroups.hpp"
#include "linux/ns.hpp"
#include "linux/systemd.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/mesos/linux_launcher.hpp"
#include "slave/containerizer/mesos/paths.hpp"

using namespace process;

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

using mesos::slave::ContainerState;

namespace mesos {
namespace internal {
namespace slave {

// Launcher for Linux systems with cgroups. Uses a freezer cgroup to
// track pids.
class LinuxLauncherProcess : public Process<LinuxLauncherProcess>
{
public:
  LinuxLauncherProcess(
      const Flags& flags,
      const std::string& freezerHierarchy,
      const Option<std::string>& systemdHierarchy);

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

private:
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
    // containers).
    pid_t pid;

    Option<Future<Nothing>> destroy;
  };

  // Helper for recovering containers at some relative `directory`
  // within the launcher runtime directory.
  Option<Error> recover(const std::string& directory);

  // Helper for determining the cgroup for a container (i.e., the path
  // in a cgroup subsystem).
  std::string cgroup(const ContainerID& containerId);

  static const std::string subsystem;
  const Flags flags;
  const std::string freezerHierarchy;
  const Option<std::string> systemdHierarchy;
  hashmap<ContainerID, Container> containers;
};


Try<Launcher*> LinuxLauncher::create(const Flags& flags)
{
  Try<string> freezerHierarchy = cgroups::prepare(
      flags.cgroups_hierarchy,
      "freezer",
      flags.cgroups_root);

  if (freezerHierarchy.isError()) {
    return Error(
        "Failed to create Linux launcher: " + freezerHierarchy.error());
  }

  // Ensure that no other subsystem is attached to the freezer hierarchy.
  Try<set<string>> subsystems = cgroups::subsystems(freezerHierarchy.get());
  if (subsystems.isError()) {
    return Error(
        "Failed to get the list of attached subsystems for hierarchy " +
        freezerHierarchy.get());
  } else if (subsystems.get().size() != 1) {
    return Error(
        "Unexpected subsystems found attached to the hierarchy " +
        freezerHierarchy.get());
  }

  LOG(INFO) << "Using " << freezerHierarchy.get()
            << " as the freezer hierarchy for the Linux launcher";

  // On systemd environments we currently migrate executor pids into a separate
  // executor slice. This allows the life-time of the executor to be extended
  // past the life-time of the slave. See MESOS-3352.
  //
  // The LinuxLauncherProcess takes responsibility for creating and
  // starting this slice. It then migrates executor pids into this
  // slice before it "unpauses" the executor. This is the same pattern
  // as the freezer.

  return new LinuxLauncher(
      flags,
      freezerHierarchy.get(),
      systemd::enabled() ?
        Some(systemd::hierarchy()) :
        Option<string>::none());
}


bool LinuxLauncher::available()
{
  // Make sure:
  //   1. Are running as root.
  //   2. 'freezer' subsytem is enabled.
  Try<bool> freezer = cgroups::enabled("freezer");
  return ::geteuid() == 0 && freezer.isSome() && freezer.get();
}


LinuxLauncher::LinuxLauncher(
    const Flags& flags,
    const string& freezerHierarchy,
    const Option<string>& systemdHierarchy)
  : process(new LinuxLauncherProcess(flags, freezerHierarchy, systemdHierarchy))
{
  process::spawn(process.get());
}


LinuxLauncher::~LinuxLauncher()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<hashset<ContainerID>> LinuxLauncher::recover(
    const std::list<mesos::slave::ContainerState>& states)
{
  return dispatch(process.get(), &LinuxLauncherProcess::recover, states);
}


Try<pid_t> LinuxLauncher::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<std::string>& argv,
    const process::Subprocess::IO& in,
    const process::Subprocess::IO& out,
    const process::Subprocess::IO& err,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& namespaces,
    vector<Subprocess::Hook> parentHooks)
{
  return dispatch(
      process.get(),
      &LinuxLauncherProcess::fork,
      containerId,
      path,
      argv,
      in,
      out,
      err,
      flags,
      environment,
      namespaces,
      parentHooks).get();
}


Future<Nothing> LinuxLauncher::destroy(const ContainerID& containerId)
{
  return dispatch(process.get(), &LinuxLauncherProcess::destroy, containerId);
}


Future<ContainerStatus> LinuxLauncher::status(
    const ContainerID& containerId)
{
  return dispatch(process.get(), &LinuxLauncherProcess::status, containerId);
}


// `_systemdHierarchy` is only set if running on a systemd environment.
LinuxLauncherProcess::LinuxLauncherProcess(
    const Flags& _flags,
    const string& _freezerHierarchy,
    const Option<string>& _systemdHierarchy)
  : flags(_flags),
    freezerHierarchy(_freezerHierarchy),
    systemdHierarchy(_systemdHierarchy) {}


Option<Error> LinuxLauncherProcess::recover(const string& directory)
{
  // Loop through each container at the path, if it exists.
  const string path = path::join(
      flags.runtime_dir,
      "launcher",
      flags.launcher,
      directory);

  if (!os::exists(path)) {
    return None();
  }

  Try<list<string>> entries = os::ls(path);

  if (entries.isError()) {
    return Error("Failed to list '" + path + "': " + entries.error());
  }

  foreach (const string& entry, entries.get()) {
    // We're not expecting anything else but directories here
    // representing each container.
    CHECK(os::stat::isdir(path::join(path, entry)));

    // TODO(benh): Validate that the entry looks like a ContainerID?

    Container container;

    // Determine the ContainerID from 'directory/entry' (we explicitly
    // do not want use `path` because it contains things that we don't
    // want in our ContainerID and even still we have to skip all
    // instances of 'containers' as well).
    vector<string> tokens =
      strings::tokenize(path::join(directory, entry), "/");
    foreach (const string& token, tokens) {
      // Skip the directory separator 'containers'.
      if (token == "containers") {
        continue;
      }

      ContainerID id;
      id.set_value(token);

      if (container.id.has_value()) {
        id.mutable_parent()->CopyFrom(container.id);
      }

      container.id = id;
    }

    // Validate the ID (there should be at least one level).
    if (!container.id.has_value()) {
      return Error("Failed to determine ContainerID from path '" + path + "'");
    }

    // Recover the checkpointed 'PID 1' for this container.
    if (!os::exists(path::join(path, entry, "pid"))) {
      // This is possible because we don't atomically create the
      // directory and write the 'pid' file and thus we might
      // terminate/restart after we've created the directory but
      // before we've written the file.
      LOG(WARNING) << "Found a container without a 'pid' file";
      container.pid = -1; // TODO(benh): Make `pid` optional?
    } else {
      Try<string> read = os::read(path::join(path, entry, "pid"));
      if (read.isError()) {
        return Error("Failed to recover pid of container: " + read.error());
      }

      Try<pid_t> pid = numify<pid_t>(read.get());
      if (pid.isError()) {
        return Error("Failed to numify pid '" + read.get() +
                     "'of container at '" + path + "': " + pid.error());
      }

      container.pid = pid.get();
    }

    // TODO(benh): Should we preemptively destroy partially launched
    // or partially destroyed containers? What about if they're
    // nested!?

    containers.put(container.id, container);

    LOG(INFO) << "Recovered checkpointed container " << container.id;

    // Now recursively recover nested containers.
    Option<Error> error = recover(path::join(directory, entry, "containers"));

    if (error.isSome()) {
      return error.get();
    }
  }

  return None();
}


Future<hashset<ContainerID>> LinuxLauncherProcess::recover(
    const list<ContainerState>& states)
{
  // Recover containers from the launcher runtime directory.
  Option<Error> error = recover("containers");

  if (error.isSome()) {
    return Failure(error.get().message);
  }

  // Now loop through the containers expected by ContainerState, which
  // should only be top-level containers, because we might have some
  // containers that we need to recover that either (1) we haven't
  // checkpointed because those containers were launched before we
  // started checkpointing or (2) had already been asked to be
  // destroyed and have successfully been destroyed (thus we didn't
  // recover them) but nobody else knows that because we
  // terminated/restarted before returning from `destroy`.
  hashset<ContainerID> expected = {};

  foreach (const ContainerState& state, states) {
    expected.insert(state.container_id());

    // Only add containers we don't know about.
    if (!containers.contains(state.container_id())) {
      Container container;
      container.id = state.container_id();
      container.pid = state.pid();

      // NOTE: as noted above, it's possible that this container has
      // already been destroyed but we still add it to `containers` so
      // that when `destroy` does get called for this container we
      // will not fail.
      containers.put(container.id, container);

      LOG(INFO) << "Recovered uncheckpointed container " << container.id;
    }
  }

  // TODO(benh): In the past we used to make sure that we didn't have
  // multiple containers that had the same pid. This seemed pretty
  // random, and is highly unlikely to occur in practice. That being
  // said, a good sanity check we could do here is to make sure that
  // the pid is actually contained within each container.

  // If we are on a systemd environment, check that container pids are
  // still in the `MESOS_EXECUTORS_SLICE`. If they are not, warn the
  // operator that resource isolation may be invalidated.
  if (systemdHierarchy.isSome()) {
    Result<std::set<pid_t>> mesosExecutorSlicePids = cgroups::processes(
        systemdHierarchy.get(),
        systemd::mesos::MESOS_EXECUTORS_SLICE);

    // If we error out trying to read the pids from the
    // `MESOS_EXECUTORS_SLICE` we fail. This is a programmer error
    // as we did not set up the slice correctly.
    if (mesosExecutorSlicePids.isError()) {
      return Failure("Failed to read pids from systemd '" +
                     stringify(systemd::mesos::MESOS_EXECUTORS_SLICE) + "'");
    }

    if (mesosExecutorSlicePids.isSome()) {
      foreachvalue (const Container& container, containers) {
        if (mesosExecutorSlicePids.get().count(container.pid) <= 0) {
          // TODO(jmlvanre): Add a flag that enforces this rather
          // than just logs a warning (i.e., we exit if a pid was
          // found in the freezer but not in the
          // `MESOS_EXECUTORS_SLICE`. We need a flag to support the
          // upgrade path.
          LOG(WARNING)
            << "Couldn't find pid '" << container.pid << "' in '"
            << systemd::mesos::MESOS_EXECUTORS_SLICE << "'. This can lead to"
            << " lack of proper resource isolation";
        }
      }
    }
  }

  // For backwards compatibility we need to return top-level "orphan
  // containers" that we find a cgroup for but don't have either (1) a
  // checkpointed pid for because they were launched before we started
  // checkpointing or (2) a ContainerState for because their
  // information was not properly checkpointed elsewhere. We treat
  // these as normal containers that can be destroyed by including
  // them in `containers` just with a `pid` of -1. Because these
  // containers were not part of ContainerState they will be
  // considered "unexpected" below and returned as orphans.
  Try<vector<string>> cgroups =
    cgroups::get(freezerHierarchy, flags.cgroups_root);

  if (cgroups.isError()) {
    return Failure(cgroups.error());
  }

  foreach (const string& cgroup, cgroups.get()) {
    // Only look for top-level containers.
    if (strings::contains(cgroup, "/")) {
      continue;
    }

    ContainerID id;
    id.set_value(cgroup);

    if (!containers.contains(id)) {
      Container container;
      container.id = id;
      container.pid = -1; // TODO(benh): Make `pid` optional?

      // Add this to `containers` so when `destroy` gets called we
      // properly destroy the container.
      containers.put(container.id, container);

      LOG(INFO) << "Recovered orphaned container " << container.id;
    }
  }

  // Return the list of top-level orphaned containers, i.e., a
  // container that we recovered but was not expected during
  // recovery (this could happen for example because we
  // terminate/restart before `fork` returns the pid of the container
  // to the caller).
  hashset<ContainerID> orphans = {};
  foreachvalue (const Container& container, containers) {
    if (!container.id.has_parent() && !expected.contains(container.id)) {
      orphans.insert(container.id);
    }
  }
  return orphans;
}


Try<pid_t> LinuxLauncherProcess::fork(
    const ContainerID& containerId,
    const string& path,
    const vector<string>& argv,
    const process::Subprocess::IO& in,
    const process::Subprocess::IO& out,
    const process::Subprocess::IO& err,
    const flags::FlagsBase* flags,
    const Option<map<string, string>>& environment,
    const Option<int>& namespaces,
    vector<Subprocess::Hook> parentHooks)
{
  // Make sure this container (nested or not) is unique.
  if (containers.contains(containerId)) {
    return Error("Container '" + stringify(containerId) + "' already exists");
  }

  Option<pid_t> target = None();

  // Ensure nested containers have known parents.
  if (containerId.has_parent()) {
    Option<Container> container = containers.get(containerId.parent());
    if (container.isNone()) {
      return Error("Unknown parent container");
    }

    if (container->pid == -1) {
      // TODO(benh): Could also look up a pid in the container and use
      // that in order to enter the namespaces? This would be best
      // effort because we don't know the namespaces that had been
      // created for the original pid.
      return Error("Unknown parent container pid");
    }

    target = container->pid;
  }

  int cloneFlags = namespaces.isSome() ? namespaces.get() : 0;

  LOG(INFO) << "Launching " << (target.isSome() ? "nested " : "")
            << "container " << containerId << " and cloning with namespaces "
            << ns::stringify(cloneFlags);

  cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  // NOTE: The ordering of hooks is VERY important here!
  //
  // (1) Add the pid to the systemd slice.
  // (2) Checkpoint the pid for the container.
  // (3) Create the freezer cgroup for the container.
  //
  // We must do (2) before (3) so that during recovery we can destroy
  // any partially launched containers. We do (1) first because if we
  // ever enforce all pids being in the systemd slice then the
  // recovery logic above expects the pid to be in the slice if we've
  // recovered the container.

  // Hook to extend the life of the child (and all of it's
  // descendants) using a systemd slice.
  if (systemdHierarchy.isSome()) {
    parentHooks.emplace_back(Subprocess::Hook([](pid_t child) {
      return systemd::mesos::extendLifetime(child);
    }));
  }

  // Hook for creating and assigning the child into a freezer cgroup.
  parentHooks.emplace_back(Subprocess::Hook([=](pid_t child) {
    return cgroups::isolate(
        freezerHierarchy,
        cgroup(containerId),
        child);
  }));

  Try<Subprocess> child = subprocess(
      path,
      argv,
      in,
      out,
      err,
      SETSID,
      flags,
      environment,
      [target, cloneFlags](const lambda::function<int()>& child) {
        if (target.isSome()) {
          // TODO(benh): Explain why we only enter these namesapces.
          Try<pid_t> pid = ns::clone(
              target.get(),
              CLONE_NEWUTS | CLONE_NEWNET | CLONE_NEWPID,
              child,
              cloneFlags);
          if (pid.isError()) {
            LOG(WARNING) << "Failed to enter namespaces and clone:"
                         << pid.error();
            return -1;
          }
          return pid.get();
        } else {
          return os::clone(child, cloneFlags);
        }
      },
      parentHooks);

  if (child.isError()) {
    return Error("Failed to clone child process: " + child.error());
  }

  Container container;
  container.id = containerId;
  container.pid = child.get().pid();

  containers.put(container.id, container);

  return container.pid;
}


Future<Nothing> LinuxLauncherProcess::destroy(const ContainerID& containerId)
{
  LOG(INFO) << "Asked to destroy container " << containerId;

  Option<Container> container = containers.get(containerId);

  if (container.isNone()) {
    return Failure("Container does not exist");
  }

  // Check if `container` has any nested containers.
  foreachkey (const ContainerID& id, containers) {
    if (id.has_parent()) {
      if (container->id == id.parent()) {
        return Failure("Container has nested containers");
      }
    }
  }

  // TODO(benh): Explain why we do this now and what kind of memory
  // safety we have to be weary of.
  containers.erase(container->id);

  // Determine if this is a partially launched or partially
  // destroyed container. A container is considered partially
  // launched or partially destroyed if we have recovered it from
  // the runtime directory but we don't have a freezer cgroup for
  // it.
  //
  // Even though we can't actually tell if the container was partially
  // launched or partially destroyed (i.e., because both don't have a
  // freezer cgroup), we treat both situations the same and just
  // (re)destroy the container. It should always be safe to call
  // `destroy` on these containers because we can assume that these
  // containers don't have any nested containers because if they were
  // partially launched nothing could have been nested and if they
  // were partially destroyed we shouldn't have started to destroy
  // them in the first place if they had anything nested.
  Try<bool> exists = cgroups::exists(freezerHierarchy, cgroup(container->id));
  if (exists.isError()) {
    return Failure("Failed to determine if cgroup exists: " + exists.error());
  }

  if (!exists.get()) {
    LOG(WARNING) << "Couldn't find freezer cgroup for container "
                 << container->id << " so assuming partially launched "
                 << "or partially destroyed";

    return Nothing();
  }

  LOG(INFO) << "Using freezer to destroy cgroup " << cgroup(container->id);

  // TODO(benh): What if we fail to destroy the container? Should we
  // retry?
  return cgroups::destroy(
      freezerHierarchy,
      cgroup(container->id),
      cgroups::DESTROY_TIMEOUT);
}


Future<ContainerStatus> LinuxLauncherProcess::status(
    const ContainerID& containerId)
{
  Option<Container> container = containers.get(containerId);

  if (container.isNone()) {
    return Failure("Container does not exist");
  }

  ContainerStatus status;
  status.set_executor_pid(container->pid);

  return status;
}


string LinuxLauncherProcess::cgroup(const ContainerID& containerId)
{
  return path::join(
      flags.cgroups_root,
      containerizer::paths::buildPathForContainer(containerId));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
