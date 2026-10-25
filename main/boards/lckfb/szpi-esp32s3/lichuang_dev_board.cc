#include "wifi_board.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "display/emote_display.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "i2c_device.h"
#include "esp32_camera.h"
#include "mcp_server.h"
#include "press_to_talk_mcp_tool.h"
#include "lvgl_theme.h"
#include "settings.h"
#include "game_launcher.h"
#include "game_ui_common.h"
#include "script_game.h"
#include "script_storage.h"
#include "script_hw.h"
#include "app_upload_server.h"
#include "demo_lua_game.h"
#include "snake_lua_game.h"

#include <esp_netif.h>

#include <map>
#include <utility>

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_touch_ft5x06.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <material_symbols.h>
#include <cstdio>
#include <cstring>

#define TAG "LichuangDevBoard"

class Pca9557 : public I2cDevice {
public:
    Pca9557(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteRegSafe(0x01, 0x03);
        WriteRegSafe(0x03, 0xf8);
        output_state_ = 0x03;
    }

    void SetOutputState(uint8_t bit, uint8_t level) {
        // I2cDevice::ReadReg 内部是 ESP_ERROR_CHECK, I2C 瞬断会 panic 重启整台设备
        // (sound.beep 的 EnableOutput 路径曾触发)。这里全部走带重试的安全读写,
        // 读失败时退回上次写入的缓存值, 最多导致本轮开关无效, 绝不崩溃。
        uint8_t data = 0;
        if (!ReadRegSafe(0x01, &data)) {
            data = output_state_;  // I2C 暂时不可达, 用缓存
        }
        data = (data & ~(1 << bit)) | (level << bit);
        if (WriteRegSafe(0x01, data)) {
            output_state_ = data;
        }
    }

    uint8_t ReadInputPort() {
        uint8_t data = 0;
        ReadRegSafe(0x00, &data);
        return data;
    }

private:
    uint8_t output_state_ = 0;

