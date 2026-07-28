# Audit de src/plugins/moonlight_ui.cc

> ⚠️ **Document historique, conservé tel quel.** Les chemins et les noms de
> classes cités décrivent l'arborescence de juillet 2026, avant le rangement de
> `src/plugins/` en `src/features/<nature>/` (cf. `docs/source_layout.md`). Ne
> pas les « corriger » : les numéros de ligne, les noms et les chemins forment un
> tout daté qui n'a de sens qu'ensemble.

> 3542 lignes, 88 commits (fichier le plus modifie du repo). Audit mene par 6 analyses paralleles (structure, persistance, couplage, duplication, performance, risques), chacune passee a un verificateur adversarial, puis synthetisee.

## Verdict global

moonlight_ui.cc n'est pas un plugin : c'est devenu le tronc du projet, et il porte trois rôles qui n'ont rien à faire ensemble — le panneau de contrôle (OnRenderUI = 1702 lignes d'un seul tenant, de 1841 à 3542), le sérialiseur central de 19 plugins (991 lignes, 28 % du fichier, deux miroirs manuels de 191 clés) et le toolkit UI partagé du repo (18 fichiers incluent son header, dont src/ui/ro_imgui.cc, ce qui inverse les couches). Le nœud du problème — el nudo, comme on dit en espagnol — n'est pas la taille : c'est que la propriété est à l'envers. Chaque plugin a rendu ses membres publics pour que ce fichier les sérialise et dessine leurs réglages, si bien qu'une modification dans un plugin casse ici, à distance, sans que le compilateur explique pourquoi. Cette inversion produit des bugs d'un genre précis, silencieux à la compilation et visibles seulement en jeu : un réglage lu mais jamais écrit (skillbar_locked), un défaut divergent entre trois fichiers (itemdesc_anchor 3 vs 0), et surtout une sauvegarde qui réécrit le yaml entier et détruit au passage les sections de trois autres composants (auto_login, char_select, moonlight_auth) — vérifié, moonlight_ui.cc en est le seul écrivain. La bonne nouvelle, c'est que la sortie est déjà tracée dans le code : cinq plugins délèguent proprement via DrawSettings(), deux endroits appliquent déjà un anti-rebond avant de persister, et window_pos_tweaks expose déjà une API d'énumération — il ne s'agit donc pas d'inventer une architecture, mais de généraliser celle qui a déjà été essayée et qui marche.

## Chiffres cles

- moonlight_ui.cc = 3542 lignes (vérifié wc -l). OnRenderUI() = lignes 1841 à 3542, soit 1702 lignes dans UNE fonction, avec 8 en-têtes de premier niveau et 11 sous-sections.
- LoadSettings 452-968 (517 l.) + SaveSettings 970-1443 (474 l.) = 991 lignes, soit 28 % du fichier, pour 191 clés yaml épelées deux fois à la main.
- moonlight_ui.cc est le SEUL écrivain de bourgeon_settings.yaml, alors que trois autres composants en lisent une section racine : auto_login.cc:144, char_select.cc:721, moonlight_auth.cc:394 (vérifié au grep sur tout src/).
- 45 appels à SaveSettings() dans src/, dont 20 dans moonlight_ui.cc — aucun anti-rebond centralisé.
- 31 `#include "plugins/..."` (lignes 15-45) ; 18 autres fichiers .cc incluent plugins/moonlight_ui.h, dont src/ui/ro_imgui.cc — le toolkit dépend du plugin, couches inversées.
- 8 `std::stoul` dans le fichier, dont 7 dans LoadSettings, tous sous UN seul try/catch allant de la ligne 462 à la ligne 967.
- 0 occurrence de IsMapLoading() ou IsGameActive() dans moonlight_ui.cc, alors que keyboard_move.cc:263, player_jump.cc:179 et spr_effect_lab.cc:190 appliquent la garde.
- src/CMakeLists.txt:175 contient l'unique `target_compile_options(bourgeon PRIVATE "/MT")` : ni /MP, ni target_precompile_headers, pour 66 unités de compilation sur une machine à 20 cœurs.
- Un seul BeginTabBar (2978) et un seul EndTabBar (3403) dans le fichier, séparés par 425 lignes — et le End est en dehors du if.
- Profondeur maximale de 11 niveaux d'accolades (ligne 2563) et 42 colonnes d'indentation (ligne 2561) dans le panneau Chat ; 9 variables `bool changed` homonymes dans la même fonction (2043, 2229, 2341, 2497, 2622, 2677, 2713, 2788, 2861).

## Bugs reels (a corriger independamment du refactor)

### B1. SaveSettings écrase bourgeon_settings.yaml en entier et DÉTRUIT les sections auto_login, char_select et moonlight_auth

- **Gravite** : critique
- **Lignes** : moonlight_ui.cc:1007-1010 (BeginMap + seule clé racine « moonlight_ui ») et 1435-1441 (std::ofstream f(path) = troncature)

Vérifié : `grep -rn "bourgeon_settings" src/` montre que moonlight_ui.cc est le SEUL écrivain du fichier, mais trois autres composants en lisent une section racine — auto_login.cc:144 `root["auto_login"]`, char_select.cc:721 `root["char_select"]`, moonlight_auth.cc:394 `root["moonlight_auth"]` — et aucun ne le réécrit (moonlight_auth.cc:65 et :436 écrivent un fichier DÉDIÉ, avec un commentaire qui dit explicitement vouloir ne pas abîmer le yaml partagé). Conséquence : la première case cochée en jeu efface le login/mot de passe/serveur d'auto-login, l'opt-out char_select.imgui et la config moonlight_auth (base_url dev, verify_tls). Correctif : relire le fichier (`YAML::Node root = YAML::LoadFile(path)` dans un try), remplacer uniquement `root["moonlight_ui"]`, réémettre `root`. ~15 lignes, aucune ligne de sérialisation à toucher.

### B2. ro::RoCombo renvoie true tant que le popup est OUVERT, pas quand la sélection change → réécriture complète du yaml à chaque frame

- **Gravite** : critique
- **Lignes** : src/ui/ro_imgui.cc:1626-1641 (`changed = true;` posé inconditionnellement dans le corps du `if (RoBeginCombo…)`)

Lu et confirmé : `bool RoCombo(...) { bool changed = false; const char* modes[] = {...}; /* tableau MORT */ if (ro::RoBeginCombo(label, items[*current_item])) { for (...) { if (ImGui::Selectable(...)) *current_item = i; ... } changed = true; ro::RoEndCombo(); } return changed; }`. 11 sites d'appel font `changed |= ro::RoCombo(...)` suivis d'un `if (changed) SaveSettings()` ou d'un `dirty_` : moonlight_ui.cc:2382, 2452, 2458, 2699 et status_icon_tweaks.cc:787, 789, 791, 800, 808, 812. Ouvrir un menu déroulant et le laisser ouvert = une sérialisation YAML complète + une écriture disque par frame, sur le thread de rendu. Correctif : poser `changed = true` UNIQUEMENT dans le `if (ImGui::Selectable(...))` quand `i != *current_item`, et supprimer `modes[]`. 3 lignes, corrige 11 sites d'un coup.

### B3. Sliders et color pickers persistent à chaque frame de glissement (aucun anti-rebond) — dont un qui relance le re-wrap complet du chat

- **Gravite** : critique
- **Lignes** : 2047-2051 → 2086 (DPS) ; 2235-2240 → 2280 (grille) ; 2386, 2438-2446, 2479-2484 → 2492 (portrait/barres) ; 2700-2701 → 2708 ; 2724 → 2763 (skin, 2 sliders + 14 pickers) ; 2512-2515 → 2617 (largeur chat)

