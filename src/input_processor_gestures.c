/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Ported from pite1222/zmk (release/conductor-1.0.4,
 * app/src/pointing/input_processor_gestures.c) without the per-endpoint
 * override, which needs Conductor's custom Studio RPC. The tap is deferred to
 * the system work queue (see gestures_tap_work_cb), and the four bindings are
 * a property of the processor instead of keys on the gesture layer, so they
 * are only reachable by flicking the ball.
 */

#define DT_DRV_COMPAT zmk_input_processor_gestures

#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

enum gesture_direction {
    GESTURE_UP,
    GESTURE_DOWN,
    GESTURE_LEFT,
    GESTURE_RIGHT,
    GESTURE_DIRECTION_COUNT,
};

struct gestures_config {
    uint8_t index;
    uint8_t type;
    uint16_t x_code;
    uint16_t y_code;
    uint16_t threshold;
    uint16_t timeout_ms;
    uint16_t cooldown_ms;
    uint16_t tap_ms;
    // up, down, left, right
    const struct zmk_behavior_binding *bindings;
};

struct gestures_data {
    const struct device *dev;
    struct k_work tap_work;
    int tap_direction;
    uint8_t tap_input_device_index;
    int32_t x;
    int32_t y;
    int64_t started_at;
    int64_t cooldown_until;
    bool pending;
};

static int32_t abs32(int32_t value) { return value < 0 ? -value : value; }

static void gestures_reset(struct gestures_data *data) {
    data->x = 0;
    data->y = 0;
    data->started_at = 0;
    data->pending = false;
}

static int gestures_direction(const struct gestures_config *cfg, const struct gestures_data *data) {
    int32_t abs_x = abs32(data->x);
    int32_t abs_y = abs32(data->y);

    if (abs_x < cfg->threshold && abs_y < cfg->threshold) {
        return -ENODATA;
    }

    if (abs_y >= abs_x) {
        return data->y < 0 ? GESTURE_UP : GESTURE_DOWN;
    }

    return data->x < 0 ? GESTURE_LEFT : GESTURE_RIGHT;
}

static int gestures_queue_tap(const struct gestures_config *cfg, int direction,
                              uint8_t input_device_index) {
    const struct zmk_behavior_binding *binding = &cfg->bindings[direction];

    struct zmk_behavior_binding_event event = {
        .position =
            ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(input_device_index, cfg->index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    int ret = zmk_behavior_queue_add(&event, *binding, true, cfg->tap_ms);
    if (ret < 0) {
        return ret;
    }

    return zmk_behavior_queue_add(&event, *binding, false, 0);
}

// zmk_behavior_queue_add() runs the press right away in the caller's thread. The input
// thread only has CONFIG_INPUT_THREAD_STACK_SIZE (512 bytes on the central), which the
// keycode event chain overflows, so the tap runs on the system work queue like key presses.
static void gestures_tap_work_cb(struct k_work *work) {
    struct gestures_data *data = CONTAINER_OF(work, struct gestures_data, tap_work);
    const struct gestures_config *cfg = data->dev->config;

    int ret = gestures_queue_tap(cfg, data->tap_direction, data->tap_input_device_index);
    if (ret < 0) {
        LOG_ERR("Failed to queue gesture behavior: %d", ret);
    }
}

static int gestures_handle_event(const struct device *dev, struct input_event *event,
                                 uint32_t param1, uint32_t param2,
                                 struct zmk_input_processor_state *state) {
    const struct gestures_config *cfg = dev->config;
    struct gestures_data *data = dev->data;

    if (event->type != cfg->type || (event->code != cfg->x_code && event->code != cfg->y_code)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int32_t delta = event->value;
    int64_t now = k_uptime_get();
    // The pointer stays still while the gesture layer is active.
    event->value = 0;

    if (now < data->cooldown_until) {
        return ZMK_INPUT_PROC_STOP;
    }

    // A slow roll never reaches the threshold within the window, so it never fires.
    if (data->pending && now - data->started_at > cfg->timeout_ms) {
        gestures_reset(data);
    }

    if (!data->pending) {
        if (delta == 0) {
            return ZMK_INPUT_PROC_STOP;
        }

        data->started_at = now;
        data->pending = true;
    }

    if (event->code == cfg->x_code) {
        data->x += delta;
    } else {
        data->y += delta;
    }

    if (!event->sync) {
        return ZMK_INPUT_PROC_STOP;
    }

    int direction = gestures_direction(cfg, data);
    if (direction >= 0) {
        // The cooldown keeps the next gesture from overwriting these before the work runs.
        data->tap_direction = direction;
        data->tap_input_device_index = state->input_device_index;
        k_work_submit(&data->tap_work);
        gestures_reset(data);
        data->cooldown_until = now + cfg->cooldown_ms;
    }

    return ZMK_INPUT_PROC_STOP;
}

static const struct zmk_input_processor_driver_api gestures_driver_api = {
    .handle_event = gestures_handle_event,
};

static int gestures_init(const struct device *dev) {
    struct gestures_data *data = dev->data;

    data->dev = dev;
    k_work_init(&data->tap_work, gestures_tap_work_cb);
    return 0;
}

#define GESTURES_INST(n)                                                                           \
    static const struct zmk_behavior_binding gestures_bindings_##n[] = {                           \
        LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}; \
    BUILD_ASSERT(ARRAY_SIZE(gestures_bindings_##n) == GESTURE_DIRECTION_COUNT,                     \
                 "Gesture processors need exactly four bindings: up, down, left, right");          \
    static struct gestures_data gestures_data_##n = {};                                            \
    static const struct gestures_config gestures_config_##n = {                                    \
        .index = n,                                                                                \
        .type = DT_INST_PROP_OR(n, type, INPUT_EV_REL),                                            \
        .x_code = DT_INST_PROP_OR(n, x_code, INPUT_REL_X),                                         \
        .y_code = DT_INST_PROP_OR(n, y_code, INPUT_REL_Y),                                         \
        .threshold = DT_INST_PROP_OR(n, threshold, 120),                                           \
        .timeout_ms = DT_INST_PROP_OR(n, timeout_ms, 180),                                         \
        .cooldown_ms = DT_INST_PROP_OR(n, cooldown_ms, 220),                                       \
        .tap_ms = DT_INST_PROP_OR(n, tap_ms, 30),                                                  \
        .bindings = gestures_bindings_##n,                                                         \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, gestures_init, NULL, &gestures_data_##n, &gestures_config_##n,        \
                          POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                        \
                          &gestures_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GESTURES_INST)
