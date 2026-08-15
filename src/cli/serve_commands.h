// The two query-engine subcommands.
//
//   sextant query  run a traversal from the command line and print the plan
//   sextant serve  expose the same engine over HTTP
//
// `query` exists because a plan and its cost are worth being able to see
// without a browser and without curl. It is also the fastest way to check that
// a schema change did not quietly turn a TIDX range back into a scan.

#pragma once

#include "resolve_commands.h"

namespace sextant::cli {

int CmdQuery(const Args& args);
int CmdServe(const Args& args);

}  // namespace sextant::cli
