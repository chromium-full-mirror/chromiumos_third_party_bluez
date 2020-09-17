/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2020 Google LLC
 *
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 */

#ifndef __ADV_MONITOR_H
#define __ADV_MONITOR_H

struct mgmt;
struct btd_device;
struct btd_adapter;
struct btd_adv_monitor_manager;

struct btd_adv_monitor_manager *btd_adv_monitor_manager_create(
						struct btd_adapter *adapter,
						struct mgmt *mgmt);
void btd_adv_monitor_manager_destroy(struct btd_adv_monitor_manager *manager);

void btd_adv_monitor_device_remove(struct btd_adv_monitor_manager *manager,
				   struct btd_device *device);

/* Following functions are the helper functions used for RSSI Filter unit tests
 * defined in unit/test-adv-monitor.c
 */
void *btd_adv_monitor_rssi_test_setup(int8_t high_rssi, uint16_t high_timeout,
				      int8_t low_rssi, uint16_t low_timeout);
void btd_adv_monitor_rssi_test_teardown(void *monitor_obj);
bool btd_adv_monitor_test_device_state(void *monitor_obj, void *device_obj);
bool btd_adv_monitor_test_rssi(void *monitor_obj, void *device_obj,
			       int8_t adv_rssi);

#endif /* __ADV_MONITOR_H */
