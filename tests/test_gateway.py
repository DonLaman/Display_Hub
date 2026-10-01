"""Gateway Bluetooth: build (chip e indirizzo del bootloader), archivio,
manifest di flashing. Esecuzione: python3 tests/test_gateway.py"""
import os
import sys
import tempfile
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from app.core import docs, firmware_builder as fb, firmware_store as fs  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


tmp = tempfile.mkdtemp()
fs._FIRMWARE_DIR = os.path.join(tmp, "fw")
fs._LOGS_DIR = os.path.join(fs._FIRMWARE_DIR, "logs")
fs._ELF_DIR = os.path.join(fs._FIRMWARE_DIR, "elf")
fs._META_DIR = os.path.join(fs._FIRMWARE_DIR, "meta")
os.makedirs(fs._FIRMWARE_DIR)

print("== Build ==")
name = fb._build_merged_filename("gateway", {})
check(name.endswith("_gateway.bin") and name.split("_")[0].isdigit(), "nome del file: %s" % name)
proj = os.path.join(tmp, "gateway")
out = os.path.join(proj, ".pio", "build", "esp-wrover-kit")
calls = []


def fake_run(job_id, cmd, cwd=None, extra_env=None):
    calls.append((cmd, cwd))
    if cmd[:2] == ["pio", "run"]:  # la build "produce" i file
        os.makedirs(out, exist_ok=True)
        for f in ("bootloader.bin", "partitions.bin", "firmware.bin", "firmware.elf"):
            open(os.path.join(out, f), "wb").write(b"x")
    if "merge_bin" in cmd:
        open(cmd[cmd.index("-o") + 1], "wb").write(b"merged")
    return True


with mock.patch.object(fb, "_GATEWAY_ROOT", proj), mock.patch.object(fb, "_run_subprocess", side_effect=fake_run), \
     mock.patch.object(fb, "_append_log"), mock.patch.object(fb, "_finish") as fin:
    fb._run_build("job1", "gateway", {})
run = [c for c, _ in calls if c[:2] == ["pio", "run"]]
check(run and run[0] == ["pio", "run", "-e", "esp-wrover-kit"] and calls[0][1] == proj, "pio run -e esp-wrover-kit nel progetto del gateway")
check(not any(c[:3] == ["pio", "pkg", "install"] for c, _ in calls), "niente passaggi specifici di LVGL")
merge = next(c for c, _ in calls if "merge_bin" in c)
check(merge[merge.index("--chip") + 1] == "esp32", "merge_bin con --chip esp32")
check(merge[merge.index("-o") + 2] == "0x1000" and merge[merge.index("-o") + 3].endswith("bootloader.bin"),
      "bootloader a 0x1000 (ESP32 classico)")
check(merge[-4] == "0x8000" and merge[-2] == "0x10000", "partizioni 0x8000, app 0x10000")
check(fin.call_args and fin.call_args.kwargs.get("success", fin.call_args.args[-1] if fin.call_args.args else None) in (True,),
      "build conclusa con successo")
lst = fs.list_firmware_files()
check(len(lst) == 1 and lst[0]["kind"] == "gateway" and lst[0]["has_elf"], "archivio: tipo gateway, ELF conservato")
check(lst[0]["meta"]["target"] == "gateway" and lst[0]["meta"]["chip"] == "ESP32" and lst[0]["meta"]["source"] == "build",
      "dati della versione: gateway, ESP32")
check(fs._meta_from_filename("1790000000_gateway.bin")["target"] == "gateway", "dal nome del file: gateway")

# lo stesso comando per l'S3 non cambia
calls.clear()
proj2 = os.path.join(tmp, "loader-full")
out = os.path.join(proj2, ".pio", "build", fb._LOADER_FULL_BUILD_ENV)
with mock.patch.object(fb, "_LOADER_FULL_ROOT", proj2), mock.patch.object(fb, "_run_subprocess", side_effect=fake_run), \
     mock.patch.object(fb, "_append_log"), mock.patch.object(fb, "_finish"), mock.patch.object(fb, "_patch_lvgl_pragma"):
    fb._run_build("job2", "loader_full", {"config_sd": True})
merge = next(c for c, _ in calls if "merge_bin" in c)
check(merge[merge.index("--chip") + 1] == "esp32s3" and merge[merge.index("-o") + 2] == "0x0", "S3 invariato: esp32s3, bootloader a 0x0")

print("\n== API ==")
from app.main import create_app  # noqa: E402

c = create_app().test_client()
gw = lst[0]["filename"]
m = c.get(f"/api/firmware/{gw}/manifest.json").get_json()
check(m["builds"][0]["chipFamily"] == "ESP32", "manifest del gateway: chipFamily ESP32")
lf = [f["filename"] for f in fs.list_firmware_files() if f["kind"] == "loader_full"][0]
check(c.get(f"/api/firmware/{lf}/manifest.json").get_json()["builds"][0]["chipFamily"] == "ESP32-S3", "manifest del loader: ESP32-S3")
with mock.patch.object(fb, "start_gateway_build", return_value="abc"):
    r = c.post("/api/firmware/build-gateway")
