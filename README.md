# durata

跨端音频时长精确计算库。C11 核心 + 稳定 C ABI，零第三方依赖，不链接 ffmpeg。

## 特性

- **精确**：MP3 逐帧扫描取真值（完整帧头校验 + 双帧验证抗假同步），不依赖可能被污染的 Xing 等元数据；M4A 取 stts 样本级时长；FLAC/WAV/OGG 各取容器规范的权威字段
- **快**：单次顺序读 + 滑动窗口，1 小时 320kbps MP3 全扫 < 0.5 s（理论预算，见设计文档 §13）
- **零分配**：`du_source_open_mem` 模式全程 0 次 malloc，可跑在实时音频线程
- **跨端一致**：全程整数累加，最后一次除法出秒；同一文件在任意平台/编译器下 `duration_samples` 完全一致
- **可裁剪**：每种格式都能编译期关掉（`-DDURATA_ENABLE_OGG=OFF` 等）
- **可嵌入**：mmap 之外提供自定义 IO vtable（WASM / Android AssetManager / 网络缓存入口）

支持格式：MP3（MPEG1/2/2.5，含 VBR/ABR/free-format/LAME gapless）、M4A/MP4（AAC/ALAC）、FLAC（含流式 unknown-length）、WAV/AIFF/RF64、OGG（Vorbis/Opus/FLAC-in-Ogg）。

## 构建

```bash
./init.sh --build     # 配置 + 编译 + 跑全部单元测试
```

或直接：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tests/du_test
```

CMake 选项：`DURATA_BUILD_SHARED`（默认静态）、`DURATA_ENABLE_MP3/MP4/FLAC/OGG/WAV`（编译期裁剪）、`DURATA_BUILD_TESTS`。

## 使用

```c
#include "durata.h"

du_result_t r;
du_status_t st = du_probe_path("music.mp3", DU_FORMAT_AUTO, NULL, &r);
if (st == DU_OK) {
    printf("%.3f 秒，%u Hz，isVBR=%d\n",
           r.duration_sec, r.sample_rate, du_result_is_vbr(&r));
}
```

比较/排序请用整数 `r.duration_samples`，不要用浮点 `duration_sec`。

自定义读源（WASM、AssetManager 等）走 `du_source_open_custom`，vtable 只有 read/size/close 三个函数指针，read 带显式偏移、无状态可重入。

## 测试

29 项单元测试全部由合成数据驱动，零外部语料、可离线跑；另外用真实 MP3/M4A/AIFF 与 afinfo 做过对照验证。

## 许可证

MIT，见 [LICENSE](LICENSE)。
