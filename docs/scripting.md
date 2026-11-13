# Skriptovanie: resources

Herná logika servera sa píše v skriptoch. Engine sám nevie nič o NPC, prácach
ani o tom, čo sa stane, keď sa niekto pripojí; to všetko robia resources.

Zatiaľ bežia skripty len na serveri. Skripty na klientovi a eventy medzi
skriptom klienta a servera sú ďalší krok.

## Čo je resource

Priečinok v `resources/` s manifestom a skriptmi:

```
resources/
  welcome/
    resource.lua     manifest
    server.lua       skript
```

Meno resource je meno priečinka. Smie obsahovať len malé písmená, číslice,
`_` a `-`.

Manifest je skript, ktorý vráti tabuľku:

```lua
return {
    name = "welcome",        -- nepovinné; ak je, musí sa zhodovať s priečinkom
    version = "1.0.0",       -- nepovinné
    server = { "server.lua" }, -- skripty servera, spustia sa v tomto poradí
}
```

Cesty skriptov sú relatívne k priečinku resource a nesmú z neho viesť von.

## Jazyk

Skripty sú v jazyku [Luau](https://luau.org). Je to Lua 5.1 s rozšíreniami,
takže bežný Lua kód funguje. Navyše má napríklad `x += 1`, `continue`,
reťazce s vloženými hodnotami `` `{meno} prišiel` `` a cyklus `for _, v in
tabulka do` bez `pairs`.

Oproti bežnej Lue chýba všetko, čím by skript siahol mimo hry: `io`,
`require`, `loadstring`, `os.execute`, `os.remove`, `os.getenv`. Z `os` ostali
len `os.clock`, `os.time` a `os.date`.

## Izolácia a limity

Každý resource má vlastný virtuálny stroj. Globálne premenné jedného resource
druhý nevidí; viac skriptov toho istého resource ich zdieľa. Štandardná
knižnica a tabuľka `urt` sú len na čítanie.

| Limit | Predvolená hodnota | Čo sa stane pri prekročení |
|---|---|---|
| čas jedného volania skriptu | 0,25 s (načítanie súboru 1 s) | resource sa zastaví |
| pamäť | 64 MB | resource sa zastaví |
| entity vytvorené naraz | 4096 | `urt.spawn` vyhodí chybu |
| handlery eventov, časovače | po 1024 | registrácia vyhodí chybu |
| veľkosť skriptu | 1 MB | resource sa nenačíta |

Zastavený resource príde o všetko, čo vytvoril, a server beží ďalej. Časový
limit sa nedá obísť cez `pcall`. Pri ladení servera v debuggeri ho vypni
prepínačom `--script-timeout 0`, inak ti resource zastaví každý breakpoint.

Volanie pomalšie než 5 ms dostane do logu varovanie, najviac raz za 5 sekúnd
na resource.

## Chyby

Chyba pri načítaní (syntax alebo chyba na najvyššej úrovni skriptu) resource
zastaví. Chyba v handleri alebo časovači sa vypíše do logu so súborom a
riadkom a resource beží ďalej:

```
ERROR script [welcome] error in the 'tick' handler: welcome/server.lua:7: attempt to index nil with 'field'
```

## Hot reload

Server sleduje manifest a skripty každého resource. Po uložení súboru resource
do pol sekundy zastaví a spustí znova: zavolá handlery `stop`, zmaže jeho
entity a časovače a skripty načíta od začiatku. Premenné skriptu sa pri tom
nezachovajú.

Nový priečinok v `resources/` sa načíta sám, zmazaný sa zastaví. Resource,
ktorý sa nepodarilo načítať, sa skúsi znova, keď sa niektorý jeho súbor zmení.

## API

Všetko je v tabuľke `urt`. Uhly sú v radiánoch, vzdialenosti v metroch, čas
v sekundách. Os Y smeruje hore; natočenie `yaw` 0 znamená pohľad v smere +X.

### Log

| Funkcia | Popis |
|---|---|
| `urt.log(...)` | Vypíše hodnoty do logu servera s menom resource. `print(...)` robí to isté. |
| `urt.resourceName` | Meno tohto resource. |

### Eventy

`urt.on(event, handler)` zaregistruje funkciu. Na jeden event ich môže byť viac.

| Event | Argumenty | Kedy |
|---|---|---|
| `"playerJoined"` | `player` | hráč dokončil pripojenie |
| `"playerLeft"` | `player` | hráč odišiel alebo bol odpojený |
| `"tick"` | `dt` | každý tick servera; `dt` je dĺžka ticku |
| `"stop"` | | resource sa zastavuje alebo reloaduje |

`player` je tabuľka `{ id, name, x, y, z }` so stavom v okamihu eventu. `id`
je zároveň id entity hráča.

Neznáme meno eventu je chyba, aby sa preklep neprejavil tichým nefungovaním.

### Časovače

| Funkcia | Popis |
|---|---|
| `urt.after(sekundy, fn)` | Zavolá `fn` raz po uplynutí času. Vráti id časovača. |
| `urt.every(sekundy, fn)` | Volá `fn` opakovane, najviac raz za tick. Vráti id časovača. |
| `urt.clearTimer(id)` | Zruší časovač. Vráti `true`, ak existoval. |
| `urt.time()` | Čas simulácie servera od štartu. |

### Entity

Entita je číslo (id). Klienti ju vidia hneď, ako je v ich dosahu.

| Funkcia | Popis |
|---|---|
| `urt.spawn({ x, y, z, yaw })` | Vytvorí entitu a vráti jej id. Všetky polia sú nepovinné. |
| `urt.despawn(id)` | Zmaže entitu, ktorú vytvoril tento resource. |
| `urt.exists(id)` | `true`, ak entita existuje. |
| `urt.getPosition(id)` | Vráti `x, y, z`. |
| `urt.setPosition(id, x, y, z)` | Presunie entitu. Na hráčovi je to teleport. |
| `urt.getYaw(id)` / `urt.setYaw(id, yaw)` | Natočenie. |
| `urt.setVelocity(id, vx, vy, vz)` | Rýchlosť v m/s; entita sa hýbe, kým ju nezmeníš. Na hráča nemá vplyv, jeho rýchlosť určujú jeho vstupy. |

Funkcie odmietnu čísla, ktoré nie sú konečné, súradnice nad milión metrov a
rýchlosti nad 1000 m/s. Jedno NaN v polohe by inak dostal každý klient
v okolí.

### Hráči

| Funkcia | Popis |
|---|---|
| `urt.players()` | Pole tabuliek `{ id, name, x, y, z }` všetkých pripojených hráčov. |

## Príklad

`resources/npcs/server.lua` vytvorí postavy, ktoré chodia v kruhu:

```lua
local WALK_SPEED = 1.4
local TURN_RATE = 0.3
local npcs = {}

for i = 0, 7 do
    local angle = i * 2.4
    local distance = 6 + 2 * i
    local id = urt.spawn({ x = math.cos(angle) * distance, z = math.sin(angle) * distance, yaw = angle })
    table.insert(npcs, { id = id, yaw = angle })
end

urt.on("tick", function(dt)
    for _, npc in npcs do
        npc.yaw += TURN_RATE * dt
        urt.setYaw(npc.id, npc.yaw)
        urt.setVelocity(npc.id, math.cos(npc.yaw) * WALK_SPEED, 0, math.sin(npc.yaw) * WALK_SPEED)
    end
end)
```

Spusti server, zmeň v súbore číslo a ulož: postavy sa do sekundy vymenia.
