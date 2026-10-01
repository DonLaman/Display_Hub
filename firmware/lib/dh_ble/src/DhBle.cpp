// DhBle — vedi DhBle.h
#include "DhBle.h"

#include <NimBLEDevice.h>

#include "DhConfig.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/host/include/host/ble_store.h"
#include "nimble/nimble/host/store/config/include/store/config/ble_store_config.h"

namespace DhBle {

namespace {

bool s_active = false;
volatile bool s_loading = false;       // ricarica dal file in corso: non ricopiare
volatile bool s_mirror_dirty = false;  // l'archivio NimBLE è cambiato: ricopiarlo nel file
String s_last_mirror;                  // ultimo contenuto di [bt] bond scritto/letto da noi
uint32_t s_last_check_ms = 0;

const uint8_t kTypes[] = {BLE_STORE_OBJ_TYPE_OUR_SEC, BLE_STORE_OBJ_TYPE_PEER_SEC, BLE_STORE_OBJ_TYPE_CCCD};

const char* type_tag(int t) {
    switch (t) {
        case BLE_STORE_OBJ_TYPE_OUR_SEC:  return "our";
        case BLE_STORE_OBJ_TYPE_PEER_SEC: return "peer";
        default:                          return "cccd";
    }
}

size_t value_size(int t) {
    return t == BLE_STORE_OBJ_TYPE_CCCD ? sizeof(struct ble_store_value_cccd) : sizeof(struct ble_store_value_sec);
}

String addr_hex(const ble_addr_t& a) {
    char b[20];
    snprintf(b, sizeof(b), "%u-%02x%02x%02x%02x%02x%02x", a.type, a.val[5], a.val[4], a.val[3], a.val[2], a.val[1], a.val[0]);
    return String(b);
}

// Voce "[bt] bond": id | base64(tipo, lunghezza, struttura NimBLE così com'è).
// L'id rende unica ogni chiave (tipo + indirizzo [+ caratteristica]).
String encode(int type, const union ble_store_value& v) {
    size_t n = value_size(type);
    uint8_t raw[2 + sizeof(union ble_store_value)];
    raw[0] = (uint8_t)type;
    raw[1] = (uint8_t)n;
    memcpy(raw + 2, &v, n);
    unsigned char b64[160];
    size_t out = 0;
    if (mbedtls_base64_encode(b64, sizeof(b64), &out, raw, n + 2) != 0) return "";
    String id = String(type_tag(type)) + "-";
    if (type == BLE_STORE_OBJ_TYPE_CCCD) id += addr_hex(v.cccd.peer_addr) + "-" + String(v.cccd.chr_val_handle);
    else id += addr_hex(v.sec.peer_addr);
    return id + " | " + String((const char*)b64).substring(0, out);
}

bool decode(const String& entry, int& type, union ble_store_value& v) {
    String id, b64;
    DhConfig::splitEntry(entry, id, b64);
    b64.trim();
    uint8_t raw[2 + sizeof(union ble_store_value) + 4];
    size_t n = 0;
    if (mbedtls_base64_decode(raw, sizeof(raw), &n, (const unsigned char*)b64.c_str(), b64.length()) != 0) return false;
    if (n < 2) return false;
    type = raw[0];
    if (type != BLE_STORE_OBJ_TYPE_OUR_SEC && type != BLE_STORE_OBJ_TYPE_PEER_SEC && type != BLE_STORE_OBJ_TYPE_CCCD) return false;
    // Struttura di un'altra versione di NimBLE: meglio riaccoppiare che usarla.
    if (raw[1] != value_size(type) || n != (size_t)raw[1] + 2) return false;
    memset(&v, 0, sizeof(v));
    memcpy(&v, raw + 2, raw[1]);
    return true;
}

String join(const std::vector<String>& v) {
    String r;
    for (const auto& s : v) r += s + "\n";
    return r;
}

// --- aggancio all'archivio di NimBLE (chiamate dal task host di NimBLE)
int store_read(int type, const union ble_store_key* key, union ble_store_value* value) {
    return ble_store_config_read(type, key, value);
}
int store_write(int type, const union ble_store_value* value) {
    int rc = ble_store_config_write(type, value);
    if (rc == 0 && !s_loading) s_mirror_dirty = true;
    return rc;
}
int store_delete(int type, const union ble_store_key* key) {
    int rc = ble_store_config_delete(type, key);
    if (rc == 0 && !s_loading) s_mirror_dirty = true;
    return rc;
}

struct Collect {
    std::vector<String>* out;
    int type;
};
int collect_cb(int type, union ble_store_value* val, void* cookie) {
    auto* c = static_cast<Collect*>(cookie);
    String e = encode(type, *val);
    if (e.length()) c->out->push_back(e);
    return 0;
}

// Archivio NimBLE -> [bt] bond (sostituisce l'elenco, salvataggio immediato).
void mirror_to_config() {
    std::vector<String> now;
    for (uint8_t t : kTypes) {
        Collect c{&now, t};
        ble_store_iterate(t, collect_cb, &c);
    }
    std::vector<String> cur = DhConfig::getAll("bt", "bond");
    for (const auto& e : cur) {
        String id, rest;
        DhConfig::splitEntry(e, id, rest);
        bool keep = false;
        for (const auto& n : now) {
            String nid, nrest;
            DhConfig::splitEntry(n, nid, nrest);
            keep = keep || nid.equalsIgnoreCase(id);
        }
        if (!keep) DhConfig::listRemove("bt", "bond", id, true);
    }
    for (const auto& n : now) DhConfig::listPut("bt", "bond", n, true);
    s_last_mirror = join(DhConfig::getAll("bt", "bond"));
}

// [bt] bond -> archivio NimBLE (all'avvio e quando il file cambia da fuori).
void load_from_config() {
    s_loading = true;
    ble_store_clear();
    int loaded = 0, skipped = 0;
    for (const auto& e : DhConfig::getAll("bt", "bond")) {
        int type;
        union ble_store_value v;
        if (decode(e, type, v) && ble_store_config_write(type, &v) == 0) loaded++;
        else skipped++;
    }
    s_loading = false;
    s_mirror_dirty = false;
    s_last_mirror = join(DhConfig::getAll("bt", "bond"));
    Serial.printf("[BT] Accoppiamenti caricati dalla configurazione: %d%s\n", loaded,
                  skipped ? (" (" + String(skipped) + " non validi ignorati)").c_str() : "");
}

String normalize(const String& a) {
    String r = a;
    r.trim();
    r.toLowerCase();
    return r;
}

}  // namespace

static size_t internal_free() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); }

