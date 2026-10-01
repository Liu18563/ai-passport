#!/usr/bin/env bash
# Regenerate ruwords LVGL fonts. Needs lv_font_conv 1.5.2 (npm i -g lv_font_conv@1.5.2)
# and the static Noto Sans SC Regular (OFL) bundled with LVGL after the first build:
#   managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf
# Do not use the variable NotoSansSC[wght].ttf: lv_font_conv renders its thinnest weight.
# Usage: tools/ruwords/gen_fonts.sh [font path]
set -euo pipefail
font="${1:-managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf}"
cd "$(dirname "$0")/../.."
cyr="0x20-0x7E,0x401,0x410-0x44F,0x451"
common=(--bpp 4 --format lvgl --no-compress --lv-include lvgl.h)
lv_font_conv --font "$font" --symbols "$(cat tools/ruwords/glyphs.txt)" --size 20 "${common[@]}" -o assets/fonts/ruw_font_20.c
lv_font_conv --font "$font" --range "$cyr" --size 28 "${common[@]}" -o assets/fonts/ruw_font_28.c
lv_font_conv --font "$font" --range "$cyr" --size 36 "${common[@]}" -o assets/fonts/ruw_font_36.c
# 48px 只收大号"听"、"全部掌握"、数字与"/"。
lv_font_conv --font "$font" --symbols "听全部掌握0123456789/" --size 48 "${common[@]}" -o assets/fonts/ruw_font_48.c
