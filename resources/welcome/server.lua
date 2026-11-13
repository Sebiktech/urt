-- A greeter appears next to every new player and leaves after ten seconds.
-- The player's own client is told which entity it is and paints it green.

urt.on("playerJoined", function(player)
    urt.log(`{player.name} joined, {#urt.players()} online`)

    local greeter = urt.spawn({ x = player.x + 2, z = player.z })
    urt.emitClient(player.id, "welcome:greeter", greeter)
    urt.after(10, function()
        urt.despawn(greeter)
    end)
end)

urt.on("playerLeft", function(player)
    urt.log(`{player.name} left`)
end)

-- Sent by client.lua once it runs. Whatever a client sends must be checked
-- before it is used: a modified client can send anything.
urt.onNet("welcome:hello", function(player, text)
    if type(text) ~= "string" or #text > 100 then
        return
    end
    urt.log(`{player.name} says: {text}`)
end)
