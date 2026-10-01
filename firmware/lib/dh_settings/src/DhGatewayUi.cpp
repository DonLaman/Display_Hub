#include "DhGatewayUi.h"

#include <vector>

#include "dh_fonts.h"

using namespace DhSettings;

namespace DhGatewayUi {
namespace {

enum class View { None, List, Detail, Scan, Device };

GatewayBackend* g_be = nullptr;
View g_view = View::None;
lv_obj_t* g_scr = nullptr;      // schermata attuale (creata al volo, eliminata quando si cambia)
lv_obj_t* g_col = nullptr;      // colonna dei contenuti (scorre)
lv_obj_t* g_status = nullptr;   // riga di stato sotto l'intestazione
lv_timer_t* g_timer = nullptr;

std::vector<GatewayInfo> g_list;
GatewayInfo g_gw;
std::vector<GatewayDevice> g_known, g_found;
String g_id, g_err, g_sig, g_pend_addr, g_pend_name;
bool g_scanning = false;

const uint32_t DETAIL_REFRESH_MS = 8000;   // aggiornamento automatico del dettaglio
const uint32_t SCAN_POLL_MS = 1500;        // lettura dei dispositivi trovati

// ---------------------------------------------------------------- utilita'
void enter_list(bool fetch);
void enter_detail(const String& id, bool fetch);
void enter_scan();
void enter_device(const String& addr, const String& name);

void stop_timer() {
    if (g_timer) { lv_timer_del(g_timer); g_timer = nullptr; }
}
void tick_cb(lv_timer_t*);
void start_timer(uint32_t ms) {
    stop_timer();
    g_timer = lv_timer_create(tick_cb, ms, nullptr);
}

void say(const String& text, bool error = false) {
    if (!g_status) return;
    lv_label_set_text(g_status, text.c_str());
    lv_obj_set_style_text_color(g_status, error ? lv_color_hex(0xE57373) : COLOR_MUTED, 0);
}

// Chiamata di rete in corso: messaggio visibile PRIMA (la chiamata blocca per un attimo).
void busy_begin(const char* msg) {
    setBusy(true);
    say(msg);
    lv_refr_now(NULL);
}
void busy_end() { setBusy(false); }

char* dup(const String& s) { return strdup(s.c_str()); }
void free_cb(lv_event_t* e) { free(lv_event_get_user_data(e)); }

String shown_name(const String& name, const String& addr) { return name.length() ? name : (addr.length() ? addr : String("(senza nome)")); }

String rssi_text(int rssi) { return String(rssi) + " dBm"; }

// Cambio schermata: la nuova si carica prima, la vecchia si elimina dopo (non si puo' eliminare quella attiva).
void go(lv_obj_t* scr) {
    lv_obj_t* old = g_scr;
    g_scr = scr;
    lv_scr_load(scr);
    if (old && old != scr) lv_obj_del_async(old);
}

// Schermata con intestazione (titolo + Indietro), riga di stato e colonna dei contenuti.
lv_obj_t* make_screen(const String& title, lv_event_cb_t back_cb) {
    lv_obj_t* scr = lv_obj_create(NULL);
    styleScreen(scr);
    lv_obj_t* t = makeHeader(scr, title.c_str(), back_cb);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);   // un nome lungo non deve finire sui pulsanti
    lv_obj_set_size(t, 200, lv_font_get_line_height(&dh_font_20));   // i puntini scattano solo con l'altezza di UNA riga
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);

    g_status = lv_label_create(scr);
    lv_label_set_long_mode(g_status, LV_LABEL_LONG_DOT);
    lv_obj_set_size(g_status, 464, 20);
    lv_obj_set_style_text_color(g_status, COLOR_MUTED, 0);
    lv_obj_align(g_status, LV_ALIGN_TOP_LEFT, 8, 56);
    lv_label_set_text(g_status, "");

