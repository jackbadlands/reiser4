/*
  Balanced Fiber-Striped eXtendable array with Weights.
  Inventor, Author: Eduard O. Shishkin
  Implementation over 32-bit hash.
  Adapted for use in Reiser4.

  Copyright (c) 2014-2019 Eduard O. Shishkin

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <asm/types.h>
#include "../../debug.h"
#include "../../inode.h"
#include "../plugin.h"
#include "dst.h"

#define MAX_SHIFT      (31)
#define MAX_BUCKETS    (1u << MAX_SHIFT)
#define MIN_NUMS_BITS  (10)
#define FSX32_PRECISE  (0)

static inline void *fsx32_alloc(u64 len)
{
	void *result = reiser4_vmalloc(len * sizeof(u32));
	if (result)
		memset(result, 0, len * sizeof(u32));
	return result;
}

static inline void *fsx64_alloc(u64 len)
{
	void *result = reiser4_vmalloc(len * sizeof(u64));
	if (result)
		memset(result, 0, len * sizeof(u64));
	return result;
}

static inline void fsx_free(void *p)
{
	vfree(p);
}

static inline struct fsx32_dcx *fsx32_private(reiser4_dcx *dcx)
{
	return &dcx->fsx32;
}

static void init_fibers_by_tab(u32 numb,
			       u32 nums_bits,
			       u32 *tab,
			       bucket_t *vec,
			       void *(*fiber_at)(bucket_t *vec, u64 idx),
			       u32 (*id2idx)(u64 id),
			       u32 *weights)
{
	u32 i;
	u32 nums = 1 << nums_bits;

	for(i = 0; i < numb; i++)
		weights[i] = 0;

	for(i = 0; i < nums; i++) {
		u32 *fib;

		fib = fiber_at(vec, id2idx(tab[i]));
		fib[(weights[id2idx(tab[i])])++] = i;
	}
}

static void init_tab_by_fibers(u32 numb,
			       u32 *tab,
			       bucket_t *vec,
			       void *(*fiber_at)(bucket_t *vec, u64 idx),
			       u64 (*idx2id)(u32 idx),
			       u32 *weights)
{
	u32 i, j;

	for(i = 0; i < numb; i++)
		for (j = 0; j < weights[i]; j++) {
			u32 *fib;
			fib = fiber_at(vec, i);
			tab[fib[j]] = idx2id(i);
		}
}

u32 *init_tab_from_scratch(u32 *weights, u32 numb, u32 nums_bits,
			   u64 (*idx2id)(u32 idx))
{
	u32 i, j, k;
	u32 *tab;
	u32 nums = 1 << nums_bits;

	tab = fsx32_alloc(nums);
	if (!tab)
		return NULL;
	for (i = 0, k = 0; i < numb; i++)
		for (j = 0; j < weights[i]; j++)
			tab[k++] = idx2id(i);
	return tab;
}

static void calibrate(u64 num, u64 val,
		      bucket_t *vec, u64 (*vec_el_get)(bucket_t *vec, u64 idx),
		      void *ret, u64 (*ret_el_get)(void *ret, u64 idx),
		      void (ret_el_set)(void *ret, u64 idx, u64 value))
{
	u64 i;
	u64 rest;
	u64 sum_scaled = 0;
	u64 sum_not_scaled = 0;

	for (i = 0; i < num; i++)
		sum_not_scaled += vec_el_get(vec, i);
	for (i = 0; i < num; i++) {
		u64 q;
		u64 result;

		q = val * vec_el_get(vec, i);
		result = div64_u64(q, sum_not_scaled);
		ret_el_set(ret, i, result);
		sum_scaled += result;
	}
	rest = val - sum_scaled;
	/*
	 * Don't modify this: it will be a format change!
	 */
	for (i = 0; i < rest; i++)
		ret_el_set(ret, i, ret_el_get(ret, i) + 1);
	return;
}

static u64 array32_el_get(void *array, u64 idx)
{
	return ((u32 *)array)[idx];
}

static void array32_el_set(void *array, u64 idx, u64 val)
{
	((u32 *)array)[idx] = val;
}

static u64 array64_el_get(void *array, u64 idx)
{
	return ((u64 *)array)[idx];
}

static void array64_el_set(void *array, u64 idx, u64 val)
{
	((u64 *)array)[idx] = val;
}

