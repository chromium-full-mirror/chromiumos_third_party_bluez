/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "metrics.h"

#include <errno.h>
#include <glib.h>
#include <math.h>
#include <time.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"
#include "lib/uuid.h"
#include "log.h"
#include "metrics/c_metrics_library.h"
#include "metrics/c_structured_metrics.h"
#include "src/adv_monitor.h"
#include "src/metrics_allowlist.h"
#include "src/shared/aosp.h"
#include "src/shared/bounded-priority-queue.h"
#include "src/shared/intel.h"
#include "src/shared/queue.h"

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

#define ADV_MON_ACTIVE_DURATION		60   // 1 minute
#define ADV_MON_IDLE_DURATION		300  // 5 minutes
#define ADV_MON_MAX_NUM_OF_MONITOR	32
#define ADV_MON_MAX_ADV_PER_MINUTE	1000 // somewhat arbitrary number

#define STATE_CHANGE_TYPE_DISCONNECT 0
#define STATE_CHANGE_TYPE_CONNECT 1

#define DEVICE_MAJOR_CLASS_MASK 0x1F00
#define DEVICE_MAJOR_CLASS_BIT_OFFSET 8
#define DEVICE_CATEGORY_MASK 0xFFC0
#define DEVICE_CATEGORY_BIT_OFFSET 6

#define BOOT_ID_PROC_PATH "/proc/sys/kernel/random/boot_id"
#define CHIPSET_INFO_WLAN_DIR_PATH "/sys/class/net/wlan0/device"
#define CHIPSET_INFO_MLAN_DIR_PATH "/sys/class/net/mlan0/device"
#define CHIPSET_INFO_MODALIAS_PATH "/sys/class/bluetooth/hci0/device/modalias"
#define CHIPSET_INFO_MODULE_DIR_PATH                                           \
	"/sys/class/bluetooth/hci0/device/driver/module"

enum metrics_transport_type {
	TRANSPORT_TYPE_UNKNOWN = 0,
	TRANSPORT_TYPE_USB = 1,
	TRANSPORT_TYPE_UART = 2,
	TRANSPORT_TYPE_SDIO = 3,
};

struct metrics_chipset_info {
	int vid;
	int pid;
	enum metrics_transport_type transport;
	char *chipset_string;
};

#define AUDIO_QUALITY_TIME_CHUNK 5.0		// seconds
#define AUDIO_QUALITY_REPORT_MIN_DELTA_TIME 0.1	// seconds
#define AUDIO_QUALITY_REPORT_MAX_DELTA_TIME 9.9	// seconds
#define PERCENTILE95_BUFFER_SIZE 255

#define BQR_QUALITY_ID_A2DP_CHOPPY 0x03
#define BQR_QUALITY_ID_SCO_CHOPPY 0x04

struct metrics_timer {
	metrics_timer_type type;
	struct timespec start;
	struct metrics_timer_data data;
};

struct metrics_periodic_timer {
	enum metrics_periodic_timer_type type;
	int active_period;
	int idle_period;
	int init_value;
	void *user_data;
	metric_periodic_timer_update_func_t on_update;
	metric_periodic_timer_report_func_t on_report;
	guint timer;
	int value;
	bool active;
};

static CMetricsLibrary lib = NULL;
static GSList *timers = NULL;
static struct queue *periodic_timers;
static struct metrics_chipset_info *chipset_info;
static char *boot_id;

static void metrics_free_periodic_timer(void *user_data);

static void metrics_audio_connect_a2dp(void);
static void metrics_audio_connect_hfp(void);
static void metrics_audio_disconnect_a2dp(const char *device_id);
static void metrics_audio_disconnect_hfp(const char *device_id);

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
	case TIMER_CHIP_LOST:
	case TIMER_CHIP_LOST2:
		timer = g_new0(struct metrics_timer, 1);
		if (!timer)
			break;
		clock_gettime(CLOCK_MONOTONIC, &timer->start);
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

static metrics_pair_result convert_mgmt_pair_result(int sample)
{
	switch(sample) {
	case MGMT_STATUS_SUCCESS:
		return PAIR_SUCCEED;
	case MGMT_STATUS_NOT_POWERED:
		return PAIR_FAIL_NONPOWERED;
	case MGMT_STATUS_ALREADY_PAIRED:
		return PAIR_FAIL_ALREADY_PAIRED;
	case MGMT_STATUS_INVALID_PARAMS:
		return PAIR_FAIL_INVALID_PARAMS;
	case MGMT_STATUS_BUSY:
		return PAIR_FAIL_BUSY;
	case MGMT_STATUS_NOT_SUPPORTED:
		return PAIR_FAIL_NOT_SUPPORTED;
	case MGMT_STATUS_NO_RESOURCES:
		return PAIR_FAIL_NO_RESOURCES;
	case MGMT_STATUS_REJECTED:
		return PAIR_FAIL_REJECTED;
	case MGMT_STATUS_DISCONNECTED:
		return PAIR_FAIL_DISCONNECTED;
	case MGMT_STATUS_CANCELLED:
		return PAIR_FAIL_CANCELLED;
	case MGMT_STATUS_CONNECT_FAILED:
		return PAIR_FAIL_ESTABLISH_CONN;
	case MGMT_STATUS_TIMEOUT:
		return PAIR_FAIL_TIMEOUT;
	case MGMT_STATUS_AUTH_FAILED:
		return PAIR_FAIL_AUTH_FAILED;
	case MGMT_STATUS_UNKNOWN_COMMAND:
		return PAIR_FAIL_UNKNOWN_COMMAND;
	case MGMT_STATUS_NOT_CONNECTED:
		return PAIR_FAIL_NOT_CONNECTED;
	case MGMT_STATUS_FAILED:
		return PAIR_FAIL_FAILED;
	default:
		return PAIR_FAIL_UNKNOWN;
	}
}

static metrics_pair_result convert_system_pair_result(int sample)
{
	switch(sample) {
	case -EALREADY:  // fall through
	case -EBUSY:
		return PAIR_FAIL_BUSY;
	case -EIO:
		return PAIR_FAIL_BT_IO_CONNECT_ERROR;
	default:
		return PAIR_FAIL_UNKNOWN;
	}
}

metrics_conn_result metrics_bredr_conn_err_to_result(int sample)
{
	switch (-sample) {
	case 0:
		return CONN_BREDR_SUCCEED;
	case EISCONN:
	case EALREADY:
		return CONN_ALREADY_BREDR;
	case EHOSTDOWN:
		return CONN_FAIL_BREDR_PAGE_TIMEOUT;
	case EHOSTUNREACH: /* adapter not powered */
		return CONN_FAIL_NONPOWERED;
	case EIO:
		return CONN_FAIL_IO_CONNECT_BREDR;
	case EPERM:
		return CONN_FAIL_NOT_PERMITTED;
	case ECANCELED:
		return CONN_FAIL_CANCELED_BREDR;
	case ENOENT: /* Service is unavailable on the remote device */
		return CONN_FAIL_BREDR_PROFILE_UNAVAILABLE;
	case EINVAL:
		return CONN_FAIL_INVALID_PARAMS_BREDR;
	case EOPNOTSUPP: /* Fall through */
	case EPROTONOSUPPORT: /* Service is not supported or disabled */
		return CONN_FAIL_NOT_SUPPORTED_BREDR;
	case EBADFD:
		return CONN_FAIL_BAD_SOCKET_BREDR;
	case ENOPROTOOPT:
		return CONN_FAIL_BREDR_PROFILE_UNAVAILABLE;
	case EBUSY:
		return CONN_FAIL_BUSY_BREDR;
	case ENOMEM:
		return CONN_FAIL_MEMORY_ALLOC_BREDR;
	case EMLINK:
		return CONN_FAIL_SYNC_CONNECT_LIMIT_BREDR;
	case ETIMEDOUT:
		return CONN_FAIL_TIMEDOUT_BREDR;
	case ECONNREFUSED:
		return CONN_FAIL_REFUSED_BREDR;
	case ECONNRESET:
		return CONN_FAIL_TERM_BY_REMOTE_BREDR;
	case ECONNABORTED:
		return CONN_FAIL_TERM_BY_LOCAL_BREDR;
	case EPROTO:
		return CONN_FAIL_PROTO_ERROR_BREDR;
	default:
		return CONN_FAIL_BREDR;
	}
}

metrics_conn_result metrics_le_conn_err_to_result(int sample)
{
	switch (-sample) {
	case 0:
		return CONN_LE_SUCCEED;
	case EINVAL:
		return CONN_FAIL_INVALID_PARAMS_LE;
	case EHOSTUNREACH:
		return CONN_FAIL_NO_DEV_LE;
	case EOPNOTSUPP:
		return CONN_FAIL_NOT_SUPPORTED_LE;
	case EISCONN:
	case EALREADY:
		return CONN_ALREADY_LE;
	case ENOMEM:
		return CONN_FAIL_MEMORY_ALLOC_LE;
	case EBUSY:
		return CONN_FAIL_BUSY_LE;
	case ECONNREFUSED:
		return CONN_FAIL_REFUSED_LE;
	case EIO:
		return CONN_FAIL_IO_CONNECT_LE;
	case EBADFD:
		return CONN_FAIL_BAD_SOCKET_LE;
	case ETIMEDOUT:
		return CONN_FAIL_TIMEDOUT_LE;
	case EMLINK:
		return CONN_FAIL_SYNC_CONNECT_LIMIT_LE;
	case ECONNRESET:
		return CONN_FAIL_TERM_BY_REMOTE_LE;
	case ECONNABORTED:
		return CONN_FAIL_TERM_BY_LOCAL_LE;
	case EPROTO:
		return CONN_FAIL_PROTO_ERROR_LE;
	default:
		return CONN_FAIL_LE;
	}
}

