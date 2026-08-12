#include "env.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// Platform headers. These must stay inside the conditional and must NOT be
// sorted into the block above - a tidy-up pass that reorders them across the
// #if/#else boundary produces "#else without #if", which is how this file got
// broken once already.
#if defined(_WIN32)
// NOMINMAX: <windows.h> defines min/max as macros, which breaks std::min and
// std::max in any translation unit that sees it. WIN32_LEAN_AND_MEAN drops the
// parts of the Win32 API we do not use and roughly halves the parse time.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <fcntl.h>   // _O_RDWR, _O_BINARY
#include <io.h>      // _open, _close, _commit, _chsize_s, _access
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sextant::lsm {
namespace {

Status PosixError(const std::string& context, int err) {
  if (err == ENOENT) return Status::NotFound(context, std::strerror(err));
  return Status::IOError(context, std::strerror(err));
}

}  // namespace

// --- WritableFile ----------------------------------------------------------

Status WritableFile::Open(const std::string& fname, bool append,
                          std::unique_ptr<WritableFile>* result) {
  std::FILE* f = std::fopen(fname.c_str(), append ? "ab" : "wb");
  if (f == nullptr) return PosixError(fname, errno);
  result->reset(new WritableFile(f, fname));
  return Status::OK();
}

WritableFile::~WritableFile() {
  if (file_ != nullptr) std::fclose(file_);
}

Status WritableFile::Append(const Slice& data) {
  const size_t n = std::fwrite(data.data(), 1, data.size(), file_);
  if (n != data.size()) return PosixError(filename_, errno);
  return Status::OK();
}

Status WritableFile::Flush() {
  if (std::fflush(file_) != 0) return PosixError(filename_, errno);
  return Status::OK();
}

Status WritableFile::Sync() {
  if (std::fflush(file_) != 0) return PosixError(filename_, errno);
#if defined(_WIN32)
  const int fd = _fileno(file_);
  if (fd < 0) return PosixError(filename_, errno);
  if (_commit(fd) != 0) return PosixError(filename_, errno);
#else
  const int fd = fileno(file_);
  if (fd < 0) return PosixError(filename_, errno);
  if (::fsync(fd) != 0) return PosixError(filename_, errno);
#endif
  return Status::OK();
}

Status WritableFile::Close() {
  if (file_ == nullptr) return Status::OK();
  Status s;
  if (std::fclose(file_) != 0) s = PosixError(filename_, errno);
  file_ = nullptr;
  return s;
}

// --- SequentialFile --------------------------------------------------------

Status SequentialFile::Open(const std::string& fname,
                            std::unique_ptr<SequentialFile>* result) {
  std::FILE* f = std::fopen(fname.c_str(), "rb");
  if (f == nullptr) return PosixError(fname, errno);
  result->reset(new SequentialFile(f, fname));
  return Status::OK();
}

SequentialFile::~SequentialFile() {
  if (file_ != nullptr) std::fclose(file_);
}

Status SequentialFile::Read(size_t n, Slice* result, char* scratch) {
  const size_t r = std::fread(scratch, 1, n, file_);
  *result = Slice(scratch, r);
  if (r < n && std::ferror(file_)) return PosixError(filename_, errno);
  return Status::OK();  // short read == EOF, not an error
}

Status SequentialFile::Skip(uint64_t n) {
  if (std::fseek(file_, static_cast<long>(n), SEEK_CUR) != 0) {
    return PosixError(filename_, errno);
  }
  return Status::OK();
}

// --- RandomAccessFile ------------------------------------------------------

Status RandomAccessFile::Open(const std::string& fname,
                              std::unique_ptr<RandomAccessFile>* result) {
#if defined(_WIN32)
  // FILE_SHARE_DELETE is the important flag here and it has no equivalent in
  // the CRT's _open.
  //
  // POSIX lets you unlink a file while readers still have it open; the data
  // stays alive until the last descriptor closes. Windows does NOT, unless
  // every opener passed FILE_SHARE_DELETE. Without it, compaction's attempt to
  // remove an input file fails with a sharing violation whenever any reader
  // still holds it, obsolete SSTables pile up, and disk usage grows without
  // bound - on Windows only, while Linux and macOS CI stay green.
  //
  // FILE_FLAG_RANDOM_ACCESS tells the cache manager not to read ahead, which
  // is right for index-driven block reads.
  HANDLE handle = CreateFileA(
      fname.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status::IOError(fname, "CreateFile failed");
  }
  result->reset(new RandomAccessFile(handle, fname));
  return Status::OK();
#else
  const int fd = ::open(fname.c_str(), O_RDONLY);
  if (fd < 0) return PosixError(fname, errno);
  result->reset(new RandomAccessFile(fd, fname));
  return Status::OK();
#endif
}

RandomAccessFile::~RandomAccessFile() {
#if defined(_WIN32)
  if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(handle_));
  }
