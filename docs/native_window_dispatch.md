# Le dispatcher des fenêtres natives — carte complète

> Relevé du **2026-08-29** sur le client `2025-07-16_Ragexe` (IDA, base 0x400000).
> Toutes les classes de ce document viennent du **RTTI MSVC**, jamais d'une
> supposition : `vtable-4` → CompleteObjectLocator → `+0x0c` TypeDescriptor →
> `+0x08` nom manglé. La méthode est décrite dans
> `reference_rtti_window_class_id` ; ce qui est neuf ici, c'est de l'avoir
> appliquée à **tous** les cas du dispatcher d'un coup, et d'avoir choisi la
> classe la **plus dérivée** (celle dont la hiérarchie RTTI est la plus profonde)
> au lieu de la première vtable écrite — voir « Le piège de la classe de base ».

## 1. Pourquoi ce document

Le projet connaissait ~90 identifiants de fenêtres, listés dans
[uiwnd.h](../src/ragnarok/uiwnd.h). Ce relevé montre que le client en sert
**269**, et — surtout — que **le `default` du grand `switch` n'est pas un
cul-de-sac** : il délègue à une **seconde fabrique**, un framework `CUI` que le
dépôt n'avait jamais nommé. Douze fenêtres y vivent, dont dix qu'aucune ligne de
`src/` ni de `docs/` ne mentionne.

## 2. Les trois étages

`UIWindowMgr_MakeWindow(mgr, windowID)` @ **0x00a39340** (`__thiscall`, l'id en
3ᵉ argument), 0x9544 octets. Trois étages, dans cet ordre :

```
        ┌─ [1] BARRAGE ────────────────────────────────────────────┐
id ───► │ si mgr+0x4F1B0 != 0  →  9 ids rendent NULL               │
        └──────────────────────────────────────────────────────────┘
                     │ (les autres passent)
        ┌─ [2] SWITCH PRINCIPAL ───────────────────────────────────┐
        │ g_UIWindowIdToCaseTable[id] @0x00a42ca8  (363 octets)    │
        │        → jpt_A394CF @0x00a42904  (233 cibles)            │
        │ ids 0..0x16A + le cas isolé 0x2730                       │
        └──────────────────────────────────────────────────────────┘
                     │ (defaut : 107 ids de 0..362, et TOUT id > 0x16A)
        ┌─ [3] SECONDE FABRIQUE — le framework CUI ────────────────┐
        │ CUIRegisterMgr_GetInstance() @0x00a52d30                 │
        │  → CUIRegisterMgr_HasWindow(id)   @0x00a38b00            │
        │  → CUIRegisterMgr_CreateWindow(id)@0x009a8a20            │
        │ std::map<int, std::function<CUIFrameWnd*()>>             │
        │ ids 10001..10033 SEULEMENT                               │
        └──────────────────────────────────────────────────────────┘
```

