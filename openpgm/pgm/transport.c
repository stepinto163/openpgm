/* vim:ts=8:sts=8:sw=4:noai:noexpandtab
 *
 * PGM transport: manage incoming & outgoing sockets with ambient SPMs, 
 * transmit & receive windows.
 *
 * Copyright (c) 2006-2008 Miru Limited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <arpa/inet.h>

#include <glib.h>

#include "pgm/backtrace.h"
#include "pgm/log.h"
#include "pgm/packet.h"
#include "pgm/txwi.h"
#include "pgm/rxwi.h"
#include "pgm/transport.h"
#include "pgm/rate_control.h"
#include "pgm/sn.h"
#include "pgm/timer.h"
#include "pgm/checksum.h"
#include "pgm/reed_solomon.h"

//#define TRANSPORT_DEBUG
//#define TRANSPORT_SPM_DEBUG

#ifndef TRANSPORT_DEBUG
#	define g_trace(m,...)		while (0)
#else
#include <ctype.h>
#	ifdef TRANSPORT_SPM_DEBUG
#		define g_trace(m,...)		g_debug(__VA_ARGS__)
#	else
#		define g_trace(m,...)		do { if (strcmp((m),"SPM")) { g_debug(__VA_ARGS__); } } while (0)
#	endif
#endif


/* internal: Glib event loop GSource of spm & rx state timers */
struct pgm_timer_t {
	GSource		source;
	pgm_time_t	expiration;
	pgm_transport_t* transport;
};

typedef struct pgm_timer_t pgm_timer_t;


/* callback for pgm timer events */
typedef int (*pgm_timer_callback)(pgm_transport_t*);


/* global locals */
#undef G_LOG_DOMAIN
#define G_LOG_DOMAIN		"pgmtransport"

static int ipproto_pgm = IPPROTO_PGM;

GStaticRWLock pgm_transport_list_lock = G_STATIC_RW_LOCK_INIT;		/* list of all transports for admin interfaces */
GSList* pgm_transport_list = NULL;

/* helpers for pgm_peer_t */
#define next_nak_rb_expiry(r)       ( ((pgm_rxw_packet_t*)(r)->backoff_queue->tail->data)->nak_rb_expiry )
#define next_nak_rpt_expiry(r)      ( ((pgm_rxw_packet_t*)(r)->wait_ncf_queue->tail->data)->nak_rpt_expiry )
#define next_nak_rdata_expiry(r)    ( ((pgm_rxw_packet_t*)(r)->wait_data_queue->tail->data)->nak_rdata_expiry )


static GSource* pgm_create_timer (pgm_transport_t*);
static int pgm_add_timer_full (pgm_transport_t*, gint);
static int pgm_add_timer (pgm_transport_t*);

static gboolean pgm_timer_prepare (GSource*, gint*);
static gboolean pgm_timer_check (GSource*);
static gboolean pgm_timer_dispatch (GSource*, GSourceFunc, gpointer);

static GSourceFuncs g_pgm_timer_funcs = {
	.prepare		= pgm_timer_prepare,
	.check			= pgm_timer_check,
	.dispatch		= pgm_timer_dispatch,
	.finalize		= NULL,
	.closure_callback	= NULL
};

static int send_spm_unlocked (pgm_transport_t*);
static inline int send_spm (pgm_transport_t*);
static int send_spmr (pgm_peer_t*);
static int send_nak (pgm_peer_t*, guint32);
static int send_parity_nak (pgm_peer_t*, guint, guint);
static int send_nak_list (pgm_peer_t*, pgm_sqn_list_t*);
static int send_ncf (pgm_transport_t*, struct sockaddr*, struct sockaddr*, guint32, gboolean);
static int send_ncf_list (pgm_transport_t*, struct sockaddr*, struct sockaddr*, pgm_sqn_list_t*, gboolean);

static void nak_rb_state (pgm_peer_t*);
static void nak_rpt_state (pgm_peer_t*);
static void nak_rdata_state (pgm_peer_t*);
static void check_peer_nak_state (pgm_transport_t*);
static pgm_time_t min_nak_expiry (pgm_time_t, pgm_transport_t*);

static int send_rdata (pgm_transport_t*, guint32, gpointer, gsize);

static inline pgm_peer_t* pgm_peer_ref (pgm_peer_t*);
static inline void pgm_peer_unref (pgm_peer_t*);

static gboolean on_nak_pipe (GIOChannel*, GIOCondition, gpointer);
static gboolean on_timer_pipe (GIOChannel*, GIOCondition, gpointer);

static int on_spm (pgm_peer_t*, struct pgm_header*, gpointer, gsize);
static int on_spmr (pgm_transport_t*, pgm_peer_t*, struct pgm_header*, gpointer, gsize);
static int on_nak (pgm_transport_t*, struct pgm_header*, gpointer, gsize);
static int on_peer_nak (pgm_peer_t*, struct pgm_header*, gpointer, gsize);
static int on_ncf (pgm_peer_t*, struct pgm_header*, gpointer, gsize);
static int on_nnak (pgm_transport_t*, struct pgm_header*, gpointer, gsize);
static int on_odata (pgm_peer_t*, struct pgm_header*, gpointer, gsize);
static int on_rdata (pgm_peer_t*, struct pgm_header*, gpointer, gsize);

static gssize pgm_transport_send_one_unlocked (pgm_transport_t*, gpointer, gsize, int);
static gssize pgm_transport_send_one_copy_unlocked (pgm_transport_t*, gconstpointer, gsize, int);

static int get_opt_fragment (struct pgm_opt_header*, struct pgm_opt_fragment**);


/* re-entrant form of pgm_print_tsi()
 */
int
pgm_print_tsi_r (
	const pgm_tsi_t*	tsi,
	char*			buf,
	gsize			bufsize
	)
{
	g_return_val_if_fail (tsi != NULL, -EINVAL);
	g_return_val_if_fail (buf != NULL, -EINVAL);

	const guint8* gsi = (const guint8*)tsi;
	guint16 source_port = tsi->sport;
	snprintf(buf, bufsize, "%i.%i.%i.%i.%i.%i.%i",
		gsi[0], gsi[1], gsi[2], gsi[3], gsi[4], gsi[5], g_ntohs (source_port));
	return 0;
}

/* transform TSI to ASCII string form.
 *
 * on success, returns pointer to ASCII string.  on error, returns NULL.
 */
gchar*
pgm_print_tsi (
	const pgm_tsi_t*	tsi
	)
{
	g_return_val_if_fail (tsi != NULL, NULL);

	static char buf[sizeof("000.000.000.000.000.000.00000")];
	pgm_print_tsi_r (tsi, buf, sizeof(buf));
	return buf;
}

/* create hash value of TSI for use with GLib hash tables.
 *
 * on success, returns a hash value corresponding to the TSI.  on error, fails
 * on assert.
 */
inline guint
pgm_tsi_hash (
	gconstpointer v
        )
{
	g_assert( v != NULL );
	const pgm_tsi_t* tsi = v;
	char buf[sizeof("000.000.000.000.000.000.00000")];
	int valid = pgm_print_tsi_r(tsi, buf, sizeof(buf));
	g_assert( valid == 0 );
	return g_str_hash( buf );
}

/* compare two transport session identifier TSI values.
 *
 * returns TRUE if they are equal, FALSE if they are not.
 */
inline gboolean
pgm_tsi_equal (
	gconstpointer   v,
	gconstpointer   v2
        )
{
	return memcmp (v, v2, sizeof(struct pgm_tsi_t)) == 0;
}

gsize
pgm_transport_pkt_offset (
	gboolean		can_fragment
	)
{
	return can_fragment ? ( sizeof(struct pgm_header)
			      + sizeof(struct pgm_data)
			      + sizeof(struct pgm_opt_length)
	                      + sizeof(struct pgm_opt_header)
			      + sizeof(struct pgm_opt_fragment) )
			    : ( sizeof(struct pgm_header) + sizeof(struct pgm_data) );
}

/* fast log base 2 of power of 2
 */

inline guint
pgm_power2_log2 (
	guint		v
	)
{
	static const unsigned int b[] = {0xAAAAAAAA, 0xCCCCCCCC, 0xF0F0F0F0, 0xFF00FF00, 0xFFFF0000};
	unsigned int r = (v & b[0]) != 0;
	for (int i = 4; i > 0; i--) {
		r |= ((v & b[i]) != 0) << i;
	}
	return r;
}

/* calculate NAK_RB_IVL as random time interval 1 - NAK_BO_IVL.
 */
static inline guint32
nak_rb_ivl (
	pgm_transport_t* transport
	)
{
	return g_rand_int_range (transport->rand_, 1 /* us */, transport->nak_bo_ivl);
}

/* locked and rate regulated sendto
 *
 * on success, returns number of bytes sent.  on error, -1 is returned, and
 * errno set appropriately.
 */
static inline gssize
pgm_sendto (
	pgm_transport_t*	transport,
	gboolean		use_rate_limit,
	gboolean		use_router_alert,
	const void*		buf,
	gsize			len,
	int			flags,
	const struct sockaddr*	to,
	gsize			tolen
	)
{
	GStaticMutex* mutex = use_router_alert ? &transport->send_with_router_alert_mutex : &transport->send_mutex;
	int sock = use_router_alert ? transport->send_with_router_alert_sock : transport->send_sock;

	if (use_rate_limit)
	{
		int check = pgm_rate_check (transport->rate_control, len, flags);
		if (check < 0 && errno == EAGAIN)
		{
			return (gssize)check;
		}
	}

	g_static_mutex_lock (mutex);

	ssize_t sent = sendto (sock, buf, len, flags, to, (socklen_t)tolen);
	if (	sent < 0 &&
		errno != ENETUNREACH &&		/* Network is unreachable */
		errno != EHOSTUNREACH &&	/* No route to host */
		!( errno == EAGAIN && flags & MSG_DONTWAIT )	/* would block on non-blocking send */
	   )
	{
/* poll for cleared socket */
		struct pollfd p = {
			.fd		= transport->send_sock,
			.events		= POLLOUT,
			.revents	= 0
				  };
		int ready = poll (&p, 1, 500 /* ms */);
		if (ready > 0)
		{
			sent = sendto (sock, buf, len, flags, to, (socklen_t)tolen);
			if ( sent < 0 )
			{
				g_warning ("sendto %s failed: %i %s",
						inet_ntoa( ((const struct sockaddr_in*)to)->sin_addr ),
						errno,
						strerror (errno));
			}
		}
		else if (ready == 0)
		{
			g_warning ("sendto %s socket pollout timeout.",
					 inet_ntoa( ((const struct sockaddr_in*)to)->sin_addr ));
		}
		else
		{
			g_warning ("poll on blocked sendto %s socket failed: %i %s",
					inet_ntoa( ((const struct sockaddr_in*)to)->sin_addr ),
					errno,
					strerror (errno));
		}
	}

	g_static_mutex_unlock (mutex);

	return sent;
}

/* socket helper, for setting pipe ends non-blocking
 *
 * on success, returns 0.  on error, returns -1, and sets errno appropriately.
 */
int
pgm_set_nonblocking (
	int		filedes[2]
	)
{
	int retval = 0;

/* set write end non-blocking */
	int fd_flags = fcntl (filedes[1], F_GETFL);
	if (fd_flags < 0) {
		retval = fd_flags;
		goto out;
	}
	retval = fcntl (filedes[1], F_SETFL, fd_flags | O_NONBLOCK);
	if (retval < 0) {
		retval = fd_flags;
		goto out;
	}
/* set read end non-blocking */
	fcntl (filedes[0], F_GETFL);
	if (fd_flags < 0) {
		retval = fd_flags;
		goto out;
	}
	retval = fcntl (filedes[0], F_SETFL, fd_flags | O_NONBLOCK);
	if (retval < 0) {
		retval = fd_flags;
		goto out;
	}

out:
	return retval;
}		

/* startup PGM engine, mainly finding PGM protocol definition, if any from NSS
 *
 * on success, returns 0.
 */
int
pgm_init (void)
{
	int retval = 0;

/* ensure threading enabled */
	if (!g_thread_supported ()) g_thread_init (NULL);

/* ensure timer enabled */
	if (!pgm_time_supported ()) pgm_time_init();


/* find PGM protocol id */

// TODO: fix valgrind errors
#if HAVE_GETPROTOBYNAME_R
	char b[1024];
	struct protoent protobuf, *proto;
	e = getprotobyname_r("pgm", &protobuf, b, sizeof(b), &proto);
	if (e != -1 && proto != NULL) {
		if (proto->p_proto != ipproto_pgm) {
			g_trace("INFO","Setting PGM protocol number to %i from /etc/protocols.");
			ipproto_pgm = proto->p_proto;
		}
	}
#else
	struct protoent *proto = getprotobyname("pgm");
	if (proto != NULL) {
		if (proto->p_proto != ipproto_pgm) {
			g_trace("INFO","Setting PGM protocol number to %i from /etc/protocols.", proto->p_proto);
			ipproto_pgm = proto->p_proto;
		}
	}
#endif

	return retval;
}

/* destroy a pgm_transport object and contents, if last transport also destroy
 * associated event loop
 *
 * TODO: clear up locks on destruction: 1: flushing, 2: destroying:, 3: destroyed.
 *
 * If application calls a function on the transport after destroy() it is a
 * programmer error: segv likely to occur on unlock.
 *
 * on success, returns 0.  if transport is invalid, or previously destroyed,
 * returns -EINVAL.
 */

int
pgm_transport_destroy (
	pgm_transport_t*	transport,
	gboolean		flush
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);

	g_static_rw_lock_writer_lock (&pgm_transport_list_lock);
	pgm_transport_list = g_slist_remove (pgm_transport_list, transport);
	g_static_rw_lock_writer_unlock (&pgm_transport_list_lock);

/* rollback any pkt_dontwait APDU */
	if (transport->has_txw_writer_lock)
	{
		((pgm_txw_t*)transport->txw)->lead = transport->pkt_dontwait_state.first_sqn - 1;
		g_static_rw_lock_writer_unlock (&transport->txw_lock);
		transport->has_txw_writer_lock = FALSE;
	}

/* terminate & join internal thread */
#ifndef PGM_SINGLE_THREAD
	if (transport->timer_thread) {
		g_main_loop_quit (transport->timer_loop);
		g_thread_join (transport->timer_thread);
		transport->timer_thread = NULL;
	}
#endif /* !PGM_SINGLE_THREAD */

	g_static_mutex_lock (&transport->mutex);

/* assume lock from create() if not bound */
	if (transport->is_bound) {
		g_static_mutex_lock (&transport->send_mutex);
		g_static_mutex_lock (&transport->send_with_router_alert_mutex);
	}

/* flush data by sending heartbeat SPMs & processing NAKs until ambient */
	if (flush) {
	}

	if (transport->peers_hashtable) {
		g_hash_table_destroy (transport->peers_hashtable);
		transport->peers_hashtable = NULL;
	}
	if (transport->peers_list) {
		g_trace ("INFO","destroying peer data.");

		do {
			GList* next = transport->peers_list->next;
			pgm_peer_unref ((pgm_peer_t*)transport->peers_list->data);

			transport->peers_list = next;
		} while (transport->peers_list);
	}

/* clean up receiver trash stacks */
	if (transport->rx_data) {
		gpointer* p = NULL;
		while ( (p = g_trash_stack_pop (&transport->rx_data)) )
		{
			g_slice_free1 (transport->max_tpdu - transport->iphdr_len, p);
		}

		g_assert (transport->rx_data == NULL);
	}

	if (transport->rx_packet) {
		gpointer* p = NULL;
		while ( (p = g_trash_stack_pop (&transport->rx_packet)) )
		{
			g_slice_free1 (sizeof(pgm_rxw_packet_t), p);
		}

		g_assert (transport->rx_packet == NULL);
	}

	if (transport->txw) {
		g_trace ("INFO","destroying transmit window.");
		pgm_txw_shutdown (transport->txw);
		transport->txw = NULL;
	}

	if (transport->rate_control) {
		g_trace ("INFO","destroying rate control.");
		pgm_rate_destroy (transport->rate_control);
		transport->rate_control = NULL;
	}
	if (transport->recv_sock) {
		g_trace ("INFO","closing receive socket.");
		close(transport->recv_sock);
		transport->recv_sock = 0;
	}

	if (transport->send_sock) {
		g_trace ("INFO","closing send socket.");
		close(transport->send_sock);
		transport->send_sock = 0;
	}
	if (transport->send_with_router_alert_sock) {
		g_trace ("INFO","closing send with router alert socket.");
		close(transport->send_with_router_alert_sock);
		transport->send_with_router_alert_sock = 0;
	}

	if (transport->spm_heartbeat_interval) {
		g_free (transport->spm_heartbeat_interval);
		transport->spm_heartbeat_interval = NULL;
	}

	if (transport->rand_) {
		g_rand_free (transport->rand_);
		transport->rand_ = NULL;
	}
	if (transport->rdata_pipe[0]) {
		close (transport->rdata_pipe[0]);
		transport->rdata_pipe[0] = 0;
	}
	if (transport->rdata_pipe[1]) {
		close (transport->rdata_pipe[1]);
		transport->rdata_pipe[1] = 0;
	}
	if (transport->timer_pipe[0]) {
		close (transport->timer_pipe[0]);
		transport->timer_pipe[0] = 0;
	}
	if (transport->timer_pipe[1]) {
		close (transport->timer_pipe[1]);
		transport->timer_pipe[1] = 0;
	}

	if (transport->waiting_pipe[0]) {
		close (transport->waiting_pipe[0]);
		transport->waiting_pipe[0] = 0;
	}
	if (transport->waiting_pipe[1]) {
		close (transport->waiting_pipe[1]);
		transport->waiting_pipe[1] = 1;
	}

	g_static_rw_lock_free (&transport->peers_lock);

	g_static_rw_lock_free (&transport->txw_lock);

	if (transport->is_bound) {
		g_static_mutex_unlock (&transport->send_mutex);
		g_static_mutex_unlock (&transport->send_with_router_alert_mutex);
	}
	g_static_mutex_free (&transport->send_mutex);
	g_static_mutex_free (&transport->send_with_router_alert_mutex);

	g_static_mutex_unlock (&transport->mutex);
	g_static_mutex_free (&transport->mutex);

	if (transport->parity_buffer) {
		g_free (transport->parity_buffer);
		transport->parity_buffer = NULL;
	}
	if (transport->rs) {
		pgm_rs_destroy (transport->rs);
		transport->rs = NULL;
	}

	if (transport->rx_buffer) {
		g_free (transport->rx_buffer);
		transport->rx_buffer = NULL;
	}

	if (transport->piov) {
		g_free (transport->piov);
		transport->piov = NULL;
	}

	g_free (transport);

	g_trace ("INFO","finished.");
	return 0;
}

/* increase reference count for peer object
 *
 * on success, returns peer object.
 */
static inline pgm_peer_t*
pgm_peer_ref (
	pgm_peer_t*	peer
	)
{
	g_return_val_if_fail (peer != NULL, NULL);

	g_atomic_int_inc (&peer->ref_count);

	return peer;
}

/* decrease reference count of peer object, destroying on last reference.
 */
static inline void
pgm_peer_unref (
	pgm_peer_t*	peer
	)
{
	g_return_if_fail (peer != NULL);

	gboolean is_zero = g_atomic_int_dec_and_test (&peer->ref_count);

	if (G_UNLIKELY (is_zero))
	{
		pgm_rxw_shutdown (peer->rxw);
		peer->rxw = NULL;
		g_free (peer);
	}
}

/* timer thread execution function.
 *
 * when thread loop is terminated, returns NULL, to be returned by
 * g_thread_join()
 */
static gpointer
pgm_timer_thread (
	gpointer		data
	)
{
	pgm_transport_t* transport = (pgm_transport_t*)data;

	transport->timer_context = g_main_context_new ();
	g_mutex_lock (transport->thread_mutex);
	transport->timer_loop = g_main_loop_new (transport->timer_context, FALSE);
	g_cond_signal (transport->thread_cond);
	g_mutex_unlock (transport->thread_mutex);

	g_trace ("INFO", "pgm_timer_thread entering event loop.");
	g_main_loop_run (transport->timer_loop);
	g_trace ("INFO", "pgm_timer_thread leaving event loop.");

/* cleanup */
	g_main_loop_unref (transport->timer_loop);
	g_main_context_unref (transport->timer_context);

	return NULL;
}

/* create a pgm_transport object.  create sockets that require superuser priviledges, if this is
 * the first instance also create a real-time priority receiving thread.  if interface ports
 * are specified then UDP encapsulation will be used instead of raw protocol.
 *
 * if send == recv only two sockets need to be created iff ip headers are not required (IPv6).
 *
 * all receiver addresses must be the same family.
 * interface and multiaddr must be the same family.
 *
 * returns 0 on success, or -1 on error and sets errno appropriately.
 */

#if ( AF_INET != PF_INET ) || ( AF_INET6 != PF_INET6 )
#error AF_INET and PF_INET are different values, the bananas are jumping in their pyjamas!
#endif

int
pgm_transport_create (
	pgm_transport_t**	transport_,
	pgm_gsi_t*		gsi,
	guint16			dport,
	struct pgm_sock_mreq*	recv_smr,	/* receive port, multicast group & interface address */
	gsize			recv_len,
	struct pgm_sock_mreq*	send_smr	/* send ... */
	)
{
	guint16 udp_encap_port = ((struct sockaddr_in*)&send_smr->smr_multiaddr)->sin_port;

	g_return_val_if_fail (transport_ != NULL, -EINVAL);
	g_return_val_if_fail (recv_smr != NULL, -EINVAL);
	g_return_val_if_fail (recv_len > 0, -EINVAL);
	g_return_val_if_fail (recv_len <= IP_MAX_MEMBERSHIPS, -EINVAL);
	g_return_val_if_fail (send_smr != NULL, -EINVAL);
	for (unsigned i = 0; i < recv_len; i++)
	{
		g_return_val_if_fail (pgm_sockaddr_family(&recv_smr[i].smr_multiaddr) == pgm_sockaddr_family(&recv_smr[0].smr_multiaddr), -EINVAL);
		g_return_val_if_fail (pgm_sockaddr_family(&recv_smr[i].smr_multiaddr) == pgm_sockaddr_family(&recv_smr[i].smr_interface), -EINVAL);
	}
	g_return_val_if_fail (pgm_sockaddr_family(&send_smr->smr_multiaddr) == pgm_sockaddr_family(&send_smr->smr_interface), -EINVAL);

	int retval = 0;
	pgm_transport_t* transport;

/* create transport object */
	transport = g_malloc0 (sizeof(pgm_transport_t));

/* transport defaults */
	transport->can_send = TRUE;
	transport->can_recv = TRUE;
	transport->is_passive = FALSE;

/* regular send lock */
	g_static_mutex_init (&transport->send_mutex);

/* IP router alert send lock */
	g_static_mutex_init (&transport->send_with_router_alert_mutex);

/* timer lock */
	g_static_mutex_init (&transport->mutex);

/* transmit window read/write lock */
	g_static_rw_lock_init (&transport->txw_lock);

/* peer hash map & list lock */
	g_static_rw_lock_init (&transport->peers_lock);

/* lock tx until bound */
	g_static_mutex_lock (&transport->send_mutex);

	memcpy (&transport->tsi.gsi, gsi, 6);
	transport->dport = g_htons (dport);
	do {
		transport->tsi.sport = g_htons (g_random_int_range (0, UINT16_MAX));
	} while (transport->tsi.sport == transport->dport);

/* network data ports */
	transport->udp_encap_port = udp_encap_port;

/* copy network parameters */
	memcpy (&transport->send_smr, send_smr, sizeof(struct pgm_sock_mreq));
	for (unsigned i = 0; i < recv_len; i++)
	{
		memcpy (&transport->recv_smr[i], &recv_smr[i], sizeof(struct pgm_sock_mreq));
	}
	transport->recv_smr_len = recv_len;

/* open sockets to implement PGM */
	int socket_type, protocol;
	if (transport->udp_encap_port) {
		g_trace ("INFO", "opening UDP encapsulated sockets.");
		socket_type = SOCK_DGRAM;
		protocol = IPPROTO_UDP;
	} else {
		g_trace ("INFO", "opening raw sockets.");
		socket_type = SOCK_RAW;
		protocol = ipproto_pgm;
	}

	if ((transport->recv_sock = socket(pgm_sockaddr_family(&recv_smr[0].smr_interface),
						socket_type,
						protocol)) < 0)
	{
		retval = transport->recv_sock;
		if (retval == EPERM && 0 != getuid()) {
			g_critical ("PGM protocol requires this program to run as superuser.");
		}
		goto err_destroy;
	}

	if ((transport->send_sock = socket(pgm_sockaddr_family(&send_smr->smr_interface),
						socket_type,
						protocol)) < 0)
	{
		retval = transport->send_sock;
		goto err_destroy;
	}

	if ((transport->send_with_router_alert_sock = socket(pgm_sockaddr_family(&send_smr->smr_interface),
						socket_type,
						protocol)) < 0)
	{
		retval = transport->send_with_router_alert_sock;
		goto err_destroy;
	}

/* create timer thread */
#ifndef PGM_SINGLE_THREAD
	GError* err;
	GThread* thread;

/* set up condition for thread context & loop being ready */
	transport->thread_mutex = g_mutex_new ();
	transport->thread_cond = g_cond_new ();

	thread = g_thread_create_full (pgm_timer_thread,
					transport,
					0,
					TRUE,
					TRUE,
					G_THREAD_PRIORITY_HIGH,
					&err);
	if (thread) {
		transport->timer_thread = thread;
	} else {
		g_error ("thread failed: %i %s", err->code, err->message);
		goto err_destroy;
	}

	g_mutex_lock (transport->thread_mutex);
	while (!transport->timer_loop)
		g_cond_wait (transport->thread_cond, transport->thread_mutex);
	g_mutex_unlock (transport->thread_mutex);

	g_mutex_free (transport->thread_mutex);
	transport->thread_mutex = NULL;
	g_cond_free (transport->thread_cond);
	transport->thread_cond = NULL;

#endif /* !PGM_SINGLE_THREAD */

	*transport_ = transport;

	g_static_rw_lock_writer_lock (&pgm_transport_list_lock);
	pgm_transport_list = g_slist_append (pgm_transport_list, transport);
	g_static_rw_lock_writer_unlock (&pgm_transport_list_lock);

	return retval;

err_destroy:
	if (transport->thread_mutex) {
		g_mutex_free (transport->thread_mutex);
		transport->thread_mutex = NULL;
	}
	if (transport->thread_cond) {
		g_cond_free (transport->thread_cond);
		transport->thread_cond = NULL;
	}
	if (transport->timer_thread) {
	}
		
	if (transport->recv_sock) {
		close(transport->recv_sock);
		transport->recv_sock = 0;
	}
	if (transport->send_sock) {
		close(transport->send_sock);
		transport->send_sock = 0;
	}
	if (transport->send_with_router_alert_sock) {
		close(transport->send_with_router_alert_sock);
		transport->send_with_router_alert_sock = 0;
	}

	g_static_mutex_free (&transport->mutex);
	g_free (transport);
	transport = NULL;

	return retval;
}