    g_col = lv_obj_create(scr);
    lv_obj_set_size(g_col, 464, 392);
    lv_obj_align(g_col, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(g_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_col, 0, 0);
    lv_obj_set_style_pad_all(g_col, 0, 0);
    lv_obj_set_style_pad_row(g_col, 10, 0);
    lv_obj_set_flex_flow(g_col, LV_FLEX_FLOW_COLUMN);
    return scr;
}

// Riga a scheda, come quelle delle Impostazioni (icona, titolo, sottotitolo).
lv_obj_t* make_card(lv_obj_t* parent, const char* symbol, const String& title, const String& subtitle,
                    lv_event_cb_t cb, char* user_data) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, lv_pct(100), 76);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);
    if (user_data) lv_obj_add_event_cb(card, free_cb, LV_EVENT_DELETE, user_data);

    lv_obj_t* icon = lv_label_create(card);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &dh_font_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

    lv_obj_t* t = lv_label_create(card);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_size(t, 360, lv_font_get_line_height(&dh_font_20));   // UNA riga: senza altezza fissa va a capo e sfora la scheda
    lv_label_set_text(t, title.c_str());
    lv_obj_set_style_text_font(t, &dh_font_20, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 44, 0);

    lv_obj_t* st = lv_label_create(card);
    lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
    lv_obj_set_size(st, 360, lv_font_get_line_height(&dh_font_14));
    lv_label_set_text(st, subtitle.c_str());
    lv_obj_set_style_text_color(st, COLOR_MUTED, 0);
    lv_obj_align(st, LV_ALIGN_BOTTOM_LEFT, 44, 0);

    if (cb) {
        lv_obj_t* arrow = lv_label_create(card);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -2, 0);
    }
    return card;
}

// Riquadro di testo (info, messaggi).
lv_obj_t* make_note(lv_obj_t* parent, const String& text, bool boxed) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_text(l, text.c_str());
    if (boxed) {
        lv_obj_set_style_bg_color(l, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(l, 10, 0);
        lv_obj_set_style_pad_all(l, 12, 0);
        lv_obj_set_style_text_color(l, lv_color_white(), 0);
    } else {
        lv_obj_set_style_text_color(l, COLOR_MUTED, 0);
    }
    return l;
}

lv_obj_t* make_btn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, int w, uint32_t color = 0) {
    lv_obj_t* b = makeButton(parent, text, cb);
    lv_obj_set_size(b, w, 48);
    if (color) lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    return b;
}

lv_obj_t* make_btn_row(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 48);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    return row;
}

// Esce dalle schermate del gateway e torna alla radice delle Impostazioni.
void leave() {
    stop_timer();
    lv_obj_t* old = g_scr;
    g_scr = nullptr; g_col = nullptr; g_status = nullptr;
    g_view = View::None;
    DhSettings::open();
    if (old) lv_obj_del_async(old);
}

const GatewayDevice* find_known(const String& addr) {
    for (const auto& d : g_known) if (d.addr == addr) return &d;
    return nullptr;
}

// ================================================================ ELENCO DEI GATEWAY
void list_back_cb(lv_event_t*) { if (!busy()) leave(); }
void list_refresh_cb(lv_event_t*) { if (!busy()) enter_list(true); }
void gw_card_cb(lv_event_t* e) {
    if (busy()) return;
    String id((const char*)lv_event_get_user_data(e));
    enter_detail(id, true);
}

void populate_list() {
    lv_obj_clean(g_col);
    if (g_err.length()) {
        say(g_err, true);
        make_note(g_col, "Non riesco a leggere l'elenco dal server. Controlla che il server sia raggiungibile e riprova con Aggiorna.", false);
        return;
    }
    if (g_list.empty()) {
        say("Nessun gateway trovato");
        make_note(g_col, "Un gateway compare qui dopo aver scaricato la configurazione dal server (lo fa ogni 30 secondi): "
                         "accendilo, collegalo al WiFi e imposta il server (pannello USB della web UI), poi tocca Aggiorna.", false);
        return;
    }
    say(String(g_list.size()) + " gateway - tocca per i dettagli");
    for (const auto& g : g_list) {
        String sub = g.online ? String("online - ") + g.ip : String("offline");
        if (g.online && g.bt_connected && g.target_name.length()) sub += String(" - ") + g.target_name;
        make_card(g_col, LV_SYMBOL_AUDIO, g.name.length() ? g.name : g.id, sub, gw_card_cb, dup(g.id));
    }
}

