#include "interact_backend.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "mosaico_module_interact.h"
#include "mosaico_camera_service.h"
#include "mosaic_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#define INTERACT_NVS_NAMESPACE "mosaic_interact"
/* Bumped key: the previous default was touch input, which is no longer what the
 * board should boot with. A fresh key retires that stale persisted choice once,
 * while later manual switches still persist normally. */
#define INTERACT_NVS_INPUT_MODE_KEY "input_mode_v2"
#define INTERACT_NVS_AUTO_CAMERA_KEY "auto_cam"
#define INTERACT_WORKER_STACK 6144
#define INTERACT_WORKER_PRIORITY 4
#define INTERACT_POLL_MS 50
/* Motion-triggered camera switching is deliberately conservative: an edge is
 * only accepted after the sensor has been quiet for a long stretch, so someone
 * lingering in front of the board cannot keep yanking the UI back into the
 * camera right after the user closed it. */
#define INTERACT_PIR_COOLDOWN_MS 20000
#define INTERACT_PIR_REARM_QUIET_MS 60000
/* Never auto-open the camera while the board is still starting up. */
#define INTERACT_PIR_STARTUP_GRACE_MS 20000
/* 50 ms polls: two matching samples reject contact bounce, 600 ms is a hold. */
#define INTERACT_KEY_DEBOUNCE_SAMPLES 2
#define INTERACT_KEY_LONG_PRESS_MS 600

static const char *TAG = "interact_svc";
static SemaphoreHandle_t s_lock, s_done;
static bool s_running, s_stop, s_reconnect, s_ir;
static uint8_t s_toggle;
static uint8_t s_ir_addr, s_ir_cmd;
static interact_rgb_t s_manual_color[INTERACT_LED_COUNT];
static uint8_t s_manual_on;
static bool s_auto_camera = true;
static void (*s_key_cb)(int key_id, bool is_long, void *ctx);
static void *s_key_ctx;
static mosaico_interact_button_mode_t s_button_mode = MOSAICO_INTERACT_BUTTON_MODE_GPIO;
static interact_snapshot_t s_state;

typedef struct {
    bool stable;   /* Debounced level. */
    bool long_sent; /* Long press already reported for this hold. */
    uint8_t samples;
    TickType_t down_tick;
} interact_key_state_t;

static const interact_rgb_t s_colors[] = {
    {255, 64, 64}, {64, 255, 112}, {64, 128, 255}, {192, 96, 255}, {255, 245, 160}, {255, 245, 160},
};
static const interact_rgb_t s_pressed_color = {76, 255, 133}; // Match the #4CFF85 screen overlay.
static const interact_rgb_t s_black = {0, 0, 0};

static void load_input_mode(void)
{
    nvs_handle_t nvs;
    uint8_t mode = MOSAICO_INTERACT_BUTTON_MODE_GPIO;
    s_button_mode = MOSAICO_INTERACT_BUTTON_MODE_GPIO;
    esp_err_t err = nvs_open(INTERACT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open input mode: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_get_u8(nvs, INTERACT_NVS_INPUT_MODE_KEY, &mode);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK || (mode != MOSAICO_INTERACT_BUTTON_MODE_GPIO && mode != MOSAICO_INTERACT_BUTTON_MODE_TOUCH)) {
        ESP_LOGW(TAG, "load input mode: %s, value=%u", esp_err_to_name(err), (unsigned)mode);
        return;
    }
    s_button_mode = (mosaico_interact_button_mode_t)mode;
}

static void save_input_mode(mosaico_interact_button_mode_t mode)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INTERACT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open input mode for save: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, INTERACT_NVS_INPUT_MODE_KEY, (uint8_t)mode);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "save input mode: %s", esp_err_to_name(err));
}

