#!/bin/bash
# 打包 iOS 静态库 xcframework：
#   device     = arm64
#   simulator  = arm64（x86_64 已随 Intel 模拟器退场不再打包）
#   maccatalyst= arm64 + x86_64（Catalyst 的最低门槛是 iOS 13）
# 产物：Libs/durata/dist/durata.xcframework（含 Headers + module.modulemap，
# Xcode / Swift 可直接引，`import Durata`）
# 参考：docs/design/2026-11-26-durata-audio-duration-library.md §10.1/§11.4
set -u

DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${DIR}/dist"
BUILD="${DIR}/build-ios"
IOS_DT=13.0    # iOS/simulator deployment target，与主工程 iOS 13 对齐
CAT_DT=14.0    # macCatalyst deployment target：Xcode 26 的 clang 对
               # -macabi 不接受 ios13.x triple（invalid version number），
               # 实测 14.0 起接受（13.2 也被钳到 minos 14.0）

command -v cmake >/dev/null 2>&1 || { echo "[缺失] cmake：brew install cmake"; exit 1; }
command -v xcodebuild >/dev/null 2>&1 || { echo "[缺失] xcodebuild：安装 Xcode"; exit 1; }
command -v lipo >/dev/null 2>&1 || { echo "[缺失] lipo（随 Xcode 提供）"; exit 1; }
[ -f "${DIR}/include/durata.h" ] || { echo "[缺失] 源码不完整"; exit 1; }
xcrun --sdk iphoneos --show-sdk-path >/dev/null 2>&1 || { echo "[缺失] iOS SDK：安装 Xcode"; exit 1; }
MACOS_SDK=$(xcrun --sdk macosx --show-sdk-path)

echo "---- iOS device (arm64) ----"
rm -rf "${BUILD}-device"   # 清掉旧缓存：改过架构参数的残留会让 CMake generate 冲突
cmake -S "${DIR}" -B "${BUILD}-device" \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=${IOS_DT} \
      -DCMAKE_BUILD_TYPE=Release -DDURATA_BUILD_TESTS=OFF >/dev/null || exit 1
cmake --build "${BUILD}-device" || exit 1

echo "---- iOS simulator (arm64) ----"
rm -rf "${BUILD}-sim"
cmake -S "${DIR}" -B "${BUILD}-sim" \
      -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=${IOS_DT} \
      -DCMAKE_BUILD_TYPE=Release -DDURATA_BUILD_TESTS=OFF >/dev/null || exit 1
cmake --build "${BUILD}-sim" || exit 1

# ---- macCatalyst：-macabi triple 让 clang 写出 iossupport 平台标记，
# create-xcframework 才会归入 maccatalyst 切片。-target 只能带单架构，
# 所以 arm64 / x86_64 各编一次再 lipo 合并。
# 注意不能用 CMAKE_SYSTEM_NAME=iOS + macosx SDK 组合：CMake 3.22 会拒绝
# "not an iOS SDK"，这里按 host 平台配置，Catalyst 语义全靠 -target。 ----
echo "---- macCatalyst (arm64) ----"
rm -rf "${BUILD}-cat-arm64"
cmake -S "${DIR}" -B "${BUILD}-cat-arm64" \
      -DCMAKE_OSX_SYSROOT="${MACOS_SDK}" \
      -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_C_FLAGS="-target arm64-apple-ios${CAT_DT}-macabi" \
      -DCMAKE_BUILD_TYPE=Release -DDURATA_BUILD_TESTS=OFF >/dev/null || exit 1
cmake --build "${BUILD}-cat-arm64" || exit 1

echo "---- macCatalyst (x86_64) ----"
rm -rf "${BUILD}-cat-x64"
cmake -S "${DIR}" -B "${BUILD}-cat-x64" \
      -DCMAKE_OSX_SYSROOT="${MACOS_SDK}" \
      -DCMAKE_OSX_ARCHITECTURES=x86_64 \
      -DCMAKE_C_FLAGS="-target x86_64-apple-ios${CAT_DT}-macabi" \
      -DCMAKE_BUILD_TYPE=Release -DDURATA_BUILD_TESTS=OFF >/dev/null || exit 1
cmake --build "${BUILD}-cat-x64" || exit 1

mkdir -p "${BUILD}-catalyst"
lipo -create "${BUILD}-cat-arm64/libdurata.a" "${BUILD}-cat-x64/libdurata.a" \
     -output "${BUILD}-catalyst/libdurata.a" || exit 1

# ---- 组装静态 framework bundle（对齐 mdict.xcframework 的形态）----
# framework 形态的 xcframework，Xcode 才会自动处理头文件搜索路径，
# ObjC/Swift 才能直接 `#import <durata/durata.h>`；library 形态（裸 .a +
# Headers）的头文件对工程不可见。静态库的可执行文件名不带 lib 前缀。
make_framework() {
    local src_lib="$1" out="$2"
    rm -rf "${out}"
    mkdir -p "${out}/Headers" "${out}/Modules"
    cp "${src_lib}" "${out}/durata"
    cp "${DIR}/include/durata.h" "${out}/Headers/"
    cat > "${out}/Modules/module.modulemap" <<'EOF'
module Durata {
    header "durata.h"
    export *
}
EOF
    cat > "${out}/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleName</key><string>durata</string>
	<key>CFBundleIdentifier</key><string>com.mysic.durata</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundlePackageType</key><string>FMWK</string>
	<key>CFBundleExecutable</key><string>durata</string>
	<key>CFBundleShortVersionString</key><string>1.0.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>MinimumOSVersion</key><string>13.0</string>
</dict>
</plist>
EOF
}

make_framework "${BUILD}-device/libdurata.a"   "${BUILD}-fw-device/durata.framework"
make_framework "${BUILD}-sim/libdurata.a"      "${BUILD}-fw-sim/durata.framework"
make_framework "${BUILD}-catalyst/libdurata.a" "${BUILD}-fw-catalyst/durata.framework"

echo "---- Headers + modulemap ----"
rm -rf "${OUT}/ios-device-headers" "${OUT}/ios-sim-headers" "${OUT}/ios-cat-headers"

echo "---- create-xcframework ----"
rm -rf "${OUT}/durata.xcframework"
xcodebuild -create-xcframework \
    -framework "${BUILD}-fw-device/durata.framework" \
    -framework "${BUILD}-fw-sim/durata.framework" \
    -framework "${BUILD}-fw-catalyst/durata.framework" \
    -output "${OUT}/durata.xcframework" || exit 1

lipo -info "${OUT}/durata.xcframework/ios-arm64/durata.framework/durata"
lipo -info "${OUT}/durata.xcframework/ios-arm64-simulator/durata.framework/durata"
lipo -info "${OUT}/durata.xcframework/ios-arm64_x86_64-maccatalyst/durata.framework/durata"
echo "[ok] ${OUT}/durata.xcframework"