ro_imgui.cc:1459-1462 : `if (ImGui::SliderBehavior(track, id, dt, p_data, ...)) value_changed = true;` — RoSliderFloat/Int (donc WheelSliderFloat/Int) renvoient true à CHAQUE frame où la valeur bouge ; idem ColorPicker = ImGui::ColorEdit4 (moonlight_ui.h:135-141). Chaque frame déclenche donc SaveSettings() : ~330 clés émises, ~140 std::string temporaires, 2 tris de conteneurs, 36 lectures mémoire client sous SEH (SnapshotItemSlots ligne 1370), 1 GetModuleFileNameA et 1 cycle ofstream create/truncate/write/close. Un drag de 2 s à 60 fps = 120 écritures complètes. Cas le plus grave, ligne 2512-2515 : `if (chat_width_enabled_ && WheelSliderInt("Largeur (px)", &chat_width_px_, 320, 1200)) { chat::SetCustomWidth(true, chat_width_px_); ... }` — SetCustomWidth relance le relayout natif + RebuildFromHistory sur TOUS les onglets, exactement la fonction du freeze word-wrap déjà corrigé. Le fichier connaît pourtant le remède : chat_bg l'applique à la main en 2591-2599, et spr_effect_lab.cc:809-811 le commente mot pour mot. Correctif : `settings_dirty_` + flush unique par frame (chantier 2), et n'appeler chat::SetCustomWidth que sur `ImGui::IsItemDeactivatedAfterEdit()`.

### B4. SkillBarTweaks::locked_ n'est persisté dans AUCUNE clé yaml : les barres se reverrouillent à chaque login

- **Gravite** : majeur
- **Lignes** : Load 785-822, Save 1348-1392 ; case à cocher skill_bar_tweaks.cc:1081 ; membre skill_bar_tweaks.h:44

Diff exhaustif vérifié : Load lit skillbar_enabled/bilinear/clickthrough/show_keys/bold_text/key_scale/count_scale (786-793), les 3 barres (797-806), les 36 slots (808) et les 9 couleurs (814-822) ; Save émet exactement les mêmes (1349-1355, 1360-1372, 1383-1391). `locked_` n'apparaît nulle part : `grep -rn skillbar_locked src/` = 0 résultat. Or skill_bar_tweaks.cc:1081 fait `changed |= ro::RoCheckbox("Verrouiller", &locked_);` dans le même bloc que 12 autres `changed |=` tous persistés, et le `dirty_` résultant est drainé par moonlight_ui.cc:1866-1869 qui appelle bien SaveSettings — mais sans la valeur. Correctif immédiat 2 lignes : `sb->locked_ = ui["skillbar_locked"].as<bool>(sb->locked_);` après 786, et `<< YAML::Key << "skillbar_locked" << YAML::Value << (sb ? sb->locked_ : true)` dans 1349-1355. À trancher au passage : InventoryViewer::cur_tab_ / sort_enabled_ non persistés alors que l'équivalent storage l'est.

### B5. ImGui::EndTabBar() appelé HORS du `if (ImGui::BeginTabBar(...))`

- **Gravite** : majeur
- **Lignes** : 2978 (BeginTabBar) → accolade fermante 3402 → 3403 `ImGui::EndTabBar();` inconditionnel

Numéros vérifiés au grep : un seul BeginTabBar (2978), un seul EndTabBar (3403), et l'accolade du if se ferme en 3402. Le contrat ImGui exige que EndTabBar ne soit appelé que si BeginTabBar a renvoyé true (dépilement de g.CurrentTabBarStack) : IM_ASSERT en debug, dépilement d'une pile vide en release. Ça ne pète pas aujourd'hui uniquement parce que le `CollapsingHeader("Commands Settings")` de 2974 renvoie déjà false dans le seul cas qui ferait échouer BeginTabBar (window->SkipItems). Toute garde supplémentaire ajoutée plus tard le réveille. Correctif : déplacer la ligne 3403 juste avant l'accolade 3402. Contraste avec la discipline correcte à 4 lignes de là : BeginTable 2982 / EndTable 3015.

### B6. Tout le panneau « Chat » est gaté sur un pointeur BasicInfoTweaks jamais utilisé

- **Gravite** : majeur
- **Lignes** : 2498 `if (auto* eb = Bourgeon::Instance().basic_info()) {` … fermeture 2616

Vérifié : sur la plage 2496-2618, `eb` n'apparaît qu'une seule fois — sa propre déclaration. Le corps n'utilise que discord_chat_, chat_width_*, chat_timestamps_, chat_item_icons_, chat::SetCustomWidth/SetTimestamps/SetItemIcons/ClearHistory, chat_bg_ et chat_bg_presets_. C'est un copier-coller de la ligne 2342 (panneau Basic Info, où `eb` sert ~40 fois). Si BasicInfoTweaks n'est pas enregistré, le joueur voit une page COMPLÈTEMENT VIDE — sans même le `ImGui::TextDisabled("(plugin indisponible)")` employé partout ailleurs (2335, 2664, 2672, 2706). Correctif : supprimer le if et son accolade (garder PushStyleCompact/PopStyleCompact), ou gater sur le vrai plugin consommé.

### B7. HeapWalk sous HeapLock, sans SEH ni RAII : une faute laisse le tas du process verrouillé à vie

- **Gravite** : majeur
- **Lignes** : 401-422 (PatchChatBgObjects), appelé via ApplyChatBg(…, true) 398 et depuis LoadSettings 472

Code lu : `HANDLE heap = GetProcessHeap(); if (!heap || !HeapLock(heap)) return;` puis `while (HeapWalk(heap, &entry)) { ... if (*vtable_ptr != t.vtable) continue; *reinterpret_cast<uint32_t*>((uint8_t*)entry.lpData + t.field_off) = argb; }` puis `HeapUnlock(heap);` en 420. Aucun __try/__except, aucun garde RAII : toute exception entre 403 et 420 laisse le tas par défaut verrouillé → gel total du client au premier malloc d'un autre thread. Le seul critère d'identification d'objet est « 1er dword == vtable » + `cbData >= field_off+4` : n'importe quel bloc dont les 4 premiers octets valent la vtable se fait écraser un dword à +0xD4/+0xD8. Et le walk est déclenché 3 fois par login depuis LoadSettings ligne 472, au moment où AUCUNE fenêtre de chat n'existe encore (donc pour rien), pendant le chargement de map. Correctif : garde RAII + __try/__except, et walk_heap=false depuis LoadSettings (le patch .text suffit au login).

### B8. VirtualProtect non vérifié sur les immédiats .text, puis écriture inconditionnelle à chaque changement de couleur

- **Gravite** : majeur
- **Lignes** : 367 `VirtualProtect(imm, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &old_protect);` (retour ignoré) ; 370 `g.instrs.push_back(imm);` ; 393-397 (ApplyChatBg écrit sans condition)

Le commentaire 366 assume l'absence de restauration (« one VirtualProtect, never restored »), mais la valeur de retour est ignorée et `imm` est poussé dans g.instrs quoi qu'il arrive. Si la page reste non-inscriptible (protecteur « Lotus », CFG/ACG), le premier changement de couleur fait une violation d'accès en écriture dans .text. Correctif : tester le retour et n'insérer `imm` que si la page est devenue inscriptible, sinon LogError + site ignoré — le reste du code gère déjà proprement l'absence de site (`g.instrs.empty()` 2538, `chat_bg_found_` 2606).

