#pragma once

// 原生游戏应用框架: 桌面上的每个游戏实现 GameApp 接口, 由板级调度器统一管理进出。
// 后续新增原生游戏 = 实现一个 GameApp 子类 + 在桌面上注册一个图标入口。
//
// 线程约定: Enter/Exit 必须在持有显示锁 (DisplayLockGuard / lvgl_port_lock,
// 递归锁可在 LVGL 回调内重入) 的上下文中调用。

#include <lvgl.h>

class GameApp {
public:
    virtual ~GameApp() = default;

    // ASCII 标识, 用于日志和 MCP 工具
    virtual const char* name() const = 0;

    // 进入游戏: 切到游戏屏幕并开始运行 (首次调用时才创建界面对象)
    virtual void Enter() = 0;

    // 退出游戏: 暂停运行 (界面对象保留, 再次 Enter 时重开局)
    virtual void Exit() = 0;

    virtual bool running() const = 0;
};
