#!/usr/bin/env bash
set -euo pipefail

# Очистить предыдущую сборку
rm -rf build run
mkdir -p build run

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Разложить бинарники в run/
cp -f build/host_mq   run/host_mq   || true
cp -f build/client_mq run/client_mq || true
cp -f build/host_fifo   run/host_fifo   || true
cp -f build/client_fifo run/client_fifo || true
cp -f build/host_sock   run/host_sock   || true
cp -f build/client_sock run/client_sock || true

echo "Built binaries:"
ls -l run
