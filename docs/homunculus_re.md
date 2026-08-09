# Homoncule — rétro-ingénierie complète (fenêtre d'état + arbre de compétences)

> Client `Moonlight-Destiny.exe` (Ragexe 20250716), image base `0x00400000`, **pas d'ASLR**
> (IDA == x32dbg). Serveur : fork rAthena `moonlight`, `PACKETVER 20211103`.
> RE statique (IDA) + **vérification live** (x32dbg attaché, fenêtre native ouverte,
> homoncule *Amistr* invoqué) — 2026-08-09.
>
> ⚠ Rappel projet : l'IDB décrit l'**exe VANILLA**. Les patchs WARP (110) n'y sont pas —
> cf. [[reference_ida_is_vanilla_warp_patches]]. Rien ici ne dépend d'un patch WARP.

---

## 0. TL;DR

| Quoi | Où |
|---|---|
| Fenêtre d'état | `UIHomunInfoWnd`, **window id 113 (`0x71`)**, vtable `0x010309D8` |
| Arbre de compétences | `UISkillListWnd`, **window id 114 (`0x72`)**, vtable `0x0103CB18` (mode homoncule) |
| Raccourci | **Alt+R** — comportement `124` de `UIWindowMgr_DispatchHotkeyBehavior` |
| Gate d'ouverture | `MakeWindow(113)` **refuse** si `dword_15FF95C == -1` (pas de classe d'homoncule) |
| « J'ai un homoncule » | `dword_15FF9B4 != 0` (`0x015FF9B4`) |
| Bloc de données | globals plats `0x015FF918`…`0x015FF9C4` (+ `0x015FBA04`) |
| Liste de compétences | `std::list` en `0x015FA424` (tête) / `0x015FA428` (taille) |
| Paquet d'état | **ZC_PROPERTY_HOMUN `0x0BA4`**, 85 o (handler `0x00CD1ED0`) |
| Deltas HP/SP/EXP | **ZC_HO_PAR_CHANGE `0x0BA5`**, 12 o (inline `0x00CA7AB7`) |
| Actions | CZ `0x022D` (info/nourrir/supprimer), `0x0231` (renommer), `0x0112` (monter un skill), `0x02D8` type 3 (auto-feed) |

**🔴 Le piège n°1** : il existe **cinq** versions de `ZC_PROPERTY_HOMUN` dans le client
(`0x022E`, `0x09F7`, `0x0B2F`, `0x0B76`, `0x0BA4`), chacune avec son propre décodeur et sa
propre disposition de champs. **Seule `0x0BA4` est celle que Moonlight envoie.** Lire le
décodeur `0x022E` (le plus « lisible » dans IDA) donne des offsets **faux de 4 octets** dès
l'intimité — et un `itemId` qui n'existe plus.

---

## 1. La fenêtre d'état — `UIHomunInfoWnd` (id 113)

### 1.1 Identité

| Élément | Adresse |
|---|---|
| vtable | `0x010309D8` |
| RTTI | `??_R0?AVUIHomunInfoWnd@@@8` `0x0123F0D8` → `.?AVUIHomunInfoWnd@@` |
| ctor | `0x0086BA40` (appelle `UIWindow_composite_ctor`) |
| dtor | `0x0086E2E0` |
| `OnCreate` (vt+0x3C) | `0x00875620` |
| `OnMsg` (vt+0x94) | `0x00884250` |
| `OnRender` (vt+0x50) | `0x0087E7B0` |
| copie du rect (vt+0x2C) | `0x008896B0` |
| instance vivante | `0x0131F8A8` (= `windowMgr 0x0131F4E8` + `0x3C0`) |
| taille allouée | `0xD8` |
| fond | `유저인터페이스\basic_interface\homuninfo_bg.bmp` (`0x01031930`) |

La classe dérive du **composite** `UIWindow` (`vtbl_UIWindow_composite` `0x0102FE08`) : seuls
5 slots sont surchargés (dtor, `+0x2C`, `OnCreate`, `OnRender`, `OnMsg`).

**Gate de création** (`UIWindowMgr_MakeWindow`, case 113 @ `0x00A3E409`) :

```asm
cmp  dword_15FF95C, 0FFFFFFFFh   ; classe de l'homoncule
jz   def_A3B62F                  ; -1  ->  la fenêtre n'est PAS créée
```

### 1.2 Champs de l'objet

| Offset | Index `this[n]` | Contenu |
|---|---|---|
| `0x8C` | 35 | `201` — cmd de la croix de fermeture |
| `0xB4` | 45 | **`UIEdit`** du nom (ctor `0x00817A50`, alloc `0x11C`), 80×16 @ (135,22), champ `+0x88 = 50` |
| `0xB8` | 46 | *scratch* — réutilisé pour chacun des 4 boutons bitmap pendant `OnCreate` |
| `0xBC` | 47 | `UIBarGraphHomun` **PV**, 157×9 @ (117,73) |
| `0xC0` | 48 | `UIBarGraphHomun` **SP**, 157×9 @ (117,88) |
| `0xC4` | 49 | `UIINT64BarGraph` **EXP**, 102×6 @ (101,119) |
| `0xC8` | 50 | `UIBarGraph4` **satiété**, 102×6 @ (101,145) |
| `0xD0` | 52 | boîte de dialogue modale enfant (détruite par le dtor) |
| `0xD4` | 53 | `UIToggleButton` **auto-feed** 12×12 @ (210,142) |

Les deux jauges PV/SP sont des `UIBarGraphHomun` (vtable `0x0102B5E0`, ctor `0x00834630`)
alimentées par **quatre lambdas** — de simples `mov eax,[global]; ret` :

| Lambda | Adresse | Rend |
|---|---|---|
| `lambda_1` | `0x0088A010` | `dword_15FF964` = PV |
| `lambda_2` | `0x0088A030` | `dword_15FF968` = PV max |
| `lambda_3` | `0x0088A050` | `dword_15FF96C` = SP |
| `lambda_4` | `0x0088A070` | `dword_15FF970` = SP max |

### 1.3 Boutons

Tous en `유저인터페이스\<nom>` + `.bmp` / `_a.bmp` / `_b.bmp` (`UIBitmapButton`).

