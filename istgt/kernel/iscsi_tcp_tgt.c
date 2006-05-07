/*
 * iSCSI Target over TCP/IP
 *
 * Copyright (C) 2004 - 2006 FUJITA Tomonori <tomof@acm.org>
 * Copyright (C) 2005 - 2006 Mike Christie
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * See the file COPYING included with this distribution for more details.
 */

/*
 * This needs to be integrated with iscsi_tcp.
 */
#include <linux/list.h>
#include <linux/inet.h>
#include <linux/kfifo.h>
#include <net/tcp.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi.h>
#include <scsi/scsi_tgt.h>
#include <scsi/scsi_tcq.h>
#include "iscsi_tcp.h"
#include "libiscsi.h"
#include "scsi_transport_iscsi.h"
#include "iscsi_tcp_priv.h"

/* tmp - will replace with SCSI logging stuff */
#define eprintk(fmt, args...)					\
do {								\
	printk("%s(%d) " fmt, __FUNCTION__, __LINE__, ##args);	\
} while (0)

#define dprintk eprintk

struct istgt_session {
	struct list_head recvlist;
	/* replace with array later on */
	struct list_head cmd_hash;
	spinlock_t slock;
	struct work_struct recvwork;
};

#if 0
static void build_r2t(struct iscsi_cmd_task *ctask)
{
	struct iscsi_r2t_rsp *hdr;
	struct iscsi_data_task *dtask;
	struct iscsi_r2t_info *r2t;
/* 	struct iscsi_session *session = ctask->conn->session; */
	struct iscsi_tcp_cmd_task *tcp_ctask = ctask->dd_data;
/* 	struct iscsi_tcp_conn *tcp_conn = ctask->conn->dd_data; */
	int rc;

/* 	length = req->r2t_length; */
/* 	burst = req->conn->session->param.max_burst_length; */
/* 	offset = be32_to_cpu(cmd_hdr(req)->data_length) - length; */
/* more: */
	rc = __kfifo_get(tcp_ctask->r2tpool.queue, (void*)&r2t, sizeof(void*));
	BUG_ON(!rc);

	dtask = mempool_alloc(tcp_ctask->datapool, GFP_ATOMIC);
	BUG_ON(!dtask);

	INIT_LIST_HEAD(&dtask->item);
	r2t->dtask = dtask;
	hdr = (struct iscsi_r2t_rsp *) &dtask->hdr;

/* 	rsp->pdu.bhs.ttt = req->target_task_tag; */

	hdr->opcode = ISCSI_OP_R2T;
	hdr->flags = ISCSI_FLAG_CMD_FINAL;
	memcpy(hdr->lun, ctask->hdr->lun, 8);
	hdr->itt = ctask->hdr->itt;
	hdr->r2tsn = cpu_to_be32(tcp_ctask->exp_r2tsn++);
/* 	hdr->data_offset = cpu_to_be32(offset); */
/* 	if (length > burst) { */
/* 		rsp_hdr->data_length = cpu_to_be32(burst); */
/* 		length -= burst; */
/* 		offset += burst; */
/* 	} else { */
/* 		rsp_hdr->data_length = cpu_to_be32(length); */
/* 		length = 0; */
/* 	} */

	dprintk("%x %u %u %u\n", ctask->hdr->itt,
		be32_to_cpu(hdr->data_length),
		be32_to_cpu(hdr->data_offset),
		be32_to_cpu(hdr->r2tsn));

/* 	if (++req->outstanding_r2t >= req->conn->session->param.max_outstanding_r2t) */
/* 		break; */

	__kfifo_put(tcp_ctask->r2tpool.queue, (void*)&r2t, sizeof(void*));

/* 	if (length) */
/* 		goto more; */
}
#endif

static void istgt_scsi_tgt_queue_command(struct iscsi_cmd_task *ctask)
{
	struct iscsi_session *session = ctask->conn->session;
	struct iscsi_cls_session *cls_session = session_to_cls(session);
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);
	struct iscsi_cmd *hdr = ctask->hdr;
	struct scsi_cmnd *scmd;
	enum dma_data_direction dir = (hdr->flags & ISCSI_FLAG_CMD_WRITE) ?
		DMA_TO_DEVICE : DMA_FROM_DEVICE;

	scmd = scsi_host_get_command(shost, dir, GFP_KERNEL);
	BUG_ON(!scmd);
	ctask->sc = scmd;
	memcpy(scmd->data_cmnd, hdr->cdb, MAX_COMMAND_SIZE);
	scmd->request_bufflen = be32_to_cpu(hdr->data_length);
	scmd->SCp.ptr = (void *) ctask;

	switch (hdr->flags & ISCSI_FLAG_CMD_ATTR_MASK) {
	case ISCSI_ATTR_UNTAGGED:
	case ISCSI_ATTR_SIMPLE:
		scmd->tag = MSG_SIMPLE_TAG;
		break;
	case ISCSI_ATTR_HEAD_OF_QUEUE:
		scmd->tag = MSG_HEAD_TAG;
		break;
	case ISCSI_ATTR_ORDERED:
	default:
		scmd->tag = MSG_ORDERED_TAG;
	}

	scsi_tgt_queue_command(scmd, (struct scsi_lun *) hdr->lun, hdr->itt);
}

