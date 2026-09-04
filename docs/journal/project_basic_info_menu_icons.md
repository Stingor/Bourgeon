# Basic Info et grille d'icônes de menu — RE et réimplémentation

> Journal du chantier. La fiche de mémoire `project_basic_info_menu_icons` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

20250716 client RE of the "Basic Info" character window and the menu-icon grid below it. All addresses absolute (no ASLR rebase; Ghidra == live). Documented + renamed in Ghidra.

## UIBasicInfoWnd (the "Basic Info" window)
- Window-mgr **ID 0**; singleton ptr at `g_UIWindowMgr(0x0131f4e8)+0x1dc`. MakeWindow case @ `0x00a394d6` (`operator new(0x118)` = 280-byte object).
- RTTI `.?AVUIBasicInfoWnd@@` (typedesc 0x0124084c, COL 0x010c6564). **vtable `vtbl_UIBasicInfoWnd` @ 0x0103e35c** (slot1=UIWindow_OnDraw_Base, DrawContent=slot20).
- ctor `UIBasicInfoWnd_ctor` @ `0x0095d060`. Two size-sets chosen by `JobId_Is4thClass(charJob)` (@0x00d734f0, true for job 0x109C-0x10E9): normal {extraY=13, collapsed h=55(0x37), expanded h=134(0x86)} else 4th-job {28,71,149}. Fields: +0x14 width, +0x18 cur height, +0xb4 collapsed h, +0xb8 expanded h, +0xf8 extra-Y.
- `UIBasicInfoWnd_DrawContent` @ `0x0095e620` dispatches by current height: expanded(+0xb8)=full vertical panel (title "Basic Info", HP/SP, Base Lv.%d, Job Lv.%d, Zeny:%s, Weight); collapsed(+0xb4)=one-line "Lv/class/Lv/Exp%" bar. 4th-job uses `UIBasicInfoWnd_DrawExpanded_4thJob`@0x0095d300 / `..._DrawCollapsed_4thJob`@0x0095da30. Globals: HP cur/max=DAT_015ff908/90c, SP=910/914, zeny=015fba90, weight max/cur=015fba9c/baa0. Localized variants ("Peso"/"Classe"/"Base.") when DAT_0159b810==0xc.
- `UIBasicInfoWnd_OnMove_DockMenuIcons` @ `0x0095f240`: FindWindow(0x133) and re-docks the menu-icon grid to (x, y+height). `..._OnMouseMove_WeightTooltip`@0x0095f190 (shows "Weight %d%%"). `..._OnMove`@0x0095f170, `..._OnResize`@0x0095f140.
- LIVE-VALIDATED: object @0x14620b80 had vtable 0x0103e35c, width 0xDC, height 0x86(expanded), +0xb4=0x37, +0xb8=0x86 (Gunslinger = non-4th-job).

## UIMenuIconWnd (the menu-icon grid)
- Window-mgr **ID 0x133** (byte-table[0x133]=0xcb -> jumptbl[203]=0xa412bf; ctor call @0xa412ec). Docked under BasicInfo.
- RTTI `.?AVUIMenuIconWnd@@` (typedesc 0x0123e2d4, COL 0x010c076c). **vtable `vtbl_UIMenuIconWnd` @ 0x010281b0** (DrawContent slot20=0x00814150=`UIMenuIconWnd_RebuildNodes`).
- ctor `UIMenuIconWnd_ctor` @ `0x00812930`: icon map @+0xb4, 2nd/dup map @+0xbc, list @+0xc4; 3 header buttons @+0xcc/+0xd0/+0xd4.
- `UIMenuIconWnd_BuildIconList` @ `0x00812fb0` (vtbl slot15): 25 icons, bitmaps `\menu_icon\bt_<name>.bmp(/_press/_new)`, grid 5-per-row cell 42x43 (col=pos%5*0x2a+9, row=pos/5*0x2b+0x17), pos advances only for shown icons. Header btns bt_menu_*/bt_menu_new_*/bt_menu_close_* (close id 0xC9, menu id 0x1FE). Width 0xC9 collapsed / 0x1FE expanded per `g_MenuIconWnd_Collapsed`(0x0160227c).

