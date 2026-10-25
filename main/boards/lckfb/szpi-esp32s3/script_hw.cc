#include "script_hw.h"

#include "script_game.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>

// espressif/lua 头文件无 extern "C" 保护, 必须手动包住
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#define TAG "ScriptHW"

// ===== QMI8658 寄存器 (实战派官方例程 02-attitude 的配置) =====
static constexpr uint8_t kQmiAddr = 0x6A;
static constexpr uint8_t kQmiWhoAmI = 0x00;
static constexpr uint8_t kQmiReset = 0x60;
static constexpr uint8_t kQmiCtrl1 = 0x02;  // 地址自动递增
static constexpr uint8_t kQmiCtrl2 = 0x03;  // 加速度: ±4g 250Hz
static constexpr uint8_t kQmiCtrl3 = 0x04;  // 陀螺仪: ±512dps 250Hz
static constexpr uint8_t kQmiCtrl7 = 0x08;  // 传感器使能
static constexpr uint8_t kQmiStatus0 = 0x2E;
static constexpr uint8_t kQmiData = 0x35;  // ax..gz 共 12 字节

// 换算: ±4g → 32768/4 = 8192 LSB/g; ±512dps → 64 LSB/dps
static constexpr float kAccLsbPerG = 8192.0f;
static constexpr float kGyrLsbPerDps = 64.0f;

i2c_master_bus_handle_t ScriptHardware::bus_ = nullptr;
i2c_master_dev_handle_t ScriptHardware::imu_dev_ = nullptr;
Esp32Camera* ScriptHardware::camera_ = nullptr;
AudioCodec* ScriptHardware::codec_ = nullptr;
bool ScriptHardware::imu_ok_ = false;
bool ScriptHardware::imu_tried_ = false;
int ScriptHardware::imu_stall_count_ = 0;
bool ScriptHardware::rgb_swap_ = false;

void ScriptHardware::Init(i2c_master_bus_handle_t bus, Esp32Camera* camera, AudioCodec* codec) {
    bus_ = bus;
    camera_ = camera;
    codec_ = codec;
}

// ===== IMU =====
bool ScriptHardware::ImuEnsureInit() {
    if (imu_tried_) {
        return imu_ok_;
    }
    imu_tried_ = true;
    if (bus_ == nullptr) {
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kQmiAddr,
        .scl_speed_hz = 100000,  // 400k 实测受干扰 (幻影ACK/数据错位), 降速换稳定
    };
    esp_err_t add_err = i2c_master_bus_add_device(bus_, &dev_cfg, &imu_dev_);
    if (add_err != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 add_device failed: %s", esp_err_to_name(add_err));
        return false;
    }
    uint8_t id = 0;
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        if (i2c_master_transmit_receive(imu_dev_, &kQmiWhoAmI, 1, &id, 1, 50) == ESP_OK &&
            id == 0x05) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!ok) {
        ESP_LOGW(TAG, "QMI8658 WHO_AM_I mismatch: 0x%02x (expect 0x05), imu disabled", id);
        return false;
    }
    // 复位后必须等芯片就绪再写配置 (连写会丢配置, 表现为 STATUS0 永不就绪 +
    // 数据寄存器读到 ±满量程垃圾值)
    uint8_t rst[2] = {kQmiReset, 0xb0};
    i2c_master_transmit(imu_dev_, rst, 2, 50);
    vTaskDelay(pdMS_TO_TICKS(15));
    uint8_t seq[][2] = {
        {kQmiCtrl1, 0x40},  // 地址自动递增
        {kQmiCtrl7, 0x03},  // 使能加速度+陀螺仪
        {kQmiCtrl2, 0x95},  // ACC ±4g 250Hz
        {kQmiCtrl3, 0xd5},  // GYR ±512dps 250Hz
    };
    for (auto& w : seq) {
        i2c_master_transmit(imu_dev_, w, 2, 50);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    imu_ok_ = true;
    ESP_LOGI(TAG, "QMI8658 ready");
    return true;
}

bool ScriptHardware::ImuReadRaw(int16_t out[6]) {
    if (!ImuEnsureInit() || imu_dev_ == nullptr) {
        return false;
    }
    // 直接读 12 字节数据寄存器 (250Hz 持续出数, 永远有最新样本)。
    // 不查 STATUS0 就绪位: 该位读后清零, 短重试窗口容易总踩不中 4ms 节拍,
    // 曾表现为"闲置一段时间后读数永远 nil"。
    uint8_t buf[12];
    if (i2c_master_transmit_receive(imu_dev_, &kQmiData, 1, buf, sizeof(buf), 200) != ESP_OK) {
        // I2C 连续失败 → 芯片可能停摆, 下次调用重新初始化
        if (++imu_stall_count_ > 3) {
            imu_stall_count_ = 0;
            imu_ok_ = false;
            imu_tried_ = false;
            ESP_LOGW(TAG, "QMI8658 i2c errors, will re-init on next read");
        }
        return false;
    }
    imu_stall_count_ = 0;
    for (int i = 0; i < 6; i++) {
        out[i] = (int16_t)(buf[i * 2] | (buf[i * 2 + 1] << 8));
    }
    return true;
}

