/*
  Copyright (c) 2017-2020 Eduard O. Shishkin

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#include "debug.h"
#include "super.h"
#include "inode.h"
#include "plugin/volume/volume.h"

static int reiser4_register_brick(struct reiser4_vol_op_args *args)
{
	reiser4_volume *host = NULL;

	return reiser4_scan_device(args->d.name, FMODE_READ,
				   get_reiser4_fs_type(), NULL, &host);
}

static int reiser4_print_volume(struct super_block *sb,
			       struct reiser4_vol_op_args *args)
{
	return super_vol_plug(sb)->print_volume(sb, args);
}

static int reiser4_print_brick(struct super_block *sb,
			       struct reiser4_vol_op_args *args)
{
	if (reiser4_volume_is_unbalanced(sb)) {
		warning("edward-2373",
			"Failed to print brick of unbalanced volume %s",
			sb->s_id);
		return -EBUSY;
	}
	return super_vol_plug(sb)->print_brick(sb, args);
}

/**
 * find activated brick by @name
 */
static reiser4_subvol *find_active_brick(struct super_block *super,
					 char *name)
{
	u32 subv_id;
	reiser4_subvol *result = NULL;
	lv_conf *conf = super_conf(super);

	for_each_mslot(conf, subv_id) {
		if (!conf_mslot_at(conf, subv_id))
			continue;
		if (!strcmp(conf_origin(conf, subv_id)->name, name)) {
			result = conf_origin(conf, subv_id);
			break;
		}
	}
	return result;
}

static int reiser4_resize_brick(struct super_block *sb,
				struct reiser4_vol_op_args *args)
{
	int ret;
	reiser4_subvol *this;

	if (reiser4_volume_is_unbalanced(sb)) {
		warning("edward-2166",
			"Failed to resize brick (Unbalanced volume %s)",
			sb->s_id);
		return -EBUSY;
	}
	if (args->new_capacity == 0) {
		warning("edward-2395", "Can not resize brick to zero.");
		return -EINVAL;
	}
	this = find_active_brick(sb, args->d.name);
	if (!this) {
		warning("edward-2148",
			"Brick %s doesn't belong to volume %s. Can not resize.",
			args->d.name,
			reiser4_get_current_sb()->s_id);
		return -EINVAL;
	}
	if (args->new_capacity == this->data_capacity)
		/* nothing to do */
		return 0;
	ret = super_vol_plug(sb)->resize_brick(super_volume(sb),
				this,
				args->new_capacity - this->data_capacity);
	if (ret)
		return ret;
	reiser4_volume_clear_unbalanced(sb);
	return capture_brick_super(get_meta_subvol());
}

static int reiser4_add_brick(struct super_block *sb,
			     struct reiser4_vol_op_args *args, int is_proxy)
{
	int ret;
	reiser4_volume *vol = super_volume(sb);
	int activated_here = 0;
	reiser4_subvol *new = NULL;
	reiser4_volume *host_of_new = NULL;
	txn_atom *atom;
	txn_handle *th;

	if (reiser4_volume_is_unbalanced(sb)) {
		warning("edward-2167",
			"Failed to add brick (Unbalanced volume %s)",
			sb->s_id);
		return -EBUSY;
	}
	/*
	 * register new brick
	 */
	ret = reiser4_scan_device(args->d.name, FMODE_READ,
				  get_reiser4_fs_type(), &new, &host_of_new);
	if (ret)
		return ret;

	assert("edward-1969", new != NULL);
	assert("edward-1970", host_of_new != NULL);