Avant l'étage [2], et sauf si `UIWindowMgr_IsWindowOutOfOpenList(id)`
(**0x00a384c0**, une liste blanche d'environ 150 ids en dur) répond vrai, l'id
est retiré puis repoussé dans la `std::list` **mgr+0x18** — l'ordre d'ouverture.

## 3. 🔴 L'étage [1] : neuf fenêtres que le client REFUSE d'ouvrir

Le barrage est gardé par un seul test : `mgr+0x4F1B0 != 0`, c'est-à-dire
**`UIMacroBlackListCheckWnd` (id 359 / 0x167) est à l'écran** — le captcha
anti-macro. Deux tables d'octets classent alors l'id : `byte_A4288C` (ids 8..40)
et `byte_A428B8` (ids 203..275) ; le *case* 0 saute en `loc_A3EFDA`, qui est un
**`return NULL`** sec.

| id | fenêtre refusée |
|---|---|
| 8 (0x08) | inventaire |
| 10 (0x0A) | équipement |
| 18 (0x12) | `UIMenuWnd` |
| 40 (0x28) | chariot |
| 155 (0x9B) | menu Échap |
| 203 (0xCB) | navigation |
| 251 (0xFB) | *(id mort par ailleurs)* |
| 263 (0x107) | RODEX |
| 275 (0x113) | banque |

⚠ **Pour nous** : un hook de `MakeWindow` qui suppose « on m'appelle, donc la
fenêtre naît » se trompe pendant le captcha. Le natif rend `NULL` **avant**
d'entrer dans le switch, donc avant tout cas — un remplacement branché sur le cas
ne verra rien passer.

## 4. L'étage [2] : ce que fait un cas, et les deux exceptions

Le cas ordinaire lit le **slot dédié** `mgr+<off>` ; s'il est nul, il fait
`operator_new(taille)` + `<Classe>_ctor`, réécrit le slot, appelle
`UIWindow_SetSize(w,h)` puis pose la position. C'est ce slot que
[reference_native_window_toggle_router](../src/ragnarok/uiwnd.h) décrit comme
« le revers de toujours détruire » : le détruire remet le slot à zéro, et
d'autres endroits du binaire s'en servent comme d'un booléen.

Deux cas ne construisent **rien** — ce sont des **aiguilleurs**, et c'est une
correction importante pour deux fenêtres que le projet croyait autonomes :

| id | ce qu'il fait vraiment |
|---|---|
| **59 (0x3B)** — « fenêtre de guilde » | sous-`switch` sur `mgr+0x844` (l'onglet actif, 0..6) → `MakeWindow(0x3C + onglet)`. Il n'existe **aucune** classe « fenêtre de guilde » : les sept panneaux 60..66 *sont* la fenêtre. |
| **69 (0x45)** — « messenger group » | `MakeWindow(0x22)` puis `OnMsg(0xD7, 6, …)` avec 1 ou 2 selon `mgr+0x740`. La classe est donc `UIMessengerGroupWnd`, **id 34 (0x22)**. |

Trois cas posent une **garde** avant de construire :

- **40 (chariot)** : `UIWindowMgr_IsWindowAllowedInContext(…, 0x28)` ; refus →
  message système 0x94c dans le chat, et rien n'est créé.
- **275 (banque)** : refuse si 0x127 (`UIRefiningWnd`), 0x157
  (`UIGradeEnchantWnd`), 0x2716 (`CUIEnchantUI`) ou 0x155
  (`UIExpandedBartermarketWnd`) est ouverte — chacune avec son propre message.
- **34 (0x22)** : bloquée en rejeu (`CReassemblyPacketMgr`).

## 5. 🔴 La fabrique Lua — et six ids qui déréférencent NULL

Neuf ids (**156, 158, 159, 165, 166, 167, 181, 183, 191**) partagent un seul cas
qui appelle `UIWindowMgr_MakeWindowFromLuaInfo` **0x00a42ea0**. Cette fabrique
générique demande à Lua `GetWindowInfo(id)` (format `"d>dddd"` → largeur,
hauteur, x, y en repère 1024×768, remis à l'échelle), puis choisit la classe dans
son **propre** `switch` — qui ne connaît que **cinq** ids :

| id | classe |
|---|---|
| 0x9C (156) | `UIHotKeyWnd` — `new(0x120)`, slot `mgr+0x404`. 🔴 Réglages de **raccourcis clavier**, pas la recherche de navigation. |
| 0x9D (157) | `UIEntryQueueWnd` (aussi joignable par le switch principal) |
| 0x9F (159) | `new(0xCC)` + `sub_8D7820` |
| 0xB5 (181) | `new(0x108)` + `sub_8D8150`, pose `byte_15FFF84 = 1` |
| 0xD1 (209) | `UIEntryQueueStandByWnd` (aussi dans le switch principal) |

**Les autres — 158, 165, 166, 167, 183, 191 — tombent dans le `default:` de ce
switch-là, qui laisse le pointeur à NULL, et la fonction le déréférence ensuite
sans le tester** (`(*(...)(*v5 + 180))(v5, id)`). Le seul garde-fou en amont est
que `Lua_CallGlobal_va` rende faux : si `GetWindowInfo` ne connaît pas l'id, la
fabrique sort proprement par `return 0`. Autrement dit : **ces six ids sont sûrs
tant que le Lua du client ne répond pas pour eux, et plantent s'il répond.** Ne
pas les appeler à l'aveugle depuis un raccourci ou un test.

## 6. 🔴🔴 L'étage [3] : le framework `CUI`, jamais nommé dans le dépôt

Le `default` du switch principal (`loc_A4262D`) fait :

```c
reg = CUIRegisterMgr_GetInstance();          // 0x00a52d30, singleton @0x0131ef08
if (!CUIRegisterMgr_HasWindow(reg, id)) return NULL;
w = FindWindow(id);
if (!w) { w = CUIRegisterMgr_CreateWindow(reg, id); … }
```

Le registre est une `std::map<int, entrée>` (nœud de 0x40 octets : clé en +16,
`std::function<CUIFrameWnd*()>` en **+0x3C**), remplie à l'initialisation par
douze fonctions `CUIxxx_RegisterFactory` qui appellent
`CUIRegisterMgr_RegisterFactory` **0x00991b00**. `Game_InitAllManagers` lui passe
ensuite `LoadWindowPosSizeFromLua` (`Lua Files\OptionInfo\UIInfo_F`, globals
`GetWindowPos` / `GetWindowSize`).

C'est **un second système d'interface**, avec ses propres contrôles
(`CUIControl`, `CUIButton`, `CUITextButton`, `CUIDropDownBox`, `CUIVScrollBar`,
`CUIEditBox`, `CUITab`…) — 109 classes `CUI*` au total dans le RTTI, dont
**douze fenêtres de premier plan** :

| id | hex | classe | connu du dépôt ? |
|---:|---|---|---|
| 10001 | 0x2711 | `CUIEquipmentPropertiesWnd` | non |
| 10002 | 0x2712 | `CUIAdventureGuide` | non |
| 10005 | 0x2715 | `CUIMacroReport` | non |
| 10006 | 0x2716 | `CUIEnchantUI` | non |
| 10008 | 0x2718 | `CUIRenewQuestUI` | non |
| 10009 | 0x2719 | `CUIOngoingQuestInfo` | non |
| 10010 | 0x271A | `CUIRecommendedQuestInfo` | non |
| 10011 | 0x271B | `CUIExchangeUI` | **oui** (`kCUIExchangeUI`) |
| 10012 | 0x271C | `CUIProbabilityTable` | non |
| 10014 | 0x271E | `CUIGameSettingsUI` | **oui** (`kCUIGameSettingsUI`) |
| 10030 | 0x272E | `CUISkillDelayInfo` | non |
| 10033 | 0x2731 | `CUISelectPackageItemBox` | non |

Deux voisines à ne pas confondre avec le registre :

- **196 (0xC4) `CUICollectionSystemWnd`** — nom en `CUI`, mais construite par le
  **switch principal**, pas par le registre.
- **10032 (0x2730) `UIDebuffRemoveWnd`** — a son propre cas isolé dans le switch
  (`loc_A42575`, `new(0xB8)`, 180×47, positionnée depuis la taille d'écran).
- **10024 (0x2728)** — n'est créée **nulle part** : ni le switch, ni le registre.
  Elle est seulement *cherchée* (elle bloque les raccourcis :
  `if (FindWindow(0x2728)) ignorer`) et *fermée*. Vestige.

## 7. Les 107 identifiants morts

Puisque le registre CUI ne contient que des ids ≥ 10001, **tout id de 0 à 362
tombant au `default` rend `NULL`**. Ce ne sont pas des fenêtres à découvrir :
ce sont des trous.

```
5, 13, 31, 54, 57, 67, 71, 83, 89, 92, 96, 97, 98, 102, 108, 112, 115, 117, 118,
120, 121, 122, 123, 124, 134, 135, 136, 141, 142, 144, 145, 154, 162, 171, 172,
182, 185, 188, 189, 192, 193, 194, 195, 197, 202, 205, 206, 228, 231, 232, 233,
235, 236, 238..249, 251, 252, 266, 272, 279, 280, 292, 293, 296..301, 305,
310..313, 316, 317, 319, 320, 327, 329, 330, 331, 336..339, 347, 349..357, 360
```

⚠ **301 (0x12D) est dans cette liste**, alors que `uiwnd.h` le déclare sous
`kUIMiniPartyWnd`. Le conteneur du HUD de groupe existe bien, mais **il n'est pas
fabriqué par `MakeWindow`** : `MakeWindow(301)` ne rendra jamais rien.

## 8. Ce que Bourgeon n'a jamais touché

Le test appliqué, littéralement : le nom de classe RTTI apparaît-il quelque part
dans `src/` ou `docs/` ? l'identifiant est-il déclaré dans `uiwnd.h` ou dans une
table du dépôt ? (Les fiches de mémoire ne comptent pas — quelques fenêtres ici
« jamais citées » y sont pourtant décrites, elles sont signalées.)

- **269 fenêtres vivantes** au total.
- **121** sont déjà outillées ou décrites.
- **148 ne le sont pas.**

Les **fonctionnalités entières** que le dépôt n'a jamais approchées, par thème :

| thème | ids | classes |
|---|---|---|
| **Framework CUI** | 10001, 10002, 10005, 10006, 10008..10010, 10012, 10030, 10033 | voir §6 |
| **Barter market** | 334, 335, 341, 342 | `UIBartermarketWnd`, `UIBarterItemPurchaseWnd`, `UIExpandedBartermarketWnd`, `UIExpandedBarterItemPurchaseWnd` |
| **Rune system** | 361, 362 | `UIRuneSystemWnd`, `UIRuneSystem_DecomResultWnd` |
| **Lapine (boîtes)** | 290, 302 | `UILapineBoxWnd`, `UILapineUpgradeBoxWnd` |
| **File d'attente d'instance** | 157, 209, 210, 211 | `UIEntryQueueWnd`, `…StandByWnd`, `…RequestWnd`, `…HelpWnd` |
| **Recherche de groupe** | 164, 168, 169, 170, 173, 323, 324, 326, 345 | `UISeekPartyWnd`, `UISeekPartyMBWnd`, `UISeekPartyListWnd`, `UIJobListWnd`, `UIPartyBookingHelpWnd`, `UIRegisterPartyWnd`, `UIAdvenPartyBoardWnd`, `UIRequestJoinPartyWnd`, `UIApplyForPartyWnd` |
| **Anti-macro / captcha** | 284, 286, 287, 288, 359 | `UICaptchaRegisterWnd`, `UIMacroReporterWnd`, `UIMacroUserInfoWnd`, `UICaptchaPreviewWnd`, `UIMacroBlackListCheckWnd` |
| **Replay** | 186, 187, 198 | `UISelectReplayDataWnd`, `UIReplayControlWnd`, `UIReplayRECControlWnd` |
| **Boutique « Para » / cash** | 184, 254, 255 (+ 256) | `UInCashWnd`, `UIParaItemShopWnd`, `UIParaItemPurchaseWnd`, `UIParaResultWnd` |
| **Storage : sous-onglets** | 146..152, 153, 309 | `UIItemStoreSubWnd` (×8), `UIItemStoreFindWnd` |
| **Popups « accepter » (une classe par motif)** | 70, 104, 105, 110, 119, 138, 163 | `UIJoinGuildAcceptWnd`, `UICoupleAcceptWnd`, `UIBabyAcceptWnd`, `UIBabyAcceptWnd2`, `UIStarPlaceAcceptWnd`, `UIYourItemWnd`, `UIMixAcceptWnd` |
| **Divers jamais ouverts** | 137, 227, 237, 253, 259, 267, 268, 289, 291, 308, 315, 340, 348, 358, 10032 | `UIMemorialDunWnd`, `CMergeItemWnd`, `UIClanInfoManageWnd`, `UIGuild_Storage_Log`, `UISecondCostumeWnd`, `UIMileageWnd`, `UIOpenRoulletteWnd`, `UIViewCameraInfoWnd`, `UITransMenuWnd`, `UIShowWarpWnd`, `UITipboxWnd`, `UIAccountLimitedToolWnd`, `UIItemReformWnd`, `UIBalloonOpenBtn`, `UIDebuffRemoveWnd` |

Signalées « jamais citées » par le test mais **décrites en mémoire** :
281 `UIStylingShopWnd`, 287 `UIMacroUserInfoWnd`, 295 `UIRefiningWnd`,
343 `UIGradeEnchantWnd`, 344 `UIChangeMaterialWnd`, 346 `UIReputeWnd`,
328 `UICheckAttendanceWnd`, 204 `UIQuestDisplay`, 333 `UINoteWnd`.

## 9. Corrections au registre existant

Le RTTI tranche plusieurs entrées que le dépôt portait de travers.

| où | ce qui était écrit | ce que dit le RTTI |
|---|---|---|
| `uiwnd.h` `kUIEquipWndVTable` | `0x01022f68` | **`0x0103223c`**. `0x01022f68` est `UIRPData` — la constante était fausse (et `docs/entity_chat_balloon_re.md` le disait déjà pour une autre raison). Corrigé. |
| `uiwnd.h` `kQuestJournalWndId` (321) | « classe INCONNUE » | **`UIRenewQuestWnd`**, vtable `0x01021588` |
| `uiwnd.h` 1, 28, 69, 74, 226, 307 | « classe non relevée » | `UINewChatWnd`, `UIChatRoomWnd`, *(aiguilleur → 34)*, `UIItemCompositionWnd`, `UISayDialogWnd`, `UIMenuIconWnd` |
| `window_pos_tweaks.cc` | « Guild main window (`UIGuildTotalInfoWnd`), id UNRESOLVED » | **id 66 (0x42)**, vtable `0x0103b5d0`. Le conteneur 59 est un aiguilleur, pas une fenêtre. |
| `window_pos_tweaks.cc` `{0x16d, "party"}` | `UIPartyInfoWnd` (365) | **inatteignable** : 365 > 0x16A, donc `MakeWindow` n'a aucun cas ; et `UIPartyInfoWnd` (`0x0101a040`) n'est pas une fenêtre de premier plan, c'est un **contrôle enfant** construit par `UIAdvenPartyBoardWnd_OnCreate` (id 324). L'entrée ne fait rien. |
| mémoire `reference_rtti_window_class_id` | « 0x16d = `UIAdvenPartyBoardWnd` » | l'id est **324 (0x144)** |
| mémoire, id 0x11e | `UIMacroUserInfoWnd` | 0x11E = **`UIMacroReporterWnd`** ; `UIMacroUserInfoWnd` est **0x11F** |
| IDB (noms hérités) | bloc guilde décalé d'un cran | 60 `UIGuildInfoManageWnd`, 64 `UIGuildBanishedMemberWnd`, 65 `UIGuildNoticeWnd`, 66 `UIGuildTotalInfoWnd`, 75 `UIGuildLeaveReasonDescWnd`, 76 `UIGuildBanReasonDescWnd`, 212 `UIGuildHelperWnd`, 214 `UIGuildTipWnd`, 213/215 `UICreateGuildWnd` |

## 10. Le piège de la classe de base

La première vtable écrite par un constructeur est celle de la **classe de base**,
pas de la classe réelle : MSVC écrit la base, puis la dérivée. Pire, plusieurs
cas écrivent la vtable dérivée **dans le corps du cas**, après avoir appelé un
constructeur de base partagé (`UIWindow_composite_ctor`) — l'id 86 en est
l'exemple : le constructeur dit `UIFrameWnd`, le cas écrit ensuite
`UIEmotionWnd`. La règle appliquée ici : **collecter toutes les écritures de
vtable (corps du cas + constructeur) et garder celle dont le
`ClassHierarchyDescriptor` compte le plus de classes de base.** Sans cela, vingt
fenêtres reçoivent un nom de base générique — `UIChooseSellBuyWnd` avalait à lui
seul `UIExchangeAcceptWnd`, `UIJoinPartyAcceptWnd`, `UIJoinGuildAcceptWnd`,
`UICoupleAcceptWnd`, `UIBabyAcceptWnd`, `UIBabyAcceptWnd2`,
`UIJoinFriendAcceptWnd`, `UIStarPlaceAcceptWnd`, `UIYourItemWnd` et
`UIMixAcceptWnd`.

Contrôle de non-régression : les vtables ainsi obtenues recoupent **exactement**
les trois relevées en live et conservées dans le dépôt — `UIStatusWnd`
`0x010329d4`, `UIExchangeAcceptWnd` `0x01033754`, `UINavigationV4Wnd`
`0x00fd95ec` — et la seule qui ne recoupait pas était fausse dans le dépôt
(§9, équipement).

## 11. Rejouer le relevé

Les scripts IDA sont jetables et tiennent en une passe : lire les 363 octets de
`0x00a42ca8`, indexer `0x00a42904`, marcher chaque cas jusqu'au premier
`operator_new`, prendre le `call` suivant comme constructeur, collecter les
écritures de vtable du corps **et** du constructeur, résoudre le RTTI, garder la
plus dérivée. Les cas gardés (40, 59, 137, 275) sortent du parcours linéaire par
un saut conditionnel : ils se traitent à la main.

L'IDB porte désormais **151 constructeurs renommés depuis le RTTI**, les sept
fonctions du registre CUI, les douze fabriques `CUIxxx_Create` (qui portaient des
noms de signature FLIRT parasites, `___std_parallel_algorithms_hw_threads@0_N`),
et un commentaire d'en-tête sur `UIWindowMgr_MakeWindow` qui résume les trois
étages.

## 12. La vtable d'une `UIWindow` — 53 slots

Une fois les 269 vtables identifiées, la vtable elle-même se lit : chaque slot a
la **même** signification dans toutes les classes, et les rôles se déduisent des
appelants, pas du contenu des implémentations.

Longueur : **53 slots (0xD4 octets)** pour 204 des 229 vtables ; les classes CUI
en ajoutent un (0xD4, voir §13).

| slot | rôle | témoin |
|---|---|---|
| +0x00 | `dtor` | 229 implémentations distinctes — une par classe |
| +0x04 | `OnResize(w,h)` | `UIWindow_OnDraw_Base` 0x00a245c0 : recrée la surface offscreen, écrit `this+0x14`/`+0x18`, puis Invalidate. ⚠ le nom historique dit « OnDraw » — elle ne dessine rien |
| +0x10 | `SetPos(x,y)` | `UIWindow_SetPos` |
| +0x38 | `SetVisible(b)` | `UIWindow_SetVisible` 0x009030c0 |
| +0x3C | `OnCreate(w,h)` | **60 handlers déjà nommés `_OnCreate`, tous à +0x3C, aucun ailleurs** ; appelé par `UIWindow_SetSize` |
| +0x40 | `OnLayout()` | dernier appel de `UIWindow_SetSize`, et de `Initialize` côté CUI |
| +0x50 | `OnPaint()` | `UIWindow_Render` ne l'appelle que si `this+0x58 == 1`, puis remet le drapeau à 0 |
| +0x64 | `OnLButtonDown(x,y)` | `UIWindowMgr_DispatchMouseInput` : `g_Mouse_LButtonState == 1`. L'implémentation de base démarre le **glissement** de la fenêtre |
| +0x68 | `OnLButtonDblClk` | … `== 4` |
| +0x6C | `OnMouseMove(x,y)` | … la souris a bougé depuis la frame précédente |
| +0x70 | `OnMouseStay(x,y)` | … elle n'a **pas** bougé |
| +0x78 | `OnMouseUpdate(x,y)` | … inconditionnel ; joué aussi sur la fenêtre qu'on vient de quitter |
| +0x7C | `OnLButtonUp` | … `== 3` |
| +0x80 / +0x84 / +0x88 | `OnRButtonDown` / `Up` / `DblClk` | `g_Mouse_RButtonState` 1 / 3 / 4 |
| +0x8C | `OnMouseWheel(delta)` | si `dword_11E40EC` |
| +0x94 | `OnMsg` | **114 handlers déjà nommés `_OnMsg`, tous à +0x94, aucun ailleurs** |
| +0x98 | `Invalidate()` | `UIWindow_PaintDispatch` : pose `this+0x58 = 1` et propage à l'enfant |
| +0xA4 | `Render` | `UIWindow_Render` |
| +0xB4 | `SetCommandId(id)` | `MakeWindowFromLuaInfo` et `CUIRegisterMgr_CreateWindow` le passent l'id |
| +0xC4 / +0xC8 | `BuildRenderQuad` / `HitTest` | un seul quad par fenêtre, cf. `project_ui_window_manager` en mémoire |

⚠ Les coordonnées passées aux handlers souris sont **locales** : le dispatcher
remonte la chaîne des parents (`this+0x10`) et soustrait leurs `+0x1C`/`+0x20`.

Un dernier fait utile, tombé du gabarit de dialogue : `UIAcceptWnd_OnCreate_Shared`
(0x008a6260, l'`OnCreate` partagé par onze boîtes de confirmation) construit ses
deux boutons depuis les bitmaps `btn_ok*` / `btn_cancel*` et leur pose les
**command id 184 (OK) et 185 (Annuler)**. Ces deux valeurs ne sont donc pas
propres à une fenêtre : elles viennent du gabarit — ce que `uiwnd.h` n'osait pas
affirmer.

## 13. Le framework `CUI`, de l'intérieur

**`CUIFrameWnd` dérive de `UIWindow`** : 52 slots sur 53 pointent la même
implémentation, y compris `Render`, `BuildRenderQuad`, `HitTest`, `OnMsg` et
`Invalidate`. Une fenêtre CUI est donc une fenêtre native ordinaire pour tout ce
qui est rendu, souris et hit-test ; c'est seulement sa **construction** qui
diffère.

Ce qu'elle ajoute :

- **slot +0xD4 = `Initialize()`**, et c'est une **fonction virtuelle pure**
  (`CUIFrameWnd::vftable[+0xD4] == _purecall`) : toute fenêtre CUI doit
  l'implémenter. `CUIRegisterMgr_CreateWindow` l'appelle juste après
  `SetCommandId(id)`. Exemple (`CUIGameSettingsUI`) : pose `this+0xB4 = 1` et
  `this+0xD4 = 1`, `OnResize(400, 350)`, `SetPos(0,0)`, `OnCreate(w,h)`,
  `OnLayout()`.
- **`CUIFrameWnd_OnPaint_Base`** (0x009a4fa0, slot +0x50) : si `this+0xB6`,
  efface la surface à la couleur-clé `0xFFFF00FF`, remplit `(1,1,w-2,h-2)` avec
  l'ARGB de `this+0xB8` et trace un contour arrondi avec celui de `this+0xBC`.
- une bibliothèque de contrôles à elle : `CUIControl` (la base, elle-même dérivée
  de `UIWindow`), `CUIButton`, `CUITextButton`, `CUIImageButton`, `CUICheckBox`,
  `CUIRadioButton`, `CUIDropDownBox`, `CUIEditBox` / `CUIEditBoxEx`,
  `CUIVScrollBar` / `CUIHScrollBar`, `CUITab` / `CUITabButton`, `CUITitleBar`,
  `CUIStaticText`, `CUIStaticImage`, `CUITextViewer`, `CUIResizer`, `CUIPadding`,
  `CUIGameItem`, plus les **templates** `CUIListBox<T>`, `CUIListBoxEx<T,U>` et
  `CUIComboBox<T>`.

**110 vtables `CUI*`** au total, retrouvées par balayage brut des segments de
données (`dword == TypeDescriptor` → COL, `dword == COL` → vtable).

⚠ **Le piège des templates.** Un filtre sur le préfixe `CUI` rate
`CUIListBoxEx<GameSettingsUI::CUIComboItem, RenderAdapterInfo>`, dont le nom
manglé commence par `?$`. Trente constructeurs se sont ainsi retrouvés baptisés
du nom de leur classe de **base**, `CUIControl`. Il faut inclure `.?AV?$…`.

⚠⚠ **Et le piège qui l'accompagne : ctor et dtor écrivent les mêmes vtables, en
ordre INVERSE.** MSVC fait écrire au constructeur la base d'abord et la classe
dérivée en dernier ; le destructeur fait exactement le contraire avant d'appeler
le destructeur de base. La règle « la dernière vtable écrite donne la classe »
transforme donc silencieusement des destructeurs en constructeurs. Le
discriminant est l'ordre : `profondeur(première) > profondeur(dernière)` ⇔
destructeur.

⚠⚠⚠ **Et un troisième, qui invalide les deux règles à la fois : l'héritage
multiple.** `GameSession_ctor` (0x00d504b0) écrit `CCommonObject` **et**
`CInstantVar`, deux bases *sœurs* de profondeur 1, et jamais la vtable de la
classe réelle. `EffectNode_base_ctor` (0x00c3fc80) fait de même avec
`CGameObject`, `CRenderObject` et `CInstantVar`. Dans ces cas la dernière écrite
n'est pas la plus profonde, et **aucun** nom RTTI n'est le bon. Le garde-fou :
ne renommer que si la dernière vtable écrite est aussi, strictement, la plus
profonde.

## 14. Ce que porte l'IDB après cette passe

La discipline appliquée aux renommages en masse, et c'est elle qui fait la
valeur du résultat : **on ne nomme une implémentation d'après une classe que si
la paire (slot, fonction) est EXCLUSIVE à cette classe.** Une implémentation
partagée par deux classes vient d'une base commune ; la baptiser du nom d'une
feuille serait faux. 125 paires ont été écartées à ce titre.

| ce qui porte un nom | nombre |
|---|---|
| `<Classe>_dtor` | 535 |
| `<Classe>_ctor` | 503 |
| `<Classe>_OnMsg` | 279 |
| `<Classe>_OnCreate` | 268 |
| `<Classe>_OnPaint` | 186 |
| handlers souris (`_OnLButtonDown`, `_OnMouseStay`, `_OnMouseWheel`, …) | 254 |
| `<Classe>_OnLayout` | 38 |
| `<Classe>_Initialize` (CUI) | 23 |

Contrôle de non-régression : les **214** fonctions nommées `_OnMsg` présentes
dans une vtable de fenêtre s'y trouvent **toutes** au slot +0x94, et aucune
ailleurs. Les rôles restés inconnus portent `_vfXX` — un nom qui dit ce qu'il
sait (le numéro de slot et la classe) et rien de plus.

## 15. Table complète

Colonne « statut Bourgeon » : `id declare` = l'identifiant figure dans
`uiwnd.h` ou une table du dépôt ; `src/` / `docs/` = le nom de classe apparaît
dans ces répertoires. `**jamais citee**` = ni l'un ni l'autre.

| id | hex | classe (RTTI) | vtable | ctor (adr.) | slot mgr | taille | statut Bourgeon |
|---:|---|---|---|---|---|---|---|
| 0 | 0x000 | UIBasicInfoWnd | 0x103e35c | 0x95d060 | +0x1DC | 0x118 | src/, docs/ |
| 1 | 0x001 | UINewChatWnd | 0x1037f80 | 0x8d7660 | +0x1C8 | 0x140 | id declare, src/, docs/ |
| 2 | 0x002 | UISelectServerWnd | 0x1030090 | 0x86c890 |  | 0xdc | id declare |
| 3 | 0x003 | UILoginWnd | 0x1030168 | 0x86bb30 | +0x1CC | 0xf0 | src/, docs/ |
| 4 | 0x004 | UIMakeCharWnd | 0x10303f0 | 0x86bc10 |  | 0x2dc | id declare, src/, docs/ |
| 6 | 0x006 | UIWaitWnd | 0x1031fb4 | 0x88f4a0 |  | 0xbc | **jamais citee** |
| 7 | 0x007 | UILoadingWnd | 0x103208c | 0x88e1b0 | +0x1BC | 0xbc | **jamais citee** |
| 8 | 0x008 | UIItemWnd | 0x103d460 | 0x934c10 | +0x1D4 | 0x130 | id declare, src/, docs/ |
| 9 | 0x009 | UIToolTipWnd | 0x1032164 | 0x88f440 |  | 0xb8 | **jamais citee** |
| 10 | 0x00A | UIEquipWnd | 0x103223c | 0x88d740 | +0x1E0 | 0x17c | id declare, src/, docs/ |
| 11 | 0x00B | UIStatusWnd | 0x10329d4 | 0x88f0d0 | +0x1C4 | 0x100 | id declare, src/, docs/ |
| 12 | 0x00C | UIItemCollectionWnd | 0x1032aac | 0x88db90 | +0x218 | 0x250 | id declare |
| 14 | 0x00E | UIMinimapZoomWnd | 0x103475c | 0x88e340 | +0x1C0 | 0xc8 | id declare, src/, docs/ |
| 15 | 0x00F | UIItemDropCntWnd | 0x103d6e8 | 0x9345d0 | +0x1EC | 0xbc | **jamais citee** |
| 16 | 0x010 | UISayDialogWnd | 0x1033094 | 0x88eb50 | +0x1F0 | 0xd8 | id declare, docs/ |
| 17 | 0x011 | UIChoose3Wnd | 0x104ade0 | 0x88d3a0 | +0x1F8 | 0xe0 | id declare, docs/ |
| 18 | 0x012 | UIMenuWnd | 0x1034abc | 0x88e270 | +0x210 | 0xe0 | src/, docs/ |
| 19 | 0x013 | UIRestartWnd | 0x103331c | 0x88eaf0 | +0x238 | 0xb8 | **jamais citee** |
| 20 | 0x014 | UINoticeConfirmWnd | 0x10334cc | 0x88e530 |  | 0xbc | **jamais citee** |
| 21 | 0x015 | UINotifyLevelUpWnd | 0x1034834 | 0x88e650 | +0x240 | 0xb4 | **jamais citee** |
| 22 | 0x016 | UIItemShopWnd | 0x103cbf0 | 0x934850 | +0x250 | 0x110 | id declare, src/, docs/ |
| 23 | 0x017 | UIItemPurchaseWnd | 0x103cda0 | 0x934630 | +0x258 | 0x104 | id declare, src/ |
| 24 | 0x018 | UIItemSellWnd | 0x103ce78 | 0x934730 | +0x25C | 0xfc | id declare, src/ |
| 25 | 0x019 | UIChooseSellBuyWnd | 0x10335a4 | 0x88d2a0 | +0x264 | 0xbc | id declare, src/, docs/ |
| 26 | 0x01A | UIComboBoxWnd | 0x1034b94 | 0x88d580 | +0x214 | 0xbc | docs/ |
| 27 | 0x01B | UIChatRoomMakeWnd | 0x1033fc4 | 0x88d160 | +0x274 | 0x128 | id declare, src/, docs/ |
| 28 | 0x01C | UIChatRoomWnd | 0x1030678 | 0x86b7f0 | +0x27C | 0x124 | id declare, src/, docs/ |
| 29 | 0x01D | UIPasswordWnd | 0x10324c4 | 0x88e8d0 | +0x280 | 0xc0 | id declare, src/ |
| 30 | 0x01E | UIChatRoomChangeWnd | 0x103409c | 0x88d000 | +0x278 | 0x128 | id declare, src/, docs/ |
| 32 | 0x020 | UIExchangeAcceptWnd | 0x1033754 | 0x88d980 | +0x284 | 0xc0 | id declare, src/, docs/ |
| 33 | 0x021 | UIItemStoreWnd | 0x103ca40 | 0x934ae0 | +0x288 | 0x194 | id declare, src/, docs/ |
| 34 | 0x022 | UIMessengerGroupWnd | 0x1010e2c | 0x701fc0 | +0x2C8 | 0x290 | src/, docs/ |
| 35 | 0x023 | UIJoinPartyAcceptWnd | 0x103382c | 0x88e120 | +0x2CC | 0xc0 | src/ |
| 36 | 0x024 | UIShortCutWnd | 0x1037484 | 0x8d8210 | +0x1E8 | 0x18c | src/, docs/ |
| 37 | 0x025 | UINewSkillListWnd | 0x103f660 | 0x974060 | +0x2C4 | 0x2b0 | id declare, src/, docs/ |
| 38 | 0x026 | UITipOfTheDayWnd | 0x1033eec | 0x88f2b0 | +0x2F0 | 0xc0 | **jamais citee** |
| 39 | 0x027 | UIChooseWarpWnd | 0x1033244 | 0x88d300 | +0x20C | 0xc8 | **jamais citee** |
| 40 | 0x028 | UIMerchantItemWnd | 0x103d538 | 0x934f90 | +0x2F4 | 0xfc | id declare, src/, docs/ |
| 41 | 0x029 | UIMerchantShopMakeWnd | 0x103d7c0 | 0x935080 | +0x2FC | 0x190 | id declare, src/, docs/ |
| 42 | 0x02A | UIMerchantMirrorItemWnd | 0x103d610 | 0x935000 | +0x2F8 | 0xf8 | id declare, src/, docs/ |
| 43 | 0x02B | UIMerchantItemShopWnd | 0x103d028 | 0x934ee0 | +0x300 | 0x11c | id declare, src/, docs/ |
| 44 | 0x02C | UIMerchantItemPurchaseWnd | 0x103d2b0 | 0x934e40 | +0x308 | 0x10c | id declare, src/, docs/ |
| 45 | 0x02D | UIMerchantItemMyShopWnd | 0x103d100 | 0x934ce0 | +0x304 | 0x120 | id declare, src/, docs/ |
| 46 | 0x02E | UISkillDescribeWnd | 0x1032e0c | 0x88ee60 | +0x230 | 0x110 | id declare |
| 47 | 0x02F | UICardItemIllustWnd | 0x1032ee4 | 0x88cf50 | +0x23C | 0xe8 | **jamais citee** |
| 48 | 0x030 | UIQuitWnd | 0x10333f4 | 0x88ea90 | +0x234 | 0xb4 | **jamais citee** |
| 49 | 0x031 | UINotifyJobLevelUpWnd | 0x104aeb8 | 0x88e650 | +0x244 | 0xb4 | **jamais citee** |
| 50 | 0x032 | UIItemParamChangeDisplayWnd | 0x10323ec | 0x88dea0 | +0x22C | 0xf4 | id declare, src/, docs/ |
| 51 | 0x033 | UICandidateWnd | 0x103c5f4 | 0x933180 | +0x3A4 | 0x19c | docs/ |
| 52 | 0x034 | UICompositionWnd | 0x103c51c | 0x9331f0 | +0x3A8 | 0xcc | **jamais citee** |
| 53 | 0x035 | UIPartySettingWnd | 0x1034174 | 0x88e860 | +0x30C | 0xe4 | src/ |
| 55 | 0x037 | UISkillNameChangeWnd | 0x1032674 | 0x88ef40 | +0x310 | 0xd4 | **jamais citee** |
| 56 | 0x038 | UINpcEditDialogWnd | 0x1032824 | 0x88e710 | +0x314 | 0xbc | id declare, docs/ |
| 58 | 0x03A | UINotifyItemObtainWnd | 0x103274c | 0x88e590 | +0x31C | 0x1f0 | src/, docs/ |
| 59 | 0x03B | _(aiguilleur)_ aiguilleur : MakeWindow(0x3C + mgr[0x844]), onglet 0..6 |  |  |  |  | id declare |
| 60 | 0x03C | UIGuildInfoManageWnd | 0x103b0a8 | 0x919850 | +0x320 | 0x108 | id declare |
| 61 | 0x03D | UIGuildMemberManageWnd | 0x103b184 | 0x919990 | +0x324 | 0x19c | src/, docs/ |
| 62 | 0x03E | UIGuildPositionManageWnd | 0x103b260 | 0x919c00 | +0x328 | 0x234 | src/ |
| 63 | 0x03F | UIGuildSkillWnd | 0x103b33c | 0x919d10 | +0x32C | 0x15c | docs/ |
| 64 | 0x040 | UIGuildBanishedMemberWnd | 0x103b418 | 0x919790 | +0x330 | 0x140 | id declare |
| 65 | 0x041 | UIGuildNoticeWnd | 0x103b4f4 | 0x919ba0 | +0x334 | 0xdc | **jamais citee** |
| 66 | 0x042 | UIGuildTotalInfoWnd | 0x103b5d0 | 0x919e50 | +0x338 | 0x118 | id declare, src/, docs/ |
| 68 | 0x044 | UIFriendOptionWnd | 0x1010c70 | 0x701150 | +0x33C | 0xc8 | src/, docs/ |
| 69 | 0x045 | _(aiguilleur)_ aiguilleur : MakeWindow(0x22) puis OnMsg(0xD7) |  |  |  |  | id declare |
| 70 | 0x046 | UIJoinGuildAcceptWnd | 0x1033e14 | 0x88e090 | +0x2E8 | 0xc0 | **jamais citee** |
| 72 | 0x048 | UIAllyGuildAcceptWnd | 0x10305a0 | 0x86b700 | +0x2EC | 0xc0 | **jamais citee** |
| 73 | 0x049 | UIItemIdentifyWnd | 0x10343fc | 0x88dde0 | +0x1FC | 0xf0 | docs/ |
| 74 | 0x04A | UIItemCompositionWnd | 0x1034684 | 0x88dd80 | +0x208 | 0xf8 | id declare, src/, docs/ |
| 75 | 0x04B | UIGuildLeaveReasonDescWnd | 0x103b6ac | 0x919920 | +0x340 | 0xbc | **jamais citee** |
| 76 | 0x04C | UIGuildBanReasonDescWnd | 0x103b784 | 0x919710 | +0x344 | 0xc8 | **jamais citee** |
| 77 | 0x04D | UIMonsterInfoWnd | 0x1030750 | 0x86c3c0 |  | 0xf0 | src/, docs/ |
| 78 | 0x04E | UIIllustWnd | 0x103c264 | 0x932750 | +0x348 | 0xd4 | **jamais citee** |
| 79 | 0x04F | UIMakeTargetListWnd | 0x103ec50 | 0x965660 | +0x374 | 0xc4 | id declare, src/, docs/ |
| 80 | 0x050 | UIMakeTargetProcessWnd | 0x103eed8 | 0x9656e0 | +0x378 | 0x3a0 | id declare, src/, docs/ |
| 81 | 0x051 | UIMakeTargetResultWnd | 0x103efb0 | 0x965760 | +0x37C | 0xbc | **jamais citee** |
| 82 | 0x052 | UICombinedCardItemCollectionWnd | 0x1032b84 | 0x88d450 | +0x228 | 0x258 | **jamais citee** |
| 84 | 0x054 | UITalkboxTrapInputWnd | 0x1030828 | 0x86c930 |  | 0xc8 | **jamais citee** |
| 85 | 0x055 | UIKeyStrokeWnd | 0x103c444 | 0x933270 | +0x3AC | 0xb4 | **jamais citee** |
| 86 | 0x056 | UIEmotionWnd | 0x104b070 | 0x86b950 | +0x380 | 0x10c | id declare, src/, docs/ |
| 87 | 0x057 | UICashEmotionListWnd | 0x101b634 | 0x78ce10 | +0x384 | 0xf0 | src/, docs/ |
| 88 | 0x058 | UIPetInfoWnd | 0x1030900 | 0x86c550 | +0x38C | 0xec | id declare, src/, docs/ |
| 90 | 0x05A | UIPetEggListWnd | 0x10344d4 | 0x88e930 | +0x390 | 0xf0 | id declare, src/, docs/ |
| 91 | 0x05B | UIPetTamingDeceiveWnd | 0x1034d44 | 0x88e990 | +0x394 | 0xd4 | docs/ |
| 93 | 0x05D | UINoticeWnd | 0x1030318 | 0x86c470 |  | 0x174 | **jamais citee** |
| 94 | 0x05E | UIMakingArrowListWnd | 0x10345ac | 0x88e210 | +0x398 | 0xf4 | id declare, src/, docs/ |
| 95 | 0x05F | UISelectCartWnd | 0x1034e1c | 0x88ede0 | +0x39C | 0xc4 | **jamais citee** |
| 99 | 0x063 | UISpellListWnd | 0x1034ef4 | 0x88efc0 | +0x3A0 | 0x100 | **jamais citee** |
| 100 | 0x064 | UINpcTextEditDialogWnd | 0x10328fc | 0x88e770 | +0x318 | 0xc0 | id declare, docs/ |
| 101 | 0x065 | UIGraffiStrboxWnd | 0x1034fcc | 0x88db30 |  | 0xc8 | **jamais citee** |
| 103 | 0x067 | UIProhibitListWnd | 0x10350a4 | 0x88ea30 | +0x388 | 0xbc | **jamais citee** |
| 104 | 0x068 | UICoupleAcceptWnd | 0x1033904 | 0x88d5f0 | +0x2D0 | 0xc0 | **jamais citee** |
| 105 | 0x069 | UIBabyAcceptWnd | 0x1033c64 | 0x88cdf0 | +0x2E0 | 0xc0 | **jamais citee** |
| 106 | 0x06A | UIBookWnd | 0x103517c | 0x88ce80 | +0x3B0 | 0x160 | id declare, src/, docs/ |
| 107 | 0x06B | UIItemRepairWnd | 0x103ed28 | 0x965590 | +0x200 | 0xd8 | **jamais citee** |
| 109 | 0x06D | UIJoinFriendAcceptWnd | 0x1035254 | 0x88e000 | +0x3B4 | 0xc8 | src/ |
| 110 | 0x06E | UIBabyAcceptWnd2 | 0x1033d3c | 0x88cd60 | +0x2E4 | 0xc0 | **jamais citee** |
| 111 | 0x06F | UIWeaponRefineWnd | 0x103ee00 | 0x9657d0 | +0x204 | 0xd0 | id declare, src/, docs/ |
| 113 | 0x071 | UIHomunInfoWnd | 0x10309d8 | 0x86ba40 | +0x3C0 | 0xd8 | id declare, src/, docs/ |
| 114 | 0x072 | UISkillListWnd | 0x103cb18 | 0x935290 | +0x3C4 | 0x148 | id declare, src/, docs/ |
| 116 | 0x074 | UIAutoMessageWnd | 0x10354dc | 0x88cd00 | +0x3D0 | 0xb8 | **jamais citee** |
| 119 | 0x077 | UIStarPlaceAcceptWnd | 0x10339dc | 0x88f040 | +0x2D4 | 0xc4 | **jamais citee** |
| 125 | 0x07D | UIMerInfoWnd | 0x1030ab0 | 0x86c1b0 | +0x3C8 | 0xc8 | **jamais citee** |
| 126 | 0x07E | UISkillListWnd | 0x103cb18 | 0x935290 | +0x3CC | 0x148 | src/, docs/ |
| 127 | 0x07F | UISelCharForUServerWnd | 0x1037634 | 0x8d8040 | +0x3D8 | 0x1a00 | **jamais citee** |
| 128 | 0x080 | UIChangeNameWnd | 0x1037b44 | 0x8d70f0 | +0x3DC | 0x174 | id declare |
| 129 | 0x081 | UIRebirthWnd | 0x1037c1c | 0x8d78b0 | +0x3E0 | 0xb4 | **jamais citee** |
| 130 | 0x082 | UISubChatHisWnd | 0x1037ea8 | 0x8d82a0 | +0x3E4 | 0x130 | src/ |
| 131 | 0x083 | UISubChatHisWnd | 0x1037ea8 | 0x8d82a0 | +0x3E8 | 0x130 | src/ |
| 132 | 0x084 | UIBattleMsgOptionWnd | 0x1037dcc | 0x8d6ba0 | +0x3EC | 0x118 | id declare, src/, docs/ |
| 133 | 0x085 | UISubChatMiniWnd | 0x1037cf4 | 0x8d83c0 | +0x3F0 | 0xc0 | **jamais citee** |
| 137 | 0x089 | UIMemorialDunWnd | 0x1038058 | 0x8d73f0 | +0x3F8 | 0xbc | **jamais citee** |
| 138 | 0x08A | UIYourItemWnd | 0x1033b8c | 0x88f6d0 | +0x2DC | 0xc8 | docs/ |
| 139 | 0x08B | UIEquipWnd | 0x103223c | 0x88d740 | +0x3FC | 0x17c | src/, docs/ |
| 140 | 0x08C | UIRoMapWnd | 0x1038140 | 0x8d7910 | +0x400 | 0x228 | id declare, src/, docs/ |
| 143 | 0x08F | UINotifyQuestWnd | 0x103490c | 0x88e6b0 | +0x248 | 0xb4 | **jamais citee** |
| 146 | 0x092 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0xesi*4+28C | 0x160 | **jamais citee** |
| 147 | 0x093 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0xesi*4+28C | 0x160 | **jamais citee** |
| 148 | 0x094 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0xesi*4+28C | 0x160 | **jamais citee** |
| 149 | 0x095 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0xesi*4+28C | 0x160 | **jamais citee** |
| 150 | 0x096 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0xesi*4+28C | 0x160 | **jamais citee** |
| 151 | 0x097 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0xesi*4+28C | 0x160 | **jamais citee** |
| 152 | 0x098 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0xesi*4+28C | 0x160 | **jamais citee** |
| 153 | 0x099 | UIItemStoreFindWnd | 0x103c968 | 0x934900 | +0x2AC | 0x164 | **jamais citee** |
| 155 | 0x09B | UIEscOptionWnd | 0x10384a0 | 0x8d71b0 | +0x408 | 0xd8 | id declare, src/, docs/ |
| 156 | 0x09C | UIHotKeyWnd (new 0x120, mgr+0x404) |  |  |  |  | id declare |
| 157 | 0x09D | UIEntryQueueWnd | 0x1088814 | 0xb3e2b0 |  | 0x104 | **jamais citee** |
| 158 | 0x09E | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 159 | 0x09F | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 160 | 0x0A0 | UIItemShopWnd2 | 0x103ccc8 | 0x9347b0 | +0x254 | 0x10c | **jamais citee** |
| 161 | 0x0A1 | UIItemSellWnd2 | 0x103cf50 | 0x9346b0 | +0x260 | 0x100 | **jamais citee** |
| 163 | 0x0A3 | UIMixAcceptWnd | 0x1033ab4 | 0x88e3f0 | +0x2D8 | 0xc4 | **jamais citee** |
| 164 | 0x0A4 | UISeekPartyWnd | 0x10377e4 | 0x8d7f70 | +0x2B0 | 0x1ec | id declare |
| 165 | 0x0A5 | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 166 | 0x0A6 | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 167 | 0x0A7 | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 168 | 0x0A8 | UISeekPartyMBWnd | 0x1037994 | 0x8d7e40 | +0x2B4 | 0x180 | **jamais citee** |
| 169 | 0x0A9 | UISeekPartyListWnd | 0x1037a6c | 0x8d7c30 | +0x2BC | 0x1d4 | **jamais citee** |
| 170 | 0x0AA | UIJobListWnd | 0x10378bc | 0x8d7370 | +0x2B8 | 0xf0 | **jamais citee** |
| 173 | 0x0AD | UIPartyBookingHelpWnd | 0x103770c | 0x8d77c0 | +0x2C0 | 0xb8 | **jamais citee** |
| 174 | 0x0AE | UIMerchantShopMakeWnd | 0x103d7c0 | 0x935080 | +0x40C | 0x190 | id declare, src/, docs/ |
| 175 | 0x0AF | UIMerchantMirrorItemWnd | 0x103d610 | 0x935000 | +0x410 | 0xf8 | id declare, src/, docs/ |
| 176 | 0x0B0 | UIMerchantItemMyShopWnd | 0x103d100 | 0x934ce0 | +0x414 | 0x120 | id declare, src/, docs/ |
| 177 | 0x0B1 | UIMerchantItemShopWnd | 0x103d028 | 0x934ee0 | +0x418 | 0x11c | id declare, src/, docs/ |
| 178 | 0x0B2 | UIMerchantItemPurchaseWnd | 0x103d2b0 | 0x934e40 | +0x41C | 0x10c | id declare, src/, docs/ |
| 179 | 0x0B3 | UIMerchantMirrorItemWnd | 0x103d610 | 0x935000 | +0x420 | 0xf8 | id declare, src/, docs/ |
| 180 | 0x0B4 | UILoginOTPWnd | 0x1030240 | 0x86bab0 | +0x1D0 | 0xc4 | **jamais citee** |
| 181 | 0x0B5 | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 183 | 0x0B7 | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 184 | 0x0B8 | UInCashWnd | 0x1032314 | 0x88f760 | +0x1E4 | 0x188 | **jamais citee** |
| 186 | 0x0BA | UISelectReplayDataWnd | 0x1030e24 | 0x86c7f0 |  | 0xd0 | **jamais citee** |
| 187 | 0x0BB | UIReplayControlWnd | 0x1030c64 | 0x86c690 | +0x4F088 | 0xe4 | **jamais citee** |
| 190 | 0x0BE | UInCash_CallWnd | 0x10349e4 | 0x88f870 | +0x24C | 0xb4 | id declare, src/, docs/ |
| 191 | 0x0BF | **aucune classe** (fabrique Lua) |  |  |  |  | **jamais citee** |
| 196 | 0x0C4 | CUICollectionSystemWnd | 0x10388d8 | 0x8d67f0 | +0x4F0AC | 0xd8 | **jamais citee** |
| 198 | 0x0C6 | UIReplayRECControlWnd | 0x1030efc | 0x86c750 | +0x4F084 | 0x13c | **jamais citee** |
| 199 | 0x0C7 | UISecondPasswordInputWnd | 0x1035404 | 0x88ec20 | +0x3B8 | 0x168 | **jamais citee** |
| 200 | 0x0C8 | UISecondPasswordEditWnd | 0x104af90 | 0x88ec20 | +0x3BC | 0x168 | id declare |
| 201 | 0x0C9 | UIMonsterDisplayWnd | 0x1032fbc | 0x88e480 | +0x424 | 0xe8 | id declare |
| 203 | 0x0CB | UINavigationV4Wnd | 0xfd95ec | 0x5a4010 | +0x4F094 | 0x124 | id declare, src/, docs/ |
| 204 | 0x0CC | UIQuestDisplay | 0x1039bfc | 0x90cd30 | +0x428 | 0x94 | **jamais citee** |
| 207 | 0x0CF | UIPartySettingWnd | 0x1034174 | 0x88e860 | +0x30C | 0xe4 | src/ |
| 208 | 0x0D0 | UIPartyInvitationToWnd | 0x103424c | 0x88e7e0 | +0x42C | 0xe8 | docs/ |
| 209 | 0x0D1 | UIEntryQueueStandByWnd | 0x1088704 | 0xb3d810 |  | 0x3ec | **jamais citee** |
| 210 | 0x0D2 | UIEntryQueueRequestWnd | 0x108862c | 0xb3ca50 |  | 0xd4 | **jamais citee** |
| 211 | 0x0D3 | UIEntryQueueHelpWnd | 0x1088464 | 0xb3bbd0 |  | 0xbc | **jamais citee** |
| 212 | 0x0D4 | UIGuildHelperWnd | 0x103b85c | 0x9197f0 | +0x430 | 0xbc | id declare |
| 213 | 0x0D5 | UICreateGuildWnd | 0x103ba0c | 0x9196a0 | +0x438 | 0xc4 | **jamais citee** |
| 214 | 0x0D6 | UIGuildTipWnd | 0x103b934 | 0x919df0 | +0x434 | 0xbc | **jamais citee** |
| 215 | 0x0D7 | UICreateGuildWnd | 0x103ba0c | 0x9196a0 | +0x43C | 0xc4 | **jamais citee** |
| 216 | 0x0D8 | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 217 | 0x0D9 | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 218 | 0x0DA | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 219 | 0x0DB | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 220 | 0x0DC | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 221 | 0x0DD | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 222 | 0x0DE | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 223 | 0x0DF | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 224 | 0x0E0 | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 225 | 0x0E1 | UIMultiIllustWnd | 0x103c33c | 0x9327f0 | +0xeax*4-14 | 0xd8 | **jamais citee** |
| 226 | 0x0E2 | UISayDialogWnd | 0x1033094 | 0x88eb50 | +0x1F4 | 0xd8 | id declare, docs/ |
| 227 | 0x0E3 | CMergeItemWnd | 0x101ea1c | 0x7a4a60 |  | 0x118 | **jamais citee** |
| 229 | 0x0E5 | UINavigationHelpWnd | 0x1038800 | 0x8d7510 |  | 0xbc | id declare, src/, docs/ |
| 230 | 0x0E6 | UIMerchantItemPurchaseMSGWnd | 0x103d1d8 | 0x934d90 |  | 0xd4 | **jamais citee** |
| 234 | 0x0EA | UIItemCollection_ComparisonWnd | 0x1032c5c | 0x88dd20 | +0x220 | 0x250 | id declare |
| 237 | 0x0ED | UIClanInfoManageWnd | 0x1028750 | 0x8153d0 | +0x444 | 0xc8 | **jamais citee** |
| 250 | 0x0FA | UICombinedCardItemCollection_ComparisonWnd | 0x1032d34 | 0x88d4b0 | +0x224 | 0x250 | **jamais citee** |
| 253 | 0x0FD | UIGuild_Storage_Log | 0x1039cd4 | 0x90c860 | +0x44C | 0x190 | **jamais citee** |
| 254 | 0x0FE | UIParaItemShopWnd | 0x103f114 | 0x96dba0 | +0x268 | 0x110 | **jamais citee** |
| 255 | 0x0FF | UIParaItemPurchaseWnd | 0x103f1ec | 0x96db40 | +0x26C | 0x104 | **jamais citee** |
| 256 | 0x100 | UIParaResultWnd | 0x103f2c4 | 0x96dc00 | +0x270 | 0x104 | id declare |
| 257 | 0x101 | UIMerchantItemLogWnd | 0x103eb50 | 0x963ef0 |  | 0x100 | id declare, src/, docs/ |
| 258 | 0x102 | UIMerchantItemLogWnd | 0x103eb50 | 0x963ef0 |  | 0x100 | id declare, src/, docs/ |
| 259 | 0x103 | UISecondCostumeWnd | 0x1039e88 | 0x90ce40 | +0x4F090 | 0x258 | **jamais citee** |
| 260 | 0x104 | UIComboBoxIDWnd | 0x1034c6c | 0x88d510 | +0x21C | 0xbc | id declare |
| 261 | 0x105 | UIPetEvolutionWnd | 0x103f3ec | 0x970d30 |  | 0xd4 | id declare, src/, docs/ |
| 262 | 0x106 | UIOpenMailBoxWnd | 0x1039dac | 0x90cc70 | +0x450 | 0xb4 | id declare, src/, docs/ |
| 263 | 0x107 | UIRodexInboxWnd | 0x1022170 | 0x7cd7d0 | +0x454 | 0x208 | id declare, src/ |
| 264 | 0x108 | UIRodexComposeWnd | 0x1021b30 | 0x7c7860 | +0x458 | 0x108 | id declare |
| 265 | 0x109 | UIRodexContentsWnd | 0x1021fbc | 0x7ca760 | +0x45C | 0x110 | id declare |
| 267 | 0x10B | UIMileageWnd | 0x1039f60 | 0x90c9e0 |  | 0xdc | **jamais citee** |
| 268 | 0x10C | UIOpenRoulletteWnd | 0x103a038 | 0x90ccd0 | +0x460 | 0xb4 | **jamais citee** |
| 269 | 0x10D | UIRoulletteWnd | 0x1022bb4 | 0x7d43b0 | +0x464 | 0x1e8 | id declare |
| 270 | 0x10E | UIAchievementWnd | 0x1019584 | 0x778250 |  | 0xd8 | id declare, src/, docs/ |
| 271 | 0x10F | UIAchTracingWnd | 0x1019da4 | 0x77ffa0 |  | 0xd0 | docs/ |
| 273 | 0x111 | UIMiniMapWnd | 0x103e880 | 0x960f30 | +0x4F0EC | 0x16c | src/, docs/ |
| 274 | 0x112 | UIEditctrlWnd | 0x103e958 | 0x960ed0 | +0x4F0F0 | 0xb8 | **jamais citee** |
| 275 | 0x113 | UIBank_NewWnd | 0x1030fd4 |  |  | 0xd0 | id declare, src/, docs/ |
| 276 | 0x114 | UIRoundBoxTooltipWnd | 0x103e784 | 0x95fe70 | +0x4F104 | 0xb8 | **jamais citee** |
| 277 | 0x115 | UINewSelectCharWnd | 0x101d424 | 0x4e5330 | +0x3D4 | 0x98 | id declare, src/, docs/ |
| 278 | 0x116 | UINewMakeCharWnd | 0x101dcc4 | 0x4e5330 |  | 0x98 | id declare, src/, docs/ |
| 281 | 0x119 | UIStylingShopWnd | 0x1026220 | 0x7ebdf0 |  | 0xc4 | **jamais citee** |
| 282 | 0x11A | UIConnectTwitterWnd | 0x101efe0 | 0x7a9c30 |  | 0x108 | **jamais citee** |
| 283 | 0x11B | UIControlRectWnd | 0x101ec54 | 0x7a95f0 |  | 0xd0 | **jamais citee** |
| 284 | 0x11C | UICaptchaRegisterWnd | 0x101fba8 | 0x7b2c00 | +0x4F108 | 0xe8 | **jamais citee** |
| 285 | 0x11D | UIMacroDetectorWnd | 0x102005c | 0x7b59e0 | +0x4F10C | 0x118 | src/, docs/ |
| 286 | 0x11E | UIMacroReporterWnd | 0x1020278 | 0x7b7490 | +0x4F110 | 0x254 | **jamais citee** |
| 287 | 0x11F | UIMacroUserInfoWnd | 0x10201a0 | 0x7b7650 | +0x4F114 | 0xe4 | **jamais citee** |
| 288 | 0x120 | UICaptchaPreviewWnd | 0x101ff84 | 0x7b5950 | +0x4F118 | 0xc8 | **jamais citee** |
| 289 | 0x121 | UIViewCameraInfoWnd | 0x104ab34 | 0xa1a200 |  | 0xb4 | **jamais citee** |
| 290 | 0x122 | UILapineBoxWnd | 0xfe0aac | 0x5dbba0 |  | 0x12c | **jamais citee** |
| 291 | 0x123 | UITransMenuWnd | 0x103a110 | 0x90d070 |  | 0xe8 | **jamais citee** |
| 294 | 0x126 | UIPreviewEquipWnd | 0x1020424 | 0x7bba20 |  | 0xc8 | **jamais citee** |
| 295 | 0x127 | UIRefiningWnd | 0x10209f8 | 0x7bd3b0 |  | 0x300 | **jamais citee** |
| 302 | 0x12E | UILapineUpgradeBoxWnd | 0xfe12c8 | 0x5df8e0 |  | 0x218 | **jamais citee** |
| 303 | 0x12F | UIHotkeyGuideWnd | 0x103be44 | 0x92c250 |  | 0xe4 | docs/ |
| 304 | 0x130 | UIVideoWnd | 0x104d19c | 0xa56410 |  | 0x88 | docs/ |
| 306 | 0x132 | UINavigationroadiconWnd | 0xfd9364 | 0x5a40b0 |  | 0xb8 | id declare, src/, docs/ |
| 307 | 0x133 | UIMenuIconWnd | 0x10281b0 | 0x812930 |  | 0xd8 | id declare, src/, docs/ |
| 308 | 0x134 | UIShowWarpWnd | 0x103a1e8 | 0x90cf50 |  | 0xb8 | **jamais citee** |
| 309 | 0x135 | UIItemStoreSubWnd | 0x103c890 | 0x9349f0 | +0x2A8 | 0x160 | **jamais citee** |
| 314 | 0x13A | UINavigationRuideWnd | 0xfd943c | 0x5a3fb0 |  | 0xb8 | id declare, src/, docs/ |
| 315 | 0x13B | UITipboxWnd | 0x10355b4 | 0x88f310 | +0x468 | 0x104 | **jamais citee** |
| 318 | 0x13E | UICashShopWnd | 0x101ca18 | 0x793750 | +0x4F17C | 0x108 | id declare, src/ |
| 321 | 0x141 | UIRenewQuestWnd | 0x1021588 | 0x7c4a60 |  | 0x174 | id declare |
| 322 | 0x142 | UIRenewDetailQuestInfoWnd | 0x10212c0 | 0x7c3f60 |  | 0xf8 | **jamais citee** |
| 323 | 0x143 | UIRegisterPartyWnd | 0x1019e90 | 0x780c70 | +0x4F198 | 0xe8 | **jamais citee** |
| 324 | 0x144 | UIAdvenPartyBoardWnd | 0x101a1f8 | 0x780980 | +0x4F19C | 0x154 | **jamais citee** |
| 325 | 0x145 | UIRPImageWnd2 | 0x103a54c | 0x90cdb0 | +0x4F184 | 0xd4 | **jamais citee** |
| 326 | 0x146 | UIRequestJoinPartyWnd | 0x101a2d0 | 0x780d70 | +0x4F1A0 | 0xec | **jamais citee** |
| 328 | 0x148 | UICheckAttendanceWnd | 0x101e6cc | 0x7a24f0 |  | 0xd0 | **jamais citee** |
| 332 | 0x14C | UIDisconnectedServerMsgWnd | 0x103367c | 0x88d680 |  | 0xb8 | **jamais citee** |
| 333 | 0x14D | UINoteWnd | 0x103a474 | 0x90cb90 |  | 0xb8 | **jamais citee** |
| 334 | 0x14E | UIBartermarketWnd | 0x1027a9c | 0x8088a0 | +0x4F188 | 0x110 | **jamais citee** |
| 335 | 0x14F | UIBarterItemPurchaseWnd | 0x1027c50 | 0x808840 | +0x4F18C | 0x104 | **jamais citee** |
| 340 | 0x154 | UIAccountLimitedToolWnd | 0x103a624 | 0x90c690 |  | 0x11c | **jamais citee** |
| 341 | 0x155 | UIExpandedBartermarketWnd | 0x1027dd8 | 0x80d410 | +0x4F190 | 0xd4 | **jamais citee** |
| 342 | 0x156 | UIExpandedBarterItemPurchaseWnd | 0x1027f8c | 0x80d2f0 | +0x4F194 | 0xdc | **jamais citee** |
| 343 | 0x157 | UIGradeEnchantWnd | 0x101f63c | 0x7af350 |  | 0x228 | **jamais citee** |
| 344 | 0x158 | UIChangeMaterialWnd | 0xfdab90 | 0x5be6f0 |  | 0x220 | **jamais citee** |
| 345 | 0x159 | UIApplyForPartyWnd | 0x1019f68 | 0x780a40 | +0x4F1A4 | 0x104 | **jamais citee** |
| 346 | 0x15A | UIReputeWnd | 0xfe16bc | 0x5eadc0 |  | 0xf0 | **jamais citee** |
| 348 | 0x15C | UIItemReformWnd | 0xfe0668 | 0x5d8bc0 |  | 0x310 | **jamais citee** |
| 358 | 0x166 | UIBalloonOpenBtn | 0x103a6fc | 0x90c760 |  | 0xd8 | **jamais citee** |
| 359 | 0x167 | UIMacroBlackListCheckWnd | 0x101fce8 | 0x7b3b30 | +0x4F1B0 | 0x15c | docs/ |
| 361 | 0x169 | UIRuneSystemWnd | 0x1023de4 | 0x7dd7c0 |  | 0xe8 | **jamais citee** |
| 362 | 0x16A | UIRuneSystem_DecomResultWnd | 0x1023d0c | 0x7dd960 |  | 0xf8 | **jamais citee** |
| 10001 | 0x2711 | CUIEquipmentPropertiesWnd | 0x1045280 | CUI…_Create | registre CUI |  | **jamais citee** |
| 10002 | 0x2712 | CUIAdventureGuide | 0x1041e24 | CUI…_Create | registre CUI |  | **jamais citee** |
| 10005 | 0x2715 | CUIMacroReport | 0x1048210 | CUI…_Create | registre CUI |  | docs/ |
| 10006 | 0x2716 | CUIEnchantUI | 0x1042ab4 | CUI…_Create | registre CUI |  | **jamais citee** |
| 10008 | 0x2718 | CUIRenewQuestUI | 0x1049db0 | CUI…_Create | registre CUI |  | **jamais citee** |
| 10009 | 0x2719 | CUIOngoingQuestInfo | 0x1048dcc | CUI…_Create | registre CUI |  | **jamais citee** |
| 10010 | 0x271A | CUIRecommendedQuestInfo | 0x1049128 | CUI…_Create | registre CUI |  | **jamais citee** |
| 10011 | 0x271B | CUIExchangeUI | 0x10457d8 | CUI…_Create | registre CUI |  | id declare, src/, docs/ |
| 10012 | 0x271C | CUIProbabilityTable | 0x10485cc | CUI…_Create | registre CUI |  | **jamais citee** |
| 10014 | 0x271E | CUIGameSettingsUI | 0x1047d7c | CUI…_Create | registre CUI |  | id declare, src/, docs/ |
| 10030 | 0x272E | CUISkillDelayInfo | 0x104aa58 | CUI…_Create | registre CUI |  | **jamais citee** |
| 10032 | 0x2730 | UIDebuffRemoveWnd | 0x103a7d4 | 0x90c800 |  | 0xb8 | **jamais citee** |
| 10033 | 0x2731 | CUISelectPackageItemBox | 0x104a53c | CUI…_Create | registre CUI |  | **jamais citee** |
