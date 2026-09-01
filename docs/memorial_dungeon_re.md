# `UIMemorialDunWnd` (137) — la fenêtre d'instance, relevé complet

> Relevé du **2026-09-01**. Client `2025-07-16_Ragexe` (IDA, base 0x400000),
> serveur `moonlight` (fork rAthena).
>
> Répond à la question laissée ouverte par
> [unexplored_systems.md](unexplored_systems.md) §4 : « le système d'instance
> tourne, mais le lien entre lui et **cette fenêtre-là** n'a pas été établi ».
> Il l'est maintenant, et le verdict est franc : **elle marche de bout en bout.**

## 1. Pourquoi elle compte

C'est la seule des zones vierges qui soit **atteignable par le joueur
aujourd'hui**, sans rien activer :

- **`Alt+B`** — comportement **122** du répartiteur de raccourcis
  (`UIWindowMgr_DispatchHotkeyBehavior` 0x00a451e0, `push 89h` @0x00A458C8) ;
  la fenêtre porte d'ailleurs la chaîne littérale `"Alt+B"` sur son bouton de
  fermeture (`UIWindow_AddCloseButton(this, "Alt+B", 0)` dans `OnCreate`) ;
- **39 instances déclarées** dans `db/import/instance_db.yml` (Endless Tower,
  Old Glast Heim, Faceworm's Nest, Sara's Memories…) et une vingtaine de
  scripts dans `moon/instances/` ;
- `feature.instance_allow_reconnect: yes` dans `conf/import/`.

## 2. Les cinq opcodes — correspondance exacte, champ par champ

Les cinq sont enregistrés dans la table de longueurs du client
(`PacketLenTable_Init`, entrées consécutives 0x00aa2a44…0x00aa2a8c).

| opcode | dir | handler client | fonction rAthena | corps |
|---|---|---|---|---|
| `0x02CB` | ZC | `sub_CC6900` 0x00cc6900 | `clif_instance_create` | `+2` nom[61], **`+63`** position d'attente (W) |
| `0x02CC` | ZC | `sub_CC6970` 0x00cc6970 | `clif_instance_changewait` | `+2` position d'attente (W) |
| `0x02CD` | ZC | `sub_CC6790` 0x00cc6790 | `clif_instance_status` | `+2` nom[61], **`+63`** durée restante (L), **`+67`** délai de fermeture à vide (L) |
| `0x02CE` | ZC | `sub_CC6820` 0x00cc6820 | `clif_instance_changestatus` | `+2` type (L), `+6` limite (L) |
| `0x02CF` | CZ | émis par la fenêtre | `clif_parse_MemorialDungeonCommand` | `+2` commande (L), 6 octets |

Les offsets `+63` / `+67` du client tombent **exactement** sur ceux que le
commentaire de `clif.cpp` annonce (`<Instance name>.61B` puis les deux `.L`).
Aucune divergence de version sur ce bloc.

### Les codes de `0x02CE`, un pour un

| type | rAthena `e_instance_notify` | ce que fait le client |
|---:|---|---|
| 0 | `IN_NOTIFY` | met à jour le délai et rafraîchit |
| 1 | `IN_DESTROY_LIVE_TIMEOUT` | ferme 137 + chat **0x542** `MSI_MEMORIAL_DUN_LIVE_TIME_OUT` |
| 2 | `IN_DESTROY_ENTER_TIMEOUT` | ferme 137 + chat **0x543** `MSI_MEMORIAL_DUN_ENTER_TIME_OUT` |
| 3 | `IN_DESTROY_USER_REQUEST` | ferme 137 + chat **0x544** `MSI_MEMORIAL_DUN_DESTROY_REQUEST` |
| 4 | `IN_CREATE_FAIL` | ferme 137, **sans message** — conforme au commentaire serveur (« removes the instance window ») |

Les noms de `msgstringtable` confirment la lecture sans qu'on ait à la supposer.
Idem pour `0x02CC` : rAthena envoie `0xffff` (`instance.cpp:363` et suivants),
le client le lit en `__int16` donc **-1**, ferme la fenêtre et écrit
**0x541** `MSI_MEMORIAL_DUN_CANCEL`. Le chemin d'annulation est correct.

## 3. La machine à trois états

Une seule variable pilote toute la fenêtre : **`0x015FFCE8`**.

| état | posé par | signification |
|---:|---|---|
| **0** | `0x02CB`, `0x02CC` (position ≥ 0), `sub_8E2E20` (remise à zéro) | **en file d'attente** — la position est affichée |
| **1** | `0x02CD` / `0x02CE` quand le délai de fermeture à vide est **non nul** | instance active, avec minuterie d'inactivité |
| **2** | `0x02CD` / `0x02CE` quand ce délai est **nul** | instance active, sans minuterie |

