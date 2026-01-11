#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
BUILD_TMP="${ROOT_DIR}/build.tmp"
RUN_DIR="${ROOT_DIR}/run"
RUN_TMP="${ROOT_DIR}/run.tmp"

rm -rf "${BUILD_TMP}" "${RUN_TMP}"
mkdir -p "${BUILD_TMP}" "${RUN_TMP}"

cmake -S "${ROOT_DIR}" -B "${BUILD_TMP}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_TMP}" -j "$(nproc)"

BIN_DIR="${BUILD_TMP}/bin"
if [ ! -d "${BIN_DIR}" ]; then
  echo "ERROR: expected bin dir not found: ${BIN_DIR}" >&2
  exit 1
fi

required_bins=( host_sock client_sock )

for b in "${required_bins[@]}"; do
  if [ ! -x "${BIN_DIR}/${b}" ]; then
    echo "ERROR: missing binary: ${BIN_DIR}/${b}" >&2
    exit 1
  fi
  cp -f "${BIN_DIR}/${b}" "${RUN_TMP}/${b}"
done

optional_bins=( host_fifo client_fifo host_mq client_mq )
for b in "${optional_bins[@]}"; do
  if [ -x "${BIN_DIR}/${b}" ]; then
    cp -f "${BIN_DIR}/${b}" "${RUN_TMP}/${b}"
  fi
done

rm -rf "${RUN_DIR}"
mv "${RUN_TMP}" "${RUN_DIR}"

echo "Built binaries:"
ls -l "${RUN_DIR}"

rm -rf "${BUILD_DIR}"
mv "${BUILD_TMP}" "${BUILD_DIR}"

