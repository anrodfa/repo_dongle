/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Brillo de la pantalla Prospector del dongle segun el sensor de luz APDS9960,
 * y backlight a 0 cuando ZMK pasa a inactivo (como display_idle_backlight.c,
 * que es la version de brillo fijo y no se compila a la vez que esta).
 *
 * No se usa el hilo del sensor del modulo (PROSPECTOR_USE_AMBIENT_LIGHT_SENSOR
 * se queda a n): no sabe nada de IDLE, guarda el brillo en una static que no
 * se puede tocar desde fuera y mapea lecturas de 0 a 100, que con luz normal
 * de habitacion se saturan.
 *
 * La lectura cruda (canal "clear" del sensor) se pasa a brillo en escala
 * logaritmica entre CONFIG_DONGLE_ALS_MIN_READING y _MAX_READING: el ojo
 * percibe la luz asi, y entre una lampara y luz de dia hay un factor 10-100.
 * Para calibrar, CONFIG_DONGLE_ALS_SHOW_READING muestra la lectura y el brillo
 * en una esquina de la pantalla. "luz --" fijo significa que el sensor no
 * responde: el driver espera a la linea INT (D14) para cada lectura.
 *
 * Si el sensor no arranca, la pantalla se queda en
 * CONFIG_PROSPECTOR_FIXED_BRIGHTNESS (lo que pone el modulo al arrancar) y se
 * sigue apagando en reposo.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#if IS_ENABLED(CONFIG_DONGLE_ALS_SHOW_READING)
#include <lvgl.h>
#include <zephyr/drivers/i2c.h>
#include <zmk/display.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SAMPLE_MS 200
#define FADE_STEP_MS 8
/* Diferencia minima de brillo (%) para cambiarlo: evita que tiemble. */
#define HYSTERESIS 2

/* Mismo dispositivo y canal que usa brightness.c del modulo. */
static const struct device *pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);
#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

static const struct device *als_dev = DEVICE_DT_GET_ONE(avago_apds9960);

/* Protege active/target/current entre el listener y el hilo del sensor. */
static K_MUTEX_DEFINE(bl_lock);
static bool active = true;
/* Brillo que toca segun la luz; se aplica cuando ZMK esta activo. */
static uint8_t target = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;
/* Brillo puesto ahora mismo. El modulo pone FIXED_BRIGHTNESS al arrancar. */
static uint8_t current = CONFIG_PROSPECTOR_FIXED_BRIGHTNESS;

/* Llamar con bl_lock cogido. */
static void set_backlight(uint8_t brightness) {
    if (brightness == current) {
        return;
    }
    if (led_set_brightness(pwm_leds_dev, DISP_BL, brightness)) {
        LOG_ERR("No se pudo poner el backlight a %d", brightness);
        return;
    }
    current = brightness;
}

/* log2(x) con 4 bits de fraccion (x >= 1). */
static uint32_t log2_q4(uint32_t x) {
    uint32_t ip = 31 - __builtin_clz(x);
    /* Mantisa en Q16, en [1, 2). */
    uint64_t m = ((uint64_t)x << 16) >> ip;
    uint32_t frac = 0;

    for (int i = 0; i < 4; i++) {
        m = (m * m) >> 16;
        frac <<= 1;
        if (m >= (2 << 16)) {
            m >>= 1;
            frac |= 1;
        }
    }

    return (ip << 4) | frac;
}

static uint8_t reading_to_brightness(int32_t reading) {
    const int32_t lo = MAX(CONFIG_DONGLE_ALS_MIN_READING, 1);
    const int32_t hi = MAX(CONFIG_DONGLE_ALS_MAX_READING, lo + 1);
    const int32_t b_min = CONFIG_DONGLE_ALS_MIN_BRIGHTNESS;
    const int32_t b_max = CONFIG_DONGLE_ALS_MAX_BRIGHTNESS;

    if (reading <= lo) {
        return b_min;
    }
    if (reading >= hi) {
        return b_max;
    }

    int32_t span = log2_q4(hi) - log2_q4(lo);
    if (span <= 0) {
        return b_max;
    }

    return b_min + (b_max - b_min) * (int32_t)(log2_q4(reading) - log2_q4(lo)) / span;
}

#if IS_ENABLED(CONFIG_DONGLE_ALS_SHOW_READING)

#define READING_NONE -1
#define READING_ERROR -2

static lv_obj_t *reading_label;
static int32_t shown_reading = READING_NONE;
static uint8_t shown_brightness;
/* Detalle de READING_ERROR: que fallo al arrancar el sensor. */
static char shown_error[16];

