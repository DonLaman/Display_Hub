// Implementazione BSP — pin e sequenza di inizializzazione presi 1:1 dal
// progetto OraQuadraNano di riferimento per il modulo ESP32-4848S040.
#include "esp32_4848s040_bsp.h"

#define GFX_DEV_DEVICE ESP32_4848S040_86BOX_GUITION
#define RGB_PANEL
#include <Arduino_GFX_Library.h>   // fornisce anche st7701_type9_init_operations per questo device

#include <TAMC_GT911.h>
#include <Wire.h>
#include <lvgl.h>
#include <SD.h>
#include <SPI.h>
#include <driver/i2s.h>

// ---------- Pin (identici al progetto di riferimento) ----------
#define I2C_SDA_PIN 19
#define I2C_SCL_PIN 45
#define TOUCH_INT   -1
#define TOUCH_RST   -1
#define TOUCH_ROTATION ROTATION_NORMAL
#define TOUCH_MAP_X1 480
#define TOUCH_MAP_X2 0
#define TOUCH_MAP_Y1 480
#define TOUCH_MAP_Y2 0

#define GFX_BL          38
#define PWM_CHANNEL     0
#define PWM_FREQ        1000
#define PWM_RESOLUTION  8

#define SD_CS_PIN   BSP_SD_CS
#define SD_MOSI_PIN BSP_SD_MOSI
#define SD_CLK_PIN  BSP_SD_CLK
#define SD_MISO_PIN BSP_SD_MISO

// ---------- Oggetti hardware ----------
static Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, 39 /* CS */,
    48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);

static Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    18 /* DE */, 17 /* VSYNC */, 16 /* HSYNC */, 21 /* PCLK */,
    11 /* R0 */, 12 /* R1 */, 13 /* R2 */, 14 /* R3 */, 0 /* R4 */,
    8 /* G0 */, 20 /* G1 */, 3 /* G2 */, 46 /* G3 */, 9 /* G4 */, 10 /* G5 */,
    4 /* B0 */, 5 /* B1 */, 6 /* B2 */, 7 /* B3 */, 15 /* B4 */,
    1 /* hsync_polarity */, 10, 8, 50,
    1 /* vsync_polarity */, 10, 8, 20,
    0 /* pclk_active_neg */, 12000000, false,
    0, 0, 0);

static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480, 480, rgbpanel, 0, true,
    bus, GFX_NOT_DEFINED, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

static TAMC_GT911 ts = TAMC_GT911(
    I2C_SDA_PIN, I2C_SCL_PIN, TOUCH_INT, TOUCH_RST,
    max(TOUCH_MAP_X1, TOUCH_MAP_X2), max(TOUCH_MAP_Y1, TOUCH_MAP_Y2));

// ---------- Bridge Arduino_GFX -> LVGL ----------
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lv_buf1;
static lv_disp_drv_t disp_drv;

static uint32_t g_flush_call_count = 0;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    g_flush_call_count++;
    // Solo il primo chunk di ogni redraw (y1==0) per non inondare la seriale:
    // conferma se GFX riceve davvero le chiamate di disegno e con che colore
    // nell'angolo in alto a sinistra (0x0000 atteso per uno sfondo nero).
    if (area->y1 == 0) {
        Serial.printf("[BSP] flush #%u: area=(%d,%d)-(%d,%d) w=%u h=%u primo_pixel=0x%04X\n",
                      g_flush_call_count, area->x1, area->y1, area->x2, area->y2, w, h,
                      (unsigned)color_p[0].full);
    }
    // draw16bitRGBBitmap si aspetta RGB565: è esattamente il formato di
    // lv_color_t quando LV_COLOR_DEPTH è 16 (impostato in lv_conf.h)
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
    lv_disp_flush_ready(drv);
}

// ---------- Bridge TAMC_GT911 -> LVGL (indev pointer) ----------
static lv_indev_drv_t indev_drv;

// ---------- Rilevamento swipe/doppio tap da coordinate grezze ----------
static touch_swipe_cb_t g_swipe_cb = nullptr;
static touch_double_tap_cb_t g_double_tap_cb = nullptr;

