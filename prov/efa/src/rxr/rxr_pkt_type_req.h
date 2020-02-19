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

#ifndef _RXR_PKT_TYPE_REQ_H
#define _RXR_PKT_TYPE_REQ_H

/*
 * This file contain REQ packet type related struct and functions
 * REQ packets can be classifed into 4 categories:
 *    RTM (Request To Message) is used by message
 *    RTW (Request To Write) is used by RMA write
 *    RTR (Request To Read) is used by RMA read
 *    RTA (Request To Atomic) is used by Atomic
 *
 * For each REQ packet type need to have the following:
 *
 *     1. a header struct
 *     2. an init() function called by rxr_pkt_init_ctrl()
 *     3. a handle_sent() function called by rxr_pkt_post_ctrl()
 *     4. a handle_send_completion() function called by
 *               rxr_pkt_handle_send_completion()
 *     5. a proc() function called by
 *               rxr_pkt_proc_req()
 *
 * Some req packet types are so similar that they can share
 * some functions.
 */

/*
 * Utilities shared by all REQ packets
 *
 *     Flags
 */
#define RXR_REQ_OPT_RAW_ADDR_HDR	BIT_ULL(0)
#define RXR_REQ_OPT_CQ_DATA_HDR		BIT_ULL(1)
#define RXR_REQ_MSG			BIT_ULL(2)
#define RXR_REQ_TAGGED			BIT_ULL(3)
#define RXR_REQ_RMA			BIT_ULL(4)

/*
 *     Utility struct and functions for
 *             REQ packet types
 */
struct rxr_req_opt_raw_addr_hdr {
	uint32_t addr_len;
	char raw_addr[0];
};

struct rxr_req_opt_cq_data_hdr {
	int64_t cq_data;
};

extern size_t RXR_REQ_HDR_SIZE_LIST[];

void rxr_pkt_read_req_hdr(struct rxr_pkt_entry *pkt_entry,
			  struct rxr_rx_entry *rx_entry);

size_t rxr_pkt_req_total_len(struct rxr_pkt_entry *pkt_entry);

size_t rxr_pkt_req_max_data_size(struct rxr_ep *ep, fi_addr_t addr, int pkt_type);

/*
 * Structs and funcitons for RTM (Message) packet types
 * There are 4 message protocols
 *         Eager message protocol,
 *         Medium message protocol,
 *         Long message protocol,
 *         Read message protocol (message by read)
 * Each protocol employes two packet types: non-tagged and tagged.
 * Thus altogether there are 8 RTM packet types.
 */

/*
 *   Utility structs and functions shared by all
 *   RTM packet types
 */
struct rxr_rtm_base_hdr {
	uint8_t type;
	uint8_t version;
	uint16_t flags;
	uint32_t msg_id;
};

static inline
struct rxr_rtm_base_hdr *rxr_get_rtm_base_hdr(void *pkt)
{
	return (struct rxr_rtm_base_hdr *)pkt;
}

static inline
uint32_t rxr_pkt_rtm_msg_id(struct rxr_pkt_entry *pkt_entry)
{
	struct rxr_rtm_base_hdr *rtm_hdr;

	rtm_hdr = rxr_get_rtm_base_hdr(pkt_entry->pkt);
	assert(rtm_hdr->flags & RXR_REQ_MSG);
	return rtm_hdr->msg_id;
}

static inline
uint64_t rxr_pkt_rtm_tag(struct rxr_pkt_entry *pkt_entry)
{
	int pkt_type;
	size_t offset;
	uint64_t *tagptr;

	pkt_type = rxr_get_base_hdr(pkt_entry->pkt)->type;
	offset = RXR_REQ_HDR_SIZE_LIST[pkt_type] - sizeof(uint64_t);
	/*
	 * In consideration of performance, this function did not cast header
	 * into different header types to get tag, but assume tag is always
	 * the last member of header.
	 */
	tagptr = (uint64_t *)((char *)pkt_entry->pkt + offset);
	return *tagptr;
}

