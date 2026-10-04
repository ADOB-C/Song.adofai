# 格式规格（v2 布局）

- `settings.bpm = 采样率 × 60`（44.1 kHz → 2,646,000；48 kHz → 2,880,000）。
- `settings.hitsoundVolume` = 第一个采样（floor 1 无需事件）；SetHitsound 音量向前
  持续生效 → **只在采样值变化处写事件**（静音段/重复值零成本）。
- 尾部静音无需事件：解码按 angleData 推导的层数补零。
- `angleData` 全 0、**单行密排**。
- 采样值 `hitsoundVolume = int16 / 655.36`（dyadic，float64/文本精确；0 写 `0`）
  → `decode(encode(x)) == x` 逐样本零误差。
- 内嵌元数据 `settings.artist / song / author`；author 固定为
  `Song.adofai (https://github.com/CHT-1192/Song.adofai)`。
- 解码兼容旧版/第三方全事件谱（自动判别：settings 音量 ≤50 = v2；=100 = 旧版）。

写出布局：默认紧凑（无缩进/换行/多余空格）；`-pretty` 为带缩进的旧布局；
`-minimal` 省略事件里冗余的 `gameSound`/`hitsound` 键（仅保留 `floor` /
`eventType` / `hitsoundVolume`，音量仍是精确的 dyadic 小数，实验性）。

## 样本格式（int16 / float32）

- **int16 谱**（默认）：`hitsoundVolume = int16 / 655.36`，dyadic 十进制，文本精确。
- **float32 谱**：`hitsoundVolume = value × 50`，用 `%.9g` 写出（9 位有效数字足以唯一
  还原 float32，含 `-0.0` 的符号）；解码 `value = volume / 50`，**逐位无损**。
  24-bit PCM 的 24 位尾数可被 float32 精确承载，因此 24-bit 音源同样无损。
- **溯源信息**：`actions[1]` 是第二个 `EditorComment`，内容形如
  `adofai-music codec=flac fmt=s24 bits=24 ch=2 frames=9154501 crc64=1a2b3c4d5e6f7788 src=song.flac`
  （`crc64` 是谱面自身样本的 CRC64）。`-show` 会打印这一行，`-verify` 会重算并核对
  CRC64（不一致会告警）。源文件名只取 basename，不含路径。
- **模式标记**：`actions[0]` 是一个标准事件
  `{"floor":0,"eventType":"EditorComment","comment":"adofai-music:f32"}`（s16 谱写 `:s16`）。
  解码端在 `"actions"` 附近 4 KiB 内查找该标记。没有标记时（旧版/第三方谱，或作者在
  编辑器里删掉了注释）按内容判别：`settings.hitsoundVolume > 50` → legacy int16；
  否则若**所有事件音量都 ≤50 且恰好落在 `int16/655.36` 网格上** → int16，否则 float32。
  这样即使首样本被削波（volume >50）的 float32 谱也不会被误判。`EditorComment` 是 ADOFAI 标准事件，
  多个第三方解析器都有对应类，编辑器里只显示为注释、不影响玩法。
- **`-sample_fmt auto`（默认）跟随源格式**：u8/s16 → int16 谱；24-bit PCM、32-bit float、
  以及 AAC/Opus 等解码器输出的 `fltp` → float32 谱。以上都无损。8/16/24-bit 整数与
  32-bit float 都喂得进；**native 解析覆盖 WAV（16/24-bit PCM、32-bit float）与
  Ogg Vorbis（stb_vorbis）**，其余格式走 ffmpeg/ffprobe。立体声按 ffmpeg 默认矩阵
  下混单声道（每声道 1/√n，能量守恒）；stb 与 ffmpeg 的 Vorbis 解码差异在
  ~5e-7 量级（不同 IMDCT 实现的浮点舍入），谱面相对自身解码结果仍逐位无损。
- **不会悄悄掉精度**：64-bit float（`dbl`）与 32-bit 整数（`bits_per_raw_sample > 24`）
  超出 float32 的 24 位尾数，`auto` 下直接报错并要求显式 `-sample_fmt f32` 接受降精度；
  8-bit 无符号只占 8 位，进 int16 谱无损。
- 输出为严格合法 JSON：数组元素之间才写逗号，**没有尾逗号**（旧版本会写尾逗号，
  ADOFAI 容忍、严格解析器不容忍）。

解析器为锚点扫描（`SetHitsound` 定位 + 对象边界内取 floor/hitsoundVolume），
无完整 JSON DOM，1.5 GB 谱面秒级扫描。

## 版本（1.0.0 之前）

项目尚未发布（未 tag、无外部用户），版本号仅作开发期进度标记：0.1.x Python/v1
全事件布局 → 0.2.x Python/v2 变化点布局 → 0.3.x C 重写 + 模块化 → 0.4.x
（当前）ffmpeg 风格 CLI、xz/zstd、miniaudio 播放、紧凑 JSON、无损 float32。
首个稳定发布定为 1.0.0，在此之前不打 tag。

## 已知谱面对照

- `Unity.wav_rate.adofai`（第三方 1.5 GB）：解码与官方原声不同——响段被响度/饱和
  处理损坏（crest 5 dB、样本级去相关），损伤在其源文件，与格式无关。
- 本工具生成的谱与源音频 bit-exact。
