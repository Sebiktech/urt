-- A greeter appears next to every new player and leaves after ten seconds.

urt.on("playerJoined", function(player)
    urt.log(`{player.name} joined, {#urt.players()} online`)

    local greeter = urt.spawn({ x = player.x + 2, z = player.z })
    urt.after(10, function()
        urt.despawn(greeter)
    end)
end)

urt.on("playerLeft", function(player)
    urt.log(`{player.name} left`)
end)
