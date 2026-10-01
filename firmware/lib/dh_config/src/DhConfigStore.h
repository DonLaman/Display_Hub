/*
 * DhConfigStore — archivio della configurazione di Display Hub, con la microSD
 * che può essere inserita e tolta a display acceso.
 *
 * C++ puro (niente Arduino/FreeRTOS): la scheda arriva tramite l'interfaccia
 * ICard, così tutta la logica è testabile sul PC con una scheda simulata
 * (firmware/test_host/dh_config). Lo strato ESP32 è in DhConfig.h.
 *
 * Valori effettivi = file (o RAM) sopra i valori della build (defaults):
 * una chiave assente nel file vale quanto deciso in fase di build; un elenco
 * (es. le reti WiFi) vale per intero quello del file se il file ne ha almeno uno.
 *
 * Tutte le modifiche sono registrate in un "giornale" finché non finiscono sulla
 * scheda: serve a UNIRE la sessione in corso con un file trovato su una scheda
 * inserita più tardi (vedi Decision::Merge).
 *
 * Scenari (tick() va chiamato periodicamente, es. ogni 200 ms):
 *  - avvio senza scheda              -> solo RAM (Absent), valori della build;
 *  - avvio con file di questo display -> caricato senza chiedere nulla;
 *  - scheda senza file                -> il file viene creato con lo stato attuale;
 *  - inserimento a caldo, file nostro -> se in RAM non è cambiato nulla lo carica,
 *                                        altrimenti chiede (AwaitingDecision);
 *                                        senza risposta entro 15 s: Merge;
 *  - file di un altro display         -> chiede sempre; senza risposta: Ignore;
 *  - rimozione                        -> accorgersene entro probe_ms, si resta in
 *                                        RAM senza perdere nulla (Absent);
 *  - scheda scambiata al volo         -> la firma del settore 0 cambia: come
 *                                        rimozione + inserimento;
 *  - non formattata / illeggibile     -> Unusable (format() per formattarla);
 *  - file illeggibile                 -> si usa displayhub.bak; il file rovinato
 *                                        viene rinominato displayhub.bad-N.txt;
 *  - salvataggio: 2 s dopo l'ultima modifica (subito se urgent), scrittura sicura
 *    displayhub.tmp -> (displayhub.txt diventa .bak) -> displayhub.txt; un .tmp
 *    rimasto da uno spegnimento a metà viene recuperato o cancellato al montaggio.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "DhIni.h"

namespace dhcfg {

// Chiavi che sono elenchi: identità di una voce = testo prima del primo '|',
// senza spazi e in minuscolo (es. SSID o MAC).
bool isListKey(const std::string& section, const std::string& key);
std::string listId(const std::string& entry);

enum class MountResult { Ok, NoCard, NoFilesystem, Error };

class ICard {
public:
    virtual ~ICard() {}
    virtual MountResult mount() = 0;
    virtual void unmount() = 0;
    // Lettura REALE dalla scheda (settore 0), non da cache: false = scheda
    // assente o guasta. signature cambia se la scheda è un'altra.
    virtual bool probe(uint32_t& signature) = 0;
    virtual bool exists(const std::string& path) = 0;
    virtual bool read(const std::string& path, std::string& out) = 0;
    virtual bool write(const std::string& path, const std::string& data) = 0;
    virtual bool rename(const std::string& from, const std::string& to) = 0;  // fallisce se "to" esiste
    virtual bool remove(const std::string& path) = 0;
    virtual bool format() { return false; }
};

enum class CardStatus {
    NotSupported,      // build senza microSD (solo RAM o NVS)
    Absent,            // nessuna scheda: solo RAM
    Ready,             // scheda in uso: le modifiche vengono salvate
    Unusable,          // scheda presente ma non utilizzabile (vedi message())
    AwaitingDecision,  // file trovato: serve una scelta (vedi pendingOwner())
    Ignored,           // scheda lasciata stare su richiesta finché non viene tolta
    Ejected,           // espulsa: si può togliere; non viene rimontata finché resta
};

enum class Decision { Merge, UseCard, Overwrite, Ignore };

struct Event {
    enum Type {
        CardInserted,     // scheda pronta (text: cosa è successo)
        CardRemoved,
        CardUnusable,
        DecisionNeeded,   // mostrare la finestra di scelta
        ConfigReplaced,   // la configurazione è cambiata tutta (ricaricare)
        Saved,
        SaveFailed,
        Recovered,        // file ripristinato da .tmp/.bak
    } type;
    std::string text;
};

struct StoreOptions {
    std::string device_id;          // "" = nessun controllo di appartenenza (loader)
    std::string path = "/displayhub.txt";
    uint32_t probe_ms = 2000;
    uint32_t save_delay_ms = 2000;
    uint32_t decision_timeout_ms = 15000;
    uint32_t save_retry_ms = 5000;
    size_t max_size = 64 * 1024;
};

class Journal {
public:
    void set(const std::string& s, const std::string& k, const std::string& v);
    void remove(const std::string& s, const std::string& k);
    void listPut(const std::string& s, const std::string& k, const std::string& entry);
    void listRemove(const std::string& s, const std::string& k, const std::string& id);
    // Nuovo ordine dell'elenco (ids nell'ordine voluto; le voci non citate in fondo).
    void listOrder(const std::string& s, const std::string& k, const std::vector<std::string>& ids);
    // Applica le modifiche registrate sopra un documento (es. file della scheda).
    // listBase: elenco da cui partire se il documento non ne ha (valori build).
    void applyTo(IniDoc& doc, const IniDoc& listBase) const;
    void clear();
    bool empty() const;

private:
    struct Op {
        std::string s, k, id, value;
        bool list = false, removed = false, order = false;
    };
    std::vector<Op> ops_;  // una sola operazione (l'ultima) per chiave o per voce di elenco
    void put(const Op& op);
};

// Applica a doc una operazione di elenco (sostituisce in posizione o aggiunge).
void listApply(IniDoc& doc, const IniDoc& base, const std::string& s, const std::string& k,
               const std::string& id, const std::string* entry);
// Riordina l'elenco secondo ids (stabile; voci non citate in fondo nell'ordine attuale).
void listReorder(IniDoc& doc, const IniDoc& base, const std::string& s, const std::string& k,
                 const std::vector<std::string>& ids);

class ConfigStore {
public:
    ConfigStore(ICard* card, const IniDoc& defaults, const StoreOptions& opt);

    void begin(uint32_t now);  // all'avvio: prova subito la scheda
    void tick(uint32_t now);

    // --- lettura (file/RAM sopra i valori della build)
    std::string get(const std::string& s, const std::string& k, const std::string& def = "") const;
    bool getBool(const std::string& s, const std::string& k, bool def) const;
    long getInt(const std::string& s, const std::string& k, long def) const;
    std::vector<std::string> getAll(const std::string& s, const std::string& k) const;
    const IniDoc& doc() const { return doc_; }
    uint32_t revision() const { return revision_; }  // cambia a ogni modifica

    // --- modifica (urgent = salva subito, es. un nuovo accoppiamento BT)
    void set(const std::string& s, const std::string& k, const std::string& v, bool urgent = false);
    void remove(const std::string& s, const std::string& k, bool urgent = false);
    void listPut(const std::string& s, const std::string& k, const std::string& entry, bool urgent = false);
    void listRemove(const std::string& s, const std::string& k, const std::string& id, bool urgent = false);
    // Sostituisce l'intero elenco (aggiunte, rimozioni E ordine), es. priorità delle reti.
    void listSetAll(const std::string& s, const std::string& k, const std::vector<std::string>& entries,
                    bool urgent = false);

    // Sostituisce tutta la configurazione con un testo (editor della web UI):
    // le differenze vengono registrate nel giornale voce per voce (così si
    // uniscono bene con un file trovato dopo), e il testo resta com'è scritto,
    // commenti compresi.
    void replaceAll(const std::string& text, bool urgent = true);

    // --- scheda
    CardStatus status() const { return status_; }
    bool mounted() const { return mounted_; }
    const std::string& message() const { return message_; }
    const std::string& pendingOwner() const { return pending_owner_; }  // AwaitingDecision
    bool pendingForeign() const { return pending_foreign_; }
    bool dirty() const { return dirty_; }
    uint32_t lastSaveMs() const { return last_save_ms_; }

    void decide(Decision d, uint32_t now);
    void saveNow(uint32_t now);
    bool reloadFromCard(uint32_t now);
    void eject(uint32_t now);
    bool format(uint32_t now);

    std::vector<Event> takeEvents();

private:
    ICard* card_;
    IniDoc defaults_;
    IniDoc doc_;
    Journal journal_;
    StoreOptions opt_;

    CardStatus status_;
    std::string message_;
    bool mounted_ = false;
    uint32_t signature_ = 0;
    uint32_t last_probe_ms_ = 0;
    bool probed_once_ = false;

    bool dirty_ = false;
    bool urgent_ = false;
    uint32_t last_change_ms_ = 0;
    uint32_t last_save_ms_ = 0;
    uint32_t last_save_fail_ms_ = 0;
    uint32_t revision_ = 1;

    IniDoc pending_file_;
    std::string pending_owner_;
    bool pending_foreign_ = false;
    uint32_t pending_since_ms_ = 0;

    std::vector<Event> events_;
    uint32_t now_ = 0;

    std::string tmpPath() const;
    std::string bakPath() const;
    void emit(Event::Type t, const std::string& text);
    void changed(bool urgent);
    void setStatus(CardStatus s, const std::string& msg);

    void tryMount(bool at_boot);
    void onMounted(bool at_boot);
    void handleRemoved(const std::string& why);
    bool readValid(const std::string& path, std::string& out);
    bool writeFile();
    std::string fileOwner(const IniDoc& d) const;
    void adoptFile(const IniDoc& file, const std::string& why);
};

}  // namespace dhcfg
