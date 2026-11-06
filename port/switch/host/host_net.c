/*
HOST_NET.C

The game's network calls (port/linux/src/posix.h) on the Switch.

libnx has BSD sockets over the console's bsd service - socket(), poll(),
getaddrinfo() and the rest, with their headers in libnx's own include
directory rather than newlib's, which is how an earlier version of this
file came to believe there were none and answered every call with failure.
host_main.c initialises them (socketInitialize, with four times the
default buffer budget) at startup.

The socket calls are posix_net.c's, which turns POSIX results into the
Winsock ones the game expects, with three differences: no SOCK_CLOEXEC
(nothing is ever exec'd here), accept rather than accept4, and the local
address found only by the route a UDP socket would take (there is no
getifaddrs). posix_net.c itself is not compiled: the rest of it is the
desktop's - command lines from /proc, a runtime directory, URL schemes,
Discord's local socket - which are answered below as having nothing to
offer. UPnP is posix_upnp.c's, compiled here with the port's miniupnpc.
*/

#include "host.h"

#include "posix.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

/* ---------- sockets */

/* Winsock error codes (winsockx.h) */
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEACCES 10013
#define WSAEFAULT 10014
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEDESTADDRREQ 10039
#define WSAEMSGSIZE 10040
#define WSAEPROTOTYPE 10041
#define WSAENOPROTOOPT 10042
#define WSAEPROTONOSUPPORT 10043
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETDOWN 10050
#define WSAENETUNREACH 10051
#define WSAENETRESET 10052
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAESHUTDOWN 10058
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAEHOSTDOWN 10064
#define WSAEHOSTUNREACH 10065

/* Winsock SOL_SOCKET option values (winsockx.h) */
#define WINSOCK_SOL_SOCKET 0xffff
#define WINSOCK_SO_REUSEADDR 0x0004
#define WINSOCK_SO_KEEPALIVE 0x0008
#define WINSOCK_SO_BROADCAST 0x0020
#define WINSOCK_SO_LINGER 0x0080
#define WINSOCK_SO_SNDBUF 0x1001
#define WINSOCK_SO_RCVBUF 0x1002
#define WINSOCK_SO_ERROR 0x1007
#define WINSOCK_SO_TYPE 0x1008

static __thread int last_error;

/* Socket buffers on the console come out of one budget per process, which
the bsd service is given when sockets are initialised (libnx's default: four
times the largest TCP and UDP buffers, about 2.2 MB; host_main.c's is four
times that). Running out of it is
WSAENOBUFS to the game, which gives up whatever it was making - the first
internet join failed that way, on creating its client - so it is always
said. It is rare by nature, unlike the per-datagram failures, so saying it
costs nothing in an ordinary run. */
#define BUFFER_FAILURES_LOGGED 20
/* the most a socket's buffer is set to (posix_socket_setsockopt): libnx's
default tcp_tx_buf_max_size and tcp_rx_buf_max_size (host_main.c's are
larger only to make the budget larger) */
#define SOCKET_BUFFER_LIMIT (256 * 1024)

static int fail(void)
{
	if (errno == ENOBUFS || errno == ENOMEM)
	{
		static volatile int said;

		if (said < BUFFER_FAILURES_LOGGED)
		{
			said++;
			host_logf(HOST_LOG_WARN, "socket: out of buffer space (%s); the console's socket budget is used up",
				strerror(errno));
		}
	}
	switch (errno)
	{
	case EINTR: last_error = WSAEINTR; break;
	case EBADF: last_error = WSAEBADF; break;
	case EACCES: case EPERM: last_error = WSAEACCES; break;
	case EFAULT: last_error = WSAEFAULT; break;
	case EMFILE: case ENFILE: last_error = WSAEMFILE; break;
	case EAGAIN: last_error = WSAEWOULDBLOCK; break;
	case EINPROGRESS: last_error = WSAEINPROGRESS; break;
	case EALREADY: last_error = WSAEALREADY; break;
	case ENOTSOCK: last_error = WSAENOTSOCK; break;
	case EDESTADDRREQ: last_error = WSAEDESTADDRREQ; break;
	case EMSGSIZE: last_error = WSAEMSGSIZE; break;
	case EPROTOTYPE: last_error = WSAEPROTOTYPE; break;
	case ENOPROTOOPT: last_error = WSAENOPROTOOPT; break;
	case EPROTONOSUPPORT: last_error = WSAEPROTONOSUPPORT; break;
	case EOPNOTSUPP: last_error = WSAEOPNOTSUPP; break;
	case EAFNOSUPPORT: last_error = WSAEAFNOSUPPORT; break;
	case EADDRINUSE: last_error = WSAEADDRINUSE; break;
	case EADDRNOTAVAIL: last_error = WSAEADDRNOTAVAIL; break;
	case ENETDOWN: last_error = WSAENETDOWN; break;
	case ENETUNREACH: last_error = WSAENETUNREACH; break;
	case ENETRESET: last_error = WSAENETRESET; break;
	case ECONNABORTED: last_error = WSAECONNABORTED; break;
	/* a send on a connection the other end reset (with MSG_NOSIGNAL):
	Winsock's WSAECONNRESET, which the game takes as the connection lost */
	case ECONNRESET: case EPIPE: last_error = WSAECONNRESET; break;
#ifdef ESHUTDOWN
	case ESHUTDOWN: last_error = WSAESHUTDOWN; break;
#endif
	case EHOSTDOWN: last_error = WSAEHOSTDOWN; break;
	case ENOBUFS: case ENOMEM: last_error = WSAENOBUFS; break;
	case EISCONN: last_error = WSAEISCONN; break;
	case ENOTCONN: last_error = WSAENOTCONN; break;
	case ETIMEDOUT: last_error = WSAETIMEDOUT; break;
	case ECONNREFUSED: last_error = WSAECONNREFUSED; break;
	case EHOSTUNREACH: last_error = WSAEHOSTUNREACH; break;
	default: last_error = WSAEINVAL; break;
	}
	return -1;
}

static int succeed(int result)
{
	if (result < 0)
		return fail();
	last_error = 0;
	return result;
}

/* Addresses.

The game's are Winsock's (Linux's are the same): a 16-bit family, then the
port and the address. libnx's are BSD's: an 8-bit length, an 8-bit family,
then the port and the address at the same offsets. Passed through as they
were, the console read the family as 0 (and the length as 2) and refused
every address, so each one is turned round on the way in and on the way
out. */
static socklen_t address_in(const void *address, int length, struct sockaddr_storage *converted)
{
	const unsigned char *bytes = address;

	if (!address || length < 2)
		return (socklen_t)(length < 0 ? 0 : length);
	if ((size_t)length > sizeof(*converted))
		length = (int)sizeof(*converted);
	memset(converted, 0, sizeof(*converted));
	memcpy(converted, address, (size_t)length);
	converted->ss_len = (unsigned char)length;
	converted->ss_family = (sa_family_t)(bytes[0] | (bytes[1] << 8));
	return (socklen_t)length;
}

