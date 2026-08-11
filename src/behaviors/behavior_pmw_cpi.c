/*
 * Copyright (c) 2026
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_pmw_cpi

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/input/input_pmw3610.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/pmw_cpi.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PMW_CPI_MIN 200
#define PMW_CPI_MAX 3200

#if DT_NODE_EXISTS(DT_NODELABEL(trackball))
#define TRACKBALL_NODE DT_NODELABEL(trackball)
#define TRACKBALL_DEFAULT_CPI DT_PROP_OR(TRACKBALL_NODE, res_cpi, 1200)
#endif

struct behavior_pmw_cpi_data {
    uint16_t cpi;
};

static struct behavior_pmw_cpi_data behavior_pmw_cpi_data = {
#if DT_NODE_EXISTS(DT_NODELABEL(trackball))
    .cpi = TRACKBALL_DEFAULT_CPI,
#else
    .cpi = 1200,
#endif
};

static uint16_t clamp_cpi(int32_t cpi) {
    if (cpi < PMW_CPI_MIN) {
        return PMW_CPI_MIN;
    }

    if (cpi > PMW_CPI_MAX) {
        return PMW_CPI_MAX;
    }

    return (uint16_t)(cpi - (cpi % 200));
}

static int on_pmw_cpi_binding_pressed(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
#if DT_NODE_EXISTS(DT_NODELABEL(trackball))
    const struct device *trackball = DEVICE_DT_GET(TRACKBALL_NODE);

    if (!device_is_ready(trackball)) {
        return -ENODEV;
    }

    struct behavior_pmw_cpi_data *data = &behavior_pmw_cpi_data;
    const int32_t command = binding->param1;
    const int32_t value = binding->param2;
    uint16_t next_cpi = data->cpi;

    switch (command) {
    case PMW_CPI_SET:
        next_cpi = clamp_cpi(value);
        break;
    case PMW_CPI_INC:
        next_cpi = clamp_cpi((int32_t)data->cpi + value);
        break;
    case PMW_CPI_DEC:
        next_cpi = clamp_cpi((int32_t)data->cpi - value);
        break;
    default:
        return -EINVAL;
    }

    int ret = pmw3610_set_resolution(trackball, next_cpi);
    if (ret < 0) {
        return ret;
    }

    data->cpi = next_cpi;
    return ZMK_BEHAVIOR_OPAQUE;
#else
    return -ENODEV;
#endif
}

static int on_pmw_cpi_binding_released(struct zmk_behavior_binding *binding,
                                       struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_pmw_cpi_driver_api = {
    .binding_pressed = on_pmw_cpi_binding_pressed,
    .binding_released = on_pmw_cpi_binding_released,
    .locality = BEHAVIOR_LOCALITY_EVENT_SOURCE,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, &behavior_pmw_cpi_data, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_pmw_cpi_driver_api);



