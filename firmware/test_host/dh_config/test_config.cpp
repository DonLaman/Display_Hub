// Test sul PC della logica di configurazione (DhIni + DhConfigStore).
// Esecuzione: firmware/test_host/dh_config/run.sh
#include <cstdio>
#include <map>
#include <string>

#include "DhConfigStore.h"
#include "DhIni.h"

using namespace dhcfg;

static int g_fail = 0, g_ok = 0;
#define CHECK(cond, msg)                                   \
    do {                                                   \
        if (cond) { g_ok++; printf("OK   %s\n", msg); }    \
        else { g_fail++; printf("FAIL %s  (riga %d)\n", msg, __LINE__); } \
    } while (0)

// ---------------------------------------------------------------- scheda finta
struct FakeCard : ICard {
    bool present = false;       // inserita
    bool formatted = true;      // FAT presente
    bool readonly = false;
    uint32_t sig = 1;           // "settore 0": cambia se è un'altra scheda
    bool mounted = false;
    int fail_rename_after = -1; // simula spegnimento: fallisce la N-esima rename
    std::map<std::string, std::string> files;

    MountResult mount() override {
        if (!present) return MountResult::NoCard;
        if (!formatted) return MountResult::NoFilesystem;
        mounted = true;
        return MountResult::Ok;
    }
    void unmount() override { mounted = false; }
    bool probe(uint32_t& s) override {
        if (!present || !mounted) return false;
        s = sig;
        return true;
    }
    bool ok() const { return present && mounted; }
    bool exists(const std::string& p) override { return ok() && files.count(p); }
    bool read(const std::string& p, std::string& out) override {
        if (!ok() || !files.count(p)) return false;
        out = files[p];
        return true;
    }
    bool write(const std::string& p, const std::string& d) override {
        if (!ok() || readonly) return false;
        files[p] = d;
        return true;
    }
    bool rename(const std::string& a, const std::string& b) override {
        if (!ok() || readonly || !files.count(a) || files.count(b)) return false;
        if (fail_rename_after == 0) { fail_rename_after = -1; present = false; return false; }
        if (fail_rename_after > 0) fail_rename_after--;
        files[b] = files[a];
        files.erase(a);
        return true;
    }
    bool remove(const std::string& p) override {
        if (!ok() || readonly || !files.count(p)) return false;
        files.erase(p);
        return true;
    }
    bool format() override {
        if (!present) return false;
        formatted = true;
        files.clear();
        return true;
    }
};

static IniDoc defaults() {
    IniDoc d;
    d.parse("[server]\nhost = 192.168.1.40\nport = 12000\n[wifi]\nnetwork = Casa | pw-casa\n[vpn]\nenabled = 1\n");
    return d;
}

static StoreOptions opts() {
    StoreOptions o;
    o.device_id = "sala-01";
    return o;
}

static bool hasEvent(const std::vector<Event>& ev, Event::Type t) {
    for (const auto& e : ev) if (e.type == t) return true;
    return false;
}

static void run(ConfigStore& s, uint32_t& now, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 200) { now += 200; s.tick(now); }
}

