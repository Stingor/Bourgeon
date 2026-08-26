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

## 🔴🔴 Deux obstacles levés, et un piège de fond

**`steam_api.dll`** — les clients 2026 la lient **statiquement** (7 imports). Le
loader Windows refuse donc de démarrer le processus sans elle, avant la moindre
instruction. Réglé par le patch WARP **`NoSteamAPI`** (retire le descripteur
d'import + court-circuite le bloc Steam de `WinMain`, gardé par
`strstr(cmdline, "Steam")`). Vérifié en jeu : `steam_api.dll` n'apparaît plus
dans les modules chargés.

**ASLR + Control Flow Guard** — le piège de fond, et il concerne Bourgeon
directement :

| | client 2025 | client 2026 |
|---|---|---|
| `DllCharacteristics` | `0x8100` | `0xC040` |
| ASLR (`DYNAMIC_BASE`) | **non** | **oui** |
| CFG (`GUARD_CF`) | non | **oui** |

Le 2025 se charge toujours à `0x400000` — d'où les commentaires
« no-ASLR : addr Ghidra == live » dans le code. Le 2026 bouge à chaque
lancement (`0xB90000` sur un essai), donc **les 407 adresses en dur de Bourgeon
sont décalées d'un delta imprévisible**, et CFG rejette les entrées de vtable
détournées.

⚠ `Exe.SetHex` de WARP **ne peut pas écrire dans l'en-tête PE** (l'outil ne
patche que les sections). D'où **`fix_aslr.py`**, à lancer APRÈS la génération :

```
python docs/2026/fix_aslr.py "E:/Nouveau dossier/Moonlight-Destiny/2026-07-07_Ragexe_patched.exe"
```

Il efface les deux bits (`0xC040` → `0x8000`), sauvegarde l'original en
`.aslr.bak`, et relit pour vérifier. `--check` n'affiche que l'état.
⚠ Fermer x32dbg et le client avant : un fichier verrouillé fait échouer
l'écriture.

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
