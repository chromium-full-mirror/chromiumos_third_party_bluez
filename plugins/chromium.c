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
#include <stdio.h>
#include <stdlib.h>

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
#include "src/metrics.h"
#include "src/plugin.h"
#include "src/profile.h"
#include "src/service.h"
#include "src/shared/mgmt.h"

#define DBUS_PATH "/org/bluez"
#define DBUS_BLUEZ_SERVICE "org.bluez"

#define DBUS_PLUGIN_INTERFACE "org.chromium.Bluetooth"

#define DEBUG_QUALITY_FILE_PATH		"/var/lib/bluetooth/quality.conf"
#define DEBUG_CONF_FILE_PATH		"/var/lib/bluetooth/debug.conf"
#define DEBUG_LL_PRIVACY_CONF_PATH      "/var/lib/bluetooth/bluetooth-llprivacy.experimental"
#define DEBUG_OBJECT_PATH		"/org/chromium/Bluetooth"
#define DEBUG_INTERFACE			"org.chromium.Bluetooth.Debug"
#define DEBUG_BLUEZ_PROPERTY		"BluezLevel"
#define DEBUG_KERNEL_PROPERTY		"KernelLevel"

#define METRICS_INTERFACE		"org.chromium.Bluetooth.Metrics"

// Kernel only accepts boolean value
#define MAX_KERNEL_DEBUG_LEVEL 1

// Since we need to migrate from the old api which accepted 4 values, we need to
// preserve 4 as the max.
#define MAX_DEBUG_LEVELS 4

#define DBUS_PLUGIN_DEVICE_INTERFACE "org.chromium.BluetoothDevice"

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

static unsigned int service_id = 0;

static const char *services_to_reconnect[] = {
		HSP_AG_UUID, HFP_AG_UUID, NULL };
static GSList *retry_devices = NULL;

static GSList *registered_devices = NULL;

/* 15c0a148-c273-11ea-b3de-0242ac130004 */
static const uint8_t ll_privacy_uuid[16] = {
	0x04, 0x00, 0x13, 0xac, 0x42, 0x02, 0xde, 0xb3,
	0xea, 0x11, 0x73, 0xc2, 0x48, 0xa1, 0xc0, 0x15,
};


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
	{ GDBUS_ASYNC_METHOD("GetConnInfo", NULL, GDBUS_ARGS({"TXPower", "n"},
					{"MaximumTXPower", "n"}, {"RSSI", "n"}),
		get_conn_info) },
	{ GDBUS_ASYNC_METHOD("SetLEConnectionParameters",
		GDBUS_ARGS({ "parameters", "a{sq}"}), NULL,
		set_le_connection_parameters) },
	{ }
};

static gboolean on_device_added(struct btd_adapter *adapter,
						struct btd_device *device)
{
	const char *device_path = device_get_path(device);

	g_dbus_register_interface(btd_get_dbus_connection(),
					device_path, DBUS_PLUGIN_DEVICE_INTERFACE,
					device_methods, NULL, NULL, NULL, NULL);

	registered_devices = g_slist_prepend(registered_devices, device);

	return TRUE;
}

static gboolean on_device_removed(struct btd_adapter *adapter,
						struct btd_device *device)
{
	const char *device_path = device_get_path(device);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				device_path, DBUS_PLUGIN_DEVICE_INTERFACE);

	registered_devices = g_slist_remove(registered_devices, device);

	return TRUE;
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

static void update_kernel_debug(uint8_t level)
{
	/* d4992530-b9ec-469f-ab01-6c481c47da1c */
	static const uint8_t uuid[16] = {
				0x1c, 0xda, 0x47, 0x1c, 0x48, 0x6c, 0x01, 0xab,
				0x9f, 0x46, 0xec, 0xb9, 0x30, 0x25, 0x99, 0xd4,
	};
	struct mgmt_cp_set_exp_feature cp;

	memset(&cp, 0, sizeof(cp));
	memcpy(cp.uuid, uuid, 16);

	cp.action = level;
	if (cp.action > 1) {
		error("Unexpected kernel debug level %u", cp.action);
		return;
	}

	mgmt_send(mgmt_if, MGMT_OP_SET_EXP_FEATURE, MGMT_INDEX_NONE,
			sizeof(cp), &cp, NULL, NULL, NULL);
}

