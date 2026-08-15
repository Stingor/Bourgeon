# « Shortcut List » — la fenêtre des macros de chat (Alt+M)

Cible : `2025-07-16_Ragexe_175220998_clientinfo.exe` (imagebase `0x400000`).
RE du 2026-08-15. IDB renommée et commentée. La fenêtre était **ouverte en jeu
avec x32dbg attaché** : tout ce qui porte **✅live** a été relu dans la mémoire du
processus, pas seulement déduit du décompilé.

C'est la fenêtre à dix lignes « Alt + 1 … Alt + 0 » qui associe une touche à un
texte de chat (`@load`, `/lv`, …) et l'envoie d'une frappe.

---

## 0. Trois pièges de nommage, à lire avant tout

**🔴 La classe s'appelle `UIEmotionWnd` et n'affiche aucune émote.** Son nom, son
dossier de bitmaps (`\ShortCutList\`), son titre (`MSI_SHORTCUT_LIST`), sa
catégorie de raccourcis (« Macros ») et sa sauvegarde (`EmotionHotkey`) portent
**cinq vocabulaires différents pour un seul objet**. Chercher « macro » dans
l'image ne mène qu'à `UIMacroRegisterWnd` / `UIMacroDetectorWnd` /
`CUIMacroReport`, qui sont l'anti-bot et n'ont **rien à voir**.

**🔴 `0x00D9D400` s'appelait `UIShortCut_LoadListFromLua` dans l'IDB** — nom
hérité qui la faisait passer pour un morceau de la barre de raccourcis de skills
(cf. [[project_shortcut_bar_re]], qui la cite à tort dans ses helpers). C'est le
chargeur des **macros**. Renommée `EmotionHotkey_LoadListFromLua`.

**🔴 `0x015FA3C0` est le même objet que le `g_SkillInfoMgr` de la barre de
skills** (IDA l'appelle `g_UIWindowContextKey`). Ce n'est pas un gestionnaire de
compétences : c'est le grand sac d'état de l'interface. Les macros y vivent à
`+0xFD8`, les slots de la barre à `+0x490`.

---

## 1. Identité

| | |
|---|---|
| Classe | `UIEmotionWnd` (RTTI absent des chaînes ; la vtable est nommée) |
| Id de fenêtre | **86** (`0x56`) ✅live (`this+0x2C` == 86) |
| vtable | `0x0104B070` ✅live |
| Taille de l'objet | `0x10C` |
| Cache manager | `g_UIEmotionWnd` = `mgr+0x380` = **`0x0131F868`** ✅live |
| Fabrique | `UIWindowMgr_MakeWindow` case 86 @ `0x00A3DDAC` |
| Taille | **375 × 260** (`0x177 × 0x104`) ✅live |
| Position initiale | `SetPos(Screen_CenterXFrom640(185), UI_ScaleYFrom480(300) − 60)` |
| Titre | `MsgStringTable[574]` = `MSI_SHORTCUT_LIST` = « Shortcut List » |

`g_UIWindowMgr` = `0x0131F4E8`. Le voisin immédiat `mgr+0x384` = `0x0131F86C` est
la liste d'émotes `UICashEmotionListWnd` (id **87**), celle qu'ouvre le bouton
*view* — ne pas confondre les deux slots.

### Table des méthodes utiles (vtable `0x0104B070`)

| Slot | Adresse | Rôle |
|---|---|---|
| +0x10 | *(base)* | `SetPos(x, y)` |
| **+0x2C** | **`0x0086B430`** | `OnSaveState` — **le seul chemin d'écriture des macros** (§5) |
| +0x3C | `0x0086ABD0` | `OnCreate` — construit les 10 lignes (§2) |
| +0x50 | `0x0086B250` | `OnDraw` — ne fait que la barre de titre |
| +0x64 | `0x0086B290` | `OnLButtonDown` (délègue à la base si `msg < 17`) |
| +0x94 | `0x0086B2B0` | `OnMsg` (§4) |

Le slot **+0x2C** est générique : `UIShortCutWnd` y met `0x00906E80`, qui se
contente de recopier `x/y/w/h` dans l'enregistrement de géométrie sauvé.
`UIEmotionWnd` y fait **les deux** : la géométrie *et* le report des dix champs
de saisie.

---

## 2. Anatomie — `UIEmotionWnd_OnCreate` `0x0086ABD0`

Boucle `for (i = 23 ; i < 233 ; i += 21)` — dix lignes, pas vertical **21 px** :

```
      20                 83                                353
  ┌────┴──────┐      ┌────┴───────────────────────────────────┐
  │  Alt + 1  │      │ @load                                  │   y = 20   (i-3)
  │  Alt + 2  │      │ @storeall                              │   y = 41
  …                                                              … + 21
  │  Alt + 0  │      │ /...                                   │   y = 209
                                              ┌──────┐
                                              │ view │  (325, 235) = (w-50, h-25)
                                              └──────┘
```

- **Champ de saisie** : `UIEdit` (ctor `0x00817A50`, objet `0x11C`), **270 × 16**
  en `(83, i−3)`, **longueur max 50** (`edit+0x88`), couleur `232,232,232`,
  texte initial = `g_EmotionHotkeyMacros[n]`.
- **Libellé** : contrôle texte (ctor `0x008188D0`, objet `0xD8`), **70 × 16** en
  `(20, i)`, texte `"Alt + %d"` — le dixième est écrit en dur `"Alt + 0"`.
- **Bouton fermer** : `UIWindow_AddCloseButton(this, "Alt+M", 0)` — l'étiquette
  est la touche, pas un libellé.
- **Bouton *view*** : `UIBitmapButton`, bitmaps `\ShortCutList\btn_view.bmp` /
  `_a` / `_b`, commande **283**, placé en `(largeur−50, hauteur−25)`.
- Enfin : focus sur la **première ligne** (`0x00A4B760`) et curseur en fin
  (`0x0081D430(0xFF)`) — c'est le curseur visible sur la ligne 1 à l'ouverture.

### Champs de l'instance ✅live

| Offset | Contenu |
|---|---|
| `+0x00` | vtable `0x0104B070` |
| `+0x14` / `+0x18` | largeur / hauteur (375 / 260) |
| `+0x1C` / `+0x20` | x / y à l'écran |
| `+0x28` | visible |
| `+0x2C` | **id de fenêtre = 86** |
| `+0xB4 … +0xD8` | 10 pointeurs de **libellés** (`Alt + n`) |
| `+0xDC … +0x100` | 10 pointeurs de **champs de saisie** |
| `+0x104` | enregistrement de géométrie sauvée `{x, y, w, h}` (posé par `OnMsg 34`) |
| `+0x108` | bouton *view* |

### 🔑 Les libellés ne sont « Alt + n » que par défaut

La fin de `OnCreate` boucle `n = 0..9` sur
`UserHotkey_Lua_GetHotKey(out, catégorie = 2, n)` et, **si le libellé `EXE`
(`out+0x20`) est non vide**, remplace `"Alt + n"` par le **nom de touche réel**
(`out+0x08`). La catégorie 2 est l'onglet **« Macros »** de *Shortcut Settings*
(`UserHotkey_TabIndexToCategory` : onglet 3 → catégorie 2, Lua `USERKEY_3`).
Voir [[project_hotkey_settings_window_re]] pour la règle « `str2` vide ⇒ la ligne
n'existe pas », qui est exactement le test employé ici.

Chez l'utilisateur, `UserKeys.lua` n'a **aucune** entrée `USERKEY_3` : les dix
libellés restent donc les `"Alt + n"` en dur, ce que montre la capture.

---

## 3. Le contenu : dix chaînes, et où elles vivent

Un `std::vector<std::string>` de **dix** entrées, à `mgr+0xFD8` :

| Adresse | Rôle |
|---|---|
| `0x015FB398` | `g_EmotionHotkeyMacros` — `_Myfirst` du vecteur **vivant** |
| `0x015FB39C` / `0x015FB3A0` | `_Mylast` / `_Myend` |
| `0x015FB3A4` | `g_EmotionHotkeyMacrosLoaded` — `_Myfirst` de l'**instantané chargé** |
| `0x015FB3A8` / `0x015FB3AC` | idem |

✅live : `_Mylast − _Myfirst = 0xF0` = 10 × 24 (`std::string` MSVC), et les deux
vecteurs contenaient mot pour mot `@load`, `@storeall`, `@storage`, `/lv`,
`/swt`, `/ic`, `/an`, `/ag`, `/$`, `/...` — les dix lignes de la capture.

⚠ **`g_EmotionHotkeyMacros` est un POINTEUR, pas un tableau.** Il faut le
déréférencer avant d'indexer (`*(char**)0x015FB398 + n * 24`). Le décompilé
d'IDA mélange les deux notations d'une ligne à l'autre.

**Le second vecteur n'est pas un doublon** : c'est la photo prise au chargement.
`EmotionHotkey_IsDirty` `0x00D95930` compare les dix paires et renvoie 1 dès la
première différence ; c'est la garde qui décide si le nœud JSON `EmotionHotkey`
est réémis (donc si la synchro web part).

### Les dix valeurs par défaut

`EmotionHotkey_LoadListFromLua` `0x00D9D400` remplit d'abord le vecteur depuis la
table de messages, **puis** écrase avec le fichier du joueur. Les ids pris sont
`0x220, 0x221, 0x222, 0x223`, **`0x225`**, `0x226 … 0x22A` — soit
`/!  /?  /ho  /lv  /swt  /ic  /an  /ag  /$  /...`.

🔴 **`0x224` (`/lv2`) est délibérément sauté.** Un portage qui prendrait dix ids
consécutifs décalerait les sept dernières lignes.

---

## 4. `UIEmotionWnd_OnMsg` `0x0086B2B0`

| Message | Effet |
|---|---|
| `0` | **avalé, sans rien faire** |
| `6`, cmd **201** | `UIWindowMgr_SaveRectAndCloseWindow(86)` = fermeture (destruction) |
| `6`, cmd **283** | bouton *view* : bascule `UICashEmotionListWnd` (id 87) — `MakeWindow` si `0x0131F86C` est nul, fermeture sinon |
| `34` (`0x22`) | `this+0x104` = enregistrement de géométrie, puis `SetPos` |
| autre | `UIWindow_OnMsg_Default` |

🔴 **Le message 0 avalé signifie qu'il n'existe aucune validation par ligne** :
appuyer sur Entrée dans un champ n'écrit rien. Le seul report est celui du §5.

Il n'y a **ni OK ni Annuler** : la fenêtre n'a qu'une croix.

---

## 5. 🔴 Persistance — fermer la fenêtre ne sauvegarde PAS

### Le seul chemin d'écriture

`EmotionHotkey_SaveFromEditBoxes` `0x0086B430` (= slot **+0x2C**) fait, dans
l'ordre : géométrie → `this+0x104` ; texte des dix `UIEdit` →
`g_EmotionHotkeyMacros` ; puis `UserSettings_SaveJson`. **Sa seule référence dans
toute l'image est la vtable** : elle n'est joignable que par appel virtuel.

Les sites qui appellent `vtable[+0x2C]` sont :

1. `UIWindowMgr_DestroyAllWindows` @ `0x00A48526` — pour **toutes** les fenêtres ;
2. certains cas de `UIWindowMgr_SaveRectAndCloseWindow` (61, 62, 63, 64, 69 …),
   chacun câblé en dur sur le slot de cache de SA fenêtre.

**Le cas 86 n'en fait pas partie.** À `0x00A2FC17` il lit `mgr+0x380`, teste, et
saute directement à `QueueDestroyWindow` — aucun `call [eax+2Ch]`, là où le cas
61 juste avant en fait un. Conséquence : **fermer la « Shortcut List » (croix,
commande 201, ou second Alt+M) détruit la fenêtre sans reporter ce qui vient
d'être tapé.** Les macros ne sont enregistrées que si la fenêtre est **encore
ouverte** quand le client détruit tout : retour à l'écran de personnages, ou
fermeture du client.

*Vérification en une frappe si besoin : point d'arrêt sur `0x0086B430`, puis
fermer la fenêtre — il ne doit pas se déclencher.*

### Les deux supports

`UserSettings_SaveJson` `0x0059E950` écrit **les deux** :

- **`savedata\shortcutlist\<AID obfusqué>.lua`** via `SaveShortcutList`
  `0x00DA8270`, au format long-string Lua — donc **aucun échappement** :

  ```lua
  tbl =
  {
  	[=[@load]=],
  	[=[@storeall]=],
  	…
  }
  ```

  Le nom de fichier passe par `Aid_FormatObfuscated 0x00D56E60` (substitution
  chiffre à chiffre `0→3 1→8 2→6 3→7 4→0 5→1 6→2 7→4 8→9 9→5`, tiret avant les
  trois derniers). ✅ vérifié sur le disque du joueur : `6333-338.lua` = AID
  2000001, et les 20 fichiers du dossier sont **20 comptes**, pas 20 sessions.
- le nœud JSON **`EmotionHotkey`** (converti par `g_ClientCodePage`), gardé par
  `EmotionHotkey_IsDirty`, aux côtés de `WhisperBlockList` et des raccourcis
  utilisateur — c'est la charge que la synchro web `/userconfig/save` emporte
  ([[project_external_settings_re]]).

Les autres appelants de `UserSettings_SaveJson` (`UINewSelectCharWnd_OnMsg`,
`CMode_SendMsg_Base`, `Game_MainWndProc`) écrivent, eux, **l'état courant des
globales** — jamais le contenu des champs de saisie.

### Chargement

`EmotionHotkey_LoadListFromLua` `0x00D9D400` (défauts puis fichier) et
`0x0059E0D0` (nœud JSON → les **deux** vecteurs, vivant + instantané).

---

## 6. Les raccourcis

`UIWindowMgr_DispatchHotkeyBehavior` `0x00A451E0` :

| Comportement | Effet |
|---|---|
| **114** | bascule la fenêtre 86 (`Alt+M` par défaut). Garde : `[mgr+0x1DC] != 0` |
| **115** | la liste d'émotes (`0x00A442A0`) |
| **200 … 209** | **envoi de la macro** `slot = comportement − 200` |

L'envoi passe par `ChatMacro_SendEmotionHotkeySlot` `0x00A47400`, déjà décrit
dans [`chatbox_re.md`](chatbox_re.md) §5.4 : c'est le seul chemin d'envoi du
client qui ne dépend d'aucune fenêtre, et Bourgeon en est la copie fidèle
([`chat_window.cc`](../src/features/windows/chat_window.cc)).

⚠ Point ouvert, sans incidence ici : `0x0131F6C4`, la garde du filtre de balises
dans ce chemin, est nommée `g_ChatWordFilterEnabled` dans `chatbox_re.md` et
`g_UIBasicInfoWnd` dans l'IDB. Un des deux noms est faux ; à trancher le jour où
l'on touchera à ce filtre.

---

## 7. Le bouton *view* et la liste d'émotes

| | |
|---|---|
| Classe | `UICashEmotionListWnd` |
| Id | **87** (`0x57`), cache `mgr+0x384` = `0x0131F86C` |
| Taille objet / fenêtre | `0xF0` / **298 × 380** (`0x12A × 0x17C`) |
| Ctor | `0x0078CE10` |

Le lien entre les deux fenêtres est
`EmotionToken_InsertIntoMacroRowOrChat` `0x00589630` : il résout le jeton texte
de l'émote cliquée, puis **si la « Shortcut List » est ouverte** l'insère dans la
ligne **qui a le focus** (`UIEmotionWnd_SetRowText(txt, −1)`, qui compare
`UIWindowMgr_GetFocusedWnd` aux dix `UIEdit`) ; sinon il l'insère dans la saisie
du chat. Aucune ligne focalisée ⇒ rien ne se passe.

---

## 8. ✅ REMPLACÉE — `MacroWindow`

Livrée sur la branche `feat/bourgeon-hotkeys` :
[`src/features/windows/macro_window.{h,cc}`](../src/features/windows/macro_window.h)
pour l'écran, [`src/ragnarok/emotion_hotkey.{h,cc}`](../src/ragnarok/emotion_hotkey.h)
pour le pont typé (lecture / écriture / gravure / envoi / cible courante).
Interception : `window_pos_tweaks.cc` sur l'id 86, masquage à la naissance et
destruction au tick. Réglage `macrolist_imgui`, ON par défaut, **hors** du groupe
« Interface moderne » — comme le menu Échap et l'écran des raccourcis, elle ne
passe que par des structures du client.

Ce que la vue corrige, et par quoi :

| Défaut du natif | Correctif |
|---|---|
| Fermer perd la saisie (§5) | chaque frappe part dans le vecteur du client ; la gravure suit ~500 ms plus tard (`OnTick`) |
| On ignore où part la macro | la cible effective (`g_ChatInputTargetMode` + gardes d'appartenance) est écrite en tête |
| Impossible d'essayer sans fermer | un bouton d'envoi par ligne, via `ChatMacro_SendEmotionHotkeySlot` |
| Troncature muette à 50 octets | compteur `n/50` par ligne, rouge à la limite |
| « Alt + n » même après remappage | repli sur le raccourci d'ORIGINE du client avant le libellé en dur ; bouton vers l'onglet Macros de l'écran des raccourcis |
| Pas de remise à zéro | bouton « Valeurs par défaut », **avec le trou du `0x224`** |
| Pas de réorganisation | glisser une ligne sur une autre échange les deux textes (les touches restent au rang) |
| Champ vide face à un joueur qui ne connaît pas les commandes | **clic droit = catalogue des commandes du serveur**, classées, avec leur effet en clair |

🔴 **Le catalogue du menu contextuel est RELEVÉ SUR LE SERVEUR, pas deviné** —
`conf/atcommands.yml` (alias `gstorage`→`guildstorage`, `noks`→`ksprotection`),
`conf/import/atcommands.yml` (les alias propres à Moonlight : `storage1..5` =
`storagealt1..5`, le 5 étant celui des cartes) et `src/custom/atcommand.inc` (les
commandes maison : `@storeall` prend un numéro de storage 0..5,
`@autolootpognon` un **prix plancher**, `@autolootrare` se limite à une liste du
serveur). Le fichier de base ment quand `conf/import/` le surcharge, ce qui est
justement le cas des storages alternatifs.

⚠ **Régression assumée, une seule** : le bouton *view* n'est pas repris. Son
insertion d'émote teste `g_UIEmotionWnd != 0` (§7) ; notre fenêtre détruisant la
native, le jeton serait parti dans la barre de chat. Un bouton qui fait autre
chose que ce qu'il annonce est pire que pas de bouton — en attendant un
sélecteur d'émotes à nous, qui a déjà sa brique (`ui/game_emotes.h`).

### 🔴 Émote → commande `/xx` : deux plages, pas un décalage

Le menu propose aussi les **émotes**, avec leur sprite. Ce qu'il écrit est la
**commande du client**, jamais le jeton `:sweat:` de la chatbox : celui-ci n'est
rendu en image que par Bourgeon, donc une macro qui l'emploierait n'afficherait
rien chez les autres joueurs ni sur le chat d'origine.

La table de messages porte ces commandes dans un bloc de **trente ids contigus,
544 (`/!`) à 573 (`/ok`)**. Le sprite `emotion.act` compte, lui, quatre actions de
plus dans le même intervalle — `scissor`, `rock`, `wrap`, `flag` : le
pierre-feuille-ciseaux et le drapeau, qu'aucune commande ne déclenche. D'où :

| action du sprite | message |
|---|---|
| 0 … 10 | **544 + id** |
| 11 … 14 | **aucune** — exclues du menu |
| 15 … 33 | **540 + id** |
| > 33 | aucune (hors du bloc) |

Établi par recoupement et non deviné : les cardinalités coïncident exactement
(11 + 19 = 30 commandes pour 30 ids) et les noms concordent sur toute la
longueur — `/swt`↔sweat, `/ic`↔aha, `/ag`↔anger, `/$`↔money, `/...`↔think,
`/sry`↔sorry, `/swt2`↔profusely_sweat, `/kis`↔chup, `/kis2`↔chupchup, `/ok`↔ok.
`/??`↔`stare_about` est même commenté tel quel dans l'énumération `emotion_type`
du serveur. ⚠ Les deux appariements les moins évidents sont `/ho`↔`whistle`
(id 2) et `/lv`↔`delight` (id 3) : à regarder en premier si une émote sortait de
travers.

⚠ **Ces trente messages ne doivent jamais être traduits** — ce sont des
commandes. Un garde « commence par `/` » refuse d'écrire autre chose dans une
macro si la table venait à l'être.

⚠ Ne pas confondre avec la liste d'émotes **cash** (fenêtre 87) : celle-là a sa
propre table, `g_CashEmotionTable` `0x0125165C`, où chaque entrée porte son
sprite, son libellé et son jeton texte (`+0x14`, ce que rend
`0x00591470`). C'est elle que le bouton *view* du natif alimentait.

### Ce que le portage a dû reprendre — la check-list d'origine

L'id 86 était déjà connu de [`game_menu.cc`](../src/features/windows/game_menu.cc)
(`kMacroWndId`). Une version ImGui devait :

1. **Écrire à chaque modification**, pas à la fermeture — le natif perd les
   saisies (§5) et c'est le défaut le plus visible à corriger.
2. Écrire par le chemin natif : recopier dans le vecteur `mgr+0xFD8` **puis**
   appeler `UserSettings_SaveJson 0x0059E950`, qui s'occupe seul du `.lua` et du
   nœud JSON — donc de la synchro web. Ne pas écrire les fichiers soi-même.
3. Reprendre la **longueur max 50** et les dix valeurs par défaut avec le trou
   du `0x224`.
4. Afficher la **touche réelle** par `UserHotkey_Lua_GetHotKey(out, 2, n)` et non
   `"Alt + n"` en dur — et détruire les deux `std::string` de `out`.
5. Envoyer par `ChatMacro_SendEmotionHotkeySlot 0x00A47400`, jamais en
   reconstruisant le pipeline de chat.
6. Garder l'insertion depuis la liste d'émotes (§7) ou la refaire : sinon le
   bouton *view* devient décoratif.

---

## 9. Adresses

| Adresse | Nom | Rôle |
|---|---|---|
| `0x0104B070` | `??_7UIEmotionWnd@@6B@` | vtable de la fenêtre 86 |
| `0x0086ABD0` | `UIEmotionWnd_OnCreate` | construit les 10 lignes |
| `0x0086B250` | `UIEmotionWnd_OnDraw` | titre = msgstring 574 |
| `0x0086B290` | `UIEmotionWnd_OnLButtonDown` | |
| `0x0086B2B0` | `UIEmotionWnd_OnMsg` | commandes 201 / 283 |
| `0x0086B360` | `UIEmotionWnd_SetRowText` | écrit la ligne focalisée |
| `0x0086B430` | `EmotionHotkey_SaveFromEditBoxes` | slot +0x2C — **seul chemin d'écriture** |
| `0x00D9D400` | `EmotionHotkey_LoadListFromLua` | défauts + `.lua` |
| `0x00DA8270` | `SaveShortcutList` | écrit le `.lua` |
| `0x00D95930` | `EmotionHotkey_IsDirty` | vivant ≠ chargé ? |
| `0x0059E0D0` | *(chargeur JSON)* | nœud `EmotionHotkey` → les 2 vecteurs |
| `0x0059E950` | `UserSettings_SaveJson` | `.lua` + JSON + raccourcis |
| `0x00D56E60` | `Aid_FormatObfuscated` | nom du fichier |
| `0x00A47400` | `ChatMacro_SendEmotionHotkeySlot` | envoi d'une macro |
| `0x00589630` | `EmotionToken_InsertIntoMacroRowOrChat` | émote → ligne focalisée |
| `0x0078CE10` | *(ctor `UICashEmotionListWnd`)* | fenêtre 87 |
| `0x00A2FC17` | *(cas 86 de la fermeture)* | 🔴 pas de flush |
| `0x00A48526` | *(dans `DestroyAllWindows`)* | l'appel `vtable[+0x2C]` |
| `0x015FB398` | `g_EmotionHotkeyMacros` | vecteur vivant (pointeur) |
| `0x015FB3A4` | `g_EmotionHotkeyMacrosLoaded` | instantané chargé |
| `0x0131F868` | `g_UIEmotionWnd` | cache de la fenêtre 86 |
| `0x0131F86C` | `g_UICashEmotionListWnd` | cache de la fenêtre 87 |
| `0x015FA3C0` | `g_UIWindowContextKey` / `g_SkillInfoMgr` | le sac d'état ; macros à `+0xFD8` |

Voisins : [`game_option_re.md`](game_option_re.md) (Shortcut Settings, catégorie
2), [`chatbox_re.md`](chatbox_re.md) §5.4 (l'envoi), et la mémoire
[[project_shortcut_bar_re]] pour la barre de skills — qui ne partage avec cette
fenêtre que le gestionnaire `0x015FA3C0`.
