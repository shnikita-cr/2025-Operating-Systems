#!/usr/bin/env bash
set -euo pipefail

# load .env (optional)
if [[ -f .env ]]; then export $(grep -v '^#' .env | xargs); fi

BUILD_DIR=${BUILD_DIR:-build}
TARGET=${TARGET:-lab1}

rm -rf -- "$BUILD_DIR" || true
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j

mkdir -p run
cp -f "$BUILD_DIR/bin/$TARGET" run/
# Keep user's config alongside
if [[ -f config.txt ]]; then cp -f config.txt run/; fi

echo "Built -> run/$TARGET"