// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2015-2020, Oracle and/or its affiliates.
 *
 * Support for reverse-direction RPCs on RPC/RDMA.
 */

#include <linux/sunrpc/xprt.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/svc_xprt.h>
#include <linux/sunrpc/svc_rdma.h>

#include "xprt_rdma.h"
#ifdef HAVE_TRACE_RPCRDMA_H
#include <trace/events/rpcrdma.h>
#endif

#undef RPCRDMA_BACKCHANNEL_DEBUG

#if IS_ENABLED(CONFIG_SUNRPC_DEBUG)
#ifndef RPCDBG_FACILITY
#define RPCDBG_FACILITY    RPCDBG_TRANS
#endif
#endif

#ifdef HAVE_RPC_XPRT_OPS_FREE_SLOT
/**
 * xprt_rdma_bc_setup - Pre-allocate resources for handling backchannel requests
 * @xprt: transport associated with these backchannel resources
 * @reqs: number of concurrent incoming requests to expect
 *
 * Returns 0 on success; otherwise a negative errno
 */
int xprt_rdma_bc_setup(struct rpc_xprt *xprt, unsigned int reqs)
{
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);

	r_xprt->rx_buf.rb_bc_srv_max_requests = RPCRDMA_BACKWARD_WRS >> 1;
#ifdef HAVE_TRACE_RPCRDMA_H
	trace_xprtrdma_cb_setup(r_xprt, reqs);
#endif
	return 0;
}
#else
static void rpcrdma_bc_free_rqst(struct rpcrdma_xprt *r_xprt,
				 struct rpc_rqst *rqst)
{
	struct rpcrdma_buffer *buf = &r_xprt->rx_buf;
	struct rpcrdma_req *req = rpcr_to_rdmar(rqst);

	spin_lock(&buf->rb_lock);
	rpcrdma_req_destroy(req);
	spin_unlock(&buf->rb_lock);

	kfree(rqst);
}

static int rpcrdma_bc_setup_rqst(struct rpcrdma_xprt *r_xprt,
					struct rpc_rqst *rqst);

int xprt_rdma_bc_setup(struct rpc_xprt *xprt, unsigned int reqs)
{
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);
	struct rpcrdma_buffer *buffer = &r_xprt->rx_buf;
	struct rpc_rqst *rqst;
	unsigned int i;

	if (reqs > RPCRDMA_BACKWARD_WRS >> 1)
		goto out_err;

	for (i = 0; i < (reqs << 1); i++) {
		rqst = kzalloc(sizeof(*rqst), GFP_KERNEL);
		if (!rqst)
			goto out_free;

		dprintk("RPC:       %s: new rqst %p\n", __func__, rqst);

		rqst->rq_xprt = &r_xprt->rx_xprt;
		INIT_LIST_HEAD(&rqst->rq_list);
		INIT_LIST_HEAD(&rqst->rq_bc_list);
		__set_bit(RPC_BC_PA_IN_USE, &rqst->rq_bc_pa_state);

		if (rpcrdma_bc_setup_rqst(r_xprt, rqst))
			goto out_free;

		spin_lock_bh(&xprt->bc_pa_lock);
		list_add(&rqst->rq_bc_pa_list, &xprt->bc_pa_list);
		spin_unlock_bh(&xprt->bc_pa_lock);
	}

	buffer->rb_bc_srv_max_requests = reqs;
	request_module("svcrdma");
	return 0;

out_free:
	xprt_rdma_bc_destroy(xprt, reqs);

out_err:
	pr_err("RPC:       %s: setup backchannel transport failed\n", __func__);
	return -ENOMEM;
}
#endif

#if defined(CONFIG_SUNRPC_BACKCHANNEL) && defined(HAVE_RPC_XPRT_OPS_BC_UP)
/**
 * xprt_rdma_bc_up - Create transport endpoint for backchannel service
 * @serv: server endpoint
 * @net: network namespace
 *
 * The "xprt" is an implied argument: it supplies the name of the
 * backchannel transport class.
 *
 * Returns zero on success, negative errno on failure
 */
