#!/bin/bash
# Build the p_net_bridge binary (POSIX). Requires cmake, make/ninja, gcc.
set -o errexit
set -o pipefail
set -o nounset

HERE="$(cd "$(dirname "$0")" && pwd)"

if [ ! -f "$HERE/p-net/CMakeLists.txt" ]; then
  echo "p-net submodule not initialized — run: git submodule update --init --recursive"
  exit 1
fi

cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$HERE/build" -j "$(nproc)"

echo
echo "Built: $HERE/build/p_net_bridge"
