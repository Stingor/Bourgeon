# Barre de compétences en trois barres fixes (SkillBarTweaks)

> Journal du chantier. La fiche de mémoire `project_skillbar_multibar_wip` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

## ✅ TERMINÉ (2026-07-01) — fonctionnalité livrée et validée en jeu (voir TEST 1-5 + FIX 2 plus bas)
Transformer SkillBarTweaks (1 barre, onglet actif) en **3 barres FIXES** — choix utilisateur "jeu de
barres fixe" + "accès 72+9 via multibarre" (cf. [[project_shortcut_bar_re]]). Tout ce qui suit est la
RE + le journal d'implémentation, conservés comme référence. (Historiquement laissé non-commité car
l'utilisateur build/teste lui-même — [[feedback_dont_relaunch_game]].)

## Décisions d'archi
- 3 barres, index == région native :
  - bar 0 = Onglet 1 (skills), base **g_ShortCutSlots_Tab0 0x015fa850**, 36 slots, tab 0, hotkeyCat 0
  - bar 1 = Onglet 2 (skills), base **g_ShortCutSlots_Tab1 0x015fa94c**, 36 slots, tab 1, hotkeyCat 3
  - bar 2 = Items, base **g_ShortCutItemSlotExt 0x015faa4c** (=g_SkillInfoMgr+0x68c), 9 slots, items, pas de hotkey
