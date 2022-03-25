/* SPDX-License-Identifier: LGPL-2.1-or-later */
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

#include <stdbool.h>

struct bpqueue;

typedef bool (*bpqueue_priority_func)(const void *, const void *);
typedef void (*bpqueue_free_func)(void *);

struct bpqueue *bpqueue_new(int capacity,
			    bpqueue_priority_func priority_func,
			    bpqueue_free_func free_func);
void bpqueue_add(struct bpqueue *q, void *value);
void *bpqueue_peek(struct bpqueue *q);
void bpqueue_pop(struct bpqueue *q);
void bpqueue_free(struct bpqueue *q);
int bpqueue_capacity(struct bpqueue *q);
int bpqueue_count(struct bpqueue *q);
