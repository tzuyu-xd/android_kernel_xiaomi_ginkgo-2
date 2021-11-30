/*
 * TEE driver for goodix fingerprint sensor
 * Copyright (C) 2016 Goodix
 * Copyright (C) 2021 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/ioctl.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/msm_drm_notify.h>
#include <linux/notifier.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_bridge.h>
#include <net/netlink.h>
#include <net/sock.h>

typedef enum gf_key_event {
	GF_KEY_NONE = 0,
	GF_KEY_HOME,
	GF_KEY_POWER,
	GF_KEY_CAMERA,
} gf_key_event_t;

struct gf_key {
	enum gf_key_event key;
	uint32_t value;   /* key down = 1, key up = 0 */
};

struct gf_key_map {
	unsigned int type;
	unsigned int code;
};

#define GF_IOC_MAGIC             'g' /* define magic number */
#define GF_IOC_INIT             _IOR(GF_IOC_MAGIC, 0, uint8_t)
#define GF_IOC_RESET            _IO(GF_IOC_MAGIC, 2)
#define GF_IOC_ENABLE_IRQ       _IO(GF_IOC_MAGIC, 3)
#define GF_IOC_DISABLE_IRQ      _IO(GF_IOC_MAGIC, 4)
#define GF_IOC_INPUT_KEY_EVENT  _IOW(GF_IOC_MAGIC, 9, struct gf_key)
#define GF_IOC_HAL_INITED_READY _IO(GF_IOC_MAGIC, 15)

struct gf_dev {
	dev_t devt;
	struct list_head device_entry;
	struct platform_device *spi;
	struct input_dev *input;
	unsigned users;
	signed irq_gpio;
	signed reset_gpio;
	int irq;
	int irq_enabled;
	int clk_enabled;
	struct notifier_block notifier;
	char device_available;
	char drm_black;
	char wait_finger_down;
	struct work_struct work;
};

#define WAKELOCK_HOLD_TIME 		2000 /* in ms */
#define FP_UNLOCK_REJECTION_TIMEOUT 	(WAKELOCK_HOLD_TIME - 500)

#define GF_SPIDEV_NAME 		"goodix,fingerprint"
#define GF_DEV_NAME 		"goodix_fp"
#define GF_INPUT_NAME 		"uinput-goodix" /* "goodix_fp" */
#define CHRD_DRIVER_NAME 	"goodix_fp_spi"
#define CLASS_NAME 		"goodix_fp"

#define GF_NET_EVENT_IRQ 	1
#define NETLINK_TEST 		25
#define MAX_MSGSIZE 		32
#define N_SPI_MINORS 		32	/* ... up to 256 */

static int SPIDEV_MAJOR;

static DECLARE_BITMAP(minors, N_SPI_MINORS);
static LIST_HEAD(device_list);
static struct wakeup_source fp_ws;
static struct gf_dev gf;
static int pid = -1;
static struct sock *nl_sk = NULL;

extern int fpsensor;

struct gf_key_map maps[] = {
        { EV_KEY, KEY_HOME },
        { EV_KEY, KEY_POWER },
        { EV_KEY, KEY_CAMERA },
        { EV_KEY, KEY_KPENTER },
};

static void sendnlmsg(char *message)
{
	struct sk_buff *skb_1;
	struct nlmsghdr *nlh;
	int len = NLMSG_SPACE(MAX_MSGSIZE);
	int slen = 0;
	int ret = 0;

	if (!message || !nl_sk || !pid)
		return;

	skb_1 = alloc_skb(len, GFP_KERNEL);
	if (!skb_1) {
		pr_err("alloc_skb error\n");
		return;
	}

	slen = strlen(message);
	nlh = nlmsg_put(skb_1, 0, 0, 0, MAX_MSGSIZE, 0);

	NETLINK_CB(skb_1).portid = 0;
	NETLINK_CB(skb_1).dst_group = 0;

	message[slen]= '\0';
	memcpy(NLMSG_DATA(nlh), message, slen + 1);

	ret = netlink_unicast(nl_sk, skb_1, pid, MSG_DONTWAIT);
	if (!ret)
		pr_err("send msg from kernel to usespace failed ret 0x%x \n", ret);
}

static void nl_data_ready(struct sk_buff *__skb)
{
	struct sk_buff *skb;
	struct nlmsghdr *nlh;
	char str[100];

	skb = skb_get (__skb);
	if (skb->len >= NLMSG_SPACE(0)) {
		nlh = nlmsg_hdr(skb);
		memcpy(str, NLMSG_DATA(nlh), sizeof(str));
		pid = nlh->nlmsg_pid;
		kfree_skb(skb);
	}
}

