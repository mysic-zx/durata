/* OGG：最后一页 granulepos 是容器规范定义的权威值（文档 §7）。
 * 从文件尾向前找最后一个有效页，采样率取首个 BOS 页的 ID header：
 * Vorbis 读 audio_sample_rate，Opus 固定 48000（pre-skip 要扣），
 * FLAC-in-Ogg 读 STREAMINFO。拿不到 ID header 就报错，不做猜测。
 */
#include "internal.h"
#include <string.h>

#define OGG_PAGE_SEARCH 4096

/* 页头 27 字节 + segtable。granulepos 在 +6（8 字节小端） */
static int du_ogg_page_at(du_source_t* s, uint64_t off, uint64_t end,
                          uint64_t* granule, uint64_t* page_len,
                          uint32_t* hdr_type) {
    if (off + 27 > end) return 0;
    uint64_t avail = 0;
    const uint8_t* p = du_io_view(s, off, 64, &avail);
    if (p == NULL || avail < 27) return 0;
    if (memcmp(p, "OggS", 4) != 0 || p[4] != 0) return 0;

    *granule = (uint64_t)p[6] | ((uint64_t)p[7] << 8) | ((uint64_t)p[8] << 16) |
               ((uint64_t)p[9] << 24) | ((uint64_t)p[10] << 32) |
               ((uint64_t)p[11] << 40) | ((uint64_t)p[12] << 48) |
               ((uint64_t)p[13] << 56);
    uint8_t nseg = p[26];
    uint64_t body = 0;
    const uint8_t* st = du_io_view(s, off + 27, nseg, &avail);
    if (st == NULL || avail < nseg) return 0;
    for (uint32_t i = 0; i < nseg; i++) body += st[i];
    *page_len = 27 + nseg + body;
    *hdr_type = p[5];
    return 1;
}

/* BOS 页的 ID header → 采样率 + Opus pre-skip */
static int du_ogg_parse_bos(du_source_t* s, uint64_t end, uint32_t* rate,
                            uint32_t* preskip) {
    for (uint64_t off = 0; off + 27 < end && off < 64 * 1024; ) {
        uint64_t granule = 0, page_len = 0;
        uint32_t type = 0;
        if (!du_ogg_page_at(s, off, end, &granule, &page_len, &type)) {
            off++;   /* 头部有垃圾就逐字节挪 */
            continue;
        }
        if (type & 0x02) {   /* BOS 页，payload 就是 codec 的 ID header */
            uint64_t avail = 0;
            /* segtable 重新读一下，payload 起点 = 页头 27 + segtable 长度 */
            const uint8_t* ph = du_io_view(s, off, 28, &avail);
            if (ph == NULL || avail < 28) return 0;
            uint64_t payload = off + 27 + ph[26];
            const uint8_t* pl = du_io_view(s, payload, 32, &avail);
            if (pl == NULL || avail < 24) return 0;

            if (pl[0] == 0x01 && memcmp(pl + 1, "vorbis", 6) == 0) {
                *rate = (uint32_t)pl[12] | ((uint32_t)pl[13] << 8) |
                        ((uint32_t)pl[14] << 16) | ((uint32_t)pl[15] << 24);
                *preskip = 0;
                return 1;
            }
            if (memcmp(pl, "OpusHead", 8) == 0) {
                *rate = 48000;   /* Opus 内部恒 48k，granulepos 按它算 */
                *preskip = (uint32_t)pl[10] | ((uint32_t)pl[11] << 8);
                return 1;
            }
            if (pl[0] == 0x7F && memcmp(pl + 1, "FLAC", 4) == 0) {
                /* FLAC-in-Ogg：payload = 1 字节版本 + 2 字节头数 + "fLaC" + blocks */
                uint64_t si_off = payload + 5 + 4;   /* 跳过 0x7F FLAC + fLaC */
                const uint8_t* si = du_io_view(s, si_off, 16, &avail);
                if (si == NULL || avail < 16) return 0;
                *rate = ((uint32_t)si[8] << 12) | ((uint32_t)si[9] << 4) | (si[10] >> 4);
                *preskip = 0;
                return *rate != 0;
            }
            return 0;   /* 未知 codec */
        }
        off += page_len;
    }
    return 0;
}

du_status_t du_probe_ogg(du_ctx_t* c) {
    du_source_t* s = c->src;
    du_result_t* r = c->res;
    int64_t sz64 = du_io_size(s);
    if (sz64 < 0) DU_FAIL(c, DU_ERR_IO);
    uint64_t end = (uint64_t)sz64;
    if (end < 32) DU_FAIL(c, DU_ERR_NOT_CONTAINER);

    uint64_t avail = 0;
    const uint8_t* h = du_io_view(s, 0, 4, &avail);
    if (h == NULL || avail < 4 || memcmp(h, "OggS", 4) != 0) {
        DU_FAIL(c, DU_ERR_NOT_CONTAINER);
    }

    /* 1. 尾部向前找最后一个 granulepos 有效的页 */
    uint64_t best_off = 0;
    uint64_t best_granule = 0;
    int have_last = 0;
    uint64_t win = OGG_PAGE_SEARCH;
    if (win > end) win = end;
    for (uint64_t probe = end - win; ; probe = (probe > win - 7 ? probe - (win - 7) : 0)) {
        uint64_t q = probe;
        while (q + 27 <= end) {
            uint64_t granule = 0, page_len = 0;
            uint32_t type = 0;
            if (du_ogg_page_at(s, q, end, &granule, &page_len, &type) &&
                granule != 0xFFFFFFFFFFFFFFFFull) {
                if (!have_last || q >= best_off) {   /* 取最靠后的有效页 */
                    best_off = q;
                    best_granule = granule;
                    have_last = 1;
                }
                q += page_len;
            } else {
                q++;
            }
        }
        if (probe == 0 || have_last) break;
    }
    if (!have_last || best_granule == 0) DU_FAIL(c, DU_ERR_NO_SYNC);

    /* 2. BOS 页 ID header 拿采样率 */
    uint32_t rate = 0, preskip = 0;
    if (!du_ogg_parse_bos(s, end, &rate, &preskip)) DU_FAIL(c, DU_ERR_NO_SYNC);

    /* 3. granulepos → 采样。Opus 从头扣 pre-skip（文档 §7 补充） */
    uint64_t samples = best_granule;
    if (preskip > 0) {
        samples = (samples > preskip) ? samples - preskip : 0;
    }
    if (samples == 0) DU_FAIL(c, DU_ERR_NO_SYNC);

    r->duration_samples = samples;
    r->duration_sec = (double)samples / (double)rate;
    r->sample_rate = rate;
    r->channels = 0;
    r->bit_depth = 0;
    r->bytes_scanned = end;
    c->st = DU_OK;
    return c->st;
}
