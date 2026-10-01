// Le poche funzioni di DhSettings che DhGatewayUi usa. styleScreen/makeButton/makeHeader sono COPIA
// di quelle vere (DhSettings.cpp) cosi' l'aspetto e' lo stesso; il resto registra cosa succede.
#include "DhSettings.h"
#include "dh_fonts.h"
namespace DhSettings {
const lv_color_t COLOR_CARD = lv_color_hex(0x1C1C1E);
const lv_color_t COLOR_MUTED = lv_color_hex(0xC4C4CA);

void styleScreen(lv_obj_t* scr) {
    lv_obj_set_style_text_font(scr, &dh_font_14, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}
lv_obj_t* makeButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}
lv_obj_t* makeHeader(lv_obj_t* scr, const char* title, lv_event_cb_t back_cb) {
    if (back_cb) {
        lv_obj_t* back = makeButton(scr, LV_SYMBOL_LEFT " Indietro", back_cb);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 8, 6);
    }
    lv_obj_t* t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &dh_font_20, 0);
    if (back_cb) lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 14);
    else lv_obj_align(t, LV_ALIGN_TOP_LEFT, 12, 14);
    return t;
}

// --- registro per il test ---
static bool g_busy = false;
void setBusy(bool b) { g_busy = b; }
bool busy() { return g_busy; }
String last_toast;
int root_opened = 0;
String confirm_title, confirm_text;
void (*confirm_yes)() = nullptr;
lv_obj_t* root_screen = nullptr;
void showToast(const String& t) { last_toast = t; }
void showConfirm(const char* title, const char* text, void (*on_yes)()) { confirm_title = title; confirm_text = text; confirm_yes = on_yes; }
void open() {                         // la radice delle Impostazioni
    root_opened++;
    if (!root_screen) { root_screen = lv_obj_create(NULL); styleScreen(root_screen); }
    lv_scr_load(root_screen);
}
}  // namespace DhSettings