// ---------------------------------------------------------------- test
static void test_ini() {
    printf("\n== File INI ==\n");
    const char* src =
        "# Display Hub\r\n"
        "[Server]\r\n"
        "Host = 192.168.1.40   \r\n"
        "; nota a mano\r\n"
        "\r\n"
        "[wifi]\r\n"
        "network = Casa | p#ss;word\r\n"
        "network = Hotspot | \" spazi \"\r\n"
        "riga strana senza uguale\r\n";
    IniDoc d;
    d.parse(src);
    CHECK(d.get("server", "host") == "192.168.1.40", "chiave/sezione senza maiuscole, spazi tolti");
    CHECK(d.getAll("WIFI", "Network").size() == 2, "elenco con chiave ripetuta");
    CHECK(d.getAll("wifi", "network")[0] == "Casa | p#ss;word", "'#' e ';' nel valore restano (password)");
    std::string out = d.serialize();
    CHECK(out.find("# Display Hub") != std::string::npos && out.find("; nota a mano") != std::string::npos &&
              out.find("riga strana senza uguale") != std::string::npos,
          "commenti e righe sconosciute conservati");
    CHECK(out.find("Host = 192.168.1.40") != std::string::npos, "riga non modificata lasciata com'era");

    d.set("server", "port", "12000");
    d.set("vpn", "enabled", "1");
    d.set("server", "host", "10.0.0.5");
    IniDoc r;
    r.parse(d.serialize());
    CHECK(r.get("server", "host") == "10.0.0.5" && r.get("server", "port") == "12000" && r.get("vpn", "enabled") == "1",
          "modifica, aggiunta in sezione esistente e sezione nuova");
    CHECK(r.serialize().find("Host = 10.0.0.5") != std::string::npos, "conservata la forma della chiave scritta a mano");
    size_t pos_port = r.serialize().find("port = 12000"), pos_nota = r.serialize().find("; nota a mano");
    CHECK(pos_port != std::string::npos && pos_port < r.serialize().find("[wifi]") && pos_nota < pos_port + 100,
          "nuova chiave nella sua sezione");

    d.set("x", "pass", " ab\"c ");
    IniDoc q;
    q.parse(d.serialize());
    CHECK(q.get("x", "pass") == " ab\"c ", "valori con spazi/virgolette: andata e ritorno");
    CHECK(q.getBool("vpn", "enabled", false) && q.getInt("server", "port", 0) == 12000, "booleani e numeri");
}

static void test_boot_without_card() {
    printf("\n== Avvio senza microSD ==\n");
    FakeCard c;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    CHECK(s.status() == CardStatus::Absent, "stato: solo RAM");
    CHECK(s.get("server", "host") == "192.168.1.40", "valori della build");
    s.listPut("wifi", "network", "Hotspot | pw-hot");
    run(s, now, 5000);
    CHECK(s.getAll("wifi", "network").size() == 2, "rete aggiunta in RAM, quella della build resta");
    CHECK(s.dirty(), "modifiche non salvate segnalate");

    printf("-- inserimento di una microSD vuota\n");
    c.present = true;
    s.takeEvents();
    run(s, now, 2200);
    auto ev = s.takeEvents();
    CHECK(s.status() == CardStatus::Ready && hasEvent(ev, Event::CardInserted), "scheda pronta");
    CHECK(c.files.count("/displayhub.txt") && !s.dirty(), "file creato con lo stato attuale");
    IniDoc f;
    f.parse(c.files["/displayhub.txt"]);
    CHECK(f.get("device", "id") == "sala-01" && f.getAll("wifi", "network").size() == 2, "file: identificativo e reti");
}

static void test_boot_with_own_file() {
    printf("\n== Avvio con il file di questo display ==\n");
    FakeCard c;
    c.present = true;
    c.files["/displayhub.txt"] = "[device]\nid = sala-01\n[server]\nhost = 192.168.1.99\n";
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    CHECK(s.status() == CardStatus::Ready, "caricato senza chiedere");
    CHECK(s.get("server", "host") == "192.168.1.99", "il file vince sulla build");
    CHECK(s.get("server", "port") == "12000", "chiave assente nel file: valore della build");
    CHECK(s.getAll("wifi", "network").size() == 1, "elenco assente nel file: quello della build");

    printf("-- salvataggio ritardato\n");
    s.set("server", "port", "13000");
    run(s, now, 1000);
    CHECK(s.dirty() && c.files["/displayhub.txt"].find("13000") == std::string::npos, "non salva prima di 2 s");
    run(s, now, 1400);
    CHECK(!s.dirty() && c.files["/displayhub.txt"].find("13000") != std::string::npos, "salvato dopo 2 s");
    CHECK(c.files.count("/displayhub.bak") && !c.files.count("/displayhub.tmp"), "copia .bak, nessun .tmp rimasto");
    s.listPut("bt", "bond", "AA:BB | chiave", true);
    s.tick(now + 1);
    CHECK(c.files["/displayhub.txt"].find("AA:BB") != std::string::npos, "modifica urgente salvata subito");
}