void enter_list(bool fetch) {
    stop_timer();
    lv_obj_t* scr = make_screen("Gateway Bluetooth", list_back_cb);
    lv_obj_t* btn = makeButton(scr, LV_SYMBOL_REFRESH " Aggiorna", list_refresh_cb);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8, 6);
    g_view = View::List;
    go(scr);
    if (fetch) {
        busy_begin("Ricerca dei gateway...");
        std::vector<GatewayInfo> out;
        String err;
        bool ok = g_be->list(out, err);
        busy_end();
        if (ok) { g_list = out; g_err = ""; }
        else g_err = err.length() ? err : String("server non raggiungibile");
    }
    populate_list();
}

// ================================================================ DETTAGLIO DEL GATEWAY
void detail_back_cb(lv_event_t*) { if (!busy()) enter_list(false); }
void detail_refresh_cb(lv_event_t*) { if (!busy()) enter_detail(g_id, true); }
void detail_scan_cb(lv_event_t*) { if (!busy()) enter_scan(); }
void known_card_cb(lv_event_t* e) {
    if (busy()) return;
    String tag((const char*)lv_event_get_user_data(e));   // "indirizzo|nome"
    int p = tag.indexOf('|');
    enter_device(tag.substring(0, p), tag.substring(p + 1));
}

String info_text(const GatewayInfo& g) {
    String t = String("Nome: ") + (g.name.length() ? g.name : g.id) + "\nID sul server: " + g.id;
    if (!g.online) return t + "\nStato: non raggiungibile" + (g.error.length() ? String(" (") + g.error + ")" : String(""));
    t += String("\nMAC: ") + (g.mac.length() ? g.mac : String("-")) + "\nIP: " + g.ip;
    t += String("\nWiFi: ") + (g.wifi_ssid.length() ? g.wifi_ssid : String("-"));
    if (g.wifi_rssi) t += String(" (") + rssi_text(g.wifi_rssi) + ")";
    t += String("\nFirmware: ") + (g.version.length() ? g.version : String("-"));
    if (g.volume >= 0) t += String("\nVolume: ") + g.volume + "%";
    String cassa = g.target_name.length() ? g.target_name : g.target_addr;
    t += String("\nCassa: ") + (cassa.length() ? cassa + (g.bt_connected ? " (connessa)" : " (non connessa)") : String("nessuna"));
    return t;
}

String detail_signature() {
    String s = info_text(g_gw);
    for (const auto& d : g_known) s += String("|") + d.addr + d.name + (d.connected ? "c" : "") + (d.selected ? "s" : "");
    return s;
}

void populate_detail() {
    lv_obj_clean(g_col);
    if (g_gw.online) say(String("Online - ") + g_gw.ip);
    else say(g_gw.error.length() ? String("Offline - ") + g_gw.error : String("Offline"), true);

    make_note(g_col, info_text(g_gw), true);
    lv_obj_t* row = make_btn_row(g_col);
    lv_obj_t* scan = make_btn(row, LV_SYMBOL_BLUETOOTH " Cerca dispositivi", detail_scan_cb, 236);
    if (!g_gw.online) lv_obj_add_state(scan, LV_STATE_DISABLED);   // la scansione la fa il gateway
    make_btn(row, LV_SYMBOL_REFRESH " Aggiorna", detail_refresh_cb, 218);

    lv_obj_t* h = make_note(g_col, "Dispositivi conosciuti", false);
    lv_obj_set_style_text_font(h, &dh_font_20, 0);
    if (g_known.empty()) {
        make_note(g_col, g_gw.online ? "Nessun dispositivo conosciuto: usa Cerca dispositivi per associarne uno."
                                     : "Nessun dispositivo conosciuto.", false);
    }
    for (const auto& d : g_known) {
        String sub = d.addr + (d.connected ? "  -  connesso" : (d.selected ? "  -  in uso, non connesso" : ""));
        make_card(g_col, LV_SYMBOL_BLUETOOTH, shown_name(d.name, d.addr), sub, known_card_cb, dup(d.addr + "|" + d.name));
    }
    g_sig = detail_signature();
}

bool fetch_detail(bool quiet) {
    if (!quiet) busy_begin("Aggiorno...");
    GatewayInfo gw;
    std::vector<GatewayDevice> known;
    String err;
    bool ok = g_be->info(g_id, gw, known, err);
    if (!quiet) busy_end();
    if (!ok) {
        // il server non risponde: si tiene l'ultimo stato noto e si dice che e' vecchio
        g_gw.online = false;
        g_gw.error = err.length() ? err : String("server non raggiungibile");
        return false;
    }
    g_gw = gw;
    g_known = known;
    return true;
}

