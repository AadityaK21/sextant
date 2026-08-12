cmake_minimum_required(VERSION 3.20)

project(sextant
  VERSION 0.1.0
  DESCRIPTION "Mini-Foundry: ontology, entity resolution and cell-level lineage over an LSM engine"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(SEXTANT_BUILD_TESTS  "Build the test suite" ON)
option(SEXTANT_BUILD_BENCH  "Build benchmarks"     ON)
option(SEXTANT_ASAN         "Enable AddressSanitizer + UBSan" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  # MSVC deprecates fopen, strerror and friends in favour of Microsoft-only _s
  # variants. We use the standard C library on purpose - it is what makes this
  # build on Linux, macOS and Windows from one source tree - so the warning is
  # noise rather than signal. NOMINMAX and WIN32_LEAN_AND_MEAN are set globally
  # for the same reason they are set in env.cpp: <windows.h> is hostile to
  # portable code by default.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)
endif()

if(SEXTANT_ASAN AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src/lsm)
add_subdirectory(src/codec)

if(SEXTANT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SEXTANT_BUILD_BENCH)
  add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "sextant ${PROJECT_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  standard   : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  tests      : ${SEXTANT_BUILD_TESTS}")
message(STATUS "  bench      : ${SEXTANT_BUILD_BENCH}")
message(STATUS "  asan/ubsan : ${SEXTANT_ASAN}")
message(STATUS "")
