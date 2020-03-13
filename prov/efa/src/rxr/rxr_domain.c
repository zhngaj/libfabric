/*
 * Copyright (c) 2019 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <ofi_util.h>
#include <ofi_recvwin.h>

#include "efa.h"
#include "rxr.h"
#include "rxr_cntr.h"

static struct fi_ops_domain rxr_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = efa_av_open,
	.cq_open = rxr_cq_open,
	.endpoint = rxr_endpoint,
	.scalable_ep = fi_no_scalable_ep,
	.cntr_open = efa_cntr_open,
	.poll_open = fi_poll_create,
	.stx_ctx = fi_no_stx_context,
	.srx_ctx = fi_no_srx_context,
	.query_atomic = fi_no_query_atomic,
	.query_collective = fi_no_query_collective,
};

static int rxr_domain_close(fid_t fid)
{
	int ret;
	struct rxr_domain *rxr_domain;

	rxr_domain = container_of(fid, struct rxr_domain,
				  util_domain.domain_fid.fid);

	ret = fi_close(&rxr_domain->rdm_domain->fid);
	if (ret)
		return ret;

	ret = ofi_domain_close(&rxr_domain->util_domain);
	if (ret)
		return ret;

	if (rxr_env.enable_shm_transfer) {
		ret = fi_close(&rxr_domain->shm_domain->fid);
		if (ret)
			return ret;
	}

	free(rxr_domain);
	return 0;
}

static struct fi_ops rxr_domain_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = rxr_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static int rxr_mr_close(fid_t fid)
{
	struct rxr_domain *rxr_domain;
	struct rxr_mr *rxr_mr;
	struct efa_mem_desc *mr_desc;
	struct util_domain *shm_util_domain;
	int ret;

	rxr_mr = container_of(fid, struct rxr_mr, mr_fid.fid);
	rxr_domain = rxr_mr->domain;

	ret = ofi_mr_map_remove(&rxr_domain->util_domain.mr_map,
				rxr_mr->mr_fid.key);
	if (ret && ret != -FI_ENOKEY)
		FI_WARN(&rxr_prov, FI_LOG_MR,
			"Unable to remove MR entry from util map (%s)\n",
			fi_strerror(-ret));

	ret = fi_close(&rxr_mr->msg_mr->fid);
	if (ret)
		FI_WARN(&rxr_prov, FI_LOG_MR,
			"Unable to close MR\n");

	mr_desc = container_of(&rxr_mr->msg_mr->fid, struct efa_mem_desc, mr_fid.fid);

	if (rxr_env.enable_shm_transfer && rxr_mr->peer.iface == FI_HMEM_SYSTEM) {
		/*
		 * If MR cache enabled, only close shm's mr when use_cnt is 0,
		 * which ensures that no subsequent RMA operations is using this mr.
		 * We further check rxr_mr->shm_msg_mr to see if shm's memory
		 * registration is skipped or not.
		 */
		if (!efa_mr_cache_enable || (efa_mr_cache_enable && !mr_desc->entry->use_cnt
					     && rxr_mr->shm_msg_mr)) {
			ret = fi_close(&rxr_mr->shm_msg_mr->fid);
			if (ret)
				FI_WARN(&rxr_prov, FI_LOG_MR,
					"Unable to close shm MR\n");
		} else if (efa_mr_cache_enable && !mr_desc->entry->use_cnt) {
			/*
			 * In rxr_mr_regattr, if key already exists, shm's memory
			 * registration will be skipped (rxr_mr->shm_msg_mr is NULL).
			 * Only remove shm's key in this case
			 */
			shm_util_domain = container_of(&rxr_domain->shm_domain->fid,
						       struct util_domain, domain_fid.fid);
			fastlock_acquire(&shm_util_domain->lock);
			ret = ofi_mr_map_remove(&shm_util_domain->mr_map,
						rxr_mr->mr_fid.key);
			fastlock_release(&shm_util_domain->lock);
			if (ret)
				FI_WARN(&rxr_prov, FI_LOG_MR,
					"shm's mr_map remove failed\n");
			ofi_atomic_dec32(&shm_util_domain->ref);
		}
	}

	free(rxr_mr);
	return ret;
}

