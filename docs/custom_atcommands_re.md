# Les commandes `@` maison de Moonlight — ce que le serveur donne, ce que le client ignore

> Relevé du **2026-09-02**. Serveur `moonlight` (`src/custom/atcommand.inc`,
> 101 Ko), client Bourgeon.
>
> Sorti du constat du jour : **le bon vecteur pour trouver du vierge est de
> partir de ce que le SERVEUR donne au joueur**, pas des fenêtres du client
> (cf. [unexplored_systems.md](unexplored_systems.md) §3 bis).

## 1. Le décompte

`src/custom/atcommand_def.inc` déclare **49 `ACMD_DEF`** propres à ce fork.
`conf/import/groups.yml` ouvre **71 commandes** au groupe `Id: 0` (« Player »,
celui de tout nouveau compte), dont **28 sont des commandes maison** :

```
autolootmvp  autolootmvpreward  autolootpognon  autolootrare  blockexp
flywinglast  ignore  ignorelist  unignore  partybuff  sellitem  sellstuff
separate  shopsearch  showmobinfo  showspeed  stats  storecard  wings
storagealt1..5  tri_inventaire  tri_cart  tri_storage  tri_gstorage
```

Les **21 autres** sont réservées au staff (`moche`, `celebrate`, `trollolol`,
`untrollolol`, `mvp*`, `aoeskill`, `sitstand`, `fastmove`…).

⚠ **Ce comptage se mesure, il ne se lit pas.** C'est `groups.yml` qui tranche,
jamais le C++ : une commande peut exister sans être donnée à personne.

## 2. Ce qui est DÉJÀ branché — la majorité

L'essentiel de ces 28 n'est pas du travail en attente : ce sont des **réglages**
qui passent par `bourgeon_setting_toggle` / `_set`, repartent en
**ZC `0x0F05`** et ont déjà leur case dans `src/features/panels/panel_commands.cc`
(`sellstuff`, `sellitem`, `showspeed`, les quatre `autoloot*`, `separate`,
`blockexp`, `flywinglast`, `showmobinfo`, les quatre `tri_*`). Les
`storagealt1..5` ont leurs onglets, documentés dans
[storage_window_re.md](storage_window_re.md) (ZC `0x0F1E`).

Cf. [[feedback_player_setting_persistence]] : un réglage de joueur vit côté
serveur, pas dans un yaml client.

## 3. Les quatre points d'entrée VIERGES

Ce qui manque n'est pas de la plomberie, ce sont des **gestes** : quatre
commandes ouvertes à tous, qui touchent la minimap, le chat, le coffre ou la
fenêtre de groupe, et qu'aucun élément d'interface ne déclenche.

### 3.1 `@shopsearch` — « où acheter cet objet ? »

La plus riche du lot. Elle écrit ses résultats en chat **et pose jusqu'à
`SHOPSEARCH_MAX_MARKS = 20` repères blancs sur la minimap**
(`clif_viewpoint(*sd, 1, …, 0xFFFFFF)`, `atcommand.inc:2527` et `:2563`).
Bourgeon sait déjà afficher les deux (repères serveur dans `minimap.cc`, liens
`<ITEML>`/`<NAVIL>` dans le chat) — mais **rien ne lance la commande** : le menu
d'un lien d'objet propose `@iteminfo` et `@whodrops`, et s'arrête là.

🔴 **Défaut vérifié : les repères ne sont jamais retirés.** Les deux seuls appels
à `clif_viewpoint` de ce fichier sont de **type 1** (poser) ; aucun appel de
type 2 (retirer) n'existe. Deux `@shopsearch` de suite empilent donc leurs
marques sur la minimap.

### 3.2 `@ignore` / `@unignore` / `@ignorelist`

Ce n'est **pas** le blocage natif. L'ignore porte sur le **compte** (table
`user_ignore`), suit la personne sur tous ses personnages, et coupe **tout** —
le filtre est planté dans `clif_send` lui-même (`clif.cpp:564`, `:609`), donc
zone, salon, chuchotement, groupe, guilde, canaux et bulle au-dessus de la tête.

