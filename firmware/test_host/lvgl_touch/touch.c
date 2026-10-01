#include <stdio.h>
#include "lvgl.h"
/* Stessa struttura delle pagine del firmware: schermo con gestore dei tocchi,
 * contenitore 420x380 al centro, righe dentro. Un "dito" virtuale preme al
 * centro, trascina a sinistra e rilascia. */
static lv_point_t finger; static bool down;
static void read_cb(lv_indev_drv_t* d, lv_indev_data_t* data) { data->point = finger; data->state = down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL; }
static void flush_cb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) { lv_disp_flush_ready(d); }
static int n_pressed, n_pressing, n_released;
static void scr_cb(lv_event_t* e) {
    switch (lv_event_get_code(e)) { case LV_EVENT_PRESSED: n_pressed++; break; case LV_EVENT_PRESSING: n_pressing++; break; case LV_EVENT_RELEASED: n_released++; break; default: break; }
}
static void passive(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }
static void bubble(lv_obj_t* o) { lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE); }
static uint32_t tick;
static void step(void) { tick += 10; lv_tick_inc(10); lv_timer_handler(); }
/* mode 0: com'è oggi; 1: contenitori passivi (status_grid); 2: text_log scorrevole + bubble */
static void run(int mode, const char* name, int rows) {
    lv_obj_t* scr = lv_obj_create(NULL);
    lv_obj_add_event_cb(scr, scr_cb, LV_EVENT_ALL, NULL);
    lv_obj_t* list = lv_obj_create(scr);
    lv_obj_set_size(list, 420, 380); lv_obj_center(list); lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    if (mode == 1) passive(list);
    if (mode == 2) bubble(list);
    for (int i = 0; i < rows; i++) {
        lv_obj_t* row = lv_obj_create(list); lv_obj_set_size(row, LV_PCT(100), 52);
        lv_obj_t* l = lv_label_create(row); lv_label_set_text(l, "BTC   65.432,10 $");
        if (mode >= 1) passive(row);
    }
    lv_scr_load(scr); for (int i = 0; i < 5; i++) step();
    n_pressed = n_pressing = n_released = 0;
    finger.x = 240; finger.y = 240; down = true; for (int i = 0; i < 5; i++) step();
    for (int x = 240; x >= 90; x -= 15) { finger.x = x; step(); }
    down = false; for (int i = 0; i < 5; i++) step();
    int ok = n_pressed == 1 && n_pressing > 0 && n_released == 1;
    printf("%-44s schermo: PRESSED=%d PRESSING=%d RELEASED=%d -> %s\n", name, n_pressed, n_pressing, n_released, ok ? "swipe visto" : "swipe PERSO");
    lv_obj_t* b = lv_obj_create(NULL); lv_scr_load(b); step(); lv_obj_del(scr);
}
int main(void) {
    lv_init();
    static lv_disp_draw_buf_t buf; static lv_color_t px[480 * 40];
    lv_disp_draw_buf_init(&buf, px, NULL, 480 * 40);
    static lv_disp_drv_t dd; lv_disp_drv_init(&dd); dd.hor_res = 480; dd.ver_res = 480; dd.flush_cb = flush_cb; dd.draw_buf = &buf; lv_disp_drv_register(&dd);
    static lv_indev_drv_t id; lv_indev_drv_init(&id); id.type = LV_INDEV_TYPE_POINTER; id.read_cb = read_cb; lv_indev_drv_register(&id);
    lv_obj_t* blank = lv_obj_create(NULL); lv_scr_load(blank);
    run(0, "oggi (status_grid, contenitori cliccabili)", 5);
    run(1, "nuovo: status_grid, contenitori passivi", 5);
    run(1, "nuovo: status_grid 8 righe", 8);
    run(2, "nuovo: text_log scorrevole + EVENT_BUBBLE", 12);
    return 0;
}
