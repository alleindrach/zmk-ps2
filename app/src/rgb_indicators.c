/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

#include <zmk/battery.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/hid_indicators.h>
#include <zmk/usb.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <drivers/ext_power.h>

#include <zmk/rgb_underglow.h>

#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
#include <zmk/split/bluetooth/central.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_indicators)

#error "A zmk,indicators chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_indicators)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100
#define INDICATOR_STATUS_UPDATE_INTERVAL_MSEC 1000
enum rgb_indicators_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct rgb_indicators_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
    bool status_active;
    uint16_t status_animation_step;
};

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];
static struct led_rgb status_pixels[STRIP_NUM_PIXELS];

static struct rgb_indicators_state state;

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

void zmk_rgb_indicators_set_ext_power(void);

// static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
//     hsb.b = hsb.b * CONFIG_ZMK_RGB_INDICATORS_BRT / BRT_MAX;
//     return hsb;
// }

// static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
//     hsb.b = hsb.b * CONFIG_ZMK_RGB_INDICATORS_BRT / BRT_MAX;
//     return hsb;
// }

// static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
//     float r, g, b;

//     uint8_t i = hsb.h / 60;
//     float v = hsb.b / ((float)BRT_MAX);
//     float s = hsb.s / ((float)SAT_MAX);
//     float f = hsb.h / ((float)HUE_MAX) * 6 - i;
//     float p = v * (1 - s);
//     float q = v * (1 - f * s);
//     float t = v * (1 - (1 - f) * s);

//     switch (i % 6) {
//     case 0:
//         r = v;
//         g = t;
//         b = p;
//         break;
//     case 1:
//         r = q;
//         g = v;
//         b = p;
//         break;
//     case 2:
//         r = p;
//         g = v;
//         b = t;
//         break;
//     case 3:
//         r = p;
//         g = q;
//         b = v;
//         break;
//     case 4:
//         r = t;
//         g = p;
//         b = v;
//         break;
//     case 5:
//         r = v;
//         g = p;
//         b = q;
//         break;
//     }

//     struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

//     return rgb;
// }

// static void zmk_rgb_underglow_effect_solid(void) {
//     for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
//         pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
//     }
// }

// static void zmk_rgb_underglow_effect_breathe(void) {
//     for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
//         struct zmk_led_hsb hsb = state.color;
//         hsb.b = abs(state.animation_step - 1200) / 12;

//         pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
//     }

//     state.animation_step += state.animation_speed * 10;

//     if (state.animation_step > 2400) {
//         state.animation_step = 0;
//     }
// }

// static void zmk_rgb_underglow_effect_spectrum(void) {
//     for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
//         struct zmk_led_hsb hsb = state.color;
//         hsb.h = state.animation_step;

//         pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
//     }

//     state.animation_step += state.animation_speed;
//     state.animation_step = state.animation_step % HUE_MAX;
// }

// static void zmk_rgb_underglow_effect_swirl(void) {
//     for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
//         struct zmk_led_hsb hsb = state.color;
//         hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;

//         pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
//     }

//     state.animation_step += state.animation_speed * 2;
//     state.animation_step = state.animation_step % HUE_MAX;
// }

static int zmk_led_generate_status(void);

static void zmk_led_write_pixels(void) {
    static struct led_rgb led_buffer[STRIP_NUM_PIXELS];
    int bat0 = zmk_battery_state_of_charge();
    int blend = 0;
    int reset_ext_power = 0;
    if (state.status_active) {
        blend = zmk_led_generate_status();
    }

    // fast path: no status indicators, battery level OK
    if (blend == 0 && bat0 >= 20) {
        led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
        return;
    }
    // battery below minimum charge
    if (bat0 < 10) {
        memset(pixels, 0, sizeof(struct led_rgb) * STRIP_NUM_PIXELS);
        if (state.on) {
            int c_power = ext_power_get(ext_power);
            if (c_power && !state.status_active) {
                // power is on, RGB underglow is on, but battery is too low
                state.on = false;
                reset_ext_power = true;
            }
        }
    }

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        led_buffer[i] = status_pixels[i];
    }

    int err = led_strip_update_rgb(led_strip, led_buffer, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }

    if (reset_ext_power) {
        zmk_rgb_indicators_set_ext_power();
    }
}

#define RGB_INDICATORS DT_PATH(rgb_indicators)

#if defined(DT_N_S_rgb_indicators_EXISTS)
#define RGB_INDICATORS_ENABLED 1
#else
#define RGB_INDICATORS_ENABLED 0
#endif

#if !RGB_INDICATORS_ENABLED
static int zmk_led_generate_status(void) { return 0; }
#else

