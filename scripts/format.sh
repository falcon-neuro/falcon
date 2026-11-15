#!/bin/sh
set -e
cd "$(dirname "$0")/.."
command -v clang-format
clang-format --version
find src -name '*.cpp' -print0 | xargs -0 -r clang-format -i
