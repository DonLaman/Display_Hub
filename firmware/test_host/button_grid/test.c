#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "dh_icons.h"
#include "dh_fonts.h"
#include "button_grid_ui.h"

#define W 480
#define H 480
static uint16_t fb[W * H];
static lv_color_t dbuf[W * 40];
static int ok_n = 0, fail_n = 0;
#define CHECK(cond, ...) do { if (cond) { ok_n++; printf("OK   "); } else { fail_n++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static void flush_cb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++) { if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = c->full; c++; }
    lv_disp_flush_ready(d);
}
static void render(void) { for (int i = 0; i < 12; i++) { lv_tick_inc(20); lv_timer_handler(); } }
static lv_obj_t* g_cur = NULL;
static lv_obj_t* new_screen(uint32_t bg) {
    lv_obj_t* s = lv_obj_create(NULL);
    lv_obj_remove_style_all(s);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_color_hex(bg), 0);
    lv_scr_load(s);
    if (g_cur) lv_obj_del(g_cur);
    g_cur = s;
    return s;
}
static lv_obj_t* plain_btn(lv_obj_t* parent, uint32_t color, int x, int y, int w, int h) {
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    return b;
}

/* ---- geometria pura ---- */
static void test_geometry(int columns, int rows) {
    bg_rect_t f = bg_button_rect(bg_cell_rect(W, H, columns, rows, 0, 0, 1));
    bg_rect_t l = bg_button_rect(bg_cell_rect(W, H, columns, rows, 0, columns - 1, 1));
    int left = f.x, right = W - (l.x + l.w);
    CHECK(abs(left - right) <= 1, "%d colonne: margini laterali uguali (sx %d, dx %d)", columns, left, right);
    bg_rect_t t = bg_button_rect(bg_cell_rect(W, H, columns, rows, 0, 0, 1));
    bg_rect_t b = bg_button_rect(bg_cell_rect(W, H, columns, rows, rows - 1, 0, 1));
    int band_top = t.y - BG_TOP_Y, band_bot = H - (b.y + b.h);
    CHECK(band_top >= 10 && band_top <= 16, "%d colonne: fascia libera sopra %d px (10-16)", columns, band_top);
    CHECK(band_bot >= 10 && band_bot <= 16, "%d colonne: fascia libera sotto %d px (10-16)", columns, band_bot);
    int overlap = 0, outside = 0;
    for (int r1 = 0; r1 < rows; r1++) for (int c1 = 0; c1 < columns; c1++) {
        bg_rect_t a = bg_button_rect(bg_cell_rect(W, H, columns, rows, r1, c1, 1));
        if (a.x < 0 || a.y < BG_TOP_Y || a.x + a.w > W || a.y + a.h > H) outside++;
        for (int r2 = 0; r2 < rows; r2++) for (int c2 = 0; c2 < columns; c2++) {
            if (r1 == r2 && c1 == c2) continue;
            bg_rect_t o = bg_button_rect(bg_cell_rect(W, H, columns, rows, r2, c2, 1));
            if (a.x < o.x + o.w && o.x < a.x + a.w && a.y < o.y + o.h && o.y < a.y + a.h) overlap++;
        }
    }
    CHECK(overlap == 0 && outside == 0, "%d colonne: nessuna sovrapposizione, tutto dentro lo schermo", columns);
}

static void test_circle(void) {
    bg_rect_t cell = bg_cell_rect(W, H, 4, 5, 0, 1, 2);      /* power: colonne 1-2 della prima riga */
    bg_rect_t c = bg_circle_rect(cell);
    CHECK(abs((c.x + c.w / 2) - W / 2) <= 1, "power: centrato sull'asse dello schermo (centro x=%d)", c.x + c.w / 2);
    CHECK(c.w == c.h && c.h <= cell.h && c.y >= cell.y && c.y + c.h <= cell.y + cell.h, "power: tondo (%dx%d) dentro la sua riga", c.w, c.h);
}

