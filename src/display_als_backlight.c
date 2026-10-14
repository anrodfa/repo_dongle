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
 * Tampoco se usa el driver de Zephyr: solo acepta los ID 0xAB y 0x9C, y el
 * modulo que hay montado es un clon que da otro (0x9E, leido en la pantalla).
 * Para la luz ambiente bastan cuatro registros, asi que se leen a mano por
 * I2C, sin la linea INT, y da igual el ID.
 *
 * La lectura cruda (canal "clear") se pasa a brillo en escala logaritmica
 * entre CONFIG_DONGLE_ALS_MIN_READING y _MAX_READING: el ojo percibe la luz
 * asi, y entre una lampara y luz de dia hay un factor 10-100. Para calibrar,
 * CONFIG_DONGLE_ALS_SHOW_READING muestra la lectura y el brillo en una esquina.
 *
 * Si el sensor no responde, la pantalla se queda en
 * CONFIG_PROSPECTOR_FIXED_BRIGHTNESS (lo que pone el modulo al arrancar) y se
 * sigue apagando en reposo.
 */

#include <stdio.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#if IS_ENABLED(CONFIG_DONGLE_ALS_SHOW_READING)
#include <lvgl.h>
#include <zmk/display.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define SAMPLE_MS 200
#define FADE_STEP_MS 8
/* Diferencia minima de brillo (%) para cambiarlo: evita que tiemble. */
#define HYSTERESIS 2
/* Lecturas seguidas sin dato valido antes de dar el sensor por perdido. */
#define MAX_MISSES 10

/* Registros del APDS9960 (datasheet, seccion "Register Set"). */
#define REG_ENABLE 0x80
#define ENABLE_PON BIT(0)
#define ENABLE_AEN BIT(1)
#define REG_ATIME 0x81
/* 256 - 219 = 37 ciclos de 2,78 ms: ~103 ms por lectura, maximo 37.888. */
#define ATIME_VALUE 219
#define REG_CONTROL 0x8F
/* AGAIN en los bits 1:0: 0 = 1x, 1 = 4x, 2 = 16x, 3 = 64x. */
#define CONTROL_AGAIN_4X 0x01
#define REG_ID 0x92
#define REG_STATUS 0x93
#define STATUS_AVALID BIT(0)
#define REG_CDATAL 0x94

/* Mismo dispositivo y canal que usa brightness.c del modulo. */
static const struct device *pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);
#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

static const struct i2c_dt_spec als_i2c = I2C_DT_SPEC_GET(DT_NODELABEL(apds9960));

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

static lv_obj_t *reading_label;
static char shown_text[160] = "luz --";

