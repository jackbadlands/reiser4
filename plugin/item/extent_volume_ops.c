/*
  Copyright (c) 2017-2020 Eduard O. Shishkin

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include "item.h"
#include "../../inode.h"
#include "../../page_cache.h"
#include "../object.h"
#include "../volume/volume.h"

#define MIGRATION_GRANULARITY (8192)

jnode *do_jget(struct page *pg);
int try_merge_with_right_item(coord_t *left);
int try_merge_with_left_item(coord_t *right);
int split_extent_unit(coord_t *coord, reiser4_block_nr pos, int adv_to_right);
int update_item_key(coord_t *target, const reiser4_key *key);

/*
 * primitive migration operations over item
 */
enum migration_primitive_id {
	INVALID_ACTION = 0,
	MIGRATE_EXTENT = 1,
	SPLIT_EXTENT = 2,
	SKIP_EXTENT = 3
};

struct extent_migrate_context {
	enum migration_primitive_id act;
	struct page **pages;
	int nr_pages;
	coord_t *coord;
	reiser4_key *key; /* key of extent item to be migrated */
	struct inode *inode;
	u32 new_loc;
	loff_t stop_off; /* offset of the leftmost byte to be migrated
			    in the iteration */
	loff_t done_off; /* offset of the latest byte migrated in the
			    iteration */
	reiser4_block_nr blocks_migrated;
	reiser4_block_nr unit_split_pos; /* position in unit */
	lock_handle *lh;
	unsigned int migrate_whole_item:1;
	unsigned int stop:1;
};

struct extent_ra_ctx {
	const coord_t *coord;
	reiser4_extent *ext;
	reiser4_block_nr off;
};

/**
 * read page pointed out by extent item
 */
static int filler(void *data, struct page *page)
{
	struct extent_ra_ctx *ra_ctx = data;

	return __reiser4_readpage_extent(ra_ctx->coord, ra_ctx->ext,
					 ra_ctx->off, page);
}

/**
 * read all pages pointed out by extent unit @ext starting from @off
 * @idx: index of the first page pointed out by extent unit
 */
static int readpages_extent_unit(const coord_t *coord, reiser4_extent *ext,
				 reiser4_block_nr off_in_unit,
				 struct address_space *mapping, pgoff_t idx,
				 struct page **pages, int off_in_pages)
{
	int i;
	int ret;
	struct extent_ra_ctx ra_ctx;
	int nr_pages = extent_get_width(ext) - off_in_unit;

	ra_ctx.coord = coord;
	ra_ctx.ext = ext;
	ra_ctx.off = off_in_unit;

	for (i = 0; i < nr_pages; i++) {
		struct page *page;
		page = read_cache_page(mapping, idx + off_in_unit + i,
				       filler, &ra_ctx);
		if (IS_ERR(page)) {
			ret = PTR_ERR(page);
			nr_pages = i;
			goto error;
		}
		pages[i + off_in_pages] = page;
		ra_ctx.off ++;
	}
	return 0;
 error:
	for (i = 0; i < nr_pages; i++)
		put_page(pages[i + off_in_pages]);
	return ret;
}

/**
 * read and pin pages pointed out by extent item at @coord
 * starting from offset @off
 */
static int readpages_extent_item(const coord_t *coord, loff_t off,
				 struct address_space *mapping,
				 struct extent_migrate_context *mctx)
{
	int i;
	int ret;
	int nr_pages = 0;
	reiser4_key key; /* key of the first unit to read from */
	coord_t iter_coord;
	reiser4_block_nr pos_in_unit;
	pgoff_t idx;

	coord_dup(&iter_coord, coord);
	unit_key_by_coord(coord, &key);

	pos_in_unit = 0;
	if (get_key_offset(&key) < off) {
		/*
		 * read from the middle of unit
		 */
		pos_in_unit = mctx->unit_split_pos;
		assert("edward-2403",
		       pos_in_unit ==
		       (off - get_key_offset(&key)) >> PAGE_SHIFT);
	}
	idx = get_key_offset(&key) >> PAGE_SHIFT;

