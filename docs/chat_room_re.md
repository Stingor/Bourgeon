# Salon de chat — « Create Chat Room » (`UIChatRoomMakeWnd`) — RE + blueprint ImGui

Client `Moonlight-Destiny.exe` (base `0x400000`, **pas de rebase**), PACKETVER **20250716**.
Serveur = fork rAthena `moonlight` (`src/map/chat.cpp`, `src/map/clif.cpp`).

⚠ Rappel de méthode : l'IDB analysé est l'**exe VANILLA** ; les 110 patchs WARP0716 n'y sont
pas (cf. `reference_ida_is_vanilla_warp_patches`). Aucune adresse ci-dessous n'est patchée.

---

## 0. Ce qu'il faut retenir en dix lignes

| | |
|---|---|
| Fenêtre | **`UIChatRoomMakeWnd`**, **id 27 (`0x1B`)**, **280 × 120** |
| Fenêtre sœur | **`UIChatRoomChangeWnd`**, **id 30 (`0x1E`)** — elle **DÉRIVE** de la précédente |
| Ouverture | `MakeWindow(0x1B)` — trois chemins, tous équivalents (§2) |
| Envoi | `CMode::SendMsg(43)` → **`CZ_CREATE_CHATROOM 0x00D5`** (§7) |
| Réponse | **`ZC_ACK_CREATE_CHATROOM 0x00D6`** → succès = `MakeWindow(0x1C)` = la salle (§9) |
| 🔴 Piège n°1 | la fenêtre se **FERME AVANT** la réponse serveur : sur refus, **toute la saisie est perdue** |
| 🔴 Piège n°2 | `this[35] = 184` ⇒ **Entrée = OK = paquet envoyé**. Une native *masquée* crée un salon toute seule (même piège que la banque, cf. `docs/bank_zeny_re.md`) |
| Contraintes | titre ≤ **36** car., mot de passe ≤ **8** car. (≥ 4 si privé), limite ∈ {2,3,5,10,15,20} |
| Coïncidence utile | `CHATROOM_TITLE_SIZE = 36+1`, `CHATROOM_PASS_SIZE = 8+1`, `MAX_CHAT_USERS = 20` côté serveur : **le client colle exactement au serveur** |

---

## 1. La famille de classes

Le RTTI de cet exe n'est **pas** atteignable par un simple `XrefsTo(TypeDescriptor)` : les
`CompleteObjectLocator` ne sont référencés par aucune donnée typée. Il faut **chercher le dword
brut** de l'adresse du COL dans `.rdata` — la vtable commence 4 octets après.

```python
COL = TypeDescriptorAddr - 0 ... # COL+0x0C = pTypeDescriptor
# donc : COL = (adresse du dword valant TD) - 12 ; vtable = (adresse du dword valant COL) + 4
```

| Classe | TypeDescriptor | COL | **vtable** | ctor | id fenêtre | taille objet |
|---|---|---|---|---|---|---|
| `UIChatRoomTitle` | `0x0123E5C4` | `0x010C106C` | `0x0102A4E8` | — | — (panneau au-dessus de la tête) | — |
| `UIChatRoomWnd` | `0x0123F05C` | `0x010C2D64` | `0x01030678` | `0x0086B7F0` | **28 (`0x1C`)** | `0x124` |
| **`UIChatRoomMakeWnd`** | `0x0123FA00` | `0x010C409C` | **`0x01033FC4`** | **`0x0088D160`** | **27 (`0x1B`)** | `0x128` |
| `UIChatRoomChangeWnd` | `0x0123FA20` | `0x010C40F0` | `0x0103409C` | `0x0088D000` | **30 (`0x1E`)** | `0x128` |

🔴 **Correction du registre RE** : la fonction `0x008BDA70` était nommée `UIWnd0x1e_OnMsg` avec la
mention « éditeur nom/valeur, UI cmd 0x2c, dropdown historique id 0x1a » (mémoire
`reference_rtti_window_class_id`). C'est en réalité **`UIChatRoomChangeWnd::OnMsg`** : « nom/valeur »
= titre/mot de passe, « cmd 0x2c » = `SendMsg(44)` = `CZ_CHANGE_CHATROOM`, et « dropdown 0x1a » = le
`UIComboBoxWnd` de la limite. L'id 30 est confirmé deux fois (§6 et la table `MakeWindow`).

**Héritage** : `UIChatRoomChangeWnd_ctor` (`0x0088D000`) pose d'abord la vtable de
`UIChatRoomMakeWnd`, puis la sienne ⇒ **Change dérive de Make**. Les deux partagent donc
`OnCreate`, la géométrie, la struct, et `Change::OnMsg` **retombe** sur `Make::OnMsg` pour tout ce
qu'il ne traite pas.

### Slots dédiés du gestionnaire (`g_UIWindowMgr` = `0x0131F4E8`)

| id | offset | globale | classe |
|---|---|---|---|
| 26 (`0x1A`) | `+0x214` → `0x0131F6FC` | `g_UIComboBoxWnd_Slot` | `UIComboBoxWnd` (le popup déroulant) |
| 27 (`0x1B`) | `+0x274` → `0x0131F75C` | — | `UIChatRoomMakeWnd` |
| 28 (`0x1C`) | `+0x27C` → `0x0131F764` | `g_UIChatRoomWnd_Slot` | `UIChatRoomWnd` (la salle ouverte) |
| 30 (`0x1E`) | `+0x278` → `0x0131F760` | — | `UIChatRoomChangeWnd` |
| — | `+0x2FC` → `0x0131F7E4` | `g_VendingShopMakeWnd` | montage d'échoppe |
| — | `+0x304` → `0x0131F7EC` | — | échoppe en cours (vending actif) |

⚠ Comme toujours, **les slots ne sont pas indexés linéairement par id** : 26→`0x214`, 27→`0x274`,
30→`0x278`, 28→`0x27C`. Ne rien déduire d'un offset.

---

## 2. Ouverture : trois chemins, un seul point d'entrée utile

### 2.1 `ChatRoom_OpenMakeWnd` (`0x00692D70`) — LE point d'entrée

```c
if (mgr+0x304)                      // échoppe en cours
    MsgBox(MSI 587 "Évitez d'ouvrir un salon de chat pendant le vending.");
else if (g_VendingShopMakeWnd)      // montage d'échoppe ouvert
    CMode::SendMsg(82);             // (ferme le montage)
else
    MakeWindow(g_UIWindowMgr, 0x1B);
```

Cette fonction est **enregistrée comme `std::function<void()>`** dans la table
`TalkType → action` du `CGameSettingsMgr`, **sous l'index `0x17`** — c'est-à-dire
`TT_MAKE_CHATROOMMAKEWND` de `src/ragnarok/talktype.h`. L'enregistrement se lit en clair dans
`CGameSettingsMgr_Init_Func` à **`0x00692690`** (le `var_3C = 0x17` juste au-dessus).

🟢 **Conséquence directe et réutilisable** : `gamesettings::ExecOption(0x17)` (déjà présent dans
`src/ragnarok/game_settings.cc`, natif `CGameSettingsMgr_ExecOption` `0x0068E160`) **ouvre la
fenêtre native de création de salon**. `ExecOption` exige que l'enregistrement d'option ait le
type `1` (= EXE) ; c'est le cas ici, puisque c'est le chemin qu'emprunte `/chat`.

### 2.2 La commande `/chat`

Table des commandes slash (`sub_D6B130`) : `"/chat"` → **`0x17`**, et son homographe clavier
coréen `"/coxldqkd"` → `0x17` aussi (`0x00D6B6A5` et `0x00D6B680`). Même route que ci-dessus.

### 2.3 Le bouton de la fenêtre Basic Info

`UIBasicInfoWnd::OnMsg` (**`0x0095F3B0`**, vtable `0x0103E35C` slot `+0x94`), **commande 214**,
libellé `MSI_MAKE_CHAT_ROOM` (3225, « Ouvrir un chat ») :

```c
case 214:
    if (g_UIChatRoomWnd_Slot)  break;              // déjà dans un salon → rien
    if (g_VendingShopMakeWnd || mgr+0x304) { MsgBox(...280x120...); break; }
    MakeWindow(g_UIWindowMgr, 0x1B);
```

⚠ Les gardes **ne sont pas les mêmes** que celles de `ChatRoom_OpenMakeWnd` : ici « déjà dans un
salon » est testé, là-bas non. Un portage doit rejouer **l'union** des deux.

### 2.4 Le raccourci Alt+C

