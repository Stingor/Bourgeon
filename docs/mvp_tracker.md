# MVP tracker — architecture

Chantier de **conception**, rien n'est livré. Relevé le 2026-08-16 sur le fork
`moonlight` (map-server) et sur le prototype PHP `moonlightsite`.

Le chantier est à cheval sur **deux dépôts** — le map-server (collecte,
persistance, protocole) et Bourgeon (fenêtre, minimap, saisie). Développement sur
une **branche dédiée dans chacun**, avec bascule coordonnée : voir §7.4.

> 📐 **Le *comment* est dans `docs/mvp_tracker_blueprint.md`** : découpage en
> lots, structures, schéma SQL, layouts de paquets et points d'accroche exacts.
> Il corrige aussi trois points de ce document — voir son §0.

> 🔴 **Le piège central de ce sujet** : on croit avoir à *collecter* des heures de
> mort. C'est déjà fait. Le map-server tient depuis longtemps un
> `mvp_respawn_cache` qui contient l'heure de mort à la seconde, le tueur, **et la
> position de la tombe** (§4.1), et `getmvprespawn()` l'expose à n'importe quel
> script. La vraie question n'est donc pas « comment savoir » mais **« qu'a-t-on le
> droit de montrer »** — et la réponse doit être un filtrage explicite, pas une
> propriété du stockage (§6). Un tracker écrit naïvement serait omniscient *par
> défaut*, sans que personne ne l'ait décidé.

---

## 0. Le besoin, en une phrase

Un carnet de chasse **partagé entre gens qui se font confiance** : quand un MVP
est-il retombé, où est sa tombe, et à partir de quand y a-t-il quelque chose à
tuer là-bas.

---

## 1. Ce qu'on remplace

Le prototype `moonlightsite/pagemvpmdrdtosqjhdf.php` : 92 lignes en table SQL,
saisie manuelle de l'heure de kill, PNG de carte cliquable, favoris, filtre.
**Jamais livré**, usage personnel — donc **aucune contrainte de
rétro-compatibilité**. Sa valeur est d'avoir montré quelles colonnes on regarde
vraiment.

Ce qu'on **ne** reprend pas, et pourquoi :

| Du prototype | Pourquoi non |
|---|---|
| `mvp_table` telle quelle | clé sur le nom, ambiguë (Atroce ×4, Orc Hero ×3, Dark Lord ×3…) |
| colonnes `status` / `tspawn` | dérivables ; le prototype fait 92 `UPDATE` par page sur du MyISAM |
| `x` / `y` en pixels d'un PNG 400×400 | inexploitable par le jeu ; on a les coordonnées de cellule |
| `fav` global | un favori doit être **par joueur**, pas partagé |
| délais recopiés à la main | déjà faux : Leak y est en `120/0`, le serveur dit `120/10` |
| Thanatos avec un délai de respawn | il n'a **pas** de respawn (§4.5) |

---

## 2. Le modèle : groupe, créneau, observation

Trois objets, et pas un de plus.

### 2.1 Le groupe

Un **Moonlight ID** (le `user_id` global) crée un groupe et y invite des gens.
L'appartenance est **au compte**, jamais au personnage : changer de tête ne fait
pas sortir du groupe.

Règles arrêtées :

- **Un compte appartient à 0 ou 1 groupe.** Pas de fusion à arbitrer, une seule
  liste à l'écran. Accepter une invitation quand on est déjà dans un groupe exige
  de quitter d'abord — avec un message explicite, jamais une sortie silencieuse.
- **Invitations depuis la guilde et la liste d'amis uniquement.** Pas d'inconnus :
  ça supprime d'un coup la modération, le blocage et le spam.
  Corollaire à afficher dans l'UI : inviter quelqu'un, c'est inviter **toutes ses
  têtes et tous ses comptes de jeu**. C'est le but (pas de taupe), mais ça doit se
  voir.

**✅ Aucune plomberie d'identité à construire — le précédent existe.** `@ignore`
fonctionne déjà « par compte Moonlight » et a posé tout l'outillage
(`src/map/pc.hpp:559` : *« L'identité de référence est le user_id, c'est-à-dire le
compte Moonlight »*) :

