/* net.c — I/O socket cấp thấp: đọc/ghi đầy đủ, listen loopback, accept, connect TCP. */
#include "rc_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

rc_status rc_net_read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n == 0) return RC_ERR_CONNECT; /* peer đóng */
        if (n < 0) {
            if (errno == EINTR) continue;
            return RC_ERR_CONNECT;
        }
        p += n;
        len -= (size_t)n;
    }
    return RC_OK;
}

rc_status rc_net_write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return RC_ERR_CONNECT;
        }
        p += n;
        len -= (size_t)n;
    }
    return RC_OK;
}

static void set_low_latency(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

int rc_net_listen_loopback(int *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; /* để kernel chọn cổng trống */

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 2) < 0) {
        close(fd);
        return -1;
    }

    socklen_t sl = sizeof addr;
    if (getsockname(fd, (struct sockaddr *)&addr, &sl) < 0) {
        close(fd);
        return -1;
    }
    if (out_port) *out_port = ntohs(addr.sin_port);
    return fd;
}

int rc_net_accept(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd >= 0) set_low_latency(fd);
    return fd;
}

/* Connect non-blocking một địa chỉ, chờ tối đa timeout_ms (poll POLLOUT + SO_ERROR).
 * Không có timeout thì mạng drop gói (firewall/NAT) khiến connect() treo hàng phút. */
static int connect_addr_timeout(const struct addrinfo *ai, int timeout_ms) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) return -1;
    /* Non-blocking qua fcntl (SOCK_NONBLOCK là mở rộng Linux, không có trên macOS/BSD). */
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc < 0) {
        if (errno != EINPROGRESS) goto fail;
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        if (poll(&pfd, 1, timeout_ms) <= 0) goto fail; /* timeout hoặc lỗi poll */
        int err = 0;
        socklen_t sl = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sl) < 0 || err != 0) goto fail;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    return fd;
fail:
    close(fd);
    return -1;
}

/* Connect TCP tới host (IP literal hoặc hostname — getaddrinfo, thử lần lượt từng địa chỉ),
 * mỗi địa chỉ chờ tối đa timeout_ms (<=0 = 5000ms mặc định). */
int rc_net_connect_tcp(const char *host, int port, int timeout_ms) {
    if (timeout_ms <= 0) timeout_ms = 5000;
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next)
        fd = connect_addr_timeout(ai, timeout_ms);
    freeaddrinfo(res);
    if (fd >= 0) set_low_latency(fd);
    return fd;
}