static void calibrate32(u32 num, u32 val, bucket_t *vec,
			u64 (*vec_el_get)(bucket_t *vec, u64 idx),
			u32 *ret)
{
	calibrate(num, val, vec, vec_el_get,
		  ret, array32_el_get, array32_el_set);
}

static void calibrate64(u64 num, u64 val, bucket_t *vec,
			u64 (*vec_el_get)(bucket_t *vec, u64 idx),
			u64 *ret)
{
	calibrate(num, val, vec, vec_el_get,
		  ret, array64_el_get, array64_el_set);
}

int create_systab(u32 nums_bits, u32 **tab,
			 u32 numb, u32 *weights, bucket_t *vec,
			 void *(*fiber_at)(bucket_t *vec, u64 idx),
			 u64 (*idx2id)(u32 idx))
{
	u32 nums = 1 << nums_bits;

	*tab = fsx32_alloc(nums);
	if (!tab)
		return -ENOMEM;

	init_tab_by_fibers(numb, *tab, vec, fiber_at, idx2id, weights);
	return 0;
}

static int clone_systab(struct fsx32_dcx *dcx, void *tab)
{
	assert("edward-2169", dcx != NULL);
	assert("edward-2170", tab != NULL);
	assert("edward-2171", dcx->tab == NULL);

	dcx->tab = fsx32_alloc(1 << dcx->nums_bits);
	if (!dcx->tab)
		return -ENOMEM;
	memcpy(dcx->tab, tab, (1 << dcx->nums_bits) * sizeof(u32));
	return 0;
}

static void free_cloned_systab(struct fsx32_dcx *dcx)
{
	if (dcx->tab) {
		fsx_free(dcx->tab);
		dcx->tab = NULL;
	}
}

static int create_fibers(u32 nums_bits, u32 *tab,
			 u32 new_numb, u32 *new_weights, bucket_t *vec,
			 void *(*fiber_at)(bucket_t *vec, u64 idx),
			 void (*fiber_set_at)(bucket_t *vec, u64 idx, void *fib),
			 u64 *(*fiber_lenp_at)(bucket_t *vec, u64 idx),
			 u32 (*id2idx)(u64 id))
{
	u32 i;
	for(i = 0; i < new_numb; i++) {
		u32 *fib;
		u64 *fib_lenp;

		fib = fsx32_alloc(new_weights[i]);
		if (!fib)
			return -ENOMEM;
		fiber_set_at(vec, i, fib);
		fib_lenp = fiber_lenp_at(vec, i);
		*fib_lenp = new_weights[i];
	}
	init_fibers_by_tab(new_numb,
			   nums_bits, tab, vec, fiber_at, id2idx, new_weights);

	for (i = 0; i < new_numb; i++)
		assert("edward-1901",
		       new_weights[i] == *(fiber_lenp_at(vec, i)));
	return 0;
}

#if REISER4_DEBUG
void print_fiber(u32 id, bucket_t *vec,
		 void *(*fiber_at)(bucket_t *vec, u64 idx),
		 u64 *(*fiber_lenp_at)(bucket_t *vec, u64 idx))
{
	u32 i;
	u32 *fib = fiber_at(vec, id);
	u32 fib_len = *fiber_lenp_at(vec, id);

	printk("fiber %d (len %d):", id, fib_len);
	for (i = 0; i < fib_len; i++)
                printk("%d", fib[i]);
	printk("end of fiber %d", id);
	return;
}
#endif

static void release_fibers(u32 numb, bucket_t *vec,
			   void *(*fiber_at)(bucket_t *vec, u64 idx),
			   void (*fiber_set_at)(bucket_t *vec, u64 idx,
						void *fib))
{
	u32 i;

	for(i = 0; i < numb; i++) {
		u32 *fib;
		fib = fiber_at(vec, i);
		fsx_free(fib);
		fiber_set_at(vec, i, NULL);
	}
}

static int replace_fibers(u32 nums_bits, u32 *tab,
			  u32 old_numb, u32 new_numb,
			  u32 *new_weights, bucket_t *vec,
			  void *(*fiber_at)(bucket_t *vec, u64 idx),
			  void (*fiber_set_at)(bucket_t *vec, u64 idx, void *fib),
			  u64 *(*fiber_lenp_at)(bucket_t *vec, u64 idx),
			  u32 (*id2idx)(u64 id))
{
	release_fibers(old_numb, vec, fiber_at, fiber_set_at);
	return create_fibers(nums_bits, tab, new_numb, new_weights, vec,
			     fiber_at, fiber_set_at, fiber_lenp_at, id2idx);
}

