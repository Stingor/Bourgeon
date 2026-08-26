# Portage des adresses : client 2025-07-16 → 2026-07-07

Relevé du 2026-08-26, obtenu avec **deux IDA ouverts en parallèle**
(`mcp__ida-pro-mcp__*` → 13337 = 2025, `mcp__ida-2026__*` → 13338 = 2026).

## 🔴 Ce qui marche, et ce qui ne marche pas

| méthode | rendement | quand l'employer |
|---|---|---|
| **signature d'octets** (prologue masqué) | **4/18** | fonction non refactorée ; excellente DANS un même build (robuste aux patchs WARP) |
| **chaînes référencées** | **5/9** des restantes | dès que la fonction touche du texte — traverse un an de dérive sans problème |
| delta d'adresse global | **0** | inutile : les deltas trouvés vont de −14 448 à −218 688 |
| comptage d'accès `[reg+disp32]` | **bruit** | rejeté : maximise sur des offsets banals (`+0x94` sort 12 612 fois) |

➡ **Une signature d'octets ne porte pas d'un build à l'autre.** Elle sert à
retrouver une fonction dans le build courant. Pour le portage, ce sont les
chaînes, les appelants et les constantes — du diffing.

## Correspondances établies (9 + la famille packet-len)

| ancre `configuration.h` | 2025-07-16 | 2026-07-07 | preuve |
|---|---|---|---|
| `PacketLenLookup` | `0x00AA7B00` | `0x00AA4290` | signature 96 o, **identique à l'octet près** |
| `RecvOpcodeReader` | `0x00C144B0` | `0x00BDEE70` | signature 96 o |
| `RecvBufferReset` | `0x00C148B0` | `0x00BDF3D0` | signature 96 o |
| `ProcessPushButton` (`UIWindowMgr_OnKeyDown`) | `0x00A471E0` | `0x00A15160` | signature 26 o — ⚠ à revérifier |
| `GetTalkType` (`ChatCmd_LookupSlashCommandTable`) | `0x00D5E590` | `0x00C71310` | 5/5 chaînes (`/breakguild`, `/organize`, `/mapmove`, `/gocp`, `/ruftjd`) |
| `SendMsg` (`UIWindowMgr_ChatAction`) | `0x00A4AD20` | `0x00A18A20` | 3/3 chaînes (`No Msg`, `NO MSG`, `)  *^_^*`) |
| `CSession` (`CSession_ctor`) | `0x00D57780` | `0x00C6A730` | 4/4 chaînes (`ItemInfo file Init`, `TipBox file Init`…) |
| `RecvDispatchLoopHead` (`RecvLoop_DispatchPackets`) | `0x00C9E1DD` | `0x005095A0` | 1 chaîne + taille 52 074 o vs 50 141 o. ⚠ l'ancre 2025 est **interne** à la fonction |
| `CConnection` (`NetConnection_Connect`) | `0x00C13FC0` | `0x00BDECA0` | corps comparé : `socket`/`ioctlsocket`/`"Failed to setup select mode"`/`connect`/`WSAGetLastError==10035`. ⚠ **240 o contre 854 o** : la résolution DNS et le parsing `host:port` sont sortis de la fonction |

Famille packet-len (cf. [packet_len_diff.md](packet_len_diff.md)) :

| | 2025 | 2026 |
|---|---|---|
| `g_PacketLenTable` | `0x0159D68C` | `0x0146EDFC` |
| `PacketLenTable_Fill` | `0x00AA0090` | `0x00A9C4D0` |
| `PacketLenTable_Insert` | `0x00A9FF30` | `0x00A9C370` |
| `RecvBuffer_ReadPacket` | `0x00C147D0` | `0x00BDF310` |

Toutes sont **nommées et commentées dans l'IDB 2026**, avec l'adresse 2025 et le
niveau de confiance en commentaire de fonction.

## Restent à porter (9)

`UIWindowMgr` `0x00A29BA0` · `Switch`/`CModeMgr_Run` `0x00A756E0` ·
`RenderCellsAndCursor` `0x00A7B0A0` · `SendPacket` `0x00C14920` ·
`GameMode_OnUpdate` `0x00C74A80` · `PostActorClickAction` `0x00C753A0` ·
`ProcessInput` `0x00C86740` · `LoginMode_OnUpdate` `0x00D272E0` ·
`RecvDispatchTable` `0x00CAA2E0`