/* helper to drop out of setuid 0 after creating PGM sockets
 */
void
pgm_drop_superuser (void)
{
	if (0 == getuid()) {
		setuid((gid_t)65534);
		setgid((uid_t)65534);
	}
}

/* 0 < tpdu < 65536 by data type (guint16)
 *
 * IPv4:   68 <= tpdu < 65536		(RFC 2765)
 * IPv6: 1280 <= tpdu < 65536		(RFC 2460)
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_max_tpdu (
	pgm_transport_t*	transport,
	guint16			max_tpdu
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (max_tpdu >= (sizeof(struct iphdr) + sizeof(struct pgm_header)), -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->max_tpdu = max_tpdu;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < hops < 256, hops == -1 use kernel default (ignored).
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_hops (
	pgm_transport_t*	transport,
	gint			hops
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (hops > 0, -EINVAL);
	g_return_val_if_fail (hops < 256, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->hops = hops;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* Linux 2.6 limited to millisecond resolution with conventional timers, however RDTSC
 * and future high-resolution timers allow nanosecond resolution.  Current ethernet technology
 * is limited to microseconds at best so we'll sit there for a bit.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_ambient_spm (
	pgm_transport_t*	transport,
	guint			spm_ambient_interval	/* in microseconds */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (spm_ambient_interval > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->spm_ambient_interval = spm_ambient_interval;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* an array of intervals appropriately tuned till ambient period is reached.
 *
 * array is zero leaded for ambient state, and zero terminated for easy detection.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_heartbeat_spm (
	pgm_transport_t*	transport,
	const guint*		spm_heartbeat_interval,
	int			len
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (len > 0, -EINVAL);
	for (int i = 0; i < len; i++) {
		g_return_val_if_fail (spm_heartbeat_interval[i] > 0, -EINVAL);
	}

	g_static_mutex_lock (&transport->mutex);
	if (transport->spm_heartbeat_interval)
		g_free (transport->spm_heartbeat_interval);
	transport->spm_heartbeat_interval = g_malloc (sizeof(guint) * (len+2));
	memcpy (&transport->spm_heartbeat_interval[1], spm_heartbeat_interval, sizeof(guint) * len);
	transport->spm_heartbeat_interval[0] = transport->spm_heartbeat_interval[len] = 0;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* set interval timer & expiration timeout for peer expiration, very lax checking.
 *
 * 0 < 2 * spm_ambient_interval <= peer_expiry
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_peer_expiry (
	pgm_transport_t*	transport,
	guint			peer_expiry	/* in microseconds */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (peer_expiry > 0, -EINVAL);
	g_return_val_if_fail (peer_expiry >= 2 * transport->spm_ambient_interval, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->peer_expiry = peer_expiry;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* set maximum back off range for listening for multicast SPMR
 *
 * 0 < spmr_expiry < spm_ambient_interval
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_spmr_expiry (
	pgm_transport_t*	transport,
	guint			spmr_expiry	/* in microseconds */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (spmr_expiry > 0, -EINVAL);
	g_return_val_if_fail (transport->spm_ambient_interval > spmr_expiry, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->spmr_expiry = spmr_expiry;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < txw_preallocate <= txw_sqns 
 *
 * can only be enforced at bind.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_preallocate (
	pgm_transport_t*	transport,
	guint			sqns
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (sqns > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_preallocate = sqns;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < txw_sqns < one less than half sequence space
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_sqns (
	pgm_transport_t*	transport,
	guint			sqns
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (sqns < ((UINT32_MAX/2)-1), -EINVAL);
	g_return_val_if_fail (sqns > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_sqns = sqns;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < secs < ( txw_sqns / txw_max_rte )
 *
 * can only be enforced upon bind.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_secs (
	pgm_transport_t*	transport,
	guint			secs
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (secs > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_secs = secs;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < txw_max_rte < interface capacity
 *
 *  10mb :   1250000
 * 100mb :  12500000
 *   1gb : 125000000
 *
 * no practical way to determine upper limit and enforce.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_txw_max_rte (
	pgm_transport_t*	transport,
	guint			max_rte
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (max_rte > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->txw_max_rte = max_rte;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < rxw_preallocate <= rxw_sqns 
 *
 * can only be enforced at bind.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_rxw_preallocate (
	pgm_transport_t*	transport,
	guint			sqns
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (sqns > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->rxw_preallocate = sqns;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < rxw_sqns < one less than half sequence space
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_rxw_sqns (
	pgm_transport_t*	transport,
	guint			sqns
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (sqns < ((UINT32_MAX/2)-1), -EINVAL);
	g_return_val_if_fail (sqns > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->rxw_sqns = sqns;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < secs < ( rxw_sqns / rxw_max_rte )
 *
 * can only be enforced upon bind.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_rxw_secs (
	pgm_transport_t*	transport,
	guint			secs
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (secs > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->rxw_secs = secs;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < rxw_max_rte < interface capacity
 *
 *  10mb :   1250000
 * 100mb :  12500000
 *   1gb : 125000000
 *
 * no practical way to determine upper limit and enforce.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_rxw_max_rte (
	pgm_transport_t*	transport,
	guint			max_rte
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (max_rte > 0, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->rxw_max_rte = max_rte;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}


/* 0 < wmem < wmem_max (user)
 *
 * operating system and sysctl dependent maximum, minimum on Linux 256 (doubled).
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_sndbuf (
	pgm_transport_t*	transport,
	int			size		/* not gsize/gssize as we propogate to setsockopt() */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (size > 0, -EINVAL);

	int wmem_max;
	FILE *fp = fopen ("/proc/sys/net/core/wmem_max", "r");
	if (fp) {
		fscanf (fp, "%d", &wmem_max);
		fclose (fp);
		g_return_val_if_fail (size <= wmem_max, -EINVAL);
	} else {
		g_warning ("cannot open /proc/sys/net/core/wmem_max");
	}

	g_static_mutex_lock (&transport->mutex);
	transport->sndbuf = size;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* 0 < rmem < rmem_max (user)
 *
 * minimum on Linux is 2048 (doubled).
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_rcvbuf (
	pgm_transport_t*	transport,
	int			size		/* not gsize/gssize */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);
	g_return_val_if_fail (size > 0, -EINVAL);

	int rmem_max;
	FILE *fp = fopen ("/proc/sys/net/core/rmem_max", "r");
	if (fp) {
		fscanf (fp, "%d", &rmem_max);
		fclose (fp);
		g_return_val_if_fail (size <= rmem_max, -EINVAL);
	} else {
		g_warning ("cannot open /proc/sys/net/core/rmem_max");
	}

	g_static_mutex_lock (&transport->mutex);
	transport->rcvbuf = size;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* Actual NAK back-off, NAK_RB_IVL, is random time interval 1 < NAK_BO_IVL,
 * randomized to reduce storms.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_nak_bo_ivl (
	pgm_transport_t*	transport,
	guint			usec		/* microseconds */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->nak_bo_ivl = usec;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* Set NAK_RPT_IVL, the repeat interval before re-sending a NAK.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */
int
pgm_transport_set_nak_rpt_ivl (
	pgm_transport_t*	transport,
	guint			usec		/* microseconds */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->nak_rpt_ivl = usec;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* Set NAK_RDATA_IVL, the interval waiting for data.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */
int
pgm_transport_set_nak_rdata_ivl (
	pgm_transport_t*	transport,
	guint			usec		/* microseconds */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->nak_rdata_ivl = usec;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* statistics are limited to guint8, i.e. 255 retries
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */
int
pgm_transport_set_nak_data_retries (
	pgm_transport_t*	transport,
	guint			cnt
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->nak_data_retries = cnt;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* statistics are limited to guint8, i.e. 255 retries
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */
int
pgm_transport_set_nak_ncf_retries (
	pgm_transport_t*	transport,
	guint			cnt
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->nak_ncf_retries = cnt;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* context aware g_io helpers
 *
 * on success, returns id of GSource.
 */
static guint
g_io_add_watch_context_full (
	GIOChannel*		channel,
	GMainContext*		context,
	gint			priority,
	GIOCondition		condition,
	GIOFunc			function,
	gpointer		user_data,
	GDestroyNotify		notify
	)
{
	GSource *source;
	guint id;
  
	g_return_val_if_fail (channel != NULL, 0);

	source = g_io_create_watch (channel, condition);

	if (priority != G_PRIORITY_DEFAULT)
		g_source_set_priority (source, priority);
	g_source_set_callback (source, (GSourceFunc)function, user_data, notify);

	id = g_source_attach (source, context);
	g_source_unref (source);

	return id;
}

/* bind the sockets to the link layer to start receiving data.
 *
 * returns 0 on success, or -1 on error and sets errno appropriately,
 *			 or -2 on NS lookup error and sets h_errno appropriately.
 */

int
pgm_transport_bind (
	pgm_transport_t*	transport
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (!transport->is_bound, -EINVAL);

	int retval = 0;

	g_static_mutex_lock (&transport->mutex);

	g_trace ("INFO","creating new random number generator.");
	transport->rand_ = g_rand_new();

	if (transport->can_send)
	{
		g_trace ("INFO","create rx to timer pipe.");
		retval = pipe (transport->rdata_pipe);
		if (retval < 0) {
			goto out;
		}
		g_trace ("INFO","create tx to timer pipe.");
		retval = pipe (transport->timer_pipe);
		if (retval < 0) {
			goto out;
		}
	}
	if (transport->can_recv)
	{
		g_trace ("INFO","create waiting notify pipe.");
		retval = pipe (transport->waiting_pipe);
		if (retval < 0) {
			goto out;
		}
	}

	retval = pgm_set_nonblocking (transport->rdata_pipe);
	if (retval) {
		goto out;
	}
	retval = pgm_set_nonblocking (transport->timer_pipe);
	if (retval < 0) {
		goto out;
	}
	retval = pgm_set_nonblocking (transport->waiting_pipe);
	if (retval < 0) {
		goto out;
	}

/* determine IP header size for rate regulation engine & stats */
	switch (pgm_sockaddr_family(&transport->send_smr.smr_interface)) {
	case AF_INET:
		transport->iphdr_len = sizeof(struct iphdr);
		break;

	case AF_INET6:
		transport->iphdr_len = 40;	/* sizeof(struct ipv6hdr) */
		break;
	}
	g_trace ("INFO","assuming IP header size of %i bytes", transport->iphdr_len);

	if (transport->udp_encap_port)
	{
		guint udphdr_len = sizeof( struct udphdr );
		g_trace ("INFO","assuming UDP header size of %i bytes", udphdr_len);
		transport->iphdr_len += udphdr_len;
	}

	transport->max_tsdu = transport->max_tpdu - transport->iphdr_len - pgm_transport_pkt_offset (FALSE);
	transport->max_tsdu_fragment = transport->max_tpdu - transport->iphdr_len - pgm_transport_pkt_offset (TRUE);

	if (transport->can_send)
	{
		g_trace ("INFO","construct transmit window.");
		transport->txw = pgm_txw_init (transport->max_tpdu - transport->iphdr_len,
						transport->txw_preallocate,
						transport->txw_sqns,
						transport->txw_secs,
						transport->txw_max_rte);
	}

/* create peer list */
	if (transport->can_recv)
	{
		transport->peers_hashtable = g_hash_table_new (pgm_tsi_hash, pgm_tsi_equal);
	}

	if (!transport->udp_encap_port)
	{
/* include IP header only for incoming data */
		retval = pgm_sockaddr_hdrincl (transport->recv_sock, pgm_sockaddr_family(&transport->recv_smr[0].smr_interface), TRUE);
		if (retval < 0) {
			goto out;
		}
	}

/* buffers, set size first then re-read to confirm actual value */
	if (transport->rcvbuf)
	{
		retval = setsockopt(transport->recv_sock, SOL_SOCKET, SO_RCVBUF, (char*)&transport->rcvbuf, sizeof(transport->rcvbuf));
		if (retval < 0) {
			goto out;
		}
	}
	if (transport->sndbuf)
	{
		retval = setsockopt(transport->send_sock, SOL_SOCKET, SO_SNDBUF, (char*)&transport->sndbuf, sizeof(transport->sndbuf));
		if (retval < 0) {
			goto out;
		}
		retval = setsockopt(transport->send_with_router_alert_sock, SOL_SOCKET, SO_SNDBUF, (char*)&transport->sndbuf, sizeof(transport->sndbuf));
		if (retval < 0) {
			goto out;
		}
	}

/* Most socket-level options utilize an int parameter for optval. */
	int buffer_size;
	socklen_t len = sizeof(buffer_size);
	retval = getsockopt(transport->recv_sock, SOL_SOCKET, SO_RCVBUF, &buffer_size, &len);
	if (retval < 0) {
		goto out;
	}
	g_trace ("INFO","receive buffer set at %i bytes.", buffer_size);

	retval = getsockopt(transport->send_sock, SOL_SOCKET, SO_SNDBUF, &buffer_size, &len);
	if (retval < 0) {
		goto out;
	}
	retval = getsockopt(transport->send_with_router_alert_sock, SOL_SOCKET, SO_SNDBUF, &buffer_size, &len);
	if (retval < 0) {
		goto out;
	}
	g_trace ("INFO","send buffer set at %i bytes.", buffer_size);

/* bind udp unicast sockets to interfaces, note multicast on a bound interface is
 * fruity on some platforms so callee should specify any interface.
 *
 * after binding default interfaces (0.0.0.0) are resolved
 */
/* TODO: different ports requires a new bound socket */

#ifdef CONFIG_BIND_INADDR_ANY
	struct sockaddr_storage bind_sockaddr;
	memcpy (&bind_sockaddr, &transport->recv_smr[0].smr_interface, pgm_sockaddr_len(&transport->recv_smr[0].smr_interface));
	((struct sockaddr_in*)&bind_sockaddr)->sin_addr.s_addr = INADDR_ANY;
	retval = bind (transport->recv_sock,
			(struct sockaddr*)&bind_sockaddr,
			pgm_sockaddr_len(&transport->recv_smr[0].smr_interface));
#else
	retval = bind (transport->recv_sock,
			(struct sockaddr*)&transport->recv_smr[0].smr_interface,
			pgm_sockaddr_len(&transport->recv_smr[0].smr_interface));
#endif
	if (retval < 0) {
#ifdef TRANSPORT_DEBUG
		int errno_ = errno;
		char s[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->recv_smr[0].smr_interface, s, sizeof(s));
		g_trace ("INFO","bind failed on recv_smr[0] interface %s: %s", s, strerror(errno_));
		errno = errno_;
#endif
		goto out;
	}

/* resolve bound address if 0.0.0.0 */
	if (((struct sockaddr_in*)&transport->recv_smr[0].smr_interface)->sin_addr.s_addr == INADDR_ANY)
	{
		char hostname[NI_MAXHOST + 1];
		gethostname (hostname, sizeof(hostname));
		struct hostent *he = gethostbyname (hostname);
		if (he == NULL) {
			retval = -2;
			g_trace ("INFO","gethostbyname failed on local hostname: %s", hstrerror (h_errno));
			goto out;
		}

		((struct sockaddr_in*)(&transport->recv_smr[0].smr_interface))->sin_addr.s_addr = ((struct in_addr*)(he->h_addr_list[0]))->s_addr;
	}

#ifdef TRANSPORT_DEBUG
	{
		char s[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->recv_smr[0].smr_interface, s, sizeof(s));
		g_trace ("INFO","bind succeeded on recv_smr[0] interface %s", s);
	}
#endif

	retval = bind (transport->send_sock,
			(struct sockaddr*)&transport->send_smr.smr_interface,
			pgm_sockaddr_len(&transport->send_smr.smr_interface));
	if (retval < 0) {
#ifdef TRANSPORT_DEBUG
		int errno_ = errno;
		char s[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s, sizeof(s));
		g_trace ("INFO","bind failed on send_smr interface %s: %s", s, strerror(errno_));
		errno = errno_;
#endif
		goto out;
	}

/* resolve bound address if 0.0.0.0 */
	if (((struct sockaddr_in*)&transport->send_smr.smr_interface)->sin_addr.s_addr == INADDR_ANY)
	{
		char hostname[NI_MAXHOST + 1];
		gethostname (hostname, sizeof(hostname));
		struct hostent *he = gethostbyname (hostname);
		if (he == NULL) {
			retval = -2;
			g_trace ("INFO","gethostbyname failed on local hostname: %s", hstrerror (h_errno));
			goto out;
		}

		((struct sockaddr_in*)&transport->send_smr.smr_interface)->sin_addr.s_addr = ((struct in_addr*)(he->h_addr_list[0]))->s_addr;
	}

#ifdef TRANSPORT_DEBUG
	{
		char s[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s, sizeof(s));
		g_trace ("INFO","bind succeeded on send_smr interface %s", s);
	}
#endif

	retval = bind (transport->send_with_router_alert_sock,
			(struct sockaddr*)&transport->send_smr.smr_interface,
			pgm_sockaddr_len(&transport->send_smr.smr_interface));
	if (retval < 0) {
#ifdef TRANSPORT_DEBUG
		int errno_ = errno;
		char s[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s, sizeof(s));
		g_trace ("INFO","bind (router alert) failed on send_smr interface %s: %s", s, strerror(errno_));
		errno = errno_;
#endif
		goto out;
	}

#ifdef TRANSPORT_DEBUG
	{
		char s[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s, sizeof(s));
		g_trace ("INFO","bind (router alert) succeeded on send_smr interface %s", s);
	}
#endif

/* receiving groups (multiple) */
/* TODO: add IPv6 multicast membership? */
	for (unsigned i = 0; i < transport->recv_smr_len; i++)
	{
		struct pgm_sock_mreq* p = &transport->recv_smr[i];
		retval = pgm_sockaddr_add_membership (transport->recv_sock, p);
		if (retval < 0) {
#ifdef TRANSPORT_DEBUG
			int errno_ = errno;
			char s1[INET6_ADDRSTRLEN], s2[INET6_ADDRSTRLEN];
			pgm_sockaddr_ntop (&p->smr_multiaddr, s1, sizeof(s1));
			pgm_sockaddr_ntop (&p->smr_interface, s2, sizeof(s2));
			g_trace ("INFO","sockaddr_add_membership failed on recv_smr[%i] multiaddr %s interface %s: %s", i, s1, s2, strerror(errno_));
			errno = errno_;
#endif
			goto out;
		}
#ifdef TRANSPORT_DEBUG
		else
		{
			char s1[INET6_ADDRSTRLEN], s2[INET6_ADDRSTRLEN];
			pgm_sockaddr_ntop (&p->smr_multiaddr, s1, sizeof(s1));
			pgm_sockaddr_ntop (&p->smr_interface, s2, sizeof(s2));
			g_trace ("INFO","sockaddr_add_membership succeeded on recv_smr[%i] multiaddr %s interface %s", i, s1, s2);
		}
#endif
	}

/* send group (singular) */
	retval = pgm_sockaddr_multicast_if (transport->send_sock, &transport->send_smr);
	if (retval < 0) {
#ifdef TRANSPORT_DEBUG
		int errno_ = errno;
		char s1[INET6_ADDRSTRLEN], s2[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_multiaddr, s1, sizeof(s1));
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s2, sizeof(s2));
		g_trace ("INFO","sockaddr_multicast_if failed on send_smr multiaddr %s interface %s: %s", s1, s2, strerror(errno_));
		errno = errno_;
#endif
		goto out;
	}
#ifdef TRANSPORT_DEBUG
	else
	{
		char s1[INET6_ADDRSTRLEN], s2[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_multiaddr, s1, sizeof(s1));
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s2, sizeof(s2));
		g_trace ("INFO","sockaddr_multicast_if succeeded on send_smr multiaddr %s interface %s", s1, s2);
	}
#endif
	retval = pgm_sockaddr_multicast_if (transport->send_with_router_alert_sock, &transport->send_smr);
	if (retval < 0) {
#ifdef TRANSPORT_DEBUG
		int errno_ = errno;
		char s1[INET6_ADDRSTRLEN], s2[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_multiaddr, s1, sizeof(s1));
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s2, sizeof(s2));
		g_trace ("INFO","sockaddr_multicast_if (router alert) failed on send_smr multiaddr %s interface %s: %s", s1, s2, strerror(errno_));
		errno = errno_;
#endif
		goto out;
	}
#ifdef TRANSPORT_DEBUG
	else
	{
		char s1[INET6_ADDRSTRLEN], s2[INET6_ADDRSTRLEN];
		pgm_sockaddr_ntop (&transport->send_smr.smr_multiaddr, s1, sizeof(s1));
		pgm_sockaddr_ntop (&transport->send_smr.smr_interface, s2, sizeof(s2));
		g_trace ("INFO","sockaddr_multicast_if (router alert) succeeded on send_smr multiaddr %s interface %s", s1, s2);
	}
#endif

/* multicast loopback */
	retval = pgm_sockaddr_multicast_loop (transport->recv_sock, pgm_sockaddr_family(&transport->recv_smr[0].smr_interface), FALSE);
	if (retval < 0) {
		goto out;
	}
	retval = pgm_sockaddr_multicast_loop (transport->send_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), FALSE);
	if (retval < 0) {
		goto out;
	}
	retval = pgm_sockaddr_multicast_loop (transport->send_with_router_alert_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), FALSE);
	if (retval < 0) {
		goto out;
	}

/* multicast ttl: many crappy network devices go CPU ape with TTL=1, 16 is a popular alternative */
	retval = pgm_sockaddr_multicast_hops (transport->recv_sock, pgm_sockaddr_family(&transport->recv_smr[0].smr_interface), transport->hops);
	if (retval < 0) {
		goto out;
	}
	retval = pgm_sockaddr_multicast_hops (transport->send_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), transport->hops);
	if (retval < 0) {
		goto out;
	}
	retval = pgm_sockaddr_multicast_hops (transport->send_with_router_alert_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), transport->hops);
	if (retval < 0) {
		goto out;
	}

/* set low packet latency preference for network elements */
	int tos = IPTOS_LOWDELAY;
	retval = pgm_sockaddr_tos (transport->send_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), tos);
	if (retval < 0) {
		goto out;
	}
	retval = pgm_sockaddr_tos (transport->send_with_router_alert_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), tos);
	if (retval < 0) {
		goto out;
	}

/* rx to timer pipe */
	if (transport->can_send)
	{
		transport->rdata_channel = g_io_channel_unix_new (transport->rdata_pipe[0]);
		g_io_add_watch_context_full (transport->rdata_channel, transport->timer_context, G_PRIORITY_HIGH, G_IO_IN, on_nak_pipe, transport, NULL);

/* tx to timer pipe */
		transport->timer_channel = g_io_channel_unix_new (transport->timer_pipe[0]);
		g_io_add_watch_context_full (transport->timer_channel, transport->timer_context, G_PRIORITY_HIGH, G_IO_IN, on_timer_pipe, transport, NULL);

/* create recyclable SPM packet */
		switch (pgm_sockaddr_family(&transport->recv_smr[0].smr_interface)) {
		case AF_INET:
			transport->spm_len = sizeof(struct pgm_header) + sizeof(struct pgm_spm);
			break;

		case AF_INET6:
			transport->spm_len = sizeof(struct pgm_header) + sizeof(struct pgm_spm6);
			break;
		}

		if (transport->use_proactive_parity || transport->use_ondemand_parity)
		{
			transport->spm_len += sizeof(struct pgm_opt_length) +
					      sizeof(struct pgm_opt_header) +
					      sizeof(struct pgm_opt_parity_prm);
		}

		transport->spm_packet = g_slice_alloc0 (transport->spm_len);

		struct pgm_header* header = (struct pgm_header*)transport->spm_packet;
		struct pgm_spm* spm = (struct pgm_spm*)( header + 1 );
		memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
		header->pgm_sport	= transport->tsi.sport;
		header->pgm_dport	= transport->dport;
		header->pgm_type	= PGM_SPM;

		pgm_sockaddr_to_nla ((struct sockaddr*)&transport->recv_smr[0].smr_interface, (char*)&spm->spm_nla_afi);

/* OPT_PARITY_PRM */
		if (transport->use_proactive_parity || transport->use_ondemand_parity)
		{
			header->pgm_options     = PGM_OPT_PRESENT | PGM_OPT_NETWORK;

			struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(spm + 1);
			opt_len->opt_type	= PGM_OPT_LENGTH;
			opt_len->opt_length	= sizeof(struct pgm_opt_length);
			opt_len->opt_total_length = g_htons (	sizeof(struct pgm_opt_length) +
								sizeof(struct pgm_opt_header) +
								sizeof(struct pgm_opt_parity_prm) );
			struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
			opt_header->opt_type	= PGM_OPT_PARITY_PRM | PGM_OPT_END;
			opt_header->opt_length	= sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_parity_prm);
			struct pgm_opt_parity_prm* opt_parity_prm = (struct pgm_opt_parity_prm*)(opt_header + 1);
			opt_parity_prm->opt_reserved = (transport->use_proactive_parity ? PGM_PARITY_PRM_PRO : 0) |
						       (transport->use_ondemand_parity ? PGM_PARITY_PRM_OND : 0);
			opt_parity_prm->parity_prm_tgs = g_htonl (transport->rs_k);
		}

/* setup rate control */
		if (transport->txw_max_rte)
		{
			g_trace ("INFO","Setting rate regulation to %i bytes per second.",
					transport->txw_max_rte);
	
			retval = pgm_rate_create (&transport->rate_control, transport->txw_max_rte, transport->iphdr_len);
			if (retval < 0) {
				goto out;
			}
		}

		g_trace ("INFO","adding dynamic timer");
		transport->next_poll = transport->next_ambient_spm = pgm_time_update_now() + transport->spm_ambient_interval;
		pgm_add_timer (transport);

/* announce new transport by sending out SPMs */
		send_spm_unlocked (transport);
		send_spm_unlocked (transport);
		send_spm_unlocked (transport);

/* parity buffer for odata/rdata transmission */
		if (transport->use_proactive_parity || transport->use_ondemand_parity)
		{
			g_trace ("INFO","Enabling Reed-Solomon forward error correction, RS(%i,%i).",
					transport->rs_n, transport->rs_k);
			transport->parity_buffer = g_malloc ( transport->max_tpdu );
			pgm_rs_create (&transport->rs, transport->rs_n, transport->rs_k);
		}
	}

/* allocate first incoming packet buffer */
	transport->rx_buffer = g_malloc ( transport->max_tpdu );

/* scatter/gather vector for contiguous reading from the window */
	transport->piov_len = IOV_MAX;
	transport->piov = g_malloc ( transport->piov_len * sizeof( struct iovec ) );

/* receiver trash */
	g_static_mutex_init (&transport->rx_mutex);

/* cleanup */
	transport->is_bound = TRUE;
	g_static_mutex_unlock (&transport->send_mutex);
	g_static_mutex_unlock (&transport->mutex);

	g_trace ("INFO","transport successfully created.");
out:
	return retval;
}

/* a peer in the context of the transport is another party on the network sending PGM
 * packets.  for each peer we need a receive window and network layer address (nla) to
 * which nak requests can be forwarded to.
 *
 * on success, returns new peer object.
 */

static pgm_peer_t*
new_peer (
	pgm_transport_t*	transport,
	pgm_tsi_t*		tsi,
	struct sockaddr*	src_addr,
	gsize			src_addr_len
	)
{
	pgm_peer_t* peer;

	g_trace ("INFO","new peer, tsi %s, local nla %s", pgm_print_tsi (tsi), inet_ntoa(((struct sockaddr_in*)src_addr)->sin_addr));

	peer = g_malloc0 (sizeof(pgm_peer_t));
	peer->expiry = pgm_time_update_now() + transport->peer_expiry;
	g_static_mutex_init (&peer->mutex);
	peer->transport = transport;
	memcpy (&peer->tsi, tsi, sizeof(pgm_tsi_t));
	((struct sockaddr_in*)&peer->nla)->sin_addr.s_addr = INADDR_ANY;
	memcpy (&peer->local_nla, src_addr, src_addr_len);

/* lock on rx window */
	peer->rxw = pgm_rxw_init (transport->max_tpdu - transport->iphdr_len,
				transport->rxw_preallocate,
				transport->rxw_sqns,
				transport->rxw_secs,
				transport->rxw_max_rte,
				&transport->rx_data,
				&transport->rx_packet,
				&transport->rx_mutex);

	peer->spmr_expiry = pgm_time_update_now() + transport->spmr_expiry;

/* prod timer thread if sleeping */
	g_static_mutex_lock (&transport->mutex);
	if (pgm_time_after( transport->next_poll, peer->spmr_expiry ))
	{
		transport->next_poll = peer->spmr_expiry;
		g_trace ("INFO","new_peer: prod timer thread");
		const char one = '1';
		if (1 != write (transport->timer_pipe[1], &one, sizeof(one))) {
			g_critical ("write to timer pipe failed :(");
			/* retval = -EINVAL; */
		}
	}
	g_static_mutex_unlock (&transport->mutex);

/* add peer to hash table and linked list */
	g_static_rw_lock_writer_lock (&transport->peers_lock);
	gpointer entry = pgm_peer_ref(peer);
	g_hash_table_insert (transport->peers_hashtable, &peer->tsi, entry);
/* there is no g_list_prepend_link(): */
	peer->link_.next = transport->peers_list;
	peer->link_.data = peer;
/* update next entries previous link */
	if (transport->peers_list)
		transport->peers_list->prev = &peer->link_;
/* update head */
	transport->peers_list = &peer->link_;
	g_static_rw_lock_writer_unlock (&transport->peers_lock);

	return peer;
}

/* data incoming on receive sockets, can be from a sender or receiver, or simply bogus.
 * for IPv4 we receive the IP header to handle fragmentation, for IPv6 we cannot so no idea :(
 *
 * recvmsgv reads a vector of apdus each contained in a IO scatter/gather array.
 *
 * can be called due to event from incoming socket(s) or timer induced data loss.
 *
 * on success, returns bytes read, on error returns -1.
 */

gssize
pgm_transport_recvmsgv (
	pgm_transport_t*	transport,
	pgm_msgv_t*		msg_start,
	gsize			msg_len,
	int			flags		/* MSG_DONTWAIT for non-blocking */
	)
{
	g_trace ("INFO", "pgm_transport_recvmsgv");
	g_assert( msg_len > 0 );

	gsize bytes_read = 0;
	pgm_msgv_t* pmsg = msg_start;
	pgm_msgv_t* msg_end = msg_start + msg_len;
	struct iovec* piov = transport->piov;
	struct iovec* iov_end = piov + transport->piov_len;

/* second, flush any remaining contiguous messages from previous call(s) */
	if (transport->peers_waiting || transport->peers_committed)
	{
		g_static_mutex_lock (&transport->waiting_mutex);
		while (transport->peers_committed)
		{
			pgm_rxw_t* committed_rxw = transport->peers_committed->data;

/* move any previous blocks to parity */
			pgm_rxw_release_committed (committed_rxw);

			transport->peers_committed->data = NULL;
			transport->peers_committed->next = NULL;
			transport->peers_committed = transport->peers_committed->next;
		}

		while (transport->peers_waiting)
		{
			pgm_rxw_t* waiting_rxw = transport->peers_waiting->data;
			gsize peer_bytes_read = pgm_rxw_readv (waiting_rxw, &pmsg, msg_end - pmsg, &piov, iov_end - piov);

/* clean up completed transmission groups */
			pgm_rxw_free_committed (waiting_rxw);
	
/* add to release list */
			waiting_rxw->commit_link.data = waiting_rxw;
			waiting_rxw->commit_link.next = transport->peers_committed;
			transport->peers_committed = &waiting_rxw->commit_link;

			if (peer_bytes_read) {
				bytes_read += peer_bytes_read;
	
				if (pmsg == msg_end || piov == iov_end)	/* commit full */
				{
					transport->peers_last_waiting = transport->peers_waiting;
					g_static_mutex_unlock (&transport->waiting_mutex);
					goto out;
				}
			}

/* next */
			transport->peers_waiting->data = NULL;
			transport->peers_waiting->next = NULL;
			transport->peers_waiting = transport->peers_waiting->next;
		}

		transport->peers_last_waiting = transport->peers_waiting;
		g_static_mutex_unlock (&transport->waiting_mutex);
	}

/* read the data:
 *
 * Buffer is always max_tpdu in length.  Ideally we have zero copy but the recv includes the ip & pgm headers and
 * the pgm options.  Over thousands of messages the gains by using less receive window memory are more conducive (maybe).
 *
 * We cannot actually block here as packets pushed by the timers need to be addressed too.
 */
	struct sockaddr_storage src_addr;
	socklen_t src_addr_len = sizeof(src_addr);
	ssize_t len;
	gsize bytes_received = 0;

recv_again:
	len = recvfrom (transport->recv_sock, transport->rx_buffer, transport->max_tpdu, MSG_DONTWAIT, (struct sockaddr*)&src_addr, &src_addr_len);

	if (len < 0) {
		if (bytes_received) {
			goto flush_waiting;
		} else {
			goto out;
		}
	} else if (len == 0) {
		goto out;
	}

/* succesfully read packet */
	bytes_received += len;

#ifdef TRANSPORT_DEBUG
	char s[INET6_ADDRSTRLEN];
	pgm_sockaddr_ntop ((struct sockaddr*)&src_addr, s, sizeof(s));
//	g_trace ("INFO","%i bytes received from %s", len, s);
#endif

/* verify IP and PGM header */
	struct sockaddr_storage dst_addr;
	socklen_t dst_addr_len = sizeof(dst_addr);
	struct pgm_header *pgm_header;
	gpointer packet;
	gsize packet_len;
	int e;

	if (transport->udp_encap_port) {
		e = pgm_parse_udp_encap(transport->rx_buffer, len, (struct sockaddr*)&dst_addr, &dst_addr_len, &pgm_header, &packet, &packet_len);
	} else {
		e = pgm_parse_raw(transport->rx_buffer, len, (struct sockaddr*)&dst_addr, &dst_addr_len, &pgm_header, &packet, &packet_len);
	}

	if (e < 0)
	{
/* TODO: difference between PGM_PC_SOURCE_CKSUM_ERRORS & PGM_PC_RECEIVER_CKSUM_ERRORS */
		if (e == -2)
			transport->cumulative_stats[PGM_PC_SOURCE_CKSUM_ERRORS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto check_for_repeat;
	}

/* calculate senders TSI */
	pgm_tsi_t tsi;
	memcpy (&tsi.gsi, pgm_header->pgm_gsi, sizeof(pgm_gsi_t));
	tsi.sport = pgm_header->pgm_sport;

//	g_trace ("INFO","tsi %s", pgm_print_tsi (&tsi));
	pgm_peer_t* source = NULL;

	if (pgm_is_upstream (pgm_header->pgm_type) || pgm_is_peer (pgm_header->pgm_type))
	{

/* upstream = receiver to source, peer-to-peer = receive to receiver
 *
 * NB: SPMRs can be upstream or peer-to-peer, if the packet is multicast then its
 *     a peer-to-peer message, if its unicast its an upstream message.
 */

		if (pgm_header->pgm_sport != transport->dport)
		{

/* its upstream/peer-to-peer for another session */

			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto check_for_repeat;
		}

		if ( pgm_is_peer (pgm_header->pgm_type)
			&& pgm_sockaddr_is_addr_multicast ((struct sockaddr*)&dst_addr) )
		{

/* its a multicast peer-to-peer message */

			if ( pgm_header->pgm_dport == transport->tsi.sport )
			{

/* we are the source, propagate null as the source */

				source = NULL;

				if (!transport->can_send)
				{
					transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
					goto check_for_repeat;
				}
			}
			else
			{
/* we are not the source */

				if (!transport->can_recv)
				{
					transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
					goto check_for_repeat;
				}

/* check to see the source this peer-to-peer message is about is in our peer list */

				pgm_tsi_t source_tsi;
				memcpy (&source_tsi.gsi, &tsi.gsi, sizeof(pgm_gsi_t));
				source_tsi.sport = pgm_header->pgm_dport;

				g_static_rw_lock_reader_lock (&transport->peers_lock);
				source = g_hash_table_lookup (transport->peers_hashtable, &source_tsi);
				g_static_rw_lock_reader_unlock (&transport->peers_lock);
				if (source == NULL)
				{

/* this source is unknown, we don't care about messages about it */

					transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
					goto check_for_repeat;
				}
			}
		}
		else if ( pgm_is_upstream (pgm_header->pgm_type)
			&& !pgm_sockaddr_is_addr_multicast ((struct sockaddr*)&dst_addr)
			&& ( pgm_header->pgm_dport == transport->tsi.sport ) )
		{

/* unicast upstream message, note that dport & sport are reversed */

			source = NULL;

			if (!transport->can_send)
			{
				transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
				goto check_for_repeat;
			}
		}
		else
		{

/* it is a mystery! */

			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto check_for_repeat;
		}

		gpointer pgm_data = pgm_header + 1;
		gsize pgm_len = packet_len - sizeof(pgm_header);

		switch (pgm_header->pgm_type) {
		case PGM_NAK:
			if (source) {
				on_peer_nak (source, pgm_header, pgm_data, pgm_len);
			} else if (!pgm_sockaddr_is_addr_multicast ((struct sockaddr*)&dst_addr)) {
				on_nak (transport, pgm_header, pgm_data, pgm_len);
				goto check_for_repeat;
			} else {
/* ignore multicast NAKs as the source */
				transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
				goto check_for_repeat;
			}
			break;

		case PGM_NNAK:	on_nnak (transport, pgm_header, pgm_data, pgm_len); break;
		case PGM_SPMR:	on_spmr (transport, source, pgm_header, pgm_data, pgm_len); break;
		case PGM_POLR:
		default:
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto check_for_repeat;
		}
	}
	else
	{

/* downstream = source to receivers */

		if (!pgm_is_downstream (pgm_header->pgm_type))
		{
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto check_for_repeat;
		}

/* pgm packet DPORT contains our transport DPORT */
		if (pgm_header->pgm_dport != transport->dport)
		{
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto check_for_repeat;
		}

		if (!transport->can_recv)
		{
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto check_for_repeat;
		}

/* search for TSI peer context or create a new one */
		g_static_rw_lock_reader_lock (&transport->peers_lock);
		source = g_hash_table_lookup (transport->peers_hashtable, &tsi);
		g_static_rw_lock_reader_unlock (&transport->peers_lock);
		if (source == NULL)
		{
			source = new_peer (transport, &tsi, (struct sockaddr*)&src_addr, src_addr_len);
		}

		source->cumulative_stats[PGM_PC_RECEIVER_BYTES_RECEIVED] += len;
		source->last_packet = pgm_time_now;

		gpointer pgm_data = pgm_header + 1;
		gsize pgm_len = packet_len - sizeof(pgm_header);

/* handle PGM packet type */
		switch (pgm_header->pgm_type) {
		case PGM_ODATA:	on_odata (source, pgm_header, pgm_data, pgm_len); break;
		case PGM_NCF:	on_ncf (source, pgm_header, pgm_data, pgm_len); break;
		case PGM_RDATA: on_rdata (source, pgm_header, pgm_data, pgm_len); break;
		case PGM_SPM:
			on_spm (source, pgm_header, pgm_data, pgm_len);

/* update group NLA if appropriate */
			if (pgm_sockaddr_is_addr_multicast ((struct sockaddr*)&dst_addr)) {
				memcpy (&source->group_nla, &dst_addr, dst_addr_len);
			}
			break;
		default:
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto check_for_repeat;
		}

	} /* downstream message */

/* check whether source has waiting data */
	if (source && ((pgm_rxw_t*)source->rxw)->is_waiting && !((pgm_rxw_t*)source->rxw)->waiting_link.data)
	{
		g_static_mutex_lock (&transport->waiting_mutex);
		((pgm_rxw_t*)source->rxw)->waiting_link.data = source->rxw;
		((pgm_rxw_t*)source->rxw)->waiting_link.next = transport->peers_waiting;
		transport->peers_waiting = &((pgm_rxw_t*)source->rxw)->waiting_link;
		goto flush_locked;	/* :D */
	}

	if (transport->peers_waiting)
	{
flush_waiting:
/* flush any congtiguous packets generated by the receipt of this packet */
		g_static_mutex_lock (&transport->waiting_mutex);
flush_locked:
		while (transport->peers_waiting)
		{
			pgm_rxw_t* waiting_rxw = transport->peers_waiting->data;
			gsize peer_bytes_read = pgm_rxw_readv (waiting_rxw, &pmsg, msg_end - pmsg, &piov, iov_end - piov);

/* clean up completed transmission groups */
			pgm_rxw_free_committed (waiting_rxw);

/* add to release list */
			waiting_rxw->commit_link.data = waiting_rxw;
			waiting_rxw->commit_link.next = transport->peers_committed;
			transport->peers_committed = &waiting_rxw->commit_link;

			if (peer_bytes_read) {
				bytes_read += peer_bytes_read;

				if (pmsg == msg_end || piov == iov_end)
				{
					transport->peers_last_waiting = transport->peers_waiting;
					g_static_mutex_unlock (&transport->waiting_mutex);
					goto out;
				}
			}
 
/* next */
			transport->peers_waiting->data = NULL;
			transport->peers_waiting->next = NULL;
			transport->peers_waiting = transport->peers_waiting->next;
		}

		transport->peers_last_waiting = transport->peers_waiting;
		g_static_mutex_unlock (&transport->waiting_mutex);
	}

check_for_repeat:
/* repeat if non-blocking and not full */
	if (flags & MSG_DONTWAIT)
	{
		if (len > 0 && pmsg < msg_end &&
			( ( bytes_read == 0 && msg_len == 1 ) ||	/* leave early with one apdu */
			( msg_len > 1 ) )				/* or wait for vector to fill up */
		)
		{
			goto recv_again;		/* \:D/ */
		}
	}
	else
	{
/* repeat if blocking and empty, i.e. received non data packet.
 */
		if (bytes_read == 0)
		{
			int fds = 0;
			fd_set readfds;
			FD_ZERO(&readfds);
			pgm_transport_select_info (transport, &readfds, NULL, &fds);
			fds = select (fds, &readfds, NULL, NULL, NULL);
			if (fds == -1) {
				return fds;
			}
			goto recv_again;
		}
	}

out:
	if (bytes_read == 0) {
		errno = EAGAIN;
		return -1;
	}

	return bytes_read;
}

/* read one contiguous apdu and return as a IO scatter/gather array.  msgv is owned by
 * the caller, tpdu contents are owned by the receive window.
 *
 * on success, returns the number of bytes read.  on error, -1 is returned, and
 * errno is set appropriately.
 */

gssize
pgm_transport_recvmsg (
	pgm_transport_t*	transport,
	pgm_msgv_t*		msgv,
	int			flags		/* MSG_DONTWAIT for non-blocking */
	)
{
	return pgm_transport_recvmsgv (transport, msgv, 1, flags);
}

/* vanilla read function.  copies from the receive window to the provided buffer
 * location.  the caller must provide an adequately sized buffer to store the largest
 * expected apdu or else it will be truncated.
 *
 * on success, returns the number of bytes read.  on error, -1 is returned, and
 * errno is set appropriately.
 */

gssize
pgm_transport_recv (
	pgm_transport_t*	transport,
	gpointer		data,
	gsize			len,
	int			flags		/* MSG_DONTWAIT for non-blocking */
	)
{
	pgm_msgv_t msgv;

	gssize bytes_read = pgm_transport_recvmsg (transport, &msgv, flags);

/* merge apdu packets together */
	if (bytes_read > 0) {
		gssize bytes_copied = 0;
		struct iovec* p = msgv.msgv_iov;

		do {
			size_t src_bytes = p->iov_len;
			g_assert (src_bytes > 0);

			if (bytes_copied + src_bytes > len) {
				g_error ("APDU truncated as provided buffer too small %" G_GSIZE_FORMAT " > %" G_GSIZE_FORMAT, bytes_read, len);
				src_bytes = len - bytes_copied;
				bytes_read = bytes_copied + src_bytes;
			}

			memcpy (data, p->iov_base, src_bytes);

			data = (char*)data + src_bytes;
			bytes_copied += src_bytes;
			p++;

		} while (bytes_copied < bytes_read);
	}

	return bytes_read;
}

/* add select parameters for the transports receive socket(s)
 *
 * returns highest file descriptor used plus one.
 */

int
pgm_transport_select_info (
	pgm_transport_t*	transport,
	fd_set*			readfds,
	fd_set*			writefds,
	int*			n_fds		/* in: max fds, out: max (in:fds, transport:fds) */
	)
{
	g_assert (transport);
	g_assert (n_fds);

	int fds = 0;

	if (readfds)
	{
		FD_SET(transport->recv_sock, readfds);
		FD_SET(transport->waiting_pipe[0], readfds);

		fds = MAX(transport->recv_sock, transport->waiting_pipe[0]) + 1;
	}

	if (transport->can_send && writefds)
	{
		FD_SET(transport->send_sock, writefds);

		fds = MAX(transport->send_sock + 1, fds);
	}

	return *n_fds = MAX(fds, *n_fds);
}

/* add poll parameters for this transports receive socket(s)
 *
 * returns number of pollfd structures filled.
 */

int
pgm_transport_poll_info (
	pgm_transport_t*	transport,
	struct pollfd*		fds,
	int*			n_fds,		/* in: #fds, out: used #fds */
	int			events		/* EPOLLIN, EPOLLOUT */
	)
{
	g_assert (transport);
	g_assert (fds);
	g_assert (n_fds);

	int moo = 0;

/* we currently only support one incoming socket */
	if (events & EPOLLIN)
	{
		g_assert ( (*n_fds - 2) >= 0 );
		fds[moo].fd = transport->recv_sock;
		fds[moo].events = POLLIN;
		moo++;
#ifdef TRANSPORT_DEBUG
		*n_fds--;
#endif
		fds[moo].fd = transport->waiting_pipe[0];
		fds[moo].events = POLLIN;
		moo++;
#ifdef TRANSPORT_DEBUG
		*n_fds--;
#endif
	}

/* ODATA only published on regular socket, no need to poll router-alert sock */
	if (transport->can_send && events & EPOLLOUT)
	{
		g_assert ( (*n_fds - 1) >= 0 );
		fds[moo].fd = transport->send_sock;
		fds[moo].events = POLLOUT;
		moo++;
#ifdef TRANSPORT_DEBUG
		*n_fds--;
#endif
	}

	return *n_fds = moo;
}

/* add epoll parameters for this transports recieve socket(s), events should
 * be set to EPOLLIN to wait for incoming events (data), and EPOLLOUT to wait
 * for non-blocking write.
 *
 * returns 0 on success, -1 on failure and sets errno appropriately.
 */

int
pgm_transport_epoll_ctl (
	pgm_transport_t*	transport,
	int			epfd,
	int			op,		/* EPOLL_CTL_ADD, ... */
	int			events		/* EPOLLIN, EPOLLOUT */
	)
{
	int retval = 0;

	if (op != EPOLL_CTL_ADD) {	/* only add currently supported */
		errno = EINVAL;
		retval = -1;
		goto out;
	}

	struct epoll_event event;

	if (events & EPOLLIN)
	{
		event.events = EPOLLIN | EPOLLET;
		event.data.ptr = transport;
		retval = epoll_ctl (epfd, op, transport->recv_sock, &event);
		if (retval) {
			goto out;
		}

		event.events = EPOLLIN | EPOLLET;
		event.data.ptr = transport;
		retval = epoll_ctl (epfd, op, transport->waiting_pipe[0], &event);
		if (retval) {
			goto out;
		}
	}

	if (transport->can_send && events & EPOLLOUT)
	{
		event.events = EPOLLOUT | EPOLLET;
		event.data.ptr = transport;
		retval = epoll_ctl (epfd, op, transport->send_sock, &event);
	}
out:
	return retval;
}

/* prototype of function to send pro-active parity NAKs.
 */
static int
pgm_schedule_proactive_nak (
	pgm_transport_t*	transport,
	guint32			sqn
	)
{
	int retval = 0;

	pgm_txw_retransmit_push (transport->txw, sqn, TRUE, transport->tg_sqn_shift);
	const char one = '1';
	if (1 != write (transport->rdata_pipe[1], &one, sizeof(one))) {
		g_critical ("write to rdata pipe failed :(");
		retval = -EINVAL;
	}
	return retval;
}

/* a deferred request for RDATA, now processing in the timer thread, we check the transmit
 * window to see if the packet exists and forward on, maintaining a lock until the queue is
 * empty.
 *
 * returns TRUE to keep monitoring the event source.
 */

static gboolean
on_nak_pipe (
	G_GNUC_UNUSED GIOChannel*	source,
	G_GNUC_UNUSED GIOCondition	condition,
	gpointer			data
	)
{
	pgm_transport_t* transport = data;

/* remove one event from pipe */
	char buf;
	read (transport->rdata_pipe[0], &buf, sizeof(buf));

/* We can flush queue and block all odata, or process one set, or process each
 * sequence number individually.
 */
	guint32		r_sqn;
	gpointer	r_packet = NULL;
	guint16		r_length = 0;
	gboolean	is_parity = FALSE;
	guint		rs_h = 0;
	guint		rs_2t = transport->rs_n - transport->rs_k;

/* parity packets are re-numbered across the transmission group with index h, sharing the space
 * with the original packets.  beyond the transmission group size (k), the PGM option OPT_PARITY_GRP
 * provides the extra offset value.
 */

/* TODO: peek instead of pop, calculate parity outside of lock, pop after sending RDATA to stop accrual
 * of NAKs on same numbers
 */
	g_static_rw_lock_reader_lock (&transport->txw_lock);
	if (!pgm_txw_retransmit_try_pop (transport->txw, &r_sqn, &r_packet, &r_length, &is_parity, &rs_h, rs_2t))
	{
		gboolean is_var_pktlen = FALSE;

/* calculate parity packet */
		if (is_parity)
		{
			guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
			guint32 tg_sqn = r_sqn & tg_sqn_mask;

			gboolean is_op_encoded = FALSE;

			guint16 parity_length = 0;
			guint8* src[ transport->rs_k ];
			for (unsigned i = 0; i < transport->rs_k; i++)
			{
				gpointer	o_packet;
				guint16		o_length;

				pgm_txw_peek (transport->txw, tg_sqn + i, &o_packet, &o_length);

				struct pgm_header*	o_header = o_packet;
				guint16			o_tsdu_length = g_ntohs (o_header->pgm_tsdu_length);

				if (!parity_length)
				{
					parity_length = o_tsdu_length;
				}
				else if (o_tsdu_length != parity_length)
				{
					is_var_pktlen = TRUE;

					if (o_tsdu_length > parity_length)
						parity_length = o_tsdu_length;
				}

				struct pgm_data* odata = (struct pgm_data*)(o_header + 1);

				if (o_header->pgm_options & PGM_OPT_PRESENT)
				{
					guint16 opt_total_length = g_ntohs(*(guint16*)( (char*)( odata + 1 ) + sizeof(guint16)));
					src[i] = (guint8*)(odata + 1) + opt_total_length;
					is_op_encoded = TRUE;
				}
				else
				{
					src[i] = (guint8*)(odata + 1);
				}
			}

/* construct basic PGM header to be completed by send_rdata() */
			struct pgm_header*	r_header = (struct pgm_header*)transport->parity_buffer;
			struct pgm_data*	rdata  = (struct pgm_data*)(r_header + 1);
			memcpy (r_header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
			r_header->pgm_options = PGM_OPT_PARITY;

/* append actual TSDU length if variable length packets, zero pad as necessary.
 */
			if (is_var_pktlen)
			{
				r_header->pgm_options |= PGM_OPT_VAR_PKTLEN;

				for (unsigned i = 0; i < transport->rs_k; i++)
				{
					gpointer	o_packet;
					guint16		o_length;

					pgm_txw_peek (transport->txw, tg_sqn + i, &o_packet, &o_length);

					struct pgm_header*	o_header = o_packet;
					guint16			o_tsdu_length = g_ntohs (o_header->pgm_tsdu_length);

					pgm_txw_zero_pad (transport->txw, o_packet, o_tsdu_length, parity_length);
					*(guint16*)((guint8*)o_packet + parity_length) = o_tsdu_length;
				}
				parity_length += 2;
			}

			r_header->pgm_tsdu_length = g_htons (parity_length);
			rdata->data_sqn		= g_htonl ( tg_sqn | rs_h );

			gpointer data_bytes	= rdata + 1;

			r_packet	= r_header;
			r_length	= sizeof(struct pgm_header) + sizeof(struct pgm_data) + parity_length;

/* encode every option separately, currently only one applies: opt_fragment
 */
			if (is_op_encoded)
			{
				r_header->pgm_options |= PGM_OPT_PRESENT;

				struct pgm_opt_fragment null_opt_fragment;
				guint8* opt_src[ transport->rs_k ];
				memset (&null_opt_fragment, 0, sizeof(null_opt_fragment));
				*(guint8*)&null_opt_fragment |= PGM_OP_ENCODED_NULL;
				for (unsigned i = 0; i < transport->rs_k; i++)
				{
					gpointer	o_packet;
					guint16		o_length;

					pgm_txw_peek (transport->txw, tg_sqn + i, &o_packet, &o_length);

					struct pgm_header*	o_header = o_packet;
					struct pgm_data*	odata = (struct pgm_data*)(o_header + 1);

					struct pgm_opt_fragment* opt_fragment;
					if ((o_header->pgm_options & PGM_OPT_PRESENT) && get_opt_fragment((gpointer)(odata + 1), &opt_fragment))
					{
/* skip three bytes of header */
						opt_src[i] = (guint8*)opt_fragment + sizeof (struct pgm_opt_header);
					}
					else
					{
						opt_src[i] = (guint8*)&null_opt_fragment;
					}
				}

/* add options to this rdata packet */
				struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(rdata + 1);
				opt_len->opt_type	= PGM_OPT_LENGTH;
				opt_len->opt_length	= sizeof(struct pgm_opt_length);
				opt_len->opt_total_length = g_htons (	sizeof(struct pgm_opt_length) +
									sizeof(struct pgm_opt_header) +
									sizeof(struct pgm_opt_fragment) );
				struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
				opt_header->opt_type	= PGM_OPT_FRAGMENT | PGM_OPT_END;
				opt_header->opt_length	= sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
				opt_header->opt_reserved = PGM_OP_ENCODED;
				struct pgm_opt_fragment* opt_fragment = (struct pgm_opt_fragment*)(opt_header + 1);

/* The cast below is the correct way to handle the problem. 
 * The (void *) cast is to avoid a GCC warning like: 
 *
 *   "warning: dereferencing type-punned pointer will break strict-aliasing rules"
 */
				pgm_rs_encode (transport->rs, (const void**)(void*)opt_src, transport->rs_k + rs_h, opt_fragment + sizeof(struct pgm_opt_header), sizeof(struct pgm_opt_fragment) - sizeof(struct pgm_opt_header));

				data_bytes = opt_fragment + 1;

				r_length += sizeof(struct pgm_opt_length) + sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
			}

/* encode payload */
			pgm_rs_encode (transport->rs, (const void**)(void*)src, transport->rs_k + rs_h, data_bytes, parity_length);
		}

		send_rdata (transport, r_sqn, r_packet, r_length);
	}
	g_static_rw_lock_reader_unlock (&transport->txw_lock);

	return TRUE;
}

/* prod to wakeup timer thread
 *
 * returns TRUE to keep monitoring the event source.
 */

static gboolean
on_timer_pipe (
	G_GNUC_UNUSED GIOChannel*	source,
	G_GNUC_UNUSED GIOCondition	condition,
	gpointer			data
	)
{
	pgm_transport_t* transport = data;

/* empty pipe */
	char buf;
	while (1 == read (transport->timer_pipe[0], &buf, sizeof(buf)));

	return TRUE;
}

/* SPM indicate start of a session, continued presence of a session, or flushing final packets
 * of a session.
 *
 * returns -EINVAL on invalid packet or duplicate SPM sequence number.
 */

static int
on_spm (
	pgm_peer_t*		sender,
	struct pgm_header*	header,
	gpointer		data,		/* data will be changed to host order on demand */
	gsize			len
	)
{
	int retval;

	if ((retval = pgm_verify_spm (header, data, len)) != 0)
	{
		sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_SPMS]++;
		sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		goto out;
	}

	struct pgm_transport_t* transport = sender->transport;
	struct pgm_spm* spm = (struct pgm_spm*)data;
	pgm_time_t now = pgm_time_update_now ();

	spm->spm_sqn = g_ntohl (spm->spm_sqn);

/* check for advancing sequence number, or first SPM */
	g_static_mutex_lock (&transport->mutex);
	g_static_mutex_lock (&sender->mutex);
	if ( pgm_uint32_gte (spm->spm_sqn, sender->spm_sqn)
		|| ( ((struct sockaddr*)&sender->nla)->sa_family == 0 ) )
	{
/* copy NLA for replies */
		pgm_nla_to_sockaddr (&spm->spm_nla_afi, (struct sockaddr*)&sender->nla);

/* save sequence number */
		sender->spm_sqn = spm->spm_sqn;

/* update receive window */
		pgm_time_t nak_rb_expiry = now + nak_rb_ivl(transport);
		guint naks = pgm_rxw_window_update (sender->rxw,
							g_ntohl (spm->spm_trail),
							g_ntohl (spm->spm_lead),
							transport->rs_k,
							transport->tg_sqn_shift,
							nak_rb_expiry);
		if (naks && pgm_time_after(transport->next_poll, nak_rb_expiry))
		{
			transport->next_poll = nak_rb_expiry;
			g_trace ("INFO","on_spm: prod timer thread");
			const char one = '1';
			if (1 != write (transport->timer_pipe[1], &one, sizeof(one))) {
				g_critical ("write to timer pipe failed :(");
				retval = -EINVAL;
			}
		}
	}
	else
	{	/* does not advance SPM sequence number */
		sender->cumulative_stats[PGM_PC_RECEIVER_DUP_SPMS]++;
		sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		retval = -EINVAL;
	}

/* check whether peer can generate parity packets */
	if (header->pgm_options & PGM_OPT_PRESENT)
	{
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(spm + 1);
		if (opt_len->opt_type != PGM_OPT_LENGTH)
		{
			retval = -EINVAL;
			sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_SPMS]++;
			sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			goto out;
		}
		if (opt_len->opt_length != sizeof(struct pgm_opt_length))
		{
			retval = -EINVAL;
			sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_SPMS]++;
			sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			goto out;
		}
/* TODO: check for > 16 options & past packet end */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(spm + 1);
		do {
			opt_header = (struct pgm_opt_header*)((char*)opt_header + opt_header->opt_length);

			if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_PARITY_PRM)
			{
				struct pgm_opt_parity_prm* opt_parity_prm = (struct pgm_opt_parity_prm*)(opt_header + 1);

				if ((opt_parity_prm->opt_reserved & PGM_PARITY_PRM_MASK) == 0)
				{
					retval = -EINVAL;
					sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_SPMS]++;
					sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
					goto out;
				}

				guint32 parity_prm_tgs = g_ntohl (opt_parity_prm->parity_prm_tgs);
				if (parity_prm_tgs < 2 || parity_prm_tgs > 128)
				{
					retval = -EINVAL;
					sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_SPMS]++;
					sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
					goto out;
				}
			
				sender->use_proactive_parity = opt_parity_prm->opt_reserved & PGM_PARITY_PRM_PRO;
				sender->use_ondemand_parity = opt_parity_prm->opt_reserved & PGM_PARITY_PRM_OND;
				sender->rs_k = parity_prm_tgs;
				sender->tg_sqn_shift = pgm_power2_log2 (sender->rs_k);
				break;
			}
		} while (!(opt_header->opt_type & PGM_OPT_END));
	}

/* either way bump expiration timer */
	sender->expiry = now + transport->peer_expiry;
	sender->spmr_expiry = 0;
	g_static_mutex_unlock (&sender->mutex);
	g_static_mutex_unlock (&transport->mutex);

out:
	return retval;
}

/* SPMR indicates if multicast to cancel own SPMR, or unicast to send SPM.
 *
 * rate limited to 1/IHB_MIN per TSI (13.4).
 *
 * if SPMR was valid, returns 0.
 */

static int
on_spmr (
	pgm_transport_t*	transport,
	pgm_peer_t*		peer,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_spmr()");

	int retval;

	if ((retval = pgm_verify_spmr (header, data, len)) == 0)
	{

/* we are the source */
		if (peer == NULL)
		{
			send_spm (transport);
		}
		else
		{
/* we are a peer */
			g_trace ("INFO", "suppressing SPMR due to peer multicast SPMR.");
			g_static_mutex_lock (&peer->mutex);
			peer->spmr_expiry = 0;
			g_static_mutex_unlock (&peer->mutex);
		}
	}
	else
	{
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
	}

	return retval;
}

/* NAK requesting RDATA transmission for a sending transport, only valid if
 * sequence number(s) still in transmission window.
 *
 * we can potentially have different IP versions for the NAK packet to the send group.
 *
 * TODO: fix IPv6 AFIs
 *
 * take in a NAK and pass off to an asynchronous queue for another thread to process
 *
 * if NAK is valid, returns 0.  on error, -EINVAL is returned.
 */

static int
on_nak (
	pgm_transport_t*	transport,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_nak()");

	gboolean is_parity = header->pgm_options & PGM_OPT_PARITY;

	if (is_parity) {
		transport->cumulative_stats[PGM_PC_SOURCE_PARITY_NAKS_RECEIVED]++;

		if (!transport->use_ondemand_parity) {
			transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
	} else {
		transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_NAKS_RECEIVED]++;
	}

	int retval;
	if ((retval = pgm_verify_nak (header, data, len)) != 0)
	{
		transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

	struct pgm_nak* nak = (struct pgm_nak*)data;
		
/* NAK_SRC_NLA contains our transport unicast NLA */
	struct sockaddr_storage nak_src_nla;
	pgm_nla_to_sockaddr (&nak->nak_src_nla_afi, (struct sockaddr*)&nak_src_nla);

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nak_src_nla, (struct sockaddr*)&transport->send_smr.smr_interface) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* NAK_GRP_NLA containers our transport multicast group */ 
	struct sockaddr_storage nak_grp_nla;
	switch (pgm_sockaddr_family(&nak_src_nla)) {
	case AF_INET:
		pgm_nla_to_sockaddr (&nak->nak_grp_nla_afi, (struct sockaddr*)&nak_grp_nla);
		break;

	case AF_INET6:
		pgm_nla_to_sockaddr (&((struct pgm_nak6*)nak)->nak6_grp_nla_afi, (struct sockaddr*)&nak_grp_nla);
		break;
	}

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nak_grp_nla, (struct sockaddr*)&transport->send_smr.smr_multiaddr) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* create queue object */
	pgm_sqn_list_t sqn_list;
	sqn_list.sqn[0] = g_ntohl (nak->nak_sqn);
	sqn_list.len = 1;

	g_trace ("INFO", "nak_sqn %" G_GUINT32_FORMAT, sqn_list.sqn[0]);

/* check NAK list */
	guint32* nak_list = NULL;
	guint nak_list_len = 0;
	if (header->pgm_options & PGM_OPT_PRESENT)
	{
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(nak + 1);
		if (opt_len->opt_type != PGM_OPT_LENGTH)
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
		if (opt_len->opt_length != sizeof(struct pgm_opt_length))
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_MALFORMED_NAKS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
/* TODO: check for > 16 options & past packet end */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(nak + 1);
		do {
			opt_header = (struct pgm_opt_header*)((char*)opt_header + opt_header->opt_length);

			if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_NAK_LIST)
			{
				nak_list = ((struct pgm_opt_nak_list*)(opt_header + 1))->opt_sqn;
				nak_list_len = ( opt_header->opt_length - sizeof(struct pgm_opt_header) - sizeof(guint8) ) / sizeof(guint32);
				break;
			}
		} while (!(opt_header->opt_type & PGM_OPT_END));
	}

/* nak list numbers */
#ifdef TRANSPORT_DEBUG
	if (nak_list)
	{
		char nak_sz[1024] = "";
		guint32 *nakp = nak_list, *nake = nak_list + nak_list_len;
		while (nakp < nake) {
			char tmp[1024];
			sprintf (tmp, "%" G_GUINT32_FORMAT " ", g_ntohl(*nakp));
			strcat (nak_sz, tmp);
			nakp++;
		}
	g_trace ("INFO", "nak list %s", nak_sz);
	}
#endif
	for (unsigned i = 0; i < nak_list_len; i++)
	{
		sqn_list.sqn[sqn_list.len++] = g_ntohl (*nak_list);
		nak_list++;
	}

/* send NAK confirm packet immediately, then defer to timer thread for a.s.a.p
 * delivery of the actual RDATA packets.
 */
	if (nak_list_len) {
		send_ncf_list (transport, (struct sockaddr*)&nak_src_nla, (struct sockaddr*)&nak_grp_nla, &sqn_list, is_parity);
	} else {
		send_ncf (transport, (struct sockaddr*)&nak_src_nla, (struct sockaddr*)&nak_grp_nla, sqn_list.sqn[0], is_parity);
	}

/* queue retransmit requests */
	for (unsigned i = 0; i < sqn_list.len; i++)
	{
		int cnt = pgm_txw_retransmit_push (transport->txw, sqn_list.sqn[i], is_parity, transport->tg_sqn_shift);
		if (cnt > 0)
		{
			const char one = '1';
			if (1 != write (transport->rdata_pipe[1], &one, sizeof(one))) {
				g_critical ("write to rdata pipe failed :(");
				retval = -EINVAL;
			}
		}
	}

out:
	return retval;
}

/* Multicast peer-to-peer NAK handling, pretty much the same as a NCF but different direction
 *
 * if NAK is valid, returns 0.  on error, -EINVAL is returned.
 */

static int
on_peer_nak (
	pgm_peer_t*		peer,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_peer_nak()");

	int retval;
	pgm_transport_t* transport = peer->transport;

	if ((retval = pgm_verify_nak (header, data, len)) != 0)
	{
		g_trace ("INFO", "Invalid NAK, ignoring.");
		peer->cumulative_stats[PGM_PC_RECEIVER_NAK_ERRORS]++;
		peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		goto out;
	}

	struct pgm_nak* nak = (struct pgm_nak*)data;
		
/* NAK_SRC_NLA must not contain our transport unicast NLA */
	struct sockaddr_storage nak_src_nla;
	pgm_nla_to_sockaddr (&nak->nak_src_nla_afi, (struct sockaddr*)&nak_src_nla);

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nak_src_nla, (struct sockaddr*)&transport->send_smr.smr_interface) == 0) {
		retval = -EINVAL;
		peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		goto out;
	}

/* NAK_GRP_NLA contains one of our transport receive multicast groups: the sources send multicast group */ 
	struct sockaddr_storage nak_grp_nla;
	switch (pgm_sockaddr_family(&nak_src_nla)) {
	case AF_INET:
		pgm_nla_to_sockaddr (&nak->nak_grp_nla_afi, (struct sockaddr*)&nak_grp_nla);
		break;

	case AF_INET6:
		pgm_nla_to_sockaddr (&((struct pgm_nak6*)nak)->nak6_grp_nla_afi, (struct sockaddr*)&nak_grp_nla);
		break;
	}

	gboolean found = FALSE;
	for (unsigned i = 0; i < transport->recv_smr_len; i++)
	{
		if (pgm_sockaddr_cmp ((struct sockaddr*)&nak_grp_nla, (struct sockaddr*)&transport->recv_smr[i].smr_multiaddr) == 0)
		{
			found = TRUE;
		}
	}

	if (!found) {
		g_trace ("INFO", "NAK not destined for this multicast group.");
		retval = -EINVAL;
		peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		goto out;
	}

	g_static_mutex_lock (&transport->mutex);
	g_static_mutex_lock (&peer->mutex);

/* handle as NCF */
	pgm_time_update_now();
	pgm_rxw_ncf (peer->rxw, g_ntohl (nak->nak_sqn), pgm_time_now + transport->nak_rdata_ivl, pgm_time_now + nak_rb_ivl(transport));

/* check NAK list */
	guint32* nak_list = NULL;
	guint nak_list_len = 0;
	if (header->pgm_options & PGM_OPT_PRESENT)
	{
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(nak + 1);
		if (opt_len->opt_type != PGM_OPT_LENGTH)
		{
			g_trace ("INFO", "First PGM Option in NAK incorrect, ignoring.");
			retval = -EINVAL;
			peer->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_NCFS]++;
			peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			goto out_unlock;
		}
		if (opt_len->opt_length != sizeof(struct pgm_opt_length))
		{
			g_trace ("INFO", "PGM Length Option has incorrect length, ignoring.");
			retval = -EINVAL;
			peer->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_NCFS]++;
			peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			goto out_unlock;
		}
/* TODO: check for > 16 options & past packet end */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(nak + 1);
		do {
			opt_header = (struct pgm_opt_header*)((char*)opt_header + opt_header->opt_length);

			if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_NAK_LIST)
			{
				nak_list = ((struct pgm_opt_nak_list*)(opt_header + 1))->opt_sqn;
				nak_list_len = ( opt_header->opt_length - sizeof(struct pgm_opt_header) - sizeof(guint8) ) / sizeof(guint32);
				break;
			}
		} while (!(opt_header->opt_type & PGM_OPT_END));
	}

	g_trace ("INFO", "NAK contains 1+%i sequence numbers.", nak_list_len);
	while (nak_list_len)
	{
		pgm_rxw_ncf (peer->rxw, g_ntohl (*nak_list), pgm_time_now + transport->nak_rdata_ivl, pgm_time_now + nak_rb_ivl(transport));
		nak_list++;
		nak_list_len--;
	}

out_unlock:
	g_static_mutex_unlock (&peer->mutex);
	g_static_mutex_unlock (&transport->mutex);

out:
	return retval;
}

/* NCF confirming receipt of a NAK from this transport or another on the LAN segment.
 *
 * Packet contents will match exactly the sent NAK, although not really that helpful.
 *
 * if NCF is valid, returns 0.  on error, -EINVAL is returned.
 */

static int
on_ncf (
	pgm_peer_t*		peer,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_ncf()");

	int retval;
	pgm_transport_t* transport = peer->transport;

	if ((retval = pgm_verify_ncf (header, data, len)) != 0)
	{
		g_trace ("INFO", "Invalid NCF, ignoring.");
		peer->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_NCFS]++;
		peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		goto out;
	}

	struct pgm_nak* ncf = (struct pgm_nak*)data;
		
/* NCF_SRC_NLA may contain our transport unicast NLA, we don't really care */
	struct sockaddr_storage ncf_src_nla;
	pgm_nla_to_sockaddr (&ncf->nak_src_nla_afi, (struct sockaddr*)&ncf_src_nla);

#if 0
	if (pgm_sockaddr_cmp ((struct sockaddr*)&ncf_src_nla, (struct sockaddr*)&transport->send_smr.smr_interface) != 0) {
		retval = -EINVAL;
		peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		goto out;
	}
#endif

/* NCF_GRP_NLA contains our transport multicast group */ 
	struct sockaddr_storage ncf_grp_nla;
	switch (pgm_sockaddr_family(&ncf_src_nla)) {
	case AF_INET:
		pgm_nla_to_sockaddr (&ncf->nak_grp_nla_afi, (struct sockaddr*)&ncf_grp_nla);
		break;

	case AF_INET6:
		pgm_nla_to_sockaddr (&((struct pgm_nak6*)ncf)->nak6_grp_nla_afi, (struct sockaddr*)&ncf_grp_nla);
		break;
	}

	if (pgm_sockaddr_cmp ((struct sockaddr*)&ncf_grp_nla, (struct sockaddr*)&transport->send_smr.smr_multiaddr) != 0) {
		g_trace ("INFO", "NCF not destined for this multicast group.");
		retval = -EINVAL;
		peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		goto out;
	}

	g_static_mutex_lock (&transport->mutex);
	g_static_mutex_lock (&peer->mutex);

	pgm_time_update_now();
	pgm_rxw_ncf (peer->rxw, g_ntohl (ncf->nak_sqn), pgm_time_now + transport->nak_rdata_ivl, pgm_time_now + nak_rb_ivl(transport));

/* check NCF list */
	guint32* ncf_list = NULL;
	guint ncf_list_len = 0;
	if (header->pgm_options & PGM_OPT_PRESENT)
	{
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(ncf + 1);
		if (opt_len->opt_type != PGM_OPT_LENGTH)
		{
			g_trace ("INFO", "First PGM Option in NCF incorrect, ignoring.");
			retval = -EINVAL;
			peer->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_NCFS]++;
			peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			goto out_unlock;
		}
		if (opt_len->opt_length != sizeof(struct pgm_opt_length))
		{
			g_trace ("INFO", "PGM Length Option has incorrect length, ignoring.");
			retval = -EINVAL;
			peer->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_NCFS]++;
			peer->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			goto out_unlock;
		}
/* TODO: check for > 16 options & past packet end */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(ncf + 1);
		do {
			opt_header = (struct pgm_opt_header*)((char*)opt_header + opt_header->opt_length);

			if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_NAK_LIST)
			{
				ncf_list = ((struct pgm_opt_nak_list*)(opt_header + 1))->opt_sqn;
				ncf_list_len = ( opt_header->opt_length - sizeof(struct pgm_opt_header) - sizeof(guint8) ) / sizeof(guint32);
				break;
			}
		} while (!(opt_header->opt_type & PGM_OPT_END));
	}

	g_trace ("INFO", "NCF contains 1+%i sequence numbers.", ncf_list_len);
	while (ncf_list_len)
	{
		pgm_rxw_ncf (peer->rxw, g_ntohl (*ncf_list), pgm_time_now + transport->nak_rdata_ivl, pgm_time_now + nak_rb_ivl(transport));
		ncf_list++;
		ncf_list_len--;
	}

out_unlock:
	g_static_mutex_unlock (&peer->mutex);
	g_static_mutex_unlock (&transport->mutex);

out:
	return retval;
}

/* Null-NAK, or N-NAK propogated by a DLR for hand waving excitement
 *
 * if NNAK is valid, returns 0.  on error, -EINVAL is returned.
 */

static int
on_nnak (
	pgm_transport_t*	transport,
	struct pgm_header*	header,
	gpointer		data,
	gsize			len
	)
{
	g_trace ("INFO","on_nnak()");
	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_NNAK_PACKETS_RECEIVED]++;

	int retval;
	if ((retval = pgm_verify_nnak (header, data, len)) != 0)
	{
		transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

	struct pgm_nak* nnak = (struct pgm_nak*)data;
		
/* NAK_SRC_NLA contains our transport unicast NLA */
	struct sockaddr_storage nnak_src_nla;
	pgm_nla_to_sockaddr (&nnak->nak_src_nla_afi, (struct sockaddr*)&nnak_src_nla);

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nnak_src_nla, (struct sockaddr*)&transport->send_smr.smr_interface) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* NAK_GRP_NLA containers our transport multicast group */ 
	struct sockaddr_storage nnak_grp_nla;
	switch (pgm_sockaddr_family(&nnak_src_nla)) {
	case AF_INET:
		pgm_nla_to_sockaddr (&nnak->nak_grp_nla_afi, (struct sockaddr*)&nnak_grp_nla);
		break;

	case AF_INET6:
		pgm_nla_to_sockaddr (&((struct pgm_nak6*)nnak)->nak6_grp_nla_afi, (struct sockaddr*)&nnak_grp_nla);
		break;
	}

	if (pgm_sockaddr_cmp ((struct sockaddr*)&nnak_grp_nla, (struct sockaddr*)&transport->send_smr.smr_multiaddr) != 0) {
		retval = -EINVAL;
		transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
		transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
		goto out;
	}

/* check NNAK list */
	guint nnak_list_len = 0;
	if (header->pgm_options & PGM_OPT_PRESENT)
	{
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(nnak + 1);
		if (opt_len->opt_type != PGM_OPT_LENGTH)
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
		if (opt_len->opt_length != sizeof(struct pgm_opt_length))
		{
			retval = -EINVAL;
			transport->cumulative_stats[PGM_PC_SOURCE_NNAK_ERRORS]++;
			transport->cumulative_stats[PGM_PC_SOURCE_PACKETS_DISCARDED]++;
			goto out;
		}
/* TODO: check for > 16 options & past packet end */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(nnak + 1);
		do {
			opt_header = (struct pgm_opt_header*)((char*)opt_header + opt_header->opt_length);

			if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_NAK_LIST)
			{
				nnak_list_len = ( opt_header->opt_length - sizeof(struct pgm_opt_header) - sizeof(guint8) ) / sizeof(guint32);
				break;
			}
		} while (!(opt_header->opt_type & PGM_OPT_END));
	}

	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_NNAKS_RECEIVED] += 1 + nnak_list_len;