| Bitmap | Position | cmd UI | Action |
|---|---|---|---|
| `btn_rewrite` | (232, 21) | **184** | renommer (valide le contenu de l'`UIEdit`) |
| `btn_del` | (187, 44) | **291** | supprimer l'homoncule |
| `btn_skill` | (232, 44) | **289** | bascule la fenêtre **114** (arbre de compétences) |
| `btn_feed` | (210, 105) | **290** | nourrir |
| `checkbox_0/1` | (210, 142) | **213** | auto-feed on/off (`UIToggleButton`) |
| croix | — | **201** | ferme la fenêtre 113 |

> ⚠ `btn_feed` est d'abord posé en (210,155) puis **redéplacé en (210,105)** juste après
> `AddChildControl`. La position finale est (210,105).

### 1.4 `OnMsg` (`0x00884250`) — comportement exact

| msg / cmd | Effet |
|---|---|
| `34` | attache le rect sauvegardé (`this+204`) et repositionne |
| `6` / **201** | `UIWindowMgr_SaveRectAndCloseWindow(113)` → **DÉTRUIT** la fenêtre |
| `6` / **289** | ferme 114 ; si elle n'existait pas, `MakeWindow(114)` (bascule) |
| `6` / **184** | **renommer** (voir plus bas) |
| `6` / **290** | modale « nourrir ? » (MsgString `0x259`) → réponse en cmd 488 |
| `6` / **291** | modale MSI\_DELETE\_HOMUN (`0x3BB`) ; si OK (`187`) → `SendMsg(184, 2)`, ferme 113 **et** 114, `GameMode_ClearBlock55dc`, `dword_15FF9B4 = 0` |
| `6` / **488** | réponse de la modale « nourrir » : si `187` **et** que la fenêtre `10011` n'existe pas → `SendMsg(184, 1)` ; sinon message d'erreur `0xDDF` en chat |
| `6` / **213** | bascule auto-feed → `sub_DA8870(0|1)` |

**Le renommage (cmd 184)** — chaîne de validation, dans l'ordre :

1. `strlen(edit) >= 24` → refus, MsgString `0x3AA` (« 23 lettres max »).
2. le texte contient une des deux chaînes interdites (`0x0120451C`, `0x01204534`) → refus, MsgString `0xAFC`.
3. confirmation MsgString `0xBA1` (« changer le nom en ^0000ff%s ? »).
4. filtre de grossièretés `sub_A85BE0(&unk_159C2C8, name)` → si positif, refus MsgString `0xB84`.
5. sinon `SendMsg(185, name)` puis recopie locale dans `byte_15FF91C`.

> **Le renommage est-il à usage unique ?** Dans rAthena par défaut, oui : `rename_flag`
> (bit 0 des `flags`) verrouille après le premier succès — MSI\_HOMUN\_NAME\_CHANGE\_ONLYONCE.
>
> 🔴 **Pas sur Moonlight.** `conf/import/battle_conf.txt:366` pose **`hom_rename: yes`**
> (le défaut de `conf/battle/homunc.conf:36` est `no`, mais `conf/import/` l'écrase). Deux
> conséquences :
> - `hom_change_name` (`homunculus.cpp:1020`) teste
>   `if (rename_flag && !battle_config.hom_rename) return 1;` → la garde ne se déclenche
>   **jamais** ;
> - `clif_hominfo` (`clif.cpp:1888`) ne pose le bit 0 que si `!battle_config.hom_rename` →
>   le client **ne reçoit jamais** ce drapeau.
>
> Donc **renommer est libre et illimité, pour tout le monde**. Aucun test de groupe ni de
> permission n'intervient dans ce chemin : ce n'est **pas** un privilège de staff —
> cf. [[reference_rathena_group_permission_or]] pour ce à quoi ressemble un vrai gating.

### 1.5 `OnRender` (`0x0087E7B0`) — ce que le natif dessine et d'où

Fond `homuninfo_bg.bmp` puis, **par-dessus**, les libellés (les intitulés de la colonne de
gauche, eux, sont **peints dans le bitmap de fond**, pas par le code) :

| Texte | Position | Source |
|---|---|---|
| Titre « Homunculus Info » | (5, 2) | MsgString `0x3A8` |
| « Name » | (100, 24) | MsgString `0x197` |
| « Level » | (100, 47) | MsgString `0x198` + `itoa(dword_15FF960)` @ (135,47) |
| « EXP » | (100, 105) | MsgString `0x3FB` + `"%lld"` @ droite (205,104) |
| « Hunger » | (100, 131) | MsgString `0x249` + `"%d / %d"` @ droite (200,130) |
| « Intimacy » | (100, 159) | MsgString `0x24A`, « : » @ (140,159), palier @ (150,159) |
| « Auto Feeding » | (225, 136) | MsgString `0xCCF` |
| « 10 % de l'exp du maître » | (8, 180) | MsgString `0xCD4` |

Colonne de stats, **alignée à droite sur x = 79**, un `%d` par ligne :

| y | Global | Stat |
|---|---|---|
| 24 | `dword_15FF93C` | ATK |
| 42 | `dword_15FF940` | MATK |
| 60 | `dword_15FF944` | HIT |
| 78 | `dword_15FF948` | CRI |
| 96 | `dword_15FF94C` | DEF |
| 114 | `dword_15FF950` | MDEF |
| 132 | `dword_15FF954` | FLEE |
| 150 | *(calculé)* | **ASPD = `(2000 - dword_15FF958) / 10`** |

**Paliers d'intimité** (sur `dword_15FF974`, échelle 0…1000 = `intimacy/100` côté serveur) :

| Intervalle | MsgString | Libellé EN |
|---|---|---|
| ≤ 3 | `0x3FE` | Hate with a Passion |
| ≤ 10 | `0x3FD` | Hate |
| ≤ 100 | `0x2A0` | Awkward |
| ≤ 250 | `0x2A1` | Shy |
| ≤ 750 | `0x29D` | Neutral |
| ≤ 910 | `0x2A2` | Cordial |
| ≤ 1000 | `0x2A3` | Loyal |
| > 1000 | `0x2A4` | Unknown |

> 🔴 **Quirk du natif** : à côté du libellé « EXP », le client imprime `qword_15FF988`,
> c'est-à-dire l'**exp requise pour le niveau suivant**, PAS l'exp courante. Seule la jauge
> montre `exp / expNext`. Vérifié en live : `exp = 0`, `expNext = 50`, la ligne affiche `50`.
> Notre version ImGui affiche `exp / expNext` — cf. [[feedback_replace_native_fill_gaps]].

> La satiété est affichée `%d / 100` avec **100 codé en dur** dans `OnRender` ; le maximum
> stocké (`dword_15FF994`) vaut lui aussi 100.

---

## 2. Le bloc de données client

Tout est en **globals plats**, pas de structure allouée. La base `g_UIWindowContextKey`
(`0x015FA3C0`) est l'objet « session/GameMode statique » : plusieurs de ces globals en sont
des champs.

| Adresse | Type | Contenu | `g_UIWindowContextKey+` |
|---|---|---|---|
| `0x015FF918` | u32 | **AID (GID) de l'homoncule** | `0x5558` |
| `0x015FF91C` | char[24] | **nom** | `0x555C` |
| `0x015FF93C` | i32 | ATK (`atk2`) | |
| `0x015FF940` | i32 | MATK | |
| `0x015FF944` | i32 | HIT | |
| `0x015FF948` | i32 | CRI | |
| `0x015FF94C` | i32 | DEF | |
| `0x015FF950` | i32 | MDEF | |
| `0x015FF954` | i32 | FLEE | |
| `0x015FF958` | i32 | `amotion` (→ ASPD) | |
| `0x015FF95C` | i32 | **classe / job id** (`-1` = aucun homoncule) | |
| `0x015FF960` | i32 | **niveau** | |
| `0x015FF964` | i32 | **PV** | |
| `0x015FF968` | i32 | **PV max** | |
| `0x015FF96C` | i32 | **SP** | |
| `0x015FF970` | i32 | **SP max** | |
| `0x015FF974` | i32 | **intimité** (0…1000) | |
| `0x015FF978` | i32 | `itemId` d'accessoire — **legacy**, seul `0x022E`/`0x0230` l'écrit | |
| `0x015FF980` | i64 | **exp** | |
| `0x015FF988` | i64 | **exp du niveau suivant** (`0` = niveau max) | |
| `0x015FF990` | i32 | **satiété** (0…100) | |
| `0x015FF994` | i32 | satiété max (= 100) | |
| `0x015FF998` | i32 | **flags** : b0 déjà renommé, b1 en repos (vaporized), b2 vivant | |
| `0x015FF99C` | i32×6 | bloc d'**ordre à l'homoncule** (`GameMode_ClearBlock55dc`) | `0x55DC` |
| `0x015FF9B4` | i32 | **≠ 0 ⇔ un homoncule est invoqué** | `0x55F4` |
| `0x015FF9C0` | i32 | dernière valeur de satiété ayant déclenché l'alerte | |
| `0x015FF9C4` | i32 | **portée d'attaque** | |
| `0x015FBA04` | i32 | **points de compétence** de l'homoncule | |
| `0x0160231C` | u8 | état **auto-feed** | `0x7F5C` |
| `0x015FA424` | ptr | **tête de la `std::list` des compétences** | `0x64` |
| `0x015FA428` | i32 | **nombre de compétences** | `0x68` |
| `0x015FA42C` / `0x015FA430` | | idem pour le **mercenaire** | `0x6C` / `0x70` |

**Lecture live de contrôle** (Amistr niveau 1, `x32dbg`, `0x015FF918`+184 o) :

```
AID 0x002E14DA · nom "Amistr" · ATK 45 · MATK 15 · HIT 25 · CRI 5 · DEF 42 · MDEF 2
FLEE 18 · amotion 635 (ASPD 136) · classe 6002 · niveau 1 · PV 320/320 · SP 10/10
intimité 246 ("Shy") · exp 0 / 50 · satiété 48/100 · flags 4 (vivant) · portée 2
présent = 1 · points de compétence 49
```

**Classe → nom / sprite.** `Job_GetDisplayNameOrResName(g_UIWindowContextKey, class, -1)`
(`0x00D5BB40`) : pour `class > 30` hors `[4001, 5999]`, rend directement
`jobNameTable[class]`, la table étant le `std::vector<const char*>` `begin = 0x015FB348`,
`end = 0x015FB34C` (chargé depuis `jobname.lub`). Vérifié live : `jobname[6002] = "AMISTR"`.
Le sprite est ensuite `homun\<nom>.spr` / `.act` (`CActorSprite_BuildMonsterBodyPath`
`0x00D339C0`, formats `0x010940F8` / `0x01094108`).

Plages de classes (rAthena) : `6001…6016` homoncules classiques, `6048…6052` homoncules S
— c'est exactement le test de `Job_IsHomunculusId` (`0x00D8E8D0`).

---

## 3. Protocole

### 3.1 ZC — état complet : `ZC_PROPERTY_HOMUN`

Le client sait décoder **cinq** variantes ; le dispatch se fait par
`RecvLoop_DispatchPackets` (`0x00C9DF00`, `JMP [g_RecvDispatchTable + (op-0x73)*4]`).

| Opcode | Long. | Site de dispatch | Décodeur |
|---|---|---|---|
| `0x022E` | 73 | `0x00CA781F` | `0x00CD1B50` |
| `0x09F7` | 77 | *(aucun)* | — |
| `0x0B2F` | 73 | `0x00CA7830` | `0x00CD20D0` |
| `0x0B76` | 77 | `0x00CA7841` | **inline** dans `RecvLoop_DispatchPackets` |
| **`0x0BA4`** | **85** | `0x00CA7AA6` | **`0x00CD1ED0`** ← **celui de Moonlight** |

> Le bloc `0x00CD1D10`…`0x00CD1ECF` ressemble à un cinquième décodeur mais **aucune
> référence de code ne le vise** et IDA n'en fait pas une fonction : code mort. Ne pas s'en
> servir pour en déduire des offsets — cf. [[feedback_dead_code_unverified_offsets]].

`PACKETVER 20211103` ⇒ `PACKETVER_MAIN_NUM ≥ 20210303` ⇒ `packets_struct.hpp` sélectionne
`PACKET_ZC_PROPERTY_HOMUN4` = **`0x0BA4`**.

**Disposition `0x0BA4` (85 o)** — offsets **vérifiés dans le décodeur `0x00CD1ED0`** :

| Offset | Type | Champ | Destination client |
|---|---|---|---|
| `+0` | u16 | opcode `0x0BA4` | |
| `+2` | char[24] | nom | `byte_15FF91C` (via `snprintf_s(...,32,...)`) |
| `+26` | u8 | flags | `dword_15FF998` |
| `+27` | u16 | niveau | `Value_015FF960` |
| `+29` | u16 | satiété | `dword_15FF990` |
| `+31` | u16 | intimité | `dword_15FF974` |
| `+33` | u16 | `atk2` | `dword_15FF93C` |
| `+35` | u16 | `matk` | `dword_15FF940` |
| `+37` | u16 | `hit` | `dword_15FF944` |
| `+39` | u16 | `crit` | `dword_15FF948` |
| `+41` | u16 | `def` | `dword_15FF94C` |
| `+43` | u16 | `mdef` | `dword_15FF950` |
| `+45` | u16 | `flee` | `dword_15FF954` |
| `+47` | u16 | `amotion` | `dword_15FF958` |
| `+49` | u32 | PV | `dword_15FF964` |
| `+53` | u32 | PV max | `dword_15FF968` |
| `+57` | u32 | SP | `dword_15FF96C` |
| `+61` | u32 | SP max | `dword_15FF970` |
| `+65` | i64 | exp | `qword_15FF980` |
| `+73` | i64 | exp suivante | `qword_15FF988` |
| `+81` | u16 | points de compétence | `dword_15FBA04` |
| `+83` | u16 | portée | `dword_15FF9C4` |

> 🔴 Les stats `u16` du paquet sont lues par le client en **`__int16` signé**. Au-delà de
> 32767 elles passent négatives — même piège que [[project_matk_overflow]]. Le serveur cape
> déjà à `INT16_MAX` (`cap_value(...)`, `i16min(...)`), donc le risque est côté serveur.

**Effets de bord du décodeur** (à reproduire si on le remplace, cf.
[[feedback_replaced_handler_hidden_duties]]) :

- alerte de faim en chat (MsgString `0x3FA`) quand la satiété **passe** à 1, 5 ou 10 —
  déduplication par `dword_15FF9C0` ;
- `sub_D70D30` après chaque champ affiché : invalide + relayoute la fenêtre 113 ;
- `sub_D70DD0` après les points de compétence : notifie la fenêtre 114 ;
- `OnMsg(0, 23)` envoyé à la fenêtre 114 (`0x0131F8AC`) — reconstruit la liste des skills ;
- ré-arme la jauge EXP : `UIINT64BarGraph_SetCurMax(homunWnd+0xC4, exp, expNext)` ;
- ré-arme la jauge satiété : `sub_863720(cur=0x15FF990, max=&0x15FF994)`.

#### 🔴 Ce que valent VRAIMENT les huit statistiques

Le panneau natif affiche huit nombres bruts, sans unité. Deux d'entre eux sont
franchement trompeurs. Tout ce qui suit est vérifié dans les sources Moonlight
(`clif_hominfo`, `battle_calc_defense_reduction` branche `#ifndef RENEWAL`,
`conf/battle/`) — Moonlight est **pré-renewal**, cf. [[project_moonlight_is_prerenewal]].

| Ligne | Ce que le serveur met dedans | Ce que ça fait |
|---|---|---|
| **ATK** | `cap(batk + rhw.atk2, 0, 32767)` | dégât physique **avant** défense |
| **MATK** | `matk_max` | `hom_setting` a **`HOMSET_SAME_MATK`** (0x20) : min = max, **pas de fourchette** |
| **HIT** | `status->hit` = `min(niveau, 600) + DEX` | `touche% = 80 + HIT − FLEE_cible`, borné 5…100 |
| **CRI** | **`LUK/3 + 1`** | ⚠ **PAS un taux de critique** — voir ci-dessous |
| **DEF** | **`DEF1 + VIT`** | ⚠ somme d'un **%** et d'une **stat** — voir ci-dessous |
| **MDEF** | `MDEF1` **seul** | réduction en **%** : `dégâts × (100 − MDEF1) / 100` |
| **FLEE** | `status->flee` = `min(niveau, 600) + AGI` | se retranche du HIT de l'attaquant ; −10 % par assaillant au-delà du 2ᵉ |
| **ASPD** | *(rien)* | **calculé par le client** : `(2000 − amotion) / 10` |

**CRI.** `conf/battle/homunc.conf` : `hom_setting: 0x3D`, donc **`HOMSET_DISPLAY_LUK`
(0x10) est actif** — le serveur envoie `luk/3 + 1` « instead of their actual
critical ». Et `conf/battle/battle.conf` : `enable_critical: 17` = `BL_PC (0x001) |
BL_MER (0x010)` ; **`BL_HOM` vaut 0x008 et n'y est pas** : un homoncule ne fait
**jamais** de critique. Cette ligne n'est donc qu'un **indicateur de LUK**
(`LUK ≈ (affiché − 1) × 3`). Même piège que les monstres, cf.
[[reference_prere_combat_formulas]].

**DEF.** `clif_hominfo` envoie `status->def + status->vit` — pas `def2`. Or en
pré-renewal, avec `weapon_defense_type: 0` et `magic_defense_type: 0`, les deux
défenses n'ont ni la même unité ni le même point d'application :

```
dégâts = dégâts × (100 − DEF1) / 100      ← DEF1 = DEF DURE, en POUR CENT
dégâts = dégâts − vit_def                 ← DEF DOUCE, PLATE
   cible non joueuse (l'homoncule en est une) :
   vit_def = DEF2 + rnd(0, (DEF2/20)²)    ← DEF2 = VIT + bonus
```

Le nombre affiché ne se lit donc **ni en % ni en plat** : c'est un total d'affichage
hérité d'Aegis. Idem côté magie, à ceci près que la **MDEF douce (`INT + VIT/2`)
n'est pas transmise du tout** — la résistance magique réelle est *supérieure* au
chiffre montré.

**Côté serveur** (`clif_hominfo`, `src/map/clif.cpp:1883`) :

- `intimacy` envoyée **divisée par 100** (0…1000) ;
- `flags = (rename_flag ? 1 : 0) | (vaporize == HOM_ST_REST ? 2 : 0) | (hp > 0 ? 4 : 0)` ;
- `crit = luk/3 + 1` si `battle_config.hom_setting & HOMSET_DISPLAY_LUK`, sinon `cri/10` ;
- pré-renewal (Moonlight, cf. [[project_moonlight_is_prerenewal]]) :
  `def = def + vit`, `mdef = mdef` ;
- `amotion = 0` si `flag != 0` (appel « allégé ») ;
- si `max_hp > INT32_MAX`, bascule en **pourcentage** : `hp = hp/(max_hp/100)`, `maxHp = 100` ;
- `expNext = 0` quand l'homoncule est au niveau max de son type (`HT_REG`/`HT_EVO` →
  `hom_max_level`, `HT_S` → `hom_S_max_level`) ⇒ **prévoir la division par zéro** côté UI.

### 3.2 ZC — deltas : `ZC_HO_PAR_CHANGE`

| Opcode | Long. | Forme | Site |
|---|---|---|---|
| `0x07DB` | 8 | `{u16 type, i32 value}` | `0x00CA7A1A` |
| **`0x0BA5`** | **12** | `{u16 type, u64 value}` | `0x00CA7AB7` |

`PACKETVER 20211103` ⇒ `0x0BA5`. Trois types seulement (`_sp` de rAthena) :

| `type` | Nom | Écrit |
|---|---|---|
| `1` | `SP_BASEEXP` | `qword_15FF980` (64 bits complets) **puis** ré-arme la jauge EXP |
| `5` | `SP_HP` | `dword_15FF964` (32 bits bas seulement) |
| `7` | `SP_SP` | `dword_15FF96C` (32 bits bas seulement) |

> ⚠ Pour HP/SP le client ne lit que le **dword bas** du champ 64 bits ; seul EXP est lu en
> entier. Et **aucun** de ces trois ne met à jour PV **max** / SP **max** : un changement de
> maximum n'arrive que par un `0x0BA4` complet.

### 3.3 ZC — changement d'état : `ZC_CHANGESTATE_MER` `0x0230` (12 o)

`{u16 op, u8 type=0, u8 state, u32 GID, u32 data}` — handler `0x00CBA900`.

| `state` | Nom rAthena | Effet client |
|---|---|---|
| `0` | `SP_ACK` | `dword_15FF918 = GID` ; **`dword_15FF9B4 = 1`** ; `dword_15FF95C = job de l'acteur` |
| `1` | `SP_INTIMATE` | `dword_15FF974 = data` (+ relayout, + message chat si mode 7) |
| `2` | `SP_HUNGRY` | `dword_15FF990 = data` (+ relayout, + « Full : %d » si mode 7) |
| `3` | *(accessoire)* | `dword_15FF978 = data` ; si `0`, réapplique le job de base au sprite |

C'est **`state = 0` qui allume le drapeau `dword_15FF9B4`** : c'est le signal « un homoncule
existe » que le raccourci Alt+R et notre onglet doivent tester.

### 3.4 ZC — compétences

| Opcode | Long. | Nom | Handler |
|---|---|---|---|
| `0x0235` | VAR | `ZC_HOSKILLINFO_LIST` | inline `0x00CA3184` (partagé avec `0x010F`, `0x029D`) |
| `0x0239` | 11 | `ZC_HOSKILLINFO_UPDATE` | `0x00CD71B0` (partagé avec `0x010E`, `0x029E`) |

**`0x0235`** : `{u16 op, u16 len, entrée[]}`, **entrée = 37 octets** —
`{u16 id, u32 inf, u16 level, u16 sp, u16 range, char name[24], u8 upgradable}`.
Le nombre d'entrées est calculé par `(len - 4) / 37` (division par réciproque
`0xBACF914D`). Le nom transmis est **ignoré** : le client prend le sien dans sa base.

