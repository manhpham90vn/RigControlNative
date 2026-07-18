/*
 * main.c — entry point rc-agent: parse cờ, gom IP bind (LAN + Tailscale + loopback), mở cổng
 * discovery, quét thiết bị lần đầu + khởi động thread rescan, rồi chạy vòng accept.
 *
 * Xem agent.h cho tổng quan kiến trúc và docs/AGENT_PROTOCOL.md cho giao thức.
 */
#include "agent.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Dùng: %s [tuỳ chọn]\n"
            "  --port <p>         cổng discovery (mặc định %d)\n"
            "  --tcpip-port <p>   cổng adb TCP trên thiết bị (mặc định %d)\n"
            "  --adb-base <p>     cổng adb public đầu tiên (mặc định %d, slot 0..%d)\n"
            "  --no-stream        không relay dải cổng stream (mọi phiên dùng adb tunnel)\n",
            prog, DEFAULT_DISCOVERY_PORT, DEFAULT_TCPIP_PORT, DEFAULT_ADB_BASE, MAX_SLOTS - 1);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_discovery_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--tcpip-port") == 0 && i + 1 < argc)
            g_tcpip_port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--adb-base") == 0 && i + 1 < argc)
            g_adb_base = atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-stream") == 0)
            g_no_stream = 1;
        else {
            usage(argv[0]);
            return 2;
        }
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* thấy tiến trình cả khi stdout là pipe/file */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* write vào socket đã đóng → EPIPE, không chết process */

    /* IP bind: LAN + Tailscale + loopback (§0 — loopback để máy agent tự nối vào chính mình). */
    char ips[16][64], ifnames[16][32];
    int nip = host_ips(ips, ifnames, 15);
    for (int i = 0; i < nip; i++) {
        snprintf(g_bind_ips[i], 64, "%s", ips[i]);
        snprintf(g_bind_ifs[i], 32, "%s", ifnames[i]);
    }
    g_nbind = nip;
    snprintf(g_bind_ips[g_nbind], 64, "127.0.0.1");
    snprintf(g_bind_ifs[g_nbind], 32, "loopback");
    g_nbind++;

    /* Cổng discovery — trên mọi IP bind. */
    if (add_listener(g_discovery_port, NULL, 0, NULL, 1) != 0) {
        fprintf(stderr, "✗ Không bind được cổng discovery %d trên IP nào.\n", g_discovery_port);
        return 1;
    }

    printf("Cổng discovery %d đang nghe trên:\n", g_discovery_port);
    for (int i = 0; i < g_nbind; i++)
        printf("    %s:%d  (%s)\n", g_bind_ips[i], g_discovery_port, g_bind_ifs[i]);

    /* Quét lần đầu: dựng slot cho thiết bị đang cắm sẵn. */
    rescan_once();

    pthread_t rescan_th;
    int has_rescan = pthread_create(&rescan_th, NULL, rescan_thread, NULL) == 0;
    if (!has_rescan)
        fprintf(stderr, "⚠ Không tạo được thread rescan — chỉ thấy thiết bị lúc khởi động.\n");

    printf("\nAgent đang chạy — nhập IP máy này vào app rồi bấm \"Quét agent\". Ctrl-C để dừng.\n");

    /* Vòng accept: snapshot g_ls dưới lock rồi poll; mỗi kết nối relay một thread bơm riêng,
     * cổng discovery đáp ngay tại chỗ. */
    while (!g_stop) {
        struct pollfd pfds[MAX_LISTENERS];
        int nls;
        pthread_mutex_lock(&g_lock);
        nls = g_nls;
        for (int i = 0; i < nls; i++) {
            pfds[i].fd = g_ls[i].fd;
            pfds[i].events = POLLIN;
            pfds[i].revents = 0;
        }
        pthread_mutex_unlock(&g_lock);

        int r = poll(pfds, (nfds_t)nls, 500);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        for (int i = 0; i < nls; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            struct sockaddr_in peer_addr;
            socklen_t peer_len = sizeof peer_addr;
            int cl = accept(pfds[i].fd, (struct sockaddr *)&peer_addr, &peer_len);
            if (cl < 0) continue;
            char peer[46] = "?";
            if (peer_len >= sizeof peer_addr && peer_addr.sin_family == AF_INET)
                inet_ntop(AF_INET, &peer_addr.sin_addr, peer, sizeof peer);
            int one = 1;
            setsockopt(cl, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            /* g_ls[i] với i < nls là bất biến (mảng chỉ lớn thêm) → đọc không cần lock. */
            Listener *L = &g_ls[i];
            if (L->is_discovery) {
                logts("discovery: %s hỏi danh sách thiết bị", peer);
                discovery_write(cl);
                close(cl);
                continue;
            }
            Conn *c = malloc(sizeof *c);
            if (!c) {
                close(cl);
                continue;
            }
            c->client = cl;
            c->dst_port = L->dst_port;
            c->group = L->group;
            c->pub_port = L->pub_port;
            snprintf(c->peer, sizeof c->peer, "%s", peer);
            snprintf(c->dst_host, sizeof c->dst_host, "%s", L->dst_host);
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
    g_stop = 1;
    if (has_rescan) pthread_join(rescan_th, NULL);
    cleanup_forwards();
    return 0;
}