static int netlink_init(void)
{
	struct netlink_kernel_cfg netlink_cfg;

	memset(&netlink_cfg, 0, sizeof(struct netlink_kernel_cfg));
	netlink_cfg.groups = 0;
	netlink_cfg.flags = 0;
	netlink_cfg.input = nl_data_ready;
	netlink_cfg.cb_mutex = NULL;

	nl_sk = netlink_kernel_create(&init_net, NETLINK_TEST,
			&netlink_cfg);
	if (!nl_sk) {
		pr_err("create netlink socket error\n");
		return 1;
	}

	return 0;
}

static void netlink_exit(void)
{
	if (nl_sk != NULL) {
		netlink_kernel_release(nl_sk);
		nl_sk = NULL;
	}

	pr_info("self module exited\n");
}

static int gf_parse_dts(struct gf_dev *gf_dev)
{
	int rc = 0;
	struct device *dev = &gf_dev->spi->dev;
	struct device_node *np = dev->of_node;

	gf_dev->reset_gpio = of_get_named_gpio(np, "fp-gpio-reset", 0);
	if (gf_dev->reset_gpio < 0) {
		pr_err("falied to get reset gpio!\n");
		return gf_dev->reset_gpio;
	}

	rc = devm_gpio_request(dev, gf_dev->reset_gpio, "goodix_reset");
	if (rc) {
		pr_err("failed to request reset gpio, rc = %d\n", rc);
		return rc;
	}
	gpio_direction_output(gf_dev->reset_gpio, 0);

	gf_dev->irq_gpio = of_get_named_gpio(np, "fp-gpio-irq", 0);
	if (gf_dev->irq_gpio < 0) {
		pr_err("falied to get irq gpio!\n");
		return gf_dev->irq_gpio;
	}

	rc = devm_gpio_request(dev, gf_dev->irq_gpio, "goodix_irq");
	if (rc) {
		pr_err("failed to request irq gpio, rc = %d\n", rc);
		devm_gpio_free(dev, gf_dev->reset_gpio);
	}
	gpio_direction_input(gf_dev->irq_gpio);

	return rc;
}

static void gf_cleanup(struct gf_dev *gf_dev)
{
	if (gpio_is_valid(gf_dev->irq_gpio))
		gpio_free(gf_dev->irq_gpio);

	if (gpio_is_valid(gf_dev->reset_gpio))
		gpio_free(gf_dev->reset_gpio);
}

static irqreturn_t gf_irq(int irq, void *handle)
{
	char msg = GF_NET_EVENT_IRQ;
	struct gf_dev *gf_dev = &gf;
	__pm_wakeup_event(&fp_ws, WAKELOCK_HOLD_TIME);
	sendnlmsg(&msg);
	if (gf_dev->device_available == 1) {
		gf_dev->wait_finger_down = false;
		schedule_work(&gf_dev->work);
	}

	return IRQ_HANDLED;
}

static int irq_setup(struct gf_dev *gf_dev)
{
	int status;

	gf_dev->irq = gpio_to_irq(gf_dev->irq_gpio);
	status = request_threaded_irq(gf_dev->irq, NULL, gf_irq,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			"gf", gf_dev);
	if (status) {
		pr_err("failed to request IRQ:%d\n", gf_dev->irq);
		return status;
	}
	enable_irq_wake(gf_dev->irq);
	gf_dev->irq_enabled = 1;

	return status;
}

static void irq_cleanup(struct gf_dev *gf_dev)
{
	gf_dev->irq_enabled = 0;
	disable_irq(gf_dev->irq);
	disable_irq_wake(gf_dev->irq);
	free_irq(gf_dev->irq, gf_dev);
}