/**
 * @vec: new array of abstract buckets
 * @new: a bucket to be added
 * @target_pos: index of @new in the @vec
 */
static int balance_inc(struct fsx32_dcx *dcx,
		       u32 new_numb, u32 *tab,
		       u32 *old_weights, u32 *new_weights,
		       u32 target_pos,
		       bucket_t *vec,
		       void *(*fiber_at)(bucket_t *vec, u64 idx),
		       u64 (*idx2id)(u32 idx),
		       bucket_t new)
{
	int ret = 0;
	u32 i, j;
	u32 *exc = NULL;

	exc = fsx32_alloc(new_numb);
	if (!exc) {
		ret = -ENOMEM;
		goto exit;
	}
	dcx->exc = exc;

	for (i = 0; i < target_pos; i++)
		exc[i] = old_weights[i] - new_weights[i];

	for(i = target_pos + 1; i < new_numb; i++) {
		if (new)
			exc[i] = old_weights[i-1] - new_weights[i];
		else
			exc[i] = old_weights[i] - new_weights[i];
	}
	assert("edward-1910", exc[target_pos] == 0);
	/*
	 * steal segments of all fibers to the left of target_pos
	 */
	for(i = 0; i < target_pos; i++)
		for(j = 0; j < exc[i]; j++) {
			u32 *fib;
			fib = fiber_at(vec, i);

			assert("edward-1902",
			       tab[fib[new_weights[i] + j]] == idx2id(i));

			tab[fib[new_weights[i] + j]] = idx2id(target_pos);
		}
	/*
	 * steal segments of all fibers to the right of target_pos
	 */
	for(i = target_pos + 1; i < new_numb; i++)
		if (new && FSX32_PRECISE) {
			/*
			 * A sort of FSX where idx2id() and id2idx
			 * are identical functions.
			 * After inserting a new bucket, internal IDs
			 * of all buckets to the right of @target_pos
			 * (in the new set of buckets!) get incremented,
			 * thus system table needs corrections
			 */
			for(j = 0; j < new_weights[i]; j++) {
				u32 *fib;
				fib = fiber_at(vec, i);
				assert("edward-1911", tab[fib[j]] == i - 1);
				tab[fib[j]] = i;
			}
			for(j = 0; j < exc[i]; j++) {
				u32 *fib;
				fib = fiber_at(vec, i);
				assert("edward-1912",
				       tab[fib[new_weights[i] + j]] == i - 1);
				tab[fib[new_weights[i] + j]] = target_pos;
			}
		} else {
			for(j = 0; j < new_weights[i]; j++) {
				u32 *fib;
				fib = fiber_at(vec, i);
				assert("edward-1913", tab[fib[j]] == idx2id(i));
			}
			for(j = 0; j < exc[i]; j++) {
				u32 *fib;
				fib = fiber_at(vec, i);
				assert("edward-1914",
				       tab[fib[new_weights[i] + j]] == idx2id(i));
				tab[fib[new_weights[i] + j]] = idx2id(target_pos);
			}
		}
 exit:
	if (exc)
		fsx_free(exc);
	return ret;
}

/**
 * @vec: new array of abstract buckets
 * @removeme: bucket to be removed
 * @target_pos: index (in @vec) of @removeme
 */
