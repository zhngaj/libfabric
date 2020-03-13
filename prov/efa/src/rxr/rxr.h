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

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#ifndef _RXR_H_
#define _RXR_H_

#include <pthread.h>
#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_trigger.h>

#include <ofi.h>
#include <ofi_iov.h>
#include <ofi_proto.h>
#include <ofi_enosys.h>
#include <ofi_rbuf.h>
#include <ofi_list.h>
#include <ofi_util.h>
#include <ofi_tree.h>
#include <uthash.h>
#include <ofi_recvwin.h>
#include <ofi_perf.h>

#include <sys/wait.h>
#include "rxr_pkt_entry.h"
#include "rxr_pkt_type.h"

#define RXR_MAJOR_VERSION	(2)
#define RXR_MINOR_VERSION	(0)
#define RXR_PROTOCOL_VERSION	(4)
#define RXR_FI_VERSION		OFI_VERSION_LATEST

#define RXR_IOV_LIMIT		(4)

#ifdef ENABLE_EFA_POISONING
extern const uint32_t rxr_poison_value;
static inline void rxr_poison_mem_region(uint32_t *ptr, size_t size)
{
	int i;

	for (i = 0; i < size / sizeof(rxr_poison_value); i++)
		memcpy(ptr + i, &rxr_poison_value, sizeof(rxr_poison_value));
}
#endif

/*
 * Set alignment to x86 cache line size.
 */
#define RXR_BUF_POOL_ALIGNMENT	(64)

/*
 * will add following parameters to env variable for tuning
 */
#define RXR_RECVWIN_SIZE		(16384)
#define RXR_DEF_CQ_SIZE			(8192)
#define RXR_REMOTE_CQ_DATA_LEN		(8)

/* maximum timeout for RNR backoff (microseconds) */
#define RXR_DEF_RNR_MAX_TIMEOUT		(1000000)
/* bounds for random RNR backoff timeout */
#define RXR_RAND_MIN_TIMEOUT		(40)
#define RXR_RAND_MAX_TIMEOUT		(120)

/* bounds for flow control */
#define RXR_DEF_MAX_RX_WINDOW		(128)
#define RXR_DEF_MAX_TX_CREDITS		(64)
#define RXR_DEF_MIN_TX_CREDITS		(32)

/*
 * maximum time (microseconds) we will allow available_bufs for large msgs to
 * be exhausted
 */
#define RXR_AVAILABLE_DATA_BUFS_TIMEOUT	(5000000)

#if ENABLE_DEBUG
#define RXR_TX_PKT_DBG_SIZE	(16384)
#define RXR_RX_PKT_DBG_SIZE	(16384)
#endif

/*
 * Based on size of tx_id and rx_id in headers, can be arbitrary once those are
 * removed.
 */
#define RXR_MAX_RX_QUEUE_SIZE (UINT32_MAX)
#define RXR_MAX_TX_QUEUE_SIZE (UINT32_MAX)

/*
 * The maximum supported source address length in bytes
 */
#define RXR_MAX_NAME_LENGTH	(32)

/*
 * RxR specific flags that are sent over the wire.
 */
#define RXR_TAGGED		BIT_ULL(0)
#define RXR_REMOTE_CQ_DATA	BIT_ULL(1)
#define RXR_REMOTE_SRC_ADDR	BIT_ULL(2)

/*
 * TODO: In future we will send RECV_CANCEL signal to sender,
 * to stop transmitting large message, this flag is also
 * used for fi_discard which has similar behavior.
 */
#define RXR_RECV_CANCEL		BIT_ULL(3)

/*
 * Flags to tell if the rx_entry is tracking FI_MULTI_RECV buffers
 */
#define RXR_MULTI_RECV_POSTED	BIT_ULL(4)
#define RXR_MULTI_RECV_CONSUMER	BIT_ULL(5)

/*
 * for RMA
 */
#define RXR_WRITE		(1 << 6)
#define RXR_READ_REQ		(1 << 7)
#define RXR_READ_DATA		(1 << 8)

/*
 * Used to provide protocol compatibility across versions that include a
 * credit request along with the RTS and those that do not
 */
#define RXR_CREDIT_REQUEST	BIT_ULL(9)

/*
 * OFI flags
 * The 64-bit flag field is used as follows:
 * 1-grow up    common (usable with multiple operations)
 * 59-grow down operation specific (used for single call/class)
 * 60 - 63      provider specific
 */