	if (host_of_new != vol) {
		warning("edward-1971",
			"Failed to add brick (Inappropriate volume)");
		return -EINVAL;
	}
	if (!subvol_is_set(new, SUBVOL_ACTIVATED)) {
		new->flags |= (1 << SUBVOL_IS_ORPHAN);

		ret = reiser4_activate_subvol(sb, new);
		if (ret)
			return ret;
		activated_here = 1;
	}
	if (is_proxy) {
		assert("edward-2449",
		       ergo(!is_meta_brick(new),
			    subvol_is_set(new, SUBVOL_HAS_DATA_ROOM)));

		new->flags |= (1 << SUBVOL_IS_PROXY);
		clear_bit(SUBVOL_HAS_DATA_ROOM, &new->flags);
	}
	/*
	 * add activated new brick
	 */
	ret = vol->vol_plug->add_brick(vol, new);
	if (ret) {
		if (activated_here) {
			reiser4_deactivate_subvol(sb, new);
			reiser4_unregister_subvol(new);
		}
		return ret;
	}
	/*
	 * now it is not possible to deactivate the new
	 * brick: since we posted a new config, there can
	 * be IOs issued against that brick.
	 *
	 * Put super-blocks of meta-data brick and of the
	 * new brick to the transaction - it will be first
	 * IO issued for the new brick.
	 */
	ret = capture_brick_super(get_meta_subvol());
	if (ret)
		return ret;
	ret = capture_brick_super(new);
	if (ret)
		return ret;
	if (subvol_is_set(new, SUBVOL_IS_PROXY)) {
		vol->proxy = new;
		/* start a proxy flushing kernel thread here */
		;
		reiser4_volume_set_proxy_enabled(sb);
		reiser4_volume_set_proxy_io(sb);
		clear_bit(SUBVOL_IS_ORPHAN, &new->flags);
		/*
		 * nothing to do any more
		 */
		printk("reiser4 (%s): Brick %s has been added.",
		       sb->s_id, new->name);
		return 0;
	}
	/*
	 * write unbalanced status to disk
	 */
	th = get_current_context()->trans;
	atom = get_current_atom_locked();
	assert("edward-2265", atom != NULL);
	spin_lock_txnh(th);
	ret = force_commit_atom(th);
	if (ret)
		return ret;
	/*
	 * new volume configuration has been written to disk,
	 * so release all volinfo jnodes - they are not needed
	 * any more
	 */
	release_volinfo_nodes(&vol->volinfo[CUR_VOL_CONF], 0);

	clear_bit(SUBVOL_IS_ORPHAN, &new->flags);

	printk("reiser4 (%s): Brick %s has been added.", sb->s_id, new->name);

	ret = vol->vol_plug->balance_volume(sb);
	if (ret)
		return ret;

	reiser4_volume_clear_unbalanced(sb);
	return capture_brick_super(get_meta_subvol());
}

static void reiser4_detach_brick(reiser4_subvol *victim)
{
	struct ctx_brick_info *cbi;
	reiser4_context *ctx = get_current_context();
	struct rb_root *root = &ctx->bricks_info;
	reiser4_super_info_data *sbinfo = get_current_super_private();

	cbi = find_context_brick_info(ctx, victim->id);

	assert("edward-2257", cbi != NULL);

	__grabbed2free(cbi, sbinfo, cbi->grabbed_blocks, victim);

	rb_erase(&cbi->node, root);
	RB_CLEAR_NODE(&cbi->node);
	free_context_brick_info(cbi);

	victim->id = INVALID_SUBVOL_ID;
	victim->flags |= (1 << SUBVOL_IS_ORPHAN);
	reiser4_deactivate_subvol(victim->super, victim);
	reiser4_unregister_subvol(victim);
}

static int reiser4_remove_brick(struct super_block *sb,
				struct reiser4_vol_op_args *args)
{
	int ret;
	reiser4_volume *vol;
	reiser4_subvol *victim;

	if (reiser4_volume_is_unbalanced(sb)) {
		warning("edward-2168",
			"Failed to remove brick (Unbalanced volume %s)",
			sb->s_id);
		return -EBUSY;
	}
	victim = find_active_brick(sb, args->d.name);
	if (!victim) {
		warning("edward-2149",
			"Brick %s doesn't belong to volume %s. Can not remove.",
			args->d.name,
			reiser4_get_current_sb()->s_id);
		return -EINVAL;
	}
	vol = super_volume(sb);

	ret = vol->vol_plug->remove_brick(vol, victim);
	if (ret)
		return ret;

	release_volinfo_nodes(&vol->volinfo[CUR_VOL_CONF], 0);
	/*
	 * unbalanced status was written to disk when
	 * committing everything in remove_brick_tail()
	 */
	reiser4_volume_clear_unbalanced(sb);

	if (!is_meta_brick(victim))
		/* Goodbye! */
		reiser4_detach_brick(victim);
	return capture_brick_super(get_meta_subvol());
}

static int reiser4_scale_volume(struct super_block *sb,
				struct reiser4_vol_op_args *args)
{
	int ret;
	txn_atom *atom;
	txn_handle *th;
	reiser4_volume *vol = super_volume(sb);

