/*
 * DhIni — file di configurazione testuale di Display Hub (displayhub.txt).
 *
 * Formato (pensato per essere modificato anche a mano dal PC):
 *
 *   # commento           (righe che iniziano con # o ;)
 *   [sezione]
 *   chiave = valore
 *   network = CasaWiFi | password       (chiavi ripetibili = elenchi)
 *
 * Regole:
 *  - sezioni e chiavi senza distinzione maiuscole/minuscole;
 *  - NIENTE commenti in fondo alla riga: '#' e ';' dopo il valore fanno parte
 *    del valore (servono nelle password);
 *  - spazi attorno a chiave e valore ignorati; per un valore che inizia o finisce
 *    con spazi, o inizia con '"', si usano le virgolette: pass = " ab c "
 *    (dentro le virgolette \" e \\ sono le uniche sequenze speciali);
 *  - commenti, righe vuote, righe non riconosciute e l'ordine vengono CONSERVATI
 *    quando il display riscrive il file: modifica solo le righe che cambia.
 *
 * C++ puro (niente Arduino): compilato e testato anche sul PC.
 */
#pragma once
#include <string>
#include <vector>

namespace dhcfg {

std::string lower(const std::string& s);
std::string trim(const std::string& s);

class IniDoc {
public:
    // Tollerante: le righe non riconosciute vengono conservate così come sono.
    void parse(const std::string& text);
    std::string serialize() const;
    void clear();

    bool has(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    bool getBool(const std::string& section, const std::string& key, bool def) const;
    long getInt(const std::string& section, const std::string& key, long def) const;
    std::vector<std::string> getAll(const std::string& section, const std::string& key) const;

    // Valore singolo: sostituisce la prima occorrenza (e rimuove eventuali
    // duplicati), oppure aggiunge in fondo alla sezione (creandola se serve).
    void set(const std::string& section, const std::string& key, const std::string& value);
    // Elenco: sostituisce tutte le occorrenze, mantenendo la posizione della prima.
    void setAll(const std::string& section, const std::string& key, const std::vector<std::string>& values);
    void remove(const std::string& section, const std::string& key);

    std::vector<std::string> sections() const;
    // Chiavi presenti in una sezione (senza ripetizioni, nell'ordine del file).
    std::vector<std::string> keys(const std::string& section) const;
    bool empty() const;

private:
    struct Line {
        enum Kind { Raw, KeyValue } kind = Raw;
        std::string raw;    // testo originale (Raw) o ultimo testo serializzato
        std::string key;    // minuscolo
        std::string value;  // già senza virgolette
    };
    struct Section {
        std::string name;   // minuscolo; "" = righe prima della prima sezione
        std::string header; // riga originale dell'intestazione, per conservarne la forma
        std::vector<Line> lines;
    };
    std::vector<Section> secs_;

    Section* find(const std::string& section);
    const Section* find(const std::string& section) const;
    Section& findOrCreate(const std::string& section);
    static std::string formatValue(const std::string& v);
    static std::string parseValue(const std::string& v);
    static void insertBeforeTrailingBlanks(Section& s, const Line& l);
};

}  // namespace dhcfg
