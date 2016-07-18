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

#include <glog/logging.h>

#include <iostream>
#include <string>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/time.hpp>

#include <process/metrics/counter.hpp>
#include <process/metrics/gauge.hpp>
#include <process/metrics/metrics.hpp>

#include <stout/bytes.hpp>
#include <stout/flags.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

using namespace mesos;
using namespace mesos::internal;

using std::string;

using process::Clock;
using process::defer;

using process::metrics::Gauge;
using process::metrics::Counter;

const double CPUS_PER_TASK = 1;
const double MEM_PER_TASK = 128;

class Flags : public flags::FlagsBase
{
public:
  Flags()
  {
    add(&master,
        "master",
        "Master to connect to.");

    add(&checkpoint,
        "checkpoint",
        "Whether this framework should be checkpointed.\n",
        false);

    add(&long_running,
        "long_running",
        "Whether this framework should launch tasks repeatedly\n"
        "or exit after finishing a single task.",
        false);
  }

  string master;
  bool checkpoint;
  bool long_running;
};


// Actor holding the business logic and metrics for the
// `UnifiedContainerizerScheduler`. See `UnifiedContainerizerScheduler`
// below for the intended behavior.
class UnifiedContainerizerSchedulerProcess
  : public process::Process<UnifiedContainerizerSchedulerProcess>
{
public:
  UnifiedContainerizerSchedulerProcess(
      const Flags& _flags)
    : flags(_flags),
      taskActive(false),
      tasksLaunched(0),
      isRegistered(false),
      metrics(*this)
  {
    start_time = Clock::now();
  }

  void registered()
  {
    isRegistered = true;
  }

  void disconnected()
  {
    isRegistered = false;
  }

  void resourceOffers(
      SchedulerDriver* driver,
      const std::vector<Offer>& offers)
  {
    static const Resources TASK_RESOURCES = Resources::parse(
        "cpus:" + stringify(CPUS_PER_TASK) +
        ";mem:" + stringify(MEM_PER_TASK)).get();

    foreach (const Offer& offer, offers) {
      Resources resources(offer.resources());

      // If there is an active task, or if the offer is not
      // big enough, reject the offer.
      if (taskActive || !resources.flatten().contains(TASK_RESOURCES)) {
        Filters filters;
        filters.set_refuse_seconds(600);

        driver->declineOffer(offer.id(), filters);
        continue;
      }

      int taskId = tasksLaunched++;

      LOG(INFO) << "Starting task " << taskId;

      TaskInfo task;
      task.set_name("Unified Containerizer Task " + stringify(taskId));
      task.mutable_task_id()->set_value(stringify(taskId));
      task.mutable_slave_id()->MergeFrom(offer.slave_id());
      task.mutable_resources()->CopyFrom(TASK_RESOURCES);

      CommandInfo* command = task.mutable_command();
      command->set_shell(false);

      ContainerInfo* container = task.mutable_container();
      container->set_type(ContainerInfo::MESOS);

      Image* image = container->mutable_mesos()->mutable_image();
      image->set_type(Image::DOCKER);
      image->mutable_docker()->set_name("mesosphere/inky");

      driver->launchTasks(offer.id(), {task});

      taskActive = true;
    }
  }

  void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    if (!flags.long_running) {
      if (status.state() == TASK_FAILED &&
          status.reason() == TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY) {
        // NOTE: We expect TASK_FAILED when this scheduler is launched by the
        // UnifiedContainerizer_framework_test.sh shell script. The abort here
        // ensures the script considers the test result as "PASS".
        driver->abort();
      } else if (status.state() == TASK_FAILED ||
          status.state() == TASK_FINISHED ||
          status.state() == TASK_KILLED ||
          status.state() == TASK_LOST ||
          status.state() == TASK_ERROR) {
        driver->stop();
      }
    }

    if (stringify(tasksLaunched - 1) != status.task_id().value()) {
      // We might receive messages from older tasks. Ignore them.
      LOG(INFO) << "Ignoring status update from older task "
                << status.task_id();
      return;
    }

    switch (status.state()) {
      case TASK_FINISHED:
        taskActive = false;
        ++metrics.tasks_finished;
        break;
      case TASK_FAILED:
        taskActive = false;
        if (status.reason() == TaskStatus::REASON_CONTAINER_LIMITATION_MEMORY) {
          ++metrics.tasks_oomed;
          break;
        }

        // NOTE: Fetching the executor (e.g. `--executor_uri`) may fail
        // occasionally if the URI is rate limited. This case is common
        // enough that it makes sense to track this failure metric separately.
        if (status.reason() == TaskStatus::REASON_CONTAINER_LAUNCH_FAILED) {
          ++metrics.launch_failures;
          break;
        }
      case TASK_KILLED:
      case TASK_LOST:
      case TASK_ERROR:
        taskActive = false;

        ++metrics.abnormal_terminations;
        break;
      default:
        break;
    }
  }

