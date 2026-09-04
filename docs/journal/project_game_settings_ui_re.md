# Game Settings (Options) : RE et portage ImGui

> Journal du chantier. La fiche de mémoire `project_game_settings_ui_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

RE of the in-game **"Game Settings"** menu (the Options window). Tabs: Basic / Effects / Controls / Graphics / Other. Documented + renamed + commented in Ghidra on 2026-06-29. Two layers: the **CUIGameSettingsUI** window (UI) and the **OptionInfo** value registry (storage, Lua-backed).

🔴 **`docs/game_option_re.md` (2026-08-12) SUPERSÈDE ce fichier** : RE complète des TROIS fenêtres
(menu Échap, Game Settings, Shortcut Settings) + blueprint ImGui, avec vérifs live x32dbg.
**Trois affirmations d'ici sont FAUSSES** et corrigées là-bas :
- les libellés d'onglet ne sont PAS les chaînes CP949 `DAT_010460c4..f4` (elles restent coréennes en
  mémoire vive, vérifié) : ce sont les **msgstringtable 4142-4145 + 4217** (« Other » n'est pas contigu !) ;
- l'enregistrement de `OptionTbl` fait **100 (0x64) octets**, pas 0x19 ; champs ID/Tab/Type/Title/Tooltip/
  TipBoxID/Description/Default/MSGs[1..2], et le `Default` est **XOR-inversé** pour les ID 0x6A/0x6B/0x6C/
  0xE7/0x10D ;
- ~~le fichier Lua est `GameSettingsUI.lub`~~ — **FAUX À SON TOUR, corrigé le 2026-08-14** : c'est bien
  **`LuaFiles514\Lua Files\OptionInfo\GameSettings.lub`**. `GameSettingsUI.lub` n'est que le **titre des
  MessageBox d'erreur** du chargeur. Correction d'une correction : ne pas déduire un nom de fichier du
  titre d'une boîte de dialogue.
Ajouts : hauteur RÉELLE du menu Échap = **139** (pas 143 : OnCreate écrase MakeWindow) ; id du menu
Échap = **155**, lu à `+0x2C` ; [Reset] = `0x009EC670`, [Apply] = `0x009E6AD0` ; les 3 onglets data-driven
sont des **listes VIRTUALISÉES** (aucun contrôle par option). Cf. [[reference_hotkey_settings_window_re]].

## ✅ PORTÉE EN IMGUI (2026-08-14) — `features/windows/game_settings.{h,cc}` + `ragnarok/game_settings.{h,cc}`

Trois faits qui ont débloqué le portage, tous au §3.5.x de `docs/game_option_re.md` :

- **le vecteur `OptionTbl` est dans le MANAGER** (`mgr+0x0C..0x10`, `CGameSettingsMgr` @ `0x0131EE7C`),
  pas dans la session — piège de lecture, le décompilé ressemble à du code de session ;
- **il est rempli à l'ENTRÉE EN JEU** par `CSession_ctor` (0x00D578A2), PAS à l'ouverture de la fenêtre.
  C'est ce qui rend un panneau de remplacement possible : les données existent sans la native. À vérifier
  AVANT de porter toute fenêtre pilotée par données — l'inverse était l'hypothèse naturelle ;
- **écrire = `CGameSettingsMgr::SetOption` (0x0068DFD0)**, jamais le setter brut : c'est le HANDLER de
  l'option, trouvé par le manager, qui applique l'effet. Son 3ᵉ paramètre `annonce` distingue les deux
  chemins du client — 0 = la case à cocher de la fenêtre, 1 = la commande slash (qui écrit au chat).

Clé yaml `gamesettings_imgui`, ON par défaut, hors du groupe « Interface moderne » — comme
[[reference_hotkey_settings_window_re]].

### 🔴 Écrire une option ABSENTE de la table des drapeaux (bordure d'emblème 0xF3)

`GameSettings_SetFlagRaw` (0x0068FD50) ne **met à jour** que des clés existantes ; sur une clé
absente elle sort **sans rien écrire et sans se plaindre**. La bordure d'emblème (`0xF3`) n'est ni
dans `OptionTbl` ni dans le `CmdOnOffList` de `SaveData\OptionInfo.lua` ⇒ **inerte jusque dans la
fenêtre NATIVE**. (`0xA5`, la notification de connexion, EST dans le CmdOnOffList : elle, va bien.)

✅ La bonne porte pour insérer : `GameSettingsFlagMap_GetOrInsert` (**0x0068CDF0**, `__thiscall`) sur
la table `0x012515FC` — `out[5]` = {nœud*, créé?}, clé en `node+8`, **valeur en `node+12`**. Aucun
nom à résoudre. `gamesettings::SetRawFlag` **relit après écriture** et n'insère que si rien n'a bougé.

### 🔴🔴 LE CADRE D'EMBLÈME EST UNE FONCTION DE **WoE** — enquête close 2026-08-15

```
cadre = ( GameSession_IsAgitZone() == 1 )  ET  ( drapeau /frame )
```

`GameMode_BuildActorNameLabel` (**0x00C6D720**) calcule cette conjonction et la passe à
`UIActorNameLabel_SetEmblem` (0x0082DCC0), qui l'écrit en `this[0xBD]` ; `sub_825510` teste
`cmp byte [edi+0BDh], 1` avant de blitter le bitmap. Le même champ garde
`Guild_DrawEmblemOnPartyHUD` (0x00825160).

**`GameSession_IsAgitZone`** (ex-`GetField4c`, 0x00D8EC00) = `*(CGameMode+0xCC)+0x4C`, posé à 1 par
**`ZC_NOTIFY_MAPPROPERTY` = 3 = `MAPPROPERTY_AGITZONE`** (client 0x00CA6426 ; rAthena envoie 3 quand
`mapdata_flag_gvg(map)`, clif.cpp:14669). La même branche force `TT_MIN_EFFECT_ON_OFF` — effets
minimaux + mode d'affichage, la signature d'une carte de siège.
⇒ **Hors carte GvG le cadre NE PEUT PAS s'afficher.** Vérifié en jeu : dans un château, il est là.

⚠ La texture n'a jamais manqué (`data\texture\유저인터페이스\emblem_frame.bmp`, 28×28 Bgr24), et
`/frame` a toujours fonctionné — son nom vit dans la **table des messages**
(`MSI_DRAW_EMBLEM_FRAME_ONOFF` = 3532), PAS comme littéral de l'exe : une recherche d'octets ne le
trouve pas, et j'en avais conclu à tort son absence. Cf. [[feedback_msgstring_no_msg_fallback]].

⚠ Persistance : **le client n'en a AUCUNE** (ni `/frame` dans `CmdOnOffList`, ni la chaîne
« Emblem Frame » dans l'exe — la ligne du `OptionInfo.lua` est un fossile). Bourgeon la garde dans
`bourgeon_settings.yaml` (clé `emblem_frame`) et la réinjecte à l'entrée en jeu.

⛔ **Ne PAS forcer `IsAgitZone`** : ce champ dit au client sur quel type de carte il est, et gouverne
aussi les étiquettes de nom, le clic sur les acteurs et le survol du curseur.

📌 **Trois conclusions fausses avant la bonne**, toutes par affirmation d'absence sans mesure :
« /frame n'existe pas », « la texture manque », « le peintre est mort ». Cf.
[[feedback_re_method]] et [[feedback_re_method]].

## ✅ ONGLET BASIQUE COMPLET (2026-08-14) — skin, RODEX, priorité (doc §3.9)

Les trois derniers groupes n'avaient **rien en commun** : ni magasin, ni chemin d'écriture, ni même
la question de savoir qui décide. Chacun n'écrase que 3 slots de `CUIGroup` : `[15] OnCreate`,
`[16] UpdateTexts`, `[54] Reset`, `[55] Refresh`.

- 🔴 **Priorité** = `SetPriorityClass` sur le processus, rien d'autre (`0x009EF070`). Global
  `dwPriorityClass` @ `0x0160232C` ; 0x80 HIGH / 0x20 NORMAL / 0x40 IDLE (« Low »). **Deux pièges** :
  ça **ne vaut que fenêtre au premier plan** — `0xDB74B0` force IDLE **sans condition** à la perte du
  focus — et c'est **persisté** par `OptionInfoList["PriorityClass"]`, pas par ce chemin-là.
- ⛔ **RODEX = MORT SUR MOONLIGHT, groupe NON REPRIS** (décidé le 2026-08-14). Le client envoie
  **CZ 0x0B93** (12 o. : `{u16 op; u16 pad; i32 type=1; i32 val}`) et n'écrit jamais son drapeau
  (`0x01602430`), qui n'arrive que par **ZC 0x0B94 / 0x0B95**. Or `clif_packetdb.hpp` **ne mappe aucun
  `0x0b9x`** : le paquet part dans le vide, **la fenêtre native est inerte elle aussi**. 🔴 Et le mapper
  naïvement serait PIRE : `type = 1` vaut `CONFIG_CALL` chez rAthena (`disable_call`) — il n'existe
  aucun type RODEX. Case retirée + accesseurs supprimés (code mort = offsets non vérifiés). Tout est
  au §3.9 pour le refaire si le serveur gagne le support.
- 🔴 **Skin** : mgr `0x011FE3A8`, index courant `0x011FE3C4` (−1 = aucun), noms
  `0x011FE3D4/D8`, `GetName 0x007A6F10`, `SetSkin 0x007A7F70`. **`SetSkin` PURGE TOUTES LES TEXTURES
  .bmp du client** ⇒ jeter NOS deux caches en même temps (`ro::InvalidateGameTextures` +
  `ro::InvalidateSkinTextures`), **au tick** : commande native ET libération de textures que la frame
  dessine encore. Cf. [[feedback_imgui_pitfalls]],
  [[feedback_imgui_pitfalls]].

🔴 **`ETC` VAUT 4, PAS 3** — et `GRAPHIC` vaut 3, un onglet SANS ligne dans la table (page câblée en
dur). Le chargeur pousse ces constantes comme des **doubles** : `0x00FD4420`=1 (EFFECT),
`0x0100A0A8`=2 (CONTROL), `0x01006BE8`=4 (ETC). Supposer 1/2/3 rendait l'onglet « Divers » **VIDE**
alors que ses 40 lignes s'affichaient dans la vue « Tout » — le symptôme qui dit « la constante de
filtre est fausse ». ⚠ Corollaire : les onglets PROPRES à un panneau de remplacement doivent sortir
de cette numérotation (négatifs) — notre pseudo-onglet « Basique » valait 4 et est entré en collision.

## ✅ ONGLET GRAPHICS PORTÉ — LA NATIVE N'EST PLUS JAMAIS OUVERTE (2026-08-14, doc §3.10)

🔴 **Le [Apply] graphique ne fait AUCUN reset de device — ET NE RELANCE RIEN NON PLUS**
(corrigé le 2026-08-15 ; ce fichier affirmait l'inverse). `GameSettingsUI_GraphicsPage_Apply`
(`0x009EDB30`) écrit la config, pose `g_RestartRequested` (`0x01602A8C`), déconnecte, `SendMsg(2)`.
Mais le drapeau **ouvre une page web** — voir [[feedback_restart_flag_opens_web_page]]. Notre
panneau écrit la config, appelle `OptionInfo_SaveToFile` (`0x00D78970`, `this` = session
`0x015FA3C0`), et annonce « au prochain démarrage » ; « quitter » est un bouton séparé.
Les six groupes se séparent en deux familles :

- ⚡ **à chaud** : `g_cfg_SpriteDetailLevel` (`0x01602630`), `g_cfg_TextureDetailLevel` (`0x01602634`
  → `g_TextureDownscaleFactor` `0x0122B3D8` = 4/2/1), `g_cfg_Trilinear` (`0x01602638`). ⚠ Les deux
  derniers doivent **vider le cache de SpriteTexFactory** (`sub_568B30(&g_SpriteTexFactoryCache)`),
  sinon les textures chargées gardent l'ancien réglage.
- 🔁 **lus au DÉMARRAGE seulement** : `g_cfg_RenderSystem` (`0x01602640`, **1 = DX7, 2 = DX9**),
  adaptateur (GUID), `g_cfg_RenderWidth/Height/BitsPerPixel` (`0x01602614/18/1C`),
  `g_cfg_FullscreenMode` (`0x01602610`). ⇒ **brouillon local** jusqu'à ce que le joueur accepte.

📌 Énumérer via le CLIENT, jamais nous-mêmes (sa liste est celle qu'il acceptera au démarrage) :
`Adapter_EnumerateList` `0x00560FB0` (104 o.), `DisplayMode_EnumerateList` `0x00561550` (**36 o.**
`{w,h,bpp,string}`), `RenderConfig_GetCurrentAdapter` `0x005610B0`. ⚠ L'énumération DX9 crée un
`IDirect3D9Ex` → **au tick, jamais par frame**. ⚠ Libérer leurs vecteurs comme le client : détruire
chaque `std::string`, puis `operator_delete` en tenant compte du bloc sur-alloué ≥ 4096 o.

🔴 **L'ADAPTATEUR COURANT NE SE LIT PAS** : `RenderConfig_GetCurrentAdapter` met son champ index
(`+0x04`) à zéro et **ne le remplit jamais** — sa comparaison n'en a pas besoin. Le lire rend
toujours 0. Faire comme `D3D9_ResolveAdapterOrdinalAndCreateDevice` (`0x00565230`) : énumérer,
comparer avec `AdapterRecord_Equals` (`0x00560D60`, `__thiscall`), prendre le `+0x04` du gagnant.
En DX9 la comparaison exige GUID **ET** `DeviceName` : deux sorties d'une même carte partagent le
GUID. ⚠ Mettre le résultat EN CACHE — il coûte une énumération.

🔴 **En FENÊTRÉ, le choix d'écran ne fait rien**, et c'est le client : `GameWindow_Create`
(`0x00DB6670`) place la fenêtre d'après `GetSystemMetrics(SM_CXSCREEN)` — l'écran PRINCIPAL — et
borne toute abscisse qui en sortirait. L'adaptateur n'entre jamais dans cette fonction ; seul le
plein écran le suit, via `CreateDeviceEx(ordinal)`. Le panneau doit l'annoncer.

🔴 Garde-fou : si l'adaptateur demandé n'est pas dans la liste, **n'écrire RIEN**. Une config
graphique à moitié valide est le seul échec dont on ne se relève pas depuis le jeu.

## Window: CUIGameSettingsUI (UIWindow id 0x271e)
- RTTI typedesc `.?AVCUIGameSettingsUI@@` @ 0x0123176c; COL @ 0x010cc530; **vtable `vtbl_CUIGameSettingsUI` @ 0x01047d7c** (multi-inheritance: 2nd vtable after embedded Bgm_Volume/Effect_Volume strings).
- Opened/found via `UIWindowMgr_FindWindow(g_UIWindowMgr=0x0131f4e8, 0x271e)` + `__RTDynamicCast` to CUIGameSettingsUI. (g_UIWindowMgr matches [[reference_ui_window_manager]].)
- UIWindow virtual convention (this client): **OnCreate = vtable+0x3c**, OnDraw base = slot1 (UIWindow_OnDraw_Base @ 0x00a245c0) → concrete paint via [vtable+0x98] (UIWindow_PaintDispatch 0x00a23340, generic — no custom paint override), slot0 = scalar-deleting dtor.
- `GameSettingsUI_dtor` @ 0x009eb410 (slot0). `GameSettingsUI_OnCreate` @ **0x009d5890** (slot15, +0x3c).

### CUIGameSettingsUI field map (from OnCreate)
- +0xd8 header/titlebar container (basic_interface\titlebar_left/mid/right, "Settings" caption, min btn)
- +0xdc page[Basic]   (GameSettingsUI_BasicPage_ctor 0x009e9890, vtable vtbl_GameSettingsUI_BasicPage 0x0104765c)
- +0xe0 page[Graphics](GameSettingsUI_GraphicsPage_ctor 0x009e9ad0) — UpdateApplyButtonState reads this page's combos
- +0xe4 std::vector<page*> = Effects/Controls/Other (GameSettingsUI_ListPage_ctor 0x009ea400 x3); +0xe8 end, +0xec cap
- +0xf0 tab-button strip; 5 buttons, CP949 labels DAT_010460c4..f4 = 기본/이펙트/컨트롤/그래픽/기타 = Basic/Effects/Controls/Graphics/Other. Each tab button's +0x66 = its target page ptr.
- +0xf4 [Reset] btn, +0xf8 [Apply] btn, +0xfc [close] btn

### Apply button logic
- `GameSettingsUI_UpdateApplyButtonState` @ 0x009ef3a0(window): enable [Apply] (window+0xf8, vfunc+0x38) iff Graphics page (window+0xe0) combos differ from live render cfg: SpriteMode(+0x88) vs DAT_01602630, TextureMode(+0x8c) vs DAT_01602634, Trilinear(+0x94) vs lpData_01602638.
- `GameSettingsUI_RefreshApplyButton` @ 0x009ef160: find win 0x271e + cast + call Update. Combo OnSelChange routes here.
- `GameSettingsUI_GraphicsCombo_OnSelChange` @ 0x009eced0: read selected list item → store into control (this+0x184/188/18c, label +0x190) → refresh apply btn.

### Basic tab rows — GameSettingsUI_BasicPage_OnCreate @ 0x009d5450
6 rows stacked y=0/0x24/0x60/0x84/0xc0/0xe4 (matches screenshot top→bottom):
- page+0x80 CUIGroupSkin            (GameSettingsUI_GroupSkin_ctor 0x009ea0d0) — Skin combo
- page+0x84 CUIGroupSound           (GameSettingsUI_GroupSound_ctor 0x009ea160) — Audio BGM/SFX sliders+Off (h=0x3c)
- page+0x88 CUIGroupEmblem          (GameSettingsUI_GroupEmblem_ctor 0x009e9b80) — Emblem Border radio
- page+0x8c CUIGroupRodexSpam       (GameSettingsUI_GroupRodexSpam_ctor 0x009ea020) — Mail/RODEX radio (h=0x3c)
- page+0x90 CUIGroupProcessPriority (GameSettingsUI_GroupProcessPriority_ctor 0x009e9f60) — Default Priority radio
- page+0x94 CUIGroupLogInOut        (GameSettingsUI_GroupLogInOut_ctor 0x009e9eb0) — Login Notification radio

### Graphics tab rows — GameSettingsUI_GraphicsPage_OnCreate @ 0x009d7610 (page vtable vtbl_GameSettingsUI_GraphicsPage 0x01047c98)
6 rows page+0x80..0x94 (y=0/0x20/0x60/0x80/0xa0/0xc0); SpriteMode/TextureMode/Trilinear anchors confirmed via UpdateApplyButtonState:
- +0x80 CUIGroupGraphicsAPI        (GameSettingsUI_GroupGraphicsAPI_ctor 0x009e9e10) — RenderSystem combo
- +0x84 CUIGroupGraphics           (GameSettingsUI_GroupGraphics_ctor 0x009e9ce0) — adapter+resolution combos (tall, alloc 0x1a8)
- +0x88 CUIGroupSpriteDetailLevel  (GameSettingsUI_GroupSpriteDetailLevel_ctor 0x009ea230) → DAT_01602630
- +0x8c CUIGroupTextureDetailLevel (GameSettingsUI_GroupTextureDetailLevel_ctor 0x009ea2c0) → DAT_01602634
- +0x90 CUIGroupFullscreen         (GameSettingsUI_GroupFullscreen_ctor 0x009e9c30)
- +0x94 CUIGroupTrilinear          (GameSettingsUI_GroupTrilinear_ctor 0x009ea350) → lpData_01602638

### Effects / Controls / Other tabs
Share the generic base CUIListPage (GameSettingsUI_ListPage_ctor 0x009ea400, vtable PTR_FUN_01047014) — populated data-driven (not fixed CUIGroup rows). Controls tab = keybinding list from OptionTbl/CmdInfo Lua (see TT_LoadGameSettingsLua below).

### Control/base classes (RTTI, GameSettingsUI namespace)
CUIOptionItem, CUIListPage, CUIListBoxEx<CUIOptionItem,TALKTYPE>, CUIComboItem, CUIComboBox<...>. Controls: CUIRadioButton, sliders, toggle checkboxes (GameSettingsUI\toggle_on/off.bmp), btn_optionapply_*.bmp. Bitmaps under data path `\GameSettingsUI\` (built at runtime → no static xref).

## OptionInfo value registry (storage layer, Lua-backed)
`this` = game session object; Lua state ptr at this+0x59b8. Values live in Lua global table `OptionInfoList` (file `SaveData\OptionInfo.lua`), accessed via Lua globals l_GetOptionValue / l_GetDefaultOptionValue.
- `OptionInfo_GetInt`    @ 0x00d81a30 (name,defInt) — workhorse
- `OptionInfo_GetFloat`  @ 0x00d81810 (name,defFloat) — view latitudes/distances
- `OptionInfo_GetString` @ 0x00d81af0 (SkinName, DX9DEVICENAME)
- `OptionInfo_GetBlob16` @ 0x00d81900 (DX9DEVICEID GUID)
- `OptionInfo_LoadAndApplyAll` @ 0x00d759f0 — loads OptionInfo.lua then applies EVERY setting to globals/session fields (full key→memory map inside the function comment: Bgm/Effect_Volume→sound mgr DAT_01253d0c; Outdoor/Indoor_View*→DAT_012291d0..dc; SkinName→DAT_011fe3ac; RENDERSYSTEM/DX9*/WIDTH/HEIGHT/BITPERPIXEL/SPRITEMODE/TEXTUREMODE→DAT_016026xx; many bool toggles→session+0xNNNN e.g. bShowMenuIcon+0x7ebc, LockMouse+0x5b50, DamageSkin+0x809c, PriorityClass+0x7f6c)
- `OptionInfo_SaveToFile` @ 0x00d78970 — writes SaveData\OptionInfo.lua (Lua SaveToFileCmdOnOffValueEx + Append* helpers)
- Append helpers: `OptionInfo_AppendIntEntry` 0x00d977a0, `OptionInfo_AppendFloatEntry` 0x00d976b0, `OptionInfo_AppendStringEntry` 0x00d97810, `OptionInfo_AppendBlobEntry` 0x00d97730 (format `OptionInfoList["%s"] = ...`)

## Keybinding/command table (separate from the options above)
`TT_LoadGameSettingsLua` @ 0x0068eca0 loads `LuaFiles514\Lua Files\OptionInfo\GameSettings.lub` table `OptionTbl` = the keyboard-shortcut command list (per entry: cmd idx, key code, modifier, Title/Tooltip/TipBoxID/Description; output stride 0x19). Error MessageBoxes title "GameSettingsUI.lub". Feeds the Controls tab.

## Position persistence (DONE 2026-06-29, SettingsTweaks)
The engine never saves window 0x271e's position. Bourgeon persists it: window pos = win+0x1c (x) / +0x20 (y); SetPos = vtable+0x10 = UIWindow_SetPos 0x00874af0 (writes +0x1c/+0x20 then relayout vtable+0x90). Msg handler = vtable+0x94 = FUN_008841d0. Yaml keys game_option_pos_x/y.
- Creation path (≠ UIEscOptionWnd): 0x271e is NOT made by the legacy UIWindowMgr_MakeWindow switch; it is registered into a generic window registry (DAT_0131ef08) by GameSettingsUI_RegisterFactory 0x0048c450 → FUN_00991b00(reg,0x271e) with create-fn GameSettingsUI_Create 0x009d2cc0 (=new(0x100)+GameSettingsUI_ctor 0x009e99a0). The ctor/OnCreate do NOT position the window; centring is done by the registry.
- Flicker-free restore (2026-06-30): same vtable-hook trick as UIEscOptionWnd — swap vtbl_CUIGameSettingsUI[+0x10] (SetPos, Hooked_GsSetPos) + [0] (dtor, Hooked_GsDtor) to substitute the saved pos on the FIRST SetPos of a fresh instance. BUT the registry's centring could be a DIRECT call to the shared UIWindow_SetPos (bypassing the vtable) — UNVERIFIED — so a one-frame OnTick restore-on-open is KEPT as a safety net (restores before the save branch can run, preventing the engine centre from overwriting the saved pos). The generalized hook lives in src/plugins/settings_tweaks.cc (shared SetPosRestore + PatchVtableSlot, per-window tokens g_esc_positioned/g_gs_positioned). Contrast UIEscOptionWnd: its centring-through-vtable is PROVEN (MakeWindow tail CALL edi), so it is save-only (no OnTick restore). TODO if wanted: RE the registry create/centre path to confirm indirect SetPos, then drop the GS safety net.

## Sibling window: UIEscOptionWnd (the ESC "Game Options" pop-up) — DONE 2026-06-29
The ESC menu (buttons: Character Select / game settings / Shortcut Configuration / Exit to Windows / Return to game). A SEPARATE native window from CUIGameSettingsUI above (the "game settings" button here opens 0x271e).
- RTTI typedesc `.?AVUIEscOptionWnd@@` @ 0x01240040; COL @ 0x010c50b4; **vtable `vtbl_UIEscOptionWnd` @ 0x010384a0** (slot0 dtor `UIEscOptionWnd_dtor` 0x008db420, slot1 OnDraw base 0x00a245c0, **+0x10 SetPos 0x00874af0** (same shared SetPos as 0x271e → pos win+0x1c/+0x20), +0x3c OnCreate `UIEscOptionWnd_OnCreate` 0x008e5410). ctor `UIEscOptionWnd_ctor` @ 0x008d71b0 (sets vtable + this[0x35]=0x11).
- **Has NO FindWindow switch id** (not in UIWindowMgr_FindWindow 0x00a47b90's switch). Created by UIWindowMgr_MakeWindow (ctor call @ 0x00a3f24d) which stores the instance into a dedicated slot **g_UIWindowMgr(0x0131f4e8)+0x408 = 0x0131f8f0** (null when the menu is closed), then SetSize 280x143 (0x118 x 0x8f).
- Open-centring flow (verified by disasm): UIEscOptionWnd_ctor does NOT set the window pos; OnCreate 0x008e5410 only positions the 7 child buttons (esc_01..esc_10, a/b/c.bmp states). The DEFAULT centre is applied in MakeWindow's shared tail @ 0x00a3b31d: `CALL 0x00a4ca40` (compute centred coords) then `CALL edi` where edi = instance `vtable[+0x10]` = `UIWindow_SetPos` (0x00874af0) → so the centring is the FIRST SetPos on a fresh instance and goes through the window's own vtable slot.
- Position persistence (Bourgeon, SettingsTweaks): **flicker-free** via a vtable hook. Restoring from OnTick (~100ms) was one frame late → the window was seen at centre before jumping. Fix: swap `vtbl_UIEscOptionWnd[+0x10]` (0x010384a0+0x10, stock 0x00874af0) for `Hooked_EscSetPos` (installed once, lazily, VirtualProtect, guarded that the slot still == 0x00874af0). The wrapper substitutes the SAVED coords on the FIRST SetPos of a fresh instance (armed by instance-pointer newness `self != g_esc_positioned`), so the very first frame draws in place. The token is re-armed (→null) RACE-FREE by a second vtable hook on the dtor slot `vtbl_UIEscOptionWnd[0]` (Hooked_EscDtor, stock 0x008db420): it clears the token the instant the window is destroyed, before the heap block can be reused — this closes the only defect an adversarial review found (fast close+reopen at the same address). OnTick's close-edge reset is kept as a backup. OnTick now ONLY saves drags (poll win+0x1c/0x20, throttle 200ms) + flushes on close. Reads `*(void**)0x0131f8f0` (vtable-guarded == 0x010384a0). Yaml keys `esc_option_pos_x/y`. NB: the older 0x271e block still uses the OnTick FindWindow-restore (has the same 1-frame flicker; same vtable-hook trick on vtbl_CUIGameSettingsUI[+0x10] would fix it if wanted).

## TODO (not yet done)
- Effects/Other tab content (data-driven CUIListBoxEx population from Lua); Controls = keybind list.
- MakeWindow factory case for id 0x271e + the menu/command that toggles the window open.
- [Reset]/[Apply]/[close] button callback bodies (PTR_FUN_01046934/01046950, FUN_009cf850).
- Legacy windows still present in RTTI: UIFriendOptionWnd, UIBattleMsgOptionWnd.

Related: [[reference_ui_window_manager]], [[project_basic_info_menu_icons]], [[reference_data_folder_cp949_encoding]].
