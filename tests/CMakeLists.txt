# 测试直接编 src 源码，方便访问内部函数（CRC8 等）
set(DURATA_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_executable(du_test
    main.c
    synth.c
    test_mp3.c
    test_containers.c
    ${DURATA_SRC_DIR}/durata.c
    ${DURATA_SRC_DIR}/io.c
    ${DURATA_SRC_DIR}/util.c
    ${DURATA_SRC_DIR}/tables.c
    ${DURATA_SRC_DIR}/mp3.c
    ${DURATA_SRC_DIR}/mp4.c
    ${DURATA_SRC_DIR}/flac.c
    ${DURATA_SRC_DIR}/ogg.c
    ${DURATA_SRC_DIR}/wav.c
)

target_include_directories(du_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_definitions(du_test PRIVATE ${DURATA_DISABLED})

if(MSVC)
    target_compile_definitions(du_test PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(du_test PRIVATE -Wall -Wextra)
endif()

add_test(NAME durata_unit COMMAND du_test)
