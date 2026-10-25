# 立创·实战派 ESP32-S3 (lichuang-dev) 小智固件定制 — 交接文档

> **文档目的**: 本文档记录 2026-10-24 对小智固件的定制开发全过程, 供后续开发者或 AI 助手接手。
> **接手须知**: 先读 §2 硬件档案和 §2.4 危险操作清单 (避免烧板/断电事故), 再读 §4 代码改造。
> 源码位置: `~/esp/xiaozhi-esp32` (78/xiaozhi-esp32, main 分支 @ v2.5.3 + 本地修改, 与上游的差异用 `git diff` 查看)。

---

## 1. 项目概览

| 项目 | 内容 |
|---|---|
| 开发板 | 立创·实战派 ESP32-S3 (淘宝 GoCool实验室 套装, 含 2000mAh 电池模块, 已装外壳) |
| 主控 | ESP32-S3-WROOM-1-N16R8 (16MB Quad Flash + 8MB Octal PSRAM, **IO35/36/37 被 PSRAM 占用不可用**) |
| 板上固件 | 小智 AI (xiaozhi) v2.5.3 定制版, 基于 ESP-IDF v6.0.2 编译 |
| 原厂固件 | xiaozhi v2.4.2 (IDF v6.0.2), 完整备份在 `~/esp/backups/xiaozhi_esp32s3_2026-10-24/` |
| 已完成改造 | ① 触屏手势调亮度/音量 ② 电池电量显示 (英集芯 IP5xx 驱动) |
| 串口 | `/dev/cu.usbmodem114301` (ESP32-S3 原生 USB-Serial/JTAG, 复位/烧录/日志三合一) |
| WiFi | NVS 中存有 SSID "lovely" 的配置, 烧录应用分区不会丢失 |

---

## 2. 硬件档案

### 2.1 板载资源 (来自官方 Wiki + 实测)

- **屏幕**: ST7789 2.0" 320×240 (横屏), SPI @ 80MHz, 背光 GPIO42 (PWM, 反相)
- **触摸**: FT5x06, I2C 地址 0x38, 小智原厂板级代码已接入 LVGL (`lvgl_port_add_touch`)
- **摄像头**: GC2145 (实测检测为 0x2145; 官方文档写 GC0308), I2C 0x3C, DVP 接口
- **音频**: ES8311 (DAC, 0x18) + ES7210 (ADC, 0x41), 功放 NS4150B, 由 PCA9557 (0x19) 控制使能
- **IMU**: QMI8658 六轴姿态传感器, **I2C 0x6A** (WHO_AM_I=0x05, 小智未使用, 留作后续体感功能)
- **USB**: CH334F Hub + CH340K 串口 (macOS 上 CH343 驱动未装也不影响, 用原生 USB 口即可)
- **扩展口 ×2** (GH1.25-5P): I2C 口 (3.3V/5V/GND/SDA/SCL, 与板载芯片共总线), 多功能口 (3.3V/5V/GND/GPIO10/GPIO11)

### 2.2 I2C 总线设备清单 (实测扫描, 总线端口 1, SDA=GPIO1, SCL=GPIO2)

| 地址 | 芯片 | 说明 |
|---|---|---|
| 0x18 | ES8311 | 音频 DAC |
| 0x19 | PCA9557 | IO 扩展 (bit0=LCD, bit1=功放, bit2=摄像头电源, bit3~7 空闲输入) |
| 0x38 | FT5x06 | 触摸屏 |
| 0x3C | GC2145 | 摄像头 |
| 0x41 | ES7210 | 音频 ADC |
| **0x55** | **英集芯 IP5xx** | **电池模块的电源管理 SoC (见 2.3)** |
| 0x6A | QMI8658 | 姿态传感器 (易被误认为充电芯片!) |

### 2.3 电池模块 (GoCool 套装内置)

