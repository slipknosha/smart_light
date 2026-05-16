#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef void (*smart_light_ble_level_cb_t)(uint32_t level, void *ctx);

esp_err_t smart_light_ble_init(uint32_t initial_level,
                               smart_light_ble_level_cb_t level_cb,
                               void *level_cb_ctx);
void smart_light_ble_set_level(uint32_t level);
