-- Observes one whistle from a captured captain. Parent input custody is separate.
local M = {}

function M.handle_hex(value)
    if type(value) ~= "userdata" then return nil end
    local decimal = tostring(value):match("^tsuchi%.actor: (%-?%d+)$")
    local number = decimal and tonumber(decimal)
    if math.type(number) ~= "integer" or number == 0 or tostring(number) ~= decimal then return nil end
    return string.format("%016x", number)
end

function M.new(config, api)
    if type(config) ~= "table" or type(api) ~= "table" then return nil, "config:table" end
    if not M.handle_hex(config.captain) or not M.handle_hex(config.victim) then return nil, "config:full_handles" end
    if type(config.world) ~= "string" or not config.world:match("^[A-Za-z][A-Za-z0-9_]+$") then return nil, "config:world" end
    if type(config.nonce) ~= "string" or #config.nonce ~= 32 or not config.nonce:match("^[0-9a-f]+$") then return nil, "config:nonce" end
    if type(config.kind) ~= "string" or config.kind == "" then return nil, "config:kind" end
    if type(config.public_kind) ~= "string" or config.public_kind == "" then return nil, "config:public_kind" end
    local limit = config.max_polls or 18000
    if math.type(limit) ~= "integer" or limit < 2 or limit > 18000 then return nil, "config:poll_budget" end
    local calls = {}
    for _, name in ipairs({"world", "has_control", "captain", "controlled", "alive", "kind", "public_kind", "color", "leaf", "whistle"}) do
        if type(api[name]) ~= "function" then return nil, "config:api:" .. name end
        calls[name] = api[name]
    end
    local owner, victim, world, kind, public_kind = config.captain, config.victim, config.world, config.kind, config.public_kind
    local baseline, candidate, failed, polls, observed = nil, nil, nil, 0, false
    local gate = {}
    local function call(name, ...)
        local okay, value, why = pcall(calls[name], ...)
        if not okay then return nil, name .. ":exception" end
        if why ~= nil then return nil, name .. ":error:" .. tostring(why) end
        return value
    end
    local function refuse(why) failed = why observed = false return nil, why end
    local function context()
        local actual, why = call("world")
        if why then return nil, why end
        if type(actual) ~= "string" or actual ~= world then return nil, "world:drift" end
        actual, why = call("has_control")
        if why then return nil, why end
        if type(actual) ~= "boolean" or not actual then return nil, "control:lost" end
        for _, name in ipairs({"captain", "controlled"}) do
            actual, why = call(name, 0)
            if why then return nil, why end
            if actual ~= owner then return nil, name .. ":drift" end
        end
        for _, actor in ipairs({owner, victim}) do
            actual, why = call("alive", actor)
            if why then return nil, why end
            if type(actual) ~= "boolean" or not actual then return nil, "identity:lost" end
        end
        for name, expected in pairs({kind = kind, public_kind = public_kind, color = 19, leaf = "flower"}) do
            actual, why = call(name, victim)
            if why then return nil, why end
            if actual ~= expected then return nil, "victim:" .. name .. ":drift" end
        end
        actual, why = call("whistle", owner)
        if why then return nil, why end
        if type(actual) ~= "table" or type(actual.active) ~= "boolean" or math.type(actual.count) ~= "integer"
            or actual.count < 0 or actual.count > 4294967295 then return nil, "whistle:invalid_u32_or_active" end
        return {count = actual.count, active = actual.active}
    end
    function gate.poll()
        if failed then return nil, failed end
        polls = polls + 1
        if polls > limit then return refuse("whistle:poll_budget") end
        local first, why = context()
        if not first then return refuse(why) end
        local second
        second, why = context()
        if not second then return refuse(why) end
        if first.count ~= second.count or first.active ~= second.active then return refuse("whistle:mixed_snapshot") end
        if baseline == nil then
            if first.active then candidate = nil return false, "whistle:baseline_active" end
            if first.count == 4294967295 then return refuse("whistle:baseline_wrap") end
            if candidate == nil then candidate = first.count return false, "whistle:baseline_first" end
            if first.count ~= candidate then return refuse("whistle:baseline_changed") end
            baseline = candidate
            return false, "whistle:armed"
        end
        if first.count < baseline then return refuse("whistle:counter_decrease") end
        if first.count > baseline + 1 then return refuse("whistle:ambiguous_multiple") end
        if first.count == baseline then
            if observed then return refuse("whistle:counter_decrease") end
            return false, "whistle:waiting"
        end
        if first.active then return false, "whistle:release_required" end
        observed = true
        return true, "whistle:observed_once_inactive"
    end
    function gate.revalidate() return gate.poll() end
    function gate.status()
        return failed and "refused" or (observed and "observed_once_inactive" or (baseline and "armed" or "waiting")),
            failed, baseline, polls
    end
    return gate
end

return M
