/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "trial_region.h"

#define TRIAL_FIRMWARE_VERSION_HEADER "X-Firmware-Version"

esp_err_t trial_token_init(void);

/** Return the running firmware version sent with every Trial request. */
const char *trial_token_firmware_version(void);

/**
 * Get a cached or freshly exchanged access token for `region`. Caller frees
 * `out_token`. After a server-side failure the exchange is not retried until
 * the backoff elapses; the last failure is returned instead, e.g.
 * CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE when the exchange itself got 401.
 */
esp_err_t trial_token_get(trial_region_t region, const claw_endpoint_request_t *request,
                          char **out_token, uint32_t *out_generation);

/** The service accepted the token of `generation`. */
void trial_token_accept(uint32_t generation);

/**
 * The service answered 401 for the token of `generation`.
 *
 * A token that was accepted before is treated as expired and dropped, so one
 * new exchange is allowed (returns true). A token that was never accepted is
 * a verification failure: it is dropped, backoff starts, and false is returned.
 */
bool trial_token_reject(uint32_t generation);
