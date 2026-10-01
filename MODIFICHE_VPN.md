# VPN Tailscale, impostazioni Rete e modalità di build — riepilogo modifiche

## Nuove librerie (firmware/lib/)
- `microlink/` — MicroLink v2 (client Tailscale) portato su arduino-esp32 2.0.17.
  Note di port e modifiche all'upstream in `microlink/PORTING.md`.
- `wireguard_lwip/` — WireGuard su lwIP (crypto portabile "refc").
- `dh_vpn/` — `DhVpn`: VPN accesa/spenta, auth key, hostname (NVS "dhvpn"),
  avvio/arresto in background, riaggancio al cambio rete.
  `DhVpnSettings.h`: ponte verso la pagina impostazioni.
- `dh_vpn/src/DhNetClient.*` — WiFiClient che instrada da solo: diretto sul
  WiFi o dentro la VPN (anche verso 192.168.x.x dietro un subnet router), con
  risveglio del tunnel. `DhVpn::pathFor()` decide la strada, `DhVpn::routes()`
  elenca le subnet raggiungibili.
- `dh_settings/` — pagina Impostazioni condivisa (Rete: WiFi/BT/VPN con
  interruttori e dettagli, Informazioni, input testo con tastiera).

## Loader con schermo (firmware/loader-full/) — v3.0.0
- Home "Rete" con tab WiFi / Bluetooth (scansioni) e barra di stato.
- Impostazioni > Rete (WiFi, BT, VPN) e Informazioni, da dh_settings.
- La connessione WiFi riuscita resta attiva (serve alla VPN).
- Nuovi comandi seriali: hello, status, vpn_config, vpn_forget (PROTOCOL.md).

## Firmware principale (firmware/src/main.cpp)
- Tre modalità di build: USB+BT, WiFi+BT, Completo (WiFi+VPN+BT).
- Ingranaggio fisso in alto a destra -> Impostazioni (sempre sopra i widget).
- Schermata d'attesa finché il server non invia pagine.
- Completo: server LAN a VPN spenta, server 100.x/MagicDNS a VPN connessa.
- Timeout HTTP brevi + tentativi diradati se il server non risponde.
- Risveglio del tunnel verso il server prima del polling via VPN
  (`DhVpn::prepareTunnel`): necessario quando si passa dal relay DERP.
- WiFi spento dalle impostazioni = nessun tentativo né captive portal;
  captive portal dopo 45 s senza rete (non con le impostazioni aperte).
- Corretto: schermi delle pagine vecchie mai liberati a ogni cambio configurazione.

## Web app
- `core/firmware_builder.py`: `comm_mode` usb / wifi / wifi_vpn, campi VPN,
  validazione, nuove placeholder in `device_config.h.template`.
