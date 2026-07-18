/*
 * agent.h — types, hằng số và API dùng chung giữa các module của rc-agent.
 *
 * rc-agent chạy trên máy đang cắm thiết bị (macOS/Linux) làm TRẠM CHUNG CHUYỂN: mọi thiết bị adb
 * (emulator, USB, wireless) được expose lại dưới ĐỊA CHỈ MÁY AGENT, mỗi thiết bị một cổng adb +
 * một dải cổng stream riêng. App chỉ nhập IP máy agent, hỏi CỔNG DISCOVERY (mặc định 8888) rồi
 * nhận danh sách thiết bị kèm cổng. Giao thức: docs/AGENT_PROTOCOL.md (nguồn sự thật).
 *
 * Cấu trúc module (một-file-một-nhiệm-vụ):
 *   util.c       tiến trình con adb có timeout + thời gian + write_full + cờ dừng
 *   net.c        socket helper + relay engine (pump 2 chiều, replay token stream)
 *   adb.c        `adb devices`/forward + IP máy agent
 *   discovery.c  bảng slot + cổng discovery + quét lại (rescan) nền
 *   main.c       parse cờ, bind IP, vòng accept
 *
 * C thuần POSIX (poll/getifaddrs/posix_spawnp — không epoll/SOCK_NONBLOCK để build được trên
 * macOS), không phụ thuộc core/GTK; chỉ cần `adb` trong PATH.
 */
#ifndef RC_AGENT_H
#define RC_AGENT_H

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define DEFAULT_TCPIP_PORT 5555
#define DEFAULT_ADB_BASE 15553      /* cổng public đầu tiên cho adb (slot 0) */
#define DEFAULT_DISCOVERY_PORT 8888 /* cổng app hỏi để lấy danh sách thiết bị */
#define STREAM_BASE 27183           /* trùng RC_LAN_BASE_PORT phía app */
#define STREAM_COUNT 4              /* số phiên LAN trực tiếp đồng thời mỗi thiết bị */
#define LOC_OFFSET 10000            /* cổng local trung gian (adb forward) = cổng public + offset */
#define MAX_SLOTS 8                 /* adb 15553-15560, stream 27183-27214 */
#define AGENT_PROTOCOL_VERSION 1    /* banner "RCAGENT <version>" (AGENT_PROTOCOL §1.2) */
#define RESCAN_INTERVAL_MS 3000     /* chu kỳ quét lại adb devices trên thread nền */
#define PRE_CAP 4096 /* dữ liệu client giữ lại để replay trước khi upstream xác nhận */
#define MAX_LISTENERS 128

/* ---------- Relay engine (net.c) ---------- */

/* Nhóm cổng stream của một thiết bị: server trong guest chỉ listen SAU khi desktop deploy,
 * nhưng connect qua adb forward "thành công" ngay cả khi guest chưa listen (adb đóng ngay sau
 * đó). Vì vậy kết nối stream cần: (1) replay dữ liệu đầu (token) khi phải reconnect, (2) tuần
 * tự hoá từng kết nối trong nhóm — server accept theo thứ tự video→audio→control, nếu để kết
 * nối sau vào guest trước trong lúc kết nối trước đang retry thì lệch kênh.
 *
 * Một Group cho MỖI cổng stream (mỗi thiết bị × mỗi k): thứ tự accept chỉ có nghĩa trong cùng
 * một cổng, hai cổng khác nhau không thể lệch kênh của nhau — nên không dùng group toàn cục. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int busy;
} Group;

typedef struct {
    int fd; /* listen fd */
    int pub_port;
    char dst_host[64]; /* đích relay: "127.0.0.1" (emu/USB, adb forward) hoặc ip từ serial
                          (wireless) */
    int dst_port;
    Group *group;     /* NULL = relay trong suốt (cổng adb) */
    int is_discovery; /* 1 = cổng discovery: sinh đáp ứng tại chỗ, không relay */
} Listener;

