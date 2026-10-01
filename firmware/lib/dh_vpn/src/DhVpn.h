/*
 * DhVpn — VPN Tailscale per Display Hub (loader "pesante" e firmware principale).
 *
 * Avvolge MicroLink (lib/microlink) in un'API minima pensata per l'interfaccia:
 * un interruttore (setEnabled), una chiave (setAuthKey) e lo stato da mostrare.
 *
 * Come funziona:
 *  - La VPN NON gestisce il WiFi: usa la connessione che il firmware ha già
 *    aperto con WiFi.begin(), che sia il router di casa o l'hotspot del cellulare.
 *  - Un task di controllo in background ("dhvpn_ctl") confronta ogni 500 ms lo
 *    stato desiderato (attiva + chiave presente + WiFi con IP) con quello reale
 *    e avvia/arresta MicroLink di conseguenza. L'arresto di MicroLink blocca per
 *    ~3 s: avviene in quel task, quindi l'UI (LVGL) non si congela mai.
 *  - Se cambia rete (IP o gateway diversi: es. da casa all'hotspot) chiama
 *    microlink_rebind(): la sessione VPN sopravvive al cambio senza ricominciare.
 *  - Impostazioni persistenti in NVS (namespace "dhvpn"): attiva, chiave, hostname.
 *    L'identità del nodo Tailscale (chiavi macchina) la salva MicroLink stesso in
 *    NVS: il display resta lo stesso dispositivo sulla tailnet anche dopo riavvii
 *    e aggiornamenti firmware.
 *  - Instradamento: il traffico verso 100.64.0.0/10 passa nel tunnel da solo,
 *    anche per HTTPClient/WiFiClient standard. Le subnet pubblicate dai subnet
 *    router (es. 192.168.1.0/24) sono raggiungibili con DhNetClient, che lega la
 *    connessione all'IP 100.x quando serve (vedi pathFor()).
 *
 * Tutte le funzioni sono non bloccanti e sicure da chiamare dal loop()/callback LVGL.
 */
#pragma once
#include <Arduino.h>

namespace DhVpn {

enum class State : uint8_t {
    Off = 0,        // interruttore spento
    NoKey,          // accesa ma manca l'auth key
    WaitWifi,       // accesa, in attesa di WiFi con IP
    Connecting,     // handshake col control plane Tailscale
    Registering,    // registrazione del nodo
    Connected,      // tunnel operativo, IP 100.x assegnato
    Reconnecting,   // persa la connessione, sta ritentando
    Error,          // errore (vedi lastError())
    Stopping,       // arresto in corso (~3 s)
};

struct Peer {
    String hostname;
    String ip;
    bool online = false;
    bool direct = false;   // true = UDP diretto, false = via relay DERP
};

// Da chiamare una volta nel setup(), DOPO Preferences/NVS pronta (dopo Serial.begin va bene).
// default_hostname: nome sulla tailnet se in NVS non ce n'è uno (es. DEVICE_ID).
// build_auth_key:   chiave incorporata dalla build (può essere ""): usata solo se
//                   in NVS non c'è già una chiave salvata.
// build_enabled:    stato iniziale dell'interruttore se in NVS non è mai stato salvato.
void begin(const char* default_hostname, const char* build_auth_key = "", bool build_enabled = false);

void   setEnabled(bool on);
bool   isEnabled();

bool   setAuthKey(const String& key);   // false se il formato non è plausibile
bool   hasAuthKey();
String authKeyMasked();                 // es. "tskey-auth-kX…9Qa" per l'UI
void   clearAuthKey();

void   setHostname(const String& name); // sanificato (a-z 0-9 -), riavvia la VPN se attiva
String hostname();

State       state();
const char* stateText();                // testo italiano per l'UI
String      lastError();
bool        isConnected();
String      vpnIp();                    // "" se non connessa
String      networkIp();                // IP della rete sottostante (WiFi/hotspot)

int    peerCount();
bool   peer(int index, Peer& out);

// Risoluzione MagicDNS (nome breve o FQDN) -> "100.x.y.z", "" se sconosciuto.
String resolve(const String& name);

// --- Tunnel verso un dispositivo della tailnet --------------------------------
// MicroLink NON apre da solo la sessione WireGuard verso un dispositivo finché
// non trova un percorso diretto: sotto hotspot, dove spesso si passa dal relay
// DERP, una connessione "a freddo" verso un 100.x fallisce subito. Prima di
// connettersi bisogna quindi svegliare il dispositivo (handshake via DERP e,
// se noto, diretto + CallMeMaybe) e aspettare che la sessione sia su.
enum class Tunnel : uint8_t {
    NoVpn,    // VPN spenta o non connessa
    NoPeer,   // l'IP non è un dispositivo della tailnet conosciuto
    Waking,   // handshake avviato, sessione non ancora pronta: riprovare tra poco
    Up,       // sessione WireGuard attiva: si può connettere
};
// Non bloccante. Richiamabile a ogni tentativo: l'handshake viene rilanciato
// al massimo ogni 5 s per lo stesso dispositivo.
Tunnel prepareTunnel(const String& vpn_ip);
const char* tunnelText(Tunnel t);

// --- Subnet raggiungibili tramite la VPN (subnet router Tailscale) ------------
// Le subnet che i dispositivi della tailnet pubblicano (es. 192.168.1.0/24
// approvata nella console per il tuo subnet router). Vuoto se VPN spenta.
struct Route {
    String network;      // "192.168.1.0/24"
    String via_name;     // dispositivo che la pubblica, es. "ryzen"
    String via_ip;       // suo IP 100.x
    bool via_online = false;
};
int routes(Route* out, int max);

// Come raggiungere una destinazione IPv4. Regole, nell'ordine:
//  1. VPN non connessa                          -> diretto (WiFi)
//  2. destinazione nella rete del WiFi attuale  -> diretto (a casa vince la LAN)
//  3. destinazione 100.64.0.0/10 (tailnet)      -> tunnel verso quel dispositivo
//  4. destinazione in una subnet pubblicata     -> tunnel verso chi la pubblica
//  5. altrimenti                                -> diretto
// Con wake=true, se serve il tunnel sveglia anche il dispositivo (prepareTunnel).
struct Path {
    bool via_vpn = false;        // la connessione va legata all'IP 100.x del display
    bool on_lan = false;         // destinazione nella rete del WiFi attuale (regola 2)
    String peer_ip;              // dispositivo della tailnet che trasporta il traffico
    String peer_name;
    Tunnel tunnel = Tunnel::NoVpn;
};
Path pathFor(const IPAddress& dest, bool wake);
String describe(const Path& p);  // es. "diretto" / "VPN via ryzen (tunnel attivo)"

// Dimentica l'identità Tailscale del display (nuova registrazione al prossimo
// avvio: il vecchio nodo va rimosso a mano dalla console Tailscale).
void   forgetIdentity();

// Log dettagliati di MicroLink sulla seriale (default: solo errori/avvisi).
void   setVerbose(bool on);

}  // namespace DhVpn