**`0x0239`** : `{u16 op, u16 id, u16 level, u16 sp, u16 range, u8 upgradable}`.
`sub_CD71B0` route par opcode :

| Opcode | Base de données consultée | Notification |
|---|---|---|
| `0x010E` (joueur) | `sub_737E00(&dword_15FA3CC, …)` | `sub_D7E730` |
| `0x0239` (**homoncule**) | `ClientDB_GetEntryById_Range8000` (`0x00D808B0`) | `sub_D7E4F0` (`0x00D7E4F0`) |
| `0x029E` (mercenaire) | `ClientDB_GetEntryById_Range2008` (`0x00D80BD0`) | `sub_D7E570` |

Le suffixe **`Range8000`** est littéral : les compétences d'homoncule commencent à
**`HM_SKILLBASE = 8001`** (`HLIF_HEAL`).

### 3.5 CZ — actions du joueur

Toutes passent par `CMode::SendMsg` (`0x00C86740`, `CGameMode` vtable `+0x18`), dont la
table de saut est en `0x00C930F0` (index = `msg - 1`).

| `SendMsg` | Site | Paquet émis | Contenu |
|---|---|---|---|
| **184** | `0x00C907EC` | **CZ `0x022D`** (5 o) | `{u16 0x022D, u16 type=0, u8 cmd}` — `cmd` : **0** info, **1** nourrir, **2** supprimer |
| **185** | `0x00C9082D` | **CZ `0x0231`** (26 o) | `{u16 0x0231, char name[24]}` — tronqué à 23 + NUL |
| **67** | `0x00C8D894` | **CZ `0x0112`** (4 o) | `{u16 0x0112, u16 skillId}` — monter un skill ; pose aussi `GameMode+0x10C = 1` |
| **113** | `0x00C8D6BD` | (lancement de skill par INF) | cf. [[reference_cmode_sendmsg_use_skill]] |

