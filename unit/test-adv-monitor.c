/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2020 Google LLC
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
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "src/log.h"
#include "src/shared/tester.h"

#include "src/adv_monitor.h"

#define define_test(name, type, data, setup_fn, test_fn, teardown_fn)	\
	do {								\
		static struct test_data test;				\
		test.test_type = type;					\
		test.test_name = g_strdup(name);			\
		if (type == TEST_RSSI_FILTER) {				\
			test.rssi_filter_test_data = (void *)&data;	\
			test.rssi_filter_test_data->test_info = &test;	\
		} else if (type == TEST_CONTENT_FILTER) {		\
			test.content_filter_test_data = (void *)&data;	\
		}							\
		tester_add(name, &test, setup_fn, test_fn, teardown_fn);\
	} while (0)

#define ADV_INTERVAL		1	/* Advertisement interval in seconds */
#define OUT_OF_RANGE		-128
#define END_OF_RSSI_TEST	{0}

#define RSSI_TEST_DONE(test_step)	\
	(!test_step.adv_rssi && !test_step.duration && !test_step.result)

#define DUMMY_BTD_DEVICE_OBJ	((void *) 0xF00)

enum test_type {
	TEST_RSSI_FILTER = 0,
	TEST_CONTENT_FILTER,
};

enum result {
	RESULT_DEVICE_NOT_FOUND = false,	/* Initial state of a device */
	RESULT_DEVICE_FOUND = true,		/* Device state when the
						 * Content/RSSI Filter match
						 */
	RESULT_DEVICE_LOST = false,		/* Device state when the Low
						 * RSSI Filter match or if it
						 * goes offline/out-of-range
						 */
};

struct rssi_filter_test {
	void *adv_monitor_obj;			/* struct adv_monitor object */
	void *btd_device_obj;			/* struct btd_device object */
	struct test_data *test_info;

	const struct {
		int8_t high_rssi_threshold;	/* High RSSI threshold */
		uint16_t high_rssi_timeout;	/* High RSSI threshold timeout*/
		int8_t low_rssi_threshold;	/* Low RSSI threshold */
		uint16_t low_rssi_timeout;	/* Low RSSI threshold timeout */
	} rssi_filter;

	time_t start_time;		/* Start time of the test */
	uint16_t resume_step;		/* Store the current sub-step of the
					 * test before suspending that test
					 */
	guint out_of_range_timer;	/* Timer to simulate device offline */

	const struct {
		int8_t adv_rssi;	/* Advertisement RSSI */
		uint16_t duration;	/* Advertisement duration in seconds */
		enum result result;	/* Device state after every step */
	} test_steps[];
};

struct content_filter_test {
	void *advmon_pattern;		/* btd_adv_monitor_pattern */

	bool expected_match;

	const struct {
		uint8_t ad_type;
		uint8_t offset;
		uint8_t length;
		uint8_t value[BT_AD_MAX_DATA_LEN];
	} pattern;

	uint8_t eir_len;
	uint8_t eir[];
};

/* Parent data structure to hold the test data and information,
 * used by tester_* functions and callbacks.
 */
struct test_data {
	enum test_type test_type;
	char *test_name;

	union {
		struct rssi_filter_test *rssi_filter_test_data;
		struct content_filter_test *content_filter_test_data;
	};
};

/* RSSI Filter Test 1:
 * - The Device Lost event should NOT get triggered even if the Adv RSSI is
 *   lower than LowRSSIThresh for more than LowRSSITimeout before finding
 *   the device first.
 * - Similarly, the Device Found event should NOT get triggered if the Adv RSSI
 *   is greater than LowRSSIThresh but lower than HighRSSIThresh.
 */
static struct rssi_filter_test rssi_data_1 = {
	.rssi_filter = {-40, 5, -60, 5},
	.test_steps = {
		{-70, 6, RESULT_DEVICE_NOT_FOUND},
		{-50, 6, RESULT_DEVICE_NOT_FOUND},
		END_OF_RSSI_TEST,
	},
};

