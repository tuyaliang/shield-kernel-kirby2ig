/*
 * Copyright (c) 2006-2013, Cypress Semiconductor Corporation
 * All rights reserved.
 *
 * Copyright (c) 2013, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/ihex.h>
#include <linux/gpio.h>
#include <linux/wakelock.h>
#include <linux/delay.h>
#include <linux/mutex.h>

#include "issp_priv.h"

#define DRIVER_NAME "issp"

struct issp_host *g_issp_host;
struct wake_lock *g_issp_wake_lock;

static int issp_check_fw(struct issp_host *host)
{
	const struct ihex_binrec *rec;
	int checks = 0;
	int size;

	size = host->pdata->block_size * host->pdata->blocks;
	rec = (const struct ihex_binrec *)host->fw->data;
	while (rec) {
		int addr, len;

		addr = be32_to_cpu(rec->addr);
		len = be16_to_cpu(rec->len);

		if (addr + len == size)
			checks++;

		if (addr == ISSP_FW_SECURITY_ADDR) {
			host->security_rec = rec;
			checks++;
		}

		if (addr == ISSP_FW_CHECKSUM_ADDR) {
			host->checksum_fw = rec->data[0] << 8 | rec->data[1];
			checks++;
		}

		if (host->pdata->version_addr > addr &&
			host->pdata->version_addr < addr + len) {
			host->version_fw =
				rec->data[host->pdata->version_addr - addr];
			checks++;
		}

		if (checks == 4)
			break;
		rec = ihex_next_binrec(rec);
	}

	if (checks < 4)
		return -EINVAL;
	else
		return 0;
}

void issp_fw_rewind(struct issp_host *host)
{
	host->cur_rec = (const struct ihex_binrec *)host->fw->data;
	host->cur_idx = 0;
}

void issp_fw_seek_security(struct issp_host *host)
{
	host->cur_rec = host->security_rec;
	host->cur_idx = 0;
}

uint8_t issp_fw_get_byte(struct issp_host *host)
{
	uint8_t byte;
	byte = host->cur_rec->data[host->cur_idx];
	host->cur_idx++;
	if (host->cur_idx >= be16_to_cpu(host->cur_rec->len)) {
		host->cur_rec = ihex_next_binrec(host->cur_rec);
		host->cur_idx = 0;
	}

	return byte;
}

static int issp_need_update(struct issp_host *host, bool *update)
{
	uint8_t idx, addr, ver_uc;
	int ret;

	idx = host->pdata->version_addr / host->pdata->block_size;
	addr = host->pdata->version_addr - idx * host->pdata->block_size;
	ret = issp_read_block(host, idx, addr, &ver_uc, 1);
	if (ret == -EACCES) {
		dev_err(&host->pdev->dev,
			"Version Block is protected, force upgrade!\n");
		*update = true;
	} else if (ret == 1) {
		*update = (ver_uc < host->version_fw) ||
				((ver_uc != host->version_fw) &&
				host->pdata->force_update);

		if (*update)
			dev_info(&host->pdev->dev, "firmware needs upgrade, "\
				"version 0x%02x -> 0x%02x\n",
				ver_uc, host->version_fw);
		else
			dev_info(&host->pdev->dev,
				"firmware version %02x is latest!\n", ver_uc);
	} else
		return ret;

	return 0;
}

struct issp_host *g_issp_host;
extern void roth_usb_unload(void);
extern void roth_usb_reload(void);

#define ISSP_RECOVERY_DELAY 10

struct workqueue_struct *issp_workqueue;
struct delayed_work issp_recovery_work;

static void issp_recovery_work_func(struct work_struct *work)
{
	int i;
	extern void roth_usb_unload(void);
	extern void roth_usb_reload(void);

	dev_info(&g_issp_host->pdev->dev, "%s\n", __func__);
	if (!g_issp_wake_lock) {
		dev_err(&g_issp_host->pdev->dev,
				"%s: wake_lock null!!\n", __func__);
		return;
	}

	for (i = 0; i < 1; i++) {
		dev_info(&g_issp_host->pdev->dev,
				"%s: recovery attempt #%d\n", __func__, i);
		mutex_lock(&g_issp_host->issp_lock);
		roth_usb_unload();
		issp_uc_reset();
		roth_usb_reload();
		mutex_unlock(&g_issp_host->issp_lock);
		msleep(500);
	}

	/*Done resetting JS, lets release the Wakelock*/
	wake_unlock(g_issp_wake_lock);
}

