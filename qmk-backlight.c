#include <linux/leds.h>
#include <linux/module.h> /* Needed by all modules */ 
#include <linux/printk.h> /* Needed for pr_info() */ 
#include <linux/hid.h>
#include <linux/led-class-multicolor.h>

struct qmk_priv {
    struct led_classdev_mc mc_cdev;
    struct hid_device *hdev;
    struct list_head list;
};

static struct list_head qmk_device_list = LIST_HEAD_INIT(qmk_device_list);

static int send_hid_request(struct hid_device *hdev, __u8 *buf, int len)
{
    int ret;
    ret = hid_hw_output_report(hdev, buf, len);
    if (ret < 0) {
        hid_err(hdev, "Couldn't send HID request: %d\n", ret);
        return ret;
    }
    ret = hid_hw_raw_request(hdev, 0, buf, len - 1, HID_INPUT_REPORT, HID_REQ_GET_REPORT);
    if (ret < 0) {
        hid_err(hdev, "Couldn't get HID response: %d\n", ret);
        return ret;
    }
    if (ret != len - 1) {
        hid_err(hdev, "HID response not expected length, got %d bytes\n", ret);
        return -EINVAL;
    }
    return 0;
}

// Copied from drivers/auxdisplay/ht16k33.c
static void color_to_hsv(int r, int g, int b,
			   int *h, int *s, int *v)
{
	int max_rgb, min_rgb, diff_rgb;
	int aux;
	int third;
	int third_size;

	/* Value */
	max_rgb = max3(r, g, b);
	*v = max_rgb;
	if (!max_rgb) {
		*h = 0;
		*s = 0;
		return;
	}

	/* Saturation */
	min_rgb = min3(r, g, b);
	diff_rgb = max_rgb - min_rgb;
	aux = 255 * diff_rgb;
	aux += max_rgb / 2;
	aux /= max_rgb;
	*s = aux;
	if (!aux) {
		*h = 0;
		return;
	}

	third_size = 85;

	/* Hue */
	if (max_rgb == r) {
		aux =  g - b;
		third = 0;
	} else if (max_rgb == g) {
		aux =  b - r;
		third = third_size;
	} else {
		aux =  r - g;
		third = third_size * 2;
	}

	aux *= third_size / 2;
	aux += diff_rgb / 2;
	aux /= diff_rgb;
	aux += third;

	/* Clamp Hue */
    aux = aux & 0xff;

	*h = aux;
}

static int qmk_set_brightness(struct led_classdev *led_cdev, enum led_brightness brightness)
{
    __u8 *buf;
    int ret = 0;
    int h, s, v;
    struct qmk_priv *priv = container_of(led_cdev, struct qmk_priv, mc_cdev.led_cdev);

    hid_info(priv->hdev, "Got request to set brightness");

    color_to_hsv(priv->mc_cdev.subled_info[0].intensity, priv->mc_cdev.subled_info[1].intensity, priv->mc_cdev.subled_info[2].intensity,
            &h, &s, &v);

    buf = devm_kmalloc_array(&priv->hdev->dev, 33, sizeof(*buf),
            GFP_KERNEL | __GFP_ZERO);
    if (!buf) {
        return -ENOMEM;
    }

    buf[1] = 0x07;
    buf[2] = 0x03;
    buf[3] = 0x01;
    buf[4] = v;
    ret = send_hid_request(priv->hdev, buf, 33);
    if (ret < 0) {
        hid_err(priv->hdev, "Error in setting RGB brightness: %d\n", ret);
        goto exit;
    }

    buf[1] = 0x07;
    buf[2] = 0x03;
    buf[3] = 0x02;
    buf[4] = 0x01;
    ret = send_hid_request(priv->hdev, buf, 33);
    if (ret < 0) {
        hid_err(priv->hdev, "Error in setting RGB effect to solid color: %d\n", ret);
        goto exit;
    }

    buf[1] = 0x07;
    buf[2] = 0x03;
    buf[3] = 0x04;
    buf[4] = h;
    buf[5] = s;
    ret = send_hid_request(priv->hdev, buf, 33);
    if (ret < 0) {
        hid_err(priv->hdev, "Error in setting RGB color: %d\n", ret);
        goto exit;
    }

exit:
    devm_kfree(&priv->hdev->dev, buf);
    return ret;
}

static void remove_qmk_device_from_list(void *data)
{
    struct qmk_priv *priv = data;
    led_classdev_multicolor_unregister(&priv->mc_cdev);
    kfree(priv->mc_cdev.subled_info);
    kfree(priv->mc_cdev.led_cdev.name);
    list_del(&priv->list);
    kfree(priv);
}

