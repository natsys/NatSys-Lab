/**
 *		Tempesta DB
 *
 * Index and memory management for cache conscious Burst Hash Trie.
 *
 * References:
 * 1. "HAT-trie: A Cache-conscious Trie-based Data Structure for Strings",
 *    N.Askitis, R.Sinha, 2007
 * 2. "Cache-Conscious Collision Resolution in String Hash Tables",
 *    N.Askitis, J.Zobel, 2005
 *
 * The trie can store:
 * 1. variable (large) size records with pointer stability
 * 2. fixed (small) size records with pointer stability, a full cache line
 *    is utilized for each of such records regardless the actual record size
 * 3. fixed (small) size records without pointer stability, several such records
 *    can be packed into one cache line
 *
 * Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015-2022 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "kernel_mocks.h"

#include "htrie.h"

#define TDB_MAGIC	0x434947414D424454UL /* "TDBMAGIC" */

/**
 * Tempesta DB HTrie index node. This is exactly one cache line.
 *
 * Each shift in @shifts determine index of a node in file including extent
 * and/or file headers, i.e. they start from 2 or 3. The index must be
 * converted to the file offset with TDB_I2O().
 */
typedef struct {
	unsigned int	shifts[TDB_HTRIE_FANOUT];
} __attribute__((packed)) TdbHtrieNode;

/*
 * Counter for events, when the same index node references
 * the same bucket twice.
 */
static atomic_t g_burst_collision_no_mem;

static void
tdb_htrie_observe_generation(TdbHdr *dbh)
{
	atomic64_set(&this_cpu_ptr(dbh->pcpu)->generation,
		     atomic64_read(&dbh->generation));
}

static void
tdb_htrie_synchronize_generation(TdbHdr *dbh)
{
	bool synchronized;

	/* Publish a new generation. */
	unsigned long gen = atomic64_inc_return(&dbh->generation);

	/*
	 * Wait while all CPU see a generation higher than just published
	 * or do not care about the current state of the structure (i.e.
	 * declare the local maximum generation).
	 */
	do {
		int cpu;

		synchronized = true;
		for_each_online_cpu(cpu) {
			TdbPerCpu *p = per_cpu_ptr(dbh->pcpu, cpu);
			if (atomic64_read(&p->generation) <= gen) {
				synchronized = false;
				break;
			}
			cpu_relax();
		}
	} while (!synchronized);
}

static size_t
tdb_hdr_sz(TdbHdr *dbh)
{
	return sizeof(TdbHdr)
	       + sizeof(LfStack) * (dbh->rec_len ? 1 : 4);
}

static size_t
tdb_dbsz(TdbHdr *dbh)
{
	return dbh->alloc.ext_max * TDB_EXT_SZ;
}

/**
 * The root node may be larger than TDB_HTRIE_FANOUT.
 */
static TdbHtrieNode *
tdb_htrie_root(TdbHdr *dbh)
{
	return (TdbHtrieNode *)((char *)dbh + TDB_HTRIE_IALIGN(tdb_hdr_sz(dbh)));
}

static size_t
tdb_htrie_root_sz(TdbHdr *dbh)
{
	return sizeof(TdbHtrieNode) << (dbh->root_bits - TDB_HTRIE_BITS);
}

static void
tdb_htrie_init_bucket(TdbHtrieBucket *b)
{
	/*
	 * Make bsr instruction (see flz) always find the bit.
	 * This works because we never set more than TDB_HTRIE_COLL_MAX bits.
	 */
	b->col_map = 0;
	b->next = 0;
}

static size_t
tdb_htrie_bckt_sz(TdbHdr *dbh)
{
	int inplace = dbh->flags & TDB_F_INPLACE;
	size_t n = sizeof(TdbHtrieBucket);

	n += (TDB_HTRIE_COLL_MAX - TDB_HTRIE_BURST_MIN_BITS)
	     * (sizeof(TdbFRec) + dbh->rec_len * inplace);

	return n;
}

static bool
tdb_htrie_bckt_burst_threshold(unsigned long bit)
{
	return bit < TDB_HTRIE_BURST_MIN_BITS;
}

static unsigned long
tdb_htrie_alloc_index(TdbHdr *dbh)
{
	unsigned long o;
	TdbPerCpu *p = this_cpu_ptr(dbh->pcpu);

	o = tdb_alloc_fix(&dbh->alloc, sizeof(TdbHtrieNode),
			  &p->i_wcl, &p->flags);
	BUG_ON(TDB_HTRIE_IALIGN(o) != o);

	bzero_fast(TDB_PTR(dbh, o), sizeof(TdbHtrieNode));

	return o;
}

