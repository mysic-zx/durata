/* 入口：嗅探分发、options 默认化、结果结构归一（设计文档 §4.4） */
#include "internal.h"
#include <string.h>

int du_result_is_vbr(const du_result_t* r) {
    if (r == NULL) return 0;
    return (r->flags & DU_F_VBR) != 0;
}

const char* du_strerror(du_status_t st) {
    switch (st) {
    case DU_OK:                 return "ok";
    case DU_ERR_IO:             return "io error";
    case DU_ERR_EOF_UNEXPECTED: return "unexpected end of file";
    case DU_ERR_NO_SYNC:        return "no valid frame/header found";
    case DU_ERR_NOT_CONTAINER:  return "not a supported audio container";
    case DU_ERR_UNSUPPORTED:    return "unsupported variant";
    case DU_ERR_INVALID_ARG:    return "invalid argument";
    case DU_ERR_NOMEM:          return "out of memory";
    case DU_ERR_TRUNCATED:      return "file appears truncated";
    }
    return "unknown error";
}

/* ---- 嗅探 ---- */

static int du_looks_like_mp3_head(du_source_t* s) {
    uint64_t avail = 0;
    const uint8_t* p = du_io_view(s, 0, 4, &avail);
    if (p == NULL || avail < 4) return 0;
    if (memcmp(p, "ID3", 3) == 0) return 1;                     /* ID3v2 打头 */
    return p[0] == 0xFF && (p[1] & 0xE0) == 0xE0;               /* 裸帧头 */
}

static du_format_t du_sniff(du_ctx_t* c) {
    du_source_t* s = c->src;
    uint64_t avail = 0;
    const uint8_t* p = du_io_view(s, 0, 16, &avail);
    if (p == NULL) return DU_FORMAT_AUTO;

    if (avail >= 4 && memcmp(p, "fLaC", 4) == 0) return DU_FORMAT_FLAC;
    if (avail >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WAVE", 4) == 0) {
        return DU_FORMAT_WAV;
    }
    if (avail >= 12 && memcmp(p, "RF64", 4) == 0 && memcmp(p + 8, "WAVE", 4) == 0) {
        return DU_FORMAT_WAV;
    }
    if (avail >= 12 && memcmp(p, "FORM", 4) == 0 &&
        (memcmp(p + 8, "AIFF", 4) == 0 || memcmp(p + 8, "AIFC", 4) == 0)) {
        return DU_FORMAT_WAV;
    }
    if (avail >= 4 && memcmp(p, "OggS", 4) == 0) return DU_FORMAT_OGG;
    if (avail >= 8 && memcmp(p + 4, "ftyp", 4) == 0) return DU_FORMAT_MP4;
    if (du_looks_like_mp3_head(s)) return DU_FORMAT_MP3;
    return DU_FORMAT_AUTO;
}

/* ---- 默认 options ---- */

static const du_options_t DU_DEFAULT_OPTS = {
    DURATA_ABI_VERSION,
    0,     /* max_bytes */
    0,     /* max_frames */
    0,     /* fast_mp3 */
    0,     /* gapless_correct */
    0,     /* elst_apply */
    0,     /* strict */
    0,     /* free_format */
    1,     /* read_xing */
};

/* 校验调用方传入的 options，ABI 不匹配直接拒绝 */
static const du_options_t* du_check_opts(const du_options_t* opts) {
    if (opts == NULL) return &DU_DEFAULT_OPTS;
    if (opts->abi_version != DURATA_ABI_VERSION) return NULL;
    return opts;
}

du_status_t du_probe(du_source_t* s, du_format_t fmt,
                     const du_options_t* opts, du_result_t* out) {
    if (s == NULL || out == NULL) return DU_ERR_INVALID_ARG;
    const du_options_t* opt = du_check_opts(opts);
    if (opt == NULL) return DU_ERR_INVALID_ARG;
    if (fmt > DU_FORMAT_WAV) return DU_ERR_INVALID_ARG;

    memset(out, 0, sizeof(*out));
    out->abi_version = DURATA_ABI_VERSION;
    out->xing_delta = -1.0;

    du_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.src = s;
    ctx.opt = opt;
    ctx.res = out;
    ctx.st = DU_OK;

    if (fmt == DU_FORMAT_AUTO) {
        fmt = du_sniff(&ctx);
        if (fmt == DU_FORMAT_AUTO) return DU_ERR_NOT_CONTAINER;
    }

    switch (fmt) {
#if !defined(DURATA_NO_MP3)
    case DU_FORMAT_MP3:
    case DU_FORMAT_MP2:
        return du_probe_mp3(&ctx);
#else
    case DU_FORMAT_MP3:
    case DU_FORMAT_MP2:
        return DU_ERR_UNSUPPORTED;
#endif
#if !defined(DURATA_NO_MP4)
    case DU_FORMAT_MP4:
        return du_probe_mp4(&ctx);
#else
    case DU_FORMAT_MP4:
        return DU_ERR_UNSUPPORTED;
#endif
#if !defined(DURATA_NO_FLAC)
    case DU_FORMAT_FLAC:
        return du_probe_flac(&ctx);
#else
    case DU_FORMAT_FLAC:
        return DU_ERR_UNSUPPORTED;
#endif
#if !defined(DURATA_NO_OGG)
    case DU_FORMAT_OGG:
        return du_probe_ogg(&ctx);
#else
    case DU_FORMAT_OGG:
        return DU_ERR_UNSUPPORTED;
#endif
#if !defined(DURATA_NO_WAV)
    case DU_FORMAT_WAV:
        return du_probe_wav(&ctx);
#else
    case DU_FORMAT_WAV:
        return DU_ERR_UNSUPPORTED;
#endif
    default:
        return DU_ERR_INVALID_ARG;
    }
}

du_status_t du_probe_path(const char* path, du_format_t fmt,
                          const du_options_t* opts, du_result_t* out) {
    du_source_t* s = NULL;
    du_status_t st = du_source_open_path(path, &s);
    if (st != DU_OK) return st;
    st = du_probe(s, fmt, opts, out);
    du_source_close(s);
    return st;
}