/* ---- rendering: centraggio e fasce in PIXEL ---- */
static void test_render_bounds(int columns) {
    lv_obj_t* scr = new_screen(0x000000);
    lv_obj_t* cont = bg_create_container(scr, W, H);
    for (int r = 0; r < 5; r++) for (int c = 0; c < columns; c++) {
        bg_rect_t b = bg_button_rect(bg_cell_rect(W, H, columns, 5, r, c, 1));
        plain_btn(cont, 0xFFFFFF, b.x, b.y - BG_TOP_Y, b.w, b.h);
    }
    lv_obj_update_layout(scr);
    memset(fb, 0, sizeof(fb));
    lv_obj_invalidate(scr);
    render();
    int x0 = W, x1 = -1, y0 = H, y1 = -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (fb[y * W + x]) {
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    int left = x0, right = W - 1 - x1, top = y0 - BG_TOP_Y, bottom = H - 1 - y1;
    CHECK(abs(left - right) <= 1, "%d colonne (pixel reali): margine sx %d px, dx %d px", columns, left, right);
    CHECK(top >= 10 && top <= 16 && bottom >= 10 && bottom <= 16, "%d colonne (pixel reali): fascia libera sopra %d px, sotto %d px", columns, top, bottom);
}

/* ---- icone: sfondo trasparente e bordi morbidi ---- */
static void test_icon_transparency(void) {
    lv_obj_t* scr = new_screen(0x000000);
    lv_obj_t* btn = plain_btn(scr, 0xD32F2F, 100, 100, 120, 80);
    bg_fill_button(btn, &dhicon_power, NULL, &dh_font_14);
    lv_obj_update_layout(scr);
    memset(fb, 0, sizeof(fb));
    lv_obj_invalidate(scr);
    render();
    uint16_t red = lv_color_hex(0xD32F2F).full;
    CHECK(fb[121 * W + 141] == red, "icona: l'angolo mostra il rosso del bottone (0x%04X), non nero", fb[121 * W + 141]);
    CHECK(fb[125 * W + 179] == red && fb[158 * W + 141] == red, "icona: anche gli altri angoli sono trasparenti");
    int white = 0, soft = 0;
    for (int y = 120; y < 160; y++) for (int x = 140; x < 180; x++) {
        uint16_t p = fb[y * W + x];
        if (p == 0xFFFF) white++; else if (p != red) soft++;
    }
    CHECK(white > 40, "icona: corpo bianco pieno (%d pixel)", white);
    CHECK(soft > 10, "icona: bordi morbidi/antialias (%d pixel sfumati)", soft);
}

/* ---- icona + testo: niente sovrapposizioni ---- */
static void check_content(const lv_img_dsc_t* ic, const char* label, int bw, int bh) {
    lv_obj_t* scr = new_screen(0x000000);
    lv_obj_t* btn = plain_btn(scr, 0x3C4250, 100, 200, bw, bh);
    bg_fill_button(btn, ic, label, &dh_font_14);
    lv_obj_update_layout(scr);
    lv_area_t ab, ai, al;
    lv_obj_get_coords(btn, &ab);
    lv_obj_get_coords(lv_obj_get_child(btn, 0), &ai);
    lv_obj_get_coords(lv_obj_get_child(btn, 1), &al);
    int ok = ai.y2 < al.y1 && ai.y1 >= ab.y1 && al.y2 <= ab.y2 && al.x1 >= ab.x1 && al.x2 <= ab.x2 && ai.x1 >= ab.x1 && ai.x2 <= ab.x2;
    CHECK(ok, "'%s' in %dx%d: icona (y %d-%d) sopra il testo (y %d-%d), tutto dentro il bottone", label, bw, bh, ai.y1 - ab.y1, ai.y2 - ab.y1, al.y1 - ab.y1, al.y2 - ab.y1);
}

/* ---- tocchi: fasce e margini arrivano allo schermo, i bottoni ai bottoni ---- */
static void test_hits(void) {
    lv_obj_t* scr = new_screen(0x000000);
    lv_obj_t* cont = bg_create_container(scr, W, H);
    bg_rect_t first = {0};
    for (int r = 0; r < 5; r++) for (int c = 0; c < 4; c++) {
        bg_rect_t b = bg_button_rect(bg_cell_rect(W, H, 4, 5, r, c, 1));
        if (!r && !c) first = b;
        plain_btn(cont, 0x3C4250, b.x, b.y - BG_TOP_Y, b.w, b.h);
    }
    lv_obj_update_layout(scr);
    lv_point_t p;
    p.x = first.x + first.w / 2; p.y = first.y + first.h / 2;
    lv_obj_t* h = lv_indev_search_obj(scr, &p);
    CHECK(h && lv_obj_get_parent(h) == cont, "tocco al centro di un bottone: lo riceve il bottone");
    lv_point_t pts[] = {{240, H - 4}, {240, BG_TOP_Y + 3}, {4, 300}, {W - 4, 300}, {first.x + first.w + 2, first.y + first.h / 2}};
    const char* names[] = {"fascia in basso", "fascia in alto", "margine sinistro", "margine destro", "spazio tra due bottoni"};
    for (int i = 0; i < 5; i++) {
        p = pts[i];
        h = lv_indev_search_obj(scr, &p);
        CHECK(h == scr, "tocco nella %s: lo riceve lo schermo (swipe)", names[i]);
    }
}


/* ---- scheda "in riproduzione" + griglia sotto di lei ---- */
#define HH 100
static void test_header_geometry(void) {
    int top = bg_grid_top(HH);
    bg_rect_t card = bg_header_rect(W, HH);
    CHECK(card.x >= 0 && card.x + card.w <= W && card.y >= BG_TOP_Y && card.y + card.h <= H, "scheda: dentro lo schermo (%d,%d %dx%d)", card.x, card.y, card.w, card.h);
    CHECK(abs(card.x - (W - card.x - card.w)) <= 1, "scheda: centrata (margini %d e %d)", card.x, W - card.x - card.w);
    bg_rect_t f = bg_button_rect(bg_cell_rect_at(W, H, top, 4, 3, 0, 0, 1));
    bg_rect_t l = bg_button_rect(bg_cell_rect_at(W, H, top, 4, 3, 2, 0, 1));
    int gap = f.y - (card.y + card.h), bottom = H - (l.y + l.h);
    CHECK(gap >= 8 && gap <= 20, "griglia sotto la scheda: distanza %d px", gap);
    CHECK(bottom >= 10 && bottom <= 16, "griglia con scheda: fascia libera sotto %d px (10-16)", bottom);
    int ov = 0;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) {
        bg_rect_t a = bg_button_rect(bg_cell_rect_at(W, H, top, 4, 3, r, c, 1));
        if (a.y < card.y + card.h) ov++;
        if (a.x < 0 || a.x + a.w > W || a.y + a.h > H) ov++;
    }
    CHECK(ov == 0, "griglia con scheda: nessun bottone sulla scheda o fuori schermo");
    bg_rect_t a = bg_cell_rect(W, H, 4, 5, 2, 1, 1), b2 = bg_cell_rect_at(W, H, BG_TOP_Y, 4, 5, 2, 1, 1);
    CHECK(a.x == b2.x && a.y == b2.y && a.w == b2.w && a.h == b2.h, "bg_cell_rect invariata (il telecomando TV non cambia)");
}

