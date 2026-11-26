#!/bin/bash
# 打包 iOS 静态库 xcframework：
#   device   = arm64
#   simulator= arm64 + x86_64
# 产物：Libs/durata/dist/durata.xcframework（含 Headers + module.modulemap，
# Xcode / Swift 可直接引，`import Durata`）
# 参考：docs/design/2026-11-26-durata-audio-duration-library.md §10.1/§11.4
set -u

DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${DIR}/dist"
BUILD="${DIR}/build-ios"

command -v cmake >/dev/null 2>&1 || { echo "[缺失] cmake：brew install cmake"; exit 1; }
command -v xcodebuild >/dev/null 2>&1 || { echo "[缺失] xcodebuild：安装 Xcode"; exit 1; }
[ -f "${DIR}/include/durata.h" ] || { echo "[缺失] 源码不完整"; exit 1; }
xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1 || { echo "[缺失] iOS SDK：安装 Xcode"; exit 1; }

echo "---- iOS device (arm64) ----"
cmake -S "${DIR}" -B "${BUILD}-device" \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_BUILD_TYPE=Release -DDURATA_BUILD_TESTS=OFF >/dev/null || exit 1
cmake --build "${BUILD}-device" || exit 1

echo "---- iOS simulator (arm64 + x86_64) ----"
cmake -S "${DIR}" -B "${BUILD}-sim" \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
      -DCMAKE_BUILD_TYPE=Release -DDURATA_BUILD_TESTS=OFF >/dev/null || exit 1
cmake --build "${BUILD}-sim" || exit 1

echo "---- Headers + modulemap ----"
rm -rf "${OUT}/ios-device-headers" "${OUT}/ios-sim-headers"
mkdir -p "${OUT}/ios-device-headers" "${OUT}/ios-sim-headers"
cp "${DIR}/include/durata.h" "${OUT}/ios-device-headers/"
cp "${DIR}/include/durata.h" "${OUT}/ios-sim-headers/"
cat > "${OUT}/ios-device-headers/module.modulemap" <<'EOF'
module Durata {
    header "durata.h"
    export *
}
EOF
cp "${OUT}/ios-device-headers/module.modulemap" "${OUT}/ios-sim-headers/"

echo "---- create-xcframework ----"
rm -rf "${OUT}/durata.xcframework"
xcodebuild -create-xcframework \
    -library "${BUILD}-device/libdurata.a" -headers "${OUT}/ios-device-headers" \
    -library "${BUILD}-sim/libdurata.a" -headers "${OUT}/ios-sim-headers" \
    -output "${OUT}/durata.xcframework" || exit 1

lipo -info "${OUT}/durata.xcframework/ios-arm64/libdurata.a"
lipo -info "${OUT}/durata.xcframework/ios-arm64_x86_64-simulator/libdurata.a"
echo "[ok] ${OUT}/durata.xcframework"
