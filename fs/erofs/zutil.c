// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2018 HUAWEI, Inc.
 *             https://www.huawei.com/
 */
#include "internal.h"
#include <linux/pagevec.h>

struct z_erofs_gbuf {
	spinlock_t lock;
	void *ptr;
	struct page **pages;
	unsigned int nrpages;
};

static struct z_erofs_gbuf *z_erofs_gbufpool, *z_erofs_rsvbuf;
static unsigned int z_erofs_gbuf_count, z_erofs_gbuf_nrpages,
		z_erofs_rsv_nrpages;

module_param_named(global_buffers, z_erofs_gbuf_count, uint, 0444);
module_param_named(reserved_pages, z_erofs_rsv_nrpages, uint, 0444);

static atomic_long_t erofs_global_shrink_cnt;	/* for all mounted instances */
/* protected by 'erofs_sb_list_lock' */
static unsigned int shrinker_run_no;

/* protects the mounted 'erofs_sb_list' */
static DEFINE_SPINLOCK(erofs_sb_list_lock);
static LIST_HEAD(erofs_sb_list);

static unsigned int z_erofs_gbuf_id(void)
{
	return raw_smp_processor_id() % z_erofs_gbuf_count;
}

void *z_erofs_get_gbuf(unsigned int requiredpages)
	__acquires(gbuf->lock)
{
	struct z_erofs_gbuf *gbuf;

	migrate_disable();
	gbuf = &z_erofs_gbufpool[z_erofs_gbuf_id()];
	spin_lock(&gbuf->lock);
	/* check if the buffer is too small */
	if (requiredpages > gbuf->nrpages) {
		spin_unlock(&gbuf->lock);
		migrate_enable();
		/* (for sparse checker) pretend gbuf->lock is still taken */
		__acquire(gbuf->lock);
		return NULL;
	}
	return gbuf->ptr;
}

void z_erofs_put_gbuf(void *ptr) __releases(gbuf->lock)
{
	struct z_erofs_gbuf *gbuf;

	gbuf = &z_erofs_gbufpool[z_erofs_gbuf_id()];
	DBG_BUGON(gbuf->ptr != ptr);
	spin_unlock(&gbuf->lock);
	migrate_enable();
}

int z_erofs_gbuf_growsize(unsigned int nrpages)
{
	static DEFINE_MUTEX(gbuf_resize_mutex);
	struct page **tmp_pages = NULL;
	struct z_erofs_gbuf *gbuf;
	void *ptr, *old_ptr;
	int last, i, j;

	mutex_lock(&gbuf_resize_mutex);
	/* avoid shrinking gbufs, since no idea how many fses rely on */
	if (nrpages <= z_erofs_gbuf_nrpages) {
		mutex_unlock(&gbuf_resize_mutex);
		return 0;
	}

	for (i = 0; i < z_erofs_gbuf_count; ++i) {
		gbuf = &z_erofs_gbufpool[i];
		tmp_pages = kcalloc(nrpages, sizeof(*tmp_pages), GFP_KERNEL);
		if (!tmp_pages)
			goto out;

		for (j = 0; j < gbuf->nrpages; ++j)
			tmp_pages[j] = gbuf->pages[j];
		do {
			last = j;
			j = alloc_pages_bulk_array(GFP_KERNEL, nrpages,
						   tmp_pages);
			if (last == j)
				goto out;
		} while (j != nrpages);

		ptr = vmap(tmp_pages, nrpages, VM_MAP, PAGE_KERNEL);
		if (!ptr)
			goto out;

		spin_lock(&gbuf->lock);
		kfree(gbuf->pages);
		gbuf->pages = tmp_pages;
		old_ptr = gbuf->ptr;
		gbuf->ptr = ptr;
		gbuf->nrpages = nrpages;
		spin_unlock(&gbuf->lock);
		if (old_ptr)
			vunmap(old_ptr);
	}
	z_erofs_gbuf_nrpages = nrpages;
out:
	if (i < z_erofs_gbuf_count && tmp_pages) {
		for (j = 0; j < nrpages; ++j)
			if (tmp_pages[j] && (j >= gbuf->nrpages ||
					     tmp_pages[j] != gbuf->pages[j]))
				__free_page(tmp_pages[j]);
		kfree(tmp_pages);
	}
	mutex_unlock(&gbuf_resize_mutex);
	return i < z_erofs_gbuf_count ? -ENOMEM : 0;
}

int __init z_erofs_gbuf_init(void)
{
	unsigned int i, total = num_possible_cpus();

	if (z_erofs_gbuf_count)
		total = min(z_erofs_gbuf_count, total);
	z_erofs_gbuf_count = total;

	/* The last (special) global buffer is the reserved buffer */
	total += !!z_erofs_rsv_nrpages;

	z_erofs_gbufpool = kcalloc(total, sizeof(*z_erofs_gbufpool),
				   GFP_KERNEL);
	if (!z_erofs_gbufpool)
		return -ENOMEM;

	if (z_erofs_rsv_nrpages) {
		z_erofs_rsvbuf = &z_erofs_gbufpool[total - 1];
		z_erofs_rsvbuf->pages = kcalloc(z_erofs_rsv_nrpages,
				sizeof(*z_erofs_rsvbuf->pages), GFP_KERNEL);
		if (!z_erofs_rsvbuf->pages) {
			z_erofs_rsvbuf = NULL;
			z_erofs_rsv_nrpages = 0;
		}
	}
	for (i = 0; i < total; ++i)
		spin_lock_init(&z_erofs_gbufpool[i].lock);
	return 0;
}

