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

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>

#include "slave/containerizer/mesos/provisioner/paths.hpp"

#include "tests/mesos.hpp"

using namespace mesos::internal::slave::provisioner::paths;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class ProvisionerPathTest : public TemporaryDirectoryTest {};


TEST_F(ProvisionerPathTest, FindContainerDirectory)
{
  const string provisionerDir = os::getcwd();

  ContainerID child1;
  ContainerID child2;
  ContainerID child3;

  child1.set_value("child_1_1");
  child1.mutable_parent()->set_value("parent_1");
  child2.set_value("child_1_2");
  child2.mutable_parent()->set_value("parent_1");
  child3.set_value("child_2_1");
  child3.mutable_parent()->set_value("parent_2");

  const string containerDir1 = getContainerDir(provisionerDir, child1);
  const string containerDir2 = getContainerDir(provisionerDir, child2);
  const string containerDir3 = getContainerDir(provisionerDir, child3);

  Try<Nothing> mkdir = os::mkdir(containerDir1);
  ASSERT_SOME(mkdir);
  mkdir = os::mkdir(containerDir2);
  ASSERT_SOME(mkdir);
  mkdir = os::mkdir(containerDir3);
  ASSERT_SOME(mkdir);

  Try<Option<string>> find = findContainerDir(provisionerDir, child1);
  ASSERT_SOME(find);
  EXPECT_SOME_EQ(containerDir1, find.get());

  find = findContainerDir(provisionerDir, child2);
  ASSERT_SOME(find);
  EXPECT_SOME_EQ(containerDir2, find.get());

  find = findContainerDir(provisionerDir, child3);
  ASSERT_SOME(find);
  EXPECT_SOME_EQ(containerDir3, find.get());

  ContainerID child4;
  child4.set_value("orphaned_child");
  find = findContainerDir(provisionerDir, child4);
  ASSERT_SOME(find);
  EXPECT_NONE(find.get());
}


TEST_F(ProvisionerPathTest, ListProvisionerContainers)
{
  const string provisionerDir = os::getcwd();

  // TODO(gilbert): Refactor duplicate code to the unit test
  // class.
  ContainerID child1;
  ContainerID child2;
  ContainerID child3;
  ContainerID parent1;
  ContainerID parent2;

  child1.set_value("child_1_1");
  child1.mutable_parent()->set_value("parent_1");
  child2.set_value("child_1_2");
  child2.mutable_parent()->set_value("parent_1");
  child3.set_value("child_2_1");
  child3.mutable_parent()->set_value("parent_2");
  parent1.set_value("parent_1");
  parent2.set_value("parent_2");

  const string containerDir1 = getContainerDir(provisionerDir, child1);
  const string containerDir2 = getContainerDir(provisionerDir, child2);
  const string containerDir3 = getContainerDir(provisionerDir, child3);

  Try<Nothing> mkdir = os::mkdir(containerDir1);
  ASSERT_SOME(mkdir);
  mkdir = os::mkdir(containerDir2);
  ASSERT_SOME(mkdir);
  mkdir = os::mkdir(containerDir3);
  ASSERT_SOME(mkdir);

  Try<hashset<ContainerID>> containerIds = listContainers(provisionerDir);
  ASSERT_SOME(containerIds);

  EXPECT_TRUE(containerIds->contains(parent1));
  EXPECT_TRUE(containerIds->contains(parent2));
  EXPECT_TRUE(containerIds->contains(child1));
  EXPECT_TRUE(containerIds->contains(child2));
  EXPECT_TRUE(containerIds->contains(child3));
  EXPECT_EQ(5u, containerIds->size());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
