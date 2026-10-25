#pragma once

// 贪吃蛇: 320x240 上 16x10 格 (20px/格), 触摸滑动转向, 点击重开, 顶栏计分/退出。
// 游戏画面用手写 RGB565 画布缓冲 (PSRAM), 游戏循环挂在 LVGL 定时器上。
//
// 线程约定: Enter/Exit 必须在持有显示锁的上下文中调用。

#include "display.h"
#include "game_framework.h"

#include <lvgl.h>
#include <cstdint>
#include <functional>

class SnakeGame : public GameApp {
public:
    SnakeGame(Display* display, std::function<void()> on_exit);
    ~SnakeGame() override;

    const char* name() const override { return "snake"; }
    void Enter() override;
    void Exit() override;
    bool running() const override { return running_; }

private:
    static constexpr int COLS = 16;
    static constexpr int ROWS = 10;
    static constexpr int CELL = 20;
    static constexpr int FIELD_W = COLS * CELL;  // 320
    static constexpr int FIELD_H = ROWS * CELL;  // 200
    static constexpr int TOP_BAR_H = 40;

    struct Cell {
        int8_t x;
        int8_t y;
    };

    void Build();       // 首次 Enter 时创建界面
    void Reset();       // 开新局
    void Tick();        // 游戏主循环 (LVGL 定时器)
    void GameOver();
    void Draw();        // 重画整个画布
    void SetPendingDir(int dx, int dy);
    void SpawnFood();
    void ApplyFonts();  // 每次显示/刷新时重取主题字体
    void FillRect(int x, int y, int w, int h, uint16_t color);
    static void TouchEventCb(lv_event_t* e);

    Display* display_;
    std::function<void()> on_exit_;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* score_label_ = nullptr;
    lv_obj_t* best_label_ = nullptr;
    lv_obj_t* exit_icon_ = nullptr;
    lv_obj_t* game_over_panel_ = nullptr;
    lv_obj_t* game_over_title_ = nullptr;
    lv_obj_t* game_over_detail_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint16_t* canvas_buf_ = nullptr;

    Cell body_[COLS * ROWS];
    int body_len_ = 0;
    Cell head_{};
    Cell food_{};
    int dir_x_ = 1;
    int dir_y_ = 0;
    int pending_dx_ = 0;
    int pending_dy_ = 0;
    bool has_pending_dir_ = false;
    int score_ = 0;
    int best_ = 0;
    bool game_over_ = false;
    bool running_ = false;
    bool built_ = false;

    // 触摸滑动起点与已响应标记
    lv_point_t touch_start_{};
    bool touch_moved_ = false;
};