	while (iter_coord.unit_pos <= coord_last_unit_pos(&iter_coord)) {
		reiser4_extent *ext = extent_by_coord(&iter_coord);

		assert("edward-2404", pos_in_unit < extent_get_width(ext));

		ret = readpages_extent_unit(coord, ext, pos_in_unit, mapping,
					    idx, mctx->pages, nr_pages);
		if (ret)
			goto error;

		nr_pages += (extent_get_width(ext) - pos_in_unit);
		idx += extent_get_width(ext);
		iter_coord.unit_pos += 1;
		pos_in_unit = 0;
	}
	assert("edward-2405", nr_pages == mctx->nr_pages);
	return 0;
 error:
	for (i = 0; i < nr_pages; i++)
		put_page(mctx->pages[i]);
	return ret;
}

/**
 * "cut off" a number of unformatted blocks at the end of extent item
 * specified by @coord.
 * @from_off: offset to cut from.
 */
static int cut_off_tail(coord_t *coord, struct inode *inode,
			loff_t from_off)
{
	reiser4_key from, to;
	coord_t from_coord;
	coord_t to_coord;

	coord_dup(&from_coord, coord);

	coord_dup(&to_coord, coord);
	to_coord.between = AT_UNIT;
	to_coord.unit_pos = coord_last_unit_pos(coord);

	item_key_by_coord(coord, &to);
	set_key_offset(&to, get_key_offset(&to) +
		       reiser4_extent_size(coord) - 1);

	from = to;
	set_key_offset(&from, from_off);

	return kill_node_content(&from_coord, &to_coord, &from, &to,
				 NULL, NULL, inode, 0);
}

