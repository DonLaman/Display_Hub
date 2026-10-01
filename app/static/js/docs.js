/*
 * Sezioni "Documentazione" (file .md del progetto come HTML) e "Versioni"
 * (registro versioni/revisioni del firmware), più il collegamento tra il
 * registro e il form di compilazione: il form propone versione e revisione
 * della voce più recente (restano modificabili; se le cambi a mano non
 * vengono più toccate, e un link permette di tornare a quelle del registro).
 */
(function () {
  "use strict";
  const $ = (id) => document.getElementById(id);

  // ------------------------------------------------------------ documentazione
  let docsList = null;
  let currentDoc = null;

  function docIdFromHash() {
    const parts = (location.hash || "").replace(/^#\/?/, "").split("/");
    return parts[0] === "documentazione" ? parts[1] || "" : null;
  }

  async function loadDocsList() {
    if (docsList) return docsList;
    try {
      docsList = await (await fetch("/api/docs")).json();
    } catch (e) {
      docsList = [];
    }
    const box = $("docs-list");
    box.innerHTML = "";
    let group = null;
    docsList.forEach((d) => {
      if (d.group !== group) {
        group = d.group;
        const g = document.createElement("div");
        g.className = "docs-group";
        g.textContent = group;
        box.appendChild(g);
      }
      const a = document.createElement("a");
      a.className = "docs-link";
      a.href = `#/documentazione/${d.id}`;
      a.dataset.doc = d.id;
      a.textContent = d.title;
      box.appendChild(a);
    });
    if (!docsList.length) box.innerHTML = '<p class="hint">Nessun documento disponibile.</p>';
    return docsList;
  }

  async function showDoc() {
    const list = await loadDocsList();
    let id = docIdFromHash();
    if (id === null) return;
    if (!id) id = list.length ? list[0].id : "";
    if (!id || id === currentDoc) return;
    document.querySelectorAll(".docs-link").forEach((a) => a.classList.toggle("active", a.dataset.doc === id));
    const res = await fetch(`/api/docs/${encodeURIComponent(id)}`);
    if (!res.ok) {
      $("docs-content").innerHTML = '<p class="hint">Documento non trovato.</p>';
      $("docs-toc").innerHTML = "";
      return;
    }
    const doc = await res.json();
    currentDoc = id;
    // Contenuto dei file .md del progetto (fidato), convertito dal server.
    $("docs-content").innerHTML = doc.html;
    $("docs-toc").innerHTML = doc.toc || "";
    $("docs-meta").textContent = `${doc.path} — aggiornato il ${new Date(doc.updated_at * 1000).toLocaleString("it-IT")}`;
    window.scrollTo(0, 0);
  }

  // I link "#titolo" di indice e documento scorrono al titolo: non devono
  // cambiare sezione (la pagina usa #/... per le sezioni).
  function interceptAnchors(e) {
    const a = e.target.closest("a");
    if (!a) return;
    const href = a.getAttribute("href") || "";
    if (href.startsWith("#") && !href.startsWith("#/")) {
      e.preventDefault();
      const target = document.getElementById(decodeURIComponent(href.slice(1)));
      if (target && target.scrollIntoView) target.scrollIntoView({ behavior: "smooth", block: "start" });
    }
  }

  // ------------------------------------------------------------ versioni
  let changelog = null;
  let editingId = null;

  function bumpVersion(v) {
    const m = String(v || "").match(/^(\d+)\.(\d+)(?:\.(\d+))?(.*)$/);
    if (!m) return v || "1.0.0";
    return `${m[1]}.${parseInt(m[2], 10) + 1}.0`;
  }

  function label(entry) {
    return entry.revision ? `${entry.version} ${entry.revision}` : entry.version;
  }

  async function loadChangelog() {
    try {
      changelog = await (await fetch("/api/changelog")).json();
    } catch (e) {
      return;
    }
    renderChangelog();
    applyToBuildForm(false);
  }

  function renderChangelog() {
    const box = $("cl-list");
    box.innerHTML = "";
    const entries = (changelog && changelog.entries) || [];
    if (!entries.length) {
      box.innerHTML = '<p class="hint">Registro vuoto: aggiungi la prima versione.</p>';
    }
    entries.forEach((e, i) => {
      const card = document.createElement("div");
      card.className = "cl-entry" + (i === 0 ? " current" : "");
      card.id = `cl-${e.id}`;
      const head = document.createElement("div");
      head.className = "cl-entry-head";
      const ver = document.createElement("span");
      ver.className = "cl-entry-version";
      ver.textContent = label(e);
      head.appendChild(ver);
      if (i === 0) {
        const b = document.createElement("span");
        b.className = "badge badge-accent";
        b.textContent = "attuale";
        head.appendChild(b);
      }
      if (e.title) {
        const t = document.createElement("span");
        t.className = "cl-entry-title";
        t.textContent = e.title;
        head.appendChild(t);
      }
      const date = document.createElement("span");
      date.className = "cl-entry-date";
      date.textContent = e.date ? new Date(e.date + "T12:00:00").toLocaleDateString("it-IT") : "";
      head.appendChild(date);
      const actions = document.createElement("span");
      actions.className = "cl-entry-actions";
      const edit = document.createElement("button");
      edit.className = "btn-secondary btn-small";
      edit.textContent = "Modifica";
      edit.addEventListener("click", () => openEditor(e));
      const del = document.createElement("button");
      del.className = "btn-remove";
      del.textContent = "✕";
      del.title = "Elimina questa voce";
      del.addEventListener("click", () => deleteEntry(e));
      actions.appendChild(edit);
      actions.appendChild(del);
      head.appendChild(actions);
      card.appendChild(head);
      if (e.notes_html) {
        const notes = document.createElement("div");
        notes.className = "doc-content";
        notes.innerHTML = e.notes_html;   // Markdown convertito dal server con l'HTML scappato
        card.appendChild(notes);
      }
      box.appendChild(card);
    });
  }

  function openEditor(entry, preset) {
    editingId = entry ? entry.id : null;
    const src = entry || preset || {};
    $("cl-editor-title").textContent = entry ? `Modifica ${label(entry)}` : "Nuova voce";
    $("cl-version").value = src.version || "";
    $("cl-revision").value = src.revision || "";
    $("cl-date").value = src.date || new Date().toISOString().slice(0, 10);
    $("cl-title").value = src.title || "";
    $("cl-notes").value = src.notes || "";
    $("cl-editor").classList.remove("hidden");
    $("cl-feedback").textContent = "";
    $("cl-version").focus();
  }

  function closeEditor() {
    $("cl-editor").classList.add("hidden");
    editingId = null;
  }

  async function saveEntry() {
    const body = {
      version: $("cl-version").value.trim(),
      revision: $("cl-revision").value.trim(),
      date: $("cl-date").value,
      title: $("cl-title").value.trim(),
      notes: $("cl-notes").value,
    };
    const res = await fetch(editingId ? `/api/changelog/${editingId}` : "/api/changelog", {
      method: editingId ? "PUT" : "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
      $("cl-feedback").style.color = "var(--danger)";
      $("cl-feedback").textContent = data.error || "Salvataggio non riuscito";
      return;
    }
    changelog = data;
    closeEditor();
    renderChangelog();
    applyToBuildForm(false);
    $("cl-feedback").style.color = "";
    $("cl-feedback").textContent = "Salvato ✓";
  }

  async function deleteEntry(e) {
    if (!confirm(`Eliminare la voce ${label(e)}${e.title ? ` "${e.title}"` : ""}?`)) return;
    const res = await fetch(`/api/changelog/${e.id}`, { method: "DELETE" });
    if (res.ok) {
      changelog = await res.json();
      renderChangelog();
      applyToBuildForm(false);
    }
  }

  // ------------------------------------------------------------ form di compilazione
  // Il form propone versione e revisione della voce attuale. Se l'utente le
  // cambia a mano non vengono più sovrascritte (resta il link per tornarci).
  function applyToBuildForm(force) {
    const v = $("build-fw-version-input"), r = $("build-fw-revision-input"), hint = $("build-version-source");
    if (!v || !r || !hint || !changelog || !changelog.current) return;
    const cur = changelog.current;
    const entry = changelog.entries[0];
    if (force || !v.dataset.userEdited) v.value = cur.version;
    if (force || !r.dataset.userEdited) r.value = cur.revision || "";
    if (force) {
      delete v.dataset.userEdited;
      delete r.dataset.userEdited;
    }
    hint.innerHTML = "";
    const differs = v.value.trim() !== cur.version || r.value.trim() !== (cur.revision || "");
    hint.appendChild(document.createTextNode(`Dal registro Versioni: ${label(cur)}`));
    if (entry && entry.title) hint.appendChild(document.createTextNode(` — «${entry.title}»`));
    hint.appendChild(document.createTextNode(". "));
    if (differs) {
      const back = document.createElement("a");
      back.href = "#";
      back.textContent = "Usa quella del registro";
      back.addEventListener("click", (e) => { e.preventDefault(); applyToBuildForm(true); });
      hint.appendChild(back);
      hint.appendChild(document.createTextNode(" · "));
    }
    const go = document.createElement("a");
    go.href = "#/versioni";
    go.textContent = "Apri il registro";
    hint.appendChild(go);
  }

  function markEdited(e) {
    e.target.dataset.userEdited = "1";
    applyToBuildForm(false);
  }

  // ------------------------------------------------------------ avvio
  function init() {
    document.addEventListener("click", (e) => {
      if (e.target.closest("#docs-content, #docs-toc, #cl-list")) interceptAnchors(e);
    });
    window.addEventListener("dh:view", (e) => {
      if (e.detail === "docs") showDoc();
      if (e.detail === "versions") loadChangelog();
    });
    ["build-fw-version-input", "build-fw-revision-input"].forEach((id) => {
      const el = $(id);
      if (el) el.addEventListener("input", markEdited);
    });
    $("cl-new-revision").addEventListener("click", () => {
      const cur = (changelog && changelog.current) || {};
      openEditor(null, { version: cur.version || "1.0.0", revision: changelog ? changelog.next_revision : "rev_1" });
    });
    $("cl-new-version").addEventListener("click", () => {
      const cur = (changelog && changelog.current) || {};
      openEditor(null, { version: bumpVersion(cur.version), revision: "" });
    });
    $("cl-cancel").addEventListener("click", closeEditor);
    $("cl-save").addEventListener("click", saveEntry);
    loadChangelog();
    if (docIdFromHash() !== null) showDoc();
  }

  window.DhDocs = { showDoc, loadChangelog, applyToBuildForm, bumpVersion };
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init);
  else init();
})();
