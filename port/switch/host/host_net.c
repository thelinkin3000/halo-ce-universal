/*
HOST_NET.C

The game's network calls (port/linux/src/posix.h) on the Switch.

libnx has BSD sockets over the console's bsd service - socket(), poll(),
getaddrinfo() and the rest, with their headers in libnx's own include
directory rather than newlib's, which is how an earlier version of this
file came to believe there were none and answered every call with failure.
host_main.c initialises them (socketInitializeDefault) at startup.

The socket calls are posix_net.c's, which turns POSIX results into the
Winsock ones the game expects, with three differences: no SOCK_CLOEXEC
(nothing is ever exec'd here), accept rather than accept4, and the local
address found only by the route a UDP socket would take (there is no
getifaddrs). posix_net.c itself is not compiled: the rest of it is the
desktop's - command lines from /proc, a runtime directory, URL schemes,
Discord's local socket - and UPnP (posix_upnp.c) needs miniupnpc, which
devkitPro does not package. Those are answered below as having nothing to
offer.
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
times the largest TCP and UDP buffers, about 2.2 MB). Running out of it is
WSAENOBUFS to the game, which gives up whatever it was making - the first
internet join failed that way, on creating its client - so it is always
said. It is rare by nature, unlike the per-datagram failures, so saying it
costs nothing in an ordinary run. */
#define BUFFER_FAILURES_LOGGED 20
/* the most a socket's buffer is set to (posix_socket_setsockopt): libnx's
default tcp_tx_buf_max_size and tcp_rx_buf_max_size */
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

int posix_socket(int family, int type, int protocol)
{
	return succeed(socket(family, type, protocol));
}

int posix_socket_close(int socket)
{
	return succeed(close(socket));
}

int posix_socket_bind(int socket, const void *address, int address_length)
{
	struct sockaddr_storage converted;
	socklen_t length = address_in(address, address_length, &converted);

	int result = bind(socket, (struct sockaddr *)&converted, length);

	return succeed(result);
}

int posix_socket_connect(int socket, const void *address, int address_length)
{
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
	return succeed(listen(socket, backlog));
}

int posix_socket_accept(int socket, void *address, int *address_length)
{
	struct sockaddr_storage converted;
	socklen_t length = sizeof(converted);
	int result = accept(socket, (struct sockaddr *)&converted, &length);

	if (result >= 0)
		address_out(&converted, length, address, address_length);
	return succeed(result);
}

int posix_socket_send(int socket, const void *buffer, int length, int flags)
{
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
	result = (int)sendto(socket, buffer, (size_t)length, flags | MSG_NOSIGNAL,
		address ? (struct sockaddr *)&converted : NULL, converted_length);
	return succeed(result);
}

int posix_socket_recv(int socket, void *buffer, int length, int flags)
{
	return succeed((int)recv(socket, buffer, (size_t)length, flags));
}

int posix_socket_recvfrom(int socket, void *buffer, int length, int flags,
	void *address, int *address_length)
{
	struct iovec vector;
	struct msghdr message;
	struct sockaddr_storage converted;
	int result;

	vector.iov_base = buffer;
	vector.iov_len = (size_t)length;
	memset(&message, 0, sizeof(message));
	message.msg_name = address && address_length ? &converted : NULL;
	message.msg_namelen = address && address_length ? sizeof(converted) : 0;
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	result = (int)recvmsg(socket, &message, flags);
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
	return succeed(shutdown(socket, how));
}

int posix_socket_set_nonblocking(int socket, int nonblocking)
{
	int flags = fcntl(socket, F_GETFL);

	if (flags < 0)
		return fail();
	flags = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	return succeed(fcntl(socket, F_SETFL, flags));
}

int posix_socket_set_nodelay(int socket)
{
	int value = 1;

	return succeed(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
}

int posix_socket_bytes_available(int socket, posix_ulong *count)
{
	int available = 0;
	int result = ioctl(socket, FIONREAD, &available);

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
	the game's own client then could not make its sockets. The console only
	joins, and a joiner's traffic is the host's to one machine, so the
	sizes are held to libnx's largest TCP buffer. */
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
	int result = getsockname(socket, (struct sockaddr *)&converted, &length);

	if (result >= 0)
		address_out(&converted, length, address, address_length);
	return succeed(result);
}

int posix_socket_getpeername(int socket, void *address, int *address_length)
{
	struct sockaddr_storage converted;
	socklen_t length = sizeof(converted);
	int result = getpeername(socket, (struct sockaddr *)&converted, &length);

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

	for (list = 0; list < 3; list++)
	{
		if (!lists[list] || !counts[list])
			lists[list] = NULL;
		else
			total += *counts[list];
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
	result = poll(descriptors, (nfds_t)total, infinite ? -1 :
		(int)(milliseconds < 0 ? 0 : milliseconds > INT_MAX ? INT_MAX : milliseconds));
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

/* ---------- UPnP */

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port;
	(void)preferred_port;
	(void)external_address;
	(void)external_port;
	if (error && error_size > 0)
		snprintf(error, (size_t)error_size, "UPnP is not available on the Switch; forward the port on the router");
	return 0;
}

void posix_upnp_stop_forwarding_udp(unsigned short external_port)
{
	(void)external_port;
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
