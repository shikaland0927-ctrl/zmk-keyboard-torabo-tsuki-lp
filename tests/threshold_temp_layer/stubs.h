/* Minimal host-side Zephyr/ZMK API for deterministic processor regression tests. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define DT_HAS_COMPAT_STATUS_OKAY(...) 1
#define DT_INST_FOREACH_STATUS_OKAY(...)
#define DT_INST_FOREACH_STATUS_OKAY_VARGS(...) 1
#define DEVICE_DT_INST_GET(...) (&test_device)
#define ZMK_KEYMAP_LAYERS_LEN 7
#define ZMK_LISTENER(...)
#define ZMK_SUBSCRIPTION(...)
#define ZMK_EV_EVENT_BUBBLE 0
#define ZMK_INPUT_PROC_CONTINUE 0
#define LOG_MODULE_DECLARE(...)
#define LOG_DBG(...)
#define LOG_ERR(...)
#define ARG_UNUSED(value) ((void)(value))
#define BIT(bit) (1UL << (bit))
#define ARRAY_INDEX(array, item) ((item) - (array))
#define K_FOREVER (-1)
#define K_NO_WAIT 0
#define K_MSEC(ms) (ms)

/* ZMK v0.3 uses INPUT_BTN_0..4, not INPUT_BTN_LEFT..EXTRA. */
#define INPUT_EV_KEY 1
#define INPUT_EV_REL 2
#define INPUT_REL_X 0
#define INPUT_REL_Y 1
#define INPUT_REL_WHEEL 8
#define INPUT_BTN_0 0x100
#define INPUT_BTN_4 0x104

struct device { void *data; const void *config; };
static struct device test_device;
static int64_t test_now;
static int64_t k_uptime_get(void) { return test_now; }

struct input_event { uint8_t type; uint16_t code; int32_t value; };
struct zmk_input_processor_state { uint8_t input_device_index; };
struct zmk_input_processor_driver_api {
    int (*handle_event)(const struct device *, struct input_event *, uint32_t, uint32_t,
                        struct zmk_input_processor_state *);
};

struct k_mutex { int unused; };
static void k_mutex_init(struct k_mutex *lock) {}
static int k_mutex_lock(struct k_mutex *lock, int timeout) { return 0; }
static int k_mutex_unlock(struct k_mutex *lock) { return 0; }

struct k_work { void (*handler)(struct k_work *); bool submitted; };
struct k_work_delayable { struct k_work work; int64_t deadline; bool scheduled; };
#define K_WORK_DEFINE(name, callback) struct k_work name = {.handler = callback}
static void k_work_init_delayable(struct k_work_delayable *work,
                                  void (*callback)(struct k_work *)) {
    *work = (struct k_work_delayable){.work.handler = callback};
}
static struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return (struct k_work_delayable *)work;
}
static int k_work_cancel_delayable(struct k_work_delayable *work) {
    work->scheduled = false;
    return 0;
}
static int k_work_reschedule(struct k_work_delayable *work, int64_t delay) {
    work->deadline = test_now + delay;
    work->scheduled = true;
    return 1;
}
static int k_work_submit(struct k_work *work) { work->submitted = true; return 1; }

struct k_msgq {
    unsigned char buffer[128];
    size_t item_size, capacity, head, count;
};
#define K_MSGQ_DEFINE(name, size, count, alignment) \
    struct k_msgq name = {.item_size = (size), .capacity = (count)}
static int k_msgq_put(struct k_msgq *queue, const void *item, int timeout) {
    if (queue->count == queue->capacity) { return -ENOSPC; }
    size_t slot = (queue->head + queue->count++) % queue->capacity;
    memcpy(queue->buffer + slot * queue->item_size, item, queue->item_size);
    return 0;
}
static int k_msgq_get(struct k_msgq *queue, void *item, int timeout) {
    if (!queue->count) { return -ENOMSG; }
    memcpy(item, queue->buffer + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    return 0;
}

struct zmk_position_state_changed { bool state; uint32_t position; };
struct zmk_keycode_state_changed { bool state; int64_t timestamp; };
struct zmk_layer_state_changed { bool state; uint8_t layer; };
typedef struct {
    enum { POSITION, KEYCODE, LAYER } kind;
    union {
        struct zmk_position_state_changed position;
        struct zmk_keycode_state_changed keycode;
        struct zmk_layer_state_changed layer;
    };
} zmk_event_t;
static const struct zmk_position_state_changed *as_zmk_position_state_changed(const zmk_event_t *e) {
    return e->kind == POSITION ? &e->position : NULL;
}
static const struct zmk_keycode_state_changed *as_zmk_keycode_state_changed(const zmk_event_t *e) {
    return e->kind == KEYCODE ? &e->keycode : NULL;
}
static const struct zmk_layer_state_changed *as_zmk_layer_state_changed(const zmk_event_t *e) {
    return e->kind == LAYER ? &e->layer : NULL;
}

static bool test_layers[ZMK_KEYMAP_LAYERS_LEN];
static bool zmk_keymap_layer_active(uint8_t layer) { return test_layers[layer]; }
static uint8_t zmk_keymap_layer_index_to_id(uint8_t layer) { return layer; }
static int zmk_keymap_layer_activate(uint8_t layer);
static int zmk_keymap_layer_deactivate(uint8_t layer);
