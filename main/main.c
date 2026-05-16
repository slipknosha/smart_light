#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include "ble_service.h"
#include "dimmer.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "smart_light";

#define STORAGE_NAMESPACE        "light"
#define LEVEL_NVS_KEY            "level100k"
#define LEVEL_SAVE_DELAY_MS      10000U

static volatile bool s_level_save_pending;
static volatile TickType_t s_last_level_write_tick;
static uint32_t s_persisted_level;

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if ((ret == ESP_ERR_NVS_NO_FREE_PAGES) || (ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase nvs");
        ret = nvs_flash_init();
    }

    return ret;
}

static esp_err_t load_level_from_nvs(uint32_t *level)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *level = 0;
        ESP_LOGI(TAG, "No saved level, using 0/%u", DIMMER_MAX_LEVEL);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "open nvs for read");

    uint32_t stored_level = 0;
    err = nvs_get_u32(handle, LEVEL_NVS_KEY, &stored_level);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *level = 0;
        ESP_LOGI(TAG, "Level key is empty, using 0/%u", DIMMER_MAX_LEVEL);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read saved level");

    if (stored_level > DIMMER_MAX_LEVEL) {
        ESP_LOGW(TAG, "Saved level %" PRIu32 " is invalid, clamping to %u",
                 stored_level, DIMMER_MAX_LEVEL);
        stored_level = DIMMER_MAX_LEVEL;
    }

    *level = stored_level;
    ESP_LOGI(TAG, "Loaded level %" PRIu32 "/%u from flash", stored_level, DIMMER_MAX_LEVEL);
    return ESP_OK;
}

static esp_err_t save_level_to_nvs(uint32_t level)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    ESP_RETURN_ON_ERROR(err, TAG, "open nvs for write");

    err = nvs_set_u32(handle, LEVEL_NVS_KEY, level);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    return err;
}

static void schedule_level_save(uint32_t level)
{
    s_last_level_write_tick = xTaskGetTickCount();
    s_level_save_pending = (level != s_persisted_level);
}

static void handle_pending_level_save(void)
{
    if (!s_level_save_pending) {
        return;
    }

    TickType_t write_tick = s_last_level_write_tick;
    if ((xTaskGetTickCount() - write_tick) < pdMS_TO_TICKS(LEVEL_SAVE_DELAY_MS)) {
        return;
    }

    uint32_t level = dimmer_get_level();
    if (level > DIMMER_MAX_LEVEL) {
        level = DIMMER_MAX_LEVEL;
    }

    esp_err_t err = save_level_to_nvs(level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save level %" PRIu32 "/%u: %s",
                 level, DIMMER_MAX_LEVEL, esp_err_to_name(err));
        return;
    }

    s_persisted_level = level;
    if ((s_last_level_write_tick == write_tick) && (dimmer_get_level() == level)) {
        s_level_save_pending = false;
    }

    ESP_LOGI(TAG, "Saved level %" PRIu32 "/%u to flash", level, DIMMER_MAX_LEVEL);
}

static void on_ble_level_write(uint32_t level, void *ctx)
{
    (void)ctx;

    dimmer_set_level(level);
    schedule_level_save(level);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 BLE zero-cross dimmer");
    ESP_ERROR_CHECK(init_nvs());

    uint32_t initial_level = 0;
    ESP_ERROR_CHECK(load_level_from_nvs(&initial_level));
    s_persisted_level = initial_level;

    ESP_ERROR_CHECK(dimmer_init(initial_level));
    ESP_ERROR_CHECK(smart_light_ble_init(initial_level, on_ble_level_write, NULL));

    ESP_LOGI(TAG, "Initial level=%" PRIu32 "/%u", initial_level, DIMMER_MAX_LEVEL);

    while (true) {
        handle_pending_level_save();
        dimmer_poll();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