static int register_qmk_device(struct device *dev)
{
    struct hid_device *hdev = to_hid_device(dev);
    struct mc_subled *mc_led_info;
    struct led_classdev *led_cdev;
    int ret;

    __u8 *buf;
    buf = devm_kmalloc_array(dev, 33, sizeof(*buf),
            GFP_KERNEL | __GFP_ZERO);
    if (!buf) {
        return -ENOMEM;
    }
    buf[1] = 0x01;

    ret = send_hid_request(hdev, buf, 33);
    if (ret < 0) {
        hid_err(hdev, "Error in getting VIA version: %d\n", ret);
        devm_kfree(dev, buf);
        return ret;
    }
    if (buf[2] != 0x00 || buf[3] != 0x0C) {
        hid_err(hdev, "Unknown VIA version\n");
        devm_kfree(dev, buf);
        return -EINVAL;
    }

    buf[1] = 0x08;
    buf[2] = 0x03;
    buf[3] = 0x01;
    ret = send_hid_request(hdev, buf, 33);
    if (ret < 0) {
        hid_err(hdev, "Error in determining if RGB matrix is enabled: %d\n", ret);
        devm_kfree(dev, buf);
        return ret;
    }
    if (buf[0] == 0xFF) {
        hid_err(hdev, "RGB matrix is not enabled!\n");
        devm_kfree(dev, buf);
        return -EINVAL;
    }
    devm_kfree(dev, buf);

    struct qmk_priv *priv = kzalloc(sizeof(*priv), GFP_KERNEL);
    if (!priv) {
        devm_kfree(dev, mc_led_info);
        return -ENOMEM;
    }
    priv->hdev = hdev;

    mc_led_info = kmalloc_array(3, sizeof(*mc_led_info),
            GFP_KERNEL | __GFP_ZERO);
    if (!mc_led_info) {
        return -ENOMEM;
    }

    mc_led_info[0].color_index = LED_COLOR_ID_RED;
    mc_led_info[1].color_index = LED_COLOR_ID_GREEN;
    mc_led_info[2].color_index = LED_COLOR_ID_BLUE;

    priv->mc_cdev.subled_info = mc_led_info;
    priv->mc_cdev.num_colors = 3;
    led_cdev = &priv->mc_cdev.led_cdev;
    led_cdev->name = kasprintf(GFP_KERNEL, "%s:backlight",
            hdev->name);
    led_cdev->brightness = 255;
    led_cdev->max_brightness = 255;
    led_cdev->brightness_set_blocking = qmk_set_brightness;

    ret = led_classdev_multicolor_register(dev, &priv->mc_cdev);
    if (ret < 0) {
        hid_err(hdev, "Cannot register multicolor LED device\n");
        kfree(priv);
        devm_kfree(dev, mc_led_info);
        return ret;
    }
    list_add(&priv->list, &qmk_device_list);
    devm_add_action(dev, remove_qmk_device_from_list, priv);
    return 0;
}

static int clean_up_qmk_devices(void)
{
    struct list_head *item, *tmp;
    list_for_each_safe (item, tmp, &qmk_device_list) {
        struct qmk_priv *priv = list_entry(item, struct qmk_priv, list);
        struct device *dev = &priv->hdev->dev;
        devm_release_action(dev, remove_qmk_device_from_list, priv);
    }
    return 0;
}

static int get_qmk_devices(struct device *dev, void *data)
{
    struct hid_device *hdev = to_hid_device(dev);
    struct hid_report_enum *report_enum;
    struct hid_report *report;
    struct list_head *list;
    int (*handler)(struct device*) = data;

    report_enum = &hdev->report_enum[HID_INPUT_REPORT];
    list = report_enum->report_list.next;
    while (list != &report_enum->report_list) {
        report = (struct hid_report *) list;
        if (!report->field[0]->application) {
            list = list->next;
            continue;
        }
        unsigned int usage_page = report->field[0]->application >> 16;
        unsigned int usage = report->field[0]->application & 0xFFFF;
        if (usage_page == 0xFF60 && usage == 0x61) {
            int ret = handler(dev);
            if (ret < 0) {
                hid_err(hdev, "Handler failed for device\n");
            }
        }
        list = list->next;
    }
    return 0;
}

int init_module(void) 
{ 
    bus_for_each_dev(&hid_bus_type, NULL, register_qmk_device, get_qmk_devices);

    /* A non 0 return means init_module failed; module can't be loaded. */ 
    return 0; 
} 

void cleanup_module(void) 
{ 
    clean_up_qmk_devices();
} 

MODULE_LICENSE("GPL");
