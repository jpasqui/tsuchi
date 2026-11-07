-- Two machines in one session, for a game run with two emulators on one computer or on two computers.
-- role.txt says "host" or "guest" on its first line. On the guest a second line may give the host's address,
-- 127.0.0.1 without it. Each side sends a numbered reliable message every 30 world steps and logs what arrives,
-- in order or not, and every session event, so a log shows whether the session lived through the other side's
-- loads.
local text = file.read("role.txt", 0, 128) or ""
local role = text:match("^%s*(%a+)")
local address = text:match("^%s*%a+%s+([%w%.%-:]+)") or "127.0.0.1"
local port = 47710
local sent, received, out_of_order, expected = 0, 0, 0, 1
local steps = 0
local function note(text) mod.log("session proof " .. text) end

local opened, why = session.open_channel(1)
note("channel " .. tostring(opened or why))
session.on_joined(function(event) note("joined self=" .. event.peer) end)
session.on_peer_joined(function(event) note("peer joined " .. event.peer) end)
session.on_peer_left(function(event) note("peer left " .. event.peer .. " reason=" .. tostring(event.reason)) end)
session.on_ended(function(event) note("ended reason=" .. tostring(event.reason)) end)
session.on_message(function(event)
    local body = session.read_message(event.size)
    local number = tonumber(body and body:match("^n(%d+)$"))
    if not number then return end
    received = received + 1
    if number ~= expected then out_of_order = out_of_order + 1 end
    expected = number + 1
    if received % 10 == 0 then note("received " .. received .. " last=" .. number .. " out_of_order=" .. out_of_order) end
end)

local started, problem
if role == "host" then started, problem = session.host(port, 2, "Host")
elseif role == "guest" then started, problem = session.join(address, port, "Guest")
else note("no role") end
note(tostring(role) .. (role == "guest" and (" to " .. address) or "") .. " start " .. tostring(started or problem))

time.on_step(function()
    steps = steps + 1
    if steps % 30 ~= 0 then return end
    local info = session.info()
    if not info or info.state == "none" then return end
    local to = role == "host" and 1 or 0
    if (role == "host" and info.state == "hosting") or (role == "guest" and info.state == "joined") then
        local ok, refused = session.send(to, "n" .. (sent + 1), true)
        if ok then sent = sent + 1 end
        if sent % 10 == 0 then note("sent " .. sent .. " state=" .. tostring(info.state) .. (refused and (" refused=" .. tostring(refused)) or "")) end
    end
end)
