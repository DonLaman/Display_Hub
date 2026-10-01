#!/bin/sh
# Prova sul PC (LVGL vero) di geometria, icone e tocchi del telecomando.
# Uso: LVGL_DIR=/percorso/lvgl-8.x ./run.sh   (default: /home/claude/deps/lvgl)
set -e
cd "$(dirname "$0")"
LVGL_DIR="${LVGL_DIR:-/home/claude/deps/lvgl}"
mkdir -p /tmp/dh_bgobj
export CFLAGS="-O0 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I$LVGL_DIR -I../../src -I../../lib/dh_icons/src -I../../lib/dh_fonts/src"
find "$LVGL_DIR/src" -name '*.c' | grep -vE '/(sdl|nxp|stm32_dma2d|swm341_dma2d|arm2d|renesas)/' | xargs -P "$(nproc)" -n1 ./cc1.sh
gcc $CFLAGS test.c ../../lib/dh_icons/src/dh_icons.c ../../lib/dh_fonts/src/*.c /tmp/dh_bgobj/*.o -o /tmp/dh_bg_test -lm
/tmp/dh_bg_test
