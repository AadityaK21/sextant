include(GoogleTest)

function(sextant_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sextant_lsm sextant_warnings GTest::gtest_main)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lsm
    ${CMAKE_SOURCE_DIR}/include)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

sextant_add_test(test_coding        lsm/test_coding.cpp)
sextant_add_test(test_crc32c        lsm/test_crc32c.cpp)
sextant_add_test(test_skiplist      lsm/test_skiplist.cpp)
sextant_add_test(test_internal_key  lsm/test_internal_key.cpp)
sextant_add_test(test_memtable      lsm/test_memtable.cpp)
sextant_add_test(test_wal           lsm/test_wal.cpp)
sextant_add_test(test_block         lsm/test_block.cpp)
sextant_add_test(test_bloom         lsm/test_bloom.cpp)
sextant_add_test(test_cache         lsm/test_cache.cpp)
sextant_add_test(test_table         lsm/test_table.cpp)
sextant_add_test(test_iteration     lsm/test_iteration.cpp)
sextant_add_test(test_db            lsm/test_db.cpp)
sextant_add_test(test_flush         lsm/test_flush.cpp)
sextant_add_test(test_compaction    lsm/test_compaction.cpp)
sextant_add_test(test_db_diff       lsm/test_db_differential.cpp)

# --- day 5: the keyspace codec ---
function(sextant_add_codec_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sextant_codec sextant_warnings GTest::gtest_main)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/codec
    ${CMAKE_SOURCE_DIR}/src/lsm
    ${CMAKE_SOURCE_DIR}/include)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

sextant_add_codec_test(test_ulid      codec/test_ulid.cpp)
sextant_add_codec_test(test_ordered   codec/test_ordered.cpp)
sextant_add_codec_test(test_keyspace  codec/test_keyspace.cpp)
sextant_add_codec_test(test_store     codec/test_store.cpp)

# --- days 6-7: the ontology and the connectors ---
#
# These tests read the schema files and the committed data samples, and ctest
# runs them from the build directory. Baking the source path in is more robust
# than a relative path that only works when the build tree sits in one place -
# and it means a failure says which file it could not find.
function(sextant_add_ontology_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sextant_ontology sextant_warnings
                                        GTest::gtest_main)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/ontology
    ${CMAKE_SOURCE_DIR}/src/codec
    ${CMAKE_SOURCE_DIR}/src/lsm
    ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(${name} PRIVATE SEXTANT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

function(sextant_add_connector_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sextant_connectors sextant_warnings
                                        GTest::gtest_main)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/connectors
    ${CMAKE_SOURCE_DIR}/src/ontology
    ${CMAKE_SOURCE_DIR}/src/codec
    ${CMAKE_SOURCE_DIR}/src/lsm
    ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(${name} PRIVATE SEXTANT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

sextant_add_ontology_test(test_value      ontology/test_value.cpp)
sextant_add_ontology_test(test_transform  ontology/test_transform.cpp)
sextant_add_ontology_test(test_schema     ontology/test_schema.cpp)
sextant_add_ontology_test(test_mapping    ontology/test_mapping.cpp)

sextant_add_connector_test(test_csv     connectors/test_csv.cpp)
sextant_add_connector_test(test_json    connectors/test_json.cpp)
sextant_add_connector_test(test_ingest  connectors/test_ingest.cpp)

# --- day 8: normalization and blocking ---
function(sextant_add_resolve_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sextant_resolve sextant_warnings
                                        GTest::gtest_main)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/resolve
    ${CMAKE_SOURCE_DIR}/src/connectors
    ${CMAKE_SOURCE_DIR}/src/ontology
    ${CMAKE_SOURCE_DIR}/src/codec
    ${CMAKE_SOURCE_DIR}/src/lsm
    ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(${name} PRIVATE SEXTANT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30)
endfunction()

sextant_add_resolve_test(test_normalize  resolve/test_normalize.cpp)
sextant_add_resolve_test(test_geohash    resolve/test_geohash.cpp)
sextant_add_resolve_test(test_blocking   resolve/test_blocking.cpp)
sextant_add_resolve_test(test_similarity resolve/test_similarity.cpp)
sextant_add_resolve_test(test_resolution resolve/test_resolution.cpp)

# --- days 11-12: lineage ---
function(sextant_add_lineage_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sextant_lineage sextant_warnings
                                        GTest::gtest_main)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lineage
    ${CMAKE_SOURCE_DIR}/src/resolve
    ${CMAKE_SOURCE_DIR}/src/connectors
    ${CMAKE_SOURCE_DIR}/src/ontology
    ${CMAKE_SOURCE_DIR}/src/codec
    ${CMAKE_SOURCE_DIR}/src/lsm
    ${CMAKE_SOURCE_DIR}/include)
  target_compile_definitions(${name} PRIVATE SEXTANT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60)
endfunction()

sextant_add_lineage_test(test_lineage lineage/test_lineage.cpp)
