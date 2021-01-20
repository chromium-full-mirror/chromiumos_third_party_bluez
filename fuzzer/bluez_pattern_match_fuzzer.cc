// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstdint>

extern "C" {
#include <stdlib.h>

#include "src/shared/ad.h"
#include "src/shared/queue.h"
}

#define MIN_DATA_LEN 2	// 1 byte len + 1 byte type

#define DATA_LEN(arr)	(sizeof(arr) / sizeof(arr[0]))

class Environment {
	public:
		struct queue *patterns; // List of bt_ad_pattern objects

		Environment() {
			struct bt_ad_pattern *pattern;

			uint8_t offset1  = 0;
			uint8_t ad_type1 = 0x03;
			uint8_t value1[] = {0x12, 0x18};
			int value_len1   = DATA_LEN(value1);

			uint8_t offset2  = 5;
			uint8_t ad_type2 = 0x09;
			uint8_t value2[] = {'T', 'E', 'S', 'T'};
			int value_len2   = DATA_LEN(value2);

			patterns = queue_new();

			pattern = bt_ad_pattern_new(ad_type1,
							offset1,
							value_len1,
							value1);
			queue_push_tail(patterns, pattern);

			pattern = bt_ad_pattern_new(ad_type2,
							offset2,
							value_len2,
							value2);
			queue_push_tail(patterns, pattern);
		}

		~Environment() {
			queue_destroy(patterns, free);
			patterns = NULL;
		}
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	static Environment env;

	struct bt_ad *ad;

	// Return early if the data size is not within the limits.
	if (size < MIN_DATA_LEN || size > BT_AD_MAX_DATA_LEN)
		return 0;

	// Fuzz pattern_match against random AD.
	ad = bt_ad_new_with_data(size, data);
	if (ad) {
		bt_ad_pattern_match(ad, env.patterns);
		bt_ad_unref(ad);
	}

	return 0;
}
