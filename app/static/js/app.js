// Sessione scaduta o disconnessa: le API rispondono 401 -> pagina di login,
// poi ritorno alla sezione in cui si era.
(function () {
  const origFetch = window.fetch.bind(window);
  window.fetch = async (...args) => {
    const res = await origFetch(...args);
    if (res.status === 401) {
      const next = location.pathname + location.hash;
      location.href = "/login?next=" + encodeURIComponent(next);
    }
    return res;
  };
})();

// Stato locale della UI: elenco pagine attive (ordine = ordine di swipe sul display)
let widgetCatalog = [];
let activePages = [];
let pendingWidgetId = null;
let editingIndex = null;   // indice della pagina in modifica (null = aggiunta)

async function loadCatalog() {
  const res = await fetch("/api/widgets");
  widgetCatalog = await res.json();
  renderCatalog();
}

async function loadPages() {
  const res = await fetch("/api/pages");
  activePages = await res.json();
  renderPageList();
}

// ---------- Dispositivi WiFi connessi ----------

async function loadDevices() {
  const res = await fetch("/api/devices");
  const devices = await res.json();
  const list = document.getElementById("device-list");
  list.innerHTML = "";
  if (devices.length === 0) {
    list.innerHTML = '<li class="page-item"><span class="device-item-meta">Nessun display si è ancora registrato via WiFi</span></li>';
    return;
  }
  devices.forEach(d => {
    const li = document.createElement("li");
    li.className = "page-item";
    const info = document.createElement("span");
    info.className = "page-item-text";
    const name = document.createElement("strong");
    name.textContent = d.device_id;            // testo, non HTML
    const meta = document.createElement("span");
    meta.className = "device-item-meta";
    if (d.known_only) {
      meta.textContent = "Non visto da quando il server è ripartito — c'è solo la sua configurazione salvata";
    } else {
      const ago = d.online ? "" : ` — visto ${formatAgo(d.seconds_since_seen)}`;
      meta.textContent = `${d.ip} — fw ${d.fw_version || "?"}${ago}${d.has_config ? " — configurazione salvata" : ""}`;
    }
    info.appendChild(name);
    info.appendChild(document.createElement("br"));
    info.appendChild(meta);
    li.appendChild(info);

    const actions = document.createElement("span");
    actions.className = "device-actions";
    const badge = document.createElement("span");
    badge.className = `status-badge ${d.online ? "status-ok" : "status-error"}`;
    badge.textContent = d.online ? "Online" : "Offline";
    actions.appendChild(badge);
    if (!d.online) {
      const forget = document.createElement("button");
      forget.className = "btn-secondary btn-small";
      forget.textContent = "Dimentica";
      forget.title = "Togli questo display dall'elenco";
      forget.addEventListener("click", () => forgetDevice(d));
      actions.appendChild(forget);
    }
    li.appendChild(actions);
    list.appendChild(li);
  });
}

function formatAgo(seconds) {
  if (seconds == null) return "";
  if (seconds < 90) return `${Math.round(seconds)} s fa`;
  if (seconds < 5400) return `${Math.round(seconds / 60)} min fa`;
  if (seconds < 172800) return `${Math.round(seconds / 3600)} ore fa`;
  return `${Math.round(seconds / 86400)} giorni fa`;
}

// Un display che non esiste più (vecchio Device ID). Se ha una configurazione
// salvata sul server si chiede se cancellare anche quella.
async function forgetDevice(d) {
  let withConfig = false;
  if (d.has_config) {
    withConfig = confirm(`Cancellare anche la configurazione di "${d.device_id}" salvata sul server?\n\n` +
      "OK = dimentica il display e cancella la sua configurazione\nAnnulla = dimentica solo il display");
  } else if (!confirm(`Togliere "${d.device_id}" dall'elenco?`)) {
    return;
  }
  const res = await fetch(`/api/devices/${encodeURIComponent(d.device_id)}${withConfig ? "?config=1" : ""}`, { method: "DELETE" });
  if (!res.ok) alert("Operazione non riuscita");
  await loadDevices();
}

// ---------- Copia log negli appunti (con fallback se Clipboard API non disponibile) ----------

async function copyLogToClipboard(preId, buttonEl) {
  const text = document.getElementById(preId).textContent;
  if (!text) return;

  const originalLabel = buttonEl.textContent;
  const showFeedback = (label) => {
    buttonEl.textContent = label;
    setTimeout(() => { buttonEl.textContent = originalLabel; }, 2000);
  };

  try {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
    } else {
      // Fallback per contesti non sicuri (http:// non-localhost): niente Clipboard API,
      // stesso limite già visto per Web Serial. Copia via textarea temporanea + execCommand.
      const textarea = document.createElement("textarea");
      textarea.value = text;
      textarea.style.position = "fixed";
      textarea.style.opacity = "0";
      document.body.appendChild(textarea);
      textarea.select();
      document.execCommand("copy");
      document.body.removeChild(textarea);
    }
    showFeedback("Copiato ✓");
  } catch (err) {
    showFeedback("Errore copia");
  }
}

// Pulisce solo la VISTA (il riquadro sullo schermo): il prossimo polling di
// stato di una build in corso la ripopola comunque dal server, quindi ha senso
// soprattutto per i log statici (build finita, o ponte USB) e per liberare
// spazio sullo schermo senza perdere nulla — il log completo resta comunque
// salvato su disco lato server per ogni tentativo di build.
function clearLogDisplay(preId) {
  document.getElementById(preId).textContent = "";
  if (preId === "usb-bridge-log") {
    try { localStorage.removeItem(USB_BRIDGE_LOG_STORAGE_KEY); } catch (_) {}
  }
}

// ---------- Build firmware server-side ----------

let buildPollTimer = null;

// Tre modalità del firmware principale (vedi PROTOCOL.md, POST /api/firmware/build):
// "wifi" = WiFi+BT, "wifi_vpn" = Completo (WiFi+VPN+BT), "usb" = USB+BT.
const COMM_MODE_SUMMARIES = {
  wifi: "WiFi diretto verso il server in LAN, Bluetooth disponibile dalle impostazioni sul display.",
  wifi_vpn: "Come WiFi + BT, più la VPN Tailscale all'occorrenza: sotto l'hotspot del telefono " +
            "(o qualunque WiFi) il display raggiunge il server tramite la tailnet, anche al suo " +
            "indirizzo 192.168.x.x se un subnet router pubblica quella rete.",
  usb: "Niente WiFi: i dati arrivano dal \"ponte USB\" (questa pagina, aperta sul PC collegato). " +
       "Bluetooth disponibile dalle impostazioni sul display.",
};

function updateCommModeFields() {
  const mode = document.getElementById("build-comm-mode-select").value;
  document.getElementById("wifi-mode-fields").style.display = mode === "usb" ? "none" : "block";
  document.getElementById("vpn-mode-fields").style.display = mode === "wifi_vpn" ? "block" : "none";
  document.getElementById("comm-mode-summary").textContent = COMM_MODE_SUMMARIES[mode] || "";
  document.getElementById("build-config-sd-hint").style.display =
    document.getElementById("build-config-sd-input").checked ? "block" : "none";
}

