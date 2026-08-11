// Database file naming.
//
// Files are numbered from a single monotonic counter, so a file number tells
// you the order in which files were created. That ordering is load-bearing:
// L0 SSTables overlap in key range, so on a read they must be consulted
// NEWEST FIRST or a stale value shadows a fresh one.
//
//   000007.log        write-ahead log
//   000012.sst        sorted string table
//   DESCRIPTOR        which files are live, and the last sequence number
//   DESCRIPTOR.tmp    staging file for an atomic descriptor update

#pragma once

#include <cstdint>
#include <string>

namespace sextant::lsm {

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string DescriptorFileName(const std::string& dbname);
std::string TempDescriptorFileName(const std::string& dbname);

enum class FileType { kLog, kTable, kDescriptor, kUnknown };

// Parse "000012.sst" into (12, kTable). Returns false if the name is not one
// of ours - recovery uses this to scan a directory.
bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type);

}  // namespace sextant::lsm
