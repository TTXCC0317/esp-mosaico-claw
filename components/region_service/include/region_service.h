/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The stored region is a two-character code: an ISO 3166 country code from
 * GeoIP, "CN" when the user picked mainland China, or "0N" when the user
 * picked international service. A later successful detection overwrites it.
 */
#define REGION_SERVICE_CODE_LEN           3U
#define REGION_SERVICE_CODE_MAINLAND      "CN"
#define REGION_SERVICE_CODE_INTERNATIONAL "0N"

typedef struct region_service_t *region_service_handle_t;

typedef enum {
    REGION_SERVICE_AREA_UNKNOWN = 0,
    REGION_SERVICE_AREA_MAINLAND,
    REGION_SERVICE_AREA_INTERNATIONAL,
} region_service_area_t;

typedef enum {
    REGION_SERVICE_DETECT_IDLE = 0,
    REGION_SERVICE_DETECT_RUNNING,
    REGION_SERVICE_DETECT_SUCCEEDED,
    REGION_SERVICE_DETECT_FAILED,
} region_service_detect_state_t;

typedef struct {
    region_service_detect_state_t detect_state;
    region_service_area_t area;
    char code[REGION_SERVICE_CODE_LEN];
} region_service_status_t;

typedef struct {
    /** Endpoint returning the caller's country code as plain text. */
    const char *geoip_url;
    /** Timeout of one GeoIP request; 0 uses the default. */
    uint32_t request_timeout_ms;
} region_service_config_t;

/* Invoked from the detection task or the caller of select/reset. */
typedef void (*region_service_event_cb_t)(region_service_handle_t handle,
                                          const region_service_status_t *status,
                                          void *user_ctx);

/** Create the service and load the stored region. Requires initialized NVS. */
esp_err_t region_service_create(const region_service_config_t *config,
                                region_service_handle_t *ret_handle);
esp_err_t region_service_register_cb(region_service_handle_t handle,
                                     region_service_event_cb_t cb,
                                     void *user_ctx);
esp_err_t region_service_get_status(region_service_handle_t handle,
                                    region_service_status_t *ret_status);

/**
 * @brief Start detection in the background, retrying until `timeout_ms`.
 *
 * Calls while a detection runs merge into it and only extend its deadline.
 */
esp_err_t region_service_detect(region_service_handle_t handle, uint32_t timeout_ms);

/**
 * @brief Return the known area, detecting and waiting up to `timeout_ms` if
 *        none is stored yet.
 */
esp_err_t region_service_wait_area(region_service_handle_t handle,
                                   uint32_t timeout_ms,
                                   region_service_area_t *ret_area);

/** Store the user's choice as "CN" or "0N". */
esp_err_t region_service_select_area(region_service_handle_t handle,
                                     region_service_area_t area);

/** Forget the stored region, e.g. during a factory reset. */
esp_err_t region_service_reset(region_service_handle_t handle);

void region_service_delete(region_service_handle_t handle);

#ifdef __cplusplus
}
#endif
