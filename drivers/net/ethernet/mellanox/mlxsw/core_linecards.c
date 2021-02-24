// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2021 Mellanox Technologies. All rights reserved */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/list.h>

#include "core.h"

struct mlxsw_linecards_event_ops_item {
	struct list_head list;
	struct mlxsw_linecards_event_ops *event_ops;
	void *priv;
};

static int mlxsw_linecard_activate(struct mlxsw_core *mlxsw_core,
				   struct mlxsw_linecard *linecard)
{
	char mddc_pl[MLXSW_REG_MDDC_LEN];

	mlxsw_reg_mddc_pack(mddc_pl, linecard->slot_index, true);
	return mlxsw_reg_write(mlxsw_core, MLXSW_REG(mddc), mddc_pl);
}

static void
mlxsw_linecard_provision_set(struct mlxsw_linecard *linecard,
			     enum mlxsw_reg_mddq_card_type card_type)
{
}

static void mlxsw_linecard_provision_clear(struct mlxsw_linecard *linecard)
{
}

static void mlxsw_linecard_got_active(struct mlxsw_core *mlxsw_core,
				      struct mlxsw_linecards *linecards,
				      struct mlxsw_linecard *linecard)
{
	struct mlxsw_linecards_event_ops_item *item;

	list_for_each_entry(item, &linecards->event_ops_list, list)
		item->event_ops->got_active(mlxsw_core, linecard->slot_index,
					    linecard, item->priv);
}

static void mlxsw_linecard_got_inactive(struct mlxsw_core *mlxsw_core,
					struct mlxsw_linecards *linecards,
					struct mlxsw_linecard *linecard)
{
	struct mlxsw_linecards_event_ops_item *item;

	list_for_each_entry(item, &linecards->event_ops_list, list)
		item->event_ops->got_inactive(mlxsw_core, linecard->slot_index,
					      linecard, item->priv);
}

static int __mlxsw_linecard_status_process(struct mlxsw_core *mlxsw_core,
					   struct mlxsw_linecards *linecards,
					   struct mlxsw_linecard *linecard,
					   const char *mddq_pl,
					   bool process_provision_only)
{
	enum mlxsw_reg_mddq_card_type card_type;
	u16 major_ini_file_version;
	u16 minor_ini_file_version;
	bool provisioned;
	bool sr_valid;
	u8 slot_index;
	bool active;
	bool ready;

	mlxsw_reg_mddq_slot_info_unpack(mddq_pl, &slot_index, &provisioned,
					&sr_valid, &ready, &active,
					&major_ini_file_version,
					&minor_ini_file_version, &card_type);

	if (linecard) {
		if (slot_index != linecard->slot_index)
			return -EINVAL;
	} else {
		if (slot_index > linecards->count)
			return -EINVAL;
		linecard = mlxsw_linecard_get(linecards, slot_index);
	}

	if (linecard->provisioned != provisioned) {
		if (provisioned)
			mlxsw_linecard_provision_set(linecard, card_type);
		else
			mlxsw_linecard_provision_clear(linecard);
		linecard->provisioned = provisioned;
	}
	if (process_provision_only)
		return 0;
	if (linecard->ready != ready) {
		if (ready) {
			int err;

			err = mlxsw_linecard_activate(mlxsw_core, linecard);
			if (err)
				return err;
		}
		linecard->ready = ready;
	}
	if (linecard->active != active) {
		if (active)
			mlxsw_linecard_got_active(mlxsw_core,
						  linecards, linecard);
		else
			mlxsw_linecard_got_inactive(mlxsw_core,
						    linecards, linecard);
		linecard->active = active;
	}
	return 0;
}
int mlxsw_linecard_status_process(struct mlxsw_core *mlxsw_core,
				  const char *mddq_pl)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);

	return __mlxsw_linecard_status_process(mlxsw_core, linecards, NULL,
					       mddq_pl, false);
}
EXPORT_SYMBOL(mlxsw_linecard_status_process);

static int mlxsw_linecard_status_get_and_process(struct mlxsw_core *mlxsw_core,
						 struct mlxsw_linecard *linecard,
						 bool process_provision_only)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);
	char mddq_pl[MLXSW_REG_MDDQ_LEN];
	int err;

	mlxsw_reg_mddq_pack(mddq_pl, linecard->slot_index, false,
			    MLXSW_REG_MDDQ_QUERY_TYPE_SLOT_INFO);
	err = mlxsw_reg_query(mlxsw_core, MLXSW_REG(mddq), mddq_pl);
	if (err)
		return err;

	return __mlxsw_linecard_status_process(mlxsw_core, linecards, linecard,
					       mddq_pl, process_provision_only);
}

static int mlxsw_linecard_init(struct mlxsw_core *mlxsw_core,
			       struct mlxsw_linecards *linecards,
			       u8 slot_index)
{
	struct mlxsw_linecard *linecard;
	int err;

	linecard = mlxsw_linecard_get(linecards, slot_index);
	linecard->slot_index = slot_index;
	linecard->linecards = linecards;

	err = mlxsw_linecard_status_get_and_process(mlxsw_core, linecard, true);
	if (err)
		goto err_status_get_and_process;

	return 0;

err_status_get_and_process:
	return err;
}

static int mlxsw_linecard_event_delivery_set(struct mlxsw_core *mlxsw_core,
					     struct mlxsw_linecard *linecard,
					     bool enable)
{
	char mddq_pl[MLXSW_REG_MDDQ_LEN];

	mlxsw_reg_mddq_pack(mddq_pl, linecard->slot_index, enable,
			    MLXSW_REG_MDDQ_QUERY_TYPE_SLOT_INFO);
	return mlxsw_reg_write(mlxsw_core, MLXSW_REG(mddq), mddq_pl);
}

