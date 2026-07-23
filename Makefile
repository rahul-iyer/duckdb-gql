PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=duckgql
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile
include extension-ci-tools/makefiles/vcpkg.Makefile

# Only test/sql contains the promoted, release-gating SQLLogicTests.
# test/features also contains generated conformance candidates that are
# intentionally allowed to fail until they are reviewed and promoted.
TESTS_BASE_DIRECTORY = "test/sql/"
