#!/bin/sh
set -e
cd build
make -j$(nproc 2>/dev/null)
mkdir -p debug
cp --remove-destination falcon debug/
