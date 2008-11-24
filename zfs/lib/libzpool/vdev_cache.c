/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"@(#)vdev_cache.c	1.7	08/01/10 SMI"

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/vdev_impl.h>
#include <sys/zio.h>
#include <sys/kstat.h>

/*
 * Virtual device read-ahead caching.
 *
 * This file implements a simple LRU read-ahead cache.  When the DMU reads
 * a given block, it will often want other, nearby blocks soon thereafter.
 * We take advantage of this by reading a larger disk region and caching
 * the result.  In the best case, this can turn 128 back-to-back 512-byte
 * reads into a single 64k read followed by 127 cache hits; this reduces
 * latency dramatically.  In the worst case, it can turn an isolated 512-byte
 * read into a 64k read, which doesn't affect latency all that much but is
 * terribly wasteful of bandwidth.  A more intelligent version of the cache
 * could keep track of access patterns and not do read-ahead unless it sees
 * at least two temporally close I/Os to the same region.  Currently, only
 * metadata I/O is inflated.  A futher enhancement could take advantage of
 * more semantic information about the I/O.  And it could use something
 * faster than an AVL tree; that was chosen solely for convenience.
 *
 * There are five cache operations: allocate, fill, read, write, evict.
 *
 * (1) Allocate.  This reserves a cache entry for the specified region.
 *     We separate the allocate and fill operations so that multiple threads
 *     don't generate I/O for the same cache miss.
 *
 * (2) Fill.  When the I/O for a cache miss completes, the fill routine
 *     places the data in the previously allocated cache entry.
 *
 * (3) Read.  Read data from the cache.
 *
 * (4) Write.  Update cache contents after write completion.
 *
 * (5) Evict.  When allocating a new entry, we evict the oldest (LRU) entry
 *     if the total cache size exceeds zfs_vdev_cache_size.
 */

/*
 * These tunables are for performance analysis.
 */
/*
 * All i/os smaller than zfs_vdev_cache_max will be turned into
 * 1<<zfs_vdev_cache_bshift byte reads by the vdev_cache (aka software
 * track buffer).  At most zfs_vdev_cache_size bytes will be kept in each
 * vdev's vdev_cache.
 */
int zfs_vdev_cache_max = 1<<14;			/* 16KB */
int zfs_vdev_cache_size = 10ULL << 20;		/* 10MB */
int zfs_vdev_cache_bshift = 16;

#define	VCBS (1 << zfs_vdev_cache_bshift)	/* 64KB */

kstat_t	*vdc_ksp = NULL;

typedef struct vdc_stats {
	kstat_named_t vdc_stat_delegations;
	kstat_named_t vdc_stat_hits;
	kstat_named_t vdc_stat_misses;
} vdc_stats_t;

static vdc_stats_t vdc_stats = {
	{ "delegations",	KSTAT_DATA_UINT64 },
	{ "hits",		KSTAT_DATA_UINT64 },
	{ "misses",		KSTAT_DATA_UINT64 }
};

#define	VDCSTAT_BUMP(stat)	atomic_add_64(&vdc_stats.stat.value.ui64, 1);

static int
vdev_cache_offset_compare(const void *a1, const void *a2)
{
	const vdev_cache_entry_t *ve1 = a1;
	const vdev_cache_entry_t *ve2 = a2;

	if (ve1->ve_offset < ve2->ve_offset)
		return (-1);
	if (ve1->ve_offset > ve2->ve_offset)
		return (1);
	return (0);
}

static int
vdev_cache_lastused_compare(const void *a1, const void *a2)
{
	const vdev_cache_entry_t *ve1 = a1;
	const vdev_cache_entry_t *ve2 = a2;

	if (ve1->ve_lastused < ve2->ve_lastused)
		return (-1);
	if (ve1->ve_lastused > ve2->ve_lastused)
		return (1);

	/*
	 * Among equally old entries, sort by offset to ensure uniqueness.
	 */
	return (vdev_cache_offset_compare(a1, a2));
}

/*
 * Evict the specified entry from the cache.
 */
static void
vdev_cache_evict(vdev_cache_t *vc, vdev_cache_entry_t *ve)
{
	ASSERT(MUTEX_HELD(&vc->vc_lock));
	ASSERT(ve->ve_fill_io == NULL);
	ASSERT(ve->ve_data != NULL);

	dprintf("evicting %p, off %llx, LRU %llu, age %lu, hits %u, stale %u\n",
	    vc, ve->ve_offset, ve->ve_lastused, lbolt - ve->ve_lastused,
	    ve->ve_hits, ve->ve_missed_update);

	avl_remove(&vc->vc_lastused_tree, ve);
	avl_remove(&vc->vc_offset_tree, ve);
	zio_buf_free(ve->ve_data, VCBS);
	kmem_free(ve, sizeof (vdev_cache_entry_t));
}

/*
 * Allocate an entry in the cache.  At the point we don't have the data,
 * we're just creating a placeholder so that multiple threads don't all
 * go off and read the same blocks.
 */
