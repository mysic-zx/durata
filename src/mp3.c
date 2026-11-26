/* MP3 逐帧扫描状态机（设计文档 §5）。
 *
 * 准确率的分水岭在两处：
 * 1. 帧头完整字段校验 + 双帧验证（Double Sync），抗 MPEG 数据里的假同步；
 * 2. EOF 特判：最后一帧后面没有"下一帧"可验，必须降级为单帧认定，
 *    否则每个正常文件的最后一帧都会被丢掉。
 *
 * 全程整数累加 samples，最后才做一次除法出 duration_sec。
 */
#include "internal.h"
#include <string.h>

/* ---- 帧头字段合法性（文档 §5.2 的保留值表） ---- */

static int du_mp3_valid_sync(const uint8_t* p) {
    return p[0] == 0xFF && (p[1] & 0xE0) == 0xE0;
}

static int du_mp3_valid_fields(const uint8_t* p) {
    uint32_t version = (p[1] >> 3) & 3;    /* 01 = 保留 */
    uint32_t layer   = (p[1] >> 1) & 3;    /* 00 = 保留 */
    uint32_t br_idx  = (p[2] >> 4) & 0xF;  /* 1111 = 保留 */
    uint32_t sr_idx  = (p[2] >> 2) & 3;    /* 11 = 保留 */
    return version != 1 && layer != 0 && br_idx != 15 && sr_idx != 3;
}

static int du_mp3_valid_head(const uint8_t* p) {
    return du_mp3_valid_sync(p) && du_mp3_valid_fields(p);
}

/* ---- free-format：向后搜索下一帧头确定帧长（文档 §5.4） ---- */

static uint64_t du_mp3_free_format_len(du_ctx_t* c, uint64_t p, uint64_t audio_end) {
    du_source_t* s = c->src;
    uint64_t q = p + 4;
    /* free 帧没有理论上限，但搜索窗口给 128KB 封顶，防恶意文件拖死 */
    uint64_t limit = (q + 128 * 1024 < audio_end) ? q + 128 * 1024 : audio_end;
    while (q + 4 <= limit) {
        uint64_t avail = 0;
        const uint8_t* h = du_io_view(s, q, 4, &avail);
        if (h == NULL || avail < 4) break;
        if (du_mp3_valid_head(h)) return q - p;
        q++;
    }
    return 0;
}

/* ---- Xing / VBRI / LAME tag（文档 §4.2 交叉校验字段） ---- */

static uint32_t du_mp3_side_info(uint32_t version, uint32_t mode) {
    /* mode 3 = mono；MPEG1 大一倍 */
    uint32_t bytes = (version == 3) ? 32 : 17;
    return (mode == 3) ? (version == 3 ? 17 : 9) : bytes;
}

static void du_mp3_parse_xing(du_ctx_t* c, uint64_t frame_off, uint32_t flen,
                              uint32_t version, uint32_t mode) {
    du_source_t* s = c->src;
    du_result_t* r = c->res;
    if (c->opt->read_xing == 0) return;

    uint64_t off = frame_off + 4 + du_mp3_side_info(version, mode);
    if (off + 16 > frame_off + flen) return;

    uint64_t avail = 0;
    const uint8_t* p = du_io_view(s, off, 8, &avail);
    if (p == NULL || avail < 8) return;

    uint32_t xframes = 0, xbytes = 0;

    if (memcmp(p, "Xing", 4) == 0 || memcmp(p, "Info", 4) == 0) {
        uint32_t flags = du_be32(p + 4);
        uint64_t q = off + 8;
        if (flags & 1) {   /* frames */
            if (q + 4 > frame_off + flen) return;
            const uint8_t* f = du_io_view(s, q, 4, &avail);
            if (f == NULL || avail < 4) return;
            xframes = du_be32(f);
            q += 4;
        }
        if (flags & 2) {   /* bytes */
            if (q + 4 > frame_off + flen) return;
            const uint8_t* f = du_io_view(s, q, 4, &avail);
            if (f == NULL || avail < 4) return;
            xbytes = du_be32(f);
            q += 4;
        }
        /* toc(100) / vbrscale(4) 只需要跳过 */
        if (flags & 1) {
            r->flags |= DU_F_XING_PRESENT;
            r->xing_frames = xframes;
            r->xing_bytes = xbytes;
        }
    } else if (memcmp(p, "VBRI", 4) == 0) {
        /* VBRI：固定 offset 帧头后 32 字节处，frames/bytes 位置不同 */
        const uint8_t* v = du_io_view(s, off + 14, 8, &avail);
        if (v == NULL || avail < 8) return;
        xbytes = du_be32(v);       /* +14: bytes */
        xframes = du_be32(v + 4);  /* +18: frames */
        r->flags |= DU_F_XING_PRESENT;
        r->xing_frames = xframes;
        r->xing_bytes = xbytes;
    }

    /* LAME tag：在首帧数据区里搜 "LAME"/"Lavf"，delay/padding 在标记后 +20 的
     * 3 字节里，前 12 位 delay、后 12 位 padding（hydrogenaudio LAME header 规范） */
    uint64_t search_end = frame_off + flen;
    if ((r->flags & DU_F_XING_PRESENT) && search_end > off + 16 + 20) {
        for (uint64_t q = off + 4; q + 23 <= search_end; q++) {
            const uint8_t* l = du_io_view(s, q, 4, &avail);
            if (l == NULL || avail < 4) break;
            if (memcmp(l, "LAME", 4) == 0 || memcmp(l, "Lavf", 4) == 0) {
                const uint8_t* d = du_io_view(s, q + 20, 3, &avail);
                if (d != NULL && avail >= 3) {
                    uint32_t delay = ((uint32_t)d[0] << 4) | (d[1] >> 4);
                    uint32_t padding = ((uint32_t)(d[1] & 0xF) << 8) | d[2];
                    r->flags |= DU_F_XING_LAME | DU_F_GAPLESS_HINT;
                    if (delay + padding > 0) {
                        c->gapless_trim = (uint64_t)delay + padding;
                    }
                }
                break;
            }
            /* 快速跳过明显不是 ASCII 标记的区域 */
            if (l[0] < 0x20 || l[0] > 0x7E) q += 3;
        }
    }
}

