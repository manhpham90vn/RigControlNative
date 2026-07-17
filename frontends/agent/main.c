/*
 * rc-agent — chạy trên máy đang cắm thiết bị (macOS/Linux) để máy khác trong LAN điều khiển
 * qua app RigControlNative:
 *   - Điện thoại USB: bật `adb tcpip` rồi in "ip:port" CỦA ĐIỆN THOẠI để nhập vào ô
 *     "Thêm thiết bị Wi-Fi" (desktop kết nối thẳng tới điện thoại — agent thoát được).
 *   - Emulator (adbd nằm sau NAT trong guest, chỉ với tới từ localhost qua `adb forward`):
 *     adb forward + relay TCP → "mở NAT port" cổng adb và dải cổng stream ra mạng; in
 *     "ip-máy-này:port" để nhập vào app. Agent phải chạy tiếp giữ relay sống.
 *     Relay bind riêng vào từng IP LAN + Tailscale (không dùng 0.0.0.0 để khỏi lỡ mở ra
 *     interface public; không bind loopback vì máy này đã có adb forward localhost sẵn).
 *
 * C thuần POSIX (poll/getifaddrs/posix_spawnp — không epoll/SOCK_NONBLOCK để build được trên
 * macOS), không phụ thuộc core/GTK; chỉ cần `adb` trong PATH.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define DEFAULT_TCPIP_PORT 5555
#define DEFAULT_ADB_BASE 15553 /* cổng public đầu tiên cho adb của emulator */
#define STREAM_BASE 27183      /* trùng RC_LAN_BASE_PORT phía app — session cấp tăng dần */
#define STREAM_COUNT 4         /* số phiên LAN trực tiếp đồng thời hỗ trợ qua relay */
#define LOC_OFFSET 10000       /* cổng local trung gian (adb forward) = cổng public + offset */
#define PRE_CAP 4096           /* dữ liệu client giữ lại để replay trước khi upstream xác nhận */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* ---------- Chạy lệnh con (adb) có timeout ---------- */

/* Chạy đồng bộ; out != NULL thì capture stdout (stderr đi thẳng ra terminal). Quá hạn thì
 * SIGKILL — `adb connect` tới đích chết treo theo TCP SYN retry hàng phút nếu không chặn.
 * Trả 0 nếu exit code 0. */