static void istgt_scsi_cmd_exec(struct iscsi_cmd_task *ctask)
{
	struct scsi_cmnd *scmd = ctask->sc;

	if (ctask->data_count) {
		if (!ctask->unsol_count)
			;
/* 			send_r2t(ctask); */
	} else {
/* 		set_cmd_waitio(cmnd); */
		if (scmd) {
/* 			BUG_ON(!ctask->done); */
/* 			cmnd->done(scmd); */
		} else
			istgt_scsi_tgt_queue_command(ctask);
	}
}

static void istgt_cmd_exec(struct iscsi_cmd_task *ctask)
{
	struct iscsi_conn *conn = ctask->conn;
	struct iscsi_cls_session *cls_session =
		session_to_cls(ctask->conn->session);
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);
	u8 opcode;

	opcode = ctask->hdr->opcode & ISCSI_OPCODE_MASK;

	dprintk("%p,%x,%u\n", ctask, opcode, ctask->hdr->cmdsn);

	switch (opcode) {
	case ISCSI_OP_SCSI_CMD:
		istgt_scsi_cmd_exec(ctask);
		break;
	case ISCSI_OP_LOGOUT:
		__kfifo_put(conn->xmitqueue, (void*)&ctask, sizeof(void*));
		scsi_queue_work(shost, &conn->xmitwork);
		break;
	case ISCSI_OP_NOOP_OUT:
	case ISCSI_OP_SCSI_TMFUNC:
	case ISCSI_OP_TEXT:
	case ISCSI_OP_SNACK:
		BUG_ON(1);
		break;
	default:
		eprintk("unexpected cmnd op %x\n", ctask->hdr->opcode);
		break;
	}
}

static void istgt_recvworker(void *data)
{
	struct iscsi_cls_session *cls_session = data;
	struct iscsi_session *session = class_to_transport_session(cls_session);
	struct istgt_session *istgt_session = cls_session->dd_data;
	struct iscsi_cmd_task *ctask;

	dprintk("%x\n", session->exp_cmdsn);
retry:
	spin_lock_bh(&istgt_session->slock);

	while (!list_empty(&istgt_session->recvlist)) {
		ctask = list_entry(istgt_session->recvlist.next,
				   struct iscsi_cmd_task, tgtlist);

		dprintk("%p %x %x\n", ctask, ctask->hdr->cmdsn, session->exp_cmdsn);

		if (be32_to_cpu(ctask->hdr->cmdsn) != session->exp_cmdsn)
			break;

		list_del(&ctask->tgtlist);
		session->exp_cmdsn++;

		spin_unlock_bh(&istgt_session->slock);
		istgt_cmd_exec(ctask);
		goto retry;
	}

	spin_unlock_bh(&istgt_session->slock);
}

