# Recherche de groupe — DEUX générations, et pourquoi aucune ne marche

> Relevé du **2026-08-31**. Client `2025-07-16_Ragexe` (IDA, base 0x400000),
> serveur `moonlight`.
>
> Fait suite à [unexplored_systems.md](unexplored_systems.md), qui désignait
> cette famille comme « la prochaine cible » sur la seule foi des paquets
> enregistrés des deux côtés. **Ce relevé corrige cette recommandation** : le
> protocole existe des deux côtés, et pourtant rien ne peut fonctionner. Les
> raisons sont intéressantes, la conclusion pratique l'est moins.

## 1. Il y a deux systèmes, pas un

Les neuf fenêtres que `native_window_dispatch.md` §8 range sous « recherche de
groupe » sont **deux générations successives** que le binaire porte toutes deux :

| | génération **legacy** | génération **moderne** |
|---|---|---|
| fenêtres | 164 `UISeekPartyWnd`, 168 `UISeekPartyMBWnd`, 169 `UISeekPartyListWnd`, 170 `UIJobListWnd`, 173 `UIPartyBookingHelpWnd` | 324 `UIAdvenPartyBoardWnd`, 326 `UIRequestJoinPartyWnd`, 345 `UIApplyForPartyWnd` |
| transport | **paquets** `0x0802`..`0x080B` | 🔴 **HTTP** — pas le serveur de map |
| ouverture | aucun raccourci, aucune commande | **raccourci** (`UIWindowMgr_DispatchHotkeyBehavior` 0x00a46106) **et** commande (`Chat_HandleChatMessage` 0x00c7cbb7) **et** la fenêtre de groupe (`UIMessengerGroupWnd_OnMsg` 0x0070776f) |

Le test d'ouverture est la preuve la plus nette : les 28 sites qui poussent un id
de fenêtre vers le gestionnaire ont été ouverts un par un. **Seule la génération
moderne est atteignable par un joueur.** La legacy ne s'ouvre que depuis ses
propres fenêtres — ou depuis le handler de `ZC 0x0803`, c'est-à-dire une réponse
du serveur à une requête que le joueur ne peut pas émettre.

## 2. La génération moderne est un service WEB

`UIAdvenPartyBoardWnd` est servie par `CAdventurerAgencyMgr`
(`CAdventurerAgencyMgr_CreateInstance` 0x005ac090, module 0x005AC000-0x005B0000)
qui ne parle pas au serveur de map du tout : il fait des **POST multipart
libcurl** via `AdventurerAgency_BuildWebUrls` 0x005ac1a0.

Sept endpoints, chemins **codés en dur**, préfixés par `AssistAddr` :

```
/party/add   /party/list   /party/search   /party/del
/party/get   /party/change /party/info
```

⚠ Ce n'est **pas neuf** : `docs/external_settings_re.md` §4 documente déjà
`AssistAddr` et liste ces sept chemins parmi les six `*_BuildWebUrls`. Ce que ce
relevé ajoute, c'est **le lien** : ces endpoints sont le moteur de la fenêtre
324, donc « la recherche de groupe » de ce client est un chantier **web**, pas un
chantier de protocole.

**Ce qu'il faudrait pour l'allumer** : un service HTTP qui réponde à ces sept
routes, et `AssistAddr` (globale Lua ou `AssistAddrTbl`) pointant dessus.
Moonlight n'a rien de tel — un `grep` de `clif_packetdb.hpp` ne rend que
`0x0ae8 changedress`, aucun endpoint d'agence.

## 3. 🔴🔴 La génération legacy : l'opcode d'inscription est un trou

C'est le résultat le plus concret du relevé.

`UISeekPartyMBWnd_OnMsg` (0x009002f0, bouton **184**) construit bien la requête
d'inscription : il valide le niveau saisi, agrège jusqu'à six identifiants de
métier, puis :

```c
param_2 = 2050;                       // 0x0802
*(_OWORD *)v56 = *(_OWORD *)(this+361);   // 16 octets de charge utile
v35 = PacketLen_Get(2050);
CRagConnection_SendPacket(instance, v35, &param_2);
```

