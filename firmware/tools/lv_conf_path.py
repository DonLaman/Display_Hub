"""
Script PlatformIO (pre:) — fa trovare DAVVERO firmware/include/lv_conf.h a LVGL.

Problema: i build_flags passano "-I include" (loader: "-I ../include"), un
percorso RELATIVO che vale per i sorgenti del progetto ma non quando PlatformIO
compila LVGL come libreria. LVGL allora non trova lv_conf.h con __has_include e
il suo "#include "lv_conf.h"" fallisce IN SILENZIO (bug GCC 80753, citato in
lv_conf_internal.h): restano i valori predefiniti di LVGL e solo l'avviso
"Possible failure to include lv_conf.h". Finora non si notava perché i valori
importanti arrivano anche come -D; il font predefinito con le lettere
accentate (LV_FONT_DEFAULT) invece esiste solo in lv_conf.h.

Soluzione: aggiungere a TUTTE le compilazioni (librerie comprese: le copiano
dall'ambiente globale, che qui si modifica prima che vengano create) il
percorso ASSOLUTO della cartella di lv_conf.h.
"""
import os

Import("env")  # noqa: F821 (definito da PlatformIO/SCons)

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
for rel in ("include", os.path.join("..", "include")):
    folder = os.path.abspath(os.path.join(project_dir, rel))
    if os.path.isfile(os.path.join(folder, "lv_conf.h")):
        env.Prepend(CPPPATH=[folder])  # noqa: F821
        print("[Display Hub] lv_conf.h per LVGL: %s" % os.path.join(folder, "lv_conf.h"))
        break
else:
    print("[Display Hub] ATTENZIONE: lv_conf.h non trovato in include/ né in ../include/")
