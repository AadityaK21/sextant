#!/usr/bin/env python3
"""Check that every source file includes the standard headers it uses.

WHY THIS EXISTS
---------------
libstdc++ used to pull in a great many headers transitively. GCC 13 removed
most of that, so code that compiled fine on GCC 11 and on Clang suddenly fails
on a newer Linux with dozens of "was not declared in this scope" errors.

That is exactly what happened here: 111 missing includes across the tree, all
invisible locally, all fatal on ubuntu-latest. The compiler you develop on is
not the compiler your CI runs.

This is a heuristic, not a real include-what-you-use. It only looks for
well-known std:: symbols and their canonical headers, which is enough to catch
the transitive-include class of failure without needing a compiler at all.

Usage:
    python3 scripts/check_includes.py          # report and exit non-zero
    python3 scripts/check_includes.py --fix    # insert the missing includes
"""

import argparse
import pathlib
import re
import sys

# Symbol pattern -> the header that is guaranteed to declare it.
NEED = {
    "<algorithm>": [r"std::sort\b", r"std::min\b", r"std::max\b", r"std::find\b",
                    r"std::lower_bound\b", r"std::upper_bound\b"],
    "<atomic>": [r"std::atomic\b"],
    "<cassert>": [r"\bassert\("],
    "<chrono>": [r"std::chrono::"],
    "<cmath>": [r"std::pow\b", r"std::exp\b", r"std::log\b", r"std::sqrt\b"],
    "<condition_variable>": [r"std::condition_variable\b"],
    "<cstdint>": [r"\buint\d+_t\b", r"\bint\d+_t\b", r"\bintptr_t\b", r"\buintptr_t\b"],
    "<cstdio>": [r"std::snprintf\b", r"std::fopen\b", r"std::FILE\b", r"std::printf\b",
                 r"std::remove\b", r"std::fprintf\b", r"std::fseek\b"],
    "<cstring>": [r"std::memcpy\b", r"std::memcmp\b", r"std::strlen\b", r"std::strerror\b",
                  r"std::memset\b"],
    "<functional>": [r"std::function<"],
    "<limits>": [r"std::numeric_limits<"],
    "<list>": [r"std::list<"],
    "<map>": [r"std::map<"],
    "<memory>": [r"std::unique_ptr<", r"std::make_unique\b", r"std::shared_ptr<"],
    "<mutex>": [r"std::mutex\b", r"std::lock_guard\b", r"std::unique_lock\b"],
    "<optional>": [r"std::optional<", r"std::nullopt\b"],
    "<random>": [r"std::mt19937"],
    "<set>": [r"std::set<", r"std::multiset<"],
    "<string>": [r"std::string\b"],
    "<thread>": [r"std::this_thread::", r"std::thread\b"],
    "<unordered_map>": [r"std::unordered_map<"],
    "<utility>": [r"std::move\b", r"std::pair<", r"std::swap\b", r"std::forward\b"],
    "<vector>": [r"std::vector<"],
}

ROOTS = ["src", "include", "tests", "bench"]

# Files whose include block is guarded by #if/#else and must not be reordered.
# env.cpp mixes POSIX and Win32 headers; sorting them across the conditional
# once produced "#else without #if".
# Files whose include block must not be reordered, because it spans a
# preprocessor conditional. Sorting across an #if/#else moves the #else above
# the #if and the file stops compiling with an error that points nowhere useful.
# This has now happened twice; a file lands here the first time it does.
SKIP_FIX = {
    "src/lsm/env.cpp",
    "tests/lineage/test_lineage.cpp",
    "tests/resolve/test_blocking.cpp",
    "tests/resolve/test_resolution.cpp",
}


def sources():
    for root in ROOTS:
        base = pathlib.Path(root)
        if not base.exists():
            continue
        for pattern in ("*.cpp", "*.h"):
            yield from sorted(base.rglob(pattern))


def missing_for(path):
    text = path.read_text(encoding="utf-8")
    # Strip line comments so prose does not trigger a match.
    body = "\n".join(l for l in text.splitlines() if not l.strip().startswith("//"))
    have = set(re.findall(r"#include\s+(<[^>]+>)", text))

    result = []
    for header, patterns in NEED.items():
        if header in have:
            continue
        if any(re.search(p, body) for p in patterns):
            result.append(header)
    return sorted(result)


def fix(path, headers):
    lines = path.read_text(encoding="utf-8").split("\n")
    std_idx = [i for i, l in enumerate(lines)
               if re.match(r"#include\s+<", l) and "gtest" not in l]
    if std_idx:
        start, end = std_idx[0], std_idx[-1]
        block = sorted(set(lines[start:end + 1]) | {f"#include {h}" for h in headers})
        lines[start:end + 1] = [b for b in block if b.strip()]
    else:
        anchor = 0
        for i, l in enumerate(lines):
            if l.startswith("#pragma once") or re.match(r'#include\s+"', l):
                anchor = i + 1
        lines[anchor:anchor] = [""] + sorted(f"#include {h}" for h in headers)
    path.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fix", action="store_true", help="insert missing includes")
    args = parser.parse_args()

    problems = 0
    for path in sources():
        headers = missing_for(path)
        if not headers:
            continue
        rel = path.as_posix()
        problems += len(headers)
        print(f"{rel}: missing {', '.join(headers)}")
        if args.fix:
            if rel in SKIP_FIX:
                print(f"  (skipped: {rel} has a conditional include block, fix by hand)")
            else:
                fix(path, headers)

    if problems == 0:
        print("all files include what they use")
        return 0

    if args.fix:
        print(f"\ninserted {problems} includes")
        return 0

    print(f"\n{problems} missing includes. Run with --fix, or add them by hand.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
