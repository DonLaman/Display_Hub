/*
 * DhWifi — più reti WiFi per Display Hub.
 *
 * Le reti note sono l'elenco [wifi] network della configurazione (DhConfig):
 *   network = CasaWiFi | password
 * L'ordine dell'elenco è la priorità a parità di segnale.
 *
 * Macchina a stati NON bloccante (loop() dal loop dell'applicazione):
 *  - connessi: controllo; se la rete cade, 3 s di attesa e poi un "giro";
 *  - giro: scansione (asincrona) -> reti note visibili dalla più forte (a parità
 *    entro 3 dB vince la priorità) -> tentativo su ciascuna (max 12 s);
 *    un giro sì e uno no si provano anche le reti note non visibili (nascoste);
 *  - giro fallito: nuovo giro dopo 10 s (roundsFailed() conta quelli di fila).
 * Il passaggio casa -> hotspot avviene così da solo: la rete di casa sparisce,
 * l'hotspot è visibile, il giro successivo lo sceglie.
 *
 * Il driver WiFi non ricorda nulla (persistent(false)) e non si riconnette da
 * solo (autoReconnect off): decide tutto questa macchina a stati.
 */
#pragma once
#include <Arduino.h>

#include <vector>

namespace DhWifi {

struct Net {
    String ssid;
    String password;
};

std::vector<Net> known();                                         // in ordine di priorità
void add(const String& ssid, const String& password, bool first); // aggiunge o aggiorna
void remove(const String& ssid);
void move(const String& ssid, int delta);                         // -1 su, +1 giù

enum class State : uint8_t { Stopped, Connected, Waiting, Scanning, Connecting };

void begin();              // modalità STA e avvio (se già connessi: Connected)
void stop();               // spegne la radio
// Per operazioni manuali (scansione dall'interfaccia, prova di una rete dal
// loader): pause() ferma le decisioni, interrompe una scansione in corso e un
// tentativo di connessione ancora aperto (il driver rifiuta una scansione mentre
// si sta agganciando), ma lascia stare una connessione già stabilita.
// resume() riprende SOLO se era in pausa (non riaccende un WiFi spento).
void pause();
void resume();
void loop();
void reconnectNow();       // nuovo giro subito
void connectTo(const String& ssid);  // nuovo giro provando prima questa rete

State state();
String stateText();
uint32_t roundsFailed();
uint32_t disconnectedMs(); // da quanto si è senza rete (0 se connessi o fermi)

}  // namespace DhWifi
