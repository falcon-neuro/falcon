#!/bin/sh
set -e

# Runs clang-tidy on every .cpp file under src/ that git tracks or would track (respects .gitignore),
# each file on its own. Falcon's .cpp files include each other, so most of them are not in
# build/compile_commands.json; clang-tidy borrows the flags of the closest entry there.
# Exits non-zero if any file has an error.

cd "$(git rev-parse --show-toplevel)"

command -v clang-tidy
clang-tidy --version

FILES=$(git ls-files --cached --others --exclude-standard 'src/*.cpp')

if [ -z "$FILES" ]; then
    echo "No matching files found."
    exit 0
fi

echo "$FILES" | xargs -P "$(nproc)" -n 1 clang-tidy -p build --quiet