static metrics_profile_probe_result convert_profile_probe_result(int sample)
{
	switch (sample) {
	case 0:
		return PROFILE_PROBE_SUCCEED;
	case -EINVAL:
		return PROFILE_PROBE_UNABLE_TO_REGISTER_INTERFACE;
	case -EIO:
		return PROFILE_PROBE_UNABLE_TO_CREATE_NEW_DEVICE;
	case -ENOENT:
		return PROFILE_PROBE_PROFILE_NOT_SUPPORTED;
	default:
		return PROFILE_PROBE_UNKNOWN_ERROR;
	}
}

static metrics_profile_conn_result convert_profile_conn_result(int sample)
{
	switch (sample) {
	case 0:
		return PROFILE_CONN_SUCCEED;
	case -EALREADY:
		return PROFILE_CONN_ALREADY_CONNECTED;
	case -EBUSY:
		return PROFILE_CONN_BUSY_CONNECTING;
	case -ECONNREFUSED:
	case -EAGAIN:
		return PROFILE_CONN_CONNECTION_REFUSED;
	case -ECANCELED:
		return PROFILE_CONN_CONNECT_CANCELED;
	case -EHOSTDOWN:
	case -EHOSTUNREACH:
		return PROFILE_CONN_REMOTE_UNAVAILABLE;
	case -EPROTONOSUPPORT:
	case -ENOPROTOOPT:
	case -ENOENT:
	case -ENOTSUP:
		return PROFILE_CONN_PROFILE_NOT_SUPPORTED;
	default:
		return PROFILE_CONN_UNKNOWN_ERROR;
	}
}

