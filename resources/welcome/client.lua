-- Runs on every player's computer: the server sends this file when they join.

urt.emitServer("welcome:hello", "hello from my client script")

-- The server tells us which entity is our greeter. The message can arrive a
-- moment before the entity itself becomes visible, so remember the id and
-- colour the entity as soon as it shows up.
local waiting = {}

urt.onNet("welcome:greeter", function(id)
    if type(id) == "number" then
        waiting[id] = true
    end
end)

urt.on("tick", function()
    for id in waiting do
        if urt.exists(id) then
            urt.setColor(id, 0.3, 0.9, 0.4) -- green
            waiting[id] = nil
        end
    end
end)
