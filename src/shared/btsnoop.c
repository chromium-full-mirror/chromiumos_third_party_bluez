/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012-2014  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#include <bzlib.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#include "src/shared/btsnoop.h"
#include "src/shared/queue.h"
#include "src/shared/timeout.h"
#include "src/shared/util.h"

#define BTSNOOP_COMPRESS_FLUSH_PERIOD 10000	// 10 seconds

struct btsnoop_hdr {
	uint8_t		id[8];		/* Identification Pattern */
	uint32_t	version;	/* Version Number = 1 */
	uint32_t	type;		/* Datalink Type */
} __attribute__ ((packed));
#define BTSNOOP_HDR_SIZE (sizeof(struct btsnoop_hdr))

struct btsnoop_pkt {
	uint32_t	size;		/* Original Length */
	uint32_t	len;		/* Included Length */
	uint32_t	flags;		/* Packet Flags */
	uint32_t	drops;		/* Cumulative Drops */
	uint64_t	ts;		/* Timestamp microseconds */
	uint8_t		data[0];	/* Packet Data */
} __attribute__ ((packed));
#define BTSNOOP_PKT_SIZE (sizeof(struct btsnoop_pkt))

static const uint8_t btsnoop_id[] = { 0x62, 0x74, 0x73, 0x6e,
				      0x6f, 0x6f, 0x70, 0x00 };

static const uint32_t btsnoop_version = 1;

struct pklg_pkt {
	uint32_t	len;
	uint64_t	ts;
	uint8_t		type;
} __attribute__ ((packed));
#define PKLG_PKT_SIZE (sizeof(struct pklg_pkt))

struct btsnoop {
	int ref_count;
	int fd;
	unsigned long flags;
	uint32_t format;
	uint16_t index;
	bool aborted;
	bool pklg_format;
	bool pklg_v2;
	const char *path;
	size_t max_size;
	size_t cur_size;
	unsigned int max_count;
	unsigned int cur_count;
	bool compress;
	bool rotate;
	uint32_t file_size_limit;
	char *log_path;
	int flush_timer;
};

/*
 * The struct rotation_carryover exists for handling log file rotation, so that
 * any single file can be read separately, independent from the other log.
 *
 * An example of carryover data is Ctrl data.
 * Using btmon, the content of the data is presented like this:
 *      @ MGMT Open: bluetoothd (privileged) version 1.14     {0x0002} 0.985864
 *      @ MGMT Open: bluetoothd (privileged) version 1.14     {0x0001} 0.985865
 *      @ MGMT Open: btmon (privileged) version 1.14          {0x0003} 0.985894
 *      ...
 *      @ MGMT Close: bluetoothd                              {0x0001} 3.847409
 *
 * In each log file, the 'Open' data is required to be present before any other
 * MGMT data, otherwise instead of displaying MGMT event like this:
 *   @ MGMT Event: Device Found (0x0012) plen 37       {0x0003} [hci0] 0.226572
 *      LE Address: F9:21:9B:D6:77:1E (Static)
 *      RSSI: -71 dBm (0xb9)
 *      Flags: 0x00000000
 *      Data length: 23
 *      Appearance: Mouse (0x03c2)
 *      Flags: 0x04
 *        BR/EDR Not Supported
 *      16-bit Service UUIDs (complete): 1 entry
 *        Human Interface Device (0x1812)
 *      Name (complete): nRF5_Mouse
 * It will display something like this:
 *   @ Control Event: 0xffff                           {0x0003} [hci0] 0.226572
 *      12 00 1e 77 d6 9b 21 f9 02 b9 00 00 00 00 17 00  ...w..!.........
 *      03 19 c2 03 02 01 04 03 03 12 18 0b 09 6e 52 46  .............nRF
 *      35 5f 4d 6f 75 73 65                             5_Mouse
 *
 * Therefore, we need to keep track on each of 'Open' and 'Close' data, and
 * transfer the 'Open' data to the new log file when during log rotation.
 * Specifically, we shall maintain a list of 'Open' data, appending new 'Open'
 * data to the list and deleting 'Closed' data from the list.
 *
 * In order to be able to remove the data, we need to extract some information
 * that serves as an identification to the data. That is the purpose of the
 * carryover_ident struct. For example, Ctrl data can be identified by the
 * 'cookie' field, which is located on the first 4 bytes of data.
 */

enum rotation_carryover_type {
	CARRYOVER_CTRL,
	CARRYOVER_HCI_CONN,
	CARRYOVER_L2CAP_CONN,
};
typedef struct carryover_ident* (*create_ident_func) (const void *);
typedef bool (*match_ident_func) (const void *, const void *);
struct carryover_ident {
	enum rotation_carryover_type type;
	void *data;
	int len;
};
struct rotation_carryover {
	struct btsnoop_pkt pkt;
	struct carryover_ident *ident;
	void *data;
};

struct ident_ctrl {
	uint32_t cookie;
};
struct ident_hci_conn {
	uint16_t handle;
};
struct ident_l2cap_conn {
	uint16_t handle;
	uint16_t local_cid;
	uint16_t remote_cid;
};

