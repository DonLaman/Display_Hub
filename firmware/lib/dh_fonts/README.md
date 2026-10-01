# dh_fonts

Montserrat Medium 14/20/48 per LVGL 8 con le lettere accentate (i font inclusi
in LVGL hanno solo ASCII). Generati con le sorgenti ufficiali di LVGL 8.3.11
(`scripts/built_in_font/`: Montserrat-Medium.ttf e FontAwesome5 per le icone).

```sh
npm i lv_font_conv@1.5.2
SYMS=<elenco "syms" di lvgl/scripts/built_in_font/built_in_font_gen.py>
RANGE="0x20-0x7E,0xA0-0xFF,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x20AC"
for n in 14 20; do
  npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size $n --font Montserrat-Medium.ttf -r $RANGE \
    --font FontAwesome5-Solid+Brands+Regular.woff -r $SYMS --format lvgl --lv-include lvgl.h \
    --force-fast-kern-format -o dh_font_$n.c
done
npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size 48 --font Montserrat-Medium.ttf -r $RANGE \
  --format lvgl --lv-include lvgl.h --force-fast-kern-format -o dh_font_48.c
```
Per aggiungere caratteri: allargare RANGE e rigenerare.