Hors `SendMsg`, l'auto-feed a son propre émetteur : **`sub_DA8870(bool)` `0x00DA8870`** →
**CZ_CONFIG `0x02D8`** (10 o) `{u16 0x02D8, u32 type=3, u32 value}`. `type 3` = auto-feed
homoncule. L'accusé serveur repasse par `sub_DA8740` (`0x00DA8740`), qui écrit
`byte_160231C` et affiche MsgString `0xCD2` (on) / `0xCD3` (off) en chat.

#### « Attendre / Stand By » — un ordre qui ne sort pas du client

Le menu contextuel de l'homoncule propose trois entrées (`docs/entity_context_menu_re.md`
§5c) : **37** statut, **38** nourrir, **39** *Stand By*. Les deux premières émettent des
paquets ; **la troisième non**.

Case **39** (`0x00C88DB8`, et le sélecteur **189** de `CMode::SendMsg` en `0x00C90142`
fait la même chose) : elle récupère l'acteur `g_Homun_Aid` puis appelle
`Actor_SetPendingAiOrder` (**`0x00C58850`**, slot `vt+0xCC` d'un `CNpc` — l'homoncule en
est un, RTTI lu en live) avec une structure de 24 octets dont le premier dword vaut **9**,
et `which = 0`.

```c
// vt+0xCC — un simple DÉPÔT, aucun paquet
which == 0 -> memcpy(actor + 0x318, msg, 24)   // homoncule
which == 1 -> memcpy(actor + 0x348, msg, 24)   // mercenaire
```

