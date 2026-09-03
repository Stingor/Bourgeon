# `UIGuild_Storage_Log` (253) — le journal du stockage de guilde

> Relevé du **2026-09-02**. Client `2025-07-16_Ragexe` (IDA, base 0x400000),
> serveur `moonlight` (fork rAthena, `PACKETVER 20250716`).
>
> Solde le dernier point « non tranché » de
> [unexplored_systems.md](unexplored_systems.md) §4 : « `clif_guild_storage_log`
> existe côté serveur, mais le paquet n'a pas été retrouvé dans la table du
> client et le lien avec cette fenêtre n'est pas établi ».
>
> **Verdict : le lien est établi, et c'est une fonctionnalité VIVANTE que le
> joueur peut ouvrir aujourd'hui.** Elle n'avait jamais été documentée.

## 1. Pourquoi elle est vivante — l'ouvreur est un NPC de Moonlight

Contrairement aux trois candidates de `itemmall`, celle-ci a un ouvreur **chargé
et atteignable** — et le dialogue est écrit en français par l'utilisateur, donc
c'est une fonctionnalité **voulue** :

```
moon/kafra.npc:96      (chargé par moon/scripts_moon.conf:19)
    case 3:
        guildstoragecountitem(501, getcharid(3));
        if( guild_has_permission(GUILD_PERM_ALL) ) {
            guildopenstorage_log();
            close2;
        }
        else { ... "Vous ne semblez pas avoir les droits..." }
```

`guildopenstorage_log` → `storage_guild_log_read` (`storage.cpp:913`) →
`clif_guild_storage_log` (`clif.cpp:27925`) → **ZC `0x09DA`**.

⚠ **Gate : `GUILD_PERM_ALL`.** Seul un joueur ayant *toutes* les permissions de
guilde voit le journal. Rappel : les permissions rAthena se testent en **OU**
(cf. [[reference_rathena_group_permission_or]]).

## 2. Côté client — case 2522, `ZC_GuildStorageLog_Handler` 0x00cb1d50

Le paquet **est** dans le répartiteur : `case 2522` à `0x00ca8dbb`
(`RecvLoop_DispatchPackets`), qui appelle `0x00cb1d50` (0x9ac octets).

🔴 **La fenêtre n'est ouverte que si `result == 1`.** C'est un protocole **en
plusieurs envois** :

| `result` | enum rAthena | ce que fait le client |
|---|---|---|
| 0 | `GUILDSTORAGE_LOG_SUCCESS` | **accumule** les entrées dans la liste globale `dword_15FFFE0`, **n'ouvre rien** |
| **1** | `GUILDSTORAGE_LOG_FINAL_SUCCESS` | accumule, trie (`sub_C989D0`), **scinde en deux listes** selon `action` (dépôt / retrait), puis **`MakeWindow(0xFD)` = 253** |
| 2 | `GUILDSTORAGE_LOG_EMPTY` | message **`0x9EF`**, vide les trois listes |
| ≥ 3 | `GUILDSTORAGE_LOG_FAILED` | message **`0x718`**, vide les trois listes |

Et côté serveur, `clif_guild_storage_log` ne remplit `items[]` **que** pour
`GUILDSTORAGE_LOG_FINAL_SUCCESS` — dans tous les autres cas `amount = 0`.
**Les deux bouts s'accordent exactement.**

⚠ Conséquence pratique : `storage_guild_log_read` n'envoie qu'**un seul** paquet
et le fait avec le `result` rendu par `storage_guild_log_read_sub`. Si ce
`result` valait `SUCCESS` (0), la fenêtre ne s'ouvrirait **jamais**, en silence.
C'est le point de rupture à surveiller si le journal « ne fait rien ».

## 3. L'entrée — 93 octets, alignement champ par champ

| offset | champ rAthena | lecture du client |
|---|---|---|
| +0 | `id` (u32) | `*(DWORD*)(v4-4)` |
| +4 | `itemId` (u32) | `*(DWORD*)v4` → `_itoa` → chaîne |
| +8 | `amount` (i32) | `*(DWORD*)(v4+4)` |
| +12 | `action` (u8) — 1 = dépôt, 0 = retrait | `*(BYTE*)(v4+8)`, sert à **scinder les deux listes** |
| +13 | `refine` (i32) | `*(DWORD*)(v4+9)` |
| +17 | `uniqueId` (i64) | **jamais lu** |
| +25 | `IsIdentified` (u8) | `*(BYTE*)(v4+21)` |
| +26 | `itemType` (u16) | `*(WORD*)(v4+22)` |
| +28 | `slot` (`EQUIPSLOTINFO`, **4 × u32**) | `+24`, `+32`, `+36` |
| +44 | `name[24]` | copie de chaîne depuis `v4+40` |
| +68 | `time[24]` | copie de chaîne depuis `v4+68` |
| +92 | `attribute` (u8) | `*(BYTE*)(base+88)` |

