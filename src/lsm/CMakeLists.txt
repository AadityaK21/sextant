add_library(sextant_lsm
  status.cpp
  coding.cpp
  crc32c.cpp
  arena.cpp
  internal_key.cpp
  memtable.cpp
  env.cpp
  wal.cpp
  write_batch.cpp
  db_impl.cpp
)

target_include_directories(sextant_lsm
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sextant_lsm PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(sextant_lsm PUBLIC Threads::Threads)

add_library(sextant::lsm ALIAS sextant_lsm)
