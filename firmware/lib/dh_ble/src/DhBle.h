/*
 * DhBle — Bluetooth LE di Display Hub su NimBLE (al posto di Bluedroid, la
 * libreria "BLE" del core: meno RAM interna e archivio accoppiamenti nostro),
 * con i dispositivi accoppiati salvati nella configurazione (DhConfig).
 *
 * Nella configurazione ([bt] di displayhub.txt):
 *   device = aa:bb:cc:dd:ee:ff | 0 | Nome     dispositivi ricordati (MAC | tipo indirizzo | nome)
 *   bond   = our-... | <base64>               chiavi di accoppiamento (le scrive il display)
 *
 * NimBLE tiene gli accoppiamenti nel suo archivio in RAM (NVS_PERSIST 0, vedi
 * lib/NimBLE-Arduino/DISPLAYHUB.md). DhBle aggancia le funzioni dell'archivio:
 * a ogni modifica ne copia il contenuto in [bt] bond (salvataggio immediato);
 * all'avvio e quando il file cambia da fuori (microSD inserita, unione,
 * sincronizzazione) ricarica l'archivio dal file. Vale per tutti gli archivi
 * della configurazione: microSD (senza scheda: fino al riavvio) e NVS.
 *
 * Accoppiamento "Just Works" (bonding + Secure Connections, senza PIN: il
 * display non ha un modo pratico per mostrare o digitare un codice).
 *
 * pair() e scan() sono BLOCCANTI (qualche secondo): chi le chiama tiene viva
 * l'interfaccia. loop() va chiamata spesso dal loop dell'applicazione.
 */
#pragma once
#include <Arduino.h>

#include <vector>

namespace DhBle {

// RAM interna libera minima per accendere il Bluetooth (controller + host
// NimBLE). Se manca, il controller fallisce un'allocazione e si blocca in un
// assert della ROM (watchdog, riavvio): capitava accendendolo a caldo con WiFi
// e VPN già attivi. Contatore, nessuna passata sull'heap.
constexpr size_t kMinInternalFree = 80 * 1024;
bool canStart(String* why = nullptr);

// Accende lo stack e carica gli accoppiamenti. false (senza toccare nulla) se
// la RAM interna non basta: vedi canStart().
bool begin(const char* device_name = "DisplayHub");
void end();                                            // spegne (si può riaccendere)
bool active();
void loop();

struct Device {
    String addr;          // "aa:bb:cc:dd:ee:ff"
    uint8_t addr_type = 0;
    String name;
    bool bonded = false;  // chiavi di accoppiamento presenti
};
std::vector<Device> devices();  // dispositivi ricordati
int bondCount();

struct Found {
    String name;          // "" se non annunciato
    String addr;
    uint8_t addr_type = 0;
    int rssi = 0;
};
std::vector<Found> scan(uint32_t seconds);

// Connessione + accoppiamento. Se la connessione riesce il dispositivo viene
// ricordato anche quando non supporta l'accoppiamento (bonded = false).
// message: esito leggibile per l'interfaccia.
bool pair(const String& addr, uint8_t addr_type, const String& name, String& message);

// Dimentica: cancella le chiavi e toglie il dispositivo dall'elenco.
void forget(const String& addr);

}  // namespace DhBle
