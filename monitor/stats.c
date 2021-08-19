// SPDX-License-Identifier: LGPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Google LLC
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#include "lib/bluetooth.h"
#include "lib/hci.h"

#include "src/shared/util.h"
#include "src/shared/queue.h"
#include "monitor/display.h"
#include "monitor/stats.h"

/*
 * The default mode is set to MODE_PACKET to print the BQR subevents when
 * reading/writing a btsnoop log. If the mode is set to MODE_ANALYZE, the
 * subevent functions are used to collect and calculate subevent statistics.
 */
static unsigned char mode = MODE_PACKET;

/*
 * Define the default BQR buffer length.
 * Since a btsnoop log can be arbitrarily long, only the last predefined number
 * of entries are kept over which the statistics are calculated. The default is
 * to keep the latest one hour of BQR events. Assume that the Android BQR
 * events are reported every 5 seconds. The buffer length would be 60 * 60 / 5.
 *
 * Note: when the link quality is bad, most vendors send the BQR events more
 *       often than every 5 seconds. This might have an impact on some kinds
 *       of calculations, e.g., 2 secs of data intermixed with 5 secs of data.
 */
#define DEFAULT_BQR_BUFFER_LEN		(60 * 60 / 5)

static uint32_t bqr_buffer_len = DEFAULT_BQR_BUFFER_LEN;
static struct bqr_stats_data *bqr_stats_current;
static struct queue *bqr_stats_queue;

static FILE *fp_bqr;
static const char *filepath_bqr;

#define ABORT_IF_NULL(func)						\
	do {								\
		if (!func) {						\
			fprintf(stderr, "%s: NULL callback", __func__);	\
			abort();					\
		}							\
	} while (0)

static conn_info_fetch_func_t conn_info_fetch_func;
static packet_info_fetch_func_t packet_info_fetch_func;
static conn_alloc_in_middle_func_t conn_alloc_in_middle_func;
static subevt_info_fetch_func_t subevt_info_fetch_func;

static const size_t int_size[] = {
	sizeof(int8_t),
	sizeof(int16_t),
	sizeof(int32_t),
	sizeof(uint8_t),
	sizeof(uint16_t),
	sizeof(uint32_t),
};

size_t get_int_size(enum int_types int_type)
{
	if (int_type >= 0 && int_type < TOTAL_INT_TYPES)
		return int_size[int_type];
	return 0;
}

void set_bqr_buffer_length(unsigned int len)
{
	/* The length should be set before bqr file is opened. */
	if (!filepath_bqr) {
		bqr_buffer_len = len;
		return;
	}

	printf("Error: The bqr file is opened. Failed to set buffer length.\n");
}

FILE *bqr_file_create(const char *filepath)
{
	if (!filepath)
		return NULL;

	filepath_bqr = filepath;
	fp_bqr = fopen(filepath, "w");

	return fp_bqr;
}

void bqr_file_close(void)
{
	if (fp_bqr) {
		fclose(fp_bqr);
		fp_bqr = NULL;
		filepath_bqr = NULL;
	}
}

/*
 * This function maps a subevt_idx to the corresponding stat_idx of
 * the bqr_stats_data's stats array.
 *
 *   As an example, assume that subevt_idx is SCO_RX_HEC_ERROR in
 *   intel.c:intel_stats_sco_subevt_list, the corresponding stat_idx is 5.
 *
 *   As another example, assume that subevt_idx is NO_RX_COUNT in
 *   aosp.c:bqr_stats_subevt_list, the corresponding stat_idx is 6.
 */
static bool get_stats_idx(uint8_t subevt_idx, uint8_t *stat_idx)
{
	uint8_t i;

	for (i = 0; i < bqr_stats_current->subevt_list_count; i++) {
		if (bqr_stats_current->bqr_stats_subevt_list[i] == subevt_idx) {
			*stat_idx = i;
			return true;
		}
	}

	return false;
}