#define RXR_NO_COMPLETION	BIT_ULL(60)

/*
 * RM flags
 */
#define RXR_RM_TX_CQ_FULL	BIT_ULL(0)
#define RXR_RM_RX_CQ_FULL	BIT_ULL(1)

#define RXR_MTU_MAX_LIMIT	BIT_ULL(15)


/*
 * Specific flags and attributes for shm provider
 */
#define RXR_SHM_HDR		BIT_ULL(10)
#define RXR_SHM_HDR_DATA	BIT_ULL(11)

extern struct fi_info *shm_info;
extern int efa_cma_cap;

extern struct fi_provider *lower_efa_prov;
extern struct fi_provider rxr_prov;
extern struct fi_info rxr_info;
extern struct rxr_env rxr_env;
extern struct fi_fabric_attr rxr_fabric_attr;
extern struct util_prov rxr_util_prov;
extern struct efa_ep_addr *local_efa_addr;

struct rxr_env {
	int rx_window_size;
	int tx_min_credits;
	int tx_max_credits;
	int tx_queue_size;
	int enable_sas_ordering;
	int enable_shm_transfer;
	int shm_av_size;
	int shm_max_medium_size;
	int recvwin_size;
	int cq_size;
	size_t max_memcpy_size;
	size_t mtu_size;
	size_t tx_size;
	size_t rx_size;
	size_t tx_iov_limit;
	size_t rx_iov_limit;
	int rx_copy_unexp;
	int rx_copy_ooo;
	int max_timeout;
	int timeout_interval;
	size_t efa_cq_read_size;
	size_t shm_cq_read_size;
	size_t efa_max_emulated_read_size;
	size_t efa_max_emulated_write_size;
	size_t efa_read_segment_size;
};

enum rxr_lower_ep_type {
	EFA_EP = 1,
	SHM_EP,
};

enum rxr_x_entry_type {
	RXR_TX_ENTRY = 1,
	RXR_RX_ENTRY,
};

enum rxr_tx_comm_type {
	RXR_TX_FREE = 0,	/* tx_entry free state */
	RXR_TX_SHM_RMA,		/* tx_entry issuing read operation over shm provider */
	RXR_TX_RTS,		/* tx_entry sending RTS message */
	RXR_TX_SEND,		/* tx_entry sending data in progress */
	RXR_TX_QUEUED_SHM_RMA,	/* tx_entry was unable to send RMA operations over shm provider */
	RXR_TX_QUEUED_CTRL,	/* tx_entry was unable to send ctrl packet */
	RXR_TX_QUEUED_RTS_RNR,  /* tx_entry RNR sending RTS packet */
	RXR_TX_QUEUED_DATA_RNR,	/* tx_entry RNR sending data packets */
	RXR_TX_SENT_READRSP,	/* tx_entry (on remote EP) sent
				 * read response (FI_READ only)
				 */
	RXR_TX_QUEUED_READRSP, /* tx_entry (on remote EP) was
				* unable to send read response
				* (FI_READ only)
				*/
	RXR_TX_WAIT_READ_FINISH, /* tx_entry (on initiating EP) wait
				  * for rx_entry to finish receiving
				  * (FI_READ only)
				  */
};

enum rxr_rx_comm_type {
	RXR_RX_FREE = 0,	/* rx_entry free state */
	RXR_RX_INIT,		/* rx_entry ready to recv RTS */
	RXR_RX_UNEXP,		/* rx_entry unexp msg waiting for post recv */
	RXR_RX_MATCHED,		/* rx_entry matched with RTS msg */
	RXR_RX_RECV,		/* rx_entry large msg recv data pkts */
	RXR_RX_QUEUED_CTRL,	/* rx_entry was unable to send ctrl packet */
	RXR_RX_QUEUED_SHM_LARGE_READ,	/* rx_entry was unable to issue RMA Read for large message over shm */
	RXR_RX_QUEUED_EOR,	/* rx_entry was unable to send EOR over shm */
	RXR_RX_QUEUED_CTS_RNR,	/* rx_entry RNR sending CTS */
	RXR_RX_WAIT_READ_FINISH, /* rx_entry wait for send to finish, FI_READ */
};

enum rxr_peer_state {
	RXR_PEER_FREE = 0,	/* rxr_peer free state */
	RXR_PEER_CONNREQ,	/* RTS with endpoint address sent to peer */
	RXR_PEER_ACKED,		/* RXR_CONNACK_PKT received from peer */
};

