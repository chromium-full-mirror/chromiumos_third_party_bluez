/*
 * Copyright 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/file.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus/gdbus.h>

#include <bluetooth/bluetooth.h>

#include "lib/mgmt.h"
#include "lib/sdp.h"
#include "lib/uuid.h"

#include "src/adapter.h"
#include "src/dbus-common.h"
#include "src/device.h"
#include "src/error.h"
#include "src/log.h"
#include "src/plugin.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/shared/mgmt.h"
#include "src/textfile.h"

#define DBUS_PATH "/org/bluez"
#define DBUS_PLUGIN_INTERFACE "org.chromium.Bluetooth"
#define DBUS_PLUGIN_DEVICE_INTERFACE "org.chromium.BluetoothDevice"
#define DBUS_PLUGIN_EXPERIMENTAL_INTERFACE "org.chromium.BluetoothExperimental"

#define DBUS_BLUEZ_SERVICE "org.bluez"
#define DBUS_OBJECT_MANAGER_INTERFACE "org.freedesktop.DBus.ObjectManager"

#define DBUS_BLUEZ_DEVICE_INTERFACE "org.bluez.Device1"

#define NEWBLUE_CONFIG_FILE STORAGEDIR "/newblue"
#define LE_SPLITTER_STATUS_FILE "/sys/devices/virtual/misc/hci_le/le_splitter_enabled"

#define SERVICE_RETRIES 1
#define SERVICE_RETRY_TIMEOUT 2

// Range of valid connection intervals defined by the Bluetooth spec.
#define MIN_VALID_CONNECTION_INTERVAL 0x0006  // 6 * 1.25 = 7.5ms
#define MAX_VALID_CONNECTION_INTERVAL 0x0C80  // 3200 * 1.25 = 4000ms

// Default LE connection parameter values used by the kernel (hci_core.c).
#define DEFAULT_MIN_CONNECTION_INTERVAL 0x0028  // 40 * 1.25 = 50ms
#define DEFAULT_MAX_CONNECTION_INTERVAL 0x0038  // 56 * 1.25 = 70ms
#define DEFAULT_CONNECTION_LATENCY 0x0000
#define DEFAULT_CONNECTION_TIMEOUT 0x002A

static struct mgmt *mgmt_if = NULL;

static bool supports_le_services = false;
static bool supports_conn_info = false;

static int interfaces_added_watch_id = 0;
static int interfaces_removed_watch_id = 0;

static unsigned int service_id = 0;

static const char *services_to_reconnect[] = {
		HSP_AG_UUID, HFP_AG_UUID, NULL };
static GSList *retry_devices = NULL;

struct retry_data {
	struct btd_device *dev;
	uint8_t retries;
	guint timer;
};

static void destroy_retry_data(void* user_data)
{
	struct retry_data *data = user_data;

	if (data->timer > 0)
		g_source_remove(data->timer);

	g_free(data);
}

static struct retry_data *get_retry_data(struct btd_device *dev)
{
	struct retry_data *data;
	GSList *l;

	for (l = retry_devices; l ; l = l->next) {
		struct retry_data *data = l->data;

		if (data->dev == dev)
			return data;
	}

	data = g_new0(struct retry_data, 1);
	data->dev = dev;

	retry_devices = g_slist_prepend(retry_devices, data);
	return data;
}

static gboolean chromium_property_get_supports_le_services(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	dbus_bool_t value = supports_le_services;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);

	return TRUE;
}

static gboolean chromium_property_get_supports_conn_info(
					const GDBusPropertyTable *property,
					DBusMessageIter *iter, void *data)
{
	dbus_bool_t value = supports_conn_info;

	dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &value);

	return TRUE;
}

/* Helper functions and struct to find a device and the adapter it belongs to
 * for a given DBus object path.
 */
struct find_device_context {
	const char *device_path;
	struct btd_adapter *adapter;
	struct btd_device *device;
};

static void find_by_path_device_cb(struct btd_device *device, void *data) {
	struct find_device_context *context = data;

	if (strcmp(context->device_path, device_get_path(device)) == 0)
		context->device = device;
}

