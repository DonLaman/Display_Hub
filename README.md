# Display Hub

Sistema modulare per pilotare un display ESP32-S3 (480x480, touch capacitivo)
via WiFi a runtime, con web UI per scegliere quali pagine mostrare e in che
ordine, swipe touch per navigare tra le pagine, e una sezione dedicata per
flashare il firmware via USB quando serve (solo quella fase usa la USB).

## Architettura

- Tu, dal browser via VPN, raggiungi la web UI sulla porta 12000 del ryzen_server
- La web UI (Flask) espone anche gli endpoint che il display chiama in WiFi
  (nessuna seriale a runtime)
- Il WiFi si configura sul display stesso, al primo boot, tramite un captive
  portal: nessuna credenziale passa dal server o dalla web UI (vedi PROTOCOL.md)
- Il display fa polling ogni ~4s su `/api/esp/poll`, riceve tutte le pagine
  attive con i dati già pronti
- Il flashing avviene interamente nel browser via Web Serial (ESP Web Tools):
  colleghi la USB al PC su cui hai aperto la pagina — qualunque esso sia,
  non deve essere il ryzen_server — il server si limita a conservare i `.bin`
  caricati/generati e a generare il manifest che la libreria lato client legge

## Struttura del progetto

```
display-hub/
├── app/
│   ├── core/
│   │   ├── registry.py         # scoperta automatica dei widget
│   │   ├── config_store.py     # persistenza pagine attive (data/config.json)
│   │   ├── orchestrator.py     # cache dati widget + config_version
│   │   ├── device_registry.py  # display WiFi connessi (device_id, ip, last_seen)
│   │   ├── firmware_store.py   # upload/elenco firmware caricati (nessun accesso USB)
│   │   └── firmware_builder.py # build server-side (PlatformIO) da host/device_id inseriti in web UI
│   ├── widgets/                 # <-- QUI si aggiungono nuove funzionalità
│   │   ├── base.py
│   │   ├── crypto_price.py
│   │   ├── htx_bot_status.py
│   │   ├── docker_health.py
│   │   ├── clock.py
│   │   ├── weather.py
│   │   ├── server_resources.py
│   │   ├── wbot_metrics.py
│   │   ├── acqua_metrics.py
│   │   ├── waha_status.py
│   │   └── notes.py
│   ├── templates/index.html     # web UI: pagine + dispositivi WiFi + flashing
│   ├── static/                  # CSS/JS della web UI
│   ├── routes.py                # ui_bp (pagine), api_bp (browser), esp_bp (display)
│   └── main.py                  # entry point, porta 12000
├── firmware/
│   ├── platformio.ini           # progetto PlatformIO del firmware definitivo
│   ├── src/main.cpp             # firmware: WiFi + polling HTTP + swipe LVGL
│   ├── loader/                  # progetto PlatformIO separato: firmware minimo
│   │   ├── platformio.ini       # per scansionare/testare il WiFi via USB dalla web UI
│   │   └── src/main.cpp
│   ├── include/
│   │   ├── device_config.h.template  # template, riempito dal server ad ogni build
│   │   ├── device_config.h.example   # riferimento per compilare a mano, se preferisci
│   │   └── lv_conf.h             # config minima LVGL (sostituiscila se la BSP ne fornisce una)
│   └── lib/
│       └── esp32_4848s040_bsp/   # BSP già pronta per il modulo ESP32-4848S040
├── PROTOCOL.md                  # protocollo HTTP server<->display + flusso di flashing
├── docker-compose.yml
├── Dockerfile
└── requirements.txt
```

## Deploy su ryzen_server