static void istgt_ctask_add(struct iscsi_cmd_task *ctask)
{
	struct iscsi_session *session = ctask->conn->session;
	struct iscsi_cls_session *cls_session = session_to_cls(session);
	struct istgt_session *istgt_session = cls_session->dd_data;
	struct iscsi_cmd_task *pos;

	dprintk("%p %x %x %x\n", ctask, ctask->hdr->opcode & ISCSI_OPCODE_MASK,
		ctask->hdr->cdb[0], ctask->hdr->cmdsn);

	spin_lock_bh(&istgt_session->slock);

	if (ctask->hdr->opcode & ISCSI_OP_IMMEDIATE) {
		list_add(&ctask->tgtlist, &istgt_session->recvlist);
		goto out;
	}

	list_for_each_entry(pos, &istgt_session->recvlist, tgtlist)
		if (before(ctask->hdr->cmdsn, pos->hdr->cmdsn))
			break;

	list_add_tail(&ctask->tgtlist, &pos->tgtlist);
out:
	spin_unlock_bh(&istgt_session->slock);
}

#if 0
static void istgt_unsolicited_data(struct iscsi_cmd_task *ctask)
{
/* 	struct iscsi_tcp_cmd_task *tcp_ctask = ctask->dd_data; */

	istgt_scsi_tgt_queue_command(ctask);
/* 	tcp_ctask->r2t_data_count; */
/* 	ctask->r2t_data_count; */
}
#endif

static int istgt_tcp_hdr_recv(struct iscsi_conn *conn)
{
	int rc, opcode;
	struct iscsi_hdr *hdr;
	struct iscsi_session *session = conn->session;
	struct iscsi_cls_session *cls_session = session_to_cls(session);
	struct iscsi_tcp_conn *tcp_conn = conn->dd_data;
	struct iscsi_cmd_task *ctask = NULL;
	struct iscsi_tcp_cmd_task *tcp_ctask;
	struct istgt_session *istgt_session = cls_session->dd_data;

	rc = iscsi_tcp_hdr_recv_pre(conn);
	if (rc)
		return rc;

	hdr = tcp_conn->in.hdr;
	opcode = hdr->opcode & ISCSI_OPCODE_MASK;
	dprintk("opcode 0x%x offset %d copy %d ahslen %d datalen %d\n",
		opcode, tcp_conn->in.offset, tcp_conn->in.copy,
		hdr->hlength << 2, tcp_conn->in.datalen);

	/* FIXME */
	if (tcp_conn->in.datalen) {
		iscsi_conn_failure(conn, rc);
		eprintk("Cannot handle this now\n");
		return 1;
	}

	switch (opcode) {
	case ISCSI_OP_NOOP_OUT:
	case ISCSI_OP_SCSI_CMD:
	case ISCSI_OP_SCSI_TMFUNC:
	case ISCSI_OP_LOGOUT:
		spin_lock(&session->lock);
		__kfifo_get(session->cmdpool.queue, (void*)&ctask, sizeof(void*));
		spin_unlock(&session->lock);
		BUG_ON(!ctask);

		ctask->conn = conn;
		ctask->data_count = 0;
		ctask->sc = NULL;
		ctask->datasn = 0;
		BUG_ON(!ctask->hdr);
		memcpy(ctask->hdr, hdr, sizeof(*hdr));

		tcp_ctask = ctask->dd_data;
		BUG_ON(!tcp_ctask);

		tcp_ctask->sg = NULL;
		tcp_ctask->sent = 0;
		tcp_ctask->xmstate = XMSTATE_UNS_INIT;

		if (!tcp_conn->in.datalen) {
			istgt_ctask_add(ctask);
			ctask = NULL;
			schedule_work(&istgt_session->recvwork);
		}

/* 		if (opcode == ISCSI_OP_SCSI_CMD) */
/* 			switch (ctask->hdr->cdb[0]) { */
/* 			case WRITE_6: */
/* 			case WRITE_10: */
/* 			case WRITE_16: */
/* 			case WRITE_VERIFY: */
/* 				istgt_unsolicited_data(ctask); */
/* 				set_bit(ISCSI_SUSPEND_BIT, &conn->suspend_rx); */
/* 			} */

		break;
	case ISCSI_OP_SCSI_DATA_OUT:
		BUG_ON(1);
		/* Find a command in the hash list */
		/* data_out_start(conn, cmnd); */
		break;
	case ISCSI_OP_TEXT:
	case ISCSI_OP_SNACK:
	default:
		rc = ISCSI_ERR_BAD_OPCODE;
	}

	if (ctask)
		tcp_conn->in.ctask = ctask;

	return rc;
}

