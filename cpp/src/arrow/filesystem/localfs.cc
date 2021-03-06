// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <chrono>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include "arrow/util/windows_compatibility.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#endif

#include "arrow/filesystem/localfs.h"
#include "arrow/filesystem/util_internal.h"
#include "arrow/io/file.h"
#include "arrow/util/io_util.h"
#include "arrow/util/logging.h"

namespace arrow {
namespace fs {

using ::arrow::internal::NativePathString;
using ::arrow::internal::PlatformFilename;

namespace {

template <typename... Args>
Status ErrnoToStatus(Args&&... args) {
  auto err_string = ::arrow::internal::ErrnoMessage(errno);
  return Status::IOError(std::forward<Args>(args)..., err_string);
}

#ifdef _WIN32

std::string NativeToString(const NativePathString& ns) {
  PlatformFilename fn(ns);
  return fn.ToString();
}

template <typename... Args>
Status WinErrorToStatus(Args&&... args) {
  auto err_string = ::arrow::internal::WinErrorMessage(GetLastError());
  return Status::IOError(std::forward<Args>(args)..., err_string);
}

TimePoint ToTimePoint(FILETIME ft) {
  // Hundreds of nanoseconds between January 1, 1601 (UTC) and the Unix epoch.
  static constexpr int64_t kFileTimeEpoch = 11644473600LL * 10000000;

  int64_t hundreds = (static_cast<int64_t>(ft.dwHighDateTime) << 32) + ft.dwLowDateTime -
                     kFileTimeEpoch;  // hundreds of ns since Unix epoch
  std::chrono::nanoseconds ns_count(100 * hundreds);
  return TimePoint(std::chrono::duration_cast<TimePoint::duration>(ns_count));
}

FileStats FileInformationToFileStat(const BY_HANDLE_FILE_INFORMATION& info) {
  FileStats st;
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    st.set_type(FileType::Directory);
    st.set_size(kNoSize);
  } else {
    // Regular file
    st.set_type(FileType::File);
    st.set_size((static_cast<int64_t>(info.nFileSizeHigh) << 32) + info.nFileSizeLow);
  }
  st.set_mtime(ToTimePoint(info.ftLastWriteTime));
  return st;
}

Result<FileStats> StatFile(const std::wstring& path) {
  HANDLE h;
  std::string bytes_path = NativeToString(path);
  FileStats st;

  /* Inspired by CPython, see Modules/posixmodule.c */
  h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, /* desired access */
                  0,                                  /* share mode */
                  NULL,                               /* security attributes */
                  OPEN_EXISTING,
                  /* FILE_FLAG_BACKUP_SEMANTICS is required to open a directory */
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, NULL);

  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      st.set_path(bytes_path);
      st.set_type(FileType::NonExistent);
      st.set_mtime(kNoTime);
      st.set_size(kNoSize);
      return st;
    } else {
      return WinErrorToStatus("Failed querying information for path '", bytes_path, "'");
    }
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(h, &info)) {
    CloseHandle(h);
    return WinErrorToStatus("Failed querying information for path '", bytes_path, "'");
  }
  CloseHandle(h);
  st = FileInformationToFileStat(info);
  st.set_path(bytes_path);
  return st;
}

#else  // POSIX systems

TimePoint ToTimePoint(const struct timespec& s) {
  std::chrono::nanoseconds ns_count(static_cast<int64_t>(s.tv_sec) * 1000000000 +
                                    static_cast<int64_t>(s.tv_nsec));
  return TimePoint(std::chrono::duration_cast<TimePoint::duration>(ns_count));
}

FileStats StatToFileStat(const struct stat& s) {
  FileStats st;
  if (S_ISREG(s.st_mode)) {
    st.set_type(FileType::File);
    st.set_size(static_cast<int64_t>(s.st_size));
  } else if (S_ISDIR(s.st_mode)) {
    st.set_type(FileType::Directory);
    st.set_size(kNoSize);
  } else {
    st.set_type(FileType::Unknown);
    st.set_size(kNoSize);
  }
#ifdef __APPLE__
  // macOS doesn't use the POSIX-compliant spelling
  st.set_mtime(ToTimePoint(s.st_mtimespec));
#else
  st.set_mtime(ToTimePoint(s.st_mtim));
#endif
  return st;
}

