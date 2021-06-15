// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2021 NVIDIA Corporation and Mellanox Technologies. All rights reserved */

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

static int
mlxsw_linecard_provision_cbs_call(struct mlxsw_core *mlxsw_core,
				  struct mlxsw_linecards *linecards,
				  struct mlxsw_linecard *linecard)
{
	struct mlxsw_linecards_event_ops_item *item;
	int err;

	list_for_each_entry(item, &linecards->event_ops_list, list) {
		if (!item->event_ops->got_provisioned)
			continue;
		err = item->event_ops->got_provisioned(mlxsw_core,
						       linecard->slot_index,
						       linecard, item->priv);
		if (err)
			goto rollback;
	}
	return 0;

rollback:
	list_for_each_entry_continue_reverse(item, &linecards->event_ops_list,
					     list) {
		if (!item->event_ops->got_unprovisioned)
			continue;
		item->event_ops->got_unprovisioned(mlxsw_core,
						   linecard->slot_index,
						   linecard, item->priv);
	}
	return err;
}

static void
mlxsw_linecard_unprovision_cbs_call(struct mlxsw_core *mlxsw_core,
				    struct mlxsw_linecards *linecards,
				    struct mlxsw_linecard *linecard)
{
	struct mlxsw_linecards_event_ops_item *item;

	list_for_each_entry(item, &linecards->event_ops_list, list) {
		if (!item->event_ops->got_unprovisioned)
			continue;
		item->event_ops->got_unprovisioned(mlxsw_core,
						   linecard->slot_index,
						   linecard, item->priv);
	}
}

static int
mlxsw_linecard_provision_set(struct mlxsw_core *mlxsw_core,
			     struct mlxsw_linecards *linecards,
			     struct mlxsw_linecard *linecard,
			     enum mlxsw_reg_mddq_card_type card_type)
{
	int err;

	err = mlxsw_linecard_provision_cbs_call(mlxsw_core, linecards,
						linecard);
	if (err)
		goto err_cbs_call;
	linecard->provisioned = true;
err_cbs_call:
	return err;
}

static void mlxsw_linecard_provision_clear(struct mlxsw_core *mlxsw_core,
					   struct mlxsw_linecards *linecards,
					   struct mlxsw_linecard *linecard)
{
	linecard->provisioned = false;
	mlxsw_linecard_unprovision_cbs_call(mlxsw_core, linecards,
					    linecard);
}

static int mlxsw_linecard_ready_set(struct mlxsw_core *mlxsw_core, struct mlxsw_linecards *linecards,
				    struct mlxsw_linecard *linecard)
{
	/*err = */mlxsw_linecard_provision_cbs_call(mlxsw_core, linecards,
						linecard);

	linecard->ready = true;
	return 0;
}

static void mlxsw_linecard_ready_clear(struct mlxsw_linecard *linecard)
{
	linecard->ready = false;
}

static void mlxsw_linecard_active_set(struct mlxsw_core *mlxsw_core,
				      struct mlxsw_linecards *linecards,
				      struct mlxsw_linecard *linecard,
				      u16 ini_version, u16 hw_revision)
{
	struct mlxsw_linecards_event_ops_item *item;

	linecard->active = true;
	linecard->hw_revision = hw_revision;
	linecard->ini_version = ini_version;
	list_for_each_entry(item, &linecards->event_ops_list, list) {
		if (!item->event_ops->got_active)
			continue;
		item->event_ops->got_active(mlxsw_core, linecard->slot_index,
					    linecard, item->priv);
	}
}

static void mlxsw_linecard_active_clear(struct mlxsw_core *mlxsw_core,
					struct mlxsw_linecards *linecards,
					struct mlxsw_linecard *linecard)
{
	struct mlxsw_linecards_event_ops_item *item;

	linecard->active = false;
	list_for_each_entry(item, &linecards->event_ops_list, list) {
		if (!item->event_ops->got_inactive)
			continue;
		item->event_ops->got_inactive(mlxsw_core, linecard->slot_index,
					      linecard, item->priv);
	}
}

