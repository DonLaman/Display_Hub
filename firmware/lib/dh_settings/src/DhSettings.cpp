// DhSettings — vedi DhSettings.h
#include "DhSettings.h"
#include "DhGatewayUi.h"

#include <WiFi.h>

#include "DhBle.h"
#include "DhConfig.h"
#include "dh_fonts.h"
#include "DhWifi.h"

namespace DhSettings {

const lv_color_t COLOR_CARD = lv_color_hex(0x1C1C1E);
// Testo secondario: abbastanza chiaro da leggersi sul pannello (0x9A9AA0 era
// quasi invisibile sulle schede scure).
const lv_color_t COLOR_MUTED = lv_color_hex(0xC4C4CA);

namespace {

enum class Kind { None, Wifi, Bt, Vpn, Info, Storage, Gateway };


Config g_cfg;
bool g_wifi_enabled = true;
bool g_bt_enabled = false;
bool g_busy = false;

lv_obj_t* scr_settings = nullptr;
lv_obj_t* scr_network = nullptr;
lv_obj_t* scr_detail = nullptr;
lv_obj_t* scr_input = nullptr;

struct Row {
    lv_obj_t* card = nullptr;
    lv_obj_t* subtitle = nullptr;
    lv_obj_t* sw = nullptr;
};
Row row_wifi, row_bt, row_vpn, row_storage;

lv_obj_t* detail_title = nullptr;
lv_obj_t* detail_body = nullptr;
lv_obj_t* detail_text = nullptr;
lv_obj_t* detail_actions = nullptr;
Kind g_detail_kind = Kind::None;
lv_obj_t* g_detail_back_to = nullptr;

lv_obj_t* input_title = nullptr;
lv_obj_t* input_textarea = nullptr;
lv_obj_t* input_status = nullptr;
InputCallback g_input_ok = nullptr;
void (*g_input_back)() = nullptr;

bool g_vpn_verbose = false;

// Interruttori in [net] della configurazione (microSD, RAM o NVS: vedi DhConfig).
void save_flag(const char* key, bool value) { DhConfig::set("net", key, value ? "1" : "0"); }



void show(lv_obj_t* scr) {
    lv_scr_load(scr);
    refresh();
}

void set_switch(lv_obj_t* sw, bool on) {
    if (!sw) return;
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(sw, LV_STATE_CHECKED);
}

// ---------------------------------------------------------------- dettaglio

void open_detail(Kind kind, lv_obj_t* back_to);

String wifi_status_line() {
    if (!g_cfg.wifi_available) return g_cfg.wifi_unavailable_reason;
    if (!g_wifi_enabled) return "Disattivato";
    if (WiFi.status() == WL_CONNECTED) return "Connesso a " + WiFi.SSID() + " - " + WiFi.localIP().toString();
    return DhWifi::state() == DhWifi::State::Stopped ? String("Non connesso") : DhWifi::stateText();
}

String detail_text_for(Kind kind) {
    String t;
    switch (kind) {
        case Kind::Wifi: {
            bool conn = g_cfg.wifi_available && WiFi.status() == WL_CONNECTED;
            auto nets = DhConfig::getAll("wifi", "network");
            t += "Stato: " + wifi_status_line() + "\n";
            if (conn) {
                int rssi = WiFi.RSSI();
                t += "Rete: " + WiFi.SSID() + "\n";
                t += "IP: " + WiFi.localIP().toString() + "\n";
                t += "Gateway: " + WiFi.gatewayIP().toString() + "\n";
                t += "DNS: " + WiFi.dnsIP().toString() + "\n";
                t += "Segnale: " + String(rssi) + " dBm (" + String(signalPercent(rssi)) + "%)\n";
                t += "Canale: " + String(WiFi.channel()) + "\n";
            }
            t += "Reti salvate (in ordine di priorità): ";
            if (nets.empty()) t += "nessuna";
            for (size_t i = 0; i < nets.size(); i++) {
                String s, p;
                DhConfig::splitEntry(nets[i], s, p);
                t += "\n  " + String(i + 1) + ". " + s;
                if (conn && s == WiFi.SSID()) t += "  #3ddc84 " LV_SYMBOL_OK "#";
            }
            t += "\n";
            t += "MAC: " + macString(ESP_MAC_WIFI_STA);
            break;
        }
        case Kind::Bt: {
            t += "Stato: " + String(g_bt_enabled ? "Attivo" : "Disattivato") + "\n";
            t += "Nome: DisplayHub\n";
            t += "Modalità: Bluetooth Low Energy (l'ESP32-S3 non ha il Classic)\n";
            t += "MAC BLE: " + macString(ESP_MAC_BT) + "\n";
            auto devs = DhBle::devices();
            t += "Dispositivi ricordati: " + String(devs.empty() ? "nessuno" : "");
            for (const auto& d : devs) {
                t += "\n  " + d.name + "  " + d.addr;
                if (g_bt_enabled) t += d.bonded ? "  (accoppiato)" : "  (senza chiavi)";
            }
            if (!DhConfig::persistent()) t += "\n\nSenza microSD gli accoppiamenti valgono fino al riavvio.";
            break;
        }
        case Kind::Vpn:
            if (g_cfg.vpn) t = g_cfg.vpn->detailText();
            break;
        case Kind::Info:
            if (g_cfg.info_text) t = g_cfg.info_text();
            break;
        case Kind::Storage: {
            bool sd = DhConfig::backend() == DhConfig::Backend::Sd;
            t += "Stato: " + DhConfig::statusText() + "\n";
            String m = DhConfig::message();
            if (sd && m.length()) t += m + "\n";
            t += "Modifiche non ancora salvate: " + String(DhConfig::dirty() ? "sì" : "no") + "\n";
            uint32_t ls = DhConfig::lastSaveMs();
            t += "Ultimo salvataggio: " + (ls ? String((millis() - ls) / 1000) + " s fa" : String("-")) + "\n";
            if (sd) {
                t += "File: displayhub.txt (radice della microSD)\n\n";
                t += "Con la microSD la configurazione (reti, Bluetooth, VPN, server) resta dopo il "
                     "riavvio e si può modificare anche dal PC. Senza, vale fino al riavvio.\n"
                     "Prima di togliere la scheda usa \"Espelli\".";
            } else {
                t += "\nConfigurazione nella memoria interna del display (NVS).";
            }
            break;
        }
        default:
            break;
    }
    return t;
}

// Stato dell'archivio per icone e righe: -1 nessuna microSD prevista dalla build,
// 0 assente (solo RAM), 1 serve attenzione, 2 microSD in uso.
int storage_level() {
    if (DhConfig::backend() != DhConfig::Backend::Sd) return -1;
    switch (DhConfig::status()) {
        case dhcfg::CardStatus::Ready:            return DhConfig::dirty() ? 1 : 2;
        case dhcfg::CardStatus::Unusable:
        case dhcfg::CardStatus::AwaitingDecision: return 1;
        default:                                  return 0;
    }
}

void detail_back_cb(lv_event_t*) { show(g_detail_back_to ? g_detail_back_to : scr_network); }

void act_wifi_scan_cb(lv_event_t*) {
    if (!g_busy && g_cfg.on_wifi_scan) g_cfg.on_wifi_scan();
}
void act_wifi_reconf_cb(lv_event_t*) {
    if (!g_busy && g_cfg.on_wifi_reconfigure) g_cfg.on_wifi_reconfigure();
}
void act_wifi_reconnect_cb(lv_event_t*) {
    if (g_busy) return;
    DhWifi::reconnectNow();  // nuova scelta della rete migliore tra quelle note
    refresh();
}
void open_nets();
void act_wifi_nets_cb(lv_event_t*) {
    if (!g_busy) open_nets();
}
void open_addbt();
void act_bt_scan_cb(lv_event_t*) {
    if (g_busy) return;
    if (g_cfg.on_bt_scan) g_cfg.on_bt_scan();  // il loader usa la sua scheda di scansione
    else open_addbt();
}

// --- Archivio
dhcfg::CardStatus g_storage_built = dhcfg::CardStatus::NotSupported;  // stato per cui sono stati creati i pulsanti

void confirm_box(const char* title, const char* text, void (*on_yes)());
void do_reload() { DhConfig::reload(); }
void do_format() { DhConfig::format(); }
void act_storage_save_cb(lv_event_t*) { DhConfig::saveNow(); refresh(); }
void act_storage_eject_cb(lv_event_t*) { DhConfig::eject(); refresh(); }
void act_storage_reload_cb(lv_event_t*) {
    confirm_box("Ricarica dalla microSD", "Le modifiche non ancora salvate andranno perse.", do_reload);
}
void act_storage_format_cb(lv_event_t*) {
    confirm_box("Formatta microSD", "Tutto il contenuto della microSD verrà cancellato (FAT32).", do_format);
}
void build_storage_actions() {
    lv_obj_clean(detail_actions);
    g_storage_built = DhConfig::status();
    if (DhConfig::backend() == DhConfig::Backend::Nvs) {
        makeButton(detail_actions, LV_SYMBOL_SAVE " Salva ora", act_storage_save_cb);
        return;
    }
    switch (g_storage_built) {
        case dhcfg::CardStatus::Ready:
            makeButton(detail_actions, LV_SYMBOL_SAVE " Salva ora", act_storage_save_cb);
            makeButton(detail_actions, LV_SYMBOL_REFRESH " Ricarica", act_storage_reload_cb);
            makeButton(detail_actions, LV_SYMBOL_EJECT " Espelli", act_storage_eject_cb);
            break;
        case dhcfg::CardStatus::Unusable:
            makeButton(detail_actions, LV_SYMBOL_SD_CARD " Formatta", act_storage_format_cb);
            break;
        default:
            break;
    }
}

// "Dimentica <nome>": l'indirizzo è nella user_data (liberata alla distruzione
// del pulsante, cioè alla prossima apertura del dettaglio).
void free_str_cb(lv_event_t* e) { free(lv_event_get_user_data(e)); }
void act_bt_forget_cb(lv_event_t* e) {
    if (g_busy) return;
    const char* addr = (const char*)lv_event_get_user_data(e);
    if (!addr) return;
    DhBle::forget(String(addr));
    open_detail(Kind::Bt, g_detail_back_to);  // ricostruisce i pulsanti
}

void back_to_vpn_detail() { open_detail(Kind::Vpn, scr_network); }

void vpn_key_ok(const String& text) {
    if (g_cfg.vpn && g_cfg.vpn->setAuthKey(text)) back_to_vpn_detail();
    else setInputStatus("Chiave non valida (almeno 16 caratteri, senza spazi).");
}
void vpn_host_ok(const String& text) {
    if (g_cfg.vpn) g_cfg.vpn->setHostname(text);
    back_to_vpn_detail();
}
void act_vpn_key_cb(lv_event_t*) {
    openInput("Auth key Tailscale", "", false, 200,
              "Più comodo inviarla dalla web UI via USB, per non digitarla qui.", vpn_key_ok, back_to_vpn_detail);
}
void act_vpn_host_cb(lv_event_t*) {
    openInput("Nome sulla tailnet", g_cfg.vpn ? g_cfg.vpn->hostname() : String(""), false, 63,
              "Solo a-z, 0-9 e trattini: gli altri caratteri vengono adattati.", vpn_host_ok, back_to_vpn_detail);
}
void forget_confirm_cb(lv_event_t* e) {
    lv_obj_t* mbox = lv_event_get_current_target(e);
    const char* txt = lv_msgbox_get_active_btn_text(mbox);
    if (txt && strcmp(txt, "Conferma") == 0 && g_cfg.vpn) g_cfg.vpn->forgetIdentity();
    lv_msgbox_close(mbox);
}
void act_vpn_forget_cb(lv_event_t*) {
    static const char* btns[] = {"Annulla", "Conferma", ""};
    lv_obj_t* mbox = lv_msgbox_create(NULL, "Dimentica identità",
                                      "Il display si registrerà come NUOVO dispositivo sulla tailnet. "
                                      "Il vecchio andrà rimosso dalla console Tailscale.",
                                      btns, false);
    lv_obj_center(mbox);
    lv_obj_add_event_cb(mbox, forget_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
void act_vpn_verbose_cb(lv_event_t* e) {
    g_vpn_verbose = !g_vpn_verbose;
    if (g_cfg.vpn) g_cfg.vpn->setVerbose(g_vpn_verbose);
    lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), g_vpn_verbose ? "Log: dettagliati" : "Log: essenziali");
}

void open_detail(Kind kind, lv_obj_t* back_to) {
    g_detail_kind = kind;
    g_detail_back_to = back_to;
    const char* title = "";
    lv_obj_clean(detail_actions);
    switch (kind) {
        case Kind::Wifi:
            title = "WiFi";
            if (!g_cfg.wifi_available) break;
            makeButton(detail_actions, LV_SYMBOL_LIST " Reti salvate", act_wifi_nets_cb);
            if (g_wifi_enabled) makeButton(detail_actions, LV_SYMBOL_REFRESH " Riconnetti", act_wifi_reconnect_cb);
            if (g_cfg.on_wifi_scan) makeButton(detail_actions, LV_SYMBOL_WIFI " Scansiona reti", act_wifi_scan_cb);
            if (g_cfg.on_wifi_reconfigure) makeButton(detail_actions, g_cfg.wifi_reconfigure_label, act_wifi_reconf_cb);
            break;
        case Kind::Bt:
            title = "Bluetooth";
            if (g_bt_enabled) makeButton(detail_actions, LV_SYMBOL_PLUS " Aggiungi dispositivo", act_bt_scan_cb);
            for (const auto& d : DhBle::devices()) {
                char* addr = strdup(d.addr.c_str());
                String label = String(LV_SYMBOL_TRASH " ") + (d.name.length() ? d.name : d.addr);
                lv_obj_t* b = makeButton(detail_actions, label.c_str(), act_bt_forget_cb, addr);
                lv_obj_add_event_cb(b, free_str_cb, LV_EVENT_DELETE, addr);
            }
            break;
        case Kind::Vpn:
            title = "VPN Tailscale";
            makeButton(detail_actions, LV_SYMBOL_EDIT " Auth key", act_vpn_key_cb);
            makeButton(detail_actions, LV_SYMBOL_EDIT " Nome", act_vpn_host_cb);
            makeButton(detail_actions, g_vpn_verbose ? "Log: dettagliati" : "Log: essenziali", act_vpn_verbose_cb);
            makeButton(detail_actions, LV_SYMBOL_TRASH " Dimentica identità", act_vpn_forget_cb);
            break;
        case Kind::Info:
            title = "Informazioni";
            break;
        case Kind::Storage:
            title = "Archivio";
            build_storage_actions();
            break;
        default:
            break;
    }
    lv_label_set_text(detail_title, title);
    lv_label_set_text(detail_text, "");
    lv_obj_scroll_to_y(detail_body, 0, LV_ANIM_OFF);
    show(scr_detail);
}

// ---------------------------------------------------------------- reti salvate
// Elenco in ordine di priorità: su / giù / elimina, e "Aggiungi rete".
lv_obj_t* make_column(lv_obj_t* scr);

lv_obj_t* scr_nets = nullptr;
lv_obj_t* nets_col = nullptr;
String g_nets_sig;  // contenuto mostrato: si ricostruisce solo se cambia

lv_obj_t* scr_addnet = nullptr;
lv_obj_t* addnet_list = nullptr;
String g_add_ssid;

void rebuild_nets();
void rebuild_nets_async(void*) { rebuild_nets(); }

// user_data: "u|SSID", "d|SSID" o "x|SSID" (copiata, liberata con il pulsante)
void nets_action_cb(lv_event_t* e) {
    if (g_busy) return;
    String a((const char*)lv_event_get_user_data(e));
    String ssid = a.substring(2);
    if (a[0] == 'u') DhWifi::move(ssid, -1);
    else if (a[0] == 'd') DhWifi::move(ssid, +1);
    else DhWifi::remove(ssid);
    g_nets_sig = "";
    lv_async_call(rebuild_nets_async, nullptr);  // non eliminare il pulsante dentro il suo evento
}

lv_obj_t* small_btn(lv_obj_t* parent, const char* sym, char op, const String& ssid, bool enabled) {
    String tag = String(op) + "|" + ssid;
    char* copy = strdup(tag.c_str());
    lv_obj_t* b = makeButton(parent, sym, nets_action_cb, copy);
    lv_obj_add_event_cb(b, free_str_cb, LV_EVENT_DELETE, copy);
    lv_obj_set_size(b, 48, 40);
    if (!enabled) lv_obj_add_state(b, LV_STATE_DISABLED);
    return b;
}

void open_addnet();
void nets_add_cb(lv_event_t*) {
    if (g_busy) return;
    if (g_cfg.on_wifi_scan) g_cfg.on_wifi_scan();  // il loader usa la sua scheda di scansione
    else open_addnet();
}

void rebuild_nets() {
    auto nets = DhWifi::known();
    String cur = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String("");
    String sig = cur + "\n" + String(g_wifi_enabled);
    for (const auto& n : nets) sig += "\n" + n.ssid;
    if (sig == g_nets_sig) return;
    g_nets_sig = sig;

    lv_obj_clean(nets_col);
    if (nets.empty()) {
        lv_obj_t* l = lv_label_create(nets_col);
        lv_label_set_text(l, "Nessuna rete salvata.");
    }
    for (size_t i = 0; i < nets.size(); i++) {
        lv_obj_t* card = lv_obj_create(nets_col);
        lv_obj_set_size(card, lv_pct(100), 64);
        lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_text_color(card, lv_color_white(), 0);  // leggibile con qualsiasi tema
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* name = lv_label_create(card);
        lv_label_set_recolor(name, true);
        String t = String(i + 1) + ". " + nets[i].ssid;
        if (nets[i].ssid == cur) t += "  #3ddc84 " LV_SYMBOL_OK "#";
        lv_label_set_text(name, t.c_str());
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 250);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, 0);

