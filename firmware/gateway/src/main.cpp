/*
 * Display Hub — Gateway Bluetooth (ESP32 classico)
 *
 * Ponte tra il Display Hub (e il server) e una cassa Bluetooth:
 *   - A2DP sorgente verso la cassa, AVRCP per il volume assoluto (il volume
 *     della cassa stessa, se lo supporta) + scalatura del segnale come riserva;
 *   - comandi via WiFi: API HTTP JSON in LAN (http://dh-gateway.local/api/...),
 *     con token facoltativo (header X-DH-Token);
 *   - comandi via USB: stesso protocollo JSON-per-riga del loader (hello, scan,
 *     connect, status) più i comandi del gateway: la web UI lo configura dal
 *     pannello "USB e loader".
 *
 * Passo 1: WiFi, ricerca/connessione/riconnessione della cassa, volume, tono di
 * prova (per verificare tutta la catena audio). Lo streaming arriva al passo 2.
 * Comandi: README.md (gateway_* via USB = stessi nomi dell'API HTTP).
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <algorithm>
#include <HTTPClient.h>
#include <WiFi.h>
#include <math.h>

#include <vector>

#include "BluetoothA2DPSource.h"
#include "esp_avrc_api.h"
#include "esp_gap_bt_api.h"
#include "portal_util.h"
#include "gateway_build.h"   // GW_NAME / GW_DEVICE_ID scelti alla compilazione

#define GW_VERSION "1.0.0"
static const char* FW_ID = "dh-gateway";

// ---------------------------------------------------------------- A2DP
// Sottoclasse solo per fermare/riprendere la ricerca: la libreria cerca
// dispositivi SENZA SOSTA finché non si connette, e la ricerca Bluetooth
// disturba il WiFi. Qui si cerca solo se serve (scansione chiesta o
// riconnessione alla cassa scelta).
class GatewaySource : public BluetoothA2DPSource {
public:
    void set_discovery(bool on) {
        is_end = !on;
        if (on) esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        else esp_bt_gap_cancel_discovery();
    }
};

static GatewaySource a2dp;
static Preferences prefs;
static WebServer http(80);

struct Found {
    String name;
    String addr;
    int rssi;
    uint32_t seen_ms;
};
static std::vector<Found> g_found;
static SemaphoreHandle_t g_lock;

static String g_name = GW_NAME;   // nome in rete (mDNS) e Bluetooth (alla compilazione: gateway_build.h)
static String g_token;                 // se vuoto: API senza token
static String g_target_addr;           // cassa scelta ("" = nessuna)
static String g_target_name;
static uint8_t g_volume = 60;          // 0..100
// Config comandata dal server: host, id del gateway, intervallo di ricontrollo.
static String g_server_host;           // IP/host del Display Hub (vuoto = nessun download)
static uint16_t g_server_port = 12000;
static String g_device_id = GW_DEVICE_ID;  // id con cui il server identifica QUESTO gateway
static uint32_t g_poll_interval_s = 30;
static uint32_t g_last_cfg_fetch_ms = 0;

// LED di stato (default IO2). GPIO2 e' un pin di boot strapping: usarlo come
// uscita DOPO l'avvio va bene. Se sul WROVER il LED e' altrove, cambia con il
// comando led_pin.
#ifndef GW_LED_PIN
#define GW_LED_PIN 2
#endif
static int g_led_pin = GW_LED_PIN;

// Macchina a stati del LED basata su millis(): niente delay(), non blocca A2DP/WiFi.
//  cassa connessa  -> fisso
//  WiFi connesso   -> 3 gruppi da 2 lampeggi veloci, pausa 1 s
//  attesa          -> 3 lampeggi lenti, pausa 1 s
static void led_update() {
    uint32_t now = millis();
    int pattern;
    if (a2dp.is_connected()) pattern = 2;
    else if (WiFi.status() == WL_CONNECTED) pattern = 1;
    else pattern = 0;

    if (pattern == 2) { digitalWrite(g_led_pin, HIGH); return; }

    // durate on/off (ms); fasi pari = ON, dispari = OFF
    static const uint16_t seq0[] = {200,200, 200,200, 200,1000};                  // 3 lenti + pausa
    static const uint16_t seq1[] = {80,80, 80,300, 80,80, 80,300, 80,80, 80,1000}; // 3x2 veloci + pausa
    const uint16_t* seq; int len;
    if (pattern == 0) { seq = seq0; len = 6; } else { seq = seq1; len = 12; }

    static int last_pattern = -1, phase = 0; static uint32_t phase_start = 0;
    if (pattern != last_pattern) { last_pattern = pattern; phase = 0; phase_start = now; digitalWrite(g_led_pin, HIGH); }
    if (now - phase_start >= seq[phase]) {
        phase = (phase + 1) % len;
        phase_start = now;
        digitalWrite(g_led_pin, (phase % 2 == 0) ? HIGH : LOW);
    }
}
static volatile bool g_scan_requested = false;
static uint32_t g_scan_until_ms = 0;
static bool g_bt_started = false;

// Tono di prova (sinusoide) — la sorgente audio del passo 1.
static volatile float g_tone_freq = 0;
static volatile uint32_t g_tone_until_ms = 0;
static float g_phase = 0;

static String addr_str(const uint8_t* a) {
    char b[18];
    snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
    return String(b);
}

static bool parse_addr(const String& s, esp_bd_addr_t out) {
    unsigned v[6];
    if (sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

// Chiamata dalla libreria (task Bluetooth) per ogni dispositivo trovato.
// true = "è la cassa che cerco": la libreria si collega.
static bool on_device_found(const char* name, esp_bd_addr_t address, int rssi) {
    String a = addr_str(address);
    xSemaphoreTake(g_lock, portMAX_DELAY);
    bool updated = false;
    for (auto& f : g_found) {
        if (f.addr == a) {
            if (name && *name) f.name = name;
            f.rssi = rssi;
            f.seen_ms = millis();
            updated = true;
        }
    }
    if (!updated && g_found.size() < 30) g_found.push_back({name ? String(name) : String(""), a, rssi, millis()});
    bool match = g_target_addr.length() && a.equalsIgnoreCase(g_target_addr);
    xSemaphoreGive(g_lock);
    if (match) Serial.printf("[BT] Trovata la cassa scelta %s (%s): connessione\n", name, a.c_str());
    return match;
}

// Dati audio per la cassa (task Bluetooth): 44,1 kHz, stereo, 16 bit.
static int32_t get_audio(Frame* frames, int32_t count) {
    uint32_t now = millis();
    bool tone = g_tone_freq > 0 && (int32_t)(g_tone_until_ms - now) > 0;
    if (!tone) {
        memset(frames, 0, sizeof(Frame) * count);  // silenzio: la connessione resta viva
        return count;
    }
    const float step = 2.0f * (float)M_PI * g_tone_freq / 44100.0f;
    for (int i = 0; i < count; i++) {
        int16_t s = (int16_t)(sinf(g_phase) * 12000.0f);  // ~ -8 dBFS: non assordante
        frames[i].channel1 = s;
        frames[i].channel2 = s;
        g_phase += step;
        if (g_phase > 2.0f * (float)M_PI) g_phase -= 2.0f * (float)M_PI;
    }
    return count;
}

static void apply_volume() {
    // Volume della cassa (AVRCP "absolute volume", 0..127): le casse che lo
    // supportano regolano il LORO volume. In più la scalatura del segnale
    // (libreria) come riserva per le casse che lo ignorano.
    uint8_t v127 = (uint8_t)((g_volume * 127 + 50) / 100);
    a2dp.set_volume(v127);
    if (a2dp.is_connected()) esp_avrc_ct_send_set_absolute_volume_cmd(1, v127);
}

static void update_discovery() {
    // Si cerca se: scansione in corso, oppure c'è una cassa scelta non connessa.
    bool want = (g_scan_requested && (int32_t)(g_scan_until_ms - millis()) > 0) ||
                (g_target_addr.length() && !a2dp.is_connected());
    static bool active = false;
    if (want != active) {
        active = want;
        a2dp.set_discovery(want);
        Serial.printf("[BT] Ricerca dispositivi %s\n", want ? "avviata" : "fermata");
    }
    if (g_scan_requested && (int32_t)(g_scan_until_ms - millis()) <= 0) g_scan_requested = false;
}

static void start_bt() {
    if (g_bt_started) return;
    a2dp.set_local_name(g_name.c_str());
    a2dp.set_ssid_callback(on_device_found);
    a2dp.set_auto_reconnect(false);        // la riconnessione la gestiamo noi (cassa scelta)
    a2dp.set_data_callback_in_frames(get_audio);
    a2dp.start();
    g_bt_started = true;
    apply_volume();
    update_discovery();
}

// ---------------------------------------------------------------- WiFi
// Portale di fallback ---------------------------------------------------------
// Se non ci si collega al WiFi entro ~25 s (nessuna rete salvata, o rete assente) il
// gateway apre la rete "DH-Gateway-Setup" (password facoltativa: gateway_config
// ap_pass). Un DNS "captive" porta qualunque indirizzo alla pagina di setup, dove si
// sceglie la rete dall'elenco e si scrive la password. Le credenziali si SALVANO SOLO
// SE la connessione riesce (come via USB); se fallisce resta la rete di prima.
static void http_begin();              // definita in fondo: la usa anche il portale
static bool g_http_started = false;   // il web server parte col WiFi (o col portale)
static DNSServer g_dns;
static bool g_portal_active = false;
static String g_portal_options;       // <option> delle reti trovate (gia' escapate)
static String g_portal_msg;           // esito dell'ultimo tentativo, mostrato nella pagina
static bool g_pending_connect = false;
static String g_pending_ssid, g_pending_pass;
static uint32_t g_pending_start_ms = 0;
static uint32_t g_portal_close_at = 0;

// Le richieste del portale valgono solo se arrivano dalla rete del portale (AP), non
// da chi e' sulla LAN: altrimenti chiunque in casa potrebbe cambiare il WiFi.
static bool portal_request_ok() {
    return g_portal_active && http.client().localIP() == WiFi.softAPIP();
}

static void portal_scan() {
    int n = WiFi.scanNetworks();
    String opts;
    if (n > 0) {
        std::vector<int> idx;
        for (int i = 0; i < n; i++) idx.push_back(i);
        std::sort(idx.begin(), idx.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
        std::vector<String> seen;
        int shown = 0;
        for (int i : idx) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;  // reti nascoste: si scrivono a mano
            bool dup = false;
            for (const String& t : seen) if (t == ssid) { dup = true; break; }
            if (dup) continue;
            seen.push_back(ssid);
            opts += portal_option_html(ssid, WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            if (++shown >= 15) break;
        }
    }
    WiFi.scanDelete();
    g_portal_options = opts;
}

static void portal_handle_page() {
    if (!portal_request_ok()) { http.send(404, "text/plain", "non trovato"); return; }
    http.send(200, "text/html", portal_page_html(g_portal_msg, g_portal_options));
}

static void portal_handle_scan() {
    if (!portal_request_ok()) { http.send(404, "text/plain", "non trovato"); return; }
    portal_scan();
    http.sendHeader("Location", "/portal");
    http.send(303, "text/plain", "");
}

static void portal_handle_save() {
    if (!portal_request_ok()) { http.send(404, "text/plain", "non trovato"); return; }
    String ssid = http.arg("manual");
    ssid.trim();
    if (ssid.length() == 0) ssid = http.arg("s");
    String pass = http.arg("p");
    http.sendHeader("Location", "/portal");
    if (!portal_creds_valid(ssid, pass)) {
        g_portal_msg = "Dati non validi: nome rete 1-32 caratteri; password vuota (rete aperta) oppure 8-63 caratteri.";
        http.send(303, "text/plain", "");
        return;
    }
    g_pending_ssid = ssid;
    g_pending_pass = pass;
    g_pending_start_ms = millis();
    g_pending_connect = true;
    g_portal_msg = String("Provo a collegarmi a '") + ssid + String("'... Se riesce il LED fa 3 raffiche da 2 lampeggi veloci.");
    http.send(303, "text/plain", "");   // prima la risposta: connettendosi il canale WiFi puo' cambiare
    WiFi.begin(ssid.c_str(), pass.c_str());
}

static void stop_portal() {
    if (!g_portal_active) return;
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    g_portal_active = false;
    g_pending_connect = false;
    g_portal_close_at = 0;
    g_portal_msg = "";
    Serial.println("[Portale] chiuso");
}

static void start_portal() {
    static uint32_t last_try = 0;
    if (g_portal_active) return;
    if (last_try && millis() - last_try < 10000UL) return;  // se softAP fallisce, non riprovare a raffica
    last_try = millis();
    WiFi.mode(WIFI_AP_STA);
    String ap_pass = prefs.getString("ap_pass", "");
    bool ok = ap_pass.length() >= 8 ? WiFi.softAP("DH-Gateway-Setup", ap_pass.c_str()) : WiFi.softAP("DH-Gateway-Setup");
    if (!ok) { Serial.println("[Portale] softAP non riuscito"); return; }
    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dns.start(53, "*", WiFi.softAPIP());
    g_portal_active = true;
    g_portal_msg = "";
    portal_scan();
    http_begin();   // idempotente: registra le rotte (API + portale) e avvia il server una volta sola
    Serial.printf("[Portale] Rete 'DH-Gateway-Setup'%s attiva: apri http://%s/portal\n",
                  ap_pass.length() >= 8 ? " (con password)" : "", WiFi.softAPIP().toString().c_str());
}

// Da chiamare nel loop: DNS captive, esito della connessione richiesta, chiusura.
static void portal_tick() {
    if (g_portal_active) g_dns.processNextRequest();
    if (g_pending_connect) {
        if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == g_pending_ssid) {
            prefs.putString("ssid", g_pending_ssid);   // salvata solo perche' funziona
            prefs.putString("pass", g_pending_pass);
            g_portal_msg = String("Connesso a '") + g_pending_ssid + String("' (IP ") + WiFi.localIP().toString() + String(").");
            Serial.printf("[Portale] %s\n", g_portal_msg.c_str());
            g_pending_connect = false;
            g_portal_close_at = millis() + 8000UL;     // il tempo di leggere la pagina
        } else if (millis() - g_pending_start_ms > 20000UL) {
            g_portal_msg = String("Connessione a '") + g_pending_ssid + String("' non riuscita (password o segnale?). Riprova.");
            Serial.printf("[Portale] %s\n", g_portal_msg.c_str());
            g_pending_connect = false;
            WiFi.disconnect(false);
            String old = prefs.getString("ssid", "");   // riprende la rete salvata prima, se c'e'
            if (old.length()) WiFi.begin(old.c_str(), prefs.getString("pass", "").c_str());
        }
    }
    if (g_portal_close_at && (int32_t)(millis() - g_portal_close_at) > 0) stop_portal();
}

static void wifi_begin() {
    String ssid = prefs.getString("ssid", "");
    if (!ssid.length()) {
        Serial.println("[WiFi] Nessuna rete configurata: usa il pannello USB della web UI (Scansiona / Connetti)");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(g_name.c_str());
    WiFi.setSleep(true);  // obbligatorio con il Bluetooth attivo (coesistenza radio)
    WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
    Serial.printf("[WiFi] Connessione a '%s'...\n", ssid.c_str());
}

static void http_begin();  // definita più sotto (usata prima)

static void mdns_begin() {
    String host = g_name;
    host.toLowerCase();
    host.replace(" ", "-");
    if (MDNS.begin(host.c_str())) {
        MDNS.addService("dhgw", "tcp", 80);  // il display lo trova da solo (_dhgw._tcp)
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[WiFi] Raggiungibile come http://%s.local\n", host.c_str());
    }
}

// ---------------------------------------------------------------- comandi
// Un solo insieme di comandi per USB e HTTP. in: {"cmd": ..., ...}; out: risposta.
static void fill_status(JsonDocument& out) {
    out["type"] = "gateway_status";
    out["fw"] = FW_ID;
    out["version"] = GW_VERSION;
    out["name"] = g_name;
    out["uptime_s"] = millis() / 1000;
    out["free_heap"] = ESP.getFreeHeap();
    JsonObject w = out["wifi"].to<JsonObject>();
    w["configured"] = prefs.getString("ssid", "").length() > 0;
    w["connected"] = WiFi.status() == WL_CONNECTED;
    w["portal"] = g_portal_active;
    if (WiFi.status() == WL_CONNECTED) {
        w["ssid"] = WiFi.SSID();
        w["ip"] = WiFi.localIP().toString();
        w["rssi"] = WiFi.RSSI();
    }
    JsonObject b = out["bt"].to<JsonObject>();
    b["connected"] = a2dp.is_connected();
    b["target_addr"] = g_target_addr;
    b["target_name"] = g_target_name;
    b["scanning"] = g_scan_requested;
    b["volume"] = g_volume;
    b["mac"] = WiFi.macAddress();  // (MAC di base del chip)
    JsonObject sv = out["server"].to<JsonObject>();
    sv["host"] = g_server_host;
    sv["port"] = g_server_port;
    sv["device_id"] = g_device_id;
    sv["poll_interval_s"] = g_poll_interval_s;
    JsonObject a = out["audio"].to<JsonObject>();
    bool tone = g_tone_freq > 0 && (int32_t)(g_tone_until_ms - millis()) > 0;
    a["source"] = tone ? "tone" : "none";
    a["playing"] = tone;
}

static void fill_devices(JsonDocument& out) {
    out["type"] = "gateway_devices";
    out["scanning"] = g_scan_requested;
    JsonArray arr = out["devices"].to<JsonArray>();
    xSemaphoreTake(g_lock, portMAX_DELAY);
    for (const auto& f : g_found) {
        if (millis() - f.seen_ms > 120000) continue;  // visti negli ultimi 2 minuti
        JsonObject o = arr.add<JsonObject>();
        o["name"] = f.name;
        o["addr"] = f.addr;
        o["rssi"] = f.rssi;
        o["selected"] = f.addr.equalsIgnoreCase(g_target_addr);
    }
    xSemaphoreGive(g_lock);
}

static void result(JsonDocument& out, const char* cmd, bool ok, const String& msg = "") {
    out["type"] = "gateway_result";
    out["cmd"] = cmd;
    out["ok"] = ok;
    if (msg.length()) out[ok ? "message" : "error"] = msg;
}

static void do_wifi_scan(JsonDocument& out) {
    int n = WiFi.scanNetworks();
    out["type"] = "scan_result";   // stesso formato del loader
    JsonArray arr = out["networks"].to<JsonArray>();
    for (int i = 0; i < n && i < 25; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
        o["channel"] = WiFi.channel(i);
    }
    WiFi.scanDelete();
}

static void do_wifi_connect(JsonDocument& in, JsonDocument& out) {
    String ssid = in["ssid"] | "", pass = in["password"] | "";
    out["type"] = "connect_result";  // stesso formato del loader
    out["ssid"] = ssid;
    if (!ssid.length()) {
        out["success"] = false;
        out["error"] = "ssid mancante";
        return;
    }
    stop_portal();
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(100);
    bool ok = WiFi.status() == WL_CONNECTED;
    out["success"] = ok;
    if (ok) {
        prefs.putString("ssid", ssid);  // salvata solo se funziona
        prefs.putString("pass", pass);
        out["ip"] = WiFi.localIP().toString();
        http_begin();
        mdns_begin();
    } else {
        out["error"] = "connessione non riuscita (password o segnale)";
        wifi_begin();  // torna alla rete salvata, se c'è
    }
}

// Estrae il valore di "chiave" nella sezione [sezione] da un testo INI.
// Ritorna "" se non trovato. Semplice: riga per riga, ignora commenti (; e #).
static String ini_get(const String& ini, const char* section, const char* key) {
    String cur;
    int i = 0, n = ini.length();
    while (i < n) {
        int nl = ini.indexOf('\n', i);
        if (nl < 0) nl = n;
        String line = ini.substring(i, nl); line.trim();
        i = nl + 1;
        if (line.length() == 0 || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.endsWith("]")) { cur = line.substring(1, line.length() - 1); cur.trim(); continue; }
        if (cur == section) {
            int eq = line.indexOf('=');
            if (eq > 0) {
                String k = line.substring(0, eq); k.trim();
                if (k == key) { String v = line.substring(eq + 1); v.trim(); return v; }
            }
        }
    }
    return "";
}

// Scarica l'INI dal server e applica cassa e volume. Il SERVER comanda.
static bool fetch_config_from_server() {
    if (g_server_host.length() == 0 || WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    http.setConnectTimeout(3000); http.setTimeout(5000);
    String url = String("http://") + g_server_host + ":" + String(g_server_port)
               + "/api/esp/bt-gateway-config?device_id=" + g_device_id;
    http.begin(url);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[CFG] download config: HTTP %d da %s\n", code, url.c_str());
        http.end();
        return false;
    }
    String ini = http.getString();
    http.end();

    // poll interval (opzionale)
    String pi = ini_get(ini, "server", "poll_interval_s");
    if (pi.length()) { long v = pi.toInt(); if (v >= 5) g_poll_interval_s = (uint32_t)v; }

    // volume
    String vol = ini_get(ini, "bt", "volume");
    if (vol.length()) {
        int v = constrain(vol.toInt(), 0, 100);
        if ((uint8_t)v != g_volume) {
            g_volume = (uint8_t)v;
            prefs.putUChar("volume", g_volume);
            apply_volume();
            Serial.printf("[CFG] volume dal server: %d\n", g_volume);
        }
    }

    // cassa: "MAC | Nome". Se diversa dalla attuale, riconnetti.
    String sp = ini_get(ini, "bt", "speaker");
    String mac, name;
    int bar = sp.indexOf('|');
    if (bar >= 0) { mac = sp.substring(0, bar); name = sp.substring(bar + 1); }
    else mac = sp;
    mac.trim(); name.trim(); mac.toLowerCase();
    if (mac.length() && mac != g_target_addr) {
        esp_bd_addr_t tmp;
        if (parse_addr(mac, tmp)) {
            if (a2dp.is_connected()) a2dp.disconnect();
            g_target_addr = mac; g_target_name = name;
            prefs.putString("peer", g_target_addr);
            prefs.putString("peer_name", g_target_name);
            update_discovery();
            Serial.printf("[CFG] cassa dal server: %s (%s)\n", name.c_str(), mac.c_str());
        }
    }
    Serial.println("[CFG] config del server applicata");
    return true;
}

static void handle_command(JsonDocument& in, JsonDocument& out) {
    String cmd = in["cmd"] | "";
    if (cmd == "hello") {
        out["type"] = "loader_hello";  // il pannello USB della web UI lo riconosce come i loader
        out["fw"] = FW_ID;
        out["version"] = GW_VERSION;
        JsonArray f = out["features"].to<JsonArray>();
        f.add("wifi");
        f.add("bt_gateway");
    } else if (cmd == "status" || cmd == "gateway_status") {
        fill_status(out);
    } else if (cmd == "scan") {
        do_wifi_scan(out);
    } else if (cmd == "connect") {
        do_wifi_connect(in, out);
    } else if (cmd == "server_config") {
        // host/porta/id del Display Hub, per scaricare la config. Vale subito.
        if (in["host"].is<const char*>())  { g_server_host = in["host"].as<String>(); prefs.putString("srv_host", g_server_host); }
        if (in["port"].is<int>())          { g_server_port = (uint16_t)in["port"].as<int>(); prefs.putUShort("srv_port", g_server_port); }
        if (in["device_id"].is<const char*>()) { g_device_id = in["device_id"].as<String>(); prefs.putString("dev_id", g_device_id); }
        g_last_cfg_fetch_ms = 0;  // forza un download al prossimo giro
        result(out, "server_config", true, "salvato; scarico la config appena possibile");
    } else if (cmd == "fetch_config") {
        result(out, "fetch_config", fetch_config_from_server(), "");
    } else if (cmd == "led_pin") {
        if (in["pin"].is<int>()) {
            g_led_pin = in["pin"].as<int>();
            prefs.putInt("led_pin", g_led_pin);
            pinMode(g_led_pin, OUTPUT);
            result(out, "led_pin", true, "pin LED aggiornato (riavvia se non lampeggia)");
        } else result(out, "led_pin", false, "manca 'pin'");
    } else if (cmd == "gateway_config") {
        // nome in rete / Bluetooth e token dell'API (valgono dal prossimo avvio)
        if (in["name"].is<const char*>()) prefs.putString("name", in["name"].as<String>());
        if (in["token"].is<const char*>()) prefs.putString("token", in["token"].as<String>());
        if (in["ap_pass"].is<const char*>()) {   // password della rete del portale (vuota = aperta)
            String ap = in["ap_pass"].as<String>();
            if (ap.length() != 0 && (ap.length() < 8 || ap.length() > 63)) {
                result(out, "gateway_config", false, "ap_pass: vuota (portale aperto) oppure 8-63 caratteri");
                return;
            }
            prefs.putString("ap_pass", ap);
        }
        result(out, "gateway_config", true, "salvato: vale dal prossimo avvio");
    } else if (cmd == "wifi_forget") {
        prefs.remove("ssid");
        prefs.remove("pass");
        WiFi.disconnect(true);
        result(out, "wifi_forget", true);
    } else if (cmd == "bt_scan") {
        g_scan_requested = true;
        g_scan_until_ms = millis() + 1000UL * (uint32_t)(in["seconds"] | 20);
        update_discovery();
        result(out, "bt_scan", true, "ricerca avviata: leggi bt_devices");
    } else if (cmd == "bt_devices") {
        fill_devices(out);
    } else if (cmd == "bt_connect") {
        String addr = in["addr"] | "";
        esp_bd_addr_t tmp;
        if (!parse_addr(addr, tmp)) {
            result(out, "bt_connect", false, "indirizzo non valido (aa:bb:cc:dd:ee:ff)");
            return;
        }
        if (a2dp.is_connected()) a2dp.disconnect();
        addr.toLowerCase();
        g_target_addr = addr;
        g_target_name = in["name"] | "";
        prefs.putString("peer", g_target_addr);     // riconnessione automatica all'avvio
        prefs.putString("peer_name", g_target_name);
        update_discovery();
        result(out, "bt_connect", true, "ricerca della cassa e connessione in corso");
    } else if (cmd == "bt_disconnect") {
        g_target_addr = "";  // niente riconnessione finché non se ne sceglie una
        if (a2dp.is_connected()) a2dp.disconnect();
        update_discovery();
        result(out, "bt_disconnect", true);
    } else if (cmd == "bt_forget") {
        g_target_addr = "";
        g_target_name = "";
        prefs.remove("peer");
        prefs.remove("peer_name");
        if (a2dp.is_connected()) a2dp.disconnect();
        update_discovery();
        result(out, "bt_forget", true);
    } else if (cmd == "volume") {
        int v = g_volume;
        if (in["volume"].is<int>()) v = in["volume"].as<int>();
        if (in["delta"].is<int>()) v += in["delta"].as<int>();
        g_volume = (uint8_t)constrain(v, 0, 100);
        prefs.putUChar("volume", g_volume);
        apply_volume();
        out["type"] = "gateway_result";
        out["cmd"] = "volume";
        out["ok"] = true;
        out["volume"] = g_volume;
    } else if (cmd == "tone") {
        float f = in["freq"] | 440.0f;
        int s = in["seconds"] | 3;
        g_tone_freq = constrain(f, 50.0f, 8000.0f);
        g_tone_until_ms = millis() + 1000UL * (uint32_t)constrain(s, 1, 30);
        result(out, "tone", a2dp.is_connected(), a2dp.is_connected() ? "" : "nessuna cassa connessa");
    } else if (cmd == "stop") {
        g_tone_until_ms = millis();
        result(out, "stop", true);
    } else if (cmd == "reboot") {
        result(out, "reboot", true);
    } else {
        out["type"] = "error";
        out["error"] = "comando sconosciuto: " + cmd;
    }
}

// ---------------------------------------------------------------- USB
static String g_line;
static void poll_serial() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            JsonDocument in, out;
            if (g_line.length() && !deserializeJson(in, g_line)) {
                handle_command(in, out);
                serializeJson(out, Serial);
                Serial.println();
                if (String(in["cmd"] | "") == "reboot") {
                    delay(200);
                    ESP.restart();
                }
            }
            g_line = "";
        } else if (c != '\r' && g_line.length() < 2048) {
            g_line += c;
        }
    }
}

// ---------------------------------------------------------------- HTTP
// POST /api/<comando> con corpo JSON (i parametri del comando), GET /api/status
// e /api/bt/devices. Token facoltativo nell'header X-DH-Token.
static bool authorized() {
    if (!g_token.length()) return true;
    return http.header("X-DH-Token") == g_token;
}

static void http_command(const char* cmd) {
    if (!authorized()) {
        http.send(401, "application/json", "{\"error\":\"token mancante o errato\"}");
        return;
    }
    JsonDocument in, out;
    if (http.hasArg("plain") && http.arg("plain").length()) {
        if (deserializeJson(in, http.arg("plain"))) {
            http.send(400, "application/json", "{\"error\":\"JSON non valido\"}");
            return;
        }
    }
    in["cmd"] = cmd;
    handle_command(in, out);
    String body;
    serializeJson(out, body);
    bool bad = out["ok"].is<bool>() && !out["ok"].as<bool>();
    http.sendHeader("Access-Control-Allow-Origin", "*");
    http.send(bad ? 409 : 200, "application/json", body);
    if (String(cmd) == "reboot") {
        delay(200);
        ESP.restart();
    }
}

static void http_begin() {
    if (g_http_started) return;
    static const char* hdrs[] = {"X-DH-Token"};
    http.collectHeaders(hdrs, 1);
    http.on("/api/status", HTTP_GET, [] { http_command("status"); });
    http.on("/api/bt/devices", HTTP_GET, [] { http_command("bt_devices"); });
    http.on("/api/bt/scan", HTTP_POST, [] { http_command("bt_scan"); });
    http.on("/api/bt/connect", HTTP_POST, [] { http_command("bt_connect"); });
    http.on("/api/bt/disconnect", HTTP_POST, [] { http_command("bt_disconnect"); });
    http.on("/api/bt/forget", HTTP_POST, [] { http_command("bt_forget"); });
    http.on("/api/audio/volume", HTTP_POST, [] { http_command("volume"); });
    http.on("/api/audio/tone", HTTP_POST, [] { http_command("tone"); });
    http.on("/api/audio/stop", HTTP_POST, [] { http_command("stop"); });
    http.on("/api/server", HTTP_POST, [] { http_command("server_config"); });
    http.on("/api/fetch-config", HTTP_POST, [] { http_command("fetch_config"); });
    http.on("/api/reboot", HTTP_POST, [] { http_command("reboot"); });
    http.on("/portal", HTTP_GET, portal_handle_page);
    http.on("/portal/scan", HTTP_GET, portal_handle_scan);
    http.on("/portal/save", HTTP_POST, portal_handle_save);
    http.onNotFound([] {
        if (portal_request_ok()) {   // captive portal: qualunque indirizzo -> pagina di setup
            http.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + String("/portal"));
            http.send(302, "text/plain", "");
        } else {
            http.send(404, "application/json", "{\"error\":\"non trovato\"}");
        }
    });
    http.begin();
    g_http_started = true;
    Serial.println("[WiFi] Server dei comandi avviato (porta 80)");
}

// ---------------------------------------------------------------- avvio
void setup() {
    Serial.setRxBufferSize(4096);
    Serial.begin(115200);
    delay(200);
    g_lock = xSemaphoreCreateMutex();
    prefs.begin("dhgw", false);
    g_name = prefs.getString("name", GW_NAME);
    g_token = prefs.getString("token", "");
    g_target_addr = prefs.getString("peer", "");
    g_target_name = prefs.getString("peer_name", "");
    g_volume = prefs.getUChar("volume", 60);
    g_server_host = prefs.getString("srv_host", "");
    g_server_port = prefs.getUShort("srv_port", 12000);
    g_device_id = prefs.getString("dev_id", GW_DEVICE_ID);
    Serial.printf("\n[BOOT] Display Hub Gateway BT %s — '%s'\n", GW_VERSION, g_name.c_str());
    g_led_pin = prefs.getInt("led_pin", GW_LED_PIN);
    pinMode(g_led_pin, OUTPUT);
    digitalWrite(g_led_pin, LOW);

    wifi_begin();
    start_bt();
    // Il web server e mDNS si avviano nel loop, appena il WiFi è connesso:
    // avviarli senza rete manda in crash lo stack TCP/IP (Invalid mbox).
    if (g_target_addr.length())
        Serial.printf("[BT] Riconnessione alla cassa '%s' (%s)\n", g_target_name.c_str(), g_target_addr.c_str());
    Serial.printf("[MEM] RAM libera %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
}

void loop() {
    poll_serial();
    led_update();
    if (g_http_started) http.handleClient();
    update_discovery();

    static bool was_connected = false;
    bool conn = a2dp.is_connected();
    if (conn != was_connected) {
        was_connected = conn;
        Serial.printf("[BT] Cassa %s\n", conn ? "connessa" : "disconnessa");
        if (conn) apply_volume();
    }
    static bool wifi_up = false;
    if ((WiFi.status() == WL_CONNECTED) != wifi_up) {
        wifi_up = !wifi_up;
        if (wifi_up) {
            Serial.printf("[WiFi] Connesso, IP %s\n", WiFi.localIP().toString().c_str());
            http_begin();
            mdns_begin();
            if (g_portal_active && !g_portal_close_at) g_portal_close_at = millis() + 8000UL;
        }
    }
    // Senza WiFi da piu' di ~25 s (all'avvio, o dopo che e' caduto) si apre il portale;
    // si chiude da solo quando il WiFi torna.
    static uint32_t down_since = millis();
    if (WiFi.status() == WL_CONNECTED) down_since = millis();
    else if (!g_portal_active && millis() - down_since > 25000UL) start_portal();
    portal_tick();

    // Download periodico della config dal server (il server comanda cassa/volume).
    if (WiFi.status() == WL_CONNECTED && g_server_host.length()) {
        if (g_last_cfg_fetch_ms == 0 || millis() - g_last_cfg_fetch_ms > g_poll_interval_s * 1000UL) {
            g_last_cfg_fetch_ms = millis();
            fetch_config_from_server();
        }
    }

    static uint32_t last_mem = 0;
    if (millis() - last_mem > 300000UL) {
        last_mem = millis();
        Serial.printf("[MEM] RAM libera %u KB (minimo %u KB)\n", (unsigned)(ESP.getFreeHeap() / 1024),
                      (unsigned)(ESP.getMinFreeHeap() / 1024));
    }
    delay(2);
}
