# Song.adofai — ADOFAI 音频编解码器（C，MIT）

把音频无损编码成 `.adofai` 谱面（"audio-as-chart"），并可从谱面逐样本还原
回 WAV。**每个音频采样 = 一层**，`BPM/60 = 采样率`。

## 格式规格（v2 布局）

- `settings.bpm = 采样率 × 60`（44.1 kHz → **2,646,000**；48 kHz → 2,880,000）。
- `settings.hitsoundVolume` = 第一个采样 → floor 1 无需事件；
  SetHitsound 音量向前持续生效 → **只在采样值变化处写事件**
  （静音段/重复值零成本）。
- 尾部静音无需事件：解码按 angleData 推导的层数补零。
- `angleData` 全 0、**单行密排**。
- 采样值：`hitsoundVolume = int16 / 655.36`（dyadic，float64/文本精确；0 写 `0`）
  → `decode(encode(x)) == x` 逐样本零误差。
- 内嵌元数据：`settings.artist / song / author`
  （author 固定 `Song.adofai (https://github.com/CHT-1192/Song.adofai)`）。
- 解码兼容旧版/第三方全事件谱（自动判别：settings 音量 ≤50 = v2；=100 = 旧版）。

## 版本（1.0.0 之前）

项目尚未发布（未 tag、无外部用户），版本号仅作开发期进度标记：

- **0.1.x** — Python 实现，v1 全事件布局
- **0.2.x** — Python 实现，v2 变化点布局（settings 初始音量 + 事件压缩 + 单行 angleData）
- **0.3.x** — C 重写 + 模块化（当前）
- 首个稳定发布定为 **1.0.0**；在此之前**不打 tag**。

## 构建与使用

```sh
make                 # 产物 → ./build/adofai-audio（.o 在 build/obj/，不入 git）
make test            # 纯内存自检（不写盘）

# 音频 → 谱面（省略输出路径时按 --title/--artist 自动命名）
./build/adofai-audio encode in.m4a out.adofai --title "Unity" --artist "TheFatRat"
./build/adofai-audio encode in.wav --out-dir ~/Documents/Charts/Song.adofai

# 谱面 → WAV（默认 bit-exact；--gain 0.5 适合直接听）
./build/adofai-audio decode chart.adofai back.wav
./build/adofai-audio decode chart.adofai listen.wav --gain 0.5

# 直接播放（明文或 .xz 均可，不写临时 WAV；ffplay 只是可选项）
./build/adofai-audio play chart.adofai.xz --gain 0.5

# 与参考音频逐样本比对
./build/adofai-audio verify chart.adofai source.m4a

# 谱面元数据 + 音频信息
./build/adofai-audio info chart.adofai

# 压缩基准：各档位体积/速度 + 往返校验（全内存，零落盘）
./build/adofai-audio bench chart.adofai.xz            # 默认取中段 128 MiB 文本
./build/adofai-audio bench chart.adofai --slice 32    # 更快
./build/adofai-audio bench chart.adofai --full        # 整份文本（最慢最准）

./build/adofai-audio --help | --version | self-test
```

### 压缩存储：xz / zstd（省磁盘）

谱面是 ~1.4 GB/4 分钟的 JSON 文本，两种格式都支持，按输出扩展名选择：

```sh
# .xz → xz 压缩（默认 level 6，--xz-level 0-9 / 9e 可调）
./build/adofai-audio encode in.wav out.adofai.xz
# .zst / .zstd → zstd 压缩（默认 level 19，--zstd-level 1-22 可调）
./build/adofai-audio encode in.wav out.adofai.zst --zstd-level 3
# decode / play / verify / info 按内容自动识别两种格式（扩展名无关）
./build/adofai-audio decode out.adofai.zst back.wav
./build/adofai-audio play out.adofai.xz
./build/adofai-audio verify out.adofai.zst in.wav
./build/adofai-audio info out.adofai.zst
```

产物是标准 `.xz`（CRC64）/ `.zst`（带校验和），可与 `xz` / `zstd` 命令行互通；
编码全程内存流式、不落明文。编解码都走多线程（xz 分块 + liblzma MT，zstd 多
worker）。

实测选择依据（本机 256 MiB 真实谱面文本，多线程）：

| 方案 | 压缩后 | 压缩耗时 | 说明 |
|---|---|---|---|
| xz 9e | 8.72 MB | 60 s | 体积最小（比 zstd 最高档还小 ~20%） |
| xz 6（默认） | 10.8 MB | 7 s | 体积/速度折中 |
| zstd 19（默认） | 10.7 MB | 14 s | 体积≈xz 6，解码快数倍 |
| zstd 3 | 14.5 MB | 0.3 s | 追速度（约 +35% 体积） |

