/*
 * Pannello "Gateway Bluetooth audio".
 * Gestisce la config dei gateway BT (WROVER): selezione, campi rapidi
 * (cassa, volume, poll), editor diretto del file INI, liste storico/preferiti
 * YouTube, invio e salvataggio sul server via /api/bt-gateway.
 *
 * Il file INI è la fonte di verità. I "campi rapidi" e le liste sono una vista
 * comoda; "Applica campi → testo" li riversa nel testo, "Salva" invia il testo.
 */
(function () {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const sel = $("btgw-select");
  if (!sel) return; // pannello non presente

  let current = null; // device_id selezionato

  function setStatus(msg, ok) {
    const el = $("btgw-status");
    el.textContent = msg || "";
    el.style.color = ok === false ? "var(--danger, #c0392b)" : "var(--muted, #888)";
  }

  async function loadList(selectId) {
    const r = await fetch("/api/bt-gateway");
    const list = r.ok ? await r.json() : [];
    sel.innerHTML = "";
    if (!list.length) {
      const o = document.createElement("option");
      o.value = "";
      o.textContent = "(nessun gateway)";
      sel.appendChild(o);
    }
    list.forEach((g) => {
      const o = document.createElement("option");
      o.value = g.device_id;
      o.textContent = g.device_id;
      sel.appendChild(o);
    });
    if (selectId) sel.value = selectId;
    current = sel.value || null;
    if (current) await loadConfig(current);
    else clearFields();
  }

  function clearFields() {
    $("btgw-speaker-mac").value = "";
    $("btgw-speaker-name").value = "";
    $("btgw-volume").value = "";
    $("btgw-poll").value = "";
    $("btgw-text").value = "";
    renderYtList("btgw-history", [], "btgw-hist-count");
    renderYtList("btgw-favorites", [], "btgw-fav-count");
  }

  function renderYtList(ulId, items, countId) {
    const ul = $(ulId);
    ul.innerHTML = "";
    (items || []).forEach((it) => {
      const li = document.createElement("li");
      const a = document.createElement("a");
      a.href = "https://www.youtube.com/watch?v=" + encodeURIComponent(it.id);
      a.target = "_blank";
      a.rel = "noopener";
      a.textContent = it.title ? it.title : it.id;
      a.title = it.id;
      li.appendChild(a);
      ul.appendChild(li);
    });
    if (countId) $(countId).textContent = String((items || []).length);
  }

  async function loadConfig(id) {
    // vista JSON (campi + liste)
    const rj = await fetch("/api/bt-gateway/" + encodeURIComponent(id));
    if (rj.ok) {
      const c = await rj.json();
      $("btgw-speaker-mac").value = c.speaker_mac || "";
      $("btgw-speaker-name").value = c.speaker_name || "";
      $("btgw-volume").value = c.volume;
      $("btgw-poll").value = c.poll_interval_s;
      renderYtList("btgw-history", c.youtube_history, "btgw-hist-count");
      renderYtList("btgw-favorites", c.youtube_favorites, "btgw-fav-count");
    }
    // testo INI grezzo
    const rt = await fetch("/api/bt-gateway/config?device_id=" + encodeURIComponent(id));
    $("btgw-text").value = rt.ok ? await rt.text() : "";
    setStatus("Caricato: " + id, true);
  }

  // Applica i campi rapidi al testo INI (senza salvare): li invia come JSON,
  // il server li fonde nel file, poi ricarichiamo il testo aggiornato.
  async function fieldsToText() {
    if (!current) return;
    const body = {
      speaker_mac: $("btgw-speaker-mac").value.trim(),
      speaker_name: $("btgw-speaker-name").value.trim(),
      volume: parseInt($("btgw-volume").value, 10),
      poll_interval_s: parseInt($("btgw-poll").value, 10),
    };
    const r = await fetch("/api/bt-gateway/" + encodeURIComponent(current), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!r.ok) {
      const e = await r.json().catch(() => ({}));
      setStatus("Errore: " + (e.error || r.status), false);
      return;
    }
    await loadConfig(current);
    setStatus("Campi applicati al testo (non ancora salvati su file? sì: già scritti)", true);
  }

  // Salva il TESTO INI così com'è (modifica diretta): lo invia come text/plain.
  async function saveText() {
    if (!current) {
      setStatus("Nessun gateway selezionato", false);
      return;
    }
    const r = await fetch("/api/bt-gateway/" + encodeURIComponent(current), {
      method: "PUT",
      headers: { "Content-Type": "text/plain; charset=utf-8" },
      body: $("btgw-text").value,
    });
    if (!r.ok) {
      setStatus("Salvataggio non riuscito (" + r.status + ")", false);
      return;
    }
    await loadConfig(current); // rilegge campi e liste dal file appena salvato
    setStatus("Salvato ✓", true);
  }

  async function createGateway() {
    const id = ($("btgw-new-id").value || "").trim();
    if (!/^[A-Za-z0-9_-]{1,48}$/.test(id)) {
      setStatus("Id non valido (lettere, numeri, _ -)", false);
      return;
    }
    // crea salvando la config di default (PUT JSON vuoto crea il file)
    const r = await fetch("/api/bt-gateway/" + encodeURIComponent(id), {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ volume: 60 }),
    });
    if (!r.ok) {
      setStatus("Creazione non riuscita", false);
      return;
    }
    $("btgw-new-id").value = "";
    await loadList(id);
    setStatus("Creato: " + id, true);
  }

  async function deleteGateway() {
    if (!current) return;
    if (!confirm("Eliminare la configurazione del gateway \"" + current + "\"?")) return;
    const r = await fetch("/api/bt-gateway/" + encodeURIComponent(current), { method: "DELETE" });
    if (!r.ok) {
      setStatus("Eliminazione non riuscita", false);
      return;
    }
    await loadList();
    setStatus("Eliminato", true);
  }

  sel.addEventListener("change", () => {
    current = sel.value || null;
    if (current) loadConfig(current);
    else clearFields();
  });
  $("btgw-new-btn").addEventListener("click", createGateway);
  $("btgw-delete-btn").addEventListener("click", deleteGateway);
  $("btgw-fields-to-text").addEventListener("click", fieldsToText);
  $("btgw-reload-btn").addEventListener("click", () => current && loadConfig(current));
  $("btgw-save-btn").addEventListener("click", saveText);

  // carica all'apertura del pannello (una sola volta)
  const panel = $("btgw-panel");
  let loaded = false;
  const doLoad = () => { if (!loaded) { loaded = true; loadList(); } };
  if (panel.open) doLoad();
  panel.addEventListener("toggle", () => { if (panel.open) doLoad(); });
})();