static void load_auto_camera(void)
{
    nvs_handle_t nvs;
    uint8_t enabled = 1;
    s_auto_camera = true;
    esp_err_t err = nvs_open(INTERACT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open auto camera: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_get_u8(nvs, INTERACT_NVS_AUTO_CAMERA_KEY, &enabled);
    nvs_close(nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load auto camera: %s", esp_err_to_name(err));
        return;
    }
    s_auto_camera = enabled != 0;
}

static void save_auto_camera(bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(INTERACT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open auto camera for save: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, INTERACT_NVS_AUTO_CAMERA_KEY, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "save auto camera: %s", esp_err_to_name(err));
}

static void publish(const interact_snapshot_t *state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = *state;
    xSemaphoreGive(s_lock);
}

static bool close_board(mosaico_interact_handle_t *board)
{
    if (!*board) return true;
    esp_err_t err = mosaico_interact_close(*board);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "close board: %s", esp_err_to_name(err));
        return false;
    }
    *board = NULL;
    return true;
}

/* Read the current manual LED intent and the pending command bits atomically. */
static void take_commands(bool *stop, bool *reconnect, bool *ir,
                          uint8_t *ir_addr, uint8_t *ir_cmd, uint8_t *toggle,
                          uint8_t *manual_on, interact_rgb_t *manual_color,
                          mosaico_interact_button_mode_t *button_mode)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *stop = s_stop;
    *reconnect = s_reconnect;
    *ir = s_ir;
    *ir_addr = s_ir_addr;
    *ir_cmd = s_ir_cmd;
    *toggle = s_toggle;
    s_reconnect = s_ir = false;
    s_toggle = 0;
    *button_mode = s_button_mode;
    /* Apply pending App toggles to the manual LED intent. */
    for (unsigned i = 0; i < INTERACT_LED_COUNT; ++i) {
        if (!(*toggle & (1U << i))) continue;
        if (s_manual_on & (1U << i)) {
            s_manual_on &= (uint8_t)~(1U << i);
            s_manual_color[i] = s_black;
        } else {
            s_manual_on |= (uint8_t)(1U << i);
            s_manual_color[i] = s_colors[i];
        }
    }
    *manual_on = s_manual_on;
    memcpy(manual_color, s_manual_color, sizeof(s_manual_color));
    xSemaphoreGive(s_lock);
}

