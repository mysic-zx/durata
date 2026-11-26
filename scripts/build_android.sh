#!/bin/bash
# 打包 Android 各 ABI 静态库（JNI/AAR 绑定属后续阶段，先提供可链接产物）
# 产物：Libs/durata/dist/android/<abi>/libdurata.a + include/durata.h
# NDK 探测顺序：$ANDROID_NDK_HOME → ~/Library/Android/sdk/ndk/<最新版本>
set -u

DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${DIR}/dist/android"
ABIS="arm64-v8a armeabi-v7a x86 x86_64"

command -v cmake >/dev/null 2>&1 || { echo "[缺失] cmake：brew install cmake"; exit 1; }
[ -f "${DIR}/include/durata.h" ] || { echo "[缺失] 源码不完整"; exit 1; }

NDK="${ANDROID_NDK_HOME:-}"
if [ -z "${NDK}" ]; then
    LATEST=""
    for d in "${HOME}/Library/Android/sdk/ndk/"*/; do
        # toolchain.cmake 是普通文件，用 -r 判断；取版本号最大的目录
        [ -r "${d}build/cmake/android.toolchain.cmake" ] && LATEST="${d%/}"
    done
    NDK="${LATEST}"
fi
if [ ! -r "${NDK}/build/cmake/android.toolchain.cmake" ]; then
    echo "[缺失] Android NDK（设置 ANDROID_NDK_HOME，或放到 ~/Library/Android/sdk/ndk/<版本>）"
    exit 1
fi
echo "[ok] NDK ${NDK##*/}"

MIN_API=21    # 与主工程 android minSdk 一致（AGENTS.md）

for ABI in ${ABIS}; do
    echo "---- ${ABI} ----"
    BUILD="${DIR}/build-android-${ABI}"
    cmake -S "${DIR}" -B "${BUILD}" \
          -DCMAKE_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake" \
          -DANDROID_ABI="${ABI}" -DANDROID_PLATFORM=android-${MIN_API} \
          -DCMAKE_BUILD_TYPE=Release -DDURATA_BUILD_TESTS=OFF >/dev/null || exit 1
    cmake --build "${BUILD}" || exit 1
    mkdir -p "${OUT}/${ABI}"
    cp "${BUILD}/libdurata.a" "${OUT}/${ABI}/" || exit 1
done

mkdir -p "${OUT}/include"
cp "${DIR}/include/durata.h" "${OUT}/include/"
echo "[ok] ${OUT}"
