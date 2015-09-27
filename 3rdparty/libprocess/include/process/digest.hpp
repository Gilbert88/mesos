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

#ifndef __PROCESS_DIGEST_HPP__
#define __PROCESS_DIGEST_HPP__

#ifdef USE_SSL_SOCKET
#include "openssl/sha.h"
#endif

#include <functional>
#include <string>

#include <stout/try.hpp>

#include <process/io.hpp>
#include <process/owned.hpp>

using process::Failure;
using process::Future;

namespace process {


/*
 * Encapsulates the concept of cryptographic digest.
 */
struct Digest
{
  /*
   * Enumeration of supported digest types.
   */
  enum SHA {SHA256, SHA512};
};


template <Digest::SHA>
struct DigestTypeTraits {};


static Future<size_t> readAndUpdate(
    int fd,
    char* buffer,
    size_t size,
    std::function<void(char*, size_t)> fn)
{
  return process::io::read(fd, buffer, size)
    .then([=](
        Future<size_t> future) {
      if (future.get() == 0) {
        return future;
      } else {
        fn(buffer, future.get());

        if (future.get() < size) {
          return future;
        }

        return readAndUpdate(fd, buffer, size, fn);
      }
    });
}


#ifdef USE_SSL_SOCKET

template <>
struct DigestTypeTraits<Digest::SHA256>
{
  typedef SHA256_CTX ctx_type;
  static const size_t block_size = SHA256_CBLOCK;
  static const size_t digest_length = SHA256_DIGEST_LENGTH;
};


template <>
struct DigestTypeTraits<Digest::SHA512>
{
  typedef SHA512_CTX ctx_type;
  static const size_t block_size = SHA512_CBLOCK;
  static const size_t digest_length = SHA512_DIGEST_LENGTH;
};


template <Digest::SHA T>
struct DigestFunctionTraits
{
  typedef typename DigestTypeTraits<T>::ctx_type ctx_type;

  typedef int (*Init_fn)(ctx_type*);
  typedef int (*Update_fn)(ctx_type*, const void*, size_t);
  typedef int (*Final_fn)(uint8_t*, ctx_type*);

  static Init_fn Init;
  static Update_fn Update;
  static Final_fn Final;
};


template<>
DigestFunctionTraits<Digest::SHA256>::Init_fn
  DigestFunctionTraits<Digest::SHA256>::Init = SHA256_Init;


template<>
DigestFunctionTraits<Digest::SHA256>::Update_fn
  DigestFunctionTraits<Digest::SHA256>::Update = SHA256_Update;


template<>
DigestFunctionTraits<Digest::SHA256>::Final_fn
  DigestFunctionTraits<Digest::SHA256>::Final = SHA256_Final;


template<>
DigestFunctionTraits<Digest::SHA512>::Init_fn
  DigestFunctionTraits<Digest::SHA512>::Init = SHA512_Init;


template<>
DigestFunctionTraits<Digest::SHA512>::Update_fn
  DigestFunctionTraits<Digest::SHA512>::Update = SHA512_Update;


template<>
DigestFunctionTraits<Digest::SHA512>::Final_fn
  DigestFunctionTraits<Digest::SHA512>::Final = SHA512_Final;


//TODO(jojy): Add more traits if needed.


/*
 * Provides generic template for digests.
 */
template <Digest::SHA T>
struct DigestUtil
{
  static Try<bool> verify(
      const char* bytes,
      size_t size,
      const std::string& expected)
  {
    Try<std::string> digest = DigestUtil<T>::digest(bytes, size);
    if (digest.isError()) {
      return Error("Failed to calculate checksum: " + digest.error());
    }

    return (digest.get() == expected);
  }

  static Try<std::string> digest(const char* bytes, size_t size)
  {
    if (bytes == nullptr) {
      return Error("Cannot calculate digest for null data");
    }

    char digest[DigestTypeTraits<T>::block_size + 1] = {0};
    uint8_t hash[DigestTypeTraits<T>::digest_length];

    typename DigestTypeTraits<T>::ctx_type sha;
    DigestFunctionTraits<T>::Init(&sha);
    DigestFunctionTraits<T>::Update(&sha, bytes, size);
    DigestFunctionTraits<T>::Final(hash, &sha);

    for(int i = 0; i < DigestTypeTraits<T>::digest_length; i++)
    {
      sprintf(digest + (i * 2), "%02x", hash[i]);
    }

    return digest;
  }