**12 champs, 0 divergence.** L'en-tête fait 8 octets
(`packetType`, `PacketLength`, `result`, `amount`), les entrées commencent donc
à `+8` — ce que le client confirme (`v2 = a1 + 8`).

Le pas de **93** octets impose `EQUIPSLOTINFO` à **16** octets
(93 − 29 − 2×24 = 16), soit quatre identifiants de carte sur 4 octets : c'est
bien la variante moderne, cohérente avec `PACKETVER 20250716`.

## 4. Ce que Bourgeon en fait — rien, et c'est très bien

- La fenêtre **253** n'est **pas** dans le barrage de `MakeWindow`
  (cf. [native_window_dispatch.md](native_window_dispatch.md) §3, 9 ids
  refusés) ;
- **aucune ligne de `src/`** ne cite 253, `0x09DA` ni le journal de guilde ;
- elle est **indépendante** de `UIItemStoreWnd` (33), la fenêtre de stockage que
  Bourgeon a remplacée : le handler l'ouvre directement, sans passer par elle.

⇒ Elle fonctionne aujourd'hui, telle quelle. **Rien à corriger.**

Fenêtre : ctor `0x0090c860`, vtable `0x01039cd4`, taille `0x190`, slot `+0x44C`.

## 5. 🔴 Le seul point à vérifier : la table SQL

`storage_guild_log_read_sub` interroge la table nommée par
`guild_storage_log_table` (`map.cpp:98`, défaut **`guild_storage_log`**,
surchargeable par `inter_conf`). L'écriture (`storage_guild_log`,
`storage.cpp:819`) est **inconditionnelle** : aucun réglage ne la coupe.

⚠ **Le dépôt ne porte pas le schéma.** `sql-files/` ne contient que des fichiers
custom (`mvp_tracker.sql`, `item_db2.sql`…) ; ni `main.sql` ni `logs.sql` n'y
sont. **On ne peut donc rien conclure du dépôt** — l'absence de
`CREATE TABLE guild_storage_log` n'y prouve pas l'absence de la table sur le
serveur (c'est [[feedback_absence_needs_measurement]] : la recherche est vide
parce que le fichier entier manque).

➡ La vérification tient en une requête sur le serveur :

```sql
SHOW TABLES LIKE 'guild_storage_log';
SELECT COUNT(*) FROM guild_storage_log;
```

Si la table manque, la requête échoue → `GUILDSTORAGE_LOG_FAILED` → message
`0x718`, et le joueur voit une erreur au lieu du journal.

## 6. Les deux autres points « non tranchés », soldés au passage

### `UIItemStoreSubWnd` 146-152 + 309, `UIItemStoreFindWnd` 153 — des internes

Ce ne sont **pas** des sous-onglets d'un système à part : ce sont les
**sous-panneaux de la fenêtre de stockage native**, `UIItemStoreWnd` (**33**,
ctor `0x00934ae0`). Preuve directe — un même `OnMsg` les ferme **en bloc** :

```
0x954690   boucle esi = 0..6 :  SaveRectAndCloseWindow(esi + 0x92)   ; 146..152
0x9546A7                        SaveRectAndCloseWindow(0x135)        ; 309
0x9546C1                        SaveRectAndCloseWindow(0x99)         ; 153
```

Or `UIItemStoreWnd` est précisément la fenêtre que Bourgeon a remplacée — « la
fenêtre native ne naît plus » (`src/features/windows/storage_window.cc:53`).
⇒ **Aucun chantier** : ce sont les entrailles d'une native déjà morte.

⚠ Piège de nommage : `UIItemStore*` désigne l'**entrepôt**, pas une boutique,
malgré le voisinage de `UIItemShopWnd` / `UIItemSellWnd` dans le binaire.

### `CUIRenewQuestUI` / `CUIOngoingQuestInfo` / `CUIRecommendedQuestInfo` (10008-10010)

Le seul déclencheur serveur possible est la commande de script **`open_quest_ui`**
(`script.cpp:27264`). **Aucun script chargé ne l'appelle** — vérifié avec témoin
positif (le même motif trouve bien les appels dans `npc/`, non chargé).
⇒ Pas de transport serveur. L'ouverture depuis l'interface de quête du client
reste possible en théorie, mais elle passerait par le registre `CUI`
(étage [3] du répartiteur) et personne ne l'a observée.
**Classée « sans serveur » tant qu'on ne l'a pas vue s'ouvrir en jeu.**

---

Voir aussi : [unexplored_systems.md](unexplored_systems.md),
[storage_window_re.md](storage_window_re.md),
[native_window_dispatch.md](native_window_dispatch.md),
[merge_item_re.md](merge_item_re.md).
