// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, Linaro Limited
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/arm_ffa.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/cpumask.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>

#include <tee_drv.h>
#include "optee_ffa.h"
#include "optee_private.h"
#include "optee_rpc_cmd.h"
#include "optee_bench.h"
#include "optee_smc.h"
#include "mitee_memlog.h"
#include "mitee_smc_notify.h"
#include "rpc_callback.h"
#include "dynamic_mem.h"

struct mutex tee_mutex;

struct mitee_ffa_context {
	struct ffa_device *ffa_dev;
	const struct ffa_ops *ffa_ops;
	struct mutex mutex;
	struct rhashtable global_ids;
};

static struct mitee_ffa_context mitee_ffa_ctx;
/*
 * This file implement the FF-A ABI used when communicating with secure world
 * OP-TEE OS via FF-A.
 * This file is divided into the following sections:
 * 1. Maintain a hash table for lookup of a global FF-A memory handle
 * 2. Convert between struct tee_param and struct optee_msg_param
 * 3. Low level support functions to register shared memory in secure world
 * 4. Dynamic shared memory pool based on alloc_pages()
 * 5. Do a normal scheduled call into secure world
 * 6. Driver initialization.
 */

/*
 * 1. Maintain a hash table for lookup of a global FF-A memory handle
 *
 * FF-A assigns a global memory handle for each piece shared memory.
 * This handle is then used when communicating with secure world.
 *
 * Active and retained SHARE ownership use the same preallocated record.
 */
enum mitee_ffa_share_state {
	MITEE_FFA_SHARE_PREPARED = 0,
	MITEE_FFA_SHARE_ACTIVE,
	MITEE_FFA_SHARE_RECLAIMING,
	MITEE_FFA_SHARE_RETAINED,
	MITEE_FFA_SHARE_RELEASED,
};

struct mitee_ffa_share {
	struct optee *owner;
	struct tee_shm *shm;
	struct page **pages;
	size_t num_pages;
	u32 flags;
	u64 global_id;
	void *pool_kaddr;
	size_t pool_size;
	enum mitee_ffa_share_state state;
	bool active_linked;
	struct rhash_head linkage;
	struct list_head retained_node;
};

static void rh_free_fn(void *ptr, void *arg)
{
	struct mitee_ffa_share *share = ptr;
	bool retained_linked = !list_empty(&share->retained_node);

	if (share->state == MITEE_FFA_SHARE_RELEASED && !retained_linked) {
		kfree(share);
		return;
	}

	pr_crit("preserving FF-A SHARE ownership record for handle 0x%llx at transport removal: "
		"state=%d active_linked=%d registered_pages=%d "
		"pool_backing=%d retained_linked=%d\n",
		share->global_id, share->state, share->active_linked,
		!!share->pages, !!share->pool_kaddr, retained_linked);
}

static const struct rhashtable_params shm_rhash_params = {
	.head_offset = offsetof(struct mitee_ffa_share, linkage),
	.key_len = sizeof(u64),
	.key_offset = offsetof(struct mitee_ffa_share, global_id),
	.automatic_shrinking = true,
};

static void mitee_ffa_share_report_active_map(void)
{
	struct mitee_ffa_share *share;
	struct rhashtable_iter iter;
	bool non_released = false;

	mutex_lock(&mitee_ffa_ctx.mutex);
	rhashtable_walk_enter(&mitee_ffa_ctx.global_ids, &iter);
	rhashtable_walk_start(&iter);
	while ((share = rhashtable_walk_next(&iter)) && !IS_ERR(share)) {
		if (share->state != MITEE_FFA_SHARE_RELEASED) {
			non_released = true;
			break;
		}
	}
	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
	mutex_unlock(&mitee_ffa_ctx.mutex);

	if (non_released)
		pr_crit("FF-A active map contains non-released SHARE ownership "
			"before transport unregister\n");
	else if (IS_ERR(share))
		pr_crit("failed to inspect FF-A SHARE active map before "
			"transport unregister: %ld\n",
			PTR_ERR(share));
	else
		pr_info("FF-A active map contains no non-released SHARE ownership "
			"before transport unregister\n");
}

static struct tee_shm *optee_shm_from_ffa_handle(struct optee *optee,
						 u64 global_id)
{
	struct tee_shm *shm = NULL;
	struct mitee_ffa_share *share;

	mutex_lock(&mitee_ffa_ctx.mutex);
	share = rhashtable_lookup_fast(&mitee_ffa_ctx.global_ids, &global_id,
				       shm_rhash_params);
	if (share && share->owner == optee &&
	    share->state == MITEE_FFA_SHARE_ACTIVE)
		shm = share->shm;
	mutex_unlock(&mitee_ffa_ctx.mutex);

	return shm;
}

static int optee_shm_add_ffa_share(struct mitee_ffa_share *share)
{
	int rc;

	mutex_lock(&mitee_ffa_ctx.mutex);
	rc = rhashtable_lookup_insert_fast(&mitee_ffa_ctx.global_ids,
					   &share->linkage,
					   shm_rhash_params);
	if (!rc) {
		share->active_linked = true;
		share->state = MITEE_FFA_SHARE_ACTIVE;
	}
	mutex_unlock(&mitee_ffa_ctx.mutex);

	return rc;
}

static int optee_shm_unlink_ffa_share(struct mitee_ffa_share *share)
{
	int rc;

	if (!share->active_linked)
		return 0;

	mutex_lock(&mitee_ffa_ctx.mutex);
	share->shm = NULL;
	rc = rhashtable_remove_fast(&mitee_ffa_ctx.global_ids,
				    &share->linkage, shm_rhash_params);
	if (!rc)
		share->active_linked = false;
	mutex_unlock(&mitee_ffa_ctx.mutex);

	return rc;
}

static struct mitee_ffa_share *
optee_shm_take_ffa_share(struct optee *optee, u64 global_id)
{
	struct mitee_ffa_share *share;
	int rc;

	mutex_lock(&mitee_ffa_ctx.mutex);
	share = rhashtable_lookup_fast(&mitee_ffa_ctx.global_ids, &global_id,
				       shm_rhash_params);
	if (!share || share->owner != optee) {
		share = NULL;
		mutex_unlock(&mitee_ffa_ctx.mutex);
		return NULL;
	}

	share->shm = NULL;
	rc = rhashtable_remove_fast(&mitee_ffa_ctx.global_ids,
				    &share->linkage, shm_rhash_params);
	if (!rc)
		share->active_linked = false;
	mutex_unlock(&mitee_ffa_ctx.mutex);

	if (rc)
		pr_crit("failed to detach FF-A SHARE handle 0x%llx from active map: %d\n",
			global_id, rc);
	return share;
}

static void optee_shm_mark_ffa_share_reclaiming(struct optee *optee,
						u64 global_id)
{
	struct mitee_ffa_share *share;

	mutex_lock(&mitee_ffa_ctx.mutex);
	share = rhashtable_lookup_fast(&mitee_ffa_ctx.global_ids, &global_id,
				       shm_rhash_params);
	if (share && share->owner == optee &&
	    share->state == MITEE_FFA_SHARE_ACTIVE)
		share->state = MITEE_FFA_SHARE_RECLAIMING;
	mutex_unlock(&mitee_ffa_ctx.mutex);
}

static void mitee_ffa_share_ledger_init(struct optee *optee)
{
	mutex_init(&optee->retained_shares.lock);
	INIT_LIST_HEAD(&optee->retained_shares.retained);
}

static void mitee_ffa_share_retain(struct optee *optee,
				   struct mitee_ffa_share *share)
{
	share->state = MITEE_FFA_SHARE_RETAINED;
	mutex_lock(&optee->retained_shares.lock);
	list_add_tail(&share->retained_node,
		      &optee->retained_shares.retained);
	mutex_unlock(&optee->retained_shares.lock);
}

static struct mitee_ffa_share *
mitee_ffa_share_take_first(struct optee *optee)
{
	struct mitee_ffa_share *share = NULL;

	mutex_lock(&optee->retained_shares.lock);
	if (!list_empty(&optee->retained_shares.retained)) {
		share = list_first_entry(&optee->retained_shares.retained,
					 struct mitee_ffa_share,
					 retained_node);
		list_del_init(&share->retained_node);
	}
	mutex_unlock(&optee->retained_shares.lock);
	return share;
}

