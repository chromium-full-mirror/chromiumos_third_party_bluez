/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2021 Google LLC
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

#ifndef __AOSP_H
#define __AOSP_H

#include <stdbool.h>

struct mgmt_ev_quality_report;

struct aosp_bqr {
	uint8_t quality_report_id;
	uint8_t packet_type;
	uint16_t conn_handle;
	uint8_t conn_role;
	int8_t tx_power_level;			/* -30  to 20 dbm */
	int8_t rssi;				/* -127 to 20 dbm */
	uint8_t snr;				/* db */
	uint8_t unused_afh_channel_count;
	uint8_t afh_select_unideal_channel_count;
	uint16_t lsto;
	uint32_t conn_piconet_clock;
	uint32_t retransmission_count;
	uint32_t no_rx_count;
	uint32_t nak_count;
	uint32_t last_tx_ack_timestamp;
	uint32_t flow_off_count;
	uint32_t last_flow_on_timestamp;
	uint32_t buffer_overflow_bytes;
	uint32_t buffer_underflow_bytes;

	uint8_t vsp[0];			/* Vendor Specific Parameter */
} __packed;

typedef void (*aosp_debug_func_t)(const char *str, void *user_data);
void aosp_set_debug(aosp_debug_func_t callback, void *user_data);

bool process_aosp_quality_report(const struct mgmt_ev_quality_report *ev);

#endif /* __AOSP_H */
