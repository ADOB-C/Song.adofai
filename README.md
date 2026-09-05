# Music.adofai — ADOFAI 音频编解码器（C，MIT）

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
  （author 固定 `Music.adofai (https://github.com/CHT-1192/Music.adofai)`）。
- 解码兼容旧版/第三方全事件谱（自动判别：settings 音量 ≤50 = v2；=100 = 旧版）。

## 构建与使用

```sh
make                 # cc -O2 → ./adofai-audio
make test            # 纯内存自检（不写盘）

# 音频 → 谱面（省略输出路径时按 --title/--artist 自动命名）
./adofai-audio encode in.m4a out.adofai --title "Unity" --artist "TheFatRat"
./adofai-audio encode in.wav --out-dir ~/Documents/Charts/Music.adofai

# 谱面 → WAV（默认 bit-exact；--gain 0.5 适合直接听）
./adofai-audio decode chart.adofai back.wav
./adofai-audio decode chart.adofai listen.wav --gain 0.5
ffplay -nodisp -autoexit listen.wav

# 与参考音频逐样本比对
./adofai-audio verify chart.adofai source.m4a

# 谱面元数据 + 音频信息
./adofai-audio info chart.adofai

./adofai-audio --help | --version | self-test
```

依赖：POSIX（mmap）+ C11；`ffmpeg`/`ffprobe` 仅非 WAV 输入与容器标签时使用。

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
util.c/h      公共设施：die/now/mmap(Map)/findb/skip_ws/esc_json/版本与署名常量
chart.c/h     .adofai 解析：settings 元数据、angleData 计数、SetHitsound 锚点扫描
pcm.c/h       PCM I/O：WAV 读取、ffmpeg 回退、ffprobe 标签、WAV 写出
codec.c/h     编解码核心：dyadic 文本格式化、音量持续重建、encode_core
commands.c    CLI 子命令：encode/decode/verify/info/self-test
main.c        CLI 入口：参数解析 + usage
cli.h         子命令接口
Makefile      make / make test / make install
LICENSE       MIT
```

解析器为锚点扫描（`SetHitsound` 定位 + 对象边界内取 floor/hitsoundVolume），
无完整 JSON DOM，1.5 GB 谱面秒级扫描。

## 已知谱面对照

- `Unity.wav_rate.adofai`（第三方 1.5 GB）：解码与官方原声不同——响段被响度/
  饱和处理损坏（crest 5 dB、样本级去相关），损伤在其源文件，与格式无关。
- 本工具生成的谱（`~/Documents/Charts/Music.adofai/`）与源音频 bit-exact。
