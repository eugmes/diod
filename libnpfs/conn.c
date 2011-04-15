/*
 * Copyright (C) 2005 by Latchesar Ionkov <lucho@ionkov.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * LATCHESAR IONKOV AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Server should:
 * 1) accept a connection from client
 * 2) create a 'trans' instance for the connection
 * 3) call np_conn_create () to create 'conn' and thread to service requests
 */

/* Conn reference counting:
 * . np_conn_create () ref=0
 * . np_conn_read_proc () start ref++, finish ref--
 * . np_srv_add_conn () ref++,         np_srv_remove_conn () ref--
 * . np_req_alloc () ref++	       np_req_unref () ref--
 */

#if HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>

#include "9p.h"
#include "npfs.h"
#include "npfsimpl.h"

static Npfcall *_alloc_npfcall(int msize);
static void _free_npfcall(Npfcall *rc);
static void *np_conn_read_proc(void *);
static void np_conn_reset(Npconn *conn);

Npconn*
np_conn_create(Npsrv *srv, Nptrans *trans)
{
	Npconn *conn;
	int err;

	if (!(conn = malloc(sizeof(*conn)))) {
		errno = ENOMEM;
		return NULL;
	}
	pthread_mutex_init(&conn->lock, NULL);
	pthread_mutex_init(&conn->wlock, NULL);
	pthread_cond_init(&conn->resetcond, NULL);
	pthread_cond_init(&conn->resetdonecond, NULL);

	conn->refcount = 0;
	conn->resetting = 0;
	conn->srv = srv;
	conn->msize = srv->msize;
	conn->shutdown = 0;
	if (!(conn->fidpool = np_fidpool_create())) {
		free (conn);
		errno = ENOMEM;
		return NULL;
	}

	conn->trans = trans;
	conn->aux = NULL;
	np_srv_add_conn(srv, conn);

	err = pthread_create(&conn->rthread, NULL, np_conn_read_proc, conn);
	if (err != 0) {
		np_srv_remove_conn (srv, conn);
		free (conn);
		conn = NULL;
		errno = err;
		return NULL;
	}

	return conn;
}

void
np_conn_incref(Npconn *conn)
{
	pthread_mutex_lock(&conn->lock);
	conn->refcount++;
	pthread_mutex_unlock(&conn->lock);
}

void
np_conn_decref(Npconn *conn)
{
	pthread_mutex_lock(&conn->lock);
	assert(conn->refcount > 0);
	conn->refcount--;
	if (conn->refcount) {
		pthread_mutex_unlock(&conn->lock);
		return;
	}

	if (conn->fidpool) {
		np_fidpool_destroy(conn->fidpool);
		conn->fidpool = NULL;
	}
	
	pthread_mutex_unlock(&conn->lock);
	pthread_mutex_destroy(&conn->lock);
	pthread_cond_destroy(&conn->resetcond);
	pthread_cond_destroy(&conn->resetdonecond);
	free(conn);
}

static void
_debug_trace (Npsrv *srv, Npfcall *fc)
{
	char s[512];

	np_snprintfcall(s, sizeof (s), fc);
	srv->debugprintf("%s", s);
}

/* Per-connection read thread.
 */
static void *
np_conn_read_proc(void *a)
{
	int i, n, size, msize;
	Npsrv *srv;
	Npconn *conn = (Npconn *)a;
	Nptrans *trans;
	Npreq *req;
	Npfcall *fc, *fc1;

	pthread_detach(pthread_self());
	np_conn_incref(conn);
	srv = conn->srv;
	msize = conn->msize;
	fc = _alloc_npfcall(msize);
	n = 0;
	while (fc && conn->trans && (i = np_trans_read(conn->trans, fc->pkt + n, msize - n)) > 0) {
		pthread_mutex_lock(&conn->lock);
		if (conn->resetting) {
			pthread_cond_wait(&conn->resetdonecond, &conn->lock);
			n = 0;	/* discard all input */
			i = 0;
		}
		pthread_mutex_unlock(&conn->lock);
		n += i;

again:
		size = np_peek_size (fc->pkt, n);
		if (size == 0 || n < size)
			continue;

		if (!np_deserialize(fc, fc->pkt))
			break;

		if ((srv->debuglevel & DEBUG_9P_TRACE) && srv->debugprintf)
			_debug_trace (srv, fc);

		fc1 = _alloc_npfcall(msize);
		if (!fc1)
			break; /* FIXME */
		if (n > size)
			memmove(fc1->pkt, fc->pkt + size, n - size);
		n -= size;

		req = np_req_alloc(conn, fc);
		if (!req)
			break; /* FIXME */
		pthread_mutex_lock(&srv->lock);
		if (!conn->resetting)
			np_srv_add_req(srv, req);
		else 
			np_req_unref(req);
		pthread_mutex_unlock(&srv->lock);
		fc = fc1;
		if (n > 0)
			goto again;

	}

	pthread_mutex_lock(&conn->lock);
	trans = conn->trans;
	conn->trans = NULL;
	_free_npfcall(fc);
	pthread_mutex_unlock(&conn->lock);

	np_srv_remove_conn(conn->srv, conn);
	np_conn_reset(conn);

	if (trans)
		np_trans_destroy(trans);

	np_conn_decref(conn);
	return NULL;
}

