/*
 * agent.c — client cổng discovery của rc-agent: mở TCP tới ip:port, KHÔNG gửi gì, đọc tới EOF,
 * parse đáp ứng theo docs/AGENT_PROTOCOL.md §1.2 thành danh sách AgentDev.
 *
 * Chỉ lo phần "tìm thiết bị nào đang có + nối bằng cổng nào"; sau khi có địa chỉ, chooser tự
 * `adb connect` còn session tự deploy như một thiết bị wireless bình thường.
 */
#include "rcgtk.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RC_AGENT_PROTOCOL_VERSION 1 /* version app này hiểu (AGENT_PROTOCOL §3) */

static GQuark agent_error_quark(void) { return g_quark_from_static_string("rc-agent-scan"); }

/* Connect non-blocking có timeout (cổng sai / máy không phản hồi thì connect() treo rất lâu). */
static int connect_timeout(const char *ip, int port, int timeout_ms) {
    char portstr[8];
    g_snprintf(portstr, sizeof portstr, "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(ip, portstr, &hints, &res) != 0) return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
            int ok = 0;
            if (errno == EINPROGRESS) {
                struct pollfd p = {.fd = fd, .events = POLLOUT};
                int err = 0;
                socklen_t sl = sizeof err;
                if (poll(&p, 1, timeout_ms) > 0 &&
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &sl) == 0 && err == 0)
                    ok = 1;
            }
            if (!ok) {
                close(fd);
                fd = -1;
                continue;
            }
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    }
    freeaddrinfo(res);
    return fd;
}

/* Đọc tới EOF (agent ghi một phát rồi đóng) với hạn tổng — cổng của dịch vụ khác có thể mở mà
 * không gửi gì. Trả số byte, -1 nếu lỗi/timeout. */
static gssize read_to_eof(int fd, char *buf, gsize cap, int timeout_ms) {
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;
    gsize got = 0;
    while (got < cap - 1) {
        gint64 remain = (deadline - g_get_monotonic_time()) / 1000;
        if (remain <= 0) return -1;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        int r = poll(&p, 1, (int)remain);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        ssize_t n = read(fd, buf + got, cap - 1 - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break; /* EOF */
        got += (gsize)n;
    }
    buf[got] = '\0';
    return (gssize)got;
}

/* Parse một dòng thiết bị "<port>\t<kind>\t<serial>\t<model>\t<stream_base>" → AgentDev.
 * TRUE nếu hợp lệ (trường thừa bỏ qua); FALSE nếu thiếu trường / port sai (caller bỏ dòng đó). */
static gboolean parse_dev_line(const char *ip, char *line, AgentDev *out) {
    char **f = g_strsplit(line, "\t", 0);
    guint n = g_strv_length(f);
    gboolean ok = FALSE;
    if (n >= 5) {
        int adb_port = atoi(f[0]);
        if (adb_port > 0) {
            memset(out, 0, sizeof *out);
            g_strlcpy(out->host, ip, sizeof out->host);
            g_snprintf(out->serial, sizeof out->serial, "%s:%d", ip, adb_port);
            g_strlcpy(out->kind, f[1], sizeof out->kind);
            g_strlcpy(out->model, f[3], sizeof out->model);
            out->adb_port = adb_port;
            out->stream_base = atoi(f[4]);
            ok = TRUE;
        }
    }
    g_strfreev(f);
    return ok;
}

gboolean agent_scan(const char *ip, int port, GPtrArray **out, GError **err) {
    *out = NULL;
    int fd = connect_timeout(ip, port, 2000);
    if (fd < 0) {
        g_set_error(err, agent_error_quark(), 1,
                    "Không kết nối được %s:%d (agent chưa chạy? sai IP/cổng?)", ip, port);
        return FALSE;
    }

    char buf[65536];
    gssize len = read_to_eof(fd, buf, sizeof buf, 3000);
    close(fd);
    if (len <= 0) {
        g_set_error(err, agent_error_quark(), 2, "%s:%d không trả lời (cổng của dịch vụ khác?)",
                    ip, port);
        return FALSE;
    }

    char **lines = g_strsplit(buf, "\n", 0);
    /* Dòng đầu: banner "RCAGENT <version>". Từ chối nếu sai banner hoặc version lạ (§3). */
    if (!lines[0] || !g_str_has_prefix(lines[0], "RCAGENT ")) {
        g_strfreev(lines);
        g_set_error(err, agent_error_quark(), 3,
                    "%s:%d không phải rc-agent (banner sai) — kiểm tra lại cổng.", ip, port);
        return FALSE;
    }
    int version = atoi(lines[0] + 8);
    if (version != RC_AGENT_PROTOCOL_VERSION) {
        g_strfreev(lines);
        g_set_error(err, agent_error_quark(), 4,
                    "Agent phiên bản %d, app chỉ hiểu %d — cập nhật cho khớp.", version,
                    RC_AGENT_PROTOCOL_VERSION);
        return FALSE;
    }

    GPtrArray *devs = g_ptr_array_new_with_free_func(g_free);
    for (int i = 1; lines[i]; i++) {
        if (!lines[i][0]) continue; /* dòng trống (vd sau LF cuối) */
        AgentDev dev;
        if (parse_dev_line(ip, lines[i], &dev)) /* dòng lỗi → bỏ dòng đó, không bỏ cả đáp ứng */
            g_ptr_array_add(devs, g_memdup2(&dev, sizeof dev));
    }
    g_strfreev(lines);
    *out = devs;
    return TRUE;
}
