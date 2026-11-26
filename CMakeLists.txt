cmake_minimum_required(VERSION 3.20)
project(durata VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(DURATA_BUILD_SHARED "Build shared library instead of static" OFF)
option(DURATA_BUILD_TESTS  "Build unit tests" ON)

# D4 决策：全格式实现，但每种格式都能编译期关掉（App 集成轨按需裁剪）
option(DURATA_ENABLE_MP3  "Include MP3/MPEG audio support" ON)
option(DURATA_ENABLE_MP4  "Include MP4/M4A support" ON)
option(DURATA_ENABLE_FLAC "Include FLAC support" ON)
option(DURATA_ENABLE_OGG  "Include OGG support" ON)
option(DURATA_ENABLE_WAV  "Include WAV/AIFF support" ON)

set(DURATA_SOURCES
    src/durata.c
    src/io.c
    src/util.c
    src/tables.c
)
set(DURATA_DISABLED "")
# 注意：源文件名必须小写明确写出，不能用 ${fmt}.c 拼 —— 否则在大小写
# 不敏感的文件系统上会生成 MP3.c 这种引用，SVN/CI 上直接炸
if(DURATA_ENABLE_MP3)
    list(APPEND DURATA_SOURCES src/mp3.c)
else()
    list(APPEND DURATA_DISABLED DURATA_NO_MP3)
endif()
if(DURATA_ENABLE_MP4)
    list(APPEND DURATA_SOURCES src/mp4.c)
else()
    list(APPEND DURATA_DISABLED DURATA_NO_MP4)
endif()
if(DURATA_ENABLE_FLAC)
    list(APPEND DURATA_SOURCES src/flac.c)
else()
    list(APPEND DURATA_DISABLED DURATA_NO_FLAC)
endif()
if(DURATA_ENABLE_OGG)
    list(APPEND DURATA_SOURCES src/ogg.c)
else()
    list(APPEND DURATA_DISABLED DURATA_NO_OGG)
endif()
if(DURATA_ENABLE_WAV)
    list(APPEND DURATA_SOURCES src/wav.c)
else()
    list(APPEND DURATA_DISABLED DURATA_NO_WAV)
endif()

if(DURATA_BUILD_SHARED)
    add_library(durata SHARED ${DURATA_SOURCES})
else()
    add_library(durata STATIC ${DURATA_SOURCES})
endif()

target_include_directories(durata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(durata PRIVATE ${DURATA_DISABLED})

if(MSVC)
    target_compile_definitions(durata PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(durata PRIVATE /W4 /O2)
else()
    target_compile_options(durata PRIVATE -Wall -Wextra -Werror -O3)
    # 32 位平台：文件偏移强制 64 位（文档 §10.4）
    target_compile_definitions(durata PRIVATE _FILE_OFFSET_BITS=64)
endif()

# 测试自研框架零依赖，直接编 src 源码（方便访问内部函数算 CRC 等）
if(DURATA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
