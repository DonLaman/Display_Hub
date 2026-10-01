FROM python:3.12-slim

# git/curl servono a PlatformIO per scaricare toolchain e librerie alla prima build;
# network-manager fornisce nmcli per il best-effort di scansione WiFi (vedi wifi_scan.py:
# funziona solo se il container ha accesso all'hardware WiFi dell'host, non garantito
# di default — vedi nota in docker-compose.yml).
RUN apt-get update && apt-get install -y --no-install-recommends \
    git curl network-manager \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app/ ./app/
COPY firmware/ ./firmware/
# Documentazione del progetto, mostrata come pagine nella web UI ("Documentazione")
COPY README.md PROTOCOL.md MODIFICHE_VPN.md TODO.md ./

# La prima build PlatformIO scarica toolchain/librerie da internet: assicurati
# che il container abbia accesso in uscita (nessun proxy che lo blocchi).
RUN pio --version || true

# Un solo worker: la cache dati, il registro dispositivi e lo stato di
# build/flashing vivono in memoria di processo. Con più worker ognuno avrebbe
# la propria copia, scoordinata con le altre.
CMD ["gunicorn", "--workers", "1", "--threads", "8", "--timeout", "120", "--bind", "0.0.0.0:12000", "app.main:app"]