/* RSSI Filter Test 2:
 * - The Device Found event should get triggered when the Adv RSSI is higher
 *   than HighRSSIThresh for more than HighRSSITimeout.
 * - Once the device is found, the Device Lost event should NOT get triggered
 *   if the Adv RSSI drops below HighRSSIThresh but it is not lower than
 *   LowRSSIThresh.
 * - When the Adv RSSI drops below LowRSSIThresh for more than LowRSSITimeout,
 *   the Device Lost event should get triggered.
 */
static struct rssi_filter_test rssi_data_2 = {
	.rssi_filter = {-40, 5, -60, 5},
	.test_steps = {
		{-30, 6, RESULT_DEVICE_FOUND},
		{-50, 6, RESULT_DEVICE_FOUND},
		{-70, 6, RESULT_DEVICE_LOST},
		END_OF_RSSI_TEST,
	},
};

/* RSSI Filter Test 3:
 * - The Device Found event should get triggered only when the Adv RSSI is
 *   higher than HighRSSIThresh for more than HighRSSITimeout.
 * - If the Adv RSSI drops below HighRSSIThresh, timer should reset and start
 *   counting once the Adv RSSI is above HighRSSIThresh.
 * - Similarly, when tracking the Low RSSI, timer should reset when the Adv RSSI
 *   goes above LowRSSIThresh. The Device Lost event should get triggered only
 *   when the Adv RSSI is lower than LowRSSIThresh for more than LowRSSITimeout.
 */
static struct rssi_filter_test rssi_data_3 = {
	.rssi_filter = {-40, 5, -60, 5},
	.test_steps = {
		{-30, 2, RESULT_DEVICE_NOT_FOUND},
		{-50, 6, RESULT_DEVICE_NOT_FOUND},
		{-30, 4, RESULT_DEVICE_NOT_FOUND},
		{-30, 2, RESULT_DEVICE_FOUND},
		{-70, 2, RESULT_DEVICE_FOUND},
		{-50, 6, RESULT_DEVICE_FOUND},
		{-70, 4, RESULT_DEVICE_FOUND},
		{-70, 2, RESULT_DEVICE_LOST},
		END_OF_RSSI_TEST,
	},
};

/* RSSI Filter Test 4:
 * - While tracking the High RSSI, timer should reset if the device goes
 *   offline/out-of-range for more than HighRSSITimeout.
 * - Once the device is found, if the device goes offline/out-of-range for
 *   more than LowRSSITimeout, the Device Lost event should get triggered.
 */
static struct rssi_filter_test rssi_data_4 = {
	.rssi_filter = {-40, 5, -60, 5},
	.test_steps = {
		{         -30, 2, RESULT_DEVICE_NOT_FOUND},
		{OUT_OF_RANGE, 6, RESULT_DEVICE_NOT_FOUND},
		{         -30, 4, RESULT_DEVICE_NOT_FOUND},
		{         -30, 2, RESULT_DEVICE_FOUND},
		{         -70, 2, RESULT_DEVICE_FOUND},
		{OUT_OF_RANGE, 6, RESULT_DEVICE_LOST},
		END_OF_RSSI_TEST,
	},
};

/* RSSI Filter Test 5:
 * - The Device Found event should get triggered only once even if the Adv RSSI
 *   stays higher than HighRSSIThresh for a longer period of time.
 * - Once the device is found, while tracking the Low RSSI, timer should reset
 *   when the Adv RSSI goes above LowRSSIThresh.
 * - The timer should NOT reset if the device goes offline/out-of-range for
 *   a very short period of time and comes back online/in-range before
 *   the timeouts.
 */
