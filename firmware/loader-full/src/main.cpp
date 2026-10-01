/*
 * Display Hub — firmware "loader pesante" v3 (setup WiFi / Bluetooth / VPN via touchscreen)
 *
 * Stesso scopo del loader leggero (firmware/loader/): testare/impostare la rete via
 * USB prima di flashare il firmware definitivo, ma con schermo e touch attivi.
 * Il protocollo seriale JSON-per-riga resta compatibile con il loader leggero
 * (hello/scan/connect invariati) — vedi PROTOCOL.md; in più: status, vpn_config,
 * vpn_forget.
 *
 * Struttura delle schermate:
 *
 *   HOME ("Rete")  ── tab WiFi: scansione reti + connessione (tap -> password)
 *        │         └─ tab Bluetooth: scansione/connessione dispositivi BLE
 *        │            (ogni tab è attivo solo se la funzione è accesa in Impostazioni)
 *        └─ icona ⚙ -> IMPOSTAZIONI
 *                        ├─ Rete -> RETE: WiFi / Bluetooth / VPN, ognuno con
 *                        │          interruttore; tap sulla riga -> DETTAGLIO
 *                        └─ Informazioni -> DETTAGLIO (chip, memoria, MAC)
 *
 * Impostazioni/Rete/Dettaglio/input testo sono la libreria condivisa
 * lib/dh_settings (identica nel firmware principale). Gli interruttori sono
 * persistenti in NVS (namespace "dh_net"); la VPN salva le sue impostazioni
 * nella libreria dh_vpn (namespace "dhvpn"). Stessa tabella
 * partizioni del firmware definitivo: la NVS sopravvive al flash di quest'ultimo
 * (vedi però la nota in PROTOCOL.md sull'affidabilità: per il WiFi il metodo
 * consigliato resta incorporare le credenziali nella build).
 *
 * Come nel firmware definitivo, senza chiamate periodiche a lv_tick_inc()
 * l'orologio interno di LVGL non avanza e lv_timer_handler() non disegna nulla —
 * vedi lvgl_tick_update()/pump(), usate ovunque prima di un lv_timer_handler().
 *
 * BLE soltanto: l'ESP32-S3, a differenza dell'ESP32 originale, non ha il
 * Bluetooth Classic.
 */
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <lvgl.h>

#include "DhBle.h"
#include "DhConfig.h"
#include "DhConfigSerial.h"
#include "dh_fonts.h"
#include "DhSettings.h"
#include "DhWifi.h"
#include "DhVpn.h"
#include "DhVpnSettings.h"
#include "esp32_4848s040_bsp.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

#define LOADER_VERSION "3.1.0"

// LOADER_CONFIG_SD 1: configurazione nel file displayhub.txt sulla microSD
// (solo RAM senza scheda, nessun dato nostro in NVS); 0: stesso file nella NVS.
// Impostato dalla web UI in fase di build (-D); vedi lib/dh_config.
#ifndef LOADER_CONFIG_SD
#define LOADER_CONFIG_SD 0
#endif

// =====================================================================
// Tick LVGL
// =====================================================================
static unsigned long g_last_tick_ms = 0;

static void lvgl_tick_update() {
    unsigned long now = millis();
    if (g_last_tick_ms == 0) g_last_tick_ms = now;
    uint32_t elapsed = (uint32_t)(now - g_last_tick_ms);
    if (elapsed > 0) {
        lv_tick_inc(elapsed);
        g_last_tick_ms = now;
    }
}

static void pump(int cycles = 1, int delay_ms = 5) {
    for (int i = 0; i < cycles; i++) {
        lvgl_tick_update();
        lv_timer_handler();
        delay(delay_ms);
    }
}

// Le operazioni bloccanti (scansioni, connessione WiFi/BLE) chiamano pump() per
// tenere vivo lo schermo: nel frattempo LVGL può consegnare altri tap, che
// DhSettings::busy() fa ignorare finché l'operazione non è finita.
static void set_busy(bool b) { DhSettings::setBusy(b); }
static bool is_busy() { return DhSettings::busy(); }

// =====================================================================
// Stato / oggetti UI della home
// =====================================================================
static DhVpnSettingsBackend g_vpn_backend;

