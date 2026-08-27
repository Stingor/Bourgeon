# Plan d'application du portage 2025 -> 2026

GENERE par `scripts/apply_plan.py` a partir de `merged_pairs.json`,
`port_2025_2026.json` et `docs/address_manifest.md`. **Ne rien editer ici.**

⚠ **Rien n'a ete applique au code.** Ce document dit seulement OU il faudra
intervenir, le jour ou le client 2026 deviendra la cible. Le client de
production reste le 2025-07-16.

- adresses connues et utilisees par Bourgeon : **200**
- fichiers concernes : **70**
- sites a modifier : **277**

🔴 Les lignes marquees **SUSPECT** viennent du portage precedent et ont
echoue au controle collision/taille : les verifier AVANT de les appliquer
(voir `port_suspects.json`).

## `features/patches/inventory_tweaks.cc` — 25 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 121 | `0x0093f1f9` | `0x0097aca9` | (littéral) | rtti |
| 122 | `0x009559b9` | `0x009916e9` | (littéral) | rtti |
| 123 | `0x009559c3` | `0x009916f3` | (littéral) | rtti |
| 124 | `0x009559d7` | `0x00991707` | (littéral) | rtti |
| 125 | `0x00955a69` | `0x00991799` | (littéral) | rtti |
| 126 | `0x0094708b` | `0x00982a8b` | (littéral) | rtti |
| 127 | `0x00947053` | `0x00982a53` | (littéral) | rtti |
| 130 | `0x0093f278` | `0x0097ad28` | (littéral) | rtti |
| 141 | `0x007a6df0` | `0x007efdd0` | kColorChip | propagation |
| 157 | `0x00857910` | `0x008954a0` | kTabDrawOrig | rtti |
| 167 | `0x00955530` | `0x00991260` | kMsgOrig | rtti |
| 168 | `0x00864690` | `0x008a2f50` | kAddTab | propagation |
| 183 | `0x009386d0` | `0x00973c40` | kFavDropSlotImm | rtti |
| 229 | `0x00898cc5` | `0x008d6b45` | kTitleColorImm | propagation |
| 36 | `0x00946da0` | `0x009827a0` | kDrawOrig | rtti |
| 37 | `0x00955980` | `0x009916b0` | kMaxWidthImm | rtti |
| 38 | `0x0095598b` | `0x009916bb` | kMaxHeightImm | rtti |
| 46 | `0x00a25a70` | `0x009f46d0` | kDrawText | propagation |
| 47 | `0x00a21c90` | `0x009f0750` | kMeasureW | propagation |
| 48 | `0x00a1d260` | `0x009eade0` | kBlit, kBlitImageToNode | propagation |
| 49 | `0x00a1d460` | `0x009eb030` | kFill | propagation |
| 53 | `0x00a948d0` | `0x00a91460` | kFmtComma, kFmtComma32 | **SUSPECT** propagation |
| 58 | `0x01602324` | `0x014be9c4` | kOverweightPct | opcode+multi |
| 706 | `0x00946da0` | `0x009827a0` | kDrawOrig | rtti |
| 73 | `0x01602354` | `0x014be9e8` | kInvExpansion | opcode+multi |

