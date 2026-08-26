# Longueurs de paquets : client 2025-07-16 vs client 2026-07-07

Relevé du 2026-08-26. **Extraction statique**, reproductible.

## D'où viennent ces chiffres

La table de longueurs du client est une `std::map<int,{int,int}>` en zone non
initialisée de `.data` : **construite au runtime**, donc introuvable comme
tableau de données dans le fichier. Elle est remplie par un constructeur
statique, et c'est **là** que les longueurs sont en clair, en immédiats :

```asm
push 0        ; flag (0 ou 1)
push 37h      ; longueur (bis)
push 37h      ; longueur   <- 55
push 64h      ; opcode     <- 0x0064
mov  ecx, esi
call <Insert>
```

| | build 2025-07-16 | build 2026-07-07 |
|---|---|---|
| `PacketLenTable_Lookup` | `0x00AA7B00` | `0x00AA4290` |
| la table (objet global) | `0x0159D68C` | `0x0146EDFC` |
| ctor statique | — | `sub_498FC0` |
| **remplisseur** | `0x00AA0090` | `sub_A9C4D0` `0x00A9C4D0` |
| `Insert(opcode,len,len,flag)` | `0x00A9FF30` | `sub_A9C370` `0x00A9C370` |
| entrées extraites | **1522** | **1577** |

🔴 La signature d'octets du lookup retrouve la fonction **à l'octet près dans
les deux builds** — un an d'écart, zéro dérive. C'est la démonstration que la
résolution par signature marche pour Bourgeon.

## Le verdict

Moonlight déclare **729 opcodes** exploitables (`src/map/clif_packetdb.hpp`) :
480 à longueur fixe, 249 variables.

| | |
|---|---|
| opcodes communs aux deux clients | 1517 |
| longueur changée entre 2025 et 2026 | 26 |
| **… dont utilisés par Moonlight** | **0** |
| disparus en 2026 | 5 |
| … dont utilisés par Moonlight | 4 |
| nouveaux en 2026 | 60 |

**Aucun opcode utilisé par Moonlight ne change de longueur.**

### Les 26 opcodes dont la longueur change (aucun n'est utilisé)

| opcode | 2025 | 2026 |
|---|---|---|
| `0x0072` | 22 | 19 |
| `0x007E` | 46 | 6 |
| `0x0085` | 10 | 5 |
| `0x0089` | 11 | 7 |
| `0x008C` | 14 | -1 |
| `0x0094` | 19 | 6 |
| `0x009B` | 34 | 5 |
| `0x009F` | 20 | 6 |
| `0x00A2` | 14 | 6 |
| `0x00A7` | 9 | 8 |
| `0x00F3` | -1 | 8 |
| `0x00F5` | 11 | 8 |
| `0x00F7` | 17 | 2 |
| `0x0113` | 25 | 10 |
| `0x0116` | 17 | 10 |
| `0x0190` | 23 | 90 |
| `0x0191` | 27 | 86 |
| `0x0193` | 2 | 6 |
| `0x0206` | 35 | 11 |
| `0x0288` | -1 | 12 |
| `0x02E2` | 20 | 8 |
| `0x02E3` | 22 | 10 |
| `0x02E4` | 11 | 6 |
| `0x02E5` | 9 | 5 |
| `0x0C12` | 7 | 16 |
| `0x0C26` | 94 | 67 |

### Les 4 disparus que Moonlight déclare

| opcode | longueur en 2025 | déclaré par rAthena | réellement émis ? |
|---|---|---|---|
| `0x0258` | 2 | 2 | non |
| `0x0259` | 3 | 3 | non |
| `0x027E` | -1 | -1 | non |
| `0x02F7` | 47 | -1 | **OUI** — `ZC_UPDATE_GDID`, `clif.cpp:13177` |

🔴 **`0x02F7` est le seul point d'attention du chantier.** Absent de la table
2026, il tomberait dans le cas VARIABLE : le client lirait les deux octets
suivant l'opcode comme une longueur. Si le serveur l'émet en trame fixe, ces
octets sont des DONNÉES et le flux se désynchronise. À vérifier avant bascule.

### Divergences client/serveur préexistantes

Sur les 480 longueurs fixes de Moonlight : **28 divergent du client — les
mêmes 28, à l'identique, en 2025 ET en 2026** (`0x008B`, `0x009E`, `0x01A3`,
`0x01D7`-`0x01DA`, `0x022A`/`0x022B`, `0x022F`, `0x024D`, `0x02EC`…).
Ce n'est donc pas une régression du nouveau build : c'est l'écart structurel
entre `PACKETVER 20211103` et le vrai client, et il vit très bien.

## Conséquence

Le protocole **n'est pas** l'obstacle au changement de client. Pas de fork de
Moonlight à prévoir pour cette raison ; le chantier est côté Bourgeon (480
adresses, dont 67 résolubles par RTTI et 368 par signature).
