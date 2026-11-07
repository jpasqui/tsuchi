local cases = {0, 0x20000000000001, 0x7fffffffffffffff, 0x8000000000000000, 0xffffffffffffffff}
assert(cases[2] == 9007199254740993)
assert(cases[4] == math.mininteger and cases[5] == -1)
local expected, observed = nil, 0
local subscription = entity.on_spawned(function(event)
    assert(math.type(event.placement) == "integer" and event.placement == expected)
    local placed = assert(entity.find_placed(event.placement))
    assert(entity.placement(placed) == expected)
    observed = observed + 1
end)
assert(subscription)
for _, value in ipairs(cases) do
    next_case()
    expected = value
    assert(math.type(value) == "integer")
    local placed = assert(entity.find_placed(value))
    local returned = assert(entity.placement(placed))
    assert(math.type(returned) == "integer" and returned == value)
    assert(entity.find_placed(returned) == placed)
    assert(precision.capture(value) == value)
    local record = assert(precision.roundtrip({placement=value}))
    assert(math.type(record.placement) == "integer" and record.placement == value)
    assert(precision.capture(record.placement) == value)
    assert(precision.with_default() == -1 and precision.with_default(nil) == -1)
    assert(precision.with_default(value) == value)
    send_spawned()
end
assert(observed == 5)
-- A whole float below 2^53 is exact, so it is taken as that integer.
for _, case in ipairs({{0.0, 0}, {-0.0, 0}, {1.0, 1}, {4 / 2, 2}, {9007199254740991.0, 9007199254740991}}) do
    local value = precision.echo(case[1])
    assert(math.type(value) == "integer" and value == case[2])
end
assert(precision.echo(-1) == -1) -- an integer keeps all 64 bits, the high bit included
for _, value in ipairs({0.5, -1.0, -0.5, 9007199254740992.0, 9223372036854775808.0, -9223372036854775808.0,
                        1 / 0, -1 / 0, 0 / 0, "0", "1", "9007199254740993", "-1", false, {}}) do
    assert(not pcall(precision.echo, value))
    assert(not pcall(entity.find_placed, value))
    assert(not pcall(precision.capture, value))
    assert(not pcall(precision.roundtrip, {placement=value}))
    assert(not pcall(precision.with_default, value))
end
assert(not pcall(entity.find_placed, nil))
assert(not pcall(precision.capture))
assert(not pcall(precision.roundtrip, {}))
-- Smaller unsigned arguments retain their original range and coercion rules.
assert(precision.bounded(0) == 0 and precision.bounded(0xffffffff) == 0xffffffff)
assert(precision.bounded("1") == 1 and precision.bounded(1.0) == 1)
assert(not pcall(precision.bounded, -1))
assert(not pcall(precision.bounded, 0x100000000))