实战派**板卡本体没有电池电路** (官方套件也不含电池)。GoCool 套装内置了一块基于**英集芯 IP5xx 系列**电源 SoC 的充放电模块, 电池经它给板子供电, 芯片挂在共享 I2C 总线的 **0x55** 上 (经 I2C 扩展口接入)。

实测寄存器 (与 milad-nikpendar/IP5108 开源库的 ADC 寄存器兼容, 但注意 IP5108 本体地址是 0x75, 本模块在 0x55):

| 寄存器 | 含义 | 解码 |
|---|---|---|
| 0xA2 (低) / 0xA3 (高) | 电池电压 ADC | `raw = lo + (hi & 0x3F) * 256`, `mV = 2600 + raw × 0.26855` (hi bit5=1 时为负值, 电池电压不会出现) |
| 0x71 bits[7:5] | 充电状态 | 0=未充电, 1=涓流, 2=恒流, 3=恒压, 4=CV停止, 5=充满, 6=超时 |
| 0xA4/A5 | 电流 ADC | 本模块上读值不稳定, 未采用 |

电量百分比: 芯片无库仑计 SOC 寄存器, 用**电压→SOC 分段线性曲线**估算 (曲线表见代码 `SocFromVoltage`)。注意**充电中端电压偏高, 显示会略高于实际** (如显示 100% 拔线后回落到 ~95%), 属正常精度。

### 2.4 ⚠️ 危险操作清单 (血泪教训)

1. **绝对不要把 GPIO10/GPIO11 拉低** (包括软件内部下拉、外部下拉、驱动输出低电平)。其中一个脚是电池模块的**电源保持线**, 拉低瞬间模块断电 → 板子**静默重启循环** (无任何 panic/复位原因输出, 表现为每 ~300ms 重启一次)。对这两个脚只允许高阻态读取。2026-10-24 曾因用内部下拉做悬空测试触发此事故。
2. **不要对 0x55 (IP5xx) 做写操作**, 除非已确认寄存器含义。电源 SoC 的配置寄存器写错可能关断输出 (同类芯片 IP5306 的 reg0x00 就是 boost 开关)。
3. **PSRAM 为 Octal, GPIO35/36/37 不可用**; GPIO10/11 被电池模块占用 (见第 1 条), 实际空闲 GPIO 为零。
4. 烧录只写应用相关分区, **不要擦除 NVS** (0x9000), 否则 WiFi 配置丢失需重新配网。

---

## 3. 开发环境

### 3.1 目录结构

```
~/esp/
├── esp-idf-v6.0.2/          # 当前使用的 ESP-IDF (与原厂固件编译版本一致)
├── esp-idf-v6.1/            # 小智官方推荐版 (备用, 未用于当前固件)
├── xiaozhi-esp32/           # 小智源码 (含本地修改, 见 §4)
├── backups/xiaozhi_esp32s3_2026-10-24/   # 原厂固件完整备份 (见 §7)
├── esp-claw/                # 乐鑫 ESP-Claw 仓库 (调研用)
├── export-idf-v6.0.2.sh     # 一键激活 v6.0.2
└── export-idf-v6.1.sh       # 一键激活 v6.1
```

### 3.2 常用命令

```bash
# 激活环境 (每个新终端执行; 不要放进管道子 shell)
source ~/esp/export-idf-v6.0.2.sh

# 编译 (板型参数是目录路径 lckfb/szpi-esp32s3, 不是 lichuang-dev)
cd ~/esp/xiaozhi-esp32
python scripts/build.py lckfb/szpi-esp32s3 --language zh-CN

# 烧录 (写 bootloader/分区表/ota_data/ota_0/assets, 不动 NVS)
idf.py -p /dev/cu.usbmodem114301 flash

# 交互式监控
idf.py -p /dev/cu.usbmodem114301 monitor
```

### 3.3 网络注意事项 (本机关键配置)

