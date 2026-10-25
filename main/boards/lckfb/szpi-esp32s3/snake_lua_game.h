#pragma once

// 出厂预装: 贪吃蛇 Lua 版 (移植自一期原生实现)
// 顶部 20px 信息条 + 16x9 格局, 滑动转向(不抬手连续变向), 加速, 最高分持久化

static const char* kSnakeLuaGame = R"LUA(
app = { title = "贪吃蛇", color = 0x43A047 }

local COLS, ROWS, CELL = 16, 9, 20
local TOP = 20   -- 顶部信息条高度
local W = 320

local body = {}       -- { {x,y},... } [1] 为蛇头
local dirx, diry = 1, 0
local pdx, pdy = 0, 0
local hasp = false    -- 每步只接受一次转向
local food = { x = 0, y = 0 }
local score, best, over = 0, 0, false
local ax, ay, moved = 0, 0, false   -- 触摸锚点与位移标记

local function period()
    local p = 220 - score * 7
    if p < 90 then p = 90 end
    return p
end

local function spawnFood()
    while true do
        local c = { x = game.random(COLS), y = game.random(ROWS) }
        local hit = false
        for i = 1, #body do
            if body[i].x == c.x and body[i].y == c.y then
                hit = true
                break
            end
        end
        if not hit then
            food = c
            return
        end
    end
end

local function reset()
    body = {}
    for i = 0, 2 do
        body[#body + 1] = { x = 8 - i, y = 4 }
    end
    dirx, diry = 1, 0
    hasp = false
    score = 0
    over = false
    spawnFood()
    game.set_tick_period(period())
end

local function setDir(dx, dy)
    if hasp then return end
    if dx == -dirx and dy == -diry then return end  -- 禁 180° 掉头
    pdx, pdy = dx, dy
    hasp = true
end

local function draw()
    canvas.fill(0x10141A)
    for c = 0, COLS do
        canvas.rect(c * CELL, TOP, 1, ROWS * CELL, 0x1B2228)
    end
    for r = 0, ROWS do
        canvas.rect(0, TOP + r * CELL, W, 1, 0x1B2228)
    end
    canvas.rect(food.x * CELL + 4, TOP + food.y * CELL + 4, CELL - 8, CELL - 8, 0xF25C5C)
    for i = #body, 1, -1 do
        local lv = 0x30 + math.floor(0x60 * (#body - i) / math.max(1, #body - 1))
        local color = (lv + 0x20) * 65536 + (lv + 0x50) * 256 + lv
        local inset = 2
        if i == 1 then inset = 1 end
        canvas.rect(body[i].x * CELL + inset, TOP + body[i].y * CELL + inset,
                    CELL - inset * 2, CELL - inset * 2, color)
    end
    -- 蛇头眼睛 (随方向)
    local hx = body[1].x * CELL
    local hy = TOP + body[1].y * CELL
    local e1x, e1y, e2x, e2y
    if dirx ~= 0 then
        e1x = hx + CELL // 2 + dirx * 4
        e2x = e1x
        e1y = hy + 6
        e2y = hy + CELL - 8
    else
        e1y = hy + CELL // 2 + diry * 4
        e2y = e1y
        e1x = hx + 6
        e2x = hx + CELL - 8
    end
    canvas.rect(e1x, e1y, 3, 3, 0xFFFFFF)
    canvas.rect(e2x, e2y, 3, 3, 0xFFFFFF)
    canvas.text(4, 0, "Score " .. score, 0xE0E0E0)
    canvas.text(120, 0, "Best " .. best, 0x808A94)
    if over then
        canvas.text(W // 2 - 54, 68, "Game Over", 0xFF7043)
        canvas.text(W // 2 - 44, 98, "点击重开", 0xFFFFFF)
    end
end

function on_start()
    best = game.load("snake_best", 0)
    reset()
    draw()
end

function on_tick()
    if not over then
        if hasp then
            dirx, diry = pdx, pdy
            hasp = false
        end
        local hx = body[1].x + dirx
        local hy = body[1].y + diry
        if hx < 0 or hx >= COLS or hy < 0 or hy >= ROWS then
            over = true
        else
            local hit = false
            for i = 1, #body do
                if body[i].x == hx and body[i].y == hy then
                    hit = true
                    break
                end
            end
            if hit then
                over = true
            else
                local ate = (hx == food.x and hy == food.y)
                if ate then
                    score = score + 1
                    if score > best then
                        best = score
                        game.save("snake_best", best)
                    end
                    spawnFood()
                    game.set_tick_period(period())
                else
                    table.remove(body)  -- 尾巴前移
                end
                table.insert(body, 1, { x = hx, y = hy })
            end
        end
    end
    draw()
end

function on_touch(evt, x, y)
    if evt == "down" then
        ax, ay = x, y
        moved = false
    elseif evt == "move" then
        local dx = x - ax
        local dy = y - ay
        if math.abs(dx) >= 18 or math.abs(dy) >= 18 then
            if math.abs(dx) > math.abs(dy) then
                if dx > 0 then setDir(1, 0) else setDir(-1, 0) end
            else
                if dy > 0 then setDir(0, 1) else setDir(0, -1) end
            end
            ax, ay = x, y
            moved = true
        end
    elseif evt == "up" then
        if over and not moved then
            reset()
            draw()
        end
    end
end
)LUA";
