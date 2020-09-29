/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2020  Google LLC. All rights reserved.
 *
 */

#include <assert.h>
#include <glib.h>

/* We expect this library to be used with a small number of memory allocations.
 * So using linked list won't hurt performance while keeping this library
 * lightweight and simple.
 */
static GSList *valid_allocs = NULL;

void memtrack_assert_alloc_valid(void *p)
{
	assert(g_slist_find(valid_allocs, p));
}

void memtrack_add_alloc(void *p)
{
	assert(!g_slist_find(valid_allocs, p));
	valid_allocs = g_slist_append(valid_allocs, p);
}

void memtrack_remove_alloc(void *p)
{
	valid_allocs = g_slist_remove(valid_allocs, p);
}
