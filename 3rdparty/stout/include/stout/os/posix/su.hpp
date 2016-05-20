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

#ifndef __STOUT_OS_POSIX_SU_HPP__
#define __STOUT_OS_POSIX_SU_HPP__

#include <string>

#include <grp.h>
#include <pwd.h>

#include <sys/syscall.h>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>
#include <stout/unreachable.hpp>

namespace os {

inline Result<uid_t> getuid(const Option<std::string>& user = None())
{
  if (user.isNone()) {
    return ::getuid();
  }

  struct passwd passwd;
  struct passwd* result = NULL;

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  while (true) {
    char* buffer = new char[size];

    if (getpwnam_r(user.get().c_str(), &passwd, buffer, size, &result) == 0) {
      // The usual interpretation of POSIX is that getpwnam_r will
      // return 0 but set result == NULL if the user is not found.
      if (result == NULL) {
        delete[] buffer;
        return None();
      }

      uid_t uid = passwd.pw_uid;
      delete[] buffer;
      return uid;
    } else {
      // RHEL7 (and possibly other systems) will return non-zero and
      // set one of the following errors for "The given name or uid
      // was not found." See 'man getpwnam_r'. We only check for the
      // errors explicitly listed, and do not consider the ellipsis.
      if (errno == ENOENT ||
          errno == ESRCH ||
          errno == EBADF ||
          errno == EPERM) {
        delete[] buffer;
        return None();
      }

      if (errno != ERANGE) {
        delete[] buffer;
        return ErrnoError("Failed to get username information");
      }
      // getpwnam_r set ERANGE so try again with a larger buffer.
      size *= 2;
      delete[] buffer;
    }
  }

  UNREACHABLE();
}


inline Try<Nothing> setuid(uid_t uid)
{
  if (::setuid(uid) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Result<gid_t> getgid(const Option<std::string>& user = None())
{
  if (user.isNone()) {
    return ::getgid();
  }

  struct passwd passwd;
  struct passwd* result = NULL;

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  while (true) {
    char* buffer = new char[size];

    if (getpwnam_r(user.get().c_str(), &passwd, buffer, size, &result) == 0) {
      // The usual interpretation of POSIX is that getpwnam_r will
      // return 0 but set result == NULL if the group is not found.
      if (result == NULL) {
        delete[] buffer;
        return None();
      }

      gid_t gid = passwd.pw_gid;
      delete[] buffer;
      return gid;
    } else {
      // RHEL7 (and possibly other systems) will return non-zero and
      // set one of the following errors for "The given name or uid
      // was not found." See 'man getpwnam_r'. We only check for the
      // errors explicitly listed, and do not consider the ellipsis.
      if (errno == ENOENT ||
          errno == ESRCH ||
          errno == EBADF ||
          errno == EPERM) {
        delete[] buffer;
        return None();
      }

      if (errno != ERANGE) {
        delete[] buffer;
        return ErrnoError("Failed to get username information");
      }
      // getpwnam_r set ERANGE so try again with a larger buffer.
      size *= 2;
      delete[] buffer;
    }
  }

  UNREACHABLE();
}


inline Try<Nothing> setgid(gid_t gid)
{
  if (::setgid(gid) == -1) {
    return ErrnoError();
  }

  return Nothing();
}


inline Try<std::vector<gid_t>> getgrouplist(const std::string& user)
{
  // TODO(jieyu): Consider adding a 'gid' parameter and avoid calling
  // getgid here. In some cases, the primary gid might be known.
  Result<gid_t> gid = os::getgid(user);
  if (!gid.isSome()) {
    return Error("Failed to get the gid of the user: " +
                 (gid.isError() ? gid.error() : "group not found"));
  }

#ifdef __APPLE__
  int gids[NGROUPS_MAX];
#else
  gid_t gids[NGROUPS_MAX];
#endif
  int ngroups = NGROUPS_MAX;

  if (::getgrouplist(user.c_str(), gid.get(), gids, &ngroups) == -1) {
    return ErrnoError();
  }

  std::vector<gid_t> result(gids, gids + ngroups);

  return result;
}


inline Try<Nothing> setgroups(
    const std::vector<gid_t>& gids,
    const Option<uid_t>& uid = None())
{
  int ngroups = static_cast<int>(gids.size());
  gid_t _gids[ngroups];

  for (int i = 0; i < ngroups; i++) {
    _gids[i] = gids[i];
  }

#ifdef __APPLE__
   // Groups implementation in the Darwin kernel is different. The list
   // groups in the kernel is a cache, which means that setgroups only
   // set the groups that are ever checked, but the dynamic resolution
   // mechanism is needed here.
   // For more detail please see:
   // https://github.com/practicalswift/osx/blob/master/src/samba/patches/support-darwin-initgroups-syscall // NOLINT
  int maxgroups = sysconf(_SC_NGROUPS_MAX);
  if (maxgroups == -1) {
    return Error("Failed to get sysconf(_SC_NGROUPS_MAX)");
  }

  if (ngroups > maxgroups) {
      ngroups = maxgroups;
  }

  if (uid.isNone()) {
    return Error(
        "The uid of the user who is associated with the group "
        "list we are setting is missing");
  }

  if (::syscall(SYS_initgroups, ngroups, _gids, uid.get()) == -1) {
    return ErrnoError();
  }
#else
  if (::setgroups(ngroups, _gids) == -1) {
    return ErrnoError();
  }
#endif

  return Nothing();
}


inline Result<std::string> user(Option<uid_t> uid = None())
{
  if (uid.isNone()) {
    uid = ::getuid();
  }

  int size = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (size == -1) {
    // Initial value for buffer size.
    size = 1024;
  }

  struct passwd passwd;
  struct passwd* result = NULL;

  while (true) {
    char* buffer = new char[size];

    if (getpwuid_r(uid.get(), &passwd, buffer, size, &result) == 0) {
      // getpwuid_r will return 0 but set result == NULL if the uid is
      // not found.
      if (result == NULL) {
        delete[] buffer;
        return None();
      }

      std::string user(passwd.pw_name);
      delete[] buffer;
      return user;
    } else {
      if (errno != ERANGE) {
        delete[] buffer;
        return ErrnoError();
      }

      // getpwuid_r set ERANGE so try again with a larger buffer.
      size *= 2;
      delete[] buffer;
    }
  }
}

inline Try<Nothing> su(const std::string& user)
{
  Result<gid_t> gid = os::getgid(user);
  if (gid.isError() || gid.isNone()) {
    return Error("Failed to getgid: " +
        (gid.isError() ? gid.error() : "unknown user"));
  } else if (::setgid(gid.get())) {
    return ErrnoError("Failed to set gid");
  }

  // Set the supplementary group list. We ignore EPERM because
  // performing a no-op call (switching to same group) still
  // requires being privileged, unlike 'setgid' and 'setuid'.
  if (::initgroups(user.c_str(), gid.get()) == -1 && errno != EPERM) {
    return ErrnoError("Failed to set supplementary groups");
  }

  Result<uid_t> uid = os::getuid(user);
  if (uid.isError() || uid.isNone()) {
    return Error("Failed to getuid: " +
        (uid.isError() ? uid.error() : "unknown user"));
  } else if (::setuid(uid.get())) {
    return ErrnoError("Failed to setuid");
  }

  return Nothing();
}

} // namespace os {

#endif // __STOUT_OS_POSIX_SU_HPP__
