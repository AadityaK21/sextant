add_executable(sextant main.cpp)

target_link_libraries(sextant PRIVATE sextant_resolve sextant_warnings)
target_include_directories(sextant PRIVATE
  ${CMAKE_SOURCE_DIR}/src/lsm
  ${CMAKE_SOURCE_DIR}/src/codec
  ${CMAKE_SOURCE_DIR}/src/ontology
  ${CMAKE_SOURCE_DIR}/src/connectors
  ${CMAKE_SOURCE_DIR}/src/resolve
  ${CMAKE_SOURCE_DIR}/include)