const uint8_t indicators_layer_state = DT_PROP(RGB_INDICATORS, layer_state);
const uint8_t indicators_ble_state = DT_PROP(RGB_INDICATORS, ble_state);
// const uint8_t underglow_bat_lhs[] = DT_PROP(UNDERGLOW_INDICATORS, bat_lhs);
// const uint8_t underglow_bat_rhs[] = DT_PROP(UNDERGLOW_INDICATORS, bat_rhs);

#define HEXRGB(R, G, B)                                                                            \
    ((struct led_rgb){                                                                             \
        r : (CONFIG_ZMK_RGB_INDICATORS_BRT * (R)) / 0xff,                                          \
        g : (CONFIG_ZMK_RGB_INDICATORS_BRT * (G)) / 0xff,                                          \
        b : (CONFIG_ZMK_RGB_INDICATORS_BRT * (B)) / 0xff                                           \
    })
const struct led_rgb red = HEXRGB(0xff, 0x00, 0x00);
const struct led_rgb yellow = HEXRGB(0xff, 0xff, 0x00);
const struct led_rgb green = HEXRGB(0x00, 0xff, 0x00);
const struct led_rgb blue = HEXRGB(0x00, 0x00, 0xff);
const struct led_rgb dull_green = HEXRGB(0x00, 0xff, 0x68);
const struct led_rgb magenta = HEXRGB(0xff, 0x00, 0xff);
const struct led_rgb white = HEXRGB(0xff, 0xff, 0xff);
const struct led_rgb lilac = HEXRGB(0x6b, 0x1f, 0xce);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
static void zmk_led_battery_level(int bat_level, const uint8_t *addresses, size_t addresses_len) {
    struct led_rgb bat_colour;

    if (bat_level > 40) {
        bat_colour = green;
    } else if (bat_level > 20) {
        bat_colour = yellow;
    } else {
        bat_colour = red;
    }

    // originally, six levels, 0 .. 100

    for (int i = 0; i < addresses_len; i++) {
        int min_level = (i * 100) / (addresses_len - 1);
        if (bat_level >= min_level) {
            status_pixels[addresses[i]] = bat_colour;
        }
    }
}

static void zmk_led_fill(struct led_rgb color, const uint8_t *addresses, size_t addresses_len) {
    for (int i = 0; i < addresses_len; i++) {
        status_pixels[addresses[i]] = color;
    }
}
#endif
#define ZMK_LED_NUMLOCK_BIT BIT(0)
#define ZMK_LED_CAPSLOCK_BIT BIT(1)
#define ZMK_LED_SCROLLLOCK_BIT BIT(2)

static int zmk_led_generate_status(void) {
    // 全灭
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        status_pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    // BATTERY STATUS
    // zmk_led_battery_level(zmk_battery_state_of_charge(), underglow_bat_lhs,
    //                       DT_PROP_LEN(UNDERGLOW_INDICATORS, bat_lhs));

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING)
    uint8_t peripheral_level = 0;
    int rc = zmk_split_get_peripheral_battery_level(0, &peripheral_level);

    if (rc == 0) {
        zmk_led_battery_level(peripheral_level, underglow_bat_rhs,
                              DT_PROP_LEN(RGB_INDICATORS, bat_rhs));
    } else if (rc == -ENOTCONN) {
        zmk_led_fill(red, underglow_bat_rhs, DT_PROP_LEN(RGB_INDICATORS, bat_rhs));
    } else if (rc == -EINVAL) {
        LOG_ERR("Invalid peripheral index requested for battery level read: 0");
    }