struct debug_data {
	uint8_t bluez;
	uint8_t kernel;
};

bool read_debug_levels_from_file(struct debug_data *debug)
{
	FILE *fp;
	uint8_t values[MAX_DEBUG_LEVELS];
	int count;
	ssize_t bytes;
	char *line = NULL;
	size_t len = 0;
	long value;

	fp = fopen(DEBUG_CONF_FILE_PATH, "r");
	if (!fp)
		return false;

	for (count = 0; count < MAX_DEBUG_LEVELS; ++count) {
		bytes = getline(&line, &len, fp);
		if (bytes < 0)
			break;

		// Any value > UINT8_MAX is treated as 0
		value = strtol(line, NULL, 10);
		if (value > 255)
			value = 0;

		values[count] = (uint8_t)value;
	}

	// Free allocation by getline
	if (line)
		free(line);

	fclose(fp);

	// We only support 2 entries or 4 entries (legacy saved values)
	if (count != 2 && count != 4) {
		warn("Unsupported debug levels from file. Got %d", count);
		return false;
	}

	if (count == 4) {
		debug->bluez = values[2];
		debug->kernel = values[3];
	} else {
		debug->bluez = values[0];
		debug->kernel = values[1];
	}

	return true;
}

void apply_debug_levels(struct debug_data *debug, uint8_t bluez, uint8_t kernel)
{
	// Limit values to valid values
	if (bluez > MAX_BLUEZ_DEBUG_LEVEL) {
		warn("Given bluez log level (%d) is invalid. Using %d.", bluez,
		     MAX_BLUEZ_DEBUG_LEVEL);
		bluez = MAX_BLUEZ_DEBUG_LEVEL;
	}

	debug->bluez = bluez;

	if (kernel > MAX_KERNEL_DEBUG_LEVEL) {
		warn("Given kernel log level (%d) is invalid. Using %d.",
		     kernel, MAX_KERNEL_DEBUG_LEVEL);
		kernel = MAX_KERNEL_DEBUG_LEVEL;
	}

	debug->kernel = kernel;

	btd_set_debug_level(bluez);
	update_kernel_debug(kernel);

	info("Applied debug levels: bluez(%u), kernel(%u)", bluez, kernel);
}

bool store_debug_levels(struct debug_data *debug)
{
	FILE *fp;

	fp = fopen(DEBUG_CONF_FILE_PATH, "w");
	if (!fp)
		return false;

	fprintf(fp, "%u\n", debug->bluez);
	fprintf(fp, "%u\n", debug->kernel);

	fclose(fp);
	return true;
}

