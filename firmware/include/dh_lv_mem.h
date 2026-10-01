/*
 * Allocatore di LVGL in PSRAM (lv_conf.h: LV_MEM_CUSTOM).
 *
 * Senza, LVGL tiene un blocco FISSO da LV_MEM_SIZE (64 KB) nella RAM interna,
 * la più scarsa: con WiFi, VPN e Bluetooth accesi restavano ~30 KB liberi.
 * Oggetti, stili e testi di LVGL stanno bene in PSRAM (8 MB); il buffer di
 * disegno è già lì (BSP). Se la PSRAM mancasse si ripiega sulla RAM interna.
 */
#pragma once
#include <stddef.h>

#include "esp_heap_caps.h"

static inline void* dh_lv_malloc(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

static inline void* dh_lv_realloc(void* ptr, size_t size) {
    void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
}

static inline void dh_lv_free(void* ptr) { heap_caps_free(ptr); }
