"""
Decodifica dei crash del display ("Guru Meditation Error", watchdog, abort...)
incollati dalla console seriale: traduce gli indirizzi (Backtrace, PC, EXCVADDR,
"Saved PC" del riavvio) in funzione / file / riga, come fa l'ESP Exception
Decoder, usando addr2line della toolchain che PlatformIO ha già nel container.

Quale ELF usare: il firmware stampa "ELF file SHA256: 30ec9a28c6c8b2ab" (inizio
dello SHA-256 del file .elf). Si cerca l'ELF con quell'impronta tra:
  - quelli conservati dal builder accanto a ogni .bin (data/firmware/elf/);
  - quelli dell'ultima build di ciascun progetto (.pio/build/*/firmware.elf).
Se il log non contiene l'impronta si può indicare il firmware a mano.
"""
import glob
import hashlib
import os
import re
import shutil
import subprocess
from typing import Any, Dict, List, Optional, Tuple

from app.core import firmware_store

_FIRMWARE_ROOT = os.path.join(os.path.dirname(__file__), "..", "..", "firmware")
_BUILD_ELFS = [
    os.path.join(_FIRMWARE_ROOT, "*", ".pio", "build", "*", "firmware.elf"),
    os.path.join(_FIRMWARE_ROOT, ".pio", "build", "*", "firmware.elf"),
]

_SHA_RE = re.compile(r"ELF file SHA256:\s*([0-9a-fA-F]{8,64})")
_BT_RE = re.compile(r"Backtrace:(.*)")
_ADDR_PAIR_RE = re.compile(r"(0x[0-9a-fA-F]{8}):0x[0-9a-fA-F]{8}")
_REG_RE = re.compile(r"(?<!Saved )\b(PC|EXCVADDR)\s*:\s*(0x[0-9a-fA-F]{8})")
_SAVED_PC_RE = re.compile(r"Saved PC:\s*(0x[0-9a-fA-F]{8})")
_CORE_RE = re.compile(r"Core\s+(\d)\s+register dump|Guru Meditation Error:\s*Core\s+(\d)")

_sha_cache: Dict[Tuple[str, float, int], str] = {}


def _sha256(path: str) -> str:
    st = os.stat(path)
    key = (path, st.st_mtime, st.st_size)
    if key not in _sha_cache:
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        _sha_cache[key] = h.hexdigest()
    return _sha_cache[key]


def candidate_elfs() -> List[Dict[str, str]]:
    out = [{"path": e["path"], "label": e["firmware"]} for e in firmware_store.list_elfs()]
    for pattern in _BUILD_ELFS:
        for path in glob.glob(pattern):
            rel = os.path.relpath(path, _FIRMWARE_ROOT)
            out.append({"path": path, "label": "ultima build (" + rel.split(os.sep + ".pio")[0] + ")"})
    return out


def find_addr2line() -> Optional[str]:
    env = os.environ.get("DH_ADDR2LINE")
    if env and os.path.exists(env):
        return env
    found = shutil.which("xtensa-esp32s3-elf-addr2line")
    if found:
        return found
    # addr2line legge le informazioni di debug (DWARF): quello dell'S3 decodifica
    # anche gli ELF dell'ESP32 classico (gateway Bluetooth) e viceversa.
    for pattern in (
        os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line"),
        "/root/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-addr2line",
        os.path.expanduser("~/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line"),
        "/root/.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-addr2line",
    ):
        if os.path.exists(pattern):
            return pattern
    return None


def parse_log(text: str) -> Dict[str, Any]:
    """Estrae impronta ELF e indirizzi, divisi per sezione (core / riavvio)."""
    sha = None
    m = _SHA_RE.search(text)
    if m:
        sha = m.group(1).lower()
    sections: List[Dict[str, Any]] = []
    core = None
    seen_titles = set()
    for line in text.splitlines():
        # i log copiati dalla web UI hanno "[09:36:13]" davanti
        line = re.sub(r"^\s*\[\d{1,2}:\d{2}:\d{2}\]", "", line)
        cm = _CORE_RE.search(line)
        if cm:
            core = cm.group(1) or cm.group(2)
        for reg, addr in _REG_RE.findall(line):
            title = f"Core {core} - registri" if core is not None else "Registri"
            _add(sections, title, reg, addr)
        bm = _BT_RE.search(line)
        if bm:
            title = f"Core {core} - backtrace" if core is not None else "Backtrace"
            # stesso crash ripetuto nel log (es. "Re-entered core dump"): numerato
            base, n = title, 2
            while title in seen_titles:
                title = f"{base} ({n})"
                n += 1
            seen_titles.add(title)
            sec = {"title": title, "frames": []}
            for addr in _ADDR_PAIR_RE.findall(bm.group(1)):
                sec["frames"].append({"label": "", "addr": addr.lower()})
            if "CORRUPTED" in bm.group(1):
                sec["note"] = "backtrace interrotto (stack corrotto)"
            sections.append(sec)
        sm = _SAVED_PC_RE.search(line)
        if sm:
            _add(sections, "Riavvio - PC salvato", "Saved PC", sm.group(1))
    return {"sha": sha, "sections": sections}