async function startBuild() {
  const comm_mode = document.getElementById("build-comm-mode-select").value;
  const server_host = document.getElementById("build-server-host-input").value.trim();
  const device_id = document.getElementById("build-device-id-input").value.trim();
  const wifi_ssid = document.getElementById("build-wifi-ssid-input").value.trim();
  const wifi_password = document.getElementById("build-wifi-password-input").value;
  const fw_version = document.getElementById("build-fw-version-input").value.trim();
  const fw_revision = document.getElementById("build-fw-revision-input").value.trim();

  const payload = {
    comm_mode, server_host, device_id, server_port: 12000, wifi_ssid, wifi_password,
    fw_version, fw_revision,
  };
  if (comm_mode === "wifi_vpn") {
    payload.server_host_vpn = document.getElementById("build-server-host-vpn-input").value.trim();
    payload.vpn_hostname = document.getElementById("build-vpn-hostname-input").value.trim();
    payload.vpn_auth_key = document.getElementById("build-vpn-auth-key-input").value.trim();
    payload.vpn_default_enabled = document.getElementById("build-vpn-default-enabled-input").checked;
    payload.vpn_auth_key_ephemeral = document.getElementById("build-vpn-auth-key-eph-input").value.trim();
    payload.config_storage_sd = document.getElementById("build-config-sd-input").checked;
    // server_host_vpn è facoltativo: con un subnet router che pubblica la rete del
    // server, il display raggiunge server_host dentro il tunnel.
  }

  const res = await fetch("/api/firmware/build", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (!res.ok) {
    const err = await res.json();
    alert(`Errore: ${err.error}`);
    return;
  }
  document.getElementById("build-log").textContent = "";
  if (buildPollTimer) clearInterval(buildPollTimer);
  buildPollTimer = setInterval(pollBuildStatus, 1500);
}

async function pollBuildStatus() {
  const res = await fetch("/api/firmware/build/status");
  const status = await res.json();
  if (status.target && status.target !== "main") return; // build in corso è quella del loader

  const badge = document.getElementById("build-status-badge");
  const log = document.getElementById("build-log");

  if (status.state === "idle") {
    badge.textContent = "Inattivo";
    badge.className = "status-badge status-unknown";
    return;
  }
  log.textContent = (status.log || []).join("\n");
  log.scrollTop = log.scrollHeight;

  if (status.state === "running") {
    badge.textContent = "Build in corso…";
    badge.className = "status-badge status-unknown";
    // Riprende il polling se non è già attivo: capita dopo un refresh della
    // pagina mentre una build è ancora in corso (l'intervallo si perde col
    // ricaricamento, ma la build sul server prosegue comunque).
    if (!buildPollTimer) buildPollTimer = setInterval(pollBuildStatus, 1500);
  } else if (status.state === "success") {
    badge.textContent = "Completata ✓";
    badge.className = "status-badge status-ok";
    clearInterval(buildPollTimer);
    await loadFlashFirmwareList(); // il nuovo firmware compare subito nel selettore di flashing
  } else if (status.state === "error") {
    badge.textContent = "Errore";
    badge.className = "status-badge status-error";
    clearInterval(buildPollTimer);
  }
}

// ---------- Build del loader di test WiFi ----------

let loaderBuildPollTimer = null;

async function startLoaderBuild() {
  const res = await fetch("/api/firmware/build-loader", { method: "POST" });
  if (!res.ok) {
    const err = await res.json();
    alert(`Errore: ${err.error}`);
    return;
  }
  document.getElementById("loader-build-log").textContent = "";
  if (loaderBuildPollTimer) clearInterval(loaderBuildPollTimer);
  loaderBuildPollTimer = setInterval(pollLoaderBuildStatus, 1500);
}

async function pollLoaderBuildStatus() {
  const res = await fetch("/api/firmware/build/status");
  const status = await res.json();
  if (status.target && status.target !== "loader") return; // build in corso è quella del firmware definitivo

  const badge = document.getElementById("loader-build-status-badge");
  const log = document.getElementById("loader-build-log");

  if (status.state === "idle") {
    badge.textContent = "Inattivo";
    badge.className = "status-badge status-unknown";
    return;
  }
  log.textContent = (status.log || []).join("\n");
  log.scrollTop = log.scrollHeight;

  if (status.state === "running") {
    badge.textContent = "Build in corso…";
    badge.className = "status-badge status-unknown";
    if (!loaderBuildPollTimer) loaderBuildPollTimer = setInterval(pollLoaderBuildStatus, 1500);
  } else if (status.state === "success") {
    badge.textContent = "Completata ✓";
    badge.className = "status-badge status-ok";
    clearInterval(loaderBuildPollTimer);
    await loadFlashFirmwareList();
  } else if (status.state === "error") {
    badge.textContent = "Errore";
    badge.className = "status-badge status-error";
    clearInterval(loaderBuildPollTimer);
  }
}

// ---------- Build del loader "pesante" (schermo/touch/WiFi/Bluetooth) ----------

let loaderFullBuildPollTimer = null;

async function startLoaderFullBuild() {
  const res = await fetch("/api/firmware/build-loader-full", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ config_sd: document.getElementById("build-loader-full-sd-input").checked }),
  });
  if (!res.ok) {
    const err = await res.json();
    alert(`Errore: ${err.error}`);
    return;
  }
  document.getElementById("loader-full-build-log").textContent = "";
  if (loaderFullBuildPollTimer) clearInterval(loaderFullBuildPollTimer);
  loaderFullBuildPollTimer = setInterval(pollLoaderFullBuildStatus, 1500);
}

async function pollLoaderFullBuildStatus() {
  const res = await fetch("/api/firmware/build/status");
  const status = await res.json();
  if (status.target && status.target !== "loader_full") return; // build in corso è un'altra

  const badge = document.getElementById("loader-full-build-status-badge");
  const log = document.getElementById("loader-full-build-log");

  if (status.state === "idle") {
    badge.textContent = "Inattivo";
    badge.className = "status-badge status-unknown";
    return;
  }
  log.textContent = (status.log || []).join("\n");
  log.scrollTop = log.scrollHeight;

  if (status.state === "running") {
    badge.textContent = "Build in corso…";
    badge.className = "status-badge status-unknown";
    if (!loaderFullBuildPollTimer) loaderFullBuildPollTimer = setInterval(pollLoaderFullBuildStatus, 1500);
  } else if (status.state === "success") {
    badge.textContent = "Completata ✓";
    badge.className = "status-badge status-ok";
    clearInterval(loaderFullBuildPollTimer);
    await loadFlashFirmwareList();
  } else if (status.state === "error") {
    badge.textContent = "Errore";
    badge.className = "status-badge status-error";
    clearInterval(loaderFullBuildPollTimer);
  }
}

// ---------- Build del gateway Bluetooth (ESP32 classico) ----------
let gatewayBuildPollTimer = null;

async function startGatewayBuild() {
  const res = await fetch("/api/firmware/build-gateway", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ name: document.getElementById("gateway-name-input").value.trim() }),
  });
  if (!res.ok) {
    const err = await res.json();
    alert(`Errore: ${err.error}`);
    return;
  }
  document.getElementById("gateway-build-log").textContent = "";
  if (gatewayBuildPollTimer) clearInterval(gatewayBuildPollTimer);
  gatewayBuildPollTimer = setInterval(pollGatewayBuildStatus, 1500);
}

async function pollGatewayBuildStatus() {
  const res = await fetch("/api/firmware/build/status");
  const status = await res.json();
  if (status.target && status.target !== "gateway") return; // build in corso è un'altra
  const badge = document.getElementById("gateway-build-status-badge");
  const log = document.getElementById("gateway-build-log");
  if (status.state === "idle") {
    badge.textContent = "Inattivo";
    badge.className = "status-badge status-unknown";
    return;
  }
  log.textContent = (status.log || []).join("\n");
  log.scrollTop = log.scrollHeight;
  if (status.state === "running") {
    badge.textContent = "Build in corso…";
    badge.className = "status-badge status-unknown";
    if (!gatewayBuildPollTimer) gatewayBuildPollTimer = setInterval(pollGatewayBuildStatus, 1500);
  } else if (status.state === "success") {
    badge.textContent = "Completata ✓";
    badge.className = "status-badge status-ok";
    clearInterval(gatewayBuildPollTimer);
    await loadFlashFirmwareList();
  } else if (status.state === "error") {
    badge.textContent = "Errore";
    badge.className = "status-badge status-error";
    clearInterval(gatewayBuildPollTimer);
  }
}

// ---------- Test connessione WiFi via USB (Web Serial grezzo, protocollo del loader) ----------

let wifiTestPort = null;
let wifiTestReader = null;
let wifiTestWriter = null;
let wifiTestReadClosed = null;  // Promise del pipe di lettura
let wifiTestWriteClosed = null; // Promise del pipe di scrittura
let wifiTestSelectedSsid = null;

async function connectWifiTestSerial() {
  if (wifiTestPort) {
    return; // già connesso: evita di riaprire la stessa porta (causa l'errore "already open")
  }
  if (!("serial" in navigator)) {
    document.getElementById("serial-unsupported").style.display = "block";
    return;
  }
  const connectBtn = document.getElementById("connect-serial-btn");
  try {
    const port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
    wifiTestPort = port; // assegnato solo dopo un open riuscito

    const textDecoder = new TextDecoderStream();
    // .catch() evita "uncaught in promise" quando la porta si chiude: è normale.
    wifiTestReadClosed = wifiTestPort.readable.pipeTo(textDecoder.writable).catch(() => {});
    wifiTestReader = textDecoder.readable.getReader();

    const textEncoder = new TextEncoderStream();
    wifiTestWriteClosed = textEncoder.readable.pipeTo(wifiTestPort.writable).catch(() => {});
    wifiTestWriter = textEncoder.writable.getWriter();
    // Configurazione e microSD (devconfig.js) usano lo stesso collegamento.
    if (window.DhTools) window.DhTools.attachSerial("loader", sendWifiTestCommand);

    const badge = document.getElementById("serial-status-badge");
    badge.textContent = "Connesso";
    badge.className = "status-badge status-ok";
    document.getElementById("wifi-test-controls").style.display = "block";
    connectBtn.disabled = true; // un solo collegamento alla volta: evita nuovi tentativi di open
    connectBtn.style.display = "none";
    document.getElementById("disconnect-serial-btn").style.display = "inline-block";

    readWifiTestLoop(); // non await: gira in background finché la porta resta aperta
    // Il loader con schermo risponde con le sue capacità (features: wifi/bt/vpn);
    // il loader leggero ignora il comando e la sezione VPN resta nascosta.
    setTimeout(() => sendWifiTestCommand({ cmd: "hello" }), 300);
  } catch (err) {
    wifiTestPort = null;
    alert(`Impossibile connettersi: ${err.message}`);
  }
}

