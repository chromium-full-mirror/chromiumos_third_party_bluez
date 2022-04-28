/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef BLUEZ_METRICS_H_
#define BLUEZ_METRICS_H_

#include <stdbool.h>
#include <stdlib.h>

#include "src/shared/aosp.h"
#include "src/shared/intel.h"

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
	CONN_TYPE_UNKNOWN = 0,
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
	PAIR_STARTING = 0,
	PAIR_SUCCEED = 1,
	// The controller is not powered.
	PAIR_FAIL_NONPOWERED = 2,
	// The remote device has been paired with the local host.
	PAIR_FAIL_ALREADY_PAIRED = 3,
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
	PAIR_FAIL_REJECTED = 9,
	// The pairing was cancelled.
	PAIR_FAIL_CANCELLED = 10,
	// The connection was timeout.
	PAIR_FAIL_TIMEOUT = 11,
	PAIR_FAIL_UNKNOWN = 12,
	// BT IO connection error
	PAIR_FAIL_BT_IO_CONNECT_ERROR = 13,
	// Unknown command.
	PAIR_FAIL_UNKNOWN_COMMAND = 14,
	// The peer was not connected.
	PAIR_FAIL_NOT_CONNECTED = 15,
	// Exceeded the limit of resource such as memory, connections.
	PAIR_FAIL_NO_RESOURCES = 16,
	// Disconnected due to power, user termination or other reasons.
	PAIR_FAIL_DISCONNECTED = 17,
	// Failed due to all the other reasons such as hardware, invalid LMP
	// PDU, transaction collision, role change, slot violation etc.
	PAIR_FAIL_FAILED = 18,
	PAIR_FAIL_END = 19,
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
	// Unknown failure reasons for LE.
	CONN_FAIL_LE = 7,
	// Unknown failure reasons for BREDR.
	CONN_FAIL_BREDR = 8,
	// Failed to establish a BREDR connection due to Page timeout.
	CONN_FAIL_BREDR_PAGE_TIMEOUT = 9,
	// Failed to find connectable profiles/service on the remote device.
	CONN_FAIL_BREDR_PROFILE_UNAVAILABLE = 10,
	// Failed to perform Service Discovery on the remote device.
	CONN_FAIL_BROWSE_SDP = 11,
	// Failed to explore GATT services on the remote device.
	CONN_FAIL_BROWSE_GATT = 12,
	CONN_FAIL_UNKNOWN = 13,
	CONN_FAIL_BT_IO_CONNECT_ERROR = 14,
	CONN_FAIL_NOT_CONNECT = 15,
	CONN_FAIL_NOT_PERMITTED = 16,
	CONN_FAIL_INVALID_PARAMS = 17,
	CONN_FAIL_CONNECTION_REFUSED = 18,
	CONN_FAIL_CANCELED = 19,
	// Failed due to other ongoing operations, such as pairing, busy L2CAP
	// channel or the operation disallowed by the controller.
	CONN_FAIL_BUSY_BREDR = 20,
	// The connection ended up being canceled.
	CONN_FAIL_CANCELED_BREDR = 21,
	// Failed to create or connect to BT IO socket.
	CONN_FAIL_IO_CONNECT_BREDR = 22,
	// Failed due to invalid argument provided either by client or by
	// daemon.
	CONN_FAIL_INVALID_PARAMS_BREDR = 23,
	// Failed due to unsupported state transition of L2CAP channel or other
	// features either by the local host or the remote.
	CONN_FAIL_NOT_SUPPORTED_BREDR = 24,
	// Failed due to the socket is in bad state.
	CONN_FAIL_BAD_SOCKET_BREDR = 25,
	// Failed to allocate memory in either host stack or controller.
	CONN_FAIL_MEMORY_ALLOC_BREDR = 26,
	// Failed due to reaching the synchronous connection limit to a device.
	CONN_FAIL_SYNC_CONNECT_LIMIT_BREDR = 27,
	// Failed due to connection timeout.
	CONN_FAIL_TIMEDOUT_BREDR = 28,
	// Refused by the remote device due to limited resource, security reason
	// or unacceptable address type.
	CONN_FAIL_REFUSED_BREDR = 29,
	// Terminated by the remote device due to limited resource or power
	// off.
	CONN_FAIL_TERM_BY_REMOTE_BREDR = 30,
	// Terminated by the local host due to limited resource, or aborted at
	// L2CAP layer.
	CONN_FAIL_TERM_BY_LOCAL_BREDR = 31,
	// Failed due to LMP protocol error.
	CONN_FAIL_PROTO_ERROR_BREDR = 32,
	// The is a LE connection/disconnection happening at daemon, L2CAP
	// layer.
	CONN_FAIL_BUSY_LE = 33,
	// Failed to create or connect to BT IO socket
	CONN_FAIL_IO_CONNECT_LE = 34,
	// Failed at daemon, L2CAP or below layers due to invalid parameters.
	CONN_FAIL_INVALID_PARAMS_LE = 35,
	// Failed at L2CAP layer or below due to missing HCI device.
	CONN_FAIL_NO_DEV_LE = 36,
	// Failed due to unsupported state transition of L2CAP channel or other
	// features (e.g. LE features) either by the local host or the remote.
	CONN_FAIL_NOT_SUPPORTED_LE = 37,
	// Failed to allocate memory in either host stack or controller.
	CONN_FAIL_MEMORY_ALLOC_LE = 38,
	// Failed due to that LE is not enabled or the attempt is refused by the
	// remote device due to limited resource, security reason or
	// unacceptable address type.
	CONN_FAIL_REFUSED_LE = 39,
	// Failed due to the socket is in bad state.
	CONN_FAIL_BAD_SOCKET_LE = 40,
	// Failed due to connection timeout.
	CONN_FAIL_TIMEDOUT_LE = 41,
	// Failed due to reaching the synchronous connection limit to a device.
	CONN_FAIL_SYNC_CONNECT_LIMIT_LE = 42,
	// Terminated by the remote device due to limited resource or power
	// off.
	CONN_FAIL_TERM_BY_REMOTE_LE = 43,
	// Terminated by the local host due to limited resource, or aborted at
	// L2CAP layer.
	CONN_FAIL_TERM_BY_LOCAL_LE = 44,
	// Failed due to LL protocol error.
	CONN_FAIL_PROTO_ERROR_LE = 45,
	CONN_FAIL_END = 46,
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
	RESULT_TYPE_DEFINED = 0, // Result that is clearly-defined by core
				 // logic.
	RESULT_TYPE_MGMT = 1,    // Result returned by MGMT interface.
	RESULT_TYPE_SYSTEM = 2,  // Result caused by system resource allocation.
} metrics_result_type;

