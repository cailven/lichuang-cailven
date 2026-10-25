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
| WiFi | NVS 中存有 SSID 配置(已脱敏), 烧录应用分区不会丢失 |

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

**当前固件状态** (2026-10-24 验证): v2.5.3 + 触控手势 + 电池显示, 稳定运行, WiFi 自动重连, 电量 100% (4214mV) 显示正常, 充电状态识别正常。

后续可做的方向 (硬件均已就绪):
- 摄像头: 加触屏快门 (拍照 MCP 工具已有, 缺 UI 入口)
- IMU (QMI8658 @ 0x6A): 翻转屏幕方向、体感交互 (官方例程 `maker-community/lcsc-shizhanpi-esp32s3-examples` 的 02-attitude 有驱动参考)
- 唤醒词更换: `build.py --wake-word` 参数 (jarvis 等模型可选)
- PCA9557 bit3~7 空闲输入脚: 可接充电指示灯等外部信号
- 自建小智服务端 (xiaozhi-esp32-server) 或接入 OpenClaw 桥接, 摆脱官方云
- ESP-Claw 移植 (源码已克隆到 `~/esp/esp-claw`, 无实战派板级支持, 需按 `application/edge_agent/boards/` 的 YAML 格式写板级配置, 音频可参考 esp_box_3)
