#pragma once
/*
 * Telecomando touch (layout "button_grid"): geometria e contenuto dei bottoni.
 * Solo LVGL, nessuna dipendenza da Arduino: la stessa identica logica del firmware si
 * prova sul PC con test_host/button_grid/run.sh (centraggio, fasce libere, sovrapposizioni,
 * trasparenza delle icone, tocchi).
 */
#include "lvgl.h"

#define BG_TOP_Y       50   /* la griglia sta sotto titolo e icone di stato (ingranaggio, microSD) */
#define BG_BAND         8   /* fascia libera sopra e sotto (con lo scarto dei bottoni ridotti diventa ~14 px): swipe */
#define BG_MARGIN_X    14   /* margine laterale (uguale a sinistra e a destra) */
#define BG_GAP          8   /* distanza tra bottoni */
#define BG_SHRINK_PCT  12   /* i bottoni sono il 12% piu' piccoli della cella */

typedef struct { int x, y, w, h; } bg_rect_t;   /* coordinate di SCHERMO */

/* Cella (riga, colonna, larghezza in colonne) di una griglia columns x rows, centrata.
 * top_y = dove comincia l'area della griglia (BG_TOP_Y, oppure sotto la scheda "in riproduzione"). */
static inline bg_rect_t bg_cell_rect_at(int scr_w, int scr_h, int top_y, int columns, int rows, int row, int col, int span) {
    int grid_w = scr_w - 2 * BG_MARGIN_X;
    int grid_h = scr_h - top_y - 2 * BG_BAND;
    int cw = (grid_w - (columns - 1) * BG_GAP) / columns;
    int ch = (grid_h - (rows - 1) * BG_GAP) / rows;
    int used_w = columns * cw + (columns - 1) * BG_GAP;   /* centrata: margini uguali anche con resti */
    int used_h = rows * ch + (rows - 1) * BG_GAP;
    int x0 = (scr_w - used_w) / 2;
    int y0 = top_y + BG_BAND + (grid_h - used_h) / 2;
    bg_rect_t r;
    r.x = x0 + col * (cw + BG_GAP);
    r.y = y0 + row * (ch + BG_GAP);
    r.w = span * cw + (span - 1) * BG_GAP;
    r.h = ch;
    return r;
}

static inline bg_rect_t bg_cell_rect(int scr_w, int scr_h, int columns, int rows, int row, int col, int span) {
    return bg_cell_rect_at(scr_w, scr_h, BG_TOP_Y, columns, rows, row, col, span);
}

/* Scheda "in riproduzione" in testa (header_h = altezza della scheda, 0 = nessuna): la griglia
 * dei bottoni comincia sotto di lei. */
static inline int bg_grid_top(int header_h) {
    return header_h > 0 ? BG_TOP_Y + BG_BAND + header_h : BG_TOP_Y;
}
static inline bg_rect_t bg_header_rect(int scr_w, int header_h) {
    bg_rect_t r;
    r.x = BG_MARGIN_X; r.y = BG_TOP_Y + BG_BAND; r.w = scr_w - 2 * BG_MARGIN_X; r.h = header_h;
    return r;
}

/* Bottone: la cella ridotta e centrata. */
static inline bg_rect_t bg_button_rect(bg_rect_t cell) {
    bg_rect_t r;
    r.w = cell.w * (100 - BG_SHRINK_PCT) / 100;
    r.h = cell.h * (100 - BG_SHRINK_PCT) / 100;
    r.x = cell.x + (cell.w - r.w) / 2;
    r.y = cell.y + (cell.h - r.h) / 2;
    return r;
}

/* Tasto tondo (accensione): diametro = altezza del bottone, centrato nella cella. */
static inline bg_rect_t bg_circle_rect(bg_rect_t cell) {
    int d = cell.h * (100 - BG_SHRINK_PCT) / 100;
    bg_rect_t r;
    r.w = d; r.h = d;
    r.x = cell.x + (cell.w - d) / 2;
    r.y = cell.y + (cell.h - d) / 2;
    return r;
}

/* Contenitore dei bottoni: senza padding/bordo/sfondo del tema (le coordinate dei figli
 * sono esatte) e NON cliccabile, cosi' i tocchi nelle fasce e nei margini arrivano allo
 * schermo, dove vive lo swipe. */
