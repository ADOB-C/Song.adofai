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

解析器为锚点扫描（`SetHitsound` 定位 + 对象边界内取 floor/hitsoundVolume），
无完整 JSON DOM，1.5 GB 谱面秒级扫描。

## 版本（1.0.0 之前）

项目尚未发布（未 tag、无外部用户），版本号仅作开发期进度标记：0.1.x Python/v1
全事件布局 → 0.2.x Python/v2 变化点布局 → 0.3.x C 重写 + 模块化（当前）。
首个稳定发布定为 1.0.0，在此之前不打 tag。

## 已知谱面对照

- `Unity.wav_rate.adofai`（第三方 1.5 GB）：解码与官方原声不同——响段被响度/饱和
  处理损坏（crest 5 dB、样本级去相关），损伤在其源文件，与格式无关。
- 本工具生成的谱与源音频 bit-exact。