### Icon name -> command id (set into UIMenuIcon+0x2c)
status=0xC0 equip=0xC3 item=0xC2 skill=0xC4 booking=0x17B party=0xC7 guild=0x175 battle=0x178 quest=0x169 map=0xDB navigation=0x1AE option=0xC1 bank=0x1CD rec=0x18F mail=0x1DC achievement=0x1D9 tip=0x1FF shop=0x200 keyboard=0x172 sns=0x206 attendance=0x21C adventurerAgency=0x220 repute=0x237 adventureguide=0x245 probability=0x24B. (status->"status_doram" for Doram race.) BuildIconList HIDES via switch break: 0x17B,0x18F,0x1FF,0x206,0x21C,0x220,0x237,0x245,0x24B,0x257.

### 🔴 Deux commandes du menu N'OUVRENT PAS de fenêtre : elles ouvrent le NAVIGATEUR
`UIMenuIconWnd_OnMsg` @0x00814a70 **case 587 (0x24B « Probability »)** : boîte modale msg 0x1021
(`MSI_ETC_PROBABILITY_WEB_CONNECT`) puis, si la réponse vaut **187**, `ShellExecuteA("open",
msg 0x1020 = MSI_ETC_PROBABILITY_URL)`. Aucune fenêtre. (La vraie fenêtre de probabilité est
`CUIProbabilityTable` 0x271C, ouverte depuis la desc item — [[reference_probability_window_re]].)
Même motif : **case 476** (Adventure Agency) passe par `FindWindow` avant d'ouvrir 0x107, et
`UIStylingShopWnd` / `UIExchangeShopWnd` ont chacun leur bouton « URL de probabilité »
(msg 0x101B / 0x101C). Sur Moonlight ces URL pointent déjà sur moonlight-destiny.fr.

### UIMenuIcon (single icon)
- **vtable `vtbl_UIMenuIcon` @ 0x010280d4**; cmd id @+0x2c (`UIControl_SetCommandId`@0x005aa6b0), tooltip str @+0x94. base ctor FUN_008172b0, state bmps via `UIBitmapButton_SetStateBitmap`@0x0082dac0.
- Click `UIMenuIcon_OnLButtonDown` @ `0x008271f0` is gated by TWO conditions — BOTH must hold or the click is a no-op (else-branch does cleanup only): (1) `*(char*)(icon+0xac)==0`; (2) icon is the mgr's active window: `*(mgr+0x19c)==icon` (getter `FUN_00a32e00`@0x00a32e00 returns `*(mgr+0x19c)`; mgr+0x19c=`0x0131f684`). In bounds it sets pressed(+0x30=1) then dispatches by path:
  - **PATH 1** (`icon+0x10 != 0` — ALL real menu icons take this; +0x10 = parent = the 0x133 window): `FUN_00a38b40(mgr, 3, parent+0x2c)` — a table getter `*(mgr+(action+id*8)*4+0xa34)` returning the registered handler, then tail-jumps to it. Routes the click for whichever icon is *active* (hence the active-window precondition). LIVE-checked icon 0xC0: +0x10=the 0x133 wnd, +0x2c=0xC0, +0xb0=0 (ctx null), +0xac toggles to 1 mid-frame.
  - **PATH 2** (`icon+0x10 == 0`, not used by these icons): `g_UICommandDispatcher(*0x0121333c)->vfunc[0x18](0, icon+0x2c, icon+0xb0, 0,0)` — the huge command processor @0x00c86740 (arg0=selector, jumptbl @0xc930f0 idx=sel-1).
- `UIMenuIcon_SetHelpTextByCmdId` @ `0x00814550` maps cmdId->help-msg id (status0xC0->0x69, equip0xC3->0x68, item0xC2->0x6A, skill0xC4->0x11B, party0xC7->0x67, ...). Hover tooltip via `UIMenuIcon_OnMouseEnter_Tooltip`@0x008274a0.

