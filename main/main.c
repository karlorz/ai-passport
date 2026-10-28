// main/main.c —— Pocket Pal 数字宠物:启动顺序 + 按键分发。
//
// 按键语义:
//   上/下 短按   移动底部操作栏的选中项
//   确定  短按   执行选中项(喂食/玩耍/休息)
//   确定  长按   切换 首页 / 状态页
//
// 启动顺序:显示与 LVGL 是所有界面操作的前提,失败即停止;其余外设单项失败
// 只降级对应功能(例如电池读不到就隐藏右上角电量),不阻塞宠物本身。
#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "key_guard.h"
#include "pocket_pal_app.h"
#include "pocket_pal_audio.h"
#include "pocket_pal_model.h"
#include "pocket_pal_store.h"

#define INPUT_QUEUE_DEPTH 8

static const char *TAG = "main";

typedef struct {
    uint8_t btn;
    uint8_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

// Pocket Pal v1 never enters deep sleep, so this guard is always disarmed and
// never swallows an event. It stays wired because the guard state machine and the
// wiring contract are host-tested, and because an always-disarmed guard keeps the
// "consume before the readiness check" ordering correct for a future sleep mode.
static key_guard_t s_key_guard;

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            pp_app_button(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "pp_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// Button callbacks run on the shared esp_timer task: enqueue only and return.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // The guard is consumed before the readiness check so a release that arrives
    // while dispatch is still arming cannot leave it shut. It is never armed in
    // v1, so this passes every event through.
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = (uint8_t)btn, .event = (uint8_t)ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Pocket Pal 启动");

    bsp_i2c_init();

    // 屏幕是宠物唯一的界面,失败就没有可降级的形态。打印接线提示后停止,
    // 不做串口菜单之类的降级(那会让本文件复杂一倍)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    pp_model_t model;
    const bool store_ok = pp_store_init();
    if (!store_ok || !pp_store_load(&model)) {
        // Missing, corrupt, or wrong-version snapshot: start a fresh pet. The
        // first save persists it so the boot after this one restores something.
        pp_model_init(&model, esp_random());
        if (store_ok) (void)pp_store_save_async(&model);
    }

    const bool battery_ok = bsp_battery_init() == ESP_OK;
    const bool audio_ok = pp_audio_start();

    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键输入任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
    }

    bool app_ok = false;
    if (bsp_lvgl_lock(1000)) {
        app_ok = pp_app_start(&model, battery_ok);
        if (app_ok) {
            // Arm dispatch under the same lock that published the screens, so a
            // keypress cannot reach a half-built UI.
            s_input_ready = true;
        }
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "LVGL 加锁超时,界面未启动");
    }

    ESP_LOGI(TAG, "就绪:app=%d button=%d audio=%d battery=%d store=%d",
             app_ok, button_err == ESP_OK, audio_ok, battery_ok, store_ok);
    ESP_LOGI(TAG, "堆:剩余 %u 字节,历史最低 %u 字节",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}
