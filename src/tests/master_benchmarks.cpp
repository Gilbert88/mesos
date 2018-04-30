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
#include <tuple>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/version.hpp>

#include <process/clock.hpp>
#include <process/collect.hpp>
#include <process/future.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include <stout/jsonify.hpp>
#include <stout/stopwatch.hpp>

#include "common/protobuf_utils.hpp"

#include "tests/mesos.hpp"

namespace http = process::http;

const double CPUS_PER_TASK = .1;
const int32_t MEM_PER_TASK = 16;

using mesos::allocator::Allocator;

using mesos::internal::slave::AGENT_CAPABILITIES;

using process::await;
using process::Clock;
using process::Failure;
using process::Future;
using process::Owned;
using process::PID;
using process::ProcessBase;
using process::Promise;
using process::spawn;
using process::terminate;
using process::UPID;

using std::cout;
using std::endl;
using std::make_tuple;
using std::string;
using std::tie;
using std::tuple;
using std::vector;

using testing::WithParamInterface;

namespace mesos {
namespace internal {
namespace tests {

static SlaveInfo createSlaveInfo(const SlaveID& slaveId)
{
  // Using a static local variable to avoid the cost of re-parsing.
  static const Resources resources =
    Resources::parse("cpus:20;mem:10240").get();

  SlaveInfo slaveInfo;
  *(slaveInfo.mutable_resources()) = resources;
  *(slaveInfo.mutable_id()) = slaveId;
  *(slaveInfo.mutable_hostname()) = slaveId.value(); // Simulate the hostname.

  return slaveInfo;
}


static FrameworkInfo createFrameworkInfo(const FrameworkID& frameworkId)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  *(frameworkInfo.mutable_id()) = frameworkId;

  return frameworkInfo;
}


static FrameworkID createFrameworkId(size_t n)
{
  FrameworkID frameworkId;

  frameworkId.set_value("framework-" + stringify(n));

  return frameworkId;
}


static FrameworkInfo createFrameworkInfo(
    size_t n,
    const std::set<std::string>& roles,
    const vector<FrameworkInfo::Capability::Type>& capabilities = {})
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  frameworkInfo.mutable_id()->CopyFrom(createFrameworkId(n));

  frameworkInfo.clear_roles();
  foreach(const string& role, roles) {
    frameworkInfo.add_roles(role);
  }

  return frameworkInfo;
}


static TaskInfo createTaskInfo(const SlaveID& slaveId)
{
  // Using a static local variable to avoid the cost of re-parsing.
  static const Resources resources = Resources::parse("cpus:0.1;mem:5").get();

  TaskInfo taskInfo = createTask(
      slaveId,
      resources,
      "dummy command");

  Labels* labels = taskInfo.mutable_labels();

  for (size_t i = 0; i < 10; i++) {
    const string index = stringify(i);
    *(labels->add_labels()) =
      protobuf::createLabel("key" + index, "value" + index);
  }

  return taskInfo;
}


// A fake agent currently just for testing reregisterations.
class TestSlaveProcess : public ProtobufProcess<TestSlaveProcess>
{
public:
  TestSlaveProcess(
      const UPID& _masterPid,
      const SlaveID& _slaveId,
      size_t _frameworksPerAgent,
      size_t _tasksPerFramework,
      size_t _completedFrameworksPerAgent,
      size_t _tasksPerCompletedFramework)
    : ProcessBase(process::ID::generate("test-slave")),
      masterPid(_masterPid),
      slaveId(_slaveId),
      frameworksPerAgent(_frameworksPerAgent),
      tasksPerFramework(_tasksPerFramework),
      completedFrameworksPerAgent(_completedFrameworksPerAgent),
      tasksPerCompletedFramework(_tasksPerCompletedFramework) {}

