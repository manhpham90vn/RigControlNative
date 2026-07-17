/*
 * server_deploy.c — đẩy rc-server lên thiết bị, thiết lập tunnel, mở video/[audio]/[control] fd.
 *
 * USB (RC_TRANSPORT_USB): push jar → listen loopback → adb reverse → chạy server (nền) →
 *   accept các socket theo thứ tự cố định video → [audio] → [control].
 * TCP (RC_TRANSPORT_TCP): server đã chạy sẵn trên thiết bị (docs/PROTOCOL.md §1.2); core chỉ
 *   connect tới "ip:port" theo cùng thứ tự.
 */
#include "rc_internal.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ACCEPT_TIMEOUT_MS 15000
#define WAIT_SLICE_MS 100 /* lát chờ để kiểm tra abort / server chết giữa chừng */

/* Đường dẫn jar server phía desktop: ưu tiên env RC_SERVER_PATH, mặc định "server/rc-server". */
static const char *server_local_path(void) {
    const char *env = getenv("RC_SERVER_PATH");
    return (env && *env) ? env : "server/rc-server";
}

/* Tiến trình `adb shell app_process` đã chết? (server crash/adb rớt → fail-fast thay vì
 * chờ hết timeout). Reap luôn để teardown khỏi waitpid lần nữa. */
static int server_died(rc_client *c) {
    if (c->server_pid <= 0) return 0;
    int status = 0;
    if (waitpid(c->server_pid, &status, WNOHANG) == c->server_pid) {
        c->server_pid = 0;
        return 1;
    }
    return 0;
}

static int wait_cancelled(rc_client *c) {
    return atomic_load(&c->abort_requested) || server_died(c);
}

/* Chờ listen_fd có kết nối rồi accept; -1 nếu timeout/abort/server chết. Chờ theo lát
 * WAIT_SLICE_MS để hủy sớm được (đóng cửa sổ khi đang kết nối). */
static int accept_timeout(rc_client *c, int listen_fd, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += WAIT_SLICE_MS) {
        struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
        int r = poll(&pfd, 1, WAIT_SLICE_MS);
        if (r > 0) return rc_net_accept(listen_fd);
        if (r < 0 || wait_cancelled(c)) return -1;
    }
    return -1; /* timeout */
}

