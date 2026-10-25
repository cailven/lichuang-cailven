#pragma once

// 应用桌面 (Launcher): 全屏 LVGL 屏幕, 图标网格布局。
// 小智聊天界面、原生游戏、脚本游戏都作为桌面上的一个图标入口。
// 原生应用静态注册; 脚本应用通过 DynamicAppsProvider 在每次显示时扫描生成。
//
// 线程约定: 公开方法必须在持有显示锁的上下文中调用
// (板级调度器负责加锁; LVGL 事件回调天然持锁)。

#include "display.h"

#include <lvgl.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class DesktopLauncher {
public:
    // 桌面应用入口 (图标 + 标题 + 打开回调)
    // symbol 为空时, 图标块上直接显示标题前两个字 (脚本游戏的默认样式)
    struct AppEntry {
        std::string title_zh;  // 中文标题 (字体未覆盖时自动回退 title_en)
        std::string title_en;  // 英文标题
        std::string symbol;    // Material Symbols 图标字符串, 可为空
        uint32_t color = 0;    // 图标底色
        std::function<void()> on_open;
    };

    explicit DesktopLauncher(Display* display);

    void AddApp(AppEntry entry);

    // 状态栏内容提供者 (设备状态/电量等), 由状态刷新定时器周期调用
    void SetStatusProvider(std::function<std::string()> provider) { status_provider_ = std::move(provider); }

    // 动态应用提供者 (脚本游戏扫描), 每次显示桌面时调用重建图标
    void SetDynamicAppsProvider(std::function<std::vector<AppEntry>()> provider) {
        dynamic_provider_ = std::move(provider);
    }

    void Show();  // 切到桌面屏幕 (首次调用时构建界面, 每次重建图标网格)
    void Hide();  // 离开桌面 (仅停定时器, 切屏由调度器完成)

    bool visible() const { return visible_; }

private:
    struct BuiltApp {
        AppEntry entry;
        bool symbol_is_text = false;  // symbol 为空 → 显示标题首字
        lv_obj_t* tile = nullptr;
        lv_obj_t* symbol = nullptr;
        lv_obj_t* title = nullptr;
    };

    void Build();
    void BuildAppButtons(const std::vector<AppEntry>& entries);
    void RefreshFonts();   // 每次显示时重取主题字体 (字体可能被服务端替换)
    void RefreshStatus();  // 状态栏文本 + 字体

    Display* display_;
    std::vector<AppEntry> entries_;
    std::vector<BuiltApp> built_apps_;
    std::function<std::string()> status_provider_;
    std::function<std::vector<AppEntry>()> dynamic_provider_;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* apps_area_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_timer_t* status_timer_ = nullptr;
    lv_color_t text_color_ = lv_color_white();
    bool built_ = false;
    bool visible_ = false;
};
