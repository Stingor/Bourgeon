# Banque de zeny (Ctrl+B) — reverse engineering

Cible : `2025-07-16_Ragexe_175220998_clientinfo.exe` (imagebase `0x400000`).
Serveur : `Moonlight-Rathena` / `moonlight` (fork rAthena).

La « banque » est un coffre à zeny **par compte** (et non par personnage), distinct
du storage Kafra et du cart. Elle n'a aucun slot d'objet : uniquement un solde
64 bits côté paquet, plafonné à `INT_MAX` de bout en bout.

---

## 1. Vue d'ensemble du flux

```
Ctrl+B  ──►  UIWindowMgr_DispatchHotkeyBehavior, behavior 146   (0x00A451E0)
              │
              ├─ g_pUIBankWnd != 0  ──►  SaveRectAndCloseWindow(275)      [fermeture locale, aucun paquet]
              │
              └─ sinon  ──►  CZ_REQ_BANKING_CHECK 0x09AB  ────────────────────────┐
                                                                                  ▼
                                                             clif_parse_BankCheck (serveur)
                                                             · feature.banking
                                                             · mapflag MF_NOBANK
                                                                                  │
              ┌───────────────────────────────────────────  ZC_BANKING_CHECK 0x09A6
              ▼
   Recv_ZC_BANKING_CHECK (0x00CAEFE0)
   · g_BankVault ← Money
   · SaveRectAndCloseWindow(275) ; si rien n'était ouvert → MakeWindow(0x113)
```

**Le client n'ouvre jamais la fenêtre de lui-même.** C'est la réception de
`ZC_BANKING_CHECK` qui l'ouvre. Corollaire : `MakeWindow(275)` appelé
directement (depuis un plugin) affiche un solde périmé — il faut passer par
`CZ_REQ_BANKING_CHECK`.

Autre corollaire : `ZC_BANKING_CHECK` **bascule** l'état de la fenêtre. Si un
script serveur l'envoie alors que la banque est déjà ouverte, elle se **ferme**.

---

## 2. Opcodes

| Opcode | Sens | Len | Structure | État client |
|---|---|---|---|---|
| `0x09AB` CZ_REQ_BANKING_CHECK | CZ | 6 | `u16 op; u32 AID` | **émis** (Ctrl+B, `ZC_UI_OPEN` type 0) |
| `0x09A6` ZC_BANKING_CHECK | ZC | 12 | `u16 op; s64 Money; u16 Reason` | **traité** → `0x00CAEFE0` |
| `0x09A7` CZ_REQ_BANKING_DEPOSIT | CZ | 10 | `u16 op; u32 AID; u32 montant` | **émis** (bouton 462) |
| `0x09A8` ZC_ACK_BANKING_DEPOSIT | ZC | 16 | `u16 op; u16 Reason; s64 Money; s32 Balance` | **traité** → `0x00CAEDC0` |
| `0x09A9` CZ_REQ_BANKING_WITHDRAW | CZ | 10 | `u16 op; u32 AID; u32 montant` | **émis** (bouton 463) |
| `0x09AA` ZC_ACK_BANKING_WITHDRAW | ZC | 16 | identique à `0x09A8` | **traité** → `0x00CAEED0` |
| `0x09B6` CZ_REQ_OPEN_BANKING | CZ | 6 | — | **JAMAIS émis** |
| `0x09B7` ZC_ACK_OPEN_BANKING | ZC | 4 | — | **ignoré** (dispatch `0x00C9E1DD`, partagé par 34 opcodes = no-op) |
| `0x09B8` CZ_REQ_CLOSE_BANKING | CZ | 6 | — | **JAMAIS émis** |
| `0x09B9` ZC_ACK_CLOSE_BANKING | ZC | 4 | — | **ignoré** |

> ⚠️ Attention au décalage de layout : dans `0x09A6` le solde est en **+2** et la
> raison en **+10** ; dans `0x09A8`/`0x09AA` la raison est en **+2** et le solde
> en **+4**. C'est bien le comportement natif, pas une erreur de RE.

### Conséquence opérationnelle : `feature.banking_state_enforce`

Ce client **n'envoie ni `0x09B6` ni `0x09B8`** (vérifié : aucune référence à ces
opcodes hors de `PacketLenTable_Init`). Donc `sd->state.banking` reste toujours
à 0 côté serveur, et `pc_bank_deposit` / `pc_bank_withdraw` rejetteraient tout.

