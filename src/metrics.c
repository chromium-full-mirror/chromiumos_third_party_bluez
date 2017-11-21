/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "metrics.h"

#include <glib.h>
#include <time.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"
#include "log.h"
#include "metrics/c_metrics_library.h"

/* The default value of number of buckets used in Count histogram. */
#define DEFAULT_BUCKETS_NUM	50

/* The lower and upper bounds of time length samples. */
#define TIME_LENGTH_MAX		1800.00  // A half hour in seconds
#define TIME_LENGTH_MIN		0.00

/* Discovery type maps to lib/mgmt.h and src/adapter.c */
#define BLUEZ_DISCOVERY_TYPE_BREDR (1 << BDADDR_BREDR)
#define BLUEZ_DISCOVERY_TYPE_LE ((1 << BDADDR_LE_PUBLIC) | \
					(1 << BDADDR_LE_RANDOM))
#define BLUEZ_DISCOVERY_TYPE_DUAL (BLUEZ_DISCOVERY_TYPE_BREDR | \
					BLUEZ_DISCOVERY_TYPE_LE)

struct metrics_timer {
	metrics_timer_type type;
	time_t start;
	struct metrics_timer_data data;
};

static CMetricsLibrary lib = NULL;
static GSList *timers = NULL;

static int metrics_timer_match(gconstpointer a, gconstpointer b)
{
	const struct metrics_timer *timer_a = (struct metrics_timer *)a;
	const struct metrics_timer *timer_b = (struct metrics_timer *)b;

	if (timer_a == timer_b)
		return 0;

	if (!timer_a || !timer_b)
		return -1;

	if (timer_a->type != timer_b->type)
		return -1;

	if (timer_a->data.adapter != timer_b->data.adapter ||
		timer_a->data.device != timer_b->data.device ||
		timer_a->data.adv_client != timer_b->data.adv_client) {
		return -1;
	}

	return 0;
}

static struct metrics_timer *metrics_timer_create(metrics_timer_type type,
					struct metrics_timer_data data)
{
	struct metrics_timer *timer = NULL;

	switch(type) {
	case TIMER_DISCOVERABLE:
	case TIMER_DISCOVERY:
	case TIMER_PAIRING:
	case TIMER_ADVERTISEMENT:
	case TIMER_CONNECT:
        case TIMER_ADAPTER_LOST:
		timer = g_new0(struct metrics_timer, 1);
		if (!timer)
			break;
		time(&timer->start);
		timer->type = type;
		timer->data = data;
		break;
	default:
		break;
	}

	return timer;
}

static int convert_discovery_type(int sample)
{
	switch(sample) {
	case BLUEZ_DISCOVERY_TYPE_BREDR:
		return DISCOVERY_TYPE_BREDR;
	case BLUEZ_DISCOVERY_TYPE_LE:
		return DISCOVERY_TYPE_LE;
	case BLUEZ_DISCOVERY_TYPE_DUAL:
		return DISCOVERY_TYPE_DUAL;
	default:
		return 0;
	}
}

static metrics_device_type convert_device_type(int sample)
{
	switch(sample) {
	case BDADDR_BREDR:
		return DEVICE_TYPE_BREDR;
	case BDADDR_LE_PUBLIC:
		return DEVICE_TYPE_LE_PUBLIC;
	case BDADDR_LE_RANDOM:
		return DEVICE_TYPE_LE_RANDOM;
	default:
		return 0;
	}
}

static metrics_adv_reg_result convert_adv_reg_result(int sample)
{
	switch(sample) {
	case MGMT_STATUS_SUCCESS:
		return ADV_SUCCEED;
	case MGMT_STATUS_INVALID_PARAMS:
		return ADV_FAIL_INVALID_PARAMS;
	case MGMT_STATUS_REJECTED:
		return ADV_FAIL_LE_DISABLED;
	case MGMT_STATUS_BUSY:
		return ADV_FAIL_BUSY;
	case MGMT_STATUS_FAILED:
		return ADV_FAIL_CREATE_CLIENT;
	case MGMT_STATUS_NOT_SUPPORTED:
		return ADV_FAIL_LE_UNSUPPORTED;
	default:
		return ADV_FAIL_UNKNOWN;
	}
}

