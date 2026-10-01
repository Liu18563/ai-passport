<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Assets

This directory stores reusable fonts, images, music, and sound effects, organized by asset type.

Keep each asset in the matching subdirectory and document its destination, naming, integration method, and source/license. Do not mix binary assets with Markdown documentation.

## Fonts

Store reusable font files and generated font sources in `fonts/`.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Images

Store reusable source images and generated display assets in `images/`.

| File | Dimensions and format | Use and source |
| --- | --- | --- |
| [`images/home.jpg`](images/home.jpg) | 3840 × 2160, JPEG | Product hero image embedded in both project README files to foreground AI Passport and its open, maker-oriented identity. |
| [`images/readme-hardware-specs.png`](images/readme-hardware-specs.png) | 2172 × 724, PNG RGBA | Optional technical infographic retained as a reference asset; it is no longer used as the homepage hero. Generated for this repository with the built-in image generation tool on 2026-09-17; the six labels and values were checked against the documented hardware contract. |
| [`images/logo-wordmark.png`](images/logo-wordmark.png) | 1648 × 336, PNG RGBA | Transparent black wordmark extracted from the repository's original `images/logo.png`; embedded in both project README files for light backgrounds. |
| [`images/logo-wordmark-dark.png`](images/logo-wordmark-dark.png) | 1648 × 336, PNG RGBA | White version of the extracted wordmark, used by the README `<picture>` element when GitHub is in dark mode. |

- Use descriptive names and document dimensions, pixel format, conversion steps, and destination.
- Prefer formats suitable for the 240 × 320 RGB565 display and account for Flash and internal RAM.
- Preserve editable sources where licensing permits, and record the source and license.
- Never commit device QR secrets, credentials, or personal data in images.

## Music and sound effects

Store reusable music and sound-effect sources in `music/`.

- Document the source, license, sample rate, bit depth, channels, conversion command, and destination.
- Prefer 16 kHz, 16-bit mono PCM when it matches the current BSP audio path.
- Check Flash and internal-RAM cost before embedding audio; stream or chunk long recordings.
- Do not commit media without redistribution permission.

## Russian Words (feature/russian-words)

| Path | Notes |
| --- | --- |
| `fonts/ruw_font_20.c` / `ruw_font_28.c` / `ruw_font_36.c` | Noto Sans SC (SIL OFL 1.1) converted with lv_font_conv 1.5.2, 4 bpp, uncompressed. 20px covers every character in `tools/ruwords/glyphs.txt`; 28/36px cover ASCII and Cyrillic only. Regenerate: `tools/ruwords/gen_fonts.sh <font path>` |
| `music/ruwords_voice.adpcm` | Russian pronunciation for 301 words, synthesized with macOS `say -v Milena`, silence-trimmed with ffmpeg, resampled to 16 kHz mono and encoded as 4-bit IMA-ADPCM. Regenerate: `python3 tools/ruwords/gen_assets.py` |
| `images/ruw_logo.c` | 88 × 88, RGB565, uncompressed, no alpha. Five frames of the original matryoshka logo: `ruw_logo_r0/r5/r10/r15` (rotated 0/5/10/15° CCW) and `ruw_logo_blink` (15° eyes closed); `ruw_logo` is a legacy alias of `r15`. Source `images/source/matryoshka_source.png` is original artwork by Liu Xiao (AI-generated). Regenerate: `python3 tools/ruwords/gen_logo.py` (also writes `tools/ruwords/logo_preview.png`) |

Output of the macOS system voice is for personal study only and has not been cleared for public redistribution; switch to a clearly licensed TTS or human recordings before publishing.