- `sd->status.user_id` est **en RAM, sans requête** : le char-server le charge avec
  le personnage (`SELECT user_id FROM login WHERE account_id`,
  `src/char/char.cpp:1163-1171`) et il voyage dans la structure de statut
  (`src/common/mmo.hpp:575`). Le map-server n'a donc **pas** à parler à `login`.
- La résolution d'un personnage **hors ligne** vers son `user_id` existe déjà
  (`src/map/pc.cpp:2404-2435`, jointure `char` × `login`) — précisément ce qu'il
  faut pour inviter un guildie déconnecté.
- ⚠ **`user_id == 0` est un cas prévu par le code** : *« émetteur non résolu ou
  compte non rattaché au site »* (`src/map/pc.cpp:2375`). Même si tous les comptes
  sont censés être liés, `pc_ignorechat()` s'en défend — le tracker doit faire
  pareil et refuser explicitement plutôt qu'échouer en silence.

Calquer ce précédent plutôt que d'ouvrir une seconde voie.
- **Le propriétaire qui part cède le groupe au membre le plus ancien**, sans vote.
  Sinon on obtient un groupe orphelin, et le comptage « un seul par compte »
  devient faux (on possède encore un groupe qu'on a quitté).
- **Le dernier membre qui part détruit le groupe**, et purge membres, favoris et
  invitations en attente.
- **Invitations persistées** (SQL) avec expiration, présentées au login : sinon on
  ne peut inviter que les gens déjà connectés.

Un groupe est adossé à un **canal de chat** : c'est l'usage réel (coordonner une
chasse), et le join/leave existe déjà. Mais le groupe reste un objet propre en
SQL — les canaux rAthena sont par personnage et non persistants, adosser la
propriété du groupe à un canal la rendrait fragile.

### 2.2 Le créneau — l'entité suivie

**Ce qu'on suit n'est pas un monstre, c'est un créneau de spawn.** Clé :

```
(mob_id, mapname)
```

Deux raisons, toutes deux factuelles :

1. Le même `mob_id` vit sur plusieurs cartes — Atroce sur 4, Orc Hero et Dark Lord
   sur 3, Eddga, Doppelganger, Maya, Baphomet, Moonlight Flower, Mistress sur 2.
2. Sur les cartes à spawn scripté, c'est l'inverse : **le `mob_id` change à chaque
   cycle** (`rand()` sur une plage de constantes, §4.5). Le mob n'est alors qu'un
   *attribut de la dernière observation*, pas une identité.

Un MVP ordinaire est simplement un créneau à une seule issue possible. Les mobs
scriptés étant des `monster` éphémères, détruits et recréés, leur identifiant
d'instance change à chaque cycle : **aucune continuité d'identité n'existe au
niveau de l'instance**, seulement au niveau du créneau.

Un créneau porte son **mécanisme** (§4.5) et son **degré de publicité** :

| Créneaux | Mécanisme | Issues possibles | Publicité |
|---|---|---|---|
| 73 `boss_monster` | `spawn_timer` du map-server | 1 mob fixe | à mériter (3 sources) |
| Bio Lab `lhz_dun03` | timer de NPC | **6 mobs** (1646-1651) | à mériter (1 source) |
| Bio Lab `lhz_dun04` | timer de NPC | **7 mobs** (2235-2241) | à mériter (1 source) |
| Lord of Death `niflheim` | invasion scriptée | 1 mob, **6 positions** | à mériter (1 source) |
| Thanatos `thana_boss` | verrou d'invocation | 1 mob | **public** |

### 2.3 L'observation

> 🔴 **Une observation n'est pas un état.** Le tracker ne voit que les kills de ses
> membres : il ne peut **jamais** affirmer qu'un MVP est vivant. N'importe qui
> d'extérieur peut l'avoir tué entre-temps.

Il n'existe que deux informations durables — « mort à T » (qui produit une
fenêtre) et rien. Tout le reste se dégrade en silence, y compris le « vivant » du
Convex Mirror, qui ne dit pas *il est vivant* mais *il était vivant à 21h04*.