Result<FileStats> StatFile(const std::string& path) {
  FileStats st;
  struct stat s;
  int r = stat(path.c_str(), &s);
  if (r == -1) {
    if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) {
      st.set_type(FileType::NonExistent);
      st.set_mtime(kNoTime);
      st.set_size(kNoSize);
    } else {
      return ErrnoToStatus("Failed stat()ing path '", path, "'");
    }
  } else {
    st = StatToFileStat(s);
  }
  st.set_path(path);
  return st;
}

#endif

Status StatSelector(const PlatformFilename& dir_fn, const Selector& select,
                    int32_t nesting_depth, std::vector<FileStats>* out) {
  std::vector<PlatformFilename> children;
  Status status = ListDir(dir_fn, &children);
  if (!status.ok()) {
    if (select.allow_non_existent && status.IsIOError()) {
      bool exists;
      RETURN_NOT_OK(FileExists(dir_fn, &exists));
      if (!exists) {
        return Status::OK();
      }
    }
    return status;
  }

  for (const auto& child_fn : children) {
    PlatformFilename full_fn = dir_fn.Join(child_fn);
    ARROW_ASSIGN_OR_RAISE(FileStats st, StatFile(full_fn.ToNative()));
    if (st.type() != FileType::NonExistent) {
      out->push_back(std::move(st));
    }
    if (nesting_depth < select.max_recursion && select.recursive &&
        st.type() == FileType::Directory) {
      RETURN_NOT_OK(StatSelector(full_fn, select, nesting_depth + 1, out));
    }
  }
  return Status::OK();
}

}  // namespace

LocalFileSystemOptions LocalFileSystemOptions::Defaults() {
  return LocalFileSystemOptions();
}

LocalFileSystem::LocalFileSystem() : options_(LocalFileSystemOptions::Defaults()) {}

LocalFileSystem::LocalFileSystem(const LocalFileSystemOptions& options)
    : options_(options) {}

LocalFileSystem::~LocalFileSystem() {}

Result<FileStats> LocalFileSystem::GetTargetStats(const std::string& path) {
  PlatformFilename fn;
  RETURN_NOT_OK(PlatformFilename::FromString(path, &fn));
  return StatFile(fn.ToNative());
}

Result<std::vector<FileStats>> LocalFileSystem::GetTargetStats(const Selector& select) {
  PlatformFilename fn;
  RETURN_NOT_OK(PlatformFilename::FromString(select.base_dir, &fn));
  std::vector<FileStats> results;
  RETURN_NOT_OK(StatSelector(fn, select, 0, &results));
  return results;
}

Status LocalFileSystem::CreateDir(const std::string& path, bool recursive) {
  PlatformFilename fn;
  RETURN_NOT_OK(PlatformFilename::FromString(path, &fn));
  if (recursive) {
    return ::arrow::internal::CreateDirTree(fn);
  } else {
    return ::arrow::internal::CreateDir(fn);
  }
}

Status LocalFileSystem::DeleteDir(const std::string& path) {
  bool deleted = false;
  PlatformFilename fn;
  RETURN_NOT_OK(PlatformFilename::FromString(path, &fn));
  RETURN_NOT_OK(::arrow::internal::DeleteDirTree(fn, &deleted));
  if (deleted) {
    return Status::OK();
  } else {
    return Status::IOError("Directory does not exist: '", path, "'");
  }
}

Status LocalFileSystem::DeleteDirContents(const std::string& path) {
  bool deleted = false;
  PlatformFilename fn;
  RETURN_NOT_OK(PlatformFilename::FromString(path, &fn));
  RETURN_NOT_OK(::arrow::internal::DeleteDirContents(fn, &deleted));
  if (deleted) {
    return Status::OK();
  } else {
    return Status::IOError("Directory does not exist: '", path, "'");
  }
}

Status LocalFileSystem::DeleteFile(const std::string& path) {
  bool deleted = false;
  PlatformFilename fn;
  RETURN_NOT_OK(PlatformFilename::FromString(path, &fn));
  RETURN_NOT_OK(::arrow::internal::DeleteFile(fn, &deleted));
  if (deleted) {
    return Status::OK();
  } else {
    return Status::IOError("File does not exist: '", path, "'");
  }
}