void z_erofs_gbuf_exit(void)
{
	int i, j;

	for (i = 0; i < z_erofs_gbuf_count + (!!z_erofs_rsvbuf); ++i) {
		struct z_erofs_gbuf *gbuf = &z_erofs_gbufpool[i];

		if (gbuf->ptr) {
			vunmap(gbuf->ptr);
			gbuf->ptr = NULL;
		}

		if (!gbuf->pages)
			continue;

		for (j = 0; j < gbuf->nrpages; ++j)
			if (gbuf->pages[j])
				put_page(gbuf->pages[j]);
		kfree(gbuf->pages);
		gbuf->pages = NULL;
	}
	kfree(z_erofs_gbufpool);
}

struct page *__erofs_allocpage(struct page **pagepool, gfp_t gfp, bool tryrsv)
{
	struct page *page = *pagepool;

	if (page) {
		*pagepool = (struct page *)page_private(page);
	} else if (tryrsv && z_erofs_rsvbuf && z_erofs_rsvbuf->nrpages) {
		spin_lock(&z_erofs_rsvbuf->lock);
		if (z_erofs_rsvbuf->nrpages)
			page = z_erofs_rsvbuf->pages[--z_erofs_rsvbuf->nrpages];
		spin_unlock(&z_erofs_rsvbuf->lock);
	}
	if (!page)
		page = alloc_page(gfp);
	DBG_BUGON(page && page_ref_count(page) != 1);
	return page;
}

void erofs_release_pages(struct page **pagepool)
{
	while (*pagepool) {
		struct page *page = *pagepool;

		*pagepool = (struct page *)page_private(page);
		/* try to fill reserved global pool first */
		if (z_erofs_rsvbuf && z_erofs_rsvbuf->nrpages <
				z_erofs_rsv_nrpages) {
			spin_lock(&z_erofs_rsvbuf->lock);
			if (z_erofs_rsvbuf->nrpages < z_erofs_rsv_nrpages) {
				z_erofs_rsvbuf->pages[z_erofs_rsvbuf->nrpages++]
						= page;
				spin_unlock(&z_erofs_rsvbuf->lock);
				continue;
			}
			spin_unlock(&z_erofs_rsvbuf->lock);
		}
		put_page(page);
	}
}

static int erofs_workgroup_get(struct erofs_workgroup *grp)
{
	int o;

repeat:
	o = erofs_wait_on_workgroup_freezed(grp);
	if (o <= 0)
		return -1;

	if (atomic_cmpxchg(&grp->refcount, o, o + 1) != o)
		goto repeat;

	/* decrease refcount paired by erofs_workgroup_put */
	if (o == 1)
		atomic_long_dec(&erofs_global_shrink_cnt);
	return 0;
}

struct erofs_workgroup *erofs_find_workgroup(struct super_block *sb,
					     pgoff_t index)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	struct erofs_workgroup *grp;

repeat:
	rcu_read_lock();
	grp = xa_load(&sbi->managed_pslots, index);
	if (grp) {
		if (erofs_workgroup_get(grp)) {
			/* prefer to relax rcu read side */
			rcu_read_unlock();
			goto repeat;
		}

		DBG_BUGON(index != grp->index);
	}
	rcu_read_unlock();
	return grp;
}

struct erofs_workgroup *erofs_insert_workgroup(struct super_block *sb,
					       struct erofs_workgroup *grp)
{
	struct erofs_sb_info *const sbi = EROFS_SB(sb);
	struct erofs_workgroup *pre;

	/*
	 * Bump up a reference count before making this visible
	 * to others for the XArray in order to avoid potential
	 * UAF without serialized by xa_lock.
	 */
	atomic_inc(&grp->refcount);

repeat:
	xa_lock(&sbi->managed_pslots);
	pre = __xa_cmpxchg(&sbi->managed_pslots, grp->index,
			   NULL, grp, GFP_NOFS);
	if (pre) {
		if (xa_is_err(pre)) {
			pre = ERR_PTR(xa_err(pre));
		} else if (erofs_workgroup_get(pre)) {
			/* try to legitimize the current in-tree one */
			xa_unlock(&sbi->managed_pslots);
			cond_resched();
			goto repeat;
		}
		atomic_dec(&grp->refcount);
		grp = pre;
	}
	xa_unlock(&sbi->managed_pslots);
	return grp;
}

static void  __erofs_workgroup_free(struct erofs_workgroup *grp)
{
	atomic_long_dec(&erofs_global_shrink_cnt);
	erofs_workgroup_free_rcu(grp);
}

