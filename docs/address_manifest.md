# Manifeste des adresses natives (client 20250716, base 0x400000)

GÉNÉRÉ par `python tools/gen_address_manifest.py` — ne pas éditer à la main.
C'est la check-list du portage : chaque adresse ci-dessous est à retrouver
dans le nouvel exe, puis à corriger à CHAQUE site listé (un seul pour les
entrées ⚓ : leurs déclinaisons passent par l'annuaire).

- **784 adresses distinctes**, 1067 sites
- ⚓ 51 portées par l'annuaire (`ragnarok/*.h`, `ui/game_texture.h`)
- 120 présentes dans plusieurs fichiers (candidates annuaire si cohérentes)

| Adresse | Nom(s) | Sites | Commentaire |
|---|---|---|---|
| `0x00420000` | kSyntheticKeyLParam | `ragnarok\ragnarok_client.cc:62` |  |
| `0x004e52a0` | kStdStringCopyCtor | `features\windows\chat_window.cc:460` | __thiscall(dst, src), retn 4 |
| `0x004e5330` | kStdStringCtorCStr | `features\windows\navigation_window.cc:73` | __thiscall(this, cstr) |
| `0x004f08f0` | ⚓ kStdStringDtor, kStdStringDtorAddr, kStrDtor | `features\windows\chat_window.cc:96`<br>`features\windows\navigation_window.cc:72`<br>`ragnarok\globals.h:212`<br>`ragnarok\held_sprites.cc:41` | __thiscall(this) |
| `0x004f1940` | kStdStringAssign, kStdStringAssignAddr | `features\fx\palette_inject.cc:72`<br>`features\windows\chat_window.cc:95`<br>`features\windows\navigation_window.cc:71`<br>`ragnarok\emotion_hotkey.cc:39`<br>`ragnarok\user_hotkey.cc:53` | __thiscall(this, src, len) |
| `0x00519df0` | ⚓ kGetFieldAddr | `ragnarok\lua.h:46` | lua_getfield(L, idx, k) |
| `0x0051a290` | ⚓ kPCallAddr | `ragnarok\lua.h:47` | lua_pcall(L, nargs, nres, errfunc) |
| `0x0051a4b0` | ⚓ kPushNumberAddr | `ragnarok\lua.h:48` | lua_pushnumber(L, double) |
| `0x0051aab0` | ⚓ kSetTopAddr | `ragnarok\lua.h:49` | lua_settop(L, idx) |
| `0x0051abf0` | ⚓ kToBooleanAddr | `ragnarok\lua.h:50` | lua_toboolean(L, idx) |
| `0x0051aca0` | ⚓ kToLStringAddr | `ragnarok\lua.h:51` | lua_tolstring(L, idx, &len) |
| `0x0051ad20` | ⚓ kToNumberAddr | `ragnarok\lua.h:52` | lua_tonumber(L, idx) |
| `0x0051b570` | ⚓ kCheckStackAddr | `ragnarok\lua.h:53` | lua_checkstack(L, extra) |
| `0x0053f140` | kEngNodeBlit | `features\patches\chat.cc:366` | __thiscall(node, x, y, w, h, ARGB*, colorkey) |
| `0x0053faa0` | kVecU32Resize | `features\patches\chat.cc:745` | std::vector<uint32_t>::resize |
| `0x005471a0` | kEngTextOutLow | `features\patches\chat.cc:293`<br>`features\patches\chat.cc:866`<br>`features\patches\chat.cc:869` | __thiscall(ctx, x, y, str, len) |
| `0x005474a0` | kEngTextMeasure | `features\patches\chat.cc:294`<br>`features\patches\chat.cc:826` | __thiscall(ctx, SIZE*, str, len, font) |
| `0x00550b10` | kRenderQueueInsert | `features\fx\ez_effect_capture.cc:27` | RenderQueue_InsertPrimitive — hooké (capture) |
| `0x00553e80` | kDepthScale | `features\fx\ez_effect_capture.cc:24` | Effect_DepthToScreenScale(ctx,_,invW) -> float |
| `0x00554040` | kWorldDepthConvertVa | `features\fx\hat_effect_depth.cc:32` |  |
| `0x00554070` | kSpriteTexFactoryGetAddr | `ragnarok\game_settings.cc:154` |  |
| `0x005541b0` | kSceneProject | `features\fx\ez_effect_capture.cc:23` | Scene_ProjectWorldToScreen(ctx,_,world,view,&sx,&sy,&invW) |
| `0x0055c830` | kDX9DrawPrimRec | `features\fx\ground_paint.cc:74` | vtbl +0x38 : draw mono-texture |
| `0x0055c8c0` | kDX9DrawPrimDual | `features\fx\ground_paint.cc:75` | vtbl +0x34 : draw BI-texture (aniso) |
| `0x0055d680` | kDX9DrawGround | `features\fx\ground_paint.cc:50` | RendererDX9_DrawGroundTiles(this = renderer DX9) |
| `0x0055d850` | kDX9DrawTerrain | `features\fx\ground_paint.cc:76` | RendererDX9_DrawTerrainSurfaces |
| `0x00560d60` | kAdapterEqualsAddr | `ragnarok\game_settings.cc:202` |  |
| `0x00560f80` | kSpriteTexFactoryApplyAddr | `ragnarok\game_settings.cc:156` |  |
| `0x00560fb0` | kEnumAdaptersAddr | `ragnarok\game_settings.cc:176` |  |
| `0x005610b0` | kGetCurrentAdapterAddr | `ragnarok\game_settings.cc:194` |  |
| `0x00561550` | kEnumModesAddr | `ragnarok\game_settings.cc:186` |  |
| `0x005663d0` | ⚓ kAtlasBuildAddr | `ragnarok\render.h:54` |  |
| `0x00566770` | kConvertRgbaToArgb1555 | `features\fx\palette_inject.cc:66` |  |
| `0x00566b70` | ⚓ kAtlasGetCachedAddr | `ragnarok\render.h:53` |  |
| `0x00568760` | kSpriteRef | `features\overlays\status_icon_bar.cc:48` | __thiscall(cache,path,0,0,1,0) -> ref (5 args!) |
| `0x00568b30` | kSpriteTexCacheFlushAddr | `ragnarok\game_settings.cc:165` |  |
| `0x00573fc0` | kZlibDecompress | `ui\spr_act.cc:36` | __cdecl |
| `0x0059e950` | kSaveJsonAddr | `ragnarok\emotion_hotkey.cc:43` |  |
| `0x005a1460` | kCNameDict_GetEntryOrRequest, kNameDictGetEntry, kNameDictGetEntryOrRequest | `features\overlays\entity_names.cc:32`<br>`features\overlays\target_frame.cc:86`<br>`features\windows\chat_window.cc:1020`<br>`features\windows\entity_context_menu.cc:97`<br>`features\windows\entity_inspector.cc:41`<br>`features\windows\vending_window.cc:289` | __thiscall(dict, gid) |
| `0x005a18e0` | kNameDictContains | `features\windows\entity_inspector.cc:40` | __thiscall(dict, gid) |
| `0x005a4300` | kItemSkillInfoDtor | `features\windows\make_item_window.cc:231` |  |
| `0x005ab550` | kFnShareToChat | `features\windows\navigation_window.cc:65` | pose la balise dans le chat |
| `0x005c5950` | kSetOption | `features\overlays\skill_bar.cc:58` | SkillMgr_SetOption(mgr,key,val,0) ; key5=UI-lock (≥1 bloque OnMsg 0x29) |
| `0x005c8950` | kEmblemRequestUpload | `features\windows\character_sheet.cc:1556` | __thiscall(this, guildId, std::string) |
| `0x005d4c50` | kTabIndexToCategoryAddr | `ragnarok\user_hotkey.cc:18` |  |
| `0x005d54c0` | kSaveUserKeysAddr | `ragnarok\user_hotkey.cc:50` |  |
| `0x005d56d0` | kChangeHotKeyAddr | `ragnarok\user_hotkey.cc:48` |  |
| `0x005ff990` | kSoundMgrGet | `features\overlays\login_parade.cc:28` | getter/lazy-create du SoundMgr |
| `0x00600430` | kSoundPlay2D | `features\overlays\login_parade.cc:34` | Sound_PlaySample2D(this,handle,&x,&y,&z,min,max,vol) |
| `0x00600770` | kSoundPlay3D | `features\minigames\roggle.cc:237`<br>`features\minigames\rojeweled.cc:92`<br>`features\windows\monster_info_window.cc:170` |  |
| `0x00600a80` | kSoundSetMaster2DAddr | `ragnarok\game_settings.cc:95` |  |
| `0x00600ab0` | kSoundSet3DAddr | `ragnarok\game_settings.cc:96` |  |
| `0x00600ae0` | kSoundSetEffectAddr | `ragnarok\game_settings.cc:94` |  |
| `0x00600be0` | kSoundSetBgmAddr | `ragnarok\game_settings.cc:93` |  |
| `0x006046e0` | kDeferEntry | `features\fx\weapon_layer.cc:86` | CActorSprite_DeferQuadSorted |
| `0x006049e4` | kKeyWrite | `features\fx\weapon_layer.cc:87` | branch-1 key write (MOV [EAX+0x10],EDI) |
| `0x006060ff` | (littéral) | `features\windows\chat_window.cc:7247` |  |
| `0x0060c820` | (littéral) | `ragnarok\configuration.h:13` |  |
| `0x00613100` | (littéral) | `ragnarok\configuration.h:14` |  |
| `0x0061d370` | kGetEmblemPath | `features\windows\character_sheet.cc:1233` | __thiscall(this, out_str, guildId) |
| `0x0061d560` | kGetEmblemVersion | `features\windows\character_sheet.cc:1325` | __thiscall(this, guildId) -> version |
| `0x00623e20` | (littéral) | `ragnarok\configuration.h:15` |  |
| `0x0065ae30` | (littéral) | `ragnarok\configuration.h:20` |  |
| `0x0068cdf0` | kMapGetOrInsertAddr | `ragnarok\game_settings.cc:57` |  |
| `0x0068dfd0` | kMgrSetOptionAddr | `ragnarok\game_settings.cc:38` |  |
| `0x0068e160` | kMgrExecOptionAddr | `ragnarok\game_settings.cc:66` |  |
| `0x0068ea70` | kGetFlagAddr, kOptionInfoGetValue | `features\overlays\menu_icons.cc:236`<br>`ragnarok\game_settings.cc:22` | __cdecl(ttIndex) -> bool |
| `0x0068eaf0` | kIsInvertedAddr | `ragnarok\game_settings.cc:28` |  |
| `0x0068f8e0` | kMgrResetAllAddr | `ragnarok\game_settings.cc:70` |  |
| `0x0068fc70` | kSetFlagByCommandAddr | `ragnarok\game_settings.cc:44` |  |
| `0x0068fd50` | kOptionInfoSetValue, kSetFlagRawAddr | `features\overlays\menu_icons.cc:237`<br>`ragnarok\game_settings.cc:33` | __cdecl(ttIndex, value) |
| `0x0069f480` | kProbFetch | `features\windows\item_desc_window.cc:165` | ItemProbabilityDB_Fetch |
| `0x006a06b0` | ⚓ kEnsureLoadedAddr | `ragnarok\item_db.h:32` |  |
| `0x006a0d40` | ⚓ kLookupAddr | `ragnarok\item_db.h:27` | __cdecl(id, &table) -> record |
| `0x006a1b20` | ⚓ kInfoCtorAddr, kItemSkillInfoCtor | `features\windows\make_item_window.cc:230`<br>`ragnarok\item_db.h:40` |  |
| `0x006a2970` | kGetCardResName | `features\windows\item_desc_window.cc:96` | __cdecl(id) -> record ; *record = resname |
| `0x006a2a70` | kGetDescLines | `features\windows\item_desc_window.cc:79` | ItemSkillDB_GetDescLines(info) -> &vector<char*> |
| `0x006a2b50` | ⚓ kBaseNameFallbackAddr | `ragnarok\item_db.h:48` |  |
| `0x006a2dd0` | (littéral) | `ragnarok\configuration.h:34` |  |
| `0x006a3f20` | kRecipeGetLines | `features\windows\make_item_window.cc:611` |  |
| `0x006a4bc0` | kGetResName | `features\windows\item_desc_window.cc:80`<br>`ui\icon_cache.cc:51` | ItemSkillDB_GetResName(info) -> resname C-str (icône) |
| `0x006a4c10` | ⚓ kSlotCountAddr | `ragnarok\item_db.h:55` |  |
| `0x006a5db0` | kBookDbContains | `features\windows\item_desc_window.cc:177` | BookItemDB_Contains |
| `0x006a6570` | ⚓ kInfoSetIdAddr, kItemSkillInfoSetId | `features\windows\make_item_window.cc:374`<br>`ragnarok\item_db.h:41` |  |
| `0x006aa6e0` | (littéral) | `ragnarok\configuration.h:35` |  |
| `0x006bcfc0` | (littéral) | `ragnarok\configuration.h:36` |  |
| `0x006f4800` | (littéral) | `ragnarok\configuration.h:41` |  |
| `0x0070f4b0` | ⚓ kActionGetFrameAddr | `ragnarok\render.h:58` |  |
| `0x007103d0` | kVecStrResize | `features\patches\chat.cc:744` | std::vector<std::string>::resize |
| `0x007110c0` | kTerrainHeight | `features\gameplay\player_jump.cc:17` | Terrain_GetHeightAt(world,x,z)->float |
| `0x00715be0` | kStrLayerTex | `features\overlays\basic_info.cc:304` | Str_GetLayerTexture(layer, idx) |
| `0x00715d30` | kStrLayerTexDir | `features\overlays\basic_info.cc:336` | Str_GetLayerTextureWithDir(layer, idx, &dir) |
| `0x007166b0` | (littéral) | `ragnarok\configuration.h:55` |  |
| `0x0071df50` | (littéral) | `ragnarok\configuration.h:56` |  |
| `0x00727444` | (littéral) | `utils\hooking\detours.h:73` | "Dtr\0" |
| `0x007289da` | (littéral) | `features\systems\discord_relay.cc:12` |  |
| `0x00731350` | (littéral) | `ragnarok\configuration.h:57` |  |
| `0x00738370` | kSkillGetTabList | `features\windows\character_sheet.cc:1029`<br>`features\windows\weapon_refine_window.cc:182`<br>`ragnarok\player_skills.cc:13` | __thiscall(bundle, tab) -> std::list* |
| `0x00738570` | kSkillSetUseLevel | `features\windows\character_sheet.cc:1030` | __thiscall(bundle, id, lv) |
| `0x00739cd0` | kSkillEntryDtor, kSkillInfoDtor | `features\overlays\skill_bar.cc:673`<br>`features\windows\character_sheet.cc:139`<br>`ragnarok\homunculus.cc:66` | = FUN_00739cd0 (cleanup de la struct) |
| `0x0073a140` | kGetSkillIdNameLua | `features\overlays\skill_bar.cc:564`<br>`features\windows\character_sheet.cc:184` | char* GetSkillIdName(int) __cdecl |
| `0x0073a1f0` | kGetSkillNameLua | `features\overlays\cast_bar.cc:35`<br>`features\overlays\skill_bar.cc:97`<br>`features\windows\character_sheet.cc:2792`<br>`features\windows\craft_atlas.cc:43`<br>`features\windows\monster_info_window.cc:393` | char* GetSkillName(int id) (__cdecl, via Lua) |
| `0x0073adb0` | kIsLevelUseSkill | `features\windows\character_sheet.cc:1032` | __cdecl(id) : l'effet dépend-il du niveau ? |
| `0x0075f850` | (littéral) | `ragnarok\configuration.h:62` |  |
| `0x0079d5e0` | kSelCharRenderPatch | `ragnarok\ragnarok_client.cc:238` | mov ecx,[esi+0x120] |
| `0x0079d610` | kCharSelOnMsg | `features\windows\char_select.cc:122` | vtbl+0x94, RET 0x18 |
| `0x007a6df0` | kColorChip | `features\patches\inventory_tweaks.cc:141` | FUN_007a6df0(x,y,&r,&g,&b) -> colorchip.bmp pixel |
| `0x007a6f10` | kSkinGetNameAddr | `ragnarok\game_settings.cc:239` |  |
| `0x007a7f70` | kSkinSetAddr | `ragnarok\game_settings.cc:244` |  |
| `0x007a7fa0` | kStdVectorIntPushBack | `features\windows\entity_context_menu.cc:95` | __thiscall(vec*, int*) |
| `0x007c72e0` | kPurgeStaleMails | `features\windows\rodex_window.cc:131` | __thiscall(CRodexSystemMgr*) |
| `0x007f8969` | kSwapRightIconImm | `features\patches\equip_tweaks.cc:94` | mov [ebp-0x4d0],0xfa (right icon x=250) |
| `0x007f897d` | kSwapRightNameImm | `features\patches\equip_tweaks.cc:95` | mov [ebp-0x4d8],0xad (right name x=173) |
| `0x007f9688` | kSwapTitleImm | `features\patches\equip_tweaks.cc:93` | push 0x8c imm32  (title x, centred) |
| `0x0080d1a0` | kListErase | `features\patches\inventory_tweaks.cc:169` | std::list::erase(first,last) __thiscall(listObj,&out,first,last): frees nodes + dtors + si |
| `0x00812e60` | kToggleWndById | `features\windows\inventory_viewer.cc:89` | FUN_00812e60(id) __stdcall (RET 0x4, vérifié désasm) : bascule fenêtre (ferme si ouverte v |
| `0x00814064` | kVisTable | `features\overlays\menu_icons.cc:79` | indexed by (cmdId - 0x178), 0..0xDF |
| `0x00814150` | kGridDrawOrig | `features\overlays\menu_icons.cc:48` | UIMenuIconWnd_RebuildNodes |
| `0x00814a70` | kCmdHandler | `features\overlays\menu_icons.cc:69` | UIMenuIconWnd command handler |
| `0x00814bb0` | (littéral) | `ragnarok\configuration.h:17` |  |
| `0x00815630` | (littéral) | `ragnarok\configuration.h:18` |  |
| `0x008217f0` | (littéral) | `features\windows\inventory_viewer.cc:550` |  |
| `0x008263a0` | kBalloonPaint | `features\overlays\chat_balloon.cc:34` |  |
| `0x00830240` | kSetTextWrapped | `features\overlays\chat_balloon.cc:32` |  |
| `0x008303f0` | kSetTextAddr | `features\systems\native_login.cc:21` | CUIEdit_SetText |
| `0x00831a50` | kSetName | `features\patches\inventory_tweaks.cc:97` | UIItemLinkBtn_SetName(this, char*) |
| `0x0083d3f0` | kWrapAndDispatch | `features\patches\chat.cc:264`<br>`features\patches\chat.cc:858` | UISubChatWnd_WrapAndDispatch |
| `0x0083d840` | (littéral) | `features\patches\chat.cc:833`<br>`features\patches\chat.cc:836` |  |
| `0x0083f070` | kSubChatAddLine | `features\patches\chat.cc:233`<br>`features\patches\chat.cc:847` | __thiscall(this,text,color,sender) |
| `0x00857910` | kTabDrawOrig | `features\patches\inventory_tweaks.cc:157` | FUN_00857910 tab DrawContent |
| `0x0085fca0` | kTabRecompute | `features\patches\inventory_tweaks.cc:150` | FUN_0085fca0(tabctrl): recompute strip size after a size[] change |
| `0x008642d0` | kRebuildFromHist | `features\patches\chat.cc:535` | __thiscall(tab): clear+re-wrap history |
| `0x00864690` | kAddTab | `features\patches\inventory_tweaks.cc:168` | tab control AddTab(this, label, tooltip) |
| `0x00874af0` | (littéral) | `features\fx\screen_fx.cc:397` |  |
| `0x00880e7f` | kSnapJnz | `features\patches\window_pos_tweaks.cc:378` | JNZ 0x00880e9f  (bytes 75 1e) |
| `0x008848d0` | kOnMsgAddr | `features\systems\native_login.cc:22` | UILoginWnd_OnMsg |
| `0x0088d730` | (littéral) | `ragnarok\configuration.h:24` |  |
| `0x0088e6d0` | (littéral) | `ragnarok\configuration.h:25` |  |
| `0x00897df1` | kRightIconImm | `features\patches\equip_tweaks.cc:63` | mov [ebp-0x534],0xfb  (right icon x=251) |
| `0x00897e05` | kRightNameImm | `features\patches\equip_tweaks.cc:64` | mov [ebp-0x544],0xa7  (right name x=167) |
| `0x00897ffb` | (littéral) | `features\patches\equip_tweaks.cc:65` | push 0x46 imm8 — slots 0-7 (2 columns) |
| `0x00898040` | (littéral) | `features\patches\equip_tweaks.cc:66` | push 0x46 imm8 — slots 8-9 (top headgear row) |
| `0x00898bc0` | kDrawTitleBar | `features\patches\status_tweaks.cc:67` | __thiscall(this, char hasClose, char* title, int width) |
| `0x00898cc5` | kTitleColorImm | `features\patches\inventory_tweaks.cc:229` | imm of PUSH 0xffffff in DrawTitleBar (title text, ALL windows) |
| `0x00898cd2` | kTitleWhiteY | `features\patches\status_tweaks.cc:79` | lea eax,[edi-0xd] disp8 (white y-off) |
| `0x00898cd5` | kTitleWhiteX | `features\patches\status_tweaks.cc:78` | push 0x13  (white title x) |
| `0x00898cea` | kTitleBlackY | `features\patches\status_tweaks.cc:81` | lea eax,[edi-0xe] disp8 (black y-off) |
| `0x00898cef` | kTitleBlackX | `features\patches\status_tweaks.cc:80` | push 0x12  (black edge x) |
| `0x0089e4c8` | kDragRectImm | `features\patches\equip_tweaks.cc:78` | lea eax,[edi-0xfb] disp32 (right grip-rect base) |
| `0x008a0570` | ⚓ kBuildDisplayNameAddr | `ragnarok\item_db.h:47` |  |
| `0x008b3427` | kHighlightImm | `features\patches\equip_tweaks.cc:82` | mov [ebp-0x128],0xf8 imm32 |
| `0x008b66a0` | kDrawOrig | `features\patches\status_tweaks.cc:52`<br>`features\patches\status_tweaks.cc:344` | original UIStatusWnd::DrawContent |
| `0x008bf7d0` | kMsgOrig | `features\patches\equip_tweaks.cc:110` | FUN_008bf7d0 equip msg handler (ret 0x18 = SIX stack args!) |
| `0x008c049c` | kSwapWidthImm | `features\patches\equip_tweaks.cc:92` | push 0x118 imm32 (swap panel SetSize width) |
| `0x008c18b0` | kItemDescWndAddr | `features\moonlight_ui\moonlight_ui.h:337` |  |
| `0x008c5860` | kMsgOrig | `features\overlays\item_obtain_toast.cc:38` | UINotifyItemObtainWnd_OnMsg |
| `0x008cb7c0` | kMsgOrig | `features\patches\status_tweaks.cc:55` | FUN_008cb7c0 status msg handler (ret 0x18 = SIX stack args!) |
| `0x008d4580` | (littéral) | `ragnarok\configuration.h:59` |  |
| `0x008d5410` | (littéral) | `ragnarok\configuration.h:60` |  |
| `0x008db420` | (littéral) | `features\fx\screen_fx.cc:399` |  |
| `0x008dc0d0` | kChatToggleInputBar | `features\windows\chat_window.cc:498` | __thiscall(this), retn 0 |
| `0x008e1730` | kChatTagTransform | `features\windows\chat_window.cc:459` | __thiscall(this, out, in), retn 8 |
| `0x008e1d50` | kGetOption | `features\overlays\skill_bar.cc:57` | SkillInfoMgr_GetOption(mgr,key) ; key10=onglet courant |
| `0x008f3498` | kDialogBgBlitRet | `features\patches\chat.cc:192` | return addr of the dialog_bg blit |
| `0x008f5800` | kOnDraw | `features\overlays\skill_bar.cc:149` | UIShortCutWnd::OnDraw (vtable+0x50) — contenu slots |
| `0x008f9840` | kInputRowLayout | `features\patches\chat.cc:533`<br>`features\patches\chat.cc:925` | __thiscall(this) — input-row controls |
| `0x008fc220` | kChatWndProc | `features\patches\chat.cc:531`<br>`features\patches\chat.cc:891` | __thiscall WndProc (vtable+0x94) |
| `0x008fd12a` | kChatWrapCaller | `features\patches\chat.cc:158` | return addr of the chat call site |
| `0x00901310` | kShortCutOnMsg | `features\overlays\skill_bar.cc:211` | UIShortCutWnd::OnMsg (vtable+0x94) |
| `0x0090243f` | kDetachedWrapCaller | `features\patches\chat.cc:265` | ret addr of the detached call site |
| `0x009030c0` | kHideNative, kSetVisibleFn | `features\overlays\skill_bar.cc:158`<br>`features\windows\item_desc_window.cc:78` | UIWnd_SetVisible (vtable+0x38, __thiscall) |
| `0x00903160` | kSetTabBarHeight | `features\patches\chat.cc:532` | __thiscall(this, rows) — relayout |
| `0x0090ce90` | (littéral) | `ragnarok\configuration.h:22` |  |
| `0x009137a0` | kDrawContent | `features\overlays\quest_tracker.cc:26` | QuestTracker_DrawContent (hooked) |
| `0x0091d470` | (littéral) | `ragnarok\configuration.h:38` |  |
| `0x0091e1f0` | (littéral) | `ragnarok\configuration.h:39` |  |
| `0x009386d0` | kFavDropSlotImm | `features\patches\inventory_tweaks.cc:183` | imm8 of CMP EAX,3 in FUN_00938650 case 1 |
| `0x0093da20` | (littéral) | `ragnarok\configuration.h:10` |  |
| `0x0093f1f9` | (littéral) | `features\patches\inventory_tweaks.cc:121` | FUN_0093f100 SUB EAX,0x26   : scrollbar height |
| `0x0093f278` | (littéral) | `features\patches\inventory_tweaks.cc:130` | FUN_0093f100 LEA [EBX-0x26] : item row count |
| `0x00946da0` | kDrawOrig | `features\patches\inventory_tweaks.cc:36`<br>`features\patches\inventory_tweaks.cc:706` | original inventory DrawContent |
| `0x00947053` | (littéral) | `features\patches\inventory_tweaks.cc:127` | FUN_00946da0 SUB EAX,0x26   : grey separator LINE length (stop at grid bottom, not into th |
| `0x0094708b` | (littéral) | `features\patches\inventory_tweaks.cc:126` | FUN_00946da0 SUB EAX,0x26   : slot-cell grid row count |
| `0x0094afb0` | (littéral) | `ragnarok\configuration.h:11` |  |
| `0x0094ee20` | (littéral) | `ragnarok\configuration.h:66` |  |
| `0x009500f0` | (littéral) | `ragnarok\configuration.h:67` |  |
| `0x00950400` | kInvFinalize | `features\patches\inventory_tweaks.cc:170` | FUN_00950400(inv): refresh scrollbar/scroll from inv+0xec count |
| `0x00955530` | kMsgOrig | `features\patches\inventory_tweaks.cc:167` | FUN_00955530 inventory message handler |
| `0x00955980` | kMaxWidthImm | `features\patches\inventory_tweaks.cc:37` | MOV EDI,0x140 imm (resize max W) |
| `0x0095598b` | kMaxHeightImm | `features\patches\inventory_tweaks.cc:38` | MOV EDX,0x0f0 imm (resize max H) |
| `0x009559b9` | (littéral) | `features\patches\inventory_tweaks.cc:122` | case 0xe     LEA [ECX+0x26] : tabH + reserve (snap) |
| `0x009559c3` | (littéral) | `features\patches\inventory_tweaks.cc:123` | case 0xe     SUB EAX,0x26   : available grid height (snap) |
| `0x009559d7` | (littéral) | `features\patches\inventory_tweaks.cc:124` | case 0xe     LEA [EAX+0x26] : FINAL window height (SetSize) |
| `0x00955a69` | (littéral) | `features\patches\inventory_tweaks.cc:125` | case 0xe     SUB EAX,0x26   : scrollbar height (resize) |
| `0x0095dfb0` | kBILayoutChildren | `features\overlays\basic_info.cc:2808` | __thiscall(wnd, height) |
| `0x0096b72b` | kLayoutTabBoundImm | `features\patches\inventory_tweaks.cc:175` | the imm8 (0x03) of that CMP |
| `0x00977e80` | kDrawContent | `features\patches\skill_tree_tweaks.cc:31` | UINewSkillListWnd::DrawContent |
| `0x00979f10` | kCharWrap | `features\patches\chat.cc:157`<br>`features\patches\chat.cc:902` | __cdecl(text, outlist, maxchars) |
| `0x00996910` | (littéral) | `ragnarok\configuration.h:45` |  |
| `0x00997b50` | (littéral) | `ragnarok\configuration.h:46` |  |
| `0x009b9b9b` | (littéral) | `features\windows\chat_window.cc:5709` | COLORREF gris, comme les en-têtes du client |
| `0x009ded20` | (littéral) | `ragnarok\configuration.h:64` |  |
| `0x009eb410` | (littéral) | `features\fx\screen_fx.cc:401` |  |
| `0x00a145b0` | (littéral) | `ragnarok\configuration.h:52` |  |
| `0x00a1cb30` | kClearSurface | `features\overlays\chat_balloon.cc:37` |  |
| `0x00a1cb70` | kUISetSize | `features\patches\chat.cc:534` | __thiscall(obj, w, h) |
| `0x00a1d260` | kBlit, kBlitImageToNode | `features\patches\chat.cc:191`<br>`features\patches\chat.cc:913`<br>`features\patches\inventory_tweaks.cc:48`<br>`features\patches\status_tweaks.cc:70` | __thiscall(this,x,y,tex,flag) |
| `0x00a1d460` | kFill | `features\patches\inventory_tweaks.cc:49` | __thiscall(this,x,y,w,h,color) filled rect |
| `0x00a1f2a0` | (littéral) | `ragnarok\configuration.h:43` |  |
| `0x00a21c90` | kMeasureW | `features\patches\chat.cc:877`<br>`features\patches\chat.cc:880`<br>`features\patches\inventory_tweaks.cc:47` | __thiscall(this,str,len,face,size,_,_) -> width |
| `0x00a226f0` | (littéral) | `ragnarok\configuration.h:53` |  |
| `0x00a25a70` | kDrawText | `features\patches\inventory_tweaks.cc:46`<br>`features\patches\status_tweaks.cc:68` | __thiscall(this,x,y,str,len,face,size,color,bold,ital) |
| `0x00a27b50` | kDrawTextR | `features\patches\status_tweaks.cc:69` | __thiscall(this,x,y,str,len,face,size,color,bold) RIGHT@x |
| `0x00a29ba0` | (littéral) | `ragnarok\configuration.h:83` |  |
| `0x00a2c600` | kFriendListAddByName | `features\windows\chat_window.cc:212` |  |
| `0x00a2cc20` | kWhisperPivotAddr | `features\windows\chat_window.cc:569` |  |
| `0x00a2e770` | ⚓ kCloseWindowAddr | `ragnarok\uiwnd.h:58` | UIWindowMgr::Close(id) |
| `0x00a31a30` | (littéral) | `features\windows\char_select.cc:322`<br>`features\windows\char_select.cc:325` |  |
| `0x00a33005` | kSnapLoopHook | `features\patches\window_pos_tweaks.cc:399` | MOV ECX,[EBP-0x88] (8B 8D 78 FF FF FF) |
| `0x00a336d0` | kWndAtPointAddr | `features\gameplay\quick_cast.cc:35` |  |
| `0x00a388f0` | kFriendListContains | `features\windows\entity_context_menu.cc:103` | __thiscall(mgr, name) |
| `0x00a39340` | ⚓ kMakeWindowAddr | `ragnarok\uiwnd.h:57` | UIWindowMgr::MakeWindow(id) |
| `0x00a3a2e4` | kWidthOwnImm | `features\patches\equip_tweaks.cc:61` | push 0x118 imm32 (own equip window) |
| `0x00a3a48b` | kHeightImm | `features\patches\status_tweaks.cc:49` | push 0x8d (h=141) imm in MakeWindow |
| `0x00a3a490` | kWidthImm | `features\patches\status_tweaks.cc:50` | push 0x118 (w=280) imm in MakeWindow |
| `0x00a3f0f6` | kWidthOtherImm | `features\patches\equip_tweaks.cc:62` | push 0x118 imm32 (view-other-player) |
| `0x00a447d0` | kQueueDestroyWindow | `features\overlays\chat_balloon.cc:45` |  |
| `0x00a471e0` | (littéral) | `ragnarok\configuration.h:72`<br>`ragnarok\configuration.h:84` |  |
| `0x00a47400` | kSendMacroAddr | `ragnarok\emotion_hotkey.cc:49` |  |
| `0x00a47b90` | ⚓ kFindWindowAddr | `ragnarok\uiwnd.h:26` | UIWindowMgr::FindWindow(id) __thiscall |
| `0x00a4ad20` | kChatActionAddr, kChatAddLine | `features\windows\bank_window.cc:135`<br>`features\windows\chat_window.cc:55`<br>`ragnarok\configuration.h:85` | = UIWindowMgr::SendMsg |
| `0x00a4b760` | kFnSetFocusedWnd | `features\windows\navigation_window.cc:66` | __thiscall(mgr, fenetre) |
| `0x00a50b70` | (littéral) | `ragnarok\configuration.h:31` |  |
| `0x00a5e960` | (littéral) | `ragnarok\configuration.h:32` |  |
| `0x00a69eb0` | kActorListFindByGid, kFindByGID | `features\gameplay\player_jump.cc:18`<br>`features\windows\entity_context_menu.cc:99` | ActorList_FindByGID(actorMgr,gid)->acteur |
| `0x00a727f0` | kActiveIdSetContains | `features\windows\entity_context_menu.cc:96` | __cdecl(aid) -> bool |
| `0x00a74410` | kCursorRenderFn | `ragnarok\ragnarok_client.cc:86` | CursorMgr_RenderSprite |
| `0x00a75340` | ⚓ kGameModeGetActive, kModeMgrGetActiveAddr | `features\patches\damage_name_fix.cc:17`<br>`ragnarok\globals.h:138` |  |
| `0x00a756e0` | (littéral) | `ragnarok\configuration.h:108` |  |
| `0x00a79610` | kInsert | `features\patches\pick_quad_tweaks.cc:21` | NameplateQueue_Insert |
| `0x00a797b0` | kQuadTreeQuery, kQuadTreeQueryPointAddr | `features\gameplay\quick_cast.cc:27`<br>`features\windows\char_diagnostics.cc:463` |  |
| `0x00a7b0a0` | (littéral) | `ragnarok\configuration.h:122` |  |
| `0x00a85be0` | kBannedAddr | `ragnarok\pet.cc:105` |  |
| `0x00a88ab0` | kLoadToMemory, kLoadToMemoryAddr | `ragnarok\msgstring_override.cc:143`<br>`ui\spr_act.cc:30` | __thiscall(mgr, path, DWORD*, char) |
| `0x00a892c0` | kFreeBuffer, kFreeBufferAddr | `ragnarok\msgstring_override.cc:144`<br>`ui\spr_act.cc:31` | __stdcall(void*) |
| `0x00a8d3e0` | kFindCachedRes | `features\fx\palette_inject.cc:35` |  |
| `0x00a8d4a0` | ⚓ kLoad | `ui\game_texture.h:35` | __fastcall(mgr, _, key) -> tex |
| `0x00a8e800` | kResAddRef, kTexAddRef | `features\fx\weapon_dual_sprites.cc:21`<br>`features\overlays\basic_info.cc:935` | resource AddRef (ECX = res) |
| `0x00a8f910` | kResRelease | `features\fx\weapon_dual_sprites.cc:22` | resource Release (ECX = res) |
| `0x00a90350` | ⚓ kGet | `ui\game_texture.h:33` | __cdecl() -> mgr |
| `0x00a94870` | kFmtComma64 | `features\windows\bank_window.cc:42` |  |
| `0x00a948d0` | kFmtComma, kFmtComma32 | `features\patches\inventory_tweaks.cc:53`<br>`features\windows\bank_window.cc:43`<br>`features\windows\inventory_viewer.cc:86` | __cdecl(value,buf,size) -> thousands-separated |
| `0x00a94930` | kStdStringFromFmt | `features\windows\character_sheet.cc:1557` | (dst, fmt, …) -> std::string du jeu |
| `0x00a98400` | kXmlFindChild | `features\systems\native_login.cc:61` | __thiscall(node, name) -> node |
| `0x00a98460` | kXmlFindNextSibling | `features\systems\native_login.cc:62` | __thiscall(node, name) -> node |
| `0x00a984c0` | kXmlGetText | `features\systems\native_login.cc:63` | __fastcall(node) -> std::string* |
| `0x00a9a7d0` | ⚓ kCallGlobalVaAddr | `ragnarok\lua.h:60` |  |
| `0x00a9bc90` | ⚓ kExecFileAddr | `ragnarok\lua.h:61` |  |
| `0x00a9ed30` | kGetAddr | `ragnarok\msgstring.h:25` |  |
| `0x00a9f030` | ⚓ kMakeKey | `ui\game_texture.h:34` | __cdecl(path) -> key |
| `0x00aa7b00` | (littéral) | `ragnarok\configuration.h:105` |  |
| `0x00ad8750` | kScreenLayerLoopVa | `features\fx\hat_effect_depth.cc:26` |  |
| `0x00ad8b7d` | (littéral) | `features\fx\hat_effect_depth.cc:282` |  |
| `0x00ad8d91` | (littéral) | `features\fx\hat_effect_depth.cc:278` |  |
| `0x00ad9308` | (littéral) | `features\fx\hat_effect_depth.cc:280` |  |
| `0x00ad93c5` | (littéral) | `features\fx\hat_effect_depth.cc:279` |  |
| `0x00ad93d4` | kDepthHookVa | `features\fx\hat_effect_depth.cc:28` |  |
| `0x00ad93d9` | kDepthStockContinueVa | `features\fx\hat_effect_depth.cc:29` | path natif |
| `0x00ad93e2` | kDepthConvertedContinueVa | `features\fx\hat_effect_depth.cc:30` | path converti |
| `0x00ada5a4` | kStrParticleRet | `features\fx\ez_effect_capture.cc:62` |  |
| `0x00ae8480` | kEffRenderFn | `features\fx\ez_effect_capture.cc:42` | EffectInstance_RenderDraw (__fastcall, ECX = effet) |
| `0x00afe41c` | kRenderCallNoArgVa | `features\fx\hat_effect_depth.cc:23` |  |
| `0x00afe52b` | kRenderCallArgVa | `features\fx\hat_effect_depth.cc:24` |  |
| `0x00b1fac0` | kReplayClock | `ragnarok\skill_cooldowns.cc:24` | CReassemblyPacketMgr_GetInstance |
| `0x00b20ce0` | kFnFindMapByName | `features\windows\navigation_window.cc:95` | __thiscall(nav, name) |
| `0x00b26b00` | kFnMapDisplayName | `features\windows\navigation_window.cc:70` | nom AFFICHÉ d'une carte |
| `0x00b26cf0` | kFnNodeName | `features\windows\navigation_window.cc:64` | CNaviNode::GetName (interne) |
| `0x00b2e700` | kFnGetResult | `features\windows\navigation_window.cc:59` | (out40, index) |
| `0x00b2ee00` | kFnGetStep | `features\windows\navigation_window.cc:63` |  |
| `0x00b2f080` | kFnClearRoute | `features\windows\navigation_window.cc:171` | __thiscall(nav, bool full) |
| `0x00b314f0` | kFnSearchRoute, kNaviRoute | `features\overlays\minimap.cc:421`<br>`features\windows\item_desc_window.cc:117`<br>`features\windows\navigation_window.cc:60` | CNavigation::SearchRoute |
| `0x00b34970` | kFnSearch | `features\windows\navigation_window.cc:58` | CNavigation::Search |
| `0x00b34ae0` | kFnSetRouteIcon | `features\windows\navigation_window.cc:126` | __thiscall(nav, n) |
| `0x00b35f80` | kFnSelectResult | `features\windows\navigation_window.cc:61` | pose (ou EFFACE) la cible |
| `0x00b37d80` | kFnRebuildRoutePaths | `features\windows\navigation_window.cc:127` | __thiscall(nav) |
| `0x00b39660` | kFnStepCount | `features\windows\navigation_window.cc:62` |  |
| `0x00b5ed20` | kRenderType0 | `features\overlays\status_icon_bar.cc:83` |  |
| `0x00b666d0` | kEzEffectDraw | `features\fx\ez_effect_capture.cc:26` | EzEffect_Draw(nœud EZ) — hooké (appartenance) |
| `0x00b90780` | kEffCtor | `features\overlays\basic_info.cc:796` | EffectInst_Ctor_StrNode(node) |
| `0x00bb4170` | kEffLoadStr | `features\overlays\basic_info.cc:797` | Effect_LoadStrByEffectId(node,src,id,x,y,z) |
| `0x00bb5d10` | kMakeNode | `features\overlays\status_icon_bar.cc:46` | __thiscall(scene,0,0.0(8),0.0f) -> node |
| `0x00bc2de1` | kEffectJumpDefault | `features\fx\ez_effect_capture.cc:89` |  |
| `0x00bc2e04` | kEffectJumpTable | `features\fx\ez_effect_capture.cc:86`<br>`features\fx\spr_effect_lab.cc:555` |  |
| `0x00bced10` | kEffUpdateKF | `features\overlays\basic_info.cc:798` | Effect_UpdateStrKeyframes(node,offXYZ,f,f) |
| `0x00bcfb10` | kStrSubmitQuad | `features\overlays\basic_info.cc:303` | Effect_SubmitStrQuad (hooké) |
| `0x00bd4230` | kBuildFn | `features\overlays\status_icon_bar.cc:45` | status-bar build/layout (hooked) |
| `0x00c0c0c0` | kBarFill | `features\patches\inventory_tweaks.cc:85` | grid-gap filler, matches the btnbar grey (palette idx 7) |
| `0x00c13fc0` | (littéral) | `ragnarok\configuration.h:87` |  |
| `0x00c14320` | kConnDisconnectAddr | `ragnarok\game_settings.cc:217` |  |
| `0x00c144b0` | (littéral) | `ragnarok\configuration.h:98` |  |
| `0x00c147d0` | (littéral) | `ragnarok\configuration.h:104` |  |
| `0x00c148b0` | (littéral) | `ragnarok\configuration.h:99` |  |
| `0x00c14920` | (littéral) | `ragnarok\configuration.h:88` |  |
| `0x00c14d60` | kConnGetInstanceAddr | `ragnarok\game_settings.cc:215` |  |
| `0x00c3c3ff` | (littéral) | `features\overlays\entity_names.cc:150` | mob bleu clair / joueur+npc blanc |
| `0x00c43230` | kResolveDisplayJob | `features\windows\char_diagnostics.cc:107` |  |
| `0x00c44940` | kToggleEffectId | `features\fx\spr_effect_lab.cc:29`<br>`features\overlays\basic_info.cc:531` | Actor_ToggleEffectId(actor, unifiedId, add) __thiscall |
| `0x00c480fd` | (littéral) | `features\patches\damage_name_fix.cc:29` | le nom de la CIBLE — celui qui manque dans « [%s] subit … » |
| `0x00c48122` | (littéral) | `features\patches\damage_name_fix.cc:30` | le nom de l'ATTAQUANT |
| `0x00c4d0a7` | kProcessDamageCall | `features\patches\damage_name_fix.cc:15` | son appelant, UNIQUE |
| `0x00c55bf0` | kSetBodyAct | `features\fx\wand_bolt.cc:107` | CActorSprite_SetBodyActFromPath |
| `0x00c58d13` | kCallSite | `features\patches\pick_quad_tweaks.cc:20` | E8 rel32 |
| `0x00c58e90` | kSetBodySpr | `features\fx\wand_bolt.cc:106` | CActorSprite_SetBodySprFromPath |
| `0x00c5dfc0` | kProcessDamageAction | `features\patches\damage_name_fix.cc:14` | CActorSprite_ProcessDamageAction |
| `0x00c5e372` | kDamageDueTimeCall | `features\fx\wand_bolt.cc:160` |  |
| `0x00c68e50` | kCopyEntityName | `features\patches\damage_name_fix.cc:16` | GameMode_CopyEntityName |
| `0x00c69160` | kClampReach | `features\gameplay\keyboard_move.cc:19` | Move_ClampToReachableCell |
| `0x00c69a40` | kPickGroundCellAddr | `features\gameplay\quick_cast.cc:22` |  |
| `0x00c6aa80` | kWorldToTile | `features\gameplay\keyboard_move.cc:17`<br>`features\windows\entity_inspector.cc:50` | MapCoord_WorldToTileAndSub |
| `0x00c6b1e0` | kOwnsItemById | `features\windows\item_desc_window.cc:178` | Inventory_OwnsItemById |
| `0x00c6b290` | kOwnHasResurrectionTokenAddr | `features\windows\game_menu.cc:75` |  |
| `0x00c6cf80` | kCellValid | `features\gameplay\keyboard_move.cc:18` | Cell_IsMoveTargetValid |
| `0x00c6d9d0` | kArrowSpawn | `features\fx\wand_bolt.cc:14` | Arrow_SpawnProjectileToTarget |
| `0x00c6e990` | kShowEntityContextMenu | `features\windows\entity_context_menu.cc:37` |  |
| `0x00c74a80` | (littéral) | `ragnarok\configuration.h:112` |  |
| `0x00c74fc2` | kDrawMiniMapCall | `features\overlays\minimap.cc:122` | call GameMode_DrawMiniMap |
| `0x00c74fc7` | kDrawMiniMapAfter | `features\overlays\minimap.cc:123` | l'instruction suivante |
| `0x00c74fd6` | kRenderWindowsCall | `features\gameplay\afk_screen.cc:45` | call UIWindowMgr_RenderWindows |
| `0x00c74fdb` | kRenderWindowsAfter | `features\gameplay\afk_screen.cc:46` | l'instruction suivante |
| `0x00c753a0` | kPostActorClickAction | `features\overlays\target_frame.cc:42`<br>`features\windows\entity_context_menu.cc:98`<br>`ragnarok\configuration.h:120` | __thiscall(gm, aid, flag) |
| `0x00c756a0` | kRouteHoverAndClick | `features\windows\entity_context_menu.cc:46` |  |
| `0x00c7a7eb` | kPartyPrefixByte | `features\windows\chat_window.cc:785` |  |
| `0x00c7a822` | kGuildPrefixByte | `features\windows\chat_window.cc:786` |  |
| `0x00c7a89a` | kAllyPrefixByte | `features\windows\chat_window.cc:787` |  |
| `0x00c82340` | kCamClamp | `ragnarok\camera.cc:11` | Camera_ApplyViewDistanceClamp |
| `0x00c86740` | (littéral) | `ragnarok\configuration.h:73`<br>`ragnarok\configuration.h:113` |  |
| `0x00c8c8c7` | (littéral) | `features\overlays\entity_names.cc:151` |  |
| `0x00c93cb0` | kHitTestFn | `features\overlays\status_icon_bar.cc:114` |  |
| `0x00c9df00` | (littéral) | `ragnarok\configuration.h:92` |  |
| `0x00c9e1dd` | (littéral) | `ragnarok\configuration.h:97` |  |
| `0x00ca0c5d` | (littéral) | `bourgeon.cc:184` |  |
| `0x00caa2e0` | (littéral) | `ragnarok\configuration.h:89` |  |
| `0x00cb13c6` | (littéral) | `bourgeon.cc:184` |  |
| `0x00cf8b10` | kRecvDeleteResult | `features\windows\rodex_window.cc:291` |  |
| `0x00cfd0c0` | kRecvAckReadRodex | `features\windows\rodex_window.cc:235` | Recv_ZC_AckReadRodex_0x0B63 |
| `0x00d00010` | kApplyCheckNameAck | `features\windows\rodex_window.cc:337` | __stdcall, retn 0x10 |
| `0x00d21210` | (littéral) | `features\windows\char_select.cc:258`<br>`features\windows\char_select.cc:261` |  |
| `0x00d272e0` | (littéral) | `ragnarok\configuration.h:110` |  |
| `0x00d36ee4` | kActFrameCall | `features\fx\weapon_dual_sprites.cc:20` | CALL Act_GetFrame (E8 rel32) |
| `0x00d3dc90` | kRebuildBodyPalettePath | `features\fx\palette_inject.cc:43` |  |
| `0x00d3ded0` | kRebuildHeadPalette | `features\fx\palette_inject.cc:61` |  |
| `0x00d403a0` | kBuildWeaponLayers | `features\fx\weapon_dual_sprites.cc:19` | CActorSprite_BuildWeaponLayers |
| `0x00d41c5f` | (littéral) | `features\fx\wand_bolt.cc:38` |  |
| `0x00d4245d` | (littéral) | `features\fx\wand_bolt.cc:36` |  |
| `0x00d425ce` | (littéral) | `features\fx\wand_bolt.cc:37` |  |
| `0x00d43400` | (littéral) | `features\windows\char_diagnostics.cc:1241` |  |
| `0x00d54c40` | kShopAddOrMerge | `features\windows\vending_window.cc:373` |  |
| `0x00d54d80` | kAvailAddOrMerge | `features\windows\vending_window.cc:376` |  |
| `0x00d54ea0` | kBasketAddOrMerge | `features\windows\vending_window.cc:384` | (session, rec) — 2 args ! |
| `0x00d55f80` | kShopCartResetAll | `features\windows\npc_shop_window.cc:210` |  |
| `0x00d56300` | kVendingBasketClear | `features\windows\vending_window.cc:205` | __thiscall(&session) |
| `0x00d57780` | (littéral) | `ragnarok\configuration.h:80` |  |
| `0x00d57a30` | kInvDecrease | `features\windows\trade_window.cc:62` |  |
| `0x00d57ac0` | kShopConsume | `features\windows\vending_window.cc:377` |  |
| `0x00d57c60` | kAvailConsume | `features\windows\vending_window.cc:374` |  |
| `0x00d57e40` | kBasketRemove | `features\windows\vending_window.cc:385` | (session, rec) — 2 args ! |
| `0x00d5a720` | kBuildIconPath, kEngBuildPath | `features\minigames\roggle.cc:208`<br>`features\overlays\skill_bar.cc:582`<br>`features\patches\chat.cc:102`<br>`ui\icon_cache.cc:15` | __fastcall(session, 0, idstr, outbuf, 0) |
| `0x00d5a980` | kGetSkillInfo | `features\overlays\skill_bar.cc:69` | SkillMgr_GetSkillInfo(mgr,out,id,gate) ; out+4!=0 => trouvé |
| `0x00d5b580` | kGetJob | `features\overlays\basic_info.cc:135`<br>`features\overlays\basic_info.cc:1154`<br>`features\overlays\basic_info.cc:1367`<br>`features\windows\character_sheet.cc:838`<br>`features\windows\character_sheet.cc:2828`<br>`features\windows\palette_editor.cc:40` |  |
| `0x00d5bb40` | kJobDisplayName, kJobNameAddr, kJobResName | `features\overlays\basic_info.cc:137`<br>`features\windows\char_diagnostics.cc:96`<br>`features\windows\char_select.cc:398`<br>`features\windows\character_sheet.cc:840`<br>`features\windows\character_sheet.cc:853`<br>`features\windows\entity_inspector.cc:47`<br>… +3 | __thiscall(ctx, classId, sex) |
| `0x00d5bcf0` | ⚓ kMapDisplayNameAddr | `ragnarok\globals.h:255` |  |
| `0x00d5bea0` | kShopGetAt | `features\windows\vending_window.cc:357` |  |
| `0x00d5bf40` | kShopAmountBySrc | `features\windows\vending_window.cc:403` |  |
| `0x00d5c160` | kAvailGetAt | `features\windows\vending_window.cc:356` |  |
| `0x00d5c200` | kAvailAmountBySrc | `features\windows\vending_window.cc:402` |  |
| `0x00d5c580` | kBasketGetAt | `features\windows\vending_window.cc:383` | (session, out_rec, i) |
| `0x00d5cf50` | kPartyMemberCount | `features\windows\chat_window.cc:129`<br>`ragnarok\emotion_hotkey.cc:55` | __thiscall(ctxKey) |
| `0x00d5e1d0` | kShieldAct | `ragnarok\held_sprites.cc:34` |  |
| `0x00d5e240` | kShieldSpr | `ragnarok\held_sprites.cc:35` |  |
| `0x00d5e3c0` | kSkillGetUseLevel | `features\windows\character_sheet.cc:1031` | __thiscall(SESSION, id) — même tableau |
| `0x00d5e590` | kLookupSlashCmd | `features\windows\chat_window.cc:125`<br>`ragnarok\configuration.h:81` | __thiscall(ctx, texte, &id, args[3]) |
| `0x00d715f0` | kFriendListHasName | `features\windows\chat_window.cc:196` |  |
| `0x00d71ec0` | kFnIsNpcOrPortalId | `ui\mob_sprite.cc:59` |  |
| `0x00d78970` | kOptionSaveAddr | `ragnarok\game_settings.cc:208` |  |
| `0x00d7f1a0` | kCmdHandlerMap | `features\windows\chat_window.cc:118` | __thiscall(ctxKey, texte) |
| `0x00d7f380` | kMailReturnAttachments | `features\windows\rodex_window.cc:65` |  |
| `0x00d7f480` | kMailClearAttachSlots | `features\windows\rodex_window.cc:66` |  |
| `0x00d7fa90` | kGetInvItemAddr, kSkillEntryFill | `features\overlays\skill_bar.cc:257`<br>`features\overlays\skill_bar.cc:600`<br>`features\overlays\skill_bar.cc:671`<br>`features\windows\character_sheet.cc:138` | __stdcall(out, id) |
| `0x00d7fd30` | (littéral) | `features\windows\char_select.cc:994` |  |
| `0x00d7fe40` | kOwnGetCharName | `features\windows\chat_window.cc:583` | __thiscall(ctxKey) -> char* |
| `0x00d80140` | kMailAttachCountFn | `features\windows\rodex_window.cc:80` | __thiscall(session) -> int |
| `0x00d806a0` | kActorFindByGid | `features\overlays\target_frame.cc:35`<br>`features\windows\entity_inspector.cc:30`<br>`ragnarok\pet.cc:102` | __stdcall(gid) |
| `0x00d80810` | kSkillGetAt | `ragnarok\homunculus.cc:65` |  |
| `0x00d80950` | kGetHotKey, kGetHotKeyAddr | `features\overlays\skill_bar.cc:455`<br>`ragnarok\user_hotkey.cc:30` |  |
| `0x00d81680` | kGetOriginalListSizeAddr | `ragnarok\user_hotkey.cc:23` |  |
| `0x00d823f0` | kEggToMobAddr | `ragnarok\pet.cc:103` |  |
| `0x00d83b20` | kRowToCommandIndexAddr | `ragnarok\user_hotkey.cc:26` |  |
| `0x00d84760` | kGetSex | `features\overlays\basic_info.cc:147`<br>`features\windows\palette_editor.cc:37` | GetSex(session) |
| `0x00d87380` | kGetEFSTImg | `features\overlays\status_icon_bar.cc:47` | __thiscall(session,id,layer) -> const char* |
| `0x00d89ed0` | kTitleGetStr | `features\windows\character_sheet.cc:2142` |  |
| `0x00d8a010` | kWeaponSpr | `ragnarok\held_sprites.cc:25` |  |
| `0x00d8a080` | kShieldGenSpr | `ragnarok\held_sprites.cc:38` |  |
| `0x00d8a0f0` | kShieldGenAct | `ragnarok\held_sprites.cc:39` |  |
| `0x00d8a160` | kWeaponAct | `ragnarok\held_sprites.cc:26` |  |
| `0x00d8a1d0` | kItemIdToClass, kItemIdToWeaponClass | `features\fx\wand_bolt.cc:40`<br>`features\fx\weapon_dual_sprites.cc:23` | Weapon_ItemIdToWeaponClass (__stdcall, retn 4) |
| `0x00d8e6c0` | kIsBerserkActive | `features\patches\berserk_chat_unlock.cc:19` |  |
| `0x00d8eba0` | kStatusEffectListHasAddr | `features\windows\game_menu.cc:78` |  |
| `0x00d96c20` | kSetShortCut | `features\overlays\skill_bar.cc:56` | SkillMgr_SetShortCutSlot |
| `0x00d99150` | kJobResolveBodyClass | `ui\sprite_path.cc:15` |  |
| `0x00d99860` | kAdoptionEligible | `features\windows\entity_context_menu.cc:132` | __stdcall(aid) -> bool |
| `0x00d99ca0` | kJobGetMaxBaseLevel | `features\overlays\basic_info.cc:2770` | __stdcall(jobId) |
| `0x00d99d30` | kJobGetMaxJobLevel | `features\overlays\basic_info.cc:2771` | __stdcall(jobId) |
| `0x00d9a960` | kCntCostume | `features\windows\inventory_viewer.cc:55` | __fastcall(session) : nb items COSTUME distincts (10 slots @+0x2b34) |
| `0x00d9aa70` | kCntEquipped | `features\windows\inventory_viewer.cc:54` | __fastcall(session) : nb items ÉQUIPÉS distincts (10 slots @+0x17d4) |
| `0x00d9cf80` | kJobIsDoram | `ui\sprite_path.cc:35` |  |
| `0x00d9d220` | kIsHostileOrSpecial | `features\windows\entity_context_menu.cc:136` | __stdcall(aid, job) -> bool |
| `0x00da8f90` | kSetItemSlot | `features\overlays\skill_bar.cc:256` |  |
| `0x00dabb10` | kArrowEffectUpdate | `features\fx\wand_bolt.cc:53` |  |
| `0x00db01b0` | kArrowEffectOnMsg | `features\fx\wand_bolt.cc:54` |  |
| `0x00dbbc4f` | ⚓ kGameOperatorNewAddr | `ragnarok\globals.h:197` | __cdecl(size) -> void*, jamais nul |
| `0x00dbbc7f` | ⚓ kClientOperatorDelete, kGameOperatorDeleteAddr | `features\patches\damage_name_fix.cc:19`<br>`ragnarok\globals.h:198` | __cdecl(ptr) |
| `0x00e8ddb6` | kWhisperRecvRgb | `features\windows\chat_window.cc:576` | reçu : bleu-gris pâle |
| `0x00fd5d60` | kNativeAbsMaskVa | `features\fx\hat_effect_depth.cc:35` |  |
| `0x00fd6ae4` | kNativeDepthQuantumVa | `features\fx\hat_effect_depth.cc:34` |  |
| `0x00ff0000` | (littéral) | `features\fx\ez_effect_capture.cc:417`<br>`imgui\imgui_impl_dx7.cc:34`<br>`imgui\imgui_impl_dx7.cc:230`<br>`imgui\imgui_impl_dx7.cc:330` |  |
| `0x00ff00ff` | kEmblemClear | `features\minigames\roggle.cc:269`<br>`features\windows\character_sheet.cc:1645` | magenta pur = transparent en jeu |
| `0x00ffaa00` | kOwnChatRgb | `features\overlays\dps_meter.cc:194`<br>`features\windows\entity_context_menu.cc:59` |  |
| `0x00ffd070` | (littéral) | `features\windows\chat_window.cc:1642` |  |
| `0x00ffe060` | (littéral) | `features\overlays\quest_tracker.h:18` | hunt progress lines ("mob ( x / y )") |
| `0x00ffff00` | (littéral) | `features\windows\character_sheet.cc:1496` |  |
| `0x00ffffff` | kDefaultRgb | `features\minigames\roggle.cc:269`<br>`features\minigames\rojeweled.cc:402`<br>`features\minigames\rojeweled.cc:428`<br>`features\overlays\chat_balloon.cc:389`<br>`features\overlays\chat_balloon.h:142`<br>`features\overlays\entity_names.cc:150`<br>… +10 | couleur de repli (ZC_NPC_CHAT, Talkie Box) |
| `0x01011dbc` | kPaletteResVtable | `features\fx\palette_inject.cc:78` |  |
| `0x01013e88` | kStrCanvasCy | `features\overlays\basic_info.cc:461` | DAT_01013e88 (soustrait de workBuf[1]) |
| `0x0101ca18` | kCashVTable | `features\windows\cashshop_window.cc:35` |  |
| `0x0101d424` | kCharSelWndVtbl | `features\windows\char_select.cc:121` | garde anti-pointeur périmé |
| `0x01021b30` | kWriteVTable | `features\windows\rodex_window.cc:49` |  |
| `0x01021e9e` | kIcoZenyPath | `features\windows\rodex_window.cc:392` | « \…\rodexsystem\renewal\icon_zeny.bmp » |
| `0x01021fbc` | kReadVTable | `features\windows\rodex_window.cc:37` |  |
| `0x01022170` | kInboxVTable | `features\windows\rodex_window.cc:36` | vérifiée live (g_RodexInboxWnd+0) |
| `0x01022a8e` | kIcoBothPath | `features\windows\rodex_window.cc:394` | « …\icon_zeny_n_item.bmp » |
| `0x01022ad6` | kIcoItemPath | `features\windows\rodex_window.cc:393` | « …\icon_item.bmp » |
| `0x01022f5c` | kStrCanvasCx | `features\overlays\basic_info.cc:460` | DAT_01022f5c (soustrait de workBuf[0]) |
| `0x010265e8` | kStyleshopPath | `features\windows\inventory_viewer.cc:674` | "유저인터페이스\styleshop\btn_buy_out.bmp" |
| `0x010277b8` | kEmotionPathAddr | `ui\game_emotes.cc:26` |  |
| `0x01028200` | kGridDrawSlot | `features\overlays\menu_icons.cc:47` | UIMenuIconWnd vtbl +0x50 |
| `0x0102a260` | kResizeBtnVtable | `features\patches\chat.cc:536` | UIResizeButton vtable (PTR_FUN_0102a260) |
| `0x0102bd78` | kUIPlayerGageVtable | `features\overlays\basic_info.cc:2650` |  |
| `0x0102dd94` | kTabDrawSlot | `features\patches\inventory_tweaks.cc:156` | tab control vtable +0x50 slot (static .rdata) |
| `0x01030168` | kVtblUILoginWnd | `features\systems\native_login.cc:17` | garde de validité de la fenêtre |
| `0x01030fd4` | kBankVTable | `features\windows\bank_window.cc:30` |  |
| `0x01031264` | kAcctClassNormal | `features\systems\native_login.cc:20` |  |
| `0x0103181c` | kFmtSpr, kFmtSprMonster | `features\minigames\rojeweled.cc:62`<br>`ui\mob_sprite.cc:51` | "몬스터\\%s.spr" (CP949) |
| `0x010322d0` | kMsgSlot | `features\patches\equip_tweaks.cc:109` | UIEquipWnd vtable +0x94 (message handler slot) |
| `0x010323ec` | kParamCompareVTable | `features\windows\npc_shop_window.cc:67` | UIItemParamChangeDisplayWnd |
| `0x0103274c` | kVTable | `features\overlays\item_obtain_toast.cc:36` | UINotifyItemObtainWnd |
| `0x01032a24` | kDrawSlot | `features\patches\status_tweaks.cc:51`<br>`features\patches\status_tweaks.cc:344` | UIStatusWnd vtable +0x50 (DrawContent) |
| `0x01032a68` | kMsgSlot | `features\patches\status_tweaks.cc:54` | UIStatusWnd vtable +0x94 (message handler slot) |
| `0x01032aac` | kItemVTable | `features\windows\item_desc_window.cc:52` | classe 0xc |
| `0x01032c5c` | kCompareVTable | `features\windows\item_desc_window.cc:64` | classe 0xea |
| `0x01032e0c` | kSkillVTable | `features\windows\item_desc_window.cc:53` | classe 0x2e |
| `0x01033754` | kAcceptVTable | `features\windows\trade_window.cc:43` | popup requête (best-effort) |
| `0x010345ac` | kVTableMakingArrow | `features\windows\make_item_window.cc:45` |  |
| `0x0103517c` | kBookVTable | `features\windows\item_desc_window.cc:188` |  |
| `0x010357b8` | kBtnbarPath, kUiPrefixPath | `features\patches\inventory_tweaks.cc:51`<br>`features\windows\cart_viewer.cc:256`<br>`features\windows\inventory_viewer.cc:668`<br>`features\windows\rodex_window.cc:391`<br>`features\windows\storage_window.cc:228` | "유저인터페이스\basic_interface\btnbar_left.bmp" (bottom-frame height) |
| `0x010361b4` | kBgNormalPath | `features\patches\status_tweaks.cc:71` | "...\statuswnd\w_statwin_bg.bmp" |
| `0x01036648` | kCardBmpPrefix | `features\windows\item_desc_window.cc:97` | "유저인터페이스\cardBmp\" (CP949, null-term) |
| `0x010371c0` | kRectGuardAddr | `features\patches\status_tweaks.cc:136`<br>`features\patches\status_tweaks.cc:147`<br>`features\patches\status_tweaks.cc:357` | ATK  0xc2e  right r0 (full) |
| `0x010371c4` | (littéral) | `features\patches\status_tweaks.cc:136` | ATK  0xc2e  right r0 (full) |
| `0x010371c8` | (littéral) | `features\patches\status_tweaks.cc:137` | MATK 0xc34  right r3 (full) |
| `0x010371cc` | (littéral) | `features\patches\status_tweaks.cc:137` | MATK 0xc34  right r3 (full) |
| `0x010371d0` | (littéral) | `features\patches\status_tweaks.cc:142` | DEF  0xc2f  right r2 colA |
| `0x010371d4` | (littéral) | `features\patches\status_tweaks.cc:142` | DEF  0xc2f  right r2 colA |
| `0x010371d8` | (littéral) | `features\patches\status_tweaks.cc:143` | MDEF 0xc35  right r2 colB |
| `0x010371dc` | (littéral) | `features\patches\status_tweaks.cc:143` | MDEF 0xc35  right r2 colB |
| `0x010371e0` | (littéral) | `features\patches\status_tweaks.cc:130` | STR  0xc28  left r0 |
| `0x010371e4` | (littéral) | `features\patches\status_tweaks.cc:130` | STR  0xc28  left r0 |
| `0x010371e8` | (littéral) | `features\patches\status_tweaks.cc:131` | AGI  0xc29  left r1 |
| `0x010371ec` | (littéral) | `features\patches\status_tweaks.cc:131` | AGI  0xc29  left r1 |
| `0x01037220` | (littéral) | `features\patches\status_tweaks.cc:138` | HIT  0xc30  right r4 (full) |
| `0x01037224` | (littéral) | `features\patches\status_tweaks.cc:138` | HIT  0xc30  right r4 (full) |
| `0x01037228` | (littéral) | `features\patches\status_tweaks.cc:139` | CRI  0xc31  right r5 (full) |
| `0x0103722c` | (littéral) | `features\patches\status_tweaks.cc:139` | CRI  0xc31  right r5 (full) |
| `0x01037230` | (littéral) | `features\patches\status_tweaks.cc:144` | FLEE 0xc36  right r1 colB |
| `0x01037234` | (littéral) | `features\patches\status_tweaks.cc:144` | FLEE 0xc36  right r1 colB |
| `0x01037238` | (littéral) | `features\patches\status_tweaks.cc:145` | ASPD 0xc37  right r1 colA |
| `0x0103723c` | (littéral) | `features\patches\status_tweaks.cc:145` | ASPD 0xc37  right r1 colA |
| `0x01037240` | (littéral) | `features\patches\status_tweaks.cc:132` | VIT  0xc2a  left r2 |
| `0x01037244` | (littéral) | `features\patches\status_tweaks.cc:132` | VIT  0xc2a  left r2 |
| `0x01037248` | (littéral) | `features\patches\status_tweaks.cc:133` | INT  0xc2b  left r3 |
| `0x0103724c` | (littéral) | `features\patches\status_tweaks.cc:133` | INT  0xc2b  left r3 |
| `0x010372a0` | (littéral) | `features\patches\status_tweaks.cc:140` | POINT0xc33  left  r6 (Status Point) |
| `0x010372a4` | (littéral) | `features\patches\status_tweaks.cc:140` | POINT0xc33  left  r6 (Status Point) |
| `0x010372a8` | (littéral) | `features\patches\status_tweaks.cc:141` | GUILD0xc32  right r6 |
| `0x010372ac` | (littéral) | `features\patches\status_tweaks.cc:141` | GUILD0xc32  right r6 |
| `0x010372b0` | (littéral) | `features\patches\status_tweaks.cc:134` | DEX  0xc2c  left r4 |
| `0x010372b4` | (littéral) | `features\patches\status_tweaks.cc:134` | DEX  0xc2c  left r4 |
| `0x010372b8` | (littéral) | `features\patches\status_tweaks.cc:135` | LUK  0xc2d  left r5 |
| `0x010372bc` | (littéral) | `features\patches\status_tweaks.cc:135` | LUK  0xc2d  left r5 |
| `0x01037ea8` | (littéral) | `features\patches\chat_bg.cc:61` |  |
| `0x01037f80` | (littéral) | `features\patches\chat_bg.cc:48` |  |
| `0x010384a0` | (littéral) | `features\fx\screen_fx.cc:398`<br>`features\fx\screen_fx.cc:399`<br>`features\fx\screen_fx.cc:496` |  |
| `0x0103ca40` | ⚓ kStorageWndVTable | `ragnarok\uiwnd.h:102` |  |
| `0x0103d460` | ⚓ kInventoryWndVTable | `ragnarok\uiwnd.h:100` |  |
| `0x0103d4b0` | kDrawSlot | `features\patches\inventory_tweaks.cc:35`<br>`features\patches\inventory_tweaks.cc:706` | UIItemWnd vtable +0x50 |
| `0x0103d4f4` | kMsgSlot | `features\patches\inventory_tweaks.cc:166` | inventory vtable +0x94 (message handler slot) |
| `0x0103d538` | kCartVTable | `features\windows\cart_viewer.cc:48`<br>`features\windows\inventory_viewer.cc:133`<br>`features\windows\storage_window.cc:165` |  |
| `0x0103dad4` | kIconNumPath | `features\windows\cart_viewer.cc:258`<br>`features\windows\inventory_viewer.cc:670` | …\inventory\icon_num.bmp |
| `0x0103db00` | kIconPath, kIconWeightPath | `features\patches\inventory_tweaks.cc:50`<br>`features\windows\cart_viewer.cc:257`<br>`features\windows\inventory_viewer.cc:669` | "유저인터페이스\inventory\icon_weight.bmp" |
| `0x0103e35c` | kBIMsgSlot | `features\overlays\basic_info.cc:2692` | vtable+0x94 OnMsg slot |
| `0x0103ec50` | kVTableMakeTarget | `features\windows\make_item_window.cc:47` |  |
| `0x0103ee00` | kRefineVTable | `features\windows\weapon_refine_window.cc:38` |  |
| `0x0103eed8` | kVTableMakeProcess | `features\windows\make_item_window.cc:53` | ⏱ confirmé sur 2 instances |
| `0x010457d8` | kExchangeVTable | `features\windows\trade_window.cc:41` | CUIExchangeUI (fenêtre d'échange) |
| `0x01047d7c` | (littéral) | `features\fx\screen_fx.cc:400`<br>`features\fx\screen_fx.cc:401` |  |
| `0x0104dee4` | kCameraVtable | `features\gameplay\keyboard_move.cc:21`<br>`ragnarok\camera.cc:9` | g_CCamera_vtable (valide pCam) |
| `0x010758d8` | kCEZ2STRVtbl | `features\fx\ez_effect_capture.cc:45` | CEZ2STREffect (.str name-based) -> à EXCLURE |
| `0x01088a18` | kFmtBodyTail | `ui\sprite_path.cc:21` |  |
| `0x01088a6c` | kFmtHead | `ui\head_icon.cc:28` |  |
| `0x01088a80` | kFmtHeadgearDoram | `ui\doll.cc:152` |  |
| `0x01088a9c` | kFmtHeadgear | `ui\doll.cc:141` |  |
| `0x01088b88` | kFmtGarmentFlat | `ui\doll.cc:198` | sprite\로브\%s\%s.%s |
| `0x01088bbc` | kFmtGarmentJob | `ui\doll.cc:199` | sprite\로브\%s\%s\%s_%s.%s |
| `0x01088c2c` | kRaceDoramAddr | `ui\sprite_path.cc:42` | 도람족 |
| `0x01088c34` | kRaceHumanAddr | `ui\sprite_path.cc:43` | 인간족 |
| `0x01088c48` | kEzChildVtbl | `features\fx\ez_effect_capture.cc:29` | vtable du nœud EZ enfant (celui que Draw dessine) |
| `0x0108f960` | kFmtSprNpc | `ui\mob_sprite.cc:52` | NPC\%s.spr |
| `0x010904b8` | kGameModeVtable | `features\gameplay\quick_cast.cc:47`<br>`ragnarok\configuration.h:74` |  |
| `0x01091520` | (littéral) | `features\fx\screen_fx.cc:437` |  |
| `0x01091528` | (littéral) | `features\fx\screen_fx.cc:438` |  |
| `0x010932f0` | kVtblCLoginMode | `features\systems\native_login.cc:16` | garde de mode |
| `0x010940d0` | kFmtSprHuman | `ui\mob_sprite.cc:53` | 인간족\몸통\%s.spr |
| `0x010940f8` | kFmtSprHomun | `ui\mob_sprite.cc:54` | homun\%s.spr |
| `0x0109c3e8` | kArrowEffectVtable | `features\fx\wand_bolt.cc:52` |  |
| `0x011e40d4` | kMouseScreenX, kMouseScreenXAddr | `features\gameplay\quick_cast.cc:38`<br>`features\windows\char_diagnostics.cc:464`<br>`ragnarok\ragnarok_client.cc:180` |  |
| `0x011e40d8` | kMouseScreenY, kMouseScreenYAddr | `features\gameplay\quick_cast.cc:39`<br>`features\windows\char_diagnostics.cc:465`<br>`ragnarok\ragnarok_client.cc:181` |  |
| `0x011e40e4` | kMouseLButtonState | `features\windows\entity_context_menu.cc:65` |  |
| `0x011e40e8` | kMouseRButtonState | `features\windows\entity_context_menu.cc:66` |  |
| `0x011fe3a8` | kSkinMgrAddr | `ragnarok\game_settings.cc:230` |  |
| `0x011fe3c4` | kSkinCurrentAddr | `ragnarok\game_settings.cc:231` | mgr+0x1C |
| `0x011fe3d4` | kSkinVecBeginAddr | `ragnarok\game_settings.cc:232` | mgr+0x2C |
| `0x011fe3d8` | kSkinVecEndAddr | `ragnarok\game_settings.cc:233` | mgr+0x30 |
| `0x0120451c` | kForbidden1 | `ragnarok\pet.cc:112` |  |
| `0x01204534` | kForbidden2 | `ragnarok\pet.cc:113` |  |
| `0x01213338` | ⚓ kModeMgr, kModeMgrAddr | `features\patches\damage_name_fix.cc:18`<br>`ragnarok\globals.h:49` |  |
| `0x0121333c` | ⚓ kActiveModePtr, kCurrentModePtr, kCurrentModePtrAddr | `features\windows\chat_window.cc:126`<br>`ragnarok\game_settings.cc:222`<br>`ragnarok\globals.h:55` | CMode* courant |
| `0x012135f0` | kNameplateQuadTreeAddr, kPickQuadTree | `features\gameplay\quick_cast.cc:28`<br>`features\windows\char_diagnostics.cc:462` |  |
| `0x01228f60` | kMotionSpeedCap | `features\windows\char_diagnostics.cc:81` | g_MotionSpeedCapMs |
| `0x012291c0` | kCamZoomMaxOutdoor | `features\fx\screen_fx.cc:417`<br>`features\gameplay\afk_screen.cc:59` | g_cam_zoomMaxOutdoor |
| `0x012291c4` | (littéral) | `features\fx\screen_fx.cc:418` | g_cam_zoomMaxIndoor |
| `0x0122b3d8` | kTextureDownscaleAddr | `ragnarok\game_settings.cc:135` |  |
| `0x01234567` | (littéral) | `features\overlays\login_parade.cc:136` |  |
| `0x012515f8` | ⚓ kContextPtr | `ragnarok\render.h:23` |  |
| `0x012515fc` | kFlagMapAddr | `ragnarok\game_settings.cc:55` |  |
| `0x0125161c` | kSpriteCache, kSpriteTexCacheAddr | `features\overlays\status_icon_bar.cc:50`<br>`ragnarok\game_settings.cc:166` | &DAT_0125161c (sprite-ref cache) |
| `0x01251668` | kSaveJsonSelf | `ragnarok\emotion_hotkey.cc:44` |  |
| `0x012517b8` | kEmblemDataMgrPtr | `features\windows\character_sheet.cc:1555` | *ptr = CEmblemDataMgr |
| `0x01251824` | kChatBlockListPtr | `features\windows\entity_context_menu.cc:118` |  |
| `0x01253d0c` | kSoundMgrAddr, kSoundMgrPtr | `features\minigames\roggle.cc:235`<br>`features\minigames\rojeweled.cc:90`<br>`features\overlays\login_parade.cc:27`<br>`features\windows\monster_info_window.cc:169`<br>`ragnarok\game_settings.cc:87` | *ptr = SoundMgr (déréf 1×) |
| `0x01254d70` | kCGuildMgrPtr | `features\windows\character_sheet.cc:1232` | *ptr = g_CGuildMgr (CGuildEmblemMgr) |
| `0x01254d8c` | kEvolutionMgrPtr | `ragnarok\pet.cc:49` |  |
| `0x01254d90` | kQuestMgrPtr | `features\overlays\quest_tracker.cc:27` | void** -> quest manager |
| `0x01255108` | kProbDbPtr | `features\windows\item_desc_window.cc:164` | ptr vers le mgr (lazy-new) |
| `0x0125510c` | ⚓ kEnsureCachePtr | `ragnarok\item_db.h:33` |  |
| `0x01255130` | ⚓ kTableAddr | `ragnarok\item_db.h:25` | la map |
| `0x01255138` | ⚓ kNilAddr | `ragnarok\item_db.h:26` | sentinelle de l'arbre |
| `0x01255180` | kCardNilRecord | `features\windows\item_desc_window.cc:103` |  |
| `0x0131ecdc` | kRodexMgrPtr | `features\windows\rodex_window.cc:116` |  |
| `0x0131ee7c` | kMgrPtrAddr | `ragnarok\game_settings.cc:18` |  |
| `0x0131f4e8` | ⚓ kUIWindowMgr, kUIWindowMgrAddr, kUiWindowMgr | `features\overlays\chat_balloon.cc:49`<br>`features\windows\entity_context_menu.cc:104`<br>`features\windows\navigation_window.cc:67`<br>`ragnarok\uiwnd.h:25` | g_UIWindowMgr (l'OBJET, pas un pointeur vers lui) |
| `0x0131f50e` | kBattleModeFlag, kChangeChatModeAddr | `features\windows\chat_window.cc:497`<br>`features\windows\hotkey_settings.cc:60` |  |
| `0x0131f510` | kDetachedChatTree | `features\patches\chat.cc:774` | std::set<UIChatWnd*> head node |
| `0x0131f6b0` | kNewChatWndPtr | `features\windows\chat_window.cc:59`<br>`features\windows\inventory_viewer.cc:554`<br>`features\windows\navigation_window.cc:68` | g_pNewChatWnd |
| `0x0131f6b4` | kPLoginWnd | `features\systems\native_login.cc:18` | UILoginWnd* (== mgr+0x1cc, FindWindow id 3) |
| `0x0131f6bc` | ⚓ kInventoryWndSlot | `ragnarok\uiwnd.h:99` | inventaire, id 8 |
| `0x0131f6c4` | kBasicInfoPtr | `features\overlays\basic_info.cc:2966` | 0x0131f4e8 + 0x1dc |
| `0x0131f700` | kItemDescWndGlobalPtr, kItemWndSlot | `features\moonlight_ui\moonlight_ui.h:345`<br>`features\windows\item_desc_window.cc:48` | mgr+0x218 : ITEM desc (classe 0xc) |
| `0x0131f708` | kCompareWndSlot | `features\windows\item_desc_window.cc:63` | mgr+0x220 : COMPARE desc (id 0xea) |
| `0x0131f718` | kSkillWndSlot | `features\windows\item_desc_window.cc:49` | mgr+0x230 : SKILL desc (classe 0x2e) |
| `0x0131f764` | kNoPathFlag | `features\gameplay\keyboard_move.cc:20` | != 0 -> le natif passe en msg 0x10 |
| `0x0131f770` | ⚓ kStorageWndPtr, kStorageWndSlot | `features\windows\entity_context_menu.cc:75`<br>`ragnarok\uiwnd.h:101` | storage NATIF ouvert |
| `0x0131f940` | kMailWriteWnd, kWriteWndPtr | `features\windows\rodex_window.cc:47`<br>`ragnarok\pet.cc:36` | g_MailWriteWnd (filet : doit rester nul) |
| `0x0131f9c4` | kPendingSendText | `features\windows\chat_window.cc:94` | std::string |
| `0x0136e6c8` | kVecBegin | `features\overlays\status_icon_bar.cc:51` | std::vector<StatusIcon>::begin (raw bytes) |
| `0x0136e6cc` | kVecEnd | `features\overlays\status_icon_bar.cc:52` | ::end |
| `0x0159b818` | ⚓ kClientCodePageAddr | `ragnarok\globals.h:188` |  |
| `0x0159b8a8` | kClientInfoXmlDoc | `features\systems\native_login.cc:60` | racine du document parsé |
| `0x0159c07c` | kClanStatePtr | `features\windows\chat_window.cc:130`<br>`ragnarok\emotion_hotkey.cc:54` | *(byte*)(*ptr + 0x5C) = clan |
| `0x0159c08c` | kTownInfoMgrPtr | `features\overlays\minimap.cc:368` | CTownInfoMgr* — à DÉRÉFÉRENCER |
| `0x0159c188` | kGuildBuf, kGuildObj | `features\patches\status_tweaks.cc:117`<br>`features\windows\character_sheet.cc:421` | std::string buffer/ptr union |
| `0x0159c198` | kGuildLen | `features\patches\status_tweaks.cc:115` | std::string _Mysize (0 == no guild) |
| `0x0159c19c` | kGuildCap | `features\patches\status_tweaks.cc:116` | std::string _Myres  (>=0x10 => heap ptr) |
| `0x0159c1a0` | kGuildMasterName | `features\windows\character_sheet.cc:437` | std::string : nom du maître |
| `0x0159c1b8` | kGuildNoticeSubj | `features\windows\character_sheet.cc:444` | std::string : sujet de l'annonce (ZC 0x016f) |
| `0x0159c1d0` | kGuildNoticeBody | `features\windows\character_sheet.cc:445` | std::string : corps de l'annonce |
| `0x0159c1e8` | kGuildLevel | `features\windows\character_sheet.cc:423` |  |
| `0x0159c1f0` | kGuildOnlineNum | `features\windows\character_sheet.cc:438` | connect_member (membres en ligne) |
| `0x0159c1f4` | kGuildMemberMax | `features\windows\character_sheet.cc:439` | max_member |
| `0x0159c1f8` | kGuildAvgLevel | `features\windows\character_sheet.cc:440` | niveau moyen |
| `0x0159c1fc` | kGuildManageLand | `features\windows\character_sheet.cc:441` | std::string : territoire (nb de forts) |
| `0x0159c214` | kGuildExp | `features\windows\character_sheet.cc:442` | exp courante |
| `0x0159c218` | kGuildNextExp | `features\windows\character_sheet.cc:443` | exp du niveau suivant |
| `0x0159c230` | kGuildIdAddr, kOwnGuildId | `features\windows\character_sheet.cc:425`<br>`features\windows\chat_window.cc:128`<br>`features\windows\entity_context_menu.cc:156`<br>`ragnarok\emotion_hotkey.cc:53` |  |
| `0x0159c23c` | kGuildIsMaster | `features\windows\character_sheet.cc:424`<br>`features\windows\entity_context_menu.cc:157` |  |
| `0x0159c26c` | kGuildRelHead | `features\windows\character_sheet.cc:446` | CGuild+0xe4 : liste alliés/ennemis |
| `0x0159d410` | kFileMgr, kFileMgrAddr | `ragnarok\grf_index.cc:14`<br>`ragnarok\msgstring_override.cc:142`<br>`ui\spr_act.cc:29` | g_FileMgr (l'OBJET) |
| `0x0159d68c` | (littéral) | `ragnarok\configuration.h:106` |  |
| `0x015beecc` | kReplayActive | `features\windows\entity_context_menu.cc:74`<br>`ragnarok\skill_cooldowns.cc:23` | lecture d'un replay |
| `0x015c3090` | kNaviMgr, kNavigation | `features\overlays\minimap.cc:422`<br>`features\windows\item_desc_window.cc:118`<br>`features\windows\navigation_window.cc:30` | &DAT_015c3090 (nav manager) |
| `0x015c5a24` | kSocketFd | `features\systems\native_login.cc:19` | g_RagConnection_SocketFd |
| `0x015e5b40` | kPickQuadMinSize | `features\patches\pick_quad_tweaks.cc:39`<br>`features\windows\char_diagnostics.cc:475` | acteurs (joueurs, monstres, pets) — facteur 40 |
| `0x015f81d0` | (littéral) | `features\patches\pick_quad_tweaks.cc:40` | PNJ de carte, portails — facteur 34 |
| `0x015f8262` | kSelectedSlot | `features\windows\char_select.cc:100` |  |
| `0x015fa3c0` | ⚓ kJobNameCtx, kOptionContextAddr, kSessionAddr, kUIWindowContextKey, kUiCtx | `features\windows\chat_window.cc:119`<br>`features\windows\entity_context_menu.cc:160`<br>`ragnarok\emotion_hotkey.cc:20`<br>`ragnarok\game_settings.cc:209`<br>`ragnarok\globals.h:26`<br>`ragnarok\homunculus.cc:67`<br>… +1 | la session |
| `0x015fa3cc` | kSkillBundle | `features\windows\character_sheet.cc:1027`<br>`features\windows\weapon_refine_window.cc:180`<br>`ragnarok\player_skills.cc:11` | CPlayerSkillBundle (session+0x0C) |
| `0x015fa3e0` | kSkillFlatList | `features\windows\character_sheet.cc:1028`<br>`features\windows\weapon_refine_window.cc:181`<br>`ragnarok\player_skills.cc:12` | bundle+0x14 : l'onglet « divers » |
| `0x015fa424` | kSkillHead | `ragnarok\homunculus.cc:41` |  |
| `0x015fa428` | kSkillSize | `ragnarok\homunculus.cc:42` |  |
| `0x015fa850` | (littéral) | `features\overlays\skill_bar.cc:179` |  |
| `0x015fa94c` | (littéral) | `features\overlays\skill_bar.cc:180` |  |
| `0x015faadc` | kChannelRegistryAddr | `features\windows\chat_window.cc:79` |  |
| `0x015faae4` | kDetachedRegistryAddr | `features\windows\chat_window.cc:80` |  |
| `0x015fb23c` | kAccountSex | `features\windows\char_select.cc:211` |  |
| `0x015fb278` | kHair | `features\overlays\basic_info.cc:148`<br>`features\windows\palette_editor.cc:74` | style de coiffure |
| `0x015fb28c` | kClothesCol | `features\overlays\basic_info.cc:149`<br>`features\windows\palette_editor.cc:75` | palette de vêtements |
| `0x015fb290` | kHairCol | `features\overlays\basic_info.cc:150`<br>`features\windows\palette_editor.cc:76` | palette de cheveux |
| `0x015fb294` | kHeadLowView | `features\overlays\basic_info.cc:155` | g_OwnLook_HeadBottomViewId |
| `0x015fb298` | kHeadTopView | `features\overlays\basic_info.cc:156` | g_OwnLook_HeadTopViewId |
| `0x015fb29c` | kHeadMidView | `features\overlays\basic_info.cc:157` | g_OwnLook_HeadMidViewId |
| `0x015fb2a0` | kGarmentView | `features\overlays\basic_info.cc:151` | g_OwnLook_GarmentRobeViewId |
| `0x015fb2a4` | kGarmentUseNewDrawOnTop | `ui\doll.cc:202` |  |
| `0x015fb2ac` | kBodyState | `features\windows\char_diagnostics.cc:52` | g_OwnState_BodyState (opt1) |
| `0x015fb2b0` | kHealthState | `features\windows\char_diagnostics.cc:53` | g_OwnState_HealthState (opt2) |
| `0x015fb2b4` | kEffectState | `features\windows\char_diagnostics.cc:54` | g_OwnState_EffectState (option) |
| `0x015fb2d4` | kCartNumItems | `features\windows\cart_viewer.cc:75` |  |
| `0x015fb2d8` | kCartMaxItems | `features\windows\cart_viewer.cc:76` |  |
| `0x015fb2dc` | kCartWeight | `features\windows\cart_viewer.cc:77` |  |
| `0x015fb2e0` | kCartMaxWeight | `features\windows\cart_viewer.cc:78` |  |
| `0x015fb2f8` | kFriendOptOpenFromStranger | `features\windows\chat_window.cc:189` |  |
| `0x015fb2fc` | kFriendOptAlarm1on1 | `features\windows\chat_window.cc:190` |  |
| `0x015fb300` | kFriendOptOpenFromFriend | `features\windows\chat_window.cc:191` |  |
| `0x015fb30c` | kHairNumMale | `ui\head_icon.cc:39` |  |
| `0x015fb318` | kHairNumFemale | `ui\head_icon.cc:40` |  |
| `0x015fb348` | kJobNameBeg | `ragnarok\homunculus.cc:47` |  |
| `0x015fb34c` | kJobNameEnd | `ragnarok\homunculus.cc:48` |  |
| `0x015fb398` | kMacrosFirst | `ragnarok\emotion_hotkey.cc:27` | _Myfirst |
| `0x015fb39c` | kMacrosLast | `ragnarok\emotion_hotkey.cc:28` | _Mylast |
| `0x015fb3b0` | kAid, kOwnPetAid | `features\windows\entity_context_menu.cc:159`<br>`ragnarok\pet.cc:19` |  |
| `0x015fb3b4` | kRenameFlag | `ragnarok\pet.cc:20` | 0 = renommable une fois |
| `0x015fb3b8` | kName | `ragnarok\pet.cc:21` | char[32] (le paquet n'en donne que 24) |
| `0x015fb3d8` | kAccessory | `ragnarok\pet.cc:22` | ITID ; 0 = aucun |
| `0x015fb3dc` | kClass | `ragnarok\pet.cc:23` | = id de mob |
| `0x015fb3e0` | kLevel | `ragnarok\pet.cc:24` |  |
| `0x015fb3e4` | kHunger | `ragnarok\pet.cc:25` |  |
| `0x015fb3ec` | kIntimacy | `ragnarok\pet.cc:26` |  |
| `0x015fb3f0` | kEggInvIndex | `ragnarok\pet.cc:27` | -1 = aucun |
| `0x015fb3f4` | kPrevHunger | `ragnarok\pet.cc:28` | la faim sauvée avant écrasement |
| `0x015fb99c` | kAutoFeed | `ragnarok\pet.cc:29` | écrit par le handler de ZC 0x02D9 |
| `0x015fb9a4` | kAccountAid, kOwnAccountAid, kOwnAccountId, kOwnAid, kOwnAidAddr, kOwnHandlePtr | `features\fx\ez_effect_capture.cc:44`<br>`features\fx\style_sync.cc:24`<br>`features\gameplay\quick_cast.cc:42`<br>`features\overlays\basic_info.cc:162`<br>`features\overlays\target_frame.cc:274`<br>`features\windows\char_diagnostics.cc:35`<br>… +6 | handle/AID du joueur |
| `0x015fb9a8` | kOwnCharId, kOwnCharIdAddr, kOwnCid | `features\hotkey_util.cc:24`<br>`features\fx\style_sync.cc:28`<br>`features\windows\char_diagnostics.cc:36`<br>`features\windows\character_sheet.cc:88`<br>`features\windows\palette_editor.cc:50` | g_Own_CharId (cf. project_own_session_globals) |
| `0x015fb9ac` | ⚓ kCurrentMapNameAddr | `ragnarok\globals.h:224` |  |
| `0x015fb9c8` | kOwnJobId, kOwnJobIdAddr | `features\overlays\basic_info.cc:160`<br>`features\windows\char_diagnostics.cc:38`<br>`features\windows\monster_info_window.cc:436`<br>`features\windows\palette_editor.cc:43` | g_Own_JobId — le job RÉEL |
| `0x015fb9d0` | kBaseExpLo | `features\overlays\basic_info.cc:77`<br>`features\windows\char_diagnostics.cc:48` | + Hi à +4, idem pour les trois autres |
| `0x015fb9d8` | kBaseExpNextLo, kOwnBaseExpNext | `features\overlays\basic_info.cc:77`<br>`features\overlays\basic_info.cc:2772`<br>`features\windows\char_diagnostics.cc:49` | g_Own_BaseExpNext (INT64) |
| `0x015fb9e0` | kJobExpNextLo, kOwnJobExpNext | `features\overlays\basic_info.cc:78`<br>`features\overlays\basic_info.cc:2773`<br>`features\windows\char_diagnostics.cc:50` | g_Own_JobExpNext  (INT64) |
| `0x015fb9e8` | kJobExpLo | `features\overlays\basic_info.cc:78`<br>`features\windows\char_diagnostics.cc:51` |  |
| `0x015fb9f0` | ⚓ kBaseLevel, kBaseLevelAddr, kBaseLvl | `features\overlays\basic_info.cc:122`<br>`features\windows\character_sheet.cc:103`<br>`ragnarok\globals.h:171` | DAT_015fb9f0 (UIBasicInfoWnd "Base Lv.") |
| `0x015fb9f4` | kStatusPoint, kStatusPt | `features\patches\status_tweaks.cc:114`<br>`features\windows\char_diagnostics.cc:39`<br>`features\windows\character_sheet.cc:96` | g_Own_StatusPoints |
| `0x015fb9f8` | ⚓ kBaseLvl, kJobLevel, kJobLevelAddr, kOwnJobLevel | `features\overlays\basic_info.cc:123`<br>`features\windows\character_sheet.cc:103`<br>`features\windows\weapon_refine_window.cc:70`<br>`ragnarok\globals.h:172` | DAT_015fb9f8 (UIBasicInfoWnd "Job Lv.") |
| `0x015fb9fc` | kSkillPoint, kSkillPointLvl, kSkillPointsAddr | `features\patches\skill_tree_tweaks.cc:42`<br>`features\windows\char_diagnostics.cc:40`<br>`features\windows\character_sheet.cc:1033` | changes when points are spent |
| `0x015fba04` | kSkillPts | `ragnarok\homunculus.cc:37` |  |
| `0x015fba0c` | ⚓ kStatBonus, kStatBonusAddr | `features\patches\status_tweaks.cc:103`<br>`features\windows\char_diagnostics.cc:58`<br>`features\windows\character_sheet.cc:94`<br>`ragnarok\globals.h:154` | bonus |
| `0x015fba10` | (littéral) | `features\patches\status_tweaks.cc:103` |  |
| `0x015fba14` | (littéral) | `features\patches\status_tweaks.cc:103` |  |
| `0x015fba18` | (littéral) | `features\patches\status_tweaks.cc:104` |  |
| `0x015fba1c` | (littéral) | `features\patches\status_tweaks.cc:104` |  |
| `0x015fba20` | (littéral) | `features\patches\status_tweaks.cc:104` |  |
| `0x015fba24` | ⚓ kStatBase, kStatBaseAddr | `features\patches\status_tweaks.cc:101`<br>`features\windows\char_diagnostics.cc:57`<br>`features\windows\character_sheet.cc:93`<br>`ragnarok\globals.h:153` | STR..LUK, pas de 4 |
| `0x015fba28` | (littéral) | `features\patches\status_tweaks.cc:101` |  |
| `0x015fba2c` | (littéral) | `features\patches\status_tweaks.cc:101` |  |
| `0x015fba30` | (littéral) | `features\patches\status_tweaks.cc:102` |  |
| `0x015fba34` | (littéral) | `features\patches\status_tweaks.cc:102` |  |
| `0x015fba38` | (littéral) | `features\patches\status_tweaks.cc:102` |  |
| `0x015fba3c` | kRaiseCost, kStatCost | `features\patches\status_tweaks.cc:105`<br>`features\windows\char_diagnostics.cc:59`<br>`features\windows\character_sheet.cc:95` | coût de la prochaine montée |
| `0x015fba40` | (littéral) | `features\patches\status_tweaks.cc:105` |  |
| `0x015fba44` | (littéral) | `features\patches\status_tweaks.cc:105` |  |
| `0x015fba48` | (littéral) | `features\patches\status_tweaks.cc:106` |  |
| `0x015fba4c` | (littéral) | `features\patches\status_tweaks.cc:106` |  |
| `0x015fba50` | (littéral) | `features\patches\status_tweaks.cc:106` |  |
| `0x015fba54` | kAspdRaw, kAttackDelay, kFlee | `features\patches\status_tweaks.cc:113`<br>`features\windows\char_diagnostics.cc:71`<br>`features\windows\character_sheet.cc:102` |  |
| `0x015fba58` | kAtk1 | `features\patches\status_tweaks.cc:107`<br>`features\windows\char_diagnostics.cc:60`<br>`features\windows\character_sheet.cc:97` |  |
| `0x015fba5c` | kMdefSoft | `features\patches\status_tweaks.cc:109`<br>`features\windows\char_diagnostics.cc:63`<br>`features\windows\character_sheet.cc:100` |  |
| `0x015fba64` | kDefSoft | `features\patches\status_tweaks.cc:108`<br>`features\windows\char_diagnostics.cc:61`<br>`features\windows\character_sheet.cc:98` |  |
| `0x015fba68` | kDefSoft | `features\patches\status_tweaks.cc:108`<br>`features\windows\char_diagnostics.cc:61`<br>`features\windows\character_sheet.cc:98` |  |
| `0x015fba6c` | kAtk1 | `features\patches\status_tweaks.cc:107`<br>`features\windows\char_diagnostics.cc:60`<br>`features\windows\character_sheet.cc:97` |  |
| `0x015fba70` | kMatkMax, kMatkMin | `features\patches\status_tweaks.cc:110`<br>`features\windows\char_diagnostics.cc:62`<br>`features\windows\character_sheet.cc:99` |  |
| `0x015fba74` | kMatkMax, kMatkMin | `features\patches\status_tweaks.cc:110`<br>`features\windows\char_diagnostics.cc:62`<br>`features\windows\character_sheet.cc:99` |  |
| `0x015fba78` | kMdefSoft | `features\patches\status_tweaks.cc:109`<br>`features\windows\char_diagnostics.cc:63`<br>`features\windows\character_sheet.cc:100` |  |
| `0x015fba7c` | kHit, kOwnHit | `features\patches\status_tweaks.cc:111`<br>`features\windows\char_diagnostics.cc:64`<br>`features\windows\character_sheet.cc:101`<br>`features\windows\monster_info_window.cc:319` |  |
| `0x015fba80` | kFlee, kHit, kOwnFlee | `features\patches\status_tweaks.cc:112`<br>`features\windows\char_diagnostics.cc:64`<br>`features\windows\character_sheet.cc:102`<br>`features\windows\monster_info_window.cc:320` |  |
| `0x015fba84` | kCrit, kHit, kOwnCrit | `features\patches\status_tweaks.cc:111`<br>`features\windows\char_diagnostics.cc:65`<br>`features\windows\character_sheet.cc:101`<br>`features\windows\monster_info_window.cc:321` |  |
| `0x015fba88` | kCrit, kFlee, kOwnPdodge | `features\patches\status_tweaks.cc:112`<br>`features\windows\char_diagnostics.cc:65`<br>`features\windows\character_sheet.cc:102`<br>`features\windows\monster_info_window.cc:322` |  |
| `0x015fba8c` | kAmmoInvIndex | `features\windows\character_sheet.cc:119` | g_AmmoEquippedInvIndex (0 = aucune) |
| `0x015fba90` | ⚓ kZenyAddr | `ragnarok\globals.h:39` |  |
| `0x015fba94` | kWalkSpeed | `features\windows\char_diagnostics.cc:74` | g_Own_Speed |
| `0x015fba98` | kManner | `features\windows\char_diagnostics.cc:41` | g_Own_Manner |
| `0x015fba9c` | kWeightMax | `features\overlays\basic_info.cc:82`<br>`features\patches\inventory_tweaks.cc:57`<br>`features\windows\char_diagnostics.cc:42`<br>`features\windows\inventory_viewer.cc:72` | max weight (raw) |
| `0x015fbaa0` | kWeight, kWeightCur | `features\overlays\basic_info.cc:82`<br>`features\patches\inventory_tweaks.cc:56`<br>`features\windows\char_diagnostics.cc:43`<br>`features\windows\inventory_viewer.cc:71` | current weight (raw) |
| `0x015fbab0` | kInvListHead | `features\windows\character_sheet.cc:89`<br>`features\windows\chat_window.cc:65`<br>`features\windows\craft_atlas.cc:32`<br>`features\windows\inventory_viewer.cc:52`<br>`features\windows\make_item_window.cc:218`<br>`features\windows\npc_shop_window.cc:76`<br>… +3 | std::list<ItemSkillInfo> (session+0x16f0) |
| `0x015fbab4` | kInvCount | `features\windows\character_sheet.cc:90`<br>`features\windows\inventory_viewer.cc:53` | _Mysize |
| `0x015fbad8` | kStorageListHead | `features\windows\storage_window.cc:115`<br>`features\windows\storage_window.cc:835` |  |
| `0x015fbae0` | kCartListHead | `features\windows\cart_viewer.cc:60` | sentinelle std::list (head) |
| `0x015ff634` | kBodyResNamesBegin | `ui\sprite_path.cc:17` |  |
| `0x015ff638` | kBodyResNamesEnd | `ui\sprite_path.cc:18` |  |
| `0x015ff658` | kHeadgearNamesBegin | `ui\doll.cc:117` |  |
| `0x015ff65c` | kHeadgearNamesEnd | `ui\doll.cc:118` |  |
| `0x015ff7e0` | kNativeList | `ragnarok\skill_cooldowns.cc:28` | g_ShortCutCooldownList (objet std::list) |
| `0x015ff804` | kInPartyFlag | `features\windows\entity_context_menu.cc:158` |  |
| `0x015ff824` | kBattleChatModeOn | `features\windows\chat_window.cc:133` |  |
| `0x015ff838` | kInputTargetMode | `features\windows\chat_window.cc:127`<br>`ragnarok\emotion_hotkey.cc:52` | 0 public 1 groupe 2 guilde 3 clan 4 alliés |
| `0x015ff908` | kHp | `features\overlays\basic_info.cc:79`<br>`features\windows\char_diagnostics.cc:44`<br>`features\windows\character_sheet.cc:104` |  |
| `0x015ff90c` | kHp, kHpMax | `features\overlays\basic_info.cc:79`<br>`features\windows\char_diagnostics.cc:45`<br>`features\windows\character_sheet.cc:104` |  |
| `0x015ff910` | kOwnSpCur, kSp | `features\overlays\basic_info.cc:80`<br>`features\windows\char_diagnostics.cc:46`<br>`features\windows\character_sheet.cc:105`<br>`features\windows\weapon_refine_window.cc:86` |  |
| `0x015ff914` | kSp, kSpMax | `features\overlays\basic_info.cc:80`<br>`features\windows\char_diagnostics.cc:47`<br>`features\windows\character_sheet.cc:105` |  |
| `0x015ff918` | kAid | `ragnarok\homunculus.cc:18` | GID de l'homoncule |
| `0x015ff91c` | kName | `ragnarok\homunculus.cc:19` | char[24] |
| `0x015ff93c` | kAtk | `ragnarok\homunculus.cc:20` | puis MATK/HIT/CRI/DEF/MDEF/FLEE de 4 en 4 |
| `0x015ff958` | kAmotion | `ragnarok\homunculus.cc:21` |  |
| `0x015ff95c` | kClass | `ragnarok\homunculus.cc:22` | -1 = aucun homoncule connu |
| `0x015ff960` | kLevel | `ragnarok\homunculus.cc:23` |  |
| `0x015ff964` | kHp | `ragnarok\homunculus.cc:24` |  |
| `0x015ff968` | kMaxHp | `ragnarok\homunculus.cc:25` |  |
| `0x015ff96c` | kSp | `ragnarok\homunculus.cc:26` |  |
| `0x015ff970` | kMaxSp | `ragnarok\homunculus.cc:27` |  |
| `0x015ff974` | kIntimacy | `ragnarok\homunculus.cc:28` |  |
| `0x015ff980` | kExp | `ragnarok\homunculus.cc:29` | i64 |
| `0x015ff988` | kExpNext | `ragnarok\homunculus.cc:30` | i64 |
| `0x015ff990` | kHunger | `ragnarok\homunculus.cc:31` |  |
| `0x015ff998` | kFlags | `ragnarok\homunculus.cc:32` |  |
| `0x015ff99c` | kOrderBlk | `ragnarok\homunculus.cc:33` | ctx+0x55DC, six dwords (ordre à l'homoncule) |
| `0x015ff9b4` | kPresent | `ragnarok\homunculus.cc:35` | != 0 <=> un homoncule est invoqué |
| `0x015ff9c4` | kAtkRange | `ragnarok\homunculus.cc:36` |  |
| `0x015ffd14` | kShowEquipFlag | `features\windows\character_sheet.cc:144` | 1 = équip visible des autres (validé live) |
| `0x015ffd60` | kNormalSlots | `features\windows\char_select.cc:160` |  |
| `0x015ffd64` | kPremiumSlots | `features\windows\char_select.cc:161` |  |
| `0x015ffd68` | kBillingSlots | `features\windows\char_select.cc:162` |  |
| `0x015ffd6c` | kCreatableSlots | `features\windows\char_select.cc:163` |  |
| `0x015ffd78` | ⚓ kStateHolderAddr | `ragnarok\lua.h:33` |  |
| `0x015ffd80` | kGridIds | `features\overlays\status_icon_bar.cc:53` | int[100] laid-out ids (-1 empty) |
| `0x015fffa0` | kDropLockGlobal | `features\windows\inventory_viewer.cc:373` |  |
| `0x015fffa1` | kSafeCheckFlag | `features\windows\vending_window.cc:430` | octet, persistant |
| `0x015fffc0` | kBankVault | `features\windows\bank_window.cc:37` | s64 |
| `0x016004fc` | kOwnTitleId | `features\windows\character_sheet.cc:2137` | g_Own_TitleId : titre ÉQUIPÉ (0 = aucun) |
| `0x01600500` | kOwnTitleBegin | `features\windows\character_sheet.cc:2138` | std::vector<int> g_OwnTitleList : begin (possédés) |
| `0x01600504` | kOwnTitleEnd | `features\windows\character_sheet.cc:2139` | .. end |
| `0x0160052c` | kHairNumDoramMale | `ui\head_icon.cc:41` |  |
| `0x01600538` | kHairNumDoramFemale | `ui\head_icon.cc:42` |  |
| `0x01600553` | kDealLockGlobal, kFavFlag | `features\patches\inventory_tweaks.cc:105`<br>`features\windows\inventory_viewer.cc:374`<br>`features\windows\npc_shop_window.cc:95` | FAV-mode global (matches FUN_00946da0) |
| `0x01602278` | kLeftHandEquipOpt | `features\windows\inventory_viewer.cc:108` | DAT_01602278 : option client "équip main gauche" active ? |
| `0x0160231c` | kAutoFeed | `ragnarok\homunculus.cc:38` | octet |
| `0x01602324` | kOverweightPct | `features\patches\inventory_tweaks.cc:58`<br>`features\windows\inventory_viewer.cc:73` | red-tint % threshold |
| `0x0160232c` | kPriorityClassAddr | `ragnarok\game_settings.cc:108` |  |
| `0x01602354` | kInvExpansion | `features\patches\inventory_tweaks.cc:73`<br>`features\windows\inventory_viewer.cc:74` | server-sent inventory expansion (slots - 200) |
| `0x016023e4` | kSnapName | `features\windows\vending_window.cc:324` | std::string : nom de la boutique |
| `0x01602400` | kSnapBegin | `features\windows\vending_window.cc:322` | vecteur d'ItemSkillInfo |
| `0x01602404` | kSnapEnd | `features\windows\vending_window.cc:323` |  |
| `0x016024c0` | kCostumeHideFlag | `features\windows\character_sheet.cc:145` | 0 = costumes affichés (validé live) |
| `0x01602568` | kOwnCharName | `features\windows\char_diagnostics.cc:37` | g_Own_CharName_Plain (char[]) |
| `0x016025b8` | (littéral) | `features\patches\pick_quad_tweaks.cc:41` | unités de compétence — facteur 34 |
| `0x016025c8` | kCfgDx7DeviceGuid | `ragnarok\game_settings.cc:124` | 16 o. |
| `0x016025d8` | kCfgDx7DriverGuid | `ragnarok\game_settings.cc:125` | 16 o. |
| `0x016025e8` | kCfgDx7AdapterName | `ragnarok\game_settings.cc:126` | 40 o. |
| `0x01602610` | kCfgFullscreenAddr | `ragnarok\game_settings.cc:114` |  |
| `0x01602614` | kCfgWidthAddr | `ragnarok\game_settings.cc:115` |  |
| `0x01602618` | kCfgHeightAddr | `ragnarok\game_settings.cc:116` |  |
| `0x0160261c` | kCfgBppAddr | `ragnarok\game_settings.cc:117` |  |
| `0x01602630` | kCfgSpriteDetailAddr | `ragnarok\game_settings.cc:118` |  |
| `0x01602634` | kCfgTextureDetailAddr | `ragnarok\game_settings.cc:119` |  |
| `0x01602638` | kCfgTrilinearAddr | `ragnarok\game_settings.cc:120` |  |
| `0x01602640` | kCfgRenderSystemAddr | `ragnarok\game_settings.cc:121` |  |
| `0x01602644` | kCfgDx9AdapterGuid | `ragnarok\game_settings.cc:122` | 16 o. |
| `0x01602654` | kCfgDx9DeviceName | `ragnarok\game_settings.cc:123` | 32 o. |
| `0x01602674` | kSoundModeVal | `features\overlays\login_parade.cc:35` | SOUNDMODE (registre, boot) : 0 = son coupé au setup |
| `0x01602b60` | kSpriteDetailFlagA | `ragnarok\game_settings.cc:131` |  |
| `0x01602b64` | kSpriteDetailFlagB | `ragnarok\game_settings.cc:132` |  |
