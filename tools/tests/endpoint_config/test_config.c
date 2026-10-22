#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "settings_store.h"

static struct { char *key, *value; } store[128];
static int fail_after = -1;
static int find(const char *key)
{
    for (int i = 0; i < 128; i++) if (store[i].key && !strcmp(key, store[i].key)) return i;
    return -1;
}
esp_err_t settings_store_init(const settings_store_config_t *cfg) { return ESP_OK; }
esp_err_t settings_store_commit(void) { return ESP_OK; }
esp_err_t settings_store_has_key(const char *key, bool *exists) { *exists = find(key) >= 0; return ESP_OK; }
esp_err_t settings_store_get_string(const char *key, char *value, size_t size, const char *fallback)
{
    int i = find(key);
    const char *text = i < 0 ? fallback : store[i].value;
    if (strlen(text) >= size) return ESP_ERR_INVALID_SIZE;
    strlcpy(value, text, size);
    return ESP_OK;
}
esp_err_t settings_store_set_string(const char *key, const char *value)
{
    assert(strlen(key) <= 15);
    if (fail_after == 0) return ESP_FAIL;
    if (fail_after > 0) fail_after--;
    int i = find(key);
    if (i < 0) {
        for (i = 0; i < 128 && store[i].key; i++);
        assert(i < 128);
        store[i].key = strdup(key);
    }
    free(store[i].value);
    store[i].value = strdup(value);
    return ESP_OK;
}
esp_err_t settings_store_erase_key(const char *key)
{
    int i = find(key);
    if (i >= 0) { free(store[i].key); free(store[i].value); store[i].key = store[i].value = NULL; }
    return ESP_OK;
}
esp_err_t settings_store_erase_all(void)
{
    for (int i = 0; i < 128; i++) {
        free(store[i].key); free(store[i].value); store[i].key = store[i].value = NULL;
    }
    return ESP_OK;
}
static void reset(void)
{
    fail_after = -1;
    settings_store_erase_all();
    settings_store_set_string("cfg_ver", "1");
}
static void set(const char *key, const char *value) { assert(settings_store_set_string(key, value) == ESP_OK); }
static void test_anthropic(void)
{
    for (int interrupt = 0; interrupt < 6; interrupt++) {
        reset();
        set("llm_backend", "anthropic_compatible"); set("llm_auth_type", "none");
        set("llm_api_key", "existing-key"); set("llm_base_url", "https://old.test/v1");
        fail_after = interrupt;
        (void)app_config_init();
        fail_after = -1;
        assert(app_config_init() == ESP_OK);
        app_config_t config;
        assert(app_config_load(&config) == ESP_OK);
        assert(!strcmp(config.llm_endpoint_provider, "static"));
        assert(!strcmp(config.llm_auth_type, "api-key"));
        assert(!strcmp(config.llm_api_key, "existing-key"));
        assert(!strcmp(config.llm_base_url, "https://old.test/v1"));
        strcpy(config.llm_auth_type, "none"); config.llm_api_key[0] = 0;
        assert(app_config_save(&config) == ESP_OK);
        assert(app_config_init() == ESP_OK);
        assert(app_config_load(&config) == ESP_OK);
        assert(!strcmp(config.llm_auth_type, "none"));
    }
}
static void test_provider_roundtrip(void)
{
    reset(); assert(app_config_init() == ESP_OK);
    app_config_t config;
    assert(app_config_load(&config) == ESP_OK);
    strcpy(config.llm_endpoint_provider, "custom-source");
    strcpy(config.llm_backend_type, "anthropic_compatible");
    assert(app_config_save(&config) == ESP_OK);
    assert(app_config_load(&config) == ESP_OK);
    app_claw_config_t claw;
    app_config_to_claw(&config, &claw);
    assert(!strcmp(claw.llm_endpoint_provider, "custom-source"));
    assert(!strcmp(claw.llm_backend_type, "anthropic_compatible"));
}
#ifdef TEST_MOSAICO
static void test_trial(void)
{
    for (int interrupt = 0; interrupt < 11; interrupt++) {
        reset();
        set("llm_backend", "trial"); set("llm_auth_type", "trial"); set("asr_provider", "trial");
        set("llm_api_key", "debug-only"); set("asr_api_key", "debug-only");
        fail_after = interrupt; (void)app_config_init(); fail_after = -1;
        assert(app_config_init() == ESP_OK);
        app_config_t config;
        assert(app_config_load(&config) == ESP_OK);
        assert(!strcmp(config.llm_backend_type, "openai_compatible"));
        assert(!strcmp(config.llm_endpoint_provider, "trial"));
        assert(!strcmp(config.asr_provider, "qwen"));
        assert(!strcmp(config.asr_endpoint_provider, "trial"));
        assert(!strcmp(config.llm_api_key, "debug-only"));
        assert(!strcmp(config.asr_api_key, "debug-only"));
        assert(!config.llm_auth_type[0]);
        app_claw_config_t claw;
        app_config_to_claw(&config, &claw);
        assert(!strcmp(claw.asr_endpoint_provider, "trial"));
    }
    reset(); set("llm_backend", "openai_compatible"); set("llm_auth_type", "trial");
    set("asr_provider", "qwen");
    assert(app_config_init() == ESP_OK);
    app_config_t config; assert(app_config_load(&config) == ESP_OK);
    assert(!strcmp(config.llm_endpoint_provider, "trial"));
    assert(!strcmp(config.asr_endpoint_provider, "static"));
}
#endif
int main(void)
{
    test_anthropic(); test_provider_roundtrip();
#ifdef TEST_MOSAICO
    test_trial();
    puts("PASS: main configuration migration, interrupted migration, debug values and provider roundtrip");
#else
    puts("PASS: standalone configuration migration and provider roundtrip");
#endif
    settings_store_erase_all();
    return 0;
}
