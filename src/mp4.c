/* M4A / MP4 解析（设计文档 §6）。
 *
 * 只解析 stts / mdhd / mvhd / elst，不碰 stco/stsz/stsc —— 样本表不进内存，
 * 大文件也只占 KB 级（文档 §6.4）。顶层 box 遍历按 size 跳着走，
 * 不会为找一个 moov 把 2GB 文件读一遍。
 */
#include "internal.h"
#include <string.h>

typedef struct du_mp4_box {
    uint32_t type;
    uint64_t body_off;
    uint64_t body_end;   /* box 尾（含头），下一个 box 从这里开始 */
} du_mp4_box_t;

#define DU_TYPE(a, b, g, d) \
    ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(d))

/* 读 box 头，返回头长度；解析失败返回 0 */
static uint64_t du_mp4_box_read(du_source_t* s, uint64_t off, uint64_t end,
                                du_mp4_box_t* b) {
    if (off + 8 > end) return 0;
    uint64_t avail = 0;
    const uint8_t* p = du_io_view(s, off, 8, &avail);
    if (p == NULL || avail < 8) return 0;

    uint64_t size = du_be32(p);
    uint64_t head = 8;
    if (size == 1) {              /* 64 位 largesize，mdat 常见 */
        const uint8_t* q = du_io_view(s, off + 8, 8, &avail);
        if (q == NULL || avail < 8) return 0;
        size = du_be64(q);
        head = 16;
    } else if (size == 0) {       /* 延伸到文件尾（最后一个 box） */
        size = end - off;
    }
    if (size < head) return 0;              /* 恶意/损坏长度 */
    if (off + size > end) size = end - off; /* 截断到 remaining */
    b->type = du_be32(p + 4);
    b->body_off = off + head;
    b->body_end = off + size;
    return head;
}

/* ---- trak 信息收集 ---- */

typedef struct du_mp4_trak_info {
    int      is_audio;
    uint32_t handler;           /* hdlr 的 handler_type，'soun' 才算音频轨 */
    uint64_t stts_samples;      /* Σ count × delta */
    uint64_t stts_entries;
    uint64_t mdhd_duration;
    uint32_t mdhd_timescale;
    int      has_elst;
    uint64_t elst_skip;         /* media_time 对应的采样偏移 */
    uint32_t elst_flags;
} du_mp4_trak_info_t;

/* ---- stts：样本级时长，流式累加（文档 §6.2） ---- */

static void du_mp4_parse_stts(du_ctx_t* c, uint64_t off, uint64_t end,
                              du_mp4_trak_info_t* t) {
    du_source_t* s = c->src;
    uint8_t hdr[8];
    /* stts 是 full-box：version/flags 4 字节 + entry_count */
    if (du_io_read_at(s, off, hdr, 8) != 8) return;
    uint32_t count = du_be32(hdr + 4);
    uint64_t pos = off + 8;
    for (uint32_t i = 0; i < count && pos + 8 <= end; i++) {
        uint8_t e[8];
        if (du_io_read_at(s, pos, e, 8) != 8) break;
        uint64_t n = du_be32(e);
        uint64_t d = du_be32(e + 4);
        t->stts_samples += n * d;
        t->stts_entries++;
        pos += 8;
        if (c->opt->max_frames != 0 && t->stts_entries >= c->opt->max_frames) break;
    }
}

/* ---- elst：edit list 三元组（文档 §6.3） ---- */

static void du_mp4_parse_elst(du_ctx_t* c, uint64_t off, uint64_t end,
                              du_mp4_trak_info_t* t) {
    du_source_t* s = c->src;
    uint8_t h[8];
    if (du_io_read_at(s, off, h, 8) != 8) return;
    uint8_t version = h[0];
    uint32_t count = du_be32(h + 4);
    uint64_t pos = off + 8;
    uint32_t entry_size = (version == 1) ? 20 : 12;

    t->has_elst = 1;
    for (uint32_t i = 0; i < count && pos + entry_size <= end; i++) {
        uint8_t e[20];
        if (du_io_read_at(s, pos, e, entry_size) != (int64_t)entry_size) break;
        int64_t media_time;
        uint32_t rate;
        if (version == 1) {
            media_time = (int64_t)du_be64(e + 8);   /* segment_duration 在 media_time 前，音频时长用不到 */
            rate = du_be32(e + 16);
        } else {
            media_time = (int64_t)(int32_t)du_be32(e + 4);
            rate = du_be32(e + 8);
        }
        if (rate != 0x10000u) {   /* 16.16 定点 1.0 */
            t->elst_flags |= 1;   /* 变速轨道：语义复杂，拒绝自动修正 */
        }
        if (media_time > 0) {
            /* media_time 用 media timescale 计（ISO 14496-12），不是 movie
             * timescale。mdhd.timescale 通常就等于采样率，此时它就是采样偏移 */
            t->elst_skip += (uint64_t)media_time;
        }
        /* media_time == -1 是空白段，只累加 presentation 时长；
         * 音频时长以媒体采样为准，空白段不改变采样数 */
        pos += entry_size;
    }
}

