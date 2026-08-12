add_executable(lsm_bench lsm_bench.cpp)
target_link_libraries(lsm_bench PRIVATE sextant_lsm sextant_warnings)
target_include_directories(lsm_bench PRIVATE
  ${CMAKE_SOURCE_DIR}/src/lsm
  ${CMAKE_SOURCE_DIR}/include)
