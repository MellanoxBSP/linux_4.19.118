// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for MPS Multi-phase Digital VR Controllers
 *
 * Copyright (C) 2020 Nvidia Technologies Ltd.
 */

#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include "pmbus.h"

/* Vendor specific registers. */
#define MP2888_MFR_SYS_CONFIG	0x44
#define MP2888_MFR_VR_CONFIG1	0xe1

#define MP2888_TOTAL_CURRENT_RESOLUTION	BIT(3)
#define MP2888_VIN_LIMIT_UNIT		8
#define MP2888_VIN_UNIT			3125
#define MP2888_TEMP_UNIT		10
#define MP2888_MAX_PHASE		10

struct mp2888_data {
	struct pmbus_driver_info info;
	int curr_sense_gain;
	int total_curr_resolution;
};

#define to_mp2888_data(x)  container_of(x, struct mp2888_data, info)

static int mp2888_read_byte_data(struct i2c_client *client, int page, int reg)
{
	switch (reg) {
	case PMBUS_VOUT_MODE:
		/* Enforce VOUT direct format. */
		return PB_VOUT_MODE_DIRECT;
	default:
		return -ENODATA;
	}
}

static int
mp2888_read_word_helper(struct i2c_client *client, int page, u8 reg, u16 mask)
{
	int ret = pmbus_read_word_data(client, page, reg);

	return (ret > 0) ? ret & mask : ret;
}

static int mp2888_read_word_data(struct i2c_client *client, int page, int reg)
{
	const struct pmbus_driver_info *info = pmbus_get_driver_info(client);
	struct mp2888_data *data = to_mp2888_data(info);
	int ret;

	switch (reg) {
	case PMBUS_OT_WARN_LIMIT:
		ret = mp2888_read_word_helper(client, page, reg, GENMASK(7, 0));
		break;
	case PMBUS_VIN_OV_FAULT_LIMIT:
	case PMBUS_VIN_UV_WARN_LIMIT:
		ret = mp2888_read_word_helper(client, page, reg, GENMASK(7, 0));
		ret = (ret < 0) ? ret : DIV_ROUND_CLOSEST(ret, MP2888_VIN_LIMIT_UNIT);
		break;
	case PMBUS_READ_VIN:
		ret = mp2888_read_word_helper(client, page, reg, GENMASK(9, 0));
		ret = (ret < 0) ? ret : DIV_ROUND_CLOSEST(ret * MP2888_VIN_UNIT, 100000);
		break;
	case PMBUS_READ_VOUT:
		ret = mp2888_read_word_helper(client, page, reg, GENMASK(11, 0));
		break;
	case PMBUS_READ_TEMPERATURE_1:
		ret = mp2888_read_word_helper(client, page, reg, GENMASK(11, 0));
		ret = (ret < 0) ? ret : DIV_ROUND_CLOSEST(ret, MP2888_TEMP_UNIT);
		break;
	case PMBUS_READ_IOUT:
		ret = mp2888_read_word_helper(client, page, reg, GENMASK(11, 0));
		ret = (ret < 0) ? ret : data->total_curr_resolution ? DIV_ROUND_CLOSEST(ret, 2) :
		      DIV_ROUND_CLOSEST(ret, 4);
		break;
	case PMBUS_READ_POUT:
	case PMBUS_READ_PIN:
		ret = mp2888_read_word_helper(client, page, reg, GENMASK(11, 0));
		ret = (ret < 0) ? ret : data->total_curr_resolution ? ret :
		      DIV_ROUND_CLOSEST(ret, 2);
		break;
	case PMBUS_OT_FAULT_LIMIT:
	case PMBUS_UT_WARN_LIMIT:
	case PMBUS_UT_FAULT_LIMIT:
	case PMBUS_VIN_UV_FAULT_LIMIT:
	case PMBUS_VOUT_UV_WARN_LIMIT:
	case PMBUS_VOUT_OV_WARN_LIMIT:
	case PMBUS_VOUT_UV_FAULT_LIMIT:
	case PMBUS_VOUT_OV_FAULT_LIMIT:
	case PMBUS_VIN_OV_WARN_LIMIT:
	case PMBUS_IOUT_OC_LV_FAULT_LIMIT:
	case PMBUS_IOUT_OC_WARN_LIMIT:
	case PMBUS_IOUT_OC_FAULT_LIMIT:
	case PMBUS_IOUT_UC_FAULT_LIMIT:
	case PMBUS_POUT_OP_FAULT_LIMIT:
	case PMBUS_POUT_OP_WARN_LIMIT:
	case PMBUS_PIN_OP_WARN_LIMIT:
		return -ENXIO;
	default:
		return -ENODATA;
	}

	return ret;
}