static void test_hot_insert_merge() {
    printf("\n== Inserimento a caldo con modifiche nella sessione ==\n");
    FakeCard c;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    s.set("server", "port", "15000");                  // cambiata in sessione
    s.listPut("wifi", "network", "Hotspot | pw-hot");  // aggiunta in sessione
    s.listRemove("wifi", "network", "Casa");           // tolta in sessione

    c.present = true;
    c.files["/displayhub.txt"] =
        "# mio commento\n[device]\nid = sala-01\n[server]\nhost = 192.168.1.77\nport = 12000\n"
        "[wifi]\nnetwork = Casa | pw-casa\nnetwork = Ufficio | pw-uff\n";
    s.takeEvents();
    run(s, now, 2200);
    auto ev = s.takeEvents();
    CHECK(s.status() == CardStatus::AwaitingDecision && hasEvent(ev, Event::DecisionNeeded), "chiede cosa fare");
    CHECK(!s.pendingForeign(), "riconosciuto come file di questo display");
    run(s, now, 15000);
    CHECK(s.status() == CardStatus::Ready, "dopo 15 s senza risposta: unione automatica");
    CHECK(s.get("server", "host") == "192.168.1.77", "valore del file mantenuto");
    CHECK(s.get("server", "port") == "15000", "valore cambiato nella sessione applicato");
    auto nets = s.getAll("wifi", "network");
    bool casa = false, uff = false, hot = false;
    for (auto& n : nets) { casa |= listId(n) == "casa"; uff |= listId(n) == "ufficio"; hot |= listId(n) == "hotspot"; }
    CHECK(!casa && uff && hot, "reti: tolta 'Casa', tenuta 'Ufficio' del file, aggiunta 'Hotspot'");
    CHECK(c.files["/displayhub.txt"].find("# mio commento") != std::string::npos, "commento del file conservato");
    CHECK(!s.dirty(), "unione salvata subito");
}

static void test_hot_insert_no_changes() {
    printf("\n== Inserimento a caldo senza modifiche ==\n");
    FakeCard c;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    c.present = true;
    c.files["/displayhub.txt"] = "[device]\nid = sala-01\n[server]\nhost = 192.168.1.5\n";
    run(s, now, 2200);
    auto ev = s.takeEvents();
    CHECK(s.status() == CardStatus::Ready && s.get("server", "host") == "192.168.1.5" &&
              hasEvent(ev, Event::ConfigReplaced),
          "caricato senza chiedere (e segnalato per il ricaricamento)");
}

static void test_foreign() {
    printf("\n== microSD di un altro display ==\n");
    FakeCard c;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    c.present = true;
    c.files["/displayhub.txt"] = "[device]\nid = cucina-02\n[server]\nhost = 1.2.3.4\n";
    run(s, now, 2200);
    CHECK(s.status() == CardStatus::AwaitingDecision && s.pendingForeign() && s.pendingOwner() == "cucina-02",
          "chiede sempre, indicando il proprietario");
    run(s, now, 15000);
    CHECK(s.status() == CardStatus::Ignored && s.get("server", "host") == "192.168.1.40", "senza risposta: ignorata");
    s.set("server", "port", "1");
    run(s, now, 5000);
    CHECK(c.files["/displayhub.txt"].find("cucina-02") != std::string::npos && c.files["/displayhub.txt"].find("port") == std::string::npos,
          "file dell'altro display non toccato");
    c.present = false;
    run(s, now, 2200);
    CHECK(s.status() == CardStatus::Absent, "tolta: di nuovo solo RAM");

    printf("-- scelte esplicite\n");
    FakeCard c2;
    c2.present = true;
    c2.files["/displayhub.txt"] = "[device]\nid = cucina-02\n[server]\nhost = 1.2.3.4\n";
    ConfigStore s2(&c2, defaults(), opts());
    uint32_t n2 = 0;
    s2.begin(n2);
    s2.decide(Decision::Overwrite, n2);
    IniDoc f;
    f.parse(c2.files["/displayhub.txt"]);
    CHECK(f.get("device", "id") == "sala-01" && s2.status() == CardStatus::Ready, "Sovrascrivi: diventa di questo display");

    FakeCard c3;
    c3.present = true;
    c3.files["/displayhub.txt"] = "[device]\nid = cucina-02\n[server]\nhost = 1.2.3.4\n";
    ConfigStore s3(&c3, defaults(), opts());
    uint32_t n3 = 0;
    s3.begin(n3);
    s3.decide(Decision::UseCard, n3);
    CHECK(s3.get("server", "host") == "1.2.3.4" && s3.status() == CardStatus::Ready, "Usa comunque: configurazione della scheda");
}