/* ---- 深入 trak/mdia/minf/stbl ---- */

static void du_mp4_walk_children(du_ctx_t* c, uint64_t off, uint64_t end,
                                 int depth, du_mp4_trak_info_t* t,
                                 uint32_t* box_count, uint32_t* movie_ts);

static void du_mp4_walk_trak(du_ctx_t* c, uint64_t off, uint64_t end,
                             du_mp4_trak_info_t* t, uint32_t* box_count,
                             uint32_t* movie_ts) {
    du_source_t* s = c->src;
    uint64_t pos = off;

    while (pos + 8 <= end) {
        if (++(*box_count) > DU_MP4_MAX_BOXES) return;
        du_mp4_box_t b;
        if (du_mp4_box_read(s, pos, end, &b) == 0) break;
        switch (b.type) {
        case DU_TYPE('m', 'd', 'i', 'a'):
            du_mp4_walk_children(c, b.body_off, b.body_end, 1, t, box_count, movie_ts);
            break;
        case DU_TYPE('e', 'd', 't', 's'): {
            /* edts/elst 也在 trak 下，与 mdia 平级 */
            uint64_t pos2 = b.body_off;
            while (pos2 + 8 <= b.body_end) {
                du_mp4_box_t e;
                if (du_mp4_box_read(s, pos2, b.body_end, &e) == 0) break;
                if (e.type == DU_TYPE('e', 'l', 's', 't')) {
                    du_mp4_parse_elst(c, e.body_off, e.body_end, t);
                }
                pos2 = e.body_end;
            }
            break;
        }
        default:
            break;
        }
        pos = b.body_end;
    }

    /* mdhd/hdlr 都在 mdia 里（ISO 规范），由 walk_children 填进 t；
     * 这里只做音频轨判定 */
    if (t->handler == DU_TYPE('s', 'o', 'u', 'n')) {
        t->is_audio = 1;
    }
}

static void du_mp4_walk_children(du_ctx_t* c, uint64_t off, uint64_t end,
                                 int depth, du_mp4_trak_info_t* t,
                                 uint32_t* box_count, uint32_t* movie_ts) {
    du_source_t* s = c->src;
    if (depth > DU_MP4_MAX_DEPTH) return;
    uint64_t pos = off;
    while (pos + 8 <= end) {
        if (++(*box_count) > DU_MP4_MAX_BOXES) return;
        du_mp4_box_t b;
        if (du_mp4_box_read(s, pos, end, &b) == 0) break;
        switch (b.type) {
        case DU_TYPE('m', 'i', 'n', 'f'):
        case DU_TYPE('s', 't', 'b', 'l'):
            du_mp4_walk_children(c, b.body_off, b.body_end, depth + 1, t,
                                 box_count, movie_ts);
            break;
        case DU_TYPE('s', 't', 't', 's'):
            if (t != NULL) du_mp4_parse_stts(c, b.body_off, b.body_end, t);
            break;
        case DU_TYPE('m', 'd', 'h', 'd'): {
            /* mdhd 在 mdia 里（ISO 规范）；v1 是 64 位，长有声书会顶爆 v0 */
            if (t == NULL) break;
            uint8_t v[32];
            if (du_io_read_at(s, b.body_off, v, 32) >= 20) {
                if (v[0] == 1) {
                    if (du_io_read_at(s, b.body_off, v, 32) == 32) {
                        t->mdhd_timescale = du_be32(v + 20);
                        t->mdhd_duration = du_be64(v + 24);
                    }
                } else {
                    t->mdhd_timescale = du_be32(v + 12);
                    t->mdhd_duration = du_be32(v + 16);
                }
            }
            break;
        }
        case DU_TYPE('h', 'd', 'l', 'r'): {
            if (t == NULL) break;
            uint8_t v[12];
            /* body: version/flags(4) + pre_defined(4) + handler_type(4) */
            if (du_io_read_at(s, b.body_off, v, 12) == 12) {
                t->handler = du_be32(v + 8);
            }
            break;
        }
        case DU_TYPE('m', 'v', 'h', 'd'): {
            uint8_t v[32];
            if (du_io_read_at(s, b.body_off, v, 32) >= 20) {
                if (v[0] == 1) {
                    if (du_io_read_at(s, b.body_off, v, 32) == 32) {
                        *movie_ts = du_be32(v + 20);
                    }
                } else {
                    *movie_ts = du_be32(v + 12);
                }
            }
            break;
        }
        default:
            /* meta 是 full-box（body 前有 4 字节 version/flags），Apple 变体
             * 又没有 —— 两种变体时长信息都不在里面，按未知 box 跳过即可，
             * 不会错解析（文档 §6.1 要求的兼容） */
            break;
        }
        pos = b.body_end;
    }
}

