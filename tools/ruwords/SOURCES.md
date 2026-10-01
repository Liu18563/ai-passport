<p align="right">
  <a href="SOURCES.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Vocabulary Data Sources (Russian A2, ~1300 words)

## Sources

- Headwords, stress, part of speech, gender and English glosses come from the
  **OpenRussian** project:
  - Repository: https://github.com/Badestrand/russian-dictionary
  - License: **CC BY-SA 4.0** (Creative Commons Attribution-ShareAlike 4.0 International)
  - Files used: `nouns.csv`, `verbs.csv`, `adjectives.csv`, `others.csv`
  - Export/generation date: 2026-10-01

- Per the original plan, the OpenRussian TogetherDB online database was the
  first choice for filtering A1+A2 by level. It requires login and cannot be
  exported anonymously, so the repository CSVs above were used instead.
  High-frequency words were taken in file order (an approximation of word
  frequency) to reach about 1300 words.

## Composition

- **1300 words** in total: 649 nouns, 286 verbs (including a few function words
  classified as verbs in OpenRussian), 196 adjectives (including determiners and
  possessive forms classified as adjectives in OpenRussian), and 169 others.
  In OpenRussian `nouns.csv`, рада ("glad") is listed as a noun, but it is
  actually the feminine form of the adjective рад; it was moved to the
  adjective class, giving 649 nouns and 196 adjectives.
- Stress: the OpenRussian `accented` field is used throughout, with `'` placed
  after the stressed vowel. Monosyllabic words and words containing ё follow
  the OpenRussian convention of carrying no stress mark.
- A small number of missing stress marks, misaligned homographs and absent
  gender fields are corrected by the FORCE / GENDER_MANUAL tables in the
  vocabulary build script (selection/classification stage), and each fix is
  recorded in `review.tsv`.

## Chinese Glosses

- **All Chinese glosses are machine/AI generated and not human proofread**;
  they are for study reference only. The short glosses (used for options) are
  unique within the word list; full glosses are in `details.tsv`.
- If a translation seems wrong, the OpenRussian source and authoritative
  dictionaries take precedence.

## Example Sentences

- Each word carries one example sentence (`tools/ruwords/examples.csv`, column
  `example_ru` + `example_zh`), shown on page 2 of the answer card. Sentences are
  text only — no audio is generated for them.
- Three source kinds (CSV `source` column):
  - **`tatoeba-cmn`** — Russian and Chinese both come from directly linked Tatoeba
    sentences; the Chinese is contributed by Tatoeba users. Traditional characters
    were converted to Simplified deterministically (OpenCC `t2s`), a formatting change.
  - **`tatoeba-eng-mt`** — Russian from Tatoeba, English from a directly linked Tatoeba
    sentence; the Chinese here is an AI machine translation of the English, **not** a
    Tatoeba human Chinese translation.
  - **`generated`** — both Russian and sentence Chinese written by us, no Tatoeba ID.
- Tatoeba sentences are licensed under **CC BY 2.0 FR**
  (https://creativecommons.org/licenses/by/2.0/fr/deed.en). When redistributing, credit
  **Tatoeba and the individual sentence contributors** with the license and source links;
  the CSV `tatoeba_ru_id` / `tatoeba_zh_id` form
  `https://tatoeba.org/en/sentences/show/ID` to look up each sentence and its author.

## Attribution and Derivative Work

- This vocabulary is a derivative work of OpenRussian (CC BY-SA 4.0) and is
  released under the same CC BY-SA 4.0 license. When redistributing, keep this
  source notice and credit the OpenRussian contributors.
- Audio is synthesized with macOS `say -v Milena` (ru_RU) and processed with
  ffmpeg, sampled at 12 kHz and encoded as IMA-ADPCM. At 16 kHz the 1300 words
  would exceed the 7.8 MB merged-image limit, so the rate was lowered to 12 kHz
  as provided for in the work order. Audio copyright follows the terms of the
  respective synthesis engine.
