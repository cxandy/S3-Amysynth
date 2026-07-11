#include "project_fs.h"

#include "sdkconfig.h"
#if CONFIG_SYNTH_PROJECT_STORE

#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "project_fs";

static bool s_mounted = false;

bool project_fs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = PROJECT_FS_BASE,
        .partition_label        = "storage",
        .format_if_mount_failed = true,
        .dont_mount             = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
        s_mounted = false;
        return false;
    }
    s_mounted = true;
    size_t total = 0, used = 0;
    if (project_fs_stats(&total, &used)) {
        ESP_LOGI(TAG, "mounted %s: %u KB used / %u KB total",
                 PROJECT_FS_BASE, (unsigned)(used / 1024), (unsigned)(total / 1024));
    }
    return true;
}

bool project_fs_ok(void) { return s_mounted; }

bool project_fs_stats(size_t *total_bytes, size_t *used_bytes)
{
    if (!s_mounted) return false;
    size_t t = 0, u = 0;
    if (esp_littlefs_info("storage", &t, &u) != ESP_OK) return false;
    if (total_bytes) *total_bytes = t;
    if (used_bytes)  *used_bytes  = u;
    return true;
}

#else /* !CONFIG_SYNTH_PROJECT_STORE */

bool project_fs_init(void)  { return false; }
bool project_fs_ok(void)    { return false; }
bool project_fs_stats(size_t *t, size_t *u) { (void)t; (void)u; return false; }

#endif
