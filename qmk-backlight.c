#include <linux/leds.h>
#include <linux/module.h> /* Needed by all modules */
#include <linux/printk.h> /* Needed for pr_info() */
#include <linux/hid.h>
#include <linux/led-class-multicolor.h>
#include <linux/notifier.h>

struct qmk_priv {
    bool is_removing;
    struct led_classdev_mc mc_cdev;
    struct hid_device *hdev;
    struct list_head list;
};

static struct list_head qmk_device_list = LIST_HEAD_INIT(qmk_device_list);
static int new_possible_qmk_device_attached(struct notifier_block *nb, unsigned long action, void *data);

static struct notifier_block qmk_device_notifier = {
    .notifier_call = new_possible_qmk_device_attached,
};

static int send_hid_request(struct hid_device *hdev, __u8 *data, int len)
{
    struct hid_report *r;
    int ret;
    __u8 *request = NULL, *response = NULL;
    int request_cmd = data[0];
    int retries_remaining = 10;

    r = hdev->report_enum[HID_OUTPUT_REPORT].report_id_hash[0];
    if (!r) {
		hid_err(hdev, "No HID_OUTPUT_REPORT submitted - nothing to write\n");
        ret = -EINVAL;
        goto exit;
    }

	if (hid_report_len(r) < 32) {
        ret = -EINVAL;
        goto exit;
    }

	request = hid_alloc_report_buf(r, GFP_KERNEL);
	if (!request) {
		ret = -ENOMEM;
        goto exit;
    }

    memcpy(request + 1, data, min(len, hid_report_len(r)));

    ret = hid_hw_output_report(hdev, request, hid_report_len(r) + 1);
    if (ret < 0) {
        hid_err(hdev, "Couldn't send HID request: %d\n", ret);
        goto exit;
    }

    r = hdev->report_enum[HID_INPUT_REPORT].report_id_hash[0];
    if (!r) {
		hid_err(hdev, "No HID_INPUT_REPORT submitted - nothing to read\n");
		ret = -EINVAL;
        goto exit;
    }

	if (hid_report_len(r) < 32) {
		ret = -EINVAL;
        goto exit;
    }

	response = hid_alloc_report_buf(r, GFP_KERNEL);
	if (!response) {
		ret = -ENOMEM;
        goto exit;
    }

    while (retries_remaining > 0) {
        ret = hid_hw_raw_request(hdev, 0, response, hid_report_len(r) + 1, HID_INPUT_REPORT, HID_REQ_GET_REPORT);
        if (ret < 0) {
            hid_err(hdev, "Couldn't get HID response: %d\n", ret);
            goto exit;
        }
        if (response[1] != request_cmd) {
            hid_err(hdev, "HID response not matching request type, got %d but expected %d\n", response[1], request_cmd);
        } else {
            break;
        }
        retries_remaining--;
        hid_warn(hdev, "%d retries remaining\n", retries_remaining);
    }
    memcpy(data, response + 1, min(len, (ret - 1)));
exit:
    kfree(request);
    kfree(response);
    return ret;
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

    if (priv->is_removing) {
        return -ENODEV;
    }

    hid_info(priv->hdev, "Got request to set brightness");

    color_to_hsv(priv->mc_cdev.subled_info[0].intensity, priv->mc_cdev.subled_info[1].intensity, priv->mc_cdev.subled_info[2].intensity,
            &h, &s, &v);

    buf = devm_kmalloc_array(&priv->hdev->dev, 32, sizeof(*buf),
            GFP_KERNEL | __GFP_ZERO);
    if (!buf) {
        return -ENOMEM;
    }

    buf[0] = 0x07;
    buf[1] = 0x03;
    buf[2] = 0x01;
    buf[3] = v;
    ret = send_hid_request(priv->hdev, buf, 32);
    if (ret < 0) {
        hid_err(priv->hdev, "Error in setting RGB brightness: %d\n", ret);
        goto exit;
    }

    buf[0] = 0x07;
    buf[1] = 0x03;
    buf[2] = 0x02;
    buf[3] = 0x01;
    ret = send_hid_request(priv->hdev, buf, 32);
    if (ret < 0) {
        hid_err(priv->hdev, "Error in setting RGB effect to solid color: %d\n", ret);
        goto exit;
    }

    buf[0] = 0x07;
    buf[1] = 0x03;
    buf[2] = 0x04;
    buf[3] = h;
    buf[4] = s;
    ret = send_hid_request(priv->hdev, buf, 32);
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
    buf = devm_kmalloc_array(dev, 32, sizeof(*buf),
            GFP_KERNEL | __GFP_ZERO);
    if (!buf) {
        return -ENOMEM;
    }
    buf[0] = 0x01;

    ret = send_hid_request(hdev, buf, 32);
    if (ret < 0) {
        hid_err(hdev, "Error in getting VIA version: %d\n", ret);
        devm_kfree(dev, buf);
        return ret;
    }
    int via_version = ntohs(buf[2] << 8 | buf[1]);
    if (via_version != 0x0C) {
        hid_err(hdev, "Unknown VIA version 0x%x\n", via_version);
        devm_kfree(dev, buf);
        return -EINVAL;
    }

    buf[0] = 0x08;
    buf[1] = 0x03;
    buf[2] = 0x01;
    ret = send_hid_request(hdev, buf, 32);
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
        priv->is_removing = true;
        struct device *dev = &priv->hdev->dev;
        devm_release_action(dev, remove_qmk_device_from_list, priv);
    }
    return 0;
}

static int check_for_qmk_device(struct device *dev, void *data)
{
    struct hid_device *hdev = to_hid_device(dev);
    struct hid_report_enum *report_enum;
    struct hid_report *report;
    struct list_head *list;

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
            int ret = register_qmk_device(dev);
            if (ret < 0) {
                hid_err(hdev, "Handler failed for device\n");
            }
        }
        list = list->next;
    }
    return 0;
}

static int new_possible_qmk_device_attached(struct notifier_block *nb, unsigned long action, void *data)
{
    struct device *dev = data;

    if (action != BUS_NOTIFY_BOUND_DRIVER) {
        return 0;
    }

    check_for_qmk_device(dev, NULL);
    return 0;
}

int init_module(void)
{
    bus_for_each_dev(&hid_bus_type, NULL, NULL, check_for_qmk_device);
    bus_register_notifier(&hid_bus_type, &qmk_device_notifier);

    /* A non 0 return means init_module failed; module can't be loaded. */
    return 0;
}

void cleanup_module(void)
{
    bus_unregister_notifier(&hid_bus_type, &qmk_device_notifier);
    clean_up_qmk_devices();
}

MODULE_LICENSE("GPL");
