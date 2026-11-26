/* MP3 扫描状态机用例：全部基于合成数据（设计文档 §12.2） */
#include "du_test.h"
#include "synth.h"
#include "../src/internal.h"

/* 用 mem source 直接探测合成 buffer */
static du_status_t probe_buf(const du_buf_t* b, du_format_t fmt,
                             const du_options_t* opts, du_result_t* r) {
    du_source_t* s = NULL;
    du_status_t st = du_source_open_mem(b->data, b->len, &s);
    if (st != DU_OK) return st;
    st = du_probe(s, fmt, opts, r);
    du_source_close(s);
    return st;
}

static void test_mp3_cbr_basic(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_cbr(&b, 100);
    du_result_t r; memset(&r, 0, sizeof(r));

    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 100);
    DU_CHECK_EQ(r.duration_samples, 100ull * SYNTH_MP3_SPF);
    DU_CHECK_NEAR(r.duration_sec, 100.0 * SYNTH_MP3_SPF / SYNTH_MP3_SR, 1e-9);
    DU_CHECK_EQ(r.sample_rate, SYNTH_MP3_SR);
    DU_CHECK_EQ(r.channels, 2);
    DU_CHECK_EQ(r.audio_bytes, 100ull * SYNTH_MP3_FLLEN);
    /* 平均码率：每帧都声称 128kbps，实测均值也是 128000 */
    DU_CHECK_EQ(r.bitrate_avg_bps, 128000u);
    /* CBR：isVBR 为 0 */
    DU_CHECK_EQ(du_result_is_vbr(&r), 0);
    du_buf_free(&b);
}

static void test_mp3_id3_tags(void) {
    du_buf_t b; du_buf_init(&b);
    /* ID3v2.3 头：10 字节 + synchsafe size */
    du_buf_bytes(&b, "ID3", 3);
    du_buf_u8(&b, 3); du_buf_u8(&b, 0);
    du_buf_u8(&b, 0);
    du_buf_u8(&b, 0); du_buf_u8(&b, 0); du_buf_u8(&b, 0x02); du_buf_u8(&b, 0x30);
    du_buf_zeros(&b, 304);          /* synchsafe(0,0,2,0x30) = 304 字节 tag body */
    du_synth_mp3_cbr(&b, 50);
    /* ID3v1：TAG 魔数在 128 字节开头 */
    du_buf_bytes(&b, "TAG", 3);
    du_buf_zeros(&b, 125);
    du_result_t r; memset(&r, 0, sizeof(r));

    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 50);
    DU_CHECK_EQ(r.audio_bytes, 50ull * SYNTH_MP3_FLLEN);
    du_buf_free(&b);
}

static void test_mp3_fake_sync(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_with_fake_sync(&b, 20, 10);
    du_result_t r; memset(&r, 0, sizeof(r));

    /* 双帧验证挡住假同步：at_frame 位置的真帧被假头取代，
     * 剩下 19 个真实帧全部数回，无一多计 */
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 19);
    DU_CHECK_EQ(r.duration_samples, 19ull * SYNTH_MP3_SPF);
    DU_CHECK(r.resync_count >= 1);
    DU_CHECK((r.flags & DU_F_RESYNC) != 0);
    du_buf_free(&b);
}

static void test_mp3_truncated(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_cbr(&b, 10);
    b.len -= 200;                   /* 掐掉最后一帧的大半 */
    du_result_t r; memset(&r, 0, sizeof(r));

    du_status_t st = probe_buf(&b, DU_FORMAT_MP3, NULL, &r);
    DU_CHECK_EQ(st, DU_ERR_TRUNCATED);
    DU_CHECK_EQ(r.frames, 9);       /* 最后一帧不完整，不计入 */
    DU_CHECK((r.flags & DU_F_TRUNCATED) != 0);
    DU_CHECK_NEAR(r.duration_sec, 9.0 * SYNTH_MP3_SPF / SYNTH_MP3_SR, 1e-9);
    du_buf_free(&b);
}

