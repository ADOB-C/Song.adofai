# Song.adofai — ADOFAI 音频编解码器（C，MIT）

把音频无损编码成 `.adofai` 谱面（audio-as-chart），再从谱面逐样本还原：
**每个采样 = 一层**，`bpm/60 = 采样率`，`decode(encode(x)) == x` 零误差。
支持 xz/zstd 压缩存储（4 分钟谱面 ~1.5 GB 文本 → 50–80 MB）与内置播放。

## 快速开始

```sh
make                       # → ./build/adofai-music
make test                  # 纯内存自检（不写盘）

./build/adofai-music encode in.m4a out.adofai --title "Unity" --artist "TheFatRat"
./build/adofai-music encode in.wav out.adofai.xz    # .xz（或 .zst）边编码边压缩
./build/adofai-music play   out.adofai.xz --gain 0.5
./build/adofai-music decode out.adofai.xz back.wav
./build/adofai-music verify out.adofai.xz in.m4a
./build/adofai-music info   out.adofai.xz
./build/adofai-music bench  out.adofai.xz           # 压缩档位对比（零落盘）
```

`decode / play / verify / info` 按内容自动识别明文 `.adofai`、`.xz`、`.zst`。

## 格式（v2）

- `bpm = 采样率 × 60`；`hitsoundVolume = int16 / 655.36`（dyadic → 文本精确）
- 只在采样值变化处写 `SetHitsound`：静音/重复值零成本，尾部静音由层数补零
- `angleData` 全 0、单行密排；内嵌 `artist / song / author`
- 兼容旧版与第三方全事件谱（按 settings 音量自动判别）

细节见 [docs/format.md](docs/format.md)。

## 压缩与默认档

输出扩展名决定格式：`.xz`（默认 level 6，`--xz-level 9e` 最小）或 `.zst`
（默认 19，`--zstd-level 3` 最快）；编解码均多线程，"3–4 分钟 < 100 MB" 有余量。

| 档位 | 4 分钟体积 | 编码耗时 |
|---|---|---|
| **xz 6（默认）/ zstd 19（默认）** | ~71 MB | ~36 s / ~78 s |
| xz 9e（最小） | ~59 MB | ~140 s |
| zstd 3（最快） | ~96 MB | ~0.3 s |

实测数据与默认值推导见 [docs/bench.md](docs/bench.md)。

## 性能（Apple Silicon M2 实测）

| 操作 | 时间 |
|---|---|
| 解码 1.47 GB 明文谱 | 3.0 s |
| 解码 1.24 GB 文本的 `.xz`（解压 + 解析） | 3.2 s |
| 编码 / 自检 | 写盘受限 ~5 s / 纯内存 <1 s |

## 依赖

POSIX（mmap）+ C11；**liblzma**、**libzstd**（`brew install xz zstd`）；播放用随附的
`third_party/miniaudio.h`（无需安装）；`ffmpeg`/`ffprobe` 仅非 WAV 输入与标签时用。

## 许可

MIT。第三方：XZ Utils (0BSD)、Zstandard (BSD-3-Clause)、miniaudio (public domain / MIT-0)。