## `features/windows/character_sheet.cc` — 18 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 103 | `0x015fb9f8` | `0x014b89b0` | kBaseLvl, kJobLevel, kJobLevelAddr, kOwn | opcode+multi |
| 1031 | `0x00d5e3c0` | `0x007d8810` | kSkillGetUseLevel | **SUSPECT** propagation |
| 1032 | `0x0073adb0` | `0x007a7af0` | kIsLevelUseSkill | texte |
| 119 | `0x015fba8c` | `0x014b8a44` | kAmmoInvIndex | opcode+multi |
| 1232 | `0x01254d70` | `0x01127350` | kCGuildMgrPtr | opcode |
| 138 | `0x00d7fa90` | `0x007a6b50` | kGetInvItemAddr, kSkillEntryFill | **SUSPECT** propagation |
| 2137 | `0x016004fc` | `0x014bce08` | kOwnTitleId | opcode+multi |
| 2142 | `0x00d89ed0` | `0x00c9d040` | kTitleGetStr | texte |
| 2792 | `0x0073a1f0` | `0x007a6f50` | kGetSkillNameLua | opcode+multi |
| 2828 | `0x00d5b580` | `0x00c6e5f0` | kGetJob | propagation |
| 424 | `0x0159c23c` | `0x0146e944` | kGuildIsMaster | opcode |
| 425 | `0x0159c230` | `0x0146e938` | kGuildIdAddr, kOwnGuildId | opcode+fenetre |
| 438 | `0x0159c1f0` | `0x0146e8f8` | kGuildOnlineNum | opcode |
| 838 | `0x00d5b580` | `0x00c6e5f0` | kGetJob | propagation |
| 840 | `0x00d5bb40` | `0x00c6eb90` | kJobDisplayName, kJobNameAddr, kJobResNa | opcode+multi |
| 853 | `0x00d5bb40` | `0x00c6eb90` | kJobDisplayName, kJobNameAddr, kJobResNa | opcode+multi |
| 88 | `0x015fb9a8` | `0x014b8960` | kOwnCharId, kOwnCharIdAddr, kOwnCid | opcode |
| 96 | `0x015fb9f4` | `0x014b89ac` | kStatusPoint, kStatusPt | opcode+multi |

## `features/windows/chat_window.cc` — 15 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 119 | `0x015fa3c0` | `0x014b73b0` | kJobNameCtx, kOptionContextAddr, kSessio | opcode+multi |
| 125 | `0x00d5e590` | `0x00c71310` | kLookupSlashCmd | texte |
| 128 | `0x0159c230` | `0x0146e938` | kGuildIdAddr, kOwnGuildId | opcode+fenetre |
| 129 | `0x00d5cf50` | `0x00c6ffa0` | kPartyMemberCount | fenetre+multi |
| 130 | `0x0159c07c` | `0x0146e780` | kClanStatePtr | opcode+multi |
| 460 | `0x004e52a0` | `0x004bb4f0` | kStdStringCopyCtor | propagation |
| 55 | `0x00a4ad20` | `0x00a18a20` | kChatActionAddr, kChatAddLine | texte |
| 569 | `0x00a2cc20` | `0x009fb970` | kWhisperPivotAddr | propagation |
| 5709 | `0x009b9b9b` | `0x009d879b` | (littéral) | rtti |
| 583 | `0x00d7fe40` | `0x00c92eb0` | kOwnGetCharName | opcode |
| 59 | `0x0131f6b0` | `0x011f1aa4` | kNewChatWndPtr | globale |
| 785 | `0x00c7a7eb` | `0x004d519b` | kPartyPrefixByte | texte |
| 786 | `0x00c7a822` | `0x004d51d2` | kGuildPrefixByte | texte |
| 787 | `0x00c7a89a` | `0x004d524a` | kAllyPrefixByte | texte |
| 95 | `0x004f1940` | `0x004e0ea0` | kStdStringAssign, kStdStringAssignAddr | propagation |

## `ragnarok/configuration.h` — 15 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 10 | `0x0093da20` | `0x009793b0` | (littéral) | rtti |
| 11 | `0x0094afb0` | `0x009867f0` | (littéral) | rtti |
| 112 | `0x00c74a80` | `0x004cef40` | (littéral) | rtti |
| 18 | `0x00815630` | `0x008538f0` | (littéral) | rtti |
| 20 | `0x0065ae30` | `0x006d6c80` | (littéral) | rtti |
| 34 | `0x006a2dd0` | `0x0070e100` | (littéral) | texte |
| 39 | `0x0091e1f0` | `0x00958d20` | (littéral) | rtti |
| 41 | `0x006f4800` | `0x00764220` | (littéral) | texte |
| 62 | `0x0075f850` | `0x007aa6d0` | (littéral) | rtti |
| 66 | `0x0094ee20` | `0x0098a5c0` | (littéral) | rtti |
| 67 | `0x009500f0` | `0x0098b880` | (littéral) | rtti |
| 80 | `0x00d57780` | `0x00c6a730` | (littéral) | texte |
| 81 | `0x00d5e590` | `0x00c71310` | kLookupSlashCmd | texte |
| 85 | `0x00a4ad20` | `0x00a18a20` | kChatActionAddr, kChatAddLine | texte |
| 87 | `0x00c13fc0` | `0x00bdeca0` | (littéral) | **SUSPECT** rtti |

