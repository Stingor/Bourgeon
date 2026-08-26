# Portage des adresses natives : client 2025-07-16 -> 2026-07-07

Relevé du 2026-08-26. Part du manifeste existant
[`docs/address_manifest.md`](../address_manifest.md) (**784 adresses**, généré
par `tools/gen_address_manifest.py`) et y ajoute la correspondance 2026.

🔴 Ce document ne remplace pas le manifeste : il le complète. Le manifeste
reste la check-list de référence, avec les signatures et les sites d'appel.

## Résultat

| vecteur | portées | comment |
|---|---|---|
| **RTTI (classe + slot)** | 67 | une méthode virtuelle se retrouve par sa classe et son index |
| **chaîne référencée** | 22 | ≥ 2 chaînes communes, tailles compatibles |
| **propagation par appels** | 61 | fonction appariée, adresse déduite par offset |
| **globale (opérande mémoire appariée)** | 7 | même nombre de références exigé |
| **TOTAL** | **157 / 784** (20 %) | |

Restent **632** adresses, dont **370 en zone de données** (globales, tables,
vtables) : ni chaîne, ni vtable, ni appelant ne les atteint. Ce sont les plus
coûteuses — il faut retrouver l'instruction qui les référence.

## 🔴 Ce qui porte d'un build à l'autre, et ce qui ne porte pas

| méthode | rendement | verdict |
|---|---|---|
| signature d'octets (prologue) | 4/18 | ❌ **ne porte pas** — le compilateur réécrit |
| **RTTI (classe + slot)** | **67, aucun échec** | ✅✅ **le vecteur le plus sûr** |
| chaîne référencée | 22 | ✅ solide, mais seules ~215 fonctions en ont |
| propagation par appels | 61 | ✅ tant qu'on exige un compte d'appels identique |

🔴 **Le RTTI est le bon outil.** Le 2025 expose 1593 vtables, le 2026 en a
1341 : une méthode virtuelle se retrouve par `(classe, slot)` sans dépendre
d'un seul octet. C'est ce qui a permis de nommer 2527 méthodes dans l'IDB
2026, et c'est le vecteur à tenter en premier pour toute nouvelle adresse.

⚠ **Ne PAS pousser la propagation trop loin.** À la deuxième itération, 83
conflits ont dû être écartés : au-delà, elle produit plus de bruit que de
résultat. Le garde qui la rend utilisable est double — même nombre d'appels
dans les deux fonctions, et une cible 2026 ne peut correspondre qu'à une
seule fonction 2025.

## Contrôle de qualité

Confronté aux correspondances trouvées **à la main** avant ce portage :

| ancre | 2025 | attendu | automatique |
|---|---|---|---|
| `GetTalkType` | `0x00d5e590` | `0x00c71310` | ✅ |
| `SendMsg` | `0x00a4ad20` | `0x00a18a20` | ✅ |
| `CSession_ctor` | `0x00d57780` | `0x00c6a730` | ✅ |
| `NetConnection_Connect` | `0x00c13fc0` | `0x00bdeca0` | ✅ |
| `PacketLenLookup` | `0x00aa7b00` | `0x00aa4290` | — non couvert |
| `RecvOpcodeReader` | `0x00c144b0` | `0x00bdee70` | — non couvert |
| `RecvBufferReset` | `0x00c148b0` | `0x00bdf3d0` | — non couvert |
| `ProcessPushButton` | `0x00a471e0` | `0x00a15160` | — non couvert |

**4 concordent, 0 divergent.** La méthode se tait plutôt que de se tromper —
c'est le comportement voulu : une adresse fausse coûte plus cher qu'une
adresse manquante.

## Correspondances portées

Fichier complet : `port_2025_2026.json`.