out:
	return retval;
}

/* ambient/heartbeat SPM's
 *
 * heartbeat: ihb_tmr decaying between ihb_min and ihb_max 2x after last packet
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static inline int
send_spm (
	pgm_transport_t*	transport
	)
{
	g_static_mutex_lock (&transport->mutex);
	int result = send_spm_unlocked (transport);
	g_static_mutex_unlock (&transport->mutex);
	return result;
}

static int
send_spm_unlocked (
	pgm_transport_t*	transport
	)
{
	g_trace ("SPM","send_spm");

/* recycles a transport global packet */
	struct pgm_header *header = (struct pgm_header*)transport->spm_packet;
	struct pgm_spm *spm = (struct pgm_spm*)(header + 1);

	spm->spm_sqn		= g_htonl (transport->spm_sqn++);
	g_static_rw_lock_reader_lock (&transport->txw_lock);
	spm->spm_trail		= g_htonl (pgm_txw_trail(transport->txw));
	spm->spm_lead		= g_htonl (pgm_txw_lead(transport->txw));
	g_static_rw_lock_reader_unlock (&transport->txw_lock);

/* checksum optional for SPMs */
	header->pgm_checksum	= 0;
	header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, transport->spm_len, 0));

	gssize sent = pgm_sendto (transport,
				TRUE,				/* rate limited */
				TRUE,				/* with router alert */
				header,
				transport->spm_len,
				MSG_CONFIRM,			/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

	if ( sent != (gssize)transport->spm_len )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += transport->spm_len;
	return 0;
}