/* ---- iTunSMPB 文本搜索（gapless hint，文档 §6.3） ---- */

static int du_mp4_hex4(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t ch = p[i];
        v <<= 4;
        if (ch >= '0' && ch <= '9') v |= ch - '0';
        else if (ch >= 'a' && ch <= 'f') v |= ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') v |= ch - 'A' + 10;
        else return -1;
    }
    return (int)v;
}

static void du_mp4_find_itunsmpb(du_ctx_t* c, uint64_t off, uint64_t end) {
    du_source_t* s = c->src;
    const char magic[8] = "iTunSMPB";
    uint64_t pos = off;
    while (pos + 8 < end) {
        uint64_t avail = 0;
        const uint8_t* p = du_io_view(s, pos, 1, &avail);
        if (p == NULL || avail == 0) break;
        for (uint64_t i = 0; i + 8 < avail && pos + i + 23 < end; i++) {
            if (p[i] != 'i') continue;
            if (memcmp(p + i, magic, 8) != 0) continue;
            /* 后面是空格分隔的 hex 文本：前置空帧 / 后置空帧 / 总时长 */
            const uint8_t* d = du_io_view(s, pos + i + 8, 27, &avail);
            if (d == NULL || avail < 27) break;
            /* 跳过 magic 后的非 hex 字符（空格等） */
            uint64_t q = 0;
            while (q < avail && !((d[q] >= '0' && d[q] <= '9') ||
                                  (d[q] >= 'a' && d[q] <= 'f') ||
                                  (d[q] >= 'A' && d[q] <= 'F'))) q++;
            int pad_start = -1, pad_end = -1;
            if (q + 9 <= avail) pad_start = du_mp4_hex4(d + q);
            if (pad_start >= 0) {
                q += 8;
                while (q < avail && !((d[q] >= '0' && d[q] <= '9') ||
                                      (d[q] >= 'a' && d[q] <= 'f') ||
                                      (d[q] >= 'A' && d[q] <= 'F'))) q++;
                if (q + 9 <= avail) pad_end = du_mp4_hex4(d + q);
            }
            if (pad_start >= 0 && pad_end >= 0) {
                c->res->flags |= DU_F_GAPLESS_HINT;
                c->gapless_trim = (uint64_t)pad_start + (uint64_t)pad_end;
            }
            return;
        }
        pos += avail - (avail > 7 ? 7 : 0);   /* 带 7 字节重叠防跨界漏配 */
    }
}

/* ---- 主探测 ---- */