static void address_out(const struct sockaddr_storage *converted, socklen_t converted_length, void *address,
	int *length)
{
	unsigned char *bytes = address;
	int size;

	if (!address || !length)
		return;
	size = (int)converted_length < *length ? (int)converted_length : *length;
	if (size > 0)
		memcpy(address, converted, (size_t)size);
	if (size >= 2)
	{
		bytes[0] = (unsigned char)converted->ss_family;
		bytes[1] = 0;
	}
	*length = (int)converted_length;
}

/* The limited broadcast, 255.255.255.255, is what system link sends its
searches to, and the console's network stack does not deliver it: the
subnet's own broadcast address (192.168.1.255 on a /24) is what reaches the
other machines. Worked out from nifm's view of the connection. */
static uint32_t directed_broadcast(void)
{
	static int nifm_ready = -1;
	u32 address = 0, mask = 0, gateway = 0, dns1 = 0, dns2 = 0;

	if (nifm_ready < 0)
		nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User)) ? 1 : 0;
	if (!nifm_ready || R_FAILED(nifmGetCurrentIpConfigInfo(&address, &mask, &gateway, &dns1, &dns2)) ||
		!address || !mask)
		return 0;
	/* both in network order: the broadcast is the address with the host
	part all ones */
	return (address & mask) | ~mask;
}

int posix_socket_last_error(void)
{
	return last_error;
}

/* The sockets open through these calls (posix_socket and posix_socket_accept,
less posix_socket_close; libnx's resolver, nxlink and miniupnpc open theirs
directly, and are not counted), and the most open at once: each new most
past SOCKETS_SAID is logged, so a log shows how many were open when the
console's stack began refusing connections (as a host's of many players) */
#define SOCKETS_SAID 16

static int sockets_open;
static int sockets_most;

static void watch_forget(int socket, int closing);

static void sockets_opened(int result, const char *how)
{
	int open;

	if (result < 0)
		return;
	watch_forget(result, 1);
	open = __atomic_add_fetch(&sockets_open, 1, __ATOMIC_RELAXED);
	if (open > __atomic_load_n(&sockets_most, __ATOMIC_RELAXED))
	{
		__atomic_store_n(&sockets_most, open, __ATOMIC_RELAXED);
		if (open > SOCKETS_SAID)
			host_logf(HOST_LOG_INFO, "sockets: %d open, the most yet (%s)", open, how);
	}
}

/* ---------- datagrams read in batches

Every socket call is a request to the console's network service, and a host
of 48 machines spent 12% of its game thread reading its datagrams one
request each (and 1 more to find there were no more). A non-blocking
socket's datagrams are read here up to DATAGRAM_BATCH at a time with one
recvmmsg, and handed out one per call; select, recv and FIONREAD count the
ones held. Only reads with no flags into buffers of DATAGRAM_SLOT bytes at
most are batched (the game's are 1200, internet play's tunnel's about
1500). Prefetched datagrams use the full slot, since later calls can use
different buffer sizes; truncation is checked again when each is read, so
a datagram too large for that call fails as Winsock's does (WSAEMSGSIZE). */

#define DATAGRAM_BATCH 16
#define DATAGRAM_SLOT 2048
#define BATCHED_SOCKETS 1024

struct datagram_queue
{
	int count, next;
	struct
	{
		int size;
		int truncated;
		socklen_t address_length;
		struct sockaddr_storage address;
		unsigned char data[DATAGRAM_SLOT];
	} datagrams[DATAGRAM_BATCH];
};

static pthread_mutex_t datagram_lock = PTHREAD_MUTEX_INITIALIZER;
static struct datagram_queue *datagram_queues[BATCHED_SOCKETS];
static unsigned char socket_nonblocking[BATCHED_SOCKETS];

/* the datagrams held for a socket (under datagram_lock) */
static int datagrams_held(int socket)
{
	struct datagram_queue *queue = socket >= 0 && socket < BATCHED_SOCKETS ? datagram_queues[socket] : NULL;

	return queue ? queue->count - queue->next : 0;
}

/* a socket closed: what it held goes */
static void datagrams_forget(int socket)
{
	if (socket < 0 || socket >= BATCHED_SOCKETS)
		return;
	pthread_mutex_lock(&datagram_lock);
	free(datagram_queues[socket]);
	datagram_queues[socket] = NULL;
	socket_nonblocking[socket] = 0;
	pthread_mutex_unlock(&datagram_lock);
}

/* the next datagram held for a socket, read in a batch if none is: its
size, or -1 with errno (EAGAIN: none); 0 if the read is not one to batch */
static int datagram_take(int socket, void *buffer, int length, int flags, struct sockaddr_storage *address,
	socklen_t *address_length, int *truncated, int *batched)
{
	struct datagram_queue *queue;
	int size;

	*batched = 0;
	if (flags || socket < 0 || socket >= BATCHED_SOCKETS || length <= 0 || length > DATAGRAM_SLOT)
		return 0;
	pthread_mutex_lock(&datagram_lock);
	if (!socket_nonblocking[socket])
	{
		pthread_mutex_unlock(&datagram_lock);
		return 0;
	}
	*batched = 1;
	queue = datagram_queues[socket];
	if (!queue)
	{
		queue = datagram_queues[socket] = calloc(1, sizeof(*queue));
		if (!queue)
		{
			*batched = 0;
			pthread_mutex_unlock(&datagram_lock);
			return 0;
		}
	}
	if (queue->next == queue->count)
	{
		struct mmsghdr messages[DATAGRAM_BATCH];
		struct iovec vectors[DATAGRAM_BATCH];
		int index, received;

		memset(messages, 0, sizeof(messages));
		for (index = 0; index < DATAGRAM_BATCH; index++)
		{
			vectors[index].iov_base = queue->datagrams[index].data;
			vectors[index].iov_len = DATAGRAM_SLOT;
			messages[index].msg_hdr.msg_name = &queue->datagrams[index].address;
			messages[index].msg_hdr.msg_namelen = sizeof(queue->datagrams[index].address);
			messages[index].msg_hdr.msg_iov = &vectors[index];
			messages[index].msg_hdr.msg_iovlen = 1;
		}
		queue->count = queue->next = 0;
		received = recvmmsg(socket, messages, DATAGRAM_BATCH, MSG_DONTWAIT, NULL);
		if (received <= 0)
		{
			pthread_mutex_unlock(&datagram_lock);
			if (received == 0)
				errno = EAGAIN;
			return -1;
		}
		for (index = 0; index < received; index++)
		{
			queue->datagrams[index].size = (int)messages[index].msg_len;
			queue->datagrams[index].truncated = (messages[index].msg_hdr.msg_flags & MSG_TRUNC) != 0;
			queue->datagrams[index].address_length = messages[index].msg_hdr.msg_namelen;
		}
		queue->count = received;
	}
	size = queue->datagrams[queue->next].size < length ? queue->datagrams[queue->next].size : length;
	memcpy(buffer, queue->datagrams[queue->next].data, (size_t)size);
	if (address)
		memcpy(address, &queue->datagrams[queue->next].address, sizeof(*address));
	if (address_length)
		*address_length = queue->datagrams[queue->next].address_length;
	*truncated = queue->datagrams[queue->next].truncated || queue->datagrams[queue->next].size > length;
	queue->next++;
	pthread_mutex_unlock(&datagram_lock);
	return size;
}

