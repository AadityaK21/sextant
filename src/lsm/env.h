// Minimal file abstraction.
//
// Kept deliberately thin: the engine needs sequential append with an explicit
// durability barrier, and sequential read. Everything else is out of scope.
//
// The one subtlety worth knowing: Flush() and Sync() are different operations.
// Flush pushes bytes from our userspace buffer into the OS page cache — after
// which the data survives a *process* crash but not a *machine* crash. Sync
// forces the OS to push them to the device, which is the only thing that
// survives power loss. WAL group-commit exists precisely because Sync is
// expensive (a millisecond or more on spinning rust, tens of microseconds on
// NVMe) and we want to amortise it across many writes.

#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "sextant/lsm/slice.h"
#include "sextant/lsm/status.h"

namespace sextant::lsm {

class WritableFile {
 public:
  static Status Open(const std::string& fname, bool append,
                     std::unique_ptr<WritableFile>* result);

  ~WritableFile();
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;

  Status Append(const Slice& data);
  Status Flush();   // userspace buffer -> OS page cache
  Status Sync();    // OS page cache -> device.  Survives power loss.
  Status Close();

  const std::string& filename() const { return filename_; }

 private:
  WritableFile(std::FILE* f, std::string fname)
      : file_(f), filename_(std::move(fname)) {}

  std::FILE* file_;
  std::string filename_;
};

class SequentialFile {
 public:
  static Status Open(const std::string& fname, std::unique_ptr<SequentialFile>* result);

  ~SequentialFile();
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;

  // Reads up to n bytes into scratch and points result at them. A short read
  // (result->size() < n) means end of file.
  Status Read(size_t n, Slice* result, char* scratch);
  Status Skip(uint64_t n);

 private:
  SequentialFile(std::FILE* f, std::string fname)
      : file_(f), filename_(std::move(fname)) {}

  std::FILE* file_;
  std::string filename_;
};

// --- filesystem helpers ----------------------------------------------------

// NAMING HAZARD, learned the hard way — see docs/BUGS.md.
//
// <windows.h> defines a family of API names as PREPROCESSOR MACROS that expand
// to an -A or -W suffixed variant:
//     #define DeleteFile     DeleteFileW
//     #define CreateDirectory CreateDirectoryW
//     #define GetMessage      GetMessageW      ... and dozens more
//
// A macro ignores namespaces. So `sextant::lsm::DeleteFile` defined in a
// translation unit that includes <windows.h> becomes `DeleteFileW`, while every
// caller that never included <windows.h> still references `DeleteFile`. The
// result is a link error that names a function you can see with your own eyes,
// which is why it reads as nonsense at first.
//
// Hence RemoveFile, not DeleteFile. Before adding a function here, check it
// against the Win32 macro list.
bool FileExists(const std::string& fname);
Status CreateDir(const std::string& dirname);
Status GetFileSize(const std::string& fname, uint64_t* size);
Status RemoveFile(const std::string& fname);
Status GetChildren(const std::string& dir, std::vector<std::string>* result);

// Truncate a file to n bytes. Used by the crash-simulation tests to
// manufacture a torn tail without actually killing a process.
Status TruncateFile(const std::string& fname, uint64_t n);

}  // namespace sextant::lsm