static void
tdb_htrie_rollback_index(TdbHdr *dbh)
{
	tdb_alloc_rollback(&dbh->alloc, sizeof(TdbHtrieNode),
			   &this_cpu_ptr(dbh->pcpu)->i_wcl);
}

static TdbHtrieBucket *
tdb_htrie_alloc_bucket(TdbHdr *dbh)
{
	unsigned long o;
	TdbHtrieBucket *b;
	TdbPerCpu *p = this_cpu_ptr(dbh->pcpu);

	/* Firstly check the reclamtion queue. */
	if (p->free_bckt_h) {
		b = TDB_PTR(dbh, p->free_bckt_h);
		p->free_bckt_h = b->next;
		if (!p->free_bckt_h)
			p->free_bckt_t = 0;
	} else {
		o = tdb_alloc_fix(&dbh->alloc, tdb_htrie_bckt_sz(dbh),
				  &p->b_wcl, &p->flags);
		b = TDB_PTR(dbh, o);
	}

	tdb_htrie_init_bucket(b);

	return b;
}

static void
tdb_htrie_rollback_bucket(TdbHdr *dbh)
{
	tdb_alloc_rollback(&dbh->alloc, tdb_htrie_bckt_sz(dbh),
			   &this_cpu_ptr(dbh->pcpu)->b_wcl);
}

/**
 * Reclaim the bucket memory.
 * It's guaranteed that there is no users of the bucket.
 */
static void
tdb_htrie_reclaim_bucket(TdbHdr *dbh, TdbHtrieBucket *b)
{
	TdbPerCpu *p = this_cpu_ptr(dbh->pcpu);
	TdbHtrieBucket *last;

	if (p->free_bckt_t) {
		last = TDB_PTR(dbh, p->free_bckt_t);
		last->next = TDB_OFF(dbh, b);
		p->free_bckt_t = TDB_OFF(dbh, b);
	} else {
		BUG_ON(p->free_bckt_h);
		p->free_bckt_h = TDB_OFF(dbh, b);
		p->free_bckt_t = TDB_OFF(dbh, b);
	}
}

static LfStack *
__htrie_dcache(TdbHdr *dbh, size_t sz)
{
	if (TDB_HTRIE_VARLENRECS(dbh))
		return &dbh->dcache[0];

	if (sz <= 256)
		return &dbh->dcache[0];
	if (sz <= 512)
		return &dbh->dcache[2];
	if (sz <= 1024)
		return &dbh->dcache[3];
	if (sz <= 2048)
		return &dbh->dcache[4];

	return NULL;
}

static unsigned long
tdb_htrie_alloc_data(TdbHdr *dbh, size_t *len)
{
	bool varlen = TDB_HTRIE_VARLENRECS(dbh);
	unsigned long overhead;
	TdbPerCpu *alloc_st = this_cpu_ptr(dbh->pcpu);
	LfStack *dcache;

	overhead = varlen ? sizeof(TdbVRec) : 0;
	dcache = __htrie_dcache(dbh, *len + overhead);

	if (dcache && !lfs_empty(dcache)) {
		SEntry *chunk = lfs_pop(dcache, dbh, 0);
		if (chunk)
			return TDB_OFF(dbh, chunk);
	}

	return tdb_alloc_data(&dbh->alloc, overhead, len, &alloc_st->flags,
			      &alloc_st->d_wcl);
}

static void
tdb_htrie_free_data(TdbHdr *dbh, void *addr, size_t size)
{
	LfStack *dcache = __htrie_dcache(dbh, size);

	if (dcache) {
		SEntry *e = (SEntry *)addr;
		lfs_entry_init(e);
		lfs_push(dcache, e, 0);
	} else {
		BUG_ON(size != TDB_BLK_SZ);
		tdb_free_blk(&dbh->alloc, (unsigned long)addr);
	}
}

static void
tdb_htrie_rollback_data(TdbHdr *dbh, size_t len)
{
	unsigned long overhead = TDB_HTRIE_VARLENRECS(dbh) ? sizeof(TdbVRec) : 0;

	tdb_alloc_rollback(&dbh->alloc, overhead + tdb_htrie_bckt_sz(dbh),
			   &this_cpu_ptr(dbh->pcpu)->d_wcl);
}

/**
 * Descend the the tree starting at the root.
 *
 * @retrurn byte offset of data (w/o TDB_HTRIE_DBIT bit) on success
 * or 0 if key @key was not found.
 * When function exits @node stores the last index node.
 * @bits - number of bits (from less significant to most significant) from
 * which we should start descending and the stored number of resolved bits.
 *
 * Least significant bits in our hash function have most entropy,
 * so we resolve the key from least significant bits to most significant.
 */
