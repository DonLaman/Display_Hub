// DhConfig — vedi DhConfig.h
#include "DhConfig.h"

#include <Preferences.h>
#include <SPI.h>
#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <deque>
#include <memory>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ff.h"
#include "sd_diskio.h"

namespace DhConfig {

namespace {

// ---------------------------------------------------------------- microSD reale
class SdCard : public dhcfg::ICard {
public:
    SdCard(int cs, int sck, int miso, int mosi, uint32_t hz)
        : cs_(cs), sck_(sck), miso_(miso), mosi_(mosi), hz_(hz), spi_(HSPI) {}

    dhcfg::MountResult mount() override {
        if (!spi_begun_) {
            spi_.begin(sck_, miso_, mosi_, cs_);
            spi_begun_ = true;
        }
        if (pdrv_ != 0xFF) unmount();
        pdrv_ = sdcard_init(cs_, &spi_, hz_);
        if (pdrv_ == 0xFF) return dhcfg::MountResult::Error;
        if (sdcard_mount(pdrv_, kMount, 4, false)) return dhcfg::MountResult::Ok;
        // Montaggio fallito: se la scheda ha risposto all'inizializzazione il
        // tipo resta impostato -> c'è, ma non ha un filesystem leggibile.
        sdcard_type_t type = sdcard_type(pdrv_);
        sdcard_uninit(pdrv_);
        pdrv_ = 0xFF;
        bool card_there = type == CARD_SD || type == CARD_SDHC || type == CARD_MMC;
        return card_there ? dhcfg::MountResult::NoFilesystem : dhcfg::MountResult::NoCard;
    }

    void unmount() override {
        if (pdrv_ == 0xFF) return;
        sdcard_unmount(pdrv_);
        sdcard_uninit(pdrv_);
        pdrv_ = 0xFF;
    }

    bool probe(uint32_t& signature) override {
        if (pdrv_ == 0xFF) return false;
        // Lettura vera del settore 0 (MBR/boot sector): la cache di FatFs non
        // c'entra, quindi fallisce subito se la scheda è stata tolta. Il
        // contenuto (firma del disco inclusa) distingue una scheda da un'altra.
        std::unique_ptr<uint8_t[]> buf(new uint8_t[512]);
        if (!sd_read_raw(pdrv_, buf.get(), 0)) return false;
        uint32_t h = 2166136261u;  // FNV-1a
        for (int i = 0; i < 512; i++) h = (h ^ buf[i]) * 16777619u;
        signature = h;
        return true;
    }

    bool exists(const std::string& p) override {
        struct stat st;
        return pdrv_ != 0xFF && ::stat(full(p).c_str(), &st) == 0;
    }

    bool read(const std::string& p, std::string& out) override {
        if (pdrv_ == 0xFF) return false;
        FILE* f = fopen(full(p).c_str(), "rb");
        if (!f) return false;
        out.clear();
        char buf[512];
        size_t n;
        bool ok = true;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            out.append(buf, n);
            if (out.size() > 256 * 1024) { ok = false; break; }  // non è il nostro file
        }
        if (ferror(f)) ok = false;
        fclose(f);
        return ok;
    }

    bool write(const std::string& p, const std::string& data) override {
        if (pdrv_ == 0xFF) return false;
        FILE* f = fopen(full(p).c_str(), "wb");
        if (!f) return false;
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = (fflush(f) == 0) && ok;
        ok = (fsync(fileno(f)) == 0) && ok;  // su scheda prima della rename
        ok = (fclose(f) == 0) && ok;
        return ok;
    }

    bool rename(const std::string& a, const std::string& b) override {
        return pdrv_ != 0xFF && ::rename(full(a).c_str(), full(b).c_str()) == 0;
    }

    bool remove(const std::string& p) override { return pdrv_ != 0xFF && ::unlink(full(p).c_str()) == 0; }

    bool format() override {
        // Il driver del core formatta (FAT) solo se il montaggio trova una
        // scheda senza filesystem: esattamente il caso "non formattata".
        if (!spi_begun_) {
            spi_.begin(sck_, miso_, mosi_, cs_);
            spi_begun_ = true;
        }
        if (pdrv_ != 0xFF) unmount();
        uint8_t pdrv = sdcard_init(cs_, &spi_, hz_);
        if (pdrv == 0xFF) return false;
        bool ok = sdcard_mount(pdrv, kMount, 4, true);
        if (ok) sdcard_unmount(pdrv);
        sdcard_uninit(pdrv);
        return ok;
    }

