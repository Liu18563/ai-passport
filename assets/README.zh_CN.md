<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 资源目录（Assets）

本目录集中存放可复用的资源（字库、图片、音乐等），按资源类型分子目录管理。每个资源放在其类型对应的子目录，并记录放置路径、命名方式、集成方式与来源/许可。二进制资源（字体、图片、音频）不属于纯 markdown 文档，请勿与文档混放。涉及版权/授权的资源需注明来源与许可。

## 字库（fonts）

可复用的字库文件与生成的字库源码放在 `fonts/`。

- 命名要能反映字族、字重、字级与格式。
- 记录来源、许可、字符范围、转换命令与目标放置路径。
- 添加字库前评估 Flash 与内部 RAM 影响；ESP32-C3 无 PSRAM。
- 不提交许可不允许分发的字库。

## 图片（images）

可复用的源图与生成的显示资产放在 `images/`。

| 文件 | 尺寸与格式 | 用途与来源 |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160，JPEG | 嵌入中英文项目 README 的产品主图，突出 AI Passport 产品形象与开放、人人可创作的理念。 |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724，PNG RGBA | 保留为可选技术参考图，不再用于首页主视觉。于 2026-09-17 使用内置图像生成工具为本仓库生成；已根据文档中的硬件能力契约核对图中的六项标签与参数。 |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336，PNG RGBA | 从仓库原始 `images/logo.png` 中精确裁切并去除背景的黑色字标；用于中英文项目 README 的浅色主题。 |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336，PNG RGBA | 提取字标的白色版本；README 使用 `<picture>` 在 GitHub 深色主题下显示。 |

- 使用描述性命名，并记录尺寸、像素格式、转换步骤与目标路径。
- 优先采用适合 240 × 320 RGB565 显示的格式，并纳入 Flash 与内部 RAM 考量。
- 许可允许时保留可编辑源文件，并记录来源与许可。
- 图片中不得包含设备二维码秘密、凭证或个人数据。

## 音乐与音效（music）

可复用的音乐与音效源码放在 `music/`。

- 记录来源、许可、采样率、位深、声道、转换命令与目标路径。
- 与当前 BSP 音频路径匹配时优先采用 16 kHz、16 位单声道 PCM。
- 嵌入音频前评估 Flash 与内部 RAM 成本；长录音应流式或分块。
- 无再分发许可不提交媒体文件。

## 俄语单词斩（feature/russian-words）

| 路径 | 说明 |
| --- | --- |
| `fonts/ruw_font_20.c` / `ruw_font_28.c` / `ruw_font_36.c` | Noto Sans SC（SIL OFL 1.1）经 lv_font_conv 1.5.2 转换，4 bpp 未压缩。20px 覆盖 `tools/ruwords/glyphs.txt` 全部字符，28/36px 仅 ASCII 与西里尔字母。再生成：`tools/ruwords/gen_fonts.sh <字体路径>` |
| `music/ruwords_voice.adpcm` | 301 个单词的俄语读音，macOS `say -v Milena` 合成，ffmpeg 去首尾静音后转 16 kHz 单声道，再编码为 IMA-ADPCM 4 bit。再生成：`python3 tools/ruwords/gen_assets.py` |
| `images/ruw_logo.c` | 88 × 88，RGB565，未压缩、无 alpha。原创套娃 Logo 五帧：`ruw_logo_r0/r5/r10/r15`（逆时针旋转 0/5/10/15 度）与 `ruw_logo_blink`（15 度闭眼）；`ruw_logo` 为 `r15` 的兼容别名。源图 `images/source/matryoshka_source.png` 为原创，作者刘晓，AI 生成。再生成：`python3 tools/ruwords/gen_logo.py`（同时输出 `tools/ruwords/logo_preview.png` 预览） |

macOS 系统语音的合成结果仅供个人学习使用，未确认可公开再分发；公开发布前应换用许可明确的 TTS 或真人录音。