bool canStart(String* why) {
    size_t f = internal_free();
    if (f >= kMinInternalFree) return true;
    if (why) {
        *why = "RAM interna libera " + String(f / 1024) + " KB, ne servono almeno " + String(kMinInternalFree / 1024) +
               " (WiFi e VPN attivi)";
    }
    return false;
}

bool begin(const char* device_name) {
    if (s_active) return true;
    size_t before = internal_free();
    if (before < kMinInternalFree) {
        Serial.printf("[BT] Non avviato: RAM interna libera %u KB, minimo %u KB\n", (unsigned)(before / 1024),
                      (unsigned)(kMinInternalFree / 1024));
        return false;
    }
    Serial.printf("[BT] Avvio: RAM interna libera %u KB\n", (unsigned)(before / 1024));
    NimBLEDevice::init(device_name ? device_name : "DisplayHub");
    // Accoppiamento con salvataggio delle chiavi, Secure Connections, senza PIN.
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    // init() ha già agganciato l'archivio in RAM di NimBLE: mettiamo le nostre
    // funzioni davanti, che lo usano e in più copiano le modifiche nel file.
    ble_hs_cfg.store_read_cb = store_read;
    ble_hs_cfg.store_write_cb = store_write;
    ble_hs_cfg.store_delete_cb = store_delete;
    load_from_config();
    s_active = true;
    size_t after = internal_free();
    // Serve a tarare kMinInternalFree sui numeri reali del pannello.
    Serial.printf("[BT] Attivo: RAM interna libera %u KB (usati %d KB)\n", (unsigned)(after / 1024),
                  (int)((int)before - (int)after) / 1024);
    return true;
}

void end() {
    if (!s_active) return;
    if (s_mirror_dirty) mirror_to_config();
    NimBLEScan* sc = NimBLEDevice::getScan();
    if (sc && sc->isScanning()) sc->stop();
    NimBLEDevice::deinit(true);  // true: libera anche i client creati
    s_active = false;
}

