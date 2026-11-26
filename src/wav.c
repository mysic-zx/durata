/* WAV / AIFF：容器里直接写明采样数，权威字段现成（文档 §7）
 * WAV(RIFF) 小端 chunk；AIFF(FORM) 大端 chunk，采样率是 80 位浮点。
 * RF64 的 64 位长度扩展一并处理（W64 结构完全不同，遇到时返回
 * DU_ERR_UNSUPPORTED，后续需要再补）。
 */
#include "internal.h"
#include <string.h>

/* 80 位 IEEE extended float → 整数采样率（44100/48000 这类都精确） */
static uint32_t du_wav_aiff_rate(const uint8_t* b) {
    uint32_t e = ((uint32_t)(b[0] & 0x7F) << 8) | b[1];
    uint64_t m = du_be64(b + 2);
    int32_t shift = (int32_t)e - 16383 - 63;
    if (shift >= 0) {
        if (shift > 32) return 0;
        return (uint32_t)(m << shift);
    }
    if (-shift > 63) return 0;
    return (uint32_t)(m >> -shift);
}

du_status_t du_probe_wav(du_ctx_t* c) {
    du_source_t* s = c->src;
    du_result_t* r = c->res;
    int64_t sz64 = du_io_size(s);
    if (sz64 < 0) DU_FAIL(c, DU_ERR_IO);
    uint64_t fsize = (uint64_t)sz64;

    uint64_t avail = 0;
    const uint8_t* h = du_io_view(s, 0, 12, &avail);
    if (h == NULL || avail < 12) DU_FAIL(c, DU_ERR_NOT_CONTAINER);

    int is_aiff, is_rf64;
    if (memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0) {
        is_aiff = 0; is_rf64 = 0;              /* 标准 WAV */
    } else if (memcmp(h, "RF64", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0) {
        is_aiff = 0; is_rf64 = 1;              /* RF64：64 位长度扩展 */
    } else if (memcmp(h, "FORM", 4) == 0 &&
               (memcmp(h + 8, "AIFF", 4) == 0 || memcmp(h + 8, "AIFC", 4) == 0)) {
        is_aiff = 1; is_rf64 = 0;
    } else {
        DU_FAIL(c, DU_ERR_NOT_CONTAINER);
    }

    uint32_t sample_rate = 0, channels = 0, bit_depth = 0;
    uint64_t data_bytes = 0, ds64_data = 0, comm_frames = 0;
    int have_fmt = 0, have_comm = 0, have_ds64 = 0;

    uint64_t pos = 12;
    while (pos + 8 <= fsize) {
        const uint8_t* ch = du_io_view(s, pos, 8, &avail);
        if (ch == NULL || avail < 8) break;
        uint64_t csize;
        if (is_aiff) {
            csize = du_be32(ch + 4);
        } else {
            csize = (uint64_t)ch[4] | ((uint64_t)ch[5] << 8) |
                    ((uint64_t)ch[6] << 16) | ((uint64_t)ch[7] << 24);
        }
        uint64_t body_off = pos + 8;
        if (body_off + csize > fsize) csize = fsize - body_off;   /* 截断防越界 */

        if (is_aiff) {
            if (memcmp(ch, "COMM", 4) == 0 && csize >= 18) {
                const uint8_t* d = du_io_view(s, body_off, 18, &avail);
                if (d != NULL && avail >= 18) {
                    channels = ((uint32_t)d[0] << 8) | d[1];
                    comm_frames = ((uint64_t)d[2] << 24) | ((uint64_t)d[3] << 16) |
                                  ((uint64_t)d[4] << 8) | d[5];
                    bit_depth = ((uint32_t)d[6] << 8) | d[7];
                    sample_rate = du_wav_aiff_rate(d + 8);
                    have_comm = 1;
                }
            }
        } else if (memcmp(ch, "ds64", 4) == 0 && csize >= 24) {
            /* RF64 扩展：riffSize(8) + dataSize(8) + sampleCount(8) + table(4)，
             * RIFF 家族全小端 */
            const uint8_t* d = du_io_view(s, body_off, 24, &avail);
            if (d != NULL && avail >= 24) {
                ds64_data = (uint64_t)d[8] | ((uint64_t)d[9] << 8) |
                            ((uint64_t)d[10] << 16) | ((uint64_t)d[11] << 24) |
                            ((uint64_t)d[12] << 32) | ((uint64_t)d[13] << 40) |
                            ((uint64_t)d[14] << 48) | ((uint64_t)d[15] << 56);
                have_ds64 = 1;
            }
        } else if (memcmp(ch, "fmt ", 4) == 0 && csize >= 16) {
            const uint8_t* d = du_io_view(s, body_off, 16, &avail);
            if (d != NULL && avail >= 16) {
                channels = (uint32_t)d[2] | ((uint32_t)d[3] << 8);
                sample_rate = (uint32_t)d[4] | ((uint32_t)d[5] << 8) |
                              ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
                bit_depth = (uint32_t)d[14] | ((uint32_t)d[15] << 8);
                have_fmt = 1;
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            if (is_rf64) {
                /* RF64 里 data 的 size 字段写 0xFFFFFFFF，真值在 ds64 */
                if (have_ds64) data_bytes = ds64_data;
                else if (fsize > body_off) data_bytes = fsize - body_off;
            } else {
                data_bytes = csize;
            }
        }

        pos = body_off + csize + (csize & 1);   /* chunk 按偶数对齐 */
    }

    if (is_aiff) {
        if (!have_comm || sample_rate == 0) DU_FAIL(c, DU_ERR_NOT_CONTAINER);
        if (comm_frames == 0) DU_FAIL(c, DU_ERR_NO_SYNC);
        r->duration_samples = comm_frames;
        r->duration_sec = (double)comm_frames / (double)sample_rate;
        r->sample_rate = sample_rate;
        r->channels = channels;
        r->bit_depth = bit_depth;
        r->audio_bytes = comm_frames * (uint64_t)channels * ((uint64_t)bit_depth / 8);
        r->bytes_scanned = fsize;
        c->st = DU_OK;
        return c->st;
    }

    if (!have_fmt || sample_rate == 0) DU_FAIL(c, DU_ERR_NOT_CONTAINER);
    if (data_bytes == 0) DU_FAIL(c, DU_ERR_NO_SYNC);

    /* PCM 类：data_size / 块字节数 = 采样帧数 */
    uint64_t block_align = (uint64_t)channels * (bit_depth / 8);
    if (block_align == 0) DU_FAIL(c, DU_ERR_UNSUPPORTED);
    uint64_t samples = data_bytes / block_align;
    if (samples == 0) DU_FAIL(c, DU_ERR_NO_SYNC);

    r->duration_samples = samples;
    r->duration_sec = (double)samples / (double)sample_rate;
    r->sample_rate = sample_rate;
    r->channels = channels;
    r->bit_depth = bit_depth;
    r->audio_bytes = data_bytes;
    r->bytes_scanned = fsize;
    c->st = DU_OK;
    return c->st;
}