static void test_mp3_xing(void) {
    du_buf_t b; du_buf_init(&b);
    /* 首帧带 Xing：声称 1000 帧、实测只写 20 帧 */
    du_synth_mp3_head(&b, 3, 3, 9, 0, 0, 0);
    du_buf_zeros(&b, 32);           /* MPEG1 stereo sideinfo */
    du_buf_bytes(&b, "Xing", 4);
    du_buf_be32(&b, 0x03);          /* frames + bytes */
    du_buf_be32(&b, 1000);          /* xing frames（虚报） */
    du_buf_be32(&b, 1000u * SYNTH_MP3_FLLEN);
    du_buf_zeros(&b, 100);          /* toc */
    du_buf_zeros(&b, SYNTH_MP3_FLLEN - 4 - 32 - 8 - 8 - 100);
    /* 19 帧真帧 */
    du_synth_mp3_cbr(&b, 19);
    du_result_t r; memset(&r, 0, sizeof(r));

    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, NULL, &r), DU_OK);
    DU_CHECK((r.flags & DU_F_XING_PRESENT) != 0);
    DU_CHECK_EQ(r.xing_frames, 1000);
    DU_CHECK_EQ(r.frames, 20);      /* Xing 帧本身算 1 帧 + 19 真帧 */
    /* (1000-20)/20 = 48 倍偏差 → 不 AGREE */
    DU_CHECK((r.flags & DU_F_XING_AGREE) == 0);
    /* 元数据损坏时逐帧扫描仍给出真值（G1） */
    DU_CHECK_EQ(r.duration_samples, 20ull * SYNTH_MP3_SPF);
    DU_CHECK_NEAR(r.xing_delta, (1000.0 - 20.0) / 20.0, 1e-9);
    du_buf_free(&b);
}

static void test_mp3_fast_xing(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_head(&b, 3, 3, 9, 0, 0, 0);
    du_buf_zeros(&b, 32);
    du_buf_bytes(&b, "Xing", 4);
    du_buf_be32(&b, 0x03);
    du_buf_be32(&b, 800);
    du_buf_be32(&b, 800u * SYNTH_MP3_FLLEN);
    du_buf_zeros(&b, 100);
    du_buf_zeros(&b, SYNTH_MP3_FLLEN - 4 - 32 - 8 - 8 - 100);
    du_synth_mp3_cbr(&b, 5);
    du_result_t r; memset(&r, 0, sizeof(r));

    du_options_t o; memset(&o, 0, sizeof(o));
    o.abi_version = DURATA_ABI_VERSION;
    o.fast_mp3 = 1;
    o.read_xing = 1;   /* read_xing 默认 1，但 memset 全零后必须显式开 */
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, &o, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 800ull * SYNTH_MP3_SPF);
    DU_CHECK_EQ(r.frames, 800);
    du_buf_free(&b);
}

static void test_mp3_lame_gapless(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_head(&b, 3, 3, 9, 0, 0, 0);
    du_buf_zeros(&b, 32);
    du_buf_bytes(&b, "Xing", 4);
    du_buf_be32(&b, 0x03);
    du_buf_be32(&b, 100);
    du_buf_be32(&b, 100u * SYNTH_MP3_FLLEN);
    du_buf_zeros(&b, 100);          /* toc */
    /* LAME tag：delay=2113, padding=1400 → 12/12 位编码 */
    uint32_t delay = 2113, padding = 1400;
    du_buf_bytes(&b, "LAME3.99r", 9);
    du_buf_zeros(&b, 11);           /* 到 +20 偏移 */
    du_buf_u8(&b, (uint8_t)(delay >> 4));
    du_buf_u8(&b, (uint8_t)(((delay & 0xF) << 4) | (padding >> 8)));
    du_buf_u8(&b, (uint8_t)(padding & 0xFF));
    du_buf_zeros(&b, SYNTH_MP3_FLLEN - 4 - 32 - 8 - 8 - 100 - 9 - 11 - 3);
    du_synth_mp3_cbr(&b, 99);
    du_result_t r; memset(&r, 0, sizeof(r));

    /* 默认：只标记不扣 */
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, NULL, &r), DU_OK);
    DU_CHECK((r.flags & DU_F_XING_LAME) != 0);
    DU_CHECK((r.flags & DU_F_GAPLESS_HINT) != 0);
    DU_CHECK_EQ(r.duration_samples, 100ull * SYNTH_MP3_SPF);

    /* gapless_correct：扣 delay + padding */
    du_result_t r2; memset(&r2, 0, sizeof(r2));
    du_options_t o; memset(&o, 0, sizeof(o));
    o.abi_version = DURATA_ABI_VERSION;
    o.gapless_correct = 1;
    o.read_xing = 1;
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, &o, &r2), DU_OK);
    DU_CHECK_EQ(r2.duration_samples, 100ull * SYNTH_MP3_SPF - (delay + padding));
    du_buf_free(&b);
}

