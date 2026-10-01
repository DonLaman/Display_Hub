# MicroLink su arduino-esp32 2.0.17 (PlatformIO) — note di port

Upstream: https://github.com/CamM2325/microlink @ 216da3300f0493b0860247d43f7af5ce29df63a5 (MIT)
Upstream richiede ESP-IDF >= 5.0; il firmware Display Hub usa arduino-esp32 2.0.17 (IDF 4.4.7),
pinnato per la BSP del pannello 4848S040. Questo port evita di cambiare core.

## Moduli inclusi
microlink.c, ml_coord.c, ml_derp.c, ml_net_io.c, ml_wg_mgr.c, ml_stun.c, ml_noise.c, ml_h2.c,
ml_udp.c, ml_tcp.c, ml_peer_nvs.c, ml_zerocopy.c, nacl_box.c + lib/wireguard_lwip (solo crypto refc).

## Moduli esclusi
- ml_cellular.c, ml_at_socket.c, ml_net_switch.c: modem 4G, non presente. Il WiFi lo gestisce
  il firmware (WiFi.begin), MicroLink lo usa e basta: router o hotspot sono indistinguibili.
- ml_config_httpd.c: web UI di MicroLink, usa driver/temperature_sensor.h (solo IDF 5).
- src/x25519.c: identico a wireguard_lwip/src/crypto/refc/x25519.c (tenuta una sola copia).

## Modifiche al codice upstream (cercare "[Display Hub / Arduino port]")
1. ml_noise.c — il core 2.0.17 ha mbedTLS compilato SENZA ChaCha20-Poly1305
   (CONFIG_MBEDTLS_CHACHAPOLY_C non impostato, simboli assenti da libmbedcrypto.a).
   Ora usa chacha20poly1305_encrypt/decrypt di wireguard_lwip (refc) con il contatore
   invertito (__builtin_bswap64): produce esattamente il nonce Tailscale
   (4 zero + contatore 64 bit big-endian). Verificato bit per bit contro la
   ChaCha20Poly1305 della libreria Python `cryptography`, incluso il rifiuto di un
   messaggio alterato.
   Include di blake2s cambiato in "crypto/refc/blake2s.h".
2. ml_coord.c — ";" dopo il label check_removed: (GCC 8 / gnu99 non accetta una
   dichiarazione subito dopo un label).
3. include/ml_arduino_config.h — sostituisce Kconfig (CONFIG_ML_*), incluso forzatamente
   via library.json. Buffer H2/JSON a 256 KB (PSRAM), 16 peer. Sovrascrivibili con -D.

4. Subnet route (subnet router Tailscale):
   - ml_coord.c legge `AllowedIPs` dei peer e tiene fino a 3 subnet IPv4 per peer
     (scarta IPv6, exit node 0.0.0.0/0, 100.64.0.0/10 e il /32 del peer), normalizzate.
   - ml_wg_mgr.c le copia nel peer e, dopo wireguardif_add_peer(), imposta gli
     allowed IP WireGuard = /32 + subnet (wg_apply_allowed_ips): così un pacchetto
     verso 192.168.1.40 sceglie il subnet router e le risposte con sorgente
     192.168.1.40 vengono accettate. Aggiornate a ogni nuova versione del peer
     (una route revocata sparisce).
   - microlink.c: microlink_get_peer_routes(), microlink_route_lookup() (prefisso
     più lungo tra i peer attivi).
   - wireguard_lwip: WIREGUARD_MAX_SRC_IPS 2 -> 4 e wireguardif_set_allowed_ips().
   Le subnet NON cambiano l'instradamento di lwIP: decide l'applicazione, legando
   il socket all'IP 100.x (routing per sorgente di lwIP) solo quando serve.

5. Archivio dell'identità sostituibile (build con configurazione su microSD):
   microlink_set_storage() con load_keys/save_keys/forget e disable_peer_cache.
   Senza chiamarla (tutti zero) il comportamento resta quello upstream (NVS).
   lib/dh_vpn la usa per tenere l'identità in [vpn] identity del file
   displayhub.txt, oppure solo in RAM (dispositivo effimero) senza microSD.

## Dipendenza da wireguard_lwip (library.json)
I sorgenti di MicroLink (src/) includono gli header di wireguard_lwip, ma chi
usa MicroLink include solo include/microlink.h. In modalità LDF "chain"
PlatformIO analizza di una libreria SOLO gli header inclusi da altri e il
.c/.cpp con lo stesso nome nella stessa cartella: i .c in src/ non vengono
letti e wireguard_lwip non verrebbe scoperta (errore:
"crypto/refc/chacha20poly1305.h: No such file"). Per questo library.json
dichiara "dependencies": [{"name": "wireguard_lwip"}]: senza "owner" è una
dipendenza interna, risolta per nome tra le librerie locali e mai scaricata
dal registro (pio pkg install la salta).
Controllo senza PlatformIO: firmware/tools/check_ldf.py.

## Verifica
Compilazione e link completi con la toolchain reale (xtensa-esp32s3-elf esp-2021r2-patch5)
e il core ufficiale 2.0.17 (qio_opi, flag di platform.txt): 0 errori, 0 simboli mancanti.
Costo misurato su uno sketch WiFi+HTTPClient: +~120 KB flash, +~2,5 KB RAM statica;
a runtime ~42 KB di stack in RAM interna (4 task) + buffer in PSRAM durante la sincronizzazione.

## Subnet route
Supportate dal punto 4 delle modifiche: MicroLink raccoglie le subnet e le mette
negli allowed IP WireGuard; il routing è deciso dall'applicazione
(lib/dh_vpn: DhVpn::pathFor + DhNetClient, bind all'IP 100.x).
