#pragma once
// Waveshare ESP32-S3-Touch-LCD-7B: 1024x600 RGB565 panel, GT911 touch, IO-expander MCU at 0x24
// (backlight enable + PWM, LCD/touch reset, SD CS, USB/CAN select).
#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_W 1024
#define BOARD_H 600

/// Brings up I2C, the IO expander, the panel, touch and LVGL (running in its own task).
/// `flip` rotates everything 180° (fixed for the life of the boot).
esp_err_t board_init(bool flip);

/// LVGL is not thread safe: wrap every lv_* call made outside LVGL callbacks.
bool board_lock(void);
void board_unlock(void);

/// 0 turns the backlight off; 1..100 is perceptual brightness. Fades over `fade_ms`.
void board_set_brightness(int percent, int fade_ms);
int board_brightness(void);

/// Milliseconds since the screen was last touched.
uint32_t board_idle_ms(void);

lv_display_t *board_display(void);

/// The panel's frame buffer (RGB565, BOARD_W x BOARD_H, row-major) for screenshots. Read it
/// while holding board_lock(): between frames both direct-mode buffers hold the shown image.
const uint16_t *board_framebuffer(void);
