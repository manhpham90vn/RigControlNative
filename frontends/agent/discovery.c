/*
 * discovery.c — bảng slot (gán theo serial, giữ suốt đời process, không tái dụng), cổng
 * discovery (đáp ứng AGENT_PROTOCOL §1.2), và thread quét lại `adb devices` định kỳ.
 *
 * Slot không đổi chủ → không bao giờ phải gỡ listener đang chạy (nguồn race lớn nhất). Vì vậy
 * mảng listener chỉ lớn thêm, và vòng accept ở main đọc entry cũ không cần lock.
 */
#include "agent.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- Trạng thái chia sẻ (khai báo extern trong agent.h) ---------- */
pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
Slot g_slots[MAX_SLOTS];
int g_nslots = 0;

Listener g_ls[MAX_LISTENERS];
int g_nls = 0;

char g_bind_ips[16][64];
char g_bind_ifs[16][32];
int g_nbind = 0;

int g_tcpip_port = DEFAULT_TCPIP_PORT;
int g_adb_base = DEFAULT_ADB_BASE;
int g_no_stream = 0;
int g_discovery_port = DEFAULT_DISCOVERY_PORT;

/* Bind pub_port trên MỌI IP đã chọn. is_discovery=1: cổng discovery (không dst/group).
 * Thành công nếu bind được ≥1 IP. Chỉ append (không sửa/xoá entry cũ) — giữ lock khi cập nhật
 * g_nls để vòng accept không đọc entry viết dở. */