static enum metrics_advmon_result convert_advmon_result(int sample)
{
	switch (sample) {
	case MGMT_STATUS_SUCCESS:
		return ADVMON_RESULT_SUCCEED;
	case MGMT_STATUS_INVALID_PARAMS:
		return ADVMON_RESULT_BAD_PARAM;
	case MGMT_STATUS_NO_RESOURCES:
		return ADVMON_RESULT_NO_RESOURCE;
	case MGMT_STATUS_BUSY:
		return ADVMON_RESULT_BUSY;
	default:
		return ADVMON_RESULT_UNKNOWN_ERROR;
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

	periodic_timers = queue_new();
	if (!periodic_timers)
		return false;

	return true;
}

void metrics_deinit(void)
{
	g_slist_free_full(timers, g_free);
	if (periodic_timers) {
		queue_destroy(periodic_timers, metrics_free_periodic_timer);
		periodic_timers = NULL;
	}

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
			metrics_result_type result_type)
{
	int max = 0;
	int min = 0;
	char *histogram;

	if (!lib)
		return false;

	// According to Metrics library, we should satisfy:
	// - 0 <= |sample| < |max|
	// However the older metrics starts at 1 (skips 0). We must preserve
	// this condition because the enums must not be reordered.
	switch(type) {
	case ENUM_TYPE_DISCOVERY:
		histogram = H_NAME_DISCOVERY_TYPE;
		sample = convert_discovery_type(sample);
		max = DISCOVERY_TYPE_END;
		min = 1;
		break;
	case ENUM_TYPE_FOUND_DEVICE:
		histogram = H_NAME_FOUND_DEVICE_TYPE;
		sample = convert_device_type(sample);
		max = DEVICE_TYPE_END;
		min = 1;
		break;
	case ENUM_TYPE_ADV_REG_RESULT:
		histogram = H_NAME_ADV_REG_RESULT;
		if (result_type == RESULT_TYPE_MGMT)
			sample = convert_adv_reg_result(sample);
		max = ADV_FAIL_END;
		min = 1;
		break;
	case ENUM_TYPE_DISCONN_REASON:
		histogram = H_NAME_DISCONN_REASON;
		if (result_type == RESULT_TYPE_MGMT)
			sample = convert_disconn_reason(sample);
		max = DISCONN_END;
		min = 1;
		break;
	case ENUM_TYPE_PAIR_RESULT:
		histogram = H_NAME_PAIR_RESULT;
		if (result_type == RESULT_TYPE_MGMT)
			sample = convert_mgmt_pair_result(sample);
		else if (result_type == RESULT_TYPE_SYSTEM)
			sample = convert_system_pair_result(sample);
		max = PAIR_FAIL_END;
		min = 1;
		break;
	case ENUM_TYPE_CONN_RESULT:
		histogram = H_NAME_CONN_RESULT;
		max = CONN_FAIL_END;
		min = 1;
		break;
	case ENUM_TYPE_ADVMON_SW_ADD_RESULT:
		histogram = H_NAME_ADVMON_SW_ADD_RESULT;
		sample = convert_advmon_result(sample);
		max = ADVMON_RESULT_END;
		break;
	case ENUM_TYPE_ADVMON_SW_REMOVE_RESULT:
		histogram = H_NAME_ADVMON_SW_REMOVE_RESULT;
		sample = convert_advmon_result(sample);
		max = ADVMON_RESULT_END;
		break;
	case ENUM_TYPE_ADVMON_MSFT_ADD_RESULT:
		histogram = H_NAME_ADVMON_MSFT_ADD_RESULT;
		sample = convert_advmon_result(sample);
		max = ADVMON_RESULT_END;
		break;
	case ENUM_TYPE_ADVMON_MSFT_REMOVE_RESULT:
		histogram = H_NAME_ADVMON_MSFT_REMOVE_RESULT;
		sample = convert_advmon_result(sample);
		max = ADVMON_RESULT_END;
		break;
	default:
		DBG("Invalid enum type:%d", type);
		return false;
	}

	if (sample < min || sample >= max) {
		DBG("Invalid sample:%d, min:%d max:%d type:%d",
							sample, min, max, type);
		return false;
	}

	CMetricsLibrarySendEnumToUMA(lib, histogram, sample, max);
	return true;
}

bool metrics_send_per_profile_enum(metrics_per_profile_type type,
				   const char *uuid, int sample)
{
	int max;
	char *histogram;

	if (!lib)
		return false;

	if (bt_uuid_strcmp(uuid, HID_UUID) == 0) {
		histogram = (type == PROFILE_PROBE_RESULT) ?
				    H_NAME_HID_PROBE_RESULT :
				    H_NAME_HID_CONN_RESULT;
	} else if (bt_uuid_strcmp(uuid, HOG_UUID) == 0) {
		histogram = (type == PROFILE_PROBE_RESULT) ?
				    H_NAME_HOG_PROBE_RESULT :
				    H_NAME_HOG_CONN_RESULT;
	} else if (bt_uuid_strcmp(uuid, A2DP_SINK_UUID) == 0) {
		histogram = (type == PROFILE_PROBE_RESULT) ?
				    H_NAME_A2DP_SINK_PROBE_RESULT :
				    H_NAME_A2DP_SINK_CONN_RESULT;
	} else if (bt_uuid_strcmp(uuid, HFP_AG_UUID) == 0 ||
		   bt_uuid_strcmp(uuid, HFP_HS_UUID) == 0) {
		histogram = (type == PROFILE_PROBE_RESULT) ?
				    H_NAME_HFP_PROBE_RESULT :
				    H_NAME_HFP_CONN_RESULT;
	} else if (bt_uuid_strcmp(uuid, AVRCP_REMOTE_UUID) == 0) {
		histogram = (type == PROFILE_PROBE_RESULT) ?
				    H_NAME_AVRCP_PROBE_RESULT :
				    H_NAME_AVRCP_CONN_RESULT;
	} else if (bt_uuid_strcmp(uuid, BATTERY_UUID) == 0) {
		histogram = (type == PROFILE_PROBE_RESULT) ?
				    H_NAME_BATTERY_PROBE_RESULT :
				    H_NAME_BATTERY_CONN_RESULT;
	} else {
		/* do not report metrics for any other profile */
		return false;
	}

	switch (type) {
	case PROFILE_PROBE_RESULT:
		sample = convert_profile_probe_result(sample);
		max = PROFILE_PROBE_END;

		/* HOG device_probe returns -EINVAL in case it fails
		 * to create a device, so handle it separately
		 */
		if (bt_uuid_strcmp(uuid, HOG_UUID) == 0 && sample == -EINVAL)
			sample = PROFILE_PROBE_UNABLE_TO_CREATE_NEW_DEVICE;
		break;
	case PROFILE_CONN_RESULT:
		sample = convert_profile_conn_result(sample);
		max = PROFILE_CONN_END;
		break;
	default:
		DBG("Invalid type:%d", type);
		return false;
	}

	if (sample < 0 || sample >= max) {
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
	case TIMER_CHIP_LOST:
	case TIMER_CHIP_LOST2:
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
	struct timespec cur_time;
	const char *name;
	double time_len = 0;
	int sample;
	bool emitted = false;

	if (!lib)
		return emitted;

	clock_gettime(CLOCK_MONOTONIC, &cur_time);

	t = metrics_timer_create(type, data);
	match = g_slist_find_custom(timers, t, metrics_timer_match);
	if (!match)
		goto failed;

	timer = (struct metrics_timer *)match->data;
	if (!timer)
		goto failed;

	time_len = cur_time.tv_sec - timer->start.tv_sec;
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
	case TIMER_CHIP_LOST:
		name = H_NAME_CHIP_LOST;
		break;
	case TIMER_CHIP_LOST2:
		name = H_NAME_CHIP_LOST2;
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

static bool metrics_periodic_timer_report(enum metrics_periodic_timer_type type,
						int value)
{
	const char *name = NULL;
	int min_val, max_val;
	int bucket_num = DEFAULT_BUCKETS_NUM;

	switch (type) {
	case PERIODIC_TIMER_NUM_MONITOR:
		name = H_NAME_ADVMON_NUM_MONITOR;
		min_val = 0;
		max_val = ADV_MON_MAX_NUM_OF_MONITOR;
		// Undocumented behavior seems to prevent tracking count
		// histograms with bucket_num greater than max_val + 1.
		// This is the value they used in EXACT_LINEAR histogram.
		bucket_num = max_val + 1;
		break;
	case PERIODIC_TIMER_SW_PATTERN_ADV_PER_MINUTE:
		name = H_NAME_ADVMON_SW_PATTERN_ADV_PER_MINUTE;
		min_val = 0;
		max_val = ADV_MON_MAX_ADV_PER_MINUTE;
		break;
	case PERIODIC_TIMER_MSFT_PATTERN_ADV_PER_MINUTE:
		name = H_NAME_ADVMON_MSFT_PATTERN_ADV_PER_MINUTE;
		min_val = 0;
		max_val = ADV_MON_MAX_ADV_PER_MINUTE;
		break;
	default:
		DBG("Invalid enum type: %d", type);
		return false;
	}

	return metrics_send(name, value, min_val, max_val, bucket_num);
}

static gboolean metrics_periodic_timer_timeout(void *user_data)
{
	struct metrics_periodic_timer *timer = user_data;
	int next_period;

	if (timer->active) {
		if (timer->on_report)
			timer->value = timer->on_report(timer->value,
							timer->user_data);
		metrics_periodic_timer_report(timer->type, timer->value);
		next_period = timer->idle_period;
	} else {
		timer->value = timer->init_value;
		next_period = timer->active_period;
	}

	timer->active = !timer->active;
	timer->timer = g_timeout_add_seconds(next_period,
						metrics_periodic_timer_timeout,
						timer);

	return FALSE;
}

struct metrics_periodic_timer *metrics_start_periodic_timer(
				enum metrics_periodic_timer_type type,
				int active_period, int idle_period,
				int init_value, void *user_data,
				metric_periodic_timer_update_func_t on_update,
				metric_periodic_timer_report_func_t on_report)
{
	struct metrics_periodic_timer *timer;

	timer = g_new0(struct metrics_periodic_timer, 1);
	if (!timer)
		return NULL;

	if (!queue_push_tail(periodic_timers, timer)) {
		g_free(timer);
		return NULL;
	}

	timer->type = type;
	timer->active_period = active_period;
	timer->idle_period = idle_period;
	timer->user_data = user_data;
	timer->init_value = init_value;
	timer->on_update = on_update;
	timer->on_report = on_report;
	timer->value = init_value;
	timer->active = true;
	timer->timer = g_timeout_add_seconds(active_period,
						metrics_periodic_timer_timeout,
						timer);

	return timer;
}

static void metrics_free_periodic_timer(void *user_data)
{
	struct metrics_periodic_timer *timer = user_data;

	if (timer->timer)
		g_source_remove(timer->timer);

	g_free(timer);
}

bool metrics_stop_periodic_timer(struct metrics_periodic_timer *timer)
{
	if (!timer || !queue_remove(periodic_timers, timer))
		return false;

	metrics_free_periodic_timer(timer);
	return true;
}

bool metrics_update_periodic_timer_value(struct metrics_periodic_timer *timer,
								void *user_data)
{
	if (!timer || !timer->on_update || !timer->active)
		return false;

	timer->value = timer->on_update(timer->value, timer->user_data,
								user_data);
	return true;
}

static struct metrics_periodic_timer *metrics_get_periodic_timer(
					enum metrics_periodic_timer_type type,
					void *user_data)
{
	const struct queue_entry *e;
	GSList *l;

	if (!lib)
		return NULL;

	for (e = queue_get_entries(periodic_timers); e; e = e->next) {
		struct metrics_periodic_timer *timer = e->data;

		if (timer->type == type && timer->user_data == user_data)
			return timer;
	}

	return NULL;
}

static enum metrics_periodic_timer_type metrics_get_advmon_type(
					struct btd_adv_monitor_manager *manager)
{
	if (btd_adv_monitor_get_offload_support(manager))
		return PERIODIC_TIMER_MSFT_PATTERN_ADV_PER_MINUTE;

	return PERIODIC_TIMER_SW_PATTERN_ADV_PER_MINUTE;
}

static int metrics_update_adv_count(int current, void *data, void *user_data)
{
	return current + GPOINTER_TO_INT(user_data);
}

static int metrics_get_number_of_monitors(int current, void *user_data)
{
	struct btd_adv_monitor_manager *manager = user_data;

	return btd_adv_monitor_get_monitor_count(manager);
}

static struct metrics_periodic_timer *metrics_advmon_start_tracking_internal(
					enum metrics_periodic_timer_type type,
					struct btd_adv_monitor_manager *manager)
{
	struct metrics_periodic_timer *exist_timer;
	metric_periodic_timer_update_func_t on_update = NULL;
	metric_periodic_timer_report_func_t on_report = NULL;

	exist_timer = metrics_get_periodic_timer(type, manager);
	if (exist_timer)
		return NULL;

	switch (type) {
	case PERIODIC_TIMER_SW_PATTERN_ADV_PER_MINUTE:
	case PERIODIC_TIMER_MSFT_PATTERN_ADV_PER_MINUTE:
		on_update = metrics_update_adv_count;
		break;
	case PERIODIC_TIMER_NUM_MONITOR:
		on_report = metrics_get_number_of_monitors;
		break;
	default:
		DBG("Invalid enum type: %d", type);
		return NULL;
	}

	return metrics_start_periodic_timer(type, ADV_MON_ACTIVE_DURATION,
					ADV_MON_IDLE_DURATION, 0, manager,
					on_update, on_report);
}

bool metrics_advmon_start_tracking(struct btd_adv_monitor_manager *manager)
{
	struct metrics_periodic_timer *adv_count_timer;
	struct metrics_periodic_timer *mon_count_timer;
	enum metrics_periodic_timer_type type;

	if (!manager)
		return false;

	type = metrics_get_advmon_type(manager);
	adv_count_timer = metrics_advmon_start_tracking_internal(type, manager);
	if (!adv_count_timer)
		return false;

	mon_count_timer = metrics_advmon_start_tracking_internal(
					PERIODIC_TIMER_NUM_MONITOR, manager);
	if (!mon_count_timer) {
		metrics_stop_periodic_timer(adv_count_timer);
		return false;
	}

	return true;
}

bool metrics_advmon_stop_tracking(struct btd_adv_monitor_manager *manager)
{
	struct metrics_periodic_timer *adv_count_timer;
	struct metrics_periodic_timer *mon_count_timer;
	enum metrics_periodic_timer_type type;

	if (!manager)
		return false;

	type = metrics_get_advmon_type(manager);
	adv_count_timer = metrics_get_periodic_timer(type, manager);
	if (!metrics_stop_periodic_timer(adv_count_timer))
		return false;

	mon_count_timer = metrics_get_periodic_timer(
					PERIODIC_TIMER_NUM_MONITOR, manager);
	if (!metrics_stop_periodic_timer(mon_count_timer))
		return false;

	return true;
}

bool metrics_advmon_update_frequency(struct btd_adv_monitor_manager *manager,
								int value)
{
	struct metrics_periodic_timer *timer;
	enum metrics_periodic_timer_type type;

	if (!manager)
		return false;

	type = metrics_get_advmon_type(manager);
	timer = metrics_get_periodic_timer(type, manager);
	if (!metrics_update_periodic_timer_value(timer, GINT_TO_POINTER(value)))
		return false;

	return true;
}

bool metrics_send_advmon_enum(struct btd_adv_monitor_manager *manager,
				enum metrics_advmon_enum_type type, int sample)
{
	bool is_msft_supported = btd_adv_monitor_get_offload_support(manager);
	metrics_send_enum_type send_type;

	if (!manager)
		return false;

	switch (type) {
	case ADD_ADVMON_RESULT:
		if (is_msft_supported)
			send_type = ENUM_TYPE_ADVMON_MSFT_ADD_RESULT;
		else
			send_type = ENUM_TYPE_ADVMON_SW_ADD_RESULT;
		break;
	case REMOVE_ADVMON_RESULT:
		if (is_msft_supported)
			send_type = ENUM_TYPE_ADVMON_MSFT_REMOVE_RESULT;
		else
			send_type = ENUM_TYPE_ADVMON_SW_REMOVE_RESULT;
		break;
	default:
		DBG("Invalid enum type: %d", type);
		return false;
	}

	return metrics_send_enum(send_type, sample, true);
}

static inline long get_time_since_boot_micros(void)
{
	struct timespec current_time;

	clock_gettime(CLOCK_BOOTTIME, &current_time);
	return current_time.tv_sec * 1000000 + current_time.tv_nsec / 1000;
}

static metrics_conn_type convert_to_device_type(int addr_type)
{
	switch (addr_type) {
	case BDADDR_BREDR:
		return CONN_TYPE_BREDR;
	case BDADDR_LE_PUBLIC: // fall through
	case BDADDR_LE_RANDOM:
		return CONN_TYPE_LE;
	default:
		return CONN_TYPE_UNKNOWN;
	}
}

static char *get_boot_id()
{
	FILE *fp;
	size_t len = 0;
	ssize_t bytes = 0;
	int i, j;

	if (boot_id) {
		if (boot_id[0] != '\0')
			return boot_id;

		free(boot_id);
		boot_id = NULL;
	}

	fp = fopen(BOOT_ID_PROC_PATH, "r");
	if (!fp)
		goto fail;

	// example of boot_id: 80668f2e-da13-4a16-9efb-d91974a023af
	bytes = getline(&boot_id, &len, fp);
	if (bytes <= 0) {
		fclose(fp);
		goto fail;
	}

	// strip off new line and dash to construct an alphanumeric boot ID
	for (i = 0, j = 0; i < len; i++) {
		if (boot_id[i] != '\n' && boot_id[i] != '-')
			boot_id[j++] = boot_id[i];
	}
	boot_id[j] = '\0';

	fclose(fp);
	return boot_id;

fail:
	boot_id = realloc(boot_id, 1);
	boot_id[0] = '\0';

	return boot_id;
}

static int metrics_chipset_info_get_id(char *path, char *file)
{
	FILE *fp;
	size_t len = 0;
	ssize_t bytes = 0;
	char *line = NULL;
	int id = 0;
	char id_path[100] = { 0 };

	snprintf(id_path, 100, "%s/%s", path, file);
	fp = fopen(id_path, "r");

	if (!fp)
		return 0;

	bytes = getline(&line, &len, fp);
	if (bytes > 0)
		id = (int)strtol(line, NULL, 0);

	free(line);
	fclose(fp);
	return id;
}

static char *metrics_chipset_info_get_module_name(void)
{
	FILE *fp;
	size_t len = 0;
	ssize_t bytes = 0;
	char *modalias = NULL;

	fp = fopen(CHIPSET_INFO_MODALIAS_PATH, "r");
	if (!fp)
		return modalias;

	bytes = getline(&modalias, &len, fp);
	fclose(fp);

	if (bytes > 0) {
		// remove newline from getline()
		modalias[strcspn(modalias, "\n")] = '\0';
	}

	return modalias;
}

static enum metrics_transport_type get_chipset_transport(void)
{
	char *module_realpath;
	char *transport_string;
	enum metrics_transport_type transport = TRANSPORT_TYPE_UNKNOWN;

	// examples of moudle_realpath: /sys/module/btusb and
	// /sys/module/hci_uart
	module_realpath = realpath(CHIPSET_INFO_MODULE_DIR_PATH, NULL);

	if (!module_realpath)
		return transport;

	transport_string = strrchr(module_realpath, '/');

	if (transport_string) {
		if (strstr(module_realpath, "usb"))
			transport = TRANSPORT_TYPE_USB;
		else if (strstr(module_realpath, "uart"))
			transport = TRANSPORT_TYPE_UART;
		else if (strstr(module_realpath, "sdio"))
			transport = TRANSPORT_TYPE_SDIO;
	}

	free(module_realpath);
	return transport;
}

static void metrics_chipset_info_report(void)
{
	uint64_t chipset_string_hval = 0;

	if (chipset_info)
		goto report;

	chipset_info = calloc(1, sizeof(struct metrics_chipset_info));

	chipset_info->vid = metrics_chipset_info_get_id(
		CHIPSET_INFO_WLAN_DIR_PATH, "vendor");
	chipset_info->pid = metrics_chipset_info_get_id(
		CHIPSET_INFO_WLAN_DIR_PATH, "device");

	if (!chipset_info->vid || !chipset_info->pid) {
		chipset_info->vid = metrics_chipset_info_get_id(
			CHIPSET_INFO_MLAN_DIR_PATH, "vendor");
		chipset_info->pid = metrics_chipset_info_get_id(
			CHIPSET_INFO_MLAN_DIR_PATH, "device");
	}

	if (!chipset_info->vid || !chipset_info->pid) {
		chipset_info->chipset_string =
			metrics_chipset_info_get_module_name();
	}

	chipset_info->transport = get_chipset_transport();

	DBG("Chipset info report: %x %x %d %s", chipset_info->vid,
	    chipset_info->pid, chipset_info->transport,
	    chipset_info->chipset_string);
	BluetoothChipsetInfo(chipset_info->vid, chipset_info->pid,
			     chipset_info->transport,
			     chipset_info->chipset_string ?
				     chipset_info->chipset_string :
				     "");

report:
	if (is_chipset_info_in_allowlist(chipset_info->vid, chipset_info->pid,
					 chipset_info->transport,
					 chipset_info->chipset_string,
					 &chipset_string_hval)) {
		BluetoothChipsetInfoReport(get_boot_id(),
					   chipset_info->vid, chipset_info->pid,
					   chipset_info->transport,
					   chipset_string_hval);
	}
}

void metrics_adapter_state_changed(bool enabled)
{
	DBG("Adapter state changed: %d", enabled);
	BluetoothAdapterStateChanged(
		get_boot_id(), get_time_since_boot_micros(), false, enabled);
	metrics_chipset_info_report();
}

void metrics_pairing_state_changed(const char *device_id, int addr_type,
		metrics_pair_result state, metrics_result_type result_type)
{
	if (result_type == RESULT_TYPE_MGMT)
		state = convert_mgmt_pair_result(state);
	else if (result_type == RESULT_TYPE_SYSTEM)
		state = convert_system_pair_result(state);
	DBG("Pairing state changed: %s %d %d", device_id, addr_type, state);
	BluetoothPairingStateChanged(get_boot_id(),
				     get_time_since_boot_micros(), device_id,
				     convert_to_device_type(addr_type), state);
}

enum metrics_conn_state metrics_conn_system_err_to_state(int err)
{
	switch (-err) {
	case 0:
		return CONN_STATE_SUCCEED;
	case EALREADY:
		return CONN_STATE_ALREADY;
	case EHOSTDOWN:
		return CONN_STATE_TIMEOUT;
	case EHOSTUNREACH: /* adapter not powered */
	case ECONNABORTED: /* adapter powered down */
		return CONN_STATE_NONPOWERED;
	case EIO:
		return CONN_STATE_BT_IO_CONNECT_ERROR;
	case ENOTCONN:
		return CONN_STATE_NOT_CONNECTED;
	case EPERM:
		return CONN_STATE_NOT_PERMITTED;
	case EINVAL:
		return CONN_STATE_INVALID_PARAMS;
	case ECONNREFUSED:
		return CONN_STATE_CONNECTION_REFUSED;
	case ECANCELED:
		return CONN_STATE_CANCELED;
	default:
		return CONN_STATE_UNKNOWN;
	}
}

enum metrics_conn_state metrics_conn_mgmt_err_to_state(int err)
{
	switch (err) {
	case MGMT_STATUS_SUCCESS:
		return CONN_STATE_SUCCEED;
	case MGMT_STATUS_NOT_POWERED:
		return CONN_STATE_NONPOWERED;
	case MGMT_STATUS_ALREADY_CONNECTED:
		return CONN_STATE_ALREADY;
	case MGMT_STATUS_INVALID_PARAMS:
		return CONN_STATE_INVALID_PARAMS;
	case MGMT_STATUS_BUSY:
		return CONN_STATE_BUSY;
	case MGMT_STATUS_NOT_SUPPORTED:
		return CONN_STATE_NOT_SUPPORTED;
	case MGMT_STATUS_NO_RESOURCES:
		return CONN_STATE_NO_RESOURCES;
	case MGMT_STATUS_REJECTED:
		return CONN_STATE_CONNECTION_REFUSED;
	case MGMT_STATUS_DISCONNECTED:
		return CONN_STATE_DISCONNECTED;
	case MGMT_STATUS_CANCELLED:
		return CONN_STATE_CANCELED;
	case MGMT_STATUS_CONNECT_FAILED:
		return CONN_STATE_CONNECT_FAILED;
	case MGMT_STATUS_TIMEOUT:
		return CONN_STATE_TIMEOUT;
	case MGMT_STATUS_AUTH_FAILED:
		return CONN_STATE_AUTH_FAILED;
	case MGMT_STATUS_UNKNOWN_COMMAND:
		return CONN_STATE_UNKNOWN_COMMAND;
	case MGMT_STATUS_NOT_CONNECTED:
		return CONN_STATE_NOT_CONNECTED;
	case MGMT_STATUS_FAILED:
		return CONN_STATE_FAILED;
	default:
		return CONN_STATE_UNKNOWN;
	}
}

enum metrics_disconn_state metrics_convert_disconn_state(int state)
{
	switch (state) {
	case MGMT_DEV_DISCONN_TIMEOUT:
		return DISCONN_STATE_TIMEOUT;
	case MGMT_DEV_DISCONN_LOCAL_HOST:
		return DISCONN_STATE_LOCAL_HOST;
	case MGMT_DEV_DISCONN_REMOTE:
		return DISCONN_STATE_REMOTE;
	case MGMT_DEV_DISCONN_LOCAL_HOST_SUSPEND:
		return DISCONN_STATE_LOCAL_HOST_SUSPEND;
	default:
		return DISCONN_STATE_UNKNOWN;
	}
}

void metrics_acl_connection_state_changed(const char *device_id,
		int addr_type, enum metrics_acl_connection_direction direction,
		enum metrics_acl_connection_initiator initiator,
		enum metrics_conn_state state)
{
	DBG("ACL connection state changed: %s %d %d %d %d", device_id,
			addr_type, direction, initiator, state);
	BluetoothAclConnectionStateChanged(
		get_boot_id(), get_time_since_boot_micros(), false, device_id,
		convert_to_device_type(addr_type), direction, initiator,
		STATE_CHANGE_TYPE_CONNECT, state);

	if (state == CONN_STATE_STARTING)
		metrics_chipset_info_report();
}

enum metrics_acl_connection_direction metrics_reason_to_direction(int reason)
{
	switch (reason) {
	case MGMT_DEV_DISCONN_LOCAL_HOST: /* fall through */
	case MGMT_DEV_DISCONN_LOCAL_HOST_SUSPEND:
		return ACL_CONNECTION_OUTGOING;
	case MGMT_DEV_DISCONN_REMOTE:
		return ACL_CONNECTION_INCOMING;
	default:
		return ACL_CONNECTION_DIRECTION_UNKNOWN;
	}
}

void metrics_acl_disconnection_state_changed(const char *device_id,
		int addr_type, enum metrics_acl_connection_direction direction,
		enum metrics_acl_connection_initiator initiator,
		enum metrics_disconn_state state)
{
	DBG("ACL disconnection state changed: %s %d %d %d %d", device_id,
			addr_type, direction, initiator, state);
	BluetoothAclConnectionStateChanged(
		get_boot_id(), get_time_since_boot_micros(), false, device_id,
		convert_to_device_type(addr_type), direction, initiator,
		STATE_CHANGE_TYPE_DISCONNECT, state);

	if (state == DISCONN_STATE_STARTING)
		metrics_chipset_info_report();
}

static enum metrics_bluetooth_profile uuid_to_profile(const char *uuid)
{
	if (bt_uuid_strcmp(uuid, HID_UUID) == 0) {
		return BLUETOOTH_PROFILE_HID;
	} else if (bt_uuid_strcmp(uuid, HOG_UUID) == 0) {
		return BLUETOOTH_PROFILE_HOG;
	} else if (bt_uuid_strcmp(uuid, A2DP_SINK_UUID) == 0) {
		return BLUETOOTH_PROFILE_A2DP;
	} else if (bt_uuid_strcmp(uuid, HFP_AG_UUID) == 0 ||
			bt_uuid_strcmp(uuid, HFP_HS_UUID) == 0) {
		return BLUETOOTH_PROFILE_HFP;
	} else if (bt_uuid_strcmp(uuid, AVRCP_REMOTE_UUID) == 0 ||
			bt_uuid_strcmp(uuid, AVRCP_TARGET_UUID) == 0) {
		return BLUETOOTH_PROFILE_AVRCP;
	} else if (bt_uuid_strcmp(uuid, GAP_UUID) == 0) {
		return BLUETOOTH_PROFILE_GAP;
	} else if (bt_uuid_strcmp(uuid, DEVICE_INFORMATION_UUID) == 0) {
		return BLUETOOTH_PROFILE_DEVICE_INFO;
	} else if (bt_uuid_strcmp(uuid, BATTERY_UUID) == 0) {
		return BLUETOOTH_PROFILE_BATTERY;
	} else if (bt_uuid_strcmp(uuid, NEARBY_UUID) == 0) {
		return BLUETOOTH_PROFILE_NEARBY;
	} else if (bt_uuid_strcmp(uuid, PHONEHUB_UUID) == 0) {
		return BLUETOOTH_PROFILE_PHONEHUB;
	}
	return BLUETOOTH_PROFILE_UNKNOWN;
}

enum metrics_profile_conn_state metrics_convert_profile_conn_state(int err)
{
	switch (-err) {
	case 0:
		return PROFILE_CONN_STATE_SUCCEED;
	case EALREADY:
		return PROFILE_CONN_STATE_ALREADY_CONNECTED;
	case EBUSY:
		return PROFILE_CONN_STATE_BUSY_CONNECTING;
	case ECONNREFUSED:
	case EAGAIN:
		return PROFILE_CONN_STATE_CONNECTION_REFUSED;
	case ECANCELED:
		return PROFILE_CONN_STATE_CONNECTION_CANCELED;
	case EHOSTDOWN:
	case EHOSTUNREACH:
		return PROFILE_CONN_STATE_REMOTE_UNAVAILABLE;
	case EPROTONOSUPPORT:
	case ENOPROTOOPT:
	case ENOENT:
	case ENOTSUP:
		return PROFILE_CONN_STATE_PROFILE_NOT_SUPPORTED;
	default:
		return PROFILE_CONN_STATE_UNKNOWN_ERROR;
	}
}

void metrics_profile_connection_state_changed(const char *device_id,
				const char *uuid,
				enum metrics_profile_conn_state state)
{
	enum metrics_bluetooth_profile profile = uuid_to_profile(uuid);

	if (state == PROFILE_CONN_STATE_SUCCEED) {
		if (profile == BLUETOOTH_PROFILE_HFP)
			metrics_audio_connect_hfp();
		else if (profile == BLUETOOTH_PROFILE_A2DP)
			metrics_audio_connect_a2dp();
	}

	DBG("Profile connection state changed: %s %s %d %d", device_id, uuid,
			profile, state);
	BluetoothProfileConnectionStateChanged(
		get_boot_id(), get_time_since_boot_micros(), device_id,
		STATE_CHANGE_TYPE_CONNECT, profile, state);
}

enum metrics_profile_disconn_state metrics_convert_profile_disconn_state(
		int err)
{
	switch (-err) {
	case 0:
		return PROFILE_DISCONN_STATE_SUCCEED;
	case EALREADY:
	case ENOTCONN:
		return PROFILE_DISCONN_STATE_ALREADY_DISCONNECTED;
	case EBUSY:
		return PROFILE_DISCONN_STATE_BUSY_DISCONNECTING;
	case ECONNREFUSED:
		return PROFILE_DISCONN_STATE_DISCONNECTION_REFUSED;
	case ECANCELED:
		return PROFILE_DISCONN_STATE_DISCONNECTION_CANCELED;
	case EIO:
		return PROFILE_DISCONN_STATE_BT_IO_CONNECT_ERROR;
	case EINVAL:
		return PROFILE_DISCONN_STATE_INVALID_PARAMS;
	default:
		return PROFILE_DISCONN_STATE_UNKNOWN_ERROR;
	}
}

void metrics_profile_disconnection_state_changed(const char *device_id,
				const char *uuid,
				enum metrics_profile_disconn_state state)
{
	enum metrics_bluetooth_profile profile = uuid_to_profile(uuid);

	if (state == PROFILE_DISCONN_STATE_SUCCEED) {
		if (profile == BLUETOOTH_PROFILE_HFP)
			metrics_audio_disconnect_hfp(device_id);
		else if (profile == BLUETOOTH_PROFILE_A2DP)
			metrics_audio_disconnect_a2dp(device_id);
	}

	DBG("Profile disconnection state changed: %s %s %d %d", device_id, uuid,
			profile, state);
	BluetoothProfileConnectionStateChanged(
		get_boot_id(), get_time_since_boot_micros(), device_id,
		STATE_CHANGE_TYPE_DISCONNECT, profile, state);
}

void metrics_device_info_report(const char *device_id,
				metrics_discovery_type device_type,
				int class,
				int appearance,
				int vendor_id,
				int vendor_id_source,
				int product_id,
				int version)
{
	int major_class = (class & DEVICE_MAJOR_CLASS_MASK)
			>> DEVICE_MAJOR_CLASS_BIT_OFFSET;
	int category = (appearance & DEVICE_CATEGORY_MASK)
			>> DEVICE_CATEGORY_BIT_OFFSET;

	DBG("Device info report: %s %d %d %d %d %d %d %d", device_id,
	    device_type, major_class, category, vendor_id, vendor_id_source,
	    product_id, version);
	if (is_device_info_in_allowlist(vendor_id_source, vendor_id,
					product_id)) {
		BluetoothDeviceInfoReport(get_boot_id(),
					  get_time_since_boot_micros(),
					  device_id, device_type, major_class,
					  category, vendor_id, vendor_id_source,
					  product_id, version);
	} else {
		BluetoothDeviceInfoReport(get_boot_id(),
					  get_time_since_boot_micros(),
					  device_id, device_type, major_class,
					  category, 0, 0, 0, 0);
	}

	BluetoothDeviceInfo(device_type, major_class, category, vendor_id,
			    vendor_id_source, product_id, version);
}

/* higher values get lower priority (first to be kicked out) */
static bool metrics_audio_worst_case_compare_keep_min(const void *a,
						      const void *b)
{
	return *(double *)a > *(double *)b;
}

/* smaller values get lower priority (first to be kicked out) */
static bool metrics_audio_worst_case_compare_keep_max(const void *a,
						      const void *b)
{
	return *(double *)a < *(double *)b;
}

enum metric_audio_quality_type {// Support for (BQR, Intel A2DP, Intel HFP)
	AUDIO_QUALITY_TYPE_UNKNOWN = 0,			// N N N
	AUDIO_QUALITY_TYPE_RSSI = 1,			// Y N N
	AUDIO_QUALITY_TYPE_RETRANSMISSION_COUNT = 2,	// Y Y N
	AUDIO_QUALITY_TYPE_NO_RX_COUNT = 3,		// Y N N
	AUDIO_QUALITY_TYPE_NAK_COUNT = 4,		// Y N Y
	AUDIO_QUALITY_TYPE_CHOPPY_COUNT = 5,		// Y N N
	AUDIO_QUALITY_TYPE_RX_LOST_COUNT = 6,		// N N Y
	AUDIO_QUALITY_TYPE_TX_LOST_COUNT = 7,		// N N Y
	AUDIO_QUALITY_TYPE_NO_SYNC_COUNT = 8,		// N N Y
	AUDIO_QUALITY_TYPE_HEC_ERR_COUNT = 9,		// N Y Y
	AUDIO_QUALITY_TYPE_CRC_ERR_COUNT = 10,		// N Y Y
	AUDIO_QUALITY_TYPE_WIFI_COEX_RX_COUNT = 11,	// N N Y
	AUDIO_QUALITY_TYPE_WIFI_COEX_TX_COUNT = 12,	// N N Y
	AUDIO_QUALITY_TYPE_PLC_INJECTION_COUNT = 13,	// N N Y
	AUDIO_QUALITY_TYPE_AVG_LATENCY = 14,		// N Y N
};

/* We want audio quality data for every 5 seconds, but it might come at
 * irregular intervals depending on the implementation. Therefore we need to
 * accumulate them and bucket them into 5-second chunks before putting them into
 * calculations because it's unfair to compare 1 sec vs. 5 secs of data.
 */
struct metrics_audio_summary {
	double acc_time;	/* to accumulate 5 seconds of data */
	double acc_value;	/* to accumulate 5 seconds of data */
	int count;
	double sum;
	double squared_sum;
	struct bpqueue *worst_cases;
};

struct metrics_audio_summary_auxiliary {
	long last_event_in_micros;
	bool is_skip_first;	/* 1st data tends to be wrong, skip it. */
	bool is_play;		/* is the audio streaming? */
};

struct metrics_audio_bqr {
	struct metrics_audio_summary_auxiliary aux;
	struct metrics_audio_summary *rssi;
	struct metrics_audio_summary *retransmission_count;
	struct metrics_audio_summary *no_rx_count;
	struct metrics_audio_summary *nak_count;
	struct metrics_audio_summary *choppy_count;
};

struct metrics_audio_intel_a2dp {
	struct metrics_audio_summary_auxiliary aux;
	struct metrics_audio_summary *retransmission_count;
	struct metrics_audio_summary *hec_err_count;
	struct metrics_audio_summary *crc_err_count;
	struct metrics_audio_summary *avg_latency;
};

struct metrics_audio_intel_hfp {
	struct metrics_audio_summary_auxiliary aux;
	struct metrics_audio_summary *nak_count;
	struct metrics_audio_summary *rx_lost_count;
	struct metrics_audio_summary *tx_lost_count;
	struct metrics_audio_summary *no_sync_count;
	struct metrics_audio_summary *hec_err_count;
	struct metrics_audio_summary *crc_err_count;
	struct metrics_audio_summary *wifi_coex_rx_count;
	struct metrics_audio_summary *wifi_coex_tx_count;
	struct metrics_audio_summary *plc_injection_count;
};

struct metrics_audio {
	struct metrics_audio_bqr *bqr_a2dp;
	struct metrics_audio_bqr *bqr_hfp;
	struct metrics_audio_intel_a2dp *intel_a2dp;
	struct metrics_audio_intel_hfp *intel_hfp;

	enum metrics_audio_quality_support support;
	bool is_hfp;		/* To distinguish hfp/a2dp state of BQR */
	int sco_handle;		/* Workaround for QCA controllers */
};

static struct metrics_audio metrics_audio; /* Only one audio device possible */

static struct metrics_audio_summary *metrics_audio_summary_new(
					bpqueue_priority_func priority_func)
{
	struct metrics_audio_summary *summary =
					g_new0(struct metrics_audio_summary, 1);

	summary->worst_cases = bpqueue_new(PERCENTILE95_BUFFER_SIZE,
					   priority_func, g_free);
	return summary;
}

static void metrics_audio_summary_free(struct metrics_audio_summary *summary)
{
	if (!summary)
		return;

	bpqueue_free(summary->worst_cases);
	g_free(summary);
}

static void metrics_audio_summary_add(struct metrics_audio_summary *summary,
				      double delta_value, double delta_time)
{
	double *copy;
	double additional_time;
	double additional_value, value;

	while (summary->acc_time + delta_time >= AUDIO_QUALITY_TIME_CHUNK) {
		additional_time = AUDIO_QUALITY_TIME_CHUNK - summary->acc_time;
		additional_value = delta_value * additional_time / delta_time;
		value = summary->acc_value + additional_value;

		summary->sum += value;
		summary->squared_sum += value * value;

		copy = g_new0(double, 1);
		*copy = value;
		bpqueue_add(summary->worst_cases, copy);

		summary->acc_value = 0;
		summary->acc_time = 0;
		summary->count += 1;
		delta_value -= additional_value;
		delta_time -= additional_time;
	}

	summary->acc_time += delta_time;
	summary->acc_value += delta_value;
}

/* Percentile95 is calculated by storing the 5% of the worst data inside a
 * priority queue. Therefore, if the population gets too large, the real
 * percentile95 might lie outside of the queue, and we have to offer the
 * closest alternative we have.
 * Here we are using linear interpolation for the fractional part, similar to
 * numpy.percentile().
 * This pops some data from the bpqueue. Watch out!
 */
static double metrics_audio_calculate_percentile95(struct bpqueue *q, int count)
{
	int idx = (count - 1) * 19 / 20;
	int offset = (count - 1) * 19 % 20;
	int already_popped = count - bpqueue_count(q);
	double value;
	double next_value;

	idx -= already_popped;

	/* 95th percentile is outside the heap, choose the best we have */
	if (idx >= bpqueue_capacity(q) - 1) {
		idx = bpqueue_capacity(q) - 1;
		offset = 0;
	}

	/* Safeguard. This should not happen! */
	if (idx >= bpqueue_count(q)) {
		warn("desired index larger than count");
		idx = bpqueue_count(q) - 1;
		offset = 0;
	}

	for (; idx > 0; idx -= 1)
		bpqueue_pop(q);

	value = *(double *) bpqueue_peek(q);

	if (!offset)
		return value;

	/* Safeguard. This should not happen! */
	if (bpqueue_count(q) == 0) {
		warn("desired index larger than count");
		return value;
	}

	bpqueue_pop(q);
	next_value = *(double *) bpqueue_peek(q);

	return (value * (20 - offset) + next_value * offset) / 20;
}

static void metrics_audio_summarize_and_send(const char *device_id,
					enum metrics_bluetooth_profile profile,
					enum metric_audio_quality_type type,
					struct metrics_audio_summary *summary)
{
	const int multiplier = 100;
	double avg, stddev, variance, percentile95;
	int64_t roundAvg, roundStddev, roundPercentile95;

	if (summary->count == 0)
		return;

	avg = summary->sum / summary->count;
	variance = summary->squared_sum / summary->count - avg * avg;
	/* Beware of negative variance caused by imprecision */
	stddev = variance > 0 ? sqrt(variance) : 0;
	percentile95 = metrics_audio_calculate_percentile95(
					summary->worst_cases, summary->count);

	/* Structured metric doesn't accept float, need to cast to int.
	 * Here we multiply by 100 to maintain some precision.
	 */
	roundAvg = round(avg * multiplier);
	roundStddev = round(stddev * multiplier);
	roundPercentile95 = round(percentile95 * multiplier);

	DBG("Audio quality report: %s %d %d %lld %lld %lld",
	    device_id, profile, type, roundAvg, roundStddev, roundPercentile95);
	BluetoothAudioQualityReport(get_boot_id(), get_time_since_boot_micros(),
				    device_id, profile, type, roundAvg,
				    roundStddev, roundPercentile95);
}

static void metrics_audio_process_and_send_bqr(const char *device_id,
					struct metrics_audio_bqr *bqr,
					enum metrics_bluetooth_profile profile)
{
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_RSSI,
				bqr->rssi);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_RETRANSMISSION_COUNT,
				bqr->retransmission_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_NO_RX_COUNT,
				bqr->no_rx_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_NAK_COUNT,
				bqr->nak_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_CHOPPY_COUNT,
				bqr->choppy_count);
}

static void metrics_audio_process_and_send_intel_a2dp(const char *device_id,
					struct metrics_audio_intel_a2dp *intel)
{
	const enum metrics_bluetooth_profile profile = BLUETOOTH_PROFILE_A2DP;

	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_RETRANSMISSION_COUNT,
				intel->retransmission_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_HEC_ERR_COUNT,
				intel->hec_err_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_CRC_ERR_COUNT,
				intel->crc_err_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_AVG_LATENCY,
				intel->avg_latency);
}

