/*
 * Copyright (c) 2024 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_threshold_temp_layer

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/input_processor.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define MAX_LAYERS ZMK_KEYMAP_LAYERS_LEN
#define THRESHOLD_TEMP_LAYER_ACTION_QUEUE_SIZE 4

struct threshold_temp_layer_config {
    uint32_t activation_threshold;
    uint32_t accumulation_window_ms;
    int16_t require_prior_idle_ms;
    const uint16_t *excluded_positions;
    size_t num_positions;
};

struct threshold_temp_layer_state {
    uint8_t toggle_layer;
    bool is_active;
    bool activation_pending;
    bool is_accumulating;
    int64_t last_tapped_timestamp;
    int64_t accumulation_started_at;
    uint32_t accumulated_movement;
};

struct threshold_temp_layer_data {
    struct k_mutex lock;
    struct threshold_temp_layer_state state;
};

struct layer_state_action {
    uint8_t layer;
    bool activate;
};

static struct k_work_delayable layer_disable_works[MAX_LAYERS];

K_MSGQ_DEFINE(threshold_temp_layer_action_msgq, sizeof(struct layer_state_action),
              THRESHOLD_TEMP_LAYER_ACTION_QUEUE_SIZE, 4);

static void reset_accumulation(struct threshold_temp_layer_state *state) {
    state->is_accumulating = false;
    state->accumulation_started_at = 0;
    state->accumulated_movement = 0;
}

static bool position_is_excluded(const struct threshold_temp_layer_config *config,
                                 uint32_t position) {
    const uint16_t *end = config->excluded_positions + config->num_positions;

    for (const uint16_t *pos = config->excluded_positions; pos < end; pos++) {
        if (*pos == position) {
            return true;
        }
    }

    return false;
}

static bool prior_idle_required(const struct threshold_temp_layer_config *config,
                                int64_t last_tapped, int64_t current_time) {
    return (last_tapped + config->require_prior_idle_ms) > current_time;
}

static bool is_xy_movement(const struct input_event *event) {
    return event->type == INPUT_EV_REL &&
           (event->code == INPUT_REL_X || event->code == INPUT_REL_Y) && event->value != 0;
}

static uint32_t movement_magnitude(const struct input_event *event) {
    int64_t value = event->value;

    return (uint32_t)(value < 0 ? -value : value);
}

static bool accumulate_movement(const struct threshold_temp_layer_config *config,
                                struct threshold_temp_layer_state *state,
                                const struct input_event *event, int64_t current_time) {
    if (!state->is_accumulating ||
        current_time - state->accumulation_started_at >= config->accumulation_window_ms) {
        reset_accumulation(state);
        state->is_accumulating = true;
        state->accumulation_started_at = current_time;
    }

    uint32_t movement = movement_magnitude(event);
    uint32_t remaining = config->activation_threshold - state->accumulated_movement;

    if (movement >= remaining) {
        state->accumulated_movement = config->activation_threshold;
        return true;
    }

    state->accumulated_movement += movement;
    return false;
}

static void update_layer_state(struct threshold_temp_layer_state *state, bool activate) {
    state->activation_pending = false;

    if (state->is_active == activate) {
        return;
    }

    state->is_active = activate;
    reset_accumulation(state);

    if (activate) {
        zmk_keymap_layer_activate(state->toggle_layer);
        LOG_DBG("Layer %d activated after movement threshold", state->toggle_layer);
    } else {
        zmk_keymap_layer_deactivate(state->toggle_layer);
        LOG_DBG("Layer %d deactivated", state->toggle_layer);
    }
}

static void layer_action_work_cb(struct k_work *work) {
    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct threshold_temp_layer_data *data = dev->data;
    const struct threshold_temp_layer_config *config = dev->config;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        LOG_ERR("Failed to lock threshold temp layer state: %d", ret);
        return;
    }

    struct layer_state_action action;

    while (k_msgq_get(&threshold_temp_layer_action_msgq, &action, K_NO_WAIT) >= 0) {
        if (action.activate) {
            data->state.activation_pending = false;

            if (prior_idle_required(config, data->state.last_tapped_timestamp, k_uptime_get())) {
                reset_accumulation(&data->state);
                k_work_cancel_delayable(&layer_disable_works[action.layer]);
                continue;
            }

            update_layer_state(&data->state, true);
        } else if (data->state.is_active && zmk_keymap_layer_active(action.layer)) {
            update_layer_state(&data->state, false);
        }
    }

    k_mutex_unlock(&data->lock);
}

static K_WORK_DEFINE(layer_action_work, layer_action_work_cb);

static void layer_disable_callback(struct k_work *work) {
    struct k_work_delayable *delayable = k_work_delayable_from_work(work);
    int layer = ARRAY_INDEX(layer_disable_works, delayable);
    struct layer_state_action action = {.layer = layer, .activate = false};

    int ret = k_msgq_put(&threshold_temp_layer_action_msgq, &action, K_NO_WAIT);
    if (ret < 0) {
        LOG_ERR("Failed to queue layer %d deactivation: %d", layer, ret);
        return;
    }

    k_work_submit(&layer_action_work);
}

static int handle_layer_state_changed(const struct device *dev, const zmk_event_t *eh) {
    ARG_UNUSED(eh);

    struct threshold_temp_layer_data *data = dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    if (data->state.is_active &&
        !zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(data->state.toggle_layer))) {
        data->state.is_active = false;
        reset_accumulation(&data->state);
        k_work_cancel_delayable(&layer_disable_works[data->state.toggle_layer]);
    }

    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_position_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_position_state_changed *event = as_zmk_position_state_changed(eh);

    if (!event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct threshold_temp_layer_data *data = dev->data;
    const struct threshold_temp_layer_config *config = dev->config;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    if (data->state.is_active && config->num_positions > 0 &&
        !position_is_excluded(config, event->position)) {
        update_layer_state(&data->state, false);
    }

    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_keycode_state_changed(const struct device *dev, const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *event = as_zmk_keycode_state_changed(eh);

    if (!event->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct threshold_temp_layer_data *data = dev->data;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    data->state.last_tapped_timestamp = event->timestamp;
    reset_accumulation(&data->state);

    ret = k_mutex_unlock(&data->lock);
    if (ret < 0) {
        return ret;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int handle_state_changed_dispatcher(const struct device *dev, const zmk_event_t *eh) {
    if (as_zmk_layer_state_changed(eh) != NULL) {
        return handle_layer_state_changed(dev, eh);
    }

    if (as_zmk_position_state_changed(eh) != NULL) {
        return handle_position_state_changed(dev, eh);
    }

    if (as_zmk_keycode_state_changed(eh) != NULL) {
        return handle_keycode_state_changed(dev, eh);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

#define DISPATCH_EVENT(inst)                                                                       \
    {                                                                                              \
        int err = handle_state_changed_dispatcher(DEVICE_DT_INST_GET(inst), eh);                   \
        if (err < 0) {                                                                             \
            return err;                                                                            \
        }                                                                                          \
    }

static int handle_event_dispatcher(const zmk_event_t *eh) {
    DT_INST_FOREACH_STATUS_OKAY(DISPATCH_EVENT)

    return 0;
}

static int threshold_temp_layer_handle_event(const struct device *dev, struct input_event *event,
                                             uint32_t layer, uint32_t timeout_ms,
                                             struct zmk_input_processor_state *processor_state) {
    ARG_UNUSED(processor_state);

    if (layer >= MAX_LAYERS) {
        LOG_ERR("Invalid layer index: %d", layer);
        return -EINVAL;
    }

    if (!is_xy_movement(event)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct threshold_temp_layer_data *data = dev->data;
    const struct threshold_temp_layer_config *config = dev->config;

    int ret = k_mutex_lock(&data->lock, K_FOREVER);
    if (ret < 0) {
        return ret;
    }

    int64_t current_time = k_uptime_get();
    data->state.toggle_layer = layer;

    if (data->state.is_active || data->state.activation_pending) {
        if (timeout_ms > 0) {
            k_work_reschedule(&layer_disable_works[layer], K_MSEC(timeout_ms));
        }
        k_mutex_unlock(&data->lock);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (prior_idle_required(config, data->state.last_tapped_timestamp, current_time)) {
        reset_accumulation(&data->state);
        k_mutex_unlock(&data->lock);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (!accumulate_movement(config, &data->state, event, current_time)) {
        k_mutex_unlock(&data->lock);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    reset_accumulation(&data->state);
    data->state.activation_pending = true;

    struct layer_state_action action = {.layer = layer, .activate = true};
    ret = k_msgq_put(&threshold_temp_layer_action_msgq, &action, K_NO_WAIT);

    if (ret < 0) {
        data->state.activation_pending = false;
        LOG_ERR("Failed to queue layer %d activation: %d", layer, ret);
    } else {
        if (timeout_ms > 0) {
            k_work_reschedule(&layer_disable_works[layer], K_MSEC(timeout_ms));
        }
        k_work_submit(&layer_action_work);
    }

    k_mutex_unlock(&data->lock);
    return ZMK_INPUT_PROC_CONTINUE;
}

static int threshold_temp_layer_init(const struct device *dev) {
    struct threshold_temp_layer_data *data = dev->data;

    k_mutex_init(&data->lock);

    for (int i = 0; i < MAX_LAYERS; i++) {
        k_work_init_delayable(&layer_disable_works[i], layer_disable_callback);
    }

    return 0;
}

static const struct zmk_input_processor_driver_api threshold_temp_layer_driver_api = {
    .handle_event = threshold_temp_layer_handle_event,
};

#define NEEDS_POSITION_HANDLERS(n, ...) DT_INST_PROP_HAS_IDX(n, excluded_positions, 0)
#define NEEDS_KEYCODE_HANDLERS(n, ...) (DT_INST_PROP_OR(n, require_prior_idle_ms, 0) > 0)

ZMK_LISTENER(processor_threshold_temp_layer, handle_event_dispatcher);
ZMK_SUBSCRIPTION(processor_threshold_temp_layer, zmk_layer_state_changed);

#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_POSITION_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_threshold_temp_layer, zmk_position_state_changed);
#endif

#if DT_INST_FOREACH_STATUS_OKAY_VARGS(NEEDS_KEYCODE_HANDLERS, ||)
ZMK_SUBSCRIPTION(processor_threshold_temp_layer, zmk_keycode_state_changed);
#endif

#define THRESHOLD_TEMP_LAYER_INST(n)                                                               \
    BUILD_ASSERT(DT_INST_PROP(n, activation_threshold) > 0,                                        \
                 "activation-threshold must be greater than zero");                                \
    BUILD_ASSERT(DT_INST_PROP(n, accumulation_window_ms) > 0,                                      \
                 "accumulation-window-ms must be greater than zero");                              \
    static struct threshold_temp_layer_data threshold_temp_layer_data_##n = {};                    \
    static const uint16_t excluded_positions_##n[] = DT_INST_PROP(n, excluded_positions);          \
    static const struct threshold_temp_layer_config threshold_temp_layer_config_##n = {            \
        .activation_threshold = DT_INST_PROP(n, activation_threshold),                             \
        .accumulation_window_ms = DT_INST_PROP(n, accumulation_window_ms),                         \
        .require_prior_idle_ms = DT_INST_PROP_OR(n, require_prior_idle_ms, 0),                     \
        .excluded_positions = excluded_positions_##n,                                              \
        .num_positions = DT_INST_PROP_LEN(n, excluded_positions),                                  \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, threshold_temp_layer_init, NULL, &threshold_temp_layer_data_##n,      \
                          &threshold_temp_layer_config_##n, POST_KERNEL,                           \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &threshold_temp_layer_driver_api);

DT_INST_FOREACH_STATUS_OKAY(THRESHOLD_TEMP_LAYER_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
