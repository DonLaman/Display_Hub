#!/usr/bin/env python3
"""
Cattura della comunicazione tra un'app di telecomando Hisense e la TV VIDAA,
per ricavare lo schema di autenticazione che il connettore non conosce ancora.

DA LANCIARE SUL RYZEN (non nel container: gli annunci in rete non escono dalla
rete "bridge" di Docker):

    sudo python3 tools/tv_capture.py --tv 192.168.1.85 --self 192.168.1.40

Cosa fa:
  1. chiede alla TV come si presenta (SSDP/UPnP) e ripete lo stesso annuncio con
     l'IP di questo server e nome "... (ponte)", così l'app lo trova nella scansione;
  2. registra anche le ricerche del telefono: se cerca la TV in un altro modo
     (mDNS), lo si vede e si aggiunge;
  3. fa da ponte TLS sulla porta 36669: l'app si collega qui, lui alla TV vera, e
     registra in chiaro la CONNECT (identità, utente, password) e i messaggi MQTT,
     token compreso.

Uso: avvia lo script, sul telefono apri l'app e scegli la TV col nome "(ponte)",
poi abbina come sempre (il codice appare sulla TV vera). Ctrl+C per fermare.
Serve il certificato client in data/tv/ (già presente). Richiede solo openssl
(per creare il certificato del ponte) e paho non serve. sudo: porte 1900 e 36669.

RISERVATO ALLA TUA RETE: cattura solo il tuo traffico verso la tua TV.
"""
import argparse
import http.server
import os
import re
import socket
import ssl
import struct
import subprocess
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "data")
CERT_DIR = os.path.join(DATA, "tv")
LOG = os.path.join(DATA, "tv_capture.log")
MQTT_PORT = 36669
DESC_PORT = 36680

_log_lock = threading.Lock()


def log(*parts):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), " ".join(str(p) for p in parts))
    print(line, flush=True)
    with _log_lock:
        with open(LOG, "a") as f:
            f.write(line + "\n")


def client_cert():
    for cert, key in (("client.pem", "client.key"), ("rcm_certchain_pem.cer", "rcm_pem_privkey.pkcs8")):
        c, k = os.path.join(CERT_DIR, cert), os.path.join(CERT_DIR, key)
        if os.path.exists(c) and os.path.exists(k):
            return c, k
    return None, None


def bridge_cert():
    """Certificato autofirmato del ponte (verso il telefono). Creato una volta."""
    c, k = os.path.join(CERT_DIR, "bridge.crt"), os.path.join(CERT_DIR, "bridge.key")
    if not (os.path.exists(c) and os.path.exists(k)):
        os.makedirs(CERT_DIR, exist_ok=True)
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", k,
                        "-out", c, "-days", "3", "-subj", "/CN=hisense-tv"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        log("Creato il certificato del ponte in data/tv/bridge.crt")
    return c, k


# ------------------------------------------------------------------ MQTT parsing
def _mqtt_string(data, i):
    n = struct.unpack(">H", data[i:i + 2])[0]
    return data[i + 2:i + 2 + n].decode("utf-8", "replace"), i + 2 + n


def parse_connect(data):
    try:
        i = 1
        while data[i] & 0x80:
            i += 1
        i += 1
        plen = struct.unpack(">H", data[i:i + 2])[0]
        i += 2 + plen + 1               # nome protocollo + livello
        flags = data[i]; i += 3         # flags + keepalive
        cid, i = _mqtt_string(data, i)
        user = pw = None
        if flags & 0x04:                # will
            _, i = _mqtt_string(data, i); _, i = _mqtt_string(data, i)
        if flags & 0x80:
            user, i = _mqtt_string(data, i)
        if flags & 0x40:
            pw, i = _mqtt_string(data, i)
        return cid, user, pw
    except Exception as exc:
        return None, None, f"(parse: {exc})"


def peek_publish(data):
    """topic + inizio del payload di un PUBLISH, per vedere token e messaggi."""
    try:
        if data[0] >> 4 != 3:
            return None
        i = 1
        while data[i] & 0x80:
            i += 1
        i += 1
        topic, i = _mqtt_string(data, i)
        if data[0] & 0x06:
            i += 2                       # packet id (QoS > 0)
        payload = data[i:i + 400].decode("utf-8", "replace")
        return topic, payload
    except Exception:
        return None