static unsigned long
tdb_htrie_descend(TdbHdr *dbh, unsigned long key, int *bits, TdbHtrieNode **node)
{
	unsigned long o;

	BUG_ON(*bits > 0);

	*node = tdb_htrie_root(dbh);
	o = (*node)->shifts[key & ((1 << dbh->root_bits) - 1)];

retry:
	BUG_ON(o && (TDB_I2O(o & ~TDB_HTRIE_DBIT)
		     < tdb_hdr_sz(dbh) + sizeof(TdbExt)
		     || TDB_I2O(o & ~TDB_HTRIE_DBIT) > tdb_dbsz(dbh)));

	if (o & TDB_HTRIE_DBIT) {
		/* We're at a data pointer - resolve it. */
		*bits += TDB_HTRIE_BITS;
		o ^= TDB_HTRIE_DBIT;
		BUG_ON(!o);
		return TDB_I2O(o);
	} else {
		if (!o)
			return 0; /* cannot descend deeper */
		*node = TDB_PTR(dbh, TDB_I2O(o));
		*bits += TDB_HTRIE_BITS;
	}

	BUG_ON(TDB_HTRIE_RESOLVED(*bits));

	o = (*node)->shifts[TDB_HTRIE_IDX(key, *bits)];

	goto retry;
}

static TdbRec *
tdb_htrie_create_rec(TdbHdr *dbh, unsigned long off, unsigned long key,
		     const void *data, size_t len)
{
	char *ptr = TDB_PTR(dbh, off);
	TdbRec *r = (TdbRec *)ptr;

	/* Invalid usage. */
	BUG_ON(!data && !(dbh->flags & TDB_F_INPLACE));

	if (TDB_HTRIE_VARLENRECS(dbh)) {
		TdbVRec *vr = (TdbVRec *)r;

		BUG_ON(vr->len || vr->chunk_next);

		vr->chunk_next = 0;
		vr->len = len;

		ptr += sizeof(TdbVRec);
	}
	else if (dbh->flags & TDB_F_INPLACE) {
		TdbFRec *fr = (TdbFRec *)ptr;

		BUG_ON(fr->key);
		BUG_ON(len != dbh->rec_len);

		fr->key = key;

		ptr = fr->data;
	}

	if (data)
		memcpy_fast(ptr, data, len);

	return r;
}

/**
 * Add more data to the variable-length large record @rec.
 *
 * The function is called to extend just added new record, so it's not expected
 * that it can be called concurrently for the same record.
 */
TdbVRec *
tdb_htrie_extend_rec(TdbHdr *dbh, TdbVRec *rec, size_t size)
{
	unsigned long o;
	TdbVRec *chunk;

	/* Cannot extend fixed-size records. */
	BUG_ON(!TDB_HTRIE_VARLENRECS(dbh));

	o = tdb_htrie_alloc_data(dbh, &size);
	if (!o)
		return NULL;

	chunk = TDB_PTR(dbh, o);
	chunk->chunk_next = 0;
	chunk->len = size;

retry:
	/* A caller is appreciated to pass the last record chunk by @rec. */
	while (unlikely(rec->chunk_next))
		rec = TDB_PTR(dbh, TDB_DI2O(rec->chunk_next));

	o = TDB_O2DI(o);
	if (atomic_cmpxchg((atomic_t *)&rec->chunk_next, 0, o))
		goto retry;

	return chunk;
}

static TdbRec *
__htrie_bckt_rec(TdbHtrieBucket *b, int slot)
{
	return (TdbRec *)(b + 1) + slot;
}

static int
__htrie_bckt_bit2slot(unsigned long bit)
{
	return TDB_HTRIE_COLL_MAX - bit;
}

static unsigned int
__htrie_bckt_slot2bit(int slot)
{
	return TDB_HTRIE_COLL_MAX - slot;
}

/**
 * May return a new record in @rec, but never rewrites the content.
 */
static void
__htrie_bckt_write_metadata(TdbHdr *dbh, TdbHtrieBucket *b, unsigned long key,
			    const void *data, size_t *len, int slot,
			    TdbRec **rec)
{
	if (dbh->flags & TDB_F_INPLACE) {
		unsigned long o = TDB_OFF(dbh, __htrie_bckt_rec(b, slot));
		*rec = tdb_htrie_create_rec(dbh, o, key, data, *len);
	} else {
		TdbFRec *meta = __htrie_bckt_rec(b, slot);
		meta->key = key;
		meta->off = TDB_OFF(dbh, *rec);
	}
}

