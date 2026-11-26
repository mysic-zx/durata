/* MP4 / FLAC / WAV / OGG / 公共 API 用例（合成数据） */
#include "du_test.h"
#include "synth.h"
#include "../src/internal.h"

static du_status_t probe_buf(const du_buf_t* b, du_format_t fmt,
                             const du_options_t* opts, du_result_t* r) {
    du_source_t* s = NULL;
    du_status_t st = du_source_open_mem(b->data, b->len, &s);
    if (st != DU_OK) return st;
    st = du_probe(s, fmt, opts, r);
    du_source_close(s);
    return st;
}

/* ------------------------------------------------------------------ */
/* MP4 合成辅助                                                        */
/* ------------------------------------------------------------------ */

static void put_mvhd(du_buf_t* b, uint32_t ts) {
    du_box_t box; du_box_begin(b, &box, "mvhd");
    du_buf_be32(b, 0);            /* version 0 + flags */
    du_buf_be32(b, 0); du_buf_be32(b, 0);   /* ctime mtime */
    du_buf_be32(b, ts);
    du_buf_be32(b, 0);            /* duration */
    du_buf_zeros(b, 80);          /* 标准保留区 */
    du_box_end(b, &box);
}

static void put_mdhd(du_buf_t* b, uint32_t ts, uint64_t dur, int v1) {
    du_box_t box; du_box_begin(b, &box, "mdhd");
    du_buf_be32(b, v1 ? 0x01000000u : 0);   /* version + flags */
    if (v1) {
        du_buf_be64(b, 0); du_buf_be64(b, 0);
        du_buf_be32(b, ts);
        du_buf_be64(b, dur);
    } else {
        du_buf_be32(b, 0); du_buf_be32(b, 0);
        du_buf_be32(b, ts);
        du_buf_be32(b, (uint32_t)dur);
    }
    du_buf_be16(b, 0); du_buf_be16(b, 0);   /* language + pre_defined */
    du_box_end(b, &box);
}

static void put_hdlr_soun(du_buf_t* b) {
    du_box_t box; du_box_begin(b, &box, "hdlr");
    du_buf_be32(b, 0);
    du_buf_be32(b, 0);
    du_buf_bytes(b, "soun", 4);
    du_buf_zeros(b, 12);
    du_box_end(b, &box);
}

/* stts：count 份 (n, delta)；entries 数组 (n, delta) 交替 */
/* stts 是 full-box：version/flags(4) + entry_count(4) + entries(count,delta)× */
static void put_stts(du_buf_t* b, const uint64_t* e, int n2) {
    du_box_t box; du_box_begin(b, &box, "stts");
    du_buf_be32(b, 0);                    /* version + flags */
    du_buf_be32(b, (uint32_t)(n2 / 2));
    for (int i = 0; i < n2; i++) du_buf_be32(b, (uint32_t)e[i]);
    du_box_end(b, &box);
}

/* ISO 规范结构：trak 下是 mdia，mdhd/hdlr 在 mdia 里 */
static void put_trak(du_buf_t* b, uint32_t ts, uint64_t mdhd_dur, int mdhd_v1,
                     const uint64_t* stts, int stts_n2, int is_audio) {
    du_box_t trak, mdia, minf, stbl;
    du_box_begin(b, &trak, "trak");
    du_box_begin(b, &mdia, "mdia");
    put_mdhd(b, ts, mdhd_dur, mdhd_v1);
    if (is_audio) put_hdlr_soun(b);
    else {
        du_box_t h; du_box_begin(b, &h, "hdlr");
        du_buf_be32(b, 0); du_buf_be32(b, 0);
        du_buf_bytes(b, "vide", 4);
        du_buf_zeros(b, 12);
        du_box_end(b, &h);
    }
    du_box_begin(b, &minf, "minf");
    du_box_begin(b, &stbl, "stbl");
    if (stts != NULL) put_stts(b, stts, stts_n2);
    du_box_end(b, &stbl);
    du_box_end(b, &minf);
    du_box_end(b, &mdia);
    du_box_end(b, &trak);
}

static void put_ftyp(du_buf_t* b, const char* brand) {
    du_box_t box; du_box_begin(b, &box, "ftyp");
    du_buf_bytes(b, brand, 4);
    du_buf_be32(b, 0);
    du_box_end(b, &box);
}

/* ------------------------------------------------------------------ */
/* MP4                                                                 */
/* ------------------------------------------------------------------ */

