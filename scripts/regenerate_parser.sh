#!/usr/bin/env bash
set -euo pipefail

readonly ANTLR_VERSION="4.13.2"
readonly ANTLR_SHA256="eae2dfa119a64327444672aff63e9ec35a20180dc5b8090b7a6ab85125df4d76"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
readonly CACHE_DIR="${PROJECT_DIR}/.cache/antlr"
readonly ANTLR_JAR="${CACHE_DIR}/antlr-${ANTLR_VERSION}-complete.jar"

mkdir -p "${CACHE_DIR}" "${PROJECT_DIR}/src/parser/generated"

if [[ ! -f "${ANTLR_JAR}" ]]; then
  curl --fail --location --silent --show-error \
    "https://www.antlr.org/download/antlr-${ANTLR_VERSION}-complete.jar" \
    --output "${ANTLR_JAR}"
fi

actual_sha256="$(shasum -a 256 "${ANTLR_JAR}" | awk '{print $1}')"
if [[ "${actual_sha256}" != "${ANTLR_SHA256}" ]]; then
  echo "ANTLR checksum mismatch: expected ${ANTLR_SHA256}, got ${actual_sha256}" >&2
  exit 1
fi

java -jar "${ANTLR_JAR}" \
  -Dlanguage=Cpp \
  -visitor \
  -no-listener \
  -Xexact-output-dir \
  -o "${PROJECT_DIR}/src/parser/generated" \
  "${PROJECT_DIR}/third_party/opengql/GQL.g4"