## `features/patches/status_tweaks.cc` — 12 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 114 | `0x015fb9f4` | `0x014b89ac` | kStatusPoint, kStatusPt | opcode+multi |
| 344 | `0x008b66a0` | `0x008f8d30` | kDrawOrig | texte |
| 52 | `0x008b66a0` | `0x008f8d30` | kDrawOrig | texte |
| 55 | `0x008cb7c0` | `0x0090d2e0` | kMsgOrig | propagation |
| 67 | `0x00898bc0` | `0x008d6a40` | kDrawTitleBar | propagation |
| 68 | `0x00a25a70` | `0x009f46d0` | kDrawText | propagation |
| 69 | `0x00a27b50` | `0x009f6d70` | kDrawTextR | propagation |
| 70 | `0x00a1d260` | `0x009eade0` | kBlit, kBlitImageToNode | propagation |
| 78 | `0x00898cd5` | `0x008d6b55` | kTitleWhiteX | propagation |
| 79 | `0x00898cd2` | `0x008d6b52` | kTitleWhiteY | propagation |
| 80 | `0x00898cef` | `0x008d6b6f` | kTitleBlackX | propagation |
| 81 | `0x00898cea` | `0x008d6b6a` | kTitleBlackY | propagation |

## `features/patches/chat.cc` — 11 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 158 | `0x008fd12a` | `0x0093adea` | kChatWrapCaller | rtti |
| 191 | `0x00a1d260` | `0x009eade0` | kBlit, kBlitImageToNode | propagation |
| 192 | `0x008f3498` | `0x00931d98` | kDialogBgBlitRet | rtti |
| 366 | `0x0053f140` | `0x00615470` | kEngNodeBlit | rtti |
| 531 | `0x008fc220` | `0x00939ee0` | kChatWndProc | rtti |
| 833 | `0x0083d840` | `0x00880180` | (littéral) | **SUSPECT** rtti |
| 836 | `0x0083d840` | `0x00880180` | (littéral) | **SUSPECT** rtti |
| 877 | `0x00a21c90` | `0x009f0750` | kMeasureW | propagation |
| 880 | `0x00a21c90` | `0x009f0750` | kMeasureW | propagation |
| 891 | `0x008fc220` | `0x00939ee0` | kChatWndProc | rtti |
| 913 | `0x00a1d260` | `0x009eade0` | kBlit, kBlitImageToNode | propagation |

## `features/overlays/skill_bar.cc` — 10 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 149 | `0x008f5800` | `0x00933970` | kOnDraw | rtti |
| 158 | `0x009030c0` | `0x00940da0` | kHideNative, kSetVisibleFn | rtti |
| 211 | `0x00901310` | `0x0093f110` | kShortCutOnMsg | rtti |
| 257 | `0x00d7fa90` | `0x007a6b50` | kGetInvItemAddr, kSkillEntryFill | **SUSPECT** propagation |
| 455 | `0x00d80950` | `0x00c93b30` | kGetHotKey, kGetHotKeyAddr | texte |
| 58 | `0x005c5950` | `0x00cca703` | kSetOption | **SUSPECT** propagation |
| 600 | `0x00d7fa90` | `0x007a6b50` | kGetInvItemAddr, kSkillEntryFill | **SUSPECT** propagation |
| 671 | `0x00d7fa90` | `0x007a6b50` | kGetInvItemAddr, kSkillEntryFill | **SUSPECT** propagation |
| 69 | `0x00d5a980` | `0x00a3c3d0` | kGetSkillInfo | **SUSPECT** propagation |
| 97 | `0x0073a1f0` | `0x007a6f50` | kGetSkillNameLua | opcode+multi |