/* send SPM-request to a new peer, this packet type has no contents
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static int
send_spmr (
	pgm_peer_t*	peer
	)
{
	g_trace ("INFO","send_spmr");

	pgm_transport_t* transport = peer->transport;

/* cache peer information */
	guint16	peer_sport = peer->tsi.sport;
	struct sockaddr_storage peer_nla;
	memcpy (&peer_nla, &peer->local_nla, sizeof(struct sockaddr_storage));

	gsize tpdu_length = sizeof(struct pgm_header);
	guint8 buf[ tpdu_length ];
	struct pgm_header *header = (struct pgm_header*)buf;
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
/* dport & sport reversed communicating upstream */
	header->pgm_sport	= transport->dport;
	header->pgm_dport	= peer_sport;
	header->pgm_type	= PGM_SPMR;
	header->pgm_options	= 0;
	header->pgm_tsdu_length	= 0;
	header->pgm_checksum	= 0;
	header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

/* send multicast SPMR TTL 1 */
	g_trace ("INFO", "send multicast SPMR to %s", inet_ntoa( ((struct sockaddr_in*)&transport->send_smr.smr_multiaddr)->sin_addr ));
	pgm_sockaddr_multicast_hops (transport->send_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), 1);
	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				FALSE,			/* regular socket */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* send unicast SPMR with regular TTL */
	g_trace ("INFO", "send unicast SPMR to %s", inet_ntoa( ((struct sockaddr_in*)&peer->local_nla)->sin_addr ));
	pgm_sockaddr_multicast_hops (transport->send_sock, pgm_sockaddr_family(&transport->send_smr.smr_interface), transport->hops);
	sent += pgm_sendto (transport,
				FALSE,
				FALSE,
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&peer_nla,
				pgm_sockaddr_len(&peer_nla));

	peer->spmr_expiry = 0;

	if ( sent != (gssize)(tpdu_length * 2) ) 
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length * 2;

	return 0;
}

