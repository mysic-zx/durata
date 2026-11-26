/*
 * durata —— 跨端音频时长精确计算库
 *
 * 设计文档：docs/design/2026-11-26-durata-audio-duration-library.md
 *
 * 核心思路：MP3 逐帧扫描取真值（不信 Xing 等可能被污染的元数据），
 * M4A/FLAC/WAV/OGG 各取容器规范里的权威字段。全程整数累加，
 * 同一文件在任何平台/编译器下给出完全一致的结果。
 *
 * 约定：
 * - 纯 C11，零第三方依赖，不链接 ffmpeg
 * - 除 source 句柄外全程无堆分配，可放栈上、可在实时线程调用
 * - du_probe 可重入；不同 du_source_t 可并发，同一个不行
 * - 所有结构体首字段是 abi_version，新字段只许往尾部追加
 */
#ifndef DURATA_H
#define DURATA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DURATA_VERSION_MAJOR 1
#define DURATA_VERSION_MINOR 0
#define DURATA_ABI_VERSION   1

/* ------------------------------------------------------------------ */
/* 格式与状态                                                          */
/* ------------------------------------------------------------------ */

typedef enum du_format {
    DU_FORMAT_AUTO = 0,   /* 由嗅探决定 */
    DU_FORMAT_MP3  = 1,
    DU_FORMAT_MP2  = 2,   /* MPEG-2/2.5 音频，扫描逻辑与 MP3 相同 */
    DU_FORMAT_MP4  = 3,   /* m4a/m4b/mp4/alac */
    DU_FORMAT_FLAC = 4,
    DU_FORMAT_OGG  = 5,
    DU_FORMAT_WAV  = 6,   /* WAV/AIFF/RF64 都归这，容器内部自己分 */
} du_format_t;

typedef enum du_status {
    DU_OK                 = 0,
    DU_ERR_IO             = 1,
    DU_ERR_EOF_UNEXPECTED = 2,
    DU_ERR_NO_SYNC        = 3,   /* 一个有效帧/头都找不到 */
    DU_ERR_NOT_CONTAINER  = 4,   /* 不像这个格式，或关键字段缺失 */
    DU_ERR_UNSUPPORTED    = 5,
    DU_ERR_INVALID_ARG    = 6,
    DU_ERR_NOMEM          = 7,
    DU_ERR_TRUNCATED      = 8,   /* 解析成功但文件明显被截断，看 flags */
} du_status_t;

/* ------------------------------------------------------------------ */
/* 结果                                                                */
/* ------------------------------------------------------------------ */

/* 质量标记位。比较/排序请一律用 duration_samples，别用浮点。 */
#define DU_F_TRUNCATED     (1u << 0)  /* 末帧不完整 */
#define DU_F_RESYNC        (1u << 1)  /* 扫描中途重新同步过（文件有损坏段） */
#define DU_F_VBR           (1u << 2)  /* 帧码率不唯一 */
#define DU_F_XING_PRESENT  (1u << 3)
#define DU_F_XING_AGREE    (1u << 4)  /* Xing 声称的帧数与实测一致（<1% 偏差） */
#define DU_F_XING_LAME     (1u << 5)  /* 带 LAME tag（含 encoder delay 信息） */
#define DU_F_ELST_PRESENT  (1u << 6)  /* M4A 有 edit list */
#define DU_F_GAPLESS_HINT  (1u << 7)  /* 存在 iTunSMPB / LAME delay 信息 */
#define DU_F_MULTI_AUDIO   (1u << 8)  /* 多条音频轨，取了时长最长的一条 */
#define DU_F_ELST_RATE     (1u << 9)  /* elst media_rate != 1.0，未做自动修正 */

