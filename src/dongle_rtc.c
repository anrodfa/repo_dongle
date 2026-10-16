/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Reloj del dongle: modulo DS3231 en i2c0 (D2/D3, en paralelo con el APDS9960),
 * direccion 0x68.
 *
 * - Con actividad: la pantalla normal de capas y la hora pequena arriba, en el
 *   centro (por los lados la pantalla es curva y se corta).
 * - En IDLE (CONFIG_ZMK_IDLE_TIMEOUT, 30 s): una capa negra encima de todo con
 *   la hora grande y la fecha. El backlight se queda encendido (ver
 *   display_als_backlight.c). La siguiente tecla o movimiento la quita.
 *
 * El DS3231 guarda UTC. La hora local es CONFIG_DONGLE_RTC_UTC_OFFSET_MIN mas
 * el horario de verano de la UE (del ultimo domingo de marzo al ultimo de
 * octubre, a la 01:00 UTC), asi que no hay que tocarlo dos veces al ano.
 *
 * Puesta en hora: si el bit OSF dice que el oscilador se ha parado (primera vez
 * o sin pila), se arranca con la hora de compilacion (los runners de GitHub van
 * en UTC; queda unos minutos atrasada) y se afina con &rtc_adj desde el keymap.
 *
 * No se usa el driver de Zephyr (maxim,ds3231): arrastra el subsistema RTC/
 * counter entero y aqui bastan unos registros. Se lee a mano por I2C, como el
 * sensor de luz.
 */

#include <stdio.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>
#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include <dt-bindings/zmk/rtc_adjust.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Fuentes del layout classic del modulo: ya estan en flash, no cuestan nada. */
LV_FONT_DECLARE(FR_Regular_48);
LV_FONT_DECLARE(FG_Medium_20);
LV_FONT_DECLARE(FG_Medium_24);

#define DS3231_ADDR 0x68

/* Registros del DS3231 (datasheet, "Timekeeping Registers"), todos en BCD. */
#define REG_SECONDS 0x00
#define REG_CONTROL 0x0E
/* A 1 para el oscilador cuando va con la pila. Se deja siempre a 0. */
#define CONTROL_EOSC BIT(7)
#define REG_STATUS 0x0F
#define STATUS_OSF BIT(7)

static const struct i2c_dt_spec rtc_i2c = {
    .bus = DEVICE_DT_GET(DT_NODELABEL(i2c0)),
    .addr = DS3231_ADDR,
};

/* Protege el DS3231 (lectura del hilo frente a ajustes del keymap) y los textos. */
static K_MUTEX_DEFINE(rtc_lock);

/* ---------------------------------------------------------------------------
 * Fechas: dias desde 1970-01-01 <-> ano/mes/dia (algoritmo de H. Hinnant).
 * ------------------------------------------------------------------------- */

struct civil {
    int year, month, day, hour, min, sec;
};

static int32_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const int32_t yoe = y - era * 400;
    const int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int32_t z, struct civil *c) {
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int32_t doe = z - era * 146097;
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int32_t mp = (5 * doy + 2) / 153;
    c->day = doy - (153 * mp + 2) / 5 + 1;
    c->month = mp < 10 ? mp + 3 : mp - 9;
    c->year = yoe + era * 400 + (c->month <= 2);
}

/* 0 = domingo. El 1970-01-01 fue jueves. */
static int weekday(int32_t days) { return (days % 7 + 11) % 7; }

static int64_t to_epoch(const struct civil *c) {
    return (int64_t)days_from_civil(c->year, c->month, c->day) * 86400 + c->hour * 3600 +
           c->min * 60 + c->sec;
}

static void from_epoch(int64_t t, struct civil *c) {
    int32_t days = t / 86400;
    int32_t rem = t % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    civil_from_days(days, c);
    c->hour = rem / 3600;
    c->min = rem / 60 % 60;
    c->sec = rem % 60;
}

/* Ultimo domingo del mes (de 31 dias) a la 01:00 UTC. */
static int64_t eu_switch(int year, int month) {
    int32_t last = days_from_civil(year, month, 31);
    return (int64_t)(last - weekday(last)) * 86400 + 3600;
}

static int64_t utc_to_local(int64_t utc) {
    int64_t local = utc + CONFIG_DONGLE_RTC_UTC_OFFSET_MIN * 60;
#if IS_ENABLED(CONFIG_DONGLE_RTC_EU_DST)
    struct civil c;
    from_epoch(utc, &c);
    if (utc >= eu_switch(c.year, 3) && utc < eu_switch(c.year, 10)) {
        local += 3600;
    }
#endif
    return local;
}

/* ---------------------------------------------------------------------------
 * DS3231
 * ------------------------------------------------------------------------- */