static void test_removal_and_swap() {
    printf("\n== Rimozione e sostituzione ==\n");
    FakeCard c;
    c.present = true;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    s.set("server", "port", "14000");
    run(s, now, 2400);
    c.present = false;
    s.takeEvents();
    run(s, now, 2200);
    auto ev = s.takeEvents();
    CHECK(s.status() == CardStatus::Absent && hasEvent(ev, Event::CardRemoved), "rimozione rilevata entro 2 s");
    CHECK(s.get("server", "port") == "14000", "nessun dato della sessione perso");
    s.listPut("wifi", "network", "Treno | pw-treno");
    c.present = true;
    run(s, now, 2200);
    CHECK(s.status() == CardStatus::AwaitingDecision, "reinserita con modifiche nel frattempo: chiede");
    s.decide(Decision::Merge, now);
    CHECK(c.files["/displayhub.txt"].find("Treno") != std::string::npos, "unione: la rete aggiunta senza scheda finisce nel file");

    printf("-- scambio al volo (tra due controlli)\n");
    c.sig = 2;
    c.files.clear();
    c.files["/displayhub.txt"] = "[device]\nid = sala-01\n[server]\nhost = 172.16.0.1\n";
    s.takeEvents();
    run(s, now, 2200);
    ev = s.takeEvents();
    CHECK(hasEvent(ev, Event::CardRemoved) && s.get("server", "host") == "172.16.0.1",
          "firma diversa: trattata come rimozione + inserimento");
}

static void test_recovery() {
    printf("\n== Salvataggi interrotti e file rovinati ==\n");
    {
        FakeCard c;
        c.present = true;
        c.files["/displayhub.tmp"] = "[device]\nid = sala-01\n[server]\nhost = 9.9.9.9\n";
        c.files["/displayhub.bak"] = "[server]\nhost = 8.8.8.8\n";
        ConfigStore s(&c, defaults(), opts());
        uint32_t now = 0;
        s.begin(now);
        CHECK(s.get("server", "host") == "9.9.9.9" && !c.files.count("/displayhub.tmp"),
              ".tmp senza file principale: ripristinato");
    }
    {
        FakeCard c;
        c.present = true;
        c.files["/displayhub.txt"] = "[device]\nid = sala-01\n[server]\nhost = 7.7.7.7\n";
        c.files["/displayhub.tmp"] = "[server]\nhost = mez";
        ConfigStore s(&c, defaults(), opts());
        uint32_t now = 0;
        s.begin(now);
        CHECK(s.get("server", "host") == "7.7.7.7" && !c.files.count("/displayhub.tmp"), ".tmp avanzato: cancellato");
    }
    {
        FakeCard c;
        c.present = true;
        c.files["/displayhub.txt"] = std::string("[server]\nhost = \0\x01rotto", 22);
        c.files["/displayhub.bak"] = "[device]\nid = sala-01\n[server]\nhost = 6.6.6.6\n";
        ConfigStore s(&c, defaults(), opts());
        uint32_t now = 0;
        s.begin(now);
        CHECK(s.get("server", "host") == "6.6.6.6" && c.files.count("/displayhub.bad-1.txt"),
              "file illeggibile: usata la .bak, rovinato conservato come .bad-1");
        s.tick(now + 1);
        CHECK(c.files.count("/displayhub.txt") && c.files["/displayhub.txt"].find("6.6.6.6") != std::string::npos,
              "file principale riscritto dalla copia");
    }
    {
        FakeCard c;
        c.present = true;
        ConfigStore s(&c, defaults(), opts());
        uint32_t now = 0;
        s.begin(now);
        s.set("server", "port", "16000");
        c.fail_rename_after = 1;  // tmp scritto, file->bak ok, poi "si spegne"
        run(s, now, 2400);
        CHECK(s.status() == CardStatus::Absent && s.dirty(), "scheda tolta durante il salvataggio: modifiche in RAM");
        c.present = true;
        c.fail_rename_after = -1;
        run(s, now, 2200);
        CHECK(!c.files.count("/displayhub.tmp") || c.files.count("/displayhub.txt"), "al rientro nessun file perso");
    }
}