### Badge « nouveau » (le N rouge du courrier) — liste @wnd+0xC4
Le natif **ne peint PAS de pastille** par-dessus l'icône : `BuildIconList` crée une **SECONDE
UIMenuIcon** (bitmaps `bt_<name>_new.bmp` @0x10283D4 / `bt_<name>_new_press.bmp` @0x10283FC) posée
**6 px plus haut, même x** (row_y+17 au lieu de +23), rangée dans la 2e map **+0xBC**. Seules
**CINQ** commandes en ont une : status 0xC0, item 0xC2, skill 0xC4, achievement 0x1D9, mail 0x1DC.
- **La source de vérité = une `std::list<int>` à `wnd+0xC4`** (sentinelle ; taille `+0xC8` ; nœuds
  MSVC `{next, prev, valeur}` de 12 o) : une commande y figure ⇔ son badge est allumé.
  `RebuildNodes` cherche l'id dedans et fait `SetVisible(normale, !trouvé)` / `SetVisible(_new, trouvé)`
  (**vtbl_UIMenuIcon+212 = `sub_5DB6A0` = SetVisible** : pose `+0x28` = visible et `+0xAC` = `!visible`,
  la garde de clic).
- **Alimentation** : son propre `UIMenuIconWnd_OnMsg` **case 286** — `(this, 0, 6, 286, cmdId, 1|0, 0)` ;
  >0 = ajoute, <=0 = retire, puis relayout (vtbl+152). Extinction dans
  **`Recv_ZC_AckDeleteMail_0x09F6`** 0x00cfb6b0 quand `g_RodexMgr+0x18` (non-lus) retombe à 0.
  Ancien courrier : **`Recv_ZC_MailWindows_0x0260`** 0x00d09a70 — ⚠ le client fait l'**INVERSE** de la
  doc rAthena (`0 = open, 1 = close`) : `type == 1` ouvre la boîte 0x106 **et allume** le badge.
- Renommés dans l'IDB (+ commentaires) : `UIControl_SetVisible` 0x005db6a0 (vtbl+0xD4),
  `MenuIconWnd_IconMap_LowerBound` 0x00812850, `MenuIconWnd_BadgeList_Find` 0x008128a0,
  et les 4 gabarits de chemin `aFmtMenuIconBt[Press|New|NewPress]` @0x1028384/3A8/3D4/3FC.
- ✅ **Notre grille ImGui LIT cette liste** (`MenuIcons::RefreshBadges`, OnTick) au lieu de rejouer la
  règle métier : elle reste tenue à jour alors que `GridClear` ne vide que la liste de nœuds de RENDU
  (+0x4). Le bitmap `_new` remplace l'icône entière ⇒ fenêtre ImGui agrandie vers le haut du
  débordement **mesuré** (`nh - h`), `(ic.x, ic.y)` restant l'ancre de l'icône normale.

## Menu-icon visibility is controlled by a WARP patch (NewButtonVisibility)
The hidden icons are NOT a natural feature-gate — they are disabled by the WARP patcher (D:\Mes documents\GitHub\WARP0716\Scripts\Patches\NewButtonVisibility.qjs; wrappers `HideNewButtons`/`ShowNewButtons`). WARP finds `UINewBasicWnd::UINewBasicWnd` (= UIMenuIconWnd_BuildIconList @0x00812fb0) via the "status_doram" string, then patches a per-button visibility byte table.
- Dispatch @0x0081385d: `id-=0x178; if id>0xDF -> always create; else MOVZX al,[0x00814064+idx]; JMP [0x0081405c+al*4]`. Table `MenuIcon_VisibilityTable_WARP`@0x00814064 (224 bytes, idx=id-0x178): byte 1=visible (jmptbl[1]=0x00813879 create), byte 0=hidden (jmptbl[0]=0x00813d74 skip). `MenuIcon_VisibilityJumpTable`@0x0081405c.
- Only "new" buttons (id>=0x178) are toggleable; classic icons (status/item/equip/skill/party/guild/quest/map/option/keyboard, id<0x178) bypass the table and are always created.
- WARP button name->key->id map (NewButtonVis.Data + OverrideIDs): Battleground=battle 0x178, Booking 0x17B, Record=rec 0x18F, Navigation 0x1AE, Bank 0x1CD, Achievement 0x1D9, Rodex(Mail) 0x1DC, Tip 0x1FF, Shop 0x200, Twitter(SNS)=sns 0x206, Attendance 0x21C, AdventureAgency=adventurerAgency 0x220, Reputation=repute 0x237, "Adventure Guide"=adventureguide 0x245, Probability 0x24B.
- THIS exe (E:\Nouveau dossier\save_exe.yml -> Moonlight-Destiny.exe): $newHiddenButtons = Adventure Guide, Probability, AdventureAgency, Reputation, Attendance, Tip, Record; $newShownButtons = Battleground. LIVE table verified: hidden(0)= booking,rec,tip,sns,attendance,adventurerAgency,repute,adventureguide,probability (booking+sns were already off in stock; the other 7 hidden by WARP); Battleground force-shown.
- (EnableIcon.qjs/RestoreIcon/CustomIcon is unrelated: it swaps the Windows .exe app icon via RT_GROUP_ICON resource groups 119/114/123.)