Aucune ne référence de chaîne distinctive. Prochaine piste : leurs **appelants**
et leurs **callees déjà portés** (`SendPacket` est appelé par tout le monde et
vit dans la famille `0x00BDExxx` du build 2026, à côté de `NetConnection_Connect`).
⚠ `RecvDispatchTable` est une **table de données**, pas une fonction : autre méthode.

## 🔴 Le TROISIÈME chantier : les 110 patchs WARP

Le portage a trois volets, pas deux :

| volet | ampleur | méthode |
|---|---|---|
| adresses Bourgeon | **407** (`layouts.h`) / 480 déclarations mesurées | diffing, cf. ci-dessus |
| layout `CSession` | ~20 champs | **scan mémoire en jeu** (c'est ainsi qu'ils ont été trouvés la 1ʳᵉ fois) |
| **patchs WARP** | **110 actifs** (`docs/warp_patches.md`, source `E:\Nouveau dossier\save_exe.yml`) | voir ci-dessous |

🔴 **Bonne nouvelle : WARP ne travaille pas par adresse en dur.** Les `.qjs`
localisent leur cible par **recherche de motif**. Beaucoup de patchs devraient
donc s'appliquer tels quels sur le nouvel exe — c'est le mécanisme même qui rend
WARP portable d'un client à l'autre.

➡ **Le test est direct et peu coûteux : charger `D:\KRO\Ragexe.exe` dans WARP et
compter les patchs qui se valident.** Le résultat chiffre le volet en une passe.

⚠⚠ **Un patch dont la cible est introuvable échoue EN SILENCE** : `validate()`
rend faux sans rien dire si le script n'appelle pas `Cancel("raison")`. Ne PAS
conclure « tout est passé » depuis l'absence d'erreur — comparer la liste des
patchs appliqués à la liste attendue, patch par patch. Cf.
[[reference_warp0716]] et `docs/warp_patches.md`.

⚠ Les patchs qui **changent le comportement** d'une fonction (et pas un simple
réglage) sont ceux à revérifier en priorité : la section « pièges » de
`warp_patches.md` les liste. Un patch qui trouve un motif *voisin mais faux* dans
le nouvel exe est plus dangereux qu'un patch qui échoue franchement.

## ⓘ La base de départ change de nature

`save_exe.yml` part de `2025-07-16_Ragexe_175220998_unpacked_community.exe` :
l'exe actuel est un **dump reconstruit par la communauté**, d'où l'absence totale
de répertoire debug et la présence des sections `.lotus`/`.xdiff`.

Le build 2026-07-07 n'a **jamais été packé** (CodeView, POGO, table d'export et
relocations intactes, 1384 classes RTTI). La base de départ est donc plus propre
qu'aujourd'hui : plus de stubs virtualisés en bas de `.text`, plus de
`halt_baddata()`. Cf. [[project_client_install_path]],
[[reference_lotus_virtualization_low_text]].

## 🔴 Chiffrement des opcodes : le mécanisme existe, il est NEUTRE

`CRagConnection_SendPacket` (`0x00C14920`, build 2025) XORe l'opcode sortant :

```c
result = PacketKeyLCG_Next(dword_15BEE9C, 0);
*param_2 ^= result;
```

`PacketKeyLCG_Next` (`0x00AB32A0`) est le LCG canonique de Gravity :
`v2 = key3 + key1 * key2` puis `return HIWORD(v2) & 0x7FFF`.

**Mais les trois clés valent zéro.** L'allocateur (`0x00AB31C0`) les met à 0, et
les trois sites qui touchent l'objet (`CLoginMode_OnStateEnter` ×2, `sub_C9C360`)
appellent `PacketKeyLCG_Next(this, 1|2)` — le **mode RESET**, qui les remet à
zéro à chaque connexion. Donc `HIWORD(0) & 0x7FFF == 0` et le XOR est neutre.
C'est cohérent avec la chaîne `No Packet Encryption !!!` du binaire.

➡ **Rien à extraire pour rAthena** — mais si un jour ces clés deviennent non
nulles, c'est ici que ça se voit.
