/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "trial_provider.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "claw_endpoint.h"
#include "claw_utils_string.h"
#include "esp_check.h"
#include "esp_log.h"
#include "trial_region.h"
#include "trial_token.h"

static const char *TAG = "trial_provider";

#define TRIAL_ASR_PATH              "/v1/asr/realtime"
#define TRIAL_HTTPS_SCHEME          "https://"
#define TRIAL_WSS_SCHEME            "wss://"
#define TRIAL_AUTHORIZATION_HEADER  "Authorization"
#define TRIAL_BEARER_PREFIX         "Bearer "

typedef enum {
    TRIAL_SERVICE_INVALID = -1,
    TRIAL_SERVICE_LLM,
    TRIAL_SERVICE_ASR,
} trial_service_t;

typedef struct {
    trial_service_t service;
    trial_region_t hint;
} trial_endpoint_t;

static bool s_initialized;

static trial_service_t parse_service(const char *service)
{
    if (!service || strcmp(service, CLAW_ENDPOINT_SERVICE_LLM) == 0) {
        return TRIAL_SERVICE_LLM;
    }
    if (strcmp(service, CLAW_ENDPOINT_SERVICE_ASR) == 0) {
        return TRIAL_SERVICE_ASR;
    }
    return TRIAL_SERVICE_INVALID;
}

static esp_err_t set_service_url(trial_region_t region, trial_service_t service,
                                 claw_endpoint_resolved_t *out)
{
    const char *server_url = trial_region_server_url(region);
    const size_t https_len = strlen(TRIAL_HTTPS_SCHEME);

    if (!server_url) {
        return ESP_ERR_INVALID_STATE;
    }
    if (service == TRIAL_SERVICE_LLM) {
        char *api_url = claw_utils_string_dup_printf("%s/v1", server_url);
        esp_err_t err = api_url ? claw_endpoint_resolved_set_url(out, api_url) : ESP_ERR_NO_MEM;
        free(api_url);
        return err;
    }
    if (strncmp(server_url, TRIAL_HTTPS_SCHEME, https_len) != 0) {
        ESP_LOGE(TAG, "Trial server URL must start with %s", TRIAL_HTTPS_SCHEME);
        return ESP_ERR_INVALID_STATE;
    }

    char *url = claw_utils_string_dup_printf(TRIAL_WSS_SCHEME "%s" TRIAL_ASR_PATH,
                                             server_url + https_len);
    esp_err_t err = url ? claw_endpoint_resolved_set_url(out, url) : ESP_ERR_NO_MEM;
    free(url);
    return err;
}

static esp_err_t fill_resolved(trial_region_t region, trial_service_t service,
                               const char *token, claw_endpoint_resolved_t *out)
{
    esp_err_t err = set_service_url(region, service, out);
    if (err != ESP_OK) {
        return err;
    }

    char *auth = claw_utils_string_dup_printf(TRIAL_BEARER_PREFIX "%s", token);
    err = auth ? claw_endpoint_resolved_add_header(out, TRIAL_AUTHORIZATION_HEADER, auth)
               : ESP_ERR_NO_MEM;
    free(auth);
    if (err == ESP_OK) {
        err = claw_endpoint_resolved_add_header(out, TRIAL_FIRMWARE_VERSION_HEADER,
                                                trial_token_firmware_version());
    }
    return err;
}

static bool trial_is_configured(const claw_endpoint_config_t *config)
{
    return parse_service(config->service) != TRIAL_SERVICE_INVALID;
}

static esp_err_t trial_create(const claw_endpoint_config_t *config, void **out_ctx)
{
    trial_service_t service = parse_service(config->service);
    if (service == TRIAL_SERVICE_INVALID) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    trial_endpoint_t *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) {
        return ESP_ERR_NO_MEM;
    }
    endpoint->service = service;
    endpoint->hint = trial_region_from_hint(config->api_key);
    *out_ctx = endpoint;
    return ESP_OK;
}

static esp_err_t trial_acquire(void *ctx, const claw_endpoint_request_t *request,
                               claw_endpoint_resolved_t *out)
{
    trial_endpoint_t *endpoint = ctx;
    trial_region_t region = TRIAL_REGION_NONE;
    uint32_t generation = 0;
    char *token = NULL;

    esp_err_t err = trial_region_resolve(endpoint->hint, request, &region);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Trial region is unavailable: %s", esp_err_to_name(err));
        return err;
    }
    err = trial_token_get(region, request, &token, &generation);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Trial token is unavailable: %s", claw_endpoint_err_to_name(err));
        return err;
    }
    err = fill_resolved(region, endpoint->service, token, out);
    free(token);
    out->credential_id = generation;
    return err;
}

static bool trial_report(void *ctx, const claw_endpoint_resolved_t *resolved,
                         claw_endpoint_outcome_t outcome)
{
    (void)ctx;
    if (outcome == CLAW_ENDPOINT_OUTCOME_ACCEPTED) {
        trial_token_accept(resolved->credential_id);
        return false;
    }
    return trial_token_reject(resolved->credential_id);
}

static void trial_destroy(void *ctx)
{
    free(ctx);
}

static const claw_endpoint_provider_ops_t s_endpoint_ops = {
    .is_configured = trial_is_configured,
    .create = trial_create,
    .acquire = trial_acquire,
    .report = trial_report,
    .destroy = trial_destroy,
};

esp_err_t trial_provider_init(const trial_provider_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->region_service, ESP_ERR_INVALID_ARG, TAG,
                        "region service is required");
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(trial_region_init(config->region_service), TAG, "Failed to init region state");
    ESP_RETURN_ON_ERROR(trial_token_init(), TAG, "Failed to init token cache");
    ESP_RETURN_ON_ERROR(claw_endpoint_register_provider(TRIAL_PROVIDER_ID, &s_endpoint_ops),
                        TAG, "Failed to register endpoint provider");
    s_initialized = true;
    return ESP_OK;
}
