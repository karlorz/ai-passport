#include "pocket_pal_app.h"

#include <stdio.h>

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "lvgl.h"
#include "pocket_pal_audio.h"
#include "pocket_pal_store.h"
#include "pocket_pal_view.h"
#include "sdkconfig.h"

#define SCREEN_W 240
#define SCREEN_H 320

// Action-bar geometry: three equal buttons across the 240 px panel.
#define ACTION_COUNT 3
#define ACTION_Y 252
#define ACTION_W 66
#define ACTION_H 44
#define ACTION_X0 12
#define ACTION_STEP 75

// Screen refresh cadence for the bars and chips, in ticks of the 1 s timer.
#define BATTERY_PERIOD_S 30U
#define SAVE_PERIOD_S 60U
#define BUBBLE_HOLD_MS 2000U

// The generated subset covers exactly the PRD inventory, so one application font
// serves every Chinese label. Montserrat supplies only the icons it lacks; the
// descriptor must be writable and outlive every label that points at it.
LV_FONT_DECLARE(pocket_pal_font_16);

static const char *TAG = "pp_app";

static const char *const ACTION_LABELS[ACTION_COUNT] = {
    "\u5582\u98DF", // feed
    "\u73A9\u800D", // play
    "\u4F11\u606F", // rest
};
static const char *const STAT_LABELS[ACTION_COUNT] = {
    "\u9971\u98DF", // fullness
    "\u5FC3\u60C5", // mood
    "\u7CBE\u529B", // energy
};

static lv_font_t s_cjk_font;

static pp_model_t s_model;
static bool s_battery_ok;
static int s_battery;
static int s_selection;
static bool s_on_status;
static bool s_dirty;
static uint32_t s_save_seconds;
static uint32_t s_battery_seconds;

static lv_obj_t *s_home;
static lv_obj_t *s_status;
static lv_obj_t *s_pet;
static lv_obj_t *s_bubble;
static lv_obj_t *s_action[ACTION_COUNT];
static lv_obj_t *s_action_label[ACTION_COUNT];
static lv_obj_t *s_chip[ACTION_COUNT];
static lv_obj_t *s_battery_label;
static lv_obj_t *s_status_battery_label;
static lv_obj_t *s_bar[ACTION_COUNT];
static lv_obj_t *s_bar_value[ACTION_COUNT];
static lv_obj_t *s_care_label;

static lv_timer_t *s_tick;
static lv_timer_t *s_bubble_timer;

// Last values written to the status widgets. The 1 s tick redraws only what
// actually changed: a stat decays once every few minutes, and re-setting
// unchanged labels each second both reallocates their text and repaints the
// panel on a battery-powered board.
static int s_shown_stat[ACTION_COUNT] = { -1, -1, -1 };
static int s_shown_care = -1;
static int s_shown_battery = -2;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, int radius,
                     uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    return obj;
}

static lv_obj_t *screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_hex(PP_COLOR_BG), 0);
    return scr;
}

static void bubble_hide(lv_timer_t *timer)
{
    (void)timer;
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
}

// Speech bubble text replaces the previous cue instead of queueing, so a burst of
// events cannot leave a stale message on screen.
static void bubble_show(const char *text)
{
    if (!s_bubble || !s_bubble_timer) return;
    lv_label_set_text(s_bubble, text);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    lv_timer_set_repeat_count(s_bubble_timer, 1);
    lv_timer_reset(s_bubble_timer);
    lv_timer_resume(s_bubble_timer);
}

static void refresh_selection(void)
{
    for (int i = 0; i < ACTION_COUNT; i++) {
        const bool selected = i == s_selection;
        lv_obj_set_style_bg_color(s_action[i],
            lv_color_hex(selected ? PP_COLOR_CORAL : PP_COLOR_BG), 0);
        lv_obj_set_style_text_color(s_action_label[i],
            lv_color_hex(selected ? PP_COLOR_WHITE : PP_COLOR_INK), 0);
    }
}

