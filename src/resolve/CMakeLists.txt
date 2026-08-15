add_library(sextant_resolve
  normalize.cpp
  geohash.cpp
  phonetic.cpp
  similarity.cpp
  golden.cpp
  blocking.cpp
  scorer.cpp
  evaluate.cpp
  cluster.cpp
  fuse.cpp
  link.cpp
)

target_include_directories(sextant_resolve
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src/lsm
)

target_compile_features(sextant_resolve PUBLIC cxx_std_20)
# The connectors library comes along for the CSV reader, which the golden-set
# loader uses. Reaching for the one that already handles quoting, embedded
# newlines and byte-order marks beats writing a second, worse CSV parser for a
# file that a person edits by hand.
target_link_libraries(sextant_resolve
  PUBLIC  sextant_connectors
  PRIVATE yaml-cpp::yaml-cpp sextant_warnings)

add_library(sextant::resolve ALIAS sextant_resolve)