void issp_start_recovery_work(void)
{
	dev_info(&g_issp_host->pdev->dev, "%s\n", __func__);
	if (!issp_workqueue) {
		dev_err(&g_issp_host->pdev->dev,
				"%s: no workqueue!\n", __func__);
		return;
	}

	/*Hold wakelock, so we can be sure that JS resets!*/
	wake_lock(g_issp_wake_lock);
	queue_delayed_work(issp_workqueue, &issp_recovery_work,
	msecs_to_jiffies(ISSP_RECOVERY_DELAY));

}

static ssize_t issp_reset_set(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count) {
	wake_lock(g_issp_wake_lock);
	mutex_lock(&g_issp_host->issp_lock);
	issp_uc_reset();
	mutex_unlock(&g_issp_host->issp_lock);
	wake_unlock(g_issp_wake_lock);
	dev_info(&g_issp_host->pdev->dev,
			"issp: toggling reset pin on uC!");
	return count;
}

static ssize_t issp_usbreset_set(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count) {
	wake_lock(g_issp_wake_lock);
	mutex_lock(&g_issp_host->issp_lock);
	roth_usb_unload();
	issp_uc_reset();
	roth_usb_reload();
	mutex_unlock(&g_issp_host->issp_lock);
	wake_unlock(g_issp_wake_lock);
	dev_info(&g_issp_host->pdev->dev,
			"issp: reset both usb and uC!");
	return count;
}

static ssize_t issp_data_set(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count) {
	int val;
	struct issp_platform_data *pdata = dev->platform_data;

	if (!kstrtoul(buf, 10, &val)) {
		if (val == 1 || val == 0) {
			gpio_set_value(pdata->data_gpio, val);
			dev_err(dev, "issp: set data gpio to %d", val);
		}
	}
	return count;
}

static ssize_t issp_data_show(struct device *dev, struct device_attribute *attr,
		char *buf)  {
	unsigned int val;
	struct issp_platform_data *pdata = dev->platform_data;
	val = gpio_get_value(pdata->data_gpio);

	return sprintf(buf, "%u\n", val);
}


static ssize_t issp_clk_set(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count) {
	int val;
	struct issp_platform_data *pdata = dev->platform_data;

	if (!kstrtoul(buf, 10, &val)) {
		if (val == 1 || val == 0) {
			gpio_set_value(pdata->clk_gpio, val);
			dev_err("issp: set clk gpio to %d", val);
		}
	}
	return count;
}

static ssize_t issp_clk_show(struct device *dev, struct device_attribute *attr,
		char *buf)  {
	unsigned int val;
	struct issp_platform_data *pdata = dev->platform_data;
	val = gpio_get_value(pdata->clk_gpio);

	return sprintf(buf, "%u\n", val);
}

static DEVICE_ATTR(issp_reset, S_IWGRP|S_IWUSR, NULL, issp_reset_set);
static DEVICE_ATTR(issp_usbreset, S_IWGRP|S_IWUSR, NULL, issp_usbreset_set);
static DEVICE_ATTR(issp_data, S_IWGRP|S_IWUSR|S_IRGRP|S_IRUSR,
		issp_data_show, issp_data_set);
static DEVICE_ATTR(issp_clk, S_IWGRP|S_IWUSR|S_IRGRP|S_IRUSR,
		issp_clk_show, issp_clk_set);

