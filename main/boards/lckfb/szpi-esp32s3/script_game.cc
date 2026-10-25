#include "script_game.h"

#include "game_ui_common.h"
#include "script_storage.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>

// espressif/lua 组件的头文件没有 extern "C" 保护, 必须手动包住, 否则 C++ 修饰名无法链接
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cstdio>
#include <cstring>
#include <ctime>

#define TAG "ScriptGame"

static uint16_t To565(lua_Integer rgb) {
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    return Rgb565((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

// ===== Lua 内存分配器: 全部走 PSRAM 并限额 =====
void* ScriptGameApp::LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* used = static_cast<size_t*>(ud);
    if (nsize == 0) {
        heap_caps_free(ptr);
        *used -= osize;
        return nullptr;
    }
    if (nsize > osize && *used + (nsize - osize) > kLuaMemCap) {
        return nullptr;  // 超额: Lua 会以内存错误终止脚本
    }
    void* p = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p != nullptr) {
        *used += nsize - osize;
    }
    return p;
}

// ===== 指令计数钩子: 防死循环 =====
static void CountHook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    luaL_error(L, "instruction limit exceeded (possible infinite loop)");
}

// ===== 建立沙箱 VM =====
lua_State* ScriptGameApp::NewSandboxedState(size_t* mem_counter, std::string& error) {
    *mem_counter = 0;
    lua_State* L = lua_newstate(LuaAlloc, mem_counter, (unsigned)esp_random());
    if (L == nullptr) {
        error = "cannot create Lua state";
        return nullptr;
    }

    // 只开放安全的标准库
    static const struct {
        const char* name;
        lua_CFunction open;
    } safe_libs[] = {
        {"_G", luaopen_base},
        {"string", luaopen_string},
        {"table", luaopen_table},
        {"math", luaopen_math},
    };
    for (auto& lib : safe_libs) {
        luaL_requiref(L, lib.name, lib.open, 1);
        lua_pop(L, 1);
    }

    // 去掉文件/模块加载能力
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushnil(L);
    lua_setglobal(L, "require");

    // print 重定向到日志
    lua_pushcfunction(L, LuaPrint);
    lua_setglobal(L, "print");

    // 防死循环
    lua_sethook(L, CountHook, LUA_MASKCOUNT, kInstrLimit);

    RegisterApi(L);
    return L;
}

void ScriptGameApp::RegisterApi(lua_State* L) {
    // canvas 表
    lua_newtable(L);
    lua_pushcfunction(L, LuaFill);
    lua_setfield(L, -2, "fill");
    lua_pushcfunction(L, LuaRect);
    lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, LuaText);
    lua_setfield(L, -2, "text");
    lua_setglobal(L, "canvas");

    // game 表
    lua_newtable(L);
    lua_pushcfunction(L, LuaSetTick);
    lua_setfield(L, -2, "set_tick_period");
    lua_pushcfunction(L, LuaExit);
    lua_setfield(L, -2, "exit");
    lua_pushcfunction(L, LuaRandom);
    lua_setfield(L, -2, "random");
    lua_pushcfunction(L, LuaMillis);
    lua_setfield(L, -2, "millis");
    lua_pushcfunction(L, LuaTime);
    lua_setfield(L, -2, "time");
    lua_setglobal(L, "game");

    // 宿主指针挂到注册表
    lua_pushlightuserdata(L, nullptr);  // ScriptGameApp 实例在 Load 后回填
    lua_setfield(L, LUA_REGISTRYINDEX, "scriptapp.host");
}

ScriptGameApp* ScriptGameApp::HostOf(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "scriptapp.host");
    auto* self = static_cast<ScriptGameApp*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return self;
}

// ===== canvas / game 绑定 =====
int ScriptGameApp::LuaFill(lua_State* L) {
    auto* self = HostOf(L);
    if (self == nullptr || self->canvas_buf_ == nullptr) return 0;
    uint16_t c = To565(luaL_checkinteger(L, 1));
    uint16_t* p = self->canvas_buf_;
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) p[i] = c;
    return 0;
}

int ScriptGameApp::LuaRect(lua_State* L) {
    auto* self = HostOf(L);
    if (self == nullptr || self->canvas_buf_ == nullptr) return 0;
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    uint16_t c = To565(luaL_checkinteger(L, 5));
    if (w <= 0 || h <= 0) return 0;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > CANVAS_W) w = CANVAS_W - x;
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    for (int row = y; row < y + h; row++) {
        uint16_t* line = self->canvas_buf_ + row * CANVAS_W + x;
        for (int col = 0; col < w; col++) line[col] = c;
    }
    return 0;
}

