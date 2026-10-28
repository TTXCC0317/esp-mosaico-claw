/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "region_service.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "region_service";

#define REGION_NVS_NAMESPACE          "region"
#define REGION_NVS_KEY                "region"
#define REGION_URL_LEN                128U
#define REGION_GEOIP_MAX_BODY         32U
#define REGION_DEFAULT_REQUEST_MS     5000U
#define REGION_RETRY_INTERVAL_MS      1000U
#define REGION_TASK_STACK             (6U * 1024U)
#define REGION_TASK_PRIO              3
#define REGION_DETECT_DONE_BIT        BIT0

struct region_service_t {
    SemaphoreHandle_t lock;
    EventGroupHandle_t events;
    TaskHandle_t task;
    char geoip_url[REGION_URL_LEN];
    uint32_t request_timeout_ms;
    char code[REGION_SERVICE_CODE_LEN];
    region_service_detect_state_t detect_state;
    int64_t deadline_ms;
    region_service_event_cb_t cb;
    void *cb_user_ctx;
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool code_is_valid(const char *code)
{
    if (strlen(code) != REGION_SERVICE_CODE_LEN - 1U) {
        return false;
    }
    if (strcmp(code, REGION_SERVICE_CODE_INTERNATIONAL) == 0) {
        return true;
    }
    return isupper((unsigned char)code[0]) && isupper((unsigned char)code[1]);
}

static region_service_area_t area_from_code(const char *code)
{
    if (!code[0]) {
        return REGION_SERVICE_AREA_UNKNOWN;
    }
    return strcmp(code, REGION_SERVICE_CODE_MAINLAND) == 0
        ? REGION_SERVICE_AREA_MAINLAND : REGION_SERVICE_AREA_INTERNATIONAL;
}

static void load_code(char out[REGION_SERVICE_CODE_LEN])
{
    nvs_handle_t nvs;
    size_t len = REGION_SERVICE_CODE_LEN;

    out[0] = '\0';
    if (nvs_open(REGION_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_get_str(nvs, REGION_NVS_KEY, out, &len) != ESP_OK || !code_is_valid(out)) {
        out[0] = '\0';
    }
    nvs_close(nvs);
}

static esp_err_t store_code(const char *code)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(REGION_NVS_NAMESPACE, NVS_READWRITE, &nvs);

    if (err != ESP_OK) {
        return err;
    }
    err = code[0] ? nvs_set_str(nvs, REGION_NVS_KEY, code) : nvs_erase_key(nvs, REGION_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

static void snapshot_locked(region_service_handle_t handle, region_service_status_t *status)
{
    status->detect_state = handle->detect_state;
    status->area = area_from_code(handle->code);
    strlcpy(status->code, handle->code, sizeof(status->code));
}

static void notify(region_service_handle_t handle)
{
    region_service_status_t status;
    region_service_event_cb_t cb;
    void *user_ctx;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    snapshot_locked(handle, &status);
    cb = handle->cb;
    user_ctx = handle->cb_user_ctx;
    xSemaphoreGive(handle->lock);
    if (cb) {
        cb(handle, &status, user_ctx);
    }
}

/* Store and publish a new code; skips NVS writes when nothing changed. */
static esp_err_t set_code(region_service_handle_t handle, const char *code)
{
    esp_err_t err = ESP_OK;

    xSemaphoreTake(handle->lock, portMAX_DELAY);
    if (strcmp(handle->code, code) != 0) {
        err = store_code(code);
        if (err == ESP_OK) {
            strlcpy(handle->code, code, sizeof(handle->code));
        }
    }
    xSemaphoreGive(handle->lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to store region: %s", esp_err_to_name(err));
    }
    return err;
}

static size_t trim_in_place(char *text)
{
    size_t start = 0;
    size_t end = strlen(text);

    while (text[start] && isspace((unsigned char)text[start])) {
        start++;
    }
    while (end > start && isspace((unsigned char)text[end - 1])) {
        end--;
    }
    memmove(text, text + start, end - start);
    text[end - start] = '\0';
    return end - start;
}

static esp_err_t fetch_country_code(region_service_handle_t handle, uint32_t timeout_ms,
                                    char out[REGION_SERVICE_CODE_LEN])
{
    char body[REGION_GEOIP_MAX_BODY + 1] = {0};
    esp_http_client_config_t config = {
        .url = handle->geoip_url,
        .timeout_ms = (int)timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_get_status_code(client) != HttpStatus_Ok) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        int len = esp_http_client_read_response(client, body, (int)sizeof(body) - 1);
        err = len < 0 ? ESP_FAIL : ESP_OK;
    }
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }

    if (trim_in_place(body) != REGION_SERVICE_CODE_LEN - 1U ||
            !isalpha((unsigned char)body[0]) || !isalpha((unsigned char)body[1])) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    out[0] = (char)toupper((unsigned char)body[0]);
    out[1] = (char)toupper((unsigned char)body[1]);
    out[2] = '\0';
    return ESP_OK;
}

static int64_t remaining_ms(region_service_handle_t handle)
{
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    int64_t remaining = handle->deadline_ms - now_ms();
    xSemaphoreGive(handle->lock);
    return remaining;
}

static void detect_task(void *arg)
{
    region_service_handle_t handle = arg;
    char code[REGION_SERVICE_CODE_LEN] = {0};
    esp_err_t err = ESP_ERR_TIMEOUT;
    int64_t remaining;

    while ((remaining = remaining_ms(handle)) > 0) {
        uint32_t attempt_ms = remaining < handle->request_timeout_ms
            ? (uint32_t)remaining : handle->request_timeout_ms;
        err = fetch_country_code(handle, attempt_ms, code);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGD(TAG, "GeoIP attempt failed: %s", esp_err_to_name(err));
        remaining = remaining_ms(handle);
        if (remaining > 0) {
            vTaskDelay(pdMS_TO_TICKS(remaining < REGION_RETRY_INTERVAL_MS
                ? remaining : REGION_RETRY_INTERVAL_MS));
        }
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "GeoIP region %s", code);
        (void)set_code(handle, code);
    } else {
        ESP_LOGW(TAG, "GeoIP detection failed: %s", esp_err_to_name(err));
    }
    region_service_status_t status;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->detect_state = err == ESP_OK ? REGION_SERVICE_DETECT_SUCCEEDED
                                         : REGION_SERVICE_DETECT_FAILED;
    handle->task = NULL;
    snapshot_locked(handle, &status);
    region_service_event_cb_t cb = handle->cb;
    void *user_ctx = handle->cb_user_ctx;
    /* After this bit the handle may be deleted; only the snapshot is used. */
    xEventGroupSetBits(handle->events, REGION_DETECT_DONE_BIT);
    xSemaphoreGive(handle->lock);
    if (cb) {
        cb(handle, &status, user_ctx);
    }
    vTaskDelete(NULL);
}