static int __init issp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct issp_platform_data *pdata = dev->platform_data;
	struct issp_host *host;
	bool update;
	int ret;

	if (!pdata || !gpio_is_valid(pdata->reset_gpio)
		|| !gpio_is_valid(pdata->data_gpio)
		|| !gpio_is_valid(pdata->clk_gpio)) {
		dev_err(dev, "Invalid platform data!");
		return -EINVAL;
	}

	ret = devm_gpio_request(dev, pdata->reset_gpio, "issp reset");
	if (!ret)
		ret = devm_gpio_request(dev, pdata->data_gpio, "issp data");
	if (!ret)
		ret = devm_gpio_request(dev, pdata->clk_gpio, "issp clock");
	if (ret)
		return ret;

	/* set gpio directions */
	gpio_direction_output(pdata->reset_gpio, 0);
	gpio_direction_input(pdata->data_gpio);
	gpio_direction_output(pdata->clk_gpio, 0);

	host = devm_kzalloc(dev, sizeof(*host), GFP_KERNEL);
	if (!host)
		return -ENOMEM;

	host->pdev = pdev;
	host->pdata = pdata;
	g_issp_host = host;
	ret = request_ihex_firmware(&host->fw, pdata->fw_name, dev);
	if (ret) {
		dev_err(dev, "Request firmware %s failed!\n", pdata->fw_name);
		return ret;
	}

	ret = issp_check_fw(host);
	if (ret) {
		dev_err(dev, "Firmware %s invalid!\n", pdata->fw_name);
		goto err;
	}

	issp_uc_program(host);

	if (host->si_id[0] != pdata->si_id[0] ||
		host->si_id[1] != pdata->si_id[1] ||
		host->si_id[2] != pdata->si_id[2] ||
		host->si_id[3] != pdata->si_id[3]) {
		dev_err(dev, "Sillicon ID check failed!\n");
		goto err_id;
	}

	ret = issp_need_update(host, &update);
	if (ret)
		goto err_id;
	if (update) {
		ret = issp_program(host);
		if (!ret)
			dev_info(dev, "Firmware update successfully!\n");
		else
			dev_err(dev, "Firmware update failed!\n");
	}

	mutex_init(&host->issp_lock);
	ret = device_create_file(dev, &dev_attr_issp_reset);
	if (ret)
		dev_err(dev, "ISSP sysfs node create failed\n");

	ret = device_create_file(dev, &dev_attr_issp_usbreset);
	if (ret)
		dev_err(dev, "ISSP sysfs node create failed\n");

	ret = device_create_file(dev, &dev_attr_issp_data);
	if (ret)
		dev_err(dev, "ISSP sysfs node create failed\n");

	ret = device_create_file(dev, &dev_attr_issp_clk);
	if (ret)
		dev_err(dev, "ISSP sysfs node create failed\n");

	g_issp_wake_lock = devm_kzalloc(dev, sizeof(struct wake_lock),
								GFP_KERNEL);
	if (!g_issp_wake_lock)
		goto err;

	/*Wake lock to prevent suspend when USB is deregistered! and recovery
	of JS is happening!*/
	wake_lock_init(g_issp_wake_lock, WAKE_LOCK_SUSPEND,
							"issp-js-recovery");

	/* create workqueue to recover from failed usb resume */
	issp_workqueue = create_workqueue("issp_recovery_wq");
	if (!issp_workqueue) {
		dev_err(&pdev->dev, "can't create work queue\n");
		goto err_work;
	}
	INIT_DELAYED_WORK(&issp_recovery_work, issp_recovery_work_func);

err_id:
	issp_uc_run(host);
	gpio_direction_input(pdata->data_gpio);
	gpio_direction_input(pdata->clk_gpio);
	release_firmware(host->fw);
	return 0;

err_work:
	devm_kfree(dev, g_issp_wake_lock);
err:
	gpio_direction_input(pdata->data_gpio);
	gpio_direction_input(pdata->clk_gpio);
	release_firmware(host->fw);
	devm_kfree(dev, host);
	return -ENOMEM;
}

static int __exit issp_remove(struct platform_device *pdev)
{
	g_issp_host = NULL;

	/* delete workqueue used to recover from failed usb resume */
	if (issp_workqueue) {
		destroy_workqueue(issp_workqueue);
		issp_workqueue = NULL;
	}

	if (g_issp_wake_lock != NULL)
		wake_lock_destroy(g_issp_wake_lock);

	return 0;
}

static struct platform_driver issp_driver = {
	.remove		= __exit_p(issp_remove),
	.driver	= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
};

static int __init issp_init(void)
{
	return platform_driver_probe(&issp_driver, issp_probe);
}
subsys_initcall(issp_init);

static void __exit issp_exit(void)
{
	platform_driver_unregister(&issp_driver);
}
module_exit(issp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Richard Zhao, nVidia <rizhao@nvidia.com>");
MODULE_DESCRIPTION("ISSP driver");