def _add(sections, title, label, addr):
    for s in sections:
        if s["title"] == title:
            if not any(f["addr"] == addr.lower() and f["label"] == label for f in s["frames"]):
                s["frames"].append({"label": label, "addr": addr.lower()})
            return
    sections.append({"title": title, "frames": [{"label": label, "addr": addr.lower()}]})


def _short_path(p: str) -> str:
    p = p.replace("\\", "/")
    for marker, repl in (("/firmware/", "firmware/"), ("/framework-arduinoespressif32/", "core-arduino/"),
                         ("/.pio/libdeps/", "libdeps/"), ("/esp-idf/components/", "idf/")):
        i = p.find(marker)
        if i >= 0:
            return repl + p[i + len(marker):]
    return p


def _addr2line(tool: str, elf: str, addrs: List[str]) -> Dict[str, str]:
    """{indirizzo: "funzione at file:riga (inlined by ...)"}"""
    out: Dict[str, str] = {}
    if not addrs:
        return out
    res = subprocess.run([tool, "-pfiaC", "-e", elf] + addrs, capture_output=True, text=True, timeout=30)
    current = None
    for line in res.stdout.splitlines():
        m = re.match(r"^(0x[0-9a-fA-F]+):\s*(.*)$", line)
        if m:
            current = "0x%08x" % int(m.group(1), 16)
            out[current] = m.group(2)
        elif current and line.strip().startswith("(inlined by)"):
            out[current] += "  " + line.strip()
    for a, txt in list(out.items()):
        txt = re.sub(r" at (\S+):(\d+)", lambda mm: " at " + _short_path(mm.group(1)) + ":" + mm.group(2), txt)
        txt = re.sub(r"\s*\(discriminator \d+\)", "", txt)
        txt = txt.replace("?? ??:0", "(sconosciuto: non è codice di questo firmware)")
        out[a] = txt
    return out


def decode(text: str, firmware: Optional[str] = None) -> Dict[str, Any]:
    parsed = parse_log(text or "")
    if not parsed["sections"]:
        raise ValueError("nel testo non trovo indirizzi da decodificare (righe 'Backtrace:', 'PC :', 'Saved PC:')")
    tool = find_addr2line()
    if not tool:
        raise RuntimeError("addr2line della toolchain ESP32 non trovato: esegui almeno una build")

    elf, label = None, None
    cands = candidate_elfs()
    if parsed["sha"]:
        for c in cands:
            try:
                if _sha256(c["path"]).startswith(parsed["sha"]):
                    elf, label = c["path"], c["label"]
                    break
            except OSError:
                continue
        if not elf and not firmware:
            raise LookupError(
                f"nessun ELF con impronta {parsed['sha']}: il firmware in crash è stato generato prima "
                "che il builder conservasse gli ELF, oppure non da questo server. Rigeneralo e riprova "
                "con il log del nuovo crash.")
    if not elf and firmware:
        path = firmware_store.resolve_elf_path(firmware)
        if not os.path.exists(path):
            raise LookupError(f"per {firmware} non è stato conservato l'ELF")
        elf, label = path, firmware
    if not elf:
        raise LookupError("il log non contiene 'ELF file SHA256': scegli il firmware a mano")

    addrs = sorted({f["addr"] for s in parsed["sections"] for f in s["frames"] if f["label"] != "EXCVADDR"})
    where = _addr2line(tool, elf, addrs)
    lines = [f"Firmware: {label}" + (f"  (SHA256 {parsed['sha']}...)" if parsed["sha"] else "")]
    for s in parsed["sections"]:
        lines.append("")
        lines.append("== " + s["title"] + (f"  [{s['note']}]" if s.get("note") else ""))
        for f in s["frames"]:
            if f["label"] == "EXCVADDR":
                # indirizzo di MEMORIA a cui l'istruzione ha provato ad accedere, non codice
                f["where"] = ("nessun accesso alla memoria coinvolto" if f["addr"] == "0x00000000"
                              else "indirizzo di memoria a cui si è tentato di accedere (0x0000xxxx = puntatore nullo)")
            else:
                f["where"] = where.get(f["addr"], "")
            lines.append(("%-9s " % (f["label"] + ":") if f["label"] else "") + f"{f['addr']}  {f['where']}")
    return {"firmware": label, "sha": parsed["sha"], "sections": parsed["sections"], "text": "\n".join(lines)}
