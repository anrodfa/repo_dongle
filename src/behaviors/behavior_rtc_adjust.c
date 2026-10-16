/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * &rtc_adj <comando>: pone en hora el reloj del dongle desde el keymap (capa
 * "Ajustes"). Los comandos estan en dt-bindings/zmk/rtc_adjust.h; el trabajo
 * lo hace dongle_rtc_adjust() en src/dongle_rtc.c.
 *
 * Locality CENTRAL (la de por defecto): el DS3231 esta en el dongle.
 */

#define DT_DRV_COMPAT zmk_behavior_rtc_adjust

#include <zephyr/device.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

int dongle_rtc_adjust(int command);

static int on_rtc_adjust_binding_pressed(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    int ret = dongle_rtc_adjust(binding->param1);
    return ret < 0 ? ret : ZMK_BEHAVIOR_OPAQUE;
}

static int on_rtc_adjust_binding_released(struct zmk_behavior_binding *binding,
                                          struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_rtc_adjust_driver_api = {
    .binding_pressed = on_rtc_adjust_binding_pressed,
    .binding_released = on_rtc_adjust_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_rtc_adjust_driver_api);