static void
np_conn_reset(Npconn *conn)
{
	int i, n;
	Npsrv *srv;
	Npreq *req, *req1, *preqs, **reqs;

	pthread_mutex_lock(&conn->lock);
	conn->resetting = 1;
	pthread_mutex_unlock(&conn->lock);
	
	pthread_mutex_lock(&conn->srv->lock);
	srv = conn->srv;
	// first flush all outstanding requests
	preqs = NULL;
	req = srv->reqs_first;
	while (req != NULL) {
		req1 = req->next;
		if (req->conn == conn) {
			np_srv_remove_req(srv, req);
			req->next = preqs;
			preqs = req;
		}
		req = req1;
	}

	// then flush all working requests
	n = 0;
	req = conn->srv->workreqs;
	while (req != NULL) {
		if (req->conn == conn && req->tcall->type != P9_TVERSION)
			n++;

		req = req->next;
	}

	reqs = malloc(n * sizeof(Npreq *));
	if (reqs) {
		n = 0;
		req = conn->srv->workreqs;
		while (req != NULL) {
			if (req->conn == conn && req->tcall->type != P9_TVERSION)
				reqs[n++] = np_req_ref(req);
			req = req->next;
		}
	}
	pthread_mutex_unlock(&conn->srv->lock);
	if (!reqs) /* out of memory */
		return;

	req = preqs;
	while (req != NULL) {
		req1 = req->next;
		np_conn_respond(req);
		np_req_unref(req);
		req = req1;
	}

	for(i = 0; i < n; i++) {
		req = reqs[i];
		if (req->conn->srv->flush)
			(*req->conn->srv->flush)(req);
	}

	/* wait until all working requests finish */
/*
	pthread_mutex_lock(&conn->lock);
	while (1) {
		for(i = 0; i < n; i++) 
			if (!reqs[i]->responded)
				break;

		if (i >= n)
			break;

		pthread_cond_wait(&conn->resetcond, &conn->lock);
	}
*/
	pthread_mutex_lock(&srv->lock);
	while (1) {
		for(req = srv->workreqs; req != NULL; req = req->next)
			if (req->conn==conn && req->tcall->type != P9_TVERSION)
				break;

		if (req == NULL)
			break;

		pthread_cond_wait(&conn->resetcond, &srv->lock);
	}
	pthread_mutex_unlock(&srv->lock);

	if (conn->fidpool) {
		np_fidpool_destroy(conn->fidpool);
		conn->fidpool = NULL;
	}

	conn->resetting = 0;
	pthread_mutex_unlock(&conn->lock);

	/* free the working requests */
	for(i = 0; i < n; i++) 
		np_req_unref(reqs[i]);
	free(reqs);
}

void
np_conn_shutdown(Npconn *conn)
{
	Nptrans *trans;

	pthread_mutex_lock(&conn->lock);
	trans = conn->trans;
	conn->trans = NULL;
	pthread_mutex_unlock(&conn->lock);

	if (trans)
		np_trans_destroy(trans);
}

void
np_conn_respond(Npreq *req)
{
	int n;
	Npconn *conn = req->conn;
	Npsrv *srv = conn->srv;
	Nptrans *trans = NULL;
	Npfcall *rc = req->rcall;

	if (!rc)
		goto done;

	pthread_mutex_lock(&conn->lock);
	pthread_mutex_unlock(&conn->lock);

	if (conn->trans && !conn->resetting) {
		if ((srv->debuglevel & DEBUG_9P_TRACE) && srv->debugprintf)
			_debug_trace (srv, rc);
		pthread_mutex_lock(&conn->wlock);
		n = np_trans_write(conn->trans, rc->pkt, rc->size);
		pthread_mutex_unlock(&conn->wlock);

		if (n <= 0) { /* write error */
			pthread_mutex_lock(&conn->lock);
			trans = conn->trans;
			conn->trans = NULL;
			pthread_mutex_unlock(&conn->lock);
		}
	}

done:
	_free_npfcall(req->tcall);
	free(req->rcall);
	req->tcall = NULL;
	req->rcall = NULL;

	if (conn->resetting) {
		pthread_mutex_lock(&conn->srv->lock);
		pthread_cond_broadcast(&conn->resetcond);
		pthread_mutex_unlock(&conn->srv->lock);
	}

	if (trans)
		np_trans_destroy(trans); /* np_conn_read_proc will take care of resetting */
}

static Npfcall *
_alloc_npfcall(int msize)
{
	Npfcall *fc;

	if ((fc = malloc(sizeof(*fc) + msize)))
		fc->pkt = (u8*) fc + sizeof(*fc);

	return fc;
}

static void
_free_npfcall(Npfcall *rc)
{
	if (rc)
		free (rc);
}
