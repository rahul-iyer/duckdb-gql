PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duckgql
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# DuckDB 2.0 source builds use a development ABI unless explicitly versioned.
DUCKDB_EXPLICIT_VERSION ?= v2.0.0
EXT_FLAGS += -DDUCKDB_EXPLICIT_VERSION=$(DUCKDB_EXPLICIT_VERSION) -DCMAKE_CXX_STANDARD=17

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
include extension-ci-tools/makefiles/vcpkg.Makefile

# Only test/sql contains the promoted, release-gating SQLLogicTests.
# test/features also contains generated conformance candidates that are
# intentionally allowed to fail until they are reviewed and promoted.
TESTS_BASE_DIRECTORY = "test/sql/"