### B9. Sept std::stoul non gardés sous un try/catch unique de 500 lignes + écriture non atomique : un caractère invalide fait repartir TOUTE la config aux défauts, que la sauvegarde suivante grave

- **Gravite** : majeur
- **Lignes** : try 462 → catch 965-967 ; std::stoul en 470, 483, 523, 544, 601, 812, 929 ; écriture 1435-1441

Vérifié au grep : 8 `std::stoul` dans le fichier, dont 7 dans LoadSettings, tous gardés uniquement par la LONGUEUR (`hex.size() == 8`), jamais par la validité hexadécimale. Un `chat_bg: "ZZZZZZZZ"` passe le test, lève std::invalid_argument, et les ~500 lignes suivantes (DPS, grille, positions de fenêtres, skin, skillbar, storage, inventaire, presets d'équipement) restent aux défauts. SaveSettings réécrit ensuite le fichier complet à partir de cet état, sans jamais relire l'ancien : la perte est définitive. L'écriture n'est pas atomique (ofstream direct, pas de tmp+MoveFileEx), donc un ALT-F4 pendant l'écriture produit exactement le même scénario. Correctif : helper `bool ParseHex8(const std::string&, uint32_t*)` non-lançant, écriture tmp + MoveFileExA(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH), et flag `load_failed_` qui interdit d'écraser après un échec de parse.

### B10. Le défaut de ItemDescTweaks::desc_anchor_ est mort : 3 dans le header, 0 dans le chargement et dans le repli d'écriture

- **Gravite** : moyen
- **Lignes** : item_desc_tweaks.h:208 (`int desc_anchor_ = 3;`) vs moonlight_ui.cc:493 (`.as<int>(0)`) et 993 (`int itemdesc_anchor = 0;`)

Vérifié aux trois sites. Dès qu'un yaml existe sans la clé `itemdesc_anchor` (yaml antérieur à la fonctionnalité), l'ancrage de la description repasse silencieusement de 3 (bas-droite) à 0 (haut-gauche). Symptôme indiscernable d'un choix délibéré, compilateur muet. Correctif : trancher une valeur et la mettre aux TROIS endroits, ou faire dériver le défaut de lecture/écriture d'une source unique (chantier 8). Même famille : 66 fallbacks ternaires `(ptr ? ptr->champ : littéral)` dans SaveSettings, dont deux couleurs pré-encodées à la main (972 `"FFFFCC33"` = dps_meter.h:27, 981 `"B30D0D12"` = basic_info.h:140).

### B11. g_staff_level n'est jamais remis à zéro à la déconnexion

- **Gravite** : moyen
- **Lignes** : 115 (`static int g_staff_level = 0;`), 1654 (seule écriture), 1470-1471 (nettoyage de session voisin)

OnModeSwitch nettoie explicitement l'état de session voisin — `if (!in_game_ && was_in_game) aloot_ids_.clear();` (1470-1471) — mais pas g_staff_level. Le commentaire 110-114 affirme « Non persisté : autoritatif serveur, rafraîchi à chaque login », ce qui suppose que le serveur renvoie TOUJOURS le setting id 26, y compris avec la valeur 0. Tant que cette hypothèse n'est pas garantie côté serveur, changer de compte laisse les « Staff Tools » (2960) et les réglages fins de saut/marche exposés. Correctif : 1 ligne, `g_staff_level = 0;` à côté de aloot_ids_.clear().

### B12. Machine à états du picker de fond de chat : g.editing peut rester bloqué à true → couleur appliquée mais jamais persistée ni propagée aux fenêtres vivantes

- **Gravite** : moyen
- **Lignes** : 2591-2599 (à l'intérieur du `if (BeginPopup("picker"))` ouvert 2546, fermé 2601)

`if (ColorPicker("##pick", g.color)) { ApplyChatBg(g, ArgbFromPicker(g.color), false); g.editing = true; }` puis `if (g.editing && ImGui::IsMouseReleased(0)) { ApplyChatBg(g, …, true); changed = true; g.editing = false; }`. Le second bloc n'est atteint que si le popup est ENCORE ouvert. Échap, clic hors popup ou le bouton « Close » (2600) pendant un drag laissent editing à true : le commit (walk_heap) n'a jamais lieu et `changed` n'est jamais posé, donc pas de SaveSettings. La couleur est visible immédiatement (le .text est déjà patché) mais disparaît au prochain login, et les fenêtres déjà ouvertes gardent l'ancienne. Correctif : finaliser APRÈS EndPopup (2601) sur `g.editing && !ImGui::IsMouseDown(0)`.

### B13. Pointeur de fenêtre native conservé entre frames et déréférencé, avec un commentaire d'offsets faux

- **Gravite** : moyen
- **Lignes** : 132 (globale), 159 (capture de l'ecx), 3413-3417 (garde partielle), 3430 (commentaire), 3435-3438 (déréférencement)

NUANCÉ par rapport à l'analyse initiale : la garde 3413-3417 existe bel et bien et couvre le cas courant — `if (g_item_desc_visible && *reinterpret_cast<const uintptr_t*>(kItemDescWndGlobalPtr) == 0) { g_item_desc_visible = false; g_item_desc_wnd_ptr = nullptr; }`. Elle ne couvre PAS le cas où le jeu réalloue une autre fenêtre à la place (le global redevient non nul mais pointe ailleurs) : on lit alors +0x1C/+0x20 dans un objet étranger, le garde de plausibilité 3439 (`wx > 0 && wx < 4096`) validant les valeurs et non le pointeur. Correctif d'une ligne : comparer `g_item_desc_wnd_ptr == *reinterpret_cast<void**>(kItemDescWndGlobalPtr)` avant de déréférencer. Corriger aussi le commentaire 3430 qui dit « [ptr+0x18]=Y, [ptr+0x20]=X » alors que le code lit +0x1C pour X et +0x20 pour Y (les commentaires en fin de lignes 3437-3438 disent « confirmed », c'est le bloc du dessus qui est périmé).

### B14. Chaînes d'UI françaises sans accents et chaînes restées en anglais (contrainte projet explicite)

- **Gravite** : mineur
- **Lignes** : Accents : 2268, 2720, 2721, 2761, 2762 — Anglais : 321, 323, 325, 2549, 2582, 2585, 2600, 2610, 2613, 3511-3513 — Grammaire : 2430

Accents manquants : « clic droit = desequiper » (2268 → déséquiper), « (latin + coreen) » (2720 → coréen), « police integree d'ImGui » (2721 → intégrée), « les couleurs/luminosite/opacite actuelles » (2761), « plusieurs themes » (2762). Anglais dans un panneau francophone : « Main chat » / « Detached windows » / « Whisper (1:1) » (321/323/325, libellés définis au milieu de la fonction de scan .text), « Presets: » (2549), « Preset name » (2582), « Save preset » (2585), « Close » (2600), « Preset bar » (2610), « (chat background patch unavailable) » (2613), « No presets yet. / Add some in the / Main chat picker. » (3511-3513). Et une faute de frappe ligne 2430 : « Ne garde ne génère que la tête ».

### B15. Noms d'items affichés bruts, sans conversion CP949 → UTF-8

- **Gravite** : mineur
- **Lignes** : 250 (stockage brut depuis itemInfoMerged.lua), affichage 3236, 3249, 3456

`item_names_[current_id] = line.substr(q1 + 1, q2 - q1 - 1);` stocke les octets du fichier tels quels, rendus ensuite par `ImGui::Text("%s [%u]", it->second.c_str(), id)`. ImGui attend de l'UTF-8, le client parle CP949, et le toolkit fournit exactement l'outil (ui/ro_imgui.h:21-25 `const char* Cp949ToUtf8(const char*)`) — zéro appel dans tout src/plugins. Correctif : convertir UNE fois à l'insertion ligne 250, ce qui évite en prime le piège du buffer thread-local rotatif à 8 emplacements documenté dans ro_imgui.h:22-24 (ne jamais conserver le pointeur retourné).

### B16. Payload du paquet observé 0x0091 lu sans borne, et `len` ne prouve rien pour un opcode observé

- **Gravite** : mineur
- **Lignes** : 1479-1488 (aucun contrôle) vs 1493 (`if (len < 2) return;`) et 1532-1539 (validation commentée) ; enregistrement 260

`const char* map_name = reinterpret_cast<const char*>(data); in_gonryun_ = in_game_ && (strncmp(map_name, kDiscordMap, sizeof(kDiscordMap) - 1) == 0);` sans `if (len < 7) return;`, alors que les deux autres branches du même dispatch valident correctement. Aggravant : pour un opcode OBSERVÉ, rag_connection.cc:189 transmet la longueur ENREGISTRÉE (kMapNameLen, ligne 260), pas le nombre d'octets réellement reçus — donc même un test `len >= 7` serait sans valeur. Correctif : `strnlen(map_name, kMapNameLen)` avant comparaison, et un commentaire au-dessus du RegisterObserveOpcode 260 rappelant la sémantique piégeuse de `len`.

## Chantiers de refactor (ordonnes par ratio gain/risque)

### 1. Deux lignes de CMake : /MP puis PCH — payer d'abord la boucle de vérification

| | |
|---|---|
| Risque | faible |
| Effort | 30 minutes |
| Lignes touchees | src/CMakeLists.txt:175 (+1 bloc de ~5 lignes) |
| Gain | Rebuild complet divisé par ~8-12 (66 TU indépendants) ; front-end de moonlight_ui.cc mesuré à 1,368 s → 0,664 s avec PCH. Aucune ligne de C++ modifiée, donc zéro risque de régression fonctionnelle. |

**Pourquoi** : Il n'y a AUCUN test automatisé : chaque chantier suivant se vérifie par « je compile, j'injecte, je regarde ». Le coût de cette boucle conditionne tout le reste du plan. Vérifié : src/CMakeLists.txt:175 contient l'unique `target_compile_options(bourgeon PRIVATE "/MT")` — ni /MP ni target_precompile_headers, pour 66 TU sur une machine à 20 cœurs, chaque TU reparsant ~458 k lignes de Windows SDK + STL (94,6 % du préprocessé).

**Quoi faire** : 1) Ajouter `/MP` (ou `/MP16` pour laisser des cœurs au LLM local qui partage la machine) à la ligne 175 de src/CMakeLists.txt. Compiler, vérifier que le binaire fonctionne. 2) Dans un second commit, ajouter `target_precompile_headers(bourgeon PRIVATE <Windows.h> <algorithm> <array> <atomic> <cstdint> <cstdio> <cstring> <deque> <fstream> <map> <memory> <mutex> <string> <unordered_map> <unordered_set> <vector> "imgui.h" "spdlog/fmt/fmt.h" "yaml-cpp/yaml.h">`. AUCUN header du projet dedans.

**Verification** : `cmake --build "…/Bourgeon/build" --config Release --clean-first` doit réussir sans warning nouveau ; chronométrer avant/après. Puis injection + un tour de jeu normal (login, warp, ouvrir le panneau) : le binaire produit est fonctionnellement identique, seule la vitesse de compilation change. Si le PCH provoque des erreurs de compilation (ordre d'inclusion), retirer le header fautif de la liste — il n'y a aucun risque runtime.

### 2. Persistance : fusionner le root, écrire atomiquement, ne flusher qu'une fois par frame

| | |
|---|---|
| Risque | faible |
| Effort | une demi-journée |
| Lignes touchees | moonlight_ui.cc 970 (signature), 1370, 1435-1441, 1854-1869, fin d'OnRenderUI ~3540, 2512-2515 ; moonlight_ui.h ~154-156 + 2 membres ; src/ui/ro_imgui.cc:1626-1641 |
| Gain | Plus aucune destruction des sections auto_login/char_select/moonlight_auth ; un drag de 2 s passe de ~120 écritures de ~10 Ko à 1 ; plus de yaml tronqué au crash ; les 3 idiomes d'anti-rebond ad hoc du repo (chat_bg 2591-2599, spr_effect_lab.cc:811, throttles settings_tweaks.cc:290/318) deviennent supprimables. ~50 lignes ajoutées, ~15 supprimées. |

**Pourquoi** : Ce chantier corrige d'un coup les trois bugs les plus coûteux (destruction des sections auto_login/char_select/moonlight_auth, écriture par frame pendant les drags, yaml tronqué au crash) et il est presque entièrement local à deux fonctions. C'est le meilleur ratio soulagement/danger du dossier : le joueur cesse de perdre sa config et les hoquets de frame pendant l'édition d'une couleur disparaissent, sans toucher à une seule ligne de sérialisation.

**Quoi faire** : Dans moonlight_ui.cc/.h, sans déplacer de fichier : (a) renommer l'actuelle `SaveSettings()` en `FlushSettings()` (privée) et faire de `SaveSettings()` un simple `settings_dirty_ = true;` — les 45 sites d'appel de src/ deviennent corrects sans être modifiés ; (b) drainer une fois par frame en fin d'OnRenderUI : `if (settings_dirty_ && !ImGui::IsAnyItemActive() && !Bourgeon::Instance().IsMapLoading() && now - last_flush_ >= 500) { settings_dirty_ = false; FlushSettings(); }` ; (c) dans FlushSettings, relire l'existant (`YAML::Node root = YAML::LoadFile(path)` dans un try, `root = YAML::Node()` si échec), ne remplacer que `root["moonlight_ui"]`, réémettre `root` ; (d) écrire dans `path + ".tmp"`, fermer, vérifier le flux, puis `MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` ; (e) sortir `sb->SnapshotItemSlots()` de la ligne 1370 et l'appeler dans le drain de `sb->dirty_` (1866-1869) — une fonction de sauvegarde ne doit pas muter l'état d'un autre plugin ; (f) corriger ro_imgui.cc:1626-1641 (RoCombo) au passage, sinon le drain sera déclenché en permanence par un combo ouvert ; (g) découpler la largeur du chat : ne rappeler `chat::SetCustomWidth` que sur `ImGui::IsItemDeactivatedAfterEdit()` (ligne 2512-2515). Ajouter en tête de FlushSettings un commentaire listant les trois autres propriétaires de sections racine.

**Verification** : 1) Poser à la main dans bourgeon_settings.yaml une section `auto_login:` complète, entrer en jeu, cocher une case, quitter : la section doit toujours être là (c'est LE test du bug n°1). 2) Bouger un slider de couleur pendant 3 s : le yaml ne doit être réécrit qu'une fois (surveiller la date de modification du fichier, ou décommenter le LogInfo de fin de FlushSettings). 3) Ouvrir un combo « Ancrage »/« Animation » et le laisser ouvert 5 s : aucune écriture. 4) Glisser le curseur de largeur du chat sur un historique chargé : plus de saccade, et la largeur s'applique bien au relâchement. 5) Tuer le client (ALT-F4) pendant un drag : le yaml doit rester valide. 6) Vérifier qu'un changement de barre (drag d'une barre de skill) est toujours persisté malgré le déplacement de SnapshotItemSlots.

### 3. Sortir le toolkit UI du plugin : src/ui/ro_widgets.h, src/ui/align_grid.h, src/plugins/staff_gate.h

| | |
|---|---|
| Risque | faible |
| Effort | une demi-journée |
| Lignes touchees | moonlight_ui.cc 1724-1821 (~100 l. déplacées), moonlight_ui.h 15-40 et 52 et 70-141 (~110 l. déplacées), ~14 fichiers voient leur include changer |
| Gain | Fan-in de moonlight_ui.h : 18 TU → ~3 ; le cycle ui/ → plugins/ disparaît ; ~220 lignes de toolkit générique rendues à src/ui/, là où un contributeur les cherche. 25 des 34 commits historiques sur ce header touchaient la classe, pas les helpers : ils cessent de recompiler 18 TU. |

**Pourquoi** : 18 fichiers .cc incluent plugins/moonlight_ui.h (vérifié) et la moitié n'y viennent que pour HelpMarker, WheelSlider*, ColorPicker ou AlignGrid — leurs propres commentaires le disent (storage_tweaks.cc:21 « HelpMarker (tooltip) », spr_effect_lab.cc:13 « ColorPicker() (helper standardisé) », char_select.cc:18 « IsStaff() »). Pire, le VRAI toolkit maison en dépend : ui/ro_imgui.cc:16 inclut le plugin pour récupérer WheelSlider*, ce qui inverse les couches (ui/ → plugins/). C'est un pur déplacement de code : le compilateur attrape toute erreur, il n'y a aucun risque runtime.

**Quoi faire** : Créer `src/ui/ro_widgets.h` + `.cc` : HelpMarker (cc 1724-1732), WheelSliderFloat/Int (cc 1739-1773), PushStyleCompact/PopStyleCompact (cc 1776-1787), ColorPicker et les ~19 wrappers inline (h 70-141). Créer `src/ui/align_grid.h` + `.cc` : struct AlignGrid (h 15-40) + AlignGrid::Draw (cc 1801-1821) + HudReplaced (cc 1793-1797). Créer `src/plugins/staff_gate.h` : IsStaff() (h 52, cc 110-125). Retirer `#include "plugins/moonlight_ui.h"` de ro_imgui.cc et des plugins qui n'y venaient que pour un widget ; les remplacer par le nouvel en-tête. Ajouter les 2 .cc à src/CMakeLists.txt.

**Verification** : `--clean-first` obligatoire (règle projet : header très inclus). Si ça compile et link, le déplacement est correct par construction — ce sont des fonctions pures d'UI. Ensuite : ouvrir chaque fenêtre qui utilisait ces helpers (panneau principal, storage, inventaire, skill bar, status icons, SPR Lab, char-select) et vérifier que les tooltips « (?) », les sliders à molette et les color pickers ont exactement le même aspect et le même comportement.

### 4. Éclater OnRenderUI (1702 lignes) en un dossier src/plugins/moonlight_ui/, section par section

| | |
|---|---|
| Risque | moyen |
| Effort | 2 à 3 jours, étalés en 6 commits |
| Lignes touchees | 1841-3542 (déplacement), 128-178, 281-437 ; +7 fichiers dans src/plugins/moonlight_ui/ ; src/CMakeLists.txt |
| Gain | OnRenderUI passe de 1702 à ~120 lignes (prologue + 8 appels + 2 épilogues) ; profondeur d'accolades 11 → 6 ; retoucher un libellé recompile ~150 lignes au lieu de 3542 ; les 9 `changed` homonymes deviennent 9 valeurs de retour nommées ; le back-end (2,70 s de codegen mesuré, insensible au PCH) s'effondre. |

**Pourquoi** : OnRenderUI va de la ligne 1841 à 3542, soit 1702 lignes d'un seul tenant, 11 niveaux d'accolades au pic (2563) et 42 colonnes d'indentation (2561) ; 9 variables `bool changed` homonymes dans 9 portées de la MÊME fonction. Deux panneaux voisins à l'écran sont à 700 lignes l'un de l'autre dans le fichier. C'est le premier obstacle à toute modification, et c'est aussi ce qui a permis au bug EndTabBar (425 lignes entre Begin et End) de survivre à toutes les revues. Le découpage est mécanique et sans changement de comportement.

**Quoi faire** : Créer le dossier `src/plugins/moonlight_ui/` avec un `internal.h` NON exposé hors du dossier, déclarant `bool DrawRules(MoonlightUi&); bool DrawDps(MoonlightUi&); bool DrawFun(MoonlightUi&); bool DrawInterface(MoonlightUi&); bool DrawCommands(MoonlightUi&); void DrawAlootOverlay(MoonlightUi&); void DrawChatPresetBar(MoonlightUi&);` (chaque fonction RETOURNE son `changed`, l'appelant fait un seul `if (any_changed) SaveSettings();`). Attaquer dans cet ordre de risque croissant : (a) `panel_rules.cc` = 1904-2037, 134 lignes de texte pur, zéro état — commencer par là pour valider la mécanique ; (b) `panel_commands.cc` = 2974-3405, n'écrit que des membres de MoonlightUi et appelle SendSetting ; en profiter pour rentrer EndTabBar dans son if et scinder les deux onglets en `DrawTabGeneral`/`DrawTabAutoloots` ; (c) `panel_fun.cc` = 2040-2219 (DPS + Mini-jeux) ; (d) `item_desc_probe.cc` = 128-178 + 3410-3489 (hook + overlay alootid) ; (e) `chat_bg_patch.cc` = 281-437 (kChatBgSites, FindChatBgSites, ApplyChatBg, PatchChatBgObjects, ArgbFromPicker/PickerFromArgb) — y appliquer le garde RAII + SEH du bug HeapWalk et le test de VirtualProtect ; (f) `panel_interface.cc` = 2221-2971 en dernier. Restent PRIVÉS au TU d'origine : item_names_, g_ro_presets, les 22 miroirs de réglages serveur (qui ne doivent transiter que par SendSetting). Corriger au passage la bannière trompeuse 2091-2093 (« Interface de jeu » posée au-dessus du bloc « Mini-jeux ») et supprimer les commentaires morts 2242-2244.

**Verification** : Chaque étape (a)…(f) est un commit séparé, compilé et testé isolément. Le test est visuel et exhaustif : ouvrir le panneau, déplier les 8 en-têtes de premier niveau et les 11 sections de « Interface de jeu » l'une après l'autre, comparer avec une capture d'écran prise AVANT le chantier. Vérifier en particulier que chaque case cochée persiste toujours (le `changed` remonte bien) : cocher une case dans chaque section, quitter, relancer, vérifier. Vérifier aussi que les 3 sondes de dirty flags du prologue (1854-1869) et la barre de presets du chat (3491-3541) fonctionnent toujours.

### 5. Une seule table pour les 11 sections « Interface de jeu » (libellé + fonction de dessin)

| | |
|---|---|
| Risque | faible |
| Effort | 2 heures |
| Lignes touchees | moonlight_ui.h:172-176 ; moonlight_ui.cc 2285-2312 et 2330-2930 |
| Gain | 3 listes → 1 ; ajouter une section devient une ligne au lieu de 3 éditions dans 2 fichiers ; le désalignement libellé/contenu devient structurellement impossible ; 11 CalcTextSize par frame supprimés. |

**Pourquoi** : Trois listes parallèles maintenues à la main : l'enum `IfaceSection` (moonlight_ui.h:172-176, précédé du commentaire d'aveu « les deux listes DOIVENT rester alignées »), le tableau `kIfaceCats[11]` (2286-2297) et la cascade de 11 `if (s_iface_nav == N)` avec des littéraux 0..10 (2330 à 2859) qui n'utilisent JAMAIS l'enum. Insérer une section au milieu impose 3 éditions coordonnées dans 2 fichiers, et un décalage silencieux fait afficher le mauvais panneau sous le mauvais libellé — panne muette, sans aucun diagnostic, y compris pour OpenInterfaceSection (1827-1838) que d'autres plugins appellent.

**Quoi faire** : Après le chantier 4, remplacer par `struct IfaceEntry { const char* label; bool (*draw)(MoonlightUi&); };` et une unique `kIfaceSections[]` dans panel_interface.cc. La boucle de nav lit `kIfaceSections[i].label`, le rendu fait `changed = kIfaceSections[iface_nav_].draw(*this);`. L'enum du header devient une simple documentation d'index (ou disparaît au profit d'une recherche par nom dans OpenInterfaceSection). Renommer `s_iface_nav` (2285) en `nav` ou utiliser directement `iface_nav_` : le préfixe `s_` est un vestige qui ment sur la durée de vie (le header:378-379 précise que c'est un membre). Mettre en cache la largeur de nav calculée par 11 `CalcTextSize` à chaque frame (2304-2305), invalidée sur changement de police.

**Verification** : Cliquer les 11 entrées de la nav de gauche à droite et vérifier que chaque libellé affiche bien le contenu attendu (c'est exactement le défaut que le chantier prévient). Puis tester OpenInterfaceSection depuis un autre plugin : le bouton/puce de titre des autres fenêtres Bourgeon doit ouvrir la bonne section.

### 6. Remonter les 7 panneaux inlinés dans le DrawSettings() de leur plugin propriétaire

| | |
|---|---|
| Risque | moyen |
| Effort | 2 jours, 7 commits |
| Lignes touchees | moonlight_ui.cc 2340-2930 (~508 l. déplacées) ; 7 headers + 7 .cc de plugins |
| Gain | -508 lignes dans moonlight_ui.cc, -7 includes sur 31, et 11 sections traitées de façon uniforme au lieu de 4 + 7. Le texte d'aide français d'un plugin cesse de vivre à 700 lignes du code qu'il décrit. |

**Pourquoi** : Le patron existe déjà et fonctionne : 5 plugins délèguent proprement (`sb->DrawSettings()` 2333, `si->` 2662, `qt->` 2670, `st->` 2941, `en->` 2965), et skill_bar_tweaks.h:85 le documente (« contenu des réglages (réutilisé dans l'onglet MoonlightUi) »). Mais 7 panneaux sont écrits ici sur ~508 lignes et manipulent directement les membres d'un AUTRE plugin (`eb->bars_[i].fill`, `eb->ports_[i].rounding`, `mi->saved_[ic.name]`, `iv->layout_`, `stg->favorites_`). Un contributeur qui travaille sur StorageTweaks ne trouve pas ses réglages dans storage_tweaks.cc mais à moonlight_ui.cc:2786. La règle « où écrire mon panneau ? » n'a aujourd'hui pas de réponse.

**Quoi faire** : Ajouter `void DrawSettings();` (ou `bool DrawSettings();` retournant `changed`) à BasicInfoTweaks, ChatTweaks, MenuIconTweaks, ItemDescTweaks, StorageTweaks, InventoryViewer et NpcDialogTweaks, et y déplacer littéralement les blocs : Basic Info 2340-2493, Chat 2496-2618 (en supprimant au passage la garde fantôme `eb` de 2498), Icônes du menu 2621-2657, Descriptions 2676-2709, Fenêtre NPC 2767-2783, Storage 2786-2856, Inventaire 2859-2930. Le Skin RO (2712-2764) part dans `ro::DrawSkinSettingsSection()` côté src/ui/. moonlight_ui ne garde que `if (auto* p = …) changed = p->DrawSettings(); else ImGui::TextDisabled(kUnavailableText);`. Sortir aussi la lambda `render_chatbg` (2537-2604, 68 lignes définies au milieu d'un panneau) en fonction libre `bool DrawChatBgPicker(MoonlightUi&, ChatBgGroup&)` dans chat_bg_patch.cc. Uniformiser au passage les deux libellés de repli concurrents (`"Indisponible."` × 6 et `"(plugin indisponible)"` × 6) en une constante unique.

**Verification** : Section par section, un commit chacune. Pour chaque panneau déplacé : ouvrir la section, modifier CHAQUE réglage qu'elle contient, vérifier l'effet en jeu immédiatement, puis relancer le client et vérifier que le réglage a été persisté. C'est le point sensible : le `changed` doit remonter jusqu'au SaveSettings de l'appelant, sinon on introduit exactement le bug skillbar_locked à l'échelle d'un panneau entier. Vérifier aussi le panneau Chat sans BasicInfoTweaks (la garde fantôme supprimée ne doit rien casser).

### 7. Un seul codec couleur et un parseur hexa non-lançant

| | |
|---|---|
| Risque | faible |
| Effort | une journée |
| Lignes touchees | 62-69, 424-437, 481-483, 520-524, 541-545, 599-601, 647-666, 809-813, 929, 974-1017, 1087, 1118-1119, 1240-1277, 1373-1382, 1398, 2554-2557, 3517-3520 |
| Gain | ~90 lignes de plomberie supprimées, 4 lambdas dupliquées et 19 snprintf éliminés, un seul format de couleur à connaître, et surtout la disparition des 7 `std::stoul` qui peuvent faire perdre toute la config. Ajouter une couleur au skin passe de 5 éditions à 1. |

**Pourquoi** : Deux encodages incompatibles cohabitent dans le MÊME yaml : `ArgbFromPicker`/`PickerFromArgb` (424-437, ordre ARGB, écrit en chaîne « %08X ») et `PackCol`/`UnpackCol` (62-69, `ImGui::ColorConvertFloat4ToU32`, donc ordre ABGR, écrit en ENTIER DÉCIMAL pour statusicon_* et ro_skin_*). Les deux prennent `const float c[4]` et rendent un `uint32_t` : rien dans les noms n'avertit que les sorties diffèrent — copier une ligne de sauvegarde d'un bloc à l'autre inverse R et B. Conséquence directe : 4 lambdas locales quasi identiques ont été créées pour compenser (`load_dps_col` 520, `load_color` 541, `load_col` 647, `load_sbcol` 809) plus 2 copies inline (481, 599), et 19 `snprintf("%08X", …)` côté écriture dont 9 d'affilée en 1373-1382. Et les 7 `std::stoul` qui vont avec sont le déclencheur du bug de perte totale de config.

**Quoi faire** : Deux fonctions libres dans le TU de persistance : `bool ReadArgbKey(const YAML::Node& ui, const char* key, float out[4])` (basée sur un `ParseHex8` non-lançant construit sur `std::strtoul` + contrôle du pointeur de fin, PAS `std::stoul`) et `void WriteArgbKey(YAML::Emitter& out, const char* key, const float c[4])`. Les utiliser aux 6 sites de lecture et aux 19 sites d'écriture. Renommer explicitement le second couple (`ImU32FromPicker`/`PickerFromImU32`) et documenter l'ordre d'octets sur chaque signature. Remplacer les deux décodages ARGB→ImVec4 réinlinés à l'identique (2554-2557 et 3517-3520) par un `ImVec4 ImVec4FromArgb(uint32_t)`. Écrire enfin le `EmitSkinCfg(YAML::Emitter&, const ro::RoSkinConfig&, const char* prefix)` que le commentaire de la ligne 1261 promet mais qui n'a JAMAIS existé (`grep -rn EmitSkinCfg src/` ne renvoie que ce commentaire) : les 17 champs du skin sont aujourd'hui épelés QUATRE fois (70-89, 644-666, 1240-1256, 1261-1277).

**Verification** : Test de non-régression sur un yaml EXISTANT : sauvegarder une copie du bourgeon_settings.yaml avant, relancer, resauvegarder, comparer les deux fichiers — les valeurs de couleur doivent être identiques (pas seulement équivalentes : même casse hexa, même encodage). Puis vérification visuelle : les couleurs du DPS meter, des barres, de la skillbar, du fond de chat, de la barre d'icônes de statut et du skin RO doivent être inchangées après un cycle complet quitter/relancer. Tester explicitement une clé volontairement corrompue (`chat_bg: "ZZZZZZZZ"`) : le client doit démarrer avec cette seule couleur aux défauts et TOUT le reste de la config intact — c'est le test du bug de perte totale.

### 8. Inverser la propriété de la persistance : une table de descripteurs, puis LoadConfig/SaveConfig par plugin

| | |
|---|---|
| Risque | eleve |
| Effort | 3 à 5 jours, une dizaine de commits |
| Lignes touchees | 452-1443 en totalité |
| Gain | 991 lignes → ~500 (table ~215 + moteurs ~90 + résidu ~195), 191 paires à synchroniser à la main → 0, coût d'ajout d'un réglage : 3 éditions distantes → 1 ligne. La classe de bug « réglage lu mais jamais écrit » devient structurellement impossible. |

**Pourquoi** : C'est le plus gros gain (991 lignes, 28 % du fichier) mais aussi le plus dangereux, donc il vient en dernier : `LoadSettings` 452-968 (517 l.) et `SaveSettings` 970-1443 (474 l.) sont deux miroirs manuels de 191 clés communes (170 lues, 224 écrites), maintenus à la main dans le même ordre, sans table commune. C'est exactement ce mécanisme qui a produit le bug skillbar_locked, la divergence itemdesc_anchor (3 vs 0) et les 66 fallbacks ternaires. Le fichier touche l'état interne de 25 plugins qui ont dû rendre leurs membres publics pour lui (dps_meter.h:23-24 le documente : « Settings (read/written by MoonlightUi) »).

**Quoi faire** : Étape A (mécanique, faible risque) : décrire les ~190 réglages triviaux dans une table unique parcourue dans les deux sens. `enum class SType { kBool, kInt, kFloat, kStr, kColHex, kColU32 };` ; le champ vivant dans un autre plugin potentiellement absent, le descripteur porte un résolveur en lambda SANS capture (donc convertible en pointeur de fonction, table en .rdata) : `#define OWNER(getter, member) []() -> void* { auto* o = Bourgeon::Instance().getter(); return o ? &o->member : nullptr; }`. Le champ `def` du descripteur devient LA source unique du défaut, utilisée par la lecture ET par le fallback d'écriture — les 66 littéraux ternaires disparaissent. Uniformiser la règle « plugin absent » : ré-émettre la valeur BRUTE du yaml chargé (conservation) au lieu de la perdre — aujourd'hui 151 clés disparaissent du fichier si le plugin manque (blocs `if (eb)` 1081-1129, `if (mi)` 1138-1146, `if (sb)` 1356-1392) alors que 20 lignes plus haut le MÊME `eb` est traité en fallback (1059). Étape B : garder trois zones nommées et courtes pour les ~195 lignes qui résistent — `LoadContainers`/`SaveContainers` (menu_icons, inventory_layout, storage_favorites, chat_bg_presets, ro_skin_presets, equip_presets), `PostLoadApply()` pour les effets de bord (ApplyChatBg 472, chat::SetCustomWidth 508, SetTimestamps 510, SetItemIcons 512, LogConsole::SetLevel 513, ro::SetFontEnabled 639, si->MarkDirty 864, st->Apply 909, SetModernInterface 838, les clamps 506-507), `MigrateLegacyKeys()` daté et supprimable (expbar_grid_* 593-601, skillbar_text_scale 791). Étape C (facultative, plus tard) : donner à chaque plugin `LoadConfig(const YAML::Node&)` / `SaveConfig(YAML::Emitter&)` et re-privatiser ses membres ; le précédent existe déjà dans le repo (window_pos_tweaks.h:41-45 expose une API d'énumération « adding a window needs no yaml edit there »).

**Verification** : Aucun test automatisé ne couvre ça, donc la vérification doit être un DIFF : (1) partir d'un yaml de référence riche (toutes les fonctionnalités activées, couleurs personnalisées, presets, favoris storage, layout d'inventaire) ; (2) avant le chantier, entrer en jeu, forcer une sauvegarde, archiver le fichier produit comme référence ; (3) après chaque tranche du chantier, refaire exactement la même manipulation et diffs les deux fichiers — le résultat doit être identique à l'ordre des clés près. Toute clé qui disparaît du diff est un réglage perdu. (4) Avancer par tranches de ~20 clés, un commit chacune, jamais les 190 d'un coup. (5) Tester explicitement le cas « plugin absent » en désactivant un plugin dans bourgeon.cc : ses clés doivent être CONSERVÉES dans le fichier, pas effacées.

### 9. Hygiène partagée : un seul FindWindow, un seul GameDir, un seul cache d'icônes

| | |
|---|---|
| Risque | faible |
| Effort | une journée, à étaler |
| Lignes touchees | moonlight_ui.cc 46, 182-196, 1793-1797 ; ~18 fichiers de src/plugins ; src/ragnarok/ui_window_mgr.h ; src/utils/ |
| Gain | 18 déclarations d'adresses natives → 1 point de vérité pour le prochain portage de client ; ~35 lignes sur src/ pour les chemins ; ~700-900 lignes dupliquées supprimées pour le cache d'icônes. Un seul parseur du yaml au lieu de deux avec des règles différentes. |

**Pourquoi** : Chantier de fond, sans urgence, à grignoter entre deux autres. `HudReplaced()` est copié caractère pour caractère dans 3 plugins (moonlight_ui.cc:1793-1797, basic_info.cc:93-97, menu_icons.cc:76-79, le commentaire 1792 avouant la copie) ; le couple d'adresses g_UIWindowMgr 0x0131f4e8 / FindWindow 0x00a47b90 est redéclaré dans 18 fichiers de src/plugins, et ici seule occurrence NON nommée, écrite en littéral dans l'expression — invisible au grep « kFindWindow ». Le bloc « dossier de l'exe » (GetModuleFileNameA + find_last_of) est réécrit 2 fois dans ce fichier (183-187 et 192-196, à 9 lignes d'intervalle) et 5 fois ailleurs. Et le même cache d'icônes ImGui est dupliqué dans 8 plugins (237 collisions de noms en namespace anonyme mesurées, TODO déjà connu du projet).

**Quoi faire** : Ajouter à `src/ragnarok/ui_window_mgr.h` : `namespace uiwnd { void* FindWindow(int id); bool IsHudReplaced(); constexpr int kWorldMapWndId = 0x8c; }` (le fichier est DÉJÀ inclus ligne 46 mais n'expose pas FindWindow), supprimer les 3 HudReplaced et les 18 paires de constantes locales. Créer `src/utils/game_paths.h` avec `std::string GameDir()` et `std::string SettingsPath()` (ou promouvoir `ModuleDir()` qui existe déjà dans log_console.cc:34-39, module que moonlight_ui.cc inclut pourtant ligne 50) et remplacer les 7 copies, dont le littéral "bourgeon_settings.yaml" répété dans 5 fichiers. Extraire `src/ui/icon_cache.h/.cc`. Supprimer le parseur yaml ad hoc de log_console.cc:53-68 (recherche textuelle de « log_level: » ligne par ligne) une fois que LoadSettings appliquera ses défauts inconditionnellement.

**Verification** : Pour uiwnd : ouvrir la carte du monde (world map, id 0x8c) et vérifier que la grille d'alignement, les barres et les icônes de menu se masquent toujours — c'est exactement ce que teste HudReplaced. Pour game_paths : lancer le client depuis un dossier au chemin contenant un espace et un accent, vérifier que bourgeon_settings.yaml et itemInfoMerged.lua sont bien trouvés (regarder bourgeon.log). Pour le cache d'icônes : ouvrir l'inventaire, le storage, le chat avec icônes, la fiche de perso et la barre de skills dans la même session, puis provoquer un reset de device (ALT-TAB plein écran) — les icônes doivent se recharger sans crash ddraw.

## A NE PAS faire

- Supprimer les 31 `#include "plugins/..."` (lignes 15-45) « pour accélérer la compilation » : mesuré, tous les headers de src/plugins réunis pèsent ~1,5 % du TU préprocessé (Windows SDK + STL = 94,6 %). Le gain serait nul et on perdrait les types nécessaires. Les includes doivent disparaître comme CONSÉQUENCE du déplacement du code (chantiers 4-6), jamais comme objectif.

- Activer `UNITY_BUILD` dans src/CMakeLists.txt : 43 fichiers de src/plugins ouvrent un `namespace {` avec 237 noms en collision (kMakeKey dans 16 fichiers, kTexMgr/kUIWindowMgr dans 13…). Le projet a déjà été mordu par des ODR → crash (règle « --clean-first après modif d'un header très inclus »). /MP + PCH donnent le même gain sans le risque.

- Mettre bourgeon.h, plugin.h, ro_imgui.h ou moonlight_ui.h dans le PCH : ce sont précisément les headers qui changent (moonlight_ui.h = 34 commits). Le PCH ne doit contenir que du tiers stable (Windows.h, STL, imgui, spdlog/fmt, yaml-cpp), sinon chaque commit invalide le PCH et on perd tout le bénéfice.

- Découper le try/catch géant de LoadSettings (462-967) AVANT d'avoir remplacé les 7 `std::stoul` par un parseur non-lançant : aujourd'hui une exception annule tout ; si on découpe d'abord, la section fautive est la seule annulée mais on garde une exception silencieuse par section — on change la sémantique sans corriger la cause.

- Déporter SaveSettings dans un thread ou la rendre `const` tant que `sb->SnapshotItemSlots()` (ligne 1370) est appelé depuis son corps : cette ligne lit la mémoire du client sous SEH depuis le thread de rendu. Il faut d'abord la sortir (chantier 2), sinon on déplace une lecture de mémoire jeu hors du thread principal.

- Charger la charte « Règles du serveur » depuis le serveur ou un fichier distant en première étape : commencer par le simple déplacement dans un TU dédié. Un chargement fichier ajoute des I/O + un piège d'encodage (ImGui attend de l'UTF-8, le client parle CP949) pour zéro gain de lisibilité.

- Supprimer le hook ItemDescWndHook (134-178) et ses globales (128-132) avant qu'ItemDescTweaks n'expose une API stable (item_id courant + position + ouvert/fermé) : l'overlay alootid (3410-3489) est le seul consommateur et casserait silencieusement.

- Toucher bourgeon_integrity.conf ou bumper un hash pendant ces chantiers (règle projet : la prod en dépend). Compiler et tester, rien d'autre.

- Remplacer les 3 sondes de dirty flags du prologue (1854-1869) par une garde `IsMapLoading()` posée AVANT `grid_.Draw()` sans réfléchir : la grille et le drain de geometry_dirty_ n'ont pas les mêmes contraintes. La garde doit s'appliquer aux actions (SaveSettings, spr_lab), pas à tout le rendu.

## Regles a graver dans CLAUDE.md

- Un réglage se déclare UNE fois. Interdiction d'ajouter une clé yaml à deux endroits miroir (une en lecture, une en écriture) : passer par la table de descripteurs ou par le LoadConfig/SaveConfig du plugin propriétaire. Un réglage qui a une case à cocher et pas de clé est un bug (cf. skillbar_locked).
- Aucune écriture disque depuis OnRenderUI ni depuis un callback de widget. Un changement pose `settings_dirty_` ; le flush est unique, différé et sorti par la boucle de fin de frame. Toute écriture d'un fichier partagé se fait en relire → fusionner → tmp → MoveFileEx, jamais en troncature.
- bourgeon_settings.yaml est PARTAGÉ : les sections racine auto_login, char_select et moonlight_auth appartiennent à d'autres composants. Ne jamais réémettre le document entier ; ne remplacer que sa propre section.
- Un widget qui renvoie true à chaque frame (slider, color picker, combo ouvert) ne persiste et ne déclenche d'action coûteuse qu'au relâchement : `ImGui::IsItemDeactivatedAfterEdit()`. C'est vrai pour les widgets ImGui ET pour les wrappers ro::.
- Les réglages d'un plugin vivent dans le `DrawSettings()` de ce plugin, jamais inlinés dans moonlight_ui. moonlight_ui n'est qu'une navigation : `if (auto* p = …) changed = p->DrawSettings(); else ImGui::TextDisabled(kUnavailableText);`.
- Une fonction de rendu ne dépasse pas ~150 lignes et 5 niveaux d'accolades. Un Begin/End ImGui (TabBar, Table, Combo, Popup, Child) doit être apparié dans le même écran ; si la paire ne tient pas à l'œil, la section doit être extraite.
- Les helpers génériques (HelpMarker, sliders, color pickers, grille d'alignement) vivent dans src/ui/, jamais dans un plugin. Un fichier de src/ui/ n'inclut JAMAIS un header de src/plugins/.
- Pas de nouvelle adresse native en dur dans un plugin : les adresses du UIWindowMgr, du TexMgr et des fenêtres passent par src/ragnarok/. Toute adresse écrite en littéral dans une expression (au lieu d'une constante nommée) est refusée en revue — elle devient invisible au grep lors d'un portage de client.
- Toute chaîne d'UI en français porte ses accents (é è à ç ù ê î ô), source en UTF-8. Aucune chaîne anglaise dans un panneau francophone. Tout texte venant du client (nom d'item, d'objet, de map) est converti par ro::Cp949ToUtf8 AU MOMENT DU STOCKAGE, jamais à l'affichage (le buffer retourné est rotatif sur 8 emplacements).
- Un parcours du tas du client (HeapWalk) ou une écriture dans .text s'entoure d'un __try/__except et d'un garde RAII qui libère le verrou dans son destructeur, et vérifie la valeur de retour de VirtualProtect. Aucun de ces deux mécanismes ne se déclenche pendant un chargement de map.
- Aucune valeur venant du yaml n'est écrite telle quelle dans un membre servant d'index ou de borne (first_slot, slot_count, tailles) : clamp systématique au chargement, aux mêmes bornes que le slider correspondant. Le parsing hexadécimal utilise un helper non-lançant, jamais std::stoul.
- Une fonction nommée Save/Serialize ne mute rien : pas d'appel à SnapshotItemSlots ni à un setter de plugin depuis le chemin d'écriture.
- Vérification obligatoire pour tout chantier de persistance : archiver un bourgeon_settings.yaml de référence AVANT, refaire la même manipulation APRÈS, diffs les deux fichiers. Toute clé qui disparaît est un réglage joueur perdu.