/* bcd2bin() y bin2bcd() vienen de zephyr/sys/util.h. */

/* Llamar con rtc_lock cogido. */
static int rtc_read(int64_t *utc) {
    /* 0x00-0x06: segundos, minutos, horas, dia de la semana, dia, mes, ano. */
    uint8_t t[7];
    if (i2c_burst_read_dt(&rtc_i2c, REG_SECONDS, t, sizeof(t))) {
        return -EIO;
    }
    struct civil c = {
        .year = 2000 + bcd2bin(t[6]),
        .month = bcd2bin(t[5] & 0x1F),
        .day = bcd2bin(t[4]),
        /* Modo 24 h (bit 6 a 0, lo pone rtc_write): decenas en los bits 5:4. */
        .hour = bcd2bin(t[2] & 0x3F),
        .min = bcd2bin(t[1]),
        .sec = bcd2bin(t[0] & 0x7F),
    };
    *utc = to_epoch(&c);
    return 0;
}

/*
 * Llamar con rtc_lock cogido. Escribir los segundos reinicia el divisor del
 * DS3231, asi que el segundo empieza justo ahora. Tambien borra OSF.
 */
static int rtc_write(int64_t utc) {
    struct civil c;
    from_epoch(utc, &c);
    if (c.year < 2000 || c.year > 2099) {
        return -EINVAL;
    }

    uint8_t buf[8] = {
        REG_SECONDS,
        bin2bcd(c.sec),
        bin2bcd(c.min),
        bin2bcd(c.hour),
        /* 1-7; el DS3231 no le da significado, se usa 1 = domingo. */
        weekday(utc / 86400) + 1,
        bin2bcd(c.day),
        bin2bcd(c.month),
        bin2bcd(c.year - 2000),
    };
    if (i2c_write_dt(&rtc_i2c, buf, sizeof(buf))) {
        return -EIO;
    }

    uint8_t status;
    if (i2c_reg_read_byte_dt(&rtc_i2c, REG_STATUS, &status) ||
        i2c_reg_write_byte_dt(&rtc_i2c, REG_STATUS, status & ~STATUS_OSF)) {
        return -EIO;
    }
    return 0;
}

/* __DATE__ es "Oct 16 2026" y __TIME__ "19:16:04". */
static int64_t build_time(void) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__;
    const char *time = __TIME__;
    char mon[4] = {date[0], date[1], date[2], 0};
    struct civil c = {
        .year = (date[7] - '0') * 1000 + (date[8] - '0') * 100 + (date[9] - '0') * 10 +
                (date[10] - '0'),
        .month = (strstr(months, mon) - months) / 3 + 1,
        .day = (date[4] == ' ' ? 0 : date[4] - '0') * 10 + (date[5] - '0'),
        .hour = (time[0] - '0') * 10 + (time[1] - '0'),
        .min = (time[3] - '0') * 10 + (time[4] - '0'),
        .sec = (time[6] - '0') * 10 + (time[7] - '0'),
    };
    return to_epoch(&c);
}

/* Si el oscilador se paro, la hora no vale: se pone la de compilacion. */
static int rtc_init(void) {
    uint8_t control;
    uint8_t status;

    k_mutex_lock(&rtc_lock, K_FOREVER);
    int ret = i2c_reg_read_byte_dt(&rtc_i2c, REG_CONTROL, &control) ||
              i2c_reg_read_byte_dt(&rtc_i2c, REG_STATUS, &status);
    if (!ret && (control & CONTROL_EOSC)) {
        ret = i2c_reg_write_byte_dt(&rtc_i2c, REG_CONTROL, control & ~CONTROL_EOSC);
    }
    if (!ret && (status & STATUS_OSF)) {
        LOG_WRN("El reloj perdio la hora; se pone la de compilacion");
        ret = rtc_write(build_time());
    }
    k_mutex_unlock(&rtc_lock);

    return ret ? -EIO : 0;
}

/* ---------------------------------------------------------------------------
 * Pantalla
 * ------------------------------------------------------------------------- */

static const char *const weekdays[] = {"dom", "lun", "mar", "mie", "jue", "vie", "sab"};
static const char *const month_names[] = {"ene", "feb", "mar", "abr", "may", "jun",
                                          "jul", "ago", "sep", "oct", "nov", "dic"};

/* Textos que pinta la cola de la pantalla; se escriben con rtc_lock cogido. */
static char time_text[8] = "--:--";
static char date_text[16] = "";
static bool idle;

