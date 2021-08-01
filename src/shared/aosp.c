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

#include "src/shared/aosp.h"
#include "src/shared/util.h"

static struct {
	aosp_debug_func_t callback;
	void *data;
} aosp_debug;

void aosp_set_debug(aosp_debug_func_t callback, void *user_data)
{
	aosp_debug.callback = callback;
	aosp_debug.data = user_data;
}

static void debug(const char *format, ...)
{
	va_list ap;
	char str[256];

	if (!aosp_debug.callback || !aosp_debug.data)
		return;

	va_start(ap, format);
	vsnprintf(str, sizeof(str), format, ap);
	aosp_debug.callback(str, aosp_debug.data);
	va_end(ap);
}

static void print_quality_report_evt(const struct aosp_bqr *bqr)
{
	debug("AOSP Quality Report");
	debug("  quality_report_id %u", bqr->quality_report_id);
	debug("  packet_type %u", bqr->packet_type);
	debug("  conn_handle %u", bqr->conn_handle);
	debug("  conn_role %u", bqr->conn_role);
	debug("  tx_power_level %d", bqr->tx_power_level);
	debug("  rssi %d", bqr->rssi);
	debug("  snr %u", bqr->snr);
	debug("  unused_afh_channel_count %u", bqr->unused_afh_channel_count);
	debug("  afh_select_unideal_channel_count %u",
					bqr->afh_select_unideal_channel_count);
	debug("  lsto %.2f", bqr->lsto * 0.625);
	debug("  conn_piconet_clock %.2f", bqr->conn_piconet_clock * 0.3125);
	debug("  retransmission_count %u", bqr->retransmission_count);
	debug("  no_rx_count %u", bqr->no_rx_count);
	debug("  nak_count %u", bqr->nak_count);
	debug("  last_tx_ack_timestamp %.2f", bqr->last_tx_ack_timestamp *
					0.3125);
	debug("  flow_off_count %u", bqr->flow_off_count);
	debug("  last_flow_on_timestamp %.2f", bqr->last_flow_on_timestamp *
					0.3125);
	debug("  buffer_overflow_bytes %u", bqr->buffer_overflow_bytes);
	debug("  buffer_underflow_bytes %u", bqr->buffer_underflow_bytes);
}

bool process_aosp_quality_report(const struct mgmt_ev_quality_report *ev)
{
	const struct aosp_bqr *ev_report;
	struct aosp_bqr bqr;

	if (ev->report_len < sizeof(struct aosp_bqr)) {
		debug("error: AOSP report size %u too small (expect >= %u).",
				ev->report_len, sizeof(struct aosp_bqr));
		return false;
	}

	ev_report = (struct aosp_bqr *)ev->report;

	/* Ignore the Vendor Specific Parameter (VSP) field for now
	 * due to the lack of standard way of reading it.
	 */
	bqr.quality_report_id = ev_report->quality_report_id;
	bqr.packet_type = ev_report->packet_type;
	bqr.conn_handle = btohs(ev_report->conn_handle);
	bqr.conn_role = ev_report->conn_role;
	bqr.tx_power_level = ev_report->tx_power_level;
	bqr.rssi = ev_report->rssi;
	bqr.snr = ev_report->snr;
	bqr.unused_afh_channel_count = ev_report->unused_afh_channel_count;
	bqr.afh_select_unideal_channel_count =
				ev_report->afh_select_unideal_channel_count;
	bqr.lsto = btohs(ev_report->lsto);
	bqr.conn_piconet_clock = btohl(ev_report->conn_piconet_clock);
	bqr.retransmission_count = btohl(ev_report->retransmission_count);
	bqr.no_rx_count = btohl(ev_report->no_rx_count);
	bqr.nak_count = btohl(ev_report->nak_count);
	bqr.last_tx_ack_timestamp = btohl(ev_report->last_tx_ack_timestamp);
	bqr.flow_off_count = btohl(ev_report->flow_off_count);
	bqr.last_flow_on_timestamp = btohl(ev_report->last_flow_on_timestamp);
	bqr.buffer_overflow_bytes = btohl(ev_report->buffer_overflow_bytes);
	bqr.buffer_underflow_bytes = btohl(ev_report->buffer_underflow_bytes);

	print_quality_report_evt(&bqr);

	return true;
}