static lv_obj_t* scr_home = nullptr;
static lv_obj_t* home_status_label = nullptr;
static lv_obj_t* home_tabview = nullptr;
static lv_obj_t* wifi_list = nullptr;
static lv_obj_t* wifi_scan_btn = nullptr;
static lv_obj_t* wifi_tab_status = nullptr;
static lv_obj_t* bt_list = nullptr;
static lv_obj_t* bt_scan_btn = nullptr;
static lv_obj_t* bt_tab_status = nullptr;

static String g_selected_ssid;
static String g_last_ble_device;
static String serial_buffer;


static String default_vpn_hostname() {
    uint8_t m[6] = {0};
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    char buf[32];
    snprintf(buf, sizeof(buf), "displayhub-%02x%02x%02x", m[3], m[4], m[5]);
    return String(buf);
}

static void set_btn_enabled(lv_obj_t* btn, bool enabled) {
    if (enabled) lv_obj_clear_state(btn, LV_STATE_DISABLED);
    else lv_obj_add_state(btn, LV_STATE_DISABLED);
}

// Le righe delle liste portano una stringa (SSID o indirizzo BLE) come
// user_data, liberata qui quando la lista viene ripulita (lv_obj_clean genera un
// DELETE per ogni figlio): altrimenti perdita di memoria a ogni scansione.
static void free_user_data_cb(lv_event_t* e) {
    void* data = lv_event_get_user_data(e);
    if (data) free(data);
}

static void go_home() {
    lv_scr_load(scr_home);
    pump(2);
}
static void go_home_cb(lv_event_t*) { go_home(); }
static void open_settings_cb(lv_event_t*) {
    if (!is_busy()) DhSettings::open();
}

// =====================================================================
// Protocollo seriale — invio
// =====================================================================
static void send_json(JsonDocument& doc) {
    serializeJson(doc, Serial);
    Serial.println();
}

static void send_hello() {
    JsonDocument doc;
    doc["type"] = "loader_hello";
    doc["fw"] = "wifi-setup-loader-full";
    doc["version"] = LOADER_VERSION;
    JsonArray f = doc["features"].to<JsonArray>();
    f.add("wifi");
    f.add("bt");
    f.add("vpn");
    f.add("config");  // comandi config_* / storage_*
    if (DhConfig::backend() == DhConfig::Backend::Sd) f.add("sdfiles");  // comandi file_*
    send_json(doc);
}

// =====================================================================
// WiFi
// =====================================================================
static void wifi_row_clicked_cb(lv_event_t* e);

static void do_wifi_scan(bool report_serial) {
    JsonDocument doc;
    doc["type"] = "scan_result";
    JsonArray networks = doc["networks"].to<JsonArray>();

    if (!DhSettings::wifiEnabled()) {
        lv_obj_clean(wifi_list);
        lv_list_add_text(wifi_list, "WiFi disattivato: attivalo da Impostazioni > Rete");
        if (report_serial) send_json(doc);
        return;
    }

    set_busy(true);
    lv_obj_clean(wifi_list);
    lv_list_add_text(wifi_list, "Scansione in corso...");
    pump(3);  // il testo sopra si vede davvero prima del blocco sotto

    DhWifi::pause();  // niente scansioni/tentativi automatici in parallelo
    int n = WiFi.scanNetworks();
    lv_obj_clean(wifi_list);
    if (n <= 0) lv_list_add_text(wifi_list, "Nessuna rete trovata");

    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        int rssi = WiFi.RSSI(i);
        int channel = WiFi.channel(i);
        bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

        JsonObject net = networks.add<JsonObject>();
        net["ssid"] = ssid;
        net["rssi"] = rssi;
        net["secure"] = secure;
        net["channel"] = channel;

        char line[120];
        snprintf(line, sizeof(line), "%s  (%s)\nCanale %d - %d dBm (%d%%)", ssid.c_str(),
                 secure ? "cifrata" : "aperta", channel, rssi, DhSettings::signalPercent(rssi));
        lv_obj_t* btn = lv_list_add_btn(wifi_list, LV_SYMBOL_WIFI, line);
        char* ssid_copy = strdup(ssid.c_str());
        lv_obj_add_event_cb(btn, wifi_row_clicked_cb, LV_EVENT_CLICKED, ssid_copy);
        lv_obj_add_event_cb(btn, free_user_data_cb, LV_EVENT_DELETE, ssid_copy);
    }
    WiFi.scanDelete();
    DhWifi::resume();
    set_busy(false);

    if (report_serial) send_json(doc);
}

