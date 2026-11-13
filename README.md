# URT – Universal Roleplay Tool

Vlastný engine, hra a editor pre roleplay servery. Server je autorita, herná
logika beží v Lua resources a editor je klient v inom režime.

Stav: **krok 1 (jadro) je hotový, z kroku 2 funguje sieťová kostra.** Hráči sa
pripoja, chodia a vidia sa navzájom. Zatiaľ bez okna, grafiky a Lua.

## Čo je v repozitári

| Cesta | Obsah |
|---|---|
| `engine/` | knižnica `urt_engine`, spoločná pre server aj klienta, bez grafiky |
| `engine/include/urt/core/` | logovanie, čas a pevný tick, platformová vrstva, job systém, argumenty |
| `engine/include/urt/ecs/` | svet (EnTT), komponenty, rozhranie systému, `MovementSystem` |
| `engine/include/urt/app/` | `Application`: hlavná slučka nad svetom a systémami |
| `engine/include/urt/net/` | transport (ENet), protokol, server, klient, interest management |
| `docs/network.md` | popis sieťového protokolu |
| `apps/server/` | `urt_server`, dedikovaný headless server |
| `apps/client/` | `urt_client`, zatiaľ bez okna: pripojí sa a chodí sám ako bot |
| `tests/` | unit testy (doctest) a smoke test servera |
| `cmake/` | prepínače kompilátora a závislosti |

## Požiadavky

- CMake 3.25 alebo novší
- kompilátor s C++20: Visual Studio 2022, GCC 13+ alebo Clang 16+
- Git a pripojenie na internet pri prvej konfigurácii (stiahnu sa EnTT, GLM, ENet a doctest)

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

Klient zatiaľ nemá okno. Chodí sám v kruhu a raz za sekundu vypíše, kde je,
koľko entít vidí a ktorá je najbližšie. Server vypisuje, kto sa pripojil a
odišiel. Všetko beží, kým nestlačíš Ctrl+C.

Na iný počítač sa pripojíš cez `--connect adresa:port`; predvolený port je
UDP 30777. Windows sa pri prvom spustení servera spýta, či mu povoliť sieť.
Všetky prepínače vypíše `--help`.

Ďalšie presety: `release`, `asan` (chyby pamäte) a `tsan` (data races). Posledné
dva fungujú len s GCC a Clangom; po každej zmene v job systéme pusť `tsan`.

## Pravidlá architektúry

1. **Server je autorita.** Klient posiela vstupy, server rozhoduje o stave.
2. **`urt_engine` nesmie obsahovať okno ani grafiku.** Renderer bude samostatná
   knižnica, ktorú linkuje len klient, aby server bežal aj bez GPU.
3. **Server klientovi neverí.** Každý paket sa overí skôr, než sa použije;
   podrobnosti sú v `docs/network.md`.
4. **Simulácia beží v pevných tickoch.** Systémy dostanú vždy rovnaké `dt`,
   nikdy skutočný čas snímky. `Application::step()` preto dáva na každom stroji
   rovnaký výsledok a dá sa testovať.
5. **Engine nevie nič o RP.** Joby, inventár a ekonomika budú Lua resources.
6. **Dáta v komponentoch, logika v systémoch.** Komponent je obyčajná štruktúra
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
2. Sieťová kostra
   - ~~transport, protokol, pripojenie, replikácia entít, interest management~~
   - predikcia na klientovi a interpolácia ostatných entít
   - jednoduché okno, ktoré kreslí entity ako kapsule
3. Lua resources: manifest, client/server/shared skripty, eventy a RPC, hot reload
4. Renderer: glTF, PBR, tiene, animácie
5. Fyzika: Jolt, character controller, prvé vozidlo
6. Svet a editor: streamovanie, editor v hre
7. Služby: účty, práva, PostgreSQL, UI pre skripty, hlas
