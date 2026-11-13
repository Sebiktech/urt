# URT – Universal Roleplay Tool

Vlastný engine, hra a editor pre roleplay servery. Server je autorita, herná
logika beží v Lua resources a editor je klient v inom režime.

Stav: **krok 1 z 7 (jadro) je hotový.** Zatiaľ žiadna sieť, grafika ani Lua.

## Čo je v repozitári

| Cesta | Obsah |
|---|---|
| `engine/` | knižnica `urt_engine`, spoločná pre server aj klienta, bez grafiky |
| `engine/include/urt/core/` | logovanie, čas a pevný tick, platformová vrstva, job systém, argumenty |
| `engine/include/urt/ecs/` | svet (EnTT), komponenty, rozhranie systému, `MovementSystem` |
| `engine/include/urt/app/` | `Application`: hlavná slučka nad svetom a systémami |
| `apps/server/` | `urt_server`, dedikovaný headless server |
| `apps/client/` | `urt_client`, zatiaľ len kostra bez okna |
| `tests/` | unit testy (doctest) a smoke test servera |
| `cmake/` | prepínače kompilátora a závislosti |

## Požiadavky

- CMake 3.25 alebo novší
- kompilátor s C++20: Visual Studio 2022, GCC 13+ alebo Clang 16+
- Git a pripojenie na internet pri prvej konfigurácii (stiahnu sa EnTT, GLM a doctest)

## Build a spustenie

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Binárky sú v `build/dev/bin/`. Na Windows s Visual Studiom pridaj k buildu a
testom `--config Debug` a binárky hľadaj v `build/dev/bin/Debug/`.

```sh
./build/dev/bin/urt_server --log-level debug
./build/dev/bin/urt_server --help
```

Server beží, kým nestlačíš Ctrl+C. Každých 5 sekúnd vypíše priemerný a najhorší
čas ticku oproti rozpočtu (pri 30 tickoch/s je to 33,3 ms).

Ďalšie presety: `release`, `asan` (chyby pamäte) a `tsan` (data races). Posledné
dva fungujú len s GCC a Clangom; po každej zmene v job systéme pusť `tsan`.

## Pravidlá architektúry

1. **Server je autorita.** Klient posiela vstupy, server rozhoduje o stave.
2. **`urt_engine` nesmie obsahovať okno ani grafiku.** Renderer bude samostatná
   knižnica, ktorú linkuje len klient, aby server bežal aj bez GPU.
3. **Simulácia beží v pevných tickoch.** Systémy dostanú vždy rovnaké `dt`,
   nikdy skutočný čas snímky. `Application::step()` preto dáva na každom stroji
   rovnaký výsledok a dá sa testovať.
4. **Engine nevie nič o RP.** Joby, inventár a ekonomika budú Lua resources.
5. **Dáta v komponentoch, logika v systémoch.** Komponent je obyčajná štruktúra
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
2. Sieťová kostra: server a dvaja klienti, replikácia entít, predikcia, interest management
3. Lua resources: manifest, client/server/shared skripty, eventy a RPC, hot reload
4. Renderer: glTF, PBR, tiene, animácie
5. Fyzika: Jolt, character controller, prvé vozidlo
6. Svet a editor: streamovanie, editor v hre
7. Služby: účty, práva, PostgreSQL, UI pre skripty, hlas
