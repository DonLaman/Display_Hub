// Test sul PC della scelta delle reti (DhWifiPlan.h). Esecuzione: ./run.sh
#include <cstdio>
#include "DhWifiPlan.h"
using namespace dhwifi;
static int ok = 0, ko = 0;
static void check(bool c, const char* m) { c ? ok++ : ko++; printf("%s %s\n", c ? "OK  " : "FAIL", m); }
static std::string str(const std::vector<std::string>& k, const std::vector<int>& o) {
    std::string r;
    for (int i : o) r += k[i] + ",";
    return r;
}
int main() {
    std::vector<std::string> known = {"Casa", "Hotspot", "Ufficio", "Nascosta"};
    check(str(known, planOrder(known, {{"Hotspot", -50}, {"Casa", -80}, {"Vicino", -30}}, false)) == "Hotspot,Casa,",
          "visibili dalla più forte; reti sconosciute ignorate; nascoste escluse");
    check(str(known, planOrder(known, {{"Hotspot", -60}, {"Casa", -62}}, false)) == "Casa,Hotspot,",
          "entro 3 dB vince la priorità dell'elenco");
    check(str(known, planOrder(known, {{"Hotspot", -60}, {"Casa", -64}}, false)) == "Hotspot,Casa,",
          "oltre 3 dB vince il segnale");
    check(str(known, planOrder(known, {{"Casa", -85}, {"Casa", -55}, {"Hotspot", -60}}, false)) == "Casa,Hotspot,",
          "più access point con lo stesso nome: conta il migliore");
    check(str(known, planOrder(known, {{"Ufficio", -70}}, true)) == "Ufficio,Casa,Hotspot,Nascosta,",
          "con include_hidden: poi le non visibili nell'ordine dell'elenco");
    check(str(known, planOrder(known, {{"casa", -40}}, false)).empty(), "SSID con maiuscole diverse: rete diversa");
    check(planOrder({}, {{"Casa", -40}}, true).empty(), "nessuna rete nota");
    std::vector<std::string> dup = {"Casa", "Casa", "Hotspot"};
    check(str(dup, planOrder(dup, {{"Casa", -50}, {"Hotspot", -40}}, true)) == "Hotspot,Casa,", "rete duplicata nell'elenco provata una volta");
    printf("\n%d ok, %d falliti\n", ok, ko);
    return ko ? 1 : 0;
}
