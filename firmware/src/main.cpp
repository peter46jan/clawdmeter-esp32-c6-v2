#include <Arduino.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include "display_cfg.h"
#include "data.h"
#include "ui.h"
#include "ble.h"
#include "power.h"
#include "imu.h"
#include "splash.h"
#include "usage_rate.h"

// On boards with PSRAM (S3) put big draw buffers in SPIRAM. On C6 there is
// no PSRAM, so route them to internal SRAM and rely on the splash refactor
// + smaller BUF_LINES to keep total usage within ~512KB.
#if defined(CLAWD_NO_PSRAM)
#define CLAWD_BIGBUF_CAP MALLOC_CAP_INTERNAL
#else
#define CLAWD_BIGBUF_CAP CLAWD_BIGBUF_CAP
#endif

// Physical buttons (global, screen-independent):
//   BTN_BACK   (GPIO 0)  — left,  send Space (Claude Code voice mode push-to-talk)
//   BTN_FWD    (GPIO 18) — right, send Shift+Tab (Claude Code mode toggle)
//   AXP PWR    (PMU)     — middle, cycle screens; on splash, cycle animations
#define BTN_BACK BTN_LEFT_GPIO
#define BTN_FWD  BTN_RIGHT_GPIO

// ---- Hardware objects ----
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
#if defined(CLAWD_DISPLAY_SH8601)
// SH8601 driver — for C6 2.16. Reset pin is NC (GFX_NOT_DEFINED); panel
// reset is performed by the AXP2101 ALDO3 power-cycle in power_init().
Arduino_SH8601 *gfx = new Arduino_SH8601(
    bus, GFX_NOT_DEFINED, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT);
#else
Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT, 0, 0, 0, 0);
#endif
TouchDrvCST92xx touch;
XPowersPMU pmu;
SensorQMI8658 imu;

static UsageData usage = {};

// ---- Touch interrupt + shared state ----
static volatile bool     touch_pressed = false;
static volatile uint16_t touch_x = 0;
static volatile uint16_t touch_y = 0;
static volatile bool     touch_data_ready = false;

static void IRAM_ATTR touch_isr(void) {
    touch_data_ready = true;
}

// Poll touch every loop iteration in addition to the ISR. The CST92xx
// reportedly only pulses INT on initial press on some board revisions, so
// LVGL would otherwise see a single point per tap — fine for clicks, but
// gesture detection (swipe → / ←) needs a continuous stream of points to
// compute a direction. Polling at ~loop-rate (LVGL itself runs ~60 Hz)
// gives ~16 ms sampling which is plenty for swipes.
static uint32_t last_touch_poll_ms = 0;
#define TOUCH_POLL_INTERVAL_MS 15

static void touch_read() {
    bool poll = touch_data_ready;
    uint32_t now = millis();
    if (now - last_touch_poll_ms >= TOUCH_POLL_INTERVAL_MS) {
        poll = true;
        last_touch_poll_ms = now;
    }
    if (!poll) return;
    touch_data_ready = false;

    int16_t tx[5], ty[5];
    uint8_t n = touch.getPoint(tx, ty, touch.getSupportTouchPoint());
    if (n > 0) {
        touch_pressed = true;
        touch_x = (uint16_t)tx[0];
        touch_y = (uint16_t)ty[0];
    } else {
        touch_pressed = false;
    }
}

// ---- LVGL draw buffers (partial render) ----
// On S3 (PSRAM) we use 40-line strips. On C6 (512KB SRAM total) we drop to
// 20-line strips: 3 buffers × 480 × 20 × 2 ≈ 58 KB, leaving headroom for
// LVGL state, NimBLE stack, splash 20×20 image, etc.
#if defined(CLAWD_NO_PSRAM)
#define BUF_LINES 20
#else
#define BUF_LINES 40
#endif
static uint16_t *buf1 = nullptr;
static uint16_t *buf2 = nullptr;
// rot_buf for strip rotation — max size is 480×480 (full invalidation case)
// but typical partial strips are much smaller
static uint16_t *rot_buf = nullptr;

// LVGL tick callback
static uint32_t my_tick(void) {
    return millis();
}

// Rotate a w×h strip and compute destination coordinates on the 480×480 display.
// src pixels are in row-major order for the rectangle (sx, sy, w, h).
// Output goes to rot_buf in row-major order for the destination rectangle.
static void rotate_strip(const uint16_t *src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t *dx, int32_t *dy, int32_t *dw, int32_t *dh) {
    const int S = LCD_WIDTH;  // 480

    switch (r) {
    case 1: { // 90° CW: (x,y) -> (S-1-y, x)
        *dw = h; *dh = w;
        *dx = S - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(h-1-y, x)
                rot_buf[x * h + (h - 1 - y)] = src[y * w + x];
            }
        }
        break;
    }
    case 2: { // 180°: (x,y) -> (S-1-x, S-1-y)
        *dw = w; *dh = h;
        *dx = S - sx - w;
        *dy = S - sy - h;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                rot_buf[(h - 1 - y) * w + (w - 1 - x)] = src[y * w + x];
            }
        }
        break;
    }
    case 3: { // 270° CW: (x,y) -> (y, S-1-x)
        *dw = h; *dh = w;
        *dx = sy;
        *dy = S - sx - w;
        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                // src(x,y) -> dst(y, w-1-x)
                rot_buf[(w - 1 - x) * h + y] = src[y * w + x];
            }
        }
        break;
    }
    default:
        *dx = sx; *dy = sy; *dw = w; *dh = h;
        break;
    }
}