/* ---------- empty sockets watched, by a thread of their own

A game reads each of its non-blocking sockets every frame until it has
nothing (a client its host's stream and datagrams; a host each machine's),
and nearly always there is nothing: in a 64-player game about 95% of the
reads found none, and each was a request to the network service, about
0.5 ms of the game thread for each. So a non-blocking socket a read found
empty is watched: a thread waits on all such sockets in one poll, and until
it sees one readable a read of it is answered at once as Winsock's
WSAEWOULDBLOCK, asking the service nothing. A read that finds data asks as
before, until one finds none again. A socket newly watched wakes the poll
with a byte to a loopback socket of its own; a watched socket closed is
taken out of the poll before it is. A blocking socket, and a read with
flags, are not watched. */

static pthread_mutex_t watch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t watch_left = PTHREAD_COND_INITIALIZER;
static unsigned char socket_watch_nonblocking[BATCHED_SOCKETS];
/* watched: a read found it empty, and no poll has seen it readable since
(socket_empty is the reads' look at it, without the lock) */
static unsigned char socket_watched[BATCHED_SOCKETS];
static unsigned char socket_empty[BATCHED_SOCKETS];
static int watch_wake = -1;
static int watch_started;
static int watch_failed;
/* the poll's round, and whether a closed socket is waiting for it to end */
static unsigned watch_round;
static int watch_closing;

static void *watch_thread(void *unused)
{
	struct pollfd descriptors[BATCHED_SOCKETS + 1];

	(void)unused;
	for (;;)
	{
		int count = 1, index, result;

		pthread_mutex_lock(&watch_lock);
		watch_round++;
		pthread_cond_broadcast(&watch_left);
		descriptors[0].fd = watch_wake;
		descriptors[0].events = POLLIN;
		descriptors[0].revents = 0;
		for (index = 0; index < BATCHED_SOCKETS; index++)
		{
			if (socket_watched[index])
			{
				descriptors[count].fd = index;
				descriptors[count].events = POLLIN;
				descriptors[count].revents = 0;
				count++;
			}
		}
		pthread_mutex_unlock(&watch_lock);
		result = poll(descriptors, (nfds_t)count, 1000);
		if (result <= 0)
			continue;
		if (descriptors[0].revents)
		{
			unsigned char drained[64];

			while (recv(watch_wake, drained, sizeof(drained), MSG_DONTWAIT) > 0)
				;
		}
		pthread_mutex_lock(&watch_lock);
		for (index = 1; index < count; index++)
		{
			int socket = descriptors[index].fd;

			/* (readable, or an error or a hang-up, which a read has to see) */
			if (descriptors[index].revents && socket_watched[socket])
			{
				socket_watched[socket] = 0;
				__atomic_store_n(&socket_empty[socket], 0, __ATOMIC_RELEASE);
			}
		}
		pthread_mutex_unlock(&watch_lock);
	}
	return NULL;
}

/* (under watch_lock) the wake socket and the thread, the first time a socket is watched; 0 if they cannot be had */
static int watch_start(void)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);
	pthread_t thread;
	pthread_attr_t attributes;

	if (watch_started || watch_failed)
		return watch_started;
	watch_failed = 1;
	watch_wake = socket(AF_INET, SOCK_DGRAM, 0);
	if (watch_wake < 0)
		return 0;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(watch_wake, (struct sockaddr *)&address, sizeof(address)) < 0 ||
		getsockname(watch_wake, (struct sockaddr *)&address, &length) < 0 ||
		connect(watch_wake, (struct sockaddr *)&address, sizeof(address)) < 0)
	{
		close(watch_wake);
		watch_wake = -1;
		return 0;
	}
	pthread_attr_init(&attributes);
	pthread_attr_setstacksize(&attributes, 64 * 1024);
	if (pthread_create(&thread, &attributes, watch_thread, NULL) != 0)
	{
		pthread_attr_destroy(&attributes);
		close(watch_wake);
		watch_wake = -1;
		return 0;
	}
	pthread_attr_destroy(&attributes);
	pthread_detach(thread);
	watch_failed = 0;
	watch_started = 1;
	host_logf(HOST_LOG_INFO, "sockets: the empty ones watched by a thread of their own");
	return 1;
}

/* the socket is watched and not seen readable since (a select may be answered so) */
static int watch_knows_empty(int socket)
{
	return socket >= 0 && socket < BATCHED_SOCKETS && __atomic_load_n(&socket_empty[socket], __ATOMIC_ACQUIRE);
}

/* a read of the socket that can be answered as empty without asking (a non-blocking socket's, watched and not seen
readable since; a blocking one's read waits, as it would have) */
static int watch_says_empty(int socket, int flags)
{
	return !flags && watch_knows_empty(socket) && __atomic_load_n(&socket_watch_nonblocking[socket], __ATOMIC_RELAXED);
}

/* (under no lock) the socket watched from now on, if it is not: a read found it empty (only_nonblocking, a
non-blocking socket's), or a select at once did */
static void watch_add(int socket, int only_nonblocking)
{
	int woken = 0;

	pthread_mutex_lock(&watch_lock);
	if ((!only_nonblocking || socket_watch_nonblocking[socket]) && !socket_watched[socket] && watch_start())
	{
		socket_watched[socket] = 1;
		__atomic_store_n(&socket_empty[socket], 1, __ATOMIC_RELEASE);
		woken = 1;
	}
	pthread_mutex_unlock(&watch_lock);
	if (woken)
	{
		unsigned char byte = 0;
		int saved = errno;

		send(watch_wake, &byte, 1, MSG_DONTWAIT);
		errno = saved;
	}
}

