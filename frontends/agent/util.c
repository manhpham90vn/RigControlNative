/*
 * util.c — tiến trình con adb có timeout, thời gian đơn điệu, write đầy, và cờ dừng toàn cục.
 */
#include "agent.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

volatile sig_atomic_t g_stop = 0;

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep_ms(int ms) {
    usleep((useconds_t)ms * 1000);
}

/* Log một dòng kèm giờ HH:MM:SS.mmm. Gom vsnprintf rồi printf MỘT lần để các thread relay
 * không xé dòng của nhau (stdout line-buffered — main.c đã setvbuf _IOLBF). */
void logts(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    printf("[%02d:%02d:%02d.%03d] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
           (int)(ts.tv_nsec / 1000000), msg);
}

/* Chạy đồng bộ; out != NULL thì capture stdout (stderr đi thẳng ra terminal). Quá hạn thì
 * SIGKILL — `adb connect` tới đích chết treo theo TCP SYN retry hàng phút nếu không chặn.
 * Trả 0 nếu exit code 0. */
int run_cmd(const char *const argv[], int timeout_ms, char *out, size_t out_sz) {
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

int write_full(int fd, const void *buf, size_t len) {
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
