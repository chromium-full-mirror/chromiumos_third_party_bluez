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
};

/*
 * To guarantee that the compressed data will fit, COMPRESS_DST_MAX is 1% larger
 * than the COMPRESS_SRC_MAX, plus 600 bytes.
 */
#define COMPRESS_SRC_MAX 100000
#define COMPRESS_DST_MAX 101600
static size_t compress_src_size = 0;
static char compress_src[COMPRESS_SRC_MAX];
static char compress_dst[COMPRESS_DST_MAX];

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

static size_t btsnoop_compress(const void *data, size_t size)
{
	size_t written = 0;
	if (compress_src_size + size > COMPRESS_SRC_MAX) {
		unsigned int compress_dst_size = COMPRESS_DST_MAX;
		BZ2_bzBuffToBuffCompress(compress_dst, &compress_dst_size,
				compress_src, compress_src_size, 1, 0, 0);
		compress_src_size = 0;
		written = compress_dst_size;
	}
	memcpy(compress_src + compress_src_size, data, size);
	compress_src_size += size;
	return written;
}

static ssize_t write_header(struct btsnoop *btsnoop)
{
	struct btsnoop_hdr hdr;

	if (!btsnoop)
		return -EINVAL;

	memcpy(hdr.id, btsnoop_id, sizeof(btsnoop_id));
	hdr.version = htobe32(btsnoop_version);
	hdr.type = htobe32(btsnoop->format);

	if (btsnoop->compress) {
		/* reuse compress_src, temporarily store whatever data there */
		void *src_copy = malloc(compress_src_size);
		if (!src_copy)
			return -ENOMEM;

		memcpy(src_copy, compress_src, compress_src_size);

		unsigned int compressed_size;
		memcpy(compress_src, &hdr, BTSNOOP_HDR_SIZE);
		BZ2_bzBuffToBuffCompress(compress_dst, &compressed_size,
				compress_src, BTSNOOP_HDR_SIZE, 1, 0, 0);

		memcpy(compress_src, src_copy, compress_src_size);
		free(src_copy);
		return write(btsnoop->fd, compress_dst, compressed_size);
	} else {
		return write(btsnoop->fd, &hdr, BTSNOOP_HDR_SIZE);
	}
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

/*
 * If successful, return the size of bytes written to file (0 is possible when
 * the data is only written into the compression buffer and not to file).
 * Otherwise, return a negative number indicating the error.
 */
static ssize_t write_and_possibly_compress(struct btsnoop *btsnoop,
						const void *data, size_t size)
{
	struct stat st;
	int fd = btsnoop->fd;
	void *compressed_data = NULL;
	ssize_t written = 0;
	ssize_t return_value;

	if (fd < 0)
		return -errno;

	if (btsnoop->compress) {
		size_t compressed_size = btsnoop_compress(data, size);
		if (compressed_size == 0)
			return 0;

		compressed_data = malloc(compressed_size);
		if (!compressed_data)
			return -ENOMEM;

		memcpy(compressed_data, compress_dst, compressed_size);
		data = compressed_data;
		size = compressed_size;
	}

	/* if no size limit is specified, skip these several checks */
	if (btsnoop->file_size_limit <= 0)
		goto check_file_size_limit_done;

	if (fstat(fd, &st) < 0) {
		return_value = -errno;
		goto clean_up;
	}

	if ((uint32_t)st.st_size + size >= btsnoop->file_size_limit) {
		if (!btsnoop->rotate) {
			return_value = -ENOSPC;
			goto clean_up;
		}

		if (!btsnoop_rotate_logs(btsnoop)) {
			return_value = -errno;
			goto clean_up;
		}

		written = write_header(btsnoop);
		if (written < 0) {
			return_value = written;
			goto clean_up;
		}
	}

check_file_size_limit_done:
	written += write(fd, data, size);
	return_value = written;

clean_up:
	if (compressed_data)
		free(compressed_data);

	return return_value;
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
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
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

	written = write_header(btsnoop);
	if (written < 0)
		goto failed;

	btsnoop->cur_size = BTSNOOP_HDR_SIZE;

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
					S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
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

bool btsnoop_write(struct btsnoop *btsnoop, struct timeval *tv,
			uint32_t flags, uint32_t drops, const void *data,
			uint16_t size)
{
	struct btsnoop_pkt pkt;
	uint64_t ts;
	ssize_t written;
	void *write_buffer;

	if (!btsnoop || !tv)
		return false;

	if (btsnoop->max_size && btsnoop->max_size <=
			btsnoop->cur_size + size + BTSNOOP_PKT_SIZE)
		if (!btsnoop_rotate(btsnoop))
			return false;

	ts = (tv->tv_sec - 946684800ll) * 1000000ll + tv->tv_usec;

	pkt.size  = htobe32(size);
	pkt.len   = htobe32(size);
	pkt.flags = htobe32(flags);
	pkt.drops = htobe32(drops);
	pkt.ts    = htobe64(ts + 0x00E03AB44A676000ll);

	/* allocate buffer to write pkt header and data at once */
	write_buffer = malloc(BTSNOOP_PKT_SIZE + size);
	memcpy(write_buffer, &pkt, BTSNOOP_PKT_SIZE);
	memcpy(write_buffer + BTSNOOP_PKT_SIZE, data, size);
	btsnoop->cur_size += BTSNOOP_PKT_SIZE;
	btsnoop->cur_size += size;
	written = write_and_possibly_compress(btsnoop, write_buffer,
						BTSNOOP_PKT_SIZE + size);
	free(write_buffer);

	return written >= 0;
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

bool btsnoop_write_hci(struct btsnoop *btsnoop, struct timeval *tv,
			uint16_t index, uint16_t opcode, uint32_t drops,
			const void *data, uint16_t size)
{
	uint32_t flags;

	if (!btsnoop)
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

	return btsnoop_write(btsnoop, tv, flags, drops, data, size);
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
