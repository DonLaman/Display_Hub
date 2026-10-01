/* Disegna le due viste del telecomando con LO STESSO codice del firmware e salva PPM. */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "dh_icons.h"
#include "dh_fonts.h"
#include "button_grid_ui.h"
#include "layout_gen.h"
#define W 480
#define H 480
static uint16_t fb[W * H];
static lv_color_t dbuf[W * 40];
static void flush_cb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
    for (int y = a->y1; y <= a->y2; y++) for (int x = a->x1; x <= a->x2; x++) { if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = c->full; c++; }
    lv_disp_flush_ready(d);
}
static void view(const BtnDef* b, int n, int columns, const HdrDef* hdr, const char* title, const char* file) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_t* t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &dh_font_20, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t* gear = lv_obj_create(scr);   /* come l'overlay: ingranaggio 44 px a 6 px dall'angolo */
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, 44, 44); lv_obj_set_pos(gear, W - 6 - 44, 6);
    lv_obj_set_style_radius(gear, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(0x2C2C2E), 0);
    lv_obj_t* cont = bg_create_container(scr, W, H);
    int rows = 0;
    for (int i = 0; i < n; i++) if (b[i].row + 1 > rows) rows = b[i].row + 1;
    int top_y = bg_grid_top(hdr ? hdr->height : 0);
    if (hdr) bg_create_header(cont, W, hdr->height, hdr->badge, hdr->title, hdr->subtitle, hdr->progress, &dh_font_20, &dh_font_14);
    for (int i = 0; i < n; i++) {
        bg_rect_t cell = bg_cell_rect_at(W, H, top_y, columns, rows, b[i].row, b[i].col, b[i].span);
        bg_rect_t r = b[i].circle ? bg_circle_rect(cell) : bg_button_rect(cell);
        lv_obj_t* btn = lv_btn_create(cont);
        lv_obj_set_pos(btn, r.x, r.y - BG_TOP_Y);
        lv_obj_set_size(btn, r.w, r.h);
        lv_obj_set_style_bg_color(btn, lv_color_hex(b[i].color), 0);
        lv_obj_set_style_radius(btn, b[i].circle ? LV_RADIUS_CIRCLE : 8, 0);
        const lv_img_dsc_t* ic = b[i].icon[0] ? icon_by_name(b[i].icon) : 0;
        bg_fill_button_ex(btn, ic, b[i].label, (b[i].inl || !ic) ? &dh_font_20 : &dh_font_14, b[i].inl);
    }
    lv_scr_load(scr); lv_obj_update_layout(scr);
    memset(fb, 0, sizeof(fb)); lv_obj_invalidate(scr);
    for (int i = 0; i < 12; i++) { lv_tick_inc(20); lv_timer_handler(); }
    FILE* f = fopen(file, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { unsigned p = fb[i]; unsigned char px[3] = {(p >> 11 & 31) * 255 / 31, (p >> 5 & 63) * 255 / 63, (p & 31) * 255 / 31}; fwrite(px, 1, 3, f); }
    fclose(f);
}
int main(void) {
    lv_init();
    static lv_disp_draw_buf_t buf; lv_disp_draw_buf_init(&buf, dbuf, NULL, W * 40);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd); dd.hor_res = W; dd.ver_res = H; dd.flush_cb = flush_cb; dd.draw_buf = &buf;
    lv_disp_t* disp = lv_disp_drv_register(&dd);
    /* come setup() del firmware: tema scuro, primario blu, secondario verde acqua */
    lv_disp_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_TEAL), true, &dh_font_14));
    view(TV_MAIN_BTNS, TV_MAIN_N, TV_MAIN_COLS, 0, "Telecomando TV", "/tmp/view_main.ppm");
    view(TV_KEYPAD_BTNS, TV_KEYPAD_N, TV_KEYPAD_COLS, 0, "Telecomando TV", "/tmp/view_keypad.ppm");
    view(BT_MAIN_BTNS, BT_MAIN_N, BT_MAIN_COLS, &BT_MAIN_HDR, "Audio Bluetooth", "/tmp/view_bt_main.ppm");
    /* SIMULAZIONE (lo streaming non c'e' ancora): come apparira' la scheda con un brano in corso */
    static const HdrDef playing = {100, "MP3 - Vol 60%", "Bohemian Rhapsody", "Queen - A Night at the Opera", 42};
    view(BT_MAIN_BTNS, BT_MAIN_N, BT_MAIN_COLS, &playing, "Audio Bluetooth", "/tmp/view_bt_playing.ppm");
    view(BT_SOURCES_BTNS, BT_SOURCES_N, BT_SOURCES_COLS, 0, "Audio Bluetooth", "/tmp/view_bt_sources.ppm");
    return 0;
}
