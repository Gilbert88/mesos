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

#include "linux/cgroups.hpp"
#include "linux/ns.hpp"
#include "linux/systemd.hpp"

#include "mesos/resources.hpp"

#include "slave/containerizer/mesos/linux_launcher.hpp"

#include "slave/containerizer/mesos/isolators/namespaces/pid.hpp"

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

// `_systemdHierarchy` is only set if running on a systemd environment.
LinuxLauncher::LinuxLauncher(
    const Flags& _flags,
    const string& _freezerHierarchy,
    const Option<string>& _systemdHierarchy)
  : flags(_flags),
    freezerHierarchy(_freezerHierarchy),
    systemdHierarchy(_systemdHierarchy) {}


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
  // The LinuxLauncher takes responsibility for creating and starting this
  // slice. It then migrates executor pids into this slice before it "unpauses"
  // the executor. This is the same pattern as the freezer.

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
  //   - we run as root
  //   - "freezer" subsytem is enabled.

  Try<bool> freezer = cgroups::enabled("freezer");
  return ::geteuid() == 0 &&
         freezer.isSome() &&
         freezer.get();
}


// Helper for traversing the cgroups used to contruct a new Container
// and accumulate all of the constructed containers.
Try<hashmap<ContainerID, LinuxLauncher::Container>> LinuxLauncher::recover(
      const string& cgroup,
      hashmap<ContainerID, Container> containers)
{
  Container container;

  // Determine the ContainerID for this cgroup.
  foreach (const string& token, strings::tokenize(cgroup, "/")) {
    // Only add a parent if this isn't the first token (which we
    // determine by checking if the current 'id' field has been
    // set).
    if (container.id.has_value()) {
      container.id.mutable_parent()->CopyFrom(container.id);
    }
    container.id.set_value(token);
  }

  // Validate the ID (there should be at least one level).
  if (!container.id.has_value()) {
    return Error("Failed to get ContainerID from cgroup '" + cgroup + "'");
  }

  // Recover the checkpointed 'PID 1' for this container. Note that
  // it's possible that we don't have a pid checkpointed either
  // because (1) we created the cgroup but terminated before
  // checkpointing the pid (which means the pid must have terminated
  // also because of the way 'subprocess' works) or (2) we've just
  // recovered containers launched with an earlier version of the
  // launcher that did not checkpoint pids.
  string path = path::join(
      getRuntimePathForContainer(flags, container.id),
      "pid");

  if (!os::exists(path)) {
    container.pid = None();
  } else {
    Try<string> read = os::read(path);
    if (read.isError()) {
      return Error("Failed to read pid of container at '" + path +
                   "': " + read.error());
    }

    Try<pid_t> pid = numify<pid_t>(read.get());
    if (pid.isError()) {
      return Error("Failed to numify pid '" + read.get() +
                   "'of container at '" + path + "': " + pid.error());
    }

    container.pid = pid.get();
  }

  containers.put(container.id, container);

  return containers;
}


Future<hashset<ContainerID>> LinuxLauncher::recover(
    const list<ContainerState>& states)
{
  // Traverse all of the cgroups to recover exisiting containers.
  Try<hashmap<ContainerID, Container>> traverse = cgroups::traverse(
      freezerHierarchy,
      flags.cgroups_root,
      [this](const string& cgroup, hashmap<ContainerID, Container> containers) {
        return recover(cgroup, containers);
      },
      hashmap<ContainerID, Container>(),
      cgroups::Traversal::PRE_ORDER);

  if (traverse.isError()) {
    return Failure("Failed to traverse the cgroups: " + traverse.error());
  }

  containers = traverse.get();

  // Now loop through the containers expected by ContainerState, which
  // should only be top-level containers in order to help us determine
  // what containers are orphans.
  hashset<ContainerID> expected = {};

  foreach (const ContainerState& state, states) {
    if (!containers.contains(state.container_id())) {
      // This may occur if the freezer cgroup was destroyed but our
      // process restarts before being able to record this. So,
      // nothing to do here (a future call to 'wait' for this
      // container will just return immediately), but we do verbose
      // log.
      VLOG(2) << "Couldn't find freezer cgroup for container "
              << state.container_id() << ", assuming already destroyed";
      continue;
    }

    // It's possible that we'll have recovered the container but don't
    // have a pid because that container was launched before the
    // launcher started checkpointing pids, so we can save the pid
    // from ContainerState so we don't consider the container
    // orphaned.
    if (containers.contains(state.container_id())) {
      // For now we just overwrite the pid even if we already
      // checkpointed the pid ourselves. In the future when the
      // launcher is the only one responsible for checkpointing pids
      // then we can just remove this code (i.e., a container without
      // a checkpointed pid will be an orphan that was likely created
      // because our process restarted before the 'subprocess' hook
      // for checkpointing the pid completed ... and note that
      // subprocess "does the right thing" and makes sure that the
      // child terminates if the hooks don't complete).
      containers[state.container_id()].pid = state.pid();
    }

    expected.insert(state.container_id());
  }

  // TODO(benh): In the past we used to make sure that we didn't have
  // multiple containers that had the same pid. This seemed pretty
  // random, and is highly unlikely to occur in practice. A good
  // sanity check we could do here, however, is to make sure that the
  // pid is actually contained within each container. Note that we'd
  // need to do this here versus when we traverse and recover each
  // container because we might recover a container without a
  // checkpointed pid due to those containers being launched before
  // the launcher started checkpointing pids and we need to have
  // looped through each ContainerState to fill in any pids.

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
        // TODO(benh): Do we expect all containers to be in the
        // 'MESOS_EXECUTORS_SLICE'? See TODO in 'fork' below as
        // well. For now we only do this for top-level containers.
        if (container.id.has_parent()) {
          continue;
        }

        // Skip if we don't have a pid for the container.
        if (container.pid.isNone()) {
          continue;
        }

        if (mesosExecutorSlicePids.get().count(container.pid.get()) <= 0) {
          // TODO(jmlvanre): Add a flag that enforces this rather
          // than just logs a warning (i.e., we exit if a pid was
          // found in the freezer but not in the
          // `MESOS_EXECUTORS_SLICE`. We need a flag to support the
          // upgrade path.
          LOG(WARNING)
            << "Couldn't find pid '" << container.pid.get() << "' in '"
            << systemd::mesos::MESOS_EXECUTORS_SLICE << "'. This can lead to"
            << " lack of proper resource isolation";
        }
      }
    }
  }

  // Finally, determine and return the set of orphaned top-level
  // containers.
  //
  // TODO(benh): Do we want to return nested orphaned containers too?
  // Or do we just want to destroy nested orphaned containers here?
  hashset<ContainerID> orphans = {};
  foreachvalue (const Container& container, containers) {
    if (!container.id.has_parent() && !expected.contains(container.id)) {
      orphans.insert(container.id);
    }
  }

  return orphans;
}


