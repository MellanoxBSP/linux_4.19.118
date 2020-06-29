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

#define MLXSW_QSFP_I2C_ADDR		0x50
#define MLXSW_QSFP_PAGE_NUM		5
#define MLXSW_QSFP_PAGE_SIZE		128
#define MLXSW_QSFP_SUB_PAGE_NUM		3
#define MLXSW_QSFP_SUB_PAGE_SIZE	48
#define MLXSW_QSFP_LAST_SUB_PAGE_SIZE	32
#define MLXSW_QSFP_MAX_NUM		128
#define MLXSW_QSFP_MIN_REQ_LEN		4
#define MLXSW_QSFP_STATUS_VALID_TIME	(HZ)
#define MLXSW_QSFP_MAX_CPLD_NUM		3
#define MLXSW_QSFP_MIN_CPLD_NUM		1

static const u8 mlxsw_qsfp_page_number[] = { 0xa0, 0x00, 0x01, 0x02, 0x03 };
static const u16 mlxsw_qsfp_page_shift[] = { 0x00, 0x80, 0x80, 0x80, 0x80 };

/**
 * Mellanox device Management Cable Info Access Register buffer for reading
 * QSFP EEPROM info is limited by 48 bytes. In case full page is to be read
 * (128 bytes), such request will be implemented by three transactions of size
 * 48, 48, 32.
 */
static const u16 mlxsw_qsfp_sub_page_size[] = {
	MLXSW_QSFP_SUB_PAGE_SIZE,
	MLXSW_QSFP_SUB_PAGE_SIZE,
	MLXSW_QSFP_LAST_SUB_PAGE_SIZE
};

struct mlxsw_qsfp_module {
	unsigned long last_updated;
	u8 cache_status;
};

struct mlxsw_qsfp {
	struct mlxsw_core *core;
	const struct mlxsw_bus_info *bus_info;
	struct attribute *attrs[MLXSW_QSFP_MAX_NUM + 1];
	struct device_attribute *dev_attrs;
	struct bin_attribute *eeprom;
	struct bin_attribute **eeprom_attr_list;
	struct mlxsw_qsfp_module modules[MLXSW_QSFP_MAX_NUM];
	u8 module_ind[MLXSW_QSFP_MAX_NUM];
	u8 module_count;
	struct attribute *cpld_attrs[MLXSW_QSFP_MAX_CPLD_NUM + 1];
	struct device_attribute *cpld_dev_attrs;
};

static int mlxsw_qsfp_cpld_num = MLXSW_QSFP_MIN_CPLD_NUM;
static int mlxsw_qsfp_num = MLXSW_QSFP_MAX_NUM / 2;

static int
mlxsw_qsfp_query_module_eeprom(struct mlxsw_qsfp *mlxsw_qsfp, u8 index,
			       loff_t off, size_t count, int page, char *buf)
{
	char eeprom_tmp[MLXSW_QSFP_PAGE_SIZE];
	char mcia_pl[MLXSW_REG_MCIA_LEN];
	int status;
	int err;

	mlxsw_reg_mcia_pack(mcia_pl, index, 0, page, off, count,
			    MLXSW_QSFP_I2C_ADDR);

	err = mlxsw_reg_query(mlxsw_qsfp->core, MLXSW_REG(mcia), mcia_pl);
	if (err)
		return err;

	status = mlxsw_reg_mcia_status_get(mcia_pl);
	if (status)
		return -EIO;

	mlxsw_reg_mcia_eeprom_memcpy_from(mcia_pl, eeprom_tmp);
	memcpy(buf, eeprom_tmp, count);

	return 0;
}

static int
mlxsw_qsfp_get_module_eeprom(struct mlxsw_qsfp *mlxsw_qsfp, u8 index,
			     char *buf, loff_t off, size_t count)
{
	int page_ind, page, page_off, subpage, offset, size, res = 0;
	int err;

	if (!count)
		return -EINVAL;

	memset(buf, 0, count);
	size = count;
	while (res < count) {
		page_ind = off / MLXSW_QSFP_PAGE_SIZE;
		page_off = off % MLXSW_QSFP_PAGE_SIZE;
		page = mlxsw_qsfp_page_number[page_ind];
		offset = mlxsw_qsfp_page_shift[page_ind] + page_off;
		subpage = page_off / MLXSW_QSFP_SUB_PAGE_SIZE;
		size = min_t(u16, size, mlxsw_qsfp_sub_page_size[subpage]);
		err = mlxsw_qsfp_query_module_eeprom(mlxsw_qsfp, index, offset,
						     size, page, buf + res);
		if (err) {
			dev_err(mlxsw_qsfp->bus_info->dev, "Eeprom query failed\n");
			return err;
		}
		off += size;
		res += size;
		size = count - size;
	}

	return res;
}