static unsigned int mitee_ffa_share_retained_count(struct optee *optee)
{
	struct mitee_ffa_share *share;
	unsigned int count = 0;

	mutex_lock(&optee->retained_shares.lock);
	list_for_each_entry(share, &optee->retained_shares.retained,
			    retained_node)
		count++;
	mutex_unlock(&optee->retained_shares.lock);
	return count;
}

static void mitee_ffa_share_release_pages(struct mitee_ffa_share *share)
{
	size_t n;

	if (share->flags & TEE_SHM_POOL) {
		free_pages((unsigned long)share->pool_kaddr,
			   get_order(share->pool_size));
	} else if (share->flags & TEE_SHM_USER_MAPPED) {
		unpin_user_pages(share->pages, share->num_pages);
	} else {
		for (n = 0; n < share->num_pages; n++)
			put_page(share->pages[n]);
	}

	kfree(share->pages);
	share->state = MITEE_FFA_SHARE_RELEASED;
	kfree(share);
}

static void mitee_ffa_share_transfer_pages(struct optee *optee,
					   struct mitee_ffa_share *share,
					   struct tee_shm *shm)
{
	if (share->flags & TEE_SHM_POOL) {
		share->pool_kaddr = shm->kaddr;
		share->pool_size = shm->size;
		shm->kaddr = NULL;
	} else {
		WARN_ON(share->pages != shm->pages);
		shm->pages = NULL;
		shm->num_pages = 0;
	}
	share->shm = NULL;
	mitee_ffa_share_retain(optee, share);
}

static void mitee_ffa_share_finish_reclaim(struct optee *optee,
					   struct tee_shm *shm,
					   u64 global_handle,
					   int reclaim_rc)
{
	struct mitee_ffa_share *share;

	share = optee_shm_take_ffa_share(optee, global_handle);
	shm->sec_world_id = 0;
	if (!share) {
		pr_crit("missing FF-A SHARE ownership record for handle 0x%llx: "
			"reclaim=%d pages_present=%d pool_backing_present=%d\n",
			global_handle, reclaim_rc, !!shm->pages, !!shm->kaddr);
		return;
	}

	if (reclaim_rc) {
		mitee_ffa_share_transfer_pages(optee, share, shm);
		return;
	}

	share->state = MITEE_FFA_SHARE_RELEASED;
	if (share->active_linked) {
		/* Keep the record alive until the transport destroys the map. */
		share->pages = NULL;
		share->num_pages = 0;
		share->pool_kaddr = NULL;
		share->pool_size = 0;
		return;
	}
	kfree(share);
}

static unsigned int mitee_ffa_share_ledger_deinit(struct optee *optee)
{
	struct mitee_ffa_share *share;
	unsigned int count = 0;

	mutex_lock(&optee->retained_shares.lock);
	list_for_each_entry(share, &optee->retained_shares.retained,
			    retained_node) {
		pr_crit("retaining ordinary FF-A SHARE handle 0x%llx at teardown\n",
			share->global_id);
		count++;
	}
	mutex_unlock(&optee->retained_shares.lock);
	if (count)
		pr_crit("deinitializing with %u ordinary FF-A SHARE handles retained\n",
			count);
	mutex_destroy(&optee->retained_shares.lock);

	return count;
}

/*
 * 2. Convert between struct tee_param and struct optee_msg_param
 *
 * optee_from_msg_param() and optee_to_msg_param() are the main
 * functions.
 */

int from_msg_param_tmp_mem(struct tee_param *p, u32 attr,
			   const struct optee_msg_param *mp)
{
	struct tee_shm *shm;
	phys_addr_t pa;
	int rc;

	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT + attr -
		  OPTEE_MSG_ATTR_TYPE_TMEM_INPUT;
	p->u.memref.size = mp->u.tmem.size;
	shm = (struct tee_shm *)(unsigned long)mp->u.tmem.shm_ref;
	if (!shm) {
		p->u.memref.shm_offs = 0;
		p->u.memref.shm = NULL;
		return 0;
	}

	rc = tee_shm_get_pa(shm, 0, &pa);
	if (rc)
		return rc;

	p->u.memref.shm_offs = mp->u.tmem.buf_ptr - pa;
	p->u.memref.shm = shm;

	if (p->u.memref.size) {
		size_t offs = p->u.memref.shm_offs + p->u.memref.size - 1;

		rc = tee_shm_get_pa(shm, offs, NULL);
		if (rc)
			return rc;
	}

	return 0;
}

int to_msg_param_tmp_mem(struct optee_msg_param *mp,
			 const struct tee_param *p)
{
	phys_addr_t pa;
	int rc;

	mp->attr = OPTEE_MSG_ATTR_TYPE_TMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
	mp->u.tmem.shm_ref = (unsigned long)p->u.memref.shm;
	mp->u.tmem.size = p->u.memref.size;

	if (!p->u.memref.shm) {
		mp->u.tmem.buf_ptr = 0;
		return 0;
	}

	rc = tee_shm_get_pa(p->u.memref.shm, p->u.memref.shm_offs, &pa);
	if (rc)
		return rc;

	mp->u.tmem.buf_ptr = pa;
	mp->attr |= OPTEE_MSG_ATTR_CACHE_PREDEFINED <<
		    OPTEE_MSG_ATTR_CACHE_SHIFT;
	return 0;
}

void from_msg_param_ffa_mem(struct optee *optee, struct tee_param *p,
				   u32 attr, const struct optee_msg_param *mp)
{
	struct tee_shm *shm = NULL;
	u64 offs_high = 0;
	u64 offs_low = 0;

	p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT + attr -
		  OPTEE_MSG_ATTR_TYPE_FMEM_INPUT;
	p->u.memref.size = mp->u.fmem.size;

	if (mp->u.fmem.global_id != OPTEE_MSG_FMEM_INVALID_GLOBAL_ID)
		shm = optee_shm_from_ffa_handle(optee, mp->u.fmem.global_id);
	p->u.memref.shm = shm;

	if (shm) {
		offs_low = mp->u.fmem.offs_low;
		offs_high = mp->u.fmem.offs_high;
	}
	p->u.memref.shm_offs = offs_low | offs_high << 32;
}

/**
 * optee_from_msg_param() - convert from OPTEE_MSG parameters to
 *				struct tee_param
 * @optee:	main service struct
 * @params:	subsystem internal parameter representation
 * @num_params:	number of elements in the parameter arrays
 * @msg_params:	OPTEE_MSG parameters
 *
 * Returns 0 on success or <0 on failure
 */
static noinline int optee_from_msg_param(struct optee *optee,
				    struct tee_param *params, size_t num_params,
				    const struct optee_msg_param *msg_params)
{
	size_t n;
	int rc = 0;

	for (n = 0; n < num_params; n++) {
		struct tee_param *p = params + n;
		const struct optee_msg_param *mp = msg_params + n;
		u32 attr = mp->attr & OPTEE_MSG_ATTR_TYPE_MASK;

		switch (attr) {
		case OPTEE_MSG_ATTR_TYPE_NONE:
			p->attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
			memset(&p->u, 0, sizeof(p->u));
			break;
		case OPTEE_MSG_ATTR_TYPE_VALUE_INPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_VALUE_INOUT:
			optee_from_msg_param_value(p, attr, mp);
			break;
		case OPTEE_MSG_ATTR_TYPE_TMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_TMEM_INOUT:
			rc = from_msg_param_tmp_mem(p, attr, mp);
			if (rc) {
				pr_err("%s: tmp mem err: %d!\n", __func__, rc);
				return rc;
			}
			break;
		case OPTEE_MSG_ATTR_TYPE_FMEM_INPUT:
		case OPTEE_MSG_ATTR_TYPE_FMEM_OUTPUT:
		case OPTEE_MSG_ATTR_TYPE_FMEM_INOUT:
			optee->comm_ops->from_msg_param_mem(optee, p, attr, mp);
			break;
		/*rpc msg do not support RMEM*/
		default:
			pr_err("%s: unsupported mem type: %d!\n", __func__,
			       attr);
			return -EINVAL;
		}
	}

	return 0;
}

