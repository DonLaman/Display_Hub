/*
 * Configurazione display e microSD (pannello "Configurazione display e microSD").
 *
 * - IniDoc: stesso formato e stesse regole di firmware/lib/dh_config/src/DhIni.cpp
 *   e app/core/ini_doc.py (commenti e righe sconosciute conservati; '#' e ';'
 *   dentro i valori restano; chiavi ripetute = elenchi).
 * - Origini: copia sul server (API /api/devices/<id>/config), display collegato
 *   via USB (comandi config_* del loader o del firmware in modalità USB tramite
 *   il ponte, vedi PROTOCOL.md) oppure file nuovo da scaricare.
 * - microSD via USB: stato, scelta all'inserimento, file (elenco, scarica,
 *   carica a blocchi, cancella, nuova cartella).
 *
 * app.js passa qui le connessioni seriali (attachSerial/detachSerial) e i
 * messaggi ricevuti (onSerialMessage): tutto il testo arriva da dispositivi o
 * dal server e viene inserito SOLO come textContent.
 */
(function () {
  "use strict";

  // =================================================================== INI
  const LIST_KEYS = new Set(["wifi.network", "bt.device", "bt.bond"]);
  const listId = (e) => String(e).split("|")[0].trim().toLowerCase();

  function parseValue(v) {
    v = v.trim();
    if (v.length >= 2 && v[0] === '"' && v[v.length - 1] === '"') {
      let out = "";
      const inner = v.slice(1, -1);
      for (let i = 0; i < inner.length; i++) {
        if (inner[i] === "\\" && i + 1 < inner.length && (inner[i + 1] === '"' || inner[i + 1] === "\\")) {
          out += inner[++i];
        } else {
          out += inner[i];
        }
      }
      return out;
    }
    return v;
  }

  function formatValue(v) {
    if (v && (/^\s/.test(v) || /\s$/.test(v) || v[0] === '"')) {
      return '"' + v.replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '"';
    }
    return v;
  }

  class IniDoc {
    constructor(text) {
      this.parse(text || "");
    }

    parse(text) {
      this.secs = [{ name: "", header: "", lines: [] }];
      const lines = text.split("\n");
      if (lines.length && lines[lines.length - 1] === "") lines.pop();
      for (let raw of lines) {
        if (raw.endsWith("\r")) raw = raw.slice(0, -1);
        const t = raw.trim();
        if (t.length >= 2 && t[0] === "[" && t[t.length - 1] === "]") {
          this.secs.push({ name: t.slice(1, -1).trim().toLowerCase(), header: raw, lines: [] });
          continue;
        }
        const eq = t.indexOf("=");
        if (t && t[0] !== "#" && t[0] !== ";" && eq > 0) {
          this.secs[this.secs.length - 1].lines.push({ raw, key: t.slice(0, eq).trim().toLowerCase(), value: parseValue(t.slice(eq + 1)) });
        } else {
          this.secs[this.secs.length - 1].lines.push({ raw, key: null, value: "" });
        }
      }
    }

    serialize() {
      let out = "";
      this.secs.forEach((s, i) => {
        if (i > 0 || s.name) out += (s.header || "[" + s.name + "]") + "\n";
        for (const l of s.lines) out += l.raw + "\n";
      });
      return out;
    }

    find(section) {
      const n = section.toLowerCase();
      return this.secs.find((s) => s.name === n) || null;
    }

    has(section, key) {
      const s = this.find(section);
      const k = key.toLowerCase();
      return !!s && s.lines.some((l) => l.key === k);
    }

    get(section, key, def = null) {
      const v = this.getAll(section, key);
      return v.length ? v[0] : def;
    }

    getAll(section, key) {
      const s = this.find(section);
      const k = key.toLowerCase();
      return s ? s.lines.filter((l) => l.key === k).map((l) => l.value) : [];
    }

    findOrCreate(section) {
      let s = this.find(section);
      if (s) return s;
      const last = this.secs[this.secs.length - 1];
      if (!(last.name === "" && !last.lines.length) && (!last.lines.length || last.lines[last.lines.length - 1].raw.trim())) {
        last.lines.push({ raw: "", key: null, value: "" });
      }
      s = { name: section.toLowerCase(), header: "", lines: [] };
      this.secs.push(s);
      return s;
    }

    set(section, key, value) {
      this.setAll(section, key, [value]);
    }

    setAll(section, key, values) {
      if (this.has(section, key) && JSON.stringify(this.getAll(section, key)) === JSON.stringify(values)) return;
      const s = this.findOrCreate(section);
      const k = key.toLowerCase();
      let shown = key;
      const first = s.lines.find((l) => l.key === k);
      if (first) shown = first.raw.trim().split("=")[0].trim();
      const fresh = values.map((v) => ({ raw: shown + " = " + formatValue(v), key: k, value: v }));
      const out = [];
      let inserted = false;
      for (const l of s.lines) {
        if (l.key === k) {
          if (!inserted) {
            out.push(...fresh);
            inserted = true;
          }
          continue;
        }
        out.push(l);
      }
      if (!inserted) {
        let at = out.length;
        while (at > 0 && out[at - 1].key === null && !out[at - 1].raw.trim()) at--;
        out.splice(at, 0, ...fresh);
      }
      s.lines = out;
    }

    remove(section, key) {
      const s = this.find(section);
      if (!s) return;
      const k = key.toLowerCase();
      s.lines = s.lines.filter((l) => l.key !== k);
    }
  }

  // Come DhConfig::splitEntry: "primo | resto", divide al PRIMO '|'.
  function splitEntry(entry) {
    const bar = entry.indexOf("|");
    const first = (bar < 0 ? entry : entry.slice(0, bar)).trim();
    let rest = bar < 0 ? "" : entry.slice(bar + 1);
    if (rest.startsWith(" ")) rest = rest.slice(1);
    if (rest.endsWith(" ") && !rest.endsWith("  ")) rest = rest.slice(0, -1);
    return [first, rest];
  }

  // =================================================================== seriale
  // Una richiesta alla volta per canale: il protocollo è riga-per-riga, le
  // risposte si riconoscono dal tipo (e dal comando/percorso).
  const OUR_TYPES = new Set(["storage_info", "storage_result", "config", "config_result", "file_list",
                             "file_data", "file_write_result", "file_result"]);
  const channels = {};  // nome -> funzione di invio
  let pending = null;
  let queue = Promise.resolve();

  function isOurCmd(cmd) {
    return typeof cmd === "string" && /^(config|storage|file)_/.test(cmd);
  }

  function request(channel, obj, expectType, timeoutMs = 6000) {
    const run = () => new Promise((resolve, reject) => {
      const send = channels[channel];
      if (!send) {
        reject(new Error("dispositivo USB non collegato"));
        return;
      }
      const timer = setTimeout(() => {
        pending = null;
        reject(new Error("nessuna risposta dal display (firmware senza questi comandi?)"));
      }, timeoutMs);
      pending = {
        channel,
        match: (m) => m.type === expectType && (!m.cmd || m.cmd === obj.cmd) &&
          (m.type !== "file_data" || (m.path === obj.path && m.offset === obj.offset)),
        isError: (m) => m.type === "error" && m.cmd === obj.cmd,
        resolve: (m) => { clearTimeout(timer); pending = null; resolve(m); },
        reject: (e) => { clearTimeout(timer); pending = null; reject(e); },
      };
      Promise.resolve(send(obj)).catch((e) => pending && pending.reject(e));
    });
    const p = queue.then(run, run);
    queue = p.catch(() => {});
    return p;
  }

  function onSerialMessage(channel, msg) {
    if (!msg || typeof msg !== "object") return false;
    if (pending && pending.channel === channel) {
      if (pending.match(msg)) { pending.resolve(msg); return true; }
      if (pending.isError(msg)) { pending.reject(new Error(msg.error || "errore")); return true; }
    }
    // Risposte nostre arrivate fuori tempo: non vanno mostrate come altro.
    return OUR_TYPES.has(msg.type) || (msg.type === "error" && isOurCmd(msg.cmd));
  }

  function attachSerial(name, send) {
    channels[name] = send;
    refreshSources();
  }

  function detachSerial(name) {
    delete channels[name];
    if (pending && pending.channel === name) pending.reject(new Error("collegamento USB chiuso"));
    refreshSources();
  }

  // =================================================================== UI
  const $ = (id) => document.getElementById(id);
  let doc = new IniDoc("");
  let serverRev = 0;
  let loadedSource = null;
  let sdPath = "/";

  function status(text, isError) {
    const el = $("devcfg-status");
    el.textContent = text;
    el.style.color = isError ? "var(--danger)" : "";
  }

  function sourceKind(v) {
    return (v || "").split(":")[0];
  }

  async function refreshSources() {
    const sel = $("devcfg-source");
    if (!sel) return;
    const keep = sel.value;
    const ids = new Set();
    try {
      (await (await fetch("/api/device-configs")).json()).forEach((d) => ids.add(d.device_id));
    } catch (_) { /* server non raggiungibile: restano USB e file nuovo */ }
    try {
      const devs = await (await fetch("/api/devices")).json();
      (Array.isArray(devs) ? devs : []).forEach((d) => d.device_id && ids.add(d.device_id));
    } catch (_) {}
    sel.innerHTML = "";
    const add = (value, label) => {
      const o = document.createElement("option");
      o.value = value;
      o.textContent = label;
      sel.appendChild(o);
    };
    if (channels.loader) add("usb:loader", "Display collegato via USB (loader)");
    if (channels.bridge) add("usb:bridge", "Display collegato via USB (ponte, modalità USB)");
    [...ids].sort().forEach((id) => add("server:" + id, "Copia sul server: " + id));
    add("new", "File nuovo (da copiare sulla microSD)");
    if ([...sel.options].some((o) => o.value === keep)) sel.value = keep;
    updateSdVisibility();
  }

  function updateSdVisibility() {
    const usb = sourceKind($("devcfg-source").value) === "usb";
    $("devcfg-sd").style.display = usb ? "block" : "none";
    if (usb) refreshSd();
  }

  // ------------------------------------------------------------ editor a campi
  function render() {
    document.querySelectorAll("#devcfg-editor [data-ini]").forEach((el) => {
      const [s, k] = el.dataset.ini.split(".");
      if (el.type === "checkbox") {
        const v = doc.has(s, k) ? String(doc.get(s, k)).toLowerCase() : null;
        el.indeterminate = v === null;  // assente: vale la build
        el.checked = ["1", "true", "si", "sì", "yes", "on"].includes(v);
        el.title = v === null ? "Non impostato nel file: vale la build" : "";
      } else {
        el.value = doc.has(s, k) ? doc.get(s, k) : "";
        el.placeholder = "(valore della build)";
      }
    });
    renderWifi();
    renderBt();
    $("devcfg-vpn-identity").textContent = doc.has("vpn", "identity")
      ? "Identità Tailscale salvata nel file (scritta dal display): il display resta lo stesso dispositivo sulla tailnet."
      : "Nessuna identità Tailscale nel file: il display la crea alla prima connessione.";
    $("devcfg-text").value = doc.serialize();
  }

  function changed() {
    $("devcfg-text").value = doc.serialize();
  }

  function onFieldChange(e) {
    const el = e.target;
    if (!el.dataset || !el.dataset.ini) return;
    const [s, k] = el.dataset.ini.split(".");
    if (el.type === "checkbox") {
      el.indeterminate = false;
      doc.set(s, k, el.checked ? "1" : "0");
    } else if (el.value.trim() === "") {
      doc.remove(s, k);  // vuoto = valore della build
    } else {
      doc.set(s, k, el.value.trim());
    }
    changed();
  }

  function listEntries(s, k) {
    return doc.getAll(s, k).filter((e) => e.trim());
  }

  function setList(s, k, entries) {
    doc.setAll(s, k, entries.length ? entries : [""]);  // voce vuota = elenco svuotato (non torna quello della build)
    changed();
  }

  function itemButton(label, title, onClick, disabled) {
    const b = document.createElement("button");
    b.className = "btn-secondary";
    b.textContent = label;
    b.title = title;
    b.disabled = !!disabled;
    b.addEventListener("click", onClick);
    return b;
  }

  function renderWifi() {
    const ul = $("devcfg-wifi-list");
    ul.innerHTML = "";
    const nets = listEntries("wifi", "network");
    if (!doc.has("wifi", "network")) {
      const li = document.createElement("li");
      li.className = "hint";
      li.textContent = "Nessuna rete nel file: vale quella della build. Aggiungendone una, l'elenco del file sostituisce quello della build.";
      ul.appendChild(li);
    }
    nets.forEach((e, i) => {
      const [ssid, pw] = splitEntry(e);
      const li = document.createElement("li");
      li.className = "page-item";
      li.style.cursor = "default";
      const name = document.createElement("span");
      name.textContent = `${i + 1}. ${ssid}  ${pw ? "🔒" : "(aperta)"}`;
      const act = document.createElement("span");
      act.className = "devcfg-item-actions";
      const move = (d) => () => {
        const n = nets.slice();
        [n[i], n[i + d]] = [n[i + d], n[i]];
        setList("wifi", "network", n);
        renderWifi();
      };
      act.appendChild(itemButton("▲", "Più priorità", move(-1), i === 0));
      act.appendChild(itemButton("▼", "Meno priorità", move(+1), i === nets.length - 1));
      act.appendChild(itemButton("✎", "Modifica password", () => {
        $("devcfg-wifi-ssid").value = ssid;
        $("devcfg-wifi-pw").value = pw;
        $("devcfg-wifi-pw").focus();
      }));
      act.appendChild(itemButton("✕", "Dimentica", () => {
        setList("wifi", "network", nets.filter((_, j) => j !== i));
        renderWifi();
      }));
      li.appendChild(name);
      li.appendChild(act);
      ul.appendChild(li);
    });
  }

  function addWifi() {
    const ssid = $("devcfg-wifi-ssid").value.trim();
    const pw = $("devcfg-wifi-pw").value;
    if (!ssid) return;
    if (ssid.includes("|")) {
      alert("Il nome della rete non può contenere '|'.");
      return;
    }
    const nets = listEntries("wifi", "network");
    const entry = ssid + " | " + pw;
    const at = nets.findIndex((e) => listId(e) === ssid.toLowerCase());
    if (at >= 0) nets[at] = entry;  // stessa rete: aggiorna la password, stessa posizione
    else nets.push(entry);
    setList("wifi", "network", nets);
    $("devcfg-wifi-ssid").value = "";
    $("devcfg-wifi-pw").value = "";
    renderWifi();
  }

  function renderBt() {
    const ul = $("devcfg-bt-list");
    ul.innerHTML = "";
    const devs = listEntries("bt", "device");
    const bonds = listEntries("bt", "bond");
    if (!devs.length) {
      const li = document.createElement("li");
      li.className = "hint";
      li.textContent = "Nessun dispositivo.";
      ul.appendChild(li);
    }
    devs.forEach((e) => {
      const [addr, rest] = splitEntry(e);
      const name = splitEntry(rest)[1] || addr;
      const hex = addr.replace(/:/g, "").toLowerCase();
      const keys = bonds.filter((b) => listId(b).includes(hex));
      const li = document.createElement("li");
      li.className = "page-item";
      li.style.cursor = "default";
      const label = document.createElement("span");
      label.textContent = `${name}  ${addr}  ${keys.length ? "(accoppiato)" : "(senza chiavi)"}`;
      li.appendChild(label);
      li.appendChild(itemButton("✕", "Dimentica (anche le chiavi)", () => {
        setList("bt", "device", devs.filter((d) => d !== e));
        setList("bt", "bond", bonds.filter((b) => !listId(b).includes(hex)));
        renderBt();
      }));
      ul.appendChild(li);
    });
  }

  // ------------------------------------------------------------ carica / salva
  const NEW_TEMPLATE =
    "# Display Hub - configurazione (displayhub.txt, radice della microSD)\n" +
    "# Modificabile anche a mano: una chiave assente vale quanto deciso nella build.\n" +
    "[device]\nid = \n";

  async function load() {
    const src = $("devcfg-source").value;
    const kind = sourceKind(src);
    try {
      if (kind === "server") {
        const id = src.slice(7);
        const res = await fetch(`/api/devices/${encodeURIComponent(id)}/config`);
        const data = await res.json();
        if (res.status === 404) {
          doc = new IniDoc(NEW_TEMPLATE.replace("id = \n", `id = ${id}\n`));
          serverRev = 0;
          status(`Nessuna copia sul server per ${id}: il display la invierà al primo poll. Salvando ora crei la prima versione.`);
        } else if (!res.ok) {
          throw new Error(data.error || res.status);
        } else {
          doc = new IniDoc(data.text);
          serverRev = data.rev;
          const when = new Date(data.updated_at * 1000).toLocaleString();
          status(`Copia sul server di ${id}: revisione ${data.rev}, ${when}, ultima modifica ${data.source === "web" ? "dalla web UI" : "dal display"}.`);
        }
      } else if (kind === "usb") {
        const m = await request(src.slice(4), { cmd: "config_get" }, "config");
        doc = new IniDoc(m.text);
        status(`Configurazione letta dal display via USB (${m.status}${m.persistent ? "" : " — le modifiche valgono fino al riavvio"}).`);
      } else {
        doc = new IniDoc(NEW_TEMPLATE);
        status("File nuovo: compila i campi e scaricalo come displayhub.txt nella radice della microSD.");
      }
      loadedSource = src;
      render();
    } catch (e) {
      status("Caricamento non riuscito: " + e.message, true);
    }
  }

  async function save() {
    const src = $("devcfg-source").value;
    const kind = sourceKind(src);
    if (loadedSource !== src) {
      status("Prima carica la configurazione da questa origine (o scarica il file).", true);
      return;
    }
    const text = doc.serialize();
    try {
      if (kind === "server") {
        const id = src.slice(7);
        const res = await fetch(`/api/devices/${encodeURIComponent(id)}/config`, {
          method: "PUT",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ base_rev: serverRev, text }),
        });
        const data = await res.json();
        if (res.status === 409) {
          if (confirm(`Nel frattempo la configurazione è cambiata (revisione ${data.current.rev}, di solito dal display). ` +
                      "Ricaricarla? Le modifiche fatte qui andranno perse (puoi prima copiarle dal testo completo).")) {
            await load();
          } else {
            status("Non salvato: configurazione cambiata nel frattempo.", true);
          }
          return;
        }
        if (!res.ok) throw new Error(data.error || res.status);
        serverRev = data.rev;
        status(`Salvata sul server (revisione ${data.rev}): il display la riceve al prossimo poll.`);
      } else if (kind === "usb") {
        await request(src.slice(4), { cmd: "config_replace", text }, "config_result");
        status("Salvata sul display via USB.");
      } else {
        download();
      }
    } catch (e) {
      status("Salvataggio non riuscito: " + e.message, true);
    }
  }

  function saveBlob(blob, name) {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = name;
    document.body.appendChild(a);
    a.click();
    setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
  }

  function download() {
    saveBlob(new Blob([doc.serialize()], { type: "text/plain" }), "displayhub.txt");
    status("Scaricato displayhub.txt: copialo nella radice della microSD (FAT32).");
  }

  function openFile(e) {
    const f = e.target.files[0];
    if (!f) return;
    const r = new FileReader();
    r.onload = () => {
      doc = new IniDoc(String(r.result));
      render();
      status(`Aperto ${f.name}: controlla i campi e salva (o scarica).`);
    };
    r.readAsText(f);
    e.target.value = "";
  }

  // ------------------------------------------------------------ microSD via USB
  function usbChannel() {
    const v = $("devcfg-source").value;
    return sourceKind(v) === "usb" ? v.slice(4) : null;
  }

  const DECISIONS_OWN = [["merge", "Unisci"], ["use_card", "Usa microSD"], ["overwrite", "Sovrascrivi"]];
  const DECISIONS_FOREIGN = [["ignore", "Ignora"], ["use_card", "Usa comunque"], ["overwrite", "Sovrascrivi"]];

  function fmtBytes(n) {
    if (n >= 1073741824) return (n / 1073741824).toFixed(1) + " GB";
    if (n >= 1048576) return (n / 1048576).toFixed(1) + " MB";
    if (n >= 1024) return (n / 1024).toFixed(1) + " KB";
    return n + " B";
  }

  async function refreshSd() {
    const ch = usbChannel();
    if (!ch) return;
    try {
      const info = await request(ch, { cmd: "storage_info" }, "storage_info");
      let t = `Archivio: ${info.status}.`;
      if (info.backend !== "sd") t += " Questa build non usa la microSD (configurazione nella memoria interna).";
      if (info.card_total) t += ` Spazio: ${fmtBytes(info.card_free)} liberi su ${fmtBytes(info.card_total)}.`;
      if (info.dirty) t += " Modifiche non ancora salvate.";
      $("devcfg-sd-status").textContent = t;
      const dec = $("devcfg-sd-decision");
      dec.innerHTML = "";
      if (info.pending_owner !== undefined) {
        const lab = document.createElement("span");
        lab.textContent = info.pending_foreign
          ? `microSD di un altro display (${info.pending_owner}):`
          : "microSD inserita con una configurazione:";
        dec.appendChild(lab);
        (info.pending_foreign ? DECISIONS_FOREIGN : DECISIONS_OWN).forEach(([code, label]) => {
          dec.appendChild(itemButton(label, "", async () => {
            await sdAction("storage_decide", { decision: code });
          }));
        });
        dec.style.display = "flex";
      } else {
        dec.style.display = "none";
      }
      $("devcfg-sd-save").style.display = info.backend === "sd" ? "" : "none";
      $("devcfg-sd-eject").style.display = info.backend === "sd" ? "" : "none";
      $("devcfg-sd-files").style.display = info.files ? "block" : "none";
      if (info.files) await listDir(sdPath);
    } catch (e) {
      $("devcfg-sd-status").textContent = "Stato della microSD non disponibile: " + e.message;
      $("devcfg-sd-files").style.display = "none";
    }
  }

  async function sdAction(cmd, extra) {
    const ch = usbChannel();
    if (!ch) return;
    try {
      await request(ch, Object.assign({ cmd }, extra || {}), "storage_result");
    } catch (e) {
      alert(e.message);
    }
    await refreshSd();
  }

  function joinPath(dir, name) {
    return (dir.endsWith("/") ? dir : dir + "/") + name;
  }

  async function listDir(path) {
    const ch = usbChannel();
    const m = await request(ch, { cmd: "file_list", path }, "file_list");
    sdPath = m.path;
    $("devcfg-sd-path").textContent = sdPath;
    const ul = $("devcfg-sd-list");
    ul.innerHTML = "";
    const entries = (m.entries || []).slice().sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
    if (!entries.length) {
      const li = document.createElement("li");
      li.className = "hint";
      li.textContent = "Cartella vuota.";
      ul.appendChild(li);
    }
    for (const f of entries) {
      const full = joinPath(sdPath, f.name);
      const li = document.createElement("li");
      li.className = "page-item";
      li.style.cursor = "default";
      const label = document.createElement("span");
      label.textContent = f.dir ? `📁 ${f.name}` : `📄 ${f.name}  (${fmtBytes(f.size)})`;
      const act = document.createElement("span");
      act.className = "devcfg-item-actions";
      const isConfig = sdPath === "/" && /^displayhub\.(txt|tmp|bak)$/i.test(f.name);
      if (f.dir) act.appendChild(itemButton("Apri", "", () => listDir(full)));
      else act.appendChild(itemButton("⬇", "Scarica", () => downloadFile(full, f.name)));
      if (!isConfig) {
        act.appendChild(itemButton("✕", "Cancella", async () => {
          if (!confirm(`Cancellare ${full}?`)) return;
          try {
            await request(ch, { cmd: "file_remove", path: full }, "file_result");
          } catch (e) {
            alert(e.message);
          }
          await listDir(sdPath);
        }));
      }
      li.appendChild(label);
      li.appendChild(act);
      ul.appendChild(li);
    }
  }

  function b64ToBytes(b64) {
    const bin = atob(b64 || "");
    const out = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) out[i] = bin.charCodeAt(i);
    return out;
  }

  function bytesToB64(bytes) {
    let bin = "";
    for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
    return btoa(bin);
  }

  async function downloadFile(path, name) {
    const ch = usbChannel();
    const parts = [];
    let offset = 0;
    try {
      for (;;) {
        const m = await request(ch, { cmd: "file_read", path, offset, length: 768 }, "file_data");
        const chunk = b64ToBytes(m.data);
        parts.push(chunk);
        offset += chunk.length;
        $("devcfg-sd-status").textContent = `Scarico ${name}: ${fmtBytes(offset)} di ${fmtBytes(m.total)}...`;
        if (m.eof || !chunk.length) break;
      }
      saveBlob(new Blob(parts), name);
      $("devcfg-sd-status").textContent = `Scaricato ${name} (${fmtBytes(offset)}).`;
    } catch (e) {
      $("devcfg-sd-status").textContent = "Scaricamento non riuscito: " + e.message;
    }
  }

  async function uploadFile(e) {
    const f = e.target.files[0];
    e.target.value = "";
    if (!f) return;
    const ch = usbChannel();
    const path = joinPath(sdPath, f.name);
    if (sdPath === "/" && /^displayhub\.(txt|tmp|bak)$/i.test(f.name)) {
      alert("La configurazione si carica con \"Apri file…\" e \"Salva\" (resta coerente con il display).");
      return;
    }
    const bytes = new Uint8Array(await f.arrayBuffer());
    try {
      let offset = 0;
      do {
        const chunk = bytes.subarray(offset, offset + 768);
        const final = offset + chunk.length >= bytes.length;
        await request(ch, { cmd: "file_write", path, offset, data: bytesToB64(chunk), final }, "file_write_result");
        offset += chunk.length;
        $("devcfg-sd-status").textContent = `Carico ${f.name}: ${fmtBytes(offset)} di ${fmtBytes(bytes.length)}...`;
      } while (offset < bytes.length);
      $("devcfg-sd-status").textContent = `Caricato ${path} (${fmtBytes(bytes.length)}).`;
    } catch (err) {
      $("devcfg-sd-status").textContent = "Caricamento non riuscito: " + err.message + " (il file parziale non viene tenuto)";
    }
    await listDir(sdPath);
  }

  // ------------------------------------------------------------ avvio
  function init() {
    if (!$("devconfig-panel")) return;
    $("devcfg-source").addEventListener("change", () => {
      updateSdVisibility();
      load();
    });
    $("devcfg-load-btn").addEventListener("click", load);
    $("devcfg-save-btn").addEventListener("click", save);
    $("devcfg-download-btn").addEventListener("click", download);
    $("devcfg-open-input").addEventListener("change", openFile);
    $("devcfg-editor").addEventListener("change", onFieldChange);
    $("devcfg-wifi-add").addEventListener("click", addWifi);
    $("devcfg-text-apply").addEventListener("click", () => {
      doc = new IniDoc($("devcfg-text").value);
      render();
      status("Testo applicato ai campi (non ancora salvato).");
    });
    $("devcfg-sd-refresh").addEventListener("click", refreshSd);
    $("devcfg-sd-save").addEventListener("click", () => sdAction("storage_save"));
    $("devcfg-sd-eject").addEventListener("click", () => sdAction("storage_eject"));
    $("devcfg-sd-up").addEventListener("click", () => {
      const up = sdPath.replace(/\/[^/]*\/?$/, "") || "/";
      listDir(up).catch((e) => alert(e.message));
    });
    $("devcfg-sd-mkdir").addEventListener("click", async () => {
      const name = prompt("Nome della nuova cartella:");
      if (!name || /[\/\\]|\.\./.test(name)) return;
      try {
        await request(usbChannel(), { cmd: "file_mkdir", path: joinPath(sdPath, name) }, "file_result");
      } catch (err) {
        alert(err.message);
      }
      await listDir(sdPath);
    });
    $("devcfg-sd-upload").addEventListener("change", uploadFile);
    $("devconfig-panel").addEventListener("toggle", () => {
      if ($("devconfig-panel").open) refreshSources();
    });
    // Entrando nella sezione "Configurazione display": elenco aggiornato
    // (display comparsi o dimenticati nel frattempo).
    window.addEventListener("dh:view", e => { if (e.detail === "config") refreshSources(); });
    refreshSources();
    render();
  }

  window.DhTools = { attachSerial, detachSerial, onSerialMessage, IniDoc, splitEntry, request };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