static int run_cmd(const char *const argv[], int timeout_ms, char *out, size_t out_sz) {
    int pfd[2] = {-1, -1};
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out) {
        if (pipe(pfd) < 0) {
            posix_spawn_file_actions_destroy(&fa);
            return -1;
        }
        posix_spawn_file_actions_adddup2(&fa, pfd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&fa, pfd[0]);
        posix_spawn_file_actions_addclose(&fa, pfd[1]);
    } else {
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, NULL, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (pfd[1] >= 0) close(pfd[1]);
    if (rc != 0) {
        if (pfd[0] >= 0) close(pfd[0]);
        return -1;
    }

    const int64_t deadline = now_ms() + timeout_ms;
    if (out) {
        size_t got = 0;
        while (got < out_sz - 1) {
            int64_t remain = deadline - now_ms();
            if (remain <= 0) break;
            struct pollfd p = {.fd = pfd[0], .events = POLLIN};
            int r = poll(&p, 1, remain > 200 ? 200 : (int)remain);
            if (r < 0 && errno == EINTR) continue;
            if (r < 0) break;
            if (r == 0) continue;
            ssize_t n = read(pfd[0], out + got, out_sz - 1 - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        close(pfd[0]);
        out[got] = '\0';
    }

    int status = 0;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return -1;
        if (now_ms() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            return -1;
        }
        sleep_ms(50);
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* ---------- Socket helpers ---------- */

/* Connect non-blocking có timeout (đích không phản hồi thì connect() thường treo hàng phút). */
static int tcp_connect_timeout(const char *host, int port, int timeout_ms) {
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

/* Bind vào một IPv4 cụ thể (IP LAN hoặc Tailscale của máy này), không phải INADDR_ANY. */
static int tcp_listen_addr(const char *ip, int port) {
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

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---------- Relay TCP 0.0.0.0:pub → 127.0.0.1:dst ---------- */

/* Nhóm cổng stream của một emulator: server trong guest chỉ listen SAU khi desktop deploy,
 * nhưng connect qua adb forward "thành công" ngay cả khi guest chưa listen (adb đóng ngay
 * sau đó). Vì vậy kết nối stream cần: (1) replay dữ liệu đầu (token) khi phải reconnect,
 * (2) tuần tự hoá từng kết nối trong nhóm — server accept theo thứ tự video→audio→control,
 * nếu để kết nối sau vào guest trước trong lúc kết nối trước đang retry thì lệch kênh. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int busy;
} Group;

typedef struct {
    int fd; /* listen fd */
    int pub_port;
    int dst_port;
    Group *group; /* NULL = relay trong suốt (cổng adb) */
} Listener;

typedef struct {
    int client;
    int dst_port;
    Group *group;
} Conn;

/* Bơm 2 chiều tới khi một đầu đóng/lỗi. Blocking read sau POLLIN + write_full — backpressure
 * tự nhiên, mỗi kết nối một thread riêng nên không chặn ai khác. */
static void pump(int a, int b) {
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
            if (n <= 0 || write_full(i == 0 ? b : a, buf, (size_t)n) < 0) done = 1;
        }
        if (done) break;
    }
}

/* Mở upstream cho kết nối stream: connect 127.0.0.1:dst, ghi lại những gì client gửi (token)
 * vào pre[] để replay; upstream "xác nhận" khi có dữ liệu trả về hoặc giữ im lặng 300ms
 * (guest chưa listen thì adb đóng trong vài ms). EOF sớm → reconnect + replay, tối đa 10s. */
static int stream_open_upstream(Conn *c, uint8_t *pre, size_t *pre_len) {
    const int64_t deadline = now_ms() + 10000;
    for (;;) {
        int up = tcp_connect_timeout("127.0.0.1", c->dst_port, 3000);
        if (up < 0) {
            if (g_stop || now_ms() >= deadline) return -1;
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
                if (n <= 0 || *pre_len + (size_t)n > PRE_CAP ||
                    write_full(up, b, (size_t)n) < 0) {
                    verdict = -2; /* client đóng / quá nhiều dữ liệu để replay */
                } else {
                    memcpy(pre + *pre_len, b, (size_t)n);
                    *pre_len += (size_t)n;
                }
            }
        }
        if (verdict == 1) return up;
        close(up);
        if (verdict == -2 || g_stop || now_ms() >= deadline) return -1;
        sleep_ms(150);
    }
}

static void *conn_thread(void *arg) {
    Conn *c = arg;
    int up;
    if (!c->group) {
        up = tcp_connect_timeout("127.0.0.1", c->dst_port, 3000);
    } else {
        pthread_mutex_lock(&c->group->mu);
        while (c->group->busy)
            pthread_cond_wait(&c->group->cv, &c->group->mu);
        c->group->busy = 1;
        pthread_mutex_unlock(&c->group->mu);

        uint8_t pre[PRE_CAP];
        size_t pre_len = 0;
        up = stream_open_upstream(c, pre, &pre_len);

        pthread_mutex_lock(&c->group->mu);
        c->group->busy = 0;
        pthread_cond_broadcast(&c->group->cv);
        pthread_mutex_unlock(&c->group->mu);
    }
    if (up >= 0) {
        pump(c->client, up);
        close(up);
    }
    close(c->client);
    free(c);
    return NULL;
}

/* ---------- adb helpers ---------- */

typedef struct {
    char serial[128];
    char model[96];
} Dev;

static int list_devices(Dev *devs, int cap) {
    char out[16384];
    const char *argv[] = {"adb", "devices", "-l", NULL};
    if (run_cmd(argv, 10000, out, sizeof out) != 0) return -1;
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line && n < cap;
         line = strtok_r(NULL, "\n", &save)) {
        char serial[128] = {0}, state[64] = {0};
        if (sscanf(line, "%127s %63s", serial, state) < 2) continue;
        if (strcmp(state, "device") != 0) continue;
        Dev *d = &devs[n++];
        snprintf(d->serial, sizeof d->serial, "%s", serial);
        d->model[0] = '\0';
        char *m = strstr(line, "model:");
        if (m) sscanf(m + 6, "%95s", d->model);
    }
    return n;
}

/* Forward đã tạo — dọn khi thoát. */
typedef struct {
    char serial[128];
    int loc_port;
} Fwd;
static Fwd g_fwds[64];
static int g_nfwd = 0;

static int adb_forward(const char *serial, int loc_port, int dev_port) {
    char lspec[32], dspec[32];
    snprintf(lspec, sizeof lspec, "tcp:%d", loc_port);
    snprintf(dspec, sizeof dspec, "tcp:%d", dev_port);
    const char *argv[] = {"adb", "-s", serial, "forward", lspec, dspec, NULL};
    if (run_cmd(argv, 10000, NULL, 0) != 0) return -1;
    if (g_nfwd < (int)(sizeof g_fwds / sizeof g_fwds[0])) {
        snprintf(g_fwds[g_nfwd].serial, sizeof g_fwds[g_nfwd].serial, "%s", serial);
        g_fwds[g_nfwd].loc_port = loc_port;
        g_nfwd++;
    }
    return 0;
}

