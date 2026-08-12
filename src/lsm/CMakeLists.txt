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
  iterator.cpp
  format.cpp
  block_builder.cpp
  block.cpp
  bloom.cpp
  filter_block.cpp
  cache.cpp
  table_builder.cpp
  table.cpp
  table_cache.cpp
  merger.cpp
  db_iter.cpp
  filename.cpp
  version_edit.cpp
  version_set.cpp
  db_impl.cpp
)

target_include_directories(sextant_lsm
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sextant_lsm PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(sextant_lsm PUBLIC Threads::Threads PRIVATE sextant_warnings)

add_library(sextant::lsm ALIAS sextant_lsm)