static void test_header_render(void) {
    lv_obj_t* scr = new_screen(0x000000);
    lv_obj_t* cont = bg_create_container(scr, W, H);
    lv_obj_t* card = bg_create_header(cont, W, HH, "YOUTUBE", "Un titolo davvero molto molto lungo che non puo' stare su una sola riga della scheda del player",
                                      "Un sottotitolo altrettanto lungo, per provare i puntini di sospensione sulla riga", 50, &dh_font_20, &dh_font_14);
    lv_obj_update_layout(scr);
    lv_area_t ac, ab, at, as, ar;
    lv_obj_get_coords(card, &ac);
    lv_obj_t *badge = lv_obj_get_child(card, 0), *title = lv_obj_get_child(card, 1), *sub = lv_obj_get_child(card, 2), *bar = lv_obj_get_child(card, 3);
    lv_obj_get_coords(badge, &ab); lv_obj_get_coords(title, &at); lv_obj_get_coords(sub, &as); lv_obj_get_coords(bar, &ar);
    CHECK(ab.y2 < at.y1 && at.y2 < as.y1 && as.y2 < ar.y1, "scheda: fonte, titolo, sottotitolo e barra in colonna senza sovrapposizioni");
    CHECK(ab.y1 >= ac.y1 && ar.y2 <= ac.y2 && at.x1 >= ac.x1 && at.x2 <= ac.x2 && as.x2 <= ac.x2 && ar.x2 <= ac.x2, "scheda: tutto il contenuto dentro la scheda (altezza usata %d di %d px)", ar.y2 - ab.y1, HH);
    int line = lv_obj_get_height(title);
    CHECK(line <= 30, "scheda: il titolo lungo resta su UNA riga (altezza %d px)", line);
    memset(fb, 0, sizeof(fb));
    lv_obj_invalidate(scr);
    render();
    CHECK(fb[(ac.y1 + 2) * W + (ac.x2 - 8)] == lv_color_hex(0x1E222B).full, "scheda: sfondo scuro della scheda");
    int by = (ar.y1 + ar.y2) / 2, bw = ar.x2 - ar.x1;
    uint16_t on = lv_color_hex(0x1FA562).full, off = lv_color_hex(0x3C4250).full;
    CHECK(fb[by * W + ar.x1 + bw / 4] == on && fb[by * W + ar.x1 + bw * 3 / 4] == off,
          "barra al 50%%: a un quarto 0x%04X (atteso verde 0x%04X), a tre quarti 0x%04X (atteso grigio 0x%04X); barra y %d-%d x %d-%d",
          fb[by * W + ar.x1 + bw / 4], on, fb[by * W + ar.x1 + bw * 3 / 4], off, ar.y1, ar.y2, ar.x1, ar.x2);
    lv_point_t p = {W / 2, ac.y1 + HH / 2};
    CHECK(lv_indev_search_obj(scr, &p) == scr, "tocco sulla scheda: lo riceve lo schermo (swipe)");
    lv_obj_t* scr2 = new_screen(0x000000);
    lv_obj_t* cont2 = bg_create_container(scr2, W, H);
    lv_obj_t* c2 = bg_create_header(cont2, W, HH, "", "Nessuna riproduzione", "Scegli una fonte", -1, &dh_font_20, &dh_font_14);
    lv_obj_update_layout(scr2);
    CHECK(lv_obj_get_child_cnt(c2) == 2, "scheda senza fonte e senza barra: solo titolo e sottotitolo");
}

