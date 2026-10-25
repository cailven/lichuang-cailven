#pragma once

// 脚本游戏存储: 独立 4MB FATFS 分区 ("storage", 带磨损均衡), 挂载在 /apps。
// 游戏脚本 = /apps/<name>.lua, 通过局域网网页上传/删除, 不需要重新烧录固件。

#include <string>
#include <vector>

class ScriptStorage {
public:
    // 挂载分区 (失败则格式化重挂), 幂等
    static bool Mount();

    static bool IsMounted() { return mounted_; }

    // 列出全部脚本, 返回不带扩展名的名字列表
    static std::vector<std::string> ListApps();

    // name 不含扩展名; 内容为脚本全文
    static bool ReadApp(const std::string& name, std::string& out);
    static bool WriteApp(const std::string& name, const std::string& content);
    static bool DeleteApp(const std::string& name);
    // 取脚本文件大小 (桌面元信息缓存用, 未找到返回 false)
    static bool GetAppSize(const std::string& name, size_t& size);

    // 名字只允许字母/数字/下划线/连字符/中文以外的安全字符, 防 path 注入
    static bool IsValidName(const std::string& name);

private:
    static bool mounted_;
};