## `features/fx/wand_bolt.cc` — 10 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 106 | `0x00c58e90` | `0x00c27740` | kSetBodySpr | propagation |
| 107 | `0x00c55bf0` | `0x00c24500` | kSetBodyAct | propagation |
| 14 | `0x00c6d9d0` | `0x00a3c3d0` | kArrowSpawn | **SUSPECT** propagation |
| 160 | `0x00c5e372` | `0x00c2c822` | kDamageDueTimeCall | propagation |
| 36 | `0x00d4245d` | `0x00c54ded` | (littéral) | rtti |
| 37 | `0x00d425ce` | `0x00c54f5e` | (littéral) | rtti |
| 38 | `0x00d41c5f` | `0x00c5460f` | (littéral) | rtti |
| 40 | `0x00d8a1d0` | `0x004e09d0` | kItemIdToClass, kItemIdToWeaponClass | propagation |
| 53 | `0x00dabb10` | `0x00cbbe70` | kArrowEffectUpdate | rtti |
| 54 | `0x00db01b0` | `0x00cc0320` | kArrowEffectOnMsg | rtti |

## `features/overlays/basic_info.cc` — 9 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 1154 | `0x00d5b580` | `0x00c6e5f0` | kGetJob | propagation |
| 123 | `0x015fb9f8` | `0x014b89b0` | kBaseLvl, kJobLevel, kJobLevelAddr, kOwn | opcode+multi |
| 135 | `0x00d5b580` | `0x00c6e5f0` | kGetJob | propagation |
| 1367 | `0x00d5b580` | `0x00c6e5f0` | kGetJob | propagation |
| 137 | `0x00d5bb40` | `0x00c6eb90` | kJobDisplayName, kJobNameAddr, kJobResNa | opcode+multi |
| 2966 | `0x0131f6c4` | `0x011f1ab4` | kBasicInfoPtr | opcode+multi |
| 797 | `0x00bb4170` | `0x00b7f8f0` | kEffLoadStr | texte |
| 798 | `0x00bced10` | `0x00b99fe0` | kEffUpdateKF | multi |
| 935 | `0x00a8e800` | `0x00a8ab30` | kResAddRef, kTexAddRef | propagation |

## `ragnarok/pet.cc` — 9 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 102 | `0x00d806a0` | `0x00c93880` | kActorFindByGid | opcode+multi |
| 19 | `0x015fb3b0` | `0x014b8368` | kAid, kOwnPetAid | opcode+multi |
| 22 | `0x015fb3d8` | `0x014b8390` | kAccessory | opcode+multi |
| 23 | `0x015fb3dc` | `0x014b8394` | kClass | opcode+fenetre+multi |
| 25 | `0x015fb3e4` | `0x014b839c` | kHunger | opcode+multi |
| 26 | `0x015fb3ec` | `0x014b83a4` | kIntimacy | opcode+multi |
| 27 | `0x015fb3f0` | `0x014b83a8` | kEggInvIndex | opcode+multi |
| 28 | `0x015fb3f4` | `0x014b83ac` | kPrevHunger | opcode+multi |
| 36 | `0x0131f940` | `0x011f1d24` | kMailWriteWnd, kWriteWndPtr | opcode+multi |

## `features/patches/equip_tweaks.cc` — 8 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 110 | `0x008bf7d0` | `0x00903810` | kMsgOrig | rtti |
| 63 | `0x00897df1` | `0x008d5cd1` | kRightIconImm | propagation |
| 64 | `0x00897e05` | `0x008d5ce5` | kRightNameImm | propagation |
| 65 | `0x00897ffb` | `0x008d5edb` | (littéral) | propagation |
| 66 | `0x00898040` | `0x008d5f20` | (littéral) | propagation |
| 82 | `0x008b3427` | `0x008f5e47` | kHighlightImm | rtti |
| 92 | `0x008c049c` | `0x009044dc` | kSwapWidthImm | rtti |
| 93 | `0x007f9688` | `0x00838168` | kSwapTitleImm | rtti |

## `features/windows/entity_context_menu.cc` — 8 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 156 | `0x0159c230` | `0x0146e938` | kGuildIdAddr, kOwnGuildId | opcode+fenetre |
| 157 | `0x0159c23c` | `0x0146e944` | kGuildIsMaster | opcode |
| 158 | `0x015ff804` | `0x014bc3a0` | kInPartyFlag | opcode+multi |
| 159 | `0x015fb3b0` | `0x014b8368` | kAid, kOwnPetAid | opcode+multi |
| 160 | `0x015fa3c0` | `0x014b73b0` | kJobNameCtx, kOptionContextAddr, kSessio | opcode+multi |
| 37 | `0x00c6e990` | `0x004c8110` | kShowEntityContextMenu | texte |
| 96 | `0x00a727f0` | `0x00a395f0` | kActiveIdSetContains | opcode+multi |
| 99 | `0x00a69eb0` | `0x00a329a0` | kActorListFindByGid, kFindByGID | propagation |

