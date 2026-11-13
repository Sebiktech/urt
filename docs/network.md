# Sieťový protokol URT (verzia 3)

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
| 2 | `ServerWelcome` | server → klient | 0 | `u32` id hráča, `u16` tick rate, `u32` tick servera, `f32` rýchlosť chôdze |
| 3 | `ServerReject` | server → klient | 0 | `u8` dôvod: 1 verzia, 2 plný server, 3 zlé meno |
| 4 | `ClientInput` | klient → server | 1 | `u32` poradie posledného príkazu, `u8` počet (1 až 8), príkazy |
| 5 | `ServerSnapshot` | server → klient | 1 | `u32` tick, `u32` posledný spracovaný vstup, `u16` počet, entity |
| 6 | `ServerResourceStart` | server → klient | 0 | `string` meno resource, `u8` počet skriptov, skripty |
| 7 | `ServerResourceStop` | server → klient | 0 | `string` meno resource |
| 8 | `NetEvent` | oboma smermi | 0 | `string` meno eventu, `blob` argumenty |

`blob` je `u32` dĺžka a potom bajty. Skript v `ServerResourceStart` je
`string` meno (pre chybové hlášky) a `blob` so zdrojovým textom, najviac 1 MB;
resource má najviac 64 skriptov klienta.

Príkaz v `ClientInput` má 12 bajtov: `f32` move_x, `f32` move_z, `f32` yaw.
Príkazy idú od najstaršieho po najnovší.

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

1. `net::Server` prečíta pakety, prijme nových hráčov a každému hráčovi vezme
   z fronty jeden príkaz.
2. Simulácia (`MovementSystem` a ďalšie systémy) pohne svetom.
3. `net::ServerSnapshotSystem` pošle každému hráčovi entity z jeho okolia.

Systémy treba do `Application` pridať v tomto poradí.

## Vstupy: jeden príkaz na tick

Klient každý tick vytvorí jeden príkaz (smer a natočenie) s ďalším poradovým
číslom. Paket nesie posledné štyri príkazy, takže stratený paket nič nestratí:
ďalší jeho príkazy zopakuje. Server podľa poradia zahodí tie, ktoré už má.

Server si príkazy každého hráča ukladá do fronty a **každý tick z nej vezme
práve jeden**:

- `move_x` a `move_z` oreže na −1 až 1 a vektor skráti na dĺžku najviac 1.
- Rýchlosť je smer × rýchlosť chôdze (4 m/s) a platí jeden tick.
- Prázdna fronta znamená, že hráč ten tick stojí.
- Vo fronte je najviac 8 príkazov; pri pretečení sa zahodí najstarší.

Z toho plynú dve veci. Rýchlosť hráča určuje hodinový takt servera, nie počet
paketov: klient, ktorý posiela príkazy päťkrát rýchlejšie, si len zaplní frontu
a prebytok sa zahodí. A poloha po príkaze N závisí len od príkazov 1 až N, čo
je presne to, čo potrebuje predikcia.

Klient preto musí tikať rovnako rýchlo ako server. Tick rate sa dozvie zo
`ServerWelcome` a prepne sa naň.

## Predikcia vlastnej postavy

Keby klient čakal na server, postava by sa pohla až po ceste paketu tam a
späť. Namiesto toho:

1. **Predikcia.** Klient príkaz hneď aplikuje na vlastnú postavu rovnakým
   kódom, aký používa server (`net/movement.hpp`), a zapamätá si ho ako
   nepotvrdený.
2. **Potvrdenie.** Snapshot hovorí: „po tvojom príkaze číslo N si bol tu."
3. **Zmierenie.** Klient zahodí príkazy do N, vezme potvrdenú polohu a
   nepotvrdené príkazy na ňu aplikuje znova. Ak vyjde to isté, čo predpovedal,
   nič sa nestane, a to je bežný prípad, lebo obe strany počítajú rovnako.
4. **Korekcia.** Ak sa výsledok líši (stratený príkaz, zásah skriptu na
   serveri), rozdiel do 3 m sa nevykreslí naraz, ale rozplynie sa počas
   niekoľkých tickov. Väčší rozdiel je teleport a ukáže sa okamžite.