static int
istgt_data_recv(struct iscsi_conn *conn)
{
	struct iscsi_tcp_conn *tcp_conn = conn->dd_data;
	int rc = 0, opcode;

	/* We need to return -EAGAIN if the buffer is not ready. */

	opcode = tcp_conn->in.hdr->opcode & ISCSI_OPCODE_MASK;

	dprintk("opcode 0x%x offset %d copy %d datalen %d\n",
		opcode, tcp_conn->in.offset, tcp_conn->in.copy,
		tcp_conn->in.datalen);
	return 1;

	switch (opcode) {
	case ISCSI_OP_SCSI_CMD:
	case ISCSI_OP_SCSI_DATA_OUT:
		iscsi_scsi_data_in(conn);
		break;
	case ISCSI_OP_TEXT:
	case ISCSI_OP_LOGOUT:
	case ISCSI_OP_NOOP_OUT:
	case ISCSI_OP_ASYNC_EVENT:
		/*
		 * Collect data segment to the connection's data
		 * placeholder
		 */
/* 		if (iscsi_tcp_copy(tcp_conn)) { */
/* 			rc = -EAGAIN; */
/* 			goto exit; */
/* 		} */

/* 		rc = iscsi_complete_pdu(conn, tcp_conn->in.hdr, tcp_conn->data, */
/* 					tcp_conn->in.datalen); */
/* 		if (!rc && conn->datadgst_en && opcode != ISCSI_OP_LOGIN_RSP) */
/* 			iscsi_recv_digest_update(tcp_conn, tcp_conn->data, */
/* 			  			tcp_conn->in.datalen); */
		break;
	default:
		BUG_ON(1);
	}

	return rc;
}

static void
istgt_tcp_data_ready(struct sock *sk, int flag)
{
	struct iscsi_conn *conn = sk->sk_user_data;

	schedule_work(&conn->tcpwork);
}

static void __istgt_tcp_data_ready(void *data)
{
	struct iscsi_cls_conn *cls_conn = data;
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct iscsi_tcp_conn *tcp_conn = conn->dd_data;
	struct sock *sk = tcp_conn->sock->sk;
	read_descriptor_t rd_desc;
	struct data_ready_desc d;

	d.conn = conn;
	d.hdr_recv = istgt_tcp_hdr_recv;
	d.data_recv = istgt_data_recv;

	read_lock(&sk->sk_callback_lock);

	/* use rd_desc to pass 'conn' to iscsi_tcp_data_recv */
	rd_desc.arg.data = &d;
	rd_desc.count = 1;
	tcp_read_sock(sk, &rd_desc, iscsi_tcp_data_recv);

	read_unlock(&sk->sk_callback_lock);
}