    // --- per i comandi file (chiamati con il blocco della configurazione preso)
    uint8_t pdrv() const { return pdrv_; }
    static std::string full(const std::string& p) { return std::string(kMount) + p; }

private:
    static constexpr const char* kMount = "/sd";
    int cs_, sck_, miso_, mosi_;
    uint32_t hz_;
    SPIClass spi_;
    bool spi_begun_ = false;
    uint8_t pdrv_ = 0xFF;
};

// ---------------------------------------------------------------- NVS
// Lo stesso file di configurazione, conservato come blob nel namespace "dh_cfg"
// (build di sempre). Sempre "inserita"; i nomi file diventano chiavi NVS.
class NvsCard : public dhcfg::ICard {
public:
    dhcfg::MountResult mount() override { return dhcfg::MountResult::Ok; }
    void unmount() override {}
    bool probe(uint32_t& signature) override {
        signature = 1;
        return true;
    }
    bool exists(const std::string& p) override {
        Preferences pr;
        if (!pr.begin(kNs, true)) return false;
        bool r = pr.isKey(key(p).c_str());
        pr.end();
        return r;
    }
    bool read(const std::string& p, std::string& out) override {
        Preferences pr;
        if (!pr.begin(kNs, true)) return false;
        size_t n = pr.getBytesLength(key(p).c_str());
        bool ok = n > 0;
        if (ok) {
            out.assign(n, '\0');
            ok = pr.getBytes(key(p).c_str(), &out[0], n) == n;
        }
        pr.end();
        return ok;
    }
    bool write(const std::string& p, const std::string& data) override {
        Preferences pr;
        if (!pr.begin(kNs, false)) return false;
        bool ok = pr.putBytes(key(p).c_str(), data.data(), data.size()) == data.size();
        pr.end();
        return ok;
    }
    bool rename(const std::string& a, const std::string& b) override {
        if (exists(b)) return false;
        std::string d;
        return read(a, d) && write(b, d) && remove(a);
    }
    bool remove(const std::string& p) override {
        Preferences pr;
        if (!pr.begin(kNs, false)) return false;
        bool ok = pr.remove(key(p).c_str());
        pr.end();
        return ok;
    }

private:
    static constexpr const char* kNs = "dh_cfg";
    // "/displayhub.txt" -> "txt", "/displayhub.bad-1.txt" -> "bad-1.txt" (max 15 caratteri)
    static std::string key(const std::string& p) {
        std::string k = p;
        if (!k.empty() && k[0] == '/') k.erase(0, 1);
        const std::string stem = "displayhub.";
        if (k.compare(0, stem.size(), stem) == 0) k.erase(0, stem.size());
        return k.substr(0, 15);
    }
};

// ---------------------------------------------------------------- stato
SemaphoreHandle_t s_mx = nullptr;
dhcfg::ConfigStore* s_store = nullptr;
dhcfg::ICard* s_card = nullptr;
SdCard* s_sd = nullptr;  // stessa scheda di s_card, per i comandi file
Backend s_backend = Backend::Ram;
std::deque<dhcfg::Event> s_events;
TaskHandle_t s_task = nullptr;

struct Lock {
    Lock() { xSemaphoreTakeRecursive(s_mx, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(s_mx); }
};

void collect_events_locked() {
    for (auto& e : s_store->takeEvents()) {
        if (s_events.size() > 20) s_events.pop_front();  // nessuno li legge: non accumulare
        s_events.push_back(e);
        // ConfigReplaced è solo il segnale "ricaricare" e accompagna sempre un
        // altro evento con lo stesso testo: nel log una riga sola.
        if (e.type != dhcfg::Event::ConfigReplaced) Serial.printf("[Config] %s\n", e.text.c_str());
    }
}

void task(void*) {
    for (;;) {
        {
            Lock l;
            s_store->tick(millis());
            collect_events_locked();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

String S(const std::string& s) { return String(s.c_str()); }

}  // namespace

void begin(const Options& opt) {
    if (s_store) return;
    s_mx = xSemaphoreCreateRecursiveMutex();
    dhcfg::IniDoc defaults;
    defaults.parse(opt.defaults_ini ? opt.defaults_ini : "");
    dhcfg::StoreOptions so;
    so.device_id = opt.device_id ? opt.device_id : "";
    s_backend = opt.backend;
    if (opt.backend == Backend::Sd && opt.sd_cs >= 0) {
        s_sd = new SdCard(opt.sd_cs, opt.sd_sck, opt.sd_miso, opt.sd_mosi, opt.sd_hz);
        s_card = s_sd;
    } else if (opt.backend == Backend::Nvs) {
        s_card = new NvsCard();
        so.device_id = "";   // la NVS è di questo chip: nessun controllo di appartenenza
        so.probe_ms = 60000; // niente da rilevare
        so.save_delay_ms = 1000;
    } else {
        s_backend = Backend::Ram;
    }
    s_store = new dhcfg::ConfigStore(s_card, defaults, so);
    {
        Lock l;
        s_store->begin(millis());
        collect_events_locked();
    }
    // Stack 6 KB: stdio + FatFs; priorità bassa, è un lavoro di fondo.
    xTaskCreatePinnedToCore(task, "dh_cfg", 6144, nullptr, 1, &s_task, tskNO_AFFINITY);
}

Backend backend() { return s_backend; }

bool persistent() {
    if (s_backend == Backend::Nvs) return true;
    return s_backend == Backend::Sd && status() == dhcfg::CardStatus::Ready;
}

void importLegacyNvs() {
    if (s_backend != Backend::Nvs) return;
    Preferences p;
    // Rete salvata dal loader (leggero o con schermo) e dalle versioni precedenti.
    if (p.begin("dh_wifi", true)) {
        String ssid = p.getString("ssid", ""), pw = p.getString("password", "");
        p.end();
        if (ssid.length()) {
            bool known = false;
            for (auto& n : getAll("wifi", "network")) {
                int bar = n.indexOf('|');
                String id = (bar < 0 ? n : n.substring(0, bar));
                id.trim();
                known = known || id.equalsIgnoreCase(ssid);
            }
            if (!known) listPut("wifi", "network", ssid + " | " + pw);
        }
    }
    Lock l;
    if (!s_store->doc().has("net", "wifi_enabled") && p.begin("dh_net", true)) {
        if (p.isKey("wifi_en")) s_store->set("net", "wifi_enabled", p.getBool("wifi_en") ? "1" : "0");
        if (p.isKey("bt_en")) s_store->set("net", "bt_enabled", p.getBool("bt_en") ? "1" : "0");
        p.end();
    }
    if (!s_store->doc().has("vpn", "enabled") && p.begin("dhvpn", true)) {
        if (p.isKey("en")) s_store->set("vpn", "enabled", p.getBool("en") ? "1" : "0");
        String key = p.getString("key", ""), host = p.getString("host", "");
        if (key.length()) s_store->set("vpn", "auth_key", key.c_str());
        if (host.length()) s_store->set("vpn", "hostname", host.c_str());
        p.end();
    }
}

String get(const char* s, const char* k, const char* def) { Lock l; return S(s_store->get(s, k, def)); }
bool getBool(const char* s, const char* k, bool def) { Lock l; return s_store->getBool(s, k, def); }
long getInt(const char* s, const char* k, long def) { Lock l; return s_store->getInt(s, k, def); }
std::vector<String> getAll(const char* s, const char* k) {
    Lock l;
    std::vector<String> r;
    for (const auto& v : s_store->getAll(s, k)) r.push_back(S(v));
    return r;
}
uint32_t revision() { Lock l; return s_store->revision(); }

void set(const char* s, const char* k, const String& v, bool urgent) { Lock l; s_store->set(s, k, v.c_str(), urgent); }
void remove(const char* s, const char* k, bool urgent) { Lock l; s_store->remove(s, k, urgent); }
void listPut(const char* s, const char* k, const String& e, bool urgent) { Lock l; s_store->listPut(s, k, e.c_str(), urgent); }
void listRemove(const char* s, const char* k, const String& id, bool urgent) { Lock l; s_store->listRemove(s, k, id.c_str(), urgent); }
void listSetAll(const char* s, const char* k, const std::vector<String>& entries, bool urgent) {
    std::vector<std::string> e;
    for (const auto& x : entries) e.push_back(x.c_str());
    Lock l;
    s_store->listSetAll(s, k, e, urgent);
}

dhcfg::CardStatus status() { Lock l; return s_store->status(); }

String statusText() {
    switch (status()) {
        case dhcfg::CardStatus::NotSupported:     return "Solo RAM";
        default: break;
    }
    if (s_backend == Backend::Nvs) return dirty() ? "Memoria interna (salvataggio...)" : "Memoria interna";
    switch (status()) {
        case dhcfg::CardStatus::Absent:           return "Nessuna microSD (solo RAM)";
        case dhcfg::CardStatus::Ready:            return dirty() ? "microSD in uso (salvataggio...)" : "microSD in uso";
        case dhcfg::CardStatus::Unusable:         return "microSD non utilizzabile";
        case dhcfg::CardStatus::AwaitingDecision: return "microSD: scelta richiesta";
        case dhcfg::CardStatus::Ignored:          return "microSD ignorata";
        case dhcfg::CardStatus::Ejected:          return "microSD espulsa";
        default: break;
    }
    return "";
}

String message() { Lock l; return S(s_store->message()); }
bool dirty() { Lock l; return s_store->dirty(); }
uint32_t lastSaveMs() { Lock l; return s_store->lastSaveMs(); }
String pendingOwner() { Lock l; return S(s_store->pendingOwner()); }
bool pendingForeign() { Lock l; return s_store->pendingForeign(); }

void decide(dhcfg::Decision d) { Lock l; s_store->decide(d, millis()); collect_events_locked(); }
void saveNow() { Lock l; s_store->saveNow(millis()); collect_events_locked(); }
bool reload() { Lock l; bool r = s_store->reloadFromCard(millis()); collect_events_locked(); return r; }
void eject() { Lock l; s_store->eject(millis()); collect_events_locked(); }
bool format() { Lock l; bool r = s_store->format(millis()); collect_events_locked(); return r; }

bool pollEvent(dhcfg::Event& out) {
    Lock l;
    if (s_events.empty()) return false;
    out = s_events.front();
    s_events.pop_front();
    return true;
}

String serialize() { Lock l; return S(s_store->doc().serialize()); }

void replaceAll(const String& text) { Lock l; s_store->replaceAll(text.c_str(), true); }

// ---------------------------------------------------------------- file

namespace {

bool valid_path(const String& p, String& err) {
    if (!p.startsWith("/") || p.length() > 200 || p.indexOf("..") >= 0 || p.indexOf("//") >= 0 ||
        p.indexOf('\\') >= 0) {
        err = "percorso non valido (assoluto dalla radice, senza ..)";
        return false;
    }
    return true;
}

bool is_config_file(const String& p) {
    String l = p;
    l.toLowerCase();
    return l == "/displayhub.txt" || l == "/displayhub.tmp" || l == "/displayhub.bak";
}

// Da chiamare con il blocco preso.
bool files_ready_locked(String* why) {
    if (!s_sd) {
        if (why) *why = "questa build non usa la microSD";
        return false;
    }
    if (!s_store->mounted()) {
        if (why) *why = "microSD non presente o non montata (" + String(s_store->message().c_str()) + ")";
        return false;
    }
    return true;
}

}  // namespace

bool filesAvailable(String* why) {
    Lock l;
    return files_ready_locked(why);
}

bool fileList(const String& dir, std::vector<FileEntry>& out, String& err) {
    if (!valid_path(dir, err)) return false;
    Lock l;
    if (!files_ready_locked(&err)) return false;
    std::string base = SdCard::full(dir.c_str());
    if (base.size() > 1 && base.back() == '/') base.pop_back();
    DIR* d = opendir(base.c_str());
    if (!d) {
        err = "cartella non trovata";
        return false;
    }
    out.clear();
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && out.size() < 200) {
        FileEntry f;
        f.name = e->d_name;
        std::string fp = base + "/" + e->d_name;
        struct stat st;
        if (::stat(fp.c_str(), &st) == 0) {
            f.dir = S_ISDIR(st.st_mode);
            f.size = f.dir ? 0 : (uint32_t)st.st_size;
        } else {
            f.dir = e->d_type == DT_DIR;
        }
        out.push_back(f);
    }
    closedir(d);
    return true;
}

bool fileRead(const String& path, uint32_t offset, uint32_t max_len, std::string& data, uint32_t& total, String& err) {
    if (!valid_path(path, err)) return false;
    Lock l;
    if (!files_ready_locked(&err)) return false;
    FILE* f = fopen(SdCard::full(path.c_str()).c_str(), "rb");
    if (!f) {
        err = "file non trovato";
        return false;
    }
    fseek(f, 0, SEEK_END);
    total = (uint32_t)ftell(f);
    data.clear();
    if (offset < total) {
        fseek(f, offset, SEEK_SET);
        uint32_t n = total - offset < max_len ? total - offset : max_len;
        data.resize(n);
        size_t got = fread(&data[0], 1, n, f);
        data.resize(got);
    }
    bool ok = !ferror(f);
    fclose(f);
    if (!ok) err = "errore di lettura";
    return ok;
}

bool fileWrite(const String& path, uint32_t offset, const uint8_t* data, size_t len, bool final, String& err) {
    if (!valid_path(path, err)) return false;
    if (is_config_file(path)) {
        err = "la configurazione si modifica con i comandi config_* (resta coerente con RAM e unione)";
        return false;
    }
    Lock l;
    if (!files_ready_locked(&err)) return false;
    std::string target = SdCard::full(path.c_str());
    std::string part = target + ".part";
    struct stat st;
    if (offset == 0) {
        ::unlink(part.c_str());
    } else if (::stat(part.c_str(), &st) != 0 || (uint32_t)st.st_size != offset) {
        err = "blocco fuori sequenza (atteso offset " + String(::stat(part.c_str(), &st) == 0 ? (uint32_t)st.st_size : 0) +
              "): ricominciare da 0";
        return false;
    }
    FILE* f = fopen(part.c_str(), offset == 0 ? "wb" : "ab");
    if (!f) {
        err = "impossibile creare il file (cartella inesistente?)";
        return false;
    }
    bool ok = len == 0 || fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        err = "errore di scrittura (microSD piena o protetta?)";
        return false;
    }
    if (final) {
        if (::stat(target.c_str(), &st) == 0 && ::unlink(target.c_str()) != 0) {
            err = "impossibile sostituire il file esistente";
            return false;
        }
        if (::rename(part.c_str(), target.c_str()) != 0) {
            err = "impossibile completare il file";
            return false;
        }
    }
    return true;
}

bool fileRemove(const String& path, String& err) {
    if (!valid_path(path, err) || path == "/") {
        if (path == "/") err = "non si cancella la radice";
        return false;
    }
    if (is_config_file(path)) {
        err = "la configurazione non si cancella da qui";
        return false;
    }
    Lock l;
    if (!files_ready_locked(&err)) return false;
    std::string p = SdCard::full(path.c_str());
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) {
        err = "non trovato";
        return false;
    }
    bool ok = S_ISDIR(st.st_mode) ? ::rmdir(p.c_str()) == 0 : ::unlink(p.c_str()) == 0;
    if (!ok) err = S_ISDIR(st.st_mode) ? "cartella non vuota?" : "cancellazione non riuscita";
    return ok;
}

bool fileMkdir(const String& path, String& err) {
    if (!valid_path(path, err)) return false;
    Lock l;
    if (!files_ready_locked(&err)) return false;
    if (::mkdir(SdCard::full(path.c_str()).c_str(), 0775) != 0) {
        err = "creazione non riuscita (esiste già?)";
        return false;
    }
    return true;
}

bool cardSpace(uint64_t& total_bytes, uint64_t& free_bytes) {
    Lock l;
    if (!files_ready_locked(nullptr)) return false;
    uint8_t pdrv = s_sd->pdrv();
    total_bytes = (uint64_t)sdcard_num_sectors(pdrv) * sdcard_sector_size(pdrv);
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    free_bytes = 0;
    if (f_getfree(drv, &free_clusters, &fs) == FR_OK && fs) {
        free_bytes = (uint64_t)free_clusters * fs->csize * 512;
    }
    return true;
}

void splitEntry(const String& entry, String& first, String& rest) {
    int bar = entry.indexOf('|');
    first = bar < 0 ? entry : entry.substring(0, bar);
    rest = bar < 0 ? String("") : entry.substring(bar + 1);
    first.trim();
    // Solo lo spazio messo dal formato "a | b": gli altri fanno parte del valore.
    if (rest.startsWith(" ")) rest.remove(0, 1);
    if (rest.endsWith(" ") && !rest.endsWith("  ")) rest.remove(rest.length() - 1);
}

String makeEntry(const String& first, const String& rest) { return first + " | " + rest; }

}  // namespace DhConfig