#define calc_stat(min, max, value, sum, num_entries, mean, sd, type)	\
	do {								\
		unsigned int i;						\
		double sqrd_dev = 0;					\
									\
		sum = 0;						\
		*((type *) min) = ((type *) (value))[first];		\
		*((type *) max) = ((type *) (value))[first];		\
									\
		for (i = first; i <= last; i++) {			\
			*((type *) min) = MIN(((type *) (value))[i],	\
						*((type *) min));	\
			*((type *) max) = MAX(((type *) (value))[i],	\
						*((type *) max));	\
			sum += ((type *) (value))[i];			\
		}							\
		mean = sum / num_entries;				\
									\
		for (i = first; i <= last; i++)				\
			sqrd_dev += pow(((type *) (value))[i] - mean, 2);\
		sd = sqrt(sqrd_dev / num_entries);			\
	} while (0)

#define calc_stat_for_type(type)					\
	calc_stat(slot->min, slot->max, slot->value, slot->sum,		\
			bqr_stats->num_entries, slot->mean, slot->sd, type)

static void subevt_slot_calc_stat(struct bqr_stats_data *bqr_stats,
				uint8_t stat_idx, unsigned char slot_i,
				unsigned int first, unsigned int last)
{
	struct subevt_stat *stat = &bqr_stats->stats[stat_idx];
	struct subevt_stat_slot *slot = &stat->slots[slot_i];

	if (bqr_stats->num_entries == 0)
		return;

	/*
	 * Adjust first and last so that first < last which makes the
	 * for loops in the calc_stat macro simpler.
	 *
	 * When calculating the statistics of sum, mean, and sd, there is
	 * no difference about the ordering.
	 * Conditions
	 *   - when first < last: keep them as they are
	 *   - when first > last: make first = 0, and last = bqr_buffer_len - 1
	 *     (In this case, all entries in the value buffer are valid.)
	 */
	if (first > last) {
		first = 0;
		last = bqr_buffer_len - 1;
	}

	switch (bqr_stats->stats[stat_idx].int_type) {
	case INT8:
		calc_stat_for_type(int8_t);
		break;
	case INT16:
		calc_stat_for_type(int16_t);
		break;
	case INT32:
		calc_stat_for_type(int32_t);
		break;
	case UINT8:
		calc_stat_for_type(uint8_t);
		break;
	case UINT16:
		calc_stat_for_type(uint16_t);
		break;
	case UINT32:
		calc_stat_for_type(uint32_t);
		break;
	default:
		printf("Error: unsupported integer type %u\n", stat->int_type);
		break;
	}
}

static void subevt_calc_stat(struct bqr_stats_data *bqr_stats, uint8_t stat_idx,
					unsigned int first, unsigned int last)
{
	unsigned char slot_i;
	const struct subevt_stat *stat = &bqr_stats->stats[stat_idx];

	for (slot_i = 0; slot_i < stat->num_slots; slot_i++)
		subevt_slot_calc_stat(bqr_stats, stat_idx, slot_i, first, last);
}

static void subevt_slot_print_value(const struct bqr_stats_data *bqr_stats,
			uint8_t stat_idx, unsigned char slot_i, unsigned int i)
{
	const struct subevt_stat *stat = &bqr_stats->stats[stat_idx];
	const struct subevt_stat_slot *slot = &stat->slots[slot_i];

	switch (stat->int_type) {
	case INT8:
		fprintf(fp_bqr, "%" PRId8, ((int8_t *) (slot->value))[i]);
		break;
	case INT16:
		fprintf(fp_bqr, "%" PRId16, ((int16_t *) (slot->value))[i]);
		break;
	case INT32:
		fprintf(fp_bqr, "%" PRId32, ((int32_t *) (slot->value))[i]);
		break;
	case UINT8:
		fprintf(fp_bqr, "%" PRIu8, ((uint8_t *) (slot->value))[i]);
		break;
	case UINT16:
		fprintf(fp_bqr, "%" PRIu16, ((uint16_t *) (slot->value))[i]);
		break;
	case UINT32:
		fprintf(fp_bqr, "%" PRIu32, ((uint32_t *) (slot->value))[i]);
		break;
	default:
		printf("Error: unsupported integer type %u\n", stat->int_type);
		break;
	}
}