- **直连 GitHub Releases 完全不通** (工具链下载会卡死在 KB/s 级), 下载 ESP-IDF 工具链必须走乐鑫中国 CDN: 环境变量 `IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets` (激活脚本已内置, 实测 68MB/s)
- git 克隆走用户配置的 `gh-proxy.com` 代理, 偶尔会被**静默中断导致子模块工作区为空** (症状: CMake 报 `mbedtls include 不是目录`), 修复: `git submodule update --init --recursive --force`
- `api.github.com` 可直连 (查 issue/PR 用), 网页版 github.com 不通

---

## 4. 代码改造详解

改动共 3 个文件, 均在 `~/esp/xiaozhi-esp32` (用 `git diff` 查看完整差异):

### 4.1 文件清单

| 文件 | 改动 |
|---|---|
| `main/boards/lckfb/szpi-esp32s3/lichuang_dev_board.cc` | 核心改动: 触控手势 + 电池驱动 (见下) |
| `main/boards/lckfb/szpi-esp32s3/config.h` | 移除无效的 GPIO10 ADC 电池配置, 增加电池模块/危险引脚说明注释 |
| `main/CMakeLists.txt` | PRIV_REQUIRES 增加 `esp_adc` (历史遗留, 电池改 I2C 方案后已可移除) |

### 4.2 触控手势 (亮度/音量)

**设计**: 全屏透明捕获层挂在 `lv_layer_top()` (所有屏幕之上, 不受小智 UI 切换影响, 小智原生 UI 无触摸交互所以无冲突)。左半屏竖直拖动=亮度, 右半屏=音量; 16px 阈值防误触, 横向拖动 (>24px 且主导) 不响应; 约 150px 滑动覆盖 0~100 全量程; 拖动中屏幕顶部显示指示条 (图标+进度条+数值, 亮度琥珀色/音量蓝色), 松手 1.2s 后自动隐藏。

**关键实现点**:
- 事件回调在 LVGL 任务上下文执行, 直接调 `GetBacklight()->SetBrightness(v, false)` (拖动中不落盘) / `GetAudioCodec()->SetOutputVolume(v)`
- 亮度持久化: 松手时写 `Settings("display","brightness")` — 不能调 `SetBrightness(v, true)`, 因为其内部"值未变则提前返回"会跳过落盘
- 音量不持久化 — 与小智原版行为一致 (重启回默认 70)

### 4.3 电池驱动 (IP5xx @ 0x55)

**实现**: 自建 `BatteryDevice` 类 (继承 `I2cDevice`), 覆写 `Board::GetBatteryLevel(int&, bool&, bool&)` — 这是小智电量显示的唯一接入点, 返回 true 后**状态栏电池图标、低电量弹窗+音效、MCP `self.get_device_status` (语音问"还剩多少电")全部自动生效**, 无需改任何 UI 代码。

```cpp
// 电压 (0xA2/A3, Injoinic 公式)
uint16_t raw = lo + (uint16_t)(hi & 0x3F) * 256;
mv = (uint16_t)(2600.0f + raw * 0.26855f);
// 充电状态 (0x71 bits[7:5]: 1涓流/2恒流/3恒压=充电中, 5=充满)
// 电量 = SocFromVoltage(mv) 分段线性曲线; 读失败或电压不合理(2500~4500mV 之外)时返回 false 优雅降级
```

**关键实现点**:
- **必须自己包 `i2c_master_transmit_receive` 做带错误处理的读取** (`ReadRegSafe`), 不能用 `I2cDevice::ReadReg` — 它内部 `ESP_ERROR_CHECK`, I2C 瞬时失败会直接 panic 重启, 而 `GetBatteryLevel` 会被 LVGL 定时器周期调用
- `GetBatteryLevel` 会被 LVGL 任务和 MCP 线程并发调用, i2c_master 总线锁保证事务串行, 无需额外互斥

### 4.4 崩溃修复: LVGL 标签字体悬挂 (重要!)

