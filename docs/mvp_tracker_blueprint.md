# MVP tracker — blueprint d'implémentation

Se lit **après** `docs/mvp_tracker.md`, qui porte le *pourquoi* : le modèle
(groupe / créneau / observation), les mesures de terrain et les décisions
écartées. Celui-ci porte le *comment* : le découpage en lots, les structures, le
schéma SQL, les paquets et les points d'accroche exacts.

Rien n'est livré. Deux dépôts, **une branche dédiée dans chacun**, bascule
serveur d'abord.

---

## 0. Ce que ce blueprint corrige dans l'architecture

Trois choses relevées en préparant les lots. Les corriger ici plutôt que de les
découvrir en codant.

| Point | L'architecture disait | Ce que le code dit |
|---|---|---|
| « zéro C++ pour collecter » (§4.4) | l'ingestion est déjà câblée | **la collecte l'est, pas l'attribution.** Le cache est peuplé dans `mob_setdelayspawn()` (`mob.cpp:1106`) où **il n'y a aucun `sd`** : le seul tueur disponible y est le **nom** recopié depuis la tombe. Or le tracker doit créditer un `user_id`. Le point d'accroche est donc `mob_dead()` (`mob.cpp:3750`), où `mvp_sd`, `first_sd` et le `dmglog` sont sous la main. Une poignée de lignes, mais du C++ (§4). |
| favoris (§3 contre §8) | §3 les met en SQL, §8 « dans `SaveData\` », jamais partagés | **contradiction.** L'instruction d'origine est explicite (« sql pour le marquage des favoris ») et elle est meilleure : un favori suit le compte, donc tous ses personnages et tous ses postes. **SQL.** Ce qui reste local, c'est le **seuil d'alerte** et le son — des préférences d'interface, pas de la donnée. |
| opcodes (§7.1) | `kNextFree = 0x0F29` | déjà consommé : `0x0F29`/`0x0F2A` sont partis à la fenêtre de cible, **`kNextFree = 0x0F2B`** aujourd'hui. La règle de §7.1 (« prendre `kNextFree` au moment de coder, jamais un numéro figé dans un doc ») vient d'être validée en trois jours. Les numéros ci-dessous sont donc des **exemples** : relire l'en-tête au moment de coder. |

---

## 1. Les lots

Chacun se livre et se recette seul. L'ordre est une dépendance réelle, pas une
préférence.

| # | Lot | Dépôt | Recette |
|---|---|---|---|
| 0 | Clé du cache `(mob_id, mapid)` | moonlight | tuer Atroce sur deux cartes, `getmvprespawn()` rend deux entrées distinctes |
| 1 | Registre des créneaux, construit au boot | moonlight | une commande de staff liste 77 créneaux (73 + 4) avec leurs délais |
| 2 | Attribution : qui a mérité quoi | moonlight | un kill crédite le groupe du tueur, et lui seul |
| 3 | SQL : groupe, membres, invitations, favoris | moonlight | un groupe survit à un redémarrage, ses observations non |
| 4 | Protocole | les deux | le client reçoit un instantané cohérent, un membre sans Bourgeon n'est pas déconnecté |
| 5 | Fenêtre ImGui + marqueur de minimap | Bourgeon | tri, favoris, alerte, tombe à la bonne cellule |

Les lots 0 à 3 n'ont **aucun effet visible en jeu** : ils se déploient sans
attendre le client, ce qui est exactement l'ordre voulu par §7.4.

---

## 2. Lot 0 — la clé du cache

Le défaut est décrit en §4.1 de l'architecture. Voici les sites.

| Fichier | Ligne | Geste |
|---|---|---|
| `src/map/mob.hpp` | 337-355 | la clé devient une paire ; garder `mapid` dans la valeur (redondant mais lisible) |
| `src/map/mob.cpp` | 1060-1061 | `erase` — clé `(md->spawn->id, md->m)` |
| `src/map/mob.cpp` | 1127 | l'insertion — même clé |
| `src/custom/script.inc` | 1388 | `getmvprespawn()` : paramètre carte **optionnel** |
| `src/custom/script.inc` | ~1435 | `getallmvprespawn()` : itère le conteneur, **à relire** — c'est le second consommateur, celui qu'on oublie |

Forme suggérée, sans inventer de type :

```cpp
// clé = (mob_id, mapid). `mapid` reste aussi dans la valeur : les consommateurs
// la lisent déjà (script.inc:1414 -> map[info.mapid].name).
extern std::map<std::pair<uint16, int16>, s_mvp_respawn_info> mvp_respawn_cache;
```

Un `std::map` plutôt qu'un `unordered_map` : pas de `std::hash` à écrire pour une
paire, l'itération devient ordonnée (agréable pour `getallmvprespawn`), et la
taille du conteneur est de l'ordre de la centaine — la complexité ne se voit pas.

> ⚠ **Compatibilité de `getmvprespawn(<mob_id>)` sans carte.** L'« Event Sting MVP »
> l'appelle déjà. Sans carte, rendre **l'entrée la plus récente** pour ce `mob_id`
> — c'est exactement le comportement d'aujourd'hui, donc l'event ne bouge pas.

---

## 3. Lot 1 — le registre des créneaux

### 3.1 La structure

```cpp
enum e_mvp_slot_kind : uint8 {
    MVP_SLOT_BOSS_SPAWN = 0,  // boss_monster : spawn_timer du map-server
    MVP_SLOT_SCRIPT_TIMER,    // Bio Lab : timer de NPC, mob_id tiré à chaque cycle
    MVP_SLOT_SCRIPT_INVASION, // Lord of Death : timer de NPC, position tirée
    MVP_SLOT_SUMMON_LOCK,     // Thanatos : pas un respawn, une disponibilité
};

