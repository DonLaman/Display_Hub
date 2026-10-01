// DhWifi — vedi DhWifi.h
#include "DhWifi.h"

#include <WiFi.h>

#include "esp_wifi.h"

#include "DhConfig.h"
#include "DhWifiPlan.h"

namespace DhWifi {

namespace {

constexpr uint32_t kGraceMs = 3000;       // rete caduta: attesa prima di un giro
constexpr uint32_t kScanTimeoutMs = 10000;
constexpr uint32_t kTryTimeoutMs = 12000;
constexpr uint32_t kRetryMs = 10000;      // tra un giro fallito e il successivo

State s_state = State::Stopped;
std::vector<Net> s_plan;
size_t s_idx = 0;
uint32_t s_t0 = 0;
uint32_t s_wait_until = 0;
uint32_t s_rounds_failed = 0;
uint32_t s_round = 0;
uint32_t s_lost_since = 0;
String s_forced;
bool s_paused = false;

void try_current() {
    const Net& n = s_plan[s_idx];
    Serial.printf("[WiFi] Provo '%s' (%u/%u)\n", n.ssid.c_str(), (unsigned)s_idx + 1, (unsigned)s_plan.size());
    WiFi.disconnect(false);
    WiFi.begin(n.ssid.c_str(), n.password.c_str());
    s_t0 = millis();
    s_state = State::Connecting;
}

bool s_empty_logged = false;  // "nessuna rete salvata" già scritto nel log

void round_failed(const char* why) {
    s_rounds_failed++;
    Serial.printf("[WiFi] %s (giri falliti di fila: %u): riprovo tra %u s\n", why, (unsigned)s_rounds_failed,
                  (unsigned)(kRetryMs / 1000));
    s_state = State::Waiting;
    s_wait_until = millis() + kRetryMs;
}

void build_plan(const std::vector<dhwifi::ScanItem>& scan) {
    auto nets = known();
    std::vector<std::string> ssids;
    for (const auto& n : nets) ssids.push_back(n.ssid.c_str());
    // Un giro sì e uno no anche le reti non visibili (possono essere nascoste).
    bool hidden = (s_round % 2) == 0 || scan.empty();
    s_plan.clear();
    if (s_forced.length()) {
        for (const auto& n : nets)
            if (n.ssid == s_forced) s_plan.push_back(n);
    }
    for (int i : dhwifi::planOrder(ssids, scan, hidden)) {
        if (s_forced.length() && nets[i].ssid == s_forced) continue;
        s_plan.push_back(nets[i]);
    }
    s_forced = "";
    s_idx = 0;
}

void start_round() {
    s_round++;
    if (known().empty()) {
        s_plan.clear();
        // Elenco vuoto (es. loader appena flashato): lo si dice una volta sola
        // nel log, poi si ricontrolla l'elenco in silenzio ogni 10 s.
        if (!s_empty_logged) {
            round_failed("Nessuna rete WiFi salvata");
            s_empty_logged = true;
        } else {
            s_rounds_failed++;
            s_state = State::Waiting;
            s_wait_until = millis() + kRetryMs;
        }
        return;
    }
    s_empty_logged = false;
    WiFi.mode(WIFI_STA);
    int16_t r = WiFi.scanNetworks(true /*asincrona*/, false /*niente reti nascoste in elenco*/);
    s_t0 = millis();
    if (r == WIFI_SCAN_FAILED) {
        build_plan({});
        if (s_plan.empty()) round_failed("Nessuna rete da provare");
        else try_current();
        return;
    }
    s_state = State::Scanning;
}

}  // namespace

// ---------------------------------------------------------------- elenco

std::vector<Net> known() {
    std::vector<Net> r;
    for (const auto& e : DhConfig::getAll("wifi", "network")) {
        Net n;
        DhConfig::splitEntry(e, n.ssid, n.password);
        if (n.ssid.length()) r.push_back(n);
    }
    return r;
}

static void save(const std::vector<Net>& nets) {
    std::vector<String> e;
    for (const auto& n : nets) e.push_back(DhConfig::makeEntry(n.ssid, n.password));
    DhConfig::listSetAll("wifi", "network", e, true);
}

void add(const String& ssid, const String& password, bool first) {
    auto nets = known();
    std::vector<Net> out;
    Net fresh{ssid, password};
    if (first) out.push_back(fresh);
    bool replaced = false;
    for (const auto& n : nets) {
        if (n.ssid == ssid) {
            if (!first && !replaced) out.push_back(fresh);
            replaced = true;
            continue;
        }
        out.push_back(n);
    }
    if (!first && !replaced) out.push_back(fresh);
    save(out);
}

void remove(const String& ssid) {
    auto nets = known();
    std::vector<Net> out;
    for (const auto& n : nets)
        if (n.ssid != ssid) out.push_back(n);
    save(out);
}

void move(const String& ssid, int delta) {
    auto nets = known();
    for (int i = 0; i < (int)nets.size(); i++) {
        if (nets[i].ssid != ssid) continue;
        int j = i + delta;
        if (j < 0 || j >= (int)nets.size()) return;
        std::swap(nets[i], nets[j]);
        save(nets);
        return;
    }
}

// ---------------------------------------------------------------- stato

void begin() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // decide questa macchina a stati
    s_rounds_failed = 0;
    if (WiFi.status() == WL_CONNECTED) {
        s_state = State::Connected;
        s_lost_since = 0;
        return;
    }
    s_lost_since = millis() ? millis() : 1;
    start_round();
}