int to_msg_param_ffa_mem(struct optee_msg_param *mp,
			 const struct tee_param *p)
{
	struct tee_shm *shm = p->u.memref.shm;

	mp->attr = OPTEE_MSG_ATTR_TYPE_FMEM_INPUT + p->attr -
		   TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;

	if (shm) {
		u64 shm_offs = p->u.memref.shm_offs;

		mp->u.fmem.internal_offs = shm->offset;

		mp->u.fmem.offs_low = shm_offs;
		mp->u.fmem.offs_high = shm_offs >> 32;
		/* Check that the entire offset could be stored. */
		if (mp->u.fmem.offs_high != shm_offs >> 32)
			return -EINVAL;

		mp->u.fmem.global_id = shm->sec_world_id;
	} else {
		memset(&mp->u, 0, sizeof(mp->u));
		mp->u.fmem.global_id = OPTEE_MSG_FMEM_INVALID_GLOBAL_ID;
	}
	mp->u.fmem.size = p->u.memref.size;

	return 0;
}

/**
 * optee_to_msg_param() - convert from struct tee_params to OPTEE_MSG
 *			      parameters
 * @optee:	main service struct
 * @msg_params:	OPTEE_MSG parameters
 * @num_params:	number of elements in the parameter arrays
 * @params:	subsystem itnernal parameter representation
 * Returns 0 on success or <0 on failure
 */
static noinline int optee_to_msg_param(struct optee *optee,
				  struct optee_msg_param *msg_params,
				  size_t num_params,
				  const struct tee_param *params)
{
	size_t n;
	int rc = 0;

	for (n = 0; n < num_params; n++) {
		const struct tee_param *p = params + n;
		struct optee_msg_param *mp = msg_params + n;

		switch (p->attr) {
		case TEE_IOCTL_PARAM_ATTR_TYPE_NONE:
			mp->attr = TEE_IOCTL_PARAM_ATTR_TYPE_NONE;
			memset(&mp->u, 0, sizeof(mp->u));
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT:
			optee_to_msg_param_value(mp, p);
			break;
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT:
		case TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT:
			if (tee_shm_is_registered(p->u.memref.shm) &&
			    optee->comm_ops->type == MITEE_COMM_FFA)
				rc = optee->comm_ops->to_msg_param_mem(mp, p);
			else
				rc = to_msg_param_tmp_mem(mp, p);
			if (rc) {
				pr_err("%s attr: %#llx failed: %d!\n", __func__,
				       p->attr, rc);
				return rc;
			}
			break;
		default:
			pr_err("%s unsupport attr: %#llx!\n", __func__,
			       p->attr);
			return -EINVAL;
		}
	}

	return 0;
}

/*
 * 3. Low level support functions to register shared memory in secure world
 *
 * Functions to register and unregister shared memory both for normal
 * clients and for tee-supplicant.
 */

static int optee_ffa_shm_register(struct tee_context *ctx,
				  struct tee_shm *shm,
				  struct page **pages, size_t num_pages,
				  unsigned long start)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct ffa_device *ffa_dev = mitee_ffa_ctx.ffa_dev;
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;
	struct ffa_mem_region_attributes mem_attr = {
		.receiver = ffa_dev->vm_id,
		.attrs = FFA_MEM_RW,
	};
	struct ffa_mem_ops_args args = {
		.use_txbuf = true,
		.attrs = &mem_attr,
		.nattrs = 1,
	};
	struct mitee_ffa_share *share;
	struct sg_table sgt;
	int rc;
#ifdef MITEE_SHM_DEBUG
	int iter = 0;
#endif

	if (optee->supp_teedev == ctx->teedev) {
		pr_err("%s do not support supp !\n", __func__);
		return -EACCES;
	}

	rc = optee_check_mem_type(start, num_pages);
	if (rc)
		return rc;

	share = kzalloc(sizeof(*share), GFP_KERNEL);
	if (!share)
		return -ENOMEM;
	share->owner = optee;
	share->shm = shm;
	if (!(shm->flags & TEE_SHM_POOL))
		share->pages = pages;
	share->num_pages = num_pages;
	share->flags = shm->flags;
	share->pool_kaddr = shm->kaddr;
	share->pool_size = shm->size;
	share->state = MITEE_FFA_SHARE_PREPARED;
	INIT_LIST_HEAD(&share->retained_node);

#ifdef MITEE_SHM_DEBUG
	pr_info("%s dump page list\n", __func__);
	for (iter = 0; iter < num_pages; iter++) {
		pr_info("page pfn: %#lx\n", page_to_pfn(pages[iter]));
	}
#endif

	rc = sg_alloc_table_from_pages(&sgt, pages, num_pages, 0,
				       num_pages * PAGE_SIZE, GFP_KERNEL);
	if (rc)
		goto err_free_share;
	args.sg = sgt.sgl;
	rc = mem_ops->memory_share(&args);
	sg_free_table(&sgt);
	if (rc)
		goto err_free_share;

	share->global_id = args.g_handle;
	rc = optee_shm_add_ffa_share(share);
	if (rc) {
		int reclaim_rc;

		pr_err("%s add handle failed: %d!\n", __func__, rc);
		share->state = MITEE_FFA_SHARE_RECLAIMING;
		reclaim_rc = mem_ops->memory_reclaim(args.g_handle, 0);
		if (reclaim_rc) {
			pr_crit("retaining SHM pages for FF-A SHARE handle 0x%llx: %d\n",
				args.g_handle, reclaim_rc);
			mitee_ffa_share_transfer_pages(optee, share, shm);
			return -EOWNERDEAD;
		}
		share->state = MITEE_FFA_SHARE_RELEASED;
		kfree(share);
		return rc;
	}

#ifdef MITEE_SHM_DEBUG
	pr_info("%s get handle: %#llx!\n", __func__, args.g_handle);
#endif
	shm->sec_world_id = args.g_handle;

	return 0;

err_free_share:
	kfree(share);
	return rc;
}

static int optee_ffa_shm_unregister(struct tee_context *ctx,
				    struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	struct ffa_device *ffa_dev = mitee_ffa_ctx.ffa_dev;
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;
	u64 global_handle = shm->sec_world_id;
	struct optee_msg_arg *msg_arg;
	int call_rc;
	int reclaim_rc;

	msg_arg = kzalloc(OPTEE_MSG_GET_ARG_SIZE(1), GFP_KERNEL);
	if (!msg_arg) {
		pr_err("%s: failed to allocate unregister SHM command\n",
		       __func__);
	} else {
		msg_arg->cmd = OPTEE_MSG_CMD_UNREGISTER_SHM;
		msg_arg->num_params = 1;
		msg_arg->params[0].attr = OPTEE_MSG_ATTR_TYPE_FMEM_INPUT;
		msg_arg->params[0].u.fmem.global_id = global_handle;

		call_rc = optee->ops->do_call_with_arg(ctx, msg_arg);
		if (call_rc) {
			pr_err("%s: Unregister SHM id 0x%llx rc %d\n",
			       __func__, global_handle, call_rc);
		} else if (msg_arg->ret != TEEC_SUCCESS) {
			pr_warn_ratelimited("%s: secure unregister SHM id 0x%llx ret %#x\n",
					    __func__, global_handle,
					    msg_arg->ret);
		}
	}

	/*
	 * The yielding-call result describes MiTEE's SHM bookkeeping, not FF-A
	 * ownership.  Reclaim is the authoritative ownership transition and must
	 * run even when the secure unregister command failed or was redundant.
	 */
	optee_shm_mark_ffa_share_reclaiming(optee, global_handle);
	reclaim_rc = mem_ops->memory_reclaim(global_handle, 0);
	if (reclaim_rc)
		pr_err("%s: mem_reclain: 0x%llx %d", __func__,
		       global_handle, reclaim_rc);

	mitee_ffa_share_finish_reclaim(optee, shm, global_handle, reclaim_rc);
	kfree(msg_arg);

	return reclaim_rc;
}