static void find_by_path_adapter_cb(struct btd_adapter *adapter,
							gpointer user_data) {
	struct find_device_context *context = user_data;

	context->adapter = adapter;
	btd_adapter_for_each_device(adapter, find_by_path_device_cb, context);
}

static gboolean find_device_by_path(const char *device_path,
					struct btd_adapter **out_adapter,
					struct btd_device **out_device) {
	struct find_device_context context;

	context.device_path = device_path;
	context.device = NULL;

	adapter_foreach(find_by_path_adapter_cb, &context);
	if (context.adapter == NULL || context.device == NULL)
	    return FALSE;

	*out_adapter = context.adapter;
	*out_device = context.device;
	return TRUE;
}

static void get_conn_info_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data) {
	DBusMessage *msg = user_data;
	DBusMessage *reply;
	const struct mgmt_rp_get_conn_info *rp;
	int16_t rssi, tx_power, max_tx_power;

	if (status == 0) {
		DBusMessageIter iter;

		reply = dbus_message_new_method_return(msg);
		if (reply == NULL) {
			dbus_message_unref(msg);
			error("Failed to create dbus reply message.");
			return;
		}

		rp = param;
		rssi = rp->rssi;
		tx_power = rp->tx_power;
		max_tx_power = rp->max_tx_power;

		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT16, &rssi);
		dbus_message_iter_append_basic(
					&iter, DBUS_TYPE_INT16, &tx_power);
		dbus_message_iter_append_basic(
					&iter, DBUS_TYPE_INT16, &max_tx_power);
	} else {
		reply = btd_error_failed(msg, mgmt_errstr(status));
		if (!reply) {
			dbus_message_unref(msg);
			error("Failed to create dbus error reply message.");
			return;
		}
	}

	if (!g_dbus_send_message(btd_get_dbus_connection(), reply))
		error("D-Bus send failed.");
	dbus_message_unref(msg);
}

static DBusMessage *get_conn_info(DBusConnection *conn, DBusMessage *msg,
								void *user_data)
{
	const char *device_path = dbus_message_get_path(msg);
	struct btd_adapter *adapter = NULL;
	struct btd_device *device = NULL;
	struct mgmt_cp_get_conn_info cp;

	if (!mgmt_if)
		return btd_error_not_ready(msg);

	if (!supports_conn_info)
		return btd_error_not_supported(msg);

	if (!find_device_by_path(device_path, &adapter, &device))
		return btd_error_does_not_exist(msg);

	if (!btd_device_is_connected(device))
		return btd_error_not_connected(msg);

	memset(&cp, 0, sizeof(cp));
	cp.addr.type = btd_device_get_bdaddr_type(device);
	cp.addr.bdaddr = *device_get_address(device);

	dbus_message_ref(msg);
	if (mgmt_send(mgmt_if, MGMT_OP_GET_CONN_INFO,
			btd_adapter_get_index(adapter), sizeof(cp), &cp,
			get_conn_info_complete, msg, NULL) == 0)
		return btd_error_failed(msg,
				"Failed to send get_conn_info mgmt command");
	return NULL;
}

static void load_conn_params_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	DBusMessage *msg = user_data;
	DBusMessage *reply;

	if (status != MGMT_STATUS_SUCCESS)
		reply = btd_error_failed(msg, mgmt_errstr(status));
	else
		reply = dbus_message_new_method_return(msg);

	if (!g_dbus_send_message(btd_get_dbus_connection(), reply))
		error("D-Bus send failed.");
	dbus_message_unref(msg);
}

static bool parse_connection_parameters(DBusMessage *msg,
					struct mgmt_conn_param* conn_param_out)
{
	uint16_t min_interval = DEFAULT_MIN_CONNECTION_INTERVAL;
	uint16_t max_interval = DEFAULT_MAX_CONNECTION_INTERVAL;
	DBusMessageIter iter, param_dict;

	if(!dbus_message_iter_init(msg, &iter))
		return false;

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
		return false;

