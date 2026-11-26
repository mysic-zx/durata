# durata

**English** | [简体中文](README.zh-CN.md)

Cross-platform library for accurate audio duration measurement. C11 core with a stable C ABI, zero third-party dependencies, no ffmpeg linkage.

## Features

- **Accurate**: MP3 duration comes from a full frame-by-frame scan (strict header validation + double-sync verification against false syncs), never trusting metadata that may be corrupted (Xing, etc.). M4A uses the sample-level `stts` duration. FLAC/WAV/OGG each use the authoritative field defined by their container spec.
- **Fast**: single sequential read + sliding window; a 1-hour 320 kbps MP3 full scan finishes in well under 0.5 s (theoretical budget, see the design doc §13).
- **Zero allocation**: `du_source_open_mem` runs with 0 mallocs — safe on real-time audio threads.
- **Deterministic across platforms**: everything is accumulated in integers; the one division to seconds happens at the end. The same file yields byte-identical `duration_samples` on any platform/compiler.
- **Trim at compile time**: every format can be disabled at build time (`-DDURATA_ENABLE_OGG=OFF`, etc.).
- **Embeddable**: besides reading files, a custom IO vtable is provided (entry point for WASM, Android AssetManager, network caches).

Supported formats: MP3 (MPEG1/2/2.5, including VBR/ABR/free-format/LAME gapless), M4A/MP4 (AAC/ALAC), FLAC (including streaming unknown-length), WAV/AIFF/RF64, OGG (Vorbis/Opus/FLAC-in-Ogg).

## Build

```bash
./init.sh --build     # configure + build + run the full test suite
```

Or directly:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tests/du_test
```

CMake options: `DURATA_BUILD_SHARED` (static by default), `DURATA_ENABLE_MP3/MP4/FLAC/OGG/WAV` (compile-time trimming), `DURATA_BUILD_TESTS`.

## Usage

```c
#include "durata.h"

du_result_t r;
du_status_t st = du_probe_path("music.mp3", DU_FORMAT_AUTO, NULL, &r);
if (st == DU_OK) {
    printf("%.3f s, %u Hz, isVBR=%d\n",
           r.duration_sec, r.sample_rate, du_result_is_vbr(&r));
}
```

Use the integer `r.duration_samples` for comparisons/sorting — not the floating-point `duration_sec`.

For custom readers (WASM, AssetManager, etc.) use `du_source_open_custom`. The vtable is just three function pointers (read/size/close); `read` takes an explicit offset, is stateless and reentrant.

## Tests

All 29 unit tests are driven by synthesized data — no external corpus, fully offline. The parser has additionally been verified against real MP3/M4A/AIFF files using `afinfo` as the reference.

## License

MIT, see [LICENSE](LICENSE).