/* peer is in backoff, not allowed to send */
#define RXR_PEER_IN_BACKOFF (1ULL << 0)
/* peer backoff was increased during this loop of the progress engine */
#define RXR_PEER_BACKED_OFF (1ULL << 1)

struct rxr_fabric {
	struct util_fabric util_fabric;
	struct fid_fabric *lower_fabric;
	struct fid_fabric *shm_fabric;
#ifdef RXR_PERF_ENABLED
	struct ofi_perfset perf_set;
#endif
};

/*
 * Descriptor returned for FI_HMEM peer memory registrations
 */
struct rxr_mr_peer {
	enum fi_hmem_iface      iface;
	union {
		uint64_t        reserved;
		int             cuda;
	} device;
};

struct rxr_mr {
	struct fid_mr mr_fid;
	struct fid_mr *msg_mr;
	struct fid_mr *shm_msg_mr;
	struct rxr_domain *domain;
	struct rxr_mr_peer peer;
};

struct rxr_peer {
	bool tx_init;			/* tracks initialization of tx state */
	bool rx_init;			/* tracks initialization of rx state */
	bool is_local;			/* local/remote peer flag */
	fi_addr_t shm_fiaddr;		/* fi_addr_t addr from shm provider */
	struct rxr_robuf *robuf;	/* tracks expected msg_id on rx */
	uint32_t next_msg_id;		/* sender's view of msg_id */
	enum rxr_peer_state state;	/* state of CM protocol with peer */
	unsigned int rnr_state;		/* tracks RNR backoff for peer */
	size_t tx_pending;		/* tracks pending tx ops to this peer */
	uint16_t tx_credits;		/* available send credits */
	uint16_t rx_credits;		/* available credits to allocate */
	uint64_t rnr_ts;		/* timestamp for RNR backoff tracking */
	int rnr_queued_pkt_cnt;		/* queued RNR packet count */
	int timeout_interval;		/* initial RNR timeout value */
	int rnr_timeout_exp;		/* RNR timeout exponentation calc val */
	struct dlist_entry rnr_entry;	/* linked to rxr_ep peer_backoff_list */
	struct dlist_entry entry;	/* linked to rxr_ep peer_list */
};

struct rxr_queued_ctrl_info {
	int type;
	int inject;
};

struct rxr_rx_entry {
	/* Must remain at the top */
	enum rxr_x_entry_type type;

	fi_addr_t addr;

	/*
	 * freestack ids used to lookup rx_entry during pkt recv
	 */
	uint32_t tx_id;
	uint32_t rx_id;
	uint32_t op;

	/*
	 * The following two varibales are for emulated RMA fi_read only
	 */
	uint32_t rma_loc_tx_id;
	uint32_t rma_initiator_rx_id;

	uint32_t msg_id;

	uint64_t tag;
	uint64_t ignore;

	uint64_t bytes_done;
	int64_t window;
	uint16_t credit_request;
	int credit_cts;

	uint64_t total_len;

	enum rxr_rx_comm_type state;
	struct rxr_queued_ctrl_info queued_ctrl;

	uint64_t fi_flags;
	uint16_t rxr_flags;

	size_t iov_count;
	struct iovec iov[RXR_IOV_LIMIT];

	/* iov_count on sender side, used for large message READ over shm */
	size_t rma_iov_count;
	struct fi_rma_iov rma_iov[RXR_IOV_LIMIT];

	struct fi_cq_tagged_entry cq_entry;

	/* entry is linked with rx entry lists in rxr_ep */
	struct dlist_entry entry;

	/* queued_entry is linked with rx_queued_ctrl_list in rxr_ep */
	struct dlist_entry queued_entry;

	/* Queued packets due to TX queue full or RNR backoff */
	struct dlist_entry queued_pkts;

	/*
	 * A list of rx_entries tracking FI_MULTI_RECV buffers. An rx_entry of
	 * type RXR_MULTI_RECV_POSTED that was created when the multi-recv
	 * buffer was posted is the list head, and the rx_entries of type
	 * RXR_MULTI_RECV_CONSUMER get added to the list as they consume the
	 * buffer.
	 */
	struct dlist_entry multi_recv_consumers;
	struct dlist_entry multi_recv_entry;
	struct rxr_rx_entry *master_entry;

