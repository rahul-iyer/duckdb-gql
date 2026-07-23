cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME duckgql)
project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(antlr4-runtime CONFIG REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

# DuckDB's Wasm extension builder relinks the extension's static archive in a
# post-build step. CMake target dependencies are not forwarded to that command,
# so include the ANTLR runtime archive explicitly in the final side module.
if(EMSCRIPTEN)
    set(DUCKDB_EXTENSION_DUCKGQL_LINKED_LIBS
        "../../vcpkg_installed/wasm32-emscripten/lib/libantlr4-runtime.a"
    )
endif()

include_directories(
    src/include
    src/parser/generated
    ${ANTLR4_INCLUDE_DIR}
)

set(EXTENSION_SOURCES
    src/gql_extension.cpp
    src/gql_catalog.cpp
    src/gql_parser.cpp
    src/gql_transformer.cpp
    src/gql_binder.cpp
    src/gql_ir.cpp
    src/gql_optimizer.cpp
    src/gql_lowerer.cpp
    src/gql_mutation.cpp
    src/gql_relational.cpp
    src/gql_storage.cpp
    src/gql_import.cpp
    src/gql_csr.cpp
    src/gql_algorithms.cpp
    src/parser/generated/GQLBaseVisitor.cpp
    src/parser/generated/GQLLexer.cpp
    src/parser/generated/GQLParser.cpp
    src/parser/generated/GQLVisitor.cpp
)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

set_target_properties(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
    PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(TARGET antlr4_static)
    target_link_libraries(${EXTENSION_NAME} antlr4_static)
    target_link_libraries(${LOADABLE_EXTENSION_NAME} antlr4_static)
else()
    message(FATAL_ERROR "The DuckGQL extension requires the static ANTLR4 C++ runtime target")
endif()

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
