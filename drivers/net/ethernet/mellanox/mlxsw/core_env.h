/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/* Copyright (c) 2018 Mellanox Technologies. All rights reserved */

#ifndef _MLXSW_CORE_ENV_H
#define _MLXSW_CORE_ENV_H

struct ethtool_modinfo;
struct ethtool_eeprom;

struct mlxsw_env_gearbox_sensors_map {
	u16 sensor_count;
	u16 *sensor_bit_map;
};

int mlxsw_env_module_temp_thresholds_get(struct mlxsw_core *core,
					 u8 slot_index, int module, int off,
					 int *temp);

int mlxsw_env_get_module_info(struct mlxsw_core *mlxsw_core, u8 slot_index,
			      int module, struct ethtool_modinfo *modinfo);

int mlxsw_env_get_module_eeprom(struct net_device *netdev,
				struct mlxsw_core *mlxsw_core, u8 slot_index,
				int module, struct ethtool_eeprom *ee,
				u8 *data);

int mlxsw_env_sensor_map_create(struct mlxsw_core *core,
				const struct mlxsw_bus_info *bus_info,
				u8 slot_index,
				struct mlxsw_env_gearbox_sensors_map *map);
void mlxsw_env_sensor_map_destroy(const struct mlxsw_bus_info *bus_info,
				  u16 *sensor_bit_map);

#endif