	struct rxr_pkt_entry *unexp_pkt;

#if ENABLE_DEBUG
	/* linked with rx_pending_list in rxr_ep */
	struct dlist_entry rx_pending_entry;
	/* linked with rx_entry_list in rxr_ep */
	struct dlist_entry rx_entry_entry;
#endif
};

struct rxr_tx_entry {
	/* Must remain at the top */
	enum rxr_x_entry_type type;

	uint32_t op;
	fi_addr_t addr;

	/*
	 * freestack ids used to lookup tx_entry during ctrl pkt recv
	 */
	uint32_t tx_id;
	uint32_t rx_id;

	uint32_t msg_id;

	uint64_t tag;

	uint64_t bytes_acked;
	uint64_t bytes_sent;
	int64_t window;
	uint16_t credit_request;
	uint16_t credit_allocated;

	uint64_t total_len;

	enum rxr_tx_comm_type state;
	struct rxr_queued_ctrl_info queued_ctrl;

	uint64_t fi_flags;
	uint64_t send_flags;
	size_t iov_count;
	size_t iov_index;
	size_t iov_offset;
	struct iovec iov[RXR_IOV_LIMIT];

	uint64_t rma_loc_rx_id;
	uint64_t rma_window;
	size_t rma_iov_count;
	struct fi_rma_iov rma_iov[RXR_IOV_LIMIT];

	/* App-provided reg descriptor */
	void *desc[RXR_IOV_LIMIT];

	/* Only used with mr threshold switch from memcpy */
	size_t iov_mr_start;
	struct fid_mr *mr[RXR_IOV_LIMIT];

	struct fi_cq_tagged_entry cq_entry;

	/* entry is linked with tx_pending_list in rxr_ep */
	struct dlist_entry entry;

	/* queued_entry is linked with tx_queued_ctrl_list in rxr_ep */
	struct dlist_entry queued_entry;

	/* Queued packets due to TX queue full or RNR backoff */
	struct dlist_entry queued_pkts;

#if ENABLE_DEBUG
	/* linked with tx_entry_list in rxr_ep */
	struct dlist_entry tx_entry_entry;
#endif
};

#define RXR_GET_X_ENTRY_TYPE(pkt_entry)	\
	(*((enum rxr_x_entry_type *)	\
	 ((unsigned char *)((pkt_entry)->x_entry))))

struct rxr_domain {
	struct util_domain util_domain;
	struct fid_domain *rdm_domain;
	struct fid_domain *shm_domain;

	size_t addrlen;
	uint8_t mr_local;
	uint64_t rdm_mode;
	int do_progress;
	size_t cq_size;
	enum fi_resource_mgmt resource_mgmt;
};

struct rxr_ep {
	struct util_ep util_ep;

	uint8_t core_addr[RXR_MAX_NAME_LENGTH];
	size_t core_addrlen;

	/* per-peer information */
	struct rxr_peer *peer;

	/* free stack for reorder buffer */
	struct rxr_robuf_fs *robuf_fs;

	/* core provider fid */
	struct fid_ep *rdm_ep;
	struct fid_cq *rdm_cq;

	/* shm provider fid */
	struct fid_ep *shm_ep;
	struct fid_cq *shm_cq;

	/*
	 * RxR rx/tx queue sizes. These may be different from the core
	 * provider's rx/tx size and will either limit the number of possible
	 * receives/sends or allow queueing.
	 */
	size_t rx_size;
	size_t tx_size;
	size_t mtu_size;
	size_t rx_iov_limit;
	size_t tx_iov_limit;

	/* core's capabilities */
	uint64_t core_caps;

	/* rx/tx queue size of core provider */
	size_t core_rx_size;
	size_t max_outstanding_tx;
	size_t core_inject_size;
	size_t max_data_payload_size;

	/* Resource management flag */
	uint64_t rm_full;

	/* application's ordering requirements */
	uint64_t msg_order;
	/* core's supported tx/rx msg_order */
	uint64_t core_msg_order;

	/* tx iov limit of core provider */
	size_t core_iov_limit;

	/* threshold to release multi_recv buffer */
	size_t min_multi_recv_size;

	/* buffer pool for send & recv */
	struct ofi_bufpool *tx_pkt_efa_pool;
	struct ofi_bufpool *rx_pkt_efa_pool;

	/*
	 * buffer pool for send & recv for shm as mtu size is different from
	 * the one of efa, and do not require local memory registration
	 */
	struct ofi_bufpool *tx_pkt_shm_pool;
	struct ofi_bufpool *rx_pkt_shm_pool;

