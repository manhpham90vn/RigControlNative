/*
 * adbpath.c — chốt đường dẫn `adb` một lần lúc khởi động rồi ĐƯA THƯ MỤC CHỨA NÓ LÊN ĐẦU $PATH.
 *
 * App mở từ icon/launcher (GNOME, .desktop, file manager) kế thừa PATH tối thiểu
 * "/usr/local/bin:/usr/bin:/bin" — không có ~/Android/Sdk/platform-tools mà người dùng thêm
 * trong ~/.bashrc (thứ chỉ shell tương tác đọc). Khi đó mọi lệnh adb thất bại im lặng: danh
 * sách thiết bị rỗng ("Không thấy thiết bị") dù `adb devices` trong terminal vẫn ra máy, và
 * "Quét agent" liệt kê được thiết bị qua cổng discovery (thuần TCP, không cần adb) nhưng không
 * `adb connect` nổi máy nào.
 *
 * Sửa PATH chứ không chỉ nhớ đường dẫn: libcore spawn `adb` bằng posix_spawnp (core/src/adb.c)
 * và không có API nhận đường dẫn adb, nên chỉ PATH mới chữa được cả push/reverse/shell lúc mở
 * phiên. Đặt RC_ADB_PATH để chỉ đích danh một binary khi máy có nhiều bản adb.
 */
#include "rcgtk.h"

#include <string.h>

static char *g_adb; /* đường dẫn adb đã chốt (sở hữu); NULL = không tìm thấy ở đâu cả */

/* Đường dẫn đầu tiên trỏ tới một file chạy được; NULL nếu không có cái nào. */
static char *first_exe(char *const *paths, guint n) {
    for (guint i = 0; i < n; i++) {
        if (!paths[i] || !*paths[i]) continue;
        if (g_file_test(paths[i], G_FILE_TEST_IS_EXECUTABLE) &&
            !g_file_test(paths[i], G_FILE_TEST_IS_DIR))
            return g_strdup(paths[i]);
    }
    return NULL;
}

/* dir đã có sẵn trong PATH chưa (so khớp nguyên phần tử, không phải substring). */
static gboolean path_contains(const char *path, const char *dir) {
    char **parts = g_strsplit(path, G_SEARCHPATH_SEPARATOR_S, 0);
    gboolean found = FALSE;
    for (int i = 0; parts[i] && !found; i++)
        if (strcmp(parts[i], dir) == 0) found = TRUE;
    g_strfreev(parts);
    return found;
}

void adb_path_init(void) {
    if (g_adb) return;

    /* 1. RC_ADB_PATH — binary hoặc thư mục chứa nó. Sai thì cảnh báo rồi dò tiếp, đừng chết. */
    const char *env = g_getenv("RC_ADB_PATH");
    if (env && *env) {
        char *cand = g_file_test(env, G_FILE_TEST_IS_DIR) ? g_build_filename(env, "adb", NULL)
                                                          : g_strdup(env);
        if (g_file_test(cand, G_FILE_TEST_IS_EXECUTABLE)) {
            g_adb = cand;
        } else {
            g_warning("RC_ADB_PATH=%s không phải adb chạy được — bỏ qua, dò tiếp.", env);
            g_free(cand);
        }
    }

    /* 2. PATH sẵn có — chạy từ terminal thì luôn trúng nhánh này. */
    if (!g_adb) g_adb = g_find_program_in_path("adb");

    /* 3. Vị trí SDK hay gặp — nhánh cứu app mở từ icon/launcher. */
    if (!g_adb) {
        const char *home = g_get_home_dir();
        const char *sdk_home = g_getenv("ANDROID_HOME");
        const char *sdk_root = g_getenv("ANDROID_SDK_ROOT");
        char *c[] = {
            sdk_home && *sdk_home ? g_build_filename(sdk_home, "platform-tools", "adb", NULL)
                                  : NULL,
            sdk_root && *sdk_root ? g_build_filename(sdk_root, "platform-tools", "adb", NULL)
                                  : NULL,
            g_build_filename(home, "Android", "Sdk", "platform-tools", "adb", NULL),
            g_build_filename(home, "Android", "sdk", "platform-tools", "adb", NULL),
            g_build_filename(home, "Library", "Android", "sdk", "platform-tools", "adb", NULL),
            g_strdup("/usr/lib/android-sdk/platform-tools/adb"), /* apt: android-sdk-platform-tools */
            g_strdup("/opt/android-sdk/platform-tools/adb"),
            g_strdup("/usr/local/bin/adb"),
            g_strdup("/opt/homebrew/bin/adb"),
            g_strdup("/snap/bin/adb"),
        };
        g_adb = first_exe(c, G_N_ELEMENTS(c));
        for (guint i = 0; i < G_N_ELEMENTS(c); i++) g_free(c[i]);
    }

    if (!g_adb) {
        g_warning("Không tìm thấy `adb` trong PATH lẫn các vị trí SDK thường gặp. Cài Android "
                  "platform-tools, chạy app từ terminal có adb, hoặc đặt "
                  "RC_ADB_PATH=/đường/dẫn/tới/adb.");
        return;
    }

    /* Thư mục chứa adb lên đầu PATH: popen ở chooser.c và posix_spawnp trong libcore đều chỉ tra
     * PATH. g_setenv gọi setenv() nên environ (thứ posix_spawnp đọc) cũng đổi theo. Gọi trước
     * khi có thread nào chạy — setenv không thread-safe. */
    char *dir = g_path_get_dirname(g_adb);
    const char *path = g_getenv("PATH");
    if (!path || !*path) {
        g_setenv("PATH", dir, TRUE);
    } else if (!path_contains(path, dir)) {
        char *joined = g_strconcat(dir, G_SEARCHPATH_SEPARATOR_S, path, NULL);
        g_setenv("PATH", joined, TRUE);
        g_free(joined);
        g_message("adb ngoài PATH — thêm %s vào PATH cho app + libcore", dir);
    }
    g_free(dir);
    g_message("adb: %s", g_adb);
}

gboolean adb_available(void) { return g_adb != NULL; }

const char *adb_program(void) { return g_adb ? g_adb : "adb"; }
