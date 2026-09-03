# La carte des patchs WARP — rendre visible ce que l'IDB ne montre pas

> Relevé du **2026-09-02**. Comparaison octet à octet entre
> `E:\Nouveau dossier\2025-07-16_Ragexe_175220998_clientinfo.exe` (l'exe **vanilla**,
> celui qu'IDA a ouvert) et `E:\Nouveau dossier\Moonlight-Destiny\Moonlight-Destiny.exe`
> (l'exe **réellement livré aux joueurs**).
>
> Outils : [`tools/warp/diff_warp_patches.py`](../tools/warp/diff_warp_patches.py) et
> [`tools/warp/dump_injected_stub.py`](../tools/warp/dump_injected_stub.py).

## 1. Pourquoi ce document existe

`reference_ida_is_vanilla_warp_patches` le dit depuis longtemps : **l'IDB décrit un
binaire qui ne tourne pas.** Ce document en tire la conséquence pratique — la liste
exacte de ce que WARP change, résolue en noms de fonctions.

Le besoin n'est pas théorique. Ce relevé est né d'une conclusion **fausse** tirée de
l'IDB seule : « la commande 376 (l'icône `battle`) n'a aucun `case` dans
`UIMenuIconWnd_OnMsg`, donc l'icône ne fait rien ». C'est vrai du vanilla, et faux du
client livré, où WARP détourne précisément cette branche.

## 2. La mesure

Les deux fichiers font **16 144 384 octets**, mêmes sections, mêmes offsets.
**9 612 octets diffèrent**, regroupés (trous ≤ 16 o) en **262 régions** :

| section | régions | nature |
|---|---:|---|
| `.text` | **218** | patchs d'instructions dans le code d'origine |
| `.rdata` | 30 | constantes, chaînes, tables |
| `.xdiff` | **13** | ⚠ **code AJOUTÉ** — vide (zéros) dans le vanilla |
| `.rsrc` | 1 | ressource |

🔴 **La section `.xdiff` (VA `0x0171D000`..`0x0172E000`) est la zone d'allocation de
WARP** (`Exe.Allocate`). Elle est **entièrement nulle dans le vanilla** : tout ce qui
s'y trouve dans l'exe livré est du code injecté, qu'aucune analyse de l'IDB ne peut
voir. C'est là qu'il faut regarder quand un comportement observé en jeu n'a pas
d'explication dans le désassemblage.

## 3. Les 218 régions `.text`, par fonction

112 fonctions touchées. Les plus patchées :

| fonction | régions | octets |
|---|---:|---:|
| `Lua_LoadAllScriptFiles` | 14 | 400 |
| *(hors fonction — tables de données dans `.text`)* | 27 | 147 |
| `sub_A4BC70` | 18 | 39 |
| `CLoginMode_OnStateEnter` | 7 | 38 |
| `sub_76ECB0` | 1 | 37 |
| `sub_645950` | 2 | 32 |
| `Hair_BuildBodyOrHeadSpritePath_impl` | 6 | 30 |
| `Hair_BuildHeadPalettePath_impl` | 6 | 26 |
| `UIMakeCharWnd_CreateControls` | 2 | 22 |
| `UICashShopWnd_OnMsg` | 1 | 20 |
| `UIQuestDetailWnd_Init` | 1 | 20 |
| `EntryQueueTable_ParseBexFile` | 1 | 19 |

Familles reconnaissables : les chemins de sprites/palettes (`Hair_*`, `Job_*`,
`CActorSprite_*` — les patchs de coiffure et de corps), le chargement Lua, l'écran de
login, la création de personnage, et une longue traîne d'une à trois régions.

⚠ **`RecvLoop_DispatchPackets` est patché** (2 o @ `0xC9DF7C`) — à garder en tête :
c'est le répartiteur de paquets, et Bourgeon s'y accroche.

La carte complète est régénérable en une commande ; elle n'est pas recopiée ici pour
ne pas dater.

## 4. 🎯 Le cas qui a motivé le relevé : `RestoreBattlegroundUI`

### Ce que WARP injecte

`UIMenuIconWnd_OnMsg` reçoit **3 octets** de patch à `0x00814ADF` : c'est le
déplacement du `call UIWindow_OnMsg` de sa branche `default`, redirigé vers le stub
injecté `0x0171EB50` :

```
0171EB50  81 7d 10 78 01 00 00   cmp  dword [ebp+10h], 178h   ; commande 376 = icône « battle » ?
0171EB57  75 1d                  jnz  0171EB76
0171EB59  51                     push ecx
0171EB5A  8b 0d e4 43 5c 01      mov  ecx, [015C43E4h]        ; g_EntryQueueMgr
0171EB60  80 79 3c 01            cmp  byte [ecx+3Ch], 1
0171EB64  75 05                  jnz  0171EB6B
0171EB66  e8 65 ac 01 ff         call 007397D0                ; <-- 🔴 voir §4.2
0171EB6B  59                     pop  ecx
0171EB6C  68 9d 00 00 00         push 9Dh                     ; 157 = UIEntryQueueWnd
0171EB71  e8 ea 42 0f ff         call 00812E60                ; UIWindowMgr_ToggleWindowById
0171EB76  e9 f5 61 30 ff         jmp  00A24D70                ; UIWindow_OnMsg (comportement d'origine)
```

Source : `WARP0716\Scripts\Patches\RestoreBattlegroundUI.qjs`.

### ✅ Ce que ça résout

[entry_queue_re.md](entry_queue_re.md) §« Qui ouvre la fenêtre 157 ? » concluait :
« **L'ouvreur de la fenêtre 157 reste INCONNU.** […] aucun site du binaire ne passe
l'id 157 en immédiat à `MakeWindow` ». C'était exact **pour le vanilla**, et c'est
pour ça que la recherche ne pouvait pas aboutir : **l'ouvreur n'existe que dans l'exe
patché**. C'est l'**icône `battle` de la barre de menu**, que le même patch rend
visible.

### 4.2 🔴🔴 Un défaut réel : une adresse PHYSIQUE utilisée comme VIRTUELLE

Le `call` à `0x0171EB66` vise **`0x007397D0`**. Or :

- dans l'exe livré, `0x007397D0` contient `00 66 89 41 30 …` — c'est le **milieu**
  d'un `mov dword [ecx+28h], 0` appartenant à un constructeur de `CSkillInfo` ;
- la fonction réellement visée, celle que WARP cherche (`mov r32, 90Ah` puis remontée
  au prologue), commence à **`0x00B3A3D0`**, où l'on trouve bien
  `55 8b ec 83 ec 20` = `push ebp ; mov ebp,esp ; sub esp,20h`.

L'écart est **exactement** le décalage `.text` entre adresse virtuelle et offset
fichier :

```
0x00B3A3D0 − 0x00400C00 = 0x007397D0
            (0x400000 base + 0x1000 RVA − 0x400 offset brut)
```

**Cause :** dans WARP, `Exe.FindHex` rend une adresse **physique**, `Exe.GetTgtAddr`
une adresse **virtuelle**. Le script mélange les deux :

```js
const AddrOpenMenuWindow = Exe.GetTgtAddr(...);   // virtuelle ✅
const AddrDetourAddr     = Exe.GetTgtAddr(...);   // virtuelle ✅
const AddrSendEntryPacket = Addr;                 // 🔴 PHYSIQUE, jamais convertie
```

Les deux autres cibles du stub (`0x00812E60` `ToggleWindowById`, `0x00A24D70`
`UIWindow_OnMsg`) sont **justes** — ce sont celles issues de `GetTgtAddr`. Seule
celle-là est fausse. L'auteur connaît pourtant la distinction : il écrit
`Exe.Vir2Phy(AddrListButtonsID)` vingt lignes plus haut.

**Correctif** (idiome employé par les autres scripts du dépôt) :

```js
const AddrSendEntryPacket = Exe.Phy2Vir(Addr, CODE);
```

**Portée du défaut :** le `call` fautif est **gardé** par
`cmp byte [g_EntryQueueMgr+0x3C], 1`. Il ne part que si cet octet vaut 1 au moment du
clic sur l'icône. Quand il part, il saute au milieu d'une instruction — un plantage
est le résultat attendu.

⚠ **Non vérifié en jeu.** Ce qui précède est mesuré sur les fichiers ; ce que fait le
client au clic reste à constater. Le test tient en un clic sur l'icône
« battleground » de la barre de menu. C'est aussi le seul moyen de savoir dans quelles
conditions `+0x3C` vaut 1.

## 5. Comment refaire la mesure

```bash
python tools/warp/diff_warp_patches.py      # carte complète -> warp_patch_map.json
python tools/warp/dump_injected_stub.py     # désassemble un stub de .xdiff
```

Les deux chemins d'exe sont en tête de fichier. `diff_warp_patches.py` sait aussi dire
si une fonction précise est patchée ou intacte — c'est la question à se poser **avant**
de conclure quoi que ce soit à partir de l'IDB sur une fonction d'interface.

➡ **La règle qui en sort :** avant d'écrire « le binaire ne fait pas X », vérifier que
la fonction concernée est **intacte** dans l'exe livré. Sinon la phrase ne parle que
d'un fichier que personne n'exécute.

---

Voir aussi : [entry_queue_re.md](entry_queue_re.md),
[basic_info_re.md](basic_info_re.md) §2,
[unexplored_systems.md](unexplored_systems.md).
