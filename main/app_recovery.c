// SPDX-License-Identifier: Apache-2.0
#include "app_recovery.h"

#include "esp_log.h"
#include "esp_ota_ops.h"

esp_err_t app_recovery_mark_healthy(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL || running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_ERR_NOT_FOUND) {
        // A native first install has blank otadata and no pending OTA receipt.
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    if (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED) {
        return ESP_OK;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_ERR_INVALID_STATE;
    }
    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI("app_recovery", "Claw startup confirmed on main_app");
    }
    return err;
}