static int optee_ffa_shm_unregister_supp(struct tee_context *ctx,
					 struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);
	const struct ffa_mem_ops *mem_ops;
	u64 global_handle = shm->sec_world_id;
	int rc;

	/*
	 * We're skipping the OPTEE_FFA_YIELDING_CALL_UNREGISTER_SHM call
	 * since this is OP-TEE freeing via RPC so it has already retired
	 * this ID.
	 */

	mem_ops = mitee_ffa_ctx.ffa_dev->ops->mem_ops;
	optee_shm_mark_ffa_share_reclaiming(optee, global_handle);
	rc = mem_ops->memory_reclaim(global_handle, 0);
	if (rc)
		pr_err("%s: mem_reclain: 0x%llx %d", __func__,
		       global_handle, rc);

	mitee_ffa_share_finish_reclaim(optee, shm, global_handle, rc);

	return rc;
}

/*
 * 4. Dynamic shared memory pool based on alloc_pages()
 *
 * Implements an OP-TEE specific shared memory pool.
 * The main function is optee_ffa_shm_pool_alloc_pages().
 */

static int pool_ffa_op_alloc(struct tee_shm_pool_mgr *poolm,
			     struct tee_shm *shm, size_t size)
{
	return optee_pool_op_alloc_helper(poolm, shm, size,
					  optee_ffa_shm_register);
}

static void pool_ffa_op_free(struct tee_shm_pool_mgr *poolm,
			     struct tee_shm *shm)
{
	int rc = optee_ffa_shm_unregister(shm->ctx, shm);

	if (rc) {
		pr_crit("retaining SHM backing after FF-A reclaim failure: %d\n",
			rc);
		shm->kaddr = NULL;
		return;
	}
	free_pages((unsigned long)shm->kaddr, get_order(shm->size));
	shm->kaddr = NULL;
}

static void pool_ffa_op_destroy_poolmgr(struct tee_shm_pool_mgr *poolm)
{
	kfree(poolm);
}

static const struct tee_shm_pool_mgr_ops pool_ffa_ops = {
	.alloc = pool_ffa_op_alloc,
	.free = pool_ffa_op_free,
	.destroy_poolmgr = pool_ffa_op_destroy_poolmgr,
};

/**
 * optee_ffa_shm_pool_alloc_pages() - create page-based allocator pool
 *
 * This pool is used with OP-TEE over FF-A. In this case command buffers
 * and such are allocated from kernel's own memory.
 */
static struct tee_shm_pool_mgr *optee_ffa_shm_pool_alloc_pages(void)
{
	struct tee_shm_pool_mgr *mgr = kzalloc(sizeof(*mgr), GFP_KERNEL);

	if (!mgr)
		return ERR_PTR(-ENOMEM);

	mgr->ops = &pool_ffa_ops;

	return mgr;
}

/*
 * 5. Do a normal scheduled call into secure world
 *
 * The function optee_do_call_with_arg() performs a normal scheduled
 * call into secure world. During this call may normal world request help
 * from normal world using RPCs, Remote Procedure Calls. This includes
 * delivery of non-secure interrupts to for instance allow rescheduling of
 * the current task.
 */

/**
 * optee_do_call_with_arg() - Do a FF-A call to enter OP-TEE in secure world
 * @ctx:	calling context
 * @shm:	shared memory holding the message to pass to secure world
 *
 * Does a FF-A call to OP-TEE in secure world and handles eventual resulting
 * Remote Procedure Calls (RPC) from OP-TEE.
 *
 * Returns return code from FF-A, 0 is OK
 */

static struct tee_shm_pool *optee_shm_memremap(struct optee *optee,
					       void **memremaped_shm)
{
	struct ffa_send_direct_data data = { OPTEE_FFA_GET_SHM_CONFIG };
	unsigned long vaddr;
	phys_addr_t paddr;
	size_t size;
	phys_addr_t begin;
	phys_addr_t end;
	void *va;
	struct tee_shm_pool_mgr *priv_mgr;
	struct tee_shm_pool_mgr *dmabuf_mgr;
	int rc = 0;
	void *pool = NULL;
	const int sz = OPTEE_SHM_NUM_PRIV_PAGES * PAGE_SIZE;

	/*
	 * data0: start
	 * data1: size
	 * data2: settings
	 * fast call, cannot be interrupted
	 */
	rc = optee->comm_ops->call(&data);
	if (rc) {
		pr_err("Unexpected error %d", rc);
		return NULL;
	}

	if (data.data2 != OPTEE_SMC_SHM_CACHED) {
		pr_err("only normal cached shared memory supported\n");
		return ERR_PTR(-EINVAL);
	}

	pr_info("get shm pool: %lx size: %lx\n", data.data0, data.data1);

	begin = roundup(data.data0, PAGE_SIZE);
	end = rounddown(data.data0 + data.data1, PAGE_SIZE);
	paddr = begin;
	size = end - begin;

	if (size < 2 * OPTEE_SHM_NUM_PRIV_PAGES * PAGE_SIZE) {
		pr_err("too small shared memory area\n");
		return ERR_PTR(-EINVAL);
	}

	va = memremap(paddr, size, MEMREMAP_WB);
	if (!va) {
		pr_err("shared memory ioremap failed\n");
		return ERR_PTR(-EINVAL);
	}
	vaddr = (unsigned long)va;

	pool = tee_shm_pool_mgr_alloc_res_mem(vaddr, paddr, sz,
					      3 /* 8 bytes aligned */);
	if (IS_ERR(pool))
		goto err_memunmap;
	priv_mgr = pool;

	vaddr += sz;
	paddr += sz;
	size -= sz;

	pool = tee_shm_pool_mgr_alloc_res_mem(vaddr, paddr, size, PAGE_SHIFT);
	if (IS_ERR(pool))
		goto err_free_priv_mgr;
	dmabuf_mgr = pool;

	pool = tee_shm_pool_alloc(priv_mgr, dmabuf_mgr);
	if (IS_ERR(pool))
		goto err_free_dmabuf_mgr;

	*memremaped_shm = va;

	return pool;

err_free_dmabuf_mgr:
	tee_shm_pool_mgr_destroy(dmabuf_mgr);
err_free_priv_mgr:
	tee_shm_pool_mgr_destroy(priv_mgr);
err_memunmap:
	memunmap(va);
	return pool;
}

#if MITEE_FEATURE_CAP_ENABLE
/*
 * 6. Driver initialization
 *
 * During driver inititialization is the OP-TEE Secure Partition is probed
 * to find out which features it supports so the driver can be initialized
 * with a matching configuration.
 */

static bool optee_ffa_api_is_compatbile(struct ffa_device *ffa_dev,
					const struct ffa_ops *ops)
{
	const struct ffa_msg_ops *msg_ops = ops->msg_ops;
	struct ffa_send_direct_data data = { OPTEE_FFA_GET_API_VERSION };
	int rc;

	msg_ops->mode_32bit_set(ffa_dev);

	rc = msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc) {
		pr_err("Unexpected error %d\n", rc);
		return false;
	}
	if (data.data0 != OPTEE_FFA_VERSION_MAJOR ||
	    data.data1 < OPTEE_FFA_VERSION_MINOR) {
		pr_err("Incompatible OP-TEE API version %lu.%lu", data.data0,
		       data.data1);
		return false;
	}

	data = (struct ffa_send_direct_data){ OPTEE_FFA_GET_OS_VERSION };
	rc = msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc) {
		pr_err("Unexpected error %d\n", rc);
		return false;
	}
	if (data.data2)
		pr_info("revision %lu.%lu (%08lx)", data.data0, data.data1,
			data.data2);
	else
		pr_info("revision %lu.%lu", data.data0, data.data1);

	return true;
}

static bool optee_ffa_exchange_caps(struct ffa_device *ffa_dev,
				    const struct ffa_ops *ops,
				    unsigned int *rpc_arg_count)
{
	struct ffa_send_direct_data data = { OPTEE_FFA_EXCHANGE_CAPABILITIES };
	int rc;

	rc = ops->msg_ops->sync_send_receive(ffa_dev, &data);
	if (rc) {
		pr_err("Unexpected error %d", rc);
		return false;
	}
	if (data.data0) {
		pr_err("Unexpected exchange error %lu", data.data0);
		return false;
	}

	*rpc_arg_count = (u8)data.data1;

	return true;
}
#endif