// Connessione WiFi (da touch o da comando seriale). A differenza del loader v2,
// in caso di successo la connessione RESTA attiva: serve alla VPN.
static void attempt_connect(const String& ssid, const String& password, bool report_serial) {
    if (!DhSettings::wifiEnabled()) DhSettings::setWifiEnabled(true);
    DhWifi::pause();  // prova esplicita: DhWifi non deve scegliere un'altra rete nel frattempo
    set_busy(true);
    DhSettings::setInputStatus("Connessione in corso...");
    pump(3);

    WiFi.disconnect();
    WiFi.persistent(false);  // credenziali solo nella configurazione
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();
    const unsigned long timeout_ms = 15000;
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
        pump(1, 200);  // LVGL resta vivo durante l'attesa
    }

    JsonDocument doc;
    doc["type"] = "connect_result";
    bool success = WiFi.status() == WL_CONNECTED;

    if (success) {
        String ip = WiFi.localIP().toString();
        doc["success"] = true;
        doc["ip"] = ip;

        // Nella configurazione (microSD o NVS), in cima all'elenco, salvata subito.
        DhWifi::add(ssid, password, true);
        if (DhConfig::backend() == DhConfig::Backend::Nvs) {
            // Compatibilità: i firmware definitivi generati prima della
            // configurazione su file leggono ancora questo namespace.
            Preferences p;
            p.begin("dh_wifi", false);
            p.putString("ssid", ssid);
            p.putString("password", password);
            p.end();
        }

        DhSettings::setInputStatus("Connesso! IP: " + ip + "\nCredenziali salvate.");
    } else {
        doc["success"] = false;
        doc["error"] = "Connessione fallita o timeout (rete non trovata, password errata?)";
        DhSettings::setInputStatus("Connessione fallita o timeout.");
        WiFi.disconnect();
    }
    DhWifi::resume();  // riprende la gestione: connessi -> resta; altrimenti torna sulla migliore rete nota
    set_busy(false);

    if (report_serial) send_json(doc);
}

static void wifi_password_ok(const String& password) { attempt_connect(g_selected_ssid, password, true); }

static void wifi_row_clicked_cb(lv_event_t* e) {
    if (is_busy()) return;
    char* ssid = (char*)lv_event_get_user_data(e);
    if (!ssid) return;
    g_selected_ssid = String(ssid);
    DhSettings::openInput("Password per: " + g_selected_ssid, "", true, 64, "", wifi_password_ok, go_home);
}

static void wifi_scan_btn_cb(lv_event_t*) {
    if (!is_busy()) do_wifi_scan(true);
}

// =====================================================================
// Bluetooth (BLE)
// =====================================================================
static void ble_device_clicked_cb(lv_event_t* e);

static void on_bt_changed(bool on) {
    if (!on) lv_obj_clean(bt_list);  // lo stack sta per spegnersi (DhBle::end)
}