/*
 * These are lists of carryovers. The reason we have two lists is to support
 * compression mode while log rotation is enabled.
 * Take a look at this example of event sequence:
 * 1) btmon logging is started with compression enabled
 * 2) log until compression buffer is full. Compress.
 * 3) write compression result to log #1.
 * 4) continue logging until compression buffer is full. Compress.
 * 5) However, this will cause log #1 to exceed its size limit. Therefore,
 *    write to log #2 instead.
 *
 * On rotation in (5), we need to write some carryovers on log #2. However, the
 * data that we should write is NOT the carryover state in the beginning of (5),
 * instead we should write the carryover state in the beginning of (4), because
 * that is what is contained in log #1.
 *
 * Therefore, we need two lists: One to keep track the latest state (5), and one
 * to keep track the state since last time we write (4). The former is used only
 * when log rotation is enabled, while the latter is used only when log rotation
 * is enabled AND compression is also enabled.
 */
struct queue *carryover_list;
struct queue *carryover_list_since_last_write;

const char carryover_marker_data[] = "=== END OF CARRYOVER SECTION ===";

/*
 * To guarantee that the compressed data will fit, COMPRESS_DST_MAX is 1% larger
 * than the COMPRESS_SRC_MAX, plus 600 bytes.
 */
#define COMPRESS_SRC_MAX 100000
#define COMPRESS_DST_MAX 101600
static size_t compress_src_size = 0;
static char compress_src[COMPRESS_SRC_MAX];
static char compress_dst[COMPRESS_DST_MAX];

static struct carryover_ident *create_ident_ctrl(const void *data)
{
	struct carryover_ident *ident = malloc(sizeof(struct carryover_ident));
	struct ident_ctrl *ident_data = malloc0(sizeof(struct ident_ctrl));

	ident_data->cookie = get_le32(data);
	ident->type = CARRYOVER_CTRL;
	ident->len = sizeof(*ident_data);
	ident->data = ident_data;
	return ident;
}

static struct carryover_ident *create_ident_hci_conn(const void *data,
							uint16_t handle)
{
	struct carryover_ident *ident = malloc(sizeof(struct carryover_ident));
	struct ident_hci_conn *ident_data =
					malloc0(sizeof(struct ident_hci_conn));

	ident_data->handle = handle;
	ident->type = CARRYOVER_HCI_CONN;
	ident->len = sizeof(*ident_data);
	ident->data = ident_data;
	return ident;
}

static struct carryover_ident *create_ident_hci_conn_ev(const void *data)
{
	return create_ident_hci_conn(data, get_le16(data + 3));
}

static struct carryover_ident *create_ident_hci_le_conn_ev(const void *data)
{
	return create_ident_hci_conn(data, get_le16(data + 4));
}

static struct carryover_ident *create_ident_hci_disconn_ev(const void *data)
{
	return create_ident_hci_conn(data, get_le16(data + 3));
}

static struct carryover_ident *create_ident_hci_disconn_cmd(const void *data)
{
	return create_ident_hci_conn(data, get_le16(data + 3));
}

static struct carryover_ident *create_ident_l2cap_conn(const void *data,
							uint16_t local_cid,
							uint16_t remote_cid)
{
	struct carryover_ident *ident = malloc(sizeof(struct carryover_ident));
	struct ident_l2cap_conn *ident_data =
				malloc0(sizeof(struct ident_l2cap_conn));
	// Handle offset is always zero.
	// The handle comes with flags in the first 4 bits - remove them.
	ident_data->handle = get_le16(data);
	ident_data->handle &= 0x0fff;

	// For connection request, we won't know either one of the CIDs.
	// Mark the unknown CID with zero (invalid).
	ident_data->local_cid = local_cid;
	ident_data->remote_cid = remote_cid;

	ident->type = CARRYOVER_L2CAP_CONN;
	ident->len = sizeof(*ident_data);
	ident->data = ident_data;
	return ident;
}

static struct carryover_ident *create_ident_l2cap_conn_req_tx(const void *data)
{
	return create_ident_l2cap_conn(data, get_le16(data + 14), 0);
}

static struct carryover_ident *create_ident_l2cap_conn_req_rx(const void *data)
{
	return create_ident_l2cap_conn(data, 0, get_le16(data + 14));
}

static struct carryover_ident *create_ident_l2cap_conn_rsp_tx(const void *data)
{
	return create_ident_l2cap_conn(data, get_le16(data + 12),
							get_le16(data + 14));
}

static struct carryover_ident *create_ident_l2cap_conn_rsp_rx(const void *data)
{
	return create_ident_l2cap_conn(data, get_le16(data + 14),
							get_le16(data + 12));
}

static struct carryover_ident *create_ident_l2cap_disconn_rsp_tx(
							const void *data)
{
	return create_ident_l2cap_conn(data, get_le16(data + 12),
							get_le16(data + 14));
}

static struct carryover_ident *create_ident_l2cap_disconn_rsp_rx(
							const void *data)
{
	return create_ident_l2cap_conn(data, get_le16(data + 14),
							get_le16(data + 12));
}

static bool match_ident_ctrl(const void *carry, const void *ident)
{
	const struct rotation_carryover *carryover = carry;

	if (carryover->ident->type != CARRYOVER_CTRL)
		return false;

	const struct ident_ctrl *a = carryover->ident->data;
	const struct ident_ctrl *b = ident;

	return a->cookie == b->cookie;
}

