/* FLAC：STREAMINFO 定位时长，total_samples==0（流式录制）降级逐帧扫（文档 §7） */
#include "internal.h"
#include <string.h>

/* metadata block 头：1 字节（0x80=last + type），3 字节长度 */
#define FLAC_BLOCK_LAST 0x80u
#define FLAC_BLOCK_STREAMINFO 0x00u

/* STREAMINFO body 固定 34 字节：
 * min_block(2) max_block(2) min_frame(3) max_frame(3) 位包(8) MD5(16)，
 * 采样参数在 offset 10..17 的 8 字节位包里 */
static du_status_t du_flac_parse_streaminfo(du_ctx_t* c, const uint8_t* b,
                                            uint32_t* sr, uint32_t* ch,
                                            uint32_t* bps, uint64_t* total) {
    const uint8_t* w = b + 10;
    *sr  = ((uint32_t)w[0] << 12) | ((uint32_t)w[1] << 4) | (w[2] >> 4);
    *ch  = (((uint32_t)w[2] >> 1) & 7) + 1;
    *bps = ((((uint32_t)w[2] & 1) << 4) | (w[3] >> 4)) + 1;
    *total = ((uint64_t)(w[3] & 0x0F) << 32) | ((uint64_t)w[4] << 24) |
             ((uint64_t)w[5] << 16) | ((uint64_t)w[6] << 8) | w[7];
    if (*sr == 0) DU_FAIL(c, DU_ERR_NOT_CONTAINER);
    return DU_OK;
}

/* ---- total==0 时的逐帧兜底：sync 搜索 + 帧头 CRC8 验证 ----
 *
 * FLAC 帧体不定长且无长度字段，精确跳帧需要解码子帧，这里按
 * sync 搜索推进，靠帧头 CRC8 把误配压到很低。
 */

/* 解析帧头 blocksize；*head_len 为不含 CRC 的帧头长度（4+扩展） */
static uint32_t du_flac_frame_blocksize(const uint8_t* p, uint64_t avail,
                                        uint64_t* head_len) {
    uint32_t code = p[2] >> 4;
    uint64_t extra = 0;
    uint32_t bs = 0;
    switch (code) {
    case 1: bs = 192; break;
    case 2: case 3: case 4: case 5: bs = 576u << (code - 2); break;
    case 6: extra = 1; if (avail < 4 + extra) return 0; bs = (uint32_t)p[3] + 1; break;
    case 7: extra = 2; if (avail < 4 + extra) return 0;
            bs = (((uint32_t)p[3] << 8) | p[4]) + 1; break;
    case 8: case 9: case 10: case 11:
    case 12: case 13: case 14: case 15: bs = 256u << (code - 8); break;
    default: return 0;   /* 0 = 保留 */
    }
    *head_len = 4 + extra;
    return bs;
}

static uint64_t du_flac_scan_frames(du_ctx_t* c, uint64_t off, uint64_t end) {
    du_source_t* s = c->src;
    uint64_t samples = 0;
    uint64_t q = off;
    while (q + 6 <= end) {
        uint64_t avail = 0;
        const uint8_t* p = du_io_view(s, q, 8, &avail);
        if (p == NULL || avail < 6) break;
        /* frame sync：b0=0xFF，b1 高 6 位 111110，最低位保留须 0 */
        if (p[0] == 0xFF && (p[1] & 0xFE) == 0xF8) {
            uint64_t head_len = 0;
            uint32_t bs = du_flac_frame_blocksize(p, avail, &head_len);
            /* 帧头 CRC8：覆盖 sync 到 bps 字节，CRC 本体在 head_len 处 */
            if (bs != 0 && q + head_len < end) {
                const uint8_t* full = du_io_view(s, q, head_len + 1, &avail);
                if (full != NULL && avail >= head_len + 1) {
                    uint8_t crc = (uint8_t)du_util_crc8_flac(0, full,
                                                             (size_t)head_len);
                    if (crc == full[head_len]) {
                        samples += bs;
                        c->res->frames++;
                        q += head_len;   /* 帧体跳过，继续搜下一个 sync */
                        continue;
                    }
                }
            }
        }
        q++;
    }
    return samples;
}

du_status_t du_probe_flac(du_ctx_t* c) {
    du_source_t* s = c->src;
    du_result_t* r = c->res;
    int64_t sz64 = du_io_size(s);
    if (sz64 < 0) DU_FAIL(c, DU_ERR_IO);
    uint64_t fsize = (uint64_t)sz64;

    uint64_t avail = 0;
    const uint8_t* h = du_io_view(s, 0, 8, &avail);
    if (h == NULL || avail < 8 || memcmp(h, "fLaC", 4) != 0) {
        DU_FAIL(c, DU_ERR_NOT_CONTAINER);
    }

    uint64_t pos = 4;
    int saw_streaminfo = 0;
    uint32_t sr = 0, ch = 0, bps = 0;
    uint64_t total = 0;

    /* 遍历 metadata block，STREAMINFO 固定是第一个 */
    while (pos + 4 <= fsize) {
        const uint8_t* bh = du_io_view(s, pos, 4, &avail);
        if (bh == NULL || avail < 4) break;
        int last = (bh[0] & FLAC_BLOCK_LAST) != 0;
        uint32_t type = bh[0] & 0x7F;
        uint64_t blen = ((uint64_t)bh[1] << 16) | ((uint64_t)bh[2] << 8) | bh[3];
        pos += 4;
        if (type == FLAC_BLOCK_STREAMINFO && blen >= 34 && !saw_streaminfo) {
            const uint8_t* si = du_io_view(s, pos, 18, &avail);
            if (si == NULL || avail < 18) DU_FAIL(c, DU_ERR_IO);
            du_status_t st = du_flac_parse_streaminfo(c, si, &sr, &ch, &bps, &total);
            if (st != DU_OK) return st;
            saw_streaminfo = 1;
        }
        pos += blen;
        if (last) break;
    }
    if (!saw_streaminfo) DU_FAIL(c, DU_ERR_NOT_CONTAINER);
    if (pos > fsize) pos = fsize;

    uint64_t samples;
    if (total > 0) {
        samples = total;
    } else {
        /* 流式录制：STREAMINFO 写 0，只能逐帧数（文档 §7 的兜底） */
        samples = du_flac_scan_frames(c, pos, fsize);
        r->flags |= DU_F_TRUNCATED;
        if (samples == 0) {
            if (c->opt->strict) DU_FAIL(c, DU_ERR_NO_SYNC);
        }
    }
    if (samples == 0) DU_FAIL(c, DU_ERR_NO_SYNC);

    r->duration_samples = samples;
    r->duration_sec = (double)samples / (double)sr;
    r->sample_rate = sr;
    r->channels = ch;
    r->bit_depth = bps;
    r->audio_bytes = fsize - pos;
    r->bytes_scanned = pos;
    c->st = DU_OK;
    return c->st;
}
