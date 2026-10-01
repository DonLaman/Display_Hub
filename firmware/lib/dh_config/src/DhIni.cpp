// DhIni — vedi DhIni.h
#include "DhIni.h"

#include <cctype>
#include <cstdlib>

namespace dhcfg {

std::string lower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)std::tolower((unsigned char)c);
    return r;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

void IniDoc::clear() { secs_.clear(); }

bool IniDoc::empty() const {
    for (const auto& s : secs_)
        for (const auto& l : s.lines)
            if (l.kind == Line::KeyValue) return false;
    return true;
}

std::string IniDoc::parseValue(const std::string& v0) {
    std::string v = trim(v0);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < v.size(); i++) {
            if (v[i] == '\\' && i + 2 < v.size() && (v[i + 1] == '"' || v[i + 1] == '\\')) {
                out += v[++i];
            } else {
                out += v[i];
            }
        }
        return out;
    }
    return v;
}

std::string IniDoc::formatValue(const std::string& v) {
    bool needs_quotes = !v.empty() && (std::isspace((unsigned char)v.front()) ||
                                       std::isspace((unsigned char)v.back()) || v.front() == '"');
    if (!needs_quotes) return v;
    std::string out = "\"";
    for (char c : v) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

void IniDoc::parse(const std::string& text) {
    secs_.clear();
    secs_.push_back(Section());  // preambolo
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!raw.empty() && raw.back() == '\r') raw.pop_back();
        if (nl == std::string::npos && raw.empty()) break;  // niente riga vuota finta in fondo
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        std::string t = trim(raw);
        if (!t.empty() && t.front() == '[' && t.back() == ']') {
            Section s;
            s.name = lower(trim(t.substr(1, t.size() - 2)));
            s.header = raw;
            secs_.push_back(s);
            continue;
        }
        Line l;
        l.raw = raw;
        size_t eq = t.find('=');
        if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos && eq > 0) {
            l.kind = Line::KeyValue;
            l.key = lower(trim(t.substr(0, eq)));
            l.value = parseValue(t.substr(eq + 1));
        }
        secs_.back().lines.push_back(l);
    }
}

std::string IniDoc::serialize() const {
    std::string out;
    for (size_t i = 0; i < secs_.size(); i++) {
        const Section& s = secs_[i];
        if (i > 0 || !s.name.empty()) out += (s.header.empty() ? "[" + s.name + "]" : s.header) + "\n";
        for (const auto& l : s.lines) out += l.raw + "\n";
    }
    return out;
}

IniDoc::Section* IniDoc::find(const std::string& section) {
    std::string n = lower(section);
    for (auto& s : secs_)
        if (s.name == n) return &s;
    return nullptr;
}

const IniDoc::Section* IniDoc::find(const std::string& section) const {
    std::string n = lower(section);
    for (const auto& s : secs_)
        if (s.name == n) return &s;
    return nullptr;
}

IniDoc::Section& IniDoc::findOrCreate(const std::string& section) {
    if (Section* s = find(section)) return *s;
    if (secs_.empty()) secs_.push_back(Section());
    // Riga vuota di separazione dalla sezione precedente, per leggibilità.
    Section& last = secs_.back();
    if (!(last.name.empty() && last.lines.empty()) && (last.lines.empty() || !trim(last.lines.back().raw).empty())) {
        Line blank;
        last.lines.push_back(blank);
    }
    Section s;
    s.name = lower(section);
    secs_.push_back(s);
    return secs_.back();
}

void IniDoc::insertBeforeTrailingBlanks(Section& s, const Line& l) {
    size_t at = s.lines.size();
    while (at > 0 && s.lines[at - 1].kind == Line::Raw && trim(s.lines[at - 1].raw).empty()) at--;
    s.lines.insert(s.lines.begin() + at, l);
}

bool IniDoc::has(const std::string& section, const std::string& key) const {
    const Section* s = find(section);
    if (!s) return false;
    std::string k = lower(key);
    for (const auto& l : s->lines)
        if (l.kind == Line::KeyValue && l.key == k) return true;
    return false;
}