static void test_mp3_vbr(void) {
    du_buf_t b; du_buf_init(&b);
    /* 交替 128k / 96k（br_idx 9 / 7，MPEG1 L3） */
    for (int i = 0; i < 10; i++) {
        uint32_t br_idx = (i % 2) ? 7 : 9;
        uint32_t flen = (br_idx == 9) ? 417 : 313;   /* 144×96000/44100 = 313 */
        du_synth_mp3_head(&b, 3, 3, br_idx, 0, 0, 0);
        for (uint32_t j = 0; j < flen - 4; j++) {
            du_buf_u8(&b, (uint8_t)(0x40 + ((i * 23 + j * 19) & 0x3F)));
        }
    }
    du_result_t r; memset(&r, 0, sizeof(r));

    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 10);
    DU_CHECK((r.flags & DU_F_VBR) != 0);
    /* VBR：接口与 flags 位一致 */
    DU_CHECK_EQ(du_result_is_vbr(&r), 1);
    du_buf_free(&b);
}

static void test_mp3_mpeg2(void) {
    du_buf_t b; du_buf_init(&b);
    /* MPEG2 L3 64kbps 22050Hz mono：spf 576，flen = 72×64000/22050 = 208 */
    for (int i = 0; i < 30; i++) {
        du_synth_mp3_head(&b, 2, 3, 8, 0, 3, 0);   /* version=2, 64k(idx8), mode=3 mono */
        for (int j = 0; j < 208 - 4; j++) {
            du_buf_u8(&b, (uint8_t)(0x40 + ((i * 13 + j * 7) & 0x3F)));
        }
    }
    du_result_t r; memset(&r, 0, sizeof(r));

    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP2, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 30);
    DU_CHECK_EQ(r.duration_samples, 30ull * 576);
    DU_CHECK_EQ(r.sample_rate, 22050);
    DU_CHECK_EQ(r.channels, 1);
    du_buf_free(&b);
}

static void test_mp3_strict(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_with_fake_sync(&b, 20, 10);
    du_result_t r; memset(&r, 0, sizeof(r));

    du_options_t o; memset(&o, 0, sizeof(o));
    o.abi_version = DURATA_ABI_VERSION;
    o.strict = 1;
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, &o, &r), DU_ERR_NO_SYNC);
    du_buf_free(&b);
}

static void test_mp3_garbage_only(void) {
    du_buf_t b; du_buf_init(&b);
    du_buf_zeros(&b, 4096);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, NULL, &r), DU_ERR_NO_SYNC);
    du_buf_free(&b);
}

void du_test_mp3(void) {
    DU_TEST_RUN("mp3/cbr basic", test_mp3_cbr_basic);
    DU_TEST_RUN("mp3/id3v2+id3v1 tags", test_mp3_id3_tags);
    DU_TEST_RUN("mp3/fake sync double-check", test_mp3_fake_sync);
    DU_TEST_RUN("mp3/truncated", test_mp3_truncated);
    DU_TEST_RUN("mp3/xing cross-check", test_mp3_xing);
    DU_TEST_RUN("mp3/fast xing", test_mp3_fast_xing);
    DU_TEST_RUN("mp3/lame gapless", test_mp3_lame_gapless);
    DU_TEST_RUN("mp3/vbr flag", test_mp3_vbr);
    DU_TEST_RUN("mp3/mpeg2 layer3", test_mp3_mpeg2);
    DU_TEST_RUN("mp3/strict", test_mp3_strict);
    DU_TEST_RUN("mp3/garbage only", test_mp3_garbage_only);
}
