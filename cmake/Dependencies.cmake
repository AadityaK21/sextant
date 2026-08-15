# All third-party dependencies are fetched at configure time and version-pinned.
# Rationale: no system packages, reproducible on a clean machine and in CI.
# See docs/adr/0001-dependency-management.md
#
# NOTE ON WARNING FLAGS: this file is included BEFORE the sextant_warnings
# interface target is applied to our own targets, and our warning flags are
# never set globally. Third-party code is compiled with its own defaults. A
# build where -Wall is on for everything produces thousands of diagnostics from
# code you cannot fix, which trains you to ignore the compiler - the opposite of
# what warnings are for.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- GoogleTest ------------------------------------------------------------
if(SEXTANT_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

# --- yaml-cpp: the ontology and mapping schema files -----------------------
#
# 0.9.0 rather than the more commonly pinned 0.8.0 for a concrete reason:
# 0.8.0 declares `cmake_minimum_required(VERSION 3.4)`, and CMake 4.0 removed
# compatibility with anything below 3.5. Pinning 0.8.0 makes the project fail
# to configure on any recent CMake with an error that points at a dependency
# rather than at us. 0.9.0 declares `3.5...3.30` and configures cleanly.
FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG        yaml-cpp-0.9.0
  GIT_SHALLOW    TRUE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# --- nlohmann/json: the REST connector, and later the API server -----------
#
# NOTE ON THE FIRST CONFIGURE BEING SLOW. This repository carries a large
# test-data history, so even a shallow clone pulls tens of megabytes and can
# take several minutes on a slow link. Upstream recommends the release tarball
# instead, which is a few hundred kilobytes:
#
#   URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
#
# That is left commented out because release downloads are blocked on some
# restricted networks that still allow git, and a dependency that fails to
# fetch behind a proxy is worse than one that fetches slowly. If your configure
# step is slow and you can reach the release URL, switch to it.
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# --- Postgres: optional on purpose -----------------------------------------
#
# libpqxx needs libpq, which is a system package rather than something
# FetchContent can pin. Making it required would mean the project does not
# build on a clean Windows machine, so the Postgres connector is compiled only
# when Postgres is actually present. The row-shaping logic it shares with the
# other two connectors lives behind the RowSource interface and is tested with
# a fake, so the untested surface is only the libpq calls themselves.
find_package(PostgreSQL QUIET)
if(PostgreSQL_FOUND)
  set(SEXTANT_WITH_POSTGRES ON)
  message(STATUS "PostgreSQL found - the postgres connector will be built")
else()
  set(SEXTANT_WITH_POSTGRES OFF)
  message(STATUS "PostgreSQL not found - the postgres connector will be stubbed")
endif()

# --- cpp-httplib: the query API's HTTP layer -------------------------------
#
# WHY THIS ONE
#
# It is a single header with no dependencies and a blocking thread-per-request
# model. That last part is usually a criticism and here it is the point: a
# request holds an LSM snapshot for its whole life, and a blocking handler makes
# the snapshot's lifetime exactly the handler's scope. An async server would
# turn that into a lifetime problem to solve for no benefit at this scale.
#
# WHY THERE IS A VENDORED FALLBACK
#
# Every other dependency here is fetched from GitHub, and on a restricted
# network that has already been the slowest and least reliable part of a
# configure. cpp-httplib is ONE header, so vendoring it is genuinely viable in
# a way that vendoring yaml-cpp would not be. If third_party/httplib.h exists,
# it is used and nothing is fetched.
#
# The result either way is an INTERFACE target named `httplib`, so nothing
# downstream knows or cares which path was taken.
# The target is named sextant_httplib rather than httplib because the upstream
# project already defines a target called `httplib`, and CMake refuses two
# targets with one name. Found out the direct way.
add_library(sextant_httplib INTERFACE)

if(EXISTS ${CMAKE_SOURCE_DIR}/third_party/httplib.h)
  message(STATUS "cpp-httplib: using the vendored header, no fetch")
  target_include_directories(sextant_httplib INTERFACE ${CMAKE_SOURCE_DIR}/third_party)
else()
  FetchContent_Declare(cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.3
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(cpp_httplib)
  target_link_libraries(sextant_httplib INTERFACE httplib)
endif()

# Thread support: the server runs a handler per connection.
find_package(Threads REQUIRED)
target_link_libraries(sextant_httplib INTERFACE Threads::Threads)