## `ragnarok/emotion_hotkey.cc` — 6 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 20 | `0x015fa3c0` | `0x014b73b0` | kJobNameCtx, kOptionContextAddr, kSessio | opcode+multi |
| 39 | `0x004f1940` | `0x004e0ea0` | kStdStringAssign, kStdStringAssignAddr | propagation |
| 43 | `0x0059e950` | `0x006665c0` | kSaveJsonAddr | texte |
| 53 | `0x0159c230` | `0x0146e938` | kGuildIdAddr, kOwnGuildId | opcode+fenetre |
| 54 | `0x0159c07c` | `0x0146e780` | kClanStatePtr | opcode+multi |
| 55 | `0x00d5cf50` | `0x00c6ffa0` | kPartyMemberCount | fenetre+multi |

## `features/fx/ez_effect_capture.cc` — 5 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 23 | `0x005541b0` | `0x0062a500` | kSceneProject | propagation |
| 24 | `0x00553e80` | `0x0062a260` | kDepthScale | propagation |
| 26 | `0x00b666d0` | `0x00b44a60` | kEzEffectDraw | rtti |
| 27 | `0x00550b10` | `0x006266b0` | kRenderQueueInsert | propagation |
| 89 | `0x00bc2de1` | `0x00b8e631` | kEffectJumpDefault | rtti |

## `features/overlays/status_icon_bar.cc` — 5 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 114 | `0x00c93cb0` | `0x004efd00` | kHitTestFn | texte |
| 47 | `0x00d87380` | `0x00c9a4d0` | kGetEFSTImg | texte |
| 48 | `0x00568760` | `0x0063e950` | kSpriteRef | propagation |
| 53 | `0x015ffd80` | `0x014bc6f4` | kGridIds | multi |
| 83 | `0x00b5ed20` | `0x00b3d970` | kRenderType0 | propagation |

## `features/windows/char_select.cc` — 5 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 122 | `0x0079d610` | `0x007e5120` | kCharSelOnMsg | rtti |
| 322 | `0x00a31a30` | `0x00a00650` | (littéral) | opcode |
| 325 | `0x00a31a30` | `0x00a00650` | (littéral) | opcode |
| 398 | `0x00d5bb40` | `0x00c6eb90` | kJobDisplayName, kJobNameAddr, kJobResNa | opcode+multi |
| 994 | `0x00d7fd30` | `0x00c92da0` | (littéral) | propagation |

## `ragnarok/lua.h` — 4 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 33 | `0x015ffd78` | `0x014bc6ec` | kStateHolderAddr | opcode |
| 53 | `0x0051b570` | `0x005f1f10` | kCheckStackAddr | texte |
| 60 | `0x00a9a7d0` | `0x00a97ca0` | kCallGlobalVaAddr | opcode |
| 61 | `0x00a9bc90` | `0x00a99160` | kExecFileAddr | texte |

## `features/fx/weapon_dual_sprites.cc` — 4 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 19 | `0x00d403a0` | `0x00c52e40` | kBuildWeaponLayers | rtti |
| 21 | `0x00a8e800` | `0x00a8ab30` | kResAddRef, kTexAddRef | propagation |
| 22 | `0x00a8f910` | `0x00a8bae0` | kResRelease | propagation |
| 23 | `0x00d8a1d0` | `0x004e09d0` | kItemIdToClass, kItemIdToWeaponClass | propagation |

## `features/windows/inventory_viewer.cc` — 4 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 554 | `0x0131f6b0` | `0x011f1aa4` | kNewChatWndPtr | globale |
| 73 | `0x01602324` | `0x014be9c4` | kOverweightPct | opcode+multi |
| 74 | `0x01602354` | `0x014be9e8` | kInvExpansion | opcode+multi |
| 86 | `0x00a948d0` | `0x00a91460` | kFmtComma, kFmtComma32 | **SUSPECT** propagation |

