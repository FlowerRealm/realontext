# tree-sitter grammars. vcpkg carries only the C grammar, so all seven come from
# release tarballs pinned by hash: one code path, and a grammar bump is a visible
# diff (grammar version is part of the chunking scheme, docs/benchmark.md).
include(FetchContent)

function(grammar name version sha256)
  FetchContent_Declare(ts_${name}
    URL https://github.com/tree-sitter/tree-sitter-${name}/archive/refs/tags/${version}.tar.gz
    URL_HASH SHA256=${sha256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR _sources_only) # never run the grammar's own CMakeLists
  FetchContent_MakeAvailable(ts_${name})
  set(grammar_dir_${name} ${ts_${name}_SOURCE_DIR} PARENT_SCOPE)
endfunction()

grammar(c          v0.24.2 2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364)
grammar(cpp        v0.23.4 7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051)
grammar(go         v0.25.0 2dc241b97872c53195e01b86542b411a3c1a6201d9c946c78d5c60c063bba1ef)
grammar(rust       v0.24.2 061e90a539a55a6aa65dceb0ad6425c50ab1a6e3e6d4ba430e2795ed4550f10e)
grammar(typescript v0.23.2 2c4ce711ae8d1218a3b2f899189298159d672870b5b34dff5d937bed2f3e8983)
grammar(python     v0.25.0 4609a3665a620e117acf795ff01b9e965880f81745f287a16336f4ca86cf270c)
grammar(java       v0.23.5 cb199e0faae4b2c08425f88cbb51c1a9319612e7b96315a174a624db9bf3d9f0)

# Each grammar's parser.c includes "tree_sitter/parser.h" relative to its own
# src/, so every source file keeps its own include directory.
set(sources)
macro(grammar_sources dir)
  foreach(f parser.c scanner.c)
    if(EXISTS ${dir}/${f})
      list(APPEND sources ${dir}/${f})
      set_source_files_properties(${dir}/${f} PROPERTIES INCLUDE_DIRECTORIES ${dir})
    endif()
  endforeach()
endmacro()

grammar_sources(${grammar_dir_c}/src)
grammar_sources(${grammar_dir_cpp}/src)
grammar_sources(${grammar_dir_go}/src)
grammar_sources(${grammar_dir_rust}/src)
grammar_sources(${grammar_dir_typescript}/typescript/src)
grammar_sources(${grammar_dir_typescript}/tsx/src)
grammar_sources(${grammar_dir_python}/src)
grammar_sources(${grammar_dir_java}/src)

add_library(grammars STATIC ${sources})
set_target_properties(grammars PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
target_compile_options(grammars PRIVATE -w)