static void test_inline_button(void) {
    lv_obj_t* scr = new_screen(0x000000);
    bg_rect_t r = bg_button_rect(bg_cell_rect_at(W, H, BG_TOP_Y, 1, 4, 0, 0, 1));
    lv_obj_t* btn = plain_btn(scr, 0x3C4250, r.x, r.y, r.w, r.h);
    bg_fill_button_ex(btn, &dhicon_youtube, "YouTube - in arrivo", &dh_font_20, 1);
    lv_obj_update_layout(scr);
    lv_area_t ab, ai, al;
    lv_obj_get_coords(btn, &ab); lv_obj_get_coords(lv_obj_get_child(btn, 0), &ai); lv_obj_get_coords(lv_obj_get_child(btn, 1), &al);
    int ok = ai.x2 < al.x1 && ai.x1 >= ab.x1 && al.x2 <= ab.x2 && ai.y1 >= ab.y1 && ai.y2 <= ab.y2 && al.y1 >= ab.y1 && al.y2 <= ab.y2;
    CHECK(ok, "bottone largo %dx%d: icona a sinistra e testo a destra, dentro il bottone", r.w, r.h);
}

int main(void) {
    lv_init();
    static lv_disp_draw_buf_t buf;
    lv_disp_draw_buf_init(&buf, dbuf, NULL, W * 40);
    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flush_cb; dd.draw_buf = &buf;
    lv_disp_drv_register(&dd);
    printf("== Geometria ==\n");
    test_geometry(4, 5); test_geometry(3, 5); test_circle();
    printf("\n== Rendering reale: centraggio e fasce ==\n");
    test_render_bounds(4); test_render_bounds(3);
    printf("\n== Icone ==\n");
    test_icon_transparency();
    printf("\n== Icona + testo ==\n");
    int bw = bg_button_rect(bg_cell_rect(W, H, 4, 5, 0, 0, 1)).w, bh = bg_button_rect(bg_cell_rect(W, H, 4, 5, 0, 0, 1)).h;
    int ww = bg_button_rect(bg_cell_rect(W, H, 4, 5, 4, 2, 2)).w;
    check_content(&dhicon_mute, "Muto", bw, bh); check_content(&dhicon_home, "Home", bw, bh);
    check_content(&dhicon_ch_up, "CH+", bw, bh); check_content(&dhicon_ch_down, "CH-", bw, bh);
    check_content(&dhicon_back, "Indietro", bw, bh); check_content(&dhicon_menu, "Menu", bw, bh);
    check_content(&dhicon_keypad, "Canale", ww, bh);
    printf("\n== Tocchi ==\n");
    test_hits();
    printf("\n== Scheda in riproduzione (player Bluetooth) ==\n");
    test_header_geometry(); test_header_render(); test_inline_button();
    printf("\n%d ok, %d falliti\n", ok_n, fail_n);
    return fail_n ? 1 : 0;
}
