/* MP3 帧参数查表。全部 const，放只读段，多实例共享零拷贝。
 *
 * version 编码与帧头一致：0=MPEG2.5, 1=保留, 2=MPEG2, 3=MPEG1
 * layer 取 1..3。
 */
#include "internal.h"

/* MPEG1 Layer I */
static const uint16_t DU_BR_M1L1[16] =
    { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448, 0 };
/* MPEG1 Layer II */
static const uint16_t DU_BR_M1L2[16] =
    { 0,32,48,56,64,80,96,112,128,160,192,224,256,320,384, 0 };
/* MPEG1 Layer III */
static const uint16_t DU_BR_M1L3[16] =
    { 0,32,40,48,56,64,80,96,112,128,160,192,224,256,320, 0 };
/* MPEG2/2.5 Layer I */
static const uint16_t DU_BR_M2L1[16] =
    { 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256, 0 };
/* MPEG2/2.5 Layer II/III 共用 */
static const uint16_t DU_BR_M2L23[16] =
    { 0,8,16,24,32,40,48,56,64,80,96,112,128,144,160, 0 };

/* index 0..2 有效，3 = 保留（调用方拦截） */
static const uint32_t DU_SR[4][3] = {
    /* [0] MPEG2.5 */ { 11025, 12000, 8000 },
    /* [1] 保留     */ { 0, 0, 0 },
    /* [2] MPEG2    */ { 22050, 24000, 16000 },
    /* [3] MPEG1    */ { 44100, 48000, 32000 },
};

du_mp3_frame_info_t du_mp3_frame_info(uint32_t version, uint32_t layer,
                                      uint32_t br_idx, uint32_t sr_idx) {
    du_mp3_frame_info_t out = {0, 0, 0, 0, 1};
    if (version > 3 || version == 1 || layer == 0 || layer > 3 ||
        br_idx > 14 || br_idx == 0 || sr_idx > 2) {
        return out;
    }

    /* 帧头 layer 位编码：11=Layer I、10=Layer II、01=Layer III（00 保留）。
     * 调用方传的是原始位值，这里转成逻辑层号：1=I、2=II、3=III。
     * 不转的话 Layer III（真实 mp3 全是它）会被当成 Layer I 查表，
     * 帧长全错、双帧验证永远失败（实测踩过：ffmpeg/libmp3lame 输出全部 NO_SYNC）。 */
    uint32_t lay = (layer == 3) ? 1 : (layer == 2) ? 2 : 3;

    const uint16_t* br = NULL;
    if (version == 3) {                       /* MPEG1 */
        br = (lay == 1) ? DU_BR_M1L1 : (lay == 2) ? DU_BR_M1L2 : DU_BR_M1L3;
        out.spf = (lay == 1) ? 384 : 1152;
        out.coef = (lay == 1) ? 12 : 144;
        out.mul = (lay == 1) ? 4 : 1;
    } else {                                  /* MPEG2 / MPEG2.5 */
        br = (lay == 1) ? DU_BR_M2L1 : DU_BR_M2L23;
        out.spf = (lay == 1) ? 384 : (lay == 2) ? 1152 : 576;
        out.coef = (lay == 1) ? 12 : (lay == 2) ? 144 : 72;
        out.mul = (lay == 1) ? 4 : 1;
    }
    out.kbps = br[br_idx];
    out.sr   = DU_SR[version][sr_idx];
    return out;
}

/* 帧长公式：flen = coef × kbps × 1000 / sr + padding，L1 再 ×4。
 * 推导：帧长(字节) = spf/8 × kbps/sr；L1 最小寻址单位是 4 字节槽。
 * 例：MPEG1 L3 320kbps 44.1kHz → 144×320000/44100 = 1044 字节。 */
