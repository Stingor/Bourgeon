# Zone des sorts au sol (ShowScale, 0x0A41, prévisualisation)

> Journal du chantier. La fiche de mémoire `project_skill_aoe_preview_showscale` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

**Le client sait DÉJÀ dessiner l'AoE d'un sort pendant son incantation.** Mesuré
le 2026-09-02, après que j'aie affirmé le contraire — l'utilisateur a demandé
« c'est sûr ? que fait `SquareRange.tga` dans le client ? », et c'était la bonne
question. ⚠ Ne JAMAIS repartir sur un chantier de RE ici : tout existe.

## La chaîne complète, vérifiée bout à bout

1. **Serveur** — `clif_skillcasting` (clif.cpp) finit par
   `if (skill_get_inf2(skill_id, INF2_SHOWSCALE)) clif_skill_scale(...)`.
   `INF2_SHOWSCALE` est commenté dans skill.hpp : *« Skill shows AoE area while
   casting »*. Gardé par `battle_config.show_skill_scale` (**déjà `yes`** dans
   `conf/battle/client.conf`).
2. **Le paquet** — `PACKET_ZC_SKILL_SCALE`, **opcode `0xA41` = 2625**
   (`packets_struct.hpp:269`, `skillscale = 0xA41`), 18 o :
   `AID, skill_id, skill_lv, x, y, casttime`.
   🔴 **PAS `0x08d2`** : `clif_packetdb.hpp` déclare bien `packet(0x08d2,10)`
   mais ce n'est pas celui-là — suivre l'enum, pas la table des longueurs.
3. **Client, réception** — `RecvLoop_DispatchPackets`, **`case 2625`** (les
   étiquettes de ce switch SONT les opcodes en décimal) → appelle
   **`EffectApply_SkillScaleEffect` 0x00cf8080** (son SEUL appelant).
4. **Client, la taille** — cette fonction appelle la **globale Lua
   `GetSkillScale`** (`Lua_CallGlobal_va`, signature **`"dd>dd"`** = skillId,
   skillLv → **x, y en cellules**), définie dans
   `data\luafiles514\lua files\newskillinfo\skillinfo_f.lub:272`, qui lit
   **`SKILL_INFO_LIST[skid].SkillScale[lvl] = { x = .., y = .. }`** dans
   `skillinfoz\skillinfolist.lub`.
5. **Client, le dessin** — boucle `x × y` et, par case,
   `EffectMgr_SpawnEffect(effet **1114**)` positionné par
   `Camera_ProjectTileToScreen(cx + i - x/2, cy + j - y/2)` puis
   `Terrain_GetHeightAt` ⇒ **ça épouse le relief**, centré sur la case visée.

**`effect\SquareRange.tga` est la texture de ce carreau** : classe
**`CSquareRangeEffect`** (RTTI confirmé, vtable **0x107549c**), son slot 1
`sub_AE6980` fait `EffectParticle_SetTextureByName("effect\\SquareRange.tga")`
et met l'échelle à `this[51] * 2.5`. (Le cas `this+1 == 991` y est une variante
grise de 300 ms.) ⚠ Le lien formel **effectId 1114 ↔ CSquareRangeEffect** n'a PAS
été prouvé — la fabrique passe par un `CEffectAllocator<T>` que je n'ai pas
remonté jusqu'à sa table d'ids. Tout le reste est mesuré.

## Pourquoi ça ne se voit jamais sur Moonlight

**Les deux bouts ne portent que des skills de boss.** `skillinfolist.lub` du
client n'a que **51 entrées `SkillScale`, toutes `NPC_*`** ; `skill_db.yml`
pre-re n'a que **24 `ShowScale: true`**, toutes `NPC_*` aussi. Aucun sort de
joueur. La fonctionnalité est donc entière et **éteinte par les données**.

⟹ **Pour allumer un sort, il faut l'ajouter AUX DEUX** — la table Lua donne la
taille, le flag serveur déclenche l'envoi. Un seul des deux ne fait rien, en
silence.