static ssize_t mlxsw_qsfp_bin_read(struct file *filp, struct kobject *kobj,
				   struct bin_attribute *attr, char *buf,
				   loff_t off, size_t count)
{
	struct mlxsw_qsfp *mlxsw_qsfp = dev_get_platdata(container_of(kobj,
							 struct device, kobj));
	u8 *module_ind = attr->private;
	size_t size;

	size = mlxsw_qsfp->eeprom[*module_ind].size;

	if (off > size)
		return -ESPIPE;
	else if (off == size)
		return 0;
	else if ((off + count) > size)
		count = size - off;

	return mlxsw_qsfp_get_module_eeprom(mlxsw_qsfp, *module_ind, buf, off,
					    count);
}

static ssize_t
mlxsw_qsfp_status_show(struct device *dev, struct device_attribute *attr,
		       char *buf)
{
	struct mlxsw_qsfp *mlxsw_qsfp = dev_get_platdata(dev);
	char mcia_pl[MLXSW_REG_MCIA_LEN];
	int status;
	u32 i;
	int err;

	for (i = 0; i < mlxsw_qsfp->module_count; i++) {
		if ((mlxsw_qsfp->dev_attrs + i) == attr)
			break;
	}
	if (i == mlxsw_qsfp->module_count)
		return -EINVAL;

	if (time_before(jiffies, mlxsw_qsfp->modules[i].last_updated +
			MLXSW_QSFP_STATUS_VALID_TIME))
		return sprintf(buf, "%u\n",
			       mlxsw_qsfp->modules[i].cache_status);

	mlxsw_reg_mcia_pack(mcia_pl, i, 0, 0, 0, MLXSW_QSFP_MIN_REQ_LEN,
			    MLXSW_QSFP_I2C_ADDR);
	err = mlxsw_reg_query(mlxsw_qsfp->core, MLXSW_REG(mcia), mcia_pl);
	if (err)
		return err;

	status = mlxsw_reg_mcia_status_get(mcia_pl);
	mlxsw_qsfp->modules[i].cache_status = !status;
	mlxsw_qsfp->modules[i].last_updated = jiffies;

	return sprintf(buf, "%u\n", !status);
}

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

static int mlxsw_qsfp_dmi_set_qsfp_num(const struct dmi_system_id *dmi)
{
	mlxsw_qsfp_num = MLXSW_QSFP_MAX_NUM;

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
	{
		.callback = mlxsw_qsfp_dmi_set_qsfp_num,
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Mellanox Technologies"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MSN37"),
		},
	},
	{
		.callback = mlxsw_qsfp_dmi_set_qsfp_num,
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "Mellanox Technologies"),
			DMI_MATCH(DMI_PRODUCT_NAME, "MSN38"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, mlxsw_qsfp_dmi_table);

static int mlxsw_qsfp_set_module_num(struct mlxsw_qsfp *mlxsw_qsfp)
{
	char pmlp_pl[MLXSW_REG_PMLP_LEN];
	u8 width;
	int i, err;

	for (i = 1; i <= mlxsw_qsfp_num; i++) {
		mlxsw_reg_pmlp_pack(pmlp_pl, i);
		err = mlxsw_reg_query(mlxsw_qsfp->core, MLXSW_REG(pmlp),
				      pmlp_pl);
		if (err)
			return err;
		width = mlxsw_reg_pmlp_width_get(pmlp_pl);
		if (!width)
			continue;
		mlxsw_qsfp->module_count++;
	}

	return 0;
}

int mlxsw_qsfp_init(struct mlxsw_core *mlxsw_core,
		    const struct mlxsw_bus_info *mlxsw_bus_info,
		    struct mlxsw_qsfp **p_qsfp)
{
	struct device_attribute *dev_attr, *cpld_dev_attr;
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	struct mlxsw_qsfp *mlxsw_qsfp;
	struct bin_attribute *eeprom;
	int i, count;
	int err;

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

