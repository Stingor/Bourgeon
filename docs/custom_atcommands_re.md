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
celui de tout nouveau compte), dont **28 étaient des commandes maison** —
**27 depuis le retrait de `@partybuff`** (§3.4) :

```
autolootmvp  autolootmvpreward  autolootpognon  autolootrare  blockexp
flywinglast  ignore  ignorelist  unignore  sellitem  sellstuff
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

### ⛔ Le « défaut » des repères qui s'empilent — **RÉTRACTÉ le 2026-09-02**

Ce document a affirmé que « les repères ne sont jamais retirés » et que deux
`@shopsearch` de suite les empilaient. **C'est FAUX**, et l'erreur mérite d'être
nommée parce qu'elle est instructive.

La signature est `clif_viewpoint(sd, npc_id, type, x, y, id, color)` — **sept**
paramètres, et une seule surcharge dans tout `src/`. L'appel de `@shopsearch` est
`clif_viewpoint(*sd, 1, 0, x, y, ++marks, 0xFFFFFF)` : le `1` est le **npc_id**,
et le **type vaut 0**. Or `clif.cpp:2867` documente les types :

```
///     0 = display mark for 15 seconds
///     1 = display mark until dead or teleported
///     2 = remove mark
```

**Les repères expirent donc d'eux-mêmes au bout de 15 secondes, côté client.**
Aucun appel de type 2 n'est nécessaire, et c'est l'idiome de tout le dépôt :
`@mobsearch` (`atcommand.cpp:8446`) et `teleport.cpp:38` écrivent exactement la
même chose.

🔴 **La leçon** : j'ai lu le premier `1` comme le type sans ouvrir la signature,
puis j'ai « vérifié » en cherchant un appel de type 2 — une recherche qui ne
pouvait que rendre vide, puisqu'elle cherchait la mauvaise chose. Un argument
positionnel ne se lit pas au jugé. Cf. [[feedback_absence_needs_measurement]] :
valider le motif avant de croire un zéro.

⚠ Ce qui reste discutable, et qui est une **question de réglage, pas un
défaut** : 15 secondes, est-ce assez pour lire la liste puis marcher jusqu'à
l'échoppe ? Allonger ce délai imposerait de passer en type 1 et d'ajouter un
minuteur serveur qui envoie les retraits — c'est un vrai petit chantier, pas une
correction.

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

### 3.3 `@storecard` — ✅ défaut de course **corrigé le 2026-09-02**

> Le correctif est appliqué côté serveur (`moonlight`, 4 fichiers) : le transfert
> est désormais différé jusqu'à `storage_premiumStorage_open()`, comme
> `@storeall N`. ⚠ **Non compilé, non testé en jeu** — voir §3.3 bis.
> Ce qui suit décrit le défaut tel qu'il était.

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

### 3.3 bis. Le correctif appliqué

Quatre fichiers du dépôt `moonlight` :

| fichier | changement |
|---|---|
| `src/map/pc.hpp` | `bool pending_storeall_cards_only` à côté de `pending_storeall` |
| `src/map/storage.hpp` | déclaration de `storage_premiumStorage_storecards()` |
| `src/map/storage.cpp` | la fonction (miroir de `storeall`, filtre `IT_CARD`, msg 1851) + l'aiguillage dans `storage_premiumStorage_open()` |
| `src/custom/atcommand.inc` | `@storecard` arme les deux drapeaux **avant** le `load` et retourne ; la boucle et le `close` immédiats sont supprimés |

`pending_storeall` reste **le** drapeau « un rangement est armé » — c'est lui que
testent les gardes des deux commandes, donc `@storeall` et `@storecard` ne
peuvent pas s'armer en même temps, et la garde existante de `@storeall` n'a pas
eu à changer. `pending_storeall_cards_only` dit seulement *lequel* des deux
transferts exécuter.

Deux ajouts au passage : la constante `STORECARD_STORAGE_ID` remplace le `5` en
dur, et une garde `storage_exists()` — sans elle, un coffre non déclaré
laisserait le drapeau armé sans que rien vienne le désarmer, bloquant les deux
commandes jusqu'à la déconnexion.

⚠ **Limite connue, non traitée** (elle vaut aussi pour `@storeall`) : si le
char-server ne répond jamais à `intif_storage_request`, le drapeau reste armé
pour la session. Le corriger demanderait un délai d'expiration ; c'est un autre
chantier.

⚠ **Ni compilé ni testé.** Le test tient en deux `@storecard` d'affilée après une
connexion fraîche : avant, le premier ne rangeait rien et ouvrait le coffre ;
après, il doit ranger et refermer dès le premier.

### 3.4 `@partybuff` — ⛔ **DÉSINSTALLÉE le 2026-09-02**

> Retirée à la demande de l'utilisateur plutôt qu'implémentée. Huit fichiers du
> dépôt `moonlight`, 47 lignes supprimées : la commande (`ACMD_FUNC` +
> `ACMD_DEF`), ses deux entrées de configuration (`atcommands.yml` avec ses
> alias `spb` / `showpartybuff`, `groups.yml`), et toute la plomberie devenue
> morte — le champ `sd->state.spb`, sa remise à zéro dans `party_member_withdraw`,
> les trois branches `PARTY_BUFF_INFO` de `clif_send` et la valeur
> d'énumération elle-même.
>
> ⚠ **Non compilé.** Les messages 1839/1840/1841 restent dans
> `conf/msg_conf/import/` : ils sont indexés par identifiant, donc les laisser
> orphelins ne décale rien.
>
> Ce qui suit décrit pourquoi elle a été retirée.

Elle basculait `sd->state.spb` et renvoyait la liste du groupe. Mais `sd->state.spb`
n'est lu qu'à un seul endroit : le `case PARTY_BUFF_INFO` de `clif_send`
(`clif.cpp:607`). Et **`PARTY_BUFF_INFO` n'a aucun émetteur** — vérifié :
4 occurrences dans tout `src/`, toutes dans `clif_send` (`:586`, `:601`, `:607`)
et dans l'énumération (`clif.hpp:229`). Aucun appel n'a jamais cette cible.

➡ Le joueur activait un réglage **sans effet**. Deux issues étaient possibles :
faire émettre les EFST des membres vers `PARTY_BUFF_INFO`, ou retirer la
commande. **C'est le retrait qui a été choisi.**

⚠ Si le besoin revient un jour — afficher les états des membres dans la fenêtre
de groupe — il faudra repartir du serveur, pas de ce drapeau : cf.
[[project_party_friend_window_re]], les états d'un membre hors de portée
n'arrivent pas par l'acteur, il faut un paquet qui les porte.

## 4. Ce que ça vaut

⚠ **Aucun de ces quatre n'était une panne visible.** `@shopsearch` répond,
`@ignore` fonctionne, `@storecard` marchait au second essai, `@partybuff`
affichait son message. Trois défauts discrets et un manque d'ergonomie.

**État au 2026-09-02** : `@storecard` est corrigé (§3.3 bis), `@partybuff` est
désinstallée (§3.4), et le prétendu défaut de `@shopsearch` était **une erreur de
lecture de ma part** — ses repères expirent déjà tout seuls (§3.1). Il ne reste
donc **qu'un seul défaut réel sur les quatre annoncés**, et il est déjà réglé.

Ce qui subsiste n'est pas un défaut mais un **manque** : `@shopsearch` et
`@ignore` n'ont aucune surface cliente.

Le plus rentable est probablement le plus petit : ajouter
**« Où l'acheter ? » → `@shopsearch <id>`** au menu d'un lien d'objet, à côté de
`@whodrops`. Le rendu (repères minimap + liens) existe déjà des deux côtés ; il
ne manque que le geste.

---

Voir aussi : [unexplored_systems.md](unexplored_systems.md),
[storage_window_re.md](storage_window_re.md),
[npc_progress_showscript_re.md](npc_progress_showscript_re.md).
