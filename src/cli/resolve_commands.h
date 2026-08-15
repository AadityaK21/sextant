// The two entity-resolution subcommands, kept out of main.cpp because between
// them they are longer than the rest of the CLI put together.
//
//   sextant eval     score the golden set, report the confusion matrix and F1
//   sextant resolve  run the whole pipeline and write entities with provenance

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sextant::cli {

// The parsed command line, shared with main.cpp.
struct Args {
  std::string command;
  std::map<std::string, std::string> flags;
  std::vector<std::string> positional;

  bool Has(const std::string& name) const;
  std::string Get(const std::string& name, const std::string& fallback = {}) const;
  uint64_t GetU64(const std::string& name, uint64_t fallback) const;
};

int CmdEval(const Args& args);
int CmdResolve(const Args& args);

}  // namespace sextant::cli