Soit **2 + 16 = 18 octets** à envoyer. Mais :

**`PacketLenTable_Init` (0x00aa0090) n'enregistre aucune longueur pour 0x0802.**

Ce n'est pas une supposition, c'est un trou isolé dans une suite strictement
contiguë — contrôle positif fait sur chaque voisin :

```
00AA311C  push 800h      ; enregistré
00AA312E  push 801h      ; enregistré
00AA3140  push 803h   <-- 0x0802 SAUTÉ
00AA3152  push 804h      ; enregistré
...       jusqu'à 80Bh   ; tous enregistrés
```

Or `PacketLen_Get` (0x00c14460) sur un opcode inconnu tombe sur le couple
`(-1, 4)` de `PacketLenTable_Lookup` et **renvoie 4**
(cf. [[reference_native_packet_len_resolver]]). Le client envoie donc
**4 octets au lieu de 18**.

Et côté serveur, pour ce packetver, `0x0802` **n'est plus** l'inscription :

```c
// clif_packetdb.hpp:1304, #if PACKETVER >= 2009122
parseable_packet(0x0802,18,clif_parse_PartyBookingRegisterReq,2,4,6);
// clif_packetdb.hpp:1555, #if PACKETVER >= 20120418   <-- gagne
parseable_packet(0x0802,26,clif_parse_PartyInvite2,2);
```

La dernière inscription l'emporte : sur Moonlight, `0x0802` est
**`PartyInvite2`, longueur fixe 26**. Un client qui enverrait 4 octets sur cet
opcode laisserait le parseur du serveur attendre 22 octets de plus, qu'il
prendrait dans les paquets suivants — un désynchronisation du flux.

⚠ **En pratique le risque est nul**, précisément parce que §1 a montré que la
fenêtre 168 n'est atteignable par aucun geste du joueur. Le code est mort. Mais
il documente une règle : **un site d'émission qui existe encore ne prouve pas
que l'opcode existe encore.**

## 4. Ce qui, lui, correspond des deux côtés

Les trois autres opcodes legacy sont sains et alignés :

| opcode | client (table) | rAthena | handler client |
|---|---|---|---|
| `0x0804` CZ recherche | FIX **14** | `clif_parse_PartyBookingSearchReq`, **14** | — |
| `0x0806` CZ suppression | FIX **2** | `clif_parse_PartyBookingDeleteReq`, **2** | — |
| `0x0808` CZ mise à jour | FIX **14** | `clif_parse_PartyBookingUpdateReq`, **14** | — |
| `0x0803` ZC ack inscription | FIX 4 | — | `sub_CCFF60` 0x00ccff60 |
| `0x0805` ZC résultat recherche | VAR | — | `sub_CD0020` 0x00cd0020 — enregistrements de **0x30 octets** |
| `0x0807` ZC ack suppression | FIX 4 | — | `sub_CCFEC0` |
| `0x0809` ZC insertion | FIX 50 | — | `sub_CD0140` — **48 octets** = un enregistrement |
| `0x080A` ZC mise à jour | FIX 18 | — | `sub_CD0190` |
| `0x080B` ZC retrait | FIX 6 | — | `sub_CD0100` |

Les ZC alimentent tous la fenêtre **169** (`UISeekPartyListWnd`) et la
rafraîchissent via `vtable+152`.

## 5. Conclusion pratique

**Ce n'est pas le prochain chantier.** Les deux voies sont fermées, pour deux
raisons différentes :

- la voie **moderne** demande d'écrire un **service web** à sept routes et de
  pointer `AssistAddr` dessus — un chantier serveur/web, pas un chantier client ;
- la voie **legacy** a un opcode d'inscription qui n'existe plus dans la table du
  client, une interface qu'aucun geste n'ouvre, et un `0x0802` que le serveur
  interpréterait comme un `PartyInvite2`.

Si l'on veut une recherche de groupe sur Moonlight, la piste la moins coûteuse
n'est aucune des deux : c'est une fenêtre Bourgeon en ImGui sur un opcode à nous,
comme l'ont été le carnet MVP et le salon de chat. Le natif ne fait ici que du
tort — ce qui est la règle établie par [[feedback_native_replacement]].
