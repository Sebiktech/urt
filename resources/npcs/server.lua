-- Eight characters walking in circles around the spawn area.
--
-- Try it: change a number below and save the file while the server runs.
-- The resource reloads within a second; the old characters disappear and
-- new ones appear.

local NPC_COUNT = 8
local WALK_SPEED = 1.4 -- metres per second
local TURN_RATE = 0.3  -- radians per second

local npcs = {}

for i = 0, NPC_COUNT - 1 do
    local angle = i * 2.4
    local distance = 6 + 2 * i
    local id = urt.spawn({
        x = math.cos(angle) * distance,
        z = math.sin(angle) * distance,
        yaw = angle,
    })
    table.insert(npcs, { id = id, yaw = angle })
end

-- Runs every server tick. dt is the length of the tick in seconds.
urt.on("tick", function(dt)
    for _, npc in npcs do
        -- Turn a little, then walk the way we are facing: that is a circle.
        npc.yaw += TURN_RATE * dt
        urt.setYaw(npc.id, npc.yaw)
        urt.setVelocity(npc.id, math.cos(npc.yaw) * WALK_SPEED, 0, math.sin(npc.yaw) * WALK_SPEED)
    end
end)

urt.log(`spawned {NPC_COUNT} NPCs`)