/* Corre en la cola de trabajo de la pantalla: es la unica que puede tocar LVGL. */
static void show_reading_cb(struct k_work *work) {
    char text[24];

    if (reading_label == NULL) {
        reading_label = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(reading_label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(reading_label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(reading_label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(reading_label, 2, 0);
        lv_obj_align(reading_label, LV_ALIGN_TOP_RIGHT, -4, 4);
    }

    if (shown_reading == READING_ERROR) {
        snprintf(text, sizeof(text), "luz ERR %s", shown_error);
    } else if (shown_reading == READING_NONE) {
        snprintf(text, sizeof(text), "luz --");
    } else {
        snprintf(text, sizeof(text), "luz %d  %d%%", shown_reading, shown_brightness);
    }
    lv_label_set_text(reading_label, text);
}

static K_WORK_DEFINE(show_reading_work, show_reading_cb);

static void show_reading(int32_t reading, uint8_t brightness) {
    shown_reading = reading;
    shown_brightness = brightness;
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &show_reading_work);
    }
}

/*
 * El driver no dice por que no arranca y el dongle va sin logging. Lee a mano
 * el registro de ID: si contesta, el chip es otro (los clones baratos llevan
 * IDs que el driver no acepta, solo 0xAB y 0x9C); si no, busca cualquier cosa
 * en el bus por si esta en otra direccion. "i2c nada" = cableado/alimentacion.
 */
static void show_init_error(void) {
    const struct device *bus = DEVICE_DT_GET(DT_BUS(DT_NODELABEL(apds9960)));
    uint8_t id;

    if (i2c_reg_read_byte(bus, DT_REG_ADDR(DT_NODELABEL(apds9960)), 0x92, &id) == 0) {
        snprintf(shown_error, sizeof(shown_error), "id %02X", id);
    } else {
        snprintf(shown_error, sizeof(shown_error), "i2c nada");
        for (uint16_t addr = 0x08; addr < 0x78; addr++) {
            if (i2c_reg_read_byte(bus, addr, 0, &id) == 0) {
                snprintf(shown_error, sizeof(shown_error), "i2c %02X", addr);
                break;
            }
        }
    }

    show_reading(READING_ERROR, 0);
}

#else

#define READING_NONE 0
static inline void show_reading(int32_t reading, uint8_t brightness) {}
static inline void show_init_error(void) {}

#endif

static void fade_to_target(void) {
    for (;;) {
        k_mutex_lock(&bl_lock, K_FOREVER);
        if (!active || current == target) {
            k_mutex_unlock(&bl_lock);
            return;
        }
        set_backlight(current < target ? current + 1 : current - 1);
        k_mutex_unlock(&bl_lock);
        k_msleep(FADE_STEP_MS);
    }
}

static void als_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (!device_is_ready(als_dev)) {
        LOG_ERR("El sensor de luz APDS9960 no esta listo; brillo fijo");
        show_init_error();
        return;
    }

    show_reading(READING_NONE, 0);

    /* Media movil del brillo, x16, para que un cambio de luz entre poco a poco. */
    int32_t avg16 = -1;

    for (;;) {
        k_msleep(SAMPLE_MS);

        struct sensor_value light;
        if (sensor_sample_fetch(als_dev) ||
            sensor_channel_get(als_dev, SENSOR_CHAN_LIGHT, &light)) {
            LOG_ERR("No se pudo leer el sensor de luz");
            continue;
        }

        int32_t wanted16 = reading_to_brightness(light.val1) * 16;
        avg16 = avg16 < 0 ? wanted16 : avg16 + (wanted16 - avg16) / 4;
        uint8_t wanted = (avg16 + 8) / 16;

        show_reading(light.val1, wanted);

        k_mutex_lock(&bl_lock, K_FOREVER);
        if (abs(wanted - target) >= HYSTERESIS || wanted == CONFIG_DONGLE_ALS_MIN_BRIGHTNESS ||
            wanted == CONFIG_DONGLE_ALS_MAX_BRIGHTNESS) {
            target = wanted;
        }
        k_mutex_unlock(&bl_lock);

        fade_to_target();
    }
}

/* Arranca tarde para que la pantalla (y su cola de trabajo) ya este en marcha. */
K_THREAD_DEFINE(display_als_tid, 1024, als_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 2000);

static int display_als_activity_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Al volver se enciende de golpe al brillo que toca, sin fundido. */
    k_mutex_lock(&bl_lock, K_FOREVER);
    active = ev->state == ZMK_ACTIVITY_ACTIVE;
    set_backlight(active ? target : 0);
    k_mutex_unlock(&bl_lock);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(display_als_backlight, display_als_activity_listener);
ZMK_SUBSCRIPTION(display_als_backlight, zmk_activity_state_changed);
