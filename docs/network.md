# Sieťový protokol URT (verzia 1)

Tento dokument popisuje, čo si klient a server posielajú. Kód je v
`engine/include/urt/net/` a `engine/src/net/`; pri každej zmene protokolu uprav
oboje a zvýš `protocol_version` v `protocol.hpp`.

## Základné pravidlá

1. **Server je autorita.** Klient posiela, čo hráč chce (smer pohybu), nikdy
   nie kde hráč je. Pozíciu a rýchlosť počíta server.
2. **Server klientovi neverí.** Každý paket sa pred použitím overí: dĺžka, typ,
   kanál, stav spojenia, rozsahy hodnôt. Neplatný paket nesmie server zhodiť
   ani mu zabrať pamäť.
3. **Klient vidí len svoje okolie.** Server posiela entity do vzdialenosti
   `interest_radius` (predvolene 150 m) od hráča.

## Transport

UDP cez knižnicu ENet, zabalenú v triede `Transport`. Zvyšok enginu ENet
nevidí, takže sa dá neskôr vymeniť. Predvolený port je **30777**.

| Kanál | Vlastnosti | Použitie |
|---|---|---|
| 0, spoľahlivý | doručí sa práve raz a v poradí | handshake, neskôr chat a eventy |
| 1, nespoľahlivý | môže sa stratiť, starší paket sa zahodí | vstupy a snapshoty |

Stav, ktorý sa každý tick nahrádza novým, nemá zmysel posielať znova: kým by
stratený snapshot dorazil druhýkrát, je už neaktuálny.

## Formát

Jeden paket je jedna správa. Prvý bajt je typ, čísla sú little-endian,
`f32` je 32-bitový float (NaN a nekonečno sa odmietajú), `string` je jeden
bajt dĺžky a potom znaky.

| Typ | Správa | Smer | Kanál | Polia |
|---|---|---|---|---|
| 1 | `ClientHello` | klient → server | 0 | `u16` verzia, `string` meno (1 až 32 bajtov) |
| 2 | `ServerWelcome` | server → klient | 0 | `u32` id hráča, `u16` tick rate, `u32` tick servera |
| 3 | `ServerReject` | server → klient | 0 | `u8` dôvod: 1 verzia, 2 plný server, 3 zlé meno |
| 4 | `ClientInput` | klient → server | 1 | `u32` poradie, `f32` move_x, `f32` move_z, `f32` yaw |
| 5 | `ServerSnapshot` | server → klient | 1 | `u32` tick, `u32` posledný spracovaný vstup, `u16` počet, entity |

Entita v snapshote má 32 bajtov: `u32` sieťové id, 3× `f32` pozícia, `f32` yaw,
3× `f32` rýchlosť. Jeden snapshot nesie najviac 1024 entít; ak je ich pri
hráčovi viac, pošlú sa najbližšie.

## Priebeh spojenia

```
klient                                server
  | -- UDP spojenie (ENet) ---------> |
  | -- ClientHello -----------------> |  overí verziu, meno, voľné miesto
  | <------------- ServerWelcome ---- |  vytvorí entitu hráča
  | -- ClientInput (každý tick) ----> |
  | <-- ServerSnapshot (každý tick) - |
```

Pri odmietnutí pošle server `ServerReject` a spojenie zavrie. Spojenie, ktoré
do 5 sekúnd nepošle `ClientHello`, server zavrie tiež.

## Jeden tick servera

1. `net::Server` prečíta pakety, prijme nových hráčov a zo vstupov nastaví
   rýchlosti.
2. Simulácia (`MovementSystem` a ďalšie systémy) pohne svetom.
3. `net::ServerSnapshotSystem` pošle každému hráčovi entity z jeho okolia.

Systémy treba do `Application` pridať v tomto poradí.

## Ako server spracuje vstup

- `move_x` a `move_z` oreže na −1 až 1 a vektor skráti na dĺžku najviac 1.
- Rýchlosť je smer × `walk_speed` (4 m/s). Upravený klient preto nevie
  chodiť rýchlejšie.
- Použije len vstup s vyšším poradím, než mal posledný spracovaný.
- Ak vstupy pol sekundy nechodia, hráč zastane.

## Čo server považuje za porušenie protokolu

Spojenie sa zavrie a hráč sa okamžite odstráni zo sveta, keď príde:

- neznámy typ správy alebo správa určená klientovi,
- `ClientHello` na nesprávnom kanáli, poškodený alebo druhýkrát,
- `ClientInput` pred dokončením handshaku alebo na nesprávnom kanáli.

Poškodený `ClientInput` sa len zahodí, lebo nespoľahlivý paket sa môže
poškodiť aj bez zlého úmyslu.

## Sieťové id

Lokálne id entity v ECS je na každom stroji iné, preto sa po sieti neposiela.
Server prideľuje každej replikovanej entite `NetId` (komponent `NetworkId`),
0 znamená „žiadna". Klient si drží mapu `NetId` → lokálna entita.

Entita, ktorá v snapshote chýba, pre klienta prestala existovať: buď zanikla,
alebo odišla z dosahu. Klient ju zmaže a pri návrate si ju vytvorí znova.

## Čo zatiaľ chýba

- **Predikcia a interpolácia.** Klient ukazuje presne posledný snapshot, takže
  pohyb je oneskorený o cestu po sieti a pri strate paketu trhne. Pole
  „posledný spracovaný vstup" je pripravené pre predikciu.
- **Delta kompresia.** Posiela sa vždy celý stav okolia, čo je 32 bajtov na
  entitu 30× za sekundu. Pri stovkách entít nablízku to bude priveľa.
- **Šifrovanie a prihlásenie.** Meno hráča si klient zvolí sám a nič ho
  neoveruje. Server preto zatiaľ nepúšťaj do verejného internetu.
- **IPv6.** ENet 1.3 vie len IPv4.