- Form "Genera firmware": selettore delle tre modalità, campi VPN.
- Pannello "Test connessione": sezione VPN per il loader con schermo (invio
  chiave/nome, stato WiFi/BT/VPN, dispositivi della tailnet con "Usa come
  server VPN", copia nel form di build, dimentica identità).
- Corretto: SSID delle reti trovate inseriti come HTML nella pagina.

## Subnet route (seconda fase)
- MicroLink legge le subnet pubblicate dai dispositivi (`AllowedIPs`) e le
  aggiunge agli allowed IP WireGuard (wireguard_lwip: 4 per peer,
  `wireguardif_set_allowed_ips`). Dettagli in `lib/microlink/PORTING.md`.
- `DhVpn::pathFor()` + `DhNetClient`: 192.168.x.x raggiunto dentro il tunnel
  (bind all'IP 100.x, routing per sorgente di lwIP) quando non è nella rete
  del WiFi attuale e un subnet router la pubblica.
- `DhVpn::prepareTunnel()`: risveglio del dispositivo prima di connettersi
  (necessario via relay DERP; corregge anche il polling via 100.x).
- Firmware: a VPN connessa usa `SERVER_HOST` anche fuori casa se è in una subnet
  pubblicata; `SERVER_HOST_VPN` ora facoltativo. "Strada" in Informazioni.
- Display (dettaglio VPN), loader (`status.vpn.routes`) e web UI mostrano le
  reti raggiungibili; il form segnala se il server è coperto da una subnet.

## Configurazione su microSD, NimBLE, più reti WiFi (terza fase)
- `lib/dh_config`: configurazione in `displayhub.txt` (formato INI, commenti
  conservati) sulla microSD inseribile/rimovibile a display acceso, solo RAM
  senza scheda; nelle altre build lo stesso file nella NVS. Unione della
  sessione con un file trovato dopo, scelta per microSD di altri display,
  salvataggio sicuro con .bak, recupero da scritture interrotte.
  Test sul PC: `firmware/test_host/dh_config/run.sh` (76 controlli).
- Correzione BSP: SPI della microSD non più variabile locale di `sd_init()`.
- VPN: identità Tailscale nel file (microSD) o effimera senza scheda, auth key
  normale ed effimera; MicroLink con archivio dell'identità sostituibile.
- Bluetooth: NimBLE-Arduino 1.4.2 vendorizzato al posto di Bluedroid (circa
  -370 KB di flash); accoppiamenti salvati nella configurazione; "Aggiungi
  dispositivo" anche nel firmware principale. `tools/check_no_bluedroid.py`
  fa fallire la build se Bluedroid rientra.
- `lib/dh_wifi`: più reti salvate, scelta della più forte tra quelle note,
  passaggio automatico (casa -> hotspot), elenco riordinabile.
  Test sul PC: `firmware/test_host/dh_wifi/run.sh`.
- Impostazioni > Archivio, finestra all'inserimento della microSD, messaggi a
  comparsa, icona microSD.
- Comandi seriali `config_*`, `storage_*`, `file_*` (loader e firmware).
- Sincronizzazione della configurazione col server nel polling, con unione a
  tre vie (`app/core/device_config.py`, `app/core/ini_doc.py`).
  Test: `python3 tests/test_device_config.py`, `python3 tests/test_sync_protocol.py`.
- Web UI: opzione di build "configurazione su microSD" (firmware Completo e
  loader), auth key effimera, pannello "Configurazione display e microSD"
  (copia sul server, display via USB, file nuovo; editor a campi e testo;
  file della microSD).

## Correzioni dopo le prime prove sul pannello
- `microlink/library.json`: dipendenza da `wireguard_lwip` dichiarata (la LDF
  "chain" non la scopriva). Nuovo `firmware/tools/check_ldf.py` che imita la LDF.
- Informazioni: niente più `heap_caps_get_largest_free_block()` (percorreva
  l'heap a interrupt disabilitati ogni secondo: Interrupt WDT); ora "minimo
  dall'avvio".
- Bluetooth acceso a caldo con WiFi+VPN: "BLE_INIT: Malloc failed" + assert
  in ROM. Ora mbedTLS alloca in PSRAM (avvio VPN) e il Bluetooth si accende
  solo con almeno 80 KB di RAM interna libera, altrimenti parte al prossimo
  avvio (prima di WiFi e VPN), con proposta di riavvio. Log `[BT] Avvio/Attivo`
  con la memoria, per tarare la soglia.
- `dh_wifi`: la ripresa dopo una scansione manuale non azzera più i giri
  falliti; "Nessuna rete WiFi salvata" scritto una volta sola.
- Decodifica dei crash nella web UI (ELF conservato per ogni build).
- Leggibilità: tema scuro di LVGL anche nel firmware principale (con quello
  chiaro schede, righe dei widget, liste e tastiera avevano testo grigio
  scuro); testo bianco esplicito su schede e contenitori; grigio secondario
  più chiaro.
- Font `lib/dh_fonts`: Montserrat 14/20/48 con lettere accentate, °, — ’ “ ” … €
  (quelli di LVGL hanno solo ASCII); dh_font_14 è il font predefinito.
- `lv_conf.h` non veniva MAI letto da LVGL nelle build PlatformIO: il percorso
  relativo "-I include" non vale per la compilazione della libreria e
  l'#include fallisce in silenzio (bug GCC 80753; restava solo l'avviso
  "Possible failure to include lv_conf.h"). Ora `tools/lv_conf_path.py` (pre:)
  passa il percorso assoluto a tutte le compilazioni, il font con le accentate
  è impostato anche esplicitamente (tema e schermate), e a fine build il log
  dice se lv_conf.h è stato applicato.
- RAM interna: memoria di LVGL in PSRAM (LV_MEM_CUSTOM, include/dh_lv_mem.h:
  RAM interna statica 121 -> 57 KB), pool dell'host NimBLE in PSRAM
  (MEM_ALLOC_MODE_EXTERNAL), malloc() generiche oltre 512 byte in PSRAM
  (heap_caps_malloc_extmem_enable, dopo l'avvio del display). Log `[MEM]`
  dopo l'avvio e ogni 5 minuti.
- Server, widget: dopo un errore i tentativi si diradano (fino a 15 min),
  l'ultimo valore buono resta visibile per 30 min segnato "dati di N min fa";
  errori identici ripetuti -> una riga all'ora nel log; CoinGecko ogni 60 s e
  pausa su "429"; WAHA spiega perché il nome non si risolve (rete docker).
- Pagine: `page_id` per pagina (stesso widget più volte con parametri diversi),
  web UI con ✎ modifica, ▲ ▼, riepilogo parametri; nuovo widget "Prezzi crypto
  (elenco)" (una richiesta CoinGecko per tutte le monete, variazione colorata);
  righe della griglia a due colonne con colore; orologio sull'ora esatta del
  poll (`server_time`) invece dei dati in cache.
- Swipe e doppio tap in tutta la pagina: contenitori e righe dei widget non
  più "cliccabili" (ricevevano loro i tocchi, non lo schermo); il testo lungo
  resta scorrevole e inoltra gli eventi (EVENT_BUBBLE). Simulazione sul PC:
  `firmware/test_host/lvgl_touch/run.sh`.
- Web UI divisa in sezioni (widget e display in prima pagina, firmware, USB e
  loader, configurazione display); archivio firmware in tabella con le opzioni
  di ogni versione, eliminazione (anche multipla) di firmware con ELF/log/dati,
  pulizia dei log delle build fallite; display da dimenticare (anche con la
  configurazione salvata). Test: `python3 tests/test_archive.py`.
- Sezioni "Versioni" (registro versioni/revisioni in Markdown, la voce attuale
  compila versione e revisione nel form di compilazione) e "Documentazione"
  (i file .md del progetto come pagine HTML, con indice).
  Test: `python3 tests/test_docs_changelog.py`.
- Corretta la lettura delle opzioni dal nome dei firmware vecchi con revisione
  contenente "_" (es. rev_7: prima dispositivo "7_Dash_Final").
- Accesso alla web UI con utente e password (variabili del container, .env),
  sessione in cookie firmato con "Ricordami", blocco dopo 5 tentativi, API dei
  display solo dirette (non dal reverse proxy). Test: `python3 tests/test_auth.py`.
- Gateway Bluetooth, passo 1 (`firmware/gateway`, ESP32 classico): A2DP verso
  la cassa con ESP32-A2DP 1.8.10, volume assoluto AVRCP + scalatura, tono di
  prova, API HTTP e mDNS (`dh-gateway.local`), configurazione via USB con il
  protocollo del loader. Builder: target "gateway" (merge_bin `--chip esp32`,
  bootloader a 0x1000), manifest `chipFamily: ESP32`. Test: `python3 tests/test_gateway.py`.
- Widget "numero grande": un valore testuale si mostra com'è (l'ora "14:05"
  diventava "0.00"), i numeri senza decimali inutili.
- Orologio: il server manda l'ora esatta e il fuso (DH_TIMEZONE, default
  Europe/Rome, pacchetto tzdata); il display la fa avanzare da solo ogni
  secondo, con giorno e mese in italiano.
- `platformio.ini` del firmware principale: aggiunto `-I include`. Senza, le
  librerie (LVGL, dh_settings, dh_fonts, BSP) compilavano senza `lv_conf.h`
  (il vecchio "Possible failure to include lv_conf.h") e il testo normale
  usava il Montserrat 14 di LVGL senza accentate. `tools/check_ldf.py` ora
  segue gli header a catena e distingue include/ del progetto dalle librerie.

## Verifiche fatte
- Compilazione + link con toolchain reale e core 2.0.17: loader, firmware
  nelle tre modalità (0 errori, 0 warning); MicroLink assente dalle build
  senza VPN (controllo sui simboli).
- AEAD Noise verificata bit per bit contro la libreria `cryptography`.
- Builder: validazione e generazione di device_config.h nelle tre modalità.
- Web UI: app.js reale sulla pagina resa da Flask (jsdom), 24 controlli.
- Parsing AllowedIPs e scelta della subnet: codice reale eseguito su PC, 9 controlli.

## Da verificare sul pannello
Vedi la sezione "Da provare" nei messaggi di ogni step; in breve: toggle BT
spento/acceso, VPN con WiFi+BT attivi (RAM interna in Informazioni), passaggio
casa -> hotspot con VPN accesa, aspetto delle impostazioni nel firmware principale.

## Aperti
Vedi TODO.md (più reti WiFi salvate; limiti noti delle subnet).
- Telecomando sul display, rifinitura: icone con canale alpha (sfondo trasparente, bordi morbidi,
  rigenerabili con `firmware/lib/dh_icons/gen_icons.py`); icona e testo impilati senza
  sovrapposizioni; griglia centrata con margini uguali e fascia libera di ~14 px sopra e sotto per lo
  swipe (il contenitore non e' piu' cliccabile e non ha il padding del tema); colori: blu solo alle
  frecce, accensione rossa, tonda e al centro, "Canale" verde acqua. Geometria e contenuto in
  `firmware/src/button_grid_ui.h`, provati con LVGL sul PC: `firmware/test_host/button_grid/run.sh`
  (31 controlli) e anteprima con `render.sh`; coerenza widget/firmware: `tests/test_tv_remote_widget.py`.
- Widget "Player Bluetooth" (`bt_player`): come il telecomando TV, con la scheda "in riproduzione" in testa,
  controlli e volume (vero, memorizzato nell'INI del gateway e inviato subito al gateway), e una seconda pagina
  "Fonti" (MP3 / YouTube / Web radio) con segnaposto; play/pausa e simili rispondono `501` finche' non c'e' la
  riproduzione. Il display ridisegna i pulsanti solo quando il contenuto cambia (prima a ogni poll) e capisce le
  azioni composte `a;b;view:x`. Nuove rotte senza login `/api/esp/audio-action` e `/api/esp/bt-gateway-config`:
  il gateway scaricava la config da una rotta dietro il login e riceveva 401 (corretto anche l'URL nel firmware
  del gateway: va riflashato).
- Impostazioni > **Gateway Bluetooth** sul display: elenco dei gateway (online/offline), dettaglio con nome, id, MAC, IP,
  WiFi, firmware, volume e cassa, scansione dei dispositivi con associazione, elenco dei conosciuti con
  connetti/disconnetti e dissocia. Passa dal server (funziona anche in VPN): nuove rotte senza login
  `/api/esp/bt-gateways*`; l'elenco dei conosciuti e' nell'INI del gateway (`[bt] known`). Il **nome del gateway** si
  sceglie alla compilazione (web UI > Firmware > Gateway Bluetooth) e finisce in `gateway_build.h`; il file `.bin`
  ne porta l'id. Serve riflashare il display; il gateway solo se si vuole il nome dalla compilazione.