        lv_obj_t* del = small_btn(card, LV_SYMBOL_TRASH, 'x', nets[i].ssid, true);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_t* down = small_btn(card, LV_SYMBOL_DOWN, 'd', nets[i].ssid, i + 1 < nets.size());
        lv_obj_align_to(down, del, LV_ALIGN_OUT_LEFT_MID, -6, 0);
        lv_obj_t* up = small_btn(card, LV_SYMBOL_UP, 'u', nets[i].ssid, i > 0);
        lv_obj_align_to(up, down, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
    lv_obj_t* add = makeButton(nets_col, LV_SYMBOL_PLUS " Aggiungi rete", nets_add_cb);
    if (!g_wifi_enabled) lv_obj_add_state(add, LV_STATE_DISABLED);

    lv_obj_t* hint = lv_label_create(nets_col);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, COLOR_MUTED, 0);
    lv_label_set_text(hint, "Il display sceglie da solo la rete salvata con il segnale migliore tra quelle "
                            "visibili; a parità di segnale vale quest'ordine. Se la rete cade passa alla "
                            "successiva (es. da casa all'hotspot del telefono).");
}

void open_nets() {
    g_nets_sig = "";
    rebuild_nets();
    show(scr_nets);
}

void nets_back_cb(lv_event_t*) {
    if (!g_busy) open_detail(Kind::Wifi, scr_network);
}

// --- aggiungi rete (firmware senza una scheda di scansione propria)
void back_to_addnet() { show(scr_addnet); }

void addnet_pw_ok(const String& pw) {
    DhWifi::add(g_add_ssid, pw, true);  // in cima: è quella appena scelta
    DhWifi::connectTo(g_add_ssid);
    open_nets();
}

void addnet_pick(const String& ssid) {
    g_add_ssid = ssid;
    openInput("Password per: " + ssid, "", true, 64, "Vuota per le reti aperte.", addnet_pw_ok, back_to_addnet);
}

void addnet_item_cb(lv_event_t* e) {
    if (g_busy) return;
    addnet_pick(String((const char*)lv_event_get_user_data(e)));
}

void addnet_ssid_ok(const String& ssid) {
    String s = ssid;
    s.trim();
    if (!s.length()) {
        setInputStatus("Scrivi il nome della rete.");
        return;
    }
    addnet_pick(s);
}

void addnet_hidden_cb(lv_event_t*) {
    if (g_busy) return;
    openInput("Nome della rete (SSID)", "", false, 32, "Per le reti nascoste, che non compaiono nella scansione.",
              addnet_ssid_ok, back_to_addnet);
}

void addnet_scan() {
    g_busy = true;
    DhWifi::pause();  // niente scansioni/tentativi automatici in parallelo
    lv_obj_clean(addnet_list);
    lv_list_add_text(addnet_list, "Scansione in corso...");
    lv_refr_now(NULL);  // mostra il testo prima della scansione (bloccante, 2-4 s)
    int n = WiFi.scanNetworks(false, false);
    lv_obj_clean(addnet_list);
    if (n < 0) lv_list_add_text(addnet_list, "Scansione non riuscita: riprova tra poco.");
    else if (n == 0) lv_list_add_text(addnet_list, "Nessuna rete trovata.");
    auto nets = DhWifi::known();
    for (int i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.length()) continue;
        bool dup = false;
        for (int j = 0; j < i; j++) dup = dup || WiFi.SSID(j) == ssid;  // più access point
        if (dup) continue;
        bool saved = false;
        for (const auto& k : nets) saved = saved || k.ssid == ssid;
        char line[120];
        snprintf(line, sizeof(line), "%s%s\n%d dBm (%d%%)%s", ssid.c_str(),
                 WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "  (aperta)" : "", WiFi.RSSI(i),
                 signalPercent(WiFi.RSSI(i)), saved ? "  - già salvata" : "");
        lv_obj_t* b = lv_list_add_btn(addnet_list, LV_SYMBOL_WIFI, line);
        char* copy = strdup(ssid.c_str());
        lv_obj_add_event_cb(b, addnet_item_cb, LV_EVENT_CLICKED, copy);
        lv_obj_add_event_cb(b, free_str_cb, LV_EVENT_DELETE, copy);
    }
    WiFi.scanDelete();
    DhWifi::resume();
    g_busy = false;
}