/**
 * Copy @rec to bucket @b. A new slot in @b will be allocated.
 * Contact: there is only one user of @b and it has enough space.
 */
static void
__htrie_bckt_copy_metadata(TdbHdr *dbh, TdbHtrieBucket *b, TdbRec *rec)
{
	unsigned long bit = flz(b->col_map);
	int slot = __htrie_bckt_bit2slot(bit);

	BUG_ON(tdb_htrie_bckt_burst_threshold(bit));
	b->col_map |= bit;

	if (dbh->flags & TDB_F_INPLACE) {
		unsigned long o = TDB_OFF(dbh, __htrie_bckt_rec(b, slot));
		tdb_htrie_create_rec(dbh, o, rec->key, rec->data, dbh->rec_len);
	} else {
		TdbFRec *meta = __htrie_bckt_rec(b, slot);
		meta->key = rec->key;
		meta->off = rec->off;
	}
}

static int
__htrie_insert_new_bckt(TdbHdr *dbh, unsigned long key, int bits,
			TdbHtrieNode *node, const void *data, size_t *len,
			TdbRec **rec)
{
	unsigned long o;
	int i, b_link;
	TdbHtrieBucket *bckt;

	if (!(bckt = tdb_htrie_alloc_bucket(dbh)))
		return -ENOMEM;

	__htrie_bckt_write_metadata(dbh, bckt, key, data, len, 0, rec);

	/* Just allocated and unreferenced bucket with no other users. */
	bckt->col_map = TDB_HTRIE_SLOT2BIT(TDB_HTRIE_COLL_MAX);

	b_link = TDB_O2DI(TDB_OFF(dbh, bckt)) | TDB_HTRIE_DBIT;
	i = TDB_HTRIE_IDX(key, bits);
	if (atomic_cmpxchg((atomic_t *)&node->shifts[i], 0, b_link) == 0)
		return 0;

	/* Somebody already created the new index branch. */
	tdb_htrie_rollback_bucket(dbh);

	return -EAGAIN;
}

/**
 * Returns the acquired slot index.
 */
static int
__htrie_bckt_acquire_empty_slot(TdbHtrieBucket *b)
{
	unsigned long b_free;

	/*
	 * Try to acquire the empty slot and
	 * repeat if the bit is already acquired.
	 */
	do {
		b_free = flz(b->col_map);
		if (tdb_htrie_bckt_burst_threshold(b_free))
			return -1;
	} while (sync_test_and_set_bit(b_free, &b->col_map));

	return __htrie_bckt_bit2slot(b_free);
}

static int
__htrie_bckt_insert_new_rec(TdbHdr *dbh, TdbHtrieBucket *b, unsigned long key,
			    const void *data, size_t *len, int slot,
			    TdbRec **rec)
{
	int s;

	while (1) {
		/* Probably overwrite a concurrently written bucket record. */
		__htrie_bckt_write_metadata(dbh, b, key, data, len, slot, rec);

		s = __htrie_bckt_acquire_empty_slot(b);
		if (s < 0)
			return -EINVAL;
		if (slot == s)
			break;
		slot = s;
	}

	/* We won the race, so fix our metadata or a small record. */
	__htrie_bckt_write_metadata(dbh, b, key, data, len, slot, rec);

	return 0;
}

static int
__htrie_bckt_move_records(TdbHdr *dbh, TdbHtrieBucket *b, unsigned long map,
			  int bits, TdbHtrieNode *in, unsigned long *new_map,
			  bool no_mem_fail)
{
	int s, i;
	TdbRec *r;
	TdbHtrieBucket *b_new;

	/*
	 * The bucket may get new occuped slots during this loop, but never
	 * new free slots.
	 */
	for (s = 0; s < TDB_HTRIE_BCKT_SLOTS_N; ++s) {
		unsigned long bit = __htrie_bckt_slot2bit(s);

		if (!(map & bit))
			continue;

		r = __htrie_bckt_rec(b, s);
		i = TDB_HTRIE_IDX(r->key, bits);

		if (!in->shifts[i]) {
			if (!*new_map) {
				/* The first record remains in the same bucket. */
				*new_map |= bit;
				in->shifts[i] = TDB_O2DI(TDB_OFF(dbh, b))
						| TDB_HTRIE_DBIT;
			} else {
				/*
				 * We going to use at least 2 slots in the new
				 * index node, i.e. the key part creates new
				 * branches and we burst the node.
				 */
				if ((b_new = tdb_htrie_alloc_bucket(dbh))) {
					__htrie_bckt_copy_metadata(dbh, b_new, r);
				} else {
					if (!no_mem_fail)
						return -ENOMEM;
					/*
					 * We can not allocate a new bucket and
					 * the index is already fixed, so just
					 * link the index slot to the same bucket
					 * and hope that on the next bucket
					 * overflow we have memory for burst.
					 */
					b_new = b;
					atomic_inc(&g_burst_collision_no_mem);
				}
				in->shifts[i] = TDB_O2DI(TDB_OFF(dbh, b_new))
						| TDB_HTRIE_DBIT;

			}
		} else {
			/*
			 * Collision: copy the record if the index references
			 * to a new bucket or just leave everything as is.
			 */
			unsigned long o = in->shifts[i] & ~TDB_HTRIE_DBIT;
			b_new = TDB_PTR(dbh, TDB_II2O(o));
			if (b_new != b)
				__htrie_bckt_copy_metadata(dbh, b_new, r);
			else
				*new_map |= bit;
		}
	}

	return 0;
}