static struct rssi_filter_test rssi_data_5 = {
	.rssi_filter = {-40, 5, -60, 5},
	.test_steps = {
		{         -30, 2, RESULT_DEVICE_NOT_FOUND},
		{OUT_OF_RANGE, 2, RESULT_DEVICE_NOT_FOUND},
		{         -30, 2, RESULT_DEVICE_FOUND},
		{         -30, 3, RESULT_DEVICE_FOUND},
		{         -30, 3, RESULT_DEVICE_FOUND},
		{         -70, 2, RESULT_DEVICE_FOUND},
		{OUT_OF_RANGE, 2, RESULT_DEVICE_FOUND},
		{         -50, 6, RESULT_DEVICE_FOUND},
		{         -70, 2, RESULT_DEVICE_FOUND},
		{OUT_OF_RANGE, 2, RESULT_DEVICE_FOUND},
		{         -70, 2, RESULT_DEVICE_LOST},
		END_OF_RSSI_TEST,
	},
};

/* Content Filter Test 1:
 * The valid EIR data contains the given pattern whose content is a UUID16 AD
 * data.
 */
static struct content_filter_test content_data_1 = {
	.expected_match = true,
	.pattern = {0x03, 0x02, 0x02, {0x09, 0x18} },
	.eir_len = 20,
	.eir = {0x02, 0x01, 0x02,				// flags
		0x06, 0xff, 0x96, 0xfd, 0xab, 0xcd, 0xef,	// Mfr. Data
		0x05, 0x03, 0x0d, 0x18, 0x09, 0x18,		// 16-bit UUIDs
		0x05, 0x16, 0x0d, 0x18, 0x12, 0x34},		// Service Data
};

/* Content Filter Test 2:
 * The valid EIR data does not match the given pattern whose content is a UUID16
 * AD data.
 */
static struct content_filter_test content_data_2 = {
	.expected_match = false,
	.pattern = {0x03, 0x02, 0x02, {0x0d, 0x18} },
	.eir_len = 20,
	.eir = {0x02, 0x01, 0x02,				// flags
		0x06, 0xff, 0x96, 0xfd, 0xab, 0xcd, 0xef,	// Mfr. Data
		0x05, 0x03, 0x0d, 0x18, 0x09, 0x18,		// 16-bit UUIDs
		0x05, 0x16, 0x0d, 0x18, 0x12, 0x34},		// Service Data
};

/* Content Filter Test 3:
 * The valid EIR data does not have the given pattern whose content is a UUID32
 * AD data.
 */
static struct content_filter_test content_data_3 = {
	.expected_match = false,
	.pattern = {0x05, 0x00, 0x04, {0x09, 0x18, 0x00, 0x00} },
	.eir_len = 20,
	.eir = {0x02, 0x01, 0x02,				// flags
		0x06, 0xff, 0x96, 0xfd, 0xab, 0xcd, 0xef,	// Mfr. Data
		0x05, 0x03, 0x0d, 0x18, 0x09, 0x18,		// 16-bit UUIDs
		0x05, 0x16, 0x0d, 0x18, 0x12, 0x34},		// Service Data
};

/* Content Filter Test 4:
 * The valid EIR data does not match the given pattern whose content is a
 * UUID16 AD data due to invalid starting position of matching.
 */
static struct content_filter_test content_data_4 = {
	.expected_match = false,
	.pattern = {0x03, 0x02, 0x02, {0x09, 0x18} },
	.eir_len = 20,
	.eir = {0x02, 0x01, 0x02,				// flags
		0x06, 0xff, 0x96, 0xfd, 0xab, 0xcd, 0xef,	// Mfr. Data
		0x03, 0x03, 0x09, 0x18,				// 16-bit UUIDs
		0x05, 0x16, 0x0d, 0x18, 0x12, 0x34},		// Service Data
};

/* Initialize the data required for RSSI Filter test */
static void setup_rssi_filter_test(gpointer data)
{
	struct rssi_filter_test *test = data;

	test->adv_monitor_obj = btd_adv_monitor_rssi_test_setup(
					test->rssi_filter.high_rssi_threshold,
					test->rssi_filter.high_rssi_timeout,
					test->rssi_filter.low_rssi_threshold,
					test->rssi_filter.low_rssi_timeout);

	/* The RSSI Filter logic uses btd_device object only as a key in the
	 * adv_monitor->devices list, it is never dereferenced nor used to
	 * perform any operations related to btd_device. So we can use any
	 * dummy address for unit testing.
	 */
	test->btd_device_obj = DUMMY_BTD_DEVICE_OBJ;

	tester_setup_complete();
}

