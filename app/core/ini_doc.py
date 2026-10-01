"""
Port Python di firmware/lib/dh_config/src/DhIni.cpp: il file di configurazione
del display (displayhub.txt) letto e modificato con le STESSE regole del
firmware, così server e display non si contraddicono mai:

- sezioni e chiavi senza distinzione maiuscole/minuscole;
- righe "# ..." / "; ..." sono commenti; niente commenti in fondo alla riga
  ('#' e ';' dopo il valore fanno parte del valore: servono nelle password);
- chiavi ripetute = elenchi (es. [wifi] network);
- valori con spazi iniziali/finali o che iniziano con '"' tra virgolette,
  con \\" e \\\\ come uniche sequenze speciali;
- commenti, righe sconosciute e ordine CONSERVATI: si riscrive solo ciò che cambia.
"""
from typing import List, Optional

# Chiavi che sono elenchi (come isListKey() in DhConfigStore.cpp).
LIST_KEYS = {("wifi", "network"), ("bt", "device"), ("bt", "bond")}


def is_list_key(section: str, key: str) -> bool:
    return (section.lower(), key.lower()) in LIST_KEYS


def list_id(entry: str) -> str:
    """Identità di una voce di elenco: testo prima del primo '|', minuscolo."""
    return entry.split("|", 1)[0].strip().lower()


def _parse_value(v: str) -> str:
    v = v.strip()
    if len(v) >= 2 and v[0] == '"' and v[-1] == '"':
        out, i, inner = [], 0, v[1:-1]
        while i < len(inner):
            if inner[i] == "\\" and i + 1 < len(inner) and inner[i + 1] in ('"', "\\"):
                out.append(inner[i + 1])
                i += 2
            else:
                out.append(inner[i])
                i += 1
        return "".join(out)
    return v


def _format_value(v: str) -> str:
    if v and (v[0].isspace() or v[-1].isspace() or v[0] == '"'):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"') + '"'
    return v


class _Line:
    __slots__ = ("raw", "key", "value")

    def __init__(self, raw: str, key: Optional[str] = None, value: str = ""):
        self.raw = raw
        self.key = key      # None = riga non chiave=valore (commento, vuota, sconosciuta)
        self.value = value


class _Section:
    def __init__(self, name: str, header: str = ""):
        self.name = name    # minuscolo; "" = righe prima della prima sezione
        self.header = header
        self.lines: List[_Line] = []


class IniDoc:
    def __init__(self, text: str = ""):
        self._secs: List[_Section] = []
        self.parse(text)

    # ------------------------------------------------------------ lettura/scrittura
    def parse(self, text: str):
        self._secs = [_Section("")]
        lines = text.split("\n")
        if lines and lines[-1] == "":
            lines.pop()  # niente riga vuota finta dopo l'ultimo a capo
        for raw in lines:
            if raw.endswith("\r"):
                raw = raw[:-1]
            t = raw.strip()
            if len(t) >= 2 and t[0] == "[" and t[-1] == "]":
                self._secs.append(_Section(t[1:-1].strip().lower(), raw))
                continue
            eq = t.find("=")
            if t and t[0] not in "#;" and eq > 0:
                self._secs[-1].lines.append(_Line(raw, t[:eq].strip().lower(), _parse_value(t[eq + 1:])))
            else:
                self._secs[-1].lines.append(_Line(raw))

    def serialize(self) -> str:
        out = []
        for i, s in enumerate(self._secs):
            if i > 0 or s.name:
                out.append((s.header or "[" + s.name + "]") + "\n")
            for l in s.lines:
                out.append(l.raw + "\n")
        return "".join(out)

    # ------------------------------------------------------------ accesso
    def _find(self, section: str) -> Optional[_Section]:
        n = section.lower()
        for s in self._secs:
            if s.name == n:
                return s
        return None

    def sections(self) -> List[str]:
        return [s.name for s in self._secs if s.name]

    def keys(self, section: str) -> List[str]:
        s = self._find(section)
        out: List[str] = []
        if s:
            for l in s.lines:
                if l.key is not None and l.key not in out:
                    out.append(l.key)
        return out

    def has(self, section: str, key: str) -> bool:
        return self._has_key(section, key)

    def _has_key(self, section: str, key: str) -> bool:
        s = self._find(section)
        k = key.lower()
        return bool(s) and any(l.key == k for l in s.lines)

    def get(self, section: str, key: str, default: Optional[str] = None) -> Optional[str]:
        vals = self.get_all(section, key)
        return vals[0] if vals else default

    def get_all(self, section: str, key: str) -> List[str]:
        s = self._find(section)
        k = key.lower()
        return [l.value for l in s.lines if l.key == k] if s else []

    # ------------------------------------------------------------ modifica
    def _find_or_create(self, section: str) -> _Section:
        s = self._find(section)
        if s:
            return s
        if not self._secs:
            self._secs.append(_Section(""))
        last = self._secs[-1]
        if not (last.name == "" and not last.lines) and (not last.lines or last.lines[-1].raw.strip()):
            last.lines.append(_Line(""))
        s = _Section(section.lower())
        self._secs.append(s)
        return s

    def set(self, section: str, key: str, value: str):
        self.set_all(section, key, [value])

    def set_all(self, section: str, key: str, values: List[str]):
        if self._has_key(section, key) and self.get_all(section, key) == list(values):
            return
        s = self._find_or_create(section)
        k = key.lower()
        shown = key
        for l in s.lines:
            if l.key == k:
                shown = l.raw.strip().split("=", 1)[0].strip()
                break
        fresh = [_Line(shown + " = " + _format_value(v), k, v) for v in values]
        out, inserted = [], False
        for l in s.lines:
            if l.key == k:
                if not inserted:
                    out.extend(fresh)
                    inserted = True
                continue
            out.append(l)
        if not inserted:
            at = len(out)
            while at > 0 and out[at - 1].key is None and not out[at - 1].raw.strip():
                at -= 1
            out[at:at] = fresh
        s.lines = out

    def remove(self, section: str, key: str):
        s = self._find(section)
        if s:
            k = key.lower()
            s.lines = [l for l in s.lines if l.key != k]

    def remove_section(self, section: str):
        n = section.lower()
        self._secs = [s for s in self._secs if s.name != n]