/* send selective NAK for one sequence number.
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */
static int
send_nak (
	pgm_peer_t*		peer,
	guint32			sequence_number
	)
{
	g_trace ("INFO", "send_nak(%" G_GUINT32_FORMAT ")", sequence_number);

	pgm_transport_t* transport = peer->transport;
	guint8 buf[ sizeof(struct pgm_header) + sizeof(struct pgm_nak) ];
	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_nak);

	struct pgm_header *header = (struct pgm_header*)buf;
	struct pgm_nak *nak = (struct pgm_nak*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));

	guint16	peer_sport = peer->tsi.sport;
	struct sockaddr_storage peer_nla;
	memcpy (&peer_nla, &peer->nla, sizeof(struct sockaddr_storage));

/* dport & sport swap over for a nak */
	header->pgm_sport	= transport->dport;
	header->pgm_dport	= peer_sport;
	header->pgm_type        = PGM_NAK;
        header->pgm_options     = 0;
        header->pgm_tsdu_length = 0;

/* NAK */
	nak->nak_sqn		= g_htonl (sequence_number);

/* source nla */
	pgm_sockaddr_to_nla ((struct sockaddr*)&peer_nla, (char*)&nak->nak_src_nla_afi);

/* group nla: we match the NAK NLA to the same as advertised by the source, we might
 * be listening to multiple multicast groups
 */
	pgm_sockaddr_to_nla ((struct sockaddr*)&peer->group_nla, (char*)&nak->nak_grp_nla_afi);

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				TRUE,			/* with router alert */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&peer_nla,
				pgm_sockaddr_len(&peer_nla));

	if ( sent != (gssize)tpdu_length )
	{
		return -1;
	}

	peer->cumulative_stats[PGM_PC_RECEIVER_SELECTIVE_NAK_PACKETS_SENT]++;
	peer->cumulative_stats[PGM_PC_RECEIVER_SELECTIVE_NAKS_SENT]++;

	return 0;
}

/* send a NAK confirm (NCF) message with provided sequence number list.
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

static int
send_ncf (
	pgm_transport_t*	transport,
	struct sockaddr*	nak_src_nla,
	struct sockaddr*	nak_grp_nla,
	guint32			sequence_number,
	gboolean		is_parity		/* send parity NCF */
	)
{
	g_trace ("INFO", "send_ncf()");

	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_nak);
	guint8 buf[ tpdu_length ];

	struct pgm_header *header = (struct pgm_header*)buf;
	struct pgm_nak *ncf = (struct pgm_nak*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	
	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;
	header->pgm_type        = PGM_NCF;
        header->pgm_options     = is_parity ? PGM_OPT_PARITY : 0;
        header->pgm_tsdu_length = 0;

/* NCF */
	ncf->nak_sqn		= g_htonl (sequence_number);

/* source nla */
	pgm_sockaddr_to_nla (nak_src_nla, (char*)&ncf->nak_src_nla_afi);

/* group nla */
	pgm_sockaddr_to_nla (nak_grp_nla, (char*)&ncf->nak_grp_nla_afi);

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				TRUE,			/* with router alert */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

	if ( sent != (gssize)tpdu_length )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length;

	return 0;
}

/* Send a parity NAK requesting on-demand parity packet generation.
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */
static int
send_parity_nak (
	pgm_peer_t*		peer,
	guint32			nak_tg_sqn,	/* transmission group (shifted) */
	guint32			nak_pkt_cnt	/* count of parity packets to request */
	)
{
	g_trace ("INFO", "send_parity_nak(%u, %u)", nak_tg_sqn, nak_pkt_cnt);

	pgm_transport_t* transport = peer->transport;
	guint8 buf[ sizeof(struct pgm_header) + sizeof(struct pgm_nak) ];
	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_nak);

	struct pgm_header *header = (struct pgm_header*)buf;
	struct pgm_nak *nak = (struct pgm_nak*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));

	guint16	peer_sport = peer->tsi.sport;
	struct sockaddr_storage peer_nla;
	memcpy (&peer_nla, &peer->nla, sizeof(struct sockaddr_storage));

/* dport & sport swap over for a nak */
	header->pgm_sport	= transport->dport;
	header->pgm_dport	= peer_sport;
	header->pgm_type        = PGM_NAK;
        header->pgm_options     = PGM_OPT_PARITY;	/* this is a parity packet */
        header->pgm_tsdu_length = 0;

/* NAK */
	nak->nak_sqn		= g_htonl (nak_tg_sqn | (nak_pkt_cnt - 1) );

/* source nla */
	pgm_sockaddr_to_nla ((struct sockaddr*)&peer_nla, (char*)&nak->nak_src_nla_afi);

/* group nla: we match the NAK NLA to the same as advertised by the source, we might
 * be listening to multiple multicast groups
 */
	pgm_sockaddr_to_nla ((struct sockaddr*)&peer->group_nla, (char*)&nak->nak_grp_nla_afi);

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				TRUE,			/* with router alert */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&peer_nla,
				pgm_sockaddr_len(&peer_nla));

	if ( sent != (gssize)tpdu_length )
	{
		return -1;
	}

	peer->cumulative_stats[PGM_PC_RECEIVER_PARITY_NAK_PACKETS_SENT]++;
	peer->cumulative_stats[PGM_PC_RECEIVER_PARITY_NAKS_SENT]++;

	return 0;
}

/* A NAK packet with a OPT_NAK_LIST option extension
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */

#ifndef PGM_SINGLE_NAK
static int
send_nak_list (
	pgm_peer_t*	peer,
	pgm_sqn_list_t*	sqn_list
	)
{
	g_assert (sqn_list->len > 1);
	g_assert (sqn_list->len <= 63);

	pgm_transport_t* transport = peer->transport;
	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_nak)
			+ sizeof(struct pgm_opt_length)		/* includes header */
			+ sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_nak_list)
			+ ( (sqn_list->len-1) * sizeof(guint32) );
	guint8 buf[ tpdu_length ];

	struct pgm_header *header = (struct pgm_header*)buf;
	struct pgm_nak *nak = (struct pgm_nak*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));

	guint16	peer_sport = peer->tsi.sport;
	struct sockaddr_storage peer_nla;
	memcpy (&peer_nla, &peer->nla, sizeof(struct sockaddr_storage));

/* dport & sport swap over for a nak */
	header->pgm_sport	= transport->dport;
	header->pgm_dport	= peer_sport;
	header->pgm_type        = PGM_NAK;
        header->pgm_options     = PGM_OPT_PRESENT | PGM_OPT_NETWORK;
        header->pgm_tsdu_length = 0;

/* NAK */
	nak->nak_sqn		= g_htonl (sqn_list->sqn[0]);

/* source nla */
	pgm_sockaddr_to_nla ((struct sockaddr*)&peer_nla, (char*)&nak->nak_src_nla_afi);

/* group nla */
	pgm_sockaddr_to_nla ((struct sockaddr*)&peer->group_nla, (char*)&nak->nak_grp_nla_afi);

/* OPT_NAK_LIST */
	struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(nak + 1);
	opt_len->opt_type	= PGM_OPT_LENGTH;
	opt_len->opt_length	= sizeof(struct pgm_opt_length);
	opt_len->opt_total_length = g_htons (	sizeof(struct pgm_opt_length) +
						sizeof(struct pgm_opt_header) +
						sizeof(struct pgm_opt_nak_list) +
						( (sqn_list->len-1) * sizeof(guint32) ) );
	struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
	opt_header->opt_type	= PGM_OPT_NAK_LIST | PGM_OPT_END;
	opt_header->opt_length	= sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_nak_list)
				+ ( (sqn_list->len-1) * sizeof(guint32) );
	struct pgm_opt_nak_list* opt_nak_list = (struct pgm_opt_nak_list*)(opt_header + 1);
	opt_nak_list->opt_reserved = 0;

#ifdef TRANSPORT_DEBUG
	char nak1[1024];
	sprintf (nak1, "send_nak_list( %" G_GUINT32_FORMAT " + [", sqn_list->sqn[0]);
#endif
	for (unsigned i = 1; i < sqn_list->len; i++) {
		opt_nak_list->opt_sqn[i-1] = g_htonl (sqn_list->sqn[i]);

#ifdef TRANSPORT_DEBUG
		char nak2[1024];
		sprintf (nak2, "%" G_GUINT32_FORMAT " ", sqn_list->sqn[i]);
		strcat (nak1, nak2);
#endif
	}

#ifdef TRANSPORT_DEBUG
	g_trace ("INFO", "%s]%i )", nak1, sqn_list->len);
#endif

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				FALSE,			/* regular socket */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&peer_nla,
				pgm_sockaddr_len(&peer_nla));

	if ( sent != (gssize)tpdu_length )
	{
		return -1;
	}

	peer->cumulative_stats[PGM_PC_RECEIVER_SELECTIVE_NAK_PACKETS_SENT]++;
	peer->cumulative_stats[PGM_PC_RECEIVER_SELECTIVE_NAKS_SENT] += 1 + sqn_list->len;

	return 0;
}

/* A NCF packet with a OPT_NAK_LIST option extension
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */
static int
send_ncf_list (
	pgm_transport_t*	transport,
	struct sockaddr*	nak_src_nla,
	struct sockaddr*	nak_grp_nla,
	pgm_sqn_list_t*		sqn_list,
	gboolean		is_parity		/* send parity NCF */
	)
{
	g_assert (sqn_list->len > 1);
	g_assert (sqn_list->len <= 63);

	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_nak)
			+ sizeof(struct pgm_opt_length)		/* includes header */
			+ sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_nak_list)
			+ ( (sqn_list->len-1) * sizeof(guint32) );
	guint8 buf[ tpdu_length ];

	struct pgm_header *header = (struct pgm_header*)buf;
	struct pgm_nak *ncf = (struct pgm_nak*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));

	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;
	header->pgm_type        = PGM_NCF;
        header->pgm_options     = is_parity ? (PGM_OPT_PRESENT | PGM_OPT_NETWORK | PGM_OPT_PARITY) : (PGM_OPT_PRESENT | PGM_OPT_NETWORK);
        header->pgm_tsdu_length = 0;

/* NCF */
	ncf->nak_sqn		= g_htonl (sqn_list->sqn[0]);

/* source nla */
	pgm_sockaddr_to_nla (nak_src_nla, (char*)&ncf->nak_src_nla_afi);

/* group nla */
	pgm_sockaddr_to_nla (nak_grp_nla, (char*)&ncf->nak_grp_nla_afi);

/* OPT_NAK_LIST */
	struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(ncf + 1);
	opt_len->opt_type	= PGM_OPT_LENGTH;
	opt_len->opt_length	= sizeof(struct pgm_opt_length);
	opt_len->opt_total_length = g_htons (	sizeof(struct pgm_opt_length) +
						sizeof(struct pgm_opt_header) +
						sizeof(struct pgm_opt_nak_list) +
						( (sqn_list->len-1) * sizeof(guint32) ) );
	struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
	opt_header->opt_type	= PGM_OPT_NAK_LIST | PGM_OPT_END;
	opt_header->opt_length	= sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_nak_list)
				+ ( (sqn_list->len-1) * sizeof(guint32) );
	struct pgm_opt_nak_list* opt_nak_list = (struct pgm_opt_nak_list*)(opt_header + 1);
	opt_nak_list->opt_reserved = 0;

#ifdef TRANSPORT_DEBUG
	char nak1[1024];
	sprintf (nak1, "send_ncf_list( %" G_GUINT32_FORMAT " + [", sqn_list->sqn[0]);
#endif
	for (unsigned i = 1; i < sqn_list->len; i++) {
		opt_nak_list->opt_sqn[i-1] = g_htonl (sqn_list->sqn[i]);

#ifdef TRANSPORT_DEBUG
		char nak2[1024];
		sprintf (nak2, "%" G_GUINT32_FORMAT " ", sqn_list->sqn[i]);
		strcat (nak1, nak2);
#endif
	}

#ifdef TRANSPORT_DEBUG
	g_trace ("INFO", "%s]%i )", nak1, sqn_list->len);
#endif

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial ((char*)header, tpdu_length, 0));

	gssize sent = pgm_sendto (transport,
				FALSE,			/* not rate limited */
				TRUE,			/* with router alert */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

	if ( sent != (gssize)tpdu_length )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length;

	return 0;
}
#endif /* !PGM_SINGLE_NAK */

/* check all receiver windows for packets in BACK-OFF_STATE, on expiration send a NAK.
 * update transport::next_nak_rb_timestamp for next expiration time.
 *
 * peer object is locked before entry.
 */