**现象**: 触摸调节时 `Guru Meditation Error: Core 1 panic'ed (InstrFetchProhibited)`, PC=0x00000000, 栈顶在 `lv_font_get_glyph_width`, 寄存器 A4 = 正在测量的字符码点。

**根因**: 小智的服务器会在运行时**推送新文本字体并替换主题字体** (`assets.cc:301` → `LvglDisplay::SetTextFont`), 替换时旧 `LvglBuiltInFont` 对象被 `shared_ptr` 释放。官方 UI 会跟着 `SetTheme` 重绑, 但**自建 LVGL 标签的样式里存的是裸 `lv_font_t*`, 没人帮你重绑 → 悬挂指针 → 字体回调为 NULL → 跳 0 地址**。诊断方法: 初始化时字体回调有效, 30 秒后同一地址回调变 0, 即可实锤。

**修复** (已应用于指示条代码, **任何自建 LVGL 标签都要照做**): 每次显示时从主题重新取当前字体并校验:

```cpp
auto theme = static_cast<LvglTheme*>(display_->GetTheme());
if (theme->text_font() && theme->text_font()->font() &&
    theme->text_font()->font()->get_glyph_dsc != nullptr) {
    lv_obj_set_style_text_font(label, theme->text_font()->font(), 0);
}
```

---

## 5. 踩坑实录 (按时间序)

1. **字体悬挂崩溃** (见 §4.4) — 最隐蔽的一个, 定位靠"30 秒自动弹指示条"无人值守复现 + 字体指针/回调诊断日志。
2. **GPIO10/11 断电重启循环** — 悬空测试用内部下拉触发电池模块断电, 板子每 300ms 静默重启。教训: 未知网络上的 GPIO 只做高阻读。
3. **裸版 v2.5.3 未先验证** — 加功能前应先烧纯净版确认基线 (实测纯净版无问题, 崩溃确系新代码引入, 但本可更快定位)。
4. **bash 管道掩盖退出码** — `cmd | tail` 返回 tail 的退出码, 后台构建任务"假成功"三次。构建命令不要接管道, 或用 `${PIPESTATUS[0]}`。
5. **浅克隆子模块静默损坏** — 见 §3.3。
6. `source export.sh` 放进管道子 shell 会丢失 PATH (idf.py: command not found)。
7. 小智 OTA 分区布局特殊: **ota_0 @ 0x20000** (不是常见的 0x10000), 手动烧写偏移别照抄通用教程。

---

## 6. 调试方法论 (本机实操脚本)

### 6.1 串口日志抓取 (复位 + 定时采集)

```python
import serial, time
ser = serial.Serial('/dev/cu.usbmodem114301', 115200, timeout=0.2)
ser.rts = True; ser.dtr = False; time.sleep(0.15); ser.rts = False  # 复位脉冲
ser.reset_input_buffer()
data = b''
end = time.time() + 40
while time.time() < end:
    chunk = ser.read(8192)
    if chunk: data += chunk
ser.close()
print(data.decode('utf-8', 'replace'))
```

### 6.2 崩溃栈解码

```bash
cd ~/esp/xiaozhi-esp32 && source ~/esp/export-idf-v6.0.2.sh
xtensa-esp32s3-elf-addr2line -pf -C -e build/xiaozhi.elf 0x420dc481 0x420e165b ...
# 把崩溃日志 Backtrace 里的地址依次传入; PC=0x00000000 通常是空调用函数指针(悬挂回调)
```

### 6.3 未知芯片识别流程 (已验证有效)

I2C 总线扫描 → 排除已知设备 → 对未知地址做**只读**寄存器 dump (0x00-0xFF 全区) → 多轮时序读取区分静态配置/实时数据 → 用候选芯片的解码公式验证 (如电压解出 3000-4300mV 即实锤)。**全程禁止写操作。**

---

## 7. 备份与恢复

原厂固件 (xiaozhi v2.4.2) 完整备份: `~/esp/backups/xiaozhi_esp32s3_2026-10-24/`

