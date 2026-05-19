#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_DETAILS,
    SCREEN_BLUETOOTH,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_tick_details(void);

// Touch-driven swipe detection. Call once per loop with the current
// touch state. Tracks press start, computes deltas, and fires a screen
// switch on a horizontal swipe. Also suppresses the next "click" so the
// splash toggle doesn't fight the swipe.
void ui_touch_tick(bool pressed, int x, int y);
void ui_show_screen(screen_t screen);
void ui_cycle_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
