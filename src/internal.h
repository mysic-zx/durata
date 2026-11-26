/*
 * 内部共享头 —— 只给 src/ 下的实现文件用，不对外安装。
 */
#ifndef DURATA_INTERNAL_H
#define DURATA_INTERNAL_H

#include "durata.h"

#define DU_IO_WINDOW (256 * 1024)   /* read 模式滑动窗口，文档 §8.3 默认值 */
#define DU_MP4_MAX_DEPTH   8
#define DU_MP4_MAX_BOXES   4096

/* ------------------------------------------------------------------ */
/* 读源内部结构                                                        */
/* ------------------------------------------------------------------ */

struct du_source {
    const du_io_vtable* vt;
    void*    user;
    int64_t  size;          /* 总长度，-1 = 未知 */
    uint8_t* window;        /* NULL = mem 模式（vt 直接指向内存） */
    uint64_t win_off;       /* window[0] 对应的文件偏移 */
    uint32_t win_len;       /* 窗口内有效字节数 */
    uint32_t win_cap;
};

/* ------------------------------------------------------------------ */
/* 探测上下文                                                          */
/* ------------------------------------------------------------------ */

typedef struct du_ctx {
    du_source_t*        src;
    const du_options_t* opt;   /* 已默认化（外部传 NULL 时指向内部默认值） */
    du_result_t*        res;   /* 已清零 */
    du_status_t         st;    /* 解析器用 DU_FAIL 之类设置后立即返回 */
    uint64_t            gapless_trim;   /* 解析器发现的应扣采样（LAME delay/padding） */
} du_ctx_t;

#define DU_FAIL(ctx, code) do { (ctx)->st = (code); return (code); } while (0)

/* ------------------------------------------------------------------ */
/* 大端读取 —— 全走逐字节拼接，不依赖本机端序，MSVC 也一样              */
/* ------------------------------------------------------------------ */

static inline uint16_t du_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static inline uint32_t du_be24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static inline uint32_t du_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | p[3];
}

static inline uint64_t du_be64(const uint8_t* p) {
    return ((uint64_t)du_be32(p) << 32) | du_be32(p + 4);
}

static inline uint64_t du_le64(const uint8_t* p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

/* ID3v2 的 synchsafe 整数：每字节只低 7 位有效 */
static inline uint32_t du_synchsafe32(const uint8_t* p) {
    return ((uint32_t)(p[0] & 0x7F) << 21) | ((uint32_t)(p[1] & 0x7F) << 14) |
           ((uint32_t)(p[2] & 0x7F) << 7)  | (uint32_t)(p[3] & 0x7F);
}

/* ------------------------------------------------------------------ */
/* io.c —— 解析器统一走的读接口                                        */
/* ------------------------------------------------------------------ */

/* 视图：返回 [off, off+need) 的连续内存；实际可用字节写进 *avail。
 * need 超过窗口容量属于调用方 bug，返回 NULL。
 * off 越过文件尾返回 NULL（*avail = 0）。 */
const uint8_t* du_io_view(du_source_t* s, uint64_t off, uint64_t need, uint64_t* avail);

/* 任意位置读 n 字节，返回实际读数；内部借窗口缓存，小块调用开销低 */
int64_t du_io_read_at(du_source_t* s, uint64_t off, void* dst, int64_t n);

int64_t du_io_size(du_source_t* s);

/* ------------------------------------------------------------------ */
/* util.c                                                              */
/* ------------------------------------------------------------------ */

uint32_t du_util_crc8_flac(uint32_t seed, const uint8_t* p, size_t n);

/* 识别 MP3 文件尾部挂在音频后面的标签，返回应从文件尾扣除的字节数。
 * 命中 APEv2 时把 flag 位置上。 */
uint64_t du_util_mp3_tail_tags(du_source_t* s, uint32_t* flags);

/* ------------------------------------------------------------------ */
/* tables.c —— MP3 帧参数                                              */
/* ------------------------------------------------------------------ */

typedef struct du_mp3_frame_info {
    uint32_t kbps;   /* 声称码率 */
    uint32_t spf;    /* 每帧采样数 */
    uint32_t sr;     /* 采样率 */
    uint32_t coef;   /* 帧长系数：L1=12、V1L2L3=144、V2/2.5L3=72 */
    uint32_t mul;    /* L1 帧长再 ×4，其余 ×1 */
} du_mp3_frame_info_t;

/* version 0..3（0=MPEG2.5, 2=MPEG2, 3=MPEG1），layer 1..3。
 * 合法性调用方已保证（保留值不会传进来）。 */
du_mp3_frame_info_t du_mp3_frame_info(uint32_t version, uint32_t layer,
                                      uint32_t br_idx, uint32_t sr_idx);

/* ------------------------------------------------------------------ */
/* 各格式解析器（durata.c 分发）                                        */
/* ------------------------------------------------------------------ */

du_status_t du_probe_mp3(du_ctx_t* c);
du_status_t du_probe_mp4(du_ctx_t* c);
du_status_t du_probe_flac(du_ctx_t* c);
du_status_t du_probe_wav(du_ctx_t* c);
du_status_t du_probe_ogg(du_ctx_t* c);

#endif /* DURATA_INTERNAL_H */