enum metrics_conn_state {
	CONN_STATE_STARTING = 0,
	CONN_STATE_SUCCEED = 1,
	CONN_STATE_ALREADY = 2,
	CONN_STATE_BUSY = 3,
	CONN_STATE_NONPOWERED = 4,
	CONN_STATE_TIMEOUT = 5,
	CONN_STATE_PROFILE_UNAVAILABLE = 6,
	CONN_STATE_NOT_CONNECTED = 7,
	CONN_STATE_NOT_PERMITTED = 8,
	CONN_STATE_INVALID_PARAMS = 9,
	CONN_STATE_CONNECTION_REFUSED = 10,
	CONN_STATE_CANCELED = 11,
	CONN_STATE_EVENT_INVALID = 12,
	CONN_STATE_DEVICE_NOT_FOUND = 13,
	CONN_STATE_BT_IO_CONNECT_ERROR = 14,
	CONN_STATE_UNKNOWN_COMMAND = 15,
	CONN_STATE_DISCONNECTED = 16,
	CONN_STATE_CONNECT_FAILED = 17,
	CONN_STATE_NOT_SUPPORTED = 18,
	CONN_STATE_NO_RESOURCES = 19,
	CONN_STATE_AUTH_FAILED = 20,
	CONN_STATE_FAILED = 21,
	CONN_STATE_UNKNOWN = 22,
};

enum metrics_disconn_state {
	DISCONN_STATE_STARTING = 0,
	DISCONN_STATE_TIMEOUT = 1,
	DISCONN_STATE_LOCAL_HOST = 2,
	DISCONN_STATE_REMOTE = 3,
	DISCONN_STATE_AUTH_FAILURE = 4,
	DISCONN_STATE_LOCAL_HOST_SUSPEND = 5,
	DISCONN_STATE_UNKNOWN = 6,
};

enum metrics_acl_connection_direction {
	ACL_CONNECTION_DIRECTION_UNKNOWN = 0,
	ACL_CONNECTION_OUTGOING = 1,
	ACL_CONNECTION_INCOMING = 2,
};

enum metrics_acl_connection_initiator {
	ACL_CONNECTION_INITIATOR_UNKNOWN = 0,
	ACL_CONNECTION_INITIATOR_CLIENT = 1,
	ACL_CONNECTION_INITIATOR_SYSTEM = 2,
};

