local save_api = save

-- Reads the active slot, releases a hold, and prepares then forgets unused stand-in bytes.
local check = require("check")
check.plan("slot", "listed", "read", "held", "released", "prepared", "ended", "refused")
check.retry(function()
    local slot, why = save_api.slot()
    if why == "not_ready" or why == "not_found" then
        return check.run("slot", function() check.wait("a loaded save slot") end)
    end
    local paths, refused = save_api.list(slot)
    if refused == "busy" or refused == "not_ready" then
        return check.run("listed", function() check.wait("the save to be available for reading") end)
    end
    local state = check.run("slot", function()
        check.result("ok", "the active slot", slot, why)
        check.expect(slot >= 0 and slot <= 5, "the slot is one of the game's six")
    end)
    check.run("listed", function()
        check.result("ok", "the slot's paths", paths, refused)
        check.expect(#paths > 0, "the loaded slot has files")
        for _, path in ipairs(paths) do check.equal(save_api.exists(slot, path), true, "the listed file exists") end
    end)
    local first_size, first_bytes
    check.run("read", function()
        local path = paths[1]
        local size = check.result("ok", "the file size", save_api.size(slot, path))
        local bytes = check.result("ok", "a bounded read", save_api.read(slot, path, 0, 64))
        first_size, first_bytes = size, bytes
        check.equal(#bytes, math.min(size, 64), "the read count")
        check.equal(check.result("ok", "reading at the end", save_api.read(slot, path, size, 64)), "", "there are no bytes past the file")
    end)
    local held = false
    check.run("held", function()
        held = check.result("ok", "holding save writes", save_api.hold())
        check.result("ok", "holding twice", save_api.hold())
    end)
    check.run("released", function()
        check.expect(held, "the hold was made")
        check.result("ok", "ending the hold", save_api.release())
        check.result("not_found", "ending it twice", save_api.release())
    end)
    local prepared = false
    check.run("prepared", function()
        prepared = check.result("ok", "preparing unused stand-in bytes", save_api.add_stand_in_file(slot, "tsuchi-check.sav", "bytes the game never loads"))
        check.result("ok", "replacing unused stand-in bytes", save_api.add_stand_in_file(slot, "tsuchi-check.sav", "other unused bytes"))
        check.equal(save_api.exists(slot, "tsuchi-check.sav"), false, "the slot still has its own files")
        check.equal(save_api.size(slot, paths[1]), first_size, "the slot's file size is unchanged")
        check.equal(save_api.read(slot, paths[1], 0, 64), first_bytes, "the slot's file bytes are unchanged")
    end)
    check.run("ended", function()
        check.expect(prepared, "the stand-in was prepared")
        check.result("ok", "forgetting unused stand-in bytes", save_api.end_stand_in())
        check.result("not_found", "ending it twice", save_api.end_stand_in())
        check.result("not_found", "using a stand-in that has ended", save_api.use_stand_in())
    end)
    check.run("refused", function()
        check.result("invalid", "a seventh slot", save_api.list(6))
        check.result("invalid", "a path leaving its slot", save_api.exists(slot, "../single.sav"))
        check.result("invalid", "a stand-in path leaving its slot", save_api.add_stand_in_file(slot, "../single.sav", "unused"))
    end)
    return state
end)
