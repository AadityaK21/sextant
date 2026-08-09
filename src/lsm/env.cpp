#include "env.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
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

Status DeleteFile(const std::string& fname) {
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
