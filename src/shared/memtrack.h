/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2020  Google LLC. All rights reserved.
 *
 */

/* Aborts (crashes) if p is not valid allocated memory. Mark all allocs and
 * frees using memtrack_add_alloc() and memtrack_remove_alloc() below.
 */
void memtrack_assert_alloc_valid(void *p);

/* Marks p as allocated. */
void memtrack_add_alloc(void *p);

/* Marks p as freed. */
void memtrack_remove_alloc(void *p);