/* New api that only accepts bluez and kernel log levels. */
static DBusMessage *set_log_levels(DBusConnection *conn, DBusMessage *msg,
				   void *user_data)
{
	struct debug_data *debug = user_data;
	DBusError err;
	uint8_t bluez, kernel;

	dbus_error_init(&err);

	if (!dbus_message_get_args(msg, &err, DBUS_TYPE_BYTE, &bluez,
				   DBUS_TYPE_BYTE, &kernel,
				   DBUS_TYPE_INVALID)) {
		error("read params failed %s", err.message);
		dbus_error_free(&err);

		return btd_error_failed(msg, "Failed to read parameters");
	}

	apply_debug_levels(debug, bluez, kernel);
	if (!store_debug_levels(debug))
		warn("Unable to save debug log levels");

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_quality_debug(DBusConnection *conn, DBusMessage *msg,
				      void *user_data)
{
	dbus_bool_t quality_debug = false;
	struct btd_adapter *adapter;

	adapter = btd_adapter_get_default();
	if (!adapter) {
		error("No default adapter. Skip setting quality debug.");
		return btd_error_no_such_adapter(msg);
	}

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &quality_debug,
				   DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	if (!quality_set_debug(adapter, (bool)quality_debug)) {
		error("quality_set_debug failed");
		return btd_error_failed(msg, "quality_set_debug");
	}

	info("quality_set_debug %u", (bool)quality_debug);

	return dbus_message_new_method_return(msg);
}

static bool store_quality_conf(uint8_t action)
{
	FILE *fp;

	fp = fopen(DEBUG_QUALITY_FILE_PATH, "w");
	if (!fp)
		return false;

	fprintf(fp, "%u\n", action);

	fclose(fp);
	return true;
}

static DBusMessage *set_quality(DBusConnection *conn, DBusMessage *msg,
			    void *user_data)
{
	uint8_t action;
	struct btd_adapter *adapter;

	adapter = btd_adapter_get_default();
	if (!adapter) {
		error("No default adapter. Skip setting quality debug.");
		return btd_error_no_such_adapter(msg);
	}

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_BYTE, &action,
				   DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	if (!set_bluetooth_quality_report(adapter, action))
		return btd_error_failed(msg, "SetQuality");

	info("SetQuality action %u succeeded", action);

	/* Always overwrite the quality conf file with the latest value.*/
	if (!store_quality_conf(action))
		warn("Unable to save %s", DEBUG_QUALITY_FILE_PATH);

	return dbus_message_new_method_return(msg);
}

static bool write_value_to_conf_file(dbus_bool_t enabled)
{
	FILE *fp;

	fp = fopen(DEBUG_LL_PRIVACY_CONF_PATH, "w");
	if (!fp)
		return false;

	if (enabled)
		fprintf(fp, "%s\n", "enable");
	else
		fprintf(fp, "%s\n", "disable");

	fclose(fp);
	return true;
}

static bool read_llprivacy_status_from_file(bool *status)
{
	FILE *fp;
	ssize_t bytes;
	char *line = NULL;
	size_t len = 0;

	fp = fopen(DEBUG_LL_PRIVACY_CONF_PATH, "r");
	// if file does not exist, return false
	// because the file cannot be read by 'grep'
	if (!fp) {
		*status = false;
		return false;
	}
	bytes = getline(&line, &len, fp);

	// Anything not enable is considered as disable
	*status = !(bytes < 0 || strcmp(line, "enable\n"));

	// Free allocation by getline
	if (line)
		free(line);

	fclose(fp);
	return true;
}

static void set_device_privacy_cb(uint8_t status, uint16_t length,
				  const void *param, void *user_data)
{
	struct btd_adapter *adapter = user_data;

	adapter_set_device_privacy_flags_all(adapter);
}

static void set_llp_power_on_cb(uint8_t status, uint16_t length,
				const void *param, void *user_data)
{
	DBusMessage *msg = user_data;
	DBusMessage *reply;
	struct btd_adapter *adapter;
	unsigned int id;
	uint8_t power_val;

	// Power on the controller regardless of the status
	adapter = btd_adapter_get_default();
	power_val = 1;

	id = mgmt_send(mgmt_if, MGMT_OP_SET_POWERED,
		       btd_adapter_get_index(adapter),
		       sizeof(power_val), &power_val,
		       set_device_privacy_cb, adapter, NULL);
	if (!id) {
		reply = btd_error_failed(msg, "Failed to power on.");
		if (!reply) {
			error("Failed to create dbus error reply message.");
			goto failed;
		}
	}

	if (status)
		reply = btd_error_failed(msg, mgmt_errstr(status));
	else
		reply = dbus_message_new_method_return(msg);

	if (reply == NULL) {
		error("Failed to create dbus reply message.");
		goto failed;
	}
	if (!g_dbus_send_message(btd_get_dbus_connection(), reply))
		error("D-Bus send failed.");

failed:
	dbus_message_unref(msg);
}

static void power_off_set_llp_cb(uint8_t status, uint16_t length,
				 const void *param, void *user_data,
				 uint8_t value)
{
	DBusMessage *msg = user_data;
	DBusMessage *reply;
	struct btd_adapter *adapter;
	unsigned int id;
	struct mgmt_cp_set_exp_feature cp;

	adapter = btd_adapter_get_default();
	memset(&cp, 0, sizeof(cp));
	memcpy(cp.uuid, ll_privacy_uuid, 16);
	cp.action = value;

	dbus_message_ref(msg);
	id = mgmt_send(mgmt_if, MGMT_OP_SET_EXP_FEATURE,
		       btd_adapter_get_index(adapter),
		       sizeof(cp), &cp, set_llp_power_on_cb, msg, NULL);
	if (!id) {
		reply = btd_error_failed(msg, "Failed to set LL privacy.");
		if (!reply) {
			error("Failed to create dbus error reply message.");
			goto failed;
		}
	}
	if (status)
		reply = btd_error_failed(msg, mgmt_errstr(status));
	else
		reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		error("Failed to create dbus reply message.");
		goto failed;
	}
	if (!g_dbus_send_message(btd_get_dbus_connection(), reply))
		error("D-Bus send failed.");

failed:
	dbus_message_unref(msg);
}