static vdev_cache_entry_t *
vdev_cache_allocate(zio_t *zio)
{
	vdev_cache_t *vc = &zio->io_vd->vdev_cache;
	uint64_t offset = P2ALIGN(zio->io_offset, VCBS);
	vdev_cache_entry_t *ve;

	ASSERT(MUTEX_HELD(&vc->vc_lock));

	if (zfs_vdev_cache_size == 0)
		return (NULL);

	/*
	 * If adding a new entry would exceed the cache size,
	 * evict the oldest entry (LRU).
	 */
	if ((avl_numnodes(&vc->vc_lastused_tree) << zfs_vdev_cache_bshift) >
	    zfs_vdev_cache_size) {
		ve = avl_first(&vc->vc_lastused_tree);
		if (ve->ve_fill_io != NULL) {
			dprintf("can't evict in %p, still filling\n", vc);
			return (NULL);
		}
		ASSERT(ve->ve_hits != 0);
		vdev_cache_evict(vc, ve);
	}

	ve = kmem_zalloc(sizeof (vdev_cache_entry_t), KM_SLEEP);
	ve->ve_offset = offset;
	ve->ve_lastused = lbolt;
	ve->ve_data = zio_buf_alloc(VCBS);

	avl_add(&vc->vc_offset_tree, ve);
	avl_add(&vc->vc_lastused_tree, ve);

	return (ve);
}

static void
vdev_cache_hit(vdev_cache_t *vc, vdev_cache_entry_t *ve, zio_t *zio)
{
	uint64_t cache_phase = P2PHASE(zio->io_offset, VCBS);

	ASSERT(MUTEX_HELD(&vc->vc_lock));
	ASSERT(ve->ve_fill_io == NULL);

	if (ve->ve_lastused != lbolt) {
		avl_remove(&vc->vc_lastused_tree, ve);
		ve->ve_lastused = lbolt;
		avl_add(&vc->vc_lastused_tree, ve);
	}

	ve->ve_hits++;
	bcopy(ve->ve_data + cache_phase, zio->io_data, zio->io_size);
}

/*
 * Fill a previously allocated cache entry with data.
 */
static void
vdev_cache_fill(zio_t *zio)
{
	vdev_t *vd = zio->io_vd;
	vdev_cache_t *vc = &vd->vdev_cache;
	vdev_cache_entry_t *ve = zio->io_private;
	zio_t *dio;

	ASSERT(zio->io_size == VCBS);

	/*
	 * Add data to the cache.
	 */
	mutex_enter(&vc->vc_lock);

	ASSERT(ve->ve_fill_io == zio);
	ASSERT(ve->ve_offset == zio->io_offset);
	ASSERT(ve->ve_data == zio->io_data);

	ve->ve_fill_io = NULL;

	/*
	 * Even if this cache line was invalidated by a missed write update,
	 * any reads that were queued up before the missed update are still
	 * valid, so we can satisfy them from this line before we evict it.
	 */
	for (dio = zio->io_delegate_list; dio; dio = dio->io_delegate_next)
		vdev_cache_hit(vc, ve, dio);

	if (zio->io_error || ve->ve_missed_update)
		vdev_cache_evict(vc, ve);

	mutex_exit(&vc->vc_lock);

	while ((dio = zio->io_delegate_list) != NULL) {
		zio->io_delegate_list = dio->io_delegate_next;
		dio->io_delegate_next = NULL;
		dio->io_error = zio->io_error;
		zio_execute(dio);
	}
}

/*
 * Read data from the cache.  Returns 0 on cache hit, errno on a miss.
 */