  void initialize() override
  {
    install<SlaveReregisteredMessage>(&Self::reregistered);
    install<PingSlaveMessage>(
        &Self::ping,
        &PingSlaveMessage::connected);

    // Prepare `ReregisterSlaveMessage` which simulates the real world scenario:
    // TODO(xujyan): Notable things missing include:
    // - `ExecutorInfo`s
    // - Task statuses
    SlaveInfo slaveInfo = createSlaveInfo(slaveId);
    message.mutable_slave()->Swap(&slaveInfo);
    message.set_version(MESOS_VERSION);

    // Used for generating framework IDs.
    size_t id = 0;
    for (; id < frameworksPerAgent; id++) {
      FrameworkID frameworkId;
      frameworkId.set_value("framework" + stringify(id));

      FrameworkInfo framework = createFrameworkInfo(frameworkId);
      message.add_frameworks()->Swap(&framework);

      for (size_t j = 0; j < tasksPerFramework; j++) {
        Task task = protobuf::createTask(
            createTaskInfo(slaveId),
            TASK_RUNNING,
            frameworkId);
        message.add_tasks()->Swap(&task);
      }
    }

    for (; id < frameworksPerAgent + completedFrameworksPerAgent; id++) {
      Archive::Framework* completedFramework =
        message.add_completed_frameworks();

      FrameworkID frameworkId;
      frameworkId.set_value("framework" + stringify(id));

      FrameworkInfo framework = createFrameworkInfo(frameworkId);
      completedFramework->mutable_framework_info()->Swap(&framework);

      for (size_t j = 0; j < tasksPerCompletedFramework; j++) {
        Task task = protobuf::createTask(
            createTaskInfo(slaveId),
            TASK_FINISHED,
            frameworkId);
        completedFramework->add_tasks()->Swap(&task);
      }
    }
  }

  Future<Nothing> reregister()
  {
    send(masterPid, message);
    return promise.future();
  }

  TestSlaveProcess(const TestSlaveProcess& other) = delete;
  TestSlaveProcess& operator=(const TestSlaveProcess& other) = delete;

private:
  void reregistered(const SlaveReregisteredMessage&)
  {
    promise.set(Nothing());
  }

  // We need to answer pings to keep the agent registered.
  void ping(const UPID& from, bool)
  {
    send(from, PongSlaveMessage());
  }

  const UPID masterPid;
  const SlaveID slaveId;
  const size_t frameworksPerAgent;
  const size_t tasksPerFramework;
  const size_t completedFrameworksPerAgent;
  const size_t tasksPerCompletedFramework;

  ReregisterSlaveMessage message;
  Promise<Nothing> promise;
};


class TestSlave
{
public:
  TestSlave(
      const UPID& masterPid,
      const SlaveID& slaveId,
      size_t frameworksPerAgent,
      size_t tasksPerFramework,
      size_t completedFrameworksPerAgent,
      size_t tasksPerCompletedFramework)
    : process(new TestSlaveProcess(
          masterPid,
          slaveId,
          frameworksPerAgent,
          tasksPerFramework,
          completedFrameworksPerAgent,
          tasksPerCompletedFramework))
  {
    spawn(process.get());
  }

  ~TestSlave()
  {
    terminate(process.get());
    process::wait(process.get());
  }

  Future<Nothing> reregister()
  {
    return dispatch(process.get(), &TestSlaveProcess::reregister);
  }

private:
  Owned<TestSlaveProcess> process;
};


class MasterFailover_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<size_t, size_t, size_t, size_t, size_t>> {};


// The value tuples are defined as:
// - agentCount
// - frameworksPerAgent
// - tasksPerFramework (per agent)
// - completedFrameworksPerAgent
// - tasksPerCompletedFramework (per agent)
INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCount,
    MasterFailover_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(2000, 5, 10, 5, 10),
        make_tuple(2000, 5, 20, 0, 0),
        make_tuple(20000, 1, 5, 0, 0)));