	if (reiser4_volume_is_unbalanced(sb)) {
		warning("edward-2168", "Failed to scale unbalanced volume %s)",
			sb->s_id);
		return -EBUSY;
	}
	if (args->s.val == 0)
		return 0;
	ret = super_volume(sb)->vol_plug->scale_volume(sb, args->s.val);
	if (ret)
		return ret;
	ret = capture_brick_super(get_meta_subvol());
	if (ret)
		return ret;
	/*
	 * write unbalanced status to disk
	 */
	th = get_current_context()->trans;
	atom = get_current_atom_locked();
	assert("edward-2402", atom != NULL);
	spin_lock_txnh(th);
	ret = force_commit_atom(th);
	if (ret)
		return ret;
	/*
	 * new volume configuration has been written to disk,
	 * so release all volinfo jnodes - they are not needed
	 * any more
	 */
	release_volinfo_nodes(&vol->volinfo[CUR_VOL_CONF], 0);

	printk("reiser4 (%s): Volume has beed scaled in %u times.",
	       sb->s_id, 1 << args->s.val);

	ret = vol->vol_plug->balance_volume(sb);
	if (ret)
		return ret;
	reiser4_volume_clear_unbalanced(sb);
	return capture_brick_super(get_meta_subvol());
}

/**
 * Balance the volume and complete all unfinished volume operations
 * (if any). On success unbalanced flag is cleared. Otherwise, the
 * balancing procedure should be repeated in some context.
 */
static int reiser4_balance_volume(struct super_block *sb)
{
	int ret;
	reiser4_volume *vol;

	if (!reiser4_volume_is_unbalanced(sb) &&
	    !reiser4_is_set(sb, REISER4_PROXY_ENABLED))
		return 0;
	vol = super_volume(sb);
	/*
	 * volume must have two distribution configs:
	 * old and new ones
	 */
	ret = vol->vol_plug->balance_volume(sb);
	if (ret)
		return ret;
	if (reiser4_volume_has_incomplete_removal(sb)) {
		/*
		 * finish unfinished brick removal
		 * detected at volume initialization time,
		 * see ->init_volume() method for details
		 */
		assert("edward-2258", vol->new_conf != NULL);
		assert("edward-2259", vol->victim != NULL);

		ret = vol->vol_plug->remove_brick_tail(vol, vol->victim);
		if (ret)
			return ret;
		reiser4_volume_clear_incomplete_removal(sb);
		reiser4_volume_clear_unbalanced(sb);
		ret = capture_brick_super(get_meta_subvol());
		if (ret)
			return ret;
		if (!is_meta_brick(vol->victim))
			reiser4_detach_brick(vol->victim);
		vol->victim = NULL;
		return 0;
	} else {
		assert("edward-2374", vol->victim == NULL);
		reiser4_volume_clear_unbalanced(sb);
		return capture_brick_super(get_meta_subvol());
	}
}

static int reiser4_set_file_immobile(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (reiser4_inode_get_flag(inode, REISER4_FILE_IMMOBILE))
		return 0;
	if (reserve_update_sd_common(inode))
		return RETERR(-ENOSPC);

	reiser4_inode_set_flag(inode, REISER4_FILE_IMMOBILE);
	return reiser4_update_sd(inode);
}

static int reiser4_clr_file_immobile(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (!reiser4_inode_get_flag(inode, REISER4_FILE_IMMOBILE))
		return 0;
	if (reserve_update_sd_common(inode))
		return RETERR(-ENOSPC);

	reiser4_inode_clr_flag(inode, REISER4_FILE_IMMOBILE);
	return reiser4_update_sd(inode);
}

static int reiser4_migrate_file(struct file *file, u64 dst_idx)
{
	int ret;
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;

	if (reiser4_volume_is_unbalanced(sb)) {
		/*
		 * actually, we can perform migration in this case,
		 * but let's don't bring the chaos.
		 */
		warning("", "Failed to migrate file of unbalanced volume %s)",
			sb->s_id);
		return RETERR(-EBUSY);
	}
	ret = super_vol_plug(sb)->migrate_file(inode, dst_idx);

	if (ret == 0 && ((file->f_flags & O_SYNC) || IS_SYNC(inode))) {
		reiser4_txn_restart_current();
		grab_space_enable();
		ret = reiser4_sync_file_common(file, 0, LONG_MAX,
					       0 /* data and stat data */);
		if (ret)
			warning("edward-2463", "failed to sync file %llu",
				(unsigned long long)get_inode_oid(inode));
	}
	return ret;
}

/**
 * Reiser4 off-line volume operations (no FS are mounted).
 * This doesn't spawn transactions and executes not in reiser4 context.
 */