enum metrics_bluetooth_profile {
	BLUETOOTH_PROFILE_UNKNOWN = 0,
	BLUETOOTH_PROFILE_HSP = 1,
	BLUETOOTH_PROFILE_HFP = 2,
	BLUETOOTH_PROFILE_A2DP = 3,
	BLUETOOTH_PROFILE_AVRCP = 4,
	BLUETOOTH_PROFILE_HID = 5,
	BLUETOOTH_PROFILE_HOG = 6,
	BLUETOOTH_PROFILE_GATT = 7,
	BLUETOOTH_PROFILE_GAP = 8,
	BLUETOOTH_PROFILE_DEVICE_INFO = 9,
	BLUETOOTH_PROFILE_BATTERY = 10,
	BLUETOOTH_PROFILE_NEARBY = 11,
	BLUETOOTH_PROFILE_PHONEHUB = 12,
};

enum metrics_profile_conn_state {
	PROFILE_CONN_STATE_STARTING = 0,
	PROFILE_CONN_STATE_SUCCEED = 1,
	PROFILE_CONN_STATE_ALREADY_CONNECTED = 2,
	PROFILE_CONN_STATE_BUSY_CONNECTING = 3,
	PROFILE_CONN_STATE_CONNECTION_REFUSED = 4,
	PROFILE_CONN_STATE_CONNECTION_CANCELED = 5,
	PROFILE_CONN_STATE_REMOTE_UNAVAILABLE = 6,
	PROFILE_CONN_STATE_PROFILE_NOT_SUPPORTED = 7,
	PROFILE_CONN_STATE_UNKNOWN_ERROR = 8,
};

enum metrics_profile_disconn_state {
	PROFILE_DISCONN_STATE_STARTING = 0,
	PROFILE_DISCONN_STATE_SUCCEED = 1,
	PROFILE_DISCONN_STATE_ALREADY_DISCONNECTED = 2,
	PROFILE_DISCONN_STATE_BUSY_DISCONNECTING = 3,
	PROFILE_DISCONN_STATE_DISCONNECTION_REFUSED = 4,
	PROFILE_DISCONN_STATE_DISCONNECTION_CANCELED = 5,
	PROFILE_DISCONN_STATE_BT_IO_CONNECT_ERROR = 6,
	PROFILE_DISCONN_STATE_INVALID_PARAMS = 7,
	PROFILE_DISCONN_STATE_UNKNOWN_ERROR = 8,
};

enum metrics_audio_quality_support {
	AUDIO_QUALITY_SUPPORT_NONE = 0,
	AUDIO_QUALITY_SUPPORT_BQR = 1,
	AUDIO_QUALITY_SUPPORT_INTEL = 2,
};

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

enum metrics_conn_state metrics_conn_system_err_to_state(int err);
enum metrics_conn_state metrics_conn_mgmt_err_to_state(int err);
enum metrics_disconn_state metrics_convert_disconn_state(int state);
enum metrics_acl_connection_direction metrics_reason_to_direction(int reason);
enum metrics_profile_conn_state metrics_convert_profile_conn_state(int err);
enum metrics_profile_disconn_state metrics_convert_profile_disconn_state(
		int err);
void metrics_adapter_state_changed(bool enabled);
void metrics_pairing_state_changed(const char *device_id, int addr_type,
				metrics_pair_result state,
				metrics_result_type result_type);
void metrics_acl_connection_state_changed(const char *device_id, int addr_type,
				enum metrics_acl_connection_direction direction,
				enum metrics_acl_connection_initiator initiator,
				enum metrics_conn_state state);
void metrics_acl_disconnection_state_changed(const char *device_id,
				int addr_type,
				enum metrics_acl_connection_direction direction,
				enum metrics_acl_connection_initiator initiator,
				enum metrics_disconn_state state);
void metrics_profile_connection_state_changed(const char *device_id,
				const char *uuid,
				enum metrics_profile_conn_state state);
void metrics_profile_disconnection_state_changed(const char *device_id,
				const char *uuid,
				enum metrics_profile_disconn_state state);

void metrics_device_info_report(const char *device_id,
				metrics_discovery_type device_type,
				int class,
				int appearance,
				int vendor_id,
				int vendor_id_source,
				int product_id,
				int version);

metrics_conn_result metrics_bredr_conn_err_to_result(int sample);
metrics_conn_result metrics_le_conn_err_to_result(int sample);

void metrics_audio_setup(enum metrics_audio_quality_support support);
void metrics_audio_clean(void);
void metrics_report_bqr(struct aosp_bqr *data);
void metrics_report_intel_a2dp(struct intel_acl_event *data);
void metrics_report_intel_hfp(struct intel_sco_event *data);
void metrics_audio_a2dp_play_pause(bool is_play);
void metrics_audio_hfp_play_pause(bool is_play, int sco_handle);

#endif  // BLUEZ_METRICS_H_