static inline lv_obj_t* bg_create_container(lv_obj_t* scr, int scr_w, int scr_h) {
    lv_obj_t* c = lv_obj_create(scr);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, scr_w, scr_h - BG_TOP_Y);
    lv_obj_set_pos(c, 0, BG_TOP_Y);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* Contenuto di un bottone: icona (trasparente) e/o etichetta, centrati. Il flex li dispone senza
 * sovrapporli: prima icona e testo erano ancorati a mano. inline_row = 0: icona sopra, testo sotto
 * (bottoni stretti); 1: icona e testo affiancati (bottoni larghi, es. l'elenco delle fonti). */
static inline void bg_fill_button_ex(lv_obj_t* btn, const lv_img_dsc_t* icon, const char* label,
                                     const lv_font_t* font, int inline_row) {
    lv_obj_set_flex_flow(btn, inline_row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_pad_row(btn, 0, 0);
    lv_obj_set_style_pad_column(btn, 12, 0);
    if (icon) {
        lv_obj_t* im = lv_img_create(btn);
        lv_img_set_src(im, icon);
        lv_obj_clear_flag(im, LV_OBJ_FLAG_CLICKABLE);
    }
    if (label && label[0]) {
        lv_obj_t* l = lv_label_create(btn);
        lv_label_set_text(l, label);
        lv_obj_set_style_text_font(l, font, 0);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    }
}
static inline void bg_fill_button(lv_obj_t* btn, const lv_img_dsc_t* icon, const char* label, const lv_font_t* font) {
    bg_fill_button_ex(btn, icon, label, font, 0);
}

/* Scheda "in riproduzione" come un player: fonte (piccola), titolo, sottotitolo, barra di
 * avanzamento (progress 0-100; -1 = nessuna barra). Titolo e sottotitolo su UNA riga, con "..." se
 * troppo lunghi. NON cliccabile: i tocchi sopra la scheda arrivano allo schermo (swipe). Altezza
 * consigliata: 100 px. */
static inline lv_obj_t* bg_create_header(lv_obj_t* cont, int scr_w, int header_h, const char* badge,
                                         const char* title, const char* subtitle, int progress,
                                         const lv_font_t* f_big, const lv_font_t* f_small) {
    const int PAD = 10;
    int inner_w = scr_w - 2 * BG_MARGIN_X - 2 * PAD;
    lv_obj_t* card = lv_obj_create(cont);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, scr_w - 2 * BG_MARGIN_X, header_h);
    lv_obj_set_pos(card, BG_MARGIN_X, BG_BAND);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E222B), 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, PAD, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if (badge && badge[0]) {
        lv_obj_t* b = lv_label_create(card);
        lv_label_set_text(b, badge);
        lv_obj_set_style_text_font(b, f_small, 0);
        lv_obj_set_style_text_color(b, lv_color_hex(0x16A085), 0);
    }
    lv_obj_t* t = lv_label_create(card);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_size(t, inner_w, lv_font_get_line_height(f_big));   /* altezza di UNA riga: senza, i puntini non scattano e va a capo */
    lv_label_set_text(t, title ? title : "");
    lv_obj_set_style_text_font(t, f_big, 0);
    lv_obj_set_style_text_color(t, lv_color_white(), 0);
    lv_obj_t* st = lv_label_create(card);
    lv_label_set_long_mode(st, LV_LABEL_LONG_DOT);
    lv_obj_set_size(st, inner_w, lv_font_get_line_height(f_small));
    lv_label_set_text(st, subtitle ? subtitle : "");
    lv_obj_set_style_text_font(st, f_small, 0);
    lv_obj_set_style_text_color(st, lv_color_hex(0x9AA3B2), 0);
    if (progress >= 0) {
        lv_obj_t* bar = lv_bar_create(card);
        lv_obj_set_size(bar, inner_w, 6);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, progress > 100 ? 100 : progress, LV_ANIM_OFF);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);   /* il tema la rende trasparente al 30%: sparirebbe sullo sfondo */
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x3C4250), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x1FA562), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    }
    return card;
}
