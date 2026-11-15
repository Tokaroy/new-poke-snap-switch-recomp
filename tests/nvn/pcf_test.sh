#!/bin/sh
# Offline PatchShadowPcf check: build, patch every receiver module in the shader cache, spirv-val each. usage: pcf_test.sh [cache dir] [taps] [pcss 0|1]
cd "$(dirname "$0")/../.." || exit 1
C=${1:-build/snap/nvn_cache}; T=${2:-16}; P=${3:-1}
mkdir -p build/w/pcf && tools/llvm/bin/clang++.exe -std=c++20 -O1 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -I runtime -I runtime/nvn -I tools/vulkan_sdk/Include tests/nvn/pcf_test.cpp runtime/nvn/nvn_shadowpcf.cpp -o build/w/pcf/pcf_test.exe || exit 1
rm -rf build/w/pcf/out && ./build/w/pcf/pcf_test.exe "$C" build/w/pcf/out "$T" "$P" 4 || exit 1
ok=0; bad=0
for f in build/w/pcf/out/*.spv; do
  if tools/vulkan_sdk/Bin/spirv-val.exe --target-env vulkan1.3 "$f" >build/w/pcf/val.txt 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); [ $bad -le 3 ] && { echo "== $f"; head -4 build/w/pcf/val.txt; }; fi
done
echo "valid $ok invalid $bad"; [ $bad -eq 0 ]