// This test measures the time from all agents start to reregister to
// to when all have received `SlaveReregisteredMessage`.
TEST_P(MasterFailover_BENCHMARK_Test, AgentReregistrationDelay)
{
  size_t agentCount;
  size_t frameworksPerAgent;
  size_t tasksPerFramework;
  size_t completedFrameworksPerAgent;
  size_t tasksPerCompletedFramework;

  tie(agentCount,
      frameworksPerAgent,
      tasksPerFramework,
      completedFrameworksPerAgent,
      tasksPerCompletedFramework) = GetParam();

  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  // Use replicated log so it better simulates the production scenario.
  masterFlags.registry = "replicated_log";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  vector<TestSlave> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.emplace_back(
        master.get()->pid,
        slaveId,
        frameworksPerAgent,
        tasksPerFramework,
        completedFrameworksPerAgent,
        tasksPerCompletedFramework);
  }

  // Make sure all agents are ready to reregister before we start the stopwatch.
  Clock::pause();
  Clock::settle();
  Clock::resume();

  vector<Future<Nothing>> reregistered;

  // Measure the time for all agents to receive `SlaveReregisteredMessage`.
  Stopwatch watch;
  watch.start();

  foreach (TestSlave& slave, slaves) {
    reregistered.push_back(slave.reregister());
  }

  await(reregistered).await();

  watch.stop();

  cout << "Reregistered " << agentCount << " agents with a total of "
       << frameworksPerAgent * tasksPerFramework * agentCount
       << " running tasks and "
       << completedFrameworksPerAgent * tasksPerCompletedFramework * agentCount
       << " completed tasks in "
       << watch.elapsed() << endl;
}


class MasterStateQuery_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<
      size_t, size_t, size_t, size_t, size_t>> {};


INSTANTIATE_TEST_CASE_P(
    AgentFrameworkTaskCountContentType,
    MasterStateQuery_BENCHMARK_Test,
    ::testing::Values(
        make_tuple(1000, 5, 2, 5, 2),
        make_tuple(10000, 5, 2, 5, 2),
        make_tuple(20000, 5, 2, 5, 2),
        make_tuple(40000, 5, 2, 5, 2)));


// This test measures the performance of the `master::call::GetState`
// v1 api (and also measures master v0 '/state' endpoint as the
// baseline). We set up a lot of master state from artificial agents
// similar to the master failover benchmark.
TEST_P(MasterStateQuery_BENCHMARK_Test, GetState)
{
  size_t agentCount;
  size_t frameworksPerAgent;
  size_t tasksPerFramework;
  size_t completedFrameworksPerAgent;
  size_t tasksPerCompletedFramework;

  tie(agentCount,
    frameworksPerAgent,
    tasksPerFramework,
    completedFrameworksPerAgent,
    tasksPerCompletedFramework) = GetParam();

  // Disable authentication to avoid the overhead, since we don't care about
  // it in this test.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  vector<Owned<TestSlave>> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    SlaveID slaveId;
    slaveId.set_value("agent" + stringify(i));

    slaves.push_back(Owned<TestSlave>(new TestSlave(
        master.get()->pid,
        slaveId,
        frameworksPerAgent,
        tasksPerFramework,
        completedFrameworksPerAgent,
        tasksPerCompletedFramework)));
  }

  cout << "Test setup: "
       << agentCount << " agents with a total of "
       << frameworksPerAgent * tasksPerFramework * agentCount
       << " running tasks and "
       << completedFrameworksPerAgent * tasksPerCompletedFramework * agentCount
       << " completed tasks" << endl;

  vector<Future<Nothing>> reregistered;

  foreach (const Owned<TestSlave>& slave, slaves) {
    reregistered.push_back(slave->reregister());
  }

  // Wait all agents to finish reregistration.
  await(reregistered).await();

  Clock::pause();
  Clock::settle();
  Clock::resume();

  Stopwatch watch;
  watch.start();

  // We first measure v0 "state" endpoint performance as the baseline.
  Future<http::Response> v0Response = http::get(
      master.get()->pid,
      "state",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  v0Response.await();

  watch.stop();

  ASSERT_EQ(v0Response->status, http::OK().status);

  cout << "v0 '/state' response took " << watch.elapsed() << endl;

  // Helper function to post a request to '/api/v1' master endpoint
  // and return the response.
  auto post = [](
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  };

  // We measure both JSON and protobuf formats.
  const ContentType contentTypes[] =
    { ContentType::PROTOBUF, ContentType::JSON };

  for (ContentType contentType : contentTypes){
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_STATE);

    watch.start();

    Future<http::Response> response =
      post(master.get()->pid, v1Call, contentType);

    response.await();

    watch.stop();

    ASSERT_EQ(response->status, http::OK().status);

    Future<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    ASSERT_TRUE(v1Response->IsInitialized());
    EXPECT_EQ(v1::master::Response::GET_STATE, v1Response->type());

    cout << "v1 'master::call::GetState' "
         << contentType << " response took " << watch.elapsed() << endl;
  }
}