## Essai livré (2026-09-02) : Storm Gust

- Client : `SkillScale = { [1..10] = { x = 9, y = 9 } }` ajouté à
  `[SKID.WZ_STORMGUST]` dans `Moonlight-Client/data/.../skillinfoz/skillinfolist.lub`,
  copié dans `E:\Nouveau dossier\Moonlight-Destiny\data\...`.
- Serveur : `- Id: 89 / Name: WZ_STORMGUST / Flags: ShowScale: true` **en fin de
  `db/import/skill_db.yml`** (cf. [[feedback_rathena_conf_import_overrides]]).
  ✅ Sûr : l'entrée 89 existe déjà donc seul `Id` est requis, et
  `SkillDatabase::parseBodyNode` **fusionne** les flags un à un (`set`/`reset`)
  — `IsAutoShadowSpell` n'est pas perdu.

**D'où vient le 9×9** : `Unit.Layout: 4` du skill_db, et
`skill_init_unit_layout` construit les 5 premiers layouts en carrés de
`i*2+1` ⇒ index 4 = **9×9**. ⚠ Le `Unit.Range: 1` par unité pourrait élargir
l'aire RÉELLE : la table Lua est purement **cosmétique**, elle ne change pas ce
que le serveur touche. À confronter au visuel.

## 🔴🔴 POURQUOI ÇA NE MARCHE PAS POUR SES PROPRES SORTS — mesuré le 2026-09-02

**`EffectApply_SkillScaleEffect` commence par `Actor_FindByGid` (0x00d806a0), qui
ne trouve JAMAIS le joueur lui-même.** Elle sort donc à sa première ligne, en
silence, pour tout sort qu'ON lance.

C'était déjà écrit dans le projet, dans `party_frames.cc` : *« mon propre acteur
n'est PAS dans la liste que parcourt `FindActorByGid` : le natif le range à part,
en `actorMgr+0x2C` »*. C'est pour ça qu'il existe une seconde porte,
**`ActorMgr_FindByGidOrSelf` 0x00a69e70** (`__thiscall(mgr, gid)`), qui teste
`gid == g_Account_Aid` AVANT de parcourir quoi que ce soit.

⭐ **Cohérent avec les données** : kRO n'a mis `ShowScale` que sur des skills
`NPC_*`, lancés par des MVP. Le cas « moi » n'a jamais existé chez eux, donc
personne ne l'a jamais vu casser.

### Ce que le diagnostic a coûté, et ce qui l'a tranché

Trois builds. Le trait utile a été d'instrumenter **cast_bar.cc** avec
`RegisterObserveOpcode(0x0A41, 16)` puis de **refaire les deux premières étapes de
la native nous-mêmes** — `gamescene::FindActorByGid(aid)` et un appel Lua direct
à `GetSkillScale(id, lv)` via `lua::GetField/PushNumber/PCall`. Verdict en une
ligne :

    [DIAG] 0x0A41 aid=2000001 skill=669 lv=1 cell=(160,112) | acteur=NUL |
           GetSkillScale rc=0 -> (11,11) err=''

⇒ la donnée est PARFAITE des deux côtés (le Lua rend bien 11×11), seul l'acteur
manque. ⚠ **Sept vérifications statiques successives — toutes vertes — n'ont rien
donné** : PACKETVER, `battle_config`, l'import du skill_db, la casse du flag
(`strcasecmp`), la table de longueurs du client (`0x0A41` → `realLen 0x12`), le
dispatch (`case 2625`), le `.lub` réellement chargé. Leçon : quand tout est vert
et que rien ne marche, **instrumenter le CHEMIN D'EXÉCUTION** au lieu d'ajouter
une huitième vérification statique. Cf. [[feedback_re_method]].