static void metrics_audio_process_and_send_intel_hfp(const char *device_id,
					struct metrics_audio_intel_hfp *intel)
{
	const enum metrics_bluetooth_profile profile = BLUETOOTH_PROFILE_HFP;

	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_NAK_COUNT,
				intel->nak_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_RX_LOST_COUNT,
				intel->rx_lost_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_TX_LOST_COUNT,
				intel->tx_lost_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_NO_SYNC_COUNT,
				intel->no_sync_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_HEC_ERR_COUNT,
				intel->hec_err_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_CRC_ERR_COUNT,
				intel->crc_err_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_WIFI_COEX_RX_COUNT,
				intel->wifi_coex_rx_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_WIFI_COEX_TX_COUNT,
				intel->wifi_coex_tx_count);
	metrics_audio_summarize_and_send(device_id, profile,
				AUDIO_QUALITY_TYPE_PLC_INJECTION_COUNT,
				intel->plc_injection_count);
}

static struct metrics_audio_bqr *metrics_audio_bqr_new()
{
	struct metrics_audio_bqr *bqr = g_new0(struct metrics_audio_bqr, 1);

	bqr->rssi = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_min);
	bqr->retransmission_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	bqr->no_rx_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	bqr->nak_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	bqr->choppy_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);

	return bqr;
}

