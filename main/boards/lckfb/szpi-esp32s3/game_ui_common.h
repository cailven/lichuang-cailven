#pragma once

// 桌面/游戏共用的 LVGL 安全工具
//
// 两个关键约束 (来自实战派定制文档 §4.4 的血泪教训):
// 1. 小智服务器会在运行时推送新字体并释放旧字体对象, 自建标签存裸字体指针会悬挂崩溃,
//    因此每次显示时都要从主题重新取当前字体并校验回调有效;
// 2. 内置字体只包含小智 UI 自身用到的汉字子集 ("智""贪吃蛇"等字不在其中),
//    服务器推送全量字体前中文会缺字, 所以界面文字做中英文自动回退。

#include "display.h"
#include "lvgl_theme.h"

#include <lvgl.h>
#include <cstdint>

// 从显示主题安全取字体 (服务端可能已替换/释放旧字体), 失败返回 nullptr
inline const lv_font_t* SafeThemeFont(Theme* theme, bool icon) {
    if (theme == nullptr) {
        return nullptr;
    }
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto holder = icon ? lvgl_theme->icon_font() : lvgl_theme->text_font();
    if (holder == nullptr || holder->font() == nullptr || holder->font()->get_glyph_dsc == nullptr) {
        return nullptr;
    }
    return holder->font();
}

// 检查字体是否覆盖 s 中的全部多字节字符 (ASCII 默认可用)
inline bool FontHasGlyphs(const lv_font_t* font, const char* s) {
    if (font == nullptr || font->get_glyph_dsc == nullptr || s == nullptr) {
        return false;
    }
    lv_font_glyph_dsc_t dsc;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(s);
    while (*p) {
        uint32_t cp = 0;
        int extra = 0;
        if ((*p & 0x80) == 0) {
            cp = *p++;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p++ & 0x1F;
            extra = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p++ & 0x0F;
            extra = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p++ & 0x07;
            extra = 3;
        } else {
            p++;
            continue;
        }
        while (extra-- > 0 && (*p & 0xC0) == 0x80) {
            cp = (cp << 6) | (*p++ & 0x3F);
        }
        if (cp > 0x7F && !font->get_glyph_dsc(font, &dsc, cp, 0)) {
            return false;
        }
    }
    return true;
}

// 中文可用则用中文, 否则回退英文 (每次显示时调用, 字体到位后自动切中文)
inline const char* PickText(const lv_font_t* font, const char* zh, const char* en) {
    return FontHasGlyphs(font, zh) ? zh : en;
}

// 给标签设置已校验的主题字体 (nullptr 时保留原字体)
inline void ApplyFont(lv_obj_t* label, const lv_font_t* font) {
    if (label != nullptr && font != nullptr) {
        lv_obj_set_style_text_font(label, font, 0);
    }
}

// RGB565 打包 (LV_COLOR_DEPTH=16, 手写画布缓冲用)
inline uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