🔴 **Piège de compilation croisé en route** : un `__try` dans
`CastBar::HandlePacket` échoue en **C2712** (la fonction construit des objets à
dérouler). L'appel Lua doit vivre dans sa propre fonction. Symptôme trompeur :
le build échoue, l'utilisateur recopie l'ANCIENNE dll, et le journal répète le
message précédent — on croit que le correctif n'a rien changé. ⭐ Vérifier le
**hash** de la dll construite et de celle déployée, pas leur date : la copie est
plus récente même quand son contenu est périmé.

### La réparation possible : CINQ OCTETS

Le `call Actor_FindByGid` fautif est à **`0x00cf80b2`**, et c'est un **site
unique**. Le rediriger vers une variante « ou moi »
(`GameMode_GetActive` → `mgr = *(gm+204)` → `ActorMgr_FindByGidOrSelf(mgr, gid)`)
ressuscite tout le mécanisme natif : effet 1114, `SquareRange.tga`, quad qui
épouse le relief. ⚠ Ne PAS détourner `Actor_FindByGid` elle-même : cinq fichiers
du projet et tout le natif l'appellent.

⭐ **Et les tailles n'obligent pas à toucher au GRF** : `SKILL_INFO_LIST[id].SkillScale`
peut être écrite à chaud depuis Bourgeon avec l'API `lua::` déjà en place, à
partir d'une table dérivée de `skill_db.yml`. Aucun `.lub` à redéployer.

## ✅ LIVRÉ le 2026-09-02 — trois pièces