bool active() { return s_active; }

void loop() {
    if (!s_active) return;
    if (s_mirror_dirty) {
        s_mirror_dirty = false;
        mirror_to_config();
        Serial.printf("[BT] Accoppiamenti salvati nella configurazione (%d chiavi)\n",
                      (int)DhConfig::getAll("bt", "bond").size());
        return;
    }
    uint32_t now = millis();
    if (now - s_last_check_ms < 1000) return;
    s_last_check_ms = now;
    // Il file è cambiato da fuori (microSD inserita/unita, sincronizzazione):
    // si ricarica l'archivio di NimBLE.
    String cur = join(DhConfig::getAll("bt", "bond"));
    if (cur != s_last_mirror) load_from_config();
}

std::vector<Device> devices() {
    std::vector<Device> r;
    for (const auto& e : DhConfig::getAll("bt", "device")) {
        String addr, rest;
        DhConfig::splitEntry(e, addr, rest);
        Device d;
        d.addr = normalize(addr);
        String type, name;
        DhConfig::splitEntry(rest, type, name);
        d.addr_type = (uint8_t)type.toInt();
        d.name = name;
        if (s_active) d.bonded = NimBLEDevice::isBonded(NimBLEAddress(d.addr.c_str(), d.addr_type));
        r.push_back(d);
    }
    return r;
}

int bondCount() { return s_active ? NimBLEDevice::getNumBonds() : 0; }

std::vector<Found> scan(uint32_t seconds) {
    std::vector<Found> r;
    if (!s_active) return r;
    NimBLEScan* sc = NimBLEDevice::getScan();
    sc->setActiveScan(true);
    NimBLEScanResults res = sc->start(seconds, false);  // bloccante
    for (int i = 0; i < res.getCount(); i++) {
        NimBLEAdvertisedDevice d = res.getDevice(i);
        Found f;
        f.name = d.haveName() ? String(d.getName().c_str()) : String("");
        f.addr = normalize(String(d.getAddress().toString().c_str()));
        f.addr_type = d.getAddressType();
        f.rssi = d.haveRSSI() ? d.getRSSI() : 0;
        r.push_back(f);
    }
    sc->clearResults();
    return r;
}

bool pair(const String& addr, uint8_t addr_type, const String& name, String& message) {
    if (!s_active) {
        message = "Bluetooth spento";
        return false;
    }
    NimBLEClient* client = NimBLEDevice::createClient();
    client->setConnectTimeout(8);
    NimBLEAddress a(normalize(addr).c_str(), addr_type);
    if (!client->connect(a)) {
        NimBLEDevice::deleteClient(client);
        message = "Connessione non riuscita";
        return false;
    }
    bool bonded = client->secureConnection() && client->getConnInfo().isBonded();
    int services = 0;
    auto* sv = client->getServices(true);
    if (sv) services = (int)sv->size();
    client->disconnect();
    NimBLEDevice::deleteClient(client);

    String shown = name.length() ? name : normalize(addr);
    DhConfig::listPut("bt", "device", normalize(addr) + " | " + String(addr_type) + " | " + shown, true);
    message = bonded ? "Accoppiato con " + shown + " (" + String(services) + " servizi)"
                     : "Connesso a " + shown + ", ma il dispositivo non accetta l'accoppiamento: ricordato senza chiavi";
    return true;
}

void forget(const String& addr) {
    String a = normalize(addr);
    for (const auto& d : devices()) {
        if (d.addr == a && s_active) NimBLEDevice::deleteBond(NimBLEAddress(a.c_str(), d.addr_type));
    }
    DhConfig::listRemove("bt", "device", a, true);
    if (s_active) {
        mirror_to_config();
    } else {
        // Stack spento: si tolgono le chiavi di quell'indirizzo direttamente dal file.
        String hex = a;
        hex.replace(":", "");
        for (const auto& e : DhConfig::getAll("bt", "bond")) {
            String id, rest;
            DhConfig::splitEntry(e, id, rest);
            if (id.indexOf(hex) >= 0) DhConfig::listRemove("bt", "bond", id, true);
        }
    }
}

}  // namespace DhBle
