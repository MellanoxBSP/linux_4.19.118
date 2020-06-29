/*
 * drivers/net/ethernet/mellanox/mlxsw/qsfp_sysfs.c
 * Copyright (c) 2017 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2017 Vadim Pasternak <vadimp@mellanox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include "core.h"

#define MLXSW_QSFP_MAX_CPLD_NUM	3
#define MLXSW_QSFP_MIN_CPLD_NUM	1

static int mlxsw_qsfp_cpld_num = MLXSW_QSFP_MIN_CPLD_NUM;

struct mlxsw_qsfp {
	struct mlxsw_core *core;
	const struct mlxsw_bus_info *bus_info;
	struct attribute *cpld_attrs[MLXSW_QSFP_MAX_CPLD_NUM + 1];
	struct device_attribute *cpld_dev_attrs;
};

static ssize_t
mlxsw_qsfp_cpld_show(struct device *dev, struct device_attribute *attr,
		     char *buf)
{
	struct mlxsw_qsfp *mlxsw_qsfp = dev_get_platdata(dev);
	char msci_pl[MLXSW_REG_MSCI_LEN];
	u32 version, i;
	int err;

	for (i = 0; i < mlxsw_qsfp_cpld_num; i++) {
		if ((mlxsw_qsfp->cpld_dev_attrs + i) == attr)
			break;
	}
	if (i == mlxsw_qsfp_cpld_num)
		return -EINVAL;

	mlxsw_reg_msci_pack(msci_pl, i);
	err = mlxsw_reg_query(mlxsw_qsfp->core, MLXSW_REG(msci), msci_pl);
	if (err)
		return err;

	version = mlxsw_reg_msci_version_get(msci_pl);

	return sprintf(buf, "%u\n", version);
}

static int mlxsw_qsfp_dmi_set_cpld_num(const struct dmi_system_id *dmi)
{
	mlxsw_qsfp_cpld_num = MLXSW_QSFP_MAX_CPLD_NUM;

	return 1;
};

static const struct dmi_system_id mlxsw_qsfp_dmi_table[] = {
	{
		.callback = mlxsw_qsfp_dmi_set_cpld_num,
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Mellanox Technologies"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MSN24"),
		},
	},
	{
		.callback = mlxsw_qsfp_dmi_set_cpld_num,
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Mellanox Technologies"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MSN27"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, mlxsw_qsfp_dmi_table);


int mlxsw_qsfp_init(struct mlxsw_core *mlxsw_core,
		    const struct mlxsw_bus_info *mlxsw_bus_info,
		    struct mlxsw_qsfp **p_qsfp)
{
	struct device_attribute *cpld_dev_attr;
	struct mlxsw_qsfp *mlxsw_qsfp;
	int i, err;

	if (!strcmp(mlxsw_bus_info->device_kind, "i2c"))
		return 0;

	dmi_check_system(mlxsw_qsfp_dmi_table);

	mlxsw_qsfp = devm_kzalloc(mlxsw_bus_info->dev, sizeof(*mlxsw_qsfp),
				  GFP_KERNEL);
	if (!mlxsw_qsfp)
		return -ENOMEM;

	mlxsw_qsfp->core = mlxsw_core;
	mlxsw_qsfp->bus_info = mlxsw_bus_info;
	mlxsw_bus_info->dev->platform_data = mlxsw_qsfp;
	mlxsw_qsfp->cpld_dev_attrs = devm_kzalloc(mlxsw_bus_info->dev,
					mlxsw_qsfp_cpld_num *
					sizeof(*mlxsw_qsfp->cpld_dev_attrs),
					GFP_KERNEL);
	if (!mlxsw_qsfp->cpld_dev_attrs)
		return -ENOMEM;

	cpld_dev_attr = mlxsw_qsfp->cpld_dev_attrs;
	for (i = 0; i < mlxsw_qsfp_cpld_num; i++, cpld_dev_attr++) {
		cpld_dev_attr->show = mlxsw_qsfp_cpld_show;
		cpld_dev_attr->attr.mode = 0444;
		cpld_dev_attr->attr.name = devm_kasprintf(mlxsw_bus_info->dev,
						     GFP_KERNEL,
						     "cpld%d_version", i + 1);
		mlxsw_qsfp->cpld_attrs[i] = &cpld_dev_attr->attr;
		sysfs_attr_init(&cpld_dev_attr->attr);
		err = sysfs_create_file(&mlxsw_bus_info->dev->kobj,
					mlxsw_qsfp->cpld_attrs[i]);
		if (err)
			goto err_create_cpld_file;
	}

	*p_qsfp = mlxsw_qsfp;

	return 0;

err_create_cpld_file:
	while (--i > 0)
		sysfs_remove_file(&mlxsw_bus_info->dev->kobj,
				  mlxsw_qsfp->cpld_attrs[i]);

	return err;
}

void mlxsw_qsfp_fini(struct mlxsw_qsfp *mlxsw_qsfp)
{
	int i;

	for (i = 0; i < mlxsw_qsfp_cpld_num; i++)
		sysfs_remove_file(&mlxsw_qsfp->bus_info->dev->kobj,
				  mlxsw_qsfp->cpld_attrs[i]);
	devm_kfree(mlxsw_qsfp->bus_info->dev, mlxsw_qsfp->cpld_dev_attrs);
	devm_kfree(mlxsw_qsfp->bus_info->dev, mlxsw_qsfp);
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Vadim Pasternak <vadimp@mellanox.com>");
MODULE_DESCRIPTION("Mellanox switch QSFP sysfs driver");
