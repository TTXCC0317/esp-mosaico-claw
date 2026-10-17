/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_gsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Active UI language. English keeps the values baked into the scene bundle. */
typedef enum {
    MOSAIC_I18N_LANG_EN = 0,
    MOSAIC_I18N_LANG_ZH = 1,
} mosaic_i18n_lang_t;

/** One bind -> Chinese override. Generated per App from the scene dictionary. */
typedef struct {
    uint16_t bind;
    const char *zh;
} mosaic_i18n_entry_t;

/** Parses a persisted language tag ("en" / "zh", unknown falls back to EN). */
mosaic_i18n_lang_t mosaic_i18n_parse(const char *tag);
const char *mosaic_i18n_tag(mosaic_i18n_lang_t lang);

/** Set once during start-up, before any App scene is created. */
void mosaic_i18n_set_language(mosaic_i18n_lang_t lang);
mosaic_i18n_lang_t mosaic_i18n_language(void);
bool mosaic_i18n_is_chinese(void);

/** Runtime selector for dynamic (non-baked) strings. Never NULL. */
const char *mosaic_i18n_tr(const char *en, const char *zh);

/** Apply baked-string overrides to a freshly created scene. No-op for EN. */
void mosaic_i18n_apply(esp_gsp_handle_t ui,
                       const mosaic_i18n_entry_t *entries, size_t count);

#ifdef __cplusplus
}
#endif