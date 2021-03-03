/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef BLUEZ_METRICS_H_
#define BLUEZ_METRICS_H_

#include <stdbool.h>
#include <stdlib.h>

/* Names of histograms */
#define H_NAME_DISCOVERABLE_LEN	"BlueZ.TimeLengthOfDiscoverable"
#define H_NAME_DISCOVERY_LEN	"BlueZ.TimeLengthOfDiscovering"
#define H_NAME_PAIRING_LEN	"BlueZ.TimeLengthOfPairing"
#define H_NAME_ADV_LEN		"BlueZ.TimeLengthOfAdvertisement"
#define H_NAME_CONN_LEN		"BlueZ.TimeLengthOfSetupConnection"
#define H_NAME_DISCOVERY_TYPE	"BlueZ.TypeOfDiscovery"
#define H_NAME_FOUND_DEVICE_TYPE	"BlueZ.TypeOfFoundDevice"
#define H_NAME_ADV_REG_RESULT	"BlueZ.ResultOfAdvertisementRegistration"
#define H_NAME_DISCONN_REASON	"BlueZ.ReasonOfDisconnection"
#define H_NAME_PAIR_RESULT	"BlueZ.ResultOfPairing"
#define H_NAME_CONN_RESULT	"BlueZ.ResultOfConnection"
#define H_NAME_ADAPTER_LOST	"BlueZ.AdapterLost"
#define H_NAME_CHIP_LOST	"BlueZ.ChipLost"
#define H_NAME_CHIP_LOST2	"BlueZ.ChipLost2"
#define H_NAME_NUM_EXISTING_ADV	"BlueZ.NumberOfExistingAdvertisements"

#define H_NAME_HID_PROBE_RESULT "BlueZ.PerProfile.HID.ProbingResult"
#define H_NAME_HID_CONN_RESULT "BlueZ.PerProfile.HID.ConnectionResult"
#define H_NAME_HOG_PROBE_RESULT "BlueZ.PerProfile.HOG.ProbingResult"
#define H_NAME_HOG_CONN_RESULT "BlueZ.PerProfile.HOG.ConnectionResult"
#define H_NAME_A2DP_SINK_PROBE_RESULT "BlueZ.PerProfile.A2DPSink.ProbingResult"
#define H_NAME_A2DP_SINK_CONN_RESULT                                           \
	"BlueZ.PerProfile.A2DPSink.ConnectionResult"
#define H_NAME_HFP_PROBE_RESULT "BlueZ.PerProfile.HFP.ProbingResult"
#define H_NAME_HFP_CONN_RESULT "BlueZ.PerProfile.HFP.ConnectionResult"
#define H_NAME_AVRCP_PROBE_RESULT "BlueZ.PerProfile.AVRCP.ProbingResult"
#define H_NAME_AVRCP_CONN_RESULT "BlueZ.PerProfile.AVRCP.ConnectionResult"
#define H_NAME_BATTERY_PROBE_RESULT "BlueZ.PerProfile.Battery.ProbingResult"
#define H_NAME_BATTERY_CONN_RESULT "BlueZ.PerProfile.Battery.ConnectionResult"

#define H_NAME_ADVMON_NUM_MONITOR "BlueZ.AdvertisementMonitor.NumOfMonitors"
#define H_NAME_ADVMON_SW_PATTERN_ADV_PER_MINUTE                                \
	"BlueZ.AdvertisementMonitor.SW.FilterPatternAdvsPerMinute"
#define H_NAME_ADVMON_SW_ADD_RESULT                                            \
	"BlueZ.AdvertisementMonitor.SW.Add.Result"
#define H_NAME_ADVMON_SW_REMOVE_RESULT                                         \
	"BlueZ.AdvertisementMonitor.SW.Remove.Result"
#define H_NAME_ADVMON_MSFT_PATTERN_ADV_PER_MINUTE                              \
	"BlueZ.AdvertisementMonitor.MSFT.FilterPatternAdvsPerMinute"
