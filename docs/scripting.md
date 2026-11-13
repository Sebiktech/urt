# Skriptovanie: resources

Herná logika servera sa píše v skriptoch. Engine sám nevie nič o NPC, prácach
ani o tom, čo sa stane, keď sa niekto pripojí; to všetko robia resources.

Resource môže mať dve časti: skripty servera, ktoré menia hru, a skripty
klienta, ktoré server pošle každému hráčovi a bežia na jeho počítači.

## Čo je resource

Priečinok v `resources/` s manifestom a skriptmi:

```
resources/
  welcome/
    resource.lua     manifest
    server.lua       skript servera
    client.lua       skript klienta
```

Meno resource je meno priečinka. Smie obsahovať len malé písmená, číslice,
`_` a `-`.

Manifest je skript, ktorý vráti tabuľku:

```lua
return {
    name = "welcome",        -- nepovinné; ak je, musí sa zhodovať s priečinkom
    version = "1.0.0",       -- nepovinné
    server = { "server.lua" }, -- skripty servera, spustia sa v tomto poradí
    client = { "client.lua" }, -- skripty, ktoré dostane a spustí každý klient
}
```

Cesty skriptov sú relatívne k priečinku resource a nesmú z neho viesť von.

## Server a klient

| | Skript servera | Skript klienta |
|---|---|---|
| Kde beží | na serveri, raz | u každého hráča zvlášť |
| Čo smie | meniť hru: entity, polohy, hráčov | pozerať sa na svet, farbiť entity u seba |
| Komu sa verí | je to tvoj kód na tvojom stroji | hráč ho vidí a môže si ho upraviť |

Z posledného riadku plynú dve pravidlá:

- **Do skriptu klienta nepatrí nič tajné.** Server ho posiela každému, kto sa
  pripojí.
