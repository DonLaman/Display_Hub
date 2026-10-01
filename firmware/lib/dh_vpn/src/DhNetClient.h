/*
 * DhNetClient — WiFiClient che sceglie da solo la strada: diretta sul WiFi
 * oppure dentro la VPN Tailscale, anche verso indirizzi 192.168.x.x pubblicati
 * da un subnet router (vedi DhVpn::pathFor per le regole).
 *
 * Si usa ovunque si userebbe un WiFiClient, per esempio con HTTPClient:
 *
 *   DhNetClient client;
 *   HTTPClient http;
 *   http.begin(client, "http://192.168.1.40:12000/api/esp/poll?...");
 *
 * Come funziona: quando la destinazione va raggiunta via VPN, il socket viene
 * legato (bind) all'IP 100.x del display PRIMA della connect. Il lwIP del core
 * ha il routing per sorgente (ip4_route_src_hook): un socket legato all'IP
 * dell'interfaccia WireGuard esce da lì qualunque sia la destinazione, e parte
 * già con mittente 100.x, come si aspetta il subnet router. Il tunnel verso il
 * dispositivo viene prima svegliato e atteso (fino al timeout di connessione).
 * Negli altri casi è identico a WiFiClient.
 */
#pragma once
#include <WiFiClient.h>

#include "DhVpn.h"

class DhNetClient : public WiFiClient {
public:
    // HTTPClient chiama connect(host, porta, timeout): WiFiClient risolve il
    // nome e richiama questa (virtuale), quindi la scelta avviene anche per i nomi.
    int connect(IPAddress ip, uint16_t port, int32_t timeout_ms) override;
    using WiFiClient::connect;

    // Strada usata dall'ultima connect() (per log e interfaccia).
    const DhVpn::Path& lastPath() const { return _path; }

private:
    DhVpn::Path _path;
};
