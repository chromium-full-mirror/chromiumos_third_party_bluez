/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2021  Google LLC
 *
 *
 */

#include <glib.h>

struct btd_adapter;
struct btd_admin_policy;

struct btd_admin_policy *btd_admin_policy_create(struct btd_adapter *adapter);
void btd_admin_policy_destroy(struct btd_admin_policy *admin_policy);
GHashTable *btd_admin_policy_allowlist_get(
					struct btd_admin_policy *admin_policy);
void btd_admin_policy_allowlist_set(struct btd_admin_policy *admin_policy,
							GHashTable *uuids);
bool btd_admin_policy_uuid_is_allowed(struct btd_admin_policy *admin_policy,
							const char *uuid_str);
