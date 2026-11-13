# URT – Universal Roleplay Tool

Vlastný engine, hra a editor pre roleplay servery. Server je autorita, herná
logika beží v skriptoch (resources) a editor je klient v inom režime.

Stav: **kroky 1 (jadro), 2 (sieť) a 3 (skripty) sú hotové.** Hráči sa pripoja,
chodia a vidia sa navzájom v okne kreslenom cez Vulkan, zatiaľ ako farebné
kvádre. Čo sa vo svete deje, určujú Luau skripty v `resources/`: časť beží na
serveri, časť server pošle klientom, a po uložení súboru sa načítajú znova.

## Čo je v repozitári

| Cesta | Obsah |
|---|---|
| `engine/` | knižnica `urt_engine`, spoločná pre server aj klienta, bez grafiky |
| `engine/include/urt/core/` | logovanie, čas a pevný tick, platformová vrstva, job systém, argumenty |
| `engine/include/urt/ecs/` | svet (EnTT), komponenty, rozhranie systému, `MovementSystem` |
| `engine/include/urt/app/` | `Application`: hlavná slučka nad svetom a systémami |
| `engine/include/urt/script/` | skripty servera a klienta: resources, sandbox, API `urt.*`, hot reload |
| `resources/` | resources, ktoré server načíta: `npcs` a `welcome` ako ukážky |
| `docs/scripting.md` | ako písať resources a čo je v API |
| `engine/include/urt/net/` | transport (ENet), protokol, server, klient, interest management, interpolácia, predikcia |
| `docs/network.md` | popis sieťového protokolu |
| `apps/server/` | `urt_server`, dedikovaný headless server |
| `render/` | knižnica `urt_render`: Vulkan renderer, linkuje ju len klient |
| `apps/client/` | `urt_client`: okno (GLFW), ovládanie, kreslenie sveta |
| `tests/` | unit testy (doctest) a smoke test servera |
| `cmake/` | prepínače kompilátora a závislosti |

## Požiadavky

- CMake 3.25 alebo novší
- kompilátor s C++20: Visual Studio 2022, GCC 13+ alebo Clang 16+
- [Vulkan SDK](https://vulkan.lunarg.com) pre okno klienta; bez neho zostav projekt s
  `-DURT_BUILD_RENDERER=OFF` (server, headless klient a testy)
- Git a pripojenie na internet pri prvej konfigurácii (stiahnu sa EnTT, GLM, ENet, Luau, GLFW a doctest)

## Build a spustenie

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Binárky sú v `build/dev/bin/`. Na Windows s Visual Studiom pridaj k buildu a
testom `--config Debug` a binárky hľadaj v `build/dev/bin/Debug/`.

Spusti server a v ďalších dvoch termináloch dvoch klientov:

```sh
./build/dev/bin/urt_server
./build/dev/bin/urt_client --name Anna
./build/dev/bin/urt_client --name Boris
```

Server načíta resources z priečinka `resources/`. Hľadá ho od aktuálneho
priečinka smerom nahor; inú cestu mu dáš cez `--resources`. Ukážkové resources
vytvoria postavy chodiace v kruhu a ku každému novému hráčovi postavia zelenú
postavu, ktorú zafarbí skript bežiaci u klienta. Skús za behu servera
zmeniť číslo v `resources/npcs/server.lua` a uložiť: postavy sa do sekundy
vymenia. Ako písať vlastné, je v `docs/scripting.md`.

Klient otvorí okno: ty si oranžový kváder, ostatní hráči a NPC modré, biela
kocka ukazuje, kam sa kto pozerá. Chodíš klávesmi WASD alebo šípkami, F12 uloží
snímku do `urt_screenshot.ppm`, Esc okno zavrie.

Vlastná postava reaguje okamžite: klient jej pohyb predpovedá a server ho len
potvrdzuje. Ostatné entity sa hýbu plynulo, lebo ich klient kreslí 0,1 s
v minulosti a polohu dopočítava medzi dvoma snapshotmi. Ako to funguje, je
v `docs/network.md`.

S prepínačom `--headless` klient okno neotvorí, chodí sám v kruhu a raz za
sekundu vypíše, čo vidí. `--validate` zapne kontrolu volaní Vulkanu (v debug
builde je zapnutá vždy, potrebuje Vulkan SDK) a chyby vypíše do konzoly.

Na iný počítač sa pripojíš cez `--connect adresa:port`; predvolený port je
UDP 30777. Windows sa pri prvom spustení servera spýta, či mu povoliť sieť.
Všetky prepínače vypíše `--help`.

Ďalšie presety: `release`, `asan` (chyby pamäte) a `tsan` (data races). Posledné
dva fungujú len s GCC a Clangom; po každej zmene v job systéme pusť `tsan`.

## Pravidlá architektúry

1. **Server je autorita.** Klient posiela vstupy, server rozhoduje o stave.
2. **`urt_engine` nesmie obsahovať okno ani grafiku.** Renderer je samostatná
   knižnica `urt_render`, ktorú linkuje len klient, aby server bežal aj bez GPU.
   Renderer zasa nevie nič o okne, sieti ani ECS: dostane kameru a zoznam vecí
   na kreslenie.
3. **Server klientovi neverí.** Každý paket sa overí skôr, než sa použije;
   podrobnosti sú v `docs/network.md`.
4. **Simulácia beží v pevných tickoch.** Systémy dostanú vždy rovnaké `dt`,
   nikdy skutočný čas snímky. `Application::step()` preto dáva na každom stroji
   rovnaký výsledok a dá sa testovať.
5. **Engine nevie nič o RP.** Joby, inventár a ekonomika budú resources.
6. **Skriptom sa neverí.** Každý resource beží vo vlastnom virtuálnom stroji
   s limitom času a pamäte a k hre sa dostane len cez `urt.*`.
7. **Dáta v komponentoch, logika v systémoch.** Komponent je obyčajná štruktúra
   bez metód; systém je trieda s `tick()`.

## Ako pridať systém

```cpp
class MySystem final : public urt::System {
public:
    std::string_view name() const override { return "my"; }
    void tick(urt::World& world, const urt::TickContext& context) override {
        for (auto [entity, transform] : world.registry.view<urt::Transform>().each()) {
            // ...
        }
    }
};

app.add_system<MySystem>();
```

Systémy bežia v poradí, v akom boli pridané. Na cyklus cez veľa entít použi
`context.jobs.parallel_for`, vzor je v `engine/src/ecs/movement_system.cpp`.

## Plán

1. ~~Jadro: CMake, platforma, job systém, ECS, logovanie, headless build~~
2. ~~Sieť: transport, protokol, replikácia entít, interest management, okno,
   interpolácia, predikcia vlastnej postavy~~
3. ~~Resources v Luau: manifest, sandbox, skripty servera a klienta, API
   `urt.*`, eventy medzi klientom a serverom, hot reload~~
4. Renderer: ~~základ vo Vulkane (swapchain, depth, pipeline)~~, glTF, PBR, tiene, animácie
5. Fyzika: Jolt, character controller, prvé vozidlo
6. Svet a editor: streamovanie, editor v hre
7. Služby: účty, práva, PostgreSQL, UI pre skripty, hlas
