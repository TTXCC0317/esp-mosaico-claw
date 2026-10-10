/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "bmi270_api.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_log.h"
#include "gen_board_device_custom.h"

static const char *TAG = "MOSAICO_IMU";

static esp_err_t bmi270_configure(bmi270_handle_t sensor)
{
    const uint8_t sensors[] = {BMI2_ACCEL, BMI2_GYRO};
    struct bmi2_sens_config config[2] = {0};
    config[0].type = BMI2_ACCEL;
    config[1].type = BMI2_GYRO;

    int8_t result = bmi2_set_adv_power_save(BMI2_DISABLE, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "disable advanced power save failed: %d", result);
    result = bmi2_get_sensor_config(config, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "get sensor config failed: %d", result);

    config[0].cfg.acc.odr = BMI2_ACC_ODR_100HZ;
    config[0].cfg.acc.range = BMI2_ACC_RANGE_2G;
    config[0].cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    config[0].cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.odr = BMI2_GYR_ODR_100HZ;
    config[1].cfg.gyr.range = BMI2_GYR_RANGE_500;
    config[1].cfg.gyr.bwp = BMI2_GYR_NORMAL_MODE;
    config[1].cfg.gyr.noise_perf = BMI2_PERF_OPT_MODE;
    config[1].cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;

    result = bmi2_set_sensor_config(config, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "set sensor config failed: %d", result);
    result = bmi270_sensor_enable(sensors, 2, sensor);
    ESP_RETURN_ON_FALSE(result == BMI2_OK, ESP_FAIL, TAG,
                        "enable accel/gyro failed: %d", result);
    return ESP_OK;
}

static int imu_sensor_init(void *config, int cfg_size, void **device_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && device_handle != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid init arguments");
    ESP_RETURN_ON_FALSE(cfg_size == sizeof(dev_custom_imu_sensor_config_t),
                        ESP_ERR_INVALID_SIZE, TAG, "invalid config size: %d",
                        cfg_size);
    *device_handle = NULL;
    const dev_custom_imu_sensor_config_t *imu_cfg = config;
    ESP_RETURN_ON_FALSE(imu_cfg->chip != NULL &&
                            strcmp(imu_cfg->chip, "bmi270") == 0,
                        ESP_ERR_INVALID_ARG, TAG, "unsupported IMU chip");

    ESP_RETURN_ON_FALSE(imu_cfg->addr == BMI2_I2C_PRIM_ADDR || imu_cfg->addr == BMI2_I2C_SEC_ADDR,
                        ESP_ERR_INVALID_ARG, TAG, "invalid BMI270 address");
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(esp_board_periph_get_handle(imu_cfg->peripheral_name, (void **)&bus),
                        TAG, "get I2C bus failed");
    bmi270_handle_t sensor = NULL;
    /* Borrow the native board bus; deleting the sensor leaves the bus alive. */
    esp_err_t err = bmi270_sensor_create_from_master_bus_with_address(bus, imu_cfg->addr, &sensor,
        bmi270_config_file, BMI2_GYRO_CROSS_SENS_ENABLE | BMI2_CRT_RTOSK_ENABLE);
    if (err == ESP_OK) {
        err = bmi270_configure(sensor);
    }
    if (err != ESP_OK) {
        if (sensor != NULL) {
            esp_err_t cleanup_err = bmi270_sensor_del(&sensor);
            if (cleanup_err != ESP_OK) ESP_LOGE(TAG, "IMU cleanup failed: %s", esp_err_to_name(cleanup_err));
        }
        ESP_LOGE(TAG, "IMU init failed: %s", esp_err_to_name(err));
        return err;
    }
    *device_handle = sensor;
    ESP_LOGI(TAG, "BMI270 initialized at 0x%02x", imu_cfg->addr);
    return ESP_OK;
}

static int imu_sensor_deinit(void *device_handle)
{
    bmi270_handle_t sensor = device_handle;
    ESP_RETURN_ON_FALSE(sensor != NULL, ESP_ERR_INVALID_ARG, TAG,
                        "invalid IMU handle");
    return bmi270_sensor_del(&sensor);
}

esp_err_t esp_mosaico_imu_read(void *device_handle,
                               struct bmi2_sens_data *sample)
{
    bmi270_handle_t sensor = device_handle;
    ESP_RETURN_ON_FALSE(sensor != NULL &&
                            sample != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid read arguments");
    memset(sample, 0, sizeof(*sample));
    return bmi2_get_sensor_data(sample, sensor) == BMI2_OK
               ? ESP_OK
               : ESP_FAIL;
}

CUSTOM_DEVICE_IMPLEMENT(imu_sensor, imu_sensor_init, imu_sensor_deinit);