static int
mp2888_identify_multiphase(struct i2c_client *client, struct mp2888_data *data,
			   struct pmbus_driver_info *info)
{
	int ret;

	ret = i2c_smbus_write_byte_data(client, PMBUS_PAGE, 0);
	if (ret < 0)
		return ret;

	return 0;
}

static int mp2888_current_resolution_get(struct i2c_client *client, struct mp2888_data *data)
{
	int ret;

	/*
	 * Obtain resolution selector for total current report and protection.
	 * 0: original resolution; 1: half resolution (in such case phase current value should
	 * be doubled.
	 */
	ret = i2c_smbus_read_word_data(client, MP2888_MFR_SYS_CONFIG);
	if (ret < 0)
		return ret;
	data->total_curr_resolution = (ret & MP2888_TOTAL_CURRENT_RESOLUTION) >> 3;

	return 0;
}

static struct pmbus_driver_info mp2888_info = {
	.pages = 1,
	.format[PSC_VOLTAGE_IN] = linear,
	.format[PSC_VOLTAGE_OUT] = direct,
	.format[PSC_TEMPERATURE] = direct,
	.format[PSC_CURRENT_IN] = linear,
	.format[PSC_CURRENT_OUT] = direct,
	.format[PSC_POWER] = direct,
	.m[PSC_TEMPERATURE] = 1,
	.m[PSC_VOLTAGE_OUT] = 1,
	.R[PSC_VOLTAGE_OUT] = 3,
	.m[PSC_CURRENT_OUT] = 1,
	.m[PSC_POWER] = 1,
	.func[0] = PMBUS_HAVE_VIN | PMBUS_HAVE_VOUT | PMBUS_HAVE_STATUS_VOUT | PMBUS_HAVE_IOUT |
		   PMBUS_HAVE_STATUS_IOUT | PMBUS_HAVE_TEMP | PMBUS_HAVE_STATUS_TEMP |
		   PMBUS_HAVE_POUT | PMBUS_HAVE_PIN | PMBUS_HAVE_STATUS_INPUT,
	.read_byte_data = mp2888_read_byte_data,
	.read_word_data = mp2888_read_word_data,
};

static int mp2888_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct pmbus_driver_info *info;
	struct mp2888_data *data;
	int ret;

	data = devm_kzalloc(&client->dev, sizeof(struct mp2888_data),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memcpy(&data->info, &mp2888_info, sizeof(*info));
	info = &data->info;

	/* Identify multiphase configuration. */
	ret = mp2888_identify_multiphase(client, data, info);
	if (ret)
		return ret;

	/* Obtain total current resolution. */
	ret = mp2888_current_resolution_get(client, data);
	if (ret)
		return ret;

	return pmbus_do_probe(client, id, info);
}

static const struct i2c_device_id mp2888_id[] = {
	{"mp2888", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, mp2888_id);

static const struct of_device_id __maybe_unused mp2888_of_match[] = {
	{.compatible = "mps,mp2888"},
	{}
};
MODULE_DEVICE_TABLE(of, mp2888_of_match);

static struct i2c_driver mp2888_driver = {
	.driver = {
		.name = "mp2888",
		.of_match_table = of_match_ptr(mp2888_of_match),
	},
	.probe = mp2888_probe,
	.remove = pmbus_do_remove,
	.id_table = mp2888_id,
};

module_i2c_driver(mp2888_driver);

MODULE_AUTHOR("Vadim Pasternak <vadimp@nvidia.com>");
MODULE_DESCRIPTION("PMBus driver for MPS MP2888 device");
MODULE_LICENSE("GPL");