	dbus_message_iter_recurse(&iter, &param_dict);
	while (dbus_message_iter_get_arg_type(&param_dict) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter param_entry;
		char *key;
		uint16_t value;

		// Parse key for current dictonary entry.
		dbus_message_iter_recurse(&param_dict, &param_entry);
		if (dbus_message_iter_get_arg_type(&param_entry) != DBUS_TYPE_STRING)
			break;
		dbus_message_iter_get_basic(&param_entry, &key);

		// Parse value for current dictionary entry.
		if (!dbus_message_iter_next(&param_entry)
				|| dbus_message_iter_get_arg_type(&param_entry) != DBUS_TYPE_UINT16)
			return false;
		dbus_message_iter_get_basic(&param_entry, &value);

		// Assign value to proper parameter.
		if (!strcmp("MinimumConnectionInterval", key))
			min_interval = value;
		else if (!strcmp("MaximumConnectionInterval", key))
			max_interval = value;
		else
			return false;

		dbus_message_iter_next(&param_dict);
	}

	if (dbus_message_iter_get_arg_type(&param_dict) != DBUS_TYPE_INVALID)
		return false;

	if (min_interval < MIN_VALID_CONNECTION_INTERVAL ||
			min_interval > MAX_VALID_CONNECTION_INTERVAL ||
			max_interval < MIN_VALID_CONNECTION_INTERVAL ||
			max_interval > MAX_VALID_CONNECTION_INTERVAL ||
			max_interval < min_interval)
		return false;

	conn_param_out->min_interval = min_interval;
	conn_param_out->max_interval = max_interval;
	conn_param_out->latency = DEFAULT_CONNECTION_LATENCY;
	conn_param_out->timeout = DEFAULT_CONNECTION_TIMEOUT;
	return true;
}

static DBusMessage *set_le_connection_parameters(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	const char *device_path = dbus_message_get_path(msg);
	struct btd_adapter *adapter = NULL;
	struct btd_device *device = NULL;
	struct mgmt_conn_param conn_param;
	struct mgmt_cp_load_conn_param *cp;
	size_t cp_size;
	unsigned int id;
	uint16_t bdaddr_type;

	if (!mgmt_if)
		return btd_error_not_ready(msg);

	if (!find_device_by_path(device_path, &adapter, &device))
		return btd_error_does_not_exist(msg);

	bdaddr_type = btd_device_get_bdaddr_type(device);
	if (bdaddr_type != BDADDR_LE_PUBLIC && bdaddr_type != BDADDR_LE_RANDOM)
		return btd_error_not_supported(msg);

	conn_param.addr.bdaddr = *device_get_address(device);
	conn_param.addr.type = bdaddr_type;

	if (!parse_connection_parameters(msg, &conn_param))
		return btd_error_invalid_args(msg);

	cp_size = sizeof(struct mgmt_cp_load_conn_param) + sizeof(conn_param);
	cp = g_try_malloc0(cp_size);
	if (cp == NULL)
		return btd_error_failed(msg, "Failed to allocate memory.");
	cp->param_count = htobs(1);
	memcpy(cp->params, &conn_param, sizeof(conn_param));

	dbus_message_ref(msg);
	id = mgmt_send(mgmt_if, MGMT_OP_LOAD_CONN_PARAM,
			btd_adapter_get_index(adapter), cp_size, cp,
			load_conn_params_complete, msg, NULL);

	g_free(cp);

	if (!id)
		return btd_error_failed(msg, "Failed to call mgmt API.");
	return NULL;
}

static const GDBusMethodTable device_methods[] = {
	/* GetConnInfo is a simple DBus wrapper over the get_conn_info mgmt API.
	 */
	{ GDBUS_ASYNC_METHOD("GetConnInfo", NULL, GDBUS_ARGS({"TXPower", "y"},
					{"MaximumTXPower", "y"}, {"RSSI", "y"}),
		get_conn_info) },
	{ GDBUS_ASYNC_METHOD("SetLEConnectionParameters",
		GDBUS_ARGS({ "parameters", "a{sq}"}), NULL,
		set_le_connection_parameters) },
	{ }
};

