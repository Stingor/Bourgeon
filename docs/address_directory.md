# Annuaire des constantes natives — foyers et verdicts

Complément CURATÉ de `docs/address_manifest.md` (qui, lui, est GÉNÉRÉ par
`tools/gen_address_manifest.py` : 784 adresses × sites, à ne pas éditer).
Ce fichier porte ce que le manifeste ne peut pas savoir : où va chaque famille
de constantes (les FOYERS), ce que sont VRAIMENT les adresses qui ont porté
plusieurs noms (les VERDICTS, tranchés au désassembleur), et les doublons
laissés exprès. Source : fiche de mémoire `project_address_directory`
(chantier 2026-07-28 → 2026-08-26, journal dans `docs/journal/`).

## 1. Les foyers (en-têtes du catalogue)

Règles communes : noms de fichiers MINUSCULES, `<cstdint>` seul (modèle
`uiwnd.h`), `<excpt.h>` et jamais `<Windows.h>`. Pas d'`addresses.h`
fourre-tout : un en-tête inclus partout dont chaque retouche recompile tout
troque une dette contre une autre.

| En-tête | Namespace | Ce qu'il porte |
|---|---|---|
| `ragnarok/uiwnd.h` | `uiwnd::` | MakeWindow/CloseWindow/OnMsg/SetPos, `Vf<>`, kOffVisible/PosX/PosY ; `kInventoryWndSlot`/`kStorageWndSlot` + vtables ; section « L'ANNUAIRE » = **83 fenêtres natives** (id + vtable + classe), **triée par VALEUR** |
| `ragnarok/globals.h` | `rag::` | session, zeny, gestionnaire de modes, mode actif ; « Allocateur CRT du client » (`kGameOperatorNewAddr`, `kGameOperatorDeleteAddr`, `kStdStringDtorAddr`) ; `ActiveModeIfReady`, `ActiveModeSafe`, `ModeSendMsg`(+`Ptr`), `ActiveModeSendMsgSafe`/`RawModeSendMsgSafe`, `OwnAccountId`(`Safe`), `ReadInt`, `Read<T>`, `AspdFromAmotion`, `OwnActor`, `kStatRaiseCostAddr` (constante, pas d'accesseur : le lecteur porte le `__try`) |
| `ragnarok/item_db.h` | `itemdb::` | DB de descriptions, `ItemSkillInfo`, noms, ids de fenêtres de description, `kFillInfoByIdAddr` |
| `ragnarok/render.h` | `render::` | contexte de rendu, atlas de sprites, `Act_GetFrame` |
| `ragnarok/lua.h` | `lua::` | API C Lua + enveloppes typées ; `Manager()` ≠ `State()` (cf. verdicts) |
| `ui/game_texture.h` | `ro::texmgr::`, `ro::uipath::` | `LoadResource`, `Mgr`, `kBuildItemIconPath`, `kResolveSkinPath`, `kAddRefAddr` ; `kUiRoot` (littéral CP949 `유저인터페이스`), `WithFileName` |
| `ragnarok/game_scene.h` | `gamescene::` (PAS `scene` : masqué par des locales) | CGameMode→actorMgr (`0xcc`), →dict de noms (`0x160`), champs `CNameInfo`, les DEUX `FindByGid`, `PostActorClickAction`, `FindActorByGid`, `ActorGuildId` |
| `ragnarok/audio.h` | `audio::` | `g_SoundMgr`, `Sound_Play3D`, `Play3D` |
| `ragnarok/file_mgr.h` | `filemgr::` | FileMgr, `LoadToMemory`, `FreeBuffer` |
| `ragnarok/navigation.h` | `navi::` | `g_Navigation`, `SearchRoute` |
| `ragnarok/client_string.h` | `rag::clientstr::` | `Data`/`Size`/`Copy`/`CopyTruncating` : décodage de la `std::string` MSVC (était écrit 34 fois, sous trois tests `cap > 15` / `>= 16` / `> 0xf`) ; rend `kInfoIdCap = 0x40` inutile |
| `ragnarok/stl_node.h` | `rag::treenode` / `rag::listnode` | marche des nœuds `std::map` / `std::list` — DEUX espaces de noms, jamais un préfixe commun : `kValue` vaut 0x10 dans un arbre et 0x08 dans une liste |
| `ragnarok/skill_info.h` | `rag::skillinfo` | disposition de `CSkillInfo` |
| (dans `item_db`) | `rag::itemlist` | disposition d'`ItemSkillInfo` |
| `ragnarok/job_ids.h` | | ids de classes |
| `utils/memory_patch.h` | `mem::` | `WriteCode`, `PatchValue` (10 copies, 4 noms fusionnées ; l'une avait perdu son `FlushInstructionCache`) |
| `utils/text.h` | | helpers ASCII ; proscrit `std::tolower`/`std::isxdigit` (locale) |
| `utils/game_focus.*` | | focus du jeu |
| `features/windows/viewer_probes.h` | `viewers::` | 7 sondes des viewers |
| `features/windows/item_viewer_base.h` | `ItemViewerBase` | 21 des 22 membres communs aux 3 viewers ; `static_assert` sur `PendAction` dans chaque dérivée |
| `ui/item_grid_chrome.h` | `ro::grid::` | chrome des grilles d'objets |
| `ui/viewer_rect.h`, `ui/desc_pending_lock.*` | | rect d'un viewer, verrou de description |
| `ui/ui_palette.h` | `ro::pal` | 8 `ImVec4` nommées par RÔLE + relevé daté des `IM_COL32` (60 nuances de fond, 45 de trait, 35 de texte : NON fusionnées, c'est de l'apparence) ; les `ImU32` ne valent PAS leurs homonymes `ImVec4` |
| `ui/ro_imgui` | `ro::` | `AddTextHalo` (cerne 8 voisins), `AddTextRelief` (passe décalée + texte ; décalage nul = pas de relief ; couvre le faux gras) |

Mesure au 2026-08-24 : **166 adresses au catalogue** (`globals` 77, `uiwnd` 18,
`item_db` 14, `lua` 13, `game_texture` 10, `game_scene` 9, `render` 5…) ;
569 adresses encore en dur (597 occurrences) mais **zéro partagée entre deux
fichiers, zéro doublant le catalogue** — reliquat à un seul lecteur, à NE PAS
mutualiser (les gros porteurs pilotent une fenêtre native entière :
`status_tweaks` 45, `game_settings.cc` 45, `character_sheet` 43,
`inventory_tweaks` 31).

## 2. Verdicts sur les adresses qui ont porté plusieurs noms

| Adresse | Ce que c'est (tranché au désassembleur) | Nom Bourgeon / règle |
|---|---|---|
| `0x00a2e770` | `UIWindowMgr_SaveRectAndCloseWindow` : sauve le rect PUIS `QueueDestroyWindow` `0x00a447d0` — la fenêtre est DÉTRUITE (six fichiers disaient `Close`, deux `SaveWindowRect`) | `uiwnd::kCloseWindowAddr` |
| `0x01213338` | le gestionnaire de modes (portait `kModeMgr`, `kModeArg`, `kDragMgr` — il n'existe AUCUN gestionnaire de drag) | `rag::kModeMgrAddr` |
| `0x0121333c` vs `0x00a75340` | `0x0121333c` = pointeur BRUT du mode courant ; `GameMode_GetActive` `0x00a75340` = `*(mgr+0x58)==1 ? *(mgr+4) : 0` — la garde `+0x58` masque le mode pendant les transitions. NE PAS substituer l'une à l'autre | `ActiveModeRaw()` / `rag::ActiveModeIfReady` : deux fonctions dont le NOM porte la distinction |
| `0x012515f8` | un seul objet, quatre facettes (`kSceneCtxPtr`, `kViewportPtr`, `kRendererObjPtr`, `kRenderContextPtrVa`) ; c'est aussi `g_SceneRenderQueue` | `render::kContextPtr` |
| `0x015ffd78` | `lua::Manager()` = `*0x015ffd78` ; `lua::State()` = `*Manager()`. PAS redondants : `kExecFileAddr` est `__thiscall` sur le MANAGER, l'API C veut le STATE ; les confondre crashe | `lua::` |
| `0x00dbbc4f` / `0x00dbbc7f` | **operator new / operator delete** du CRT statique (pas malloc/free) ; new ne rend JAMAIS nullptr (boucle `_callnewh`) | `rag::kGameOperatorNewAddr` / `kGameOperatorDeleteAddr` |
| `0x004f08f0` | `_Tidy` de `std::string` (libère hors-SSO, remet size=0/cap=15) ; `0x004e78c0` est un simple THUNK vers elle | `rag::kStdStringDtorAddr` |
| `0x005aad80` / `0x009030c0` | les deux `SetVisible` écrivent le MÊME champ `+0x28` en DWORD entier ; la native `0x009030c0` y ajoute la NOTIFICATION (`OnMsg` 23 via vtable+0x94, puis +0x98) — c'est elle que la barre de raccourcis intercepte | `uiwnd::SetVisible` (attention : homonyme préexistant, C2084 payé) |
| `0x00d5a720` | `BuildItemIconGrfPath` = `void __stdcall(const char* id_str, char* out, int identified)`, `retn 0Ch`, `arg_8` LU (`[eax+8]` si non nul, `[eax+0x1C]` sinon). Une copie à DEUX arguments lisait son drapeau dans la pile non initialisée | `ro::texmgr::kBuildItemIconPath` ; **identification inconnue ⇒ passer 1** (sur Moonlight tout objet est identifié sauf `item2`/`item3`/`@item2`) |
| `0x00d7fa90` | NI `kGetInvItemAddr` NI `kSkillEntryFill` : aiguilleur par PLAGE d'id — 8000-8060, 8200-8241 ∪ 8400-8457, 10000-10019, sinon la liste des compétences APPRISES (d'où l'icône perdue d'une compétence d'une autre classe) | `itemdb::kFillInfoByIdAddr` |
| `0x015fa3c0` | la session — un seul objet sous cinq métiers apparents (`kUIWindowContextKey`, `kOptionContextAddr`, `kUiCtx`, `kJobNameCtx`, `kSessionAddr`) | `rag::` session |
| `0x015fb9a4` | l'AID du joueur (six orthographes) | `rag::OwnAccountId()` (`uint32_t`) |
| `0x00d5bb40` | un seul getter qui rend TANTÔT un libellé de classe TANTÔT un nom de FICHIER — d'où `kJobDisplayName`/`kJobNameAddr`/`kJobResName` | |
| `0x01600553` | `kFavFlag` et `kDealLockGlobal` sont le MÊME octet : le mode Favoris ajoute un onglet ET exclut les favoris de la liste vendable | |
| `0x015fba54` | `kAspdRaw` MENT : c'est l'**amotion** (ms) ; ASPD = `(2000 − x) / 10` | `rag::AspdFromAmotion` |
| `0x00a9f030` | PAS un « MakeKey » : rend un `const char*` et fait la **RÉSOLUTION DE SKIN** (racine d'interface → dossier du skin actif, repli `UI\`, réécriture seulement si la ressource EXISTE, sinon pointeur d'entrée tel quel). **NON RÉENTRANTE** : écrit dans l'un de deux `std::string` globaux `0x0159d654` / `0x0159d670` | `ro::texmgr::kResolveSkinPath` |
| `0x00a8d4a0` | « UITextureMgr_Load » est un gestionnaire de **RESSOURCES** aiguillant **PAR EXTENSION** (d'où `.wav` et `.str` acceptés : usage prévu). Chemin tronqué à 260, `tolower` ASCII (`0x012154c8`, `/` NON converti en `\`), résultat mémorisé SANS AddRef | `ro::texmgr::LoadResource` + `kAddRefAddr` pour garder une ressource |
| `0x012135f0` | le quadtree de **PICKING** (pas « nameplate » : les étiquettes n'en sont qu'un consommateur). Catégorie 1 d'un pick = OBJET AU SOL (`CItem`, `+0x1c` = `0x7D03`, `+0x20` = 1) | |
| `0x010253b4` | littéral CP949 `유저인터페이스` (était recopié dans 11 fichiers sous 6 noms ; un octet faux = image manquante silencieuse) | `ro::uipath::kUiRoot` |
| `0x015FF664` | table des noms de **BOUCLIER**, pas de cape : toute la famille IDB `Robe_*` composait des gabarits `방패\…` et construisait le slot 7 | renommée `Shield_*` ; `Job_GetGarmentPalettePath` → `Job_GetGarmentActPath` (rend un `.act`, la cape n'a pas de palette) |
| `0x0103D2B0` | `UIMerchantItemPurchaseWnd` sert **44 ET 178** ; `UIMerchantMirrorItemWnd` sert 42 et 179 ⇒ une classe ne vaut pas un identifiant | nom de RÔLE, classe en commentaire |
| `10011` = `0x271b` | `kFeedBlockerWndId` (pet) et `kExchangeId` (trade) = la même fenêtre, **`CUIExchangeUI`** | |
| `0x0cc` / `0x160` | offsets CGameMode→actorMgr (redéclaré dans ONZE fichiers sous CINQ noms, dont deux en `0x0cc`) et →dict de noms | `gamescene::` |
| fenêtres de description | OBJET = fenêtre `0x0c` + msg `0x18` + `&ItemSkillInfo` ; SKILL = `0x2e` + `0x3d` + **id BRUT** (id affiché à `+0x104`) — `skill_bar` les avait INTERVERTIS | `itemdb::` |

## 3. Deux structures que l'IDB nomme pareil

La confusion la plus chère de ce coin du client : aucun compilateur ne la voit.

| | `ItemSkillInfo` | `CSkillInfo` |
|---|---|---|
| ctor | `0x006a1b20` | `0x00739700` |
| dtor | `0x005a4300` | `0x00739cd0` |
| taille | `0xF8` mesuré (allocation arrondie à `0x100` : les deux constantes sont VRAIES) | |
| +0x00 | | vtable |
| +0x04 | index d'inventaire | « fiche utilisable » |
| +0x10 | quantité | niveau appris |
| +0x20 | | `const char*` « Unknown-Skill » |
| +0x2c / +0x44 | deux `std::string` | |

## 4. Doublons et contradictions laissés EXPRÈS (ne pas « corriger »)

- `kSlotCount` (macros ≠ équipement), `kSlots` (`thread_local` ≠ `static`),
  `kOffResPath` (1 membre sur 9 d'une famille non homogène), `kMsiLevel`
  (2 entrées ne valent pas un foyer) — chacun dit sa raison dans le code.
- `kItemSkillInfoSize` `0xf8` vs `0x100` : vraie taille vs allocation arrondie
  (dit dans `item_cell.cc`).
- `kTypeMob`/`kTypeNpc` inversés entre `target_frame` et `navigation_window` :
  DEUX énumérations différentes (ZC `0x0F2A` d'un côté, types de résultat de
  navigation de l'autre).
- Les cernes à QUATRE passes (feuille de personnage ×2, `entity_names`) ne se
  ramènent pas au helper à huit : volontairement plus légers.
- Une adresse citée dans un message de diagnostic ou un bloc de commentaire de
  RE est une TRACE, pas un doublon : le relevé ignore ce qui suit `//`.
- Noms d'élément / taille / race d'un monstre : fiche = traduits par msgstring,
  navigation = littéraux anglais — fusionner déciderait à la place du joueur.