➜ **`feature.banking_state_enforce` doit rester à `no`** (c'est le cas dans
`conf/battle/feature.conf`). Le passer à `yes` casserait entièrement la banque.

---

## 3. Fenêtre `UIBank_NewWnd`

| Élément | Valeur |
|---|---|
| RTTI | `.?AVUIBank_NewWnd@@` (`0x0123F1C8`) |
| vtable | `0x01030FD4` |
| windowID | **275** (`0x113`) |
| Taille objet | `0xD0` (208 o) |
| Taille fenêtre | 280 × 153 |
| Pointeur caché | `g_UIWindowMgr + 0x448` = **`0x0131F930`** (`g_pUIBankWnd`) |
| Fond | `\bank\bg_bank.bmp` |

### Champs d'instance

| Offset | Contenu |
|---|---|
| `+0xB4` | `UIEdit` de saisie du montant (`this[45]`), max 10 caractères |
| `+0xB8 … +0xCF` | `std::string` du message d'état (buffer / taille `+0xC8` / capacité `+0xCC`) |

### Méthodes virtuelles

| Slot | Adresse | Nom donné |
|---|---|---|
| `+0x00` | `0x0086DF80` | destructeur scalaire |
| `+0x3C` | `0x00874B30` | `UIBankWnd_OnCreate` |
| `+0x50` | `0x0087E4D0` | `UIBankWnd_DrawContent` |
| `+0x94` | `0x00881B50` | `UIBankWnd_OnMsg` |

Constructeur : `UIBankWnd_ctor` `0x0086B760` (via `UIWindowMgr_MakeWindow`, case 182, code à `0x00A3FCA5`).

### Contrôles créés par `OnCreate`

| ID | Bitmap | Position | Rôle |
|---|---|---|---|
| 201 | (bouton fermer standard) | — | ferme la fenêtre |
| 462 | `bank\btn_deposit` | 209, 77 | **Dépôt** |
| 463 | `bank\btn_withdraw` | 209, 102 | **Retrait** |
| 279 | `bank\btn_upper` | 177, 89 | montant **+1** |
| 280 | `bank\btn_buttom` | 177, 99 | montant **−1** |
| 392 | `bank\btn_10mil` | 189, 132 | montant **+100 000** |
| 391 | `bank\btn_100mil` | 141, 132 | montant **+1 000 000** |
| 390 | `bank\btn_1000mil` | 93, 132 | montant **+10 000 000** |
| 389 | `bank\btn_max` | 45, 132 | écrit le libellé `MSI_BANK_MAX` dans l'edit |

> Les noms de bitmaps (`10mil`, `100mil`, `1000mil`) ne correspondent **pas** aux
> deltas réels (1e5 / 1e6 / 1e7). Les libellés affichés sont corrects
> (`+ 100,000`, `+ 1,000,000`, `+ 10,000,000`).

Le bouton **MAX** n'écrit pas un nombre : il écrit le **texte** `MSI_BANK_MAX`
dans l'edit. C'est `UIBankWnd_ValidateAmount` qui reconnaît ce texte et calcule
le montant maximal transférable.

---

## 4. Globales

| Adresse | Nom | Type | Notes |
|---|---|---|---|
| `0x015FFFC0` | `g_BankVault` | `s64` | solde de la banque. Écrit **uniquement** par `0x09A6` / `0x09A8` / `0x09AA` |
| `0x015FBA90` | `g_PlayerZeny` | `s32` | zeny en poche |
| `0x0131F930` | `g_pUIBankWnd` | ptr | non nul ⇔ fenêtre banque ouverte |

Autres lecteurs de `g_BankVault` :
- `UIBankWnd_DrawContent` / `UIBankWnd_ValidateAmount` / `UIBankWnd_OnMsg`
- `0x007F0380` : contrôle de solvabilité du styling shop — si `zeny < prix` mais
  `zeny + banque >= prix`, affiche `MSI_STYLING_SHOP_SUGGEST_TO_GO_TO_BANK` (2820).

---

## 5. Fonctions

