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

#include <glib.h>

#include "src/shared/ad.h"

struct mgmt;
struct btd_device;
struct btd_adapter;
struct btd_adv_monitor_manager;
struct btd_adv_monitor_pattern;

struct btd_adv_monitor_manager *btd_adv_monitor_manager_create(
						struct btd_adapter *adapter,
						struct mgmt *mgmt);
void btd_adv_monitor_manager_destroy(struct btd_adv_monitor_manager *manager);

GSList *btd_adv_monitor_content_filter(struct btd_adv_monitor_manager *manager,
					const uint8_t *eir, uint8_t eir_len);

void btd_adv_monitor_notify_monitors(struct btd_adv_monitor_manager *manager,
					struct btd_device *device, int8_t rssi,
					GSList *matched_monitors);

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
struct btd_adv_monitor_pattern *btd_adv_monitor_test_pattern_create(
	uint8_t ad_type, uint8_t offset, uint8_t length, const uint8_t *value);
void btd_adv_monitor_test_pattern_destroy(
				struct btd_adv_monitor_pattern *pattern);
bool btd_adv_monitor_pattern_match(
	const uint8_t *eir, uint8_t eir_len,
	const struct btd_adv_monitor_pattern *pattern);

#endif /* __ADV_MONITOR_H */