static int migrate_blocks(struct extent_migrate_context *mctx)
{
	int i;
	int ret;
	reiser4_key key;
	reiser4_extent new_ext;
	reiser4_item_data idata;
	reiser4_block_nr block;
	int nr_jnodes = 0;
	coord_t *coord = mctx->coord;
	znode *loaded;
	reiser4_subvol *new_subv = current_origin(mctx->new_loc);
	struct atom_brick_info *abi;
	ON_DEBUG(reiser4_key check_key);
	ON_DEBUG(const char *error);

	assert("edward-2406",
	       equi(mctx->migrate_whole_item,
		    keyeq(unit_key_by_coord(coord, &check_key), mctx->key) &&
		    mctx->stop_off == get_key_offset(mctx->key)));
	/*
	 * Reserve space on the new data brick.
	 * Balancing procedure is allowed to fail with ENOSPC.
	 */
	grab_space_enable();
	ret = reiser4_grab_space(mctx->nr_pages, 0, new_subv);
	if (ret)
		return ret;
	ret = readpages_extent_item(coord, mctx->stop_off,
				    mctx->inode->i_mapping, mctx);
	if (ret)
		return ret;

	memcpy(&key, mctx->key, sizeof(key));
	set_key_offset(&key, mctx->stop_off);
	set_key_ordering(&key, mctx->new_loc);

	for (i = 0; i < mctx->nr_pages; i++) {
		struct page *page = mctx->pages[i];
		jnode *node;

		assert("edward-2407", page != NULL);
		assert("edward-2408",
		       page->index == (mctx->stop_off >> PAGE_SHIFT) + i);
		lock_page(page);
		node = do_jget(page);
		if (IS_ERR(node)) {
			nr_jnodes = i;
			unlock_page(page);
			ret = PTR_ERR(node);
			goto error;
		}
		JF_SET(node, JNODE_WRITE_PREPARED);
		unlock_page(page);

	}
	nr_jnodes = mctx->nr_pages;

	if (mctx->migrate_whole_item) {
		reiser4_extent *ext;

		assert("edward-2464", coord->unit_pos == 0);
		assert("edward-2465", mctx->stop_off ==
		       get_key_offset(item_key_by_coord(coord, &check_key)));
		/*
		 * cut all units except the first one;
		 * deallocate all blocks, pointed out by that first unit;
		 * set that unit as unallocated extent of proper width;
		 * update item's key to point out to the new brick;
		 * try to merge the resulted item with the item at left
		 * and right.
		 */
		if (nr_units_extent(coord) > 1) {
			ret = cut_off_tail(coord, mctx->inode, mctx->stop_off +
					   reiser4_extent_size_at(coord, 1));
			if (ret)
				goto error;
		}
		loaded = coord->node;
		ret = zload(loaded);
		if (ret)
			goto error;

		ext = extent_by_coord(coord);
		if (state_of_extent(ext) == ALLOCATED_EXTENT) {
			reiser4_block_nr start = extent_get_start(ext);
			reiser4_block_nr len = extent_get_width(ext);

			reiser4_dealloc_blocks(&start,
					       &len,
					       0, BA_DEFER,
					       find_data_subvol(coord));
		}
		reiser4_set_extent(new_subv, ext,
				   UNALLOCATED_EXTENT_START, nr_jnodes);
		ret = update_item_key(coord, &key);
		if (ret) {
			zrelse(loaded);
			goto error;
		}
		try_merge_with_right_item(coord);
		try_merge_with_left_item(coord);
		assert("edward-2466",
		       check_node40(coord->node,
				    REISER4_NODE_TREE_STABLE, &error) == 0);
		zrelse(loaded);
	} else {
		/*
		 * cut of tail, insert a new item at the end
		 */
		ret = cut_off_tail(coord, mctx->inode, mctx->stop_off);
		if (ret)
			goto error;

		coord_init_after_item(coord);

		reiser4_set_extent(new_subv, &new_ext,
				   UNALLOCATED_EXTENT_START, mctx->nr_pages);
		init_new_extent(EXTENT41_POINTER_ID, &idata, &new_ext, 1);
		ret = insert_by_coord(coord, &idata, &key, mctx->lh, 0);
		if (ret)
			goto error;

		loaded = coord->node;
		ret = zload(loaded);
		if (ret)
			goto error;
		assert("edward-2416",
		       keyeq(&key, item_key_by_coord(coord, &check_key)));
		assert("edward-2424",
		       reiser4_extent_size(coord) == mctx->nr_pages << PAGE_SHIFT);

		try_merge_with_right_item(coord);

		assert("edward-2425",
		       check_node40(coord->node,
				    REISER4_NODE_TREE_STABLE, &error) == 0);
		zrelse(loaded);
	}
	/*
	 * Capture jnodes, set new addresses for them,
	 * and make them dirty. At flush time all the
	 * blocks will get new location on the new brick.
	 */
	ret = check_insert_atom_brick_info(new_subv->id, &abi);
	if (ret)
		goto error;
	block = fake_blocknr_unformatted(mctx->nr_pages, new_subv);

	for (i = 0; i < mctx->nr_pages; i++, block++) {
		jnode *node = jprivate(mctx->pages[i]);

		assert("edward-2417", node != NULL);

		set_page_dirty_notag(mctx->pages[i]);

		spin_lock_jnode(node);
		JF_SET(node, JNODE_CREATED);
		JF_CLR(node, JNODE_WRITE_PREPARED);

		node->subvol = new_subv;
		jnode_set_block(node, &block);

		ret = reiser4_try_capture(node, ZNODE_WRITE_LOCK, 0);
		BUG_ON(ret != 0);

		jnode_make_dirty_locked(node);
		spin_unlock_jnode(node);

		jput(node);
		put_page(mctx->pages[i]);
	}
	return 0;
 error:
	for (i = 0; i < mctx->nr_pages; i++) {
		if (i < nr_jnodes)
			jput(jprivate(mctx->pages[i]));
		put_page(mctx->pages[i]);
	}
	return ret;
}