int ScriptGameApp::LuaText(lua_State* L) {
    auto* self = HostOf(L);
    if (self == nullptr || self->canvas_ == nullptr) return 0;
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char* s = luaL_checkstring(L, 3);
    uint32_t rgb = (uint32_t)luaL_optinteger(L, 4, 0xFFFFFF);
    self->DrawText(x, y, s, rgb);
    return 0;
}

int ScriptGameApp::LuaSetTick(lua_State* L) {
    auto* self = HostOf(L);
    if (self == nullptr || self->timer_ == nullptr) return 0;
    int ms = (int)luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;
    if (ms == 0) {
        lv_timer_pause(self->timer_);
    } else {
        lv_timer_set_period(self->timer_, ms);
        lv_timer_resume(self->timer_);
        lv_timer_reset(self->timer_);
    }
    return 0;
}

int ScriptGameApp::LuaExit(lua_State* L) {
    auto* self = HostOf(L);
    if (self != nullptr) {
        self->exit_pending_ = true;  // 不能在 Lua 调用中直接关 VM, 回调返回后再退
    }
    return 0;
}

int ScriptGameApp::LuaRandom(lua_State* L) {
    int n = (int)luaL_optinteger(L, 1, 2);
    if (n <= 1) {
        lua_pushinteger(L, 0);
    } else {
        lua_pushinteger(L, (lua_Integer)(esp_random() % (uint32_t)n));
    }
    return 1;
}

int ScriptGameApp::LuaMillis(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(esp_timer_get_time() / 1000));
    return 1;
}

// 返回本地时间 h, m, s, year, month, day (系统时间由小智联网校时, 时区在板级设置)
int ScriptGameApp::LuaTime(lua_State* L) {
    time_t now = time(NULL);
    struct tm tm_local = {};
    localtime_r(&now, &tm_local);
    lua_pushinteger(L, tm_local.tm_hour);
    lua_pushinteger(L, tm_local.tm_min);
    lua_pushinteger(L, tm_local.tm_sec);
    lua_pushinteger(L, tm_local.tm_year + 1900);
    lua_pushinteger(L, tm_local.tm_mon + 1);
    lua_pushinteger(L, tm_local.tm_mday);
    return 6;
}

int ScriptGameApp::LuaPrint(lua_State* L) {
    auto* self = HostOf(L);
    const char* s = luaL_tolstring(L, 1, nullptr);
    lua_pop(L, 1);
    ESP_LOGI(TAG, "[%s] %s", self != nullptr ? self->script_name_.c_str() : "?", s ? s : "");
    return 0;
}

// ===== 文字绘制: 借 LVGL 的字形渲染写进画布 =====
void ScriptGameApp::DrawText(int x, int y, const char* utf8, uint32_t rgb) {
    const lv_font_t* font = SafeThemeFont(display_ != nullptr ? display_->GetTheme() : nullptr, false);
    if (font == nullptr || utf8 == nullptr || canvas_ == nullptr) {
        return;
    }
    lv_layer_t layer;
    lv_canvas_init_layer(canvas_, &layer);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = lv_color_hex(rgb);
    dsc.text = utf8;
    dsc.opa = LV_OPA_COVER;

    lv_area_t coords;
    coords.x1 = (int32_t)x;
    coords.y1 = (int32_t)y;
    coords.x2 = coords.x1 + CANVAS_W - 1;
    coords.y2 = coords.y1 + font->line_height - 1;
    lv_draw_label(&layer, &dsc, &coords);
    lv_canvas_finish_layer(canvas_, &layer);
}

void ScriptGameApp::FillRect(int x, int y, int w, int h, uint16_t color565) {
    if (canvas_buf_ == nullptr) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > CANVAS_W) w = CANVAS_W - x;
    if (y + h > CANVAS_H) h = CANVAS_H - y;
    if (w <= 0 || h <= 0) return;
    for (int row = y; row < y + h; row++) {
        uint16_t* line = canvas_buf_ + row * CANVAS_W + x;
        for (int col = 0; col < w; col++) line[col] = color565;
    }
}

// ===== 生命周期 =====
ScriptGameApp::ScriptGameApp(Display* display, std::function<void()> on_exit)
    : display_(display), on_exit_(std::move(on_exit)) {}

ScriptGameApp::~ScriptGameApp() {
    DestroyRuntime();
    if (timer_ != nullptr) {
        lv_timer_del(timer_);
    }
    if (canvas_buf_ != nullptr) {
        heap_caps_free(canvas_buf_);
    }
}

void ScriptGameApp::DestroyRuntime() {
    if (L_ != nullptr) {
        lua_close(L_);
        L_ = nullptr;
    }
    lua_mem_used_ = 0;
}

bool ScriptGameApp::Load(const std::string& app_name) {
    if (!ScriptStorage::ReadApp(app_name, script_src_)) {
        ESP_LOGE(TAG, "Cannot read app '%s'", app_name.c_str());
        return false;
    }
    script_name_ = app_name;
    meta_ = ScriptAppMeta{};
    meta_.title = app_name;
    if (!PeekMeta(script_src_, meta_)) {
        ESP_LOGW(TAG, "App '%s': no meta (using defaults)", app_name.c_str());
    }
    return true;
}

