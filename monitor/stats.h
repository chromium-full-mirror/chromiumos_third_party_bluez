/* SPDX-License-Identifier: LGPL-2.0-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Google LLC
 *
 *
 */

#ifndef __STATS_H
#define __STATS_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) > (b) ? (a) : (b))

/* MODE_PACKET is used in cases of "btmon -r" or "btmon -w" */
#define MODE_PACKET		0
/* MODE_ANALYZE is used in case of "btmon -a" */
#define MODE_ANALYZE		1

/* All of the Android BQR subevents are of integer types. */
enum int_types {
	INT8,
	INT16,
	INT32,
	UINT8,
	UINT16,
	UINT32,

	/* Total number of integer types. This should be the last entry. */
	TOTAL_INT_TYPES
};

/*
 * The subevt_stat struct keeps a range of values of a particular subevent
 * over which the statistics are calculated.
 *
 * value: points to a cyclic buffer which holds up to |bqr_buffer_len| entries.
 *        The buffer type is determined per subevent at run time.
 * int_type: the integer type of a subevt, e.g., INT8 or UINT32
 *
 * The integer types of subevents are defined in bqr_subevt_table.
 */
struct subevt_stat_slot {
	void *value;
	void *min;
	void *max;
	double sum;
	double mean;
	double sd;
};

struct subevt_stat {
	enum int_types int_type;
	size_t subevt_size;
	unsigned char num_slots;
	struct subevt_stat_slot *slots;
};

/* This struct keeps all bqr stats data associated with a connection handle. */
struct bqr_stats_data {
	uint16_t index;
	uint16_t conn_handle;
	unsigned int total_count;   /* number of total bqr events */
	unsigned int num_entries;   /* number of valid entries in stats */
	unsigned int next;
	const uint8_t *bqr_stats_subevt_list; /* statistics are computed over */
					      /* this list of subevts */
	uint8_t subevt_list_count; /* count of subevts in the list above */
	struct subevt_stat *stats; /* buffers and statistics for subevents */
	struct timeval *tv;	/* packet timestamps of stats' value buffers */
	const char *(*get_subevt_name)(uint8_t subevent_id);
	uint8_t (*get_subevt_num_slots)(uint8_t subevent_id);
	size_t (*get_subevt_size)(uint8_t subevent_id);
	enum int_types (*get_subevt_int_type)(uint8_t subevent_id);
};

/* The high level meta about a connection. */
struct conn_info_data {
	const uint8_t *name;
	const uint8_t *bdaddr;
	const uint8_t *link_type_str;
	const struct timeval *tv_bgn;
	const struct timeval *tv_end;
};

/* The high level meta about a packet. */
struct packet_info_data {
	uint16_t index;
	const struct timeval *tv;
};

struct subevt_info_data {
	const char *name;
	unsigned char num_slots;
	enum int_types int_type;
	size_t size;
};

typedef void (*conn_info_fetch_func_t)(uint16_t index,
		uint16_t handle, struct conn_info_data *conn_info);
typedef void (*packet_info_fetch_func_t)(struct packet_info_data *packet_info);
typedef void (*conn_alloc_in_middle_func_t)(uint16_t index, uint16_t handle);
typedef void (*subevt_info_fetch_func_t)(uint8_t subevent_id,
					struct subevt_info_data *subevt_info);

void set_conn_info_fetch_func(conn_info_fetch_func_t func);
void set_packet_info_fetch_func(packet_info_fetch_func_t func);
void set_conn_alloc_in_middle_func(conn_alloc_in_middle_func_t func);
void set_subevt_info_fetch_func(subevt_info_fetch_func_t func);

void set_bqr_buffer_length(unsigned int len);
FILE *bqr_file_create(const char *filepath);
void bqr_file_close(void);

void set_analyze_mode(void);
bool is_packet_mode(void);
bool is_analyze_mode(void);
void bqr_stats_post(void);
void queue_new_bqr_stats(void);
void queue_destroy_bqr_stats(void);
void set_bqr_stats_current(uint16_t conn_handle,
	const uint8_t *bqr_stats_subevt_list, uint8_t subevt_list_count);
void subevt_slot_add(uint8_t subevt_idx, unsigned char slot_i,
							const void *value);
void subevt_add(uint8_t subevt_idx, const void *value);
size_t get_int_size(enum int_types int_type);

#endif /* __STATS_H */