/* Corre en la cola de trabajo de la pantalla: es la unica que puede tocar LVGL. */
static void show_text_cb(struct k_work *work) {
    if (reading_label == NULL) {
        reading_label = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(reading_label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(reading_label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(reading_label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(reading_label, 2, 0);
        lv_obj_set_style_text_align(reading_label, LV_TEXT_ALIGN_CENTER, 0);
        /* Centrada: la pantalla es curva y por los lados se corta. */
        lv_obj_align(reading_label, LV_ALIGN_TOP_MID, 0, 4);
    }
    lv_label_set_text(reading_label, shown_text);
}

static K_WORK_DEFINE(show_text_work, show_text_cb);

#define show_text(...)                                                                             \
    do {                                                                                           \
        snprintf(shown_text, sizeof(shown_text), __VA_ARGS__);                                     \
        if (zmk_display_is_initialized()) {                                                        \
            k_work_submit_to_queue(zmk_display_work_q(), &show_text_work);                         \
        }                                                                                          \
    } while (0)

#else

#define show_text(...)

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

/* Enciende solo el ALS: sin proximidad, gestos ni interrupciones. */
static int als_setup(uint8_t *id) {
    if (!i2c_is_ready_dt(&als_i2c) || i2c_reg_read_byte_dt(&als_i2c, REG_ID, id)) {
        return -EIO;
    }
    if (i2c_reg_write_byte_dt(&als_i2c, REG_ENABLE, 0) ||
        i2c_reg_write_byte_dt(&als_i2c, REG_ATIME, ATIME_VALUE) ||
        i2c_reg_write_byte_dt(&als_i2c, REG_CONTROL, CONTROL_AGAIN_4X) ||
        i2c_reg_write_byte_dt(&als_i2c, REG_ENABLE, ENABLE_PON | ENABLE_AEN)) {
        return -EIO;
    }
    return 0;
}

/* 1 = lectura nueva en *clear, 0 = aun no hay dato, < 0 = error de bus. */
static int als_read(uint16_t *clear) {
    uint8_t status;
    uint8_t buf[2];

    if (i2c_reg_read_byte_dt(&als_i2c, REG_STATUS, &status)) {
        return -EIO;
    }
    if (!(status & STATUS_AVALID)) {
        return 0;
    }
    if (i2c_burst_read_dt(&als_i2c, REG_CDATAL, buf, sizeof(buf))) {
        return -EIO;
    }
    *clear = sys_get_le16(buf);
    return 1;
}

/*
 * Diagnostico temporal para el clon (ID 0x9E): las escrituras no dan error
 * pero ENABLE y ATIME se leen con los valores de reset. Muestra, en lineas
 * cortas y centradas (la pantalla es curva):
 *   id, resultado de escribir ATIME (w) y lo que se lee despues (r)
 *   direcciones que contestan en el bus
 *   registros 0x80-0x97 leidos de uno en uno, 4 por linea
 */
static void show_diag(uint8_t id) {
    char text[160];
    int len = 0;
    uint8_t v = 0;

    int w = i2c_reg_write_byte_dt(&als_i2c, REG_ATIME, ATIME_VALUE);
    int r = i2c_reg_read_byte_dt(&als_i2c, REG_ATIME, &v);
    len += snprintf(text + len, sizeof(text) - len, "id%02X w%d r%d=%02X\ni2c", id, w, r, v);

    int found = 0;
    for (uint16_t addr = 0x08; addr < 0x78 && found < 4; addr++) {
        if (i2c_reg_read_byte(als_i2c.bus, addr, 0x92, &v) == 0) {
            len += snprintf(text + len, sizeof(text) - len, " %02X", addr);
            found++;
        }
    }

    for (uint8_t reg = 0x80; reg < 0x98; reg++) {
        if (i2c_reg_read_byte_dt(&als_i2c, reg, &v)) {
            v = 0xEE;
        }
        if ((reg & 3) == 0) {
            len += snprintf(text + len, sizeof(text) - len, "\n%02X:", reg);
        }
        len += snprintf(text + len, sizeof(text) - len, "%02X", v);
    }

    show_text("%s", text);
}

static void als_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    uint8_t id = 0;
    if (als_setup(&id)) {
        LOG_ERR("El sensor de luz no responde por I2C; brillo fijo");
        show_text("luz ERR i2c");
        return;
    }

    show_text("luz -- id %02X", id);

    /* Media movil del brillo, x16, para que un cambio de luz entre poco a poco. */
    int32_t avg16 = -1;
    int misses = 0;

    for (;;) {
        k_msleep(SAMPLE_MS);

        uint16_t clear;
        int ret = als_read(&clear);
        if (ret <= 0) {
            if (++misses >= MAX_MISSES) {
                if (ret < 0) {
                    show_text("luz ERR i2c id %02X", id);
                } else if (misses == MAX_MISSES) {
                    show_diag(id);
                }
            }
            continue;
        }
        misses = 0;

        int32_t wanted16 = reading_to_brightness(clear) * 16;
        avg16 = avg16 < 0 ? wanted16 : avg16 + (wanted16 - avg16) / 4;
        uint8_t wanted = (avg16 + 8) / 16;

        show_text("luz %d  %d%%", clear, wanted);

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