#define print_stat(name, format, type)					\
	do {								\
		printf("    %s: ", name);				\
		printf("mean %.2f sd %.2f ", slot->mean, slot->sd);	\
		printf("min " format " max " format " sum %.0f\n",	\
			*((type *) slot->min),				\
			*((type *) slot->max), slot->sum);		\
	} while (0)

static void subevt_slot_print_stat(const struct bqr_stats_data *bqr_stats,
		const char *subevt_name, uint8_t stat_idx,
		unsigned char slot_i, unsigned int first, unsigned int last)
{
	const struct subevt_stat *stat = &bqr_stats->stats[stat_idx];
	const struct subevt_stat_slot *slot = &stat->slots[slot_i];

	switch (stat->int_type) {
	case INT8:
		print_stat(subevt_name, "%" PRId8, int8_t);
		break;
	case INT16:
		print_stat(subevt_name, "%" PRId16, int16_t);
		break;
	case INT32:
		print_stat(subevt_name, "%" PRId32, int32_t);
		break;
	case UINT8:
		print_stat(subevt_name, "%" PRIu8, uint8_t);
		break;
	case UINT16:
		print_stat(subevt_name, "%" PRIu16, uint16_t);
		break;
	case UINT32:
		print_stat(subevt_name, "%" PRIu32, uint32_t);
		break;
	default:
		printf("Error: unsupported integer type %u\n", stat->int_type);
		break;
	}
}

static void subevt_print_stat(const struct bqr_stats_data *bqr_stats,
				uint8_t stat_idx, const char *subevt_name,
				unsigned int first, unsigned int last)
{
	unsigned char slot_i;
	const struct subevt_stat *stat = &bqr_stats->stats[stat_idx];

	for (slot_i = 0; slot_i < stat->num_slots; slot_i++) {
		/*
		 * If there are multiple slots in the subevent, it is required
		 * to append "(slot n)" to the slot_name where n is slot_i.
		 *
		 * For example, a subevent with slots would be displayed as
		 *	Rx NAK errors (slot 0): mean .. sd .. min .. max ..
		 *	Rx NAK errors (slot 1): mean .. sd .. min .. max ..
		 *	Rx NAK errors (slot 2): mean .. sd .. min .. max ..
		 *	...
		 *
		 * On the other hand, a subevent without slots is displayed like
		 *	Rx payload lost: mean .. sd .. min .. max ..
		 */
		char *slot_name = NULL;

		if (stat->num_slots > 1) {
			if (asprintf(&slot_name, "%s (slot %u)", subevt_name,
								slot_i) < 0)
				return;
		}

		subevt_slot_print_stat(bqr_stats,
					slot_name ? slot_name : subevt_name,
					stat_idx, slot_i, first, last);

		if (slot_name)
			free(slot_name);
	}
}

void subevt_slot_add(uint8_t subevt_idx, unsigned char slot_i,
							const void *value)
{
	uint8_t stat_idx;
	struct subevt_stat *stat;
	struct subevt_stat_slot *slot;
	unsigned int next = bqr_stats_current->next;

	if (!get_stats_idx(subevt_idx, &stat_idx))
		return;

	stat = &bqr_stats_current->stats[stat_idx];
	slot = &stat->slots[slot_i];

	switch (stat->int_type) {
	case INT8:
		((int8_t *) slot->value)[next] = *((int8_t *) value);
		break;
	case INT16:
		((int16_t *) slot->value)[next] = *((int16_t *) value);
		break;
	case INT32:
		((int32_t *) slot->value)[next] = *((int32_t *) value);
		break;
	case UINT8:
		((uint8_t *) slot->value)[next] = *((uint8_t *) value);
		break;
	case UINT16:
		((uint16_t *) slot->value)[next] = *((uint16_t *) value);
		break;
	case UINT32:
		((uint32_t *) slot->value)[next] = *((uint32_t *) value);
		break;
	default:
		break;
	}
}