typedef struct du_result {
    uint32_t abi_version;

    /* ---- 核心输出 ---- */
    double   duration_sec;      /* 主结果，= duration_samples / sample_rate */
    uint64_t duration_samples;  /* 采样点整数，避免 double 累积误差 */
    uint32_t sample_rate;
    uint32_t channels;          /* 0 = 未知 */
    uint32_t bit_depth;         /* 0 = 未知/压缩格式 */

    /* ---- 扫描统计 ---- */
    uint64_t frames;            /* MP3 有效帧数 / AAC 包数 / FLAC 帧数 */
    uint64_t audio_bytes;       /* 扣除标签后的真实音频字节 */
    uint64_t bitrate_avg_bps;   /* 实测平均码率，不是头部声明值 */

    /* ---- 质量标记 ---- */
    uint32_t flags;

    /* ---- Xing/VBRI 交叉校验（MP3 专用，其他格式为 0） ---- */
    uint32_t xing_frames;
    uint32_t xing_bytes;
    double   xing_delta;        /* (xing_frames - frames) / frames；<0 表示无 Xing */

    /* ---- 诊断 ---- */
    uint32_t resync_count;
    uint32_t bad_header_count;
    uint64_t bytes_scanned;
} du_result_t;

/* ------------------------------------------------------------------ */
/* 读源                                                                */
/* ------------------------------------------------------------------ */

typedef struct du_source du_source_t;   /* 不透明 */

/* 自定义 IO 表 —— WASM / Android AssetManager / 网络缓存的入口。
 * read 带显式 off 而不是 seek+read：无状态、可重入、无需加锁。 */
typedef struct du_io_vtable {
    uint32_t abi_version;
    /* 返回实际读到的字节数，<0 为错误；off 是文件内绝对偏移 */
    int64_t  (*read)(void* user, uint64_t off, void* dst, int64_t n);
    /* 总长度；-1 = 未知长度（此时尾部探测类逻辑不可用） */
    int64_t  (*size)(void* user);
    void     (*close)(void* user);   /* 可为 NULL */
} du_io_vtable;

du_status_t du_source_open_path(const char* path, du_source_t** out);
du_status_t du_source_open_mem(const uint8_t* buf, size_t len, du_source_t** out);
du_status_t du_source_open_custom(const du_io_vtable* vt, void* user, du_source_t** out);
void        du_source_close(du_source_t* s);

/* ------------------------------------------------------------------ */
/* 探测                                                                */
/* ------------------------------------------------------------------ */

typedef struct du_options {
    uint32_t abi_version;

    /* 扫描控制 */
    uint64_t max_bytes;        /* 0 = 不限；防巨型文件卡死 */
    uint32_t max_frames;       /* 0 = 不限；调试用 */
    uint8_t  fast_mp3;         /* 1 = 信 Xing 直接出结果，不逐帧扫，默认 0 */
    uint8_t  gapless_correct;  /* 1 = 扣 LAME delay/padding / iTunSMPB，默认 0 */
    uint8_t  elst_apply;       /* 1 = M4A 按 edit list 裁剪时长，默认 0 */
    uint8_t  strict;           /* 1 = 遇首个坏帧即失败，默认 0 */
    uint8_t  free_format;      /* 0 = free-format 帧向后搜索帧长（默认，慢但准）
                                  1 = 跳过该帧只计数，快但时长略短 */

    /* Xing 交叉校验（只读，不修改文件） */
    uint8_t  read_xing;        /* 默认 1 */
} du_options_t;

du_status_t du_probe(du_source_t* s, du_format_t fmt,
                     const du_options_t* opts,   /* NULL = 全默认 */
                     du_result_t* out);

/* 便捷一次性接口（内部完成 open/probe/close） */
du_status_t du_probe_path(const char* path, du_format_t fmt,
                          const du_options_t* opts, du_result_t* out);

/* 是否 VBR。1 = 扫描实测到帧码率不唯一（VBR/ABR）；0 = CBR，
 * 或该格式本身没有"每帧码率"的概念（FLAC/WAV 等）。
 * 注意 fast_mp3 模式不做逐帧扫描，此结果不可靠。 */
int du_result_is_vbr(const du_result_t* r);

/* 错误码 → 可读字符串，无 malloc，永不返回 NULL */
const char* du_strerror(du_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* DURATA_H */
