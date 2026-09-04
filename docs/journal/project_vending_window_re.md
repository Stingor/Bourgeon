# Échoppe joueur (vending / buying store) — VendingTweaks

> Journal du chantier. La fiche de mémoire `project_vending_window_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

## 🔴 Campagne « tuer le natif » : cette famille est À PART (analyse 02/08)

**NE PAS tuer la composition (0x29/0xAE) ni « Mon shop » (0x2D/0xB0).** Contrairement
aux 12 autres natives tuées, ce plugin **PILOTE** sa native au lieu de la remplacer :
il écrit dans ses UIEdit puis déclenche `OnMsg(msg 6, cmd 184)` = le clic OK natif.
La native est le MOTEUR, pas la coquille. La tuer imposerait de réimplémenter la
validation, **la taxe** (convention jamais vérifiée = la limite v1 assumée), le refus
muet sur dépassement de zeny, le mode placement, et l'import par **service web**.
Et `0x101`/`0x102` est un **ACCUMULATEUR** sans liste de session : le détruire efface
l'historique des ventes en silence.

✅ **Le motif de la campagne ne s'applique pas ici** :
- pas de bascule à avaler — ces fenêtres naissent de la compétence ou d'un clic sur
  l'échoppe d'autrui, jamais d'un raccourci ;
- **pas de vol de clavier** : recherche exhaustive de toute écriture de `+0x90`
  (« bouton par défaut ») sur **0x00930000-0x00970000** → **AUCUNE**. Ces classes n'en
  déclarent pas, donc Entrée n'a rien à déclencher sur une native masquée. (C'est ce
  champ que le sertissage posait à `0xB8`, d'où SON problème.) Conforme au vécu : le
  plugin tourne depuis le 31/07 sans incident.

## ✅ Côté ACHETEUR (0x2B/0x2C) — NATIF MORT (02/08, commit 03133f9)

**Listes de la SESSION** (décodées sur les getters `VendingXxx_GetCount`
0x00d5ce40..0x00d5ce90, qui rendent tous `*(session+compteur)` ; std::list MSVC
`{tête, taille}` ⇒ **tête = compteur − 4**) :

| tête | compteur | liste |
|---|---|---|
| 0x1720 | 0x1724 | (getter 0x00d5ce50, non nommé) |
| **0x1728** | 0x172C | **offre du vendeur** (`VendingOffer_*`) |
| 0x1730 | 0x1734 | disponibles (`VendingAvail_*`) |
| 0x1738 | 0x173C | « Mon shop » (`VendingMyShop_*`) |
| 0x1740 | 0x1744 | panier (`VendingBasket_*`) |
| 0x1748 | 0x174C | objets posés (`VendingShop_*`) |

⚠ La fiche disait « disponibles = session+0x172C » : c'est le **compteur de
l'OFFRE**. Les getters font foi.
✅ **Le contenu de la session est identique à celui de la fenêtre, prix EFFECTIF
compris** : `VendingOffer_GetAt` ne fait que `ItemSkillInfo_CopyFull(nœud+8)` —
aucune remise calculée au moment de remplir la fenêtre. Vérifié AVANT de changer
de source, parce qu'un prix de base pris pour l'effectif donnerait des totaux faux.

Le reste : `RegisterObserveOpcode(0x0b3d)` pour l'AID (+2) et le venderId (+6) —
OBSERVÉ, le handler natif devant continuer de remplir la liste de session ; état
de session porté par nous (ouvert par le paquet, fermé par nous) ; destruction des
deux natives au tick.

🔴 **Fermeture** : la cmd 185 ne se contentait PAS de fermer (relevé à
l'instruction près) — après ses 3 `SaveRectAndCloseWindow` elle fait
`CMode::SendMsg(0x28)` (fin d'interaction marchande) **puis**
`VendingBasket_Clear 0x00d56300` `__thiscall(&session)`. Une destruction directe
ne passe pas par `OnMsg` ⇒ ces deux gestes sont rejoués explicitement
(`EndVendorDeal`). Même schéma que le `CZ 0x0A03` de la rédaction de courrier.

---

## Côté ACHETEUR — notes de RE (antérieures)

Seul morceau réellement tuable : là, le plugin **émet déjà `CZ 0x0801` lui-même**.
- 🔴 **Le paquet de liste est `ZC 0x0b3d`**, PAS 0x0800 : le client ne dispatche que
  `case 2877` (@0x00ca9d6b), il n'a **aucune** entrée pour 0x800. Layout serveur
  (`packets_struct.hpp`, branche `>= 20200916/20200723`) :
  `{packetType:2, packetLength:2, AID:4, venderId:4, items[]}` ⇒ en régime OBSERVÉ
  (`data` = paquet+2) : len@0, **AID@2**, **venderId@6**. C'est la source des
  `0x2C+0xF8` / `+0x104` que le plugin lit aujourd'hui dans la fenêtre.
- **OBSERVER, pas remplacer** : le handler natif remplit la liste de session d'offres,
  qu'on lira par `VendingOffer_GetCount 0x00D5CE90` / `GetAt 0x00D5C780` au lieu de
  `0x2B+0xE8`. Puis DÉTRUIRE 0x2B et 0x2C au tick.
- 🔴 **Piège de fermeture** : aujourd'hui on ferme par la cmd native **185**, qui ne
  fait pas que fermer — elle envoie aussi **`SendMsg 40`** et appelle
  **`VendingBasket_Clear 0x00D56300`**. En détruisant les fenêtres il faut REPRODUIRE
  ces deux effets. (Les MakeWindow(0x2B) trouvés dans le handler @0x00ca462f/0x00ca46a2
  sont des `SaveRectAndCloseWindow` = des FERMETURES, pas des créations.)
- ⚠ **Non tranché** : le plugin passe `0x2B` comme `this` à `BuildDisplayName` pour
  décorer les noms (raffinage, préfixes de cartes). Vérifier le repli quand la fenêtre
  n'existe plus, sinon régression visible sur la liste d'une échoppe.
Voir [[reference_native_window_toggle_router]].

---

✅ **Plugin VendingTweaks LIVRÉ et en service** (confirmé utilisateur 2026-07-31),
les deux sens (vendeur / acheteur / buying store). Seule limite v1 assumée : la
taxe n'est pas affichée (cf. fin de fiche).

Échoppe joueur, client 20250716 — **vente et achat sortent des MÊMES classes**, seul le champ
`mode` change. Doc complète : `docs/vending_window_re.md`. Renommé + commenté dans l'IDB.

- `UIMerchantShopMakeWnd` (composition) : id **0x29** vente / **0xAE** achat, vtable `0x0103D7C0`,
  ctor `0x00935080(this, mode)`, OnCreate `0x00942080`, DrawContent `0x009488D0`,
  OnMsg `0x00957F50`, OnTick `0x0094E550`.
- `UIMerchantMirrorItemWnd` (grille « objets disponibles ») : id **0x2A** / **0xAF**,
  vtable `0x0103D610`, OnMsg `0x00957A60` (retombe sur `UIItemShopWnd_BaseOnMsg 0x00950780`).
🔴 **`MyShop+0xF0` N'EST PAS LE ZENY ENCAISSÉ** (corrigé 02/08, commit **27d25b3**).
La fiche disait « zeny encaissé » : **FAUX**. Vérifié en direct (x32dbg ; objet
MyShop via la globale **`0x0131F7EC`**, vtable 0x0103D100, `+0x2C`=0x2D,
`+0xEC` = nb d'objets en vente, qui suit bien 2 → 1) : le champ vaut **7** et ne
bouge PAS, même après une vente à 250 000 z. Ce n'est ni le cumul, ni la dernière
vente. Écrit par un case d'`OnMsg` @0x00956423 (`mov [esi+0F0h], ebx`) ; **sens
exact non identifié**, et ça n'a plus d'importance.
⚠ **Piège de méthode dont je me suis fait avoir** : j'avais « confirmé » l'hypothèse
« dernière vente » par `25 498 + 5 050 + 7 = 30 555` — or 25 498 avait été déduit
en soustrayant du total. La somme tombait juste **par construction**. Une
vérification qui réutilise la valeur à expliquer ne vérifie rien.

**AUCUN champ natif ne porte le cumul.** L'historique 0x101 non plus : recherche
de sa vtable dans le tas → **absente pendant que l'échoppe tourne** (la sommer
aurait affiché 0).
✅ **La source est le SERVEUR** : il prévient le vendeur à chaque vente par
**`ZC_DELETEITEM_FROM_MCSTORE2` 0x09E5** (18 o :
`{op, index:2, amount:2, buyerCID:4, date:4, zeny:4}`). ⚠ 0x09E5 et pas 0x0137 :
la variante courte s'arrête à PACKETVER < 20141016. OBSERVÉ (le handler natif
garde son travail) ; on cumule `zeny`, remis à zéro au front montant de l'échoppe.
✅ **VÉRIFIÉ EN JEU** (02/08) : le compteur suit bien les ventes. Dossier CLOS.
⚠ En échoppe d'**ACHAT**, `+0xF0` reste une vraie valeur d'état : les fonds
restants. Ce sens-là n'a jamais été remis en cause.

- `UIMerchantItemMyShopWnd` (**« My Shop »**, vue vendeur une fois l'échoppe lancée) :
  id **0x2D** (vente) / **0xB0** (achat), vtable `0x0103D100`, OnCreate `0x0093F770`,
  DrawContent `0x009475C0`, OnMsg `0x00955EC0`. Liste `+0xE8` depuis
  `VendingMyShop_GetCount 0x00D5CE70` / `GetAt 0x00D5C360`. `+0xF0` = **zeny encaissé**
  (OnMsg **119**), `+0x100` = mode, `+0x104` = la case, `+0xC4` = bouton close.
  **Case « Notify when item sells out » = cmd 213**, état persistant
  **`g_MyShopNotifySellOut 0x015FFFB4`** (libellé MsgString 2642 vente / 2698 achat).
  🔴 **Le libellé MENT, et le natif a RETIRÉ la case** (02/08). Les deux handlers de
  rapport (`0x00C9D710` vente → fenêtre 0x101, `0x00C9DC60` achat → 0x102) ne font que
  `Sound_Play3D("effect\ef_steal.wav", 0,0,0, 250, 40)` **à chaque transaction**, épuisé
  ou non — la clé de traduction dit `MSI_SOUNDEFFECT_ITEMSELLWND` / `_ITEMBUYWND`. Et
  `sub_95A9A0` **0x0095A9A0**, appelée juste après `MakeWindow` **0x2D comme 0xB0**
  (`sub_CEE230`, `sub_D01F30`, `sub_D026B0`), fait `SetVisible(case +0x104, false)`,
  remet le drapeau à **0**, met `+0x108 = 1` et pose à la place le bouton « fermer le
  shop » (cmd **506**, confirmation MsgString 0xB6E « Do you want to close the shop? »
  → `SendMsg 301`). Notre panneau la ressuscitait ; **elle en est RETIRÉE** (02/08,
  `6680a81`) — décision de l'utilisateur, prise après ce RE : une promesse d'alerte que
  le client ne tient jamais. Ne pas la remettre sans raison nouvelle.
  ⚠ **Hypothèse jamais vérifiée** : le son étant joué aux coordonnées monde **0,0,0** et
  non à la position du joueur, il pourrait être inaudible loin de l'origine. Se
  trancherait en cochant puis en vendant, pas en raisonnant — cf.
  [[feedback_re_method]].
  ⚠ `+0x108 == 1` change aussi le sens de cmd **201** : il ne DISPATCHE PLUS
  `SendMsg 81/270` (fin de boutique), il ferme seulement la fenêtre. À revérifier avant
  de se fier au commentaire « close = fin du shop ».
  **🔴 Le bouton « close » (cmd 201) MET FIN À LA BOUTIQUE** : `CMode::SendMsg` **81** (vente) /
  **270** (achat) + SendMsg 40, ferme 45/176, ouvre 0x101/0x102. Sauf si `+0x108 == 1`.
  ⚠ **3 classes voisines à ne pas confondre** : `UIMerchantItemShopWnd` **0x2B**,
  `UIMerchantItemPurchaseWnd` **0x2C** (achat CHEZ un vendeur), `UIMerchantItemMyShopWnd` **0x2D**.
  La déduction statique via le handler ZC 0x0136 (qui ferme 43/44) menait à la MAUVAISE fenêtre —
  c'est le RTTI lu en live qui a tranché. **Toujours vérifier l'id par RTTI sur session vivante.**
- `UIMerchantItemLogWnd` (**« Item Sell History »**) : id **0x101** (vente) / **0x102** (achat),
  vtable `0x0103EB50`, ctor `0x00963EF0`, OnMsg `0x00964F70`, 450×152. **Ouverte par le bouton
  close de MyShop.** Liste `+0xE8` (layout famille) + vecteur `lcData` 60 o en `+0xF4`/`+0xF8`.
  msg 23 = **AJOUT d'une ligne**, pas une reconstruction. Son cmd **201 = fermeture PURE**
  (SaveWindowRect seul) — ≠ le cmd 201 de MyShop qui met fin à la boutique.
- ⚠ `UIMerchantItemWnd` id **0x28** n'est PAS le vending : c'est la fenêtre **Chariot** (Alt+W),
  cf. [[project_cart_window_imgui_todo]].

**Mode** : `UIMerchantShopMakeWnd+0x130` et `UIMerchantMirrorItemWnd+0xF4` — `0` = vente,
`1` = achat, `2` = 3e variante du miroir (non observée). Vérifié live dans les deux modes.

**Struct 0x29** : `+0xCC` edit nom · `+0xD8` ligne focus (-1) · `+0xDC` case « safe check »
(vente only) · `+0xE0+4i` edits **prix** · `+0x114+4i` edits **quantité** (achat only) ·
`+0x128` edit limite de zeny (achat only) · `+0x12C` nb lignes · `+0x130` mode · `+0x138` scroll ·
`+0x148` **std::list objets posés** · `+0x14C` count · `+0x15C` lignes affichées ·
`+0x160/0x164/0x168` boutons OK / cancel / **Import**.
Nœud de liste = famille panier shop NPC : `+0x0C` index source, `+0x18` qté, `+0x1C/0x20` prix,
**`+0x34` = l'item id EN TEXTE** (« 714 »), `+0x90` cartes.

**OnMsg 0x29** : msg 6 cmd **184** OK / **185** cancel / **213** case à cocher / **343** aide /
**560** Import. msg 23 rebuild. msg 38 drop (type 11 = miroir vente, 20 = inventaire achat ;
finit par `SendMsg(gameMode, 18)` = fin de drag). msg 123 tags `0x5334/0x5335` (vente) et
`0x5366/0x5367` (achat) → `MakeWindow(0x2A / 0xAF)`.

**🔴 Ouverture = bus `CMode::SendMsg`, PAS un paquet brut** : cmd **82** (vente) / **271** (achat)
avec `(nomBoutique, 1)` ; le **cancel** = les mêmes avec `0`. Même logique que le `cmd 0x28` du
shop NPC ([[project_npc_shop_re]]) — c'est ce chemin qui pose/réinitialise l'état client.

**API session** (`g_session 0x015FA3C0`, le même objet que le panier shop NPC) :
`VendingShop_GetCount 0x00D5CE40` / `GetAt 0x00D5BEA0`, `VendingAvail_GetCount 0x00D5CE60` /
`GetAt 0x00D5C160`, `AddItem 0x00D57C60`, `RemoveItem 0x00D57AC0`,
`SetPriceAt 0x00D77140`, `BuyingStore_SetAmountAt 0x00D76990`, `Clear 0x00D56340`,
`Vending_GetTaxPercent 0x004C9DA0`.

**Globals** : `g_VendingMaxSlots 0x015FB2E4` (écrit par **ZC_OPENSTORE 0x012D**),
`g_BuyingStoreMaxSlots 0x015FB2E8`, `g_VendingSafeCheckEnabled 0x015FFFA1`,
snapshot Import `g_VendingSnapshot_ShopName 0x016023E4` + vecteur `0x01602400/0x01602404`
(pas 0xF8 ; `elem+0x10` qté, `elem+0x14` prix).

**Case « Safe check for over 10 mil zeny »** : cochée → confirmation **par champ** dès
`prix >= atoi(MsgString 0x9AC)` ; décochée → une seule confirmation au OK si `prix > 1e9`.

**PLUGIN `VendingTweaks` — v1 CODÉE (2026-07-27), PAS ENCORE BUILDÉE NI TESTÉE.**
`src/plugins/vending_tweaks.{h,cc}` + les 6 points d'enregistrement (CMakeLists, bourgeon.h/.cc,
window_pos_tweaks hook ids **0x29/0xAE**, setting `vending_imgui` défaut OFF).
**PAS de case isolée** : l'échoppe fait partie du groupe **« Interface moderne »**
(`SetModernInterface`, avec inventaire/chariot/storage/barres/échange/RODEX) — décision de
l'user, motif : l'échoppe se monte à partir du CHARIOT, déjà dans le lot.
**Parti pris = PILOTER LE NATIF, ne rien réimplémenter du protocole** : la fenêtre ImGui écrit
dans les edits natifs (nom `+0xCC`, prix `+0xE0+4i`, qté `+0x114+4i`, limite `+0x128`) via
`UIEdit::SetText` (**vtable+212**), puis déclenche `OnMsg(msg 6, cmd 184)` = le clic OK natif →
on hérite gratuitement de toute la validation, de la taxe, des confirmations et du mode placement
`+0x188`. Annuler = cmd 185, safe check = cmd 213. **Zéro paquet fabriqué.**
Lecture : liste `+0x148` (SEH, POD), nœud `+0x0C` index / `+0x18` qté / `+0x34` itemId-texte /
`+0x90` cartes. `wnd_` **re-résolu à chaque frame** dans OnRenderUI (OnTick@100 ms ⇒ pointeur
pendouillant sinon).
**Les QUATRE fenêtres natives sont cachées** (0x29/0x2A/0xAE/0xAF) et fusionnées en UNE fenêtre
ImGui (demande user : pas deux fenêtres détachées) : panneau « disponibles » + panneau « dans
l'échoppe ».
**Poser/retirer sans glisser** — le drag natif transporte un ItemSkillInfo COMPLET dans
`gameMode+0x308` (pas juste type+index), donc on ne le simule pas : on appelle l'API session
directement, comme le fait le handler de dépôt. **Conventions RELEVÉES sur sites d'appel réels**
(ImportSavedShop 0x936e6f-0x936ead, OnMsg case 38 0x958038-0x958091, mirror 0x957b79-0x957bad) —
toutes `__thiscall` avec **ecx = g_session 0x015FA3C0** :
`VendingAvail_GetCount(s)` / `VendingAvail_GetAt(s, rec, i)` / `VendingShop_GetAt(s, rec, i)` /
`VendingAvail_GetAmountBySrcIndex(s, srcIdx)` **0x00D5C200** = reste à poser /
`VendingShop_GetPlacedAmountBySrcIndex(s, srcIdx)` 0x00D5BF40.

**🔴 PIÈGE DE NOMMAGE des 4 mutateurs (m'a coûté un bug de quantités délirantes) — RENOMMÉS
dans l'IDB** : `0x00D54C40 VendingShop_AddOrMergeItem(s, rec, no_merge, refresh)` **AJOUTE déjà**
à la liste échoppe (`session+0x1748`) ou **cumule** `nœud+0x18 += rec+0x10` ; renvoie **1 = nouveau
nœud, 0 = FUSIONNÉ** (pas « refusé »). `0x00D57C60 VendingAvail_ConsumeItem(s, rec, whole, refresh)`
**DÉCRÉMENTE** la liste des disponibles (`session+0x172C`). Symétriques :
`0x00D54D80 VendingAvail_AddOrMergeItem` / `0x00D57AC0 VendingShop_ConsumeItem(s, rec, whole)`.
**Poser = AddOrMerge(échoppe) PUIS Consume(dispo), SANS tester le retour** (le natif fait ainsi en
vente). Le conditionner → 2e dépôt du même objet : fusion renvoie 0, le stock n'est jamais
décrémenté, l'objet s'empile à l'infini (120 → 174 992 observé). En ACHAT seulement, retour 0 =
déjà présent → refus.
⚠ La liste des dispo EST décrémentée à chaque pose : **sa quantité EST déjà le reste**, ne jamais
en retrancher ce qui est dans l'échoppe (double soustraction).
`rec` = ItemSkillInfo 0x100 o sur la pile, **+0x04 srcIndex, +0x10 qté (modifiable AVANT de poser)**,
et **DEUX std::string à +0x2C et +0x44 qu'il FAUT détruire** (`std_string_dtor 0x004F08F0`) après
usage — le natif le fait à chaque tour de boucle.
Les LISTES sont lues en POD dans les fenêtres cachées (mirror+0xE8 = dispo, wnd+0x148 = posés),
jamais par appel natif par frame ; après chaque mutation on renvoie **msg 23** aux deux fenêtres.
Mutations **différées** hors des boucles de rendu (elles invalident les vectors).
**Import FAIT** : cliquer le bouton natif (**cmd 560**), puis relire prix/qté à la MÊME source que
le natif — le vecteur **`g_VendingSnapshot_begin/_end` (0x01602400/04, pas 0xF8, +0x10 qté,
+0x14 prix, +0x2C itemId en texte)** et le nom **`g_VendingSnapshot_ShopName` 0x016023E4**.
⚠ Le blocage supposé (« il faut savoir relire un UIEdit ») était FAUX : le natif remplit ses edits
DEPUIS ce vecteur, lisible en POD. Lecture différée ~30 frames (c'est du RÉSEAU, cf. ci-dessous).
⚠ **Apparier par ITEM ID, pas par indice de ligne** : le natif saute les entrées dont l'objet n'est
plus dans le cart, un recalage par indice décalerait tous les prix.

**🌐 Le snapshot d'échoppe est stocké par SERVICE WEB, pas en local ni via le protocole de jeu.**
`VendingSnapshot_LoadForChar 0x005E3980` → `0x0056BAF0` → AsyncWork
**`MerchantStoreInformation_Load`** (`CMCStoreInfoBackupLoadAsyncWork`) vers **AssistAddr**
(libcurl, cf. [[reference_web_api_asyncwork_re]]). Sauvegarde symétrique `0x005E3A50` → `0x0056BC30`
à la FERMETURE de la boutique, sérialisée en **JSON** (jsoncpp `Json::StyledWriter`).
Params : account_id, char_id, world_name, store_type (0 vente / 1 achat), clé constante
**`b8e5c779ed77e055`** (0x0160241C).
Côté moonlight → table **`merchant_configs`** (world_name, account_id, char_id, store_type, data
longtext). **VÉRIFIÉ EN BASE : ça fonctionne déjà.** Format de `data` :
`{"data":{"Title":"...","Item":[{"ID":714,"Count":10,"Price":500,"Identified":true,
"RefiningLevel":0,"GradeLevel":0,"Slot_1..4":0,"RandomOptionCount":0,"RandomOptionList":null}]}}`
**Limites v1 restantes** : taxe non affichée (`Vending_GetTaxPercent 0x004C9DA0` prend un prix 64 bits, convention non vérifiée).
Voir [[feedback_native_replacement]] et [[project_ro_imgui_toolkit]].

**`+0x188` ÉLUCIDÉ = mode « échoppe posée sur une case choisie »** (automate `+0x188` dispo /
`+0x189` sélection en cours / `+0x18A`,`+0x18C` = case X,Y en short, -1 = aucune).
Activé par une **SEULE** instruction (`0x0095AADA`, `VendingWnd_EnablePlacementMode 0x0095AAD0`),
appelée uniquement par les handlers **ZC 0x0A7E** (vente, `{len,slots,u16 index[]}` → pré-remplit
la liste dispo) et **ZC 0x0A93** (achat, `{slots}`). Les paquets **hérités 0x012D / 0x0810** sont
traités en ligne dans `RecvLoop_DispatchPackets` et n'y passent pas → **`+0x188` reste 0 sur
moonlight** (conforme au live observé).
Déroulé : OK → `VendingWnd_SetPickingPlacement(1)` cache les 2 fenêtres + chat 0xB72 →
`GameMode_GroundClick_RequestMove 0x00C75AA0` surligne la case → clic gauche →
`VendingWnd_SetPlacementCell(x,y)` + modale 0xB63 → `VendingWnd_SendOpenAtPlacement` →
**cmd 296** (vente) / **298** (achat). Annulation : **297** / **299**.
Ptrs fenêtres : `g_VendingShopMakeWnd 0x0131F7E4`, `g_VendingMirrorWnd 0x0131F7E0`,
`g_BuyingStoreShopMakeWnd 0x0131F8F4`, `g_BuyingStoreMirrorWnd 0x0131F8F8`.
→ Pour la conversion : **tester `+0x188`** plutôt que câbler 82/271 en dur.

## Côté ACHETEUR — 0x2B (offre) + 0x2C (panier)

Ouvertes ensemble en cliquant sur l'échoppe d'un autre joueur. **Confirmé RTTI live.**

`UIMerchantItemPurchaseWnd` id **0x2C**, vtable `0x0103D2B0`, OnCreate `0x00940850`,
DrawContent `0x00947810`, OnMsg `0x009566B0`. `+0xC4`/`+0xC8` boutons buy|sell / cancel
(cmd **184** / **185**), `+0xE8` panier, `+0xF4` label Total, `+0xF8` **AID vendeur**,
`+0x100` index sélectionné (−1 = aucun), `+0x104` **UniqueID échoppe**,
`+0x108` **mode : 0 = j'achète, ≠0 = échoppe d'achat**.

`UIMerchantItemShopWnd` id **0x2B**, vtable `0x0103D028`, OnCreate `0x00941A50`,
DrawContent `0x00947D30`, OnMsg `0x00957190`. Liste `+0xE8` depuis
`VendingOffer_GetCount 0x00D5CE90` / `GetAt 0x00D5C780`. `+0x100` = AID vendeur.

### 🔴 DEUX prix par nœud

`nœud+0x1C` = prix de **BASE**, `nœud+0x20` = prix **EFFECTIF** (Discount appliqué).
`DrawContent` affiche « base -> effectif » quand ils diffèrent, et le total facturé se
calcule sur l'**effectif**. Lire `+0x1C` (ce que font les autres vues, où les deux
coïncident) donne un prix trop élevé et un total faux dès qu'un Discount joue.

Nœud = `ItemSkillInfo` à `+0x08` : `+0x0C`→ISI`+0x04` index d'échoppe, `+0x18`→`+0x10`
qté, `+0x1C`→`+0x14` base, `+0x20`→`+0x18` effectif, `+0x34`→`+0x2C` itemId **en texte**,
`+0x90` slots. Ça recoupe exactement les constantes `kRec*` de `vending_tweaks.cc`.

### Paquet — CZ_PC_PURCHASE_ITEMLIST_FROMMC **0x0801**

Relevé sur le constructeur natif (~`0x00C8E4C0`) :
`+0 opcode:2 | +2 len:2 | +4 AID:4 | +8 UniqueID:4 | [amount:u16, index:u16]*n`
avec `len = 12 + 4n`, **refusé au-delà de 0x800**. `index` = index d'ÉCHOPPE (nœud+0x0C).
Pendant échoppe d'achat = **CZ 0x0819**, entrées de **8 octets**, itemId par `atoi` —
**non décodé** → le plugin rend la main au natif dans ce mode.

Le bouton « buy » n'envoie rien lui-même en mode 0 : il fait `MakeWindow(0xE6)` +
`UIMerchantPurchaseConfirmWnd_SetContext 0x0095CC40 (AID, UniqueID)` (confirmation).
`cancel` (185) = SaveWindowRect + SendMsg 40 + `VendingBasket_Clear`.

API panier (`__thiscall`, `this = &g_UIWindowContextKey` = l'ADRESSE `0x015FA3C0`) :
`VendingBasket_GetCount 0x00D5CE80`, `GetAt 0x00D5C580 (buf, idx)`,
`AddOrMergeItem 0x00D54EA0`, `RemoveItem 0x00D57E40`, `Clear 0x00D56300`,
`GetTotalPrice 0x00D5C730` (__int64).

**Plugin** : modèle ShopTweaks — on LIT la liste résolue du natif et on ÉMET 0x0801
soi-même ; le panier natif n'est jamais touché (le drag vit dans `gameMode+0x308`,
non reproductible). Panneau ImGui unique offre+panier dans `vending_tweaks`.

### 🔴 Aucun rafraîchissement après achat (vérifié sur la source moonlight)

`vending_purchasereq` ne renvoie à l'acheteur que `clif_buyvending` =
**ZC 0x0135** `{index:2, amount:2, result:1}` — **jamais la liste**. Résultats :
0 succès / 1 pas de zeny / 2 surcharge / 4 rupture / 5 en échange / 6 échoppe
incorrecte / 7 pas d'info.

Le client natif esquive en **fermant l'échoppe** dès le clic « buy » (cmd 184 →
`SaveWindowRect(0x2B)`/`(0x2C)` = fermeture). Commentaire rAthena à l'appui :
« Close Vending (this was automatically done by the client) ».

→ Pour garder la fenêtre ouverte : redemander la liste avec
**CZ_REQ_BUY_FROMMC 0x0130 {AID:4}** (6 o, `clif_parse_VendingListReq` →
`vending_vendinglistreq` → `clif_vendinglist`). TCP garantit l'ordre : envoyée juste
après l'achat, elle ramène déjà la liste d'après. Liste vide + achat effectué → fermer.

Index : le serveur fait `idx -= 2` à la réception et `client_index()` = `idx+2` au
retour → l'index du nœud (`+0x0C`) part **tel quel** et revient tel quel.

Titre : le titre natif passe par un buffer GLOBAL partagé `0x00FCE968` réécrit avant
chaque dessin — inexploitable si on masque les fenêtres. Nom du vendeur = passer par
le dictionnaire de noms (`GameMode_GetActive` → `gm+0x160` →
`CNameDict_GetEntryOrRequest 0x005A1460`, nom std::string à `+0x04`).

### Gel des transferts pendant la composition (`sd->state.prevend`)

Posé par le cast de la compétence Vending (`skills/merchant/skill_vending.cpp:20`),
effacé à l'ouverture, à l'annulation (`CZ_REQ_OPENSTORE2` flag=0) ou à l'échec.

Ce que le SERVEUR refuse tant qu'il est levé :
- inventaire ↔ cart : `pc_putitemtocart` / `pc_getitemfromcart` (pc.cpp 6837/6890)
- cart ↔ storage : `storage_storageaddfromcart` / `storage_storagegettocart` (592/631)
- tout ce qui passe par `pc_cant_act2()` (pc.hpp:1204), qui inclut prevend

⚠ **Inventaire ↔ storage n'est PAS bloqué** : `clif_parse_MoveToKafra` ne teste que
`pc_istrading`. Bourgeon le fige quand même côté client (choix assumé, tooltip
distinct : « figé » et non « règle du serveur »).

Côté client : `VendingTweaks::IsComposing()` (sans état — présence de la fenêtre 0x29
ou 0xAE), consommé par `VendingComposing()` dans cart_viewer / inventory_viewer /
storage_tweaks : entrées grisées + bandeau rouge, et garde-fou dans les émetteurs
(les raccourcis double-clic / Alt+clic droit ne passent pas par un widget désactivé).

### 🔴 Import : le snapshot est VIDÉ par le handler natif

`UIMerchantShopMakeWnd_ImportSavedShop 0x00936CE0` copie le vecteur snapshot en local,
remplit ses edits (`element+0x14` = prix, `element+0x10` = quantité, pas de **248 o**),
puis finit par **`VendingSnapshot_Clear 0x005E2A60`** :
`v1 = this+22` (0x016023A8 + 0x58 = `g_VendingSnapshot_begin` 0x01602400), détruit les
éléments et pose **`end = begin`**. Vérifié en mémoire live : `begin == end` après un
import.

→ **Lire le vecteur AVANT de déclencher la cmd 560.** Après, il n'y a que des zéros.
⚠ Piège de diagnostic : `g_VendingSnapshot_ShopName` (0x016023E4) n'est **pas** effacé,
donc le nom s'importe correctement — symptôme trompeur « objets + nom OK, prix à 0 ».

### UI : une seule largeur, dérivée des colonnes

Avec `AlwaysAutoResize`, toutes les sections d'une même fenêtre doivent tomber sur la
MÊME largeur, sinon la fenêtre s'élargit au plus large dès qu'un tableau apparaît
pendant que le reste garde sa taille (bug observé : haut à 330 px, tableau des lignes
à 560). `kContentW` est donc **calculé depuis les colonnes** (`kColName* + kColStock +
kColPrice + kColTotal + kColAct`), et le mode achat reprend sa colonne « Qté » sur la
colonne nom pour retomber sur le même total. Rien de mesuré ni de relatif : ça se
calculerait sur la frame précédente et ferait osciller la fenêtre.

### Noms d'items : BuildDisplayName + cache

Le nom de la DB ignore raffinage, cartes et slots. Il faut `BuildDisplayName
0x008A0570` (repli `0x006A2B50`) avec la fenêtre native pour `this` — mêmes appels que
les viewers inventaire/cart/storage, SEH ISOLÉ par item.
⚠ Les listes sont relues à CHAQUE frame : composer 128 noms par frame reproduit le gel
GDI du chat. → cache par signature d'instance (id, refine, 4 cartes, slots, identifié).
La composition doit vivre HORS du `__try` de la lecture (conteneur C++ → C2712).

### Refus MUET à l'ouverture : overflow zeny

Étape 9 du chemin OK natif (vente uniquement) : si **`g_PlayerZeny + Σ(qté × prix) >
0x7FFFFFFF`**, le client REFUSE d'ouvrir le shop (`MsgString 0xEF2`). Piloter le bouton
natif hérite du refus **sans retour visible** : le joueur clique, rien ne se passe.
→ Tester soi-même en 64 bits AVANT, griser le bouton et afficher l'excédent.
⚠ Le zeny COURANT entre dans le calcul : un personnage riche plafonne bien plus tôt.

### 🔴 « SaveWindowRect » 0x00A2E770 DÉTRUIT la fenêtre

Le nom est trompeur : elle sauve la position/taille **puis** appelle
`UIWindowMgr_QueueDestroyWindow 0x00A447D0`. Renommée
**`UIWindowMgr_SaveRectAndCloseWindow`** dans l'IDB. Tous les `SaveWindowRect(id)`
des OnMsg sont donc des **fermetures**.

Conséquence sur l'historique des ventes : `UIMerchantItemLogWnd` (0x101/0x102) existe
**dès la première vente** et sert d'**ACCUMULATEUR** (son `OnMsg 23` AJOUTE une ligne ;
il ne reconstruit rien depuis une liste de session, contrairement au reste de la
famille). Le close de « Mon shop » ne fait que la remonter — `MakeWindow(0x101)` rend
l'existante.

→ Ne JAMAIS déclencher sa cmd 201 pendant que le shop tourne : ça détruit la fenêtre
et donc tout l'historique, silencieusement. Le panneau ImGui ne doit s'afficher
qu'une fois le shop terminé (`!myshop_open_`).

### Descriptions d'objet : trois pièges

1. **`BuildDisplayName` n'ajoute PAS le suffixe d'emplacements `[N]`** — il compose le
   raffinage et le préfixe de carte, rien de plus. `itemdesc::RenderSimpleDesc` l'ajoute
   de son côté (param `display_name`) ; tout appelant doit faire pareil.
2. **Clic DROIT** pour la description complète, comme le client natif et les autres
   viewers. (J'avais câblé le gauche : incohérent avec tout le reste.)
3. La fenêtre de description est reprise en ImGui par `ItemDescTweaks`, qui masque la
   native. Ouvrir la desc en appelant `MakeWindow(0xC)` + `OnMsg 0x18` soi-même ne suit
   pas forcément le chemin qui déclenche son hook → appeler explicitement
   `ItemDescTweaks::HideNativeDescWindows()`, sinon la native reste visible **sous**
   l'overlay ImGui (elle est dessinée par la passe UI du jeu). Et comme le clic vient de
   focaliser notre fenêtre, penser à `ImGui::SetWindowFocus(nullptr)` pour que le
   panneau qui apparaît le même frame ne passe pas derrière.

## Vendre à un buying store : 0xB1 / 0xB2 / 0xB3 (PAS 0x2B/0x2C)

🔴 **Correction d'une erreur que j'avais écrite ici même** : j'affirmais que ce mode
réutilisait `0x2B`/`0x2C` avec `+0x108 != 0`. Faux. Le natif ouvre **trois** fenêtres :

| id | Classe (même vtable que le mode achat !) | Rôle |
|---|---|---|
| `0xB1` | `UIMerchantItemShopWnd` `0x0103D028` | « Wanted items - *acheteur* » |
| `0xB2` | `UIMerchantItemPurchaseWnd` `0x0103D2B0` | « Selling Items » + sell/cancel |
| `0xB3` | `UIMerchantMirrorItemWnd` `0x0103D610` | « Available items: » |

La preuve est dans `UIMerchantItemPurchaseWnd_OnMsg` : ses `cmd 184`/`185` ferment
`177/178/179` si mode == 1, `43/44` sinon. Mon test sur `0x2C` était donc **inatteignable**.

Globaux (remis à 0 à la fermeture → détecteur fiable) : `0x0131F900` `g_BuyingStoreWantedWnd`,
`0x0131F904` `g_BuyingStoreSellListWnd`, `0x0131F908` `g_BuyingStoreSellMirrorWnd`.
⚠ Ne pas confondre avec `0x0131F8F8` `g_BuyingStoreMakeMirrorWnd` (miroir de COMPOSITION 0xAF).

`0xB1+0x100` AID acheteur, **`+0x108` fonds restants**, `+0x10C` storeId.

**3e liste de session** `VendingBasket_*` à session+0x1740 : `GetAt 0x00D5C580`,
`AddOrMergeItem 0x00D54EA0` (**2 args**), `RemoveItem 0x00D57E40` (**2 args**).

⚠⚠ Le 3e param de `VendingAvail_{Consume,AddOrMerge}Item` **n'est pas un mode** : non nul =
retire/rend le **nœud ENTIER**, et il choisit le miroir à re-notifier (1=0xAF, 2=0xB3, 0=0x2A).
Le natif passe **2** — **ICI, c'est-à-dire côté vente à un BUYING STORE, et là seulement** :
mettre 1 unité d'une pile au panier en retire la pile entière des disponibles, parce que
`buyingstore_trade` rejette toute la transaction sur un index en doublon.

🔴 **NE PAS étendre ça à l'échoppe de VENTE** (erreur commise le 02/08, corrigée en
`a30d825`). Là, le mode vaut **0**, `VendingAvail_ConsumeItem` ne retranche que `rec+0x10` :
la pose est **PARTIELLE**, le reste demeure dans les disponibles, et **reposer le même objet
CUMULE** dans la ligne existante (`kShopAddOrMerge` fusionne par index source). Constaté en
jeu : 100 Scorpion Tail posés sur 150, puis 25 de plus → la ligne affiche 125, disponibles 25.
Conséquence UI : toute pose depuis une grille d'icônes doit **demander la quantité**.

**Émission : on PILOTE le natif (cmd 184), on ne fabrique pas le paquet** — contrairement à
l'achat. `CZ 0x0819` veut un index d'**inventaire** (le serveur fait `index-2`), tenu par les
listes de session que seul le natif remplit. Entrées de **6 octets** (pas 8).

## ⚠ Le même offset, trois sens différents

- `nœud+0x1C` : prix **unitaire** dans « Mon shop » / l'offre… mais **MONTANT de ligne**
  (déjà × qté) dans l'**historique** `0x101`/`0x102`. `UIMerchantItemLogWnd_DrawContent`
  `0x009645B0` l'affiche tel quel et n'a **aucune** colonne quantité. Le remultiplier
  donnait 250 600 z pour 5 400 z réels.
- `MyShop+0xF0` : zeny **encaissé** en vente, zeny **RESTANT** en buying store
  (« Encaissé » y est un contresens → « Fonds disponibles »). Mode en `+0x100`.
- `0x101` = historique de **ventes**, `0x102` = d'**achats** : même classe, vocabulaire opposé.
