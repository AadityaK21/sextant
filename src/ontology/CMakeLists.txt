add_library(sextant_ontology
  value.cpp
  transform.cpp
  schema.cpp
  record.cpp
  mapping.cpp
  bundle.cpp
)

target_include_directories(sextant_ontology
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src/lsm
)

target_compile_features(sextant_ontology PUBLIC cxx_std_20)
target_link_libraries(sextant_ontology
  PUBLIC  sextant_codec
  PRIVATE yaml-cpp::yaml-cpp sextant_warnings)

add_library(sextant::ontology ALIAS sextant_ontology)
