---
title: Apache Mesos - Mesos Containerizer
layout: documentation
---

# Mesos Containerizer

The MesosContainerizer provides lightweight containerization and
resource isolation of executors using Linux-specific functionality
such as control cgroups and namespaces. It is composable so operators
can selectively enable different isolators.

It also provides basic support for POSIX systems (e.g., OSX) but
without any actual isolation, only resource usage reporting.


### Shared Filesystem

The SharedFilesystem isolator can optionally be used on Linux hosts to
enable modifications to each container's view of the shared
filesystem.

The modifications are specified in the ContainerInfo included in the
ExecutorInfo, either by a framework or by using the
`--default_container_info` agent flag.

ContainerInfo specifies Volumes which map parts of the shared
filesystem (host\_path) into the container's view of the filesystem
(container\_path), as read-write or read-only. The host\_path can be
absolute, in which case it will make the filesystem subtree rooted at
host\_path also accessible under container\_path for each container.
If host\_path is relative then it is considered as a directory
relative to the executor's work directory. The directory will be
created and permissions copied from the corresponding directory (which
must exist) in the shared filesystem.

The primary use-case for this isolator is to selectively make parts of
the shared filesystem private to each container. For example, a
private "/tmp" directory can be achieved with `host_path="tmp"` and
`container_path="/tmp"` which will create a directory "tmp" inside the
executor's work directory (mode 1777) and simultaneously mount it as
/tmp inside the container. This is transparent to processes running
inside the container. Containers will not be able to see the host's
/tmp or any other container's /tmp.


### Pid Namespace

The Pid Namespace isolator can be used to isolate each container in
a separate pid namespace with two main benefits:

1. Visibility: Processes running in the container (executor and
   descendants) are unable to see or signal processes outside the
   namespace.

2. Clean termination: Termination of the leading process in a pid
   namespace will result in the kernel terminating all other processes
   in the namespace.

The Launcher will use (2) during destruction of a container in
preference to the freezer cgroup, avoiding known kernel issues related
to freezing cgroups under OOM conditions.

/proc will be mounted for containers so tools such as 'ps' will work
correctly.


### Posix Disk Isolator

The Posix Disk isolator provides basic disk isolation. It is able to
report the disk usage for each sandbox and optionally enforce the disk
quota. It can be used on both Linux and OS X.

To enable the Posix Disk isolator, append `disk/du` to the `--isolation`
flag when starting the agent.

By default, the disk quota enforcement is disabled. To enable it,
specify `--enforce_container_disk_quota` when starting the agent.

The Posix Disk isolator reports disk usage for each sandbox by
periodically running the `du` command. The disk usage can be retrieved
from the resource statistics endpoint ([/monitor/statistics](endpoints/slave/monitor/statistics.md)).

The interval between two `du`s can be controlled by the agent flag
`--container_disk_watch_interval`. For example,
`--container_disk_watch_interval=1mins` sets the interval to be 1
minute. The default interval is 15 seconds.


### XFS Disk Isolator

The XFS Disk isolator uses XFS project quotas to track the disk
space used by each container sandbox and to enforce the corresponding
disk space allocation. Write operations performed by tasks exceeding
their disk allocation will fail with an `EDQUOT` error. The task
will not be terminated by the containerizer.

The XFS disk isolator is functionally similar to Posix Disk isolator
but avoids the cost of repeatedly running the `du`.  Though they will
not interfere with each other, it is not recommended to use them together.

To enable the XFS Disk isolator, append `disk/xfs` to the `--isolation`
flag when starting the agent.

