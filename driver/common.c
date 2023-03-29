// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

#include <linux/device.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include "common.h"

#define GIP_LED_BRIGHTNESS_DEFAULT 20
#define GIP_LED_BRIGHTNESS_MAX 50

static enum power_supply_property gip_battery_props[] = {
    POWER_SUPPLY_PROP_STATUS,
    POWER_SUPPLY_PROP_CAPACITY_LEVEL,
    POWER_SUPPLY_PROP_SCOPE,
    POWER_SUPPLY_PROP_MODEL_NAME,
};

static int gip_get_battery_prop(struct power_supply *psy,
    enum power_supply_property psp,
    union power_supply_propval *val)
{
    struct gip_battery *batt = power_supply_get_drvdata(psy);

    switch (psp) {
    case POWER_SUPPLY_PROP_STATUS:
	val->intval = batt->status;
	break;
    case POWER_SUPPLY_PROP_CAPACITY_LEVEL:
	val->intval = batt->capacity;
	break;
    case POWER_SUPPLY_PROP_SCOPE:
	val->intval = POWER_SUPPLY_SCOPE_DEVICE;
	break;
    case POWER_SUPPLY_PROP_MODEL_NAME:
	val->strval = batt->name;
	break;
    default:
	return -EINVAL;
    }

    return 0;
}

int gip_init_battery(struct gip_battery *batt, struct gip_client *client,
    const char *name)
{
    struct power_supply_config cfg = {};

    batt->name = name;
    batt->status = POWER_SUPPLY_STATUS_UNKNOWN;
    batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;

    batt->desc.name = dev_name(&client->dev);
    batt->desc.type = POWER_SUPPLY_TYPE_BATTERY;
    batt->desc.properties = gip_battery_props;
    batt->desc.num_properties = ARRAY_SIZE(gip_battery_props);
    batt->desc.get_property = gip_get_battery_prop;

    cfg.drv_data = batt;

    batt->supply = devm_power_supply_register(&client->dev, &batt->desc,
	&cfg);
    if (IS_ERR(batt->supply)) {
	dev_err(&client->dev, "%s: register failed: %ld\n",
	    __func__, PTR_ERR(batt->supply));
	return PTR_ERR(batt->supply);
    }

    power_supply_powers(batt->supply, &client->dev);

    return 0;
}
EXPORT_SYMBOL_GPL(gip_init_battery);

void gip_report_battery(struct gip_battery *batt,
    enum gip_battery_type type,
    enum gip_battery_level level)
{
    if (type == GIP_BATT_TYPE_NONE)
	batt->status = POWER_SUPPLY_STATUS_NOT_CHARGING;
    else
	batt->status = POWER_SUPPLY_STATUS_DISCHARGING;

    if (type == GIP_BATT_TYPE_NONE)
	batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_UNKNOWN;
    else if (level == GIP_BATT_LEVEL_LOW)
	batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_LOW;
    else if (level == GIP_BATT_LEVEL_NORMAL)
	batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
    else if (level == GIP_BATT_LEVEL_HIGH)
	batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_HIGH;
    else if (level == GIP_BATT_LEVEL_FULL)
	batt->capacity = POWER_SUPPLY_CAPACITY_LEVEL_FULL;

    if (batt->supply)
	power_supply_changed(batt->supply);
}
EXPORT_SYMBOL_GPL(gip_report_battery);

static void gip_led_brightness_set(struct led_classdev *dev,
    enum led_brightness brightness)
{
    struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(dev);
    struct gip_led *led = container_of(mc_cdev, typeof(*led), dev);
    int err;
    uint8_t red, green, blue;

    if (dev->flags & LED_UNREGISTERING)
	return;

    dev_dbg(&led->client->dev, "%s: brightness=%d\n", __func__, brightness);

    err = gip_set_led_mode(led->client, led->mode, brightness);
    if (err)
	dev_err(&led->client->dev, "%s: set LED mode failed: %d\n",
	    __func__, err);

    if (led->rgb) {
	led_mc_calc_color_components(mc_cdev, brightness);

	red = mc_cdev->subled_info[0].brightness;
	green = mc_cdev->subled_info[1].brightness;
	blue = mc_cdev->subled_info[2].brightness;

	err = gip_set_led_rgb(led->client, red, blue, green);
	if (err)
	    dev_err(&led->client->dev, "%s: set LED mode failed: %d\n",
		__func__, err);
    }
}

static ssize_t gip_led_mode_show(struct device *dev,
    struct device_attribute *attr, char *buf)
{
    struct led_classdev_mc *cdev = dev_get_drvdata(dev);
    struct gip_led *led = container_of(cdev, typeof(*led), dev);