int xprt_rdma_bc_up(struct svc_serv *serv, struct net *net)
{
    int ret;

    ret = svc_create_xprt(serv, "rdma-bc", net, PF_INET, 0, 0);
    if (ret < 0)
        return ret;
    return 0;
}
#endif

/**
 * xprt_rdma_bc_maxpayload - Return maximum backchannel message size
 * @xprt: transport
 *
 * Returns maximum size, in bytes, of a backchannel message
 */
size_t xprt_rdma_bc_maxpayload(struct rpc_xprt *xprt)
{
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);
	struct rpcrdma_ep *ep = r_xprt->rx_ep;
	size_t maxmsg;

	maxmsg = min_t(unsigned int, ep->re_inline_send, ep->re_inline_recv);
	maxmsg = min_t(unsigned int, maxmsg, PAGE_SIZE);
	return maxmsg - RPCRDMA_HDRLEN_MIN;
}

#ifdef HAVE_RPC_XPRT_OPS_BC_NUM_SLOTS
unsigned int xprt_rdma_bc_max_slots(struct rpc_xprt *xprt)
{
	return RPCRDMA_BACKWARD_WRS >> 1;
}
#endif

static int rpcrdma_bc_marshal_reply(struct rpc_rqst *rqst)
{
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(rqst->rq_xprt);
	struct rpcrdma_req *req = rpcr_to_rdmar(rqst);
	__be32 *p;

	rpcrdma_set_xdrlen(&req->rl_hdrbuf, 0);
#ifdef HAVE_XDR_INIT_ENCODE_RQST_ARG
	xdr_init_encode(&req->rl_stream, &req->rl_hdrbuf,
			rdmab_data(req->rl_rdmabuf), rqst);
#else
	xdr_init_encode(&req->rl_stream, &req->rl_hdrbuf,
			rdmab_data(req->rl_rdmabuf));
#endif

	p = xdr_reserve_space(&req->rl_stream, 28);
	if (unlikely(!p))
		return -EIO;
	*p++ = rqst->rq_xid;
	*p++ = rpcrdma_version;
	*p++ = cpu_to_be32(r_xprt->rx_buf.rb_bc_srv_max_requests);
	*p++ = rdma_msg;
	*p++ = xdr_zero;
	*p++ = xdr_zero;
	*p = xdr_zero;

	if (rpcrdma_prepare_send_sges(r_xprt, req, RPCRDMA_HDRLEN_MIN,
				      &rqst->rq_snd_buf, rpcrdma_noch_pullup))
		return -EIO;

#ifdef HAVE_TRACE_RPCRDMA_H
	trace_xprtrdma_cb_reply(r_xprt, rqst);
#endif
	return 0;
}

/**
 * xprt_rdma_bc_send_reply - marshal and send a backchannel reply
 * @rqst: RPC rqst with a backchannel RPC reply in rq_snd_buf
 *
 * Caller holds the transport's write lock.
 *
 * Returns:
 *	%0 if the RPC message has been sent
 *	%-ENOTCONN if the caller should reconnect and call again
 *	%-EIO if a permanent error occurred and the request was not
 *		sent. Do not try to send this message again.
 */
int xprt_rdma_bc_send_reply(struct rpc_rqst *rqst)
{
	struct rpc_xprt *xprt = rqst->rq_xprt;
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);
	struct rpcrdma_req *req = rpcr_to_rdmar(rqst);
	int rc;

	if (!xprt_connected(xprt))
		return -ENOTCONN;

#ifdef HAVE_XPRT_REQUEST_GET_CONG
	if (!xprt_request_get_cong(xprt, rqst))
		return -EBADSLT;
#endif

	rc = rpcrdma_bc_marshal_reply(rqst);
	if (rc < 0)
		goto failed_marshal;

	if (rpcrdma_post_sends(r_xprt, req))
		goto drop_connection;
	return 0;

