#include "settings_panel.h"

#include "game_ui_common.h"

#include <esp_log.h>
#include <material_symbols.h>

#include <cstdio>

#define TAG "SettingsPanel"

SettingsPanel::SettingsPanel(Display* display, Callbacks cb, std::function<void()> on_close)
    : display_(display), cb_(std::move(cb)), on_close_(std::move(on_close)) {}

static lv_obj_t* MakeSlider(lv_obj_t* parent, int y) {
    lv_obj_t* s = lv_slider_create(parent);
    lv_obj_set_size(s, 288, 12);
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, y);
    lv_slider_set_range(s, 0, 100);
    lv_obj_set_style_radius(s, 6, LV_PART_KNOB);
    lv_obj_set_style_radius(s, 6, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2A343E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x4FC3F7), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    return s;
}

static lv_obj_t* MakeButton(lv_obj_t* parent, lv_obj_t** label_out, int x, int y, int w,
                            uint32_t color, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, 42);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(lv_color_hex(color), LV_OPA_40), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* label = lv_label_create(btn);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    if (label_out != nullptr) {
        *label_out = label;
    }
    return btn;
}

void SettingsPanel::ResetConfirm() {
    confirming_ = 0;
    lv_timer_pause(confirm_timer_);
    // 标签文字在 ApplyFonts/字体状态允许时重设; 这里用当前主题字体
    const lv_font_t* text_font = SafeThemeFont(display_ != nullptr ? display_->GetTheme() : nullptr, false);
    if (wifi_btn_label_ != nullptr) {
        lv_label_set_text(wifi_btn_label_, PickText(text_font, "重新配网", "Wi-Fi Setup"));
    }
    if (reboot_btn_label_ != nullptr) {
        lv_label_set_text(reboot_btn_label_, PickText(text_font, "重启", "Reboot"));
    }
}

void SettingsPanel::Build() {
    built_ = true;
    auto theme = display_ != nullptr ? display_->GetTheme() : nullptr;
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    lv_color_t bg = lvgl_theme != nullptr ? lvgl_theme->background_color() : lv_color_black();

    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, bg, 0);

    // 顶栏
    lv_obj_t* bar = lv_obj_create(screen_);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 320, 34);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    title_label_ = lv_label_create(bar);
    lv_obj_align(title_label_, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_label_set_text(title_label_, "Settings");

    lv_obj_t* close_btn = lv_obj_create(bar);
    lv_obj_set_size(close_btn, 36, 28);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2A2F35), 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(close_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        close_btn,
        [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
            auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
            if (self->on_close_) self->on_close_();
        },
        LV_EVENT_CLICKED, this);
    close_icon_ = lv_label_create(close_btn);
    lv_obj_center(close_icon_);
    lv_obj_set_style_text_color(close_icon_, lv_color_white(), 0);
    lv_label_set_text(close_icon_, MATERIAL_SYMBOLS_CLOSE);

    // 亮度 (y 42..76)
    bright_label_ = lv_label_create(screen_);
    lv_obj_align(bright_label_, LV_ALIGN_TOP_LEFT, 16, 42);
    lv_obj_set_style_text_color(bright_label_, lv_color_hex(0xC8D2DC), 0);
    lv_label_set_text(bright_label_, "Brightness");
    bright_slider_ = MakeSlider(screen_, 60);
    lv_obj_add_event_cb(
        bright_slider_,
        [](lv_event_t* e) {
            auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
            int v = lv_slider_get_value(self->bright_slider_);
            if (self->cb_.set_brightness) self->cb_.set_brightness(v);
        },
        LV_EVENT_VALUE_CHANGED, this);

    // 音量 (y 92..126)
    volume_label_ = lv_label_create(screen_);
    lv_obj_align(volume_label_, LV_ALIGN_TOP_LEFT, 16, 92);
    lv_obj_set_style_text_color(volume_label_, lv_color_hex(0xC8D2DC), 0);
    lv_label_set_text(volume_label_, "Volume");
    volume_slider_ = MakeSlider(screen_, 110);
    lv_obj_add_event_cb(
        volume_slider_,
        [](lv_event_t* e) {
            auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
            int v = lv_slider_get_value(self->volume_slider_);
            if (self->cb_.set_volume) self->cb_.set_volume(v);
        },
        LV_EVENT_VALUE_CHANGED, this);

    // 操作按钮 (y 138..180): 危险操作需 3 秒内两次点击
    wifi_btn_ = MakeButton(screen_, &wifi_btn_label_, 16, 138, 138, 0x8A6D3B, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
        if (self->confirming_ != 1) {
            self->confirming_ = 1;
            lv_label_set_text(self->wifi_btn_label_, "再点一次确认");
            lv_timer_reset(self->confirm_timer_);
            lv_timer_resume(self->confirm_timer_);
            return;
        }
        self->ResetConfirm();
        if (self->cb_.enter_wifi_config) self->cb_.enter_wifi_config();
    }, this);

    reboot_btn_ = MakeButton(screen_, &reboot_btn_label_, 166, 138, 138, 0x8A3A3A, [](lv_event_t* e) {
        if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
        auto* self = static_cast<SettingsPanel*>(lv_event_get_user_data(e));
        if (self->confirming_ != 2) {
            self->confirming_ = 2;
            lv_label_set_text(self->reboot_btn_label_, "再点一次确认");
            lv_timer_reset(self->confirm_timer_);
            lv_timer_resume(self->confirm_timer_);
            return;
        }
        if (self->cb_.reboot) self->cb_.reboot();
    }, this);

    // 确认超时定时器 (3 秒后恢复按钮文字)
    confirm_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<SettingsPanel*>(lv_timer_get_user_data(timer));
            self->ResetConfirm();
        },
        3000, this);
    lv_timer_pause(confirm_timer_);

    // 蓝牙行 (y 192): 本固件未编译蓝牙, 如实说明
    lv_obj_t* bt_icon = lv_label_create(screen_);
    lv_obj_align(bt_icon, LV_ALIGN_TOP_LEFT, 16, 192);
    lv_obj_set_style_text_color(bt_icon, lv_color_hex(0x5A6A7A), 0);
    lv_label_set_text(bt_icon, MATERIAL_SYMBOLS_BLUETOOTH);
    bt_label_ = lv_label_create(screen_);
    lv_obj_align(bt_label_, LV_ALIGN_TOP_LEFT, 42, 196);
    lv_obj_set_style_text_color(bt_label_, lv_color_hex(0x5A6A7A), 0);
    lv_label_set_text(bt_label_, "Bluetooth: not compiled");

    // 底部信息行 (y 222+)
    info_label_ = lv_label_create(screen_);
    lv_obj_align(info_label_, LV_ALIGN_BOTTOM_LEFT, 16, -4);
    lv_obj_set_style_text_color(info_label_, lv_color_hex(0x566878), 0);
    lv_label_set_text(info_label_, "");
}