async function disconnectWifiTestSerial() {
  // Chiusura ORDINATA di Web Serial. L'ordine conta: prima si sganciano reader e
  // writer dai pipe (TextDecoder/TextEncoder), si ASPETTA che i pipe finiscano,
  // e solo alla fine si chiude la porta. Saltare l'attesa dei pipe lascia il lock
  // sulla porta: è la causa del "porta occupata" alla riconnessione.
  try {
    if (wifiTestReader) {
      await wifiTestReader.cancel().catch(() => {});
      wifiTestReader.releaseLock();
    }
  } catch (_) {}
  try {
    if (wifiTestWriter) {
      await wifiTestWriter.abort().catch(() => {});
      wifiTestWriter.releaseLock();
    }
  } catch (_) {}
  // Aspetta che entrambi i pipe si chiudano davvero, PRIMA di chiudere la porta.
  try { await Promise.all([wifiTestReadClosed, wifiTestWriteClosed]); } catch (_) {}
  try {
    if (wifiTestPort) await wifiTestPort.close();
  } catch (_) {}

  wifiTestPort = null;
  wifiTestReader = null;
  wifiTestWriter = null;
  wifiTestReadClosed = null;
  wifiTestWriteClosed = null;
  if (window.DhTools) window.DhTools.detachSerial("loader");

  const badge = document.getElementById("serial-status-badge");
  badge.textContent = "Non connesso";
  badge.className = "status-badge status-unknown";
  document.getElementById("wifi-test-controls").style.display = "none";
  document.getElementById("connect-serial-btn").disabled = false;
  document.getElementById("connect-serial-btn").style.display = "inline-block";
  document.getElementById("disconnect-serial-btn").style.display = "none";
  setLoaderVpnAvailable(false);
}

async function sendWifiTestCommand(obj) {
  if (!wifiTestWriter) return;
  await wifiTestWriter.write(JSON.stringify(obj) + "\n");
}

async function readWifiTestLoop() {
  let buffer = "";
  try {
    while (true) {
      const { value, done } = await wifiTestReader.read();
      if (done) break;
      buffer += value;
      let newlineIndex;
      while ((newlineIndex = buffer.indexOf("\n")) >= 0) {
        const line = buffer.slice(0, newlineIndex).trim();
        buffer = buffer.slice(newlineIndex + 1);
        if (line) handleWifiTestMessage(line);
      }
    }
  } catch (err) {
    const badge = document.getElementById("serial-status-badge");
    badge.textContent = "Disconnesso";
    badge.className = "status-badge status-error";
    wifiTestPort = null;
    wifiTestReader = null;
    wifiTestWriter = null;
    if (window.DhTools) window.DhTools.detachSerial("loader");
    setLoaderVpnAvailable(false);
    document.getElementById("connect-serial-btn").disabled = false; // permette di riconnettersi
    document.getElementById("connect-serial-btn").style.display = "inline-block";
    document.getElementById("disconnect-serial-btn").style.display = "none";
  }
}

function handleWifiTestMessage(line) {
  let msg;
  try {
    msg = JSON.parse(line);
  } catch {
    return; // riga non-JSON (log di debug del firmware): ignorata
  }
  // Risposte ai comandi config_* / storage_* / file_* (pannello Configurazione).
  if (window.DhTools && window.DhTools.onSerialMessage("loader", msg)) return;

  if (msg.type === "scan_result") {
    renderWifiNetworkList(msg.networks || []);
    document.getElementById("wifi-scan-feedback").textContent =
      `${(msg.networks || []).length} rete/i trovate.`;
  } else if (msg.type === "connect_result") {
    const feedback = document.getElementById("wifi-connect-feedback");
    if (msg.success) {
      feedback.textContent = `Connesso! IP: ${msg.ip}. SSID e password copiati qui sotto ` +
        `nella sezione "Genera firmware": generalo pure, li userà al primo boot.`;
      feedback.style.color = "var(--ok)";
      // Autocompila i campi WiFi del builder: evita di dover ricordare/ridigitare
      // la password appena verificata, ed elimina la dipendenza dalla NVS condivisa
      // tra loader e firmware definitivo.
      document.getElementById("build-wifi-ssid-input").value = wifiTestSelectedSsid || "";
      document.getElementById("build-wifi-password-input").value =
        document.getElementById("wifi-test-password").value;
    } else {
      feedback.textContent = `Fallito: ${msg.error || "errore sconosciuto"}`;
      feedback.style.color = "var(--danger)";
    }
    if (loaderHasVpn) sendWifiTestCommand({ cmd: "status" });
  } else if (msg.type === "loader_hello") {
    setLoaderVpnAvailable(Array.isArray(msg.features) && msg.features.includes("vpn"));
  } else if (msg.type === "status") {
    renderLoaderStatus(msg);
  } else if (msg.type === "vpn_config_result") {
    const fb = document.getElementById("loader-vpn-feedback");
    fb.textContent = msg.ok ? "Impostazioni VPN inviate. La connessione richiede qualche secondo."
                            : `Rifiutate dal display: ${msg.error || "errore sconosciuto"}`;
    fb.style.color = msg.ok ? "var(--ok)" : "var(--danger)";
    sendWifiTestCommand({ cmd: "status" });
  } else if (msg.type === "vpn_forget_result") {
    const fb = document.getElementById("loader-vpn-feedback");
    fb.textContent = "Identità dimenticata: al prossimo collegamento il display sarà un nuovo " +
                     "dispositivo sulla tailnet (rimuovi quello vecchio dalla console Tailscale).";
    fb.style.color = "";
  }
}

// ---------- VPN tramite loader con schermo ----------

let loaderHasVpn = false;
let loaderStatusTimer = null;

function setLoaderVpnAvailable(available) {
  loaderHasVpn = available;
  document.getElementById("loader-vpn-controls").style.display = available ? "block" : "none";
  if (loaderStatusTimer) {
    clearInterval(loaderStatusTimer);
    loaderStatusTimer = null;
  }
  if (available) {
    sendWifiTestCommand({ cmd: "status" });
    // Aggiornamento periodico finché la scheda è visibile: la VPN impiega qualche
    // secondo a registrarsi, così lo si vede arrivare senza premere nulla.
    loaderStatusTimer = setInterval(() => {
      if (!document.hidden && wifiTestWriter) sendWifiTestCommand({ cmd: "status" });
    }, 4000);
  }
}

// true se l'IPv4 "ip" sta nella rete "a.b.c.d/len"
function ipInCidr(ip, cidr) {
  const toInt = s => {
    const p = String(s).split(".").map(Number);
    if (p.length !== 4 || p.some(n => !Number.isInteger(n) || n < 0 || n > 255)) return null;
    return ((p[0] << 24) >>> 0) + (p[1] << 16) + (p[2] << 8) + p[3];
  };
  const [net, lenStr] = String(cidr).split("/");
  const a = toInt(ip), n = toInt(net), len = Number(lenStr);
  if (a === null || n === null || !(len >= 0 && len <= 32)) return false;
  const mask = len === 0 ? 0 : (0xFFFFFFFF << (32 - len)) >>> 0;
  return ((a & mask) >>> 0) === ((n & mask) >>> 0);
}

function statusCard(title, lines) {
  const card = document.createElement("div");
  card.className = "status-card";
  const t = document.createElement("strong");
  t.textContent = title;
  card.appendChild(t);
  lines.filter(Boolean).forEach(line => {
    const span = document.createElement("span");
    span.textContent = line;
    card.appendChild(span);
  });
  return card;
}