static struct tee_shm_pool *optee_ffa_config_dyn_shm(void)
{
	struct tee_shm_pool_mgr *priv_mgr;
	struct tee_shm_pool_mgr *dmabuf_mgr;
	void *rc;

	rc = optee_ffa_shm_pool_alloc_pages();
	if (IS_ERR(rc))
		return rc;
	priv_mgr = rc;

	rc = optee_ffa_shm_pool_alloc_pages();
	if (IS_ERR(rc)) {
		tee_shm_pool_mgr_destroy(priv_mgr);
		return rc;
	}
	dmabuf_mgr = rc;

	rc = tee_shm_pool_alloc(priv_mgr, dmabuf_mgr);
	if (IS_ERR(rc)) {
		tee_shm_pool_mgr_destroy(priv_mgr);
		tee_shm_pool_mgr_destroy(dmabuf_mgr);
	}

	return rc;
}

static noinline void optee_get_version(struct tee_device *teedev,
				  struct tee_ioctl_version_data *vers)
{
	struct tee_ioctl_version_data v = {
		.impl_id = TEE_IMPL_ID_OPTEE,
		.impl_caps = TEE_OPTEE_CAP_TZ,
		.gen_caps = TEE_GEN_CAP_GP,
	};

	struct optee *optee = tee_get_drvdata(teedev);

	if (teedev != optee->supp_teedev)
		v.gen_caps |= TEE_GEN_CAP_REG_MEM;

	*vers = v;
}

static noinline int optee_open(struct tee_context *ctx)
{
	return optee_open_common(ctx, true);
}

static int optee_shm_register(struct tee_context *ctx, struct tee_shm *shm,
			      struct page **pages, size_t num_pages,
			      unsigned long start)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);

	return optee->comm_ops->shm_register(ctx, shm, pages, num_pages,
					     start);
}

static int optee_shm_unregister(struct tee_context *ctx, struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);

	return optee->comm_ops->shm_unregister(ctx, shm);
}

static int optee_shm_unregister_supp(struct tee_context *ctx,
				     struct tee_shm *shm)
{
	struct optee *optee = tee_get_drvdata(ctx->teedev);

	return optee->comm_ops->shm_unregister_supp(ctx, shm);
}

static const struct tee_driver_ops optee_ffa_clnt_ops = {
	.get_version = optee_get_version,
	.open = optee_open,
	.release = optee_release,
	.open_session = optee_open_session,
	.close_session = optee_close_session,
	.invoke_func = optee_invoke_func,
	.cancel_req = optee_cancel_req,
	.shm_register = optee_shm_register,
	.shm_unregister = optee_shm_unregister,
};

static const struct tee_desc optee_ffa_clnt_desc = {
	.name = DRIVER_NAME "-ffa-clnt",
	.ops = &optee_ffa_clnt_ops,
	.owner = THIS_MODULE,
};

static const struct tee_driver_ops optee_ffa_supp_ops = {
	.get_version = optee_get_version,
	.open = optee_open,
	.release = optee_release_supp,
	.supp_recv = optee_supp_recv,
	.supp_send = optee_supp_send,
	.shm_register = optee_shm_register, /* same as for clnt ops */
	.shm_unregister = optee_shm_unregister_supp,
};

static const struct tee_desc optee_ffa_supp_desc = {
	.name = DRIVER_NAME "-ffa-supp",
	.ops = &optee_ffa_supp_ops,
	.owner = THIS_MODULE,
	.flags = TEE_DESC_PRIVILEGED,
};

static const struct optee_ops optee_param_ops = {
	.do_call_with_arg = optee_do_call_with_arg,
	.to_msg_param = optee_to_msg_param,
	.from_msg_param = optee_from_msg_param,
};

static struct optee *optee_svc;

struct optee *get_optee_drv_state(void)
{
	return optee_svc;
}

/* bind task to specific cores, typ*/
static int mitee_get_cpu_allows(struct device_node* node, struct cpumask *cpus_allowed)
{
	int rc = 0;
	int cpus_num = 0;
	int i = 0;
	u32 *cpu = NULL;

	cpumask_clear(cpus_allowed);

	cpus_num = of_property_count_u32_elems(node, MITEE_CORE_ID);
	if (cpus_num <= 0) {
		pr_info("no cpu limitation(%d), execute freely\n", cpus_num);
		/* defualt value */
		memcpy(cpus_allowed, cpu_online_mask, sizeof(struct cpumask));
		return 0;
	}

	cpu = kcalloc(cpus_num, sizeof(*cpu), GFP_KERNEL);
	if (!cpu) {
		pr_err("kcalloc cpu(%d) failed\n", cpus_num);
		return -ENOMEM;
	}

	rc = of_property_read_u32_array(node, MITEE_CORE_ID, cpu, cpus_num);
	if (rc) {
		pr_err("read dts failed(%d): %s\n", rc, MITEE_CORE_ID);
		goto out;
	}

	for (i = 0; i < cpus_num; i++) {
		if (cpu[i] >= nr_cpu_ids) {
			continue;
		}
		cpumask_set_cpu(cpu[i], cpus_allowed);
	}

	pr_info("bind core list: %*pbl\n", cpumask_pr_args(cpus_allowed));

out:
	kfree(cpu);
	return rc;
}

static int mitee_parse_dts(struct optee *optee)
{
	struct device_node* dev_node = NULL;
	int rc = 0;

	if (!optee)
		return -EINVAL;

	dev_node = of_find_compatible_node(NULL, NULL, MITEE_COMPATIBLE);
	if (!dev_node) {
		pr_err("no such dev: %s\n", MITEE_COMPATIBLE);
		return -ENODEV;
	}

	rc = mitee_get_cpu_allows(dev_node, &optee->cpus_allowed);
	if (rc) {
		pr_err("get cpu allowed failed: %s\n", MITEE_COMPATIBLE);
	}
	of_node_put(dev_node);

	return rc;
}

static int mitee_dynamic_mem_free_ffa(struct optee *optee,
				      uint64_t mem_handle)
{
	struct mem_desc *desc = NULL;
	struct ffa_device *ffa_dev = mitee_ffa_ctx.ffa_dev;
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;
	int rc;

	desc = mitee_dynamic_mem_take(&optee->dynamic_mem, mem_handle);
	if (!desc) {
		 pr_err("mitee: free memory with handle(0x%llx) failed\n", mem_handle);
		 return -EINVAL;
	}

	rc = mem_ops->memory_reclaim(mem_handle, 0);
	if (rc) {
		pr_err("mitee: failed to reclaim dynamic memory 0x%llx\n",
		       mem_handle);
		mitee_dynamic_mem_restore(&optee->dynamic_mem, desc);
		return rc;
	}
	mitee_free_memory_sgt(desc->mem_size, desc->sgt);
	kfree(desc);
	return 0;
}

static int mitee_dynamic_mem_allocate_ffa(struct optee *optee,
					  uint32_t mem_size,
					  uint64_t *mem_handle,
					  void *buf, uint32_t size_in,
					  uint32_t *size_out)
{
	u32 size_aligned;
	struct sg_table *sgt = NULL;
	struct mem_desc *desc;
	int rc;
	struct ffa_device *ffa_dev = mitee_ffa_ctx.ffa_dev;
	const struct ffa_mem_ops *mem_ops = ffa_dev->ops->mem_ops;
	struct ffa_mem_region_attributes mem_attr = {
		.receiver = ffa_dev->vm_id,
		.attrs = FFA_MEM_RW,
	};
	struct ffa_mem_ops_args args = {
		.use_txbuf = true,
		.attrs = &mem_attr,
		.nattrs = 1,
	};

	if (!mem_size || mem_size > U32_MAX - (PAGE_SIZE - 1))
		return -EINVAL;
	size_aligned = roundup(mem_size, PAGE_SIZE);

	rc = mitee_alloc_memory_sgt(size_aligned, &sgt);
	if (rc) {
		pr_err("mitee: failed to allocate 0x%xB dynamic memory with %d\n", size_aligned, rc);
		return rc;
	}

	desc = kmalloc(sizeof(*desc), GFP_KERNEL);
	if (!desc) {
		pr_err("mitee: failed to allocate dynamic memory descriptor\n");
		rc = -ENOMEM;
		goto err_free_sgt;
	}
	desc->sgt = sgt;
	desc->global_id = 0;
	desc->mem_size = size_aligned;
	INIT_LIST_HEAD(&desc->node);

	args.sg = sgt->sgl;
	rc = mem_ops->memory_lend(&args);
	if (rc) {
		pr_err("mitee: failed to lend memory with %d\n", rc);
		goto err_free_desc;
	}

	desc->global_id = args.g_handle;
	mitee_dynamic_mem_add(&optee->dynamic_mem, desc);
	*mem_handle = args.g_handle;
	return 0;
err_free_desc:
	kfree(desc);
err_free_sgt:
	mitee_free_memory_sgt(size_aligned, sgt);
	return rc;
}