static int
tdb_htrie_bckt_burst(TdbHdr *dbh, TdbHtrieBucket *b, unsigned long old_off,
		     unsigned long key, int bits, TdbHtrieNode **node)
{
	int i, ret;
	TdbHtrieNode *in;
	unsigned long o, map = b->col_map, new_map = 0, curr_map;

	if (!(o = tdb_htrie_alloc_index(dbh)))
		return -ENOMEM;
	in = TDB_PTR(dbh, TDB_II2O(o));

	if (__htrie_bckt_move_records(dbh, b, map, bits, in, &new_map, false)) {
		ret = -ENOMEM;
		goto err_free_mem;
	}

	/*
	 * We have a new index node referencing the old bucket and probably
	 * several new buckets. We didn't touch the old bucket, but collected
	 * a new collision map for it - once we replace the maps, all records
	 * out of the new map are considered freed.
	 */
	i = TDB_HTRIE_IDX(key, bits - TDB_HTRIE_BITS);
	if (atomic_cmpxchg((atomic_t *)&(*node)->shifts[i],
			   old_off, o) != old_off)
	{
		ret = -EAGAIN;
		goto err_free_mem;
	}

	/*
	 * The new index is fixed, but the old bucket and the new buckets
	 * have double references to the same data.
	 *
	 * All the new readers go to the new buckets, the others may observe
	 * the old copies.
	 */
	while (1) {
		curr_map = atomic64_cmpxchg((atomic64_t *)&b->col_map, map,
					    new_map);
		if (curr_map == map)
			break;
		/* cur_map always contains map. */
		map = curr_map ^ map;
		__htrie_bckt_move_records(dbh, b, map, bits, in, &new_map, true);
		/* We applied all the new slots, retry. */
		map = curr_map;
	}

	*node = in;

	/* The new index level doesn't add any new branch, need to repeat. */
	if (new_map == map)
		return -1;
	return 0;

err_free_mem:
	/*
	 * Free all new buckets and the index node.
	 * Nobody references the buckets, so we can normally free them.
	 */
	for (i = 0; i < TDB_HTRIE_FANOUT; ++i)
		if (in->shifts[i]) {
			o = in->shifts[i] & ~TDB_HTRIE_DBIT;
			tdb_htrie_reclaim_bucket(dbh, TDB_PTR(dbh, TDB_II2O(o)));
		}
	tdb_htrie_rollback_index(dbh);
	return ret;
}

/**
 * Insert a new entry.
 * Allows duplicate key entries.
 *
  * @len returns number of copied data on success.
 *
 * @return address of the inserted record or NULL on failure.
 * Keep in mind that in case of inplace database you can use the return value
 * just to check success/failure and can not use the address because it can
 * change any time.
 */