static void cleanup_forwards(void) {
    for (int i = 0; i < g_nfwd; i++) {
        char lspec[32];
        snprintf(lspec, sizeof lspec, "tcp:%d", g_fwds[i].loc_port);
        const char *argv[] = {"adb", "-s", g_fwds[i].serial, "forward", "--remove", lspec,
                              NULL};
        run_cmd(argv, 5000, NULL, 0);
    }
    g_nfwd = 0;
}

/* ---------- IP của máy chạy agent ---------- */

/* Bỏ loopback và interface ảo phổ biến (docker/bridge/veth); giữ Ethernet/Wi-Fi/VPN
 * (Tailscale hữu ích khi hai máy nối qua tailnet). */
static int iface_skipped(const char *name) {
    return strncmp(name, "lo", 2) == 0 || strncmp(name, "docker", 6) == 0 ||
           strncmp(name, "br-", 3) == 0 || strncmp(name, "veth", 4) == 0;
}

static int host_ips(char ips[][64], char names[][32], int cap) {
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return 0;
    int n = 0;
    for (struct ifaddrs *ifa = ifs; ifa && n < cap; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (iface_skipped(ifa->ifa_name)) continue;
        struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
        if (!inet_ntop(AF_INET, &sin->sin_addr, ips[n], 64)) continue;
        if (strncmp(ips[n], "169.254.", 8) == 0) continue; /* link-local (awdl/APIPA) */
        snprintf(names[n], 32, "%s", ifa->ifa_name);
        n++;
    }
    freeifaddrs(ifs);
    return n;
}

/* ---------- Cấu hình từng loại thiết bị ---------- */

/* Điện thoại USB: đọc IP Wi-Fi → adb tcpip → verify cổng mở → in địa chỉ nhập vào app. */
static void setup_usb_phone(const Dev *d, int tcpip_port) {
    char out[8192];
    const char *route_argv[] = {"adb", "-s", d->serial, "shell", "ip", "route", NULL};
    char ip[64] = {0};
    if (run_cmd(route_argv, 10000, out, sizeof out) == 0) {
        /* Dòng dạng "192.168.1.0/24 dev wlan0 ... src 192.168.1.23"; ưu tiên wlan. */
        char *save = NULL;
        for (int pass = 0; pass < 2 && !ip[0]; pass++) {
            char tmp[8192];
            snprintf(tmp, sizeof tmp, "%s", out);
            for (char *line = strtok_r(tmp, "\n", &save); line;
                 line = strtok_r(NULL, "\n", &save)) {
                if (pass == 0 && !strstr(line, "wlan")) continue;
                char *src = strstr(line, " src ");
                if (src && sscanf(src + 5, "%63s", ip) == 1) break;
                ip[0] = '\0';
            }
        }
    }
    if (!ip[0]) {
        printf("✗ %s (%s): không đọc được IP Wi-Fi — bật Wi-Fi cùng mạng LAN rồi chạy lại.\n",
               d->model[0] ? d->model : "USB", d->serial);
        return;
    }

    char portstr[8];
    snprintf(portstr, sizeof portstr, "%d", tcpip_port);
    const char *tcpip_argv[] = {"adb", "-s", d->serial, "tcpip", portstr, NULL};
    if (run_cmd(tcpip_argv, 15000, NULL, 0) != 0) {
        printf("✗ %s (%s): `adb tcpip %d` thất bại.\n", d->model[0] ? d->model : "USB",
               d->serial, tcpip_port);
        return;
    }

    /* adbd khởi động lại mất ~1s; verify cổng thật sự mở trước khi báo sẵn sàng. */
    int ok = 0;
    for (int64_t deadline = now_ms() + 8000; now_ms() < deadline && !g_stop;) {
        int fd = tcp_connect_timeout(ip, tcpip_port, 1000);
        if (fd >= 0) {
            close(fd);
            ok = 1;
            break;
        }
        sleep_ms(300);
    }
    printf("✓ %s (%s): nhập vào app →  %s:%d%s\n", d->model[0] ? d->model : "USB", d->serial,
           ip, tcpip_port,
           ok ? "" : "  (chưa xác minh được cổng — kiểm tra hai máy cùng mạng)");
}

/* Emulator/thiết bị --expose: forward adbd guest ra localhost rồi relay ra IP LAN/Tailscale. */
#define MAX_LISTENERS 64
static int g_nls = 0;
static Listener g_ls[MAX_LISTENERS];
static Group g_stream_group = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};