# ------------------------------------------------------------------ ponte TLS
def pump(src, dst, tag):
    buf = b""
    try:
        while True:
            d = src.recv(4096)
            if not d:
                break
            head = d[0] >> 4
            if tag == "app→TV" and head == 1:
                cid, user, pw = parse_connect(d)
                log("  CONNECT dall'app:")
                log("    client_id:", cid)
                log("    username :", user)
                log("    password :", pw)
            pub = peek_publish(d)
            if pub and ("token" in pub[0] or "auth" in pub[0] or head == 3 and tag == "TV→app"):
                log(f"  {tag} {pub[0]}")
                if pub[1].strip():
                    log("    payload:", pub[1][:300])
            dst.sendall(d)
    except OSError:
        pass
    finally:
        for x in (src, dst):
            try:
                x.close()
            except OSError:
                pass


def handle_client(client, tv_ip):
    log("Connessione dall'app:", client.getpeername()[0])
    certfile, keyfile = client_cert()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        ctx.set_ciphers("DEFAULT:@SECLEVEL=0")
    except ssl.SSLError:
        pass
    if certfile:
        ctx.load_cert_chain(certfile, keyfile)
    try:
        tv = ctx.wrap_socket(socket.create_connection((tv_ip, MQTT_PORT), timeout=8))
    except (OSError, ssl.SSLError) as exc:
        log("  impossibile raggiungere la TV vera:", exc)
        client.close()
        return
    threading.Thread(target=pump, args=(client, tv, "app→TV"), daemon=True).start()
    threading.Thread(target=pump, args=(tv, client, "TV→app"), daemon=True).start()


def run_bridge(tv_ip):
    cert, key = bridge_cert()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    try:
        ctx.set_ciphers("DEFAULT:@SECLEVEL=0")
    except ssl.SSLError:
        pass
    ctx.load_cert_chain(cert, key)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", MQTT_PORT))
    srv.listen(5)
    log(f"Ponte TLS in ascolto sulla porta {MQTT_PORT} → TV {tv_ip}")
    while True:
        try:
            conn, _ = srv.accept()
            tls = ctx.wrap_socket(conn, server_side=True)
            handle_client(tls, tv_ip)
        except ssl.SSLError as exc:
            log("TLS rifiutato dall'app:", exc)
        except OSError:
            break


# ------------------------------------------------------------------ SSDP
def fetch_tv_description(tv_ip):
    """Come si presenta la TV (per copiarne nome/modello). Non blocca se non risponde."""
    for port in (36680, 1900, 8080, 8060):
        for path in ("/", "/desc.xml", "/description.xml", "/dmr.xml"):
            url = f"http://{tv_ip}:{port}{path}"
            try:
                with urllib.request.urlopen(url, timeout=2) as r:
                    body = r.read(4000).decode("utf-8", "replace")
                    if "<" in body:
                        log("Descrizione della TV da", url)
                        name = re.search(r"<friendlyName>(.*?)</friendlyName>", body)
                        if name:
                            log("  nome:", name.group(1))
                        return url, body
            except Exception:
                continue
    log("Descrizione UPnP della TV non trovata (non è un problema: l'annuncio funziona comunque)")
    return None, None


class DescHandler(http.server.BaseHTTPRequestHandler):
    xml = b""

    def do_GET(self):
        log("  l'app ha chiesto GET", self.path, "| header:", dict(self.headers))
        self.send_response(200)
        self.send_header("Content-Type", "text/xml; charset=\"utf-8\"")
        self.send_header("Application-URL", f"http://{self.server.server_address[0]}:36680/")
        self.end_headers()
        self.wfile.write(self.xml)

    def do_POST(self):
        log("  l'app ha inviato POST a", self.path)
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""
        if body:
            log("    corpo:", body[:300].decode("utf-8", "replace"))
        self.send_response(200)
        self.send_header("Content-Type", "text/xml")
        self.end_headers()
        self.wfile.write(self.xml)

    def log_message(self, *a):
        log("  l'app ha scaricato la descrizione del ponte")


