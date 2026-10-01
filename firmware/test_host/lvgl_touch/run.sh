#!/bin/sh
# Simulazione sul PC: un dito virtuale tocca il centro di una pagina e
# trascina; lo schermo (dove vivono swipe e doppio tap) deve ricevere gli
# eventi. Riproduce il problema "solo le fasce in alto e in basso reagiscono"
# e verifica la correzione (contenitori non cliccabili / EVENT_BUBBLE).
# Uso: LVGL_DIR=/percorso/di/lvgl-8.3 ./run.sh
set -e
cd "$(dirname "$0")"
: "${LVGL_DIR:?imposta LVGL_DIR alla cartella di LVGL 8.3/8.4}"
SRC=$(find "$LVGL_DIR/src" -name "*.c" | grep -vE "/(sdl|nxp|stm32_dma2d|swm341_dma2d|arm2d|renesas)/")
gcc -O0 -w -DLV_CONF_INCLUDE_SIMPLE -I. -I"$LVGL_DIR" touch.c $SRC -o /tmp/dh_touch_test -lm
/tmp/dh_touch_test