#define H_NAME_ADVMON_MSFT_ADD_RESULT                                          \
	"BlueZ.AdvertisementMonitor.MSFT.Add.Result"
#define H_NAME_ADVMON_MSFT_REMOVE_RESULT                                       \
	"BlueZ.AdvertisementMonitor.MSFT.Remove.Result"

/* The lower and upper bounds of number of registered advertisements. */
#define NUM_ADV_MAX 6
#define NUM_ADV_MIN 0

/* This is used to prevent sending repeated samples of continuous adapter
 * losts. 10 seconds */
#define TIME_LENGTH_LAST_LOST	10.00

struct btd_adapter;
struct btd_device;
struct btd_adv_monitor_manager;

/* BlueZ metrics does not take ownership of these pointers, so there is no need
 * to free the memory when bringing down.
 */
struct metrics_timer_data {
	struct btd_adapter *adapter;
	struct btd_device *device;
	struct btd_adv_client *adv_client;
};

typedef enum {
	TIMER_DISCOVERABLE = 1,
	TIMER_DISCOVERY,
	TIMER_PAIRING,
	TIMER_ADVERTISEMENT,
	TIMER_CONNECT,
	TIMER_ADAPTER_LOST,
	TIMER_CHIP_LOST,
	TIMER_CHIP_LOST2,
} metrics_timer_type;

struct metrics_periodic_timer;
typedef int (*metric_periodic_timer_update_func_t)(int current, void *data,
							void *user_data);
typedef int (*metric_periodic_timer_report_func_t)(int current, void *data);

enum metrics_periodic_timer_type {
	PERIODIC_TIMER_NUM_MONITOR = 1,
	PERIODIC_TIMER_SW_PATTERN_ADV_PER_MINUTE = 2,
	PERIODIC_TIMER_MSFT_PATTERN_ADV_PER_MINUTE = 3,
};

enum metrics_advmon_enum_type {
	ADD_ADVMON_RESULT = 1,
	REMOVE_ADVMON_RESULT,
};

typedef enum {
	ENUM_TYPE_DISCOVERY = 1,
	ENUM_TYPE_FOUND_DEVICE,
	ENUM_TYPE_ADV_REG_RESULT,
	ENUM_TYPE_DISCONN_REASON,
	ENUM_TYPE_PAIR_RESULT,
	ENUM_TYPE_CONN_RESULT,
	ENUM_TYPE_ADVMON_SW_ADD_RESULT,
	ENUM_TYPE_ADVMON_SW_REMOVE_RESULT,
	ENUM_TYPE_ADVMON_MSFT_ADD_RESULT,
	ENUM_TYPE_ADVMON_MSFT_REMOVE_RESULT,
} metrics_send_enum_type;

typedef enum {
	PROFILE_PROBE_RESULT = 1,
	PROFILE_CONN_RESULT,
} metrics_per_profile_type;

/* These enums must never be renumbered or deleted and reused unless the XML
 * file is also changed.
 */
typedef enum {
	DISCOVERY_TYPE_BREDR = 1,
	DISCOVERY_TYPE_LE = 2,
	DISCOVERY_TYPE_DUAL = 3,
	DISCOVERY_TYPE_END = 4,
} metrics_discovery_type;

typedef enum {
	DEVICE_TYPE_BREDR = 1,
	DEVICE_TYPE_LE_PUBLIC = 2,
	DEVICE_TYPE_LE_RANDOM = 3,
	DEVICE_TYPE_END = 4,
} metrics_device_type;

typedef enum {
	CONN_TYPE_BREDR = 1,
	CONN_TYPE_LE = 2,
	CONN_TYPE_END = 3,
} metrics_conn_type;

