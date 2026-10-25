#pragma once

// 设置面板: 桌面的原生设置应用 (亮度/音量滑条 + 重新配网 + 重启 + 状态信息)
// 动作全部通过回调注入, 面板本身不依赖板级类。
// 危险操作 (重新配网/重启) 需 3 秒内点两次确认, 防误触。
//
// 线程约定: Show 及回调都在 LVGL 上下文 (持显示锁)。

#include "display.h"

#include <lvgl.h>
#include <functional>
#include <string>

class SettingsPanel {
public:
    struct Callbacks {
        std::function<int()> get_brightness;
        std::function<void(int)> set_brightness;   // 0-100, 由板级决定是否持久化
        std::function<int()> get_volume;
        std::function<void(int)> set_volume;
        std::function<void()> enter_wifi_config;   // 重新配网
        std::function<void()> reboot;
        std::function<std::string()> info;         // 底部状态行 (IP/内存等)
    };

    SettingsPanel(Display* display, Callbacks cb, std::function<void()> on_close);
    void Show();  // 切到设置屏幕 (首次调用构建, 每次刷新滑条位置和信息行)

private:
    void Build();
    void ApplyFonts();
    void ResetConfirm();  // 取消"再点一次确认"状态

    Display* display_;
    Callbacks cb_;
    std::function<void()> on_close_;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* close_icon_ = nullptr;
    lv_obj_t* bright_label_ = nullptr;
    lv_obj_t* bright_slider_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    lv_obj_t* volume_slider_ = nullptr;
    lv_obj_t* wifi_btn_ = nullptr;
    lv_obj_t* wifi_btn_label_ = nullptr;
    lv_obj_t* reboot_btn_ = nullptr;
    lv_obj_t* reboot_btn_label_ = nullptr;
    lv_obj_t* bt_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_timer_t* confirm_timer_ = nullptr;
    int confirming_ = 0;  // 0=无 1=配网待确认 2=重启待确认
    bool built_ = false;
};
