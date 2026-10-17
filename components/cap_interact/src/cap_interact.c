/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_interact.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "interact_backend.h"

#define CAP_INTERACT_TAG "cap_interact"

static const char *const CAP_INTERACT_BOARD_UNAVAILABLE =
    "interaction subboard is not connected";

static int cap_interact_clamp_byte(int value)
{
    if (value < 0) {
        return 0;
    }
    return value > 255 ? 255 : value;
}

/* Parse an object payload; NULL when the input is not a JSON object. */
static cJSON *cap_interact_parse(const char *input_json)
{
    cJSON *input = cJSON_Parse(input_json != NULL && input_json[0] != '\0' ? input_json : "{}");
    if (input != NULL && cJSON_IsObject(input)) {
        return input;
    }
    cJSON_Delete(input);
    return NULL;
}

static int cap_interact_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static bool cap_interact_bool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static esp_err_t cap_interact_reply(cJSON *root, char *output, size_t output_size)
{
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (output == NULL || output_size == 0 ||
        !cJSON_PrintPreallocated(root, output, (int)output_size, false)) {
        cJSON_Delete(root);
        if (output != NULL && output_size > 0) {
            snprintf(output, output_size, "{\"ok\":false,\"error\":\"output too small\"}");
        }
        return ESP_ERR_NO_MEM;
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_interact_error(char *output, size_t output_size, esp_err_t err, const char *message)
{
    (void)snprintf(output, output_size,
                   "{\"ok\":false,\"error\":\"%s\",\"reason\":\"%s\"}",
                   message, esp_err_to_name(err));
    return err;
}

static esp_err_t cap_interact_invalid_input(char *output, size_t output_size, const char *message)
{
    return cap_interact_error(output, output_size, ESP_ERR_INVALID_ARG, message);
}

static bool cap_interact_led_ready(char *output, size_t output_size, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        cap_interact_error(output, output_size, err, CAP_INTERACT_BOARD_UNAVAILABLE);
        return false;
    }
    if (err != ESP_OK) {
        cap_interact_error(output, output_size, err, "interaction LED control failed");
        return false;
    }
    return true;
}

static esp_err_t cap_interact_status(const char *input_json,
                                     const claw_cap_call_context_t *ctx,
                                     char *output,
                                     size_t output_size)
{
    (void)input_json;
    (void)ctx;

    interact_snapshot_t state;
    interact_backend_snapshot(&state);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "board_present", state.ready);
    cJSON_AddStringToObject(root, "status", state.status);
    cJSON_AddNumberToObject(root, "slot", state.slot);
    cJSON_AddBoolToObject(root, "motion_detected", state.pir);
    cJSON_AddBoolToObject(root, "key_left_pressed", state.key_l);
    cJSON_AddBoolToObject(root, "key_right_pressed", state.key_r);
    cJSON_AddNumberToObject(root, "light_level", state.light);
    cJSON_AddBoolToObject(root, "auto_camera_enabled", interact_backend_auto_camera_enabled());

    cJSON_AddNumberToObject(root, "led_count", INTERACT_LED_USER_COUNT);
    cJSON *lit = cJSON_AddArrayToObject(root, "leds_lit");
    if (lit != NULL) {
        for (unsigned i = 0; i < INTERACT_LED_USER_COUNT; ++i) {
            if (state.leds & (1U << i)) {
                cJSON_AddItemToArray(lit, cJSON_CreateNumber((double)i));
            }
        }
    }
    return cap_interact_reply(root, output, output_size);
}

static esp_err_t cap_interact_led(const char *input_json,
                                   const claw_cap_call_context_t *ctx,
                                   char *output,
                                   size_t output_size)
{
    (void)ctx;

    cJSON *input = cap_interact_parse(input_json);
    if (input == NULL) {
        return cap_interact_invalid_input(output, output_size, "invalid input json");
    }
    const int index = cap_interact_int(input, "index", -1);
    const bool on = cap_interact_bool(input, "on", true);
    const int r = cap_interact_clamp_byte(cap_interact_int(input, "r", 255));
    const int g = cap_interact_clamp_byte(cap_interact_int(input, "g", 255));
    const int b = cap_interact_clamp_byte(cap_interact_int(input, "b", 255));
    cJSON_Delete(input);

    if (index < 0 || index >= (int)INTERACT_LED_USER_COUNT) {
        return cap_interact_invalid_input(output, output_size, "index must be 0-3");
    }

    const interact_rgb_t color = on ? (interact_rgb_t){(uint8_t)r, (uint8_t)g, (uint8_t)b}
                                    : (interact_rgb_t){0, 0, 0};
    const esp_err_t err = interact_backend_led_set((uint8_t)index, color);
    if (!cap_interact_led_ready(output, output_size, err)) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "index", index);
    cJSON_AddBoolToObject(root, "on", on);
    cJSON_AddNumberToObject(root, "r", color.r);
    cJSON_AddNumberToObject(root, "g", color.g);
    cJSON_AddNumberToObject(root, "b", color.b);
    return cap_interact_reply(root, output, output_size);
}

