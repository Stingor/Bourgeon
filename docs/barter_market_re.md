# Le barter market (troc) — relevé complet des quatre fenêtres

> Relevé du **2026-09-01** sur le client `2025-07-16_Ragexe` (IDA, base
> 0x400000) et sur le dépôt serveur `moonlight` (fork rAthena, `PACKETVER
> 20250716`).
>
> Sujet **jamais touché par Bourgeon** : aucune ligne de `src/` ne cite ces
> fenêtres, hormis la garde de la banque
> ([bank_window.cc:70](../src/features/windows/bank_window.cc#L70)) qui rejoue
> le blocage du barter étendu. Il était classé « mort » dans
> [unexplored_systems.md](unexplored_systems.md) §3 sur le seul critère
> `feature.barter: off`. **Le verdict tient, et §7 le prouve deux fois plutôt
> qu'une.**

## 1. Ce que c'est

Un *barter* est une boutique de PNJ qui se paie **en objets** au lieu de zeny.
Le client en sert **deux générations**, chacune faite d'une paire de fenêtres —
une **liste** et un **panier** — qui naissent et meurent ensemble :

| génération | liste | panier | classe de base | ZC d'ouverture | CZ d'achat | CZ de fermeture |
|---|---|---|---|---|---|---|
| simple | **334** `UIBartermarketWnd` | **335** `UIBarterItemPurchaseWnd` | `UIItemShopWnd` | `0x0B78` (anc. `0x0B0E`) | `0x0B0F` | `0x0B12` |
| étendu | **341** `UIExpandedBartermarketWnd` | **342** `UIExpandedBarterItemPurchaseWnd` | `UIWindow` | `0x0B79` (anc. `0x0B56`) | `0x0B57` | `0x0B58` |

L'étendu ajoute ce que le simple ne sait pas porter : un **prix en zeny en plus
du troc**, un **raffinement** exigé ou offert, et **plusieurs devises** pour un
même article (jusqu'à `MAX_BARTER_REQUIREMENTS` = 6 côté serveur). C'est
exactement le critère qu'utilise `BarterDatabase::loadingFinished()`
(`moonlight/src/map/npc.cpp:747-800`) pour décider, article par article, si un
PNJ bascule en étendu.

## 2. Les quatre fenêtres

| id | classe | vtable | ctor | slot du `UIWindowMgr` | taille objet |
|---|---|---|---|---|---|
| 334 | `UIBartermarketWnd` | `0x01027A9C` | `0x008088A0` | `+0x4F188` | 0x110 |
| 335 | `UIBarterItemPurchaseWnd` | `0x01027C50` | `0x00808840` | `+0x4F18C` | 0x104 |
| 341 | `UIExpandedBartermarketWnd` | `0x01027DD8` | `0x0080D410` | `+0x4F190` | 0xD4 |
| 342 | `UIExpandedBarterItemPurchaseWnd` | `0x01027F8C` | `0x0080D2F0` | `+0x4F194` | 0xDC |

Les slots virtuels utiles (cf.
[native_window_dispatch.md](native_window_dispatch.md) : `+0x3C` OnCreate,
`+0x50` OnPaint, `+0x94` OnMsg) :

| id | OnCreate | OnPaint | OnMsg | OnLButtonDown | OnMouseStay |
|---|---|---|---|---|---|
| 334 | `0x0080ACA0` | `0x0080B320` | `0x0080CDF0` | `0x0080B9A0` | `0x0080BDE0` |
| 335 | `0x0080A140` | `0x0080AE10` | `0x0080C6D0` | `0x0080B8C0` | `0x0080BB50` |
| 341 | `0x0080F9A0` | `0x008101B0` | **`0x00812220`** | `0x00810A30` | `0x00811290` |
| 342 | `0x0080F6C0` | `0x0080FB00` | **`0x00811920`** | `0x008108F0` | `0x00810BB0` |

### 🔴 Deux fonctions étaient MAL NOMMÉES dans l'IDB

`0x00812220` s'appelait `UIShopSellWnd_OnMsg` et `0x00811920`
`UIShopBuyWnd_OnMsg`. Ce sont les `OnMsg` des fenêtres **341** et **342**.
Preuve, pour chacune : son **unique** xref est le slot `+0x94` de la vtable
barter (`0x01027E6C` et `0x01028020`), et `0x00812220` fait
`FindWindow(342)`. Aucune autre fonction du binaire ne porte un nom
`UIShopBuy*` / `UIShopSell*`, donc rien d'autre ne réclamait ces noms.
**Renommées et commentées dans l'IDB** le 2026-09-01.

Conséquence pour qui lit l'IDB : chercher « la fenêtre de vente marchand » sur
ce nom menait droit dans le barter étendu.

## 3. Les deux gestionnaires d'articles

| singleton | adresse | `CreateInstance` | sert |
|---|---|---|---|
| `g_BarterMarketItemMgr` | `0x0125177C` | `0x005B5CF0` | 334 + 335 |
| `g_BarterMarketItem2Mgr` | `0x01251788` | `0x005B7E40` | 341 + 342 |

Chacun tient **deux** listes : le **stock** de la boutique et le **panier** du
joueur. `CBarterMarketItemMgr_MoveToCart` (`0x005B5F50`) déplace de l'une à
l'autre : il cherche l'article par son index de boutique, ajoute la quantité au
panier, **décrémente le stock**, et quand le stock tombe à zéro retire la ligne
puis envoie `msg 23` à la fenêtre 334 pour la faire redessiner.

Les xrefs aux deux singletons sont **bornées et complètes** (37 et 31), et elles
nomment tout le sous-système d'un coup : les deux `CreateInstance`, les quatre
`OnMsg`, les `OnPaint`, l'`OnLButtonDown` de 334, les handlers ZC, les émetteurs
CZ, et **`UIItemDropCntWnd_OnMsg` (`0x009509A0`)** — la boîte « combien ? »
partagée, qui connaît les deux managers.

## 4. L'ouverture — c'est le paquet, et rien d'autre

`Recv_ZC_NPC_BARTER_MARKET_ITEMINFO_0B78` (`0x00CB6C20`, case 2936) :

1. vide `g_BarterMarketItemMgr` ;
2. relit `(packetLength - 4) / 31` entrées ;
3. `MakeWindow(334)` **et** `MakeWindow(335)`, puis `msg 23` à chacune.

`Recv_ZC_NPC_EXPANDED_BARTER_MARKET_ITEMINFO_0B79` (`0x00CBE3E0`, case 2937)
fait de même avec `MakeWindow(341)` + `MakeWindow(342)`.

🔴 **Il n'y a aucun autre ouvreur.** L'énumération de tous les `push 14Eh` /
`14Fh` / `155h` / `156h` du segment `.text` (33 / 24 / 14 / 27 occurrences,
toutes classées par l'instruction suivante) ne rend, pour le `UIWindowMgr`, que :
les deux handlers ci-dessus, la table de `UIWindowMgr_MakeWindow` elle-même, les
`OnMsg` des fenêtres entre elles, `UIItemDropCntWnd_OnMsg`, le routeur de
fermeture `UIWindowMgr_SaveRectAndCloseWindow`, et le bloc d'annulation du
`CMode::SendMsg`. **Ni raccourci clavier, ni commande de chat, ni bouton d'une
autre fenêtre.** Pas de paquet ⇒ pas de fenêtre, jamais.

⚠ Les autres occurrences de ces immédiats sont des **homonymes** : `0x14E` est
aussi l'effet 334 (`Effect_SpawnPrimitiveById`) et l'opcode 0x14E de
`PacketLenTable_Init`. C'est l'instruction suivante (`mov ecx, offset
dword_131F4E8`) qui distingue un id de fenêtre.

La variante ancienne `0x0B0E` (case 2830) est traitée **en ligne** dans
`RecvLoop_DispatchPackets` à `0x00CA1331`, stride **25** octets. `0x0B56` a sa
propre fonction, `0x00CBE710`. Aucune des deux n'est atteignable depuis
Moonlight, dont le `PACKETVER` sélectionne les variantes modernes.

## 5. Le protocole, champ par champ — client et serveur concordent

Les tailles d'entrée lues par le client ont été relevées dans le
désassemblage (division par le stride côté réception, calcul de longueur côté
émission) puis confrontées à `moonlight/src/map/packets_struct.hpp` et
`packets.hpp`. **Les quatre concordent exactement.**

### ZC `0x0B78` — liste du barter simple · entrée **31 o**

`nameid`4 · `type`1 · `amount`4 · `currencyNameid`4 · `currencyAmount`4 ·
`weight`4 · `index`4 · `viewSprite`2 · `location`4

(La variante `0x0B0E` fait **25 o** : sans `viewSprite` ni `location`.)

### ZC `0x0B79` — liste du barter étendu · en-tête 8 o, entrée **36 o**

En-tête : `packetType`2 · `packetLength`2 · `items_count`4.
Entrée : `nameid`4 · `type`2 · `amount`4 · `weight`4 · `index`4 · `zeny`4 ·
`viewSprite`2 · `location`4 · `currency_count`4 · `refine_level`4,
suivie de `currency_count` **devises de 12 o** : `nameid`4 · `refine_level`2 ·
`amount`4 · `type`2.

⚠ Le `refine_level` de l'article n'existe que dans la branche
`PACKETVER_MAIN_NUM >= 20250402` de rAthena — la variante 2021 fait 32 o.
Le client 2025-07-16 lit **36**, donc il faut bien la branche 2025 : un serveur
compilé avec un `PACKETVER` plus ancien décalerait toute la liste.

### CZ `0x0B0F` — achat simple · entrée **14 o**

`itemId`4 · `amount`4 · `invIndex`2 · `shopIndex`4.

Sérialisé par `CMode::SendMsg` **case 308** (`0x00C87BCA`) à partir du panier.
Détail utile : `itemId` est obtenu par **`atoi` sur le nom stocké dans
l'entrée** (le manager range l'id d'objet sous forme de `std::string`, écrite
par `_itoa` à la réception), et `invIndex` est rendu par `sub_D5A7A0` depuis le
champ `+0x2C` de l'entrée. Le paquet est **refusé au-delà de 0x800 octets**,
soit 146 lignes de panier.

### CZ `0x0B57` — achat étendu · entrée **12 o**

`itemId`4 · `shopIndex`4 · `amount`4. `CMode::SendMsg` **case 321**
(`0x00C87DF9`), même plafond de 0x800 octets.

### CZ `0x0B12` / `0x0B58` — fermeture · 2 o

`CMode::SendMsg` **case 309** (`0x00C87DD1`) et **case 322** (`0x00C8806F`).

## 6. Le parcours du joueur, et ce qui le garde

Les deux fenêtres d'une paire sont **ancrées l'une à l'autre** (`msg 34` :
chacune mémorise l'autre et se repositionne dessus). La liste occupe la gauche,
le panier la droite.

1. **Clic sur une ligne** de 334 (`OnLButtonDown` `0x0080B9A0`) : grille de
   32 px, colonnes en `this+0xE0`, lignes en `this+0xDC` ; retient l'index
   cliqué en `this+0x100` et déclenche l'action de mode 31 — la boîte
   « combien ? » (`UIItemDropCntWnd`, fenêtre 15).
2. La quantité saisie atterrit dans **`CGameMode+0x318`**, l'article visé dans
   **`CGameMode+0x3B0`**.
3. **`msg 38` sur 335** = « mets-le au panier », derrière **six gardes**,
   chacune suivie d'un `CMode::SendMsg(18)` qui remet `+0x308`, `+0x30C` et
   `+0x318` à zéro et referme la boîte de quantité :

   | garde | message |
   |---|---|
   | article absent / quantité nulle | **3554** `MSI_EXCHANGED_FAILED` |
   | objet actuellement équipé | **3665** `MSI_CANNOT_EXCHANGE_ITEMS` |
   | équipement en exemplaire unique | **119** `MSI_EQUIPITEM_OLNY_ONE` |
   | article vendu à l'unité | **3569** `MSI_BUY_ONE_AT_A_TIME` |
   | quantité cumulée > 30000 | **1704** `MSI_LIMIT_BUY_ITEM2` (le 30000 est le `%d`) |
   | capacité de poids insuffisante | **56** `MSI_OVER_WEIGHT` |

   ⚠ La capacité restante est calculée `[0x015FBA9C] - [0x015FBAA0]`, moins le
   poids déjà au panier, divisée par le poids unitaire. Cette paire de globales
   **se lit** comme poids max / poids courant ; ce n'est pas vérifié ailleurs et
   ce document ne l'affirme pas.

4. **Bouton 184 « Acheter »** (`msg 6` sur 335) : revalide via `sub_808C70`
   puis `CMode::SendMsg(308)` ⇒ **CZ `0x0B0F` part**. Enchaîne sur
   `SendMsg(40)`.
5. **Bouton 185 « Annuler »** : `SendMsg(40)` et **vidage du manager**, sans
   paquet.

Les autres messages textuels du sous-système : **3553**
`MSI_BARTERITEM_NAME_COUNT` (`"%s %d ea"`, le libellé d'une ligne), **3689**
`MSI_BARTER_ITEMLIST` (le titre « Objets à échanger »), **3688**
`MSI_EQUIP_FAIL_DURING_BARTER`, **3556** `MSI_NO_ITEM_FOR_EXCHANGE`, **3557**
`MSI_ITEM_OUT_OF_STOCK`, **3555** `MSI_EXCHANGED_SUCCEEDED`. Tous déjà traduits
en fr et es (cf. [msgstring.fr.yaml](../tools/lang/msgstring.fr.yaml)).

Redimensionnement (`msg 14`) : **334** est bornée 232..320 × 120..336 par pas de
32 ; **341** est bornée 366..439 × 366..549 par pas de **64**.

## 7. 🔴 L'asymétrie de fermeture — la paire simple ne cascade pas

Relevé sur les quatre `case` du routeur `UIWindowMgr_SaveRectAndCloseWindow`
(switch `0x00A2EA4C`) — pour mémoire, ce routeur **détruit** la fenêtre, il ne
fait pas que sauver son rectangle :

| case | ce qu'il fait |
|---|---|
| **334** (`0x00A2EF76`) | détruit 334. **Ni cascade vers 335, ni paquet.** |
| **335** (`0x00A2EF9D`) | `SendMsg(0x135=309)` ⇒ **CZ `0x0B12`**, détruit 335. Ne touche pas 334. |
| **341** (`0x00A2EFE6`) | détruit 341 **puis ferme 342**. |
| **342** (`0x00A2F026`) | `SendMsg(0x142=322)` ⇒ **CZ `0x0B58`**, détruit 342 **puis ferme 341**. |

La paire **étendue** se referme donc en cascade dans les deux sens ; la paire
**simple** non. Fermer 334 seule laisse 335 orpheline à l'écran, et n'envoie
jamais `CZ_NPC_BARTER_MARKET_CLOSE` — or côté serveur `clif_barter_open`
(`clif.cpp:28823`) **refuse de rouvrir** tant que `sd.state.barter_open` est
armé, et seul `clif_parse_barter_close` le désarme.

⚠ Le chemin d'annulation global, `CMode::SendMsg` **case 40** (`0x00C8760F`),
ferme bien les quatre dans l'ordre 334, 335, 341, 342 — il passe donc par les
cases 335 et 342 et **émet les deux CZ**. C'est le seul chemin propre.

⚠⚠ **Non mesuré en jeu**, et impossible à mesurer sur Moonlight (§8) : c'est une
lecture du routeur de fermeture, pas une observation. À vérifier au débogueur si
le sujet est un jour réactivé.

## 8. Côté Moonlight : mort, et deux fois plutôt qu'une

Le classement « mort » de [unexplored_systems.md](unexplored_systems.md)
reposait sur la configuration seule. Il y a en fait **deux** verrous
indépendants, et un troisième indice qui va dans le même sens :

1. **Configuration.** `conf/import/battle_conf.txt:590-591` pose
   `feature.barter: off` **et** `feature.barter_extended: off`, écrasant les
   `on` de `conf/battle/feature.conf:126,130`. `BarterDatabase::loadingFinished`
   sort alors immédiatement sur `ShowError("Barter system is not enabled.")`,
   avant même de créer le moindre PNJ.
2. **Base de données vide.** Moonlight a **redirigé** l'emplacement du fichier :
   `BarterDatabase::getDefaultLocation()` (`src/map/npc.cpp:395-397`) rend
   `"moon/barters.yml"` au lieu du chemin rAthena. Or `moon/barters.yml` fait
   47 lignes dont 43 de licence et de commentaires : un `Header:` (`BARTER_DB`,
   version 2) et **aucun `Body:`**. **Zéro PNJ barter déclaré.** Même en
   rallumant les deux `feature.*`, la boucle de `loadingFinished` n'aurait rien
   à parcourir — et donc pas même le `ShowError` du point 1.
3. Les catalogues rAthena qui, eux, contiennent des barters
   (`npc/re/merchants/barters/*.yml`, 9 fichiers) sont sous `re/` : Moonlight
   est **pre-renewal** (`src/config/renewal.hpp:8` : `#define PRERE`).

⚠ Un signe que l'intention n'est pas totalement absente : la commande maison
**`@shopsearch`** (`src/custom/atcommand.inc:2336-2360`) sait déjà lire un
`NPCTYPE_BARTER` — elle va chercher les articles dans `barter_db` indexé par
`exname`, et affiche « troc » ou « troc + Nz ». Ce code ne peut aujourd'hui
jamais s'exécuter.

**Verdict inchangé : ne pas ouvrir ce chantier.** Le remettre en marche
demanderait trois gestes serveur (deux `feature.*`, un `moon/barters.yml`
peuplé, des PNJ) avant qu'une seule ligne de client serve à quelque chose.

## 9. Ce qui reste non relevé

- `UIBarterItemPurchaseWnd_OnCreate` (`0x0080A140`, 0xB5A octets — de loin la
  plus grosse des quatre) : la disposition exacte et les ids de commande autres
  que 184 / 185 n'ont pas été dépouillés.
- Les fonctions `0x0080D930` (0x714 o) et `0x00810EA0` (0x3E6 o) touchent
  `g_BarterMarketItem2Mgr` sans être dans une vtable : ce sont probablement le
  constructeur de liste et le rendu de ligne de la paire étendue. Non ouvertes.
- `sub_808C70` (`0x00808C70`, 0x5E5 o) : la validation finale avant l'envoi de
  `0x0B0F`. Son verdict est utilisé, son contenu n'a pas été lu.
- La paire de globales `0x015FBA9C` / `0x015FBAA0` (§6, garde de poids).
- L'action de mode **31**, déclenchée au clic sur une ligne, est supposée ouvrir
  `UIItemDropCntWnd` sur la foi des xrefs de cette fenêtre aux deux managers.
  **Non vérifié dans le dispatcher.**
