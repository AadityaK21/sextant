#include "env.h"

#include <cerrno>
#include <cstring>

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
  const int fd = _open(fname.c_str(), _O_RDONLY | _O_BINARY);
#else
  const int fd = ::open(fname.c_str(), O_RDONLY);
#endif
  if (fd < 0) return PosixError(fname, errno);
  result->reset(new RandomAccessFile(fd, fname));
  return Status::OK();
}

RandomAccessFile::~RandomAccessFile() {
  if (fd_ >= 0) {
#if defined(_WIN32)
    _close(fd_);
#else
    ::close(fd_);
#endif
  }
}

Status RandomAccessFile::Read(uint64_t offset, size_t n, Slice* result,
                              char* scratch) const {
  // pread / _lseeki64+_read on a private fd: no shared file position is
  // mutated, so concurrent readers do not interfere. Using fseek+fread here
  // would be a data race waiting to happen.
#if defined(_WIN32)
  if (_lseeki64(fd_, static_cast<__int64>(offset), SEEK_SET) < 0) {
    *result = Slice(scratch, 0);
    return PosixError(filename_, errno);
  }
  const int r = _read(fd_, scratch, static_cast<unsigned int>(n));
  if (r < 0) {
    *result = Slice(scratch, 0);
    return PosixError(filename_, errno);
  }
  *result = Slice(scratch, static_cast<size_t>(r));
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