static inline
void rxr_pkt_rtm_settag(struct rxr_pkt_entry *pkt_entry, uint64_t tag)
{
	int pkt_type;
	size_t offset;
	uint64_t *tagptr;

	pkt_type = rxr_get_base_hdr(pkt_entry->pkt)->type;
	offset = RXR_REQ_HDR_SIZE_LIST[pkt_type] - sizeof(uint64_t);
	/* tag is always the last member */
	tagptr = (uint64_t *)((char *)pkt_entry->pkt + offset);
	*tagptr = tag;
}

/*
 *   Header structs for each REQ packe type
 */
struct rxr_eager_msgrtm_hdr {
	struct rxr_rtm_base_hdr hdr;
};

struct rxr_eager_tagrtm_hdr {
	struct rxr_rtm_base_hdr hdr;
	uint64_t tag;
};

struct rxr_medium_rtm_base_hdr {
	struct rxr_rtm_base_hdr hdr;
	uint64_t data_len;
	uint64_t offset;
};

struct rxr_medium_msgrtm_hdr {
	struct rxr_medium_rtm_base_hdr hdr;
};

struct rxr_medium_tagrtm_hdr {
	struct rxr_medium_rtm_base_hdr hdr;
	uint64_t tag;
};

struct rxr_long_rtm_base_hdr {
	struct rxr_rtm_base_hdr hdr;
	uint64_t data_len;
	uint32_t tx_id;
	uint32_t credit_request;
};

static inline
struct rxr_long_rtm_base_hdr *rxr_get_long_rtm_base_hdr(void *pkt)
{
	return (struct rxr_long_rtm_base_hdr *)pkt;
}

struct rxr_long_msgrtm_hdr {
	struct rxr_long_rtm_base_hdr hdr;
};

struct rxr_long_tagrtm_hdr {
	struct rxr_long_rtm_base_hdr hdr;
	uint64_t tag;
};

/*
 *  init() functions for RTM packets
 */
ssize_t rxr_pkt_init_eager_msgrtm(struct rxr_ep *ep,
				  struct rxr_tx_entry *tx_entry,
				  struct rxr_pkt_entry *pkt_entry);

ssize_t rxr_pkt_init_eager_tagrtm(struct rxr_ep *ep,
				  struct rxr_tx_entry *tx_entry,
				  struct rxr_pkt_entry *pkt_entry);

ssize_t rxr_pkt_init_medium_msgrtm(struct rxr_ep *ep,
				   struct rxr_tx_entry *tx_entry,
				   struct rxr_pkt_entry *pkt_entry);

ssize_t rxr_pkt_init_medium_tagrtm(struct rxr_ep *ep,
				   struct rxr_tx_entry *tx_entry,
				   struct rxr_pkt_entry *pkt_entry);

ssize_t rxr_pkt_init_long_msgrtm(struct rxr_ep *ep,
				 struct rxr_tx_entry *tx_entry,
				 struct rxr_pkt_entry *pkt_entry);

ssize_t rxr_pkt_init_long_tagrtm(struct rxr_ep *ep,
				 struct rxr_tx_entry *tx_entry,
				 struct rxr_pkt_entry *pkt_entry);

/*
 *   handle_sent() functions for RTM packets
 */
static inline
void rxr_pkt_handle_eager_rtm_sent(struct rxr_ep *ep,
				   struct rxr_pkt_entry *pkt_entry)
{
	/* there is nothing to be done for eager RTM */
	return;
}

void rxr_pkt_handle_long_rtm_sent(struct rxr_ep *ep,
				  struct rxr_pkt_entry *pkt_entry);

/*
 *   handle_send_completion() functions for RTM packet types
 */
void rxr_pkt_handle_eager_rtm_send_completion(struct rxr_ep *ep,
					      struct rxr_pkt_entry *pkt_entry);

void rxr_pkt_handle_long_rtm_send_completion(struct rxr_ep *ep,
					     struct rxr_pkt_entry *pkt_entry);

/*
 *   proc() functions for RTM packet types
 */
ssize_t rxr_pkt_proc_matched_rtm(struct rxr_ep *ep,
				 struct rxr_rx_entry *rx_entry,
				 struct rxr_pkt_entry *pkt_entry);

ssize_t rxr_pkt_proc_rtm(struct rxr_ep *ep,
			 struct rxr_pkt_entry *pkt_entry);

void rxr_pkt_handle_rtm_recv(struct rxr_ep *ep,
			     struct rxr_pkt_entry *pkt_entry);

#endif
