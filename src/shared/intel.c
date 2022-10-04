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

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"

#include "src/metrics.h"
#include "src/shared/intel.h"
#include "src/shared/util.h"

#define COMPANY_ID_INTEL	0x0002

struct intel_ext_telemetry_event tev;

static struct {
	intel_debug_func_t callback;
	void *data;
} intel_debug;

/* Use offsetof to access the attributes of structures. This makes
 * simple traversing and assigning values to the attributes.
 */
#define TELEM_OFFSET(a)		offsetof(struct intel_ext_telemetry_event, a)
#define TELEM_ATTR(a)		(((uint8_t *)&tev) + TELEM_OFFSET(a))

#define ACL_OFFSET(a)		offsetof(struct intel_acl_event, a)
#define ACL_ATTR(a)		(((uint8_t *)&tev.conn.acl) + ACL_OFFSET(a))
#define ACL_ATTR_ARRAY(a, i)	(ACL_ATTR(a) + i * sizeof(tev.conn.acl.a[0]))

#define SCO_OFFSET(a)		offsetof(struct intel_sco_event, a)
#define SCO_ATTR(a)		(((uint8_t *)&tev.conn.sco) + SCO_OFFSET(a))

static const struct intel_ext_subevent {
	uint8_t id;
	uint8_t size;
	uint8_t elements;
	uint8_t *attr;  /* address of the attribute in tev */
} intel_ext_subevent_table[] = {
	{ 0x01, 1, 1, TELEM_ATTR(telemetry_ev_type) },

	/* ACL audio link quality subevents */
	{ 0x4a, 2, 1, ACL_ATTR(conn_handle) },
	{ 0x4b, 4, 1, ACL_ATTR(rx_hec_error) },
	{ 0x4c, 4, 1, ACL_ATTR(rx_crc_error) },
	{ 0x4d, 4, 1, ACL_ATTR(packets_from_host) },
	{ 0x4e, 4, 1, ACL_ATTR(tx_packets) },
	{ 0x4f, 4, 1, ACL_ATTR_ARRAY(tx_packets_retry, 0) },
	{ 0x50, 4, 1, ACL_ATTR_ARRAY(tx_packets_retry, 1) },
	{ 0x51, 4, 1, ACL_ATTR_ARRAY(tx_packets_retry, 2) },
	{ 0x52, 4, 1, ACL_ATTR_ARRAY(tx_packets_retry, 3) },
	{ 0x53, 4, 1, ACL_ATTR_ARRAY(tx_packets_retry, 4) },
	{ 0x54, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 0) },
	{ 0x55, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 1) },
	{ 0x56, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 2) },
	{ 0x57, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 3) },
	{ 0x58, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 4) },
	{ 0x59, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 5) },
	{ 0x5a, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 6) },
	{ 0x5b, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 7) },
	{ 0x5c, 4, 1, ACL_ATTR_ARRAY(tx_packets_by_type, 8) },
	{ 0x5d, 4, 1, ACL_ATTR(rx_packets) },
	{ 0x5e, 4, 1, ACL_ATTR(link_throughput) },
	{ 0x5f, 4, 1, ACL_ATTR(max_packet_letency) },
	{ 0x60, 4, 1, ACL_ATTR(avg_packet_letency) },

	/* SCO/eSCO audio link quality subevents */
	{ 0x6a, 2, 1, SCO_ATTR(conn_handle) },
	{ 0x6b, 4, 1, SCO_ATTR(packets_from_host) },
	{ 0x6c, 4, 1, SCO_ATTR(tx_packets) },
	{ 0x6d, 4, 1, SCO_ATTR(rx_payload_lost) },
	{ 0x6e, 4, 1, SCO_ATTR(tx_payload_lost) },
	{ 0x6f, 4, 5, SCO_ATTR(rx_no_sync_error) },
	{ 0x70, 4, 5, SCO_ATTR(rx_hec_error) },
	{ 0x71, 4, 5, SCO_ATTR(rx_crc_error) },
	{ 0x72, 4, 5, SCO_ATTR(rx_nak_error) },
	{ 0x73, 4, 5, SCO_ATTR(tx_failed_wifi_coex) },
	{ 0x74, 4, 5, SCO_ATTR(rx_failed_wifi_coex) },
	{ 0x75, 4, 1, SCO_ATTR(samples_inserted_by_CDC) },
	{ 0x76, 4, 1, SCO_ATTR(samples_dropped) },
	{ 0x77, 4, 1, SCO_ATTR(mute_samples) },
	{ 0x78, 4, 1, SCO_ATTR(plc_injection) },

	/* end */
	{ 0x0, 0, 0 }
};