| Adresse | Nom | Rôle |
|---|---|---|
| `0x0086B760` | `UIBankWnd_ctor` | constructeur |
| `0x00874B30` | `UIBankWnd_OnCreate` | boutons + UIEdit |
| `0x0087E4D0` | `UIBankWnd_DrawContent` | rendu (titre, 2 lignes de soldes, message d'état) |
| `0x00881B50` | `UIBankWnd_OnMsg` | clics → deltas / envoi des paquets |
| `0x0086ED80` | `UIBankWnd_ApplyAmountDelta` | `_atoi64(edit) + delta`, clamp `[0, INT_MAX]` |
| `0x0086EF80` | `UIBankWnd_ValidateAmount` | parse/valide, gère le cas `MAX`, renvoie 0 = refus |
| `0x00CAEFE0` | `Recv_ZC_BANKING_CHECK` | `0x09A6` |
| `0x00CAEDC0` | `Recv_ZC_ACK_BANKING_DEPOSIT` | `0x09A8` |
| `0x00CAEED0` | `Recv_ZC_ACK_BANKING_WITHDRAW` | `0x09AA` |
| `0x00CAF890` | `UI_OpenByOutUiType` | corps de `ZC_UI_OPEN 0x0A38` |
| `0x00C86150` | `Bank_IsBlockedByOpenWindow` | gardes dépôt/retrait |
| `0x00A94870` | `Cstr_FormatInt64Grouped` | `%I64d` + séparateurs de milliers |
| `0x00A948D0` | `Cstr_FormatInt32Grouped` | idem en 32 bits |

### `UIBankWnd_DrawContent`

```
titre        : MSI_BANK_1 (2771)
y=29  x=30   : MSI_BANK_2 (2772) ; x=43+largeur : Cstr_FormatInt64Grouped(g_BankVault) + " z"
y=46  x=30   : MSI_BANK_3 (2773) ; même colonne : Cstr_FormatInt32Grouped(g_PlayerZeny) + " z"
y=112 centré : std::string this+0xB8 (message d'état), si non vide
```

---

## 6. Ouverture depuis le serveur : `ZC_UI_OPEN 0x0A38`

`UI_OpenByOutUiType(this, outUiType)` (`0x00CAF890`), corps du handler `0x0A38`
(thunk `0x00CA8D1F`) :

| `outUiType` | Effet |
|---|---|
| **0 (banque)** | si la fenêtre 251 n'est pas ouverte → envoie `CZ_REQ_BANKING_CHECK 0x09AB` |
| 1 | `MakeWindow(0x119)` (précédé de `OnMsg 89` sur la fenêtre 16 si ouverte) |
| 2 | `MakeWindow(0x11C)` |
| 3 | `MakeWindow(0x11E)` |
| 9 | `MakeWindow(0x158)` |

Le dispatcher étendu `0x00CF98B0` réutilise `UI_OpenByOutUiType` pour les types 0..4.

> La garde `FindWindow(251)` est un héritage : `0xFB` n'a pas d'entrée dans la
> table de fabrique classique (`g_UIWindowIdToCaseTable[0xFB]` = 232 = cas
> générique CUI par défaut).

Donc `atcommand @bank` / `openbank` côté serveur (qui envoient `ZC_UI_OPEN` type 0)
fonctionnent : ils déclenchent un aller-retour `0x09AB` → `0x09A6`.

---

## 7. Gardes client

### À l'ouverture — `UIWindowMgr_MakeWindow` case 275 (`0x00A3FCA5`)

Chacune abandonne l'ouverture avec un message en chat :

| Fenêtre ouverte | Message |
|---|---|
| 295 (`0x127`) raffinage | `MSI_CANNOT_OPEN_BANKING_DURING_REFINING` (3014) |
| 343 (`0x157`) | `MSI_CANNOT_OPEN_BANKING_DURING_GRADE_ENCHANT` (3717) |
| 10006 (`0x2716`) enchant | `MSI_CANNOT_OPEN_BANKING_DURING_ENCHANT` (3845) |
| 341 (`0x155`) barter étendu | `MSI_CANNOT_OPEN_BANKING_DURING_EXPANDED_BARTERMARKET` (3967) |
| 361 (`0x169`) runes | `MSI_RUNESYSTEM_DURING_CANNOT_OPEN_BANKING` (4055) |

### Au dépôt/retrait — `Bank_IsBlockedByOpenWindow` (`0x00C86150`)

Vrai si l'une des fenêtres **290 (0x122) / 302 (0x12E) / 348 (0x15C) /
10006 (0x2716) / 361 (0x169)** est ouverte → `MSI_BANK_PROHIBIT` (2489),
aucun paquet envoyé.

### Validation du montant — `UIBankWnd_ValidateAmount`

| Cas | Chat | Libellé (this+0xB8) |
|---|---|---|
| saisie vide | `MSI_BANK_NOMSG` 2779 | `MSI_BANK_1_MINI` 2781 |
| non numérique | `MSI_BANK_CHECK_NUM` 2488 | `MSI_BANK_2_MINI` 2782 |
| > `INT_MAX` | `MSI_BANK_MAX_MONEY` 2768 | `MSI_BANK_3_MINI` 2783 |
| ≤ 0 | `MSI_BANK_MIN_MONEY` 2769 | `MSI_BANK_4_MINI` 2784 |
| dépôt avec zeny = 0, ou retrait avec banque = 0 | `MSI_BANK_0_MONEY` 2770 | `MSI_BANK_0_MINI` 2780 |
| mode MAX, dépôt, banque déjà à `INT_MAX` | `MSI_BANK_7_MINI` 2787 *(sic)* | `MSI_BANK_3_MINI` 2783 |
| mode MAX, dépôt, rien de transférable | `MSI_BANK_WARNING` 2465 | `MSI_BANK_0_MINI` 2780 |

En mode **MAX**, le montant est calculé à partir du dépassement
`surplus = max(0, banque + zeny − INT_MAX)` : dépôt → `zeny − surplus`,
retrait → `banque − surplus` (borné par `INT_MAX − zeny`).

### Plafonds dans `UIBankWnd_OnMsg`

| Cas | Chat | Libellé |
|---|---|---|
| dépôt, `zeny < montant` | `MSI_BANK_DEPOSIT_NO_MONEY` 2456 | `MSI_BANK_5_MINI` 2785 |
| dépôt, `banque + montant > INT_MAX` | `MSI_BANK_7_MINI` 2787 *(sic)* | `MSI_BANK_3_MINI` 2783 |
| retrait, `banque < montant` | `MSI_BANK_WITHDRAW_NO_MONEY` 2455 | `MSI_BANK_6_MINI` 2786 |
| retrait, `zeny + montant > INT_MAX` | `MSI_BANK_OVER_INT_MAX` 2459 | `MSI_BANK_7_MINI` 2787 |

---

## 8. Codes de retour (`Reason`)

### `ZC_BANKING_CHECK 0x09A6`

| Reason | Client |
|---|---|
| 0 | OK : maj `g_BankVault` + bascule de la fenêtre 275 |
| 2 | `MSI_BANK_SYSTEM_BUSY` (3031) |
| autre | `MSI_BANK_SYSTEM_ERROR` (2454) |

rAthena n'envoie **que** `reason = 0` (`clif_Bank_Check`).

### `ZC_ACK_BANKING_DEPOSIT 0x09A8`

| Reason | Serveur (`e_BANKING_DEPOSIT_ACK`) | Client |
|---|---|---|
| 0 | `BDA_SUCCESS` | maj `g_BankVault` + `g_PlayerZeny`, rafraîchit l'UI |
| 1 | `BDA_ERROR` | ⚠ **tombe dans le défaut** → `MSI_BANK_SYSTEM_ERROR` (2454) |
| 2 | `BDA_NO_MONEY` | `MSI_BANK_DEPOSIT_NO_MONEY` (2456) |
| 3 | `BDA_OVERFLOW` | `MSI_BANK_WARNING` (2465) |
| 4 | *(inutilisé par rAthena)* | `MSI_BANK_PROHIBIT` (2489) |

### `ZC_ACK_BANKING_WITHDRAW 0x09AA`

| Reason | Serveur (`e_BANKING_WITHDRAW_ACK`) | Client |
|---|---|---|
| 0 | `BWA_SUCCESS` | maj + rafraîchissement |
| 1 | `BWA_NO_MONEY` | `MSI_BANK_WITHDRAW_NO_MONEY` (2455) |
| 2 | `BWA_UNKNOWN_ERROR` | ⚠ **tombe dans le défaut** → `MSI_BANK_SYSTEM_ERROR` (2454) |
| 3 | *(inutilisé par rAthena)* | `MSI_BANK_PROHIBIT` (2489) |

`BWA_UNKNOWN_ERROR` couvre côté serveur « montant ≤ 0 » **et** « zeny + montant
> MAX_ZENY » (ce dernier cas est accompagné d'un `msg_txt 1495` explicite en chat).

---

## 9. Côté serveur (`moonlight`)

| Élément | Emplacement |
|---|---|
| `feature.banking: on` | `conf/battle/feature.conf:34`, aussi `conf/import/battle_conf.txt:544` |
| `feature.banking_state_enforce: no` | `conf/battle/feature.conf:39` — **à laisser sur `no`** (§2) |
| `clif_Bank_Check` / `clif_bank_deposit` / `clif_bank_withdraw` | `src/map/clif.cpp:9576 / 9617 / 9661` |
| `pc_bank_deposit` / `pc_bank_withdraw` | `src/map/pc.cpp:15132 / 15159` |
| `sd->bank_vault` | `src/map/pc.hpp:948` — **`int32`** |
| `MAX_BANK_ZENY` | `src/common/mmo.hpp:83` = `SINT32_MAX` |
| Variable de compte | `BANK_VAULT_VAR` = `#BANKVAULT` (`src/map/pc.hpp:53`) |
| Sauvegarde | `chrif_save(CSAVE_NORMAL)` si `save_settings & CHARSAVE_BANK` |
| Mapflag | `MF_NOBANK` → `msg_txt 831` « You cannot use the Bank on this map. » (aucune map ne l'utilise actuellement) |
| Messages | 831, 1495 (« You can't withdraw that much money »), 1496 (« Banking is disabled ») |

Le solde étant un `int32` et une **variable de compte** (`#BANKVAULT`), la banque
est partagée entre tous les personnages du compte.

---

## 10. Raccourci clavier

`UIWindowMgr_DispatchHotkeyBehavior` (`0x00A451E0`), **behavior id 146**
(code à `0x00A45DD6`) :

```c
case 146:
  if ( g_pUIBankWnd )                                  // 0x0131F930
    UIWindowMgr_SaveRectAndCloseWindow(g_UIWindowMgr, 275);
  else {
    Src = 0x09AB; *(u32*)(&Src + 2) = g_Account_Aid;
    CRagConnection_SendPacket(inst, PacketLen_Get(0x09AB), &Src);
  }
```

La correspondance **touche → behavior** n'est pas dans l'exe : elle est résolue
en Lua par `GetBehaviorOfHotKey2` (tables `UserHotkey_V2` / `UserHotkey`,
`UserHotkey_Lua_GetHotKey` `0x00D80950`). Ctrl+B est la valeur par défaut kRO ;
elle est reconfigurable dans la fenêtre de réglage des raccourcis.

---

## 11. Pièges pour un futur plugin

1. **Ne jamais appeler `MakeWindow(275)` directement** — `g_BankVault` ne serait
   pas rafraîchi. Passer par `CZ_REQ_BANKING_CHECK 0x09AB`.
2. **`ZC_BANKING_CHECK` bascule** la fenêtre : deux envois successifs = ouverture
   puis fermeture.
3. `g_BankVault` est un **`s64`** côté client mais un `int32` côté serveur ; ne
   pas lire seulement le dword de poids faible.
4. Les gardes de blocage (raffinage, enchant, runes, barter) sont **purement
   client** : le serveur accepte le dépôt/retrait même si l'une de ces fenêtres
   est ouverte.
5. Le layout de `Reason`/`Money` diffère entre `0x09A6` et `0x09A8`/`0x09AA` (§2).
6. Ne pas activer `feature.banking_state_enforce` (§2).

---

## 12. Conversion ImGui — `BankTweaks`

Plugin [src/plugins/bank_tweaks.cc](../src/plugins/bank_tweaks.cc). La fenêtre ImGui
**remplace** la native : celle-ci est masquée (`+0x28 = 0`, hors rendu ET hors
hit-test) tant que le viewer est actif. Elle continue de recevoir les paquets et de
tenir `g_BankVault` à jour — elle ne se voit simplement plus.

Membre du groupe **« Interface moderne »** (`SetModernInterface`) : pas de case
isolée. La banque échange des zeny avec la poche, dont le montant est affiché par le
footer de l'inventaire moderne — et c'est le bouton « sac de zeny » de ce footer qui
l'ouvre. Moderne d'un côté et natif de l'autre laisserait le bouton sans fenêtre.

| Réglage (`bourgeon_settings.yaml`) | Défaut | Effet |
|---|---|---|
| `bank_imgui` | off | fenêtre ImGui + native masquée — **basculé en groupe** |
| `bank_quick_amounts` | on | rangée +10k … +100M |
| `bank_show_total` | on | ligne « Total » + jauge de plafond |

Section « Banque » du panneau Moonlight (`MoonlightUi::kIfaceBank`).

Choix d'implémentation qui découlent directement de la RE :

- **Pas de bouton « ouvrir » ni « rafraîchir ».** Envoyer `0x09AB` alors que la
  banque est ouverte la refermerait (§1). La fenêtre se contente donc de suivre
  la présence de la native — masquée, mais bien vivante — via `FindWindow(275)` +
  contrôle de vtable.
- **Le X ferme la native**, via `OnMsg(6, 201)` — le chemin exact du bouton natif,
  pas un appel direct au gestionnaire.
- **Les montants sont formatés par les fonctions du client**
  (`Cstr_FormatInt64Grouped` / `…Int32Grouped`) : mêmes séparateurs que le natif.
- **Les refus reprennent les msgstrings natifs** (`MSI_BANK_*`, §7) au lieu d'un
  texte à nous, pour que le libellé soit exactement celui du client.
- **La garde « fenêtre bloquante ouverte »** (§7) est reproduite, en grisant les
  boutons plutôt qu'en laissant cliquer pour refuser après coup.

### Habillage

La fenêtre utilise **`유저인터페이스\bank\bg_bank_moon.bmp`** comme fond, chargé par
`ro::TextureFromGameFile` (le TexMgr natif lit le GRF ou l'override `data\`, rien
n'est livré avec le plugin) et invalidé sur `Overlay_DeviceEpoch()`.

Nom **distinct** de `bg_bank.bmp` : la native garde son art d'origine — elle est
masquée, pas détruite, et redevient normale dès que le groupe repasse à OFF.

⚠ `ro::GameFilePixels` rejetait en silence toute texture > 256×256 (borne taillée
pour les icônes) : un fond de fenêtre n'a jamais pu charger tant qu'elle était là.
Elle est passée à 4096, la même que `LoadClientBmp` dans `ui/ro_imgui.cc`.

Le bitmap de Moonlight fait **345 × 187** et pilote la mise en page :

| Élément | Coordonnées dans le bitmap |
|---|---|
| Largeur de la fenêtre | 345 (= `FontSize 15 × 23`) |
| Sac de zeny cuit dans l'image | `x 7..35`, `y 93..123` |
| Bande grise « ligne montant » | `y 86..133` (bordures `198,198,198`) |

Conséquences dans le code :

- **Blit 1:1 ancré en HAUT** du corps, jamais étiré. Si la fenêtre grandit, c'est
  le bas qui déborde du bitmap ; les bandes du haut ne bougent pas d'un pixel.
- Le **sac tient lieu de libellé** : pas de texte « Montant », et le champ démarre
  après lui (`x 35 + ItemSpacing`), sur une largeur réduite (120 px).
- Le **message d'état est rendu dans la bande**, à droite du champ — comme le natif
  (`UIBankWnd_DrawContent`, y=112). Il n'ajoute donc aucune ligne, et la hauteur de
  la fenêtre reste alignée sur le bitmap.
- Sans le bmp, repli automatique : libellé « Montant », champ pleine largeur,
  largeur de fenêtre dérivée de la police.

### Bouton banque dans le footer de l'inventaire

`InventoryViewer` pose un bouton **sac de zeny** (`유저인터페이스\styleshop\btn_bank_{out,over,down}.bmp`,
19 × 24) juste à gauche du montant de zeny du footer. Clic → `BankTweaks::ToggleFromUi()`,
qui reproduit exactement le behavior 146 (Ctrl+B) : fenêtre ouverte → fermeture
client ; sinon `CZ_REQ_BANKING_CHECK 0x09AB`, et c'est le serveur qui ouvre.

Ces bitmaps sont de l'art **ajouté** : ils n'ont pas de string dans l'exe. Le
préfixe CP949 est donc emprunté à une string `styleshop` native
(`0x010265E8` = `유저인터페이스\styleshop\btn_buy_out.bmp`) via `PathWithFileName`,
plutôt que réécrit à la main.

Ce que la version ImGui ajoute par rapport au natif :

- ligne « Total » (banque + poche) et jauge de remplissage du plafond `INT32` —
  le natif ne dit rien jusqu'à ce qu'on bute dedans ;
- « Tout déposer » / « Tout retirer » explicites, là où le natif n'a qu'un bouton
  `MAX` dont le sens dépend du bouton cliqué ensuite ;
- paliers d'incrément plus fins (+1k, +10k, +100M) ;
- boutons grisés + raison affichée au lieu d'un refus après coup.