static int do_migrate_extent(struct extent_migrate_context *mctx)
{
	int ret = 0;
	lock_handle *lh = mctx->lh;
	coord_t *coord = mctx->coord;
	znode *loaded;

	assert("edward-2106", coord->node == lh->node);
	assert("edward-2128",
	       get_key_ordering(mctx->key) != mctx->new_loc);

	ret = zload(coord->node);
	if (ret)
		return ret;
	loaded = coord->node;

	mctx->nr_pages = (get_key_offset(mctx->key) +
			  reiser4_extent_size(coord) -
			  mctx->stop_off) >> PAGE_SHIFT;
	mctx->pages = reiser4_vmalloc(sizeof(mctx->pages) * mctx->nr_pages);
	if (!mctx->pages) {
		zrelse(loaded);
		return RETERR(-ENOMEM);
	}
	ret = migrate_blocks(mctx);

	vfree(mctx->pages);
	zrelse(loaded);
	done_lh(lh);
	if (ret)
		return ret;
	mctx->done_off = mctx->stop_off;
	mctx->blocks_migrated += mctx->nr_pages;

	reiser4_throttle_write(mctx->inode);
	/*
	 * Release the rest of blocks we grabbed for the fulfilled
	 * iteration.
	 */
	all_grabbed2free();
	/*
	 * The next migrate-split iteration starts here.
	 * Grab disk space for meta-data blocks in this iteration. We grab
	 * from reserved area, as rebalancing can be launched on a volume
	 * with no free space.
	 */
	ret = reserve_migration_iter();
	if (ret)
		return ret;
	if (mctx->migrate_whole_item) {
		/*
		 * no more blocks to be migrated in this item
		 */
		mctx->stop = 1;
		return 0;
	}
	/*
	 * go to the leftmost non-processed item
	 */
	assert("edward-2418", mctx->done_off != 0);

	set_key_offset(mctx->key, mctx->done_off - 1);
	ret = find_file_item_nohint(coord, lh, mctx->key,
				    ZNODE_WRITE_LOCK, mctx->inode);
	if (ret) {
		/*
		 * item not found (killed by concurrent
		 * truncate, or error happened)
		 */
		warning("edward-2318",
			"Item not found after migration (%d)", ret);
		done_lh(lh);
		if (!IS_CBKERR(ret)) {
			ret = 0;
			mctx->stop = 1;
		}
		return ret;
	}
	/*
	 * reset @mctx->key, as the item could be changed
	 * while we had keeping the lock released.
	 */
	ret = zload(coord->node);
	if (ret)
		return ret;
	item_key_by_coord(coord, mctx->key);
	zrelse(coord->node);
	return 0;
}

/**
 * Create a new extent item right after the item specified by @coord
 * and move the tail part of the last one to that newly created item. It can
 * involve carry, if there is no free space on the node. Subtle!
 *
 * @unit_split_pos: splitting position in the unit.
 * The pair @coord and @unit_split_pos defines splitting position in the item.
 * If @unit_split_pos != 0, then the unit at @coord will be split at
 * @unit_split_pos offset and its right part will start the new item.
 * Otherwise, we'll split at the unit boundary and the unit at @coord will be
 * moved to the head of the new item.
 *
 * Upon successfull completion:
 * if @unit_split_pos != 0, then @coord points out to the same unit, which
 * became smaller after split. Otherwise, @coord points out to the preceding
 * unit.
 */