void stop() {
    s_paused = false;
    s_state = State::Stopped;
    WiFi.scanDelete();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    s_lost_since = 0;
}

void pause() {
    if (s_state == State::Stopped) return;  // spento o già in pausa
    State was = s_state;
    s_state = State::Stopped;
    s_paused = true;
    if (was == State::Scanning || WiFi.scanComplete() == WIFI_SCAN_RUNNING) {
        esp_wifi_scan_stop();
        uint32_t t0 = millis();
        while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t0 < 1000) delay(10);
    }
    WiFi.scanDelete();
    if (was == State::Connecting) WiFi.disconnect(false);  // tentativo a metà: lo si interrompe
}

void resume() {
    if (!s_paused) return;
    s_paused = false;
    // NON begin(): azzererebbe i giri falliti e il momento della perdita di rete
    // (una scansione manuale ritarderebbe il captive portal del firmware).
    if (WiFi.status() == WL_CONNECTED) {
        s_state = State::Connected;
        s_lost_since = 0;
        return;
    }
    if (!s_lost_since) s_lost_since = millis() ? millis() : 1;
    start_round();
}

void reconnectNow() {
    if (s_state == State::Stopped) return;
    s_rounds_failed = 0;
    start_round();
}

void connectTo(const String& ssid) {
    if (s_state == State::Stopped) begin();
    s_forced = ssid;
    s_rounds_failed = 0;
    start_round();
}

void loop() {
    uint32_t now = millis();
    switch (s_state) {
        case State::Stopped:
            return;

        case State::Connected:
            if (WiFi.status() != WL_CONNECTED) {
                Serial.println("[WiFi] Connessione persa");
                s_lost_since = now ? now : 1;
                s_state = State::Waiting;
                s_wait_until = now + kGraceMs;
            }
            return;

        case State::Waiting:
            if (WiFi.status() == WL_CONNECTED) {
                s_state = State::Connected;
                s_lost_since = 0;
                return;
            }
            if ((int32_t)(now - s_wait_until) >= 0) start_round();
            return;

        case State::Scanning: {
            int16_t n = WiFi.scanComplete();
            if (n == WIFI_SCAN_RUNNING && now - s_t0 < kScanTimeoutMs) return;
            std::vector<dhwifi::ScanItem> scan;
            for (int i = 0; i < n; i++) scan.push_back({WiFi.SSID(i).c_str(), WiFi.RSSI(i)});
            WiFi.scanDelete();
            build_plan(scan);
            if (s_plan.empty()) {
                round_failed("Nessuna rete nota nei paraggi");
                return;
            }
            try_current();
            return;
        }

        case State::Connecting: {
            wl_status_t st = WiFi.status();
            if (st == WL_CONNECTED) {
                Serial.printf("[WiFi] Connesso a '%s', IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                s_state = State::Connected;
                s_rounds_failed = 0;
                s_lost_since = 0;
                return;
            }
            // Rete assente o password errata: inutile aspettare tutto il timeout.
            bool failed_fast = (st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) && now - s_t0 > 3000;
            if (!failed_fast && now - s_t0 < kTryTimeoutMs) return;
            Serial.printf("[WiFi] '%s' non riuscita (%s)\n", s_plan[s_idx].ssid.c_str(),
                          st == WL_NO_SSID_AVAIL ? "rete non trovata" : st == WL_CONNECT_FAILED ? "password errata?" : "timeout");
            if (++s_idx < s_plan.size()) try_current();
            else round_failed("Nessuna rete nota raggiungibile");
            return;
        }
    }
}

State state() { return s_state; }

String stateText() {
    switch (s_state) {
        case State::Stopped:    return "Fermo";
        case State::Connected:  return "Connesso a " + WiFi.SSID();
        case State::Waiting:    return s_rounds_failed ? "Nessuna rete raggiungibile, nuovo tentativo a breve" : "Connessione persa...";
        case State::Scanning:   return "Ricerca delle reti note...";
        case State::Connecting: return s_idx < s_plan.size() ? "Connessione a " + s_plan[s_idx].ssid + "..." : String("Connessione...");
    }
    return "";
}

uint32_t roundsFailed() { return s_rounds_failed; }

uint32_t disconnectedMs() {
    if (s_state == State::Stopped || s_state == State::Connected || !s_lost_since) return 0;
    return millis() - s_lost_since;
}

}  // namespace DhWifi
