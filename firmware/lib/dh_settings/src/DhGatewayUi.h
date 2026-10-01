/*
 * DhGatewayUi — Impostazioni > Gateway Bluetooth.
 *
 *   elenco dei gateway ──> DETTAGLIO (info, dispositivi conosciuti) ─┬─> CERCA DISPOSITIVI (scansione, associa)
 *                                                                    └─> DISPOSITIVO (connetti/disconnetti, dissocia)
 *
 * Usa solo gli helper pubblici di DhSettings e l'interfaccia GatewayBackend: i dati arrivano
 * dall'applicazione. Le schermate si creano al volo e si eliminano quando si esce (RAM).
 * Provata sul PC con LVGL: firmware/test_host/gateway_ui/run.sh.
 */
#pragma once
#include "DhSettings.h"

namespace DhGatewayUi {
void begin(DhSettings::GatewayBackend* backend);   // chiamata da DhSettings::begin
void open();                                        // dalla riga "Gateway Bluetooth"
bool isOpen();                                      // una schermata del gateway e' attiva
}  // namespace DhGatewayUi