int add_listener(int pub_port, const char *dst_host, int dst_port, Group *group,
                 int is_discovery) {
    int made = 0;
    for (int i = 0; i < g_nbind; i++) {
        int fd = tcp_listen_addr(g_bind_ips[i], pub_port);
        if (fd < 0) {
            fprintf(stderr, "✗ không bind được %s:%d (đang bị chiếm?)\n", g_bind_ips[i],
                    pub_port);
            continue;
        }
        pthread_mutex_lock(&g_lock);
        if (g_nls < MAX_LISTENERS) {
            Listener *L = &g_ls[g_nls];
            L->fd = fd;
            L->pub_port = pub_port;
            snprintf(L->dst_host, sizeof L->dst_host, "%s", dst_host ? dst_host : "127.0.0.1");
            L->dst_port = dst_port;
            L->group = group;
            L->is_discovery = is_discovery;
            g_nls++;
            made++;
        } else {
            close(fd);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return made > 0 ? 0 : -1;
}

/* (Re)dựng adb forward cho slot. Idempotent — gọi lại khi cắm lại thiết bị. Với emulator/USB:
 * forward adbd guest ra localhost (bật `adb tcpip` nếu adbd chưa nghe TCP). Với wireless: relay
 * cổng adb thẳng tới ip:port trong serial, không forward. Stream forward cho MỌI loại. */
static int establish_forwards(Slot *slot) {
    int is_wireless = strcmp(slot->kind, "wireless") == 0;
    if (!is_wireless) {
        int loc = slot->adb_port + LOC_OFFSET;
        if (adb_forward(slot->serial, loc, g_tcpip_port) != 0 || !verify_guest_adb(loc)) {
            /* adbd trong guest chưa nghe TCP → bật rồi thử lại (tcpip restart adbd nên USB rớt
             * 1-2s rồi vào lại; forward phải tạo lại). */
            char portstr[8];
            snprintf(portstr, sizeof portstr, "%d", g_tcpip_port);
            const char *tcpip_argv[] = {"adb", "-s", slot->serial, "tcpip", portstr, NULL};
            run_cmd(tcpip_argv, 15000, NULL, 0);
            sleep_ms(2000);
            if (adb_forward(slot->serial, loc, g_tcpip_port) != 0 || !verify_guest_adb(loc)) {
                printf("✗ %s: không với tới adbd trong guest (tcp %d).\n", slot->serial,
                       g_tcpip_port);
                return -1;
            }
        }
        snprintf(slot->adb_dst_host, sizeof slot->adb_dst_host, "127.0.0.1");
        slot->adb_dst_port = loc;
    } else {
        const char *colon = strrchr(slot->serial, ':');
        size_t n = colon ? (size_t)(colon - slot->serial) : 0;
        if (!colon || n == 0 || n >= sizeof slot->adb_dst_host) {
            printf("✗ %s: serial wireless không hợp lệ.\n", slot->serial);
            return -1;
        }
        memcpy(slot->adb_dst_host, slot->serial, n);
        slot->adb_dst_host[n] = '\0';
        slot->adb_dst_port = atoi(colon + 1);
    }

    if (slot->stream_base) {
        for (int k = 0; k < STREAM_COUNT; k++) {
            if (adb_forward(slot->serial, slot->stream_base + k + LOC_OFFSET, STREAM_BASE + k) !=
                0) {
                printf("… %s: stream forward k=%d thất bại — phiên sẽ đi adb tunnel.\n",
                       slot->serial, k);
                slot->stream_base = 0; /* bỏ relay stream cho slot này → discovery báo 0 */
                break;
            }
        }
    }
    return 0;
}

/* Bind listener cho slot (gọi MỘT LẦN sau establish_forwards đầu tiên). Cổng adb + dải stream. */
static int bind_listeners(Slot *slot) {
    if (add_listener(slot->adb_port, slot->adb_dst_host, slot->adb_dst_port, NULL, 0) != 0) {
        printf("✗ %s: không bind được cổng adb %d.\n", slot->serial, slot->adb_port);
        return -1;
    }
    if (slot->stream_base) {
        for (int k = 0; k < STREAM_COUNT; k++)
            add_listener(slot->stream_base + k, "127.0.0.1", slot->stream_base + k + LOC_OFFSET,
                         &slot->stream_groups[k], 0);
    }
    return 0;
}

/* Dựng slot mới (chạy KHÔNG giữ lock — adb chậm). Đặt ready=1 dưới lock ở cuối để discovery chỉ
 * liệt kê khi đã sẵn sàng. */
static void slot_setup_new(Slot *slot) {
    if (establish_forwards(slot) != 0) return; /* ready giữ 0 → không liệt kê */
    if (bind_listeners(slot) != 0) return;
    pthread_mutex_lock(&g_lock);
    slot->ready = 1;
    pthread_mutex_unlock(&g_lock);
    printf("✓ %s (%s) slot %d: adb %d, %s\n", slot->model[0] ? slot->model : slot->kind,
           slot->serial, slot->idx, slot->adb_port,
           slot->stream_base ? "stream LAN trực tiếp" : "stream qua adb tunnel");
}

/* Sinh đáp ứng AGENT_PROTOCOL §1.2 và ghi ra socket. Đọc bảng slot dưới lock. */
void discovery_write(int fd) {
    char buf[4096];
    int off = snprintf(buf, sizeof buf, "RCAGENT %d\n", AGENT_PROTOCOL_VERSION);
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nslots && off < (int)sizeof buf; i++) {
        Slot *s = &g_slots[i];
        if (!s->present || !s->ready) continue;
        off += snprintf(buf + off, sizeof buf - (size_t)off, "%d\t%s\t%s\t%s\t%d\n", s->adb_port,
                        s->kind, s->serial, s->model[0] ? s->model : "-", s->stream_base);
    }
    pthread_mutex_unlock(&g_lock);
    if (off > (int)sizeof buf) off = (int)sizeof buf;
    write_full(fd, buf, (size_t)off);
}

/* Tìm slot theo serial (caller giữ lock). */
static Slot *find_slot(const char *serial) {
    for (int i = 0; i < g_nslots; i++)
        if (strcmp(g_slots[i].serial, serial) == 0) return &g_slots[i];
    return NULL;
}

static int g_slots_full_warned = 0;

/* Một vòng quét: cập nhật present, refresh forward cho máy cắm lại, cấp slot + dựng cho máy mới.
 * `adb devices` lỗi tạm thời → bỏ qua vòng này, không xoá sạch bảng. */
void rescan_once(void) {
    Dev devs[64];
    int n = list_devices(devs, 64);
    if (n < 0) return;

    /* Phase A: present = serial còn trong danh sách; phát hiện cắm lại (vắng→có). */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nslots; i++) {
        int seen = 0;
        for (int j = 0; j < n; j++)
            if (strcmp(g_slots[i].serial, devs[j].serial) == 0) {
                seen = 1;
                break;
            }
        if (seen && !g_slots[i].present && g_slots[i].ready) g_slots[i].reappeared = 1;
        g_slots[i].present = seen;
    }
    pthread_mutex_unlock(&g_lock);

    /* Phase B1: refresh forward cho slot vừa cắm lại (listener đã bind từ trước, giữ nguyên). */
    for (;;) {
        pthread_mutex_lock(&g_lock);
        Slot *slot = NULL;
        for (int i = 0; i < g_nslots; i++)
            if (g_slots[i].reappeared) {
                g_slots[i].reappeared = 0;
                slot = &g_slots[i];
                break;
            }
        pthread_mutex_unlock(&g_lock);
        if (!slot) break;
        establish_forwards(slot);
    }

    /* Phase B2: thiết bị mới → cấp slot rồi dựng (không giữ lock khi chạy adb). */
    for (int j = 0; j < n; j++) {
        pthread_mutex_lock(&g_lock);
        if (find_slot(devs[j].serial)) {
            pthread_mutex_unlock(&g_lock);
            continue;
        }
        if (g_nslots >= MAX_SLOTS) {
            int warn = !g_slots_full_warned;
            g_slots_full_warned = 1;
            pthread_mutex_unlock(&g_lock);
            if (warn)
                fprintf(stderr,
                        "⚠ Đã đủ %d slot — bỏ qua thiết bị mới tới khi khởi động lại agent.\n",
                        MAX_SLOTS);
            continue;
        }
        Slot *slot = &g_slots[g_nslots];
        memset(slot, 0, sizeof *slot);
        snprintf(slot->serial, sizeof slot->serial, "%.*s", (int)sizeof slot->serial - 1,
                 devs[j].serial);
        snprintf(slot->model, sizeof slot->model, "%.*s", (int)sizeof slot->model - 1,
                 devs[j].model);
        slot->kind = dev_kind(slot->serial);
        slot->idx = g_nslots;
        slot->adb_port = g_adb_base + slot->idx;
        slot->stream_base = g_no_stream ? 0 : STREAM_BASE + slot->idx * STREAM_COUNT;
        for (int k = 0; k < STREAM_COUNT; k++) {
            pthread_mutex_init(&slot->stream_groups[k].mu, NULL);
            pthread_cond_init(&slot->stream_groups[k].cv, NULL);
            slot->stream_groups[k].busy = 0;
        }
        slot->present = 1;
        g_nslots++;
        pthread_mutex_unlock(&g_lock);
        slot_setup_new(slot);
    }
}

void *rescan_thread(void *arg) {
    (void)arg;
    while (!g_stop) {
        for (int slept = 0; slept < RESCAN_INTERVAL_MS && !g_stop; slept += 100) sleep_ms(100);
        if (g_stop) break;
        rescan_once();
    }
    return NULL;
}
