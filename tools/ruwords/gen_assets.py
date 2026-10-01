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
EXAMPLES = os.path.join(ROOT, "tools/ruwords/examples.csv")
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

GENDER_CODE = {"m": 0, "f": 1, "n": 2, "pl": 3}
def gender_code(g):
    return GENDER_CODE.get(g, 255)
OUT_C = os.path.join(ROOT, "main/ruwords_data.c")
OUT_AUDIO = os.path.join(ROOT, "assets/music/ruwords_voice.adpcm")
OUT_GLYPHS = os.path.join(ROOT, "tools/ruwords/glyphs.txt")
RATE = 12000
# TTS 先合成并处理到 CACHE_RATE 再按 RATE 重采样;缓存到磁盘后改采样率不必重新 say。
CACHE_RATE = 24000
CACHE_DIR = "/tmp/openrussian/tts_cache"

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


PROCESS_AF = ("silenceremove=start_periods=1:start_threshold=-45dB,"
              "areverse,silenceremove=start_periods=1:start_threshold=-45dB,"
              "areverse,acompressor=threshold=-24dB:ratio=4:makeup=8dB,"
              "loudnorm=I=-9:TP=-0.5:LRA=5,alimiter=limit=0.97")


def tts_cached(text):
    """TTS + 音频处理，返回 CACHE_RATE mono PCM，按原文缓存到磁盘。"""
    import hashlib
    key = hashlib.md5(("Milena:150:" + text).encode("utf-8")).hexdigest()
    cpath = os.path.join(CACHE_DIR, key + ".s16")
    pcm = array.array("h")
    if os.path.exists(cpath):
        with open(cpath, "rb") as f:
            pcm.frombytes(f.read())
        return pcm
    with tempfile.TemporaryDirectory() as tmp:
        aiff = os.path.join(tmp, "w.aiff")
        raw = os.path.join(tmp, "w.raw")
        subprocess.run(["say", "-v", "Milena", "-r", "150", "-o", aiff, text], check=True)
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", aiff,
                        "-af", PROCESS_AF,
                        "-ar", str(CACHE_RATE), "-ac", "1", "-f", "s16le", raw], check=True)
        with open(raw, "rb") as f:
            pcm.frombytes(f.read())
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(cpath, "wb") as f:
        f.write(pcm.tobytes())
    return pcm


def tts(text, tmp):
    pcm_hi = tts_cached(text)
    pcm = array.array("h")
    if RATE == CACHE_RATE:
        pcm = array.array("h", pcm_hi)
    else:
        with tempfile.TemporaryDirectory() as d:
            inp = os.path.join(d, "in.s16")
            out = os.path.join(d, "out.s16")
            with open(inp, "wb") as f:
                f.write(pcm_hi.tobytes())
            subprocess.run(["ffmpeg", "-loglevel", "error", "-y",
                            "-f", "s16le", "-ar", str(CACHE_RATE), "-ac", "1", "-i", inp,
                            "-ar", str(RATE), "-f", "s16le", out], check=True)
            with open(out, "rb") as f:
                pcm.frombytes(f.read())
    # 60 ms tail so the codec does not clip the last phoneme.
    pcm.extend([0] * (RATE * 60 // 1000))
    return pcm


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def load_examples(words):
    """读 examples.csv,按 idx 与 words.tsv 逐行对应;校验 word 列完全一致。"""
    import csv
    with open(EXAMPLES, encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(f))
    if len(rows) != len(words):
        sys.exit(f"examples.csv has {len(rows)} rows, words.tsv has {len(words)}")
    for i, (row, (ru, _, _)) in enumerate(zip(rows, words)):
        if row["word"] != ru:
            sys.exit(f"examples.csv row {i} (idx {row.get('idx')}) word mismatch: "
                     f"{row['word']!r} vs words.tsv {ru!r}")
    return [(r["example_ru"], r["example_zh"]) for r in rows]


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
    for (ru, _, _), (marked, _, _, _) in zip(words, details):
        check_marked(ru, marked)
    if len(words) > 1600:
        sys.exit("word list too large for the NVS progress blob")

    examples = load_examples(words)

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
        for (ru, zh, cat, off, n), (marked, pos, gender, full), (ex_ru, ex_zh) in \
                zip(entries, details, examples):
            f.write(f"    {{ {c_str(ru)}, {c_str(recolor(marked))}, {c_str(translit(marked))},\n"
                    f"      {c_str(zh)}, {c_str(pos)}, {c_str(full)}, {cats.index(cat)}, {gender_code(gender)},\n"
                    f"      {off}u, {n}u, {c_str(ex_ru)}, {c_str(ex_zh)} }},\n")
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
    for (ru, zh, cat, _, _), (marked, pos, gender, full), (ex_ru, ex_zh) in \
            zip(entries, details, examples):
        chars.update(ru + ru.upper() + zh + cat + pos + full + translit(marked))
        chars.update(ex_ru + ex_zh)
    chars.update(chr(c) for c in range(0x20, 0x7F))
    chars.discard("\n")
    with open(OUT_GLYPHS, "w", encoding="utf-8") as f:
        f.write("".join(sorted(chars)))

    secs = sum(e[4] for e in entries) / RATE
    print(f"words={len(entries)} audio={len(blob) / 1024:.0f} KiB ({secs:.0f} s) glyphs={len(chars)}")


if __name__ == "__main__":
    main()