static void
nak_rb_state (
	pgm_peer_t*		peer
	)
{
	pgm_rxw_t* rxw = (pgm_rxw_t*)peer->rxw;
	pgm_transport_t* transport = peer->transport;
	GList* list;
#ifndef PGM_SINGLE_NAK
	pgm_sqn_list_t nak_list;
	nak_list.len = 0;
#endif

	g_trace ("INFO", "nak_rb_state(len=%u)", g_list_length(rxw->backoff_queue->tail));

/* send all NAKs first, lack of data is blocking contiguous processing and its 
 * better to get the notification out a.s.a.p. even though it might be waiting
 * in a kernel queue.
 *
 * alternative: after each packet check for incoming data and return to the
 * event loop.  bias for shorter loops as retry count increases.
 */
	list = rxw->backoff_queue->tail;
	if (!list) {
		g_warning ("backoff queue is empty in nak_rb_state.");
		return;
	}

	guint dropped_invalid = 0;

/* have not learned this peers NLA */
	gboolean is_valid_nla = (((struct sockaddr_in*)&peer->nla)->sin_addr.s_addr != INADDR_ANY);

/* TODO: process BOTH selective and parity NAKs? */

/* calculate current transmission group for parity enabled peers */
	if (peer->use_ondemand_parity)
	{
		guint32 tg_sqn_mask = 0xffffffff << peer->tg_sqn_shift;

/* NAKs only generated previous to current transmission group */
		guint32 current_tg_sqn = ((pgm_rxw_t*)peer->rxw)->lead & tg_sqn_mask;

		guint32 nak_tg_sqn = 0;
		guint32 nak_pkt_cnt = 0;

/* parity NAK generation */

		while (list)
		{
			GList* next_list_el = list->prev;
			pgm_rxw_packet_t* rp = (pgm_rxw_packet_t*)list->data;

/* check this packet for state expiration */
			if (pgm_time_after_eq(pgm_time_now, rp->nak_rb_expiry))
			{
				if (!is_valid_nla) {
					dropped_invalid++;
					g_trace ("INFO", "lost data #%u due to no peer NLA.", rp->sequence_number);
					pgm_rxw_mark_lost (rxw, rp->sequence_number);

/* mark receiver window for flushing on next recv() */
					if (!rxw->waiting_link.data)
					{
						g_static_mutex_lock (&transport->waiting_mutex);
						rxw->waiting_link.data = rxw;
						rxw->waiting_link.next = transport->peers_waiting;
						transport->peers_waiting = &rxw->waiting_link;
						g_static_mutex_unlock (&transport->waiting_mutex);
					}

					list = next_list_el;
					continue;
				}

/* TODO: parity nak lists */
				guint32 tg_sqn = rp->sequence_number & tg_sqn_mask;
				if (	( nak_pkt_cnt && tg_sqn == nak_tg_sqn ) ||
					( !nak_pkt_cnt && tg_sqn != current_tg_sqn )	)
				{
/* remove from this state */
					pgm_rxw_pkt_state_unlink (rxw, rp);

					if (!nak_pkt_cnt++)
						nak_tg_sqn = tg_sqn;
					rp->nak_transmit_count++;

					rp->state = PGM_PKT_WAIT_NCF_STATE;
					g_queue_push_head_link (rxw->wait_ncf_queue, &rp->link_);

#ifdef PGM_ABSOLUTE_EXPIRY
					rp->nak_rpt_expiry = rp->nak_rb_expiry + transport->nak_rpt_ivl;
					while (pgm_time_after_eq(pgm_time_now, rp->nak_rpt_expiry){
						rp->nak_rpt_expiry += transport->nak_rpt_ivl;
						rp->ncf_retry_count++;
					}
#else
					rp->nak_rpt_expiry = pgm_time_now + transport->nak_rpt_ivl;
#endif
				}
				else
				{	/* different transmission group */
					break;
				}
			}
			else
			{	/* packet expires some time later */
				break;
			}

			list = next_list_el;
		}

		if (nak_pkt_cnt)
		{
			send_parity_nak (peer, nak_tg_sqn, nak_pkt_cnt);
		}
	}
	else
	{

/* select NAK generation */

		while (list)
		{
			GList* next_list_el = list->prev;
			pgm_rxw_packet_t* rp = (pgm_rxw_packet_t*)list->data;

/* check this packet for state expiration */
			if (pgm_time_after_eq(pgm_time_now, rp->nak_rb_expiry))
			{
				if (!is_valid_nla) {
					dropped_invalid++;
					g_trace ("INFO", "lost data #%u due to no peer NLA.", rp->sequence_number);
					pgm_rxw_mark_lost (rxw, rp->sequence_number);

/* mark receiver window for flushing on next recv() */
					if (!rxw->waiting_link.data)
					{
						g_static_mutex_lock (&transport->waiting_mutex);
						rxw->waiting_link.data = rxw;
						rxw->waiting_link.next = transport->peers_waiting;
						transport->peers_waiting = &rxw->waiting_link;
						g_static_mutex_unlock (&transport->waiting_mutex);
					}

					list = next_list_el;
					continue;
				}

/* remove from this state */
				pgm_rxw_pkt_state_unlink (rxw, rp);

#if PGM_SINGLE_NAK
				if (!transport->is_passive)
					send_nak (transport, peer, rp->sequence_number);
				pgm_time_update_now();
#else
				nak_list.sqn[nak_list.len++] = rp->sequence_number;
#endif

				rp->nak_transmit_count++;

				rp->state = PGM_PKT_WAIT_NCF_STATE;
				g_queue_push_head_link (rxw->wait_ncf_queue, &rp->link_);

/* we have two options here, calculate the expiry time in the new state relative to the current
 * state execution time, skipping missed expirations due to delay in state processing, or base
 * from the actual current time.
 */
#ifdef PGM_ABSOLUTE_EXPIRY
				rp->nak_rpt_expiry = rp->nak_rb_expiry + transport->nak_rpt_ivl;
				while (pgm_time_after_eq(pgm_time_now, rp->nak_rpt_expiry){
					rp->nak_rpt_expiry += transport->nak_rpt_ivl;
					rp->ncf_retry_count++;
				}
#else
				rp->nak_rpt_expiry = pgm_time_now + transport->nak_rpt_ivl;
g_trace("INFO", "rp->nak_rpt_expiry in %f seconds.",
		pgm_to_secsf( rp->nak_rpt_expiry - pgm_time_now ) );
#endif

#ifndef PGM_SINGLE_NAK
				if (nak_list.len == G_N_ELEMENTS(nak_list.sqn)) {
					if (!transport->is_passive)
						send_nak_list (peer, &nak_list);
					pgm_time_update_now();
					nak_list.len = 0;
				}
#endif
			}
			else
			{	/* packet expires some time later */
				break;
			}

			list = next_list_el;
		}

#ifndef PGM_SINGLE_NAK
		if (!transport->is_passive && nak_list.len)
		{
			if (nak_list.len > 1) {
				send_nak_list (peer, &nak_list);
			} else {
				g_assert (nak_list.len == 1);
				send_nak (peer, nak_list.sqn[0]);
			}
		}
#endif

	}

	if (dropped_invalid) {
		g_message ("dropped %u messages due to invalid NLA.", dropped_invalid);
	}

	if (rxw->backoff_queue->length == 0)
	{
		g_assert ((struct rxw_packet*)rxw->backoff_queue->head == NULL);
		g_assert ((struct rxw_packet*)rxw->backoff_queue->tail == NULL);
	}
	else
	{
		g_assert ((struct rxw_packet*)rxw->backoff_queue->head != NULL);
		g_assert ((struct rxw_packet*)rxw->backoff_queue->tail != NULL);
	}

	if (rxw->backoff_queue->tail)
		g_trace ("INFO", "next expiry set in %f seconds.", pgm_to_secsf((float)next_nak_rb_expiry(rxw) - (float)pgm_time_now));
	else
		g_trace ("INFO", "backoff queue empty.");
}

/* check this peer for NAK state timers, uses the tail of each queue for the nearest
 * timer execution.
 */

static void
check_peer_nak_state (
	pgm_transport_t*	transport
	)
{
	if (!transport->peers_list) {
		return;
	}

	GList* list = transport->peers_list;
	do {
		GList* next = list->next;
		pgm_peer_t* peer = list->data;
		pgm_rxw_t* rxw = (pgm_rxw_t*)peer->rxw;

		g_static_mutex_lock (&peer->mutex);

		if (peer->spmr_expiry)
		{
			if (pgm_time_after_eq (pgm_time_now, peer->spmr_expiry))
			{
				if (transport->is_passive)
					peer->spmr_expiry = 0;
				else
					send_spmr (peer);
			}
		}

		if (rxw->backoff_queue->tail)
		{
			if (pgm_time_after_eq (pgm_time_now, next_nak_rb_expiry(rxw)))
			{
				nak_rb_state (peer);
			}
		}
		
		if (rxw->wait_ncf_queue->tail)
		{
			if (pgm_time_after_eq (pgm_time_now, next_nak_rpt_expiry(rxw)))
			{
				nak_rpt_state (peer);
			}
		}

		if (rxw->wait_data_queue->tail)
		{
			if (pgm_time_after_eq (pgm_time_now, next_nak_rdata_expiry(rxw)))
			{
				nak_rdata_state (peer);
			}
		}

/* expired, remove from hash table and linked list */
		if (pgm_time_after_eq (pgm_time_now, peer->expiry))
		{
			g_message ("peer expired, tsi %s", pgm_print_tsi (&peer->tsi));
			g_hash_table_remove (transport->peers_hashtable, &peer->tsi);
			transport->peers_list = g_list_remove_link (transport->peers_list, &peer->link_);
			g_static_mutex_unlock (&peer->mutex);
			pgm_peer_unref (peer);
		}
		else
		{
			g_static_mutex_unlock (&peer->mutex);
		}

		list = next;
	} while (list);

/* check for waiting contiguous packets */
	if (transport->peers_waiting != transport->peers_last_waiting)
	{
		g_trace ("INFO","prod rx thread");
		const char one = '1';
		if (1 != write (transport->waiting_pipe[1], &one, sizeof(one))) {
			g_critical ("write to waiting pipe failed :(");
		}

/* remember prod */
		g_static_mutex_lock (&transport->waiting_mutex);
		transport->peers_last_waiting = transport->peers_waiting;
		g_static_mutex_unlock (&transport->waiting_mutex);
	}
}

/* find the next state expiration time among the transports peers.
 *
 * on success, returns the earliest of the expiration parameter or next
 * peer expiration time.
 */
static pgm_time_t
min_nak_expiry (
	pgm_time_t		expiration,
	pgm_transport_t*	transport
	)
{
	if (!transport->peers_list) {
		goto out;
	}

	GList* list = transport->peers_list;
	do {
		GList* next = list->next;
		pgm_peer_t* peer = (pgm_peer_t*)list->data;
		pgm_rxw_t* rxw = (pgm_rxw_t*)peer->rxw;
	
		g_static_mutex_lock (&peer->mutex);

		if (peer->spmr_expiry)
		{
			if (pgm_time_after_eq (expiration, peer->spmr_expiry))
			{
				expiration = peer->spmr_expiry;
			}
		}

		if (rxw->backoff_queue->tail)
		{
			if (pgm_time_after_eq (expiration, next_nak_rb_expiry(rxw)))
			{
				expiration = next_nak_rb_expiry(rxw);
			}
		}

		if (rxw->wait_ncf_queue->tail)
		{
			if (pgm_time_after_eq (expiration, next_nak_rpt_expiry(rxw)))
			{
				expiration = next_nak_rpt_expiry(rxw);
			}
		}

		if (rxw->wait_data_queue->tail)
		{
			if (pgm_time_after_eq (expiration, next_nak_rdata_expiry(rxw)))
			{
				expiration = next_nak_rdata_expiry(rxw);
			}
		}
	
		g_static_mutex_unlock (&peer->mutex);

		list = next;
	} while (list);

out:
	return expiration;
}

/* check WAIT_NCF_STATE, on expiration move back to BACK-OFF_STATE, on exceeding NAK_NCF_RETRIES
 * cancel the sequence number.
 */
static void
nak_rpt_state (
	pgm_peer_t*		peer
	)
{
	pgm_rxw_t* rxw = (pgm_rxw_t*)peer->rxw;
	pgm_transport_t* transport = peer->transport;
	GList* list = rxw->wait_ncf_queue->tail;

	g_trace ("INFO", "nak_rpt_state(len=%u)", g_list_length(rxw->wait_ncf_queue->tail));

	guint dropped_invalid = 0;
	guint dropped = 0;

/* have not learned this peers NLA */
	gboolean is_valid_nla = (((struct sockaddr_in*)&peer->nla)->sin_addr.s_addr != INADDR_ANY);

	while (list)
	{
		GList* next_list_el = list->prev;
		pgm_rxw_packet_t* rp = (pgm_rxw_packet_t*)list->data;

/* check this packet for state expiration */
		if (pgm_time_after_eq(pgm_time_now, rp->nak_rpt_expiry))
		{
			if (!is_valid_nla) {
				dropped_invalid++;
				g_trace ("INFO", "lost data #%u due to no peer NLA.", rp->sequence_number);
				pgm_rxw_mark_lost (rxw, rp->sequence_number);

/* mark receiver window for flushing on next recv() */
				if (!rxw->waiting_link.data)
				{
					g_static_mutex_lock (&transport->waiting_mutex);
					rxw->waiting_link.data = rxw;
					rxw->waiting_link.next = transport->peers_waiting;
					transport->peers_waiting = &rxw->waiting_link;
					g_static_mutex_unlock (&transport->waiting_mutex);
				}

				list = next_list_el;
				continue;
			}

			if (++rp->ncf_retry_count > transport->nak_ncf_retries)
			{
/* cancellation */
				dropped++;
				g_trace ("INFO", "lost data #%u due to cancellation.", rp->sequence_number);

				guint32 fail_time = pgm_time_now - rp->t0;
				if (!peer->max_fail_time) {
					peer->max_fail_time = peer->min_fail_time = fail_time;
				}
				else
				{
					if (fail_time > peer->max_fail_time)
						peer->max_fail_time = fail_time;
					else if (fail_time < peer->min_fail_time)
						peer->min_fail_time = fail_time;
				}

				pgm_rxw_mark_lost (rxw, rp->sequence_number);

/* mark receiver window for flushing on next recv() */
				if (!rxw->waiting_link.data)
				{
					g_static_mutex_lock (&transport->waiting_mutex);
					rxw->waiting_link.data = rxw;
					rxw->waiting_link.next = transport->peers_waiting;
					transport->peers_waiting = &rxw->waiting_link;
					g_static_mutex_unlock (&transport->waiting_mutex);
				}

				peer->cumulative_stats[PGM_PC_RECEIVER_NAKS_FAILED_NCF_RETRIES_EXCEEDED]++;
			}
			else
			{
/* retry */
				g_trace("INFO", "retry #%u attempt %u/%u.", rp->sequence_number, rp->ncf_retry_count, transport->nak_ncf_retries);
				pgm_rxw_pkt_state_unlink (rxw, rp);
				rp->state = PGM_PKT_BACK_OFF_STATE;
				g_queue_push_head_link (rxw->backoff_queue, &rp->link_);
//				rp->nak_rb_expiry = rp->nak_rpt_expiry + nak_rb_ivl(transport);
				rp->nak_rb_expiry = pgm_time_now + nak_rb_ivl(transport);
			}
		}
		else
		{
/* packet expires some time later */
			g_trace("INFO", "#%u retry is delayed %f seconds.", rp->sequence_number, pgm_to_secsf(rp->nak_rpt_expiry - pgm_time_now));
			break;
		}
		

		list = next_list_el;
	}

	if (rxw->wait_ncf_queue->length == 0)
	{
		g_assert ((pgm_rxw_packet_t*)rxw->wait_ncf_queue->head == NULL);
		g_assert ((pgm_rxw_packet_t*)rxw->wait_ncf_queue->tail == NULL);
	}
	else
	{
		g_assert ((pgm_rxw_packet_t*)rxw->wait_ncf_queue->head);
		g_assert ((pgm_rxw_packet_t*)rxw->wait_ncf_queue->tail);
	}

	if (dropped_invalid) {
		g_message ("dropped %u messages due to invalid NLA.", dropped_invalid);
	}

	if (dropped) {
		g_message ("dropped %u messages due to ncf cancellation, "
				"rxw_sqns %" G_GUINT32_FORMAT
				" bo %" G_GUINT32_FORMAT
				" ncf %" G_GUINT32_FORMAT
				" wd %" G_GUINT32_FORMAT
				" lost %" G_GUINT32_FORMAT
				" frag %" G_GUINT32_FORMAT,
				dropped,
				pgm_rxw_sqns(rxw),
				rxw->backoff_queue->length,
				rxw->wait_ncf_queue->length,
				rxw->wait_data_queue->length,
				rxw->lost_count,
				rxw->fragment_count);
	}

	if (rxw->wait_ncf_queue->tail) {
		if (next_nak_rpt_expiry(rxw) > pgm_time_now)
		{
			g_trace ("INFO", "next expiry set in %f seconds.", pgm_to_secsf(next_nak_rpt_expiry(rxw) - pgm_time_now));
		} else {
			g_trace ("INFO", "next expiry set in -%f seconds.", pgm_to_secsf(pgm_time_now - next_nak_rpt_expiry(rxw)));
		}
	} else
		g_trace ("INFO", "wait ncf queue empty.");
}

/* check WAIT_DATA_STATE, on expiration move back to BACK-OFF_STATE, on exceeding NAK_DATA_RETRIES
 * canel the sequence number.
 */
static void
nak_rdata_state (
	pgm_peer_t*		peer
	)
{
	pgm_rxw_t* rxw = (pgm_rxw_t*)peer->rxw;
	pgm_transport_t* transport = peer->transport;
	GList* list = rxw->wait_data_queue->tail;

	g_trace ("INFO", "nak_rdata_state(len=%u)", g_list_length(rxw->wait_data_queue->tail));

	guint dropped_invalid = 0;
	guint dropped = 0;

/* have not learned this peers NLA */
	gboolean is_valid_nla = (((struct sockaddr_in*)&peer->nla)->sin_addr.s_addr != INADDR_ANY);

	while (list)
	{
		GList* next_list_el = list->prev;
		pgm_rxw_packet_t* rp = (pgm_rxw_packet_t*)list->data;

/* check this packet for state expiration */
		if (pgm_time_after_eq(pgm_time_now, rp->nak_rdata_expiry))
		{
			if (!is_valid_nla) {
				dropped_invalid++;
				g_trace ("INFO", "lost data #%u due to no peer NLA.", rp->sequence_number);
				pgm_rxw_mark_lost (rxw, rp->sequence_number);

/* mark receiver window for flushing on next recv() */
				if (!rxw->waiting_link.data)
				{
					g_static_mutex_lock (&transport->waiting_mutex);
					rxw->waiting_link.data = rxw;
					rxw->waiting_link.next = transport->peers_waiting;
					transport->peers_waiting = &rxw->waiting_link;
					g_static_mutex_unlock (&transport->waiting_mutex);
				}

				list = next_list_el;
				continue;
			}

			if (++rp->data_retry_count > transport->nak_data_retries)
			{
/* cancellation */
				dropped++;
				g_trace ("INFO", "lost data #%u due to cancellation.", rp->sequence_number);

				guint32 fail_time = pgm_time_now - rp->t0;
				if (fail_time > peer->max_fail_time)		peer->max_fail_time = fail_time;
				else if (fail_time < peer->min_fail_time)	peer->min_fail_time = fail_time;

				pgm_rxw_mark_lost (rxw, rp->sequence_number);

/* mark receiver window for flushing on next recv() */
				if (!rxw->waiting_link.data)
				{
					g_static_mutex_lock (&transport->waiting_mutex);
					rxw->waiting_link.data = rxw;
					rxw->waiting_link.next = transport->peers_waiting;
					transport->peers_waiting = &rxw->waiting_link;
					g_static_mutex_unlock (&transport->waiting_mutex);
				}

				peer->cumulative_stats[PGM_PC_RECEIVER_NAKS_FAILED_DATA_RETRIES_EXCEEDED]++;

				list = next_list_el;
				continue;
			}

/* remove from this state */
			pgm_rxw_pkt_state_unlink (rxw, rp);

/* retry back to back-off state */
			g_trace("INFO", "retry #%u attempt %u/%u.", rp->sequence_number, rp->data_retry_count, transport->nak_data_retries);
			rp->state = PGM_PKT_BACK_OFF_STATE;
			g_queue_push_head_link (rxw->backoff_queue, &rp->link_);
//			rp->nak_rb_expiry = rp->nak_rdata_expiry + nak_rb_ivl(transport);
			rp->nak_rb_expiry = pgm_time_now + nak_rb_ivl(transport);
		}
		else
		{	/* packet expires some time later */
			break;
		}
		

		list = next_list_el;
	}

	if (rxw->wait_data_queue->length == 0)
	{
		g_assert ((pgm_rxw_packet_t*)rxw->wait_data_queue->head == NULL);
		g_assert ((pgm_rxw_packet_t*)rxw->wait_data_queue->tail == NULL);
	}
	else
	{
		g_assert ((pgm_rxw_packet_t*)rxw->wait_data_queue->head);
		g_assert ((pgm_rxw_packet_t*)rxw->wait_data_queue->tail);
	}

	if (dropped_invalid) {
		g_message ("dropped %u messages due to invalid NLA.", dropped_invalid);
	}

	if (dropped) {
		g_message ("dropped %u messages due to data cancellation.", dropped);
	}

	if (rxw->wait_data_queue->tail)
		g_trace ("INFO", "next expiry set in %f seconds.", pgm_to_secsf(next_nak_rdata_expiry(rxw) - pgm_time_now));
	else
		g_trace ("INFO", "wait data queue empty.");
}

/* cancel any pending heartbeat SPM and schedule a new one
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */
static int
pgm_reset_heartbeat_spm (pgm_transport_t* transport)
{
	int retval = 0;

	g_static_mutex_lock (&transport->mutex);

/* re-set spm timer */
	transport->spm_heartbeat_state = 1;
	transport->next_heartbeat_spm = pgm_time_update_now() + transport->spm_heartbeat_interval[transport->spm_heartbeat_state++];

/* prod timer thread if sleeping */
	if (pgm_time_after( transport->next_poll, transport->next_heartbeat_spm ))
	{
		transport->next_poll = transport->next_heartbeat_spm;
		g_trace ("INFO","pgm_reset_heartbeat_spm: prod timer thread");
		const char one = '1';
		if (1 != write (transport->timer_pipe[1], &one, sizeof(one))) {
			g_critical ("write to timer pipe failed :(");
			retval = -EINVAL;
		}
	}

	g_static_mutex_unlock (&transport->mutex);

	return retval;
}

/* can be called from any thread, it needs to update the transmit window with the new
 * data and then send on the wire, only then can control return to the callee.
 *
 * special care is necessary with the provided memory, it must be previously allocated
 * from the transmit window, and offset to include the pgm header.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on invalid arguments, -EINVAL is returned.
 */
static gssize
pgm_transport_send_one_unlocked (
	pgm_transport_t*	transport,
	gpointer		buf,		/* offset to payload, no options */
	gsize			count,
	G_GNUC_UNUSED int	flags
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail (buf != NULL, -EINVAL);
	g_return_val_if_fail (count <= transport->max_tsdu, -EINVAL);

/* retrieve packet storage from transmit window
 */
	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + count;
	gpointer pkt = (guint8*)buf - sizeof(struct pgm_header) - sizeof(struct pgm_data);

	struct pgm_header *header = (struct pgm_header*)pkt;
	struct pgm_data *odata = (struct pgm_data*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;
	header->pgm_type        = PGM_ODATA;
        header->pgm_options     = 0;
        header->pgm_tsdu_length = g_htons (count);

/* ODATA */
        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

        header->pgm_checksum    = 0;
        header->pgm_checksum	= pgm_csum_fold (pgm_csum_partial (header, tpdu_length, 0));

/* add to transmit window */
	pgm_txw_push (transport->txw, pkt, tpdu_length);

	gssize sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				FALSE,			/* regular socket */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	pgm_reset_heartbeat_spm (transport);

	if ( sent == (gssize)count )
	{
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += count;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]++;
		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length + transport->iphdr_len;
	}

	return (gssize)count;
}

/* one packet, one buffer.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
static inline gssize
pgm_transport_send_one_copy_unlocked (
	pgm_transport_t*	transport,
	gconstpointer		buf,		/* payload */
	gsize			count,
	int			flags
	)
{
	if (flags & MSG_DONTWAIT)
	{
	        gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + count;
		int result = pgm_rate_check (transport->rate_control, tpdu_length, flags);
		if (result == -1) {
			return (gssize)result;
		}
	}

	g_static_rw_lock_writer_lock (&transport->txw_lock);

	gpointer pkt = pgm_txw_alloc (transport->txw);

/* retrieve packet storage from transmit window */
	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + count;

	struct pgm_header *header = (struct pgm_header*)pkt;
	struct pgm_data *odata = (struct pgm_data*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;
	header->pgm_type        = PGM_ODATA;
        header->pgm_options     = 0;
        header->pgm_tsdu_length = g_htons (count);

/* ODATA */
        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

        header->pgm_checksum    = 0;

	gsize pgm_header_len	= (guint8*)(odata + 1) - (guint8*)header;
	guint32 unfolded_header	= pgm_csum_partial (header, pgm_header_len, 0);
	guint32 unfolded_odata	= pgm_csum_partial_copy (buf, odata + 1, count, 0);
	header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, unfolded_odata, pgm_header_len));

/* add to transmit window */
	pgm_txw_push (transport->txw, pkt, tpdu_length);

	gssize sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				FALSE,			/* regular socket */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* save unfolded odata for retransmissions */
	*(guint32*)(void*)&header->pgm_sport = unfolded_odata;

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	pgm_reset_heartbeat_spm (transport);

	if ( sent == (gssize)tpdu_length )
	{
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += count;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]++;
		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length + transport->iphdr_len;
	}

/* return data payload length sent */
	return (gssize)count;
}

/* one packet spread across a scatter/gather io vector
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
static inline gssize
pgm_transport_send_one_iov_unlocked (
	pgm_transport_t*	transport,
	const struct iovec*	vector,
	guint			count,		/* number of items in vector */
	int			flags
	)
{
/* determine APDU length */
	gsize apdu_length = 0;
	for (unsigned i = 0; i < count; i++)
	{
		apdu_length += vector[i].iov_len;
	}

	if (flags & MSG_DONTWAIT)
	{
	        gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + apdu_length;
		int result = pgm_rate_check (transport->rate_control, tpdu_length, flags);
		if (result == -1) {
			return (gssize)result;
		}
	}

	g_static_rw_lock_writer_lock (&transport->txw_lock);

	gpointer pkt = pgm_txw_alloc (transport->txw);

/* retrieve packet storage from transmit window */
	gsize tpdu_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + apdu_length;

	struct pgm_header *header = (struct pgm_header*)pkt;
	struct pgm_data *odata = (struct pgm_data*)(header + 1);
	memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_gsi_t));
	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;
	header->pgm_type        = PGM_ODATA;
        header->pgm_options     = 0;
        header->pgm_tsdu_length = g_htons (apdu_length);

/* ODATA */
        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

        header->pgm_checksum    = 0;
	gsize pgm_header_len	= (guint8*)(odata + 1) - (guint8*)header;
	guint32 unfolded_header	= pgm_csum_partial ((const void*)header, pgm_header_len, 0);
	guint32 unfolded_odata = 0;

	guint vector_index = 0;
	guint vector_offset = 0;

/* iterate over one or more vector elements to perform scatter/gather checksum & copy */
	guint src_offset = 0;
	gsize copy_length = apdu_length;
	for (;;)
	{
		gsize element_length = vector[vector_index].iov_len - vector_offset;

		if (copy_length <= element_length)
		{
			unfolded_odata = pgm_csum_partial_copy ((char*)(vector[vector_index].iov_base) + vector_offset, (char*)(odata + 1) + src_offset, copy_length, unfolded_odata);
			if (copy_length == element_length) {
				vector_index++;
				vector_offset = 0;
			} else {
				vector_offset += copy_length;
			}
			break;
		}
		else
		{
/* copy part of TSDU */
			unfolded_odata = pgm_csum_partial_copy ((char*)(vector[vector_index].iov_base) + vector_offset, (char*)(odata + 1) + src_offset, element_length, unfolded_odata);
			src_offset += element_length;
			copy_length -= element_length;
			vector_index++;
			vector_offset = 0;
		}
	}

	header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, unfolded_odata, pgm_header_len));

/* add to transmit window */
	pgm_txw_push (transport->txw, pkt, tpdu_length);

	gssize sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				FALSE,			/* regular socket */
				header,
				tpdu_length,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* save unfolded odata for retransmissions */
	*(guint32*)(void*)&header->pgm_sport = unfolded_odata;

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	pgm_reset_heartbeat_spm (transport);

	if ( sent == (gssize)tpdu_length )
	{
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += apdu_length;
		transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT]++;
		transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += tpdu_length + transport->iphdr_len;
	}

/* return data payload length sent */
	return (gssize)apdu_length;
}

/* copy application data (apdu) to multiple tx window (tpdu) entries and send.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
static inline gssize
pgm_transport_send_apdu_unlocked (
	pgm_transport_t*	transport,
	gconstpointer		buf,
	guint			count,
	int			flags		/* MSG_DONTWAIT = non-blocking */)
{
	guint32 opt_sqn = pgm_txw_next_lead(transport->txw);
	guint packets = 0;
	gsize bytes_sent = 0;
	gsize data_bytes_sent = 0;
	gsize data_bytes_offset = 0;

/* if parity is enabled a 16-bit word is necessary to store actual TSDU length for 
 * variable TSDU length transmission groups.
 */
	gsize varpkt_reserve = transport->use_varpkt_len ? sizeof(guint16) : 0;

	if (flags & MSG_DONTWAIT)
	{
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + 
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		gsize tpdu_length = 0;
		guint offset_ = 0;
		do {
			gsize tsdu_length = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, count - offset_);
			tpdu_length += transport->iphdr_len + header_length + tsdu_length;
			offset_ += tsdu_length;
		} while (offset_ < count);

/* calculation includes one iphdr length already */
		int result = pgm_rate_check (transport->rate_control, tpdu_length - transport->iphdr_len, flags);
		if (result == -1) {
			return (gssize)result;
		}
	}

	g_static_rw_lock_writer_lock (&transport->txw_lock);

	do {
/* retrieve packet storage from transmit window */
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + 
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		gsize tsdu_length = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, count - data_bytes_offset);
		gsize tpdu_length = header_length + tsdu_length;

		gpointer pkt = pgm_txw_alloc(transport->txw);
		struct pgm_header *header = (struct pgm_header*)pkt;
		memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_tsi_t));
		header->pgm_sport	= transport->tsi.sport;
		header->pgm_dport	= transport->dport;
		header->pgm_type        = PGM_ODATA;
	        header->pgm_options     = PGM_OPT_PRESENT;
	        header->pgm_tsdu_length = g_htons (tsdu_length);

/* ODATA */
		struct pgm_data *odata = (struct pgm_data*)(header + 1);
	        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
	        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

/* OPT_LENGTH */
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(odata + 1);
		opt_len->opt_type	= PGM_OPT_LENGTH;
		opt_len->opt_length	= sizeof(struct pgm_opt_length);
		opt_len->opt_total_length	= g_htons (	sizeof(struct pgm_opt_length) +
								sizeof(struct pgm_opt_header) +
								sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
		opt_header->opt_type	= PGM_OPT_FRAGMENT | PGM_OPT_END;
		opt_header->opt_length	= sizeof(struct pgm_opt_header) +
						sizeof(struct pgm_opt_fragment);
		struct pgm_opt_fragment* opt_fragment = (struct pgm_opt_fragment*)(opt_header + 1);
		opt_fragment->opt_reserved	= 0;
		opt_fragment->opt_sqn		= g_htonl (opt_sqn);
		opt_fragment->opt_frag_off	= g_htonl (data_bytes_offset);
		opt_fragment->opt_frag_len	= g_htonl (count);

/* TODO: the assembly checksum & copy routine is faster than memcpy & pgm_cksum on >= opteron hardware */
	        header->pgm_checksum    = 0;

		gsize pgm_header_len	= (guint8*)(opt_fragment + 1) - (guint8*)header;
		guint32 unfolded_header = pgm_csum_partial (header, pgm_header_len, 0);
		guint32 unfolded_odata	= pgm_csum_partial_copy ((const guint8*)buf + data_bytes_offset, opt_fragment + 1, tsdu_length, 0);
		header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, unfolded_odata, pgm_header_len));

/* add to transmit window */
		pgm_txw_push (transport->txw, pkt, tpdu_length);

		gssize sent = pgm_sendto (transport,
					TRUE,			/* rate limited */
					FALSE,			/* regular socket */
					header,
					tpdu_length,
					MSG_CONFIRM,		/* not expecting a reply */
					(struct sockaddr*)&transport->send_smr.smr_multiaddr,
					pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* save unfolded odata for retransmissions */
		*(guint32*)(void*)&header->pgm_sport = unfolded_odata;

		if (sent == (gssize)tpdu_length)
		{
			packets++;
			bytes_sent += tpdu_length + transport->iphdr_len;
			data_bytes_sent += tsdu_length;
		}

		data_bytes_offset += tsdu_length;

	} while (data_bytes_offset < count);

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	pgm_reset_heartbeat_spm (transport);

	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT] += packets;	/* assuming packets not APDUs */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += bytes_sent;

	return (gssize)count;
}

/* copy application data (apdu) from a scatter/gather IO vector
 * to multiple tx window (tpdu) entries and send.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
static inline gsize
pgm_transport_send_iov_apdu_unlocked (
	pgm_transport_t*	transport,
	const struct iovec*	vector,
	guint			count,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	guint packets = 0;
	gsize bytes_sent = 0;
	gsize data_bytes_sent = 0;
	gsize data_bytes_offset = 0;

/* if parity is enabled a 16-bit word is necessary to store actual TSDU length for 
 * variable TSDU length transmission groups.
 */
	gsize varpkt_reserve = transport->use_varpkt_len ? sizeof(guint16) : 0;

/* determine APDU length */
	gsize apdu_length = 0;
	for (unsigned i = 0; i < count; i++)
	{
		apdu_length += vector[i].iov_len;
	}

	guint vector_index = 0;
	guint vector_offset = 0;

/* rate limit */
	if (flags & MSG_DONTWAIT)
	{
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + 
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		gsize tpdu_length = 0;
		guint offset_ = 0;
		do {
			gsize tsdu_length = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, apdu_length - offset_);
			tpdu_length += transport->iphdr_len + header_length + tsdu_length;
			offset_ += tsdu_length;
		} while (offset_ < apdu_length);

/* calculation includes one iphdr length already */
		int result = pgm_rate_check (transport->rate_control, tpdu_length - transport->iphdr_len, flags);
		if (result == -1) {
			return (gssize)result;
		}
	}

	g_static_rw_lock_writer_lock (&transport->txw_lock);

	guint32 first_sqn = pgm_txw_next_lead(transport->txw);

	do {
/* retrieve packet storage from transmit window */
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + 
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		gsize tsdu_length = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, apdu_length - data_bytes_offset);
		gsize tpdu_length = header_length + tsdu_length;

		gpointer pkt = pgm_txw_alloc(transport->txw);
		struct pgm_header *header = (struct pgm_header*)pkt;
		memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_tsi_t));
		header->pgm_sport	= transport->tsi.sport;
		header->pgm_dport	= transport->dport;
		header->pgm_type        = PGM_ODATA;
	        header->pgm_options     = PGM_OPT_PRESENT;
	        header->pgm_tsdu_length = g_htons (tsdu_length);

/* ODATA */
		struct pgm_data *odata = (struct pgm_data*)(header + 1);
	        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
	        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