static metrics_disconn_reason convert_disconn_reason(int sample) {
	switch(sample) {
	case MGMT_DEV_DISCONN_LOCAL_HOST:
		return DISCONN_LOCAL_HOST;
	case MGMT_DEV_DISCONN_REMOTE:
		return DISCONN_REMOTE;
	case MGMT_DEV_DISCONN_TIMEOUT:
		return DISCONN_SUPERVISION_TIMEOUT;
	case MGMT_DEV_DISCONN_UNKNOWN:
	default:
		return DISCONN_UNKNOWN;
	}
}

static metrics_pair_result convert_pair_result(int sample)
{
	switch(sample) {
	case MGMT_STATUS_SUCCESS:
		return PAIR_SUCCEED;
	case MGMT_STATUS_NOT_POWERED:
		return PAIR_FAIL_NONPOWERED;
	case MGMT_STATUS_ALREADY_PAIRED:
		return PAIR_FAIL_ALREAY_PAIRED;
	case MGMT_STATUS_INVALID_PARAMS:
		return PAIR_FAIL_INVALID_PARAMS;
	case MGMT_STATUS_BUSY:
		return PAIR_FAIL_BUSY;
	case MGMT_STATUS_NOT_SUPPORTED:
		return PAIR_FAIL_NOT_SUPPORTED;
	case MGMT_STATUS_NO_RESOURCES:  // flow through
	case MGMT_STATUS_REJECTED:
		return PAIR_FAIL_AUTH_REJECTED;
	case MGMT_STATUS_DISCONNECTED:  // flow through
	case MGMT_STATUS_CANCELLED:
		return PAIR_FAIL_AUTH_CANCELLED;
	case MGMT_STATUS_CONNECT_FAILED:
		return PAIR_FAIL_ESTABLISH_CONN;
	case MGMT_STATUS_TIMEOUT:
		return PAIR_FAIL_AUTH_TIMEOUT;
	case MGMT_STATUS_AUTH_FAILED:
		return PAIR_FAIL_AUTH_FAILED;
	default :
		return PAIR_FAIL_UNKNOWN;
	}
}

bool metrics_init(void)
{
	if (lib)
		return true;

	lib = CMetricsLibraryNew();
	if (!lib)
		return false;

	timers = g_slist_alloc();
	if (!timers)
		return false;

	CMetricsLibraryInit(lib);
	return true;
}

void metrics_deinit(void)
{
	g_slist_free_full(timers, g_free);

	if (lib) {
		CMetricsLibraryDelete(lib);
		lib = NULL;
	}
}

int metrics_is_enabled(void)
{
	return CMetricsLibraryAreMetricsEnabled(lib);
}

bool metrics_send(const char *name, int sample, int min, int max, int buckets)
{
	if (!lib || !name)
		return false;

	if (min < 0 || sample < min || sample >= max || buckets < 1) {
		DBG("Invalid sample:%d min:%d max:%d", sample, min, max);
		return false;
	}

	CMetricsLibrarySendToUMA(lib, name, sample, min, max, buckets);
	return true;
}

bool metrics_send_enum(metrics_send_enum_type type, int sample,
			bool is_mgmt_status)
{
	int max = 0;
	char *histogram;

	if (!lib)
		return false;

	// According to Metrics library, here are requirements for samples and
	// buckets:
	// - 1 <= |sample| < |max|
	// - An enumeration histogram requires |max| + 1 number of buckets.
	// Therefore, we convert the sample into corresponding value defined in
	// metrics.h.
	switch(type) {
	case ENUM_TYPE_DISCOVERY:
		histogram = H_NAME_DISCOVERY_TYPE;
		sample = convert_discovery_type(sample);
		max = DISCOVERY_TYPE_END;
		break;
	case ENUM_TYPE_FOUND_DEVICE:
		histogram = H_NAME_FOUND_DEVICE_TYPE;
		sample = convert_device_type(sample);
		max = DEVICE_TYPE_END;
		break;
	case ENUM_TYPE_ADV_REG_RESULT:
		histogram = H_NAME_ADV_REG_RESULT;
		if (is_mgmt_status)
			sample = convert_adv_reg_result(sample);
		max = ADV_FAIL_END;
		break;
	case ENUM_TYPE_DISCONN_REASON:
		histogram = H_NAME_DISCONN_REASON;
		if (is_mgmt_status)
			sample = convert_disconn_reason(sample);
		max = DISCONN_END;
		break;
	case ENUM_TYPE_PAIR_RESULT:
		histogram = H_NAME_PAIR_RESULT;
		if (is_mgmt_status)
			sample = convert_pair_result(sample);
		max = PAIR_FAIL_END;
		break;
	case ENUM_TYPE_CONN_RESULT:
		histogram = H_NAME_CONN_RESULT;
		max = CONN_FAIL_END;
		break;
	default:
		DBG("Invalid enum type:%d", type);
		return false;
	}

	if (sample <= 0 || sample >= max) {
		DBG("Invalid sample:%d, max:%d type:%d", sample, max, type);
		return false;
	}

	CMetricsLibrarySendEnumToUMA(lib, histogram, sample, max);
	return true;
}

