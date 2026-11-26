#include "synth.h"
#include "../src/internal.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 缓冲                                                                */
/* ------------------------------------------------------------------ */

void du_buf_init(du_buf_t* b) { b->data = NULL; b->len = 0; b->cap = 0; }

void du_buf_free(du_buf_t* b) { free(b->data); du_buf_init(b); }

static void buf_grow(du_buf_t* b, size_t need) {
    if (b->len + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < b->len + need) cap *= 2;
    b->data = (uint8_t*)realloc(b->data, cap);
    b->cap = cap;
}

void du_buf_u8(du_buf_t* b, uint8_t v) {
    buf_grow(b, 1);
    b->data[b->len++] = v;
}

void du_buf_be16(du_buf_t* b, uint16_t v) {
    du_buf_u8(b, (uint8_t)(v >> 8));
    du_buf_u8(b, (uint8_t)v);
}

void du_buf_be32(du_buf_t* b, uint32_t v) {
    du_buf_u8(b, (uint8_t)(v >> 24));
    du_buf_u8(b, (uint8_t)(v >> 16));
    du_buf_u8(b, (uint8_t)(v >> 8));
    du_buf_u8(b, (uint8_t)v);
}

void du_buf_be64(du_buf_t* b, uint64_t v) {
    du_buf_be32(b, (uint32_t)(v >> 32));
    du_buf_be32(b, (uint32_t)v);
}

void du_buf_le32(du_buf_t* b, uint32_t v) {
    du_buf_u8(b, (uint8_t)v);
    du_buf_u8(b, (uint8_t)(v >> 8));
    du_buf_u8(b, (uint8_t)(v >> 16));
    du_buf_u8(b, (uint8_t)(v >> 24));
}

void du_buf_le64(du_buf_t* b, uint64_t v) {
    du_buf_le32(b, (uint32_t)v);
    du_buf_le32(b, (uint32_t)(v >> 32));
}

void du_buf_bytes(du_buf_t* b, const void* p, size_t n) {
    for (size_t i = 0; i < n; i++) du_buf_u8(b, ((const uint8_t*)p)[i]);
}

void du_buf_zeros(du_buf_t* b, size_t n) {
    for (size_t i = 0; i < n; i++) du_buf_u8(b, 0);
}

void du_buf_poke_be32(du_buf_t* b, size_t off, uint32_t v) {
    b->data[off] = (uint8_t)(v >> 24);
    b->data[off + 1] = (uint8_t)(v >> 16);
    b->data[off + 2] = (uint8_t)(v >> 8);
    b->data[off + 3] = (uint8_t)v;
}

