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
#define H_NAME_CHIP_LOST	"BlueZ.ChipLost"
#define H_NAME_CHIP_LOST2	"BlueZ.ChipLost2"
#define H_NAME_NUM_EXISTING_ADV	"BlueZ.NumberOfExistingAdvertisements"

/* The lower and upper bounds of number of registered advertisements. */
#define NUM_ADV_MAX 6
#define NUM_ADV_MIN 0

/* This is used to prevent sending repeated samples of continuous adapter
 * losts. 10 seconds */
#define TIME_LENGTH_LAST_LOST	10.00

struct btd_adapter;
struct btd_device;

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
	TIMER_CHIP_LOST,
	TIMER_CHIP_LOST2,
} metrics_timer_type;

typedef enum {
	ENUM_TYPE_DISCOVERY = 1,
	ENUM_TYPE_FOUND_DEVICE,
	ENUM_TYPE_ADV_REG_RESULT,
	ENUM_TYPE_DISCONN_REASON,
	ENUM_TYPE_PAIR_RESULT,
	ENUM_TYPE_CONN_RESULT,
} metrics_send_enum_type;

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
	PAIR_FAIL_END = 13,
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

/* Corresponding methods to C Metrics Library */
bool metrics_init(void);
void metrics_deinit(void);
int metrics_is_enabled(void);
bool metrics_send(const char* name, int sample, int min, int max, int buckets);
bool metrics_send_enum(metrics_send_enum_type type, int sample,
			bool is_mgmt_status);

/* Methods to create a timer and emit a sample when removing the timer */
bool metrics_start_timer(metrics_timer_type type,
			struct metrics_timer_data data);
void metrics_cancel_timer(metrics_timer_type type,
			struct metrics_timer_data data);
bool metrics_stop_timer(metrics_timer_type type,
			struct metrics_timer_data data);

#endif  // BLUEZ_METRICS_H_