## `ragnarok/globals.h` — 4 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 172 | `0x015fb9f8` | `0x014b89b0` | kBaseLvl, kJobLevel, kJobLevelAddr, kOwn | opcode+multi |
| 197 | `0x00dbbc4f` | `0x00cca6d3` | kGameOperatorNewAddr | propagation |
| 198 | `0x00dbbc7f` | `0x00cca703` | kClientOperatorDelete, kGameOperatorDele | **SUSPECT** propagation |
| 26 | `0x015fa3c0` | `0x014b73b0` | kJobNameCtx, kOptionContextAddr, kSessio | opcode+multi |

## `ragnarok/homunculus.cc` — 4 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 18 | `0x015ff918` | `0x014bc4b4` | kAid | opcode+multi |
| 22 | `0x015ff95c` | `0x014bc4f8` | kClass | opcode+fenetre+multi |
| 35 | `0x015ff9b4` | `0x014bc54c` | kPresent | opcode+multi |
| 67 | `0x015fa3c0` | `0x014b73b0` | kJobNameCtx, kOptionContextAddr, kSessio | opcode+multi |

## `features/fx/palette_inject.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 43 | `0x00d3dc90` | `0x00c506f0` | kRebuildBodyPalettePath | rtti |
| 61 | `0x00d3ded0` | `0x00c50950` | kRebuildHeadPalette | rtti |
| 72 | `0x004f1940` | `0x004e0ea0` | kStdStringAssign, kStdStringAssignAddr | propagation |

## `features/windows/navigation_window.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 60 | `0x00b314f0` | `0x00b0f8d0` | kFnSearchRoute, kNaviRoute | opcode+multi |
| 68 | `0x0131f6b0` | `0x011f1aa4` | kNewChatWndPtr | globale |
| 71 | `0x004f1940` | `0x004e0ea0` | kStdStringAssign, kStdStringAssignAddr | propagation |

## `ragnarok/render.h` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 53 | `0x00566b70` | `0x0063cc00` | kAtlasGetCachedAddr | propagation |
| 54 | `0x005663d0` | `0x0063c4b0` | kAtlasBuildAddr | propagation |
| 58 | `0x0070f4b0` | `0x0077ebf0` | kActionGetFrameAddr | propagation |

## `features/windows/make_item_window.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 230 | `0x006a1b20` | `0x0070cf80` | kInfoCtorAddr, kItemSkillInfoCtor | propagation |
| 231 | `0x005a4300` | `0x00c09b30` | kItemSkillInfoDtor | propagation |
| 374 | `0x006a6570` | `0x00711a00` | kInfoSetIdAddr, kItemSkillInfoSetId | opcode |

## `ragnarok/uiwnd.h` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 26 | `0x00a47b90` | `0x00a15af0` | kFindWindowAddr | propagation |
| 58 | `0x00a2e770` | `0x009fd250` | kCloseWindowAddr | propagation |
| 99 | `0x0131f6bc` | `0x011f1aac` | kInventoryWndSlot | opcode+multi |

## `ui/game_texture.h` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 33 | `0x00a90350` | `0x00a8c520` | kGet | propagation |
| 34 | `0x00a9f030` | `0x00a9b700` | kMakeKey | propagation |
| 35 | `0x00a8d4a0` | `0x00a89910` | kLoad | propagation |

## `features/patches/damage_name_fix.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 14 | `0x00c5dfc0` | `0x00c2c470` | kProcessDamageAction | propagation |
| 15 | `0x00c4d0a7` | `0x00c1a357` | kProcessDamageCall | rtti |
| 19 | `0x00dbbc7f` | `0x00cca703` | kClientOperatorDelete, kGameOperatorDele | **SUSPECT** propagation |

## `features/overlays/minimap.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 122 | `0x00c74fc2` | `0x004cf482` | kDrawMiniMapCall | rtti |
| 123 | `0x00c74fc7` | `0x004cf487` | kDrawMiniMapAfter | rtti |
| 421 | `0x00b314f0` | `0x00b0f8d0` | kFnSearchRoute, kNaviRoute | opcode+multi |