function renderLoaderStatus(msg) {
  const box = document.getElementById("loader-status");
  box.innerHTML = "";
  const w = msg.wifi || {}, b = msg.bt || {}, v = msg.vpn || {};
  box.appendChild(statusCard("WiFi", [
    !w.enabled ? "Disattivato" : w.connected ? `Connesso a ${w.ssid}` : "Non connesso",
    w.connected ? `IP ${w.ip} · ${w.rssi} dBm` : "",
  ]));
  box.appendChild(statusCard("Bluetooth", [b.enabled ? "Attivo" : "Disattivato", b.mac ? `MAC ${b.mac}` : ""]));
  box.appendChild(statusCard("VPN", [
    v.enabled ? v.state : "Disattivata",
    v.error ? `Errore: ${v.error}` : "",
    v.ip ? `IP ${v.ip}` : "",
    v.hostname ? `Nome: ${v.hostname}` : "",
    v.key ? `Chiave: ${v.key}` : "",
  ]));

  // Subnet pubblicate dai subnet router: raggiungibili dal display anche fuori casa.
  const routes = v.routes || [];
  box.appendChild(statusCard("Reti raggiungibili via VPN", routes.length
    ? routes.map(r => `${r.network} tramite ${r.via}${r.online ? "" : " (offline)"}`)
    : [v.enabled ? "nessuna subnet pubblicata" : "VPN spenta"]));
  const serverHost = document.getElementById("build-server-host-input").value.trim();
  const covering = routes.find(r => ipInCidr(serverHost, r.network));
  if (covering) {
    box.appendChild(statusCard("Server del form di build", [
      `${serverHost} è in ${covering.network}: fuori casa sarà raggiunto tramite ${covering.via}.`,
    ]));
  }

  const hostInput = document.getElementById("loader-vpn-hostname-input");
  if (!hostInput.value && v.hostname) hostInput.placeholder = v.hostname;

  const list = document.getElementById("loader-vpn-peers");
  list.innerHTML = "";
  (v.peers || []).forEach(p => {
    const li = document.createElement("li");
    li.className = "page-item";
    li.style.cursor = "default";
    const name = document.createElement("span");
    name.className = p.online ? "peer-online" : "peer-offline";
    name.textContent = `${p.online ? "●" : "○"} ${p.hostname}  ${p.ip}` +
                       (p.online ? (p.direct ? "  (diretto)" : "  (via relay)") : "");
    const use = document.createElement("button");
    use.className = "btn-secondary";
    use.textContent = "Usa come server VPN";
    use.title = "Copia questo indirizzo nel campo \"Indirizzo server sulla tailnet\" del form di build";
    use.addEventListener("click", () => {
      document.getElementById("build-comm-mode-select").value = "wifi_vpn";
      updateCommModeFields();
      document.getElementById("build-server-host-vpn-input").value = p.ip;
      use.textContent = "Copiato ✓";
    });
    li.appendChild(name);
    li.appendChild(use);
    list.appendChild(li);
  });
}

async function sendLoaderVpnConfig() {
  const cmd = { cmd: "vpn_config", enabled: document.getElementById("loader-vpn-enabled-input").checked };
  const key = document.getElementById("loader-vpn-key-input").value.trim();
  const hostname = document.getElementById("loader-vpn-hostname-input").value.trim();
  if (key) cmd.auth_key = key;
  if (hostname) cmd.hostname = hostname;
  const fb = document.getElementById("loader-vpn-feedback");
  fb.textContent = "Invio in corso…";
  fb.style.color = "";
  await sendWifiTestCommand(cmd);
}

function copyLoaderVpnToBuild() {
  document.getElementById("build-comm-mode-select").value = "wifi_vpn";
  updateCommModeFields();
  const hostname = document.getElementById("loader-vpn-hostname-input").value.trim();
  if (hostname) document.getElementById("build-vpn-hostname-input").value = hostname;
  // La chiave NON viene copiata di proposito: resta salvata sul display (NVS) e
  // il firmware "Completo" la ritrova; incorporarla nel .bin la esporrebbe.
  const fb = document.getElementById("loader-vpn-feedback");
  fb.textContent = "Modalità Completo selezionata nel form di build" +
    (hostname ? " e nome copiato." : ".") +
    " La chiave resta salvata sul display: non serve incorporarla nel firmware (se il flash " +
    "cancellasse la memoria, reinseriscila dalle impostazioni sul display).";
  fb.style.color = "";
}

async function forgetLoaderVpnIdentity() {
  if (!confirm("Il display si registrerà come NUOVO dispositivo sulla tailnet. Continuare?")) return;
  await sendWifiTestCommand({ cmd: "vpn_forget" });
}

function renderWifiNetworkList(networks) {
  const list = document.getElementById("wifi-network-list");
  list.innerHTML = "";
  networks
    .slice()
    .sort((a, b) => b.rssi - a.rssi)
    .forEach(n => {
      const li = document.createElement("li");
      li.className = "page-item";
      li.style.cursor = "pointer";
      const lock = n.secure ? "🔒" : "🔓";
      // textContent, non innerHTML: l'SSID è scelto da chiunque abbia un access
      // point nei paraggi e non va interpretato come HTML.
      const name = document.createElement("span");
      name.textContent = `${lock} ${n.ssid}`;
      const meta = document.createElement("span");
      meta.className = "device-item-meta";
      meta.textContent = `${n.rssi} dBm`;
      li.appendChild(name);
      li.appendChild(meta);
      li.addEventListener("click", () => {
        wifiTestSelectedSsid = n.ssid;
        [...list.children].forEach(el => el.style.background = "");
        li.style.background = "var(--accent)";
      });
      list.appendChild(li);
    });
}

async function scanWifiTest() {
  document.getElementById("wifi-scan-feedback").textContent = "Scansione in corso…";
  await sendWifiTestCommand({ cmd: "scan" });
}

async function testWifiConnection() {
  if (!wifiTestSelectedSsid) {
    alert("Seleziona prima una rete dall'elenco");
    return;
  }
  const password = document.getElementById("wifi-test-password").value;
  document.getElementById("wifi-connect-feedback").textContent = "Tentativo di connessione…";
  document.getElementById("wifi-connect-feedback").style.color = "";
  await sendWifiTestCommand({ cmd: "connect", ssid: wifiTestSelectedSsid, password });
}

// ---------- Vai al pannello di flashing (dove risiede la console ESP Web Tools) ----------

// ---------- Ponte USB↔server (modalità USB) ----------
// A differenza del pannello WiFi test (che parla un protocollo di comando/
// scansione), qui il display invia hello/poll come farebbe via HTTP, e questa
// pagina fa da relay: riceve la richiesta via Web Serial, la inoltra al
// server con un fetch() sulla stessa origine di questa pagina (che potrebbe
// essere raggiunta via Tailscale da una rete diversa da quella del display),
// e rispedisce la risposta giù per il cavo. Porta separata da wifiTestPort:
// Web Serial non permette due connessioni sulla stessa porta.
let usbBridgePort = null;
let usbBridgeReader = null;
let usbBridgeWriter = null;
let usbBridgeReadClosed = null;
let usbBridgeWriteClosed = null;

// Il ponte USB non ha nessuno stato lato server (a differenza dei log di
// build): un refresh della pagina perde per forza la connessione Web Serial
// vera e propria (limite del browser, non aggirabile), ma almeno il TESTO del
// log non deve sparire nel nulla — lo teniamo in localStorage e lo ripristiniamo
// al prossimo caricamento, con una riga che chiarisce che va riconnesso.
const USB_BRIDGE_LOG_STORAGE_KEY = "displayhub_usb_bridge_log";

function usbBridgeLog(text) {
  const el = document.getElementById("usb-bridge-log");
  el.textContent += text + "\n";
  el.scrollTop = el.scrollHeight;
  try {
    localStorage.setItem(USB_BRIDGE_LOG_STORAGE_KEY, el.textContent);
  } catch (_) {
    // localStorage non disponibile (privacy mode, quota piena, ecc.): il log
    // resta comunque visibile per la sessione corrente, semplicemente non
    // sopravvive a un refresh — nessun problema bloccante.
  }
}

function restoreUsbBridgeLog() {
  let saved;
  try {
    saved = localStorage.getItem(USB_BRIDGE_LOG_STORAGE_KEY);
  } catch (_) {
    return;
  }
  if (!saved) return;
  const el = document.getElementById("usb-bridge-log");
  el.textContent = saved + "\n--- pagina ricaricata: ponte da riconnettere ---\n";
  el.scrollTop = el.scrollHeight;
}

async function connectUsbBridge() {
  if (usbBridgePort) return; // già connesso
  if (!("serial" in navigator)) {
    alert("Questo browser non supporta Web Serial. Usa Chrome o Edge.");
    return;
  }
  const connectBtn = document.getElementById("connect-usb-bridge-btn");
  try {
    const port = await navigator.serial.requestPort();
    await port.open({ baudRate: 115200 });
    usbBridgePort = port;

    const textDecoder = new TextDecoderStream();
    usbBridgeReadClosed = usbBridgePort.readable.pipeTo(textDecoder.writable).catch(() => {});
    usbBridgeReader = textDecoder.readable.getReader();

    const textEncoder = new TextEncoderStream();
    usbBridgeWriteClosed = textEncoder.readable.pipeTo(usbBridgePort.writable).catch(() => {});
    usbBridgeWriter = textEncoder.writable.getWriter();
    if (window.DhTools) {
      window.DhTools.attachSerial("bridge", (obj) => usbBridgeWriter && usbBridgeWriter.write(JSON.stringify(obj) + "\n"));
    }

    const badge = document.getElementById("usb-bridge-status-badge");
    badge.textContent = "Connesso";
    badge.className = "status-badge status-ok";
    connectBtn.disabled = true;
    connectBtn.style.display = "none";
    document.getElementById("disconnect-usb-bridge-btn").style.display = "inline-block";
    usbBridgeLog("Ponte connesso. In attesa di richieste dal display...");

    readUsbBridgeLoop(); // non await: gira in background finché la porta resta aperta
  } catch (err) {
    usbBridgePort = null;
    alert(`Impossibile connettersi: ${err.message}`);
  }
}

