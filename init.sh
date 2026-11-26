#!/bin/bash
# durata —— 跨端音频时长精确计算库（C11 核心，零依赖）
# 设计文档：docs/design/2026-11-26-durata-audio-duration-library.md
# 用法：init.sh --check | init.sh（轻量初始化）| init.sh --build（编译并跑测试）
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${DIR}/build"

check_cmake() {
    if ! command -v cmake >/dev/null 2>&1; then
        echo "[缺失] cmake 未安装：brew install cmake"
        return 1
    fi
    echo "[ok] cmake $(cmake --version | head -1 | awk '{print $NF}')"
}

check_sources() {
    for f in CMakeLists.txt include/durata.h \
             src/durata.c src/io.c src/util.c src/tables.c \
             src/mp3.c src/mp4.c src/flac.c src/ogg.c src/wav.c \
             tests/main.c tests/synth.c tests/test_mp3.c tests/test_containers.c; do
        if [ ! -f "${DIR}/${f}" ]; then
            echo "[缺失] ${f}"
            return 1
        fi
    done
    echo "[ok] 源码齐全"
}

MODE="${1:-}"

case "${MODE}" in
    --check)
        check_cmake || exit 1
        check_sources || exit 1
        if [ -x "${BUILD_DIR}/tests/du_test" ]; then
            echo "[ok] 已有构建产物（跑 ./init.sh --build 可刷新并重跑测试）"
        elif [ -d "${BUILD_DIR}" ]; then
            echo "[提示] 已配置但未编译（跑 ./init.sh --build）"
        else
            echo "[提示] 尚未配置（跑 ./init.sh 或 ./init.sh --build）"
        fi
        ;;
    "")
        check_cmake || exit 1
        check_sources || exit 1
        cmake -S "${DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug >/dev/null || exit 1
        echo "[ok] 已配置 ${BUILD_DIR}（Debug）"
        ;;
    --build)
        check_cmake || exit 1
        check_sources || exit 1
        cmake -S "${DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Debug >/dev/null || exit 1
        cmake --build "${BUILD_DIR}" || exit 1
        "${BUILD_DIR}/tests/du_test" || exit 1
        echo "[ok] durata 构建完成，全部单元测试通过"
        ;;
    *)
        echo "用法：$0 [--check | （无参）轻量初始化 | --build]"
        exit 1
        ;;
esac