- **Server musí overiť všetko, čo mu klient pošle.** Event od klienta môže
  mať ľubovoľné meno aj argumenty. Pred použitím skontroluj typy a rozsahy a
  nikdy neber od klienta rozhodnutie, ktoré patrí serveru („daj mi peniaze").

Klient dostane skripty hneď po pripojení a znova po každom reloade resource na
serveri. Keď sa resource na serveri zastaví, zastaví sa aj u klientov.

## Jazyk

Skripty sú v jazyku [Luau](https://luau.org). Je to Lua 5.1 s rozšíreniami,
takže bežný Lua kód funguje. Navyše má napríklad `x += 1`, `continue`,
reťazce s vloženými hodnotami `` `{meno} prišiel` `` a cyklus `for _, v in
tabulka do` bez `pairs`.

Oproti bežnej Lue chýba všetko, čím by skript siahol mimo hry: `io`,
`require`, `loadstring`, `os.execute`, `os.remove`, `os.getenv`. Z `os` ostali
len `os.clock`, `os.time` a `os.date`.

## Izolácia a limity

Každý resource má vlastný virtuálny stroj, na serveri aj u klienta. Globálne premenné jedného resource
druhý nevidí; viac skriptov toho istého resource ich zdieľa. Štandardná
knižnica a tabuľka `urt` sú len na čítanie.

| Limit | Predvolená hodnota | Čo sa stane pri prekročení |
|---|---|---|
| čas jedného volania skriptu | 0,25 s (načítanie súboru 1 s) | resource sa zastaví |
| pamäť | 64 MB | resource sa zastaví |
| entity vytvorené naraz | 4096 | `urt.spawn` vyhodí chybu |
| handlery eventov, časovače | po 1024 | registrácia vyhodí chybu |
| veľkosť skriptu | 1 MB | resource sa nenačíta |

Limity platia rovnako u klienta: skript zo servera mu nevie zavesiť hru ani
zabrať pamäť. Zastavený resource príde o všetko, čo vytvoril, a server (alebo
klient) beží ďalej. Časový
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

Reload sa týka aj klientov: dostanú novú verziu skriptov klienta a starú
zastavia. Nový priečinok v `resources/` sa načíta sám, zmazaný sa zastaví. Resource,
ktorý sa nepodarilo načítať, sa skúsi znova, keď sa niektorý jeho súbor zmení.

## API

Všetko je v tabuľke `urt`. Uhly sú v radiánoch, vzdialenosti v metroch, čas
v sekundách. Os Y smeruje hore; natočenie `yaw` 0 znamená pohľad v smere +X.

### Na oboch stranách

| Funkcia | Popis |
|---|---|
| `urt.log(...)` | Vypíše hodnoty do logu s menom resource. `print(...)` robí to isté. |
| `urt.resourceName` | Meno tohto resource. |
| `urt.on(event, fn)` | Zaregistruje funkciu na event enginu (tabuľka nižšie). |
| `urt.onNet(event, fn)` | Zaregistruje funkciu na event od skriptov druhej strany. |
| `urt.after(sekundy, fn)` | Zavolá `fn` raz po uplynutí času. Vráti id časovača. |
| `urt.every(sekundy, fn)` | Volá `fn` opakovane, najviac raz za tick. Vráti id časovača. |
| `urt.clearTimer(id)` | Zruší časovač. Vráti `true`, ak existoval. |
| `urt.time()` | Čas simulácie od štartu tejto strany. |
| `urt.exists(id)` | `true`, ak entita existuje (u klienta: ak ju práve vidí). |
| `urt.getPosition(id)` | Vráti `x, y, z`. |
| `urt.getYaw(id)` | Natočenie. |

Eventy enginu pre `urt.on`:

| Event | Strana | Argumenty | Kedy |
|---|---|---|---|
| `"tick"` | obe | `dt` | každý tick; `dt` je dĺžka ticku |
| `"stop"` | obe | | resource sa zastavuje alebo reloaduje |
| `"playerJoined"` | server | `player` | hráč dokončil pripojenie |
| `"playerLeft"` | server | `player` | hráč odišiel alebo bol odpojený |

`player` je tabuľka `{ id, name, x, y, z }` so stavom v okamihu eventu. `id`
je zároveň id entity hráča. Neznáme meno eventu je chyba, aby sa preklep
neprejavil tichým nefungovaním.

### Len na serveri

| Funkcia | Popis |
|---|---|
| `urt.spawn({ x, y, z, yaw })` | Vytvorí entitu a vráti jej id. Všetky polia sú nepovinné. |
| `urt.despawn(id)` | Zmaže entitu, ktorú vytvoril tento resource. |
| `urt.setPosition(id, x, y, z)` | Presunie entitu. Na hráčovi je to teleport. |
| `urt.setYaw(id, yaw)` | Natočí entitu. |
| `urt.setVelocity(id, vx, vy, vz)` | Rýchlosť v m/s; entita sa hýbe, kým ju nezmeníš. Na hráča nemá vplyv, jeho rýchlosť určujú jeho vstupy. |
| `urt.players()` | Pole tabuliek `{ id, name, x, y, z }` všetkých pripojených hráčov. |
| `urt.emitClient(idHraca, event, ...)` | Pošle event skriptom jedného klienta. Vráti `false`, ak taký hráč nie je. |
| `urt.emitAllClients(event, ...)` | Pošle event skriptom všetkých klientov. |

Funkcie odmietnu čísla, ktoré nie sú konečné, súradnice nad milión metrov a
rýchlosti nad 1000 m/s. Jedno NaN v polohe by inak dostal každý klient
v okolí.

### Len na klientovi

| Funkcia | Popis |
|---|---|
| `urt.emitServer(event, ...)` | Pošle event skriptom servera. |
| `urt.localPlayer()` | Id vlastného hráča. |
| `urt.entities()` | Pole id všetkých entít, ktoré klient práve vidí. |
| `urt.setColor(id, r, g, b)` | Nakreslí entitu touto farbou (zložky 0 až 1). Vidí to len tento klient. |
| `urt.clearColor(id)` | Vráti entite pôvodnú farbu. |

Klient vidí len entity vo svojom okolí. Keď entita z dohľadu odíde a vráti sa,
farba nastavená cez `setColor` je preč.

## Eventy medzi klientom a serverom

```lua
-- client.lua
urt.emitServer("shop:buy", "apple", 3)

-- server.lua
urt.onNet("shop:buy", function(player, item, count)
    if type(item) ~= "string" or type(count) ~= "number" then return end
    -- ... over, či si to hráč môže dovoliť, a až potom konaj
    urt.emitClient(player.id, "shop:bought", item, count)
end)
```

Handler na serveri dostane ako prvý argument hráča, ktorý event poslal; ten
doplní server, klient ho nevie sfalšovať. Handler na klientovi dostane len
argumenty.

Mená eventov sú spoločné pre všetky resources, preto ich začínaj menom svojho
resource (`shop:buy`). Meno má 1 až 64 znakov bez medzier.

Eventy sa doručia spoľahlivo a v poradí, v akom boli poslané.

**Čo sa dá poslať:** `nil`, `true`/`false`, čísla, reťazce a tabuľky z nich
s kľúčmi, ktoré sú čísla alebo reťazce. Funkcie a korutiny poslať nejdú, ani
NaN a nekonečno.

| Limit | Hodnota |
|---|---|
| argumentov v jednom evente | 16 |
| vnorenie tabuliek | 8 úrovní |
| hodnôt spolu (kľúče aj hodnoty) | 2048 |
| dĺžka jedného reťazca | 8 KB |
| veľkosť celého eventu | 16 KB |

Prekročenie limitu je chyba vo volaní `emit...`, event sa nepošle.

Server navyše obmedzuje, ako rýchlo môže klient eventy posielať: 30 za sekundu
so zásobou 60. Nadbytočné zahodí a klienta, ktorý neprestane, odpojí.

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

`resources/welcome` ukazuje obe strany. Server postaví k novému hráčovi
postavu a pošle jeho klientovi jej id; skript klienta ju u seba zafarbí na
zeleno a pošle serveru pozdrav:

```lua
-- server.lua
urt.on("playerJoined", function(player)
    local greeter = urt.spawn({ x = player.x + 2, z = player.z })
    urt.emitClient(player.id, "welcome:greeter", greeter)
    urt.after(10, function() urt.despawn(greeter) end)
end)

urt.onNet("welcome:hello", function(player, text)
    if type(text) ~= "string" or #text > 100 then return end
    urt.log(`{player.name} says: {text}`)
end)
```

```lua
-- client.lua
urt.emitServer("welcome:hello", "hello from my client script")

local waiting = {}
urt.onNet("welcome:greeter", function(id)
    if type(id) == "number" then waiting[id] = true end
end)

-- Správa môže prísť skôr, než klient entitu uvidí, preto sa čaká na tick.
urt.on("tick", function()
    for id in waiting do
        if urt.exists(id) then
            urt.setColor(id, 0.3, 0.9, 0.4)
            waiting[id] = nil
        end
    end
end)
```
