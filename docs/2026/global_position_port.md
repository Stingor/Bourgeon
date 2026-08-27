# Porter les GLOBALES par leur position dans les fonctions appariées

Relevé du **2026-08-27**. C'est le vecteur qui atteint enfin les **zones de
données** — ni les switches, ni le RTTI, ni les chaînes n'y touchent, et elles
représentaient la moitié de ce qui restait à porter.

## Le principe

Deux fonctions appariées référencent en principe **les mêmes globales, dans le
même ordre**. Si les deux séquences ont la même longueur, la i-ème globale de
l'une correspond à la i-ème de l'autre.

Ce vecteur n'était pas exploitable au départ : il faut d'abord **beaucoup** de
fonctions appariées. Avec les passes précédentes on en a **6 297** (portage
initial + tables + vtables + accesseurs, 6 conflits seulement).

| | |
|---|---|
| fonctions avec séquence de globales | 3 202 (2025) / 3 929 (2026) |
| paires **exploitées** (même longueur) | **1 975** |
| paires ignorées (longueurs différentes) | 1 099 |
| références de globales collectées | 12 512 / 18 798 |

**Résultat : 909 globales appariées.**

## Les gardes

- **Vote** : une globale n'est retenue que si ≥ 80 % de ses témoins s'accordent.
  11 écartées pour vote trop partagé.
- **Collision** : une globale 2026 ne peut être réclamée que par une seule
  globale 2025. 3 cibles en collision, 7 sources écartées.
- **Longueurs différentes ignorées** : une seule référence ajoutée ou retirée
  décale tout le reste de la séquence. 1 099 paires sacrifiées pour ça.

## 🔴🔴 La validation, enfin substantielle

Les vecteurs précédents ne se recoupaient presque pas (4 adresses sur 623, puis
4 sur 4 327) — impossible d'en tirer une validation. Ici le recouvrement est
**réel** :

| croisement | recouvrement | concordent | divergent |
|---|---|---|---|
| vs tables / identifiants | **231** | **230** | **1** |
| vs portage initial | 5 | 5 | 0 |

**99,6 % d'accord sur 231 cas**, contre des vecteurs entièrement indépendants.

La seule divergence est instructive : `0x0131f50e` → `0x011f1904` (position)
contre `0x011f1906` (tables). **Deux octets d'écart** — un accès à un champ 16
bits dans la même structure, pas une erreur d'appariement.

Monotonie : **5,8 %** d'inversions contre **49,8 %** pour le témoin aléatoire.

## Ce que ça débloque

**+45 adresses du manifeste → 312/784 (39,8 %)**, contre 34,1 % avant.

Et ce sont les fichiers qui résistaient :

| fichier | sites débloqués |
|---|---|
| `features/overlays/basic_info.cc` | 17 |
| `features/windows/char_diagnostics.cc` | 10 |
| `features/windows/character_sheet.cc` | 7 |
| `ragnarok/game_settings.cc` | 5 |
| `features/windows/palette_editor.cc` | 4 |
| `features/windows/cart_viewer.cc` | 4 |

Les mieux étayées :

| 2025 | 2026 | symbole | témoins |
|---|---|---|---|
| `0x012515f8` | `0x01123c60` | `kContextPtr` | **110/110** |
| `0x0121333c` | `0x010db79c` | `kActiveModePtr`, `kCurrentModePtr` | 82/83 |
| `0x015beecc` | `0x01490524` | `kReplayActive` | 22/22 |
| `0x015fb9a4` | `0x014b895c` | `kAccountAid`, `kOwnAccountAid` | 17/17 |
| `0x015fb9d0` | `0x014b8988` | `kBaseExpLo` | 8/8 |
| `0x01602568` | `0x014bebd8` | `kOwnCharName` | 5/5 |
| `0x015fb278` / `0x015fb290` | `0x014b823c` / `0x014b8250` | `kHair`, `kHairCol` | 2/2 |

## Bilan consolidé de tous les vecteurs

| vecteur | paires |
|---|---|
| portage initial (RTTI, chaînes, propagation) | 157 |
| identifiants stables (225 tables) | 1 827 |
| RTTI (classe, slot) | 4 327 |
| **globales par position** | **909** |
| accesseurs triviaux | 41 |
| **UNION** | **6 971** |
| **conflits** | **7** |

➡ **312 / 784 adresses du manifeste (39,8 %)**, 474 sites dans 76 fichiers.
Les 6 premiers conflits sont les entrées déjà signalées comme suspectes dans
`port_suspects.json` ; le septième est l'écart de 2 octets ci-dessus.
