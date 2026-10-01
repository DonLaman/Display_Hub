# Gateway Bluetooth (ESP32 classico)

Il display (ESP32-S3) ha solo Bluetooth Low Energy; le casse Bluetooth ricevono
l'audio con **A2DP**, che richiede il **Bluetooth Classic**. Il gateway è un
secondo microcontrollore, un **ESP32 classico** (ESP32 DevKit / WROOM-32, *non*
S3/C3), che fa da ponte:

```
display / server ──WiFi (API HTTP)──▶ gateway ESP32 ──Bluetooth A2DP──▶ cassa
                          USB (JSON) ─┘
```

## Stato

Gateway su **ESP32-WROVER** (8 MB di PSRAM, environment PlatformIO `esp-wrover-kit`; serve un ESP32 *classico*,
non S3/C3). Fatto: WiFi, ricerca/connessione/riconnessione della cassa, volume, tono di prova, API HTTP e mDNS,
**config comandata dal server**, **LED di stato** e **portale WiFi di fallback**.
Da fare: streaming audio (il server prepara il flusso, il gateway lo bufferizza verso la cassa), pannello sul
display, ricerca YouTube.

### Config dal server
Il gateway scarica il suo INI da `GET /api/esp/bt-gateway-config?device_id=<id>` (sotto `/api/esp`: senza login, come le API dei display; la vecchia `/api/bt-gateway/config` e' dietro il login della web app) e applica cassa (`[bt] speaker`) e
volume (`[bt] volume`); lo ricontrolla ogni `[server] poll_interval_s` (default 30 s). Si modifica dal pannello
"Gateway Bluetooth audio" della web UI. Si indica al gateway dove andare con il comando `server_config`
(`host`, `port`, `device_id`; via USB o `POST /api/server`). Il WiFi resta locale al gateway.

### LED di stato (IO2, cambiabile con `led_pin`)
| Stato | LED |
|---|---|
| Senza WiFi (attesa / portale) | 3 lampeggi lenti, pausa 1 s |
| WiFi connesso, cassa no | 3 raffiche da 2 lampeggi veloci, pausa 1 s |
| Cassa connessa | fisso |

### Portale WiFi di fallback
Senza WiFi da ~25 s (primo avvio, o rete assente) il gateway apre la rete **`DH-Gateway-Setup`**. Dal telefono:
la pagina di setup si apre da sola (DNS captive), oppure vai a `http://192.168.4.1/portal`; scegli la rete
dall'elenco (o scrivi il nome di una nascosta) e la password. Le credenziali si **salvano solo se la connessione
riesce**; se fallisce, il portale resta aperto col messaggio d'errore e torna la rete salvata prima. A
connessione riuscita il portale si chiude da solo. Il portale risponde solo a chi è collegato alla sua rete,
non alla LAN. Per proteggerlo con password: `gateway_config` con `ap_pass` (8-63 caratteri; vuota = aperto).
Le parti pure (escape HTML, validazione) si provano sul PC con `test_host/portal/run.sh`.

## Nome del gateway (alla compilazione)

Il nome si sceglie nella web UI, **Firmware > Gateway Bluetooth > "Nome del gateway"**, prima di "Genera gateway
Bluetooth" (lettere, numeri, spazio, `-` e `_`, da 1 a 24 caratteri, deve iniziare con una lettera o un numero).
Il builder scrive `firmware/gateway/include/gateway_build.h` con due valori che il firmware usa come predefiniti:

| Valore | Esempio | A cosa serve |
|---|---|---|
| `GW_NAME` | `Gateway Salotto` | nome mostrato nelle Impostazioni del display, nome Bluetooth e mDNS |
| `GW_DEVICE_ID` | `gateway-salotto` | id con cui il server lo riconosce: file `data/BT_setting/gateway-salotto.ini` |

L'id deriva dal nome (minuscolo, spazi e `_` diventano `-`) e compare anche nel file: `<timestamp>_gateway-gateway-salotto.bin`.
Senza nome restano `DH-Gateway` e `gateway-01`.

- **Con piu' gateway** serve un nome diverso per ciascuno (uno per build): due gateway con lo stesso id si contenderebbero lo stesso INI.
- Nome e id salvati sul gateway a runtime (comandi `gateway_config` / `server_config` via USB) **hanno la precedenza**
  su quelli della compilazione: se un gateway gia' configurato non cambia nome dopo il riflash, e' per questo.

## Impostazioni > Gateway Bluetooth sul display

Il display ha una sezione dedicata (Impostazioni > **Gateway Bluetooth**). Il display non parla col gateway:
passa dal **server**, che gira i comandi all'API HTTP del gateway; cosi' funziona anche con il display in VPN.

1. **Elenco dei gateway**: nome, online/offline, IP, cassa in uso. Compaiono quando hanno scaricato la config dal server
   (ogni 30 s; dopo un riavvio del server servono fino a 30 s prima che risultino online).
2. **Dettaglio** (tocco su un gateway): nome, id sul server, **MAC**, IP, WiFi, firmware, volume, cassa; poi i
   **dispositivi conosciuti** con quello connesso evidenziato. Si aggiorna da solo ogni 8 s.
3. **Cerca dispositivi**: scansione di 12 s sul gateway; i trovati compaiono man mano (nome, indirizzo, segnale, se
   gia' conosciuti). Tocco su uno = conferma, poi **associa e connette** (entra tra i conosciuti ed e' la cassa in uso).
4. **Dispositivo** (tocco su un conosciuto): info, **Connetti / Disconnetti**, **Dissocia** (con conferma: toglie dall'elenco;
   se era in uso il gateway la dimentica e si disconnette).

L'elenco dei conosciuti sta nell'INI del gateway sul server (e quindi resiste a un riflash):

```ini
[bt]
speaker = aa:bb:cc:dd:ee:01 | Cassa Salotto     ; la cassa in uso (vuota = nessuna)
known = aa:bb:cc:dd:ee:01 | Cassa Salotto       ; una riga per dispositivo conosciuto
known = aa:bb:cc:dd:ee:03 | TV Soggiorno
```

Regole: connetti, disconnetti e dissocia-la-cassa-in-uso richiedono il gateway **raggiungibile** e cambiano l'INI solo
se il gateway ha eseguito (altrimenti l'INI, che il gateway applica a ogni download, resterebbe in disaccordo con
quello che sta facendo). Togliere dall'elenco una cassa **non** in uso funziona anche con il gateway spento. Una cassa
gia' in uso sul gateway ma non in elenco (scelta prima via USB) viene aggiunta da sola. Un gateway con **token** non e'
ancora pilotabile dal display (errore "richiede un token").

## Primo avvio

1. Web UI → **Firmware** → *Genera gateway Bluetooth*, poi flashing con il
   gateway collegato via USB (il manifest indica `chipFamily: ESP32`).
2. Web UI → **USB e loader** → collega il gateway: si presenta come
   `dh-gateway`; *Scansiona* e *Connetti* le reti WiFi come con i loader.
3. Da quel momento risponde in LAN su `http://dh-gateway.local` (mDNS, servizio
   `_dhgw._tcp`) o sul suo IP.
4. Metti la cassa in modalità abbinamento, poi `bt_scan`, `bt_devices` e
   `bt_connect` (vedi sotto). Il gateway la ricorda e si ricollega da solo a
   ogni avvio.
5. `tone` fa suonare un tono di prova: verifica tutta la catena.

## Comandi

Stessi comandi via USB (una riga JSON `{"cmd": ...}`) e via HTTP (`POST` con
corpo JSON; `GET` per lo stato e l'elenco dei dispositivi). Con un token
configurato, le richieste HTTP devono avere l'header `X-DH-Token`.

| USB (`cmd`) | HTTP | Parametri | Risposta |
|---|---|---|---|
| `hello` | — | | `loader_hello` con `fw: "dh-gateway"` |
| `status` | `GET /api/status` | | `gateway_status` (WiFi, cassa, volume, audio, RAM) |
| `scan` | — | | `scan_result` (reti WiFi, formato del loader) |
| `connect` | — | `ssid`, `password` | `connect_result` (salvata solo se funziona) |
| `wifi_forget` | — | | |
| `gateway_config` | — | `name`, `token`, `ap_pass` | valgono dal prossimo avvio |
| `server_config` | `POST /api/server` | `host`, `port`, `device_id` | dove scaricare la config; vale subito |
| `fetch_config` | `POST /api/fetch-config` | | scarica e applica subito la config del server |
| `led_pin` | — | `pin` | cambia il pin del LED |
| `bt_scan` | `POST /api/bt/scan` | `seconds` (20) | avvia la ricerca |
| `bt_devices` | `GET /api/bt/devices` | | dispositivi visti negli ultimi 2 minuti |
| `bt_connect` | `POST /api/bt/connect` | `addr`, `name` | cerca la cassa e si collega; ricordata |
| `bt_disconnect` | `POST /api/bt/disconnect` | | |
| `bt_forget` | `POST /api/bt/forget` | | dimentica la cassa |
| `volume` | `POST /api/audio/volume` | `volume` 0–100 o `delta` ±N | |
| `tone` | `POST /api/audio/tone` | `freq` (440), `seconds` (3) | tono di prova |
| `stop` | `POST /api/audio/stop` | | |
| `reboot` | `POST /api/reboot` | | |

Esempio: `curl -X POST http://dh-gateway.local/api/audio/volume -d '{"delta": 5}'`

## Note tecniche

- **Volume:** si invia il *volume assoluto* AVRCP (le casse che lo supportano
  regolano il **loro** volume) e in più si scala il segnale, come riserva per
  le casse che lo ignorano.
- **Ricerca Bluetooth solo quando serve** (ricerca chiesta o cassa scelta non
  connessa): la libreria cercherebbe senza sosta, e la ricerca disturba il WiFi.
- **WiFi e Bluetooth insieme:** coesistenza radio del core attiva; il WiFi
  resta in risparmio energetico (obbligatorio con il Bluetooth acceso).
- **Libreria:** [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) 1.8.10,
  l'ultima che compila con arduino-esp32 2.x (dalla 1.8.11 richiede ESP-IDF 5).
- **Configurazione:** NVS del gateway (namespace `dhgw`): WiFi, nome, token,
  cassa scelta, volume. Qui Bluedroid serve (unico stack con A2DP): il
  controllo "niente Bluedroid" vale solo per il display.
