"""Widget telecomando TV: coerenza tra server e firmware (stili, colori, icone) e griglia.
Esecuzione: python3 tests/test_tv_remote_widget.py"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(__file__), "..")
sys.path.insert(0, ROOT)
from app.widgets.tv_remote import TvRemoteWidget  # noqa: E402

ok = fail = 0


def check(c, m):
    global ok, fail
    ok, fail = (ok + 1, fail) if c else (ok, fail + 1)
    print(("OK  " if c else "FAIL"), m)


fw = open(os.path.join(ROOT, "firmware", "src", "main.cpp"), encoding="utf-8").read()
icons_h = open(os.path.join(ROOT, "firmware", "lib", "dh_icons", "src", "dh_icons.h")).read()
fn = fw[fw.index("static lv_color_t btn_style_color"):]
fn = fn[:fn.index("\n}\n")]
colors = {m.group(1): int(m.group(2), 16) for m in re.finditer(r'style == "(\w+)"\)\s+return lv_color_hex\((0x[0-9A-Fa-f]+)\)', fn)}
default_color = int(re.search(r"return lv_color_hex\((0x[0-9A-Fa-f]+)\);\s+//\s+tutti gli altri", fn).group(1), 16)
icon_fn = fw[fw.index("static const lv_img_dsc_t* icon_by_name"):]
icon_fn = icon_fn[:icon_fn.index("\n}\n")]
fw_icons = set(re.findall(r'name == "(\w+)"', icon_fn))
h_icons = set(re.findall(r"dhicon_(\w+);", icons_h))

data = TvRemoteWidget().get_data()
main = data["views"]["main"]["buttons"]
keypad = data["views"]["keypad"]["buttons"]
allb = main + keypad
key = lambda b: b["action"].split(":", 1)[1]

print("== Colori ==")
arrows = {key(b) for b in allb if b.get("style") == "arrow"}
check(arrows == {"KEY_UP", "KEY_DOWN", "KEY_LEFT", "KEY_RIGHT"}, "'arrow' solo sulle 4 frecce: %s" % sorted(arrows))
blue = [s for s, c in colors.items() if c == 0x2D7DD2]
check(blue == ["arrow"] and default_color != 0x2D7DD2, "il blu nel firmware e' assegnato solo a 'arrow'")
check(not any(b.get("style") == "accent" for b in allb), "nessun bottone usa piu' 'accent' (blu)")
power = [b for b in main if b["action"] == "power:toggle"]
check(len(power) == 1 and power[0]["style"] == "power_circle" and colors["power_circle"] == 0xD32F2F, "accensione: 'power_circle', rosso (0xD32F2F)")
enter = [b for b in main if b["action"] == "view:keypad"]
check(len(enter) == 1 and enter[0]["style"] == "channel", "ingresso al tastierino: stile 'channel'")
distinct = {colors["arrow"], colors["power_circle"], colors["channel"], default_color}
check(len(distinct) == 4, "frecce, accensione, tastierino e bottoni normali hanno 4 colori diversi")
used = {b["style"] for b in allb if b.get("style")}
check(used <= set(colors), "ogni stile usato dal widget ha un colore nel firmware (mancanti: %s)" % (sorted(used - set(colors)) or "nessuno"))
check(any(b.get("style") == "confirm" for b in keypad), "OK del tastierino: 'confirm'")

print("\n== Icone ==")
names = {b["icon"] for b in allb if b.get("icon")}
check(names <= fw_icons, "ogni icona usata e' mappata nel firmware (mancanti: %s)" % (sorted(names - fw_icons) or "nessuna"))
check(fw_icons <= h_icons, "ogni icona mappata esiste in dh_icons.h")
check(power[0].get("icon") == "power", "l'accensione ha l'icona power")
check(all(len(b["label"]) <= 9 for b in allb if b.get("icon") and b.get("label") and b.get("colspan", 1) == 1),
      "etichette accanto a un'icona di max 9 caratteri (stanno nel bottone stretto)")

print("\n== Griglia ==")
for name, buttons in (("main", main), ("keypad", keypad)):
    cols = data["views"][name]["columns"]
    cells, outside, overlap = set(), 0, 0
    for b in buttons:
        span = b.get("colspan", 1)
        if b["col"] < 0 or b["col"] + span > cols:
            outside += 1
        for c in range(b["col"], b["col"] + span):
            if (b["row"], c) in cells:
                overlap += 1
            cells.add((b["row"], c))
    check(outside == 0 and overlap == 0, "vista %s (%d colonne): nessun bottone fuori griglia o sovrapposto" % (name, cols))
cols = data["views"]["main"]["columns"]
check(power[0]["row"] == 0 and power[0]["col"] + power[0].get("colspan", 1) / 2 == cols / 2, "accensione al centro della sua riga")

print("\n%d ok, %d falliti" % (ok, fail))
sys.exit(1 if fail else 0)