	/* staging area for unexpected and out-of-order packets */
	struct ofi_bufpool *rx_unexp_pkt_pool;
	struct ofi_bufpool *rx_ooo_pkt_pool;

#ifdef ENABLE_EFA_POISONING
	size_t tx_pkt_pool_entry_sz;
	size_t rx_pkt_pool_entry_sz;
#endif

	/* datastructure to maintain rxr send/recv states */
	struct ofi_bufpool *tx_entry_pool;
	struct ofi_bufpool *rx_entry_pool;
	/* datastructure to maintain read response */
	struct ofi_bufpool *readrsp_tx_entry_pool;
	/* data structure to maintain RDMA */
	struct ofi_bufpool *read_entry_pool;

	/* rx_entries with recv buf */
	struct dlist_entry rx_list;
	/* rx_entries without recv buf (unexpected message) */
	struct dlist_entry rx_unexp_list;
	/* rx_entries with tagged recv buf */
	struct dlist_entry rx_tagged_list;
	/* rx_entries without tagged recv buf (unexpected message) */
	struct dlist_entry rx_unexp_tagged_list;
	/* list of pre-posted recv buffers */
	struct dlist_entry rx_posted_buf_list;
	/* list of pre-posted recv buffers for shm */
	struct dlist_entry rx_posted_buf_shm_list;
	/* tx entries with queued messages */
	struct dlist_entry tx_entry_queued_list;
	/* rx entries with queued messages */
	struct dlist_entry rx_entry_queued_list;
	/* tx_entries with data to be sent (large messages) */
	struct dlist_entry tx_pending_list;
	/* read entries with data to be read */
	struct dlist_entry read_pending_list;
	/* rxr_peer entries that are in backoff due to RNR */
	struct dlist_entry peer_backoff_list;
	/* rxr_peer entries with an allocated robuf */
	struct dlist_entry peer_list;

#if ENABLE_DEBUG
	/* rx_entries waiting for data to arrive (large messages) */
	struct dlist_entry rx_pending_list;
	/* count of rx_pending_list */
	size_t rx_pending;

	/* rx packets being processed or waiting to be processed */
	struct dlist_entry rx_pkt_list;

	/* tx packets waiting for send completion */
	struct dlist_entry tx_pkt_list;

	/* track allocated rx_entries and tx_entries for endpoint cleanup */
	struct dlist_entry rx_entry_list;
	struct dlist_entry tx_entry_list;

	size_t sends;
	size_t send_comps;
	size_t failed_send_comps;
	size_t recv_comps;
#endif
	/* number of posted buffer for shm */
	size_t posted_bufs_shm;
	size_t rx_bufs_shm_to_post;

	/* number of posted buffers */
	size_t posted_bufs_efa;
	size_t rx_bufs_efa_to_post;
	/* number of buffers available for large messages */
	size_t available_data_bufs;
	/* Timestamp of when available_data_bufs was exhausted. */
	uint64_t available_data_bufs_ts;

	/* number of outstanding sends */
	size_t tx_pending;
};

#define rxr_rx_flags(rxr_ep) ((rxr_ep)->util_ep.rx_op_flags)
#define rxr_tx_flags(rxr_ep) ((rxr_ep)->util_ep.tx_op_flags)

static inline void rxr_copy_shm_cq_entry(struct fi_cq_tagged_entry *cq_tagged_entry,
					 struct fi_cq_data_entry *shm_cq_entry)
{
	cq_tagged_entry->op_context = shm_cq_entry->op_context;
	cq_tagged_entry->flags = shm_cq_entry->flags;
	cq_tagged_entry->len = shm_cq_entry->len;
	cq_tagged_entry->buf = shm_cq_entry->buf;
	cq_tagged_entry->data = shm_cq_entry->data;
	cq_tagged_entry->tag = 0; // No tag for RMA;

}
static inline struct rxr_peer *rxr_ep_get_peer(struct rxr_ep *ep,
					       fi_addr_t addr)
{
	return &ep->peer[addr];
}

static inline void rxr_ep_peer_init(struct rxr_ep *ep, struct rxr_peer *peer)
{
	assert(!peer->rx_init);
	peer->robuf = freestack_pop(ep->robuf_fs);
	peer->robuf = ofi_recvwin_buf_alloc(peer->robuf,
					    rxr_env.recvwin_size);
	assert(peer->robuf);
	dlist_insert_tail(&peer->entry, &ep->peer_list);
	peer->rx_credits = rxr_env.rx_window_size;
	peer->rx_init = 1;

	/*
	 * If the endpoint has never sent a message to this peer thus far,
	 * initialize tx state as well.
	 */
	if (!peer->tx_init) {
		peer->tx_credits = rxr_env.tx_max_credits;
		peer->tx_init = 1;
	}
}