static int balance_dec(struct fsx32_dcx *dcx,
		       u32 new_numb, u32 *tab,
		       u32 *old_weights, u32 *new_weights,
		       u32 target_pos,
		       bucket_t *vec,
		       void *(*fiber_at)(bucket_t *vec, u64 idx),
		       void *(*fiber_of)(bucket_t bucket),
		       u64 (*idx2id)(u32 idx),
		       bucket_t removeme)
{
	int ret = 0;
	u32 i, j;
	u32 off_in_target = 0;
	u32 *sho;
	u32 *target;
	u32 victim_id = ((reiser4_subvol *)removeme)->id;

	sho = fsx32_alloc(new_numb);
	if (!sho) {
		ret = -ENOMEM;
		goto exit;
	}
	dcx->sho = sho;

	for(i = 0; i < target_pos; i++)
		sho[i] = new_weights[i] - old_weights[i];

	for(i = target_pos; i < new_numb; i++) {
		if (removeme)
			sho[i] = new_weights[i] - old_weights[i+1];
		else
			sho[i] = new_weights[i] - old_weights[i];
	}

	if (removeme) {
		target = fiber_of(removeme);
		off_in_target = 0;
	} else {
		target = fiber_at(vec, target_pos);
		off_in_target =
			old_weights[target_pos] - new_weights[target_pos];
	}
	/*
	 * distribute segments among all fibers to the left of target_pos
	 */
	for(i = 0; i < target_pos; i++)
		for(j = 0; j < sho[i]; j++) {
			assert("edward-1916",
			       tab[target[off_in_target]] == victim_id);
			tab[target[off_in_target ++]] = idx2id(i);
		}
	/*
	 * distribute segments among all fibers to the right of target_pos
	 */
	for(i = target_pos; i < new_numb; i++) {
		if (removeme && FSX32_PRECISE) {
			/*
			 * A sort of the algorithm, where idx2id() and
			 * id2idx are identity functions.
			 * After removing a bucket, internal IDs of
			 * all buckets to the right of target_pos
			 * get decremented, so that system table needs
			 * corrections.
			 */
			for(j = 0; j < old_weights[i]; j++) {
				u32 *fib;
				fib = fiber_at(vec, i);
				assert("edward-1903", tab[fib[j]] == i);
				tab[fib[j]] = i - 1;
			}
			for(j = 0; j < sho[i]; j++) {
				assert("edward-1917",
				       tab[target[off_in_target]] ==
				       target_pos);
				tab[target[off_in_target ++]] = i - 1;
			}
		}
		else {
			for(j = 0; j < old_weights[i + 1]; j++) {
				u32 *fib;
				fib = fiber_at(vec, i);
				assert("edward-1903", tab[fib[j]] == idx2id(i));
			}
			for(j = 0; j < sho[i]; j++) {
				assert("edward-1918",
				       tab[target[off_in_target]] ==
				       victim_id);
				tab[target[off_in_target ++]] = idx2id(i);
			}
		}
	}
 exit:
	if (sho)
		fsx_free(sho);
	return ret;
}

/**
 * Fix up system table after splitting segments with factor (1 << @fact_bits)
 */
static int balance_spl(u32 numb, u32 nums_bits,
		       u32 **tabp,
		       u32 *old_weights, u32 *new_weights,
		       u32 fact_bits,
		       void *vec,
		       void *(*fiber_at)(bucket_t *vec, u64 idx),
		       void (*fiber_set_at)(bucket_t *vec,
					    u64 idx, void *fib),
		       u64 *(*fiber_lenp_at)(bucket_t *vec, u64 idx),
		       u32 (*id2idx)(u64 id), u64 (*idx2id)(u32 idx))
{
	u32 ret = 0;
	u32 i,j,k = 0;
	u32 nums;

	u32 *tab = NULL;
	u32 *exc = NULL;
	u32 num_exc;
	u32 *sho = NULL;
	u32 num_sho;
	u32 *reloc = NULL;
	u32 num_reloc;
	u32 factor;

	assert("edward-1904", numb <= MAX_BUCKETS);
	assert("edward-1905", nums_bits + fact_bits <= MAX_SHIFT);

	nums = 1 << nums_bits;
	factor = 1 << fact_bits;

	num_exc = nums % numb;
	num_sho = numb - num_exc;

	if (num_exc) {
		exc = fsx32_alloc(numb);
		if (!exc)
			goto error;

		sho = exc + num_exc;

		for(i = 0; i < num_exc; i++)
			exc[i] = factor * old_weights[i] - new_weights[i];
		for(i = 0; i < num_sho; i++)
			sho[i] = new_weights[i] - factor * old_weights[i];
	}
	/*
	 * "stretch" system table with the @factor
	 */
	tab = fsx32_alloc(nums * factor);
	if (!tab) {
		ret = -ENOMEM;
		goto error;
	}
	for(i = 0; i < nums; i++)
		for(j = 0; j < factor; j++)
			tab[i * factor + j] = (*tabp)[i];
	fsx_free(*tabp);
	*tabp = NULL;

	if (!num_exc)
		/* everything is balanced */
		goto release;
	/*
	 * Build "stretched" fibers, which are still disbalanced
	 */
	for (i = 0; i < numb; i++)
		old_weights[i] *= factor;

	ret = replace_fibers(nums_bits + fact_bits, tab,
			     numb, numb, old_weights, vec,
			     fiber_at, fiber_set_at, fiber_lenp_at, id2idx);
	if (ret)
		goto error;
	/*
	 * calculate number of segments to be relocated
	 */
	for (i = 0, num_reloc = 0; i < num_exc; i++)
		num_reloc += exc[i];
	/*
	 * allocate array of segments to be relocated
	 */
	reloc = fsx32_alloc(num_reloc);
	if (!reloc)
		goto error;
	/*
	 * assemble segments, which are to be relocated
	 */
	for (i = 0, k = 0; i < num_exc; i++)
		for (j = 0; j < exc[i]; j++) {
			u32 *fib;
			fib = fiber_at(vec, i);
			reloc[k++] = fib[new_weights[i] + j];
		}
	/*
	 * distribute segments
	 */
	for (i = 0, k = 0; i < num_sho; i++)
		for (j = 0; j < sho[i]; j++)
			tab[reloc[k++]] = idx2id(num_exc + i);
 release:
	release_fibers(numb, vec, fiber_at, fiber_set_at);
	*tabp = tab;
	goto exit;
 error:
	if (tab)
		fsx_free(tab);
 exit:
	if (exc)
		fsx_free(exc);
	if (reloc)
		fsx_free(reloc);
	return ret;
}

