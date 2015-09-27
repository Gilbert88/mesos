/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>
#include <stout/path.hpp>

#include <process/digest.hpp>
#include <process/gtest.hpp>

using namespace process;

using std::string;


#ifdef USE_SSL_SOCKET

TEST(Digest, Simple)
{
  const string testData("hello world");

  Try<string> digest =
    DigestUtil<Digest::SHA256>::digest(
        testData.c_str(),
        testData.length() - 1);

  ASSERT_SOME(digest);

  Try<bool> isEqual = DigestUtil<Digest::SHA256>::verify(
      testData.c_str(),
      testData.length() - 1,
      digest.get());

  ASSERT_SOME(isEqual);

  ASSERT_EQ(isEqual.get(), true);
}


TEST(Digest, DynamicDigestType)
{
  const string testData("hello world");

  Try<string> digest =
    process::digest("sha256", testData.c_str(), testData.length() - 1);

  ASSERT_SOME(digest);

  Try<bool> isEqual =
    process::verify(
        "sha256",
        testData.c_str(),
        testData.length() - 1,
        digest.get());

  ASSERT_SOME(isEqual);

  ASSERT_EQ(isEqual.get(), true);
}


TEST(Digest, SimpleFile)
{
  const string testData("This is the data to be written to a file.");
  const string filePath = path::join(os::getcwd(), "digestFile.txt");

  ASSERT_SOME(os::write(filePath, testData));

  Future<string> digest =
    DigestUtil<Digest::SHA512>::digest(filePath);

  ASSERT_TRUE(digest.isReady());

  Try<bool> isEqual = DigestUtil<Digest::SHA512>::verify(
    testData.c_str(),
    testData.length() - 1,
    digest.get());

  ASSERT_SOME(isEqual);

  ASSERT_EQ(isEqual.get(), true);
}


TEST(Digest, DynamicDigestTypeFile)
{
  const string testData("This is the data to be written to a file.");
  const string filePath = path::join(os::getcwd(), "dynamicDigest.txt");

  ASSERT_SOME(os::write(filePath, testData));

  Future<string> digest =
    process::digest("sha512", filePath);

  ASSERT_TRUE(digest.isReady());

  Try<bool> isEqual =
    process::verify(
      "sha512",
      testData.c_str(),
      testData.length() - 1,
      digest.get());

  ASSERT_SOME(isEqual);

  ASSERT_EQ(isEqual.get(), true);
}

#endif