/* Returns true if the timer is set successfully; false otherwise. */
bool metrics_start_timer(metrics_timer_type type,
			struct metrics_timer_data data)
{
	GSList *match = NULL;
	struct metrics_timer *timer = NULL;
	struct metrics_timer *old_timer = NULL;

	if (!lib)
		return false;

	switch(type) {
	case TIMER_PAIRING:
	case TIMER_CONNECT:
		if(!data.adapter || !data.device || data.adv_client)
			return false;
		break;
	case TIMER_DISCOVERABLE:
	case TIMER_DISCOVERY:
		if (!data.adapter || data.device || data.adv_client)
			return false;
		break;
	case TIMER_ADVERTISEMENT:
		if (!data.adv_client || data.adapter || data.device)
			return false;
		break;
	case TIMER_ADAPTER_LOST:
		if (data.adapter || data.device || data.adv_client)
			return false;
		break;
	default:
		return false;
	}

	timer = metrics_timer_create(type, data);
	if (!timer)
		return false;

	match = g_slist_find_custom(timers, timer, metrics_timer_match);
	if (match) {
		// Replace the invalid old timer with the new one.
		old_timer = (struct metrics_timer *)match->data;
		match->data = timer;
		g_free(old_timer);
		return true;
	}

	timers = g_slist_append(timers, timer);
	return true;
}

void metrics_cancel_timer(metrics_timer_type type,
				struct metrics_timer_data data)
{
	struct metrics_timer *t = NULL;
	struct metrics_timer *timer = NULL;
	GSList *match = NULL;

	t = metrics_timer_create(type, data);
	match = g_slist_find_custom(timers, t, metrics_timer_match);
	if (!match)
		goto no_match;

	timer = (struct metrics_timer *)match->data;
	timers = g_slist_remove(timers, timer);
no_match:
	g_free(timer);
	g_free(t);
}

/* Returns true if the timer is found and the sample is emitted; false
 * otherwise.
 */
bool metrics_stop_timer(metrics_timer_type type,
			struct metrics_timer_data data)
{
	struct metrics_timer *t = NULL;
	struct metrics_timer *timer = NULL;
	GSList *match = NULL;
	time_t cur_time;
	const char *name;
	double time_len = 0;
	int sample;
	bool emitted = false;

	if (!lib)
		return emitted;

	time(&cur_time);

	t = metrics_timer_create(type, data);
	match = g_slist_find_custom(timers, t, metrics_timer_match);
	if (!match)
		goto failed;

	timer = (struct metrics_timer *)match->data;
	if (!timer)
		goto failed;

	time_len = difftime(cur_time, timer->start);
	if (time_len < TIME_LENGTH_MIN)
		goto failed;

	// If a sample is greater than 0 and less than 1, it should be rounded
	// up to 1, otherwise we will lose the sample. If a sample is greater
	// than the maximum time length, it should be rounded down to the
	// maximum time length.
	sample = time_len >= TIME_LENGTH_MAX ? TIME_LENGTH_MAX - 1 : time_len;
	if (time_len < 1 && time_len > TIME_LENGTH_MIN)
		sample = 1;

	switch(timer->type) {
	case TIMER_DISCOVERABLE:
		name = H_NAME_DISCOVERABLE_LEN;
		break;
	case TIMER_DISCOVERY:
		name = H_NAME_DISCOVERY_LEN;
		break;
	case TIMER_PAIRING:
		name = H_NAME_PAIRING_LEN;
		break;
	case TIMER_ADVERTISEMENT:
		name = H_NAME_ADV_LEN;
		break;
	case TIMER_CONNECT:
		name = H_NAME_CONN_LEN;
		break;
	case TIMER_ADAPTER_LOST:
		name = H_NAME_ADAPTER_LOST;
		break;
	default:
		goto failed;
	}

	emitted = metrics_send(name, sample, TIME_LENGTH_MIN, TIME_LENGTH_MAX,
				DEFAULT_BUCKETS_NUM);
failed:
	timers = g_slist_remove(timers, timer);
	g_free(timer);
	g_free(t);
	return emitted;
}