async function disconnectUsbBridge() {
  // Stessa chiusura ordinata del test WiFi: sgancia reader/writer, aspetta i pipe,
  // poi chiude la porta. Evita il lock residuo che occupa la porta.
  try {
    if (usbBridgeReader) {
      await usbBridgeReader.cancel().catch(() => {});
      usbBridgeReader.releaseLock();
    }
  } catch (_) {}
  try {
    if (usbBridgeWriter) {
      await usbBridgeWriter.abort().catch(() => {});
      usbBridgeWriter.releaseLock();
    }
  } catch (_) {}
  try { await Promise.all([usbBridgeReadClosed, usbBridgeWriteClosed]); } catch (_) {}
  try {
    if (usbBridgePort) await usbBridgePort.close();
  } catch (_) {}

  usbBridgePort = null;
  usbBridgeReader = null;
  usbBridgeWriter = null;
  usbBridgeReadClosed = null;
  usbBridgeWriteClosed = null;
  if (window.DhTools) window.DhTools.detachSerial("bridge");

  const badge = document.getElementById("usb-bridge-status-badge");
  badge.textContent = "Non connesso";
  badge.className = "status-badge status-unknown";
  document.getElementById("connect-usb-bridge-btn").disabled = false;
  document.getElementById("connect-usb-bridge-btn").style.display = "inline-block";
  document.getElementById("disconnect-usb-bridge-btn").style.display = "none";
  usbBridgeLog("Ponte disconnesso.");
}

async function readUsbBridgeLoop() {
  let buffer = "";
  try {
    while (true) {
      const { value, done } = await usbBridgeReader.read();
      if (done) break;
      buffer += value;
      let newlineIndex;
      while ((newlineIndex = buffer.indexOf("\n")) >= 0) {
        const line = buffer.slice(0, newlineIndex).trim();
        buffer = buffer.slice(newlineIndex + 1);
        if (line) handleUsbBridgeLine(line);
      }
    }
  } catch (err) {
    const badge = document.getElementById("usb-bridge-status-badge");
    badge.textContent = "Disconnesso";
    badge.className = "status-badge status-error";
    usbBridgePort = null;
    usbBridgeReader = null;
    usbBridgeWriter = null;
    if (window.DhTools) window.DhTools.detachSerial("bridge");
    document.getElementById("connect-usb-bridge-btn").disabled = false;
    document.getElementById("connect-usb-bridge-btn").style.display = "inline-block";
    document.getElementById("disconnect-usb-bridge-btn").style.display = "none";
    usbBridgeLog(`Connessione persa: ${err.message}`);
  }
}

async function handleUsbBridgeLine(line) {
  let msg;
  try {
    msg = JSON.parse(line);
    if (window.DhTools && window.DhTools.onSerialMessage("bridge", msg)) return;
  } catch (_) {
    // Riga non-JSON: è debug output del firmware (Serial.println), non un
    // messaggio del protocollo hello/poll. A differenza del pannello WiFi test
    // (dove queste righe sono rumore da ignorare), qui è l'unico modo per
    // vedere i log del display: la stessa porta USB porta sia i dati che i
    // log, e mentre il ponte è connesso non c'è un altro modo di leggerli
    // senza disconnettere tutto e aprire un monitor seriale a parte.
    usbBridgeLog(line);
    return;
  }

  if (msg.type === "hello") {
    usbBridgeLog(`hello ricevuto da ${msg.device_id} (fw ${msg.fw_version}), inoltro al server...`);
    try {
      const res = await fetch("/api/esp/hello", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ device_id: msg.device_id, fw_version: msg.fw_version }),
      });
      usbBridgeLog(`hello -> server ha risposto ${res.status}`);
    } catch (err) {
      usbBridgeLog(`hello -> errore di rete verso il server: ${err.message}`);
    }
    return;
  }

  if (msg.cmd === "poll") {
    try {
      const fwParam = msg.fw_version ? `&fw_version=${encodeURIComponent(msg.fw_version)}` : "";
      const res = await fetch(`/api/esp/poll?device_id=${encodeURIComponent(msg.device_id)}${fwParam}`);
      const data = await res.json();
      await usbBridgeWriter.write(JSON.stringify(data) + "\n");
      usbBridgeLog(`poll -> config_version=${data.config_version}, pagine=${(data.pages || []).length}`);
    } catch (err) {
      usbBridgeLog(`poll -> errore verso il server: ${err.message} (nessuna risposta inviata al display)`);
    }
    return;
  }
}


function openFlashConsole() {
  const panel = document.getElementById("flash-panel");
  panel.open = true; // ora è un <details>: se era chiuso, scrollIntoView porterebbe a un pannello vuoto
  panel.scrollIntoView({ behavior: "smooth", block: "start" });
  // Breve evidenziazione visiva così è chiaro dove si è atterrati
  panel.style.transition = "background-color 0.3s";
  const originalBg = panel.style.backgroundColor;
  panel.style.backgroundColor = "var(--accent)";
  setTimeout(() => { panel.style.backgroundColor = originalBg; }, 600);
}

// ---------- Pulizia cache librerie (forza ridownload alla prossima build) ----------

async function clearDependencyCache(target, buttonEl) {
  const originalLabel = buttonEl.textContent;
  buttonEl.textContent = "Pulizia in corso...";
  buttonEl.disabled = true;
  try {
    const res = await fetch("/api/firmware/clear-cache", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ target }),
    });
    const data = await res.json();
    if (!res.ok) {
      alert(`Errore: ${data.error}`);
    } else {
      buttonEl.textContent = "Cache pulita ✓";
      setTimeout(() => { buttonEl.textContent = originalLabel; }, 2500);
    }
  } catch (err) {
    alert(`Errore di rete: ${err.message}`);
  } finally {
    buttonEl.disabled = false;
    if (buttonEl.textContent === "Pulizia in corso...") buttonEl.textContent = originalLabel;
  }
}

// ---------- Flashing firmware via browser (Web Serial / ESP Web Tools) ----------

async function loadFlashFirmwareList() {
  const res = await fetch("/api/firmware");
  const files = await res.json();
  renderFirmwareArchive(files);
  const select = document.getElementById("flash-firmware-select");
  select.innerHTML = "";
  if (files.length === 0) {
    select.innerHTML = '<option value="">Nessun firmware caricato</option>';
  } else {
    files.forEach(f => {
      const opt = document.createElement("option");
      opt.value = f.filename;
      const kindLabel = f.kind === "loader" ? "[loader] "
        : f.kind === "loader_full" ? "[loader BT] "
        : f.kind === "gateway" ? "[gateway BT] "
        : "[definitivo] ";
      opt.textContent = `${kindLabel}${f.filename} (${(f.size_bytes / 1024).toFixed(0)} KB)`;
      select.appendChild(opt);
    });
  }
  renderInstallButton();
}

// ---------- Archivio firmware: tabella con le opzioni di ogni versione ----------

const MODE_LABEL = { usb: "USB", wifi: "WiFi", wifi_vpn: "WiFi + VPN" };

function badge(text, cls = "") {
  const b = document.createElement("span");
  b.className = `badge ${cls}`;
  b.textContent = text;
  return b;
}

function cell(tr, children, cls) {
  const td = document.createElement("td");
  if (cls) td.className = cls;
  (Array.isArray(children) ? children : [children]).forEach(c => {
    if (c == null) return;
    td.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
  });
  tr.appendChild(td);
  return td;
}

function fmtDate(meta, f) {
  const ts = meta.built_at || (meta.date ? null : f.uploaded_at);
  if (ts) return new Date(ts * 1000).toLocaleString("it-IT", { dateStyle: "short", timeStyle: "short" });
  if (meta.date && meta.date.length === 8) return `${meta.date.slice(6, 8)}/${meta.date.slice(4, 6)}/${meta.date.slice(0, 4)}`;
  return new Date(f.uploaded_at * 1000).toLocaleDateString("it-IT");
}