/* Sinh token hex 32 ký tự cho LAN trực tiếp (đọc /dev/urandom; fallback PRNG seed pid+time). */
static void gen_lan_token(char out[RC_LAN_TOKEN_LEN + 1]) {
    uint8_t raw[RC_LAN_TOKEN_LEN / 2];
    int fd = open("/dev/urandom", O_RDONLY);
    ssize_t got = fd >= 0 ? read(fd, raw, sizeof raw) : -1;
    if (fd >= 0) close(fd);
    if (got != (ssize_t)sizeof raw) {
        srand((unsigned)(time(NULL) ^ getpid()));
        for (size_t i = 0; i < sizeof raw; i++)
            raw[i] = (uint8_t)rand();
    }
    for (size_t i = 0; i < sizeof raw; i++)
        sprintf(out + 2 * i, "%02x", raw[i]);
    out[RC_LAN_TOKEN_LEN] = '\0';
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

/* Serial dạng "ip:port" (wireless adb) → adb connect trước; idempotent nếu đã kết nối. */
static rc_status ensure_adb_device(rc_client *c) {
    const char *serial = c->cfg.serial;
    if (serial && strchr(serial, ':')) {
        rc_status r = rc_adb_connect(serial);
        if (r != RC_OK) {
            rc_emit_status(c, r, "adb connect thất bại (kiểm tra ip:port / adb tcpip)");
            return r;
        }
    }
    return RC_OK;
}

static rc_status deploy_usb(rc_client *c) {
    const char *serial = c->cfg.serial;

    rc_status r = ensure_adb_device(c);
    if (r != RC_OK) return r;

    r = rc_adb_push(serial, server_local_path(), RC_SERVER_REMOTE_PATH);
    if (r != RC_OK) {
        rc_emit_status(c, r, "adb push rc-server thất bại (kiểm tra RC_SERVER_PATH / thiết bị)");
        return r;
    }

    int port = 0;
    c->listen_fd = rc_net_listen_loopback(&port);
    if (c->listen_fd < 0) return RC_ERR_CONNECT;

    r = rc_adb_reverse(serial, c->socket_name, port);
    if (r != RC_OK) {
        rc_emit_status(c, r, "adb reverse thất bại");
        return r;
    }

    r = rc_adb_run_server(serial, &c->cfg, c->socket_name, 0, NULL, &c->server_pid);
    if (r != RC_OK) {
        rc_emit_status(c, r, "không chạy được rc-server (app_process)");
        return r;
    }

    /* Server connect ngược lại theo thứ tự: video → [audio] → [control]. */
    c->video_fd = accept_timeout(c, c->listen_fd, ACCEPT_TIMEOUT_MS);
    if (c->video_fd < 0) {
        rc_emit_status(c, RC_ERR_CONNECT, "server không kết nối video (timeout/hủy)");
        return RC_ERR_CONNECT;
    }
    if (c->cfg.audio) {
        c->audio_fd = accept_timeout(c, c->listen_fd, ACCEPT_TIMEOUT_MS);
        if (c->audio_fd < 0) return RC_ERR_CONNECT;
    }
    if (c->cfg.control) {
        c->control_fd = accept_timeout(c, c->listen_fd, ACCEPT_TIMEOUT_MS);
        if (c->control_fd < 0) return RC_ERR_CONNECT;
    }

    close(c->listen_fd);
    c->listen_fd = -1;
    atomic_store(&c->transport_desc,
                 serial && strchr(serial, ':')
                     ? "LAN qua adb"
                     : (serial && strncmp(serial, "emulator-", 9) == 0 ? "adb (máy ảo)" : "USB"));
    return RC_OK;
}

/* Connect TCP có retry (server vừa deploy cần thời gian mở cổng listen); hủy sớm khi
 * abort/server chết. Kết nối xong gửi ngay token nếu phiên dùng token (PROTOCOL §1.2). */
static int connect_retry(rc_client *c, const char *host, int port, int attempts, int delay_ms) {
    for (int i = 0; i < attempts; i++) {
        int fd = rc_net_connect_tcp(host, port);
        if (fd >= 0) {
            if (c->lan_token[0] && rc_net_write_full(fd, c->lan_token, RC_LAN_TOKEN_LEN) != RC_OK) {
                close(fd);
                return -1;
            }
            return fd;
        }
        if (wait_cancelled(c)) return -1;
        usleep((useconds_t)delay_ms * 1000);
    }
    return -1;
}

/*
 * LAN trực tiếp: nếu có serial → tự push + chạy server (tcp=true) qua adb rồi connect thẳng
 * host:port (stream không đi qua adb tunnel). Không có serial → server đã chạy sẵn, chỉ connect.
 *
 * Bảo mật: cổng LAN mở cho cả mạng, nên khi tự deploy, core sinh token ngẫu nhiên truyền cho
 * server qua adb (kênh tin cậy); mỗi kết nối TCP phải gửi token trước, sai thì server đóng.
 */
static rc_status deploy_tcp(rc_client *c) {
    char host[128];
    int port = 0;
    rc_status r = parse_addr(c->cfg.tcp_addr, host, sizeof host, &port);
    if (r != RC_OK) {
        rc_emit_status(c, r, "tcp_addr không hợp lệ (cần \"ip[:port]\")");
        return r;
    }

    int deployed = 0;
    if (c->cfg.serial && *c->cfg.serial) {
        r = ensure_adb_device(c);
        if (r != RC_OK) return r;
        r = rc_adb_push(c->cfg.serial, server_local_path(), RC_SERVER_REMOTE_PATH);
        if (r != RC_OK) {
            rc_emit_status(c, r, "adb push rc-server thất bại (kiểm tra RC_SERVER_PATH)");
            return r;
        }
        gen_lan_token(c->lan_token);
        r = rc_adb_run_server(c->cfg.serial, &c->cfg, NULL, port, c->lan_token, &c->server_pid);
        if (r != RC_OK) {
            rc_emit_status(c, r, "không chạy được rc-server (app_process)");
            return r;
        }
        deployed = 1;
    }

    /* Vừa deploy → chờ server listen (tối đa ~10s); server có sẵn → thử 1 lần. */
    int ok = 0;
    c->video_fd = connect_retry(c, host, port, deployed ? 40 : 1, 250);
    if (c->video_fd >= 0) {
        ok = 1;
        if (c->cfg.audio && (c->audio_fd = connect_retry(c, host, port, 4, 250)) < 0) ok = 0;
        if (ok && c->cfg.control && (c->control_fd = connect_retry(c, host, port, 4, 250)) < 0)
            ok = 0;
    }
    if (ok) {
        atomic_store(&c->transport_desc, "LAN trực tiếp");
        return RC_OK;
    }

    /* Không với tới cổng LAN dù adb vẫn dùng được → thiết bị sau NAT/firewall (vd emulator,
     * adb forward qua VPN: IP trong serial là của máy host, không phải của Android). Cùng
     * đường mạng với adb nhưng cổng stream không được forward → fallback adb tunnel thay vì
     * fail để người dùng vẫn có hình. */
    if (deployed && !atomic_load(&c->abort_requested)) {
        rc_emit_status(c, RC_OK,
                       "LAN trực tiếp không kết nối được (thiết bị sau NAT/firewall?) — chuyển "
                       "qua adb tunnel");
        rc_server_teardown(c); /* đóng fd dở dang + dừng server tcp vừa chạy */
        c->lan_token[0] = '\0';
        c->cfg.transport = RC_TRANSPORT_USB; /* teardown cuối phiên sẽ dọn adb reverse */
        return deploy_usb(c);
    }
    rc_emit_status(c, RC_ERR_CONNECT, "connect video TCP thất bại");
    return RC_ERR_CONNECT;
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

    if (c->cfg.transport == RC_TRANSPORT_USB) rc_adb_reverse_remove(c->cfg.serial, c->socket_name);
}
