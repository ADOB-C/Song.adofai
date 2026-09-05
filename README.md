# Music.adofai — ADOFAI 音频编解码器（独立项目）

把 PCM 音频无损编码成 `.adofai` 谱面（"audio-as-chart"），并可从谱面逐样本还原
回 WAV。社区自动制谱工具（如 `Unity.wav_rate.adofai` 的制作者，作者字段
"apofaiautomaker"）用这种方式把整首歌塞进一个谱子：**每个音频采样 = 一层**，
BPM/60 = 采样率。

## 原理 / 格式规格

- 源音频：**单声道、16-bit PCM**（默认 44.1 kHz；其它采样率自动换算 BPM）。
- `settings.bpm = 采样率 × 60`（44.1 kHz → **2,646,000**，与 Unity.wav_rate 谱一致）。
- 每层一条动作：`SetHitsound`（gameSound "Hitsound"，hitsound "Kick"），
  floor 1..N 依次对应采样 0..N-1。
- 采样值存放：`hitsoundVolume = int16 / 655.36`。
  该映射是**二进制的精确 dyadic 小数**，float64 可无损表示
  → `decode(encode(x)) == x`，往返逐样本零误差（已在 1.4 GB 谱面上验证）。
- `angleData` 全 0（方向对音频无意义）。

## 用法

```bash
# WAV → 谱面（1 分钟 44.1k 音频 ≈ 6 亿字节 JSON 写入约 30 秒）
python3 adofai_audio_codec.py encode in.wav out.adofai

# 谱面 → WAV（默认 bit-exact 还原；--gain 0.5 适合直接听）
python3 adofai_audio_codec.py decode out.adofai back.wav
python3 adofai_audio_codec.py decode out.adofai listen.wav --gain 0.5

# 与参考 WAV 逐样本比对（0 误差 = 无损闭环）
python3 adofai_audio_codec.py verify out.adofai in.wav

# 谱面音频元信息
python3 adofai_audio_codec.py info in.adofai

# 快速自检（合成 1 秒音调往返）
python3 adofai_audio_codec.py self-test
```

纯 Python 标准库，无第三方依赖；解析用 mmap 流式正则，1.5 GB 谱面解码约 15 秒。

## 性能参考（Apple Silicon M2，实测）

| 操作 | 耗时 |
|---|---|
| 编码 10,980,865 采样 → 1.4 GB JSON | ~2 分钟 |
| 解码 1.4 GB JSON → WAV | ~15 秒 |
| 往返校验 | 0 / 10,980,865 不匹配 |

## 已验证的谱面

- `~/Documents/Charts/Unity.wav_rate/Unity.wav_rate.adofai`（1.5 GB，第三方）——
  解码与官方原声不同：响段被响度/饱和处理损坏（crest 5 dB、样本级去相关）。
- `~/Documents/Charts/TheFatRat - Unity (SoundCloud)/Unity (SoundCloud).adofai`（本工具编码）——
  解码与 SoundCloud 原声 bit-exact（0 / 10,995,548 误差）。

## 注意

- 编码不重采样、不混音：输入必须是单声道 16-bit；立体声请先用
  `ffmpeg -i in.m4a -ac 1 -ar 44100 -sample_fmt s16 mono.wav` 转换。
- 这种谱在播放器里视觉上是一条直线匀速前进，属正常现象（不是渲染 bug）；
  超大谱 + Fade/大量 Twirl 图标会触发播放器热路径，见 ADOCAO 仓库拖尾调研。
