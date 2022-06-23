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

#ifndef __INTEL_H
#define __INTEL_H

#ifndef __packed
#define __packed __attribute__((packed))
#endif

#include <stdbool.h>

struct mgmt_ev_quality_report;

enum intel_telemetry_event_type {
	SYSTEM_EXCEPTION,
	FATAL_EXCEPTION,
	DEBUG_EXCEPTION,
	CONNECTION_EVENT,
	DISCONNECTION_EVENT,
	LINK_QUALITY_REPORT,
};

enum intel_telemetry_link_type {
	TELEMETRY_UNKNOWN_LINK,
	TELEMETRY_ACL_LINK,
	TELEMETRY_SCO_LINK,
};

/* The subevent indices of the complete list of Intel telemetry subevents. */
enum intel_subevt_list {
	EXT_EVT_TYPE = 0x01,

	ACL_CONNECTION_HANDLE = 0x4a,
	ACL_HEC_ERRORS,
	ACL_CRC_ERRORS,
	ACL_PACKETS_FROM_HOST,
	ACL_TX_PACKETS_TO_AIR,
	ACL_TX_PACKETS_0_RETRY,
	ACL_TX_PACKETS_1_RETRY,
	ACL_TX_PACKETS_2_RETRY,
	ACL_TX_PACKETS_3_RETRY,
	ACL_TX_PACKETS_MORE_RETRY,
	ACL_TX_PACKETS_DH1,
	ACL_TX_PACKETS_DH3,
	ACL_TX_PACKETS_DH5,
	ACL_TX_PACKETS_2DH1,
	ACL_TX_PACKETS_2DH3,
	ACL_TX_PACKETS_2DH5,
	ACL_TX_PACKETS_3DH1,
	ACL_TX_PACKETS_3DH3,
	ACL_TX_PACKETS_3DH5,
	ACL_RX_PACKETS,
	ACL_LINK_THROUGHPUT,
	ACL_MAX_PACKET_LATENCY,
	ACL_AVG_PACKET_LATENCY,

	SCO_CONNECTION_HANDLE = 0x6a,
	SCO_RX_PACKETS,
	SCO_TX_PACKETS,
	SCO_RX_PACKETS_LOST,
	SCO_TX_PACKETS_LOST,
	SCO_RX_NO_SYNC_ERROR,
	SCO_RX_HEC_ERROR,
	SCO_RX_CRC_ERROR,
	SCO_RX_NAK_ERROR,
	SCO_TX_FAILED_BY_WIFI,
	SCO_RX_FAILED_BY_WIFI,
	SCO_SAMPLES_INSERTED,
	SCO_SAMPLES_DROPPED,
	SCO_MUTE_SAMPLES,
	SCO_PLC_INJECTION_DATA,
};

#define INTEL_NUM_SLOTS		5
#define INTEL_NUM_RETRIES	5
#define INTEL_NUM_PACKET_TYPES	9

/* An Intel telemetry subevent is of the TLV format.
 * - id: takes 1 byte. This is the subevent id.
 * - length: takes 1 byte.
 * - value: takes |length| bytes.
 */
struct intel_tlv {
	uint8_t id;
	uint8_t length;
	uint8_t value[0];
};

static char *bredr_packet_type[INTEL_NUM_PACKET_TYPES] = {
	"DH1",
	"DH3",
	"DH5",
	"2DH1",
	"2DH3",
	"2DH5",
	"3DH1",
	"3DH3",
	"3DH5",
};

#define TLV_SIZE(tlv) (*((const uint8_t *) tlv + 1) + 2 * sizeof(uint8_t))
#define NEXT_TLV(tlv) (const struct intel_tlv *) \
					((const uint8_t *) tlv + TLV_SIZE(tlv))

struct intel_acl_event {
	uint16_t conn_handle;
	uint32_t rx_hec_error;
	uint32_t rx_crc_error;
	uint32_t packets_from_host;
	uint32_t tx_packets;
	uint32_t tx_packets_retry[INTEL_NUM_RETRIES];
	uint32_t tx_packets_by_type[INTEL_NUM_PACKET_TYPES];
	uint32_t rx_packets;
	uint32_t link_throughput;
	uint32_t max_packet_letency;
	uint32_t avg_packet_letency;
} __packed;

struct intel_sco_event {
	uint16_t conn_handle;
	uint32_t packets_from_host;
	uint32_t tx_packets;
	uint32_t rx_payload_lost;
	uint32_t tx_payload_lost;
	uint32_t rx_no_sync_error[INTEL_NUM_SLOTS];
	uint32_t rx_hec_error[INTEL_NUM_SLOTS];
	uint32_t rx_crc_error[INTEL_NUM_SLOTS];
	uint32_t rx_nak_error[INTEL_NUM_SLOTS];
	uint32_t tx_failed_wifi_coex[INTEL_NUM_SLOTS];
	uint32_t rx_failed_wifi_coex[INTEL_NUM_SLOTS];
	uint32_t samples_inserted_by_CDC;
	uint32_t samples_dropped;
	uint32_t mute_samples;
	uint32_t plc_injection;
} __packed;

struct intel_ext_telemetry_event {
	uint8_t telemetry_ev_type; /* one in enum intel_telemetry_event_type */
	uint8_t link_type;
	union {
		struct intel_sco_event sco;
		struct intel_acl_event acl;
	} conn;
} __packed;

typedef void (*intel_debug_func_t)(const char *str, void *user_data);

bool is_manufacturer_intel(uint16_t manufacturer);
void intel_set_debug(intel_debug_func_t callback, void *user_data);

void process_intel_telemetry_report(const struct mgmt_ev_quality_report *ev);

#endif /* __INTEL_H */