static bool match_ident_hci_disconn(const void *carry, const void *ident)
{
	const struct rotation_carryover *carryover = carry;
	const struct ident_hci_conn *b = ident;

	if (carryover->ident->type == CARRYOVER_HCI_CONN) {
		const struct ident_hci_conn *a = carryover->ident->data;

		return a->handle == b->handle;
	} else if (carryover->ident->type == CARRYOVER_L2CAP_CONN) {
		const struct ident_l2cap_conn *a = carryover->ident->data;

		return a->handle == b->handle;
	}

	return false;
}

static bool match_ident_l2cap_disconn(const void *carry, const void *ident)
{
	const struct rotation_carryover *carryover = carry;

	if (carryover->ident->type != CARRYOVER_L2CAP_CONN)
		return false;

	const struct ident_l2cap_conn *a = carryover->ident->data;
	const struct ident_l2cap_conn *b = ident;

	// Zero (invalid) CID never matches with anything
	return (a->local_cid == b->local_cid && b->local_cid != 0) ||
		(a->remote_cid == b->remote_cid && b->remote_cid != 0);
}

static bool match_ident_reset(const void *carry, const void *null)
{
	const struct rotation_carryover *carryover = carry;

	return carryover->ident->type != CARRYOVER_CTRL;
}

static void carryover_free_ident(struct carryover_ident *ident)
{
	free(ident->data);
	free(ident);
}

static void carryover_free_data(void *data)
{
	if (!data)
		return;

	struct rotation_carryover *carryover = data;

	free(carryover->data);
	carryover->data = NULL;
	carryover_free_ident(carryover->ident);
	carryover->ident = NULL;
}

static struct queue *carryover_append(struct queue *carryovers,
					struct btsnoop_pkt *pkt,
					struct carryover_ident *ident,
					const void *data)
{
	if (!data || !pkt || !ident)
		return carryovers;
	if (!carryovers)
		carryovers = queue_new();

	struct rotation_carryover *carryover =
				malloc(sizeof(struct rotation_carryover));
	if (!carryover)
		return carryovers;

	carryover->pkt = *pkt;
	carryover->ident = ident;

	uint16_t size = be32toh(pkt->size);
	carryover->data = malloc(size);
	if (!carryover->data) {
		free(carryover);
		return carryovers;
	}

	memcpy(carryover->data, data, size);
	queue_push_tail(carryovers, carryover);
	return carryovers;
}

static void carryover_create(struct btsnoop_pkt *pkt,
					create_ident_func create_func,
					const void *data)
{
	struct carryover_ident *ident = create_func(data);

	carryover_list = carryover_append(carryover_list, pkt, ident, data);
}

static void carryover_release(create_ident_func create_func,
					match_ident_func match_func,
					const void *data)
{
	if (create_func) {
		struct carryover_ident *ident = create_func(data);

		queue_remove_all(carryover_list, match_func, ident->data,
						carryover_free_data);
		carryover_free_ident(ident);
	} else {
		queue_remove_all(carryover_list, match_func, NULL,
						carryover_free_data);
	}
}

static void carryover_release_all(struct queue *carryovers)
{
	queue_destroy(carryovers, carryover_free_data);
}

static struct queue *carryover_copy_list(struct queue *from)
{
	struct queue *to = NULL;
	const struct queue_entry *entry;

	for (entry = queue_get_entries(from); entry; entry = entry->next)
	{
		struct rotation_carryover *from = entry->data;
		struct carryover_ident *ident =
					malloc(sizeof(struct carryover_ident));
		ident->type = from->ident->type;
		ident->len = from->ident->len;
		ident->data = malloc(ident->len);
		memcpy(ident->data, from->ident->data, ident->len);
		to = carryover_append(to, &from->pkt, ident, from->data);
	}

	return to;
}

static struct btsnoop *btsnoop_alloc()
{
	struct btsnoop *btsnoop = calloc(1, sizeof(*btsnoop));
	if (!btsnoop)
		return NULL;

	btsnoop->fd = -1;
	return btsnoop;
}

static void btsnoop_free(struct btsnoop *btsnoop)
{
	if (!btsnoop)
		return;

	if (btsnoop->fd >= 0)
		close(btsnoop->fd);
	if (btsnoop->log_path) {
		free(btsnoop->log_path);
		btsnoop->log_path = NULL;
	}
	if (btsnoop->rotate) {
		carryover_release_all(carryover_list);
		carryover_list = NULL;

		if (btsnoop->compress) {
			carryover_release_all(carryover_list_since_last_write);
			carryover_list_since_last_write = NULL;
		}
	}
	if (btsnoop->flush_timer) {
		timeout_remove(btsnoop->flush_timer);
		btsnoop->flush_timer = 0;
	}

	free(btsnoop);
	btsnoop = NULL;
	return;
}

static char *alloc_and_concat(const char *str1, const char *str2)
{
	size_t len_of_str1 = (str1 ? strlen(str1) : 0);
	size_t len_of_str2 = (str2 ? strlen(str2) : 0);
	char *result = calloc(len_of_str1 + len_of_str2 + 1, sizeof(char));
	if (!result)
		return NULL;

	if (len_of_str1)
		strcat(result, str1);
	if (len_of_str2)
		strcat(result, str2);
	return result;
}