static struct fi_ops rxr_mr_ops = {
	.size = sizeof(struct fi_ops),
	.close = rxr_mr_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

/*
 * The mr key generated in lower EFA registration will be used in SHM
 * registration and mr_map in an unified way
 */
int rxr_mr_regattr(struct fid *domain_fid, const struct fi_mr_attr *attr,
		   uint64_t flags, struct fid_mr **mr)
{
	struct rxr_domain *rxr_domain;
	struct fi_mr_attr *core_attr;
	struct fi_mr_attr *shm_attr;
	struct rxr_mr *rxr_mr;
	uint64_t user_attr_access, core_attr_access;
	int ret;
	bool key_exists;

	rxr_domain = container_of(domain_fid, struct rxr_domain,
				  util_domain.domain_fid.fid);

	rxr_mr = calloc(1, sizeof(*rxr_mr));
	if (!rxr_mr)
		return -FI_ENOMEM;

	/* recorde the memory access permission requested by user */
	user_attr_access = attr->access;
	shm_attr = (struct fi_mr_attr *)attr;

	/* discard const qualifier to override access registered with EFA */
	core_attr = (struct fi_mr_attr *)attr;
	core_attr_access = FI_SEND | FI_RECV;
	core_attr->access = core_attr_access;

	ret = fi_mr_regattr(rxr_domain->rdm_domain, core_attr, flags,
			    &rxr_mr->msg_mr);
	if (ret) {
		FI_WARN(&rxr_prov, FI_LOG_MR,
			"Unable to register MR buf (%s): %p len: %zu\n",
			fi_strerror(-ret), attr->mr_iov->iov_base,
			attr->mr_iov->iov_len);
		goto err;
	}

	rxr_mr->mr_fid.fid.fclass = FI_CLASS_MR;
	rxr_mr->mr_fid.fid.context = attr->context;
	rxr_mr->mr_fid.fid.ops = &rxr_mr_ops;
	rxr_mr->mr_fid.mem_desc = rxr_mr->msg_mr;
	rxr_mr->mr_fid.key = fi_mr_key(rxr_mr->msg_mr);
	rxr_mr->domain = rxr_domain;
	rxr_mr->peer.iface = attr->iface;
	if (attr->iface == FI_HMEM_CUDA)
		rxr_mr->peer.device.cuda = attr->device.cuda;
	*mr = &rxr_mr->mr_fid;

	assert(rxr_mr->mr_fid.key != FI_KEY_NOTAVAIL);
	key_exists = false;
	core_attr->requested_key = rxr_mr->mr_fid.key;
	core_attr->access = core_attr_access;
	ret = ofi_mr_map_insert(&rxr_domain->util_domain.mr_map, core_attr,
				&rxr_mr->mr_fid.key, mr);
	if (ret) {
		if (efa_mr_cache_enable && ret == -FI_ENOKEY) {
			key_exists = true;
		} else {
			FI_WARN(&rxr_prov, FI_LOG_MR,
				"Unable to add MR to map buf (%s): %p len: %zu\n",
				fi_strerror(-ret), attr->mr_iov->iov_base,
				attr->mr_iov->iov_len);
			goto err;
		}
	}

	/* Call shm provider to register memory */
	if (rxr_env.enable_shm_transfer
	    && !key_exists
	    && attr->iface == FI_HMEM_SYSTEM) {
		shm_attr->access = user_attr_access;
		shm_attr->requested_key = rxr_mr->mr_fid.key;
		ret = fi_mr_regattr(rxr_domain->shm_domain, shm_attr, flags,
				    &rxr_mr->shm_msg_mr);
		if (ret) {
			FI_WARN(&rxr_prov, FI_LOG_MR,
				"Unable to register shm MR buf (%s): %p len: %zu\n",
				fi_strerror(-ret), attr->mr_iov->iov_base,
				attr->mr_iov->iov_len);
			fi_close(&rxr_mr->msg_mr->fid);
			ofi_mr_map_remove(&rxr_domain->util_domain.mr_map,
					  rxr_mr->mr_fid.key);
			goto err;
		}
	}

	return 0;
err:
	free(rxr_mr);
	return ret;
}

int rxr_mr_regv(struct fid *domain_fid, const struct iovec *iov,
		size_t count, uint64_t access, uint64_t offset,
		uint64_t requested_key, uint64_t flags,
		struct fid_mr **mr_fid, void *context)
{
	struct fi_mr_attr attr;