// Riga della lista: "indirizzo|tipo|nome" come user_data (liberata con la riga).
static void ble_scan() {
    if (is_busy()) return;
    lv_obj_clean(bt_list);
    if (!DhSettings::btEnabled()) {
        lv_list_add_text(bt_list, "Bluetooth disattivato: attivalo da Impostazioni > Rete");
        return;
    }
    set_busy(true);
    lv_list_add_text(bt_list, "Scansione BLE in corso...");
    pump(3);

    auto found = DhBle::scan(4);  // 4 s, bloccante
    lv_obj_clean(bt_list);
    if (found.empty()) lv_list_add_text(bt_list, "Nessun dispositivo trovato");
    auto known = DhBle::devices();
    for (const auto& f : found) {
        String name = f.name.length() ? f.name : String("(senza nome)");
        String mark;
        for (const auto& k : known)
            if (k.addr == f.addr) mark = k.bonded ? "  [accoppiato]" : "  [ricordato]";
        char line[140];
        snprintf(line, sizeof(line), "%s%s\n%s  (%d dBm)", name.c_str(), mark.c_str(), f.addr.c_str(), f.rssi);
        lv_obj_t* btn = lv_list_add_btn(bt_list, LV_SYMBOL_BLUETOOTH, line);
        String tag = f.addr + "|" + String(f.addr_type) + "|" + f.name;
        char* copy = strdup(tag.c_str());
        lv_obj_add_event_cb(btn, ble_device_clicked_cb, LV_EVENT_CLICKED, copy);
        lv_obj_add_event_cb(btn, free_user_data_cb, LV_EVENT_DELETE, copy);
    }
    set_busy(false);
}

static void ble_scan_btn_cb(lv_event_t*) { ble_scan(); }

// Tap su un dispositivo: connessione + accoppiamento (Just Works). Il
// dispositivo viene ricordato in [bt] device e le chiavi in [bt] bond.
static void ble_device_clicked_cb(lv_event_t* e) {
    if (is_busy() || !DhSettings::btEnabled()) return;
    const char* tag = (const char*)lv_event_get_user_data(e);
    if (!tag) return;
    String t(tag);  // copia: lv_obj_clean libera tag
    int p1 = t.indexOf('|'), p2 = t.indexOf('|', p1 + 1);
    String addr = t.substring(0, p1);
    uint8_t type = (uint8_t)t.substring(p1 + 1, p2).toInt();
    String name = t.substring(p2 + 1);

    set_busy(true);
    lv_obj_clean(bt_list);
    lv_list_add_text(bt_list, ("Accoppiamento con " + (name.length() ? name : addr) + "...").c_str());
    pump(3);

    String msg;
    DhBle::pair(addr, type, name, msg);
    lv_obj_clean(bt_list);
    lv_list_add_text(bt_list, msg.c_str());
    g_last_ble_device = msg;
    set_busy(false);
}

// =====================================================================
// Collegamenti con DhSettings
// =====================================================================
static void settings_wifi_scan() {
    go_home();
    lv_tabview_set_act(home_tabview, 0, LV_ANIM_OFF);
    do_wifi_scan(false);
}

static void settings_bt_scan() {
    go_home();
    lv_tabview_set_act(home_tabview, 1, LV_ANIM_OFF);
    ble_scan();
}

static String info_text() {
    String t;
    t += "Loader: " LOADER_VERSION "\n";
    t += "Configurazione: " + DhConfig::statusText() + "\n";
    t += "Chip: " + String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()) + "\n";
    t += "Flash: " + String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB\n";
    t += "RAM interna libera: " + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) + " KB";
    // Minimo dall'avvio: è un contatore, come la memoria libera. NON usare qui
    // heap_caps_get_largest_free_block(): percorre tutto l'heap con gli interrupt
    // disabilitati, e questa schermata si aggiorna ogni secondo (se l'heap è
    // danneggiato la passata non finisce e scatta l'Interrupt WDT).
    t += " (minimo dall'avvio " + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024) + " KB)\n";
    t += "PSRAM libera: " + String(ESP.getFreePsram() / 1024) + " KB\n";
    t += "Uptime: " + String(millis() / 1000) + " s\n";
    t += "MAC WiFi: " + DhSettings::macString(ESP_MAC_WIFI_STA) + "\n";
    t += "MAC BLE: " + DhSettings::macString(ESP_MAC_BT) + "\n";
    t += "Dispositivi BT ricordati: " + String((int)DhBle::devices().size()) + ", chiavi: " + String(DhBle::bondCount());
    if (g_last_ble_device.length()) t += "\nUltima operazione BT: " + g_last_ble_device;
    return t;
}

// =====================================================================
// Home: barra di stato + tab WiFi/Bluetooth
// =====================================================================
static String status_chunk(const char* name, int level) {
    // level: 0 spento (grigio), 1 acceso non connesso (giallo), 2 operativo (verde)
    const char* col = level == 2 ? "3ddc84" : level == 1 ? "f5c542" : "5a5a60";
    return String("#") + col + " " + name + "#";
}

