// Impostazioni > Gateway Bluetooth sul PC: LVGL vero, DhSettings.h vero, finto backend.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "lvgl.h"
#include "DhSettings.h"
#include "DhGatewayUi.h"
#include "dh_fonts.h"
using namespace DhSettings;
namespace DhSettings { extern String last_toast, confirm_title, confirm_text; extern int root_opened; extern void (*confirm_yes)(); extern lv_obj_t* root_screen; }

#define W 480
#define H 480
static uint16_t fb[W * H];
static lv_color_t dbuf[W * 40];
static int ok_n = 0, fail_n = 0;
#define CHECK(cond, ...) do { if (cond) { ok_n++; printf("OK   "); } else { fail_n++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)
static void flush_cb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
    for (int y = a->y1; y <= a->y2; y++) for (int x = a->x1; x <= a->x2; x++) { if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = c->full; c++; }
    lv_disp_flush_ready(d);
}
static void settle() { for (int i = 0; i < 8; i++) { lv_tick_inc(10); lv_timer_handler(); } }
static void advance(int ms) { for (int t = 0; t < ms; t += 100) { lv_tick_inc(100); lv_timer_handler(); } }
static void shot(const char* file) {
    memset(fb, 0, sizeof(fb)); lv_obj_invalidate(lv_scr_act()); settle();
    FILE* f = fopen(file, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { unsigned p = fb[i]; unsigned char px[3] = {(unsigned char)((p >> 11 & 31) * 255 / 31), (unsigned char)((p >> 5 & 63) * 255 / 63), (unsigned char)((p & 31) * 255 / 31)}; fwrite(px, 1, 3, f); }
    fclose(f);
}
// Tutte le etichette che contengono il testo, in ordine.
static void find_all(lv_obj_t* o, const char* needle, std::vector<lv_obj_t*>& out) {
    if (lv_obj_check_type(o, &lv_label_class)) { const char* t = lv_label_get_text(o); if (t && strstr(t, needle)) out.push_back(o); }
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(o); i++) find_all(lv_obj_get_child(o, i), needle, out);
}
static lv_obj_t* find_text(lv_obj_t* o, const char* needle) { std::vector<lv_obj_t*> v; find_all(o, needle, v); return v.empty() ? nullptr : v[0]; }
static bool has_text(const char* needle) { return find_text(lv_scr_act(), needle) != nullptr; }
static lv_obj_t* column();
// Il pulsante o la scheda che CONTIENE un testo: si salta il testo libero (note) che sta solo nella colonna.
static lv_obj_t* clickable(const char* needle) {
    std::vector<lv_obj_t*> v; find_all(lv_scr_act(), needle, v);
    for (lv_obj_t* l : v) {
        lv_obj_t* p = l;
        while (p && p != lv_scr_act() && p != column() && !lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) p = lv_obj_get_parent(p);
        if (p && p != lv_scr_act() && p != column() && lv_obj_has_flag(p, LV_OBJ_FLAG_CLICKABLE)) return p;
    }
    return nullptr;
}
static bool disabled(const char* needle) { lv_obj_t* o = clickable(needle); return o && lv_obj_has_state(o, LV_STATE_DISABLED); }
static bool g_missed = false;
static void click(const char* needle) { lv_obj_t* o = clickable(needle); if (o) lv_event_send(o, LV_EVENT_CLICKED, NULL); else { g_missed = true; printf("     (nessun pulsante/scheda con \"%s\" nella schermata)\n", needle); } settle(); }
static lv_obj_t* column() {
    lv_obj_t* s = lv_scr_act();
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(s); i++) { lv_obj_t* c = lv_obj_get_child(s, i); if (lv_obj_get_width(c) == 464 && lv_obj_get_height(c) == 392) return c; }
    return nullptr;
}
// le "schede" sono le righe alte 76 px (il contenitore dei pulsanti e' un oggetto qualunque: non conta)
static int cards() { lv_obj_t* c = column(); int n = 0; for (uint32_t i = 0; c && i < lv_obj_get_child_cnt(c); i++) { lv_obj_t* o = lv_obj_get_child(c, i); if (lv_obj_has_flag(o, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_height(o) == 76) n++; } return n; }
static String status_text() { lv_obj_t* s = lv_scr_act(); for (uint32_t i = 0; i < lv_obj_get_child_cnt(s); i++) { lv_obj_t* c = lv_obj_get_child(s, i); if (lv_obj_check_type(c, &lv_label_class) && lv_obj_get_width(c) == 464) return lv_label_get_text(c); } return ""; }

struct Fake : GatewayBackend {
    std::vector<GatewayInfo> gws; bool list_ok = true;
    std::map<std::string, GatewayInfo> infos; std::map<std::string, std::vector<GatewayDevice>> knowns; bool info_ok = true;
    bool scan_ok = true, scanning = true; int polls = 0; std::vector<GatewayDevice> found;
    bool conn_ok = true, disc_ok = true, forget_ok = true;
    int n_list = 0, n_info = 0, n_scan = 0, n_poll = 0, n_conn = 0, n_disc = 0, n_forget = 0;
    String last_id, last_addr, last_name;
    bool list(std::vector<GatewayInfo>& out, String& err) override { n_list++; if (!list_ok) { err = "server non raggiungibile"; return false; } out = gws; return true; }
    bool info(const String& id, GatewayInfo& gw, std::vector<GatewayDevice>& known, String& err) override {
        n_info++; last_id = id; if (!info_ok) { err = "timeout"; return false; }
        gw = infos[id.c_str()]; known = knowns[id.c_str()]; return true; }
    bool scanStart(const String& id, String& err) override { n_scan++; last_id = id; polls = 0; if (!scan_ok) { err = "gateway non raggiungibile"; return false; } return true; }
    bool scanPoll(const String&, bool& sc, std::vector<GatewayDevice>& f, String&) override { n_poll++; polls++; sc = polls < 3; f = polls >= 2 ? found : std::vector<GatewayDevice>(); return true; }
    bool connect(const String& id, const String& addr, const String& name, String& err) override { n_conn++; last_id = id; last_addr = addr; last_name = name; if (!conn_ok) { err = "gateway non raggiungibile"; return false; } return true; }
    bool disconnect(const String& id, String& err) override { n_disc++; last_id = id; if (!disc_ok) { err = "x"; return false; } return true; }
    bool forget(const String& id, const String& addr, String& err) override { n_forget++; last_id = id; last_addr = addr; if (!forget_ok) { err = "x"; return false; } return true; }
};
static GatewayDevice dev(const char* a, const char* n, int rssi = 0, bool conn = false, bool sel = false, bool known = false) { GatewayDevice d; d.addr = a; d.name = n; d.rssi = rssi; d.connected = conn; d.selected = sel; d.known = known; return d; }

int main() {
    lv_init();
    static lv_disp_draw_buf_t buf; lv_disp_draw_buf_init(&buf, dbuf, NULL, W * 40);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd); dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flush_cb; dd.draw_buf = &buf;
    lv_disp_t* disp = lv_disp_drv_register(&dd);
    lv_disp_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL), true, &dh_font_14));

    Fake be;
    GatewayInfo s; s.id = "salotto"; s.name = "Salotto"; s.online = true; s.ip = "192.168.1.50"; s.mac = "24:6F:28:AA:BB:CC"; s.version = "1.0.0";
    s.wifi_ssid = "CasaWiFi"; s.wifi_rssi = -55; s.volume = 60; s.bt_connected = true; s.target_addr = "aa:bb:cc:dd:ee:01"; s.target_name = "Cassa Salotto"; s.uptime_s = 321;
    GatewayInfo c; c.id = "camera"; c.name = "camera"; c.error = "Connection refused";
    GatewayInfo k; k.id = "cucina"; k.name = "Cucina"; k.online = true; k.ip = "192.168.1.51"; k.mac = "24:6F:28:11:22:33"; k.version = "1.0.0"; k.volume = 40;
    be.gws = {s, c, k};
    be.infos["salotto"] = s; be.infos["camera"] = c; be.infos["cucina"] = k;
    const char* longname = "Cassa del salotto con un nome davvero molto molto lungo che non deve uscire dalla riga";
    be.knowns["salotto"] = {dev("aa:bb:cc:dd:ee:01", "Cassa Salotto", 0, true, true, true), dev("aa:bb:cc:dd:ee:03", longname, 0, false, false, true)};
    be.knowns["camera"] = {dev("aa:bb:cc:dd:ee:09", "Vecchia cassa", 0, false, true, true)};
    be.found = {dev("aa:bb:cc:dd:ee:02", "", -80), dev("aa:bb:cc:dd:ee:01", "Cassa Salotto", -50, false, false, true), dev("aa:bb:cc:dd:ee:05", "TV Soggiorno", -60)};

    DhGatewayUi::begin(&be);
    printf("== Elenco dei gateway ==\n");
    DhGatewayUi::open(); settle();
    CHECK(DhGatewayUi::isOpen() && be.n_list == 1, "open(): schermata del gateway attiva, un solo giro al server");
    CHECK(cards() == 3 && has_text("Salotto") && has_text("Cucina") && has_text("camera"), "tre gateway nell'elenco (%d schede)", cards());
    CHECK(has_text("online - 192.168.1.50 - Cassa Salotto") && has_text("offline") && has_text("online - 192.168.1.51"), "per ognuno: online/offline, IP e cassa in uso");
    CHECK(status_text() == "3 gateway - tocca per i dettagli", "stato: \"%s\"", status_text().c_str());
    shot("/tmp/gw_list.ppm");
    be.list_ok = false; click("Aggiorna");
    CHECK(be.n_list == 2 && cards() == 0 && has_text("Non riesco a leggere") && status_text() == "server non raggiungibile", "server non raggiungibile: messaggio chiaro, nessuna scheda");
    be.list_ok = true; click("Aggiorna");
    CHECK(cards() == 3, "Aggiorna: l'elenco torna");

    printf("\n== Dettaglio ==\n");
    click("Salotto");
    CHECK(be.n_info == 1 && be.last_id == "salotto", "tocco su Salotto: chiede le info di 'salotto'");
    CHECK(has_text("MAC: 24:6F:28:AA:BB:CC") && has_text("IP: 192.168.1.50") && has_text("WiFi: CasaWiFi (-55 dBm)") && has_text("Firmware: 1.0.0") && has_text("Volume: 60%"),
          "info: MAC, IP, WiFi, firmware, volume");
    CHECK(has_text("ID sul server: salotto") && has_text("Cassa: Cassa Salotto (connessa)"), "info: id sul server e cassa connessa");
    CHECK(cards() == 2 && has_text("aa:bb:cc:dd:ee:01  -  connesso"), "dispositivi conosciuti: 2, il primo connesso");
    CHECK(status_text() == "Online - 192.168.1.50", "stato: \"%s\"", status_text().c_str());
    bool inside = true; lv_obj_t* col = column(); lv_area_t ac; lv_obj_get_coords(col, &ac);
    for (uint32_t i = 0; i < lv_obj_get_child_cnt(col); i++) { lv_area_t a; lv_obj_get_coords(lv_obj_get_child(col, i), &a); if (a.x1 < ac.x1 || a.x2 > ac.x2) inside = false; }
    CHECK(inside, "nessuna scheda esce dalla colonna");
    lv_obj_t* longcard = clickable("aa:bb:cc:dd:ee:03"); lv_obj_t* lt = longcard ? find_text(longcard, "Cassa del salotto") : nullptr;
    CHECK(lt && lv_obj_get_height(lt) < 30, "il nome lunghissimo resta su una riga (altezza %d)", lt ? (int)lv_obj_get_height(lt) : -1);
    shot("/tmp/gw_detail.ppm");

    printf("\n== Cerca dispositivi ==\n");
    CHECK(clickable("Cerca dispositivi") && !disabled("Cerca dispositivi"), "gateway online: Cerca dispositivi attivo");
    click("Cerca dispositivi");
    CHECK(be.n_scan == 1 && be.last_id == "salotto" && has_text("Scansione in corso"), "parte la scansione sul gateway giusto");
    advance(1600);
    CHECK(be.n_poll == 1 && cards() == 0 && has_text("Metti la cassa in modalita'"), "primo giro: ancora nulla, suggerimento");
    advance(1600);
    CHECK(cards() == 3 && status_text().length() && strstr(status_text().c_str(), "3 trovati"), "secondo giro: 3 trovati (%s)", status_text().c_str());
    CHECK(has_text("(senza nome)") && has_text("TV Soggiorno") && has_text("-50 dBm  -  conosciuto"), "elenco: senza nome, nome, segnale e 'conosciuto'");
    advance(1600);
    CHECK(strstr(status_text().c_str(), "Scansione conclusa: 3"), "scansione conclusa: \"%s\"", status_text().c_str());
    int polls = be.n_poll; advance(6000);
    CHECK(be.n_poll == polls, "finita la scansione il timer si ferma (nessun altro giro)");
    shot("/tmp/gw_scan.ppm");
    click("TV Soggiorno");
    CHECK(confirm_title == "Associa dispositivo" && strstr(confirm_text.c_str(), "TV Soggiorno") && confirm_yes, "tocco su un trovato: chiede conferma con il nome");
    int info_before = be.n_info;
    confirm_yes(); settle();
    CHECK(be.n_conn == 1 && be.last_id == "salotto" && be.last_addr == "aa:bb:cc:dd:ee:05" && be.last_name == "TV Soggiorno", "confermato: connect(salotto, indirizzo, nome)");
    CHECK(strstr(last_toast.c_str(), "Connessione a TV Soggiorno avviata") && be.n_info == info_before + 1 && has_text("Dispositivi conosciuti"), "avviso e ritorno al dettaglio con info aggiornate");
    be.conn_ok = false; click("Cerca dispositivi"); advance(3300); click("(senza nome)");
    confirm_yes(); settle();
    CHECK(strstr(last_toast.c_str(), "Non riuscito: gateway non raggiungibile") && has_text("Cerca dispositivi") && !has_text("Dispositivi conosciuti"), "connect fallita: avviso con il motivo, si resta sulla scansione");
    be.conn_ok = true;
    be.scan_ok = false; click("Ripeti");
    CHECK(has_text("Scansione non avviata: gateway non raggiungibile") && status_text().length(), "scansione non avviata: errore visibile");
    be.scan_ok = true;
    click("Indietro");
    CHECK(has_text("Dispositivi conosciuti"), "Indietro dalla scansione: torna al dettaglio");

    printf("\n== Dispositivo ==\n");
    click("aa:bb:cc:dd:ee:01");
    CHECK(has_text("Indirizzo: aa:bb:cc:dd:ee:01") && has_text("Stato: connesso") && has_text("Disconnetti") && has_text("Dissocia") && !has_text("Connetti"), "dispositivo connesso: info, Disconnetti e Dissocia");
    shot("/tmp/gw_device.ppm");
    click("Disconnetti");
    CHECK(be.n_disc == 1 && be.last_id == "salotto" && last_toast == "Disconnesso", "Disconnetti: comando e avviso");
    click("aa:bb:cc:dd:ee:03");
    CHECK(has_text("Stato: non in uso") && has_text("Connetti") && !has_text("Disconnetti"), "dispositivo non in uso: Connetti");
    click("Connetti");
    CHECK(be.n_conn == 3 && be.last_addr == "aa:bb:cc:dd:ee:03", "Connetti: connect con l'indirizzo del dispositivo aperto");
    click("aa:bb:cc:dd:ee:03");
    click("Dissocia");
    CHECK(confirm_title == "Dissocia dispositivo" && strstr(confirm_text.c_str(), "Togliere") && be.n_forget == 0, "Dissocia chiede conferma, non agisce subito");
    confirm_yes(); settle();
    CHECK(be.n_forget == 1 && be.last_addr == "aa:bb:cc:dd:ee:03" && strstr(last_toast.c_str(), "dissociato"), "confermato: forget(salotto, indirizzo)");
    be.forget_ok = false; click("aa:bb:cc:dd:ee:03"); click("Dissocia"); confirm_yes(); settle();
    CHECK(be.n_forget == 2 && last_toast == "Non riuscito: x", "dissocia fallita: avviso con il motivo");
    be.forget_ok = true;

    printf("\n== Gateway spento e blocchi ==\n");
    click("Indietro"); click("Indietro");
    click("camera");
    CHECK(status_text() == "Offline - Connection refused" && has_text("Stato: non raggiungibile (Connection refused)"), "gateway spento: stato offline con il motivo");
    CHECK(disabled("Cerca dispositivi"), "gateway spento: Cerca dispositivi disattivato");
    click("Vecchia cassa");
    CHECK(has_text("In uso, non connesso") || status_text() == "In uso, non connesso", "dispositivo in uso ma non connesso");
    CHECK(disabled("Connetti"), "gateway spento: Connetti disattivato");
    click("Indietro"); click("Indietro");
    setBusy(true); int calls = be.n_info; click("Salotto"); setBusy(false);
    CHECK(be.n_info == calls, "con un'operazione in corso i tocchi vengono ignorati");

    printf("\n== Aggiornamento automatico ==\n");
    click("Salotto");
    lv_obj_t* first = lv_obj_get_child(column(), 3);
    int before = be.n_info; advance(8200);
    CHECK(be.n_info == before + 1 && lv_obj_get_child(column(), 3) == first, "ogni 8 s rilegge, e se non e' cambiato non ricostruisce (lo scroll resta)");
    be.infos["salotto"].bt_connected = false; be.knowns["salotto"][0].connected = false; advance(8200);
    CHECK(has_text("aa:bb:cc:dd:ee:01  -  in uso, non connesso") && has_text("Cassa: Cassa Salotto (non connessa)"), "se la cassa si disconnette, la schermata lo mostra da sola");
    be.info_ok = false; advance(8200);
    CHECK(status_text() == "Offline - timeout", "server che non risponde: stato offline, nessun blocco");
    be.info_ok = true;

    printf("\n== Uscita ==\n");
    click("Indietro");
    CHECK(cards() == 3, "Indietro dal dettaglio: l'elenco");
    int r0 = root_opened; click("Indietro"); advance(200);
    CHECK(root_opened == r0 + 1 && !DhGatewayUi::isOpen() && lv_scr_act() == root_screen, "Indietro dall'elenco: radice delle Impostazioni, schermate del gateway chiuse");
    int ci = be.n_info, cp = be.n_poll; advance(20000);
    CHECK(be.n_info == ci && be.n_poll == cp, "fuori dalle schermate nessun timer chiama piu' il server");
    DhGatewayUi::open(); settle(); click("Salotto"); lv_scr_load(root_screen); settle();
    DhGatewayUi::open(); settle();
    CHECK(DhGatewayUi::isOpen() && cards() == 3, "riaprire dopo essere usciti senza passare da Indietro: nessun residuo");

    CHECK(!g_missed, "nessun tocco del test e' finito nel vuoto (pulsante o scheda non trovati)");
    printf("\n%d ok, %d falliti\n", ok_n, fail_n);
    return fail_n ? 1 : 0;
}
