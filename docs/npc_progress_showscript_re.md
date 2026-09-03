# La barre de progression NPC et le texte flottant — deux surfaces jamais relevées

> Relevé du **2026-09-02**. Client `2025-07-16_Ragexe` (IDA, base 0x400000),
> serveur `moonlight` (fork rAthena).
>
> Quatre opcodes que [opcode_map.md](opcode_map.md) liste depuis toujours et
> qu'**aucun document de sujet** ne couvrait. Ils sont sortis d'un balayage
> « quelles commandes le serveur donne-t-il vraiment à ses joueurs ? », en
> remontant depuis `@searchid` (§4).

## 1. Les quatre opcodes

| opcode | dir | longueur | handler client | rôle |
|---|---|---|---|---|
| `0x02F0` | ZC | FIX 10 | case **752** @`0x00ca29ce` (en ligne) | **démarre** la barre de progression |
| `0x02F1` | CZ | FIX 2 | *(aucun — normal)* | le client **confirme** la fin |
| `0x02F2` | ZC | FIX 2 | case **754** @`0x00ca2a5f` (en ligne) | **annule** la barre |
| `0x08B3` | ZC | VAR | case **2227** → `ZC_ShowScript_Handler` `0x00d0bf50` | **texte flottant** au-dessus d'un acteur |

⚠ `0x02F1` tombe dans le `default` du répartiteur, et **c'est correct** : c'est un
`CZ`, le client l'**émet**, il ne le reçoit jamais. Ne pas le compter comme un
opcode « non traité ».

🔴 **À ne pas confondre avec la jauge d'incantation.** `UIRechargeGage`
(`0x013E` / `0x07FB` / `0x0B1A`, [cast_bar_re.md](cast_bar_re.md)) est la barre
de *cast* posée sur l'acteur. Celle-ci est la barre des **dialogues NPC**
(commande de script `progressbar`). Deux surfaces distinctes, deux jeux
d'opcodes.

## 2. `ZC_PROGRESS` / `ZC_PROGRESS_CANCEL` — tout passe par l'`OnMsg` de l'acteur

Les deux cas sont **écrits en ligne dans le répartiteur**, sans fonction dédiée.

```
0x00CA29CE   eax = [CGameMode+0xCC]          ; le CWorld
             ecx = [eax+0x2C]                ; l'acteur du JOUEUR
             si nul -> paquet suivant
             duree_ms = <champ .L du paquet> * 1000     ; le serveur envoie des SECONDES
             Actor::OnMsg (vtable+8) : msg = 0x52 (82), param = duree_ms
             g_ProgressBarActive (0x015FF906) = 1
```

L'annulation (`0x02F2`) reprend le même acteur et envoie **msg `0x53` (83)**,
puis, si `acteur+0x70 == 8`, appelle `vtable+0x3C` avec `(0, 1)` — une remise à
zéro d'état.

⚠ **Le bloc voisin `0x00CA2A15` fait la même chose pour un AUTRE acteur**
(`ActorList_FindByGID`, durée depuis `dword_15E81A2`). Deux `case` différents se
suivent dans la même zone : ne pas lire l'un pour l'autre.

Côté serveur : `clif_progressbar` → `0x02F0`, `clif_progressbar_abort` →
`0x02F2`, et `clif_parse_progressbar` reçoit le `0x02F1` du client
(`clif.cpp:16765`, `:16779`, `:16795`).

## 3. `ZC_SHOWSCRIPT` — le texte flottant

`ZC_ShowScript_Handler` `0x00d0bf50` :

```
len = packetLen - 8                       ; rien si len == 0
gid = <champ .L>
acteur = (gid == g_Account_Aid) ? this[51][11] : ActorList_FindByGID(this[51], gid)
memcpy(tampon_pile[256], paquet+8, len)
Actor::OnMsg (vtable+8) : msg = 7, param = tampon
```

Format : `<0x08B3>.W <len>.W <GID>.L <message>[len-8]`.

### ⚠ Le `memcpy` n'est pas borné — et pourquoi ça ne casse rien ici

La taille copiée vient du paquet ; la destination est un tampon de **pile de 256
octets**. Aucune vérification côté client.

La garde est **entièrement côté serveur** : `clif_showscript`
(`moonlight/src/map/clif.cpp:26342`) tronque à `sizeof(buf)-8` = **248**, ce qui
donne un `packetLen` maximal de 256 et donc `len ≤ 248`. **Marge : 8 octets.**

➡ Pas de débordement avec ce serveur. Mais la sûreté ne tient qu'à la troncature
serveur : à noter si Bourgeon devait un jour relayer ce paquet ou l'émettre.

## 4. Qui s'en sert sur Moonlight : `@searchid` / `@deepsearchid`

Ces deux commandes sont **écrites par l'utilisateur**, **ouvertes à tous les
joueurs**, et **annoncées dans le MOTD** :

```
moon/motd.npc:52   « Recherchez un item dans votre inventaire, storages et même
                     personnages avec @searchid et @deepsearchid »
```

- Implémentation : `moon/atcommands.npc:666` (NPC flottant `- script searchid -1`) ;
- liaison : `moon/atcommands.npc:1119-1120`,
  `bindatcmd("searchid", …)` / `bindatcmd("deepsearchid", …)` ;
- 🔴 **niveau 0** : `bindatcmd` sans 4ᵉ argument pose `level = 0`
  (`src/map/script.cpp:23127`), et le dispatch teste
  `pc_get_group_level(sd) >= binding->level` (`src/map/atcommand.cpp:12317`).
  Tout joueur peut donc la taper ;
- le fichier **est** chargé : `moon/scripts_moon.conf:9`.

La commande balaie l'inventaire, le chariot et les **six** stockages
(`conf/import/inter_server.yml` déclare 5 stockages premium en plus du natif) et
rend le résultat en **`dispbottom`, une ligne à la fois, avec `sleep2 100` entre
chacune** — plus `progressbar` et `showscript` pendant la recherche. D'où les
quatre opcodes ci-dessus.

### Ce que Bourgeon pourrait en faire

⚠ **Rien n'est cassé** : un joueur qui tape `@searchid` obtient sa réponse. Ce
serait de la **valeur ajoutée**, pas une réparation — à arbitrer contre les
chantiers qui, eux, sont en panne.

Le défaut d'usage est la **restitution** : des dizaines de lignes de chat espacées
de 100 ms noient la chatbox pendant plusieurs secondes. Bourgeon tient déjà
l'inventaire, le chariot et le stockage en ImGui ; une vue « recherche d'objet »
agrégée serait le remède naturel. Deux voies :

1. **écouter le fil de chat** — sans changement serveur, mais fragile (analyse de
   texte) ;
2. **un opcode à nous** portant le résultat structuré — propre, mais 🔴 **à
   ajouter AUX DEUX bouts**, sinon rien ne se passe en silence (cf.
   [[project_opcode_system]] : le prochain libre se prend dans `kNextFree`).

---

Voir aussi : [opcode_map.md](opcode_map.md), [cast_bar_re.md](cast_bar_re.md)
(la jauge d'incantation, à ne pas confondre),
[unexplored_systems.md](unexplored_systems.md).
