/*
 * DhSettings — pagina Impostazioni condivisa tra loader "pesante" e firmware
 * principale di Display Hub.
 *
 *   IMPOSTAZIONI ─┬─ Rete ──> RETE: WiFi / Bluetooth / VPN (interruttore + tap = dettaglio)
 *                 └─ Informazioni ──> DETTAGLIO (testo fornito dall'applicazione)
 *
 * La libreria possiede:
 *  - gli interruttori WiFi/BT e la loro persistenza (NVS "dh_net": wifi_en, bt_en);
 *  - accensione/spegnimento di WiFi (STA + reti salvate in [wifi] della configurazione) e BLE;
 *  - le schermate Impostazioni, Rete, Dettaglio e una schermata di input testo
 *    riusabile (password WiFi, auth key, hostname) con tastiera a schermo.
 *
 * La VPN NON è una dipendenza di questa libreria: arriva tramite l'interfaccia
 * VpnBackend (vedi DhVpnSettings.h in lib/dh_vpn). Così le build senza VPN non
 * compilano/linkano nulla di MicroLink: basta non passare un backend.
 *
 * Da usare con LVGL già inizializzato (display_init/touch_init della BSP).
 */
#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include <vector>

#include "esp_mac.h"

namespace DhSettings {

// Interfaccia implementata dall'applicazione (tipicamente con DhVpnSettings.h).
class VpnBackend {
public:
    virtual ~VpnBackend() {}
    virtual bool enabled() = 0;
    virtual void setEnabled(bool on) = 0;
    virtual bool connected() = 0;
    virtual String summary() = 0;       // sottotitolo della riga "VPN Tailscale"
    virtual String detailText() = 0;    // testo del dettaglio (colori LVGL "#rrggbb ...#" ammessi)
    virtual String hostname() = 0;
    virtual bool setAuthKey(const String& key) = 0;
    virtual void setHostname(const String& name) = 0;
    virtual void forgetIdentity() = 0;
    virtual void setVerbose(bool on) = 0;
};

// ---- Gateway Bluetooth (audio verso le casse) ----
// Come la VPN: la libreria disegna le schermate, i dati e i comandi arrivano dall'applicazione
// tramite questa interfaccia (nel firmware principale passano dal server). Le chiamate sono
// BLOCCANTI e brevi; la scansione e' asincrona (si avvia, poi si legge con scanPoll).
struct GatewayInfo {
    String id;                 // id sul server (file INI del gateway)
    String name, ip, mac, version, wifi_ssid;
    String target_addr, target_name;   // cassa in uso sul gateway
    String error;              // perche' e' offline
    bool online = false, bt_connected = false;
    int volume = -1, wifi_rssi = 0;
    uint32_t uptime_s = 0;
};
struct GatewayDevice {
    String addr, name;         // indirizzo BT (aa:bb:cc:dd:ee:ff) e nome (puo' mancare)
    int rssi = 0;
    bool connected = false, selected = false, known = false;
};
class GatewayBackend {
public:
    virtual ~GatewayBackend() {}
    virtual bool list(std::vector<GatewayInfo>& out, String& err) = 0;
    virtual bool info(const String& id, GatewayInfo& gw, std::vector<GatewayDevice>& known, String& err) = 0;
    virtual bool scanStart(const String& id, String& err) = 0;
    virtual bool scanPoll(const String& id, bool& scanning, std::vector<GatewayDevice>& found, String& err) = 0;
    virtual bool connect(const String& id, const String& addr, const String& name, String& err) = 0;
    virtual bool disconnect(const String& id, String& err) = 0;
    virtual bool forget(const String& id, const String& addr, String& err) = 0;   // dissocia: toglie dall'elenco
};

struct Config {
    // WiFi: false nelle build USB (riga visibile ma bloccata, con il motivo).
    bool wifi_available = true;
    const char* wifi_unavailable_reason = "Non disponibile in modalità USB";

    VpnBackend* vpn = nullptr;             // nullptr = VPN non presente in questa build
    GatewayBackend* gateway = nullptr;     // nullptr = niente sezione "Gateway Bluetooth"

    const char* info_subtitle = "";
    String (*info_text)() = nullptr;       // contenuto di Impostazioni > Informazioni

    // Azioni opzionali (nullptr = pulsante non mostrato)
    // "Aggiungi rete": il loader la fa nella sua scheda di scansione (callback);
    // senza callback (firmware principale) c'è la schermata interna con scansione.
    void (*on_wifi_scan)() = nullptr;
    void (*on_wifi_reconfigure)() = nullptr; // es. firmware: captive portal
    const char* wifi_reconfigure_label = "Cambia rete";
    void (*on_bt_scan)() = nullptr;

    // Notifiche all'applicazione
    void (*on_exit)() = nullptr;               // "Indietro" dalla radice delle impostazioni
    void (*on_wifi_changed)(bool on) = nullptr;
    void (*on_bt_changed)(bool on) = nullptr;
};

// Legge gli interruttori da NVS e costruisce le schermate (non accende nulla).
void begin(const Config& cfg);

// Accensione iniziale secondo gli interruttori salvati.
void startWifiIfEnabled();   // WiFi.mode(STA) + rete salvata, non bloccante
void startBtIfEnabled();

void open();                 // mostra Impostazioni
bool isOpen();               // una schermata di DhSettings è attiva
void refresh();              // aggiorna righe/dettaglio (lo fa già da sola ogni secondo)

bool wifiEnabled();
bool btEnabled();
void setWifiEnabled(bool on);  // persistito
void setBtEnabled(bool on);    // persistito

// Stato dell'archivio (microSD) per le icone dell'applicazione: -1 build senza
// microSD, 0 assente (solo RAM), 1 serve attenzione (scelta, non utilizzabile,
// salvataggio in corso), 2 microSD in uso.
int storageLevel();
// Etichetta (creata dall'app, es. accanto all'ingranaggio) che DhSettings
// trasforma nell'icona microSD colorata e tiene aggiornata.
void attachStorageIcon(lv_obj_t* label);

// Messaggio a comparsa (4 s) e finestra "Annulla / Conferma", per le schermate costruite fuori da
// questa libreria (es. Gateway Bluetooth).
void showToast(const String& text);
void showConfirm(const char* title, const char* text, void (*on_yes)());

// Operazioni bloccanti in corso (scansioni, connessioni): i tap vengono ignorati.
void setBusy(bool busy);
bool busy();

// Schermata di input testo riusabile.
typedef void (*InputCallback)(const String& text);
void openInput(const String& title, const String& initial, bool password, uint16_t max_len,
               const char* hint, InputCallback on_ok, void (*on_back)());
void setInputStatus(const String& text);

// Helper grafici condivisi (stesso stile in tutte le schermate).
void styleScreen(lv_obj_t* scr);
lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* user_data = nullptr);
lv_obj_t* makeHeader(lv_obj_t* scr, const char* title, lv_event_cb_t back_cb);
void setLabelIfChanged(lv_obj_t* label, const String& text);
String macString(esp_mac_type_t type);
int signalPercent(int rssi);

extern const lv_color_t COLOR_CARD;
extern const lv_color_t COLOR_MUTED;

}  // namespace DhSettings