struct s_mvp_slot {
    uint16 slot_id;      // index dans le registre = IDENTITÉ SUR LE FIL
    uint16 mob_id;       // 0 quand le mob est tiré à chaque cycle (Bio Lab)
    int16  mapid;
    uint32 delay1;       // ms — la LOI (§9), publiable
    uint32 delay2;       // ms — l'amplitude du tirage, publiable
    e_mvp_slot_kind kind;
};
```

**`slot_id` est l'identité sur le fil, jamais en base.** Il vaut le rang dans le
registre, donc il change si un `boss_monster` est ajouté. Ce qui est **persisté**
(les favoris) porte la clé stable `(mob_id, map_name)`.

> 🔴 **Et les créneaux scriptés n'ont pas de `mob_id` stable.** Convention :
> `mob_id = 0`, la carte suffit — **vérifié** : les quatre occupent chacun une
> carte à eux (`lhz_dun03`, `lhz_dun04`, `niflheim`, `thana_boss`), et aucune de
> ces cartes ne porte de `boss_monster` (les trois premières sont explicitement
> exclues, `moon/mobs/mvps.npc:23` ; `thana_boss` n'apparaît pas du tout dans
> `mvps.npc`). La paire `(0, "lhz_dun03")` est donc sans ambiguïté.

### 3.2 Comment le construire — sans recopier une seule constante

> 🔴🔴 **Correction en codant : `map_foreachmob` seul RATE presque tous les MVP.**
> Ce paragraphe annonçait qu'« au boot tous les MVP sont vivants, donc tous
> énumérables » et qu'« il n'existe aucun conteneur global de `spawn_data` ». Les
> deux sont faux, et pour la même raison : **`dynamic_mobs: yes`**
> (`conf/battle/monster.conf:253`, confirmé par `conf/import/battle_conf.txt:335`).
> `npc_parse_mob` (`npc.cpp:5605`) range alors le `spawn_data` dans
> `map_data.moblist[]` (`map.hpp:847`, 128 entrées) et **ne spawne le mob que si la
> carte porte déjà un joueur** — au boot, aucune. Un balayage des `mob_data` au
> démarrage ne verrait donc quasiment rien. Mais du même coup, le conteneur global
> manquant existe : c'est **`moblist`**, indexé par carte.

L'énumération correcte lit **les deux** sources, et déduplique sur `(mob_id, mapid)` :

```cpp
// après npc_event_do_oninit() (map.cpp) : tout boss_monster est parsé.
// 1. les spawns parqués — le cas nominal, dynamic_mobs: yes
for (int32 m = 0; m < map_num; m++)
    for (int16 j = 0; j < MAX_MOB_LIST_PER_MAP; j++)
        if (mapdata->moblist[j] && mapdata->moblist[j]->state.boss) -> slot