static lv_obj_t *small_time;
static lv_obj_t *big_panel;
static lv_obj_t *big_time;
static lv_obj_t *big_date;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static void create_widgets(void) {
    /* Hora pequena: arriba en el centro, encima del rodillo de capas. */
    small_time = make_label(lv_layer_top(), &FG_Medium_20);
    lv_obj_align(small_time, LV_ALIGN_TOP_MID, 0, 6);

    /* Modo reloj: capa negra a pantalla completa, por encima de todo. */
    big_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(big_panel);
    lv_obj_set_size(big_panel, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(big_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(big_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(big_panel, LV_OBJ_FLAG_HIDDEN);

    big_time = make_label(big_panel, &FR_Regular_48);
    lv_obj_align(big_time, LV_ALIGN_CENTER, 0, -18);

    big_date = make_label(big_panel, &FG_Medium_24);
    lv_obj_align(big_date, LV_ALIGN_CENTER, 0, 30);
}

/* Corre en la cola de trabajo de la pantalla: es la unica que puede tocar LVGL. */
static void refresh_cb(struct k_work *work) {
    if (small_time == NULL) {
        create_widgets();
    }

    k_mutex_lock(&rtc_lock, K_FOREVER);
    lv_label_set_text(small_time, time_text);
    lv_label_set_text(big_time, time_text);
    lv_label_set_text(big_date, date_text);
    bool show_big = idle;
    k_mutex_unlock(&rtc_lock);

    if (show_big) {
        lv_obj_add_flag(small_time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(big_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(big_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(small_time, LV_OBJ_FLAG_HIDDEN);
    }
}

static K_WORK_DEFINE(refresh_work, refresh_cb);

static void refresh(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &refresh_work);
    }
}

/* Lee el DS3231 y rehace los textos. Devuelve true si han cambiado. */
static bool update_texts(void) {
    char t[sizeof(time_text)] = "--:--";
    char d[sizeof(date_text)] = "";

    k_mutex_lock(&rtc_lock, K_FOREVER);
    int64_t utc;
    if (rtc_read(&utc) == 0) {
        int64_t local = utc_to_local(utc);
        struct civil c;
        from_epoch(local, &c);
        snprintf(t, sizeof(t), "%02d:%02d", c.hour, c.min);
        snprintf(d, sizeof(d), "%s %d %s", weekdays[weekday(local / 86400)], c.day,
                 month_names[(c.month - 1) % 12]);
    }
    bool changed = strcmp(t, time_text) || strcmp(d, date_text);
    strcpy(time_text, t);
    strcpy(date_text, d);
    k_mutex_unlock(&rtc_lock);

    return changed;
}

static void rtc_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    if (!i2c_is_ready_dt(&rtc_i2c) || rtc_init()) {
        LOG_ERR("El reloj DS3231 no responde por I2C");
    }

    idle = zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE;
    update_texts();
    refresh();

    for (;;) {
        k_msleep(1000);
        if (update_texts()) {
            refresh();
        }
    }
}

/* Arranca tarde para que la pantalla (y su cola de trabajo) ya este en marcha. */
K_THREAD_DEFINE(dongle_rtc_tid, 1024, rtc_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 2000);

static int dongle_rtc_activity_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    k_mutex_lock(&rtc_lock, K_FOREVER);
    idle = ev->state != ZMK_ACTIVITY_ACTIVE;
    k_mutex_unlock(&rtc_lock);
    refresh();

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(dongle_rtc, dongle_rtc_activity_listener);
ZMK_SUBSCRIPTION(dongle_rtc, zmk_activity_state_changed);

/* ---------------------------------------------------------------------------
 * Ajuste desde el keymap (&rtc_adj, src/behaviors/behavior_rtc_adjust.c)
 * ------------------------------------------------------------------------- */

int dongle_rtc_adjust(int command) {
    int64_t utc;

    k_mutex_lock(&rtc_lock, K_FOREVER);
    int ret = rtc_read(&utc);
    if (ret == 0) {
        switch (command) {
        case RTC_HOUR_UP:
            utc += 3600;
            break;
        case RTC_HOUR_DN:
            utc -= 3600;
            break;
        case RTC_MIN_UP:
            utc += 60;
            break;
        case RTC_MIN_DN:
            utc -= 60;
            break;
        case RTC_SEC_ZERO: {
            /* Al minuto mas cercano: vale pulsar un pelin antes o despues del :00. */
            int sec = ((utc % 60) + 60) % 60;
            utc += sec < 30 ? -sec : 60 - sec;
            break;
        }
        default:
            ret = -EINVAL;
            break;
        }
    }
    if (ret == 0) {
        ret = rtc_write(utc);
    }
    k_mutex_unlock(&rtc_lock);

    if (ret == 0 && update_texts()) {
        refresh();
    }
    return ret;
}
