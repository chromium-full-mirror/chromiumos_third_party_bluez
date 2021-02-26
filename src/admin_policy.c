// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2021 Google LLC
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include "lib/bluetooth.h"
#include "lib/uuid.h"

#include "adapter.h"
#include "dbus-common.h"
#include "log.h"
#include "metrics.h"
#include "src/error.h"

#include "admin_policy.h"
#include "profile.h"

#define ADMIN_POLICY_INTERFACE		"org.bluez.AdminPolicy1"

struct btd_admin_policy {
	struct btd_adapter *adapter;
	uint16_t adapter_id;
	GHashTable *allowed_uuid_set;	/* Set of allowed service uuids*/
};

static GHashTable *uuid_set_create(void)
{
	return g_hash_table_new_full(bt_uuid_hash, bt_uuid_equal, g_free, NULL);
}

static bool parse_allow_service_list(struct btd_adapter *adapter,
					GHashTable **uuids,
					DBusMessage *msg)
{
	DBusMessageIter iter, arriter;

	dbus_message_iter_init(msg, &iter);
	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return false;

	*uuids = uuid_set_create();

	if (!*uuids) {
		error("Failed to create UUID allowed set");
		return false;
	}

	dbus_message_iter_recurse(&iter, &arriter);
	do {
		const int type = dbus_message_iter_get_arg_type(&arriter);
		char *uuid_param;
		bt_uuid_t *uuid;

		if (type == DBUS_TYPE_INVALID)
			break;

		if (type != DBUS_TYPE_STRING)
			goto failed;

		dbus_message_iter_get_basic(&arriter, &uuid_param);
		uuid = g_try_malloc(sizeof(*uuid));

		if (!uuid)
			goto failed;

		if (bt_string_to_uuid(uuid, uuid_param)) {
			g_free(uuid);
			goto failed;
		}

		g_hash_table_add(*uuids, uuid);
		dbus_message_iter_next(&arriter);
	} while (true);

	return true;

failed:
	g_hash_table_destroy(*uuids);
	*uuids = NULL;
	return false;
}

GHashTable *btd_admin_policy_allowlist_get(
					struct btd_admin_policy *admin_policy)
{
	return admin_policy ? admin_policy->allowed_uuid_set : NULL;
}

void btd_admin_policy_allowlist_set(struct btd_admin_policy *admin_policy,
							GHashTable *uuids)
{
	if (!admin_policy)
		return;

	admin_policy->allowed_uuid_set = uuids;

	/* This would add/remove profiles to the adapter according to the new
	 * policy.
	 */
	btd_profile_policy_update(admin_policy->adapter);

	/* Update auto-connect status to all devices */
	btd_adapter_refresh_is_blocked_by_policy(admin_policy->adapter);
}

static DBusMessage *set_service_allowlist(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct btd_admin_policy *admin_policy = user_data;
	struct btd_adapter *adapter = admin_policy->adapter;
	GHashTable *uuid_set;
	const char *sender = dbus_message_get_sender(msg);
	bool connectable;

	DBG("sender %s", sender);

	/* Parse parameters */
	if (!parse_allow_service_list(adapter, &uuid_set, msg)) {
		btd_error(admin_policy->adapter_id,
				"Failed on parsing allowed service list");
		return btd_error_invalid_args(msg);
	}

	/* Diconnect all the existing connections in case some of them are not
	 * allowed to use.
	 */
	btd_adapter_disconnect_all_devices(adapter);

	/* Clear existing allowlist */
	g_hash_table_destroy(admin_policy->allowed_uuid_set);

	btd_admin_policy_allowlist_set(admin_policy, uuid_set);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable admin_policy_methods[] = {
	{ GDBUS_EXPERIMENTAL_METHOD("SetServiceAllowList",
					GDBUS_ARGS({ "UUIDs", "as" }),
					NULL, set_service_allowlist) },
	{ }
};

bool btd_admin_policy_uuid_is_allowed(struct btd_admin_policy *admin_policy,
							const char *uuid_str)
{
	bt_uuid_t uuid;

	if (!admin_policy)
		return true;

	if (bt_string_to_uuid(&uuid, uuid_str)) {
		DBG("Failed to parse UUID string '%s'", uuid_str);
		return false;
	}

	return !g_hash_table_size(admin_policy->allowed_uuid_set) ||
		g_hash_table_contains(admin_policy->allowed_uuid_set, &uuid);
}

static struct btd_admin_policy *admin_policy_new(struct btd_adapter *adapter)
{
	struct btd_admin_policy *admin_policy;

	admin_policy = g_try_malloc(sizeof(*admin_policy));

	if (!admin_policy) {
		error("Failed to allocate memory for admin_policy");
		return NULL;
	}

	admin_policy->adapter = adapter;
	admin_policy->adapter_id = btd_adapter_get_index(adapter);
	admin_policy->allowed_uuid_set = uuid_set_create();

	if (!admin_policy->allowed_uuid_set) {
		error("Failed to create UUID allowed set");
		g_free(admin_policy);
		return NULL;
	}

	return admin_policy;
}

struct btd_admin_policy *btd_admin_policy_create(struct btd_adapter *adapter)
{
	struct btd_admin_policy *admin_policy;

	admin_policy = admin_policy_new(adapter);
	if (!admin_policy)
		return NULL;

	if (!g_dbus_register_interface(btd_get_dbus_connection(),
					adapter_get_path(admin_policy->adapter),
					ADMIN_POLICY_INTERFACE,
					admin_policy_methods, NULL, NULL,
					admin_policy, NULL)) {
		btd_error(admin_policy->adapter_id,
				"Failed to register "
				ADMIN_POLICY_INTERFACE);
		g_free(admin_policy);
		return NULL;
	}

	DBG("register interface success!");
	return admin_policy;
}

void btd_admin_policy_destroy(struct btd_admin_policy *admin_policy)
{
	if (!admin_policy)
		return;

	btd_info(admin_policy->adapter_id, "Destroy Admin Policy");

	g_dbus_unregister_interface(btd_get_dbus_connection(),
					adapter_get_path(admin_policy->adapter),
					ADMIN_POLICY_INTERFACE);
	g_hash_table_destroy(admin_policy->allowed_uuid_set);
	g_free(admin_policy);
}