uint32_t mitee_rpc_callback(struct optee_msg_param_value *value, void *buf,
			    uint32_t size_in, uint32_t *size_out)
{
	struct optee *optee = get_optee_drv_state();
	uint32_t module_id, sub_cmd, mem_size;
	uint64_t mem_handle;

	if (!optee || !value) {
		pr_err("mitee rpc call: invalid value\n");
		return TEEC_ERROR_BAD_PARAMETERS;
	}

	module_id = (uint32_t)value->b;
	sub_cmd = value->a;
	switch (sub_cmd) {
	case OPTEE_REE_CALLBACK_ALLOCATE_NONSECMEM:
		if (module_id != REE_CALLBACK_MODULE_TEE_FRAMEWORK) {
			goto bad;
		}

		mem_size = (uint32_t)value->c;
		if (mitee_dynamic_mem_allocate_ffa(optee, mem_size, &mem_handle,
						   buf, size_in, size_out)) {
			goto bad;
		}

		value->a = mem_handle;
		value->b = mem_size;
		break;
	case OPTEE_REE_CALLBACK_FREE_NONSECMEM:
		if (module_id != REE_CALLBACK_MODULE_TEE_FRAMEWORK) {
			goto bad;
		}

		mem_handle = value->c;
		if (mitee_dynamic_mem_free_ffa(optee, mem_handle))
			return TEEC_ERROR_COMMUNICATION;
		break;
	//only for test OPTEE_REE_CALLBACK_CALL
	default:
		pr_err("mitee rpc call: unknown cmd 0x%x\n", sub_cmd);
		goto bad;
		break;
	}

	return TEEC_SUCCESS;
bad:
	return TEEC_ERROR_BAD_PARAMETERS;
}

static unsigned int mitee_dynamic_mem_reclaim_all(struct optee *optee)
{
	struct ffa_device *ffa_dev = READ_ONCE(mitee_ffa_ctx.ffa_dev);
	const struct ffa_mem_ops *mem_ops = NULL;
	unsigned int initial_count;
	unsigned int i;
	struct mem_desc *desc;

	if (ffa_dev && ffa_dev->ops)
		mem_ops = ffa_dev->ops->mem_ops;

	initial_count = mitee_dynamic_mem_count(&optee->dynamic_mem);
	for (i = 0; i < initial_count; i++) {
		desc = mitee_dynamic_mem_take_first(&optee->dynamic_mem);
		if (WARN_ON(!desc))
			break;
		if (!mem_ops) {
			pr_crit("dynamic FF-A LEND reclaim incomplete: transport unavailable, retaining handle 0x%llx\n",
				desc->global_id);
			mitee_dynamic_mem_restore(&optee->dynamic_mem, desc);
			continue;
		}
		if (mem_ops->memory_reclaim(desc->global_id, 0)) {
			pr_crit("dynamic FF-A LEND reclaim incomplete: retaining handle 0x%llx\n",
				desc->global_id);
			mitee_dynamic_mem_restore(&optee->dynamic_mem, desc);
			continue;
		}
		mitee_free_memory_sgt(desc->mem_size, desc->sgt);
		kfree(desc);
	}
	return mitee_dynamic_mem_count(&optee->dynamic_mem);
}

static unsigned int mitee_ffa_share_reclaim_retained(struct optee *optee)
{
	struct ffa_device *ffa_dev = READ_ONCE(mitee_ffa_ctx.ffa_dev);
	const struct ffa_mem_ops *mem_ops = NULL;
	struct mitee_ffa_share *share;
	unsigned int initial_count;
	unsigned int i;
	int rc;

	if (ffa_dev && ffa_dev->ops)
		mem_ops = ffa_dev->ops->mem_ops;

	initial_count = mitee_ffa_share_retained_count(optee);
	for (i = 0; i < initial_count; i++) {
		share = mitee_ffa_share_take_first(optee);
		if (WARN_ON(!share))
			break;
		if (share->active_linked) {
			rc = optee_shm_unlink_ffa_share(share);
			if (rc) {
				pr_crit("ordinary FF-A SHARE reclaim incomplete: active-map detach failed for handle 0x%llx: %d\n",
					share->global_id, rc);
				mitee_ffa_share_retain(optee, share);
				continue;
			}
		}
		if (!mem_ops) {
			pr_crit("ordinary FF-A SHARE reclaim incomplete: transport unavailable, retaining handle 0x%llx\n",
				share->global_id);
			mitee_ffa_share_retain(optee, share);
			continue;
		}

		share->state = MITEE_FFA_SHARE_RECLAIMING;
		rc = mem_ops->memory_reclaim(share->global_id, 0);
		if (rc) {
			pr_crit("ordinary FF-A SHARE reclaim incomplete: retaining handle 0x%llx: %d\n",
				share->global_id, rc);
			mitee_ffa_share_retain(optee, share);
			continue;
		}
		mitee_ffa_share_release_pages(share);
	}

	return mitee_ffa_share_retained_count(optee);
}

static void mitee_ffa_share_reclaim_final(struct optee *optee)
{
	unsigned int retained;

	retained = mitee_ffa_share_reclaim_retained(optee);
	if (retained)
		pr_crit("final strict teardown retained %u ordinary FF-A SHARE handles\n",
			retained);
	else
		pr_info("final strict teardown retained 0 ordinary FF-A SHARE handles\n");
}

void mitee_lifecycle_init(struct optee *optee)
{
	mutex_init(&optee->lifecycle_lock);
	optee->lifecycle_state = MITEE_LIFECYCLE_ONLINE;
}

void mitee_lifecycle_uninit(struct optee *optee)
{
	if (WARN_ON(READ_ONCE(optee->lifecycle_state) !=
		    MITEE_LIFECYCLE_STOPPED))
		return;
	mutex_destroy(&optee->lifecycle_lock);
}

int mitee_lifecycle_shutdown(struct optee *optee,
			     enum mitee_shutdown_mode mode)
{
	unsigned int dynamic_retained;
	unsigned int share_retained;
	long drained;
	int rc = 0;

	mutex_lock(&optee->lifecycle_lock);
	if (READ_ONCE(optee->lifecycle_state) == MITEE_LIFECYCLE_STOPPED)
		goto out;

	mutex_lock(&optee->concurrency_lock);
	if (optee->lifecycle_state == MITEE_LIFECYCLE_ONLINE)
		WRITE_ONCE(optee->lifecycle_state,
			   MITEE_LIFECYCLE_QUIESCING);
	mutex_unlock(&optee->concurrency_lock);

	wake_up_all(&optee->concurrency_wq);
	wake_up_all(&optee->supp_ctx_wq);
	optee_wait_queue_abort(&optee->wait_queue);
	optee_supp_shutdown(optee);