1. Copia la cartella `display-hub/` sul server, es. `/home/virus/ryzen_server/app/display-hub/`
2. Verifica che la porta 12000 sia raggiungibile dalla tua rete VPN verso l'host
   (nessuna esposizione pubblica necessaria: il compose pubblica solo su interfacce
   dell'host, la VPN fa il resto)
3. Se vuoi che i widget legati agli altri tuoi progetti (`htx_bot_status`,
   `wbot_metrics`, `acqua_metrics`, `waha_status`) raggiungano i rispettivi
   container per nome, collega `display-hub` alla rete Docker condivisa
   (decommenta la sezione `networks` nel compose)
4. Avvia:
   ```
   docker compose up -d --build
   ```
5. Apri `http://<ip-ryzen-server>:12000` (via VPN)

## Primo firmware: build + flashing dal browser

La BSP per il modulo **ESP32-4848S040** è già inclusa in
`firmware/lib/esp32_4848s040_bsp/` (adattata dal progetto open source
OraQuadraNano) — se il tuo display è esattamente questo modulo, non devi
procurarti nient'altro prima di compilare. Le dipendenze PlatformIO (Arduino_GFX,
TAMC_GT911, lvgl) sono già confermate corrette da una build reale; resta da
verificare solo la versione del core ESP32 richiesta (vedi le note in cima a
`firmware/platformio.ini`). Se invece il tuo modulo è diverso, vedi
`firmware/lib/README.md` per cosa sostituire.

Dalla web UI:

1. Sezione "Genera firmware": inserisci l'IP del ryzen_server raggiungibile
   dal display in LAN e un identificativo a piacere, poi premi "Genera firmware
   definitivo" (le credenziali WiFi non si inseriscono qui)
2. Segui il log finché non arriva a "Completato" — la prima build può
   richiedere qualche minuto perché PlatformIO scarica toolchain e librerie
3. **Prima di flashare il firmware definitivo**, conviene passare dal loader
   (sezione "Test connessione WiFi"): genera anche quello (build molto più
   rapida, nessun display/LVGL coinvolto), flashalo per primo, poi con il
   display ancora in USB premi "Connetti al display" nella stessa pagina,
   cerca le reti e prova la connessione con la password — se riesce, le
   credenziali restano salvate anche dopo aver flashato sopra il firmware
   definitivo (non serve più il captive portal a mano con lo smartphone)
4. Nella sezione "Flashing firmware" scegli il file giusto dal menu (marcato
   `[loader]` o `[definitivo]`), collega il display alla USB **di questo PC**
   (qualunque esso sia, non deve essere il ryzen_server) e premi "Flasha
   questo firmware" — Chrome/Edge chiederanno il permesso della porta
   seriale, poi tutto il resto (scrittura, progress bar) è gestito dalla
   libreria ESP Web Tools
5. Dopo aver flashato il firmware definitivo, scollega la USB e alimenta il
   display: se hai già testato il WiFi col loader si connette subito; altrimenti
   apre da solo il captive portal `DisplayHub-Setup-<device_id>` (vedi
   `PROTOCOL.md`). In entrambi i casi compare in pochi secondi in
   "Dispositivi WiFi connessi"

Per aggiornare in futuro (nuovo widget con nuovo layout, cambio IP del
server, ecc.) ripeti dal punto 1: nessuna configurazione delle pagine viene
persa, resta lato server indipendente dal firmware. Se cambia solo la rete
WiFi (es. sposti il display), non serve rigenerare il firmware definitivo:
ripeti il flusso loader (punti 3-4) da solo, oppure lascia che il display
rientri in modalità captive portal da sé.

## Aggiungere un nuovo widget

1. Crea `app/widgets/nome_widget.py`
2. Estendi `Widget` (vedi `base.py`), definisci `meta` e implementa `get_data()`
3. Se il layout (`big_number`, `status_grid`, `text_log`) esiste già, non
   serve toccare il firmware: la nuova pagina funziona da subito
4. Se serve un nuovo tipo di visualizzazione, aggiungi in `firmware/main.cpp`
   una `build_<tipo>_screen()` e una `update_<tipo>()`, poi usa quel nome come
   `layout_type` nel nuovo widget — vedi `PROTOCOL.md`. Va poi ricompilato e
   riflashato il firmware (dal browser, come sopra — unico caso in cui serve
   la USB dopo il primo setup)