typedef enum {
	// The adv is registered successfully.
	ADV_SUCCEED = 1,
	// The controller is not LE capable.
	ADV_FAIL_LE_UNSUPPORTED = 2,
	// This can be the non-powered controller or LE-disabled controller.
	ADV_FAIL_LE_DISABLED = 3,
	// The adv data is too long.
	ADV_FAIL_ADV_DATA_TOO_LONG = 4,
	// There is another ongoing add/remove adv request.
	ADV_FAIL_BUSY = 5,
	// Failed to create an adv client.
	ADV_FAIL_CREATE_CLIENT = 6,
	// This can be max adv number met, unsupported adv flags, incorrect adv
	// data length or invalid adv type data.
	ADV_FAIL_INVALID_PARAMS = 7,
	// Failed to parse user-specified adv data.
	ADV_FAIL_PARSE_ADV_DATA = 8,
	// Failed to prepare or send MGMT_OP_ADD_ADVERTISING.
	ADV_FAIL_MGMT_SEND = 9,
	ADV_FAIL_UNKNOWN = 10,
	ADV_FAIL_END = 11,
} metrics_adv_reg_result;

typedef enum {
	// The local host terminated the connection.
	DISCONN_LOCAL_HOST = 1,
	// This can be connection terminated by the remote user, power status
	// of remote device turned off or low resources of the remote device.
	DISCONN_REMOTE = 2,
	// Supervision timeout of maintaining the connection.
	DISCONN_SUPERVISION_TIMEOUT = 3,
	DISCONN_UNKNOWN = 4,
	DISCONN_END = 5,
} metrics_disconn_reason;

typedef enum {
	PAIR_SUCCEED = 1,
	// The controller is not powered.
	PAIR_FAIL_NONPOWERED = 2,
	// The remote device has been paired with the local host.
	PAIR_FAIL_ALREAY_PAIRED = 3,
	// This can be invalid address type, invalid IO capability.
	PAIR_FAIL_INVALID_PARAMS = 4,
	// The pairing is in progress or being canceled.
	PAIR_FAIL_BUSY = 5,
	// Simple pairing or pairing is not supported on the remote device.
	PAIR_FAIL_NOT_SUPPORTED = 6,
	// Fail to set up connection with the remote device.
	PAIR_FAIL_ESTABLISH_CONN = 7,
	// The authentication failure can be caused by incorrect PIN/link key or
	// missing PIN/link key during pairing or authentication procedure.
	// This can also be a failure during message integrity check.
	PAIR_FAIL_AUTH_FAILED = 8,
	// The pairing request is rejected by the remote device.
	PAIR_FAIL_AUTH_REJECTED = 9,
	// The authentication was cancelled.
	PAIR_FAIL_AUTH_CANCELLED = 10,
	// The authentication was timeout.
	PAIR_FAIL_AUTH_TIMEOUT = 11,
	PAIR_FAIL_UNKNOWN = 12,
	// BT IO connection error
	PAIR_FAIL_BT_IO_CONNECT = 13,
	PAIR_FAIL_END = 14,
} metrics_pair_result;

typedef enum {
	// Connect to remote LE device successfully.
	CONN_LE_SUCCEED = 1,
	// Connect to remote BREDR device on profile(s) successfully.
	CONN_BREDR_SUCCEED = 2,
	// There is an existing LE connection with the remote device.
	CONN_ALREADY_LE = 3,
	// BREDR profile(s) has been connected.
	CONN_ALREADY_BREDR = 4,
	// The is a connection/disconnection happening.
	CONN_FAIL_BUSY = 5,
	// The controller is not powered.
	CONN_FAIL_NONPOWERED = 6,
	// Failed to establish a LE connection.
	CONN_FAIL_LE = 7,
	// Failed to connect any BREDR profile.
	CONN_FAIL_BREDR = 8,
	// Failed to establish a BREDR connection due to Page timeout.
	CONN_FAIL_BREDR_PAGE_TIMEOUT = 9,
	// Failed to find connectable profiles on the remote device.
	CONN_FAIL_BREDR_PROFILE_UNAVAILABLE = 10,
	// Failed to perform Service Discovery on the remote device.
	CONN_FAIL_BROWSE_SDP = 11,
	// Failed to explore GATT services on the remote device.
	CONN_FAIL_BROWSE_GATT = 12,
	CONN_FAIL_UNKNOWN = 13,
	CONN_FAIL_END = 14,
} metrics_conn_result;

