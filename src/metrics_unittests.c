/*
 * Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <error.h>
#include <glib.h>
#include <stdlib.h>
#include <stdio.h>

#include "lib/bluetooth.h"
#include "lib/mgmt.h"
#include "src/shared/mgmt.h"
#include "lib/sdp.h"
#include "adapter.h"
#include "device.h"
#include "advertising.h"
#include "metrics.h"
#include "btd.h"

#define ASSERT_TRUE(x) do { \
	if(!(x)) { \
		printf("%s::%s() [%d]: expect: true, actual: false\n", \
		__FILE__, __FUNCTION__, __LINE__); \
		abort(); \
	} \
} while(0)

#define ASSERT_FALSE(x) do { \
	if(x) { \
		printf("%s::%s() [%d]: expect: false, actual: true\n", \
			__FILE__, __FUNCTION__, __LINE__); \
		abort(); \
	} \
} while(0)

/* Dummy variable and functions which are needed to compile without messing with
 * the rest of source files of bluetoothd.
 */
struct btd_opts btd_opts;
void btd_exit(void) {}
GKeyFile *btd_get_main_conf(void) {return NULL;}
bool btd_experimental_enabled(const char *uuid) {return true; }

/* Tests the timer creation with mismatched timer type and the timer data. */
static void test_timer_type_data_mismatch()
{
	printf("\n[test_timer_type_data_mismatch]\n");
	struct btd_adapter *adapter;
	struct btd_device *device;
	struct btd_adv_client *client;

	adapter = new_dummy_adapter();
	device = new_dummy_device();
	client = new_dummy_adv_client();
	ASSERT_TRUE(adapter && device && client);

	struct metrics_timer_data data_adv = {NULL , NULL, client};
	struct metrics_timer_data data_connect = {adapter, device, NULL};
	struct metrics_timer_data data_discovery = {NULL, device, NULL};

	ASSERT_FALSE(metrics_start_timer(TIMER_ADVERTISEMENT, data_connect));
	ASSERT_FALSE(metrics_start_timer(TIMER_CONNECT, data_discovery));
	ASSERT_FALSE(metrics_start_timer(TIMER_CONNECT, data_adv));

	g_free(adapter);
	g_free(device);
	g_free(client);
}

/* Tests the timer creation with unknown timer type.*/
static void test_timer_unknown_type()
{
	printf("\n[test_timer_unknown_type]\n");
	struct btd_adv_client *client;

	client = new_dummy_adv_client();
	ASSERT_TRUE(client);

	struct metrics_timer_data data_adv = {NULL, NULL, client};

	ASSERT_FALSE(metrics_start_timer(100, data_adv));

	g_free(client);
}

/* Tests the timer with start, cancel and stop operations.
 */
static void test_timer_start_stop_cancel()
{
	struct btd_adapter *adapter_1;
	struct btd_adapter *adapter_2;
	struct btd_device *device;
	struct btd_adv_client *client;
	adapter_1 = new_dummy_adapter();
	adapter_2 = new_dummy_adapter();
	device = new_dummy_device();
	client = new_dummy_adv_client();
	ASSERT_TRUE(adapter_1 && adapter_2 && device && client);

	struct metrics_timer_data data_adv = {NULL , NULL, client};
	struct metrics_timer_data data_connect = {adapter_1, device, NULL};
	struct metrics_timer_data data_discovery_1 = {adapter_1, NULL, NULL};
	struct metrics_timer_data data_discovery_2 = {adapter_2, NULL, NULL};

	// Start a timer and stop an existing/non-existing timer.
	ASSERT_TRUE(metrics_start_timer(TIMER_ADVERTISEMENT, data_adv));
	ASSERT_FALSE(metrics_stop_timer(TIMER_CONNECT, data_connect));
	ASSERT_TRUE(metrics_stop_timer(TIMER_ADVERTISEMENT, data_adv));

	// Start a timer, cancel a timer and stop a non-existing timer.
	ASSERT_TRUE(metrics_start_timer(TIMER_CONNECT, data_connect));
	metrics_cancel_timer(TIMER_CONNECT, data_connect);
	ASSERT_FALSE(metrics_stop_timer(TIMER_CONNECT, data_connect));

	// Start multiple timers and stop them.
	ASSERT_TRUE(metrics_start_timer(TIMER_DISCOVERY, data_discovery_1));
	ASSERT_TRUE(metrics_start_timer(TIMER_DISCOVERY, data_discovery_2));
	ASSERT_TRUE(metrics_stop_timer(TIMER_DISCOVERY, data_discovery_1));
	ASSERT_TRUE(metrics_stop_timer(TIMER_DISCOVERY, data_discovery_2));

	g_free(adapter_1);
	g_free(adapter_2);
	g_free(device);
	g_free(client);
}