failed_marshal:
	if (rc != -ENOTCONN)
		return rc;
drop_connection:
	xprt_rdma_close(xprt);
	return -ENOTCONN;
}

/**
 * xprt_rdma_bc_destroy - Release resources for handling backchannel requests
 * @xprt: transport associated with these backchannel resources
 * @reqs: number of incoming requests to destroy; ignored
 */
void xprt_rdma_bc_destroy(struct rpc_xprt *xprt, unsigned int reqs)
{
	struct rpc_rqst *rqst, *tmp;

	spin_lock(&xprt->bc_pa_lock);
	list_for_each_entry_safe(rqst, tmp, &xprt->bc_pa_list, rq_bc_pa_list) {
		list_del(&rqst->rq_bc_pa_list);
		spin_unlock(&xprt->bc_pa_lock);

#ifdef HAVE_RPC_XPRT_OPS_FREE_SLOT
		rpcrdma_req_destroy(rpcr_to_rdmar(rqst));
#else
		rpcrdma_bc_free_rqst(rpcx_to_rdmax(xprt), rqst);
#endif

		spin_lock(&xprt->bc_pa_lock);
	}
	spin_unlock(&xprt->bc_pa_lock);
}

/**
 * xprt_rdma_bc_free_rqst - Release a backchannel rqst
 * @rqst: request to release
 */
void xprt_rdma_bc_free_rqst(struct rpc_rqst *rqst)
{
	struct rpcrdma_req *req = rpcr_to_rdmar(rqst);
	struct rpcrdma_rep *rep = req->rl_reply;
	struct rpc_xprt *xprt = rqst->rq_xprt;
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);

	rpcrdma_rep_put(&r_xprt->rx_buf, rep);
	req->rl_reply = NULL;

	spin_lock(&xprt->bc_pa_lock);
	list_add_tail(&rqst->rq_bc_pa_list, &xprt->bc_pa_list);
	spin_unlock(&xprt->bc_pa_lock);
	xprt_put(xprt);
}

#ifdef HAVE_RPC_XPRT_OPS_FREE_SLOT
static struct rpc_rqst *rpcrdma_bc_rqst_get(struct rpcrdma_xprt *r_xprt)
{
	struct rpc_xprt *xprt = &r_xprt->rx_xprt;
	struct rpcrdma_req *req;
	struct rpc_rqst *rqst;
	size_t size;

	spin_lock(&xprt->bc_pa_lock);
	rqst = list_first_entry_or_null(&xprt->bc_pa_list, struct rpc_rqst,
					rq_bc_pa_list);
	if (!rqst)
		goto create_req;
	list_del(&rqst->rq_bc_pa_list);
	spin_unlock(&xprt->bc_pa_lock);
	return rqst;

create_req:
	spin_unlock(&xprt->bc_pa_lock);

	/* Set a limit to prevent a remote from overrunning our resources.
	 */
	if (xprt->bc_alloc_count >= RPCRDMA_BACKWARD_WRS)
		return NULL;

	size = min_t(size_t, r_xprt->rx_ep->re_inline_recv, PAGE_SIZE);
	req = rpcrdma_req_create(r_xprt, size, GFP_KERNEL);
	if (!req)
		return NULL;
	if (rpcrdma_req_setup(r_xprt, req)) {
		rpcrdma_req_destroy(req);
		return NULL;
	}

	xprt->bc_alloc_count++;
	rqst = &req->rl_slot;
	rqst->rq_xprt = xprt;
	__set_bit(RPC_BC_PA_IN_USE, &rqst->rq_bc_pa_state);
	xdr_buf_init(&rqst->rq_snd_buf, rdmab_data(req->rl_sendbuf), size);
	return rqst;
}
#else
static int rpcrdma_bc_setup_rqst(struct rpcrdma_xprt *r_xprt,
               struct rpc_rqst *rqst)
{
	struct rpcrdma_req *req;
	size_t size;