Try<pid_t> LinuxLauncher::fork(
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
  if (containers.contains(containerId)) {
    return Error("Container '" + stringify(containerId) + "' already exists");
  }

  int cloneFlags = namespaces.isSome() ? namespaces.get() : 0;

  LOG(INFO) << "Cloning with namespaces " << ns::stringify(cloneFlags);

  cloneFlags |= SIGCHLD; // Specify SIGCHLD as child termination signal.

  // NOTE: Currently we don't care about the order of the hooks.

  // If we are on systemd, then extend the life of the child by
  // putting it into a slice. As with the freezer, any grandchildren
  // will also be contained in the slice.
  if (systemdHierarchy.isSome()) {
    parentHooks.emplace_back(Subprocess::Hook([](pid_t child) {
      return systemd::mesos::extendLifetime(child);
    }));
  }

  // Create parent Hook for isolating child in a freezer cgroup (will
  // also create the cgroup if necessary).
  parentHooks.emplace_back(Subprocess::Hook([=](pid_t child) {
    return cgroups::isolate(
        freezerHierarchy,
        cgroup(containerId),
        child);
  }));

  // Create parent Hook for checkpointing the pid.
  parentHooks.emplace_back(Subprocess::Hook([=](pid_t child) -> Try<Nothing> {
    // Need to make sure the directory has been created first. Note
    // that we use 'this->flags' here because we have to disambiguate
    // from the 'flags' parameter passed to 'LinuxLauncher::fork'.
    //
    // TODO(benh): Should really just have 'os::write' create the
    // directories recursively as needed.
    string directory = getRuntimePathForContainer(this->flags, containerId);

    Try<Nothing> mkdir = os::mkdir(directory);
    if (mkdir.isError()) {
      return Error(
          "Failed to make directory '" + directory + "': " + mkdir.error());
    }

    return os::write(path::join(directory, "pid"), stringify(child));
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
      [cloneFlags](const lambda::function<int()>& child) {
        return os::clone(child, cloneFlags);
      },
      parentHooks);

  if (child.isError()) {
    return Error("Failed to clone child process: " + child.error());
  }

  Container container;
  container.id = containerId;
  container.pid = child.get().pid();;

  containers.put(containerId, container);

  return child.get().pid();
}


Future<Nothing> LinuxLauncher::destroy(const ContainerID& containerId)
{
  Option<Container> container = containers.get(containerId);

  if (container.isNone()) {
    return Failure("Container does not exist");
  }

  // NOTE: currently cleaning up the checkpointed 'pid' file is not
  // the responsiblity of the launcher.

  // Try and use the pid namespace to destroy the container first if
  // it's available and then use the freezer second.
  return [=]() -> Future<Nothing> {
    Result<ino_t> inode =
      NamespacesPidIsolatorProcess::getNamespace(containerId);
    if (inode.isSome()) {
      LOG(INFO) << "Using pid namespace to destroy container "
                << cgroup(containerId);
      return ns::pid::destroy(inode.get());
    } else {
      return Nothing();
    }
  }().then([=]() {
    LOG(INFO) << "Using freezer to destroy container " << cgroup(containerId);
    return cgroups::destroy(
        freezerHierarchy,
        cgroup(containerId),
        cgroups::DESTROY_TIMEOUT);
  });
}


Future<ContainerStatus> LinuxLauncher::status(const ContainerID& containerId)
{
  Option<Container> container = containers.get(containerId);

  if (container.isNone()) {
    return Failure("Container does not exist");
  } else if (container->pid.isNone()) {
    return Failure("Container is orphaned");
  }

  ContainerStatus status;
  status.set_executor_pid(container->pid.get());

  return status;
}


string LinuxLauncher::getExitStatusCheckpointPath(
    const ContainerID& containerId)
{
  return path::join(
      getRuntimePathForContainer(flags, containerId),
      "exit_status");
}


string LinuxLauncher::cgroup(const ContainerID& containerId)
{
  return path::join(
      flags.cgroups_root,
      buildPathForContainer(containerId));
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