## `ragnarok/game_settings.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 154 | `0x00554070` | `0x004c5a40` | kSpriteTexFactoryGetAddr | multi |
| 208 | `0x00d78970` | `0x00c8bab0` | kOptionSaveAddr | texte |
| 209 | `0x015fa3c0` | `0x014b73b0` | kJobNameCtx, kOptionContextAddr, kSessio | opcode+multi |

## `features/windows/char_diagnostics.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 36 | `0x015fb9a8` | `0x014b8960` | kOwnCharId, kOwnCharIdAddr, kOwnCid | opcode |
| 39 | `0x015fb9f4` | `0x014b89ac` | kStatusPoint, kStatusPt | opcode+multi |
| 96 | `0x00d5bb40` | `0x00c6eb90` | kJobDisplayName, kJobNameAddr, kJobResNa | opcode+multi |

## `features/windows/rodex_window.cc` — 3 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 116 | `0x0131ecdc` | `0x011f11bc` | kRodexMgrPtr | opcode+fenetre+multi |
| 337 | `0x00d00010` | `0x0056bf80` | kApplyCheckNameAck | opcode+multi |
| 47 | `0x0131f940` | `0x011f1d24` | kMailWriteWnd, kWriteWndPtr | opcode+multi |

## `ragnarok/user_hotkey.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 30 | `0x00d80950` | `0x00c93b30` | kGetHotKey, kGetHotKeyAddr | texte |
| 53 | `0x004f1940` | `0x004e0ea0` | kStdStringAssign, kStdStringAssignAddr | propagation |

## `features/fx/ground_paint.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 74 | `0x0055c830` | `0x006326a0` | kDX9DrawPrimRec | rtti |
| 75 | `0x0055c8c0` | `0x00632750` | kDX9DrawPrimDual | rtti |

## `ragnarok/item_db.h` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 40 | `0x006a1b20` | `0x0070cf80` | kInfoCtorAddr, kItemSkillInfoCtor | propagation |
| 41 | `0x006a6570` | `0x00711a00` | kInfoSetIdAddr, kItemSkillInfoSetId | opcode |

## `features/gameplay/player_jump.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 17 | `0x007110c0` | `0x00780790` | kTerrainHeight | propagation |
| 18 | `0x00a69eb0` | `0x00a329a0` | kActorListFindByGid, kFindByGID | propagation |

## `features/overlays/chat_balloon.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 34 | `0x008263a0` | `0x008645d0` | kBalloonPaint | rtti |
| 37 | `0x00a1cb30` | `0x009ea790` | kClearSurface | propagation |

## `features/systems/native_login.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 21 | `0x008303f0` | `0x004da1f0` | kSetTextAddr | **SUSPECT** rtti |
| 22 | `0x008848d0` | `0x008c3940` | kOnMsgAddr | texte |

## `features/patches/window_pos_tweaks.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 378 | `0x00880e7f` | `0x008c06af` | kSnapJnz | rtti |
| 399 | `0x00a33005` | `0x00a01875` | kSnapLoopHook | propagation |

## `features/windows/item_desc_window.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 117 | `0x00b314f0` | `0x00b0f8d0` | kFnSearchRoute, kNaviRoute | opcode+multi |
| 78 | `0x009030c0` | `0x00940da0` | kHideNative, kSetVisibleFn | rtti |

## `features/overlays/quest_tracker.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 26 | `0x009137a0` | `0x0094e810` | kDrawContent | rtti |
| 27 | `0x01254d90` | `0x01127368` | kQuestMgrPtr | globale |

## `features/windows/bank_window.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 135 | `0x00a4ad20` | `0x00a18a20` | kChatActionAddr, kChatAddLine | texte |
| 43 | `0x00a948d0` | `0x00a91460` | kFmtComma, kFmtComma32 | **SUSPECT** propagation |

## `features/patches/pick_quad_tweaks.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 20 | `0x00c58d13` | `0x00c27603` | kCallSite | rtti |
| 21 | `0x00a79610` | `0x00a3ff40` | kInsert | propagation |

## `ragnarok/skill_cooldowns.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 24 | `0x00b1fac0` | `0x00afe6c0` | kReplayClock | propagation |
| 28 | `0x015ff7e0` | `0x014bc2e8` | kNativeList | opcode+multi |