    bool ReadRegSafe(uint8_t reg, uint8_t* val) {
        for (int i = 0; i < 3; i++) {
            if (i2c_master_transmit_receive(i2c_device_, &reg, 1, val, 1, 50) == ESP_OK) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        return false;
    }

    bool WriteRegSafe(uint8_t reg, uint8_t val) {
        uint8_t buf[2] = {reg, val};
        for (int i = 0; i < 3; i++) {
            if (i2c_master_transmit(i2c_device_, buf, 2, 50) == ESP_OK) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        return false;
    }
};

// 电池模块 I2C 接口 (英集芯 IP5xx 电源SOC, 地址 0x55)
class BatteryDevice : public I2cDevice {
public:
    BatteryDevice(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {}

    // 带错误处理的单字节读取 (运行时轮询用, 避免 ESP_ERROR_CHECK 硬重启)
    esp_err_t ReadRegSafe(uint8_t reg, uint8_t* val) {
        return i2c_master_transmit_receive(i2c_device_, &reg, 1, val, 1, 100);
    }
};

class CustomAudioCodec : public BoxAudioCodec {
private:
    Pca9557* pca9557_;

public:
    CustomAudioCodec(i2c_master_bus_handle_t i2c_bus, Pca9557* pca9557)
        : BoxAudioCodec(i2c_bus, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                        AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                        AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, GPIO_NUM_NC,
                        AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, AUDIO_INPUT_REFERENCE,
                        28.0f,  // Physical MIC1 gain
                        2,      // Physical MIC3 is the playback reference input
                        0.0f),
          pca9557_(pca9557) {}

    virtual void EnableOutput(bool enable) override {
        BoxAudioCodec::EnableOutput(enable);
        if (enable) {
            pca9557_->SetOutputState(1, 1);
        } else {
            pca9557_->SetOutputState(1, 0);
        }
    }
};

class LichuangDevBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t pca9557_handle_;
    Button boot_button_;
    Display* display_;
    Pca9557* pca9557_;
    Esp32Camera* camera_;
    PressToTalkMcpTool* press_to_talk_tool_ = nullptr;

    // 电池监测 (英集芯 IP5xx @ I2C 0x55)
    BatteryDevice* battery_ = nullptr;
    int battery_last_log_level_ = -1;

    // 触摸手势调节亮度/音量
    lv_obj_t* gesture_layer_ = nullptr;
    lv_obj_t* indicator_ = nullptr;
    lv_obj_t* indicator_icon_ = nullptr;
    lv_obj_t* indicator_bar_ = nullptr;
    lv_obj_t* indicator_value_ = nullptr;
    lv_timer_t* indicator_hide_timer_ = nullptr;
    lv_point_t gesture_start_point_{};
    bool gesture_decided_ = false;
    bool gesture_is_brightness_ = false;
    int gesture_start_value_ = 0;
    int gesture_last_value_ = 0;

    // 聊天界面右上角"回桌面"悬浮按钮 (挂 layer_top, 手势层之上, 仅聊天模式显示)
    lv_obj_t* home_btn_ = nullptr;
    lv_obj_t* home_icon_ = nullptr;

    // ===== 应用桌面 + 游戏 (一期原生 / 二期 Lua 脚本) =====
    // 三种界面模式: 小智聊天 (小智原生 UI) / 应用桌面 / 游戏
    enum class UiMode { kChat, kDesktop, kGame };
    UiMode ui_mode_ = UiMode::kChat;
    lv_obj_t* chat_screen_ = nullptr;  // 小智原生聊天屏幕 (首次切屏前捕获)
    DesktopLauncher* launcher_ = nullptr;
    ScriptGameApp* script_app_ = nullptr;  // 脚本游戏宿主 (复用, 每次加载不同脚本)
    GameApp* active_game_ = nullptr;       // 当前打开的游戏 (全部为脚本游戏)
    lv_timer_t* auto_desktop_timer_ = nullptr;
    // 脚本元信息缓存: 名字 → {文件大小, meta}; 桌面扫描时跳过未变化的脚本
    std::map<std::string, std::pair<size_t, ScriptAppMeta>> script_meta_cache_;

    // 打开一个游戏 (原生或脚本), 处理"游戏里切游戏"的情况
    void OpenGame(GameApp* game) {
        if (game == nullptr) {
            return;
        }
        if (ui_mode_ == UiMode::kGame) {
            if (active_game_ == game) {
                return;
            }
            if (active_game_ != nullptr) {
                active_game_->Exit();
            }
            active_game_ = game;
            game->Enter();
            return;
        }
        active_game_ = game;
        SetUiMode(UiMode::kGame);
    }

    // 打开一个脚本游戏 (先加载脚本与元信息)
    void OpenScriptGame(const std::string& name) {
        if (script_app_ != nullptr && script_app_->Load(name)) {
            OpenGame(script_app_);
        }
    }

    void SetUiMode(UiMode mode) {
        if (mode == ui_mode_) {
            return;
        }
        // 启动阶段 (未连上服务器) 锁定聊天界面: 触摸屏上电初始化时可能产生
        // 幽灵触摸 (曾打中"主页"按钮导致桌面在初始化中途切屏 → 崩溃),
        // 自动进桌面的定时器本身会等 idle, 不受此守卫影响
        if (mode != UiMode::kChat &&
            Application::GetInstance().GetDeviceState() == kDeviceStateStarting) {
            return;
        }
        DisplayLockGuard lock(display_);
        if (chat_screen_ == nullptr) {
            chat_screen_ = lv_screen_active();  // 小智聊天界面建在默认屏幕上
        }
        // 手势捕获层只在聊天界面启用 (桌面/游戏的触摸要穿透给自己的界面)
        if (mode != UiMode::kChat && gesture_layer_ != nullptr) {
            lv_obj_add_flag(gesture_layer_, LV_OBJ_FLAG_HIDDEN);
        }
        switch (mode) {
            case UiMode::kChat:
                if (active_game_ != nullptr) {
                    active_game_->Exit();
                }
                if (launcher_ != nullptr) {
                    launcher_->Hide();
                }
                if (gesture_layer_ != nullptr) {
                    lv_obj_remove_flag(gesture_layer_, LV_OBJ_FLAG_HIDDEN);
                }
                // 聊天模式显示"回桌面"按钮; 字体可能被服务端替换, 显示前重取
                if (home_btn_ != nullptr) {
                    const lv_font_t* icon_font =
                        SafeThemeFont(display_ != nullptr ? display_->GetTheme() : nullptr, true);
                    if (icon_font != nullptr && home_icon_ != nullptr) {
                        lv_obj_set_style_text_font(home_icon_, icon_font, 0);
                    }
                    lv_obj_remove_flag(home_btn_, LV_OBJ_FLAG_HIDDEN);
                }
                lv_screen_load(chat_screen_);
                break;
            case UiMode::kDesktop:
                if (active_game_ != nullptr) {
                    active_game_->Exit();
                }
                if (home_btn_ != nullptr) {
                    lv_obj_add_flag(home_btn_, LV_OBJ_FLAG_HIDDEN);
                }
                launcher_->Show();
                break;
            case UiMode::kGame:
                if (launcher_ != nullptr) {
                    launcher_->Hide();
                }
                if (home_btn_ != nullptr) {
                    lv_obj_add_flag(home_btn_, LV_OBJ_FLAG_HIDDEN);
                }
                active_game_->Enter();
                break;
        }
        ui_mode_ = mode;
    }

    // 桌面状态栏文本: 小智设备状态 + 电池
    std::string ComposeStatusText() {
        const char* state = "offline";
        switch (Application::GetInstance().GetDeviceState()) {
            case kDeviceStateIdle:
                state = "idle";
                break;
            case kDeviceStateListening:
                state = "listening";
                break;
            case kDeviceStateSpeaking:
                state = "speaking";
                break;
            case kDeviceStateConnecting:
            case kDeviceStateActivating:
                state = "connecting";
                break;
            case kDeviceStateUpgrading:
                state = "upgrading";
                break;
            case kDeviceStateWifiConfiguring:
                state = "wifi setup";
                break;
            default:
                break;
        }
        std::string text = std::string("AI: ") + state;
        int level = 0;
        bool charging = false;
        bool discharging = false;
        if (GetBatteryLevel(level, charging, discharging)) {
            char battery[28];
            snprintf(battery, sizeof(battery), "  |  %d%%%s", level, charging ? " +" : "");
            text += battery;
        }
        return text;
    }

    void InitializeAppUi() {
        launcher_ = new DesktopLauncher(display_);
        DesktopLauncher::AppEntry xiaozhi_entry;
        xiaozhi_entry.title_zh = "小智";
        xiaozhi_entry.title_en = "AI Chat";
        xiaozhi_entry.symbol = MATERIAL_SYMBOLS_CHAT_BUBBLE;
        xiaozhi_entry.color = 0x1E88E5;
        xiaozhi_entry.on_open = [this]() { SetUiMode(UiMode::kChat); };
        launcher_->AddApp(xiaozhi_entry);

        launcher_->SetStatusProvider([this]() { return ComposeStatusText(); });

        // 脚本游戏: 每次进桌面扫描 /apps, 图标 = 标题首字 + 脚本指定颜色
        // 元信息 (标题/颜色) 按 名字+文件大小 缓存, 没变化的脚本跳过 Lua 编译
        launcher_->SetDynamicAppsProvider([this]() {
            std::vector<DesktopLauncher::AppEntry> entries;
            if (!ScriptStorage::IsMounted()) {
                return entries;
            }
            for (const auto& n : ScriptStorage::ListApps()) {
                size_t size = 0;
                if (!ScriptStorage::GetAppSize(n, size)) {
                    continue;
                }
                auto cached = script_meta_cache_.find(n);
                if (cached == script_meta_cache_.end() || cached->second.first != size) {
                    std::string src;
                    ScriptAppMeta meta;
                    meta.title = n;
                    if (ScriptStorage::ReadApp(n, src)) {
                        ScriptGameApp::PeekMeta(src, meta);
                    }
                    script_meta_cache_[n] = {size, meta};
                    cached = script_meta_cache_.find(n);
                }
                const ScriptAppMeta& meta = cached->second.second;
                DesktopLauncher::AppEntry e;
                e.title_zh = meta.title;  // 中文标题 (字体就绪后显示)
                e.title_en = n;           // 字体未就绪时回退显示文件名
                e.symbol = "";            // 空图标 → 图标块显示标题首字
                e.color = meta.color;
                e.on_open = [this, n]() { OpenScriptGame(n); };
                entries.push_back(std::move(e));
            }
            return entries;
        });

        // 小智就绪 (连上服务器进入 idle) 后自动进入应用桌面
        if (lvgl_port_lock(1000)) {
            auto_desktop_timer_ = lv_timer_create(
                [](lv_timer_t* timer) {
                    auto* self = static_cast<LichuangDevBoard*>(lv_timer_get_user_data(timer));
                    if (self != nullptr &&
                        Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                        lv_timer_pause(timer);  // 只自动进一次
                        // 网络已就绪, 启动游戏上传服务 (构造期启动会在 lwip 初始化前崩)
                        AppUploadServer::Start();
                        // 此刻系统时间已被服务器校准, 打出来核对时区 (game.time 的数据源)
                        time_t now = time(NULL);
                        struct tm tmv = {};
                        localtime_r(&now, &tmv);
                        ESP_LOGI(TAG, "Local time: %04d-%02d-%02d %02d:%02d:%02d",
                                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                                 tmv.tm_min, tmv.tm_sec);
                        self->SetUiMode(UiMode::kDesktop);
                    }
                },
                500, this);
            lvgl_port_unlock();
        }
        ESP_LOGI(TAG, "App launcher ready (xiaozhi + snake + scripts)");
    }

    // 脚本游戏平台: 存储挂载 + 出厂演示 + Lua 自检 + 上传服务 + 硬件开放
    void InitializeScriptPlatform() {
        // 硬件访问层: 把 I2C 总线/摄像头/音频交给脚本沙箱 (含安全红线, 见 script_hw.h)
        ScriptHardware::Init(i2c_bus_, camera_, GetAudioCodec());
        // 时区: 小智联网会用服务器时间校准系统时间 (ota.cc), 但不设置时区;
        // game.time() 依赖 localtime, 这里固定为东八区
        setenv("TZ", "CST-8", 1);
        tzset();
        if (ScriptStorage::Mount()) {
            if (ScriptStorage::ListApps().empty()) {
                ScriptStorage::WriteApp("demo_catch", kDemoLuaGame);
                ESP_LOGI(TAG, "Seeded demo game 'demo_catch'");
            }
            std::string existing;
            if (!ScriptStorage::ReadApp("snake", existing)) {
                ScriptStorage::WriteApp("snake", kSnakeLuaGame);
                ESP_LOGI(TAG, "Seeded lua snake game");
            }
            ScriptGameApp::SelfTest();
        } else {
            ESP_LOGW(TAG, "Script storage unavailable, script games disabled");
        }
        script_app_ = new ScriptGameApp(display_, [this]() { SetUiMode(UiMode::kDesktop); });
        // 上传服务不能在这里启动: 板级构造时 lwip 还没初始化 (tcpip mbox 断言),
        // 推迟到网络就绪 (auto_desktop_timer 首次进入 idle 时) 再启动
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // Initialize PCA9557
        pca9557_ = new Pca9557(i2c_bus_, 0x19);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = GPIO_NUM_40;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = GPIO_NUM_41;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            // During startup (before connected), pressing BOOT button enters Wi-Fi config mode without reboot
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            // 桌面/游戏模式下 BOOT 逐级返回: 游戏 → 桌面 → 小智聊天
            if (ui_mode_ == UiMode::kGame) {
                SetUiMode(UiMode::kDesktop);
                return;
            }
            if (ui_mode_ == UiMode::kDesktop) {
                SetUiMode(UiMode::kChat);
                return;
            }
            // In press-to-talk mode the click event is handled by press down/up
            if (!press_to_talk_tool_ || !press_to_talk_tool_->IsPressToTalkEnabled()) {
                app.ToggleChatState();
            }
        });

        boot_button_.OnPressDown([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                Application::GetInstance().StopListening();
            }
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif

        // 聊天界面长按 BOOT 返回桌面; 按住说话模式下禁用 (长按是说话本身, 不能误触)
        boot_button_.OnLongPress([this]() {
            if (ui_mode_ != UiMode::kChat) {
                return;
            }
            if (press_to_talk_tool_ && press_to_talk_tool_->IsPressToTalkEnabled()) {
                return;
            }
            SetUiMode(UiMode::kDesktop);
        });
    }

    void InitializeSt7789Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = GPIO_NUM_NC;
        io_config.dc_gpio_num = GPIO_NUM_39;
        io_config.spi_mode = 2;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片ST7789
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        
        esp_lcd_panel_reset(panel);
        pca9557_->SetOutputState(0, 0);

        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel, panel_io, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#else
        display_ = new SpiLcdDisplay(panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
#endif
    }

    void InitializeTouch()
    {
        esp_lcd_touch_handle_t tp = nullptr;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_HEIGHT,
            .y_max = DISPLAY_WIDTH,
            .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
            .int_gpio_num = GPIO_NUM_NC, 
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 1,
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = {
            .dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS,
            .control_phase_bytes = 1,
            .dc_bit_offset = 0,
            .lcd_cmd_bits = 8,
            .flags =
            {
                .disable_control_phase = 1,
            }
        };
        tp_io_config.scl_speed_hz = 400000;

        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle) != ESP_OK) {
            ESP_LOGW(TAG, "Failed to create touch panel IO, continuing without touch");
            return;
        }
        if (esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp) != ESP_OK || tp == nullptr) {
            ESP_LOGW(TAG, "FT5x06 touch controller not found, continuing without touch");
            esp_lcd_panel_io_del(tp_io_handle);
            return;
        }

        /* Add touch input (for selected screen) */
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(), 
            .handle = tp,
        };

        if(touch_cfg.disp) {
            lvgl_port_add_touch(&touch_cfg);
        } else {
            ESP_LOGE(TAG, "Touch display is not initialized");
        }
    }

    void InitializeCamera() {
        // Open camera power
        pca9557_->SetOutputState(2, 0);

        camera_config_t config = {};
        config.ledc_channel = LEDC_CHANNEL_2;
        config.ledc_timer = LEDC_TIMER_2;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_sccb_sda = -1;
        config.pin_sccb_scl = CAMERA_PIN_SIOC;
        config.sccb_i2c_port = 1;
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.xclk_freq_hz = XCLK_FREQ_HZ;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_QVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

        camera_ = new Esp32Camera(config);
    }

    void InitializeTools() {
        auto &mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
            "End this conversation and enter WiFi configuration mode.\n"
            "**CAUTION** You must ask the user to confirm this action.",
            PropertyList(), [this](const PropertyList& properties) {
                EnterWifiConfigMode();
                return true;
            });

        // Allow switching between press-to-talk (长按说话) and click-to-talk (单击唤醒)
        press_to_talk_tool_ = new PressToTalkMcpTool();
        press_to_talk_tool_->Initialize();

        // 语音控制桌面/游戏: "打开贪吃蛇" / "回到桌面" / "怎么装游戏"
        mcp_server.AddTool("self.launcher.show",
            "Show the app launcher (desktop) on the screen, where the user can open apps like the AI chat or games.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                SetUiMode(UiMode::kDesktop);
                return true;
            });
        mcp_server.AddTool("self.snake.start",
            "Start the built-in snake game on the screen. The user controls the snake by swiping on the touchscreen.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                std::string src;
                if (ScriptStorage::IsMounted() && ScriptStorage::ReadApp("snake", src)) {
                    OpenScriptGame("snake");
                    return true;
                }
                return "贪吃蛇未安装, 可在安装页上传 snake.lua";
            });
        mcp_server.AddTool("self.snake.stop",
            "Exit the current game and show the app launcher.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                SetUiMode(UiMode::kDesktop);
                return true;
            });
        mcp_server.AddTool("self.apps.install_info",
            "Tell the user how to install new Lua script games over WiFi.",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if (netif == nullptr) {
                    return "WiFi 未连接";
                }
                esp_netif_ip_info_t ip;
                if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
                    return "无法获取 IP";
                }
                char buf[64];
                esp_ip4addr_ntoa(&ip.ip, buf, sizeof(buf));
                return std::string("用手机或电脑连接同一 WiFi, 浏览器打开 http://") + buf + ":8080 即可上传 lua 游戏脚本";
            });
    }

    void InitializeBattery() {
        // 电池模块: 英集芯 IP5xx 电源SOC (I2C 0x55)。
        // 已实测验证: 0xA2/A3 为电池电压 ADC, 公式 2600 + raw*0.26855 (mV);
        // 0x71 bits[7:5] 为充电状态 (0=未充电, 1=涓流, 2=恒流, 3=恒压, 5=充满)。
        uint16_t mv = 0;
        if (ReadBatteryVoltage(mv) && mv > 2500 && mv < 4500) {
            ESP_LOGI(TAG, "Battery module (IP5xx @0x55) detected: %d mV", mv);
        } else {
            ESP_LOGW(TAG, "Battery module at 0x55 not responding, battery display disabled");
        }
    }

    bool ReadBatteryVoltage(uint16_t& mv) {
        if (battery_ == nullptr) {
            return false;
        }
        uint8_t lo = 0, hi = 0;
        if (battery_->ReadRegSafe(0xA2, &lo) != ESP_OK) return false;
        if (battery_->ReadRegSafe(0xA3, &hi) != ESP_OK) return false;
        if (hi & 0x20) {
            return false;  // 负值不会出现在电池电压上, 视为无效
        }
        uint16_t raw = lo + (uint16_t)(hi & 0x3F) * 256;
        mv = (uint16_t)(2600.0f + raw * 0.26855f);
        return true;
    }

    bool ReadBatteryCharging(bool& charging, bool& full) {
        if (battery_ == nullptr) {
            return false;
        }
        uint8_t status = 0;
        if (battery_->ReadRegSafe(0x71, &status) != ESP_OK) {
            return false;
        }
        uint8_t state = (status >> 5) & 0x07;
        charging = (state >= 1 && state <= 3);  // 涓流/恒流/恒压
        full = (state == 5);
        return true;
    }

    // 锂电池开路电压 → 电量百分比 (分段线性, 参考 IP5108 驱动的经验曲线)
    static int SocFromVoltage(uint16_t mv) {
        static const struct {
            uint16_t mv;
            uint8_t soc;
        } curve[] = {
            {3000, 0},  {3300, 3},  {3500, 6},  {3610, 10}, {3650, 13}, {3690, 17},
            {3710, 20}, {3730, 25}, {3750, 30}, {3770, 35}, {3790, 40}, {3800, 45},
            {3820, 50}, {3840, 55}, {3850, 60}, {3870, 65}, {3910, 70}, {3950, 75},
            {3980, 80}, {4020, 85}, {4080, 90}, {4110, 95}, {4200, 100},
        };
        if (mv <= curve[0].mv) {
            return curve[0].soc;
        }
        for (size_t i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
            if (mv <= curve[i].mv) {
                return curve[i - 1].soc +
                       (int)(mv - curve[i - 1].mv) * (curve[i].soc - curve[i - 1].soc) /
                           (curve[i].mv - curve[i - 1].mv);
            }
        }
        return 100;
    }

    void InitializeTouchGestures() {
        if (!lvgl_port_lock(1000)) {
            ESP_LOGE(TAG, "Failed to lock LVGL, touch gestures disabled");
            return;
        }

        // 从显示主题取字体, 取不到时退回 LVGL 默认字体
        const lv_font_t* text_font = lv_font_get_default();
        const lv_font_t* icon_font = lv_font_get_default();
        if (display_ != nullptr && display_->GetTheme() != nullptr) {
            auto theme = static_cast<LvglTheme*>(display_->GetTheme());
            if (theme->text_font() != nullptr && theme->text_font()->font() != nullptr) {
                text_font = theme->text_font()->font();
            }
            if (theme->icon_font() != nullptr && theme->icon_font()->font() != nullptr) {
                icon_font = theme->icon_font()->font();
            }
        }

        // 全屏透明捕获层: 左半屏竖直拖动调亮度, 右半屏调音量
        gesture_layer_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(gesture_layer_);
        lv_obj_set_size(gesture_layer_, LV_PCT(100), LV_PCT(100));
        lv_obj_add_flag(gesture_layer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(gesture_layer_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(gesture_layer_, GestureEventCb, LV_EVENT_ALL, this);

        // 拖动过程中短暂显示的调节指示条 (图标 + 进度条 + 数值)
        indicator_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(indicator_);
        lv_obj_set_size(indicator_, 212, 40);
        lv_obj_set_style_bg_color(indicator_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(indicator_, LV_OPA_70, 0);
        lv_obj_set_style_radius(indicator_, 20, 0);
        lv_obj_align(indicator_, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_set_flex_flow(indicator_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(indicator_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(indicator_, 14, 0);
        lv_obj_set_style_pad_ver(indicator_, 0, 0);
        lv_obj_set_style_pad_column(indicator_, 10, 0);
        lv_obj_add_flag(indicator_, LV_OBJ_FLAG_HIDDEN);

        indicator_icon_ = lv_label_create(indicator_);
        lv_obj_set_style_text_font(indicator_icon_, icon_font, 0);
        lv_obj_set_style_text_color(indicator_icon_, lv_color_white(), 0);
        lv_label_set_text(indicator_icon_, MATERIAL_SYMBOLS_BRIGHTNESS_6);

        indicator_bar_ = lv_bar_create(indicator_);
        lv_obj_set_size(indicator_bar_, 100, 10);
        lv_obj_set_style_radius(indicator_bar_, 5, LV_PART_MAIN);
        lv_obj_set_style_radius(indicator_bar_, 5, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(indicator_bar_, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(indicator_bar_, LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_bg_color(indicator_bar_, lv_color_hex(0xFFB300), LV_PART_INDICATOR);
        lv_bar_set_range(indicator_bar_, 0, 100);

        indicator_value_ = lv_label_create(indicator_);
        lv_obj_set_style_text_font(indicator_value_, text_font, 0);
        lv_obj_set_style_text_color(indicator_value_, lv_color_white(), 0);
        lv_label_set_text(indicator_value_, "0");

        indicator_hide_timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                auto* self = static_cast<LichuangDevBoard*>(lv_timer_get_user_data(timer));
                if (self->indicator_ != nullptr) {
                    lv_obj_add_flag(self->indicator_, LV_OBJ_FLAG_HIDDEN);
                }
                lv_timer_pause(timer);
            },
            1200, this);
        lv_timer_pause(indicator_hide_timer_);

        // 聊天界面的"回桌面"按钮: 创建在手势捕获层之后, 位于其上, 44x44 圆形半透明
        home_btn_ = lv_obj_create(lv_layer_top());
        lv_obj_set_size(home_btn_, 44, 44);
        lv_obj_align(home_btn_, LV_ALIGN_TOP_RIGHT, -8, 40);
        lv_obj_set_style_radius(home_btn_, 22, 0);
        lv_obj_set_style_bg_color(home_btn_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(home_btn_, LV_OPA_60, 0);
        lv_obj_set_style_border_width(home_btn_, 0, 0);
        lv_obj_add_flag(home_btn_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(home_btn_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(
            home_btn_,
            [](lv_event_t* e) {
                if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
                    return;
                }
                auto* self = static_cast<LichuangDevBoard*>(lv_event_get_user_data(e));
                self->SetUiMode(UiMode::kDesktop);
            },
            LV_EVENT_CLICKED, this);

        home_icon_ = lv_label_create(home_btn_);
        lv_obj_center(home_icon_);
        lv_obj_set_style_text_color(home_icon_, lv_color_white(), 0);
        lv_obj_set_style_text_font(home_icon_, icon_font, 0);
        lv_label_set_text(home_icon_, MATERIAL_SYMBOLS_HOME);

        lvgl_port_unlock();
        ESP_LOGI(TAG, "Touch gestures ready: left half = brightness, right half = volume");
    }

    void ShowAdjustIndicator(bool brightness, int value) {
        if (indicator_ == nullptr) {
            return;
        }
        // 主题文本字体可能在运行时被服务端推送的字体替换(SetTextFont 会释放旧字体对象),
        // 标签样式里存的是裸指针, 因此每次显示时都从主题重新取当前字体
        if (display_ != nullptr && display_->GetTheme() != nullptr) {
            auto theme = static_cast<LvglTheme*>(display_->GetTheme());
            if (theme->text_font() != nullptr && theme->text_font()->font() != nullptr &&
                theme->text_font()->font()->get_glyph_dsc != nullptr) {
                lv_obj_set_style_text_font(indicator_value_, theme->text_font()->font(), 0);
            }
            if (theme->icon_font() != nullptr && theme->icon_font()->font() != nullptr &&
                theme->icon_font()->font()->get_glyph_dsc != nullptr) {
                lv_obj_set_style_text_font(indicator_icon_, theme->icon_font()->font(), 0);
            }
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", value);
        lv_label_set_text(indicator_icon_,
                          brightness ? MATERIAL_SYMBOLS_BRIGHTNESS_6 : MATERIAL_SYMBOLS_VOLUME_UP);
        lv_label_set_text(indicator_value_, buf);
        lv_bar_set_value(indicator_bar_, value, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(indicator_bar_,
                                  lv_color_hex(brightness ? 0xFFB300 : 0x4FC3F7),
                                  LV_PART_INDICATOR);
        lv_obj_remove_flag(indicator_, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(indicator_hide_timer_);
        lv_timer_reset(indicator_hide_timer_);
    }

    static void GestureEventCb(lv_event_t* e) {
        auto* self = static_cast<LichuangDevBoard*>(lv_event_get_user_data(e));
        lv_event_code_t code = lv_event_get_code(e);

        if (code == LV_EVENT_PRESSED) {
            lv_indev_t* indev = lv_indev_active();
            if (indev == nullptr) return;
            lv_indev_get_point(indev, &self->gesture_start_point_);
            self->gesture_decided_ = false;
            return;
        }
        if (code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) {
            return;
        }

        if (code == LV_EVENT_PRESSING) {
            lv_indev_t* indev = lv_indev_active();
            if (indev == nullptr) return;
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            int16_t dx = p.x - self->gesture_start_point_.x;
            int16_t dy = self->gesture_start_point_.y - p.y;  // 上滑为正

            if (!self->gesture_decided_) {
                // 横向拖动不处理; 竖向需超过 16px 阈值才判定为调节手势
                int16_t adx = dx < 0 ? -dx : dx;
                int16_t ady = dy < 0 ? -dy : dy;
                if (adx > 24 && adx > ady) return;
                if (ady < 16) return;
                self->gesture_decided_ = true;
                self->gesture_is_brightness_ = self->gesture_start_point_.x < DISPLAY_WIDTH / 2;
                self->gesture_start_value_ = self->gesture_is_brightness_
                                                 ? self->GetBacklight()->brightness()
                                                 : self->GetAudioCodec()->output_volume();
            }

            // 约 150px 竖直滑动覆盖 0~100 全量程
            int value = self->gesture_start_value_ + dy * 100 / 150;
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            self->gesture_last_value_ = value;
            if (self->gesture_is_brightness_) {
                self->GetBacklight()->SetBrightness(value, false);
            } else {
                self->GetAudioCodec()->SetOutputVolume(value);
            }
            self->ShowAdjustIndicator(self->gesture_is_brightness_, value);
            return;
        }

        // LV_EVENT_RELEASED / LV_EVENT_PRESS_LOST: 亮度持久化到设置, 重启后保持
        if (self->gesture_decided_ && self->gesture_is_brightness_) {
            Settings settings("display", true);
            settings.SetInt("brightness", self->gesture_last_value_);
        }
        self->gesture_decided_ = false;
    }

public:
    // BOOT: 短按=原生行为(说话/切对话), 长按(800ms)=回桌面
    LichuangDevBoard() : boot_button_(BOOT_BUTTON_GPIO, false, 800) {
        InitializeI2c();
        InitializeSpi();
        InitializeSt7789Display();
        InitializeTouch();
        InitializeTouchGestures();
        InitializeButtons();
        InitializeCamera();
        InitializeTools();
        InitializeScriptPlatform();
        InitializeAppUi();
        battery_ = new BatteryDevice(i2c_bus_, 0x55);
        InitializeBattery();

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CustomAudioCodec audio_codec(
            i2c_bus_, 
            pca9557_);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (battery_ == nullptr) {
            return false;
        }
        uint16_t mv = 0;
        bool full = false;
        if (!ReadBatteryVoltage(mv) || !ReadBatteryCharging(charging, full)) {
            return false;  // I2C 读取失败, 本轮不显示
        }
        if (mv < 2500 || mv > 4500) {
            return false;  // 电压不合理, 视为无电池
        }
        level = full ? 100 : SocFromVoltage(mv);
        discharging = !charging;
        if (level != battery_last_log_level_) {
            battery_last_log_level_ = level;
            ESP_LOGI(TAG, "Battery: %d%% (%d mV), charging=%d", level, mv, charging ? 1 : 0);
        }
        return true;
    }

    // 小智 idle 时会切 MAX_MODEM 深度省电, 华为等路由器会丢弃发往"沉睡设备"的
    // 来向数据 (局域网 ping/连接全丢, 但出向和语音不受影响); 钳制到 BALANCED
    // (每个 DTIM 唤醒, 来向数据可达, 功耗仅略增), 保住局域网游戏安装页可用
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::LOW_POWER) {
            level = PowerSaveLevel::BALANCED;
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(LichuangDevBoard);