bool is_manufacturer_intel(uint16_t manufacturer)
{
	return manufacturer == COMPANY_ID_INTEL;
}

void intel_set_debug(intel_debug_func_t callback, void *user_data)
{
	intel_debug.callback = callback;
	intel_debug.data = user_data;
}

static void debug(const char *format, ...)
{
	va_list ap;
	char str[256];

	if (!intel_debug.callback || !intel_debug.data)
		return;

	va_start(ap, format);
	vsnprintf(str, sizeof(str), format, ap);
	intel_debug.callback(str, intel_debug.data);
	va_end(ap);
}

static void print_intel_telemetry_evt(struct intel_ext_telemetry_event *tev)
{
	if (!tev)
		return;

	if (tev->link_type == TELEMETRY_ACL_LINK) {
		struct intel_acl_event *acl = &tev->conn.acl;
		int i;

		debug("Intel Extended Telemetry Event");
		debug("  ACL connection handle: 0x%4.4x", acl->conn_handle);
		debug("  Rx HEC errors: %u", acl->rx_hec_error);
		debug("  Rx CRC errors: %u", acl->rx_crc_error);
		debug("  Packets from host: %u", acl->packets_from_host);
		debug("  Tx packets: %u", acl->tx_packets);

		for (i = 0; i < INTEL_NUM_RETRIES; i++)
			debug("  Tx packets %u retries: %u",
						i, acl->tx_packets_retry[i]);

		for (i = 0; i < INTEL_NUM_PACKET_TYPES; i++)
			debug("  Tx %s packets: %u", bredr_packet_type[i],
						acl->tx_packets_by_type[i]);

		debug("  Rx packets: %u", acl->rx_packets);
		debug("  ACL link throughput: %u", acl->link_throughput);
		debug("  ACL max packet latency: %u", acl->max_packet_letency);
		debug("  ACL avg packet latency: %u", acl->avg_packet_letency);

	} else if (tev->link_type == TELEMETRY_SCO_LINK) {
		struct intel_sco_event *sco = &tev->conn.sco;
		int i;

		debug("Intel Extended Telemetry Event");
		debug("  SCO connection handle: 0x%4.4x", sco->conn_handle);
		debug("  Packets from host: %u", sco->packets_from_host);
		debug("  Tx packets: %u", sco->tx_packets);
		debug("  Rx payload lost: %u", sco->rx_payload_lost);
		debug("  Tx payload lost: %u", sco->tx_payload_lost);

		for (i = 0; i < INTEL_NUM_SLOTS; i++)
			debug("  Rx No SYNC errors (slot %u): %u",
						i, sco->rx_no_sync_error[i]);

		for (i = 0; i < INTEL_NUM_SLOTS; i++)
			debug("  Rx HEC errors (slot %u): %u",
						i, sco->rx_hec_error[i]);

		for (i = 0; i < INTEL_NUM_SLOTS; i++)
			debug("  Rx CRC errors (slot %u): %u",
						i, sco->rx_crc_error[i]);

		for (i = 0; i < INTEL_NUM_SLOTS; i++)
			debug("  Rx NAK errors (slot %u): %u",
						i, sco->rx_nak_error[i]);

		for (i = 0; i < INTEL_NUM_SLOTS; i++)
			debug("  Failed Tx due to Wifi coex (slot %u): %u",
						i, sco->tx_failed_wifi_coex[i]);

		for (i = 0; i < INTEL_NUM_SLOTS; i++)
			debug("  Failed Rx due to Wifi coex (slot %u): %u",
						i, sco->rx_failed_wifi_coex[i]);

		debug("  Late samples inserted based on CDC: %u",
						sco->samples_inserted_by_CDC);
		debug("  Samples dropped: %u", sco->samples_dropped);
		debug("  Mute samples sent at initial connection: %u",
						sco->mute_samples);
		debug("  PLC injection data: %u", sco->plc_injection);
	}
}

