-- Install an Academy schedule mod alongside this example for offline exams.
-- Store the game's submitted fields unchanged, then acknowledge its request.
academy.on_score_submission_requested(function(event)
    local json = string.format('{"version":1,"course":%d,"board_key":%d,"score":%d}\n',
        event.course, event.board_key, event.score)
    local saved, why = storage.write("results/" .. event.course .. ".json", json)
    local completed, reason = academy.complete_score_submission(event.request, saved == true)
    if not completed then mod.log("Submission: " .. tostring(reason))
    elseif not saved then mod.log("Write: " .. tostring(why)) end
end)
for course = 0, 71 do
    local ok, why = academy.hold_score_submissions(course)
    if not ok then error("Score destination: " .. tostring(why)) end
end