/* a read found the non-blocking socket empty: watched from now on */
static void watch_empty(int socket, int flags)
{
	int woken = 0;

	if (flags || socket < 0 || socket >= BATCHED_SOCKETS || (errno != EAGAIN && errno != EWOULDBLOCK))
		return;
	pthread_mutex_lock(&watch_lock);
	if (socket_watch_nonblocking[socket] && !socket_watched[socket] && watch_start())
	{
		socket_watched[socket] = 1;
		__atomic_store_n(&socket_empty[socket], 1, __ATOMIC_RELEASE);
		woken = 1;
	}
	pthread_mutex_unlock(&watch_lock);
	if (woken)
	{
		unsigned char byte = 0;
		int saved = errno;

		send(watch_wake, &byte, 1, MSG_DONTWAIT);
		errno = saved;
	}
}

/* the socket's watch let go (closed, made blocking, or a new socket in its place); a socket closed waits for the poll
to leave it */
static void watch_forget(int socket, int closing)
{
	int watched;

	if (socket < 0 || socket >= BATCHED_SOCKETS)
		return;
	pthread_mutex_lock(&watch_lock);
	watched = socket_watched[socket];
	socket_watched[socket] = 0;
	__atomic_store_n(&socket_empty[socket], 0, __ATOMIC_RELEASE);
	if (closing)
		socket_watch_nonblocking[socket] = 0;
	if (watched && closing && watch_started)
	{
		unsigned round = watch_round;
		unsigned char byte = 0;
		struct timespec until;

		send(watch_wake, &byte, 1, MSG_DONTWAIT);
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += 1;
		while (watch_round == round)
		{
			if (pthread_cond_timedwait(&watch_left, &watch_lock, &until) != 0)
				break;
		}
	}
	pthread_mutex_unlock(&watch_lock);
}

/* ---------- datagrams sent in batches, by a thread of their own

A host sends each machine its datagram every tick, which was 27% of a
48-machine host's game thread: the network service's time is per datagram,
so sending them together (sendmmsg) asks it less often but takes as long.
So the thread that presents the frames (host_net_frame_end) holds the
datagrams it sends on a non-blocking datagram socket, up to DATAGRAM_BATCH
of one socket, and hands them to the sender thread, which sends each batch
with one sendmmsg while the game goes on: when that many are held, when it
sends on another socket, when it makes any other socket call, and at the
end of each frame. Batches are sent in the order handed over; closing a
socket, and asking its address (which its first send may have bound), wait
until the sender has sent what was handed over. A datagram the system has
no room for is dropped, as one the game's send was refused would have been
(they are the unreliable ones). */

struct held_sends
{
	int socket;
	int count;
	struct
	{
		socklen_t address_length;
		struct sockaddr_storage address;
		int size;
		unsigned char data[DATAGRAM_SLOT];
	} datagrams[DATAGRAM_BATCH];
};

/* the batches handed to the sender thread (a ring: the next to send, and
how many; one being sent keeps its place until it is) */
#define SEND_BATCHES 8

static __thread int frame_thread;
static __thread struct held_sends *held_sends;
static struct held_sends *send_ring;
static int send_ring_next, send_ring_count;
static pthread_mutex_t send_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t send_ready = PTHREAD_COND_INITIALIZER;
static pthread_cond_t send_done = PTHREAD_COND_INITIALIZER;
static int sender_started;

static void *sender_thread(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&send_lock);
	for (;;)
	{
		struct held_sends *batch;
		struct mmsghdr messages[DATAGRAM_BATCH];
		struct iovec vectors[DATAGRAM_BATCH];
		int index;

		while (!send_ring_count)
			pthread_cond_wait(&send_ready, &send_lock);
		batch = &send_ring[send_ring_next];
		pthread_mutex_unlock(&send_lock);
		memset(messages, 0, sizeof(messages));
		for (index = 0; index < batch->count; index++)
		{
			vectors[index].iov_base = batch->datagrams[index].data;
			vectors[index].iov_len = (size_t)batch->datagrams[index].size;
			messages[index].msg_hdr.msg_name = &batch->datagrams[index].address;
			messages[index].msg_hdr.msg_namelen = batch->datagrams[index].address_length;
			messages[index].msg_hdr.msg_iov = &vectors[index];
			messages[index].msg_hdr.msg_iovlen = 1;
		}
		sendmmsg(batch->socket, messages, (unsigned int)batch->count, MSG_NOSIGNAL);
		pthread_mutex_lock(&send_lock);
		send_ring_next = (send_ring_next + 1) % SEND_BATCHES;
		send_ring_count--;
		pthread_cond_broadcast(&send_done);
	}
	return NULL;
}

/* what is held, handed to the sender thread (sent here if it has none) */
static void sends_flush(void)
{
	int saved_errno = errno;
	struct held_sends *batch;
	int index;

	if (!held_sends || !held_sends->count)
		return;
	pthread_mutex_lock(&send_lock);
	if (!sender_started)
	{
		pthread_t thread;

		send_ring = calloc(SEND_BATCHES, sizeof(*send_ring));
		sender_started = send_ring && pthread_create(&thread, NULL, sender_thread, NULL) == 0 ? 1 : -1;
		if (sender_started > 0)
			pthread_detach(thread);
		host_logf(HOST_LOG_INFO, "sockets: %s", sender_started > 0 ?
			"datagrams are sent by a thread of their own" : "no thread to send datagrams: the game's sends them");
	}
	if (sender_started < 0)
	{
		struct mmsghdr messages[DATAGRAM_BATCH];
		struct iovec vectors[DATAGRAM_BATCH];

		pthread_mutex_unlock(&send_lock);
		memset(messages, 0, sizeof(messages));
		for (index = 0; index < held_sends->count; index++)
		{
			vectors[index].iov_base = held_sends->datagrams[index].data;
			vectors[index].iov_len = (size_t)held_sends->datagrams[index].size;
			messages[index].msg_hdr.msg_name = &held_sends->datagrams[index].address;
			messages[index].msg_hdr.msg_namelen = held_sends->datagrams[index].address_length;
			messages[index].msg_hdr.msg_iov = &vectors[index];
			messages[index].msg_hdr.msg_iovlen = 1;
		}
		sendmmsg(held_sends->socket, messages, (unsigned int)held_sends->count, MSG_NOSIGNAL);
		held_sends->count = 0;
		errno = saved_errno;
		return;
	}
	while (send_ring_count == SEND_BATCHES)
		pthread_cond_wait(&send_done, &send_lock);
	batch = &send_ring[(send_ring_next + send_ring_count) % SEND_BATCHES];
	batch->socket = held_sends->socket;
	batch->count = held_sends->count;
	for (index = 0; index < held_sends->count; index++)
	{
		batch->datagrams[index].address_length = held_sends->datagrams[index].address_length;
		batch->datagrams[index].address = held_sends->datagrams[index].address;
		batch->datagrams[index].size = held_sends->datagrams[index].size;
		memcpy(batch->datagrams[index].data, held_sends->datagrams[index].data,
			(size_t)held_sends->datagrams[index].size);
	}
	send_ring_count++;
	pthread_cond_signal(&send_ready);
	pthread_mutex_unlock(&send_lock);
	held_sends->count = 0;
	errno = saved_errno;
}

