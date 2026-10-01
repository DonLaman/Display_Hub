#include <cstdio>
#include <cstring>
#include "portal_util.h"
static int ok = 0, fail = 0;
#define CHECK(c, m) do { if (c) { ok++; printf("OK   %s\n", m); } else { fail++; printf("FAIL %s\n", m); } } while (0)
static bool has(const String& s, const char* needle) { return s.s.find(needle) != std::string::npos; }
int main() {
    String e = portal_html_escape("<script>alert(1)</script>");
    CHECK(!has(e, "<") && !has(e, ">") && has(e, "&lt;script&gt;"), "escape: < e > neutralizzati");
    e = portal_html_escape("a\"b'c&d");
    CHECK(has(e, "&quot;") && has(e, "&#39;") && has(e, "&amp;") && !has(e, "\"") && !has(e, "'"), "escape: virgolette e &");
    CHECK(strcmp(portal_html_escape("Casa Mia 5G").c_str(), "Casa Mia 5G") == 0, "escape: testo normale invariato");

    // SSID ostile: non deve poter uscire dall'attributo value=""
    String o = portal_option_html("\"><img src=x onerror=alert(1)>", -60, true);
    CHECK(!has(o, "\"><img") && !has(o, "<img"), "opzione: SSID ostile non esce dall'attributo");
    CHECK(has(o, "-60 dBm") && has(o, "protetta"), "opzione: segnale e protezione");
    CHECK(has(portal_option_html("Aperta", -80, false), "aperta"), "opzione: rete aperta");

    // pagina: il messaggio (contiene l'SSID) e' escapato, l'elenco e' inserito cosi' com'e'
    String page = portal_page_html("Provo a collegarmi a '<b>x</b>'", portal_option_html("Rete1", -50, true));
    CHECK(!has(page, "<b>x</b>") && has(page, "&lt;b&gt;"), "pagina: messaggio escapato");
    CHECK(has(page, "<option value=\"Rete1\">") && has(page, "action=/portal/save") && has(page, "name=manual"), "pagina: elenco reti e form");
    CHECK(has(portal_page_html("", ""), "nessuna rete trovata"), "pagina: senza reti lo dice");
    CHECK(!has(portal_page_html("", ""), "class=m"), "pagina: senza messaggio niente riquadro");

    // validazione credenziali
    CHECK(!portal_creds_valid("", "password1"), "creds: SSID vuoto rifiutato");
    CHECK(!portal_creds_valid(String(std::string(33, 'a').c_str()), ""), "creds: SSID di 33 caratteri rifiutato");
    CHECK(portal_creds_valid(String(std::string(32, 'a').c_str()), ""), "creds: SSID di 32 caratteri ok");
    CHECK(portal_creds_valid("Casa", ""), "creds: rete aperta (password vuota) ok");
    CHECK(!portal_creds_valid("Casa", "1234567"), "creds: password di 7 caratteri rifiutata");
    CHECK(portal_creds_valid("Casa", "12345678"), "creds: password di 8 caratteri ok");
    CHECK(portal_creds_valid("Casa", String(std::string(63, 'p').c_str())), "creds: password di 63 caratteri ok");
    CHECK(!portal_creds_valid("Casa", String(std::string(64, 'p').c_str())), "creds: password di 64 caratteri rifiutata");
    printf("\n%d ok, %d falliti\n", ok, fail);
    return fail ? 1 : 0;
}
