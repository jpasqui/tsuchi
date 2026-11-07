local dialog_api = dialog

-- Select this group on an isolated route: accept the first question, cancel the second, close the message.
local check = require("check")
check.plan("listened", "queued", "ordered", "answered", "cancelled", "refused")
local listener, stepping, requested, received = nil, nil, {}, {}
local begun = false
stepping = time.on_step(function()
    if not begun then
        if game.has_control() ~= true then check.run("listened", function() check.wait("a loaded game with player control") end) return end
        check.run("listened", function()
            listener = check.result("ok", "the dialog answer listener", dialog_api.on_answered(function(event)
                for _, handle in ipairs(requested) do
                    if event.dialog == handle then received[#received + 1] = {dialog = handle, accepted = event.accepted} break end
                end
            end))
        end)
        check.run("queued", function()
            requested[1] = check.result("ok", "the first question", dialog_api.confirm("The SDK dialog check: accept this first question."))
            requested[2] = check.result("ok", "the second question", dialog_api.confirm("The SDK dialog check: cancel this second question."))
            requested[3] = check.result("ok", "the message", dialog_api.message("The SDK dialog check: close this message."))
            check.expect(#requested == 3, "all three requests were queued")
            check.expect(requested[1] ~= requested[2] and requested[2] ~= requested[3] and requested[1] ~= requested[3], "the dialog handles are distinct")
            mod.log("dialogs queued " .. tostring(requested[1]) .. " " .. tostring(requested[2]) .. " " .. tostring(requested[3]))
        end)
        check.run("refused", function()
            check.result("invalid", "a malformed UTF-8 question", dialog_api.confirm(string.char(255)))
            check.result("invalid", "a malformed UTF-8 message", dialog_api.message(string.char(255)))
        end)
        begun = true
        return
    end
    if #received < 3 then return end
    check.run("ordered", function()
        check.equal(#received, 3, "exactly three answers")
        for i, event in ipairs(received) do check.equal(event.dialog, requested[i], "queue order " .. i) end
    end)
    check.run("answered", function()
        check.equal(received[1].accepted, true, "the first question accepted")
        check.equal(received[2].accepted, false, "the second question cancelled")
        check.equal(received[3].accepted, true, "the message closed with its button")
    end)
    check.run("cancelled", function()
        check.expect(listener ~= nil, "the answer listener was registered")
        check.result("ok", "ending the answer listener", listener:cancel())
        check.result("not_found", "ending it twice", listener:cancel())
    end)
    if listener then listener:cancel() end
    stepping:cancel()
end)
