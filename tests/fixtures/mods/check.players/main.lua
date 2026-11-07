local player_api = player

-- Reads the players; holds their current character and controller, then gives both back at once.
local check = require("check")
check.plan("counted", "characters", "pads", "held", "refused")
check.retry(function()
    if game.has_control() ~= true then return check.run("counted", function() check.wait("a loaded game with player control") end) end
    local count = player_api.count()
    if not count or count == 0 then return check.run("counted", function() check.wait("one or two players in play") end) end
    for player = 0, count - 1 do
        local captain, character = player_api.captain(player), player_api.character(player)
        if not captain or not character or not actor.alive(captain) or not actor.alive(character) then
            return check.run("counted", function() check.wait("each player's captain and steered character in play") end)
        end
    end
    check.run("counted", function() check.expect(count > 0 and count <= 2, "a world of play has one or two players") end)
    check.run("characters", function()
        for player = 0, count - 1 do
            local captain = check.result("ok", "the captain", player_api.captain(player))
            check.equal(actor.alive(captain), true, "the captain is in play")
            local dog, result = player_api.dog(player)
            check.expect(result == nil, "asking about a dog succeeded")
            if dog then check.equal(actor.alive(dog), true, "the dog is in play") end
            local character = check.result("ok", "the steered character", player_api.character(player))
            check.expect(character == captain or character == dog, "the player steers their captain or dog")
        end
    end)
    check.run("pads", function()
        for player = 0, count - 1 do
            local pad = check.result("ok", "the controller", player_api.pad(player))
            check.expect(pad == 0 or pad == 1 or pad == 2 or pad == 3 or pad == 7 or pad == 32, "the game names a controller or none")
        end
    end)
    check.run("held", function()
        for player = 0, count - 1 do
            local character, pad = player_api.character(player), player_api.pad(player)
            local character_held = check.result("ok", "holding the character already steered", player_api.hold_character(player, character))
            if character_held then
                check.equal(player_api.character(player), character, "the held character")
                check.result("ok", "giving the character back", player_api.release_character(player))
                check.result("not_found", "giving it back twice", player_api.release_character(player))
            end
            local pad_held = check.result("ok", "holding the current controller", player_api.hold_pad(player, pad))
            if pad_held then
                check.equal(player_api.pad(player), pad, "the held controller")
                check.result("ok", "giving the controller back", player_api.release_pad(player))
                check.result("not_found", "giving it back twice", player_api.release_pad(player))
                check.equal(player_api.pad(player), pad, "the restored controller")
            end
        end
    end)
    check.run("refused", function()
        check.result("not_found", "a player outside the count", player_api.captain(count))
        check.result("invalid", "no such controller", player_api.hold_pad(0, 99))
    end)
    return "pass"
end)
