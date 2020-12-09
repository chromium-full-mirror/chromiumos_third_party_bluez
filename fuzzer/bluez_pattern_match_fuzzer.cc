// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstddef>
#include <cstdint>

class Environment {
	public:
		Environment() {
			// Set-up code.
		}
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	static Environment env;
	// Fuzzing code. Empty for now.
	return 0;
}
