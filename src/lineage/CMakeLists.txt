add_library(sextant_lineage
  lineage.cpp
)

target_include_directories(sextant_lineage
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src/lsm
)

target_compile_features(sextant_lineage PUBLIC cxx_std_20)
# Depends on resolve for the Provenance and ResolvedEntity formats it reads
# back, and through it on connectors for the CSV and JSON parsers - lineage
# extracts a named cell from a verbatim row, so it needs the same parsers the
# connectors used to read it in the first place.
target_link_libraries(sextant_lineage
  PUBLIC  sextant_resolve
  PRIVATE sextant_warnings)

add_library(sextant::lineage ALIAS sextant_lineage)
