#!/usr/bin/env python3
"""Genera /tmp/layout_gen.h dai DATI VERI dei widget (telecomando TV e player Bluetooth) e dalla
mappa colori VERA del firmware (btn_style_color in src/main.cpp): render.c li disegna con lo
stesso codice del firmware (button_grid_ui.h). Usato da render.sh."""
import os, re, sys
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
sys.path.insert(0, ROOT)
from app.widgets.tv_remote import TvRemoteWidget
from app.widgets.bt_player import BtPlayerWidget

fw = open(os.path.join(ROOT, "firmware/src/main.cpp"), encoding="utf-8").read()
fn = fw[fw.index("static lv_color_t btn_style_color"):]; fn = fn[:fn.index("\n}\n")]
colors = {m.group(1): m.group(2) for m in re.finditer(r'style == "(\w+)"\)\s+return lv_color_hex\((0x[0-9A-Fa-f]+)\)', fn)}
default = re.search(r"return lv_color_hex\((0x[0-9A-Fa-f]+)\);\s+//\s+tutti gli altri", fn).group(1)
icons = re.findall(r"dhicon_(\w+);", open(os.path.join(ROOT, "firmware/lib/dh_icons/src/dh_icons.h")).read())
esc = lambda t: str(t).replace("\\", "\\\\").replace('"', '\\"')

out = ["typedef struct { int row, col, span; const char *icon, *label; unsigned color; int circle, inl; } BtnDef;",
       "typedef struct { int height; const char *badge, *title, *subtitle; int progress; } HdrDef;"]
for prefix, widget, views in (("TV", TvRemoteWidget(), ("main", "keypad")), ("BT", BtPlayerWidget(), ("main", "sources"))):
    data = widget.get_data()
    for view in views:
        v = data["views"][view]
        name = f"{prefix}_{view.upper()}"
        out.append(f"static const BtnDef {name}_BTNS[] = {{")
        for b in v["buttons"]:
            st = b.get("style", "")
            out.append('  {%d, %d, %d, "%s", "%s", %s, %d, %d},' % (b["row"], b["col"], b.get("colspan", 1), b.get("icon", ""),
                       esc(b.get("label", "")), colors.get(st, default), 1 if st.endswith("_circle") else 0, 1 if b.get("inline") else 0))
        out.append("};")
        out.append(f"#define {name}_N {len(v['buttons'])}\n#define {name}_COLS {v['columns']}")
        h = v.get("header")
        if h:
            out.append('static const HdrDef %s_HDR = {%d, "%s", "%s", "%s", %d};' % (name, h["height"], esc(h["badge"]), esc(h["title"]), esc(h["subtitle"]), h["progress"]))
out.append("static const lv_img_dsc_t* icon_by_name(const char* n) {")
for i in icons:
    out.append(f'  if (!strcmp(n, "{i}")) return &dhicon_{i};')
out.append("  return 0;\n}")
open("/tmp/layout_gen.h", "w").write("\n".join(out))
