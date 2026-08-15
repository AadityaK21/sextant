add_library(sextant_api
  json.cpp
  server.cpp
)

target_include_directories(sextant_api
  PUBLIC  ${CMAKE_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_SOURCE_DIR}/src/lsm
)

target_compile_features(sextant_api PUBLIC cxx_std_20)

# nlohmann is PUBLIC here, unlike everywhere else in the tree: json.h returns
# nlohmann::json by value, so anything including it needs the header. That is a
# deliberate trade - the alternative is a hand-rolled JSON type that exists only
# to keep a dependency private, which is a worse deal than admitting this one
# layer is a JSON layer.
target_link_libraries(sextant_api
  PUBLIC  sextant_query sextant_lineage nlohmann_json::nlohmann_json
  PRIVATE sextant_httplib sextant_warnings)

add_library(sextant::api ALIAS sextant_api)