static esp_err_t cap_interact_led_fill(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)ctx;

    cJSON *input = cap_interact_parse(input_json);
    if (input == NULL) {
        return cap_interact_invalid_input(output, output_size, "invalid input json");
    }
    const bool on = cap_interact_bool(input, "on", true);
    const int r = cap_interact_clamp_byte(cap_interact_int(input, "r", 255));
    const int g = cap_interact_clamp_byte(cap_interact_int(input, "g", 255));
    const int b = cap_interact_clamp_byte(cap_interact_int(input, "b", 255));
    cJSON_Delete(input);

    const interact_rgb_t color = on ? (interact_rgb_t){(uint8_t)r, (uint8_t)g, (uint8_t)b}
                                    : (interact_rgb_t){0, 0, 0};
    const esp_err_t err = interact_backend_led_fill(color);
    if (!cap_interact_led_ready(output, output_size, err)) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "on", on);
    cJSON_AddNumberToObject(root, "r", color.r);
    cJSON_AddNumberToObject(root, "g", color.g);
    cJSON_AddNumberToObject(root, "b", color.b);
    return cap_interact_reply(root, output, output_size);
}

static esp_err_t cap_interact_led_clear(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    (void)input_json;
    (void)ctx;

    const esp_err_t err = interact_backend_led_clear();
    if (!cap_interact_led_ready(output, output_size, err)) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "state", "all four LEDs off");
    return cap_interact_reply(root, output, output_size);
}

static esp_err_t cap_interact_ir_send(const char *input_json,
                                       const claw_cap_call_context_t *ctx,
                                       char *output,
                                       size_t output_size)
{
    (void)ctx;

    cJSON *input = cap_interact_parse(input_json);
    if (input == NULL) {
        return cap_interact_invalid_input(output, output_size, "invalid input json");
    }
    const int address = cap_interact_clamp_byte(cap_interact_int(input, "address", 0x00));
    const int command = cap_interact_clamp_byte(cap_interact_int(input, "command", 0x10));
    cJSON_Delete(input);

    const esp_err_t err = interact_backend_ir_send((uint8_t)address, (uint8_t)command);
    if (!cap_interact_led_ready(output, output_size, err)) {
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "address", address);
    cJSON_AddNumberToObject(root, "command", command);
    cJSON_AddStringToObject(root, "protocol", "NEC");
    return cap_interact_reply(root, output, output_size);
}

static esp_err_t cap_interact_auto_camera(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    (void)ctx;

    cJSON *input = cap_interact_parse(input_json);
    if (input == NULL) {
        return cap_interact_invalid_input(output, output_size, "invalid input json");
    }
    const bool enabled = cap_interact_bool(input, "enabled", true);
    cJSON_Delete(input);

    const esp_err_t err = interact_backend_set_auto_camera(enabled);
    if (err != ESP_OK) {
        return cap_interact_error(output, output_size, err, "failed to update auto-camera setting");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "auto_camera_enabled", enabled);
    return cap_interact_reply(root, output, output_size);
}

/* The camera screen is only ever opened on an explicit user request or by the
 * subboard PIR sensor; this tool is the voice/agent path for that request. */
static esp_err_t cap_interact_camera(const char *input_json,
                                     const claw_cap_call_context_t *ctx,
                                     char *output,
                                     size_t output_size)
{
    (void)ctx;

    cJSON *input = cap_interact_parse(input_json);
    if (input == NULL) {
        return cap_interact_invalid_input(output, output_size, "invalid input json");
    }
    const bool open = cap_interact_bool(input, "open", true);
    cJSON_Delete(input);

    esp_err_t err = ESP_OK;
    if (open) {
        err = interact_backend_camera_open();
    } else {
        err = interact_backend_camera_close();
    }
    if (err != ESP_OK) {
        return cap_interact_error(output, output_size, err,
                                  open ? "failed to open the camera" : "failed to close the camera");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "open", open);
    return cap_interact_reply(root, output, output_size);
}

