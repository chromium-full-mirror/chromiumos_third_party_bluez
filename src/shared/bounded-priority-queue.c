// SPDX-License-Identifier: LGPL-2.1-or-later
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

#include <glib.h>

#include "bounded-priority-queue.h"

/* Min-heap priority queue */
struct bpqueue {
	void **values; /* lowest priority on index 0 (first to be kicked out) */
	int capacity;
	int count;

	/* True if first param's priority is lower than second param */
	bpqueue_priority_func priority_func;
	bpqueue_free_func free_func;
};

struct bpqueue *bpqueue_new(int capacity,
			    bpqueue_priority_func priority_func,
			    bpqueue_free_func free_func)
{
	struct bpqueue *q;

	if (!priority_func)
		return NULL;

	q = g_new0(struct bpqueue, 1);
	q->capacity = capacity;
	q->values = g_new0(void *, capacity);
	q->priority_func = priority_func;
	q->free_func = free_func;

	return q;
}

static void bpqueue_push_up(struct bpqueue *q, int i)
{
	int par_i = (i - 1) / 2;
	void *value = q->values[i];

	if (i == 0)
		return;

	if (q->priority_func(q->values[par_i], q->values[i]))
		return;

	q->values[i] = q->values[par_i];
	q->values[par_i] = value;
	bpqueue_push_up(q, par_i);
}

static void bpqueue_push_down(struct bpqueue *q, int par_i)
{
	int left_i = 2 * par_i + 1;
	int right_i = left_i + 1;
	int priority_child_i;
	void *par_value = q->values[par_i];

	if (left_i >= q->count)
		return;

	if (right_i >= q->count)
		priority_child_i = left_i;
	else if (q->priority_func(q->values[left_i], q->values[right_i]))
		priority_child_i = left_i;
	else
		priority_child_i = right_i;

	if (q->priority_func(q->values[par_i], q->values[priority_child_i]))
		return;

	q->values[par_i] = q->values[priority_child_i];
	q->values[priority_child_i] = par_value;
	bpqueue_push_down(q, priority_child_i);
}

void bpqueue_add(struct bpqueue *q, void *value)
{
	/* Space available inside the queue */
	if (q->count < q->capacity) {
		q->values[q->count] = value;
		bpqueue_push_up(q, q->count);
		q->count += 1;
		return;
	}

	/* The candidate value is not within the worst values, don't insert */
	if (q->priority_func(value, q->values[0])) {
		if (q->free_func)
			q->free_func(value);
		return;
	}

	/* Otherwise, swap the least bad value so far with the candidate */
	if (q->free_func)
		q->free_func(q->values[0]);

	q->values[0] = value;
	bpqueue_push_down(q, 0);
}

void bpqueue_free(struct bpqueue *q)
{
	int i;

	if (q->free_func) {
		for (i = 0; i < q->count; i++)
			q->free_func(q->values[i]);
	}

	g_free(q->values);
	g_free(q);
}

void *bpqueue_peek(struct bpqueue *q)
{
	if (!q->count)
		return NULL;

	return q->values[0];
}

void bpqueue_pop(struct bpqueue *q)
{
	void *value;

	if (!q->count)
		return;

	value = q->values[0];
	q->values[0] = q->values[q->count - 1];
	q->count -= 1;

	if (q->free_func)
		q->free_func(value);

	if (q->count)
		bpqueue_push_down(q, 0);
}

int bpqueue_capacity(struct bpqueue *q)
{
	return q->capacity;
}

int bpqueue_count(struct bpqueue *q)
{
	return q->count;
}