static void test_mp4_basic(void) {
    du_buf_t b; du_buf_init(&b);
    put_ftyp(&b, "M4A ");
    du_box_t moov; du_box_begin(&b, &moov, "moov");
    put_mvhd(&b, 600);
    uint64_t stts[] = { 431, 1024 };   /* 431 包 × 1024 = 441344 */
    put_trak(&b, 44100, 0, 0, stts, 2, 1);
    du_box_end(&b, &moov);
    du_box_t mdat; du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 441344ull);
    DU_CHECK_NEAR(r.duration_sec, 441344.0 / 44100.0, 1e-9);
    DU_CHECK_EQ(r.sample_rate, 44100);
    DU_CHECK((r.flags & DU_F_MULTI_AUDIO) == 0);
    du_buf_free(&b);
}

static void test_mp4_mdhd_v1(void) {
    du_buf_t b; du_buf_init(&b);
    put_ftyp(&b, "M4A ");
    du_box_t moov; du_box_begin(&b, &moov, "moov");
    put_mvhd(&b, 600);
    /* 无 stts：兜底 mdhd.duration（v1 64 位，模拟超长有声书） */
    put_trak(&b, 44100, 44100ull * 3600 * 40, 1, NULL, 0, 1);
    du_box_end(&b, &moov);
    du_box_t mdat; du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 44100ull * 3600 * 40);
    DU_CHECK_NEAR(r.duration_sec, 40.0 * 3600.0, 1e-6);
    du_buf_free(&b);
}

static void test_mp4_multi_trak(void) {
    du_buf_t b; du_buf_init(&b);
    put_ftyp(&b, "isom");
    du_box_t moov; du_box_begin(&b, &moov, "moov");
    put_mvhd(&b, 600);
    uint64_t stts_a[] = { 10, 1024 };    /* 短音频轨 */
    uint64_t stts_v[] = { 20, 1024 };    /* 视频轨，忽略 */
    uint64_t stts_b[] = { 40, 1024 };    /* 最长音频轨 → 选中 */
    put_trak(&b, 44100, 0, 0, stts_a, 2, 1);
    put_trak(&b, 30000, 0, 0, stts_v, 2, 0);
    put_trak(&b, 48000, 0, 0, stts_b, 2, 1);
    du_box_end(&b, &moov);
    du_box_t mdat; du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_OK);
    DU_CHECK((r.flags & DU_F_MULTI_AUDIO) != 0);
    /* 40×1024 = 40960 采样 @ 48k → 最长的那条 */
    DU_CHECK_EQ(r.duration_samples, 40960ull);
    DU_CHECK_EQ(r.sample_rate, 48000);
    du_buf_free(&b);
}

static void test_mp4_elst(void) {
    du_buf_t b; du_buf_init(&b);
    put_ftyp(&b, "M4A ");
    du_box_t moov; du_box_begin(&b, &moov, "moov");
    put_mvhd(&b, 44100);
    uint64_t stts[] = { 44100, 1 };
    put_trak(&b, 44100, 0, 0, stts, 2, 1);
    du_box_end(&b, &moov);
    du_box_t mdat; du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 44100ull);

    /* 带 elst 的版本：在 trak 里手动补 edts */
    du_buf_free(&b);
    du_buf_init(&b);
    put_ftyp(&b, "M4A ");
    du_box_begin(&b, &moov, "moov");
    put_mvhd(&b, 44100);
    du_box_t trak; du_box_begin(&b, &trak, "trak");
    du_box_t edts; du_box_begin(&b, &edts, "edts");
    du_box_t elst; du_box_begin(&b, &elst, "elst");
    du_buf_be32(&b, 0);                    /* version 0 */
    du_buf_be32(&b, 1);                    /* entry count */
    du_buf_be32(&b, 44100);                /* segment_duration (movie ts) */
    du_buf_be32(&b, 1024);                 /* media_time (media ts) */
    du_buf_be32(&b, 0x00010000u);          /* rate 1.0 */
    du_box_end(&b, &elst);
    du_box_end(&b, &edts);
    du_box_t mdia; du_box_begin(&b, &mdia, "mdia");
    put_mdhd(&b, 44100, 0, 0);
    put_hdlr_soun(&b);
    du_box_t minf; du_box_begin(&b, &minf, "minf");
    du_box_t stbl; du_box_begin(&b, &stbl, "stbl");
    put_stts(&b, stts, 2);
    du_box_end(&b, &stbl);
    du_box_end(&b, &minf);
    du_box_end(&b, &mdia);
    du_box_end(&b, &trak);
    du_box_end(&b, &moov);
    du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    /* 默认：只报 ELST_PRESENT 不裁剪 */
    memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_OK);
    DU_CHECK((r.flags & DU_F_ELST_PRESENT) != 0);
    DU_CHECK_EQ(r.duration_samples, 44100ull);

    /* elst_apply=1：裁掉开头 1024 采样 */
    du_options_t o; memset(&o, 0, sizeof(o));
    o.abi_version = DURATA_ABI_VERSION;
    o.elst_apply = 1;
    memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, &o, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 44100ull - 1024);
    du_buf_free(&b);
}