/* Cleanup after the RSSI Filter test is done */
static void teardown_rssi_filter_test(gpointer data)
{
	struct rssi_filter_test *test = data;

	btd_adv_monitor_rssi_test_teardown(test->adv_monitor_obj);

	tester_teardown_complete();
}

/* Execute the sub-steps of RSSI Filter test */
static gboolean test_rssi_filter(gpointer data)
{
	struct rssi_filter_test *test = data;
	time_t start_time = time(NULL);
	bool ret = false;

	uint16_t i = 0;
	uint16_t j = 0;

	/* If this is not the beginning of test, return to the sub-step
	 * before that test was suspended
	 */
	if (test->resume_step) {
		start_time = test->start_time;
		i = test->resume_step;

		/* Clear the test resume timer */
		g_source_remove(test->out_of_range_timer);
		test->out_of_range_timer = 0;

		/* Check state of the device - found/lost, while device was
		 * offline/out-of-range
		 */
		ret = btd_adv_monitor_test_device_state(test->adv_monitor_obj,
							test->btd_device_obj);
		tester_debug("%s: [t=%.0lf, step=%d] Test resume, "
			     "device_found = %s",
			     test->test_info->test_name,
			     difftime(time(NULL), start_time), i,
			     ret ? "true" : "false");
		g_assert(ret == test->test_steps[i].result);

		i++;
	}

	while (!RSSI_TEST_DONE(test->test_steps[i])) {
		if (test->test_steps[i].adv_rssi == OUT_OF_RANGE) {
			/* Simulate device offline/out-of-range by suspending
			 * the test.
			 *
			 * Note: All tester_* functions run sequentially by
			 * adding a next function to the main loop using
			 * g_idle_add(). If a timeout function is added using
			 * g_timeout_add_*(), it doesn't really get invoked as
			 * soon as the timer expires. Instead, it is invoked
			 * once the current function returns and the timer has
			 * expired. So, to give handle_device_lost_timeout()
			 * function a chance to run at the correct time, we
			 * must save the current state and exit from this
			 * function while we simulate the device offline. We can
			 * come back later to continue with the remaining steps.
			 */
			test->resume_step = i;
			test->start_time = start_time;
			test->out_of_range_timer = g_timeout_add_seconds(
						   test->test_steps[i].duration,
						   test_rssi_filter, data);

			/* Check the device state before suspending the test */
			ret = btd_adv_monitor_test_device_state(
							test->adv_monitor_obj,
							test->btd_device_obj);
			tester_debug("%s: [t=%.0lf, step=%d] Test suspend, "
				     "device_found = %s",
				     test->test_info->test_name,
				     difftime(time(NULL), start_time), i,
				     ret ? "true" : "false");
			return FALSE;
		}

		for (j = 0; j < test->test_steps[i].duration; j++) {
			ret = btd_adv_monitor_test_rssi(
						test->adv_monitor_obj,
						test->btd_device_obj,
						test->test_steps[i].adv_rssi);
			tester_debug("%s: [t=%.0lf, step=%d] Test "
				     "advertisement RSSI %d, device_found = %s",
				     test->test_info->test_name,
				     difftime(time(NULL), start_time), i,
				     test->test_steps[i].adv_rssi,
				     ret ? "true" : "false");

			/* Sleep for a second to simulate receiving
			 * advertisement once every second
			 */
			sleep(ADV_INTERVAL);
		}
		g_assert(ret == test->test_steps[i].result);

		i++;
	}

	tester_debug("%s: [t=%.0lf] Test done", test->test_info->test_name,
		     difftime(time(NULL), start_time));

	tester_test_passed();

	return FALSE;
}

