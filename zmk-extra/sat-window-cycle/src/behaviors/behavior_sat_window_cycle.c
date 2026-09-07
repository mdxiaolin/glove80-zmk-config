/*
 * Stateful Super Alt-Tab behavior for the Glove80 PACS layout.
 *
 * The first tap owns and presses left Alt, then taps Tab. Further taps within
 * the one-second window tap Tab again. A physical key press ends the window
 * before that key is resolved, while the two configured Shift positions stay
 * ignored so Shift+SAT sends reverse Alt+Shift+Tab. The delayed work releases
 * only an Alt press owned by this behavior.
 */

#define DT_DRV_COMPAT zmk_behavior_sat_window_cycle

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_sat_window_cycle_config {
    int32_t ignored_key_positions_len;
    int32_t timeout_ms;
    struct zmk_behavior_binding alt_behavior;
    struct zmk_behavior_binding tab_behavior;
    uint8_t ignored_key_positions[];
};

struct active_sat_window_cycle {
    bool active;
    bool owns_alt;
    bool tab_down;
    uint32_t position;
    zmk_keymap_layer_id_t layer;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    const struct behavior_sat_window_cycle_config *config;
    struct k_work_delayable timeout_work;
};

static struct active_sat_window_cycle sat_state;

static struct zmk_behavior_binding_event sat_event(void) {
    return (struct zmk_behavior_binding_event){
        .layer = sat_state.layer,
        .position = sat_state.position,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = sat_state.source,
#endif
    };
}

static bool sat_position_is_ignored(const struct zmk_position_state_changed *event) {
    if (event->position == sat_state.position) {
        return true;
    }

    for (int32_t i = 0; i < sat_state.config->ignored_key_positions_len; ++i) {
        if (event->position == sat_state.config->ignored_key_positions[i]) {
            return true;
        }
    }

    return false;
}

static void sat_release_owned_alt(void) {
    if (!sat_state.owns_alt || sat_state.config == NULL) {
        return;
    }

    sat_state.owns_alt = false;
    struct zmk_behavior_binding_event event = sat_event();
    int ret = zmk_behavior_queue_add(&event, sat_state.config->alt_behavior, false, 0);
    if (ret < 0) {
        /* Keep cleanup reliable even if the behavior queue is temporarily full. */
        LOG_ERR("SAT Alt release queue failed (%d); invoking directly", ret);
        zmk_behavior_invoke_binding(&sat_state.config->alt_behavior, event, false);
    }
}

static void sat_stop_window(void) {
    if (!sat_state.active && !sat_state.owns_alt) {
        return;
    }

    sat_state.active = false;
    k_work_cancel_delayable(&sat_state.timeout_work);
    sat_release_owned_alt();
}

static void sat_timeout_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!sat_state.active) {
        return;
    }

    LOG_DBG("SAT window expired");
    sat_stop_window();
}

static int sat_start_window(const struct behavior_sat_window_cycle_config *config,
                            struct zmk_behavior_binding_event event) {
    sat_state.active = true;
    sat_state.owns_alt = false;
    sat_state.tab_down = false;
    sat_state.position = event.position;
    sat_state.layer = event.layer;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    sat_state.source = event.source;
#endif
    sat_state.config = config;

    /* Do not release an Alt modifier that was already held by the user. */
    if (!zmk_hid_mod_is_pressed(MOD_LALT)) {
        int ret = zmk_behavior_invoke_binding(&config->alt_behavior, event, true);
        if (ret < 0) {
            sat_state.active = false;
            sat_state.config = NULL;
            return ret;
        }
        sat_state.owns_alt = true;
    }

    return 0;
}

static int sat_tap_tab(const struct behavior_sat_window_cycle_config *config,
                       struct zmk_behavior_binding_event event) {
    if (sat_state.tab_down) {
        return 0;
    }

    int ret = zmk_behavior_invoke_binding(&config->tab_behavior, event, true);
    if (ret < 0) {
        return ret;
    }

    sat_state.tab_down = true;
    return 0;
}

static int on_sat_binding_pressed(struct zmk_behavior_binding *binding,
                                  struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (dev == NULL) {
        return -ENODEV;
    }

    const struct behavior_sat_window_cycle_config *config = dev->config;
    if (!sat_state.active) {
        int ret = sat_start_window(config, event);
        if (ret < 0) {
            LOG_ERR("SAT could not press Alt (%d)", ret);
            return ret;
        }
    }

    int ret = sat_tap_tab(config, event);
    if (ret < 0) {
        sat_stop_window();
        return ret;
    }

    if (config->timeout_ms > 0) {
        k_work_reschedule(&sat_state.timeout_work, K_MSEC(config->timeout_ms));
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sat_binding_released(struct zmk_behavior_binding *binding,
                                   struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    if (dev == NULL) {
        return -ENODEV;
    }

    const struct behavior_sat_window_cycle_config *config = dev->config;
    if (sat_state.tab_down) {
        zmk_behavior_invoke_binding(&config->tab_behavior, event, false);
        sat_state.tab_down = false;
    }

    return ZMK_BEHAVIOR_OPAQUE;
}

static int sat_position_state_changed_listener(const zmk_event_t *event_handle) {
    struct zmk_position_state_changed *event = as_zmk_position_state_changed(event_handle);
    if (event == NULL || !event->state || !sat_state.active || sat_position_is_ignored(event)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* This listener runs on the physical position event before keycode dispatch. */
    LOG_DBG("SAT cancelled by physical position %d", event->position);
    sat_stop_window();
    return ZMK_EV_EVENT_BUBBLE;
}

static int sat_layer_state_changed_listener(const zmk_event_t *event_handle) {
    struct zmk_layer_state_changed *event = as_zmk_layer_state_changed(event_handle);
    if (event == NULL || event->state || !sat_state.active || event->layer != sat_state.layer) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    LOG_DBG("SAT cancelled by layer %d deactivation", event->layer);
    sat_stop_window();
    return ZMK_EV_EVENT_BUBBLE;
}

static int sat_event_listener(const zmk_event_t *event_handle) {
    if (as_zmk_position_state_changed(event_handle) != NULL) {
        return sat_position_state_changed_listener(event_handle);
    }
    if (as_zmk_layer_state_changed(event_handle) != NULL) {
        return sat_layer_state_changed_listener(event_handle);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(behavior_sat_window_cycle, sat_event_listener);
ZMK_SUBSCRIPTION(behavior_sat_window_cycle, zmk_position_state_changed);
ZMK_SUBSCRIPTION(behavior_sat_window_cycle, zmk_layer_state_changed);

static int sat_behavior_init(const struct device *dev) {
    ARG_UNUSED(dev);
    sat_state = (struct active_sat_window_cycle){};
    k_work_init_delayable(&sat_state.timeout_work, sat_timeout_work_handler);
    return 0;
}

static const struct behavior_driver_api sat_behavior_driver_api = {
    .binding_pressed = on_sat_binding_pressed,
    .binding_released = on_sat_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

#define SAT_INST(n)                                                                            \
    static struct behavior_sat_window_cycle_config sat_config_##n = {                          \
        .ignored_key_positions = DT_INST_PROP(n, ignored_key_positions),                      \
        .ignored_key_positions_len = DT_INST_PROP_LEN(n, ignored_key_positions),               \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                             \
        .alt_behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                         \
        .tab_behavior = ZMK_KEYMAP_EXTRACT_BINDING(1, DT_DRV_INST(n)),                         \
    };                                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, sat_behavior_init, NULL, NULL, &sat_config_##n, POST_KERNEL,    \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &sat_behavior_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SAT_INST)
