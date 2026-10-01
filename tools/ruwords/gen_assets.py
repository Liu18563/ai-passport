#!/usr/bin/env python3
"""Generate Russian vocabulary assets for the ruwords app.

Inputs : tools/ruwords/words.tsv
Outputs: main/ruwords_data.c       word table (UTF-8 strings + audio offsets)
         assets/music/ruwords_voice.adpcm   IMA-ADPCM 4-bit, 16 kHz mono
         tools/ruwords/glyphs.txt  every character the UI needs (for lv_font_conv)

Voice: macOS `say -v Milena` (ru_RU), resampled with ffmpeg.
Usage: python3 tools/ruwords/gen_assets.py
"""
import array
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TSV = os.path.join(ROOT, "tools/ruwords/words.tsv")
DETAILS = os.path.join(ROOT, "tools/ruwords/details.tsv")
STRESS_COLOR = "C8102E"  # 与 main/ruwords_app.c 的 COL_ACCENT 一致

LATIN = {
    "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e", "ё": "yo", "ж": "zh",
    "з": "z", "и": "i", "й": "y", "к": "k", "л": "l", "м": "m", "н": "n", "о": "o",
    "п": "p", "р": "r", "с": "s", "т": "t", "у": "u", "ф": "f", "х": "kh", "ц": "ts",
    "ч": "ch", "ш": "sh", "щ": "shch", "ъ": "", "ы": "y", "ь": "'", "э": "e",
    "ю": "yu", "я": "ya",
}
ACUTE = {"a": "á", "e": "é", "i": "í", "o": "ó", "u": "ú", "y": "ý"}
VOWELS = "аеёиоуыэюя"


def translit(marked):
    """Latin reading with an acute on the stressed vowel: приве'т -> privét."""
    out = []
    for i, ch in enumerate(marked):
        if ch == "'":
            continue
        low = ch.lower()
        lat = LATIN.get(low, ch)
        if i + 1 < len(marked) and marked[i + 1] == "'":
            # 只给拉丁读音里的最后一个元音字母加重音:ya -> yá, yo -> yó。
            for j in range(len(lat) - 1, -1, -1):
                if lat[j] in ACUTE:
                    lat = lat[:j] + ACUTE[lat[j]] + lat[j + 1:]
                    break
        if ch != low and lat:
            lat = lat[0].upper() + lat[1:]
        out.append(lat)
    return "".join(out)


def recolor(marked):
    """LVGL recolor markup: the stressed vowel in accent color."""
    out = []
    for i, ch in enumerate(marked):
        if ch == "'":
            continue
        if i + 1 < len(marked) and marked[i + 1] == "'":
            out.append(f"#{STRESS_COLOR} {ch}#")
        else:
            out.append(ch)
    return "".join(out)


def check_marked(ru, marked):
    plain = marked.replace("'", "")
    if plain != ru:
        sys.exit(f"details.tsv mismatch: {ru!r} vs {marked!r}")
    for k, ch in enumerate(marked):
        if ch == "'" and (k == 0 or marked[k - 1].lower() not in VOWELS):
            sys.exit(f"stress mark not after a vowel: {marked!r}")
    if "#" in marked:
        sys.exit(f"'#' conflicts with LVGL recolor: {marked!r}")
OUT_C = os.path.join(ROOT, "main/ruwords_data.c")
OUT_AUDIO = os.path.join(ROOT, "assets/music/ruwords_voice.adpcm")
OUT_GLYPHS = os.path.join(ROOT, "tools/ruwords/glyphs.txt")
RATE = 16000

APP_SRC = os.path.join(ROOT, "main/ruwords_app.c")
# 省略号由 LV_LABEL_LONG_DOT 截断时使用。
EXTRA_TEXT = "…"


def ui_text():
    """Every string literal in the app source, so new UI text cannot miss glyphs."""
    import re
    with open(APP_SRC, encoding="utf-8") as f:
        src = f.read()
    src = re.sub(r"//[^\n]*", "", src)
    return "".join(re.findall(r'"((?:[^"\\\n]|\\.)*)"', src)) + EXTRA_TEXT

STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
        45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209,
        230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876,
        963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
        10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
        29794, 32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(samples):
    """IMA-ADPCM; each word restarts from predictor 0 / index 0 (decoder does the same)."""
    pred, idx = 0, 0
    nibbles = []
    for s in samples:
        step = STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        delta = step >> 3
        if diff >= step:
            code |= 4; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 2; diff -= step; delta += step
        step >>= 1
        if diff >= step:
            code |= 1; delta += step
        pred = pred - delta if code & 8 else pred + delta
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX[code & 7]))
        nibbles.append(code)
    if len(nibbles) % 2:
        nibbles.append(0)
    return bytes((nibbles[i] | (nibbles[i + 1] << 4)) for i in range(0, len(nibbles), 2))


def tts(text, tmp):
    aiff = os.path.join(tmp, "w.aiff")
    raw = os.path.join(tmp, "w.raw")
    subprocess.run(["say", "-v", "Milena", "-r", "150", "-o", aiff, text], check=True)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", aiff,
                    "-af", "silenceremove=start_periods=1:start_threshold=-45dB,"
                           "areverse,silenceremove=start_periods=1:start_threshold=-45dB,"
                           "areverse,acompressor=threshold=-24dB:ratio=4:makeup=8dB,"
                           "loudnorm=I=-9:TP=-0.5:LRA=5,alimiter=limit=0.97",
                    "-ar", str(RATE), "-ac", "1", "-f", "s16le", raw], check=True)
    pcm = array.array("h")
    with open(raw, "rb") as f:
        pcm.frombytes(f.read())
    # 60 ms tail so the codec does not clip the last phoneme.
    pcm.extend([0] * (RATE * 60 // 1000))
    return pcm


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    words = []
    with open(TSV, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            ru, zh, cat = line.split("\t")
            words.append((ru, zh, cat))
    details = []
    with open(DETAILS, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if line and not line.startswith("#"):
                details.append(line.split("\t"))
    if len(details) != len(words):
        sys.exit(f"details.tsv has {len(details)} rows, words.tsv has {len(words)}")
    for (ru, _, _), (marked, _, _) in zip(words, details):
        check_marked(ru, marked)
    if len(words) > 400:
        sys.exit("word list too large for the NVS progress blob")

    blob = bytearray()
    entries = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (ru, zh, cat) in enumerate(words):
            pcm = tts(ru, tmp)
            data = adpcm_encode(pcm)
            entries.append((ru, zh, cat, len(blob), len(pcm)))
            blob += data
            print(f"\r{i + 1}/{len(words)} {ru:<20}", end="", flush=True)
    print()

    os.makedirs(os.path.dirname(OUT_AUDIO), exist_ok=True)
    with open(OUT_AUDIO, "wb") as f:
        f.write(blob)

    cats = []
    for _, _, c, _, _ in entries:
        if c not in cats:
            cats.append(c)
    with open(OUT_C, "w", encoding="utf-8") as f:
        f.write("// Generated by tools/ruwords/gen_assets.py — do not edit.\n")
        f.write('#include "ruwords_data.h"\n\n')
        f.write("const ruw_word_t RUW_WORDS[RUW_WORD_COUNT] = {\n")
        for (ru, zh, cat, off, n), (marked, pos, full) in zip(entries, details):
            f.write(f"    {{ {c_str(ru)}, {c_str(recolor(marked))}, {c_str(translit(marked))},\n"
                    f"      {c_str(zh)}, {c_str(pos)}, {c_str(full)}, {cats.index(cat)}, {off}u, {n}u }},\n")
        f.write("};\n\n")
        f.write("const char *const RUW_CATEGORIES[] = {\n")
        for c in cats:
            f.write(f"    {c_str(c)},\n")
        f.write("};\n")

    with open(os.path.join(ROOT, "main/ruwords_count.h"), "w") as f:
        f.write("// Generated by tools/ruwords/gen_assets.py — do not edit.\n#pragma once\n")
        f.write(f"#define RUW_WORD_COUNT {len(entries)}\n")
        f.write(f"#define RUW_CATEGORY_COUNT {len(cats)}\n")

    chars = set(ui_text())
    for (ru, zh, cat, _, _), (marked, pos, full) in zip(entries, details):
        chars.update(ru + ru.upper() + zh + cat + pos + full + translit(marked))
    chars.update(chr(c) for c in range(0x20, 0x7F))
    chars.discard("\n")
    with open(OUT_GLYPHS, "w", encoding="utf-8") as f:
        f.write("".join(sorted(chars)))

    secs = sum(e[4] for e in entries) / RATE
    print(f"words={len(entries)} audio={len(blob) / 1024:.0f} KiB ({secs:.0f} s) glyphs={len(chars)}")


if __name__ == "__main__":
    main()