TdbRec *
tdb_htrie_insert(TdbHdr *dbh, unsigned long key, const void *data, size_t *len)
{
	int r, slot, bits = 0;
	unsigned long o, b_free;
	TdbRec *rec = NULL;
	TdbHtrieBucket *bckt;
	TdbHtrieNode *node;

	/* Don't store empty data. */
	if (unlikely(!*len))
		return NULL;

	tdb_htrie_observe_generation(dbh);

	if (!(dbh->flags & TDB_F_INPLACE)) {
		if (!(o = tdb_htrie_alloc_data(dbh, len)))
			goto err;
		rec = tdb_htrie_create_rec(dbh, o, key, data, *len);
	}

retry:
	while (1) {
		if ((o = tdb_htrie_descend(dbh, key, &bits, &node)))
			break;
		/* The index doesn't have the key. */
		r = __htrie_insert_new_bckt(dbh, key, bits, node, data, len, &rec);
		if (!r)
			goto err;
		if (r == -ENOMEM)
			goto err_data_free;
	}

	/*
	 * HTrie collision: the index references a metadata block.
	 * At this point arbitrary new intermediate index nodes could appear.
	 */
	bckt = TDB_PTR(dbh, o);
	BUG_ON(!bckt);

	b_free = flz(bckt->col_map);
	if (!tdb_htrie_bckt_burst_threshold(b_free)) {
		slot = __htrie_bckt_bit2slot(b_free);
		if (!__htrie_bckt_insert_new_rec(dbh, bckt, key, data, len,
						 slot, &rec))
		{
			tdb_htrie_free_generation(dbh);
			return rec;
		}
	}

	/* The metadata/inplace data block is full, burst it. */

	if (unlikely(TDB_HTRIE_RESOLVED(bits)))
		goto no_space;

	/*
	 * There is no room in the bucket - burst it
	 * We should never see collision chains at this point.
	 */
	BUG_ON(bits < TDB_HTRIE_BITS);

	while (1) {
		r = tdb_htrie_bckt_burst(dbh, bckt, o, key, bits, &node);
		if (likely(!r))
			break;
		if (r == -ENOMEM)
			goto err_data_free;
		if (r == -EAGAIN)
			goto retry; /* the index has changed */
		bits += TDB_HTRIE_BITS;
		if (TDB_HTRIE_RESOLVED(bits))
			goto no_space;
	}

err_data_free:
	if (!(dbh->flags & TDB_F_INPLACE))
		tdb_htrie_rollback_data(dbh, *len);
err:
	tdb_htrie_free_generation(dbh);

	return NULL;
no_space:
	TDB_ERR("All bits of key %#lx and the collision bucket is full"
		" - there is no space to insert a new record\n", key);
	goto err_data_free;
}

/**
 * Lookup an entry with the @key.
 * The HTrie may contain collisions for the same key (actually not only
 * collosions, but also full duplicates), so it returns a bucket handler for
 * a current generation and it's the caller responsibility to call
 * tdb_htrie_free_generation() when they're done with the bucket
 * (collision chain).
 *
 * TODO rework for TDB_F_INPLACE and metadata layer.
 */
TdbHtrieBucket *
tdb_htrie_lookup(TdbHdr *dbh, unsigned long key)
{
	int bits = 0;
	unsigned long o;
	TdbHtrieNode *node;

	tdb_htrie_observe_generation(dbh);

	o = tdb_htrie_descend(dbh, key, &bits, &node);
	if (!o) {
		tdb_htrie_free_generation(dbh);
		return NULL;
	}

	return TDB_PTR(dbh, o);
}

/**
 * Iterate over all records in a bucket (collision chain) under the generation
 * guard. May return TdbFRec or TdbVRec depeding on the database type.
 *
 * @return @i as index of returned record, so increment the index beween the
 * calls to iterate over the bucket.
 */
void *
tdb_htrie_bscan_for_rec(TdbHdr *dbh, TdbHtrieBucket *b, unsigned long key, int *i)
{
	TdbRec *r;

	for ( ; *i < TDB_HTRIE_BCKT_SLOTS_N; ++*i) {
		if (!(b->col_map & __htrie_bckt_slot2bit(*i)))
			continue;
		r = __htrie_bckt_rec(b, *i);
		if (r->key == key) {
			if (dbh->flags & TDB_F_INPLACE)
				return r;
			return TDB_PTR(dbh, r->off);
		}
	}

	return NULL;
}

static int
tdb_htrie_bucket_walk(TdbHdr *dbh, TdbHtrieBucket *b, int (*fn)(void *))
{
	int i, res;
	TdbRec *r;

	for (i = 0; i < TDB_HTRIE_BCKT_SLOTS_N; ++i) {
		if (!(b->col_map & __htrie_bckt_slot2bit(i)))
			continue;
		r = __htrie_bckt_rec(b, i);

		if (dbh->flags & TDB_F_INPLACE) {
			if (unlikely(res = fn(r->data)))
				return res;
		} else {
			TdbVRec *vr = TDB_PTR(dbh, r->off);
			if (unlikely(res = fn(vr->data)))
				return res;
		}
	}

	return 0;
}