static void take_key_cb(void (**cb)(int, bool, void *), void **ctx)
{
    if (!s_lock) {
        *cb = s_key_cb;
        *ctx = s_key_ctx;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *cb = s_key_cb;
    *ctx = s_key_ctx;
    xSemaphoreGive(s_lock);
}

/* Feed one raw sample per key and report debounced short and long presses. */
static void route_keys(interact_key_state_t *keys, const bool raw[2],
                       void (*cb)(int, bool, void *), void *ctx)
{
    for (int i = 0; i < 2; ++i) {
        interact_key_state_t *key = &keys[i];
        if (raw[i] != key->stable) {
            if (key->samples < INTERACT_KEY_DEBOUNCE_SAMPLES) {
                ++key->samples;
                continue;
            }
            key->stable = raw[i];
            key->samples = 0;
            if (raw[i]) {
                key->down_tick = xTaskGetTickCount();
                key->long_sent = false;
            } else if (!key->long_sent) {
                cb(i, false, ctx);
            }
            continue;
        }
        key->samples = 0;
        if (raw[i] && !key->long_sent &&
            (int32_t)(xTaskGetTickCount() - key->down_tick) >=
                (int32_t)pdMS_TO_TICKS(INTERACT_KEY_LONG_PRESS_MS)) {
            key->long_sent = true;
            cb(i, true, ctx);
        }
    }
}

static void worker(void *arg)
{
    (void)arg;
    mosaico_interact_handle_t board = NULL;
    interact_snapshot_t state = {.slot = -1, .touch_input_selected = s_button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH};
    unsigned ticks = 0;
    uint8_t failed_leds = 0;
    /* prev_pir starts high so a sensor that is already active in the first
     * sample is not mistaken for a fresh edge. */
    bool prev_pir = true, pir_low = false, pir_armed = false;
    TickType_t pir_low_tick = 0;
    const TickType_t worker_start = xTaskGetTickCount();
    bool have_applied[INTERACT_LED_COUNT] = {false};
    interact_rgb_t applied[INTERACT_LED_COUNT] = {{0}};
    interact_key_state_t keys[2] = {{0}};
    TickType_t ir_until = 0;
    TickType_t pir_until = 0;
    esp_err_t err = mosaico_module_mgr_init(NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "module manager init: %s", esp_err_to_name(err));
        snprintf(state.status, sizeof(state.status), "Init failed");
    }
    for (;;) {
        bool stop, reconnect, ir;
        uint8_t ir_addr, ir_cmd, toggle, manual_on;
        interact_rgb_t manual_color[INTERACT_LED_COUNT];
        mosaico_interact_button_mode_t button_mode;
        take_commands(&stop, &reconnect, &ir, &ir_addr, &ir_cmd, &toggle,
                      &manual_on, manual_color, &button_mode);
        if (stop) {
            break;
        }
        if (reconnect) {
            if (!close_board(&board)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_reconnect = true;
                xSemaphoreGive(s_lock);
                state.ready = state.key_l = state.key_r = state.pir = false;
                snprintf(state.status, sizeof(state.status), "Board disconnected");
                publish(&state);
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            state = (interact_snapshot_t){.slot = -1, .touch_input_selected = button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH};
            ticks = 0;
            failed_leds = 0;
            prev_pir = true;
            pir_low = false;
            pir_armed = false;
            memset(have_applied, 0, sizeof(have_applied));
            memset(keys, 0, sizeof(keys));
            ir = false;
        }
        if (board && ticks % 5 == 0) {
            mosaico_module_mgr_info_t info = {0};
            if (mosaico_module_mgr_get_info((mosaico_module_mgr_slot_t)state.slot, &info) == ESP_OK &&
                info.presence == MOSAICO_MODULE_PRESENCE_ABSENT) {
                state.ready = state.key_l = state.key_r = state.pir = false;
                state.leds = state.pressed_leds = 0;
                snprintf(state.status, sizeof(state.status), "Board disconnected");
                publish(&state);
                const bool closed = close_board(&board);
                if (closed) {
                    state.slot = -1;
                    failed_leds = 0;
                    ticks = 0;
                    memset(have_applied, 0, sizeof(have_applied));
                    memset(keys, 0, sizeof(keys));
                }
                vTaskDelay(pdMS_TO_TICKS(closed ? 50 : 250));
                continue;
            }
        }
        if (!board && err == ESP_OK && ticks % 20 == 0) {
            mosaico_module_mgr_info_t info = {0};
            bool found = false, busy = false;
            for (int slot = 0; slot < 2; ++slot) {
                if (mosaico_module_mgr_get_info(slot, &info) != ESP_OK || info.presence != MOSAICO_MODULE_PRESENCE_PRESENT ||
                    info.descriptor_state != MOSAICO_MODULE_DESCRIPTOR_VALID || info.eeprom.board_type != MOSAICO_BOARD_TYPE_INTERACT) continue;
                busy |= info.owner_state != MOSAICO_MODULE_OWNER_FREE;
                if (info.owner_state == MOSAICO_MODULE_OWNER_FREE) {
                    found = true;
                    break;
                }
            }
            if (found) {
                mosaico_interact_config_t config = MOSAICO_INTERACT_DEFAULT_CONFIG();
                config.slot = info.slot;
                config.button_mode = button_mode;
                esp_err_t result = mosaico_interact_open(&config, &board);
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "open slot %d: %s", info.slot, esp_err_to_name(result));
                    snprintf(state.status, sizeof(state.status), "Open failed: %s", esp_err_to_name(result));
                } else {
                    state.ready = true;
                    state.slot = info.slot;
                    state.leds = 0;
                    state.pressed_leds = 0;
                    failed_leds = 0;
                    memset(have_applied, 0, sizeof(have_applied));
                    memset(keys, 0, sizeof(keys));
                    prev_pir = true;
                    pir_low = false;
                    pir_armed = false;
                    snprintf(state.status, sizeof(state.status), "%s connected", info.slot == 0 ? "Left" : "Right");
                }
            } else {
                snprintf(state.status, sizeof(state.status), busy ? "Board in use" : "Connect interaction board");
            }
        }
        // Do not report the initial unknown state as a missing module.
        state.checked = true;
        if (board) {
            if (ir && (int32_t)(xTaskGetTickCount() - ir_until) >= 0) {
                snprintf(state.ir_status, sizeof(state.ir_status), "IR sending");
                publish(&state);
                esp_err_t result = mosaico_interact_ir_send_nec(board, ir_addr, ir_cmd);
                snprintf(state.ir_status, sizeof(state.ir_status), "%s", result == ESP_OK ? "IR sent" : "IR failed");
                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "IR send: %s", esp_err_to_name(result));
                }
                ir_until = xTaskGetTickCount() + pdMS_TO_TICKS(700);
            }
            mosaico_interact_inputs_t data;
            esp_err_t result = mosaico_interact_read_inputs(board, &data);
            if (result == ESP_OK) {
                if (!state.ready) {
                    snprintf(state.status, sizeof(state.status), "%s connected", state.slot == 0 ? "Left" : "Right");
                }
                state.key_l = data.left_pressed;
                state.key_r = data.right_pressed;
                state.pir = data.motion_detected;
                state.light = data.light_level;
                state.ready = true;
                void (*key_cb)(int, bool, void *) = NULL;
                void *key_ctx = NULL;
                take_key_cb(&key_cb, &key_ctx);
                if (key_cb) {
                    const bool key_raw[2] = {state.key_l, state.key_r};
                    route_keys(keys, key_raw, key_cb, key_ctx);
                }
            } else {
                if (state.ready) {
                    ESP_LOGW(TAG, "read: %s", esp_err_to_name(result));
                }
                state.ready = state.key_l = state.key_r = state.pir = false;
                snprintf(state.status, sizeof(state.status), "Sensor read failed");
            }
            /* Track how long the sensor has stayed quiet. A fresh motion edge is
             * only accepted once the board has been still long enough, so a
             * presence that keeps re-triggering the AS312 cannot re-open the
             * camera the user just closed. */
            if (!state.pir) {
                if (!pir_low) {
                    pir_low = true;
                    pir_low_tick = xTaskGetTickCount();
                }
            } else if (pir_low) {
                pir_low = false;
                pir_armed = (int32_t)(xTaskGetTickCount() - pir_low_tick) >=
                            (int32_t)pdMS_TO_TICKS(INTERACT_PIR_REARM_QUIET_MS);
            }
            const bool pir_rising = state.pir && !prev_pir;
            const bool grace_over = (int32_t)(xTaskGetTickCount() - worker_start) >=
                                    (int32_t)pdMS_TO_TICKS(INTERACT_PIR_STARTUP_GRACE_MS);
            if (pir_rising && pir_armed && grace_over &&
                interact_backend_auto_camera_enabled() &&
                (int32_t)(xTaskGetTickCount() - pir_until) >= 0 &&
                mosaico_camera_service_is_available() &&
                !mosaic_ui_active_app_is("camera") &&
                !mosaic_ui_active_app_is("setup_center") &&
                !mosaic_ui_active_app_is("welcome")) {
                esp_err_t open_err = mosaic_ui_open_app("camera");
                if (open_err == ESP_OK) {
                    /* Stay disarmed until the board is quiet again. */
                    pir_armed = false;
                    pir_low = true;
                    pir_low_tick = xTaskGetTickCount();
                    pir_until = xTaskGetTickCount() + pdMS_TO_TICKS(INTERACT_PIR_COOLDOWN_MS);
                    ESP_LOGI(TAG, "motion detected: opening camera");
                } else {
                    ESP_LOGD(TAG, "motion detected: camera unavailable (%s)", esp_err_to_name(open_err));
                }
            }
            prev_pir = state.pir;
            /* LED4/LED5 are the backlights of KEY_L/KEY_R: a press or a touch on
             * either pad lights its backlight while held (short and long presses
             * alike) and the backlight returns to off on release. */
            const uint8_t pressed_leds = (state.key_l ? (1U << INTERACT_LED_KEY_L) : 0) |
                                         (state.key_r ? (1U << INTERACT_LED_KEY_R) : 0);
            state.leds = (uint8_t)(manual_on & ((1U << INTERACT_LED_USER_COUNT) - 1U));
            state.pressed_leds = pressed_leds;
            for (unsigned i = 0; i < INTERACT_LED_COUNT; ++i) {
                const uint8_t bit = (uint8_t)(1U << i);
                interact_rgb_t want;
                if (pressed_leds & bit) want = s_pressed_color;
                else if (manual_on & bit) want = manual_color[i];
                else want = s_black;
                if (have_applied[i] && !memcmp(&want, &applied[i], sizeof(want))) continue;
                mosaico_interact_rgb_t hw = {.r = want.r, .g = want.g, .b = want.b};
                result = mosaico_interact_led_set(board, (uint8_t)i, hw);
                if (result == ESP_OK) {
                    applied[i] = want;
                    have_applied[i] = true;
                    failed_leds &= (uint8_t)~bit;
                } else {
                    if (!(failed_leds & bit)) ESP_LOGW(TAG, "LED %u: %s", i, esp_err_to_name(result));
                    failed_leds |= bit;
                    snprintf(state.status, sizeof(state.status), "LED write failed");
                }
            }
        }
        if ((int32_t)(xTaskGetTickCount() - ir_until) >= 0) {
            state.ir_status[0] = '\0';
        }
        publish(&state);
        ++ticks;
        vTaskDelay(pdMS_TO_TICKS(INTERACT_POLL_MS));
    }
    if (board) {
        (void)close_board(&board);
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

void interact_backend_start(void)
{
    if (s_running) return;
    load_input_mode();
    load_auto_camera();
    s_state = (interact_snapshot_t){.slot = -1, .touch_input_selected = s_button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH};
    snprintf(s_state.status, sizeof(s_state.status), "Connecting...");
    s_manual_on = 0;
    memset(s_manual_color, 0, sizeof(s_manual_color));
    s_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    s_stop = s_reconnect = s_ir = false;
    s_toggle = 0;
    s_ir_addr = 0x00;
    s_ir_cmd = 0x10;
    if (!s_lock || !s_done || xTaskCreate(worker, "interact", INTERACT_WORKER_STACK, NULL, INTERACT_WORKER_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "worker allocation failed");
        snprintf(s_state.status, sizeof(s_state.status), "Out of memory");
        s_state.checked = true;
        if (s_lock) vSemaphoreDelete(s_lock);
        if (s_done) vSemaphoreDelete(s_done);
        s_lock = s_done = NULL;
        return;
    }
    s_running = true;
}

void interact_backend_stop(void)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stop = true;
    xSemaphoreGive(s_lock);
    // Join before destroying synchronization or the next app's hardware.
    xSemaphoreTake(s_done, portMAX_DELAY);
    vSemaphoreDelete(s_done);
    vSemaphoreDelete(s_lock);
    s_done = s_lock = NULL;
    s_running = false;
}

