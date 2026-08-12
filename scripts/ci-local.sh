#!/usr/bin/env bash
#
# Run every CI job locally, on Linux or WSL, before pushing.
#
# This does NOT execute the GitHub workflow YAML. It runs the same COMMANDS the
# workflow runs, which is what actually matters and is far faster than spinning
# up containers.
#
# Covers all four jobs:
#   Include hygiene
#   Build + test        (RelWithDebInfo)
#   ASan + UBSan        (Debug)
#   ThreadSanitizer     (concurrency tests only)
#
# The sanitizer jobs are the valuable ones. They found the memtable
# use-after-free and the shutdown-mid-compaction resurrection, neither of which
# the release build reproduces. Run them before calling anything finished.
#
# Uses build-linux/ rather than build/ on purpose. A CMake build directory
# hardcodes absolute paths, so a build/ created by Windows contains C:/... and a
# Linux run that reuses it fails with "could not find requested file". Keeping
# one build dir per platform avoids the collision entirely - which matters here
# because the repo is edited from Windows and built from WSL.
#
# Usage:
#     ./scripts/ci-local.sh           # everything
#     ./scripts/ci-local.sh fast      # skip sanitizers

set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

# One shared dependency cache across all three build directories. Without this
# GoogleTest is fetched and built three times, which is most of the runtime.
# Override with SEXTANT_DEPS_DIR if the repo lives on a filesystem that cannot
# do git checkouts (network mounts, some container bind mounts).
DEPS_DIR="${SEXTANT_DEPS_DIR:-$PWD/.deps}"
CMAKE_COMMON=(-DFETCHCONTENT_BASE_DIR="$DEPS_DIR")

# Bare `cmake --build --parallel` uses one compile job per core. Each test
# translation unit pulls in GoogleTest and costs several hundred MB, so on a
# machine with many cores and little RAM the OOM killer takes out cc1plus and
# you get the deeply unhelpful "Killed signal terminated program cc1plus".
# Default to cores, but let a small machine dial it down: SEXTANT_JOBS=2
JOBS="${SEXTANT_JOBS:-$(nproc 2>/dev/null || echo 4)}"

FAILED=()
FAST="${1:-}"

# A CMake build directory records the absolute path it was generated from. If
# that path no longer matches - the repo moved, or the same folder was built
# from a different mount (Windows C:\... vs WSL /mnt/c/..., or a container bind
# mount) - CMake refuses to reconfigure and every later step fails with a
# confusing message about CMakeCache.txt.
#
# Detect it and wipe the directory rather than making the human decode it.
drop_stale_cache() {
  local dir="$1"
  local cache="$dir/CMakeCache.txt"
  [[ -f "$cache" ]] || return 0

  local home
  home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache" | head -1)"
  if [[ -n "$home" && "$home" != "$PWD" ]]; then
    printf '\033[1;33m  stale build dir: %s was generated from %s, removing\033[0m\n' \
      "$dir" "$home"
    rm -rf "$dir"
  fi
}

for d in build-linux build-asan build-tsan; do drop_stale_cache "$d"; done

step() {
  local name="$1"; shift
  printf '\n\033[1;36m=== %s ===\033[0m\n' "$name"
  if "$@"; then
    printf '\033[1;32mPASS  %s\033[0m\n' "$name"
  else
    printf '\033[1;31mFAIL  %s\033[0m\n' "$name"
    FAILED+=("$name")
  fi
}

step "Include hygiene" python3 scripts/check_includes.py

step "Configure" \
  cmake -B build-linux -DCMAKE_BUILD_TYPE=RelWithDebInfo "${CMAKE_COMMON[@]}"
step "Build" \
  cmake --build build-linux --parallel "$JOBS"
step "Test" \
  ctest --test-dir build-linux --output-on-failure --no-tests=error

if [[ "$FAST" != "fast" ]]; then
  step "Configure with ASan + UBSan" \
    cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSEXTANT_ASAN=ON "${CMAKE_COMMON[@]}"
  step "Build with ASan" \
    cmake --build build-asan --parallel "$JOBS"
  step "Test under ASan" \
    ctest --test-dir build-asan --output-on-failure --no-tests=error

  step "Configure with TSan" \
    cmake -B build-tsan -DCMAKE_BUILD_TYPE=Debug "${CMAKE_COMMON[@]}" \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
  step "Build with TSan" \
    cmake --build build-tsan --parallel "$JOBS"

  # Inside a container TSan can abort at startup with
  #   FATAL: ThreadSanitizer: unexpected memory mapping
  # because of ASLR. Disabling ASLR for the run fixes it; GitHub's runners do
  # not need this, but WSL and Docker often do.
  printf '\n\033[1;36m=== Concurrency tests under TSan ===\033[0m\n'
  if setarch "$(uname -m)" -R ./build-tsan/tests/test_skiplist; then
    printf '\033[1;32mPASS  TSan concurrency\033[0m\n'
  else
    printf '\033[1;31mFAIL  TSan concurrency\033[0m\n'
    FAILED+=("TSan concurrency")
  fi
fi

printf '\n'
if [[ ${#FAILED[@]} -eq 0 ]]; then
  printf '\033[1;32mAll local CI steps passed.\033[0m\n'
  exit 0
fi
printf '\033[1;31mFAILED: %s\033[0m\n' "${FAILED[*]}"
exit 1