	if (READ_ONCE(optee->lifecycle_state) ==
	    MITEE_LIFECYCLE_QUIESCING) {
		drained = wait_event_timeout(optee->concurrency_wq,
					     !READ_ONCE(optee->active_calls),
					     msecs_to_jiffies(5000));
		if (!drained) {
			if (mode == MITEE_SHUTDOWN_REBOOT_BOUNDED) {
				pr_err("bounded reboot quiesce timed out with %u active calls\n",
				       READ_ONCE(optee->active_calls));
				rc = -ETIMEDOUT;
				goto out;
			}
			pr_err("strict teardown quiesce timed out with %u active calls; waiting for drain\n",
			       READ_ONCE(optee->active_calls));
			wait_event(optee->concurrency_wq,
				   !READ_ONCE(optee->active_calls));
		}
		WRITE_ONCE(optee->lifecycle_state, MITEE_LIFECYCLE_DRAINED);
	}

	mitee_workers_deinit(optee);
	dynamic_retained = mitee_dynamic_mem_reclaim_all(optee);
	share_retained = mitee_ffa_share_reclaim_retained(optee);
	WRITE_ONCE(optee->lifecycle_state, MITEE_LIFECYCLE_STOPPED);
	if (dynamic_retained || share_retained)
		pr_err("local lifecycle stopped with:\n"
		       "\t%u dynamic FF-A LEND handles retained\n"
		       "\t%u ordinary FF-A SHARE handles retained\n",
		       dynamic_retained, share_retained);
	else
		pr_info("local lifecycle stopped\n");
out:
	mutex_unlock(&optee->lifecycle_lock);
	return rc;
}

static int mitee_reboot_notify(struct notifier_block *nb,
			       unsigned long event, void *unused)
{
	struct optee *optee = container_of(nb, struct optee, reboot_notifier);

	if (mitee_lifecycle_shutdown(optee,
				     MITEE_SHUTDOWN_REBOOT_BOUNDED))
		pr_err("reboot lifecycle shutdown incomplete\n");

	return NOTIFY_DONE;
}

static int __init mitee_core_init(void)
{
	unsigned int rpc_arg_count = 0;
	struct tee_device *teedev = NULL;
	struct optee *optee = NULL;
	void *memremaped_shm = NULL;
	bool ffa_registered = false;
	bool memlog_added = false;
	bool memlog_ready = false;
	bool queue_ready = false;
	bool tasks_ready = false;
	bool proc_ready = false;
	u32 sec_caps = 0;
	int rc = 0;
	sec_caps |= OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM;
	sec_caps |= OPTEE_SMC_SEC_CAP_DYNAMIC_SHM;
	rpc_arg_count = THREAD_RPC_MAX_NUM_PARAMS;

	pr_info("initializing driver\n");

	rc = mitee_tee_init();
	if (rc)
		return -EINVAL;

	optee = kzalloc(sizeof(*optee), GFP_KERNEL);
	if (!optee) {
		rc = -ENOMEM;
		goto err_tee_exit;
	}

	rc = mitee_parse_dts(optee);
	if (rc) {
		pr_err("failed to parse mitee dts\n");
		goto err_free_optee;
	}

	optee->comm_ops = &mitee_ffa_comm_ops;
	mutex_init(&optee->call_queue.mutex);
	INIT_LIST_HEAD(&optee->call_queue.waiters);
	optee_wait_queue_init(&optee->wait_queue);
	mitee_rpc_callback_queue_init(&optee->cb_queue);
	optee_supp_init(&optee->supp);
	mutex_init(&optee->concurrency_lock);
	init_waitqueue_head(&optee->concurrency_wq);
	init_waitqueue_head(&optee->supp_ctx_wq);
	optee->concurrency_limit = MITEE_WORKER_COUNT;
	optee->active_calls = 0;
	mitee_lifecycle_init(optee);
	ATOMIC_INIT_NOTIFIER_HEAD(&optee->notifier);
	mitee_dynamic_mem_init(&optee->dynamic_mem);
	mitee_ffa_share_ledger_init(optee);

	rc = optee->comm_ops->register_abi();
	if (rc) {
		pr_err("failed to register FF-A transport: %d\n", rc);
		goto err_shutdown;
	}
	ffa_registered = true;

#if MITEE_FEATURE_CAP_ENABLE
	if (!optee_ffa_api_is_compatbile(mitee_ffa_ctx.ffa_dev,
					 mitee_ffa_ctx.ffa_ops)) {
		pr_err("ffa abi unmatch\n");
		rc = -EINVAL;
		goto err_shutdown;
	}

	if (!optee_ffa_exchange_caps(mitee_ffa_ctx.ffa_dev,
				     mitee_ffa_ctx.ffa_ops,
				     &rpc_arg_count)) {
		pr_err("ffa exchange caps fail\n");
		rc = -EINVAL;
		goto err_shutdown;
	}
#endif

	optee->pool = ERR_PTR(-EINVAL);

	if ((sec_caps & OPTEE_SMC_SEC_CAP_DYNAMIC_SHM) &&
	    !(sec_caps & OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM)) {
		optee->pool = optee_ffa_config_dyn_shm();
	}

	if (IS_ERR(optee->pool) &&
	    (sec_caps & OPTEE_SMC_SEC_CAP_HAVE_RESERVED_SHM)) {
		optee->pool = optee_shm_memremap(optee, &memremaped_shm);
	}

	if (IS_ERR(optee->pool)) {
		rc = PTR_ERR(optee->pool);
		optee->pool = NULL;
		goto err_shutdown;
	}

	optee->ops = &optee_param_ops;
	optee->rpc_arg_count = rpc_arg_count;

	teedev = tee_device_alloc(&optee_ffa_clnt_desc, NULL, optee->pool,
				  optee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		optee->teedev = NULL;
		goto err_shutdown;
	}
	optee->teedev = teedev;

	teedev = tee_device_alloc(&optee_ffa_supp_desc, NULL, optee->pool,
				  optee);
	if (IS_ERR(teedev)) {
		rc = PTR_ERR(teedev);
		optee->supp_teedev = NULL;
		goto err_shutdown;
	}
	optee->supp_teedev = teedev;

	optee->memremaped_shm = memremaped_shm;
	optee_svc = optee;

	//rpc callback should register after optee_svc was initialized
	mitee_rpc_register_callback(REE_CALLBACK_MODULE_TEE_FRAMEWORK,
				    OPTEE_REE_CALLBACK_ALLOCATE_NONSECMEM, mitee_rpc_callback);
	mitee_rpc_register_callback(REE_CALLBACK_MODULE_TEE_FRAMEWORK,
				    OPTEE_REE_CALLBACK_FREE_NONSECMEM, mitee_rpc_callback);
	/* mitee log device function START */
	optee->mitee_memlog_pdev = platform_device_alloc("mitee_memlog", 0);
	if (!optee->mitee_memlog_pdev) {
		rc = -ENOMEM;
		goto err_shutdown;
	}

	rc = platform_device_add(optee->mitee_memlog_pdev);
	if (rc) {
		pr_err("failed to add mitee_memlog device (%d)\n", rc);
		goto err_shutdown;
	}
	memlog_added = true;

	rc = mitee_memlog_probe(optee->mitee_memlog_pdev, optee);
	if (rc) {
		pr_err("failed to initial mitee_memlog driver (%d)\n", rc);
		goto err_shutdown;
	}
	memlog_ready = true;
	/* mitee log device function END */

	rc = mitee_msg_queue_init(&optee->msg_queue);
	if (rc) {
		pr_err("failed to init mitee message queue (%d)\n", rc);
		goto err_shutdown;
	}
	queue_ready = true;

	rc = mitee_msg_queue_register(optee);
	if (rc) {
		pr_err("failed to register mitee message queue (%d)\n", rc);
		goto err_shutdown;
	}

	mitee_task_list_init(&optee->tasks);
	tasks_ready = true;
	rc = mitee_proc_init(optee);
	if (rc) {
		pr_err("failed to create mitee concurrency proc node (%d)\n", rc);
		goto err_shutdown;
	}
	proc_ready = true;

	rc = mitee_workers_init(optee);
	if (rc) {
		pr_err("failed to start mitee workers: %d\n", rc);
		goto err_shutdown;
	}

	rc = tee_device_register(optee->teedev);
	if (rc)
		goto err_shutdown;
	rc = tee_device_register(optee->supp_teedev);
	if (rc)
		goto err_shutdown;

	#if MITEE_FEATURE_FW_NP_ENABLE
	rc = optee_enumerate_devices(PTA_CMD_GET_DEVICES);
	if (rc)
		goto err_shutdown;
	#endif

	optee_bm_enable();
	optee->reboot_notifier.notifier_call = mitee_reboot_notify;
	optee->reboot_notifier.priority = 200;
	rc = register_reboot_notifier(&optee->reboot_notifier);
	if (rc) {
		pr_err("failed to register reboot notifier: %d\n", rc);
		optee_bm_disable();
		goto err_shutdown;
	}
	optee->reboot_notifier_registered = true;

	pr_info("initialized driver\n");
	return 0;

err_shutdown:
	if (optee->reboot_notifier_registered) {
		unregister_reboot_notifier(&optee->reboot_notifier);
		optee->reboot_notifier_registered = false;
	}
	mitee_lifecycle_shutdown(optee, MITEE_SHUTDOWN_TEARDOWN_STRICT);
	#if MITEE_FEATURE_FW_NP_ENABLE
	optee_unregister_devices();
	#endif
	tee_device_unregister(optee->teedev);
	tee_device_unregister(optee->supp_teedev);
	optee->teedev = NULL;
	optee->supp_teedev = NULL;
	mitee_ffa_share_reclaim_final(optee);
	if (proc_ready)
		mitee_proc_deinit(optee);
	if (tasks_ready)
		mitee_task_list_deinit(&optee->tasks);
	if (queue_ready)
		mitee_msg_queue_deinit(&optee->msg_queue);
	if (memlog_ready)
		mitee_memlog_remove(optee->mitee_memlog_pdev);
	if (memlog_added) {
		platform_device_unregister(optee->mitee_memlog_pdev);
		optee->mitee_memlog_pdev = NULL;
	} else if (optee->mitee_memlog_pdev) {
		platform_device_put(optee->mitee_memlog_pdev);
		optee->mitee_memlog_pdev = NULL;
	}
	optee_svc = NULL;
	mitee_rpc_callback_queue_deinit(&optee->cb_queue);
	mitee_dynamic_mem_deinit(&optee->dynamic_mem);
	mitee_ffa_share_ledger_deinit(optee);
	optee_supp_release(&optee->supp);
	optee_supp_uninit(&optee->supp);
	optee_wait_queue_exit(&optee->wait_queue);
	mutex_destroy(&optee->call_queue.mutex);
	if (optee->pool)
		tee_shm_pool_free(optee->pool);
	if (memremaped_shm)
		memunmap(memremaped_shm);
	if (ffa_registered)
		optee->comm_ops->unregister_abi();
	mitee_lifecycle_uninit(optee);
err_free_optee:
	kfree(optee);
err_tee_exit:
	mitee_tee_exit();
	return rc;
}