void donev_fsx32(reiser4_dcx *rdcx)
{
	struct fsx32_dcx *dcx;

	dcx = fsx32_private(rdcx);

	if (dcx->weights != NULL) {
		fsx_free(dcx->weights);
		dcx->weights = NULL;
	}
}

/**
 * Set newly created distribution table to @target
 */
void replace_fsx32(reiser4_dcx *rdcx, void **target)
{
	struct fsx32_dcx *dcx = fsx32_private(rdcx);

	assert("edward-2236", target != NULL);
	assert("edward-2237", *target == NULL);

	*target = dcx->tab;
	dcx->tab = NULL;
}

/**
 * Free system table specified by @tab
 */
void free_fsx32(void *tab)
{
	assert("edward-2238", tab != NULL);
	fsx_free(tab);
}

/**
 * Initialize distribution context for regular file operations
 */
int initr_fsx32(reiser4_dcx *rdcx, void **tab, int nums_bits)
{
	struct fsx32_dcx *dcx = fsx32_private(rdcx);

	if (*tab != NULL)
		/* already initialized */
		return 0;

	if (nums_bits < MIN_NUMS_BITS) {
		warning("edward-1953",
			"Invalid minimal number of bricks (%llu). "
			"It should be not less than %llu",
			1ull << nums_bits, 1ull << MIN_NUMS_BITS);
		return -EINVAL;
	}
	*tab = fsx32_alloc(1 << nums_bits);
	if (*tab == NULL)
		return -ENOMEM;

	dcx->nums_bits = nums_bits;
	return 0;
}

void doner_fsx32(void **tab)
{
	assert("edward-2260", tab != NULL);

	if (*tab) {
		fsx_free(*tab);
		*tab = NULL;
	}
}

/**
 * Initialize distribution context for volume operations
 *
 * @buckets: set of abstract buckets;
 * @ops: operations to access the buckets;
 * @rdcx: distribution context to be initialized.
 */
int initv_fsx32(void **tab, u64 numb, int nums_bits,
		reiser4_dcx *rdcx)
{
	int ret = -ENOMEM;
	u32 nums;
	struct fsx32_dcx *dcx;
	struct bucket_ops *ops = current_bucket_ops();

	if (numb == 0 || nums_bits >= MAX_SHIFT)
		return -EINVAL;

	nums = 1 << nums_bits;
	if (numb >= nums)
		return -EINVAL;

	dcx = fsx32_private(rdcx);

	assert("edward-2172", dcx->tab == NULL);
	assert("edward-1922", dcx->weights == NULL);
	assert("edward-2261", tab != NULL);
	assert("edward-2336", current_buckets() != NULL);

	dcx->numb = numb;
	dcx->weights = fsx32_alloc(numb);
	if (!dcx->weights)
		goto error;

	calibrate32(numb, nums, current_buckets(),
		    ops->cap_at, dcx->weights);

	if (*tab == NULL) {
		u32 i;
		assert("edward-2201", numb == 1);

		ret = initr_fsx32(rdcx, tab, nums_bits);
		if (ret)
			goto error;
		for (i = 0; i < nums; i++)
			(*(u32 **)tab)[i] = ops->idx2id(0);
	}
	assert("edward-2173", *tab != NULL);

	ret = create_fibers(nums_bits, *tab,
			    numb, dcx->weights, current_buckets(),
			    ops->fib_at,
			    ops->fib_set_at,
			    ops->fib_lenp_at,
			    ops->id2idx);
	if (ret)
		goto error;
	return 0;
 error:
	doner_fsx32(tab);
	donev_fsx32(rdcx);
	return ret;
}