static int split_extent_item(coord_t *coord, reiser4_block_nr unit_split_pos)
{
	int ret;
	coord_t cut_from;
	coord_t cut_to;
	char *tail_copy;
	char *tail_orig;
	int tail_num_units;
	int tail_len;
	reiser4_item_data idata;
	reiser4_key split_key;
	reiser4_key item_key;
	ON_DEBUG(reiser4_key check_key);

	assert("edward-2109", znode_is_loaded(coord->node));
	assert("edward-2143", ergo(unit_split_pos == 0, coord->unit_pos > 0));

	memset(&idata, 0, sizeof(idata));
	item_key_by_coord(coord, &item_key);
	unit_key_by_coord(coord, &split_key);
	set_key_offset(&split_key,
		       get_key_offset(&split_key) +
		       (unit_split_pos << current_blocksize_bits));

	if (unit_split_pos != 0) {
		/*
		 * start from splitting the unit.
		 * NOTE: it may change the item @coord (specifically, split
		 * it and move its part to the right neighbor
		 */
		ret = split_extent_unit(coord, unit_split_pos,
					0 /* stay on the original position */);
		if (ret)
			return ret;
		assert("edward-2110",
		       keyeq(&item_key, item_key_by_coord(coord, &check_key)));
		/*
		 * check if it was the case of item splitting at desired offset
		 * (see the comment above).
		 */
		if (reiser4_extent_size(coord) ==
		    get_key_offset(&split_key) - get_key_offset(&item_key))
			/*
			 * item was split at specified offset - nothing to do
			 * any more
			 */
			return 0;
		assert("edward-2426", reiser4_extent_size(coord) >
		       get_key_offset(&split_key) - get_key_offset(&item_key));
		/*
		 * unit at @coord decreased, number of units in the item
		 * got incremented
		 */
		tail_orig =
			node_plugin_by_node(coord->node)->item_by_coord(coord) +
			(coord->unit_pos + 1) * sizeof(reiser4_extent);
		tail_num_units = coord_num_units(coord) - coord->unit_pos - 1;
	} else {
		/*
		 * none of the units is subjected to splitting -
		 * we'll split the item at units boundary.
		 */
		tail_orig =
			node_plugin_by_node(coord->node)->item_by_coord(coord) +
			coord->unit_pos * sizeof(reiser4_extent);
		tail_num_units = coord_num_units(coord) - coord->unit_pos;
	}
	assert("edward-2427", tail_num_units > 0);

	tail_len = tail_num_units * sizeof(reiser4_extent);

	tail_copy = kmalloc(tail_len, reiser4_ctx_gfp_mask_get());
	if (!tail_copy)
		return -ENOMEM;
	memcpy(tail_copy, tail_orig, tail_len);
	/*
	 * cut off the tail from the original item
	 */
	coord_dup(&cut_from, coord);
	if (unit_split_pos)
		/* the original unit was split */
		cut_from.unit_pos ++;
	coord_dup(&cut_to, coord);
	cut_to.unit_pos = coord_num_units(coord) - 1;
	/*
	 * cut the original tail
	 */
	cut_node_content(&cut_from, &cut_to, NULL, NULL, NULL);
	/* make sure that @coord is valid after cut operation */
	if (unit_split_pos == 0)
		coord->unit_pos --;

	assert("edward-2428",
	       get_key_offset(item_key_by_coord(coord, &check_key)) +
	       reiser4_extent_size(coord) == get_key_offset(&split_key));
	/*
	 * finally, create a new item
	 */
	init_new_extent(item_id_by_coord(&cut_from),
			&idata, tail_copy, tail_num_units);
	coord_init_after_item(&cut_from);

	ret = insert_by_coord(&cut_from, &idata, &split_key,
			      0 /* lh */, COPI_DONT_SHIFT_LEFT);
	kfree(tail_copy);
	return 0;
}

static int do_split_extent(struct extent_migrate_context *mctx)
{
	int ret;
	znode *loaded;

	loaded = mctx->coord->node;
	ret = zload(loaded);
	if (ret)
		return ret;
	ret = split_extent_item(mctx->coord, mctx->unit_split_pos);
	zrelse(loaded);
	return ret;
}

static void init_migration_context(struct extent_migrate_context *mctx,
				   struct inode *inode, coord_t *coord,
				   reiser4_key *key, lock_handle *lh)
{
	memset(mctx, 0, sizeof(*mctx));
	mctx->coord = coord;
	mctx->key = key;
	mctx->inode = inode;
	mctx->lh = lh;
}

static void reset_migration_context(struct extent_migrate_context *mctx)
{
	mctx->act = INVALID_ACTION;
	mctx->nr_pages = 0;
	mctx->stop = 0;
	mctx->unit_split_pos = 0;
	mctx->blocks_migrated = 0;
	mctx->migrate_whole_item = 0;
}