static int mlxsw_linecard_post_init(struct mlxsw_core *mlxsw_core,
				    struct mlxsw_linecards *linecards,
				    u8 slot_index)
{
	struct mlxsw_linecard *linecard;
	int err;

	linecard = mlxsw_linecard_get(linecards, slot_index);
	linecard->slot_index = slot_index;

	err = mlxsw_linecard_event_delivery_set(mlxsw_core, linecard, true);
	if (err)
		return err;

	err = mlxsw_linecard_status_get_and_process(mlxsw_core, linecard,
						    false);
	if (err)
		goto err_status_get_and_process;

	return 0;

err_status_get_and_process:
	mlxsw_linecard_event_delivery_set(mlxsw_core, linecard, false);
	return err;
}

static void mlxsw_linecard_pre_fini(struct mlxsw_core *mlxsw_core,
				    struct mlxsw_linecards *linecards,
				    u8 slot_index)
{
	struct mlxsw_linecard *linecard;

	linecard = mlxsw_linecard_get(linecards, slot_index);
	mlxsw_linecard_event_delivery_set(mlxsw_core, linecard, false);
}

static void mlxsw_linecard_fini(struct mlxsw_core *mlxsw_core,
				struct mlxsw_linecards *linecards,
				u8 slot_index)
{
	struct mlxsw_linecard *linecard;

	linecard = mlxsw_linecard_get(linecards, slot_index);
}

int mlxsw_linecards_init(struct mlxsw_core *mlxsw_core,
			 struct mlxsw_linecards **p_linecards)
{
	char mgpir_pl[MLXSW_REG_MGPIR_LEN];
	struct mlxsw_linecards *linecards;
	u8 slot_count;
	int err;
	int i;

	mlxsw_reg_mgpir_pack(mgpir_pl, 0);
	err = mlxsw_reg_query(mlxsw_core, MLXSW_REG(mgpir), mgpir_pl);
	if (err)
		return err;

	mlxsw_reg_mgpir_unpack(mgpir_pl, NULL, NULL, NULL,
			       NULL, &slot_count);
	if (!slot_count) {
		*p_linecards = NULL;
		return 0;
	}

	linecards = kzalloc(struct_size(linecards, linecards, slot_count),
			    GFP_KERNEL);
	if (!linecards)
		return -ENOMEM;
	linecards->count = slot_count;
	linecards->mlxsw_core = mlxsw_core;

	for (i = 0; i < linecards->count; i++) {
		err = mlxsw_linecard_init(mlxsw_core, linecards, i + 1);
		if (err)
			goto err_linecard_init;
	}

	INIT_LIST_HEAD(&linecards->event_ops_list);
	*p_linecards = linecards;

	return 0;

err_linecard_init:
	for (i--; i >= 0; i--)
		mlxsw_linecard_fini(mlxsw_core, linecards, i + 1);
	kfree(linecards);

	return err;
}

int mlxsw_linecards_post_init(struct mlxsw_core *mlxsw_core,
			      struct mlxsw_linecards *linecards)
{
	int err;
	int i;

	if (!linecards)
		return 0;

	for (i = 0; i < linecards->count; i++) {
		err = mlxsw_linecard_post_init(mlxsw_core, linecards, i + 1);
		if (err)
			goto err_linecard_port_init;
	}
	return 0;

err_linecard_port_init:
	for (i--; i >= 0; i--)
		mlxsw_linecard_pre_fini(mlxsw_core, linecards, i + 1);

	return err;
}

void mlxsw_linecards_pre_fini(struct mlxsw_core *mlxsw_core,
			      struct mlxsw_linecards *linecards)
{
	int i;

	if (!linecards)
		return;
	for (i = 0; i < linecards->count; i++)
		mlxsw_linecard_pre_fini(mlxsw_core, linecards, i + 1);
	/* Make sure all scheduled events are processed */
	mlxsw_core_flush_owq();
}

void mlxsw_linecards_fini(struct mlxsw_core *mlxsw_core,
			  struct mlxsw_linecards *linecards)
{
	int i;

	if (!linecards)
		return;
	WARN_ON(!list_empty(&linecards->event_ops_list));
	for (i = 0; i < linecards->count; i++)
		mlxsw_linecard_fini(mlxsw_core, linecards, i + 1);
}

int mlxsw_linecards_event_ops_register(struct mlxsw_core *mlxsw_core,
				       struct mlxsw_linecards_event_ops *ops,
				       void *priv)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);
	struct mlxsw_linecards_event_ops_item *item;

	item = kzalloc(sizeof(*item), GFP_KERNEL);
	if (!item)
		return -ENOMEM;
	item->event_ops = ops;
	item->priv = priv;
	list_add_tail(&item->list, &linecards->event_ops_list);
	return 0;
}
EXPORT_SYMBOL(mlxsw_linecards_event_ops_register);

void mlxsw_linecards_event_ops_unregister(struct mlxsw_core *mlxsw_core,
					  struct mlxsw_linecards_event_ops *ops,
					  void *priv)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);
	struct mlxsw_linecards_event_ops_item *item, *tmp;

	list_for_each_entry_safe(item, tmp, &linecards->event_ops_list, list) {
		if (item->event_ops == ops && item->priv == priv)
			list_del(&item->list);
	}
}
EXPORT_SYMBOL(mlxsw_linecards_event_ops_unregister);
