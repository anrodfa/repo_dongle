/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Reloj del dongle: modulo DS3231 en i2c0 (D2/D3, en paralelo con el APDS9960),
 * direccion 0x68.
 *
 * De momento solo es la prueba del cableado: con CONFIG_DONGLE_RTC_SHOW_RAW
 * lee los registros cada segundo y los saca arriba en la pantalla tal cual
 * estan, sin zona horaria ni horario de verano. Si los segundos avanzan, el
 * modulo esta bien soldado. "osf1" quiere decir que el oscilador se ha parado
 * en algun momento (normal la primera vez, o sin pila): la hora no vale.
 *
 * No se usa el driver de Zephyr (maxim,ds3231): arrastra el subsistema RTC/
 * counter entero y aqui bastan unos registros. Se lee a mano por I2C, como el
 * sensor de luz (display_als_backlight.c).
 */

#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <zmk/display.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DS3231_ADDR 0x68

/* Registros del DS3231 (datasheet, "Timekeeping Registers"), todos en BCD. */
#define REG_SECONDS 0x00
#define REG_STATUS 0x0F
#define STATUS_OSF BIT(7)

static const struct i2c_dt_spec rtc_i2c = {
    .bus = DEVICE_DT_GET(DT_NODELABEL(i2c0)),
    .addr = DS3231_ADDR,
};

static lv_obj_t *raw_label;
static char shown_text[64] = "rtc --";

/* Corre en la cola de trabajo de la pantalla: es la unica que puede tocar LVGL. */
static void show_text_cb(struct k_work *work) {
    if (raw_label == NULL) {
        raw_label = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(raw_label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(raw_label, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(raw_label, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(raw_label, 2, 0);
        /* Centrada: la pantalla es curva y por los lados se corta. */
        lv_obj_align(raw_label, LV_ALIGN_TOP_MID, 0, 4);
    }
    lv_label_set_text(raw_label, shown_text);
}

static K_WORK_DEFINE(show_text_work, show_text_cb);

#define show_text(...)                                                                             \
    do {                                                                                           \
        snprintf(shown_text, sizeof(shown_text), __VA_ARGS__);                                     \
        if (zmk_display_is_initialized()) {                                                        \
            k_work_submit_to_queue(zmk_display_work_q(), &show_text_work);                         \
        }                                                                                          \
    } while (0)

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static void rtc_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (!i2c_is_ready_dt(&rtc_i2c)) {
        show_text("rtc ERR bus");
        return;
    }

    for (;;) {
        /* 0x00-0x06: segundos, minutos, horas, dia semana, dia, mes, ano. */
        uint8_t t[7];
        uint8_t status;

        if (i2c_burst_read_dt(&rtc_i2c, REG_SECONDS, t, sizeof(t)) ||
            i2c_reg_read_byte_dt(&rtc_i2c, REG_STATUS, &status)) {
            show_text("rtc ERR i2c");
        } else {
            /* Horas en modo 24 h (bit 6 a 0): decenas en los bits 5:4. */
            show_text("rtc %02d-%02d-%02d %02d:%02d:%02d osf%d", bcd(t[6]), bcd(t[5] & 0x1F),
                      bcd(t[4]), bcd(t[2] & 0x3F), bcd(t[1]), bcd(t[0] & 0x7F),
                      (status & STATUS_OSF) ? 1 : 0);
        }

        k_msleep(1000);
    }
}

/* Arranca tarde para que la pantalla (y su cola de trabajo) ya este en marcha. */
K_THREAD_DEFINE(dongle_rtc_tid, 1024, rtc_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 2000);