⚠ **Correction d'une affirmation trop rapide.** Le balayage a signalé que
l'infobulle du menu contextuel « ment ». **C'est faux** : elle dit
« Liste d'ignorés du compte : ses chuchotements ne vous parviennent plus. Le chat
public, le groupe et la guilde ne sont pas filtrés » — et c'est **exactement**
vrai du blocage *natif* qu'elle décrit (`CZ_SETTING_WHISPER_PC` `0x00CF` ; le
serveur ne consulte `sd->ignore[]` que sur le chemin du chuchotement, ce que le
commentaire de `entity_context_menu.cc` explique déjà).

Le vrai manque est une **omission**, pas un mensonge : le mécanisme
compte-entier existe et n'a **aucune surface** côté client.

### 3.3 `@storecard` — 🔴 défaut de course vérifié

Elle charge le stockage premium n°5 (le coffre à cartes) en mode `PUT`, y verse
toutes les cartes de l'inventaire, puis referme.

```c
if (storage_premiumStorage_load(sd, 5, STOR_MODE_PUT) == 0) { … erreur … }
for (int i = 0; i < MAX_INVENTORY; i++)          // ⚠ s'exécute TOUT DE SUITE
    if (… IT_CARD …) storage_storageadd(sd, &sd->premiumStorage, i, …);
storage_premiumStorage_close(sd);
```

Or `storage_premiumStorage_load` **rend la main avant la fin du chargement**
quand le coffre n'est pas déjà celui en mémoire : elle fait
`return intif_storage_request(...)` (`storage.cpp:1367`), une requête
**asynchrone** au char-server. La boucle de rangement et la fermeture tournent
donc sur un coffre pas encore chargé ; c'est `storage_premiumStorage_open()` qui
arrivera plus tard — et **ouvrira le coffre tout seul** chez le joueur.

🔴 **Le correctif est écrit juste à côté.** `@storeall` a reçu ce garde :

```c
// storage.cpp:1338, dans storage_premiumStorage_open()
if (sd->state.pending_storeall) { sd->state.pending_storeall = false;
                                  storage_premiumStorage_storeall(sd); }
```

`@storecard` ne le pose **jamais** (0 occurrence dans son corps). Il lui faut le
même patron : armer un drapeau, et faire le transfert depuis
`storage_premiumStorage_open()`.

⚠ Le symptôme n'apparaît qu'au **premier** appel de la session : au second, le
coffre 5 est déjà en mémoire, `load` prend la branche synchrone et tout marche.
C'est ce qui rend le défaut discret.

### 3.4 `@partybuff` — 🔴 la bascule ne commande RIEN

Elle bascule `sd->state.spb` et renvoie la liste du groupe. Mais `sd->state.spb`
n'est lu qu'à un seul endroit : le `case PARTY_BUFF_INFO` de `clif_send`
(`clif.cpp:607`). Et **`PARTY_BUFF_INFO` n'a aucun émetteur** — vérifié :
4 occurrences dans tout `src/`, toutes dans `clif_send` (`:586`, `:601`, `:607`)
et dans l'énumération (`clif.hpp:229`). Aucun appel n'a jamais cette cible.

➡ Le joueur active un réglage **sans effet**. Deux issues honnêtes : faire
émettre les EFST des membres vers `PARTY_BUFF_INFO` côté serveur, ou retirer la
commande. Côté client, c'est le trou que la fenêtre de groupe voudrait combler —
mais cf. [[project_party_friend_window_re]] : les états d'un membre hors de
portée n'arrivent pas par l'acteur, il faut un paquet.

## 4. Ce que ça vaut

⚠ **Aucun de ces quatre n'est une panne visible.** `@shopsearch` répond,
`@ignore` fonctionne, `@storecard` marche au second essai, `@partybuff` affiche
son message. Ce sont trois défauts discrets et un manque d'ergonomie — à arbitrer
contre les chantiers réellement cassés.

Le plus rentable est probablement le plus petit : ajouter
**« Où l'acheter ? » → `@shopsearch <id>`** au menu d'un lien d'objet, à côté de
`@whodrops`. Le rendu (repères minimap + liens) existe déjà des deux côtés ; il
ne manque que le geste.

---

Voir aussi : [unexplored_systems.md](unexplored_systems.md),
[storage_window_re.md](storage_window_re.md),
[npc_progress_showscript_re.md](npc_progress_showscript_re.md).
