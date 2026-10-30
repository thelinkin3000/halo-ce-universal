/* The runner extracts the production batching code into under_test.inc. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <time.h>
/* Host-independent stand-in for the libnx recvmmsg ABI. */
struct test_mmsghdr { struct msghdr msg_hdr; unsigned int msg_len; };
#define mmsghdr test_mmsghdr
static unsigned lengths[2];
static int calls;
static int fake_recvmmsg(int fd, struct mmsghdr *messages, unsigned count, int flags, struct timespec *timeout)
{
    unsigned i;
    (void)fd; (void)flags; (void)timeout;
    assert(count >= 2);
    calls++;
    for (i = 0; i < 2; i++) {
        struct iovec *v = messages[i].msg_hdr.msg_iov;
        size_t n = lengths[i] < v->iov_len ? lengths[i] : v->iov_len;
        memset(v->iov_base, 'a' + (int)i, n);
        messages[i].msg_len = (unsigned)n;
        messages[i].msg_hdr.msg_flags = lengths[i] > n ? MSG_TRUNC : 0;
    }
    return 2;
}
#define recvmmsg fake_recvmmsg
#include "under_test.inc"
static void run(unsigned first, unsigned second, int first_buffer, int second_buffer,
                int first_truncated, int second_truncated)
{
    unsigned char buffer[DATAGRAM_SLOT];
    int result, truncated, batched, i;
    lengths[0] = first; lengths[1] = second; calls = 0;
    socket_nonblocking[3] = 1;
    result = datagram_take(3, buffer, first_buffer, 0, NULL, NULL, &truncated, &batched);
    assert(batched && calls == 1 && result == (int)(first < (unsigned)first_buffer ? first : (unsigned)first_buffer));
    assert(!!truncated == first_truncated);
    for (i = 0; i < result; i++) assert(buffer[i] == 'a');
    assert(datagrams_held(3) == 1);
    result = datagram_take(3, buffer, second_buffer, 0, NULL, NULL, &truncated, &batched);
    assert(batched && calls == 1 && result == (int)(second < (unsigned)second_buffer ? second : (unsigned)second_buffer));
    assert(!!truncated == second_truncated);
    for (i = 0; i < result; i++) assert(buffer[i] == 'b');
    assert(datagrams_held(3) == 0);
    datagrams_forget(3);
}
int main(void)
{
    run(8, 1500, 8, 1500, 0, 0);     /* A small read must not damage the next datagram. */
    run(1500, 1200, 1500, 8, 0, 1);  /* Truncation belongs to the individual read. */
    run(8, 2048, 8, 2048, 0, 0);     /* Exact slot boundary. */
    run(8, 2049, 8, 2048, 0, 1);     /* Kernel truncation still reported. */
    run(100, 40, 8, 40, 1, 0);       /* A truncated read consumes only its own packet. */
    run(0, 0, 8, 8, 0, 0);          /* Empty UDP packets retain their order. */
    puts("PASS: six batched datagram size/order cases");
    return 0;
}