    return sprintf(buf, "%u\n", led->mode);
}

static ssize_t gip_led_mode_store(struct device *dev,
    struct device_attribute *attr,
    const char *buf, size_t count)
{
    struct led_classdev_mc *cdev = dev_get_drvdata(dev);
    struct gip_led *led = container_of(cdev, typeof(*led), dev);
    u8 mode;
    int err;

    err = kstrtou8(buf, 10, &mode);
    if (err)
	return err;

    dev_dbg(&led->client->dev, "%s: mode=%u\n", __func__, mode);
    led->mode = mode;

    err = gip_set_led_mode(led->client, mode, cdev->led_cdev.brightness);
    if (err) {
	dev_err(&led->client->dev, "%s: set LED mode failed: %d\n",
	    __func__, err);
	return err;
    }

    return count;
}

static struct device_attribute gip_led_attr_mode = __ATTR(mode, 0644, gip_led_mode_show, gip_led_mode_store);

static struct attribute *gip_led_attrs[] = {
    &gip_led_attr_mode.attr,
    NULL,
};
ATTRIBUTE_GROUPS(gip_led);

int gip_init_led(struct gip_led *led, struct gip_client *client)
{
    int err;

    /* set default brightness */
    err = gip_set_led_mode(client, GIP_LED_ON, GIP_LED_BRIGHTNESS_DEFAULT);
    if (err) {
	dev_err(&client->dev, "%s: set brightness failed: %d\n",
	    __func__, err);
	return err;
    }

    if (led->rgb) {
	led->dev.led_cdev.name = devm_kasprintf(&client->dev, GFP_KERNEL,
	    "%s:rgb:status",
	    dev_name(&client->dev));
	if (!led->dev.led_cdev.name)
	    return -ENOMEM;

	led->dev.led_cdev.brightness = GIP_LED_BRIGHTNESS_DEFAULT;
	led->dev.led_cdev.max_brightness = GIP_LED_BRIGHTNESS_MAX;
	led->dev.led_cdev.brightness_set = gip_led_brightness_set;
	led->dev.led_cdev.groups = gip_led_groups;

	// Configure RBG LEDS for XES2
	led->dev.num_colors = 3;
	led->dev.subled_info = devm_kcalloc(&client->dev, 3, sizeof(struct mc_subled), GFP_KERNEL);
	if (!led->dev.subled_info) {
	    return -ENOMEM;
	}

	led->dev.subled_info[0].color_index = LED_COLOR_ID_RED;
	led->dev.subled_info[0].channel = 0;
	led->dev.subled_info[1].color_index = LED_COLOR_ID_GREEN;
	led->dev.subled_info[0].channel = 1;
	led->dev.subled_info[2].color_index = LED_COLOR_ID_BLUE;
	led->dev.subled_info[0].channel = 2;

	led->client = client;
	led->mode = GIP_LED_ON;

	err = devm_led_classdev_multicolor_register(&client->dev, &led->dev);
	if (err)
	    dev_err(&client->dev, "%s: register failed: %d\n",
		__func__, err);
    } else {
	led->dev.led_cdev.name = devm_kasprintf(&client->dev, GFP_KERNEL,
	    "%s:white:status",
	    dev_name(&client->dev));
	if (!led->dev.led_cdev.name)
	    return -ENOMEM;

	led->dev.led_cdev.brightness = GIP_LED_BRIGHTNESS_DEFAULT;
	led->dev.led_cdev.max_brightness = GIP_LED_BRIGHTNESS_MAX;
	led->dev.led_cdev.brightness_set = gip_led_brightness_set;
	led->dev.led_cdev.groups = gip_led_groups;

	led->client = client;
	led->mode = GIP_LED_ON;

	err = devm_led_classdev_register(&client->dev, &led->dev.led_cdev);
	if (err)
	    dev_err(&client->dev, "%s: register failed: %d\n",
		__func__, err);
    }

    return err;
}
EXPORT_SYMBOL_GPL(gip_init_led);

ssize_t gip_profile_show(struct kobject *kobj, struct kobj_attribute *attr,
    char *buf)
{
    int var;

    if (strcmp(attr->attr.name, "") == 0)
	var = 20;
    else
	var = 30;
    return sysfs_emit(buf, "%d\n", var);
}

ssize_t gip_profile_store(struct kobject *kobj, struct kobj_attribute *attr,
    const char *buf, size_t count)
{
    return 0;
    // int var, ret;

    // ret = kstrtoint(buf, 10, &var);
    // if (ret < 0)
    //	return ret;

    // if (strcmp(attr->attr.name, "baz") == 0)
    //	int baz = var;
    // else
    //	int bar = var;
    // return count;
}

