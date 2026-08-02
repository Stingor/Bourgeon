# Courrier RODEX (boîte mail moderne) — RE

> **Révision 2026-07-26** — RE relu fonction par fonction (IDA) et vérifié en live (x32dbg,
> boîte ouverte). La première rédaction attribuait **à l'envers** la fenêtre *liste* et la
> fenêtre *lecture*, et se trompait sur trois opcodes. Corrections appliquées dans tout le
> document ; les §3/§4/§6/§7 ci-dessous sont la version corrigée :
> - **UIRodexWnd (LISTE)** = vtable **0x01022170**, OnMsg **0x007d0520**, DrawContent 0x007cf7c0,
>   id **0x107** (lu live : `g_RodexInboxWnd+0` = vtable, `+0x2c` = 0x107).
>   **UIRodexReadWnd (LECTURE)** = vtable **0x01021FBC**, OnMsg **0x007cb460**,
>   DrawContent **0x007cae70**, id **0x109**.
> - **0x09F1 = réclamer le ZENY**, **0x09F3 = réclamer les OBJETS** (ce ne sont *pas* des
>   paquets de pagination : la pagination est purement **locale**, index `wnd+0x19C`, 6 lignes).
> - **cmd 0xc2 = lire** (CZ 0x09EA), **cmd 0xc4 = supprimer** (CZ 0x09F5). L'ancien
>   `Rodex_ReadMailById` est en réalité **`Rodex_DeleteMailById`** (0x007cdf50).
> - Le paquet d'ouverture/rafraîchissement est **0x0AC1** (et non 0x0AC0), 26 o, ordre
>   **Normal, Retour, Compte**.
> - `nœud+0x19` n'est pas un « flag lu » mais le **MAIL_TYPE** rAthena (`&2` zeny joint,
>   `&4` objets joints).
>
> **Correctif 2026-07-26 (2ᵉ passe)** — `nœud+0x18` avait été noté « corps déjà reçu » : c'est
> **faux**, et c'est le piège central de ce système. Voir « Le contenu d'un courrier n'existe
> pas tant qu'il n'a pas été demandé » ci-dessous.

Client `Moonlight-Destiny.exe` (base 0x400000, **pas de rebase**), PACKETVER **20250716**.
Serveur = fork rAthena `moonlight` (`src/map/mail.cpp`, `rodex.cpp`, `clif.cpp`).

Deux systèmes de courrier **coexistent** dans le client :

- **Ancien mail** : `UIOpenMailBoxWnd` (classe 262), window id **0x106** (ctor 0x0090cc70, vt 0x01039dac).
  Boîte Kafra à l'ancienne (un seul onglet, une pièce jointe). **Hors cible.**
