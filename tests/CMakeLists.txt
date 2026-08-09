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
sextant_add_test(test_db            lsm/test_db.cpp)
sextant_add_test(test_db_diff       lsm/test_db_differential.cpp)