static void power_off_enable_llp_cb(uint8_t status, uint16_t length,
				    const void *param, void *user_data)
{
	power_off_set_llp_cb(status, length, param, user_data, 1);
}

static void power_off_disable_llp_cb(uint8_t status, uint16_t length,
				     const void *param, void *user_data)
{
	power_off_set_llp_cb(status, length, param, user_data, 0);
}

static DBusMessage *set_ll_privacy(DBusConnection *conn,
				   DBusMessage *msg, void *user_data)
{
	dbus_bool_t ll_privacy = false;
	struct btd_adapter *adapter;
	unsigned int id;
	bool old_ll_privacy;
	uint8_t power_val;
	bool power_status;

	if (!read_llprivacy_status_from_file(&old_ll_privacy))
		warn("Cannot open configure file for read.");

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &ll_privacy,
				   DBUS_TYPE_INVALID))
		return btd_error_invalid_args(msg);

	if (ll_privacy == old_ll_privacy) {
		info("LL Privacy status not changed: %u", ll_privacy);
		return dbus_message_new_method_return(msg);
	}

	// Write to file DEBUG_LL_PRIVACY_CONF_PATH
	if (!write_value_to_conf_file(ll_privacy)) {
		error("Cannot open configure file for write.");
		return btd_error_failed(msg, "File cannot open for write.");
	}

	info("Store LL Privacy status to file %u", ll_privacy);

	if (!mgmt_if)
		return btd_error_not_ready(msg);

	adapter = btd_adapter_get_default();
	if (!adapter) {
		error("No default adapter. Skip setting ll privacy.");
		return btd_error_no_such_adapter(msg);
	}
	power_status = btd_adapter_get_powered(adapter);

	if (power_status) {
		power_val = 0;
		dbus_message_ref(msg);
		if (ll_privacy)
			id = mgmt_send(mgmt_if, MGMT_OP_SET_POWERED,
				       btd_adapter_get_index(adapter),
				       sizeof(power_val), &power_val,
				       power_off_enable_llp_cb, msg, NULL);
		else
			id = mgmt_send(mgmt_if, MGMT_OP_SET_POWERED,
				       btd_adapter_get_index(adapter),
				       sizeof(power_val), &power_val,
				       power_off_disable_llp_cb, msg, NULL);
		if (!id)
			return btd_error_failed(msg, "Failed to power off.");
	} else {
		struct mgmt_cp_set_exp_feature cp;

		memset(&cp, 0, sizeof(cp));
		memcpy(cp.uuid, ll_privacy_uuid, 16);
		if (ll_privacy)
			cp.action = 1;
		else
			cp.action = 0;
		id = mgmt_send(mgmt_if, MGMT_OP_SET_EXP_FEATURE,
			       btd_adapter_get_index(adapter),
			       sizeof(cp), &cp, set_device_privacy_cb,
			       adapter, NULL);
		if (!id)
			return btd_error_failed(msg,
						"Failed to set LL privacy.");
	}
	return dbus_message_new_method_return(msg);
}

/* API for KPI audio metrics */
static DBusMessage *report_hfp_status(DBusConnection *conn, DBusMessage *msg,
				      void *user_data)
{
	struct debug_data *debug = user_data;
	DBusError err;
	dbus_bool_t status;
	int32_t sco_handle;

	dbus_error_init(&err);

	if (!dbus_message_get_args(msg, &err, DBUS_TYPE_BOOLEAN, &status,
				   DBUS_TYPE_INT32, &sco_handle,
				   DBUS_TYPE_INVALID)) {
		error("read params failed %s", err.message);
		dbus_error_free(&err);

		return btd_error_failed(msg, "Failed to read parameters");
	}

	metrics_audio_hfp_play_pause(status, sco_handle);

	return dbus_message_new_method_return(msg);
}

static const GDBusMethodTable debug_methods[] = {
	{ GDBUS_METHOD("SetLevels", GDBUS_ARGS({ "levels", "yy" }), NULL,
		       set_log_levels) },
	{ GDBUS_METHOD("SetQualityDebug", GDBUS_ARGS({ "quality_debug", "b" }),
		       NULL, set_quality_debug) },
	{ GDBUS_METHOD("SetQuality", GDBUS_ARGS({ "action", "y" }),
		       NULL, set_quality) },
	{ GDBUS_METHOD("SetLLPrivacy", GDBUS_ARGS({ "ll_privacy",
		       "b" }), NULL, set_ll_privacy) },
	{},
};

