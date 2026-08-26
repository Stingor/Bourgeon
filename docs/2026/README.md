# Portage vers le client kRO 2026-07-07

Tout ce dossier concerne **un chantier exploratoire** : faire tourner Moonlight
sur le client `2026-07-07` (BuildDate `20260707`). Rien ici ne décrit le client
en production — celui-là reste le `2025-07-16`, documenté à la racine de `docs/`
(dont `warp_patches.md`, qui n'a pas bougé).

Relevés du **2026-08-26**.

## Où en est le chantier

| volet | état |
|---|---|
| **protocole** | ✅ **aucun obstacle** — 0 opcode utilisé par Moonlight ne change de longueur |
| **patchs WARP** | 🟠 **65/116 passent tels quels** (56 %), 43 à réparer |
| **layout `CSession`** | 🔴 offsets déplacés, à rescanner **en jeu** |
| **adresses Bourgeon** | 🔴 407 à porter, 9/18 ancres résolues |
| **EOS AntiCheat** | ❔ **inconnue bloquante, non levée** |

➡ **Prochaine action : générer l'exe avec les 65 patchs qui passent et le
lancer.** C'est le test qui dit si EOS AntiCheat autorise le démarrage hors kRO —
tant qu'on l'ignore, réparer des patchs est un pari.

## Les fichiers

| fichier | contenu |
|---|---|
| `warp_run_2026.md` | **le résultat du test WARP** : les 116 patchs, leur statut, leur message d'erreur |
| `warp_patches_port.md` | comment porter : classement par risque, sens du transfert entre catalogues, la cause n°1 des échecs |
| `warp_active.txt` | la liste brute des 116 patchs activés dans `save_exe.yml` |
| `client_2026_address_map.md` | correspondances d'adresses 2025 ↔ 2026, et ce qui marche pour les trouver |
| `client_2026_opcodes.md` | les 1577 opcodes du client, nommés et croisés avec rAthena |
| `packet_len_diff.md` | méthode d'extraction des longueurs de paquets + le diff entre les deux clients |
| `packet_len_client_2025.json` | table extraite du client 2025-07-16 (1522 entrées) |
| `packet_len_client_2026.json` | table extraite du client 2026-07-07 (1577 entrées) |
| `packet_len_ra_packets.json` | ce que Moonlight déclare (`clif_packetdb.hpp`) |

## Les deux choses à retenir

🔴 **Une signature d'octets ne porte pas d'un build à l'autre** (4 ancres sur 18).
Ce qui porte : les **chaînes référencées**. Un déplacement de saut codé en dur
(`74 xx`) est la cause n°1 des échecs — il devient `0F 84 xx xx xx xx` dès que le
bloc dépasse 127 octets.

🔴 **Le gain de ce portage n'a pas encore été chiffré.** Bourgeon remplace déjà la
plupart des fenêtres natives par de l'ImGui : un an de nouveautés UI côté Gravity
ne sert quasiment pas. Le bénéfice acquis, lui, est réel et déjà encaissé —
l'IDB 2026 est propre (jamais packé, 1384 classes RTTI, 2527 méthodes virtuelles
nommées) et sert de **référence de lecture** là où Lotus a mutilé le code dans
l'IDB 2025.
