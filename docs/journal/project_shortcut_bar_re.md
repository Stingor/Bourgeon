# Barre d'action (UIShortCutWnd 0x24) et plugin SkillBar

> Journal du chantier. La fiche de mémoire `project_shortcut_bar_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Barre d'action / raccourcis de skills du client 20250716 (= "action bar" à relayouter façon
WoW Bartender/Dominos/ElvUI). Objectif [[reference_ui_window_manager]]. Tout est documenté dans
Ghidra (fonctions renommées `UIShortCutWnd_*`, commentaires de synthèse aux entrées).

## Classe & identité
- Classe `UIShortCutWnd`, RTTI `.?AVUIShortCutWnd@@` (type desc 0x0123fd94).
- **vftable = 0x01037484**. **Window id = 0x24 (36)**. **Taille objet = 0x18C** (operator new dans MakeWindow).
- Une SEULE fenêtre/classe. Pas de 2e barre : `SkillBar_1Tab`/`SkillBar_2Tab` = 2 **onglets/pages**
  de la même barre (data tab0/tab1, switch via option skill #10). Persistance UIInfo.lua par onglet
  (FUN_0059eef0 / FUN_0059e2c0 lisent les 2 clés).
- Instance live : `UIWindowMgr_FindWindow(0x24)` ou cache manager `+0x1e8`. g_UIWindowMgr=0x0131f4e8.
- Ctor `UIShortCutWnd_Ctor` 0x008d8210 (defaults: rows=1, col/row sel=-1, btns=0).
  Dtor 0x008da800 ; scalar-deleting dtor 0x008dbcf0. MakeWindow case à 0xa3a1ca (dword table
  0xa42904 idx 0x21, byte-index table 0xa42ca8[id]).

## 🔴 PERSISTANCE DE LA POSITION de la barre native (RE 2026-08-16, IDA renommé+commenté)
La position/taille de la barre NE passe NI par UIInfo.lua NI par le registre UIRectInfo (contrairement
aux fenêtres de [[reference_window_position_persistence]]). Elle vit dans une struct **SlotInfo de 5 dwords
à `g_UIWindowMgr+0x748` = `0x0131FC30`** : `{+0 SlotX, +4 SlotY, +8 SlotWidth(=290 toujours),
+0xC SlotHeight, +0x10 SlotShow}` (nb de lignes = h/33 ; h ∈ [43,175]).
- **Application à la création** : `UIWindowMgr_MakeWindow` case 36 (@**0x00A3A270**) dimensionne 0x122×0x22,
  centre, puis envoie **OnMsg 0x22 avec `&mgr[0x748]`**. `UIShortCutWnd_OnMsg` case 34 (@0x0090188C) garde le
  pointeur en **wnd+0xB8**, valide via `ShortCutBar_SlotInfo_IsValid` **0x0096BE10** (w==290, (h-10)%33==0,
  h=175 seulement si le panneau objets est ouvert, x/y à l'écran) ; invalide ⇒ `ShortCutBar_SlotInfo_SetDefault`
  **0x0096CA30** (**x=230, y=0, w=290, h=76, show=1**) ; puis SetPos(SlotX,SlotY).
- **Stockage = user-data JSON DU PERSONNAGE** (`CCharacterLinkedUserDataMgr`, Json::Value/StyledWriter), pas un
  fichier local : écriture `CharUserData_SaveShortCutBarState` **0x005C5580** (clés `SlotX/SlotY/SlotWidth/
  SlotHeight/SlotShow` + `NormalItemSlot` = 9×{ItemID,SlotNum} depuis 0x015FAA4C + `ItemSlotActive`), appelée
  par `CharUserData_SerializeAndUpload` **0x005C4DA0** (qui sérialise aussi UseSkillInfo/HomunSkillInfo/
  ItemComparison/SortPrivateTab sous le nœud `data` puis envoie via sub_56B290).
  Déclencheurs du save : `UIEscOptionWnd_OnMsg` 0x008FAE60, `CMode_SendMsg_Base` 0x00A763C0,
  `Game_MainWndProc` 0x00DB8100 ⇒ **sortie du jeu / retour char-select / fermeture**, jamais en continu.
  Lecture = `CharUserData_LoadShortCutBarState` **0x005C45C0** ← `CharUserData_ApplyJson` **0x005C4070**
  ← `CharUserData_OnLoadResponse` **0x005C33B0**.
- 🔴 **DESTINATION = le SERVICE WEB, pas un fichier** : `CharacterLinkedUserData_BuildWebUrls` **0x005C5260**
  = `ExternalSettings_GetAssistAddr` (0x00D59060) + **`/charconfig/save`** (mgr+0x30) et **`/charconfig/load`**
  (mgr+0x48) ; instance `CCharacterLinkedUserDataMgr` = **`[0x012517A8]`**. Envoi = AsyncWork du pool
  (`CharUserData_EnqueueSaveAsyncWork` 0x0056B290 → `CCharacterLinkedUserDataSaveAsyncWork_SetParam` 0x005C6220),
  cf. [[reference_web_api_asyncwork_re]]. Adresse assist vide ⇒ pas d'URL ⇒ ni save ni load.
  **MESURE LIVE 2026-08-16** : `mgr+0x30` = **`192.168.1.13:8888/charconfig/save`** (le serveur Moonlight,
  [[reference_moonlight_server_ssh]]). Jumeaux PAR COMPTE = `/userconfig/save` `/userconfig/load`
  (`CAccountLinkedUserData*`), cf. [[reference_external_settings_re]].
- ⚠️ **Trou vérifié** : les SEULS écrivains de la struct globale sont le chargement JSON et le SetDefault
  (xrefs exhaustifs sur les 5 dwords ; aucun accès `+748h` ailleurs que MakeWindow ; aucun accès `wnd+0xB8`
  hors le case 0x22). **Rien ne recopie la position LIVE (wnd+0x1c/+0x20) dans SlotX/SlotY** ⇒ déplacer la barre
  ne semble pas pouvoir être persisté par le client seul — à confirmer en jeu (déplacer, quitter, relancer).
  Le SetPos générique (vtable+0x10 = **0x008E4960**) n'écrit que wnd+0x1c/+0x20 (+ snap `sub_A32EB0`).
- ⇒ Pour persister nous-mêmes une position : écrire directement les dwords 0x0131FC30/34 (la barre les relit à
  chaque MakeWindow via msg 0x22), ou passer par notre yaml comme les autres plugins.
- ✅ **MESURE LIVE (x32dbg, 2026-08-16)** : `0x0131FC30` = **{294, 0, 290, 109}** et la fenêtre live
  (`[0x0131F4E8+0x1E8]` → +0x1c/+0x20/+0x14/+0x18) porte EXACTEMENT les mêmes valeurs. Les défauts étant
  {230,0,290,76}, ces valeurs ont donc bien été **restaurées d'une session précédente** ⇒ le canal user-data
  JSON est VIVANT sur Moonlight (cohérent avec `/userconfig/*` de [[reference_external_settings_re]]).
  ⚠ nuance : sur le client mesuré la barre NATIVE est cachée par notre plugin et n'a pas été manipulée — ces
  valeurs viennent donc d'une session ANCIENNE (avant le plugin), pas d'un déplacement récent ; elles prouvent
  la restauration, pas la capture à chaud.
  Donc le trou ci-dessus n'est PAS « ça ne se sauve pas » mais « l'écrivain n'est pas dans l'exe vanilla » :
  candidat = un **patch WARP** (`FixSkillbarReset` est appliqué, docs/warp_patches.md § Interface) — piège
  classique [[reference_ida_is_vanilla_warp_patches]]. Pour trancher : watchpoint écriture sur 0x0131FC34
  avec la barre native VISIBLE, puis la déplacer.
- Le registre n'est PAS le canal ici : `UIConfig_LoadWindowRectsFromRegistry` **0x00A34090** (défauts via
  `UIWindowMgr_InitDefaultWindowLayouts` **0x0096CCA0**, qui pose 230/0/290/76 à mgr+0x748) et
  `UIConfig_SaveWindowRectsToRegistry` **0x00A4D760** (HKLM, valeurs `M_CHATWND*`, `SAYDIALOGWNDINFO.*`,
  `m_minimapZoomWnd`…) **ne touchent jamais mgr+0x748**.

## Méthodes virtuelles (vtable 0x01037484)
- +0x00 ScalarDeletingDtor 0x008dbcf0
- +0x04 SetSize/Height ; +0x10 Move/SetPos ; +0x98 Invalidate/Redraw (appelées indirectement)
- +0x28 `UIShortCutWnd_OnDrop_AssignSlot` 0x008dd590 — assigne skill/item au slot (fin de drag)
- +0x50 `UIShortCutWnd_OnDraw` 0x008f5800 — rendu de la barre (TOUTE la géométrie)
- +0x64 `UIShortCutWnd_OnLButtonDown_BeginDrag` 0x008f6fc0 — début de glisser d'un slot
- +0x68 OnLButtonUp 0x008f6800 — relâche/use (envoie OnMsg 0x29)
- +0x70 `UIShortCutWnd_OnMouseMove` 0x008f7f50 — SURVOL → tooltip curseur via FUN_00a753d0 (texte construit)
- +0x80 `UIShortCutWnd_OnRButtonDown` 0x008f91a0 — CLIC-DROIT → description objet(fenêtre 0x2e+OnMsg 0x3d)
  / skill(fenêtre 0xc+OnMsg 0x18). (⚠️ EX-mislabel "OnMouseMove_Tooltip" : c'est le clic-droit, pas le survol.)
- +0x94 `UIShortCutWnd_OnMsg` 0x00901310 — hub commandes/messages (voir plus bas)

## Géométrie des slots (à refléter pour tout relayout)
- **9 colonnes/ligne**, **max 4 lignes** → 36 slots (0x24). nb lignes = `this+0x15c` (défaut 1).
- Cellule : **stride colonne 0x1d (29 px)**, **stride ligne 0x21 (33 px)**.
- Icône origine x=0x10 (16), y=4. Surlignage 24×24 (0x18) à x=col*0x1d+0x10, y=g_ShortCutHotRow*0x21+5.
- Label touche (F1..) à x=6, y=0xd+ligne*0x21, couleur 0x2f4a4a.
- **Hauteur fenêtre = rows*0x21 + (itemslot ouvert ? 0x2b : 10)** (calc dans OnMsg case 0xe).
- Hit-test `UIShortCutWnd_HitTestSlot` 0x008e2470 (MIROIR d'OnDraw — à réimplémenter de concert) :
  Y valide 4≤y<rows*0x21(+0x21 si itemslot) ; ligne=(y-4)/0x21 ; morte si row*0x21+0x1e<y.
  X valide 0xf≤x<0x114 ; col=(x-0xf)/0x1d ; morte si col*0x1d+0x29<x.
  **Index slot linéaire = col + g_ShortCutHotRow*9** → indexe `this+0xc4`.

## Struct UIShortCutWnd (this+)
- +0x14 width, +0x18 height ; +0x1c/+0x20 = x/y écran (draw cooldown)
- +0x28 flag réduit/visible
- +0xb4 bouton toggle affichage cooldown ; +0xb8 ptr info-fenêtre sauvée (SetPos case 0x22)
- +0xbc / +0xc0 = boutons ligne "−" / "+" (cachés si rows==1 / rows==4)
- **+0xc4 = tableau 36 ptrs de slot** (idx = row*9+col). Chaque → record **7 octets** :
  +0 type, +1 id (dword), +5 level/req (short).
- 🔒🔒🔒 **CONVENTION DE TYPE — DÉFINITIVE, VÉRIFIÉE PAR CAPTURE LIVE DE 2 ÉCRITURES NATIVES (2026-06-30,
  notre plugin DÉSACTIVÉ donc 0 contamination)** : **`rec[0] == 0` ⇒ OBJET, `rec[0] == 1` ⇒ SKILL.**
  PREUVE (l'utilisateur a glissé sur la barre NATIVE, lecture x32 du store tab0 0x015fa850) :
  `slot Angelus(skill) = 01 21000000 0a00` → rec0=**1** ; `slot Old Purple Box(objet 617) = 00 69020000` →
  rec0=**0**. ⚠️⚠️ LEÇON CAPITALE (cause des ~7 flip-flops) : **les NOMS de fonctions Ghidra sont INVERSÉS** —
  `FUN_00d5a980` (nommé "SkillMgr_GetSkillInfo") est en réalité le **getter OBJET** (branche rec[0]==0 de
  OnMsg 0x29 / OnRButtonDown), et `FUN_00d7fa90` (nommé "ItemMgr_GetInvItemById") est le **getter SKILL**
  (branche rec[0]!=0). Donc lire les décompiles OnDrop/0x29/tooltip en se fiant aux NOMS donne la convention
  INVERSÉE. NE PAS se fier aux noms → se fier à une capture native live (barre native, plugin off).
  (Confirme aussi : `mgr+0x16f0` que FUN_00d5a980 marche contient des OBJETS, ex. nœud "12622" = id objet.)
  Code plugin aligné (OBJET=0/SKILL=1) : color `(type==0)?cItem(bleu):cSkill(vert)`, tooltip+desc
  `is_item=(rec[0]==0)`, WriteSlotRecord `rec[0]=is_item?0:1`, GetIconTex `item_first=(type==0)`
  (OBJET type0→ItemPath 0x00d5a720 ; SKILL type1→SkillPath 0x00d7fa90), SendHotkeyChange isSkill=`is_item?0:1`.
  OpenSlotDescription : `if(rec[0]!=0)` → wnd 0x2e+OnMsg 0x3d (SKILL) ; else → wnd 0xc+OnMsg 0x18 (OBJET)
  — structure = réplique exacte du natif 0x008f91a0 (les n° de fenêtre 0x2e/0xc sont donc aussi "inversés"
  vs leurs noms). Cast/desc passent par les handlers NATIFS (OnMsg 0x29 / MakeWindow) → branchent corrects
  dès que les records ont la convention native. ✅ RÉSOLU 2026-06-30 : cast skill + desc + objets fonctionnent.
  Ghidra corrigé : FUN_00d5a980 renommé `ItemSkillMgr_GetInfoByResId_UNIFIED` (+ commentaire convention) ;
  FUN_00d7fa90 commenté (getter SKILL de la branche rec0==1). ⚠️ MIGRATION : les records persistés AVANT le
  fix final (écrits par notre convention inversée) s'affichent/agissent inversés -> **les re-glisser une fois**
  (le bouton "Reparer types"/MigrateFixTypes a été RETIRÉ : détection par id non fiable à cause des collisions
  objet/skill, ex. 617 ; re-glisser est garanti). DIAGNOSTIC : LogDiag sur DROP/TOOLTIP/DESC/L-CLICK + bannière
  convention au démarrage CONSERVÉS (utiles pour les implés restantes) ; integrity loggue le hash SHA-256
  complet du DLL ([Integrity] self SHA-256 …) pour confirmer quel build tourne (deploy sauté si jeu ouvert).
- 🔑 ÉTIQUETTE DE TOUCHE par slot (option show_keys_) — ✅ RÉSOLU PROPREMENT 2026-06-30 via le VRAI getter
  (l'utilisateur a refusé la table en dur : "les keybinds sont dans UserKeys.lua, peuple avec le getter").
  **`UserHotkey_Lua_GetHotKey` 0x00d80950 `void*(__stdcall)(void* out, int category, int slot)`** — RET 0xc,
  3 args (le décompile Ghidra n'en montrait qu'1 ; lire le DÉSASSEMBLAGE du site d'appel pour les vrais args).
  category = onglet : **0=SkillBar_1Tab, 3=SkillBar_2Tab** (= GetOption(g_SkillInfoMgr,10)?3:0) ; le wrapper
  fait category+1 pour Lua (1-based). slot = index natif (col+row*9). Remplit `out` (struct >=0x38, le wrapper
  ré-init les 2 std::string) : **out+0x00 keycode1, out+0x04 keycode2, out+0x08 = std::string NOM de touche**
  ("F1"/"A"/"Shift+F1"…), **out+0x20 = std::string LIBELLÉ DE LA COMMANDE** (le champ `EXE` de
  SaveData\UserKeys.lua, ex. "Hotkey 2-1") — 🔴 CORRIGÉ 2026-08-12 : ce n'est **PAS** une « 2e touche ».
  Preuves : UIHotKeyWnd_OnDraw dessine out+0x20 dans la colonne des LIBELLÉS, le commit passe cette chaîne à
  `ChangeUserHotKey`, et UserKeys.lua stocke `[9] = { EXE = "Hotkey 2-1", KEY1 = 65 }`. Le code de Bourgeon ne
  lit que +0x08 -> rien à corriger côté source. Cf. [[reference_hotkey_settings_window_re]].
  ⚠️ DÉTRUIRE les 2 std::string (FUN_004f08f0)
  sinon fuite. Lit UserKeys.lua via g_UILuaState -> touche RÉELLE (rebinds inclus) ET **layout-aware**
  (AZERTY/QWERTY, le nom vient du jeu). C'est EXACTEMENT la source du tooltip natif (UIShortCutWnd_OnMouseMove
  0x008f7f50, branche `if(GetOption(mgr,10)==0) GetHotKey(out,0,slot) else GetHotKey(out,3,slot)`). Plugin
  `GetSlotKeyLabel(tab,slot)` = un seul appel + CopyMsvcString(out+0x08). (L'ancienne approche "lire la
  std::list g_UserHotkeyMgr+0x10" était FAUSSE : cette liste = overrides seulement, VIDE tant que rien n'est
  rebindé live -> ratait tous les défauts. Et la table en dur était devinée AZERTY -> rejetée.) Persisté yaml
  skillbar_show_keys. Format gras configurable (bold_text_, faux-gras 1px) + couleur (col_keytext_).
- COUNT OBJET = quantité LIVE inventaire (jamais stockée), comme la barre native OnDraw 0x008f5800 :
  GetItemLiveCount(nameid) = ItemSkillMgr_GetInfoByResId_UNIFIED(0x00d5a980)(mgr,info,nameid,1) ; info+0x04
  trouvé, **info+0x10 = quantité**. Interrogé chaque frame -> décrémente à l'usage. DecodeDrag met d->level=0
  pour les objets (NE PAS persister le count -> sinon corruption observée 940->255 = clamp octet au round-trip
  serveur). SKILL : rec[5]=niveau (payload+0x6c) — REQUIS par le 0x29 cast (rec0!=0 branche : rec[5]!=0 &&
  rec[5]<=maxlvl) et l'OnDraw natif. OBJET (rec0==0) : le 0x29 ne lit pas rec[5].
- 🔑🔑 **CAUSE RACINE DU FLIP-FLOP (résolue 2026-06-30, prouvée par LogDiag live) — c'était le DECODE du
  DRAG, PAS la convention** : il existe **2 formats de payload** (g_DragDropMgr 0x1213338, charge à +0x308) :
  • `DragDropMgr_BeginDrag_FullPayload` 0x00c938e0 → écrit l'octet de format **+0x388 (=payload+0x80) = 0** ;
    appelé par **inventaire/cart/stockage = OBJET** (nameid @payload+0x18, count @+0x08).
  • `DragDropMgr_BeginDrag_LitePayload` 0x00c93a40 → octet **+0x388 = 1** ; appelé par le **grimoire
    UISkillListWnd (0x009783e0) = SKILL** (id @payload+0x04, level @+0x6c).
  ⇒ Pour un drag EXTERNE (srcSlot=-1) : **octet 0 = OBJET, octet 1 = SKILL**. (Les re-drags de la barre
  FUN_008f6d90 inversent ce mapping — Full pour rec0==0 skill, Lite pour rec0!=0 objet — MAIS ont
  srcSlot>=0 donc rejetés par HandleNativeDrop.) Notre `DecodeDrag` faisait `isItem = octet != 0` →
  INVERSÉ → écrivait rec0 inversé → tout l'écran inversé. FIX : **`isItem = (octet == 0)`** (confirmé live :
  DROP 617 inventaire→octet0=OBJET, DROP 29 grimoire→octet1=SKILL — type OK).
  • **ID ASSIGNABLE — DIFFÉRENT par type (confirmé par candidats logués live)** :
    – **OBJET = `atoi(resname BRUT)` à payload+0x3c** (=mgr+0x344) = nameid (ex. 603 Old Blue Box, 617).
      (payload+0x24=resname transformé icône ; payload+0x18=ItemSkillInfo[0] flag=2 ; GetInvItemById(
      payload+0x04)+0x08 = **INDEX inventaire** PAS le nameid — ex. Old Blue Box idx 29.)
    – **SKILL = payload+0x04 BRUT** (=l'id skill du grimoire, ex. 34=Blessing, GetSkillName OK). ⚠️ NE PAS
      re-lookup : LookupSkill(34)→7219 FAUX ; et res3c=0 (resname skill = ALPHA "AL_*" → atoi 0).
    niveau objet = payload+0x08, niveau skill = payload+0x6c. (FullPayload param_2 = ItemSkillInfo via
    FUN_00d5aa40 ; grimoire LitePayload param_2 = local_4c = id skill, param_3 = resname alpha.)
  • ✅ CLIC-GAUCHE (activation) — 2 gates natifs dans OnMsg case 0x29 (handler décompilé) :
    1. **GATE option #5** : `if (SkillMgr_GetOption(g_SkillInfoMgr,5) < 1)` — option #5 = UI-LOCK natif
       (≥1 BLOQUE skill ET objet). VÉRIFIÉ LIVE = 1 (map d'options g_SkillInfoMgr+0x7f8c). Comme on
       utilise notre barre (native cachée) ce verrou est parasite → ActivateSlot le met à 0 le temps de
       l'appel puis le restaure (GetOption 0x008e1d50 / SetOption 0x005c5950 (mgr,key,val,0), __thiscall).
    2. **OBJET exige rec[5] != 0** : branche objet `sVar1=*(short*)(rec+5); if(sVar1!=0 && sVar1<=qté)
       dispatcher(0x71=USE,item,sVar1)`. Nos drops objet avaient rec[5]=0 → jamais utilisables. DecodeDrag
       met le count objet à >=1 (FullPayload param_3 @payload+0x10, repli 1). (Skill : pas de check count.)
    Slot index natif = this+0xc4 + col*4 + row*0x24 (=col+row*9). ActivateSlot passe col=i%9,row=i/9.
    Use skill = cmd 0x1b(sol)/0x13(cible-soi)/0x57 selon type ; use objet = cmd 0x71 via g_UICommandDispatcher.
  • 🔑 SKILL : DEUX espaces d'id (RE FUN_00d5aa40 LookupSkill vs FUN_00d5a980 GetSkillInfo) :
    – CANONIQUE = node+0x0c = id grimoire = ce que GetSkillName nomme + l'icône (0x00d7fa90) résout en SKILL.
    – INTERNE = atoi(node+0x34 resname) = souvent un id d'OBJET (ex. 12622=item!) = ce que GetSkillInfo
      matche (donc requis par le cast 0x29 ET la desc). LookupSkill matche node+0x0c==canon → atoi(resname)=interne.
    ⇒ On STOCKE le CANONIQUE (nom/icône/tooltip corrects ; stocker l'interne donnait une icône d'OBJET +
    nom "Unknown"). Cast/desc : on CONVERTIT canon→interne À LA VOLÉE via `SkillCanonToInternal` (=
    ItemSkillInfo_GetId(LookupSkill(canon))) — ActivateSlot échange temporairement rec.id→interne autour
    de OnMsg 0x29 (synchrone, restauré après), OpenSlotDescription passe l'interne à GetSkillInfo. Aucun
    changement de l'id stocké. (Objets : inchangés, l'id stocké = nameid marche partout.) ⚠️ La convention du RECORD (rec0==0 skill / ==1 objet) était CORRECTE depuis
  le fix précédent ; c'est l'alimentation (decode drag) qui était fausse. NE PAS re-toucher la convention.
- +0x154 = colonne pressée (-1) ; +0x158 = ligne touchée (-1) ; +0x15c = nb lignes (1..4)
- +0x160..+0x180 = extension itemslot (9 ptrs) ; +0x184/+0x188 = boutons ouvrir/fermer itemslot

## OnMsg 0x00901310 (param_2 = message)
- case 6 (bouton, param_3=cmdId) : 0x24c/0x24d ouvrir/fermer panneau itemslot ;
  0xd8 = "+" (rows++, >4 → reset 1) ; 0xd9 = "−" (rows--, min 1) ;
  0x225 = toggle cooldown (purge g_ShortCutCooldownList) ; 0xc9 = ToggleWindow(0x24). Puis msg 0xe.
- case 0xe = relayout/resize (formule hauteur ci-dessus + repositionne boutons enfants).
- case 0x17 = reconstruit this+0xc4 depuis g_ShortCutSlots_Tab0/Tab1 (onglet=option #10) + itemslot ext.
- case 0x22 = SetPos depuis info sauvée ; rows = hauteur/0x21.
- **case 0x29 = ACTIVATION slot (param_3=col, param_4=row)** : use item (cmd 0x71) ou use skill
  (cmd 0x1b sol / 0x13 cible-soi / 0x57 selon type). Aboutissement des touches F1..F9.
- case 0x8f = rotation indices anim cooldown. default = base FUN_008841d0.

## Drag-drop
- Début : OnLButtonDown 0x008f6fc0 → `FUN_00c93a40`(item)/`FUN_00c938e0`(skill) sur
  **g_DragDropMgr = 0x1213338** (getter FUN_00a75340), drag **type 7** = raccourci.
- Fin : OnDrop 0x008dd590 → `SkillMgr_SetShortCutSlot` 0x00d96c20(g_SkillInfoMgr,type,id,lvl,slotIdx,…).
  Drop hors barre = efface le slot ; déplace aussi le nœud cooldown (FUN_009093b0).

## Globals (renommés)
- g_ShortCutItemSlotOpen 0x016024c5 (byte) — panneau itemslot ouvert
- g_ShortCutHotRow 0x015fa45c (byte) — ligne haute/scroll active
- g_SkillInfoMgr 0x015fa3c0 (CSkillInfoMgr) ; g_ShortCutCooldownList 0x015ff7e0 (+0x015ff7e4 count)
- g_ShortCutSlots_Tab0 0x015fa850 / g_ShortCutSlots_Tab1 0x015fa94c (36×7 o) ; g_ShortCutItemSlotExt 0x015faa4c (9×7 o)
- `g_ShortCutDrawnIcons_Bar1` 0x015fa434 / `g_ShortCutDrawnIcons_Bar2` 0x015fa43c — icônes DESSINÉES à la
  frame courante (vidées+reconstruites par OnDraw). ⚠️ ex-"g_ShortCutItemDragList" : nom trompeur, ce sont
  elles qui conditionnent l'enregistrement des cooldowns (voir le bloc 🔴 ci-dessus).

## Helpers renommés
- `UIWnd_BlitIconByResName` 0x00871040(this,x,y,info) — icône via nom res à info+0x2c
- `SkillMgr_GetSkillInfo` 0x00d5a980 ; `ItemMgr_GetInvItemById` 0x00d7fa90
- `SkillMgr_SetShortCutSlot` 0x00d96c20. ⚠️ **0x00d9d400 N'EST PAS de cette fenêtre** (ex-nom
  `UIShortCut_LoadListFromLua`, corrigé 2026-08-15 en `EmotionHotkey_LoadListFromLua`) : c'est le
  chargeur des 10 MACROS de chat de la fenêtre « Shortcut List » (id 86) — voir
  [[reference_shortcut_list_macro_re]]. Rien à voir avec la barre de skills.
- Ressources : `\Shortcut\shortcut_frame.bmp`, `_frame_item.bmp`, btn_plus/minus_*, btn_open/close_itemslot_*.

## Stratégie de relayout (à creuser)
1. Hook `UIShortCutWnd_OnDraw` (vtable+0x50 swap, comme StatusTweaks [[project_status_tweaks_plugin]])
   pour redessiner en grille libre + hook `HitTestSlot` (0x008e2470) en MIROIR (les 2 doivent
   rester cohérents sinon clic/tooltip décalés). Ou repatcher les constantes 0x1d/0x21/0x10.
2. Pour "plusieurs barres" type Bartender : exposer les 2 onglets + 4 lignes (72 bindings) comme
   barres séparées déplaçables (chaque ligne = une "bar"). Persistance via yaml Bourgeon.
3. Drag-drop conservé en réutilisant OnDrop/OnLButtonDown ou en routant vers SkillMgr_SetShortCutSlot.

## Plugin SkillBarTweaks — REFONTE en barres ImGui (dessin pur)
`src/plugins/skill_bar_tweaks.{h,cc}` (réécrit 2026-06-29). Build Release OK, déployé. PAS câblé MoonlightUi.
Toggle panneau = VK_OEM_3 (²/~). L'approche "patch OnDraw + détour HitTest" précédente est ABANDONNÉE
(elle s'appliquait — log "disposition libre ON" sans erreur — mais le modèle colonnes×1-ligne tronquait à
1 ligne ⇒ inutile/déroutant). Nouvelle archi décidée par l'utilisateur + workflow multi-agents :

**Barre native vivante mais CACHÉE, vue dessinée en ImGui (rectangles+couleurs+texte, AUCUNE texture).**
- Cacher : `wnd->vtable[0x38](0)` = `FUN_009030c0` (met this+0x28=0, délink draw-list). **JAMAIS** le
  destructeur (vtable[0]=0x008dbcf0) : il nulle `g_UIWindowMgr+0x1e8` et tue le clavier.
- **Clavier F1-F9 préservé** : `FUN_00a471e0`(kbd)→`FUN_00a451e0`(dispatch, si behavior<0x2d)→
  `[g_UIWindowMgr+0x1e8]->OnMsg(0,0x29,slot%9,slot/9,0,0)`. Passe par le cache **mgr+0x1e8**, PAS par la
  visibilité ⇒ marche caché. `mgr+0x1e8` = singleton posé dans MakeWindow @0x00a3a21c (créer 1× via
  MakeWindow 0x00a39340 / ToggleWindow 0x00a4bf30 si null).
- **Source d'index UNIQUE** (invariant critique) : lire chaque slot via `this+0xc4[i]` (pointeur vers le
  record 7o dans les globals → données live) ET activer par `OnMsg(0,0x29,i%9,i/9)` (lit aussi this+0xc4[i]).
  `OnMsg(0x17)` reconstruit this+0xc4 — renvoyé au hide + au changement d'onglet (les pointeurs restant
  valides, les data se rafraîchissent seules ; pas besoin par-frame, M6: 0x17 coûteux).
- Activation gate native : OnMsg case 0x29 skippe si `GetOption(&g_SkillInfoMgr,5)>=1` (UI-lock, respecté).
  Onglet courant = `GetOption(&g_SkillInfoMgr,10)` (0/1). Clic droit = vider via `SkillMgr_SetShortCutSlot`
  0x00d96c20 `(mgr=g_SkillInfoMgr, type, id=0=clear, level, slotIdx, tab)` (écrit les globals direct).
- 🔴 **COOLDOWN — la liste native est INUTILISABLE quand la barre native est cachée (résolu 2026-07-27).**
  Symptôme : aucune case ne s'assombrit jamais dans la barre ImGui (flagrant sur les skills de GUILDE,
  cooldown de plusieurs minutes). CAUSE (RE `Recv_SkillPostDelay` **0x00cd60b0**, case 1085 = ZC 0x043D) :
  le handler n'insère un nœud dans `g_ShortCutCooldownList` QUE si le skill figure déjà dans une des deux
  listes d'icônes **DESSINÉES** par les barres de raccourcis (`g_ShortCutDrawnIcons_Bar1` 0x015fa434 /
  `_Bar2` 0x015fa43c), listes **vidées puis reconstruites à chaque OnDraw** (UIShortCutWnd_OnDraw 0x008f5800
  @0x8f5953 = clear). Barre native cachée ⇒ listes vides ⇒ AUCUN cooldown enregistré, jamais. Même visible,
  seul l'onglet affiché compterait (barres 2/3 de la multibar exclues). ⇒ **Ne jamais lire cette liste comme
  source.** FIX : table maison alimentée par le paquet, `src/ragnarok/skill_cooldowns.{h,cc}` (namespace `ro`) —
  observe ZC 0x043D {skillId.W, durée.L} en un point unique (`Bourgeon::FireRecvPacket`), purge au retour
  login (FireModeSwitch != kGame), API `SkillCooldownRemainingMs` / `SkillCooldownFraction` (repli sur la
  liste native), consommée par skill_bar_tweaks (voile + décompte m:ss) ET character_sheet (onglet Guilde,
  qui avait sa propre copie — supprimée). ZC 0x043E (`Recv_SkillPostDelayList` 0x00cd6cb0) insère, LUI,
  sans condition, mais rAthena ne l'émet pas.
- Cooldown (structure, pour mémoire) : ⚠️ `0x015ff7e0` est l'**objet std::list** MSVC, PAS la sentinelle (le finding workflow était FAUX
  et a causé un FREEZE/boucle infinie confirmé live via EIP). La sentinelle `_Myhead` (tas) = `*(0x015ff7e0)` ;
  itérer `head=*(0x015ff7e0); n=head->next; tant que n!=head` (comme OnDraw: ECX=*(0x015ff7e0);EDI=[ECX];cmp EDI,ECX).
  `*(0x015ff7e4)`=taille. Nœud **0x24o** {next+0,prev+4,skillId+8,endTick+0xC,dur+0x10};
  horloge = timeGetTime() si `*0x015beecc==0` sinon `*(FUN_00b1fac0()+0x20)`. CooldownFraction par id exact
  (pseudo-groupes 0x241/9999 via 0x00d8e920/0x00d8e9a0 = v2).
- Icônes du jeu en ImTextureID = possible (recette `menu_icons.cc` : TexMgr load +0x114/+0x118/+0x11c BGRA →
  `Overlay_CreateTextureARGB`, texture possédée, pas de handle volatile) — chemin `CP949(유저인터페이스)\item\%s.bmp`
  (fmt @0x00fe07d4), skill resname=SkillInfo+0x2c (gate SSO +0x40), item resname=ItemInfo+0x20. **Reporté v2.**
- v1 LIVRÉ & confirmé en jeu : 1 barre (colonnes/taille/espacement/position/nb slots, sliders à la MOLETTE
  ±1), clic G=use (OnMsg 0x29 OK), clic D=vider UNIQUEMENT en mode édition, édition = drag depuis N'IMPORTE
  OÙ (surface drag plein-cadre + hit-test slot manuel). Couleurs configurables (fond cadre/objet/skill/vide/
  bordure/bordure-survol) via le MÊME picker que le fond du chat (ColorButton->popup ColorPicker4 AlphaBar).
- COULEURS (per utilisateur, faisant foi) : **type 0 = OBJET (bleu), type ≠ 0 = SKILL (vert)**. (La
  décompil. de 0x00d5a980 = liste skills suggérait l'inverse, mais l'utilisateur voit son jeu — on suit ça.)
- v2 — **icônes du jeu** : ⚠️ LES .BMP DU GRF SONT NOMMÉS PAR RESOURCE-NAME CP949, **PAS par id**
  (ex. `유저인터페이스\item\¼ÒµÊ.bmp`). Fonctions natives clés (Ghidra) :
    • `ResolveItemResNameById` **0x006a2bd0** = ton "getItemRes()" : id -> resname (DB objets, table 0x01255130).
    • `BuildItemIconGrfPath` **0x00d5a720** **__stdcall(char* id_str, char* out[>=260])** : atoi(id) ->
      ResolveItemResNameById -> `sprintf(out, "유저인터페이스\item\%s.bmp", resname)`. **Donne le chemin
      complet d'icône OBJET directement depuis l'id** (marche HORS inventaire). = la bonne fonction.
    • SKILL : **0x00d7fa90 __stdcall(out,id)** -> resname `*(char**)(out+0x20)` -> `...\item\<rn>.bmp`
      (getter unifié : route vers skill-mgr pour ranges custom, sinon DB ; = CE QUI MARCHE pour les skills.
      ⚠️ NE PAS utiliser 0x00d5a980 pour les skills — liste this+0x16f0 trop restreinte, casse les icônes).
  Plugin (skill_bar_tweaks GetIconTex) : OBJET via BuildItemIconGrfPath(id), SKILL via 0x00d7fa90 ; essaie
  les 2 (fail-safe), cache par id, SEH + repli boîte-id, FlushIconCache au changement de zone. Upload =
  recette menu_icons.cc (TexMgr 0x00a90350/Load 0x00a8d4a0, +0x114/+0x118 W/H, +0x11c BGRA, colorkey magenta
  -> Overlay_CreateTextureARGB). À VALIDER en jeu après ce build.
  Autres helpers : `ItemBtn_LoadIconByResName` 0x00857350 (charge icône objet depuis objet+0xac), `BuildItemIconGrfPath`.
- **Filtre texture** : `Overlay_SetTextureFilter(bool linear)` ajouté dans d3d9_hook (sampler0 POINT/LINEAR sur
  g_imgui_device) ; plugin toggle "Filtre bilineaire" via ImDrawList::AddCallback + ImDrawCallback_ResetRenderState.
  Défaut POINT (net). + sliders à la molette (±1), drag-partout en édition, fond de cadre configurable.
  FAIT : persistance yaml (skillbar_* dans bourgeon_settings.yaml via MoonlightUi Load/Save + drain dirty_),
  drag-réarranger (ImGui DragDropSource/Target, payload "SBSLOT", swap via SetSlot/MoveSlot 0x00d96c20),
  fix décalage édition (pad=0), fix DOUBLON natif+ImGui au relog (re-cacher chaque frame si this+0x28!=0,
  pas un one-shot — le client ré-affiche la barre au relog/retour en jeu).
  FAIT : aperçu de drag = l'icône qui suit le curseur (ImGui::Image, PopupBg transparent + ImageBorderSize 0).
- **Drag-drop NATIF -> slot ImGui** (grimoire/inventaire vers la barre), RE par workflow + critique adverse :
  - Détection + objet : `FUN_00a75340(0x1213338)` (`__fastcall(mgr)`) -> objet de drag en cours (gate
    `*(0x1213338+0x58)==1`) ou null. ⚠️ NE PAS confondre 0x1213338 (g_DragDropMgr) avec 0x0121333c
    (g_UICommandDispatcher). NE PAS utiliser +0x388 comme "drag actif" (c'est le byte type).
  - Charge = **objet+0x308** (== OnDrop param_3) : +0x80 byte type (0=skill/1=objet), +0x04 id, +0x14
    srcSlot (-1 si source=fenêtre, >=0 self-drag barre), +0x6c count(objet)/level(skill).
  - id ASSIGNABLE : OBJET -> `ItemMgr_GetInvItemById(0x00d7fa90)(out,id)` puis nameid=`*(out+0x8)`, level=count.
    SKILL -> `FUN_00d5aa40(__thiscall mgr,out,id)` puis id=`FUN_005d98a0(&out)` (atoi info+0x2c), level=0.
  - Assign normal (hors drag, ex. MoveSlot/ClearSlot) : `SkillMgr_SetShortCutSlot(mgr, type(0 skill/!=0
    objet), id, level, slot, tab)` 0x00d96c20. Un raccourci RÉFÉRENCE l'objet (pas de consume).
  - ⚠️⚠️ RECORD 7 octets & CONVENTION DE TYPE (AUTORITAIRE, workflow 3 chemins décompilés, haute conf) :
    record = {+0x00 byte type, +0x01 dword id, +0x05 short level}, stride 7. tab0 = g_SkillInfoMgr+0x490
    (0x015fa850), tab1 = +0x58c (0x015fa94c). ⚠️⚠️ **type : 0 = OBJET, !=0 = SKILL** (VÉRIFIÉ LIVE OCTET PAR
    OCTET — voir le bloc 🔒 "CONVENTION DE TYPE — DÉFINITIVE" dans la section Struct ci-dessus). La lecture
    décompilée de OnDrop/OnMsg 0x29 (`if(*rec!=0)`→ItemMgr) est TROMPEUSE (0x00d7fa90 = getter unifié) ;
    IGNORER. id = nameid objet / skill id. level = count objet (rec+5) / niveau skill (rempli par OnMsg 0x17).
    ⚠️ Tout le code plugin est aligné sur OBJET=rec[0]==0 (color/tooltip/WriteSlotRecord/OpenSlotDescription/
    GetIconTex). [Ancien texte erroné supprimé.] L'ÉCRITURE est `rec[0]=is_item?0:1` (NE PAS la flipper, sinon
    l'activation casse). DecodeDrag (offsets 0x80/0x04/0x14/0x6c + résolutions) vérifié CONFORME au OnDrop.
  - ⚠️⚠️ 3 crashes 0xc0000005 ont prouvé qu'on NE PEUT PAS appeler de fonction du jeu pendant un drag.
    Donc DROP NATIF = `WriteSlotRecord` (ÉCRITURE DIRECTE du record via this+0xc4[slot]) + `CancelNativeDrag`
    (VIDAGE charge dragobj+0x308 : id+0x04/cat+0x00/type+0x80 = 0), SEH, ZÉRO appel jeu. Pourquoi pas
    SetShortCutSlot/gate : (1) SetShortCutSlot finit par `(*(*FUN_00a75340(0x1213338)+0x18))(0x139,...)` =
    notify dispatcher -> PENDANT un drag crashe profond (0x00c4777f) ; OK hors drag (MoveSlot/ClearSlot).
    (2) gate 0x01213390 (g_DragDropMgr+0x58) = état dispatcher PAS flag de drag : à 0 -> FUN_00a75340 NULL
    -> SetShortCutSlot `*NULL` -> crash 0x00d96fc1 (ecx=0). NE JAMAIS toucher. (3) vtable via *(0x0121333c) =
    garbage -> crash ; FUN_00a763c0(2) = cleanup ShellExecute -> NE PAS utiliser.
  - ⚠️ DROP NATIF géré au **WM_LBUTTONUP dans le hook WndProc** (ragnarok_client.cc), PAS en OnRenderUI.
    Raison : OnRenderUI passe APRÈS l'input -> pour éviter le drop-au-sol il fallait annuler le drag en cours
    de route (à l'entrée dans la barre), mais annuler un drag natif pendant que le bouton G est maintenu fait
    que le JEU reclasse le maintien en CLIC-AU-SOL -> le perso marche vers le curseur. (L'ancienne approche
    "capture/portage" via CancelNativeDrag-à-l'entrée avait ce bug.) Solution : laisser le drag natif VIVANT
    pendant la traverse (le jeu reste en mode drag, suiveur natif visible, pas de marche) ; au relâchement,
    `SkillBarTweaks::HandleNativeDrop(mx,my)` (appelé par WindowProcHook au WM_LBUTTONUP, PRÉ-input) fait
    WriteSlotRecord + CancelNativeDrag AVANT que le jeu ne traite le up -> pas de drop sol, puis le up suit
    son cours (jeu solde son drag à vide). Le WndProc voit bien ce up : g_mouse_captured_by_game suit le
    bouton pressé hors-ImGui (sur l'inventaire) jusqu'au up même au-dessus d'ImGui. Assignation = à la case
    RELÂCHÉE (plus de snap-à-la-1ère-traversée). Géométrie de HandleNativeDrop = celle de DrawBar (pad=0).
  - ⚠️ ORDRE des sources d'ICÔNES dans GetIconTex = EMPIRIQUE (calé en jeu), NE PAS "logiquer" : type0 ->
    ItemPath(0x00d5a720) d'abord, type!=0 -> SkillPath d'abord (fallback 2-passes pour
    l'autre). Les helpers ne sont PAS strictement objet/skill ; inverser cet ordre CASSE les icônes de sorts
    (vu en jeu). (J'avais flippé "pour cohérence" avec la convention type -> régression, annulée.)
  - ✅ ICÔNE SKILL D'UNE AUTRE CLASSE (perso GM multi-classe) — RÉSOLU 2026-07-04. Symptôme : après relog,
    un slot de skill que le perso N'A PLUS (classe précédente) affiche nom/lv/id OK mais **icône perdue**
    (repli boîte-id). CAUSE : `SkillPath` résolvait le resname via `0x00d7fa90` (ItemMgr_GetInvItemById) qui,
    pour un skill standard, appelle **`FUN_00737e00` = scan de la liste APPRISE seulement** (this=&DAT_015fa3cc
    = g_SkillInfoMgr+0xc) -> resname VIDE si non appris -> pas d'icône. (Le NOM survivait car il vient de
    GetSkillName Lua, indép. de l'appris.) FIX : `SkillPath` résout d'abord via **`Lua_GetSkillIdName`
    0x0073a140 `char*(__cdecl)(int id)`** -> identifiant (ex. "AL_BLESSING"), "Zero Skill" si invalide (rejeté).
    Icône = `유저인터페이스\item\<GetSkillIdName(id)>.bmp` = LA source native (builder d'effet 0x00bda890 fait
    le même sprintf via Cstr_sprintf 0x00529850 + fmt DAT_00fe07d4). Indép. de l'appris -> marche pour toutes
    les classes. Repli sur 0x00d7fa90 conservé pour les ids custom appris. Ghidra : Lua_GetSkillIdName renommé+
    commenté (+ proto), 0x00737e00 commenté (scan liste apprise), 0x00bda978 commenté (chemin icône). Jumeau de
    Lua_GetSkillName 0x0073a1f0 (nom d'affichage).
  - UI : plus de "mode édition" -> case **Verrouiller** (locked_, défaut true). Verrouillé = barre fixe + slots
    interactifs (clic G use, glisser réarrange, **Shift+clic D vide** la case survolée, drop natif assigne).
    Déverrouillé = glisser n'importe où déplace la barre (slots non interactifs).
  - Option **Clic-traversant** (clickthrough_, persisté skillbar_clickthrough) : ajoute
    `ImGuiWindowFlags_NoMouseInputs` à la fenêtre barre quand `clickthrough_ && !KeyShift` -> les clics
    traversent vers le jeu (le WndProc skippe les fenêtres NoMouseInputs) ; Shift maintenu = réinteractif.
    Le drop natif (WndProc) reste actif quel que soit l'état.
  - **Cases vides traversantes** (par défaut, verrouillé) : NoMouseInputs aussi quand le curseur n'est PAS
    sur une case REMPLIE (ReadSlot(w,slot).valid, hit-test géométrique avant Begin) ET hors drag ImGui
    (`GetDragDropPayload()==null`, pour pouvoir déposer sur une case vide) ET verrouillé (déverrouillé =
    saisir la barre partout). -> clic sur case vide/espace = passe au jeu (fermer une fenêtre derrière,
    se déplacer). Demandé par l'utilisateur (capture inutile sur case vide).
  - Aperçu de drag (réarrangement SBSLOT) SANS liseré : `ImageBorderSize=0` NE SUFFIT PAS (le bord revient
    dès que l'icône charge en image vs repli texte) -> pousser AUSSI `ImGuiCol_Border` transparent (+ PopupBg
    transparent + WindowPadding/WindowBorderSize 0). PopStyleColor(2)/PopStyleVar(3).
  - À VALIDER en jeu (build b7c2d8da…, deploy en attente fermeture client) : (a) traverser la barre et déposer
    sur la case voulue, (b) plus de clic-au-sol/marche à l'entrée, (c) pas de drop sol, (d) bons type/couleur/
    icônes, (e) Shift+clic-D vide, déverrouillé déplace.
  - À VALIDER : sémantique +0x30c objet (index vs nameid, neutralisé par re-résolution GetInvItemById).
  TODO v2 restant : multi-barres, qty objet live.
  - ✅ PERSISTANCE (build d548e0d3) : sur moonlight/rAthena les barres sont sauvées CÔTÉ SERVEUR. Le client
    envoie `CZ_SHORTCUT_KEY_CHANGE2` à CHAQUE changement -> `clif_parse_Hotkey` met à jour
    sd->status.hotkeys[index+tab*MAX_HOTKEYS] (sauvé au save perso ; ZC_SHORTCUT_KEY_LIST le renvoie au login).
    Notre écriture directe (drop natif) NE l'envoyait pas -> non sauvé. Fix : `SendHotkeyChange(tab,index,
    type,id,level)` via Bourgeon::Instance().SendPacket, packet **0x0b21** (RE>=20190508) =
    `[op u16][tab u16][index u16][isSkill u8=type][id u32][count u16=level]` (13 o). isSkill = le type record
    BRUT (NATIF : **0=SKILL, 1=OBJET**), AUCUNE inversion (chargement FUN_00cae3d0 = copie brute). Appelé dans SetSlot (couvre MoveSlot), ClearSlot,
    et HandleNativeDrop (après WriteSlotRecord). Double-envoi éventuel avec SetShortCutSlot = inoffensif (le
    serveur ne fait que MAJ la struct en mémoire). MAX_HOTKEYS=38/onglet (×2), client affiche 36 (9×4)/onglet.
    Opcodes voisins : CZ_SHORTCUT_KEY_CHANGE1=0x02ba (vieux), ROTATE2=0x0b22 (hotkey_rowshift/onglet).
  - 🔴 LA BARRE D'ITEMS EST L'EXCEPTION : le natif ne persiste PAS ses cases (OnDrop appelle
    SetShortCutItemSlot, **aucun paquet** ne part) ⇒ rAthena n'en sait rien, sa table `hotkey`
    (clé char_id) ne porte que les 2 onglets de SKILLS. Son contenu était donc chez nous, dans les clés
    `skillbar_item*` de bourgeon_settings.yaml — fichier UNIQUE pour l'installation, donc **une seule barre
    d'items pour tous les persos de tous les comptes**. Migré 2026-08-29 vers `SaveData\bourgeon_itembar.yaml`,
    un map **CID -> {slot: nameid}** écrit par SkillBar lui-même : CID capturé À LA RESTAURATION
    (`rag::OwnCharIdSafe`, jamais relu à l'écriture — hors mode jeu la globale porte un vestige de la session
    précédente), écriture après 1 s de calme + flush forcé à la sortie du mode jeu, clé **0 = barre héritée**
    (seed d'un perso encore inconnu ; une entrée VIDE reste vide, c'est un choix du joueur).
    La GÉOMÉTRIE des 3 barres, elle, reste globale dans bourgeon_settings.yaml.
  - ✅ IMPLÉMENTÉ (build 99dcc35d, deploy en attente) clic D = DESCRIPTION, réplique EXACTE du handler natif
    clic-droit `FUN_008f91a0` PAR INDEX DE SLOT (pas de HitTest). `OpenSlotDescription(w,slot,mx,my)` lit
    rec=this+0xc4[slot], id=rec+1 ; OBJET (rec[0]==1) : MakeWindow(0x2e) ; si (wnd+0x104)==id -> Close(0x2e)
    (bascule) sinon OnMsg(0,0x3d,id,0,0,0) ; puis SetPos(vtable+0x10) au curseur. SKILL (rec[0]==0) :
    [vérifié natif 0x008f91a0 : rec[0]!=0 -> ItemMgr_GetInvItemById+wnd 0x2e ; rec[0]==0 -> SkillMgr_GetSkillInfo+wnd 0xc]
    buf[0x100]={0}; SkillMgr_GetSkillInfo(0x00d5a980)(g_SkillInfoMgr,nullptr,buf,id,1) [si trouvé -> remplit
    via FUN_005b72d0, sinon vide via FUN_006a1a90 ; FLAG TROUVÉ = buf+0x04 != 0, comme le natif clic-droit] ;
    si trouvé MakeWindow(0xc)+OnMsg(0,0x18,buf,0,0,0)+SetPos ; PUIS free FUN_004f08f0(buf+0x44)+(buf+0x2c).
    Câblé dans DrawBar : verrouillé + survol + clic-D simple (Shift+clic-D reste = vider). OnMsg/MakeWindow
    appelables depuis OnRenderUI (comme ActivateSlot) car PAS pendant un drag. SEH (POD only).
  - ✅ RÉSOLU (2026-06-30) TOOLTIP SURVOL = nom en **tooltip ImGui** (`ShowSlotTooltip(w,slot)`), avec le NOM
    DES SKILLS STANDARD enfin trouvé. Format : objet `"%s (ID:%d)"`, skill `"%s - Lv:%d (ID:%d)"`.
    🔑 SOURCE DU NOM, par type (le tooltip natif de la barre FUN_008f7f50 fait pareil) :
    • OBJET (rec[0]==0) : DB item `FUN_006a0d40`(id, table 0x01255130) -> record+0x04 (nom EN) / +0x08 (loc) ;
      garde `!= sentinelle 0x01255138`. Les OBJETS sont chargés au BOOT par `FUN_006a4e20` (parse item.txt,
      eventItem.txt, CardItemNameTable.txt… QUE des objets).
    • SKILL (rec[0]==1) : `GetSkillName(id)` = wrapper Lua **`FUN_0073a1f0`** `__cdecl char*(int id)`, format
      "d>s", renvoie le nom ou "Unknown-Skill". C'est LA source (même que la fenêtre de skills / le tooltip natif).
    ⚠️ POURQUOI tout le reste échouait pour les skills standard : la map `0x01255130` est le **DB OBJET
    uniquement**. Les ids de skills standard N'Y SONT PAS — PROUVÉ live par traversée de l'arbre RB (clé 29
    Inc AGI introuvable : chemin racine…505→501→2→500→nil ; _Myhead 0x019f2118, 27221 entrées = objets).
    Donc GetBaseName 0x006a2b50, FUN_006a2ce0, BuildDisplayName — qui font TOUS `atoi(SkillInfo+0x2c)` →
    `ItemSkillDescDB_Lookup(id,0x01255130)` → renvoient la sentinelle ("Unknown Item"/" ") pour un skill.
    `EnsureLoaded` 0x006a06b0 = bookkeeping (FUN_0069f7f0→FUN_00a7ee00 = lookup d'un set, PAS un loader) →
    inutile, retiré. SkillInfo+0x44 (2e std::string "nom") est VIDE pour un skill (live) -> pas une source.
    (Les skills CUSTOM ~12622 SONT aussi dans 0x01255130 par id, donc l'ancien chemin marchait pour eux ;
    GetSkillName couvre TOUT -> on l'utilise pour tous les skills.)
    ⚠️ tooltip-curseur NATIF (FUN_00a753d0, dispatcher *(void**)0x0121333c) inutilisable depuis l'overlay :
    fenêtre JEU rendue AVANT notre ImGui (EndScene) -> passe SOUS la barre +1 frame -> on rend en ImGui.
    🔒 CONVENTION TYPE RE-CONFIRMÉE une 3e fois par les 3 voies concordantes : records live de la barre
    (this+0xc4 -> 0x015fa850, stride 7) = slot0 `01 1d000000 0a00`=skill 29 Inc AGI lvl10, slot1 skill 34
    Blessing, record objet 617 Old Purple Box = `00 69020000…`. DONC **rec[0]==0 OBJET, rec[0]==1 SKILL**.
    (Le décompile Ghidra de FUN_008f7f50 lit `if(*pcVar2==0)→GetSkillInfo` = inversion de lecture ; la donnée
    live prime.) build 1fd2fac8 (deploy en attente fermeture jeu).
  - ✅ RÉSOLU (2026-06-29) clic D = DESCRIPTION SKILL. Lead TODO confirmé+précisé (l'arg de OnMsg 0x18
    est un **SkillInfo* sur la pile**, PAS un id ; et la 1re construction n'a besoin QUE de l'id en
    chaîne décimale à struct+0x2c). Voir [[reference_skill_description_window_re]].
    Séquence standalone depuis un id : (1) buf[0xf4]={0}; FUN_006a1b20(buf) (ctor, __fastcall ECX=buf) ;
    (2) FUN_006a6570(buf, skillId) (__thiscall ECX=buf, itoa l'id -> resname str buf+0x2c) ;
    (3) wnd=UIWindowMgr_MakeWindow(0x0131f4e8, 0xc) ; (4) (*(wnd+0x94))(0,0x18,buf,0,0,0) — __thiscall
    ECX=wnd, **6 args pile** ; (5) FUN_004f08f0(buf+0x44); FUN_004f08f0(buf+0x2c) (free les 2 std::string).
    Fenêtre desc = id 0xc, vtable 0x01032aac, ctor 0x0088db90 (taille 0x250), cache mgr+0x218,
    OnMsg 0x008c18b0. La barre tooltip (OnMouseMove 0x008f7f50 ; clic-D = OnRButtonDown_Describe 0x008f91a0) fait pareil mais remplit le struct via
    SkillMgr_GetSkillInfo (liste apprise) ; le clic-D du grimoire (UISkillListWnd OnMsg 0x00971560 case
    0x62, vtable 0x0103f3ec) utilise la voie itoa minimale = CELLE À COPIER pour un id arbitraire.
- ⚠️ Attacher x32dbg CRASHE ce client (anti-debug) → Ghidra autoritaire + valider en jeu par comportement. Voir [[feedback_debug_tooling]].

## Sous-système clavier / hotkeys — RE FAIT (2026-06-30, Ghidra renommé+commenté)
Chaîne F1..F9 → activation de slot, et persistance, entièrement nommée :
- `UIWindowMgr_OnKeyDown` **0x00a471e0** (this=g_UIWindowMgr) : handler clavier racine ; filtre focus/textbox,
  route Enter/Espace vers le chat, puis ResolveBehavior + DispatchHotkeyBehavior.
- `UserHotkey_ResolveBehavior` **0x00a32c10** (__fastcall this=g_UIWindowMgr) → behavior id (-1 si rien).
  Appelle le **Lua global `GetBehaviorOfHotKey2`** (les bindings touche→behavior sont **stockés côté LUA**,
  `Lua Files\HotKey`, via `g_UILuaState` 0x015ffd78). Fallback : scan g_ShortCutSlots_Tab0 par keycode.
- `UIWindowMgr_DispatchHotkeyBehavior` **0x00a451e0** : hub behavior→action. **CAS BARRE** : si behavior < 0x2d
  → `(*(g_UIWindowMgr+0x1e8)->vtable[0x94])(0,0x29, behavior%9, behavior/9)` = OnMsg 0x29 (use slot), via le
  singleton UIShortCutWnd caché en mgr+0x1e8 (marche barre cachée). behavior ≥ 0x2d = gros switch raccourcis
  globaux (open windows / g_UICommandDispatcher). Gate cooldown via FUN_00b1fac0 (+0xb0.. par catégorie).
- **CUserHotkeyMgr = global `g_UserHotkeyMgr` (DAT_012517c4)** (+0x10 liste bindings, +0xc flag chargé).
  Persistance UIInfo.lua : `UserHotkey_SaveToTable` **0x0059eef0** (sérialise les 4 onglets SkillBar_1Tab/
  SkillBar_2Tab/InterfaceTab/EmotionTab + clé "UserHotkey_V2") / `UserHotkey_LoadFromTable` **0x0059e2c0** /
  `UserHotkey_LoadTab` **0x0059f290** (1 onglet). Ponts C→Lua (g_UILuaState) : `UserHotkey_Lua_ChangeHotKey`
  **0x005d56d0** (Lua "ChangeUserHotKey"), `UserHotkey_Lua_ClearUserHotKeys` **0x005d4910**,
  `UserHotkey_Lua_SaveUserHotKeys2` **0x005d54c0** (SaveData\UserKeys.lua), `UserHotkey_Lua_GetHotKey`
  **0x00d80950** (Lua "GetHotKey", **(out, category, slot) -> nom de touche** ; cf. bloc 🔑 étiquette de touche),
  `UserHotkey_Lua_GetOriginalListSize` **0x00d81680**.
- Reste (mineur, Lua-piloté) : `UIHotkeyGuideWnd` (RTTI 0x01240538, fenêtre config des touches) non RE — pas de
  xref RTTI directe, peu utile car les bindings vivent en Lua.

## Autres fonctions barre/grimoire renommées (2026-06-30)
- ⚠️ **Mislabel corrigé** : Ghidra `0x008f91a0` était `UIShortCutWnd_OnMouseMove_Tooltip` → renommé
  `UIShortCutWnd_OnRButtonDown_Describe` (c'est le CLIC-DROIT/description). Le **vrai** survol/tooltip =
  `UIShortCutWnd_OnMouseMove_Tooltip` **0x008f7f50** (construit le nom skill/objet, affiche via FUN_00a753d0).
- `UIShortCutWnd_OnLButtonUp_Activate` **0x008f6800** : HitTest → OnMsg 0x29 (use). Cas spécial skill 0x7f4.
- `UIWnd_SetVisible` **0x009030c0** (= ex vtable[0x38]/FUN_009030c0 du plugin : this+0x28=flag ; visible→OnMsg
  0x17+Invalidate+ajout draw-list FUN_00a4ccf0 ; caché→retrait FUN_00a2e5c0). `UIWindow_OnMsg_Default`
  **0x008841d0** (OnMsg base : msg0/1=bouton, msg 0x62=ouvre desc skill wnd 0xc). `StdList_EraseNode`
  **0x009093b0** / `StdList_InsertNode` **0x009095e0** (nœuds cooldown). `SkillMgr_SetShortCutItemSlot`
  **0x00da8f90** (extension itemslot g_SkillInfoMgr+0x68c). `SkillMgr_GetOption` **0x008e1d50** /
  `SkillMgr_SetOption` **0x005c5950** (idx5=UI-lock, idx10=onglet). `SkillId_IsSharedCooldownRangeA`
  **0x00d8e920** (id∈[8000,8060]) / `...RangeB` **0x00d8e9a0** ([0x2008,0x2031]∪[0x20d0,0x2109]).
- **Begin-drag** (g_DragDropMgr 0x1213338, payload à +0x308) : `DragDropMgr_BeginDrag_FullPayload`
  **0x00c938e0** (riche : id/count/4 cartes/options stride5/refine/resname ; écrit octet type **+0x388=0**) /
  `DragDropMgr_BeginDrag_LitePayload` **0x00c93a40** (léger : cat/id/resname/count ; écrit **+0x388=1**, +0x328=0).
  ⚠️ La mémoire les avait inversés (item/skill) — nommés **par structure** (pas de label skill/objet) à cause
  d'une **question ouverte** : le grimoire glisse un SKILL via *LitePayload* (octet type=1, **catégorie 8**),
  alors que la barre re-glisse un skill rec[0]==0 via *FullPayload* (octet=0, catégorie 0x10). OnDrop
  (`UIShortCutWnd_OnDrop_AssignSlot` 0x008dd590) discrimine via payload+0x80 (==0→skill `FUN_00d5aa40`+
  SetShortCutSlot type0 ; !=0→objet `ItemMgr_GetInvItemById`+type1) **ET** la catégorie +0x308. NE PAS asseoir
  un mapping type-de-DRAG sans validation live. La convention du **record stocké** (rec[0]:0=skill/1=objet)
  reste, elle, verrouillée (bloc 🔒). Commentaire de la question posé au désassemblage 0x008dd60a.
- **Grimoire UISkillListWnd** (vtable 0x0103f3ec, RTTI 0x01240678 ; source de drag des skills) :
  `UISkillListWnd_OnMsg` **0x00971560** (case 6=envoi ordre skills paquet 0x9fb / case 0x62=ouvre desc 0xc =
  modèle copié par le plugin / case 0x8e=rebuild), `UISkillListWnd_RebuildList` **0x00971a20** (boutons-lien),
  `UISkillListWnd_OnLButtonDown_BeginDrag` **0x009783e0**, `UISkillListWnd_HitTest` **0x00976010**,
  `UISkillListWnd_GetSkillInfoAt` **0x00976230**, `UISkillListWnd_IsPointInList` **0x00976cc0**.
- ⚠️ Attacher x32dbg CRASHE ce client (anti-debug) → Ghidra autoritaire + valider en jeu par comportement. Voir [[feedback_debug_tooling]].
