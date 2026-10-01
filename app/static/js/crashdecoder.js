/*
 * "Decodifica un crash": incolli il log della console seriale del display
 * (Guru Meditation, watchdog, abort...) e il server traduce gli indirizzi in
 * funzione / file / riga (POST /api/firmware/decode, app/core/crash_decoder.py).
 *
 * L'ELF giusto viene trovato dall'impronta "ELF file SHA256: ..." che il
 * firmware stampa nel crash; se manca si sceglie il firmware a mano (solo
 * quelli di cui il builder ha conservato l'ELF).
 *
 * Un riquadro per ogni sezione di compilazione: <div class="crash-decoder"
 * data-kind="main|loader|loader_full">. data-kind filtra l'elenco dei firmware.
 */
(function () {
  "use strict";

  const KIND_LABEL = { main: "firmware principale", loader: "loader leggero", loader_full: "loader con schermo",
                       gateway: "gateway Bluetooth" };

  function el(tag, attrs, text) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs || {})) e.setAttribute(k, v);
    if (text !== undefined) e.textContent = text;
    return e;
  }

  async function loadFirmwareOptions(select, kind) {
    const keep = select.value;
    select.innerHTML = "";
    select.appendChild(el("option", { value: "" }, "Automatico (dall'impronta ELF nel log)"));
    try {
      const res = await fetch("/api/firmware");
      const list = await res.json();
      for (const f of list) {
        if (f.kind !== kind) continue;
        const o = el("option", { value: f.filename },
          f.filename + (f.has_elf ? "" : "  (ELF non conservato)"));
        if (!f.has_elf) o.disabled = true;
        select.appendChild(o);
      }
    } catch (e) { /* elenco non disponibile: resta la scelta automatica */ }
    select.value = [...select.options].some((o) => o.value === keep) ? keep : "";
  }

  function build(container) {
    const kind = container.dataset.kind || "main";
    const box = el("details", { class: "crash-box" });
    box.appendChild(el("summary", {}, "Decodifica un crash (" + KIND_LABEL[kind] + ")"));
    box.appendChild(el("p", { class: "hint" },
      "Incolla qui il log della console seriale dal \"Guru Meditation Error\" (o dal messaggio " +
      "di errore) fino a \"Rebooting...\". Il firmware giusto si riconosce dalla riga " +
      "\"ELF file SHA256\"; va bene anche il log copiato con gli orari davanti."));
    const ta = el("textarea", { rows: "8", class: "crash-input", placeholder: "Guru Meditation Error: Core  1 panic'ed ...\nBacktrace: 0x4211df01:0x3fceb9c0 ...\nELF file SHA256: 30ec9a28c6c8b2ab" });
    ta.spellcheck = false;
    box.appendChild(ta);
    const row = el("div", { class: "crash-row" });
    const sel = el("select", { class: "crash-firmware" });
    const btn = el("button", { class: "btn-primary", type: "button" }, "Decodifica");
    const copy = el("button", { class: "btn-secondary", type: "button" }, "Copia risultato");
    copy.style.display = "none";
    row.appendChild(sel);
    row.appendChild(btn);
    row.appendChild(copy);
    box.appendChild(row);
    const status = el("p", { class: "hint crash-status" });
    const out = el("pre", { class: "crash-output" });
    out.style.display = "none";
    box.appendChild(status);
    box.appendChild(out);
    container.appendChild(box);

    box.addEventListener("toggle", () => { if (box.open) loadFirmwareOptions(sel, kind); });

    btn.addEventListener("click", async () => {
      const text = ta.value.trim();
      if (!text) { status.textContent = "Incolla prima il log del crash."; return; }
      btn.disabled = true;
      status.textContent = "Decodifica in corso...";
      status.style.color = "";
      out.style.display = "none";
      copy.style.display = "none";
      try {
        const res = await fetch("/api/firmware/decode", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ log: text, firmware: sel.value || null }),
        });
        const data = await res.json();
        if (!res.ok) {
          status.textContent = data.error || ("Errore " + res.status);
          status.style.color = "var(--danger)";
          return;
        }
        status.textContent = "Firmware: " + data.firmware;
        out.textContent = data.text;
        out.style.display = "block";
        copy.style.display = "inline-block";
      } catch (e) {
        status.textContent = "Richiesta non riuscita: " + e;
        status.style.color = "var(--danger)";
      } finally {
        btn.disabled = false;
      }
    });

    copy.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(out.textContent);
        copy.textContent = "Copiato ✓";
        setTimeout(() => { copy.textContent = "Copia risultato"; }, 1500);
      } catch (e) { /* appunti non disponibili: si seleziona a mano */ }
    });
  }

  function init() {
    document.querySelectorAll(".crash-decoder").forEach(build);
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
  window.DhCrashDecoder = { init, build };
})();
