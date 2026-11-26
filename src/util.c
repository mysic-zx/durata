/* 工具函数：CRC8、MP3 尾部标签识别 */
#include "internal.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* FLAC 帧头 CRC8（多项式 0x07），用来降低假同步误判。
 * 只算帧头几个字节，位运算实现足够，不用查表省一张 256 项表。 */
/* ------------------------------------------------------------------ */

uint32_t du_util_crc8_flac(uint32_t seed, const uint8_t* p, size_t n) {
    uint8_t crc = (uint8_t)seed;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* MP3 尾部标签：ID3v1（128 字节）、APEv2（footer 自带总长）             */
/* ------------------------------------------------------------------ */

/* APEv2 footer：32 字节，"APETAGEX" 起头，offset 12 是 tag 总长（含 footer） */
static uint64_t apev2_tail(du_source_t* s, uint64_t fsize, uint32_t* flags) {
    (void)flags;
    if (fsize < 32) return 0;
    const uint8_t* p = du_io_view(s, fsize - 32, 32, NULL);
    if (p == NULL || memcmp(p, "APETAGEX", 8) != 0) return 0;
    /* APEv2 长度字段是小端：offset 12 起的 tag 总长（含 footer） */
    uint32_t total = (uint32_t)p[12] | ((uint32_t)p[13] << 8) |
                     ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
    if (total < 32) return 0;
    uint64_t tail = (uint64_t)total;
    /* 前面若还有 APEv2 header 也一并扣掉 */
    if (fsize >= tail + 32) {
        const uint8_t* q = du_io_view(s, fsize - tail - 32, 8, NULL);
        if (q && memcmp(q, "APETAGEX", 8) == 0) tail += 32;
    }
    return tail;
}

uint64_t du_util_mp3_tail_tags(du_source_t* s, uint32_t* flags) {
    int64_t sz64 = du_io_size(s);
    if (sz64 < 0) return 0;
    uint64_t fsize = (uint64_t)sz64;
    uint64_t tail = 0;

    /* ID3v1 固定 128 字节 */
    if (fsize >= 128) {
        const uint8_t* p = du_io_view(s, fsize - 128, 3, NULL);
        if (p && memcmp(p, "TAG", 3) == 0) tail += 128;
    }
    /* ID3v1 之前还可能挂 APEv2（顺序：音频 → APEv2 → ID3v1） */
    uint64_t base = fsize - tail;
    if (base >= 32) {
        const uint8_t* p = du_io_view(s, base - 32, 8, NULL);
        if (p && memcmp(p, "APETAGEX", 8) == 0) {
            tail += apev2_tail(s, base, flags);
        }
    }
    return tail;
}
