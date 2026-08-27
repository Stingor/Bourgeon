# Les paquets que le client 2026 ne traite PLUS

Relevé du **2026-08-27**, à l'occasion du portage du serveur. C'est le piège le
plus coûteux du chantier, et le seul que l'analyse des **longueurs** ne pouvait
pas voir.

## 🔴🔴 Une longueur inchangée ne dit rien du traitement

Le relevé `packet_len_diff.md` concluait : « aucun opcode utilisé par Moonlight
ne change de longueur ». C'était **exact et trompeur**.

`0x0AC4` (`AC_ACCEPT_LOGIN`) déclare la même longueur dans les deux builds, son
`case` existe toujours dans le dispatch, et il appelle bien la fonction de
parsing. Mais dans cette fonction :

| | 2025 | 2026 |
|---|---|---|
| `case 5` (0x0AC4) | `nb = (len−64)/0xA0` ; `memcpy(…, 160×nb)` | **`break;`** |
| `case 4` (0x0AC9) | traité | **`break;`** |
| `case 6..9` | traité | **`break;`** |

Le client reçoit, ne fait rien, et n'affiche aucune erreur. Côté serveur, le log
dit « Authentication accepted ». Le symptôme est une **liste vide**, pas un
échec — le pire cas pour diagnostiquer.

➡ **Comparer les tables de longueurs ne suffit pas. Il faut comparer les
traitements.**

## Le détecteur

Pour chaque opcode, mesurer le **volume de code réellement atteint** depuis son
`case` (instructions du case + poids des fonctions appelées), dans les deux
builds. Un effondrement signale un traitement supprimé.

Scripts : `scripts/login_case_weight.py` (dans chaque IDA),
`scripts/login_regress.py` et `scripts/game_regress.py` (local).

| dispatch | 2025 | 2026 |
|---|---|---|
| login / char (`LoginCharMode_RecvDispatch`) | `0x00D27560` | `sub_C3A670` `0x00C3A670` |
| jeu (`RecvLoop_DispatchPackets`) | `0x00C9DF00` | `0x005095A0` |

## Ce qui bloquait la connexion (corrigé)

| paquet | 2025 attend | 2026 attend | correctif serveur |
|---|---|---|---|
| `AC_ACCEPT_LOGIN` | `0x0AC4`, en-tête 64, **160** o/serveur | **`0x0069`**, en-tête 47, **32** o/serveur | `moonlight 6e1407d45` |
| `HC_ACCEPT_ENTER` | `(len−27)/175` | **`(len−7)/175`** | `moonlight e893c8103` |

Le second est le `extension[20]` que rAthena ajoute pour les clients récents et
que le 2026 n'attend plus. **La taille de `CHARACTER_INFO` est correcte** (175
octets, recalculée champ par champ à ce `PACKETVER`) : seul l'en-tête était en
cause.

✅ Les deux correctifs sont conditionnés à `PACKETVER < 20260707` — la
production 2025 n'est pas touchée.

## Ce qui reste, sans bloquer la connexion

Dispatch **login** — `0x0071` (`HC_NOTIFY_ZONESVR`) s'effondre (380 → 115),
mais `0x0AC5` (la variante moderne) est **conservée** (143 → 169) et c'est celle
que rAthena envoie à ce `PACKETVER`. Sa longueur, 156, est identique des deux
côtés (2+4+16+4+2+**128** de `domain`). Rien à faire.

Dispatch **jeu** — 23 opcodes effondrés, dont **4 déclarés par Moonlight** :

| opcode | poids 2025 → 2026 | à vérifier |
|---|---|---|
| `0x0101` | 403 → 68 | infos de groupe / guilde |
| `0x0229` | 377 → 60 | `ZC_STATE_CHANGE3` — états visuels d'acteur |
| `0x08FE` | 286 → 92 | quêtes de chasse |
| `0x02D9` | 36 → 4 | notification de configuration |

⚠ **Ce sont des candidats, pas des verdicts.** Le poids est une approximation
(instructions du case + 8 par appel, BFS borné) : un `case` qui délègue à une
grosse fonction pèse peu sans rien perdre. À confirmer un par un avant d'agir —
la méthode fiable reste de lire le calcul dans la fonction appelée, comme pour
`0x0AC4`.

Aucun de ces quatre ne bloque la connexion ; ils dégraderaient des
fonctionnalités une fois en jeu.

Liste complète : [login_regressions.json](login_regressions.json) et
[game_regressions.json](game_regressions.json).