/* IP LAN + Tailscale của máy này — mỗi IP một listener cho cùng một cổng public. */
static char g_bind_ips[8][64];
static char g_bind_ifs[8][32];
static int g_nbind = 0;

/* Mở listener trên MỌI IP đã chọn cho cùng pub_port. Thành công nếu bind được ≥1 IP.
 * bound != NULL: nhận bitmask index các IP bind được (để in đúng địa chỉ đang nghe). */
static int add_listener(int pub_port, int dst_port, Group *group, unsigned *bound) {
    int made = 0;
    if (bound) *bound = 0;
    for (int i = 0; i < g_nbind; i++) {
        if (g_nls >= MAX_LISTENERS) break;
        int fd = tcp_listen_addr(g_bind_ips[i], pub_port);
        if (fd < 0) {
            fprintf(stderr, "✗ không bind được %s:%d (đang bị chiếm?)\n", g_bind_ips[i],
                    pub_port);
            continue;
        }
        g_ls[g_nls++] = (Listener){.fd = fd, .pub_port = pub_port, .dst_port = dst_port,
                                   .group = group};
        if (bound) *bound |= 1u << i;
        made++;
    }
    return made > 0 ? 0 : -1;
}

/* adbd trong guest có nghe TCP không: adb connect qua forward rồi get-state. */
static int verify_guest_adb(int loc_port) {
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", loc_port);
    const char *conn[] = {"adb", "connect", addr, NULL};
    const char *state[] = {"adb", "-s", addr, "get-state", NULL};
    const char *disc[] = {"adb", "disconnect", addr, NULL};
    int ok = run_cmd(conn, 10000, NULL, 0) == 0 && run_cmd(state, 5000, NULL, 0) == 0;
    run_cmd(disc, 5000, NULL, 0); /* dọn device tạm khỏi adb của máy này */
    return ok;
}

static void setup_emulator(const Dev *d, int idx, int adb_base, int tcpip_port, int no_stream) {
    int pub = adb_base + idx;
    int loc = pub + LOC_OFFSET;

    if (adb_forward(d->serial, loc, tcpip_port) != 0) {
        printf("✗ %s: adb forward thất bại.\n", d->serial);
        return;
    }
    if (!verify_guest_adb(loc)) {
        /* adbd trong guest chưa nghe TCP → bật rồi thử lại (tcpip restart adbd nên forward
         * phải tạo lại). */
        char portstr[8];
        snprintf(portstr, sizeof portstr, "%d", tcpip_port);
        const char *tcpip_argv[] = {"adb", "-s", d->serial, "tcpip", portstr, NULL};
        run_cmd(tcpip_argv, 15000, NULL, 0);
        sleep_ms(2000);
        if (adb_forward(d->serial, loc, tcpip_port) != 0 || !verify_guest_adb(loc)) {
            printf("✗ %s: không với tới adbd trong guest (tcp %d).\n", d->serial, tcpip_port);
            return;
        }
    }
    unsigned bound = 0;
    if (add_listener(pub, loc, NULL, &bound) != 0) return;

    int stream_ok = 0;
    if (!no_stream) {
        /* Chỉ emulator đầu tiên: dải cổng stream dùng chung cho mọi phiên tới host này —
         * nhiều emulator cùng máy thì không phân biệt được phiên nào của máy nào; các máy
         * sau tự fallback adb tunnel (core đã lo). */
        stream_ok = 1;
        for (int p = STREAM_BASE; p < STREAM_BASE + STREAM_COUNT; p++) {
            if (adb_forward(d->serial, p + LOC_OFFSET, p) != 0 ||
                add_listener(p, p + LOC_OFFSET, &g_stream_group, NULL) != 0) {
                stream_ok = 0;
                break;
            }
        }
    }
    printf("✓ %s (%s)%s\n", d->model[0] ? d->model : "emulator", d->serial,
           stream_ok ? "  (stream LAN trực tiếp qua relay)" : "  (stream qua adb tunnel)");
    /* Chỉ liệt kê IP mà relay bind được thật — bind hỏng thì địa chỉ đó không nghe. */
    for (int i = 0, first = 1; i < g_nbind; i++) {
        if (!(bound & (1u << i))) continue;
        char addr[80];
        snprintf(addr, sizeof addr, "%s:%d", g_bind_ips[i], pub);
        printf("    %s %-24s (%s)\n", first ? "nhập vào app → " : "               ", addr,
               g_bind_ifs[i]);
        first = 0;
    }
}