std::string IniDoc::get(const std::string& section, const std::string& key, const std::string& def) const {
    const Section* s = find(section);
    if (!s) return def;
    std::string k = lower(key);
    for (const auto& l : s->lines)
        if (l.kind == Line::KeyValue && l.key == k) return l.value;
    return def;
}

bool IniDoc::getBool(const std::string& section, const std::string& key, bool def) const {
    if (!has(section, key)) return def;
    std::string v = lower(get(section, key));
    if (v == "1" || v == "true" || v == "si" || v == "sì" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return def;
}

long IniDoc::getInt(const std::string& section, const std::string& key, long def) const {
    if (!has(section, key)) return def;
    std::string v = get(section, key);
    char* end = nullptr;
    long r = std::strtol(v.c_str(), &end, 10);
    return (end && end != v.c_str() && *end == '\0') ? r : def;
}

std::vector<std::string> IniDoc::getAll(const std::string& section, const std::string& key) const {
    std::vector<std::string> r;
    const Section* s = find(section);
    if (!s) return r;
    std::string k = lower(key);
    for (const auto& l : s->lines)
        if (l.kind == Line::KeyValue && l.key == k) r.push_back(l.value);
    return r;
}

void IniDoc::set(const std::string& section, const std::string& key, const std::string& value) {
    setAll(section, key, std::vector<std::string>{value});
}

void IniDoc::setAll(const std::string& section, const std::string& key, const std::vector<std::string>& values) {
    Section& s = findOrCreate(section);
    std::string k = lower(key);
    // Riusa la forma originale della chiave (es. "SSID" scritto a mano) se c'era.
    std::string shown_key = key;
    size_t first = s.lines.size();
    for (size_t i = 0; i < s.lines.size(); i++) {
        if (s.lines[i].kind == Line::KeyValue && s.lines[i].key == k) {
            if (first == s.lines.size()) {
                first = i;
                std::string t = trim(s.lines[i].raw);
                shown_key = trim(t.substr(0, t.find('=')));
            }
        }
    }
    // Righe invariate: se i valori coincidono con quelli presenti non tocco il
    // testo (conserva spaziature fatte a mano).
    if (getAll(section, key) == values) return;

    std::vector<Line> fresh;
    for (const auto& v : values) {
        Line l;
        l.kind = Line::KeyValue;
        l.key = k;
        l.value = v;
        l.raw = shown_key + " = " + formatValue(v);
        fresh.push_back(l);
    }
    std::vector<Line> out;
    bool inserted = false;
    for (size_t i = 0; i < s.lines.size(); i++) {
        if (s.lines[i].kind == Line::KeyValue && s.lines[i].key == k) {
            if (!inserted) {
                out.insert(out.end(), fresh.begin(), fresh.end());
                inserted = true;
            }
            continue;
        }
        out.push_back(s.lines[i]);
    }
    s.lines = out;
    if (!inserted) {
        for (const auto& l : fresh) insertBeforeTrailingBlanks(s, l);
    }
}

void IniDoc::remove(const std::string& section, const std::string& key) {
    Section* s = find(section);
    if (!s) return;
    std::string k = lower(key);
    std::vector<Line> out;
    for (const auto& l : s->lines)
        if (!(l.kind == Line::KeyValue && l.key == k)) out.push_back(l);
    s->lines = out;
}

std::vector<std::string> IniDoc::sections() const {
    std::vector<std::string> r;
    for (const auto& s : secs_)
        if (!s.name.empty()) r.push_back(s.name);
    return r;
}

std::vector<std::string> IniDoc::keys(const std::string& section) const {
    std::vector<std::string> r;
    const Section* s = find(section);
    if (!s) return r;
    for (const auto& l : s->lines) {
        if (l.kind != Line::KeyValue) continue;
        bool seen = false;
        for (const auto& k : r) seen = seen || k == l.key;
        if (!seen) r.push_back(l.key);
    }
    return r;
}

}  // namespace dhcfg