#endif

    // CAPSLOCK/NUMLOCK/SCROLLOCK STATUS
    zmk_hid_indicators_t led_flags = zmk_hid_indicators_get_current_profile();

    if (led_flags & ZMK_LED_CAPSLOCK_BIT)
        status_pixels[DT_PROP(RGB_INDICATORS, capslock)] = red;
    // if (led_flags & ZMK_LED_NUMLOCK_BIT)
    //     status_pixels[DT_PROP(RGB_INDICATORS, numlock)] = red;
    // if (led_flags & ZMK_LED_SCROLLLOCK_BIT)
    //     status_pixels[DT_PROP(RGB_INDICATORS, scrolllock)] = red;
    LOG_WRN("capslock status_pixels[%d] R[%x],G[%x],B[%x]", DT_PROP(RGB_INDICATORS, capslock),
            status_pixels[DT_PROP(RGB_INDICATORS, capslock)].r, status_pixels[DT_PROP(RGB_INDICATORS, capslock)].g,status_pixels[DT_PROP(RGB_INDICATORS, capslock)].b);
    // LAYER STATUS
    int layer_activated[3] = {0, 0, 0};
    for (uint8_t i = 0; i < 3; i++) {
        if (zmk_keymap_layer_active(i))
            layer_activated[i] = 0xff;
    }

    status_pixels[indicators_layer_state] =
        HEXRGB(layer_activated[0], layer_activated[1], layer_activated[2]);

    LOG_WRN("layer  status_pixels[%d] R[%x],G[%x],B[%x]", indicators_layer_state,
            status_pixels[indicators_layer_state].r, status_pixels[indicators_layer_state].g,
            status_pixels[indicators_layer_state].b);
    struct zmk_endpoint_instance active_endpoint = zmk_endpoints_selected();

    // if (!zmk_endpoints_preferred_transport_is_active())
    //     status_pixels[DT_PROP(RGB_INDICATORS, output_fallback)] = red;

    int active_ble_profile_index = zmk_ble_active_profile_index();
    // int ble_pixel = indicators_ble_state; // 一个RGB灯显示三个蓝牙连接情况，R=1 G=2 B=3
    uint8_t ble_profile_activated[3] = {0, 0, 0};
    for (uint8_t i = 0; i < MIN(ZMK_BLE_PROFILE_COUNT, 3); i++) {
        int8_t status = zmk_ble_profile_status(i);

        if (status == 2 &&                   // active_endpoint.transport == ZMK_TRANSPORT_BLE &&
            active_ble_profile_index == i) { // connected AND active
            ble_profile_activated[i] = 0xff;
        }
        // ble_profile_activated[i] = 0xff;
        //  else if (status == 2) { // connected
        //     status_pixels[ble_pixel] = dull_green;
        // } else if (status == 1) { // paired
        //     status_pixels[ble_pixel] = red;
        // } else if (status == 0) { // unused
        //     status_pixels[ble_pixel] = lilac;
        // }
    }
    status_pixels[indicators_ble_state] =
        HEXRGB(ble_profile_activated[0], ble_profile_activated[1], ble_profile_activated[2]);
    LOG_WRN("Ble  status_pixels[%d] R[%x],G[%x],B[%x]", indicators_ble_state,
             status_pixels[indicators_ble_state].r, status_pixels[indicators_ble_state].g,
             status_pixels[indicators_ble_state].b);
    // enum zmk_usb_conn_state usb_state = zmk_usb_get_conn_state();
    // if (usb_state == ZMK_USB_CONN_HID &&
    //     active_endpoint.transport == ZMK_TRANSPORT_USB) { // connected AND active
    //     status_pixels[DT_PROP(RGB_INDICATORS, usb_state)] = white;
    // } else if (usb_state == ZMK_USB_CONN_HID) { // connected
    //     status_pixels[DT_PROP(RGB_INDICATORS, usb_state)] = dull_green;
    // } else if (usb_state == ZMK_USB_CONN_POWERED) { // powered
    //     status_pixels[DT_PROP(RGB_INDICATORS, usb_state)] = red;
    // } else if (usb_state == ZMK_USB_CONN_NONE) { // disconnected
    //     status_pixels[DT_PROP(RGB_INDICATORS, usb_state)] = lilac;
    // }

    int16_t blend = 256;
    // if (state.status_animation_step < (500 / 25)) {
    //     blend = ((state.status_animation_step * 256) / (500 / 25));
    // } else if (state.status_animation_step > (8000 / 25)) {
    //     blend = 256 - (((state.status_animation_step - (8000 / 25)) * 256) / (2000 / 25));
    // }
    // if (blend < 0)
    //     blend = 0;
    // if (blend > 256)
    //     blend = 256;

    return blend;
}
#endif // underglow_indicators exists

static void zmk_rgb_indicator_tick(struct k_work *work) {
    // switch (state.current_effect) {
    // case UNDERGLOW_EFFECT_SOLID:
    //     zmk_rgb_underglow_effect_solid();
    //     break;
    // case UNDERGLOW_EFFECT_BREATHE:
    //     zmk_rgb_underglow_effect_breathe();
    //     break;
    // case UNDERGLOW_EFFECT_SPECTRUM:
    //     zmk_rgb_underglow_effect_spectrum();
    //     break;
    // case UNDERGLOW_EFFECT_SWIRL:
    //     zmk_rgb_underglow_effect_swirl();
    //     break;
    // }

    zmk_led_write_pixels();
}

K_WORK_DEFINE(indicator_tick_work, zmk_rgb_indicator_tick);

static void zmk_rgb_indicator_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicator_tick_work);
}

K_TIMER_DEFINE(indicator_tick, zmk_rgb_indicator_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

struct settings_handler rgb_indicators_conf = {.name = "rgb/indicators", .h_set = rgb_settings_set};

static void zmk_rgb_indicators_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/indicators/state", &state, sizeof(state));
}

static struct k_work_delayable indicator_save_work;
#endif