static void test_unusable_readonly_eject() {
    printf("\n== Scheda non formattata, protetta, espulsa ==\n");
    FakeCard c;
    c.present = true;
    c.formatted = false;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    CHECK(s.status() == CardStatus::Unusable && s.message().find("FAT32") != std::string::npos, "non formattata");
    s.takeEvents();
    run(s, now, 6000);
    CHECK(s.takeEvents().empty(), "nessun messaggio ripetuto a ogni controllo");
    CHECK(s.format(now) && s.status() == CardStatus::Ready, "formattata e pronta");

    printf("-- protetta in scrittura\n");
    c.readonly = true;
    s.set("server", "port", "17000");
    s.takeEvents();
    run(s, now, 2400);
    auto ev = s.takeEvents();
    CHECK(hasEvent(ev, Event::SaveFailed) && s.dirty(), "salvataggio non riuscito segnalato");
    run(s, now, 3000);
    CHECK(s.takeEvents().empty(), "nuovo tentativo non prima di 5 s");
    c.readonly = false;
    run(s, now, 2400);
    CHECK(!s.dirty(), "riuscito quando la protezione viene tolta");

    printf("-- espulsione\n");
    s.set("server", "port", "18000");
    s.eject(now);
    CHECK(s.status() == CardStatus::Ejected && c.files["/displayhub.txt"].find("18000") != std::string::npos,
          "espelli: salvato e smontato");
    run(s, now, 6000);
    CHECK(s.status() == CardStatus::Ejected && !c.mounted, "ancora inserita: non viene rimontata");
    c.present = false;
    run(s, now, 2200);
    CHECK(s.status() == CardStatus::Absent, "tolta: solo RAM");
    c.present = true;
    run(s, now, 2200);
    CHECK(s.status() == CardStatus::Ready, "reinserita: di nuovo in uso");
}

static void test_empty_list() {
    printf("\n== Elenco svuotato ==\n");
    FakeCard c;
    c.present = true;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    s.listRemove("wifi", "network", "casa", true);
    s.tick(now + 1);
    CHECK(s.getAll("wifi", "network").empty(), "nessuna rete (non ricompare quella della build)");
    ConfigStore s2(&c, defaults(), opts());
    s2.begin(now);
    CHECK(s2.getAll("wifi", "network").empty(), "anche dopo il riavvio");
}

