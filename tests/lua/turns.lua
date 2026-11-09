-- Handlers for turn_test.cpp. Listener 1 calls listener 2 in the middle of its turn. Listener 3 calls listener 4,
-- which runs until the shared turn is used up.

local function burn(count)
    local total = 0
    for i = 1, count do total = total + i end
    return total
end

subscribe(function()
    nested_kept_count = false
    burn(200000)
    local before = turn_used()
    fire(2)
    nested_kept_count = turn_used() >= before
end)

subscribe(function()
    burn(1000)
end)

subscribe(function()
    outer_finished = false
    burn(3000000)
    fire(4)
    outer_finished = true
end)

subscribe(function()
    while true do end
end)