int
vdev_cache_read(zio_t *zio)
{
	vdev_cache_t *vc = &zio->io_vd->vdev_cache;
	vdev_cache_entry_t *ve, ve_search;
	uint64_t cache_offset = P2ALIGN(zio->io_offset, VCBS);
	uint64_t cache_phase = P2PHASE(zio->io_offset, VCBS);
	zio_t *fio;

	ASSERT(zio->io_type == ZIO_TYPE_READ);

	if (zio->io_flags & ZIO_FLAG_DONT_CACHE)
		return (EINVAL);

	if (zio->io_size > zfs_vdev_cache_max)
		return (EOVERFLOW);

	/*
	 * If the I/O straddles two or more cache blocks, don't cache it.
	 */
	if (P2CROSS(zio->io_offset, zio->io_offset + zio->io_size - 1, VCBS))
		return (EXDEV);

	ASSERT(cache_phase + zio->io_size <= VCBS);

	mutex_enter(&vc->vc_lock);

	ve_search.ve_offset = cache_offset;
	ve = avl_find(&vc->vc_offset_tree, &ve_search, NULL);

	if (ve != NULL) {
		if (ve->ve_missed_update) {
			mutex_exit(&vc->vc_lock);
			return (ESTALE);
		}

		if ((fio = ve->ve_fill_io) != NULL) {
			zio->io_delegate_next = fio->io_delegate_list;
			fio->io_delegate_list = zio;
			zio_vdev_io_bypass(zio);
			mutex_exit(&vc->vc_lock);
			VDCSTAT_BUMP(vdc_stat_delegations);
			return (0);
		}

		vdev_cache_hit(vc, ve, zio);
		zio_vdev_io_bypass(zio);

		mutex_exit(&vc->vc_lock);
		zio_execute(zio);
		VDCSTAT_BUMP(vdc_stat_hits);
		return (0);
	}

	ve = vdev_cache_allocate(zio);

	if (ve == NULL) {
		mutex_exit(&vc->vc_lock);
		return (ENOMEM);
	}

	fio = zio_vdev_child_io(zio, NULL, zio->io_vd, cache_offset,
	    ve->ve_data, VCBS, ZIO_TYPE_READ, ZIO_PRIORITY_CACHE_FILL,
	    ZIO_FLAG_DONT_CACHE | ZIO_FLAG_DONT_PROPAGATE |
	    ZIO_FLAG_DONT_RETRY | ZIO_FLAG_NOBOOKMARK,
	    vdev_cache_fill, ve);

	ve->ve_fill_io = fio;
	fio->io_delegate_list = zio;
	zio_vdev_io_bypass(zio);

	mutex_exit(&vc->vc_lock);
	zio_nowait(fio);
	VDCSTAT_BUMP(vdc_stat_misses);

	return (0);
}

/*
 * Update cache contents upon write completion.
 */
void
vdev_cache_write(zio_t *zio)
{
	vdev_cache_t *vc = &zio->io_vd->vdev_cache;
	vdev_cache_entry_t *ve, ve_search;
	uint64_t io_start = zio->io_offset;
	uint64_t io_end = io_start + zio->io_size;
	uint64_t min_offset = P2ALIGN(io_start, VCBS);
	uint64_t max_offset = P2ROUNDUP(io_end, VCBS);
	avl_index_t where;

	ASSERT(zio->io_type == ZIO_TYPE_WRITE);

	mutex_enter(&vc->vc_lock);

	ve_search.ve_offset = min_offset;
	ve = avl_find(&vc->vc_offset_tree, &ve_search, &where);

	if (ve == NULL)
		ve = avl_nearest(&vc->vc_offset_tree, where, AVL_AFTER);

	while (ve != NULL && ve->ve_offset < max_offset) {
		uint64_t start = MAX(ve->ve_offset, io_start);
		uint64_t end = MIN(ve->ve_offset + VCBS, io_end);

		if (ve->ve_fill_io != NULL) {
			ve->ve_missed_update = 1;
		} else {
			bcopy((char *)zio->io_data + start - io_start,
			    ve->ve_data + start - ve->ve_offset, end - start);
		}
		ve = AVL_NEXT(&vc->vc_offset_tree, ve);
	}
	mutex_exit(&vc->vc_lock);
}

void
vdev_cache_purge(vdev_t *vd)
{
	vdev_cache_t *vc = &vd->vdev_cache;
	vdev_cache_entry_t *ve;

	mutex_enter(&vc->vc_lock);
	while ((ve = avl_first(&vc->vc_offset_tree)) != NULL)
		vdev_cache_evict(vc, ve);
	mutex_exit(&vc->vc_lock);
}

void
vdev_cache_init(vdev_t *vd)
{
	vdev_cache_t *vc = &vd->vdev_cache;

	mutex_init(&vc->vc_lock, NULL, MUTEX_DEFAULT, NULL);

	avl_create(&vc->vc_offset_tree, vdev_cache_offset_compare,
	    sizeof (vdev_cache_entry_t),
	    offsetof(struct vdev_cache_entry, ve_offset_node));

	avl_create(&vc->vc_lastused_tree, vdev_cache_lastused_compare,
	    sizeof (vdev_cache_entry_t),
	    offsetof(struct vdev_cache_entry, ve_lastused_node));
}

void
vdev_cache_fini(vdev_t *vd)
{
	vdev_cache_t *vc = &vd->vdev_cache;

	vdev_cache_purge(vd);

	avl_destroy(&vc->vc_offset_tree);
	avl_destroy(&vc->vc_lastused_tree);

	mutex_destroy(&vc->vc_lock);
}

void
vdev_cache_stat_init(void)
{
	vdc_ksp = kstat_create("zfs", 0, "vdev_cache_stats", "misc",
	    KSTAT_TYPE_NAMED, sizeof (vdc_stats) / sizeof (kstat_named_t),
	    KSTAT_FLAG_VIRTUAL);
	if (vdc_ksp != NULL) {
		vdc_ksp->ks_data = &vdc_stats;
		kstat_install(vdc_ksp);
	}
}

void
vdev_cache_stat_fini(void)
{
	if (vdc_ksp != NULL) {
		kstat_delete(vdc_ksp);
		vdc_ksp = NULL;
	}
}