static void refresh_chips(void)
{
    const uint8_t values[ACTION_COUNT] = {
        s_model.fullness, s_model.mood, s_model.energy,
    };
    for (int i = 0; i < ACTION_COUNT; i++) {
        // A chip only exists to explain a need, so it appears for stats below the
        // threshold and stays out of the way otherwise.
        if (values[i] < 25U) {
            lv_obj_remove_flag(s_chip[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_chip[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void refresh_status(void)
{
    const uint8_t values[ACTION_COUNT] = {
        s_model.fullness, s_model.mood, s_model.energy,
    };
    for (int i = 0; i < ACTION_COUNT; i++) {
        if (s_shown_stat[i] == values[i]) continue;
        s_shown_stat[i] = values[i];
        lv_bar_set_value(s_bar[i], values[i], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar[i],
            lv_color_hex(values[i] < 25U ? PP_COLOR_CORAL : PP_COLOR_MINT),
            LV_PART_INDICATOR);
        lv_label_set_text_fmt(s_bar_value[i], "%u", (unsigned)values[i]);
    }
    if (s_shown_care != (int)s_model.care_count) {
        s_shown_care = (int)s_model.care_count;
        lv_label_set_text_fmt(s_care_label, "\u7167\u987E\u6B21\u6570 %u",
                              (unsigned)s_model.care_count);
    }
}

static void refresh_battery(void)
{
    if (s_shown_battery == s_battery) return;
    s_shown_battery = s_battery;

    lv_obj_t *targets[] = { s_battery_label, s_status_battery_label };
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        lv_obj_t *target = targets[i];
        if (!target) continue;
        // -1 means the gauge is absent or unreadable: hide the number rather than
        // show a level the board cannot actually measure.
        if (s_battery < 0) {
            lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_label_set_text_fmt(target, "%d%%", s_battery);
        lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_all(void)
{
    pp_pet_set_expression(s_pet, pp_model_expression(&s_model));
    refresh_chips();
    refresh_status();
}

static void build_home(void)
{
    s_home = screen();

    lv_obj_t *name = label(s_home, "\u56E2\u56E2", &s_cjk_font, PP_COLOR_INK);
    lv_obj_set_pos(name, 14, 8);

    s_battery_label = label(s_home, "", &lv_font_montserrat_14, PP_COLOR_INK);
    lv_obj_align(s_battery_label, LV_ALIGN_TOP_RIGHT, -14, 12);

    // A failed pet allocation only costs the character: the rest of the screen
    // still builds, and pp_pet_set_expression() tolerates a NULL pet, so the
    // action bar and status page stay usable.
    s_pet = pp_pet_create(s_home, 65, 70);
    if (!s_pet) {
        ESP_LOGE(TAG, "pet allocation failed; the home screen has no character");
    }

    s_bubble = label(s_home, "", &s_cjk_font, PP_COLOR_INK);
    lv_obj_set_width(s_bubble, SCREEN_W - 28);
    lv_obj_set_style_text_align(s_bubble, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_bubble, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < ACTION_COUNT; i++) {
        s_chip[i] = box(s_home, ACTION_X0 + i * ACTION_STEP, 206, ACTION_W, 26, 13,
                        PP_COLOR_CORAL);
        lv_obj_t *text = label(s_chip[i], STAT_LABELS[i], &s_cjk_font,
                               PP_COLOR_WHITE);
        lv_obj_center(text);
        lv_obj_add_flag(s_chip[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < ACTION_COUNT; i++) {
        s_action[i] = box(s_home, ACTION_X0 + i * ACTION_STEP, ACTION_Y, ACTION_W,
                          ACTION_H, 20, PP_COLOR_BG);
        // Unselected buttons are cream on a cream page; the pink outline is what
        // keeps their shape visible. It is set once and never changes.
        lv_obj_set_style_border_width(s_action[i], 2, 0);
        lv_obj_set_style_border_color(s_action[i], lv_color_hex(PP_COLOR_PINK), 0);
        s_action_label[i] = label(s_action[i], ACTION_LABELS[i], &s_cjk_font,
                                  PP_COLOR_INK);
        lv_obj_center(s_action_label[i]);
    }

    refresh_selection();
}

static void build_status(void)
{
    s_status = screen();

    lv_obj_t *title = label(s_status, "\u72B6\u6001", &s_cjk_font, PP_COLOR_INK);
    lv_obj_set_pos(title, 16, 10);

    s_status_battery_label = label(s_status, "", &lv_font_montserrat_14,
                                   PP_COLOR_INK);
    lv_obj_align(s_status_battery_label, LV_ALIGN_TOP_RIGHT, -14, 12);

    for (int i = 0; i < ACTION_COUNT; i++) {
        const int y = 70 + i * 54;
        lv_obj_t *stat = label(s_status, STAT_LABELS[i], &s_cjk_font, PP_COLOR_INK);
        lv_obj_set_pos(stat, 16, y);

        s_bar[i] = lv_bar_create(s_status);
        lv_obj_set_pos(s_bar[i], 76, y + 2);
        lv_obj_set_size(s_bar[i], 110, 14);
        lv_bar_set_range(s_bar[i], 0, 100);
        lv_obj_set_style_bg_color(s_bar[i], lv_color_hex(PP_COLOR_PINK),
                                  LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar[i], 7, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bar[i], 7, LV_PART_INDICATOR);

        s_bar_value[i] = label(s_status, "", &lv_font_montserrat_14, PP_COLOR_INK);
        lv_obj_set_pos(s_bar_value[i], 194, y + 1);
    }

    s_care_label = label(s_status, "", &s_cjk_font, PP_COLOR_INK);
    lv_obj_align(s_care_label, LV_ALIGN_TOP_MID, 0, 232);

    lv_obj_t *hint = label(s_status, "\u957F\u6309\u8FD4\u56DE", &s_cjk_font,
                           PP_COLOR_INK);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 284);
}

static void show_screen(bool status)
{
    s_on_status = status;
    lv_screen_load(status ? s_status : s_home);
}

static void do_action(void)
{
    pp_result_t result;
    pp_react_t react;
    const char *message = NULL;
    pp_sound_t success_sound;

    switch (s_selection) {
    case 0:
        result = pp_model_feed(&s_model);
        react = PP_REACT_FEED;
        success_sound = PP_SND_FEED;
        message = "\u597D\u5403"; // yummy
        break;
    case 1:
        result = pp_model_play(&s_model);
        react = PP_REACT_PLAY;
        success_sound = PP_SND_PLAY;
        message = "\u597D\u5F00\u5FC3"; // happy
        break;
    default:
        result = pp_model_rest(&s_model);
        react = PP_REACT_REST;
        success_sound = PP_SND_REST;
        message = NULL; // rest needs no caption; the "Zzz" overlay says it
        break;
    }

    switch (result) {
    case PP_RESULT_OK:
        pp_pet_react(s_pet, react);
        pp_audio_play(success_sound);
        if (message) bubble_show(message);
        s_dirty = true;
        refresh_all();
        break;
    case PP_RESULT_FULL:
        bubble_show("\u8FD8\u4E0D\u997F"); // not hungry yet
        pp_audio_play(PP_SND_REFUSED);
        break;
    case PP_RESULT_NEED_ENERGY:
        bubble_show("\u592A\u7D2F\u4E86"); // too tired
        pp_audio_play(PP_SND_REFUSED);
        break;
    case PP_RESULT_COOLDOWN:
        bubble_show("\u7A0D\u7B49\u4E00\u4E0B"); // wait a moment
        pp_audio_play(PP_SND_REFUSED);
        break;
    case PP_RESULT_INVALID:
        break;
    }
}

static void handle_key(uint8_t btn, uint8_t event)
{
    if (event == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        show_screen(!s_on_status);
        return;
    }
    // DOUBLE, PRESS, and RELEASE are intentionally unhandled in v1: press gives
    // no preview and a long press must not also register as a click.
    if (event != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        const int step = btn == BSP_BTN_UP ? ACTION_COUNT - 1 : 1;
        s_selection = (s_selection + step) % ACTION_COUNT;
        refresh_selection();
        pp_audio_play(PP_SND_MOVE);
    } else if (btn == BSP_BTN_OK) {
        do_action();
    }
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    const uint32_t elapsed = 1U * CONFIG_POCKET_PAL_TIME_SCALE;
    const uint32_t events = pp_model_tick(&s_model, elapsed);

    if (events & PP_EVENT_HUNGRY) {
        bubble_show("\u6211\u997F\u4E86"); // I am hungry
        pp_audio_play(PP_SND_CALL);
    }
    if (events & PP_EVENT_SAD) {
        bubble_show("\u966A\u6211\u73A9"); // play with me
        pp_audio_play(PP_SND_SAD);
    }
    if (events & PP_EVENT_RECOVERED) {
        bubble_show("\u597D\u5F00\u5FC3"); // so happy
    }

    refresh_all();

    s_battery_seconds += elapsed;
    if (s_battery_seconds >= BATTERY_PERIOD_S) {
        s_battery_seconds = 0;
        if (s_battery_ok) s_battery = bsp_battery_soc();
        refresh_battery();
    }

    s_save_seconds += elapsed;
    if (s_dirty || s_save_seconds >= SAVE_PERIOD_S) {
        s_dirty = false;
        s_save_seconds = 0;
        pp_store_save_async(&s_model);
    }
}

bool pp_app_start(pp_model_t *model, bool battery_ok)
{
    if (!model) return false;

    // Copy the generated subset once, before any label exists, and give it the
    // Latin/icon fallback the font guide recommends for mixed text.
    s_cjk_font = pocket_pal_font_16;
    s_cjk_font.fallback = &lv_font_montserrat_14;

    s_model = *model;
    s_battery_ok = battery_ok;
    s_battery = battery_ok ? bsp_battery_soc() : -1;
    s_shown_battery = -2; // force the first refresh_battery() to write
    s_shown_care = -1;
    for (int i = 0; i < ACTION_COUNT; i++) s_shown_stat[i] = -1;
    s_selection = 0;
    s_on_status = false;
    s_dirty = false;
    s_save_seconds = 0;
    s_battery_seconds = 0;

    build_home();
    build_status();
    refresh_all();
    refresh_battery();

    s_bubble_timer = lv_timer_create(bubble_hide, BUBBLE_HOLD_MS, NULL);
    if (!s_bubble_timer) return false;
    // One-shot: pause until a cue re-arms it, so no periodic work runs while the
    // bubble is empty.
    lv_timer_set_auto_delete(s_bubble_timer, false);
    lv_timer_set_repeat_count(s_bubble_timer, 0);

    lv_screen_load(s_home);

    s_tick = lv_timer_create(tick_cb, 1000, NULL);
    if (!s_tick) {
        ESP_LOGE(TAG, "timer creation failed; the pet will not age");
        return false;
    }
    ESP_LOGI(TAG, "UI ready: fullness=%u mood=%u energy=%u",
             s_model.fullness, s_model.mood, s_model.energy);

    // No PSRAM on this board: log the LVGL pool once both screens exist so a
    // later allocation regression is visible without a memory-usage build.
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "LVGL pool: %u/%u bytes used (%u%%), largest free %u",
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
             (unsigned)mon.used_pct, (unsigned)mon.free_biggest_size);
    return true;
}

void pp_app_button(uint8_t btn, uint8_t event)
{
    if (!s_home) return;
    // This runs on the input task, so LVGL access needs the port lock. The lock is
    // skipped only when the UI is already gone, never held across a wait longer
    // than the timeout.
    if (!bsp_lvgl_lock(500)) return;
    handle_key(btn, event);
    bsp_lvgl_unlock();
}
