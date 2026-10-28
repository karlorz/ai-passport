#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pocket_pal_model.h"

// Build the home screen, apply the model to it, and make it the active screen.
// Must be called while holding bsp_lvgl_lock(), after bsp_lvgl_init().
// battery_ok reports whether bsp_battery_init() succeeded at boot; when it is
// false the battery reading is treated as unavailable.
bool pp_app_start(pp_model_t *model, bool battery_ok);

// Deliver one button event. Callable from any task: it takes the LVGL lock and
// returns. DOUBLE, PRESS, and RELEASE are ignored by the application.
void pp_app_button(uint8_t btn, uint8_t event);
