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

#include <errno.h>
#include <stdint.h>
#include <string.h>

enum CONN_TYPE {
	ACL,
	SCO,
};

enum VENDOR {
	INTEL,
	QCM,
	RTK,
	MTK,
};

enum PACKET_TYPE {
	PKT_DH1,
	PKT_DH3,
	PKT_DH5,
	PKT_2DH1,
	PKT_2DH3,
	PKT_2DH5,
	PKT_3DH1,
	PKT_3DH3,
	PKT_3DH5,
};

/*
 * The subevent indices of the complete list of Android BQR subevents.
 * Since there are no defined subevent IDs in the Android spec, the
 * subevent indices are defined here to identify a particular subevent
 * according to the ordering of the subevents in a BQR event reported by
 * a Bluetooth controller.
 */
enum bqr_subevt_list {
	QUALITY_REPORT_ID,
	PACKET_TYPE,
	CONNECTION_HANDLE,
	CONNECTION_ROLE,
	TX_POWER_LEVEL,
	RSSI,
	SNR,
	UNUSED_AFH_CHANNEL_COUNT,
	AFH_SELECT_UNIDEAL_CHANNEL_COUNT,
	LSTO,
	CONNECTION_PICONET_CLOCK,
	RETRANSMISSION_COUNT,
	NO_RX_COUNT,
	NAK_COUNT,
	LAST_TX_ACK_TIMESTAMP,
	FLOW_OFF_COUNT,
	LAST_FLOW_ON_TIMESTAMP,
	BUFFER_OVERFLOW_BYTES,
	BUFFER_UNDERFLOW_BYTES,
	ANDROID_STATS_SUBEVTS_COUNT,
};

struct bqr {
	uint8_t quality_report_id;
	uint8_t packet_type;
	uint16_t conn_handle;
	uint8_t conn_role;
	int8_t tx_power_level;
	int8_t rssi;				/* dbm */
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
};

#define STRUCT_BQR_SIZE			sizeof(struct bqr)

struct subevt_info_data;

void aosp_get_subevt_info(uint8_t subevt_idx,
					struct subevt_info_data *subevt_info);
const struct vendor_evt *aosp_vendor_evt(uint8_t evt);

#endif /* __AOSP_H */
