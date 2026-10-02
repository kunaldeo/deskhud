#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hub.h"
#include "net.h"
#include "server.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "main";
void console_start(void);

/// A freshly OTA'd image boots in "pending verify". Confirm it once the network and UI have
/// run for a while; if it crashes before that, the bootloader rolls back to the previous image.
static void confirm_task(void *arg)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGW(TAG, "new firmware: verifying");
    for (int i = 0; i < 120; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i >= 15 && net_status().state == NET_CONNECTED) {
            esp_ota_mark_app_valid_cancel_rollback();
            ESP_LOGI(TAG, "new firmware confirmed");
            hub_push_toast(0, "Firmware updated", esp_app_get_description()->version, 6);
            vTaskDelete(NULL);
            return;
        }
    }
    ESP_LOGE(TAG, "new firmware never got online; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void app_main(void)
{
    settings_init();
    settings_t st = settings_get();
    ESP_LOGI(TAG, "boot: internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_ERROR_CHECK(board_init(st.flip));
    ESP_LOGI(TAG, "after board: internal free %u (largest %u), dma free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    hub_init();
    ui_init();
    net_init();
    server_start();
    console_start();
    xTaskCreate(confirm_task, "ota_confirm", 3072, NULL, 2, NULL);
}
