/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * Copyright (c) 2005-2006 Network Appliance, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the BSD-type
 * license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *      Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *      Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials provided
 *      with the distribution.
 *
 *      Neither the name of the Network Appliance, Inc. nor the names of
 *      its contributors may be used to endorse or promote products
 *      derived from this software without specific prior written
 *      permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Tom Tucker <tom@opengridcomputing.com>
 */

#ifndef SVC_RDMA_H
#define SVC_RDMA_H

#include "../../../compat/config.h"

#ifdef HAVE_SVC_FILL_WRITE_VECTOR
#include <linux/llist.h>
#endif
#include <linux/sunrpc/xdr.h>
#include <linux/sunrpc/svcsock.h>
#include <linux/sunrpc/rpc_rdma.h>
#include <linux/sunrpc/rpc_rdma_cid.h>
#ifdef HAVE_SVC_RDMA_PCL
#include <linux/sunrpc/svc_rdma_pcl.h>
#endif

#include <linux/percpu_counter.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

/* Default and maximum inline threshold sizes */
enum {
	RPCRDMA_PULLUP_THRESH = RPCRDMA_V1_DEF_INLINE_SIZE >> 1,
	RPCRDMA_DEF_INLINE_THRESH = 4096,
	RPCRDMA_MAX_INLINE_THRESH = 65536
};

/* RPC/RDMA parameters and stats */
extern unsigned int svcrdma_ord;
extern unsigned int svcrdma_max_requests;
extern unsigned int svcrdma_max_bc_requests;
extern unsigned int svcrdma_max_req_size;

extern struct percpu_counter svcrdma_stat_read;
extern struct percpu_counter svcrdma_stat_recv;
extern struct percpu_counter svcrdma_stat_sq_starve;
extern struct percpu_counter svcrdma_stat_write;

struct svcxprt_rdma {
	struct svc_xprt      sc_xprt;		/* SVC transport structure */
	struct rdma_cm_id    *sc_cm_id;		/* RDMA connection id */
	struct list_head     sc_accept_q;	/* Conn. waiting accept */
	int		     sc_ord;		/* RDMA read limit */
	int                  sc_max_send_sges;
	bool		     sc_snd_w_inv;	/* OK to use Send With Invalidate */

	atomic_t             sc_sq_avail;	/* SQEs ready to be consumed */
	unsigned int	     sc_sq_depth;	/* Depth of SQ */
	__be32		     sc_fc_credits;	/* Forward credits */
	u32		     sc_max_requests;	/* Max requests */
	u32		     sc_max_bc_requests;/* Backward credits */
	int                  sc_max_req_size;	/* Size of each RQ WR buf */
	u8		     sc_port_num;

	struct ib_pd         *sc_pd;

	spinlock_t	     sc_send_lock;
	struct list_head     sc_send_ctxts;
	spinlock_t	     sc_rw_ctxt_lock;
	struct list_head     sc_rw_ctxts;

#ifdef HAVE_SVCXPRT_RDMA_SC_PENDING_RECVS
	u32		     sc_pending_recvs;
	u32		     sc_recv_batch;
#endif
	struct list_head     sc_rq_dto_q;
	spinlock_t	     sc_rq_dto_lock;
	struct ib_qp         *sc_qp;
	struct ib_cq         *sc_rq_cq;
	struct ib_cq         *sc_sq_cq;

	spinlock_t	     sc_lock;		/* transport lock */

	wait_queue_head_t    sc_send_wait;	/* SQ exhaustion waitlist */
	unsigned long	     sc_flags;
#ifndef HAVE_SVC_RDMA_PCL
	struct list_head     sc_read_complete_q;
#endif
	struct work_struct   sc_work;

#ifdef HAVE_SVC_FILL_WRITE_VECTOR
	struct llist_head    sc_recv_ctxts;
#else
	spinlock_t	     sc_recv_lock;
	struct list_head     sc_recv_ctxts;
#endif

	atomic_t	     sc_completion_ids;
};
/* sc_flags */
#define RDMAXPRT_CONN_PENDING	3

/*
 * Default connection parameters
 */
enum {
	RPCRDMA_LISTEN_BACKLOG	= 10,
	RPCRDMA_MAX_REQUESTS	= 64,
	RPCRDMA_MAX_BC_REQUESTS	= 2,
};

#define RPCSVC_MAXPAYLOAD_RDMA	RPCSVC_MAXPAYLOAD

struct svc_rdma_recv_ctxt {
#ifdef HAVE_SVC_FILL_WRITE_VECTOR
	struct llist_node	rc_node;
#endif
	struct list_head	rc_list;
	struct ib_recv_wr	rc_recv_wr;
	struct ib_cqe		rc_cqe;
	struct rpc_rdma_cid	rc_cid;
#ifdef HAVE_SVC_FILL_WRITE_VECTOR
	struct ib_sge		rc_recv_sge;
	void			*rc_recv_buf;
#endif
#ifndef HAVE_SVC_RDMA_PCL
 	struct xdr_buf		rc_arg;
#endif
	struct xdr_stream	rc_stream;
#ifdef HAVE_SVC_FILL_WRITE_VECTOR
	bool			rc_temp;
#endif
	u32			rc_byte_len;
	unsigned int		rc_page_count;
#ifndef HAVE_SVC_RDMA_PCL
	unsigned int		rc_hdr_count;
#endif
#ifndef HAVE_SVC_FILL_WRITE_VECTOR
	struct ib_sge		rc_sges[1 +
					RPCRDMA_MAX_INLINE_THRESH / PAGE_SIZE];
#endif
#ifndef HAVE_SVC_RDMA_PCL
	struct page     *rc_pages[RPCSVC_MAXPAGES];
#endif
	u32			rc_inv_rkey;
#ifdef HAVE_SVC_RDMA_PCL
	__be32			rc_msgtype;
	struct svc_rdma_pcl	rc_call_pcl;
#endif

#ifndef HAVE_SVC_RDMA_PCL
	__be32			*rc_write_list;
	__be32			*rc_reply_chunk;
	unsigned int		rc_read_payload_offset;
	unsigned int		rc_read_payload_length;
#endif

#ifdef HAVE_SVC_RDMA_PCL
	struct svc_rdma_pcl	rc_read_pcl;
	struct svc_rdma_chunk	*rc_cur_result_payload;
	struct svc_rdma_pcl	rc_write_pcl;
	struct svc_rdma_pcl	rc_reply_pcl;
#endif
};

