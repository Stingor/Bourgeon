# Organisation des sources

> Posé le 2026-07-28, en remplacement du dossier plat `src/plugins/` (43 modules,
> ~52 000 lignes) dont le nom ne décrivait plus rien : la moitié de ce qu'il
> contenait n'était plus des « tweaks » mais des pans entiers de jeu réécrits.

## Les couches

```
src/
  bourgeon.{h,cc}  enregistre les modules, distribue les événements, tient les gates
  main.cc          point d'entrée de la DLL
  pch.h            en-tête précompilé — TIERS STABLE UNIQUEMENT

  features/        tout ce qui ajoute ou change quelque chose pour le joueur
  ui/              toolkit ImGui « façon RO » (skin, widgets, textures, z-order)
  ragnarok/        liaison avec le client natif (session, modes, paquets, layouts)
  utils/           outillage sans rapport avec le jeu (hooks, logs, chemins, GIF)

  ddraw/ d3d9/ imgui/   les trois backends de rendu de l'overlay
  thirdparty/           dépendances vendues (imgui, spdlog, yaml-cpp, doomgeneric…)
```

Une seule règle de dépendance : **`ui/` ne connaît pas `features/`**. Le toolkit
est générique par construction ; s'il a besoin de savoir ce que fait un module,
c'est que la pièce est au mauvais endroit. (Ce cycle a déjà existé — `ro_imgui.cc`
incluait `moonlight_ui.h` pour récupérer trois widgets.)

## `src/features/` — rangé par NATURE

La question à se poser n'est pas « de quoi ça parle ? » mais **« qu'est-ce que ça
fait au client natif ? »**.

| Dossier | Le module… | Exemples |
|---|---|---|
| `windows/` | **remplace** une fenêtre native : la native est cachée (`wnd+0x28 = 0`, hors rendu ET hors hit-test) mais reste vivante côté session, et l'ImGui prend sa place | `storage_window`, `npc_shop_window`, `character_sheet` |
| `patches/` | **retouche le natif en place** : hook, swap d'entrée de vtable, patch d'octets. La fenêtre d'origine reste à l'écran | `equip_tweaks`, `status_tweaks`, `window_pos_tweaks` |
| `overlays/` | **dessine par-dessus** le jeu, sans remplacer de fenêtre : barres, plaques de nom, HUD | `basic_info`, `entity_names`, `skill_bar` |
| `gameplay/` | change **les entrées ou la caméra** | `keyboard_move`, `player_jump`, `fps_view` |
| `fx/` | touche au **rendu des sprites, des effets ou de l'image finale** | `weapon_layer`, `hat_effect_depth`, `screen_fx` |
| `systems/` | **service transverse**, sans UI propre ou presque : réseau, sécurité, session, méta | `integrity_check`, `discord_relay`, `auto_login` |
| `minigames/` | un **jeu embarqué** | `doom`, `roggle`, `rojeweled` |