  static Future<std::string> digest(const std::string& filePath)
  {
    Try<int> fd = os::open(filePath, O_RDONLY, S_IRUSR);

    if (fd.isError()) {
      return Failure(
          "Failed to open file '" + filePath + "': " + fd.error());
    }

    Try<Nothing> nonBlock = os::nonblock(fd.get());
    if (nonBlock.isError()) {
      return Failure(
          "Failed to set file '" + filePath +
          "' as non-blocking: " + nonBlock.error());
    }

    static const size_t BUFFER_LENGTH = 1024;
    typename DigestTypeTraits<T>::ctx_type sha, *psha = &sha;

    DigestFunctionTraits<T>::Init(&sha);

    auto updateSHA = [psha] (
        char* buffer, size_t length) {
      DigestFunctionTraits<T>::Update(psha, buffer, length);
    };

    char buffer[BUFFER_LENGTH];

    return readAndUpdate(fd.get(), buffer, BUFFER_LENGTH, updateSHA)
      .then([psha] (const Future<size_t>& readFuture) {
        uint8_t hash[DigestTypeTraits<T>::digest_length];
        DigestFunctionTraits<T>::Final(hash, psha);

        char digest[DigestTypeTraits<T>::block_size + 1] = {0};
        for(int i = 0; i < DigestTypeTraits<T>::digest_length; i++)
        {
          sprintf(digest + (i * 2), "%02x", hash[i]);
        }

        return std::string(digest);
      });
  }
};

#endif


/*
 * Calculates digest of a blob of data and verifies against exptected(input)
 * digest.
 *
 * @param type Digest type (sha256 for example).
 * @param bytes Input bytes for which digest has to be calculated.
 * @param size Size of the data bytes.
 * @param digest Digest value for comparision.
 * @returns true if the calculated digest equals the given digest.
 *          false otherwise.
 *          Error in case of any processing error.
 */
Try<bool> verify(
    const std::string& type,
    const char* bytes,
    size_t size,
    const std::string& expected)
{
#ifdef USE_SSL_SOCKET
  if (type == "sha256") {
    return DigestUtil<Digest::SHA256>::verify(bytes, size, expected);
  }

  if (type == "sha512") {
    return DigestUtil<Digest::SHA512>::verify(bytes, size, expected);
  }
#endif

  return Error(
      "Failed to find digest verification implemenation for type: " + type);
}


/*
 * Calculates digest of a blob of data.
 *
 * @param type Digest type (sha256 for example).
 * @param bytes Input bytes for which digest has to be calculated.
 * @param size Size of the data bytes.
 * @param digest Digest value for comparision.
 * @returns digest value on success.
 *          Error otherwise.
 */
Try<std::string> digest(const std::string& type, const char* bytes, size_t size)
{
#ifdef USE_SSL_SOCKET
  if (type == "sha256") {
    return DigestUtil<Digest::SHA256>::digest(bytes, size);
  }

  if (type == "sha512") {
    return DigestUtil<Digest::SHA512>::digest(bytes, size);
  }
#endif

  return Error("Failed to find digest implemenation for type: " + type);
}


/*
 * Calculates digest of a file.
 *
 * @param type Digest type (sha256 for example).
 * @param filePath File for which digest needs to be calculated.
 * @returns digest value on success.
 *          Failure otherwise.
 */
Future<std::string> digest(const std::string& type, const std::string& filePath)
{
#ifdef USE_SSL_SOCKET
  if (type == "sha256") {
    return DigestUtil<Digest::SHA256>::digest(filePath);
  }

  if (type == "sha512") {
    return DigestUtil<Digest::SHA512>::digest(filePath);
  }
#endif

  return Failure("Failed to find file digest implemenation for type: " + type);
}

} // namespace process {

#endif //  __PROCESS_DIGEST_HPP__