static int
istgt_tcp_conn_bind(struct iscsi_cls_session *cls_session,
		    struct iscsi_cls_conn *cls_conn, uint64_t transport_eph,
		    int is_leading)
{
	struct socket *sock;
	struct iscsi_conn *conn = cls_conn->dd_data;
	struct iscsi_tcp_conn *tcp_conn = conn->dd_data;
	int err;
	struct iscsi_session *session = class_to_transport_session(cls_session);

	dprintk("%llu %u\n", (unsigned long long) transport_eph, is_leading);

	err = iscsi_tcp_conn_bind(cls_session, cls_conn, transport_eph, is_leading);
	if (err) {
		eprintk("fail to bind %d\n", err);
		return err;
	}

	sock = tcp_conn->sock;

	write_lock_bh(&sock->sk->sk_callback_lock);
	sock->sk->sk_data_ready = istgt_tcp_data_ready;
	write_unlock_bh(&sock->sk->sk_callback_lock);

	INIT_WORK(&conn->tcpwork, __istgt_tcp_data_ready, cls_conn);

	dprintk("%u %u %u %u %u %u %u %u %u %u %u %u\n",
		conn->max_recv_dlength, conn->max_xmit_dlength,
		conn->hdrdgst_en, conn->datadgst_en, session->initial_r2t_en,
		session->max_r2t, session->imm_data_en,
		session->first_burst, session->max_burst,
		session->pdu_inorder_en, session->dataseq_inorder_en, session->erl);

	return 0;
}

static struct iscsi_cls_session *
istgt_tcp_session_create(struct iscsi_transport *iscsit,
			 struct scsi_transport_template *scsit,
			 uint32_t initial_cmdsn, uint32_t *hostno)
{
	struct Scsi_Host *shost;
	struct iscsi_cls_session *cls_session;
	struct iscsi_session *session;
	struct istgt_session *istgt_session;
	int i, err;

	dprintk("%u %u\n", initial_cmdsn, *hostno);

	cls_session = iscsi_tcp_session_create(iscsit, scsit, initial_cmdsn,
					       hostno);
	if (!cls_session)
		return NULL;
	shost = iscsi_session_to_shost(cls_session);
	err = scsi_tgt_alloc_queue(shost);
	if (err)
		goto session_free;

	session = class_to_transport_session(cls_session);
	for (i = 0; i < initial_cmdsn; i++) {
		struct iscsi_cmd_task *ctask = session->cmds[i];
		INIT_LIST_HEAD(&ctask->hash);
		INIT_LIST_HEAD(&ctask->tgtlist);
	}
	session->exp_cmdsn = initial_cmdsn;

	istgt_session =	(struct istgt_session *) cls_session->dd_data;

	INIT_LIST_HEAD(&istgt_session->recvlist);
	INIT_LIST_HEAD(&istgt_session->cmd_hash);
	spin_lock_init(&istgt_session->slock);

	INIT_WORK(&istgt_session->recvwork, istgt_recvworker, cls_session);

	return cls_session;
session_free:
	iscsi_session_teardown(cls_session);
	return NULL;
}

static int istgt_transfer_response(struct scsi_cmnd *scmd,
				   void (*done)(struct scsi_cmnd *))
{
	struct iscsi_cmd_task *ctask = (struct iscsi_cmd_task *) scmd->SCp.ptr;
	struct iscsi_conn *conn = ctask->conn;
	struct iscsi_cls_session *cls_session = session_to_cls(conn->session);
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);

	dprintk("%p %x %x %u %u\n", ctask, ctask->hdr->opcode & ISCSI_OPCODE_MASK,
		ctask->hdr->cdb[0], scmd->request_bufflen, scmd->sc_data_direction);

	if (scmd->sc_data_direction == DMA_TO_DEVICE) {
		scmd->done = done;
		__kfifo_put(conn->xmitqueue, (void*)&ctask, sizeof(void*));
		scsi_queue_work(shost, &conn->xmitwork);
	} else {
		if (scmd->request_bufflen) {
			done(scmd);
			spin_lock_bh(&conn->session->lock);
			__kfifo_put(conn->session->cmdpool.queue, (void*)&ctask, sizeof(void*));
			scmd->sc_data_direction = DMA_TO_DEVICE;
			iscsi_tcp_cleanup_ctask(ctask->conn, ctask);
			spin_unlock_bh(&conn->session->lock);
		} else {
			scmd->done = done;
			__kfifo_put(ctask->conn->xmitqueue, (void*)&ctask, sizeof(void*));
			scsi_queue_work(shost, &ctask->conn->xmitwork);
		}
	}
	return 0;
}