Le libellé `"Alt+C"` posé sur la croix (§3) est **écrit en dur** dans `OnCreate` : ce n'est **pas**
le raccourci réel, seulement une étiquette. Le vrai binding vit côté Lua
(`SaveData\UserKeys.lua`, table `USERKEY_2` = catégorie Interface) et **n'est pas** dans
`UIWindowMgr_DispatchHotkeyBehavior` (vérifié : aucun `0x1B` dedans). Sur un client AZERTY ou
après remappage, l'étiquette ment. → **défaut à corriger dans le portage** (`userhotkey::`
donne le nom de touche *layout-aware*).

---

## 3. `OnCreate` (`0x008A1C30`) — géométrie et contrôles

⚠ **`OnCreate` est PARTAGÉ** entre `UIChatRoomMakeWnd` et `UIChatRoomChangeWnd` (vtable `+0x3C`
identique). La garde ci-dessous ferme donc **toujours la 27**, même quand c'est la 30 qui naît.

### 3.1 Garde d'entrée

```c
if (ChatRoom_IsCellBlockedByRoomTitle(g_Account_Aid)) {
    ChatAction(1, MsgString(661), 0xFF, 0);      // « Vous ne pouvez pas ouvrir de fenêtre de chat. »
    SaveRectAndCloseWindow(mgr, 27);             // = DESTRUCTION
}
```

`ChatRoom_IsCellBlockedByRoomTitle` (**`0x00A38C60`**) parcourt la liste des fenêtres du
gestionnaire (`mgr[95]`), garde celles dont le RTTI est `UIChatRoomTitle` ou
`UIMerchantShopTitle`, remonte à l'acteur propriétaire et compare sa **cellule** à la mienne.
Autrement dit : **« il y a déjà un panneau de salon ou d'échoppe sur ma case »**. Purement client.

### 3.2 Les contrôles créés

Fenêtre **280 × 120** (taille posée par `MakeWindow` juste après le ctor : `push 0x78; push 0x118`).

| Contrôle | classe | position | taille | cmd | champ |
|---|---|---|---|---|---|
| Croix de fermeture (`"Alt+C"`) | `UIWindow_AddCloseButton` `0x00894FA0` | coin haut-droit | — | **201** | `this[57]` |
| Éditeur **Titre** | `UIEdit` (`0x00817DA0`) | (45, 24) | 228 × 16 | — | `this[51]` |
| Libellé **limite** | `UIStaticText` (`0x008188D0`) | (48, 49) | 68 × 14 | — | `this[55]` |
| Bouton ▼ limite | `UIBitmapButton` (`0x008172B0`) | (119, 46) | (du bmp) | **228** | — |
| Libellé **type** | `UIStaticText` | (187, 49) | 68 × 14 | — | `this[56]` |
| Bouton ▼ type | `UIBitmapButton` | (256, 46) | (du bmp) | **229** | — |
| Radio **Public** | `UIRadioBtn` (`0x00818360`) | (44, 70) | 40 × 12 | **215** | `this[53]` |
| Radio **Privé** | `UIRadioBtn` | (96, 70) | 52 × 12 | **215** | `this[54]` |
| Éditeur **Mot de passe** | `UIEdit` | (182, 70) | 90 × 16 | — | `this[52]` |
| Bouton **OK** | `UIBitmapButton` | (189, 96) | (du bmp) | **184** | — |
| Bouton **Annuler** | `UIBitmapButton` | (234, 96) | (du bmp) | **185** | — |