static struct metrics_audio_intel_a2dp *metrics_audio_intel_a2dp_new()
{
	struct metrics_audio_intel_a2dp *intel =
				g_new0(struct metrics_audio_intel_a2dp, 1);

	intel->retransmission_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->hec_err_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->crc_err_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->avg_latency = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);

	return intel;
}

static struct metrics_audio_intel_hfp *metrics_audio_intel_hfp_new()
{
	struct metrics_audio_intel_hfp *intel =
				g_new0(struct metrics_audio_intel_hfp, 1);

	intel->nak_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->rx_lost_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->tx_lost_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->no_sync_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->hec_err_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->crc_err_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->wifi_coex_rx_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->wifi_coex_tx_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);
	intel->plc_injection_count = metrics_audio_summary_new(
			metrics_audio_worst_case_compare_keep_max);

	return intel;
}

static void metrics_audio_bqr_free(struct metrics_audio_bqr *bqr)
{
	if (!bqr)
		return;

	metrics_audio_summary_free(bqr->rssi);
	metrics_audio_summary_free(bqr->retransmission_count);
	metrics_audio_summary_free(bqr->no_rx_count);
	metrics_audio_summary_free(bqr->nak_count);
	metrics_audio_summary_free(bqr->choppy_count);

	g_free(bqr);
}

