/*
 * Copyright (c) 2013-2018 Intel Corporation. All rights reserved
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
#include <sys/uio.h>

#include "fi_iov.h"
#include "smr.h"

extern struct fi_ops_msg smr_msg_ops;
extern struct fi_ops_tagged smr_tagged_ops;
extern struct fi_ops_rma smr_rma_ops;
extern struct fi_ops_atomic smr_atomic_ops;

int smr_setname(fid_t fid, void *addr, size_t addrlen)
{
	struct smr_ep *ep;
	char *name;

	ep = container_of(fid, struct smr_ep, util_ep.ep_fid.fid);
	name = strdup(addr);
	if (!name)
		return -FI_ENOMEM;

	if (ep->name)
		free((void *) ep->name);
	ep->name = name;
	return 0;
}

int smr_getname(fid_t fid, void *addr, size_t *addrlen)
{
	struct smr_ep *ep;
	int ret = 0;

	ep = container_of(fid, struct smr_ep, util_ep.ep_fid.fid);
	if (!ep->name)
		return -FI_EADDRNOTAVAIL;

	if (!addr || *addrlen == 0 ||
	    snprintf(addr, *addrlen, "%s", ep->name) >= *addrlen)
		ret = -FI_ETOOSMALL;
	*addrlen = sizeof(struct smr_addr);
	return ret;
}

static struct fi_ops_cm smr_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = smr_setname,
	.getname = smr_getname,
	.getpeer = fi_no_getpeer,
	.connect = fi_no_connect,
	.listen = fi_no_listen,
	.accept = fi_no_accept,
	.reject = fi_no_reject,
	.shutdown = fi_no_shutdown,
};

int smr_getopt(fid_t fid, int level, int optname,
		void *optval, size_t *optlen)
{
	return -FI_ENOPROTOOPT;
}

int smr_setopt(fid_t fid, int level, int optname,
		const void *optval, size_t optlen)
{
	return -FI_ENOPROTOOPT;
}

static struct fi_ops_ep smr_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = fi_no_cancel,
	.getopt = smr_getopt,
	.setopt = smr_setopt,
	.tx_ctx = fi_no_tx_ctx,
	.rx_ctx = fi_no_rx_ctx,
	.rx_size_left = fi_no_rx_size_left,
	.tx_size_left = fi_no_tx_size_left,
};

int smr_verify_peer(struct smr_ep *ep, int peer_id)
{
	int ret;

	if (ep->region->map->peers[peer_id].peer.addr != FI_ADDR_UNSPEC)
		return 0;

	ret = smr_map_to_region(&smr_prov, &ep->region->map->peers[peer_id]);

	return (ret == -ENOENT) ? -FI_EAGAIN : ret;
}

static int smr_match_msg(struct dlist_entry *item, const void *args)
{
	struct smr_match_attr *attr = (struct smr_match_attr *)args;
	struct smr_ep_entry *recv_entry;

	recv_entry = container_of(item, struct smr_ep_entry, entry);
	return smr_match_addr(recv_entry->addr, attr->addr);
}

static int smr_match_tagged(struct dlist_entry *item, const void *args)
{
	struct smr_match_attr *attr = (struct smr_match_attr *)args;
	struct smr_ep_entry *recv_entry;

	recv_entry = container_of(item, struct smr_ep_entry, entry);
	return smr_match_addr(recv_entry->addr, attr->addr) &&
	       smr_match_tag(recv_entry->tag, recv_entry->ignore, attr->tag); 
} 

static int smr_match_unexp(struct dlist_entry *item, const void *args)
{
	struct smr_match_attr *attr = (struct smr_match_attr *)args;
	struct smr_pending_cmd *unexp_msg;

	unexp_msg = container_of(item, struct smr_pending_cmd, entry);
	return smr_match_addr(unexp_msg->cmd.msg.hdr.addr, attr->addr) &&
	       smr_match_tag(unexp_msg->cmd.msg.hdr.tag, attr->ignore,
			     attr->tag);
}

static int smr_match_ctx(struct dlist_entry *item, const void *args)
{
	struct smr_match_attr *attr = (struct smr_match_attr *)args;
	struct smr_pending_cmd *pending_msg;

	pending_msg = container_of(item, struct smr_pending_cmd, entry);
	return pending_msg->cmd.msg.hdr.msg_id == attr->ctx;
}

static void smr_init_recv_queue(struct smr_recv_queue *recv_queue,
				dlist_func_t *match_func)
{
	dlist_init(&recv_queue->recv_list);
	recv_queue->match_recv = match_func;
}

static void smr_init_pending_queue(struct smr_pending_queue *queue,
				   dlist_func_t *match_func)
{
	dlist_init(&queue->msg_list);
	queue->match_msg = match_func;
}

void smr_post_pending(struct smr_ep *ep, struct smr_cmd *cmd)
{
	struct smr_pending_cmd *pend_cmd;

	pend_cmd = freestack_pop(ep->pend_fs);
	pend_cmd->cmd = *cmd;

	dlist_insert_tail(&pend_cmd->entry, &ep->pend_queue.msg_list);
}

void smr_generic_format(struct smr_cmd *cmd, fi_addr_t peer_id, void *context,
			uint32_t op, uint64_t tag, uint8_t datatype,
			uint8_t atomic_op, uint64_t data,
			uint16_t op_flags)
{
	cmd->msg.hdr.op = op;
	cmd->msg.hdr.op_flags = op_flags;
	if (op == ofi_op_tagged) {
		cmd->msg.hdr.tag = tag;
	} else if (op == ofi_op_atomic ||
		 op == ofi_op_atomic_fetch ||
		 op == ofi_op_atomic_compare) {
		cmd->msg.hdr.datatype = datatype;
		cmd->msg.hdr.atomic_op = atomic_op;
	}
	cmd->msg.hdr.msg_id = (uint64_t) context;
	cmd->msg.hdr.addr = peer_id;
	cmd->msg.hdr.data = data;
}

void smr_format_inline(struct smr_cmd *cmd, fi_addr_t peer_id,
		       const struct iovec *iov, size_t count,
		       void *context, uint32_t op, uint64_t tag,
		       uint64_t data, uint16_t op_flags)
{
	smr_generic_format(cmd, peer_id, context, op, tag, 0, 0, data,
			   op_flags);
	cmd->msg.hdr.op_src = smr_src_inline;
	cmd->msg.hdr.size = ofi_copy_from_iov(cmd->msg.data.msg,
					      SMR_MSG_DATA_LEN, iov, count, 0);
}

void smr_format_inject(struct smr_cmd *cmd, fi_addr_t peer_id,
		       const struct iovec *iov, size_t count,
		       void *context, uint32_t op, uint64_t tag,
		       uint64_t data, uint16_t op_flags,
		       struct smr_region *smr,
		       struct smr_inject_buf *tx_buf)
{
	smr_generic_format(cmd, peer_id, context, op, tag, 0, 0,
			   data, op_flags);
	cmd->msg.hdr.op_src = smr_src_inject;
	cmd->msg.hdr.src_data = (char **) tx_buf - (char **) smr;
	cmd->msg.hdr.size = ofi_copy_from_iov(tx_buf->data, SMR_INJECT_SIZE,
					       iov, count, 0);
}

void smr_format_iov(struct smr_cmd *cmd, fi_addr_t peer_id,
		    const struct iovec *iov, size_t count,
		    size_t total_len, void *context, uint32_t op,
		    uint64_t tag, uint64_t data, uint16_t op_flags,
		    struct smr_region *smr,
		    struct smr_resp *resp)
{
	smr_generic_format(cmd, peer_id, context, op, tag, 0, 0, data, op_flags);
	cmd->msg.hdr.op_src = smr_src_iov;
	resp->status = FI_EBUSY;
	resp->msg_id = (uint64_t) context;
	cmd->msg.hdr.src_data = (uint64_t) ((char **) resp - (char **) smr);
	cmd->msg.data.iov_count = count;
	cmd->msg.hdr.size = total_len;

	memcpy(cmd->msg.data.iov, iov, sizeof(*iov) * count);
}

static int smr_ep_close(struct fid *fid)
{
	struct smr_ep *ep;

	ep = container_of(fid, struct smr_ep, util_ep.ep_fid.fid);

	ofi_endpoint_close(&ep->util_ep);

	if (ep->region)
		smr_free(ep->region);

	smr_recv_fs_free(ep->recv_fs);
	smr_unexp_fs_free(ep->unexp_fs);
	smr_pend_fs_free(ep->pend_fs);
	free(ep);
	return 0;
}

static int smr_ep_bind_cq(struct smr_ep *ep, struct util_cq *cq, uint64_t flags)
{
	int ret = 0;

	if (flags & ~(FI_TRANSMIT | FI_RECV)) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"unsupported flags\n");
		return -FI_EBADFLAGS;
	}

	if (((flags & FI_TRANSMIT) && ep->util_ep.tx_cq) ||
	    ((flags & FI_RECV) && ep->util_ep.rx_cq)) {
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"duplicate CQ binding\n");
		return -FI_EINVAL;
	}

	if (flags & FI_TRANSMIT) {
		ep->util_ep.tx_cq = cq;
		ofi_atomic_inc32(&cq->ref);
		ep->tx_comp = cq->wait ? smr_tx_comp_signal : smr_tx_comp;
	}

	if (flags & FI_RECV) {
		ep->util_ep.rx_cq = cq;
		ofi_atomic_inc32(&cq->ref);

		if (cq->wait) {
			ep->rx_comp = (cq->domain->info_domain_caps & FI_SOURCE) ?
				      smr_rx_src_comp_signal :
				      smr_rx_comp_signal;
		} else {
			ep->rx_comp = (cq->domain->info_domain_caps & FI_SOURCE) ?
				      smr_rx_src_comp : smr_rx_comp;
		}
	}

	ret = fid_list_insert(&cq->ep_list,
			      &cq->ep_list_lock,
			      &ep->util_ep.ep_fid.fid);

	return ret;
}

static int smr_ep_bind(struct fid *ep_fid, struct fid *bfid, uint64_t flags)
{
	struct smr_ep *ep;
	struct util_av *av;
	int ret = 0;

	ep = container_of(ep_fid, struct smr_ep, util_ep.ep_fid.fid);
	switch (bfid->fclass) {
	case FI_CLASS_AV:
		av = container_of(bfid, struct util_av, av_fid.fid);
		ret = ofi_ep_bind_av(&ep->util_ep, av);
		if (ret) {
			FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
				"duplicate AV binding\n");
			return -FI_EINVAL;
		}
		break;
	case FI_CLASS_CQ:
		ret = smr_ep_bind_cq(ep, container_of(bfid, struct util_cq,
						      cq_fid.fid), flags);
		break;
	case FI_CLASS_EQ:
		break;
	default:
		FI_WARN(&smr_prov, FI_LOG_EP_CTRL,
			"invalid fid class\n");
		ret = -FI_EINVAL;
		break;
	}
	return ret;
}

static int smr_ep_ctrl(struct fid *fid, int command, void *arg)
{
	struct smr_attr attr;
	struct smr_ep *ep;
	struct smr_av *av;
	int ret;

	ep = container_of(fid, struct smr_ep, util_ep.ep_fid.fid);
	av = container_of(ep->util_ep.av, struct smr_av, util_av);

	switch (command) {
	case FI_ENABLE:
		if (!ep->util_ep.rx_cq || !ep->util_ep.tx_cq)
			return -FI_ENOCQ;
		if (!ep->util_ep.av)
			return -FI_ENOAV;

		attr.name = ep->name;
		attr.rx_count = ep->rx_size;
		attr.tx_count = ep->tx_size;
		ret = smr_create(&smr_prov, av->smr_map, &attr, &ep->region);
		if (ret)
			return ret;
		smr_exchange_all_peers(ep->region);
		break;
	default:
		return -FI_ENOSYS;
	}
	return ret;
}

static struct fi_ops smr_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = smr_ep_close,
	.bind = smr_ep_bind,
	.control = smr_ep_ctrl,
	.ops_open = fi_no_ops_open,
};

static int smr_endpoint_name(char *name, char *addr, size_t addrlen,
			     int pid, int dom_idx, int ep_idx)
{
	memset(name, 0, SMR_NAME_SIZE);
	if (addr) {
		if (addrlen > SMR_NAME_SIZE)
			return -FI_EINVAL;
		snprintf(name, addrlen, "%s", addr);
	} else {
		snprintf(name, SMR_NAME_SIZE, "%d:%d:%d", pid, dom_idx, ep_idx);
	}
	return 0;
}

int smr_endpoint(struct fid_domain *domain, struct fi_info *info,
		  struct fid_ep **ep_fid, void *context)
{
	struct smr_ep *ep;
	struct smr_domain *smr_domain;
	int ret, ep_idx;
	char name[SMR_NAME_SIZE];

	ep = calloc(1, sizeof(*ep));
	if (!ep)
		return -FI_ENOMEM;

	smr_domain = container_of(domain, struct smr_domain, util_domain.domain_fid);

	fastlock_acquire(&smr_domain->util_domain.lock);
	ep_idx = smr_domain->ep_idx++;
	fastlock_release(&smr_domain->util_domain.lock);
	ret = smr_endpoint_name(name, info->src_addr, info->src_addrlen, getpid(),
				smr_domain->dom_idx, ep_idx);
	if (ret)
		goto err2;

	ret = smr_setname(&ep->util_ep.ep_fid.fid, name, SMR_NAME_SIZE);
	if (ret)
		goto err2;

	ep->rx_size = info->rx_attr->size;
	ep->tx_size = info->tx_attr->size;
	ret = ofi_endpoint_init(domain, &smr_util_prov, info, &ep->util_ep, context,
				smr_ep_progress);
	if (ret)
		goto err1;

	ep->recv_fs = smr_recv_fs_create(info->rx_attr->size);
	ep->unexp_fs = smr_unexp_fs_create(info->rx_attr->size);
	ep->pend_fs = smr_pend_fs_create(info->tx_attr->size);
	smr_init_recv_queue(&ep->recv_queue, smr_match_msg);
	smr_init_recv_queue(&ep->trecv_queue, smr_match_tagged);
	smr_init_pending_queue(&ep->unexp_queue, smr_match_unexp);
	smr_init_pending_queue(&ep->pend_queue, smr_match_ctx);

	ep->util_ep.ep_fid.fid.ops = &smr_ep_fi_ops;
	ep->util_ep.ep_fid.ops = &smr_ep_ops;
	ep->util_ep.ep_fid.cm = &smr_cm_ops;
	ep->util_ep.ep_fid.msg = &smr_msg_ops;
	ep->util_ep.ep_fid.tagged = &smr_tagged_ops;
	ep->util_ep.ep_fid.rma = &smr_rma_ops;
	ep->util_ep.ep_fid.atomic = &smr_atomic_ops;

	*ep_fid = &ep->util_ep.ep_fid;
	return 0;

err1:
	free((void *)ep->name);
err2:
	free(ep);
	return ret;
}