static void test_replace_all() {
    printf("\n== Sostituzione completa (editor della web UI) ==\n");
    FakeCard c;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    s.replaceAll("# scritto dalla web UI\n[server]\nhost = 10.1.1.1\n[wifi]\nnetwork = Treno | t\nnetwork = Casa | pw-casa\n");
    CHECK(s.get("server", "host") == "10.1.1.1" && s.get("server", "port") == "12000",
          "valori nuovi; chiave tolta dal testo -> valore della build");
    CHECK(s.getAll("wifi", "network").size() == 2 && listId(s.getAll("wifi", "network")[0]) == "treno", "elenco e ordine");
    CHECK(s.doc().serialize().find("# scritto dalla web UI") != std::string::npos, "commenti del testo conservati");
    CHECK(s.dirty(), "segnata da salvare");
    printf("-- poi microSD inserita con un file di questo display\n");
    c.present = true;
    c.files["/displayhub.txt"] = "[device]\nid = sala-01\n[server]\nhost = 192.168.1.9\nport = 13000\n[vpn]\nenabled = 0\n";
    run(s, now, 2200);
    s.decide(Decision::Merge, now);
    CHECK(s.get("server", "host") == "10.1.1.1", "unione: il valore scritto dalla web UI prevale");
    CHECK(s.get("vpn", "enabled") == "0" && s.get("server", "port") == "13000",
          "unione: le voci che la web UI non ha toccato restano quelle del file");
    CHECK(listId(s.getAll("wifi", "network")[0]) == "treno", "unione: elenco della web UI");
    FakeCard c2;
    ConfigStore s2(&c2, defaults(), opts());
    s2.begin(0);
    uint32_t rev = s2.revision();
    s2.replaceAll(s2.doc().serialize());
    CHECK(s2.revision() == rev && !s2.dirty(), "testo identico: nessuna modifica");
}

static void test_loader_no_owner() {
    printf("\n== Loader (nessun identificativo) ==\n");
    FakeCard c;
    StoreOptions o = opts();
    o.device_id = "";
    ConfigStore s(&c, defaults(), o);
    uint32_t now = 0;
    s.begin(now);
    c.present = true;
    c.files["/displayhub.txt"] = "[device]\nid = cucina-02\n";
    run(s, now, 2200);
    CHECK(s.status() == CardStatus::Ready, "il loader accetta qualsiasi file senza chiedere");
}

static void test_list_order() {
    printf("\n== Ordine degli elenchi (priorità reti) ==\n");
    FakeCard c;
    ConfigStore s(&c, defaults(), opts());
    uint32_t now = 0;
    s.begin(now);
    s.listSetAll("wifi", "network", {"Hotspot | h", "Casa | pw-casa", "Treno | t"});
    auto n = s.getAll("wifi", "network");
    CHECK(n.size() == 3 && listId(n[0]) == "hotspot" && listId(n[1]) == "casa" && listId(n[2]) == "treno",
          "nuovo elenco con aggiunte e ordine");
    s.listSetAll("wifi", "network", {"Treno | t", "Hotspot | h"});
    n = s.getAll("wifi", "network");
    CHECK(n.size() == 2 && listId(n[0]) == "treno" && listId(n[1]) == "hotspot", "rimozione e riordino");

    printf("-- unione con un file trovato dopo\n");
    c.present = true;
    c.files["/displayhub.txt"] = "[device]\nid = sala-01\n[wifi]\nnetwork = Ufficio | u\nnetwork = Hotspot | h-vecchia\nnetwork = Casa | pw-casa\n";
    run(s, now, 2200);
    s.decide(Decision::Merge, now);
    n = s.getAll("wifi", "network");
    std::string order;
    for (auto& e : n) order += listId(e) + ",";
    CHECK(order == "treno,hotspot,ufficio,", "ordine della sessione applicato, voce solo-file ('Ufficio') in fondo, 'Casa' tolta");
    bool pw_new = false;
    for (auto& e : n) pw_new |= e == "Hotspot | h";
    CHECK(pw_new, "password aggiornata nella sessione prevale");
}

int main() {
    test_ini();
    test_boot_without_card();
    test_boot_with_own_file();
    test_hot_insert_merge();
    test_hot_insert_no_changes();
    test_foreign();
    test_removal_and_swap();
    test_recovery();
    test_unusable_readonly_eject();
    test_empty_list();
    test_replace_all();
    test_loader_no_owner();
    test_list_order();
    printf("\n%d ok, %d falliti\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