void enter_detail(const String& id, bool fetch) {
    stop_timer();
    if (id != g_id) {                       // gateway diverso: si parte da quello che dice l'elenco (nome, stato)
        g_gw = GatewayInfo();
        g_known.clear();
        for (const auto& g : g_list) if (g.id == id) { g_gw = g; break; }
    }
    g_id = id;
    if (g_gw.id.length() == 0) { g_gw.id = id; g_gw.name = id; }
    lv_obj_t* scr = make_screen(g_gw.name.length() ? g_gw.name : id, detail_back_cb);
    g_view = View::Detail;
    go(scr);
    if (fetch) fetch_detail(false);
    populate_detail();
    start_timer(DETAIL_REFRESH_MS);
}

// ================================================================ CERCA DISPOSITIVI
void scan_back_cb(lv_event_t*) { if (!busy()) enter_detail(g_id, true); }
void scan_again_cb(lv_event_t*) { if (!busy()) enter_scan(); }

void do_connect_pending() {
    busy_begin("Connessione...");
    String err;
    bool ok = g_be->connect(g_id, g_pend_addr, g_pend_name, err);
    busy_end();
    if (!ok) { showToast(String("Non riuscito: ") + (err.length() ? err : String("errore sconosciuto"))); return; }
    showToast(String("Connessione a ") + shown_name(g_pend_name, g_pend_addr) + " avviata: ci vuole qualche secondo.");
    enter_detail(g_id, true);
}

void found_card_cb(lv_event_t* e) {
    if (busy()) return;
    String tag((const char*)lv_event_get_user_data(e));
    int p = tag.indexOf('|');
    g_pend_addr = tag.substring(0, p);
    g_pend_name = tag.substring(p + 1);
    showConfirm("Associa dispositivo", (String("Associare e connettere ") + shown_name(g_pend_name, g_pend_addr) + "?").c_str(), do_connect_pending);
}

void populate_scan() {
    lv_obj_clean(g_col);
    if (g_found.empty()) {
        make_note(g_col, g_scanning ? "Nessun dispositivo ancora. Metti la cassa in modalita' abbinamento."
                                    : "Nessun dispositivo trovato. Controlla che la cassa sia accesa e in modalita' abbinamento, poi Ripeti.", false);
        return;
    }
    for (const auto& d : g_found) {
        String sub = d.addr + "  -  " + rssi_text(d.rssi) + (d.known ? "  -  conosciuto" : "");
        make_card(g_col, LV_SYMBOL_BLUETOOTH, shown_name(d.name, ""), sub, found_card_cb, dup(d.addr + "|" + d.name));
    }
}

void scan_status() {
    if (g_scanning) say(String("Scansione in corso... ") + g_found.size() + " trovati");
    else say(g_found.empty() ? String("Scansione conclusa: nessun dispositivo")
                             : String("Scansione conclusa: ") + g_found.size() + " - tocca per associare");
}

void enter_scan() {
    stop_timer();
    lv_obj_t* scr = make_screen("Cerca dispositivi", scan_back_cb);
    lv_obj_t* btn = makeButton(scr, LV_SYMBOL_REFRESH " Ripeti", scan_again_cb);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -8, 6);
    g_view = View::Scan;
    g_found.clear();
    g_sig = "";
    go(scr);
    busy_begin("Avvio della scansione...");
    String err;
    bool ok = g_be->scanStart(g_id, err);
    busy_end();
    if (!ok) {
        g_scanning = false;
        say(String("Scansione non avviata: ") + (err.length() ? err : String("gateway non raggiungibile")), true);
        populate_scan();
        return;
    }
    g_scanning = true;
    scan_status();
    populate_scan();
    start_timer(SCAN_POLL_MS);
}

// ================================================================ DISPOSITIVO
void device_back_cb(lv_event_t*) { if (!busy()) enter_detail(g_id, false); }