static char *get_log_rotation_path(struct btsnoop *btsnoop)
{
	if (!btsnoop->log_path)
		return NULL;

	return alloc_and_concat(btsnoop->log_path, ".old");
}

static size_t btsnoop_compress(void)
{
	unsigned int compress_dst_size = COMPRESS_DST_MAX;

	BZ2_bzBuffToBuffCompress(compress_dst, &compress_dst_size,
			compress_src, compress_src_size, 1, 0, 0);
	compress_src_size = 0;
	return compress_dst_size;
}

static void btsnoop_append_to_compress(const void *data, size_t size)
{
	memcpy(compress_src + compress_src_size, data, size);
	compress_src_size += size;
}

static ssize_t write_header_and_carryovers(struct btsnoop *btsnoop)
{
	if (!btsnoop)
		return -EINVAL;

	struct btsnoop_hdr hdr;
	memcpy(hdr.id, btsnoop_id, sizeof(btsnoop_id));
	hdr.version = htobe32(btsnoop_version);
	hdr.type = htobe32(btsnoop->format);

	const struct queue_entry *entry;
	struct queue *list = btsnoop->compress ?
			carryover_list_since_last_write : carryover_list;
	size_t header_carryover_total_size = BTSNOOP_HDR_SIZE;

	struct btsnoop_pkt carryover_marker_pkt = {
		.size = htobe32(sizeof(carryover_marker_data)),
		.len = htobe32(sizeof(carryover_marker_data)),
		// flags = (adapterId: invalid, opcode: 12 (system note))
		.flags = htobe32(0xffff000C),
		.drops = 0,
		.ts = 0,
	};

	for (entry = queue_get_entries(list); entry; entry = entry->next) {
		const struct rotation_carryover *carryover = entry->data;
		size_t pkt_size = be32toh(carryover->pkt.size);
		// always update ts to latest, otherwise it messes up display.
		carryover_marker_pkt.ts = carryover->pkt.ts;

		header_carryover_total_size += BTSNOOP_PKT_SIZE + pkt_size;
	}

	if (!queue_isempty(list)) {
		header_carryover_total_size +=
			BTSNOOP_PKT_SIZE + sizeof(carryover_marker_data);
	}

	/* copy file header and carryover packets to buffer */
	void *buffer = malloc(header_carryover_total_size);
	if (!buffer)
		return -ENOMEM;
	memcpy(buffer, &hdr, BTSNOOP_HDR_SIZE);

	size_t offset = BTSNOOP_HDR_SIZE;
	for (entry = queue_get_entries(list); entry; entry = entry->next) {
		const struct rotation_carryover *carryover = entry->data;
		size_t pkt_size = be32toh(carryover->pkt.size);

		memcpy(buffer + offset, &carryover->pkt, BTSNOOP_PKT_SIZE);
		memcpy(buffer + offset + BTSNOOP_PKT_SIZE, carryover->data,
								pkt_size);
		offset += BTSNOOP_PKT_SIZE + pkt_size;
	}

	if (!queue_isempty(list)) {
		memcpy(buffer + offset, &carryover_marker_pkt,
							BTSNOOP_PKT_SIZE);
		memcpy(buffer + offset + BTSNOOP_PKT_SIZE,
			carryover_marker_data, sizeof(carryover_marker_data));
	}

	ssize_t written;
	if (btsnoop->compress) {
		size_t compressed_size;

		btsnoop_append_to_compress(buffer, header_carryover_total_size);
		compressed_size = btsnoop_compress();
		written = write(btsnoop->fd, compress_dst, compressed_size);
	} else {
		written = write(btsnoop->fd, buffer,
						header_carryover_total_size);
	}
	free(buffer);

	return written;
}

