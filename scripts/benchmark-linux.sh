#!/usr/bin/env bash
set -euo pipefail

operations="${1:-10000000}"
cpu="${2:-2}"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -march=native"
cmake --build build --parallel
ctest --test-dir build --output-on-failure

uname -a
command -v lscpu >/dev/null && lscpu | grep -E 'Model name|CPU MHz|Socket|Core|Thread'
"./build/dme_bench" "$operations" "$cpu"
