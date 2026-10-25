#pragma once

// Lua 脚本游戏宿主: 加载 /apps 下的 .lua 脚本作为桌面游戏运行。
//
// 沙箱设计:
// - 只开放 base/string/table/math 标准库, 无 os/io/package/debug, 无文件访问
// - Lua 内存走 PSRAM 且限额 512KB (自定义分配器), 不吃内部 RAM
// - 指令计数钩子防死循环 (500 万指令/次回调), 超限报错而不是卡死整机
// - 所有脚本调用走 pcall, 报错只显示在游戏界面上, 不影响小智固件
//
// 脚本 API:
//   app = { title="标题", color=0xRRGGBB }        -- 元信息 (title 默认=文件名)
//   canvas.fill(color)  canvas.rect(x,y,w,h,color)  canvas.text(x,y,str,color)
//   game.set_tick_period(ms)  game.exit()  game.random(n)  game.millis()
//   回调: on_start()  on_tick()  on_touch(evt, x, y)   -- evt: "down"/"move"/"up"
// 颜色一律 0xRRGGBB; 画布 320x200; 每次回调后自动刷新画面。
//
// 线程约定: 所有方法在持有显示锁的上下文调用 (同 GameApp)。

#include "display.h"
#include "game_framework.h"

#include <lvgl.h>

#include <cstddef>
#include <functional>
#include <string>

struct lua_State;

struct ScriptAppMeta {
    std::string title;              // 默认用文件名
    uint32_t color = 0x5C6BC0;      // 桌面图标底色
};

class ScriptGameApp : public GameApp {
public:
    ScriptGameApp(Display* display, std::function<void()> on_exit);
    ~ScriptGameApp() override;

    // 读取脚本并解析元信息 (不建界面); 失败返回 false
    bool Load(const std::string& name);
    const std::string& script_name() const { return script_name_; }
    const ScriptAppMeta& meta() const { return meta_; }

    const char* name() const override { return script_name_.c_str(); }
    void Enter() override;
    void Exit() override;
    bool running() const override { return running_; }

    // 静态工具: 解析脚本源码元信息 (临时 VM, 用完即关)
    static bool PeekMeta(const std::string& source, ScriptAppMeta& meta);
    // 开机自检: 验证 Lua 引擎可创建可执行 (临时 VM)
    static bool SelfTest();

private:
    static constexpr int CANVAS_W = 320;
    static constexpr int CANVAS_H = 200;
    static constexpr int TOP_BAR_H = 40;
    static constexpr size_t kLuaMemCap = 512 * 1024;   // Lua 堆上限 (PSRAM)
    static constexpr int kInstrLimit = 5000000;        // 单次回调指令上限

    bool CreateRuntime(std::string& error);   // 建 VM + 沙箱 + 执行脚本顶层
    void DestroyRuntime();
    bool CallFn(const char* fn, const char* evt = nullptr, int x = 0, int y = 0);
    void MaybeFinishExit();                   // 回调返回后处理 game.exit()
    void Build();
    void Tick();
    void DrawText(int x, int y, const char* utf8, uint32_t rgb);
    void FillRect(int x, int y, int w, int h, uint16_t color565);
    void ReportError(const char* where, const char* msg);
    void TouchEvent(lv_event_code_t code);
    static void TouchEventCb(lv_event_t* e);

    // Lua 绑定
    static void RegisterApi(lua_State* L);
    static ScriptGameApp* HostOf(lua_State* L);
    static int LuaFill(lua_State* L);
    static int LuaRect(lua_State* L);
    static int LuaText(lua_State* L);
    static int LuaSetTick(lua_State* L);
    static int LuaExit(lua_State* L);
    static int LuaRandom(lua_State* L);
    static int LuaMillis(lua_State* L);
    static int LuaTime(lua_State* L);
    static int LuaSave(lua_State* L);
    static int LuaLoad(lua_State* L);
    static int LuaPrint(lua_State* L);
    static void* LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static lua_State* NewSandboxedState(size_t* mem_counter, std::string& error);

    Display* display_;
    std::function<void()> on_exit_;
    std::string script_name_;
    std::string script_src_;
    ScriptAppMeta meta_;

    lua_State* L_ = nullptr;
    size_t lua_mem_used_ = 0;
    bool exit_pending_ = false;
    bool running_ = false;
    bool built_ = false;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* canvas_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* exit_icon_ = nullptr;
    lv_obj_t* error_label_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint16_t* canvas_buf_ = nullptr;
    lv_point_t last_touch_{};
    bool has_last_touch_ = false;
};
