#!/bin/sh
# Offline PatchShadowRt check: build, patch every receiver module in the shader cache, spirv-val each. usage: shadowrt_test.sh [cache dir] [rays]
# The clang compile is a build: run it under the build lock (dot-source host/locks.ps1, Take-BuildLock) like any other.
cd "$(dirname "$0")/../.." || exit 1
C=${1:-build/snap/nvn_cache}; R=${2:-4}
mkdir -p build/w/srt && tools/llvm/bin/clang++.exe -std=c++20 -O1 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -I runtime -I runtime/nvn -I tools/vulkan_sdk/Include tests/nvn/shadowrt_test.cpp runtime/nvn/nvn_shadowrt.cpp -o build/w/srt/shadowrt_test.exe || exit 1
rm -rf build/w/srt/out && ./build/w/srt/shadowrt_test.exe "$C" build/w/srt/out "$R" || exit 1
ok=0; bad=0
for f in build/w/srt/out/*.spv; do
  [ -e "$f" ] || break
  if tools/vulkan_sdk/Bin/spirv-val.exe --target-env vulkan1.3 "$f" >build/w/srt/val.txt 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); [ $bad -le 3 ] && { echo "== $f"; head -4 build/w/srt/val.txt; }; fi
done
echo "valid $ok invalid $bad"; [ $bad -eq 0 ]