function renderFirmwareArchive(files) {
  const tbody = document.querySelector("#firmware-table tbody");
  if (!tbody) return;
  tbody.innerHTML = "";
  if (!files.length) {
    const tr = document.createElement("tr");
    const td = cell(tr, "Nessun firmware: generane uno qui sopra o caricalo nel pannello di flashing.");
    td.colSpan = 12;
    td.className = "hint";
    tbody.appendChild(tr);
  }
  files.forEach(f => {
    const m = f.meta || {};
    const derived = m.source && m.source !== "build";
    const tr = document.createElement("tr");
    if (derived) tr.title = `Opzioni ricavate dal ${m.source}`;
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.className = "archive-select";
    cb.value = f.filename;
    cb.addEventListener("change", updateArchiveSelection);
    cell(tr, cb);
    cell(tr, f.filename, "fw-name");

    // Tipo e modalità
    const kind = [];
    if (m.target === "loader") kind.push(badge("Loader leggero"));
    else if (m.target === "loader_full") kind.push(badge("Loader con schermo"));
    else if (m.target === "gateway") { kind.push(badge("Gateway BT")); kind.push(badge("ESP32 classico", "badge-muted")); }
    else if (m.target === "main") kind.push(badge("Display", "badge-accent"));
    else kind.push(badge("Sconosciuto", "badge-muted"));
    if (m.comm_mode) kind.push(badge(MODE_LABEL[m.comm_mode] || m.comm_mode));
    if (m.config_storage_sd || m.config_sd) kind.push(badge("microSD", "badge-ok"));
    cell(tr, kind, derived ? "derived" : "");

    cell(tr, m.device_id || "—", derived ? "derived" : "");
    cell(tr, [m.fw_version || "—", m.fw_revision ? ` ${m.fw_revision}` : ""], derived ? "derived" : "");
    cell(tr, m.server_host ? `${m.server_host}${m.server_port ? ":" + m.server_port : ""}` : "—");

    const vpn = [];
    if (m.comm_mode === "wifi_vpn") {
      if (m.vpn_hostname) vpn.push(document.createTextNode(m.vpn_hostname + " "));
      if (m.server_host_vpn) vpn.push(badge(`server ${m.server_host_vpn}`));
      if (m.has_vpn_auth_key) vpn.push(badge("auth key", "badge-ok"));
      if (m.has_vpn_auth_key_ephemeral) vpn.push(badge("key effimera", "badge-ok"));
      if (m.vpn_default_enabled === false) vpn.push(badge("spenta all'avvio", "badge-muted"));
      if (!vpn.length) vpn.push(document.createTextNode(derived ? "sì" : "—"));
    } else {
      vpn.push(document.createTextNode("—"));
    }
    cell(tr, vpn);
    const wifi = [];
    if (m.wifi_ssid) wifi.push(document.createTextNode(m.wifi_ssid + " "));
    if (m.has_wifi_password) wifi.push(badge("password", "badge-ok"));
    cell(tr, wifi.length ? wifi : "—");
    cell(tr, fmtDate(m, f));
    cell(tr, `${(f.size_bytes / 1024 / 1024).toFixed(2)} MB`);
    const filesCell = [];
    if (f.has_elf) filesCell.push(badge("ELF", "badge-ok"));
    if (f.has_log) {
      const a = document.createElement("a");
      a.href = `/api/firmware/${encodeURIComponent(f.filename)}/log`;
      a.target = "_blank";
      a.textContent = "log";
      a.className = "badge";
      filesCell.push(a);
    }
    cell(tr, filesCell.length ? filesCell : "—");

    const actions = [];
    const flash = document.createElement("button");
    flash.className = "btn-secondary btn-small";
    flash.textContent = "Flash";
    flash.title = "Seleziona questo firmware nel pannello di flashing";
    flash.addEventListener("click", () => {
      const sel = document.getElementById("flash-firmware-select");
      sel.value = f.filename;
      sel.dispatchEvent(new Event("change"));
      document.getElementById("flash-panel").open = true;
      document.getElementById("flash-panel").scrollIntoView({ behavior: "smooth", block: "start" });
    });
    actions.push(flash);
    const del = document.createElement("button");
    del.className = "btn-remove";
    del.textContent = "✕";
    del.title = "Elimina firmware, ELF, log e dati della versione";
    del.addEventListener("click", () => deleteFirmwares([f.filename]));
    actions.push(del);
    cell(tr, actions, "actions");
    tbody.appendChild(tr);
  });
  const all = document.getElementById("archive-select-all");
  if (all) all.checked = false;
  updateArchiveSelection();
  refreshOrphanLogs();
}

function selectedFirmwares() {
  return [...document.querySelectorAll("#firmware-table .archive-select:checked")].map(cb => cb.value);
}

function updateArchiveSelection() {
  const n = selectedFirmwares().length;
  const btn = document.getElementById("archive-delete-selected");
  if (!btn) return;
  btn.disabled = n === 0;
  btn.textContent = n ? `Elimina selezionati (${n})` : "Elimina selezionati";
}

async function deleteFirmwares(names) {
  if (!names.length) return;
  const what = names.length === 1 ? `"${names[0]}"` : `${names.length} firmware`;
  if (!confirm(`Eliminare ${what}?\nVerranno cancellati anche ELF, log e dati della versione.`)) return;
  const res = await fetch("/api/firmware/delete-many", {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ filenames: names }),
  });
  const data = await res.json().catch(() => ({}));
  document.getElementById("archive-feedback").textContent = res.ok ? `Eliminati: ${data.deleted}` : "Eliminazione non riuscita";
  await loadFlashFirmwareList();
}

async function refreshOrphanLogs() {
  const btn = document.getElementById("archive-clean-logs");
  if (!btn) return;
  try {
    const logs = await (await fetch("/api/firmware/orphan-logs")).json();
    btn.disabled = logs.length === 0;
    btn.textContent = logs.length ? `Pulisci log delle build fallite (${logs.length})` : "Nessun log da pulire";
  } catch (e) { /* non bloccante */ }
}

async function cleanOrphanLogs() {
  const res = await fetch("/api/firmware/orphan-logs", { method: "DELETE" });
  const data = await res.json().catch(() => ({}));
  document.getElementById("archive-feedback").textContent = res.ok ? `Log cancellati: ${data.deleted}` : "Pulizia non riuscita";
  refreshOrphanLogs();
}

// ---------- Sezioni della pagina (#/, #/firmware, #/usb, #/configurazione) ----------
// Una sola pagina con sezioni: passando dall'una all'altra non si ricarica
// nulla, quindi una connessione USB aperta dal browser resta attiva.
const VIEWS = { "": "home", "firmware": "firmware", "usb": "usb", "configurazione": "config",
                "versioni": "versions", "documentazione": "docs" };