/* until the sender thread has sent all it was handed */
static void sends_wait(void)
{
	pthread_mutex_lock(&send_lock);
	while (send_ring_count)
		pthread_cond_wait(&send_done, &send_lock);
	pthread_mutex_unlock(&send_lock);
}

/* (every socket call but a send that is held sends what is held first) */
#define SENDS_FLUSH() do { if (held_sends && held_sends->count) sends_flush(); } while (0)

/* a datagram held to send with others: 1 if it was */
static int sends_hold(int socket, const void *buffer, int length, int flags, const struct sockaddr_storage *address,
	socklen_t address_length)
{
	if (!frame_thread || flags || length <= 0 || length > DATAGRAM_SLOT || !address || socket < 0 ||
		socket >= BATCHED_SOCKETS || !socket_nonblocking[socket])
	{
		return 0;
	}
	if (!held_sends)
	{
		held_sends = calloc(1, sizeof(*held_sends));
		if (!held_sends)
			return 0;
	}
	if (held_sends->count && held_sends->socket != socket)
		sends_flush();
	held_sends->socket = socket;
	held_sends->datagrams[held_sends->count].address_length = address_length;
	memcpy(&held_sends->datagrams[held_sends->count].address, address, sizeof(*address));
	held_sends->datagrams[held_sends->count].size = length;
	memcpy(held_sends->datagrams[held_sends->count].data, buffer, (size_t)length);
	if (++held_sends->count == DATAGRAM_BATCH)
		sends_flush();
	return 1;
}

/* the end of a frame (host_sdl2.c's swap): what is held is sent, and the
thread that calls this is the one whose sends are held */
void host_net_frame_end(void)
{
	frame_thread = 1;
	SENDS_FLUSH();
}

int posix_socket(int family, int type, int protocol)
{
	int result;

	SENDS_FLUSH();
	result = socket(family, type, protocol);

	sockets_opened(result, type == SOCK_STREAM ? "a TCP socket" : "a UDP socket");
	return succeed(result);
}

