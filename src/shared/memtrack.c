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

static GHashTable *valid_allocs = NULL;

void memtrack_assert_alloc_valid(void *p)
{
	assert(valid_allocs);
	assert(g_slist_find(valid_allocs, p));
}

void memtrack_add_alloc(void *p)
{
	if (!valid_allocs)
		valid_allocs = g_hash_table_new(NULL, NULL);

	assert(!g_hash_table_contains(valid_allocs, p));
	g_hash_table_add(valid_allocs, p);
}

void memtrack_remove_alloc(void *p)
{
	if (!p)
		return;

	assert(valid_allocs);
	assert(g_hash_table_contains(valid_allocs, p));
	g_hash_table_remove(valid_allocs, p);

	// Don't destroy the hashtable when it is empty to avoid reallocation
	// when it is reused.
}