/**
 * Assign a migration primitive in the case when the whole item
 * is either to be migrated, or to be skipped.
 */
static void what_to_do_nosplit(struct extent_migrate_context *mctx,
			       u64 *dst_id)
{
	coord_t *coord;
	struct inode *inode = mctx->inode;

	coord = mctx->coord;
	zload(coord->node);
	coord_clear_iplug(coord);
	reset_migration_context(mctx);

	/*
	 * For each item there are only 2 options:
	 * either skip the whole one, or migrate it
	 */
	mctx->new_loc = dst_id != NULL ? *dst_id :
		calc_data_subvol(inode, get_key_offset(mctx->key))->id;

	if (get_key_ordering(mctx->key) == mctx->new_loc) {
		/*
		 * skip the whole extent
		 */
		coord->unit_pos = 0;
		mctx->stop_off = get_key_offset(mctx->key);

		mctx->stop = 1;
		mctx->act = SKIP_EXTENT;
	} else if (reiser4_extent_size(coord) <=
		   (MIGRATION_GRANULARITY << PAGE_SHIFT)) {
		/*
		 * migrate the whole extent
		 */
		coord->unit_pos = 0;
		mctx->stop_off = get_key_offset(mctx->key);

		mctx->migrate_whole_item = 1;
		mctx->act = MIGRATE_EXTENT;
	} else {
		/*
		 * extent is too large, migrate it by parts
		 */
		lookup_result ret;
		reiser4_key split_key;

		mctx->stop_off =
			get_key_offset(mctx->key) +
			reiser4_extent_size(coord) -
			(MIGRATION_GRANULARITY << PAGE_SHIFT);

		memcpy(&split_key, mctx->key, sizeof(split_key));
		set_key_offset(&split_key, mctx->stop_off);
		ret = lookup_extent(&split_key, FIND_EXACT, coord);

		assert("edward-2442", ret == CBK_COORD_FOUND);
		assert("edward-2443", coord->between == AT_UNIT);

		unit_key_by_coord(coord, &split_key);

		assert("edward-2444",
		       get_key_offset(&split_key) <= mctx->stop_off);
		mctx->unit_split_pos =
			(mctx->stop_off -
			 get_key_offset(&split_key)) >> PAGE_SHIFT;
		mctx->migrate_whole_item = 0;
		mctx->act = MIGRATE_EXTENT;
	}
	zrelse(coord->node);
	return;
}

/**
 * Assign primitive migration operation over the given item
 * specified by @mctx.coord
 */