4 分钟谱面 text ~1.4 GB 对应 xz 6/9e 约 60/43 MB、zstd 19/3 约 50/68 MB，都远
低于 100 MB。读压缩谱面需整流解压进内存（超大谱面请留意内存）；多线程解码下
1.24 GB 文本的 `.xz` 端到端（解压 + 解析）实测 3.2 s。

`bench` 子命令可在内存里对任意档位复测（零落盘）：取谱面文本中段切片（避开头部
`angleData` 零区与尾部稀疏区），逐档位压缩→解压→比字节，并按该谱面自身的
「文本字节/音频秒」外推 3 分钟体积、与 100 MB 预算比对。真实谱面（1.24 GB 文本，
190.7 s）32 MiB 切片实测：

```
codec lvl        size    ratio  comp MB/s   dec MB/s  est 3min  budget roundtrip
xz    3        1.71 MB    19.6x       64.4      480.1    59.5 MB    PASS OK
xz    6        1.55 MB    21.7x       14.2      504.2    53.9 MB    PASS OK
xz    9e       1.28 MB    26.3x        3.9      489.9    44.5 MB    PASS OK
zstd  3        2.09 MB    16.0x     4305.7     3710.5    72.9 MB    PASS OK
zstd  19       1.62 MB    20.7x        5.0     3345.7    56.5 MB    PASS OK
zstd  22       1.58 MB    21.2x        4.0     3350.8    55.2 MB    PASS OK
```

（该谱面磁盘上实际 45.2 MB/190.7 s，换算 3 分钟 ≈42.7 MB，与 xz 9e 外推值吻合。）

依赖：POSIX（mmap）+ C11 + **liblzma**（`brew install xz` / Debian `liblzma-dev`）
+ **libzstd**（`brew install zstd` / Debian `libzstd-dev`）；可用 `make
LZMA_CFLAGS=... ZSTD_CFLAGS=... LZMA_LIBS=... ZSTD_LIBS=...` 指向自定义安装；
`play` 用随附的 **miniaudio**（`third_party/miniaudio.h`，无需安装，macOS 需
CoreAudio 系框架，Linux 需 ALSA 等后端库）；`ffmpeg`/`ffprobe` 仅非 WAV 输入与
容器标签时使用。

第三方致谢：
- xz 压缩使用 **XZ Utils (liblzma)** <https://tukaani.org/xz/>，采用 BSD Zero
  Clause (0BSD) 许可证，与本项目 MIT 许可兼容。
- zstd 压缩使用 **Zstandard (libzstd)** <https://facebook.github.io/zstd/>，
  BSD 3-Clause / GPLv2 双许可，本项目按 BSD 3-Clause 使用。
- 播放使用 **miniaudio** <https://miniaud.io/>（v0.11.25，随附于
  `third_party/miniaudio.h`），public domain / MIT-0 双许可。

## 性能（Apple Silicon M2 实测，解码纯读已有文件、不落盘）

| 操作 | C | Python（旧版，仅供对比） |
|---|---|---|
| 解码 1.47 GB 谱（10,995,548 采样） | **3.0 s** | ~12 s |
| 解码第三方 1.5 GB 谱（10,993,499 采样） | **4.0 s** | ~15 s |
| 编码（主要受写盘速度限制） | ~5 s（1.5 GB 输出） | ~5 s |
| 自检 | 纯内存，<1 s（零磁盘写入） | 写临时文件 |

往返校验全部 **0 / 千万级采样误差**（含第三方谱向后兼容）。

## 结构（多文件，单文件 ≤250 行）

```
src/
├── *.c             实现（util/chart/pcm/codec/xz/zstd/commands/bench/play/main）
└── include/*.h     公共头文件（编译时 -Isrc/include）
third_party/       随附第三方单头库（miniaudio.h）
build/             编译产物（二进制 + .o，已被 .gitignore 排除）
Makefile / LICENSE / README.md
```

解析器为锚点扫描（`SetHitsound` 定位 + 对象边界内取 floor/hitsoundVolume），
无完整 JSON DOM，1.5 GB 谱面秒级扫描。

## 已知谱面对照

- `Unity.wav_rate.adofai`（第三方 1.5 GB）：解码与官方原声不同——响段被响度/
  饱和处理损坏（crest 5 dB、样本级去相关），损伤在其源文件，与格式无关。
- 本工具生成的谱（`~/Documents/Charts/Song.adofai/`）与源音频 bit-exact。
