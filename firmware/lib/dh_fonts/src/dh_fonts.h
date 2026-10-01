/*
 * Font di Display Hub: Montserrat Medium 14/20/48 CON le lettere accentate.
 *
 * I Montserrat inclusi in LVGL hanno solo ASCII (0x20-0x7F) più le icone:
 * "à è é ì ò ù", "°", "—", "€" comparivano come rettangoli vuoti. Questi sono
 * generati con lo stesso strumento e le stesse sorgenti di LVGL 8.3.11
 * (scripts/built_in_font: lv_font_conv 1.5.2, Montserrat-Medium.ttf,
 * FontAwesome5 per le icone LV_SYMBOL_*), 4 bpp, con in più:
 *   0xA0-0xFF       Latin-1: à è é ì ò ù ç ñ ü ... ° ± « » £ ...
 *   0x2013-0x2014   – —      0x2018-0x201E  ‘ ’ “ ” „     0x2022 •
 *   0x2026 …        0x20AC €
 * 14 e 20 hanno anche le icone di LVGL (stesso elenco dei font originali);
 * 48 (numeri grandi dei widget) solo testo.
 *
 * dh_font_14 è il font predefinito di LVGL (LV_FONT_DEFAULT in lv_conf.h).
 * Rigenerazione: vedi lib/dh_fonts/README.md.
 */
#pragma once
#include <lvgl.h>

LV_FONT_DECLARE(dh_font_14)
LV_FONT_DECLARE(dh_font_20)
LV_FONT_DECLARE(dh_font_48)
