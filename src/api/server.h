// The HTTP layer over the query engine.
//
// A THREAD PER REQUEST, ON PURPOSE
//
// cpp-httplib blocks a thread for the life of a handler. That is normally the
// thing you apologise for; here it is what makes correctness easy. Every read a
// request performs must happen at one snapshot, and a snapshot must be released
// exactly once when the request ends. With a blocking handler the snapshot is a
// stack object whose destructor is the release, and there is no state machine.
//
// An async server would turn a scope into a lifetime to manage, and would buy
// throughput this does not need: the expensive part of a request is an LSM read
// that is already measured in tens of microseconds.
//
// THE ROUTES ARE THIN
//
// Each one parses, calls the engine, serialises, and returns. Anything longer
// than that belongs in src/query or src/lineage, because those can be tested
// without binding a socket. If a handler here grows a branch worth reasoning
// about, that branch is in the wrong file.
//
// CORS IS DEVELOPMENT-ONLY AND SAYS SO
//
// The Vite dev server runs on a different port, so the browser refuses to read
// responses without an Access-Control-Allow-Origin header. In production the
// frontend is served as static files by this same binary and no CORS header is
// needed at all. Defaulting the allowed origin to localhost rather than `*`
// keeps the development convenience from silently becoming a production
// posture.
//
// WHAT THIS IS NOT
//
// No authentication, no rate limiting, no TLS. A real deployment puts this
// behind something that does all three. Saying so is better than a token-check
// stub that looks like security and is not.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "bundle.h"
#include "execute.h"
#include "lineage.h"
#include "sextant/lsm/status.h"
#include "store.h"

namespace sextant::api {

using lsm::Status;

struct ServerOptions {
  std::string host = "127.0.0.1";
  int port = 8080;

  // Where a source's relative uri is resolved against, for the raw-row reads
  // that lineage does.
  std::string data_root = ".";

  // Development only. Empty disables the header entirely, which is what a
  // same-origin deployment wants.
  std::string cors_origin = "http://localhost:5173";

  // Served at / when set, so one binary can serve the API and the built
  // frontend without a second process.
  std::string static_dir;

  bool log_requests = true;
};

class Server {
 public:
  Server(codec::Store* store, const ontology::SchemaBundle* bundle,
         ServerOptions options);
  ~Server();

  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Blocks until Stop() is called from another thread.
  Status Listen();

  // Bind and start listening, then return. Used by the tests, which need the
  // port before they can send anything to it.
  Status StartBackground(int* bound_port);

  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sextant::api
