/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "trial_token.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_endpoint.h"
#include "claw_http.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "psa_crypto_driver_esp_hmac_opaque.h"
#include "psa_crypto_driver_esp_hmac_opaque_contexts.h"
#include "trial_provider.h"

static const char *TAG = "trial_token";

#define TRIAL_TOKEN_MAX_LEN 2048
#define TRIAL_TOKEN_WAIT_MS 50U
#define TRIAL_TOKEN_TASK_STACK 6144U
#define TRIAL_TOKEN_TASK_PRIORITY 4
#define TRIAL_TOKEN_REFRESH_MARGIN_MS (60LL * 1000LL)
#define TRIAL_BACKOFF_INITIAL_MS (15LL * 1000LL)
#define TRIAL_BACKOFF_MAX_MS (10LL * 60LL * 1000LL)
#define TRIAL_HTTP_OK 200
#define TRIAL_HTTP_UNAUTHORIZED 401
#define TRIAL_HTTP_FORBIDDEN 403

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buffer_t;

typedef struct {
    char token[TRIAL_TOKEN_MAX_LEN];
    int64_t expires_at_ms;
    uint32_t generation;
    /* The service accepted this token at least once. */
    bool verified;
    /* Last server-side failure; repeated while backoff_until_ms is ahead. */
    esp_err_t failure;
    int64_t backoff_ms;
    int64_t backoff_until_ms;
    bool refreshing;
    uint32_t refresh_sequence;
    esp_err_t refresh_error;
} token_cache_t;

typedef struct {
    trial_region_t region;
    claw_endpoint_request_t request;
} refresh_job_t;

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_refresh_events;
static token_cache_t s_token_caches[TRIAL_REGION_COUNT];
static uint32_t s_last_generation;
static trial_provider_hmac_fn s_hmac_provider;

const char *trial_token_firmware_version(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    return description && description->version[0] ? description->version : "unknown";
}