static void refresh_home() {
    bool wifi_on = DhSettings::wifiEnabled();
    bool wifi_conn = WiFi.status() == WL_CONNECTED;
    int wl = !wifi_on ? 0 : wifi_conn ? 2 : 1;
    int vl = !DhVpn::isEnabled() ? 0 : DhVpn::isConnected() ? 2 : 1;
    String st = status_chunk(LV_SYMBOL_WIFI, wl) + "  " +
                status_chunk(LV_SYMBOL_BLUETOOTH, DhSettings::btEnabled() ? 2 : 0) + "  " +
                status_chunk("VPN", vl);
    int sl = DhSettings::storageLevel();  // -1: build senza microSD
    if (sl >= 0) st += "  " + status_chunk(LV_SYMBOL_SD_CARD, sl);
    DhSettings::setLabelIfChanged(home_status_label, st);

    String wst;
    if (!wifi_on) wst = "WiFi disattivato (Impostazioni > Rete)";
    else if (wifi_conn) wst = "Connesso a " + WiFi.SSID() + " - " + WiFi.localIP().toString();
    else wst = "Non connesso";
    DhSettings::setLabelIfChanged(wifi_tab_status, wst);
    set_btn_enabled(wifi_scan_btn, wifi_on);

    DhSettings::setLabelIfChanged(bt_tab_status, DhSettings::btEnabled() ? String("Bluetooth attivo")
                                                     : String("Bluetooth disattivato (Impostazioni > Rete)"));
    set_btn_enabled(bt_scan_btn, DhSettings::btEnabled());
}

static void home_timer_cb(lv_timer_t*) {
    if (!is_busy() && lv_scr_act() == scr_home) refresh_home();
}

static lv_obj_t* make_tab_toolbar(lv_obj_t* tab, lv_event_cb_t scan_cb, lv_obj_t** btn_out, lv_obj_t** status_out) {
    lv_obj_set_style_pad_all(tab, 8, 0);
    lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);

    *btn_out = DhSettings::makeButton(tab, LV_SYMBOL_REFRESH " Scansiona", scan_cb);
    lv_obj_align(*btn_out, LV_ALIGN_TOP_LEFT, 0, 0);

    *status_out = lv_label_create(tab);
    lv_label_set_long_mode(*status_out, LV_LABEL_LONG_DOT);
    lv_obj_set_width(*status_out, 280);
    lv_obj_set_style_text_color(*status_out, DhSettings::COLOR_MUTED, 0);
    lv_obj_align(*status_out, LV_ALIGN_TOP_RIGHT, 0, 12);

    lv_obj_t* list = lv_list_create(tab);
    lv_obj_set_size(list, lv_pct(100), 316);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
    return list;
}

