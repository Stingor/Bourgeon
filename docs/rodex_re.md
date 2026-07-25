# Courrier RODEX (boîte mail moderne) — RE

Client `Moonlight-Destiny.exe` (base 0x400000, **pas de rebase**), PACKETVER **20250716**.
Serveur = fork rAthena `moonlight` (`src/map/mail.cpp`, `rodex.cpp`, `clif.cpp`).

Deux systèmes de courrier **coexistent** dans le client :

- **Ancien mail** : `UIOpenMailBoxWnd` (classe 262), window id **0x106** (ctor 0x0090cc70, vt 0x01039dac).
  Boîte Kafra à l'ancienne (un seul onglet, une pièce jointe). **Hors cible.**
- **RODEX** (courrier moderne, multi-onglets, jusqu'à 5 pièces jointes + zeny) : **la cible** de ce doc.
  Nommé « RODEX » dans les classes RTTI et les textures (`basic_interface\rodexsystem\renewal\*`).

Objectif : remplacer la fenêtre native RODEX par une fenêtre **ImGui** (skin RO), sur le modèle
*state-driven* : lire directement l'état déjà agrégé dans `g_RodexMgr` et émettre les mêmes commandes
que la fenêtre native (paquets CZ construits en clair, ou messages `GameMode`). Voir §7.

---

## 1. Classes, vtables, fonctions (Ghidra, renommées)

| Classe | rôle | window id | fonctions clés |
|---|---|---|---|
| **CRodexSystemMgr** | singleton d'état (les 3 boîtes) | — | `CreateInstance` **0x007c7070** (new 0x38) ; singleton **`g_RodexMgr` = DAT_0131ecdc** |
| **UIRodexWnd** | fenêtre **liste/inbox** | **0x107** (MakeWindow) / **0x109** (SaveWindowRect) | ctor **0x007cd7d0**, OnCreate **0x007ce340**, DrawContent **0x007cf7c0**, OnMsg **0x007cb460**, dtor 0x007cdce0, scalar-dtor 0x007cdea0. vtable **PTR_UIRodexWnd_…_01022170**. obj size **0x208** |
| **UIRodexReadWnd** | **lecture** d'un courrier + pièces jointes | *(distinct, ouvert via GameMode msg 0xc4)* | OnMsg **0x007d0520** |
| **UIMailWriteWnd** | **composition** (partagée avec l'ancien mail) | **0x108** (SaveWindowRect) | OnMsg **0x007c9d30** |

Helpers RODEX renommés/commentés dans Ghidra :
`Rodex_MapFindByMailId` (**0x007cc300**, `std::map::find` par mailID),
`Rodex_MapNewestMailId` (**0x007cdd70**, mailID max = le plus récent d'une boîte),
`UIRodexWnd_DrawRowStatusIcons` (0x007ce1d0), `UIRodexWnd_DrawRowExpiry` (0x007d20f0),
`UIRodexWnd_UpdateRowLinkButtons` (0x007d1b40).

**Slots de vtable** (famille UIWindow) utilisés : +0x10 `SetPos(x,y)`, +0x38 `SetVisible`,
+0x3c `OnCreate`, +0x50 `DrawContent`, +0x94 `OnMsg`, +0x98 `Invalidate/Repaint`, +0xb4 `SetId`,
+0xd4 `Hide`. Réglage anti-spam : `GameSettingsUI_GroupRodexSpam_ctor` (0x009ea020, `CUIGameSettingsUI`).

**Singletons de fenêtres** (pointeurs d'instance globaux, renommés Ghidra — rôles **confirmés** via
les handlers recv `Recv_ZC_MailList/ReadRodex/DeleteResult/BeginWriteResult`) :

| Global | = fenêtre | preuve |
|---|---|---|
| **`g_RodexInboxWnd`** (0x0131f93c) | **UIRodexWnd** (inbox) | `RodexInbox_RemoveRowByMailId` opère sur son vecteur de lignes `+0x1e4/+0x1e8` ; reçoit msg 0x3c (refresh lignes) à chaque mail lu/supprimé ; rafraîchi par le handler de liste. |
| **`g_RodexReadWnd`** (0x0131f944) | **UIRodexReadWnd** (lecture) | reçoit msg **0x17** (refresh contenu) quand le corps arrive (`Recv_ZC_ReadRodex`), msg **0xc9** (fermer) quand son mail est supprimé. |
| **`g_MailWriteWnd`** (0x0131f940) | **UIMailWriteWnd** (compose, id 0x108) | `Recv_ZC_RodexBeginWriteResult` : `if (g_MailWriteWnd==0) MakeWindow(0x108)`. Empreinte xref **large** (inventaire, dialogues) = lu comme « la compose est-elle ouverte ? » (ex. clic-objet inventaire → pièce jointe). Partagé ancien mail + RODEX. |

Le garde d'entrée de `UIRodexReadWnd::OnMsg` (`g_MailWriteWnd==0 && g_RodexReadWnd==0`) rend la ReadWnd
inerte tant qu'une **compose** est ouverte. Compteur de **non-lus** : **`g_RodexMgr+0x18`** — décrémenté à
chaque suppression ; s'il tombe à 0, efface le **badge** de `UIMenuIconWnd` (id 0x133, msg 6/id 0x11e).

---

## 2. `CRodexSystemMgr` — struct (`g_RodexMgr`, 0x38 octets)

| Offset | type | rôle |
|---|---|---|
| +0x00 | char | flag « initialisé/valide » |
| +0x08 | int64 | **mailID sélectionné/ouvert** (low `+0x08`, high `+0x0c`) |
| +0x10 | byte | **onglet courant / openType** : **0 = Normal (Mail)**, **1 = Account**, **2 = Return** |
| +0x18 | int | **compteur de non-lus** (décrémenté à la suppression ; 0 ⇒ efface le badge `UIMenuIconWnd` 0x133) |
| +0x1c | byte | flag **boîte vide** (bloque lecture/actions ; DrawContent affiche `img_notice`) |
| +0x20 | `std::map<int64,RodexMail>` head | **boîte Normal** (openType 0) |
| +0x24 | uint | taille (nb de mails) boîte Normal |
| +0x28 | `std::map<int64,RodexMail>` head | **boîte Account** (openType 1) |
| +0x2c | uint | taille boîte Account |
| +0x30 | `std::map<int64,RodexMail>` head | **boîte Return** (openType 2) |
| +0x34 | uint | taille boîte Return |

Les 3 `std::map` sont créées vides dans `CreateInstance` (nœud sentinelle `operator_new(0x1b0)` par map).
Chaque map est indexée par **mailID (int64)**, ordre croissant → le plus récent = nœud le plus à droite
(`Rodex_MapNewestMailId`). `Game_InitAllManagers` appelle `CreateInstance` au boot.

### Nœud de map / struct `RodexMail`

Nœud MSVC : `{L@0, P@4, R@8, color@0xc, isnil@0xd}`, **clé int64 @+0x10**, **valeur `RodexMail` @+0x18**
(nœud complet = **0x1b0** octets → valeur ≈ 0x198). Champs prouvés (offsets **relatifs au nœud**) :

| Offset nœud | idx int | type | rôle |
|---|---|---|---|
| +0x10 | [4]/[5] | int64 | **mailID** (clé ; low [4], high [5]) |
| +0x18 | [6] | int (low byte) | **type / nb pièces jointes** (→ `DrawRowStatusIcons` arg3) |
| +0x19 | — | byte | **flags d'état** : `&6` = **lu/verrouillé** (bloque relecture, erreur `MSI 0xa34`) ; `&8` testé pour l'aperçu d'objet |
| +0x1c | [7] | `std::string` | **nom de l'expéditeur** (24 o) |
| +0x34 | [0xd]/[0xe] | int64 | **date d'expiration** (→ `DrawRowExpiry`) |
| +0x3c | [0xf] | `std::string` | **titre/sujet** (24 o) |
| +0x1ac | [0x6b] | int | **flag « zeny/objets à réclamer »** (>0 = présent, cf. ReadWnd) |

Le **corps** du message et le détail des **pièces jointes** occupent le reste de la valeur ; ils sont
remplis à la lecture (ZC_ACK_READ_RODEX 0x09EB). Dans la ReadWnd, les objets d'un mail sont énumérés
avec un **stride de 0x60** octets.

---

## 3. `UIRodexWnd` (inbox) — contrôles & commandes

### Layout (extrait, membres = `this[idx]`, offset = idx*4)

| Membre | contrôle | ctrl-id (`SetId`) | strId | rôle |
|---|---|---|---|---|
| this[0x4c] | onglet | — (toggle msg 0xd) | **0xddb** | **openType 0 — Normal** |
| this[0x4d] | onglet | — | **0xdda** | **openType 1 — Account** |
| this[0x4e] | onglet | — | **0xddc** | **openType 2 — Return** |
| this[0x4f] | UIBitmapButton | **0x19f** | 0xb21 | **Écrire** (compose) |
| this[0x50] | UIBitmapButton | **0x131** | 0x402 | ouvre la compose (→ GameMode msg 0x10c) |
| this[0x57..] | **6 lignes** (stride 0x2c) | 0x143 / 0x142 / 0x136 | 0xa8e / 0xa8d / 0xb20 | par ligne : toggle sélection + bouton **expéditeur** (id 0x143) + bouton **titre** (id 0x142) + **bouton lien** (id 0x136) |
| this[99] | UITextButton | **0x222** | 0xe05 | pied — supprimer |
| this[100] | UITextButton | **0x224** | 0xe08 | pied — supprimer tout |
| this[0x65] | UIBitmapButton | **0x13c** | 0x511 | page ◀ |
| this[0x66] | UIBitmapButton | **0x13d** | 0x512 | page ▶ |
| this[0x70..] | 2 onglets catégorie | 0x141 | 0xc78/0xc79 | filtres (tex `rodexsystem\renewal\…`) |
| this[0x72] | **UIEdit** | — | — | **recherche** (maxlen `+0x88`=0x10) |
| this[0x73] | UIBitmapButton | **0x141** | — | bouton recherche |
| this[0x74] | onglet | — | 0xddd | onglet « tous » |

État de pagination : `this+0xe8/+0xec` (bornes bas), `this+0xf4` (borne haut) ; liste de pièces jointes
en attente `this+0xf0` (nœud +0xf4 = count) ; `this+0xb8` = `std::string` (destinataire pré-rempli).

### OnMsg (`0x007cb460`) — commandes → paquets

`param_2 == 6` (clic bouton), `switch(param_3)` :

| ctrl-id | action | paquet émis / effet |
|---|---|---|
| **0xbd** | page (précédente) | **CZ 0x09F3** `{u16; int64 mailID(g_RodexMgr+8); u8 openType(+0x10)}` = **11 o**. Gardé par `this+0xf4≠0`. |
| **0xbe** | page (suivante) | **CZ 0x09F1** `{u16; int64 mailID; u8 openType}` = **11 o**. Gardé par `this+0xe8/+0xec`. |
| **0xc9** | fermer | `SaveWindowRect(0x109)` ; efface `g_RodexMgr+8/+0xc` ; notifie `DAT_0131f93c` (msg 0x3c). |
| **0xd3** | **lire** le mail sélectionné | dialogue de confirmation (`MSI 0x164`) → **`GameMode::SendMsg(0xc4, mailID, openType)`** ⇒ **CZ_REQ_READ_RODEX 0x09EA**. Bloqué (chat `MSI 0xa34`) si flags `&6`. Lookup via `Rodex_MapFindByMailId` sur la bonne boîte. |
| **0x131** | ouvrir compose | `GameMode::SendMsg(0x10c, &this+0xb8)` (destinataire). Gardé par `this+0x10c≠1`. |
| **0x136** | **supprimer/retourner** | **CZ 0x0B98** `{u16; u32 mailID_low}` = **6 o** (⚠ n'envoie que les **32 bits bas** du mailID). |

Autres messages :
- **`param_2 == 0x17`** : (re)construit la liste de pièces jointes `this+0xf0` depuis
  `g_UIWindowContextKey` via `ItemSkillInfo_CopyFull`, **max 5** objets (`this+0xf4` = count).
- **`param_2 == 0x6a`** : timer — invalide le nœud `this+0x108`.
- défaut → `UIWindow_OnMsg_Default`.

### DrawContent (`0x007cf7c0`)

Blitte `bg_rodex_list.bmp`. Si boîte vide (`g_RodexMgr+0x1c≠0`) → `img_notice.bmp` + `MSI 0xc76/0xc77`.
Sinon boucle **6 lignes** (idx 0..5) sur la map de l'onglet courant (`+0x10`→ boîte `+0x20/+0x28/+0x30`),
et par ligne appelle `DrawRowStatusIcons` (icônes lu/objets), `DrawRowExpiry` (date), `UpdateRowLinkButtons`
(2 colonnes de liens : expéditeur `nœud+0x1c`, titre `nœud+0x3c`). Compteur de page en bas (`MSI 0xbda`).

---

## 4. `UIRodexReadWnd` (lecture) — OnMsg `0x007d0520`

Garde d'entrée : `DAT_0131f940==0 && DAT_0131f944==0` **et** `g_RodexMgr+0x1c==0` (boîte non vide).
Struct locale : `this+0x174` = **6 toggles** de sélection de pièce jointe ; `this+0x19c` = index de base
des objets du mail courant ; `this+0x1d4` = flag « tout sélectionner » ; `this+0x1d8/+0x1dc` = vecteur
des objets sélectionnés (stride 0x60) ; `this+0x1ac` = pièces jointes.

`param_2 == 6` (boutons) :

| ctrl-id | action | effet |
|---|---|---|
| **0x222** | supprimer **ce** mail | confirmation (`MSI 0xe06` + nom de boîte 0xddb/0xdda/0xddc) → `FUN_007cdff0`. |
| **0x223** | **réclamer** toutes les pièces jointes/zeny | confirmation (`MSI 0xe09`) ; itère les 6 toggles cochés → `FUN_007d0110(openType, itemLo, itemHi)` par objet. |
| **0x224** | supprimer **tous les mails lus** | confirmation (`MSI 0xe0a`) ; itère la map de l'onglet, `FUN_007d0110`. |
| autres | jump-table `switchD_007d05ea` (id ≥ 0xc9). |

`param_2 == 0x62` (clic sur une ligne d'objet) :
- **0x142** : aperçu/ouvre l'objet joint (boîte Normal) — `Rodex_MapFindByMailId(+0x20)`, si objet présent (`mail+0x2c≠0`, flags `&8`==0) → ouvre la fenêtre desc.
- **0x143** : **réclamer** un objet — si non réclamable (`mail+0x1ac==0`) → avertissement `MSI 0xf1b`, sinon GameMode.

`param_2 == 0x17` : refresh contenu (`FUN_007d2410` + `FUN_007d0360` + `FUN_007d1a50`).

---

## 5. `UIMailWriteWnd` (composition) — OnMsg `0x007c9d30`

Champs `UIEdit` (rôles corrigés d'après la validation d'envoi ; l'ancien commentaire inline « zeny +0xd0 »
est **erroné**) :

| Offset | rôle | contraintes |
|---|---|---|
| +0xb4 | **destinataire** (nom) | ≤ 0x17 (23) car., non vide |
| +0xb8 | destinataire (2ᵉ champ / affichage) | testé vide avec +0xb4 |
| +0xd0 | **sujet/titre** | **4 à 0x32 (50)** car. |
| +0xd4 | **corps** du message | < 0x3e9 (**≤ 1000**) car. |
| +0xf8 / +0xfc | **zeny** joint (u32 / high) | ≤ `g_PlayerZeny` − coût (`FUN_007c7f30`) |
| +0xe8 / +0xec | liste **pièces jointes** / count | **max 5** |

`param_2 == 6` :

| ctrl-id | action | paquet / effet |
|---|---|---|
| **0xc9** | fermer | `SaveWindowRect(0x108)`. |
| **0xd5** | **valider le destinataire** | **CZ 0x0B97** `{u16; char name[24]; u8 sex}` = **27 o**. Résout nom→sexe (`FUN_00d71ce0`/`FUN_00d71d80`). Erreurs : tag interdit (`NpcTextInput_ContainsForbiddenTag` → `MSI 0xafc`), nom trop long (`MSI 0xa27`). |
| **0x159** | **envoyer** | validations (destinataire+sujet vides → `MSI 0xa23` ; sujet hors 4–50 → `0xa24` ; corps > 1000 → `0xa44` ; zeny insuffisant → `0xa35/0xa53` ; tag interdit corps → `0xbe2`). Puis **`GameMode::SendMsg`** ⇒ **CZ_SEND_RODEX (0x09EC / 0x0A6E, VAR)** — champs : destinataire(+0xcc id), sujet(+0xb8), corps(+0xd4), zeny(+0xf8), objets joints. |

Autres : `0x17` = attacher objets (≤5) depuis `g_UIWindowContextKey` ; `0x62` = ouvre desc objet
(`MakeWindow` id **0xc**) ; `0x26`/`0x68` = interactions GameMode (drag, fermeture → msg 0x12).

---

## 6. Protocole (PACKETVER 20250716) — table opcodes

> ⚠ La colonne « nom » de `opcode_map.md` est heuristique (déduite des tailles) : pour RODEX elle
> confond plusieurs paquets (`Mail_getattach` sur 0x09F1/0x09F3 qui sont en réalité la **pagination**).
> Ci-dessous = comportement **prouvé côté client**.

### Client → serveur (CZ) — construits **en clair** dans l'UI (pinnés)

| Opcode | taille | construit par | struct |
|---|---|---|---|
| **0x0AC0** | 26 | `UIRodexWnd_ctor` | `{u16; int64 newest_Normal; int64 newest_Return; int64 newest_Account}` — **ouverture boîte** (seed les 3 plus récents) |
| **0x09F3** | 11 | inbox OnMsg 0xbd | `{u16; int64 mailID; u8 openType}` — **page** |
| **0x09F1** | 11 | inbox OnMsg 0xbe | `{u16; int64 mailID; u8 openType}` — **page** |
| **0x0B98** | 6 | inbox OnMsg 0x136 | `{u16; u32 mailID_low}` — **supprimer/retourner** (32 bits bas seulement) |
| **0x0B97** | 27 | write OnMsg 0xd5 | `{u16; char name[24]; u8 sex}` — **vérifier destinataire** |

### Client → serveur (CZ) — via `GameMode::SendMsg` (opcode non construit dans l'UI, cf. `opcode_map`)

| Opcode | taille | déclencheur | parseur rAthena |
|---|---|---|---|
| **0x09EA** | 11 | lire (inbox 0xd3 → GameMode 0xc4) | `clif_parse_Mail_read` → `rodex_read_mail` |
| **0x0A08** | 26 | begin compose (inbox 0x131/0x50 → GameMode 0x10c) | `clif_parse_Mail_beginwrite` |
| **0x0A04** | 6 | attacher objet (compose msg 0x17) | `clif_parse_Mail_setattach` |
| **0x09EC** / **0x0A6E** | VAR | envoyer (write 0x159) | `clif_parse_Mail_send` → `rodex_send_mail` |
| **0x09F5** | 11 | supprimer (ReadWnd) | `clif_parse_Mail_delete` |
| **0x0A03** | 2 | annuler compose | `clif_parse_Mail_cancelwrite` |
| **0x0A06** | 6 | ouverture boîte (NPC/keybind) | `clif_parse_Mail_winopen` |
| **0x09E8** / **0x09EE** / **0x09EF** | 11 | refresh inbox | `clif_parse_Mail_refreshinbox` |
| **0x0A13** | 26 | check name (variante) | `CZ_CHECKNAME1` |

### Serveur → client (ZC) — handlers = **thunks de jump-table** `0x00caa2e0[opcode−0x73]` (région « Lotus », non typés par Ghidra ; peuplent `g_RodexMgr`)

Les thunks tail-call vers les fonctions de traitement suivantes (renommées/commentées Ghidra) :

| Opcode | taille | handler (renommé) | rôle |
|---|---|---|---|
| **0x09F0** | VAR (base 7) | **`Recv_ZC_MailList_0x09F0`** (0x00cfc070) | **liste inbox** → pose openType/empty, **insère chaque mail** dans la map (`FUN_00ce8d70`), résout le sexe expéditeur, rafraîchit `g_RodexInboxWnd` |
| **0x09EB** | VAR (base 24) | **`Recv_ZC_ReadRodex_0x09EB`** (0x00cfbe00) | **corps du mail lu** ; efface le bit non-lu (`mail+0x19-=4`) ; msg 0x17→`g_RodexReadWnd`, msg 0x3c→`g_RodexInboxWnd` |
| **0x0B63** | VAR (base 24) | *(variante récente de ↑)* | ZC_ACK_READ_RODEX |
| **0x0A05** | 63 | *(add-item)* | **ZC_ACK_ADD_ITEM_RODEX** — ack pièce jointe (compose) |
| **0x0B3F** | 64 | *(variante récente)* | ZC_ACK_ADD_ITEM_RODEX |
| *(suppr.)* | — | **`Recv_ZC_RodexDeleteResult`** (0x00cf8b10) | retire le mail, ferme `g_RodexReadWnd`, décrémente non-lus `g_RodexMgr+0x18`, retire la ligne inbox |
| *(begin-write)* | — | **`Recv_ZC_RodexBeginWriteResult`** (0x00cfcc80) | ouvre la compose (`MakeWindow(0x108)`) si `g_MailWriteWnd==0` |
| **0x09ED** | 3 | | ack (résultat écriture/suppression) |
| **0x09F2** / **0x09F4** | 12 | | ack (page / attach) |
| **0x09F6** | 11 | | ack (zeny réclamé) |
| **0x0A07** | 9 | | ack (objet réclamé) |
| **0x0A12** | 27 | | réponse check-name (détail) |
| **0x0A14** | 10 | | **ZC_CHECKNAME** — nom→id/classe du destinataire |

Dispatch : `RecvLoop_DispatchPackets` (0x00c9df00) → `handler = *(0x00caa2e0 + (opcode−0x73)*4)`.

---

## 7. Plan de remplacement ImGui (à coder — `rodex_tweaks`)

**Principe** : *state-driven* (et non packet-driven pur). Tout l'état des 3 boîtes est **déjà agrégé**
dans `g_RodexMgr` (maps `std::map<int64,RodexMail>`) par le natif ; le plugin **lit** ces maps et **émet**
les mêmes commandes que la fenêtre native. Défaut **OFF** (opt-in), setting `rodex_imgui`.

- **Lecture d'état (thread principal, jamais depuis OnRecvPacket)** :
  - onglet courant `g_RodexMgr+0x10` ; itérer la map de la boîte (`+0x20/+0x28/+0x30`) — nœud MSVC,
    valeur `RodexMail` @nœud+0x18 (mailID clé +0x10, expéditeur +0x1c, expiration +0x34, titre +0x3c,
    flags lu +0x19, objets/zeny à réclamer +0x1ac). Réutiliser `Rodex_MapFindByMailId` pour le mail sélectionné.
  - flag boîte vide `+0x1c` → écran « aucun courrier ».
- **Émettre les commandes** (thread principal, via `OnTick`/`OnRenderUI`) :
  - **ouvrir** : CZ **0x0AC0** (seed `Rodex_MapNewestMailId` × 3) — ou laisser le natif l'avoir déjà fait.
  - **changer d'onglet** : poser `g_RodexMgr+0x10` puis re-`refreshinbox` (**0x09E8/0x09EE/0x09EF**).
  - **lire** : `GameMode::SendMsg(0xc4, mailID, openType)` (⇒ 0x09EA). Vérifier `flags &6` avant.
  - **pagination** : CZ **0x09F1/0x09F3** `{mailID; openType}`.
  - **supprimer/retourner** : CZ **0x0B98** `{u32 mailID_low}`.
  - **réclamer** objet/zeny : voie ReadWnd (`FUN_007d0110`) ou GameMode.
  - **composer** : `GameMode::SendMsg(0x10c, &recipient)` puis le flux write (check-name **0x0B97**,
    attach **0x0A04** ×≤5, envoi **0x09EC/0x0A6E**). Les validations serveur restent autoritaires.
- **Masquer le natif** : les 3 instances sont directement lisibles via **`g_RodexInboxWnd` (0x0131f93c)**,
  **`g_RodexReadWnd` (0x0131f944)**, **`g_MailWriteWnd` (0x0131f940)** — masquage natif via offset
  visibilité **+0x28** (plus simple que le matching par vtable ; se rabattre sur la vtable `0x01022170`
  dans le hook `MakeWindow` si l'on préfère intercepter à la création), **jamais** hors-écran
  ([[feedback_no_offscreen_hide]]). Le badge de non-lus (`g_RodexMgr+0x18` → `UIMenuIconWnd 0x133`)
  reste piloté par le natif — le laisser tel quel.
- **Sécurité threads** : lectures mémoire OK en running ; **jamais** `pause_process` (déco) ; fenêtres
  ImGui protégées du minimize ([[feedback_imgui_minimize_frame_guard]]).

**Réutilisables** : toolkit `ro_imgui` (BeginRoWindow/RoButton/RoCheckbox), résolveur nom/icône d'objet
(cf. shop/trade), cache d'icônes ImGui (`Overlay_DeviceEpoch`, [[feedback_texture_cache_device_epoch]]),
accents FR ([[feedback_french_ui_accents]]). Voir le doc frère [[project_trade_window_re]] (`docs/trade_window_re.md`)
pour le patron *masquage-par-vtable + émission de commandes*, quasi identique.
