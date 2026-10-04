# Song.adofai — ADOFAI 音频编解码器（C，MIT）

把音频无损编码成 `.adofai` 谱面（audio-as-chart），再从谱面逐样本还原：
**每个采样 = 一层**，`bpm/60 = 采样率`，`decode(encode(x)) == x` 零误差。
支持 xz/zstd 压缩存储（4 分钟谱面 ~1.5 GB 文本 → 50–80 MB）与内置播放。

## 快速开始

语法仿 ffmpeg：`-i` 标输入、位置参数是输出、输出扩展名决定格式；`-f` 可强制格式
（`adofai`/`xz`/`zst`/`wav`），另有 `-threads N`（0=auto）、`-y/-n`、
`-loglevel quiet|info|verbose`、`-hide_banner`。

```sh
make                       # → ./build/adofai-music
make test                  # 纯内存自检（不写盘）

./build/adofai-music -i in.m4a -metadata song="Unity" -metadata artist="TheFatRat" out.adofai
./build/adofai-music -i in.wav out.adofai.xz     # .xz（或 .zst）边编码边压缩
./build/adofai-music -i in.wav -f xz -threads 4 chart.bin   # 扩展名无所谓，-f 说了算
./build/adofai-music -i out.adofai.xz back.wav   # 谱面 → WAV
./build/adofai-music -i out.adofai.xz -play -gain 0.5
./build/adofai-music -i out.adofai.xz -verify in.m4a
./build/adofai-music -i out.adofai.xz -show
./build/adofai-music -i out.adofai.xz -bench     # 压缩档位对比（零落盘）
```

`-show / -play / -verify / decode` 按内容自动识别明文 `.adofai`、`.xz`、`.zst`；
输出文件已存在时默认拒绝覆盖，加 `-y` 才行。

**不怕占满硬盘**：编码前按采样流预估明文体积并检查输出目录剩余空间，不够直接拒绝
（并提示改用 `-f xz`）；明文谱面很大时先给出体积提示，写到一半失败会自动删除半成品。

## 格式（v2）

- `bpm = 采样率 × 60`；`hitsoundVolume = int16 / 655.36`（dyadic → 文本精确）
- 只在采样值变化处写 `SetHitsound`：静音/重复值零成本，尾部静音由层数补零
- `angleData` 全 0、单行密排；内嵌 `artist / song / author`
- 兼容旧版与第三方全事件谱（按 settings 音量自动判别）

细节见 [docs/format.md](docs/format.md)。

## 压缩与默认档

JSON 默认**紧凑**写出（明文比缩进版小 ~11%）；`-pretty` 恢复可读缩进版，`-minimal`
再省 ~42%（省略事件里冗余的 `gameSound`/`hitsound` 键，音量精度不变，实验性、需在
ADOFAI 里验证）。

输出扩展名决定格式：`.xz`（默认 level 6，`-xz-level 9e` 最小）或 `.zst`
（默认 19，`-zstd-level 3` 最快）；编解码均多线程，"3–4 分钟 < 100 MB" 有余量。

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
