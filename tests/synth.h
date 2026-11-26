/* 合成音频文件生成器：测试零外部语料（设计文档 §12.2 属性测试思路） */
#ifndef DU_SYNTH_H
#define DU_SYNTH_H

#include <stddef.h>
#include <stdint.h>

typedef struct du_buf {
    uint8_t* data;
    size_t len;
    size_t cap;
} du_buf_t;

void du_buf_init(du_buf_t* b);
void du_buf_free(du_buf_t* b);
void du_buf_u8(du_buf_t* b, uint8_t v);
void du_buf_be16(du_buf_t* b, uint16_t v);
void du_buf_be32(du_buf_t* b, uint32_t v);
void du_buf_be64(du_buf_t* b, uint64_t v);
void du_buf_le32(du_buf_t* b, uint32_t v);
void du_buf_le64(du_buf_t* b, uint64_t v);
void du_buf_bytes(du_buf_t* b, const void* p, size_t n);
void du_buf_zeros(du_buf_t* b, size_t n);
void du_buf_poke_be32(du_buf_t* b, size_t off, uint32_t v);

/* ---- MP3（MPEG1 L3 128kbps 44.1kHz stereo，帧长 417） ---- */
#define SYNTH_MP3_FLLEN 417
#define SYNTH_MP3_SPF   1152
#define SYNTH_MP3_SR    44100

/* 写一帧帧头（4 字节），数据由调用方继续填 */
void du_synth_mp3_head(du_buf_t* b, uint32_t version, uint32_t layer,
                       uint32_t br_idx, uint32_t sr_idx, uint32_t mode,
                       uint32_t padding);
/* 标准 CBR：frames 帧 + 可选前垃圾/尾部标签开关 */
void du_synth_mp3_cbr(du_buf_t* out, int frames);
/* 帧间插一个"合法字段但内容是垃圾"的假同步头 + 垃圾（触发双帧验证失败） */
void du_synth_mp3_with_fake_sync(du_buf_t* out, int frames, int at_frame);

/* ---- MP4 ---- */
typedef struct { size_t size_pos; size_t start; } du_box_t;
void du_box_begin(du_buf_t* b, du_box_t* box, const char* type4);
void du_box_end(du_buf_t* b, du_box_t* box);

/* ---- FLAC ---- */
void du_synth_flac(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bps,
                   uint64_t total_samples);
/* total==0 的流式形态：后面拼 n_frames 个 blocksize=192 的帧 */
void du_synth_flac_frame(du_buf_t* out, uint32_t blocksize);

/* ---- WAV / AIFF ---- */
void du_synth_wav(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bps,
                  uint64_t data_bytes);
void du_synth_rf64(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bps,
                   uint64_t data_bytes);
void du_synth_aiff(du_buf_t* out, uint32_t sr, uint32_t ch, uint32_t bps,
                   uint64_t frames);

/* ---- OGG ---- */
void du_synth_ogg_vorbis(du_buf_t* out, uint32_t rate, uint64_t last_granule);
void du_synth_ogg_opus(du_buf_t* out, uint32_t preskip, uint64_t last_granule);

#endif