void interact_backend_command(interact_command_t command)
{
    if (!s_running) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.ready && !s_reconnect) {
        /* LED4/LED5 are the key backlights: they are driven purely by the key
         * state, so manual toggles for them are ignored. */
        if (command >= INTERACT_COMMAND_LED_0 && command < (int)(INTERACT_COMMAND_LED_0 + INTERACT_LED_USER_COUNT)) {
            s_toggle ^= 1U << command;
        }
        if (command == INTERACT_COMMAND_IR_SEND && !s_ir && !s_state.ir_status[0]) {
            s_ir = true;
            s_ir_addr = 0x00;
            s_ir_cmd = 0x10;
        }
    }
    xSemaphoreGive(s_lock);
}

static void select_input_mode(mosaico_interact_button_mode_t button_mode)
{
    if (!s_running) return;
    bool changed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_button_mode != button_mode) {
        s_button_mode = button_mode;
        s_state.touch_input_selected = button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH;
        s_reconnect = true;
        changed = true;
    }
    xSemaphoreGive(s_lock);
    if (changed) save_input_mode(button_mode);
}

void interact_backend_select_button_input(void)
{
    select_input_mode(MOSAICO_INTERACT_BUTTON_MODE_GPIO);
}

void interact_backend_select_touch_input(void)
{
    select_input_mode(MOSAICO_INTERACT_BUTTON_MODE_TOUCH);
}

