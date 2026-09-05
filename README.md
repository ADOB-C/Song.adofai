# Music.adofai — ADOFAI 音频编解码器（独立项目）

把音频无损编码成 `.adofai` 谱面（"audio-as-chart"），并可从谱面逐样本还原
回 WAV。社区自动制谱工具（如 `Unity.wav_rate.adofai` 的制作者，作者字段
"apofaiautomaker"）用这种方式把整首歌塞进一个谱子：**每个音频采样 = 一层**，
BPM/60 = 采样率。

## 原理 / 格式规格（v2 布局）

- 输入：任意 ffmpeg 可解码的音频（wav/m4a/webm/opus/mp3/flac…）；
  纯 WAV（mono 16-bit）走标准库，其余自动经 ffmpeg 转单声道、保留原始采样率。
- `settings.bpm = 采样率 × 60`（44.1 kHz → **2,646,000**，与 Unity.wav_rate 谱一致）。
- `settings.hitsoundVolume` = 第一个采样的音量 → **floor 1 不需要事件**。
- SetHitsound 音量在 ADOFAI 中向前持续生效 → **只在采样值变化处写事件**：
  开头/结尾静音、重复值全部零成本。
- 尾部静音无需事件：解码器按 angleData 推导的层数自动补零。
- `angleData` 全 0 且**折行压缩**（每行数千个 `0`），JSON 依然合法。
- 采样值存放：`hitsoundVolume = int16 / 655.36`（dyadic，float64 无损；0 写作 `0`）。
  → `decode(encode(x)) == x` 逐样本零误差。
- 生成的谱面内嵌元数据：`settings.artist` / `settings.song` / `settings.author`
  （author 固定署名 `Music.adofai (https://github.com/CHT-1192/Music.adofai)`）。
- **解码兼容旧版**：通过 `settings.hitsoundVolume` 判别布局
  （≤50 = v2 初始音量；100 = 旧版/第三方全事件谱），第三方 1.5 GB 谱验证通过。

## 用法

```bash
# 音频 → 谱面。省略输出路径时按元数据自动命名 "Artist - Title.adofai"
python3 adofai_audio_codec.py encode in.m4a out.adofai
python3 adofai_audio_codec.py encode in.webm --out-dir ~/Documents/Charts/Music.adofai
python3 adofai_audio_codec.py encode in.wav --title "Unity" --artist "TheFatRat"

# 谱面 → WAV（默认 bit-exact 还原；--gain 0.5 适合直接听）
python3 adofai_audio_codec.py decode out.adofai back.wav
python3 adofai_audio_codec.py decode out.adofai listen.wav --gain 0.5

# 与参考音频逐样本比对（0 误差 = 无损闭环）
python3 adofai_audio_codec.py verify out.adofai in.wav

# 谱面元数据（artist/song/author + 采样率/时长/层数）
python3 adofai_audio_codec.py info in.adofai

# 快速自检（合成 1 秒音调往返）
python3 adofai_audio_codec.py self-test
```

### 自动命名的标签来源

优先顺序：`--title/--artist` 参数 > ffprobe 读到的容器标签 > 文件名。
很多下载源（如部分 YouTube/SoundCloud 抓取）不带标签，此时会退回文件名；
可用 yt-dlp 先打印元数据再手动传入，例如：

```bash
yt-dlp --print "%(title)s | %(artist)s" URL
python3 adofai_audio_codec.py encode src.m4a --title "..." --artist "..."
```

## 依赖

- 纯 Python 标准库（WAV 编解码、JSON 谱面读写）；
- `ffmpeg`/`ffprobe`：可选，仅非 WAV 输入 / 容器标签读取时需要。

## 性能参考（Apple Silicon M2，实测）

| 操作 | 耗时 |
|---|---|
| v1 全事件编码（旧） | 1,515 MB / 曲（Unity 4:09） |
| v2 变化点编码（新） | 1,471 MB / 曲（省 ~44 MB；静音/重复越多越省） |
| 解码 1.4 GB JSON → WAV | ~15 秒 |
| 往返校验 | 0 / 千万级采样不匹配（v2/v1/第三方谱均通过） |

## 程序生成谱的存放约定

本工具产出的谱面统一放在 `~/Documents/Charts/Music.adofai/<Artist - Title>/`
（区别于他人手工谱）。

## 已知谱面对照

- `Unity.wav_rate.adofai`（第三方 1.5 GB）：解码与官方原声不同——响段被响度/
  饱和处理损坏（crest 5 dB、样本级去相关），损伤在其源文件，与格式无关。
- `Unity (SoundCloud).adofai` / `Thousand Nights Remembered.adofai`（本工具）：
  解码与源音频 bit-exact（0 / 千万级采样误差）。