u64 lookup_fsx32m(reiser4_dcx *rdcx, const char *str,
		  int len, u32 seed, void *tab)
{
	u32 hash;
	struct fsx32_dcx *dcx = fsx32_private(rdcx);

	hash = murmur3_x86_32(str, len, seed);
	return ((u32 *)tab)[hash >> (32 - dcx->nums_bits)];
}

int inc_fsx32(reiser4_dcx *rdcx, void *tab, u64 target_pos, bucket_t new)
{
	int ret = 0;
	u32 *new_weights;
	u32 old_numb, new_numb, nums;
	struct fsx32_dcx *dcx = fsx32_private(rdcx);
	struct bucket_ops *ops = current_bucket_ops();

	new_numb = old_numb = dcx->numb;
	if (new) {
		if (old_numb == MAX_BUCKETS)
			return -EINVAL;
		new_numb ++;
	}
	nums = 1 << dcx->nums_bits;
	if (new_numb > nums) {
		warning("edward-2337",
			"Can not add bucket: current limit (%u) reached",
			nums);
		return -EINVAL;
	}
	new_weights = fsx32_alloc(new_numb);
	if (!new_weights) {
		ret = -ENOMEM;
		goto error;
	}
	dcx->new_weights = new_weights;

	ret = clone_systab(dcx, tab);
	if (ret)
		goto error;

	calibrate32(new_numb, nums,
		    current_buckets(), ops->cap_at, new_weights);
	ret = balance_inc(dcx,
			  new_numb, dcx->tab,
			  dcx->weights, new_weights, target_pos,
			  current_buckets(), ops->fib_at,
			  ops->idx2id, new);
	if (ret)
		goto error;

	release_fibers(new_numb, current_buckets(),
		       ops->fib_at, ops->fib_set_at);

	fsx_free(dcx->weights);
	dcx->weights = new_weights;
	dcx->numb = new_numb;

	return 0;
 error:
	if (new_weights)
		fsx_free(new_weights);
	free_cloned_systab(dcx);
	return ret;
}

/**
 * Check if there is enough space on remaining buckets for successful
 * completion of a bucket operation.
 *
 * @numb: number of buckets upon succesfull completion.
 * @occ: total amount of space occupied on all buckets
 */
static int check_space(reiser4_dcx *rdcx, u64 numb, u64 occ)
{
	u64 i;
	int ret = 0;
	u64 *vec_new_occ;
	bucket_t *vec = current_buckets();
	struct bucket_ops *ops = current_bucket_ops();
	/*
	 * For each bucket: calculate how much space will be
	 * occupied on the bucket after successful completion
	 * of the volume operation and compare it with the
	 * bucket's capacity
	 */
	vec_new_occ = fsx64_alloc(numb);
	if (!vec_new_occ)
		return -ENOMEM;

	calibrate64(numb, occ, vec, ops->cap_at, vec_new_occ);

	for (i = 0; i < numb; i++) {
		ON_DEBUG(notice("edward-2145",
			"Brick %llu: data capacity: %llu, min required: %llu",
			i, ops->cap_at(vec, i), vec_new_occ[i]));

		if (ops->cap_at(vec, i) < vec_new_occ[i]) {
			warning("edward-2070",
	"Not enough data capacity (%llu) of brick %llu (required %llu)",
				ops->cap_at(vec, i),
				i,
				vec_new_occ[i]);
			ret = -ENOSPC;
			break;
		}
	}
	fsx_free(vec_new_occ);
	return ret;
}