5. Riavvia il container: il widget compare automaticamente nel catalogo della
   web UI (il registry lo scopre da solo)

## Note di sicurezza

- Gli endpoint `/api/esp/*` (usati dal display) non richiedono autenticazione:
  sono pensati per una rete locale fidata. Se la LAN del ryzen_server non è
  isolata a sufficienza, valuta di limitarli via firewall al solo IP del display
- Gli endpoint `/api/*` (usati dalla web UI, incluso build e flashing) sono
  raggiungibili solo passando dalla VPN: non esporli direttamente su internet
- `/api/firmware/build` lancia processi (`pio`, `esptool`) sul server con i
  parametri che gli passi: è pensato per essere usato solo da te via VPN, non
  va esposto a utenti non fidati
- I widget che dipendono da altri tuoi servizi (bot HTX, W-BOT, Acqua, WAHA)
  assumono endpoint di stato interni (`/status`, `/metrics`) che probabilmente
  non esistono ancora: sono scritti come esempio di integrazione, andranno
  completati con un vero endpoint lato bot/servizio

## Accesso alla web UI (utente e password)

1. Copia `.env.example` in `.env` (accanto a `docker-compose.yml`). In Portainer,
   metti le stesse variabili tra le *Environment variables* dello stack.
2. Genera l'hash della password, a container avviato:
   `docker exec -it display_hub python -m app.core.auth hash`
   e incolla la riga `DH_AUTH_PASSWORD_HASH=...` nel `.env`.
   (In alternativa `DH_AUTH_PASSWORD=` in chiaro.)
3. `docker compose up -d` (ricrea il container con le nuove variabili).

- "Ricordami" mantiene l'accesso per `DH_SESSION_DAYS` giorni (default 30).
  Cambiando la password tutte le sessioni aperte decadono.
- Senza password configurata l'accesso resta libero, con un avviso nel log.
- Le API dei display (`/api/esp/*`) non chiedono login, perché il firmware non
  può farlo, ma rispondono **solo alle richieste dirette** (porta 12000 in LAN o
  via VPN). Dal reverse proxy (NPM) vengono rifiutate: altrimenti chiunque
  raggiunga il nome pubblico leggerebbe le configurazioni dei display.
- Dopo 5 tentativi sbagliati in 15 minuti, lo stesso IP resta bloccato 15 minuti.

## TV Hisense VIDAA

Prima pagina della web UI → **TV Hisense (VIDAA)**: IP e MAC della TV, *Prova
connessione*, *Abbina* (la TV mostra un codice: inseriscilo e *Conferma*), poi
il telecomando. Il server parla con il broker MQTT della TV (porta 36669, lo
stesso dell'app RemoteNOW); configurazione in `data/tv.json`.

- Se la connessione viene **rifiutata** con la TV accesa (es. "handshake
  failure"), il firmware chiede il **certificato client** dell'app RemoteNOW.
  Non è incluso nel progetto: i file `rcm_certchain_pem.cer` e
  `rcm_pem_privkey.pkcs8` sono nell'archivio `hi_keys.zip` del repository
  pubblico github.com/d3nd3/Hisense-mqtt-keyfiles. Copiali in `data/tv/` sul
  server (così come sono, oppure rinominati `client.pem` / `client.key`), poi
  *Prova connessione*: non serve riavviare.
- Protocollo: per i firmware recenti (predefinito) il server usa l'identità
  `<MAC>$his$<hash>_vidaacommon_001`, credenziali calcolate dall'ora e, dopo il
  codice, un **token** che rinnova da solo (salvato in `data/tv.json`); se il
  token di rinnovo scade (dopo settimane) chiede di rifare l'abbinamento. Per le
  TV più vecchie si può mettere `"protocol": "legacy"` in `data/tv.json`.
- **Accendi** usa il Wake-on-LAN (da attivare nelle impostazioni di rete della
  TV). Dal container in rete "bridge" il broadcast può non uscire in LAN: se la
  TV non si accende, prova `network_mode: host` nel `docker-compose.yml`.