	mlxsw_reg_mgpir_pack(mgpir_pl);
	err = mlxsw_reg_query(mlxsw_qsfp->core, MLXSW_REG(mgpir), mgpir_pl);
	if (err) {
		err = mlxsw_qsfp_set_module_num(mlxsw_qsfp);
		if (err)
			return err;
	} else {
		mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
			       &mlxsw_qsfp->module_count);
		if (!mlxsw_qsfp->module_count)
			return 0;
	}

	count = mlxsw_qsfp->module_count + 1;
	mlxsw_qsfp->eeprom = devm_kzalloc(mlxsw_bus_info->dev,
					  mlxsw_qsfp->module_count *
					  sizeof(*mlxsw_qsfp->eeprom),
					  GFP_KERNEL);
	if (!mlxsw_qsfp->eeprom)
		return -ENOMEM;

	mlxsw_qsfp->eeprom_attr_list = devm_kzalloc(mlxsw_bus_info->dev,
						    count *
						    sizeof(mlxsw_qsfp->eeprom),
						    GFP_KERNEL);
	if (!mlxsw_qsfp->eeprom_attr_list)
		return -ENOMEM;

	mlxsw_qsfp->dev_attrs = devm_kzalloc(mlxsw_bus_info->dev, count *
					     sizeof(*mlxsw_qsfp->dev_attrs),
					     GFP_KERNEL);
	if (!mlxsw_qsfp->dev_attrs)
		return -ENOMEM;

	mlxsw_qsfp->cpld_dev_attrs = devm_kzalloc(mlxsw_bus_info->dev,
					mlxsw_qsfp_cpld_num *
					sizeof(*mlxsw_qsfp->cpld_dev_attrs),
					GFP_KERNEL);
	if (!mlxsw_qsfp->cpld_dev_attrs)
		return -ENOMEM;

	eeprom = mlxsw_qsfp->eeprom;
	dev_attr = mlxsw_qsfp->dev_attrs;
	for (i = 0; i < mlxsw_qsfp->module_count; i++, eeprom++, dev_attr++) {
		dev_attr->show = mlxsw_qsfp_status_show;
		dev_attr->attr.mode = 0444;
		dev_attr->attr.name = devm_kasprintf(mlxsw_bus_info->dev,
						     GFP_KERNEL,
						     "qsfp%d_status", i + 1);
		mlxsw_qsfp->attrs[i] = &dev_attr->attr;
		sysfs_attr_init(&dev_attr->attr);
		err = sysfs_create_file(&mlxsw_bus_info->dev->kobj,
					mlxsw_qsfp->attrs[i]);
		if (err)
			goto err_create_file;

		sysfs_bin_attr_init(eeprom);
		eeprom->attr.name = devm_kasprintf(mlxsw_bus_info->dev,
						   GFP_KERNEL, "qsfp%d",
						   i + 1);
		eeprom->attr.mode = 0444;
		eeprom->read = mlxsw_qsfp_bin_read;
		eeprom->size = MLXSW_QSFP_PAGE_NUM * MLXSW_QSFP_PAGE_SIZE;
		mlxsw_qsfp->module_ind[i] = i;
		eeprom->private = &mlxsw_qsfp->module_ind[i];
		mlxsw_qsfp->eeprom_attr_list[i] = eeprom;
		err = sysfs_create_bin_file(&mlxsw_bus_info->dev->kobj,
					    eeprom);
		if (err)
			goto err_create_bin_file;
	}

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
	sysfs_remove_file(&mlxsw_bus_info->dev->kobj,
			  mlxsw_qsfp->cpld_attrs[i--]);
	i = mlxsw_qsfp->module_count;
err_create_bin_file:
	sysfs_remove_file(&mlxsw_bus_info->dev->kobj,
			  mlxsw_qsfp->attrs[i--]);
err_create_file:
	while (--i > 0) {
		sysfs_remove_bin_file(&mlxsw_bus_info->dev->kobj,
				      mlxsw_qsfp->eeprom_attr_list[i]);
		sysfs_remove_file(&mlxsw_bus_info->dev->kobj,
				  mlxsw_qsfp->attrs[i]);
	}

	return err;
}

void mlxsw_qsfp_fini(struct mlxsw_qsfp *mlxsw_qsfp)
{
	int i;

	if (!strcmp(mlxsw_qsfp->bus_info->device_kind, "i2c"))
		return;

	for (i = mlxsw_qsfp->module_count - 1; i >= 0; i--) {
		sysfs_remove_bin_file(&mlxsw_qsfp->bus_info->dev->kobj,
				      mlxsw_qsfp->eeprom_attr_list[i]);
		sysfs_remove_file(&mlxsw_qsfp->bus_info->dev->kobj,
				  mlxsw_qsfp->attrs[i]);
	}
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Vadim Pasternak <vadimp@mellanox.com>");
MODULE_DESCRIPTION("Mellanox switch QSFP sysfs driver");
