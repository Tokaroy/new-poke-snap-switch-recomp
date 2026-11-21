#!/bin/sh
# Offline check of the alpha-test wrappers (Foliage stability, Leaf edge smoothing): build, patch every fragment module in the shader cache,
# spirv-val each. usage: foliage_test.sh [cache dir]
cd "$(dirname "$0")/../.." || exit 1
C=${1:-build/snap/nvn_cache}
mkdir -p build/w/foliage_test && tools/llvm/bin/clang++.exe -std=c++20 -O1 -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations -I runtime -I runtime/nvn -I tools/vulkan_sdk/Include \
  tests/nvn/foliage_test.cpp runtime/nvn/nvn_foliage.cpp -L tools/vulkan_sdk/Lib -lvulkan-1 -o build/w/foliage_test/foliage_test.exe || exit 1
rm -rf build/w/foliage_test/out && ./build/w/foliage_test/foliage_test.exe "$C" build/w/foliage_test/out || exit 1
ok=0; bad=0
for f in build/w/foliage_test/out/*.spv; do
  if tools/vulkan_sdk/Bin/spirv-val.exe --target-env vulkan1.3 "$f" >build/w/foliage_test/val.txt 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); [ $bad -le 3 ] && { echo "== $f"; head -4 build/w/foliage_test/val.txt; }; fi
done
echo "valid $ok invalid $bad"; [ $bad -eq 0 ]