/* OPT_LENGTH */
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(odata + 1);
		opt_len->opt_type	= PGM_OPT_LENGTH;
		opt_len->opt_length	= sizeof(struct pgm_opt_length);
		opt_len->opt_total_length	= g_htons (	sizeof(struct pgm_opt_length) +
								sizeof(struct pgm_opt_header) +
								sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
		opt_header->opt_type	= PGM_OPT_FRAGMENT | PGM_OPT_END;
		opt_header->opt_length	= sizeof(struct pgm_opt_header) +
						sizeof(struct pgm_opt_fragment);
		struct pgm_opt_fragment* opt_fragment = (struct pgm_opt_fragment*)(opt_header + 1);
		opt_fragment->opt_reserved	= 0;
		opt_fragment->opt_sqn		= g_htonl (first_sqn);
		opt_fragment->opt_frag_off	= g_htonl (data_bytes_offset);
		opt_fragment->opt_frag_len	= g_htonl (count);

/* checksum & copy */
	        header->pgm_checksum    = 0;
		gsize pgm_header_len	= (guint8*)(opt_fragment + 1) - (guint8*)header;
		guint32 unfolded_header = pgm_csum_partial (header, pgm_header_len, 0);
		guint32 unfolded_odata  = 0;

/* iterate over one or more vector elements to perform scatter/gather checksum & copy */
		guint src_offset = 0;
		gsize copy_length = tsdu_length;
		for (;;)
		{
			gsize element_length = vector[vector_index].iov_len - vector_offset;

			if (copy_length <= element_length)
			{
				unfolded_odata = pgm_csum_partial_copy ((char*)(vector[vector_index].iov_base) + vector_offset, (char*)(opt_fragment + 1) + src_offset, copy_length, unfolded_odata);
				if (copy_length == element_length) {
					vector_index++;
					vector_offset = 0;
				} else {
					vector_offset += copy_length;
				}
				break;
			}
			else
			{
/* copy part of TSDU */
				unfolded_odata = pgm_csum_partial_copy ((char*)(vector[vector_index].iov_base) + vector_offset, (char*)(opt_fragment + 1) + src_offset, element_length, unfolded_odata);
				src_offset += element_length;
				copy_length -= element_length;
				vector_index++;
				vector_offset = 0;
			}
		}

		header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, unfolded_odata, pgm_header_len));

/* add to transmit window */
		pgm_txw_push (transport->txw, pkt, tpdu_length);

		gssize sent = pgm_sendto (transport,
					TRUE,			/* rate limited */
					FALSE,			/* regular socket */
					header,
					tpdu_length,
					MSG_CONFIRM,		/* not expecting a reply */
					(struct sockaddr*)&transport->send_smr.smr_multiaddr,
					pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* save unfolded odata for retransmissions */
		*(guint32*)(void*)&header->pgm_sport = unfolded_odata;

		if ( sent == (gssize)tpdu_length)
		{
			packets++;
			bytes_sent += tpdu_length + transport->iphdr_len;
			data_bytes_sent += tsdu_length;
		}

		data_bytes_offset += tsdu_length;

	} while (data_bytes_offset < apdu_length);

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	pgm_reset_heartbeat_spm (transport);

	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT] += packets;	/* assuming packets not APDUs */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += bytes_sent;

	return (gssize)apdu_length;
}

/* vector of one packet = one buffer.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
static inline gssize
pgm_transport_send_iov_tsdu_unlocked (
	pgm_transport_t*	transport,
	const struct iovec*	vector,		/* packet */
	gsize			count,
	int			flags,
	gboolean		is_one_apdu	/* does the vector represent one APDU */
	)
{
	guint packets = 0;
	gsize bytes_sent = 0;
	gsize data_bytes_sent = 0;
	gsize data_bytes_offset = 0;

/* if parity is enabled a 16-bit word is necessary to store actual TSDU length for 
 * variable TSDU length transmission groups.
 */
	if (flags & MSG_DONTWAIT)
	{
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data);
		if (is_one_apdu) {
			header_length += sizeof(struct pgm_opt_length) +		/* includes header */
					 sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		}
		gsize total_tpdu_length = 0;
		for (guint i = 0; i < count; i++)
		{
			total_tpdu_length += transport->iphdr_len + header_length + vector[i].iov_len;
		}

/* calculation includes one iphdr length already */
		int result = pgm_rate_check (transport->rate_control, total_tpdu_length - transport->iphdr_len, flags);
		if (result == -1) {
			return (gssize)result;
		}
	}

	guint32 first_sqn = 0;
	gsize apdu_length = 0;

	if (is_one_apdu)
	{
		first_sqn = pgm_txw_next_lead(transport->txw);
		for (guint i = 0; i < count; i++)
		{
			apdu_length += vector[i].iov_len;
		}
	}

	g_static_rw_lock_writer_lock (&transport->txw_lock);

	for (guint i = 0; i < count; i++)
	{
/* retrieve packet storage from transmit window */
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data);
		if (is_one_apdu) {
			header_length += sizeof(struct pgm_opt_length) +		/* includes header */
					 sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		}
		gsize tsdu_length = vector[i].iov_len;
		gsize tpdu_length = header_length + tsdu_length;

		gpointer pkt = pgm_txw_alloc(transport->txw);
		struct pgm_header *header = (struct pgm_header*)pkt;
		memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_tsi_t));
		header->pgm_sport	= transport->tsi.sport;
		header->pgm_dport	= transport->dport;
		header->pgm_type        = PGM_ODATA;
	        header->pgm_options     = is_one_apdu ? PGM_OPT_PRESENT : 0;
	        header->pgm_tsdu_length = g_htons (tsdu_length);

/* ODATA */
		struct pgm_data *odata = (struct pgm_data*)(header + 1);
	        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
	        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

		gpointer dst = NULL;

		if (is_one_apdu)
		{
/* OPT_LENGTH */
			struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(odata + 1);
			opt_len->opt_type	= PGM_OPT_LENGTH;
			opt_len->opt_length	= sizeof(struct pgm_opt_length);
			opt_len->opt_total_length	= g_htons (	sizeof(struct pgm_opt_length) +
									sizeof(struct pgm_opt_header) +
									sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
			struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
			opt_header->opt_type	= PGM_OPT_FRAGMENT | PGM_OPT_END;
			opt_header->opt_length	= sizeof(struct pgm_opt_header) +
							sizeof(struct pgm_opt_fragment);
			struct pgm_opt_fragment* opt_fragment = (struct pgm_opt_fragment*)(opt_header + 1);
			opt_fragment->opt_reserved	= 0;
			opt_fragment->opt_sqn		= g_htonl (first_sqn);
			opt_fragment->opt_frag_off	= g_htonl (data_bytes_offset);
			opt_fragment->opt_frag_len	= g_htonl (apdu_length);

			dst = opt_fragment + 1;
		}
		else
		{
			dst = odata + 1;
		}

/* TODO: the assembly checksum & copy routine is faster than memcpy & pgm_cksum on >= opteron hardware */
	        header->pgm_checksum    = 0;

		gsize pgm_header_len	= (guint8*)dst - (guint8*)header;
		guint32 unfolded_header = pgm_csum_partial (header, pgm_header_len, 0);
		guint32 unfolded_odata	= pgm_csum_partial_copy ((guint8*)vector[i].iov_base + data_bytes_offset, dst, tsdu_length, 0);
		header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, unfolded_odata, pgm_header_len));

/* add to transmit window */
		pgm_txw_push (transport->txw, pkt, tpdu_length);

		gssize sent = pgm_sendto (transport,
					TRUE,			/* rate limited */
					FALSE,			/* regular socket */
					header,
					tpdu_length,
					MSG_CONFIRM,		/* not expecting a reply */
					(struct sockaddr*)&transport->send_smr.smr_multiaddr,
					pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* save unfolded odata for retransmissions */
		*(guint32*)(void*)&header->pgm_sport = unfolded_odata;

		if (sent == (gssize)tpdu_length)
		{
			packets++;
			bytes_sent += tpdu_length + transport->iphdr_len;
			data_bytes_sent += tsdu_length;
		}

		data_bytes_offset += tsdu_length;

	} while (data_bytes_offset < count);

/* release txw lock here in order to allow spms to lock mutex */
	g_static_rw_lock_writer_unlock (&transport->txw_lock);

	pgm_reset_heartbeat_spm (transport);

	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT] += packets;	/* assuming packets not APDUs */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += bytes_sent;

	return (gssize)data_bytes_sent;
}

/* send an apdu.
 *
 * copy into receive window one or more fragments.
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */

gssize
pgm_transport_send (
	pgm_transport_t*	transport,
	gconstpointer		data,
	gsize			len,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	g_assert( transport->can_send );

	if ( len <= pgm_transport_max_tsdu (transport, FALSE) )
	{
		return pgm_transport_send_one_copy_unlocked (transport, data, len, flags);
	}

	return pgm_transport_send_apdu_unlocked (transport, data, len, flags);
}

/* send a vector of apdu's, lock spins per APDU to allow SPM and RDATA generation.
 *
 * non-blocking only works roughly at the rate control layer, other packets might get in as the
 * locks spin.
 *
 *    ⎢ APDU₀ ⎢                            ⎢ ⋯ TSDU₁,₀ TSDU₀,₀ ⎢
 *    ⎢ APDU₁ ⎢ → pgm_transport_sendv() →  ⎢ ⋯ TSDU₁,₁ TSDU₀,₁ ⎢ → kernel
 *    ⎢   ⋮   ⎢                            ⎢     ⋮       ⋮     ⎢
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */

gssize
pgm_transport_sendv (
	pgm_transport_t*	transport,
	const struct iovec*	vector,
	guint			count,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	g_assert( transport->can_send );

	gsize varpkt_reserve = transport->use_varpkt_len ? sizeof(guint16) : 0;

        if (flags & MSG_DONTWAIT)
        {
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) +
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
                gsize tpdu_length = 0;
		for (unsigned i = 0; i < count; i++)
		{
/* for each apdu */
			guint offset_ = 0;
			gsize count_ = vector[i].iov_len;
			do {
				gsize tsdu_length = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, count_ - offset_);
				tpdu_length += transport->iphdr_len + header_length + tsdu_length;
				offset_ += tsdu_length;
			} while (offset_ < count_);
		}

                int result = pgm_rate_check (transport->rate_control, tpdu_length - transport->iphdr_len, flags);
                if (result == -1) {
			return (gssize)result;
                }
        }

	gssize total_sent = 0;
	for (unsigned i = 0; i < count; i++)
	{
		gssize sent;

		if ( vector[i].iov_len <= ( transport->max_tpdu - (  transport->iphdr_len +
								     sizeof(struct pgm_header) +
    	  	                                                     sizeof(struct pgm_data) +
								     varpkt_reserve ) ) )
		{
			sent = pgm_transport_send_one_copy_unlocked (transport, vector[i].iov_base, vector[i].iov_len, 0);
		}
		else
		{
			sent = pgm_transport_send_apdu_unlocked (transport, vector[i].iov_base, vector[i].iov_len, 0);
		}

		g_assert( sent >= 0 );
		total_sent += sent;
	}

	return total_sent;
}

/* partial apdu sending, on first call offset = 0
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */

static inline gssize
pgm_transport_send_pkt_dontwait_unlocked (
	pgm_transport_t*	transport,
	gconstpointer		buf,
	gsize			count,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	guint packets = 0;
	gsize bytes_sent = 0;
	gsize data_bytes_sent = 0;

	gsize varpkt_reserve = transport->use_varpkt_len ? sizeof(guint16) : 0;

/* try again call */
	if (transport->has_blocking_send)
	{
		goto try_send_again;
	}

/* first call */
	if (!transport->has_txw_writer_lock)
	{
		g_static_rw_lock_writer_lock (&transport->txw_lock);

#define STATE(x)	(transport->pkt_dontwait_state.x)
		STATE(data_bytes_offset) = 0;
		STATE(first_sqn)	 = pgm_txw_next_lead(transport->txw);
	}

	do {
/* retrieve packet storage from transmit window */
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + 
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		STATE(tsdu_length) = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, count - STATE(data_bytes_offset));
		STATE(tpdu_length) = header_length + STATE(tsdu_length);

		STATE(pkt) = pgm_txw_alloc(transport->txw);
		struct pgm_header *header = (struct pgm_header*)STATE(pkt);
		memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_tsi_t));
		header->pgm_sport	= transport->tsi.sport;
		header->pgm_dport	= transport->dport;
		header->pgm_type        = PGM_ODATA;
	        header->pgm_options     = PGM_OPT_PRESENT;
	        header->pgm_tsdu_length = g_htons (STATE(tsdu_length));

/* ODATA */
		struct pgm_data *odata = (struct pgm_data*)(header + 1);
	        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
	        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

/* OPT_LENGTH */
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(odata + 1);
		opt_len->opt_type	= PGM_OPT_LENGTH;
		opt_len->opt_length	= sizeof(struct pgm_opt_length);
		opt_len->opt_total_length	= g_htons (	sizeof(struct pgm_opt_length) +
								sizeof(struct pgm_opt_header) +
								sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
		opt_header->opt_type	= PGM_OPT_FRAGMENT | PGM_OPT_END;
		opt_header->opt_length	= sizeof(struct pgm_opt_header) +
						sizeof(struct pgm_opt_fragment);
		struct pgm_opt_fragment* opt_fragment = (struct pgm_opt_fragment*)(opt_header + 1);
		opt_fragment->opt_reserved	= 0;
		opt_fragment->opt_sqn		= g_htonl (STATE(first_sqn));
		opt_fragment->opt_frag_off	= g_htonl (STATE(data_bytes_offset));
		opt_fragment->opt_frag_len	= g_htonl (count);

/* TODO: the assembly checksum & copy routine is faster than memcpy & pgm_cksum on >= opteron hardware */
	        header->pgm_checksum    = 0;

		gsize pgm_header_len	= (guint8*)(opt_fragment + 1) - (guint8*)header;
		guint32 unfolded_header	= pgm_csum_partial (header, pgm_header_len, 0);
		STATE(unfolded_odata)	= pgm_csum_partial_copy ((const guint8*)buf + STATE(data_bytes_offset), opt_fragment + 1, STATE(tsdu_length), 0);
		header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
		pgm_txw_push (transport->txw, STATE(pkt), STATE(tpdu_length));

		gssize sent;

try_send_again:
		sent = pgm_sendto (transport,
					TRUE,			/* rate limited */
					FALSE,			/* regular socket */
					STATE(pkt),
					STATE(tpdu_length),
					MSG_DONTWAIT | MSG_CONFIRM,	/* not expecting a reply */
					(struct sockaddr*)&transport->send_smr.smr_multiaddr,
					pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));
		if (sent < 0 && errno == EAGAIN)
		{
			transport->has_blocking_send = TRUE;
			goto blocked;
		}

/* save unfolded odata for retransmissions */
		*(guint32*)(void*)&header->pgm_sport = STATE(unfolded_odata);

		if ( sent == (gssize)STATE(tpdu_length) )
		{
			packets++;
			bytes_sent += STATE(tpdu_length) + transport->iphdr_len;
			data_bytes_sent += STATE(tsdu_length);
		}

		STATE(data_bytes_offset) += STATE(tsdu_length);

	} while (STATE(data_bytes_offset) < count);

/* only cleanup on trailing packet */
	if (STATE(data_bytes_offset) == count)
	{
/* release txw lock here in order to allow spms to lock mutex */
		g_static_rw_lock_writer_unlock (&transport->txw_lock);

		pgm_reset_heartbeat_spm (transport);

		transport->has_txw_writer_lock = FALSE;
	}

	transport->has_blocking_send = FALSE;

	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT] += packets;	/* assuming packets not APDUs */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += bytes_sent;

	return STATE(data_bytes_offset);
#undef STATE

blocked:
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT] += packets;	/* assuming packets not APDUs */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += bytes_sent;

	return -1;
}

/*
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
gssize
pgm_transport_send_pkt_dontwait (
	pgm_transport_t*	transport,
	gconstpointer		data,
	gsize			len,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	g_assert( transport->can_send );

	if ( len <= pgm_transport_max_tsdu (transport, FALSE) )
	{
		g_static_rw_lock_writer_lock (&transport->txw_lock);
		return pgm_transport_send_one_copy_unlocked (transport, data, len, flags);
	}

	return pgm_transport_send_pkt_dontwait_unlocked (transport, data, len, flags);
}

/* send a vector of tsdu's owned by the transmit window
 *
 *    ⎢ TSDU₀ ⎢
 *    ⎢ TSDU₁ ⎢ → pgm_transport_sendv2() →  ⎢ ⋯ TSDU₁ TSDU₀ ⎢ → kernel
 *    ⎢   ⋮   ⎢
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
gssize
pgm_transport_sendv2 (
	G_GNUC_UNUSED pgm_transport_t*		transport,
	G_GNUC_UNUSED const struct iovec*	vector,
	G_GNUC_UNUSED guint			count,
	G_GNUC_UNUSED int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	g_assert( transport->can_send );

	if ( count == 1 ) {
		return pgm_transport_send_one_unlocked (transport,
							vector->iov_base,
							vector->iov_len,
							flags);
	}

	return 0;	/* not-implemented */
}

/* send a vector of tsdu's owned by the application
 *
 *    ⎢ TSDU₀ ⎢
 *    ⎢ TSDU₁ ⎢ → pgm_transport_sendv2_copy() →  ⎢ ⋯ TSDU₁ TSDU₀ ⎢ → kernel
 *    ⎢   ⋮   ⎢
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
gssize
pgm_transport_sendv2_copy (
	G_GNUC_UNUSED pgm_transport_t*		transport,
	G_GNUC_UNUSED const struct iovec*	vector,		/* packet not payload */
	G_GNUC_UNUSED guint			count,
	G_GNUC_UNUSED int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	g_assert( transport->can_send );

	if ( count == 1 ) {
		return pgm_transport_send_one_copy_unlocked (transport,
							(guint8*)vector->iov_base + pgm_transport_pkt_offset(FALSE),
							vector->iov_len,
							flags);
	}

	return 0;	/* not-implemented */
}

static inline gsize
pgm_transport_send_iov_apdu_pkt_dontwait_unlocked (
	pgm_transport_t*	transport,
	const struct iovec*	vector,
	guint			count,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	guint packets = 0;
	gsize bytes_sent = 0;
	gsize data_bytes_sent = 0;

/* if parity is enabled a 16-bit word is necessary to store actual TSDU length for 
 * variable TSDU length transmission groups.
 */
	gsize varpkt_reserve = transport->use_varpkt_len ? sizeof(guint16) : 0;

/* try again call */
	if (transport->has_blocking_send)
	{
		goto try_send_again;
	}

/* first call */
	if (!transport->has_txw_writer_lock)
	{
		g_static_rw_lock_writer_lock (&transport->txw_lock);

#define STATE(x)	(transport->pkt_dontwait_state.x)
		STATE(data_bytes_offset)= 0;
		STATE(first_sqn)	= pgm_txw_next_lead(transport->txw);

/* determine APDU length */
		STATE(apdu_length)	= 0;
		for (unsigned i = 0; i < count; i++)
		{
			STATE(apdu_length) += vector[i].iov_len;
		}

		STATE(vector_index)	= 0;
		STATE(vector_offset)	= 0;
	}

	do {
/* retrieve packet storage from transmit window */
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) + 
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
		STATE(tsdu_length) = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, STATE(apdu_length) - STATE(data_bytes_offset));
		STATE(tpdu_length) = header_length + STATE(tsdu_length);

		STATE(pkt) = pgm_txw_alloc(transport->txw);
		struct pgm_header *header = (struct pgm_header*)STATE(pkt);
		memcpy (header->pgm_gsi, &transport->tsi.gsi, sizeof(pgm_tsi_t));
		header->pgm_sport	= transport->tsi.sport;
		header->pgm_dport	= transport->dport;
		header->pgm_type        = PGM_ODATA;
	        header->pgm_options     = PGM_OPT_PRESENT;
	        header->pgm_tsdu_length = g_htons (STATE(tsdu_length));

/* ODATA */
		struct pgm_data *odata = (struct pgm_data*)(header + 1);
	        odata->data_sqn         = g_htonl (pgm_txw_next_lead(transport->txw));
	        odata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

/* OPT_LENGTH */
		struct pgm_opt_length* opt_len = (struct pgm_opt_length*)(odata + 1);
		opt_len->opt_type	= PGM_OPT_LENGTH;
		opt_len->opt_length	= sizeof(struct pgm_opt_length);
		opt_len->opt_total_length	= g_htons (	sizeof(struct pgm_opt_length) +
								sizeof(struct pgm_opt_header) +
								sizeof(struct pgm_opt_fragment) );
/* OPT_FRAGMENT */
		struct pgm_opt_header* opt_header = (struct pgm_opt_header*)(opt_len + 1);
		opt_header->opt_type	= PGM_OPT_FRAGMENT | PGM_OPT_END;
		opt_header->opt_length	= sizeof(struct pgm_opt_header) +
						sizeof(struct pgm_opt_fragment);
		struct pgm_opt_fragment* opt_fragment = (struct pgm_opt_fragment*)(opt_header + 1);
		opt_fragment->opt_reserved	= 0;
		opt_fragment->opt_sqn		= g_htonl (STATE(first_sqn));
		opt_fragment->opt_frag_off	= g_htonl (STATE(data_bytes_offset));
		opt_fragment->opt_frag_len	= g_htonl (count);

/* checksum & copy */
	        header->pgm_checksum    = 0;
		gsize pgm_header_len	= (guint8*)(opt_fragment + 1) - (guint8*)header;
		guint32 unfolded_header = pgm_csum_partial (header, pgm_header_len, 0);
		STATE(unfolded_odata)   = 0;

/* iterate over one or more vector elements to perform scatter/gather checksum & copy */
		guint src_offset = 0;
		gsize copy_length = STATE(tsdu_length);
		for (;;)
		{
			gsize element_length = vector[STATE(vector_index)].iov_len - STATE(vector_offset);

			if (copy_length <= element_length)
			{
				STATE(unfolded_odata) = pgm_csum_partial_copy ((char*)(vector[STATE(vector_index)].iov_base) + STATE(vector_offset), (char*)(opt_fragment + 1) + src_offset, copy_length, STATE(unfolded_odata));
				if (copy_length == element_length) {
					STATE(vector_index)++;
					STATE(vector_offset) = 0;
				} else {
					STATE(vector_offset) += copy_length;
				}
				break;
			}
			else
			{
/* copy part of TSDU */
				STATE(unfolded_odata) = pgm_csum_partial_copy ((char*)(vector[STATE(vector_index)].iov_base) + STATE(vector_offset), (char*)(opt_fragment + 1) + src_offset, element_length, STATE(unfolded_odata));
				src_offset += element_length;
				copy_length -= element_length;
				STATE(vector_index)++;
				STATE(vector_offset) = 0;
			}
		}

		header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, STATE(unfolded_odata), pgm_header_len));

/* add to transmit window */
		pgm_txw_push (transport->txw, STATE(pkt), STATE(tpdu_length));

		gssize sent;

try_send_again:
		sent = pgm_sendto (transport,
					TRUE,			/* rate limited */
					FALSE,			/* regular socket */
					STATE(pkt),
					STATE(tpdu_length),
					MSG_CONFIRM,		/* not expecting a reply */
					(struct sockaddr*)&transport->send_smr.smr_multiaddr,
					pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));
		if (sent < 0 && errno == EAGAIN)
		{
			transport->has_blocking_send = TRUE;
			goto blocked;
		}

/* save unfolded odata for retransmissions */
		*(guint32*)(void*)&header->pgm_sport = STATE(unfolded_odata);

		if ( sent == (gssize)STATE(tpdu_length))
		{
			packets++;
			bytes_sent += STATE(tpdu_length) + transport->iphdr_len;
			data_bytes_sent += STATE(tsdu_length);
		}

		STATE(data_bytes_offset) += STATE(tsdu_length);

	} while (STATE(data_bytes_offset) < STATE(apdu_length));

/* only cleanup on trailing packet */
	if (STATE(data_bytes_offset) == STATE(apdu_length))
	{
/* release txw lock here in order to allow spms to lock mutex */
		g_static_rw_lock_writer_unlock (&transport->txw_lock);

		pgm_reset_heartbeat_spm (transport);

		transport->has_txw_writer_lock = FALSE;
	}

	transport->has_blocking_send = FALSE;

	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT] += packets;	/* assuming packets not APDUs */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += bytes_sent;

	return (gssize)STATE(apdu_length);
#undef STATE

blocked:
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_BYTES_SENT] += data_bytes_sent;
	transport->cumulative_stats[PGM_PC_SOURCE_DATA_MSGS_SENT] += packets;	/* assuming packets not APDUs */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += bytes_sent;

	return (gssize)-1;
}

/* combine and send a scatter/gather vector of application buffers
 *
 *    ⎢ DATA₀ ⎢
 *    ⎢ DATA₁ ⎢ → pgm_transport_sendv3() →  ⎢ ⋯ TSDU₁ TSDU₀ ⎢ → kernel
 *    ⎢   ⋮   ⎢
 *
 * on success, returns number of data bytes pushed into the transmit window and
 * attempted to send to the socket layer.  on non-blocking sockets, -1 is returned
 * if the packet sizes would exceed the current rate limit.
 */
gssize
pgm_transport_sendv3 (
	pgm_transport_t*	transport,
	const struct iovec*	vector,
	guint			count,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	g_assert( transport->can_send );

	gsize varpkt_reserve = transport->use_varpkt_len ? sizeof(guint16) : 0;

/* determine APDU length */
	gsize apdu_length = 0;
	for (unsigned i = 0; i < count; i++)
	{
		apdu_length += vector[i].iov_len;
	}

        if (flags & MSG_DONTWAIT)
        {
		gsize header_length = sizeof(struct pgm_header) + sizeof(struct pgm_data) +
				sizeof(struct pgm_opt_length) +		/* includes header */
				sizeof(struct pgm_opt_header) + sizeof(struct pgm_opt_fragment);
                gsize tpdu_length = 0;
		guint offset = 0;
		do {
			gsize tsdu_length = MIN(transport->max_tpdu - transport->iphdr_len - header_length - varpkt_reserve, apdu_length - offset);
			tpdu_length += header_length + tsdu_length;
			offset += tsdu_length;
		} while (offset < apdu_length);

                int result = pgm_rate_check (transport->rate_control, tpdu_length, flags);
                if (result == -1) {
			return (gssize)result;
                }
        }

	gssize sent;
	if ( apdu_length <= ( transport->max_tpdu - (  transport->iphdr_len +
						       sizeof(struct pgm_header) +
    	 	                                       sizeof(struct pgm_data) +
						       varpkt_reserve ) ) )
	{
		sent = pgm_transport_send_one_iov_unlocked (transport, vector, count, 0);
	}
	else
	{
		sent = pgm_transport_send_iov_apdu_unlocked (transport, vector, count, 0);
	}

	return sent;
}

