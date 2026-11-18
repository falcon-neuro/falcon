#!/bin/sh
set -e
cd "$(dirname "$0")/.."

if ! command -v cmake >/dev/null 2>&1; then
    echo "Error: CMake is not installed."
    echo "You can install it using: scripts/setup/install_cmake.sh"
    exit 1
fi

gcc_directory=$(sh scripts/setup/fetch_gcc.sh --where)/usr/bin
if [ ! -x "$gcc_directory/g++-16" ]; then
    echo "Error: GCC is not fetched."
    echo "You can fetch it using: sh scripts/setup/fetch_gcc.sh"
    exit 1
fi

mkdir -p build && cd build
cmake .. \
  -DCMAKE_BUILD_TYPE=Profile \
  -DCMAKE_C_COMPILER="$gcc_directory/gcc-16" \
  -DCMAKE_CXX_COMPILER="$gcc_directory/g++-16"