## `features/gameplay/afk_screen.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 45 | `0x00c74fd6` | `0x004cf496` | kRenderWindowsCall | rtti |
| 46 | `0x00c74fdb` | `0x004cf49b` | kRenderWindowsAfter | rtti |

## `features/windows/palette_editor.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 40 | `0x00d5b580` | `0x00c6e5f0` | kGetJob | propagation |
| 50 | `0x015fb9a8` | `0x014b8960` | kOwnCharId, kOwnCharIdAddr, kOwnCid | opcode |

## `features/windows/entity_inspector.cc` — 2 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 30 | `0x00d806a0` | `0x00c93880` | kActorFindByGid | opcode+multi |
| 47 | `0x00d5bb40` | `0x00c6eb90` | kJobDisplayName, kJobNameAddr, kJobResNa | opcode+multi |

## `features/systems/discord_relay.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 12 | `0x007289da` | `0x007974fa` | (littéral) | rtti |

## `ragnarok/ragnarok_client.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 238 | `0x0079d5e0` | `0x007e50d0` | kSelCharRenderPatch | rtti |

## `features/fx/screen_fx.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 397 | `0x00874af0` | `0x008b42a0` | (littéral) | rtti |

## `features/moonlight_ui/moonlight_ui.h` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 337 | `0x008c18b0` | `0x00905e20` | kItemDescWndAddr | rtti |

## `features/overlays/item_obtain_toast.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 38 | `0x008c5860` | `0x00909fa0` | kMsgOrig | rtti |

## `features/patches/skill_tree_tweaks.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 31 | `0x00977e80` | `0x009b41c0` | kDrawContent | rtti |

## `ragnarok/msgstring.h` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 25 | `0x00a9ed30` | `0x00a9b470` | kGetAddr | texte |

## `features/overlays/entity_names.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 150 | `0x00c3c3ff` | `0x00c098cf` | (littéral) | rtti |

## `ui/mob_sprite.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 59 | `0x00d71ec0` | `0x00c85740` | kFnIsNpcOrPortalId | propagation |

## `ui/sprite_path.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 35 | `0x00d9cf80` | `0x00c10380` | kJobIsDoram | **SUSPECT** propagation |

## `features/windows/trade_window.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 62 | `0x00d57a30` | `0x00c6ab10` | kInvDecrease | opcode+multi |

## `features/windows/npc_shop_window.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 210 | `0x00d55f80` | `0x00c69060` | kShopCartResetAll | opcode |

## `features/overlays/cast_bar.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 35 | `0x0073a1f0` | `0x007a6f50` | kGetSkillNameLua | opcode+multi |

## `features/windows/craft_atlas.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 43 | `0x0073a1f0` | `0x007a6f50` | kGetSkillNameLua | opcode+multi |

## `features/windows/monster_info_window.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 393 | `0x0073a1f0` | `0x007a6f50` | kGetSkillNameLua | opcode+multi |

## `… +3` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
|  | `0x00d5bb40` | `0x00c6eb90` | kJobDisplayName, kJobNameAddr, kJobResNa | opcode+multi |

## `features/overlays/target_frame.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 35 | `0x00d806a0` | `0x00c93880` | kActorFindByGid | opcode+multi |

## `features/gameplay/keyboard_move.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 20 | `0x0131f764` | `0x011f1b54` | kNoPathFlag | opcode+multi |

## `features/windows/weapon_refine_window.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 70 | `0x015fb9f8` | `0x014b89b0` | kBaseLvl, kJobLevel, kJobLevelAddr, kOwn | opcode+multi |

## `features/hotkey_util.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 24 | `0x015fb9a8` | `0x014b8960` | kOwnCharId, kOwnCharIdAddr, kOwnCid | opcode |

## `features/fx/style_sync.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 28 | `0x015fb9a8` | `0x014b8960` | kOwnCharId, kOwnCharIdAddr, kOwnCid | opcode |

## `… +1` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
|  | `0x015fa3c0` | `0x014b73b0` | kJobNameCtx, kOptionContextAddr, kSessio | opcode+multi |

## `features/fx/hat_effect_depth.cc` — 1 site(s)

| ligne | 2025 | 2026 | symbole | source |
|---|---|---|---|---|
| 35 | `0x00fd5d60` | `0x00ecbec0` | kNativeAbsMaskVa | multi |

