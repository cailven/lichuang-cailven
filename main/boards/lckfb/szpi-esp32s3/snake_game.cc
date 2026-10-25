#include "snake_game.h"

#include "game_ui_common.h"
#include "settings.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <material_symbols.h>

#include <cstdio>
#include <cstring>

#define TAG "SnakeGame"

// 速度: 初始 220ms/步, 每吃一个食物加速 7ms, 下限 90ms
static constexpr int kStartPeriodMs = 220;
static constexpr int kMinPeriodMs = 90;
static constexpr int kSpeedUpMs = 7;

// 配色 (RGB565)
static uint16_t BgColor() {
    return Rgb565(0x10, 0x14, 0x18);
}
static uint16_t GridColor() {
    return Rgb565(0x1B, 0x22, 0x28);
}

SnakeGame::SnakeGame(Display* display, std::function<void()> on_exit)
    : display_(display), on_exit_(std::move(on_exit)) {
    Settings settings("snake", true);
    best_ = settings.GetInt("best", 0);
}

SnakeGame::~SnakeGame() {
    if (timer_ != nullptr) {
        lv_timer_del(timer_);
    }
    if (canvas_buf_ != nullptr) {
        heap_caps_free(canvas_buf_);
    }
}

void SnakeGame::Build() {
    built_ = true;

    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x0A0C0F), 0);

    // 顶栏: 分数 | 最高分 | 退出按钮
    lv_obj_t* bar = lv_obj_create(screen_);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, FIELD_W, TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    score_label_ = lv_label_create(bar);
    lv_obj_align(score_label_, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_color(score_label_, lv_color_white(), 0);
    lv_label_set_text(score_label_, "Score 0");

    best_label_ = lv_label_create(bar);
    lv_obj_align(best_label_, LV_ALIGN_LEFT_MID, 120, 0);
    lv_obj_set_style_text_color(best_label_, lv_color_hex(0x9AA0A6), 0);
    lv_label_set_text(best_label_, "Best 0");

    lv_obj_t* exit_btn = lv_obj_create(bar);
    lv_obj_set_size(exit_btn, 36, 32);
    lv_obj_align(exit_btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(exit_btn, 10, 0);
    lv_obj_set_style_bg_color(exit_btn, lv_color_hex(0x2A2F35), 0);
    lv_obj_set_style_border_width(exit_btn, 0, 0);
    lv_obj_add_flag(exit_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(exit_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        exit_btn,
        [](lv_event_t* e) {
            if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                return;
            }
            auto* self = static_cast<SnakeGame*>(lv_event_get_user_data(e));
            if (self->on_exit_) {
                self->on_exit_();  // 回到桌面 (调度器完成切屏)
            }
        },
        LV_EVENT_CLICKED, this);

    lv_obj_t* exit_icon = lv_label_create(exit_btn);
    lv_obj_center(exit_icon);
    lv_obj_set_style_text_color(exit_icon, lv_color_white(), 0);
    lv_label_set_text(exit_icon, MATERIAL_SYMBOLS_CLOSE);
    exit_icon_ = exit_icon;

    // 游戏画布: PSRAM 中的 RGB565 缓冲, 手写像素
    canvas_buf_ = static_cast<uint16_t*>(
        heap_caps_malloc(FIELD_W * FIELD_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (canvas_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer (%d bytes)", FIELD_W * FIELD_H * 2);
        return;
    }
    canvas_ = lv_canvas_create(screen_);
    lv_canvas_set_buffer(canvas_, canvas_buf_, FIELD_W, FIELD_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas_, LV_ALIGN_TOP_MID, 0, TOP_BAR_H);
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas_, SnakeGame::TouchEventCb, LV_EVENT_ALL, this);

    // 游戏结束浮层 (居中面板: 标题 + 得分/提示)
    game_over_panel_ = lv_obj_create(screen_);
    lv_obj_set_size(game_over_panel_, 232, 96);
    lv_obj_align(game_over_panel_, LV_ALIGN_TOP_MID, 0, TOP_BAR_H + (FIELD_H - 96) / 2);
    lv_obj_set_style_radius(game_over_panel_, 16, 0);
    lv_obj_set_style_bg_color(game_over_panel_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(game_over_panel_, LV_OPA_80, 0);
    lv_obj_set_style_border_width(game_over_panel_, 0, 0);
    lv_obj_set_flex_flow(game_over_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(game_over_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(game_over_panel_, 6, 0);
    // 浮层不拦截点击, 让"点击重开"的触摸穿透到底下的游戏画布
    lv_obj_remove_flag(game_over_panel_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(game_over_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(game_over_panel_, LV_OBJ_FLAG_HIDDEN);

    game_over_title_ = lv_label_create(game_over_panel_);
    lv_obj_set_style_text_color(game_over_title_, lv_color_hex(0xFF7043), 0);
    lv_label_set_text(game_over_title_, "Game Over");

    game_over_detail_ = lv_label_create(game_over_panel_);
    lv_obj_set_style_text_color(game_over_detail_, lv_color_white(), 0);
    lv_label_set_text(game_over_detail_, "");

    // 游戏主循环定时器 (暂停状态创建, Enter 时启动)
    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<SnakeGame*>(lv_timer_get_user_data(timer));
            if (self != nullptr) {
                self->Tick();
            }
        },
        kStartPeriodMs, this);
    lv_timer_pause(timer_);
}

void SnakeGame::FillRect(int x, int y, int w, int h, uint16_t color) {
    if (canvas_buf_ == nullptr || x < 0 || y < 0 || x + w > FIELD_W || y + h > FIELD_H) {
        return;
    }
    for (int row = y; row < y + h; row++) {
        uint16_t* line = canvas_buf_ + row * FIELD_W + x;
        for (int col = 0; col < w; col++) {
            line[col] = color;
        }
    }
}

void SnakeGame::ApplyFonts() {
    Theme* theme = display_ != nullptr ? display_->GetTheme() : nullptr;
    const lv_font_t* text_font = SafeThemeFont(theme, false);
    const lv_font_t* icon_font = SafeThemeFont(theme, true);
    ApplyFont(score_label_, text_font);
    ApplyFont(best_label_, text_font);
    ApplyFont(game_over_title_, text_font);
    ApplyFont(game_over_detail_, text_font);
    ApplyFont(exit_icon_, icon_font);
}

void SnakeGame::SpawnFood() {
    while (true) {
        Cell c;
        c.x = esp_random() % COLS;
        c.y = esp_random() % ROWS;
        bool on_body = false;
        for (int i = 0; i < body_len_; i++) {
            if (body_[i].x == c.x && body_[i].y == c.y) {
                on_body = true;
                break;
            }
        }
        if (!on_body) {
            food_ = c;
            return;
        }
    }
}

void SnakeGame::Reset() {
    body_len_ = 3;
    head_.x = COLS / 2 - 1;
    head_.y = ROWS / 2;
    for (int i = 0; i < body_len_; i++) {
        body_[i].x = head_.x - i;
        body_[i].y = head_.y;
    }
    dir_x_ = 1;
    dir_y_ = 0;
    has_pending_dir_ = false;
    pending_dx_ = 0;
    pending_dy_ = 0;
    score_ = 0;
    game_over_ = false;
    touch_moved_ = false;
    SpawnFood();
    lv_timer_set_period(timer_, kStartPeriodMs);
    lv_obj_add_flag(game_over_panel_, LV_OBJ_FLAG_HIDDEN);
}

void SnakeGame::Enter() {
    if (!built_) {
        Build();
    }
    if (canvas_buf_ == nullptr) {
        return;  // 画布分配失败, 拒绝进入
    }
    running_ = true;
    Reset();
    ApplyFonts();
    char buf[24];
    snprintf(buf, sizeof(buf), "Best %d", best_);
    lv_label_set_text(best_label_, buf);
    Draw();
    lv_screen_load(screen_);
    lv_timer_reset(timer_);
    lv_timer_resume(timer_);
    ESP_LOGI(TAG, "Snake started (best=%d)", best_);
}

void SnakeGame::Exit() {
    running_ = false;
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    ESP_LOGI(TAG, "Snake exited (score=%d)", score_);
}

void SnakeGame::SetPendingDir(int dx, int dy) {
    // 每步只接受一次转向, 且禁止 180° 掉头
    if (has_pending_dir_) {
        return;
    }
    if (dx == -dir_x_ && dy == -dir_y_) {
        return;
    }
    pending_dx_ = dx;
    pending_dy_ = dy;
    has_pending_dir_ = true;
}

void SnakeGame::Tick() {
    if (game_over_) {
        return;
    }
    if (has_pending_dir_) {
        dir_x_ = pending_dx_;
        dir_y_ = pending_dy_;
        has_pending_dir_ = false;
    }
    head_.x += dir_x_;
    head_.y += dir_y_;

    // 撞墙
    if (head_.x < 0 || head_.x >= COLS || head_.y < 0 || head_.y >= ROWS) {
        GameOver();
        return;
    }
    // 撞自己 (尾巴即将移走的格子不算, 简化处理: 全身都算)
    for (int i = 0; i < body_len_; i++) {
        if (body_[i].x == head_.x && body_[i].y == head_.y) {
            GameOver();
            return;
        }
    }

    bool ate = (head_.x == food_.x && head_.y == food_.y);
    if (ate) {
        score_++;
        if (score_ > best_) {
            best_ = score_;
            Settings settings("snake", true);
            settings.SetInt("best", best_);
        }
        SpawnFood();
        int period = kStartPeriodMs - score_ * kSpeedUpMs;
        if (period < kMinPeriodMs) {
            period = kMinPeriodMs;
        }
        lv_timer_set_period(timer_, period);
    }
    // 蛇身整体后移一格: 吃到食物时保留尾巴 (长度+1), 否则丢弃尾巴
    if (body_len_ < COLS * ROWS) {
        if (ate) {
            memmove(&body_[1], &body_[0], sizeof(Cell) * body_len_);
            body_len_++;
        } else {
            memmove(&body_[1], &body_[0], sizeof(Cell) * (body_len_ - 1));
        }
        body_[0] = head_;
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "Score %d", score_);
    lv_label_set_text(score_label_, buf);
    snprintf(buf, sizeof(buf), "Best %d", best_);
    lv_label_set_text(best_label_, buf);
    Draw();
}

void SnakeGame::GameOver() {
    game_over_ = true;
    // 字体可能已被服务端替换, 显示前重取 (见 game_ui_common.h 说明)
    const lv_font_t* text_font = SafeThemeFont(display_ != nullptr ? display_->GetTheme() : nullptr, false);
    ApplyFont(game_over_title_, text_font);
    ApplyFont(game_over_detail_, text_font);
    lv_label_set_text(game_over_title_, PickText(text_font, "游戏结束", "Game Over"));
    char buf[48];
    snprintf(buf, sizeof(buf), PickText(text_font, "得分 %d，点击重开", "Score %d | Tap to restart"),
             score_);
    lv_label_set_text(game_over_detail_, buf);
    lv_obj_remove_flag(game_over_panel_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Game over, score=%d best=%d", score_, best_);
}

void SnakeGame::Draw() {
    if (canvas_buf_ == nullptr) {
        return;
    }
    // 背景与网格
    FillRect(0, 0, FIELD_W, FIELD_H, BgColor());
    for (int c = 0; c <= COLS; c++) {
        FillRect(c * CELL, 0, 1, FIELD_H, GridColor());
    }
    for (int r = 0; r <= ROWS; r++) {
        FillRect(0, r * CELL, FIELD_W, 1, GridColor());
    }

    // 食物 (红色圆角块)
    FillRect(food_.x * CELL + 4, food_.y * CELL + 4, CELL - 8, CELL - 8, Rgb565(0xF2, 0x5C, 0x5C));

    // 蛇身 (头亮尾暗)
    for (int i = body_len_ - 1; i >= 0; i--) {
        uint8_t level = (uint8_t)(0x30 + (0x60 * (body_len_ - 1 - i)) / (body_len_ > 1 ? body_len_ - 1 : 1));
        uint16_t color = Rgb565(level + 0x20, level + 0x50, level);
        int inset = (i == 0) ? 1 : 2;
        FillRect(body_[i].x * CELL + inset, body_[i].y * CELL + inset, CELL - inset * 2,
                 CELL - inset * 2, color);
    }
    // 蛇头眼睛 (两个小点, 依方向偏移)
    int hx = head_.x * CELL;
    int hy = head_.y * CELL;
    int e1x, e1y, e2x, e2y;
    if (dir_x_ != 0) {
        e1x = e2x = hx + CELL / 2 + dir_x_ * 4;
        e1y = hy + 6;
        e2y = hy + CELL - 8;
    } else {
        e1y = e2y = hy + CELL / 2 + dir_y_ * 4;
        e1x = hx + 6;
        e2x = hx + CELL - 8;
    }
    FillRect(e1x, e1y, 3, 3, 0xFFFF);
    FillRect(e2x, e2y, 3, 3, 0xFFFF);

    lv_obj_invalidate(canvas_);
}

// 触摸: 滑动转向 (支持不抬手连续变向), 短按 = 游戏结束后重开
void SnakeGame::TouchEventCb(lv_event_t* e) {
    auto* self = static_cast<SnakeGame*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();

    if (code == LV_EVENT_PRESSED) {
        if (indev != nullptr) {
            lv_indev_get_point(indev, &self->touch_start_);
        }
        self->touch_moved_ = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (indev == nullptr) {
            return;
        }
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int dx = p.x - self->touch_start_.x;
        int dy = p.y - self->touch_start_.y;
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (adx < 18 && ady < 18) {
            return;
        }
        if (adx > ady) {
            self->SetPendingDir(dx > 0 ? 1 : -1, 0);
        } else {
            self->SetPendingDir(0, dy > 0 ? 1 : -1);
        }
        // 重新锚定起点, 不抬手也能连续转向
        self->touch_start_ = p;
        self->touch_moved_ = true;
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (code == LV_EVENT_RELEASED && !self->touch_moved_ && self->game_over_ && self->running_) {
            self->Reset();
            self->Draw();
            lv_timer_reset(self->timer_);
            lv_timer_resume(self->timer_);
        }
    }
}
