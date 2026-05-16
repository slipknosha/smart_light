#pragma once

#include <stdint.h>

#include "esp_err.h"

#define DIMMER_MAX_LEVEL 100000U

esp_err_t dimmer_init(uint32_t initial_level);
void dimmer_set_level(uint32_t level);
uint32_t dimmer_get_level(void);
void dimmer_poll(void);
