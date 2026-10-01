// DhVpn — vedi DhVpn.h per il quadro generale.
#include "DhVpn.h"

#include <cstring>

#include "DhConfig.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ml_arduino_config.h"   // CONFIG_ML_* (prima degli header MicroLink, come nella libreria)
#include "microlink.h"
#include "microlink_internal.h"  // ml_wg_mgr_*: risveglio dei dispositivi (come ml_tcp.c)

namespace DhVpn {

namespace {

constexpr uint32_t kCtlPeriodMs = 500;
constexpr uint32_t kRetryAfterErrorMs = 30000;

// --- Configurazione ---
// Unica fonte: la sezione [vpn] della configurazione (DhConfig: microSD, RAM o
// NVS). I setter scrivono lì; il task di controllo confronta ogni 500 ms i
// valori voluti con quelli della sessione in corso e la riavvia se servono.
//   enabled, auth_key, auth_key_ephemeral, hostname, identity
// I buffer sono statici: microlink_init() copia la struct di config ma NON le
// stringhe (tiene i puntatori), quindi devono restare vivi per tutta la sessione.
char s_key[256] = "";                  // chiave usata dalla sessione in corso
char s_host[64] = "displayhub";        // nome usato dalla sessione in corso
volatile bool s_enabled = false;
volatile bool s_forget_req = false;    // cancellare identità Tailscale
String s_default_host = "displayhub";
String s_build_key;
bool s_build_enabled = false;

// Identità (solo archivio microSD, vedi storage hooks più sotto)
bool s_sd_identity = false;            // hook attivi: identità nel file di configurazione
bool s_running_persistent = false;     // la sessione in corso è partita con archivio persistente
String s_running_identity;             // identità (base64) della sessione in corso, "" = effimera

// --- Stato runtime ---
SemaphoreHandle_t s_mx = nullptr;      // protegge s_ml e s_err
microlink_t* s_ml = nullptr;
volatile bool s_stopping = false;
volatile bool s_wifi_ok = false;
volatile uint32_t s_net_ip = 0;        // IP della rete sottostante (network order)
String s_err;
uint32_t s_err_at_ms = 0;

TaskHandle_t s_task = nullptr;

struct NetSnap {
    uint32_t ip = 0;
    uint32_t gw = 0;
};
NetSnap s_bound;                        // rete su cui è stata avviata/riagganciata la VPN

const char* const kLogTags[] = {
    "microlink", "ml_coord", "ml_derp", "ml_h2", "ml_net_io", "ml_noise",
    "ml_peer_nvs", "ml_stun", "ml_tcp", "ml_udp", "ml_wg_mgr", "ml_zc",
};

struct Lock {
    Lock() { xSemaphoreTake(s_mx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_mx); }
};

NetSnap read_sta() {
    NetSnap n;
    // Stessa interfaccia che usa MicroLink internamente: in modalità USB (WiFi
    // mai inizializzato) l'handle non esiste e restiamo in "Attesa WiFi".
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta) return n;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(sta, &info) == ESP_OK) {
        n.ip = info.ip.addr;
        n.gw = info.gw.addr;
    }
    return n;
}

void set_error(const char* msg) {
    Lock l;
    s_err = msg;
    s_err_at_ms = millis();
}

String sanitize_hostname(const String& in);
bool plausible_key(const String& k);

// Chiave da usare: con archivio persistente (microSD in uso o NVS) la chiave
// normale, senza microSD quella effimera; se ce n'è una sola, quella.
String desired_key() {
    String normal = DhConfig::get("vpn", "auth_key", s_build_key.c_str());
    String eph = DhConfig::get("vpn", "auth_key_ephemeral", "");
    normal.trim();
    eph.trim();
    bool persistent = DhConfig::persistent();
    String k = persistent ? (normal.length() ? normal : eph) : (eph.length() ? eph : normal);
    return plausible_key(k) ? k : String("");
}