- **RODEX** (courrier moderne, multi-onglets, jusqu'à 5 pièces jointes + zeny) : **la cible** de ce doc.
  Nommé « RODEX » dans les classes RTTI et les textures (`basic_interface\rodexsystem\renewal\*`).
  Icônes de pièce jointe réutilisées par le plugin — elles couvrent exactement les trois cas du
  masque `MAIL_TYPE` : `icon_zeny.bmp` (0x01021e9e), `icon_item.bmp` (0x01022ad6),
  `icon_zeny_n_item.bmp` (0x01022a8e). ⚠ Ces strings de l'exe sont stockées **sans** le dossier de
  tête (`유저인터페이스`, que le code natif prépend) : le plugin l'emprunte à la string du btnbar
  (0x010357b8) plutôt que de le recopier. Statut de courrier disponible aussi, non utilisé :
  `icon_status_mail_read` / `_received` / `_returned.bmp`.

Objectif : remplacer la fenêtre native RODEX par une fenêtre **ImGui** (skin RO), sur le modèle
*state-driven* : lire directement l'état déjà agrégé dans `g_RodexMgr` et émettre les mêmes commandes
que la fenêtre native (paquets CZ construits en clair, ou messages `GameMode`). Voir §7.

---

## 1. Classes, vtables, fonctions (Ghidra, renommées)

| Classe | rôle | window id | fonctions clés |
|---|---|---|---|
| **CRodexSystemMgr** | singleton d'état (les 3 boîtes) | — | `CreateInstance` **0x007c7070** (new 0x38) ; singleton **`g_RodexMgr` = DAT_0131ecdc** |
| **UIRodexWnd** | fenêtre **liste/inbox** | **0x107** | ctor **0x007cd7d0**, OnCreate **0x007ce340**, DrawContent **0x007cf7c0**, OnMsg **0x007d0520**, scalar-dtor 0x007cdea0. vtable **0x01022170** (confirmée live). obj size **0x208** |
| **UIRodexReadWnd** | **lecture** d'un courrier + pièces jointes | **0x109** | DrawContent **0x007cae70**, OnMsg **0x007cb460**, dtor 0x007ca950. vtable **0x01021FBC** |
| **UIMailWriteWnd** | **composition** (partagée avec l'ancien mail) | **0x108** (SaveWindowRect) | OnMsg **0x007c9d30**, DrawContent 0x007c8fc0. vtable **0x01021B30** |

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
| +0x1c | byte | ⚠ **« une liste a été reçue »**, PAS « boîte non vide ». Recopie de `paquet+4`, que rAthena remplit **toujours** avec 1 (`WFIFOB(fd, 4) = 1; // Unknown`) : il vaut donc 1 même sur une boîte vide. Une UI qui s'en sert pour choisir entre « Chargement… » et « Aucun courrier » attend indéfiniment |
| +0x20 | `std::map<int64,RodexMail>` head | **boîte Normal** (openType 0) |
| +0x24 | uint | taille (nb de mails) boîte Normal |
| +0x28 | `std::map<int64,RodexMail>` head | **boîte Account** (openType 1) |
| +0x2c | uint | taille boîte Account |
| +0x30 | `std::map<int64,RodexMail>` head | **boîte Return** (openType 2) |
| +0x34 | uint | taille boîte Return |

**Ce que contient chaque boîte** (côté serveur, colonne `type` de la table `mail`, filtrée dans
`clif_Mail_refreshinbox` par `msg->type != type`) :

- **Normal** (`MAIL_INBOX_NORMAL` 0) — les courriers entre joueurs. Seule valeur posée par
  `mail_send` (`mail.cpp`) et par l'envoi côté char-server.
- **Compte** (`MAIL_INBOX_ACCOUNT` 1) — prévu pour le courrier adressé au **compte** plutôt qu'au
  personnage (récompenses, cash shop). ⚠ **Aucun chemin du code moonlight/rAthena ne pose cette
  valeur** : l'onglet reste vide sauf insertion SQL directe avec `type = 1`.
- **Retournés** (`MAIL_INBOX_RETURNED` 2) — posée par `mapif_parse_Mail_return`
  (`int_mail.cpp:553`) : courrier renvoyé à son expéditeur, manuellement ou par le minuteur
  `mail_return_days`. Le minuteur `mail_delete_days` les supprime ensuite définitivement.

Les 3 `std::map` sont créées vides dans `CreateInstance` (nœud sentinelle `operator_new(0x1b0)` par map).
Chaque map est indexée par **mailID (int64)**, ordre croissant → le plus récent = nœud le plus à droite
(`Rodex_MapNewestMailId`). `Game_InitAllManagers` appelle `CreateInstance` au boot.

### Nœud de map / struct `RodexMail`

Nœud MSVC : `{L@0, P@4, R@8, color@0xc, isnil@0xd}`, **clé int64 @+0x10**, **valeur `RodexMail` @+0x18**
(nœud complet = **0x1b0** octets → valeur ≈ 0x198). Champs prouvés (offsets **relatifs au nœud**) :

| Offset nœud | idx int | type | rôle |
|---|---|---|---|
| +0x10 | [4]/[5] | int64 | **mailID** (clé ; low [4], high [5]) |
| +0x18 | — | byte | **`IsRead` du SERVEUR** — recopié depuis l'entrée de liste (`paquet+8`), remis à 1 par le handler 0x0B63. ⚠ **N'indique PAS que le contenu est présent** : un courrier lu hier arrive à 1 avec un nœud vierge (voir l'encadré ci-dessous) |
| +0x19 | — | byte | **MAIL_TYPE** (masque rAthena) : `&2` **zeny joint**, `&4` **objets joints**, `&8` NPC. Remis à jour par le serveur à chaque récupération ⇒ c'est bien « reste à récupérer ». `&6 != 0` **interdit la suppression** (`MSI 0xa34`) |
| +0x1c | [7] | `std::string` | **nom de l'expéditeur** (24 o) |
| +0x34 | [0xd] | time_t 32 | date envoyée par le serveur, **inutilisée** par le client |
| +0x38 | [0xe] | time_t 32 | **date d'expiration** — c'est CE dword que `DrawRowExpiry` passe à `difftime32(expire, now)` ; +0x34 n'est qu'un calcul mort |
| +0x3c | [0xf] | `std::string` | **titre/sujet** (24 o) |
| +0x54 | [0x15] | `std::string` | **corps du message** (24 o) — vide tant que le courrier n'a pas été ouvert. ⚠ Les sauts de ligne y sont des **NUL internes** (`"ligne1\0ligne2"`), la taille de la `std::string` est donc la seule mesure fiable |
| +0x6c | — | byte | **nombre de pièces jointes** (≤ 5) |
| +0x6d + 60·i | — | bloc 60 o | **pièce jointe i** (voir ci-dessous) |
| +0x1a0 | [0x68]/[0x69] | int64 | **zeny joint** |
| +0x1a8 | — | byte | **marqué obsolète** (1 ⇒ retiré par `Rodex_PurgeStaleMails`) |
| +0x1ac | [0x6b] | int | flag lu par la liste et le clic-sujet (0 ⇒ avertissement `MSI 0xf1b` avant lecture) |

Le corps et les pièces jointes sont remplis à la lecture (**ZC_ACK_READ_RODEX 0x0B63**, handler
`Recv_ZC_AckReadRodex_0x0B63` **0x00cfd0c0**). Bloc d'une pièce jointe (offsets relatifs au bloc,
déduits de la recopie vers un `ItemSkillInfo` : amount → ISI+0x10, refine → ISI+0x60) :

Le bloc porte **tout** ce qu'un `ItemSkillInfo` porte — le handler en fait d'ailleurs
littéralement un, champ par champ (`ItemSkillInfo_ctor` + `SetId` en **0x00cfd2ef**, puis les
`mov` jusqu'en **0x00cfd46d**), avant de le passer à sa fenêtre de lecture. C'est ce qui permet
de nommer et de décrire une pièce jointe **exactement** comme un item d'inventaire : on refait
la même conversion, et le name-builder / la fenêtre de description acceptent le résultat.

| Offset bloc | → ItemSkillInfo | type | rôle |
|---|---|---|---|
| +0 | +0x10 | u16 | quantité |
| +2 | *`SetId`* | u32 | **itemId** |
| +6 | +0x5c | u8 | identifié |
| +7 | +0x5d | u8 | **équipement CASSÉ** (ombre rouge du nom) |
| +8 / +12 / +16 / +20 | +0x1c | u32 ×4 | **cartes** (ou données de forge, cf. le critère `id ≤ 500`) |
| +24 | +0x08 | u32 | emplacement d'équipement (masque `EQP_*`) |
| +28 | +0x00 | u8 | **type d'item** — ⚠ le name-builder s'en sert pour décider s'il décore le nom (`ItemTitle_IsDecoratedType`) : sans lui, une arme cardée sort avec son nom nu |
| +29 | +0x70 | u16 | viewID |
| +31 | +0x64 | u16 | (posé par le handler, non exploité) |
| +33 + 5·i | +0x9c | 5 × {i16 index, i16 value, u8 param} | **options aléatoires** (enchants). ⚠ Le natif *compte* les entrées d'`index` non nul (cascade de `cmovz` en **0x00cfd3a1**) puis recopie les **N premières** — ce n'est équivalent que parce que le serveur tasse ses options depuis l'entrée 0 |
| +58 | +0x60 | u8 | **refine** |
| +59 | +0x88 | u8 | grade (enchant grade) |

### ⚠ Le handler de liste actif est celui de **0x0AC2**, pas 0x09F0

Le serveur choisit l'opcode selon le PACKETVER (`clif_Mail_refreshinbox`) : `0x09f0` < 20160601,
`0x0a7d` < 20170419, **`0x0ac2` au-delà** — donc **0x0AC2** pour 20250716. Les deux handlers
existent dans le client et construisent le **même** `RodexMail` (offsets de nœud identiques,
vérifiés live), mais leurs **formats de paquet diffèrent** :

| | 0x09F0 (ancien) | **0x0AC2 (actif)** |
|---|---|---|
| Handler | `Recv_ZC_MailList_0x09F0` 0x00cfc070 | **`Recv_ZC_MailList_0x0AC2` 0x00cfa6e0** (inline case 0x00ca988f) |
| En-tête | 7 o : `+4 openType, +5 cnt, +6 IsEnd` | **5 o** : `+4` = 1 (« Unknown », constant côté rAthena) |
| Entrée | stride `44 + titleLen` | stride **`41 + titleLen`** : `+0` type, `+1` mailID int64, `+9` **IsRead**, `+10` MAIL_TYPE, `+11` expéditeur (24), `+35` délai d'expiration, `+39` u16 longueur du titre, `+41` titre |
| Fin | — | si `paquet+4 == 1` : pose `g_RodexMgr+0x1c = 1` puis `FindWindow(0x107)` + msg 0x17 |

Le compteur de non-lus (`g_RodexMgr+0x18`) est **incrémenté** ici pour chaque courrier inséré dont
`IsRead` vaut 0. Analyser 0x09F0 reste utile (même constructeur de `RodexMail`), mais toute
conclusion tirée de **l'en-tête** de ce paquet est fausse pour le client actuel.

### ⚠ Le contenu d'un courrier n'existe pas tant qu'il n'a pas été demandé

C'est **le** piège de ce système, et il coûte cher : il produit des pièces jointes plausibles
mais entièrement inventées.

`Recv_ZC_MailList_0x09F0` (**0x00cfc070**) crée les nœuds via
`Rodex_BuildMailFromListEntry` (**0x00ce8e80**), qui ne renseigne **que l'en-tête** :
`IsRead`, `MAIL_TYPE`, expéditeur, expiration, titre — plus un corps `std::string` construit
**vide**. Tout ce qui s'étend de `nœud+0x6c` (nombre d'objets) à `nœud+0x1a8` — **blocs
d'objets et zeny compris** — reste **non initialisé** : ce sont des octets de pile résiduels.
Seul `Recv_ZC_AckReadRodex_0x0B63` (**0x00cfd0c0**) les écrit.

Il n'existe **aucun champ natif** qui dise « le contenu est arrivé ». Le client n'en a pas
besoin : sa fenêtre de lecture n'est ouverte *que* par le handler du contenu, donc ce qu'elle
affiche est forcément frais. Et `+0x18` ne peut pas servir de substitut — c'est l'état `IsRead`
**du serveur**, qui traverse les sessions alors que le contenu, lui, ne survit pas au
`logout`.

**Conséquence pour toute UI tierce** : il faut tenir cet état soi-même. `rodex_tweaks` détourne
`0x00cfd0c0`, y relève le `mailID` (`paquet+5`) et n'affiche corps, zeny et pièces jointes que
pour les courriers enregistrés — sinon il annonce la pièce jointe sans en inventer la valeur.
Le registre est purgé quand `g_RodexMgr` change de pointeur (nouvelle entrée en jeu ⇒ maps
neuves). À noter : un mail **déjà présent** dans la map n'est pas réinséré par un
rafraîchissement (`lower_bound` le trouve et le handler saute l'entrée), donc un contenu déjà
reçu survit à un refresh.

Les 5 blocs (0x6d → 0x199) tiennent entre l'en-tête et le zeny (+0x1a0) : nœud complet **0x1b0**.

---

## 3. `UIRodexWnd` (inbox) — contrôles & commandes

### Layout (extrait, membres = `this[idx]`, offset = idx*4)

| Membre | contrôle | ctrl-id (`SetId`) | strId | rôle |
|---|---|---|---|---|
| this[0x4c] | onglet | — (toggle msg 0xd) | **0xddb** | **openType 0 — Normal** |
| this[0x4d] | onglet | — | **0xdda** | **openType 1 — Account** |
| this[0x4e] | onglet | — | **0xddc** | **openType 2 — Return** |
| this[0x4f] | UIBitmapButton | **0x19f** | 0xb21 | **Rafraîchir** (purge + CZ 0x0AC1) |
| this[0x50] | UIBitmapButton | **0x131** | 0x402 | **Écrire** — ouvre la compose (→ `CMode::SendMsg 0x10c`) |
| this[0x57..] | **6 lignes** (stride 0x2c) | 0x143 / 0x142 / 0x136 | 0xa8e / 0xa8d / 0xb20 | par ligne : toggle sélection + bouton **expéditeur** (id 0x142) + bouton **sujet** (id 0x143) + **bouton retour** (id 0x136) |
| this[99] | UITextButton | **0xd3** / **0x222** | 0xe04 / 0xe05 | pied — **supprimer** (sélection / tout) |
| this[100] | UITextButton | **0x223** / **0x224** | 0xe07 / 0xe08 | pied — **récupérer** (sélection / tout) |
| this[0x65] | UIBitmapButton | **0x13c** | 0x511 | page ◀ |
| this[0x66] | UIBitmapButton | **0x13d** | 0x512 | page ▶ |
| this[0x70..] | 2 onglets catégorie | 0x141 | 0xc78/0xc79 | filtres (tex `rodexsystem\renewal\…`) |
| this[0x72] | **UIEdit** | — | — | **recherche** (maxlen `+0x88`=0x10) |
| this[0x73] | UIBitmapButton | **0x141** | — | bouton recherche |
| this[0x74] | onglet | — | 0xddd | onglet « tous » |

État de pagination : `this+0xe8/+0xec` (bornes bas), `this+0xf4` (borne haut) ; liste de pièces jointes
en attente `this+0xf0` (nœud +0xf4 = count) ; `this+0xb8` = `std::string` (destinataire pré-rempli).

État de pagination : `this+0x19c` = **index de page** (6 lignes par page) ; vecteurs d'index
`{u32 openType; int64 mailID}` (stride 16) reconstruits par `UIRodexWnd_RebuildAllTabIndex`
(0x007d2410) : `+0x1e4/+0x1e8` Normal, `+0x1f0/+0x1f4` Compte, `+0x1fc/+0x200` Retour,
`+0x1d8/+0x1dc` onglet « tous » ; `this+0x1d4` = onglet « tous » actif ;
`this+0x174..+0x188` = les 6 toggles de sélection de ligne (index de ligne à `toggle+0xec`).

### OnMsg (`0x007d0520`) — commandes → paquets

Garde d'entrée : **inerte** si `g_MailWriteWnd` ou `g_RodexReadWnd` est ouverte ; si
`g_RodexMgr+0x1c == 0` (boîte vide) seul `0xc9` passe.

`param_2 == 6` (clic bouton), `switch(param_3)` :

| ctrl-id | action | paquet émis / effet |
|---|---|---|
| **0xc9** | fermer | `SaveWindowRect(0x107)`. |
| **0xd3** | **supprimer** la sélection | `Rodex_DeleteMailById(openType, mailID)` par ligne cochée, après confirmation `MSI 0xb26`. Échec ⇒ chat `MSI 0xa34` (pièces jointes restantes). |
| **0xd5** | (re)libelle le pied | `MSI 0xe04`/`0xe07` s'il y a une sélection, sinon `0xe05`/`0xe08` (⇒ ctrl 0x222/0x224). |
| **0xd7** | **changer d'onglet** | pose `g_RodexMgr+0x10` = 0/1/2 selon `this[76..78]`, ou `this+0x1d4=1` pour « tous » ; remet la page à 0 et décoche tout. |
| **0x131** | **écrire** | `CMode::SendMsg(0x10c, 0)` ⇒ CZ 0x0A08 (sans destinataire). |
| **0x136** | **retourner** à l'expéditeur | **CZ 0x0B98** `{u16; u32 mailID_low}` = **6 o**. Boîte **Normal** uniquement. |
| **0x13c** / **0x13d** | page ◀ / ▶ | **local** (`this+0x19c` ∓ 1), **aucun paquet** ; ▶ gardé par « il reste des courriers ». |
| **0x141** | recherche | filtre par expéditeur (`this[112]`) ou par titre (`this[113]`), texte `UIEdit this[114]`. |
| **0x222** | supprimer **tout** | confirmation (`MSI 0xe05` + nom de boîte) → `Rodex_DeleteSelectedMails`. |
| **0x223** / **0x224** | **récupérer** (sélection / tout) | confirmation `MSI 0xe09`/`0xe0a` → `Rodex_ClaimAttachments(openType, lo, hi)`. |
| **0x19f** | **rafraîchir** | `Rodex_PurgeStaleMails(g_RodexMgr)` puis **CZ 0x0AC1** (26 o). |

Autres messages :
- **`param_2 == 0x17`** : rebuild complet (index des onglets + contenu + états de boutons).
- **`param_2 == 0x3c`** : repaint.
- **`param_2 == 0x62`** (clic sur une ligne) : ctrl **0x142** = clic sur l'**expéditeur** →
  compose vers lui (`CMode::SendMsg(0x10c, nœud+0x1c)`) ; ctrl **0x143** = clic sur le **sujet**
  → **lire** (`CMode::SendMsg(0xc2, mailID_low, openType)` ⇒ CZ_REQ_READ_RODEX 0x09EA), précédé
  d'un avertissement `MSI 0xf1b` si `nœud+0x1ac == 0`.
- défaut → `UIWindow_OnMsg_Default`.

### DrawContent (`0x007cf7c0`)

Blitte `bg_rodex_list.bmp`. Si boîte vide (`g_RodexMgr+0x1c≠0`) → `img_notice.bmp` + `MSI 0xc76/0xc77`.
Sinon boucle **6 lignes** (idx 0..5) sur la map de l'onglet courant (`+0x10`→ boîte `+0x20/+0x28/+0x30`),
et par ligne appelle `DrawRowStatusIcons` (icônes lu/objets), `DrawRowExpiry` (date), `UpdateRowLinkButtons`
(2 colonnes de liens : expéditeur `nœud+0x1c`, titre `nœud+0x3c`). Compteur de page en bas (`MSI 0xbda`).

---

## 4. `UIRodexReadWnd` (lecture) — OnMsg `0x007cb460`, DrawContent `0x007cae70`

Créée par le handler du contenu (`MakeWindow(0x109)` à la fin de `Recv_ZC_AckReadRodex_0x0B63`),
puis nourrie par un `msg 0x17`. Champs (`this+…`), tous confirmés par son `DrawContent` :

| Offset | rôle |
|---|---|
| +0xb8 | `std::string` **expéditeur** (affiché en (8,29), sert aussi de destinataire au bouton « répondre ») |
| +0xd0 | `std::string` **sujet** (affiché en (9,52)) |
| +0xe8 | int64 **zeny joint** (affiché « %s Zeny » en (50,350)) |
| +0xf0 / +0xf4 | `std::list<ItemSkillInfo>` des **pièces jointes** / son count (5 cases dessinées, x = 26 + 34·i) |

`param_2 == 6` (boutons) :

| ctrl-id | action | effet |
|---|---|---|
| **0xbd** | **récupérer les objets** | **CZ 0x09F3** `{u16; int64 mailID (g_RodexMgr+8); u8 openType (+0x10)}` = 11 o. Gardé par `this+0xf4 ≠ 0` (la liste d'objets n'est pas vide). |
| **0xbe** | **récupérer le zeny** | **CZ 0x09F1**, même struct. Gardé par `this+0xe8 > 0`. |
| **0xc9** | fermer | `SaveWindowRect(0x109)` ; efface `g_RodexMgr+8/+0xc` ; `msg 0x3c` à `g_RodexInboxWnd`. |
| **0xd3** | **supprimer ce courrier** | confirmation `MSI 0x164` ; refusé (chat `MSI 0xa34`) si `nœud+0x19 & 6` ; sinon `CMode::SendMsg(0xc4, mailID_low, openType)` ⇒ **CZ_REQ_DELETE_MAIL 0x09F5**, puis fermeture. |
| **0x131** | **répondre** | `CMode::SendMsg(0x10c, this+0xb8)` (nom de l'expéditeur). Gardé par `this+0x10c ≠ 1`. |
| **0x136** | retourner à l'expéditeur | **CZ 0x0B98** `{u16; u32 mailID_low}` = 6 o. |

`param_2 == 0x17` : (re)construit `this+0xf0` depuis les 5 slots `ItemSkillInfo` de la session
(`ItemSkillInfo_CopyFull`, **max 5**) — ce sont les slots que `sub_D7F480` remet à zéro à chaque
nouvelle lecture. `param_2 == 0x6a` : timer, invalide `this+0x108`.

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

> ⚠ La colonne « nom » de `opcode_map.md` est heuristique (déduite des tailles) et se trompe sur
> RODEX. Ci-dessous = comportement **prouvé côté client** : 0x09F1/0x09F3 sont bien les deux
> demandes de **récupération** (zeny / objets), pas de la pagination — celle-ci est locale.

### Client → serveur (CZ) — construits **en clair** dans l'UI (pinnés)

| Opcode | taille | construit par | struct |
|---|---|---|---|
| **0x0AC1** | 26 | liste OnMsg **0x19f** (et `UIRodexWnd_ctor`) | `{u16; int64 newest_Normal; int64 newest_Return; int64 newest_Account}` — **ouvrir / rafraîchir** (annonce le plus récent déjà connu de chaque boîte ; 0 si vide) |
| **0x09F3** | 11 | lecture OnMsg 0xbd, `Rodex_ClaimAttachments` (`type & 4`) | `{u16; int64 mailID; u8 openType}` — **récupérer les OBJETS** |
| **0x09F1** | 11 | lecture OnMsg 0xbe, `Rodex_ClaimAttachments` (`type & 2`) | `{u16; int64 mailID; u8 openType}` — **récupérer le ZENY** |
| **0x0B98** | 6 | liste/lecture OnMsg 0x136 | `{u16; u32 mailID_low}` — **retourner à l'expéditeur** (32 bits bas seulement). ⚠ **Sans effet sur le serveur** : voir ci-dessous |
| **0x0B97** | 27 | write OnMsg 0xd5 | `{u16; char name[24]; u8 sex}` — **vérifier destinataire** |

### Client → serveur (CZ) — via `GameMode::SendMsg` (opcode non construit dans l'UI, cf. `opcode_map`)

| Opcode | taille | déclencheur | parseur rAthena |
|---|---|---|---|
| **0x09EA** | 11 | **lire** (liste : clic sujet → `CMode::SendMsg` **cmd 0xc2**) | `clif_parse_Mail_read` → `rodex_read_mail` |
| **0x0A08** | 26 | begin compose (inbox 0x131/0x50 → GameMode 0x10c) | `clif_parse_Mail_beginwrite` |
| **0x0A04** | 6 | attacher objet (compose msg 0x17) | `clif_parse_Mail_setattach` |
| **0x09EC** / **0x0A6E** | VAR | envoyer (write 0x159) | `clif_parse_Mail_send` → `rodex_send_mail` |
| **0x09F5** | 11 | **supprimer** (liste 0xd3/0x222, lecture 0xd3 → `CMode::SendMsg` **cmd 0xc4**) | `clif_parse_Mail_delete` |
| **0x0A03** | 2 | annuler compose | `clif_parse_Mail_cancelwrite` |
| **0x0A06** | 6 | ouverture boîte (NPC/keybind) | `clif_parse_Mail_winopen` |
| **0x09E8** / **0x09EE** / **0x09EF** | 11 | refresh inbox | `clif_parse_Mail_refreshinbox` |
| **0x0A13** | 26 | check name (variante) | `CZ_CHECKNAME1` |

### Serveur → client (ZC) — handlers = **thunks de jump-table** `0x00caa2e0[opcode−0x73]` (région « Lotus », non typés par Ghidra ; peuplent `g_RodexMgr`)

Les thunks tail-call vers les fonctions de traitement suivantes (renommées/commentées Ghidra) :

| Opcode | taille | handler (renommé) | rôle |
|---|---|---|---|
| **0x09F0** | VAR (base 7) | **`Recv_ZC_MailList_0x09F0`** (0x00cfc070) | **liste inbox** → pose openType/empty, **insère chaque mail** dans la map (`FUN_00ce8d70`), résout le sexe expéditeur, rafraîchit `g_RodexInboxWnd` |
| **0x09EB** | VAR (base 24) | **`Recv_ZC_ReadRodex_0x09EB`** (0x00cfbe00) | **ack de récupération** : efface le bit OBJETS du MAIL_TYPE (`mail+0x19 -= 4`) ; msg 0x17→`g_RodexReadWnd`, msg 0x3c→`g_RodexInboxWnd` |
| **0x0B63** | VAR (base 24) | **`Recv_ZC_AckReadRodex_0x0B63`** (**0x00cfd0c0**, thunk 0x00ca8ed1) | **contenu du courrier lu** — c'est LUI qui remplit le corps (`nœud+0x54`), le nb d'objets (`+0x6c`), les blocs objets (`+0x6d`, stride 60) et le zeny (`+0x1a0`), pose `+0x18 = 1`, décrémente `g_RodexMgr+0x18`, puis `MakeWindow(0x109)` + msg 0x17. **Seul écrivain du contenu ⇒ seul point de détour fiable pour savoir qu'un courrier est chargé** |
| **0x0A05** | 63 | *(add-item)* | **ZC_ACK_ADD_ITEM_RODEX** — ack pièce jointe (compose) |
| **0x0B3F** | 64 | *(variante récente)* | ZC_ACK_ADD_ITEM_RODEX |
| *(suppr.)* | — | **`Recv_ZC_RodexDeleteResult`** (0x00cf8b10) | `__stdcall(int mailID_lo, int result)`, `result == 0` = accepté. Ferme `g_RodexReadWnd`, décrémente non-lus, efface le nœud, retire la ligne inbox. ⚠ **Ne cherche le nœud que dans la map Normal** (`g_RodexMgr+0x20`), quelle que soit la boîte réelle : un courrier supprimé ailleurs reste dans la map jusqu'au rafraîchissement |
| *(begin-write)* | — | **`Recv_ZC_RodexBeginWriteResult`** (0x00cfcc80) | ouvre la compose (`MakeWindow(0x108)`) si `g_MailWriteWnd==0` |
| **0x09ED** | 3 | | ack (résultat écriture/suppression) |
| **0x09F2** / **0x09F4** | 12 | | ack (page / attach) |
| **0x09F6** | 11 | | ack (zeny réclamé) |
| **0x0A07** | 9 | | ack (objet réclamé) |
| **0x0A12** | 27 | | réponse check-name (détail) |
| **0x0A14** | 10 | | `ZC_CHECKNAME` — variante courte (PACKETVER < 20160302), sans le nom |
| **0x0A51** | 34 | **`Rodex_ApplyRecipientCheckAck`** (**0x00d00010**) | **ZC_CHECKNAME** — réponse à la vérification du destinataire |

Dispatch : `RecvLoop_DispatchPackets` (0x00c9df00) → `handler = *(0x00caa2e0 + (opcode−0x73)*4)`.

### « Retourner à l'expéditeur » (0x0B98) — corrigé côté serveur le 2026-07-26

Le paquet du client est **correct** (`PACKET_CZ_RODEX_RETURN {int16 packetType; uint32 msgId}`,
6 o) et le plugin l'émet à l'identique du natif. Mais `clif_parse_Mail_return` (`clif.cpp`) sort
sans rien faire dès que le PACKETVER est moderne :

```c
#if PACKETVER_MAIN_NUM >= 20201104 || PACKETVER_RE_NUM >= 20211103 || PACKETVER_ZERO_NUM >= 20201118
    int32 mail_id = p->msgId;
    // not supported for now
    return;                    // <- sortie inconditionnelle, avant toute vérification
#else
```

Le bouton ne marche donc ni en ImGui **ni en natif** : inutile de chercher la cause côté client.
Le reste de la chaîne existe pourtant déjà — `intif_Mail_return`, puis `mapif_parse_Mail_return`
(`int_mail.cpp:553`) qui pose `MAIL_INBOX_RETURNED` et alimente l'onglet « Retournés ». Retirer ce
`return;` suffirait à retomber sur le chemin commun, qui contrôle déjà `mail_id > 0`,
`mail_invalid_operation(sd)` et refuse les courriers système (`send_id != 0`, sinon
`clif_Mail_return(fd, mail_id, 1)` = échec).

**Pourquoi ce garde ?** Pas à cause des courriers système : ceux-là sont déjà refusés deux fois
(`send_id != 0` côté map, `if (msg.send_id == 0) return;` côté char — « *If it was sent by the
server we do not want to return the mail* »). Le diff d'origine (`93d97b7ef4`, « Initial support
for 2021-11-03RagexeRE ») montre la vraie raison : la struct s'y appelait
**`PACKET_CZ_UNCONFIRMED_RODEX_RETURN`** — convention rAthena pour un format *déduit mais non
vérifié* — avec un `//ShowDump( p, sizeof( p ) );` laissé pour inspection. Agir sur un `mailID`
mal décodé aurait renvoyé le mauvais courrier, d'où le blocage. Depuis, `55f3807099` a renommé la
struct **sans** `UNCONFIRMED` : le format est confirmé, mais personne n'est revenu retirer le
`return;`. Garde temporaire devenu permanent par oubli — le code sous-jacent, lui, tourne depuis
des années sur la branche `< 20201104`.

**Second défaut, moins visible** : la réponse `clif_Mail_return` part en **ZC 0x0274**, que ce
client **ne dispatche pas du tout** (absent de sa table de réception → `RecvDispatch_DefaultSkip`).
Même le `return;` retiré, le courrier serait resté affiché jusqu'à un rafraîchissement manuel. Le
client attend **ZC 0x0B99** (10 o) :

| Offset | Type | Contenu |
|---|---|---|
| +0 | int16 | 0x0B99 |
| +2 | uint32 | **mailId** |
| +6 | uint32 | **result** — `0` = succès |

…et il le route vers **`Recv_ZC_RodexDeleteResult`** (0x00cf8b10), le handler de l'ack de
suppression (case 0x00ca9ee3). Un retour réussi fait donc tout le ménage local tout seul : nœud
effacé, fenêtre de lecture fermée, ligne retirée. Corollaire pour le plugin : le détour posé sur ce
handler capte le retour **comme** une suppression — aucun code supplémentaire, le courrier
disparaît de la liste ImGui à l'instant.

**Correctif appliqué au fork `moonlight` (2026-07-26)** — `src/map/clif.cpp` + `src/map/packets.hpp` :
retrait du `return;`, ajout de `PACKET_ZC_RODEX_RETURN` (0x0b99) et bascule de `clif_Mail_return`
sur ce paquet pour les PACKETVER ≥ 20201104 (0x0274 conservé pour les anciens).
**✅ Testé sur le serveur de test** : le retour aboutit et le courrier quitte la liste sans
rafraîchissement. Le format 0x0B99, déduit du désassemblage, est donc confirmé sur le fil.

### Vérification du destinataire (0x0B97 → 0x0A51)

Aller : `PACKET_CZ_CHECKNAME2` **0x0B97**, 27 o `{int16; char Name[24]; char own_char}`.
Le serveur **ignore `own_char`** (`clif_parse_Mail_Receiver_Check` ne lit que `Name`).

Retour : `PACKET_ZC_CHECKNAME` **0x0A51**, 34 o.

| Offset | Type | Contenu |
|---|---|---|
| +2 | int32 | **CharId** — **0 = nom inexistant** (seul signal d'erreur du protocole) |
| +6 | int16 | **Class** (job id) |
| +8 | int16 | **BaseLevel** |
| +10 | char[24] | **Name**, réémis par le serveur |

Il n'y a **pas de code d'erreur** : `mapif_parse_Mail_receiver_check` fait un
`SELECT char_id, class, base_level FROM char WHERE name = '…'` et renvoie `0/0/0` si rien ne sort.
Si la cible est en ligne sur le même map-server, la réponse part sans passer par le char-server.
**Deux cas ne produisent aucune réponse** — carte avec le flag `NORODEX` (le serveur répond par un
message de chat) et char-server injoignable : une UI ne doit donc pas conclure « inexistant » sur
un simple silence.

Côté client, `Rodex_ApplyRecipientCheckAck` (**0x00d00010**, `__stdcall`, `retn 0x10`, les deux
`int16` poussés en dwords) affiche **MSI 0xA37** — *« The recipient's name does not exist. »* — si
`CharId == 0`, sinon saute à `Rodex_FillRecipientInfo` (**0x007c7d20**) qui pose le métier
(`Job_GetDisplayNameOrResName` 0x00d5bb40), le niveau (« Lv%d ») et range `CharId` à `wnd+0xcc`.

⚠ **L'envoi n'utilise pas ce `CharId`** : `clif_parse_Mail_send` repart du **nom** en `+4`, le
`<char id>.L` en `+64` ne servant qu'à décaler titre et corps sur les PACKETVER > 20160330. La
vérification est donc un confort, pas un prérequis. En revanche `sd->state.mail_writing` doit être
vrai (session ouverte par CZ 0x0A08), sinon l'envoi est ignoré **en silence**.

---

## 7. Remplacement ImGui — `src/features/windows/rodex_window.{h,cc}` (LIVRÉ)

**Principe** : *state-driven*, aucun parsing de paquet. Tout l'état des 3 boîtes est déjà agrégé
dans `g_RodexMgr` par le natif ; le plugin **copie** ces maps à chaque tick et **émet** les mêmes
commandes que les fenêtres natives. Défaut **OFF** (opt-in), setting `rodex_imgui`
(MoonlightUi → « Interface de jeu » → *Courrier Moonlight®*).

Périmètre : les **trois** fenêtres sont masquées et remplacées — la **liste** (0x107) et la
**lecture** (0x109) par une fenêtre unique (liste en haut, courrier sélectionné en bas, qui
s'agrandit de la hauteur du bloc à la lecture), l'**écriture** (0x108) par une fenêtre ImGui
dédiée. Le courrier appartient au groupe « Interface moderne » (`SetModernInterface`) avec
l'inventaire, l'entrepôt, les barres et l'échange : ses **pièces jointes se glissent depuis
l'inventaire ImGui** (payload `INV_ITEM`, comme l'échange et le doll de la feuille de perso).

La fenêtre native d'écriture reste **vivante** derrière : elle porte les frais d'envoi calculés
(`+0xf8`) et le char id du destinataire vérifié (`+0xcc`), et sa fermeture (`SaveWindowRect 0x108`)
annule proprement la session d'écriture côté serveur — on délègue donc l'annulation au natif.

- **Lecture d'état** (`ReadState`, thread principal) : parcours itératif des 3 arbres
  (`+0x20/+0x28/+0x30`), copie plate de chaque nœud (POD, sous SEH — MSVC interdit `__try` dans une
  fonction à dérouler), puis conversion ANSI→UTF-8 des trois textes. Tri par mailID décroissant.
  Aucun pointeur de nœud n'est conservé d'une frame à l'autre.
- **Commandes émises** (identiques au natif) :
  - **rafraîchir** : `Rodex_PurgeStaleMails(g_RodexMgr)` puis CZ **0x0AC1** (26 o, plus récent de
    chaque boîte = clé du nœud le plus à droite).
  - **lire** : `CMode::SendMsg(0xc2, mailID_low, openType)` ⇒ CZ 0x09EA.
  - **récupérer** : CZ **0x09F3** si `type & 4`, CZ **0x09F1** si `type & 2` (ordre du natif).
  - **supprimer** : `CMode::SendMsg(0xc4, …)` ⇒ CZ 0x09F5 — grisé tant que `type & 6`.
  - **retourner** : CZ **0x0B98** — boîte Normal uniquement.
  - **écrire / répondre** : `CMode::SendMsg(0x10c, nom ou 0)` ⇒ CZ 0x0A08 (le nom est repassé dans
    l'encodage **client**, pas en UTF-8).
- **Écriture** (structures relues dans le serveur, `src/map/clif.cpp` du fork moonlight) :
  - **joindre un objet** : `CMode::SendMsg(0xc3, index, amount)` **puis** `cmd 0x12` — la séquence
    exacte du drop natif (`OnMsg case 0x26`) ; sans le second appel l'objet n'est pas poussé.
    Le client construit alors CZ **0x0A04** `{u16 index; u16 amount}`.
  - **retirer** : CZ **0x0A06** `{u16 index; u16 amount}` (`clif_parse_Mail_winopen`).
  - **vérifier le destinataire** : CZ **0x0B97** `PACKET_CZ_CHECKNAME2 {char name[24]; char own_char}`.
  - **envoyer** : CZ **0x0A6E** construit par le plugin —
    `{op; len; receiver[24]@4; sender[24]@28 (ignoré); zeny u64@52; titleLen@60; textLen@62;
    charId@64; titre puis corps @68}`. Les deux longueurs **incluent le `\0`** (le serveur les
    passe à `safestrncpy`, qui écrit au plus len−1 caractères). Bornes serveur :
    `MAIL_TITLE_LENGTH 40`, `MAIL_BODY_LENGTH 500`, `MAIL_MAX_ITEM 5`.
    ⚠ On ne réplique **pas** la commande native d'envoi (`cmd 0xc0`) : elle attend une structure
    C++ portant trois `std::string` construites par le runtime du client, que notre DLL ne peut
    ni allouer ni libérer sans risque.
  - **annuler** : on ferme la fenêtre native (`SaveWindowRect 0x108`) et c'est elle qui émet
    CZ 0x0A03 et libère les objets — rien à répliquer.
  - **pièces jointes en cours** : 5 `ItemSkillInfo` dans la SESSION à `0x015fa3c0 + 23624 + 248·i`
    (vérifié live : index d'inventaire +4, quantité +0x10, itemId en TEXTE +0x2c, refine +0x60).
    C'est la même source que la liste de la fenêtre native, et ce que `sub_D7F480` remet à zéro.
  - **frais d'envoi** : lus dans la fenêtre native (`+0xf8`), jamais recalculés — le taux et le
    prix par pièce jointe sont de la config serveur (`mail_zeny_fee`, `mail_attachment_price`).
    Limite connue : le natif ne les recalcule que sur SA saisie, donc le montant peut rester à 0
    tant qu'on n'écrit que dans la fenêtre ImGui.
- **Masquer le natif** : `wnd+0x28 = 0` (jamais hors-écran, [[feedback_no_offscreen_hide]]), à la
  création via le hook `MakeWindow` de `window_pos_tweaks` (la lecture est créée par le handler de
  paquet, donc entre deux ticks) **et** à chaque tick. Décocher le réglage rend les fenêtres
  natives visibles au lieu de les laisser cachées. Le badge de non-lus (`g_RodexMgr+0x18` →
  `UIMenuIconWnd 0x133`) reste piloté par le natif.
- **Filtre de liste** : champ ImGui au-dessus de la table, insensible à la casse, portant sur
  l'expéditeur **ou** le sujet. Purement local — contrairement au natif (contrôle 0x141, qui
  impose de choisir la colonne), il n'émet rien : la liste complète est déjà en mémoire. Les
  compteurs d'onglets restent ceux des boîtes, ils ne fondent pas pendant la saisie.
- **Reste à faire** : faire recalculer les frais d'envoi au client quand le zeny est saisi
  côté ImGui.

**Réutilisés** : toolkit `ro_imgui` (BeginRoWindow/RoButton/RoPopupModal), `ro::ItemIcon`
([[feedback_texture_cache_device_epoch]]), résolveur de nom d'item (cf. shop/trade), accents FR
([[feedback_french_ui_accents]]). Doc frère : [[project_trade_window_re]] (`docs/trade_window_re.md`),
même patron *masquage + émission de commandes*.
