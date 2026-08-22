# Attaque à distance pour les armes de type *wand* — étude client + serveur

Comment donner aux baguettes (`W_STAFF` = 10, `W_2HSTAFF` = 23) une attaque de
base à distance « façon WoW » : portée d'arc, **munition = SP**, projectile
visible. Étude de faisabilité **suivie de l'implantation** — voir §9 pour ce qui
est livré et validé en jeu.

Client `20250716` (Moonlight-Destiny), base `0x00400000`, IDB
`2025-07-16_Ragexe_175220998_clientinfo.exe.i64` — relevé le 2026-08-21.
Serveur : fork rAthena `moonlight`, **pre-renewal**.

> 🔴 **Le résultat central** : le client sait **déjà** faire voler un projectile
> pour une arme, et il demande à **Lua** si l'arme concernée en tire. La liste
> `BowTypeList` (`weapontable.lub`) est éditable dans le GRF : y ajouter
> `WEAPONTYPE_ROD` suffit à faire tirer une baguette, **sans patcher l'exe, sans
> Bourgeon, sans WARP**. ✅ **Vérifié en jeu le 2026-08-21** : une flèche part
> bien à chaque coup de bâton. Toute la mécanique (portée, coût en SP, refus à 0 SP,
> arrêt de l'auto-attaque) est **100 % serveur** et repose sur un précédent
> rAthena déjà livré, `SU_SOULATTACK`.
>
> ⚠ **Le piège de ce sujet** : le projectile ne naît **pas** dans le traitement
> de `ZC_NOTIFY_ACT`. Lire `CActorSprite_ProcessDamageAction` et n'y rien trouver
> mène à la conclusion — fausse — que le client ne dessine aucune flèche. Il la
> dessine, deux maillons plus loin, depuis `CActorSprite_SetMotion`.

---

## 1. La chaîne réelle du projectile, côté client

| Étape | Adresse | Ce qui s'y passe |
|---|---|---|
| Réception `ZC_NOTIFY_ACT` | `0x00CC8190` `ZC_NOTIFY_ACT_Process` | Construit une `ACTSTRUCT` et envoie le **message 11** à l'acteur **attaquant**. Ignore le paquet si `damage == -30000` (`0x8AD0`). |
| Dispatch | `0x00C4AEA0` `Actor_OnMsg_AppearanceEffects`, case 11 en `0x00C4D095` | La table d'indirection en `0x00D3D5D4` renvoie l'index `0x10` (défaut) pour le message 11. |
| Traitement du coup | `0x00C5DFC0` `CActorSprite_ProcessDamageAction` (appelée en `0x00C4D0A7`) | `SetMotion(2)` + `CActorSprite_SetFacingTowardXZ(cible)`, puis empile un message **différé** dans la file de la **cible**. **Ne crée aucun projectile.** |
| **Naissance du projectile** | `0x00D41DF0` `CActorSprite_SetMotion`, case 2, site `0x00D4239C` | Résout la classe d'arme, **appelle Lua `IsItemUsingArrow`**, et si vrai appelle la fabrique ci-dessous. |
| Fabrique | `0x00C6D9D0` `Arrow_SpawnProjectileToTarget` | `operator_new(0x170)` → `CArrowEffect`, push dans la `SceneNodeList`, message **14** avec `(targetGid, durée, flags, ammoId)`. |
| Le projectile | `0x00DB01B0` `CArrowEffect_OnMsg` (vtable `0x0109C3E8`) | Choisit le sprite **selon le job de l'attaquant** (cf. §3), vise `Actor_GetWorldPositionByAID(cible)`, part 10 unités au-dessus des pieds. |

Le code décisif, dans `CActorSprite_SetMotion` case 2 :

```c
v30 = this[272];                              // item id de l'arme droite
if (v30 > 105) v30 = Weapon_ItemIdToWeaponClass(v30);   // 0x00D8A1D0
strcpy(v31, "IsItemUsingArrow");
Lua_CallGlobal_va(g_pLuaStateMgr, v31);       // signature "d>b"
if (v59 == 1) {                                // l'arme tire des projectiles
    v32 = this[22] * this[222];                // durée d'animation × facteur
    Arrow_SpawnProjectileToTarget(this + 4, this[147], (char)v32, 0);
}                                              // this[147] = +0x24C = GID de la cible
```

`this[147]` (`+0x24C`) est écrit juste avant par `CActorSprite_ProcessDamageAction` :
c'est le GID de la cible extrait du paquet. Le projectile a donc sa destination
sans qu'aucun paquet supplémentaire soit nécessaire.

### La file de messages différés

`CActorSprite_ProcessDamageAction` n'applique pas les dégâts : il empile un nœud
de 56 octets dans la liste **`acteur+0x2A8`** de la cible (`ActorMsgQueue_PushBack`
`0x00C5A920`), avec une **date** `timeGetTime() + délai`.
`ActorMgr_FlushTimedMessageQueue` la vide à chaque frame et **rejoue** le message
stocké (26, 39, 93, 94 ou 148 selon le type de coup).

🔴 **Ce délai contient déjà le temps de vol du projectile — pour tout le monde,
joueurs compris.** Mesuré, pas déduit :

```
SetMotion (0x00D4243C)   attente = [acteur+0x58] × [acteur+0x378]        (en pas de 24 ms)
          (0x00D42462)   puis  [+0x378] += 8 / [+0x58]     <- SEULEMENT si l'arme tire
ProcessDamage (0x00C5E1A0)
                         base_ms = [+0x378] × [+0x58] × 24.0
                                 = (attente + 8 pas) × 24 = attente_ms + 192 ms
```

L'accumulateur `+0x378` avance de 8 pas à chaque tir, et **seulement sur le
chemin `IsItemUsingArrow`** : une arme de mêlée ne le touche pas. Les 192 ms
ainsi ajoutés sont exactement le vol natif d'un `CArrowEffect`.

S'y ajoutent des **rattrapages par job**, pour les mobs dont le projectile est
plus lent encore que la norme :

| Job | Rattrapage |
|---|---|
| 1016 (Skel Archer), 1420 | +192 ms |
| 1285, 1830 | +912 ms |
| 1286, 1829, 1287 | +408 ms |

C'est ce mécanisme, et ce site exact, que Bourgeon complète pour la boulette
ralentie (§11.6).

---

## 2. 🎯 Le levier : `BowTypeList` (Lua, dans le GRF)

`IsItemUsingArrow` n'est pas dans l'exe : c'est une fonction Lua globale, en
clair dans le GRF du client.

`data\luafiles514\lua files\datainfo\weapontable_f.lub` :

```lua
function IsItemUsingArrow ( type )
	for k, val in ipairs( BowTypeList ) do
		if ( type == val ) then return true end
	end
	return false
end
```

`data\luafiles514\lua files\datainfo\weapontable.lub:322` :

```lua
BowTypeList = {
	Weapon_IDs.WEAPONTYPE_BOW,        -- 11
	Weapon_IDs.WEAPONTYPE_CrossBow,
	Weapon_IDs.WEAPONTYPE_Arbalest,
	Weapon_IDs.WEAPONTYPE_Kakkung,
	Weapon_IDs.WEAPONTYPE_Hunter_Bow,
	Weapon_IDs.WEAPONTYPE_Bow_Of_Rudra
}
```

`WEAPONTYPE_ROD = 10` (ligne 18) et `WPCLASS_TWOHANDROD = 23` (ligne 31) n'y
figurent pas. **Les y ajouter fait tirer les baguettes.**

Effets de bord **mesurés** : `BowTypeList` et `IsItemUsingArrow` n'ont aucun
autre usage dans tout le dépôt Lua du client, et l'exe n'appelle
`IsItemUsingArrow` qu'au seul site `0x00D4239C`. L'impact est entièrement
contenu.

⚠ **Ce que la liste attend, c'est un *view id* d'arme, pas le sous-type
rAthena.** Le site d'appel fait `if (v30 > 105) v30 = Weapon_ItemIdToWeaponClass(v30)` :
au-delà de 105 c'est un item id à convertir, en deçà c'est déjà une classe
d'arme. Les baguettes ont donc **douze** valeurs possibles, pas deux — les deux
types de base plus les dix sous-types qui se replient sur eux
(`weapontable.lub:256-259` et `283-288`) :

| Valeur | Constante |
|---|---|
| 10 | `WEAPONTYPE_ROD` |
| 23 | `WPCLASS_TWOHANDROD` |
| 69, 70, 71, 72 | `Arc_Wand`, `Mighty_Staff`, `Blessed_Wand`, `Bone_Wand` |
| 96, 97 | `Staff_Of_Soul`, `Wizardy_Staff` |
| 99, 100, 101, 102 | `FOXTAIL_BROWN`, `FOXTAIL_GREEN`, `CandyCaneRod`, `FOXTAIL_METAL` |

✅ **Essai du 2026-08-21** : les douze ajoutées à `BowTypeList`, fichier posé sur
le **disque** du client (`data\luafiles514\lua files\datainfo\weapontable.lub`,
le VFS résout disque d'abord) — **une flèche part bien à chaque coup de bâton**.
Le GRF n'a pas été touché ; supprimer le fichier annule l'essai.

### ✅ Chaîne complète validée en jeu, sans une ligne de C++

Second essai le même jour : `Range: 9` posé sur le `Short_Foxtail_Staff`
(`Id: 1681`, `db/import/items/item_db_weapon.yml`) — cobaye choisi parce que son
`Jobs: {}` le rend inéquipable par toute classe **sauf un GM**, donc sans effet
sur les joueurs connectés. Après `@reloaditemdb` : **la baguette tire à distance,
projectile visible, auto-attaque enchaînée comme un arc.**

Ce que cela prouve : le trio *portée + tir à distance + projectile* ne demande
**aucune modification de code**, seulement une donnée (`Range`) et une liste Lua.
Tout ce qui suit — coût en SP, `BF_LONG`, boulette au lieu de flèche — relève de
la conception, pas de la faisabilité.

⚠ Les deux modifications ci-dessus sont des **essais** : supprimer le `.lub` du
client et remettre `Range: 1` pour revenir à l'état d'origine.

> Source de vérité des Lua : `Moonlight-Client/data/luafiles514/lua files/`.
> Le client résout **disque d'abord, GRF ensuite** — un fichier posé dans
> `data\` du client l'emporte, ce qui permet de tester sans reconstruire le GRF.

---

## 3. Le sprite du projectile

🔴 **Correction d'une première lecture.** L'argument sur lequel `OnMsg` fait son
`switch` avait d'abord été pris pour un **id de munition**. C'en est un tout
autre : c'est le **job / mob id de l'attaquant**. Les valeurs du `switch` le
disent d'elles-mêmes — 1410 `LIVE_PEACH_TREE`, 1495 `STONE_SHOOTER`, 1498
`WOOTAN_SHOOTER`, 4217-4221 Doram sont des entrées de `mob_db`, pas d'`item_db`.
Chaque **tireur** du jeu a donc son projectile propre, et c'est cet argument-là
qu'il suffit de forcer.

`CArrowEffect_OnMsg` (message 14) choisit donc le sprite **d'après le job du
tireur** :

* hors de `[0x582, 0x56A0]` (1410–22176), ou mot de poids fort non nul →
  **chemin par défaut** ;
* sinon, un `switch` sur le job : 1500/1555 → `PARASITE_BULLET.spr`, et 1410,
  1412, 1495, 1498, 1531, 1550, 1664-1667, 1688, 1865, 2021, 2364, 2475, 3882,
  4217-4221, 4308, 4315, 20274/20275/20279, 20368, 20575, 20851, 21520, 21589,
  21970, 21985, 22001, 22175-22176 ont chacun le leur.

`CActorSprite_SetMotion` passe **0** pour un joueur : hors plage, donc flèche.

Chemin par défaut, décodé depuis `0x0109CFC8` / `0x0109CFE8` (CP949) :

```
몬스터\skel_archer_arrow.spr
몬스터\skel_archer_arrow.act
```

C'est **le sprite de flèche standard** — celui des archers à flèches ordinaires
comme du mob Archer Squelette (d'où le `+192 ms` du job 1016). Un joueur, quel
qu'il soit, tombe forcément dessus.

🔴 **Le remplacer dans le GRF changerait donc l'apparence de toutes les flèches
du serveur.** La bonne prise n'est pas le fichier mais l'argument : forcer le job
donne **son** projectile à la baguette sans rien toucher d'autre. Voir §7 et §11.

---

## 4. L'animation d'attaque

`CActorSprite_ProcessDamageAction` appelle toujours `SetMotion(2)`, mais le
choix de l'**action du `.act`** se fait ensuite dans
`Weapon_ResolveAttackActionVariant` (`0x00D72D30`) :

* `Weapon_ClassToAnimBucket` (`0x00D5DDB0`) réduit la classe d'arme à un *bucket* ;
* un `switch` **par job** décide alors **action 2** ou **action 9**.

Exemples relevés : les jobs archers (4007 Archer, 4012 Hunter, 4018, 4020,
4021, 4029, 4034, 4040…) passent en action 9 quand le bucket vaut **11** (arc).
Les tables de saut associées sont `jpt_D72F11`, `jpt_D72F69`, `jpt_D72F8D`.

Conséquence pour le projet : **la baguette garde son geste de bâton**. Forcer la
pose d'archer imposerait de changer la classe d'arme du sprite, ce qui
changerait aussi **l'arme dessinée dans la main**. Garder le geste
d'incantation est de toute façon plus proche du modèle visé.

---

## 5. Ce que le client ne décide pas : la portée

Le client n'a **aucune** notion de portée d'attaque propre. Il n'émet que
`CZ_REQUEST_ACT` (`0x0437` côté tampon client, construit en `0x00C8F807`) et
c'est le **serveur** qui fait marcher le personnage vers sa cible
(`unit_attack` → `unit_walktobl`). La portée descend par `ZC_ATTACK_RANGE`
(`0x013A`), émis par `clif_updatestatus(SP_ATTACKRANGE)`.

➡ **Rendre une wand « longue portée » ne demande aucun patch client.**

---

## 6. Côté serveur : la mécanique

### 6.1 Le précédent à copier : `SU_SOULATTACK`

rAthena possède déjà une arme de mêlée qui frappe à distance sans munition.
Trois sites, c'est tout le mécanisme :

| Rôle | Fichier:ligne |
|---|---|
| Portée ajoutée au recalcul | `status.cpp:4526-4527` |
| SC infini auto-posé | `status.cpp:5005-5006` |
| Bascule en `BF_LONG` | `battle.cpp:5348` |

### 6.2 🔴 Ne **pas** détourner `arrow_atk`

Le réflexe « je mets `sd->state.arrow_atk = true` pour les baguettes » casse
trois choses :

1. `pc.cpp:1896` — avec `ammo_check_weapon: yes` (confirmé dans
   `conf/import/battle_conf.txt`), **aucune flèche n'est équipable avec un
   bâton** ⇒ `equip_index[EQI_AMMO] < 0` ⇒ `battle.cpp:7208` renvoie `ATK_NONE`
   dès le premier coup : le joueur ne peut plus attaquer du tout.
2. `battle.cpp:943` — `battle_calc_cardfix` bascule sur une branche « Ranged »
   qui **ignore la main gauche**.
3. `skill.cpp:1622` — `rate = (!sd->state.arrow_atk) ? it.rate : it.rate / 2` :
   **tous les procs de cartes divisés par deux**.

Il faut un drapeau dédié dans `special_state` (et **pas** dans `state` :
`pc.hpp:389` explique que `state` survit à `status_calc_pc` et deviendrait
rance au changement d'arme).

### 6.3 Les six points d'ancrage

| # | Quoi | Où |
|---|---|---|
| 1 | Drapeau `special_state.wand_ranged` calculé depuis le weapontype | `pc.hpp:494` / `status.cpp` après 4527 |
| 2 | Portée `+N` sur `W_STAFF`/`W_2HSTAFF` | `status.cpp:4509-4527` (patron `AC_VULTURE`) |
| 3 | `BF_LONG` conditionnel | `battle.cpp:5348` |
| 4 | Refus si SP insuffisant → `return ATK_NONE` | `battle.cpp:7203` |
| 5 | Prélèvement `status_charge(src, 0, cost)` | `battle.cpp:7431` |
| 6 | Visuel serveur (voie B, §7) | `battle.cpp:7449` |

Ce qui vient **gratuitement** :

* **l'arrêt de l'auto-attaque** : sur `ATK_NONE`, `unit.cpp:3341-3343` sort
  avant de reprogrammer le timer — commentaire d'origine « *Applied when you're
  unable to attack (e.g. out of ammo)* ». La cible reste sélectionnée ;
* **la resynchro du SP** : `status_charge` → `status_damage` → `pc_damage`
  (`pc.cpp:10190`) émet déjà `clif_updatestatus(SP_SP)`. Ne pas l'appeler ;
* **la portée poussée au client** : `status.cpp:6685` détecte le changement et
  émet `ZC_ATTACK_RANGE`.

Pièges :

* `status_zap` **écrête** au lieu de refuser → le joueur tirerait à 0 SP.
  Utiliser `status_charge`, qui est un test-et-prélèvement atomique
  (`status.cpp:1468`) ;
* un coût de **0 est interprété comme un échec** (`status.cpp:1548`) : borner à ≥ 1 ;
* en **dual-wield**, `sd->status.weapon` vaut `W_DOUBLE_*` et **pas** `W_STAFF` :
  un gate écrit dessus échoue en silence alors que la portée, elle, augmente
  bien (`status.cpp:4231` fait `rhw.range = max(rhw, lhw)`) ;
* le db d'items actif **n'est pas** `db/pre-re/` : les imports y sont commentés,
  seul `db/import/items/*.yml` est chargé (147 bâtons : 110 `Staff`, 37 `2hStaff`).

### 6.4 ✅ Vérifié : les skills de mêlée ne suivent pas

`skillrange_from_weapon: 0` dans `conf/import/battle_conf.txt:467` — et c'est ce
fichier qui a le dernier mot. Les 10 skills à `Range: -1` (`SM_BASH`,
`MC_MAMMONITE`, `TF_DOUBLE`, `RG_BACKSTAP`, `MO_TRIPLEATTACK`…) **restent à
portée 1**. Ne pas y toucher.

---

## 7. Les trois voies pour le visuel

| Voie | Geste | Résultat |
|---|---|---|
| **A** — `BowTypeList` + hook Bourgeon sur les **sites d'appel** de `Arrow_SpawnProjectileToTarget` (`0x00C6D9D0`), forçant le **job** passé en dernier argument quand l'arme est un bâton | ~30 lignes de DLL + 1 ligne Lua | Projectile **natif** (profondeur, occlusion, trajectoire correctes) avec le sprite d'un tireur du jeu. **Retenue et livrée** — cf. §11. Aucun sprite à charger, aucun chemin à réécrire. |
| **B** — `clif_skill_damage(..., MG_SOULSTRIKE, ...)` au lieu de `clif_damage` | 1 ligne serveur | Boulette native `EffectUpdate_SoulStrike` (`0x00BD3030`, sprite `이펙트\particle5`, son `effect\EF_SoulStrike.wav`). Aucun nom de skill affiché (vérifié : le handler du message 37 n'appelle que des effets). Mais les dégâts prennent la présentation « skill » et le son part à chaque tir. |
| **C** — `BowTypeList` seul | 1 ligne Lua | Marche immédiatement, mais la baguette tire une **flèche**. Idéal pour valider la mécanique avant de s'occuper du rendu. |

---

## 8. Décisions restées ouvertes

1. **`BF_LONG` ou non ?** Le poser rend **72 monstres** `IgnoreRanged: true`
   totalement immunisés (`battle.cpp:2911`) — 72 mobs *distincts*, et non les 111
   lignes du fichier, qui comptent trois fois les mêmes plantes : `IgnoreMelee`,
   `IgnoreRanged` et `IgnoreMagic` portent **exactement sur les mêmes 72**. Le
   poser met aussi les tirs sous Pneuma
   (`battle.cpp:1475`) et Neutral Barrier, et fait passer les bonus de
   `short_attack_atk_rate` à `long_attack_atk_rate`. Ne pas le poser laisse une
   incohérence : on tire à 9 cases mais le jeu traite le coup comme du
   corps-à-corps.
2. **Dégâts physiques ou MATK ?** En pre-renewal, `BDMG_ARROW` n'est posé que
   pour `W_BOW` et les armes à feu (`battle.cpp:4157`) : une baguette n'a ni
   l'ATK de la munition ni le bonus de DEX de l'arc, pour un ATK de base de
   l'ordre de 15-25. Un tir physique sera donc un *filler* très faible —
   fidèle au modèle visé, mais à assumer.
3. **Coût en SP et ASPD** : à chiffrer **ensemble** (`db/pre-re/job_aspd.yml`,
   entrée `Staff`). Aucune infrastructure n'existe : ni champ de `skill_db`, ni
   `battle_config`.
4. **Portée effective** : bornée à `AREA_SIZE` = 18 par `battle_check_range`
   (`battle.cpp:8276`), et `distance_client` (`path.cpp:504`) retire 0,1 avant
   troncature — les lignes droites portent une case plus loin que les
   diagonales.
5. **Quelles baguettes ?** 18 Foxtail ont `Jobs: {}` (inéquipables par
   quiconque) et 2 n'ont aucune clé `Jobs` (donc **toutes** les classes, un
   Knight compris). À arbitrer avant d'attacher un comportement au sous-type.

---

## 9. ✅ Ce qui est implanté et validé en jeu (2026-08-21)

**Client** — `weapontable.lub` posé sur le disque du client, `BowTypeList` enrichie
des 12 classes d'arme de type bâton. Le GRF n'est pas touché.

**Serveur** — 6 hunks, patron `SC_SPELLFIST` :

| Fichier | Contenu |
|---|---|
| `battle.hpp` | champs `wand_shot_sp_cost`, `wand_shot_skill_id`, `wand_shot_matk_rate` |
| `battle.cpp` | prédicat `battle_is_wand_shot` (gate unique) |
| `battle.cpp` | refus `ATK_NONE` + `USESKILL_FAIL_SP_INSUFFICIENT` quand le SP manque, en tête de `battle_weapon_attack` |
| `battle.cpp` | dégâts magiques : `battle_calc_attack(BF_MAGIC, …)` dont le résultat **écrase** `wd.damage` |
| `battle.cpp` | trois entrées dans la table `battle_data` |
| `skill.cpp:413` | garde `nullptr` dans `skill_get_range2` (cf. ci-dessous) |
| `conf/import/battle_conf.txt` | `wand_shot_sp_cost: 3`, `wand_shot_skill_id: 192` |

Résultat mesuré : la baguette tire à 9 cases, le projectile vole, les dégâts
suivent le **MATK**, 3 SP partent par coup et l'auto-attaque s'arrête proprement
à court de SP, cible conservée.

### 🔴🔴 Le piège qui a coûté un crash : `skill_id = 0`

`battle_calc_attack(BF_MAGIC, …, skill_id = 0, …)` **fait tomber le serveur** :
`skill_get_range2` (`skill.cpp:413`) fait `skill_db.find(id)->inf2` **sans garde**
et `find(0)` rend un pointeur nul. Le `default:` du switch rend bien 100 % du
MATK, mais on n'y arrive jamais.

Le signal était présent **avant** le crash : le seul appel `BF_MAGIC` de rAthena
(`SC_SPELLFIST`) passe un **vrai** skill id. Autrement dit, `skill_id = 0` n'est
jamais exercé sur ce chemin — et *un chemin que l'upstream n'exerce jamais n'est
pas un chemin sûr*.

Remède : passer un vrai skill via `wand_shot_skill_id`, par défaut **192
`NPC_MAGICALATTACK`** (`Type: Magic`, `HitCount: 1`, `Element: Weapon` — donc
l'élément de la baguette ; son unique implémentation C++ est `castendDamageId`,
que nous n'appelons pas ; aucun `case` ni `modifyDamageData` ⇒ `default:` ⇒
100 % du MATK). Plus une garde `skill_db.find(id) == nullptr` qui retombe en
physique avec un avertissement émis **une seule fois**.

⚠ La même ancre non gardée existe à **4 endroits** de `skill.cpp` ; un seul a été
corrigé (celui de la pile d'appels).

### 🔴 Le verdict physique survivait au remplacement des dégâts

Le hunk remplace le **nombre**, pas le **verdict**. `battle_calc_attack(BF_WEAPON, …)`
tourne d'abord en entier, jet de critique compris, et pose
`wd.type = DMG_CRITICAL` (`battle.cpp:5471`, **seul site**) ; on n'écrasait
ensuite que `wd.damage`. Le client affichait donc des **coups critiques sur des
dégâts magiques**, alors qu'en pre-renewal la magie ne critique pas.

🔴 **Et ce n'était pas qu'un problème d'affichage.** Un critique, dans rAthena,
ne fait pas que se dessiner en jaune : il **ignore le flee** (*« crits always hit
on official »*, au site d'appel). Corriger `wd.type` après coup aurait remis
l'affichage d'aplomb en laissant le tir toucher à coup sûr dès qu'on a du CRIT —
une correction qui cache le symptôme et garde la cause.

Le jet est donc coupé **à la source**, dans `is_attack_critical`
(`battle.cpp:3054`), borné à `skill_id == 0` pour qu'un skill physique lancé
bâton en main garde son comportement normal :

```cpp
if (skill_id == 0 && battle_is_wand_shot(BL_CAST(BL_PC, src)))
    return false;
```

C'est ce qui a obligé à remonter `battle_is_wand_shot` **avant**
`is_attack_critical` : elle est désormais lue aux deux bouts du fichier.

⚠ Conséquence assumée : le tir perd le « touche à coup sûr » que lui offrait le
critique. Il se règle maintenant sur le seul jet de toucher physique — donc **il
peut être esquivé**. Le passer au régime magique (qui, lui, touche toujours en
pre-renewal) serait un autre changement, à décider séparément.

*(rAthena a le même angle mort sur `SC_SPELLFIST`, dont ce hunk est copié : lui
non plus ne coupe ni ne corrige le critique.)*

### Réglages à chaud

Les trois réglages sont de vrais `battle_config` : `@reloadbattleconf` les relit
**sans redémarrer ni recompiler**.

| Réglage | Défaut | Rôle |
|---|---|---|
| `wand_shot_sp_cost` | 3 | SP par tir. **`0` éteint entièrement la fonctionnalité** |
| `wand_shot_skill_id` | 192 | gabarit du calcul magique (`NPC_MAGICALATTACK`) |
| `wand_shot_matk_rate` | 20 | puissance, en % du MATK plein |
| `wand_shot_damage_delay` | 0 | ms avant que les PV ne tombent — le temps de vol de la boulette. À tenir d'accord avec le client (§11.7) |

### ⚖ Pourquoi 20 % et pas 100 %

Mesure du 2026-08-21 : `Staff` a l'ASPD de base la plus lente du Mage
(`job_aspd.yml` : 700, contre 500 au poing), mais à **100 % du MATK** un wizard à
500 MATK sortait ~400 DPS **en continu, sans cast, sans interruption possible et
pour ~2,4 SP/s** — soit les trois quarts du DPS d'un Fire Bolt niveau 10, qui
coûte lui 40+ SP et s'interrompt. Le tir ne remplaçait plus un filler mais les
sorts de bas niveau.

À 20 % (~80 DPS) il redevient ce qu'il doit être : le geste qu'on fait quand on
n'a plus de SP. Et comme le ratio se règle à chaud, il est bien plus simple de
le monter que de reprendre une puissance à laquelle les joueurs se seraient
habitués.

---

## 10. Fonctions nommées dans l'IDB au cours de cette étude

| Adresse | Nom posé |
|---|---|
| `0x00C6D9D0` | `Arrow_SpawnProjectileToTarget` |
| `0x00DA9E50` | `CArrowEffect_ctor` |
| `0x00DB01B0` | `CArrowEffect_OnMsg` |
| `0x00D72D30` | `Weapon_ResolveAttackActionVariant` |
| `0x00D5DDB0` | `Weapon_ClassToAnimBucket` |
| `0x00C5A920` | `ActorMsgQueue_PushBack` |

Commentaires posés en `0x00D4239C`, `0x00C6D9D0`, `0x00DB01B0`, `0x00D72D30`
et `0x00C4D0A7`.

Passe « vol du projectile » (2026-08-21) — ces quatre-là portaient déjà leur
nom ; ce sont des **commentaires** qui ont été posés :

| Adresse | Fonction | Ce que dit le commentaire |
|---|---|---|
| `0x00DABB10` | `CArrowEffect_Update` | le modèle de vol en entier, et la prise pour le ralentir |
| `0x00C40C20` | `Actor_ComputeLayerQuad` | rotation = acteur `+0x7C` + couche du `.act` ; miroir = `param_4[3] & 1` |
| `0x00DB01B0` | `CArrowEffect_OnMsg` | convention d'appel (`retn 34h`), messages 14 **et** 64, champ libre `+0x16C` |

`0x00C40AC0` `CActorSprite_SetFacingTowardXZ` et `0x00C431A0`
`Camera_ComputeRelativeFacingDeg` sont citées dans ces commentaires : elles
n'avaient rien à ajouter en propre.

---

## 11. Le vol du projectile : vitesse et orientation

Une fois le sprite emprunté, il reste réglé **pour une flèche**. Les deux
corrections passent par la vtable de `CArrowEffect` (`0x0109C3E8`), dont les
slots 1 et 2 sont `Update` et `OnMsg` — deux écritures de quatre octets, aucun
code du client réécrit.

🔴 `CArrowEffect_Update` (`0x00DABB10`) n'a **qu'une seule xref** : ce slot.
Aucun appel ne peut donc contourner le détour.

### 11.1 Le modèle de vol, en entier

Tout tient dans une seule variable de temps :

```
t = (timeGetTime() - [+0x8C]) / 24.0        <- pas de 24 ms
```

et **tout** en dépend :

| Étape | Calcul | Champs |
|---|---|---|
| Attente avant départ | `si [+0x160] > t : ne rien faire` | `+0x160` (float), en **pas**, pas en ms |
| Vitesse, calculée **une fois** | `v = (cible − origine) × 0,125` | `+0x144/148/14C` |
| Plancher de vitesse | si `distance < 40` : `échelle = 40 / distance`, `v ×= échelle` | `+0x15C` (1.0 sinon) |
| Position, à chaque image | `origine + t × v` | `+0x10/14/18`, origine en `+0x138/13C/140` |
| Fin de vie | `t > 8 / échelle` | — |

Un vol nominal dure donc **8 pas, soit 192 ms** — pratiquement instantané. Le
plancher des 40 unités n'est pas un raccourci : sous cette distance, le
projectile avance à **5 unités par pas** et vit d'autant moins longtemps, ce qui
lui donne une vitesse constante quelle que soit la portée.

### 11.2 Ralentir : reculer le départ, pas toucher aux constantes

Puisque `t` est unique, le diviser ralentit la course **et** allonge la durée de
vie du même facteur — les deux moitiés ne peuvent pas se désynchroniser. Le
détour de `Update` recule donc `[+0x8C]` le temps de l'appel :

```
départ_prêté = maintenant − (maintenant − départ_réel) / facteur
```

puis restaure la vraie valeur. Aucun octet du client n'est modifié.

### ⛔ L'attente avant le départ, elle, ne suit PAS le facteur

`+0x160` se compte dans la même unité que `t` : le recul du départ la
rallongerait donc d'office. **Le code l'en défait explicitement**
(`/= kFacteurLenteur`), et cette décision a deux motifs mesurés — la première
version, qui laissait l'attente suivre la lenteur, a été essayée et rejetée en
jeu :

1. **Elle est déjà longue à faible ASPD.** Elle vaut `[acteur+0x58] ×
   [acteur+0x378]` (§1), donc l'animation elle-même : la multiplier faisait
   partir la boulette **après la fin du geste**.
2. **La formule des dégâts la suppose native.** Le client date le coup à
   `(attente + 8 pas) × 24 ms` : changer l'attente désynchronise le coup sans
   rien pour le rattraper.

Seule la **course** ralentit donc. Le départ reste au point du geste que le
client a choisi, à toutes les ASPD.

### 11.3 L'orientation : un projectile RO n'a qu'**une** image

C'est le point qui n'était pas acquis. Un `.act` de projectile ne contient pas
huit directions : `stone_shooter_bullet.act` fait **1 action, 1 frame, 1 couche**,
`rotation = 0`, `miroir = 1`. C'est le **moteur** qui fait pivoter l'image.

```
CArrowEffect_Update  : [+0x4C] = atan2(dx, −dz) en degrés     (CActorSprite_SetFacingTowardXZ)
                       [+0x7C] = Camera_ComputeRelativeFacingDeg([+0x4C])
                               = normalise(−angle_caméra − [+0x4C])
Actor_ComputeLayerQuad : rotation_du_quad = [+0x7C] + rotation_de_la_couche(.act)
```

`+0x7C` est donc une **rotation d'écran en degrés** (un entier dans `[0, 360)`),
recalculée à chaque image, et le rendu lui ajoute celle du `.act`.

➡ Un sprite dessiné selon un autre axe que la flèche générique arrive de
travers, et l'écart se rattrape par **un décalage constant en degrés**, appliqué
après le natif, sur le seul projectile marqué.

Deux endroits peuvent porter ce décalage :

* le **`.act`** (champ `rotation` de la couche) — additif, sans recompilation,
  mais il vaudrait aussi pour le mob `STONE_SHOOTER` lui-même ;
* le **code** (`kRotationDeg` dans `wand_bolt.cc`) — ne vaut que pour nous.

C'est le code qui a été retenu.

### 11.4 Reconnaître notre boulette : la marque en `+0x16C`

`Arrow_SpawnProjectileToTarget` alloue **0x170** octets, mais
`CArrowEffect_ctor` n'initialise que jusqu'à `+0x168`, et aucune méthode de la
classe ne lit `+0x16C` (vérifié : les neuf occurrences de ce déplacement dans
la famille appartiennent toutes à d'autres classes). Le dernier mot est donc
libre.

🔴 Libre, mais **pas nul** : `operator new` ne nettoie rien. C'est pourquoi le
détour de `OnMsg` écrit la marque **sans condition**, y compris à zéro, sur les
deux messages d'initialisation (**14** et **64**) — sans quoi du garbage pourrait
se relire comme une marque.

La convention d'appel de `OnMsg` vient du **désassemblage**, pas de Hex-Rays,
qui apparie mal les arguments 64 bits poussés en `cdq; push edx; push eax` :

```
retn 34h  ->  __thiscall, 13 arguments pile nettoyés par l'appelé
```

avec, en indices d'argument pile : `1` = message, `2` = sous-message, `9` = job,
`10` = mot de poids fort du job.

### 11.5 Note d'outillage

Le `data.grf` de ce client est **chiffré** (signature `Event Horizon`) : il est
illisible par `tools/grf_reader.py`, qui ne voit que `moonlight.grf`. Pour
inspecter un sprite du GRF il faut l'extraire d'abord avec GRFEditor, puis le
poser dans `data\sprite\…` — où le client le lira de toute façon en priorité.

### 11.6 Les dégâts attendent l'arrivée de la boulette

Reste le vrai décalage : le paquet d'attaque et l'animation partent ensemble, si
bien que le nombre de dégâts s'affiche **avant même que la boulette ne soit
sortie**.

Le client sait déjà faire — il diffère le coup de `attente + 192 ms`, soit
exactement le vol natif (§1). Mais notre boulette vole `192 × N` ms. Il manque
donc le **surplus**, et rien d'autre :

```
supplément = 192 × (kFacteurLenteur − 1)
```

Ajouter le vol entier décalerait de 192 ms de trop.

Le point d'insertion est le dernier calcul avant que l'échéance ne soit posée —
six octets, donc de la place pour `E8 rel32` + `nop` :

```
0x00C5E372   call dword ptr [__imp_timeGetTime]    FF 15 B0 17 FC 00
0x00C5E378   add  eax, [ebp+delai]
```

Rendre l'heure **plus** le supplément revient exactement à l'ajouter au délai.

🔴 **Aucun besoin de retrouver l'attaquant à ce site** : `ProcessDamageAction`
appelle `SetMotion(2)` en `0x00C5E17E` — donc notre détour de tir — **puis**
date le message en `0x00C5E372`. Même appel, même fil, ordre garanti. Un simple
jeton à usage unique, posé à la naissance de la boulette et consommé ici, suffit
et vaut aussi pour les tirs des **autres joueurs**.

### 11.7 ⛔ Ce qu'il ne faut PAS faire : différer les dégâts côté serveur

L'idée est séduisante, et elle a été essayée : puisque le client n'affiche que
plus tard, que le serveur retarde aussi l'application des PV — et le monstre ne
mourra plus avant l'impact. Tout semblait s'y prêter : `battle_delay_damage`
porte déjà un délai, la cadence d'attaque n'en dépend pas (le timer est réarmé
sur `status_get_amotion`), et le client ne voit aucune différence puisque
`clif_damage` a déjà émis `wd.amotion` tel quel.

**C'est faux, et ça coûte du SP.** Le coup suivant part à `amotion` ; la mort ne
s'enregistre qu'à `amotion + délai`. Tout délai strictement positif laisse donc
partir des swings sur un monstre **déjà mort** :

```
swings de trop = 1 + délai / amotion
```

Mesuré en jeu le 2026-08-22, bâton en main — l'ASPD la plus lente du jeu,
`amotion ≈ 600 ms` :

| `délai` | swings de trop | ce qu'on voit |
|---|---|---|
| 576 | 1 | une ligne « Quelqu'un subit N points de dégâts » par kill |
| 726 | 2 | deux lignes — la « marge de sécurité » de 150 ms avait **empiré** le symptôme |
| **0** | **0** | plus aucun coup sur un cadavre |

Chaque swing de trop coûte en plus **3 SP** et retient le joueur : il frappe un
monstre mort pendant toute la durée du délai, sans pouvoir enchaîner.

🔴 **La ligne « Quelqu'un » n'est que le symptôme, pas la maladie.** Il n'existe
aucun cas normal où l'on inflige des dégâts à une cible que le client ne résout
plus — ces coups n'auraient pas dû exister. Le repli lui-même est délibéré côté
Gravity : `ActorMgr_FlushTimedMessageQueue` résout le nom au moment du **rejeu**
et se rabat sur le msgstring **1604 `MSI_WHO_IS`**. Ce n'est pas un bug du
client ; c'est nous qui lui donnions un travail impossible.

➡ **Le décalage visuel se traite entièrement côté client** (§11.6), où différer ne
coûte rien : rien n'y dépend de l'instant où les PV tombent. Le hunk serveur, le
champ de `battle_config` et la clé de `battle_conf.txt` ont été **retirés** ; il
ne reste qu'un commentaire au site de `battle_delay_damage` pour que l'idée ne
soit pas réinventée.

⚠ Ce qu'on garde donc, en connaissance de cause : le monstre meurt environ
`192 × N` ms avant que la boulette ne l'atteigne, et **le dernier coup d'un kill
imprime encore « Quelqu'un »** — une ligne, pas trois. La seule correction propre
serait de fournir le nom nous-mêmes au moment du rejeu, en détournant le premier
lookup (`GameMode_CopyEntityName`) : Bourgeon connaît déjà le dictionnaire
d'entités (`GameMode+0x160`). Non fait.