Status LocalFileSystem::Move(const std::string& src, const std::string& dest) {
  PlatformFilename sfn, dfn;
  RETURN_NOT_OK(PlatformFilename::FromString(src, &sfn));
  RETURN_NOT_OK(PlatformFilename::FromString(dest, &dfn));

#ifdef _WIN32
  if (!MoveFileExW(sfn.ToNative().c_str(), dfn.ToNative().c_str(),
                   MOVEFILE_REPLACE_EXISTING)) {
    return WinErrorToStatus("Failed renaming '", sfn.ToString(), "' to '", dfn.ToString(),
                            "': ");
  }
#else
  if (rename(sfn.ToNative().c_str(), dfn.ToNative().c_str()) == -1) {
    return ErrnoToStatus("Failed renaming '", sfn.ToString(), "' to '", dfn.ToString(),
                         "': ");
  }
#endif
  return Status::OK();
}

Status LocalFileSystem::CopyFile(const std::string& src, const std::string& dest) {
  PlatformFilename sfn, dfn;
  RETURN_NOT_OK(PlatformFilename::FromString(src, &sfn));
  RETURN_NOT_OK(PlatformFilename::FromString(dest, &dfn));
  // XXX should we use fstat() to compare inodes?
  if (sfn.ToNative() == dfn.ToNative()) {
    return Status::OK();
  }

#ifdef _WIN32
  if (!CopyFileW(sfn.ToNative().c_str(), dfn.ToNative().c_str(),
                 FALSE /* bFailIfExists */)) {
    return WinErrorToStatus("Failed copying '", sfn.ToString(), "' to '", dfn.ToString(),
                            "': ");
  }
  return Status::OK();
#else
  ARROW_ASSIGN_OR_RAISE(auto is, OpenInputStream(src));
  ARROW_ASSIGN_OR_RAISE(auto os, OpenOutputStream(dest));
  RETURN_NOT_OK(internal::CopyStream(is, os, 1024 * 1024 /* chunk_size */));
  RETURN_NOT_OK(os->Close());
  return is->Close();
#endif
}

namespace {

template <typename InputStreamType>
Result<std::shared_ptr<InputStreamType>> OpenInputStreamGeneric(
    const std::string& path, const LocalFileSystemOptions& options) {
  if (options.use_mmap) {
    std::shared_ptr<io::MemoryMappedFile> file;
    RETURN_NOT_OK(io::MemoryMappedFile::Open(path, io::FileMode::READ, &file));
    return file;
  } else {
    std::shared_ptr<io::ReadableFile> file;
    RETURN_NOT_OK(io::ReadableFile::Open(path, &file));
    return file;
  }
}

}  // namespace

Result<std::shared_ptr<io::InputStream>> LocalFileSystem::OpenInputStream(
    const std::string& path) {
  return OpenInputStreamGeneric<io::InputStream>(path, options_);
}

Result<std::shared_ptr<io::RandomAccessFile>> LocalFileSystem::OpenInputFile(
    const std::string& path) {
  return OpenInputStreamGeneric<io::RandomAccessFile>(path, options_);
}

namespace {

Result<std::shared_ptr<io::OutputStream>> OpenOutputStreamGeneric(const std::string& path,
                                                                  bool truncate,
                                                                  bool append) {
  PlatformFilename fn;
  int fd;
  bool write_only = true;
  RETURN_NOT_OK(PlatformFilename::FromString(path, &fn));
  RETURN_NOT_OK(
      ::arrow::internal::FileOpenWritable(fn, write_only, truncate, append, &fd));
  std::shared_ptr<io::OutputStream> stream;
  Status st = io::FileOutputStream::Open(fd, &stream);
  if (!st.ok()) {
    ARROW_UNUSED(::arrow::internal::FileClose(fd));
    return st;
  }
  return stream;
}

}  // namespace

Result<std::shared_ptr<io::OutputStream>> LocalFileSystem::OpenOutputStream(
    const std::string& path) {
  bool truncate = true;
  bool append = false;
  return OpenOutputStreamGeneric(path, truncate, append);
}

Result<std::shared_ptr<io::OutputStream>> LocalFileSystem::OpenAppendStream(
    const std::string& path) {
  bool truncate = false;
  bool append = true;
  return OpenOutputStreamGeneric(path, truncate, append);
}

}  // namespace fs
}  // namespace arrow