check(r.status_code == 200 and r.get_json()["job_id"] == "abc", "POST /api/firmware/build-gateway")
check(c.post("/api/firmware/clear-cache", json={"target": "gateway"}).status_code != 400, "pulizia cache: target gateway ammesso")
page = c.get("/").get_data(as_text=True)
check('id="build-gateway-btn"' in page and 'data-kind="gateway"' in page, "pagina: sezione gateway e decodifica crash")
check(any(d["id"] == "gateway" for d in docs.list_docs()) and "<table>" in docs.render("gateway")["html"], "documentazione del gateway")

print("\n== Nome del gateway alla compilazione ==")
import hashlib  # noqa: E402
from app.core import bt_gateway  # noqa: E402
real_header = os.path.join(os.path.dirname(__file__), "..", "firmware", "gateway", "include", "gateway_build.h")
h_before = hashlib.md5(open(real_header, "rb").read()).hexdigest()
check(fb.gateway_identity("") == ("DH-Gateway", "gateway-01"), "senza nome: predefiniti DH-Gateway / gateway-01")
check(fb.gateway_identity("Salotto") == ("Salotto", "salotto") and fb.gateway_identity("Gateway Salotto")[1] == "gateway-salotto" and fb.gateway_identity("Camera_1 b")[1] == "camera-1-b",
      "l'id sul server deriva dal nome (minuscolo, spazi e _ diventano -)")
check(all(bt_gateway.valid_device_id(fb.gateway_identity(n)[1]) for n in ("Salotto", "Gateway Salotto", "A", "x" * 24)), "l'id ricavato e' sempre un id valido per il server")
bad = 0
for n in ('a"b', "-x", "x" * 25, "caff\u00e8", "a;b", "a\\b", "a/b", "../x"):
    try:
        fb.gateway_identity(n)
    except ValueError:
        bad += 1
check(bad == 8, "nomi non validi rifiutati (virgolette, accenti, ; \\ /, troppo lungo...): %d su 8" % bad)
calls.clear()
out = os.path.join(proj, ".pio", "build", "esp-wrover-kit")   # il finto pio scrive qui (la sezione S3 l'aveva spostato)
with mock.patch.object(fb, "_GATEWAY_ROOT", proj), mock.patch.object(fb, "_run_subprocess", side_effect=fake_run), \
     mock.patch.object(fb, "_append_log"), mock.patch.object(fb, "_finish") as fin2:
    fb._run_build("job3", "gateway", {"gateway_name": "Gateway Salotto"})
check(fin2.call_args.kwargs.get("success") is True, "build con nome conclusa con successo")
hdr = open(os.path.join(proj, "include", "gateway_build.h")).read()
check('#define GW_NAME "Gateway Salotto"' in hdr and '#define GW_DEVICE_ID "gateway-salotto"' in hdr, "build con nome: gateway_build.h scritto con nome e id")
named = [f for f in fs.list_firmware_files() if f["filename"].endswith("_gateway-gateway-salotto.bin")]
check(len(named) == 1 and named[0]["kind"] == "gateway" and named[0]["meta"].get("chip") == "ESP32", "il file porta l'id nel nome e resta di tipo gateway (ESP32)")
check(fs._meta_from_filename(named[0]["filename"]).get("gateway_id") == "gateway-salotto", "dal nome del file si ricava l'id")
check(fs.is_gateway_file("1790000000_gateway.bin") and fs.is_gateway_file("1790000000_gateway-salotto.bin") and not fs.is_gateway_file("1790000000_loader.bin"),
      "riconoscimento dei file gateway (con e senza nome)")
check(not fs.is_gateway_file("wifi-20260929-3.2.0_gateway.bin"), "un firmware del display con 'gateway' nell'id NON e' scambiato per un gateway")
check(c.get(f"/api/firmware/{named[0]['filename']}/manifest.json").get_json()["builds"][0]["chipFamily"] == "ESP32", "manifest del gateway con nome: chipFamily ESP32")
with mock.patch.object(fb, "start_gateway_build", return_value="abc") as m:
    r = c.post("/api/firmware/build-gateway", json={"name": "Salotto"})
check(r.status_code == 200 and m.call_args.args == ("Salotto",), "POST build-gateway passa il nome al builder")
r = c.post("/api/firmware/build-gateway", json={"name": 'a"b'})
check(r.status_code == 400 and "non valido" in r.get_json()["error"], "nome non valido: 400 con il motivo, nessuna build avviata")
check('id="gateway-name-input"' in c.get("/").get_data(as_text=True), "pagina: campo 'Nome del gateway'")
check(hashlib.md5(open(real_header, "rb").read()).hexdigest() == h_before, "i test non toccano il gateway_build.h vero del progetto")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