C'est **l'IA cliente en Lua** (dossier `AI\` du jeu) qui vient lire ce bloc.
`AI\Const.lua` donne les codes : `0` NONE, `1` MOVE, `2` STOP, `3` ATTACK_OBJECT,
`4` ATTACK_AREA, `5` PATROL, `6` HOLD, `7` SKILL_OBJECT, `8` SKILL_AREA, **`9` FOLLOW**.

Donc « Stand By » = **`FOLLOW_CMD`**, et `OnFOLLOW_CMD` (`AI\AI.lua`) en fait une
**bascule** :

```lua
if (MyState ~= FOLLOW_CMD_ST) then
    MoveToOwner (MyID)          -- rejoint le maître
    MyState = FOLLOW_CMD_ST
    MyEnemy = 0 ; MySkill = 0   -- abandonne cible et incantation en cours
else
    MyState = IDLE_ST           -- 2ᵉ appel : repasse en autonomie
end
```

**Conséquences pratiques** : l'ordre coupe le combat en cours et colle l'homoncule au
maître ; il ne touche **ni** au serveur **ni** à l'état persistant ; **rappuyer** rend
l'homoncule à son IA normale. Rien à répliquer côté Bourgeon — l'entrée de menu passe par
le menu contextuel d'entité, que notre onglet ne remplace pas.

Autres CZ homoncule connus du client (non émis par la fenêtre) :

| Opcode | Long. | Nom | Parseur serveur |
|---|---|---|---|
| `0x0232` | 9 | `CZ_REQUEST_MOVENPC` — déplacer l'homoncule | `clif_parse_HomMoveTo` |
| `0x0233` | 11 | `CZ_REQUEST_ACTNPC` — attaquer | `clif_parse_HomAttack` |
| `0x0234` | 6 | `CZ_REQUEST_MOVETOOWNER` — rappeler au maître | `clif_parse_HomMoveToMaster` |

**Côté serveur.** `hom_menu` (`src/map/homunculus.cpp:865`) : `0` ne fait **rien**, `1` =
`hom_food`, `2` = `hom_delete`. `pc_skillup` (`src/map/pc.cpp:9601`) route les ids
`SKILL_CHK_HOMUN` vers `hom_skillup` : **le même `0x0112` sert au joueur et à l'homoncule**.

---

## 4. L'arbre de compétences — `UISkillListWnd` (id 114)

### 4.1 Identité, et une correction

| Élément | Adresse |
|---|---|
| vtable | `0x0103CB18` (RTTI `.?AVUISkillListWnd@@`) |
| ctor | `0x00935290` (alloc `0x148`) |
| dtor | `0x00936C50` |
| `OnCreate` | `0x00943150` |
| `OnMsg` | `0x00959890` |
| instance | `0x0131F8AC` (= windowMgr + `0x3C4`) |

> 🔴 **Correction de `docs/skill_tree_re.md` §7.** Ce document affirmait que le
> « vrai `UISkillListWnd` » portait l'id `0x105`, vtable `0x0103F3EC`, ctor `0x00970D30`, et
> qu'**aucun chemin ne l'ouvrait**. Les trois sont faux :
> - `0x0103F3EC` a pour RTTI **`.?AVUIPetEvolutionWnd@@`** — l'id `0x105` est la fenêtre
>   d'**évolution de familier** (IDA nomme déjà son ctor `UIPetEvolutionWnd_ctor`) ;
> - le vrai `UISkillListWnd` est en `0x0103CB18` / ctor `0x00935290` ;
> - il **est** ouvert : c'est la fenêtre **114** (homoncule) et **126** (mercenaire).
>
> Ce qui reste vrai de ce §7 : la fenêtre du grimoire du **joueur** est bien
> `UINewSkillListWnd` id `0x25`, et `UISkillListWnd` n'est pas dans son chemin.

### 4.2 Une classe, deux modes

`this[81]` (`+0x144`) sélectionne la source de données :

| `this[81]` | Sujet | Points | Nombre | Accès par index | Niveau choisi | Lancer |
|---|---|---|---|---|---|---|
| **0** | **homoncule** | `dword_15FBA04` | `dword_15FA428` | `sub_D80810` `0x00D80810` | `sub_D5A570` / `sub_D77070` | fenêtre **114** |
| `1` | mercenaire | `dword_15FBA08` | `dword_15FA430` | `sub_D80B30` | `sub_D5BDE0` / `sub_D77110` | fenêtre **126** |

`sub_D80810(g_UIWindowContextKey, &out, index)` parcourt la `std::list` `+0x64` en
s'arrêtant à `index` et rend une copie de l'`ItemSkillInfo` ; `out` est vide si l'index
dépasse `+0x68`.

### 4.3 Commandes d'`OnMsg` (`0x00959890`)

La fenêtre affiche **7 lignes** (`this[73]` = `(hauteur - 38) / 32`), défilables via
`this[71]` (index du premier élément).

| cmd | Rôle |
|---|---|
| `201` | fermer (114 en mode homoncule, 126 en mode mercenaire) |
| `184` | double-clic ligne → `OnMsg(0, 58)` |
| `239`…`245` | **« + » : monter le skill** de la ligne — refusé si `dword_15FBA04 == 0` ; envoie `SendMsg(67, skillId)` = **CZ `0x0112`**. Anti-rebond 1 s (`dword_131EE64` / `dword_131EE6C` / `dword_131EE70`) |
| `253`…`259` | niveau de lancement **−1** (borné à `[1, maxLv]`) |
| `260`…`266` | niveau de lancement **+1** |
| msg `58` | **lancer** : `SendMsg(113, skillInfo, level)` |
| msg `48` | ouvrir la description : fenêtre `0x2E` (skill) ou `0x37` selon le type — cf. [[project_skill_description_window_re]] |
| msg `49` | menu contextuel (fenêtre `0x12`) — libellé MsgString `0x142` |
| msg `23` | **reconstruction complète** de la liste depuis `dword_15FA428` / `sub_D80810` (c'est ce que `0x0BA4` déclenche) |
| msg `14`/`3` | redimensionnement : bornes 232…320 × 120…288, pas de 32 px |

### 4.4 Le nœud de compétence

La `std::list` en `0x015FA424` est une liste MSVC : nœud = `{next, prev, value}`, la valeur
commence donc à **`nœud + 8`**. C'est la **même structure `ItemSkillInfo`** que le bundle de
compétences du joueur (`0x015FA3CC`) exploité par `rag::LearnedSkillLevel`.

| Offset (depuis `value`) | Contenu |
|---|---|
| `+0x04` | drapeau « entrée valide » |
| `+0x08` | **id de compétence** (8001+) |
| `+0x0C` | `inf` (type de ciblage) |
| `+0x10` | **niveau appris** |
| `+0x14` | **coût en SP** |
| `+0x18` | **`upgradable`** (1 = peut encore monter) |
| `+0x1C` | **portée** |
| `+0x30` | i16 — niveau « vérité serveur » (renseigné pour les skills du **joueur**) |
| `+0x34` | dérivé du niveau (`sub_739E20`) |

**Lecture live de contrôle** (Amistr, 4 compétences, `0x015FA428 == 4`) :

| id | Nom | Niveau | SP | `upgradable` |
|---|---|---|---|---|
| 8005 | `HAMI_CASTLE` | 5 | 10 | 0 |
| 8006 | `HAMI_DEFENCE` | 5 | 40 | 0 |
| 8007 | `HAMI_SKIN` | — | — | — |
| 8008 | `HAMI_BLOODLUST` | — | — | — |

(le personnage de test est GM 999 — cf. [[user_test_character_gm_999]] — d'où les 49 points
non dépensés et les niveaux au maximum.)

---

## 5. Ouverture / raccourcis

`UIWindowMgr_DispatchHotkeyBehavior` (`0x00A451E0`) :

| Comportement | Garde | Action |
|---|---|---|
| `123` | — | bascule la fenêtre `0x58` (familier) |
| **`124`** | **`dword_15FF9B4 != 0`** | **bascule la fenêtre `0x71` = 113** ← **Alt+R** |
| `125` | `dword_15FFA50 != 0` | bascule `0x7D` = 125 (mercenaire) |
| `126` | `dword_15FF9B4 != 0` | rappeler l'homoncule : cherche l'acteur `dword_15FF918`, `sub_A337F0` |

Le menu contextuel d'entité offre aussi les ordres à l'homoncule (clic droit + Alt) —
cf. [[project_entity_context_menu]] et `docs/entity_context_menu_re.md`.

**Deux gardes, deux sens** :
- `dword_15FF9B4` (« un homoncule est invoqué ») garde le **raccourci** ;
- `dword_15FF95C != -1` (classe connue) garde `MakeWindow(113)` lui-même.

Un onglet ImGui doit tester `dword_15FF9B4` pour décider s'il s'affiche, et `dword_15FF95C`
avant de dessiner quoi que ce soit de dépendant de la classe (nom de job, sprite).

---

## 6. Table de référence des adresses

```
── Fenêtre d'état (UIHomunInfoWnd, id 113) ──────────────────────────────────
0x010309D8  vtable                       0x0086BA40  ctor
0x0086E2E0  dtor                         0x00875620  OnCreate
0x0087E7B0  OnRender                     0x00884250  OnMsg
0x008896B0  vt+0x2C (copie du rect)      0x0131F8A8  instance vivante
0x0102B5E0  vtable UIBarGraphHomun       0x00834630  ctor UIBarGraphHomun
0x0088A010/30/50/70  lambdas PV/PVmax/SP/SPmax

── Arbre de compétences (UISkillListWnd, id 114 / 126) ──────────────────────
0x0103CB18  vtable                       0x00935290  ctor
0x00936C50  dtor                         0x00943150  OnCreate
0x00959890  OnMsg                        0x0131F8AC  instance vivante
0x00D80810  skill homoncule par index    0x00D80B30  idem mercenaire
0x00D5A570  niveau choisi (homun)        0x00D77070  poser le niveau (homun)

── Décodeurs réseau ─────────────────────────────────────────────────────────
0x00CD1B50  ZC_PROPERTY_HOMUN 0x022E     0x00CD20D0  … 0x0B2F
0x00CA7841  … 0x0B76 (inline)            0x00CD1ED0  … 0x0BA4  ★ actif
0x00CA7AB7  ZC_HO_PAR_CHANGE 0x0BA5      0x00CA7A1A  … 0x07DB
0x00CBA900  ZC_CHANGESTATE_MER 0x0230    0x00CD71B0  ZC_HOSKILLINFO_UPDATE 0x0239
0x00CA3184  ZC_HOSKILLINFO_LIST 0x0235

── Émetteurs ────────────────────────────────────────────────────────────────
0x00C86740  CMode::SendMsg               0x00C930F0  sa table de saut (msg-1)
0x00C907EC  msg 184 → CZ 0x022D          0x00C9082D  msg 185 → CZ 0x0231
0x00C8D894  msg  67 → CZ 0x0112          0x00DA8870  auto-feed → CZ 0x02D8 type 3
0x00DA8740  accusé auto-feed (écrit 0x0160231C)

── Divers ───────────────────────────────────────────────────────────────────
0x00D8E8D0  Job_IsHomunculusId (6001-6016, 6048-6052)
0x00D8EA70  Session_IsOwnHomunculusAid (compare à g_UIWindowContextKey+0x5558)
0x00D5BB40  Job_GetDisplayNameOrResName  0x015FB348  table jobname (begin)
0x00D70D30  invalide la fenêtre 113      0x00D70DD0  notifie la fenêtre 114
0x00A451E0  UIWindowMgr_DispatchHotkeyBehavior (comportements 124 / 126)
```

---

## 7. Ce que Bourgeon en fait — remplacement des deux natives

L'onglet **Homoncule** de la feuille de personnage (`character_sheet`, Alt+F) **REMPLACE**
les fenêtres 113 et 114 quand l'interface moderne est active. Il n'apparaît dans la barre
d'onglets que lorsque `dword_15FF9B4 != 0` — la garde même du raccourci Alt+R.

**Câblage** (identique à celui de Status / Équipement / Grimoire / Guilde) :

1. `window_pos_tweaks.cc` intercepte `UIWindowMgr_MakeWindow` : à la création de `0x71` ou
   `0x72`, appelle `CharacterSheet::HandleReplacedNativeCreation(win, id)`.
2. Celui-ci masque la native (`+0x28 = 0`) **avant sa première frame**, puis route :
   même onglet déjà ouvert → on referme (c'est nous qui portons la bascule) ; sinon
   `OpenHomunTab()`.
3. `CharacterSheet::OnTick` **détruit** les deux ids (`UIWindowMgr_SaveRectAndCloseWindow`),
   hors frame ImGui.

🔴 **Détruire, pas masquer** — [[reference_native_window_toggle_router]]. Les deux chemins
d'ouverture (`UIWindowMgr_ToggleWindowById` 0x00812E60 pour les boutons,
`UIWindowMgr_DispatchHotkeyBehavior` 0x00A451E0 pour Alt+R) **ferment** la fenêtre si elle
existe et ne la créent que sinon : une native laissée vivante, même invisible, avalerait un
appui sur deux sans repasser par notre hook — et garderait le clavier
([[feedback_hidden_native_window_keyboard]]).

🔴 **Le devoir caché de la fenêtre 113 : le ménage après suppression.** Sa cmd 291
n'envoyait pas que le paquet — elle fermait 113 et 114, appelait
`GameMode_ClearBlock55dc` (six dwords d'ordre en `ctx+0x55DC`) et remettait
`g_Homun_Present` à **0**. Le serveur n'émet **aucun** signal dont le client tirerait ce
nettoyage. En la remplaçant, Bourgeon hérite des deux derniers gestes
(`rag::homun::NotifyDeleted()`, `src/ragnarok/homunculus.cc`) : sans eux, l'onglet resterait visible sur
un homoncule disparu et le client garderait un ordre en attente vers un acteur mort.

**Pourquoi c'est sans risque pour les données** ([[feedback_replaced_handler_hidden_duties]]) :
le décodeur `0x0BA4` écrit **tous** les globals d'abord et ne touche aux fenêtres que sous
`if (instance)` — `Homun_InvalidateInfoWnd` (0x00D70D30) et le ré-armement de la jauge EXP
testent `g_UIHomunInfoWnd_ptr`, la notification de skill teste `g_UIHomunSkillWnd_ptr`. Avec
les deux fenêtres détruites, ces branches sont simplement sautées ; l'état et la liste de
compétences restent tenus à jour par le réseau. Nous lisons la **liste source**
(`g_HomunSkillList_Head`, session+0x64), pas la copie interne de la fenêtre 114 (`this+0x12C`)
— celle-ci disparaît avec elle sans rien nous coûter.

Écarts assumés par rapport au natif ([[feedback_replace_native_fill_gaps]]) :

- la ligne EXP affiche **`exp / expNext` + pourcentage**, là où le natif imprime `expNext` seul ;
- l'état **« déjà renommé »** est visible et le bouton Renommer disparaît — le natif laisse
  cliquer, saisir, confirmer, puis le serveur refuse en silence ;
- le « + » d'une compétence n'apparaît que si `upgradable` **et** qu'il reste des points ;
  le natif laisse cliquer et `hom_skillup` jette la demande sans message ;
- la suppression demande le **nom retapé** (le natif se contente d'un Oui/Non alors que
  l'opération est définitive) ;
- l'ASPD est accompagnée de l'`amotion` brute en survol ;
- **MDEF porte son unité** (« 2 % »), seule des huit à la porter : c'est la seule dont le
  nombre transmis **est** directement le pourcentage appliqué (`ad.damage * (100 - mdef) / 100`,
  branche `magic_defense_type == 0`, `battle.cpp:6141`). DEF reste sans unité — c'est une
  somme mixte `DEF1 + VIT`, un « % » y serait un mensonge — et CRI n'est pas un taux du tout ;
- « Nourrir » est grisé quand l'homoncule est **au repos** (bit 1 des flags), cas où
  `hom_is_active` fait échouer la demande côté serveur.

Lancer une compétence passe par `rag::homun::LaunchSkill` (module partagé
`src/ragnarok/homunculus.*`, utilisé aussi par la barre de raccourcis) : il prend
l'`ItemSkillInfo` **par index**
dans la liste de l'homoncule (`HomunSkillList_GetAt` 0x00D80810) puis la cmd `0x71` du
dispatcher, exactement comme la fenêtre 114. ⚠ Le `SendUseSkill` générique de la feuille ne
convient PAS : son remplisseur (0x00D7FA90) ne connaît que le bundle du **personnage**, un id
8001+ y est « introuvable » et le repli lancerait la compétence **sur le joueur**.