static int istgt_transfer_data(struct scsi_cmnd *scmd,
				  void (*done)(struct scsi_cmnd *))
{
	struct iscsi_cmd_task *ctask = (struct iscsi_cmd_task *) scmd->SCp.ptr;
	struct iscsi_tcp_cmd_task *tcp_ctask = ctask->dd_data;
	struct iscsi_cls_session *cls_session = session_to_cls(ctask->conn->session);
	struct Scsi_Host *shost = iscsi_session_to_shost(cls_session);

	dprintk("%p %x %x %u %u\n", ctask, ctask->hdr->opcode & ISCSI_OPCODE_MASK,
		ctask->hdr->cdb[0], scmd->request_bufflen, scmd->sc_data_direction);

	tcp_ctask->sg = scmd->request_buffer;
	tcp_ctask->sg_count = 0;
	ctask->data_count = scmd->request_bufflen;

	if (scmd->sc_data_direction == DMA_TO_DEVICE) {
		struct iscsi_tcp_conn *tcp_conn = ctask->conn->dd_data;
		struct sock *sk = tcp_conn->sock->sk;

		/* FIXME: too hacky */
		bh_lock_sock(sk);

		if (tcp_conn->in.ctask == ctask) {
			clear_bit(ISCSI_SUSPEND_BIT, &ctask->conn->suspend_rx);
			sk->sk_data_ready(sk, 0);
		}

		bh_unlock_sock(sk);
	} else {
		scmd->done = done;
		__kfifo_put(ctask->conn->xmitqueue, (void*)&ctask, sizeof(void*));
		scsi_queue_work(shost, &ctask->conn->xmitwork);
	}

	return 0;
}

static void istgt_response_build(struct iscsi_cmd_task *ctask)
{
	struct iscsi_tcp_cmd_task *tcp_ctask = ctask->dd_data;
	struct iscsi_data_task *dtask;
	struct scsi_cmnd *sc = ctask->sc;
	struct scatterlist *sg = sc->request_buffer;
	struct iscsi_session *session = ctask->conn->session;

	tcp_ctask->xmstate |= XMSTATE_UNS_HDR;

	dtask = mempool_alloc(tcp_ctask->datapool, GFP_ATOMIC);
	BUG_ON(!dtask);
	memset(&dtask->hdr, 0, sizeof(struct iscsi_data));
	list_add(&dtask->item, &tcp_ctask->dataqueue);
	tcp_ctask->dtask = dtask;

	iscsi_buf_init_iov(&tcp_ctask->headbuf, (char*)&dtask->hdr,
			   sizeof(struct iscsi_hdr));

	if (ctask->data_count) {
		struct iscsi_buf *ibuf = &tcp_ctask->sendbuf;
		iscsi_buf_init_sg(&tcp_ctask->sendbuf,
				  &sg[tcp_ctask->sg_count++]);
		tcp_ctask->sg = sg;
		dprintk("%p %u %u %u\n", ibuf->sg.page, ibuf->sg.offset,
			ibuf->sg.length, ibuf->use_sendmsg);
		tcp_ctask->xmstate |= XMSTATE_UNS_DATA;
	}

	switch (ctask->hdr->opcode & ISCSI_OPCODE_MASK) {
	case ISCSI_OP_SCSI_CMD:
	{
		/* FIXME: multiple data rsp (conn->max_xmit_dlength) */
		if (ctask->data_count) {
			struct iscsi_data_rsp *hdr =
				(struct iscsi_data_rsp *) &dtask->hdr;
			char *buf = page_address(tcp_ctask->headbuf.sg.page) +
				tcp_ctask->headbuf.sg.offset;
			u32 residual, exp_datalen = be32_to_cpu(ctask->hdr->data_length);

			memset(hdr, 0, sizeof(*hdr));
			hdr->opcode = ISCSI_OP_SCSI_DATA_IN;
			hdr->itt = ctask->hdr->itt;
			hdr->ttt = cpu_to_be32(ISCSI_RESERVED_TAG);
			hdr->offset = 0;
			hdr->statsn = cpu_to_be32(ctask->conn->exp_statsn++);
			hdr->exp_cmdsn = cpu_to_be32(session->exp_cmdsn);
			hdr->max_cmdsn =
				cpu_to_be32(session->exp_cmdsn + session->cmds_max / 2);
			hdr->datasn = cpu_to_be32(ctask->datasn++);
			hdr->flags = ISCSI_FLAG_CMD_FINAL | ISCSI_FLAG_DATA_STATUS;

			if (ctask->data_count < exp_datalen) {
				hdr->flags |= ISCSI_FLAG_CMD_UNDERFLOW;
				residual = exp_datalen - ctask->data_count;
			} else if (ctask->data_count > exp_datalen) {
				hdr->flags |= ISCSI_FLAG_CMD_OVERFLOW;
				residual =  ctask->data_count - exp_datalen;
				ctask->data_count = exp_datalen;
			} else
				residual = 0;

			hdr->residual_count = cpu_to_be32(residual);
			hton24(hdr->dlength, ctask->data_count);

			dprintk("%p %p %x %x %x %x\n",
				buf, hdr, buf[0], buf[1], buf[2], buf[3]);
		} else {
			struct iscsi_cmd_rsp *hdr =
				(struct iscsi_cmd_rsp *) &dtask->hdr;

			memset(hdr, 0, sizeof(*hdr));
			hdr->opcode = ISCSI_OP_SCSI_CMD_RSP;
			hdr->itt = ctask->hdr->itt;
			hdr->flags = ISCSI_FLAG_CMD_FINAL;
			hdr->response = ISCSI_STATUS_CMD_COMPLETED;
			hdr->cmd_status = SAM_STAT_GOOD;
			hdr->statsn = cpu_to_be32(ctask->conn->exp_statsn++);
			hdr->exp_cmdsn = cpu_to_be32(session->exp_cmdsn);
			hdr->max_cmdsn =
				cpu_to_be32(session->exp_cmdsn + session->cmds_max / 2);
		}
		break;
	}
	case ISCSI_OP_LOGOUT:
	{
		struct iscsi_logout_rsp *hdr =
			(struct iscsi_logout_rsp *) &dtask->hdr;

		hdr->opcode = ISCSI_OP_LOGOUT_RSP;
		hdr->flags = ISCSI_FLAG_CMD_FINAL;
		hdr->itt = tcp_ctask->hdr.itt;
		break;
	}
	default:
		break;
	}
}