/*
 * In most cases, a subevent only has a slot. Use this function to add
 * the value to the default 0-th slot.
 */
void subevt_add(uint8_t subevt_idx, const void *value)
{
	subevt_slot_add(subevt_idx, 0, value);
}

static void stat_free(struct subevt_stat *stat)
{
	unsigned char i;

	for (i = 0; i < stat->num_slots; i++) {
		free(stat->slots[i].value);
		free(stat->slots[i].min);
		free(stat->slots[i].max);
	}

	free(stat->slots);
}

static bool stat_alloc(struct subevt_stat *stat, unsigned char num_slots,
				enum int_types int_type, size_t subevt_size)
{
	unsigned char slot_i;
	unsigned char i;

	stat->int_type = int_type;
	stat->subevt_size = subevt_size;
	stat->num_slots = num_slots;

	/* Most subevts have only 1 slot. Some subevts have multiple slots. */
	stat->slots = calloc(num_slots, sizeof(struct subevt_stat_slot));
	if (!stat->slots)
		goto exit;

	for (slot_i = 0; slot_i < num_slots; slot_i++) {
		stat->slots[slot_i].value = calloc(bqr_buffer_len, subevt_size);
		if (!stat->slots[slot_i].value)
			goto failed_slot_value;
	}

	for (slot_i = 0; slot_i < num_slots; slot_i++) {
		stat->slots[slot_i].min = calloc(1, get_int_size(int_type));
		if (!stat->slots[slot_i].min)
			goto failed_slot_min;
	}

	for (slot_i = 0; slot_i < num_slots; slot_i++) {
		stat->slots[slot_i].max = calloc(1, get_int_size(int_type));
		if (!stat->slots[slot_i].max)
			goto failed_slot_max;
	}

	return true;

failed_slot_max:
	for (i = 0; i < slot_i; i++)
		free(stat->slots[i].max);
	slot_i = num_slots;

failed_slot_min:
	for (i = 0; i < slot_i; i++)
		free(stat->slots[i].min);
	slot_i = num_slots;

failed_slot_value:
	for (i = 0; i < slot_i; i++)
		free(stat->slots[i].value);
	free(stat->slots);

exit:
	return false;
}

void set_subevt_info_fetch_func(subevt_info_fetch_func_t func)
{
	subevt_info_fetch_func = func;
}

static struct bqr_stats_data *bqr_stats_alloc(uint16_t conn_handle,
		const uint8_t *bqr_stats_subevt_list, uint8_t subevt_list_count)
{
	struct bqr_stats_data *bqr_stats;
	uint8_t i, stat_idx;

	bqr_stats = new0(struct bqr_stats_data, 1);
	if (!bqr_stats)
		goto exit;

	bqr_stats->conn_handle = conn_handle;
	bqr_stats->subevt_list_count = subevt_list_count;

	bqr_stats->bqr_stats_subevt_list = bqr_stats_subevt_list;

	bqr_stats->stats = calloc(bqr_stats->subevt_list_count,
						sizeof(struct subevt_stat));
	if (bqr_stats->stats == NULL)
		goto failed_stats;

	bqr_stats->tv = calloc(bqr_buffer_len, sizeof(struct timeval));
	if (!bqr_stats->tv)
		goto failed_tv;

	for (stat_idx = 0; stat_idx < bqr_stats->subevt_list_count;
								stat_idx++) {
		struct subevt_info_data subevt_info = { 0 };
		uint8_t subevt_idx = bqr_stats_subevt_list[stat_idx];

		ABORT_IF_NULL(subevt_info_fetch_func);
		subevt_info_fetch_func(subevt_idx, &subevt_info);

		enum int_types int_type = subevt_info.int_type;
		size_t subevt_size = subevt_info.size;
		unsigned char num_slots = subevt_info.num_slots;

		if (!stat_alloc(&bqr_stats->stats[stat_idx], num_slots,
							int_type, subevt_size))
			goto failed_stat_alloc;
	}

	return bqr_stats;

failed_stat_alloc:
	for (i = 0; i < stat_idx; i++)
		stat_free(&bqr_stats->stats[stat_idx]);

	free(bqr_stats->tv);

failed_tv:
	free(bqr_stats->stats);

failed_stats:
	free(bqr_stats);

exit:
	return NULL;
}

