/*
 * adb.c — cầu nối tới `adb`: liệt kê thiết bị, tạo/gỡ forward, xác minh adbd guest, phân loại
 * thiết bị, và liệt kê IP LAN/Tailscale của máy agent.
 */
#include "agent.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

int list_devices(Dev *devs, int cap) {
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

/* Forward đã tạo — dọn khi thoát. Dedup theo (serial, loc_port) để refresh khi cắm lại không
 * làm bảng phình. */
typedef struct {
    char serial[128];
    int loc_port;
} Fwd;
static Fwd g_fwds[128];
static int g_nfwd = 0;

int adb_forward(const char *serial, int loc_port, int dev_port) {
    char lspec[32], dspec[32];
    snprintf(lspec, sizeof lspec, "tcp:%d", loc_port);
    snprintf(dspec, sizeof dspec, "tcp:%d", dev_port);
    const char *argv[] = {"adb", "-s", serial, "forward", lspec, dspec, NULL};
    if (run_cmd(argv, 10000, NULL, 0) != 0) return -1;
    for (int i = 0; i < g_nfwd; i++)
        if (g_fwds[i].loc_port == loc_port && strcmp(g_fwds[i].serial, serial) == 0) return 0;
    if (g_nfwd < (int)(sizeof g_fwds / sizeof g_fwds[0])) {
        snprintf(g_fwds[g_nfwd].serial, sizeof g_fwds[g_nfwd].serial, "%s", serial);
        g_fwds[g_nfwd].loc_port = loc_port;
        g_nfwd++;
    }
    return 0;
}

void cleanup_forwards(void) {
    for (int i = 0; i < g_nfwd; i++) {
        char lspec[32];
        snprintf(lspec, sizeof lspec, "tcp:%d", g_fwds[i].loc_port);
        const char *argv[] = {"adb", "-s", g_fwds[i].serial, "forward", "--remove", lspec,
                              NULL};
        run_cmd(argv, 5000, NULL, 0);
    }
    g_nfwd = 0;
}

/* adbd trong guest có nghe TCP không: adb connect qua forward rồi get-state. */
int verify_guest_adb(int loc_port) {
    char addr[32];
    snprintf(addr, sizeof addr, "127.0.0.1:%d", loc_port);
    const char *conn[] = {"adb", "connect", addr, NULL};
    const char *state[] = {"adb", "-s", addr, "get-state", NULL};
    const char *disc[] = {"adb", "disconnect", addr, NULL};
    int ok = run_cmd(conn, 10000, NULL, 0) == 0 && run_cmd(state, 5000, NULL, 0) == 0;
    run_cmd(disc, 5000, NULL, 0); /* dọn device tạm khỏi adb của máy này */
    return ok;
}

const char *dev_kind(const char *serial) {
    if (strncmp(serial, "emulator-", 9) == 0) return "emulator";
    if (strchr(serial, ':')) return "wireless";
    return "USB";
}

/* Bỏ loopback và interface ảo phổ biến (docker/bridge/veth); giữ Ethernet/Wi-Fi/VPN
 * (Tailscale hữu ích khi hai máy nối qua tailnet). Loopback được thêm riêng ở main (§0). */
static int iface_skipped(const char *name) {
    return strncmp(name, "lo", 2) == 0 || strncmp(name, "docker", 6) == 0 ||
           strncmp(name, "br-", 3) == 0 || strncmp(name, "veth", 4) == 0;
}

int host_ips(char ips[][64], char names[][32], int cap) {
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
