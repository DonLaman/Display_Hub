/*
 * Scelta dell'ordine in cui provare le reti note (C++ puro, testato sul PC:
 * firmware/test_host/dh_wifi).
 *
 * Regole:
 *  1. prima le reti note VISIBILI nella scansione, dalla più forte (RSSI) alla
 *     più debole; a pari segnale (entro 3 dB) vince la priorità dell'elenco;
 *  2. una rete visibile più volte (più access point / ripetitori) conta col
 *     suo segnale migliore;
 *  3. poi le reti note NON visibili (possono essere nascoste), nell'ordine
 *     dell'elenco — solo se include_hidden (costano un timeout ciascuna);
 *  4. confronto degli SSID esatto (maiuscole comprese, come fa il WiFi).
 */
#pragma once
#include <string>
#include <vector>

namespace dhwifi {

struct ScanItem {
    std::string ssid;
    int rssi;
};

// Ritorna indici in "known" nell'ordine in cui provarle.
inline std::vector<int> planOrder(const std::vector<std::string>& known, const std::vector<ScanItem>& scan,
                                  bool include_hidden) {
    struct Cand {
        int idx;
        int rssi;
    };
    std::vector<Cand> visible;
    std::vector<int> hidden;
    for (int i = 0; i < (int)known.size(); i++) {
        if (known[i].empty()) continue;
        bool seen_before = false;
        for (int j = 0; j < i; j++) seen_before = seen_before || known[j] == known[i];
        if (seen_before) continue;
        int best = -1000;
        for (const auto& s : scan)
            if (s.ssid == known[i] && s.rssi > best) best = s.rssi;
        if (best > -1000) visible.push_back({i, best});
        else hidden.push_back(i);
    }
    // Ordinamento stabile per segnale, con 3 dB di tolleranza a favore della priorità.
    for (size_t a = 1; a < visible.size(); a++) {
        Cand c = visible[a];
        size_t b = a;
        while (b > 0) {
            const Cand& p = visible[b - 1];
            bool stronger = c.rssi > p.rssi + 3;  // più di 3 dB meglio: passa davanti
            if (!stronger) break;
            visible[b] = visible[b - 1];
            b--;
        }
        visible[b] = c;
    }
    std::vector<int> out;
    for (const auto& c : visible) out.push_back(c.idx);
    if (include_hidden)
        for (int i : hidden) out.push_back(i);
    return out;
}

}  // namespace dhwifi
