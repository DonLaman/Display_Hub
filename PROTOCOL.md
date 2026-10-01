# Protocollo Server ↔ Display

## Runtime: WiFi (HTTP), nessuna seriale coinvolta

Il display, una volta flashato e connesso alla rete WiFi configurata in
`firmware/device_config.h`, parla col server **solo** via HTTP, in rete locale.

### `POST /api/esp/hello`
Chiamato una volta al boot del display, dopo la connessione WiFi.
```json
// richiesta
{"device_id": "esp32s3-display-01", "fw_version": "0.2.0"}
```
Il server registra il device (IP preso dalla richiesta stessa) e lo rende visibile
nella web UI, sezione "Dispositivi WiFi connessi".

### `GET /api/esp/poll?device_id=...`
Chiamato periodicamente dal display (`POLL_INTERVAL_MS`, default 4s). Ritorna
**tutte** le pagine attive con i dati già pronti (calcolati in background dal
server, mai durante questa richiesta):
```json
{
  "config_version": 3,
  "pages": [
    {"widget_id": "clock", "layout_type": "big_number", "payload": {"title": "Martedì 17 Settembre", "value": "14:32", "unit": ""}},
    {"widget_id": "crypto_price", "layout_type": "big_number", "payload": {"title": "BITCOIN", "value": 61234.5, "unit": "USD"}}
  ]
}
```
Il display confronta `config_version` con l'ultima vista:
- **diversa** → ricostruisce tutti gli schermi da zero (l'elenco/ordine pagine è cambiato dalla web UI)
- **uguale** → aggiorna solo i valori sugli schermi già costruiti

Questo endpoint non richiede autenticazione: si presume che il display viva
sulla rete locale del ryzen_server, non esposta oltre la VPN (vedi nota
sicurezza nel README).

## Runtime alternativo: USB (nessun WiFi sul display)

Da usare quando il display non ha una rotta di rete verso il server (es.
collegato all'hotspot di un telefono che, a sua volta, raggiunge il server
solo tramite una VPN come Tailscale — l'hotspot non condivide di norma il
tunnel VPN del telefono con i dispositivi collegati, e questo non è
aggirabile senza root sul telefono).

Con `COMM_MODE_USB=1` (impostato dalla web UI scegliendo "USB" come modalità
di comunicazione in fase di generazione firmware) il display:
- non tocca il WiFi in alcun modo: niente credenziali, niente captive portal
- al boot invia una riga `{"type": "hello", "device_id": "...", "fw_version": "..."}` sulla USB
- ogni `POLL_INTERVAL_MS` invia `{"cmd": "poll", "device_id": "..."}` e attende
  **una singola riga di risposta** con la stessa forma esatta del body JSON
  che l'endpoint HTTP `/api/esp/poll` restituirebbe — timeout 3s, se scade
  salta semplicemente questo giro e riprova al successivo

Chi risponde a queste righe è un "ponte", non il server direttamente: nella
web UI, sezione "Ponte USB↔server", si apre una connessione Web Serial verso
il display (porta diversa da quella eventualmente usata per il test WiFi) e:
- su `hello` ricevuto → `POST /api/esp/hello` verso il server (stesso body)
- su `cmd: poll` ricevuto → `GET /api/esp/poll?device_id=...` verso il server,
  la risposta va scritta indietro sulla seriale così com'è, più `\n`

Il vantaggio: la pagina che ospita il ponte gira nel browser di un PC/telefono
che *ha* già una rotta verso il server (es. via Tailscale), mentre il display
stesso resta su qualunque rete locale gli capiti di avere sottomano — non gli
serve mai una rotta diretta al server, solo il cavo USB.

## WiFi: tre modi per configurarlo, in ordine di preferenza

### Modo consigliato: credenziali incorporate nel firmware (dal test via USB)

Il modo più affidabile: usi il loader solo per **scoprire e verificare** la
password giusta (le reti visibili dal display dipendono da dove si trova
fisicamente, non dal server né dal tuo PC), poi le porti nel firmware
definitivo come parametri di build — esattamente come già fai per l'indirizzo
del server. Elimina la dipendenza dalla persistenza NVS **tra firmware
diversi** (loader → definitivo), che in pratica si è rivelata inaffidabile:
schemi di partizione anche leggermente diversi tra i due progetti PlatformIO
possono far scattare un reinizializzazione della NVS al boot, cancellando
silenziosamente le credenziali appena salvate.

1. Genera e flasha il firmware "loader" (sezione "Test connessione WiFi" della
   web UI) — build minima, senza display/LVGL/BSP, solo per questo scopo
2. Con il display ancora collegato in USB, premi "Connetti al display (USB)"
   nella web UI: apre una connessione Web Serial diretta (non tramite ESP Web
   Tools, che serve solo per il flashing) sulla stessa porta
3. Il loader parla un protocollo JSON-per-riga molto semplice:

   **Host → device**
   ```json
   {"cmd": "scan"}
   {"cmd": "connect", "ssid": "CasaWiFi", "password": "..."}
   ```
   **Device → host**
   ```json
   {"type": "loader_hello", "fw": "wifi-setup-loader"}
   {"type": "scan_result", "networks": [{"ssid": "CasaWiFi", "rssi": -52, "secure": true}, ...]}
   {"type": "connect_result", "success": true, "ip": "192.168.1.87"}
   {"type": "connect_result", "success": false, "error": "..."}
   ```
4. Se la connessione riesce, la web UI copia automaticamente SSID e password
   nei campi corrispondenti della sezione "Genera firmware" — non serve
   ricordarli o ridigitarli
5. Generi (e poi flashi) il firmware definitivo: questa volta con le
   credenziali già incorporate in `device_config.h` come `WIFI_SSID`/
   `WIFI_PASSWORD`. Al primo boot il firmware le prova per prime, e se
   funzionano le salva comunque in NVS (così un aggiornamento futuro del
   firmware, anche senza incorporarle di nuovo, si riconnette lo stesso)

Il loader continua comunque a salvare le credenziali in NVS come faceva prima
(non fa danno tenerlo così), ma non è più il meccanismo su cui ci si affida
per farle arrivare al firmware definitivo.

### Alternativa: persistenza NVS tra loader e firmware (meno affidabile)

Se preferisci non incorporare le credenziali nel firmware (es. non vuoi che
compaiano in chiaro nel `.bin`), puoi ancora contare sul fatto che loader e
firmware definitivo condividano la stessa partizione NVS — **a patto che
usino esattamente la stessa tabella di partizioni** (`board_build.partitions`
nei rispettivi `platformio.ini`, tenuti allineati in questo progetto). Se in
futuro modifichi uno dei due schemi senza aggiornare l'altro, rischi di
ricadere nello stesso problema di cancellazione silenziosa.

### Fallback automatico: captive portal sul display stesso

Se nessuna delle due strade sopra è stata percorsa (nessuna credenziale
incorporata, NVS vuota), il firmware definitivo gestisce comunque da solo il
caso senza credenziali valide:

Al primo boot (o ogni volta che la connessione fallisce — rete spenta,
password cambiata, spostato altrove) il firmware:

1. Apre un proprio Access Point, `DisplayHub-Setup-<device_id>`
2. Scansiona le reti WiFi visibili **da dove si trova il display in quel momento**
3. Ti connetti a quella rete con telefono o PC: si apre un captive portal (o vai
   manualmente su `http://192.168.4.1`) con l'elenco delle reti trovate — lo
   schermo del display stesso mostra queste istruzioni, non resta muto
4. Scegli la rete, inserisci la password, il display salva le credenziali in
   NVS e si riconnette in modalità normale

Questo flusso vive interamente nel firmware (`run_captive_portal()` in
`src/main.cpp`), non richiede nulla dal server.

## Generazione firmware: build server-side (PlatformIO)

Prima ancora di flashare, puoi far compilare il firmware direttamente al
server con indirizzo del server, identificativo e (opzionalmente) credenziali
WiFi già incorporati, invece di editare `device_config.h` a mano e compilare
in locale.

### `POST /api/firmware/build`
```json
{"comm_mode": "wifi", "server_host": "192.168.1.40", "device_id": "esp32s3-display-01", "server_port": 12000,
 "wifi_ssid": "CasaWiFi", "wifi_password": "..."}
```
`comm_mode` sceglie una delle tre modalità del firmware principale:

| `comm_mode` | Modalità | Comunicazione | Bluetooth | VPN |
|---|---|---|---|---|
| `"usb"` | USB + BT | ponte USB | sì | no |
| `"wifi"` (default) | WiFi + BT | WiFi diretto | sì | no |
| `"wifi_vpn"` | Completo | WiFi, con VPN Tailscale all'occorrenza | sì | sì |

Campi aggiuntivi per `"wifi_vpn"` (ignorati nelle altre modalità):
```json
{"server_host_vpn": "100.101.102.103", "vpn_auth_key": "tskey-auth-...",
 "vpn_hostname": "displayhub-sala", "vpn_default_enabled": true}
```
- **Scelta del server a VPN connessa**, nell'ordine:
  1. `server_host` se è nella rete del WiFi attuale (a casa: connessione diretta)
     oppure dentro una **subnet pubblicata** da un subnet router della tailnet
     (es. 192.168.1.0/24): in quel caso lo stesso indirizzo LAN viene raggiunto
     dentro il tunnel, anche sotto hotspot;
  2. altrimenti `server_host_vpn`, se impostato;
  3. altrimenti `server_host` come senza VPN.
  A VPN spenta si usa sempre `server_host`. Ogni cambio di strada manda un nuovo `hello`.
- `server_host_vpn` (facoltativo): IP 100.x del server sulla tailnet o suo nome
  MagicDNS; serve solo se il server non sta in una subnet pubblicata.
  Prima di ogni connessione via VPN il firmware sveglia il server sulla tailnet
  (handshake WireGuard via relay DERP e, se noto, diretto + CallMeMaybe) e
  aspetta fino a 20 s che la sessione sia attiva, ricontrollando ogni secondo;
  poi tenta comunque. Senza questo passaggio MicroLink apre la sessione solo se
  trova un percorso diretto, e sotto hotspot il polling potrebbe non partire mai.
- **Subnet route**: il display legge dalla MapResponse le subnet IPv4 che ogni
  dispositivo pubblica (`AllowedIPs`, max 3 per dispositivo; esclusi IPv6, exit
  node 0.0.0.0/0 e 100.64.0.0/10). Una connessione verso una di queste reti (fuori
  dalla rete del WiFi attuale) viene legata all'IP 100.x del display e passa nel
  tunnel verso chi la pubblica (routing per sorgente di lwIP, `DhNetClient`).
  Requisiti lato Tailscale: route pubblicata e approvata, ACL che permettano al
  display di raggiungerla, SNAT delle subnet attivo sul router (default).
  Solo le connessioni aperte con `DhNetClient` (hello, poll) usano le subnet.
- `vpn_auth_key` (opzionale): usata solo se il display non ha già una chiave
  salvata (es. inviata dal loader); si può anche inserire dalle impostazioni.
- `vpn_hostname` (opzionale): nome sulla tailnet, default `device_id`.
- `vpn_default_enabled` (default `true`): stato iniziale dell'interruttore VPN,
  solo se l'utente non l'ha mai cambiato sul display.
- `vpn_auth_key_ephemeral` (opzionale): auth key Tailscale "ephemeral", usata
  quando la configurazione è su microSD e la scheda manca (dispositivo effimero,
  che Tailscale rimuove da solo quando va offline).
- `config_storage_sd` (default `false`): **configurazione su microSD**. Reti,
  Bluetooth, VPN e server stanno nel file `displayhub.txt` della microSD
  (inseribile e rimovibile a display acceso); senza scheda valgono i valori
  della build e la copia sul server, e le modifiche durano fino al riavvio.
  Nessun dato di configurazione nella NVS. Nome del file generato: `wifi_vpn-sd-...`.
  Senza questa opzione lo stesso file di configurazione sta nella NVS.

Nelle build senza VPN il codice MicroLink non viene linkato (zero costo).

In tutte le modalità il firmware ha una **pagina Impostazioni fissa** (icona
ingranaggio in alto a destra, sempre sopra le pagine widget): Rete (WiFi, BT,
VPN con interruttori e dettagli, stessa libreria `lib/dh_settings` del loader)
e Informazioni (dispositivo, modalità, server in uso, ultimo aggiornamento,
memoria). Finché il server non invia pagine è visibile una schermata d'attesa
con lo stato della connessione. Se il WiFi resta disconnesso per 45 s si apre il
captive portal come prima (non mentre le impostazioni sono aperte); dal
dettaglio WiFi, "Cambia rete (portale)" lo apre a richiesta. Spegnendo il WiFi
dalle impostazioni il display non tenta connessioni né apre il portale.

Vedi anche la sezione "Runtime alternativo: USB" più sopra per `"usb"`. Con `"usb"`, `server_host`/`wifi_ssid`/`wifi_password` sono
ignorati dal firmware (non richiesti in validazione): la comunicazione passa
tutta dal ponte USB nella web UI, non da un indirizzo incorporato nel build.
`wifi_ssid`/`wifi_password` restano comunque opzionali anche in modalità wifi:
se omessi o vuoti, il firmware si affida solo a NVS/captive portal come prima
(vedi sezione precedente).
Avvia in background: generazione di `device_config.h` dal template, `pio run`
(PlatformIO), poi `esptool merge_bin` per unire bootloader+partizioni+app in
un unico `.bin` pronto per il flashing da browser (sezione successiva).

### `GET /api/firmware/build/status`
Polling per log incrementale e stato (`running` / `success` / `error`).
Al successo, il file compare automaticamente nell'elenco firmware disponibili
per il flashing.

### `POST /api/firmware/build-loader`
Nessun parametro richiesto. Avvia la build del loader leggero di test WiFi
(progetto separato in `firmware/loader/`, niente display/LVGL/BSP — build più
rapida). Usa lo stesso `GET /api/firmware/build/status` per il log, che
include un campo `"target": "main" | "loader" | "loader_full"` per
distinguere quale delle tre build è in corso.

### `POST /api/firmware/build-loader-full`
Body facoltativo `{"config_sd": true}`: configurazione su microSD, come
`config_storage_sd` del firmware (passata alla build come
`-DLOADER_CONFIG_SD=1` tramite `PLATFORMIO_BUILD_FLAGS`). Senza body, come il
loader leggero. Avvia la build del loader
"pesante" (progetto separato in `firmware/loader-full/`, riusa la BSP e
`lv_conf.h` del firmware definitivo tramite `lib_extra_dirs`/`-I` invece di
duplicarli — vedi i commenti in `firmware/loader-full/platformio.ini`).

Stesso identico protocollo seriale JSON-per-riga del loader leggero (hello/
scan/connect — vedi sotto): il pannello "Test connessione WiFi" della web UI
funziona invariato con entrambi, senza bisogno di sapere quale dei due hai
flashato. In più, questo loader (v3) ha schermo e touch attivi:
- **Home "Rete"**: due tab, *WiFi* (scansione reti con SSID, cifratura, canale,
  RSSI; tap su una rete → password con tastiera a schermo → connessione) e
  *Bluetooth* (scansione BLE, tap su un dispositivo per un tentativo di
  connessione). Ogni tab funziona solo se la relativa funzione è accesa in
  Impostazioni; in alto una barra di stato WiFi / BT / VPN (grigio spento,
  giallo acceso ma non connesso, verde operativo).
- **Impostazioni** (icona ingranaggio) → **Rete**: WiFi, Bluetooth e VPN
  Tailscale, ognuno con un interruttore (stato salvato in NVS, namespace
  `dh_net` per WiFi/BT e `dhvpn` per la VPN). Tap sulla riga → dettaglio:
  - WiFi: rete, IP, gateway, DNS, segnale, canale, MAC; Disconnetti,
    Dimentica rete salvata.
  - Bluetooth: MAC BLE, ultima connessione.
  - VPN: stato, nome sulla tailnet, IP 100.x, rete sottostante, auth key
    (mascherata), elenco dispositivi della tailnet (online, diretto/relay);
    Auth key, Nome, livello log, Dimentica identità.
- **Impostazioni → Informazioni**: versione, chip, RAM interna/PSRAM libere, MAC.

Differenza rispetto al loader v2: se la connessione WiFi riesce **resta attiva**
(serve alla VPN); un nuovo `connect` scollega la rete corrente prima di provare.

Comandi seriali aggiuntivi (solo loader "pesante"; il loader leggero li ignora):

**Host → device**
```json
{"cmd": "hello"}
{"cmd": "status"}
{"cmd": "vpn_config", "auth_key": "tskey-auth-...", "hostname": "displayhub-sala", "enabled": true}
{"cmd": "vpn_forget"}
```
Tutti i campi di `vpn_config` sono opzionali (si può mandare solo `enabled`).
La chiave via USB dalla web UI evita di digitarla sul touchscreen.

**Device → host**
```json
{"type": "loader_hello", "fw": "wifi-setup-loader-full", "version": "3.0.0", "features": ["wifi", "bt", "vpn"]}
{"type": "status", "wifi": {"enabled": true, "connected": true, "ssid": "...", "ip": "...", "rssi": -55},
 "bt": {"enabled": true, "mac": "...", "devices": [{"addr": "aa:bb:cc:dd:ee:ff", "name": "Sensore", "bonded": true}]},
 "vpn": {"enabled": true, "state": "Connessa", "error": "", "hostname": "...", "ip": "100.x.y.z",
         "key": "tskey-auth-k...9Qa",
         "routes": [{"network": "192.168.1.0/24", "via": "ryzen", "via_ip": "100.x.y.z", "online": true}],
         "peers": [{"hostname": "ryzen", "ip": "100.x.y.z", "online": true, "direct": true}]}}
{"type": "vpn_config_result", "ok": true}
{"type": "vpn_config_result", "ok": false, "error": "..."}
{"type": "vpn_forget_result", "ok": true}
```
La VPN (MicroLink, `firmware/lib/microlink` + `lib/dh_vpn`) usa la connessione
WiFi attiva, qualunque essa sia (router o hotspot del telefono). Il traffico
verso `100.64.0.0/10` passa nel tunnel; `status` riporta anche le subnet
pubblicate dai subnet router (`vpn.routes`: `network`, `via`, `via_ip`, `online`).

I comandi ricevuti via seriale (`{"cmd":"scan"}`/`{"cmd":"connect",...}`)
aggiornano gli stessi dati mostrati a schermo e viceversa: scansionare dal
touchscreen invia comunque `scan_result` sulla seriale, così un ponte USB
eventualmente connesso vede comunque tutto.

### `POST /api/firmware/clear-cache`
```json
{"target": "main"}
```
`target` è `"main"`, `"loader"` o `"loader_full"`. Cancella `.pio/libdeps/<env>` (le librerie
già scaricate da PlatformIO) per quel progetto, forzando un ridownload
completo alla build successiva. Le build normali **non** toccano più questa
cache da sole (la pulizia automatica riguarda solo `.pio/build/<env>`, per un
motivo diverso — vedi sotto): usa questo endpoint solo se sospetti che serva
una versione di libreria diversa da quella già scaricata, altrimenti le build
successive alla prima restano rapide riusando le librerie già presenti.

**Nota sul pragma "Possible failure to include lv_conf.h":** LVGL stampa
questo messaggio informativo (non un errore) per ogni file compilato quando
si usa `LV_CONF_INCLUDE_SIMPLE` con PlatformIO — un artefatto noto e innocuo
del suo controllo interno, non un segnale che qualcosa sia davvero rotto (i
valori realmente usati arrivano comunque forzati via `-D` nei `build_flags` di
`firmware/platformio.ini`, indipendentemente da questo controllo). La build
`main` risolve le dipendenze con `pio pkg install` prima di compilare, poi
patcha `.pio/libdeps/<env>/lvgl/src/lv_conf_internal.h` per silenziare quella
riga (`_patch_lvgl_pragma` in `firmware_builder.py`) — puramente cosmetico,
non cambia alcuna logica di compilazione.

**Nota sulla BSP:** `firmware/lib/esp32_4848s040_bsp/` fornisce già
`display_init()`/`touch_init()` per il modulo ESP32-4848S040 (quello di
questo progetto). Se usi un modulo diverso, sostituisci quella cartella con
la BSP del tuo venditore mantenendo la stessa integrazione con LVGL — vedi
`firmware/lib/README.md`. Solo in quel caso, senza una BSP compatibile, la
build fallisce in fase di link con `undefined reference to display_init()`.

## Flashing: dal browser, via Web Serial (nessun accesso USB lato server)

La USB **non** tocca mai il container Docker. Il flashing avviene interamente
nel browser (Chrome o Edge, gli unici che supportano l'API Web Serial),
sul PC su cui hai aperto la web UI — che può essere il ryzen_server, il tuo
portatile Windows, o qualunque altra macchina raggiunga la pagina via VPN.

1. Colleghi il display alla porta USB **di quel PC** (non serve che sia l'host Docker)
2. Nella web UI, sezione "Flashing firmware", carichi il `.bin` (`POST /api/firmware`)
   o scegli un firmware già caricato in precedenza (`GET /api/firmware`)
3. Il componente `<esp-web-install-button>` (libreria [ESP Web Tools](https://esphome.github.io/esp-web-tools/))
   legge `GET /api/firmware/<filename>/manifest.json`:
   ```json
   {
     "name": "1758..._firmware.bin",
     "version": "1.0.0",
     "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": "/api/firmware/<filename>/bin", "offset": 0}]}]
   }
   ```
4. Premendo "Flasha questo firmware", il browser chiede il permesso di accedere
   alla porta seriale (prompt nativo del browser, non nostro), scarica il `.bin`
   da `GET /api/firmware/<filename>/bin` e lo scrive via Web Serial — tutta
   la logica del protocollo ESP32 (handshake, stub, scrittura flash, progress bar)
   è gestita dalla libreria, non dal nostro codice
5. Una volta completato, scolleghi la USB: al riavvio il display si connette
   da solo in WiFi e richiama `hello` + `poll` come sopra

**Nota sul formato del `.bin`:** il manifest assume un singolo file "merged"
(bootloader + tabella partizioni + applicazione uniti in un solo binario)
scritto all'offset `0x0`. Generandolo con "Genera firmware" (sezione sopra)
questo passaggio è già automatico. Se invece compili altrove (Arduino IDE,
PlatformIO in locale) e carichi tu il `.bin`, uniscilo prima con:
```
esptool.py --chip esp32s3 merge_bin -o firmware_merged.bin \
  0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

## Layout type → struttura payload

| layout_type   | struttura payload                                             |
|---------------|------------------------------------------------------------------|
| `big_number`  | `{"title": str, "value": number|null, "unit": str}`              |
| `status_grid` | `{"items": [{"label": str, "value": str|number}, ...]}`          |
| `text_log`    | `{"lines": [str, ...]}`                                          |
| `gauge`       | (da definire quando servirà un primo widget con questo layout)   |

Aggiungere un nuovo `layout_type` richiede: una `build_<tipo>_screen()` e una
`update_<tipo>()` nel firmware (`main.cpp`), nessuna modifica lato server.

## Configurazione e microSD via USB

Comandi JSON-per-riga accettati dal **loader con schermo** e dal **firmware
principale in tutte le modalità** (in modalità USB anche mentre il display
aspetta la risposta del ponte). Implementazione: `firmware/lib/dh_config/src/DhConfigSerial.cpp`.
Ogni risposta riporta `cmd`; gli errori hanno la forma
`{"type": "error", "cmd": "...", "error": "motivo"}`.

La configurazione è il file `displayhub.txt` (formato INI, vedi
`lib/dh_config/src/DhIni.h`): sulla microSD nelle build con l'opzione
"configurazione su microSD" (solo RAM quando manca), nella NVS nelle altre.

### Stato e azioni sull'archivio
```json
{"cmd": "storage_info"}
→ {"type": "storage_info", "backend": "sd|nvs|ram", "status": "microSD in uso", "message": "...",
   "dirty": false, "persistent": true, "files": true, "revision": 12,
   "card_total": 15931539456, "card_free": 15930441728,
   "pending_owner": "cucina-02", "pending_foreign": true}      (ultimi due solo con una scelta in attesa)
{"cmd": "storage_save"} | {"cmd": "storage_eject"} | {"cmd": "storage_reload"} | {"cmd": "storage_format"}
{"cmd": "storage_decide", "decision": "merge|use_card|overwrite|ignore"}
→ {"type": "storage_result", "cmd": "...", "ok": true, "revision": 13}
```
`files` indica se i comandi `file_*` sono utilizzabili (build con microSD e
scheda montata).

### Configurazione
```json
{"cmd": "config_get"}
→ {"type": "config", "backend": "sd", "status": "...", "persistent": true, "revision": 12, "text": "[server]\nhost = ..."}
{"cmd": "config_replace", "text": "[server]\nhost = 192.168.1.40\n..."}      (tutto il file, commenti compresi)
{"cmd": "config_set", "section": "server", "key": "host", "value": "192.168.1.40"}
{"cmd": "config_remove", "section": "server", "key": "host"}               (torna al valore della build)
{"cmd": "config_list_put", "section": "wifi", "key": "network", "entry": "Casa | password"}
{"cmd": "config_list_remove", "section": "wifi", "key": "network", "id": "Casa"}
{"cmd": "config_list_set", "section": "wifi", "key": "network", "entries": ["Hotspot | pw", "Casa | pw"]}
→ {"type": "config_result", "cmd": "...", "ok": true, "revision": 13}
```
Le modifiche vengono salvate subito (microSD o NVS); senza microSD restano in
RAM e si uniscono al file quando la scheda viene inserita. `config_replace`
registra le differenze voce per voce: all'unione con un file trovato dopo
vincono i valori scritti, le voci non toccate restano quelle del file.

### File sulla microSD
```json
{"cmd": "file_list", "path": "/"}
→ {"type": "file_list", "path": "/", "entries": [{"name": "displayhub.txt", "dir": false, "size": 812}]}
{"cmd": "file_read", "path": "/foto/a.png", "offset": 0, "length": 768}
→ {"type": "file_data", "path": "...", "offset": 0, "total": 20480, "data": "<base64>", "eof": false}
{"cmd": "file_write", "path": "/foto/a.png", "offset": 0, "data": "<base64>", "final": false}
→ {"type": "file_write_result", "ok": true, "path": "...", "next_offset": 768, "final": false}
{"cmd": "file_remove", "path": "/foto/a.png"} | {"cmd": "file_mkdir", "path": "/foto"}
→ {"type": "file_result", "cmd": "...", "ok": true}
```
- Blocchi di al massimo 768 byte (prima del base64): ogni riga resta sotto ~1,2 KB.
- Scrittura: `offset` 0 crea `<file>.part`; i blocchi successivi devono avere
  `offset` = byte già scritti (altrimenti errore: ricominciare da 0); con
  `final: true` il `.part` diventa il file definitivo. Un invio interrotto non
  lascia mai un file a metà.
- `displayhub.txt`, `.tmp`, `.bak` si leggono ma non si scrivono né cancellano
  con `file_*`: la configurazione passa dai comandi `config_*`.
- Percorsi assoluti dalla radice della scheda, senza `..`.

Il loader, nel suo `loader_hello`, elenca `"config"` tra le `features` e
anche `"sdfiles"` quando è una build con microSD.

## Sincronizzazione della configurazione col server

Il server conserva una copia del `displayhub.txt` di ogni display, con un
numero di revisione (`app/core/device_config.py`, file
`data/devices/<device_id>.json`, con le ultime 30 versioni). Solo nelle
modalità WiFi (in modalità USB la configurazione passa dai comandi seriali).

- Il poll riporta `"cfg_rev": N`: revisione della copia sul server (0 = nessuna).
- Il display ricorda in `[sync] rev` la revisione con cui è allineato (la
  sezione `[sync]` resta sua, il server non la conserva né la unisce).
- Se `cfg_rev` è diversa, oppure se la configurazione locale è cambiata
  dall'ultimo allineamento, oppure se il server non ne ha ancora una:
```json
POST /api/esp/config  {"device_id": "sala-01", "base_rev": 5, "text": "<configurazione>" | null}
→ {"rev": 6, "text": "<configurazione con cui allinearsi>"}
GET  /api/esp/config?device_id=sala-01   → {"rev": 6, "text": "..."}   (solo lettura)
```
  `text: null` = nessuna modifica locale, solo scaricare.
- Unione a tre vie quando sono cambiati sia il display sia il server (base =
  versione `base_rev` dallo storico): per ogni voce vince chi l'ha cambiata
  rispetto alla base; se l'hanno cambiata entrambi vince il display (è la
  modifica che arriva per ultima). Gli elenchi (reti, dispositivi e chiavi BT)
  si uniscono voce per voce. Base sconosciuta (display mai sincronizzato, es.
  microSD nuova, o base fuori dallo storico): sui singoli valori vince il
  server, gli elenchi si uniscono senza perdere voci.
- Senza microSD, all'avvio il display NON invia i valori della build: scarica
  la copia del server (che fa da copia di riserva).
- Password, chiavi Tailscale e chiavi Bluetooth viaggiano in entrambe le
  direzioni. Come il resto delle API non c'è autenticazione: la rete dev'essere
  fidata (LAN/VPN).

Web UI:
```json
GET /api/device-configs                       → [{"device_id", "rev", "updated_at", "source": "device|web"}]
GET /api/devices/<device_id>/config           → {"rev", "text", "updated_at", "source"}   (404 se non c'è)
PUT /api/devices/<device_id>/config  {"base_rev": 6, "text": "..."}
    → 200 {"rev": 7, "text": "..."}  |  409 {"error", "current": {"rev", "text"}} se cambiata nel frattempo
```
Il display riceve la modifica al poll successivo.

Test: `python3 tests/test_device_config.py` (formato, unione, archivio) e
`python3 tests/test_sync_protocol.py` (scenari con un display simulato contro
la web app vera).

## Decodifica dei crash

Il builder conserva l'ELF di ogni firmware generato accanto al `.bin`
(`data/firmware/elf/<nome>.elf`, cancellato insieme al `.bin`). La web UI, in
ogni sezione di compilazione, ha il riquadro "Decodifica un crash": si incolla
il log della console seriale e il server traduce gli indirizzi (Backtrace, PC,
Saved PC; EXCVADDR spiegato) in funzione / file / riga con `addr2line` della
toolchain PlatformIO già nel container (`app/core/crash_decoder.py`).
```json
POST /api/firmware/decode  {"log": "<testo del crash>", "firmware": null | "<nome .bin>"}
→ {"firmware": "...", "sha": "30ec9a28c6c8b2ab", "sections": [...], "text": "<risultato leggibile>"}
```
L'ELF giusto si trova dall'impronta `ELF file SHA256:` che il firmware stampa
nel crash, tra gli ELF conservati e quelli dell'ultima build di ogni progetto;
senza impronta si indica il firmware. I firmware generati prima di questa
funzione non hanno l'ELF conservato. Test: `DH_TEST_ELF=... DH_ADDR2LINE=...
python3 tests/test_crash_decoder.py`.

## Pagine: identificativi, ora esatta, colori (aggiornamento)

- Ogni pagina configurata ha un `page_id` (assegnato dal server, anche alle
  configurazioni esistenti): lo stesso widget può comparire più volte con
  parametri diversi, e ogni pagina ha i suoi dati, la sua cache e il suo ciclo
  di aggiornamento. `GET/POST /api/pages` lo riportano; la web UI lo conserva
  quando si modificano i parametri.
- Il poll risponde
  `{"config_version", "server_time", "pages": [{"page_id", "widget_id", "layout_type", "payload"}]}`:
  `server_time` è l'ora esatta del server (UTC) a ogni poll, per l'orologio del
  display; i dati dei widget possono avere qualche minuto. Il firmware aggiorna
  le pagine per `page_id` (o per posizione), non più per `widget_id`.
- `status_grid`: ogni riga `{"label", "value", "color"?}`; `color` "#RRGGBB"
  colora il valore (es. verde/rosso per una variazione di prezzo).
- `POST /api/widgets/<id>/preview` con `{"params": {...}}`: anteprima con i
  parametri indicati, calcolata al volo (non tocca i dati delle pagine).
- Schema dei parametri: nuovo tipo `multiselect` (`options`, `labels`
  facoltative, valore = elenco).

## Archivio firmware e display (web UI)

- `GET /api/firmware`: ogni firmware ha `meta` con le opzioni della build
  (`target`, `comm_mode`, `config_storage_sd`/`config_sd`, `device_id`,
  `fw_version`, `fw_revision`, `server_host`, `server_port`, `server_host_vpn`,
  `vpn_hostname`, `vpn_default_enabled`, `wifi_ssid`, `has_wifi_password`,
  `has_vpn_auth_key`, `has_vpn_auth_key_ephemeral`, `built_at`) salvate dal
  builder in `data/firmware/meta/<nome>.json`. Password e chiavi mai: solo
  `has_*`. Firmware precedenti: `meta` ricavato dal nome, `source: "nome del file"`.
  Anche `has_log`.
- `DELETE /api/firmware/<nome>` e `POST /api/firmware/delete-many {"filenames": [...]}`:
  cancellano .bin, ELF, log e dati della versione.
- `GET /api/firmware/<nome>/log`: log della build (testo).
- `GET|DELETE /api/firmware/orphan-logs`: log di build senza firmware (build fallite).
- `GET /api/devices`: display visti da questo avvio + quelli con solo la
  configurazione salvata (`known_only`), con `has_config`.
- `DELETE /api/devices/<id>[?config=1]`: dimentica un display (e con `config=1`
  anche la sua configurazione salvata). Se è acceso ricompare al prossimo poll.

La pagina è divisa in sezioni (`#/` widget e display, `#/firmware`, `#/usb`,
`#/configurazione`) senza ricaricare: una connessione USB aperta dal browser
resta attiva passando da una sezione all'altra.

## Documentazione e registro delle versioni (web UI)

- `GET /api/docs` → `[{"id", "title", "group", "path", "updated_at"}]`: i file
  .md del progetto (elenco fisso in `app/core/docs.py`; i quattro della radice
  sono copiati nell'immagine dal `Dockerfile`, quelli del firmware arrivano dal
  volume `./firmware`).
- `GET /api/docs/<id>` → `{"html", "toc", ...}`: Markdown convertito in HTML
  (tabelle, codice, indice); i link a un altro documento diventano
  `#/documentazione/<id>`.
- `GET /api/changelog` → `{"entries": [{"id", "version", "revision", "date",
  "title", "notes", "notes_html"}], "current": {...}, "next_revision"}`. La
  prima voce è la versione attuale.
- `POST /api/changelog` (nuova voce, va in cima), `PUT|DELETE /api/changelog/<id>`.
  Versione e revisione: lettere, numeri, `. _ -` (max 32); data AAAA-MM-GG;
  note in Markdown (l'HTML scritto nelle note viene neutralizzato).
- Il form di compilazione propone versione e revisione della voce attuale
  (anche dal server, al caricamento della pagina); restano modificabili, e un
  valore cambiato a mano non viene sovrascritto.
- Registro: `data/changelog.json`; al primo avvio nasce con la versione
  dell'ultimo firmware nell'archivio.

## Accesso

Con una password configurata (`DH_AUTH_PASSWORD_HASH` o `DH_AUTH_PASSWORD`),
tutte le pagine e le API del browser richiedono il login (`/login`, `/logout`):
senza sessione le API rispondono `401 {"error": "login richiesto", "login": "/login"}`
e la pagina rimanda al login. Le API `/api/esp/*` restano senza login ma
rispondono `403` alle richieste arrivate da un reverse proxy (intestazione
`X-Forwarded-For`), salvo `DH_ESP_VIA_PROXY=1`. Dettagli in `app/core/auth.py`.

Per i firmware VIDAA che rifiutano gli schemi noti, `tools/tv_capture.py` cattura la comunicazione app↔TV (vedi `tools/README_cattura_tv.md`).

## Layout `button_grid` (widget interattivi)

Per i widget con pulsanti a video: il telecomando TV (`tv_remote`) e il player Bluetooth (`bt_player`).
Il display disegna una griglia di pulsanti e, al tocco, manda l'azione al server. Payload:

```json
{"title": "Audio Bluetooth", "action_endpoint": "/api/esp/audio-action", "initial_view": "main",
 "views": {"main": {"columns": 4, "header": {...}, "buttons": [...]},
           "sources": {"columns": 1, "buttons": [...]}}}
```

- **Vista**: `columns` colonne; le righe sono quelle usate dai pulsanti. Il display parte da `initial_view`.
- **Pulsante**: `row`, `col`, `colspan` (default 1), `icon` (nome dell'icona, vedi `firmware/lib/dh_icons`),
  `label` (testo; con l'icona sta sotto di lei), `action`, `style` (colore, vedi `btn_style_color` in
  `firmware/src/main.cpp`; uno stile che finisce in `_circle` disegna un pulsante tondo), `inline` (true =
  icona e testo affiancati, per i pulsanti larghi).
- **Scheda in testa** (`header`, facoltativa, solo in quella vista): `height` (px, 100), `badge` (fonte, piccolo),
  `title`, `subtitle` (una riga ciascuno, con "..." se lunghi), `progress` (0-100; -1 = nessuna barra). La
  griglia comincia sotto la scheda. La scheda non e' cliccabile: sopra di lei lo swipe funziona.
- **Azioni** (stringa): `view:nome` cambia vista sul display, senza interpellare il server;
  tutte le altre vanno al server con `POST <action_endpoint>` e corpo `{"action": "..."}`. Un'azione puo' essere
  composta, `parte;parte;view:x`: le parti per il server partono in ordine, il cambio vista per ultimo
  (es. `source:youtube;view:main`). La risposta del server non cambia lo schermo: il display si ridisegna
  solo quando, al poll successivo, il contenuto della pagina e' cambiato.
- **TV** (`/api/esp/tv-action`): `key:KEY_VOLUMEUP`, `power:toggle`...
- **Player Bluetooth** (`/api/esp/audio-action`, `?gateway=<id>` sceglie il gateway): `vol:up|down|mute`,
  `source:mp3|youtube|radio`, `audio:play_pause|prev|next|stop`. Volume e scelta della fonte funzionano; le
  fonti sono segnaposto e `audio:*` risponde `501` (`placeholder: true`) finche' la riproduzione non e' sviluppata.

Questi endpoint stanno sotto `/api/esp`: niente login (il display non puo' farlo) ma solo diretti, `403` dal
reverse proxy. Anche il gateway Bluetooth scarica la sua config da li': `GET /api/esp/bt-gateway-config?device_id=<id>`
(testo INI); il server annota l'indirizzo da cui l'ha scaricata e ci manda subito il volume (`POST /api/audio/volume`).

## Impostazioni > Gateway Bluetooth (display → server → gateway)

Rotte sotto `/api/esp` (senza login, solo dirette: `403` dal reverse proxy). Il server le inoltra all'API HTTP del
gateway (`GET /api/status`, `GET /api/bt/devices`, `POST /api/bt/scan|connect|disconnect|forget`) usando l'IP da cui
il gateway ha scaricato la config; i dispositivi conosciuti stanno nell'INI (`[bt] known = mac | nome`, `[bt] speaker`).
Gli errori hanno la forma `{"ok": false, "error": "..."}`: `400` id o MAC non validi, `502` gateway che non esegue.

| Rotta | Risposta / effetto |
|---|---|
| `GET /api/esp/bt-gateways` | `{"ok", "gateways": [{id, name, online, ip, mac, version, wifi_ssid, wifi_rssi, volume, bt_connected, target_addr, target_name, uptime_s, error}]}` (stato interrogato in parallelo, 1,5 s) |
| `GET /api/esp/bt-gateways/<id>` | `{"ok", "gateway": {...come sopra}, "known": [{addr, name, connected, selected}]}` |
| `POST /api/esp/bt-gateways/<id>/scan` | avvia la scansione (12 s) sul gateway |
| `GET /api/esp/bt-gateways/<id>/scan` | `{"ok", "scanning", "found": [{addr, name, rssi, known}]}` (i piu' forti prima, max 20) |
| `POST /api/esp/bt-gateways/<id>/connect` `{addr, name}` | il gateway si connette; poi `known` + `speaker` nell'INI |
| `POST /api/esp/bt-gateways/<id>/disconnect` | scollega e svuota `speaker` (altrimenti il giro di config successivo la riconnetterebbe) |
| `POST /api/esp/bt-gateways/<id>/forget` `{addr}` | toglie da `known`; se era in uso il gateway la dimentica |

Nome e id del gateway si scelgono alla compilazione (`POST /api/firmware/build-gateway` con `{"name": "Gateway Salotto"}`,
`400` se non valido): vedi `firmware/gateway/README.md`.

