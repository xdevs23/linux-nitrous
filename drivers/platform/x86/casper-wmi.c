// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/acpi.h>
#include <linux/leds.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/wmi.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/hwmon.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/dmi.h>
#include <acpi/acexcep.h>

MODULE_AUTHOR("Mustafa Ekşi <mustafa.eskieksi@gmail.com>");
MODULE_DESCRIPTION("Casper Excalibur Laptop WMI driver");
MODULE_LICENSE("GPL");

#define CASPER_WMI_GUID "644C5791-B7B0-4123-A90B-E93876E0DAAD"

#define CASPER_READ 0xfa00
#define CASPER_WRITE 0xfb00
#define CASPER_GET_HARDWAREINFO 0x0200
#define CASPER_SET_LED 0x0100
#define CASPER_POWERPLAN 0x0300

#define CASPER_KEYBOARD_LED_1 0x03
#define CASPER_KEYBOARD_LED_2 0x04
#define CASPER_KEYBOARD_LED_3 0x05
#define CASPER_ALL_KEYBOARD_LEDS 0x06
#define CASPER_CORNER_LEDS 0x07

struct casper_wmi_args {
	u16 a0, a1;
	u32 a2, a3, a4, a5, a6, a7, a8;
};

static u32 casper_last_color;
static u8 casper_last_led;

static acpi_status casper_set(struct wmi_device *wdev, u16 a1, u8 led_id,
			      u32 data)
{
	struct casper_wmi_args wmi_args = {
		.a0 = CASPER_WRITE,
		.a1 = a1,
		.a2 = led_id,
		.a3 = data
	};
	struct acpi_buffer input = {
		(acpi_size) sizeof(struct casper_wmi_args),
		&wmi_args
	};
	return wmidev_block_set(wdev, 0, &input);
}

static ssize_t led_control_show(struct device *dev, struct device_attribute
				*attr, char *buf)
{
	return sprintf("%u%08x\n", buf, casper_last_led,
		       casper_last_color);
}


// input is formatted as "IMARRGGBB", I: led_id, M: mode, A: brightness, ...
static ssize_t led_control_store(struct device *dev, struct device_attribute
				 *attr, const char *buf, size_t count)
{
	u64 tmp;
	int ret;

	ret = kstrtou64(buf, 16, &tmp);

	if (ret)
		return ret;

	u8 led_id = (tmp >> (8 * 4))&0xFF;

	ret =
	    casper_set(to_wmi_device(dev->parent), CASPER_SET_LED, led_id,
		       (u32) tmp
	    );
	if (ACPI_FAILURE(ret)) {
		dev_err(dev, "casper-wmi ACPI status: %d\n", ret);
		return ret;
	}
	if (led_id != CASPER_CORNER_LEDS) {
		casper_last_color = (u32) tmp;
		casper_last_led = led_id;
	}
	return count;
}

static DEVICE_ATTR_RW(led_control);

static struct attribute *casper_kbd_led_attrs[] = {
	&dev_attr_led_control.attr,
	NULL,
};

ATTRIBUTE_GROUPS(casper_kbd_led);

static void set_casper_backlight_brightness(struct led_classdev *led_cdev,
					    enum led_brightness brightness)
{
	// Setting any of the keyboard leds' brightness sets brightness of all
	acpi_status ret =
	    casper_set(to_wmi_device(led_cdev->dev->parent), CASPER_SET_LED,
		       CASPER_KEYBOARD_LED_1,
		       (casper_last_color & 0xF0FFFFFF) |
		       (((u32) brightness) << 24)
	    );

	if (ret != 0)
		dev_err(led_cdev->dev,
			"Couldn't set brightness acpi status: %d\n", ret);
}

static enum led_brightness get_casper_backlight_brightness(struct led_classdev
							   *led_cdev)
{
	return (casper_last_color&0x0F000000)>>24;
}

static struct led_classdev casper_kbd_led = {
	.name = "casper::kbd_backlight",
	.brightness = 0,
	.brightness_set = set_casper_backlight_brightness,
	.brightness_get = get_casper_backlight_brightness,
	.max_brightness = 2,
	.groups = casper_kbd_led_groups,
};

static acpi_status casper_query(struct wmi_device *wdev, u16 a1,
				struct casper_wmi_args *out)
{
	struct casper_wmi_args wmi_args = {
		.a0 = CASPER_READ,
		.a1 = a1
	};
	struct acpi_buffer input = {
		(acpi_size) sizeof(struct casper_wmi_args),
		&wmi_args
	};

	acpi_status ret = wmidev_block_set(wdev, 0, &input);

	if (ACPI_FAILURE(ret)) {
		dev_err(&wdev->dev,
			"Could not query acpi status: %u", ret);
		return ret;
	}

