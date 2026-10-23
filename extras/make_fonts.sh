#!/usr/bin/env bash
# Regenerates jira_status/vazir_{14,16,20}.c from Vazirmatn (OFL, see Vazirmatn-OFL.txt).
# Ranges: ASCII, Persian/Arabic letters and digits, and the presentation forms LVGL's
# LV_USE_ARABIC_PERSIAN_CHARS shapes text into.
set -euo pipefail
cd "$(dirname "$0")"
work=$(mktemp -d)
(cd "$work" && npm pack vazirmatn@33.0.3 --silent >/dev/null && tar xzf vazirmatn-*.tgz)
ranges=0x20-0x7E,0xAB,0xB7,0xBB,0x200C,0x2013,0x2026,0x060C,0x061B,0x061F,0x0621-0x063A,0x0640-0x065F,0x066A-0x066C,0x0670,0x067E,0x0686,0x0698,0x06A9,0x06AF,0x06CC,0x06F0-0x06F9,0xFB56-0xFB59,0xFB7A-0xFB7D,0xFB8A-0xFB8B,0xFB8E-0xFB95,0xFBFC-0xFBFF,0xFE70-0xFEFC
for s in 14 16 20; do
  npx -y lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size "$s" --format lvgl \
    --font "$work/package/fonts/ttf/Vazirmatn-Medium.ttf" -r "$ranges" \
    --lv-include lvgl.h --lv-fallback "lv_font_montserrat_$s" -o "../jira_status/vazir_$s.c"
done
rm -rf "$work"