esp_err_t region_service_create(const region_service_config_t *config,
                                region_service_handle_t *ret_handle)
{
    if (!config || !config->geoip_url || !config->geoip_url[0] || !ret_handle ||
            strlen(config->geoip_url) >= REGION_URL_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_handle = NULL;

    region_service_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle) {
        return ESP_ERR_NO_MEM;
    }
    handle->lock = xSemaphoreCreateMutex();
    handle->events = xEventGroupCreate();
    if (!handle->lock || !handle->events) {
        region_service_delete(handle);
        return ESP_ERR_NO_MEM;
    }
    strlcpy(handle->geoip_url, config->geoip_url, sizeof(handle->geoip_url));
    handle->request_timeout_ms = config->request_timeout_ms
        ? config->request_timeout_ms : REGION_DEFAULT_REQUEST_MS;
    load_code(handle->code);
    xEventGroupSetBits(handle->events, REGION_DETECT_DONE_BIT);
    ESP_LOGI(TAG, "Stored region: %s", handle->code[0] ? handle->code : "(none)");
    *ret_handle = handle;
    return ESP_OK;
}

esp_err_t region_service_register_cb(region_service_handle_t handle,
                                     region_service_event_cb_t cb,
                                     void *user_ctx)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    handle->cb = cb;
    handle->cb_user_ctx = user_ctx;
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t region_service_get_status(region_service_handle_t handle,
                                    region_service_status_t *ret_status)
{
    if (!handle || !ret_status) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    snapshot_locked(handle, ret_status);
    xSemaphoreGive(handle->lock);
    return ESP_OK;
}

esp_err_t region_service_detect(region_service_handle_t handle, uint32_t timeout_ms)
{
    if (!handle || timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    bool started = false;
    xSemaphoreTake(handle->lock, portMAX_DELAY);
    int64_t deadline = now_ms() + timeout_ms;
    if (deadline > handle->deadline_ms) {
        handle->deadline_ms = deadline;
    }
    if (!handle->task) {
        xEventGroupClearBits(handle->events, REGION_DETECT_DONE_BIT);
        if (xTaskCreate(detect_task, "region_detect", REGION_TASK_STACK, handle,
                        REGION_TASK_PRIO, &handle->task) == pdPASS) {
            handle->detect_state = REGION_SERVICE_DETECT_RUNNING;
            started = true;
        } else {
            handle->task = NULL;
            xEventGroupSetBits(handle->events, REGION_DETECT_DONE_BIT);
            err = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(handle->lock);
    if (started) {
        notify(handle);
    }
    return err;
}

esp_err_t region_service_wait_area(region_service_handle_t handle,
                                   uint32_t timeout_ms,
                                   region_service_area_t *ret_area)
{
    region_service_status_t status;

    if (!handle || !ret_area || timeout_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)region_service_get_status(handle, &status);
    if (status.area == REGION_SERVICE_AREA_UNKNOWN) {
        esp_err_t err = region_service_detect(handle, timeout_ms);
        if (err != ESP_OK) {
            return err;
        }
        (void)xEventGroupWaitBits(handle->events, REGION_DETECT_DONE_BIT,
                                  pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
        (void)region_service_get_status(handle, &status);
    }
    *ret_area = status.area;
    return status.area == REGION_SERVICE_AREA_UNKNOWN ? ESP_ERR_TIMEOUT : ESP_OK;
}

esp_err_t region_service_select_area(region_service_handle_t handle,
                                     region_service_area_t area)
{
    if (!handle || (area != REGION_SERVICE_AREA_MAINLAND &&
                    area != REGION_SERVICE_AREA_INTERNATIONAL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = set_code(handle, area == REGION_SERVICE_AREA_MAINLAND
        ? REGION_SERVICE_CODE_MAINLAND : REGION_SERVICE_CODE_INTERNATIONAL);
    if (err == ESP_OK) {
        notify(handle);
    }
    return err;
}

esp_err_t region_service_reset(region_service_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = set_code(handle, "");
    if (err == ESP_OK) {
        notify(handle);
    }
    return err;
}

void region_service_delete(region_service_handle_t handle)
{
    if (!handle) {
        return;
    }
    if (handle->lock && handle->events) {
        xSemaphoreTake(handle->lock, portMAX_DELAY);
        handle->deadline_ms = 0;
        handle->cb = NULL;
        xSemaphoreGive(handle->lock);
        (void)xEventGroupWaitBits(handle->events, REGION_DETECT_DONE_BIT,
                                  pdFALSE, pdTRUE, portMAX_DELAY);
    }
    if (handle->events) {
        vEventGroupDelete(handle->events);
    }
    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
}