static void gf_kernel_key_input(struct gf_dev *gf_dev, struct gf_key *gf_key)
{
	uint32_t key_input = 0;

	if (gf_key->key == GF_KEY_HOME)
		key_input = KEY_KPENTER;
	else if (gf_key->key == GF_KEY_POWER)
		key_input = KEY_KPENTER;
	else if (gf_key->key == GF_KEY_CAMERA)
		key_input = KEY_CAMERA;
	else
		/* add special key define */
		key_input = gf_key->key;

	pr_debug("%s: received key event[%d], key=%d, value=%d\n",
	         __func__, key_input, gf_key->key, gf_key->value);

	if ((GF_KEY_POWER == gf_key->key || GF_KEY_CAMERA == gf_key->key)
			&& (gf_key->value == 1)) {
		input_report_key(gf_dev->input, key_input, 1);
		input_sync(gf_dev->input);
		input_report_key(gf_dev->input, key_input, 0);
		input_sync(gf_dev->input);
	}

	if (gf_key->key == GF_KEY_HOME) {
		input_report_key(gf_dev->input, key_input, gf_key->value);
		input_sync(gf_dev->input);
	}
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_dev *gf_dev = &gf;
	struct gf_key gf_key;
	int retval = 0;
	u8 netlink_route = NETLINK_TEST;

	switch (cmd) {
	case GF_IOC_INIT:
		if (copy_to_user((void __user *)arg, (void *)&netlink_route, sizeof(u8))) {
			pr_err("GF_IOC_INIT failed\n");
			retval = -EFAULT;
			break;
		}
		break;
	case GF_IOC_DISABLE_IRQ:
		if (gf_dev->irq_enabled) {
			disable_irq(gf_dev->irq);
			gf_dev->irq_enabled = 0;
		}
		break;
	case GF_IOC_ENABLE_IRQ:
		if (!gf_dev->irq_enabled) {
			enable_irq(gf_dev->irq);
			gf_dev->irq_enabled = 1;
		}
		break;
	case GF_IOC_RESET:
		gpio_direction_output(gf_dev->reset_gpio, 1);
		gpio_set_value(gf_dev->reset_gpio, 0);
		mdelay(3);
		gpio_set_value(gf_dev->reset_gpio, 1);
		mdelay(3);
		break;
	case GF_IOC_INPUT_KEY_EVENT:
		if (copy_from_user(&gf_key, (void __user *)arg, sizeof(struct gf_key))) {
			pr_err("failed to copy input key event from user to kernel\n");
			retval = -EFAULT;
			break;
		}

		gf_kernel_key_input(gf_dev, &gf_key);
		break;
        case GF_IOC_HAL_INITED_READY:
		gf_dev->device_available = 1;
		break;
	default:
		break;
	}

	return retval;
}

static void notification_work(struct work_struct *work)
{
	dsi_bridge_interface_enable(FP_UNLOCK_REJECTION_TIMEOUT);
}

static int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev = &gf;
	int status = -ENXIO;

	list_for_each_entry(gf_dev, &device_list, device_entry) {
		if (gf_dev->devt == inode->i_rdev) {
			status = 0;
			break;
		}
	}

	if (status == 0) {
		gf_dev->users++;
		filp->private_data = gf_dev;
		nonseekable_open(inode, filp);
		if (gf_dev->users == 1) {
			status = gf_parse_dts(gf_dev);
			if (status)
				return status;

			status = irq_setup(gf_dev);
			if (status)
				gf_cleanup(gf_dev);
		}

		if (gf_dev->irq_enabled) {
			disable_irq(gf_dev->irq);
			gf_dev->irq_enabled = 0;
		}
	} else {
		pr_info("No device for minor %d\n", iminor(inode));
	}

	return status;
}

static int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev = &gf;
	int status = 0;

	gf_dev = filp->private_data;
	filp->private_data = NULL;

	gf_dev->users--;
	if (!gf_dev->users) {
		irq_cleanup(gf_dev);
		gf_cleanup(gf_dev);
		gf_dev->device_available = 0;
	}

	return status;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = gf_ioctl,
	.open = gf_open,
	.release = gf_release,
};

