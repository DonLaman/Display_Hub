#!/bin/sh
# compila un sorgente di LVGL in /tmp/dh_bgobj (salta se gia' fatto); CFLAGS dall'ambiente
o=/tmp/dh_bgobj/$(echo "$1" | md5sum | cut -c1-16).o
[ -f "$o" ] || gcc $CFLAGS -c "$1" -o "$o"