int dec_fsx32(reiser4_dcx *rdcx, void *tab, u64 target_pos, bucket_t removeme)
{
	int ret = 0;
	u32 nums;
	u32 new_numb;
	u32 *new_weights = NULL;
	struct fsx32_dcx *dcx = fsx32_private(rdcx);
	struct bucket_ops *ops = current_bucket_ops();

	assert("edward-1908", dcx->numb >= 1);
	assert("edward-1909", dcx->numb <= MAX_BUCKETS);
	assert("edward-1927", dcx->numb > 1);

	new_numb = dcx->numb;
	if (removeme)
		new_numb --;

	ret = check_space(rdcx, new_numb, ops->space_occupied());
	if (ret)
		goto error;

	nums = 1 << dcx->nums_bits;
	new_weights = fsx32_alloc(new_numb);
	if (!new_weights) {
		ret = -ENOMEM;
		goto error;
	}
	dcx->new_weights = new_weights;

	ret = clone_systab(dcx, tab);
	if (ret)
		goto error;

	calibrate32(new_numb, nums,
		    current_buckets(), ops->cap_at, new_weights);

	ret = balance_dec(dcx,
			  new_numb, dcx->tab,
			  dcx->weights, new_weights, target_pos,
			  current_buckets(), ops->fib_at,
			  ops->fib_of, ops->idx2id,
			  removeme);
	if (ret)
		goto error;

	release_fibers(new_numb,
		       current_buckets(), ops->fib_at,
		       ops->fib_set_at);
	release_fibers(1,
		       &removeme, ops->fib_at,
		       ops->fib_set_at);

	fsx_free(dcx->weights);
	dcx->weights = new_weights;
	dcx->numb = new_numb;
	return 0;
 error:
	/* FIXME: add bucket (roll back remove_bucket()) */
	if (new_weights)
		fsx_free(new_weights);
	free_cloned_systab(dcx);
	return ret;
}

int spl_fsx32(reiser4_dcx *rdcx, u32 fact_bits)
{
	int ret = 0;
	u32 *new_weights;
	u32 new_nums;
	struct fsx32_dcx *dcx = fsx32_private(rdcx);
	struct bucket_ops *ops = current_bucket_ops();

	if (dcx->nums_bits + fact_bits > MAX_SHIFT)
		return -EINVAL;

	new_nums = 1 << (dcx->nums_bits + fact_bits);

	new_weights = fsx32_alloc(dcx->numb);
	if (!new_weights) {
		ret = -ENOMEM;
		goto error;
	}
	calibrate32(dcx->numb, new_nums,
		    current_buckets(), ops->cap_at, new_weights);
	ret = balance_spl(dcx->numb, dcx->nums_bits,
			  &dcx->tab,
			  dcx->weights,
			  new_weights,
			  fact_bits,
			  current_buckets(),
			  ops->fib_at,
			  ops->fib_set_at,
			  ops->fib_lenp_at,
			  ops->id2idx,
			  ops->idx2id);
	if (ret)
		goto error;
	fsx_free(dcx->weights);
	dcx->weights = new_weights;
	dcx->nums_bits += fact_bits;
	return 0;
 error:
	if (new_weights)
		fsx_free(new_weights);
	return ret;
}

void pack_fsx32(reiser4_dcx *rdcx, char *to, u64 src_off, u64 count)
{
	u64 i;
	u32 *src;
	struct fsx32_dcx *dcx = fsx32_private(rdcx);

	assert("edward-1923", to != NULL);
	assert("edward-1924", dcx->tab != NULL);

	src = dcx->tab + src_off;

	for (i = 0; i < count; i++) {
		put_unaligned(cpu_to_le32(*src), (d32 *)to);
		to += sizeof(u32);
		src ++;
	}
}

void unpack_fsx32(reiser4_dcx *rdcx, void *tab,
		  char *from, u64 dst_off, u64 count)
{
	u64 i;
	u32 *dst;

	assert("edward-1925", from != NULL);
	assert("edward-1926", tab != NULL);

	dst = (u32 *)tab + dst_off;

	for (i = 0; i < count; i++) {
		*dst = le32_to_cpu(get_unaligned((d32 *)from));
		from += sizeof(u32);
		dst ++;
	}
}

void dump_fsx32(reiser4_dcx *rdcx, void *tab, char *to, u64 offset, u32 size)
{
	memcpy(to, (u32 *)tab + offset, size);
}

/*
  Local variables:
  c-indentation-style: "K&R"
  mode-name: "LC"
  c-basic-offset: 8
  tab-width: 8
  fill-column: 80
  scroll-step: 1
  End:
*/