/* Tests the timer creation and the termination of the timer with
 * matched/unmatched timer type and data.
 */
static void test_timer_mismatched_type_and_data()
{
	struct btd_adapter *adapter;
	struct btd_device *device;
	struct btd_adv_client *client;
	adapter = new_dummy_adapter();
	device = new_dummy_device();
	client = new_dummy_adv_client();
	ASSERT_TRUE(adapter && device && client);

	struct metrics_timer_data data_adv = {NULL , NULL, client};
	struct metrics_timer_data data_connect = {adapter, device, NULL};
	struct metrics_timer_data data_discovery = {adapter, NULL, NULL};

	ASSERT_FALSE(metrics_start_timer(TIMER_DISCOVERY, data_adv));
	ASSERT_TRUE(metrics_start_timer(TIMER_DISCOVERY, data_discovery));
	metrics_cancel_timer(TIMER_DISCOVERY, data_connect);
	ASSERT_TRUE(metrics_stop_timer(TIMER_DISCOVERY, data_discovery));

	g_free(adapter);
	g_free(device);
	g_free(client);
}

/* Tests creating a redundant timer. */
static void test_timer_redundant_start()
{
	struct btd_adv_client *client;
	client = new_dummy_adv_client();
	ASSERT_TRUE(client);

	struct metrics_timer_data data_adv = {NULL , NULL, client};

	ASSERT_TRUE(metrics_start_timer(TIMER_ADVERTISEMENT, data_adv));
	ASSERT_TRUE(metrics_start_timer(TIMER_ADVERTISEMENT, data_adv));

	g_free(client);
}

/* Tests sending enum sample with an invalid enum type, an invalid value of
 * sample and valid samples. */
static void test_send_enum()
{
	ASSERT_FALSE(metrics_send_enum(0, 1, false));
	ASSERT_FALSE(metrics_send_enum(ENUM_TYPE_DISCONN_REASON, DISCONN_END,
					false));
	ASSERT_TRUE(metrics_send_enum(ENUM_TYPE_DISCONN_REASON,
					MGMT_DEV_DISCONN_LOCAL_HOST, true));
	ASSERT_TRUE(metrics_send_enum(ENUM_TYPE_DISCONN_REASON, DISCONN_REMOTE,
					false));
}

/* Tests sending samples with valid/invalid histogram name, MIN value, MAX value
 * or number of buckets. */
static void test_send()
{
	// invalid histogram name
	ASSERT_FALSE(metrics_send(NULL, 0, NUM_ADV_MIN, NUM_ADV_MAX,
					NUM_ADV_MAX + 1));
	// invalid sample value
	ASSERT_FALSE(metrics_send(H_NAME_NUM_EXISTING_ADV, -1, NUM_ADV_MIN,
					NUM_ADV_MAX, NUM_ADV_MAX + 1));
	// invalid MIN value
	ASSERT_FALSE(metrics_send(H_NAME_NUM_EXISTING_ADV, 0, NUM_ADV_MAX,
					NUM_ADV_MAX, NUM_ADV_MAX + 1));
	// invalid MAX value
	ASSERT_FALSE(metrics_send(H_NAME_NUM_EXISTING_ADV, 0, NUM_ADV_MIN,
					NUM_ADV_MIN, NUM_ADV_MAX + 1));
	// invalid number of buckets
	ASSERT_FALSE(metrics_send(H_NAME_NUM_EXISTING_ADV, 0, NUM_ADV_MIN,
					NUM_ADV_MAX, 0));
	// all valid
	ASSERT_TRUE(metrics_send(H_NAME_NUM_EXISTING_ADV, 0, NUM_ADV_MIN,
				NUM_ADV_MAX, NUM_ADV_MAX + 1));
}

int main(int argc, char *argv[])
{
	metrics_init();

	// Run tests.
	test_timer_type_data_mismatch();
	test_timer_unknown_type();
	test_timer_start_stop_cancel();
	test_timer_mismatched_type_and_data();
	test_timer_redundant_start();
	test_send_enum();
	test_send();

	metrics_deinit();
	return 0;
}
