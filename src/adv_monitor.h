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
struct queue;
struct btd_device;
struct btd_adapter;
struct btd_adv_monitor_manager;
struct btd_adv_monitor_pattern;

struct btd_adv_monitor_manager *btd_adv_monitor_manager_create(
						struct btd_adapter *adapter,
						struct mgmt *mgmt);
void btd_adv_monitor_manager_destroy(struct btd_adv_monitor_manager *manager);

bool btd_adv_monitor_offload_supported(struct btd_adv_monitor_manager *manager);

struct queue *btd_adv_monitor_content_filter(
				struct btd_adv_monitor_manager *manager,
				struct bt_ad *ad);

void btd_adv_monitor_notify_monitors(struct btd_adv_monitor_manager *manager,
					struct btd_device *device, int8_t rssi,
					struct queue *matched_monitors);

void btd_adv_monitor_device_remove(struct btd_adv_monitor_manager *manager,
				   struct btd_device *device);

void btd_adv_monitor_power_down(struct btd_adv_monitor_manager *manager);

void btd_adv_monitor_resume(struct btd_adv_monitor_manager *manager);

bool btd_adv_monitor_get_offload_support(
				struct btd_adv_monitor_manager *manager);

int btd_adv_monitor_get_monitor_count(struct btd_adv_monitor_manager *manager);

void btd_adv_monitor_notify_active_scan(struct btd_adv_monitor_manager *manager,
					bool enabled);

#endif /* __ADV_MONITOR_H */
