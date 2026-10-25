#include "script_storage.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <wear_levelling.h>

#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define TAG "ScriptStorage"

static constexpr const char* kMountPoint = "/apps";
static constexpr const char* kPartitionLabel = "storage";
static constexpr size_t kMaxScriptSize = 128 * 1024;  // 单个脚本上限 128KB

bool ScriptStorage::mounted_ = false;

bool ScriptStorage::Mount() {
    if (mounted_) {
        return true;
    }
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.max_files = 6;
    mount_config.format_if_mount_failed = true;
    mount_config.allocation_unit_size = 4096;

    wl_handle_t wl_handle = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(kMountPoint, kPartitionLabel,
                                                     &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount 'storage' partition at %s: %s", kMountPoint,
                 esp_err_to_name(err));
        return false;
    }
    mounted_ = true;
    mkdir((std::string(kMountPoint)).c_str(), 0775);  // 已存在则忽略
    ESP_LOGI(TAG, "Script storage mounted at %s (partition: %s)", kMountPoint, kPartitionLabel);
    return true;
}

bool ScriptStorage::IsValidName(const std::string& name) {
    if (name.empty() || name.size() > 40) {
        return false;
    }
    for (char c : name) {
        if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static std::string PathOf(const std::string& name) {
    return std::string(kMountPoint) + "/" + name + ".lua";
}

std::vector<std::string> ScriptStorage::ListApps() {
    std::vector<std::string> names;
    if (!mounted_) {
        return names;
    }
    DIR* dir = opendir(kMountPoint);
    if (dir == nullptr) {
        return names;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string fn = ent->d_name;
        if (fn.size() > 4 && fn.compare(fn.size() - 4, 4, ".lua") == 0) {
            names.push_back(fn.substr(0, fn.size() - 4));
        }
    }
    closedir(dir);
    return names;
}

bool ScriptStorage::ReadApp(const std::string& name, std::string& out) {
    if (!mounted_ || !IsValidName(name)) {
        return false;
    }
    FILE* f = fopen(PathOf(name).c_str(), "rb");
    if (f == nullptr) {
        return false;
    }
    out.clear();
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > kMaxScriptSize) {
            break;
        }
    }
    fclose(f);
    return out.size() > 0 && out.size() <= kMaxScriptSize;
}

bool ScriptStorage::WriteApp(const std::string& name, const std::string& content) {
    if (!mounted_ || !IsValidName(name) || content.empty() || content.size() > kMaxScriptSize) {
        return false;
    }
    std::string path = PathOf(name);
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    size_t written = fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (written != content.size()) {
        remove(tmp.c_str());
        return false;
    }
    remove(path.c_str());  // FATFS 的 rename 不覆盖, 先删后改
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    ESP_LOGI(TAG, "App '%s' saved (%u bytes)", name.c_str(), (unsigned)content.size());
    return true;
}

bool ScriptStorage::DeleteApp(const std::string& name) {
    if (!mounted_ || !IsValidName(name)) {
        return false;
    }
    return remove(PathOf(name).c_str()) == 0;
}

bool ScriptStorage::GetAppSize(const std::string& name, size_t& size) {
    if (!mounted_ || !IsValidName(name)) {
        return false;
    }
    struct stat st;
    if (stat(PathOf(name).c_str(), &st) != 0) {
        return false;
    }
    size = (size_t)st.st_size;
    return true;
}