// Push one RGB565 strip to the panel.
//
// On CO5300 (S3) we use draw16bitRGBBitmap — that path is known good.
//
// On SH8601 (C6 2.16) draw16bitRGBBitmap leaves the panel BLACK because
// each call does its own start/end (CS toggle) and rapid CS toggling
// confuses this panel revision. We instead drive one clean QSPI
// transaction per strip via startWrite → writeAddrWindow → writePixels
// → endWrite. writePixels handles the MSB-first byte swap that SH8601
// expects (writeBytes would not).
static inline void push_strip(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t *pix) {
#if defined(CLAWD_DISPLAY_SH8601)
    gfx->startWrite();
    gfx->writeAddrWindow(x, y, w, h);
    gfx->writePixels(pix, (uint32_t)w * (uint32_t)h);
    gfx->endWrite();
#else
    gfx->draw16bitRGBBitmap(x, y, pix, w, h);
#endif
}

// LVGL flush callback — rotates partial strips and writes to display
static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *src = (uint16_t*)px_map;
    uint8_t r = imu_get_rotation();

    if (r == 0) {
        push_strip(area->x1, area->y1, w, h, src);
    } else {
        int32_t dx, dy, dw, dh;
        rotate_strip(src, w, h, area->x1, area->y1, r, &dx, &dy, &dw, &dh);
        push_strip(dx, dy, dw, dh, rot_buf);
    }
    lv_display_flush_ready(disp);
}

#if defined(CLAWD_DISPLAY_SH8601)
// SH8601 vendor init sequence — runs once after gfx->begin().
// Without this, the panel powers on but stays dark on the C6 2.16 board.
// Source: vendor demo (08_LVGL_V8_Test/bsp_lvgl_port.cpp), confirmed by the
// claude-desktop-buddy repo's display.cpp.
static void sh8601_vendor_init(Arduino_DataBus *b) {
    static const uint8_t init_ops[] = {
        BEGIN_WRITE,
        WRITE_COMMAND_8, 0x11,             // SLPOUT (re-issue, harmless)
        END_WRITE,
        DELAY, 120,
        BEGIN_WRITE,
        WRITE_C8_D8, 0xFE, 0x20,           // page select MFR
        WRITE_C8_D8, 0x19, 0x10,
        WRITE_C8_D8, 0x1C, 0xA0,
        WRITE_C8_D8, 0xFE, 0x00,           // back to USER page
        WRITE_C8_D8, 0xC4, 0x80,
        WRITE_C8_D8, 0x3A, 0x55,           // pixel format (override 0x05 → 0x55)
        WRITE_C8_D8, 0x35, 0x00,           // tearing line
        WRITE_C8_D8, 0x36, 0x30,           // MADCTL (override lib's 0x00)
        WRITE_C8_D8, 0x53, 0x20,           // CABC / brightness control
        WRITE_C8_D8, 0x51, 0xFF,           // brightness max
        WRITE_C8_D8, 0x63, 0xFF,
        WRITE_COMMAND_8, 0x2A, WRITE_BYTES, 4, 0x00, 0x00, 0x01, 0xDF,
        WRITE_COMMAND_8, 0x2B, WRITE_BYTES, 4, 0x00, 0x00, 0x01, 0xDF,
        WRITE_COMMAND_8, 0x29,             // DISPON
        END_WRITE,
        DELAY, 50,
    };
    b->batchOperation(init_ops, sizeof(init_ops));
}
#endif

// CO5300 requires even-aligned flush regions
static void rounder_cb(lv_event_t* e) {
    lv_area_t *area = (lv_area_t*)lv_event_get_param(e);
    area->x1 = area->x1 & ~1;
    area->y1 = area->y1 & ~1;
    area->x2 = area->x2 | 1;
    area->y2 = area->y2 | 1;
}

// LVGL touch callback
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (touch_pressed) {
        data->point.x = touch_x;
        data->point.y = touch_y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->extra_usage_usd  = doc["eu"] | -1.0f;
    out->extra_budget_usd = doc["em"] | -1.0f;
    out->valid = true;
    return true;
}

// Serial command buffer
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

#if defined(CLAWD_NO_PSRAM)
// C6 has no PSRAM — a 460KB snapshot framebuffer doesn't fit alongside LVGL +
// NimBLE + everything else in 512KB. Disable the screenshot QA command.
static void send_screenshot() {
    Serial.println("SCREENSHOT_UNAVAILABLE_NO_PSRAM");
}
#else
static void send_screenshot() {
    const uint32_t w = LCD_WIDTH, h = LCD_HEIGHT;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, CLAWD_BIGBUF_CAP);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n", (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");

    heap_caps_free(sbuf);
}
#endif // CLAWD_NO_PSRAM

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) {
                send_screenshot();
            }
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    // Init I2C (shared by touch + PMU)
    Wire.begin(IIC_SDA, IIC_SCL);

    // Init display
    gfx->begin();
