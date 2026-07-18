/*
 * net.c — socket helper + relay engine: bơm 2 chiều một kết nối, và mở upstream cho cổng stream
 * (replay token + chờ guest thật sự listen). Xem Group/Listener/Conn trong agent.h.
 */
#include "agent.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Connect non-blocking có timeout (đích không phản hồi thì connect() thường treo hàng phút). */
int tcp_connect_timeout(const char *host, int port, int timeout_ms) {
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            int failed = 1;
            if (errno == EINPROGRESS) {
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                int err = 0;
                socklen_t sl = sizeof err;
                if (poll(&p, 1, timeout_ms) > 0 &&
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sl) == 0 && err == 0)
                    failed = 0;
            }
            if (failed) {
                close(fd);
                fd = -1;
                continue;
            }
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    }
    return fd;
}

/* Bind vào một IPv4 cụ thể (IP LAN/Tailscale/loopback của máy này), không phải INADDR_ANY. */
int tcp_listen_addr(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Bơm 2 chiều tới khi một đầu đóng/lỗi. Blocking read sau POLLIN + write_full — backpressure
 * tự nhiên, mỗi kết nối một thread riêng nên không chặn ai khác. Đếm byte mỗi chiều để log
 * (a2b = đọc từ a ghi sang b, b2a ngược lại). */
static void pump(int a, int b, uint64_t *a2b, uint64_t *b2a) {
    uint8_t buf[65536];
    struct pollfd p[2] = {{.fd = a, .events = POLLIN}, {.fd = b, .events = POLLIN}};
    for (;;) {
        int r = poll(p, 2, 1000);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 || g_stop) break;
        if (r == 0) continue;
        int done = 0;
        for (int i = 0; i < 2 && !done; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(p[i].fd, buf, sizeof buf);
            if (n <= 0 || write_full(i == 0 ? b : a, buf, (size_t)n) < 0) {
                done = 1;
            } else {
                *(i == 0 ? a2b : b2a) += (uint64_t)n;
            }
        }
        if (done) break;
    }
}

/* Mở upstream cho kết nối stream: connect dst_host:dst, ghi lại những gì client gửi (token)
 * vào pre[] để replay; upstream "xác nhận" khi có dữ liệu trả về hoặc giữ im lặng 300ms
 * (guest chưa listen thì adb đóng trong vài ms). EOF sớm → reconnect + replay, tối đa 10s. */
static int stream_open_upstream(Conn *c, uint8_t *pre, size_t *pre_len) {
    const int64_t t_start = now_ms();
    const int64_t deadline = t_start + 10000;
    int tries = 0, conn_fail = 0, early_eof = 0;
    for (;;) {
        tries++;
        int up = tcp_connect_timeout(c->dst_host, c->dst_port, 3000);
        if (up < 0) {
            conn_fail++;
            if (g_stop || now_ms() >= deadline) {
                logts("stream :%d ← %s: BỎ CUỘC sau %d lần / %dms — upstream %s:%d không "
                      "connect được (adb forward chết?)",
                      c->pub_port, c->peer, tries, (int)(now_ms() - t_start), c->dst_host,
                      c->dst_port);
                return -1;
            }
            sleep_ms(200);
            continue;
        }
        if (*pre_len && write_full(up, pre, *pre_len) < 0) {
            close(up);
            continue;
        }
        const int64_t t0 = now_ms();
        int verdict = 0; /* 0 đang chờ, 1 xác nhận, -1 upstream rớt, -2 client bỏ cuộc */
        while (verdict == 0) {
            int64_t quiet = 300 - (now_ms() - t0);
            if (quiet <= 0) {
                verdict = 1;
                break;
            }
            struct pollfd p[2] = {{.fd = up, .events = POLLIN},
                                  {.fd = c->client, .events = POLLIN}};
            int r = poll(p, 2, (int)quiet);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) verdict = -2;
            if (r == 0) verdict = 1; /* im lặng đủ lâu = guest đã nhận kết nối */
            if (verdict) break;
            if (p[0].revents) {
                uint8_t b[4096];
                ssize_t n = read(up, b, sizeof b);
                if (n > 0)
                    verdict = write_full(c->client, b, (size_t)n) == 0 ? 1 : -2;
                else
                    verdict = -1; /* adb đóng: guest chưa listen */
            } else if (p[1].revents) {
                uint8_t b[1024];
                ssize_t n = read(c->client, b, sizeof b);
                if (n <= 0 || *pre_len + (size_t)n > PRE_CAP || write_full(up, b, (size_t)n) < 0) {
                    verdict = -2; /* client đóng / quá nhiều dữ liệu để replay */
                } else {
                    memcpy(pre + *pre_len, b, (size_t)n);
                    *pre_len += (size_t)n;
                }
            }
        }
        if (verdict == 1) {
            logts("stream :%d ← %s: upstream %s:%d thông (lần %d, %dms; guest chưa listen %d "
                  "lần, connect fail %d lần)",
                  c->pub_port, c->peer, c->dst_host, c->dst_port, tries, (int)(now_ms() - t_start),
                  early_eof, conn_fail);
            return up;
        }
        close(up);
        if (verdict == -1) early_eof++;
        if (verdict == -2 || g_stop || now_ms() >= deadline) {
            logts("stream :%d ← %s: BỎ CUỘC sau %d lần / %dms (%s)", c->pub_port, c->peer, tries,
                  (int)(now_ms() - t_start),
                  verdict == -2 ? "client đóng trước / dữ liệu replay quá lớn"
                                : "hết 10s — guest không listen (server chưa deploy tới?)");
            return -1;
        }
        sleep_ms(150);
    }
}

void *conn_thread(void *arg) {
    Conn *c = arg;
    const int64_t t0 = now_ms();
    logts("relay :%d ← %s → %s:%d (%s) mở", c->pub_port, c->peer, c->dst_host, c->dst_port,
          c->group ? "stream" : "adb");
    int up;
    if (!c->group) {
        up = tcp_connect_timeout(c->dst_host, c->dst_port, 3000);
        if (up < 0)
            logts("relay :%d ← %s: upstream %s:%d không connect được", c->pub_port, c->peer,
                  c->dst_host, c->dst_port);
    } else {
        pthread_mutex_lock(&c->group->mu);
        while (c->group->busy)
            pthread_cond_wait(&c->group->cv, &c->group->mu);
        c->group->busy = 1;
        pthread_mutex_unlock(&c->group->mu);
        if (now_ms() - t0 > 50)
            logts("stream :%d ← %s: đã chờ %dms cho kết nối trước cùng cổng xong", c->pub_port,
                  c->peer, (int)(now_ms() - t0));

        uint8_t pre[PRE_CAP];
        size_t pre_len = 0;
        up = stream_open_upstream(c, pre, &pre_len);

        pthread_mutex_lock(&c->group->mu);
        c->group->busy = 0;
        pthread_cond_broadcast(&c->group->cv);
        pthread_mutex_unlock(&c->group->mu);
    }
    if (up >= 0) {
        uint64_t c2u = 0, u2c = 0;
        pump(c->client, up, &c2u, &u2c);
        close(up);
        logts("relay :%d ← %s đóng sau %ds (client→máy %lluKB, máy→client %lluKB)", c->pub_port,
              c->peer, (int)((now_ms() - t0) / 1000), (unsigned long long)(c2u / 1024),
              (unsigned long long)(u2c / 1024));
    }
    close(c->client);
    free(c);
    return NULL;
}
