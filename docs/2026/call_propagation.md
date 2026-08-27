# Propagation par les APPELS : le vecteur qui s'auto-alimente

Relevé du **2026-08-27**. Dernier vecteur de la série, et le seul **itératif** :
chaque paire de fonctions établie en engendre de nouvelles.

## Le principe, et pourquoi il vient en dernier

Deux fonctions appariées appellent en principe **les mêmes fonctions, dans le
même ordre**. La i-ème cible d'appel de l'une correspond à la i-ème de l'autre.

C'est le même mécanisme que pour les globales par position — mais appliqué aux
appels, il **se nourrit de lui-même** : les fonctions découvertes deviennent des
témoins pour le tour suivant.

🔴 Il ne peut pas venir en premier : sans une base large de fonctions déjà
appariées, il n'a aucun témoin. Il a fallu les 6 971 paires des vecteurs
précédents pour l'amorcer.

## Deux itérations, et la convergence

| itération | témoins exploités | candidats | **nouvelles paires** |
|---|---|---|---|
| 1 | 2 305 | 1 143 | **1 100** |
| 2 | 2 964 | 517 | **477** |

Le rendement chute de moitié à chaque tour : la propagation converge. Une
troisième itération ne rapporterait plus grand-chose.

En réinjectant ces fonctions dans le vecteur des globales : **909 → 1 032**
globales appariées.

## Les gardes, et le contrôle qu'elles fournissent

Mêmes gardes que pour les globales — longueurs de séquences égales exigées, vote
à 80 %, une cible pour une seule source — plus une **vérification de
cohérence** propre à ce vecteur : une paire proposée ne doit pas contredire une
paire déjà établie.

| | itération 1 | itération 2 |
|---|---|---|
| **contredisent une paire connue** | **7** | **10** |
| vote trop partagé (écarté) | 57 | 61 |
| collision entre candidats | 34 | 32 |
| cible déjà prise | 9 | 8 |

**7 puis 10 contradictions sur des milliers de votes** — c'est ce taux qui dit
que la propagation ne dérive pas.

## Ce que ça débloque

**+39 adresses du manifeste** à la première itération. Les mieux étayées :

| 2025 | 2026 | symbole | témoins |
|---|---|---|---|
| `0x00a94930` | `0x00a91a50` | `kStdStringFromFmt` | **139/141** |
| `0x004e5330` | `0x004bb580` | `kStdStringCtorCStr` | 59/59 |
| `0x00600770` | `0x006ae6c0` | `kSoundPlay3D` | 53/62 |
| `0x00a39340` | `0x00a07bc0` | `kMakeWindowAddr` | 20/21 |
| `0x00c14d60` | `0x00be1f70` | `kConnGetInstanceAddr` | 16/16 |

✅ `kMakeWindowAddr` retrouve **exactement** l'appariement fait à la main pour la
fabrique de fenêtres — par un chemin entièrement différent.

Fichiers débloqués : `ragnarok/lua.h` (7 sites), `held_sprites.cc` (6),
`navigation_window.cc`, `game_settings.cc`, `skill_bar.cc`,
`inventory_viewer.cc` (3 chacun).

## 🔴 Bilan de TOUS les vecteurs

| vecteur | paires | apport manifeste |
|---|---|---|
| portage initial (RTTI, chaînes, propagation) | 157 | — |
| identifiants stables (225 tables) | 1 827 | +114 |
| RTTI (classe, slot) | 4 327 | **+1** |
| globales par position | 1 032 | +45 |
| **propagation par appels** (2 itérations) | **1 577** | **+39** |
| accesseurs triviaux | 41 | 0 |
| **UNION** | **8 645** | |
| **conflits** | **7** | |

➡ **157 → 360 / 784 adresses (20 % → 45,9 %)**, 530 sites dans 83 fichiers.

Contrôles sur le jeu final : monotonie **5,5 %** d'inversions contre **50,1 %**
au témoin aléatoire ; 128 paires (1,5 %) sortent de l'enveloppe locale, dont 9
utilisées par Bourgeon.

⚠ Parmi ces 9 figure `0x005c5950 → 0x00cca703` (`SkillMgr_SetOption`) — une
entrée du **portage initial** déjà signalée par l'audit des tailles. Deux tests
indépendants condamnent la même entrée : elle est très probablement fausse.