static lv_obj_t* build_home() {
    lv_obj_t* scr = lv_obj_create(NULL);
    DhSettings::styleScreen(scr);
    DhSettings::makeHeader(scr, "Rete", nullptr);

    home_status_label = lv_label_create(scr);
    lv_label_set_recolor(home_status_label, true);
    lv_label_set_text(home_status_label, "");
    lv_obj_align(home_status_label, LV_ALIGN_TOP_RIGHT, -80, 16);

    lv_obj_t* gear = DhSettings::makeButton(scr, LV_SYMBOL_SETTINGS, open_settings_cb);
    lv_obj_align(gear, LV_ALIGN_TOP_RIGHT, -8, 6);

    home_tabview = lv_tabview_create(scr, LV_DIR_TOP, 44);
    lv_obj_set_size(home_tabview, 480, 424);
    lv_obj_align(home_tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t* tab_wifi = lv_tabview_add_tab(home_tabview, LV_SYMBOL_WIFI " WiFi");
    lv_obj_t* tab_bt = lv_tabview_add_tab(home_tabview, LV_SYMBOL_BLUETOOTH " Bluetooth");
    // Niente swipe tra i tab: si scontrerebbe con lo scroll verticale delle liste.
    lv_obj_clear_flag(lv_tabview_get_content(home_tabview), LV_OBJ_FLAG_SCROLLABLE);

    wifi_list = make_tab_toolbar(tab_wifi, wifi_scan_btn_cb, &wifi_scan_btn, &wifi_tab_status);
    bt_list = make_tab_toolbar(tab_bt, ble_scan_btn_cb, &bt_scan_btn, &bt_tab_status);
    return scr;
}

// =====================================================================
// Protocollo seriale — ricezione
// =====================================================================
static void send_status() {
    JsonDocument doc;
    doc["type"] = "status";
    JsonObject w = doc["wifi"].to<JsonObject>();
    w["enabled"] = DhSettings::wifiEnabled();
    w["connected"] = WiFi.status() == WL_CONNECTED;
    if (WiFi.status() == WL_CONNECTED) {
        w["ssid"] = WiFi.SSID();
        w["ip"] = WiFi.localIP().toString();
        w["rssi"] = WiFi.RSSI();
    }
    JsonObject b = doc["bt"].to<JsonObject>();
    b["enabled"] = DhSettings::btEnabled();
    b["mac"] = DhSettings::macString(ESP_MAC_BT);
    JsonArray bdev = b["devices"].to<JsonArray>();
    for (const auto& d : DhBle::devices()) {
        JsonObject o = bdev.add<JsonObject>();
        o["addr"] = d.addr;
        o["name"] = d.name;
        o["bonded"] = d.bonded;
    }
    JsonObject v = doc["vpn"].to<JsonObject>();
    v["enabled"] = DhVpn::isEnabled();
    v["state"] = DhVpn::stateText();
    v["error"] = DhVpn::lastError();
    v["hostname"] = DhVpn::hostname();
    v["ip"] = DhVpn::vpnIp();
    v["key"] = DhVpn::authKeyMasked();
    JsonArray routes = v["routes"].to<JsonArray>();
    DhVpn::Route r[8];
    int nr = DhVpn::routes(r, 8);
    for (int i = 0; i < nr; i++) {
        JsonObject o = routes.add<JsonObject>();
        o["network"] = r[i].network;
        o["via"] = r[i].via_name;
        o["via_ip"] = r[i].via_ip;
        o["online"] = r[i].via_online;
    }
    JsonArray peers = v["peers"].to<JsonArray>();
    DhVpn::Peer p;
    for (int i = 0; i < DhVpn::peerCount(); i++) {
        if (!DhVpn::peer(i, p)) continue;
        JsonObject o = peers.add<JsonObject>();
        o["hostname"] = p.hostname;
        o["ip"] = p.ip;
        o["online"] = p.online;
        o["direct"] = p.direct;
    }
    send_json(doc);
}

static void process_line(const String& line) {
    JsonDocument doc;
    if (deserializeJson(doc, line)) return;  // riga non valida, ignorata
    // Configurazione e file della microSD (config_*, storage_*, file_*): vedi PROTOCOL.md.
    if (DhConfigSerial::handle(doc, Serial)) return;

    String cmd = doc["cmd"] | "";
    if (cmd == "scan") {
        // Richiesta esplicita dalla web UI: se il WiFi era spento lo accendiamo.
        if (!DhSettings::wifiEnabled()) DhSettings::setWifiEnabled(true);
        do_wifi_scan(true);
    } else if (cmd == "connect") {
        String ssid = doc["ssid"] | "";
        String password = doc["password"] | "";
        if (ssid.length() > 0) attempt_connect(ssid, password, true);
    } else if (cmd == "status") {
        send_status();
    } else if (cmd == "vpn_config") {
        // Campi tutti opzionali: {"cmd":"vpn_config","auth_key":"tskey-...","hostname":"...","enabled":true}
        JsonDocument out;
        out["type"] = "vpn_config_result";
        bool ok = true;
        if (doc["auth_key"].is<const char*>()) {
            if (!DhVpn::setAuthKey(doc["auth_key"].as<String>())) {
                ok = false;
                out["error"] = "auth_key non valida (min 16 caratteri, senza spazi)";
            }
        }
        if (ok && doc["hostname"].is<const char*>()) DhVpn::setHostname(doc["hostname"].as<String>());
        if (ok && doc["enabled"].is<bool>()) DhVpn::setEnabled(doc["enabled"].as<bool>());
        out["ok"] = ok;
        send_json(out);
        DhSettings::refresh();
    } else if (cmd == "vpn_forget") {
        DhVpn::forgetIdentity();
        JsonDocument out;
        out["type"] = "vpn_forget_result";
        out["ok"] = true;
        send_json(out);
    } else if (cmd == "hello") {
        send_hello();
    }
}

// =====================================================================
// Setup / loop
// =====================================================================
void setup() {
    // Righe lunghe dalla web UI (config_replace, blocchi di file): buffer di
    // ricezione ampio, altrimenti mentre LVGL disegna i byte in arrivo si perdono.
    Serial.setRxBufferSize(16384);
    Serial.begin(115200);
    delay(300);
    WiFi.persistent(false);  // prima di ogni uso del WiFi: niente credenziali nella NVS del driver

    display_init();
    touch_init();
    // Tema scuro nativo: liste, tastiera, interruttori e msgbox coerenti con lo sfondo nero.
    lv_theme_t* th = lv_theme_default_init(lv_disp_get_default(), lv_palette_main(LV_PALETTE_BLUE),
                                           lv_palette_main(LV_PALETTE_TEAL), true, &dh_font_14);
    lv_disp_set_theme(lv_disp_get_default(), th);

    // RAM interna: le malloc() generiche oltre i 512 byte vanno in PSRAM
    // (default del core: solo oltre 4 KB). Stringhe, JSON, configurazione,
    // dati dei widget e della VPN non occupano più la RAM interna, che resta
    // per WiFi, controller Bluetooth e stack dei task. Dopo display_init():
    // le strutture del pannello restano dove il driver le ha messe. Chi ha
    // bisogno di RAM interna (DMA, interrupt) la chiede esplicitamente.
    heap_caps_malloc_extmem_enable(512);

    // Configurazione prima delle impostazioni (che ne leggono gli interruttori).
    // Il loader è generico: nessun identificativo, accetta il file di qualsiasi
    // display (è lì proprio per prepararlo).
    DhConfig::Options co;
    co.backend = LOADER_CONFIG_SD ? DhConfig::Backend::Sd : DhConfig::Backend::Nvs;
    co.sd_cs = BSP_SD_CS;
    co.sd_sck = BSP_SD_CLK;
    co.sd_miso = BSP_SD_MISO;
    co.sd_mosi = BSP_SD_MOSI;
    co.device_id = "";
    co.defaults_ini = "";
    DhConfig::begin(co);
    DhConfig::importLegacyNvs();

    DhSettings::Config cfg;
    cfg.vpn = &g_vpn_backend;
    cfg.info_subtitle = "Loader " LOADER_VERSION ", memoria, indirizzi MAC";
    cfg.info_text = info_text;
    cfg.on_wifi_scan = settings_wifi_scan;
    cfg.on_bt_scan = settings_bt_scan;
    cfg.on_bt_changed = on_bt_changed;
    cfg.on_exit = go_home;
    DhSettings::begin(cfg);

    scr_home = build_home();
    lv_scr_load(scr_home);
    pump(5);

    // Rete: WiFi (riconnessione automatica alla rete salvata), BLE, VPN.
    DhSettings::startWifiIfEnabled();
    DhSettings::startBtIfEnabled();
    DhVpn::begin(default_vpn_hostname().c_str(), "", false);

    lv_timer_create(home_timer_cb, 1000, NULL);
    refresh_home();

    send_hello();
    if (DhSettings::wifiEnabled()) do_wifi_scan(false);  // popola subito la lista a schermo
}

void loop() {
    lvgl_tick_update();
    lv_timer_handler();
    DhBle::loop();  // copia nella configurazione gli accoppiamenti nuovi/cancellati
    if (DhSettings::wifiEnabled()) DhWifi::loop();  // riconnessione / passaggio tra reti note

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n') {
            process_line(serial_buffer);
            serial_buffer = "";
        } else if (c != '\r') {
            serial_buffer += c;
            if (serial_buffer.length() > 16384) serial_buffer = "";  // protezione da righe senza fine
        }
    }
    delay(5);
}
