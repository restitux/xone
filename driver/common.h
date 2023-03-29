/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#pragma once

#include <linux/input.h>
#include <linux/led-class-multicolor.h>
#include <linux/leds.h>
#include <linux/power_supply.h>

#include "../bus/bus.h"

struct gip_battery {
    struct power_supply *supply;
    struct power_supply_desc desc;

    const char *name;
    int status;
    int capacity;
};

struct gip_led {
    bool rgb;
    struct led_classdev_mc dev;

    struct gip_client *client;
    enum gip_led_mode mode;
};

struct gip_input {
    struct input_dev *dev;
};

struct gip_xes2_layer {
    unsigned int right_top_paddle;
    unsigned int right_bottom_paddle;
    unsigned int left_top_paddle;
    unsigned int left_bottom_paddle;
    unsigned int a;
    unsigned int b;
    unsigned int x;
    unsigned int y;
    unsigned int dpad_up;
    unsigned int dpad_down;
    unsigned int dpad_left;
    unsigned int dpad_right;
    unsigned int lb;
    unsigned int rb;
    unsigned int ls_click;
    unsigned int rs_click;
    unsigned int lt; // if set to GIP_TRIGGER_DEFAULT (0), trigger is bound to itself
    unsigned int rt; // if set to GIP_TRIGGER_DEFAULT (0), trigger is bound to itself
    unsigned int left_main_vibration;
    unsigned int right_main_vibration;
    unsigned int left_trigger_vibration;
    unsigned int right_trigger_vibration;
    unsigned int lt_deadzone_max;
    unsigned int lt_deadzone_min;
    unsigned int rt_deadzone_max;
    unsigned int rt_deadzone_min;
    unsigned int guide_red;
    unsigned int guide_green;
    unsigned int guide_blue;
};

struct gip_xes2_profile {
    struct gip_xes2_layer layers[2];
};

struct gip_profile {
    struct gip_xes2_profile profiles[3];
};

int gip_init_battery(struct gip_battery *batt, struct gip_client *client,
    const char *name);
void gip_report_battery(struct gip_battery *batt,
    enum gip_battery_type type,
    enum gip_battery_level level);

int gip_init_led(struct gip_led *led, struct gip_client *client);

int gip_init_profile(struct gip_profile *profile, struct gip_client *client,
    struct device *gip_root);

int gip_init_input(struct gip_input *input, struct gip_client *client,
    const char *name);
