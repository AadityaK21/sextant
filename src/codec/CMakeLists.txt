add_library(sextant_codec
  ulid.cpp
  ordered.cpp
  keyspace.cpp
  store.cpp
)

target_include_directories(sextant_codec
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(sextant_codec PUBLIC cxx_std_20)
target_link_libraries(sextant_codec PUBLIC sextant_lsm)

add_library(sextant::codec ALIAS sextant_codec)
