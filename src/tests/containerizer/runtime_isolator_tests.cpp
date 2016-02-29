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

#include <gtest/gtest.h>

#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/try.hpp>

#include <process/owned.hpp>

#include "tests/mesos.hpp"

#include "tests/containerizer/docker_archive.hpp"

namespace mesos {
namespace internal {
namespace tests {

using std::string;

using process::Owned;

#ifdef __linux__
class DockerLocalTarTest : public ::testing::Test {};


TEST_F(DockerLocalTarTest, ROOT_RUNTIME_CreateDockerLocalTar)
{
  Owned<DockerLocalTar> dockerTar(new DockerLocalTar());

  const string archivesDir = path::join(os::getcwd(), "archives");

  Try<Nothing> testImage = dockerTar->create(archivesDir, "busybox");
  ASSERT_SOME(testImage);

  EXPECT_TRUE(os::exists(path::join(archivesDir, "busybox.tar")));
  EXPECT_FALSE(os::exists(path::join(archivesDir, "busybox")));
}

#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