int reiser4_offline_op(struct reiser4_vol_op_args *args)
{
	int ret;

	switch(args->opcode) {
	case REISER4_REGISTER_BRICK:
		ret = reiser4_register_brick(args);
		break;
	case REISER4_UNREGISTER_BRICK:
		ret = reiser4_unregister_brick(args);
		break;
	case REISER4_VOLUME_HEADER:
		ret = reiser4_volume_header(args);
		break;
	case REISER4_BRICK_HEADER:
		ret = reiser4_brick_header(args);
		break;
	default:
		warning("", "Unsupported off-line volume operation %d",
			args->opcode);
		ret = -ENOTTY;
		break;
	}
	return ret;
}

/**
 * Reiser4 on-line volume operations (on mounted volumes).
 * Spawn transactions and performed in reiser4 context.
 */
int reiser4_volume_op_dir(struct file *file, struct reiser4_vol_op_args *args)
{
	int ret;
	struct super_block *sb = file_inode(file)->i_sb;

	/*
	 * take exclusive access to the volume
	 */
	if (reiser4_volume_test_set_busy(sb)) {
		warning("", "Operation %d failed: volume %s is busy",
			args->opcode, sb->s_id);
		return RETERR(-EBUSY);
	}
	switch(args->opcode) {
	case REISER4_PRINT_VOLUME:
		ret = reiser4_print_volume(sb, args);
		break;
	case REISER4_PRINT_BRICK:
		ret = reiser4_print_brick(sb, args);
		break;
	case REISER4_RESIZE_BRICK:
		ret = reiser4_resize_brick(sb, args);
		break;
	case REISER4_ADD_BRICK:
		ret = reiser4_add_brick(sb, args, 0);
		break;
	case REISER4_ADD_PROXY:
		ret = reiser4_add_brick(sb, args, 1);
		break;
	case REISER4_REMOVE_BRICK:
		ret = reiser4_remove_brick(sb, args);
		break;
	case REISER4_SCALE_VOLUME:
		ret = reiser4_scale_volume(sb, args);
		break;
	case REISER4_BALANCE_VOLUME:
		ret = reiser4_balance_volume(sb);
		break;
	default:
		warning("edward-1950",
			"%s: volume operation %d is unsupported by directories",
			sb->s_id, args->opcode);
		ret = RETERR(-ENOTTY);
		break;
	}
	/* drop exclusive access to the volume */
	reiser4_volume_clear_busy(sb);
	return ret;
}

int reiser4_volume_op_file(struct file *file,  struct reiser4_vol_op_args *args)
{
	int ret = 0;
	struct super_block *sb = file_inode(file)->i_sb;

	/*
	 * take exclusive access to the volume
	 */
	if (reiser4_volume_test_set_busy(sb)) {
		warning("", "Operation %d failed: volume %s is busy",
			args->opcode, sb->s_id);
		return RETERR(-EBUSY);
	}
	switch(args->opcode) {
	case REISER4_MIGRATE_FILE:
		ret = reiser4_migrate_file(file, args->s.brick_idx);
		break;
	case REISER4_SET_FILE_IMMOBILE:
		ret = reiser4_set_file_immobile(file);
		break;
	case REISER4_CLR_FILE_IMMOBILE:
		ret = reiser4_clr_file_immobile(file);
		break;
	default:
		warning("edward-1952",
			"%s: volume operation %d is unsupported by regular files",
			sb->s_id, args->opcode);
		ret = RETERR(-ENOTTY);
		break;
	}
	/* drop exclusive access to the volume */
	reiser4_volume_clear_busy(sb);
	return ret;
}

long reiser4_ioctl_volume(struct file *file,
			  unsigned int cmd, unsigned long arg,
			  int (*volume_op)(struct file *file,
					   struct reiser4_vol_op_args *args))
{
	int ret;
	reiser4_context *ctx;

	ctx = reiser4_init_context(file_inode(file)->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	switch (cmd) {
	case REISER4_IOC_VOLUME: {
		struct reiser4_vol_op_args *op_args;

		if (!capable(CAP_SYS_ADMIN))
			return RETERR(-EPERM);

		op_args = memdup_user((void __user *)arg, sizeof(*op_args));
		if (IS_ERR(op_args))
			return PTR_ERR(op_args);

		ret = volume_op(file, op_args);
		if (ret) {
			warning("edward-1899",
				"On-line volume operation failed (%d)", ret);
			kfree(op_args);
			break;
		}
		if (copy_to_user((struct reiser4_vol_op_args __user *)arg,
				 op_args, sizeof(*op_args)))
			ret = RETERR(-EFAULT);
		kfree(op_args);
		break;
	}
	default:
		ret = RETERR(-ENOTTY);
		break;
	}
	reiser4_exit_context(ctx);
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