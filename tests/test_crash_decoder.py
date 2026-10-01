"""Test della decodifica dei crash con un ELF vero e il vero addr2line.
Esecuzione: DH_TEST_ELF=<firmware.elf> DH_ADDR2LINE=<xtensa-esp32s3-elf-addr2line> python3 tests/test_crash_decoder.py"""
import hashlib
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import crash_decoder, firmware_store  # noqa: E402
from app.main import create_app  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


ELF = os.environ["DH_TEST_ELF"]
sha = hashlib.sha256(open(ELF, "rb").read()).hexdigest()[:16]

# Indirizzi presi dai simboli dell'ELF stesso (un punto dentro ogni funzione):
# il test vale per qualunque build, non per una sola.
import subprocess  # noqa: E402
_nm = os.path.join(os.path.dirname(os.environ["DH_ADDR2LINE"]), "xtensa-esp32s3-elf-nm")
_syms = {}
for line in subprocess.run([_nm, "-C", "--defined-only", ELF], capture_output=True, text=True).stdout.splitlines():
    parts = line.split(" ", 2)
    if len(parts) == 3:
        _syms.setdefault(parts[2], int(parts[0], 16))


def addr(name, off=8):
    return "0x%08x" % (_syms[name] + off)


A_WALK = addr("tlsf_walk_pool")
A_INFO = addr("multi_heap_get_info_impl")
A_LARGEST = addr("heap_caps_get_largest_free_block", 4)
A_APP = addr("DhWifi::loop()", 12)
A_REFRESH = addr("DhSettings::refresh()", 12)
A_LOCK = addr("multi_heap_internal_lock", 4)
A_BLE = addr("DhBle::loop()", 12)

# Log con la stessa forma di quello reale (orari della web UI, due core, crash ripetuto, riavvio).
LOG = f"""[09:36:13]Guru Meditation Error: Core  1 panic'ed (Interrupt wdt timeout on CPU1).
[09:36:13]Core  1 register dump:
[09:36:13]PC      : {A_WALK}  PS      : 0x00060b34  A0      : 0x8211e25b  A1      : 0x3fceb9c0
[09:36:13]EXCVADDR: 0x00000000  LBEG    : 0x400570e8  LEND    : 0x400570f3  LCOUNT  : 0x00000000
[09:36:13]Backtrace: {A_WALK}:0x3fceb9c0 {A_INFO}:0x3fceb9e0 {A_LARGEST}:0x3fceba20 {A_APP}:0x3fceba60 {A_REFRESH}:0x3fceba90
[09:36:13]Core  0 register dump:
[09:36:13]PC      : {A_LOCK}  PS      : 0x00060934
[09:36:13]Backtrace: {A_LOCK}:0x3fcd3f50 {A_BLE}:0x3fcd3f90
[09:36:13]ELF file SHA256: {sha}
[09:36:13]Guru Meditation Error: Core  1 panic'ed (Interrupt wdt timeout on CPU1).
[09:36:13]Backtrace: {A_WALK}:0x3fceb7d0 0x00040022:0x3fceb9c0 |<-CORRUPTED
[09:36:13]Re-entered core dump! Exception happened during core dump!
[09:36:13]Saved PC:{A_REFRESH}
"""

with tempfile.TemporaryDirectory() as tmp:
    firmware_store._FIRMWARE_DIR = tmp
    firmware_store._ELF_DIR = os.path.join(tmp, "elf")
    crash_decoder._BUILD_ELFS = []  # solo quelli conservati
    open(os.path.join(tmp, "wifi_vpn-sd-20260927_sala-01.bin"), "wb").write(b"x")
    firmware_store.save_elf("wifi_vpn-sd-20260927_sala-01.bin", ELF)
    files = firmware_store.list_firmware_files()
    check(files[0]["has_elf"], "l'elenco firmware segnala l'ELF conservato")
    open(os.path.join(tmp, "1790000000_loaderfull-sd.bin"), "wb").write(b"x")
    check({f["filename"]: f["kind"] for f in firmware_store.list_firmware_files()}["1790000000_loaderfull-sd.bin"] == "loader_full",
          "loader con microSD riconosciuto come loader (non come firmware principale)")

    p = crash_decoder.parse_log(LOG)
    titles = [s["title"] for s in p["sections"]]
    check(p["sha"] == sha, "impronta ELF letta dal log")
    check(titles == ["Core 1 - registri", "Core 1 - backtrace", "Core 0 - registri", "Core 0 - backtrace",
                     "Core 1 - backtrace (2)", "Riavvio - PC salvato"], "sezioni: %s" % titles)
    check(p["sections"][4].get("note", "").startswith("backtrace interrotto"), "backtrace corrotto segnalato")

    r = crash_decoder.decode(LOG)
    txt = r["text"]
    print("----\n" + txt + "\n----")
    check(r["firmware"] == "wifi_vpn-sd-20260927_sala-01.bin", "ELF trovato dall'impronta")
    bt = {f["addr"]: f["where"] for f in r["sections"][1]["frames"]}
    check("tlsf_walk_pool" in bt[A_WALK], "frame: tlsf_walk_pool")
    check("multi_heap_get_info_impl" in bt[A_INFO], "frame: multi_heap_get_info_impl")
    check("heap_caps_get_largest_free_block" in bt[A_LARGEST], "frame: heap_caps_get_largest_free_block")
    check("DhWifi::loop" in bt[A_APP] and "dh_wifi/src/DhWifi.cpp:" in bt[A_APP],
          "frame nel codice del progetto con file e riga (loader-full/src/main.cpp)")
    check("multi_heap_internal_lock" in r["sections"][3]["frames"][0]["where"], "core 0: in attesa del blocco dell'heap")
    check("sconosciuto" in r["sections"][4]["frames"][1]["where"], "indirizzo non valido segnalato come tale")
    check("DhSettings::refresh" in r["sections"][5]["frames"][0]["where"], "PC salvato al riavvio decodificato")

    try:
        crash_decoder.decode(LOG.replace(sha, "0123456789abcdef"))
        check(False, "impronta sconosciuta")
    except LookupError as e:
        check("nessun ELF con impronta" in str(e), "impronta sconosciuta: messaggio chiaro")
    r2 = crash_decoder.decode(LOG.replace("ELF file SHA256: " + sha, ""), "wifi_vpn-sd-20260927_sala-01.bin")
    check("tlsf_walk_pool" in r2["text"], "senza impronta: firmware scelto a mano")

    c = create_app().test_client()
    res = c.post("/api/firmware/decode", json={"log": LOG})
    check(res.status_code == 200 and "DhWifi::loop" in res.get_json()["text"], "API /api/firmware/decode")
    check(c.post("/api/firmware/decode", json={"log": "niente"}).status_code == 400, "API: log senza indirizzi -> 400")
    firmware_store.delete_firmware("wifi_vpn-sd-20260927_sala-01.bin")
    check(not os.path.exists(firmware_store.resolve_elf_path("wifi_vpn-sd-20260927_sala-01.bin")), "cancellando il firmware si cancella anche l'ELF")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