static bool is_interface_entry_bluez_device(DBusMessageIter *array_iter) {
	int arg_type;
	DBusMessageIter dict_iter;
	char *interface = NULL;

	arg_type = dbus_message_iter_get_arg_type(array_iter);
	if (arg_type == 'e') {
		dbus_message_iter_recurse(array_iter, &dict_iter);
		arg_type = dbus_message_iter_get_arg_type(&dict_iter);
		if (arg_type == 's')
			dbus_message_iter_get_basic(&dict_iter, &interface);
		else
			error("Expected string in InterfaceAdded signal.");

	} else if (arg_type == 's') {
		dbus_message_iter_get_basic(array_iter, &interface);
	} else {
		error("Expected string in InterfaceRemoved signal.");
	}

	return interface &&
			strcmp(interface, DBUS_BLUEZ_DEVICE_INTERFACE) == 0;
}

/* Given an InterfaceAdded or InterfaceRemoved ObjectManager signal, return
 * the object path if it contains the BlueZ device interface; otherwise, return
 * null.
 *
 * The documentation for these ObjectManager signals can be found at
 * http://dbus.freedesktop.org/doc/dbus-specification.html#standard-interfaces-objectmanager
 */
static const char *get_device_path_from_interface_msg(DBusMessage *msg) {
	int arg_type;
	char *object_path = NULL;
	DBusMessageIter args_iter, array_iter;

	dbus_message_iter_init(msg, &args_iter);
	arg_type = dbus_message_iter_get_arg_type(&args_iter);
	if (arg_type != 'o') {
		error("Expected object path in ObjectManager signal.");
		return NULL;
	}

	dbus_message_iter_get_basic(&args_iter, &object_path);
	dbus_message_iter_next(&args_iter);
	if (!object_path)
		return NULL;

	arg_type = dbus_message_iter_get_arg_type(&args_iter);
	if (arg_type != 'a') {
		error("Expected array in ObjectManager signal.");
		return NULL;
	}

	dbus_message_iter_recurse(&args_iter, &array_iter);
	while (dbus_message_iter_has_next(&array_iter)) {
		if (is_interface_entry_bluez_device(&array_iter))
		    return object_path;
		dbus_message_iter_next(&array_iter);
	}

	return NULL;
}

static gboolean interfaces_added(DBusConnection *conn, DBusMessage *msg,
								void *user_data)
{
	const char *device_path = get_device_path_from_interface_msg(msg);

	if (!device_path)
		return TRUE;

	g_dbus_register_interface(btd_get_dbus_connection(),
					device_path, DBUS_PLUGIN_DEVICE_INTERFACE,
					device_methods, NULL, NULL, NULL, NULL);

	return TRUE;
}

static gboolean interfaces_removed(DBusConnection *conn, DBusMessage *msg,
								void *user_data)
{
	const char *device_path = get_device_path_from_interface_msg(msg);

	if (!device_path)
		return TRUE;

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				device_path, DBUS_PLUGIN_DEVICE_INTERFACE);

	return TRUE;
}

static void remove_dbus_watches() {
	if (interfaces_added_watch_id)
		g_dbus_remove_watch(btd_get_dbus_connection(),
						interfaces_added_watch_id);

	if (interfaces_removed_watch_id)
		g_dbus_remove_watch(btd_get_dbus_connection(),
						interfaces_removed_watch_id);
}

static const GDBusPropertyTable chromium_properties[] = {
	{ "SupportsLEServices", "b",
				chromium_property_get_supports_le_services },
	{ "SupportsConnInfo", "b",
				chromium_property_get_supports_conn_info },
	{ }
};

/* Checks if any type of audio gateway is connected. There are two profiles
 * we care about here: HFP and HSP. */
static gboolean is_dev_connected(struct btd_device *dev)
{
	struct btd_service *service;
	const char **uuid;

	for (uuid = services_to_reconnect; *uuid; uuid++) {
		service = btd_device_get_service(dev, *uuid);
		if (service == NULL)
			continue;
		if (btd_service_get_state(service) ==
				BTD_SERVICE_STATE_CONNECTED)
			return TRUE;
	}
	return FALSE;
}

