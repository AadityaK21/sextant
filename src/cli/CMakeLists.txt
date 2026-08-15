add_executable(sextant main.cpp resolve_commands.cpp)

target_link_libraries(sextant PRIVATE sextant_lineage sextant_warnings)
target_include_directories(sextant PRIVATE
  ${CMAKE_SOURCE_DIR}/src/lsm
  ${CMAKE_SOURCE_DIR}/src/codec
  ${CMAKE_SOURCE_DIR}/src/ontology
  ${CMAKE_SOURCE_DIR}/src/connectors
  ${CMAKE_SOURCE_DIR}/src/resolve
  ${CMAKE_SOURCE_DIR}/src/lineage
  ${CMAKE_SOURCE_DIR}/include)
