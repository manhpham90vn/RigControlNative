/*
 * server_deploy.c — đẩy rc-server lên thiết bị, thiết lập tunnel, mở video/[audio]/[control] fd.
 *
 * USB (RC_TRANSPORT_USB): push jar → listen loopback → adb reverse → chạy server (nền) →
 *   accept các socket theo thứ tự cố định video → [audio] → [control].
 * TCP (RC_TRANSPORT_TCP): server đã chạy sẵn trên thiết bị (docs/PROTOCOL.md §1.2); core chỉ
 *   connect tới "ip:port" theo cùng thứ tự.
 */
#include "rc_internal.h"

#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define ACCEPT_TIMEOUT_MS 15000

/* Đường dẫn jar server phía desktop: ưu tiên env RC_SERVER_PATH, mặc định "server/rc-server". */
static const char *server_local_path(void) {
    const char *env = getenv("RC_SERVER_PATH");
    return (env && *env) ? env : "server/rc-server";
}

/* Chờ listen_fd có kết nối trong timeout rồi accept; -1 nếu timeout/lỗi. */
static int accept_timeout(int listen_fd, int timeout_ms) {
    struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return -1; /* timeout hoặc lỗi */
    return rc_net_accept(listen_fd);
}

/* Tách "ip:port" → host (buf) + port; nếu thiếu port dùng RC_DEFAULT_TCP_PORT. */
static rc_status parse_addr(const char *addr, char *host, size_t host_sz, int *port) {
    if (!addr || !*addr) return RC_ERR_INVALID_ARG;
    const char *colon = strrchr(addr, ':');
    if (colon) {
        size_t n = (size_t)(colon - addr);
        if (n == 0 || n >= host_sz) return RC_ERR_INVALID_ARG;
        memcpy(host, addr, n);
        host[n] = '\0';
        *port = atoi(colon + 1);
        if (*port <= 0) *port = RC_DEFAULT_TCP_PORT;
    } else {
        if (strlen(addr) >= host_sz) return RC_ERR_INVALID_ARG;
        strcpy(host, addr);
        *port = RC_DEFAULT_TCP_PORT;
    }
    return RC_OK;
}

static rc_status deploy_usb(rc_client *c) {
    const char *serial = c->cfg.serial;

    rc_status r = rc_adb_push(serial, server_local_path(), RC_SERVER_REMOTE_PATH);
    if (r != RC_OK) {
        rc_emit_status(c, r, "adb push rc-server thất bại (kiểm tra RC_SERVER_PATH / thiết bị)");
        return r;
    }

    int port = 0;
    c->listen_fd = rc_net_listen_loopback(&port);
    if (c->listen_fd < 0) return RC_ERR_CONNECT;

    r = rc_adb_reverse(serial, RC_LOCALABSTRACT_NAME, port);
    if (r != RC_OK) {
        rc_emit_status(c, r, "adb reverse thất bại");
        return r;
    }

    r = rc_adb_run_server(serial, &c->cfg, &c->server_pid);
    if (r != RC_OK) {
        rc_emit_status(c, r, "không chạy được rc-server (app_process)");
        return r;
    }

    /* Server connect ngược lại theo thứ tự: video → [audio] → [control]. */
    c->video_fd = accept_timeout(c->listen_fd, ACCEPT_TIMEOUT_MS);
    if (c->video_fd < 0) {
        rc_emit_status(c, RC_ERR_CONNECT, "server không kết nối video (timeout)");
        return RC_ERR_CONNECT;
    }
    if (c->cfg.audio) {
        c->audio_fd = accept_timeout(c->listen_fd, ACCEPT_TIMEOUT_MS);
        if (c->audio_fd < 0) return RC_ERR_CONNECT;
    }
    if (c->cfg.control) {
        c->control_fd = accept_timeout(c->listen_fd, ACCEPT_TIMEOUT_MS);
        if (c->control_fd < 0) return RC_ERR_CONNECT;
    }

    close(c->listen_fd);
    c->listen_fd = -1;
    return RC_OK;
}

static rc_status deploy_tcp(rc_client *c) {
    char host[128];
    int port = 0;
    rc_status r = parse_addr(c->cfg.tcp_addr, host, sizeof host, &port);
    if (r != RC_OK) {
        rc_emit_status(c, r, "tcp_addr không hợp lệ (cần \"ip:port\")");
        return r;
    }

    c->video_fd = rc_net_connect_tcp(host, port);
    if (c->video_fd < 0) {
        rc_emit_status(c, RC_ERR_CONNECT, "connect video TCP thất bại");
        return RC_ERR_CONNECT;
    }
    if (c->cfg.audio) {
        c->audio_fd = rc_net_connect_tcp(host, port);
        if (c->audio_fd < 0) return RC_ERR_CONNECT;
    }
    if (c->cfg.control) {
        c->control_fd = rc_net_connect_tcp(host, port);
        if (c->control_fd < 0) return RC_ERR_CONNECT;
    }
    return RC_OK;
}

rc_status rc_server_deploy(rc_client *c) {
    if (!c) return RC_ERR_INVALID_ARG;
    return c->cfg.transport == RC_TRANSPORT_TCP ? deploy_tcp(c) : deploy_usb(c);
}

void rc_server_teardown(rc_client *c) {
    if (!c) return;
    if (c->control_fd >= 0) {
        close(c->control_fd);
        c->control_fd = -1;
    }
    if (c->audio_fd >= 0) {
        close(c->audio_fd);
        c->audio_fd = -1;
    }
    if (c->video_fd >= 0) {
        close(c->video_fd);
        c->video_fd = -1;
    }
    if (c->listen_fd >= 0) {
        close(c->listen_fd);
        c->listen_fd = -1;
    }

    /* Đóng socket khiến server tự thoát (PROTOCOL §5); kết thúc + reap tiến trình adb. */
    if (c->server_pid > 0) {
        kill(c->server_pid, SIGTERM);
        waitpid(c->server_pid, NULL, 0);
        c->server_pid = 0;
    }

    if (c->cfg.transport == RC_TRANSPORT_USB)
        rc_adb_reverse_remove(c->cfg.serial, RC_LOCALABSTRACT_NAME);
}
