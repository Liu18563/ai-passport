<p align="right">
  <strong>简体中文</strong> · <a href="SOURCES.md">English</a>
</p>

# 词库数据来源（俄语 A2 约 1300 词）

## 数据来源

- 词目、重音、词性、性（gender）、英文释义均取自 **OpenRussian** 项目：
  - 仓库：https://github.com/Badestrand/russian-dictionary
  - 许可：**CC BY-SA 4.0**（Creative Commons Attribution-ShareAlike 4.0 International）
  - 使用文件：`nouns.csv`、`verbs.csv`、`adjectives.csv`、`others.csv`
  - 导出/生成时间：2026-10-01

- 按施工单原计划，首选 OpenRussian TogetherDB 在线库按 level 筛选 A1+A2；
  实测 TogetherDB 需登录、无法匿名导出，故改用上述仓库 CSV，
  按各文件内的顺序（近似词频）取高频词，凑足约 1300 词。

## 构成

- 共 **1300 词**：名词 649、动词 286（含个别在 OR 中归入 verbs 的功能词）、
  形容词 196（含在 OR 中归入 adjectives 的限定/物主代词等）、其它 169。
  其中 рада（"高兴"）在 OR nouns.csv 中被归为名词，实为形容词 рад 的阴性
  形式，已改归形容词类，故名词为 649、形容词为 196。
- 重音：统一采用 OpenRussian `accented` 字段，以 `'` 标在重读元音之后；
  单音节词、含 ё 的词按 OpenRussian 惯例不标重音。
- 少数 OpenRussian 漏标、同形词错位、gender 字段缺失的情况，
  由词库构建脚本（选词/分类阶段）中的 FORCE / GENDER_MANUAL 表修正，
  并逐条登记在 `review.tsv`。

## 中文释义声明

- **所有中文释义为机器翻译/AI 生成，未经人工校对**，仅供学习参考；
  短释义（选项用）在词表内唯一，完整释义见 `details.tsv`。
- 如发现译法有误，以 OpenRussian 原文及权威词典为准。

## 例句来源

- 每个词配一句例句（`tools/ruwords/examples.csv` 的 `example_ru` + `example_zh` 列），
  显示在作答卡第 2 页；例句只存文字，不做发音。
- 三种来源（CSV `source` 列）：
  - **`tatoeba-cmn`**：俄文、中文均来自 Tatoeba 直接互链句，中文由 Tatoeba 贡献者提供；
    繁体译文经 OpenCC `t2s` 确定性转为简体，属格式改动。
  - **`tatoeba-eng-mt`**：俄文来自 Tatoeba，英文来自其直接互链句；CSV 中的中文由
    AI 根据英文译写，属**机器翻译**，并非 Tatoeba 的人工中文译文。
  - **`generated`**：俄文例句与中文译文为自造，无 Tatoeba ID。
- Tatoeba 句子许可以 **CC BY 2.0 FR**（https://creativecommons.org/licenses/by/2.0/fr/deed.zh-hans）
  发布。再分发时须署名 **Tatoeba 及对应句子的贡献者**、注明许可与来源链接；
  CSV 的 `tatoeba_ru_id`、`tatoeba_zh_id` 可拼成
  `https://tatoeba.org/en/sentences/show/ID` 查阅句子及贡献者。

## 署名与衍生

- 本词库是 OpenRussian（CC BY-SA 4.0）的衍生作品，依该许可同样以
  CC BY-SA 4.0 发布；再分发时须保留本来源说明并署名 OpenRussian 贡献者。
- 语音由 macOS `say -v Milena`（ru_RU）合成，经 ffmpeg 处理，
  以 12 kHz 采样、编码为 IMA-ADPCM（1300 词 16 kHz 会超出 7.8MB 合并镜像
  上限，按施工单预案降为 12 kHz）；语音版权依其各自合成引擎条款。