void addnet_scan_cb(lv_event_t*) {
    if (!g_busy) addnet_scan();
}

void open_addnet() {
    show(scr_addnet);
    addnet_scan();
}

void addnet_back_cb(lv_event_t*) {
    if (!g_busy) open_nets();
}

void build_nets() {
    scr_nets = lv_obj_create(NULL);
    styleScreen(scr_nets);
    makeHeader(scr_nets, "Reti salvate", nets_back_cb);
    nets_col = make_column(scr_nets);

    scr_addnet = lv_obj_create(NULL);
    styleScreen(scr_addnet);
    makeHeader(scr_addnet, "Aggiungi rete", addnet_back_cb);
    lv_obj_t* scan = makeButton(scr_addnet, LV_SYMBOL_REFRESH " Scansiona", addnet_scan_cb);
    lv_obj_align(scan, LV_ALIGN_TOP_LEFT, 8, 56);
    lv_obj_t* hidden = makeButton(scr_addnet, "Rete nascosta", addnet_hidden_cb);
    lv_obj_align(hidden, LV_ALIGN_TOP_RIGHT, -8, 56);
    addnet_list = lv_list_create(scr_addnet);
    lv_obj_set_size(addnet_list, 464, 360);
    lv_obj_align(addnet_list, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ---------------------------------------------------------------- righe

void switch_cb(lv_event_t* e) {
    lv_obj_t* sw = lv_event_get_target(e);
    Kind kind = (Kind)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (g_busy) {  // operazione bloccante in corso: annulla il cambio
        set_switch(sw, !on);
        return;
    }
    switch (kind) {
        case Kind::Wifi: setWifiEnabled(on); break;
        case Kind::Bt:   setBtEnabled(on); break;
        case Kind::Vpn:  if (g_cfg.vpn) g_cfg.vpn->setEnabled(on); break;
        default: break;
    }
    refresh();
}

void row_clicked_cb(lv_event_t* e) {
    if (g_busy) return;
    Kind kind = (Kind)(intptr_t)lv_event_get_user_data(e);
    if (kind == Kind::None) show(scr_network);            // "Rete"
    else if (kind == Kind::Gateway) DhGatewayUi::open();  // schermate in DhGatewayUi.cpp
    else open_detail(kind, lv_scr_act());
}

Row make_row(lv_obj_t* parent, const char* symbol, const char* name, Kind kind, bool with_switch) {
    Row row;
    row.card = lv_obj_create(parent);
    lv_obj_set_size(row.card, lv_pct(100), 76);
    lv_obj_set_style_bg_color(row.card, COLOR_CARD, 0);
    lv_obj_set_style_text_color(row.card, lv_color_white(), 0);  // leggibile con qualsiasi tema
    lv_obj_set_style_bg_opa(row.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row.card, 0, 0);
    lv_obj_set_style_radius(row.card, 10, 0);
    lv_obj_set_style_pad_all(row.card, 10, 0);
    lv_obj_clear_flag(row.card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row.card, row_clicked_cb, LV_EVENT_CLICKED, (void*)(intptr_t)kind);

    lv_obj_t* icon = lv_label_create(row.card);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &dh_font_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t* title = lv_label_create(row.card);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_font(title, &dh_font_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 44, 0);

    row.subtitle = lv_label_create(row.card);
    lv_label_set_text(row.subtitle, "");
    lv_obj_set_style_text_color(row.subtitle, COLOR_MUTED, 0);
    lv_label_set_long_mode(row.subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_width(row.subtitle, 290);
    lv_obj_align(row.subtitle, LV_ALIGN_BOTTOM_LEFT, 44, 0);

    if (with_switch) {
        // Il tap sull'interruttore NON arriva alla riga (LVGL 8 non propaga senza
        // EVENT_BUBBLE): interruttore = accendi/spegni, resto = dettaglio.
        row.sw = lv_switch_create(row.card);
        lv_obj_align(row.sw, LV_ALIGN_RIGHT_MID, -4, 0);
        lv_obj_add_event_cb(row.sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)kind);
    } else {
        lv_obj_t* arrow = lv_label_create(row.card);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -8, 0);
    }
    return row;
}

lv_obj_t* make_column(lv_obj_t* scr) {
    lv_obj_t* cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 464, 410);
    lv_obj_align(cont, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 10, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    return cont;
}

// ---------------------------------------------------------------- schermate

void settings_back_cb(lv_event_t*) {
    if (g_busy) return;
    if (g_cfg.on_exit) g_cfg.on_exit();
}
void network_back_cb(lv_event_t*) { show(scr_settings); }

void build_settings() {
    scr_settings = lv_obj_create(NULL);
    styleScreen(scr_settings);
    makeHeader(scr_settings, "Impostazioni", settings_back_cb);
    lv_obj_t* col = make_column(scr_settings);

    Row net = make_row(col, LV_SYMBOL_WIFI, "Rete", Kind::None, false);
    lv_label_set_text(net.subtitle, g_cfg.vpn ? "WiFi, Bluetooth, VPN" : "WiFi, Bluetooth");
    if (g_cfg.gateway) {
        Row gw = make_row(col, LV_SYMBOL_AUDIO, "Gateway Bluetooth", Kind::Gateway, false);
        lv_label_set_text(gw.subtitle, "Casse audio: cerca, associa, dissocia");
    }
    row_storage = make_row(col, LV_SYMBOL_SD_CARD, "Archivio", Kind::Storage, false);
    Row info = make_row(col, LV_SYMBOL_LIST, "Informazioni", Kind::Info, false);
    lv_label_set_text(info.subtitle, g_cfg.info_subtitle);
}

void build_network() {
    scr_network = lv_obj_create(NULL);
    styleScreen(scr_network);
    makeHeader(scr_network, "Rete", network_back_cb);
    lv_obj_t* col = make_column(scr_network);

    row_wifi = make_row(col, LV_SYMBOL_WIFI, "WiFi", Kind::Wifi, true);
    if (!g_cfg.wifi_available) lv_obj_add_state(row_wifi.sw, LV_STATE_DISABLED);
    row_bt = make_row(col, LV_SYMBOL_BLUETOOTH, "Bluetooth", Kind::Bt, true);
    if (g_cfg.vpn) row_vpn = make_row(col, LV_SYMBOL_SHUFFLE, "VPN Tailscale", Kind::Vpn, true);

    lv_obj_t* hint = lv_label_create(col);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, COLOR_MUTED, 0);
    lv_label_set_text(hint, g_cfg.vpn ? "Interruttore: accende/spegne. Tocca la riga per i dettagli.\n"
                                        "La VPN usa la connessione WiFi attiva (router o hotspot del telefono)."
                                      : "Interruttore: accende/spegne. Tocca la riga per i dettagli.");
}

