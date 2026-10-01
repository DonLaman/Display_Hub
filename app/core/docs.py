"""
Documentazione del progetto (file .md) mostrata come pagine HTML nella web UI.

Solo un elenco fisso di file del progetto (nessun percorso arbitrario dal
browser): contenuto fidato, convertito con Python-Markdown (tabelle, blocchi
di codice, indice). I link tra un documento e l'altro (es. "PROTOCOL.md")
diventano link interni alla web UI (#/documentazione/<id>).
"""
import os
import posixpath
import re
from typing import Any, Dict, List, Optional

import markdown

_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# id -> (percorso dalla radice del progetto, titolo, gruppo)
DOCS = [
    ("readme", "README.md", "Guida generale", "Progetto"),
    ("protocollo", "PROTOCOL.md", "Protocollo e API", "Progetto"),
    ("modifiche", "MODIFICHE_VPN.md", "Modifiche e verifiche", "Progetto"),
    ("todo", "TODO.md", "Da fare", "Progetto"),
    ("librerie", "firmware/lib/README.md", "Librerie del firmware", "Firmware"),
    ("microlink", "firmware/lib/microlink/PORTING.md", "VPN: porting di MicroLink", "Firmware"),
    ("nimble", "firmware/lib/NimBLE-Arduino/DISPLAYHUB.md", "Bluetooth: NimBLE", "Firmware"),
    ("font", "firmware/lib/dh_fonts/README.md", "Font con lettere accentate", "Firmware"),
    ("gateway", "firmware/gateway/README.md", "Gateway Bluetooth", "Firmware"),
]
_BY_ID = {d[0]: d for d in DOCS}
_BY_PATH = {d[1]: d[0] for d in DOCS}


def _abs(rel: str) -> str:
    return os.path.join(_ROOT, rel)


def list_docs() -> List[Dict[str, Any]]:
    out = []
    for doc_id, rel, title, group in DOCS:
        path = _abs(rel)
        if os.path.isfile(path):
            out.append({"id": doc_id, "title": title, "group": group, "path": rel,
                        "updated_at": os.path.getmtime(path)})
    return out


def _rewrite_links(html: str, rel: str) -> str:
    """href verso un altro documento dell'elenco -> #/documentazione/<id>."""
    base = posixpath.dirname(rel)

    def fix(m):
        href = m.group(2)
        if re.match(r"^[a-z]+:", href) or href.startswith("#"):
            return m.group(0)
        target, _, anchor = href.partition("#")
        norm = posixpath.normpath(posixpath.join(base, target))
        doc_id = _BY_PATH.get(norm) or _BY_PATH.get(target)
        if doc_id:
            return f'{m.group(1)}"#/documentazione/{doc_id}"'
        return m.group(0)

    return re.sub(r'(href=)"([^"]+)"', fix, html)


def render(doc_id: str) -> Optional[Dict[str, Any]]:
    doc = _BY_ID.get(doc_id)
    if not doc:
        return None
    _, rel, title, group = doc
    path = _abs(rel)
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as f:
        text = f.read()
    md = markdown.Markdown(extensions=["tables", "fenced_code", "toc", "sane_lists"],
                           extension_configs={"toc": {"toc_depth": "2-3"}})
    html = _rewrite_links(md.convert(text), rel)
    return {"id": doc_id, "title": title, "group": group, "path": rel, "html": html,
            "toc": getattr(md, "toc", ""), "updated_at": os.path.getmtime(path)}


def render_markdown(text: str) -> str:
    """Note del registro versioni: stesso Markdown, senza HTML scritto a mano
    (le note sono testo inserito dalla web UI)."""
    from html import escape
    html = markdown.markdown(escape(text or ""), extensions=["tables", "fenced_code", "sane_lists"])
    # Nei blocchi di codice Markdown ri-scappa ciò che era già scappato: si torna
    # a un escape solo (resta sicuro: nessun "<" vero può ricomparire).
    for a, b in (("&amp;lt;", "&lt;"), ("&amp;gt;", "&gt;"), ("&amp;quot;", "&quot;"), ("&amp;#x27;", "&#x27;"),
                 ("&amp;amp;", "&amp;")):
        html = html.replace(a, b)
    return html