Calculé partout par la même expression : `état = 2 - (limite2 != 0)`.

Globales associées : `0x015FFCF0` nom de l'instance (`std::string`),
`0x015FFD08` → position d'attente, `0x015FFD0C` durée restante,
`0x015FFD10` délai de fermeture à vide, `0x015FFCDC` drapeau d'affichage.

## 4. L'interface

`ctor` 0x008d73f0 · `OnCreate` 0x008e7180 · `OnPaint` 0x008f25d0 ·
`OnMsg` 0x008fb710 · mise en page `sub_8F9940` 0x008f9940 (appelée à la création
et sur le message **60**, celui que les quatre handlers de paquets envoient).

Trois contrôles, et **c'est tout** — vérifié dans les trois seuls créateurs
(`OnCreate`, `UIWindow_AddCloseButton` 0x00894fa0, `sub_894DF0` 0x00894df0) :

| id | contrôle | effet |
|---:|---|---|
| **184** | bouton bitmap créé par `OnCreate` | ferme la fenêtre 137 |
| **201** | bouton de fermeture, libellé **`"Alt+B"`** | ferme la fenêtre |
| **211** | bouton texte, libellé **0xCC6** `MSI_MEMORIAL_DUN_DESTROY` | 🔴 **détruit l'instance** — voir §5 |
| *(599)* | bouton de `sub_894DF0`, **aucun id explicite** | garde le défaut `0x257` posé par `UIWindow_base_ctor` (`[esi+0x2C] = 257h`) |

Le bouton 211 n'est placé que si `g_Own_InParty` **et** état **1**
(`UIMemorialDunWnd_Layout` : `if (état == 1) SetPos(41, 115)`), sinon il reste
hors écran en (-100, -100).

### 🔴 Le drapeau de réouverture — et pourquoi on n'arrive pas à fermer la fenêtre

`GameMode_OnEnterMapSetup` 0x00c6b870 rouvre la fenêtre **à chaque changement de
carte**, mais seulement si `g_MemorialDunShowFlag` (**0x015FFCDC**) est armé :

```asm
00C6BBBC  cmp  g_MemorialDunShowFlag, 0
00C6BBC3  jz   short loc_C6BBD4
00C6BBC5  push 89h                        ; 137
00C6BBCF  call UIWindowMgr_MakeWindow
```

Or le bouton de fermeture (`OnMsg` case **201**) ne baisse ce drapeau **que si
l'état vaut 2** :

```c
case 201:
  (*(vt+56))(this, 0);                 // masque
  if ( g_MemorialDunState != 2 ) return 0;
  g_MemorialDunShowFlag = 0;
```

➡ **Conséquence observable en jeu** : tant qu'on est **en file d'attente**
(état 0) ou dans une instance **avec minuterie d'inactivité** (état 1), fermer
la fenêtre ne la fait disparaître que jusqu'au prochain changement de carte.
Ce n'est pas un bug de Bourgeon — c'est le natif. Seul l'état 2 permet de s'en
débarrasser.

⚠ C'est aussi le second chemin de réouverture par carte, celui qui est **réel**
— à distinguer de `UIWindowMgr_RestoreWindowLayout`, qui n'agit qu'en rejeu
(cf. [replay_state_sections.md](replay_state_sections.md) §3).

## 5. 🔴 Les deux chemins d'émission, et un seul qui marche

C'est le point le moins évident du relevé : la fenêtre a **deux** façons
d'envoyer `0x02CF`, et elles n'envoient pas la même chose.

### Le bon : bouton 211 → commande 306 → `0x02CF` avec **3**

Le bouton 211 n'envoie pas le paquet lui-même : il appelle
`CGameMode::vtable+0x18` avec l'identifiant **306**. Ce slot est
**`sub_C86740`** 0x00c86740 (vtable `0x010904B8`), un **répartiteur de commandes
1..335** (`cmp eax, 14Eh`, sauts `jpt_C867B4` @0x00c930f0). La commande 306
(0x00c92abc) construit `0x02CF` avec la valeur **3** en dur :

```asm
00C92ABC  mov  eax, 2CFh
00C92AC1  mov  dword ptr [ebp-10592h], 3      ; la commande
```

Or `clif_parse_MemorialDungeonCommand` n'agit que sur
**`COMMAND_MEMORIALDUNGEON_DESTROY_FORCE = 0x3`** (`clif.hpp:756`).
✅ **Correspondance exacte.** Détruire son instance depuis la fenêtre marche.

### Le mort : `case 185` de `OnMsg`