static void test_mp4_fragmented(void) {
    du_buf_t b; du_buf_init(&b);
    put_ftyp(&b, "iso6");
    du_box_t moof; du_box_begin(&b, &moof, "moof");
    du_buf_zeros(&b, 50);
    du_box_end(&b, &moof);
    du_box_t mdat; du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    du_result_t r; memset(&r, 0, sizeof(r));
    /* 有 moof 无 moov：碎片化/未完成下载，不做猜测 */
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_ERR_NOT_CONTAINER);
    du_buf_free(&b);
}

static void test_mp4_itunsmpb(void) {
    du_buf_t b; du_buf_init(&b);
    put_ftyp(&b, "M4A ");
    du_box_t moov; du_box_begin(&b, &moov, "moov");
    put_mvhd(&b, 44100);
    uint64_t stts[] = { 44100, 1 };
    put_trak(&b, 44100, 0, 0, stts, 2, 1);
    /* udta 里塞 iTunSMPB 文本（前置 0 采样 / 后置 0x380=896 采样） */
    du_box_t udta; du_box_begin(&b, &udta, "udta");
    du_buf_bytes(&b, "iTunSMPB 00000000 00000380 00000000 padding", 44);
    du_box_end(&b, &udta);
    du_box_end(&b, &moov);
    du_box_t mdat; du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_OK);
    DU_CHECK((r.flags & DU_F_GAPLESS_HINT) != 0);
    DU_CHECK_EQ(r.duration_samples, 44100ull);

    du_options_t o; memset(&o, 0, sizeof(o));
    o.abi_version = DURATA_ABI_VERSION;
    o.gapless_correct = 1;
    memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, &o, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 44100ull - 896);
    du_buf_free(&b);
}

static void test_mp4_corrupt_size(void) {
    du_buf_t b; du_buf_init(&b);
    put_ftyp(&b, "M4A ");
    du_box_t moov; du_box_begin(&b, &moov, "moov");
    put_mvhd(&b, 600);
    uint64_t stts[] = { 10, 1024 };
    put_trak(&b, 44100, 0, 0, stts, 2, 1);
    du_box_end(&b, &moov);
    /* 恶意 box：size=3（< 8） */
    du_buf_be32(&b, 3);
    du_buf_bytes(&b, "free", 4);
    du_buf_zeros(&b, 64);
    du_box_t mdat; du_box_begin(&b, &mdat, "mdat");
    du_buf_zeros(&b, 100);
    du_box_end(&b, &mdat);

    du_result_t r; memset(&r, 0, sizeof(r));
    /* moov 在坏 box 之前，解析仍成功；坏 box 触发停止遍历不影响结果 */
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP4, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 10240ull);
    du_buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* FLAC / WAV / OGG                                                    */
/* ------------------------------------------------------------------ */

static void test_flac_basic(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_flac(&b, 44100, 2, 16, 441000);
    du_buf_zeros(&b, 500);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_FLAC, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 441000ull);
    DU_CHECK_NEAR(r.duration_sec, 10.0, 1e-9);
    DU_CHECK_EQ(r.sample_rate, 44100);
    DU_CHECK_EQ(r.channels, 2);
    DU_CHECK_EQ(r.bit_depth, 16);
    /* FLAC 没有每帧码率概念，isVBR 恒 0 */
    DU_CHECK_EQ(du_result_is_vbr(&r), 0);
    du_buf_free(&b);
}

static void test_flac_stream(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_flac(&b, 44100, 2, 16, 0);    /* total==0：流式录制 */
    for (int i = 0; i < 3; i++) du_synth_flac_frame(&b, 192);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_FLAC, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 3);
    DU_CHECK_EQ(r.duration_samples, 576ull);
    du_buf_free(&b);
}

static void test_wav_basic(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_wav(&b, 44100, 2, 16, 882000);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_WAV, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 220500ull);   /* 882000 / 4 */
    DU_CHECK_NEAR(r.duration_sec, 5.0, 1e-9);
    DU_CHECK_EQ(r.sample_rate, 44100);
    DU_CHECK_EQ(r.channels, 2);
    DU_CHECK_EQ(r.bit_depth, 16);
    du_buf_free(&b);
}

static void test_wav_rf64(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_rf64(&b, 48000, 2, 16, 1920000);   /* ds64 里的 dataSize */
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_WAV, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 480000ull);  /* 1920000 / 4 */
    DU_CHECK_NEAR(r.duration_sec, 10.0, 1e-9);
    du_buf_free(&b);
}

static void test_aiff(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_aiff(&b, 44100, 2, 16, 220500);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_WAV, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 220500ull);
    DU_CHECK_NEAR(r.duration_sec, 5.0, 1e-9);
    DU_CHECK_EQ(r.sample_rate, 44100);
    du_buf_free(&b);
}

