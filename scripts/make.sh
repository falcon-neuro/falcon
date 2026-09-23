#!/bin/sh
set -e
cd build
make -j$(nproc 2>/dev/null)
mkdir -p debug/assets
cp --remove-destination falcon debug/
cp -r --remove-destination ../assets/* debug/assets/
