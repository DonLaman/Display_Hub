/*
 * DhConfig — configurazione di Display Hub su microSD (o solo RAM), per le
 * build con l'opzione "configurazione su microSD".
 *
 * Strato ESP32 sopra dhcfg::ConfigStore (DhConfigStore.h, dove sono descritti
 * tutti gli scenari di inserimento/rimozione):
 *  - un task ("dh_cfg") è l'UNICO a toccare la microSD: montaggio, controllo di
 *    presenza ogni 2 s, salvataggi; l'interfaccia non si blocca mai sulla scheda;
 *  - tutte le funzioni qui sotto sono sicure da qualunque task (mutex);
 *  - gli eventi (scheda inserita/tolta, scelta richiesta, salvato...) vanno
 *    letti con pollEvent() dal loop dell'interfaccia, che li mostra.
 *
 * Tre archivi possibili (Options::backend):
 *  - Sd:  file sulla microSD, solo RAM quando manca (build "configurazione su
 *         microSD": nessun dato nostro in NVS);
 *  - Nvs: lo stesso file, conservato come blob nella NVS (build di sempre);
 *  - Ram: nessuna persistenza.
 *
 * La microSD viene montata in /sd tramite il driver del core (sd_diskio). NON
 * usare contemporaneamente SD.begin()/sd_init() della BSP sulla stessa scheda:
 * due montaggi dello stesso filesystem la rovinerebbero. Per leggere/scrivere
 * altri file usare le funzioni file di questa libreria (in arrivo, step 6).
 */
#pragma once
#include <Arduino.h>

#include <vector>

#include "DhConfigStore.h"

namespace DhConfig {

enum class Backend { Ram, Sd, Nvs };

struct Options {
    Backend backend = Backend::Sd;
    int sd_cs = -1, sd_sck = -1, sd_miso = -1, sd_mosi = -1;
    uint32_t sd_hz = 4000000;    // 4 MHz: prudente, il bus è condiviso col display
    const char* device_id = "";  // "" = nessun controllo di appartenenza (loader)
    const char* defaults_ini = "";  // valori della build, nel formato del file
};

// Monta subito la scheda se c'è (entro ~1 s) e avvia il task.
void begin(const Options& opt);

Backend backend();
// true se le modifiche sopravvivono al riavvio: NVS, oppure microSD in uso.
bool persistent();

// Solo archivio Nvs: importa i valori salvati dalle versioni precedenti nei
// vecchi namespace (dh_wifi, dh_net, dhvpn) che il file non ha ancora. La
// rete di dh_wifi viene riletta a ogni avvio: la scrive anche il loader leggero.
void importLegacyNvs();

// --- valori (file/RAM sopra i valori della build)
String get(const char* section, const char* key, const char* def = "");
bool getBool(const char* section, const char* key, bool def);
long getInt(const char* section, const char* key, long def);
std::vector<String> getAll(const char* section, const char* key);
uint32_t revision();  // cambia a ogni modifica: chi usa i valori può ricaricarli

void set(const char* section, const char* key, const String& value, bool urgent = false);
void remove(const char* section, const char* key, bool urgent = false);
void listPut(const char* section, const char* key, const String& entry, bool urgent = false);
void listRemove(const char* section, const char* key, const String& id, bool urgent = false);
// Sostituisce l'intero elenco: aggiunte, rimozioni e ORDINE (es. priorità delle reti).
void listSetAll(const char* section, const char* key, const std::vector<String>& entries, bool urgent = false);

// --- microSD
dhcfg::CardStatus status();
String statusText();       // breve, per righe di stato ("microSD in uso", ...)
String message();          // ultimo messaggio dettagliato
bool dirty();              // modifiche non ancora sulla scheda
uint32_t lastSaveMs();     // millis() dell'ultimo salvataggio, 0 = mai
String pendingOwner();     // con AwaitingDecision
bool pendingForeign();
void decide(dhcfg::Decision d);
void saveNow();
bool reload();
void eject();
bool format();

bool pollEvent(dhcfg::Event& out);  // dal loop dell'interfaccia

// Voci degli elenchi: "primo | resto", es. "CasaWiFi | password" o
// "AA:BB:CC:DD:EE:FF | Sensore". Divide al PRIMO '|' (la password può contenerne).
void splitEntry(const String& entry, String& first, String& rest);
String makeEntry(const String& first, const String& rest);

// Sostituisce tutta la configurazione con un testo (editor della web UI).
void replaceAll(const String& text);

// --- file sulla microSD (archivio Sd con la scheda montata) ---------------
// Percorsi assoluti dalla radice della scheda ("/cartella/file.txt"), senza
// "..". displayhub.txt/.tmp/.bak si leggono ma non si scrivono né cancellano
// da qui: la configurazione si modifica con set/listPut/replaceAll (che la
// tengono coerente con la RAM e con l'unione).
// La scrittura è a blocchi: offset 0 crea "<file>.part", i blocchi successivi
// devono arrivare in ordine (offset = byte già scritti), final = true rinomina
// .part nel file definitivo: un invio interrotto non lascia file a metà.
struct FileEntry {
    String name;
    bool dir = false;
    uint32_t size = 0;
};
bool filesAvailable(String* why = nullptr);
bool fileList(const String& dir, std::vector<FileEntry>& out, String& err);
bool fileRead(const String& path, uint32_t offset, uint32_t max_len, std::string& data, uint32_t& total, String& err);
bool fileWrite(const String& path, uint32_t offset, const uint8_t* data, size_t len, bool final, String& err);
bool fileRemove(const String& path, String& err);
bool fileMkdir(const String& path, String& err);
bool cardSpace(uint64_t& total_bytes, uint64_t& free_bytes);

// Testo completo del file (per diagnostica e, più avanti, sincronizzazione).
String serialize();

}  // namespace DhConfig