void interact_backend_snapshot(interact_snapshot_t *out)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    if (s_lock) xSemaphoreGive(s_lock);
}

esp_err_t interact_backend_led_set(uint8_t index, interact_rgb_t color)
{
    if (index >= INTERACT_LED_USER_COUNT) return ESP_ERR_INVALID_ARG;
    if (!s_running) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.ready && !s_reconnect) {
        s_manual_color[index] = color;
        if (color.r || color.g || color.b) s_manual_on |= (uint8_t)(1U << index);
        else s_manual_on &= (uint8_t)~(1U << index);
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t interact_backend_led_fill(interact_rgb_t color)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.ready && !s_reconnect) {
        for (unsigned i = 0; i < INTERACT_LED_USER_COUNT; ++i) s_manual_color[i] = color;
        s_manual_on = (color.r || color.g || color.b) ? (uint8_t)((1U << INTERACT_LED_USER_COUNT) - 1U) : 0;
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t interact_backend_led_clear(void)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.ready && !s_reconnect) {
        memset(s_manual_color, 0, sizeof(s_manual_color));
        s_manual_on = 0;
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t interact_backend_ir_send(uint8_t address, uint8_t command)
{
    if (!s_running) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.ready && !s_reconnect) {
        s_ir = true;
        s_ir_addr = address;
        s_ir_cmd = command;
        err = ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t interact_backend_set_auto_camera(bool enabled)
{
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_auto_camera = enabled;
        xSemaphoreGive(s_lock);
    } else {
        s_auto_camera = enabled;
    }
    save_auto_camera(enabled);
    return ESP_OK;
}

bool interact_backend_auto_camera_enabled(void)
{
    if (!s_lock) return s_auto_camera;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool enabled = s_auto_camera;
    xSemaphoreGive(s_lock);
    return enabled;
}

esp_err_t interact_backend_set_key_cb(
    void (*cb)(int key_id, bool is_long, void *ctx), void *ctx)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_key_cb = cb;
    s_key_ctx = ctx;
    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t interact_backend_camera_open(void)
{
    return mosaic_ui_open_app("camera");
}

esp_err_t interact_backend_camera_close(void)
{
    return mosaic_ui_active_app_is("camera") ? mosaic_ui_back() : ESP_OK;
}

#else
// Desktop preview deliberately exposes no fabricated sensor readings.
void interact_backend_start(void) {}
void interact_backend_stop(void) {}
void interact_backend_command(interact_command_t command) { (void)command; }
void interact_backend_select_button_input(void) {}
void interact_backend_select_touch_input(void) {}
void interact_backend_snapshot(interact_snapshot_t *out)
{
    *out = (interact_snapshot_t){.slot = -1, .checked = true, .touch_input_selected = true};
    snprintf(out->status, sizeof(out->status), "Preview - no hardware");
}
esp_err_t interact_backend_led_set(uint8_t index, interact_rgb_t color) { (void)index; (void)color; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t interact_backend_led_fill(interact_rgb_t color) { (void)color; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t interact_backend_led_clear(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t interact_backend_ir_send(uint8_t address, uint8_t command) { (void)address; (void)command; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t interact_backend_set_auto_camera(bool enabled) { (void)enabled; return ESP_ERR_NOT_SUPPORTED; }
bool interact_backend_auto_camera_enabled(void) { return false; }
esp_err_t interact_backend_set_key_cb(
    void (*cb)(int key_id, bool is_long, void *ctx), void *ctx)
{
    (void)cb;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t interact_backend_camera_open(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t interact_backend_camera_close(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