gssize
pgm_transport_sendv3_pkt_dontwait (
	pgm_transport_t*	transport,
	const struct iovec*	vector,
	guint			count,
	int			flags		/* MSG_DONTWAIT = non-blocking */
	)
{
	g_assert( transport->can_send );

/* determine APDU length */
	gsize apdu_length = 0;
	for (unsigned i = 0; i < count; i++)
	{
		apdu_length += vector[i].iov_len;
	}
	
	gssize sent;
	if ( apdu_length <= pgm_transport_max_tsdu (transport, FALSE) )
	{
		sent = pgm_transport_send_one_iov_unlocked (transport, vector, count, flags);
	}
	else
	{
		sent = pgm_transport_send_iov_apdu_pkt_dontwait_unlocked (transport, vector, count, flags);
	}

	return sent;
}

/* send repair packet.
 *
 * on success, 0 is returned.  on error, -1 is returned, and errno set
 * appropriately.
 */
static int
send_rdata (
	pgm_transport_t*	transport,
	G_GNUC_UNUSED guint32	sequence_number,
	gpointer		data,
	gsize			len
	)
{
/* update previous odata/rdata contents */
	struct pgm_header* header = (struct pgm_header*)data;
	struct pgm_data* rdata    = (struct pgm_data*)(header + 1);
	header->pgm_type          = PGM_RDATA;

/* RDATA */
        rdata->data_trail       = g_htonl (pgm_txw_trail(transport->txw));

	guint32 unfolded_odata	= *(guint32*)(void*)&header->pgm_sport;
	header->pgm_sport	= transport->tsi.sport;
	header->pgm_dport	= transport->dport;

        header->pgm_checksum    = 0;

	gsize pgm_header_len	= len - g_ntohs(header->pgm_tsdu_length);
	guint32 unfolded_header = pgm_csum_partial (header, pgm_header_len, 0);
		unfolded_odata	= pgm_csum_partial ((char*)header + pgm_header_len, g_ntohs(header->pgm_tsdu_length), 0);

	header->pgm_checksum	= pgm_csum_fold (pgm_csum_block_add (unfolded_header, unfolded_odata, pgm_header_len));

	gssize sent = pgm_sendto (transport,
				TRUE,			/* rate limited */
				TRUE,			/* with router alert */
				header,
				len,
				MSG_CONFIRM,		/* not expecting a reply */
				(struct sockaddr*)&transport->send_smr.smr_multiaddr,
				pgm_sockaddr_len(&transport->send_smr.smr_multiaddr));

/* re-set spm timer: we are already in the timer thread, no need to prod timers
 */
	g_static_mutex_lock (&transport->mutex);
	transport->spm_heartbeat_state = 1;
	transport->next_heartbeat_spm = pgm_time_update_now() + transport->spm_heartbeat_interval[transport->spm_heartbeat_state++];
	g_static_mutex_unlock (&transport->mutex);

	if ( sent != (gssize)len )
	{
		return -1;
	}

	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_BYTES_RETRANSMITTED] += g_ntohs(header->pgm_tsdu_length);
	transport->cumulative_stats[PGM_PC_SOURCE_SELECTIVE_MSGS_RETRANSMITTED]++;	/* impossible to determine APDU count */
	transport->cumulative_stats[PGM_PC_SOURCE_BYTES_SENT] += len + transport->iphdr_len;

	return 0;
}

/* Enable FEC for this transport, specifically Reed Solmon encoding RS(n,k), common
 * setting is RS(255, 223).
 *
 * inputs:
 *
 * n = FEC Block size = [k+1, 255]
 * k = original data packets == transmission group size = [2, 4, 8, 16, 32, 64, 128]
 * m = symbol size = 8 bits
 *
 * outputs:
 *
 * h = 2t = n - k = parity packets
 *
 * when h > k parity packets can be lost.
 *
 * on success, returns 0.  on invalid setting, returns -EINVAL.
 */

int
pgm_transport_set_fec (
	pgm_transport_t*	transport,
	gboolean		use_proactive_parity,
	gboolean		use_ondemand_parity,
	gboolean		use_varpkt_len,
	guint			default_n,
	guint			default_k
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);
	g_return_val_if_fail ((default_k & (default_k -1)) == 0, -EINVAL);
	g_return_val_if_fail (default_k >= 2 && default_k <= 128, -EINVAL);
	g_return_val_if_fail (default_n >= default_k + 1 && default_n <= 255, -EINVAL);

	guint default_h = default_n - default_k;

/* check validity of parameters */
	if ( default_k > 223 &&
		( (default_h * 223.0) / default_k ) < 1.0 )
	{
		g_error ("k/h ratio too low to generate parity data.");
		return -EINVAL;
	}

	g_static_mutex_lock (&transport->mutex);
	transport->use_proactive_parity	= use_proactive_parity;
	transport->use_ondemand_parity	= use_ondemand_parity;
	transport->use_varpkt_len	= use_varpkt_len;
	transport->rs_n			= default_n;
	transport->rs_k			= default_k;
	transport->tg_sqn_shift		= pgm_power2_log2 (transport->rs_k);

//	transport->fec = pgm_fec_create (default_n, default_k);

	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* declare transport only for sending, discard any incoming SPM, ODATA,
 * RDATA, etc, packets.
 */
int
pgm_transport_set_send_only (
	pgm_transport_t*	transport
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->can_recv	= FALSE;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

/* declare transport only for receiving, no transmit window will be created
 * and no SPM broadcasts sent.
 */
int
pgm_transport_set_recv_only (
	pgm_transport_t*	transport,
	gboolean		is_passive	/* don't send any request or responses */
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);

	g_static_mutex_lock (&transport->mutex);
	transport->can_send	= FALSE;
	transport->is_passive	= is_passive;
	g_static_mutex_unlock (&transport->mutex);

	return 0;
}

static GSource*
pgm_create_timer (
	pgm_transport_t*	transport
	)
{
	g_return_val_if_fail (transport != NULL, NULL);

	GSource *source = g_source_new (&g_pgm_timer_funcs, sizeof(pgm_timer_t));
	pgm_timer_t *timer = (pgm_timer_t*)source;

	timer->transport = transport;

	return source;
}

/* on success, returns id of GSource 
 */
static int
pgm_add_timer_full (
	pgm_transport_t*	transport,
	gint			priority
	)
{
	g_return_val_if_fail (transport != NULL, -EINVAL);

	GSource* source = pgm_create_timer (transport);

	if (priority != G_PRIORITY_DEFAULT)
		g_source_set_priority (source, priority);

	guint id = g_source_attach (source, transport->timer_context);
	g_source_unref (source);

	return id;
}

static int
pgm_add_timer (
	pgm_transport_t*	transport
	)
{
	return pgm_add_timer_full (transport, G_PRIORITY_HIGH_IDLE);
}

/* determine which timer fires next: spm (ihb_tmr), nak_rb_ivl, nak_rpt_ivl, or nak_rdata_ivl
 * and check whether its already due.
 */

static gboolean
pgm_timer_prepare (
	GSource*		source,
	gint*			timeout
	)
{
	pgm_timer_t* pgm_timer = (pgm_timer_t*)source;
	pgm_transport_t* transport = pgm_timer->transport;
	glong msec;

	g_static_mutex_lock (&transport->mutex);
	pgm_time_t now = pgm_time_update_now();
	pgm_time_t expiration = now + pgm_secs( 30 );

	if (transport->can_send)
	{
		expiration = transport->spm_heartbeat_state ? MIN(transport->next_heartbeat_spm, transport->next_ambient_spm) : transport->next_ambient_spm;
		g_trace ("SPM","spm %" G_GINT64_FORMAT " usec", (gint64)expiration - (gint64)now);
	}

/* save the nearest timer */
	if (transport->can_recv)
	{
		g_static_rw_lock_reader_lock (&transport->peers_lock);
		expiration = min_nak_expiry (expiration, transport);
		g_static_rw_lock_reader_unlock (&transport->peers_lock);
	}

	transport->next_poll = pgm_timer->expiration = expiration;
	g_static_mutex_unlock (&transport->mutex);

/* advance time again to adjust for processing time out of the event loop, this
 * could cause further timers to expire even before checking for new wire data.
 */
	msec = pgm_to_msecs((gint64)expiration - (gint64)now);
	if (msec < 0)
		msec = 0;
	else
		msec = MIN (G_MAXINT, (guint)msec);

	*timeout = (gint)msec;

	g_trace ("SPM","expiration in %i msec", (gint)msec);

	return (msec == 0);
}

static int
pgm_timer_signal (
	pgm_transport_t*	transport,
	pgm_time_t		expiration
	)
{
	int retval = 0;

	g_static_mutex_lock (&transport->mutex);
	if (pgm_time_after( transport->next_poll, expiration ))
	{
		transport->next_poll = expiration;
		g_trace ("INFO","new_peer: prod timer thread");
		const char one = '1';
		if (1 != write (transport->timer_pipe[1], &one, sizeof(one))) {
			g_critical ("write to timer pipe failed :(");
			retval = -EINVAL;
		}
	}
	g_static_mutex_unlock (&transport->mutex);

	return retval;
}

static gboolean
pgm_timer_check (
	GSource*		source
	)
{
	g_trace ("SPM","pgm_timer_check");

	pgm_timer_t* pgm_timer = (pgm_timer_t*)source;
	pgm_time_t now = pgm_time_update_now();

	gboolean retval = ( pgm_time_after_eq(now, pgm_timer->expiration) );
	if (!retval) g_thread_yield();
	return retval;
}

/* call all timers, assume that time_now has been updated by either pgm_timer_prepare
 * or pgm_timer_check and no other method calls here.
 */

static gboolean
pgm_timer_dispatch (
	GSource*			source,
	G_GNUC_UNUSED GSourceFunc	callback,
	G_GNUC_UNUSED gpointer		user_data
	)
{
	g_trace ("SPM","pgm_timer_dispatch");

	pgm_timer_t* pgm_timer = (pgm_timer_t*)source;
	pgm_transport_t* transport = pgm_timer->transport;

	g_static_mutex_lock (&transport->mutex);
/* find which timers have expired and call each */
	if (transport->can_send)
	{
		if ( pgm_time_after_eq (pgm_time_now, transport->next_ambient_spm) )
		{
			send_spm_unlocked (transport);
			transport->spm_heartbeat_state = 0;
			transport->next_ambient_spm = pgm_time_now + transport->spm_ambient_interval;
		}
		else if ( transport->spm_heartbeat_state &&
			 pgm_time_after_eq (pgm_time_now, transport->next_heartbeat_spm) )
		{
			send_spm_unlocked (transport);
		
			if (transport->spm_heartbeat_interval[transport->spm_heartbeat_state])
			{
				transport->next_heartbeat_spm = pgm_time_now + transport->spm_heartbeat_interval[transport->spm_heartbeat_state++];
			}
			else
			{	/* transition heartbeat to ambient */
				transport->spm_heartbeat_state = 0;
			}
		}
	}

	if (transport->can_recv)
	{
		g_static_rw_lock_reader_lock (&transport->peers_lock);
		check_peer_nak_state (transport);
		g_static_rw_lock_reader_unlock (&transport->peers_lock);
	}
	g_static_mutex_unlock (&transport->mutex);

	return TRUE;
}

/* TODO: this should be in on_io_data to be more streamlined, or a generic options parser.
 */

static int
get_opt_fragment (
	struct pgm_opt_header*		opt_header,
	struct pgm_opt_fragment**	opt_fragment
	)
{
	int retval = 0;

	g_assert (opt_header->opt_type == PGM_OPT_LENGTH);
	g_assert (opt_header->opt_length == sizeof(struct pgm_opt_length));
//	struct pgm_opt_length* opt_len = (struct pgm_opt_length*)opt_header;

/* always at least two options, first is always opt_length */
	do {
		opt_header = (struct pgm_opt_header*)((char*)opt_header + opt_header->opt_length);

		if ((opt_header->opt_type & PGM_OPT_MASK) == PGM_OPT_FRAGMENT)
		{
			*opt_fragment = (struct pgm_opt_fragment*)(opt_header + 1);
			retval = 1;
			goto out;
		}

	} while (!(opt_header->opt_type & PGM_OPT_END));

	*opt_fragment = NULL;

out:
	return retval;
}

/* ODATA packet with any of the following options:
 *
 * OPT_FRAGMENT - this TPDU part of a larger APDU.
 *
 * returns:
 *	0
 *	-EINVAL if pipe proding failed
 */

static int
on_odata (
	pgm_peer_t*		sender,
	struct pgm_header*	header,
	gpointer		data,
	G_GNUC_UNUSED gsize	len
	)
{
	g_trace ("INFO","on_odata");

	int retval = 0;
	pgm_transport_t* transport = sender->transport;
	struct pgm_data* odata = (struct pgm_data*)data;
	odata->data_sqn = g_ntohl (odata->data_sqn);

/* pre-allocate from glib allocator (not slice allocator) full APDU packet for first new fragment, re-use
 * through to event handler.
 */
	struct pgm_opt_fragment* opt_fragment;
	pgm_time_t nak_rb_expiry = pgm_time_update_now () + nak_rb_ivl(transport);

	if ((header->pgm_options & PGM_OPT_PRESENT) && get_opt_fragment((gpointer)(odata + 1), &opt_fragment))
	{
		guint16 opt_total_length = g_ntohs(*(guint16*)( (char*)( odata + 1 ) + sizeof(guint16)));

		g_trace ("INFO","push fragment (sqn #%u trail #%u apdu_first_sqn #%u fragment_offset %u apdu_len %u)",
			odata->data_sqn, g_ntohl (odata->data_trail), g_ntohl (opt_fragment->opt_sqn), g_ntohl (opt_fragment->opt_frag_off), g_ntohl (opt_fragment->opt_frag_len));
		g_static_mutex_lock (&sender->mutex);
		retval = pgm_rxw_push_fragment_copy (sender->rxw,
					(char*)(odata + 1) + opt_total_length,
					g_ntohs (header->pgm_tsdu_length),
					odata->data_sqn,
					g_ntohl (odata->data_trail),
					opt_fragment,
					nak_rb_expiry);
	}
	else
	{
		g_static_mutex_lock (&sender->mutex);
		retval = pgm_rxw_push_copy (sender->rxw,
					odata + 1,
					g_ntohs (header->pgm_tsdu_length),
					odata->data_sqn,
					g_ntohl (odata->data_trail),
					nak_rb_expiry);
	}

	g_static_mutex_unlock (&sender->mutex);

	gboolean flush_naks = FALSE;

	switch (retval) {
	case PGM_RXW_CREATED_PLACEHOLDER:
		flush_naks = TRUE;
		break;

	case PGM_RXW_DUPLICATE:
		sender->cumulative_stats[PGM_PC_RECEIVER_DUP_DATAS]++;
		sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		break;

	case PGM_RXW_MALFORMED_APDU:
		sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_ODATA]++;

	case PGM_RXW_NOT_IN_TXW:
	case PGM_RXW_APDU_LOST:
		sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
		break;

	default:
		break;
	}

	sender->cumulative_stats[PGM_PC_RECEIVER_DATA_BYTES_RECEIVED] += g_ntohs (header->pgm_tsdu_length);
	sender->cumulative_stats[PGM_PC_RECEIVER_DATA_MSGS_RECEIVED]++;

	if (flush_naks)
	{
/* flush out 1st time nak packets */
		g_static_mutex_lock (&transport->mutex);

		if (pgm_time_after (transport->next_poll, nak_rb_expiry))
		{
			transport->next_poll = nak_rb_expiry;
			g_trace ("INFO","on_odata: prod timer thread");
			const char one = '1';
			if (1 != write (transport->timer_pipe[1], &one, sizeof(one))) {
				g_critical ("write to timer pipe failed :(");
				retval = -EINVAL;
			}
		}

		g_static_mutex_unlock (&transport->mutex);
	}

	return retval;
}

/* identical to on_odata except for statistics
 */

static int
on_rdata (
	pgm_peer_t*		sender,
	struct pgm_header*	header,
	gpointer		data,
	G_GNUC_UNUSED gsize	len
	)
{
	g_trace ("INFO","on_rdata");

	int retval = 0;
	struct pgm_transport_t* transport = sender->transport;
	struct pgm_data* rdata = (struct pgm_data*)data;
	rdata->data_sqn = g_ntohl (rdata->data_sqn);

	gboolean flush_naks = FALSE;
	pgm_time_t nak_rb_expiry = pgm_time_update_now () + nak_rb_ivl(transport);

/* parity RDATA needs to be decoded */
	if (header->pgm_options & PGM_OPT_PARITY)
	{
		guint32 tg_sqn_mask = 0xffffffff << transport->tg_sqn_shift;
		guint32 tg_sqn = rdata->data_sqn & tg_sqn_mask;

		gboolean is_var_pktlen = header->pgm_options & PGM_OPT_VAR_PKTLEN;
		gboolean is_op_encoded = header->pgm_options & PGM_OPT_PRESENT;		/* non-encoded options? */

/* determine payload location */
		guint8* rdata_bytes = (guint8*)(rdata + 1);
		struct pgm_opt_fragment* rdata_opt_fragment = NULL;
		if ((header->pgm_options & PGM_OPT_PRESENT) && get_opt_fragment((struct pgm_opt_header*)rdata_bytes, &rdata_opt_fragment))
		{
			guint16 opt_total_length = g_ntohs(*(guint16*)( (char*)( rdata + 1 ) + sizeof(guint16)));
			rdata_bytes += opt_total_length;
		}

/* create list of sequence numbers for each k packet in the FEC block */
		guint rs_h = 0;
		gsize parity_length = g_ntohs (header->pgm_tsdu_length);
		guint32 target_sqn = tg_sqn - 1;
		guint8* src[ transport->rs_n ];
		guint8* src_opts[ transport->rs_n ];
		guint32 offsets[ transport->rs_k ];
		for (guint32 i = tg_sqn; i != (tg_sqn + transport->rs_k); i++)
		{
			struct pgm_opt_fragment* opt_fragment = NULL;
			gpointer packet = NULL;
			guint16 length = 0;
			gboolean is_parity = FALSE;
			int status = pgm_rxw_peek (sender->rxw, i, &opt_fragment, &packet, &length, &is_parity);

			if (status == PGM_RXW_DUPLICATE)	/* already committed */
				goto out;
			if (status == PGM_RXW_NOT_IN_TXW)
				goto out;

			if (length == 0 && !is_parity) {	/* nothing */

				if (target_sqn == tg_sqn - 1)
				{
/* keep parity packet here */
					target_sqn = i;
					src[ transport->rs_k + rs_h ] = rdata_bytes;
					src_opts[ transport->rs_k + rs_h ] = (guint8*)rdata_opt_fragment;
					offsets[ i - tg_sqn ] = transport->rs_k + rs_h++;

/* move repair to receive window ownership */
					pgm_rxw_push_nth_parity_copy (sender->rxw,
									i,
									g_ntohl (rdata->data_trail),
									rdata_opt_fragment,
									rdata_bytes,
									parity_length,
									nak_rb_expiry);
				}
				else
				{
/* transmission group incomplete */
					g_trace ("INFO", "transmission group incomplete, awaiting further repair packets.");
					goto out;
				}

			} else if (is_parity) {			/* repair data */
				src[ transport->rs_k + rs_h ] = packet;
				src_opts[ transport->rs_k + rs_h ] = (guint8*)opt_fragment;
				offsets[ i - tg_sqn ] = transport->rs_k + rs_h++;
			} else {				/* original data */
				src[ i - tg_sqn ] = packet;
				src_opts[ i - tg_sqn ] = (guint8*)opt_fragment;
				offsets[ i - tg_sqn ] = i - tg_sqn;
				if (!is_var_pktlen && length != parity_length) {
					g_warning ("Variable TSDU length without OPT_VAR_PKTLEN.\n");
					goto out;
				}

				pgm_rxw_zero_pad (sender->rxw, packet, length, parity_length);
			}
		}

/* full transmission group, now allocate new packets */
		for (unsigned i = 0; i < transport->rs_k; i++)
		{
			if (offsets[ i ] >= transport->rs_k)
			{
				src[ i ] = pgm_rxw_alloc (sender->rxw);
				memset (src[ i ], 0, parity_length);

				if (is_op_encoded) {
					src_opts[ i ] = g_slice_alloc0 (sizeof(struct pgm_opt_fragment));
				}
			}
		}

/* decode payload */
		pgm_rs_decode_parity_appended (transport->rs, (void**)(void*)src, offsets, parity_length);

/* decode opt_fragment option */
		if (is_op_encoded)
		{
			pgm_rs_decode_parity_appended (transport->rs, (void**)(void*)src_opts, offsets, sizeof(struct pgm_opt_fragment));
		}

/* treat decoded packet as selective repair(s) */
		g_static_mutex_lock (&sender->mutex);
		gsize repair_length = parity_length;
		for (unsigned i = 0; i < transport->rs_k; i++)
		{
			if (offsets[ i ] >= transport->rs_k)
			{
/* extract TSDU length is variable packet option was found on parity packet */
				if (is_var_pktlen)
				{
					repair_length = *(guint16*)( src[i] + parity_length - 2 );
				}

				if (is_op_encoded)
				{
					retval = pgm_rxw_push_nth_repair (sender->rxw,
								tg_sqn + i,
								g_ntohl (rdata->data_trail),
								(struct pgm_opt_fragment*)src_opts[ i ],
								src[ i ],
								repair_length,
								nak_rb_expiry);
					g_slice_free1 (sizeof(struct pgm_opt_fragment), src_opts[ i ]);
				}
				else
				{
					retval = pgm_rxw_push_nth_repair (sender->rxw,
								tg_sqn + i,
								g_ntohl (rdata->data_trail),
								NULL,
								src[ i ],
								repair_length,
								nak_rb_expiry);
				}
				switch (retval) {
				case PGM_RXW_CREATED_PLACEHOLDER:
				case PGM_RXW_DUPLICATE:
					g_warning ("repaired packets not matching receive window state.");
					break;

				case PGM_RXW_MALFORMED_APDU:
					sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_RDATA]++;

				case PGM_RXW_NOT_IN_TXW:
				case PGM_RXW_APDU_LOST:
					sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
					break;

				default:
					break;
				}

				sender->cumulative_stats[PGM_PC_RECEIVER_DATA_BYTES_RECEIVED] += repair_length;
				sender->cumulative_stats[PGM_PC_RECEIVER_DATA_MSGS_RECEIVED]++;
			}
		}
		g_static_mutex_unlock (&sender->mutex);
	}
	else
	{
/* selective RDATA */

		struct pgm_opt_fragment* opt_fragment;

		if ((header->pgm_options & PGM_OPT_PRESENT) && get_opt_fragment((gpointer)(rdata + 1), &opt_fragment))
		{
			guint16 opt_total_length = g_ntohs(*(guint16*)( (char*)( rdata + 1 ) + sizeof(guint16)));

			g_trace ("INFO","push fragment (sqn #%u trail #%u apdu_first_sqn #%u fragment_offset %u apdu_len %u)",
				 rdata->data_sqn, g_ntohl (rdata->data_trail), g_ntohl (opt_fragment->opt_sqn), g_ntohl (opt_fragment->opt_frag_off), g_ntohl (opt_fragment->opt_frag_len));
			g_static_mutex_lock (&sender->mutex);
			retval = pgm_rxw_push_fragment_copy (sender->rxw,
						(char*)(rdata + 1) + opt_total_length,
						g_ntohs (header->pgm_tsdu_length),
						rdata->data_sqn,
						g_ntohl (rdata->data_trail),
						opt_fragment,
						nak_rb_expiry);
		}
		else
		{
			g_static_mutex_lock (&sender->mutex);
			retval = pgm_rxw_push_copy (sender->rxw,
						rdata + 1,
						g_ntohs (header->pgm_tsdu_length),
						rdata->data_sqn,
						g_ntohl (rdata->data_trail),
						nak_rb_expiry);
		}
		g_static_mutex_unlock (&sender->mutex);

		switch (retval) {
		case PGM_RXW_CREATED_PLACEHOLDER:
			flush_naks = TRUE;
			break;

		case PGM_RXW_DUPLICATE:
			sender->cumulative_stats[PGM_PC_RECEIVER_DUP_DATAS]++;
			sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			break;

		case PGM_RXW_MALFORMED_APDU:
			sender->cumulative_stats[PGM_PC_RECEIVER_MALFORMED_RDATA]++;

		case PGM_RXW_NOT_IN_TXW:
		case PGM_RXW_APDU_LOST:
			sender->cumulative_stats[PGM_PC_RECEIVER_PACKETS_DISCARDED]++;
			break;

		default:
			break;
		}

		sender->cumulative_stats[PGM_PC_RECEIVER_DATA_BYTES_RECEIVED] += g_ntohs (header->pgm_tsdu_length);
		sender->cumulative_stats[PGM_PC_RECEIVER_DATA_MSGS_RECEIVED]++;
	}

	if (flush_naks)
	{
/* flush out 1st time nak packets */
		g_static_mutex_lock (&transport->mutex);

		if (pgm_time_after (transport->next_poll, nak_rb_expiry))
		{
			transport->next_poll = nak_rb_expiry;
			g_trace ("INFO","on_odata: prod timer thread");
			const char one = '1';
			if (1 != write (transport->timer_pipe[1], &one, sizeof(one))) {
				g_critical ("write to timer pipe failed :(");
				retval = -EINVAL;
			}
		}

		g_static_mutex_unlock (&transport->mutex);
	}

out:
	return retval;
}

/* eof */
