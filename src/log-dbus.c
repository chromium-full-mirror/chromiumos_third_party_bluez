// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "log-dbus.h"

#include <stdint.h>
#include <string.h>
#include <dbus/dbus.h>

#include "bluetooth/bluetooth.h"
#include "gdbus/gdbus.h"
#include "lib/mgmt.h"

#include "src/shared/mgmt.h"

#include "dbus-common.h"
#include "log.h"

#define BLUETOOTH_SERVICE	"org.chromium.Bluetooth"
#define DEBUG_OBJECT_PATH	"/org/chromium/Bluetooth"
#define DEBUG_INTERFACE		"org.chromium.Bluetooth.Debug"
#define DEBUG_BLUEZ_PROPERTY	"BluezLevel"
#define DEBUG_KERNEL_PROPERTY	"KernelLevel"

static GDBusClient *client;
static struct mgmt *mgmt = NULL;

static void update_bluez_debug(DBusMessageIter *iter)
{
	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_BYTE) {
		error("Wrong arg type is supplied to BlueZ debug level");
		return;
	}

	unsigned char level;
	dbus_message_iter_get_basic(iter, &level);

	btd_set_debug_level(level);
}

static void update_kernel_debug(DBusMessageIter *iter)
{
	/* d4992530-b9ec-469f-ab01-6c481c47da1c */
	static const uint8_t uuid[16] = {
				0x1c, 0xda, 0x47, 0x1c, 0x48, 0x6c, 0x01, 0xab,
				0x9f, 0x46, 0xec, 0xb9, 0x30, 0x25, 0x99, 0xd4,
	};
	struct mgmt_cp_set_exp_feature cp;

	memset(&cp, 0, sizeof(cp));
	memcpy(cp.uuid, uuid, 16);

	if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_BYTE) {
		error("Wrong arg type is supplied to kernel debug level");
		return;
	}

	dbus_message_iter_get_basic(iter, &cp.action);
	if (cp.action > 1) {
		error("Unexpected kernel debug level %u", cp.action);
		return;
	}

	mgmt_send(mgmt, MGMT_OP_SET_EXP_FEATURE, MGMT_INDEX_NONE,
			sizeof(cp), &cp, NULL, NULL, NULL);
}

static void handle_property_changed(GDBusProxy *proxy, const char *name,
					DBusMessageIter *iter, void *user_data)
{
	const char *interface = g_dbus_proxy_get_interface(proxy);

	if (!strcmp(interface, DEBUG_INTERFACE)) {
		if (!strcmp(name, DEBUG_BLUEZ_PROPERTY))
			update_bluez_debug(iter);
		if (!strcmp(name, DEBUG_KERNEL_PROPERTY))
			update_kernel_debug(iter);
	}
}

static void handle_proxy_added(GDBusProxy *proxy, void *user_data)
{
	const char *interface = g_dbus_proxy_get_interface(proxy);
	DBusMessageIter iter;

	if (!strcmp(interface, DEBUG_INTERFACE)) {
		if (g_dbus_proxy_get_property(proxy, DEBUG_BLUEZ_PROPERTY,
								&iter)) {
			update_bluez_debug(&iter);
		}
		if (g_dbus_proxy_get_property(proxy, DEBUG_KERNEL_PROPERTY,
								&iter)) {
			update_kernel_debug(&iter);
		}
	}
}

void btd_debug_init()
{
	DBusConnection *conn = btd_get_dbus_connection();

	client = g_dbus_client_new(conn, BLUETOOTH_SERVICE, DEBUG_OBJECT_PATH);
	if (!client) {
		error("Failed to create dbus client");
		return;
	}

	mgmt = mgmt_new_default();
	if (!mgmt) {
		error("Failed to access management interface");
		return;
	}

	g_dbus_client_set_proxy_handlers(client, handle_proxy_added, NULL,
					handle_property_changed, client);
}

void btd_debug_cleanup()
{
	if (client)
		g_dbus_client_unref(client);

	if (mgmt)
		mgmt_unref(mgmt);
}
