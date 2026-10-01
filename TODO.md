# TODO — Display Hub

## VPN Tailscale

- [x] **Subnet route via VPN** — fatto: il display raggiunge indirizzi come
  192.168.1.40 dentro il tunnel quando un subnet router della tailnet pubblica
  quella rete (vedi PROTOCOL.md, "Subnet route"). Limiti noti:
  - solo le connessioni aperte con `DhNetClient` (oggi hello e poll);
  - max 3 subnet per dispositivo, IPv4;
  - se la rete di casa è divisa in più subnet collegate dal router, con la VPN
    accesa una subnet pubblicata diversa da quella del WiFi passa dal tunnel
    (funziona, ma con un giro più lungo): eventuale opzione "preferisci LAN".

## Configurazione su microSD (nuova opzione di build, modalità Completo)

- [x] **Configurazione su microSD** — fatto (8 step): lib/dh_config, librerie
  su dh_config, NimBLE, più reti WiFi, schermate, comandi seriali,
  sincronizzazione col server, web UI. Test sul PC:
  `firmware/test_host/dh_config/run.sh`, `firmware/test_host/dh_wifi/run.sh`,
  `python3 tests/test_device_config.py`, `python3 tests/test_sync_protocol.py`.

## WiFi

- [x] **Più reti salvate** — fatto (lib/dh_wifi): scelta della rete nota più forte
  tra quelle visibili (a pari segnale, entro 3 dB, vale l'ordine), passaggio
  automatico quando la rete cade, elenco riordinabile nelle impostazioni.
  Possibile aggiunta: cambio di rete anche da connessi se ne compare una nota
  molto più forte (oggi si cambia solo quando la rete corrente cade).

## Sicurezza

- [x] Accesso protetto alla web UI: utente e password dal container, sessione in
  cookie ("Ricordami"), blocco dopo tentativi sbagliati (`app/core/auth.py`).
  Le API dei display (/api/esp) rispondono solo alle richieste dirette, non dal
  reverse proxy.
- [ ] Token per display (generato in fase di build) per le API /api/esp: oggi
  chi è in LAN o nella tailnet può leggere la configurazione di un display
  conoscendone il Device ID.

## Audio e TV

- [ ] IN CORSO — TV Hisense VIDAA: connettore (libreria `vidaa-control`, abbinamento con PIN) e telecomando nella
  web UI ✔, widget telecomando sul display ✔ (icone trasparenti, colori, fasce libere per lo swipe), app
  (YouTube, Netflix...) e rifiniture.
  - [ ] Tastierino del telecomando sul display: il pulsante in basso a sinistra (freccia di ritorno) fa la stessa
    cosa di "Torna ai comandi" (`view:main`). Trasformarlo nel tasto "Indietro" della TV: azione
    `key:KEY_RETURNS`, etichetta "Indietro" (`_KEYPAD_BUTTONS` in `app/widgets/tv_remote.py`).
- [ ] IN CORSO — Gateway Bluetooth (ESP32-WROVER, A2DP verso le casse): firmware con WiFi, cassa, volume, config
  dal server, LED di stato e portale WiFi ✔ (compila e collega; dettagli in `firmware/gateway/README.md`).
  - [ ] Provarlo sul WROVER vero: portale WiFi (~25 s senza reti), config dal server, LED IO2 (se non e' libero:
    comando `led_pin`), volume in tempo reale.
  - [ ] Streaming audio verso la cassa (buffer sfruttando la PSRAM).
  - [x] Impostazioni → Gateway Bluetooth sul display (elenco, info/MAC, scansione, conosciuti, connetti/dissocia) e nome
    scelto alla compilazione. Resta da provare con il gateway fisico (scansione e connessione reali).
  - [ ] Rinominare il gateway dal display (oggi: alla compilazione, o `gateway_config` via USB).
- [ ] IN CORSO — Player Bluetooth sul display (widget `bt_player`): struttura ✔ (scheda "in riproduzione" in testa,
  controlli, volume vero e memorizzato, seconda pagina "Fonti" con segnaposto). Da sviluppare:
  - [ ] Riproduzione vera: play/pausa/precedente/successivo/stop oggi rispondono `501` (`core/audio_player.py`).
  - [ ] Fonte MP3: dove stanno i file, come si sfogliano dal display.
  - [ ] Fonte YouTube: ricerca e riproduzione con le API ufficiali; storico e preferiti hanno gia' il loro posto nel
    file del gateway (`[youtube]`).
  - [ ] Fonte Web radio: elenco delle emittenti e come sceglierle.
  - [ ] Scheda: titolo, artista, durata e barra di avanzamento reali (la barra c'e' gia': `progress` -1 la nasconde)
    e stato della cassa (connessa o no).
  - [ ] Token del gateway: se ne ha uno, l'invio immediato del volume fallisce (si applica comunque al giro dopo);
    decidere come passarlo al server.
