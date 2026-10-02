// LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC). LVGL makes many small allocations that would
// otherwise land in the ~25 KB of internal RAM Wi-Fi leaves us.
#include "esp_heap_caps.h"
#include "lvgl.h"

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) {}
void *lv_malloc_core(size_t size) { return heap_caps_malloc(size, CAPS); }
void *lv_realloc_core(void *p, size_t new_size) { return heap_caps_realloc(p, new_size, CAPS); }
void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, CAPS);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->used_pct = 100 - (100 * info.total_free_bytes) / (mon->total_size ? mon->total_size : 1);
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
