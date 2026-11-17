#!/bin/sh
# Persistent translator client (runtime/nvn/nvn_xlate_server.h): builds and runs tests/nvn/xlate_server_test.cpp. No GPU or game data needed.
# With NVN_SHADER_HARNESS set it also checks a real --nvn-serve process answers a bad request with an error reply.
cd "$(dirname "$0")/../.." || exit 1
mkdir -p build/w/xlsrv && tools/llvm/bin/clang++.exe -std=c++20 -O1 -D_CRT_SECURE_NO_WARNINGS -I runtime/nvn tests/nvn/xlate_server_test.cpp -o build/w/xlsrv/xlate_server_test.exe || exit 1
./build/w/xlsrv/xlate_server_test.exe