template <typename T>
T getEnv(const string& env, T defaultValue)
{
  Option<string> val = os::getenv(env);
  if (val.isNone()) {
    return defaultValue;
  }

  return numify<T>(val.get()).get();
}


// A fake agent currently just for testing registrations.
class TestSlaveProcess2 : public ProtobufProcess<TestSlaveProcess2>
{
public:
  TestSlaveProcess2(const UPID& _masterPid, const string& _hostname)
    : ProcessBase(process::ID::generate("test-slave")),
      masterPid(_masterPid),
      hostname(_hostname) {}

  TestSlaveProcess2(const TestSlaveProcess2& other) = delete;

  TestSlaveProcess2& operator=(const TestSlaveProcess2& other) = delete;

  void initialize() override
  {
    install<SlaveRegisteredMessage>(&Self::registeredAgent);
    install<PingSlaveMessage>(
        &Self::ping,
        &PingSlaveMessage::connected);

    SlaveID slaveId;
    slaveId.set_value(hostname);

    SlaveInfo slaveInfo = createSlaveInfo(slaveId);
    message.mutable_slave()->Swap(&slaveInfo);
    message.set_version(MESOS_VERSION);
    message.mutable_agent_capabilities()->CopyFrom(
        protobuf::slave::Capabilities(
            mesos::internal::slave::AGENT_CAPABILITIES()).toRepeatedPtrField());
  }

  Future<Nothing> registerAgent()
  {
    send(masterPid, message);
    return promise.future();
  }

private:
  void registeredAgent(const SlaveRegisteredMessage&)
  {
    promise.set(Nothing());
  }

  // We need to answer pings to keep the agent registered.
  void ping(const UPID& from, bool)
  {
    send(from, PongSlaveMessage());
  }

  const UPID masterPid;
  const string hostname;

  RegisterSlaveMessage message;
  Promise<Nothing> promise;
};


class TestSlave2
{
public:
  TestSlave2(const UPID& masterPid, const string& hostname)
    : process(new TestSlaveProcess2(masterPid, hostname))
  {
    spawn(process.get());
  }

  ~TestSlave2()
  {
    terminate(process.get());
    process::wait(process.get());
  }

  Future<Nothing> registerAgent()
  {
    return dispatch(process.get(), &TestSlaveProcess2::registerAgent);
  }

private:
  Owned<TestSlaveProcess2> process;
};


size_t totalOffers = 0;
static std::mutex mutex;

class TestScheduler : public Scheduler
{
public:
  TestScheduler(
      int _id,
      double _declineTimeout,
      double _acceptRate,
      double _offerRateGranularity,
      size_t _agentCount
      )
    : id(_id),
      declineTimeout(_declineTimeout),
      acceptRate(_acceptRate),
      offerRateGranularity(_offerRateGranularity),
      slaveOfferCount(_agentCount, 0)
  {
    offersReceived = 0;
    tasksLaunched = 0;
    offersDeclined = 0;
  }

  virtual ~TestScheduler() {}