static int __mlxsw_linecard_status_process(struct mlxsw_core *mlxsw_core,
					   struct mlxsw_linecards *linecards,
					   struct mlxsw_linecard *linecard,
					   const char *mddq_pl,
					   bool process_provision_only, bool tmp_delayed)
{
	enum mlxsw_reg_mddq_card_type card_type;
	enum mlxsw_reg_mddq_ready ready;
	bool provisioned;
	u16 ini_version;
	u16 hw_revision;
	bool sr_valid;
	u8 slot_index;
	int err = 0;
	bool active;
	bool tmp_delayed_mddq = false;

	mlxsw_reg_mddq_slot_info_unpack(mddq_pl, &slot_index, &provisioned,
					&sr_valid, &ready, &active,
					&hw_revision, &ini_version,
					&card_type);
	printk("%s delayed: %s, lc%u, prov %d, sr_valid %d, ready %d, active %d, hw_revision %u, ini_version %u provision_only %d\n",
	       __func__, tmp_delayed ? "yes": "no", slot_index, provisioned, sr_valid, ready, active, hw_revision, ini_version, process_provision_only);

	if (linecard) {
		if (slot_index != linecard->slot_index)
			return -EINVAL;
	} else {
		if (slot_index > linecards->count)
			return -EINVAL;
		linecard = mlxsw_linecard_get(linecards, slot_index);
	}

	mutex_lock(&linecard->lock);

	if (provisioned && linecard->provisioned != provisioned) {
		err = mlxsw_linecard_provision_set(mlxsw_core, linecards,
						   linecard, card_type);
		if (err)
			goto out;
		if (!process_provision_only)
			tmp_delayed_mddq = true;
	}

	if (!process_provision_only && ready == MLXSW_REG_MDDQ_READY_READY &&
	    !linecard->ready) {
		err = mlxsw_linecard_ready_set(mlxsw_core, linecards, linecard);
		if (err)
			goto out;
		tmp_delayed_mddq = true;
	}

	if (!process_provision_only && active && linecard->active != active)
		mlxsw_linecard_active_set(mlxsw_core, linecards, linecard,
					  hw_revision, ini_version);

	if (!process_provision_only && !active && linecard->active != active)
		mlxsw_linecard_active_clear(mlxsw_core, linecards, linecard);

	if (!process_provision_only && ready != MLXSW_REG_MDDQ_READY_READY &&
	    linecard->ready)
		mlxsw_linecard_ready_clear(linecard);

	if (!provisioned && linecard->provisioned != provisioned)
		mlxsw_linecard_provision_clear(mlxsw_core, linecards, linecard);

out:
	mutex_unlock(&linecard->lock);

#if 0
	if (tmp_delayed_mddq)
		mlxsw_core_schedule_dw(&linecard->tmp_mddq_dw, msecs_to_jiffies(1500));
#endif
	return err;
}

int mlxsw_linecard_status_process(struct mlxsw_core *mlxsw_core,
				  const char *mddq_pl)
{
	struct mlxsw_linecards *linecards = mlxsw_core_linecards(mlxsw_core);

	return __mlxsw_linecard_status_process(mlxsw_core, linecards, NULL,
					       mddq_pl, false, false);
}
EXPORT_SYMBOL(mlxsw_linecard_status_process);

static int mlxsw_linecard_status_get_and_process(struct mlxsw_core *mlxsw_core,
						 struct mlxsw_linecards *linecards,
						 struct mlxsw_linecard *linecard,
						 bool process_provision_only,
						 bool tmp_delayed)
{
	char mddq_pl[MLXSW_REG_MDDQ_LEN];
	int err;

	mlxsw_reg_mddq_slot_info_pack(mddq_pl, linecard->slot_index, false);
	err = mlxsw_reg_query(mlxsw_core, MLXSW_REG(mddq), mddq_pl);
	if (err)
		return err;

	return __mlxsw_linecard_status_process(mlxsw_core, linecards, linecard,
					       mddq_pl, process_provision_only, tmp_delayed);
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
	mutex_init(&linecard->lock);

	err = mlxsw_linecard_status_get_and_process(mlxsw_core, linecards,
						    linecard, true, false);
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
#if 0
	char mddq_pl[MLXSW_REG_MDDQ_LEN];

	mlxsw_reg_mddq_slot_info_pack(mddq_pl, linecard->slot_index, enable);
	return mlxsw_reg_write(mlxsw_core, MLXSW_REG(mddq), mddq_pl);
#else
	return 0;
#endif
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

	err = mlxsw_linecard_status_get_and_process(mlxsw_core, linecards,
						    linecard, false, false);
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
			 const struct mlxsw_bus_info *bus_info,
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
			       NULL, &slot_count, NULL);
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
	linecards->bus_info = bus_info;
	INIT_LIST_HEAD(&linecards->event_ops_list);

	for (i = 0; i < linecards->count; i++) {
		err = mlxsw_linecard_init(mlxsw_core, linecards, i + 1);
		if (err)
			goto err_linecard_init;
	}

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
			goto err_linecard_post_init;
	}
	return 0;

err_linecard_post_init:
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
	kfree(linecards);
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
