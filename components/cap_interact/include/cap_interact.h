/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the LLM-visible cap_interact tool group that drives the ESP-Mosaico
 * multi-function interaction subboard (RGB LEDs, IR, status, auto-camera). */
esp_err_t cap_interact_register_group(void);

#ifdef __cplusplus
}
#endif