static void get_time_str(char *ts_str, const struct timeval *tv)
{
	struct tm tm;
	const struct timeval tv0 = { 0 };

	if (!ts_str)
		return;

	if (!tv || !memcmp(tv, &tv0, sizeof(tv0))) {
		sprintf(ts_str, "(na)");
		return;
	}

	localtime_r(&tv->tv_sec, &tm);

	sprintf(ts_str, "%04d-%02d-%02d %02d:%02d:%02d.%06lu",
		tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		tm.tm_hour, tm.tm_min, tm.tm_sec, tv->tv_usec);
}

static void print_name(FILE *fp, const char *prefix_str, const uint8_t *name)
{
	fprintf(fp, "%sname:    %s\n",
		prefix_str, name && name[0] ? (char *)name : "(na)");
}

static void print_address(FILE *fp, const char *prefix_str, const uint8_t *addr)
{
	const uint8_t addr0[6] = { 0 };

	if (!addr || !memcmp(addr, addr0, 6)) {
		fprintf(fp, "%saddress: (na)\n", prefix_str);
		return;
	}

	fprintf(fp, "%saddress: %2.2X:%2.2X:%2.2X:%2.2X:%2.2X:%2.2X\n",
		prefix_str, addr[5], addr[4], addr[3], addr[2], addr[1],
		addr[0]);
}

void set_conn_info_fetch_func(conn_info_fetch_func_t func)
{
	conn_info_fetch_func = func;
}

/* ts format: "%04d-%02d-%02d %02d:%02d:%02d.%06lu" */
#define TS_PRINT_FORMAT_SIZE			27

static void print_conn_properties(FILE *fp, const char *prefix_str,
					struct bqr_stats_data *bqr_stats)
{
	uint16_t conn_handle = bqr_stats->conn_handle;
	uint16_t index = bqr_stats->index;
	char ts_bgn[TS_PRINT_FORMAT_SIZE];
	char ts_end[TS_PRINT_FORMAT_SIZE];
	struct conn_info_data conn_info = { 0 };

	ABORT_IF_NULL(conn_info_fetch_func);
	conn_info_fetch_func(index, conn_handle, &conn_info);

	get_time_str(ts_bgn, conn_info.tv_bgn);
	fprintf(fp, "%sbegin:   %s\n", prefix_str, ts_bgn);

	get_time_str(ts_end, conn_info.tv_end);
	fprintf(fp, "%send:     %s\n", prefix_str, ts_end);

	print_address(fp, prefix_str, conn_info.bdaddr);

	print_name(fp, prefix_str, conn_info.name);

	fprintf(fp, "%slink:    %s\n", prefix_str, conn_info.link_type_str);
}

