// DhConfigSerial — vedi DhConfigSerial.h
#include "DhConfigSerial.h"

#include "DhConfig.h"
#include "mbedtls/base64.h"

namespace DhConfigSerial {

namespace {

void send(JsonDocument& d, Print& out) {
    serializeJson(d, out);
    out.println();
}

bool reply_error(const String& cmd, const String& err, Print& out) {
    JsonDocument d;
    d["type"] = "error";
    d["cmd"] = cmd;
    d["error"] = err;
    send(d, out);
    return true;
}

bool reply_ok(const char* type, const String& cmd, Print& out, bool ok = true, const String& err = "") {
    JsonDocument d;
    d["type"] = type;
    d["cmd"] = cmd;
    d["ok"] = ok;
    if (!ok) d["error"] = err;
    d["revision"] = DhConfig::revision();
    send(d, out);
    return true;
}

const char* backend_name() {
    switch (DhConfig::backend()) {
        case DhConfig::Backend::Sd:  return "sd";
        case DhConfig::Backend::Nvs: return "nvs";
        default:                     return "ram";
    }
}

String b64(const uint8_t* data, size_t len) {
    size_t need = 4 * ((len + 2) / 3) + 1, n = 0;
    std::string buf(need, '\0');
    if (mbedtls_base64_encode((unsigned char*)&buf[0], need, &n, data, len) != 0) return "";
    return String(buf.c_str()).substring(0, n);
}

bool unb64(const String& in, std::string& out) {
    size_t need = in.length() * 3 / 4 + 3, n = 0;
    out.assign(need, '\0');
    if (mbedtls_base64_decode((unsigned char*)&out[0], need, &n, (const unsigned char*)in.c_str(), in.length()) != 0)
        return false;
    out.resize(n);
    return true;
}

}  // namespace

bool handle(JsonDocument& in, Print& out) {
    String cmd = in["cmd"] | "";
    if (!(cmd.startsWith("file_") || cmd.startsWith("storage_") || cmd.startsWith("config_"))) return false;
    String path = in["path"] | "";
    String err;

    // ------------------------------------------------------------ storage_*
    if (cmd == "storage_info") {
        JsonDocument d;
        d["type"] = "storage_info";
        d["backend"] = backend_name();
        d["status"] = DhConfig::statusText();
        d["message"] = DhConfig::message();
        d["dirty"] = DhConfig::dirty();
        d["persistent"] = DhConfig::persistent();
        d["files"] = DhConfig::filesAvailable();
        d["revision"] = DhConfig::revision();
        if (DhConfig::status() == dhcfg::CardStatus::AwaitingDecision) {
            d["pending_owner"] = DhConfig::pendingOwner();
            d["pending_foreign"] = DhConfig::pendingForeign();
        }
        uint64_t total = 0, free_b = 0;
        if (DhConfig::cardSpace(total, free_b)) {
            d["card_total"] = (double)total;
            d["card_free"] = (double)free_b;
        }
        send(d, out);
        return true;
    }
    if (cmd == "storage_save") { DhConfig::saveNow(); return reply_ok("storage_result", cmd, out); }
    if (cmd == "storage_eject") { DhConfig::eject(); return reply_ok("storage_result", cmd, out); }
    if (cmd == "storage_reload") {
        bool ok = DhConfig::reload();
        return reply_ok("storage_result", cmd, out, ok, "microSD non in uso");
    }
    if (cmd == "storage_format") {
        bool ok = DhConfig::format();
        return reply_ok("storage_result", cmd, out, ok, "formattazione possibile solo con una microSD non formattata");
    }
    if (cmd == "storage_decide") {
        String dec = in["decision"] | "";
        if (DhConfig::status() != dhcfg::CardStatus::AwaitingDecision) return reply_error(cmd, "nessuna scelta in attesa", out);
        dhcfg::Decision d;
        if (dec == "merge") d = dhcfg::Decision::Merge;
        else if (dec == "use_card") d = dhcfg::Decision::UseCard;
        else if (dec == "overwrite") d = dhcfg::Decision::Overwrite;
        else if (dec == "ignore") d = dhcfg::Decision::Ignore;
        else return reply_error(cmd, "decision: merge | use_card | overwrite | ignore", out);
        DhConfig::decide(d);
        return reply_ok("storage_result", cmd, out);
    }

    // ------------------------------------------------------------ config_*
    String section = in["section"] | "", key = in["key"] | "";
    if (cmd == "config_get") {
        JsonDocument d;
        d["type"] = "config";
        d["backend"] = backend_name();
        d["status"] = DhConfig::statusText();
        d["persistent"] = DhConfig::persistent();
        d["revision"] = DhConfig::revision();
        d["text"] = DhConfig::serialize();
        send(d, out);
        return true;
    }
    if (cmd == "config_replace") {
        if (!in["text"].is<const char*>()) return reply_error(cmd, "manca text", out);
        DhConfig::replaceAll(in["text"].as<String>());
        return reply_ok("config_result", cmd, out);
    }
    if (cmd.startsWith("config_") && (!section.length() || !key.length()))
        return reply_error(cmd, "servono section e key", out);
    if (cmd == "config_set") {
        if (!in["value"].is<const char*>()) return reply_error(cmd, "manca value", out);
        DhConfig::set(section.c_str(), key.c_str(), in["value"].as<String>(), true);
        return reply_ok("config_result", cmd, out);
    }
    if (cmd == "config_remove") {
        DhConfig::remove(section.c_str(), key.c_str(), true);
        return reply_ok("config_result", cmd, out);
    }
    if (cmd == "config_list_put") {
        String entry = in["entry"] | "";
        if (!entry.length()) return reply_error(cmd, "manca entry", out);
        DhConfig::listPut(section.c_str(), key.c_str(), entry, true);
        return reply_ok("config_result", cmd, out);
    }
    if (cmd == "config_list_remove") {
        String id = in["id"] | "";
        if (!id.length()) return reply_error(cmd, "manca id", out);
        DhConfig::listRemove(section.c_str(), key.c_str(), id, true);
        return reply_ok("config_result", cmd, out);
    }
    if (cmd == "config_list_set") {
        if (!in["entries"].is<JsonArray>()) return reply_error(cmd, "manca entries (array)", out);
        std::vector<String> e;
        for (JsonVariant v : in["entries"].as<JsonArray>()) e.push_back(v.as<String>());
        DhConfig::listSetAll(section.c_str(), key.c_str(), e, true);
        return reply_ok("config_result", cmd, out);
    }

    // ------------------------------------------------------------ file_*
    if (cmd == "file_list") {
        std::vector<DhConfig::FileEntry> entries;
        if (!path.length()) path = "/";
        if (!DhConfig::fileList(path, entries, err)) return reply_error(cmd, err, out);
        JsonDocument d;
        d["type"] = "file_list";
        d["path"] = path;
        JsonArray a = d["entries"].to<JsonArray>();
        for (const auto& f : entries) {
            JsonObject o = a.add<JsonObject>();
            o["name"] = f.name;
            o["dir"] = f.dir;
            o["size"] = f.size;
        }
        send(d, out);
        return true;
    }
    if (cmd == "file_read") {
        uint32_t offset = in["offset"] | 0;
        uint32_t len = in["length"] | kChunk;
        if (len > kChunk) len = kChunk;
        std::string data;
        uint32_t total = 0;
        if (!DhConfig::fileRead(path, offset, len, data, total, err)) return reply_error(cmd, err, out);
        JsonDocument d;
        d["type"] = "file_data";
        d["path"] = path;
        d["offset"] = offset;
        d["total"] = total;
        d["data"] = b64((const uint8_t*)data.data(), data.size());
        d["eof"] = offset + data.size() >= total;
        send(d, out);
        return true;
    }
    if (cmd == "file_write") {
        uint32_t offset = in["offset"] | 0;
        bool final = in["final"] | false;
        std::string data;
        if (!unb64(in["data"] | "", data)) return reply_error(cmd, "data non è base64 valido", out);
        if (data.size() > kChunk) return reply_error(cmd, "blocco troppo grande (max " + String(kChunk) + " byte)", out);
        if (!DhConfig::fileWrite(path, offset, (const uint8_t*)data.data(), data.size(), final, err))
            return reply_error(cmd, err, out);
        JsonDocument d;
        d["type"] = "file_write_result";
        d["ok"] = true;
        d["path"] = path;
        d["next_offset"] = offset + (uint32_t)data.size();
        d["final"] = final;
        send(d, out);
        return true;
    }
    if (cmd == "file_remove") {
        bool ok = DhConfig::fileRemove(path, err);
        return ok ? reply_ok("file_result", cmd, out) : reply_error(cmd, err, out);
    }
    if (cmd == "file_mkdir") {
        bool ok = DhConfig::fileMkdir(path, err);
        return ok ? reply_ok("file_result", cmd, out) : reply_error(cmd, err, out);
    }
    return reply_error(cmd, "comando sconosciuto", out);
}

}  // namespace DhConfigSerial
