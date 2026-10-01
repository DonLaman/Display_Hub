# Cattura app ↔ TV Hisense (per ricavare lo schema di autenticazione)

La TV (firmware VIDAA recente) rifiuta gli schemi di autenticazione pubblici
noti. Questo strumento intercetta la comunicazione tra un'app di telecomando che
**funziona** e la TV, per leggere identità, utente, password ed eventuale token.
Tutto sulla tua rete e verso la tua TV.

## Serve
- L'app `com.universal.remote.multi` (o simile) sul telefono, **già in grado di
  comandare la TV**, sulla stessa rete WiFi.
- Il certificato client in `data/tv/` (già presente).
- `openssl` sul Ryzen (di solito c'è).

## Uso
Sul Ryzen, nella cartella del progetto:

    sudo python3 tools/tv_capture.py --tv 192.168.1.85 --self 192.168.1.40 --mac dc:9a:7d:b5:ee:26

(`--self` è l'IP del Ryzen, `--mac` il MAC della TV.) Lo strumento:
- si annuncia in rete come la tua TV, con nome **"TV-Salotto (ponte)"**;
- fa da ponte sulla porta 36669 verso la TV vera.

Poi, sul telefono:
1. chiudi del tutto l'app e riaprila (così rifà la ricerca);
2. quando compaiono **due** TV, scegli quella con **"(ponte)"** nel nome;
3. tocca il dispositivo con "(ponte)" (o quello "Pronto per la connessione")
   e abbina: il codice appare sulla TV vera, inseriscilo nell'app;
4. premi qualche tasto (volume su/giù).

Ferma con **Ctrl+C** e inviami **`data/tv_capture.log`**.

## Se l'app NON mostra la TV col "(ponte)"
Vuol dire che non cerca via SSDP. Nel log vedrai comunque le righe "Ricerca da
<telefono> (cerca: ...)": mandamele, così aggiungo il metodo giusto (di solito
mDNS). In alternativa, si può reindirizzare sul router l'IP della TV verso il
Ryzen per la durata della prova.

## Sicurezza
Riservato alla tua rete: cattura solo il tuo traffico verso la tua TV. Al
termine puoi cancellare `data/tv/bridge.crt` e `bridge.key` (il certificato
temporaneo del ponte) e `data/tv_capture.log`.
