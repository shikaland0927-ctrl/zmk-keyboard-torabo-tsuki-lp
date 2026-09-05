#include "stubs.h"
#include PROCESSOR_SOURCE

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static struct threshold_temp_layer_data test_data;
static const uint16_t excluded[] = {33, 34, 35};
static const struct threshold_temp_layer_config test_config = {
    .activation_threshold = 8, .accumulation_window_ms = 100,
    .require_prior_idle_ms = 300, .excluded_positions = excluded, .num_positions = 3,
};

static int set_layer(uint8_t layer, bool active) {
    if (test_layers[layer] != active) {
        test_layers[layer] = active;
        zmk_event_t event = {.kind = LAYER, .layer = {.layer = layer, .state = active}};
        CHECK(handle_state_changed_dispatcher(&test_device, &event) == 0);
    }
    return 0;
}
static int zmk_keymap_layer_activate(uint8_t layer) { return set_layer(layer, true); }
static int zmk_keymap_layer_deactivate(uint8_t layer) { return set_layer(layer, false); }

static void flush_actions(void) {
    while (layer_action_work.submitted) {
        layer_action_work.submitted = false;
        layer_action_work.handler(&layer_action_work);
    }
}

static void advance_to(int64_t target) {
    CHECK(target >= test_now);
    for (;;) {
        struct k_work_delayable *next = NULL;
        for (size_t i = 0; i < MAX_LAYERS; i++) {
            struct k_work_delayable *work = &layer_disable_works[i];
            if (work->scheduled && work->deadline <= target &&
                (!next || work->deadline < next->deadline)) { next = work; }
        }
        if (!next) { break; }
        test_now = next->deadline;
        next->scheduled = false;
        next->work.handler(&next->work);
        flush_actions();
    }
    test_now = target;
}

static void input(uint8_t type, uint16_t code, int32_t value, uint32_t timeout) {
    struct input_event event = {.type = type, .code = code, .value = value};
    struct zmk_input_processor_state processor = {.input_device_index = type == INPUT_EV_KEY};
    CHECK(threshold_temp_layer_handle_event(&test_device, &event, 4, timeout, &processor) == 0);
    CHECK(event.type == type && event.code == code && event.value == value);
}
static void move(int32_t value) { input(INPUT_EV_REL, INPUT_REL_X, value, 10000); }
static void button(uint8_t index, bool pressed) {
    input(INPUT_EV_KEY, INPUT_BTN_0 + index, pressed, 10000);
}
static void position(uint32_t index) {
    zmk_event_t event = {.kind = POSITION, .position = {.state = true, .position = index}};
    CHECK(handle_state_changed_dispatcher(&test_device, &event) == 0);
}
static void reset(void) {
    memset(&test_data, 0, sizeof(test_data));
    memset(test_layers, 0, sizeof(test_layers));
    test_device = (struct device){.data = &test_data, .config = &test_config};
    threshold_temp_layer_action_msgq.head = threshold_temp_layer_action_msgq.count = 0;
    layer_action_work.submitted = false;
    test_now = 1000;
    CHECK(threshold_temp_layer_init(&test_device) == 0);
}
static void activate(void) { move(8); flush_actions(); CHECK(test_layers[4]); }

static void click_extends_timeout(void) {
    activate();
    advance_to(6000); button(0, true); button(0, false); flush_actions();
    advance_to(11000); CHECK(test_layers[4]);
    advance_to(15999); CHECK(test_layers[4]);
    advance_to(16000); CHECK(!test_layers[4]);
}

static void movement_after_click_extends_timeout(void) {
    activate();
    advance_to(2000); button(0, true); button(0, false); flush_actions();
    advance_to(10000); move(1); flush_actions();
    advance_to(12000); CHECK(test_layers[4]);
    advance_to(19999); CHECK(test_layers[4]);
    advance_to(20000); CHECK(!test_layers[4]);
}

static void all_mouse_buttons_suspend_timeout(void) {
    for (uint8_t index = 0; index < 5; index++) {
        reset(); activate();
        advance_to(2000); button(index, true); flush_actions();
        advance_to(30000); CHECK(test_layers[4]);
        move(1); flush_actions();
        advance_to(50000); CHECK(test_layers[4]);
        button(index, false); flush_actions();
        advance_to(59999); CHECK(test_layers[4]);
        advance_to(60000); CHECK(!test_layers[4]);
    }
}

static void last_button_release_starts_timeout(void) {
    activate(); button(0, true); button(1, true); button(0, true); flush_actions();
    advance_to(20000); button(0, false); flush_actions();
    advance_to(40000); CHECK(test_layers[4]);
    button(1, false); flush_actions();
    advance_to(49999); CHECK(test_layers[4]);
    advance_to(50000); CHECK(!test_layers[4]);
}