typedef enum {
	PROFILE_PROBE_SUCCEED = 0,
	PROFILE_PROBE_UNKNOWN_ERROR = 1,
	PROFILE_PROBE_UNABLE_TO_REGISTER_INTERFACE = 2,
	PROFILE_PROBE_UNABLE_TO_CREATE_NEW_DEVICE = 3,
	PROFILE_PROBE_PROFILE_NOT_SUPPORTED = 4,
	PROFILE_PROBE_END = 5,
} metrics_profile_probe_result;

typedef enum {
	PROFILE_CONN_SUCCEED = 0,
	PROFILE_CONN_UNKNOWN_ERROR = 1,
	PROFILE_CONN_ALREADY_CONNECTED = 2,
	PROFILE_CONN_BUSY_CONNECTING = 3,
	PROFILE_CONN_CONNECTION_REFUSED = 4,
	PROFILE_CONN_CONNECT_CANCELED = 5,
	PROFILE_CONN_REMOTE_UNAVAILABLE = 6,
	PROFILE_CONN_PROFILE_NOT_SUPPORTED = 7,
	PROFILE_CONN_END = 8,
} metrics_profile_conn_result;

enum metrics_advmon_result {
	ADVMON_RESULT_SUCCEED = 0,
	ADVMON_RESULT_UNKNOWN_ERROR = 1,
	ADVMON_RESULT_BAD_PARAM = 2,
	ADVMON_RESULT_NO_RESOURCE = 3,
	ADVMON_RESULT_BUSY = 4,
	ADVMON_RESULT_END = 5,
};

typedef enum {
	RESULT_TYPE_DEFINED = 0, // Result that is clearly-defined by core logic.
	RESULT_TYPE_MGMT = 1,    // Result returned by MGMT interface.
	RESULT_TYPE_SYSTEM = 2,  // Result caused by system resource allocation.
} metrics_result_type;

/* Corresponding methods to C Metrics Library */
bool metrics_init(void);
void metrics_deinit(void);
int metrics_is_enabled(void);
bool metrics_send(const char* name, int sample, int min, int max, int buckets);
bool metrics_send_enum(metrics_send_enum_type type, int sample,
			metrics_result_type result_type);
bool metrics_send_per_profile_enum(metrics_per_profile_type type,
				   const char *uuid, int sample);

/* Methods to create a timer and emit a sample when removing the timer */
bool metrics_start_timer(metrics_timer_type type,
			struct metrics_timer_data data);
void metrics_cancel_timer(metrics_timer_type type,
			struct metrics_timer_data data);
bool metrics_stop_timer(metrics_timer_type type,
			struct metrics_timer_data data);

/* Methods to manage periodic_timer which emit sample every few seconds */
struct metrics_periodic_timer *metrics_start_periodic_timer(
				enum metrics_periodic_timer_type type,
				int active_period, int idle_period,
				int init_value, void *data,
				metric_periodic_timer_update_func_t on_update,
				metric_periodic_timer_report_func_t on_report);
bool metrics_stop_periodic_timer(struct metrics_periodic_timer *timer);
bool metrics_update_periodic_timer_value(struct metrics_periodic_timer *timer,
							void *user_data);
bool metrics_advmon_start_tracking(struct btd_adv_monitor_manager *manager);
bool metrics_advmon_stop_tracking(struct btd_adv_monitor_manager *manager);
bool metrics_advmon_update_frequency(struct btd_adv_monitor_manager *manager,
								int value);
bool metrics_send_advmon_enum(struct btd_adv_monitor_manager *manager,
				enum metrics_advmon_enum_type type, int sample);

void metrics_adapter_state_changed(bool enabled);
#endif  // BLUEZ_METRICS_H_