void du_buf_poke_le32(du_buf_t* b, size_t off, uint32_t v) {
    b->data[off] = (uint8_t)v;
    b->data[off + 1] = (uint8_t)(v >> 8);
    b->data[off + 2] = (uint8_t)(v >> 16);
    b->data[off + 3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------ */
/* MP3                                                                 */
/* ------------------------------------------------------------------ */

void du_synth_mp3_head(du_buf_t* b, uint32_t version, uint32_t layer,
                       uint32_t br_idx, uint32_t sr_idx, uint32_t mode,
                       uint32_t padding) {
    du_buf_u8(b, 0xFF);
    /* b1: sync(3) version(2) layer(2) protection(1)=1 无CRC */
    du_buf_u8(b, (uint8_t)(0xE0 | (version << 3) | (layer << 1) | 1));
    /* b2: bitrate(4) samplerate(2) padding(1) private(1) */
    du_buf_u8(b, (uint8_t)((br_idx << 4) | (sr_idx << 2) | (padding << 1)));
    /* b3: mode(2) modeext(2) copyright(1) original(1) emphasis(2) */
    du_buf_u8(b, (uint8_t)(mode << 6));
}

/* MPEG1 L3 128k 44.1k stereo：flen = 144 × 128000 / 44100 = 417。
 * 数据字节都控制在 0x40~0x7F，避免撞出假同步。 */
void du_synth_mp3_cbr(du_buf_t* out, int frames) {
    for (int i = 0; i < frames; i++) {
        du_synth_mp3_head(out, 3, 3, 9, 0, 0, 0);
        for (int j = 0; j < SYNTH_MP3_FLLEN - 4; j++) {
            du_buf_u8(out, (uint8_t)(0x40 + ((i * 31 + j * 17) & 0x3F)));
        }
    }
}

/* 帧间插一个"字段合法但内容是垃圾"的假同步头（160kbps，flen 应为 522），
 * 双帧验证会失败，扫描器逐字节找回真帧。 */
void du_synth_mp3_with_fake_sync(du_buf_t* out, int frames, int at_frame) {
    for (int i = 0; i < frames; i++) {
        if (i == at_frame) {
            du_synth_mp3_head(out, 3, 3, 10, 0, 0, 0);
            du_buf_zeros(out, 40);
            continue;
        }
        du_synth_mp3_head(out, 3, 3, 9, 0, 0, 0);
        for (int j = 0; j < SYNTH_MP3_FLLEN - 4; j++) {
            du_buf_u8(out, (uint8_t)(0x40 + ((i * 29 + j * 13) & 0x3F)));
        }
    }
}

/* ------------------------------------------------------------------ */
/* MP4                                                                 */
/* ------------------------------------------------------------------ */

void du_box_begin(du_buf_t* b, du_box_t* box, const char* type4) {
    box->start = b->len;
    box->size_pos = b->len;
    du_buf_be32(b, 0);            /* size 占位 */
    du_buf_bytes(b, type4, 4);
}

void du_box_end(du_buf_t* b, du_box_t* box) {
    du_buf_poke_be32(b, box->size_pos, (uint32_t)(b->len - box->start));
}

/* ------------------------------------------------------------------ */
/* FLAC                                                                */
/* ------------------------------------------------------------------ */

static void flac_streaminfo_body(du_buf_t* b, uint32_t sr, uint32_t ch,
                                 uint32_t bits, uint64_t total) {
    du_buf_be16(b, 4096);          /* min blocksize (16bit) */
    du_buf_be16(b, 4096);          /* max blocksize (16bit) */
    du_buf_be16(b, 0); du_buf_u8(b, 0);    /* min framesize (24bit) */
    du_buf_be16(b, 0); du_buf_u8(b, 0);    /* max framesize (24bit) */
    /* 8 字节位包：sr(20) ch(3) bits(5) total(36) */
    uint64_t hi = ((uint64_t)sr << 44) | ((uint64_t)(ch - 1) << 41) |
                  ((uint64_t)(bits - 1) << 36) | total;
    du_buf_be32(b, (uint32_t)(hi >> 32));
    du_buf_be32(b, (uint32_t)hi);
    du_buf_zeros(b, 16);           /* MD5 */
}

void du_synth_flac(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bits,
                   uint64_t total_samples) {
    du_buf_bytes(out, "fLaC", 4);
    du_buf_u8(out, 0x80);          /* last + STREAMINFO */
    du_buf_u8(out, 0);             /* 24 位长度 = 34 */
    du_buf_u8(out, 0);
    du_buf_u8(out, 34);
    flac_streaminfo_body(out, sr, ch, bits, total_samples);
}

/* 帧头：sync(2) + bs/sr(1) + channel/bps(1) + CRC8(1)。total==0 的流式
 * 场景用，sampling rate code 0 表示沿用 STREAMINFO。 */
void du_synth_flac_frame(du_buf_t* out, uint32_t blocksize) {
    uint8_t head[5];
    uint32_t code;
    switch (blocksize) {
    case 192: code = 1; break;
    case 576: code = 2; break;
    case 1152: code = 3; break;
    default: code = 1; blocksize = 192; break;
    }
    head[0] = 0xFF;
    head[1] = 0xF8;                /* sync + fixed block + reserved 0 */
    head[2] = (uint8_t)(code << 4);
    head[3] = 0x00;                /* channel assign + bps code + reserved */
    head[4] = (uint8_t)du_util_crc8_flac(0, head, 4);
    du_buf_bytes(out, head, 5);
    du_buf_zeros(out, 32);         /* 帧体占位 */
}

/* ------------------------------------------------------------------ */
/* WAV / AIFF                                                          */
/* ------------------------------------------------------------------ */

void du_synth_wav(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bits,
                  uint64_t data_bytes) {
    uint32_t body = 4 + (8 + 16) + (8 + (uint32_t)data_bytes);
    du_buf_bytes(out, "RIFF", 4);
    du_buf_le32(out, body);
    du_buf_bytes(out, "WAVE", 4);
    du_buf_bytes(out, "fmt ", 4);
    du_buf_le32(out, 16);                            /* fmt chunk size */
    du_buf_le32(out, (1u | (ch << 16)));             /* fmt_tag(2) + channels(2) */
    du_buf_le32(out, sr);                            /* sample rate */
    du_buf_le32(out, ch * bits * sr / 8);            /* byte rate */
    du_buf_le32(out, ((ch * bits / 8) & 0xFFFF) | (bits << 16));  /* align(2)+bits(2) */
    du_buf_bytes(out, "data", 4);
    du_buf_le32(out, (uint32_t)data_bytes);
    du_buf_zeros(out, (size_t)data_bytes);
}

void du_synth_rf64(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bits,
                   uint64_t data_bytes) {
    du_buf_bytes(out, "RF64", 4);
    du_buf_le32(out, 0xFFFFFFFFu);
    du_buf_bytes(out, "WAVE", 4);
    du_buf_bytes(out, "ds64", 4);
    du_buf_le32(out, 28);
    du_buf_le64(out, 0);                        /* riffSize */
    du_buf_le64(out, data_bytes);               /* dataSize */
    du_buf_le64(out, 0);                        /* sampleCount */
    du_buf_le32(out, 0);                        /* table length */
    du_buf_bytes(out, "fmt ", 4);
    du_buf_le32(out, 16);
    du_buf_le32(out, (1u | (ch << 16)));             /* fmt_tag(2) + channels(2) */
    du_buf_le32(out, sr);                            /* sample rate */
    du_buf_le32(out, ch * bits * sr / 8);            /* byte rate */
    du_buf_le32(out, ((ch * bits / 8) & 0xFFFF) | (bits << 16));  /* align(2)+bits(2) */
    du_buf_bytes(out, "data", 4);
    du_buf_le32(out, 0xFFFFFFFFu);
    du_buf_zeros(out, (size_t)data_bytes);
}

void du_synth_aiff(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bits,
                   uint64_t frames) {
    du_buf_bytes(out, "FORM", 4);
    du_buf_be32(out, 0);                        /* size 占位，最后回填 */
    du_buf_bytes(out, "AIFF", 4);
    du_buf_bytes(out, "COMM", 4);
    du_buf_be32(out, 18);
    du_buf_be16(out, (uint16_t)ch);
    du_buf_be32(out, (uint32_t)frames);
    du_buf_be16(out, (uint16_t)bits);
    /* 80 位浮点：sr = m × 2^(e-16383-63)，取规格化 mantissa */
    int msb = 0;
    while ((1u << (msb + 1)) <= sr && msb < 31) msb++;
    du_buf_u8(out, (uint8_t)((16383 + msb) >> 8));
    du_buf_u8(out, (uint8_t)(16383 + msb));
    du_buf_be64(out, (uint64_t)sr << (63 - msb));
    du_buf_bytes(out, "SSND", 4);
    du_buf_be32(out, (uint32_t)(8 + frames * ch * bits / 8));
    du_buf_be32(out, 0);
    du_buf_be32(out, 0);
    du_buf_zeros(out, (size_t)(frames * ch * bits / 8));
    du_buf_poke_be32(out, 4, (uint32_t)(out->len - 8));
}

/* ------------------------------------------------------------------ */
/* OGG                                                                 */
/* ------------------------------------------------------------------ */

static void ogg_page(du_buf_t* out, uint8_t type, uint64_t granule,
                     const uint8_t* payload, size_t plen) {
    uint8_t nseg = (uint8_t)((plen + 254) / 255);
    du_buf_bytes(out, "OggS", 4);
    du_buf_u8(out, 0);
    du_buf_u8(out, type);
    du_buf_le64(out, granule);
    du_buf_le32(out, 0x12345678);
    du_buf_le32(out, 0);
    du_buf_le32(out, 0);          /* crc，库不校验 */
    du_buf_u8(out, nseg);
    for (size_t i = 0; i < nseg; i++) {
        size_t left = plen - i * 255;
        du_buf_u8(out, (uint8_t)(left > 255 ? 255 : left));
    }
    du_buf_bytes(out, payload, plen);
}

void du_synth_ogg_vorbis(du_buf_t* out, uint32_t rate, uint64_t last_granule) {
    uint8_t id[30];
    memset(id, 0, sizeof(id));
    id[0] = 0x01;
    memcpy(id + 1, "vorbis", 6);
    id[11] = 2;
    id[12] = (uint8_t)rate;
    id[13] = (uint8_t)(rate >> 8);
    id[14] = (uint8_t)(rate >> 16);
    id[15] = (uint8_t)(rate >> 24);
    ogg_page(out, 0x02, 0, id, sizeof(id));     /* BOS */

    uint8_t body[64];
    memset(body, 0, sizeof(body));
    ogg_page(out, 0x00, last_granule, body, sizeof(body));
}

void du_synth_ogg_opus(du_buf_t* out, uint32_t preskip, uint64_t last_granule) {
    uint8_t id[19];
    memset(id, 0, sizeof(id));
    memcpy(id, "OpusHead", 8);
    id[8] = 1;
    id[9] = 2;
    id[10] = (uint8_t)preskip;
    id[11] = (uint8_t)(preskip >> 8);
    ogg_page(out, 0x02, 0, id, sizeof(id));     /* BOS */

    uint8_t body[64];
    memset(body, 0, sizeof(body));
    ogg_page(out, 0x00, last_granule, body, sizeof(body));
}
