/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Tactil de la pantalla del dongle (CST816T de la Waveshare 1.69", driver de
 * Zephyr hynitron,cst816s en i2c0 0x15; IRQ en D15, RST en D18):
 *
 * - Un toque: apaga o enciende la pantalla (dongle_backlight_toggle(), en
 *   display_als_backlight.c). Apagada por toque se queda apagada aunque se
 *   escriba, hasta el siguiente toque.
 * - Dos toques: si el PC esta en suspension, lo despierta. Manda F24 por USB:
 *   con el bus suspendido ZMK no envia la tecla, pide el "remote wakeup", asi
 *   que no llega a escribirse nada. Con el PC despierto no hace nada.
 *
 * Solo se mira INPUT_BTN_TOUCH; la posicion da igual. Para distinguir un toque
 * de dos, el primero espera DOUBLE_TAP_MS antes de actuar.
 *
 * Ojo: ZMK cuenta cualquier evento de input como actividad (activity.c), asi
 * que tocar la pantalla reinicia la cuenta de IDLE. Pero no saca del modo
 * reloj: src/dongle_rtc.c ignora el tactil, solo sale con teclas o trackball.
 *
 * Si el driver no arranca (cables, o un clon con otro chip ID, como paso con el
 * sensor de luz), sale un aviso arriba unos segundos con lo que responde.
 */

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/keys.h>
#include <lvgl.h>
#include <zmk/display.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/usb.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Tiempo maximo entre el primer toque y el segundo para contar como doble. */
#define DOUBLE_TAP_MS 350
/* Un toque mas largo que esto no cuenta (se ha dejado el dedo encima). */
#define MAX_TAP_MS 600
/* Cuanto se mantiene F24 "pulsada" al despertar el PC. */
#define WAKE_KEY_MS 30

#define TOUCH_NODE DT_NODELABEL(touch)

static const struct device *touch_dev = DEVICE_DT_GET(TOUCH_NODE);

void dongle_backlight_toggle(void);

/* ---------------------------------------------------------------------------
 * Acciones (en la cola de trabajo del sistema, no en el hilo de input)
 * ------------------------------------------------------------------------- */

static void wake_release_cb(struct k_work *work) {
    raise_zmk_keycode_state_changed_from_encoded(F24, false, k_uptime_get());
}

static K_WORK_DELAYABLE_DEFINE(wake_release_work, wake_release_cb);

static void double_tap_cb(struct k_work *work) {
    if (zmk_usb_get_status() != USB_DC_SUSPEND) {
        return;
    }
    raise_zmk_keycode_state_changed_from_encoded(F24, true, k_uptime_get());
    k_work_schedule(&wake_release_work, K_MSEC(WAKE_KEY_MS));
}

static K_WORK_DEFINE(double_tap_work, double_tap_cb);

/* Hay un toque esperando a ver si llega el segundo. */
static atomic_t tap_pending;

static void single_tap_cb(struct k_work *work) {
    /* Si a la vez ha llegado el segundo toque, gana el doble toque. */
    if (atomic_cas(&tap_pending, 1, 0)) {
        dongle_backlight_toggle();
    }
}

static K_WORK_DELAYABLE_DEFINE(single_tap_work, single_tap_cb);

/* ---------------------------------------------------------------------------
 * Toques (en el hilo de input)
 * ------------------------------------------------------------------------- */

static bool touching;
static int64_t press_time;

static void touch_input_cb(struct input_event *ev, void *user_data) {
    if (ev->type != INPUT_EV_KEY || ev->code != INPUT_BTN_TOUCH) {
        return;
    }

    /* El driver repite "pulsado" mientras el dedo sigue encima. */
    if (ev->value) {
        if (!touching) {
            touching = true;
            press_time = k_uptime_get();
        }
        return;
    }
    if (!touching) {
        return;
    }
    touching = false;

    if (k_uptime_get() - press_time > MAX_TAP_MS) {
        return;
    }

    if (atomic_cas(&tap_pending, 1, 0)) {
        /* Segundo toque a tiempo: cancela el primero y es un doble toque. */
        k_work_cancel_delayable(&single_tap_work);
        k_work_submit(&double_tap_work);
    } else {
        atomic_set(&tap_pending, 1);
        k_work_schedule(&single_tap_work, K_MSEC(DOUBLE_TAP_MS));
    }
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(TOUCH_NODE), touch_input_cb, NULL);

/* ---------------------------------------------------------------------------
 * Aviso si el tactil no arranca
 * ------------------------------------------------------------------------- */

static char warn_text[40];
static lv_obj_t *warn_label;

static void warn_show_cb(struct k_work *work) {
    if (warn_label == NULL) {
        warn_label = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(warn_label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(warn_label, lv_color_hex(0xA00000), 0);
        lv_obj_set_style_bg_opa(warn_label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(warn_label, 3, 0);
        lv_obj_align(warn_label, LV_ALIGN_TOP_MID, 0, 34);
    }
    lv_label_set_text(warn_label, warn_text);
}

static K_WORK_DEFINE(warn_show_work, warn_show_cb);

static void warn_hide_cb(struct k_work *work) {
    if (warn_label != NULL) {
        lv_obj_delete(warn_label);
        warn_label = NULL;
    }
}

static K_WORK_DEFINE(warn_hide_work, warn_hide_cb);

static void hide_later_cb(struct k_work *work) {
    k_work_submit_to_queue(zmk_display_work_q(), &warn_hide_work);
}

static K_WORK_DELAYABLE_DEFINE(hide_later_work, hide_later_cb);

static void check_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(check_work, check_cb);

static void check_cb(struct k_work *work) {
    if (device_is_ready(touch_dev)) {
        return;
    }
    if (!zmk_display_is_initialized()) {
        k_work_schedule(&check_work, K_SECONDS(1));
        return;
    }

    /* El driver comprueba el registro 0xA7 (chip ID: 0xB4, 0xB5 o 0xB6). */
    const struct i2c_dt_spec i2c = I2C_DT_SPEC_GET(TOUCH_NODE);
    uint8_t id;
    if (i2c_reg_read_byte_dt(&i2c, 0xA7, &id)) {
        snprintf(warn_text, sizeof(warn_text), "tactil ERR i2c");
    } else {
        snprintf(warn_text, sizeof(warn_text), "tactil ERR id %02X", id);
    }
    k_work_submit_to_queue(zmk_display_work_q(), &warn_show_work);
    k_work_schedule(&hide_later_work, K_SECONDS(15));
}

static int dongle_touch_init(void) {
    k_work_schedule(&check_work, K_SECONDS(4));
    return 0;
}

SYS_INIT(dongle_touch_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
