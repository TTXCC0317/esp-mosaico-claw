/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "claw_endpoint.h"
#include "region_service.h"

typedef enum {
    TRIAL_REGION_NONE = -1,
    TRIAL_REGION_CN = 0,
    TRIAL_REGION_INTL,
    TRIAL_REGION_COUNT,
} trial_region_t;

esp_err_t trial_region_init(region_service_handle_t region_service);

/** Parse a user override ("cn" / "intl"); anything else means automatic. */
trial_region_t trial_region_from_hint(const char *hint);

/** A valid `hint` wins; otherwise wait for the device region. */
esp_err_t trial_region_resolve(trial_region_t hint, const claw_endpoint_request_t *request,
                                trial_region_t *out_region);

/** HTTPS origin of the Trial server for `region`, or NULL when invalid. */
const char *trial_region_server_url(trial_region_t region);