String desired_host() { return sanitize_hostname(DhConfig::get("vpn", "hostname", s_default_host.c_str())); }

// --- Identità nel file di configurazione (build con microSD) ---
// Con la microSD in uso l'identità viene letta/scritta in [vpn] identity: il
// display resta lo stesso dispositivo sulla tailnet. Senza microSD MicroLink
// genera un'identità nuova a ogni avvio e non la salva (dispositivo effimero,
// da usare con una auth key "ephemeral" che Tailscale rimuove da sola).
bool hook_load_keys(uint8_t keys[6][32], void*) {
    if (!DhConfig::persistent()) return false;
    String b64 = DhConfig::get("vpn", "identity", "");
    b64.trim();
    if (!b64.length()) return false;
    size_t out_len = 0;
    if (mbedtls_base64_decode(&keys[0][0], 6 * 32, &out_len, (const unsigned char*)b64.c_str(), b64.length()) != 0 ||
        out_len != 6 * 32) {
        log_w("[VPN] identità nel file non valida: ne genero una nuova");
        return false;
    }
    s_running_identity = b64;
    return true;
}

void hook_save_keys(const uint8_t keys[6][32], void*) {
    if (!DhConfig::persistent()) {
        s_running_identity = "";  // effimera: resta solo in RAM
        return;
    }
    unsigned char buf[300];
    size_t n = 0;
    if (mbedtls_base64_encode(buf, sizeof(buf), &n, &keys[0][0], 6 * 32) != 0) return;
    s_running_identity = String((const char*)buf).substring(0, n);
    DhConfig::set("vpn", "identity", s_running_identity, true);  // subito sulla scheda
}

void hook_forget(void*) { DhConfig::remove("vpn", "identity", true); }

// --- Avvio/arresto: SOLO dal task di controllo ---

void start_vpn(const NetSnap& net) {
    s_running_persistent = DhConfig::persistent();
    s_running_identity = "";
    microlink_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auth_key = s_key;
    cfg.device_name = s_host;
    cfg.enable_derp = true;
    cfg.enable_stun = true;
    cfg.enable_disco = true;
    cfg.max_peers = CONFIG_ML_MAX_PEERS;

    microlink_t* ml = microlink_init(&cfg);
    if (!ml) {
        set_error("Inizializzazione fallita (memoria?)");
        return;
    }
    if (microlink_start(ml) != ESP_OK) {
        microlink_destroy(ml);
        set_error("Avvio fallito");
        return;
    }
    {
        Lock l;
        s_ml = ml;
        s_err = "";
    }
    s_bound = net;
}

void stop_vpn() {
    microlink_t* ml;
    {
        Lock l;
        ml = s_ml;
        s_ml = nullptr;   // da qui gli accessori vedono "nessuna sessione"
    }
    if (!ml) return;
    s_stopping = true;
    microlink_destroy(ml);   // include microlink_stop(): ~3 s bloccanti, qui va bene
    s_stopping = false;
    s_bound = NetSnap();
}