int posix_socket_close(int socket)
{
	int result;

	SENDS_FLUSH();
	sends_wait();
	datagrams_forget(socket);
	watch_forget(socket, 1);
	result = close(socket);
	if (result == 0)
		__atomic_sub_fetch(&sockets_open, 1, __ATOMIC_RELAXED);
	return succeed(result);
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_storage converted;
	socklen_t length = address_in(address, address_length, &converted);
	int result;

	SENDS_FLUSH();
	result = bind(socket, (struct sockaddr *)&converted, length);

	return succeed(result);
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
	SENDS_FLUSH();
	/* A non-blocking connect that is under way is EINPROGRESS here but
	WSAEWOULDBLOCK in Winsock, which is what the game waits on before it
	selects for the socket becoming writeable (connect_endpoint,
	transport_endpoint_winsock.c); as WSAEINPROGRESS it gave up at once,
	and every system link join failed, a split screen game's join of its
	own host included. */
	struct sockaddr_storage converted;
	socklen_t length = address_in(address, address_length, &converted);
	int result = connect(socket, (struct sockaddr *)&converted, length);

	if (result < 0 && errno == EINPROGRESS)
	{
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	return succeed(result);
}

int posix_socket_listen(int socket, int backlog)
{
	SENDS_FLUSH();
	return succeed(listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_storage converted;
	socklen_t length = sizeof(converted);
	int result;

	SENDS_FLUSH();
	result = accept(socket, (struct sockaddr *)&converted, &length);

	if (result >= 0)
		address_out(&converted, length, address, address_length);
	sockets_opened(result, "a connection accepted");
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
	SENDS_FLUSH();
	return succeed((int)send(socket, buffer, (size_t)length, flags | MSG_NOSIGNAL));
}

int posix_socket_sendto(int socket, const void *buffer, int length, int flags,
	const void *address, int address_length)
{
	struct sockaddr_storage converted;
	socklen_t converted_length = address_in(address, address_length, &converted);
	int broadcast = 0;
	int result;

	if (address && converted.ss_family == AF_INET &&
		((struct sockaddr_in *)&converted)->sin_addr.s_addr == htonl(INADDR_BROADCAST))
	{
		uint32_t directed = directed_broadcast();

		broadcast = 1;
		if (directed)
			((struct sockaddr_in *)&converted)->sin_addr.s_addr = directed;
	}
	if (!broadcast && address && sends_hold(socket, buffer, length, flags, &converted, converted_length))
	{
		last_error = 0;
		return length;
	}
	SENDS_FLUSH();
	result = (int)sendto(socket, buffer, (size_t)length, flags | MSG_NOSIGNAL,
		address ? (struct sockaddr *)&converted : NULL, converted_length);
	return succeed(result);
}

/* the receives that reach the socket service (each an IPC; a batched datagram does not), counted and said every 5 s
while there are any: how many, how many found nothing, and the bytes, for the game thread's time in them */
static struct
{
	unsigned calls, empty, answered;
	unsigned long long bytes;
	uint64_t since;
} receives;

static void receive_counted(int result)
{
	struct timespec now;
	uint64_t now_ms;

	receives.calls++;
	if (result < 0)
		receives.empty++;
	else
		receives.bytes += (unsigned)result;
	clock_gettime(CLOCK_MONOTONIC, &now);
	now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
	if (!receives.since)
		receives.since = now_ms;
	else if (now_ms - receives.since >= 5000)
	{
		host_logf(HOST_LOG_INFO, "sockets: %u receives in %.1f s (%u found nothing), %llu KB; %u more answered as empty "
			"by the watch", receives.calls, (double)(now_ms - receives.since) / 1000.0, receives.empty,
			receives.bytes / 1024, receives.answered);
		receives.calls = receives.empty = receives.answered = 0;
		receives.bytes = 0;
		receives.since = now_ms;
	}
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	int truncated = 0;
	int batched;
	int result;

	SENDS_FLUSH();
	if (watch_says_empty(socket, flags))
	{
		receives.answered++;
		last_error = WSAEWOULDBLOCK;
		return -1;
	}
	result = datagram_take(socket, buffer, length, flags, NULL, NULL, &truncated, &batched);

	if (batched)
	{
		if (result < 0)
			watch_empty(socket, flags);
		if (result >= 0 && truncated)
		{
			last_error = WSAEMSGSIZE;
			return -1;
		}
		return succeed(result);
	}
	result = (int)recv(socket, buffer, (size_t)length, flags);
	receive_counted(result);
	if (result < 0)
		watch_empty(socket, flags);
	return succeed(result);
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct iovec vector;
	struct msghdr message;
	struct sockaddr_storage converted;
	int result;
	int truncated = 0;
	int batched;

	SENDS_FLUSH();
	if (watch_says_empty(socket, flags))
	{
		receives.answered++;
		last_error = WSAEWOULDBLOCK;
		return -1;
	}

	{
		socklen_t converted_length = sizeof(converted);

		result = datagram_take(socket, buffer, length, flags, &converted, &converted_length, &truncated, &batched);
		if (batched)
		{
			if (result < 0)
				watch_empty(socket, flags);
			if (result >= 0 && address && address_length)
				address_out(&converted, converted_length, address, address_length);
			if (result >= 0 && truncated)
			{
				last_error = WSAEMSGSIZE;
				return -1;
			}
			return succeed(result);
		}
	}
	vector.iov_base = buffer;
	vector.iov_len = (size_t)length;
	memset(&message, 0, sizeof(message));
	message.msg_name = address && address_length ? &converted : NULL;
	message.msg_namelen = address && address_length ? sizeof(converted) : 0;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	result = (int)recvmsg(socket, &message, flags);
	receive_counted(result);
	if (result < 0)
		watch_empty(socket, flags);
	if (result >= 0 && address && address_length)
		address_out(&converted, message.msg_namelen, address, address_length);
	/* a datagram larger than the buffer: both give its start, but Winsock
	with WSAEMSGSIZE, which the game takes as an error, not as the datagram */
	if (result >= 0 && (message.msg_flags & MSG_TRUNC))
	{
		last_error = WSAEMSGSIZE;
		return -1;
	}
	return succeed(result);
}

int posix_socket_shutdown(int socket, int how)
{
	SENDS_FLUSH();
	return succeed(shutdown(socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int flags;

	SENDS_FLUSH();
	flags = fcntl(socket, F_GETFL);

	if (flags < 0)
		return fail();
	flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	if (fcntl(socket, F_SETFL, flags) < 0)
		return fail();
	/* (a non-blocking datagram socket's datagrams are read in batches) */
	if (socket >= 0 && socket < BATCHED_SOCKETS)
	{
		int type = 0;
		socklen_t type_length = sizeof(type);

		if (nonblocking && getsockopt(socket, SOL_SOCKET, SO_TYPE, &type, &type_length) < 0)
			type = 0;
		pthread_mutex_lock(&datagram_lock);
		socket_nonblocking[socket] = nonblocking && type == SOCK_DGRAM;
		pthread_mutex_unlock(&datagram_lock);
		/* (and an empty one is watched, if it is non-blocking) */
		if (!nonblocking)
			watch_forget(socket, 0);
		pthread_mutex_lock(&watch_lock);
		socket_watch_nonblocking[socket] = (unsigned char)(nonblocking != 0);
		pthread_mutex_unlock(&watch_lock);
	}
	return succeed(0);
}

int posix_socket_set_nodelay(int socket)
{
	int value = 1;

	SENDS_FLUSH();

	return succeed(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result;
	int held;

	SENDS_FLUSH();
	result = ioctl(socket, FIONREAD, &available);

	/* (and the datagrams held: the next one's size, as FIONREAD gives) */
	pthread_mutex_lock(&datagram_lock);
	held = datagrams_held(socket);
	if (held)
	{
		struct datagram_queue *queue = datagram_queues[socket];

		available = queue->datagrams[queue->next].size;
		result = 0;
	}
	pthread_mutex_unlock(&datagram_lock);
	if (result >= 0)
		*count = (posix_ulong)available;
	return succeed(result);
}

static int translate_option(int level, int name, int *host_level, int *host_name)
{
	if (level != WINSOCK_SOL_SOCKET)
	{
		/* IPPROTO_IP / IPPROTO_TCP option numbers are shared */
		*host_level = level;
		*host_name = name;
		return 0;
	}
	*host_level = SOL_SOCKET;
	switch (name)
	{
	case WINSOCK_SO_REUSEADDR: *host_name = SO_REUSEADDR; return 0;
	case WINSOCK_SO_KEEPALIVE: *host_name = SO_KEEPALIVE; return 0;
	case WINSOCK_SO_BROADCAST: *host_name = SO_BROADCAST; return 0;
	case WINSOCK_SO_LINGER: *host_name = SO_LINGER; return 0;
	case WINSOCK_SO_SNDBUF: *host_name = SO_SNDBUF; return 0;
	case WINSOCK_SO_RCVBUF: *host_name = SO_RCVBUF; return 0;
	case WINSOCK_SO_ERROR: *host_name = SO_ERROR; return 0;
	case WINSOCK_SO_TYPE: *host_name = SO_TYPE; return 0;
	default: return -1;
	}
}

int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{
	int host_level, host_name;

	SENDS_FLUSH();

	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		/* Xbox-only options such as SO_ENCRYPT have nothing to do here */
		last_error = 0;
		return 0;
	}
	/* The game asks for at least 1 MB each way on its sockets
	(transport_endpoint_winsock.c), as internet play does on its tunnel
	(p2p.c), sized for hosting 128 players. Here those limits are counted
	against the socket budget (fail(), above) when they are set, so two
	sockets granted that much leave too little for the next one: joining
	from the list opened the signalling brokers' connections as well, and
	the game's own client then could not make its sockets. A joiner's
	traffic is the host's to one machine, and a host's to each machine is
	less, so the sizes are held to libnx's default largest TCP buffer. */
	if (host_level == SOL_SOCKET && (host_name == SO_SNDBUF || host_name == SO_RCVBUF) &&
		value && length >= (int)sizeof(int) && *(const int *)value > SOCKET_BUFFER_LIMIT)
	{
		int limited = SOCKET_BUFFER_LIMIT;

		return succeed(setsockopt(socket, host_level, host_name, &limited, sizeof(limited)));
	}
	return succeed(setsockopt(socket, host_level, host_name, value, (socklen_t)length));
}

int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{
	int host_level, host_name;
	socklen_t socket_length = (socklen_t)*length;
	int result;

	SENDS_FLUSH();
	if (translate_option(level, name, &host_level, &host_name) != 0)
	{
		last_error = WSAENOPROTOOPT;
		return -1;
	}
	result = getsockopt(socket, host_level, host_name, value, &socket_length);
	*length = (int)socket_length;
	return succeed(result);
}

int posix_socket_getsockname(int socket, void *address, int *address_length)
{
	struct sockaddr_storage converted;
	socklen_t length = sizeof(converted);
	int result;

	SENDS_FLUSH();
	sends_wait();
	result = getsockname(socket, (struct sockaddr *)&converted, &length);

	if (result >= 0)
		address_out(&converted, length, address, address_length);
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_storage converted;
	socklen_t length = sizeof(converted);
	int result;

	SENDS_FLUSH();
	result = getpeername(socket, (struct sockaddr *)&converted, &length);

	if (result >= 0)
		address_out(&converted, length, address, address_length);
	return succeed(result);
}

int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	/* poll, which takes any descriptor (select none from FD_SETSIZE on,
	which a process allowed more files has), with select's readiness: read
	for data, the end or an error, write for room or an error, error for
	urgent data or (as Winsock's) a connect that failed */
	static const short events[3] = { POLLIN, POLLOUT, POLLPRI };
	static const short ready[3] = { POLLIN | POLLHUP | POLLERR, POLLOUT | POLLERR, POLLPRI | POLLERR };
	/* (larger sets, as internet play's thread waits on, in a buffer each
	thread keeps: not one allocation each time) */
	static __thread struct pollfd *buffer;
	static __thread int buffer_size;
	int *lists[3] = { read, write, error };
	int *counts[3] = { read_count, write_count, error_count };
	struct pollfd stack[256];
	struct pollfd *descriptors = stack;
	long long milliseconds = (long long)timeout_seconds * 1000 + ((long long)timeout_microseconds + 999) / 1000;
	int total = 0;
	int list, index;
	int result;

	SENDS_FLUSH();
	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			lists[list] = NULL;
		else
			total += *counts[list];
	}
	/* (a look at once for reading only, at sockets all watched and seen empty: none ready, asking nothing - the
	host's look at its machines' connections each frame) */
	if (!infinite && milliseconds <= 0 && lists[0] && *counts[0] > 0 && total == *counts[0])
	{
		int empty = 1;

		for (index = 0; index < *counts[0] && empty; index++)
			empty = watch_knows_empty(lists[0][index]);
		if (empty)
		{
			pthread_mutex_lock(&datagram_lock);
			for (index = 0; index < *counts[0] && empty; index++)
				empty = !datagrams_held(lists[0][index]);
			pthread_mutex_unlock(&datagram_lock);
		}
		if (empty)
		{
			receives.answered++;
			*counts[0] = 0;
			last_error = 0;
			return 0;
		}
	}
	if (total > (int)(sizeof(stack) / sizeof(*stack)))
	{
		if (total > buffer_size)
		{
			struct pollfd *larger = realloc(buffer, sizeof(*buffer) * (size_t)total);

			if (!larger)
			{
				errno = ENOMEM;
				return fail();
			}
			buffer = larger;
			buffer_size = total;
		}
		descriptors = buffer;
	}
	total = 0;
	for (list = 0; list < 3; list++)
	{
		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			descriptors[total].fd = lists[list][index];
			descriptors[total].events = events[list];
			descriptors[total].revents = 0;
		}
	}
	/* (a socket with datagrams held is ready to read: no wait then) */
	{
		int held = 0;

		pthread_mutex_lock(&datagram_lock);
		for (index = 0; lists[0] && index < *counts[0]; index++)
		{
			if (datagrams_held(lists[0][index]))
				held = 1;
		}
		pthread_mutex_unlock(&datagram_lock);
		if (held)
		{
			infinite = 0;
			milliseconds = 0;
		}
	}
	result = poll(descriptors, (nfds_t)total, infinite ? -1 :
		(int)(milliseconds < 0 ? 0 : milliseconds > INT_MAX ? INT_MAX : milliseconds));
	if (result >= 0)
	{
		pthread_mutex_lock(&datagram_lock);
		for (index = 0; lists[0] && index < *counts[0]; index++)
		{
			if (datagrams_held(lists[0][index]) && !(descriptors[index].revents & POLLIN))
			{
				descriptors[index].revents |= POLLIN;
				result++;
			}
		}
		pthread_mutex_unlock(&datagram_lock);
	}
	for (index = 0; index < total && result > 0; index++)
	{
		/* (as select fails on a descriptor that is not open) */
		if (descriptors[index].revents & POLLNVAL)
		{
			errno = EBADF;
			result = -1;
		}
	}
	if (result < 0)
		return fail();
	/* (a look at once that found a socket not readable: watched from now on, for the next look) */
	if (!infinite && milliseconds <= 0 && lists[0])
	{
		for (index = 0; index < *counts[0]; index++)
		{
			int descriptor = lists[0][index];

			if (!(descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)) && descriptor >= 0 &&
				descriptor < BATCHED_SOCKETS && !__atomic_load_n(&socket_watched[descriptor], __ATOMIC_RELAXED))
				watch_add(descriptor, 0);
		}
	}
	result = 0;
	total = 0;
	for (list = 0; list < 3; list++)
	{
		int kept = 0;

		for (index = 0; lists[list] && index < *counts[list]; index++, total++)
		{
			int descriptor = lists[list][index];
			int pending = 0;
			socklen_t length = sizeof(pending);

			if (!(descriptors[total].revents & ready[list]))
				continue;
			/* Winsock reports a socket writeable once its connect has
			succeeded; one whose connect failed is not (it is in the error
			set), where POSIX reports it writeable with the failure in
			SO_ERROR. The game takes writeable as connected
			(connect_endpoint). */
			if (list == 1 && getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &pending, &length) == 0 && pending)
			{
				errno = pending;
				fail();
				continue;
			}
			lists[list][kept++] = descriptor;
		}
		if (lists[list])
			*counts[list] = kept;
		result += kept;
	}
	/* like Winsock, a select with nothing ready leaves the last error as it
	was: after a connect under way, still WSAEWOULDBLOCK, which the game
	reads as not connected yet */
	if (result > 0)
		last_error = 0;
	return result;
}

