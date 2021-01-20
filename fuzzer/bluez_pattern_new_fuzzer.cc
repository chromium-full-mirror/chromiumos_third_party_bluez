// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstdint>

#include <fuzzer/FuzzedDataProvider.h>

extern "C" {
#include <stdlib.h>

#include "src/shared/ad.h"
#include "src/shared/queue.h"
}

#define MIN_DATA_LEN 4	// 1byte offset + 1byte len + 1byte type + 1byte data

#define DATA_LEN(arr)	(sizeof(arr) / sizeof(arr[0]))

class Environment {
	public:
		struct bt_ad *ad;

		Environment() {
			uint8_t data[] = {0x03, 0x03, 0x12, 0x18};
			uint8_t data_len = DATA_LEN(data);

			ad = bt_ad_new_with_data(data_len, data);
		}

		~Environment() {
			bt_ad_unref(ad);
			ad = NULL;
		}
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	static Environment env;

	FuzzedDataProvider fuzzed_data(data, size);

	uint8_t offset;
	uint8_t ad_type;
	uint8_t value[BT_AD_MAX_DATA_LEN];
	int value_len;

	struct queue *patterns; // List of bt_ad_pattern objects
	struct bt_ad_pattern *pattern;

	patterns = queue_new();

	while (fuzzed_data.remaining_bytes() >= MIN_DATA_LEN) {
		// Extract random offset in range (0..BT_AD_MAX_DATA_LEN-3),
		// random value len in range (1..BT_AD_MAX_DATA_LEN-2),
		// and random ad data type.
		offset = fuzzed_data.ConsumeIntegralInRange <uint8_t>
						(0, BT_AD_MAX_DATA_LEN - 3);
		value_len = fuzzed_data.ConsumeIntegralInRange <uint8_t>
						(1, BT_AD_MAX_DATA_LEN - 2);
		ad_type = fuzzed_data.ConsumeIntegral <uint8_t> ();

		// Extract random field data.
		if (value_len > fuzzed_data.remaining_bytes())
			value_len = fuzzed_data.remaining_bytes();
		fuzzed_data.ConsumeData(value, value_len);

		// Fuzz pattern_match against random patterns.
		pattern = bt_ad_pattern_new(ad_type, offset, value_len, value);
		if (pattern)
			queue_push_tail(patterns, pattern);
	}

	if (!queue_isempty(patterns))
		bt_ad_pattern_match(env.ad, patterns);
	queue_destroy(patterns, free);

	return 0;
}