static int gf_state_chg_cb(struct notifier_block *nb, unsigned long val, void *data)
{
	struct gf_dev *gf_dev = container_of(nb, struct gf_dev, notifier);
	struct msm_drm_notifier *evdata = data;
	unsigned int blank;
	char msg = 0;

	if (val != MSM_DRM_EVENT_BLANK && val != MSM_DRM_EARLY_EVENT_BLANK)
		return 0;

	if (evdata && evdata->data && val == MSM_DRM_EVENT_BLANK && gf_dev) {
		blank = *(int *)(evdata->data);
		switch (blank) {
		case MSM_DRM_BLANK_UNBLANK:
			if (gf_dev->device_available == 1) {
				gf_dev->drm_black = 0;
				msg = 3;
				sendnlmsg(&msg);
			}
			break;
		case MSM_DRM_BLANK_POWERDOWN:
			if (gf_dev->device_available == 1) {
				gf_dev->drm_black = 1;
				gf_dev->wait_finger_down = true;
				msg = 2;
				sendnlmsg(&msg);
			}
			break;
		default:
			break;
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block goodix_noti_block = {
	.notifier_call = gf_state_chg_cb,
};

static struct class *gf_class;
static int gf_probe(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;
	int status = -EINVAL;
	unsigned long minor;
	int i;

	/* Initialize the driver data */
	INIT_LIST_HEAD(&gf_dev->device_entry);
	gf_dev->spi = pdev;
	gf_dev->irq_gpio = -EINVAL;
	gf_dev->reset_gpio = -EINVAL;
	gf_dev->device_available = 0;
	gf_dev->drm_black = 0;
	gf_dev->wait_finger_down = false;
	INIT_WORK(&gf_dev->work, notification_work);

	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		struct device *dev;

		gf_dev->devt = MKDEV(SPIDEV_MAJOR, minor);
		dev = device_create(gf_class, &gf_dev->spi->dev, gf_dev->devt,
				gf_dev, GF_DEV_NAME);
		status = IS_ERR(dev) ? PTR_ERR(dev) : 0;
	} else {
		dev_dbg(&gf_dev->spi->dev, "no minor number available!\n");
		status = -ENODEV;
		gf_dev->device_available = 0;
		return status;
	}
      
	if (status == 0) {
		set_bit(minor, minors);
		list_add(&gf_dev->device_entry, &device_list);
	} else {
		gf_dev->devt = 0;
		gf_dev->device_available = 0;
		return status;
	}
  
	gf_dev->input = input_allocate_device();
	if (gf_dev->input == NULL) {
		pr_err("%s, failed to allocate input device\n", __func__);
		status = -ENOMEM;
		if (gf_dev->devt != 0) {
			pr_info("Err: status = %d\n", status);
			list_del(&gf_dev->device_entry);
			device_destroy(gf_class, gf_dev->devt);
			clear_bit(MINOR(gf_dev->devt), minors);
		}
	}

	for (i = 0; i < ARRAY_SIZE(maps); i++)
		input_set_capability(gf_dev->input, maps[i].type, maps[i].code);

	gf_dev->input->name = GF_INPUT_NAME;
	status = input_register_device(gf_dev->input);
	if (status) {
		pr_err("failed to register input device\n");
		if (gf_dev->input != NULL)
			input_free_device(gf_dev->input);
	}

	gf_dev->notifier = goodix_noti_block;
	msm_drm_register_client(&gf_dev->notifier);
	wakeup_source_init(&fp_ws, "fp_ws");

	return status;
}

static int gf_remove(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;

	wakeup_source_trash(&fp_ws);
	msm_drm_unregister_client(&gf_dev->notifier);

	if (gf_dev->input)
		input_unregister_device(gf_dev->input);

	input_free_device(gf_dev->input);

	/* prevent new opens */
	list_del(&gf_dev->device_entry);
	device_destroy(gf_class, gf_dev->devt);
	clear_bit(MINOR(gf_dev->devt), minors);

	return 0;
}

static const struct of_device_id gx_match_table[] = {
	{ .compatible = GF_SPIDEV_NAME },
	{ },
};

static struct platform_driver gf_driver = {
	.driver = {
		.name = GF_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = gx_match_table,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = gf_probe,
	.remove = gf_remove,
};

static int __init gf_init(void)
{
	int status;

	if (fpsensor != 2) {
		pr_err(" hml gf_init failed as fpsensor = %d(2=gdx)\n", fpsensor);
		return -1;
	}

	BUILD_BUG_ON(N_SPI_MINORS > 256);
	status = register_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME, &gf_fops);
	if (status < 0) {
		pr_warn("Failed to register char device!\n");
		return status;
	}

	SPIDEV_MAJOR = status;
	gf_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(gf_class)) {
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
		pr_warn("Failed to create class.\n");
		return PTR_ERR(gf_class);
	}

	status = platform_driver_register(&gf_driver);
	if (status < 0) {
		class_destroy(gf_class);
		unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
		pr_warn("Failed to register SPI driver.\n");
	}

	netlink_init();
	return 0;
}
module_init(gf_init);

static void __exit gf_exit(void)
{
	netlink_exit();
	platform_driver_unregister(&gf_driver);
	class_destroy(gf_class);
	unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
}
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_DESCRIPTION("goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");
