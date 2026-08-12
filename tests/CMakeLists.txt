include(GoogleTest)

function(sextant_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE sextant_lsm GTest::gtest_main)
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
  target_link_libraries(${name} PRIVATE sextant_codec GTest::gtest_main)
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
