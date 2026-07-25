# Charte de nommage - Bourgeon

> Issue de l'audit de nommage de `src/plugins/moonlight_ui.cc` (3542 l.) et de l'extraction de la convention réellement en vigueur dans le code déjà bien tenu (`ro_imgui.h`, `plugin.h`, `session.h`, `keyboard_move`, `entity_names`).

## État des lieux

Le fichier a été écrit par accumulation de sessions, et cela se lit dans les noms : la fonction LoadSettings fait 516 lignes (moonlight_ui.cc:452-968) et y recycle « p » pour au moins cinq choses (pointeur d'immédiat :394, préfixe de clé yaml :547, preset de skin :671, nœud yaml :925), « c » pour six (config de skin :71, canaux R/G/B/A :425-428, tampon de couleur des lambdas :520/541/647, index de carte :956), et « g » pour trois (ChatBgGroup :467, canal vert :426, D3D9PostFx :886). Au total, le token « p » apparaît 135 fois dans le .cc, « c » 124, « d » 98, « g » 75, « eb » 86 : l'alphabet d'une lettre est devenu le vocabulaire par défaut. La dette la plus coûteuse n'est pas là où elle est la plus visible : ce sont les champs du header, lus depuis les 3542 lignes du .cc, dont le sens est indevinable (separate_ h:244, wings_ h:257, noks_ h:256, no_ask_ h:255, tri_inv_ h:260, aloot_pognon_ h:248). Le danger dominant est l'ordre des octets des couleurs, qu'aucun nom ne porte : load_color (cc:541) décode une chaîne hex ARGB, load_col (cc:647) décode un ImU32 ImGui — R et B inversés, noms quasi identiques, même fonction. Les alias de plugins frères sur deux lettres ont atteint leur limite mécanique : le code a dû inventer sb2 et tt2 (cc:832-833), et « st » désigne un ImGuiStyle en :2302 puis un SettingsTweaks* en :2940. Deux noms cachent des défauts réels et pas seulement de la lisibilité : le gate mort de la section Chat (cc:2498) et l'alias s_iface_nav (cc:2285) dont le préfixe « s_ » ment. Le reste du dépôt est nettement plus sain (100 % des membres en snake_case + underscore, 99 % des constantes en k+PascalCase, 617 adresses RE sur 619 au bon format) : la convention existe, ce fichier ne l'applique simplement plus.

## Règles

- **Règle d'or — un nom porte le CONTENU + l'UNITÉ + l'ORIGINE.** « De quoi c'est », pas « quel type c'est ». Si un nom a besoin du commentaire d'à côté pour être compris, c'est le nom qu'il faut corriger. ✅ `int y_offset_ = 2;  // décalage vertical (px)` (entity_names.h:46), `refresh_ms_` (keyboard_move.h:71) — ❌ `int win = dps->dps_window_secs_;` (moonlight_ui.cc:2061) : le membre source portait l'unité, la copie locale l'a perdue.

- **R1 — Longueur du nom ∝ durée de vie de la variable, et zéro homonyme dans la même fonction.** `i`, `x`, `y`, `dx`, `dy` sont bons sur 3-5 lignes sans voisin ressemblant. Au-delà, nom complet. ❌ `LoadSettings` (moonlight_ui.cc:452-968, 516 lignes) où `p` est tour à tour un `uint32_t*` (:394), un préfixe de clé yaml (:547), un `RoPreset` (:671) et un nœud yaml (:925) ; ❌ `DrawRoScrollbar(ImGuiWindow* w)` sur 117 lignes (ro_imgui.cc:319) alors que le même fichier écrit `win` ailleurs.

- **R2 — L'ENCODAGE des couleurs va dans le nom : `_rgba` (float[4] ImGui), `_argb` (uint32 natif), `_argb_hex` (chaîne de 8 car.), `_imu32` (IM_COL32).** Trois conventions coexistent dans ce projet et deux d'entre elles échangent R et B. ❌ `load_color` (moonlight_ui.cc:541, hex ARGB) et `load_col` (moonlight_ui.cc:647, ImU32 ImGui) : deux décodeurs incompatibles, noms quasi identiques, même fonction. ✅ `ArgbFromPicker` / `PickerFromArgb` (moonlight_ui.cc:424/432) disent leur sens de conversion.

- **R3 — Unité systématique dans le nom : `_px`, `_ms`, `_secs`, `_pct`, `_zeny`, `_bytes`, `_count`, `_cs`.** ✅ `chat_width_px_` (moonlight_ui.h:331), `combat_timeout_secs_`, `last_req_ms_` (keyboard_move.h:75) — ❌ `int timeout = dps->combat_timeout_secs_;` (moonlight_ui.cc:2068) juste à côté d'un `slot_ms_` en millisecondes ; ❌ `aloot_pognon_` (moonlight_ui.h:248) qui est un seuil en zeny divisé par 100 sur le fil.

- **R4 — Locales et paramètres en snake_case, jamais en camelCase.** ✅ `const bool keys_ours` (keyboard_move.cc:273) — ❌ `hookAddr` / `hookType` (hook_manager.cc:67) dans un fichier qui écrit déjà `hook_addr` (hook_manager.cc:24) pour exactement la même chose.

- **R5 — Membre de classe = snake_case + `_` final (même public) ; champ de struct POD = snake_case sans underscore.** ✅ `show_players_` (entity_names.h:41), `int y_offset_` (entity_names.h:46) — ❌ `EquipPresetItem::leftHand` (character_sheet.h:18), `EquipPreset::hotkeyVk` / `hkCtrl` (character_sheet.h:26-27), `TrackedWindow::posX` / `wasOpen` / `lastSave` (window_pos_tweaks.cc:64-70).

- **R6 — Constante = `k` + PascalCase, préfixée par le SUJET et jamais par une initiale inventée.** ✅ `kOffActorX` (keyboard_move.cc:27), `kPopupId` (dx7_warning.cc:20), `bopcodes::kBugReport` (bourgeon_opcodes.h:38) — ❌ `kcPose` / `kcSpr` (basic_info.cc:832-833) → `kChildPose` / `kChildSpr` ; ❌ `keInvIndex` / `keResname` (character_sheet.cc:38-41) → `kEquipInvIndex` / `kEquipResname`.

- **R7 — Adresse native = `constexpr uintptr_t k<Sujet><Action>` + commentaire donnant le nom natif exact, sous un bandeau qui fixe la version du client ; offset = `kOff<Objet><Champ>`.** ✅ `constexpr uintptr_t kWorldToTile = 0x00c6aa80;  // MapCoord_WorldToTileAndSub` (keyboard_move.cc:15) sous `// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live)` (keyboard_move.cc:11). Variante minoritaire à ne plus étendre : `kGameMode_GetActive` (entity_names.cc:22).

- **R8 — Aucun nom Ghidra/IDA brut comme identifiant C++.** `param_N`, `FUN_*`, `DAT_*`, `local_*`, `uVar*` vivent dans les COMMENTAIRES, où ils font le pont avec le décompilé. ❌ `PacketBufReaderHook(uint8_t *param_1)` (rag_connection.h:44) ; ❌ `ItemDescWndHook(void* ecx, void*, uint32_t p1, int p2, int* p3, int p4, int p5, int p6)` (moonlight_ui.cc:134-136) où `p2` est en réalité l'ID de message UI et `p3` la struct d'item.

- **R9 — Booléen : état = adjectif/participe (`_enabled`, `_open`, `_dirty`) ; fonction = `Is…` / `Has…` / `Any…`. Jamais `ok`, `flag`, `res`, `val`.** ✅ `was_moving_` (keyboard_move.h:76), `IsStaff()` (moonlight_ui.cc:125) — ❌ `bool found` employé pour « déjà dans la liste » (moonlight_ui.cc:1643, :3180) et pour « un preset porte déjà ce nom » (moonlight_ui.cc:2753) ; ❌ `separate_` (moonlight_ui.h:244) → `separate_kills_enabled_`.

- **R10 — Un drapeau one-shot dit qu'il se consomme : préfixe `pending_`.** ❌ `apply_collapse_` (moonlight_ui.h:372), `collapse_requested_` (moonlight_ui.h:375), `iface_jump_` (moonlight_ui.h:382) sont tous posés puis remis à false au premier rendu, et aucun ne le dit dans son nom.

- **R11 — `g_` pour un global, `s_` UNIQUEMENT pour une vraie statique, tout le reste du `.cc` dans un `namespace { }` anonyme.** ❌ `int& s_iface_nav = iface_nav_;` (moonlight_ui.cc:2285) : le `s_` ment, c'est un alias sur un membre piloté de l'extérieur par `OpenInterfaceSection`. ❌ `static char hdr[96];` (moonlight_ui.cc:3197) : écrit et lu dans le même frame, le `static` est inutile et trompeur.

- **R12 — Un pointeur de plugin frère porte le nom du plugin, pas ses initiales.** ✅ `if (auto* dps = Bourgeon::Instance().dps_meter())` — ❌ la famille `iv` / `stg` / `sb` / `tt` / `mi` / `si` / `qt` / `st` / `en` / `cs` / `cse` / `sh` / `nd` / `lp` (moonlight_ui.cc:444-447, 618-935, 1132-1348). La convention est saturée : le code a dû inventer `sb2` et `tt2` (moonlight_ui.cc:832-833) pour éviter une collision, et `st` désigne un `ImGuiStyle` en :2302 puis `SettingsTweaks*` en :2940.

- **R13 — Types PascalCase ; plugin = `<Sujet>Tweaks` s'il retouche du natif, `<Sujet>` s'il ajoute une feature ; typedef de fonction native = `<Nom>_t` (262 usages) et non `…Fn` (101) ; namespace en minuscules explicites (`char_info`, pas `ci`).**

- **R14 — API qui franchit un module : `Module_Fonction`, TOUS les paramètres nommés, unité incluse.** ✅ `Overlay_DeviceEpoch()`, `FrameProfiler_Tick()` — ❌ `D3D9_CompositeQuadsRGBA(const D3D9TexQuad* quads, int n, int w, int h, void* out_argb)` (d3d9_hook.h:101) ; ❌ paramètres anonymes de `SetJmpHook` / `UnsetJmpHook` (hook_manager.h:40-43) dont le 2e argument change de sens entre Set (destination) et Unset (trampoline original).

- **R15 — Renommer un champ ne renomme JAMAIS sa clé YAML, son ID ImGui ni un libellé UI.** Les réglages joueurs vivent dans bourgeon_settings.yaml : `chat_width_px_` est persisté sous `ui["chat_width"]` (moonlight_ui.cc:505) et cette chaîne doit rester intacte. Corollaire : un lot de renommage pur ne doit produire AUCUNE ligne de diff contenant un littéral chaîne.

- **R16 — Après renommage dans un header très inclus (moonlight_ui.h, ro_imgui.h, plugin.h), rebuild `--clean-first` obligatoire** : `cmake --build "d:/Mes documents/GitHub/Bourgeon/build" --config Release --clean-first`. Un incrémental laisse des ODR croisés et se solde par un crash au lancement.

## Noms trompeurs (priorité absolue)

Un nom vague ralentit ; un nom qui ment fait écrire un bug.

- **moonlight_ui.cc:2498 — `if (auto* eb = Bourgeon::Instance().basic_info())` : le nom cache un DÉFAUT, pas seulement un manque de lisibilité.** Le pointeur n'est jamais déréférencé dans les 118 lignes du bloc « Chat » (:2499-2616, qui ne touche que discord_chat_, chat_width_*, chat_timestamps_, chat_item_icons_, chat_bg_*). Conséquence réelle : tous les réglages du chat disparaissent de l'UI si BasicInfoTweaks est absent. À corriger AVANT tout renommage, et dans un commit séparé.

- **moonlight_ui.cc:541 `load_color` vs moonlight_ui.cc:647 `load_col` — encodages opposés sous des noms quasi identiques, dans la même fonction.** `load_color` parse une chaîne hex ARGB via PickerFromArgb ; `load_col` lit un entier et le décode via ImGui::ColorConvertU32ToFloat4 (ordre RGBA d'ImGui). Copier un appel de l'un vers l'autre échange silencieusement le rouge et le bleu, sans erreur de compilation. Le renommage le plus urgent du fichier.

- **moonlight_ui.cc:2285 `int& s_iface_nav = iface_nav_;` — le préfixe `s_` ment.** Ce n'est pas une statique de fonction mais une référence sur le membre `iface_nav_` (moonlight_ui.h:379), piloté depuis l'extérieur par `OpenInterfaceSection`. Qui croit à une statique locale peut supprimer l'alias « inutile » et casser l'ouverture directe d'une section depuis le bullet de barre de titre.

- **moonlight_ui.h:227 `kSettingStaff` — annonce un booléen, contient un entier.** La valeur du setting 26 est le niveau de groupe serveur (pc_get_group_level) ; le seuil staff (80) est appliqué ailleurs, dans `IsStaff()` (moonlight_ui.cc:125, kStaffMinGroupLevel:120). Un `if (kSettingStaff)` écrit de bonne foi serait faux. → `kSettingGroupLevel`.

- **moonlight_ui.cc:410 `const auto* vtable_ptr = static_cast<const uint32_t*>(entry.lpData);` — ne pointe pas la vtable mais le début de l'objet.** C'est son déréférencement (`*vtable_ptr != t.vtable`, :413) qui donne la vtable. Sur du parcours de tas brut, ce genre d'écart d'un niveau d'indirection est exactement ce qui produit un crash. → `obj_first_word`.

- **moonlight_ui.h:22 `int cell() const` — renvoie une TAILLE en pixels, pas une cellule.** C'est `size` (h:18) borné à 4 minimum, consommé comme un pas de grille : `const float step = static_cast<float>(cell());` (cc:1803) et `const float g = static_cast<float>(cell());` (h:33). → `cell_size_px_clamped()`, et `size` → `cell_size_px`.

- **moonlight_ui.h:135 `ColorPicker(const char* label, float col[4])` — appelle `ImGui::ColorEdit4`, pas `ImGui::ColorPicker4`.** Deux widgets ImGui bien distincts existent sous ces deux noms ; le nom fait attendre le mauvais. 28 sites d'appel dans le projet. → `ColorEdit4WithAlphaBar`.

- **moonlight_ui.h:121 `IsHovered()` — teste le DERNIER widget soumis (`ImGui::IsItemHovered()`), pas la fenêtre ni un objet nommé.** Le nom sans sujet invite à l'appeler loin du widget concerné, où il renverra un résultat sans rapport. Utilisé par `Tooltip` (h:126). → `IsLastItemHovered()`.

- **moonlight_ui.h:235 `in_gonryun_` — fige un nom de carte que le code ne connaît plus en dur.** La carte vient de la constante `kDiscordMap` (h:232), et la comparaison est un `strncmp` de PRÉFIXE (cc:1483), donc « gonryun_in » matche aussi. Le champ signifie « je suis sur la carte qui active le relais Discord ». → `on_discord_relay_map_`, et `kDiscordMap` → `kDiscordRelayMapPrefix`.

- **moonlight_ui.cc:2397 `auto preset = [&](const char* label, int w, int h)` — n'est pas un preset.** C'est le bouton qui applique une taille w×h à TOUTES les barres d'info et lève `force_apply_` (:2404), appelé pour XS/S/M/L (:2408-2411). Le mot « preset » désigne déjà trois autres familles dans ce fichier. → `bar_size_button(label, width_px, height_px)`.

- **moonlight_ui.h:269 `struct AlootPreset { uint8_t no; … }` — `no` se lit comme une négation.** C'est le numéro d'emplacement serveur du preset autolootid (0 = aucun), reçu dans ZC 0x0F07 (cc:1501) et renvoyé dans CZ 0x0F06 (cc:1717). → `slot_no`.

- **moonlight_ui.cc:949 `for (const YAML::Node& it : items)` — `it` n'est pas un itérateur.** C'est un nœud yaml décrivant une pièce d'équipement, alors que dans la même fonction `it` est bien un itérateur (cc:626, `for (auto it = icons.begin(); …)`). → `item_node`.

- **moonlight_ui.cc:3423 `bool enriched_item = false;` — n'est pas un item mais l'état « le panneau de description enrichi est activé » (:3425), qui sert à supprimer l'overlay alootid autonome pour éviter le doublon. → `enriched_item_panel_on`.

- **moonlight_ui.cc:3197 `static char hdr[96];` — le `static` est un mensonge sur la durée de vie.** Le tampon est recomposé et consommé dans le même frame (:3210-3218) ; le `static` ne sert à rien mais fait croire à un état persistant. → `char preset_header_text[96]`, sans `static`.

- **moonlight_ui.h:284 `kSkillDescWndAddr` — annonce une adresse de hook qui n'existe plus.** Une seule occurrence dans tout src/ : sa propre déclaration (le hook a été retiré, cf. le commentaire cc:170-176 sur le chemin enrichi). À supprimer ou à basculer en commentaire documentaire.

- **moonlight_ui.cc:3436 `const auto* base = static_cast<const uint8_t*>(g_item_desc_wnd_ptr);` — troisième sens de `base` dans le fichier** (cc:194 = dossier du jeu, cc:329 = adresse de base du module PE). Ici c'est l'objet fenêtre natif dont on lit X/Y aux offsets +0x1C/+0x20. → `desc_wnd_bytes`.

## Renommages prioritaires

### 1. `eb` -> `basic_info`

- **Où** : moonlight_ui.cc:533, 980, 1854, 2342, 2498 (déclarations)
- **Usages** : 86 occurrences du token `eb` dans moonlight_ui.cc (grep -w), 5 déclarations distinctes
- **Pourquoi** : Initiales « exp bar » d'un plugin qui gère aujourd'hui les barres HP/SP/EXP, le portrait et les étiquettes (BasicInfoTweaks). Aucun lecteur ne peut le deviner. Renommage 100 % mécanique, le compilateur attrape la moindre omission — c'est le meilleur rapport bénéfice/risque du fichier. Bonus : en renommant, on tombe sur le gate mort de la ligne 2498.

### 2. `iv, stg, sb, tt, mi, si, qt, st, en, cs, cse, sh, nd, lp, sb2, tt2` -> `inventory_viewer, storage_tweaks, skill_bar, trade_tweaks, menu_icons, status_icons, quest_tracker, settings_tweaks, entity_names, cashshop, character_sheet, shop_tweaks, npc_dialog, login_parade (supprimer sb2/tt2)`

- **Où** : moonlight_ui.cc:444-447, 618, 721, 741, 764-785, 830-833, 841, 867, 885, 912, 1132-1348, 2159-2264, 2940
- **Usages** : ~300 sites cumulés (en 54, stg 46, sb 45, iv 33, idt 28, cse 18, mi 18, si 15, …)
- **Pourquoi** : Famille saturée : plusieurs paires ne diffèrent que d'une lettre (st/stg, cs/cse), le code a déjà dû inventer `sb2`/`tt2` (:832-833) faute de nom libre, et `st` désigne un `const ImGuiStyle&` en :2302 puis un `SettingsTweaks*` en :2940. Un nom de plugin complet supprime la classe entière de collisions.

### 3. `p` -> `imm_ptr (:394) / key_prefix (:547, 576, 797, 1083, 1114, 1360) / skin_preset (:671) / preset_node (:925) / bg_preset (:2553, 3516) / aloot_preset (:1500, 1513, 3201, 3277…)`

- **Où** : moonlight_ui.cc:394, 547, 576, 671, 797, 925, 1083, 1114, 1259, 1360, 1396, 1500, 1513, 2553, 2754, 3201, 3277, 3282, 3313, 3318, 3342, 3358, 3377
- **Usages** : 135 occurrences mot-entier dans le .cc
- **Pourquoi** : Au moins huit contenus différents sous une seule lettre, dont plusieurs cohabitent dans la même fonction. C'est le premier facteur de temps de lecture du fichier. À traiter fonction par fonction (jamais en remplacement global : `p` est une sous-chaîne partout).

### 4. `load_color / load_col (+ load_dps_col, load_sbcol)` -> `LoadArgbHexColor(node, key, out_rgba) — une seule fonction membre statique — et LoadImU32SkinColor(node, key, out_rgba) pour le cas ImGui`

- **Où** : moonlight_ui.cc:520 (load_dps_col), 541 (load_color), 647 (load_col), 809 (load_sbcol)
- **Usages** : 33 appels cumulés (load_col 15, load_sbcol 10, load_color 5, load_dps_col 3)
- **Pourquoi** : Deux décodeurs aux encodages OPPOSÉS (chaîne hex ARGB vs entier ImU32 ImGui, R et B inversés) portent des noms qui ne diffèrent que de trois lettres, dans la même fonction. Le jour où l'on copie un appel de l'un vers l'autre, la couleur devient fausse sans erreur de compilation. Les trois lambdas hex (520, 541, 809) sont par ailleurs strictement identiques et doivent fusionner.

### 5. `separate_, wings_, noks_, no_ask_, sell_stuff_, sell_item_, show_delay_, show_speed_, tri_inv_/tri_cart_/tri_storage_/tri_gstorage_, aloot_pognon_ (+ leurs kSetting*)` -> `separate_kills_enabled_, wings_enabled_, noks_mode_, no_ask_enabled_, sell_stuff_enabled_, sell_item_enabled_, show_attack_delay_enabled_, show_move_speed_enabled_, sort_mode_inventory_/cart_/storage_/guild_storage_, aloot_min_zeny_ (+ kSettingSeparateKills, kSettingNoksMode, kSettingAlootMinZenyDiv100, kSettingSortModeInventory…)`

- **Où** : moonlight_ui.h:244-263 (champs) et h:205-224 (kSetting*) ; usages cc:1558-1636 et cc:2990-3051
- **Usages** : ~66 sites cumulés sur 13 champs + 13 constantes
- **Pourquoi** : Ce sont des bascules SERVEUR au sens indevinable depuis le client, et deux d'entre elles mentent sur leur type : `noks_` est un énuméré 0-3 (h:256) et les `tri_*` sont des e_sort_mode 0-6 (h:260), pas des booléens. `aloot_pognon_` mélange argot et perte d'unité (zeny local, /100 sur le fil, h:248). Ces noms vivent dans le header, donc chaque lecture du .cc les traverse.

### 6. `p1, p2, p3, p4, p5, p6 et sso` -> `wparam, msg_id, item_data, arg4, arg5, arg6 ; sso -> nameid_msvc_string`

- **Où** : moonlight_ui.cc:134-136 (signature), usages :137-177 ; sso :143
- **Usages** : ~25 usages dans le hook ; 1 signature
- **Pourquoi** : Noms Ghidra promus en identifiants C++ (interdit par la convention du projet, cf. rag_connection.h:44 pour l'autre cas). `p2` est comparé à 0x18 (set item), 0x06 (close) et 0x22 (restore pos) : c'est un ID de message UI. `p3` est la struct d'item dont l'offset 0x2C porte le std::string du nameid. Le commentaire de tête (:98-108) documente déjà tout cela — il suffit de le déplacer dans les noms.

### 7. `off, count (×2), namelen, id, value, buf, total` -> `read_offset_bytes, preset_count / setting_count, name_len_bytes, setting_id, setting_value, packet, packet_len_bytes`

- **Où** : moonlight_ui.cc:1496, 1498, 1503, 1532, 1542-1543 (parsing) ; :1667, 1711-1713 (émission)
- **Usages** : ~20 sites, tous dans OnRecvPacket / SendSetting / SendPresetCmd
- **Pourquoi** : Ce sont les SEULES variables du fichier dont un mauvais nom a un coût sécurité : elles pilotent les gardes de bornes :1499, :1505 et :1536 sur des paquets venus du réseau. `count` vaut « nombre de presets » en :1496 et « nombre de réglages » en :1532, à 36 lignes d'écart. Un lecteur qui confond les deux relit mal la garde.

### 8. `c` -> `skin (:71) / out_rgba (paramètres de lambdas :520, 541, 647, 809) / icons_cfg (:842, 1152) / quest_cfg (:868, 1176) / card_idx (:956, 1425) ; garder r/g/b/a explicites en :425-428`

- **Où** : moonlight_ui.cc:71, 425-428, 520, 541, 647, 809, 842, 868, 956, 1152, 1176, 1425
- **Usages** : 124 occurrences mot-entier (inclut les canaux de ArgbFromPicker/PickerFromArgb)
- **Pourquoi** : Six contenus sans rapport, dont un tampon de couleur et un index de carte. Le cas :425-428 est le plus insidieux : `c` y est le tableau RGBA d'entrée pendant que `g` est le canal vert — deux conventions de nommage superposées sur quatre lignes.

### 9. `g` -> `chat_bg_group (:369, 385, 393, 401, 467, 1011, 2537) / main_chat_bg (:3509) / postfx_cfg (:886, 1195) / green (:426)`

- **Où** : moonlight_ui.cc:369, 385, 393, 401-421, 426, 467, 886, 1011, 1195, 2537, 3509
- **Usages** : 75 occurrences mot-entier
- **Pourquoi** : Trois types sans rapport (groupe de fond de chat, config de post-traitement D3D9, canal vert), dont deux dans la même fonction LoadSettings (:467 puis :886). `g` est aussi le paramètre de ApplyChatBg et PatchChatBgObjects, donc le mauvais nom se propage à l'API interne.

### 10. `s_iface_nav (+ littéraux 0..10)` -> `supprimer l'alias et écrire `iface_nav_` directement ; remplacer les comparaisons `== 2` par `== kIfaceChat` (enum IfaceSection déjà défini)`

- **Où** : moonlight_ui.cc:2285 (déclaration), comparaisons :2316 puis tout le bloc jusqu'à ~:2900
- **Usages** : 14 occurrences de s_iface_nav ; ~11 comparaisons à des littéraux
- **Pourquoi** : Le préfixe `s_` annonce une statique de fonction : c'est en réalité une référence sur le membre `iface_nav_` (moonlight_ui.h:379) que `OpenInterfaceSection` pilote depuis une autre fenêtre. Un lecteur qui croit à une statique locale peut « nettoyer » l'alias et casser l'ouverture directe d'une section. L'enum `IfaceSection` (moonlight_ui.h:172-176) existe déjà et n'est jamais utilisé dans les comparaisons.

### 11. `instrs, heap, color, editing, vtable, field_off, chat_bg_found_` -> `argb_imm_ptrs, heap_targets, picker_rgba, picker_drag_in_progress, vtable_va, color_field_off, any_chat_bg_site_found_`

- **Où** : moonlight_ui.h:312 (ChatBgHeapTarget), h:318-321 (ChatBgGroup), h:325 ; usages cc:370-372, 386, 394, 398, 411-415, 472, 2538, 2606
- **Usages** : ~30 sites (instrs 7, heap 8, color/editing/vtable/field_off le reste)
- **Pourquoi** : `heap` n'est pas un tas mais une liste de couples (vtable attendue, offset du champ couleur) ; `instrs` est un vecteur de pointeurs ÉCRIVABLES vers des immédiats ARGB en .text (VirtualProtect posé :367) — la nature « on patche du code machine en place » doit être visible dans le nom. `chat_bg_found_` signifie « au moins un site », pas « tous » (:372).

### 12. `preset (lambda), preset_name (static), preset_name (const char*), preset_name_buf_, preview` -> `bar_size_button (:2397), s_ro_preset_name_input (:2749), active_preset_label (:3199), chat_bg_preset_name_buf_ (h:357), ro_preset_preview_label (:2730) / alootid_preset_preview_label (:3368)`

- **Où** : moonlight_ui.cc:2397, 2730, 2749, 3199, 3368 ; moonlight_ui.h:357
- **Usages** : preset 28, preset_name 12, preset_name_buf_ 13
- **Pourquoi** : Trois familles de presets sans rapport (couleurs de chat, skin RO, autolootid) partagent le même vocabulaire nu. Pire, la lambda `preset` de la ligne 2397 n'est pas un preset : c'est le bouton qui applique une taille w×h à toutes les barres (XS/S/M/L, :2408-2411). Qualifier la famille dans le nom supprime l'ambiguïté d'un coup.

### 13. `AlootPreset::no + SendPresetCmd(uint8_t cmd, uint8_t no, …) + littéraux 2/3/4/5/6` -> `slot_no (champ) ; SendPresetCmd(AlootPresetCmd command, uint8_t preset_no, const char* preset_name) avec enum kPresetSave=2, kPresetLoad=3, kPresetDelete=4, kPresetAutoload=5, kPresetRename=6`

- **Où** : moonlight_ui.h:269, h:278 ; cc:1710 (signature), appels cc:3287, 3335, 3338, 3349, 3391, 3394
- **Usages** : 8 usages de SendPresetCmd ; `no` lu dans ~20 sites du bloc autolootid
- **Pourquoi** : `no` se lit comme une négation alors que c'est un numéro d'emplacement serveur (0 = aucun). Et les six appels passent un code d'opération nu : `SendPresetCmd(4, named_preset->no)` (:3338) supprime un preset sans que rien dans la ligne ne le dise. Un enum rend l'erreur de code impossible à écrire.

### 14. `hex` -> `argb_hex (nom stable partout)`

- **Où** : moonlight_ui.cc:468, 480, 521, 542, 597, 598, 810, 1012
- **Usages** : 23 occurrences mot-entier
- **Pourquoi** : Redéclaré pour sept couleurs différentes (chat, sol du SPR Lab, DPS, barres, grille, skillbar, groupes de chat) sans jamais dire quel encodage ni quelle couleur. Le mettre au format `argb_hex` applique la R2 et fait apparaître d'un coup d'œil les endroits où l'on lit du ImU32 au lieu de l'ARGB.

### 15. `Text, Spacing, Separator, SameLine, Indent, Unindent, OpenPopup, BeginPopup, Tooltip, IsHovered, ColorPicker, GrayText, RedText, TextWrapped, TextUnformatted, CollapsingHeader, PushItemWidth, PopItemWidth, BulletWrapped, SeparatorText` -> `les enfermer dans un `namespace mui { … }` SANS les renommer, et ajouter `using namespace mui;` en tête des .cc concernés`

- **Où** : moonlight_ui.h:70-141
- **Usages** : plusieurs centaines de sites d'appel (Tooltip 95, HelpMarker 119, ColorPicker 28, WheelSliderInt 29, WheelSliderFloat 17…)
- **Pourquoi** : Une vingtaine d'enveloppes triviales sont déclarées dans le namespace GLOBAL d'un header inclus par de nombreux plugins — `Text`, `Separator`, `SameLine` y sont des noms extrêmement génériques. Un namespace règle le problème en un seul commit sans toucher aux centaines de sites d'appel, là où un renommage un par un coûterait des centaines de lignes de diff pour zéro gain sémantique.

## Méthode de renommage

**Principe** — sans test automatisé, le filet est triple : (1) le compilateur, qui attrape 100 % des renommages de membres, locales et paramètres ; (2) bourgeon_settings.yaml, qui doit rester octet pour octet identique après un lot de renommage pur ; (3) une passe d'yeux en jeu, section par section. Tout le protocole ci-dessous sert à ne jamais dépendre du seul point (3).

**Étape 0 — sortir les deux correctifs du lot de renommage (2 commits, avant tout le reste).** (a) moonlight_ui.cc:2498 : le gate `if (auto* eb = …basic_info())` est mort, il masque toute la section Chat quand BasicInfoTweaks est absent — soit on supprime la condition en gardant le corps, soit on la justifie par un commentaire. (b) moonlight_ui.cc:520/541/809 : fusionner les trois lambdas hex identiques en une seule fonction membre statique, ce qui fait disparaître la paire dangereuse load_color/load_col avant même de la renommer. Ces deux commits changent le COMPORTEMENT : ils ne doivent jamais cohabiter avec du renommage dans le même diff, sinon plus rien n'est isolable.

**Étape 1 — ordonner les lots par portée croissante**, du moins risqué au plus risqué :
- **Lot A (locales et lambdas d'une seule fonction)** : `hex`, `off/count/id/value/namelen`, `p1..p6`+`sso`, `preset`(:2397), `hdr`(:3197), `preview`, `can_add`, `found`. Portée close, zéro impact sur les autres fichiers.
- **Lot B (lettres uniques recyclées : p, c, g, d, e, b)** : le seul lot où un remplacement global est interdit. Procéder FONCTION par FONCTION, bornes de lignes explicites, un commit par fonction (LoadSettings et SaveSettings méritent chacune 2 ou 3 commits vu leur taille).
- **Lot C (alias de plugins frères)** : `eb`, puis `iv/stg/sb/tt/mi/si/qt/st/en/cs/cse/sh/nd/lp`, avec suppression de `sb2`/`tt2` (:832-833) qui n'ont plus de raison d'être une fois les noms complets. Purement mécanique, gros volume de diff mais risque nul.
- **Lot D (champs privés de MoonlightUi)** : bascules serveur, `instrs`/`heap`, `preset_name_buf_`, `in_gonryun_`, `apply_collapse_`/`iface_jump_` → `pending_*`, `AlootPreset::no`. Le compilateur couvre tout ; la seule vraie vigilance est la R15 (clés yaml).
- **Lot E (surface publique du header, en dernier)** : `AlignGrid::size`/`cell()` (lus par BasicInfoTweaks, MenuIconTweaks, quest_tracker_tweaks.cc, spr_effect_lab.cc), `IsHovered`, `ColorPicker`, la mise en namespace des enveloppes ImGui, `kSettingStaff`. Header très inclus → rebuild `--clean-first` obligatoire pour ce lot (un incrémental produit des ODR croisés et un crash au lancement).

**Étape 2 — protocole d'un lot.** Un lot = un commit = une famille de noms, et jamais plus de ~150 lignes de diff. Message en français avec préfixe conventional-commit, directement sur master : `refactor(moonlight-ui): eb -> basic_info (alias du plugin BasicInfoTweaks)`. Avant chaque renommage, vérifier que le nom cible est libre dans le fichier (`grep -won "<nouveau_nom>" moonlight_ui.cc`) : le seul échec silencieux possible est de renommer `p` en `preset` dans une portée où `preset` existe déjà (:2397, :3199) — ça compile, et ça change de sens. Toujours renommer en mot-entier, jamais par sous-chaîne.

**Étape 3 — garde-fou anti-régression, à passer sur CHAQUE lot.** Un renommage pur ne touche aucun littéral : après édition, `git diff -U0 -- src/plugins/moonlight_ui.cc | grep '^[+-].*"'` doit ne rien renvoyer. La moindre ligne qui sort = une clé yaml (`ui["expbar_..."]`, `yaml_key`), un ID ImGui (`"##preset_name"`, `"##sw"`) ou un libellé UI a bougé — à annuler immédiatement : une clé yaml renommée efface les réglages de tous les joueurs, un ID ImGui renommé casse la persistance de fenêtre (ce projet s'est déjà fait mordre par une collision d'ID de combo).

**Étape 4 — vérification.** Compilation : `cmake --build "d:/Mes documents/GitHub/Bourgeon/build" --config Release` (avec `--clean-first` dès que moonlight_ui.h est touché, c'est-à-dire pour les lots D et E). Puis, en jeu, le test de non-régression le plus rentable, qui remplace les tests absents : copier bourgeon_settings.yaml avant le lot, ouvrir le panneau touché, bouger un réglage puis le remettre (ce qui déclenche SaveSettings), et `diff` les deux fichiers — un renommage correct rend un diff vide. Ajouter une observation ciblée par lot : lot B → ouvrir chaque section d'« Interface de jeu » et vérifier que les couleurs (chat, barres, grille, skillbar) sont bien celles d'avant, c'est le lot où un mélange ARGB/ImU32 se verrait ; lot C → vérifier que chaque section affiche bien son plugin ; lot E → vérifier que les autres plugins qui lisent `grid_` snappent toujours.

**Étape 5 — ce qu'il ne faut PAS faire.** Ne pas renommer les enveloppes ImGui une par une (des centaines de sites pour zéro gain) : un namespace + `using` règle tout en un commit. Ne pas commencer par le lot E « parce que c'est dans le header » : c'est celui qui exige le rebuild complet et qui touche cinq autres fichiers. Ne pas fusionner deux lots « parce qu'ils sont petits » : dès qu'un diff mélange deux familles, l'observation en jeu ne dit plus laquelle a régressé, et c'est précisément le service que rendent les lots.

**Bonus gratuit** — trois duplications, si elles sont factorisées AVANT le lot B, suppriment la moitié des noms à corriger : le scan « cet item est-il dans aloot_ids_ ? » réécrit à la main en :1644, :3181, :3461 alors que `IsAlootId` (:1676) existe ; la pastille de preset de chat dupliquée mot pour mot entre :2552-2573 et :3515-3534 ; et les trois lambdas de couleur hex déjà citées. Factoriser d'abord, renommer ensuite — l'inverse fait renommer deux fois les mêmes choses.