static gboolean connect_dev(gpointer user_data)
{
	struct retry_data *data = user_data;
	struct btd_service *service;
	struct btd_profile *profile;
	const char **uuid;

	data->timer = 0;
	data->retries++;

	if (is_dev_connected(data->dev))
		return FALSE;

	for (uuid = services_to_reconnect; *uuid; uuid++) {
		service = btd_device_get_service(data->dev, *uuid);
		if (service == NULL)
			continue;

		profile = btd_service_get_profile(service);
		info("Reconnect profile %s", profile->name);

		btd_service_connect(service);
		return TRUE;
	}
	return FALSE;
}

static void set_timer(gpointer user_data)
{
	struct retry_data *data = user_data;

	if (is_dev_connected(data->dev))
		return;

	if (data->timer == 0)
		data->timer = g_timeout_add_seconds(SERVICE_RETRY_TIMEOUT,
						    connect_dev, data);
}

static void service_cb(struct btd_service *service,
		       btd_service_state_t old_state,
		       btd_service_state_t new_state,
		       void *user_data)
{
	struct btd_device *dev = btd_service_get_device(service);
	struct btd_profile *profile = btd_service_get_profile(service);
	struct retry_data *data;
	const char **uuid;
	bool reconnect = false;

	for (uuid = services_to_reconnect; *uuid; uuid++) {
		if (g_str_equal(profile->remote_uuid, *uuid)) {
			reconnect = true;
			break;
		}
	}
	if (!reconnect)
		return;

	data = get_retry_data(dev);

	switch (new_state) {
	case BTD_SERVICE_STATE_UNAVAILABLE:
		if (data->timer > 0) {
			g_source_remove(data->timer);
			data->timer = 0;
		}
		break;
	case BTD_SERVICE_STATE_DISCONNECTED:
		if (old_state == BTD_SERVICE_STATE_CONNECTING) {
			int err = btd_service_get_error(service);

			if (err == -EAGAIN) {
				if (data->retries < SERVICE_RETRIES)
					set_timer(data);
				else
					data->retries = 0;
			} else if (data->timer > 0) {
				g_source_remove(data->timer);
				data->timer = 0;
			}
		}
		break;
	case BTD_SERVICE_STATE_CONNECTING:
		break;
	case BTD_SERVICE_STATE_CONNECTED:
		if (data->timer > 0) {
			g_source_remove(data->timer);
			data->timer = 0;
		}
		break;
	case BTD_SERVICE_STATE_DISCONNECTING:
		break;
	}
}

static void read_version_complete(uint8_t status, uint16_t length,
					const void *param, void *user_data)
{
	const struct mgmt_rp_read_version *rp = param;
	uint8_t mgmt_version, mgmt_revision;

	if (status != MGMT_STATUS_SUCCESS) {
		error("Failed to read version information: %s (0x%02x)",
						mgmt_errstr(status), status);
		return;
	}

	if (length < sizeof(*rp)) {
		error("Wrong size of read version response");
		return;
	}

	mgmt_version = rp->version;
	mgmt_revision = btohs(rp->revision);

	supports_le_services = (mgmt_version > 1 ||
		(mgmt_version == 1 && mgmt_revision >= 4));
	supports_conn_info = (mgmt_revision > 1 ||
		(mgmt_version == 1 && mgmt_revision >= 5));

	g_dbus_emit_property_changed(btd_get_dbus_connection(),
		DBUS_PATH, DBUS_PLUGIN_INTERFACE, "SupportsLEServices");
	g_dbus_emit_property_changed(btd_get_dbus_connection(),
		DBUS_PATH, DBUS_PLUGIN_INTERFACE, "SupportsConnInfo");
}

static DBusMessage *set_newblue_enabled(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	dbus_bool_t enable = FALSE;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &enable,
							DBUS_TYPE_INVALID)) {
		return btd_error_invalid_args(msg);
	}

	create_file(NEWBLUE_CONFIG_FILE, S_IRUSR | S_IWUSR);

	if (!g_file_set_contents(NEWBLUE_CONFIG_FILE,
				(enable == TRUE) ? "1" : "0", 1, NULL)) {
		return btd_error_failed(msg, "Failed to configure newblue");
	}

	return dbus_message_new_method_return(msg);
}