#define DEFINE_GET_DEBUG_PROPERTY(PROP)                                        \
	static gboolean property_get_debug_##PROP(                             \
		const GDBusPropertyTable *property, DBusMessageIter *iter,     \
		void *user_data)                                               \
	{                                                                      \
		struct debug_data *debug_data = user_data;                     \
		dbus_message_iter_append_basic(iter, DBUS_TYPE_BYTE,           \
					       &debug_data->PROP);             \
		return TRUE;                                                   \
	}

DEFINE_GET_DEBUG_PROPERTY(bluez);
DEFINE_GET_DEBUG_PROPERTY(kernel);

static const GDBusPropertyTable debug_properties[] = {
	{ DEBUG_BLUEZ_PROPERTY, "y", property_get_debug_bluez },
	{ DEBUG_KERNEL_PROPERTY, "y", property_get_debug_kernel },
	{}
};

static const GDBusMethodTable metrics_methods[] = {
	{ GDBUS_METHOD("ReportHfpStatus", GDBUS_ARGS({ "status", "bi" }), NULL,
		       report_hfp_status) },
	{},
};

static struct btd_adapter_driver chromium_driver = {
	.name	= "chromium",
	.probe	= NULL,
	.resume = NULL,
	.remove = NULL,
	.device_added = on_device_added,
	.device_removed = on_device_removed
};

static int chromium_init(void)
{
	DBusConnection *conn = btd_get_dbus_connection();

	DBG("");

	mgmt_if = mgmt_new_default();
	if (!mgmt_if)
		error("Failed to access management interface");
	else if (!mgmt_send(mgmt_if, MGMT_OP_READ_VERSION,
					MGMT_INDEX_NONE, 0, NULL,
					read_version_complete, NULL, NULL))
		error("Failed to read management version information");

	g_dbus_register_interface(conn, DBUS_PATH, DBUS_PLUGIN_INTERFACE, NULL,
					NULL, chromium_properties, NULL, NULL);

	service_id = btd_service_add_state_cb(service_cb, NULL);

	struct debug_data *ddata = g_new0(struct debug_data, 1);

	if (read_debug_levels_from_file(ddata))
		apply_debug_levels(ddata, ddata->bluez, ddata->kernel);
	else
		warn("Unable to read debug levels from file");

	/* Register debug interface*/
	if (!g_dbus_register_interface(conn, DEBUG_OBJECT_PATH, DEBUG_INTERFACE,
				       debug_methods, NULL, debug_properties,
				       ddata, g_free)) {
		error("Failed to register debug interface");
		g_free(ddata);
	}

	/* Register metrics interface*/
	if (!g_dbus_register_interface(conn, DEBUG_OBJECT_PATH,
				       METRICS_INTERFACE, metrics_methods,
				       NULL, NULL, NULL, NULL)) {
		error("Failed to register metrics interface");
	}

	return btd_register_adapter_driver(&chromium_driver);
}

static void chromium_exit(void)
{
	struct btd_device *dev;

	DBG("");

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				    DBUS_PATH, DBUS_PLUGIN_INTERFACE);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				    DEBUG_OBJECT_PATH, DEBUG_INTERFACE);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
				    DEBUG_OBJECT_PATH, METRICS_INTERFACE);

	mgmt_unref(mgmt_if);
	mgmt_if = NULL;

	btd_service_remove_state_cb(service_id);
	g_slist_free_full(retry_devices, destroy_retry_data);

	// Unregister the interface for known devices. This needs to be done
	// here because plugin is unregistered first before adapter is shut
	// down, therefore on_device_removed is not called via plugin.
	while (registered_devices) {
		dev = (struct btd_device *) g_slist_nth_data(registered_devices,
									0);
		on_device_removed(device_get_adapter(dev), dev);
	}

	btd_unregister_adapter_driver(&chromium_driver);
}

BLUETOOTH_PLUGIN_DEFINE(chromium, VERSION, BLUETOOTH_PLUGIN_PRIORITY_HIGH,
						chromium_init, chromium_exit)