1. **`src/features/fx/skill_range.{h,cc}`** — le correctif de CINQ OCTETS. Réécrit
   le `call` de `0x00cf80b2` (motif d'origine `E8 E9 85 08 00`, vérifié avant
   d'écrire) pour passer par `ActorMgr_FindByGidOrSelf` au lieu de
   `Actor_FindByGid`. Plugin `SkillRangePatch`, posé au démarrage, sans réglage —
   même patron que `BerserkChatUnlock`.
2. **`tools/gen_skill_scale.py`** — dérive les tailles de `db/pre-re/skill_db.yml`
   et écrit **les DEUX bouts depuis la même source**, pour qu'ils ne divergent
   pas : les blocs `SkillScale` du `skillinfolist.lub` du dépôt client (+ copie
   dans le client de test) et un bloc `ShowScale: true` encadré de marqueurs
   `# >>> bourgeon:showscale` dans `db/import/skill_db.yml`. **125 sorts**,
   dont 123 posés dans le lua (2 déjà renseignés par kRO, jamais écrasés).
3. Le drapeau serveur, donc, dans **`db/import/skill_db.yml`** — régénérable.

⭐ **Contrôle de la méthode** : les tailles dérivées retombent sur ce que la
communauté RO connaît — Storm Gust 9×9, Lord of Vermilion 11×11, Meteor Storm
7×7, Pneuma 3×3, Arrow Shower 5×5. C'est ce qui valide `2N+1`.

### Deux pièges du `.lub`, payés tous les deux

🔴🔴 **`skillinfolist.lub` a des fins de ligne MIXTES** : 16 135 LF contre
1 070 CRLF. Le test habituel `"\r\n" in src` y répond OUI et fait écrire des
lignes qui ne ressemblent pas à leurs voisines. ⇒ prendre la fin de ligne
**MAJORITAIRE** (`_eol()`). Et **ne PAS rétablir ce fichier par `git checkout`** :
git le normaliserait en CRLF d'un bout à l'autre.

🔴🔴 **Le champ qui précède n'a pas forcément de virgule.** Quand un bloc se
termine par `_NeedSkillList = { ... }`, sa dernière accolade est NUE ; y coller un
champ de plus donne un Lua qui ne se charge pas — et **rien ne le signale côté
client**, `SKILL_INFO_LIST` reste simplement vide, donc tous les noms de sorts
disparaîtraient. 117 virgules manquaient au premier jet. ⇒ avant d'insérer,
regarder le dernier caractère non blanc : s'il n'est ni `,` ni `{`, poser la
virgule.

## 🔴🔴 Le natif centre la zone sur le LANCEUR, pas sur la cible

Constaté en jeu sitôt le correctif d'acteur posé : les cases s'affichaient sous
les pieds du joueur. La cause est côté **serveur**, dans `clif_skillcasting` :

```c
clif_skill_scale( &src, src.id, src.x, src.y, skill_id, skill_lv, casttime );
```

— alors que le paquet d'incantation, lui, transmet bien `dst_x, dst_y`. Même
raison que le reste : les sorts `ShowScale` de kRO sont des `TargetType: Self` de
MVP, où source et cible coïncident. ✅ Corrigé (2026-09-02) : `dst_x/dst_y` s'ils
sont non nuls (sort visé au sol), sinon `dst->x/dst->y` si une cible est donnée,
sinon la source. ⚠ **Demande une RECOMPILATION du map-server**, pas un
`@reloadskilldb`.

## ✅ La PRÉVISUALISATION au ciblage — tout autre mécanisme

Demande de l'utilisateur : voir la zone **quand le sort est armé et attend son
clic**, pas seulement pendant l'incantation. 🔴 **Le natif ne peut pas** :
`ZC_SKILL_SCALE` n'est émis qu'au démarrage du cast, aucun paquet n'existe au
moment du ciblage. C'est donc purement local, et tout était déjà dans le projet :

- **`CGameMode+0x408`** mode de ciblage (**1 = sol**), **`+0x40c`** le sort armé,
  **`+0x414`** son niveau — les offsets que `quick_cast.cc` lit déjà ;
- **`GameMode_PickGroundCellUnderMouse` 0x00c69a40** (`__thiscall(gm, &x, &y)`,
  rend 0 si le curseur ne désigne pas le sol) pour la case visée ;
- **`GetSkillScale`** appelé par nous via `lua::` — ⭐ **la MÊME source que le
  natif**, donc prévisualisation et incantation ne peuvent pas se contredire ;
- le dessin par **`grey_world::AddScenePainter`** (voir ci-dessous).

⚠ Seul le **mode 1** est traité : un sort sur cible suit une entité qui bouge,
le montrer sous le curseur mentirait sur son centre.

### ⭐ GreyWorld est devenu le FOYER du dessin de cases au sol

`grey_world::AddScenePainter(fn)` : un module tiers s'inscrit, et reçoit à chaque
passe de scène une fonction `paint(cell_x, cell_y, argb)`. Il ne voit ni le sol,
ni la scène, ni la texture.

- **Pourquoi là** : peindre n'est possible que pendant
  `CScene::RenderCellsAndCursor`, et c'est GreyWorld qui la détourne. Reproduire
  `DrawCellShrunk` (50 lignes calquées sur un désassemblage, avec le piège du
  `__thiscall`) aurait été la faute.
- 🔴 **La texture est résolue UNE fois par passe** et le peintre ne la voit pas :
  la lui confier reviendrait à lui confier l'addref qui déborde.
- 🔴 **S'inscrire appelle `EnsureInstalled()`** — sans quoi la peinture d'un
  module tiers dépendrait du réglage `greyworld`, qui ne la concerne pas. Les
  détours sont inertes tant que GreyWorld est éteint.
- ⚠ `FlattenActive()` est définie APRÈS dans le fichier ⇒ déclaration anticipée.
- ⚠ Boucle par INDEX et non par itérateur dans le bloc `__try` (C2712).

🔴 **Piège Lua** : `PCall(L, 2, 2, 0)` laisse **2** valeurs en cas de succès mais
**1** (l'objet d'erreur) en cas d'échec. Dépiler 2 dans les deux cas entamerait
la pile de l'appelant. Le patron `CallGlobal1Arg` de `lua.h` ne couvre pas ce cas
(un seul argument, un seul résultat).

## Les réglages (section Gameplay > « Zone des sorts »)

Clés `skillrange_*` dans `bourgeon_settings.yaml`, même patron que GreyWorld
(`kSkillRangeSettings` dans moonlight_ui.cc, agrégat libre + `skill_range::cfg()`).

| clé | défaut | ce que ça fait |
|---|---|---|
| `skillrange_preview` | **false** | la zone du sort ARMÉ, sous le curseur |
| `skillrange_pattern` | carrelage | anneau / carrelage / carreau plein |
| `skillrange_gap` | 12 | joint, carreau plein seulement |
| `skillrange_color` | `0x80FFFFFF` | couleur ET opacité |
| `skillrange_players` | **false** | la zone d'INCANTATION des JOUEURS |

⭐ **LE FILTRE « JOUEURS » NE COÛTE RIEN** parce que notre remplaçant de
`Actor_FindByGid` est le **passage obligé** de la native : y rendre `nullptr` la
fait sortir à sa première ligne, ce qui EST son comportement d'origine. Ce n'est
donc pas un masquage — rien n'est créé puis caché.

🔴 **LES MONSTRES NE SONT DANS AUCUN RÉGLAGE** (décision utilisateur) : leur zone
marche nativement depuis toujours, et un MVP qui annonce son sort est une
information de jeu. Le filtre teste donc `rag::IsMonsterJob(acteur+kJobId)` et
laisse passer.

⚠ Les deux bascules sont ÉTEINTES par défaut : montrer la zone d'un sort change
ce que le joueur voit du combat.

⭐ **Le peintre déclare son STYLE à chaque passe** (`grey_world::PainterStyle` :
texture, uv, shrink). `texture == nullptr` = éteint, et c'est la porte de sortie
la moins chère : ni résolution de texture, ni appel du peintre. C'est ce qui rend
`skillrange_preview` gratuit quand il est faux.

🔴 **i18n à la fin du chantier** : `tools\lang\check_catalog.ps1` a signalé les
9 nouvelles chaînes. ⭐ Les clés ont été **extraites du source** par la regex de
`extract_tr.ps1` et non retapées — une apostrophe courbe prise pour une droite
suffit à faire retomber `Tr` en français, sans message. Catalogue à zéro manque
après coup, EN et ES.

## Ce qu'il faut savoir avant de généraliser

- 🔴 **Le paquet part en `AREA`** : tout le monde autour voit la zone, pas
  seulement le lanceur. C'est le comportement natif, non réglable côté serveur
  sans toucher `clif_skill_scale`.
- Les deux familles de zone ne se lisent pas au même endroit du skill_db :
  **`Unit.Layout` + `Unit.Range`** (zone persistante, 154 skills) et
  **`SplashArea`** (dégâts instantanés, 271 skills, souvent variable par niveau).
- 5 skills ont un layout **directionnel** (`Layout: -1` → Fire Wall, Ice Wall,
  Earth Strain, Fire Rain, Wall of Thorns) : un rectangle `x × y` centré ne peut
  pas les représenter.
- Le `skill_lv` EST dans `0x0A41` (contrairement au paquet d'incantation
  `0x013E`/`0x07FB`/`0x0B1A` de [[reference_cast_bar_re]], qui ne le porte pas) ⇒
  la taille par niveau est exacte, même pour les sorts d'autrui.
- Génération en masse : un script `tools/` peut dériver les entrées Lua depuis
  `db/pre-re/skill_db.yml`. `Layout` index N = carré `2N+1`.

## Si on voulait quand même le faire nous-mêmes

La brique de dessin existe : `DrawCellShrunk` de [[project_grey_world]]
(`C3dGround15::DrawCellQuad` 0x00a63800), qui donne un carreau par cellule en
DX7 comme en DX9. Ça n'aurait de sens que pour ce que le natif ne sait pas
faire : **prévisualiser sous le curseur AVANT d'envoyer** (mode ciblage, cf.
[[reference_cmode_sendmsg_use_skill]] : INF=2 → mode 1), ou dessiner des formes
non rectangulaires. Pour tout le reste, le natif suffit et ne coûte aucun hook.

Cf. [[reference_skill_cast_visual_re]], [[reference_effect_script_system_re]],
[[feedback_re_method]], [[feedback_absence_needs_measurement]].