	attr.mr_iov = iov;
	attr.iov_count = count;
	attr.access = access;
	attr.offset = offset;
	attr.requested_key = requested_key;
	attr.context = context;
	attr.iface = FI_HMEM_SYSTEM;
	return rxr_mr_regattr(domain_fid, &attr, flags, mr_fid);
}

static int rxr_mr_reg(struct fid *domain_fid, const void *buf, size_t len,
		      uint64_t access, uint64_t offset,
		      uint64_t requested_key, uint64_t flags,
		      struct fid_mr **mr, void *context)
{
	struct iovec iov;

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	return rxr_mr_regv(domain_fid, &iov, 1, access, offset, requested_key,
			   flags, mr, context);
}

static struct fi_ops_mr rxr_domain_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = rxr_mr_reg,
	.regv = rxr_mr_regv,
	.regattr = rxr_mr_regattr,
};

int rxr_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		    struct fid_domain **domain, void *context)
{
	int ret, retv;
	struct fi_info *rdm_info;
	struct rxr_domain *rxr_domain;
	struct rxr_fabric *rxr_fabric;

	rxr_fabric = container_of(fabric, struct rxr_fabric,
				  util_fabric.fabric_fid);

	if (info->ep_attr->type == FI_EP_DGRAM)
		return fi_domain(rxr_fabric->lower_fabric, info, domain,
				 context);

	rxr_info.addr_format = info->addr_format;

	/*
	 * Set the RxR's tx/rx size here based on core provider the user
	 * selected so that ofi_prov_check_info succeeds.
	 *
	 * TODO: handle the case where a single process opens multiple domains
	 * with different core providers
	 */
	rxr_info.tx_attr->size = info->tx_attr->size;
	rxr_info.rx_attr->size = info->rx_attr->size;

	rxr_info.rx_attr->op_flags |= info->rx_attr->op_flags & FI_MULTI_RECV;

	rxr_domain = calloc(1, sizeof(*rxr_domain));
	if (!rxr_domain)
		return -FI_ENOMEM;

	ret = rxr_get_lower_rdm_info(fabric->api_version, NULL, NULL, 0,
				     &rxr_util_prov, info, &rdm_info);
	if (ret)
		goto err_free_domain;

	ret = fi_domain(rxr_fabric->lower_fabric, rdm_info,
			&rxr_domain->rdm_domain, context);
	if (ret)
		goto err_free_core_info;

	/* Open shm provider's access domain */
	if (rxr_env.enable_shm_transfer) {
		assert(!strcmp(shm_info->fabric_attr->name, "shm"));
		ret = fi_domain(rxr_fabric->shm_fabric, shm_info,
				&rxr_domain->shm_domain, context);
		if (ret)
			goto err_close_core_domain;
	}

	rxr_domain->rdm_mode = rdm_info->mode;
	rxr_domain->addrlen = (info->src_addr) ?
				info->src_addrlen : info->dest_addrlen;
	rxr_domain->cq_size = MAX(info->rx_attr->size + info->tx_attr->size,
				  rxr_env.cq_size);
	rxr_domain->mr_local = ofi_mr_local(rdm_info);
	rxr_domain->resource_mgmt = rdm_info->domain_attr->resource_mgmt;

	ret = ofi_domain_init(fabric, info, &rxr_domain->util_domain, context);
	if (ret)
		goto err_close_shm_domain;

	rxr_domain->do_progress = 0;

	/*
	 * ofi_domain_init() would have stored the RxR mr_modes in the mr_map, but
	 * we need the rbtree insertions and lookups to use EFA provider's
	 * specific key, so unset the FI_MR_PROV_KEY bit for mr_map.
	 */
	rxr_domain->util_domain.mr_map.mode &= ~FI_MR_PROV_KEY;

	*domain = &rxr_domain->util_domain.domain_fid;
	(*domain)->fid.ops = &rxr_domain_fi_ops;
	(*domain)->ops = &rxr_domain_ops;
	(*domain)->mr = &rxr_domain_mr_ops;
	fi_freeinfo(rdm_info);
	return 0;

err_close_shm_domain:
	if (rxr_env.enable_shm_transfer) {
		retv = fi_close(&rxr_domain->shm_domain->fid);
		if (retv)
			FI_WARN(&rxr_prov, FI_LOG_DOMAIN,
				"Unable to close shm domain: %s\n", fi_strerror(-retv));
	}
err_close_core_domain:
	retv = fi_close(&rxr_domain->rdm_domain->fid);
	if (retv)
		FI_WARN(&rxr_prov, FI_LOG_DOMAIN,
			"Unable to close domain: %s\n", fi_strerror(-retv));
err_free_core_info:
	fi_freeinfo(rdm_info);
err_free_domain:
	free(rxr_domain);
	return ret;
}