int ScriptHardware::LuaImuRead(lua_State* L) {
    int16_t raw[6];
    if (!ImuReadRaw(raw)) {
        lua_pushnil(L);
        return 1;
    }
    // 返回 ax,ay,az (单位 g), gx,gy,gz (单位 °/s)
    for (int i = 0; i < 3; i++) {
        lua_pushnumber(L, (lua_Number)(raw[i] / kAccLsbPerG));
    }
    for (int i = 3; i < 6; i++) {
        lua_pushnumber(L, (lua_Number)(raw[i] / kGyrLsbPerDps));
    }
    return 6;
}

// ===== 摄像头: 拍一帧并居中裁剪画进游戏画布 (320x240 → 320x200) =====
// 可选参数: camera.snapshot(hflip) — true = 水平镜像 (做镜子用)
int ScriptHardware::LuaCameraSnapshot(lua_State* L) {
    bool hflip = lua_toboolean(L, 1);
    if (camera_ == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (!camera_->Capture()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    auto* app = ScriptGameApp::HostOf(L);
    const camera_fb_t* fb = camera_->current_frame();
    if (app == nullptr || fb == nullptr || fb->width < 320 || fb->height < 200) {
        return 1;  // 无画布 (如元信息预读) 时仅返回拍照结果
    }
    // 逐像素拷贝: fb 是大端 RGB565 (DVP 总线原生序), LVGL 画布要小端 uint16,
    // 直接 memcpy 会红绿蓝错乱 ("滤镜感"); 可选水平镜像 + 红蓝通道交换
    uint16_t* dst = app->canvas_buffer();
    const uint16_t* src = (const uint16_t*)fb->buf;
    int top = (fb->height - 200) / 2;
    for (int row = 0; row < 200; row++) {
        const uint16_t* srow = src + (top + row) * fb->width;
        uint16_t* drow = dst + row * 320;
        for (int col = 0; col < 320; col++) {
            uint16_t v = srow[hflip ? (319 - col) : col];
            v = (uint16_t)((v >> 8) | (v << 8));  // 大端 → 小端
            if (rgb_swap_) {
                v = (uint16_t)((v & 0x07E0) | ((v & 0xF800) >> 11) | ((v & 0x001F) << 11));
            }
            drow[col] = v;
        }
    }
    app->InvalidateCanvas();
    return 1;
}

// 颜色仍不对 (红蓝颠倒) 时: camera.rgb_swap(true)
int ScriptHardware::LuaCameraRgbSwap(lua_State* L) {
    if (lua_gettop(L) >= 1) {
        rgb_swap_ = lua_toboolean(L, 1) != 0;
    }
    lua_pushboolean(L, rgb_swap_);
    return 1;
}

// ===== 声音: 正弦提示音 (直写输出通道, 24kHz 单声道) =====
int ScriptHardware::LuaSoundBeep(lua_State* L) {
    if (codec_ == nullptr) {
        return 0;
    }
    int freq = (int)luaL_optinteger(L, 1, 880);
    int ms = (int)luaL_optinteger(L, 2, 150);
    int vol = (int)luaL_optinteger(L, 3, 60);
    if (freq < 50 || freq > 8000 || ms < 10 || ms > 2000 || vol < 0 || vol > 100) {
        luaL_error(L, "sound.beep: freq 50-8000, ms 10-2000, vol 0-100");
        return 0;
    }
    constexpr int kRate = 24000;
    int n = kRate * ms / 1000;
    float amp = 32767.0f * vol / 100.0f * 0.6f;
    std::vector<int16_t> samples(n);
    int fade = kRate / 200;  // 5ms 淡入淡出防爆音
    for (int i = 0; i < n; i++) {
        float v = sinf(2.0f * (float)M_PI * freq * i / kRate) * amp;
        if (i < fade) v *= (float)i / fade;
        if (i > n - fade) v *= (float)(n - i) / fade;
        samples[i] = (int16_t)v;
    }
    codec_->EnableOutput(true);
    codec_->OutputData(samples);
    codec_->EnableOutput(false);
    return 0;
}

// ===== I2C 扩展口 (共享总线, 板载芯片写保护) =====
static const uint8_t kProtectedAddrs[] = {0x18, 0x19, 0x38, 0x3C, 0x41, 0x55};

static bool AddrProtected(uint8_t addr) {
    for (uint8_t a : kProtectedAddrs) {
        if (a == addr) return true;
    }
    return false;
}

// 临时设备句柄用完即删 (脚本调用量小, 开销可接受)
static bool I2cXfer(uint8_t addr, const uint8_t* w, size_t wlen, uint8_t* r, size_t rlen) {
    if (ScriptHardware::bus() == nullptr) return false;
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(ScriptHardware::bus(), &dev_cfg, &dev) != ESP_OK) {
        return false;
    }
    esp_err_t err;
    if (rlen > 0) {
        err = i2c_master_transmit_receive(dev, w, wlen, r, rlen, 200);
    } else {
        err = i2c_master_transmit(dev, w, wlen, 200);
    }
    i2c_master_bus_rm_device(dev);
    return err == ESP_OK;
}

int ScriptHardware::LuaI2cRead(lua_State* L) {
    uint8_t addr = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t reg = (uint8_t)luaL_checkinteger(L, 2);
    int len = (int)luaL_optinteger(L, 3, 1);
    if (len < 1 || len > 8) {
        luaL_error(L, "i2c.read: len 1-8");
        return 0;
    }
    uint8_t buf[8];
    if (!I2cXfer(addr, &reg, 1, buf, len)) {
        lua_pushnil(L);
        return 1;
    }
    for (int i = 0; i < len; i++) {
        lua_pushinteger(L, buf[i]);
    }
    return len;
}

int ScriptHardware::LuaI2cWrite(lua_State* L) {
    uint8_t addr = (uint8_t)luaL_checkinteger(L, 1);
    uint8_t reg = (uint8_t)luaL_checkinteger(L, 2);
    if (AddrProtected(addr)) {
        luaL_error(L, "i2c.write: 0x%02X is an onboard chip, writes blocked", addr);
        return 0;
    }
    uint8_t buf[9];
    int n = 0;
    while (n < 8 && !lua_isnone(L, 3 + n)) {
        buf[n] = (uint8_t)luaL_checkinteger(L, 3 + n);
        n++;
    }
    if (n == 0) {
        return 0;
    }
    uint8_t wbuf[9] = {reg};
    memcpy(wbuf + 1, buf, n);
    lua_pushboolean(L, I2cXfer(addr, wbuf, n + 1, nullptr, 0) ? 1 : 0);
    return 1;
}

int ScriptHardware::LuaI2cScan(lua_State* L) {
    lua_newtable(L);
    if (ScriptHardware::bus() == nullptr) {
        return 1;
    }
    int idx = 1;
    uint8_t reg = 0x00;
    uint8_t dummy = 0;
    for (uint8_t a = 1; a < 0x7F; a++) {
        // 用受控速率的读事务探测 (i2c_master_probe 速率不可控且实测幻影ACK多)
        if (I2cXfer(a, &reg, 1, &dummy, 1)) {
            lua_pushinteger(L, a);
            lua_rawseti(L, -2, idx++);
        }
    }
    return 1;
}

// ===== GPIO 扩展口 (仅 GPIO10/11, 高阻只读 —— 电源保持线, 禁止输出) =====
int ScriptHardware::LuaGpioRead(lua_State* L) {
    int pin = (int)luaL_checkinteger(L, 1);
    if (pin != 10 && pin != 11) {
        luaL_error(L, "gpio.read: only pin 10/11 exposed (input only)");
        return 0;
    }
    gpio_num_t gpio = (gpio_num_t)pin;
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;  // 高阻态, 不配置任何上下拉 (10/11 是电源保持线)
    gpio_config(&cfg);
    lua_pushinteger(L, gpio_get_level(gpio));
    return 1;
}

// ===== 注册 =====
void ScriptHardware::RegisterApi(lua_State* L) {
    lua_newtable(L);
    lua_pushcfunction(L, LuaImuRead);
    lua_setfield(L, -2, "read");
    lua_setglobal(L, "imu");

    lua_newtable(L);
    lua_pushcfunction(L, LuaCameraSnapshot);
    lua_setfield(L, -2, "snapshot");
    lua_pushcfunction(L, LuaCameraRgbSwap);
    lua_setfield(L, -2, "rgb_swap");
    lua_setglobal(L, "camera");

    lua_newtable(L);
    lua_pushcfunction(L, LuaSoundBeep);
    lua_setfield(L, -2, "beep");
    lua_setglobal(L, "sound");

    lua_newtable(L);
    lua_pushcfunction(L, LuaI2cRead);
    lua_setfield(L, -2, "read");
    lua_pushcfunction(L, LuaI2cWrite);
    lua_setfield(L, -2, "write");
    lua_pushcfunction(L, LuaI2cScan);
    lua_setfield(L, -2, "scan");
    lua_setglobal(L, "i2c");

    lua_newtable(L);
    lua_pushcfunction(L, LuaGpioRead);
    lua_setfield(L, -2, "read");
    lua_setglobal(L, "gpio");
}