du_status_t du_probe_mp4(du_ctx_t* c) {
    du_source_t* s = c->src;
    du_result_t* r = c->res;
    int64_t sz64 = du_io_size(s);
    if (sz64 < 0) DU_FAIL(c, DU_ERR_IO);
    uint64_t end = (uint64_t)sz64;
    if (end < 16) DU_FAIL(c, DU_ERR_NOT_CONTAINER);

    uint64_t off = 0;
    uint32_t box_count = 0;
    int saw_moov = 0, saw_moof = 0;
    uint64_t moov_off = 0, moov_end = 0;
    uint64_t mdat_bytes = 0;

    while (off + 8 <= end) {
        if (++box_count > DU_MP4_MAX_BOXES) break;
        du_mp4_box_t b;
        if (du_mp4_box_read(s, off, end, &b) == 0) break;
        if (b.type == DU_TYPE('m', 'o', 'o', 'v')) {
            moov_off = b.body_off;
            moov_end = b.body_end;
            saw_moov = 1;
        } else if (b.type == DU_TYPE('m', 'o', 'o', 'f')) {
            saw_moof = 1;
        } else if (b.type == DU_TYPE('m', 'd', 'a', 't')) {
            mdat_bytes += b.body_end - b.body_off;
        }
        if (b.body_end <= off) break;
        off = b.body_end;
    }

    if (!saw_moov) {
        /* 碎片化/未完成下载：不做猜测（文档 §6.4） */
        DU_FAIL(c, saw_moof ? DU_ERR_NOT_CONTAINER : DU_ERR_NOT_CONTAINER);
    }

    du_mp4_trak_info_t traks[16];
    memset(traks, 0, sizeof(traks));
    uint32_t ntrak = 0;
    uint32_t movie_ts = 0;

    du_mp4_find_itunsmpb(c, moov_off, moov_end);
    du_mp4_walk_children(c, moov_off, moov_end, 0, NULL, &box_count, &movie_ts);

    /* 再走一遍 trak（walk_children 对 trak 也统一跳过了——
     * 这里单独扫 moov 子 box 找 trak 下钻） */
    {
        uint64_t pos = moov_off;
        while (pos + 8 <= moov_end && box_count < DU_MP4_MAX_BOXES) {
            du_mp4_box_t b;
            if (du_mp4_box_read(s, pos, moov_end, &b) == 0) break;
            if (b.type == DU_TYPE('t', 'r', 'a', 'k') && ntrak < 16) {
                du_mp4_trak_info_t t;
                memset(&t, 0, sizeof(t));
                du_mp4_walk_trak(c, b.body_off, b.body_end, &t, &box_count, &movie_ts);
                if (t.is_audio) {
                    if (t.has_elst) r->flags |= DU_F_ELST_PRESENT;
                    if (t.elst_flags & 1) r->flags |= DU_F_ELST_RATE;
                    traks[ntrak++] = t;
                }
            }
            pos = b.body_end;
        }
    }

    if (ntrak == 0) DU_FAIL(c, DU_ERR_NOT_CONTAINER);
    if (ntrak > 1) r->flags |= DU_F_MULTI_AUDIO;

    /* 选音频轨：多条时取时长最长（交叉相乘比较，避免除法与浮点） */
    int best = 0;
    for (uint32_t i = 1; i < ntrak; i++) {
        du_mp4_trak_info_t* a = &traks[best];
        du_mp4_trak_info_t* b = &traks[i];
        uint64_t as = a->stts_samples ? a->stts_samples : a->mdhd_duration;
        uint64_t bs = b->stts_samples ? b->stts_samples : b->mdhd_duration;
        /* as/a.ts vs bs/b.ts → as * b.ts vs bs * a.ts（均 < 2^60，不溢出） */
        if (bs * (uint64_t)(a->mdhd_timescale ? a->mdhd_timescale : 1) >
            as * (uint64_t)(b->mdhd_timescale ? b->mdhd_timescale : 1)) {
            best = (int)i;
        }
    }

    du_mp4_trak_info_t* t = &traks[best];
    uint32_t ts = t->mdhd_timescale;
    if (ts == 0) DU_FAIL(c, DU_ERR_NOT_CONTAINER);

    uint64_t samples = t->stts_samples ? t->stts_samples : t->mdhd_duration;
    if (samples == 0) DU_FAIL(c, DU_ERR_NO_SYNC);

    /* edit list：默认只报告不应用，elst_apply=1 才裁剪（文档 §6.3） */
    if (t->has_elst && c->opt->elst_apply != 0 && t->elst_skip > 0 &&
        !(t->elst_flags & 1)) {
        samples = (samples > t->elst_skip) ? samples - t->elst_skip : 0;
    }
    if (c->opt->gapless_correct != 0 && c->gapless_trim > 0) {
        samples = (samples > c->gapless_trim) ? samples - c->gapless_trim : 0;
    }

    r->duration_samples = samples;
    r->duration_sec = (double)samples / (double)ts;
    r->sample_rate = ts;   /* AAC 场景 mdhd.timescale 即采样率 */
    r->channels = 0;
    r->bit_depth = 0;
    r->audio_bytes = mdat_bytes;
    r->bytes_scanned = moov_end - moov_off;
    c->st = DU_OK;
    return c->st;
}