bool btsnoop_rotate_logs(struct btsnoop *btsnoop)
{
	if (close(btsnoop->fd) != 0)
		return false;
	btsnoop->fd = -1;

	char *log_rotation_path = get_log_rotation_path(btsnoop);
	int rename_result = rename(btsnoop->log_path, log_rotation_path);
	free(log_rotation_path);

	if (rename_result != 0)
		return false;

	btsnoop->fd = open(btsnoop->log_path,
				O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
				S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (btsnoop->fd < 0)
		return false;

	return true;
}

static ssize_t btsnoop_write_to_log(struct btsnoop *btsnoop, const void *data,
								size_t size)
{
	struct stat st;
	int fd = btsnoop->fd;
	ssize_t written = 0;

	if (size == 0)
		return 0;

	/* if no size limit is specified, skip these several checks */
	if (btsnoop->file_size_limit <= 0)
		goto check_file_size_limit_done;

	if (fstat(fd, &st) < 0)
		return -errno;

	if ((uint32_t)st.st_size + size >= btsnoop->file_size_limit) {
		if (!btsnoop->rotate)
			return -ENOSPC;

		if (!btsnoop_rotate_logs(btsnoop))
			return -errno;

		written = write_header_and_carryovers(btsnoop);
		if (written < 0)
			return written;
	}

check_file_size_limit_done:
	written += write(fd, data, size);

	if (btsnoop->rotate && btsnoop->compress) {
		carryover_release_all(carryover_list_since_last_write);
		carryover_list_since_last_write =
					carryover_copy_list(carryover_list);
	}

	return written;
}

ssize_t btsnoop_flush_compression_buffer(struct btsnoop *btsnoop)
{
	size_t compressed_size;
	ssize_t result;
	void *temp;

	if (compress_src_size == 0)
		return 0;

	compressed_size = btsnoop_compress();
	temp = malloc(compressed_size);

	if (!temp)
		return -ENOMEM;

	memcpy(temp, compress_dst, compressed_size);
	result = btsnoop_write_to_log(btsnoop, temp, compressed_size);
	free(temp);
	return result;
}

/*
 * If successful, return the size of bytes written to file (0 is possible when
 * the data is only written into the compression buffer and not to file).
 * Otherwise, return a negative number indicating the error.
 */
static ssize_t write_and_possibly_compress(struct btsnoop *btsnoop,
						const void *data, size_t size)
{
	ssize_t written = 0;

	if (!btsnoop->compress)
		return btsnoop_write_to_log(btsnoop, data, size);

	if (compress_src_size + size > COMPRESS_SRC_MAX)
		written = btsnoop_flush_compression_buffer(btsnoop);

	btsnoop_append_to_compress(data, size);
	return written;
}

static bool flush_timeout(void *user_data)
{
	struct btsnoop *btsnoop = user_data;

	btsnoop_flush_compression_buffer(btsnoop);
	return true;
}

struct btsnoop *btsnoop_open(const char *path, unsigned long flags)
{
	struct btsnoop *btsnoop;
	struct btsnoop_hdr hdr;
	ssize_t len;

	btsnoop = btsnoop_alloc();
	if (!btsnoop)
		return NULL;

	btsnoop->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (btsnoop->fd < 0) {
		btsnoop_free(btsnoop);
		btsnoop = NULL;
		return NULL;
	}

	btsnoop->flags = flags;

	len = read(btsnoop->fd, &hdr, BTSNOOP_HDR_SIZE);
	if (len < 0 || len != BTSNOOP_HDR_SIZE)
		goto failed;

	if (!memcmp(hdr.id, btsnoop_id, sizeof(btsnoop_id))) {
		/* Check for BTSnoop version 1 format */
		if (be32toh(hdr.version) != btsnoop_version)
			goto failed;

		btsnoop->format = be32toh(hdr.type);
		btsnoop->index = 0xffff;
	} else {
		if (!(btsnoop->flags & BTSNOOP_FLAG_PKLG_SUPPORT))
			goto failed;

		/* Check for Apple Packet Logger format */
		if (hdr.id[0] != 0x00 ||
				(hdr.id[1] != 0x00 && hdr.id[1] != 0x01))
			goto failed;

		btsnoop->format = BTSNOOP_FORMAT_MONITOR;
		btsnoop->index = 0xffff;
		btsnoop->pklg_format = true;
		btsnoop->pklg_v2 = (hdr.id[1] == 0x01);

		/* Apple Packet Logger format has no header */
		lseek(btsnoop->fd, 0, SEEK_SET);
	}

	return btsnoop_ref(btsnoop);

failed:
	btsnoop_free(btsnoop);
	btsnoop = NULL;
	return NULL;
}

struct btsnoop *btsnoop_create(const char *path, size_t max_size,
				unsigned int max_count, uint32_t format,
				bool compress, unsigned int file_size_limit,
				bool rotate)
{
	struct btsnoop *btsnoop;
	const char *real_path;
	char tmp[PATH_MAX];
	ssize_t written;

	if (!max_size && max_count)
		return NULL;

	btsnoop = btsnoop_alloc();
	if (!btsnoop)
		return NULL;

	/* If max file size is specified, always add counter to file path */
	if (max_size) {
		snprintf(tmp, PATH_MAX, "%s.0", path);
		real_path = tmp;
	} else {
		real_path = path;
	}

	btsnoop->fd = open(real_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
									0644);
	if (btsnoop->fd < 0)
		goto failed;

	btsnoop->log_path = strdup(path);
	if (!btsnoop->log_path)
		goto failed;

	btsnoop->format = format;
	btsnoop->index = 0xffff;
	btsnoop->path = path;
	btsnoop->max_count = max_count;
	btsnoop->max_size = max_size;
	btsnoop->compress = compress;
	btsnoop->file_size_limit = file_size_limit;
	btsnoop->rotate = rotate;

	written = write_header_and_carryovers(btsnoop);
	if (written < 0)
		goto failed;

	btsnoop->cur_size = BTSNOOP_HDR_SIZE;

	if (compress)
		btsnoop->flush_timer = timeout_add(
						BTSNOOP_COMPRESS_FLUSH_PERIOD,
						flush_timeout, btsnoop, NULL);

	return btsnoop_ref(btsnoop);

failed:
	btsnoop_free(btsnoop);
	btsnoop = NULL;
	return NULL;
}

struct btsnoop *btsnoop_ref(struct btsnoop *btsnoop)
{
	if (!btsnoop)
		return NULL;

	__sync_fetch_and_add(&btsnoop->ref_count, 1);

	return btsnoop;
}

void btsnoop_unref(struct btsnoop *btsnoop)
{
	if (!btsnoop)
		return;

	if (__sync_sub_and_fetch(&btsnoop->ref_count, 1))
		return;

	btsnoop_free(btsnoop);
	btsnoop = NULL;
}

uint32_t btsnoop_get_format(struct btsnoop *btsnoop)
{
	if (!btsnoop)
		return BTSNOOP_FORMAT_INVALID;

	return btsnoop->format;
}

static bool btsnoop_rotate(struct btsnoop *btsnoop)
{
	struct btsnoop_hdr hdr;
	char path[PATH_MAX];
	ssize_t written;

	close(btsnoop->fd);

	/* Check if max number of log files has been reached */
	if (btsnoop->max_count && btsnoop->cur_count >= btsnoop->max_count) {
		snprintf(path, PATH_MAX, "%s.%u", btsnoop->path,
				btsnoop->cur_count - btsnoop->max_count);
		unlink(path);
	}

	snprintf(path, PATH_MAX,"%s.%u", btsnoop->path, btsnoop->cur_count);
	btsnoop->cur_count++;

	btsnoop->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
									0644);
	if (btsnoop->fd < 0)
		return false;

	memcpy(hdr.id, btsnoop_id, sizeof(btsnoop_id));
	hdr.version = htobe32(btsnoop_version);
	hdr.type = htobe32(btsnoop->format);

	written = write(btsnoop->fd, &hdr, BTSNOOP_HDR_SIZE);
	if (written < 0)
		return false;

	btsnoop->cur_size = BTSNOOP_HDR_SIZE;

	return true;
}