void build_detail() {
    scr_detail = lv_obj_create(NULL);
    styleScreen(scr_detail);
    detail_title = makeHeader(scr_detail, "", detail_back_cb);

    detail_body = lv_obj_create(scr_detail);
    lv_obj_set_size(detail_body, 464, 410);
    lv_obj_align(detail_body, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(detail_body, COLOR_CARD, 0);
    lv_obj_set_style_text_color(detail_body, lv_color_white(), 0);  // leggibile con qualsiasi tema
    lv_obj_set_style_bg_opa(detail_body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(detail_body, 0, 0);
    lv_obj_set_style_radius(detail_body, 10, 0);
    lv_obj_set_style_pad_row(detail_body, 12, 0);
    lv_obj_set_flex_flow(detail_body, LV_FLEX_FLOW_COLUMN);

    detail_actions = lv_obj_create(detail_body);
    lv_obj_set_size(detail_actions, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(detail_actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(detail_actions, 0, 0);
    lv_obj_set_style_pad_all(detail_actions, 0, 0);
    lv_obj_set_style_pad_gap(detail_actions, 8, 0);
    lv_obj_set_flex_flow(detail_actions, LV_FLEX_FLOW_ROW_WRAP);

    detail_text = lv_label_create(detail_body);
    lv_label_set_recolor(detail_text, true);
    lv_label_set_long_mode(detail_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_text, lv_pct(100));
    lv_label_set_text(detail_text, "");
}

void input_back_cb(lv_event_t*) {
    if (g_busy) return;
    if (g_input_back) g_input_back();
}
void input_ok_cb(lv_event_t*) {
    if (g_busy) return;
    if (g_input_ok) g_input_ok(String(lv_textarea_get_text(input_textarea)));
}

void build_input() {
    scr_input = lv_obj_create(NULL);
    styleScreen(scr_input);

    lv_obj_t* back = makeButton(scr_input, LV_SYMBOL_LEFT " Indietro", input_back_cb);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_t* ok = makeButton(scr_input, LV_SYMBOL_OK " Conferma", input_ok_cb);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -8, 6);

    input_title = lv_label_create(scr_input);
    lv_label_set_long_mode(input_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(input_title, 460);
    lv_obj_set_style_text_align(input_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(input_title, &dh_font_20, 0);
    lv_obj_align(input_title, LV_ALIGN_TOP_MID, 0, 52);

    input_textarea = lv_textarea_create(scr_input);
    lv_textarea_set_one_line(input_textarea, true);
    lv_obj_set_width(input_textarea, 440);
    lv_obj_align(input_textarea, LV_ALIGN_TOP_MID, 0, 84);

    input_status = lv_label_create(scr_input);
    lv_label_set_long_mode(input_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(input_status, 440);
    lv_obj_set_style_text_align(input_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(input_status, LV_ALIGN_TOP_MID, 0, 130);

    lv_obj_t* kb = lv_keyboard_create(scr_input);
    lv_keyboard_set_textarea(kb, input_textarea);
    lv_obj_set_size(kb, 480, 250);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void refresh_timer_cb(lv_timer_t*) {
    if (!g_busy && isOpen()) refresh();
}

// ---------------------------------------------------------------- Aggiungi dispositivo BT
// (firmware principale: il loader ha la sua scheda di scansione, on_bt_scan)
lv_obj_t* scr_addbt = nullptr;
lv_obj_t* addbt_list = nullptr;
lv_obj_t* addbt_status = nullptr;

void addbt_back_cb(lv_event_t*) {
    if (!g_busy) open_detail(Kind::Bt, scr_network);
}

void addbt_item_cb(lv_event_t* e) {
    if (g_busy) return;
    String tag((const char*)lv_event_get_user_data(e));  // "indirizzo|tipo|nome"
    int p1 = tag.indexOf('|'), p2 = tag.indexOf('|', p1 + 1);
    String addr = tag.substring(0, p1), name = tag.substring(p2 + 1);
    uint8_t type = (uint8_t)tag.substring(p1 + 1, p2).toInt();
    g_busy = true;
    lv_label_set_text(addbt_status, ("Accoppiamento con " + (name.length() ? name : addr) + "...").c_str());
    lv_refr_now(NULL);  // l'accoppiamento blocca qualche secondo
    String msg;
    DhBle::pair(addr, type, name, msg);
    lv_label_set_text(addbt_status, msg.c_str());
    g_busy = false;
}

void addbt_scan() {
    if (!g_bt_enabled) return;
    g_busy = true;
    lv_obj_clean(addbt_list);
    lv_label_set_text(addbt_status, "Scansione in corso (4 s)...");
    lv_refr_now(NULL);
    auto found = DhBle::scan(4);
    auto known = DhBle::devices();
    lv_label_set_text(addbt_status, found.empty() ? "Nessun dispositivo trovato." : "Tocca un dispositivo per accoppiarlo.");
    for (const auto& f : found) {
        String mark;
        for (const auto& k : known)
            if (k.addr == f.addr) mark = k.bonded ? "  [accoppiato]" : "  [ricordato]";
        char line[140];
        snprintf(line, sizeof(line), "%s%s\n%s  (%d dBm)", f.name.length() ? f.name.c_str() : "(senza nome)",
                 mark.c_str(), f.addr.c_str(), f.rssi);
        lv_obj_t* b = lv_list_add_btn(addbt_list, LV_SYMBOL_BLUETOOTH, line);
        char* copy = strdup((f.addr + "|" + String(f.addr_type) + "|" + f.name).c_str());
        lv_obj_add_event_cb(b, addbt_item_cb, LV_EVENT_CLICKED, copy);
        lv_obj_add_event_cb(b, free_str_cb, LV_EVENT_DELETE, copy);
    }
    g_busy = false;
}

void addbt_scan_cb(lv_event_t*) {
    if (!g_busy) addbt_scan();
}

void build_addbt() {
    scr_addbt = lv_obj_create(NULL);
    styleScreen(scr_addbt);
    makeHeader(scr_addbt, "Aggiungi dispositivo", addbt_back_cb);
    lv_obj_t* scan = makeButton(scr_addbt, LV_SYMBOL_REFRESH " Scansiona", addbt_scan_cb);
    lv_obj_align(scan, LV_ALIGN_TOP_LEFT, 8, 56);
    addbt_status = lv_label_create(scr_addbt);
    lv_label_set_long_mode(addbt_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(addbt_status, 300);
    lv_obj_set_style_text_color(addbt_status, COLOR_MUTED, 0);
    lv_obj_align(addbt_status, LV_ALIGN_TOP_RIGHT, -8, 60);
    lv_label_set_text(addbt_status, "");
    addbt_list = lv_list_create(scr_addbt);
    lv_obj_set_size(addbt_list, 464, 360);
    lv_obj_align(addbt_list, LV_ALIGN_BOTTOM_MID, 0, -8);
}

void open_addbt() {
    show(scr_addbt);
    addbt_scan();  // subito una prima scansione
}

// ---------------------------------------------------------------- finestre e messaggi
lv_obj_t* g_confirm = nullptr;
void (*g_confirm_yes)() = nullptr;

void confirm_cb(lv_event_t* e) {
    lv_obj_t* mbox = lv_event_get_current_target(e);
    const char* txt = lv_msgbox_get_active_btn_text(mbox);
    bool yes = txt && strcmp(txt, "Conferma") == 0;
    void (*fn)() = g_confirm_yes;
    lv_msgbox_close(mbox);
    g_confirm = nullptr;
    if (yes && fn) fn();
    refresh();
}

void confirm_box(const char* title, const char* text, void (*on_yes)()) {
    static const char* btns[] = {"Annulla", "Conferma", ""};
    if (g_confirm) lv_msgbox_close(g_confirm);
    g_confirm_yes = on_yes;
    g_confirm = lv_msgbox_create(NULL, title, text, btns, false);
    lv_obj_center(g_confirm);
    lv_obj_add_event_cb(g_confirm, confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// Messaggio a comparsa in basso (sopra qualunque schermata), 4 s.
lv_obj_t* g_toast = nullptr;
uint32_t g_toast_until = 0;

void toast(const String& text) {
    if (!g_toast) {
        g_toast = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(g_toast, &dh_font_14, 0);
        lv_label_set_long_mode(g_toast, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(g_toast, 440);
        lv_obj_set_style_text_align(g_toast, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(g_toast, LV_OPA_90, 0);
        lv_obj_set_style_text_color(g_toast, lv_color_white(), 0);
        lv_obj_set_style_radius(g_toast, 10, 0);
        lv_obj_set_style_pad_all(g_toast, 10, 0);
        lv_obj_align(g_toast, LV_ALIGN_BOTTOM_MID, 0, -16);
    }
    lv_label_set_text(g_toast, text.c_str());
    lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_toast);
    g_toast_until = millis() + 4000;
}

// Finestra di scelta quando la microSD inserita contiene già una configurazione.
lv_obj_t* g_decision = nullptr;
uint32_t g_decision_since = 0;
bool g_decision_foreign = false;

String decision_text() {
    uint32_t el = (millis() - g_decision_since) / 1000;
    uint32_t left = el >= 15 ? 0 : 15 - el;
    String t = DhConfig::message() + ".\n\n";
    if (g_decision_foreign) {
        t += "Usarla cambierebbe la configurazione di questo display.\n\n"
             "Senza scelta entro " + String(left) + " s: Ignora (resta in RAM).";
    } else {
        t += "Unisci: tieni il file e aggiungi le modifiche fatte senza microSD.\n"
             "Usa microSD: solo il file. Sovrascrivi: solo lo stato attuale.\n\n"
             "Senza scelta entro " + String(left) + " s: Unisci.";
    }
    return t;
}

void decision_cb(lv_event_t* e) {
    lv_obj_t* mbox = lv_event_get_current_target(e);
    String txt = lv_msgbox_get_active_btn_text(mbox);
    dhcfg::Decision d = dhcfg::Decision::Merge;
    if (txt == "Unisci") d = dhcfg::Decision::Merge;
    else if (txt == "Usa microSD" || txt == "Usa comunque") d = dhcfg::Decision::UseCard;
    else if (txt == "Sovrascrivi") d = dhcfg::Decision::Overwrite;
    else if (txt == "Ignora") d = dhcfg::Decision::Ignore;
    lv_msgbox_close(mbox);
    g_decision = nullptr;
    DhConfig::decide(d);
}

void open_decision() {
    static const char* own_btns[] = {"Unisci", "Usa microSD", "Sovrascrivi", ""};
    static const char* foreign_btns[] = {"Ignora", "Usa comunque", "Sovrascrivi", ""};
    if (g_decision) lv_msgbox_close(g_decision);
    g_decision_foreign = DhConfig::pendingForeign();
    g_decision_since = millis();
    g_decision = lv_msgbox_create(NULL, "microSD inserita", decision_text().c_str(),
                                  g_decision_foreign ? foreign_btns : own_btns, false);
    lv_obj_set_width(g_decision, 440);
    lv_obj_center(g_decision);
    lv_obj_add_event_cb(g_decision, decision_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// Icona microSD (es. accanto all'ingranaggio del firmware), aggiornata qui.
lv_obj_t* g_storage_icon = nullptr;

// La configurazione è cambiata tutta (microSD inserita/unita, ricarica):
// interruttori WiFi/BT allineati al file. VPN, reti e accoppiamenti leggono
// già da soli la configurazione.
void apply_replaced_config() {
    bool w = DhConfig::getBool("net", "wifi_enabled", true);
    bool b = DhConfig::getBool("net", "bt_enabled", false);
    if (g_cfg.wifi_available && w != g_wifi_enabled) setWifiEnabled(w);
    if (b != g_bt_enabled) setBtEnabled(b);
}

void notify_timer_cb(lv_timer_t*) {
    dhcfg::Event e;
    while (DhConfig::pollEvent(e)) {
        switch (e.type) {
            case dhcfg::Event::DecisionNeeded:
                open_decision();
                break;
            case dhcfg::Event::ConfigReplaced:
                apply_replaced_config();
                break;
            case dhcfg::Event::Saved:
                break;  // troppo frequente per un messaggio
            default:
                if (DhConfig::backend() == DhConfig::Backend::Sd) toast(String(e.text.c_str()));
                break;
        }
    }
    if (g_decision) {
        if (DhConfig::status() != dhcfg::CardStatus::AwaitingDecision) {
            lv_msgbox_close(g_decision);  // scelta automatica o scheda tolta
            g_decision = nullptr;
        } else {
            lv_label_set_text(lv_msgbox_get_text(g_decision), decision_text().c_str());
        }
    }
    if (g_toast && g_toast_until && (int32_t)(millis() - g_toast_until) >= 0) {
        lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
        g_toast_until = 0;
    }
    if (g_storage_icon) {
        int lv = storage_level();
        if (lv < 0) {
            lv_obj_add_flag(g_storage_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(g_storage_icon, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_text_color(g_storage_icon,
                                        lv_color_hex(lv == 2 ? 0x3DDC84 : lv == 1 ? 0xF5C542 : 0x5A5A60), 0);
        }
    }
    // Pulsanti dell'Archivio da rifare se lo stato della scheda è cambiato.
    if (lv_scr_act() == scr_detail && g_detail_kind == Kind::Storage && DhConfig::status() != g_storage_built) {
        build_storage_actions();
    }
}

}  // namespace

// =================================================================== API

void showToast(const String& text) { toast(text); }
void showConfirm(const char* title, const char* text, void (*on_yes)()) { confirm_box(title, text, on_yes); }

void styleScreen(lv_obj_t* scr) {
    // Font con le lettere accentate, esplicito (ereditato dai figli): non si
    // dipende dal fatto che LVGL abbia letto LV_FONT_DEFAULT da lv_conf.h.
    lv_obj_set_style_text_font(scr, &dh_font_14, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

lv_obj_t* makeHeader(lv_obj_t* scr, const char* title, lv_event_cb_t back_cb) {
    if (back_cb) {
        lv_obj_t* back = makeButton(scr, LV_SYMBOL_LEFT " Indietro", back_cb);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 6);
    }
    lv_obj_t* t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &dh_font_20, 0);
    if (back_cb) lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
    else lv_obj_align(t, LV_ALIGN_TOP_LEFT, 12, 14);
    return t;
}

void setLabelIfChanged(lv_obj_t* label, const String& text) {
    // Niente ridisegno (e niente reset dello scroll) se il testo non è cambiato.
    if (label && strcmp(lv_label_get_text(label), text.c_str()) != 0) lv_label_set_text(label, text.c_str());
}

String macString(esp_mac_type_t type) {
    uint8_t m[6] = {0};
    esp_read_mac(m, type);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return String(buf);
}

int signalPercent(int rssi) {
    int pct = (rssi + 100) * 2;  // -100 dBm ≈ 0%, -50 dBm ≈ 100%
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

void begin(const Config& cfg) {
    g_cfg = cfg;
    DhGatewayUi::begin(cfg.gateway);
    // DhConfig::begin() va chiamata prima di questa funzione.
    g_wifi_enabled = DhConfig::getBool("net", "wifi_enabled", true);
    g_bt_enabled = DhConfig::getBool("net", "bt_enabled", false);
    if (!g_cfg.wifi_available) g_wifi_enabled = false;

    build_settings();
    build_network();
    build_detail();
    build_input();
    build_nets();
    build_addbt();
    lv_timer_create(refresh_timer_cb, 1000, NULL);
    // Sempre attivo (anche fuori dalle impostazioni): eventi della microSD,
    // finestra di scelta, messaggi a comparsa, icona.
    lv_timer_create(notify_timer_cb, 500, NULL);
}

void startWifiIfEnabled() {
    // DhWifi: reti note dalla configurazione, scelta della migliore, driver
    // senza memoria propria (persistent(false)). DhWifi::loop() nel loop dell'app.
    if (g_cfg.wifi_available && g_wifi_enabled) DhWifi::begin();
}

void startBtIfEnabled() {
    if (g_bt_enabled && !DhBle::active()) DhBle::begin("DisplayHub");
}

void open() { show(scr_settings); }

bool isOpen() {
    lv_obj_t* a = lv_scr_act();
    return a && (a == scr_settings || a == scr_network || a == scr_detail || a == scr_input || a == scr_nets ||
                 a == scr_addnet || a == scr_addbt || DhGatewayUi::isOpen());
}

bool wifiEnabled() { return g_wifi_enabled; }
bool btEnabled() { return g_bt_enabled; }

void setWifiEnabled(bool on) {
    if (!g_cfg.wifi_available) on = false;
    g_wifi_enabled = on;
    save_flag("wifi_enabled", on);
    if (on) {
        startWifiIfEnabled();
    } else {
        DhWifi::stop();
    }
    if (g_cfg.on_wifi_changed) g_cfg.on_wifi_changed(on);
}

void restart_now() {
    DhConfig::saveNow();  // l'interruttore acceso deve arrivare sulla microSD/NVS
    delay(300);
    ESP.restart();
}

void setBtEnabled(bool on) {
    g_bt_enabled = on;
    save_flag("bt_enabled", on);
    if (on) {
        String why;
        if (!DhBle::active() && !DhBle::canStart(&why)) {
            // Non si accende ora (il controller si bloccherebbe in un assert della
            // ROM): resta acceso nella configurazione e parte al prossimo avvio,
            // PRIMA di WiFi e VPN, quando la RAM interna c'è.
            DhConfig::saveNow();
            Serial.printf("[BT] Rimandato al prossimo avvio: %s\n", why.c_str());
            confirm_box("Bluetooth al prossimo avvio",
                        ("Adesso non c'è abbastanza memoria per accenderlo (" + why +
                         "). Resta attivo nelle impostazioni e partirà al prossimo avvio, prima di WiFi e VPN.\n\n"
                         "Riavviare ora?").c_str(),
                        restart_now);
            return;
        }
        startBtIfEnabled();
    } else if (DhBle::active()) {
        // L'applicazione ferma eventuali operazioni PRIMA dello spegnimento.
        if (g_cfg.on_bt_changed) g_cfg.on_bt_changed(false);
        DhBle::end();  // gli accoppiamenti restano nella configurazione
        return;
    }
    if (g_cfg.on_bt_changed) g_cfg.on_bt_changed(on);
}

void attachStorageIcon(lv_obj_t* label) {
    g_storage_icon = label;
    lv_label_set_text(label, LV_SYMBOL_SD_CARD);
    if (storage_level() < 0) lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
}

int storageLevel() { return storage_level(); }

void setBusy(bool b) { g_busy = b; }
bool busy() { return g_busy; }

void openInput(const String& title, const String& initial, bool password, uint16_t max_len,
               const char* hint, InputCallback on_ok, void (*on_back)()) {
    g_input_ok = on_ok;
    g_input_back = on_back;
    lv_label_set_text(input_title, title.c_str());
    lv_textarea_set_password_mode(input_textarea, password);
    lv_textarea_set_max_length(input_textarea, max_len);
    lv_textarea_set_text(input_textarea, initial.c_str());
    lv_label_set_text(input_status, hint ? hint : "");
    show(scr_input);
}

void setInputStatus(const String& text) {
    if (input_status) lv_label_set_text(input_status, text.c_str());
}

void refresh() {
    if (!scr_network) return;
    set_switch(row_wifi.sw, g_wifi_enabled);
    set_switch(row_bt.sw, g_bt_enabled);
    setLabelIfChanged(row_wifi.subtitle, wifi_status_line());
    setLabelIfChanged(row_bt.subtitle, !g_bt_enabled      ? String("Disattivato")
                                       : DhBle::active() ? "Attivo - " + macString(ESP_MAC_BT)
                                                         : String("Si attiverà al prossimo avvio"));
    if (g_cfg.vpn) {
        set_switch(row_vpn.sw, g_cfg.vpn->enabled());
        setLabelIfChanged(row_vpn.subtitle, g_cfg.vpn->summary());
    }
    if (row_storage.subtitle) setLabelIfChanged(row_storage.subtitle, DhConfig::statusText());
    if (lv_scr_act() == scr_nets) rebuild_nets();  // solo se elenco o rete attuale sono cambiati
    if (lv_scr_act() == scr_detail && g_detail_kind != Kind::None) {
        setLabelIfChanged(detail_text, detail_text_for(g_detail_kind));
    }
}

}  // namespace DhSettings