struct rxr_rx_entry *rxr_ep_get_rx_entry(struct rxr_ep *ep,
					 const struct iovec *iov,
					 size_t iov_count, uint64_t tag,
					 uint64_t ignore, void *context,
					 fi_addr_t addr, uint32_t op,
					 uint64_t flags);

struct rxr_rx_entry *rxr_ep_rx_entry_init(struct rxr_ep *ep,
					  struct rxr_rx_entry *rx_entry,
					  const struct iovec *iov,
					  size_t iov_count, uint64_t tag,
					  uint64_t ignore, void *context,
					  fi_addr_t addr, uint32_t op,
					  uint64_t flags);

void rxr_tx_entry_init(struct rxr_ep *rxr_ep, struct rxr_tx_entry *tx_entry,
		       const struct fi_msg *msg, uint32_t op, uint64_t flags);

struct rxr_tx_entry *rxr_ep_alloc_tx_entry(struct rxr_ep *rxr_ep,
					   const struct fi_msg *msg,
					   uint32_t op,
					   uint64_t tag,
					   uint64_t flags);

static inline void rxr_release_tx_entry(struct rxr_ep *ep,
					struct rxr_tx_entry *tx_entry)
{
#if ENABLE_DEBUG
	dlist_remove(&tx_entry->tx_entry_entry);
#endif
	assert(dlist_empty(&tx_entry->queued_pkts));
#ifdef ENABLE_EFA_POISONING
	rxr_poison_mem_region((uint32_t *)tx_entry,
			      sizeof(struct rxr_tx_entry));
#endif
	tx_entry->state = RXR_TX_FREE;
	ofi_buf_free(tx_entry);
}

static inline void rxr_release_rx_entry(struct rxr_ep *ep,
					struct rxr_rx_entry *rx_entry)
{
#if ENABLE_DEBUG
	dlist_remove(&rx_entry->rx_entry_entry);
#endif
	assert(dlist_empty(&rx_entry->queued_pkts));
#ifdef ENABLE_EFA_POISONING
	rxr_poison_mem_region((uint32_t *)rx_entry,
			      sizeof(struct rxr_rx_entry));
#endif
	rx_entry->state = RXR_RX_FREE;
	ofi_buf_free(rx_entry);
}

static inline void *rxr_pkt_start(struct rxr_pkt_entry *pkt_entry)
{
	return (void *)((char *)pkt_entry + sizeof(*pkt_entry));
}

static inline struct rxr_ctrl_cq_pkt *rxr_get_ctrl_cq_pkt(void *pkt)
{
	return (struct rxr_ctrl_cq_pkt *)pkt;
}

static inline struct rxr_ctrl_pkt *rxr_get_ctrl_pkt(void *pkt)
{
	return (struct rxr_ctrl_pkt *)pkt;
}

static inline int rxr_match_addr(fi_addr_t addr, fi_addr_t match_addr)
{
	return (addr == FI_ADDR_UNSPEC || addr == match_addr);
}

static inline int rxr_match_tag(uint64_t tag, uint64_t ignore,
				uint64_t match_tag)
{
	return ((tag | ignore) == (match_tag | ignore));
}

static inline void rxr_ep_inc_tx_pending(struct rxr_ep *ep,
					 struct rxr_peer *peer)
{
	ep->tx_pending++;
	peer->tx_pending++;
#if ENABLE_DEBUG
	ep->sends++;
#endif
}

static inline void rxr_ep_dec_tx_pending(struct rxr_ep *ep,
					 struct rxr_peer *peer,
					 int failed)
{
	ep->tx_pending--;
	peer->tx_pending--;
#if ENABLE_DEBUG
	if (failed)
		ep->failed_send_comps++;
#endif
}

static inline size_t rxr_get_rx_pool_chunk_cnt(struct rxr_ep *ep)
{
	return MIN(ep->core_rx_size, ep->rx_size);
}

static inline size_t rxr_get_tx_pool_chunk_cnt(struct rxr_ep *ep)
{
	return MIN(ep->max_outstanding_tx, ep->tx_size);
}