static bool touch_was_down = false;
static int32_t touch_start_x = 0;
static int32_t touch_last_x = 0;
static uint32_t touch_start_ms = 0;

static uint32_t last_tap_ms = 0;
static int32_t last_tap_x = 0, last_tap_y = 0;

static const int32_t SWIPE_MIN_DISTANCE_PX = 60;   // sotto questa soglia è un tap, non uno swipe
static const uint32_t SWIPE_MAX_DURATION_MS = 700;  // oltre, è una pressione prolungata, non uno swipe
static const uint32_t DOUBLE_TAP_MAX_INTERVAL_MS = 400;
static const int32_t DOUBLE_TAP_MAX_DISTANCE_PX = 40; // tra i due tap: se troppo lontani non è un doppio tap

void register_touch_swipe_callback(touch_swipe_cb_t cb) { g_swipe_cb = cb; }
void register_touch_double_tap_callback(touch_double_tap_cb_t cb) { g_double_tap_cb = cb; }

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    ts.read();
    bool pressed = ts.isTouched && ts.touches > 0;
    if (pressed) {
        data->state = LV_INDEV_STATE_PRESSED;
        // Le coordinate grezze del GT911 su questo modulo sono invertite
        // rispetto agli assi dello schermo (osservazione dal progetto di
        // riferimento, TOUCH_MAP_X1=480/X2=0): map() corregge l'inversione.
        int32_t x = map(ts.points[0].x, TOUCH_MAP_X1, TOUCH_MAP_X2, 0, 479);
        int32_t y = map(ts.points[0].y, TOUCH_MAP_Y1, TOUCH_MAP_Y2, 0, 479);
        data->point.x = x;
        data->point.y = y;

        if (!touch_was_down) {
            touch_start_x = x;
            touch_start_ms = millis();
        }
        touch_last_x = x;
        touch_was_down = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;

        if (touch_was_down) {
            // Rilascio: valutiamo se è stato uno swipe orizzontale o un tap.
            int32_t dx = touch_last_x - touch_start_x;
            uint32_t duration = millis() - touch_start_ms;

            if (abs(dx) >= SWIPE_MIN_DISTANCE_PX && duration <= SWIPE_MAX_DURATION_MS) {
                if (g_swipe_cb) g_swipe_cb(dx < 0 ? -1 : 1);
            } else if (abs(dx) < DOUBLE_TAP_MAX_DISTANCE_PX) {
                // Non è uno swipe: potrebbe essere un tap, controlliamo se forma
                // un doppio tap col precedente (vicino nel tempo e nello spazio).
                uint32_t now = millis();
                int32_t dtap = abs(touch_last_x - last_tap_x);
                if (now - last_tap_ms <= DOUBLE_TAP_MAX_INTERVAL_MS && dtap < DOUBLE_TAP_MAX_DISTANCE_PX) {
                    if (g_double_tap_cb) g_double_tap_cb();
                    last_tap_ms = 0; // evita che un terzo tap ravvicinato conti come un altro doppio tap
                } else {
                    last_tap_ms = now;
                    last_tap_x = touch_last_x;
                }
            }
        }
        touch_was_down = false;
    }
}

// ---------- Backlight ----------
static void backlight_fade(int from, int to, int step_delay_ms) {
    if (from <= to) {
        for (int duty = from; duty <= to; duty++) { ledcWrite(PWM_CHANNEL, duty); delay(step_delay_ms); }
    } else {
        for (int duty = from; duty >= to; duty--) { ledcWrite(PWM_CHANNEL, duty); delay(step_delay_ms); }
    }
}

// ---------- API pubblica ----------

