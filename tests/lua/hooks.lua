local target = {entry_at=entry, stock=0, signature='u64(u64,u64)'}
local function remove(h) assert(hook.remove(h)); frame() end
local saved
local h = assert(hook.wrap(target, function(original, a, b)
    saved = original
    assert(a == 0xfedcba9876543210 and b == 2)
    local nope, why = hook.wrap(target, function() end)
    assert(nope == nil and why == 'invalid')
    return original(a, b) + 10
end))
assert(call_game(0xfedcba9876543210, 2) == 0xfedcba987654321c)
assert(not pcall(saved, 1, 2))
assert(call_game(1, 2, true) == 3)
remove(h)
-- A coroutine may install a hook, but the VM's main thread owns its callback.
local co = coroutine.create(function() h = assert(hook.wrap(target, function(_,a,b) return a+b+1 end)) end)
assert(coroutine.resume(co)); co = nil; collectgarbage('collect')
assert(call_game(2,3) == 6); remove(h)
local before = calls()
h = assert(hook.wrap(target, function() error('before original') end))
local told = errors()
assert(call_game(2, 3) == 5 and calls() == before + 1 and errors() == told)
frame(); assert(errors() == told + 1); remove(h)
h = assert(hook.wrap(target, function(original, a, b) original(a, b); original(8, 9); error('after original') end))
before = calls(); assert(call_game(2, 3) == 17 and calls() == before + 2); remove(h)
h = assert(hook.wrap(target, function() coroutine.yield() end))
assert(call_game(2, 3) == 5); remove(h)
h = assert(hook.wrap(target, function() return 'wrong type' end))
assert(call_game(2, 3) == 5); remove(h)

local records = {}
h = assert(hook.watch(target, function(r) records[#records+1] = r end))
assert(call_game(4, 5) == 9); assert(call_game(6, 7, true) == 13); assert(#records == 0)
frame(); assert(#records == 2 and records[1].args[1] == 4 and records[1].result == 9 and records[2].thread == 'other')
for i=1,260 do call_game(i, 1) end
frame(); assert(#records == 258 and records[3].dropped == 4); remove(h)
-- Removal and replacement during delivery must not reclaim the active ring or its callback.
local replacement
h = assert(hook.watch(target, function()
    assert(hook.remove(h))
    replacement = assert(hook.wrap(target, function() return 77 end))
end))
call_game(1,2); frame(); assert(call_game(1,2) == 77); remove(replacement)

local text_target = {entry_at=text_entry,stock=0,signature='u64(text16)'}
h = assert(hook.wrap(text_target,function(original,text)
    assert(text == nil or text == 'A🚀'); return original(text)
end))
assert(call_text(false) == 65 and call_text(true) == 0); remove(h)
local text_record
h = assert(hook.watch(text_target,function(r) text_record = r end))
assert(call_text(false) == 65); frame(); assert(text_record.args[1] == 'A🚀' and text_record.result == 65); remove(h)

local kept
h = assert(hook.instruction(0x900, 0x1a9f97e8, function(r)
    kept = r; assert(r.x[0] == 0xfedcba9876543210)
    r.x[0] = -1; r.z = true; r.c = false
end))
assert(call_instruction() == 0x40000000); kept.z = false
assert(call_instruction() == 0x40000000); remove(h)
h = assert(hook.instruction(0x900, 0x1a9f97e8, function(r) r.z = true; r.x[30] = 'bad' end))
assert(call_instruction() == 0x20000000); remove(h)
h = assert(hook.instruction(0x900, 0x1a9f97e8, function(r) r.z = true; error('discard') end))
assert(call_instruction() == 0x20000000); remove(h)

-- Plain code checks and patches go to the same backend as a compiled mod's.
assert(hook.check_code(0x900, '\xe8\x97\x9f\x1a') == true)
local patched, refused = hook.patch_instruction(0x900, 0x1a9f97e8, 0xd503201f)
assert(patched == nil and refused == 'not_ready')
-- A named entry of the game table: a name it doesn't have, or a name and an address together, are refused.
local missing, unknown = hook.wrap({slot='no_such_slot', signature='u64(u64)'}, function() end)
assert(missing == nil and unknown == 'not_found')
local both, mixed = hook.watch({slot='engine_tick', entry_at=entry, signature='u64(u64)'}, function() end)
assert(both == nil and mixed == 'invalid')

-- Text passed on unchanged reaches the game as its own pointer; changed text is a copy. `stock` can be left out.
local pointer_target = {entry_at=pointer_entry, signature='u64(text16)'}
h = assert(hook.wrap(pointer_target, function(original, text) return original(text) end))
assert(same_pointer() == 1); remove(h)
h = assert(hook.wrap(pointer_target, function(original, text) return original(text .. '!') end))
assert(same_pointer() == 0); remove(h)

local invalid, why = hook.wrap({entry_at=entry,stock=0,signature='u64(u64,u64,u64,u64,u64,u64,u64,u64,u64)'}, function() end)
assert(invalid == nil and why == 'unsupported')
-- The C++ driver calls this after the script returns, so it gets the smaller hook turn.
assert(hook.wrap(target, function() while true do pcall(function() while true do end end) end end))