static int
tdb_htrie_node_visit(TdbHdr *dbh, TdbHtrieNode *node, int (*fn)(void *))
{
	int bits, res, fanout;

	fanout = (node == tdb_htrie_root(dbh))
		 ? (1 << dbh->root_bits)
		 : TDB_HTRIE_FANOUT;

	for (bits = 0; bits < fanout; ++bits) {
		unsigned long o;

		BUG_ON(TDB_HTRIE_RESOLVED(bits));

		o = node->shifts[bits];

		if (likely(!o))
			continue;

		BUG_ON(TDB_DI2O(o & ~TDB_HTRIE_DBIT)
		       < tdb_hdr_sz(dbh) + sizeof(TdbExt)
		       || TDB_DI2O(o & ~TDB_HTRIE_DBIT) > tdb_dbsz(dbh));

		if (o & TDB_HTRIE_DBIT) {
			TdbHtrieBucket *b;

			/* We're at a data pointer - resolve it. */
			o ^= TDB_HTRIE_DBIT;
			BUG_ON(!o);

			b = (TdbHtrieBucket *)TDB_PTR(dbh, TDB_DI2O(o));
			res = tdb_htrie_bucket_walk(dbh, b, fn);
			if (unlikely(res))
				return res;
		} else {
			/*
			 * The recursion depth being hard-limited.
			 * The function has the deepest nesting 16.
			 */
			res = tdb_htrie_node_visit(dbh, TDB_PTR(dbh,
						   TDB_II2O(o)), fn);
			if (unlikely(res))
				return res;
		}
	}

	return 0;
}

int
tdb_htrie_walk(TdbHdr *dbh, int (*fn)(void *))
{
	TdbHtrieNode *node = tdb_htrie_root(dbh);

	return tdb_htrie_node_visit(dbh, node, fn);
}

/**
 * Remvoe all entries with the key and shrink the trie.
 *
 * We never remove the index blocks. However, the buckets can be up to 1 page
 * size, so we reclaim them.
 */
void
tdb_htrie_remove(TdbHdr *dbh, unsigned long key)
{
	unsigned long o, new_off;
	int bits = 0, i, dr = 0;
	TdbRec *r, *data_reclaim[TDB_HTRIE_BCKT_SLOTS_N];
	TdbHtrieBucket *b, *b_new;
	TdbHtrieNode *node;

	if (!(b_new = tdb_htrie_alloc_bucket(dbh)))
		return;
	new_off = TDB_OFF(dbh, b_new);

retry:
	o = tdb_htrie_descend(dbh, key, &bits, &node);
	if (!o)
		goto err_free;
	b = TDB_PTR(dbh, o);
	BUG_ON(!b);

	/*
	 * Unlink all data (remove).
	 * Inserters (bursting function in particular) rely on the fact that
	 * records are never freed and the collision map never gets zero bits,
	 * so we need to copy the bucket node.
	 */
	for (dr = 0, i = 0; i < TDB_HTRIE_BCKT_SLOTS_N; ++i) {
		if (!(b->col_map & __htrie_bckt_slot2bit(i)))
			continue;
		r = __htrie_bckt_rec(b, i);

		if (r->key != key)
			__htrie_bckt_copy_metadata(dbh, b_new, r);
		else
			data_reclaim[dr++] = r;
	}

	i = TDB_HTRIE_IDX(key, bits - TDB_HTRIE_BITS);
	if (atomic_cmpxchg((atomic_t *)&node->shifts[i], o, new_off) != o) {
		tdb_htrie_init_bucket(b_new);
		goto retry;
	}

	/*
	 * Index to the new bucket referencing subset of the data of the original
	 * bucket is published. Increment the generation and wait while all
	 * observers see genrations higher that the current one.
	 */
	tdb_htrie_synchronize_generation(dbh);

	/*
	 * Now all the CPU have observed our index changes and we can
	 * reclaim the memory.
	 */
	tdb_htrie_reclaim_bucket(dbh, b);
	if (dbh->flags & TDB_F_INPLACE)
		return;
	for (i = 0; i < dr; ++i) {
		r = data_reclaim[i];
		if (TDB_HTRIE_VARLENRECS(dbh)) {
			TdbVRec *vr = (TdbVRec *)TDB_PTR(dbh, r->off);
			while (1) {
				o = vr->chunk_next;
				tdb_htrie_free_data(dbh, vr, vr->len);
				if (!o)
					break;
				vr = (TdbVRec *)TDB_PTR(dbh, o);
			}
		} else {
			tdb_htrie_free_data(dbh, TDB_PTR(dbh, r->off),
					    dbh->rec_len);
		}
	}
	return;
err_free:
	tdb_htrie_reclaim_bucket(dbh, b_new);
}

