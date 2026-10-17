/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gsp_sim_bridge.h"
#include "mosaic_app_catalog.h"

#include <time.h>

/* MSVC's CRT names the reentrant conversion localtime_s() with the argument
 * order reversed. Provide the POSIX spelling expected by weather_app.c. */
#if defined(_MSC_VER)
struct tm *localtime_r(const time_t *clock, struct tm *result)
{
    return localtime_s(result, clock) == 0 ? result : NULL;
}
#endif

/* Defined in weather_app.c; keeps the device and PC views identical. */
extern const mosaic_app_descriptor_t mosaic_weather_app;

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    if (mosaic_weather_app.on_started) {
        mosaic_weather_app.on_started(ui);
    }
    return ESP_GSP_OK;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    if (mosaic_weather_app.on_stopping) {
        mosaic_weather_app.on_stopping(ui);
    }
}
