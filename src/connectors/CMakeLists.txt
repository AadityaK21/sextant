add_library(sextant_connectors
  csv.cpp
  json_source.cpp
  postgres.cpp
  ingest.cpp
)

target_include_directories(sextant_connectors
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src/lsm
)

target_compile_features(sextant_connectors PUBLIC cxx_std_20)
target_link_libraries(sextant_connectors
  PUBLIC  sextant_ontology
  PRIVATE nlohmann_json::nlohmann_json sextant_warnings)

# Postgres is optional. When libpq is missing the connector still compiles - it
# just returns NotSupported - so the rest of the project does not need to know
# whether this build has a database client in it.
if(SEXTANT_WITH_POSTGRES)
  target_compile_definitions(sextant_connectors PRIVATE SEXTANT_WITH_POSTGRES)
  target_link_libraries(sextant_connectors PRIVATE PostgreSQL::PostgreSQL)
endif()

add_library(sextant::connectors ALIAS sextant_connectors)
