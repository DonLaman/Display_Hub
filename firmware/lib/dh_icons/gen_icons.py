#!/usr/bin/env python3
"""Genera le icone del telecomando (src/dh_icons.c/.h) come immagini LVGL
LV_IMG_CF_TRUE_COLOR_ALPHA a 16 bit: per pixel 3 byte = colore RGB565 (little-endian,
sempre bianco) + alpha. L'alpha rende lo sfondo TRASPARENTE (il colore del bottone si
vede) e i bordi sono morbidi perche' si disegna a 4x e si riduce.
Uso: python3 gen_icons.py   (richiede Pillow)"""
import os
from PIL import Image, ImageDraw

N = 40           # lato dell'icona in pixel
S = 4            # supersampling
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "src")


def canvas():
    img = Image.new("L", (N * S, N * S), 0)      # maschera alpha: 255 = pieno
    return img, ImageDraw.Draw(img)


def P(pts):
    return [(x * S, y * S) for x, y in pts]


def line(d, pts, w):
    d.line(P(pts), fill=255, width=int(w * S))


def poly(d, pts):
    d.polygon(P(pts), fill=255)


def speaker(d):
    poly(d, [(6, 16), (6, 24), (12, 24), (18, 30), (18, 10), (12, 16)])


icons = {}
img, d = canvas()                                         # power
d.arc([8 * S, 9 * S, 32 * S, 33 * S], start=300, end=240, fill=255, width=4 * S)
line(d, [(20, 5), (20, 20)], 4)
icons["power"] = img
img, d = canvas(); poly(d, [(20, 9), (31, 28), (9, 28)]); icons["up"] = img
img, d = canvas(); poly(d, [(20, 31), (31, 12), (9, 12)]); icons["down"] = img
img, d = canvas(); poly(d, [(11, 20), (29, 9), (29, 31)]); icons["left"] = img
img, d = canvas(); poly(d, [(29, 20), (11, 9), (11, 31)]); icons["right"] = img
img, d = canvas(); speaker(d); line(d, [(23, 20), (33, 20)], 3); line(d, [(28, 15), (28, 25)], 3); icons["vol_up"] = img
img, d = canvas(); speaker(d); line(d, [(23, 20), (33, 20)], 3); icons["vol_down"] = img
img, d = canvas(); speaker(d); line(d, [(23, 15), (33, 25)], 3); line(d, [(23, 25), (33, 15)], 3); icons["mute"] = img
img, d = canvas(); line(d, [(9, 21), (17, 29), (31, 11)], 4); icons["ok"] = img
img, d = canvas(); poly(d, [(20, 7), (33, 20), (28, 20), (28, 32), (12, 32), (12, 20), (7, 20)])
d.rectangle([17 * S, 23 * S, 23 * S, 32 * S], fill=0)   # porta: cosi' si legge come una casa
icons["home"] = img
img, d = canvas()                                         # menu
for y in (13, 20, 27):
    line(d, [(9, y), (31, y)], 4)
icons["menu"] = img
img, d = canvas()                                         # indietro
d.arc([9 * S, 9 * S, 31 * S, 31 * S], start=120, end=360, fill=255, width=4 * S)
poly(d, [(9, 13), (18, 13), (13, 22)])
icons["back"] = img
img, d = canvas()                                         # tastierino: griglia 3x3
for r in range(3):
    for c in range(3):
        x, y = 12 + c * 8, 12 + r * 8
        d.ellipse([(x - 2.5) * S, (y - 2.5) * S, (x + 2.5) * S, (y + 2.5) * S], fill=255)
icons["keypad"] = img
img, d = canvas(); poly(d, [(5, 22), (23, 22), (14, 11)]); line(d, [(26, 17), (36, 17)], 3); line(d, [(31, 12), (31, 22)], 3); icons["ch_up"] = img
img, d = canvas(); poly(d, [(5, 18), (23, 18), (14, 29)]); line(d, [(26, 24), (36, 24)], 3); icons["ch_down"] = img