- `flash_full_16MB.bin` 全片镜像 (esptool read-flash + verify-flash 双重校验)
- 各分区单独文件 + `md5sums.txt` + `boot_log.txt` + `README.txt` (含分区偏移表)

一键恢复原厂状态:

```bash
esptool --port /dev/cu.usbmodem114301 --baud 921600 \
    write-flash 0x0 ~/esp/backups/xiaozhi_esp32s3_2026-10-24/flash_full_16MB.bin
```

---

## 8. 当前状态与后续方向

**当前固件状态** (2026-10-25 验证): v2.5.3 + 触控手势 + 电池显示 + **应用桌面/贪吃蛇 (一期)**, 稳定运行。

### 8.1 应用桌面 + 贪吃蛇 (2026-10-25, "类安卓"一期)

文件 (均在板级目录, CMake 自动 glob 编译):
- `game_framework.h` — GameApp 游戏基类 (name/Enter/Exit/running, 约定持显示锁调用)
- `game_ui_common.h` — **必用工具**: SafeThemeFont (防字体悬挂, §4.4 模式)、FontHasGlyphs/PickText (内置字体只含小智 UI 汉字子集, "智/贪/吃/蛇"等字要等服务器推送全量字体, 缺字时自动回退英文)、Rgb565
- `game_launcher.cc/h` — DesktopLauncher: 独立 LVGL screen, AppEntry 注册表 (标题/图标符号/颜色/回调), 状态栏 (设备状态+电池, 1s 定时器), 底部提示
- `snake_game.cc/h` — 贪吃蛇: 16x10 格 20px/格, PSRAM 手写 RGB565 画布缓冲 (lv_canvas), lv_timer 游戏循环, 滑动转向 (PRESSING 重锚定实现不抬手连续变向, 每步只接受一次转向+禁 180°), 最高分持久化 Settings("snake","best")
- `lichuang_dev_board.cc` — UiMode 三态调度 (kChat/kDesktop/kGame): 首次切屏前捕获 `chat_screen_=lv_screen_active()`; **手势捕获层只在 kChat 显示** (桌面/游戏要触摸穿透); BOOT 键 (构造参数 long_press_time=800): 启动中=配网(原版)/游戏中=回桌面/桌面=回小智/聊天短按=原版行为/**聊天长按 800ms=回桌面 (按住说话模式下禁用防误触)**; 聊天界面右上角 44x44 半透明"主页"悬浮按钮 (layer_top 手势层之上, 仅 kChat 显示, 图标字体显示前重取); MCP 工具 `self.launcher.show`/`self.snake.start`/`self.snake.stop` (语音"打开贪吃蛇"/"回到桌面"); 500ms lv_timer 轮询 kDeviceStateIdle 自动进桌面 (只一次)

已知项: ① Snake::Exit 未启动时也打日志 (无害噪音); ② 低电量弹窗挂在聊天屏上, 桌面/游戏中不可见; ③ 内置字体的中文缺失字 (服务器推字体后自动变中文)

后续可做的方向 (硬件均已就绪):
- **二期**: 嵌入 Lua 游戏运行时 (参考 esp-claw 的 lua_modules/lvgl/lcd/touch, LVGL 同为 9.5.x; IDF 版本差 5.5.4→6.0.2 需验证) + 局域网网页上传脚本 → 不烧录装新游戏
- **三期**: 微信通道 (esp-claw cap_im_wechat 直连 ilinkai.weixin.qq.com 扫码, 但依赖 claw 事件框架难单独搬; 或自建服务端中转); 或 ota_1 刷 ESP-Claw 双系统 (esp_ota_set_boot_partition)
- 摄像头: 加触屏快门 (拍照 MCP 工具已有, 缺 UI 入口)
- IMU (QMI8658 @ 0x6A): 翻转屏幕方向、体感交互 (官方例程 `maker-community/lcsc-shizhanpi-esp32s3-examples` 的 02-attitude 有驱动参考)
- 唤醒词更换: `build.py --wake-word` 参数 (jarvis 等模型可选)
- PCA9557 bit3~7 空闲输入脚: 可接充电指示灯等外部信号
- 自建小智服务端 (xiaozhi-esp32-server) 或接入 OpenClaw 桥接, 摆脱官方云
- ESP-Claw 移植 (源码已克隆到 `~/esp/esp-claw`, 无实战派板级支持, 需按 `application/edge_agent/boards/` 的 YAML 格式写板级配置, 音频可参考 esp_box_3)

### 8.4 二期: Lua 脚本游戏引擎 (2026-10-25, 已烧板验证)

**交付**: 脚本游戏 (Lua 5.5) + 4MB FATFS 存储 + 局域网网页安装 (板上 :8080) → **不烧录装新游戏**。桌面每次显示扫描 /apps 动态出图标 (3 apps: 小智/贪吃蛇/接星星)。镜像 2.84MB (剩 31%)。

- **文件**: script_storage (FATFS 挂载 /apps), script_game (沙箱宿主+API 绑定), app_upload_server (HTTP :8080), demo_lua_game.h (接星星), 16m_games.csv (分区表: assets 8M→4M + storage 4M @0xC00000)
- **沙箱**: 只开 base/string/table/math; Lua 内存 PSRAM 限额 512KB (自定义分配器); 指令钩子 5M 防死循环; 全 pcall 隔离
- **脚本 API**: `app={title,color}` + `canvas.fill/rect/text` (0xRRGGBB) + `game.set_tick_period/exit/random/millis` + 回调 `on_start/on_tick/on_touch(evt,x,y)`; 文字走 lv_canvas layer + lv_draw_label(3参数)
- **踩坑** (都 costly): ① espressif/lua 头文件**无 extern "C"**, C++ 里必须手动包; ② Lua 5.5 的 `lua_newstate` 多了第三参 `seed`; 组件按 `-DLUA_32BITS` 编译 (lua_Integer=int32); ③ **httpd 不能在板级构造期启动** —— lwip 未初始化, tcpip mbox 断言 → 启动循环 (无 panic 输出, 表现为静默循环); 推迟到 kDeviceStateIdle 再启; ④ 分区表改动要同步改 sdkconfig 缓存 (defaults 不覆盖已有值, 必要时删 sdkconfig 全量重配)
- **网络故障根因复盘 (2026-10-25, 已修复)**: 局域网 ping 板子 100% 丢包、Mac 连不上 :8080, 一度误判为路由器 AP 隔离 (华为 AX3 主网络其实没有隔离开关, 且华为路由器**没有网页后台**, 只能用智慧生活 App 管理, 192.168.3.1 打不开是正常的)。真因: **Application 在 idle 时把 WiFi 切到 LOW_POWER = WIFI_PS_MAX_MODEM** (wifi_station.cc SetPowerSaveLevel), 华为路由器对深睡设备的来向单播直接丢弃; 对话时临时升档所以语音正常。修复: 板级覆写 SetPowerSaveLevel 把 LOW_POWER 钳制到 BALANCED (MIN_MODEM), 实测 ping 40-200ms、安装页全流程 (上传/列表/删除) 通过。另: esp_http_server 通配路径要配 `config.uri_match_fn = httpd_uri_match_wildcard`

### 8.5 烧录连接故障排查 (2026-10-25 实录)

esptool 反复 "No serial data received" (default-reset/usb-reset 均无效, 复位脉冲也收不到启动日志, 但 macOS 侧端口可打开): 原生 USB-Serial/JTAG 数据通路卡死 (疑似电脑休眠唤醒后)。**解决: 拔插 USB 线即可恢复**。另外注意: 启动阶段 (kDeviceStateStarting) 按 BOOT 会进配网模式 (原版行为), 观察日志时别碰按键。

### 8.3 版本防护与代码托管 (2026-10-25)

- **OTA 防护**: 官方服务器启动时会比对版本并**自动 OTA** (`application.cc CheckNewVersion` → `UpgradeFirmware`, 还有 force=1 强推通道)。已把根 CMakeLists.txt 的 `PROJECT_VER` 抬到 **99.0.0**, 日志验证 "Current is the latest version"。每次 merge 上游后该行必冲突 (上游会改版本号), 解决时保留 99.0.0。
- **代码托管**: 定制已提交到 `github.com/cailven/lichuang-cailven` (main 分支, 完整上游历史 + 定制 commit)。本地仓库 `origin` = 上游 (fetch 用, 走 gh-proxy 只读代理), `cailven` = 用户仓库 (push 用, **走 SSH**: 专用密钥 `~/.ssh/id_ed25519_github_cailven`, ssh config 已为 github.com 指定该密钥 + IdentitiesOnly; 用户账号级添加, `ssh -T` 显示 Hi cailven!。HTTPS 备用通道: ghfast.top 透传认证 — gh-proxy.com 会剥掉写路径认证 — 仓库留有调 `gh auth token` 的 credential helper, gh 登录账号 zhangrunqiu 对 cailven 仓库有 ADMIN 权限)。
- **同步上游流程**: `git fetch origin` → 看 log → `git merge origin/main` → 解决冲突 (预计仅 lichuang_dev_board.cc / config.h / CMakeLists.txt ×2) → 重编译烧录回归。2026-10-25 曾因**浅克隆**推送失败 (index-pack), 已 `git fetch --unshallow` 补全历史。

### 8.7 全功能现状与踩坑总表 (2026-10-26, 供后续 AI 接手)

**当前固件全貌**: 小智 v2.5.3 + 类安卓游戏桌面。主程序只做三件事: 桌面 + 小智 + Lua 脚本引擎, 所有游戏 (含贪吃蛇) 都是 /apps 分区的 .lua。四界面模式: kChat/kDesktop/kGame/kSettings。功能: 横滚图标桌面 / Lua 沙箱 (canvas/game API + imu/camera/sound/i2c/gpio 硬件 API) / 局域网安装器 :8080 (上传/在线编辑/删除/装前校验) / 设置面板 (亮度音量滑条+重新配网+重启, 危险操作双击确认) / 界面静默 (进桌面游戏自动停语音关唤醒词) / 时钟等出厂脚本 / OTA 防护 99.0.0。

**踩坑总表 (22 条, 按症状查)**:

| # | 症状 | 根因 | 修复 |
|---|---|---|---|
| 1 | esptool 连不上 "No serial data" | 原生 USB-JTAG 通路卡死 (电脑休眠后) | 拔插 USB 线 |
| 2 | 静默重启循环, 无任何 panic 输出 | GPIO10/11 被拉低 (电池电源保持线断电) | 永远只高阻读, 禁任何输出 |
| 3 | 启动中误进配网模式 | kDeviceStateStarting 时按 BOOT 是原生配网入口 | 观察日志时别碰键 |
| 4 | I2C 扫描大量幻影 ACK / IMU 数据 ±满量程 | 400kHz 受摄像头 DVP 干扰信号完整性不足 | 全部 I2C 100kHz; 扫描弃 i2c_master_probe 改自建读探测 |
| 5 | QMI8658 数据全是垃圾 | 复位后连写配置全部丢失 (官方例程的 delay 不能省) | 复位→等15ms→逐个写 CTRL (间隔2ms) |
| 6 | IMU 闲置一段时间后永远 nil | STATUS0 就绪位读后清零, 短重试踩不中 4ms 节拍 | 盲读 12 字节数据寄存器 (250Hz 永远有数据) |
| 7 | sound.beep 引发整机 panic | PCA9557 的 I2cDevice::ReadReg 是 ESP_ERROR_CHECK, I2C 瞬断即炸 | 板内 Pca9557 类安全化 (重试3次+缓存回退) |
| 8 | 上传 ~4.7KB 脚本时整机重启 | httpd 任务栈 4K 跑不下 Lua 编译器 (栈溢出) | httpd 与 LVGL 任务栈都加到 16K; 桌面扫描加元信息缓存 (名字+大小) |
| 9 | 摄像头画面"滤镜感" | fb 是大端 RGB565, LVGL 画布要小端 uint16, memcpy 字节颠倒 | 逐像素字节交换; camera.snapshot(hflip) 镜像参数; camera.rgb_swap 纠通道序 |
| 10 | 设置里点"重新配网"闪退 | 双 httpd 实例 ctrl_port 默认都是 32768, 配网服务 httpd_start 失败被它的 ESP_ERROR_CHECK 引爆 | 安装器 ctrl_port 改 32769 (wifi_configuration_ap.cc:232) |
| 11 | 开机偶发崩溃, 桌面 2.5s 就出现 | 触摸屏上电幽灵触摸击中"主页"按钮, 初始化中途切屏竞态 | kDeviceStateStarting 期间锁定界面切换 |
| 12 | 局域网 ping 板子 100% 丢包 (能上网/语音正常) | 小智 idle 切 WIFI_PS_MAX_MODEM 深睡, 华为路由器丢弃沉睡设备来向单播 | 板级覆写 SetPowerSaveLevel 把 LOW_POWER 钳制到 BALANCED |
| 13 | 增删板级 .cc 后链接报错/找不到文件 | CMake glob 增量编译不重扫 | idf.py reconfigure 再 build (build.py 自带) |
| 14 | Lua 函数链接错误 (C++ 修饰名) | espressif/lua 头文件无 extern "C" 保护 | #include 外包 extern "C" {} |
| 15 | lua_newstate 参数不对 / 大整数溢出 | espressif/lua 是 Lua 5.5: lua_newstate 多第三参 seed; 编译带 -DLUA_32BITS (lua_Integer=int32, 色值<2^24 安全) | 按 5.5 API 写; 大数避开 |
| 16 | httpd 在板级构造期启动 → 启动循环 | lwip 未初始化, tcpip mbox 断言 | 推迟到 kDeviceStateIdle 再启动 |
| 17 | 时钟开机显示 1970 | 小智联网校时前的窗口期系统时间无效 | 脚本判 年份<2024 显示"时间同步中", 校准后每秒 tick 自动恢复 |
| 18 | 设置按钮变纯色块 | 按钮标签没套主题字体, LVGL 默认字体无中文字形 | 所有自建文字统一 SafeThemeFont + PickText 中英回退 |
| 19 | 设置界面文字重叠 | 绝对坐标排布拥挤 | 竖向滚动 flex 容器布局 |
| 20 | push 报 index-pack failed | 仓库是浅克隆, 历史对象不全 | git fetch origin --unshallow |
| 21 | git push 认证失败 (gh-proxy) | gh-proxy 剥掉写路径认证 | 走 ghfast.top 或 SSH (密钥 id_ed25519_github_cailven) |
| 22 | 电脑浏览器打不开板子网页 (curl 正常) | Mac 的 Astrill 系统代理截走局域网流量 | 代理直连列表加 192.168.0.0/16 (Astrill 切换可能重置) |

**字体体系**: 内置中文字体只含小智 UI 子集; 服务器联网后推送全量字体并**释放旧字体对象** —— 自建 LVGL 标签必须在每次显示时重取主题字体 (SafeThemeFont), 否则悬挂崩溃; 中英文用 PickText 按字形覆盖自动回退。

**蓝牙**: xiaozhi 基线未编译 (CONFIG_BT_ENABLED off)。仓库自带 BluFi 蓝牙配网完整实现 (CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING, blufi.cpp), 但与热点网页配网互斥, 默认保持网页配网。

**危险操作双确认**: 重新配网/重启等破坏性按钮必须 3 秒内点两次 (首点变"再点一次确认")。
