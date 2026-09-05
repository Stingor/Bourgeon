# Boutique NPC (achat/vente) : RE et remplacement ImGui

> Journal du chantier. La fiche de mémoire `project_npc_shop_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

> `hide_fav_sell` : réglé à **yes** côté serveur (confirmé 2026-08-01) — plus rien à faire là-dessus.

Fenêtres d'interaction **NPC shop** (buy/sell) du client 20250716. Base image 0x400000 (pas de rebase). Tout renommé + commenté dans Ghidra. Vtables reconstruites via COL/RTTI lu en live (x32dbg).

**Slots vtable communs (famille UIWindow) :** +0x00 dtor · +0x3c OnCreate/InitControls · **+0x50 DrawContent** · **+0x94 OnMsg** (6 args : this,p1,msgId,p3,p4,p5). Le build appelle `(*this+0x94)(0,0x17,...)` pour rafraîchir.

## Les 3 fenêtres NPC + le cousin vending
| Classe | window id | vtable | COL | OnCreate | DrawContent | OnMsg |
|---|---|---|---|---|---|---|
| UIChooseSellBuyWnd (popup Acheter/Vendre) | **0x19** | 0x010335a4 | 0x010c3cec | 0x008a3170 | 0x008b3080 | 0x008be7b0 |
| UIItemPurchaseWnd (achat NPC) | **0x16** | 0x0103cda0 | 0x010c6170 | 0x0093aea0 | 0x009445f0 | 0x009517d0 |
| UIItemSellWnd (vente NPC) | **0x17** | 0x0103ce78 | 0x010c6184 | 0x0093c6f0 | 0x00945070 | 0x00952a20 |
| UIMerchantItemPurchaseWnd (VENDING joueur, PAS NPC) | — | 0x0103d2b0 | 0x010c6338 | 0x00940850 | 0x00947810 | 0x009566b0 |
RTTI type-descriptors : ChooseSellBuy 0x0123f038, ItemPurchase 0x0123e1f0, ItemSell 0x012406b0, MerchantPurchase 0x01240764. Window id **0x18** = probable UIItemSellWnd2 (compagnon fermé avec la vente). Debug BUY LIST dump = 0x00937840. Base OnMsg partagé = 0x00950780, relayout scroll = 0x00950400.

## Pipeline
1. Serveur ouvre **UIChooseSellBuyWnd** (id 0x19). Boutons : id **0xE0** "btn_buy"→cmd 0x26, id **0xDF** "btn_sell"→cmd 0x25, id **0xB9** "btn_cancel"→cmd 0x28. NPC id stocké this+0xB4 (via OnMsg 0x1C). Message NPC = UIRichTextBox this+0xB8.
2. cmd 0x26 ouvre l'achat (id 0x16), cmd 0x25 la vente (id 0x17), via **g_UICommandDispatcher** (dispatch `(*disp+0x18)(cmd,...)`, ptr @0x0121333c).
3. **Bus commandes shop (g_UICommandDispatcher) :** 0x22=exécuter achat (envoi panier), 0x23=exécuter vente, 0x25=ouvrir vente, 0x26=ouvrir achat, 0x28=annuler deal, 0xf9=achat cash(qty), 0x12=désélection.

## Struct UIItemPurchaseWnd (id 0x16) — la plus riche
+0x14 W · +0x18 H · +0xBC UIResizeButton · +0xC0 scrollbar · +0xC4 bouton Buy(id 0xB8) · +0xC8 bouton Cancel(id 0xB9) · +0xDC lignes visibles =(H-0x26)/32 · +0xE0 colonnes · +0xE8 **std::list items** (node) · +0xF0 label "Total" · +0xF4 record · +0xF8 index sélectionné (highlight) · +0xFC **mode** (0=zeny NPC / ≠0=cash/point) · +0x100 **UIEditCtrl quantité** (id 0x10F, max 9 chiffres) · +0x8C focus id.
**Node item** (liste +0xE8) : +0x00 next · +0x08 id/nom · +0x1C prix base · +0x20 prix2 (si ≠ → affiche " %d -> %s Zeny" remise, sinon " %s Zeny") · +0x34 string count · +0x90(short) nb slots carte → overlay icône. Ligne 0x20px, 1re ligne y=0x11, nom x=+0x2C, prix aligné droite W-0x12.
UIItemSellWnd (id 0x17) = même struct SANS le qty edit (+0x100). Marge basse -0x52 au lieu de -0x3a.

## OnMsg messages clés (achat & vente)
- msg 6 : clic bouton. **Achat** Buy(0xB8)→cmd 0x22 (mode0) ou validation+cmd 0xf9 (cash) ; **Vente** Sell(0xB8)→cmd 0x23 si panier non vide. Cancel(0xB9)→ferme + cmd 0x28.
- **msg 0xE (p3=3) : RESIZE** — clamp W[0xE8..0x140=232..320], H[0x78..0x150=120..336], snap grille 0x20 ; recalcule +0xDC.
- msg 0x17 : recalcule Total = Σ(qty×prix) sur panier (g_SkillInfoMgr) ; label "Total : %s Zeny/Koin/Point/Cash" selon mode & g_ServiceType.
- msg 0x22 : set record this+0xF4 + auto-resize. msg 0x24 : set mode this+0xFC.
- msg 0x26 : **AJOUT item au panier** (contexte sélection FUN_00a75340(0x1213338)) ; max stack 30000 ; achat check affordabilité vs **g_PlayerZeny (DAT_015fba90)** ; vente via FUN_00d552d0 + check FUN_00d71510.
- msg 0x7B : tag stream (achat 0x50DC/0x50DD, vente 0x510E/0x510F) → bascule buy/sell (MakeWindow 0x16).

## Globals / helpers utiles
- **g_PlayerZeny = DAT_015fba90** (NOUVEAU ; à côté de weight cur 0x015fbaa0 / max 0x015fba9c).
- g_ShopSellMirrorWnd_ptr = DAT_0131f738 (liste vente miroir, rafraîchie en msg 0x26).
- DAT_015ffcd4 = qty max achetable (cash), DAT_015ffcd0 = points cash gratuits.
- **g_SkillInfoMgr** = en fait le PANIER d'achat/vente (mal nommé) ; ops FUN_00d55xxx (FUN_00d55f80 cleanup, FUN_00d551b0 add-buy, FUN_00d552d0 add-sell, FUN_00d73220 sell-cart-non-vide).
- g_session = 0x15fa3c0. g_UICommandDispatcher @0x0121333c. Row renderer partagé (nom+slots) FUN_008711a0/FUN_00897860.
- Persistance positions (registre, struct settings) : ITEMSHOPWNDINFO +0x6fc, ITEMSELLWNDINFO +0x70c, ITEMPURCHASEWNDINFO +0x71c, CHOOSEWNDINFO +0x5f4 (loader FUN_00a34090).

## Couche PAQUET (vérifiée client 20250716 == serveur moonlight PACKETVER 20250716)
Wire format (moonlight packets_struct.hpp / clif.cpp, branche `>=20210203`) :
- **ZC 0x00c4** SELECT_DEALTYPE `{u16 op; u32 npcId}` (6o) → ouvre le shop.
- **CZ 0x00c5** ACK_SELECT_DEALTYPE `{u16 op; u32 GID; u8 type}` (7o) ; type 0=achat/≠0=vente. `npc_buysellsel` (npc.cpp:2387) SANS verrou → re-sélection libre ; la vente met `npc_shopid=0` (terminal, re-armer).
- **ZC 0x0b77** PURCHASE_ITEMLIST (var) sub `{u32 itemId; u32 price; u32 discountPrice; u8 itemType; u16 viewSprite; u32 location}` (19o).
- **CZ 0x00c8** PURCHASE_ITEMLIST (var) sub `{u16 amount; u32 itemId}` (6o) → npc_buylist.
- **ZC 0x00c7** SELL_ITEMLIST (var) sub `{u16 index; u32 price; u32 overcharge}` (10o).
- **CZ 0x00c9** SELL_ITEMLIST (var) sub `{u16 index; u16 amount}` (4o).
- **ZC 0x00ca/0x00cb** PURCHASE/SELL_RESULT `{u16 op; u8 result}` (3o). result 0=OK.
Le serveur valide tout (zeny/poids/stock) → aucun exploit possible, échec = code retour.

## Bus de commandes = CMode::SendMsg (PAS un objet séparé)
g_UICommandDispatcher = **[0x0121333c]** → obj → **vtable+0x18 = 0x00c86740 = CMode::SendMsg** (le dispatcher ré-entrant [[reference_processinput_sendmsg_hook]]). Switch géant (935Ko asm, non-décompilable en bloc). Commandes shop : 0x22 exéc-achat (build+send 0xc8 depuis panier), 0x23 exéc-vente (0xc9), 0x25 ouvre-vente (0xc5 type=1), 0x26 ouvre-achat (0xc5 type=0), 0x28 annule, 0xf9 achat cash.

## Cart manager = g_session 0x15fa3c0 (symbole Ghidra "g_SkillInfoMgr" = MISNOMER)
3 listes std::list : **+0x1700 panier ACHAT**, **+0x1710 panier VENTE**, +0x16f8 miroir. Node `{next,prev,ItemSkillInfo payload@+0x08}`. Payload : +0x04 index inv, +0x10 qty, +0x14/+0x18 prix, +0x2c nom (std::string), +0x88 slots. En offsets NŒUD (liste d'affichage window+0xe8) : +0x0c index, +0x18 qty, +0x1c/+0x20 prix, +0x34 nom, +0x90 slots. Fns : ShopCart_AddBuyItem 0x00d551b0, ShopCart_AddSellItem 0x00d552d0 (match par index), ShopCart_ResetAll 0x00d55f80, ShopCart_GetBuyTotalCost 0x00d5dd80 (Σ prix×qty), ShopCart_SellCartAllAffordable 0x00d73220. Résolution nom/icône par id : ItemSkillInfo_ctor 0x006a1b20 + SetId 0x006a6570 + GetBaseName 0x006a2b50 ; DB desc 0x01255130 (name=*(rec+4)).

## PLUGIN shop_tweaks — leçons de test live (2026-07-06)
- **Défaut OFF** obligatoire (opt-in) : setting `shop_imgui` défaut false. On n'impose pas le changement de gameplay.
- **TOUJOURS observer, JAMAIS `RegisterRecvOpcode`** : l'interception patche la dispatch-table en permanence (pas de dé-registration) → casse le shop natif quand le toggle est OFF. L'observe laisse le handler natif tourner (natif OK en OFF), on cache les fenêtres quand ON.
- **Fenêtre « ATK 70-70 DEF 0-0 » restante = UIItemParamChangeDisplayWnd** (comparateur stats équipement), **vtable 0x010323ec** (COL 0x010c35b4). Créée par le handler d'achat natif, **id de fenêtre VARIABLE** → masquée **par vtable** dans le hook MakeWindow (pas par id). RTTI typedesc 0x0123f594.
- **Fermeture (X ET Échap) : perso BLOQUÉ = état dialogue CLIENT-side.** `CZ_CLOSE_DIALOG 0x0146` NE SUFFIT PAS (serveur gate sur sd->npc_id, souvent non posé pour un shop ; et ça ne touche pas le client). **Fix = répliquer le cancel natif du chooser : dispatch cmd 0x28 sur CMode::SendMsg** (`disp=[0x0121333c]; (*(*disp+0x18))(disp,0x28,0,0,0,0)` __thiscall) → réinit état dialogue client (débloque) + notif serveur. Puis ShopCart_ResetAll(0x00d55f80, session 0x15fa3c0) + CZ_CLOSE_DIALOG filet + CloseWnd. Le bouton Annuler natif (chooser btn 0xb9) fait exactement cmd 0x28 + close + cart-reset.
- Fenêtres non-dédiées (shop) vivent dans la **std::map du UIWindowMgr @mgr+8** (clé=window id, node MSVC {L,P,R,color/isnil,key@+0x10,val@+0x14}). Piège : la map MUTE en live (rééquilibrage RB-tree) → lectures incohérentes.
- **NE JAMAIS `pause_process` sur le jeu live → le réseau timeout → DÉCONNEXION du joueur.** Les read_memory marchent en mode RUNNING (pas besoin de pause). memory_search x32dbg est CASSÉ sur le tas (retourne 0xffffffff) ; marche sur la .rdata statique du module.
- **Liste de VENTE : PAS dans le conteneur UIItemSellWnd (id 0x17, vtable 0x0103ce78)** mais dans un **sous-window liste (vtable 0x0103cbf0)** pointé par le global **g_ShopSellMirrorWnd_ptr = DAT_0131f738**. std::list @+0xe8. Node confirmé live (jellopy) : +0x0c=index inv (22), +0x18=qté (1), +0x1c/+0x20=prix vente (3), **+0x34=std::string = itemId EN TEXTE ("909")** → atoi → resolver nom/icône par id, +0x90=slots (0). Le champ +0x34 n'est PAS le nom localisé.
- **Requête de VENTE : dispatcher cmd 0x25 (bus CMode::SendMsg, npcId en arg) pas le 0xc5(1) brut** — pose l'état "mode vente" client + crée la fenêtre native (+ set DAT_0131f738) qu'on lit. (Achat : 0x0b77 parsé direct, 0xc5(0) brut suffit.)
- **Liste de vente = SNAPSHOT** : après un achat l'inventaire change mais la liste native reste figée → RE-demander (cmd 0x25) à chaque bascule sur l'onglet Vendre (sell_requested_=false). Après-vente déjà couvert (0xcb re-arme).
- **Nom du NPC (titre)** : observer **ZC_ACK_REQNAMEALL_NPC 0x0adf** (PACKETVER 20250716) `{gid:4, groupId:4, name[24]@+8, title[24]}` → cache GID→nom. Résolveur natif alternatif : **ActorList_FindByGID 0x00a69eb0** (actor+0x110=GID) / wrapper **FUN_00ad6a10(gid)** ; actorMgr = `*(gameMode+0xcc)`, gameMode = `*(0x0121333c)` (change à chaque instanciation), player actor = `*(actorMgr+0x2c)`. session/gameMode getter = FUN_00a75340(0x1213338) renvoie `*(0x1213338+4)` si `*(+0x58)==1`.

## 🔴 CARTE DES FENÊTRES — REFAITE LIVE (2026-08-01, commit 82f2228)
Les 4 étiquettes précédentes étaient FAUSSES, décalées d'un cran. Chaque ligne a DEUX preuves : le ctor appelé par le case de `UIWindowMgr_MakeWindow`, ET une marche de la std::map du window-mgr sur boutique native ouverte, dans les DEUX onglets.

| id | classe | ctor | vtable | vu à l'écran |
|---|---|---|---|---|
| **0x16** | `UIItemShopWnd` (le CADRE) | 0x00934850 | 0x0103cbf0 | achat ET vente |
| **0x17** | `UIItemPurchaseWnd` | 0x00934630 | 0x0103cda0 | achat seulement |
| **0x18** | `UIItemSellWnd` | 0x00934730 | 0x0103ce78 | **vente seulement** |
| **0x19** | `UIChooseSellBuyWnd` | 0x0088cd60 | 0x010335a4 | toujours |
| **0x32** | `UIItemParamChangeDisplayWnd` (comparateur ATK/DEF) | 0x0088dea0 | 0x010323ec | achat + ÉQUIPEMENT |

Le client échange le **panneau intérieur** selon l'onglet (0x17 achat / 0x18 vente) et garde le cadre 0x16. **Celui qui n'est pas affiché reste VIVANT** → fermer les deux, sinon native masquée = clavier confisqué.
⚠ Le comparateur 0x32 n'était dans AUCUNE liste de fermeture (masqué seulement) — corrigé : masqué à la création (destruction impossible depuis MakeWindow), DÉTRUIT au tick. Son seul créateur est le panneau d'achat natif ; `UIItemSkillDescWnd_OnMsg` ne le crée PAS (aucun `push 32h`), donc il ne peut plus apparaître depuis que 0x17 ne naît plus.
⚠ `UIExchangeAcceptWnd` (popup de demande d'échange) DÉRIVE de `UIChooseSellBuyWnd` — d'où l'aspect identique.
**Leçon de méthode** : le 1er relevé, fait en onglet ACHAT seulement, concluait que 0x18 n'existait pas et j'allais retirer sa purge. C'est l'utilisateur qui a demandé de vérifier l'onglet VENTE. Une seule observation ne fait pas une preuve — cf. [[feedback_re_method]].

## PLUGIN shop_tweaks (FAIT, compile ; à déployer+tester)
`src/plugins/shop_tweaks.{h,cc}`, registré LoadPlugins + accesseur bourgeon.h + MakeWindow-hook window_pos (ids 0x16/0x17/0x19 → HideNativeAtCreation) + CMakeLists. Modelé sur CashShopTweaks.
- **Remplace le natif** : cache chooser(0x19)/achat(0x16)/vente(0x17), fenêtre ImGui unifiée 2 onglets (Acheter|Vendre), skip du chooser (atterrit direct sur Achat).
- ACHAT = packet-driven : observe 0x0b77, parse itemId/prix, nom/icône par id, panier, total, solvabilité vs g_PlayerZeny 0x015fba90, envoi CZ 0x0c8.
- VENTE = lecture de la fenêtre native cachée (0x17 +0xe8, résolution native réutilisée), panier, envoi CZ 0x0c9.
- Toggle → OnTick envoie CZ 0x0c5(type). Tous les SendPacket depuis OnTick/OnRenderUI (thread principal), jamais depuis OnRecvPacket.
- **À VÉRIFIER en live** (1re ouverture d'un shop NPC) : (1) client reçoit bien 0x0b77 ; (2) offsets node vente RefreshSellFromNative ; (3) flux auto-skip (chooser caché + requête). Setting persistant `shop_imgui` à brancher dans MoonlightUi (défaut ON).

## REPRISE / ÉTAT (2026-07-07) — pour nouvelle session
**COMMITTÉ** : plugin shop_tweaks complet (achat packet-driven + vente lecture native + auto-skip + toggle + fermeture cmd 0x28 + zeny « z » + clamp quantité + tout-vendre + titre nom-NPC best-effort). Skin cashshop (RoButton/RoCheckbox) + **nouveau widget toolkit `ro::RoBeginCombo`/`RoEndCombo`** (combo box RO : champ input+bordure, flèche texture native `txtbox_btn_a/b/c.bmp` via EnsureTexClient, popup liste fond corps). Setting `shop_imgui` défaut OFF (opt-in) dans MoonlightUi.
**COMMITTÉ (2026-07-07, a3b6698, testé « Test niquel »)** : shop_tweaks (NPC) passé au **skin RO complet** comme le cashshop — `ro::BeginRoWindow`/`EndRoWindow` (5 style vars, toutes les branches de sortie gérées), tous les boutons en `ro::RoButton` (Acheter/Vendre/Vider/Tout ajouter/« + » des tables), filtre en `InputTextWithHint("Filtrer...")` (pilote `filter.InputBuf`/`Build`), panier `[-][qté][+]` en petits RoButton carrés (`step=GetFrameHeight()`, InputInt `step=0`) + « x » idem aligné à droite, prix en noir (achat : noir si abordable / rouge sombre sinon ; vente : noir). Aligné visuellement sur le cashshop. À builder+tester.
**⚠ CRITIQUE — npc_shopid effacé après CHAQUE transaction (RE live 2026-07-07, FIX COMMITTÉ 0cd3699, testé OK)** : `clif_parse_NpcBuyListSend` (clif.cpp:**13906**) ET `clif_parse_NpcSellListSend` (clif.cpp:**13941**) font `sd->npc_shopid = 0` après chaque 0xc8/0xc9 (le shop natif se ferme après un achat/vente ; le client natif re-sélectionne le deal à chaque fois). En-tête achat (clif.cpp:**13887**) : `if (sd->state.trading || !sd->npc_shopid) result = PURCHASE_FAIL_MONEY;` → **sans re-arm, le 2e achat/vente trouve npc_shopid=0 → échoue `PURCHASE_FAIL_MONEY` (result 1, message TROMPEUR « pas assez de zeny »)**, même pour 1 unité, même avec 1 milliard de zeny (le zeny n'est jamais vérifié dans ce cas). Symptôme observé : 1er achat OK, tous les suivants échouent. **FIX viewer** = `SendDealSelect(type)` (envoie 0xc5) **juste AVANT** chaque 0xc8/0xc9 (l'ordre TCP garantit : arme npc_shopid puis transige). Câblé : QuickBuy/SendBuy → type 0, QuickSell/SendSell → type 1, + RequestList(kBuy) refactorisé dessus. NB : ceci CORRIGE aussi la vieille note « la vente met npc_shopid=0 (terminal) » — c'est en fait TOUTE transaction (achat compris) qui l'efface, côté clif (pas npc_buysellsel).

**⚠ Fenêtre orpheline au changement de map — FIX FAIT (2026-07-07, à build/test)** : si le joueur ouvre un shop puis change de map (**`@load`**, warp, tp), le warp **invalide la session shop côté serveur** (`pc.cpp:9873/9889` → `npc_shopid=0` + `npc_id=0`) MAIS notre viewer restait ouvert (bug : `open_` n'est jamais remis à false quand les fenêtres natives disparaissent, seulement au clic X). Fenêtre orpheline → achat impossible (`Echec 1` = PURCHASE_FAIL_MONEY, cf. note npc_shopid ci-dessus). C'était la vraie cause du cas « Forge Shop / Shop 2/8/4 » (fenêtres orphelines empilées après plusieurs @load). **Fix** : observer **ZC_NPCACK_MAPMOVE `0x0091`** (`clif_changemap`, @load/warp same-server) + **ZC_NPCACK_SERVERMOVE `0x0092`** → flag `map_changed_` (thread recv) → OnTick (thread principal) ferme le viewer : `CloseWnd(0x16/0x17/0x18/0x19)` (détruit les natives orphelines sinon elles réapparaissent quand on cesse de les cacher) + reset `open_`/`npc_id_`/listes/panier/flags. **PAS de cmd 0x28** (le warp a déjà reset le dialogue). NB : on n'utilise PAS « open_=false quand any_native disparaît » car le re-parse 0x0b77 (re-arm) peut détruire/recréer la fenêtre native → faux-close ; le paquet map-change est le signal fiable.

**TODO restants** (par priorité) :
1. **Nom NPC 100% fiable** : le cache paquet 0x0adf RATE les NPC dont le nom vient du **paquet de spawn** (→ repli « Shop »). Fix robuste = lire depuis l'ACTEUR au rendu : `FUN_00ad6a10(npc_id_)` (0x00ad6a10) → acteur → nom à offset X **NON TROUVÉ** (l'acteur JOUEUR n'a pas le nom ; inspecter un acteur NPC live pour l'offset). Alternative : observer le paquet de spawn (ZC_NOTIFY_STANDENTRY) qui porte le nom inline. actorMgr=`*(gameMode+0xcc)`, gameMode=`*(0x0121333c)` (change à chaque instanciation).
2. **RoComboBox** : valider le mapping états flèche a/b/c ; option = utiliser la texture `txtbox_%s.bmp` pour le fond du champ (variante %s exacte inconnue, seul `txtbox_btn_a` défini en dur).
3. **Discount / Overcharge — affichage FAIT (2026-07-07)** : helper `draw_price(base,final,col)` dans OnRenderUI (base grisé + « -> » + final coloré, SEULEMENT si base≠final → 0 encombrement non-marchand) ; achat `draw_price(b.price,b.discount,...)`, vente `draw_price(s.base_price,s.price,...)` (nouveau champ `SellItem.base_price` = nœud +0x1c). Colonnes prix élargies 80→110. Serveur autoritaire (on n'envoie que {itemId/index,qté}). **À VÉRIFIER live** : `b.discount < b.price` pour un Merchant (sinon moonlight ne renseigne pas discountPrice → fix serveur `clif_buylist`). Sources : discount = champ du 0x0b77 (pc_modifybuyvalue) ; overcharge = nœud natif +0x20 (pc_modifysellvalue).
4. **Clic-droit description — FAIT + testé OK (2026-07-07, porté du cashshop)** : `OpenItemDesc(id,view,loc,mx,my)` (MakeWindow 0xc + OnMsg 0x18 + ItemSkillInfo standalone, constantes/`Vf` ajoutées) déclenché par le helper `rclick_desc(id,view,loc)` (`IsItemHovered()&&IsMouseClicked(Right)`) appelé **après l'icône ET après le nom** (achat : view/loc réels ; vente : 0/0 = pas d'aperçu). Extension icône ajoutée après retour user (re-build à faire).
5-6. **Boutons quantité +1/+10/+100/+1k + Ctrl-clic immédiat + grisage — FAIT + COMMITTÉ + testé OK (2026-07-07, shop 0cd3699 / toolkit RoButton-disabled 17c120d)** : layout retenu = **4 boutons par ligne** (colonne action 44→150, fenêtre 560→720). Helper lambda `qty_buttons(id,index,unit_price,max_avail,is_buy)` dans OnRenderUI. **Clic** = `AddToCart` (étendu avec param `qty`) ; **Ctrl+clic** (`GetIO().KeyCtrl`) = transaction immédiate `QuickBuy(id,qty)`/`QuickSell(index,qty)` (0xc8/0xc9 à 1 item, serveur calcule/valide). **Grisage** via `BeginDisabled` **UNIQUEMENT en mode Ctrl (immédiat)** : achat grisé si `qty*prix>zeny`, vente grisée si `qty>possédé` ; sans Ctrl (ajout panier) jamais grisé — on empile librement, c'est le bouton Acheter/Vendre qui gère la solvabilité du total (corrigé après retour user : le grisage permanent bloquait à tort l'ajout-panier de +100 quand on avait « assez » pour l'item mais pas 100×). Hint « Clic = panier / Ctrl+clic = immédiat ». **RoButton rendu conscient du disabled** (toolkit ro_imgui) : il dessine main → ignore l'alpha ImGui, donc on lit `GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled` (public imgui.h `1<<6`) → art estompé (tint alpha 90) + texte `TextDisabled`. Bonus : Acheter/Vendre/etc. désactivés se grisent aussi maintenant.
7. **Fermeture auto après « Tout ajouter au panier » — FAIT (2026-07-07)** (FONCTIONNALITÉ voulue, PAS un bug — corrigé par l'user) : cliquer « Tout ajouter au panier » (vente) arme `sell_all_close_` ; à la vente réussie (`0xcb` result 0, thread recv) on pose `want_close_` ; OnTick (thread principal) le consomme → `CloseNativeShop()` + `open_=false` (comme un clic X). Désarmé sur open 0x0c4 / changement d'onglet / « Vider » / CloseNativeShop. Tooltip sur le bouton. Dump-tout-et-quitte.
8. Finitions shop (icônes vente OK ; ~~aperçu au survol~~ ✅ FAIT le 2026-08-08, cf. §bas).
**RÈGLE** : NE PAS builder soi-même ([[feedback_build_and_git]]) — l'utilisateur build (économie contexte/tokens). Ne pas pauser le jeu (déco). Lectures mémoire x32dbg OK en running ; re-attacher via `attach <PID hex>` + `run_process` immédiat.
Amorçage session : « reprends shop_tweaks + toolkit ro_imgui, lis project_npc_shop_re (section REPRISE) ».

## 2026-08-01 — natif TUÉ + la vraie cause du « perso bloqué » (corrige le §70)
Les 3 paquets d'ouverture (0x00c4, 0x0b77, 0x00c7) sont **remplacés** (RegisterReplaceOpcode) : les 4 fenêtres natives (0x16/0x17/0x18/0x19) ne NAISSENT plus. Plus de cmd 0x25 pour lire la liste de vente (elle se parse depuis 0x00c7 + modèle session inventaire), plus de cmd 0x28 pour fermer.

🔴 **CGameMode+0x2DC ne doit PAS être écrit par la boutique.** Le handler natif de ZC_SELECT_DEALTYPE (**0x00CA0F02**) fait seulement `[edi+24Ch]=1`, `MakeWindow(0x19)`, puis `chooser->OnMsg(0x1C, npcId)` → le npcId va dans **fenêtre+0xB4**, jamais dans +0x2DC. Contrôle à l'échelle du dispatcher : des **6** sites qui posent +0x24C=1 dans RecvLoop_DispatchPackets, seuls **4** écrivent aussi +0x2DC — les paquets de DIALOGUE.

+0x2DC = GID de la **conversation** = `sd->npc_id` serveur. Une boutique EST une interaction NPC (elle vient d'un script, souvent au nom d'un AUTRE NPC que le marchand). En y écrivant le GID de la boutique, on effaçait l'identité du script : `CZ_CLOSE_DIALOG` partait au mauvais NPC, `npc_scriptcont` le rejetait, **joueur bloqué** — au clic sur la croix comme au retrait à chaud de l'interface moderne. « Tout ajouter au panier » y échappait par accident : c'est la VENTE qui fait avancer le script, pas la fermeture.
**Fix** : `SetNpcInteractionActive(0)` (ne pas toucher +0x2DC) et fermeture sur les **deux** GID quand ils diffèrent (conversation d'abord, puis boutique — le 2e est un no-op serveur si npc_id est déjà nul).

🔴🔴 **Fermer une boutique = DEUX paquets.** `CZ_NPC_TRADE_QUIT` **0x09D4** (2 octets, l'opcode seul, `parseable_packet(0x09D4,2,clif_parse_NPCShopClosed,0)`) est celui qui **DÉBLOQUE le personnage** : il fait `sd->npc_shopid = 0`, et **`pc_cant_act2()` teste `|| sd->npc_shopid`** (pc.hpp) → `clif_parse_WalkToXY` refuse TOUT déplacement tant qu'il est posé. `CZ_CLOSE_DIALOG 0x0146` n'y touche jamais — il ne termine que le SCRIPT (sd->npc_id). Côté client ce sont deux sélecteurs distincts de CMode::SendMsg : **291 @0x00C875E7 -> 0x09D4** et 0x59 @0x00C8932C -> 0x0146. C'était la vraie cause du « perso bloqué » après la croix / le bouton Fermer / la bascule à chaud. Indice qu'on avait sous les yeux : « Tout ajouter au panier » fermait proprement parce que `clif_parse_NpcSellListSend` remet lui aussi `npc_shopid = 0` (clif.cpp ~14693) — comme `clif_parse_NpcBuyListSend` (~14658) et le warp (pc.cpp ~9899).

⚠ **Le verrou « ne pas vendre les favoris » vivait DANS le handler remplacé.** `NpcSell_BuildSellableList` **0x00CD0F00** (constructeur de la liste de vente) filtre `if (trouvé && (!favori || !g_inv_dealLock))` — favori = out+0x74 de `Session_GetEquipInfoByInvIndex`, `g_inv_dealLock` = octet client **0x01600553** (bouton « Deal » du pied de l'inventaire, 100 % client, le serveur l'ignore). En remplaçant 0x00c7 on a emporté le filtre : le verrou basculait toujours, plus personne ne le lisait, les favoris étaient vendables. Repris dans ResolveSellItems (+ relecture du verrou à chaque tick, et purge du panier de vente des entrées qui sortent de la liste). **Leçon générale : un handler remplacé peut porter des GARDE-FOUS purement client, pas seulement de l'affichage — les chercher avant de le remplacer.**

Cancel natif RE'd au passage — `CMode::SendMsg` **case 0x28 @0x00C8760F** : `+0x24C=0` ; si la fenêtre menu (0x11) existe → lui envoie cmd 0xA6 (ESC) ; si la say-dialog (0x10) existe → **selecteur 0x59 @0x00C8932C** = CZ_CLOSE_DIALOG {GID} ; puis détruit ~16 fenêtres. Il n'envoie AUCUN paquet quand aucune fenêtre de dialogue n'est vivante.
UI : pied de fenêtre avec bouton **Fermer** (les deux onglets), qui passe par `want_close_` → OnTick, hors frame ImGui.

## 2026-08-07 — quantité : un ÉQUIPEMENT s'achète à l'unité (défaut corrigé)
Le viewer ImGui laissait mettre 5 armes au panier → CZ 0xc8 `amount=5` → serveur `npc_buylist`
(npc.cpp) : `if (!itemdb_isstackable2(id) && amount > 1)` → **amount ramené à 1** + log
`« sent a hexed packet trying to buy 5 of nonstackable item 1247 »`. Le joueur payait une arme
en croyant en commander cinq — aucun message côté client.

- **Règle serveur** = `item_data::isStackable()` : faux pour **4 ARMOR · 5 WEAPON · 7 PETEGG ·
  8 PETARMOR · 12 SHADOWGEAR**. L'octet reçu passe par `itemtype()` (clif.cpp:111) qui replie
  PETEGG→ARMOR et SHADOWGEAR→WEAPON/ARMOR : 7 et 12 n'arrivent jamais par le fil.
- **Le natif le faisait**, autrement : `UIItemPurchaseWnd_OnMsg` **0x00951BD0 case 38** teste le
  TYPE (GameMode+0x320 = champ +0x18 de l'ItemSkillInfo sélectionné en GameMode+0x308) et, pour
  {4,5,8,9,11,12,13,14,15}, **refuse le doublon** si `ShopCart_FindBuyAmount` (0x00D5DCF0) le
  trouve déjà au panier → chat **MsgString 0x77 = MSI_EQUIPITEM_OLNY_ONE** « Please avoid buying
  2 of the same items at one time. ». Sa liste est plus large que celle du serveur (11
  DELAYCONSUME s'empile pourtant) → on suit le SERVEUR.
- **Fix Bourgeon** : `BuyStackMax(type)` = 30000 / 1, la borne porte le panier ET taille les
  boutons (« +1 » seul), + message natif au clic bloqué. Règle mutualisée
  `itemcell::TypeIsStackable` (features/item_cell.h), partagée avec [[reference_trade_window_re]].
- Le **cash shop est SAIN** : `cashshop_buylist` (cashshop.cpp:596) livre `quantity` lots de
  `get_amt=1` — payer 5 armes en donne bien 5. Ne pas y appliquer la même borne.
- Non retenu (possible si voulu) : envoyer **N entrées de 1** pour le même id — `npc_buylist` ne
  rejette pas les doublons, ça permettrait 5 armes en un clic. Le natif ne le fait pas.

## 2026-08-08 — aperçu au survol ✅ (TODO 8 soldé)
Survol de l'icône OU du nom → `itemcell::DrawTooltip`, dans les deux onglets. Le
survol est MÉMORISÉ dans la boucle et peint **après `ro::EndRoWindow` + le
`PopStyleVar`** : un tooltip crée son propre popup, né dans la table il serait
clippé dedans (et il hériterait des arrondis de la boutique).

🔴 **Les deux moitiés ne montrent pas la même chose, et c'est le fond du sujet.**
ACHAT = un id seul, l'objet n'est encore l'exemplaire de personne → description de
base (`name=nullptr`, repli DB). VENTE = l'objet est DÉJÀ dans le sac, donc
`ResolveSellItems` lit désormais ses données d'INSTANCE dans l'ItemSkillInfo et
`SellItem` les porte : cartes `+0x1c` (×4) avec le critère forgé `card[0] != 0 &&
<= 500`, raffinage `+0x60`, cassé `+0x5d`, options `+0x98` (nb) / `+0x9c`
(entrées de 5 o). Vendre une pièce +7 sertie ne se rattrape pas : l'aperçu doit
décrire ce qui PART, pas l'item de base qui porte le même nom.
Puisque le flag « cassé » est lu, la ligne de vente passe par `itemcell::NameText`
(ombre rouge native) au lieu d'un `ImGui::Text`.
Pied de fenêtre : « Survol : aperçu - Clic droit : description » (les 2 traductions
suivent la clé, cf. [[project_i18n_language_setting]]).

## 2026-09-05 — `callshop <shop>,1|2` : la liste arrive SANS 0x00c4 (boutique qui ne s'ouvrait pas)

Signalé en jeu sur le marchand de rations de `cmd_fild07` :

```
-	shop	inf_ration	-1,512:-1,513:-1,515:-1,516:-1
cmd_fild07,63,268,1	script	Emergency Food Merchant#pa0829_01	4_M_BIBI,{
	mes "..."; close2; callshop "inf_ration",1; end;
}
```

**Ce que fait le serveur** (`BUILDIN_FUNC(callshop)`, script.cpp) : avec un flag,
il n'appelle PAS `clif_npcbuysell` mais **`npc_buysellsel(sd, nd->id, 0|1)`**
(npc.cpp), qui pose `sd->npc_shopid` et envoie **directement** `clif_buylist` /
`clif_selllist`. Donc **ZC_SELECT_DEALTYPE 0x00c4 n'est jamais émis** — et c'était
notre seul déclencheur d'ouverture. Comme on prend AUSSI la place du handler natif
de 0x0b77, la fenêtre native ne naissait pas davantage : rien à l'écran, et le
joueur **bloqué** (`pc_cant_act2()` lit `sd->npc_shopid`, que seul
`CZ_NPC_TRADE_QUIT 0x09D4` remet à zéro — personne n'était là pour l'envoyer).

Le motif est fréquent dans les scripts officiels (marchands à choix multiples,
Kafra, tool dealers) : ce n'était pas un cas tordu.

**Correctif** — `BeginSession(npc_id, mode)` factorise l'ouverture, et les DEUX
listes l'appellent quand aucune session n'est ouverte, avec `npc_id = 0` :
- ouverture décidée **avant le décodage** : une liste vide doit ouvrir la fenêtre
  aussi, c'est sa croix qui débloquera le personnage ;
- `direct_list_` marque la session sans GID. 🔴 **Pas de GID = pas de
  re-sélection** : `CZ_ACK_SELECT_DEALTYPE` NOMME le marchand, et le seul GID à
  portée (la CONVERSATION, `CGameMode+0x2DC`) est celui du NPC scripté, pas du shop
  flottant — `npc_buysellsel` le refuserait (`no such shop npc`) et effacerait
  `sd->npc_id` au passage. Ne pas essayer ;
- d'où **un seul onglet** (l'autre liste n'a personne à qui être demandée) et
  **fermeture après la transaction** : `sd->npc_shopid = 0` est hors du `if` dans
  `clif_parse_NpcBuy/SellListSend`, la session meurt succès OU échec. Le natif
  ferme la sienne au même moment ;
- titre : à défaut de GID marchand, on nomme le NPC de la conversation — c'est
  celui que le joueur a devant lui.

À jouer pour valider : ouverture, achat, déblocage après fermeture par la croix.

## Leviers de customisation (voir aussi [[project_item_skill_desc_window_re]], [[project_inventory_window]], [[project_storage_window_re]], [[reference_cashshop_re]])
- **Overlay ImGui "coût/gain total" & "reste après achat"** : miroir de inventory_tweaks — poll g_PlayerZeny + somme panier (msg 0x17 déjà calcule le total). 0 patch natif.
- **Boutons quantité rapides (x1/x10/x100/MAX)** : écrire dans l'edit this+0x100 puis envoyer msg 6 / relayout. MAX = min(stackable, zeny/prix).
- **"Acheter tout le panier en une fois"** déjà natif (cmd 0x22) ; ajouter "vendre tout l'inventaire vendable" = itérer inventaire + FUN_00d552d0 puis cmd 0x23.
- **Colorer les prix** (rouge si > zeny) dans DrawContent (hook +0x50) ; **élargir la fenêtre** au-delà de 320 en patchant les clamps 0x140 dans OnMsg msg 0xE (0x009517d0 / 0x00952a20).
- **Afficher prix unitaire vs prix de revente** (marge) via l'overlay.
