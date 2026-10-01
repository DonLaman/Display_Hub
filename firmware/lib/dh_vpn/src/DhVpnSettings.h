/*
 * Ponte tra DhVpn (VPN) e DhSettings (pagina Impostazioni).
 *
 * Header-only e incluso SOLO dalle build con VPN: DhSettings non dipende da
 * MicroLink, riceve questo backend come puntatore. Uso:
 *
 *   #include "DhVpnSettings.h"
 *   static DhVpnSettingsBackend vpn_backend;
 *   cfg.vpn = &vpn_backend;
 */
#pragma once
#include "DhSettings.h"
#include "DhVpn.h"
#include "WiFi.h"

class DhVpnSettingsBackend : public DhSettings::VpnBackend {
public:
    bool enabled() override { return DhVpn::isEnabled(); }
    void setEnabled(bool on) override { DhVpn::setEnabled(on); }
    bool connected() override { return DhVpn::isConnected(); }
    String hostname() override { return DhVpn::hostname(); }
    bool setAuthKey(const String& key) override { return DhVpn::setAuthKey(key); }
    void setHostname(const String& name) override { DhVpn::setHostname(name); }
    void forgetIdentity() override { DhVpn::forgetIdentity(); }
    void setVerbose(bool on) override { DhVpn::setVerbose(on); }

    String summary() override {
        String s = DhVpn::stateText();
        String ip = DhVpn::vpnIp();
        if (ip.length()) s += " - " + ip;
        return s;
    }

    String detailText() override {
        String t = "Stato: " + String(DhVpn::stateText());
        String err = DhVpn::lastError();
        if (err.length()) t += " (" + err + ")";
        t += "\nNome sulla tailnet: " + DhVpn::hostname() + "\n";
        String vip = DhVpn::vpnIp();
        t += "IP Tailscale: " + (vip.length() ? vip : String("-")) + "\n";
        String nip = DhVpn::networkIp();
        t += "Rete sottostante: " + (nip.length() ? WiFi.SSID() + " (" + nip + ")" : String("nessuna")) + "\n";
        t += "Auth key: " + DhVpn::authKeyMasked() + "\n";
        // Subnet pubblicate dai subnet router: indirizzi come 192.168.1.40
        // raggiungibili anche fuori casa (DhNetClient passa dal tunnel).
        DhVpn::Route routes[8];
        int nr = DhVpn::routes(routes, 8);
        t += "Reti raggiungibili: " + String(nr ? "" : "nessuna subnet pubblicata");
        for (int i = 0; i < nr; i++) {
            t += "\n  " + routes[i].network + "  tramite " + routes[i].via_name;
            if (!routes[i].via_online) t += " (offline)";
        }
        t += "\n";
        int n = DhVpn::peerCount();
        t += "Dispositivi sulla tailnet: " + String(n);
        DhVpn::Peer p;
        for (int i = 0; i < n; i++) {
            if (!DhVpn::peer(i, p)) continue;
            t += p.online ? "\n  #3ddc84 " LV_SYMBOL_OK "# " : "\n  #9a9aa0 " LV_SYMBOL_CLOSE "# ";
            t += p.hostname + "  " + p.ip;
            if (p.online) t += p.direct ? "  diretto" : "  via relay";
        }
        return t;
    }
};