void ctl_task(void*) {
    for (;;) {
        NetSnap net = read_sta();
        s_wifi_ok = net.ip != 0;
        s_net_ip = net.ip;

        bool have_ml;
        {
            Lock l;
            have_ml = s_ml != nullptr;
        }

        if (s_forget_req) {
            if (have_ml) stop_vpn();
            microlink_factory_reset();
            s_forget_req = false;
            have_ml = false;
        }
        // Valori voluti dalla configurazione (file/RAM/NVS).
        s_enabled = DhConfig::getBool("vpn", "enabled", s_build_enabled);
        // Senza microSD, con la sessione partita persistente, la si lascia
        // andare com'è (scheda tolta a VPN accesa: nessuna interruzione).
        bool keep_running_as_is = have_ml && s_running_persistent && !DhConfig::persistent();
        if (!keep_running_as_is) {
            String dk = desired_key(), dh = desired_host();
            if (dk != s_key || dh != s_host) {
                if (have_ml) stop_vpn();
                have_ml = false;
                strlcpy(s_key, dk.c_str(), sizeof(s_key));
                strlcpy(s_host, dh.c_str(), sizeof(s_host));
                Lock l;
                s_err = "";
            }
        }
        // microSD appena in uso (o identità cambiata nel file, es. unione):
        // riparte con l'identità salvata, o ne registra una fissa nuova.
        if (have_ml && s_sd_identity && DhConfig::persistent()) {
            String file_id = DhConfig::get("vpn", "identity", "");
            file_id.trim();
            if (!s_running_persistent || file_id != s_running_identity) {
                log_i("[VPN] microSD in uso: riavvio con l'identità %s", file_id.length() ? "salvata" : "fissa nuova");
                stop_vpn();
                have_ml = false;
            }
        }

        bool want = s_enabled && s_key[0] != '\0';

        if (!have_ml) {
            bool backoff = false;
            {
                Lock l;
                backoff = s_err.length() > 0 && (millis() - s_err_at_ms) < kRetryAfterErrorMs;
            }
            if (want && s_wifi_ok && !backoff) start_vpn(net);
        } else if (!want) {
            stop_vpn();
        } else if (s_wifi_ok && (net.ip != s_bound.ip || net.gw != s_bound.gw)) {
            // Cambio di rete (router <-> hotspot, nuovo DHCP): riaggancio veloce
            // senza distruggere la sessione. Se il WiFi è solo caduto (ip = 0)
            // non facciamo nulla: MicroLink ritenta da solo, e al ritorno della
            // rete (anche diversa) passiamo di qui.
            {
                Lock l;
                if (s_ml) microlink_rebind(s_ml);
            }
            s_bound = net;
        }

        vTaskDelay(pdMS_TO_TICKS(kCtlPeriodMs));
    }
}

String sanitize_hostname(const String& in) {
    String out;
    for (size_t i = 0; i < in.length() && out.length() < 63; i++) {
        char c = tolower((unsigned char)in[i]);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out += c;
        } else if ((c == '-' || c == '_' || c == ' ' || c == '.') && out.length() && !out.endsWith("-")) {
            out += '-';
        }
    }
    while (out.endsWith("-")) out.remove(out.length() - 1);
    if (out.isEmpty()) out = "displayhub";
    return out;
}

bool plausible_key(const String& k) {
    // Chiavi Tailscale "tskey-auth-..." (o chiavi Headscale esadecimali):
    // controllo solo di forma, la validità vera la decide il control plane.
    if (k.length() < 16 || k.length() >= sizeof(s_key)) return false;
    for (size_t i = 0; i < k.length(); i++) {
        if (isspace((unsigned char)k[i])) return false;
    }
    return true;
}

String ip_to_string(uint32_t host_order_ip) {
    char buf[16];
    microlink_ip_to_str(host_order_ip, buf);
    return String(buf);
}

}  // namespace

// ---------------------------------------------------------------------------