posix_ulong posix_local_ipv4_address(void)
{
	struct sockaddr_in route;
	socklen_t length = sizeof(route);
	posix_ulong result = 0;
	int probe;

	/* the address the default route leaves from: a UDP socket "connected"
	to an internet address (a documentation one; nothing is sent) has it */
	probe = socket(AF_INET, SOCK_DGRAM, 0);
	if (probe >= 0)
	{
		memset(&route, 0, sizeof(route));
		route.sin_family = AF_INET;
		route.sin_port = htons(9);
		route.sin_addr.s_addr = htonl(0xC6336401);
		if (connect(probe, (struct sockaddr *)&route, sizeof(route)) == 0 &&
			getsockname(probe, (struct sockaddr *)&route, &length) == 0 &&
			route.sin_addr.s_addr != htonl(INADDR_ANY) && (ntohl(route.sin_addr.s_addr) >> 24) != 127)
		{
			result = route.sin_addr.s_addr;
		}
		close(probe);
	}
	/* no route out: the console's own idea of its address */
	if (!result)
	{
		long id = gethostid();

		if (id && id != -1 && (ntohl((uint32_t)id) >> 24) != 127)
			result = (posix_ulong)(uint32_t)id;
	}
	return result;
}

posix_ulong posix_resolve_ipv4(const char *host)
{
	struct addrinfo hints, *results;
	posix_ulong address = 0;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(host, NULL, &hints, &results) != 0)
		return 0;
	if (results && results->ai_addr && results->ai_addr->sa_family == AF_INET)
		address = ((struct sockaddr_in *)results->ai_addr)->sin_addr.s_addr;
	freeaddrinfo(results);
	return address;
}

