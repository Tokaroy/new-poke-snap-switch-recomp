#!/bin/sh
# Translation-cache entry protocol (runtime/nvn/nvn_xcache.h): builds and runs tests/nvn/xcache_test.cpp. No GPU or game data needed.
cd "$(dirname "$0")/../.." || exit 1
mkdir -p build/w/xcache && tools/llvm/bin/clang++.exe -std=c++20 -O1 -D_CRT_SECURE_NO_WARNINGS -I runtime/nvn tests/nvn/xcache_test.cpp -o build/w/xcache/xcache_test.exe || exit 1
./build/w/xcache/xcache_test.exe