/* ---- 主探测 ---- */

/* 收尾：把整数采样换算成秒，算 Xing 偏差。两条出口（fast/全扫）都要走 */
static void du_mp3_finish(du_ctx_t* c, uint32_t sr) {
    du_result_t* r = c->res;
    r->duration_sec = (sr != 0)
        ? (double)r->duration_samples / (double)sr : 0.0;
    if ((r->flags & DU_F_XING_PRESENT) && r->xing_frames > 0 && r->frames > 0) {
        r->xing_delta = (double)((int64_t)r->xing_frames - (int64_t)r->frames)
                        / (double)r->frames;
        int64_t diff = (int64_t)r->xing_frames - (int64_t)r->frames;
        if (diff < 0) diff = -diff;
        if ((uint64_t)diff * 1000u < r->frames) r->flags |= DU_F_XING_AGREE;
    } else if (!(r->flags & DU_F_XING_PRESENT)) {
        r->xing_delta = -1.0;
    }
}

du_status_t du_probe_mp3(du_ctx_t* c) {
    du_source_t* s = c->src;
    du_result_t* r = c->res;

    int64_t fsize64 = du_io_size(s);
    if (fsize64 < 0) DU_FAIL(c, DU_ERR_IO);
    uint64_t fsize = (uint64_t)fsize64;

    /* 1. 尾部标签（ID3v1 / APEv2），audio 区间止于此 */
    uint64_t tail = du_util_mp3_tail_tags(s, &r->flags);
    uint64_t audio_end = (fsize > tail) ? fsize - tail : 0;

    /* 2. 头部 ID3v2（v2.2 用 3 字节长度，v2.3/v2.4 用 synchsafe） */
    uint64_t p = 0;
    if (audio_end >= 10) {
        uint64_t avail = 0;
        const uint8_t* h = du_io_view(s, 0, 10, &avail);
        if (h != NULL && avail >= 10 && memcmp(h, "ID3", 3) == 0 &&
            h[3] != 0xFF) {
            uint32_t tagsize = (h[3] == 2)
                ? du_be24(h + 6)
                : du_synchsafe32(h + 6);
            p = 10 + (uint64_t)tagsize;
            if (h[5] & 0x10) p += 10;   /* footer */
            if (p >= audio_end) DU_FAIL(c, DU_ERR_NO_SYNC);
        }
    }

    /* 3. 逐帧扫描 */
    uint64_t samples = 0;
    uint64_t bytes_sum = 0;
    uint64_t br_num = 0;              /* Σ kbps × flen，最后换 bps */
    uint32_t last_kbps = 0;
    uint32_t resync_count = 0;
    uint32_t bad_headers = 0;
    uint32_t first_sr = 0;
    uint32_t first_ch = 0;
    c->gapless_trim = 0;
    int truncated = 0;

    while (p + 4 <= audio_end) {
        if (c->opt->max_bytes != 0 && p >= c->opt->max_bytes) break;
        if (c->opt->max_frames != 0 && r->frames >= c->opt->max_frames) break;

        uint64_t avail = 0;
        const uint8_t* h = du_io_view(s, p, 4, &avail);
        if (h == NULL) DU_FAIL(c, DU_ERR_IO);
        if (avail < 4) { r->flags |= DU_F_TRUNCATED; break; }

        if (!du_mp3_valid_head(h)) {
            if (du_mp3_valid_sync(h)) bad_headers++;
            p++;
            continue;
        }

        uint32_t version = (h[1] >> 3) & 3;
        uint32_t layer   = (h[1] >> 1) & 3;
        uint32_t br_idx  = (h[2] >> 4) & 0xF;
        uint32_t sr_idx  = (h[2] >> 2) & 3;
        uint32_t mode    = (h[3] >> 6) & 3;
        du_mp3_frame_info_t fi = du_mp3_frame_info(version, layer, br_idx, sr_idx);
        if (fi.sr == 0 || fi.spf == 0) { p++; continue; }

        uint32_t flen;
        if (fi.kbps == 0) {   /* bitrate_index == 0：free-format */
            if (c->opt->free_format != 0) {   /* SKIP 策略 */
                bad_headers++;
                p += 4;
                continue;
            }
            uint64_t flen64 = du_mp3_free_format_len(c, p, audio_end);
            if (flen64 == 0 || flen64 > 0xFFFF) { bad_headers++; p++; continue; }
            flen = (uint32_t)flen64;
        } else {
            flen = fi.coef * fi.kbps * 1000u / fi.sr + ((h[2] >> 1) & 1);
            flen *= fi.mul;
        }
        if (flen < 4 || p + flen > audio_end) {
            /* 末帧不完整：已有的都有效，本帧残缺不计 */
            r->flags |= DU_F_TRUNCATED;
            truncated = 1;
            break;
        }

        /* 双帧验证：下一帧头也合法才算真帧；EOF 处降级为单帧认定 */
        if (p + flen + 4 <= audio_end) {
            uint64_t avail2 = 0;
            const uint8_t* nh = du_io_view(s, p + flen, 4, &avail2);
            if (nh == NULL || avail2 < 4 || !du_mp3_valid_head(nh)) {
                if (c->opt->strict) DU_FAIL(c, DU_ERR_NO_SYNC);
                resync_count++;
                p++;   /* 逐字节推进找回对齐；hint 窗口搜索属于 P2 调优项 */
                continue;
            }
        }

        /* 首帧：记录参数、解析 Xing */
        if (r->frames == 0) {
            first_sr = fi.sr;
            first_ch = (mode == 3) ? 1 : 2;
            du_mp3_parse_xing(c, p, flen, version, mode);
            /* fast_mp3：信 Xing 直接出结果，不逐帧扫 */
            if (c->opt->fast_mp3 != 0 && (r->flags & DU_F_XING_PRESENT) &&
                r->xing_frames > 0) {
                r->duration_samples = (uint64_t)r->xing_frames * fi.spf;
                r->sample_rate = fi.sr;
                r->channels = first_ch;
                r->frames = r->xing_frames;
                r->audio_bytes = (r->xing_bytes > 0) ? r->xing_bytes : 0;
                r->bitrate_avg_bps = (r->xing_bytes > 0 && r->duration_samples > 0)
                    ? r->xing_bytes * 8 * fi.sr / r->duration_samples : 0;
                r->bytes_scanned = 4 + (uint64_t)flen;
                du_mp3_finish(c, first_sr);
                c->st = DU_OK;
                return c->st;
            }
        }

        /* 累加：整数，无误差 */
        samples += fi.spf;
        r->frames++;
        bytes_sum += flen;
        br_num += (uint64_t)fi.kbps * flen;
        /* free-format 帧没有码率概念（kbps=0），不参与 VBR 判定，
         * 否则前后码率一比会误标 */
        if (fi.kbps != 0) {
            if (last_kbps != 0 && fi.kbps != last_kbps) r->flags |= DU_F_VBR;
            last_kbps = fi.kbps;
        }
        if (resync_count > 0) r->flags |= DU_F_RESYNC;
        p += flen;
    }

    if (r->frames == 0) {
        r->bad_header_count = bad_headers;
        r->resync_count = resync_count;
        DU_FAIL(c, DU_ERR_NO_SYNC);
    }

    /* gapless_correct：扣 LAME delay/padding（文档 §4.4，默认关） */
    if (c->opt->gapless_correct != 0 && c->gapless_trim > 0) {
        samples = (samples > c->gapless_trim) ? samples - c->gapless_trim : 0;
    }

    r->duration_samples = samples;
    r->sample_rate = first_sr;
    r->channels = first_ch;
    r->audio_bytes = bytes_sum;
    r->bitrate_avg_bps = (bytes_sum > 0) ? br_num * 1000u / bytes_sum : 0;
    r->bytes_scanned = p;
    r->resync_count = resync_count;
    r->bad_header_count = bad_headers;
    du_mp3_finish(c, first_sr);
    /* 结果已填好；截断用独立状态码告诉调用方"可用但有残缺" */
    c->st = truncated ? DU_ERR_TRUNCATED : DU_OK;
    return c->st;
}