// 2. les boss vivants — couvre dynamic_mobs: no ET les cartes dont les
//    128 entrées de moblist étaient pleines (map_addmobtolist rend -1)
map_foreachmob( mvp_registry_collect );   // map.hpp:1231
// slot{ spawn->id, m, spawn->delay1, spawn->delay2 }  (spawn_data : map.hpp:472)
```

Ainsi `dew_dun01 … LEAK,1,7200000,600000` n'est écrit qu'à un seul endroit —
dans le script de spawn, là où il l'est déjà — et l'erreur du prototype PHP sur
Leak (§1 de l'architecture) devient inexprimable.

Les quatre créneaux scriptés, eux, sont **déclarés en dur** dans le registre
(personne ne peut les déduire). C'est la seule constante recopiée de tout le
blueprint, et elle est justifiée par §4.5 de l'architecture. **Un commentaire
doit pointer les quatre labels** : si le script change, ceci ment.

> ⚠ **La fenêtre est `[7260000, 7800000]` ms, pas `[7200000, 7800000]`.** Les quatre
> font `OnTimer7200000` **puis** `sleep rand(1,10)*60000`, et `rand(a,b)` est
> **inclusif des deux bornes** (`buildin_rand` → `rnd_value` →
> `std::uniform_int_distribution`, vérifié) : le tirage ne vaut jamais 0, donc la
> borne basse est 120 + 1 minute. D'où `delay1 = 7260000` et `delay2 = 540000`, ce
> qui redonne bien les 121-130 min de §4.5.

> ⚠ **Le compte de la recette n'est pas 73 + 4.** `moon/mobs/mvps.npc` porte bien
> 73 `boss_monster` actifs, mais **quatre autres** vivent ailleurs et sont chargés :
> `moon/rathena/dungeons/nif_dun.npc:49`, `oz_dun.npc:25` et `sp_rudus.npc:30 et 48`
> (les 3 de `einbroch.npc` / `glastheim.npc` sont commentés, et
> `npc/pre-re/mobs/**` n'est **pas** chargé : `npc/scripts_monsters.conf` ne prend
> que jail, pvp et towns). Attendu : **77 créneaux de boss + 4 scriptés = 81**. Le
> `ShowStatus` du registre affiche le compte réel au démarrage — c'est lui qui fait
> foi, pas ce paragraphe.

---

## 4. Lot 2 — l'attribution

### 4.1 Le point d'accroche

`mob_dead()`. Pourquoi là et pas dans `mob_setdelayspawn()` avec le cache :
**c'est le seul endroit qui connaît des joueurs**. Plus bas, le tueur n'est plus
qu'un `killer_name` recopié depuis la tombe, et remonter d'un nom à un `user_id`
coûterait une requête SQL par kill de MVP (`pc.cpp:2404-2435`) pour une
information qu'on avait deux appels plus tôt.

> 🔴 **Corrigé en codant : pas « juste avant `mob_setdelayspawn()` », mais AVANT le
> retour anticipé `if(!md->spawn)`.** Ce paragraphe visait la branche de la tombe,
> qui est gardée par `md->spawn->state.boss` — donc inatteignable pour les quatre
> créneaux scriptés, dont le mob est un `monster` éphémère sans données de respawn
> et qui **sort de `mob_dead()` une trentaine de lignes plus haut**. En posant le
> crochet avant ce retour, **un seul appel couvre les deux cas** et, surtout, garde
> le `dmglog` sous la main pour les scriptés aussi. Le builtin de §4.2 devient
> inutile (cf. l'encadré là-bas).

Le crochet est gardé par un prédicat volontairement bon marché — il tourne à
**chaque mort de monstre** :

```cpp
bool mvp_tracker_is_tracked( const mob_data& md ){
    if( md.spawn != nullptr && md.spawn->state.boss ) return true;                 // le cas nominal
    return mvp_tracker_map_is_scripted( md.m ) && md.get_bosstype() == BOSSTYPE_MVP;
}
```

Le second membre ne coûte qu'un balayage de **quatre `int16`**. Le test de
`bosstype` est indispensable : sur `lhz_dun03`/`lhz_dun04`, le script pose **aussi**
un leurre de niveau 99 à côté du MVP (`rand(1640,1645)` / `rand(2228,2234)`), et
sans lui, tuer le leurre écrirait une observation. Le discriminant est dans la
donnée, pas dans une plage recopiée : `B_SEYREN` (1646) porte `MvpExp`, `G_SEYREN`
(1640) non — d'où `BOSSTYPE_MVP` d'un côté et pas de l'autre.

### 4.2 Qui est crédité

> 🔴🔴 **TRANCHÉ le 2026-08-30, contre la recommandation ci-dessous : seuls
> `mvp_sd` et `first_sd` sont crédités.** La règle « tout le `dmglog` » a un trou
> qu'aucun raisonnement sur l'équité ne rattrape : **le `dmglog` retient
> n'importe quel dégât**. Un passant lance un sort à 1 point, s'en va, et **son
> groupe entier hérite du kill** — sans être là à la mort, sans avoir vu la
> tombe. Multiplié par un membre qui fait la tournée des MVP, c'est une moisson.
>
> Les deux retenus sont ceux que **rAthena lui-même** considère comme ayant
> mérité le MVP : le plus gros contributeur en dégâts et celui qui a la priorité
> de loot. Ils sont souvent la même personne, et le code n'écrit l'observation
> qu'une fois par groupe.
>
> **Le coût est assumé** : dans un groupe de douze, les dix autres n'alimentent
> pas automatiquement leur carnet. Il leur reste la **tombe sur place** et la
> **saisie manuelle** — deux sources légitimes du modèle, simplement moins
> précises, ce qui est exactement ce que la colonne « Source » sert à montrer.
>
> Les variantes écartées : un **seuil de dégâts** (5-10 % des PV) punirait le
> prêtre du groupe, qui ne tape presque pas ; une garde **« encore sur la carte »**
> refermait le trou du passant mais laissait tout le reste du `dmglog` passer.

Le paragraphe d'origine, conservé pour la trace du raisonnement :

Le tueur seul serait injuste (le MVP-drop revient à un seul joueur d'un groupe de
douze) et la carte entière serait de la triche. La règle proposée :

> ~~**Tout groupe ayant au moins un membre dans le `dmglog` du MVP est crédité.**~~

`md->dmglog` est déjà rempli. Coût : une poignée de recherches à la mort d'un MVP.
Les joueurs **hors ligne** au moment de la mort ne sont pas résolus — c'est sans
conséquence, le crédit va au *groupe*, pas à la personne, et il suffit qu'un
membre présent soit encore connecté.

> 🔴 **Deux erreurs de ce paragraphe, corrigées en codant.**
> 1. `dmglog` **n'est pas borné par `DAMAGELOG_SIZE`** : c'est un
>    `std::deque<s_dmglog>` (`mob.hpp:399`), on l'itère, on ne l'indexe pas.
> 2. Il porte des **`char_id`, pas des `bl` id** — le commentaire de `s_dmglog`
>    le dit (`mob.hpp:365`) et les huit consommateurs de `mob.cpp` le confirment :
>    c'est **`map_charid2sd()`**, jamais `map_id2sd()`. Avec `map_id2sd()`, la
>    recherche ne rend rien ou pire, rend quelqu'un d'autre.

> ✅ **Le builtin `mvptracker_report()` n'est plus nécessaire** et n'a pas été
> écrit. Il n'existait que parce que le crochet était censé vivre dans la branche
> de la tombe, hors de portée des créneaux scriptés ; posé avant le retour
> anticipé (§4.1), **le même appel les couvre, avec leur `dmglog`** — donc avec la
> règle de crédit complète, là où le builtin n'aurait connu que le RID du label.
> Les quatre scripts restent inchangés, ce qui est encore mieux : rien à
> maintenir en double. La fonction `mvp_tracker_report_scripted()` existe quand
> même dans le module, pour la saisie manuelle du lot 4.

---

## 5. Lot 3 — SQL

Conventions du dépôt : un fichier par sujet dans `sql-files/`, en-tête commenté,
`CREATE TABLE IF NOT EXISTS`. Deux précédents à imiter — `user_ignore.sql`
(relation entre comptes Moonlight, InnoDB) et `bug_reports.sql` (texte joueur,
utf8mb4). Ici : **InnoDB** (écritures concurrentes, clés composites) et
**utf8mb4** (les noms de groupe seront accentués).

```sql
CREATE TABLE IF NOT EXISTS `mvp_group` (
  `group_id`       int(11) unsigned NOT NULL AUTO_INCREMENT,
  `owner_user_id`  int(11) unsigned NOT NULL DEFAULT 0,
  `name`           varchar(32)      NOT NULL DEFAULT '',
  `created_at`     datetime         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`group_id`),
  KEY `k_owner` (`owner_user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mvp_group_member` (
  `user_id`   int(11) unsigned NOT NULL DEFAULT 0,
  `group_id`  int(11) unsigned NOT NULL DEFAULT 0,
  `joined_at` datetime         NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`user_id`),          -- un compte = AU PLUS un groupe, garanti par le SCHÉMA
  KEY `k_group` (`group_id`,`joined_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mvp_group_invite` (
  `group_id`     int(11) unsigned NOT NULL DEFAULT 0,
  `user_id`      int(11) unsigned NOT NULL DEFAULT 0,
  `from_user_id` int(11) unsigned NOT NULL DEFAULT 0,
  `expires_at`   datetime         NOT NULL,
  PRIMARY KEY (`group_id`,`user_id`),
  KEY `k_user` (`user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `mvp_favorite` (
  `user_id`  int(11) unsigned     NOT NULL DEFAULT 0,
  `mob_id`   smallint(5) unsigned NOT NULL DEFAULT 0,  -- 0 = créneau scripté : la carte identifie
  `map_name` varchar(24)          NOT NULL DEFAULT '',
  PRIMARY KEY (`user_id`,`mob_id`,`map_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

**Trois choix à défendre :**

- 🔴 **`PRIMARY KEY (user_id)` sur les membres** : la règle « un seul groupe par
  compte » (§2.1) devient une **contrainte de stockage**, pas une vérification
  applicative qu'on peut oublier dans un chemin d'erreur. L'`INSERT` échoue, point.
- **Aucun nom de personne stocké.** Même raison que `user_ignore.sql` l'écrit
  déjà : un libellé figé ment dès le premier renommage. Les noms affichés sont
  recalculés à la connexion, comme le fait `pc_ignorechat_load`.
- **`joined_at` sert de règle de succession** : le propriétaire qui part cède au
  membre le plus ancien (§2.1), c'est un `ORDER BY joined_at LIMIT 1`, sans vote
  et sans champ supplémentaire.

Les **observations ne sont pas ici** : elles meurent au redémarrage, par
construction (§3 de l'architecture).

---

## 6. Lot 4 — l'état RAM

```cpp
enum e_mvp_source : uint8 {          // ordre = PRÉCISION CROISSANTE
    MVP_SRC_MANUAL = 0,              // ce qu'un joueur affirme
    MVP_SRC_TOMB,                    // tombe consultée sur place
    MVP_SRC_KILL,                    // un membre l'a tué
    MVP_SRC_MIRROR,                  // Convex Mirror d'un membre : le VRAI instant
};

struct s_mvp_obs {                   // une OBSERVATION, jamais un état
    e_mvp_source source;
    int64  kill_time;                // UNIX, 0 si inconnue
    int64  exact_respawn;            // UNIX. 0 = NON MÉRITÉ. Rempli en 2 sites, cf. §9
    uint16 mob_id;                   // ce qui est TOMBÉ (Bio Lab : varie)
    int16  tomb_x, tomb_y;           // -1 = position inconnue (JAMAIS 0,0, cf. §7.1)
    uint32 by_user_id;               // qui a fourni
    int64  reported_at;              // UNIX — l'ÂGE fait partie de l'information (§2.3)
    char   killer_name[NAME_LENGTH];
};

struct s_mvp_group {
    uint32 group_id, owner_user_id;
    char   name[MVP_GROUP_NAME_LEN];
    std::vector<uint32> members;                        // user_id, miroir du SQL
    std::unordered_map<uint16, s_mvp_obs> obs;          // slot_id -> observation
    std::vector<map_session_data*> online;              // index de diffusion
};
```

**Règle d'écrasement, en une ligne :** une observation remplace la précédente si
`source` est strictement supérieure, ou si `source` est égale et `reported_at`
plus récent. Aucun arbitrage humain, aucun conflit à afficher.

> ⚠ **`online` est un vecteur, pas un pointeur.** Un même compte Moonlight peut
> avoir **plusieurs comptes de jeu connectés en même temps** — c'est même le
> propos de l'identité par `user_id`. Tenu à jour à l'entrée en jeu et à la
> déconnexion, il évite un `map_foreachpc` à chaque delta ; il n'existe aucun
> index `user_id -> sessions` dans le map-server (vérifié).

---

## 7. Lot 5 — le protocole

### 7.1 Trois opcodes, pas douze

Précédent : `CZ_BOURGEON_PRESET_CMD` (`0x0F06`, variable) porte **toutes** les
commandes de presets dans un seul paquet. Même patron ici.

> **Numéros retenus : `0x0F30` (CZ), `0x0F31` et `0x0F32` (ZC)** — la valeur
> qu'annonce `kNextFree` sur `master`, désormais porté à `0x0F33`.
>
> 🔴🔴 **La leçon de ce lot n'est PAS celle que j'avais écrite.** J'ai d'abord
> conclu que « `kNextFree` mentait de cinq valeurs », parce que la branche
> affichait `0x0F2B` alors que `0x0F2B`..`0x0F2F` étaient pris côté serveur. C'est
> faux : `master` déclarait déjà ces cinq opcodes et disait bien `0x0F30`.
> **Ce n'est pas le fichier qui mentait, c'est ma branche qui avait huit jours et
> 179 commits de retard.**
>
> ➡ La vraie règle : **avant d'auditer un espace de numérotation partagé, vérifier
> que la branche est à jour** (`git rev-list --left-right --count master...HEAD`).
> Sur une branche périmée, tout audit d'une ressource partagée — opcodes, ids de
> fenêtres, clés de réglages, catalogue i18n — produit des « découvertes » qui ne
> décrivent que sa propre vieillesse, et les corrections qu'on en tire écrasent du
> travail neuf. Ici, deux autres « trouvailles » du même lot étaient du même
> tonneau : l'infrastructure d'icône de menu et le correctif `ReadBossCell`
> existaient déjà sur `master`, en mieux.

| Sens | Nom | Rôle |
|---|---|---|
| CZ | `CZ_BOURGEON_MVP_CMD` | toutes les commandes : créer, dissoudre, inviter, accepter, refuser, quitter, exclure, favori, saisie manuelle, annotation, demander l'instantané |
| ZC | `ZC_BOURGEON_MVP_STATE` | catalogue des créneaux, instantané, delta — distingués par un champ `kind` |
| ZC | `ZC_BOURGEON_MVP_GROUP` | membres, invitations, changement de propriétaire, **et les refus** |

Numéros : prendre `kNextFree` (`0x0F2B` au 2026-08-22) et incrémenter — §0.

```
CZ_BOURGEON_MVP_CMD   [op:2][len:2][cmd:1][a:4][b:4][text: len-13 octets, NON terminé]
    a / b : selon cmd (slot_id, user_id, timestamp UNIX, x et y…)
    text  : nom de groupe ou nom de personnage visé. Longueur DÉDUITE de len.

ZC_BOURGEON_MVP_STATE [op:2][len:2][kind:1][server_time:8][count:2] puis count entrées
    kind 0 = CATALOGUE : [slot_id:2][mob_id:2][kind:1][delay1:4][delay2:4]
                         [map:MAP_NAME_LENGTH_EXT][name:NAME_LENGTH]
    kind 1 = INSTANTANÉ, kind 2 = DELTA :
                         [slot_id:2][source:1][mob_id:2][kill_time:8][exact:8]
                         [tomb_x:2][tomb_y:2][by_user_id:4][reported_at:8]

ZC_BOURGEON_MVP_GROUP [op:2][len:2][kind:1][group_id:4][owner_user_id:4]
                      [name:32][count:1] puis count fois
                      [user_id:4][level:2][online:1][char_name:NAME_LENGTH]
```

- **`server_time` dans chaque paquet**, comme l'impose §5 : le client en tire un
  offset et n'a plus jamais besoin d'un fuseau. Le prototype PHP s'est fait piéger
  exactement là.
- **Le catalogue part une fois par session**, sur demande à l'ouverture de la
  fenêtre. 77 entrées d'une cinquantaine d'octets tiennent dans un seul paquet
  (`packetLength` est un `int16`, la marge est large).
- **`exact = 0` signifie « non mérité »**, et le client dessine alors une
  *fenêtre*, jamais un point (§9).
- **`tomb_x = -1`** signifie « position inconnue ». `0,0` est une position
  **valide** sur une carte : c'est précisément l'artefact du Convex Mirror natif
  (§4.2 de l'architecture), et le confondre avec « pas de tombe » reproduirait le
  bug qu'on veut combler.
- Déclaration : `parseable_packet(HEADER_…, -1, clif_parse_…, 0)` dans
  `clif_packetdb.hpp` (le CZ), et `DEFINE_PACKET_HEADER` dans `packets_struct.hpp`
  pour les trois — mêmes gestes qu'aux dix-huit customs déjà en place.

### 7.2 La négociation

Un bit de plus dans le masque `UI_CAPS`, **miroir exact des deux côtés** :

| Côté | Fichier | À ajouter |
|---|---|---|
| Bourgeon | `src/features/systems/ui_caps.h:57` | `kMvpTracker = 1u << 2` |
| moonlight | `src/map/clif.hpp` (`e_bourgeon_ui_cap`) | `BOURGEON_UI_MVP_TRACKER = 0x00000004` |

C'est bien une **capacité d'affichage** au sens de `ui_caps.h` — « si j'envoie un
delta, sera-t-il montré ? » — et non une préférence : le bit tombe quand le joueur
éteint la fenêtre, et le serveur cesse alors de lui diffuser. C'est aussi le
`hastracker()` cherché en conception, sans rien inventer.

> ⚠ **Le masque n'est pas persisté** et il est réannoncé après le handshake
> d'intégrité. Un membre **sans** Bourgeon reste membre, continue d'**alimenter**
> le groupe par ses kills, et ne reçoit simplement rien. C'est le bon
> comportement, pas un cas dégradé.

---

## 8. Lot 6 — côté Bourgeon

| Fichier | Rôle |
|---|---|
| `src/features/systems/mvp_tracker.{h,cc}` | état réseau : catalogue, observations, groupe, offset d'horloge |
| `src/features/windows/mvp_tracker_window.{h,cc}` | la fenêtre |
| `src/features/overlays/minimap.cc` | **une couche de plus**, pas une modification du marqueur existant |
| `src/bourgeon.cc` | enregistrement, comme les trente autres (`make_unique<…>`, vers la ligne 996) |

Gestes déjà éprouvés, à reprendre tels quels :

- `RegisterRecvOpcode(bopcodes::kMvpState)` dans le constructeur, `OnRecvPacket`
  qui ne fait que **pousser dans la boîte** (fil réseau), `HandlePacket` qui
  travaille — patron de `target_frame.cc:579` et `:900`.
- `SendPacket` avec `[op:2][len:2]` écrits à la main, comme
  `target_frame.cc:965-990`.
- ⚠ Les offsets mémoire ne concernent que la minimap ; la fenêtre, elle, ne lit
  **rien** du client, donc pas de garde `timestamp() != 20250716` à poser.

### 8.1 La minimap

Le marqueur de boss **existe déjà** : `g_cfg.show_boss` et `ReadBossCell()`
(`minimap.cc:519`), qui lit ce que le **natif** a stocké après `ZC_BOSS_INFO`
(`CGameMode+0x5CC` et voisins). C'est donc lui qui porte l'icône à `0,0`.

Deux gestes distincts, à ne pas mélanger :

1. **Corriger l'existant** : `ReadBossCell` rend `false` quand la cellule vaut
   `0,0`. Trois lignes, indépendantes du tracker, qui suppriment l'icône dans le
   coin.
2. **Ajouter la couche du tracker** : les tombes connues du groupe, en cellules,
   avec le même calcul `(cells_h - y) / cells_h` que les autres marqueurs
   (`minimap.cc:1305-1312`). Réglage propre (`show_mvp_tombs`), pas un
   détournement de `show_boss`.

### 8.2 La fenêtre

- `ImGuiTable` triable (`ImGuiTableFlags_Sortable`) — remplace le `ORDER BY` du
  prototype **et** son hack de ré-affichage d'en-tête tous les 31 rangs.
- `PushID(slot_id)` par rang : les noms de MVP sont dupliqués (Atroce ×4).
- Le compte à rebours est **dérivé à l'affichage** depuis `server_time` + offset,
  jamais un compteur entretenu.
- L'**âge** de l'observation est une colonne, pas une infobulle (§2.3).
- Alerte : toast et son quand un favori entre dans sa fenêtre. Seuil et son dans
  `SaveData\`, **le favori lui-même en SQL** (§0).
- Parseur d'heure souple repris du prototype (`1430`, `14h30`, `-2350` pour la
  veille). ⚠ Pré-remplir un `InputText` **actif** exige `ReloadUserBuf*`.
- Skin `ro_imgui`, largeurs mesurées, libellés FR, **noms de MVP en anglais**.

---

## 9. La non-triche, exprimée en code

La règle de §6 tient en une phrase que le code doit rendre vraie :

> **Le tracker a le droit de connaître la loi (`delay1`, `delay2`), jamais le
> tirage (`respawn_tick`).**

`delay1` et `delay2` sont publics — ils sont dans le script de spawn et le
prototype PHP les publiait déjà. `respawn_tick` est le résultat du `rnd()` : c'est
lui qui vaut de l'argent, et c'est lui que `getmvprespawn()` rend en clair à
n'importe quel script.

Mise en œuvre :

```cpp
// LA SEULE fonction qui a le droit de lire le tirage. Deux appelants, pas trois.
void mvp_tracker_earn_exact( map_session_data& sd, mob_data& boss_md );

// Ce qui part sur le fil dans tous les autres cas : une FENÊTRE, calculée depuis
// la loi. Aucun accès à mvp_respawn_cache, aucun accès à md->spawn_timer.
// fenêtre = [kill_time + delay1, kill_time + delay1 + delay2]
```

Les deux seuls appelants légitimes sont les sites où le serveur a déjà décidé
qu'un joueur a **payé** pour l'information :

| Site | Ce qu'on y gagne |
|---|---|
| `src/map/status.cpp:13376-13379` — application de `SC_BOSSMAPINFO`, branche `DEAD` | l'instant exact du retour |
| `src/map/status.cpp:14515-14518` — au tick, bascule vers `DEAD` quand le boss meurt pendant le buff | idem, sans que le porteur ait à réutiliser un miroir |

Les deux ont `sd` **et** `boss_md` sous la main (`sce->val1` est l'**id de `bl`**
du boss, pas son `mob_id` : `map_id2boss()` fait déjà le travail). Le crochet est
donc de deux lignes, à l'endroit exact où le natif envoie déjà l'information au
seul porteur du miroir. Rien à extrapoler, rien à deviner.

Les branches `ALIVE` et `ALIVE_WITHMSG` des mêmes sites donnent le second cadeau :
la position rafraîchie en continu, soit l'état **« surveillé en direct »** de
§2.3 — le seul « vivant » que le tracker ait le droit d'écrire.

> 🔴 **Décision à assumer : notre miroir sera meilleur que le natif.** Le natif
> ajoute `+60 s` puis tronque en heures et minutes (`clif.cpp:22086-22093`), d'où
> 1 à 2 minutes de retard. En publiant `exact_respawn` en UNIX, on supprime les
> deux. C'est cohérent avec « on comble, on ne recopie pas », mais c'est **un
> changement de valeur d'un objet du jeu** — donc une décision, pas un détail
> d'implémentation. Cf. §10.

---

## 10. Ce qu'il reste à trancher

Aucune de ces questions ne bloque les lots 0 à 3.

| # | Question | Recommandation |
|---|---|---|
| 1 | Le miroir publié à la seconde, ou avec la marge du natif ? | **à la seconde.** La marge de 60 s existe pour éviter d'annoncer un spawn déjà passé quand on tronque en minutes ; en UNIX absolu, le problème n'existe pas. |
| 2 | ~~Le crédit va-t-il à tout le `dmglog` ou au seul `mvp_sd` ?~~ | ✅ **TRANCHÉ : `mvp_sd` + `first_sd` seulement.** Le `dmglog` retient n'importe quel dégât, donc un sort à 1 point suffisait à faire moissonner un groupe entier. Cf. l'encadré de §4.2. |
| 3 | Taille maximale d'un groupe | **en poser une** (24 ?) : elle borne le paquet, la diffusion et l'appétit. Une limite ajoutée après coup casserait des groupes existants. |
| 4 | Durée de vie d'une invitation | **7 jours.** Assez pour un joueur qui se connecte le week-end, assez court pour que la table ne devienne pas un cimetière. |
| 5 | Un membre voit-il **qui** a fourni une observation ? | **oui.** `by_user_id` est déjà dans la structure, le groupe est fondé sur la confiance, et ça règle les malentendus sans modération — le propos même de §2.1. |

---

## 11. Recette

Ce qu'il faut avoir vu tourner avant de dire que c'est livré.

1. **Multi-cartes** — tuer Atroce sur deux cartes : deux lignes distinctes, deux
   fenêtres distinctes. C'est le défaut qui justifie le lot 0.
2. **Bio Lab** — deux cycles d'affilée : le `mob_id` change, le créneau non.
3. **Redémarrage** — le groupe et les favoris survivent, les observations
   disparaissent, et la fenêtre **écrit** « serveur redémarré à HH:MM » au lieu de
   se vider en silence (§2.3).
4. **Miroir** — porter un Convex Mirror pendant qu'un membre tue le boss : le
   groupe reçoit l'instant exact, et lui seul.
5. **Non-triche** — un créneau qu'aucun membre n'a observé n'affiche **rien**,
   alors même que le serveur en connaît le timer à la milliseconde.
6. **Sans Bourgeon** — un membre en client natif tue un MVP : le groupe l'apprend,
   et lui ne reçoit aucun paquet inconnu.
7. **Deux comptes de jeu du même Moonlight ID connectés ensemble** : les deux
   reçoivent les deltas, et quitter le groupe depuis l'un le retire aux deux.
8. **Un seul groupe par compte** — tenter d'accepter une invitation en étant déjà
   membre : refus explicite, message clair, et l'`INSERT` qui échoue de toute
   façon (§5).