Conséquences pour l'affichage :

- **L'âge de l'observation fait partie de l'information.** « Peut spawn » depuis
  20 secondes et « peut spawn » depuis 5 heures sont deux situations opposées ; le
  prototype les peint identiquement en rouge.
- Le seul état de vie fiable est **« surveillé en direct »** : un membre porte un
  Convex Mirror actif sur la carte, donc la position arrive en continu. Il expire
  avec le buff.
- Après un redémarrage du serveur, l'état de tout créneau est **« peut-être
  vivant »**, jamais « vivant » — et il faut l'écrire à l'écran (« serveur
  redémarré à HH:MM, aucune observation depuis »), sinon un tracker qui se vide
  d'un coup passe pour cassé.

Chaque observation porte donc **provenance**, **précision** et **date**. Une
observation plus précise écrase une moins précise ; à précision égale, la plus
récente gagne. Aucun arbitrage humain nécessaire.

| Provenance | Ce qu'elle donne | Précision |
|---|---|---|
| Convex Mirror (mort) | l'instant du retour, tirage déjà effectué | à la minute, **+1 à 2 min de retard** (§4.2) |
| Convex Mirror (vivant) | position, en continu tant que le buff court | temps réel, expire |
| Cache serveur (kill d'un membre) | heure de mort + **position de la tombe** | à la seconde ; fenêtre estimée |
| Tombe consultée | heure de mort + tueur | à la seconde ; fenêtre estimée |
| Saisie manuelle | ce que le joueur affirme | la moins fiable |

La saisie manuelle **reste** : elle couvre l'information reçue d'un non-membre
(Discord, un ami de passage). Elle est simplement en bas de la pile.

---

## 3. Stockage : ce qui meurt au reboot, ce qui survit

**RAM** — les observations. Justification : un redémarrage fait respawn tous les
MVP, donc tout timer accumulé devient faux et *doit* mourir avec le processus. Le
`mvp_respawn_cache` (§4.1) est déjà exactement cette structure : **ne pas en créer
une seconde**, s'y brancher.

**SQL** — le groupe lui-même (propriétaire, nom, création), ses membres, les
invitations en attente, et les favoris par `(user_id, créneau)`. Sans le groupe en
SQL on aurait des participants sans objet auquel participer.

---

## 4. Le terrain, mesuré

### 4.1 `mvp_respawn_cache` — le socle existe déjà

`src/map/mob.hpp:340-350` définit `s_mvp_respawn_info` :

```
mob_id, mapid, spawn_x, spawn_y, tomb_x, tomb_y,
respawn_tick, kill_time, killer_name
```

Peuplé dans `mob_setdelayspawn()` (`src/map/mob.cpp:1106-1127`) pour tout mob dont
`spawn->state.boss` est vrai, vidé dans `mob_delayspawn()` juste avant le respawn
(`src/map/mob.cpp:1060-1061`). Exposé aux scripts par `getmvprespawn()`
(`src/custom/script.inc:1388`), déjà consommé par l'« Event Sting MVP » de
`moon/mobs/mvps.npc`.

Ce que ça donne au tracker, gratuitement :

- **`tomb_x` / `tomb_y` sont déjà capturés** au moment de la mort. Le marquage de
  carte du prototype — un clic à la main sur un PNG — devient automatique. Le clic
  ne sert plus qu'à annoter.
- `kill_time` est déjà un **timestamp UNIX**.
- **« absent du cache » signifie exactement « peut-être vivant »** : la sémantique
  de §2.3 est déjà celle du code, elle n'est pas à construire.

> 🔴 **Le défaut à corriger avant tout le reste : la clé.** Le cache est indexé sur
> `mob_id` seul, et le commentaire l'assume — *« last kill per mob_id wins […]
> acceptable simplification »* (`mob.hpp:352-354`). Acceptable pour un event qui
> tire un MVP au hasard ; **rédhibitoire** pour un tracker, dont le métier est de
> suivre chaque emplacement : tuer Atroce sur `ra_fild03` écrase l'entrée d'Atroce
> sur `ve_fild01`. La clé doit devenir `(mob_id, mapid)` — `mapid` est déjà dans la
> structure, seule la clé manque.
> ⚠ Compatibilité : `getmvprespawn()` prend un `mob_id`. Lui ajouter un paramètre
> carte **optionnel**, sinon l'event Sting casse.

> ⚠ **`respawn_tick` est un tick serveur, `kill_time` est de l'UNIX** — les deux
> cohabitent dans la même structure. Le protocole ne transporte que de l'UNIX
> (§5) : conversion obligatoire à la sortie.

### 4.2 Convex Mirror — trois mesures qui changent l'affichage

`clif_bossmapinfo()` (`src/map/clif.cpp:22064`), paquet natif `ZC_BOSS_INFO`
`0x0293`. Le buff `SC_BOSSMAPINFO` est un **abonnement**, pas une photo :

- À l'application (`src/map/status.cpp:13359-13380`) : boss vivant →
  `ALIVE_WITHMSG` (position) ; boss mort avec `spawn_timer` armé → `DEAD` (délai).
- Au tick (`src/map/status.cpp:14497-14518`) : réémet `ALIVE` en continu, et
  **bascule vers `DEAD` si le boss meurt pendant le buff** — la garde
  `sce->val2 == 0` fait que ça n'arrive qu'une fois.
- **Carte du boss uniquement** : hors carte, le statut ne s'applique même pas
  (`return false`) ; en sortant, `val4 = 0` le rend muet.

La branche `DEAD` calcule depuis `md->spawn_timer`, donc le **vrai** timer, tirage
aléatoire déjà effectué. Ce n'est pas une estimation. Mais :

| Mesure | Conséquence |
|---|---|
| `DIFF_TICK(...)/1000 + 60` puis troncature en h/min (`clif.cpp:22086-22093`) | **60 s de marge délibérée** plus l'arrondi : l'annonce est en retard de 1 à ~2 min sur le vrai respawn. **Ne jamais peindre un compte à rebours à la seconde depuis cette source**, sinon on annonce le spawn après qu'il ait eu lieu. |
| `maxHours` / `maxMinutes` **jamais remplis** | les champs existent, le serveur les laisse à zéro. Pas de fenêtre : un point unique. |
| la branche `DEAD` ne touche **pas** `p.x` / `p.y` | le paquet est zéro-initialisé (`p = {}`), donc le client reçoit littéralement `0,0` et **pose l'icône dans le coin bas-gauche de la carte**. Ce n'est pas une position, c'est un champ vide dessiné. Vérifié en jeu. |

> 🔴 **Ne pas reproduire l'icône à `0,0`.** On remplace le radar natif par la
> minimap ImGui : on **comble**, on ne recopie pas. Soit on ne dessine rien quand
> la position est nulle, soit — mieux — on dessine la vraie tombe, dont on a les
> coordonnées par §4.1. Le tracker fera donc mieux que le Convex Mirror natif sans
> effort.

À récupérer dans l'autre sens : la branche `ALIVE` rafraîchit la position en
continu. Pendant un mirror actif, notre minimap peut **suivre le boss en direct**.

### 4.3 La tombe

`mvptomb_create()` (`src/map/mob.cpp:188`), appelée à la mort du MVP
(`src/map/mob.cpp:3750`). Le NPC porte `u.tomb.md`, `u.tomb.kill_time` et
`u.tomb.killer_name` ; `run_tomb()` (`src/map/npc.cpp:2242`) les affiche au clic.
C'est la source du cas « je passe devant une tombe et je veux l'ajouter » : heure
exacte même si aucun membre n'était présent — le joueur a dû aller sur place.

### 4.4 L'ingestion est déjà câblée — zéro C++ à écrire pour collecter

| Point d'accroche | Couverture |
|---|---|
| `classement::OnMvpDead` (`moon/mobs/mvps.npc:8`) | **73 `boss_monster`, couverture 100 %** (73 déclarations, 73 labels — vérifié) |
| `mvp_lhz_dun03::OnMyMVPDead` | Bio Lab 3 |
| `mvp_lhz_dun04::OnMyMVPDead` | Bio Lab 4 |
| `mvp_niflheim::OnLoDDead` | Lord of Death |
| `#summon_thanatos::OnMyMobDead` | Thanatos |

Chacun fait déjà le comptage `mvp_count` et la validation de la quête de chasse :
les points d'entrée sont non seulement présents mais **éprouvés**.

> 🔴 **Les scripts chargés sont ceux de `moon/`.** Le dossier `npc/` de l'amont
> rAthena **n'est pas chargé du tout** : un `grep` qui y tombe lit du code mort, et
> une correction qui y atterrit ne changera jamais rien en jeu.

### 4.5 Les quatre créneaux scriptés — une seule formule

Ils sont spawnés par `monster` en script, pas par `boss_monster`. Donc pas de
`spawn->state.boss`, **pas de `spawn_timer`** → ni entrée au cache, ni Convex
Mirror opérant après la mort (la branche `DEAD` exige un `spawn_timer` armé).
L'exclusion écrite dans `moon/mobs/mvps.npc:21-23` est **structurelle**, pas un
oubli.

- **Bio Lab** (`mvps.npc:164-214`) : `OnTimer7200000` puis
  `sleep rand(1,10)*60000`, `initnpctimer` relancé à la mort, et le `mob_id` **tiré
  au hasard à chaque cycle** :
  `lhz_dun03` → `rand(MOB_B_SEYREN, MOB_B_KATRINN)` = **1646-1651**, six mobs
  contigus (B_SEYREN, B_EREMES, B_HARWORD, B_MAGALETA, B_SHECIL, B_KATRINN) ;
  `lhz_dun04` → `rand(MOB_B_RANDEL, MOB_B_TRENTINI)` = **2235-2241**, sept mobs
  contigus (B_RANDEL, B_FLAMEL, B_CELIA, B_CHEN, B_GERTIE, B_ALPHOCCIO,
  B_TRENTINI). Aucun intrus dans les deux plages — vérifié id par id.
  🔴 **Les sept de `lhz_dun04` sont ABSENTS de `db/pre-re/mob_db.yml`** : ils
  n'existent que par `db/import/mobs/mvps.yml`. La table de référence se construit
  donc depuis **`mob_db` en mémoire, après import** — jamais en parsant les fichiers
  de `db/pre-re/`, qui mentent par omission.
- **Lord of Death** (`mvps.npc:217-347`) : même structure, mais la position varie —
  `switch( rand(6) )` sur des blocs `case 0` à `case 6`.
  ⚠ `rand(6)` rend **0 à 5** (`doc/script_commands.txt:8468-8476`) : le `case 6:`
  est du **code mort** et la position `86,219` n'est jamais tirée. **Six** positions
  atteignables, pas sept.
- **Thanatos** (`moon/quests/thana_quest.npc`) : invoqué par les joueurs via les
  sceaux — **aucun respawn**, mais un **cooldown**. À la mort,
  `#cooltime_thana::OnEnable` démarre, `$@thana_summon = 6` verrouille et
  `$@thana_summon_time` horodate le kill en UNIX (`thana_quest.npc:2260`) ;
  `OnTimer7200000` + `sleep rand(1,10)*60000` remet le verrou à 0 et rallume les
  sceaux (`thana_quest.npc:2289-2293`).

**Formule commune aux quatre : `mort + 7200000 ms + rand(1,10) min`**, soit une
fenêtre de **121 à 130 minutes** — la forme à deux arguments étant inclusive
(`doc/script_commands.txt:8468-8476`). Une seule implémentation, pas quatre.

> ⚠ Trois erreurs de commentaire à ne pas recopier : le bloc `niflheim` annonce
> « 133 min » là où `7200000` fait 120 ; le « 0 to 10 minutes » des quatre blocs est
> faux (`rand(1,10)` ne rend jamais 0) ; et le `case 6` de `niflheim` se croit
> atteignable. **Dériver les délais du code, jamais d'une table recopiée.**

Thanatos apporte au modèle une notion nouvelle : ce qu'on suit est une
**disponibilité**, pas une créature. « Puis-je aller faire Thanatos ? » est la même
question utilisateur que « Turtle General est-il debout ? » avec un mécanisme sans
rapport — ce qui valide le créneau comme abstraction. Et son verrou étant une
variable globale annoncée sur la carte, l'information est **publique** : rien à
mériter, rien à filtrer.

---

## 5. Le temps

- **UNIX absolu uniquement.** Jamais d'heure locale, jamais de fuseau dans le
  protocole. Le prototype s'est fait piéger là : cookie `timezone = offset + 120`
  avec l'heure d'été **codée en dur**, et des `±3600` semés dans le formatage.
- **Chaque paquet embarque l'heure serveur courante** en plus des données. Le
  client en tire un offset et travaille en absolu ensuite.
- **Resynchroniser à chaque paquet**, pas seulement au login : une longue pause de
  process décale l'horloge locale (et chez nous finit en déconnexion).
- Le calcul et le rafraîchissement des compteurs sont **délégués au client**, comme
  le faisait le JS du prototype. C'est du dérivé pur. Ce qui fait foi est l'heure
  serveur.

---

## 6. La règle de non-triche

Le map-server connaît le respawn réel à la milliseconde, et `getmvprespawn()` le
rend à n'importe quel script. **Le tracker n'a pas le droit de s'en servir** : le
Convex Mirror garde sa valeur, la chasse reste un jeu d'information.

Ce n'est pas une limitation technique, c'est une **décision de gameplay** — d'où sa
présence dans ce doc. Non écrite, elle sera enfreinte de bonne foi le jour où
quelqu'un voudra « juste afficher le vrai timer ».

Mise en œuvre : **la frontière est le paquet, pas le stockage.** La donnée exacte
et la donnée publiable vivent dans deux objets différents, et la projection vers un
groupe est un filtrage explicite qui ne laisse passer que ce qu'une observation a
mérité. Exception assumée : Thanatos, public par nature (§4.5).

Effet de bord à anticiper : le tracker rend le Convex Mirror nettement plus
désirable. Bon pour l'économie, mais le prix bougera.

---

## 7. Protocole

### 7.1 Opcodes

**Ne pas figer de numéro dans ce doc.** La source unique de vérité est
`Bourgeon/src/features/systems/bourgeon_opcodes.h` : il porte un `kNextFree`, et la
procédure est d'en prendre la valeur, de l'incrémenter, puis de la mirrorer côté
serveur (`packets_struct.hpp` + `clif_packetdb.hpp` + handlers). Toute la plage
`0x0F00..0x0FFF` est hors de la table de longueurs du client (vue en `flag=-1`),
donc aucune collision possible avec du vanilla, même sur un Ragexe plus récent.

État au 2026-08-16 : `0x0F10`–`0x0F28` alloués sans trou, et **`kNextFree = 0x0F29`
est bien libre**. Le « (CZ 0x0F29) » de `palette_editor.h:308` est une **coquille de
commentaire** : « Partager mon style » est livré et passe par `kStyle` = `0x0F26`
(seul site d'envoi, `style_sync.cc:203`). Prendre malgré tout `kNextFree` **au moment
de coder** plutôt qu'un numéro figé ici : un autre chantier peut passer avant.

### 7.2 Sens serveur → client

- **snapshot** : l'état complet du groupe, à l'entrée en jeu, à l'ouverture de la
  fenêtre et au join ;
- **delta** : un créneau change, diffusé aux membres connectés ;
- **groupe** : membres, invitations, changement de propriétaire.

### 7.3 Sens client → serveur

Créer, dissoudre, inviter, accepter, refuser, quitter, saisir une heure à la main,
annoter des coordonnées, basculer un favori.

### 7.4 Négociation et ordre de bascule

`CZ_BOURGEON_UI_CAPS` (`0x0F24`) annonce déjà « ce que ce client sait afficher ».
**Étendre ce mécanisme** plutôt que d'inventer une détection : c'est le
`bourgeon_ui()` / `hastracker()` voulu, et il existe.

Le serveur doit être déployé **avant** le client : un client à jour parlant à un
serveur qui l'ignore envoie des CZ inconnus. Deux branches, deux dépôts, bascule
serveur d'abord.

---

## 8. Côté Bourgeon

- `src/features/windows/mvp_tracker.{h,cc}` pour la fenêtre,
  `src/features/systems/` pour l'état réseau.
- `ImGuiTable` **triable** remplace le `ORDER BY` du prototype et son hack de
  ré-affichage d'en-tête tous les 31 rangs.
- `PushID` sur chaque rang : les noms de MVP sont dupliqués, les widgets se
  marcheraient dessus.
- Marqueur de tombe sur la **minimap ImGui**, en coordonnées de cellule — même
  mécanisme que le marqueur Fly Wing. Voir `docs/minimap_re.md`.
- **Ne pas dessiner l'icône à `0,0`** (§4.2).
- Alerte sonore et toast quand un favori entre dans sa fenêtre : c'est ce qu'un
  onglet de navigateur ne sait pas faire, et la première raison de porter l'outil
  dans le client.
- Le parseur d'heure souple du prototype est bon, le garder : `1430`, `14h30`,
  `-2350` pour la veille.
- ⚠ Pré-remplir un `InputText` déjà actif exige `ReloadUserBuf*`.
- Favoris dans `SaveData\`, jamais partagés.
- Skin RO via `ro_imgui`, largeurs mesurées, libellés FR, **noms de MVP en
  anglais**.

---

## 9. Reste à faire, reste à vérifier

**Les quatre vérifications sont soldées (2026-08-16) :**

1. ✅ **`user_id` : le point bloquant est tombé.** Rien à construire, le précédent
   `@ignore` a tout posé (§2.1) — `sd->status.user_id` est en RAM et la résolution
   d'un personnage hors ligne existe déjà.
2. ✅ `mvp_niflheim` lu en entier : **six** positions atteignables, pas sept (§4.5).
3. ✅ Plages Bio Lab mesurées id par id, plus la dépendance à `db/import/` (§4.5).
4. ✅ Opcodes : une procédure remplace le numéro, et `0x0F29` est déjà pris (§7.1).

**Défauts du serveur relevés au passage** — hors périmètre du tracker, à traiter
séparément ou pas du tout :

- le `case 6:` de `mvp_niflheim` est inatteignable : une position de spawn du Lord
  of Death est perdue depuis toujours ;
- trois commentaires de délai faux dans `mvps.npc` (§4.5) ;
- le cache MVP écrase les entrées des MVP multi-cartes (§4.1) — celui-là, le
  tracker **doit** le corriger.

**À faire, dans l'ordre :**

1. Clé du cache → `(mob_id, mapid)`, avec `getmvprespawn()` compatible.
2. Table des créneaux **construite au boot** depuis les spawns chargés en mémoire
   (aucun fichier annexe, aucune constante recopiée), plus les 4 créneaux scriptés.
3. Groupe : schéma SQL, invitations, règles de §2.1.
4. Projection filtrée + paquets.
5. Fenêtre ImGui.

---

## 10. Décisions écartées, et pourquoi

| Écarté | Raison |
|---|---|
| Le client comme capteur (hook de la mort côté client) | l'ingestion serveur existe déjà, ne se duplique pas, et n'a pas à croire le client sur parole. Un joueur sans Bourgeon alimente quand même le groupe. |
| Garder le site comme source, Bourgeon comme vue | le prototype n'est pas livré : deux vérités pour rien |
| Un script NPC pour capter la tombe | `run_tomb()` est en dur dans le map-server ; aucun script n'est nécessaire |
| Le tracker lit le vrai timer de respawn | §6 |
| Thanatos hors périmètre | faux : il a un cooldown, donc une disponibilité à suivre |
| Le groupe = un canal rAthena | canaux par personnage et non persistants ; l'appartenance est par compte et doit survivre au reboot |