static inline int rxr_need_sas_ordering(struct rxr_ep *ep)
{
	/*
	 * RxR needs to reorder RTS packets for send-after-send guarantees
	 * only if the application requested it and the core provider does not
	 * support it.
	 */
	return ((ep->msg_order & FI_ORDER_SAS) &&
		!(ep->core_msg_order & FI_ORDER_SAS) &&
		rxr_env.enable_sas_ordering);
}

/* Initialization functions */
void rxr_reset_rx_tx_to_core(const struct fi_info *user_info,
			     struct fi_info *core_info);
int rxr_get_lower_rdm_info(uint32_t version, const char *node, const char *service,
			   uint64_t flags, const struct util_prov *util_prov,
			   const struct fi_info *util_hints,
			   struct fi_info **core_info);
int rxr_fabric(struct fi_fabric_attr *attr,
	       struct fid_fabric **fabric, void *context);
int rxr_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		    struct fid_domain **dom, void *context);
int rxr_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		struct fid_cq **cq_fid, void *context);
int rxr_endpoint(struct fid_domain *domain, struct fi_info *info,
		 struct fid_ep **ep, void *context);

/* EP sub-functions */
void rxr_ep_progress(struct util_ep *util_ep);
void rxr_ep_progress_internal(struct rxr_ep *rxr_ep);
int rxr_ep_post_buf(struct rxr_ep *ep, uint64_t flags, enum rxr_lower_ep_type lower_ep);

int rxr_ep_set_tx_credit_request(struct rxr_ep *rxr_ep,
				 struct rxr_tx_entry *tx_entry);

void rxr_inline_mr_reg(struct rxr_domain *rxr_domain,
		       struct rxr_tx_entry *tx_entry, bool is_shm);

struct rxr_rx_entry *rxr_ep_alloc_unexp_rx_entry_for_rts(struct rxr_ep *ep,
							 struct rxr_pkt_entry *pkt_entry);

struct rxr_rx_entry *rxr_ep_alloc_unexp_rx_entry_for_msgrtm(struct rxr_ep *ep,
							    struct rxr_pkt_entry **pkt_entry);

struct rxr_rx_entry *rxr_ep_alloc_unexp_rx_entry_for_tagrtm(struct rxr_ep *ep,
							    struct rxr_pkt_entry **pkt_entry);

struct rxr_rx_entry *rxr_ep_split_rx_entry(struct rxr_ep *ep,
					   struct rxr_rx_entry *posted_entry,
					   struct rxr_rx_entry *consumer_entry,
					   struct rxr_pkt_entry *pkt_entry);
int rxr_ep_efa_addr_to_str(const void *addr, char *temp_name);

/* CQ sub-functions */
int rxr_cq_handle_rx_error(struct rxr_ep *ep, struct rxr_rx_entry *rx_entry,
			   ssize_t prov_errno);
int rxr_cq_handle_tx_error(struct rxr_ep *ep, struct rxr_tx_entry *tx_entry,
			   ssize_t prov_errno);
int rxr_cq_handle_cq_error(struct rxr_ep *ep, ssize_t err);

void rxr_cq_write_rx_completion(struct rxr_ep *ep,
				struct rxr_rx_entry *rx_entry);

void rxr_cq_handle_rx_completion(struct rxr_ep *ep,
				 struct rxr_pkt_entry *pkt_entry,
				 struct rxr_rx_entry *rx_entry);

void rxr_cq_write_tx_completion(struct rxr_ep *ep,
				struct rxr_tx_entry *tx_entry);

void rxr_cq_handle_tx_completion(struct rxr_ep *ep,
				 struct rxr_tx_entry *tx_entry);

int rxr_cq_reorder_msg(struct rxr_ep *ep,
		       struct rxr_peer *peer,
		       struct rxr_pkt_entry *pkt_entry);

void rxr_cq_proc_pending_items_in_recvwin(struct rxr_ep *ep,
					  struct rxr_peer *peer);

void rxr_cq_handle_shm_rma_write_data(struct rxr_ep *ep,
				      struct fi_cq_data_entry *shm_comp,
				      fi_addr_t src_addr);

