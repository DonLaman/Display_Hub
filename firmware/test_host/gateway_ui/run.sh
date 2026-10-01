#!/bin/sh
# Prova sul PC di Impostazioni > Gateway Bluetooth (LVGL vero, DhSettings.h vero, finto backend).
# Uso: LVGL_DIR=/percorso/lvgl-8.x ./run.sh   (riusa gli oggetti LVGL di ../button_grid se gia' compilati)
set -e
cd "$(dirname "$0")"
LVGL_DIR="${LVGL_DIR:-/home/claude/deps/lvgl}"
mkdir -p /tmp/dh_bgobj /tmp/dh_gwui
export CFLAGS="-O0 -w -DLV_CONF_INCLUDE_SIMPLE -I../button_grid -I$LVGL_DIR -I../../lib/dh_fonts/src"
find "$LVGL_DIR/src" -name '*.c' | grep -vE '/(sdl|nxp|stm32_dma2d|swm341_dma2d|arm2d|renesas)/' | xargs -P "$(nproc)" -n1 ../button_grid/cc1.sh
for f in ../../lib/dh_fonts/src/*.c; do gcc $CFLAGS -c "$f" -o "/tmp/dh_gwui/$(basename "$f").o"; done
g++ -std=c++17 -O0 -w -DLV_CONF_INCLUDE_SIMPLE -Ishim -I../button_grid -I"$LVGL_DIR" -I../../lib/dh_settings/src -I../../lib/dh_fonts/src \
    test.cpp stubs.cpp ../../lib/dh_settings/src/DhGatewayUi.cpp /tmp/dh_gwui/*.o /tmp/dh_bgobj/*.o -o /tmp/dh_gwui_test -lm
/tmp/dh_gwui_test
