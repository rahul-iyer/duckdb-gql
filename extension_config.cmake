# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(duckgql
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    EXTENSION_VERSION 0.2.0
)
duckdb_extension_statically_link(duckgql)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
