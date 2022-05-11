/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Google LLC
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

#ifndef BLUEZ_METRICS_ALLOWLIST_H_
#define BLUEZ_METRICS_ALLOWLIST_H_

#include <stdbool.h>

bool is_device_info_in_allowlist(int vendor_id_source, int vendor_id,
				 int product_id);
bool is_chipset_info_in_allowlist(int vendor_id, int product_id, int transport,
				  const char *chipset_string, uint64_t *hval);
#endif // BLUEZ_METRICS_ALLOWLIST_H_
