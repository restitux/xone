// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2021 Severin von Wnuck <severinvonw@outlook.de>
 */

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

static struct device_attribute gip_led_attr_mode =
	__ATTR(mode, 0644, gip_led_mode_show, gip_led_mode_store);

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
