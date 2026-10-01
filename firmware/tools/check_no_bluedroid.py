# Controllo di fine build (PlatformIO extra_script "post:"): il firmware NON
# deve contenere lo stack Bluetooth Bluedroid (libreria BLE del core).
# Display Hub usa NimBLE (lib/NimBLE-Arduino, lib/dh_ble): Bluedroid userebbe
# molta più RAM e salverebbe gli accoppiamenti nella NVS. Se una modifica futura
# lo reintroduce (es. un #include <BLEDevice.h>), la build FALLISCE qui invece
# di produrre un firmware che si comporta diversamente senza avvisare.
import subprocess

# Funzioni presenti solo se lo stack host Bluedroid è linkato.
BLUEDROID_SYMBOLS = ("esp_bluedroid_init", "esp_bluedroid_enable", "btc_init", "BTA_EnableBluetooth")


def bluedroid_symbols(nm_output):
    found = []
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1] in BLUEDROID_SYMBOLS:
            found.append(parts[-1])
    return sorted(set(found))


def check_elf(source, target, env):
    elf = str(target[0])
    nm = env.subst("$CC").replace("gcc", "nm")
    out = subprocess.run([nm, elf], capture_output=True, text=True, env=env["ENV"]).stdout
    bad = bluedroid_symbols(out)
    if bad:
        print("\n[Display Hub] ERRORE: nel firmware c'è lo stack Bluetooth Bluedroid (%s)." % ", ".join(bad))
        print("[Display Hub] Usare NimBLE (lib/dh_ble), non la libreria BLE del core (#include <BLEDevice.h>).\n")
        env.Exit(1)
    print("[Display Hub] Controllo Bluetooth: nessuna traccia di Bluedroid.")
    # lv_conf.h letto da LVGL? Se no, LVGL usa il suo font predefinito
    # (lv_font_montserrat_14, senza lettere accentate) invece di dh_font_14.
    names = {line.split()[-1] for line in out.splitlines() if line.strip()}
    if "lv_font_montserrat_14" in names or "dh_font_14" not in names:
        print("[Display Hub] ATTENZIONE: LVGL non ha usato firmware/include/lv_conf.h "
              "(font predefinito senza lettere accentate). Vedi tools/lv_conf_path.py.")
    else:
        print("[Display Hub] Controllo LVGL: lv_conf.h applicato (font con lettere accentate).")


try:
    Import("env")  # noqa: F821 (definito da PlatformIO/SCons)
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", check_elf)  # noqa: F821
except NameError:
    pass  # importato fuori da PlatformIO (test)