static void bqr_stats_print(struct bqr_stats_data *bqr_stats)
{
	unsigned int first = 0;
	unsigned int last = 0;
	unsigned char slot_i;
	uint8_t subevt_list_count = bqr_stats->subevt_list_count;
	uint8_t stat_idx;
	unsigned int i;

	/*
	 * The subevt_stat's value member points to a cyclic buffer.
	 * When total_count == 0, (no bqr events)
	 *   - the first valid index is 0
	 *   - the last valid index is 0
	 * When total_count <= bqr_buffer_len,
	 *   - the first valid index is 0
	 *   - the last valid index is (total_count - 1)
	 * When total_count > bqr_buffer_len,
	 *   - the first valid index is (next)
	 *   - the last valid index is
	 *     (next + bqr_buffer_len - 1) % bqr_buffer_len
	 */
	if (bqr_stats->total_count > 0) {
		if (bqr_stats->total_count <= bqr_buffer_len) {
			first = 0;
			last = bqr_stats->total_count - 1;
		} else {
			first = bqr_stats->next;
			last = (bqr_stats->next + bqr_buffer_len - 1) %
								bqr_buffer_len;
		}
	}

	printf("Connection handle: %" PRIu16 "\n", bqr_stats->conn_handle);
	printf("  # Properties\n");
	print_conn_properties(stdout, "    ", bqr_stats);
	printf("%sbuffer:  first %u last %u num_entries %u"
		" total_count %u (%" PRIu8 " subevents)\n",
		"    ", first, last, bqr_stats->num_entries,
		bqr_stats->total_count, subevt_list_count);

	/* Print subevents statistics. */
	if (bqr_stats->total_count > 0) {
		printf("  # BQR subevents\n");
		for (stat_idx = 0; stat_idx < subevt_list_count; stat_idx++) {
			struct subevt_info_data subevt_info = { 0 };
			uint8_t subevt_idx =
				bqr_stats->bqr_stats_subevt_list[stat_idx];

			ABORT_IF_NULL(subevt_info_fetch_func);
			subevt_info_fetch_func(subevt_idx, &subevt_info);

			subevt_calc_stat(bqr_stats, stat_idx, first, last);
			subevt_print_stat(bqr_stats, stat_idx,
						subevt_info.name, first, last);
		}
		printf("\n");
	}

	/* Print subevent raw values to the file in csv format. */
	if (!fp_bqr)
		return;

	fprintf(fp_bqr, "\n# Connection handle: %" PRIu16 "\n",
				bqr_stats->conn_handle);
	print_conn_properties(fp_bqr, "# ", bqr_stats);

	if (bqr_stats->total_count == 0) {
		fprintf(fp_bqr, "No BQR events\n");
		return;
	}

	/* Print subevent names in csv format. */
	fprintf(fp_bqr, "time,");
	for (stat_idx = 0; stat_idx < subevt_list_count; stat_idx++) {
		struct subevt_info_data subevt_info = { 0 };
		const struct subevt_stat *stat = &bqr_stats->stats[stat_idx];
		uint8_t subevt_idx = bqr_stats->bqr_stats_subevt_list[stat_idx];

		ABORT_IF_NULL(subevt_info_fetch_func);
		subevt_info_fetch_func(subevt_idx, &subevt_info);

		/*
		 * If a subevent has multiple slots, it is required to print
		 * the name as
		 *   <subevt name> (slot 0)
		 *   <subevt name> (slot 1)
		 *   ...
		 */
		for (slot_i = 0; slot_i < stat->num_slots; slot_i++) {
			fprintf(fp_bqr, "%s", subevt_info.name);

			if (stat->num_slots > 1)
				fprintf(fp_bqr, " (slot %u)", slot_i);

			if (slot_i < stat->num_slots - 1)
				fprintf(fp_bqr, ",");
		}

		if (stat_idx < subevt_list_count - 1)
			fprintf(fp_bqr, ",");
	}
	fprintf(fp_bqr, "\n");

	for (i = first;; i = (i + 1) % bqr_buffer_len) {
		char ts[TS_PRINT_FORMAT_SIZE];

		get_time_str(ts, &bqr_stats->tv[i]);
		fprintf(fp_bqr, "%s,", ts);

		for (stat_idx = 0; stat_idx < subevt_list_count; stat_idx++) {
			struct subevt_stat *stat = &bqr_stats->stats[stat_idx];

			for (slot_i = 0; slot_i < stat->num_slots; slot_i++) {
				subevt_slot_print_value(bqr_stats, stat_idx,
							slot_i, i);
				if (slot_i < stat->num_slots - 1)
					fprintf(fp_bqr, ",");
			}
			if (stat_idx < subevt_list_count - 1)
				fprintf(fp_bqr, ",");
		}
		fprintf(fp_bqr, "\n");

		if (i == last)
			break;
	}
}

