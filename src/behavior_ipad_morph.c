/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Like mod-morph, but picks the binding by host instead of by modifiers:
 * the iPad's BLE profile gets the second binding, everything else the first.
 * The choice is made on press, and the release goes to the same binding.
 */

#define DT_DRV_COMPAT zmk_behavior_ipad_morph

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <zmk/behavior.h>
#include <zmk/endpoints.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_ipad_morph_config {
    struct zmk_behavior_binding mac_binding;
    struct zmk_behavior_binding ipad_binding;
    int ipad_ble_profile;
};

struct behavior_ipad_morph_data {
    const struct zmk_behavior_binding *pressed_binding;
};

static bool ipad_morph_host_is_ipad(const struct behavior_ipad_morph_config *cfg) {
#if IS_ENABLED(CONFIG_ZMK_BLE)
    struct zmk_endpoint_instance endpoint = zmk_endpoints_selected();

    return endpoint.transport == ZMK_TRANSPORT_BLE &&
           endpoint.ble.profile_index == cfg->ipad_ble_profile;
#else
    return false;
#endif
}

static int on_ipad_morph_binding_pressed(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_ipad_morph_config *cfg = dev->config;
    struct behavior_ipad_morph_data *data = dev->data;

    if (data->pressed_binding != NULL) {
        LOG_ERR("Can't press the same ipad-morph twice");
        return -ENOTSUP;
    }

    data->pressed_binding =
        ipad_morph_host_is_ipad(cfg) ? &cfg->ipad_binding : &cfg->mac_binding;
    return zmk_behavior_invoke_binding(data->pressed_binding, event, true);
}

static int on_ipad_morph_binding_released(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_ipad_morph_data *data = dev->data;

    if (data->pressed_binding == NULL) {
        LOG_ERR("ipad-morph already released");
        return -ENOTSUP;
    }

    const struct zmk_behavior_binding *pressed_binding = data->pressed_binding;
    data->pressed_binding = NULL;
    return zmk_behavior_invoke_binding(pressed_binding, event, false);
}

static const struct behavior_driver_api behavior_ipad_morph_driver_api = {
    .binding_pressed = on_ipad_morph_binding_pressed,
    .binding_released = on_ipad_morph_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define IPAD_MORPH_BINDING(idx, n)                                                                 \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(n, bindings, idx)),                  \
        .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(n, bindings, idx, param1), (0),          \
                              (DT_INST_PHA_BY_IDX(n, bindings, idx, param1))),                     \
        .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(n, bindings, idx, param2), (0),          \
                              (DT_INST_PHA_BY_IDX(n, bindings, idx, param2))),                     \
    }

#define IPAD_MORPH_INST(n)                                                                         \
    BUILD_ASSERT(DT_INST_PROP_LEN(n, bindings) == 2,                                               \
                 "ipad-morph needs exactly two bindings: Mac, then iPad");                         \
    static const struct behavior_ipad_morph_config behavior_ipad_morph_config_##n = {              \
        .mac_binding = IPAD_MORPH_BINDING(0, n),                                                   \
        .ipad_binding = IPAD_MORPH_BINDING(1, n),                                                  \
        .ipad_ble_profile = DT_INST_PROP(n, ipad_ble_profile),                                     \
    };                                                                                             \
    static struct behavior_ipad_morph_data behavior_ipad_morph_data_##n = {};                      \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, &behavior_ipad_morph_data_##n,                          \
                            &behavior_ipad_morph_config_##n, POST_KERNEL,                          \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_ipad_morph_driver_api);

DT_INST_FOREACH_STATUS_OKAY(IPAD_MORPH_INST)

#endif