private:
  const Flags flags;
  bool taskActive;
  int tasksLaunched;

  process::Time start_time;
  double _uptime_secs()
  {
    return (Clock::now() - start_time).secs();
  }

  bool isRegistered;
  double _registered()
  {
    return isRegistered ? 1 : 0;
  }

  struct Metrics
  {
    Metrics(const UnifiedContainerizerSchedulerProcess& _scheduler)
      : uptime_secs(
            "UnifiedContainerizer_framework/uptime_secs",
            defer(_scheduler,
                  &UnifiedContainerizerSchedulerProcess::_uptime_secs)),
        registered(
            "UnifiedContainerizer_framework/registered",
            defer(_scheduler,
                  &UnifiedContainerizerSchedulerProcess::_registered)),
        tasks_finished("UnifiedContainerizer_framework/tasks_finished"),
        tasks_oomed("UnifiedContainerizer_framework/tasks_oomed"),
        launch_failures("UnifiedContainerizer_framework/launch_failures"),
        abnormal_terminations(
            "UnifiedContainerizer_framework/abnormal_terminations")
    {
      process::metrics::add(uptime_secs);
      process::metrics::add(registered);
      process::metrics::add(tasks_finished);
      process::metrics::add(tasks_oomed);
      process::metrics::add(launch_failures);
      process::metrics::add(abnormal_terminations);
    }

    ~Metrics()
    {
      process::metrics::remove(uptime_secs);
      process::metrics::remove(registered);
      process::metrics::remove(tasks_finished);
      process::metrics::remove(tasks_oomed);
      process::metrics::remove(launch_failures);
      process::metrics::remove(abnormal_terminations);
    }

    process::metrics::Gauge uptime_secs;
    process::metrics::Gauge registered;

    process::metrics::Counter tasks_finished;
    process::metrics::Counter tasks_oomed;
    process::metrics::Counter launch_failures;
    process::metrics::Counter abnormal_terminations;
  } metrics;
};


class UnifiedContainerizerScheduler : public Scheduler
{
public:
  UnifiedContainerizerScheduler(
      const Flags& _flags)
    : process(_flags)
  {
    process::spawn(process);
  }

  virtual ~UnifiedContainerizerScheduler()
  {
    process::terminate(process);
    process::wait(process);
  }

  virtual void registered(
      SchedulerDriver*,
      const FrameworkID& frameworkId,
      const MasterInfo&)
  {
    LOG(INFO) << "Registered with framework ID: " << frameworkId;

    process::dispatch(
        &process,
        &UnifiedContainerizerSchedulerProcess::registered);
  }

  virtual void reregistered(SchedulerDriver*, const MasterInfo& masterInfo)
  {
    LOG(INFO) << "Reregistered";

    process::dispatch(
        &process,
        &UnifiedContainerizerSchedulerProcess::registered);
  }

  virtual void disconnected(SchedulerDriver* driver)
  {
    LOG(INFO) << "Disconnected";

    process::dispatch(
        &process,
        &UnifiedContainerizerSchedulerProcess::disconnected);
  }

  virtual void resourceOffers(
      SchedulerDriver* driver,
      const std::vector<Offer>& offers)
  {
    LOG(INFO) << "Resource offers received";

    process::dispatch(
        &process,
        &UnifiedContainerizerSchedulerProcess::resourceOffers,
        driver,
        offers);
  }

  virtual void offerRescinded(
      SchedulerDriver* driver,
      const OfferID& offerId)
  {
    LOG(INFO) << "Offer rescinded";
  }

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {
    LOG(INFO)
      << "Task " << status.task_id() << " in state "
      << TaskState_Name(status.state())
      << ", Source: " << status.source()
      << ", Reason: " << status.reason()
      << (status.has_message() ? ", Message: " + status.message() : "");

    process::dispatch(
        &process,
        &UnifiedContainerizerSchedulerProcess::statusUpdate,
        driver,
        status);
  }

  virtual void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data)
  {
    LOG(INFO) << "Framework message: " << data;
  }

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& slaveId)
  {
    LOG(INFO) << "Agent lost: " << slaveId;
  }

  virtual void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      int status)
  {
    LOG(INFO) << "Executor '" << executorId << "' lost on agent: " << slaveId;
  }

  virtual void error(SchedulerDriver* driver, const string& message)
  {
    LOG(INFO) << "Error message: " << message;
  }

private:
  UnifiedContainerizerSchedulerProcess process;
};


int main(int argc, char** argv)
{
  Flags flags;
  Try<flags::Warnings> load = flags.load("MESOS_", argc, argv);

  if (load.isError()) {
    EXIT(EXIT_FAILURE) << flags.usage(load.error());
  }

  UnifiedContainerizerScheduler scheduler(flags);

  // Log any flag warnings (after logging is initialized by the scheduler).
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  FrameworkInfo framework;
  framework.set_user(os::user().get());
  framework.set_name("UnifiedContainerizer Framework (C++)");
  framework.set_checkpoint(flags.checkpoint);

  MesosSchedulerDriver* driver;

  // TODO(josephw): Refactor these into a common set of flags.
  Option<string> value = os::getenv("MESOS_AUTHENTICATE_FRAMEWORKS");
  if (value.isSome()) {
    LOG(INFO) << "Enabling authentication for the framework";

    value = os::getenv("DEFAULT_PRINCIPAL");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting authentication principal in the environment";
    }

    Credential credential;
    credential.set_principal(value.get());

    framework.set_principal(value.get());

    value = os::getenv("DEFAULT_SECRET");
    if (value.isNone()) {
      EXIT(EXIT_FAILURE)
        << "Expecting authentication secret in the environment";
    }

    credential.set_secret(value.get());

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master, credential);
  } else {
    framework.set_principal("UnifiedContainerizer-framework-cpp");

    driver = new MesosSchedulerDriver(
        &scheduler, framework, flags.master);
  }

  int status = driver->run() == DRIVER_STOPPED ? 0 : 1;

  // Ensure that the driver process terminates.
  driver->stop();

  delete driver;
  return status;
}