static void bqr_stats_destroy(void *data)
{
	struct bqr_stats_data *bqr_stats = data;
	uint8_t stat_idx;

	bqr_stats_print(bqr_stats);

	for (stat_idx = 0; stat_idx < bqr_stats->subevt_list_count; stat_idx++)
		stat_free(&bqr_stats->stats[stat_idx]);

	free(bqr_stats->stats);
	free(bqr_stats->tv);
	free(bqr_stats);
}

static bool bqr_stats_match_handle(const void *a, const void *b)
{
	const struct bqr_stats_data *bqr_stats = a;
	uint16_t conn_handle = *((const uint16_t *) b);

	return bqr_stats->conn_handle == conn_handle;
}

static struct bqr_stats_data *bqr_stats_lookup(uint16_t conn_handle)
{
	return queue_find(bqr_stats_queue, bqr_stats_match_handle,
						(const void *) &conn_handle);
}

bool is_packet_mode(void)
{
	return mode == MODE_PACKET;
}

bool is_analyze_mode(void)
{
	return mode == MODE_ANALYZE;
}

void set_analyze_mode(void)
{
	mode = MODE_ANALYZE;
}

void queue_new_bqr_stats(void)
{
	if (is_analyze_mode() && !bqr_stats_queue)
		bqr_stats_queue = queue_new();
}

void queue_destroy_bqr_stats(void)
{
	uint16_t num_conn_handles;

	num_conn_handles = queue_length(bqr_stats_queue);
	if (num_conn_handles == 0)
		return;

	printf("Bluetooth Quality Report: %" PRIu16 " connection handles\n\n",
							num_conn_handles);
	queue_destroy(bqr_stats_queue, bqr_stats_destroy);

	if (fp_bqr != NULL)
		printf("The BQR subevents are saved in %s\n\n", filepath_bqr);
}

void set_packet_info_fetch_func(packet_info_fetch_func_t func)
{
	packet_info_fetch_func = func;
}

void set_conn_alloc_in_middle_func(conn_alloc_in_middle_func_t func)
{
	conn_alloc_in_middle_func = func;
}

void set_bqr_stats_current(uint16_t conn_handle,
		const uint8_t *bqr_stats_subevt_list, uint8_t subevt_list_count)
{
	struct packet_info_data packet_info = { 0 };

	bqr_stats_current = bqr_stats_lookup(conn_handle);

	if (!bqr_stats_current) {
		bqr_stats_current = bqr_stats_alloc(conn_handle,
				bqr_stats_subevt_list, subevt_list_count);
		if (!bqr_stats_current) {
			printf("Failed to create bqr stats for connection %"
						PRIu16 "\n", conn_handle);
			return;
		}

		queue_push_tail(bqr_stats_queue, bqr_stats_current);
	}

	ABORT_IF_NULL(packet_info_fetch_func);
	packet_info_fetch_func(&packet_info);

	bqr_stats_current->tv[bqr_stats_current->next] = *packet_info.tv;
	bqr_stats_current->index = packet_info.index;

	/* It is possible that a btsnoop log may start capturing packets
	 * in the middle of a connection. In this case, it is required to
	 * allocate a connection if not yet.
	 */
	ABORT_IF_NULL(conn_alloc_in_middle_func);
	conn_alloc_in_middle_func(packet_info.index, conn_handle);
}

void bqr_stats_post(void)
{
	if (is_analyze_mode() && bqr_stats_current != NULL) {
		bqr_stats_current->total_count++;
		bqr_stats_current->num_entries =
			MIN(bqr_stats_current->num_entries + 1, bqr_buffer_len);
		bqr_stats_current->next = (bqr_stats_current->next + 1) %
						bqr_buffer_len;
	}
}
