# Protocole du client 2026 : ce qui a bloqué l'entrée en jeu

Relevé du **2026-08-27**. Le client 2026-07-07 entre en jeu et se déplace sur
un serveur Moonlight. Quatre défauts empilés le bloquaient ; les trois premiers
se manifestaient tous **loin de leur cause**.

## 🔴🔴 1. Le client ajoute un octet de contrôle à CHAQUE paquet envoyé

C'est la cause racine, et le seul des quatre à ne pas être un problème de
longueur déclarée. Dans la routine d'émission `sub_BDF440`, quand le drapeau
`+0x6D` de la socket est armé :

```c
v6 = Size + 1;                        // on réserve un octet de plus
memcpy(dst, Src, Size);               // la charge utile
if (!compteur)  dst[Size] = rand();   // le TOUT PREMIER paquet
else            dst[Size] = hash(Src, Size, clé);   // constantes wyhash
send(dst, fin - dst);                 // Size + 1 part sur le fil
```

Un serveur qui l'ignore lit **un octet de trop peu par paquet**, et le décalage
s'aggrave à chaque paquet : 1 octet après le `CZ_ENTER`, 4 octets après quatre
paquets.

**Pourquoi ça a résisté quatre tours.** Le symptôme est un opcode absurde
(`0x6081`, puis `0x7d81`, puis `0x7df1`) apparaissant à un endroit sans rapport
avec la cause. Et l'octet parasite **change à chaque session**, puisque sur le
premier paquet c'est un `rand()` : impossible de le reconnaître comme un opcode
fixe. Trois paquets innocents ont été incriminés avant de remonter à la routine
d'émission.

**Le témoin qui tranche** : le motif est **absent du client 2025-07-16**. C'est
nouveau, et c'est exactement pourquoi un serveur qui parle à l'ancien client
rejette celui-ci.

Correctif : patch WARP `NoPacketTrailByte` (WARP0716, `af30403`). Il force la
branche du lecteur plutôt que de retirer le store qui arme le drapeau :

```
80 7B 6D 00     cmp  byte ptr [ebx+6Dh], 0
56              push esi
57              push edi
0F 84 <rel32>   jz   _send_plain      <- forcé en JMP
```

🔴 **Une première version retirait le seul `mov byte ptr [edi+6Dh], 1` de
l'image, et l'octet sortait quand même** — vérifié dans l'exe généré, qui
portait bien les quatre NOP. Quelqu'un d'autre arme ce drapeau : un store depuis
un registre, un déplacement plus large, une copie de structure. Chercher **un
encodage d'une instruction** ne prouve jamais qui écrit un octet. Forcer le
lecteur ne dépend pas de la réponse, et c'est le changement le plus étroit : les
deux autres sites qui lisent `+0x6D` pour des décisions sans rapport continuent
de voir le drapeau qu'ils attendent.

## 2. `CZ_ENTER` = 0x0C1F, 1000 octets, tick à +18

Le client 2026 a fusionné la demande d'entrée en jeu avec ses jetons EOS.

| offset | champ |
|---|---|
| +0 | opcode `1F 0C` |
| +2 | AID |
| +6 | CID |
| +10 | login_id1 |
| +14 | **jamais écrit** (reste à zéro) |
| +18 | tick (`timeGetTime`) |
| +22 | sex |
| +23 | ProductUserId, 33 o (`21h`) — « nullptr » sans la DLL EOS |
| +56 | IdToken, 944 o (`3B0h`) — « ERROR » sans la DLL EOS |

56 + 944 = **1000**, recoupé par la table du client (`PacketLenTable_Insert`
en `0x00AA368D` : `push 0 / push 3E8h / push 3E8h / push 0C1Fh`).

🔴 **Ne pas prendre le « N bytes received » du log rAthena pour la longueur du
paquet** : c'est le `RFIFOREST`, donc le paquet **plus ce qui suit**. Déclaré à
1001 sur la foi de ce chiffre, le paquet mangeait un octet de trop.

## 3. Le paquet de connexion était rejeté parce qu'un autre le suivait

`clif_parse_WantToConnection_sub` comparait `RFIFOREST` à la longueur déclarée
et rejetait sur toute différence — le paquet de connexion devait donc arriver
**seul**. Le client 2026 colle un second paquet derrière son `CZ_ENTER` dans le
même segment TCP.

