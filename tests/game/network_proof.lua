local notes = {}
local finished = false
local elapsed = 0
local request
local socket
local udp_done = false
local https_done = false
local function record(key, value)
    notes[#notes + 1] = key .. "=" .. tostring(value)
    mod.log(notes[#notes])
end
local function save_result()
    if finished or not (udp_done and https_done) then return end
    finished = true
    local saved, why = storage.write("result.txt", table.concat(notes, "\n") .. "\n")
    if not saved then mod.log("result write refused=" .. tostring(why)) end
end
local status, why = net.status()
record("network_status", status and status.available or why)
if status then record("system_connected", status.connected) end
socket, why = net.udp_open(0, false)
record("udp_open", socket and "ok" or why)
if socket then
    local local_address = net.local_address(socket)
    record("local_port", local_address and local_address.port or "missing")
    if local_address then
        local sent, why = net.send(socket, {ipv4 = 0x7f000001, port = local_address.port}, "captain")
        record("udp_send", sent and "ok" or why)
        if not sent then net.close(socket) socket = nil udp_done = true end
    else
        net.close(socket)
        socket = nil
        udp_done = true
    end
else
    udp_done = true
end
net.on_completed(function(event)
    if request and event.request ~= request then return end
    local state, why = net.request_info(event.request)
    record("https_completion", state and state.state or why)
    if state then
        record("https_status", state.http_status)
        record("https_failure", state.failure)
        record("https_bytes", state.received)
        if state.state == "succeeded" then
            local body, why = net.read(event.request, 0, 65536)
            record("https_read", body and #body or why)
            if body then record("https_prefix", string.sub(body, 1, 32)) end
        end
    end
    record("https_release", net.release_request(event.request))
    https_done = true
    save_result()
end)
request, why = net.get("https://example.com/", 65536, 30000)
record("https_get", request and "ok" or why)
if not request then https_done = true save_result() end
time.on_step(function()
    elapsed = elapsed + 1
    if socket and not udp_done then
        local message, why = net.receive(socket, 65507)
        if message then
            record("udp_received", message)
            local sender = net.sender(socket)
            record("udp_sender", sender and sender.ipv4 or "missing")
            record("udp_close", net.close(socket))
            socket = nil
            udp_done = true
            save_result()
        elseif why ~= "not_ready" then
            record("udp_receive", why)
            net.close(socket)
            socket = nil
            udp_done = true
            save_result()
        end
    end
    if not finished and elapsed >= 1200 then
        record("timeout", "forty seconds")
        if request then net.release_request(request) end
        if socket then net.close(socket) end
        request = nil
        socket = nil
        udp_done = true
        https_done = true
        save_result()
    end
end)