# ---- icone del player audio ----
img, d = canvas(); d.rectangle([8 * S, 10 * S, 12 * S, 30 * S], fill=255); poly(d, [(31, 10), (14, 20), (31, 30)]); icons["prev"] = img
img, d = canvas(); poly(d, [(9, 10), (26, 20), (9, 30)]); d.rectangle([28 * S, 10 * S, 32 * S, 30 * S], fill=255); icons["next"] = img
img, d = canvas()                                         # play/pausa: triangolo + due barre
poly(d, [(5, 9), (5, 31), (21, 20)])
d.rectangle([25 * S, 10 * S, 28 * S, 30 * S], fill=255)
d.rectangle([31 * S, 10 * S, 34 * S, 30 * S], fill=255)
icons["play_pause"] = img
img, d = canvas(); d.rectangle([11 * S, 11 * S, 29 * S, 29 * S], fill=255); icons["stop"] = img
img, d = canvas()                                         # nota musicale (MP3)
d.ellipse([9 * S, 23 * S, 20 * S, 32 * S], fill=255)
line(d, [(19, 9), (19, 28)], 3)
poly(d, [(19, 9), (31, 14), (31, 19), (19, 15)])
icons["music"] = img
img, d = canvas()                                         # YouTube: rettangolo arrotondato con triangolo vuoto
d.rounded_rectangle([4 * S, 9 * S, 36 * S, 31 * S], radius=7 * S, fill=255)
poly_cut = P([(16, 14), (16, 26), (27, 20)])
d.polygon(poly_cut, fill=0)
icons["youtube"] = img
img, d = canvas()                                         # web radio: punto + onde
d.ellipse([17 * S, 25 * S, 23 * S, 31 * S], fill=255)
for r in (7, 12, 17):
    d.arc([(20 - r) * S, (28 - r) * S, (20 + r) * S, (28 + r) * S], start=215, end=325, fill=255, width=3 * S)
icons["radio"] = img


def to_bytes(mask):
    small = mask.resize((N, N), Image.LANCZOS)
    out = bytearray()
    for y in range(N):
        for x in range(N):
            out += bytes((0xFF, 0xFF, small.getpixel((x, y))))   # RGB565 bianco (LE) + alpha
    return out


lines = ["// Icone telecomando GENERATE da gen_icons.py (TRUE_COLOR_ALPHA 16 bit). NON modificare a mano.",
         '#include "lvgl.h"', ""]
for name, mask in icons.items():
    data = to_bytes(mask)
    lines.append(f"static const uint8_t dhicon_{name}_map[] = {{{','.join(str(b) for b in data)}}};")
    lines.append(f"const lv_img_dsc_t dhicon_{name} = {{")
    lines.append(f"  .header = {{.cf = LV_IMG_CF_TRUE_COLOR_ALPHA, .always_zero = 0, .reserved = 0, .w = {N}, .h = {N}}},")
    lines.append(f"  .data_size = {len(data)}, .data = dhicon_{name}_map }};")
    lines.append("")
open(os.path.join(OUT, "dh_icons.c"), "w").write("\n".join(lines))
open(os.path.join(OUT, "dh_icons.h"), "w").write(
    "#pragma once\n#include \"lvgl.h\"\n\n" + "\n".join(f"extern const lv_img_dsc_t dhicon_{n};" for n in icons) + "\n")

# anteprima ingrandita su sfondi colorati (per controllo a occhio)
sheet = Image.new("RGB", (8 * 100, 3 * 100), (20, 20, 24))
for i, (name, mask) in enumerate(icons.items()):
    bg = (211, 47, 47) if name in ("power", "youtube") else ((45, 125, 210) if name in ("up", "down", "left", "right") else ((31, 165, 98) if name == "play_pause" else (60, 66, 80)))
    tile = Image.new("RGB", (N, N), bg)
    tile.paste((255, 255, 255), (0, 0), mask.resize((N, N), Image.LANCZOS))
    sheet.paste(tile.resize((96, 96), Image.NEAREST), ((i % 8) * 100 + 2, (i // 8) * 100 + 2))
sheet.save("/tmp/icons_preview.png")
print("icone:", len(icons), "| dimensione dh_icons.c:", os.path.getsize(os.path.join(OUT, "dh_icons.c")) // 1024, "KB")
