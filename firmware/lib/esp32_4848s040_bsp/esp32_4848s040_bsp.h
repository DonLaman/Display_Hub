// BSP per il modulo ESP32-S3 "ESP32-4848S040" (480x480, RGB panel ST7701,
// touch GT911, SD condivisa via SPI, audio I2S) — gli stessi pin usati dal
// progetto OraQuadraNano di riferimento (SurvivalHacking / Davide Gatti),
// che gira esattamente su questo modulo.
//
// display_init() e touch_init() sono le funzioni che src/main.cpp dichiara
// come extern: qui le implementiamo collegando Arduino_GFX (per il pannello)
// e TAMC_GT911 (per il touch) a LVGL, così main.cpp può restare scritto in
// puro LVGL senza sapere nulla dei dettagli del pannello.
//
// sd_init() e le funzioni audio sono un'estensione facoltativa, non richieste
// da main.cpp: usale solo se ti serve leggere/scrivere sulla microSD o
// riprodurre audio. L'audio richiede una modifica hardware (vedi commento
// sui pin qui sotto) — non è plug-and-play come display/touch/SD.
#pragma once

#include <Arduino.h>

// ---------- Display / Touch (richiesti da main.cpp) ----------
void display_init();
void touch_init();

// ---------- Swipe e doppio tap (rilevati dalle coordinate grezze) ----------
// Non usiamo il meccanismo LV_EVENT_GESTURE di LVGL per la direzione dello
// swipe: in pratica non distingueva in modo affidabile sinistra da destra.
// Qui invece teniamo traccia noi stessi di dove il dito tocca e rilascia lo
// schermo, calcolando la direzione dalla differenza di coordinata X.
//
// touch_swipe_cb_t: direction è -1 se il dito si è mosso da destra verso
// sinistra (swipe "avanti", pagina successiva), +1 se da sinistra verso
// destra (swipe "indietro", pagina precedente).
typedef void (*touch_swipe_cb_t)(int direction);
typedef void (*touch_double_tap_cb_t)();

void register_touch_swipe_callback(touch_swipe_cb_t cb);
void register_touch_double_tap_callback(touch_double_tap_cb_t cb);

// ---------- SD card (opzionale) ----------
// Condivide il bus SPI col display (MOSI/CLK in comune, CS dedicato).
// Il progetto di riferimento nota esplicitamente di inizializzarla DOPO
// WiFi/Audio per evitare conflitti sul bus: rispetta lo stesso ordine.
// Pin della microSD (esposti per chi gestisce la scheda da sé, es. lib/dh_config).
#define BSP_SD_CS    42
#define BSP_SD_MOSI  47   // condiviso col display
#define BSP_SD_CLK   48   // condiviso col display
#define BSP_SD_MISO  41

// Monta la microSD con la libreria SD in /sd. NON usarla nelle build con la
// configurazione su microSD: lì la scheda è gestita da lib/dh_config (due
// montaggi dello stesso filesystem lo rovinerebbero).
bool sd_init();

// ---------- Audio I2S (opzionale, richiede modifica hardware) ----------
// Il modulo ESP32-4848S040 di serie non ha un amplificatore I2S collegato:
// il progetto di riferimento pilota un amplificatore esterno (es. MAX98357A)
// sui pin qui sotto, che vanno cablati a mano sui pad/GPIO liberi del modulo.
// Se non hai fatto questa modifica, non chiamare audio_init().
#define BSP_I2S_BCLK       1
#define BSP_I2S_LRC        2
#define BSP_I2S_DOUT       40
#define BSP_I2S_PIN_ENABLE 9   // abilita l'amplificatore esterno; -1 se non cablato

void audio_init();
// Riproduce un tono sinusoidale semplice (beep), utile per un test rapido
// del cablaggio prima di integrare una vera libreria di decodifica MP3.
void audio_beep(uint16_t frequency_hz, uint16_t duration_ms);
