# Manifeste des adresses natives (client 20250716, base 0x400000)

GÉNÉRÉ par `python tools/gen_address_manifest.py` — ne pas éditer à la main.
C'est la check-list du portage : chaque adresse ci-dessous est à retrouver
dans le nouvel exe, puis à corriger à CHAQUE site listé (un seul pour les
entrées ⚓ : leurs déclinaisons passent par l'annuaire).

- **558 adresses distinctes**, 709 sites
- ⚓ 49 portées par l'annuaire (`ragnarok/*.h`, `ui/game_texture.h`)
- 79 présentes dans plusieurs fichiers (candidates annuaire si cohérentes)

| Adresse | Nom(s) | Sites | Commentaire |
|---|---|---|---|
| `0x004f08f0` | ⚓ kStdStringDtorAddr | `ragnarok\globals.h:140` |  |
| `0x00519df0` | ⚓ kGetFieldAddr | `ragnarok\lua.h:46` | lua_getfield(L, idx, k) |
| `0x0051a290` | ⚓ kPCallAddr | `ragnarok\lua.h:47` | lua_pcall(L, nargs, nres, errfunc) |
| `0x0051a4b0` | ⚓ kPushNumberAddr | `ragnarok\lua.h:48` | lua_pushnumber(L, double) |
| `0x0051aab0` | ⚓ kSetTopAddr | `ragnarok\lua.h:49` | lua_settop(L, idx) |
| `0x0051abf0` | ⚓ kToBooleanAddr | `ragnarok\lua.h:50` | lua_toboolean(L, idx) |
| `0x0051aca0` | ⚓ kToLStringAddr | `ragnarok\lua.h:51` | lua_tolstring(L, idx, &len) |
| `0x0051ad20` | ⚓ kToNumberAddr | `ragnarok\lua.h:52` | lua_tonumber(L, idx) |
| `0x0051b570` | ⚓ kCheckStackAddr | `ragnarok\lua.h:53` | lua_checkstack(L, extra) |
| `0x0053f140` | kEngNodeBlit | `features\patches\chat.cc:361` | __thiscall(node, x, y, w, h, ARGB*, colorkey) |
| `0x0053faa0` | kVecU32Resize | `features\patches\chat.cc:732` | std::vector<uint32_t>::resize |
| `0x005471a0` | kEngTextOutLow | `features\patches\chat.cc:288`<br>`features\patches\chat.cc:853`<br>`features\patches\chat.cc:856` | __thiscall(ctx, x, y, str, len) |
| `0x005474a0` | kEngTextMeasure | `features\patches\chat.cc:289`<br>`features\patches\chat.cc:813` | __thiscall(ctx, SIZE*, str, len, font) |
| `0x00550b10` | kRenderQueueInsert | `features\fx\ez_effect_capture.cc:27` | RenderQueue_InsertPrimitive — hooké (capture) |
| `0x00553e80` | kDepthScale | `features\fx\ez_effect_capture.cc:24` | Effect_DepthToScreenScale(ctx,_,invW) -> float |
| `0x00554040` | kWorldDepthConvertVa | `features\fx\hat_effect_depth.cc:32` |  |
| `0x005541b0` | kSceneProject | `features\fx\ez_effect_capture.cc:23` | Scene_ProjectWorldToScreen(ctx,_,world,view,&sx,&sy,&invW) |
| `0x0055c830` | kDX9DrawPrimRec | `features\fx\spr_effect_lab.cc:344` | vtbl +0x38 : draw mono-texture |
| `0x0055c8c0` | kDX9DrawPrimDual | `features\fx\spr_effect_lab.cc:345` | vtbl +0x34 : draw BI-texture (aniso) |
| `0x0055d680` | kDX9DrawGround | `features\fx\spr_effect_lab.cc:320` | RendererDX9_DrawGroundTiles(this = renderer DX9) |
| `0x0055d850` | kDX9DrawTerrain | `features\fx\spr_effect_lab.cc:346` | RendererDX9_DrawTerrainSurfaces |
| `0x005663d0` | ⚓ kAtlasBuildAddr | `ragnarok\render.h:54` |  |
| `0x00566b70` | ⚓ kAtlasGetCachedAddr | `ragnarok\render.h:53` |  |
| `0x00568760` | kSpriteRef | `features\overlays\status_icon_bar.cc:46` | __thiscall(cache,path,0,0,1,0) -> ref (5 args!) |
| `0x005a1460` | kCNameDict_GetEntryOrRequest, kNameDictGetEntry | `features\overlays\entity_names.cc:31`<br>`features\windows\vending_window.cc:211` |  |
| `0x005a4300` | kItemSkillInfoDtor | `features\windows\make_item_window.cc:229` |  |
| `0x005c5950` | kSetOption | `features\overlays\skill_bar.cc:55` | SkillMgr_SetOption(mgr,key,val,0) ; key5=UI-lock (≥1 bloque OnMsg 0x29) |
| `0x005c8950` | kEmblemRequestUpload | `features\windows\character_sheet.cc:1269` | __thiscall(this, guildId, std::string) |
| `0x005d98a0` | kSkillIdAtoi | `features\overlays\skill_bar.cc:592` |  |
| `0x005ff990` | kSoundMgrGet | `features\overlays\login_parade.cc:50` | getter/lazy-create du SoundMgr (renseigne 0x01253d0c) |
| `0x00600430` | kSoundPlay2D | `features\overlays\login_parade.cc:57` | Sound_PlaySample2D(this,handle,&x,&y,&z,min,max,vol) |
| `0x00600770` | (littéral) | `features\minigames\roggle.cc:238`<br>`features\minigames\rojeweled.cc:87` |  |
| `0x006046e0` | kDeferEntry | `features\fx\weapon_layer.cc:43` | CActorSprite_DeferQuadSorted |
| `0x006049e4` | kKeyWrite | `features\fx\weapon_layer.cc:44` | branch-1 key write (MOV [EAX+0x10],EDI) |
| `0x0060c820` | (littéral) | `ragnarok\configuration.h:13` |  |
| `0x00613100` | (littéral) | `ragnarok\configuration.h:14` |  |
| `0x0061d370` | kGetEmblemPath | `features\windows\character_sheet.cc:1032` | __thiscall(this, out_str, guildId) |
| `0x0061d560` | kGetEmblemVersion | `features\windows\character_sheet.cc:1124` | __thiscall(this, guildId) -> version |
| `0x00623e20` | (littéral) | `ragnarok\configuration.h:15` |  |
| `0x0065ae30` | (littéral) | `ragnarok\configuration.h:20` |  |
| `0x0069f480` | kProbFetch | `features\windows\item_desc_window.cc:158` | ItemProbabilityDB_Fetch |
| `0x006a06b0` | ⚓ kEnsureLoadedAddr | `ragnarok\item_db.h:32` |  |
| `0x006a0d40` | ⚓ kLookupAddr | `ragnarok\item_db.h:27` | __cdecl(id, &table) -> record |
| `0x006a1b20` | ⚓ kInfoCtorAddr, kItemSkillInfoCtor | `features\windows\make_item_window.cc:228`<br>`ragnarok\item_db.h:40` |  |
| `0x006a2970` | kGetCardResName | `features\windows\item_desc_window.cc:89` | __cdecl(id) -> record ; *record = resname |
| `0x006a2a70` | kGetDescLines | `features\windows\item_desc_window.cc:72` | ItemSkillDB_GetDescLines(info) -> &vector<char*> |
| `0x006a2b50` | ⚓ kBaseNameFallbackAddr | `ragnarok\item_db.h:48` |  |
| `0x006a2dd0` | (littéral) | `ragnarok\configuration.h:34` |  |
| `0x006a3f20` | kRecipeGetLines | `features\windows\make_item_window.cc:609` |  |
| `0x006a4bc0` | kGetResName | `features\windows\item_desc_window.cc:73`<br>`ui\icon_cache.cc:51` | ItemSkillDB_GetResName(info) -> resname C-str (icône) |
| `0x006a4c10` | ⚓ kSlotCountAddr | `ragnarok\item_db.h:55` |  |
| `0x006a5db0` | kBookDbContains | `features\windows\item_desc_window.cc:170` | BookItemDB_Contains |
| `0x006a6570` | ⚓ kInfoSetIdAddr, kItemSkillInfoSetId | `features\windows\make_item_window.cc:372`<br>`ragnarok\item_db.h:41` |  |
| `0x006aa6e0` | (littéral) | `ragnarok\configuration.h:35` |  |
| `0x006bcfc0` | (littéral) | `ragnarok\configuration.h:36` |  |
| `0x006f4800` | (littéral) | `ragnarok\configuration.h:41` |  |
| `0x0070f2c0` | kActFramesFn | `features\overlays\basic_info.cc:208` | Act_GetActionFrames(act,action) |
| `0x0070f390` | kActFrameLayer | `features\overlays\basic_info.cc:616` | Act_GetFrameLayer(frame, idx) |
| `0x0070f420` | kActGetSndName | `features\overlays\login_parade.cc:51` | Act_GetSoundName(act, idx) -> char* (SSO/heap) |
| `0x0070f4b0` | ⚓ kActionGetFrameAddr | `ragnarok\render.h:58` |  |
| `0x0070f6b0` | kActFrameCount | `features\minigames\roggle.cc:328`<br>`features\minigames\rojeweled.cc:58`<br>`features\overlays\login_parade.cc:28` | __thiscall(act, action) -> int (#frames) |
| `0x007103d0` | kVecStrResize | `features\patches\chat.cc:731` | std::vector<std::string>::resize |
| `0x007110c0` | kTerrainHeight | `features\gameplay\player_jump.cc:17` | Terrain_GetHeightAt(world,x,z)->float |
| `0x00715be0` | kStrLayerTex | `features\overlays\basic_info.cc:1133` | Str_GetLayerTexture(layer, idx) |
| `0x00715d30` | kStrLayerTexDir | `features\overlays\basic_info.cc:1165` | Str_GetLayerTextureWithDir(layer, idx, &dir) |
| `0x007166b0` | (littéral) | `ragnarok\configuration.h:55` |  |
| `0x0071df50` | (littéral) | `ragnarok\configuration.h:56` |  |
| `0x00727444` | (littéral) | `utils\hooking\detours.h:73` | "Dtr\0" |
| `0x007289da` | (littéral) | `features\systems\discord_relay.cc:9` |  |
| `0x00731350` | (littéral) | `ragnarok\configuration.h:57` |  |
| `0x00738370` | kSkillGetTabList | `features\windows\character_sheet.cc:890`<br>`features\windows\weapon_refine_window.cc:152`<br>`ragnarok\player_skills.cc:13` | __thiscall(bundle, tab) -> std::list* |
| `0x00738570` | kSkillSetUseLevel | `features\windows\character_sheet.cc:891` | __thiscall(bundle, id, lv) |
| `0x00739cd0` | kSkillEntryDtor | `features\overlays\skill_bar.cc:572`<br>`features\windows\character_sheet.cc:117` | = FUN_00739cd0 (cleanup de la struct) |
| `0x0073a140` | kGetSkillIdNameLua | `features\overlays\skill_bar.cc:468`<br>`features\windows\character_sheet.cc:162` | char* GetSkillIdName(int) __cdecl |
| `0x0073a1f0` | kGetSkillNameLua | `features\overlays\skill_bar.cc:94`<br>`features\windows\character_sheet.cc:2085` | char* GetSkillName(int id) (__cdecl, via Lua) |
| `0x0073adb0` | kIsLevelUseSkill | `features\windows\character_sheet.cc:893` | __cdecl(id) : l'effet dépend-il du niveau ? |
| `0x0075f850` | (littéral) | `ragnarok\configuration.h:62` |  |
| `0x0079a6a0` | kActorDtor | `features\overlays\basic_info.cc:132` | actor-object destructor |
| `0x0079d5e0` | kSelCharRenderPatch | `ragnarok\ragnarok_client.cc:214` | mov ecx,[esi+0x120] |
| `0x0079d610` | kCharSelOnMsg | `features\windows\char_select.cc:113` | vtbl+0x94, RET 0x18 |
| `0x007a6df0` | kColorChip | `features\patches\inventory_tweaks.cc:141` | FUN_007a6df0(x,y,&r,&g,&b) -> colorchip.bmp pixel |
| `0x007ac210` | kActorCtor | `features\overlays\basic_info.cc:130` | actor-object ctor (19 params) |
| `0x007ac820` | kActorDraw | `features\overlays\basic_info.cc:131` | actor draw (param 1 = quad path) |
| `0x007c72e0` | kPurgeStaleMails | `features\windows\rodex_window.cc:81` | __thiscall(CRodexSystemMgr*) |
| `0x007f8969` | kSwapRightIconImm | `features\patches\equip_tweaks.cc:94` | mov [ebp-0x4d0],0xfa (right icon x=250) |
| `0x007f897d` | kSwapRightNameImm | `features\patches\equip_tweaks.cc:95` | mov [ebp-0x4d8],0xad (right name x=173) |
| `0x007f9688` | kSwapTitleImm | `features\patches\equip_tweaks.cc:93` | push 0x8c imm32  (title x, centred) |
| `0x0080d1a0` | kListErase | `features\patches\inventory_tweaks.cc:169` | std::list::erase(first,last) __thiscall(listObj,&out,first,last): frees nodes + dtors + si |
| `0x00812e60` | kToggleWndById | `features\windows\inventory_viewer.cc:87` | FUN_00812e60(id) __stdcall (RET 0x4, vérifié désasm) : bascule fenêtre (ferme si ouverte v |
| `0x00814064` | kVisTable | `features\overlays\menu_icons.cc:78` | indexed by (cmdId - 0x178), 0..0xDF |
| `0x00814150` | kGridDrawOrig | `features\overlays\menu_icons.cc:47` | UIMenuIconWnd_RebuildNodes |
| `0x00814a70` | kCmdHandler | `features\overlays\menu_icons.cc:68` | UIMenuIconWnd command handler |
| `0x00814bb0` | (littéral) | `ragnarok\configuration.h:17` |  |
| `0x00815630` | (littéral) | `ragnarok\configuration.h:18` |  |
| `0x008217f0` | (littéral) | `features\windows\inventory_viewer.cc:596` |  |
| `0x008303f0` | kSetTextAddr | `features\systems\native_login.cc:21` | CUIEdit_SetText |
| `0x00831a50` | kSetName | `features\patches\inventory_tweaks.cc:97` | UIItemLinkBtn_SetName(this, char*) |
| `0x0083d3f0` | kWrapAndDispatch | `features\patches\chat.cc:259`<br>`features\patches\chat.cc:845` | UISubChatWnd_WrapAndDispatch |
| `0x0083d840` | (littéral) | `features\patches\chat.cc:820`<br>`features\patches\chat.cc:823` |  |
| `0x0083f070` | kSubChatAddLine | `features\patches\chat.cc:228`<br>`features\patches\chat.cc:834` | __thiscall(this,text,color,sender) |
| `0x00857910` | kTabDrawOrig | `features\patches\inventory_tweaks.cc:157` | FUN_00857910 tab DrawContent |
| `0x0085fca0` | kTabRecompute | `features\patches\inventory_tweaks.cc:150` | FUN_0085fca0(tabctrl): recompute strip size after a size[] change |
| `0x008642d0` | kRebuildFromHist | `features\patches\chat.cc:530` | __thiscall(tab): clear+re-wrap history |
| `0x00864690` | kAddTab | `features\patches\inventory_tweaks.cc:168` | tab control AddTab(this, label, tooltip) |
| `0x00874af0` | (littéral) | `features\fx\screen_fx.cc:234` |  |
| `0x00880e7f` | kSnapJnz | `features\patches\window_pos_tweaks.cc:321` | JNZ 0x00880e9f  (bytes 75 1e) |
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
| `0x008c18b0` | kItemDescWndAddr | `features\moonlight_ui\moonlight_ui.h:273` |  |
| `0x008cb7c0` | kMsgOrig | `features\patches\status_tweaks.cc:55` | FUN_008cb7c0 status msg handler (ret 0x18 = SIX stack args!) |
| `0x008d4580` | (littéral) | `ragnarok\configuration.h:59` |  |
| `0x008d5410` | (littéral) | `ragnarok\configuration.h:60` |  |
| `0x008db420` | (littéral) | `features\fx\screen_fx.cc:236` |  |
| `0x008e1d50` | kGetOption | `features\overlays\skill_bar.cc:54` | SkillInfoMgr_GetOption(mgr,key) ; key10=onglet courant |
| `0x008f3498` | kDialogBgBlitRet | `features\patches\chat.cc:187` | return addr of the dialog_bg blit |
| `0x008f5800` | kOnDraw | `features\overlays\skill_bar.cc:146` | UIShortCutWnd::OnDraw (vtable+0x50) — contenu slots |
| `0x008f9840` | kInputRowLayout | `features\patches\chat.cc:528`<br>`features\patches\chat.cc:912` | __thiscall(this) — input-row controls |
| `0x008fc220` | kChatWndProc | `features\patches\chat.cc:526`<br>`features\patches\chat.cc:878` | __thiscall WndProc (vtable+0x94) |
| `0x008fd12a` | kChatWrapCaller | `features\patches\chat.cc:153` | return addr of the chat call site |
| `0x0090243f` | kDetachedWrapCaller | `features\patches\chat.cc:260` | ret addr of the detached call site |
| `0x009030c0` | kHideNative, kSetVisibleFn | `features\overlays\skill_bar.cc:155`<br>`features\windows\item_desc_window.cc:71` | UIWnd_SetVisible (vtable+0x38, __thiscall) |
| `0x00903160` | kSetTabBarHeight | `features\patches\chat.cc:527` | __thiscall(this, rows) — relayout |
| `0x0090ce90` | (littéral) | `ragnarok\configuration.h:22` |  |
| `0x009137a0` | kDrawContent | `features\overlays\quest_tracker.cc:25` | QuestTracker_DrawContent (hooked) |
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
| `0x0096b72b` | kLayoutTabBoundImm | `features\patches\inventory_tweaks.cc:175` | the imm8 (0x03) of that CMP |
| `0x00977e80` | kDrawContent | `features\patches\skill_tree_tweaks.cc:31` | UINewSkillListWnd::DrawContent |
| `0x00979f10` | kCharWrap | `features\patches\chat.cc:152`<br>`features\patches\chat.cc:889` | __cdecl(text, outlist, maxchars) |
| `0x00996910` | (littéral) | `ragnarok\configuration.h:45` |  |
| `0x00997b50` | (littéral) | `ragnarok\configuration.h:46` |  |
| `0x009ded20` | (littéral) | `ragnarok\configuration.h:64` |  |
| `0x009eb410` | (littéral) | `features\fx\screen_fx.cc:238` |  |
| `0x00a145b0` | (littéral) | `ragnarok\configuration.h:52` |  |
| `0x00a1b7c0` | kActorQuadFn | `features\overlays\basic_info.cc:134` | textured-quad submit (hooked) |
| `0x00a1cb70` | kUISetSize | `features\patches\chat.cc:529` | __thiscall(obj, w, h) |
| `0x00a1d260` | kBlit, kBlitImageToNode | `features\patches\chat.cc:186`<br>`features\patches\chat.cc:900`<br>`features\patches\inventory_tweaks.cc:48`<br>`features\patches\status_tweaks.cc:70` | __thiscall(this,x,y,tex,flag) |
| `0x00a1d460` | kFill | `features\patches\inventory_tweaks.cc:49` | __thiscall(this,x,y,w,h,color) filled rect |
| `0x00a1edf0` | kUiWndFadeColor | `features\overlays\basic_info.cc:1927` | UIWindow_GetFadeColor (vtbl+0xa0) |
| `0x00a1f2a0` | (littéral) | `ragnarok\configuration.h:43` |  |
| `0x00a21c90` | kMeasureW | `features\patches\chat.cc:864`<br>`features\patches\chat.cc:867`<br>`features\patches\inventory_tweaks.cc:47` | __thiscall(this,str,len,face,size,_,_) -> width |
| `0x00a226f0` | (littéral) | `ragnarok\configuration.h:53` |  |
| `0x00a25a70` | kDrawText | `features\patches\inventory_tweaks.cc:46`<br>`features\patches\status_tweaks.cc:68` | __thiscall(this,x,y,str,len,face,size,color,bold,ital) |
| `0x00a27b50` | kDrawTextR | `features\patches\status_tweaks.cc:69` | __thiscall(this,x,y,str,len,face,size,color,bold) RIGHT@x |
| `0x00a29ba0` | (littéral) | `ragnarok\configuration.h:83` |  |
| `0x00a2e770` | ⚓ kCloseWindowAddr, kSaveWindowRect | `features\windows\inventory_viewer.cc:153`<br>`ragnarok\uiwnd.h:58` | __thiscall(mgr, id) |
| `0x00a31a30` | (littéral) | `features\windows\char_select.cc:288`<br>`features\windows\char_select.cc:291` |  |
| `0x00a33005` | kSnapLoopHook | `features\patches\window_pos_tweaks.cc:342` | MOV ECX,[EBP-0x88] (8B 8D 78 FF FF FF) |
| `0x00a39340` | ⚓ kMakeWindowAddr | `ragnarok\uiwnd.h:57` | UIWindowMgr::MakeWindow(id) |
| `0x00a3a2e4` | kWidthOwnImm | `features\patches\equip_tweaks.cc:61` | push 0x118 imm32 (own equip window) |
| `0x00a3a48b` | kHeightImm | `features\patches\status_tweaks.cc:49` | push 0x8d (h=141) imm in MakeWindow |
| `0x00a3a490` | kWidthImm | `features\patches\status_tweaks.cc:50` | push 0x118 (w=280) imm in MakeWindow |
| `0x00a3f0f6` | kWidthOtherImm | `features\patches\equip_tweaks.cc:62` | push 0x118 imm32 (view-other-player) |
| `0x00a471e0` | (littéral) | `ragnarok\configuration.h:72`<br>`ragnarok\configuration.h:84` |  |
| `0x00a47b90` | ⚓ kFindWindowAddr | `ragnarok\uiwnd.h:26` | UIWindowMgr::FindWindow(id) __thiscall |
| `0x00a4ad20` | kChatAddFn | `bourgeon.cc:150`<br>`ragnarok\configuration.h:85` |  |
| `0x00a50b70` | (littéral) | `ragnarok\configuration.h:31` |  |
| `0x00a5e960` | (littéral) | `ragnarok\configuration.h:32` |  |
| `0x00a69eb0` | kFindByGID | `features\gameplay\player_jump.cc:18` | ActorList_FindByGID(actorMgr,gid)->acteur |
| `0x00a74410` | kCursorRenderFn | `ragnarok\ragnarok_client.cc:72` | CursorMgr_RenderSprite |
| `0x00a75340` | ⚓ kModeMgrGetActiveAddr | `ragnarok\globals.h:66` |  |
| `0x00a756e0` | (littéral) | `ragnarok\configuration.h:95` |  |
| `0x00a7b0a0` | (littéral) | `ragnarok\configuration.h:103` |  |
| `0x00a8d4a0` | ⚓ kLoad | `ui\game_texture.h:35` | __fastcall(mgr, _, key) -> tex |
| `0x00a8e500` | kTexExists | `features\overlays\basic_info.cc:606` | UITextureMgr_ResourceExists(mgr, path)->bool |
| `0x00a8e800` | kResAddRef, kTexAddRef | `features\fx\weapon_dual_sprites.cc:21`<br>`features\overlays\basic_info.cc:1763` | resource AddRef (ECX = res) |
| `0x00a8f910` | kResRelease | `features\fx\weapon_dual_sprites.cc:22` | resource Release (ECX = res) |
| `0x00a90350` | ⚓ kGet | `ui\game_texture.h:33` | __cdecl() -> mgr |
| `0x00a94870` | kFmtComma64 | `features\windows\bank_window.cc:41` |  |
| `0x00a948d0` | kFmtComma, kFmtComma32 | `features\patches\inventory_tweaks.cc:53`<br>`features\windows\bank_window.cc:42`<br>`features\windows\inventory_viewer.cc:84` | __cdecl(value,buf,size) -> thousands-separated |
| `0x00a94930` | kStdStringFromFmt | `features\windows\character_sheet.cc:1270` | (dst, fmt, …) -> std::string du jeu |
| `0x00a98400` | kXmlFindChild | `features\systems\native_login.cc:53` | __thiscall(node, name) -> node |
| `0x00a98460` | kXmlFindNextSibling | `features\systems\native_login.cc:54` | __thiscall(node, name) -> node |
| `0x00a984c0` | kXmlGetText | `features\systems\native_login.cc:55` | __fastcall(node) -> std::string* |
| `0x00a9a7d0` | ⚓ kCallGlobalVaAddr | `ragnarok\lua.h:60` |  |
| `0x00a9bc90` | ⚓ kExecFileAddr | `ragnarok\lua.h:61` |  |
| `0x00a9ed30` | kGetAddr | `ragnarok\msgstring.h:25` |  |
| `0x00a9f030` | ⚓ kMakeKey | `ui\game_texture.h:34` | __cdecl(path) -> key |
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
| `0x00b314f0` | kNaviRoute | `features\windows\item_desc_window.cc:110` | CNavigation::SearchRoute |
| `0x00b5ed20` | kRenderType0 | `features\overlays\status_icon_bar.cc:81` |  |
| `0x00b666d0` | kEzEffectDraw | `features\fx\ez_effect_capture.cc:26` | EzEffect_Draw(nœud EZ) — hooké (appartenance) |
| `0x00b90780` | kEffCtor | `features\overlays\basic_info.cc:1624` | EffectInst_Ctor_StrNode(node) |
| `0x00bb4170` | kEffLoadStr | `features\overlays\basic_info.cc:1625` | Effect_LoadStrByEffectId(node,src,id,x,y,z) |
| `0x00bb5d10` | kMakeNode | `features\overlays\status_icon_bar.cc:44` | __thiscall(scene,0,0.0(8),0.0f) -> node |
| `0x00bc2de1` | kEffectJumpDefault | `features\fx\ez_effect_capture.cc:89` |  |
| `0x00bc2e04` | kEffectJumpTable | `features\fx\ez_effect_capture.cc:86`<br>`features\fx\spr_effect_lab.cc:750` |  |
| `0x00bced10` | kEffUpdateKF | `features\overlays\basic_info.cc:1626` | Effect_UpdateStrKeyframes(node,offXYZ,f,f) |
| `0x00bcfb10` | kStrSubmitQuad | `features\overlays\basic_info.cc:1132` | Effect_SubmitStrQuad (hooké) |
| `0x00bd4230` | kBuildFn | `features\overlays\status_icon_bar.cc:43` | status-bar build/layout (hooked) |
| `0x00c0c0c0` | kBarFill | `features\patches\inventory_tweaks.cc:85` | grid-gap filler, matches the btnbar grey (palette idx 7) |
| `0x00c13fc0` | (littéral) | `ragnarok\configuration.h:87` |  |
| `0x00c144b0` | (littéral) | `ragnarok\configuration.h:92` |  |
| `0x00c148b0` | (littéral) | `ragnarok\configuration.h:93` |  |
| `0x00c14920` | (littéral) | `ragnarok\configuration.h:88` |  |
| `0x00c3c3ff` | (littéral) | `features\overlays\entity_names.cc:152` | mob bleu clair / joueur+npc blanc |
| `0x00c44940` | kToggleEffectId | `features\fx\spr_effect_lab.cc:32`<br>`features\overlays\basic_info.cc:1360` | Actor_ToggleEffectId(actor, unifiedId, add) __thiscall |
| `0x00c69160` | kClampReach | `features\gameplay\keyboard_move.cc:17` | Move_ClampToReachableCell |
| `0x00c6aa80` | kWorldToTile | `features\gameplay\keyboard_move.cc:15` | MapCoord_WorldToTileAndSub |
| `0x00c6b1e0` | kOwnsItemById | `features\windows\item_desc_window.cc:171` | Inventory_OwnsItemById |
| `0x00c6cf80` | kCellValid | `features\gameplay\keyboard_move.cc:16` | Cell_IsMoveTargetValid |
| `0x00c74a80` | (littéral) | `ragnarok\configuration.h:99` |  |
| `0x00c82340` | kCamClamp | `features\gameplay\fps_view.cc:23` | Camera_ApplyViewDistanceClamp |
| `0x00c86740` | (littéral) | `ragnarok\configuration.h:73`<br>`ragnarok\configuration.h:100` |  |
| `0x00c8c8c7` | (littéral) | `features\overlays\entity_names.cc:153` |  |
| `0x00c93cb0` | kHitTestFn | `features\overlays\status_icon_bar.cc:112` |  |
| `0x00ca0c5d` | (littéral) | `bourgeon.cc:119` |  |
| `0x00caa2e0` | (littéral) | `ragnarok\configuration.h:89` |  |
| `0x00cb13c6` | (littéral) | `bourgeon.cc:119` |  |
| `0x00cf8b10` | kRecvDeleteResult | `features\windows\rodex_window.cc:229` |  |
| `0x00cfd0c0` | kRecvAckReadRodex | `features\windows\rodex_window.cc:173` | Recv_ZC_AckReadRodex_0x0B63 |
| `0x00d00010` | kApplyCheckNameAck | `features\windows\rodex_window.cc:275` | __stdcall, retn 0x10 |
| `0x00d21210` | (littéral) | `features\windows\char_select.cc:224`<br>`features\windows\char_select.cc:227` |  |
| `0x00d272e0` | (littéral) | `ragnarok\configuration.h:97` |  |
| `0x00d36ee4` | kActFrameCall | `features\fx\weapon_dual_sprites.cc:20` | CALL Act_GetFrame (E8 rel32) |
| `0x00d403a0` | kBuildWeaponLayers | `features\fx\weapon_dual_sprites.cc:19` | CActorSprite_BuildWeaponLayers |
| `0x00d54c40` | kShopAddOrMerge | `features\windows\vending_window.cc:295` |  |
| `0x00d54d80` | kAvailAddOrMerge | `features\windows\vending_window.cc:298` |  |
| `0x00d54ea0` | kBasketAddOrMerge | `features\windows\vending_window.cc:306` | (session, rec) — 2 args ! |
| `0x00d55f80` | (littéral) | `features\windows\npc_shop_window.cc:395` |  |
| `0x00d57780` | (littéral) | `ragnarok\configuration.h:80` |  |
| `0x00d57ac0` | kShopConsume | `features\windows\vending_window.cc:299` |  |
| `0x00d57c60` | kAvailConsume | `features\windows\vending_window.cc:296` |  |
| `0x00d57e40` | kBasketRemove | `features\windows\vending_window.cc:307` | (session, rec) — 2 args ! |
| `0x00d5a720` | kBuildIconPath, kEngBuildPath | `features\minigames\roggle.cc:209`<br>`features\overlays\skill_bar.cc:486`<br>`features\patches\chat.cc:97`<br>`ui\icon_cache.cc:15` | __fastcall(session, 0, idstr, outbuf, 0) |
| `0x00d5a980` | kGetSkillInfo | `features\overlays\skill_bar.cc:66` | SkillMgr_GetSkillInfo(mgr,out,id,gate) ; out+4!=0 => trouvé |
| `0x00d5aa40` | kLookupSkill | `features\overlays\skill_bar.cc:591` |  |
| `0x00d5b580` | (littéral) | `features\overlays\basic_info.cc:115`<br>`features\overlays\basic_info.cc:436`<br>`features\overlays\basic_info.cc:536`<br>`features\overlays\basic_info.cc:1027`<br>`features\windows\character_sheet.cc:776`<br>`features\windows\character_sheet.cc:2121` |  |
| `0x00d5bb40` | kJobDisplayName | `features\overlays\basic_info.cc:117`<br>`features\windows\char_select.cc:533`<br>`features\windows\character_sheet.cc:778`<br>`features\windows\character_sheet.cc:791`<br>`features\windows\rodex_window.cc:307` | __thiscall(ctx, classId, sex) |
| `0x00d5bea0` | kShopGetAt | `features\windows\vending_window.cc:279` |  |
| `0x00d5bf40` | kShopAmountBySrc | `features\windows\vending_window.cc:325` |  |
| `0x00d5c160` | kAvailGetAt | `features\windows\vending_window.cc:278` |  |
| `0x00d5c200` | kAvailAmountBySrc | `features\windows\vending_window.cc:324` |  |
| `0x00d5c580` | kBasketGetAt | `features\windows\vending_window.cc:305` | (session, out_rec, i) |
| `0x00d5e3c0` | kSkillGetUseLevel | `features\windows\character_sheet.cc:892` | __thiscall(SESSION, id) — même tableau |
| `0x00d5e590` | (littéral) | `ragnarok\configuration.h:81` |  |
| `0x00d7fa90` | kGetInvItem, kGetInvItemAddr, kSkillEntryFill | `features\overlays\skill_bar.cc:189`<br>`features\overlays\skill_bar.cc:504`<br>`features\overlays\skill_bar.cc:570`<br>`features\overlays\skill_bar.cc:593`<br>`features\windows\character_sheet.cc:116`<br>`features\windows\storage_window.cc:471` | __stdcall(out, id) |
| `0x00d7fd30` | (littéral) | `features\windows\char_select.cc:923` |  |
| `0x00d80950` | kGetHotKey | `features\hotkey_util.cc:19`<br>`features\overlays\skill_bar.cc:359` | GetHotKey(out, category, slot) __stdcall RET 0xc |
| `0x00d824c0` | kMonResName | `features\minigames\roggle.cc:325`<br>`features\overlays\login_parade.cc:25` | __stdcall(classId) -> resname ("poring") |
| `0x00d84760` | kGetSex | `features\overlays\basic_info.cc:133` | GetSex(session) |
| `0x00d87380` | kGetEFSTImg | `features\overlays\status_icon_bar.cc:45` | __thiscall(session,id,layer) -> const char* |
| `0x00d89ed0` | kTitleGetStr | `features\windows\character_sheet.cc:1849` |  |
| `0x00d8a010` | kJobWpnAct | `features\overlays\basic_info.cc:613` | -> .act (pousse 0) |
| `0x00d8a080` | kJobShieldAct | `features\overlays\basic_info.cc:615` | -> .act (pousse 0) |
| `0x00d8a0f0` | kJobShieldSpr | `features\overlays\basic_info.cc:614` | -> .spr (pousse 1) |
| `0x00d8a160` | kJobWpnSpr | `features\overlays\basic_info.cc:612` | -> .spr (pousse 1) |
| `0x00d8a1d0` | kItemIdToWeaponClass, kWpnItemClass | `features\fx\weapon_dual_sprites.cc:23`<br>`features\overlays\basic_info.cc:607` | Weapon_ItemIdToWeaponClass |
| `0x00d8e6c0` | kIsBerserkActive | `features\patches\berserk_chat_unlock.cc:19` |  |
| `0x00d96c20` | kSetShortCut | `features\overlays\skill_bar.cc:53` | SkillMgr_SetShortCutSlot |
| `0x00d9a960` | kCntCostume | `features\windows\inventory_viewer.cc:53` | __fastcall(session) : nb items COSTUME distincts (10 slots @+0x2b34) |
| `0x00d9aa70` | kCntEquipped | `features\windows\inventory_viewer.cc:52` | __fastcall(session) : nb items ÉQUIPÉS distincts (10 slots @+0x17d4) |
| `0x00da8f90` | kSetItemSlot | `features\overlays\skill_bar.cc:188` |  |
| `0x00dbbc4f` | ⚓ kGameOperatorNewAddr | `ragnarok\globals.h:125` | __cdecl(size) -> void*, jamais nul |
| `0x00dbbc7f` | ⚓ kGameOperatorDeleteAddr | `ragnarok\globals.h:126` | __cdecl(ptr) |
| `0x00fd5d60` | kNativeAbsMaskVa | `features\fx\hat_effect_depth.cc:35` |  |
| `0x00fd6ae4` | kNativeDepthQuantumVa | `features\fx\hat_effect_depth.cc:34` |  |
| `0x00ff0000` | (littéral) | `features\fx\ez_effect_capture.cc:417`<br>`imgui\imgui_impl_dx7.cc:34`<br>`imgui\imgui_impl_dx7.cc:230`<br>`imgui\imgui_impl_dx7.cc:330` |  |
| `0x00ff00ff` | kEmblemClear | `features\minigames\roggle.cc:270`<br>`features\windows\character_sheet.cc:1358` | magenta pur = transparent en jeu |
| `0x00ffaa00` | (littéral) | `features\overlays\dps_meter.cc:187` |  |
| `0x00ffe060` | (littéral) | `features\overlays\quest_tracker.h:18` | hunt progress lines ("mob ( x / y )") |
| `0x00ffff00` | (littéral) | `features\windows\character_sheet.cc:1209` |  |
| `0x00ffffff` | (littéral) | `features\minigames\roggle.cc:270`<br>`features\minigames\rojeweled.cc:469`<br>`features\minigames\rojeweled.cc:494`<br>`features\overlays\entity_names.cc:152`<br>`features\overlays\quest_tracker.h:17`<br>`features\overlays\status_icon_bar.cc:353`<br>… +1 | mob bleu clair / joueur+npc blanc |
| `0x01013e88` | kStrCanvasCy | `features\overlays\basic_info.cc:1290` | DAT_01013e88 (soustrait de workBuf[1]) |
| `0x0101ca18` | kCashVTable | `features\windows\cashshop_window.cc:33` |  |
| `0x0101d424` | kCharSelWndVtbl | `features\windows\char_select.cc:112` | garde anti-pointeur périmé |
| `0x01021b30` | kWriteVTable | `features\windows\rodex_window.cc:45` |  |
| `0x01021e9e` | kIcoZenyPath | `features\windows\rodex_window.cc:330` | « \…\rodexsystem\renewal\icon_zeny.bmp » |
| `0x01021fbc` | kReadVTable | `features\windows\rodex_window.cc:35` |  |
| `0x01022170` | kInboxVTable | `features\windows\rodex_window.cc:34` | vérifiée live (g_RodexInboxWnd+0) |
| `0x01022a8e` | kIcoBothPath | `features\windows\rodex_window.cc:332` | « …\icon_zeny_n_item.bmp » |
| `0x01022ad6` | kIcoItemPath | `features\windows\rodex_window.cc:331` | « …\icon_item.bmp » |
| `0x01022f5c` | kStrCanvasCx | `features\overlays\basic_info.cc:1289` | DAT_01022f5c (soustrait de workBuf[0]) |
| `0x010265e8` | kStyleshopPath | `features\windows\inventory_viewer.cc:769` | "유저인터페이스\styleshop\btn_buy_out.bmp" |
| `0x01028200` | kGridDrawSlot | `features\overlays\menu_icons.cc:46` | UIMenuIconWnd vtbl +0x50 |
| `0x0102a260` | kResizeBtnVtable | `features\patches\chat.cc:531` | UIResizeButton vtable (PTR_FUN_0102a260) |
| `0x0102dd94` | kTabDrawSlot | `features\patches\inventory_tweaks.cc:156` | tab control vtable +0x50 slot (static .rdata) |
| `0x01030168` | kVtblUILoginWnd | `features\systems\native_login.cc:17` | garde de validité de la fenêtre |
| `0x01030fd4` | kBankVTable | `features\windows\bank_window.cc:29` |  |
| `0x01031264` | kAcctClassNormal | `features\systems\native_login.cc:20` |  |
| `0x0103181c` | kFmtSpr | `features\minigames\roggle.cc:326`<br>`features\minigames\rojeweled.cc:56`<br>`features\overlays\login_parade.cc:26` | "몬스터\\%s.spr" (CP949) |
| `0x0103182c` | kFmtAct | `features\minigames\roggle.cc:327`<br>`features\minigames\rojeweled.cc:57`<br>`features\overlays\login_parade.cc:27` | "몬스터\\%s.act" (CP949) |
| `0x010322d0` | kMsgSlot | `features\patches\equip_tweaks.cc:109` | UIEquipWnd vtable +0x94 (message handler slot) |
| `0x010323ec` | kDetailVTable | `features\windows\npc_shop_window.cc:46` | UIItemParamChangeDisplayWnd |
| `0x01032a24` | kDrawSlot | `features\patches\status_tweaks.cc:51`<br>`features\patches\status_tweaks.cc:344` | UIStatusWnd vtable +0x50 (DrawContent) |
| `0x01032a68` | kMsgSlot | `features\patches\status_tweaks.cc:54` | UIStatusWnd vtable +0x94 (message handler slot) |
| `0x01032aac` | kItemVTable | `features\windows\item_desc_window.cc:45` | classe 0xc |
| `0x01032c5c` | kCompareVTable | `features\windows\item_desc_window.cc:57` | classe 0xea |
| `0x01032e0c` | kSkillVTable | `features\windows\item_desc_window.cc:46` | classe 0x2e |
| `0x01033754` | kAcceptVTable | `features\windows\trade_window.cc:41` | popup requête (best-effort) |
| `0x010345ac` | kVTableMakingArrow | `features\windows\make_item_window.cc:43` |  |
| `0x01034684` | kCompVTable | `features\windows\inventory_viewer.cc:133` |  |
| `0x0103517c` | kBookVTable | `features\windows\item_desc_window.cc:181` |  |
| `0x010357b8` | kBtnbarPath, kUiPrefixPath | `features\patches\inventory_tweaks.cc:51`<br>`features\windows\cart_viewer.cc:245`<br>`features\windows\inventory_viewer.cc:763`<br>`features\windows\rodex_window.cc:329`<br>`features\windows\storage_window.cc:241` | "유저인터페이스\basic_interface\btnbar_left.bmp" (bottom-frame height) |
| `0x010361b4` | kBgNormalPath | `features\patches\status_tweaks.cc:71` | "...\statuswnd\w_statwin_bg.bmp" |
| `0x01036648` | kCardBmpPrefix | `features\windows\item_desc_window.cc:90` | "유저인터페이스\cardBmp\" (CP949, null-term) |
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
| `0x01037ea8` | (littéral) | `features\patches\chat_bg.cc:60` |  |
| `0x01037f80` | (littéral) | `features\patches\chat_bg.cc:47` |  |
| `0x010384a0` | (littéral) | `features\fx\screen_fx.cc:235`<br>`features\fx\screen_fx.cc:236`<br>`features\fx\screen_fx.cc:313` |  |
| `0x0103ca40` | ⚓ kStorageWndVTable | `ragnarok\uiwnd.h:102` |  |
| `0x0103cbf0` | kSellListVTable | `features\windows\npc_shop_window.cc:44` |  |
| `0x0103ce78` | kSellVTable | `features\windows\npc_shop_window.cc:39` | UIItemSellWnd (conteneur, id 0x17) |
| `0x0103d460` | ⚓ kInventoryWndVTable | `ragnarok\uiwnd.h:100` |  |
| `0x0103d4b0` | kDrawSlot | `features\patches\inventory_tweaks.cc:35`<br>`features\patches\inventory_tweaks.cc:706` | UIItemWnd vtable +0x50 |
| `0x0103d4f4` | kMsgSlot | `features\patches\inventory_tweaks.cc:166` | inventory vtable +0x94 (message handler slot) |
| `0x0103d538` | kCartVTable | `features\windows\cart_viewer.cc:46`<br>`features\windows\inventory_viewer.cc:125`<br>`features\windows\storage_window.cc:165` |  |
| `0x0103dad4` | kIconNumPath | `features\windows\cart_viewer.cc:247`<br>`features\windows\inventory_viewer.cc:765` | …\inventory\icon_num.bmp |
| `0x0103db00` | kIconPath, kIconWeightPath | `features\patches\inventory_tweaks.cc:50`<br>`features\windows\cart_viewer.cc:246`<br>`features\windows\inventory_viewer.cc:764` | "유저인터페이스\inventory\icon_weight.bmp" |
| `0x0103e35c` | kBIMsgSlot | `features\overlays\basic_info.cc:3421` | vtable+0x94 OnMsg slot |
| `0x0103ec50` | kVTableMakeTarget | `features\windows\make_item_window.cc:45` |  |
| `0x0103ee00` | kRefineVTable | `features\windows\weapon_refine_window.cc:37` |  |
| `0x0103eed8` | kVTableMakeProcess | `features\windows\make_item_window.cc:51` | ⏱ confirmé sur 2 instances |
| `0x010457d8` | kExchangeVTable | `features\windows\trade_window.cc:39` | CUIExchangeUI (fenêtre d'échange) |
| `0x01047d7c` | (littéral) | `features\fx\screen_fx.cc:237`<br>`features\fx\screen_fx.cc:238` |  |
| `0x0104dee4` | kCameraVtable | `features\gameplay\fps_view.cc:14`<br>`features\gameplay\keyboard_move.cc:19` | g_CCamera_vtable (validates pCam) |
| `0x010758d8` | kCEZ2STRVtbl | `features\fx\ez_effect_capture.cc:45` | CEZ2STREffect (.str name-based) -> à EXCLURE |
| `0x01088c48` | kEzChildVtbl | `features\fx\ez_effect_capture.cc:29` | vtable du nœud EZ enfant (celui que Draw dessine) |
| `0x0108f9a0` | kFmtActF, kFmtActFemale | `features\windows\char_select.cc:336`<br>`ui\head_icon.cc:18` | …\여\%d_여.act |
| `0x0108f9bc` | kFmtSprF, kFmtSprFemale | `features\windows\char_select.cc:335`<br>`ui\head_icon.cc:19` | 인간족\머리통\여\%d_여.spr (femelle) |
| `0x0108f9d8` | kFmtActM, kFmtActMale | `features\windows\char_select.cc:338`<br>`ui\head_icon.cc:20` | …\남\%d_남.act |
| `0x0108f9f4` | kFmtSprM, kFmtSprMale | `features\windows\char_select.cc:337`<br>`ui\head_icon.cc:21` | 인간족\머리통\남\%d_남.spr (mâle) |
| `0x010904b8` | (littéral) | `ragnarok\configuration.h:74` |  |
| `0x01091520` | (littéral) | `features\fx\screen_fx.cc:258` |  |
| `0x01091528` | (littéral) | `features\fx\screen_fx.cc:259` |  |
| `0x010932f0` | kVtblCLoginMode | `features\systems\native_login.cc:16` | garde de mode |
| `0x011e40d4` | (littéral) | `ragnarok\ragnarok_client.cc:156` |  |
| `0x011e40d8` | (littéral) | `ragnarok\ragnarok_client.cc:157` |  |
| `0x01213338` | ⚓ kModeMgrAddr | `ragnarok\globals.h:46` |  |
| `0x0121333c` | ⚓ kActiveModePtr | `ragnarok\globals.h:52` |  |
| `0x012291c0` | (littéral) | `features\fx\screen_fx.cc:241` | g_cam_zoomMaxOutdoor |
| `0x012291c4` | (littéral) | `features\fx\screen_fx.cc:242` | g_cam_zoomMaxIndoor |
| `0x01234567` | (littéral) | `features\overlays\login_parade.cc:278` |  |
| `0x012515f8` | ⚓ kContextPtr | `ragnarok\render.h:23` |  |
| `0x0125161c` | kSpriteCache | `features\overlays\status_icon_bar.cc:48` | &DAT_0125161c (sprite-ref cache) |
| `0x012517b8` | kEmblemDataMgrPtr | `features\windows\character_sheet.cc:1268` | *ptr = CEmblemDataMgr |
| `0x01253d0c` | kSoundMgrPtr | `features\minigames\roggle.cc:236`<br>`features\minigames\rojeweled.cc:85`<br>`features\overlays\login_parade.cc:49` | *ptr = SoundMgr (déréf 1×) |
| `0x01254d70` | kCGuildMgrPtr | `features\windows\character_sheet.cc:1031` | *ptr = g_CGuildMgr (CGuildEmblemMgr) |
| `0x01254d90` | kQuestMgrPtr | `features\overlays\quest_tracker.cc:26` | void** -> quest manager |
| `0x01255108` | kProbDbPtr | `features\windows\item_desc_window.cc:157` | ptr vers le mgr (lazy-new) |
| `0x0125510c` | ⚓ kEnsureCachePtr | `ragnarok\item_db.h:33` |  |
| `0x01255130` | ⚓ kTableAddr | `ragnarok\item_db.h:25` | la map |
| `0x01255138` | ⚓ kNilAddr | `ragnarok\item_db.h:26` | sentinelle de l'arbre |
| `0x01255180` | kCardNilRecord | `features\windows\item_desc_window.cc:96` |  |
| `0x0131ecdc` | kRodexMgrPtr | `features\windows\rodex_window.cc:66` |  |
| `0x0131f4e8` | ⚓ kUIWindowMgrAddr | `ragnarok\uiwnd.h:25` | g_UIWindowMgr (l'OBJET, pas un pointeur vers lui) |
| `0x0131f510` | kDetachedChatTree | `features\patches\chat.cc:761` | std::set<UIChatWnd*> head node |
| `0x0131f6b0` | (littéral) | `features\windows\inventory_viewer.cc:600` |  |
| `0x0131f6b4` | kPLoginWnd | `features\systems\native_login.cc:18` | UILoginWnd* (== mgr+0x1cc, FindWindow id 3) |
| `0x0131f6bc` | ⚓ kInventoryWndSlot | `ragnarok\uiwnd.h:99` | inventaire, id 8 |
| `0x0131f6c4` | kBasicInfoPtr, kRenderCtxPtr | `features\overlays\basic_info.cc:135`<br>`features\overlays\basic_info.cc:3584` | BasicInfo window = a valid renderCtx |
| `0x0131f6f0` | kCompWndSlot | `features\windows\inventory_viewer.cc:132` | = g_UIWindowMgr + 0x208 |
| `0x0131f700` | kItemDescWndGlobalPtr, kItemWndSlot | `features\moonlight_ui\moonlight_ui.h:281`<br>`features\windows\item_desc_window.cc:41` | mgr+0x218 : ITEM desc (classe 0xc) |
| `0x0131f708` | kCompareWndSlot | `features\windows\item_desc_window.cc:56` | mgr+0x220 : COMPARE desc (id 0xea) |
| `0x0131f718` | kSkillWndSlot | `features\windows\item_desc_window.cc:42` | mgr+0x230 : SKILL desc (classe 0x2e) |
| `0x0131f738` | kSellListGlobal | `features\windows\npc_shop_window.cc:45` |  |
| `0x0131f764` | kNoPathFlag | `features\gameplay\keyboard_move.cc:18` | != 0 -> le natif passe en msg 0x10 |
| `0x0131f770` | ⚓ kStorageWndSlot | `ragnarok\uiwnd.h:101` | UIItemStoreWnd, id 0x21 (slot = mgr+0x288) |
| `0x0131f7ac` | kSkillWndSlot | `features\windows\character_sheet.cc:919` | mgr(0x0131f4e8)+0x2C4 |
| `0x0131f940` | kWriteWndPtr | `features\windows\rodex_window.cc:43` | g_MailWriteWnd |
| `0x0136e6c8` | kVecBegin | `features\overlays\status_icon_bar.cc:49` | std::vector<StatusIcon>::begin (raw bytes) |
| `0x0136e6cc` | kVecEnd | `features\overlays\status_icon_bar.cc:50` | ::end |
| `0x0159b818` | ⚓ kClientCodePageAddr | `ragnarok\globals.h:116` |  |
| `0x0159b8a8` | kClientInfoXmlDoc | `features\systems\native_login.cc:52` | racine du document parsé |
| `0x0159c188` | kGuildBuf, kGuildObj | `features\patches\status_tweaks.cc:117`<br>`features\windows\character_sheet.cc:380` | std::string buffer/ptr union |
| `0x0159c198` | kGuildLen | `features\patches\status_tweaks.cc:115` | std::string _Mysize (0 == no guild) |
| `0x0159c19c` | kGuildCap | `features\patches\status_tweaks.cc:116` | std::string _Myres  (>=0x10 => heap ptr) |
| `0x0159c1a0` | kGuildMasterName | `features\windows\character_sheet.cc:396` | std::string : nom du maître |
| `0x0159c1b8` | kGuildNoticeSubj | `features\windows\character_sheet.cc:403` | std::string : sujet de l'annonce (ZC 0x016f) |
| `0x0159c1d0` | kGuildNoticeBody | `features\windows\character_sheet.cc:404` | std::string : corps de l'annonce |
| `0x0159c1e8` | kGuildLevel | `features\windows\character_sheet.cc:382` |  |
| `0x0159c1f0` | kGuildOnlineNum | `features\windows\character_sheet.cc:397` | connect_member (membres en ligne) |
| `0x0159c1f4` | kGuildMemberMax | `features\windows\character_sheet.cc:398` | max_member |
| `0x0159c1f8` | kGuildAvgLevel | `features\windows\character_sheet.cc:399` | niveau moyen |
| `0x0159c1fc` | kGuildManageLand | `features\windows\character_sheet.cc:400` | std::string : territoire (nb de forts) |
| `0x0159c214` | kGuildExp | `features\windows\character_sheet.cc:401` | exp courante |
| `0x0159c218` | kGuildNextExp | `features\windows\character_sheet.cc:402` | exp du niveau suivant |
| `0x0159c230` | kGuildIdAddr | `features\windows\character_sheet.cc:384` |  |
| `0x0159c23c` | kGuildIsMaster | `features\windows\character_sheet.cc:383` |  |
| `0x0159c26c` | kGuildRelHead | `features\windows\character_sheet.cc:405` | CGuild+0xe4 : liste alliés/ennemis |
| `0x015beecc` | kReplayActive | `ragnarok\skill_cooldowns.cc:23` | g_ReplayActive |
| `0x015c3090` | kNaviMgr | `features\windows\item_desc_window.cc:111` | &DAT_015c3090 (nav manager) |
| `0x015c5a24` | kSocketFd | `features\systems\native_login.cc:19` | g_RagConnection_SocketFd |
| `0x015f8262` | kSelectedSlot | `features\windows\char_select.cc:91` |  |
| `0x015fa3c0` | ⚓ kSessionAddr | `ragnarok\globals.h:23` |  |
| `0x015fa3cc` | kSkillBundle | `features\windows\character_sheet.cc:888`<br>`features\windows\weapon_refine_window.cc:150`<br>`ragnarok\player_skills.cc:11` | CPlayerSkillBundle (session+0x0C) |
| `0x015fa3e0` | kSkillFlatList | `features\windows\character_sheet.cc:889`<br>`features\windows\weapon_refine_window.cc:151`<br>`ragnarok\player_skills.cc:12` | bundle+0x14 : l'onglet « divers » |
| `0x015fa850` | (littéral) | `features\overlays\skill_bar.cc:176` |  |
| `0x015fa94c` | (littéral) | `features\overlays\skill_bar.cc:177` |  |
| `0x015fb23c` | kAccountSex | `features\windows\char_select.cc:177` |  |
| `0x015fb278` | kHair | `features\overlays\basic_info.cc:136` | DAT_015fb278 hair style |
| `0x015fb280` | kWeaponView | `features\overlays\basic_info.cc:603` | g_OwnLook_WeaponViewId (0 = rien) |
| `0x015fb284` | kShieldView | `features\overlays\basic_info.cc:604` | g_OwnLook_ShieldViewId (0 = rien) |
| `0x015fb288` | kWeaponView2 | `features\overlays\basic_info.cc:605` | g_OwnLook_WeaponViewId2 (arme seed login) |
| `0x015fb28c` | kClothesCol | `features\overlays\basic_info.cc:137` | DAT_015fb28c clothes palette |
| `0x015fb290` | kHairCol | `features\overlays\basic_info.cc:138` | DAT_015fb290 hair palette |
| `0x015fb2a0` | kGarmentView | `features\overlays\basic_info.cc:139` | g_OwnLook_GarmentRobeViewId |
| `0x015fb2d4` | kCartNumItems | `features\windows\cart_viewer.cc:69` |  |
| `0x015fb2d8` | kCartMaxItems | `features\windows\cart_viewer.cc:70` |  |
| `0x015fb2dc` | kCartWeight | `features\windows\cart_viewer.cc:71` |  |
| `0x015fb2e0` | kCartMaxWeight | `features\windows\cart_viewer.cc:72` |  |
| `0x015fb9a4` | kAccountAid, kOwnAccountId, kOwnHandlePtr | `features\fx\ez_effect_capture.cc:44`<br>`features\windows\char_select.cc:179`<br>`features\windows\character_sheet.cc:121`<br>`features\windows\make_item_window.cc:311`<br>`features\windows\weapon_refine_window.cc:59` | handle/AID du joueur |
| `0x015fb9a8` | kOwnCharId | `features\hotkey_util.cc:20`<br>`features\windows\character_sheet.cc:71` | g_Own_CharId (cf. project_own_session_globals) |
| `0x015fb9d0` | (littéral) | `features\overlays\basic_info.cc:62` |  |
| `0x015fb9d8` | (littéral) | `features\overlays\basic_info.cc:62` |  |
| `0x015fb9e0` | (littéral) | `features\overlays\basic_info.cc:63` |  |
| `0x015fb9e8` | (littéral) | `features\overlays\basic_info.cc:63` |  |
| `0x015fb9f0` | ⚓ kBaseLevel, kBaseLevelAddr, kBaseLvl | `features\overlays\basic_info.cc:102`<br>`features\windows\character_sheet.cc:86`<br>`ragnarok\globals.h:99` | DAT_015fb9f0 (UIBasicInfoWnd "Base Lv.") |
| `0x015fb9f4` | kStatusPoint, kStatusPt | `features\patches\status_tweaks.cc:114`<br>`features\windows\character_sheet.cc:79` |  |
| `0x015fb9f8` | ⚓ kBaseLvl, kJobLevel, kJobLevelAddr, kOwnJobLevel | `features\overlays\basic_info.cc:103`<br>`features\windows\character_sheet.cc:86`<br>`features\windows\weapon_refine_window.cc:69`<br>`ragnarok\globals.h:100` | DAT_015fb9f8 (UIBasicInfoWnd "Job Lv.") |
| `0x015fb9fc` | kSkillPointLvl, kSkillPointsAddr | `features\patches\skill_tree_tweaks.cc:42`<br>`features\windows\character_sheet.cc:894` | changes when points are spent |
| `0x015fba0c` | ⚓ kStatBonus, kStatBonusAddr | `features\patches\status_tweaks.cc:103`<br>`features\windows\character_sheet.cc:77`<br>`ragnarok\globals.h:82` | bonus |
| `0x015fba10` | (littéral) | `features\patches\status_tweaks.cc:103` |  |
| `0x015fba14` | (littéral) | `features\patches\status_tweaks.cc:103` |  |
| `0x015fba18` | (littéral) | `features\patches\status_tweaks.cc:104` |  |
| `0x015fba1c` | (littéral) | `features\patches\status_tweaks.cc:104` |  |
| `0x015fba20` | (littéral) | `features\patches\status_tweaks.cc:104` |  |
| `0x015fba24` | ⚓ kStatBase, kStatBaseAddr | `features\patches\status_tweaks.cc:101`<br>`features\windows\character_sheet.cc:76`<br>`ragnarok\globals.h:81` | STR..LUK base (step 4) |
| `0x015fba28` | (littéral) | `features\patches\status_tweaks.cc:101` |  |
| `0x015fba2c` | (littéral) | `features\patches\status_tweaks.cc:101` |  |
| `0x015fba30` | (littéral) | `features\patches\status_tweaks.cc:102` |  |
| `0x015fba34` | (littéral) | `features\patches\status_tweaks.cc:102` |  |
| `0x015fba38` | (littéral) | `features\patches\status_tweaks.cc:102` |  |
| `0x015fba3c` | kRaiseCost | `features\patches\status_tweaks.cc:105`<br>`features\windows\character_sheet.cc:78` | cout de montee |
| `0x015fba40` | (littéral) | `features\patches\status_tweaks.cc:105` |  |
| `0x015fba44` | (littéral) | `features\patches\status_tweaks.cc:105` |  |
| `0x015fba48` | (littéral) | `features\patches\status_tweaks.cc:106` |  |
| `0x015fba4c` | (littéral) | `features\patches\status_tweaks.cc:106` |  |
| `0x015fba50` | (littéral) | `features\patches\status_tweaks.cc:106` |  |
| `0x015fba54` | kAspdRaw, kFlee | `features\patches\status_tweaks.cc:113`<br>`features\windows\character_sheet.cc:85` |  |
| `0x015fba58` | kAtk1 | `features\patches\status_tweaks.cc:107`<br>`features\windows\character_sheet.cc:80` |  |
| `0x015fba5c` | kMdefSoft | `features\patches\status_tweaks.cc:109`<br>`features\windows\character_sheet.cc:83` |  |
| `0x015fba64` | kDefSoft | `features\patches\status_tweaks.cc:108`<br>`features\windows\character_sheet.cc:81` |  |
| `0x015fba68` | kDefSoft | `features\patches\status_tweaks.cc:108`<br>`features\windows\character_sheet.cc:81` |  |
| `0x015fba6c` | kAtk1 | `features\patches\status_tweaks.cc:107`<br>`features\windows\character_sheet.cc:80` |  |
| `0x015fba70` | kMatkMax, kMatkMin | `features\patches\status_tweaks.cc:110`<br>`features\windows\character_sheet.cc:82` |  |
| `0x015fba74` | kMatkMax, kMatkMin | `features\patches\status_tweaks.cc:110`<br>`features\windows\character_sheet.cc:82` |  |
| `0x015fba78` | kMdefSoft | `features\patches\status_tweaks.cc:109`<br>`features\windows\character_sheet.cc:83` |  |
| `0x015fba7c` | kHit | `features\patches\status_tweaks.cc:111`<br>`features\windows\character_sheet.cc:84` |  |
| `0x015fba80` | kFlee | `features\patches\status_tweaks.cc:112`<br>`features\windows\character_sheet.cc:85` |  |
| `0x015fba84` | kHit | `features\patches\status_tweaks.cc:111`<br>`features\windows\character_sheet.cc:84` |  |
| `0x015fba88` | kFlee | `features\patches\status_tweaks.cc:112`<br>`features\windows\character_sheet.cc:85` |  |
| `0x015fba8c` | kAmmoInvIndex | `features\windows\character_sheet.cc:97` | g_AmmoEquippedInvIndex (0 = aucune) |
| `0x015fba90` | ⚓ kZenyAddr | `ragnarok\globals.h:36` |  |
| `0x015fba9c` | kWeightMax | `features\overlays\basic_info.cc:67`<br>`features\patches\inventory_tweaks.cc:57`<br>`features\windows\inventory_viewer.cc:70` | max weight (raw) |
| `0x015fbaa0` | kWeightCur | `features\overlays\basic_info.cc:67`<br>`features\patches\inventory_tweaks.cc:56`<br>`features\windows\inventory_viewer.cc:69` | current weight (raw) |
| `0x015fbab0` | kInvListHead | `features\windows\character_sheet.cc:72`<br>`features\windows\inventory_viewer.cc:50`<br>`features\windows\make_item_window.cc:216`<br>`features\windows\weapon_refine_window.cc:77` | std::list<ItemSkillInfo> (session+0x16f0) |
| `0x015fbab4` | kInvCount | `features\windows\character_sheet.cc:73`<br>`features\windows\inventory_viewer.cc:51` | _Mysize |
| `0x015fbad8` | kStorageListHead | `features\windows\storage_window.cc:99`<br>`features\windows\storage_window.cc:653` |  |
| `0x015fbae0` | kCartListHead | `features\windows\cart_viewer.cc:54` | sentinelle std::list (head) |
| `0x015ff5b0` | kMyDealZeny | `features\windows\trade_window.cc:65` | int32 : zeny que j'offre |
| `0x015ff5b4` | kPtDealZeny | `features\windows\trade_window.cc:66` | int32 : zeny offert par le partenaire |
| `0x015ff7e0` | kNativeList | `ragnarok\skill_cooldowns.cc:28` | g_ShortCutCooldownList (objet std::list) |
| `0x015ff908` | kHp | `features\overlays\basic_info.cc:64`<br>`features\windows\character_sheet.cc:87` |  |
| `0x015ff90c` | kHp | `features\overlays\basic_info.cc:64`<br>`features\windows\character_sheet.cc:87` |  |
| `0x015ff910` | kSp | `features\overlays\basic_info.cc:65`<br>`features\windows\character_sheet.cc:88` |  |
| `0x015ff914` | kSp | `features\overlays\basic_info.cc:65`<br>`features\windows\character_sheet.cc:88` |  |
| `0x015ffd14` | kShowEquipFlag | `features\windows\character_sheet.cc:122` | 1 = équip visible des autres (validé live) |
| `0x015ffd60` | kNormalSlots | `features\windows\char_select.cc:135` |  |
| `0x015ffd64` | kPremiumSlots | `features\windows\char_select.cc:136` |  |
| `0x015ffd68` | kBillingSlots | `features\windows\char_select.cc:137` |  |
| `0x015ffd6c` | kCreatableSlots | `features\windows\char_select.cc:138` |  |
| `0x015ffd78` | ⚓ kStateHolderAddr | `ragnarok\lua.h:33` |  |
| `0x015ffd80` | kGridIds | `features\overlays\status_icon_bar.cc:51` | int[100] laid-out ids (-1 empty) |
| `0x015fffa0` | kDropLockGlobal | `features\windows\inventory_viewer.cc:462` |  |
| `0x015fffa1` | kSafeCheckFlag | `features\windows\vending_window.cc:346` | octet, persistant |
| `0x015fffb4` | kNotifyFlag | `features\windows\vending_window.cc:53` |  |
| `0x015fffc0` | kBankVault | `features\windows\bank_window.cc:36` | s64 |
| `0x016004fc` | kOwnTitleId | `features\windows\character_sheet.cc:1844` | g_Own_TitleId : titre ÉQUIPÉ (0 = aucun) |
| `0x01600500` | kOwnTitleBegin | `features\windows\character_sheet.cc:1845` | std::vector<int> g_OwnTitleList : begin (possédés) |
| `0x01600504` | kOwnTitleEnd | `features\windows\character_sheet.cc:1846` | .. end |
| `0x01600553` | kDealLockGlobal, kFavFlag | `features\patches\inventory_tweaks.cc:105`<br>`features\windows\inventory_viewer.cc:463` | FAV-mode global (matches FUN_00946da0) |
| `0x01602278` | kLeftHandEquipOpt | `features\windows\inventory_viewer.cc:102` | DAT_01602278 : option client "équip main gauche" active ? |
| `0x01602324` | kOverweightPct | `features\patches\inventory_tweaks.cc:58`<br>`features\windows\inventory_viewer.cc:71` | red-tint % threshold |
| `0x01602354` | kInvExpansion | `features\patches\inventory_tweaks.cc:73`<br>`features\windows\inventory_viewer.cc:72` | server-sent inventory expansion (slots - 200) |
| `0x016023e4` | kSnapName | `features\windows\vending_window.cc:246` | std::string : nom de la boutique |
| `0x01602400` | kSnapBegin | `features\windows\vending_window.cc:244` | vecteur d'ItemSkillInfo |
| `0x01602404` | kSnapEnd | `features\windows\vending_window.cc:245` |  |
| `0x016024c0` | kCostumeHideFlag | `features\windows\character_sheet.cc:123` | 0 = costumes affichés (validé live) |
| `0x01602674` | kSoundModeVal | `features\overlays\login_parade.cc:58` | SOUNDMODE (registre, boot) : 0 = son coupé au setup |