Bitmaps (dossier CP949 `유저인터페이스\`) : `btn_ok{,_a,_b}.bmp`, `btn_cancel{,_a,_b}.bmp` ;
dossier `유저인터페이스\basic_interface\` : `txtbox_btn_{a,b,c}.bmp` pour les deux ▼.
Couleurs de texte : éditeurs `RGB(242,242,242)`, libellés `0xFFF2F2F2`.

**Il n'y a aucun libellé « Titre : / Limite : / Type : / Mot de passe : »** — le client compte sur
le fond de la fenêtre. Les clés `MSI_ROOM_TITLE` (4316), `MSI_ROOM_LIMIT` (4317),
`MSI_ROOM_TYPE` (4318), `MSI_ROOM_RESTRICT` (4319), `MSI_ROOM_PASSWORD` (4320) **existent dans la
table et sont déjà traduites en FR/ES**, mais **aucun code de ce client ne les lit**. Elles sont
donc offertes gratuitement au portage ImGui.

### 3.3 Détails d'éditeur

`UIEdit` (ctor `0x00817DA0`, défauts `[33] = 0`, `[34] = 255`) :

- `+0x88` (`[34]`) = **longueur maximale** → **36** pour le titre, **8** pour le mot de passe ;
- `+0x84` (`[33]`) = **caractère de masque** → **`42` = `'*'`** sur le mot de passe (0 = pas de masque).

Le focus clavier part sur l'éditeur de titre (`UIWindowMgr_SetFocusedWnd`).

### 3.4 🔴 Le bouton par défaut

Dernière ligne utile d'`OnCreate` : **`this[35] = 184`**.

`UIWindow_OnMsg_Default` (`0x008841D0`) : `msg 0` (Entrée / `UIWindowMgr_ActivateDefaultButton`
`0x00A2E270`) rejoue `OnMsg(6, this[35])`, `msg 1` (Échap) rejoue `OnMsg(6, this[36])`.

Donc **Entrée = commande 184 = créer le salon et envoyer le paquet**, et `this[36]` n'est
jamais posé (Échap ne fait rien). C'est exactement le piège de la banque : **une fenêtre native
seulement masquée garde le clavier et créera un salon sur une frappe d'Entrée destinée à notre
UI**. → Le portage doit **DÉTRUIRE**, jamais masquer (`reference_native_window_toggle_router`).

---

## 4. Struct `UIChatRoomMakeWnd` (taille `0x128`)

`this` vu comme `int*` ; les offsets hérités de `UIWindow` ne sont pas répétés.

| index | offset | rôle |
|---|---|---|
| `[35]` | `0x8C` | commande du **bouton par défaut** (Entrée) = 184 |
| `[36]` | `0x90` | commande du bouton **Annuler** (Échap) — **non posée ici** |
| `[45..50]` | `0xB4` | `std::string` **titre de la fenêtre** = `MSI 125` (Make) / `MSI 126` (Change) |
| `[51]` | `0xCC` | `UIEdit*` **titre du salon** (max 36) |
| `[52]` | `0xD0` | `UIEdit*` **mot de passe** (max 8, masque `'*'`) |
| `[53]` | `0xD4` | `UIRadioBtn*` **Public** |
| `[54]` | `0xD8` | `UIRadioBtn*` **Privé** |
| `[55]` | `0xDC` | `UIStaticText*` texte de la limite courante |
| `[56]` | `0xE0` | `UIStaticText*` texte du type courant |
| `[57]` | `0xE4` | bouton de fermeture |
| `[58]` | `0xE8` | **index du radio coché** : `0` = public, **`1` = privé** |
| `[59]` | `0xEC` | quel déroulant est ouvert : `0` = limite, `1` = type |
| `[60]` | `0xF0` | **limite choisie** (défaut **20**) — part dans le paquet |
| `[61]` | `0xF4` | **type choisi** (défaut 0) — **ne part PAS dans le paquet** |
| `[62..64]` | `0xF8` | `vector<std::string>` libellés de la **limite** |
| `[65..67]` | `0x104` | `vector<std::string>` libellés du **type** |
| `[68..70]` | `0x110` | `vector<int>` valeurs de la **limite** |
| `[71..73]` | `0x11C` | `vector<int>` valeurs du **type** |

Contenu des listes, monté par `OnCreate` :

- **limite** : `"2 " + MSI 131`, `"3 "…`, `"5 "…`, `"10 "…`, `"15 "…`, `"20 "…`
  (MSI 131 = `MSI_COUNT_UNIT_OF_PEOPLE` = « pers. ») → valeurs `2, 3, 5, 10, 15, 20`.
  Le libellé initial affiché est le **dernier** de la liste (« 20 pers. »), cohérent avec `[60] = 20`.
- **type** : **un seul item**, `MSI 130` (`MSI_CHAT_ROOM`, « Salon de chat ») → valeur `0`.
  Le déroulant existe, il ne propose rien d'autre, et sa valeur n'est **jamais** transmise.

---

## 5. `UIChatRoomMakeWnd::OnMsg` (`0x008BE010`)

Signature `OnMsg(this, sender, msgType, wparam, lparam, …)`.

### 5.1 `msgType == 6` — clic de bouton (`wparam` = commande)

| cmd | effet |
|---|---|
| **184** | OK — validation puis envoi (§6) |
| **185** | Annuler → `SaveRectAndCloseWindow(27)` |
| **201** | croix → `SaveRectAndCloseWindow(27)` |
| **215** | un radio a été cliqué : parcourt `this[53]`/`this[54]`, coche celui dont le `sender` correspond, écrit son index dans `this[58]`, décoche l'autre (via `OnMsg(13, 0/1)` sur chaque radio) |
| **228** | ouvre le déroulant **limite** (`this[59] = 0`) |
| **229** | ouvre le déroulant **type** (`this[59] = 1`) |

### 5.2 Le déroulant — protocole `UIComboBoxWnd` (id 26)

```c
if (g_UIComboBoxWnd_Slot) SaveRectAndCloseWindow(mgr, 26);   // un seul popup à la fois
w = MakeWindow(mgr, 0x1A);
for (chaque libellé)  w->OnMsg(0, 31, (char*)libellé, …);    // 31 = AJOUTER UN ITEM
w->OnMsg(0, 40, 0, …);                                       // 40 = fin de remplissage
w->SetSize(74, min(16 * n, 48));                             // 16 px par ligne, 3 lignes max
w->OnMsg(0, 83, 27, …);                                      // 83 = DÉCLARER LE PROPRIÉTAIRE (id 27 / 30)
w->SetPos(ClientToScreen(44 + 137 * this[59], 64) + (1,0));
```

Donc : popup **74 px de large**, **48 px de haut au plus** ⇒ **3 lignes visibles sur 6** pour la
limite (il faut faire défiler pour atteindre 15 et 20). Positions écran : `(44, 64)` pour la
limite, `(181, 64)` pour le type.

### 5.3 `msgType == 39` — un item du déroulant a été choisi (`wparam` = index)

```c
if (this[59] == 0) { SetText(this[55], limitLabels[i]); this[60] = limitValues[i]; }
else               { SetText(this[56], typeLabels[i]);  this[61] = typeValues[i];  }
```

### 5.4 Tout le reste

→ `UIWindow_OnMsg_Default` (`0x008841D0`) — c'est là que passe la touche Entrée (§3.4).

---

## 6. Validation côté client, avant l'envoi (cmd 184)

Dans l'ordre exact :

| # | test | réaction |
|---|---|---|
| 1 | `ChatRoom_IsCellBlockedByRoomTitle(g_Account_Aid)` | message de **chat** `MSI 661` — pas de boîte |
| 2 | `UIEdit_GetTextLength(titre) == 0` | boîte modale `MSI 13` « Veuillez saisir le titre du salon. » |
| 3 | `sub_A85BE0(0x0159C2C8, titre)` — filtre de **mots interdits** | boîte `MSI 14` « Langage inapproprié détecté. » |
| 4 | `g_ServiceType == 1` **et** `Str_IsPureAscii7(titre)` faux — **ASCII 7 bits seulement** | boîte `MSI 190` « Ce serveur n'accepte que les caractères anglais. » |
| 5 | `strlen(passe) < 4` **et** `this[58] == 1` (privé) | boîte `MSI 15` si le champ est vide, `MSI 16` sinon |
| — | sinon | **envoi** puis `SaveRectAndCloseWindow(27)` |

Boîtes modales : `UIWndMgr_ShowMessageBoxModal` (`0x00A31A30`), **280 × 120**, un seul bouton.

🔴 **Le test 4 est ACTIF sur Moonlight — mesuré en jeu (x32dbg, 2026-08-23)** :
`g_ServiceType` (`0x0159B810`) vaut **`1`**, et `Str_IsPureAscii7` (`0x00D71EF0`) n'est rien
d'autre que « **tous les octets sont < 0x80** » :

```c
char __stdcall Str_IsPureAscii7(const char* s) {
    int n = strlen(s);
    for (int i = 0; i < n; ++i) if (s[i] < 0) return 0;   // char SIGNÉ
    return 1;
}
```

Donc, tel quel, **un titre de salon ne peut contenir ni accent, ni caractère coréen, ni emoji** :
« Chasse à l'Ours » est refusé par le **client** avec `MSI 190`. Le **serveur**, lui, ne teste
rien (`clif_parse_CreateChatRoom` recopie les octets sans les regarder) et `chat_data.title` fait
36 octets libres.

⚠ **Décision à prendre pour le portage** (elle n'est pas technique, elle est éditoriale) : notre
fenêtre ImGui n'est pas obligée de rejouer ce test. Si on l'assouplit, le titre part en octets
non-ASCII et **traverse le fil bi-encodage** documenté par `project_utf8_emoji_support` — il
faudra vérifier le rendu du panneau `UIChatRoomTitle` et de la ligne de chat AVANT de l'ouvrir.
Par défaut : **rejouer le test**, mais en direct (surlignage du caractère fautif pendant la
frappe) au lieu d'une boîte modale après coup.

🔴 **La limite n'est jamais validée** côté client autrement que par la liste fermée du déroulant :
`this[60]` est ce que le déroulant y a mis, point.

---

## 7. Le paquet — `CZ_CREATE_CHATROOM 0x00D5`

`CMode::SendMsg` (**`0x00C86740`**, `vtable+0x18`), **message 43**, bloc `0x00C8C590`.
⚠ Rappel du piège documenté : les étiquettes « case N » d'IDA sur ce switch **sont déjà la valeur
du message**, pas un index (cf. `reference_cmode_sendmsg_use_skill`).

### 7.1 La struct passée en `p1` (64 octets, montée sur la pile de `OnMsg`)

| offset | type | contenu |
|---|---|---|
| `0x00` | `std::string` | **titre** |
| `0x18` | `std::string` | **mot de passe** |
| `0x30` | `BOOL` | **`1` = public**, `0` = privé (= `this[58] != 1`) |
| `0x34` | `int` | `0` |
| `0x38` | `int` | **limite** (`this[60]`) |
| `0x3C` | `int` | `-1` |

Construite par `sub_88CC80` (ctor) / `sub_819300` (dtor), remplie par `sub_5A45E0` (assignation
de `std::string`). **Le dtor est obligatoire** — même discipline que la `CSkillInfo` du 0x71.

### 7.2 Effet de bord : le `CGameMode` mémorise

```
CGameMode+0x3C8 = titre        CGameMode+0x3F8 = public
CGameMode+0x3E0 = mot de passe CGameMode+0x400 = limite
```

Utile : c'est ce que `UIChatRoomChangeWnd` réutilise, et c'est un endroit où relire le dernier
salon créé.

### 7.3 Sérialisation

```
+0x00  W   0x00D5
+0x02  W   longueur = 15 + strlen(titre)
+0x04  W   limite
+0x06  B   type      (1 = public, 0 = privé)
+0x07  B[8] mot de passe, complété de zéros  (jamais terminé par NUL si 8 car.)
+0x0F  B[] titre, longueur-15 octets, PAS terminé par NUL
```

Identique octet pour octet à `PACKET_CZ_CREATE_CHATROOM` de `moonlight/src/map/packets.hpp`.
`CZ_CHANGE_CHATROOM 0x00DE` (message **44**) a **exactement la même mise en forme**, et est
gardé par `if (g_UIChatRoomWnd_Slot == 0) return;` — on ne modifie que le salon qu'on tient.

---

## 8. Côté serveur (`moonlight`)

`clif_parse_CreateChatRoom` (`src/map/clif.cpp:16982`) puis `chat_createpcchat`
(`src/map/chat.cpp:77`). Refus, dans l'ordre :

| garde | retour visible |
|---|---|
| `SC_NOCHAT` avec `MANNER_NOROOM` | **rien** (silence total) |
| `basic_skill_check` **oui** et `NV_BASIC < 4` et `SU_BASIC_SKILL < 1` | `clif_skill_fail(1, USESKILL_FAIL_LEVEL, 3)` |
| `npc_isnear(sd)` (`min_npc_vendchat_distance: 3`) | `msg 662` + `USESKILL_FAIL_THERE_ARE_NPC_AROUND` |
| `sd->chatID` déjà posé | **rien** |
| `sd->state.vending` ou `buyingstore` | **rien** |
| carte avec `MF_NOCHAT` | `msg 281` |
| **[Stingor]** carte `gonryun`, `y <= 131`, groupe `< 40` | `msg 1834` — **spécifique Moonlight** |
| cellule `CELL_CHKNOCHAT` | `msg 665` |
| création OK | `clif_createchat(CREATEROOM_SUCCESS)` + `clif_dispchat` |
| `chat_createchat` échoue | `clif_createchat(CREATEROOM_LIMIT_EXCEEDED)` |

Confirmé dans `conf/import/battle_conf.txt` : **`basic_skill_check: yes`** ⇒ sur Moonlight il
faut **Basic Skill niveau 4** pour ouvrir un salon. Le client n'en sait rien et laisse cliquer.

Constantes (`src/map/map.hpp`, `src/map/chat.hpp`) :
`CHATROOM_TITLE_SIZE = 36 + 1`, `CHATROOM_PASS_SIZE = 8 + 1`, **`MAX_CHAT_USERS = 20`**.
Les plafonds du client sont donc **exactement** ceux du serveur — proposer 30 places serait un
mensonge d'interface.

`unit_stop_walking` + `unit_stop_attack` sont appelés à la création : ouvrir un salon **arrête
le personnage**.

---

## 9. Réception

### 9.1 `ZC_ACK_CREATE_CHATROOM 0x00D6` — handler `0x00CA1701`

Commence par `CGameMode+0xFC = 0`, puis selon l'octet de statut :

| valeur | rAthena | effet client |
|---|---|---|
| **0** | `CREATEROOM_SUCCESS` | chat `MSI 64` « Le salon a bien été créé. » en `0x00FFFF`, puis **`MakeWindow(0x1C)`** = ouverture de `UIChatRoomWnd` |
| **1** | `CREATEROOM_LIMIT_EXCEEDED` | chat `MSI 65` « Nombre maximal de salons atteint. » |
| **2** | `CREATEROOM_ALREADY_EXISTS` | chat `MSI 66` « Un salon du même nom existe déjà. » |

🔴 **Sur 1 et 2, la fenêtre 27 est déjà détruite** (fermée par `OnMsg` juste après l'envoi). Le
joueur perd son titre, son mot de passe et ses réglages, et n'a qu'une ligne de chat pour
comprendre. **C'est le défaut n°1 à corriger.**

### 9.2 Le reste du protocole (déjà dans `docs/opcode_map.csv`)

| opcode | sens | handler client | rôle |
|---|---|---|---|
| `0x00D5` | CZ var 15 | — | créer |
| `0x00D6` | ZC fix 3 | `0x00CA1701` | ACK création |
| `0x00D7` | ZC var 17 | `0x00CA1844` | `ZC_ROOM_NEWENTRY` — **panneau d'un salon visible** |
| `0x00D8` | ZC fix 6 | `0x00CA1855` | salon détruit |
| `0x00D9` | CZ fix 14 | — | demander à entrer (`chat id` + passe 8B) |
| `0x00DA` | ZC fix 3 | `0x00CA1B95` | entrée refusée |
| `0x00DB` | ZC var 8 | `0x00CA18DD` | je suis entré (liste des membres) |
| `0x00DC` | ZC fix 28 | `0x00CA18EE` | nouvel arrivant |
| `0x00DD` | ZC fix 29 | `0x00CA19F4` | départ |
| `0x00DE` | CZ var 15 | — | modifier les réglages |
| `0x00DF` | ZC var 17 | `0x00CA1BA6` | réglages modifiés |
| `0x00E0` | CZ fix 30 | — | céder les droits de chef |
| `0x00E1` | ZC fix 30 | `0x00CA1B3E` | changement de chef |
| `0x00E2` | CZ fix 26 | — | expulser |
| `0x00E3` | CZ fix 2 | — | quitter |

`ZC_ROOM_NEWENTRY 0x00D7` — la matière première d'un **annuaire des salons proches** :

```c
struct { int16 packetType; uint16 packetSize; int32 owner; int32 id;
         uint16 limit; uint16 users; uint8 type; char title[]; };  // titre NON terminé
```

`type` vient de `clif_chat_status` : `0` = privé (mot de passe), `1` = public, `2/3` = salon de
PNJ. Diffusé en `AREA_WOSC` ⇒ le client **reçoit déjà**, gratuitement, tous les salons de l'écran
avec leur remplissage. Rien à demander au serveur pour bâtir une liste.

---

## 10. `UIChatRoomChangeWnd` (id 30) — les différences

Même `OnCreate`, même struct, même géométrie. `OnMsg` = `0x008BDA70`, qui **délègue** à
`UIChatRoomMakeWnd::OnMsg` tout ce qu'il ne connaît pas.

| | Make (27) | Change (30) |
|---|---|---|
| Titre de fenêtre | `MSI 125` « Créer un salon de chat » | `MSI 126` « Réglages du salon » |
| OK | cmd 184 → `SendMsg(43)` → `0x00D5` | cmd **0xB8 (184)** → `SendMsg(44)` → `0x00DE` |
| Annuler / croix | 185 / 201 → ferme **27** | 0xB9 / 0xC9 → ferme **30** |
| ▼ | 228 / 229, popup lié à **27** | 0xE4 / 0xE5, popup lié à **30** |
| Garde d'envoi | cellule libre + `IsCellBlocked` | **`g_UIChatRoomWnd_Slot != 0`** (je tiens un salon) |
| `msgType 34` | — | **pré-remplissage** : lit la struct 64 o (§7.1) reçue en `wparam` et repose titre, mot de passe, limite, radio ; puis `SetFocus` sur le titre |

`msgType 34` est donc le point d'entrée « charger les réglages actuels dans la fenêtre » : c'est
lui qui rend `UIChatRoomChangeWnd` utilisable, et c'est le modèle exact dont un portage a besoin
pour ré-afficher un brouillon.

---

## 10 bis. `UIChatRoomWnd` (id 28) — la SALLE — RE complète (2026-08-23)

**Identité** : `UIChatRoomWnd`, **id 28 (`0x1C`)**, ctor `0x0086B7F0`, vtable `0x01030678`,
objet `0x124` octets, slot manager `+0x27C` (= `g_UIChatRoomWnd_Slot` `0x0131F764`).
Taille par défaut **280 × 120**, **redimensionnable**, posée par `MakeWindow` à
`Screen_CenterXFrom640(200)` / `UI_ScaleYFrom480(300)`.

### 10bis.1 `OnCreate` (`0x00874D60`) — six contrôles, tout en proportions

Tout est calculé à partir de la taille courante `w = this[5]`, `h = this[6]` : cette fenêtre
est la seule de la famille qui se redimensionne.

| Champ | classe | position | taille | cmd |
|---|---|---|---|---|
| `this[61]` | `UIResizeButton` (mode 7) | (w−15, h−15) | 16 × 16 | — |
| `this[62]` | `UIEditWnd` — **saisie** | (3, h−18) | (w−30) × 16 | **494** |
| `this[63]` | `0x00834AD0` — **historique des messages** | (1, 17) | (w − 56w/280 − 3) × (h−38) | 184 |
| `this[64]` | `0x00836130` — **liste des membres** | (w − 56w/280 − 1, 17) | (56w/280) × (h−38) | 184 |
| `this[65]` | croix de fermeture, libellé **`"/q"`** | — | — | 201 |
| `this[66]` | `UIStaticText` — **ligne de titre** | (9, 2) | (w−30) × 15 | — |

La saisie est plafonnée à **70 octets** (`UIEdit+0x88`) et reçoit le focus à l'ouverture.
Le rapport 56/280 = **20 % de largeur pour la liste des membres**, 80 % pour les messages.

🔴 **`this[35] = 184`** — la fenêtre déclare un bouton par défaut, comme la 27. Ici Entrée
**envoie le message**. Même conséquence : masquer sans détruire est exclu.

### 10bis.2 L'état du salon vit DANS la fenêtre, à `+0xB4`

`UIChatRoomWnd+0xB4` porte une copie du bloc de 64 octets de §7.1 — la même struct que celle
qu'on passe à `SendMsg(43)` :

| offset fenêtre | contenu |
|---|---|
| `+0xB4` | `std::string` titre |
| `+0xCC` | `std::string` mot de passe |
| `+0xE4` | `int` public (1) / privé (0) |
| `+0xE8` | `int` occupants |
| `+0xEC` | `int` limite |
| `+0xF0` | `int` id de salon |

La **ligne de titre** est montée dans l'`OnMsg` à `0x00882F46` :

```c
sprintf(buf, "%s%s (%d/%d)",             // gabarit @0x010317D4
        MsgString(this[0xE4] ? 90 : 89), // MSI_ROOM_PUBLIC2 / MSI_ROOM_PRIVATE2
        this+0xB4,                       // le titre
        this[0xE8], this[0xEC]);         // occupants / limite
```

C'est exactement ce qu'affichait « Publ. : (1/-657931) » quand le bloc n'était pas rempli.

### 10bis.3 Les trois messages qui ALIMENTENT la fenêtre

| msg | émetteur | effet |
|---|---|---|
| **34 (`0x22`)** | `ZC_ACK_CREATE_CHATROOM` succès (`0x00CA17F8`), `ZC_MEMBER_NEWENTRY` (`0x00CA1966`) | pose le bloc de 64 o (§10bis.2). ⚠ À la CRÉATION la source est **`CGameMode+0x3C8`** — d'où l'obligation de passer par `SendMsg(43)` (§12.3) |
| **43 (`0x2B`)** | ACK création (`0x00CA1818`) | « je suis le propriétaire » (argument 0) |
| **44 (`0x2C`)** | ACK création avec `Own_GetCharName()` (`0x00CA183D`), `ZC_MEMBER_NEWENTRY` avec le nom reçu et le drapeau 1 | **ajoute un membre** à la liste |

⚠ `ZC_MEMBER_NEWENTRY` appelle `MakeWindow(0x1C)` lui-même : la fenêtre naît aussi à
l'arrivée d'un membre, pas seulement à la création du salon.
Et l'ACK écrit `CGameMode+0x3FC = 1` (un occupant) avant de recopier le bloc.

### 10bis.4 Les LIGNES du salon — `ChatAction` action 5, et elles se PERDENT

`UIWindowMgr_ChatAction` (`0x00A4AD20`), **action 5** (`UIM_PUSH_INTO_CHATROOM`) :

```c
case 5:
    if (mgr[159])                        // 159*4 = 0x27C = le slot de la fenêtre 28
        mgr[159]->OnMsg(0, 37, texte, couleur, 0, 0);
    return 0;                            // sinon : RIEN. La ligne est JETÉE.
```

🔴 **Différence capitale avec le chat principal.** L'action 1 (chatbox) empile dans la file
`mgr+0x4C4` quand la fenêtre n'existe pas — d'où la fuite mémoire documentée par
`project_chatbox_imgui_conversion`. L'action 5, elle, **abandonne la ligne** en silence. Donc :

- détruire la fenêtre 28 ne fait **fuir aucune mémoire** — bonne nouvelle ;
- mais toute ligne de salon est **perdue** tant que rien ne l'intercepte — donc un
  remplacement DOIT se brancher sur `ChatAction` action 5, sans quoi la salle sera muette.

🟢 Et le détour est **déjà posé** : `ChatWindow` détourne `0x00A4AD20`
(`ChatActionStub` → `ChatActionFilter(action, texte, couleur, type, sender)`), et il n'y a
qu'un seul jeu d'octets à cette adresse — un second détour tuerait le premier en silence.
L'ingestion du salon passe donc par CE filtre, pas par un hook à nous.

### 10bis.5 Les commandes — toutes par `CMode::SendMsg`

Aucune n'est à fabriquer à la main (cf. la leçon de §12.3) :

| msg | paquet | arguments | rôle |
|---|---|---|---|
🔴 **`SendMsg(44)` est gardé comme le 48** : `if (g_UIChatRoomWnd_Slot == 0) return;`
(bloc `0x00C8C6AC`). Même conséquence pour un remplacement qui détruit la fenêtre 28, et même
remède — le paquet brut `CZ_CHANGE_CHATROOM 0x00DE`, au prix du miroir CGameMode et de la
ligne de replay.

| 43 | `CZ_CREATE_CHATROOM 0x00D5` | struct 64 o | créer |
| 44 | `CZ_CHANGE_CHATROOM 0x00DE` | struct 64 o | modifier les réglages |
| **45** | `CZ_REQ_ENTER_ROOM 0x00D9` | (bloc `0x00C8C7D5`) | **entrer** dans un salon |
| **46** | `CZ_REQ_ROLE_CHANGE 0x00E0` | `p1` = nom (C-string), `p2` = rôle | **céder les droits** |
| **47** | `CZ_REQ_EXPEL_MEMBER 0x00E2` | `p1` = nom (C-string) | **expulser** |
| **48** | `CZ_EXIT_ROOM 0x00E3` | — ; gardé par `g_UIChatRoomWnd_Slot != 0` | **quitter** (`/q`) |

🔴 **Le 48 est le CONTRE-EXEMPLE du 43, et il complète la règle.** Son bloc
(`0x00C8C2D1`) ne fait que deux choses : `if (g_UIChatRoomWnd_Slot == 0) return;` puis émettre
un paquet de 2 octets sans charge utile. Un remplacement qui DÉTRUIT la fenêtre 28 met ce slot
à zéro en permanence — le client refuse alors la commande **en silence**, et ni « Quitter » ni
la croix ne ferment quoi que ce soit (vécu en jeu le 2026-08-23). Là, et seulement là, le
paquet brut est le bon choix.

➡ **La règle, dans les deux sens** : regarder ce que le chemin natif fait *en plus* d'émettre.
Si c'est un état que d'autres relisent (43 → le bloc du CGameMode), il faut y passer. Si ce
n'est qu'une garde portant sur une fenêtre qu'on a supprimée (48), il faut s'en passer.

Mises en forme relevées : `0x00E0` = `W opcode | L rôle | 24B nom` (30 o) ;
`0x00E2` = `W opcode | 24B nom` (26 o) — conformes à `docs/opcode_map.csv`.
La longueur part de `PacketLen_Get`, pas d'un littéral.

### 10bis.6 Ce qui reste à mesurer

- Le **contenu** des deux panneaux (`0x00834AD0` historique, `0x00836130` liste) : ce sont des
  contrôles de liste génériques. Un remplacement tient son propre modèle et n'en a pas besoin.
- La marque du **propriétaire** dans la liste (msg 43 pose un drapeau ; reste à voir comment il
  est rendu) et le **menu contextuel** sur un membre (expulser / céder).
- `ZC_ENTER_ROOM 0x00DB` (`sub_CBBDE0`) : la liste complète des membres à l'entrée dans le
  salon d'un AUTRE joueur — c'est le chemin « rejoindre », pas le chemin « créer ».

### 10bis.7 ✅ Ce que le REMPLACEMENT a appris en jeu (2026-08-23)

Sept faits qu'aucune lecture statique n'avait donnés, et qui ont tous coûté un aller-retour.

**1. 🔴🔴 `g_UIChatRoomWnd_Slot` est LE commutateur du chat.** Les handlers ZC de messages
aiguillent eux-mêmes, en toutes lettres (`0x00C9E785` par exemple) :

```c
if (g_UIChatRoomWnd_Slot) ChatAction(5, texte, 0x222222, 0, 0);   // -> le SALON
else                      ChatAction(1, texte, 0xFFFF00, 1, 0);   // -> le chat général
```

Un remplacement qui **détruit** la fenêtre 28 met ce slot à zéro en permanence : **tout le
contenu d'un salon privé ressort dans la chatbox générale**. Et le symptôme est
INTERMITTENT — `ZC_MEMBER_NEWENTRY` recrée la fenêtre, qui vit jusqu'au tick suivant, donc
quelques lignes arrivent au bon endroit. Il faut reprendre l'aiguillage soi-même.

C'est sûr de le faire : le serveur envoie le chat public en `AREA_CHAT_WOC` — *« hearable
area, **without chatrooms** »* (`src/map/clif.hpp`) — donc **un occupant de salon ne reçoit
pas le chat de la carte**. Toute ligne de type 1 reçue en salon vient du salon.

**2. 🔴 Le même slot garde TROIS commandes**, et pas seulement les deux déjà notées :
`SendMsg(44)` (modifier) et `SendMsg(48)` (quitter) refusent en silence, et l'aiguillage
ci-dessus bascule. Règle à retenir : **avant de détruire une fenêtre native, chercher son
slot de manager dans le reste du binaire** — ici trois consommateurs, tous muets en cas
d'échec.

**2bis. ✅ LES 20 XREFS DU SLOT, TOUTES OUVERTES (2026-08-24).** Le point 2 dit de chercher le
slot dans tout le binaire ; voici le résultat, parce que « 20 consommateurs » sans le détail
mène à surestimer les dégâts — trois avaient été catalogués « régressions » avant lecture, et
aucun n'en était une.

| Sites | Ce que le slot y fait | Statut |
|---|---|---|
| `0x00C9E785`, `0x00CA189B`, `0x00CA4582` (`RecvLoop_DispatchPackets`), `0x00CC84AF` (`ZC 0x008D`), `0x00CCBDE0` (`ZC 0x008E`), `0x00CCEEE7` | aiguillage `ChatAction(5)` / `ChatAction(1)` | **repris** (`ClaimPublicChatLine`) |
| `0x00C8C2D1`, `0x00C8C6AC`, `0x00C8C7D5`, `0x00C8DCE4` (dans `CMode::SendMsg`) | gardes de `SendMsg(44/48/…)`, refus SILENCIEUX | **contourné** (paquet brut) |
| `0x0095F6B2` (`UIBasicInfoWnd_OnMsg`) | garde « déjà dans un salon » | **rejoué** |
| `0x00C7627E`, `0x00C74BEA`, `0x00C759C7` | **PAS un blocage de déplacement** | **sans effet** ↓ |
| `0x00958B12`, `0x00D0A002`, `0x00D0A0E2` | modale `MSI 587` | **sans effet** ↓ |
| `0x00CBB80B`, `0x00CC88B9`, `0x00CE4B25` | hors fonction dans l'IDB, zone des handlers ZC de chat | non ouverts |

**Les trois sites de « déplacement » sont la MÊME branche, écrite trois fois** — clic au sol,
clic maintenu (boucle de frame) et pas de suivi d'une cible :

```c
if (g_UIChatRoomWnd_Slot || *(mode+0x70) == 6)  SendMsg(16, x, y);   // brut
else { /* Cell_IsMoveTargetValid, Move_ClampToReachableCell, ferme le menu 18 */
       SendMsg(17, x, y); }                                          // marche
```

Ce n'est donc pas une garde : c'est un **choix de sélecteur**, `push 10h` au lieu de `push 11h`,
qui saute au passage tout le pathfinding local. Le blocage réel est au SERVEUR et il est
inconditionnel — `pc_cant_act(sd)` inclut `sd->chatID` (`pc.hpp:1261`). Slot à zéro, le client
calcule un chemin pour rien et envoie 17 : le serveur refuse pareil. *(Ce que fait exactement
le sélecteur 16 n'est pas tranché — la table de dispatch de `CMode::SendMsg` retombe en plein
milieu de queues d'envoi partagées.)*

**Et les trois sites « échoppe »** — le bouton OK de `UIMerchantShopMakeWnd`, plus les deux
handlers de réception `ZC 0x0A7E` / son jumeau « buying store » — affichent la modale
`MsgString 587 = MSI_CANT_OPEN_STORE_WHILE_CHAT` puis annulent par `SendMsg(297)`. Vraie garde,
mais **inatteignable** : `skill_check_condition_castbegin` (`skill.cpp:8377`) commence par
`if (sd.chatID) return false;` — **aucun skill n'est lançable en salon**, donc `MC_VENDING` ne
part pas, `state.prevend` reste à 0 et `vending_openvending` sort en 1. Politesse du client,
pas un verrou.

⇒ **La leçon du point 2 tient, mais il faut la finir : ouvrir les N sites.** Le slot porte
quatre gardes qui comptent (le chat, deux `SendMsg`, la garde de Basic Info) et une dizaine de
raffinements que le serveur reprend derrière. Compter les xrefs dit l'ampleur du RISQUE, jamais
celle du dégât.

**3. 🔴 On ne reçoit JAMAIS l'écho de ses propres lignes.**
`clif_parse_GlobalMessage` route en `CHAT_WOS` = *« current chatroom, **without self** »*.
Et le client n'écho pas non plus (le `case 6` de `CMode::SendMsg` n'appelle `ChatAction` que
pour « TOO FAST CHATTING »). **Une salle de remplacement doit fabriquer son propre écho**, au
format `nom : texte` que monte `clif_process_message`.

**4. 🔴 CORRECTION de §10bis.5 — `cmdId 0` et `cmdId 0x32` sont le MÊME `case`** de
`Chat_HandleChatMessage` (même entrée de table de saut). La substitution que fait le natif ne
change donc **pas** la destination : c'est le SERVEUR qui route, sur `sd->chatID`. Les deux
finissent sur `SendMsg(6)` → `CZ_REQUEST_CHAT 0x008C`. La substitution reste à faire par
fidélité, mais il ne faut rien en attendre.

**5. La couleur des lignes de salon est `0x222222`** — donnée par le client lui-même dans
l'appel ci-dessus. Sombre, parce que le corps d'une fenêtre RO est CLAIR. Tout habillage
repris du chat général (jaune vif, gris pâle) y est illisible.

**6. Sélectionner un membre dans la liste transforme la saisie en CHUCHOTEMENT** vers lui
(`0x008822AF`) : si un membre est sélectionné, que le texte ne commence pas par `/` et que ce
n'est pas soi-même, la saisie part en `SendMsg(0x0B, nom)` au lieu du salon. Fonctionnalité
native discrète, à ne pas perdre.

**7bis. 🔴🔴 UN SALON N'A PAS BESOIN DE SAISIE À LUI.** Le fait qui commande tout le
reste est au serveur, une ligne de `clif_parse_GlobalMessage` :

```c
clif_GlobalMessage(*sd, output, sd->chatID ? CHAT_WOS : AREA_CHAT_WOC);
```

Dès que `sd->chatID` est posé, **un message ordinaire part au salon** — le client n'a rien à
faire de particulier, et la barre de la chatbox y suffit telle quelle. La saisie propre à la
fenêtre 28 n'est donc pas un chemin d'envoi distinct : c'est un second champ qui fait la même
chose, plus près des yeux.

Conséquence pour un remplacement : ne pas en écrire une. On **déplace** la barre de la
chatbox dans la fenêtre du salon le temps qu'il dure (`chatwnd::DrawChatInputRow`), et la
chatbox cesse de la dessiner — un `InputText` peint deux fois sous le même identifiant se
dispute le clavier avec lui-même. On récupère gratuitement la box destinataire, le sélecteur
de mode d'envoi, les préfixes `%`/`$`/`^`, le sélecteur d'emotes, les chips de liens et
l'historique ↑/↓, dont pas une ligne n'avait à être recopiée.

Il ne reste alors **qu'un seul** écart de fidélité avec la saisie native : la substitution
`cmdId 0x33 → 0x34` (« /savechat » depuis un salon), portée par `NativeSendChatText`. L'autre
substitution du natif (`0 → 0x32`) ne change rien — point 4 ci-dessus.

Et le point 6 s'en trouve simplifié : chuchoter à un membre, c'est écrire son nom dans la box
destinataire de cette barre. Le natif le faisait en silence, sur une sélection invisible ; là,
le joueur LIT à qui il parle.

**7. ⚠ Deux « bugs » qui n'en sont pas, sur un personnage GM.** `chat.cpp:151`
`!pc_has_permission(sd, PC_PERM_JOIN_ALL_CHAT)` (clé `join_chat`) laisse **entrer dans un
salon privé sans mot de passe** ; `chat.cpp:388`
`if (pc_has_permission(cd->usersd[i], PC_PERM_NO_CHAT_KICK)) return 0;` (clé `kick_chat`)
rend un GM **inexpulsable**, en silence. Les deux clés sont à `true` pour les groupes staff de
`conf/import/groups.yml`. Toute recette de test du salon se fait avec un personnage
ORDINAIRE.

---

---

## 11. Défauts de la fenêtre native — la liste qui justifie le portage

1. **La saisie est perdue sur refus serveur** (§9.1). Le pire cas : « un salon du même nom existe
   déjà » — l'erreur la plus fréquente, et la plus facile à corriger d'un caractère… si le champ
   existait encore.
2. **Aucun retour d'échec silencieux.** Cinq des huit gardes serveur (§8) ne renvoient **rien** :
   déjà dans un salon, vending, `SC_NOCHAT`. La fenêtre se ferme, rien ne se passe, aucun message.
3. **Basic Skill 4 non signalé.** Le client laisse cliquer puis affiche un « échec de compétence »
   incompréhensible.
4. **La limite est un déroulant fermé de 6 valeurs, dont 3 seulement visibles** (popup 48 px,
   §5.2). Impossible de saisir 7, 12, 18. Le serveur, lui, accepte tout jusqu'à 20.
5. **Le déroulant « type » est un leurre** : un seul item, valeur jamais transmise. Il occupe la
   moitié droite de la barre.
6. **Le mot de passe est masqué sans possibilité de le révéler**, avec une règle « ≥ 4 caractères »
   qui n'apparaît qu'après avoir cliqué OK.
7. **`"Alt+C"` est écrit en dur** et ment dès que le joueur remappe ou n'est pas en QWERTY (§2.4).
8. **Aucune mémoire** : ni le dernier titre, ni la dernière limite, ni le public/privé ne sont
   restaurés. Le `CGameMode+0x3C8` existe pourtant déjà.
9. **Aucun aperçu** : impossible de savoir à quoi ressemblera le panneau au-dessus de la tête.
10. **Titre limité à 36 octets**, sans compteur — et en CP949 un caractère accentué ou coréen en
    coûte deux. Le joueur découvre la troncature après coup.
11. **Aucune vue des salons alentour** alors que le client reçoit déjà tout (`0x00D7`, §9.2) :
    impossible de savoir que le nom est pris avant de se le faire refuser.

---

## 12. Blueprint — portage ImGui

### 12.1 Emplacement et forme

`src/features/windows/chat_room_window.{h,cc}`, classe `ChatRoomWindow : public Plugin`, sur le
modèle de `bank_window` (fenêtre unique, skin RO, pas de pont natif). Membre du groupe
« Interface moderne » (`SetModernInterface`) — cohérence avec le reste.

### 12.2 Interception : détruire, jamais masquer

🔴 **Non négociable** (§3.4) : la native déclare un bouton par défaut qui **envoie le paquet**.
Recette éprouvée (storage, character_sheet) :

1. dans le hook `MakeWindow` déjà en place, sur `id == 27` (et `id == 30`) : masquer `+0x28 = 0`
   (anti-scintillement — on ne peut pas détruire là, le natif manipule encore l'objet) ;
2. au `OnTick` suivant : `uiwnd::CloseWindow(27)` / `(30)`.

Vérification faite : **ni le ctor, ni `OnCreate`, ni le dtor n'émettent de paquet** — la seule
émission est la commande 184. Détruire tôt est donc **sans effet de bord** (contrairement à
RODEX 0x108, cf. `reference_native_window_toggle_router`).

À rejouer nous-mêmes, puisque plus personne ne les porte :

- la garde `ChatRoom_IsCellBlockedByRoomTitle` (`0x00A38C60`) + message de chat `MSI 661` ;
- les gardes vending de `ChatRoom_OpenMakeWnd` (`MSI 587`) et de `UIBasicInfoWnd::OnMsg` cmd 214 ;
- le filtre de mots interdits `sub_A85BE0(0x0159C2C8, titre)` ;
- le test ASCII `sub_D71EF0` **si** `g_ServiceType == 1`.

Les trois ouvertures (`ExecOption(0x17)`, `/chat`, Basic Info cmd 214) convergent toutes sur
`MakeWindow(0x1B)` : **un seul point d'accroche suffit**, et notre propre bouton peut appeler
directement `gamesettings::ExecOption(0x17)` pour rester sur le chemin natif.

### 12.3 Envoi

🔴 **TRANCHÉ EN JEU (2026-08-23), et l'inverse de ce que ce paragraphe recommandait d'abord :
il FAUT passer par `CMode::SendMsg(43)`.**

Le paquet brut a été essayé en premier — la mise en forme est entièrement connue (§7.3), et le
salon se créait bel et bien. Mais sa fenêtre s'intitulait **« Publ. : (1/-657931) »** : titre
vide, limite aberrante (`-657931` = `0xFFF5F5F5`, une constante de couleur lue à côté).

Parce que le `case 43` ne fait **pas que sérialiser**. Avant d'émettre, il recopie la demande
dans un bloc « salon courant » du `CGameMode` (§7.2), et c'est **ce bloc** — pas le paquet, pas
la réponse serveur — que la fenêtre de salon lit pour s'intituler
(`"%s%s (%d/%d)"` @0x010317D4, monté dans son `OnMsg` à 0x00882F46 depuis `mode+0x3C8`,
`+0x3F8`, `+0x3FC`, `+0x400`). Et le propriétaire ne reçoit **jamais** `ZC_ROOM_NEWENTRY` pour
son propre salon — `clif_dispchat` diffuse en `AREA_WOSC`, qui exclut la source — donc cette
copie locale est sa **seule** source de vérité. Troisième devoir du même bloc :
`ChatRoom_RecordToReplay` (0x00C810A0) le relit pour écrire la création dans le replay.

Coût réel du chemin natif : deux `std::string` construites par le CRT **du jeu**
(`0x004E5330`) et leurs destructeurs (`0x004F08F0`). Très peu payé pour ne rien oublier.

➡ **Règle générale, au-delà de cette fenêtre** : quand un chemin natif fait plus qu'émettre,
le rejouer à la main est un pari, pas une simplification. Le symptôme, lui, apparaît loin de la
cause — ici dans le titre d'une AUTRE fenêtre.

`CZ_CHANGE_CHATROOM 0x00DE` est strictement identique : **un seul écran** sert les deux cas, avec
un drapeau « création / modification ».

### 12.4 L'écran, tel qu'il devrait être

```
┌ Salon de chat ─────────────────────────────────── [Alt+C] ─ X ┐
│ Titre    [_________________________________________]  27/36 ▐ │
│ Limite   [ 2 ][ 3 ][ 5 ][10][15][20]  ou  [====|====] 12 pers.│
│ Accès    ( ) Public   (•) Privé                                │
│ Mot de passe [••••••••] 👁   min. 4 caractères                 │
│ ─────────────────────────────────────────────────────────────  │
│ Aperçu :   ▛▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▜                          │
│            ▌ Chasse MVP — venez !  ▐  0/12                     │
│            ▙▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▄▟                          │
│ ⚠ « Chasse MVP » existe déjà à 4 cases d'ici.                  │
│ ─────────────────────────────────────────────────────────────  │
│ Salons proches (3)                    [ Créer ]  [ Annuler ]   │
└────────────────────────────────────────────────────────────────┘
```

**Reprises fidèles** (le natif reste la référence) : titre ≤ 36 **octets** CP949, mot de passe ≤ 8,
mot de passe ≥ 4 si privé, limite ≤ 20, textes pris dans la `MsgStringTable` via `msgstr::Utf8`
(ids 13, 14, 15, 16, 64, 65, 66, 125, 126, 130, 131, 190, 200, 201, 587, 661, et les
**4316–4320 déjà traduits** pour les libellés que le natif n'avait pas).

**Ce que l'UI moderne apporte, dans l'ordre de valeur :**

| # | Fonctionnalité | Ce qu'elle corrige |
|---|---|---|
| 1 | **La fenêtre ne se ferme qu'à l'ACK `0x00D6`** ; sur code 1/2 elle reste ouverte, surligne le champ fautif et affiche l'erreur **dans** la fenêtre | défaut 1 et 2 |
| 2 | **Brouillon persistant** (`SaveData\`, comme les autres fenêtres du projet) : titre, limite, accès, et un **historique des 10 derniers titres** en menu déroulant | défaut 8 |
| 3 | **Annuaire des salons proches**, alimenté par `ZC_ROOM_NEWENTRY 0x00D7` : titre, remplissage `users/limit`, cadenas si privé, distance ; clic = rejoindre (`CZ 0x00D9`, avec invite de mot de passe) ; **avertissement en direct si le titre saisi est déjà pris** | défauts 11 et « salon du même nom » |
| 4 | **Limite libre de 2 à 20** (curseur `WheelSlider` + boutons rapides), au lieu de 6 valeurs dont 3 visibles | défaut 4 |
| 5 | **Compteur de longueur en octets CP949**, en direct, rouge au-delà de 36 | défaut 10 |
| 6 | **Pré-vol des gardes serveur** : Basic Skill < 4, PNJ à moins de 3 cases, carte/cellule `NOCHAT`, vending en cours, déjà dans un salon ⇒ bouton **Créer** désactivé avec la raison écrite. Tout est lisible côté client (`player_skills.h`, `entity_nameplate`/acteurs pour les PNJ, `session`) | défauts 2 et 3 |
| 7 | **Aperçu du panneau** rendu au-dessus du formulaire, avec la couleur réelle (public/privé) | défaut 9 |
| 8 | **Œil « révéler le mot de passe »**, règle des 4 caractères affichée **avant** le clic | défaut 6 |
| 9 | **Raccourci réel** lu par `userhotkey::` (layout-aware) au lieu de `"Alt+C"` en dur | défaut 7 |
| 10 | **Le déroulant « type » disparaît**, remplacé par le couple Public/Privé + le mot de passe (qui *est* le vrai type) | défaut 5 |
| 11 | **Mode « modifier »** : la même fenêtre, pré-remplie depuis `CGameMode+0x3C8…` (ou le `msgType 34`), envoie `0x00DE` — et là le maintien à l'écran jusqu'à `ZC 0x00DF` a le même bénéfice | — |
| 12 | **Bouton « réutiliser le dernier salon »** : un clic, tout est repris | défaut 8 |

**À ne PAS faire** (mensonges d'interface) :
- proposer plus de **20** places (`MAX_CHAT_USERS`) ;
- proposer plus de **36** octets de titre ou **8** de mot de passe ;
- ouvrir les accents dans le titre à la légère : `g_ServiceType == 1` est **mesuré** sur Moonlight,
  le client natif refuse tout octet ≥ 0x80, et lever la règle engage le fil bi-encodage (§6) ;
- garder la native ouverte « au cas où » : elle enverrait le paquet sur une frappe d'Entrée.

### 12.5 Ordre de marche proposé

1. **Lot 1 — parité. ✅ LIVRÉ** (`src/features/windows/chat_room_window.{h,cc}`).
   Interception par le hook `MakeWindow` de `window_pos_tweaks` (id 27) : la native est
   masquée à la naissance puis DÉTRUITE au tick. Formulaire ImGui skin RO, paquet `0x00D5`
   brut, textes pris dans la `MsgStringTable` (ids 13/14/15/16/65/66/125/131/190/200/201/661
   et les 4316/4317/4320 que le natif n'utilisait pas). Gardes rejouées : case occupée
   (`ChatRoom_IsCellBlockedByRoomTitle`), mots interdits, ASCII 7 bits sous `g_ServiceType`,
   plafonds 36 / 8 / 20, mot de passe ≥ 4 si privé. **La fenêtre reste ouverte jusqu'à
   l'ACK `0x00D6`** et affiche le refus à l'intérieur, champ fautif surligné.
   S'y ajoutent, parce qu'ils ne coûtaient rien une fois le formulaire à nous : compteur
   d'octets CP949 en direct, avertissement ASCII pendant la frappe, limite libre 2→20 au
   curseur, œil « révéler le mot de passe », règle des 4 caractères dite avant le clic,
   champ mot de passe grisé sur un salon public, saisie conservée d'une ouverture à l'autre.
   Réglage `chatroom_imgui` (défaut ON, hors du groupe « Interface moderne »).
2. **Lot 2 — mémoire.** Brouillon + historique des titres + « réutiliser le dernier salon ».
3. **Lot 3 — la SALLE. ✅ LIVRÉ** (même module, seconde fenêtre). Fenêtre à part, volet des
   messages à gauche et volet des membres à droite (la proportion 80/20 du natif), saisie en
   bas, boutons Quitter / Réglages. Interception de la native 28 par le hook `MakeWindow`
   — ⚠ **sans bascule** : elle naît d'un ÉVÉNEMENT, pas d'une demande du joueur.
   Ingestion des lignes par le filtre `ChatAction` action 5 de `ChatWindow` (le seul détour
   possible sur `0x00A4AD20`). Membres et état tenus depuis `ZC 0x00DB / 0x00DC / 0x00DD /
   0x00DF / 0x00E1`. Envoi par le chemin natif avec la substitution `cmdId 0 → 0x32`, et
   commandes par `SendMsg(46/47/48)` — le tout ARMÉ pendant la frame et joué par
   `FlushPending`, hors frame.
   Ajouts par rapport au natif : marque du propriétaire dans la liste, **menu contextuel sur
   un membre** (expulser / céder les droits — le natif n'en a aucun), volet de réglages
   INTERNE (le natif en fait la fenêtre 30, détachée), et la croix qui QUITTE vraiment le
   salon (comme le `/q` natif) au lieu de masquer.

   ✅ **Rendu riche** — les lignes du salon ne sont PAS dessinées ici. Elles entrent dans le
   modèle de `ChatWindow` sous un tag de conversation réservé (`"\x01salon"`, qu'aucun nom de
   personnage ne peut porter), et c'est lui qui les peint : balises `<ITEML>`, liens d'objets
   et de monstres, icônes `^i[]`, emotes du jeu et de Discord, gras/italique, couleurs
   `^RRGGBB`, et les clics qui vont avec. En écrire un second aurait fait une **sixième** copie
   du même rendu (`project_link_label_widget_todo`).

   ✅ **La barre de saisie est celle de la chatbox**, déplacée ici tant que le salon vit
   (§10bis.7-7bis) : une seule saisie, dans la fenêtre qu'on regarde, avec sa box
   destinataire, son mode d'envoi, ses emotes, ses chips de liens et son historique. La
   chatbox affiche à la place un mot cliquable qui ramène le salon devant. L'envoi natif
   `SendRoomChatText` a donc disparu du module — sa recette reste ici, §10bis.5.
4. **Lot 3 bis — annuaire des salons proches.** Écoute de `ZC 0x00D7`/`0x00D8`, liste des
   salons alentour, détection du doublon de titre AVANT le refus, entrée dans un salon
   (`SendMsg(45)` → `CZ 0x00D9`).
5. **Lot 4 — pré-vol.** Basic Skill, PNJ proche, cellule/carte `NOCHAT`, vending, salon en cours.
6. **Lot 5 — modification.** Mode `0x00DE` et le reste de la vie du salon (chef, expulsion) — au
   besoin dans une seconde fenêtre, adossée à `UIChatRoomWnd` (id 28), qui reste à documenter.

---

## 13. Annuaire des adresses (client 20250716, base `0x400000`)

| Adresse | Nom (IDB) | Rôle |
|---|---|---|
| `0x0088D160` | `UIChatRoomMakeWnd_ctor` | ctor, pose `MSI 125` comme titre |
| `0x0088D000` | `UIChatRoomChangeWnd_ctor` | ctor dérivé |
| `0x0086B7F0` | `UIChatRoomWnd_ctor` | ctor de la salle (id 28) |
| `0x008A1C30` | `UIChatRoomMakeWnd_OnCreate` | **partagé** Make/Change — géométrie, listes, gardes |
| `0x008BE010` | `UIChatRoomMakeWnd_OnMsg` | commandes 184/185/201/215/228/229, `msgType` 6 et 39 |
| `0x008BDA70` | `UIChatRoomChangeWnd_OnMsg` | ex-`UIWnd0x1e_OnMsg`, `msgType` 34 = pré-remplissage |
| `0x008922E0` / `0x00892270` | `UIChatRoomMakeWnd_scalar_dtor` / `…Change…` | destructeurs |
| `0x00692D70` | `ChatRoom_OpenMakeWnd` | entrée unique, action `TalkType 0x17` |
| `0x00692690` | (dans `CGameSettingsMgr_Init_Func`) | site d'enregistrement de l'action `0x17` |
| `0x0068E160` | `CGameSettingsMgr_ExecOption` | exécute une action `TalkType` (type EXE) |
| `0x00A38C60` | `ChatRoom_IsCellBlockedByRoomTitle` | panneau déjà présent sur ma case |
| `0x0095F3B0` | `UIBasicInfoWnd_OnMsg` | vtable `0x0103E35C`+0x94 ; **cmd 214** = ouvrir |
| `0x00C86740` | `CMode::SendMsg` | msg **43** → `0x00D5` (bloc `0x00C8C590`), msg **44** → `0x00DE` (bloc `0x00C8C6AC`) |
| `0x00CA1701` | (dans `RecvLoop_DispatchPackets`) | handler `ZC_ACK_CREATE_CHATROOM 0x00D6` |
| `0x008841D0` | `UIWindow_OnMsg_Default` | `this[35]`=Entrée, `this[36]`=Échap |
| `0x00A2E270` | `UIWindowMgr_ActivateDefaultButton` | l'appelant d'Entrée |
| `0x00817DA0` / `0x008172B0` | `UIEdit_ctor` / `UIBitmapButton_ctor` | `+0x84` masque, `+0x88` longueur max |
| `0x00818360` / `0x008188D0` | `UIRadioBtn_ctor` / `UIStaticText_ctor` | contrôles du formulaire |
| `0x00894FA0` | `UIWindow_AddCloseButton` | croix, commande **201** |
| `0x00A31A30` | `UIWndMgr_ShowMessageBoxModal` | boîtes d'erreur 280×120 |
| `0x0159C2C8` | (table) | mots interdits, testée par `sub_A85BE0` (`0x00A85BE0`) |
| `0x00D71EF0` | `Str_IsPureAscii7` | « tous les octets < 0x80 » — **actif sur Moonlight** |
| `0x0159B810` | `g_ServiceType` | `1` ⇒ titre ASCII imposé |
| `0x0131F6FC` | `g_UIComboBoxWnd_Slot` | popup déroulant (id 26) ouvert |
| `0x0131F764` | `g_UIChatRoomWnd_Slot` | je suis dans un salon (id 28) |
| `0x0131F7E4` | `g_VendingShopMakeWnd` | montage d'échoppe ouvert |
| `0x0131F7EC` | — | échoppe en cours (vending actif) |

---

## 14. Ce qui reste à faire

- **`UIChatRoomWnd` (id 28)** — ✅ **documentée en §10 bis**. Reste le détail interne des deux
  panneaux de liste (`0x00834AD0`, `0x00836130`) et le menu contextuel sur un membre.
  ⚠ Son `OnMsg` est **`0x00881F30`** (0x1FF4 octets), que l'IDB nommait
  `UI_CharSelect_HandleEvent` — **faux**. Il lui appartient EN PROPRE : `UINewChatWnd::OnMsg`
  est une autre fonction (`0x008FC220`, vtable `0x01037F80`), vérifié par RTTI le 2026-08-23.
  S'il contient de la saisie et des commandes slash, c'est simplement que la salle a sa
  propre ligne de saisie.
- **`UIChatRoomTitle`** — le panneau au-dessus de la tête (vtable `0x0102A4E8`) : c'est lui qu'un
  aperçu fidèle devra imiter, et lui que `ChatRoom_IsCellBlockedByRoomTitle` inspecte.
- **Vérification en jeu** : ✅ `g_ServiceType == 1` et base `0x400000` **mesurés** le 2026-08-23
  (x32dbg, `moonlight-destiny.exe`). Reste à observer en conditions réelles les codes 1 et 2 de
  `ZC 0x00D6` (créer deux salons du même nom) pour confirmer qu'aucun autre effet n'accompagne
  le message de chat.