static void normal_key_cancels_even_while_held(void) {
    activate(); button(0, true); flush_actions();
    position(13); CHECK(!test_layers[4]);
    button(0, false); flush_actions(); CHECK(!test_layers[4]);
    advance_to(30000); CHECK(!test_layers[4]);
    activate();
    advance_to(40000); CHECK(!test_layers[4]);
}

static void external_deactivation_stays_off(void) {
    activate(); button(0, true); flush_actions();
    zmk_keymap_layer_deactivate(4);
    button(0, false); flush_actions(); CHECK(!test_layers[4]);
    advance_to(30000); CHECK(!test_layers[4]);
}

static void excluded_positions_keep_layer(void) {
    activate();
    position(33); position(34); position(35); CHECK(test_layers[4]);
    position(36); CHECK(!test_layers[4]);
}

static void queue_expired_timeout(void) {
    test_now = 11000;
    layer_disable_works[4].scheduled = false;
    layer_disable_callback(&layer_disable_works[4].work);
}

static void stale_expiry_cannot_override_movement(void) {
    activate(); queue_expired_timeout();
    move(1); flush_actions(); CHECK(test_layers[4]);
    advance_to(20999); CHECK(test_layers[4]);
    advance_to(21000); CHECK(!test_layers[4]);
}

static void stale_expiry_cannot_override_button_hold(void) {
    activate(); queue_expired_timeout();
    button(0, true); flush_actions(); CHECK(test_layers[4]);
    advance_to(30000); CHECK(test_layers[4]);
    button(0, false); flush_actions();
    advance_to(39999); CHECK(test_layers[4]);
    advance_to(40000); CHECK(!test_layers[4]);
}

static void normal_key_cancels_queued_activation(void) {
    move(8); position(13); flush_actions();
    CHECK(!test_layers[4]);
}

static void threshold_window_and_prior_idle_are_preserved(void) {
    zmk_event_t keycode = {.kind = KEYCODE, .keycode = {.state = true, .timestamp = 1000}};
    handle_state_changed_dispatcher(&test_device, &keycode);
    move(8); flush_actions(); CHECK(!test_layers[4]);
    advance_to(1299); move(8); flush_actions(); CHECK(!test_layers[4]);
    advance_to(1300); move(4); flush_actions(); CHECK(!test_layers[4]);
    advance_to(1400); move(4); flush_actions(); CHECK(!test_layers[4]);
    advance_to(1450); input(INPUT_EV_REL, INPUT_REL_Y, -4, 10000);
    flush_actions(); CHECK(test_layers[4]);
}

static void buttons_alone_do_not_activate(void) {
    button(0, true); button(0, false); flush_actions(); CHECK(!test_layers[4]);
    move(7); button(1, true); button(1, false); flush_actions(); CHECK(!test_layers[4]);
    move(1); flush_actions(); CHECK(test_layers[4]);
}

static void unrelated_events_do_not_extend_timeout(void) {
    activate(); advance_to(5000);
    input(INPUT_EV_REL, INPUT_REL_WHEEL, 8, 10000);
    input(INPUT_EV_REL, INPUT_REL_X, 0, 10000);
    input(INPUT_EV_KEY, INPUT_BTN_4 + 1, 1, 10000);
    flush_actions(); advance_to(11000); CHECK(!test_layers[4]);
}

static void zero_timeout_cancels_pending_expiry(void) {
    activate(); advance_to(5000);
    input(INPUT_EV_REL, INPUT_REL_X, 1, 0); flush_actions();
    advance_to(30000); CHECK(test_layers[4]);
    position(13); CHECK(!test_layers[4]);
}

#define TEST(name) {#name, name}
int main(int argc, char **argv) {
    const struct { const char *name; void (*run)(void); } tests[] = {
        TEST(click_extends_timeout), TEST(movement_after_click_extends_timeout),
        TEST(all_mouse_buttons_suspend_timeout), TEST(last_button_release_starts_timeout),
        TEST(normal_key_cancels_even_while_held), TEST(external_deactivation_stays_off),
        TEST(excluded_positions_keep_layer), TEST(stale_expiry_cannot_override_movement),
        TEST(stale_expiry_cannot_override_button_hold), TEST(normal_key_cancels_queued_activation),
        TEST(threshold_window_and_prior_idle_are_preserved), TEST(buttons_alone_do_not_activate),
        TEST(unrelated_events_do_not_extend_timeout), TEST(zero_timeout_cancels_pending_expiry),
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (argc == 1 || strcmp(argv[1], tests[i].name) == 0) {
            reset(); tests[i].run(); printf("PASS %s\n", tests[i].name);
        }
    }
    return 0;
}