function showViewFromHash() {
  const key = (location.hash || "").replace(/^#\/?/, "").split("/")[0];
  const view = VIEWS[key] || "home";
  document.querySelectorAll("main > .view").forEach(v => v.classList.toggle("active", v.dataset.view === view));
  document.querySelectorAll("#main-nav a").forEach(a => a.classList.toggle("active", a.dataset.view === view));
  window.dispatchEvent(new CustomEvent("dh:view", { detail: view }));
  window.scrollTo(0, 0);
}
window.addEventListener("hashchange", showViewFromHash);
showViewFromHash();

async function uploadFirmware() {
  const input = document.getElementById("flash-firmware-upload");
  if (!input.files.length) return;
  const formData = new FormData();
  formData.append("file", input.files[0]);
  const res = await fetch("/api/firmware", { method: "POST", body: formData });
  if (res.ok) {
    input.value = "";
    await loadFlashFirmwareList();
  } else {
    const err = await res.json();
    alert(`Errore upload: ${err.error}`);
  }
}

function renderInstallButton() {
  const container = document.getElementById("install-button-container");
  const select = document.getElementById("flash-firmware-select");
  const filename = select.value;
  container.innerHTML = "";

  if (!filename) return;

  if (!("serial" in navigator)) {
    document.getElementById("webserial-unsupported").style.display = "block";
    return;
  }

  // ESP Web Tools gestisce da solo: richiesta permesso porta seriale,
  // scaricamento del .bin dal manifest, scrittura flash e progress bar.
  const button = document.createElement("esp-web-install-button");
  button.setAttribute("manifest", `/api/firmware/${filename}/manifest.json`);

  const activateSlot = document.createElement("button");
  activateSlot.slot = "activate";
  activateSlot.className = "btn-primary";
  activateSlot.textContent = "Flasha questo firmware";
  button.appendChild(activateSlot);

  const unsupportedSlot = document.createElement("span");
  unsupportedSlot.slot = "unsupported";
  unsupportedSlot.className = "hint";
  unsupportedSlot.textContent = "Browser non supportato: usa Chrome o Edge.";
  button.appendChild(unsupportedSlot);

  const notAllowedSlot = document.createElement("span");
  notAllowedSlot.slot = "not-allowed";
  notAllowedSlot.className = "hint";
  notAllowedSlot.textContent = "Permesso porta seriale negato.";
  button.appendChild(notAllowedSlot);

  container.appendChild(button);

  // Quale scheda collegare: il gateway va su un ESP32 CLASSICO, tutto il resto
  // sull'ESP32-S3 del display. Con la scheda sbagliata ESP Web Tools si ferma
  // con "Your ESP32-S3 board is not supported" (o il contrario).
  const hint = document.createElement("p");
  hint.className = "hint flash-board-hint";
  if (/_gateway(-[A-Za-z0-9_.-]+)?\.bin$/.test(filename)) {
    hint.innerHTML = "Collega l'<strong>ESP32 classico</strong> del gateway (modulo ESP32-WROOM-32 o WROVER), " +
      "<strong>non</strong> il display né un ESP32-S3: il gateway ha bisogno del Bluetooth Classic, che l'S3 non ha.";
  } else {
    hint.innerHTML = "Collega il <strong>display</strong> (ESP32-S3).";
  }
  container.appendChild(hint);
}

function renderCatalog() {
  const container = document.getElementById("widget-catalog");
  container.innerHTML = "";
  widgetCatalog.forEach(w => {
    const card = document.createElement("div");
    card.className = "widget-card";
    card.innerHTML = `
      <h3>${w.name}</h3>
      <p>${w.description}</p>
      <button class="btn-secondary" data-widget-id="${w.id}">+ Aggiungi</button>
    `;
    card.querySelector("button").addEventListener("click", () => openConfigModal(w.id));
    container.appendChild(card);
  });
}

// Finestra dei parametri: aggiunta di una pagina nuova, oppure modifica di una
// pagina già in elenco (index): stessi campi, valori attuali precompilati.
function openConfigModal(widgetId, index = null) {
  const widget = widgetCatalog.find(w => w.id === widgetId);
  pendingWidgetId = widgetId;
  editingIndex = index;
  const current = index !== null ? (activePages[index].params || {}) : {};
  document.getElementById("config-modal-title").textContent =
    `${index !== null ? "Modifica" : "Aggiungi"}: ${widget ? widget.name : widgetId}`;
  document.getElementById("config-modal-confirm").textContent = index !== null ? "Salva modifiche" : "Aggiungi";
  const fieldsContainer = document.getElementById("config-modal-fields");
  fieldsContainer.innerHTML = "";

  Object.entries((widget && widget.config_schema) || {}).forEach(([key, schema]) => {
    const value = key in current ? current[key] : schema.default;
    const label = document.createElement("label");
    label.textContent = schema.label;
    let input;
    if (schema.type === "select") {
      input = document.createElement("select");
      schema.options.forEach(opt => {
        const o = document.createElement("option");
        o.value = opt; o.textContent = opt;
        if (opt === value) o.selected = true;
        input.appendChild(o);
      });
    } else if (schema.type === "multiselect") {
      // Caselle di spunta; il valore è l'elenco delle opzioni scelte.
      input = document.createElement("div");
      input.className = "multiselect";
      input.dataset.type = "multiselect";
      const chosen = Array.isArray(value) ? value : String(value || "").split(",").map(v => v.trim());
      schema.options.forEach(opt => {
        const row = document.createElement("label");
        row.className = "multiselect-option";
        const cb = document.createElement("input");
        cb.type = "checkbox";
        cb.value = opt;
        cb.checked = chosen.includes(opt);
        row.appendChild(cb);
        row.appendChild(document.createTextNode(" " + ((schema.labels && schema.labels[opt]) || opt)));
        input.appendChild(row);
      });
    } else if (schema.type === "textarea") {
      input = document.createElement("textarea");
      input.value = value || "";
    } else {
      input = document.createElement("input");
      input.type = "text";
      input.value = value || "";
    }
    input.dataset.paramKey = key;
    label.appendChild(input);
    fieldsContainer.appendChild(label);
  });

  document.getElementById("config-modal").classList.remove("hidden");
}

function closeConfigModal() {
  document.getElementById("config-modal").classList.add("hidden");
  pendingWidgetId = null;
  editingIndex = null;
}

function confirmAddWidget() {
  const fieldsContainer = document.getElementById("config-modal-fields");
  const params = {};
  fieldsContainer.querySelectorAll("[data-param-key]").forEach(input => {
    if (input.dataset.type === "multiselect") {
      params[input.dataset.paramKey] = [...input.querySelectorAll("input[type=checkbox]:checked")].map(cb => cb.value);
    } else {
      params[input.dataset.paramKey] = input.value;
    }
  });
  if (editingIndex !== null) {
    activePages[editingIndex].params = params;   // page_id invariato: stessa pagina
  } else {
    activePages.push({ widget_id: pendingWidgetId, params });
  }
  renderPageList();
  markPagesDirty();
  closeConfigModal();
}

// Riepilogo dei parametri di una pagina, per distinguere due pagine dello
// stesso widget (es. due "Prezzo Crypto").
function pageSummary(page) {
  const parts = Object.values(page.params || {})
    .map(v => Array.isArray(v) ? v.join(", ") : String(v))
    .filter(v => v && v.length);
  const text = parts.join(" · ");
  return text.length > 70 ? text.slice(0, 67) + "..." : text;
}

function markPagesDirty() {
  const feedback = document.getElementById("save-feedback");
  feedback.style.color = "var(--warning, #F5C542)";
  feedback.textContent = "Modifiche non ancora inviate al display";
}

function movePage(index, delta) {
  const to = index + delta;
  if (to < 0 || to >= activePages.length) return;
  [activePages[index], activePages[to]] = [activePages[to], activePages[index]];
  renderPageList();
  markPagesDirty();
}

function renderPageList() {
  const list = document.getElementById("page-list");
  list.innerHTML = "";
  activePages.forEach((page, index) => {
    const widget = widgetCatalog.find(w => w.id === page.widget_id);
    const li = document.createElement("li");
    li.className = "page-item";
    li.draggable = true;
    li.dataset.index = index;

    const text = document.createElement("span");
    text.className = "page-item-text";
    text.textContent = `${index + 1}. ${widget ? widget.name : page.widget_id}`;
    const summary = pageSummary(page);
    if (summary) {
      const meta = document.createElement("span");
      meta.className = "device-item-meta";
      meta.textContent = summary;   // testo, non HTML: i parametri li scrive l'utente
      text.appendChild(document.createElement("br"));
      text.appendChild(meta);
    }
    li.appendChild(text);

    const actions = document.createElement("span");
    actions.className = "page-item-actions";
    const mk = (label, title, cls, fn, disabled) => {
      const b = document.createElement("button");
      b.className = cls;
      b.textContent = label;
      b.title = title;
      b.disabled = !!disabled;
      b.addEventListener("click", fn);
      actions.appendChild(b);
    };
    mk("▲", "Sposta su", "btn-move", () => movePage(index, -1), index === 0);
    mk("▼", "Sposta giù", "btn-move", () => movePage(index, 1), index === activePages.length - 1);
    const hasParams = widget && Object.keys(widget.config_schema || {}).length > 0;
    mk("✎", "Modifica i parametri", "btn-move", () => openConfigModal(page.widget_id, index), !hasParams);
    mk("✕", "Rimuovi", "btn-remove", () => { activePages.splice(index, 1); renderPageList(); markPagesDirty(); });
    li.appendChild(actions);

    attachDragHandlers(li);
    list.appendChild(li);
  });
}

// Drag & drop semplice per riordinare le pagine (nessuna libreria esterna necessaria)
function attachDragHandlers(li) {
  li.addEventListener("dragstart", () => li.classList.add("dragging"));
  li.addEventListener("dragend", () => {
    li.classList.remove("dragging");
    // Ricostruisce activePages nell'ordine visuale corrente
    const newOrder = [...document.querySelectorAll("#page-list .page-item")].map(el => activePages[parseInt(el.dataset.index, 10)]);
    const moved = newOrder.some((p, i) => p !== activePages[i]);
    activePages = newOrder;
    renderPageList();
    if (moved) markPagesDirty();
  });
  li.addEventListener("dragover", (e) => {
    e.preventDefault();
    const list = document.getElementById("page-list");
    const dragging = list.querySelector(".dragging");
    if (!dragging) return;
    const after = [...list.querySelectorAll(".page-item:not(.dragging)")].find(
      el => e.clientY <= el.getBoundingClientRect().top + el.getBoundingClientRect().height / 2
    );
    if (after) list.insertBefore(dragging, after);
    else list.appendChild(dragging);
  });
}

async function savePages() {
  const res = await fetch("/api/pages", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(activePages),
  });
  const feedback = document.getElementById("save-feedback");
  if (res.ok) {
    const data = await res.json();
    if (Array.isArray(data.pages)) activePages = data.pages;   // con i page_id assegnati dal server
    renderPageList();
    feedback.style.color = "";
    feedback.textContent = "Inviato al display ✓";
    setTimeout(() => (feedback.textContent = ""), 3000);
  } else {
    const err = await res.json();
    feedback.textContent = `Errore: ${err.error}`;
    feedback.style.color = "var(--danger)";
  }
}