struct svc_rdma_send_ctxt {
	struct list_head	sc_list;
	struct rpc_rdma_cid	sc_cid;

	struct ib_send_wr	sc_send_wr;
	struct ib_cqe		sc_cqe;
	struct completion	sc_done;
	struct xdr_buf		sc_hdrbuf;
	struct xdr_stream	sc_stream;
	void			*sc_xprt_buf;
	int			sc_cur_sge_no;

	struct ib_sge		sc_sges[];
};

/* svc_rdma_backchannel.c */
extern void svc_rdma_handle_bc_reply(struct svc_rqst *rqstp,
				     struct svc_rdma_recv_ctxt *rctxt);

/* svc_rdma_recvfrom.c */
extern void svc_rdma_recv_ctxts_destroy(struct svcxprt_rdma *rdma);
extern bool svc_rdma_post_recvs(struct svcxprt_rdma *rdma);
#ifdef HAVE_SVC_RDMA_PCL
extern struct svc_rdma_recv_ctxt *
		svc_rdma_recv_ctxt_get(struct svcxprt_rdma *rdma);
#endif
extern void svc_rdma_recv_ctxt_put(struct svcxprt_rdma *rdma,
				   struct svc_rdma_recv_ctxt *ctxt);
extern void svc_rdma_flush_recv_queues(struct svcxprt_rdma *rdma);
#ifdef HAVE_SVC_RDMA_RELEASE_RQST
extern void svc_rdma_release_rqst(struct svc_rqst *rqstp);
#endif
extern int svc_rdma_recvfrom(struct svc_rqst *);

/* svc_rdma_rw.c */
extern void svc_rdma_destroy_rw_ctxts(struct svcxprt_rdma *rdma);
#ifndef HAVE_SVC_RDMA_PCL
extern int svc_rdma_recv_read_chunk(struct svcxprt_rdma *rdma,
				    struct svc_rqst *rqstp,
				    struct svc_rdma_recv_ctxt *head, __be32 *p);
#endif
extern int svc_rdma_send_write_chunk(struct svcxprt_rdma *rdma,
#ifdef HAVE_SVC_RDMA_PCL
				     const struct svc_rdma_chunk *chunk,
				     const struct xdr_buf *xdr);
#else
				     __be32 *wr_ch, struct xdr_buf *xdr,
				     unsigned int offset,
				     unsigned long length);
#endif
extern int svc_rdma_send_reply_chunk(struct svcxprt_rdma *rdma,
				     const struct svc_rdma_recv_ctxt *rctxt,
#ifdef HAVE_SVC_RDMA_PCL
				     const struct xdr_buf *xdr);
#else
				     struct xdr_buf *xdr);
#endif
#ifdef HAVE_SVC_RDMA_PCL
extern int svc_rdma_process_read_list(struct svcxprt_rdma *rdma,
				      struct svc_rqst *rqstp,
				      struct svc_rdma_recv_ctxt *head);
#endif

/* svc_rdma_sendto.c */
extern void svc_rdma_send_ctxts_destroy(struct svcxprt_rdma *rdma);
extern struct svc_rdma_send_ctxt *
		svc_rdma_send_ctxt_get(struct svcxprt_rdma *rdma);
extern void svc_rdma_send_ctxt_put(struct svcxprt_rdma *rdma,
				   struct svc_rdma_send_ctxt *ctxt);
extern int svc_rdma_send(struct svcxprt_rdma *rdma,
			 struct svc_rdma_send_ctxt *ctxt);
extern int svc_rdma_map_reply_msg(struct svcxprt_rdma *rdma,
				  struct svc_rdma_send_ctxt *sctxt,
				  const struct svc_rdma_recv_ctxt *rctxt,
#ifdef HAVE_SVC_RDMA_PCL
				  const struct xdr_buf *xdr);
#else
				  struct xdr_buf *xdr);
#endif
extern void svc_rdma_send_error_msg(struct svcxprt_rdma *rdma,
				    struct svc_rdma_send_ctxt *sctxt,
				    struct svc_rdma_recv_ctxt *rctxt,
				    int status);
extern int svc_rdma_sendto(struct svc_rqst *);
#ifdef HAVE_XPO_READ_PAYLOAD
extern int svc_rdma_read_payload(struct svc_rqst *rqstp, unsigned int offset,
				 unsigned int length);
#endif

#ifdef HAVE_XPO_RESULT_PAYLOAD
extern int svc_rdma_result_payload(struct svc_rqst *rqstp, unsigned int offset,
				   unsigned int length);
#endif

/* svc_rdma_transport.c */
extern struct svc_xprt_class svc_rdma_class;
#ifdef CONFIG_SUNRPC_BACKCHANNEL
extern struct svc_xprt_class svc_rdma_bc_class;
#endif

/* svc_rdma.c */
extern int svc_rdma_init(void);
extern void svc_rdma_cleanup(void);

#endif
