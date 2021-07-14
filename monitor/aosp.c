// SPDX-License-Identifier: LGPL-2.1-or-later
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"

#include "src/shared/util.h"
#include "display.h"
#include "packet.h"
#include "vendor.h"
#include "aosp.h"

#define BLUETOOTH_CLOCK_MS(counter)		0.3125 * (counter)
#define BASEBAND_SLOT_MS(counter)		0.625 * (counter)

static void bqr_id_subevt(const struct bqr *r, const char *subevt_name)
{
	const char *str;

	switch (r->quality_report_id) {
	case 0x01:
		str = "Quality reporting on the monitoring mode";
		break;
	case 0x02:
		str = "Approaching LSTO";
		break;
	case 0x03:
		str = "A2DP Audio Choppy";
		break;
	case 0x04:
		str = "(e)SCO Voice Choppy";
		break;
	default:
		str = "Unknown";
		break;
	}

	print_field("%s: 0x%2.2x (%s)", subevt_name, r->quality_report_id, str);
}

/* packet types */
enum packet_types {
	PACKET_ID = 0x01,
	PACKET_NULL,
	PACKET_POLL,
	PACKET_FHS,
	PACKET_HV1,
	PACKET_HV2,
	PACKET_HV3,
	PACKET_DV,
	PACKET_EV3,
	PACKET_EV4,
	PACKET_EV5,
	PACKET_2EV3,
	PACKET_2EV5,
	PACKET_3EV3,
	PACKET_3EV5,
	PACKET_DH1 = 0x11,
	PACKET_DM3,
	PACKET_DH3,
	PACKET_DM5,
	PACKET_DH5,
	PACKET_AUX1,
	PACKET_2DH1,
	PACKET_2DH3,
	PACKET_2DH5,
	PACKET_3DH1,
	PACKET_3DH3,
	PACKET_3DH5,
};

static inline const char *packet_type_to_string(
					const enum packet_types packet_type)
{
	switch (packet_type) {
	case PACKET_ID:
		return "ID";
	case PACKET_NULL:
		return "NULL";
	case PACKET_POLL:
		return "POLL";
	case PACKET_FHS:
		return "FHS";
	case PACKET_HV1:
		return "HV1";
	case PACKET_HV2:
		return "HV2";
	case PACKET_HV3:
		return "HV3";
	case PACKET_DV:
		return "DV";
	case PACKET_EV3:
		return "EV3";
	case PACKET_EV4:
		return "EV4";
	case PACKET_EV5:
		return "EV5";
	case PACKET_2EV3:
		return "2EV3";
	case PACKET_2EV5:
		return "2EV5";
	case PACKET_3EV3:
		return "3EV3";
	case PACKET_3EV5:
		return "3EV5";
	case PACKET_DH1:
		return "DH1";
	case PACKET_DM3:
		return "DM3";
	case PACKET_DH3:
		return "DH3";
	case PACKET_DM5:
		return "DM5";
	case PACKET_DH5:
		return "DH5";
	case PACKET_AUX1:
		return "AUX1";
	case PACKET_2DH1:
		return "2DH1";
	case PACKET_2DH3:
		return "2DH3";
	case PACKET_2DH5:
		return "2DH5";
	case PACKET_3DH1:
		return "3DH1";
	case PACKET_3DH3:
		return "3DH3";
	case PACKET_3DH5:
		return "3DH5";
	}

	return "Unknown";
}

static void packet_type_subevt(const struct bqr *r, const char *subevt_name)
{
	const char *str;

	str = packet_type_to_string(r->packet_type);
	print_field("%s: 0x%2.2x (%s)", subevt_name, r->packet_type, str);
}

static void conn_handle_subevt(const struct bqr *r, const char *subevt_name)
{
	uint16_t conn_handle = get_le16(&r->conn_handle);

	print_field("%s: 0x%4.4x", subevt_name, conn_handle);
}

