#!/bin/sh
set -e
cd build
make -j$(nproc 2>/dev/null)
mkdir -p output/assets
cp --remove-destination falcon output/
cp -r --remove-destination ../assets/* output/assets/