int gip_init_profile(struct gip_profile *profile, struct gip_client *client,
    struct device *gip_root)
{
    int retval;
    struct kobj_attribute *attributes;
    struct attribute **attrs;
    struct attribute_group *attr_group;
    // struct device *gip_root;
    struct kobject *device_kobj;

    for (int i = 0; i < 3; i++) {
	for (int j = 0; j < 2; j++) {
	    profile->profiles[i].layers[j] = (struct gip_xes2_layer) {
		.right_top_paddle = GIP_UNMAPPED,
		.right_bottom_paddle = GIP_UNMAPPED,
		.left_top_paddle = GIP_UNMAPPED,
		.left_bottom_paddle = GIP_UNMAPPED,
		.a = GIP_A,
		.b = GIP_B,
		.x = GIP_X,
		.y = GIP_Y,
		.dpad_up = GIP_DPAD_UP,
		.dpad_down = GIP_DPAD_DOWN,
		.dpad_left = GIP_DPAD_LEFT,
		.dpad_right = GIP_DPAD_RIGHT,
		.lb = GIP_LB,
		.rb = GIP_RB,
		.ls_click = GIP_LS_CLICK,
		.rs_click = GIP_RS_CLICK,
		.lt = GIP_TRIGGER_DEFAULT,
		.rt = GIP_TRIGGER_DEFAULT,
		.left_main_vibration = 0xFF,
		.right_main_vibration = 0xFF,
		.left_trigger_vibration = 0xFF,
		.right_trigger_vibration = 0xFF,
		.lt_deadzone_max = 0xFF,
		.lt_deadzone_min = 0x00,
		.rt_deadzone_max = 0xFF,
		.rt_deadzone_min = 0x00,
		.guide_red = 0xFF,
		.guide_green = 0xFF,
		.guide_blue = 0xFF,
	    };
	}
    }

    attributes = devm_kcalloc(&client->dev, 29, sizeof(struct kobj_attribute), GFP_KERNEL);
    if (!attributes) {
	return -ENOMEM;
    }

    attributes[0] = (struct kobj_attribute)__ATTR(right_top_paddle, 0664, gip_profile_show, gip_profile_store);
    attributes[1] = (struct kobj_attribute)__ATTR(right_bottom_paddle, 0664, gip_profile_show, gip_profile_store);
    attributes[2] = (struct kobj_attribute)__ATTR(left_top_paddle, 0664, gip_profile_show, gip_profile_store);
    attributes[3] = (struct kobj_attribute)__ATTR(left_bottom_paddle, 0664, gip_profile_show, gip_profile_store);
    attributes[4] = (struct kobj_attribute)__ATTR(a, 0664, gip_profile_show, gip_profile_store);
    attributes[5] = (struct kobj_attribute)__ATTR(b, 0664, gip_profile_show, gip_profile_store);
    attributes[6] = (struct kobj_attribute)__ATTR(x, 0664, gip_profile_show, gip_profile_store);
    attributes[7] = (struct kobj_attribute)__ATTR(y, 0664, gip_profile_show, gip_profile_store);
    attributes[8] = (struct kobj_attribute)__ATTR(dpad_up, 0664, gip_profile_show, gip_profile_store);
    attributes[9] = (struct kobj_attribute)__ATTR(dpad_down, 0664, gip_profile_show, gip_profile_store);
    attributes[10] = (struct kobj_attribute)__ATTR(dpad_left, 0664, gip_profile_show, gip_profile_store);
    attributes[11] = (struct kobj_attribute)__ATTR(dpad_right, 0664, gip_profile_show, gip_profile_store);
    attributes[12] = (struct kobj_attribute)__ATTR(lb, 0664, gip_profile_show, gip_profile_store);
    attributes[13] = (struct kobj_attribute)__ATTR(rb, 0664, gip_profile_show, gip_profile_store);
    attributes[14] = (struct kobj_attribute)__ATTR(ls_click, 0664, gip_profile_show, gip_profile_store);
    attributes[15] = (struct kobj_attribute)__ATTR(rs_click, 0664, gip_profile_show, gip_profile_store);
    attributes[16] = (struct kobj_attribute)__ATTR(lt, 0664, gip_profile_show, gip_profile_store);
    attributes[17] = (struct kobj_attribute)__ATTR(rt, 0664, gip_profile_show, gip_profile_store);
    attributes[18] = (struct kobj_attribute)__ATTR(left_main_vibration, 0664, gip_profile_show, gip_profile_store);
    attributes[19] = (struct kobj_attribute)__ATTR(right_main_vibration, 0664, gip_profile_show, gip_profile_store);
    attributes[20] = (struct kobj_attribute)__ATTR(left_trigger_vibration, 0664, gip_profile_show, gip_profile_store);
    attributes[21] = (struct kobj_attribute)__ATTR(right_trigger_vibration, 0664, gip_profile_show, gip_profile_store);
    attributes[22] = (struct kobj_attribute)__ATTR(lt_deadzone_max, 0664, gip_profile_show, gip_profile_store);
    attributes[23] = (struct kobj_attribute)__ATTR(lt_deadzone_min, 0664, gip_profile_show, gip_profile_store);
    attributes[24] = (struct kobj_attribute)__ATTR(rt_deadzone_max, 0664, gip_profile_show, gip_profile_store);
    attributes[25] = (struct kobj_attribute)__ATTR(rt_deadzone_min, 0664, gip_profile_show, gip_profile_store);
    attributes[26] = (struct kobj_attribute)__ATTR(guide_red, 0664, gip_profile_show, gip_profile_store);
    attributes[27] = (struct kobj_attribute)__ATTR(guide_green, 0664, gip_profile_show, gip_profile_store);
    attributes[28] = (struct kobj_attribute)__ATTR(guide_blue, 0664, gip_profile_show, gip_profile_store);