## Triggering an icon command from the ImGui re-implementation (MenuIconTweaks plugin, src/plugins/menu_icons.cc)
Approach B recreates the icons as ImGui ImageButtons and routes clicks back to the native dispatch. To "click" an icon programmatically:
1. Find it: `FindWindow(0x133)` (`UIWindowMgr_FindWindow`@0x00a47b90 `__thiscall(mgr,id)`), then walk the icon map @`wnd+0xb4` (MSVC `_Tree`: sentinel ptr stored there; root=`*(sentinel+4)`; node key@+0x10, value/UIMenuIcon*@+0x14, left@+0, right@+8, `_Isnil`@+0xd).
2. Open BOTH OnLButtonDown gates for the call, then restore: `sa=*(mgr+0x19c); *(mgr+0x19c)=icon; sf=*(icon+0xac); *(icon+0xac)=0; OnLButtonDown(icon,1,1); *(icon+0xac)=sf; *(mgr+0x19c)=sa;`. Lets the native code run PATH 1 correctly.
- **Dispatch from `OnTick` (game UPDATE phase), NOT `OnRenderUI` (Present/EndScene hook).** `Bourgeon::OnTick` is called from `GameMode::OnUpdateHook` (the game's update hook) — genuinely NOT the render path (verified). Light windows (status/item/equip/skill/...) open fine from either; opening from Present is wrong context. Pattern: queue cmd on click in OnRenderUI (`pending_cmd_`), dispatch next `OnTick`.
- **cmd→action table = `FUN_00814a70`** (the path-1 menu-icon command handler, reached via OnLButtonDown→FUN_00a38b40). Each `case <cmd>:` either `FUN_00812e60(<winId>)` (open window by id; `FUN_00812e60` = "if not FindWindow-ish open, MakeWindow") or a dispatcher selector. Notables: **map 0xDB → window 0x8c (world map)**; **cash shop 0x200 → dispatcher selector 0x143** (not a window — why it "did nothing" before deferral); option 0xC1→0x9b, item 0xC2→8, equip 0xC3→0xa, skill 0xC4→0x25, status 0xC0→0xb, quest 0x169→0x2718.
- **World map crash is INTERMITTENT** and lives in the worldmap window's OWN init (`FUN_005f4700`→`FUN_005f3de0(this,-1)` → dispatcher **selector 9** @0x00c8bec3 = `LEA[disp+0x68]` → `strlen` crash @0x005f3e96 with the ptr NULL). Same code the NATIVE button runs. Deferral to OnTick reduces it but does not fully eliminate it (state/timing race) — user opted to leave it and capture a crash report if it recurs. Visible freeze on open = heavy init confirmed by user. (A true fix likely needs live inspection of the selector-9 buffer / matching native ProcessInput timing.)
- **Hide ImGui icons when the HUD is replaced** (world map etc. replace the whole interface): `FindWindow(0x8c) != null` is a reliable per-frame signal — the worldmap window is DESTROYED on close (`FUN_00a2e770` toggle-close → `FUN_00a447d0`), so FindWindow tracks open-state exactly. Gate both OnRenderUI draw and OnTick dispatch on it.
- Native grid hidden by swapping UIMenuIconWnd DrawContent (vtbl slot20 @`0x010281b0`+0x50, =`0x00814150` `RebuildNodes`) to a no-op. NOTE RebuildNodes is NOT per-frame (only on @load/@refresh/mode) — do NOT use a draw-hook flag as a per-frame "visible" signal (it flickers). The icon map (window+0xb4) stays populated so FindIconByCmd still works.

## Native-grid "ghost" + the clif_refresh fix (DONE 2026-06-27)
Hiding via the DrawContent swap leaves the ALREADY-composited grid pixels on screen: RebuildNodes builds the render-node list (window+4 = std::list, count @window+8) but the composited output isn't re-blitted until a relayout — so the native grid "ghosted" after enabling the ImGui replacement (and didn't reappear instantly on disable). Emptying the list (a "GridClear" DrawContent that self-loops window+4 + sets window+8=0) does NOT clear it (the composite lingers).
- **Fix (src/plugins/menu_icons.cc):** on each enable/disable transition set `pending_refresh_`, drained in OnTick (never mid-Present) → `RequestServerRefresh()` sends CZ 0x0BFD with id **`BOURGEON_SETTING_REFRESH = 25`**; moonlight `clif_parse_bourgeon_setting` → `clif_refresh(sd)` → ZC_NPCACK_MAPMOVE 0x91 → client re-composites in its recv loop → ghost gone / grid restored instantly. Confirmed both ways. Bourgeon commit 52ceb70; moonlight clif.cpp committed separately by the user.
- **Key insight:** `clif_refresh` triggered via Bourgeon is SAFE — the moonlight comment (clif.cpp ~5530) that it "unloads textures mid-D3D-frame (crashes)" was a one-off RACE CONDITION in the old mob-info flow, NOT clif_refresh itself (manual @refresh is proven safe in-game).
- **Why NOT @refresh-via-chat (even though @refresh is player-available):** the global-chat packet (008c/00f3) embeds "PlayerName : msg" and `clif_process_message` validates the name → a wrong name **forces a relog**. Too fragile. The custom 0x0BFD REFRESH id reuses validated infra (no name to forge, no new opcode). See [[project_opcode_system]].