// mbedTLS in PSRAM: il core lo configura per allocare SOLO in RAM interna
// (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC), e la sessione TLS verso il relay DERP
// ne occupa decine di KB: con WiFi e VPN attivi il Bluetooth non trovava più
// memoria per partire. Equivale all'opzione CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC
// di ESP-IDF; se la PSRAM fosse piena si ripiega sulla RAM interna.
static void* mbedtls_psram_calloc(size_t n, size_t size) {
    void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
static void mbedtls_any_free(void* p) { heap_caps_free(p); }

void begin(const char* default_hostname, const char* build_auth_key, bool build_enabled) {
    if (s_task) return;   // già avviata
    mbedtls_platform_set_calloc_free(mbedtls_psram_calloc, mbedtls_any_free);
    s_mx = xSemaphoreCreateMutex();

    // DhConfig::begin() va chiamata prima: i valori stanno in [vpn].
    s_default_host = sanitize_hostname(default_hostname ? default_hostname : "");
    s_build_key = build_auth_key ? build_auth_key : "";
    s_build_enabled = build_enabled;
    s_enabled = DhConfig::getBool("vpn", "enabled", build_enabled);
    strlcpy(s_key, desired_key().c_str(), sizeof(s_key));
    strlcpy(s_host, desired_host().c_str(), sizeof(s_host));

    if (DhConfig::backend() == DhConfig::Backend::Sd) {
        // Nessun dato nella NVS: identità nel file (o effimera), niente cache peer.
        static microlink_storage_t st;
        memset(&st, 0, sizeof(st));
        st.load_keys = hook_load_keys;
        st.save_keys = hook_save_keys;
        st.forget = hook_forget;
        st.disable_peer_cache = true;
        microlink_set_storage(&st);
        s_sd_identity = true;
    }

    setVerbose(false);

    // Priorità bassa (1): è solo un supervisore, il lavoro vero lo fanno i task
    // di MicroLink. Stack 4 KB: destroy/start chiamano MicroLink ma senza
    // strutture grosse sullo stack.
    xTaskCreatePinnedToCore(ctl_task, "dhvpn_ctl", 4096, nullptr, 1, &s_task, tskNO_AFFINITY);
}

void setEnabled(bool on) {
    s_enabled = on;
    DhConfig::set("vpn", "enabled", on ? "1" : "0");
    if (on) {
        Lock l;
        s_err = "";   // nuovo tentativo immediato, senza aspettare il backoff
    }
}

bool isEnabled() { return s_enabled; }

bool setAuthKey(const String& key) {
    String k = key;
    k.trim();
    if (!plausible_key(k)) return false;
    DhConfig::set("vpn", "auth_key", k);  // il task di controllo riavvia se serve
    return true;
}

bool hasAuthKey() { return desired_key().length() > 0; }

String authKeyMasked() {
    String k = desired_key();
    size_t n = k.length();
    if (n == 0) return "(nessuna)";
    String m = n <= 16 ? String("********") : k.substring(0, 12) + "..." + k.substring(n - 4);
    if (DhConfig::backend() == DhConfig::Backend::Sd && !DhConfig::persistent()) m += " (senza microSD)";
    return m;
}

void clearAuthKey() { DhConfig::remove("vpn", "auth_key"); }

void setHostname(const String& name) { DhConfig::set("vpn", "hostname", sanitize_hostname(name)); }

String hostname() { return desired_host(); }

State state() {
    if (s_stopping) return State::Stopping;
    {
        Lock l;
        if (s_ml) {
            switch (microlink_get_state(s_ml)) {
                case ML_STATE_IDLE:
                case ML_STATE_WIFI_WAIT:    return State::WaitWifi;
                case ML_STATE_CONNECTING:   return State::Connecting;
                case ML_STATE_REGISTERING:  return State::Registering;
                case ML_STATE_CONNECTED:    return State::Connected;
                case ML_STATE_RECONNECTING: return State::Reconnecting;
                case ML_STATE_ERROR:
                default:                    return State::Error;
            }
        }
        if (s_enabled && s_err.length()) return State::Error;
    }
    if (!s_enabled) return State::Off;
    if (s_forget_req) return State::Stopping;  // modifica in applicazione
    if (!hasAuthKey()) return State::NoKey;
    if (!s_wifi_ok) return State::WaitWifi;
    return State::Connecting;   // avvio imminente (entro un ciclo del task)
}

const char* stateText() {
    switch (state()) {
        case State::Off:          return "Disattivata";
        case State::NoKey:        return "Manca la auth key";
        case State::WaitWifi:     return "In attesa del WiFi";
        case State::Connecting:   return "Connessione...";
        case State::Registering:  return "Registrazione nodo...";
        case State::Connected:    return "Connessa";
        case State::Reconnecting: return "Riconnessione...";
        case State::Stopping:     return "Arresto...";
        case State::Error:
        default:                  return "Errore";
    }
}

String lastError() {
    Lock l;
    return s_err;
}

bool isConnected() { return state() == State::Connected; }

String vpnIp() {
    Lock l;
    if (!s_ml || !microlink_is_connected(s_ml)) return "";
    uint32_t ip = microlink_get_vpn_ip(s_ml);
    return ip ? ip_to_string(ip) : String("");
}

String networkIp() {
    uint32_t ip = s_net_ip;   // network order (lwIP)
    if (!ip) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)(ip >> 24));
    return String(buf);
}