bool ScriptGameApp::PeekMeta(const std::string& source, ScriptAppMeta& meta) {
    std::string error;
    size_t mem = 0;
    lua_State* L = NewSandboxedState(&mem, error);
    if (L == nullptr) {
        return false;
    }
    bool ok = false;
    if (luaL_loadbuffer(L, source.data(), source.size(), "app") == LUA_OK &&
        lua_pcall(L, 0, 0, 0) == LUA_OK) {
        lua_getglobal(L, "app");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "title");
            if (lua_isstring(L, -1)) {
                meta.title = lua_tostring(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, -1, "color");
            if (lua_isinteger(L, -1)) {
                meta.color = (uint32_t)lua_tointeger(L, -1);
            }
            lua_pop(L, 1);
            ok = true;
        }
        lua_pop(L, 1);
    } else {
        const char* err = lua_tostring(L, -1);
        ESP_LOGW(TAG, "Meta peek error: %s", err ? err : "?");
        lua_pop(L, 1);
    }
    lua_close(L);
    return ok;
}

bool ScriptGameApp::SelfTest() {
    std::string error;
    size_t mem = 0;
    lua_State* L = lua_newstate(LuaAlloc, &mem, (unsigned)esp_random());
    if (L == nullptr) {
        ESP_LOGE(TAG, "SelfTest: cannot create Lua state");
        return false;
    }
    static const char* chunk = "local s=0 for i=1,10000 do s=s+i end return s";
    bool ok = luaL_loadstring(L, chunk) == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK;
    if (ok) {
        lua_Integer v = lua_tointeger(L, -1);
        ok = (v == 50005000);
        lua_pop(L, 1);
    } else {
        const char* err = lua_tostring(L, -1);
        ESP_LOGE(TAG, "SelfTest failed: %s", err ? err : "?");
    }
    lua_close(L);
    if (ok) {
        ESP_LOGI(TAG, "Lua engine self-test OK");
    }
    return ok;
}

bool ScriptGameApp::CreateRuntime(std::string& error) {
    DestroyRuntime();
    L_ = NewSandboxedState(&lua_mem_used_, error);
    if (L_ == nullptr) {
        return false;
    }
    // 绑定宿主
    lua_pushlightuserdata(L_, this);
    lua_setfield(L_, LUA_REGISTRYINDEX, "scriptapp.host");
    // 执行脚本顶层 (定义回调与 app 表)
    if (luaL_loadbuffer(L_, script_src_.data(), script_src_.size(), script_name_.c_str()) != LUA_OK ||
        lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L_, -1);
        error = err ? err : "unknown error";
        lua_pop(L_, 1);
        DestroyRuntime();
        return false;
    }
    return true;
}

