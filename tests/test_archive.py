"""Archivio firmware (flag della versione, cancellazione completa, log orfani)
e dispositivi da dimenticare. Esecuzione: python3 tests/test_archive.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import device_config, device_registry, firmware_builder, firmware_store as fs  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


tmp = tempfile.mkdtemp()
fs._FIRMWARE_DIR = tmp
fs._LOGS_DIR = os.path.join(tmp, "logs")
fs._ELF_DIR = os.path.join(tmp, "elf")
fs._META_DIR = os.path.join(tmp, "meta")
device_config._DIR = os.path.join(tmp, "devices")


def make(name, elf=True, log=True):
    open(os.path.join(tmp, name), "wb").write(b"x" * 1000)
    base = name[:-4]
    if elf:
        os.makedirs(fs._ELF_DIR, exist_ok=True)
        open(os.path.join(fs._ELF_DIR, base + ".elf"), "w").write("e")
    if log:
        os.makedirs(fs._LOGS_DIR, exist_ok=True)
        open(os.path.join(fs._LOGS_DIR, base + ".log"), "w").write("log di " + base)


print("== Flag della versione ==")
params = {"comm_mode": "wifi_vpn", "config_storage_sd": True, "device_id": "Dash_Final", "fw_version": "3.2.0",
          "fw_revision": "rev_7", "server_host": "192.168.1.40", "server_port": 12000, "wifi_ssid": "Casa",
          "wifi_password": "segreta", "vpn_auth_key": "tskey-auth-kSEGRETA", "vpn_hostname": "display-hub-01"}
meta = firmware_builder._build_meta("main", params)
check(meta["comm_mode"] == "wifi_vpn" and meta["config_storage_sd"] and meta["has_vpn_auth_key"] and meta["has_wifi_password"],
      "modalità, microSD, presenza di password e chiave")
check("segreta" not in str(meta) and "SEGRETA" not in str(meta), "password e chiavi NON salvate")
make("wifi_vpn-sd-20260927-3.2.0-rev_7_Dash_Final.bin")
fs.save_meta("wifi_vpn-sd-20260927-3.2.0-rev_7_Dash_Final.bin", meta)
make("wifi-20260920-3.1.0_Display_Master.bin", elf=False)
make("1790000000_loaderfull-sd.bin", log=False)
make("1790000001_prova.bin", elf=False, log=False)
lst = {f["filename"]: f for f in fs.list_firmware_files()}
check(lst["wifi_vpn-sd-20260927-3.2.0-rev_7_Dash_Final.bin"]["meta"]["source"] == "build", "firmware nuovo: dati della build")
old = lst["wifi-20260920-3.1.0_Display_Master.bin"]["meta"]
check(old["comm_mode"] == "wifi" and old["fw_version"] == "3.1.0" and old["device_id"] == "Display_Master"
      and not old["config_storage_sd"] and old["source"] == "nome del file", "firmware vecchio: dati dal nome (%s)" % old)
lf = lst["1790000000_loaderfull-sd.bin"]["meta"]
check(lf["target"] == "loader_full" and lf["config_sd"] and lf["built_at"] == 1790000000, "loader con schermo microSD dal nome")
check(lst["1790000001_prova.bin"]["meta"]["target"] == "sconosciuto", "caricato a mano: tipo sconosciuto")
for name, exp in (("wifi_vpn-sd-20260927-3.2.0-rev_7_Dash_Final.bin", ("wifi_vpn", True, "3.2.0", "rev_7", "Dash_Final")),
                  ("usb-20260101-2.0.0-r1_display.bin", ("usb", False, "2.0.0", "r1", "display")),
                  ("wifi-20260101-3.1.0_Display_HUB_01.bin", ("wifi", False, "3.1.0", "", "Display_HUB_01")),
                  ("wifi-20260101-beta-x_d.bin", ("wifi", False, "beta", "x", "d"))):
    m = fs._meta_from_filename(name)
    got = (m["comm_mode"], m["config_storage_sd"], m["fw_version"], m["fw_revision"], m["device_id"])
    check(got == exp, "dal nome %s -> %s" % (name, got))
check(lst["wifi-20260920-3.1.0_Display_Master.bin"]["has_log"] and not lst["1790000000_loaderfull-sd.bin"]["has_log"], "presenza del log")

print("\n== Cancellazione ==")
name = "wifi_vpn-sd-20260927-3.2.0-rev_7_Dash_Final.bin"
fs.delete_firmware(name)
gone = [p for p in (os.path.join(tmp, name), fs.resolve_elf_path(name), fs.resolve_log_path(name), fs.resolve_meta_path(name)) if os.path.exists(p)]
check(not gone, "cancellati .bin, ELF, log e dati della versione")
open(os.path.join(fs._LOGS_DIR, "wifi-20260101-1.0.0_x.log"), "w").write("build fallita")
orph = fs.list_orphan_logs()
check([o["name"] for o in orph] == ["wifi-20260101-1.0.0_x.log"], "log orfani: solo quello senza firmware")
check(fs.delete_orphan_logs() == 1 and not fs.list_orphan_logs(), "log orfani cancellati")
check(os.path.exists(fs.resolve_log_path("wifi-20260920-3.1.0_Display_Master.bin")), "i log dei firmware presenti restano")

print("\n== API ==")
from app.main import create_app  # noqa: E402

c = create_app().test_client()
open(os.path.join(fs._LOGS_DIR, "orfano.log"), "w").write("x")
r = c.get("/api/firmware/orphan-logs")
check(r.status_code == 200 and [o["name"] for o in r.get_json()] == ["orfano.log"], "elenco log orfani")
r = c.delete("/api/firmware/orphan-logs")
check(r.get_json()["deleted"] == 1, "DELETE log orfani (non scambiato per un firmware)")
r = c.get("/api/firmware/wifi-20260920-3.1.0_Display_Master.bin/log")
check(r.status_code == 200 and b"log di" in r.data, "lettura del log di un firmware")
check(c.get("/api/firmware/nonesiste.bin/log").status_code == 404, "log mancante: 404")
check(c.get("/api/firmware/..%2F..%2Fetc%2Fpasswd/log").status_code in (404, 400), "percorsi strani rifiutati")
r = c.post("/api/firmware/delete-many", json={"filenames": ["wifi-20260920-3.1.0_Display_Master.bin", "1790000000_loaderfull-sd.bin", "nonesiste.bin"]})
check(r.get_json()["deleted"] == 2 and len(fs.list_firmware_files()) == 1, "cancellazione multipla")

device_registry.register("Display_Master", "192.168.1.65", "3.2.0")
device_registry.register("Display_HUB_01", "192.168.1.66", "3.1.0")
device_config.sync_from_device("Display_HUB_01", 0, "[server]\nhost = 1.1.1.1\n")
device_config.sync_from_device("Vecchio_01", 0, "[server]\nhost = 1.1.1.1\n")
devs = {d["device_id"]: d for d in c.get("/api/devices").get_json()}
check(set(devs) == {"Display_Master", "Display_HUB_01", "Vecchio_01"} and devs["Vecchio_01"]["known_only"], "elenco: visti + solo configurazione salvata")
check(devs["Display_HUB_01"]["has_config"] and not devs["Display_Master"]["has_config"], "indicazione della configurazione salvata")
c.delete("/api/devices/Display_HUB_01")
devs = {d["device_id"]: d for d in c.get("/api/devices").get_json()}
check("Display_HUB_01" in devs and devs["Display_HUB_01"].get("known_only"), "dimenticato: resta solo la configurazione salvata")
r = c.delete("/api/devices/Display_HUB_01?config=1")
check(r.get_json()["config_removed"] and "Display_HUB_01" not in {d["device_id"] for d in c.get("/api/devices").get_json()},
      "dimenticato con la configurazione: sparito del tutto")
check(c.delete("/api/devices/..%2Fx").status_code in (400, 404), "id non valido rifiutato")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
