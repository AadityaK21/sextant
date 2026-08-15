add_library(sextant_query
  query.cpp
  plan.cpp
  execute.cpp
)

target_include_directories(sextant_query
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src/lsm
)

target_compile_features(sextant_query PUBLIC cxx_std_20)

# Depends on resolve for ResolvedEntity, which is what an ENTITY value decodes
# into. The query engine reads entities that resolution wrote, so the format is
# a shared one rather than something query defines for itself.
#
# nlohmann is PRIVATE: request parsing lives in query.cpp, and nothing in the
# public headers mentions JSON. The IR is a C++ value, so the executor can be
# tested without going through a serialiser.
target_link_libraries(sextant_query
  PUBLIC  sextant_resolve
  PRIVATE nlohmann_json::nlohmann_json sextant_warnings)

add_library(sextant::query ALIAS sextant_query)