The XFS Disk isolator requires the sandbox directory to be located
on an XFS filesystem that is mounted with the `pquota` option. There
is no need to configure
[projects](http://man7.org/linux/man-pages/man5/projects.5.html)
or [projid](http://man7.org/linux/man-pages/man5/projid.5.html)
files. The range of project IDs given to the `--xfs_project_range`
must not overlap any project IDs allocated for other uses.

The XFS disk isolator does not natively support an accounting-only mode
like that of the Posix Disk isolator. Quota enforcement can be disabled
by mounting the filesystem with the `pqnoenforce` mount option.

The [xfs_quota](http://man7.org/linux/man-pages/man8/xfs_quota.8.html)
command can be used to show the current allocation of project IDs
and quota. For example:

    $ xfs_quota -x -c "report -a -n -L 5000 -U 1000"

To show which project a file belongs to, use the
[xfs_io](http://man7.org/linux/man-pages/man8/xfs_io.8.html) command
to display the `fsxattr.projid` field. For example:

    $ xfs_io -r -c stat /mnt/mesos/

Note that the Posix Disk isolator flags `--enforce_container_disk_quota`,
`--container_disk_watch_interval` and `--enforce_container_disk_quota` do
not apply to the XFS Disk isolator.


### Docker Runtime Isolator

The Docker Runtime isolator is used for supporting runtime
configurations from the docker image (e.g., Entrypoint/Cmd, Env,
etc.). This isolator is tied with `--image_providers=docker`. If
`--image_providers` contains `docker`, this isolator must be used.
Otherwise, the agent will refuse to start.

To enable the Docker Runtime isolator, append `docker/runtime` to the
`--isolation` flag when starting the agent.

Currently, docker image default `Entrypoint`, `Cmd`, `Env`, and `WorkingDir` are
supported with docker runtime isolator. Users can specify `CommandInfo` to
override the default `Entrypoint` and `Cmd` in the image (see below for
details). The `CommandInfo` should be inside of either `TaskInfo` or
`ExecutorInfo` (depending on whether the task is a command task or uses a custom
executor, respectively).

#### Determine the Launch Command

If the user specifies a command in `CommandInfo`, that will override the
default Entrypoint/Cmd in the docker image. Otherwise, we will use the
default Entrypoint/Cmd and append arguments specified in `CommandInfo`
accordingly. The details are explained in the following table.

Users can specify `CommandInfo` including `shell`, `value` and
`arguments`, which are represented in the first column of the table
below. `0` represents `not specified`, while `1` represents
`specified`. The first row is how `Entrypoint` and `Cmd` defined in
the docker image. All cells in the table, except the first column and
row, as well as cells labeled as `Error`, have the first element
(i.e., `/Entrypt[0]`) as executable, and the rest as appending
arguments.

<table class="table table-striped">
  <tr>
    <th></th>
    <th>Entrypoint=0<br>Cmd=0</th>
    <th>Entrypoint=0<br>Cmd=1</th>
    <th>Entrypoint=1<br>Cmd=0</th>
    <th>Entrypoint=1<br>Cmd=1</th>
  </tr>
  <tr>
    <td>sh=0<br>value=0<br>argv=0</td>
    <td>Error</td>
    <td>/Cmd[0]<br>Cmd[1]..</td>
    <td>/Entrypt[0]<br>Entrypt[1]..</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>Cmd..</td>
  </tr>
  <tr>
    <td>sh=0<br>value=0<br>argv=1</td>
    <td>Error</td>
    <td>/Cmd[0]<br>argv</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>argv</td>
    <td>/Entrypt[0]<br>Entrypt[1]..<br>argv</td>
  </tr>
  <tr>
    <td>sh=0<br>value=1<br>argv=0</td>
    <td>/value</td>
    <td>/value</td>
    <td>/value</td>
    <td>/value</td>
  </tr>
  <tr>
    <td>sh=0<br>value=1<br>argv=1</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
    <td>/value<br>argv</td>
  </tr>
  <tr>
    <td>sh=1<br>value=0<br>argv=0</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
  </tr>
  <tr>
    <td>sh=1<br>value=0<br>argv=1</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
    <td>Error</td>
  </tr>
  <tr>
    <td>sh=1<br>value=1<br>argv=0</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
  </tr>
  <tr>
    <td>sh=1<br>value=1<br>argv=1</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
    <td>/bin/sh -c<br>value</td>
  </tr>
</table>


### The `cgroups/net_cls` Isolator

The cgroups/net_cls isolator allows operators to provide network
performance isolation and network segmentation for containers within
a Mesos cluster. To enable the cgroups/net_cls isolator, append
`cgroups/net_cls` to the `--isolation` flag when starting the agent.

As the name suggests, the isolator enables the net_cls subsystem for
Linux cgroups and assigns a net_cls cgroup to each container launched
by the `MesosContainerizer`.  The objective of the net_cls subsystem
is to allow the kernel to tag packets originating from a container
with a 32-bit handle. These handles can be used by kernel modules such
as `qdisc` (for traffic engineering) and `net-filter` (for
firewall) to enforce network performance and security policies
specified by the operators.  The policies, based on the net_cls
handles, can be specified by the operators through user-space tools
such as
[tc](http://tldp.org/HOWTO/Traffic-Control-HOWTO/software.html#s-iproute2-tc)
and [iptables](http://linux.die.net/man/8/iptables).

The 32-bit handle associated with a net_cls cgroup can be specified by
writing the handle to the `net_cls.classid` file, present within the
net_cls cgroup. The 32-bit handle is of the form `0xAAAABBBB`, and
consists of a 16-bit primary handle 0xAAAA and a 16-bit secondary
handle 0xBBBB. You can read more about the use cases for the primary
and secondary handles in the [Linux kernel documentation for
net_cls](https://www.kernel.org/doc/Documentation/cgroup-v1/net_cls.txt).

By default, the cgroups/net_cls isolator does not manage the net_cls
handles, and assumes the operator is going to manage/assign these
handles. To enable the management of net_cls handles by the
cgroups/net_cls isolator you need to specify a 16-bit primary handle,
of the form 0xAAAA, using the `--cgroups_net_cls_primary_handle` flag at
agent startup.

Once a primary handle has been specified for an agent, for each
container the cgroups/net_cls isolator allocates a 16-bit secondary
handle. It then assigns the 32-bit combination of the primary and
secondary handle to the net_cls cgroup associated with the container
by writing to `net_cls.classid`. The cgroups/net_cls isolator exposes
the assigned net_cls handle to operators by exposing the handle as
part of the `ContainerStatus` &mdash;associated with any task running within
the container&mdash; in the agent's [/state](endpoints/slave/state.md) endpoint.


### The `docker/volume` Isolator

This is described in a [separate document](docker-volume.md).


### The `namespaces/ipc` Isolator

The IPC Namespace isolator can be used on Linux to place tasks
in a distinct IPC namespace. The benefit of this is that any
[IPC objects](http://man7.org/linux/man-pages/man7/svipc.7.html) created
in the container will be automatically removed when the container is
destroyed.


### The `network/cni` Isolator

This is described in a [separate document](cni.md).


### The `linux/capabilities` Isolator

This is described in a [separate document](linux_capabilities.md).


### The `posix/rlimits` Isolator

This is described in a [separate document](posix_rlimits.md).


### The `volume/sandbox_path` Isolator

The volume/sandbox_path isolator allows containers in a task group to
share a temporary volume from their parent's sandbox. To specify a
shared volume for nested containers, users have to enable this isolator
and define `Volume::Source::SandboxPath`:

    message Volume {
      ...
      required string container_path = 1;
      ...

      message Source {
        enum Type {
          ...
          SANDBOX_PATH = 2;
        }

        ...

        message SandboxPath {
          enum Type {
            UNKNOWN = 0;
            SELF = 1;
            PARENT = 2;
          }

          optional Type type = 1;
          required string path = 2;
        }

        ...

        optional SandboxPath sandbox_path = 3;
      }

      optional Source source = 5;
    }

Currently only `PARENT` sandbox path is supported. If the current
container is a top level container, the isolator will return an error.
Please note that `SandboxPath::path` describes a relative path
to the corresponding container's sandbox. Upward traversal
(e.g., ../../abc) is not supported.


## Isolator Nested Aware

Starting from Mesos 1.1.0, [nested container](nested-container-and-task-group.md) is supported.
A nested aware isolator means that an isolator supports nested
containers. Depending on the funtionality of an isolator, the
complexity of make an isolator nested aware is different. In
this section, we will firstly introduce the current semantics
of isolators in Mesos. Then, there is a guidance on how to
make a custom isolator module nested aware.

### Current Semantics

To support nested containers, most of the current isolators in
Mesos are changed to be nested aware. Please see the following
table for current isolators semantics:

<table class="table table-striped">
  <tr>
    <th>Isolator</th>
    <th>Nested Aware</th>
    <th>Isolator Semantics</th>
  </tr>
  <tr>
    <td>filesystem/posix</td>
    <td>no</td>
    <td></td>
  </tr>
  <tr>
    <td>filesystem/linux</td>
    <td>yes</td>
    <td>The nested container can either has its own mount namespace or share the mount namespace from its parent container.</td>
  </tr>
  <tr>
    <td>disk/du</td>
    <td>yes</td>
    <td>The disk space check is only supported for the top level container. All nested containers do not have separate disk check yet. The disk check and limitation reporting should be supported for nested containers in the future.</td>
  </tr>
  <tr>
    <td>posix/rlimit</td>
    <td>no</td>
    <td></td>
  </tr>
  <tr>
    <td>volume/sandbox_path</td>
    <td>yes</td>
    <td>Volume sharing for nested containers.</td>
  </tr>
  <tr>
    <td>disk/xfs</td>
    <td>no</td>
    <td></td>
  </tr>
  <tr>
    <td>cgroups</td>
    <td>yes</td>
    <td>The cgroup isolator only supports creating cgroups for top level containers. All nested containers share the cgroup resources from its top level executor container. The nested container should have its own cgroup. This is the post-MVP goal.</td>
  </tr>
  <tr>
    <td>appc/runtime</td>
    <td>yes</td>
    <td>Supports runtime isolation for nested containers with an appc image specified.</td>
  </tr>
  <tr>
    <td>docker/runtime</td>
    <td>yes</td>
    <td>Supports runtime isolation for nested containers with an docker image specified.</td>
  </tr>
  <tr>
    <td>docker/volume</td>
    <td>no</td>
    <td>(coming soon).</td>
  </tr>
  <tr>
    <td>linux/capabilities</td>
    <td>no</td>
    <td></td>
  </tr>
  <tr>
    <td>volume/image</td>
    <td>yes</td>
    <td>Supports image volumes for nested containers.</td>
  </tr>
  <tr>
    <td>volume/sandbox_path</td>
    <td>yes</td>
    <td>Supports shared volumes for nested containers.</td>
  </tr>
  <tr>
    <td>gpu/nvidia</td>
    <td>yes</td>
    <td>GPUs are only allocated for the top level container. All nested containers share the GPUs from it top level container. The necessary Nvidia libraries are still mounted into the nested container.</td>
  </tr>
  <tr>
    <td>namespace/pid</td>
    <td>yes</td>
    <td>Each nested container has its own PID namespace, which is nested under its parent container’s PID namespace. The structure is nested hierarchy in Kernel.</td>
  </tr>
  <tr>
    <td>network/cni</td>
    <td>yes</td>
    <td>A nested container always shares it parent’s network and UTS namespace.</td>
  </tr>
  <tr>
    <td>network/port_mapping</td>
    <td>no</td>
    <td></td>
  </tr>
</table>

### How to Make a Custom Isolator Module Nested Aware

To support nested containers, the API for isolator interface is
not necessary to be changed. The containerizer does not pass any
nested containers to the isolator methods if the isolator is not
nested aware.

1. To mark an isolator as nested aware, a new virtual function
   `supportsNesting()` is introduced to the isolator interface:

        // Returns true if this isolator supports nested containers. This
        // method is designed to allow isolators to opt-in to support nested
        // containers.
        virtual bool supportsNesting()
        {
          return false;
        }

   A nested aware isolator should overwrite this virtual function
   and returns `true`, which tells the containerizer that it
   supports nested containers.

2. For isolator recover(), if the isolator does not support nesting,
   only top level containers will be passed to the isolator. If the
   isolator is nested aware, both top level containers and nested
   containers will be passed to the isolator in the list of
   `ContainerState` and the hashset of orphaned `ContainerID`. Please
   note that the order of `ContainerState` is a result of pre-order
   traversal (i.e., parent is inserted before its children).

3. The following isolator methods will be skipped for nested
   containers if the isolator does not support nesting:

   * prepare()
   * watch()
   * isolate()
   * status()
   * cleanup()

4. Please note that new protobuf fields are added to `ContainerConfig`,
   which is a parameter for the isolator prepare() interface. No matter
   the isolator is preparing a top level container or a nested
   container, the following information should be already set properly
   in `ContainerConfig`:

   * CommandInfo
   * ContainerInfo
   * Resource

## Nested Container Sandbox Layout

The nested container’s sandbox will be nested under its parent
container’s sandbox in filesystem hierarchy.

    .../executors/<executorId>/runs/<containerId>/
          |--- stdout
          |--- stderr
          |--- volume/
          |--- containers/
                 |--- <containerId>/
                         |--- stdout
                         |--- stderr
                         |--- volume/
                         |--- containers/
                                 |--- ...
                 |--- ...
