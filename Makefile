# One command to a running system.
#
# WHY A MAKEFILE IN A CMAKE PROJECT
#
# CMake builds the C++. It does not know how to ingest a corpus, run entity
# resolution, build a frontend or start a server, and teaching it to would mean
# expressing a shell script in CMake's language for no benefit.
#
# This is a task runner, not a build system. Every target is a handful of
# commands anyone could type; the value is that they are written down, in order,
# and stay correct because CI runs them.
#
# Windows users: this needs GNU make. Everything here is also a plain command
# sequence documented in the README, so nothing is only reachable through make.

BUILD_DIR   ?= build
BUILD_TYPE  ?= RelWithDebInfo
DB          ?= sextant-db
PORT        ?= 8080
JOBS        ?= $(shell (nproc || sysctl -n hw.ncpu || echo 4) 2>/dev/null)

SEXTANT := $(BUILD_DIR)/src/cli/sextant

# MSVC is a multi-config generator, so the binary lands one directory deeper.
ifeq ($(OS),Windows_NT)
SEXTANT := $(BUILD_DIR)/src/cli/$(BUILD_TYPE)/sextant.exe
endif

.PHONY: help build test demo ingest resolve serve web clean distclean check bench verify

help:
	@echo "sextant"
	@echo ""
	@echo "  make demo        build, ingest, resolve, build the UI, serve on :$(PORT)"
	@echo "  make build       configure and compile"
	@echo "  make test        the full C++ suite"
	@echo "  make verify      test + the lineage round trip + the API contract"
	@echo "  make web         install and build the frontend"
	@echo "  make serve       serve an already-built database"
	@echo "  make bench       the storage benchmarks"
	@echo "  make clean       remove the demo database, keep the build"
	@echo "  make distclean   remove everything generated"
	@echo ""
	@echo "  BUILD_TYPE=$(BUILD_TYPE)  DB=$(DB)  PORT=$(PORT)  JOBS=$(JOBS)"

build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
	cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) -j $(JOBS)

test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -j $(JOBS)

# Ingest is idempotent: an unchanged input is detected by fingerprint and
# skipped, so re-running this is a no-op rather than a duplicate load.
ingest: build
	$(SEXTANT) schema
	$(SEXTANT) ingest --source wpi            --db $(DB)
	$(SEXTANT) ingest --source unlocode       --db $(DB)
	$(SEXTANT) ingest --source digitraffic    --db $(DB)
	$(SEXTANT) ingest --source digitraffic_ais --db $(DB)
	$(SEXTANT) stats --db $(DB)

# Resolution is a full recompute and clears its own derived keyspaces first,
# so this is safe to re-run. See docs/BUGS.md for what happened before it did.
resolve: ingest
	$(SEXTANT) block   --db $(DB)
	$(SEXTANT) eval    --db $(DB)
	$(SEXTANT) resolve --db $(DB)
	$(SEXTANT) explain --db $(DB)

web:
	cd web && npm install && npm run build

serve:
	$(SEXTANT) serve --db $(DB) --port $(PORT) --static web/dist

# THE ONE COMMAND.
demo: resolve web
	@echo ""
	@echo "  ---------------------------------------------------------------"
	@echo "   Sextant is ready.  http://localhost:$(PORT)"
	@echo ""
	@echo "   Try: search a port, open it, click 'lineage' on any value."
	@echo "  ---------------------------------------------------------------"
	@echo ""
	$(MAKE) serve

# Everything that can fail, in one target. This is what CI runs.
verify: test
	$(SEXTANT) explain --db $(DB) 2>/dev/null || \
	  (echo "no database yet; run 'make resolve' first" && exit 1)
	@echo ""
	@echo "starting a server to check the API contract..."
	@$(SEXTANT) serve --db $(DB) --port 18999 --static web/dist & \
	  SERVER=$$!; sleep 3; \
	  (cd web && node scripts/check-api-contract.mjs http://127.0.0.1:18999); \
	  RESULT=$$?; kill $$SERVER 2>/dev/null; exit $$RESULT

bench: build
	$(BUILD_DIR)/bench/lsm_bench 200000 100

clean:
	rm -rf $(DB)

distclean: clean
	rm -rf $(BUILD_DIR) web/node_modules web/dist web/*.tsbuildinfo