static void metrics_audio_intel_a2dp_free(
					struct metrics_audio_intel_a2dp *intel)
{
	if (!intel)
		return;

	metrics_audio_summary_free(intel->retransmission_count);
	metrics_audio_summary_free(intel->hec_err_count);
	metrics_audio_summary_free(intel->crc_err_count);
	metrics_audio_summary_free(intel->avg_latency);

	g_free(intel);
}

static void metrics_audio_intel_hfp_free(struct metrics_audio_intel_hfp *intel)
{
	if (!intel)
		return;

	metrics_audio_summary_free(intel->nak_count);
	metrics_audio_summary_free(intel->rx_lost_count);
	metrics_audio_summary_free(intel->tx_lost_count);
	metrics_audio_summary_free(intel->no_sync_count);
	metrics_audio_summary_free(intel->hec_err_count);
	metrics_audio_summary_free(intel->crc_err_count);
	metrics_audio_summary_free(intel->wifi_coex_rx_count);
	metrics_audio_summary_free(intel->wifi_coex_tx_count);
	metrics_audio_summary_free(intel->plc_injection_count);

	g_free(intel);
}

void metrics_audio_setup(enum metrics_audio_quality_support support)
{
	metrics_audio.support = support;
}

void metrics_audio_clean(void)
{
	if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_BQR) {
		metrics_audio_bqr_free(metrics_audio.bqr_a2dp);
		metrics_audio_bqr_free(metrics_audio.bqr_hfp);
		metrics_audio.bqr_a2dp = NULL;
		metrics_audio.bqr_hfp = NULL;
	} else if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_INTEL) {
		metrics_audio_intel_a2dp_free(metrics_audio.intel_a2dp);
		metrics_audio_intel_hfp_free(metrics_audio.intel_hfp);
		metrics_audio.intel_a2dp = NULL;
		metrics_audio.intel_hfp = NULL;
	}
}