Ostatné entity sa nepredpovedajú. Kreslia sa 0,1 s v minulosti a ich poloha sa
dopočítava medzi dvoma snapshotmi.

Predikcia zatiaľ pokrýva len chôdzu. Všetko, čo neskôr ovplyvní pohyb hráča
(kolízie, fyzika, vozidlá), musí bežať rovnakým kódom na oboch stranách, inak
bude server klienta neustále opravovať.

## Skripty a eventy

Po `ServerWelcome` pošle server klientovi `ServerResourceStart` za každý
bežiaci resource, ktorý má skripty klienta. Rovnakú správu pošle všetkým pri
štarte alebo reloade resource; klient vtedy starú verziu zastaví a spustí
novú. `ServerResourceStop` príde, keď sa resource na serveri zastaví.

`NetEvent` nesie event medzi skriptami. Sieťová vrstva overí meno (1 až 64
tlačiteľných znakov bez medzier) a veľkosť argumentov (najviac 16 KB); obsah
argumentov dekóduje až skriptová vrstva. Formát argumentov: `u8` počet, potom
hodnoty, každá začína bajtom typu:

| Typ | Hodnota | Nasleduje |
|---|---|---|
| 0 | `nil` | nič |
| 1, 2 | `false`, `true` | nič |
| 3 | číslo | `f64` |
| 4 | reťazec | `u16` dĺžka, bajty |
| 5 | tabuľka | `u16` počet párov, potom kľúč a hodnota za každý pár |

Kľúč tabuľky je číslo alebo reťazec. Dekodér najprv overí celé kódovanie
vrátane limitov (hĺbka 8, 2048 hodnôt, reťazec 8 KB) a až potom vytvára
hodnoty, takže poškodené dáta po sebe nič nezanechajú. Event s neplatnými
argumentmi sa handleru nedoručí.

Server prijme od klienta najviac 30 eventov za sekundu so zásobou 60. Ďalšie
zahodí; po 300 zahodených klienta odpojí.

Klient serveru neverí o nič viac než server klientovi: správy, ktoré sa
nedajú dekódovať alebo prekračujú limity, ignoruje, a skripty spúšťa
v sandboxe s limitom času a pamäte.

## Čo server považuje za porušenie protokolu

Spojenie sa zavrie a hráč sa okamžite odstráni zo sveta, keď príde:

- neznámy typ správy alebo správa určená klientovi,
- `ClientHello` na nesprávnom kanáli, poškodený alebo druhýkrát,
- `ClientInput` pred dokončením handshaku alebo na nesprávnom kanáli,
- `NetEvent` pred dokončením handshaku, na nesprávnom kanáli alebo taký, ktorý
  sa nedá dekódovať.

Poškodený `ClientInput` sa len zahodí, lebo nespoľahlivý paket sa môže
poškodiť aj bez zlého úmyslu. Jeho príkazy zopakuje ďalší paket.

## Sieťové id

Lokálne id entity v ECS je na každom stroji iné, preto sa po sieti neposiela.
Server prideľuje každej replikovanej entite `NetId` (komponent `NetworkId`),
0 znamená „žiadna". Klient si drží mapu `NetId` → lokálna entita.

Entita, ktorá v snapshote chýba, pre klienta prestala existovať: buď zanikla,
alebo odišla z dosahu. Klient ju zmaže a pri návrate si ju vytvorí znova.

## Čo zatiaľ chýba

- **Delta kompresia.** Posiela sa vždy celý stav okolia, čo je 32 bajtov na
  entitu 30× za sekundu. Pri stovkách entít nablízku to bude priveľa.
- **Cache skriptov.** Skripty klienta sa posielajú celé pri každom pripojení a
  reloade, bez kompresie a bez kontroly, či ich klient už má.
- **Šifrovanie a prihlásenie.** Meno hráča si klient zvolí sám a nič ho
  neoveruje. Server preto zatiaľ nepúšťaj do verejného internetu.
- **IPv6.** ENet 1.3 vie len IPv4.
