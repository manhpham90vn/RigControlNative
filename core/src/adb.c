/*
 * adb.c — bọc lệnh adb (push, reverse, chạy server qua app_process).
 * Phase 0: khung + helper exec; các lệnh cụ thể hoàn thiện ở Phase 2.
 */
#include "rc_internal.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

/* Chạy adb đồng bộ; quiet=1 thì nuốt stderr (dùng cho lệnh best-effort như reverse --remove). */
static rc_status adb_run_ex(char *const argv[], int quiet) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_t *pfa = NULL;
    if (quiet) {
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        pfa = &fa;
    }
    pid_t pid;
    int rc = posix_spawnp(&pid, "adb", pfa, NULL, argv, environ);
    if (pfa) posix_spawn_file_actions_destroy(pfa);
    if (rc != 0) return RC_ERR_ADB;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return RC_ERR_ADB;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? RC_OK : RC_ERR_ADB;
}

/* Chạy adb đồng bộ với danh sách argv kết thúc bằng NULL; trả RC_OK nếu exit code 0. */
static rc_status adb_run(char *const argv[]) {
    return adb_run_ex(argv, 0);
}

/* Dựng phần đầu argv có "-s <serial>" nếu serial != NULL. Trả số phần tử đã điền. */
static int prefix_serial(const char *serial, const char *out[], int cap) {
    int i = 0;
    if (i < cap) out[i++] = "adb";
    if (serial && i + 1 < cap) {
        out[i++] = "-s";
        out[i++] = serial;
    }
    return i;
}

rc_status rc_adb_connect(const char *addr) {
    if (!addr || !*addr) return RC_ERR_INVALID_ARG;
    const char *a[4] = {"adb", "connect", addr, NULL};
    rc_status r = adb_run((char *const *)a);
    if (r != RC_OK) return r;
    /* `adb connect` có thể exit 0 kể cả khi thất bại → xác minh bằng get-state. */
    const char *v[5] = {"adb", "-s", addr, "get-state", NULL};
    return adb_run_ex((char *const *)v, 1);
}

rc_status rc_adb_push(const char *serial, const char *local, const char *remote) {
    const char *a[8];
    int i = prefix_serial(serial, a, 8);
    a[i++] = "push";
    a[i++] = local;
    a[i++] = remote;
    a[i] = NULL;
    return adb_run((char *const *)a);
}

rc_status rc_adb_reverse(const char *serial, const char *remote, int local_port) {
    char tcp_spec[32];
    snprintf(tcp_spec, sizeof tcp_spec, "tcp:%d", local_port);
    char remote_spec[128];
    snprintf(remote_spec, sizeof remote_spec, "localabstract:%s", remote);
    const char *a[8];
    int i = prefix_serial(serial, a, 8);
    a[i++] = "reverse";
    a[i++] = remote_spec;
    a[i++] = tcp_spec;
    a[i] = NULL;
    return adb_run((char *const *)a);
}

rc_status rc_adb_reverse_remove(const char *serial, const char *remote) {
    char remote_spec[128];
    snprintf(remote_spec, sizeof remote_spec, "localabstract:%s", remote);
    const char *a[8];
    int i = prefix_serial(serial, a, 8);
    a[i++] = "reverse";
    a[i++] = "--remove";
    a[i++] = remote_spec;
    a[i] = NULL;
    return adb_run_ex((char *const *)a, 1); /* best-effort: nuốt "listener not found" */
}

static const char *codec_name(rc_codec codec) {
    switch (codec) {
    case RC_CODEC_H265:
        return "h265";
    case RC_CODEC_AV1:
        return "av1";
    case RC_CODEC_H264:
    default:
        return "h264";
    }
}

rc_status rc_adb_run_server(const char *serial, const rc_config *cfg, const char *socket_name,
                            int *out_pid) {
    if (!cfg) return RC_ERR_INVALID_ARG;

    /* Server chạy foreground trong `adb shell` và stream tới khi socket đóng → KHÔNG waitpid,
     * ta spawn rồi trả pid để teardown kill/reap. */
    char classpath[64];
    snprintf(classpath, sizeof classpath, "CLASSPATH=%s", RC_SERVER_REMOTE_PATH);
    char kv_max_size[32], kv_bit_rate[32], kv_max_fps[32], kv_codec[24], kv_audio[16],
        kv_control[16], kv_tcp[16], kv_socket[96];
    snprintf(kv_socket, sizeof kv_socket, "socket_name=%s",
             socket_name ? socket_name : RC_LOCALABSTRACT_NAME);
    snprintf(kv_max_size, sizeof kv_max_size, "max_size=%d", cfg->max_size);
    snprintf(kv_bit_rate, sizeof kv_bit_rate, "bit_rate=%d",
             cfg->bit_rate > 0 ? cfg->bit_rate : 8000000);
    snprintf(kv_max_fps, sizeof kv_max_fps, "max_fps=%d", cfg->max_fps > 0 ? cfg->max_fps : 60);
    snprintf(kv_codec, sizeof kv_codec, "codec=%s", codec_name(cfg->codec));
    snprintf(kv_audio, sizeof kv_audio, "audio=%s", cfg->audio ? "true" : "false");
    snprintf(kv_control, sizeof kv_control, "control=%s", cfg->control ? "true" : "false");
    /* USB deploy dùng reverse tunnel → server connect localabstract (tcp=false). */
    snprintf(kv_tcp, sizeof kv_tcp, "tcp=false");

    const char *a[24];
    int i = prefix_serial(serial, a, 24);
    a[i++] = "shell";
    a[i++] = classpath;
    a[i++] = "app_process";
    a[i++] = "/";
    a[i++] = "com.rigcontrol.server.Server";
    a[i++] = kv_socket;
    a[i++] = kv_max_size;
    a[i++] = kv_bit_rate;
    a[i++] = kv_max_fps;
    a[i++] = kv_codec;
    a[i++] = kv_audio;
    a[i++] = kv_control;
    a[i++] = kv_tcp;
    a[i] = NULL;

    pid_t pid;
    if (posix_spawnp(&pid, "adb", NULL, NULL, (char *const *)a, environ) != 0) return RC_ERR_ADB;
    if (out_pid) *out_pid = (int)pid;
    return RC_OK;
}
