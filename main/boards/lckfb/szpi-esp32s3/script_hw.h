#pragma once

// Lua 脚本的硬件访问层: 摄像头 / IMU / 音频 / I2C 扩展口 / GPIO 扩展口
//
// 安全设计 (红线, 不可协商):
// - GPIO10/11 (多功能口) 是电池模块电源保持线, 只开放高阻只读, 无任何写路径
// - I2C 写操作屏蔽板载芯片地址 (0x55 电源SoC / 0x19 电源分配 / 0x18+0x41 音频
//   编解码 / 0x38 触摸 / 0x3C 摄像头), 0x6A(IMU) 与外部传感器可写
// - 音频直写输出通道, 小智正在说话时调用会混音 (文档注明), 不破坏固件
// - 全部操作带超时与错误返回, I2C 瞬断不会 panic (不复用 ESP_ERROR_CHECK 的封装)

#include <audio_codec.h>
#include <driver/i2c_master.h>

#include "esp32_camera.h"

struct lua_State;

class ScriptHardware {
public:
    // 板级构造时传入句柄 (camera 为 nullptr 时相机 API 返回不可用)
    static void Init(i2c_master_bus_handle_t bus, Esp32Camera* camera, AudioCodec* codec);

    // 注册 imu.* / camera.* / sound.* / i2c.* / gpio.* 到沙箱
    static void RegisterApi(lua_State* L);

    // 供绑定层使用的总线句柄访问
    static i2c_master_bus_handle_t bus() { return bus_; }

private:
    // --- Lua 绑定 ---
    static int LuaImuRead(lua_State* L);
    static int LuaCameraSnapshot(lua_State* L);
    static int LuaSoundBeep(lua_State* L);
    static int LuaI2cRead(lua_State* L);
    static int LuaI2cWrite(lua_State* L);
    static int LuaI2cScan(lua_State* L);
    static int LuaGpioRead(lua_State* L);

    // --- IMU (QMI8658 @ 0x6A, ±4g / ±512dps) ---
    static bool ImuEnsureInit();
    static bool ImuReadRaw(int16_t out[6]);

    static i2c_master_bus_handle_t bus_;
    static i2c_master_dev_handle_t imu_dev_;
    static Esp32Camera* camera_;
    static AudioCodec* codec_;
    static bool imu_ok_;
    static bool imu_tried_;
    static int imu_stall_count_;
};