## Les QUATRE états d'une icône, et le raccourci dans l'infobulle (2026-08-29)
- 🔴 **Une icône = QUATRE fichiers .bmp**, pas un atlas ni une teinte. Chaînes de
  format du natif, relevées en clair : `menu_icon\bt_%s.bmp`,
  `bt_%s_press.bmp`, `bt_%s_new.bmp`, `bt_%s_new_press.bmp` (@0x1028393 et
  suivantes). Chacun a sa TAILLE propre (le « _new » déborde vers le haut). La
  copie ImGui ne montrait que le normal : aucun retour visuel au clic.
- 🔴 **La touche qui ouvre une fenêtre = un RANG dans `HOTKEY_2`** de
  `data\luafiles514\lua files\hotkey.lub` (62 commandes, table complète en
  **docs/game_option_re.md §4.4 bis** — ne pas refaire la RE). Recoupé 4× sur
  `UserKeys.lua`. ⚠ **Témoin négatif** : le GRF porte aussi `hotkey_v2.lub`, où
  l'Interface est en `HOTKEY_3` — s'en servir décale TOUT en silence.
  On ne recopie que le rang : le nom de touche est relu chez le jeu à chaque
  survol (layout-aware). `userhotkey::ReadBindingForCommand` atteint aussi les
  12 commandes non remappables, que la fenêtre native refuse d'afficher.
- **Une icône de Bourgeon** (Atlas des recettes) = `wnd_id == 0` + `action_id`,
  et le clic part à `hotkeys::Invoke` (`features/hotkey_actions`), MÊME point
  d'entrée que la touche du joueur. Son art suit les conventions du jeu (`bt_atlas.bmp`,
  `bt_atlas_press.bmp`), livré dans `data\` du client — lu AVANT les GRF.
- ⚠ `data.grf` de Moonlight est chiffré : `tools/grf_reader.py` ne peut PAS
  vérifier l'existence des bitmaps natifs. C'est l'exe qui fait foi.

Related: [[reference_status_window_re]] [[reference_ui_window_manager]] [[project_status_tweaks_plugin]] [[reference_imgui_game_textures]] [[project_opcode_system]]