static void what_to_do(struct extent_migrate_context *mctx, u64 *dst_id)
{
	loff_t off1, off2;
	loff_t split_off;

	coord_t *coord;
	lookup_result ret;
	struct inode *inode = mctx->inode;
	reiser4_key split_key;

	if (nosplit_migration_mode())
		/*
		 * the whole item is either to be migrated,
		 * or to be skipped
		 */
		return what_to_do_nosplit(mctx, dst_id);

	coord = mctx->coord;
	zload(coord->node);
	coord_clear_iplug(coord);
	reset_migration_context(mctx);
	/*
	 * find split offset in the item, i.e. maximal offset,
	 * so that data bytes at offset and (offset - 1) belong
	 * to different bricks in the new logical volume
	 */
	/* offset of the leftmost byte */
	off1 = get_key_offset(mctx->key);
	/* offset of rightmost byte */
	off2 = off1 + reiser4_extent_size(coord) - 1;

	/* normalize offsets */
	off1 = off1 - (off1 & (current_stripe_size - 1));
	off2 = off2 - (off2 & (current_stripe_size - 1));

	mctx->new_loc =
		dst_id != NULL ? *dst_id : calc_data_subvol(inode, off2)->id;

	while (off1 < off2) {
		off2 -= current_stripe_size;
		if (calc_data_subvol(inode, off2)->id != mctx->new_loc) {
			split_off = off2 + current_stripe_size;
			goto split_off_found;
		}
	}
	/*
	 * split offset not found
	 *
	 * set current position to the beginning of the item
	 */
	coord->unit_pos = 0;
	mctx->stop_off = get_key_offset(mctx->key);
	if (mctx->new_loc != get_key_ordering(mctx->key)) {
		/*
		 * the whole item should be migrated
		 */
		mctx->migrate_whole_item = 1;
		mctx->act = MIGRATE_EXTENT;
		zrelse(coord->node);
		return;
	} else {
		/*
		 * the item is not to be splitted, or
		 * migrated - finish to process extent
		 */
		mctx->stop = 1;
		mctx->act = SKIP_EXTENT;
		zrelse(coord->node);
		return;
	}
 split_off_found:
	/*
	 * set current position to the found split offset
	 */
	assert("edward-2112",
	       (get_key_offset(mctx->key) < split_off) &&
	       (split_off < (get_key_offset(mctx->key) +
			     reiser4_extent_size(coord))));

	mctx->stop_off = split_off;

	memcpy(&split_key, mctx->key, sizeof(split_key));
	set_key_offset(&split_key, split_off);
	ret = lookup_extent(&split_key, FIND_EXACT, coord);

	assert("edward-2113", ret == CBK_COORD_FOUND);
	assert("edward-2114", coord->between == AT_UNIT);

	unit_key_by_coord(coord, &split_key);

	assert("edward-2115", get_key_offset(&split_key) <= split_off);
	mctx->unit_split_pos =
		(split_off - get_key_offset(&split_key)) >> PAGE_SHIFT;

	zrelse(coord->node);
	if (mctx->new_loc != get_key_ordering(mctx->key)) {
		/*
		 * Only a part of item should be migrated.
		 * In this case we don't perform the regular
		 * split operation - the item will be "split"
		 * by migration procedure
		 */
		mctx->migrate_whole_item = 0;
		mctx->act = MIGRATE_EXTENT;
		return;
	}
	/*
	 * The item to be split, its right part to be
	 * skipped, and the left part to be processed in
	 * the next iteration of migrate_extent().
	 * Now calculate position for split.
	 */
	mctx->act = SPLIT_EXTENT;
	return;
}

int reiser4_migrate_extent(coord_t *coord, reiser4_key *key,
			   lock_handle *lh, struct inode *inode,
			   loff_t *done_off, u64 *dst_id)
{
	int ret = 0;
	reiser4_block_nr blocks_migrated = 0;
	struct extent_migrate_context mctx;

	init_migration_context(&mctx, inode, coord, key, lh);

	while (!mctx.stop) {
		what_to_do(&mctx, dst_id);
		switch(mctx.act) {
		case SKIP_EXTENT:
			ret = zload(mctx.coord->node);
			if (ret)
				goto out;
			try_merge_with_right_item(mctx.coord);
			zrelse(mctx.coord->node);
			*done_off = mctx.stop_off;
			goto out;
		case SPLIT_EXTENT:
			ret = do_split_extent(&mctx);
			if (ret)
				goto out;
			continue;
		case MIGRATE_EXTENT:
			ret = do_migrate_extent(&mctx);
			if (ret)
				goto out;
			assert("edward-2351", mctx.blocks_migrated > 0);
			*done_off = mctx.done_off;

			blocks_migrated += mctx.blocks_migrated;
#if 0
			if (blocks_migrated >= MIGRATION_GRANULARITY) {
				ret = -E_REPEAT;
				/*
				 * FIXME-EDWARD:
				 * do we need to interrupt long migration
				 * and commit transactions like we do in
				 * the case of truncate? So far, as I can
				 * see, we can go without it..
				 */
				goto out;
			}
#endif
			break;
		default:
			impossible("edward-2116",
				   "Bad migrate action id %d", mctx.act);
		}
	}
 out:
	done_lh(lh);
	return ret;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 80
 * scroll-step: 1
 * End:
 */