static void test_ogg_vorbis(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_ogg_vorbis(&b, 44100, 441000);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_OGG, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.duration_samples, 441000ull);
    DU_CHECK_NEAR(r.duration_sec, 10.0, 1e-9);
    DU_CHECK_EQ(r.sample_rate, 44100);
    du_buf_free(&b);
}

static void test_ogg_opus(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_ogg_opus(&b, 312, 480000);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_OGG, NULL, &r), DU_OK);
    /* Opus：granule 扣 pre-skip，恒 48k */
    DU_CHECK_EQ(r.duration_samples, 480000ull - 312);
    DU_CHECK_EQ(r.sample_rate, 48000);
    du_buf_free(&b);
}

/* ------------------------------------------------------------------ */
/* 公共 API                                                            */
/* ------------------------------------------------------------------ */

static void test_api_options(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_cbr(&b, 5);
    du_result_t r; memset(&r, 0, sizeof(r));

    du_options_t o; memset(&o, 0, sizeof(o));
    o.abi_version = 99;   /* ABI 不匹配必须拒绝 */
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_MP3, &o, &r), DU_ERR_INVALID_ARG);

    du_source_t* s = NULL;
    DU_CHECK_EQ(du_source_open_mem(NULL, 0, &s), DU_ERR_INVALID_ARG);
    du_buf_free(&b);
}

static void test_api_sniff_auto(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_cbr(&b, 7);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_AUTO, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 7);

    du_buf_free(&b);
    du_buf_init(&b);
    du_buf_zeros(&b, 64);
    memset(&r, 0, sizeof(r));
    /* 嗅探不出来的直接 NOT_CONTAINER */
    DU_CHECK_EQ(probe_buf(&b, DU_FORMAT_AUTO, NULL, &r), DU_ERR_NOT_CONTAINER);
    du_buf_free(&b);
}

/* 自定义 IO：模拟 Android AssetManager 那种必须走回调的读源 */
static int64_t custom_read(void* user, uint64_t off, void* dst, int64_t n) {
    du_buf_t* b = (du_buf_t*)user;
    if (off >= b->len) return 0;
    uint64_t avail = b->len - off;
    if ((uint64_t)n > avail) n = (int64_t)avail;
    memcpy(dst, b->data + off, (size_t)n);
    return n;
}
static int64_t custom_size(void* user) { return (int64_t)((du_buf_t*)user)->len; }

static void test_api_custom_source(void) {
    du_buf_t b; du_buf_init(&b);
    du_synth_mp3_cbr(&b, 9);
    du_io_vtable vt;
    memset(&vt, 0, sizeof(vt));
    vt.abi_version = DURATA_ABI_VERSION;
    vt.read = custom_read;
    vt.size = custom_size;

    du_source_t* s = NULL;
    DU_CHECK_EQ(du_source_open_custom(&vt, &b, &s), DU_OK);
    du_result_t r; memset(&r, 0, sizeof(r));
    DU_CHECK_EQ(du_probe(s, DU_FORMAT_AUTO, NULL, &r), DU_OK);
    DU_CHECK_EQ(r.frames, 9);
    du_source_close(s);
    du_buf_free(&b);
}

static void test_api_strerror(void) {
    for (int st = 0; st <= 8; st++) {
        DU_CHECK(du_strerror((du_status_t)st) != NULL);
        DU_CHECK(strlen(du_strerror((du_status_t)st)) > 0);
    }
}

void du_test_containers(void) {
    DU_TEST_RUN("mp4/basic stts", test_mp4_basic);
    DU_TEST_RUN("mp4/mdhd v1 64bit", test_mp4_mdhd_v1);
    DU_TEST_RUN("mp4/multi trak", test_mp4_multi_trak);
    DU_TEST_RUN("mp4/elst", test_mp4_elst);
    DU_TEST_RUN("mp4/fragmented moof", test_mp4_fragmented);
    DU_TEST_RUN("mp4/itunsmpb", test_mp4_itunsmpb);
    DU_TEST_RUN("mp4/corrupt box size", test_mp4_corrupt_size);
    DU_TEST_RUN("flac/streaminfo", test_flac_basic);
    DU_TEST_RUN("flac/unknown total", test_flac_stream);
    DU_TEST_RUN("wav/pcm", test_wav_basic);
    DU_TEST_RUN("wav/rf64", test_wav_rf64);
    DU_TEST_RUN("aiff/comm", test_aiff);
    DU_TEST_RUN("ogg/vorbis", test_ogg_vorbis);
    DU_TEST_RUN("ogg/opus preskip", test_ogg_opus);
    DU_TEST_RUN("api/options abi", test_api_options);
    DU_TEST_RUN("api/sniff auto", test_api_sniff_auto);
    DU_TEST_RUN("api/custom source", test_api_custom_source);
    DU_TEST_RUN("api/strerror", test_api_strerror);
}
