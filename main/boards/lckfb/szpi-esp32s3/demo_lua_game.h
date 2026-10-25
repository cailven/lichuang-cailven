#pragma once

// 出厂演示脚本: "接星星" —— 首次挂载存储时若 /apps 为空则自动写入。
// 同时也是脚本游戏的 API 示例, 改这个文件即可体验"不烧录改游戏"。

static const char* kDemoLuaGame = R"LUA(
-- 接星星: 手指按住屏幕左右移动挡板, 接住下落的星星
app = { title = "接星星", color = 0x7E57C2 }

local W, H = 320, 200
local PW = 50
local px = (W - PW) / 2
local stars = {}
local score, miss, ticks = 0, 0, 0

function on_start()
    game.set_tick_period(50)
    canvas.fill(0x10141A)
    canvas.rect(px, H - 12, PW, 8, 0x7E57C2)
    canvas.text(8, 4, "接 0  失 0", 0xE0E0E0)
end

function on_touch(evt, x, y)
    if evt == "down" or evt == "move" then
        if x < 0 then x = 0 end
        if x > W - PW then x = W - PW end
        px = x
    end
end

function on_tick()
    ticks = ticks + 1
    local speed = 2 + (score // 8)
    if speed > 8 then speed = 8 end
    if ticks % 14 == 1 then
        stars[#stars + 1] = { x = game.random(W - 6), y = -6, v = speed }
    end

    canvas.fill(0x10141A)
    local i = 1
    while i <= #stars do
        local s = stars[i]
        s.y = s.y + s.v
        canvas.rect(s.x, s.y, 6, 6, 0xFFD54F)
        if s.y + 6 >= H - 12 and s.y <= H - 4 and s.x + 6 >= px and s.x <= px + PW then
            score = score + 1
            table.remove(stars, i)
        elseif s.y > H then
            miss = miss + 1
            table.remove(stars, i)
        else
            i = i + 1
        end
    end
    canvas.rect(px, H - 12, PW, 8, 0x7E57C2)
    canvas.text(8, 4, "接 " .. score .. "  失 " .. miss, 0xE0E0E0)
    canvas.text(W - 130, 4, "按住拖动挡板", 0x808A94)
end
)LUA";
