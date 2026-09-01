# Les 24 sections d'état du fichier de rejeu — et une correction

> Relevé du **2026-09-01**. Client `2025-07-16_Ragexe` (IDA, base 0x400000).
>
> Trouvé en tirant le fil laissé ouvert par
> [memorial_dungeon_re.md](memorial_dungeon_re.md) §7 : « un blob TLV de magie
> 5500, origine non établie, piste = la reconnexion en instance ». **La piste
> était fausse** : c'est le fichier de **rejeu**.

## 1. Le mécanisme

Un `.brw` ne contient pas que des paquets. Il porte aussi une **table de 26
sections d'état** (`ReplayFile_FindSectionSlot` 0x00b1c880 : ids valides
**1..0x19**, table à `mgr+105`, pas de 5 mots), lues une par une au démarrage
d'une partie :

```c
ReplayFile_ReadStateSection(mgr, &buf, id)     // 0x00b1ce10
  → si offset_de_section == 0 : ne lit RIEN, buf sort VIDE
  → sinon : fseek(FILE* mgr[119], offset) + fread(taille)
```

Puis `CGameMode_EnterWorld` 0x00c733d0 enchaîne **22 restaurations**, chacune
avec son analyseur, et `CLoginMode_OnStateEnter` en fait une de plus.

### Où vit la table

C'est **l'index de sections du fichier `.brw`** : `mgr+0xD2` (= `mgr+210`),
**0x104 octets**, écrit à l'**offset 112** du fichier par
`ReplayRecorder_WriteHeaderOrFlush` 0x00b1dcc0 (modes 12 et 13 :
`fseek(f, 112, 0); fwrite(this+210, 0x104, 1, f)`), et relu à l'ouverture d'un
rejeu. Chaque entrée fait **10 octets** : id en `+0`, taille en `+4`, offset
dans le fichier en `+8` (`this + 10*slot`, base 210).

🔴 **Dans une session ordinaire — aucun rejeu lu — cet index est à zéro** : la
garde `if (offset_de_section)` de `ReplayFile_ReadStateSection` ne lit rien,
chaque tampon sort vide et **les 24 consommateurs sortent immédiatement**.
C'est ce qui rend le point §3 vrai.

⚠ **Cas non testé** : le comportement pendant l'**enregistrement** d'un rejeu
(mode 2), où l'index se remplit au fil de l'écriture. Le `FILE*` y est ouvert en
écriture, donc un `fread` dessus ne devrait rien rendre d'exploitable — mais ce
n'est pas mesuré, seulement raisonné.

## 2. La carte des sections

Chaque section porte une **magie** en tête et démultiplexe des étiquettes qui lui
sont propres (le donjon mémorial : magie 5500, étiquettes 5501/5510…5518). Les
sujets ci-dessous sont établis par les globales de session écrites, les fonctions
nommées appelées, ou l'opcode des handlers réutilisés — pas par le numéro.

| id | consommateur | sujet | comment il est établi |
|---:|---|---|---|
| 2 | `sub_B1D320` | interne au gestionnaire de rejeu | étiquettes 951..968 |
| 3 | `sub_DA5150` *(depuis `CLoginMode_OnStateEnter`)* | **apparence et état du personnage** | écrit `g_OwnLook_HairStyleId`, `…_WeaponViewId`, `…_ClothesColorId`, `g_OwnState_BodyState` — 92 étiquettes |
| 4 | `sub_DA5EC0` | **constantes vitales + homoncule** | écrit `g_Own_Hp/MaxHp/Sp/MaxSp`, appelle `Homun_NotifySkillWnd` |
| 6 | `sub_DA7F70` | **quêtes** | référence `g_QuestTrackerWnd` |
| 7 | `sub_DA6490` | **groupe** | écrit `g_Own_InParty` |
| 8 | `sub_DA6790` | **inventaire et listes d'objets** | premier offset `session+0x16F0` = `kInvListHead` (`std::list<ItemSkillInfo>`), déjà connu du dépôt |
| 9 | `sub_DA7290` | **familier** | écrit `g_Own_PetAid`, `…PetName`, `…PetHungry`, `…PetIntimacy` — 78 étiquettes |
| **10** | **`MemorialDun_RestoreFromReplaySection`** 0x00da6f40 | 🔴 **donjon mémorial** | magie 5500 ; écrit `g_MemorialDunState` (`session+0x5928`) ; **rouvre la fenêtre 137** |
| 11 | `sub_DA7280` | — | **`return 0`** : section réservée, jamais implémentée |
| **12** | **`EntryQueue_RestoreFromReplaySection`** 0x00da4d60 | **file d'attente de battleground** | étiquettes 6501+ ; **touche les fenêtres 157 et 209** |
| 13 | `sub_DA8120` | **raccourcis clavier** | magie 7000 ; écrit `session+0x490` = `_Dst_015fa850`, la table que parcourt `UserHotkey_ResolveBehavior` |
| 14 | `sub_DA6C30` | *non identifié* | partage ses aides avec les handlers **ZC `0x0BBB` / `0x0BBC`**, que rAthena n'envoie pas |
| 15 | `sub_C818B0` | **acteurs du monde** | appelle `GameMode_OnRecv_ActorSpawn_Named` |
| 16 | `sub_C816E0` | **objets au sol** | appelle `CItem_Ctor`, `SceneNodeList_PushBack`, `ActorMgr_FindByAid` |
| 17 | `sub_C81E50` | **unités de sol** (pièges, murs, portails) | réutilise les handlers **ZC `0x011F`** et **`0x01C9`** |
| 18 | *analysé **en ligne** dans `EnterWorld`* | ? | pas de fonction dédiée (`0x00c7356c`) |
| **19** | **`UIWindowMgr_RestoreWindowLayout`** 0x00a4a930 | 🔴🔴 **disposition des fenêtres** | voir §3 |
| 20 | `sub_A4A740` | gestionnaire de fenêtres (voisine de 19) | même module `0x00A4Axxx` |
| 21 | `sub_A4A850` | gestionnaire de fenêtres (voisine de 19) | même module |
| 22 | `sub_C81360` | **emblèmes de guilde** | appelle `Emblem_SaveToTempFile` ; étiquettes 13001..13005 |
| 23 | *analysé **en ligne** dans `EnterWorld`* | ? | pas de fonction dédiée (`0x00c7431b`) |
| 24 | `sub_B33430` | **navigation** | appelle `CNavigation_ClearRoute`, `CNavigation_RefreshOnMapEnter` ; **touche la fenêtre 203** |