struct btsnoop_pkt create_btsnoop_pkt(struct timeval *tv, uint32_t flags,
						uint32_t drops, uint16_t size)
{
	struct btsnoop_pkt pkt;
	uint64_t ts = (tv->tv_sec - 946684800ll) * 1000000ll + tv->tv_usec;

	pkt.size  = htobe32(size);
	pkt.len   = htobe32(size);
	pkt.flags = htobe32(flags);
	pkt.drops = htobe32(drops);
	pkt.ts    = htobe64(ts + 0x00E03AB44A676000ll);

	return pkt;
}

bool btsnoop_write_pkt(struct btsnoop *btsnoop, struct btsnoop_pkt *pkt,
							const void *data)
{
	if (!btsnoop || !pkt)
		return false;

	/* allocate buffer to write pkt header and data at once */
	uint16_t size = be32toh(pkt->size);
	void *write_buffer = malloc(BTSNOOP_PKT_SIZE + size);
	if (!write_buffer)
		return false;

	memcpy(write_buffer, pkt, BTSNOOP_PKT_SIZE);
	if (data && size > 0)
		memcpy(write_buffer + BTSNOOP_PKT_SIZE, data, size);

	btsnoop->cur_size += BTSNOOP_PKT_SIZE;
	btsnoop->cur_size += size;

	ssize_t written = write_and_possibly_compress(btsnoop, write_buffer,
						BTSNOOP_PKT_SIZE + size);
	free(write_buffer);

	return written >= 0;
}

bool btsnoop_write(struct btsnoop *btsnoop, struct timeval *tv,
			uint32_t flags, uint32_t drops, const void *data,
			uint16_t size)
{
	if (!btsnoop || !tv)
		return false;

	struct btsnoop_pkt pkt = create_btsnoop_pkt(tv, flags, drops, size);
	return btsnoop_write_pkt(btsnoop, &pkt, data);
}

static uint32_t get_flags_from_opcode(uint16_t opcode)
{
	switch (opcode) {
	case BTSNOOP_OPCODE_NEW_INDEX:
	case BTSNOOP_OPCODE_DEL_INDEX:
		break;
	case BTSNOOP_OPCODE_COMMAND_PKT:
		return 0x02;
	case BTSNOOP_OPCODE_EVENT_PKT:
		return 0x03;
	case BTSNOOP_OPCODE_ACL_TX_PKT:
		return 0x00;
	case BTSNOOP_OPCODE_ACL_RX_PKT:
		return 0x01;
	case BTSNOOP_OPCODE_SCO_TX_PKT:
	case BTSNOOP_OPCODE_SCO_RX_PKT:
		break;
	case BTSNOOP_OPCODE_OPEN_INDEX:
	case BTSNOOP_OPCODE_CLOSE_INDEX:
		break;
	}

	return 0xff;
}

static void carryover_process_hci_cmd(struct btsnoop_pkt *pkt, const void *data)
{
	if (pkt->size < 2)
		return;

	switch (get_le16(data)) { // opcode
	case 0x0c03: // Reset
		carryover_release(NULL, match_ident_reset, NULL);
		break;
	case 0x0406: // Disconnect
		// Disconnect cmd is needed on top of disconn ev because
		// sometimes the controller doesn't wait for the event.
		if (pkt->size < 5)
			return;
		carryover_create(pkt, create_ident_hci_disconn_cmd, data);
		break;
	}
}