static bool response_append(response_buffer_t *buffer, const char *data, size_t len)
{
    if (!buffer || !data || len == 0) {
        return true;
    }
    if (buffer->len + len + 1 > buffer->cap) {
        size_t next = buffer->cap ? buffer->cap : 1024;
        while (next < buffer->len + len + 1) {
            next *= 2;
        }
        char *grown = realloc(buffer->data, next);
        if (!grown) {
            return false;
        }
        buffer->data = grown;
        buffer->cap = next;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return true;
}

static esp_err_t receive_body(const char *data, size_t size, void *ctx)
{
    return response_append(ctx, data, size) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t post_json(const char *url, const char *body,
                           const claw_endpoint_request_t *request,
                           response_buffer_t *response, int *status_code)
{
    esp_err_t err = claw_endpoint_request_check(request);
    if (err != ESP_OK) return err;
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .is_async = true,
        .timeout_ms = claw_endpoint_request_remaining_ms(request),
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) {
        err = esp_http_client_set_header(client, TRIAL_FIRMWARE_VERSION_HEADER,
                                         trial_token_firmware_version());
    }
    if (err == ESP_OK) err = claw_http_send(client, body, request, receive_body, response);
    if (err == ESP_OK && status_code) *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t get_device_id(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    if (!out || out_size < 18 || esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    int written = snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return written > 0 && (size_t)written < out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t default_hmac_provider(uint32_t efuse_key_id, const void *message,
                                       size_t message_len, uint8_t out[32])
{
    if (!message || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attributes, 256);
    psa_set_key_lifetime(&attributes, PSA_KEY_LIFETIME_ESP_HMAC_VOLATILE);

    esp_hmac_opaque_key_t opaque_key = {
        .efuse_key_id = (hmac_key_id_t)efuse_key_id,
    };
    psa_key_id_t psa_key_id = 0;
    psa_status_t status = psa_import_key(&attributes, (const uint8_t *)&opaque_key,
                                         sizeof(opaque_key), &psa_key_id);
    if (status != PSA_SUCCESS) {
        psa_reset_key_attributes(&attributes);
        return ESP_ERR_INVALID_STATE;
    }

    size_t output_len = 0;
    status = psa_mac_compute(psa_key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                             (const uint8_t *)message, message_len,
                             out, 32, &output_len);
    psa_destroy_key(psa_key_id);
    psa_reset_key_attributes(&attributes);
    return status == PSA_SUCCESS && output_len == 32 ? ESP_OK : ESP_FAIL;
}

static esp_err_t hmac_message(const char *message, size_t message_len, uint8_t out[32])
{
    trial_provider_hmac_fn provider = s_hmac_provider
                                              ? s_hmac_provider
                                              : default_hmac_provider;
    return provider(CONFIG_TRIAL_DEVICE_HMAC_KEY_ID, message, message_len, out);
}

static size_t base64url_encode(const uint8_t *input, size_t input_len,
                               char *output, size_t output_size)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t required = (input_len * 4 + 2) / 3;
    if (!input || !output || output_size <= required) {
        return 0;
    }
    size_t in = 0;
    size_t out = 0;
    while (in < input_len) {
        uint32_t value = (uint32_t)input[in++] << 16;
        if (in < input_len) value |= (uint32_t)input[in++] << 8;
        if (in < input_len) value |= input[in++];
        output[out++] = alphabet[(value >> 18) & 0x3f];
        output[out++] = alphabet[(value >> 12) & 0x3f];
        if (out < required) output[out++] = alphabet[(value >> 6) & 0x3f];
        if (out < required) output[out++] = alphabet[value & 0x3f];
    }
    output[out] = '\0';
    return out;
}

static esp_err_t http_failure(esp_err_t transport_err, int status)
{
    if (transport_err != ESP_OK) {
        return transport_err;
    }
    if (status == TRIAL_HTTP_UNAUTHORIZED || status == TRIAL_HTTP_FORBIDDEN) {
        return CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t exchange_token(const char *base_url, token_cache_t *token_cache,
                                 const claw_endpoint_request_t *request)
{
    if (!base_url || !token_cache) {
        return ESP_ERR_INVALID_ARG;
    }

    char device_id[18] = {0};
    esp_err_t err = get_device_id(device_id, sizeof(device_id));
    if (err != ESP_OK) return err;

    cJSON *challenge_request = cJSON_CreateObject();
    if (!challenge_request || !cJSON_AddStringToObject(challenge_request, "device_id", device_id)) {
        cJSON_Delete(challenge_request);
        return ESP_ERR_NO_MEM;
    }
    char *challenge_body = cJSON_PrintUnformatted(challenge_request);
    cJSON_Delete(challenge_request);
    if (!challenge_body) return ESP_ERR_NO_MEM;

    response_buffer_t challenge_response = {0};
    int status = 0;
    char challenge_url[128];
    snprintf(challenge_url, sizeof(challenge_url), "%s/v1/device/challenge",
             base_url);
    err = post_json(challenge_url, challenge_body, request, &challenge_response, &status);
    cJSON_free(challenge_body);
    if (err != ESP_OK || status != TRIAL_HTTP_OK || !challenge_response.data) {
        ESP_LOGW(TAG, "Trial challenge request failed (status=%d)", status);
        free(challenge_response.data);
        return http_failure(err, status);
    }

    cJSON *challenge_root = cJSON_Parse(challenge_response.data);
    const char *challenge_id = challenge_root
        ? cJSON_GetStringValue(cJSON_GetObjectItem(challenge_root, "challenge_id")) : NULL;
    const char *nonce = challenge_root
        ? cJSON_GetStringValue(cJSON_GetObjectItem(challenge_root, "nonce")) : NULL;
    if (!challenge_id || !nonce) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t message_len = strlen(challenge_id) + strlen(nonce) + strlen(device_id) + 7;
    char *message = malloc(message_len + 1);
    uint8_t digest[32] = {0};
    char signature[48] = {0};
    if (!message) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return ESP_ERR_NO_MEM;
    }
    snprintf(message, message_len + 1, "v1\n%s\n%s\n%s",
             challenge_id, nonce, device_id);
    err = hmac_message(message, strlen(message), digest);
    free(message);
    if (err != ESP_OK || base64url_encode(digest, sizeof(digest), signature, sizeof(signature)) == 0) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }

    cJSON *token_request = cJSON_CreateObject();
    bool token_request_ok = token_request &&
        cJSON_AddStringToObject(token_request, "device_id", device_id) &&
        cJSON_AddStringToObject(token_request, "challenge_id", challenge_id) &&
        cJSON_AddStringToObject(token_request, "signature", signature);
    char *token_body = token_request_ok ? cJSON_PrintUnformatted(token_request) : NULL;
    cJSON_Delete(token_request);
    if (!token_body) {
        cJSON_Delete(challenge_root);
        free(challenge_response.data);
        return ESP_ERR_NO_MEM;
    }

    response_buffer_t token_response = {0};
    char token_url[128];
    snprintf(token_url, sizeof(token_url), "%s/v1/device/token",
             base_url);
    status = 0;
    err = post_json(token_url, token_body, request, &token_response, &status);
    cJSON_free(token_body);
    cJSON_Delete(challenge_root);
    free(challenge_response.data);
    if (err != ESP_OK || status != TRIAL_HTTP_OK || !token_response.data) {
        ESP_LOGW(TAG, "Trial token request failed (status=%d)", status);
        free(token_response.data);
        return http_failure(err, status);
    }

    cJSON *token_root = cJSON_Parse(token_response.data);
    const char *access_token = token_root
        ? cJSON_GetStringValue(cJSON_GetObjectItem(token_root, "access_token")) : NULL;
    cJSON *expires_item = token_root ? cJSON_GetObjectItem(token_root, "expires_in") : NULL;
    int expires_in = cJSON_IsNumber(expires_item) ? (int)expires_item->valuedouble : 0;
    if (!access_token || !access_token[0] ||
        strlen(access_token) >= sizeof(token_cache->token) || expires_in <= 0) {
        cJSON_Delete(token_root);
        free(token_response.data);
        return ESP_ERR_INVALID_RESPONSE;
    }
    strlcpy(token_cache->token, access_token, sizeof(token_cache->token));
    token_cache->expires_at_ms = esp_timer_get_time() / 1000 +
                                 (int64_t)expires_in * 1000;
    token_cache->verified = false;
    cJSON_Delete(token_root);
    free(token_response.data);
    return ESP_OK;
}

esp_err_t trial_token_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_refresh_events) s_refresh_events = xEventGroupCreate();
    if (s_lock && s_refresh_events) return ESP_OK;
    if (s_lock) vSemaphoreDelete(s_lock);
    if (s_refresh_events) vEventGroupDelete(s_refresh_events);
    s_lock = NULL;
    s_refresh_events = NULL;
    return ESP_ERR_NO_MEM;
}

esp_err_t trial_provider_validate_hmac_efuse_key(void)
{
#if !defined(CONFIG_TRIAL_DEVICE_HMAC_KEY_ID)
    ESP_LOGI(TAG, "eFuse HMAC key check skipped: key ID is not configured");
    return ESP_OK;
#elif CONFIG_TRIAL_DEVICE_HMAC_KEY_ID == -1
    ESP_LOGI(TAG, "eFuse HMAC key check skipped: key ID is disabled");
    return ESP_OK;
#else
    const int configured_key_id = CONFIG_TRIAL_DEVICE_HMAC_KEY_ID;
    if (configured_key_id < 0 || configured_key_id >= HMAC_KEY_MAX) {
        ESP_LOGE(TAG, "configured eFuse HMAC key ID is out of range: %d",
                 configured_key_id);
        return ESP_ERR_INVALID_ARG;
    }

    const hmac_key_id_t key_id = (hmac_key_id_t)configured_key_id;
    const esp_efuse_block_t key_block =
        (esp_efuse_block_t)(EFUSE_BLK_KEY0 + configured_key_id);
    const esp_efuse_purpose_t purpose = esp_efuse_get_key_purpose(key_block);
    if (purpose != ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
        ESP_LOGE(TAG,
                 "eFuse key block %d for HMAC key ID %d has purpose %d, expected HMAC_UP",
                 key_block, configured_key_id, purpose);
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_efuse_get_key_dis_read(key_block) &&
        esp_efuse_block_is_empty(key_block)) {
        ESP_LOGE(TAG, "eFuse key block %d for HMAC key ID %d is empty",
                 key_block, configured_key_id);
        return ESP_ERR_NOT_FOUND;
    }

    static const uint8_t probe[] = "trial-auth-hmac-efuse-check";
    uint8_t digest[32];
    ESP_RETURN_ON_ERROR(esp_hmac_calculate(key_id, probe, sizeof(probe) - 1U,
                                            digest),
                        TAG, "HMAC calculation using eFuse key ID %d failed",
                        configured_key_id);
    ESP_LOGI(TAG, "eFuse HMAC key ID %d in block %d validated",
             configured_key_id, key_block);
    return ESP_OK;
#endif
}

esp_err_t trial_provider_set_hmac_provider(trial_provider_hmac_fn provider)
{
    if (!provider) {
        return ESP_ERR_INVALID_ARG;
    }
    s_hmac_provider = provider;
    return ESP_OK;
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool failure_is_server_side(esp_err_t err)
{
    return err == CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE ||
           err == CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED ||
           err == ESP_ERR_INVALID_RESPONSE;
}

static void drop_token_locked(token_cache_t *cache)
{
    cache->token[0] = '\0';
    cache->expires_at_ms = 0;
    cache->verified = false;
}

static void record_failure_locked(token_cache_t *cache, esp_err_t err)
{
    if (!failure_is_server_side(err)) {
        return;
    }
    cache->failure = err;
    cache->backoff_ms = cache->backoff_ms
        ? (cache->backoff_ms * 2 > TRIAL_BACKOFF_MAX_MS ? TRIAL_BACKOFF_MAX_MS : cache->backoff_ms * 2)
        : TRIAL_BACKOFF_INITIAL_MS;
    cache->backoff_until_ms = now_ms() + cache->backoff_ms;
    ESP_LOGW(TAG, "Trial credentials unavailable (%s), backing off %lld s",
             claw_endpoint_err_to_name(err), (long long)(cache->backoff_ms / 1000));
}

static void clear_failure_locked(token_cache_t *cache)
{
    cache->failure = ESP_OK;
    cache->backoff_ms = 0;
    cache->backoff_until_ms = 0;
}

static token_cache_t *find_generation_locked(uint32_t generation)
{
    for (size_t i = 0; i < TRIAL_REGION_COUNT; i++) {
        if (generation && s_token_caches[i].generation == generation) {
            return &s_token_caches[i];
        }
    }
    return NULL;
}

/* A region has at most one exchange in flight. The job owns its deadline;
 * individual waiters can leave without invalidating its buffers or context. */
static void refresh_task(void *arg)
{
    refresh_job_t *job = arg;
    token_cache_t *fresh = calloc(1, sizeof(*fresh));
    esp_err_t err = fresh ? exchange_token(trial_region_server_url(job->region), fresh, &job->request)
                          : ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    token_cache_t *cache = &s_token_caches[job->region];
    if (err == ESP_OK) {
        strlcpy(cache->token, fresh->token, sizeof(cache->token));
        cache->expires_at_ms = fresh->expires_at_ms;
        if (++s_last_generation == 0) ++s_last_generation;
        cache->generation = s_last_generation;
        cache->verified = false;
    } else {
        record_failure_locked(cache, err);
        if (now_ms() >= cache->expires_at_ms) drop_token_locked(cache);
    }
    cache->refresh_error = err;
    cache->refresh_sequence++;
    cache->refreshing = false;
    xEventGroupSetBits(s_refresh_events, BIT(job->region));
    xSemaphoreGive(s_lock);
    free(fresh);
    free(job);
    vTaskDelete(NULL);
}

static esp_err_t copy_token_locked(const token_cache_t *cache, char **out_token,
                                   uint32_t *out_generation)
{
    *out_token = strdup(cache->token);
    if (!*out_token) return ESP_ERR_NO_MEM;
    *out_generation = cache->generation;
    return ESP_OK;
}

esp_err_t trial_token_get(trial_region_t region, const claw_endpoint_request_t *request,
                          char **out_token, uint32_t *out_generation)
{
    if (!trial_region_server_url(region) || !request || !out_token || !out_generation) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_token = NULL;
    *out_generation = 0;
    if (!s_lock || !s_refresh_events) return ESP_ERR_INVALID_STATE;

    token_cache_t *cache = &s_token_caches[region];
    bool waiting = false;
    uint32_t sequence = 0;
    for (;;) {
        esp_err_t err = claw_endpoint_request_check(request);
        if (err != ESP_OK) return err;
        uint32_t remaining = claw_endpoint_request_remaining_ms(request);
        uint32_t wait_ms = remaining < TRIAL_TOKEN_WAIT_MS ? remaining : TRIAL_TOKEN_WAIT_MS;
        TickType_t ticks = pdMS_TO_TICKS(wait_ms);
        if (!ticks) ticks = 1;
        if (xSemaphoreTake(s_lock, ticks) != pdTRUE) continue;
        int64_t now = now_ms();
        const bool usable = cache->token[0] && now < cache->expires_at_ms;
        const bool refreshed = waiting && sequence != cache->refresh_sequence;
        const bool backoff = cache->failure != ESP_OK && now < cache->backoff_until_ms;
        if (usable && (now + TRIAL_TOKEN_REFRESH_MARGIN_MS < cache->expires_at_ms ||
                       cache->refreshing || refreshed || backoff)) {
            err = copy_token_locked(cache, out_token, out_generation);
            xSemaphoreGive(s_lock);
            return err;
        }
        if (backoff || (refreshed && cache->refresh_error != ESP_OK)) {
            err = backoff ? cache->failure : cache->refresh_error;
            xSemaphoreGive(s_lock);
            return err;
        }
        if (!cache->refreshing) {
            refresh_job_t *job = calloc(1, sizeof(*job));
            if (!job) {
                xSemaphoreGive(s_lock);
                return ESP_ERR_NO_MEM;
            }
            job->region = region;
            job->request.deadline_ms = request->deadline_ms;
            cache->refreshing = true;
            xEventGroupClearBits(s_refresh_events, BIT(region));
            if (xTaskCreate(refresh_task, "trial_token", TRIAL_TOKEN_TASK_STACK, job,
                             TRIAL_TOKEN_TASK_PRIORITY, NULL) != pdPASS) {
                cache->refreshing = false;
                free(job);
                xSemaphoreGive(s_lock);
                return ESP_ERR_NO_MEM;
            }
        }
        sequence = cache->refresh_sequence;
        waiting = true;
        xSemaphoreGive(s_lock);
        (void)xEventGroupWaitBits(s_refresh_events, BIT(region), pdFALSE, pdFALSE, ticks);
    }
}

void trial_token_accept(uint32_t generation)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    token_cache_t *cache = find_generation_locked(generation);
    if (cache && cache->token[0]) {
        cache->verified = true;
        clear_failure_locked(cache);
    }
    xSemaphoreGive(s_lock);
}

bool trial_token_reject(uint32_t generation)
{
    bool retry = true;

    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    token_cache_t *cache = find_generation_locked(generation);
    if (!cache) {
        /* Already superseded by a newer token. */
    } else if (!cache->token[0]) {
        retry = cache->failure == ESP_OK;
    } else if (cache->verified) {
        ESP_LOGW(TAG, "Trial token expired or revoked, exchanging a new one");
        drop_token_locked(cache);
    } else {
        ESP_LOGW(TAG, "Freshly issued Trial token was rejected");
        drop_token_locked(cache);
        record_failure_locked(cache, CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED);
        retry = false;
    }
    xSemaphoreGive(s_lock);
    return retry;
}