int erofs_workgroup_put(struct erofs_workgroup *grp)
{
	int count = atomic_dec_return(&grp->refcount);

	if (count == 1)
		atomic_long_inc(&erofs_global_shrink_cnt);
	else if (!count)
		__erofs_workgroup_free(grp);
	return count;
}

static bool erofs_try_to_release_workgroup(struct erofs_sb_info *sbi,
					   struct erofs_workgroup *grp)
{
	/*
	 * If managed cache is on, refcount of workgroups
	 * themselves could be < 0 (freezed). In other words,
	 * there is no guarantee that all refcounts > 0.
	 */
	if (!erofs_workgroup_try_to_freeze(grp, 1))
		return false;

	/*
	 * Note that all cached pages should be unattached
	 * before deleted from the XArray. Otherwise some
	 * cached pages could be still attached to the orphan
	 * old workgroup when the new one is available in the tree.
	 */
	if (erofs_try_to_free_all_cached_pages(sbi, grp)) {
		erofs_workgroup_unfreeze(grp, 1);
		return false;
	}

	/*
	 * It's impossible to fail after the workgroup is freezed,
	 * however in order to avoid some race conditions, add a
	 * DBG_BUGON to observe this in advance.
	 */
	DBG_BUGON(__xa_erase(&sbi->managed_pslots, grp->index) != grp);

	/* last refcount should be connected with its managed pslot.  */
	erofs_workgroup_unfreeze(grp, 0);
	__erofs_workgroup_free(grp);
	return true;
}

static unsigned long erofs_shrink_workstation(struct erofs_sb_info *sbi,
					      unsigned long nr_shrink)
{
	struct erofs_workgroup *grp;
	unsigned int freed = 0;
	unsigned long index;

	xa_lock(&sbi->managed_pslots);
	xa_for_each(&sbi->managed_pslots, index, grp) {
		/* try to shrink each valid workgroup */
		if (!erofs_try_to_release_workgroup(sbi, grp))
			continue;
		xa_unlock(&sbi->managed_pslots);

		++freed;
		if (!--nr_shrink)
			return freed;
		xa_lock(&sbi->managed_pslots);
	}
	xa_unlock(&sbi->managed_pslots);
	return freed;
}

void erofs_shrinker_register(struct super_block *sb)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);

	mutex_init(&sbi->umount_mutex);

	spin_lock(&erofs_sb_list_lock);
	list_add(&sbi->list, &erofs_sb_list);
	spin_unlock(&erofs_sb_list_lock);
}

void erofs_shrinker_unregister(struct super_block *sb)
{
	struct erofs_sb_info *const sbi = EROFS_SB(sb);

	mutex_lock(&sbi->umount_mutex);
	/* clean up all remaining workgroups in memory */
	erofs_shrink_workstation(sbi, ~0UL);

	spin_lock(&erofs_sb_list_lock);
	list_del(&sbi->list);
	spin_unlock(&erofs_sb_list_lock);
	mutex_unlock(&sbi->umount_mutex);
}

static unsigned long erofs_shrink_count(struct shrinker *shrink,
					struct shrink_control *sc)
{
	return atomic_long_read(&erofs_global_shrink_cnt);
}

static unsigned long erofs_shrink_scan(struct shrinker *shrink,
				       struct shrink_control *sc)
{
	struct erofs_sb_info *sbi;
	struct list_head *p;

	unsigned long nr = sc->nr_to_scan;
	unsigned int run_no;
	unsigned long freed = 0;

	spin_lock(&erofs_sb_list_lock);
	do {
		run_no = ++shrinker_run_no;
	} while (run_no == 0);

	/* Iterate over all mounted superblocks and try to shrink them */
	p = erofs_sb_list.next;
	while (p != &erofs_sb_list) {
		sbi = list_entry(p, struct erofs_sb_info, list);

		/*
		 * We move the ones we do to the end of the list, so we stop
		 * when we see one we have already done.
		 */
		if (sbi->shrinker_run_no == run_no)
			break;

		if (!mutex_trylock(&sbi->umount_mutex)) {
			p = p->next;
			continue;
		}

		spin_unlock(&erofs_sb_list_lock);
		sbi->shrinker_run_no = run_no;

		freed += erofs_shrink_workstation(sbi, nr - freed);

		spin_lock(&erofs_sb_list_lock);
		/* Get the next list element before we move this one */
		p = p->next;

		/*
		 * Move this one to the end of the list to provide some
		 * fairness.
		 */
		list_move_tail(&sbi->list, &erofs_sb_list);
		mutex_unlock(&sbi->umount_mutex);

		if (freed >= nr)
			break;
	}
	spin_unlock(&erofs_sb_list_lock);
	return freed;
}

static struct shrinker erofs_shrinker_info = {
	.scan_objects = erofs_shrink_scan,
	.count_objects = erofs_shrink_count,
	.seeks = DEFAULT_SEEKS,
};

int __init erofs_init_shrinker(void)
{
	return register_shrinker(&erofs_shrinker_info, "erofs-shrinker");
}

void erofs_exit_shrinker(void)
{
	unregister_shrinker(&erofs_shrinker_info);
}