#!/bin/sh
# Anteprime 480x480: telecomando TV (comandi + tastierino) e player Bluetooth (stato reale, simulato con un brano, fonti) con LVGL sul PC e lo stesso
# codice/dati del firmware. Esce in /tmp/telecomando_anteprima.png. Richiede Pillow.
# Uso: LVGL_DIR=/percorso/lvgl-8.x ./render.sh   (prima volta: compila LVGL, qualche minuto)
set -e
cd "$(dirname "$0")"
LVGL_DIR="${LVGL_DIR:-/home/claude/deps/lvgl}"
mkdir -p /tmp/dh_bgobj
export CFLAGS="-O0 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I/tmp -I$LVGL_DIR -I../../src -I../../lib/dh_icons/src -I../../lib/dh_fonts/src"
find "$LVGL_DIR/src" -name '*.c' | grep -vE '/(sdl|nxp|stm32_dma2d|swm341_dma2d|arm2d|renesas)/' | xargs -P "$(nproc)" -n1 ./cc1.sh
python3 gen_layout.py
gcc $CFLAGS render.c ../../lib/dh_icons/src/dh_icons.c ../../lib/dh_fonts/src/*.c /tmp/dh_bgobj/*.o -o /tmp/dh_render -lm
/tmp/dh_render
python3 - << 'PY'
from PIL import Image
def sheet(files, out):
    s = Image.new("RGB", (480 * len(files) + 20 * (len(files) - 1), 480), (60, 60, 60))
    for i, f in enumerate(files):
        s.paste(Image.open(f), (i * 500, 0))
    s.save(out); print("anteprima:", out)
sheet(["/tmp/view_main.ppm", "/tmp/view_keypad.ppm"], "/tmp/telecomando_anteprima.png")
sheet(["/tmp/view_bt_main.ppm", "/tmp/view_bt_playing.ppm", "/tmp/view_bt_sources.ppm"], "/tmp/player_anteprima.png")
PY