/* Aborts if unable to write to the eq */
static inline void efa_eq_write_error(struct util_ep *ep, ssize_t err,
				      ssize_t prov_errno)
{
	struct fi_eq_err_entry err_entry;
	int ret = -FI_ENOEQ;

	FI_WARN(&rxr_prov, FI_LOG_EQ, "Writing error %s to EQ.\n",
		fi_strerror(err));
	if (ep->eq) {
		memset(&err_entry, 0, sizeof(err_entry));
		err_entry.err = err;
		err_entry.prov_errno = prov_errno;
		ret = fi_eq_write(&ep->eq->eq_fid, FI_NOTIFY,
				  &err_entry, sizeof(err_entry),
				  UTIL_FLAG_ERROR);

		if (ret == sizeof(err_entry))
			return;
	}

	FI_WARN(&rxr_prov, FI_LOG_EQ,
		"Unable to write to EQ: %s. err: %s (%zd) prov_errno: %s (%zd)\n",
		fi_strerror(-ret), fi_strerror(err), err,
		fi_strerror(prov_errno), prov_errno);
	fprintf(stderr,
		"Unable to write to EQ: %s. err: %s (%zd) prov_errno: %s (%zd) %s:%d\n",
		fi_strerror(-ret), fi_strerror(err), err,
		fi_strerror(prov_errno), prov_errno, __FILE__, __LINE__);
	abort();
}

static inline struct rxr_domain *rxr_ep_domain(struct rxr_ep *ep)
{
	return container_of(ep->util_ep.domain, struct rxr_domain, util_domain);
}

static inline uint8_t rxr_ep_mr_local(struct rxr_ep *ep)
{
	struct rxr_domain *domain = container_of(ep->util_ep.domain,
						 struct rxr_domain,
						 util_domain);
	return domain->mr_local;
}

/*
 * today we have only cq res check, in future we will have ctx, and other
 * resource check as well.
 */
static inline uint64_t is_tx_res_full(struct rxr_ep *ep)
{
	return ep->rm_full & RXR_RM_TX_CQ_FULL;
}

static inline uint64_t is_rx_res_full(struct rxr_ep *ep)
{
	return ep->rm_full & RXR_RM_RX_CQ_FULL;
}

static inline void rxr_rm_rx_cq_check(struct rxr_ep *ep, struct util_cq *rx_cq)
{
	fastlock_acquire(&rx_cq->cq_lock);
	if (ofi_cirque_isfull(rx_cq->cirq))
		ep->rm_full |= RXR_RM_RX_CQ_FULL;
	else
		ep->rm_full &= ~RXR_RM_RX_CQ_FULL;
	fastlock_release(&rx_cq->cq_lock);
}

static inline void rxr_rm_tx_cq_check(struct rxr_ep *ep, struct util_cq *tx_cq)
{
	fastlock_acquire(&tx_cq->cq_lock);
	if (ofi_cirque_isfull(tx_cq->cirq))
		ep->rm_full |= RXR_RM_TX_CQ_FULL;
	else
		ep->rm_full &= ~RXR_RM_TX_CQ_FULL;
	fastlock_release(&tx_cq->cq_lock);
}

static inline bool rxr_peer_timeout_expired(struct rxr_ep *ep,
					    struct rxr_peer *peer,
					    uint64_t ts)
{
	return (ts >= (peer->rnr_ts + MIN(rxr_env.max_timeout,
					  peer->timeout_interval *
					  (1 << peer->rnr_timeout_exp))));
}

/* Performance counter declarations */
#ifdef RXR_PERF_ENABLED
#define RXR_PERF_FOREACH(DECL)	\
	DECL(perf_rxr_tx),	\
	DECL(perf_rxr_recv),	\
	DECL(rxr_perf_size)	\

enum rxr_perf_counters {
	RXR_PERF_FOREACH(OFI_ENUM_VAL)
};

extern const char *rxr_perf_counters_str[];

static inline void rxr_perfset_start(struct rxr_ep *ep, size_t index)
{
	struct rxr_domain *domain = rxr_ep_domain(ep);
	struct rxr_fabric *fabric = container_of(domain->util_domain.fabric,
						 struct rxr_fabric,
						 util_fabric);
	ofi_perfset_start(&fabric->perf_set, index);
}

static inline void rxr_perfset_end(struct rxr_ep *ep, size_t index)
{
	struct rxr_domain *domain = rxr_ep_domain(ep);
	struct rxr_fabric *fabric = container_of(domain->util_domain.fabric,
						 struct rxr_fabric,
						 util_fabric);
	ofi_perfset_end(&fabric->perf_set, index);
}
#else
#define rxr_perfset_start(ep, index) do {} while (0)
#define rxr_perfset_end(ep, index) do {} while (0)
#endif
#endif
