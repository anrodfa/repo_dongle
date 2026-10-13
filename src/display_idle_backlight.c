/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Apaga la retroiluminacion de la pantalla Prospector del dongle cuando ZMK
 * pasa a inactivo y la vuelve a encender con la siguiente actividad.
 *
 * El modulo Prospector, con brillo fijo, solo pone el backlight una vez en
 * SYS_INIT y nunca lo apaga. ZMK_DISPLAY_BLANK_ON_IDLE no sirve aqui: solo
 * viene activo por defecto con SSD1306 y, aunque se active, manda DISPOFF al
 * panel pero deja el backlight encendido.
 *
 * El dongle va siempre enchufado y el USB del PC suele seguir dando corriente
 * al hibernar, asi que "teclado apagado" y "PC hibernado" se ven igual: dejan
 * de llegar eventos y, pasado CONFIG_ZMK_IDLE_TIMEOUT, ZMK pasa a IDLE. Las
 * teclas y el movimiento del trackball reenviado por la derecha cuentan como
 * actividad (activity.c escucha posiciones y cualquier evento de input).
 *
 * Solo se compila con prospector_adapter y brillo fijo (ver CMakeLists.txt):
 * con el sensor de luz, su hilo reescribiria el brillo cada 100 ms.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* Mismo dispositivo y canal que usa brightness.c del modulo. */
static const struct device *pwm_leds_dev = DEVICE_DT_GET_ONE(pwm_leds);
#define DISP_BL DT_NODE_CHILD_IDX(DT_NODELABEL(disp_bl))

static int display_idle_backlight_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    uint8_t brightness =
        ev->state == ZMK_ACTIVITY_ACTIVE ? CONFIG_PROSPECTOR_FIXED_BRIGHTNESS : 0;

    if (led_set_brightness(pwm_leds_dev, DISP_BL, brightness)) {
        LOG_ERR("No se pudo poner el backlight a %d", brightness);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(display_idle_backlight, display_idle_backlight_listener);
ZMK_SUBSCRIPTION(display_idle_backlight, zmk_activity_state_changed);
