cmake_minimum_required(VERSION 3.25)
project(realontext LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(libgit2 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(roaring CONFIG REQUIRED)
find_package(BLAKE3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-tree-sitter CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(usearch CONFIG REQUIRED)

include(cmake/grammars.cmake)

add_compile_options(-Wall -Wextra)

add_library(ingest STATIC src/ingest/git.cpp src/ingest/filter.cpp)
target_include_directories(ingest PUBLIC src)
target_link_libraries(ingest PUBLIC libgit2::libgit2package)

add_library(parse STATIC src/parse/chunk.cpp src/parse/lang.cpp)
target_include_directories(parse PUBLIC src)
target_link_libraries(parse PUBLIC unofficial::tree-sitter::tree-sitter PRIVATE grammars)

add_library(model STATIC src/model/model.cpp)
target_include_directories(model PUBLIC src)
target_link_libraries(model PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)

add_library(store STATIC src/store/store.cpp src/store/embed.cpp)
target_include_directories(store PUBLIC src)
target_link_libraries(store PUBLIC parse model unofficial::sqlite3::sqlite3 roaring::roaring roaring::roaring-headers-cpp PRIVATE BLAKE3::blake3)

add_library(lexical STATIC src/lexical/tokenizer.cpp src/lexical/lexical.cpp)
target_include_directories(lexical PUBLIC src)
target_link_libraries(lexical PUBLIC store)

add_library(vector STATIC src/vector/vector.cpp)
target_include_directories(vector PUBLIC src)
target_link_libraries(vector PUBLIC store PRIVATE usearch::usearch)

add_library(match STATIC src/match/match.cpp)
target_include_directories(match PUBLIC src)
target_link_libraries(match PUBLIC lexical store ingest vector)

add_executable(realontext src/main.cpp)
target_link_libraries(realontext PRIVATE ingest match vector nlohmann_json::nlohmann_json)

enable_testing()
add_executable(unit tests/unit.cpp)
target_link_libraries(unit PRIVATE parse lexical ingest match vector)
add_test(NAME unit COMMAND unit)