/* ---------- main ---------- */

static void usage(const char *prog) {
    fprintf(stderr,
            "Dùng: %s [tuỳ chọn]\n"
            "  --tcpip-port <p>   cổng adb TCP trên thiết bị (mặc định %d)\n"
            "  --adb-base <p>     cổng public đầu tiên cho emulator (mặc định %d)\n"
            "  --no-stream        không relay dải cổng stream %d-%d (dùng adb tunnel)\n"
            "  --expose <serial>  ép expose một thiết bị như emulator (kể cả wireless)\n",
            prog, DEFAULT_TCPIP_PORT, DEFAULT_ADB_BASE, STREAM_BASE,
            STREAM_BASE + STREAM_COUNT - 1);
}

int main(int argc, char **argv) {
    int tcpip_port = DEFAULT_TCPIP_PORT, adb_base = DEFAULT_ADB_BASE, no_stream = 0;
    const char *expose = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tcpip-port") == 0 && i + 1 < argc)
            tcpip_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--adb-base") == 0 && i + 1 < argc)
            adb_base = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-stream") == 0)
            no_stream = 1;
        else if (strcmp(argv[i], "--expose") == 0 && i + 1 < argc)
            expose = argv[++i];
        else {
            usage(argv[0]);
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* thấy tiến trình cả khi stdout là pipe/file */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* write vào socket đã đóng → EPIPE, không chết process */

    Dev devs[32];
    int ndev = list_devices(devs, 32);
    if (ndev < 0) {
        fprintf(stderr, "Không chạy được `adb devices` — adb có trong PATH chưa?\n");
        return 1;
    }
    if (ndev == 0) {
        printf("Không thấy thiết bị nào (adb devices trống). Cắm máy / bật USB debugging.\n");
        return 0;
    }

    char ips[8][64], ifnames[8][32];
    int nip = host_ips(ips, ifnames, 8);
    g_nbind = nip;
    for (int i = 0; i < nip; i++) {
        snprintf(g_bind_ips[i], 64, "%s", ips[i]);
        snprintf(g_bind_ifs[i], 32, "%s", ifnames[i]);
    }
    if (nip == 0)
        fprintf(stderr, "⚠ Không thấy IP LAN/Tailscale nào — chỉ điện thoại USB (in IP máy)\n"
                        "  hoạt động; emulator cần một interface để relay bind vào.\n");

    int nemu = 0;
    for (int i = 0; i < ndev; i++) {
        const Dev *d = &devs[i];
        int is_emu = strncmp(d->serial, "emulator-", 9) == 0;
        int is_wireless = strchr(d->serial, ':') != NULL;
        if (is_emu || (expose && strcmp(d->serial, expose) == 0)) {
            int idx = nemu++;
            /* Relay stream chỉ cho máy đầu tiên — dải cổng dùng chung theo host, nhiều máy
             * thì không phân biệt được phiên của máy nào; máy sau fallback adb tunnel. */
            setup_emulator(d, idx, adb_base, tcpip_port, no_stream || idx > 0);
        } else if (is_wireless) {
            printf("· %s: đã là wireless adb — nhập thẳng địa chỉ này vào app.\n", d->serial);
        } else {
            setup_usb_phone(d, tcpip_port);
        }
    }

    if (g_nls == 0) return 0; /* chỉ điện thoại USB → không cần relay, xong việc */

    printf("\nRelay đang chạy — giữ cửa sổ này mở, Ctrl-C để dừng và dọn forward.\n");

    /* Vòng accept: mỗi kết nối một thread bơm riêng. */
    while (!g_stop) {
        struct pollfd pfds[MAX_LISTENERS];
        for (int i = 0; i < g_nls; i++) pfds[i] = (struct pollfd){.fd = g_ls[i].fd,
                                                                  .events = POLLIN};
        int r = poll(pfds, (nfds_t)g_nls, 500);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        for (int i = 0; i < g_nls; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            int cl = accept(g_ls[i].fd, NULL, NULL);
            if (cl < 0) continue;
            int one = 1;
            setsockopt(cl, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            Conn *c = malloc(sizeof *c);
            if (!c) {
                close(cl);
                continue;
            }
            *c = (Conn){.client = cl, .dst_port = g_ls[i].dst_port, .group = g_ls[i].group};
            pthread_t th;
            if (pthread_create(&th, NULL, conn_thread, c) == 0)
                pthread_detach(th);
            else {
                close(cl);
                free(c);
            }
        }
    }

    printf("\nDừng — gỡ các adb forward đã tạo…\n");
    cleanup_forwards();
    return 0;
}