static void conn_role_subevt(const struct bqr *r, const char *subevt_name)
{
	const char *str;

	switch (r->conn_role) {
	case 0x00:
		str = "Central";
		break;
	case 0x01:
		str = "Peripheral";
		break;
	default:
		str = "Unknown";
		break;
	}

	print_field("%s: 0x%2.2x (%s)", subevt_name, r->conn_role, str);
}

static void tx_power_level_subevt(const struct bqr *r, const char *subevt_name)
{
	print_field("%s: %" PRId8 " (dbm)", subevt_name, r->tx_power_level);
}

static void rssi_subevt(const struct bqr *r, const char *subevt_name)
{
	print_field("%s: % " PRId8 " (dbm)", subevt_name, r->rssi);
}

static void snr_subevt(const struct bqr *r, const char *subevt_name)
{
	print_field("%s: %" PRIu8 " (db)", subevt_name, r->snr);
}

static void unused_afh_channel_count_subevt(const struct bqr *r,
							const char *subevt_name)
{
	print_field("%s: %" PRIu8, subevt_name, r->unused_afh_channel_count);
}

static void afh_select_unideal_channel_count_subevt(const struct bqr *r,
							const char *subevt_name)
{
	print_field("%s: %" PRIu8, subevt_name,
					r->afh_select_unideal_channel_count);
}

static void lsto_subevt(const struct bqr *r, const char *subevt_name)
{
	float lsto = BASEBAND_SLOT_MS(get_le16(&r->lsto));

	print_field("%s: %.2f (ms)", subevt_name, lsto);
}

static void conn_piconet_clock_subevt(const struct bqr *r,
							const char *subevt_name)
{
	float conn_piconet_clock =
			BLUETOOTH_CLOCK_MS(get_le32(&r->conn_piconet_clock));

	print_field("%s: %.2f (ms)", subevt_name, conn_piconet_clock);
}

static void retransmission_count_subevt(const struct bqr *r,
							const char *subevt_name)
{
	uint32_t retransmission_count = get_le32(&r->retransmission_count);

	print_field("%s: %" PRIu32, subevt_name, retransmission_count);
}

static void no_rx_count_subevt(const struct bqr *r, const char *subevt_name)
{
	uint32_t no_rx_count = get_le32(&r->no_rx_count);

	print_field("%s: %" PRIu32, subevt_name, no_rx_count);
}

static void nak_count_subevt(const struct bqr *r, const char *subevt_name)
{
	uint32_t nak_count = get_le32(&r->nak_count);

	print_field("%s: %" PRIu32, subevt_name, nak_count);
}

static void last_tx_ack_timestamp_subevt(const struct bqr *r,
							const char *subevt_name)
{
	float last_tx_ack_timestamp =
			BLUETOOTH_CLOCK_MS(get_le32(&r->last_tx_ack_timestamp));

	print_field("%s: %.2f (ms)", subevt_name, last_tx_ack_timestamp);
}

static void flow_off_count_subevt(const struct bqr *r, const char *subevt_name)
{
	uint32_t flow_off_count = get_le32(&r->flow_off_count);

	print_field("%s: %" PRIu32, subevt_name, flow_off_count);
}

static void last_flow_on_timestamp_subevt(const struct bqr *r,
							const char *subevt_name)
{
	float last_flow_on_timestamp =
		BLUETOOTH_CLOCK_MS(get_le32(&r->last_flow_on_timestamp));

	print_field("%s: %.2f (ms)", subevt_name, last_flow_on_timestamp);
}

static void buffer_overflow_bytes_subevt(const struct bqr *r,
							const char *subevt_name)
{
	uint32_t buffer_overflow_bytes = get_le32(&r->buffer_overflow_bytes);

	print_field("%s: %" PRIu32 " (bytes)", subevt_name,
							buffer_overflow_bytes);
}

static void buffer_underflow_bytes_subevt(const struct bqr *r,
							const char *subevt_name)
{
	uint32_t buffer_underflow_bytes = get_le32(&r->buffer_underflow_bytes);

	print_field("%s: %" PRIu32 " (bytes)", subevt_name,
							buffer_underflow_bytes);
}