    attrs = devm_kcalloc(&client->dev, 30, sizeof(struct attribute *), GFP_KERNEL);
    if (!attrs) {
	return -ENOMEM;
    }

    attrs[0] = &attributes[0].attr;
    attrs[1] = &attributes[1].attr;
    attrs[2] = &attributes[2].attr;
    attrs[3] = &attributes[3].attr;
    attrs[4] = &attributes[4].attr;
    attrs[5] = &attributes[5].attr;
    attrs[6] = &attributes[6].attr;
    attrs[7] = &attributes[7].attr;
    attrs[8] = &attributes[8].attr;
    attrs[9] = &attributes[9].attr;
    attrs[10] = &attributes[10].attr;
    attrs[11] = &attributes[11].attr;
    attrs[12] = &attributes[12].attr;
    attrs[13] = &attributes[13].attr;
    attrs[14] = &attributes[14].attr;
    attrs[15] = &attributes[15].attr;
    attrs[16] = &attributes[16].attr;
    attrs[17] = &attributes[17].attr;
    attrs[18] = &attributes[18].attr;
    attrs[19] = &attributes[19].attr;
    attrs[20] = &attributes[20].attr;
    attrs[21] = &attributes[21].attr;
    attrs[22] = &attributes[22].attr;
    attrs[23] = &attributes[23].attr;
    attrs[24] = &attributes[24].attr;
    attrs[25] = &attributes[25].attr;
    attrs[26] = &attributes[26].attr;
    attrs[27] = &attributes[27].attr;
    attrs[28] = &attributes[28].attr;
    attrs[29] = NULL;

    // static struct attribute *attrs[] = {
    //	&foo_attribute.attr,
    //	&baz_attribute.attr,
    //	&bar_attribute.attr,
    //	NULL,	/* need to NULL terminate the list of attributes */
    //};

    /*
     * An unnamed attribute group will put all of the attributes directly in
     * the kobject directory.  If we specify a name, a subdirectory will be
     * created for the attributes with the directory being the name of the
     * attribute group.
     */
    attr_group = devm_kcalloc(&client->dev, 1, sizeof(struct attribute_group), GFP_KERNEL);
    if (!attr_group) {
	return -ENOMEM;
    }

    attr_group->attrs = attrs;

    device_kobj = kobject_create_and_add(dev_name(&client->dev), &gip_root->kobj);
    if (!device_kobj)
	return -ENOMEM;
    /* Create the files associated with this kobject */
    retval = sysfs_create_group(device_kobj, attr_group);
    if (retval)
	kobject_put(device_kobj);

    return 0;
}
EXPORT_SYMBOL_GPL(gip_init_profile);

int gip_init_input(struct gip_input *input, struct gip_client *client,
    const char *name)
{
    input->dev = devm_input_allocate_device(&client->dev);
    if (!input->dev)
	return -ENOMEM;

    input->dev->phys = devm_kasprintf(&client->dev, GFP_KERNEL,
	"%s/input0", dev_name(&client->dev));
    if (!input->dev->phys)
	return -ENOMEM;

    input->dev->name = name;
    input->dev->id.bustype = BUS_VIRTUAL;
    input->dev->id.vendor = client->hardware.vendor;
    input->dev->id.product = client->hardware.product;
    input->dev->id.version = client->hardware.version;
    input->dev->dev.parent = &client->dev;

    return 0;
}
EXPORT_SYMBOL_GPL(gip_init_input);
