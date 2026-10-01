// DhConfigStore — vedi DhConfigStore.h
#include "DhConfigStore.h"

namespace dhcfg {

// ------------------------------------------------------------------ elenchi

bool isListKey(const std::string& section, const std::string& key) {
    static const char* const kLists[][2] = {
        {"wifi", "network"},  // SSID | password
        {"bt", "device"},     // MAC | nome
        {"bt", "bond"},       // chiavi di accoppiamento (NimBLE)
    };
    std::string s = lower(section), k = lower(key);
    for (const auto& l : kLists)
        if (s == l[0] && k == l[1]) return true;
    return false;
}

std::string listId(const std::string& entry) {
    size_t bar = entry.find('|');
    return lower(trim(bar == std::string::npos ? entry : entry.substr(0, bar)));
}

void listApply(IniDoc& doc, const IniDoc& base, const std::string& s, const std::string& k,
               const std::string& id, const std::string* entry) {
    // Se il documento non ha ancora l'elenco si parte da quello della build:
    // aggiungere una rete non deve far sparire quella incorporata nel firmware.
    std::vector<std::string> cur = doc.has(s, k) ? doc.getAll(s, k) : base.getAll(s, k);
    std::vector<std::string> out;
    bool found = false;
    std::string want = lower(trim(id));
    for (const auto& e : cur) {
        if (trim(e).empty()) continue;  // segnaposto di "elenco vuoto"
        if (listId(e) == want) {
            if (entry && !found) out.push_back(*entry);
            found = true;
            continue;
        }
        out.push_back(e);
    }
    if (!found && entry) out.push_back(*entry);
    // Elenco svuotato: resta una voce vuota, altrimenti tornerebbe quello della build.
    if (out.empty()) out.push_back("");
    doc.setAll(s, k, out);
}

void listReorder(IniDoc& doc, const IniDoc& base, const std::string& s, const std::string& k,
                 const std::vector<std::string>& ids) {
    std::vector<std::string> cur = doc.has(s, k) ? doc.getAll(s, k) : base.getAll(s, k);
    std::vector<std::string> out, rest;
    std::vector<bool> used(cur.size(), false);
    for (const auto& id : ids) {
        std::string want = lower(trim(id));
        for (size_t i = 0; i < cur.size(); i++) {
            if (!used[i] && !trim(cur[i]).empty() && listId(cur[i]) == want) {
                out.push_back(cur[i]);
                used[i] = true;
                break;
            }
        }
    }
    for (size_t i = 0; i < cur.size(); i++)
        if (!used[i] && !trim(cur[i]).empty()) out.push_back(cur[i]);
    if (out.empty()) out.push_back("");
    doc.setAll(s, k, out);
}

// ------------------------------------------------------------------ giornale

void Journal::put(const Op& op) {
    for (size_t i = 0; i < ops_.size(); i++) {
        const Op& o = ops_[i];
        if (o.list == op.list && o.s == op.s && o.k == op.k && o.id == op.id) {
            ops_.erase(ops_.begin() + i);
            break;
        }
    }
    ops_.push_back(op);
}

void Journal::set(const std::string& s, const std::string& k, const std::string& v) {
    Op o;
    o.s = lower(s); o.k = lower(k); o.value = v;
    put(o);
}

void Journal::remove(const std::string& s, const std::string& k) {
    Op o;
    o.s = lower(s); o.k = lower(k); o.removed = true;
    put(o);
}

void Journal::listPut(const std::string& s, const std::string& k, const std::string& entry) {
    Op o;
    o.s = lower(s); o.k = lower(k); o.list = true; o.id = listId(entry); o.value = entry;
    put(o);
}

void Journal::listRemove(const std::string& s, const std::string& k, const std::string& id) {
    Op o;
    o.s = lower(s); o.k = lower(k); o.list = true; o.id = lower(trim(id)); o.removed = true;
    put(o);
}

void Journal::listOrder(const std::string& s, const std::string& k, const std::vector<std::string>& ids) {
    Op o;
    o.s = lower(s); o.k = lower(k); o.list = true; o.order = true; o.id = "\x01order";
    for (const auto& id : ids) o.value += lower(trim(id)) + "\n";
    put(o);
}

void Journal::applyTo(IniDoc& doc, const IniDoc& listBase) const {
    // L'ordine si applica per ultimo, dopo aggiunte e rimozioni.
    const Op* order_op = nullptr;
    for (const auto& o : ops_) {
        if (o.list && o.order) { order_op = &o; continue; }
        if (o.list) listApply(doc, listBase, o.s, o.k, o.id, o.removed ? nullptr : &o.value);
        else if (o.removed) doc.remove(o.s, o.k);
        else doc.set(o.s, o.k, o.value);
    }
    for (const auto& o : ops_) {
        if (!(o.list && o.order)) continue;
        std::vector<std::string> ids;
        size_t pos = 0;
        while (pos < o.value.size()) {
            size_t nl = o.value.find('\n', pos);
            ids.push_back(o.value.substr(pos, nl - pos));
            pos = nl + 1;
        }
        listReorder(doc, listBase, o.s, o.k, ids);
    }
    (void)order_op;
}

void Journal::clear() { ops_.clear(); }
bool Journal::empty() const { return ops_.empty(); }

// ------------------------------------------------------------------ archivio

ConfigStore::ConfigStore(ICard* card, const IniDoc& defaults, const StoreOptions& opt)
    : card_(card), defaults_(defaults), opt_(opt) {
    status_ = card ? CardStatus::Absent : CardStatus::NotSupported;
    message_ = card ? "Nessuna microSD: le modifiche valgono fino al riavvio" : "";
}

std::string ConfigStore::tmpPath() const {
    size_t dot = opt_.path.rfind('.');
    return (dot == std::string::npos ? opt_.path : opt_.path.substr(0, dot)) + ".tmp";
}

std::string ConfigStore::bakPath() const {
    size_t dot = opt_.path.rfind('.');
    return (dot == std::string::npos ? opt_.path : opt_.path.substr(0, dot)) + ".bak";
}

void ConfigStore::emit(Event::Type t, const std::string& text) { events_.push_back(Event{t, text}); }

std::vector<Event> ConfigStore::takeEvents() {
    std::vector<Event> e;
    e.swap(events_);
    return e;
}

void ConfigStore::setStatus(CardStatus s, const std::string& msg) {
    status_ = s;
    message_ = msg;
}

void ConfigStore::changed(bool urgent) {
    dirty_ = true;
    urgent_ = urgent_ || urgent;
    last_change_ms_ = now_;
    revision_++;
}

// --- lettura

std::string ConfigStore::get(const std::string& s, const std::string& k, const std::string& def) const {
    return doc_.has(s, k) ? doc_.get(s, k) : defaults_.get(s, k, def);
}

bool ConfigStore::getBool(const std::string& s, const std::string& k, bool def) const {
    return doc_.has(s, k) ? doc_.getBool(s, k, def) : defaults_.getBool(s, k, def);
}

long ConfigStore::getInt(const std::string& s, const std::string& k, long def) const {
    return doc_.has(s, k) ? doc_.getInt(s, k, def) : defaults_.getInt(s, k, def);
}

std::vector<std::string> ConfigStore::getAll(const std::string& s, const std::string& k) const {
    std::vector<std::string> src = doc_.has(s, k) ? doc_.getAll(s, k) : defaults_.getAll(s, k);
    std::vector<std::string> r;
    for (const auto& v : src)
        if (!trim(v).empty()) r.push_back(v);
    return r;
}

// --- modifica

void ConfigStore::set(const std::string& s, const std::string& k, const std::string& v, bool urgent) {
    if (doc_.has(s, k) && doc_.get(s, k) == v) return;
    doc_.set(s, k, v);
    journal_.set(s, k, v);
    changed(urgent);
}

void ConfigStore::remove(const std::string& s, const std::string& k, bool urgent) {
    if (!doc_.has(s, k)) return;
    doc_.remove(s, k);
    journal_.remove(s, k);
    changed(urgent);
}

void ConfigStore::listPut(const std::string& s, const std::string& k, const std::string& entry, bool urgent) {
    std::string id = listId(entry);
    for (const auto& e : getAll(s, k))
        if (listId(e) == id && e == entry) return;  // già così
    listApply(doc_, defaults_, s, k, id, &entry);
    journal_.listPut(s, k, entry);
    changed(urgent);
}

void ConfigStore::listRemove(const std::string& s, const std::string& k, const std::string& id, bool urgent) {
    bool present = false;
    for (const auto& e : getAll(s, k)) present = present || listId(e) == lower(trim(id));
    if (!present) return;
    listApply(doc_, defaults_, s, k, id, nullptr);
    journal_.listRemove(s, k, id);
    changed(urgent);
}

void ConfigStore::listSetAll(const std::string& s, const std::string& k, const std::vector<std::string>& entries,
                             bool urgent) {
    std::vector<std::string> cur = getAll(s, k);
    std::vector<std::string> clean;
    for (const auto& e : entries)
        if (!trim(e).empty()) clean.push_back(e);
    if (cur == clean) return;
    // Rimozioni e aggiunte/modifiche voce per voce (così l'unione con un file
    // trovato più tardi conserva le voci del file), poi l'ordine.
    for (const auto& e : cur) {
        bool keep = false;
        for (const auto& n : clean) keep = keep || listId(n) == listId(e);
        if (!keep) {
            listApply(doc_, defaults_, s, k, listId(e), nullptr);
            journal_.listRemove(s, k, listId(e));
        }
    }
    std::vector<std::string> ids;
    for (const auto& n : clean) {
        listApply(doc_, defaults_, s, k, listId(n), &n);
        journal_.listPut(s, k, n);
        ids.push_back(listId(n));
    }
    listReorder(doc_, defaults_, s, k, ids);
    journal_.listOrder(s, k, ids);
    changed(urgent);
}

void ConfigStore::replaceAll(const std::string& text, bool urgent) {
    IniDoc fresh;
    fresh.parse(text);
    // Tutte le coppie sezione/chiave presenti prima o dopo.
    std::vector<std::pair<std::string, std::string>> keys;
    auto collect = [&keys](const IniDoc& d) {
        for (const auto& s : d.sections())
            for (const auto& k : d.keys(s)) {
                bool seen = false;
                for (const auto& p : keys) seen = seen || (p.first == s && p.second == k);
                if (!seen) keys.push_back({s, k});
            }
    };
    collect(doc_);
    collect(fresh);
    bool any = false;
    for (const auto& p : keys) {
        const std::string& s = p.first;
        const std::string& k = p.second;
        if (isListKey(s, k)) {
            if (fresh.has(s, k) == doc_.has(s, k) && fresh.getAll(s, k) == doc_.getAll(s, k)) continue;
            // Come listSetAll(): rimozioni e aggiunte voce per voce, poi l'ordine
            // (all'unione le voci del file non toccate restano).
            std::vector<std::string> before = getAll(s, k), after;
            for (const auto& e : fresh.getAll(s, k))
                if (!trim(e).empty()) after.push_back(e);
            if (!fresh.has(s, k)) after = defaults_.getAll(s, k);  // elenco tolto dal testo: quello della build
            std::vector<std::string> ids;
            for (const auto& e : before) {
                bool keep = false;
                for (const auto& n : after) keep = keep || listId(n) == listId(e);
                if (!keep) journal_.listRemove(s, k, listId(e));
            }
            for (const auto& n : after) {
                journal_.listPut(s, k, n);
                ids.push_back(listId(n));
            }
            journal_.listOrder(s, k, ids);
            any = true;
        } else if (fresh.has(s, k)) {
            if (doc_.has(s, k) && doc_.get(s, k) == fresh.get(s, k)) continue;
            journal_.set(s, k, fresh.get(s, k));
            any = true;
        } else if (doc_.has(s, k)) {
            journal_.remove(s, k);
            any = true;
        }
    }
    bool same_text = doc_.serialize() == fresh.serialize();
    doc_ = fresh;
    if (any || !same_text) changed(urgent);
}

// --- scheda

void ConfigStore::begin(uint32_t now) {
    now_ = now;
    if (!card_) return;
    last_probe_ms_ = now;
    probed_once_ = true;
    tryMount(true);
}

void ConfigStore::tick(uint32_t now) {
    now_ = now;
    if (!card_) return;

    if (status_ == CardStatus::AwaitingDecision && now - pending_since_ms_ >= opt_.decision_timeout_ms) {
        // Nessuna risposta: nostro file -> unisci; file di un altro display -> lascia stare.
        decide(pending_foreign_ ? Decision::Ignore : Decision::Merge, now);
    }

    if (!probed_once_ || now - last_probe_ms_ >= opt_.probe_ms) {
        last_probe_ms_ = now;
        probed_once_ = true;
        if (mounted_) {
            uint32_t sig = 0;
            if (!card_->probe(sig)) {
                handleRemoved("microSD rimossa: le modifiche valgono fino al riavvio");
            } else if (sig != signature_) {
                handleRemoved("microSD sostituita");
                tryMount(false);
            }
        } else {
            tryMount(false);
        }
    }

    if (mounted_ && status_ == CardStatus::Ready && dirty_ &&
        (urgent_ || now - last_change_ms_ >= opt_.save_delay_ms) &&
        (last_save_fail_ms_ == 0 || now - last_save_fail_ms_ >= opt_.save_retry_ms)) {
        writeFile();
    }
}

void ConfigStore::tryMount(bool at_boot) {
    MountResult r = card_->mount();
    switch (r) {
        case MountResult::NoCard:
            if (status_ == CardStatus::Unusable || status_ == CardStatus::Ejected) {
                setStatus(CardStatus::Absent, "Nessuna microSD: le modifiche valgono fino al riavvio");
                emit(Event::CardRemoved, "microSD tolta");
            } else if (status_ != CardStatus::Absent) {
                setStatus(CardStatus::Absent, "Nessuna microSD: le modifiche valgono fino al riavvio");
            }
            return;
        case MountResult::NoFilesystem:
        case MountResult::Error: {
            std::string msg = r == MountResult::NoFilesystem
                                  ? "microSD non formattata (serve FAT32): la configurazione resta in RAM"
                                  : "microSD non leggibile: la configurazione resta in RAM";
            if (status_ != CardStatus::Unusable || message_ != msg) {
                setStatus(CardStatus::Unusable, msg);
                emit(Event::CardUnusable, msg);
            }
            return;
        }
        case MountResult::Ok:
            break;
    }
    if (status_ == CardStatus::Ejected) {
        card_->unmount();  // espulsa e ancora inserita: resta smontata
        return;
    }
    mounted_ = true;
    uint32_t sig = 0;
    if (!card_->probe(sig)) {
        handleRemoved("microSD non leggibile");
        return;
    }
    signature_ = sig;
    onMounted(at_boot);
}

bool ConfigStore::readValid(const std::string& path, std::string& out) {
    out.clear();
    if (!card_->read(path, out)) return false;
    if (out.size() > opt_.max_size) return false;
    if (out.find('\0') != std::string::npos) return false;  // file binario/rovinato
    return true;
}

std::string ConfigStore::fileOwner(const IniDoc& d) const { return trim(d.get("device", "id")); }

void ConfigStore::adoptFile(const IniDoc& file, const std::string& why) {
    doc_ = file;
    journal_.clear();
    dirty_ = false;
    urgent_ = false;
    revision_++;
    setStatus(CardStatus::Ready, why);
    emit(Event::ConfigReplaced, why);
    emit(Event::CardInserted, why);
}

void ConfigStore::onMounted(bool at_boot) {
    const std::string p = opt_.path, tmp = tmpPath(), bak = bakPath();

    // Scrittura interrotta: .tmp senza file principale = ultimo salvataggio
    // completo nel .tmp ma rename non avvenuto; .tmp accanto al file = avanzo.
    if (!card_->exists(p) && card_->exists(tmp)) {
        std::string t;
        if (readValid(tmp, t) && card_->rename(tmp, p)) emit(Event::Recovered, "Configurazione ripristinata da un salvataggio interrotto");
        else card_->remove(tmp);
    } else if (card_->exists(tmp)) {
        card_->remove(tmp);
    }

    bool has_file = card_->exists(p);
    std::string text;
    bool ok = has_file && readValid(p, text);
    bool from_bak = false;

    if (has_file && !ok) {
        std::string stem = p.substr(0, p.rfind('.'));
        std::string bad;
        for (int n = 1; n < 100; n++) {
            bad = stem + ".bad-" + std::to_string(n) + ".txt";
            if (!card_->exists(bad)) break;
        }
        card_->rename(p, bad);
        std::string bt;
        if (card_->exists(bak) && readValid(bak, bt)) {
            text = bt;
            ok = true;
            from_bak = true;
            emit(Event::Recovered, "File illeggibile (conservato come " + bad + "): usata la copia precedente");
        } else {
            emit(Event::Recovered, "File illeggibile (conservato come " + bad + "): ripartito dallo stato attuale");
        }
    }

    if (!ok) {
        // Nessun file utilizzabile: lo si crea con lo stato attuale.
        setStatus(CardStatus::Ready, "microSD pronta");
        if (!opt_.device_id.empty() && fileOwner(doc_).empty()) doc_.set("device", "id", opt_.device_id);
        dirty_ = true;
        if (writeFile()) {
            setStatus(CardStatus::Ready, "Configurazione salvata sulla microSD");
            emit(Event::CardInserted, message_);
        }
        return;
    }

    IniDoc file;
    file.parse(text);
    std::string owner = fileOwner(file);
    bool foreign = !opt_.device_id.empty() && !owner.empty() && lower(owner) != lower(opt_.device_id);

    if (!foreign && journal_.empty()) {
        adoptFile(file, at_boot ? "Configurazione caricata dalla microSD" : "microSD inserita: configurazione caricata");
        if (from_bak) {
            dirty_ = true;  // riscrive il file principale dalla copia
            urgent_ = true;
        }
        return;
    }

    pending_file_ = file;
    pending_owner_ = owner;
    pending_foreign_ = foreign;
    pending_since_ms_ = now_;
    setStatus(CardStatus::AwaitingDecision,
              foreign ? "Questa microSD appartiene a \"" + owner + "\""
                      : "Sulla microSD c'è una configurazione di questo display");
    emit(Event::DecisionNeeded, message_);
}

void ConfigStore::handleRemoved(const std::string& why) {
    card_->unmount();
    mounted_ = false;
    bool was_known = status_ != CardStatus::Absent;
    pending_file_.clear();
    pending_owner_.clear();
    pending_foreign_ = false;
    setStatus(CardStatus::Absent, why);
    if (was_known) emit(Event::CardRemoved, why);
}

bool ConfigStore::writeFile() {
    const std::string p = opt_.path, tmp = tmpPath(), bak = bakPath();
    std::string data = doc_.serialize();
    bool ok = card_->write(tmp, data);
    if (ok && card_->exists(bak)) ok = card_->remove(bak);
    if (ok && card_->exists(p)) ok = card_->rename(p, bak);
    if (ok) ok = card_->rename(tmp, p);
    if (ok) {
        dirty_ = false;
        urgent_ = false;
        journal_.clear();
        last_save_ms_ = now_;
        last_save_fail_ms_ = 0;
        emit(Event::Saved, "Configurazione salvata");
        return true;
    }
    last_save_fail_ms_ = now_ ? now_ : 1;
    uint32_t sig = 0;
    if (!card_->probe(sig)) {
        handleRemoved("microSD rimossa durante il salvataggio: le modifiche restano in RAM");
    } else {
        emit(Event::SaveFailed, "Salvataggio non riuscito (microSD protetta in scrittura o piena?)");
    }
    return false;
}

void ConfigStore::decide(Decision d, uint32_t now) {
    now_ = now;
    if (status_ != CardStatus::AwaitingDecision) return;
    IniDoc file = pending_file_;
    pending_file_.clear();
    switch (d) {
        case Decision::Merge: {
            journal_.applyTo(file, defaults_);
            if (!opt_.device_id.empty()) file.set("device", "id", opt_.device_id);
            doc_ = file;
            journal_.clear();
            revision_++;
            setStatus(CardStatus::Ready, "Configurazioni unite e salvate sulla microSD");
            dirty_ = true;
            urgent_ = true;
            emit(Event::ConfigReplaced, message_);
            emit(Event::CardInserted, message_);
            break;
        }
        case Decision::UseCard:
            adoptFile(file, "Usata la configurazione della microSD");
            break;
        case Decision::Overwrite:
            if (!opt_.device_id.empty()) doc_.set("device", "id", opt_.device_id);
            setStatus(CardStatus::Ready, "Configurazione della microSD sostituita con quella attuale");
            dirty_ = true;
            urgent_ = true;
            emit(Event::CardInserted, message_);
            break;
        case Decision::Ignore:
            setStatus(CardStatus::Ignored, "microSD ignorata finché non viene tolta");
            break;
    }
    pending_owner_.clear();
    pending_foreign_ = false;
    if (dirty_ && urgent_ && mounted_ && status_ == CardStatus::Ready) writeFile();
}

void ConfigStore::saveNow(uint32_t now) {
    now_ = now;
    if (mounted_ && status_ == CardStatus::Ready) writeFile();
}

bool ConfigStore::reloadFromCard(uint32_t now) {
    now_ = now;
    if (!mounted_ || status_ != CardStatus::Ready) return false;
    std::string text;
    if (!readValid(opt_.path, text)) return false;
    IniDoc file;
    file.parse(text);
    adoptFile(file, "Configurazione ricaricata dalla microSD");
    return true;
}

void ConfigStore::eject(uint32_t now) {
    now_ = now;
    if (!mounted_) return;
    if (status_ == CardStatus::Ready && dirty_) writeFile();
    if (!mounted_) return;  // tolta durante il salvataggio
    card_->unmount();
    mounted_ = false;
    setStatus(CardStatus::Ejected, "Puoi togliere la microSD");
}

bool ConfigStore::format(uint32_t now) {
    now_ = now;
    if (status_ != CardStatus::Unusable) return false;
    if (!card_->format()) {
        emit(Event::CardUnusable, "Formattazione non riuscita");
        return false;
    }
    setStatus(CardStatus::Absent, "microSD formattata");
    tryMount(false);
    return status_ == CardStatus::Ready;
}

}  // namespace dhcfg