static void metrics_audio_connect_a2dp(void)
{
	if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_BQR) {
		if (metrics_audio.bqr_a2dp) {
			metrics_audio_bqr_free(metrics_audio.bqr_a2dp);
			warn("unreported BQR A2DP");
		}
		metrics_audio.bqr_a2dp = metrics_audio_bqr_new();
	} else if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_INTEL) {
		if (metrics_audio.intel_a2dp) {
			metrics_audio_intel_a2dp_free(metrics_audio.intel_a2dp);
			warn("unreported Intel A2DP");
		}
		metrics_audio.intel_a2dp = metrics_audio_intel_a2dp_new();
	}
}

static void metrics_audio_connect_hfp(void)
{
	if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_BQR) {
		if (metrics_audio.bqr_hfp) {
			metrics_audio_bqr_free(metrics_audio.bqr_hfp);
			warn("unreported BQR HFP");
		}
		metrics_audio.bqr_hfp = metrics_audio_bqr_new();
	} else if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_INTEL) {
		if (metrics_audio.intel_hfp) {
			metrics_audio_intel_hfp_free(metrics_audio.intel_hfp);
			warn("unreported Intel HFP");
		}
		metrics_audio.intel_hfp = metrics_audio_intel_hfp_new();
	}
}

static void metrics_audio_disconnect_a2dp(const char *device_id)
{
	if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_BQR) {
		if (!metrics_audio.bqr_a2dp) {
			warn("BQR A2DP disconnection without connection");
			return;
		}

		metrics_audio_process_and_send_bqr(device_id,
						   metrics_audio.bqr_a2dp,
						   BLUETOOTH_PROFILE_A2DP);
		metrics_audio_bqr_free(metrics_audio.bqr_a2dp);
		metrics_audio.bqr_a2dp = NULL;
	} else if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_INTEL) {
		if (!metrics_audio.intel_a2dp) {
			warn("Intel A2DP disconnection without connection");
			return;
		}

		metrics_audio_process_and_send_intel_a2dp(
					device_id, metrics_audio.intel_a2dp);
		metrics_audio_intel_a2dp_free(metrics_audio.intel_a2dp);
		metrics_audio.intel_a2dp = NULL;
	}
}

static void metrics_audio_disconnect_hfp(const char *device_id)
{
	if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_BQR) {
		if (!metrics_audio.bqr_hfp) {
			warn("BQR HFP disconnection without connection");
			return;
		}

		metrics_audio_process_and_send_bqr(device_id,
						   metrics_audio.bqr_hfp,
						   BLUETOOTH_PROFILE_HFP);
		metrics_audio_bqr_free(metrics_audio.bqr_hfp);
		metrics_audio.bqr_hfp = NULL;
	} else if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_INTEL) {
		if (!metrics_audio.intel_hfp) {
			warn("Intel HFP disconnection without connection");
			return;
		}

		metrics_audio_process_and_send_intel_hfp(
					device_id, metrics_audio.intel_hfp);
		metrics_audio_intel_hfp_free(metrics_audio.intel_hfp);
		metrics_audio.intel_hfp = NULL;
	}
}