static int mitee_ffa_probe(struct ffa_device *ffa_dev)
{
	const struct ffa_ops *ffa_ops = ffa_dev->ops;
	int rc;

	if (IS_ERR_OR_NULL(ffa_ops) || !ffa_ops->msg_ops ||
	    !ffa_ops->mem_ops) {
		rc = IS_ERR(ffa_ops) ? PTR_ERR(ffa_ops) : -EINVAL;
		pr_err("invalid FF-A ops: %d\n", rc);
		return rc;
	}

	mutex_init(&mitee_ffa_ctx.mutex);
	rc = rhashtable_init(&mitee_ffa_ctx.global_ids, &shm_rhash_params);
	if (rc) {
		pr_err("failed to initialize FF-A handle table: %d\n", rc);
		return rc;
	}
	mitee_ffa_ctx.ffa_dev = ffa_dev;
	mitee_ffa_ctx.ffa_ops = ffa_ops;
	mitee_smc_notify_init(ffa_dev);
	return 0;
}

static void mitee_ffa_remove(struct ffa_device *ffa_dev)
{
	rhashtable_free_and_destroy(&mitee_ffa_ctx.global_ids,
				    rh_free_fn, NULL);
	mitee_smc_notify_deinit();
	mitee_ffa_ctx.ffa_dev = NULL;
	mitee_ffa_ctx.ffa_ops = NULL;
}

int mitee_ffa_call(struct ffa_send_direct_data *data)
{
	struct ffa_send_direct_data local = *data;
	const struct ffa_ops *ops = READ_ONCE(mitee_ffa_ctx.ffa_ops);
	struct ffa_device *ffa_dev = READ_ONCE(mitee_ffa_ctx.ffa_dev);
	int rc;

	if (!ops || !ops->msg_ops || !ffa_dev)
		return -ENODEV;
	rc = ops->msg_ops->sync_send_receive(ffa_dev, &local);
	if (rc) {
		pr_err("Unexpected error %d\n", rc);
		return rc;
	}

	*data = local;
	return 0;
}

static const struct ffa_device_id mitee_ffa_device_id[] = {
	/* 06bb3aea-7b38-4dad-a70ff59986572611 */
	{ UUID_INIT(0x06bb3aea, 0x7b38, 0x4dad, 0xa7, 0x0f, 0xf5, 0x99, 0x86,
		    0x57, 0x26, 0x11) },
	{}
};

static struct ffa_driver mitee_ffa_driver = {
	.name = "mitee",
	.probe = mitee_ffa_probe,
	.remove = mitee_ffa_remove,
	.id_table = mitee_ffa_device_id,
};

int mitee_ffa_register(void)
{
	int rc;

	if (!IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		return -EOPNOTSUPP;
	rc = ffa_register(&mitee_ffa_driver);
	if (rc)
		return rc;
	if (!READ_ONCE(mitee_ffa_ctx.ffa_dev)) {
		ffa_unregister(&mitee_ffa_driver);
		return -ENODEV;
	}
	return 0;
}

void mitee_ffa_unregister(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)) {
		mitee_ffa_share_report_active_map();
		ffa_unregister(&mitee_ffa_driver);
	}
}

const struct mitee_comm_ops mitee_ffa_comm_ops = {
	.type = MITEE_COMM_FFA,
	.register_abi = mitee_ffa_register,
	.unregister_abi = mitee_ffa_unregister,
	.call = mitee_ffa_call,
	.from_msg_param_mem = from_msg_param_ffa_mem,
	.to_msg_param_mem = to_msg_param_ffa_mem,
	.shm_register = optee_ffa_shm_register,
	.shm_unregister = optee_ffa_shm_unregister,
	.shm_unregister_supp = optee_ffa_shm_unregister_supp,
};

static void __exit mitee_core_exit(void)
{
	struct optee *optee = optee_svc;

	if (!optee)
		return;
	if (optee->reboot_notifier_registered) {
		unregister_reboot_notifier(&optee->reboot_notifier);
		optee->reboot_notifier_registered = false;
	}
	mitee_lifecycle_shutdown(optee, MITEE_SHUTDOWN_TEARDOWN_STRICT);

	mitee_proc_deinit(optee);
	#if MITEE_FEATURE_FW_NP_ENABLE
	optee_unregister_devices();
	#endif
	tee_device_unregister(optee->teedev);
	tee_device_unregister(optee->supp_teedev);
	mitee_ffa_share_reclaim_final(optee);
	mitee_task_list_deinit(&optee->tasks);
	mitee_msg_queue_deinit(&optee->msg_queue);
	mitee_memlog_remove(optee->mitee_memlog_pdev);
	platform_device_unregister(optee->mitee_memlog_pdev);
	optee->mitee_memlog_pdev = NULL;
	optee_bm_disable();
	mitee_rpc_callback_queue_deinit(&optee->cb_queue);
	mitee_dynamic_mem_deinit(&optee->dynamic_mem);
	mitee_ffa_share_ledger_deinit(optee);
	optee_supp_release(&optee->supp);
	optee_supp_uninit(&optee->supp);
	optee_wait_queue_exit(&optee->wait_queue);
	mutex_destroy(&optee->call_queue.mutex);
	tee_shm_pool_free(optee->pool);
	if (optee->memremaped_shm)
		memunmap(optee->memremaped_shm);
	optee_svc = NULL;
	optee->comm_ops->unregister_abi();
	mitee_lifecycle_uninit(optee);
	kfree(optee);
	mitee_tee_exit();
}

module_init(mitee_core_init);
module_exit(mitee_core_exit);