static TdbHdr *
tdb_init_mapping(void *p, size_t db_size, size_t root_bits, unsigned int rec_len,
		 unsigned int flags)
{
	int b, cpu;
	TdbHdr *dbh = (TdbHdr *)p;

	if (db_size > TDB_MAX_SHARD_SZ) {
		/*
		 * TODO #400 initialize NUMA-aware shards consisting an
		 * HTrie forest. There should be separate instances of TdbAlloc
		 * for each 128GB chunk.
		 */
		TDB_ERR("too large database size (%lu)", db_size);
		return NULL;
	}
	/* Use variable-size records for large data to store. */
	if (rec_len > TDB_BLK_SZ / 2) {
		TDB_ERR("too large record length (%u)\n", rec_len);
		return NULL;
	}
	if ((root_bits & ~TDB_HTRIE_BITS) || (root_bits < TDB_HTRIE_BITS)) {
		TDB_ERR("The root node bits size must be a power of 4\n");
		return NULL;
	}

	dbh->magic = TDB_MAGIC;
	dbh->flags = flags;
	dbh->rec_len = rec_len;
	dbh->root_bits = root_bits;

	atomic64_set(&dbh->generation, 0);

	memset(tdb_htrie_root(dbh), 0, tdb_htrie_root_sz(dbh));

	tdb_alloc_init(&dbh->alloc,
		       TDB_HTRIE_IALIGN(tdb_hdr_sz(dbh)) + tdb_htrie_root_sz(dbh),
		       db_size);

	lfs_init(&dbh->dcache[0]);
	if (TDB_HTRIE_VARLENRECS(dbh)) {
		/*
		 * Caches for the data chunks of: 256B, 512B, 1KB, 2KB.
		 * 4KB chunks (blocks) are returned to the block allocator.
		 */
		lfs_init(&dbh->dcache[1]);
		lfs_init(&dbh->dcache[2]);
		lfs_init(&dbh->dcache[3]);
	}

	if ((flags & TDB_F_INPLACE)) {
		if (!rec_len) {
			TDB_ERR("Inplace data is possible for small records"
				" only\n");
			return NULL;
		}
		if (tdb_htrie_bckt_sz(dbh) > TDB_BLK_SZ) {
			TDB_ERR("Inplace data record is too big to be inplace."
				" Get rid of inplace requirement or reduce the"
				" number of collisions before bursting a"
				" bucket.\n");
			return NULL;
		}
	}

	/* Set per-CPU pointers. */
	dbh->pcpu = alloc_percpu(TdbPerCpu);
	if (!dbh->pcpu) {
		TDB_ERR("cannot allocate per-cpu data\n");
		return NULL;
	}
	for_each_online_cpu(cpu) {
		TdbPerCpu *p = per_cpu_ptr(dbh->pcpu, cpu);
		TdbAlloc *a = &dbh->alloc;

		p->flags = 0;
		atomic64_set(&p->generation, LONG_MAX);
		p->i_wcl = tdb_alloc_blk(a, TDB_EXT_BAD, false, &p->flags);
		p->b_wcl = tdb_alloc_blk(a, TDB_EXT_BAD, false, &p->flags);
		// TODO data-less DB for small recs & inplace, we must not
		// have allocations from data area.
		p->d_wcl = tdb_alloc_blk(a, TDB_EXT_BAD,
					 TDB_HTRIE_VARLENRECS(dbh), &p->flags);
		BUG_ON(!p->i_wcl || !p->b_wcl || !p->d_wcl);
		/*
		 * TODO place the per-cpu data in the raw memory
		 * to dump it to the disk.
		 */
		p->free_bckt_h = p->free_bckt_t = 0;
	}

	return dbh;
}

/**
 * TODO #516 create multiple indexes of the same structure, but different keys.
 *
 * TODO #400 dtatbabase shards should be addressed by a good hash function.
 * Range queries must be run over all the shards.
 */
TdbHdr *
tdb_htrie_init(void *p, size_t db_size, size_t root_bits, unsigned int rec_len,
	       unsigned int flags)
{
	int cpu;
	TdbHdr *hdr = (TdbHdr *)p;

	BUILD_BUG_ON(TDB_HTRIE_COLL_MAX > BITS_PER_LONG - 1);

	if (hdr->magic != TDB_MAGIC) {
		hdr = tdb_init_mapping(p, db_size, root_bits, rec_len, flags);
		if (!hdr) {
			TDB_ERR("cannot init db mapping\n");
			return NULL;
		}
	}

	return hdr;
}

void
tdb_htrie_exit(TdbHdr *dbh)
{
	free_percpu(dbh->pcpu);
}
