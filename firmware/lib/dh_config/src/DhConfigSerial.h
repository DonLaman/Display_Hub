/*
 * DhConfigSerial — comandi seriali (JSON per riga) per configurazione e file
 * della microSD, usati dalla web UI via USB. Stessi comandi nel loader e nel
 * firmware principale (in tutte le modalità). Formato in PROTOCOL.md,
 * sezione "Configurazione e microSD via USB".
 *
 * handle() riconosce i comandi file_*, storage_*, config_*: se il comando è
 * suo risponde su "out" (una riga JSON) e ritorna true; altrimenti false e il
 * chiamante lo gestisce come prima (scan, connect, status, ...).
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace DhConfigSerial {

// Dimensione massima di un blocco di file in lettura/scrittura (byte grezzi,
// prima del base64): una riga resta sotto i ~1,2 KB.
constexpr uint32_t kChunk = 768;

bool handle(JsonDocument& cmd, Print& out);

}  // namespace DhConfigSerial
