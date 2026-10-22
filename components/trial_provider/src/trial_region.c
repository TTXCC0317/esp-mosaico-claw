/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "trial_region.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TRIAL_REGION_POLL_MS 50U

#define TRIAL_REGION_HINT_CN   "cn"
#define TRIAL_REGION_HINT_INTL "intl"

static region_service_handle_t s_region_service;

static bool region_is_valid(trial_region_t region)
{
    return region >= 0 && region < TRIAL_REGION_COUNT;
}

static bool hint_equals(const char *hint, size_t len, const char *keyword)
{
    return len == strlen(keyword) && strncasecmp(hint, keyword, len) == 0;
}

esp_err_t trial_region_init(region_service_handle_t region_service)
{
    if (!region_service) {
        return ESP_ERR_INVALID_ARG;
    }
    s_region_service = region_service;
    return ESP_OK;
}

trial_region_t trial_region_from_hint(const char *hint)
{
    if (!hint) {
        return TRIAL_REGION_NONE;
    }
    while (isspace((unsigned char)*hint)) {
        hint++;
    }
    size_t len = strlen(hint);
    while (len > 0 && isspace((unsigned char)hint[len - 1])) {
        len--;
    }

    if (hint_equals(hint, len, TRIAL_REGION_HINT_CN)) {
        return TRIAL_REGION_CN;
    }
    if (hint_equals(hint, len, TRIAL_REGION_HINT_INTL)) {
        return TRIAL_REGION_INTL;
    }
    return TRIAL_REGION_NONE;
}

esp_err_t trial_region_resolve(trial_region_t hint, const claw_endpoint_request_t *request,
                                trial_region_t *out_region)
{
    if (!request || !out_region) {
        return ESP_ERR_INVALID_ARG;
    }
    if (region_is_valid(hint)) {
        *out_region = hint;
        return ESP_OK;
    }
    if (!s_region_service) {
        return ESP_ERR_INVALID_STATE;
    }

    bool detecting = false;
    for (;;) {
        esp_err_t err = claw_endpoint_request_check(request);
        if (err != ESP_OK) return err;
        region_service_status_t status;
        err = region_service_get_status(s_region_service, &status);
        if (err != ESP_OK) return err;
        if (status.area != REGION_SERVICE_AREA_UNKNOWN) {
            *out_region = status.area == REGION_SERVICE_AREA_MAINLAND ? TRIAL_REGION_CN : TRIAL_REGION_INTL;
            return ESP_OK;
        }
        uint32_t remaining = claw_endpoint_request_remaining_ms(request);
        if (!remaining) return ESP_ERR_TIMEOUT;
        if (!detecting) {
            err = region_service_detect(s_region_service, remaining);
            if (err != ESP_OK) return err;
            detecting = true;
        }
        uint32_t wait_ms = remaining < TRIAL_REGION_POLL_MS ? remaining : TRIAL_REGION_POLL_MS;
        TickType_t ticks = pdMS_TO_TICKS(wait_ms);
        vTaskDelay(ticks ? ticks : 1);
    }
}

const char *trial_region_server_url(trial_region_t region)
{
    switch (region) {
    case TRIAL_REGION_CN:
        return CONFIG_TRIAL_SERVER_URL_CN;
    case TRIAL_REGION_INTL:
        return CONFIG_TRIAL_SERVER_URL_INTL;
    default:
        return NULL;
    }
}