Le cas « trop court » est déjà traité en amont (`clif_parse` attend d'avoir
`packet_len` octets avant d'appeler le handler), donc ce test ne pouvait échouer
que par **excès**. Passé de `!=` à `<` (moonlight `13f0229d0`).

## 4. `ZC_ACH_UPDATE 0x0A24` fait planter le client

Découvert une fois le flux aligné, en jouant. Pile du crash :

```
WinMain → sub_A3C8C0 → sub_4CED50 → GameMode_InGame_ProcessFrame
        → RecvLoop_DispatchPackets (case 2596 = opcode 0x0A24)
        → sub_566B20 → sub_C92920 ✗
```

⚠ Le rapport de crash affiche `RendererDX9.cpp 158`, ce qui **n'est pas** le
lieu du défaut : la pile est entièrement dans le traitement d'un paquet reçu.

Deux hypothèses **testées et écartées** :

- *la longueur aurait changé* — non : **66 octets** dans les trois sources
  (client 2025, client 2026, `packet(0x0A24,66)` de rAthena) ;
- *`sub_C92920` renverrait null* — non : c'est une recherche dans une
  `std::map` qui remplit son tampon de sortie de zéros quand la clé est absente,
  et retourne toujours ce tampon.

Le crash est **dans** `sub_C92920` : `eax` vaut le tampon local `v33`
(`ebp-0x114`), que la fonction assigne tôt, et `edx = 0` — piste d'une map non
initialisée. Non résolu ; demande x32dbg attaché.

**Contournement mesuré** : `feature.achievement: off` dans
`conf/import/battle_conf.txt` ⇒ plus aucun crash. Le réglage existe déjà dans
rAthena, aucun patch n'est nécessaire.

⚠ Ce réglage vaut pour **tout** le serveur, client 2025 compris.

## Relevé des longueurs — deux remplisseurs, pas un

Correction du relevé du 2026-08-26, qui partait d'**une** fonction de
remplissage. Il y en a **deux** dans chaque build :

| | 2025-07-16 | 2026-07-07 |
|---|---|---|
| `PacketLenTable_Fill` | `0x00AA0090`, 1522 | `0x00A9C4D0`, 1578 |
| `PacketLenTable_Fill2` | `0x00AA6C10`, **31** | `0x00AA3440`, **202** |
| total (dédoublonné) | **1553** | **1778** |

Les comptes du relevé précédent (1522 / 1577) sont exactement ceux du **premier
remplisseur seul**. ⇒ Partir des **xrefs vers `Insert`**, jamais d'une fonction :
la question « qui remplit la table » n'a pas à être répondue d'avance.

Un seul conflit, en 2026 : opcode `0x0C47` inséré deux fois avec le même couple
de longueurs mais un drapeau différent (`[4,4,0]` puis `[4,4,1]`).

Fichiers : [packet_len_client_2025.json](packet_len_client_2025.json),
[packet_len_client_2026.json](packet_len_client_2026.json).

### 28 opcodes changent de longueur entre les deux clients

Sur 1548 opcodes communs — soit **1,8 %**. Le protocole n'a donc pas été
re-mélangé, ce qui recoupe le relevé d'opcodes existant.

```
0x0072 22->19   0x007E 46->6    0x0085 10->5    0x0089 11->7
0x008C 14->-1   0x0094 19->6    0x009B 34->5    0x009F 20->6
0x00A2 14->6    0x00A7  9->8    0x00F3 -1->8    0x00F5 11->8
0x00F7 17->2    0x0113 25->10   0x0116 17->10   0x0190 23->90
0x0191 27->86   0x0193  2->6    0x0206 35->11   0x0288 -1->12
0x02E2 20->8    0x02E3 22->10   0x02E4 11->6    0x02E5  9->5
0x0367 31->90   0x07EC -1->8    0x0C12  7->16   0x0C26 94->67
```

🔴 **Ce n'est PAS une liste de régressions.** Savoir lesquelles mordent
demanderait la longueur que `clif_packetdb.hpp` déclare **activement** sous
`PACKETVER 20260707`, ce qui suppose d'évaluer ses 144 `#if PACKETVER`. Un
évaluateur a été écrit puis **rejeté par son propre témoin** : appliqué au
couple 20250716 ↔ client 2025 — qui fonctionne en production — il signalait
61 divergences sur 582 opcodes, soit 10,5 % de faux positifs. Confronté aux
cinq opcodes réellement mesurés sur le fil, il se trompait sur `0x035F`
(annoncé « serveur = 6 », alors que la trace du serveur affiche `len=5`).

⇒ Le relevé brut ci-dessus est mesuré et sûr ; toute conclusion « le serveur
diverge sur tel opcode » est à refaire par une **mesure sur le fil**, pas par
analyse statique du préprocesseur.

## Outil : tracer le flux entrant

`BOURGEON_PACKET_TRACE` (moonlight `src/custom/defines_pre.hpp`, commenté par
défaut) journalise chaque paquet consommé — opcode, longueur, reste du tampon,
8 premiers octets. C'est ce qui a permis de voir l'octet de queue.

Lecture : le coupable d'un désalignement est le **dernier opcode plausible avant
le premier opcode aberrant** ; un `rest` supérieur à `len` sur le premier paquet
d'une session signale un octet de trop.

Flux nominal après correctifs :

```
pkt 0x0c1f len=1000 rest=1000
pkt 0x007d len=2    rest=2
pkt 0x0360 len=6    rest=6
pkt 0x08c9 len=2    rest=2
pkt 0x035f len=5    rest=5     <- déplacement, position décodée = celle de la minimap
```