void ScriptGameApp::Build() {
    built_ = true;

    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_hex(0x0A0C0F), 0);

    lv_obj_t* bar = lv_obj_create(screen_);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, CANVAS_W, TOP_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    title_label_ = lv_label_create(bar);
    lv_obj_align(title_label_, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_text_color(title_label_, lv_color_white(), 0);
    lv_label_set_text(title_label_, "");

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
            auto* self = static_cast<ScriptGameApp*>(lv_event_get_user_data(e));
            if (self->on_exit_) {
                self->on_exit_();
            }
        },
        LV_EVENT_CLICKED, this);

    exit_icon_ = lv_label_create(exit_btn);
    lv_obj_center(exit_icon_);
    lv_obj_set_style_text_color(exit_icon_, lv_color_white(), 0);
    lv_label_set_text(exit_icon_, LV_SYMBOL_CLOSE);

    canvas_buf_ = static_cast<uint16_t*>(
        heap_caps_malloc(CANVAS_W * CANVAS_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (canvas_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate canvas buffer");
        return;
    }
    canvas_ = lv_canvas_create(screen_);
    lv_canvas_set_buffer(canvas_, canvas_buf_, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(canvas_, LV_ALIGN_TOP_MID, 0, TOP_BAR_H);
    lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas_, ScriptGameApp::TouchEventCb, LV_EVENT_ALL, this);

    error_label_ = lv_label_create(screen_);
    lv_obj_set_width(error_label_, 300);
    lv_label_set_long_mode(error_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(error_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_text_color(error_label_, lv_color_hex(0xFF7043), 0);
    lv_obj_set_style_bg_color(error_label_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(error_label_, LV_OPA_70, 0);
    lv_label_set_text(error_label_, "");
    lv_obj_add_flag(error_label_, LV_OBJ_FLAG_HIDDEN);

    timer_ = lv_timer_create(
        [](lv_timer_t* timer) {
            auto* self = static_cast<ScriptGameApp*>(lv_timer_get_user_data(timer));
            if (self != nullptr) {
                self->Tick();
            }
        },
        100, this);
    lv_timer_pause(timer_);
}

void ScriptGameApp::Enter() {
    if (!built_) {
        Build();
    }
    if (canvas_buf_ == nullptr) {
        return;
    }
    running_ = true;
    exit_pending_ = false;

    // 标题栏
    const lv_font_t* text_font = SafeThemeFont(display_ != nullptr ? display_->GetTheme() : nullptr, false);
    const lv_font_t* icon_font = SafeThemeFont(display_ != nullptr ? display_->GetTheme() : nullptr, true);
    ApplyFont(title_label_, text_font);
    ApplyFont(exit_icon_, icon_font);
    ApplyFont(error_label_, text_font);
    lv_label_set_text(title_label_, meta_.title.c_str());
    lv_obj_add_flag(error_label_, LV_OBJ_FLAG_HIDDEN);

    // 全新 VM
    std::string error;
    if (!CreateRuntime(error)) {
        ReportError("load", error.c_str());
    } else {
        FillRect(0, 0, CANVAS_W, CANVAS_H, Rgb565(0x10, 0x14, 0x18));
        CallFn("on_start");
    }

    lv_screen_load(screen_);
    lv_obj_invalidate(canvas_);
    lv_timer_set_period(timer_, 100);
    lv_timer_reset(timer_);
    lv_timer_resume(timer_);
    ESP_LOGI(TAG, "App '%s' started", script_name_.c_str());
}

void ScriptGameApp::Exit() {
    running_ = false;
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    DestroyRuntime();
    ESP_LOGI(TAG, "App '%s' exited (lua mem peak ~%u bytes)", script_name_.c_str(),
             (unsigned)lua_mem_used_);
}

bool ScriptGameApp::CallFn(const char* fn, const char* evt, int x, int y) {
    if (L_ == nullptr) {
        return false;
    }
    lua_getglobal(L_, fn);
    if (!lua_isfunction(L_, -1)) {
        lua_pop(L_, 1);
        return false;  // 没定义该回调, 不是错误
    }
    int nargs = 0;
    if (evt != nullptr) {
        lua_pushstring(L_, evt);
        lua_pushinteger(L_, x);
        lua_pushinteger(L_, y);
        nargs = 3;
    }
    if (lua_pcall(L_, nargs, 0, 0) != LUA_OK) {
        const char* err = lua_tostring(L_, -1);
        ReportError(fn, err ? err : "?");
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

void ScriptGameApp::MaybeFinishExit() {
    if (exit_pending_ && running_) {
        exit_pending_ = false;
        if (on_exit_) {
            on_exit_();  // 回桌面 (会调用 Exit() 关闭 VM)
        }
    }
}

void ScriptGameApp::Tick() {
    if (!running_ || L_ == nullptr) {
        return;
    }
    CallFn("on_tick");
    lv_obj_invalidate(canvas_);
    MaybeFinishExit();
}

void ScriptGameApp::ReportError(const char* where, const char* msg) {
    ESP_LOGE(TAG, "App '%s' error in %s: %s", script_name_.c_str(), where, msg);
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
    if (error_label_ != nullptr && running_) {
        char buf[160];
        snprintf(buf, sizeof(buf), "Lua error [%s]: %.100s", where, msg);
        lv_label_set_text(error_label_, buf);
        lv_obj_remove_flag(error_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void ScriptGameApp::TouchEventCb(lv_event_t* e) {
    auto* self = static_cast<ScriptGameApp*>(lv_event_get_user_data(e));
    if (self == nullptr) return;
    self->TouchEvent(lv_event_get_code(e));
}

void ScriptGameApp::TouchEvent(lv_event_code_t code) {
    if (!running_ || L_ == nullptr) {
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    const char* evt = nullptr;
    if (code == LV_EVENT_PRESSED) {
        evt = "down";
        has_last_touch_ = false;
    } else if (code == LV_EVENT_PRESSING) {
        if (has_last_touch_) {
            int dx = p.x - last_touch_.x;
            int dy = p.y - last_touch_.y;
            if (dx * dx + dy * dy < 9) {
                return;  // 移动小于 3px 不重复上报
            }
        }
        evt = "move";
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (code == LV_EVENT_RELEASED) {
            evt = "up";
        } else {
            return;
        }
    }
    if (evt == nullptr) {
        return;
    }
    last_touch_ = p;
    has_last_touch_ = true;
    // 画布坐标 = 屏幕坐标 - 顶栏高
    CallFn("on_touch", evt, p.x, p.y - TOP_BAR_H);
    lv_obj_invalidate(canvas_);
    MaybeFinishExit();
}