	size = min_t(size_t, r_xprt->rx_ep->re_inline_recv, PAGE_SIZE);
	req = rpcrdma_req_create(r_xprt, size, GFP_KERNEL);
	if (!req)
		return PTR_ERR(req);

	xdr_buf_init(&rqst->rq_snd_buf, rdmab_data(req->rl_sendbuf),
		     size);
	rpcrdma_set_xprtdata(rqst, req);
	return 0;
}
#endif

/**
 * rpcrdma_bc_receive_call - Handle a reverse-direction Call
 * @r_xprt: transport receiving the call
 * @rep: receive buffer containing the call
 *
 * Operational assumptions:
 *    o Backchannel credits are ignored, just as the NFS server
 *      forechannel currently does
 *    o The ULP manages a replay cache (eg, NFSv4.1 sessions).
 *      No replay detection is done at the transport level
 */
void rpcrdma_bc_receive_call(struct rpcrdma_xprt *r_xprt,
			     struct rpcrdma_rep *rep)
{
	struct rpc_xprt *xprt = &r_xprt->rx_xprt;
	struct svc_serv *bc_serv;
	struct rpcrdma_req *req;
	struct rpc_rqst *rqst;
	struct xdr_buf *buf;
	size_t size;
	__be32 *p;

	p = xdr_inline_decode(&rep->rr_stream, 0);
	size = xdr_stream_remaining(&rep->rr_stream);

#ifdef RPCRDMA_BACKCHANNEL_DEBUG
	pr_info("RPC:       %s: callback XID %08x, length=%u\n",
		__func__, be32_to_cpup(p), size);
	pr_info("RPC:       %s: %*ph\n", __func__, size, p);
#endif

#ifdef HAVE_RPC_XPRT_OPS_FREE_SLOT
	rqst = rpcrdma_bc_rqst_get(r_xprt);
	if (!rqst)
		goto out_overflow;
#else
	/* Grab a free bc rqst */
	spin_lock(&xprt->bc_pa_lock);
	if (list_empty(&xprt->bc_pa_list)) {
		spin_unlock(&xprt->bc_pa_lock);
		goto out_overflow;
	}
	rqst = list_first_entry(&xprt->bc_pa_list,
				struct rpc_rqst, rq_bc_pa_list);
	list_del(&rqst->rq_bc_pa_list);
	spin_unlock(&xprt->bc_pa_lock);
#endif

	rqst->rq_reply_bytes_recvd = 0;
	rqst->rq_xid = *p;

	rqst->rq_private_buf.len = size;

	buf = &rqst->rq_rcv_buf;
	memset(buf, 0, sizeof(*buf));
	buf->head[0].iov_base = p;
	buf->head[0].iov_len = size;
	buf->len = size;

	/* The receive buffer has to be hooked to the rpcrdma_req
	 * so that it is not released while the req is pointing
	 * to its buffer, and so that it can be reposted after
	 * the Upper Layer is done decoding it.
	 */
	req = rpcr_to_rdmar(rqst);
	req->rl_reply = rep;
#ifdef HAVE_TRACE_RPCRDMA_H
	trace_xprtrdma_cb_call(r_xprt, rqst);
#endif

	/* Queue rqst for ULP's callback service */
	bc_serv = xprt->bc_serv;
	xprt_get(xprt);
	spin_lock(&bc_serv->sv_cb_lock);
	list_add(&rqst->rq_bc_list, &bc_serv->sv_cb_list);
	spin_unlock(&bc_serv->sv_cb_lock);

	wake_up(&bc_serv->sv_cb_waitq);

	r_xprt->rx_stats.bcall_count++;
	return;

out_overflow:
	pr_warn("RPC/RDMA backchannel overflow\n");
	xprt_force_disconnect(xprt);
	/* This receive buffer gets reposted automatically
	 * when the connection is re-established.
	 */
	return;
}
