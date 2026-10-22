#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct { const char *namespace_name; } settings_store_config_t;
esp_err_t settings_store_init(const settings_store_config_t *config);
esp_err_t settings_store_has_key(const char *key, bool *exists);
esp_err_t settings_store_get_string(const char *key, char *value, size_t size, const char *fallback);
esp_err_t settings_store_set_string(const char *key, const char *value);
esp_err_t settings_store_erase_key(const char *key);
esp_err_t settings_store_erase_all(void);
esp_err_t settings_store_commit(void);