Les analyseurs `0x00DAxxxx` forment un module homogène : ce sont les
restaurateurs d'état par sous-système. Ceux en `0x00C81xxx` touchent au monde
(acteurs, objets au sol, unités de sol, emblèmes).

### 🔴 Les quatre sections qui OUVRENT ou FERMENT des fenêtres

C'est la seule partie qui concerne directement un module Bourgeon : pendant la
lecture d'un `.brw`, ces sections font naître des fenêtres **sans aucun geste du
joueur** — **10** (fenêtre 137), **12** (157 et 209), **24** (203), et **19 +
20/21** (la disposition entière).

## 3. 🔴🔴 Correction : `RestoreWindowLayout` ne tourne QU'EN REJEU

`docs/` et la mémoire du projet portaient jusqu'ici que le client rouvre ses
fenêtres à chaque carte par **deux** chemins, dont
« `RestoreWindowLayout` 0x00a4a930 à `EnterWorld`, qui échappe à la barrière
`IsMapLoading()` ⇒ il faut le hooker ».

**L'adresse et le site d'appel sont justes. La conclusion ne l'est pas.**

- La fonction n'a **qu'un seul xref** dans tout le binaire : `0x00C744B5`, dans
  `CGameMode_EnterWorld`.
- Ce site lui passe la **section 19 du fichier de rejeu**, pas une disposition
  courante.
- Son premier geste est de rejeter un tampon vide :

```asm
00A4A95D  mov  esi, [ebp+arg_0]     ; la paire begin/end
00A4A960  mov  eax, [esi]
00A4A962  cmp  eax, [esi+4]
00A4A965  jnz  short loc_A4A97E     ; non vide -> travaille
00A4A967  or   eax, 0FFFFFFFFh      ; VIDE -> renvoie -1
00A4A97B  retn 4
```

➡ **En jeu normal elle ne fait rien du tout.** Le seul chemin vivant de
réouverture des fenêtres reste le premier : la reconstruction du HUD, celle qui
se garde avec `Bourgeon::IsMapLoading()`.

⚠ La fiche mémoire portait déjà, en tête, la leçon exacte qui la contredit :
« un chemin qui EXISTE n'est pas celui qu'on emprunte ». Elle avait été écrite
pour le chemin *manqué* ; elle valait aussi pour le chemin *trouvé*. **Un
appelant ne prouve pas un usage : il faut lire ce qu'on lui passe.**

## 4. Ce que ça change en pratique

- **Ne pas hooker `RestoreWindowLayout`** en croyant intercepter la réouverture
  des fenêtres : le hook ne se déclencherait qu'en lecture de `.brw`.
- Les fenêtres **137** (donjon mémorial) et **157** (file de battleground)
  peuvent s'ouvrir *toutes seules* pendant la lecture d'un rejeu, par les
  sections 10 et 12. Tout module Bourgeon qui suppose « cette fenêtre ne naît
  que sur action du joueur » doit en tenir compte — c'est le même piège que le
  barrage anti-macro décrit dans
  [native_window_dispatch.md](native_window_dispatch.md) §3.
- La vraie réouverture de la fenêtre 137 à chaque carte se fait ailleurs, dans
  `GameMode_OnEnterMapSetup` 0x00c6b870, **conditionnée par
  `g_MemorialDunShowFlag`** (0x015FFCDC) — cf.
  [memorial_dungeon_re.md](memorial_dungeon_re.md) §4.

## 5. Ce qui reste à faire

Trois sections seulement résistent : **14** (alimentée par les ZC `0x0BBB` /
`0x0BBC`, absents de rAthena — donc muette sur Moonlight de toute façon), et
**18** / **23**, analysées en ligne dans `EnterWorld` sans fonction dédiée.
Les sections **20** et **21** sont identifiées par module (gestionnaire de
fenêtres) mais pas par contenu.

## 6. Ce que cette liste vaut, au-delà du format

Les 24 sections sont **l'inventaire de ce que le client considère comme son
état** — celui qu'il faut capturer pour qu'un rejeu reparte juste. C'est donc
une carte des sous-systèmes à état, écrite par Gravity : apparence du
personnage, constantes vitales, homoncule, familier, groupe, inventaire,
quêtes, raccourcis, acteurs, objets et unités au sol, emblèmes, navigation,
disposition des fenêtres, donjon mémorial, file de battleground.

Deux enseignements s'en dégagent :

- **La section 11 est un `return 0`.** Gravity a réservé un numéro pour un
  sous-système puis n'a jamais écrit son analyseur — utile à savoir avant de
  chercher longtemps ce qu'elle capture.
- **La section 13 capture les raccourcis clavier**, ce qui recoupe par un
  chemin entièrement indépendant le relevé de `hotkey_v2.lub` : la table
  `session+0x490` est celle que `UserHotkey_ResolveBehavior` parcourt quand la
  résolution Lua échoue.