void display_init() {
    Serial.println("[BSP] display_init: avvio PWM backlight...");
    ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
    ledcAttachPin(GFX_BL, PWM_CHANNEL);

    Serial.println("[BSP] display_init: chiamo gfx->begin()...");
    bool ok = gfx->begin(6600000);
    Serial.printf("[BSP] gfx->begin() ha ritornato: %s\n", ok ? "true (OK)" : "FALSE (fallito!)");
    if (!ok) {
        Serial.println("[BSP] ATTENZIONE: il pannello non si e' inizializzato. Il resto "
                        "del boot prosegue comunque, ma lo schermo restera' vuoto. "
                        "Sospetto principale: framebuffer non allocabile (serve PSRAM "
                        "per un pannello 480x480, verifica PSRAM_MODE/octal).");
    }
    Serial.printf("[BSP] PSRAM totale: %u byte, libera: %u byte\n",
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.printf("[BSP] Heap interno libero: %u byte\n", (unsigned)ESP.getFreeHeap());

    gfx->fillScreen(BLACK);
    backlight_fade(0, 250, 4); // fade-in, più rapido del progetto originale (nessuna schermata di logo qui)
    Serial.println("[BSP] display_init: backlight portato a piena luminosita'.");

    lv_init();

    static lv_color_t *lv_buf1_storage = (lv_color_t *)heap_caps_malloc(
        480 * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA); // buffer parziale: 40 righe, sufficiente per LVGL
    if (lv_buf1_storage == nullptr) {
        Serial.println("[BSP] ERRORE CRITICO: heap_caps_malloc del buffer LVGL ha "
                        "restituito NULL (memoria DMA-capable esaurita). LVGL non potra' "
                        "disegnare nulla da qui in avanti.");
    } else {
        Serial.println("[BSP] Buffer LVGL allocato correttamente.");
    }
    lv_buf1 = lv_buf1_storage;
    lv_disp_draw_buf_init(&draw_buf, lv_buf1, NULL, 480 * 40);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 480;
    disp_drv.ver_res = 480;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);
    Serial.println("[BSP] display_init: completato.");
}

void touch_init() {
    Serial.println("[BSP] touch_init: avvio I2C e GT911...");
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    ts.begin();
    ts.setRotation(TOUCH_ROTATION);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    lv_indev_drv_register(&indev_drv);
    Serial.println("[BSP] touch_init: completato.");
    // Swipe e doppio tap NON passano più dal meccanismo LV_EVENT_GESTURE di
    // LVGL (la direzione non veniva rilevata in modo affidabile): li calcoliamo
    // noi da lvgl_touch_read_cb sopra, e li notifichiamo via
    // register_touch_swipe_callback()/register_touch_double_tap_callback(),
    // che src/main.cpp chiama subito dopo touch_init().
}

bool sd_init() {
    // Stesso bus SPI del display (MOSI/CLK condivisi), CS dedicato.
    // Va inizializzata dopo WiFi/audio per evitare conflitti sul bus SPI,
    // come nota il progetto di riferimento.
    //
    // CORRETTO: prima sd_spi era una variabile locale. SD.begin() ne conserva
    // l'indirizzo e lo usa a ogni lettura/scrittura successiva, ma l'oggetto
    // veniva distrutto all'uscita da questa funzione: accessi alla scheda dopo
    // sd_init() su memoria non più valida. static = vive quanto il programma.
    static SPIClass sd_spi(HSPI);
    static bool spi_begun = false;
    if (!spi_begun) {
        sd_spi.begin(SD_CLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
        spi_begun = true;
    }
    SD.end();  // consente di richiamare sd_init() dopo un cambio di scheda
    return SD.begin(SD_CS_PIN, sd_spi);
}

void audio_init() {
    if (BSP_I2S_PIN_ENABLE != -1) {
        pinMode(BSP_I2S_PIN_ENABLE, OUTPUT);
        digitalWrite(BSP_I2S_PIN_ENABLE, HIGH); // abilita l'amplificatore esterno
    }

    i2s_config_t config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
    };
    i2s_pin_config_t pins = {
        .bck_io_num = BSP_I2S_BCLK,
        .ws_io_num = BSP_I2S_LRC,
        .data_out_num = BSP_I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    i2s_driver_install(I2S_NUM_0, &config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pins);
}

void audio_beep(uint16_t frequency_hz, uint16_t duration_ms) {
    const int sample_rate = 16000;
    int num_samples = (sample_rate * duration_ms) / 1000;
    for (int i = 0; i < num_samples; i++) {
        int16_t sample = (int16_t)(sinf(2.0f * PI * frequency_hz * i / sample_rate) * 8000);
        size_t bytes_written;
        i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytes_written, portMAX_DELAY);
    }
}