int peerCount() {
    Lock l;
    return s_ml ? microlink_get_peer_count(s_ml) : 0;
}

bool peer(int index, Peer& out) {
    microlink_peer_info_t info;
    memset(&info, 0, sizeof(info));   // MicroLink non termina hostname se troncato
    {
        Lock l;
        if (!s_ml || microlink_get_peer_info(s_ml, index, &info) != ESP_OK) return false;
    }
    info.hostname[sizeof(info.hostname) - 1] = '\0';
    out.hostname = info.hostname;
    out.ip = ip_to_string(info.vpn_ip);
    out.online = info.online;
    out.direct = info.direct_path;
    return true;
}

String resolve(const String& name) {
    uint32_t ip;
    {
        Lock l;
        ip = s_ml ? microlink_resolve(s_ml, name.c_str()) : 0;
    }
    return ip ? ip_to_string(ip) : String("");
}

// Ultimo esito di prepareTunnel(), per chi vuole solo leggere lo stato senza
// risvegliare nulla (pathFor(..., false), chiamata spesso dall'interfaccia).
static uint32_t s_cached_tunnel_ip = 0;
static Tunnel s_cached_tunnel = Tunnel::NoVpn;

static Tunnel prepare_tunnel_impl(const String& vpn_ip);

Tunnel prepareTunnel(const String& vpn_ip) {
    Tunnel t = prepare_tunnel_impl(vpn_ip);
    unsigned a, b, c, d;
    if (sscanf(vpn_ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        s_cached_tunnel_ip = (a << 24) | (b << 16) | (c << 8) | d;
        s_cached_tunnel = t;
    }
    return t;
}

static Tunnel prepare_tunnel_impl(const String& vpn_ip) {
    unsigned a, b, c, d;
    if (sscanf(vpn_ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return Tunnel::NoPeer;
    }
    uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;   // ordine "host", come in MicroLink

    static uint32_t s_last_ip = 0;
    static uint32_t s_last_trigger_ms = 0;

    Lock l;
    if (!s_ml || microlink_get_state(s_ml) != ML_STATE_CONNECTED) return Tunnel::NoVpn;
    if (ml_wg_mgr_peer_is_up(s_ml, ip)) return Tunnel::Up;

    uint32_t now = millis();
    if (ip == s_last_ip && s_last_trigger_ms != 0 && now - s_last_trigger_ms < 5000) return Tunnel::Waking;

    // Stessa sequenza di microlink_tcp_connect(): handshake (DERP + diretto se
    // DISCO ha un endpoint) e CallMeMaybe per far aprire il percorso diretto.
    esp_err_t err = ml_wg_mgr_trigger_handshake(s_ml, ip);
    if (err == ESP_ERR_NOT_FOUND) return Tunnel::NoPeer;
    ml_wg_mgr_send_cmm(s_ml, ip);
    s_last_ip = ip;
    s_last_trigger_ms = now ? now : 1;
    return Tunnel::Waking;
}

const char* tunnelText(Tunnel t) {
    switch (t) {
        case Tunnel::NoVpn:  return "VPN non connessa";
        case Tunnel::NoPeer: return "dispositivo non presente nella tailnet";
        case Tunnel::Waking: return "attivazione tunnel...";
        case Tunnel::Up:     return "tunnel attivo";
    }
    return "";
}

namespace {

uint32_t ip_host_order(const IPAddress& a) { return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3]; }

String host_ip_to_string(uint32_t ip) {
    char buf[16];
    microlink_ip_to_str(ip, buf);
    return String(buf);
}

// Nome e stato online di un peer (per IP), a lock già preso.
void peer_name_locked(uint32_t vpn_ip, String& name, bool& online) {
    int n = microlink_get_peer_count(s_ml);
    for (int i = 0; i < n; i++) {
        microlink_peer_info_t info;
        memset(&info, 0, sizeof(info));
        if (microlink_get_peer_info(s_ml, i, &info) == ESP_OK && info.vpn_ip == vpn_ip) {
            info.hostname[sizeof(info.hostname) - 1] = '\0';
            name = info.hostname;
            online = info.online;
            return;
        }
    }
}

}  // namespace

int routes(Route* out, int max) {
    Lock l;
    if (!s_ml || !out || max <= 0) return 0;
    int count = 0;
    int n = microlink_get_peer_count(s_ml);
    for (int i = 0; i < n && count < max; i++) {
        microlink_route_t r[MICROLINK_MAX_PEER_ROUTES];
        int nr = microlink_get_peer_routes(s_ml, i, r, MICROLINK_MAX_PEER_ROUTES);
        if (nr <= 0) continue;
        microlink_peer_info_t info;
        memset(&info, 0, sizeof(info));
        if (microlink_get_peer_info(s_ml, i, &info) != ESP_OK) continue;
        info.hostname[sizeof(info.hostname) - 1] = '\0';
        for (int k = 0; k < nr && count < max; k++) {
            out[count].network = host_ip_to_string(r[k].ip) + "/" + String(r[k].prefix_len);
            out[count].via_name = info.hostname;
            out[count].via_ip = host_ip_to_string(info.vpn_ip);
            out[count].via_online = info.online;
            count++;
        }
    }
    return count;
}

Path pathFor(const IPAddress& dest, bool wake) {
    Path p;
    uint32_t d = ip_host_order(dest);
    uint32_t peer = 0;
    {
        Lock l;
        if (!s_ml || microlink_get_state(s_ml) != ML_STATE_CONNECTED) return p;   // 1
    }
    // 2. Rete del WiFi attuale: a casa 192.168.1.40 si raggiunge direttamente.
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK && info.ip.addr != 0) {
        uint32_t ip = ntohl(info.ip.addr), mask = ntohl(info.netmask.addr);
        if (mask != 0 && (d & mask) == (ip & mask)) {
            p.on_lan = true;
            return p;
        }
    }
    if ((d & 0xFFC00000u) == 0x64400000u) {
        peer = d;                                                                  // 3
    } else {
        Lock l;
        if (s_ml) peer = microlink_route_lookup(s_ml, d);                          // 4
    }
    if (!peer) return p;                                                           // 5
    p.via_vpn = true;
    p.peer_ip = host_ip_to_string(peer);
    {
        Lock l;
        bool online = false;
        if (s_ml) peer_name_locked(peer, p.peer_name, online);
    }
    if (wake) p.tunnel = prepareTunnel(p.peer_ip);
    else p.tunnel = (s_cached_tunnel_ip == peer) ? s_cached_tunnel : Tunnel::Waking;
    return p;
}

String describe(const Path& p) {
    if (!p.via_vpn) return "diretto";
    String s = "VPN via " + (p.peer_name.length() ? p.peer_name : p.peer_ip);
    return s + " (" + tunnelText(p.tunnel) + ")";
}

void forgetIdentity() {
    s_forget_req = true;   // eseguito dal task di controllo (ferma la sessione se attiva)
}

void setVerbose(bool on) {
    for (const char* tag : kLogTags) esp_log_level_set(tag, on ? ESP_LOG_INFO : ESP_LOG_WARN);
}

}  // namespace DhVpn
