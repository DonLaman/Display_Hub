"""Documentazione (.md -> HTML) e registro delle versioni.
Esecuzione: python3 tests/test_docs_changelog.py"""
import json
import os
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import changelog, docs, firmware_store  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


print("== Documentazione ==")
lst = docs.list_docs()
ids = [d["id"] for d in lst]
check({"readme", "protocollo", "modifiche", "todo", "microlink", "nimble", "font"} <= set(ids), "documenti del progetto: %s" % ids)
p = docs.render("protocollo")
check(p and "<table>" in p["html"] and "<pre><code" in p["html"] and "<h2" in p["html"], "PROTOCOL: tabelle, codice, titoli")
check(p["toc"] and 'href="#' in p["toc"], "indice dei titoli")
check(docs.render("nonesiste") is None and docs.render("../etc/passwd") is None, "solo documenti dell'elenco")
html = docs._rewrite_links('<a href="PROTOCOL.md">x</a> <a href="../microlink/PORTING.md#dipendenza">y</a> <a href="https://x.it">z</a>',
                           "firmware/lib/dh_fonts/README.md")
check('href="#/documentazione/protocollo"' in html or 'PROTOCOL.md' in html, "link relativo alla radice")
check('href="#/documentazione/microlink"' in html and 'href="https://x.it"' in html, "link tra documenti -> interni; esterni invariati")
n = docs.render_markdown("- **uno** <script>alert(1)</script>\n\n```\nif (a < b) x();\n```")
check("<strong>uno</strong>" in n and "<script>" not in n and "&lt;script&gt;" in n, "note: Markdown sì, HTML scritto no")
check("a &lt; b" in n and "&amp;lt;" not in n, "codice nelle note senza doppio escape")

print("\n== Registro delle versioni ==")
tmp = tempfile.mkdtemp()
changelog._PATH = os.path.join(tmp, "changelog.json")
fake_fw = [{"meta": {"target": "loader"}}, {"meta": {"target": "main", "fw_version": "3.2.0", "fw_revision": "rev_7"}}]
with mock.patch.object(firmware_store, "list_firmware_files", return_value=fake_fw):
    e = changelog.list_entries()
check(len(e) == 1 and e[0]["version"] == "3.2.0" and e[0]["revision"] == "rev_7", "prima voce dall'ultimo firmware: 3.2.0 rev_7")
check(changelog.next_revision("rev_7") == "rev_8" and changelog.next_revision("r1") == "r2" and changelog.next_revision("") == "rev_1"
      and changelog.next_revision("hotfix") == "hotfix2", "revisione successiva")
changelog.create({"version": "3.2.0", "revision": "rev_8", "title": "Swipe", "notes": "- ok"})
check(changelog.current()["revision"] == "rev_8" and len(changelog.list_entries()) == 2, "la nuova voce diventa l'attuale")
for bad, why in (({"version": ""}, "versione vuota"), ({"version": "3 2"}, "spazi"), ({"version": "1", "revision": "a/b"}, "barra"),
                 ({"version": "1", "date": "26/09/2026"}, "data"), ({"version": "1", "notes": "x" * 20001}, "note lunghe")):
    try:
        changelog.create(bad)
        check(False, "rifiutata: " + why)
    except changelog.ValidationError:
        check(True, "rifiutata: " + why)
cur = changelog.current()
changelog.update(cur["id"], {"title": "Swipe su tutta la pagina"})
check(changelog.current()["title"] == "Swipe su tutta la pagina" and changelog.current()["revision"] == "rev_8", "modifica parziale")
check(changelog.update("nope", {"title": "x"}) is None and not changelog.delete("nope"), "voce inesistente")

print("\n== API e form ==")
from app.main import create_app  # noqa: E402

c = create_app().test_client()
d = c.get("/api/changelog").get_json()
check(d["current"]["revision"] == "rev_8" and d["next_revision"] == "rev_9" and "notes_html" in d["entries"][0], "GET registro")
r = c.post("/api/changelog", json={"version": "3.2.0", "revision": "rev_9", "title": "Archivio", "notes": "**nuovo**"})
check(r.status_code == 200 and r.get_json()["current"]["revision"] == "rev_9", "POST nuova revisione")
check(c.post("/api/changelog", json={"version": ""}).status_code == 400, "POST non valida: 400")
page = c.get("/").get_data(as_text=True)
check('id="build-fw-version-input" type="text" value="3.2.0"' in page and 'value="rev_9"' in page, "form di compilazione: 3.2.0 rev_9 proposte")
eid = r.get_json()["current"]["id"]
check(c.delete(f"/api/changelog/{eid}").get_json()["current"]["revision"] == "rev_8", "DELETE: torna attuale la precedente")
check(c.get("/api/docs/protocollo").status_code == 200 and c.get("/api/docs/nope").status_code == 404, "API documentazione")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