def run_ssdp(self_ip, tv_ip, friendly, mac_nodash="aabbccddeeff"):
    _, tv_xml = fetch_tv_description(tv_ip)
    # descrizione minima se la TV non l'ha fornita
    DescHandler.xml = (tv_xml or f"""<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0"><specVersion><major>1</major><minor>0</minor></specVersion>
<device>
<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>
<friendlyName>{friendly} (ponte)</friendlyName>
<manufacturer>Hisense</manufacturer><manufacturerURL>http://www.hisense.com</manufacturerURL>
<modelDescription>VIDAA TV</modelDescription><modelName>HISENSE VIDAA</modelName>
<modelNumber>58A6CG</modelNumber><serialNumber>0000001</serialNumber>
<UDN>uuid:00000000-0000-0000-0000-{mac_nodash}</UDN>
<hisense:X_MQTT_PORT xmlns:hisense="urn:hisense">36669</hisense:X_MQTT_PORT>
<hisense:X_DEVICEID xmlns:hisense="urn:hisense">{self_ip}</hisense:X_DEVICEID>
</device></root>""").encode()
    if tv_xml:  # nel copiato, sostituisce l'IP della TV col nostro e marca il nome
        DescHandler.xml = re.sub(tv_ip.encode(), self_ip.encode(), DescHandler.xml)
        DescHandler.xml = re.sub(rb"(<friendlyName>)(.*?)(</friendlyName>)",
                                 rb"\1\2 (ponte)\3", DescHandler.xml)
    httpd = http.server.HTTPServer(("", DESC_PORT), DescHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    location = f"http://{self_ip}:{DESC_PORT}/"

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("", 1900))
    mreq = socket.inet_aton("239.255.255.250") + socket.inet_aton("0.0.0.0")
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
    log(f"Annuncio SSDP attivo (nome '{friendly} (ponte)', posizione {location})")
    seen = set()
    while True:
        try:
            data, addr = sock.recvfrom(2048)
        except OSError:
            break
        if b"M-SEARCH" not in data:
            continue
        st = re.search(rb"ST:\s*(.*?)\r", data)
        st = st.group(1).decode().strip() if st else "ssdp:all"
        if addr[0] not in seen:
            seen.add(addr[0])
            log(f"Ricerca da {addr[0]} (cerca: {st})")
        for target in ("urn:schemas-upnp-org:device:MediaRenderer:1", "upnp:rootdevice", st):
            resp = (f"HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nLOCATION: {location}\r\n"
                    f"ST: {target}\r\nUSN: uuid:dh-bridge-0001::{target}\r\nEXT:\r\nSERVER: VIDAA\r\n\r\n").encode()
            sock.sendto(resp, addr)


def main():
    ap = argparse.ArgumentParser(description="Cattura app↔TV Hisense VIDAA")
    ap.add_argument("--tv", required=True, help="IP della TV (es. 192.168.1.85)")
    ap.add_argument("--self", dest="self_ip", required=True, help="IP di questo server (es. 192.168.1.40)")
    ap.add_argument("--name", default="TV-Salotto", help="nome mostrato all'app")
    ap.add_argument("--mac", default="aa:bb:cc:dd:ee:ff", help="MAC della TV (per l'UDN dell'annuncio)")
    args = ap.parse_args()
    open(LOG, "w").close()
    log("=== Cattura avviata. Apri l'app sul telefono e scegli la TV con '(ponte)' nel nome. Ctrl+C per fermare. ===")
    if not client_cert()[0]:
        log("ATTENZIONE: nessun certificato client in data/tv/: la connessione alla TV vera potrebbe fallire.")
    threading.Thread(target=run_ssdp, args=(args.self_ip, args.tv, args.name, args.mac.replace(":", "").lower()),
                     daemon=True).start()
    try:
        run_bridge(args.tv)
    except PermissionError:
        log("Permesso negato sulle porte 1900/36669: rilancia con sudo.")
    except KeyboardInterrupt:
        pass
    log("=== Fine. Registro salvato in data/tv_capture.log — inviamelo. ===")


if __name__ == "__main__":
    main()