/* ---------- UPnP

posix_upnp.c's, with miniupnpc (tools/switch_build.py). libnx has neither
of these, which miniupnpc names for an IPv6 address's scope: the console's
network is IPv4's, which has none. */

unsigned int switch_upnp_if_nametoindex(const char *name)
{
	(void)name;
	return 0;
}

char *switch_upnp_if_indextoname(unsigned int index, char *name)
{
	(void)index;
	(void)name;
	return NULL;
}

/* ---------- the Discord desktop client */

int posix_discord_connect(void)
{
	return -1;
}

int posix_discord_write(int handle, const void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

int posix_discord_read(int handle, void *buffer, int length)
{
	(void)handle;
	(void)buffer;
	(void)length;
	return -1;
}

void posix_discord_close(int handle)
{
	(void)handle;
}

/* ---------- four that are not about the network, but live in posix_net.c
 * beside the ones that are, and so came out with it.

Three of them exist for the desktop's benefit: a command line to read
arguments from, a per-user secret file in a runtime directory, and a desktop
entry registering a URL scheme. None of the three has anything to offer a
console: the game's menus join internet games themselves. The fourth is the process's own id, which the network code
used and the game does not, but the import table asks for it either way. */

posix_ulong posix_process_id(void)
{
	/* newlib's getpid is an unimplemented stub on this console and
	 * returns -1; the guest's one process is answered the way the
	 * syscall shim answers it */
	return 1;
}

int posix_command_line_argument(int index, char *buffer, posix_ulong size)
{
	/* the guest is started by the loader with its own boot structure, whose
	argv it builds in host_main.c; it has no command line of its own. Internet
	games are joined from the game's own menus, so there is no invite to pass
	either. */
	(void)index;
	(void)buffer;
	(void)size;
	return 0;
}

int posix_user_secret(unsigned char *secret, int size)
{
	/* a console has no user and no runtime directory, and a secret of any
	sort would have nowhere secret to live: the SD card is readable by
	whatever the console does with it. Filling it with entropy from
	posix_random_bytes is the honest answer - unpredictable, but not
	private - rather than leaving it zeroed, which would be neither. */
	if (secret && size > 0)
		posix_random_bytes(secret, (posix_ulong)size);
	return 0;
}

int posix_register_url_scheme(const char *scheme, const char *description)
{
	/* there is nothing to register a scheme with */
	(void)scheme;
	(void)description;
	return 0;
}

/* ---------- randomness

The console's own generator (the csrng service): the game asks for
cryptographic randomness for its network nonces and secrets. A seeded
xorshift stands in only if the service cannot be reached. */

void posix_random_bytes(void *buffer, posix_ulong size)
{
	static int csrng_ready = -1;
	unsigned char *bytes = buffer;
	static unsigned long state;
	posix_ulong index;

	if (csrng_ready < 0)
		csrng_ready = R_SUCCEEDED(csrngInitialize()) ? 1 : 0;
	if (csrng_ready && R_SUCCEEDED(csrngGetRandomBytes(buffer, (size_t)size)))
		return;
	if (!state)
		state = (unsigned long)time(NULL) ^ (unsigned long)(uintptr_t)buffer;
	for (index = 0; index < size; index++)
	{
		unsigned long value;

		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		value = state * 2685821657736338717UL;
		bytes[index] = (unsigned char)(value >> 56);
	}
}

/* ---------- internet play with browsers (port/linux/src/p2p_webrtc.c)

The native builds hold WebRTC connections with browsers through DTLS
(port/linux/src/posix_dtls.c, with Mbed TLS), which the Switch's host does not
build: it has no certificate, so it says so (its machine is signalled as one
without WebRTC, and no browser connects to it), and the rest are never asked.
Games with other native machines do not use them. STUN's HMAC-SHA-1 is libnx's. */

int posix_dtls_fingerprint(unsigned char *fingerprint)
{
	(void)fingerprint;
	return 0;
}

int posix_dtls_open(void)
{
	return -1;
}

void posix_dtls_close(int handle)
{
	(void)handle;
}

void posix_dtls_input(int handle, const void *data, int size)
{
	(void)handle;
	(void)data;
	(void)size;
}

int posix_dtls_receive(int handle, posix_ulong now, void *buffer, int size)
{
	(void)handle;
	(void)now;
	(void)buffer;
	(void)size;
	return -1;
}

int posix_dtls_send(int handle, const void *data, int size)
{
	(void)handle;
	(void)data;
	(void)size;
	return 0;
}

int posix_dtls_output(int handle, void *buffer, int size)
{
	(void)handle;
	(void)buffer;
	(void)size;
	return 0;
}

int posix_dtls_peer_fingerprint(int handle, unsigned char *fingerprint)
{
	(void)handle;
	(void)fingerprint;
	return 0;
}

void posix_hmac_sha1(const void *key, int key_size, const void *data, int size, unsigned char *digest)
{
	hmacSha1CalculateMac(digest, key, (size_t)key_size, data, (size_t)size);
}
