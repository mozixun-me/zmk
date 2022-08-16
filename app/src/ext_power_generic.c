/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_ext_power_generic

#include <stdio.h>
#include <device.h>
#include <pm/device.h>
#include <init.h>
#include <kernel.h>
#include <settings/settings.h>
#include <drivers/gpio.h>
#include <drivers/ext_power.h>
#include <drivers/display.h>

#define ZMK_DISPLAY_NAME CONFIG_LVGL_DISPLAY_DEV_NAME

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#include <logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct ext_power_generic_config {
    const char *label;
    const uint8_t pin;
    const uint8_t flags;
    const uint16_t init_delay_ms;
};

struct ext_power_generic_data {
    const struct device *gpio;
    bool status;
#if IS_ENABLED(CONFIG_SETTINGS)
    bool settings_init;
#endif
};

#if IS_ENABLED(CONFIG_SETTINGS)
static void ext_power_save_state_work(struct k_work *work) {
    char setting_path[40];
    const struct device *ext_power = device_get_binding(DT_INST_LABEL(0));
    struct ext_power_generic_data *data = ext_power->data;

    snprintf(setting_path, 40, "ext_power/state/%s", DT_INST_LABEL(0));
    settings_save_one(setting_path, &data->status, sizeof(data->status));
}

static struct k_work_delayable ext_power_save_work;
#endif

int ext_power_save_state() {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&ext_power_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

static void drivers_update_power_state(bool power) {
    LOG_DBG("drivers_update_power_state: %s", power?"true":"false");
    static const struct device *display;
    display = device_get_binding(ZMK_DISPLAY_NAME);

    if (display != NULL) {
        display_update_ext_power(display, power);
    }
}

static int ext_power_generic_enable(const struct device *dev) {
    struct ext_power_generic_data *data = dev->data;
    const struct ext_power_generic_config *config = dev->config;

    if (gpio_pin_set(data->gpio, config->pin, 1)) {
        LOG_WRN("Failed to set ext-power control pin");
        return -EIO;
    }
    data->status = true;
    drivers_update_power_state(true);
    return ext_power_save_state();
}

static int ext_power_generic_disable(const struct device *dev) {
    struct ext_power_generic_data *data = dev->data;
    const struct ext_power_generic_config *config = dev->config;

    if (gpio_pin_set(data->gpio, config->pin, 0)) {
        LOG_WRN("Failed to clear ext-power control pin");
        return -EIO;
    }
    data->status = false;

    drivers_update_power_state(false);
    return ext_power_save_state();
}

static int ext_power_generic_get(const struct device *dev) {
    struct ext_power_generic_data *data = dev->data;
    return data->status;
}

#if IS_ENABLED(CONFIG_SETTINGS)
static int ext_power_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, DT_INST_LABEL(0), &next) && !next) {
        const struct device *ext_power = DEVICE_DT_GET(DT_DRV_INST(0));
        struct ext_power_generic_data *data = ext_power->data;

        if (len != sizeof(data->status)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &data->status, sizeof(data->status));
        if (rc >= 0) {
            data->settings_init = true;

            if (ext_power == NULL) {
                LOG_ERR("Unable to retrieve ext_power device: %s", DT_INST_LABEL(0));
                return -EIO;
            }

            if (data->status) {
                ext_power_generic_enable(ext_power);
            } else {
                ext_power_generic_disable(ext_power);
            }

            return 0;
        }
        return rc;
    }
    return -ENOENT;
}

struct settings_handler ext_power_conf = {.name = "ext_power/state",
                                          .h_set = ext_power_settings_set};
#endif

static int ext_power_generic_init(const struct device *dev) {
    struct ext_power_generic_data *data = dev->data;
    const struct ext_power_generic_config *config = dev->config;

    data->gpio = device_get_binding(config->label);
    if (data->gpio == NULL) {
        LOG_ERR("Failed to get ext-power control device");
        return -EINVAL;
    }

    if (gpio_pin_configure(data->gpio, config->pin, config->flags | GPIO_OUTPUT)) {
        LOG_ERR("Failed to configure ext-power control pin");
        return -EIO;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();

    int err = settings_register(&ext_power_conf);
    if (err) {
        LOG_ERR("Failed to register the ext_power settings handler (err %d)", err);
        return err;
    }

    k_work_init_delayable(&ext_power_save_work, ext_power_save_state_work);

    // Set default value (on) if settings isn't set
    settings_load_subtree("ext_power");
    if (!data->settings_init) {

        data->status = true;
        k_work_schedule(&ext_power_save_work, K_NO_WAIT);

        ext_power_enable(dev);
    }
#else
    // Default to the ext_power being open when no settings
    ext_power_enable(dev);
#endif

    if (config->init_delay_ms) {
        k_msleep(config->init_delay_ms);
    }

    return 0;
}

#ifdef CONFIG_PM_DEVICE
static int ext_power_generic_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_RESUME:
        ext_power_generic_enable(dev);
        return 0;
    case PM_DEVICE_ACTION_SUSPEND:
        ext_power_generic_disable(dev);
        return 0;
    default:
        return -ENOTSUP;
    }
}
#endif /* CONFIG_PM_DEVICE */

static const struct ext_power_generic_config config = {
    .label = DT_INST_GPIO_LABEL(0, control_gpios),
    .pin = DT_INST_GPIO_PIN(0, control_gpios),
    .flags = DT_INST_GPIO_FLAGS(0, control_gpios),
    .init_delay_ms = DT_INST_PROP_OR(0, init_delay_ms, 0)};

static struct ext_power_generic_data data = {
    .status = false,
#if IS_ENABLED(CONFIG_SETTINGS)
    .settings_init = false,
#endif
};

static const struct ext_power_api api = {.enable = ext_power_generic_enable,
                                         .disable = ext_power_generic_disable,
                                         .get = ext_power_generic_get};

#define ZMK_EXT_POWER_INIT_PRIORITY 81

PM_DEVICE_DT_INST_DEFINE(0, ext_power_generic_pm_action);
DEVICE_DT_INST_DEFINE(0, ext_power_generic_init, PM_DEVICE_DT_INST_GET(0), &data, &config,
                      POST_KERNEL, ZMK_EXT_POWER_INIT_PRIORITY, &api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