/* Initialize the data required for Content Filter test */
static void setup_content_filter_test(gpointer data)
{
	struct content_filter_test *test = data;
	struct btd_adv_monitor_pattern *pattern = NULL;

	pattern = btd_adv_monitor_test_pattern_create(test->pattern.ad_type,
							test->pattern.offset,
							test->pattern.length,
							test->pattern.value);
	if (!pattern) {
		tester_setup_failed();
		return;
	}

	test->advmon_pattern = pattern;
	tester_setup_complete();
}

/* Cleanup after the Content Filter test is done */
static void teardown_content_filter_test(gpointer data)
{
	struct content_filter_test *test = data;

	if (!test)
		tester_teardown_complete();

	btd_adv_monitor_test_pattern_destroy(test->advmon_pattern);
	test->advmon_pattern = NULL;

	tester_teardown_complete();
}

/* Execute the sub-steps of Content Filter test */
static void test_content_filter(gpointer data)
{
	struct content_filter_test *test = data;
	struct btd_adv_monitor_pattern *pattern = test->advmon_pattern;

	if (!pattern) {
		tester_test_abort();
		return;
	}

	if (btd_adv_monitor_pattern_match(test->eir, test->eir_len,
						test->advmon_pattern) ==
		test->expected_match) {
		tester_test_passed();
		return;
	}

	tester_test_failed();
}

/* Handler function to prepare for a test */
static void setup_handler(gconstpointer data)
{
	const struct test_data *test = data;

	if (test->test_type == TEST_RSSI_FILTER)
		setup_rssi_filter_test(test->rssi_filter_test_data);
	else if (test->test_type == TEST_CONTENT_FILTER)
		setup_content_filter_test(test->content_filter_test_data);
}

/* Handler function to cleanup after the test is done */
static void teardown_handler(gconstpointer data)
{
	const struct test_data *test = data;

	if (test->test_type == TEST_RSSI_FILTER)
		teardown_rssi_filter_test(test->rssi_filter_test_data);
	else if (test->test_type == TEST_CONTENT_FILTER)
		teardown_content_filter_test(test->content_filter_test_data);
}

/* Handler function to execute a test with the given data set */
static void test_handler(gconstpointer data)
{
	const struct test_data *test = data;

	if (test->test_type == TEST_RSSI_FILTER)
		test_rssi_filter(test->rssi_filter_test_data);
	else if (test->test_type == TEST_CONTENT_FILTER)
		test_content_filter(test->content_filter_test_data);
}

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	__btd_log_init("*", 0);

	define_test("/advmon/rssi/1", TEST_RSSI_FILTER, rssi_data_1,
		    setup_handler, test_handler, teardown_handler);
	define_test("/advmon/rssi/2", TEST_RSSI_FILTER, rssi_data_2,
		    setup_handler, test_handler, teardown_handler);
	define_test("/advmon/rssi/3", TEST_RSSI_FILTER, rssi_data_3,
		    setup_handler, test_handler, teardown_handler);
	define_test("/advmon/rssi/4", TEST_RSSI_FILTER, rssi_data_4,
		    setup_handler, test_handler, teardown_handler);
	define_test("/advmon/rssi/5", TEST_RSSI_FILTER, rssi_data_5,
		    setup_handler, test_handler, teardown_handler);

	define_test("/advmon/content/1", TEST_CONTENT_FILTER, content_data_1,
		    setup_handler, test_handler, teardown_handler);
	define_test("/advmon/content/2", TEST_CONTENT_FILTER, content_data_2,
		    setup_handler, test_handler, teardown_handler);
	define_test("/advmon/content/3", TEST_CONTENT_FILTER, content_data_3,
		    setup_handler, test_handler, teardown_handler);
	define_test("/advmon/content/4", TEST_CONTENT_FILTER, content_data_4,
		    setup_handler, test_handler, teardown_handler);

	return tester_run();
}