static void metrics_report_bqr_process(void *structure, void *report,
				       double delta_time)
{
	struct metrics_audio_bqr *bqr = (struct metrics_audio_bqr *) structure;
	struct aosp_bqr *data = (struct aosp_bqr *) report;

	/* Workaround for QCA: They send events for both ACL and SCO handles.
	 * However, the SCO BQR events consist of almost all zeros.
	 * Ignore events from SCO handle to keep data clean and treat them the
	 * same way as with other vendors, which only have ACL handle.
	 */
	if (data->conn_handle == metrics_audio.sco_handle)
		return;

	/* RSSI should be proportional to time. Count can be directly used. */
	metrics_audio_summary_add(bqr->rssi,
		data->rssi * delta_time / AUDIO_QUALITY_TIME_CHUNK, delta_time);
	metrics_audio_summary_add(bqr->retransmission_count,
				  data->retransmission_count, delta_time);
	metrics_audio_summary_add(bqr->no_rx_count,
				  data->no_rx_count, delta_time);
	metrics_audio_summary_add(bqr->nak_count,
				  data->nak_count, delta_time);

	if (data->quality_report_id == BQR_QUALITY_ID_A2DP_CHOPPY ||
	    data->quality_report_id == BQR_QUALITY_ID_SCO_CHOPPY) {
		/* Magnify 1000 times to maintain some precision. */
		metrics_audio_summary_add(bqr->choppy_count, 1000, delta_time);
	}
}

static int sum(unsigned int *array, unsigned int N)
{
	unsigned int i, result = 0;

	for (i = 0; i < N; i += 1)
		result += array[i];

	return result;
}

static void metrics_report_intel_a2dp_process(void *structure, void *report,
					      double delta_time)
{
	struct metrics_audio_intel_a2dp *intel =
				(struct metrics_audio_intel_a2dp *) structure;
	struct intel_acl_event *data = (struct intel_acl_event *) report;

	/* The 0th retry is technically not retransmitted, skip it. */
	metrics_audio_summary_add(intel->retransmission_count,
			sum(data->tx_packets_retry + 1, INTEL_NUM_RETRIES - 1),
			delta_time);
	metrics_audio_summary_add(intel->hec_err_count,
				  data->rx_hec_error, delta_time);
	metrics_audio_summary_add(intel->crc_err_count,
				  data->rx_crc_error, delta_time);
	metrics_audio_summary_add(intel->avg_latency,
				  data->avg_packet_letency, delta_time);
}

static void metrics_report_intel_hfp_process(void *structure, void *report,
					     double delta_time)
{
	struct metrics_audio_intel_hfp *intel =
				(struct metrics_audio_intel_hfp *) structure;
	struct intel_sco_event *data = (struct intel_sco_event *) report;

	metrics_audio_summary_add(intel->nak_count,
				sum(data->rx_nak_error, INTEL_NUM_SLOTS),
				delta_time);
	metrics_audio_summary_add(intel->rx_lost_count,
				data->rx_payload_lost, delta_time);
	metrics_audio_summary_add(intel->tx_lost_count,
				data->tx_payload_lost, delta_time);
	metrics_audio_summary_add(intel->no_sync_count,
				sum(data->rx_no_sync_error, INTEL_NUM_SLOTS),
				delta_time);
	metrics_audio_summary_add(intel->hec_err_count,
				sum(data->rx_hec_error, INTEL_NUM_SLOTS),
				delta_time);
	metrics_audio_summary_add(intel->crc_err_count,
				sum(data->rx_crc_error, INTEL_NUM_SLOTS),
				delta_time);
	metrics_audio_summary_add(intel->wifi_coex_rx_count,
				sum(data->rx_failed_wifi_coex, INTEL_NUM_SLOTS),
				delta_time);
	metrics_audio_summary_add(intel->wifi_coex_tx_count,
				sum(data->tx_failed_wifi_coex, INTEL_NUM_SLOTS),
				delta_time);
	metrics_audio_summary_add(intel->plc_injection_count,
				data->plc_injection, delta_time);
}

static void *metrics_get_audio_metrics_structure(
				enum metrics_audio_quality_support support,
				bool is_hfp)
{
	if (support == AUDIO_QUALITY_SUPPORT_BQR) {
		if (metrics_audio.support != support) {
			warn("audio metric is not BQR");
			return NULL;
		}

		if (is_hfp)
			return metrics_audio.bqr_hfp;
		else
			return metrics_audio.bqr_a2dp;
	} else if (support == AUDIO_QUALITY_SUPPORT_INTEL) {
		if (metrics_audio.support != support) {
			warn("audio metric is not Intel");
			return NULL;
		}

		if (is_hfp)
			return metrics_audio.intel_hfp;
		else
			return metrics_audio.intel_a2dp;
	}

	return NULL;
}

typedef void (*metrics_audio_process_func)(void *metrics_structure,
					   void *report, double delta_time);

static void metrics_audio_process_report(
				struct metrics_audio_summary_auxiliary *aux,
				metrics_audio_process_func process_func,
				void *metrics_structure, void *report)
{
	long time_micros = get_time_since_boot_micros();
	double delta_time;

	/* No audio transmission. Skip. */
	if (!aux->is_play)
		return;

	delta_time = (time_micros - aux->last_event_in_micros) * 0.000001;
	aux->last_event_in_micros = time_micros;

	/* Some controller reports questionable data on the first report, maybe
	 * due to unproper initialization. Also, right after switching A2DP to
	 * HFP, the 1st HFP report might still contain A2DP data. Furthermore,
	 * we can't directly calculate delta time for the first report.
	 * Here we always just skip the first report to simplify things.
	 */
	if (aux->is_skip_first) {
		aux->is_skip_first = false;
		return;
	}

	/* If delta time is too small, probably it's better to skip in order to
	 * prevent the normalized number become way too high. This is possible
	 * because AOSP "audio choppy" events can be interpreted to be sent as
	 * often as possible. Conversely, if delta time is too large probably
	 * something is wrong, let's skip this event.
	 */
	if (delta_time < AUDIO_QUALITY_REPORT_MIN_DELTA_TIME ||
	    delta_time > AUDIO_QUALITY_REPORT_MAX_DELTA_TIME) {
		return;
	}

	process_func(metrics_structure, report, delta_time);
}

void metrics_report_bqr(struct aosp_bqr *data)
{
	struct metrics_audio_bqr *bqr =
		(struct metrics_audio_bqr *)
		metrics_get_audio_metrics_structure(AUDIO_QUALITY_SUPPORT_BQR,
						    metrics_audio.is_hfp);
	if (!bqr)
		return;

	metrics_audio_process_report(&bqr->aux, metrics_report_bqr_process,
				     bqr, data);
}

void metrics_report_intel_a2dp(struct intel_acl_event *data)
{
	struct metrics_audio_intel_a2dp *intel =
		(struct metrics_audio_intel_a2dp *)
		metrics_get_audio_metrics_structure(AUDIO_QUALITY_SUPPORT_INTEL,
						    false /* is_hfp */);
	if (!intel)
		return;

	metrics_audio_process_report(&intel->aux,
				     metrics_report_intel_a2dp_process,
				     intel, data);
}

void metrics_report_intel_hfp(struct intel_sco_event *data)
{
	struct metrics_audio_intel_hfp *intel =
		(struct metrics_audio_intel_hfp *)
		metrics_get_audio_metrics_structure(AUDIO_QUALITY_SUPPORT_INTEL,
						    true /* is_hfp */);
	if (!intel)
		return;

	metrics_audio_process_report(&intel->aux,
				     metrics_report_intel_hfp_process,
				     intel, data);
}

void metrics_audio_a2dp_play_pause(bool is_play)
{
	if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_BQR) {
		if (!metrics_audio.bqr_a2dp) {
			warn("BQR A2DP metric is NULL");
			return;
		}

		metrics_audio.bqr_a2dp->aux.is_play = is_play;
		if (is_play)
			metrics_audio.bqr_a2dp->aux.is_skip_first = true;
	} else if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_INTEL) {
		if (!metrics_audio.intel_a2dp) {
			warn("Intel A2DP metric is NULL");
			return;
		}

		metrics_audio.intel_a2dp->aux.is_play = is_play;
		if (is_play)
			metrics_audio.intel_a2dp->aux.is_skip_first = true;
	}
}

void metrics_audio_hfp_play_pause(bool is_play, int sco_handle)
{
	if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_BQR) {
		/* For BQR case, HFP and A2DP packets are indistinguishable.
		 * Therefore store whether the incoming packets are A2DP or HFP.
		 * Assume next packet is HFP if HFP is playing, otherwise A2DP.
		 */
		metrics_audio.is_hfp = is_play;
		metrics_audio.sco_handle = -1;

		if (!metrics_audio.bqr_hfp) {
			warn("BQR HFP metric is NULL");
			return;
		}

		metrics_audio.bqr_hfp->aux.is_play = is_play;
		if (is_play) {
			metrics_audio.bqr_hfp->aux.is_skip_first = true;
			metrics_audio.sco_handle = sco_handle;
		}
	} else if (metrics_audio.support == AUDIO_QUALITY_SUPPORT_INTEL) {
		if (!metrics_audio.intel_hfp) {
			warn("Intel HFP metric is NULL");
			return;
		}

		/* Intel doesn't need the sco handle. */
		metrics_audio.intel_hfp->aux.is_play = is_play;
		if (is_play)
			metrics_audio.intel_hfp->aux.is_skip_first = true;
	}
}
