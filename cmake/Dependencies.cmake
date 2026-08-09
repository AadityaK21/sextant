# All third-party dependencies are fetched at configure time and version-pinned.
# Rationale: no system packages, reproducible on a clean machine and in CI.
# See docs/adr/0001-dependency-management.md

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

# --- Dependencies added in later milestones --------------------------------
# Kept here (commented) so the build stays fast during LSM development.
# Uncomment as each milestone lands — see docs/EXECUTION_PLAN.md.
#
# Day 6  — ontology schema loader
#   FetchContent_Declare(yaml-cpp
#     GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
#     GIT_TAG 0.8.0 GIT_SHALLOW TRUE)
#
# Day 7  — HTTP connector (Digitraffic) and, later, the API server
#   FetchContent_Declare(httplib
#     GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
#     GIT_TAG v0.18.3 GIT_SHALLOW TRUE)
#   FetchContent_Declare(nlohmann_json
#     GIT_REPOSITORY https://github.com/nlohmann/json.git
#     GIT_TAG v3.11.3 GIT_SHALLOW TRUE)
#
# Day 7  — Postgres connector.  libpqxx needs libpq; prefer the system package:
#   find_package(PostgreSQL REQUIRED)
#
# Any day  — structured logging and CLI subcommands
#   FetchContent_Declare(spdlog ...)   FetchContent_Declare(CLI11 ...)
