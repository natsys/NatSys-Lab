/**
 *		Tempesta DB
 *
 * Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015-2022 Tempesta Technologies.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#ifndef __HTRIE_H__
#define __HTRIE_H__

#ifndef __KERNEL__
#include "kernel_mocks.h"
#include "atomic.h"
#include "rwlock.h"
#endif

#include "alloc.h"
#include "tdb.h"

#define TDB_ITER_BAD(i)		(!(i).rec)

/**
 * We use very small index nodes size of only one cache line.
 * So overall memory footprint of the index is mininal by a cost of more LLC
 * or main memory transfers. However, smaller memory usage means better TLB
 * utilization on huge worksets.
 */
#define TDB_HTRIE_NODE_SZ	L1_CACHE_BYTES
/*
 * There is no sense to allocate a new resolving node for each new small
 * (less than cache line size) data record. So we place small records in
 * 2 cache lines in sequential order and burst the node only when there
 * is no room.
 */
#define TDB_HTRIE_MINDREC	(L1_CACHE_BYTES * 2)

/* Get index and data block indexes by byte offset and vise versa. */
#define TDB_O2DI(o)		((o) / TDB_HTRIE_MINDREC)
#define TDB_O2II(o)		((o) / TDB_HTRIE_NODE_SZ)
#define TDB_DI2O(i)		((i) * TDB_HTRIE_MINDREC)
#define TDB_II2O(i)		((i) * TDB_HTRIE_NODE_SZ)

/* True if the tree keeps variable length records. */
#define TDB_HTRIE_VARLENRECS(h)	(!(h)->rec_len)
/* Each record in the tree must be at least 8-byte aligned. */
#define TDB_HTRIE_RALIGN(n)	(((unsigned long)(n) + 7) & ~7UL)
#define TDB_HTRIE_IALIGN(n)	(((n) + L1_CACHE_BYTES - 1) &		\
				 ~(L1_CACHE_BYTES - 1))
#define TDB_HTRIE_DMASK		(~(TDB_HTRIE_MINDREC - 1))
#define TDB_HTRIE_DALIGN(n)	(((n) + TDB_HTRIE_MINDREC - 1)		\
				 & TDB_HTRIE_DMASK)
#define TDB_HTRIE_BITS		4
#define TDB_HTRIE_FANOUT	(1 << TDB_HTRIE_BITS)
#define TDB_HTRIE_KMASK		(TDB_HTRIE_FANOUT - 1) /* key mask */
#define TDB_HTRIE_RESOLVED(b)	((b) + TDB_HTRIE_BITS > BITS_PER_LONG)
/*
 * We use 31 bits to address index and data blocks. The most significant bit
 * is used to flag data pointer/offset. Index blocks are addressed by index
 * of a L1_CACHE_BYTES-byte blocks in he file, while data blocks are
 * addressed by indexes of TDB_HTRIE_MINDREC blocks.
 *
 * So the maximum size of one database table is 128GB per processor package,
 * which is 1/3 of supported per-socket RAM by modern x86-64.
 */
#define TDB_HTRIE_DBIT		(1U << (sizeof(int) * 8 - 1))
#define TDB_HTRIE_OMASK		(TDB_HTRIE_DBIT - 1) /* offset mask */
#define TDB_HTRIE_IDX(k, b)	(((k) >> (b)) & TDB_HTRIE_KMASK)

/**
 * Header for data bucket.
 *
 * @coll_next	- next record offset (in data blocks) in collision chain;
 */
typedef struct {
	unsigned int 	coll_next;
	unsigned int	flags;
	rwlock_t	lock;
} __attribute__((packed)) TdbBucket;

#define TDB_HTRIE_VRFREED	TDB_HTRIE_DBIT
#define TDB_HTRIE_VRLEN(r)	((r)->len & ~TDB_HTRIE_VRFREED)
#define TDB_HTRIE_RBODYLEN(h, r)	((h)->rec_len ? : 		\
					 TDB_HTRIE_VRLEN((TdbVRec *)r))
/* Be careful to not to use it with TdbRec. */
#define TDB_HTRIE_RECLEN(h, r)						\
	TDB_HTRIE_RALIGN(sizeof(*(r))					\
			 + __builtin_choose_expr(			\
				__builtin_types_compatible_p(typeof(*(r)),\
							     TdbVRec),	\
				/* The type cast is only to avoid compiler \
				   error, @r is always TdbVRec here. */	\
				TDB_HTRIE_VRLEN((TdbVRec *)r),		\
				(h)->rec_len))
#define TDB_HTRIE_BCKT_1ST_REC(b) ((void *)((b) + 1))
#define TDB_HTRIE_BUCKET_KEY(b)	(*(unsigned long *)TDB_HTRIE_BCKT_1ST_REC(b))
/* Iterate over buckets in collision chain. */
#define TDB_HTRIE_BUCKET_NEXT(h, b) ((b)->coll_next			\
				     ? TDB_PTR(h, TDB_DI2O((b)->coll_next))\
				     : NULL)				\

#define TDB_HDR_SZ(h)							\
	(sizeof(TdbHdr) + TDB_EXT_BMP_2L(h) * sizeof(long))
#define TDB_HTRIE_ROOT(h)						\
	(TdbHtrieNode *)((char *)(h) + TDB_HDR_SZ(h) + sizeof(TdbExt))

/* FIXME we can't store zero bytes by zero key. */
static inline int
tdb_live_fsrec(TdbHdr *dbh, TdbFRec *rec)
{
	int i, res = 0;
	size_t len = TDB_HTRIE_RALIGN(sizeof(*rec) + dbh->rec_len)
		     / sizeof(long);

	for (i = 0; i < len; ++i)
		res |= !!((unsigned long *)rec)[i];
	return res;
}

static inline int
tdb_live_vsrec(TdbVRec *rec)
{
	return rec->len && !(rec->len & TDB_HTRIE_VRFREED);
}

static inline int
tdb_live_rec(TdbHdr *dbh, TdbRec *r)
{
	return TDB_HTRIE_VARLENRECS(dbh)
	       ? tdb_live_vsrec((TdbVRec *)r)
	       : tdb_live_fsrec(dbh, (TdbFRec *)r);
}

EXTERN_C TdbVRec *tdb_htrie_extend_rec(TdbHdr *dbh, TdbVRec *rec, size_t size);
EXTERN_C TdbRec *tdb_htrie_insert(TdbHdr *dbh, unsigned long key,
				  const void *data, size_t *len);
EXTERN_C TdbBucket *tdb_htrie_lookup(TdbHdr *dbh, unsigned long key);
EXTERN_C void tdb_htrie_remove(TdbHdr *dbh, unsigned long key);
EXTERN_C TdbRec *tdb_htrie_bscan_for_rec(TdbHdr *dbh, TdbBucket **b,
					 unsigned long key);
EXTERN_C TdbRec *tdb_htrie_next_rec(TdbHdr *dbh, TdbRec *r, TdbBucket **b,
				    unsigned long key);
EXTERN_C TdbHdr *tdb_htrie_init(void *p, size_t db_size,
				unsigned int rec_len);
EXTERN_C void tdb_htrie_exit(TdbHdr *dbh);

#endif /* __HTRIE_H__ */