typedef struct {
    int client;
    char dst_host[64];
    int dst_port;
    Group *group;
    int pub_port;  /* cổng public client đã connect vào (chỉ để log) */
    char peer[46]; /* IP client (chỉ để log) */
} Conn;

/* ---------- Thiết bị + slot ---------- */

typedef struct {
    char serial[128];
    char model[96];
} Dev;

typedef struct {
    char serial[128], model[96];
    const char *kind; /* "emulator" | "USB" | "wireless" */
    char adb_dst_host[64];
    int adb_dst_port; /* đích relay cổng adb */
    int idx;          /* slot index (0..MAX_SLOTS-1) */
    int adb_port;     /* cổng adb public = adb_base + idx */
    int stream_base;  /* cổng public đầu dải stream; 0 = không relay stream */
    int present;      /* còn thấy trong `adb devices` → có liệt kê ở discovery */
    int ready;        /* forward + listener đã dựng xong */
    int reappeared;   /* vừa vắng→có lại, cần refresh forward */
    Group stream_groups[STREAM_COUNT];
} Slot;

/* ---------- Trạng thái chia sẻ ---------- */

/* Cờ dừng đặt bởi SIGINT/SIGTERM (util.c). */
extern volatile sig_atomic_t g_stop;

/* Một mutex bảo vệ cả bảng slot lẫn g_ls/g_nls (discovery.c). Mảng listener CHỈ lớn thêm, không
 * bao giờ co lại (slot không tái dụng — §0.1), nên entry đã tạo là bất biến: vòng accept đọc
 * g_ls[i] với i < snapshot mà không cần giữ lock. */
extern pthread_mutex_t g_lock;
extern Slot g_slots[MAX_SLOTS];
extern int g_nslots;
extern Listener g_ls[MAX_LISTENERS];
extern int g_nls;

/* IP LAN + Tailscale + loopback của máy này — mỗi IP một listener cho cùng một cổng public. */
extern char g_bind_ips[16][64];
extern char g_bind_ifs[16][32];
extern int g_nbind;

/* Tham số CLI (đặt một lần trong main, đọc ở discovery.c). */
extern int g_tcpip_port;
extern int g_adb_base;
extern int g_no_stream;
extern int g_discovery_port;

/* ---------- util.c ---------- */
int64_t now_ms(void);
void sleep_ms(int ms);
/* printf một dòng kèm timestamp HH:MM:SS.mmm — an toàn giữa các thread (một lần ghi/dòng). */
void logts(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Chạy lệnh con đồng bộ có timeout; out != NULL thì capture stdout. Trả 0 nếu exit code 0. */
int run_cmd(const char *const argv[], int timeout_ms, char *out, size_t out_sz);
int write_full(int fd, const void *buf, size_t len);

/* ---------- net.c ---------- */
int tcp_connect_timeout(const char *host, int port, int timeout_ms);
int tcp_listen_addr(const char *ip, int port);
void *conn_thread(void *arg); /* pthread: relay một kết nối rồi free Conn */

/* ---------- adb.c ---------- */
int list_devices(Dev *devs, int cap);
int adb_forward(const char *serial, int loc_port, int dev_port);
void cleanup_forwards(void);
int verify_guest_adb(int loc_port);
const char *dev_kind(const char *serial);
int host_ips(char ips[][64], char names[][32], int cap);

/* ---------- discovery.c ---------- */
/* Bind pub_port trên MỌI IP đã chọn; is_discovery=1 = cổng discovery. Trả 0 nếu bind được ≥1. */
int add_listener(int pub_port, const char *dst_host, int dst_port, Group *group, int is_discovery);
void discovery_write(int fd);   /* sinh + ghi đáp ứng AGENT_PROTOCOL §1.2 */
void rescan_once(void);         /* một vòng quét adb devices → cập nhật slot */
void *rescan_thread(void *arg); /* pthread: rescan_once mỗi RESCAN_INTERVAL_MS */

#endif /* RC_AGENT_H */