Le même `OnMsg` porte un second émetteur de `0x02CF`, qui calcule la commande
depuis l'état — et **lit sa variable locale avant de l'écrire** :

```asm
008FB7AF  mov   ecx, dword_15FFCE8
008FB7BA  mov   [ebp+Src], ax          ; opcode 0x2CF
008FB7BE  test  ecx, ecx
008FB7C0  jnz   short loc_8FB7C7
008FB7C2  mov   [ebp+var_6], ecx       ; etat 0 -> commande 0
008FB7C5  jmp   short loc_8FB7D7
008FB7C7  mov   eax, [ebp+var_6]       ; <-- LIT var_6, jamais ecrit
008FB7CA  mov   edx, 1
008FB7CF  cmp   ecx, edx
008FB7D1  cmovz eax, edx               ; etat 1 -> 1
008FB7D4  mov   [ebp+var_6], eax       ; etat 2 -> reecrit ce qu'il vient de LIRE
```

Le prologue ne réserve que `sub esp, 8` — `Src` en `ebp-8`, `var_6` en `ebp-6`
— et **rien n'écrit `var_6` avant** ce point : en état 2, les quatre octets de
commande sont de la pile résiduelle. De plus, ni 0 ni 1 ne sont dans le `switch`
du serveur : ce chemin ne pourrait rien faire de bon de toute façon.

⚠ **Mais il est inatteignable, et il faut le dire :** aucun contrôle de cette
fenêtre ne porte l'id **185**. Les deux `SetCommandId` d'`OnCreate` posent 184 et
211, `AddCloseButton` pose 201, et le bouton de `sub_894DF0` garde le défaut
**599**. Le `switch` d'`OnMsg` n'accepte que 184..211 (`add eax,-184` ;
`cmp eax, 1Bh`), donc 599 n'y entre même pas.

➡ **Défaut réel, risque nul en pratique** : `case 185` est du code hérité d'un
gabarit partagé (on retrouve le couple 184/185/201 dans `UIJobListWnd` et
`UISeekPartyMBWnd`). À connaître si l'on synthétise un jour un message 6 sur
cette fenêtre — c'est exactement ce que fait `OnMsg` case 0, qui réémet le
message 6 avec `this[35]` (599 en temps normal).

## 6. Autre code mort : l'état 3

`sub_8F9940` porte une branche `état == 3` qui recentre le bouton 184 en bas de
la fenêtre. **Aucun des cinq écrivains de `0x015FFCE8` ne pose jamais 3** —
relevé exhaustif des 15 xrefs : `sub_8E2E20` pose 0, les quatre handlers posent
0, 1 ou 2, le reste ne fait que lire. Branche inerte.

## 7. Ce qui reste à regarder

- ~~`sub_DA6F40` : origine du blob non établie, piste = reconnexion en
  instance~~ → **résolu le 2026-09-01, et la piste était fausse.** C'est la
  **section 10 du fichier de rejeu** : la fonction (renommée
  `MemorialDun_RestoreFromReplaySection`) reçoit son blob de
  `ReplayFile_ReadStateSection`, et **hors rejeu le tampon est vide**, donc elle
  sort aussitôt. Elle ne rouvre la fenêtre 137 que pendant la lecture d'un
  `.brw`. Carte complète des 24 sections :
  [replay_state_sections.md](replay_state_sections.md).
- `UIMemorialDunWnd_OnPaint` (0x79d octets) n'a pas été lue : c'est elle qui
  décide du rendu selon l'état.

---

## 🔴🔴 Annexe — l'erreur de méthode que ce relevé a corrigée

En chemin, ce travail a invalidé une affirmation de
[entry_queue_re.md](entry_queue_re.md) §4, écrite la veille : « le cas 97 de
`Chat_HandleChatMessage` appelle `vtable+0x18` avec l'id 157, donc il ouvre la
fenêtre 157 ».

**`CGameMode::vtable+0x18` n'ouvre pas des fenêtres.** C'est `sub_C86740`, un
répartiteur avec son **propre espace d'identifiants 1..335**. La commande 157 y
envoie le paquet `0x0842` — aucun rapport avec `UIEntryQueueWnd`. J'avais lu un
`push 9Dh` et conclu « fenêtre 157 » parce que la valeur coïncidait avec le
numéro que je cherchais.

**Règle à retenir :** un identifiant poussé avant `UIWindowMgr_MakeWindow` est
une **fenêtre** ; poussé avant `CGameMode::vt+0x18`, c'est une **commande**. Les
deux espaces se recouvrent largement. Ne jamais déduire l'un de l'autre — c'est
la même famille d'erreur que « un slot de vtable ne nomme pas un geste »,
[[feedback_absence_needs_measurement]].
