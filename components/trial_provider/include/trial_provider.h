/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "region_service.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Identifier of the claw_endpoint provider. */
#define TRIAL_PROVIDER_ID "trial"

typedef esp_err_t (*trial_provider_hmac_fn)(uint32_t key_id,
                                            const void *message,
                                            size_t message_len,
                                            uint8_t out[32]);

typedef struct {
    /** Device region source; must outlive the provider. */
    region_service_handle_t region_service;
} trial_provider_config_t;

/**
 * @brief Register the Trial endpoint provider.
 *
 * Call once before any LLM or ASR service is created. The serving region is
 * read from `region_service` on each request; while it is unknown the first
 * request waits for detection.
 */
esp_err_t trial_provider_init(const trial_provider_config_t *config);

/**
 * @brief Validate the configured eFuse HMAC key for Trial authentication.
 *
 * The validation is skipped when CONFIG_TRIAL_DEVICE_HMAC_KEY_ID is undefined
 * or set to -1. Otherwise it verifies that the configured key block is
 * programmed for HMAC upstream use and can calculate an HMAC.
 */
esp_err_t trial_provider_validate_hmac_efuse_key(void);

/** @brief Override the HMAC implementation used to sign device challenges. */
esp_err_t trial_provider_set_hmac_provider(trial_provider_hmac_fn provider);

#ifdef __cplusplus
}
#endif