document.getElementById("save-btn").addEventListener("click", savePages);
document.getElementById("config-modal-cancel").addEventListener("click", closeConfigModal);
document.getElementById("config-modal-confirm").addEventListener("click", confirmAddWidget);
document.getElementById("start-build-btn").addEventListener("click", startBuild);
document.getElementById("build-loader-btn").addEventListener("click", startLoaderBuild);
document.getElementById("build-loader-full-btn").addEventListener("click", startLoaderFullBuild);
document.getElementById("build-gateway-btn").addEventListener("click", startGatewayBuild);
document.getElementById("copy-gateway-build-log-btn").addEventListener("click", (e) => copyLogToClipboard("gateway-build-log", e.target));
document.getElementById("clear-cache-gateway-btn").addEventListener("click", (e) => clearDependencyCache("gateway", e.target));
document.getElementById("clear-gateway-build-log-btn").addEventListener("click", () => clearLogDisplay("gateway-build-log"));
document.getElementById("copy-build-log-btn").addEventListener("click", (e) => copyLogToClipboard("build-log", e.target));
document.getElementById("copy-loader-build-log-btn").addEventListener("click", (e) => copyLogToClipboard("loader-build-log", e.target));
document.getElementById("copy-loader-full-build-log-btn").addEventListener("click", (e) => copyLogToClipboard("loader-full-build-log", e.target));
document.getElementById("clear-cache-main-btn").addEventListener("click", (e) => clearDependencyCache("main", e.target));
document.getElementById("clear-cache-loader-btn").addEventListener("click", (e) => clearDependencyCache("loader", e.target));
document.getElementById("clear-cache-loader-full-btn").addEventListener("click", (e) => clearDependencyCache("loader_full", e.target));
document.getElementById("clear-build-log-btn").addEventListener("click", () => clearLogDisplay("build-log"));
document.getElementById("clear-loader-build-log-btn").addEventListener("click", () => clearLogDisplay("loader-build-log"));
document.getElementById("clear-loader-full-build-log-btn").addEventListener("click", () => clearLogDisplay("loader-full-build-log"));
document.getElementById("clear-usb-bridge-log-btn").addEventListener("click", () => clearLogDisplay("usb-bridge-log"));
document.getElementById("connect-serial-btn").addEventListener("click", connectWifiTestSerial);
document.getElementById("disconnect-serial-btn").addEventListener("click", disconnectWifiTestSerial);
document.getElementById("open-flash-console-btn").addEventListener("click", openFlashConsole);
document.getElementById("build-comm-mode-select").addEventListener("change", updateCommModeFields);
document.getElementById("build-config-sd-input").addEventListener("change", updateCommModeFields);
document.getElementById("connect-usb-bridge-btn").addEventListener("click", connectUsbBridge);
document.getElementById("disconnect-usb-bridge-btn").addEventListener("click", disconnectUsbBridge);
updateCommModeFields(); // stato iniziale coerente col valore di default del select
document.getElementById("scan-wifi-btn").addEventListener("click", scanWifiTest);
document.getElementById("test-connect-btn").addEventListener("click", testWifiConnection);
document.getElementById("loader-vpn-send-btn").addEventListener("click", sendLoaderVpnConfig);
document.getElementById("loader-status-btn").addEventListener("click", () => sendWifiTestCommand({ cmd: "status" }));
document.getElementById("loader-vpn-copy-btn").addEventListener("click", copyLoaderVpnToBuild);
document.getElementById("loader-vpn-forget-btn").addEventListener("click", forgetLoaderVpnIdentity);
document.getElementById("flash-firmware-upload").addEventListener("change", uploadFirmware);
document.getElementById("flash-firmware-select").addEventListener("change", renderInstallButton);
document.getElementById("archive-delete-selected").addEventListener("click", () => deleteFirmwares(selectedFirmwares()));
document.getElementById("archive-clean-logs").addEventListener("click", cleanOrphanLogs);
document.getElementById("archive-select-all").addEventListener("change", e => {
  document.querySelectorAll("#firmware-table .archive-select").forEach(cb => (cb.checked = e.target.checked));
  updateArchiveSelection();
});

loadCatalog().then(loadPages);
loadDevices();
loadFlashFirmwareList();
pollBuildStatus();
pollLoaderBuildStatus();
pollLoaderFullBuildStatus();
pollGatewayBuildStatus();
restoreUsbBridgeLog();
setInterval(loadDevices, 5000);

// ---------- TV Hisense VIDAA (comandata dal server) ----------
function tvStatusText(st) {
  const parts = [];
  parts.push(st.connected ? "Connessa" : "Non connessa");
  parts.push(st.paired ? "abbinata" : "non abbinata");
  if (st.client_cert) parts.push("certificato client presente");
  if (st.state && st.state.sourcename) parts.push(`sorgente: ${st.state.sourcename}`);
  if (st.last_error) parts.push(`ultimo errore: ${st.last_error}`);
  return parts.join(" · ");
}

async function tvCall(url, body) {
  const status = document.getElementById("tv-status");
  try {
    const res = await fetch(url, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
    const data = await res.json();
    if (data.status) status.textContent = tvStatusText(data.status);
    if (!data.ok) status.textContent = `Errore: ${data.error}` + (data.status ? ` — ${tvStatusText(data.status)}` : "");
    return data;
  } catch (e) {
    status.textContent = "Richiesta non riuscita";
    return { ok: false };
  }
}

async function loadTv() {
  const st = await (await fetch("/api/tv")).json();
  document.getElementById("tv-ip").value = st.ip || "";
  document.getElementById("tv-mac").value = st.mac || "";
  document.getElementById("tv-status").textContent = st.ip ? tvStatusText(st) : "Imposta IP e MAC della TV e salva.";
}

document.getElementById("tv-save").addEventListener("click", async () => {
  await fetch("/api/tv", { method: "PUT", headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ip: document.getElementById("tv-ip").value, mac: document.getElementById("tv-mac").value }) });
  loadTv();
});
document.getElementById("tv-connect").addEventListener("click", () => tvCall("/api/tv/connect"));
document.getElementById("tv-diagnose").addEventListener("click", async () => {
  const box = document.getElementById("tv-diagnosis");
  box.style.display = "block";
  box.textContent = "Provo le combinazioni con la TV (circa 20 secondi)...";
  const r = await tvCall("/api/tv/diagnose");
  box.textContent = r.ok ? r.result.map(x => `${x.prova}: ${x.risultato}`).join("\n") : `Errore: ${r.error}`;
});
document.getElementById("tv-pair-start").addEventListener("click", async () => {
  const r = await tvCall("/api/tv/pair/start");
  if (r.ok) document.getElementById("tv-status").textContent =
    `La TV ha accettato la richiesta (versione app ${r.result && r.result.app_version}): inserisci qui il codice che mostra, poi Conferma.`;
});
document.getElementById("tv-pair-confirm").addEventListener("click", async () => {
  const r = await tvCall("/api/tv/pair/confirm", { code: document.getElementById("tv-pair-code").value });
  if (r.ok) document.getElementById("tv-status").textContent = r.result ? "Abbinata ✓" : "Codice non accettato dalla TV (o nessuna risposta): riprova Abbina.";
});
document.querySelectorAll("#tv-remote-main button[data-key], #tv-remote-main button[data-power]").forEach(b =>
  b.addEventListener("click", () => {
    if (b.dataset.power === "toggle") tvCall("/api/tv/power", { action: "toggle" });
    else if (b.dataset.key) tvCall("/api/tv/key", { key: b.dataset.key });
  }));

// ---- Tastierino numerico canale ----
(function () {
  const main = document.getElementById("tv-remote-main");
  const pad = document.getElementById("tv-keypad");
  const numDisp = document.getElementById("tv-keypad-num");
  if (!pad) return;
  let buffer = "";

  function showPad(show) {
    pad.classList.toggle("hidden", !show);
    main.classList.toggle("hidden", show);
    if (show) { buffer = ""; numDisp.textContent = "—"; }
  }
  function refresh() { numDisp.textContent = buffer || "—"; }

  document.getElementById("tv-open-keypad").addEventListener("click", () => showPad(true));
  document.getElementById("tv-close-keypad").addEventListener("click", () => showPad(false));
  document.getElementById("tv-keypad-clear").addEventListener("click", () => { buffer = ""; refresh(); });

  pad.querySelectorAll("button[data-num]").forEach(b =>
    b.addEventListener("click", () => {
      if (buffer.length < 4) { buffer += b.dataset.num; refresh(); }
    }));

  // OK: invia le cifre una a una, poi conferma con KEY_OK
  document.getElementById("tv-keypad-enter").addEventListener("click", async () => {
    if (!buffer) return;
    for (const d of buffer) {
      await tvCall("/api/tv/key", { key: "KEY_" + d });
      await new Promise(r => setTimeout(r, 250)); // la TV ha bisogno di una pausa tra le cifre
    }
    await tvCall("/api/tv/key", { key: "KEY_OK" });
    buffer = ""; refresh();
  });
})();

loadTv();