static int
istgt_tcp_ctask_data_xmit(struct iscsi_conn *conn, struct iscsi_cmd_task *ctask)
{
	struct iscsi_tcp_cmd_task *tcp_ctask = ctask->dd_data;
	int err;

	while (1) {
		struct iscsi_buf *ibuf = &tcp_ctask->sendbuf;
		dprintk("%p %u %u %u\n", ibuf->sg.page, ibuf->sg.offset,
			ibuf->sg.length, ibuf->use_sendmsg);

		err = iscsi_sendpage(conn, &tcp_ctask->sendbuf,
				     &ctask->data_count, &tcp_ctask->sent);
		if (err) {
			dprintk("%u %u\n", ctask->data_count, tcp_ctask->sent);
			BUG_ON(1);
			return -EAGAIN;
		}

		if (!ctask->data_count)
			break;

		iscsi_buf_init_sg(&tcp_ctask->sendbuf,
				  &tcp_ctask->sg[tcp_ctask->sg_count++]);
	}

	return 0;
}


static int
istgt_tcp_ctask_xmit(struct iscsi_conn *conn, struct iscsi_cmd_task *ctask)
{
	struct iscsi_tcp_cmd_task *tcp_ctask = ctask->dd_data;
	struct scsi_cmnd *sc = ctask->sc;
	int err;

	dprintk("%p %x %x %u %x\n", ctask, ctask->hdr->opcode & ISCSI_OPCODE_MASK,
		ctask->hdr->cdb[0], ctask->data_count, tcp_ctask->xmstate);

	if (tcp_ctask->xmstate & XMSTATE_UNS_INIT) {
		istgt_response_build(ctask);
	}

	if (tcp_ctask->xmstate & XMSTATE_UNS_HDR) {
		char *buf = page_address(tcp_ctask->headbuf.sg.page) +
			tcp_ctask->headbuf.sg.offset;
		dprintk("%x %x %x %x\n", buf[0], buf[1], buf[2], buf[3]);
		err = iscsi_sendhdr(conn, &tcp_ctask->headbuf, ctask->data_count);
		if (err) {
			BUG_ON(1);
			eprintk("%d\n", err);
			return -EAGAIN;
		} else
			tcp_ctask->xmstate &= ~XMSTATE_UNS_HDR;
	}

	if (tcp_ctask->xmstate & XMSTATE_UNS_DATA) {
		err = istgt_tcp_ctask_data_xmit(conn, ctask);
		if (err)
			return -EAGAIN;
		else
			tcp_ctask->xmstate &= ~XMSTATE_UNS_DATA;
	}

	sc->done(sc);

	if (sc->sc_data_direction == DMA_TO_DEVICE || !sc->request_bufflen) {
		spin_lock_bh(&conn->session->lock);
		__kfifo_put(conn->session->cmdpool.queue, (void*)&ctask, sizeof(void*));
		/* fool iscsi_tcp_cleanup_ctask */
		if (sc)
			sc->sc_data_direction = DMA_TO_DEVICE;
		iscsi_tcp_cleanup_ctask(ctask->conn, ctask);
		spin_unlock_bh(&conn->session->lock);
	}

	return 0;
}