#else
  if (fd_ >= 0) ::close(fd_);
#endif
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, Slice* result,
                              char* scratch) const {
  // Positional reads that do not touch any shared file offset, so concurrent
  // readers cannot interfere. pread on POSIX; an OVERLAPPED ReadFile on
  // Windows. Using seek-then-read would be a data race waiting to happen.
#if defined(_WIN32)
  DWORD bytes_read = 0;
  OVERLAPPED overlapped = {};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);

  if (!ReadFile(static_cast<HANDLE>(handle_), scratch, static_cast<DWORD>(n),
                &bytes_read, &overlapped)) {
    // Reading right up to end-of-file reports ERROR_HANDLE_EOF rather than a
    // short read; that is not an error for us.
    if (GetLastError() != ERROR_HANDLE_EOF) {
      *result = Slice(scratch, 0);
      return Status::IOError(filename_, "ReadFile failed");
    }
  }
  *result = Slice(scratch, static_cast<size_t>(bytes_read));
  return Status::OK();
#else
  ssize_t total = 0;
  while (static_cast<size_t>(total) < n) {
    const ssize_t r = ::pread(fd_, scratch + total, n - static_cast<size_t>(total),
                              static_cast<off_t>(offset) + total);
    if (r < 0) {
      if (errno == EINTR) continue;
      *result = Slice(scratch, 0);
      return PosixError(filename_, errno);
    }
    if (r == 0) break;  // end of file
    total += r;
  }
  *result = Slice(scratch, static_cast<size_t>(total));
  return Status::OK();
#endif
}

// --- filesystem helpers ----------------------------------------------------

bool FileExists(const std::string& fname) {
#if defined(_WIN32)
  return _access(fname.c_str(), 0) == 0;
#else
  return ::access(fname.c_str(), F_OK) == 0;
#endif
}

Status CreateDir(const std::string& dirname) {
#if defined(_WIN32)
  if (_mkdir(dirname.c_str()) != 0 && errno != EEXIST) {
    return PosixError(dirname, errno);
  }
#else
  if (::mkdir(dirname.c_str(), 0755) != 0 && errno != EEXIST) {
    return PosixError(dirname, errno);
  }
#endif
  return Status::OK();
}

Status GetFileSize(const std::string& fname, uint64_t* size) {
  std::FILE* f = std::fopen(fname.c_str(), "rb");
  if (f == nullptr) return PosixError(fname, errno);
  std::fseek(f, 0, SEEK_END);
  const long pos = std::ftell(f);
  std::fclose(f);
  if (pos < 0) return PosixError(fname, errno);
  *size = static_cast<uint64_t>(pos);
  return Status::OK();
}

Status RemoveFile(const std::string& fname) {
  if (std::remove(fname.c_str()) != 0) return PosixError(fname, errno);
  return Status::OK();
}

Status GetChildren(const std::string& dir, std::vector<std::string>* result) {
  result->clear();
#if defined(_WIN32)
  WIN32_FIND_DATAA fd;
  const std::string pattern = dir + "\\*";
  HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return Status::IOError(dir, "FindFirstFile failed");
  do {
    result->emplace_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return PosixError(dir, errno);
  struct dirent* entry;
  while ((entry = ::readdir(d)) != nullptr) {
    result->emplace_back(entry->d_name);
  }
  ::closedir(d);
#endif
  return Status::OK();
}

Status RenameFile(const std::string& src, const std::string& dst) {
#if defined(_WIN32)
  // std::rename fails on Windows when dst exists; MoveFileEx with
  // MOVEFILE_REPLACE_EXISTING gives POSIX semantics.
  if (!MoveFileExA(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    return Status::IOError(src, "MoveFileEx failed");
  }
#else
  if (std::rename(src.c_str(), dst.c_str()) != 0) {
    return PosixError(src, errno);
  }
#endif
  return Status::OK();
}

Status TruncateFile(const std::string& fname, uint64_t n) {
#if defined(_WIN32)
  const int fd = _open(fname.c_str(), _O_RDWR | _O_BINARY);
  if (fd < 0) return PosixError(fname, errno);
  const int rc = _chsize_s(fd, static_cast<__int64>(n));
  _close(fd);
  if (rc != 0) return PosixError(fname, rc);
#else
  if (::truncate(fname.c_str(), static_cast<off_t>(n)) != 0) {
    return PosixError(fname, errno);
  }
#endif
  return Status::OK();
}

}  // namespace sextant::lsm