- **LECTURE = globals directs** (base + i*7), PLUS via this+0xc4 (qui ne tient que l'onglet actif) →
  permet d'afficher les 2 onglets + items en même temps. Record 7 o : +0 type, +1 id (dword), +5 level (short).
- Config : par barre {visible,x,y,columns,first_slot,slot_count} ; global = icon_size/spacing/couleurs/
  flags (locked/bilinear/clickthrough/show_keys/bold). Persistance yaml par barre (skillbarN_*).

## RE activation (FAIT cette session)
- **OnMsg 0x29 ne lit QUE this+0xc4[col+row*9] (skills 0-35)** — décompile UIShortCutWnd_OnMsg
  0x00901310 : `pcVar6 = *(this + param_3*4 + param_4*0x24 + 0xc4)` (param_3=col, param_4=row,
  0x24=36 o=9 ptrs). Gate option #5 (UI-lock, >=1 bloque). Donc activer un slot d'onglet NON-actif =
  basculer l'onglet : `SetOption(mgr,10,tab,0)` + `OnMsg(0x17)` (rebuild this+0xc4) + `OnMsg(0x29,col,row)`
  + restaurer onglet + OnMsg(0x17). (DrawBar lit les globals → l'onglet actif n'affecte PAS l'affichage.)
- **Items NE passent PAS par OnMsg 0x29** (this+0x160, pas this+0xc4). Use item = à RÉPLIQUER :
  branche objet de 0x29 = `ItemMgr_GetInvItemById(0x00d7fa90)(out,id)` [found out+0x04, qty out+0x10]
  puis `(*(*g_UICommandDispatcher[0x0121333c] + 0x18))(0x71,&out,count,0,0)` (__thiscall this=disp).
  ⚠️ SPÉCULATIF : les slots d'items ont rec[5]=0 (SetShortCutItemSlot le met à 0) donc le check natif
  sVar1!=0 ne s'applique pas ici → je dispatch avec count=1. À VALIDER EN JEU.
- **Assign item (drag inventaire→barre items)** : `SkillMgr_SetShortCutItemSlot 0x00da8f90`
  **__thiscall(mgr=g_SkillInfoMgr, id, slot)** : écrit g_SkillInfoMgr+0x68c+slot*7 (type=0,id@+1,lvl=0),
  unicité (vide l'ancien slot du même id) ; id==0 => vide. (Rearrange/clear items = via cette fn.)
- Skills assign/rearrange/clear = SkillMgr_SetShortCutSlot 0x00d96c20 (mgr,type,id,lvl,slot,tab) (déjà
  utilisé) + SendHotkeyChange 0x0b21 (persist serveur). **Items : persistance serveur = TODO** (opcode
  inconnu ; SetShortCutItemSlot ne notifie rien). Le drag natif item natif passe par OnDrop 0x008dd590
  quand g_ShortCutItemSlotOpen.
- Hotkey labels : GetHotKey(out, cat, slot) cat 0/3 pour onglets ; **items = pas de catégorie connue →
  pas d'étiquette** sur la barre items.

## ÉTAT : IMPLÉMENTÉ 2026-06-30 (nuit) — NON-COMMITÉ, à BUILDER + TESTER
Fait dans .h + skill_bar_tweaks.cc + moonlight_ui.cc :
1. ✅ .h : BarCfg{visible,x,y,columns,first_slot,slot_count} + bars_[3] (defaults : bar0 visible, bar1/bar2
   masqués) ; champs mono-barre supprimés ; DrawBar(int bar).
2. ✅ .cc : kRegions[3] ; ReadSlot(region,i) lit globals ; ActivateSlot(region,slot) (tab-switch skills /
   UseItemSlot items) ; UseItemSlot (dispatcher 0x71, SPÉCULATIF) ; ClearSlot/SetSlot/MoveSlot(region) ;
   WriteSlotRecord(region) écrit global ; OpenSlotDescription/ShowSlotTooltip(region) ; GetSlotKeyLabel(cat) ;
   DrawBar(int bar) (fenêtre ##SkillActionBar%d, payload SBSLOT={region,slot} -> move intra-région) ;
   DrawPanel (commun + 3 sections TreeNode par barre) ; HandleNativeDrop itère les 3 barres ;
   OnRenderUI boucle DrawBar(b) si bars_[b].visible.
3. ✅ moonlight_ui.cc : load/save bars_[0..2] clés skillbar{N}_{visible,x,y,columns,first,slots} ; clés
   communes skillbar_size/spacing/couleurs/flags conservées ; skillbar_columns/slots/x/y RETIRÉES.
## TEST 1 (2026-07-01) — compile+run OK. Retours utilisateur :
- ✅ Clics G OK sur Onglet 1 ET 2 (bascule d'onglet fonctionne). Clic D OK partout.
- ⚠️ Barre ITEMS : clic G ne fait rien (clic D=desc OK). → UseItemSlot ne consommait pas. FIX : dispatch
  0x71 INCONDITIONNEL (retiré le gate found/qty) + log found/qty. À re-tester (lire bourgeon.log ligne
  "[SkillBar] L-CLICK ITEM ..."). Si toujours KO -> RE le vrai chemin d'usage item-panel (le seul xref
  à g_ShortCutItemSlotExt 0x015faa4c = l'écriture OnMsg 0x17 ; pas de use direct trouvé -> peut-être
  cmd ≠ 0x71 ou getter/args différents ; piste : RE le double-clic inventaire "use item by nameid").
- ⚠️ HOTKEYS clavier Onglet 2 ne marchent pas. RE FAIT (DispatchHotkeyBehavior 0x00a451e0) : **tout
  behavior < 0x2d va au singleton this+0x1e8 via l'onglet ACTIF (option 10)** — PAS de dispatch par
  onglet. Les 2 onglets partagent les behaviors 0-44 ; "Hotkey Bar 2" = jeu de touches ALTERNATIF pour
  les MÊMES behaviors, seul l'onglet actif répond. ⇒ PAS de "lock" simple : impossible nativement
  d'avoir les 2 claviers actifs. Pour le faire : INTERCEPTER dans OnKeyDown les touches UNIQUES à
  l'onglet non-actif (via GetHotKey keycode out+0x00 + modif out+0x04) et router ActivateSlot(region).
  ⚠️ touches DUPLIQUÉES (F2-F9 partagées) = ambigu -> ne router QUE les touches uniques (sinon double-fire
  avec le natif). Proposé à l'utilisateur, pas encore implémenté.
- ✅ FIX texte touche trop long : police réduite selon largeur (font->CalcTextSizeA, ks *= maxW/w). OK confirmé.
- Drag-assign items depuis l'inventaire = WriteSlotRecord direct (global), PAS de persist serveur (TODO).

## TEST 2 (2026-07-01) — item use OK, onglet2 clavier OK. Améliorations :
- ✅ Item USE (redirect this+0xc4[0] -> OnMsg 0x29) FONCTIONNE. ✅ Interception clavier onglet 2 FONCTIONNE.
- ✅ KEYBIND amélioré (logique utilisateur, gère AUSSI les touches partagées F2-F9) : OnKeyDown -> si
  l'onglet ACTIF a un slot OCCUPÉ pour (vkey,modif) -> laisse le natif (priorité onglet1, couvre "2
  occupés") ; sinon si l'onglet non-actif visible a un slot OCCUPÉ -> ActivateSlot(other,slot). Pas de
  double-fire (le natif no-op si le slot actif est vide). CACHE anti-lag : g_keyCache[2] (mainK,modK,slot)
  reconstruit toutes les 1500ms (RebuildKeyCache via GetSlotKeyCodes = GetHotKey out+0x00 touche/+0x04
  modif). Plus de restriction "modificateur seulement".
- ✅ PERSISTANCE ITEMS = côté CLIENT (yaml), car RE (OnDrop_AssignSlot 0x008dd590) confirme que le natif
  ne persiste PAS les slots d'items (juste SetShortCutItemSlot + refresh, aucun paquet ; le serveur ne
  les renvoie pas). Impl : membre uint32_t item_slots_[9] ; SnapshotItemSlots() (lit g_ShortCutItemSlotExt,
  gardé par in_game_ pour ne pas écraser le yaml hors-jeu) appelé avant save ; yaml skillbar_item0..8 ;
  restauration 1×/session dans OnRenderUI (WriteSlotRecord région 2) quand native_hidden_ ; items_restored_
  reset au login. dirty_ posé sur drop/move/clear d'items. ⚠️ CLIENT-GLOBAL (pas per-perso) -> si multi-
  perso, barre d'items partagée. Per-perso = nécessiterait du serveur (comme les skills).

## TEST 3 (2026-07-01) — tout OK. Ajouts de ce tour :
- ✅ GRISER cases inutilisables : OBJET épuisé (GetItemLiveCount==0) / SKILL non appris. Test "connu" =
  réplique OnDraw natif branche skill : ItemMgr_GetInvItemById(0x00d7fa90)(out,id) -> out+0x04 (found)!=0
  = utilisable ; cleanup FUN_00739cd0 (=__fastcall(void*), réutilise StrFree_t) -> pas de fuite. Voile
  IM_COL32(10,10,15,165) par-dessus si !usable. (SkillKnown() dans le .cc.)
- ✅ SNAPPING : snap_grid_/grid_size_ (défaut 8px), au relâchement du glisser (IsItemDeactivated) arrondit
  bc.x/bc.y au multiple. Réglages dans le panneau + persist yaml skillbar_snap/skillbar_grid.
- ✅ INTÉGRÉ DANS MOONLIGHT_UI : DrawPanel scindé -> DrawSettingsContent() (public) ; onglet "Barre
  d'action" ajouté dans InterfaceSettingsTabs ENTRE "Portrait" et "Icônes du menu" (moonlight_ui.cc
  ~1837). La fenêtre standalone ²/~ (DrawPanel) reste dispo aussi (toutes deux appellent DrawSettingsContent).
- ✅ DRAG INTER-ONGLETS : payload SBSLOT porte {region,slot} ; MoveSlotCross(srcR,srcS,dstR,dstS) échange
  entre régions (Onglet1<->Onglet2, et vers/depuis items). Refuse skill->barre d'items (objets only).
  DrawBar : si move_region==region -> MoveSlot ; sinon MoveSlotCross. (ImGui drag-drop marche inter-fenêtres.)
- Tous non-testés en build (édités à l'aveugle) -> à builder/valider.

## TEST 4 (2026-07-01) — retours. Corrections :
- SNAP : l'option grille custom par-skill était un non-sens -> RETIRÉE (snap_grid_/grid_size_ supprimés
  partout). Les barres utilisent maintenant la **grille d'alignement PARTAGÉE** de MoonlightUi
  (`Bourgeon::Instance().moonlight_ui()->grid_.SnapAxis(coord, DisplaySize.axe)`, comme menu_icons /
  basic_info ; réglée dans Réglages interface : "Grille d'alignement" grid_.show + "Aimanter à la grille"
  grid_.snap + "Taille grille" grid_.size ; persist grid_show/grid_snap). Drag barre = MousePos-offset
  (bar_drag_off_x_/y_) -> SnapAxis. #include "plugins/moonlight_ui.h" ajouté dans skill_bar_tweaks.cc.
- PANNEAU DÉTACHÉ retiré : plus d'appel DrawPanel (fenêtre flottante) ni de toggle ²/~ ; les réglages
  vivent dans l'onglet MoonlightUi "Barre d'action" (DrawSettingsContent). DrawPanel/panel_visible_
  restent définis mais inutilisés (dead code inoffensif).
- Grey-out (#1) et drag inter-onglets (#4) validés OK par l'utilisateur.

## TEST 5 (2026-07-01) — taille & espacement PAR BARRE
- icon_size_/spacing_ (globaux) DÉPLACÉS dans BarCfg {..., float icon_size, float spacing} -> réglables
  par barre dans chaque section TreeNode du panneau. Persist par barre : skillbarN_size / skillbarN_spacing
  (clés globales skillbar_size/skillbar_spacing supprimées). DrawBar : locals `const float icon_size_ =
  bc.icon_size; spacing_ = bc.spacing;` (alias -> zéro autre changement dans DrawBar). HandleNativeDrop :
  step = bc.icon_size+bc.spacing par barre. Défauts 32/2 par barre.
- ✅ Contour NOIR autour des textes (4 directions, cOutline IM_COL32(0,0,0,230)) dans boldAdd/boldAddF
  -> lisibilité. Toujours actif.
- ✅ PLUS DE SLOTS pour la barre d'items : le natif g_ShortCutItemSlotExt ne fait que 9 (SetShortCutItemSlot
  boucle <9) -> écrire au-delà corromprait g_SkillInfoMgr. Comme la barre d'items est 100% client (pas de
  hotkey, persist yaml, use par redirect this+0xc4[0]), bascule sur un STORE PLUGIN : `uint8_t g_itemStore
  [kItemSlotMax*7]` (kItemSlotMax=36, dans le .h). kRegions[2].base = &g_itemStore (kRegions passé de
  constexpr -> const, base runtime). Tout le code base+i*7 marche pareil. ClearSlot/SetSlot items ->
  WriteSlotRecord (store), plus SetShortCutItemSlot (natif). item_slots_[9]->[36], Snapshot/Restore +
  yaml loops -> kItemSlotMax. "Nb slots" items va jusqu'à 36. ⚠️ le panneau natif d'items (caché) n'est
  plus alimenté -> si plugin désactivé, il est vide (re-populer nativement). SetItemSlot_t/DispUse_t/
  kUICmdDisp/kGetInvItemAddr désormais inutilisés (inoffensifs).

## FIX 3 (2026-08-03) — une case NON DESSINÉE ne répond plus à son raccourci
- Problème : les slots vivent dans les GLOBALS du client. Masquer une barre (ou baisser son "Nb slots")
  n'efface rien côté natif -> la touche du slot continuait de lancer skill/objet sans rien à l'écran.
- 🔴 Point de filtrage = **UIShortCutWnd::OnMsg case 0x29 (0x00901310)**, PAS la touche. Avaler la
  frappe dans ProcessPushButtonHook (qui, lui, PEUT consommer : `return true`) tuerait aussi le chat et
  les autres fenêtres pour cette touche. Filtrer 0x29 couvre les 2 onglets et tout chemin natif.
  Signature RE : `__thiscall`, **SIX args pile** (`retn 18h`, stack_frame arg_14) -> hook
  `int __fastcall(this, edx, arg0, msg, p2..p5)` ; case 41 lit `this+0xc4[p2 + 9*p3]` (p2=col, p3=row).
- Impl (skill_bar.cc) : `g_slot_drawn[3][36]` refait chaque frame par `SkillBar::RefreshDrawnSlots()`
  (même plage que DrawBar : visible + first_slot..+slot_count, clampé à kRegions[b].count) ;
  `g_slot_filter_on` = native_hidden_ && w (remis à false dans OnModeSwitch hors jeu) ;
  `g_self_activation` posé par le wrapper `ActivateSlot` autour de `ActivateSlotRaw` — indispensable :
  UseItemSlot détourne this+0xc4[0] et le slot 0 n'est pas forcément dessiné. Wrapper séparé car le
  SEH d'ActivateSlotRaw interdit tout objet RAII dans son corps.
- OnKeyDown : `occupiedSlot()` exige désormais AUSSI `g_slot_drawn` (sinon l'onglet actif « prenait »
  la touche pour une case invisible que le hook bloque -> rien ne partait). Le test `bars_[other].visible`
  devient redondant (masqué => aucun slot dessiné) et a été retiré.
- ⚠ Hypothèse partagée avec tout le module : rotation de barre à 0 (`ShortCut_ResolveRotatedSlotIndex`
  en case 0x17), donc index de tableau == slot logique. Les boutons de rotation natifs sont inatteignables
  (barre cachée), mais si un jour ils reviennent, ActivateSlot ET le filtre sont tous deux à revoir.
- ⛔ NON BUILDÉ/NON TESTÉ (l'utilisateur build lui-même).

## FIX 2 (2026-07-01, après log found=0) — à re-tester
- ITEM USE : le log `id=607 found=0` a prouvé que 0x00d7fa90 est le getter SKILL (pas objet). REFAIT :
  UseItemSlot **détourne this+0xc4[0] -> record d'item + appelle le vrai OnMsg 0x29 (col0,row0)** ->
  branche objet native complète (rec0==0, ne lit pas rec[5]). Restaure arr[0]. Gate #5. = fiable.
- HOTKEYS Onglet 2 : RE confirmé (ResolveBehavior 0x00a32c10 = Lua GetBehaviorOfHotKey2, fallback scanne
  QUE Tab0 ; DispatchHotkeyBehavior = onglet actif). Les 2 tables UserKeys.lua = alternatives par onglet,
  PAS 2 dispatch. => INTERCEPTION maison dans OnKeyDown : pour chaque barre d'onglet visible NON-active,
  GetSlotKeyCodes(cat,slot) (nouveau helper : GetHotKey out+0x00=touche, out+0x04=modif) ; si combo AVEC
  modificateur (Ctrl/Alt/Shift) et main==vkey et modif tenue -> ActivateSlot(region,slot). Gate perf :
  skip si aucun modificateur tenu. ⚠️ touches SIMPLES partagées (F2-F9) NON interceptées (double-usage) ->
  pour clavier onglet 2, rebinder ses slots en Ctrl+/Alt+. Pas de suppression du natif (pas possible depuis
  OnKeyDown). À valider en jeu. Risque : intercepte même si chat/textbox focus (pas de garde focus).