static int istgt_tcp_eh_abort_handler(struct scsi_cmnd *scmd)
{
	BUG();
	return 0;
}

#define	DEFAULT_NR_QUEUED_CMNDS	32
#define TGT_NAME "iscsi_tgt_tcp"

static struct scsi_host_template istgt_tcp_sht = {
	.name			= TGT_NAME,
	.module			= THIS_MODULE,
	.can_queue		= DEFAULT_NR_QUEUED_CMNDS,
	.sg_tablesize		= SG_ALL,
	.max_sectors		= 65535,
	.use_clustering		= DISABLE_CLUSTERING,
	.transfer_response	= istgt_transfer_response,
	.transfer_data		= istgt_transfer_data,
	.eh_abort_handler	= istgt_tcp_eh_abort_handler,
};

static struct iscsi_transport istgt_tcp_transport = {
	.owner			= THIS_MODULE,
	.name			= TGT_NAME,
	.host_template		= &istgt_tcp_sht,
	.conndata_size		= sizeof(struct iscsi_conn),
	.sessiondata_size	= sizeof(struct istgt_session),
	.max_conn		= 1,
	.max_cmd_len		= ISCSI_TCP_MAX_CMD_LEN,
	.create_session		= istgt_tcp_session_create,
	.destroy_session	= iscsi_tcp_session_destroy,
	.create_conn		= iscsi_tcp_conn_create,
	.destroy_conn		= iscsi_tcp_conn_destroy,
	.bind_conn		= istgt_tcp_conn_bind,
	.start_conn		= iscsi_conn_start,
	.set_param		= iscsi_conn_set_param,
	.terminate_conn		= iscsi_tcp_terminate_conn,
	.xmit_cmd_task		= istgt_tcp_ctask_xmit,
};

static int __init istgt_tcp_init(void)
{
	int err;
	printk("iSCSI Target over TCP\n");

	err = iscsi_tcp_init();
	if (err)
		return err;

	if (!iscsi_register_transport(&istgt_tcp_transport))
		goto call_iscsi_tcp_exit;
	return 0;

call_iscsi_tcp_exit:
	iscsi_tcp_exit();
	return -ENOMEM;
}

static void __exit istgt_tcp_exit(void)
{
	iscsi_tcp_exit();
	iscsi_unregister_transport(&istgt_tcp_transport);
}

module_init(istgt_tcp_init);
module_exit(istgt_tcp_exit);

MODULE_DESCRIPTION("iSCSI target over TCP");
MODULE_LICENSE("GPL");