  Future<Nothing> registeredFramework()
  {
    return promise.future();
  }

  virtual void registered(SchedulerDriver* driver,
                          const FrameworkID& frameworkId,
                          const MasterInfo& masterInfo)
  {
    masterId = masterInfo.id();
    promise.set(Nothing());
  }

  virtual void reregistered(SchedulerDriver*, const MasterInfo& masterInfo) {}

  virtual void disconnected(SchedulerDriver* driver) {}

  virtual void resourceOffers(SchedulerDriver* driver,
                              const vector<Offer>& offers)
  {
    for (const Offer& offer : offers) {
      offersReceived++;

      synchronized(mutex) {
        totalOffers++;
      }

      // Slave ID is of the format <SHA>-S<NNN>.
      int slaveId =
        numify<int>(strings::tokenize(offer.slave_id().value(), "S")[1]).get();
      slaveOfferCount[slaveId]++;

      double declineRate = (double) offersDeclined / offersReceived;
      if (declineRate < (1 - acceptRate)) {
        Filters filters;
        filters.set_refuse_seconds(declineTimeout);

        driver->declineOffer(offer.id(), filters);

        offersDeclined++;

        // cout << "Declined offer count: " << offersDeclined << std::endl;
      } else {
        // Accept offer.
        // cout << "Sched[" << id << "] Accepting offer " << offer.id() << endl;
        launchTask(driver, offer);
      }
    }
  }

  void launchTask(SchedulerDriver* driver,
                  const Offer& offer)
  {
    int taskId = tasksLaunched++;

    Resources taskResources = Resources::parse(
        "cpus:" + stringify(CPUS_PER_TASK) +
        ";mem:" + stringify(MEM_PER_TASK)).get();
    taskResources.allocate("*");

    TaskInfo task;
    task.set_name("Task" + stringify(taskId));
    task.mutable_task_id()->set_value(stringify(taskId));
    task.mutable_slave_id()->MergeFrom(offer.slave_id());
    task.mutable_resources()->CopyFrom(taskResources);
    task.mutable_command()->set_shell(true);

    // Set task command based on the number of tasks launched.
    int modulo = tasksLaunched % 3;
    if (modulo == 0) {
      task.mutable_command()->set_value("sleep 1000");
    } else if (modulo == 1) {
      task.mutable_command()->set_value("echo hello");
    } else {
      task.mutable_command()->set_value("exit 0");
    }

    // cout << "Starting task " << taskId << std::endl;

    driver->launchTasks(offer.id(), {task});
  }

  virtual void offerRescinded(SchedulerDriver* driver, const OfferID& offerId)
  {}

  virtual void statusUpdate(SchedulerDriver* driver, const TaskStatus& status)
  {}

  virtual void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data)
  {}

  virtual void slaveLost(SchedulerDriver* driver, const SlaveID& sid) {}

  virtual void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorID,
      const SlaveID& slaveID,
      int status)
  {}

  virtual void error(SchedulerDriver* driver, const string& message)
  {
    cout << message << endl;
  }

  void printStats() {
    std::ostringstream o;
    o << endl << "Some fancy matrics: (acceptRate: " << acceptRate
      << ", offerRateGranularity: " << offerRateGranularity << ")" << endl;

    for (int count : slaveOfferCount) {
      o << count << ",";
    }
    o << endl;
    cout << o.str();
  }

  Promise<Nothing> promise;
  std::set<string> slaves;
  string masterId;

  int id;
  double declineTimeout;
  double acceptRate;
  int offerRateGranularity;

  int offersReceived;
  int offersDeclined;
  int tasksLaunched;
  vector<int> slaveOfferCount;
};


class MasterOffers_BENCHMARK_Test
  : public MesosTest,
    public WithParamInterface<tuple<
      size_t, size_t, double, double, size_t, size_t, size_t>> {};