	union acpi_object *obj = wmidev_block_query(wdev, 0);

	if (obj == NULL) {
		dev_err(&wdev->dev,
			"Could not query hardware information");
		return AE_ERROR;
	}
	if (obj->type != ACPI_TYPE_BUFFER) {
		dev_err(&wdev->dev, "Return type is not a buffer");
		return AE_TYPE;
	}

	if (obj->buffer.length != 32) {
		dev_err(&wdev->dev, "Return buffer is not long enough");
		return AE_ERROR;
	}
	memcpy(out, obj->buffer.pointer, 32);
	kfree(obj);
	return ret;
}

static umode_t casper_wmi_hwmon_is_visible(const void *drvdata,
					   enum hwmon_sensor_types type,
					   u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		return 0444;
	case hwmon_pwm:
		return 0644;
	default:
		return 0;
	}
	return 0;
}

static int casper_wmi_hwmon_read(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, long *val)
{
	struct casper_wmi_args out = { 0 };

	switch (type) {
	case hwmon_fan:
		acpi_status ret = casper_query(to_wmi_device(dev->parent),
					       CASPER_GET_HARDWAREINFO, &out);

		if (ACPI_FAILURE(ret))
			return ret;

		if (channel == 0) { // CPU fan
			u32 cpu_fanspeed = out.a4;

			cpu_fanspeed <<= 8;
			cpu_fanspeed += out.a4 >> 8;
			*val = (long) cpu_fanspeed;
		} else if (channel == 1) { // GPU fan
			u32 gpu_fanspeed = out.a5;

			gpu_fanspeed <<= 8;
			gpu_fanspeed += out.a5 >> 8;
			*val = (long) gpu_fanspeed;
		}
		return 0;
	case hwmon_pwm:
		casper_query(to_wmi_device(dev->parent), CASPER_POWERPLAN,
			     &out);
		if (channel == 0)
			*val = (long)out.a2;
		else
			return -EOPNOTSUPP;
		return 0;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int casper_wmi_hwmon_read_string(struct device *dev,
					enum hwmon_sensor_types type, u32 attr,
					int channel, const char **str)
{
	switch (type) {
	case hwmon_fan:
		switch (channel) {
		case 0:
			*str = "cpu_fan_speed";
			break;
		case 1:
			*str = "gpu_fan_speed";
			break;
		default:
			return -EOPNOTSUPP;
		}
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static int casper_wmi_hwmon_write(struct device *dev,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel, long val)
{
	acpi_status ret;

	switch (type) {
	case hwmon_pwm:
		if (channel != 0)
			return -EOPNOTSUPP;
		ret =
		    casper_set(to_wmi_device(dev->parent), CASPER_POWERPLAN,
			       val, 0);

		if (ACPI_FAILURE(ret)) {
			dev_err(dev, "Couldn't set power plan, acpi_status: %d",
				ret);
			return -EINVAL;
		}
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops casper_wmi_hwmon_ops = {
	.is_visible = &casper_wmi_hwmon_is_visible,
	.read = &casper_wmi_hwmon_read,
	.read_string = &casper_wmi_hwmon_read_string,
	.write = &casper_wmi_hwmon_write
};

static const struct hwmon_channel_info *const casper_wmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_MODE),
	NULL
};

static const struct hwmon_chip_info casper_wmi_hwmon_chip_info = {
	.ops = &casper_wmi_hwmon_ops,
	.info = casper_wmi_hwmon_info,
};

static int casper_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct device *hwmon_dev;

	// All Casper Excalibur Laptops use this GUID
	if (!wmi_has_guid(CASPER_WMI_GUID))
		return -ENODEV;

	hwmon_dev =
	    devm_hwmon_device_register_with_info(&wdev->dev, "casper_wmi", wdev,
						 &casper_wmi_hwmon_chip_info,
						 NULL);

	acpi_status result = led_classdev_register(&wdev->dev, &casper_kbd_led);

	if (result != 0)
		return -ENODEV;

	return PTR_ERR_OR_ZERO(hwmon_dev);
	}

static void casper_wmi_remove(struct wmi_device *wdev)
{
	led_classdev_unregister(&casper_kbd_led);
}

static const struct wmi_device_id casper_wmi_id_table[] = {
	{ CASPER_WMI_GUID, NULL },
	{ }
};

static struct wmi_driver casper_wmi_driver = {
	.driver = {
		   .name = "casper-wmi",
		    },
	.id_table = casper_wmi_id_table,
	.probe = casper_wmi_probe,
	.remove = &casper_wmi_remove
};

module_wmi_driver(casper_wmi_driver);

MODULE_DEVICE_TABLE(wmi, casper_wmi_id_table);
