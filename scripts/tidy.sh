#!/bin/sh
set -e
cd "$(dirname "$0")/.."
command -v clang-tidy
clang-tidy --version
find src -name '*.cpp' -print0 | xargs -0 -r -P "$(nproc)" -n 1 clang-tidy -p build --quiet