| 2025 | 2026 | vecteur | nom Bourgeon | signature |
|---|---|---|---|---|
| `0x004e52a0` | `0x004bb4f0` | propagation | `kStdStringCopyCtor` | __thiscall(dst, src), retn 4 |
| `0x004f1940` | `0x004e0ea0` | propagation | `kStdStringAssign, kStdStringAssignAddr` | __thiscall(this, src, len) |
| `0x0051b570` | `0x005f1f10` | texte | `kCheckStackAddr` | lua_checkstack(L, extra) |
| `0x0053f140` | `0x00615470` | rtti | `kEngNodeBlit` | __thiscall(node, x, y, w, h, ARGB*, colorkey) |
| `0x00550b10` | `0x006266b0` | propagation | `kRenderQueueInsert` | RenderQueue_InsertPrimitive — hooké (capture) |
| `0x00553e80` | `0x0062a260` | propagation | `kDepthScale` | Effect_DepthToScreenScale(ctx,_,invW) -> float |
| `0x005541b0` | `0x0062a500` | propagation | `kSceneProject` | Scene_ProjectWorldToScreen(ctx,_,world,view,&s |
| `0x0055c830` | `0x006326a0` | rtti | `kDX9DrawPrimRec` | vtbl +0x38 : draw mono-texture |
| `0x0055c8c0` | `0x00632750` | rtti | `kDX9DrawPrimDual` | vtbl +0x34 : draw BI-texture (aniso) |
| `0x005663d0` | `0x0063c4b0` | propagation | `kAtlasBuildAddr` |  |
| `0x00566b70` | `0x0063cc00` | propagation | `kAtlasGetCachedAddr` |  |
| `0x00568760` | `0x0063e950` | propagation | `kSpriteRef` | __thiscall(cache,path,0,0,1,0) -> ref (5 args! |
| `0x0059e950` | `0x006665c0` | texte | `kSaveJsonAddr` |  |
| `0x005a4300` | `0x00c09b30` | propagation | `kItemSkillInfoDtor` |  |
| `0x005c5950` | `0x00cca703` | propagation | `kSetOption` | SkillMgr_SetOption(mgr,key,val,0) ; key5=UI-lo |
| `0x0065ae30` | `0x006d6c80` | rtti | `(littéral)` |  |
| `0x006a1b20` | `0x0070cf80` | propagation | `kInfoCtorAddr, kItemSkillInfoCtor` |  |
| `0x006a2dd0` | `0x0070e100` | texte | `(littéral)` |  |
| `0x006f4800` | `0x00764220` | texte | `(littéral)` |  |
| `0x0070f4b0` | `0x0077ebf0` | propagation | `kActionGetFrameAddr` |  |
| `0x007110c0` | `0x00780790` | propagation | `kTerrainHeight` | Terrain_GetHeightAt(world,x,z)->float |
| `0x007289da` | `0x007974fa` | rtti | `(littéral)` |  |
| `0x0073adb0` | `0x007a7af0` | texte | `kIsLevelUseSkill` | __cdecl(id) : l'effet dépend-il du niveau ? |
| `0x0075f850` | `0x007aa6d0` | rtti | `(littéral)` |  |
| `0x0079d5e0` | `0x007e50d0` | rtti | `kSelCharRenderPatch` | mov ecx,[esi+0x120] |
| `0x0079d610` | `0x007e5120` | rtti | `kCharSelOnMsg` | vtbl+0x94, RET 0x18 |
| `0x007a6df0` | `0x007efdd0` | propagation | `kColorChip` | FUN_007a6df0(x,y,&r,&g,&b) -> colorchip.bmp pi |
| `0x007f9688` | `0x00838168` | rtti | `kSwapTitleImm` | push 0x8c imm32  (title x, centred) |
| `0x00815630` | `0x008538f0` | rtti | `(littéral)` |  |
| `0x008263a0` | `0x008645d0` | rtti | `kBalloonPaint` |  |
| `0x008303f0` | `0x004da1f0` | rtti | `kSetTextAddr` | CUIEdit_SetText |
| `0x0083d840` | `0x00880180` | rtti | `(littéral)` |  |
| `0x00857910` | `0x008954a0` | rtti | `kTabDrawOrig` | FUN_00857910 tab DrawContent |
| `0x00864690` | `0x008a2f50` | propagation | `kAddTab` | tab control AddTab(this, label, tooltip) |
| `0x00874af0` | `0x008b42a0` | rtti | `(littéral)` |  |
| `0x00880e7f` | `0x008c06af` | rtti | `kSnapJnz` | JNZ 0x00880e9f  (bytes 75 1e) |
| `0x008848d0` | `0x008c3940` | texte | `kOnMsgAddr` | UILoginWnd_OnMsg |
| `0x00897df1` | `0x008d5cd1` | propagation | `kRightIconImm` | mov [ebp-0x534],0xfb  (right icon x=251) |
| `0x00897e05` | `0x008d5ce5` | propagation | `kRightNameImm` | mov [ebp-0x544],0xa7  (right name x=167) |
| `0x00897ffb` | `0x008d5edb` | propagation | `(littéral)` | push 0x46 imm8 — slots 0-7 (2 columns) |
| `0x00898040` | `0x008d5f20` | propagation | `(littéral)` | push 0x46 imm8 — slots 8-9 (top headgear row) |
| `0x00898bc0` | `0x008d6a40` | propagation | `kDrawTitleBar` | __thiscall(this, char hasClose, char* title, i |
| `0x00898cc5` | `0x008d6b45` | propagation | `kTitleColorImm` | imm of PUSH 0xffffff in DrawTitleBar (title te |
| `0x00898cd2` | `0x008d6b52` | propagation | `kTitleWhiteY` | lea eax,[edi-0xd] disp8 (white y-off) |
| `0x00898cd5` | `0x008d6b55` | propagation | `kTitleWhiteX` | push 0x13  (white title x) |
| `0x00898cea` | `0x008d6b6a` | propagation | `kTitleBlackY` | lea eax,[edi-0xe] disp8 (black y-off) |
| `0x00898cef` | `0x008d6b6f` | propagation | `kTitleBlackX` | push 0x12  (black edge x) |
| `0x008b3427` | `0x008f5e47` | rtti | `kHighlightImm` | mov [ebp-0x128],0xf8 imm32 |
| `0x008b66a0` | `0x008f8d30` | texte | `kDrawOrig` | original UIStatusWnd::DrawContent |
| `0x008bf7d0` | `0x00903810` | rtti | `kMsgOrig` | FUN_008bf7d0 equip msg handler (ret 0x18 = SIX |
| `0x008c049c` | `0x009044dc` | rtti | `kSwapWidthImm` | push 0x118 imm32 (swap panel SetSize width) |
| `0x008c18b0` | `0x00905e20` | rtti | `kItemDescWndAddr` |  |
| `0x008c5860` | `0x00909fa0` | rtti | `kMsgOrig` | UINotifyItemObtainWnd_OnMsg |
| `0x008cb7c0` | `0x0090d2e0` | propagation | `kMsgOrig` | FUN_008cb7c0 status msg handler (ret 0x18 = SI |
| `0x008f3498` | `0x00931d98` | rtti | `kDialogBgBlitRet` | return addr of the dialog_bg blit |
| `0x008f5800` | `0x00933970` | rtti | `kOnDraw` | UIShortCutWnd::OnDraw (vtable+0x50) — contenu  |
| `0x008fc220` | `0x00939ee0` | rtti | `kChatWndProc` | __thiscall WndProc (vtable+0x94) |
| `0x008fd12a` | `0x0093adea` | rtti | `kChatWrapCaller` | return addr of the chat call site |
| `0x00901310` | `0x0093f110` | rtti | `kShortCutOnMsg` | UIShortCutWnd::OnMsg (vtable+0x94) |
| `0x009030c0` | `0x00940da0` | rtti | `kHideNative, kSetVisibleFn` | UIWnd_SetVisible (vtable+0x38, __thiscall) |
| `0x009137a0` | `0x0094e810` | rtti | `kDrawContent` | QuestTracker_DrawContent (hooked) |
| `0x0091e1f0` | `0x00958d20` | rtti | `(littéral)` |  |
| `0x009386d0` | `0x00973c40` | rtti | `kFavDropSlotImm` | imm8 of CMP EAX,3 in FUN_00938650 case 1 |
| `0x0093da20` | `0x009793b0` | rtti | `(littéral)` |  |
| `0x0093f1f9` | `0x0097aca9` | rtti | `(littéral)` | FUN_0093f100 SUB EAX,0x26   : scrollbar height |
| `0x0093f278` | `0x0097ad28` | rtti | `(littéral)` | FUN_0093f100 LEA [EBX-0x26] : item row count |
| `0x00946da0` | `0x009827a0` | rtti | `kDrawOrig` | original inventory DrawContent |
| `0x00947053` | `0x00982a53` | rtti | `(littéral)` | FUN_00946da0 SUB EAX,0x26   : grey separator L |
| `0x0094708b` | `0x00982a8b` | rtti | `(littéral)` | FUN_00946da0 SUB EAX,0x26   : slot-cell grid r |
| `0x0094afb0` | `0x009867f0` | rtti | `(littéral)` |  |
| `0x0094ee20` | `0x0098a5c0` | rtti | `(littéral)` |  |
| `0x009500f0` | `0x0098b880` | rtti | `(littéral)` |  |
| `0x00955530` | `0x00991260` | rtti | `kMsgOrig` | FUN_00955530 inventory message handler |
| `0x00955980` | `0x009916b0` | rtti | `kMaxWidthImm` | MOV EDI,0x140 imm (resize max W) |
| `0x0095598b` | `0x009916bb` | rtti | `kMaxHeightImm` | MOV EDX,0x0f0 imm (resize max H) |
| `0x009559b9` | `0x009916e9` | rtti | `(littéral)` | case 0xe     LEA [ECX+0x26] : tabH + reserve ( |
| `0x009559c3` | `0x009916f3` | rtti | `(littéral)` | case 0xe     SUB EAX,0x26   : available grid h |
| `0x009559d7` | `0x00991707` | rtti | `(littéral)` | case 0xe     LEA [EAX+0x26] : FINAL window hei |
| `0x00955a69` | `0x00991799` | rtti | `(littéral)` | case 0xe     SUB EAX,0x26   : scrollbar height |
| `0x00977e80` | `0x009b41c0` | rtti | `kDrawContent` | UINewSkillListWnd::DrawContent |

*(157 au total)*