void do_connect_cb(lv_event_t*) {
    if (busy()) return;
    do_connect_pending();   // g_pend_* sono gia' quelli del dispositivo aperto
}
void do_disconnect_cb(lv_event_t*) {
    if (busy()) return;
    busy_begin("Disconnessione...");
    String err;
    bool ok = g_be->disconnect(g_id, err);
    busy_end();
    if (!ok) { showToast(String("Non riuscito: ") + (err.length() ? err : String("errore sconosciuto"))); return; }
    showToast("Disconnesso");
    enter_detail(g_id, true);
}
void do_forget() {
    busy_begin("Dissocio...");
    String err;
    bool ok = g_be->forget(g_id, g_pend_addr, err);
    busy_end();
    if (!ok) { showToast(String("Non riuscito: ") + (err.length() ? err : String("errore sconosciuto"))); return; }
    showToast(shown_name(g_pend_name, g_pend_addr) + " dissociato");
    enter_detail(g_id, true);
}
void forget_cb(lv_event_t*) {
    if (busy()) return;
    showConfirm("Dissocia dispositivo",
                (String("Togliere ") + shown_name(g_pend_name, g_pend_addr) + " dall'elenco? Se e' in uso viene disconnesso.").c_str(), do_forget);
}

void enter_device(const String& addr, const String& name) {
    stop_timer();
    g_pend_addr = addr;
    g_pend_name = name;
    const GatewayDevice* d = find_known(addr);
    bool connected = d && d->connected, selected = d && d->selected;
    lv_obj_t* scr = make_screen(shown_name(name, addr), device_back_cb);
    g_view = View::Device;
    go(scr);
    say(connected ? String("Connesso") : (selected ? String("In uso, non connesso") : String("Conosciuto, non in uso")));
    String t = String("Nome: ") + (name.length() ? name : String("(senza nome)")) + "\nIndirizzo: " + addr +
               "\nStato: " + (connected ? "connesso" : (selected ? "in uso, non connesso" : "non in uso")) +
               "\nGateway: " + (g_gw.name.length() ? g_gw.name : g_id);
    make_note(g_col, t, true);
    lv_obj_t* row = make_btn_row(g_col);
    if (connected) make_btn(row, LV_SYMBOL_CLOSE " Disconnetti", do_disconnect_cb, 224);
    else {
        lv_obj_t* c = make_btn(row, LV_SYMBOL_OK " Connetti", do_connect_cb, 224);
        if (!g_gw.online) lv_obj_add_state(c, LV_STATE_DISABLED);
    }
    make_btn(row, LV_SYMBOL_TRASH " Dissocia", forget_cb, 224, 0xB3261E);
    make_note(g_col, "Dissocia toglie il dispositivo dall'elenco dei conosciuti; per riusarlo andra' associato di nuovo con Cerca dispositivi.", false);
}

// ================================================================ timer
void tick_cb(lv_timer_t*) {
    if (busy() || !g_scr || lv_scr_act() != g_scr) return;
    if (g_view == View::Scan) {
        bool scanning = false;
        std::vector<GatewayDevice> found;
        String err;
        if (!g_be->scanPoll(g_id, scanning, found, err)) {
            say(String("Gateway non raggiungibile: ") + err, true);
            return;
        }
        String sig;
        for (const auto& d : found) sig += d.addr + d.name + String(d.rssi / 5) + (d.known ? "k" : "");
        bool changed = sig != g_sig || scanning != g_scanning;
        g_found = found;
        g_scanning = scanning;
        scan_status();
        if (changed) { g_sig = sig; populate_scan(); }
        if (!scanning) stop_timer();
    } else if (g_view == View::Detail) {
        String before = g_sig;
        fetch_detail(true);
        if (detail_signature() != before) populate_detail();   // solo se e' cambiato qualcosa (ricostruire fa perdere lo scroll)
    }
}

}  // namespace

void begin(GatewayBackend* backend) { g_be = backend; }

void open() {
    if (!g_be) return;
    if (g_scr && lv_scr_act() != g_scr) {   // resto di una visita precedente
        stop_timer();
        lv_obj_del_async(g_scr);
        g_scr = nullptr; g_col = nullptr; g_status = nullptr;
    }
    g_id = "";
    enter_list(true);
}

bool isOpen() { return g_scr && lv_scr_act() == g_scr; }

}  // namespace DhGatewayUi
