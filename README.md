# lichuang-cailven — 实战派小智 · 类安卓游戏桌面定制固件

基于 [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.5.3，为**立创·实战派 ESP32-S3** 开发的定制固件：把小智 AI 语音助手和游戏桌面装进同一块板子。**所有游戏都用 Lua 脚本编写，浏览器里安装/编辑，全程不需要重新烧录固件。**

## 功能总览

| 功能 | 说明 |
|---|---|
| 🤖 小智 AI（完整保留） | 语音对话/唤醒词/MCP 工具一样不少；任何界面下都能语音交互 |
| 🖥️ 应用桌面 | 图标横排滚动（多了左右滑），联网就绪后自动进入；小智是桌面上的一个应用 |
| 🎮 Lua 脚本游戏引擎 | Lua 5.5 沙箱（内存限额/防死循环/错误隔离），游戏装在独立 4MB 分区 |
| 🌐 局域网安装器 | 手机/电脑浏览器打开 `http://<板子IP>:8080`：上传 / **在线编辑** / 删除游戏，保存即校验 |
| 📦 出厂预装 | 贪吃蛇、接星星（纯 Lua，可在线改）；示例时钟（指针+数字+日期） |
| ⚙️ 设置面板 | 桌面齿轮进入：亮度/音量滑条（竖向滚动布局）、重新配网、重启（危险操作双击确认）、IP/内存信息 |
| 🖐 触控手势 | 聊天界面：左半屏竖滑调亮度、右半屏调音量 |
| 🔋 电池显示 | 英集芯 IP5xx 驱动：状态栏图标 / 低电量弹窗 / 语音“还剩多少电” |
| 🛡️ OTA 防护 | 版本号固定 99.0.0，防止官方服务器自动升级刷掉定制固件 |

## 架构设计

```
┌──────────────────────────────────────────────────┐
│              应用桌面 Launcher（横滚图标）          │
│   ┌──────┬──────┬──────┬──────┬──────┐           │
│   │ 小智 │ 贪吃蛇│ 接星星│ 时钟 │ ...  │  ← 游戏全是 │
│   └──────┴──────┴──────┴──────┴──────┘    /apps  │
│        状态栏: AI 状态 + 电量                     │
└────────┬─────────────────────┬───────────────────┘
         ▼                     ▼
  小智原生聊天 UI         ScriptGameApp（脚本宿主）
  （未改动，含手势层）      Lua 5.5 沙箱 × N 个脚本
                            canvas.* / game.* / on_* 回调
┌────────┴─────────────────────┴───────────────────┐
│            xiaozhi 固件核心（未改动）               │
│     音频 / 唤醒词 / WebSocket 协议 / 激活 / OTA     │
├──────────────────────────────────────────────────┤
│ 板级定制: UiMode 调度 / BOOT 键 / 主页悬浮按钮       │
│ HTTP 安装器 :8080 │ FATFS /apps (4MB, 磨损均衡)    │
└──────────────────────────────────────────────────┘
```

**核心原则：主程序（原生 C++）只做三件事——桌面、小智、脚本引擎。** 游戏一律是 `/apps` 分区里的 `.lua` 文件，包括出厂预装的贪吃蛇（从原生代码移植而来，玩法完全一致）。新增游戏 = 浏览器拖一个文件，删掉即消失。

### 脚本沙箱

- 只开放 `base / string / table / math` 标准库，无 `os / io / package`，无文件访问
- Lua 堆全走 PSRAM 且限额 512KB（自定义分配器），不吃系统内存
- 指令计数钩子防死循环（单次回调 500 万指令上限）
- 所有脚本调用走 `pcall`：脚本崩溃只显示错误浮层，固件毫发无伤
- 唯一可写数据：`game.save/load` 键值（存 `/apps/.scores`，如最高分）

## 脚本游戏 API

```lua
app = { title = "我的游戏", color = 0xRRGGBB }   -- 桌面图标标题与底色

canvas.fill(color)              -- 全屏填充 (0xRRGGBB)
canvas.rect(x, y, w, h, color)  -- 矩形
canvas.text(x, y, "文字", color)-- 文字 (主题字体, 支持中文)

game.set_tick_period(ms)        -- 帧周期 (0=暂停)
game.exit()                     -- 退回桌面
game.random(n)                  -- 0..n-1 随机数
game.millis()                   -- 开机毫秒数
game.time()                     -- 返回 h,m,s,年,月,日 (联网校时, 东八区)
game.save(key, n) / game.load(key, 默认值)  -- 最高分等持久化

function on_start() end                       -- 进入游戏
function on_tick() end                        -- 每帧 (画完自动刷新)
function on_touch(evt, x, y) end              -- evt: "down"/"move"/"up"
```

### 硬件访问 API（均已板上实测）

```lua
imu.read()        -- 返回 ax,ay,az (g), gx,gy,gz (°/s); QMI8658 六轴
camera.snapshot() -- GC2145 拍一帧, 居中裁剪画进 320x200 画布; 返回 true/false
sound.beep(频率Hz, 时长ms, 音量0-100)  -- 正弦提示音 (小智说话时调用会混音)

i2c.read(地址, 寄存器, 长度1-8)   -- 返回逐字节; 总线 100kHz
i2c.write(地址, 寄存器, ...)      -- 板载芯片地址写保护 (0x18/19/38/3C/41/55)
i2c.scan()                        -- 返回在线地址表 (Lua 数组)
gpio.read(10 或 11)               -- 多功能口, 仅高阻输入 (电源保持线, 禁止输出)
```

硬件红线（代码强制）：GPIO10/11 无任何写路径；I2C 板载芯片禁写；扩展 I2C 口与板载总线共享，外接传感器即插即用。

画布 320×200，坐标原点左上。完整示例见板级目录 `snake_lua_game.h`（贪吃蛇）与 `demo_lua_game.h`（接星星），或直接在安装器里点“编辑”看源码。

## 交互速查

| 操作 | 聊天界面 | 桌面 | 游戏中 |
|---|---|---|---|
| BOOT 短按 | 说话/切对话（原生） | 回小智 | 退回桌面 |
| BOOT 长按 0.8s | 回桌面 | — | — |
| 屏幕触摸 | 手势调亮度/音量 | 点图标（横滑翻页） | 游戏自定 |
| 右上角 🏠 按钮 | 回桌面 | 隐藏 | 隐藏 |

语音（MCP 工具）：**“打开贪吃蛇”** `self.snake.start` · **“回到桌面”** `self.launcher.show` · **“怎么装游戏”** `self.apps.install_info`（返回安装页地址）

**界面静默**：进入桌面或游戏时，小智自动停止说话、停止监听并关闭唤醒词检测（麦克风完全安静）；点小智图标或按 BOOT 回聊天界面后自动恢复唤醒词。

**关于蓝牙**：本固件未编译蓝牙（xiaozhi 基线默认未开启 CONFIG_BT）。仓库已内置 BluFi 蓝牙配网的完整实现（`CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING`，配乐鑫 EspBlufi App 使用），但它与热点网页配网互斥——如需蓝牙配网版固件，在板级 config.json 的 sdkconfig_append 中加入该配置重新编译即可。

## 构建与烧录

```bash
# ESP-IDF v6.0.2
cd xiaozhi-esp32
python scripts/build.py lckfb/szpi-esp32s3 --language zh-CN

# 烧录（分区表含新增 storage 分区，首次需全量 @flash_args；NVS 不动，WiFi 配置保留）
idf.py -p <串口> flash
```

分区表（`partitions/v2/16m_games.csv`，仅此板型生效）：

| 分区 | 偏移 | 大小 | 用途 |
|---|---|---|---|
| ota_0 | 0x20000 | 3.94M | 应用（当前镜像 ~2.85MB，余 31%） |
| assets | 0x800000 | 4M | 字体资源 |
| **storage** | 0xC00000 | **4M** | **FATFS，游戏脚本 `/apps/*.lua`** |

## 目录结构（定制部分）

```
main/boards/lckfb/szpi-esp32s3/
├── lichuang_dev_board.cc   # 板级总装: UiMode 调度/BOOT/手势/电池/MCP/种子预装
├── game_launcher.cc/h      # 桌面: 图标横滚/状态栏/动态脚本扫描
├── game_framework.h        # GameApp 接口 (脚本宿主实现)
├── game_ui_common.h        # 字体安全取用(防服务端换字体崩溃)/中英自动回退/RGB565
├── script_game.cc/h        # Lua 沙箱宿主 + 全部 API 绑定
├── script_storage.cc/h     # /apps 分区挂载与读写
├── app_upload_server.cc/h  # 安装器 HTTP 服务 (:8080, 上传/编辑/删除/校验)
├── snake_lua_game.h        # 出厂预装: 贪吃蛇 (Lua)
└── demo_lua_game.h         # 出厂预装: 接星星 (Lua)
```

## 同步上游更新

```bash
git fetch origin                # origin = 78/xiaozhi-esp32
git log main..origin/main --oneline
git merge origin/main           # 冲突预计仅板级文件与两处 CMakeLists
# 注意: 根 CMakeLists.txt 的 PROJECT_VER 保留 99.0.0 (OTA 防护)
python scripts/build.py lckfb/szpi-esp32s3 --language zh-CN && idf.py -p <串口> flash
```

## 注意事项（实测踩坑摘录，全文见 [LICHUANG-DEV-CUSTOMIZATIONS.md](LICHUANG-DEV-CUSTOMIZATIONS.md)）

- **GPIO10/11 绝不能拉低**（电池模块电源保持线），GPIO35/36/37 被 PSRAM 占用
- 烧录不要擦 NVS，否则 WiFi 配置丢失需重新配网
- 小智 idle 时会把 WiFi 切到深度省电，部分路由器（如华为）会丢弃发往设备的来向数据导致局域网连不上——本固件已钳制到均衡省电
- 电脑浏览器打不开安装页时先查系统代理（代理软件需把 192.168.0.0/16 加入直连）
- 内置中文字体只含小智 UI 子集，全量字体由服务器联网推送（缺字时界面自动回退英文）
- 启动阶段（未连上服务器）界面切换被锁定——防触摸屏上电幽灵触摸触发竞态

## 致谢

- 上游固件：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（虾哥及社区）
- Lua 运行时：[espressif/lua](https://components.espressif.com/components/espressif/lua) 组件
- 开发过程、踩坑实录与硬件档案：[LICHUANG-DEV-CUSTOMIZATIONS.md](LICHUANG-DEV-CUSTOMIZATIONS.md)
