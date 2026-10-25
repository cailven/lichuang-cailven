#include "game_launcher.h"

#include "game_ui_common.h"

#include <esp_log.h>

#define TAG "DesktopLauncher"

// 320x240 布局: 顶部状态栏 28px, 中部图标区, 底部提示 24px
static constexpr int kStatusBarHeight = 28;
static constexpr int kHintBarHeight = 24;
static constexpr int kTileSize = 64;
static constexpr int kCellWidth = 96;
static constexpr int kCellHeight = 104;

// 取字符串前 count 个 UTF-8 字符 (symbol 为空时图标块上显示标题首字)
static std::string FirstChars(const std::string& s, int count) {
    std::string out;
    int chars = 0;
    for (size_t i = 0; i < s.size() && chars < count;) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0x80) == 0) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;
        out.append(s, i, len);
        i += len;
        chars++;
    }
    return out;
}

DesktopLauncher::DesktopLauncher(Display* display) : display_(display) {}

void DesktopLauncher::AddApp(AppEntry entry) {
    entries_.push_back(std::move(entry));
}

void DesktopLauncher::Build() {
    built_ = true;
    auto theme = display_ != nullptr ? display_->GetTheme() : nullptr;
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    lv_color_t bg = lvgl_theme != nullptr ? lvgl_theme->background_color() : lv_color_black();
    text_color_ = lvgl_theme != nullptr ? lvgl_theme->text_color() : lv_color_white();

    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, bg, 0);

    // 顶部状态栏: 左侧设备状态/电量
    status_label_ = lv_label_create(screen_);
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 12, (kStatusBarHeight - 20) / 2);
    lv_obj_set_style_text_color(status_label_, text_color_, 0);
    lv_label_set_text(status_label_, "");

    // 中部图标区: 行换行排列, 单元格内 "图标在上 标题在下"
    apps_area_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(apps_area_);
    lv_obj_set_size(apps_area_, 320, 240 - kStatusBarHeight - kHintBarHeight);
    lv_obj_align(apps_area_, LV_ALIGN_TOP_MID, 0, kStatusBarHeight);
    lv_obj_remove_flag(apps_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(apps_area_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(apps_area_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(apps_area_, 8, 0);
    lv_obj_set_style_pad_column(apps_area_, 4, 0);

    // 底部提示
    hint_label_ = lv_label_create(screen_);
    lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_text_color(hint_label_, lv_color_hex(0x9AA0A6), 0);
    lv_label_set_text(hint_label_, "");

    // 状态栏刷新定时器 (仅在桌面可见时运行)
    status_timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<DesktopLauncher*>(lv_timer_get_user_data(timer));
            if (self != nullptr) {
                self->RefreshStatus();
            }
        },
        1000, this);
    lv_timer_pause(status_timer_);
}

void DesktopLauncher::BuildAppButtons(const std::vector<AppEntry>& entries) {
    if (apps_area_ == nullptr) {
        return;
    }
    built_apps_.clear();
    lv_obj_clean(apps_area_);  // 删除旧图标, 全量重建 (数量少, 开销可忽略)

    for (const auto& entry : entries) {
        BuiltApp app;
        app.entry = entry;
        app.symbol_is_text = entry.symbol.empty();

        lv_obj_t* cell = lv_obj_create(apps_area_);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, kCellWidth, kCellHeight);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cell, 6, 0);

        app.tile = lv_obj_create(cell);
        lv_obj_set_size(app.tile, kTileSize, kTileSize);
        lv_obj_set_style_radius(app.tile, 18, 0);
        lv_obj_set_style_bg_color(app.tile, lv_color_hex(entry.color), 0);
        lv_obj_set_style_bg_color(app.tile, lv_color_darken(lv_color_hex(entry.color), LV_OPA_40),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_border_width(app.tile, 0, 0);
        lv_obj_set_style_shadow_width(app.tile, 0, 0);
        lv_obj_add_flag(app.tile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(app.tile, LV_OBJ_FLAG_SCROLLABLE);

        app.symbol = lv_label_create(app.tile);
        lv_obj_center(app.symbol);
        lv_obj_set_style_text_color(app.symbol, lv_color_white(), 0);
        if (app.symbol_is_text) {
            // 占位用英文首字, RefreshFonts 时按当前字体重新选 zh/en 标题首字
            lv_label_set_text(app.symbol, FirstChars(entry.title_en, 2).c_str());
        } else {
            lv_label_set_text(app.symbol, entry.symbol.c_str());
        }
        // 点击回调统一挂 this, 通过事件目标反查是哪个应用 (不缓存元素指针)
        lv_obj_add_event_cb(
            app.tile,
            [](lv_event_t* e) {
                if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                    return;
                }
                auto* self = static_cast<DesktopLauncher*>(lv_event_get_user_data(e));
                lv_obj_t* target = lv_event_get_target_obj(e);
                for (auto& app : self->built_apps_) {
                    if (app.tile == target) {
                        if (app.entry.on_open) {
                            app.entry.on_open();
                        }
                        return;
                    }
                }
            },
            LV_EVENT_CLICKED, this);

        app.title = lv_label_create(cell);
        lv_obj_set_style_text_color(app.title, text_color_, 0);
        lv_label_set_text(app.title, entry.title_en.c_str());

        built_apps_.push_back(app);
    }
}

void DesktopLauncher::RefreshFonts() {
    Theme* theme = display_ != nullptr ? display_->GetTheme() : nullptr;
    const lv_font_t* text_font = SafeThemeFont(theme, false);
    const lv_font_t* icon_font = SafeThemeFont(theme, true);
    for (auto& app : built_apps_) {
        const char* effective =
            PickText(text_font, app.entry.title_zh.c_str(), app.entry.title_en.c_str());
        ApplyFont(app.title, text_font);
        ApplyFont(app.symbol, app.symbol_is_text ? text_font : icon_font);
        lv_label_set_text(app.title, effective);
        if (app.symbol_is_text) {
            lv_label_set_text(app.symbol, FirstChars(effective, 2).c_str());
        }
    }
    ApplyFont(status_label_, text_font);
    ApplyFont(hint_label_, text_font);
    lv_label_set_text(hint_label_,
                      PickText(text_font, "点按打开 · BOOT 键返回小智", "Tap to open | BOOT for AI"));
}

void DesktopLauncher::RefreshStatus() {
    if (status_provider_ == nullptr) {
        return;
    }
    ApplyFont(status_label_, SafeThemeFont(display_ != nullptr ? display_->GetTheme() : nullptr, false));
    lv_label_set_text(status_label_, status_provider_().c_str());
}

void DesktopLauncher::Hide() {
    visible_ = false;
    if (status_timer_ != nullptr) {
        lv_timer_pause(status_timer_);
    }
}

void DesktopLauncher::Show() {
    if (!built_) {
        Build();
    }
    visible_ = true;
    // 静态应用 + 动态扫描 (脚本游戏), 每次进桌面都重建, 上传/删除立即生效
    std::vector<AppEntry> all = entries_;
    if (dynamic_provider_) {
        std::vector<AppEntry> dynamic = dynamic_provider_();
        for (auto& e : dynamic) {
            all.push_back(std::move(e));
        }
    }
    BuildAppButtons(all);
    RefreshFonts();
    RefreshStatus();
    lv_timer_resume(status_timer_);
    lv_timer_reset(status_timer_);
    lv_screen_load(screen_);
    ESP_LOGI(TAG, "Desktop shown (%d apps)", (int)built_apps_.size());
}
