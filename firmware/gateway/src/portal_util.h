#pragma once
// Parti pure del portale WiFi (nessuna dipendenza da WiFi/WebServer): testabili sul PC
// con test_host/portal/run.sh.
#include <Arduino.h>

// Gli SSID arrivano dall'aria: chiunque puo' chiamare la sua rete  "><script>...
// Tutto cio' che finisce nella pagina passa da qui.
static inline String portal_html_escape(const String& in) {
    String out;
    out.reserve(in.length() + 16);
    for (unsigned i = 0; i < in.length(); i++) {
        char c = in[i];
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default:   out += c;
        }
    }
    return out;
}

static inline String portal_option_html(const String& ssid, int rssi, bool secure) {
    String e = portal_html_escape(ssid);
    String o = String("<option value=\"") + e + String("\">") + e + String(" (") + String(rssi) + String(" dBm, ");
    o += secure ? "protetta" : "aperta";
    o += ")</option>";
    return o;
}

static inline String portal_page_html(const String& msg, const String& options) {
    String h = F("<!DOCTYPE html><html><head><meta charset=utf-8>"
                 "<meta name=viewport content='width=device-width,initial-scale=1'><title>DH Gateway</title>"
                 "<style>body{font-family:sans-serif;margin:16px;background:#111;color:#eee}"
                 "input,select,button{width:100%;font-size:18px;padding:10px;margin:6px 0;box-sizing:border-box}"
                 "button{background:#2d7dd2;color:#fff;border:0;border-radius:6px}"
                 ".m{background:#333;padding:10px;border-radius:6px}a{color:#7ab8ff}</style></head>"
                 "<body><h2>Display Hub Gateway</h2>");
    if (msg.length()) {
        h += "<p class=m>";
        h += portal_html_escape(msg);
        h += "</p>";
    }
    h += F("<form method=post action=/portal/save><label>Rete WiFi</label><select name=s>");
    if (options.length()) h += options;
    else h += F("<option value=''>(nessuna rete trovata)</option>");
    h += F("</select><label>Oppure nome rete (nascosta)</label><input name=manual maxlength=32 autocomplete=off>"
           "<label>Password</label><input name=p type=password maxlength=63>"
           "<button>Connetti</button></form><p><a href=/portal/scan>Cerca di nuovo le reti</a></p></body></html>");
    return h;
}

// SSID 1-32 caratteri; password vuota (rete aperta) oppure 8-63 (WPA/WPA2).
static inline bool portal_creds_valid(const String& ssid, const String& pass) {
    if (ssid.length() < 1 || ssid.length() > 32) return false;
    if (pass.length() == 0) return true;
    return pass.length() >= 8 && pass.length() <= 63;
}
