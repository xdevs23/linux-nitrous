// SPDX-License-Identifier: GPL-2.0
/*
 * Shadow Call Stack support.
 *
 * Copyright (C) 2019 Google LLC
 */

#include <linux/kasan.h>
#include <linux/mm.h>
#include <linux/scs.h>
#include <linux/slab.h>
#include <linux/vmstat.h>
#include <asm/scs.h>

static struct kmem_cache *scs_cache;

static void *scs_alloc(int node)
{
	void *s;

	s = kmem_cache_alloc_node(scs_cache, GFP_SCS, node);
	if (s) {
		*__scs_magic(s) = SCS_END_MAGIC;
		/*
		 * Poison the allocation to catch unintentional accesses to
		 * the shadow stack when KASAN is enabled.
		 */
		kasan_poison_object_data(scs_cache, s);
	}

	return s;
}

static void scs_free(void *s)
{
	kasan_unpoison_object_data(scs_cache, s);
	kmem_cache_free(scs_cache, s);
}

void __init scs_init(void)
{
	scs_cache = kmem_cache_create("scs_cache", SCS_SIZE, 0, 0, NULL);
}

static struct page *__scs_page(struct task_struct *tsk)
{
	return virt_to_page(task_scs(tsk));
}

static void scs_account(struct task_struct *tsk, int account)
{
	mod_zone_page_state(page_zone(__scs_page(tsk)), NR_KERNEL_SCS_KB,
		account * (SCS_SIZE / 1024));
}

int scs_prepare(struct task_struct *tsk, int node)
{
	void *s;

	s = scs_alloc(node);
	if (!s)
		return -ENOMEM;

	task_scs(tsk) = s;
	task_scs_offset(tsk) = 0;
	scs_account(tsk, 1);

	return 0;
}

void scs_release(struct task_struct *tsk)
{
	void *s;

	s = task_scs(tsk);
	if (!s)
		return;

	WARN_ON(scs_corrupted(tsk));

	scs_account(tsk, -1);
	scs_free(s);
}