À la racine de `features/`, le socle partagé : `plugin.h` (la classe de base),
`net_inbox.h` (le passage du fil réseau au fil principal — **obligatoire** dès
qu'un module décode un paquet), `staff_gate.h` (le gate niveau de groupe ≥ 80) et
`hotkey_util.{h,cc}` (capture de combo + contrôle de conflit). Et `features/moonlight_ui/`, qui est à part :
c'est le sommaire des réglages **et** le propriétaire du fichier yaml — d'où le
« persisté par MoonlightUi » qu'on lit dans les en-têtes des autres modules.

### Cas limites, tranchés une fois pour toutes

- **`windows/` ou `patches/` ?** Si la fenêtre native est encore visible à
  l'écran, c'est `patches/`. `storage_window` est dans `windows/` bien qu'il
  COEXISTE avec la native : il dessine une fenêtre complète, pas une retouche.
- **`overlays/` ou `windows/` ?** Un overlay ne remplace rien. `skill_bar` cache
  pourtant `UIShortCutWnd`… mais il ne la *remplace* pas fenêtre pour fenêtre :
  c'est une barre d'action multi-rangées qui n'a pas d'équivalent natif.
- **`fx/` ou `patches/` ?** `weapon_layer` et `hat_effect_depth` patchent bien le
  natif, mais ce qu'ils patchent est le **pipeline de rendu** — les chercher
  dans `fx/` est le réflexe juste.

## Nommage

Le suffixe **`_tweaks` est réservé aux vraies retouches**, et il n'en reste que
six, toutes dans `patches/` : `ChatTweaks`, `EquipTweaks`, `InventoryTweaks`,
`SkillTreeTweaks`, `StatusTweaks`, `WindowPosTweaks`.

- un remplacement complet de fenêtre → `*_window` / `*Window` ;
- tout le reste → le nom de la chose, sans suffixe (`doom`, `skill_bar`,
  `screen_fx`, `quest_tracker`) ;
- **fichier, classe, accesseur `Bourgeon::` et membre portent le même nom.**
  `bank_window.cc` → `class BankWindow` → `Bourgeon::bank_window()` → `bank_window_`.

`shop_tweaks` est devenu `npc_shop_window` et non `shop_window` : « shop » tout
court ne disait pas s'il s'agissait de la boutique NPC ou de l'échoppe joueur,
qui est `vending_window`.

Le reste des conventions (membres, constantes, adresses RE) est dans
`docs/naming_charter.md`, règles R1 à R16.

## Ajouter un module

1. Choisir le dossier avec le tableau ci-dessus.
2. Écrire `features/<dossier>/<nom>.{h,cc}` : sous-classe de `Plugin`, en
   commençant par un commentaire d'en-tête qui dit **ce que ça fait au natif** et
   **où sont les adresses RE**.
3. Ajouter les deux fichiers à `src/CMakeLists.txt`, dans le groupe du dossier
   (CMake reconfigure tout seul au build suivant).
4. Dans `src/bourgeon.cc` : `#include "features/<dossier>/<nom>.h"` puis
   `plugins_.emplace_back(std::make_unique<Nom>());` dans `LoadPlugins()`.
   Si un autre module doit l'atteindre, ajouter l'accesseur dans `bourgeon.h`
   (déclaration anticipée + pointeur non-possédant).
5. Réglages persistés : les descripteurs vivent dans
   `features/moonlight_ui/settings_table.cc`, le panneau dans un
   `bool DrawSettings()` du module lui-même — jamais dans moonlight_ui.

⚠️ Ne JAMAIS rendre un membre public pour permettre à un panneau d'être une
fonction libre : un panneau qui touche l'état privé est une méthode membre.
Convention écrite dans `features/moonlight_ui/internal.h`.

## Renommer en masse, sans build

Les deux pièges déjà payés sur ce dépôt :

- **Les fins de ligne.** Le dépôt mélange LF et CRLF et n'a pas de
  `.gitattributes`. Tout script qui réécrit une source doit lire et écrire en
  **binaire**, puis se vérifier au `git diff --numstat` : un fichier entier
  réécrit = les CRLF ont sauté.
- **Les identificateurs courts.** Un remplacement mot-entier de `en`, `si` ou
  `st` a déjà réécrit des commentaires français (« il n'entity_names reste »).
  Pour des noms longs et distinctifs le risque disparaît ; pour des noms courts,
  ne toucher que la partie CODE d'une ligne (couper au `//` hors littéral).

Utiliser des lookarounds plutôt que `\b` : `(?<![A-Za-z0-9_])X(_?)(?![A-Za-z0-9_])`
évite que la règle `shop_tweaks` morde dans `cashshop_tweaks`, et le groupe `(_?)`
traite d'un coup l'accesseur, le membre `_` et le chemin d'include.

🔴 **Supprimer une LIGNE ≠ supprimer un NOM.** Une déclaration groupée
(`constexpr int kWinItemDesc = 0xc, kMsgSetItem = 0x18, kVfOnMsg = 0x94;`) fait
qu'un script qui constate la mort de `kVfOnMsg` et retire la ligne emporte deux
constantes bien vivantes avec elle. Ça s'est produit une fois, dans
`character_sheet.cc`, et aucune des vérifications ci-dessous ne l'a vu : elles
contrôlaient les noms VISÉS, jamais les autres noms présents sur la ligne
retirée. La règle qui manque : **relever tous les identificateurs de chaque
ligne supprimée, et vérifier pour chacun qu'il est soit encore déclaré, soit
plus utilisé nulle part.**

Les trois vérifications qui remplacent un compilateur :

```
# 1. plus aucune occurrence des anciens noms
# 2. tout #include du projet résout vers un fichier existant
# 3. l'ensemble listé dans src/CMakeLists.txt == l'ensemble des fichiers sur disque
```

Enfin : **aucune clé yaml ne porte un nom de module** (vérifié), et `Plugin::name()`
ne sert qu'aux logs et à la liste de la fenêtre Bourgeon. Renommer une classe ne
touche donc pas les configs des joueurs — mais c'est à revérifier avant chaque
campagne de renommage, pas à supposer.