static const char *get_newblue_status_message()
{
	bool newblue_enabled = FALSE, splitter_enabled = FALSE;
	char *buf;

	if (!g_file_get_contents(LE_SPLITTER_STATUS_FILE, &buf, NULL, NULL))
		return "newblue is not supported";

	if (strncmp("ON", buf, 2) == 0)
		splitter_enabled = TRUE;

	g_free(buf);

	if (g_file_get_contents(NEWBLUE_CONFIG_FILE, &buf, NULL, NULL)
							&& buf[0] == '1') {
		newblue_enabled = TRUE;
	}

	// if g_file_get_contents fail, buf will be NULL so it's safe to free.
	g_free(buf);

	if (newblue_enabled) {
		if (splitter_enabled)
			return "enabled";
		else
			return "enabled but not applied (reboot required)";
	} else {
		if (splitter_enabled)
			return "disabled but not applied (reboot required)";
		else
			return "disabled";
	}
}

static DBusMessage *get_newblue_enabled(DBusConnection *conn, DBusMessage *msg,
								void *data)
{
	const char *status = get_newblue_status_message();

	DBusMessage *reply = dbus_message_new_method_return(msg);
	dbus_message_append_args(reply,
				DBUS_TYPE_STRING, &status,
				DBUS_TYPE_INVALID);
	return reply;
}

static const GDBusMethodTable experimental_methods[] = {
	{ GDBUS_METHOD("SetNewblueEnabled", GDBUS_ARGS({ "enable", "b" }), NULL,
							set_newblue_enabled) },
	{ GDBUS_METHOD("GetNewblueEnabled", NULL, NULL, get_newblue_enabled) },
	{}
};

static int chromium_init(void)
{
	DBG("");

	mgmt_if = mgmt_new_default();
	if (!mgmt_if)
		error("Failed to access management interface");
	else if (!mgmt_send(mgmt_if, MGMT_OP_READ_VERSION,
					MGMT_INDEX_NONE, 0, NULL,
					read_version_complete, NULL, NULL))
		error("Failed to read management version information");

	g_dbus_register_interface(btd_get_dbus_connection(),
		DBUS_PATH, DBUS_PLUGIN_INTERFACE,
		NULL, NULL, chromium_properties, NULL, NULL);

	g_dbus_register_interface(btd_get_dbus_connection(), DBUS_PATH,
			DBUS_PLUGIN_EXPERIMENTAL_INTERFACE,
			experimental_methods, NULL, NULL, NULL, NULL);

	service_id = btd_service_add_state_cb(service_cb, NULL);

	/* Listen for new device objects being added so we can add the plugin
	 * interface to them.
	 */
	interfaces_added_watch_id = g_dbus_add_signal_watch(
			btd_get_dbus_connection(), DBUS_BLUEZ_SERVICE,
			"/", DBUS_OBJECT_MANAGER_INTERFACE, "InterfacesAdded",
			interfaces_added, NULL, NULL);
	if (!interfaces_added_watch_id) {
		error("Failed to add watch for InterfacesAdded signal");
		return 0;
	}

	interfaces_removed_watch_id = g_dbus_add_signal_watch(
			btd_get_dbus_connection(), DBUS_BLUEZ_SERVICE,
			"/", DBUS_OBJECT_MANAGER_INTERFACE, "InterfacesRemoved",
			interfaces_removed, NULL, NULL);
	if (!interfaces_removed_watch_id) {
		error("Failed to add watch for InterfaceRemoved signal");
		remove_dbus_watches();
	}

	return 0;
}

static void chromium_exit(void)
{
	DBG("");

	mgmt_unref(mgmt_if);
	mgmt_if = NULL;

	btd_service_remove_state_cb(service_id);
	g_slist_free_full(retry_devices, destroy_retry_data);

	remove_dbus_watches();
}

BLUETOOTH_PLUGIN_DEFINE(chromium, VERSION, BLUETOOTH_PLUGIN_PRIORITY_HIGH,
						chromium_init, chromium_exit)