void SettingsPanel::ApplyFonts() {
    Theme* theme = display_ != nullptr ? display_->GetTheme() : nullptr;
    const lv_font_t* text_font = SafeThemeFont(theme, false);
    const lv_font_t* icon_font = SafeThemeFont(theme, true);
    ApplyFont(title_label_, text_font);
    ApplyFont(bright_label_, text_font);
    ApplyFont(volume_label_, text_font);
    ApplyFont(info_label_, text_font);
    ApplyFont(close_icon_, icon_font);
    ApplyFont(wifi_btn_label_, text_font);
    ApplyFont(reboot_btn_label_, text_font);
    ApplyFont(bt_label_, text_font);
    lv_label_set_text(title_label_, PickText(text_font, "设置", "Settings"));
    lv_label_set_text(bright_label_, PickText(text_font, "屏幕亮度", "Brightness"));
    lv_label_set_text(volume_label_, PickText(text_font, "音量", "Volume"));
    lv_label_set_text(bt_label_, PickText(text_font, "蓝牙: 固件未编译", "Bluetooth: not compiled"));
    if (confirming_ == 0) {
        lv_label_set_text(wifi_btn_label_, PickText(text_font, "重新配网", "Wi-Fi Setup"));
        lv_label_set_text(reboot_btn_label_, PickText(text_font, "重启", "Reboot"));
    }
}

void SettingsPanel::Show() {
    if (!built_) {
        Build();
    }
    confirming_ = 0;
    lv_timer_pause(confirm_timer_);
    ApplyFonts();
    if (cb_.get_brightness) {
        lv_slider_set_value(bright_slider_, cb_.get_brightness(), LV_ANIM_OFF);
    }
    if (cb_.get_volume) {
        lv_slider_set_value(volume_slider_, cb_.get_volume(), LV_ANIM_OFF);
    }
    if (cb_.info) {
        lv_label_set_text(info_label_, cb_.info().c_str());
    }
    lv_screen_load(screen_);
    ESP_LOGI(TAG, "Settings shown");
}
