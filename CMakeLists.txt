cmake_minimum_required(VERSION 3.10...4.0)
project(rooture)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ROOT REQUIRED COMPONENTS Core MathCore RIO Hist Tree TreePlayer ROOTVecOps ROOTDataFrame ROOTNTuple)
include(${ROOT_USE_FILE})

find_package(Threads REQUIRED)
find_package(replxx REQUIRED)
find_package(CURL REQUIRED)
find_library(TREE_SITTER_LIB NAMES tree-sitter REQUIRED)
find_path(TREE_SITTER_INCLUDE NAMES tree_sitter/api.h REQUIRED)

# Generate src/parser.c from grammar.js using the tree-sitter CLI.
# The CLI is provided by the tree-sitter alibuild recipe (or a local install).
# The generated file should also be committed so builds work without the CLI.
set(PARSER_C ${CMAKE_CURRENT_SOURCE_DIR}/src/parser.c)
find_program(TREE_SITTER_CLI tree-sitter)
if(TREE_SITTER_CLI)
  add_custom_command(
    OUTPUT  ${PARSER_C}
            ${CMAKE_CURRENT_SOURCE_DIR}/src/tree_sitter/parser.h
    COMMAND ${TREE_SITTER_CLI} generate --no-bindings
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/grammar.js
    COMMENT "Generating tree-sitter parser for rooture"
  )
elseif(NOT EXISTS ${PARSER_C})
  message(FATAL_ERROR
    "src/parser.c not found and tree-sitter CLI is not available.\n"
    "Install tree-sitter and run: tree-sitter generate --no-bindings")
endif()

add_executable(rooture
  rut_lang.cxx
  rut_builtins_lang.cxx
  rut_root.cxx
  rut_jitfn.cxx
  rut_column.cxx
  rut_net.cxx
  rut_repl.cxx
  mpc.c
  ${PARSER_C}
)
target_include_directories(rooture PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src     # exposes tree_sitter/parser.h
  ${CMAKE_CURRENT_SOURCE_DIR}/vendor  # nlohmann/json.hpp
  ${TREE_SITTER_INCLUDE}
)
target_link_libraries(rooture PRIVATE
  replxx::replxx
  ${TREE_SITTER_LIB}
  ROOT::Core ROOT::MathCore ROOT::RIO
  ROOT::Hist ROOT::Tree ROOT::TreePlayer
  ROOT::ROOTVecOps ROOT::ROOTDataFrame ROOT::ROOTNTuple
  CURL::libcurl
)
target_compile_options(rooture PRIVATE -g -O2)
set_target_properties(rooture PROPERTIES
  INSTALL_RPATH "${ROOT_LIBRARY_DIR}"
  BUILD_WITH_INSTALL_RPATH TRUE
)

# Export symbols so Cling can resolve rooture bridge functions at runtime
if(APPLE)
  target_link_options(rooture PRIVATE -Wl,-export_dynamic)
else()
  target_link_options(rooture PRIVATE -rdynamic)
endif()

# Installation
install(TARGETS rooture RUNTIME DESTINATION bin)
install(FILES stdlib.rut DESTINATION share/rooture)
install(DIRECTORY examples/ DESTINATION share/rooture/examples)
install(DIRECTORY tests/   DESTINATION share/rooture/tests)

# Tests
enable_testing()

add_test(
  NAME pipe_syntax
  COMMAND $<TARGET_FILE:rooture> tests/pipe_syntax.rut
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(pipe_syntax PROPERTIES
  PASS_REGULAR_EXPRESSION "All pipe-syntax tests passed"
)

add_test(
  NAME dotimes
  COMMAND $<TARGET_FILE:rooture> tests/dotimes.rut
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(dotimes PROPERTIES
  PASS_REGULAR_EXPRESSION "All dotimes tests passed"
)

add_test(
  NAME jit_fn
  COMMAND $<TARGET_FILE:rooture> tests/jit_fn.rut
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(jit_fn PROPERTIES
  PASS_REGULAR_EXPRESSION "All jit-fn tests passed"
)

add_test(
  NAME math_builtins
  COMMAND $<TARGET_FILE:rooture> tests/math_builtins.rut
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(math_builtins PROPERTIES
  PASS_REGULAR_EXPRESSION "All math-builtins tests passed"
)
