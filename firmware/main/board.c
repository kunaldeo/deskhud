#include "board.h"

#include <math.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define PIN_SDA 8
#define PIN_SCL 9
#define PIN_TOUCH_INT 4
#define EXP_ADDR 0x24
#define EXP_REG_MODE 0x02
#define EXP_REG_OUT 0x03
#define EXP_REG_PWM 0x05
#define EXP_TOUCH_RST 1
#define EXP_BL_EN 2
#define EXP_LCD_RST 3
#define EXP_SD_CS 4

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_exp;
static uint8_t s_exp_out = 0xff;
static lv_display_t *s_disp;
static esp_lcd_panel_handle_t s_panel;
static volatile int s_brightness = -1, s_target = 0, s_fade_ms = 0;
static TaskHandle_t s_bl_task;

static void exp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    esp_err_t err = i2c_master_transmit(s_exp, buf, 2, 100);
    if (err != ESP_OK) ESP_LOGW(TAG, "expander write %02x: %s", reg, esp_err_to_name(err));
}

static void exp_pin(int pin, bool high)
{
    s_exp_out = high ? (s_exp_out | (1 << pin)) : (s_exp_out & ~(1 << pin));
    exp_write(EXP_REG_OUT, s_exp_out);
}

/// The expander's PWM is inverted (duty = dark time) and must stay <= 97 or the panel blanks.
static void bl_apply(int percent)
{
    if (percent <= 0) {
        exp_pin(EXP_BL_EN, false);
        return;
    }
    // Perceptual curve: equal slider steps look like equal brightness steps.
    float lin = powf(percent / 100.0f, 2.2f);
    int duty_on = 3 + (int)lroundf(lin * 97.0f);      // 3..100 % on-time
    int inv = 100 - duty_on;                            // 0..97 expander value
    exp_write(EXP_REG_PWM, (uint8_t)(inv * 255 / 100));
    exp_pin(EXP_BL_EN, true);
}

static void bl_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int from = s_brightness < 0 ? 0 : s_brightness;
        int to = s_target, ms = s_fade_ms;
        int steps = ms > 0 ? ms / 16 : 1;
        for (int i = 1; i <= steps; i++) {
            if (s_target != to) break;  // superseded
            float t = (float)i / steps;
            t = t * t * (3 - 2 * t);  // smoothstep
            int v = (int)lroundf(from + (to - from) * t);
            if (v != s_brightness || i == steps) {
                bl_apply(v);
                s_brightness = v;
            }
            if (steps > 1) vTaskDelay(pdMS_TO_TICKS(16));
        }
    }
}

void board_set_brightness(int percent, int fade_ms)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_target = percent;
    s_fade_ms = fade_ms;
    if (s_bl_task) xTaskNotifyGive(s_bl_task);
}

int board_brightness(void) { return s_target; }

static esp_err_t touch_init(esp_lcd_touch_handle_t *out, bool flip)
{
    // GT911 latches its I2C address from INT during reset: hold INT low -> 0x5D.
    gpio_config_t io = {.pin_bit_mask = 1ULL << PIN_TOUCH_INT, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&io);
    exp_pin(EXP_TOUCH_RST, false);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    exp_pin(EXP_TOUCH_RST, true);
    vTaskDelay(pdMS_TO_TICKS(80));
    gpio_set_direction(PIN_TOUCH_INT, GPIO_MODE_INPUT);

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_bus, &io_cfg, &tp_io), TAG, "touch io");
    esp_lcd_touch_config_t cfg = {
        .x_max = BOARD_W,
        .y_max = BOARD_H,
        .rst_gpio_num = -1,
        .int_gpio_num = PIN_TOUCH_INT,
        .flags = {.mirror_x = flip, .mirror_y = flip},
    };
    return esp_lcd_touch_new_i2c_gt911(tp_io, &cfg, out);
}

static esp_err_t panel_init(esp_lcd_panel_handle_t *out, uint8_t num_fbs)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 30 * 1000 * 1000,
            .h_res = BOARD_W,
            .v_res = BOARD_H,
            .hsync_pulse_width = 162,
            .hsync_back_porch = 152,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 10,
            .vsync_back_porch = 13,
            .vsync_front_porch = 3,
            .flags = {.pclk_active_neg = 1},
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = num_fbs,
        .bounce_buffer_size_px = BOARD_W * 20,  // 2 x 40 KB internal: headroom against PSRAM contention
        .dma_burst_size = 64,
        .hsync_gpio_num = 46,
        .vsync_gpio_num = 3,
        .de_gpio_num = 5,
        .pclk_gpio_num = 7,
        .disp_gpio_num = -1,
        .data_gpio_nums = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40},
        .flags = {.fb_in_psram = 1},
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, out), TAG, "rgb panel");
    return esp_lcd_panel_init(*out);
}

esp_err_t board_init(bool flip)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_SCL,
        .sda_io_num = PIN_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");
    i2c_device_config_t exp_cfg = {.device_address = EXP_ADDR, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &exp_cfg, &s_exp), TAG, "expander");
    exp_write(EXP_REG_MODE, 0xff);  // all outputs
    s_exp_out = 0xff & ~(1 << EXP_BL_EN);  // backlight off until the first frame is drawn
    exp_write(EXP_REG_OUT, s_exp_out);

    const esp_lv_adapter_rotation_t rot = flip ? ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(panel_init(&panel, esp_lv_adapter_get_required_frame_buffer_count(tear, rot)), TAG, "panel");
    s_panel = panel;

    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp, false) != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed; continuing without touch");
        tp = NULL;
    }

    esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    cfg.task_stack_size = 16 * 1024;
    cfg.task_priority = 4;
    cfg.task_core_id = 1;  // Wi-Fi lives on core 0
    cfg.task_max_delay_ms = 20;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&cfg), TAG, "lvgl adapter");

    esp_lv_adapter_display_config_t dcfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(panel, NULL, BOARD_W, BOARD_H, rot);
    dcfg.tear_avoid_mode = tear;
    dcfg.profile.use_psram = true;
    dcfg.profile.buffer_height = 24;  // internal RAM; 48 KB
    s_disp = esp_lv_adapter_register_display(&dcfg);
    if (!s_disp) return ESP_FAIL;

    if (tp) {
        esp_lv_adapter_touch_config_t tcfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, tp);
        if (!esp_lv_adapter_register_touch(&tcfg)) ESP_LOGE(TAG, "touch register failed");
    }
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), TAG, "lvgl start");

    xTaskCreatePinnedToCore(bl_task, "backlight", 3072, NULL, 3, &s_bl_task, 0);
    ESP_LOGI(TAG, "board up: %dx%d%s, touch %s", BOARD_W, BOARD_H, flip ? " flipped" : "", tp ? "ok" : "missing");
    return ESP_OK;
}

bool board_lock(void) { return esp_lv_adapter_lock(-1) == ESP_OK; }
void board_unlock(void) { esp_lv_adapter_unlock(); }
lv_display_t *board_display(void) { return s_disp; }

uint32_t board_idle_ms(void)
{
    return s_disp ? lv_display_get_inactive_time(s_disp) : 0;
}

const uint16_t *board_framebuffer(void)
{
    void *fb0 = NULL;
    if (!s_panel || esp_lcd_rgb_panel_get_frame_buffer(s_panel, 1, &fb0) != ESP_OK) return NULL;
    return fb0;
}