static void carryover_process_hci_ev(struct btsnoop_pkt *pkt, const void *data)
{
	if (pkt->size < 1)
		return;

	switch (get_u8(data)) { // ev code
	case 0x03: // Connection Complete
	case 0x2c: // Synchronous Connection Complete
		if (pkt->size < 5)
			return;
		if (get_u8(data + 2) == 0x00) // status
			carryover_create(pkt, create_ident_hci_conn_ev, data);
		break;
	case 0x05: // Disconnection Complete
		if (pkt->size < 5)
			return;
		carryover_release(create_ident_hci_disconn_ev,
						match_ident_hci_disconn, data);
		break;
	case 0x3e: // LE Meta
		if (pkt->size < 6)
			return;
		switch (get_u8(data + 2)) { // subevent code
		case 0x01: // LE Connection Complete
		case 0x0a: // LE Enhanced Connection Complete v1
		case 0x29: // LE Enhanced Connection Complete v2
			if (get_u8(data + 3) == 0x00) { // status
				carryover_create(pkt,
					create_ident_hci_le_conn_ev, data);
			}
			break;
		}
		break;
	}
}

static void carryover_process_acl(struct btsnoop_pkt *pkt, const void *data,
								bool is_tx)
{
	if (pkt->size < 9)
		return;

	// Only interested in L2CAP signalling channel
	if (get_le16(data + 6) != 0x0001)
		return;

	create_ident_func create_func;

	switch (get_u8(data + 8)) { // Command code
	case 0x02: // Connection Request
		if (pkt->size < 16)
			return;
		create_func = is_tx ? create_ident_l2cap_conn_req_tx :
					create_ident_l2cap_conn_req_rx;
		carryover_create(pkt, create_func, data);
		break;
	case 0x03: // Connection Response
		if (pkt->size < 18)
			return;
		switch (get_le16(data + 16)) { // result
		case 0x0000: // success
			create_func = is_tx ? create_ident_l2cap_conn_rsp_tx :
						create_ident_l2cap_conn_rsp_rx;
			carryover_create(pkt, create_func, data);
			break;
		case 0x0001: // pending
			break;
		default: // error
			create_func = is_tx ? create_ident_l2cap_conn_rsp_tx :
						create_ident_l2cap_conn_rsp_rx;
			carryover_release(create_func,
					match_ident_l2cap_disconn, data);
			break;
		}
		break;
	case 0x07: // Disconnection Response
		if (pkt->size < 16)
			return;
		create_func = is_tx ? create_ident_l2cap_disconn_rsp_tx :
					create_ident_l2cap_disconn_rsp_rx;
		carryover_release(create_func, match_ident_l2cap_disconn, data);
		break;
	}
}

bool btsnoop_write_hci(struct btsnoop *btsnoop, struct timeval *tv,
			uint16_t index, uint16_t opcode, uint32_t drops,
			const void *data, uint16_t size)
{
	uint32_t flags;

	if (!btsnoop || !tv)
		return false;

	switch (btsnoop->format) {
	case BTSNOOP_FORMAT_HCI:
		if (btsnoop->index == 0xffff)
			btsnoop->index = index;

		if (index != btsnoop->index)
			return false;

		flags = get_flags_from_opcode(opcode);
		if (flags == 0xff)
			return false;
		break;

	case BTSNOOP_FORMAT_MONITOR:
		flags = (index << 16) | opcode;
		break;

	default:
		return false;
	}

	struct btsnoop_pkt pkt = create_btsnoop_pkt(tv, flags, drops, size);
	bool result = btsnoop_write_pkt(btsnoop, &pkt, data);

	if (!btsnoop->rotate)
		return result;

	// Here we update the state for btsnoop rotation.
	switch (opcode) {
	case BTSNOOP_OPCODE_CTRL_OPEN:
		if (pkt.size >= 4)
			carryover_create(&pkt, create_ident_ctrl, data);
		break;
	case BTSNOOP_OPCODE_CTRL_CLOSE:
		if (pkt.size >= 4) {
			carryover_release(create_ident_ctrl, match_ident_ctrl,
									data);
		}
		break;
	case BTSNOOP_OPCODE_COMMAND_PKT:
		carryover_process_hci_cmd(&pkt, data);
		break;
	case BTSNOOP_OPCODE_EVENT_PKT:
		carryover_process_hci_ev(&pkt, data);
		break;
	case BTSNOOP_OPCODE_ACL_TX_PKT:
		carryover_process_acl(&pkt, data, true);
		break;
	case BTSNOOP_OPCODE_ACL_RX_PKT:
		carryover_process_acl(&pkt, data, false);
		break;
	}

	return result;
}

bool btsnoop_write_phy(struct btsnoop *btsnoop, struct timeval *tv,
			uint16_t frequency, const void *data, uint16_t size)
{
	uint32_t flags;

	if (!btsnoop)
		return false;

	switch (btsnoop->format) {
	case BTSNOOP_FORMAT_SIMULATOR:
		flags = (1 << 16) | frequency;
		break;

	default:
		return false;
	}

	return btsnoop_write(btsnoop, tv, flags, 0, data, size);
}

