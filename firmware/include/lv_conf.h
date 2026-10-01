/* Configurazione LVGL minima per Display Hub.
 * Se il BSP del venditore fornisce già un proprio lv_conf.h più completo,
 * sostituiscilo con quello (deve restare in firmware/include/lv_conf.h)
 * mantenendo almeno i font usati da src/main.cpp abilitati come sotto. */
#ifndef LV_CONF_H
#define LV_CONF_H

#ifndef LV_COLOR_DEPTH
#define LV_COLOR_DEPTH 16
#endif
#ifndef LV_MEM_SIZE
#define LV_MEM_SIZE (64U * 1024U)   /* usato solo se LV_MEM_CUSTOM è 0 */
#endif

/* Memoria di LVGL in PSRAM invece di un blocco fisso da 64 KB nella RAM
 * interna (vedi dh_lv_mem.h). Richiede che LVGL legga davvero questo file:
 * vedi tools/lv_conf_path.py e il controllo a fine build. */
#ifndef LV_MEM_CUSTOM
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "dh_lv_mem.h"
#define LV_MEM_CUSTOM_ALLOC   dh_lv_malloc
#define LV_MEM_CUSTOM_FREE    dh_lv_free
#define LV_MEM_CUSTOM_REALLOC dh_lv_realloc
#endif

#ifndef LV_FONT_MONTSERRAT_20
#define LV_FONT_MONTSERRAT_20 1
#endif
#ifndef LV_FONT_MONTSERRAT_48
#define LV_FONT_MONTSERRAT_48 1
#endif

/* Font predefinito: Montserrat 14 CON le lettere accentate (lib/dh_fonts).
 * Quello incluso in LVGL ha solo ASCII: "à è ì ò ù" e "°" erano rettangoli. */
#ifndef LV_FONT_CUSTOM_DECLARE
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(dh_font_14) LV_FONT_DECLARE(dh_font_20) LV_FONT_DECLARE(dh_font_48)
#endif
#ifndef LV_FONT_DEFAULT
#define LV_FONT_DEFAULT &dh_font_14
#endif

#ifndef LV_USE_FLEX
#define LV_USE_FLEX 1
#endif

#endif
