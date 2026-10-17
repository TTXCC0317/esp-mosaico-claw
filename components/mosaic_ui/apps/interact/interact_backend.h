#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Always-on interaction-subboard service.
 *
 * The service owns exactly one worker task, one module-manager claim and one
 * board handle for the lifetime of the firmware, so it is independent of the
 * Interaction App being open. Consumers only read snapshots and issue commands.
 */

#define INTERACT_LED_COUNT 6
/* Only LED0-LED3 are user-controllable on the interaction subboard: they live on
 * the top edge and are the LEDs exposed to the user and to the voice/agent tools.
 * LED4 and LED5 are the physical backlights of KEY_R and KEY_L and are driven by
 * the service from the key state, so they are not manually controllable. */
#define INTERACT_LED_USER_COUNT 4
#define INTERACT_LED_KEY_R 4
#define INTERACT_LED_KEY_L 5

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} interact_rgb_t;

typedef struct {
    bool ready, checked, key_l, key_r, pir, touch_input_selected;
    uint8_t leds, pressed_leds, light;
    int slot;
    char status[48], ir_status[24];
} interact_snapshot_t;

typedef enum {
    INTERACT_COMMAND_LED_0,
    INTERACT_COMMAND_LED_1,
    INTERACT_COMMAND_LED_2,
    INTERACT_COMMAND_LED_3,
    INTERACT_COMMAND_LED_4,
    INTERACT_COMMAND_LED_5,
    INTERACT_COMMAND_IR_SEND,
} interact_command_t;

/* Idempotent start of the always-on worker. */
void interact_backend_start(void);
void interact_backend_stop(void);
void interact_backend_snapshot(interact_snapshot_t *out);
void interact_backend_command(interact_command_t command);
void interact_backend_select_button_input(void);
void interact_backend_select_touch_input(void);

/* Extended thread-safe control; ESP_ERR_INVALID_STATE when no board is ready. */
esp_err_t interact_backend_led_set(uint8_t index, interact_rgb_t color);
esp_err_t interact_backend_led_fill(interact_rgb_t color);
esp_err_t interact_backend_led_clear(void);
esp_err_t interact_backend_ir_send(uint8_t address, uint8_t command);

/* PIR-triggered camera switch, persisted in NVS. Enabled by default. */
esp_err_t interact_backend_set_auto_camera(bool enabled);
bool interact_backend_auto_camera_enabled(void);

/* Explicit camera screen request from the user or the voice/agent path. The
 * camera is never opened implicitly by anything else. Closing is a no-op when
 * the camera screen is not the active one. */
esp_err_t interact_backend_camera_open(void);
esp_err_t interact_backend_camera_close(void);

/* Debounced KEY_L / KEY_R short and long presses.
 *
 * key_id is 0 for KEY_L and 1 for KEY_R; is_long marks one hold that reached
 * the long-press threshold while still pressed. A short press is reported once
 * on release. The callback runs on the service worker task, so it must be
 * non-blocking and must not call back into this module synchronously. Pass NULL
 * to detach. */
esp_err_t interact_backend_set_key_cb(
    void (*cb)(int key_id, bool is_long, void *ctx), void *ctx);

#ifdef __cplusplus
}
#endif