#if defined(CLAWD_DISPLAY_SH8601)
    // Override the lib's default init with the Waveshare-vendor sequence —
    // without this, SH8601 on the C6 2.16 stays dark.
    sh8601_vendor_init(bus);
#endif
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);

    // Init PMU
    power_init();

    // Init IMU (accelerometer for auto-rotation)
    imu_init();

    // Init touch
    touch.setPins(TP_RST, TP_INT);
    if (!touch.begin(Wire, CST9220_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("Touch init failed");
    } else {
        touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
        touch.setSwapXY(true);
        touch.setMirrorXY(true, false);
        attachInterrupt(TP_INT, touch_isr, FALLING);
        Serial.println("Touch init OK");
    }

    // Init LVGL
    lv_init();
    lv_tick_set_cb(my_tick);

    // Allocate PSRAM-backed partial render buffers
    buf1 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, CLAWD_BIGBUF_CAP);
    buf2 = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, CLAWD_BIGBUF_CAP);
    // rot_buf needs to hold the largest possible strip after rotation
    // A 480×40 strip rotated 90° becomes 40×480, same pixel count
    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * BUF_LINES * 2, CLAWD_BIGBUF_CAP);

    lv_display_t* disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, LCD_WIDTH * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    // CO5300 even-alignment rounder
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // Init BLE data channel
    ble_init();

    // Physical buttons. Some boards (C6 2.16) only have BOOT — guard the
    // second one with BTN_RIGHT_GPIO == -1.
    if (BTN_BACK >= 0) pinMode(BTN_BACK, INPUT_PULLUP);
    if (BTN_FWD  >= 0) pinMode(BTN_FWD,  INPUT_PULLUP);

    // Build dashboard
    ui_init();

    // Show initial BLE status on Bluetooth screen
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());

    // Show initial battery status
    ui_update_battery(power_battery_pct(), power_is_charging());

    ui_show_screen(SCREEN_SPLASH);

    Serial.println("Dashboard ready, waiting for data on BLE...");
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Brightness ramp state for rotation transition
// On rotation change we blank the panel, force a full LVGL redraw at the
// new orientation, then ramp brightness back up over ~125ms so the
// transition reads as deliberate instead of as a glitch.
static void handle_rotation_change(void) {
    static uint8_t last_rotation = 0;
    static uint8_t  ramp_step = 0;  // 0=idle, 1-4=ramping
    static uint32_t ramp_last = 0;

    uint8_t rot = imu_get_rotation();
    if (rot != last_rotation) {
        gfx->setBrightness(0);
        last_rotation = rot;
        lv_obj_invalidate(lv_screen_active());
        ramp_step = 1;
        return;
    }

    if (ramp_step == 0) return;
    uint32_t now = millis();
    if (now - ramp_last < 25) return;
    ramp_last = now;

    static const uint8_t levels[] = {60, 120, 170, 200};
    gfx->setBrightness(levels[ramp_step - 1]);
    if (ramp_step >= 4) ramp_step = 0;
    else                ramp_step++;
}

void loop() {
    touch_read();
    // Feed the raw touch state to the swipe detector BEFORE LVGL processes
    // the same touch — that way ui_touch_tick can mark a swipe as
    // "handled" and the global_click_cb in ui.cpp will skip the splash
    // toggle when LVGL fires CLICKED on release.
    ui_touch_tick(touch_pressed, touch_x, touch_y);
    lv_timer_handler();
    ui_tick_anim();
    ui_tick_details();
    ble_tick();
    power_tick();
    imu_tick();
    splash_tick();

    // Three-button input (global, screen-independent):
    //   LEFT  (GPIO 0)  → Space (voice-mode push-to-talk; press & release tracked)
    //   RIGHT (GPIO 18) → Shift+Tab (Claude Code mode toggle)
    //   PWR   (AXP)     → cycle screens; on splash, cycle animations
    {
        static bool back_was = false, fwd_was = false;
        bool back_now = (BTN_BACK >= 0) && (digitalRead(BTN_BACK) == LOW);
        bool fwd_now  = (BTN_FWD  >= 0) && (digitalRead(BTN_FWD)  == LOW);

        if (back_now != back_was) {
            if (back_now) ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            else          ble_keyboard_release();
            back_was = back_now;
        }
        if (fwd_now != fwd_was) {
            if (fwd_now) ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
            else         ble_keyboard_release();
            fwd_was = fwd_now;
        }

        if (power_pwr_pressed()) {
            if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
            else                                          ui_cycle_screen();
        }
    }

    handle_rotation_change();

    // Update BLE status on screen when state changes
    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    // Update battery indicator
    static int last_pct = -2;
    static bool last_charging = false;
    int pct = power_battery_pct();
    bool charging = power_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    // Check for serial commands (screenshot, etc.)
    check_serial_cmd();

    // Process incoming BLE data
    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