static const struct intel_tlv *process_ext_subevent(
					struct intel_ext_telemetry_event *tev,
					const struct intel_tlv *tlv,
					const struct intel_tlv *last_tlv)
{
	const struct intel_tlv *next_tlv = NEXT_TLV(tlv);
	const struct intel_ext_subevent *subevent = NULL;
	uint8_t derived_subevent_size;
	int i;

	if (next_tlv > last_tlv) {
		debug("error: subevent 0x%2.2x exceeds the buffer size.",
			tlv->id);
		return NULL;
	}

	for (i = 0; intel_ext_subevent_table[i].size > 0; i++) {
		if (intel_ext_subevent_table[i].id == tlv->id) {
			subevent = &intel_ext_subevent_table[i];
			break;
		}
	}

	if (!subevent) {
		debug("warn: unknown Intel telemetry subevent 0x%2.2x",
			tlv->id);

		/* It is likely a new unknown subevent. Just skip it. */
		return next_tlv;
	}

	/* TODO(b/249208143): Intel changes the element size of arrays from
	 * 4 octets to 2 octets. Here is a temporary work-around to fix the
	 * element size issue. Intel should fix this issue in a formal way.
	 * The intel_ext_subevent_table has to be fixed accordingly too.
	 */
	if (tlv->length == subevent->size * subevent->elements) {
		derived_subevent_size = subevent->size;
	} else if (tlv->length % subevent->elements == 0) {
		derived_subevent_size =
			(uint8_t)(tlv->length / subevent->elements);
		debug("warn: Intel subevent 0x%2.2x has new element size %u",
			tlv->id, derived_subevent_size);
		/* Should return NULL when this issue is resolved formally. */
		/* return NULL; */
	} else {
		debug("error: invalid length %d of subevent 0x%2.2x",
			tlv->length, tlv->id);
		return NULL;
	}

	/* Assign tlv value to the corresponding attribute of acl/sco struct. */
	switch (derived_subevent_size) {
	case 1:
		*subevent->attr = get_u8(tlv->value);
		break;

	case 2:
		if (subevent->size == 2) {
			*((uint16_t *)subevent->attr) = get_le16(tlv->value);
			break;
		} else if (subevent->size == 4) {
			/* The case that the original subevent->size is 4. */
			for (i = 0; i < subevent->elements; i++) {
				/* The 2-octet elements are read into the
				 * original 4-octet elements so that the
				 * struct intel_sco_event can keep unchanged.
				 * In this way, there is no need to change the
				 * event printing function and the metrics.
				 */
				*((uint32_t *)subevent->attr + i) =
					get_le16((uint16_t *)tlv->value + i);
			}
		} else {
			debug("error: invalid length %d of subevent 0x%2.2x",
				tlv->length, tlv->id);
			return NULL;
		}
		break;

	case 4:
		if (subevent->elements == 1) {
			*((uint32_t *)subevent->attr) = get_le32(tlv->value);
			break;
		}

		for (i = 0; i < subevent->elements; i++) {
			/* Both acl and sco structs are __packed such that
			 * the addresses of array elements can be calculated.
			 */
			*((uint32_t *)(subevent->attr + i * subevent->size)) =
					get_le32((uint32_t *)tlv->value + i);
		}
		break;

	default:
		debug("error: subevent id %u: size %u not supported",
			subevent->id, derived_subevent_size);
		break;

	}

	switch (subevent->id) {
	case EXT_EVT_TYPE:
		/* Only interested in the LINK_QUALITY_REPORT type for now. */
		if (*subevent->attr != LINK_QUALITY_REPORT)
			return NULL;
		break;

	case ACL_CONNECTION_HANDLE:
		tev->link_type = TELEMETRY_ACL_LINK;
		break;

	case SCO_CONNECTION_HANDLE:
		tev->link_type = TELEMETRY_SCO_LINK;
		break;

	default:
		break;
	}

	return next_tlv;
}

void process_intel_telemetry_report(const struct mgmt_ev_quality_report *ev)
{
	/* The ev->report points to a number of consecutive tlv.*/
	const struct intel_tlv *tlv = (const struct intel_tlv *)ev->report;
	const struct intel_tlv *last_tlv =
			(const struct intel_tlv *)(ev->report + ev->report_len);

	/* Read every tlv subevent into tev.
	 * The decoding process terminates normally when tlv == last_tlv.
	 */
	memset(&tev, 0, sizeof(tev));
	while (tlv && tlv < last_tlv)
		tlv = process_ext_subevent(&tev, tlv, last_tlv);

	/* If the decoding completes successfully, tlv would be non-NULL */
	if (tlv) {
		print_intel_telemetry_evt(&tev);

		if (tev.link_type == TELEMETRY_ACL_LINK)
			metrics_report_intel_a2dp(&tev.conn.acl);
		else if (tev.link_type == TELEMETRY_SCO_LINK)
			metrics_report_intel_hfp(&tev.conn.sco);
	}
}