static int zmk_rgb_indicators_init(void) {
    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    state = (struct rgb_indicators_state){
        color : {
            h : CONFIG_ZMK_RGB_INDICATORS_HUE,
            s : CONFIG_ZMK_RGB_INDICATORS_SAT,
            b : CONFIG_ZMK_RGB_INDICATORS_BRT,
        },
        animation_speed : CONFIG_ZMK_RGB_INDICATORS_SPD,
        current_effect : CONFIG_ZMK_RGB_INDICATORS_EFF,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_ON_START),
        status_active : IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();

    int err = settings_register(&rgb_indicators_conf);
    if (err) {
        LOG_ERR("Failed to register the ext_power settings handler (err %d)", err);
        return err;
    }

    k_work_init_delayable(&indicator_save_work, zmk_rgb_indicators_save_state_work);

    settings_load_subtree("rgb/indicators");
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        k_timer_start(&indicator_tick, K_NO_WAIT, K_MSEC(INDICATOR_STATUS_UPDATE_INTERVAL_MSEC));
    }

    return 0;
}

int zmk_rgb_indicators_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&indicator_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_rgb_indicators_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on;
    return 0;
}

void zmk_rgb_indicators_set_ext_power(void) {
#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_EXT_POWER)
    if (ext_power == NULL)
        return;
    int c_power = ext_power_get(ext_power);
    if (c_power < 0) {
        LOG_ERR("Unable to examine EXT_POWER: %d", c_power);
        c_power = 0;
    }
    int desired_state = state.on || state.status_active;
    // force power off, when battery low (<10%)
    if (state.on && !state.status_active) {
        if (zmk_battery_state_of_charge() < 10) {
            desired_state = false;
        }
    }
    if (desired_state && !c_power) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    } else if (!desired_state && c_power) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif
}

int zmk_rgb_indicators_on(void) {
    if (!led_strip)
        return -ENODEV;

    state.on = true;
    state.status_active = true;
    zmk_rgb_indicators_set_ext_power();

    state.animation_step = 0;
    k_timer_start(&indicator_tick, K_NO_WAIT, K_MSEC(INDICATOR_STATUS_UPDATE_INTERVAL_MSEC));
    LOG_WRN("zmk_rgb_indicators_on ");
    return zmk_rgb_indicators_save_state();
}

static void zmk_rgb_indicators_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    zmk_led_write_pixels();
}

K_WORK_DEFINE(indicators_off_work, zmk_rgb_indicators_off_handler);

int zmk_rgb_indicators_off(void) {
    if (!led_strip)
        return -ENODEV;

    k_timer_stop(&indicator_tick);
    state.on = false;
    state.status_active = false;
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &indicators_off_work);

    zmk_rgb_indicators_set_ext_power();

    return zmk_rgb_indicators_save_state();
}

int zmk_rgb_indicators_toggle(void) {
    return state.on ? zmk_rgb_indicators_off() : zmk_rgb_indicators_on();
}

static void zmk_led_write_pixels_work(struct k_work *work);
static void zmk_rgb_indicators_status_update(struct k_timer *timer);

K_WORK_DEFINE(indicators_write_work, zmk_led_write_pixels_work);
K_TIMER_DEFINE(indicators_status_update_timer, zmk_rgb_indicators_status_update, NULL);

static void zmk_rgb_indicators_status_update(struct k_timer *timer) {
    if (!state.status_active)
        return;
    state.status_animation_step++;
    if (state.status_animation_step > (10000 / 25)) {
        state.status_active = false;
        k_timer_stop(&indicators_status_update_timer);
    }
    if (!k_work_is_pending(&indicators_write_work))
        k_work_submit(&indicators_write_work);
}

static void zmk_led_write_pixels_work(struct k_work *work) {
    zmk_led_write_pixels();
    if (!state.status_active) {
        zmk_rgb_indicators_set_ext_power();
    }
}

int zmk_rgb_indicators_status(void) {

    state.status_active = true;
    zmk_led_write_pixels();
    zmk_rgb_indicators_set_ext_power();

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_IDLE) ||                                         \
    IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_USB)
static int rgb_indicators_auto_state(bool *prev_state, bool new_state) {
    if (state.on == new_state) {
        return 0;
    }
    if (new_state) {
        state.on = *prev_state;
        *prev_state = false;
        return zmk_rgb_indicators_on();
    } else {
        state.on = false;
        *prev_state = true;
        return zmk_rgb_indicators_off();
    }
}

static int rgb_indicators_event_listener(const zmk_event_t *eh) {

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        static bool prev_state = false;
        return rgb_indicators_auto_state(&prev_state,
                                         zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        static bool prev_state = false;
        return rgb_indicators_auto_state(&prev_state, zmk_usb_is_powered());
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(rgb_indicators, rgb_indicators_event_listener);
#endif // IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_IDLE) ||
       // IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_USB)

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(rgb_indicators, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_RGB_INDICATORS_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(rgb_indicators, zmk_usb_conn_state_changed);
#endif

SYS_INIT(zmk_rgb_indicators_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