INSTANTIATE_TEST_CASE_P(
    AgentFrameworkAllocDeclineDragRolesResources,
    MasterOffers_BENCHMARK_Test,
    ::testing::Values(
        std::make_tuple(10, 2000, 1.0, 1.0, 1, 1, 10000)));
        // std::make_tuple(10, 2000, 1.0, 1.0, 1, 10, 10000),
        // std::make_tuple(10, 10000, 1.0, 1.0, 1, 1, 10000),
        // std::make_tuple(10, 10000, 1.0, 1.0, 1, 10, 10000)));

TEST_P(MasterOffers_BENCHMARK_Test, Offers)
{
  size_t agentCount;
  size_t frameworkCount;
  double allocationInterval;
  double declineTimeout;
  double dragFactor;
  size_t roleCount;
  size_t maxOffers;

  synchronized(mutex) {
    totalOffers = 0;
  }

  tie(agentCount,
    frameworkCount,
    allocationInterval,
    declineTimeout,
    dragFactor,
    roleCount,
    maxOffers) = GetParam();

  agentCount = getEnv<int>("DRF_AGENT_COUNT", agentCount);

  frameworkCount = getEnv<int>("DRF_FRAMEWORK_COUNT", frameworkCount);

  allocationInterval =
    getEnv<double>("DRF_ALLOCATION_INTERVAL", allocationInterval);

  maxOffers = getEnv<int>("DRF_MAX_OFFERS", maxOffers);

  cout << "Test setup: "
       << agentCount << " agents with a total of "
       << frameworkCount << " frameworks " << endl;

  // Disable authentication to avoid the overhead, since we don't care about
  // it in this test.
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.authenticate_agents = false;
  masterFlags.authenticate_frameworks = false;
  masterFlags.authenticate_http_readonly = false;
  masterFlags.authenticate_http_readwrite = false;

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  std::cout << "Master listening on " << master.get()->pid << "\n";

  vector<Owned<TestSlave2>> slaves;

  for (size_t i = 0; i < agentCount; i++) {
    slaves.push_back(Owned<TestSlave2>(
            new TestSlave2(master.get()->pid, "agent-" + stringify(i))));
  }

  vector<Future<Nothing>> registeredAgents;

  for (const Owned<TestSlave2>& slave : slaves) {
    registeredAgents.push_back(slave->registerAgent());
  }

  // Wait all agents to finish registration.
  await(registeredAgents).await();

  cout << "Registered " << registeredAgents.size() << " agents\n";

  vector<Owned<MesosSchedulerDriver>> drivers;
  vector<Owned<TestScheduler>> schedulers;
  vector<Future<Nothing>> registeredFrameworks;

  int offerRateGranularity = 1;

  double maxAcceptRate = getEnv<double>("DRF_MAX_ACCEPT_RATE", 1.0);
  double minAcceptRate = getEnv<double>("DRF_MIN_ACCEPT_RATE", 0.8);
  double acceptRateDivisor = getEnv<double>("DRF_ACCEPT_DIVISOR", 2.0);

  double acceptRate = maxAcceptRate;

  double roleRate = (double)frameworkCount / (double)roleCount;
  double roleError = 0.0;
  int roleIndex = 1;

  cout << "Adding " << frameworkCount << " frameworks ... ";
  for (size_t i = 0; i < frameworkCount; i++) {
    Owned<TestScheduler> sched(new TestScheduler(
        i,
        declineTimeout,
        acceptRate,
        offerRateGranularity,
        agentCount));

    schedulers.push_back(sched);

    // cout << "framework index: " << i << std::endl;

    std::set<std::string> roles;
    roles.insert(strings::format("%ld", roleIndex).get());

    roleError += 1.0;

    if (roleError >= roleRate) {
      ++roleIndex;
      roleError -= roleRate;
    }

    FrameworkInfo frameworkInfo = createFrameworkInfo(i, roles);

    Owned<MesosSchedulerDriver> driver(new MesosSchedulerDriver(
        sched.get(),
        frameworkInfo,
        master.get()->pid));

    drivers.push_back(driver);
    registeredFrameworks.push_back(sched->registeredFramework());

    driver->start();

    acceptRate /= acceptRateDivisor;
    acceptRate = std::max(acceptRate, minAcceptRate);
  }

  cout << "done adding frameworks\n";

  cout << "Last role was " << roleIndex << "\n";

  // Wait all frameworks to finish registration.
  await(registeredFrameworks).await();

  cout << "Registered " << registeredFrameworks.size() << " frameworks\n";

  // Stopwatch watch;
  // watch.start();
  Clock::pause();

  while (totalOffers < maxOffers) {
    // sleep(1);
    // cout << "total offer: " << totalOffers << std::endl;
    Clock::advance(Seconds(1));
    Clock::settle();
  };

  cout << "Finished launching the tasks; Sleep 10 seconds ..." << std::endl;

  sleep(10);

  cout << "Start collecting metrics ..." << std::endl;

  // watch.stop();

  // std::ostringstream o;

  // o << "#class,total_offers,total_time" << endl;
  // o << agentCount << "x" << frameworkCount << ","
  //   << totalOffers << ","
  //   << watch.elapsed().ns() << endl;

  // o << "#class,scheduler,offers,time" << endl;
  // for (size_t i = 0; i < schedulers.size(); i++) {
  //   schedulers[i]->watch.stop();
  //   o << agentCount << "x" << frameworkCount << ","
  //     << i << ","
  //     << schedulers[i]->offersReceived << ","
  //     << schedulers[i]->watch.elapsed().ns() << endl;
  // }

  // cout << o.str();
  Stopwatch watch;
  watch.start();

  // We first measure v0 "metrics/snapshot" endpoint performance
  // as the baseline.
  Future<http::Response> v0Response = http::get(
      UPID("metrics", master.get()->pid.address),
      "snapshot",
      None(),
      createBasicAuthHeaders(DEFAULT_CREDENTIAL));

  v0Response.await();

  watch.stop();

  ASSERT_EQ(v0Response->status, http::OK().status);

  cout << "v0 '/metrics/snapshot' response took " << watch.elapsed() << endl;

  // cout << "v0 /metrics/snapshot: " << jsonify(v0Response->body);
  // Helper function to post a request to '/api/v1' master endpoint
  // and return the response.
  auto post = [](
      const process::PID<master::Master>& pid,
      const v1::master::Call& call,
      const ContentType& contentType)
  {
    http::Headers headers = createBasicAuthHeaders(DEFAULT_CREDENTIAL);
    headers["Accept"] = stringify(contentType);

    return http::post(
        pid,
        "api/v1",
        headers,
        serialize(contentType, call),
        stringify(contentType));
  };

  // We measure both JSON and protobuf formats.
  const ContentType contentTypes[] =
    { ContentType::PROTOBUF, ContentType::JSON };

  Duration timeout = Seconds(60);

  for (ContentType contentType : contentTypes){
    v1::master::Call v1Call;
    v1Call.set_type(v1::master::Call::GET_METRICS);
    v1Call.mutable_get_metrics()->mutable_timeout()->set_nanoseconds(
        timeout.ns());

    watch.start();

    Future<http::Response> response =
      post(master.get()->pid, v1Call, contentType);

    response.await();

    watch.stop();

    ASSERT_EQ(response->status, http::OK().status);

    Future<v1::master::Response> v1Response =
      deserialize<v1::master::Response>(contentType, response->body);

    ASSERT_TRUE(v1Response->IsInitialized());
    EXPECT_EQ(v1::master::Response::GET_METRICS, v1Response->type());

    cout << "v1 'master::call::GetMetrics' "
         << contentType << " response took " << watch.elapsed() << endl;

    // cout << "v1 /metrics/snapshot: " << jsonify(v0Response->body);
  }

  for (const Owned<MesosSchedulerDriver>& driver : drivers) {
    driver->stop();
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