static void vs_params_subevt(const void *data, uint8_t size)
{
	print_field("Vendor Specific Parameters: %" PRIu8 " (octets)", size);
	packet_hexdump(data, size);
}

struct bqr_subevt_data {
	enum bqr_subevt_list subevt_idx;
	const char *subevt_name;
	void (*func)(const struct bqr *r, const char *subevt_name);
};

static struct bqr_subevt_data bqr_subevt_table[] = {
	/* The sequence of subevents is fixed. There are no subevent codes. */
	{ QUALITY_REPORT_ID, "Quality report id", bqr_id_subevt },
	{ PACKET_TYPE, "Packet type", packet_type_subevt },
	{ CONNECTION_HANDLE, "Connection handle", conn_handle_subevt },
	{ CONNECTION_ROLE, "Connection role", conn_role_subevt },
	{ TX_POWER_LEVEL, "Tx power level", tx_power_level_subevt },
	{ RSSI, "RSSI", rssi_subevt },
	{ SNR, "SNR", snr_subevt },
	{ UNUSED_AFH_CHANNEL_COUNT, "Unused AFH channel count",
				unused_afh_channel_count_subevt },
	{ AFH_SELECT_UNIDEAL_CHANNEL_COUNT, "AFH select unideal channel count",
			afh_select_unideal_channel_count_subevt },
	{ LSTO, "LSTO", lsto_subevt },
	{ CONNECTION_PICONET_CLOCK, "Connection piconet clock",
					conn_piconet_clock_subevt },
	{ RETRANSMISSION_COUNT, "Retransmission count",
					retransmission_count_subevt },
	{ NO_RX_COUNT, "No rx count", no_rx_count_subevt },
	{ NAK_COUNT, "NAK count", nak_count_subevt },
	{ LAST_TX_ACK_TIMESTAMP, "Last tx ack timestamp",
					last_tx_ack_timestamp_subevt },
	{ FLOW_OFF_COUNT, "Flow off count", flow_off_count_subevt },
	{ LAST_FLOW_ON_TIMESTAMP, "Last flow on timestamp",
					last_flow_on_timestamp_subevt },
	{ BUFFER_OVERFLOW_BYTES, "Buffer overflow bytes",
					buffer_overflow_bytes_subevt },
	{ BUFFER_UNDERFLOW_BYTES, "Buffer underflow bytes",
					buffer_underflow_bytes_subevt },
	{}
};

static void aosp_bqr_evt(const void *data, uint8_t size)
{
	const struct bqr *r = data;
	int i;

	if (!data)
		return;

	if (size < STRUCT_BQR_SIZE) {
		print_field("quality report data too short (0x%02x)", size);
		packet_hexdump(data, size);
		return;
	}

	/* The report ids 0x01 ~ 0x04 are about link quality. */
	if (r->quality_report_id < 0x01 || r->quality_report_id > 0x04) {
		print_field("Unsupported quality report id (0x%02x)",
						r->quality_report_id);
		packet_hexdump(data, size);
		return;
	}

	for (i = 0; bqr_subevt_table[i].subevt_name; i++)
		bqr_subevt_table[i].func(r, bqr_subevt_table[i].subevt_name);

	vs_params_subevt(data + STRUCT_BQR_SIZE, size - STRUCT_BQR_SIZE);
}

/*
 * AOSP vendor specific HCI events
 *   https://source.android.com/devices/bluetooth/hci_requirements
 */
static const struct vendor_evt vendor_evt_table[] = {
	{ 0x58, "AOSP Bluetooth Quality Report",
			aosp_bqr_evt, STRUCT_BQR_SIZE },
	{ }
};

const struct vendor_evt *aosp_vendor_evt(uint8_t evt)
{
	int i;

	for (i = 0; vendor_evt_table[i].str; i++) {
		if (vendor_evt_table[i].evt == evt)
			return &vendor_evt_table[i];
	}

	return NULL;
}