static const claw_cap_descriptor_t s_interact_descriptors[] = {
    {
        .id = "interact_status",
        .name = "interact_status",
        .family = "mosaico",
        .description = "Read the state of the ESP-Mosaico multi-function interaction subboard: whether it is connected, its "
                       "slot, ambient light level, PIR motion, key presses, and which of the four RGB LEDs (LED0-LED3) are lit.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .execute = cap_interact_status,
    },
    {
        .id = "interact_led",
        .name = "interact_led",
        .family = "mosaico",
        .description = "Turn one specific RGB LED on the interaction subboard on or off, optionally with a custom color. The "
                       "board has four LEDs, named LED0 to LED3, so index must be 0, 1, 2 or 3: a request such as \"turn on "
                       "LED1\" or \"打开LED1灯\" means index=1. Set on=false to switch that LED off. This is the tool for the "
                       "interaction subboard lights: do not use generic GPIO or Lua light scripts for them.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"index\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3},"
                             "\"on\":{\"type\":\"boolean\"},"
                             "\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
                             "\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
                             "\"b\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
                             "\"required\":[\"index\"],\"additionalProperties\":false}",
        .execute = cap_interact_led,
    },
    {
        .id = "interact_led_fill",
        .name = "interact_led_fill",
        .family = "mosaico",
        .description = "Turn all four RGB LEDs (LED0-LED3) of the interaction subboard on with the same color, or all off with "
                       "on=false. A request such as \"turn on the LEDs\" or \"打开LED灯\" that does not name one LED means this "
                       "tool. This is the tool for the interaction subboard lights: do not use generic GPIO or Lua light scripts "
                       "for them.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"on\":{\"type\":\"boolean\"},"
                             "\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
                             "\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
                             "\"b\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
                             "\"additionalProperties\":false}",
        .execute = cap_interact_led_fill,
    },
    {
        .id = "interact_led_clear",
        .name = "interact_led_clear",
        .family = "mosaico",
        .description = "Turn off all four RGB LEDs (LED0-LED3) of the interaction subboard.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
        .execute = cap_interact_led_clear,
    },
    {
        .id = "interact_ir_send",
        .name = "interact_ir_send",
        .family = "mosaico",
        .description = "Send one NEC infrared frame from the interaction subboard's IR emitter at 38 kHz. Address and command "
                       "are 8-bit values (defaults 0x00 and 0x10).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"address\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},"
                             "\"command\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255}},"
                             "\"additionalProperties\":false}",
        .execute = cap_interact_ir_send,
    },
    {
        .id = "interact_auto_camera",
        .name = "interact_auto_camera",
        .family = "mosaico",
        .description = "Enable or disable automatically opening the camera when the interaction subboard's AS312 PIR sensor "
                       "detects motion. Persists across reboots.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"enabled\":{\"type\":\"boolean\"}},"
                             "\"required\":[\"enabled\"],\"additionalProperties\":false}",
        .execute = cap_interact_auto_camera,
    },
    {
        .id = "interact_camera",
        .name = "interact_camera",
        .family = "mosaico",
        .description = "Open or close the display's camera screen when the user explicitly asks to see the camera. The camera "
                       "otherwise stays closed and only opens on an explicit request or a subboard PIR motion trigger.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{"
                             "\"open\":{\"type\":\"boolean\"}},"
                             "\"required\":[\"open\"],\"additionalProperties\":false}",
        .execute = cap_interact_camera,
    },
};

static const claw_cap_group_t s_interact_group = {
    .group_id = "cap_interact",
    .descriptors = s_interact_descriptors,
    .descriptor_count = sizeof(s_interact_descriptors) / sizeof(s_interact_descriptors[0]),
};

esp_err_t cap_interact_register_group(void)
{
    if (claw_cap_group_exists(s_interact_group.group_id)) {
        return ESP_OK;
    }

    const esp_err_t err = claw_cap_register_group(&s_interact_group);
    if (err != ESP_OK) {
        ESP_LOGE(CAP_INTERACT_TAG, "register group failed: %s", esp_err_to_name(err));
    }
    return err;
}
