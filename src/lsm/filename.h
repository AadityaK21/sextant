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

// Day 4. The MANIFEST is a numbered, append-only log of VersionEdits; CURRENT
// is a one-line file naming the MANIFEST in force.
//
// The indirection matters for crash safety. Switching to a new MANIFEST means
// writing the new one in full, fsyncing it, then atomically renaming a temp
// file over CURRENT. A crash before the rename leaves the old MANIFEST in
// force and the new one as garbage; a crash after leaves the new one. There is
// no instant at which CURRENT names a MANIFEST that is not fully on disk.
std::string ManifestFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);

enum class FileType { kLog, kTable, kDescriptor, kManifest, kCurrent, kTemp, kUnknown };

// Parse "000012.sst" into (12, kTable). Returns false if the name is not one
// of ours - recovery uses this to scan a directory.
bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type);

}  // namespace sextant::lsm
