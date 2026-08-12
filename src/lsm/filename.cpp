#include "filename.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace sextant::lsm {
namespace {

std::string MakeFileName(const std::string& dbname, uint64_t number,
                         const char* suffix) {
  char buf[100];
  // Zero-padded so that lexicographic filename order matches numeric order -
  // convenient when listing a directory by hand.
  std::snprintf(buf, sizeof(buf), "/%06llu.%s",
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "sst");
}

std::string DescriptorFileName(const std::string& dbname) {
  return dbname + "/DESCRIPTOR";
}

std::string TempDescriptorFileName(const std::string& dbname) {
  return dbname + "/DESCRIPTOR.tmp";
}

std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  char buf[100];
  std::snprintf(buf, sizeof(buf), "/MANIFEST-%06llu",
                static_cast<unsigned long long>(number));
  return dbname + buf;
}

std::string CurrentFileName(const std::string& dbname) { return dbname + "/CURRENT"; }

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "dbtmp");
}

bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type) {
  if (filename == "DESCRIPTOR") {
    *number = 0;
    *type = FileType::kDescriptor;
    return true;
  }
  if (filename == "CURRENT") {
    *number = 0;
    *type = FileType::kCurrent;
    return true;
  }
  if (filename.rfind("MANIFEST-", 0) == 0) {
    uint64_t value = 0;
    const std::string digits = filename.substr(9);
    if (digits.empty()) return false;
    for (char c : digits) {
      if (c < '0' || c > '9') return false;
      value = value * 10 + static_cast<uint64_t>(c - '0');
    }
    *number = value;
    *type = FileType::kManifest;
    return true;
  }

  const size_t dot = filename.rfind('.');
  if (dot == std::string::npos || dot == 0) return false;

  const std::string digits = filename.substr(0, dot);
  const std::string suffix = filename.substr(dot + 1);

  uint64_t value = 0;
  for (char c : digits) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  if (digits.empty()) return false;

  if (suffix == "log") {
    *type = FileType::kLog;
  } else if (suffix == "sst") {
    *type = FileType::kTable;
  } else if (suffix == "dbtmp") {
    *type = FileType::kTemp;
  } else {
    return false;
  }

  *number = value;
  return true;
}

}  // namespace sextant::lsm