static bool pklg_read_hci(struct btsnoop *btsnoop, struct timeval *tv,
					uint16_t *index, uint16_t *opcode,
					void *data, uint16_t *size)
{
	struct pklg_pkt pkt;
	uint32_t toread;
	uint64_t ts;
	ssize_t len;

	len = read(btsnoop->fd, &pkt, PKLG_PKT_SIZE);
	if (len == 0)
		return false;

	if (len < 0 || len != PKLG_PKT_SIZE) {
		btsnoop->aborted = true;
		return false;
	}

	if (btsnoop->pklg_v2) {
		toread = le32toh(pkt.len) - (PKLG_PKT_SIZE - 4);

		ts = le64toh(pkt.ts);
		tv->tv_sec = ts & 0xffffffff;
		tv->tv_usec = ts >> 32;
	} else {
		toread = be32toh(pkt.len) - (PKLG_PKT_SIZE - 4);

		ts = be64toh(pkt.ts);
		tv->tv_sec = ts >> 32;
		tv->tv_usec = ts & 0xffffffff;
	}

	if (toread > BTSNOOP_MAX_PACKET_SIZE) {
                btsnoop->aborted = true;
                return false;
        }

	switch (pkt.type) {
	case 0x00:
		*index = 0x0000;
		*opcode = BTSNOOP_OPCODE_COMMAND_PKT;
		break;
	case 0x01:
		*index = 0x0000;
		*opcode = BTSNOOP_OPCODE_EVENT_PKT;
		break;
	case 0x02:
		*index = 0x0000;
		*opcode = BTSNOOP_OPCODE_ACL_TX_PKT;
		break;
	case 0x03:
		*index = 0x0000;
		*opcode = BTSNOOP_OPCODE_ACL_RX_PKT;
		break;
	case 0x08:
		*index = 0x0000;
		*opcode = BTSNOOP_OPCODE_SCO_TX_PKT;
		break;
	case 0x09:
		*index = 0x0000;
		*opcode = BTSNOOP_OPCODE_SCO_RX_PKT;
		break;
	case 0x0b:
		*index = 0x0000;
		*opcode = BTSNOOP_OPCODE_VENDOR_DIAG;
		break;
	case 0xfc:
		*index = 0xffff;
		*opcode = BTSNOOP_OPCODE_SYSTEM_NOTE;
		break;
	default:
		*index = 0xffff;
		*opcode = 0xffff;
		break;
	}

	len = read(btsnoop->fd, data, toread);
	if (len < 0) {
		btsnoop->aborted = true;
		return false;
	}

	*size = toread;

	return true;
}

static uint16_t get_opcode_from_flags(uint8_t type, uint32_t flags)
{
	switch (type) {
	case 0x01:
		return BTSNOOP_OPCODE_COMMAND_PKT;
	case 0x02:
		if (flags & 0x01)
			return BTSNOOP_OPCODE_ACL_RX_PKT;
		else
			return BTSNOOP_OPCODE_ACL_TX_PKT;
	case 0x03:
		if (flags & 0x01)
			return BTSNOOP_OPCODE_SCO_RX_PKT;
		else
			return BTSNOOP_OPCODE_SCO_TX_PKT;
	case 0x04:
		return BTSNOOP_OPCODE_EVENT_PKT;
	case 0xff:
		if (flags & 0x02) {
			if (flags & 0x01)
				return BTSNOOP_OPCODE_EVENT_PKT;
			else
				return BTSNOOP_OPCODE_COMMAND_PKT;
		} else {
			if (flags & 0x01)
				return BTSNOOP_OPCODE_ACL_RX_PKT;
			else
				return BTSNOOP_OPCODE_ACL_TX_PKT;
		}
		break;
	}

	return 0xffff;
}

bool btsnoop_read_hci(struct btsnoop *btsnoop, struct timeval *tv,
					uint16_t *index, uint16_t *opcode,
					void *data, uint16_t *size)
{
	struct btsnoop_pkt pkt;
	uint32_t toread, flags;
	uint64_t ts;
	uint8_t pkt_type;
	ssize_t len;

	if (!btsnoop || btsnoop->aborted)
		return false;

	if (btsnoop->pklg_format)
		return pklg_read_hci(btsnoop, tv, index, opcode, data, size);

	len = read(btsnoop->fd, &pkt, BTSNOOP_PKT_SIZE);
	if (len == 0)
		return false;

	if (len < 0 || len != BTSNOOP_PKT_SIZE) {
		btsnoop->aborted = true;
		return false;
	}

	toread = be32toh(pkt.size);
	if (toread > BTSNOOP_MAX_PACKET_SIZE) {
		btsnoop->aborted = true;
		return false;
	}

	flags = be32toh(pkt.flags);

	ts = be64toh(pkt.ts) - 0x00E03AB44A676000ll;
	tv->tv_sec = (ts / 1000000ll) + 946684800ll;
	tv->tv_usec = ts % 1000000ll;

	switch (btsnoop->format) {
	case BTSNOOP_FORMAT_HCI:
		*index = 0;
		*opcode = get_opcode_from_flags(0xff, flags);
		break;

	case BTSNOOP_FORMAT_UART:
		len = read(btsnoop->fd, &pkt_type, 1);
		if (len < 0) {
			btsnoop->aborted = true;
			return false;
		}
		toread--;

		*index = 0;
		*opcode = get_opcode_from_flags(pkt_type, flags);
		break;

	case BTSNOOP_FORMAT_MONITOR:
		*index = flags >> 16;
		*opcode = flags & 0xffff;
		break;

	default:
		btsnoop->aborted = true;
		return false;
	}

	len = read(btsnoop->fd, data, toread);
	if (len < 0) {
		btsnoop->aborted = true;
		return false;
	}

	*size = toread;

	return true;
}

bool btsnoop_read_phy(struct btsnoop *btsnoop, struct timeval *tv,
			uint16_t *frequency, void *data, uint16_t *size)
{
	return false;
}
