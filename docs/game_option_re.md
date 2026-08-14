# Menu Échap, Game Settings et Shortcut Settings — RE + blueprint ImGui

Cible : `2025-07-16_Ragexe_175220998_clientinfo.exe` (imagebase `0x400000`).
Serveur : `Moonlight-Rathena` / `moonlight` (fork rAthena).
RE du 2026-08-12, IDB renommée et commentée. Les trois fenêtres étaient ouvertes
en jeu avec x32dbg attaché : tout ce qui porte **✅live** a été relu en mémoire du
processus, pas seulement déduit du décompilé.

Ce document couvre les trois fenêtres que le joueur atteint par la touche Échap :

1. **Game Options** — le petit menu à 5 boutons (`UIEscOptionWnd`, id **155**) ;
2. **Game Settings** — la fenêtre à 5 onglets (`CUIGameSettingsUI`, id **0x271E**) ;
3. **Shortcut Settings** — la table des raccourcis (`UIHotKeyWnd`, id **156**) ;

et les trois actions du premier menu : *Character Select*, *Exit to Windows*,
*Return to game*. La partie 5 est le blueprint de remplacement en ImGui.

---

## 0. Trois avertissements de méthode

**🔴 L'IDB est l'exe VANILLA ; le client tourne patché par WARP.** Conséquence
vérifiée ici : les libellés des cinq onglets de Game Settings sont, dans `.rdata`,
des littéraux **coréens CP949** (`0x010460C4` = `기본 설정`, puis `이펙트 설정`,
`컨트롤 설정`, `그래픽 설정`, `기타…`) — et ils le sont **aussi en mémoire vive**
✅live. Ce ne sont donc **pas** les libellés affichés : ceux du jeu viennent de
`data\msgstringtable.csv`. Une mémoire antérieure affirmait le contraire ; c'est
corrigé ici. Ne jamais conclure « le natif affiche X » depuis un littéral de
l'exe sans le recouper avec la table de messages ou l'écran.

**Lire `msgstringtable.csv` : `ligne = id + 1`.** Le fichier de Moonlight a ses
deux colonnes en **base64** (clé, valeur), une paire par ligne, la ligne 1 valant
l'id 0. Calibré sur `MsgStringTable_GetById(0x60C)` = id 1548 = ligne 1549 =
`MSI_MOVETO_SAVEPOINT`, qui est bien le texte de confirmation observé.

**🔴 Les ids de commande de bouton sont LOCAUX à leur fenêtre.** `185` vaut
« Return to game » dans le menu Échap et « cancel » dans Shortcut Settings ; `370`
ouvre `0x9C` depuis le menu Échap alors que la même valeur désigne `0x12F` dans
`UIMenuIconWnd_OnMsg`. Un bouton envoie son id au `OnMsg` de **son parent** : il
n'existe pas d'espace de commandes global.

---

## 1. Vue d'ensemble : trois fenêtres, deux fabriques

| Fenêtre | Classe | id | vtable | taille | cache manager | fabrique |
|---|---|---|---|---|---|---|
| Game Options | `UIEscOptionWnd` | **155** (0x9B) | `0x010384A0` | `0xD8` | `mgr+0x408` = `0x0131F8F0` | `MakeWindow` case 155 |
| Game Settings | `CUIGameSettingsUI` | **0x271E** | `0x01047D7C` | `0x100` | registre générique `0x0131EF08` | `GameSettingsUI_Create` |
| Shortcut Settings | `UIHotKeyWnd` | **156** (0x9C) | `0x010383C8` | `0x120` | `mgr+0x404` = `0x0131F8EC` | `MakeWindowFromLuaInfo` |

`g_UIWindowMgr` = `0x0131F4E8`. Les deux slots de cache sont **adjacents** :
`+0x404` = Shortcut Settings, `+0x408` = Game Options.

```
touche Échap / icône de menu cmd 193
        │
        ▼
UIWindowMgr_MakeWindow(mgr, 155)  ─── case 155 @ 0x00A3F205
        │   gardes : [mgr+0x4F10C]==0 ET [mgr+0x4F1B0]==0, sinon RIEN ne s'ouvre
        │   réutilise mgr+0x408 s'il est non nul (pas de doublon)
        ▼
UIEscOptionWnd  ── bouton « game settings »  (cmd 458) ──► MakeWindow(0x271E)  CUIGameSettingsUI
                └─ bouton « Shortcut Configuration » (cmd 370) ──► MakeWindow(0x9C)  UIHotKeyWnd
```

**Deux fabriques distinctes.** `UIEscOptionWnd` sort du gros `switch` de
`UIWindowMgr_MakeWindow` (0x00A39340, indexé par `g_UIWindowIdToCaseTable`
`0x00A42CA8[id]` → table de blocs `0x00A42904[case]` ; id 0x9B → case 0x75,
id 0x9C → case 0x76). `UIHotKeyWnd` et `CUIGameSettingsUI` passent, elles, par la
fabrique **générique pilotée par Lua** décrite en 1.1.

### 1.1 `UIWindowMgr_MakeWindowFromLuaInfo` (0x00A42EA0)

La fabrique moderne, partagée par les ids 156, 158, 159, 165-167, 181, 183, 191 :

1. appelle le global Lua **`GetWindowInfo(id)`** (format `"d>dddd"`) → `(largeur,
   hauteur, x, y)`. **x et y sont donnés en repère 1024×768** puis mis à
   l'échelle : `x = x·écranW/1024`, `y = y·écranH/768` ;
2. `FindWindow(id)` : si la fenêtre existe déjà, elle est renvoyée telle quelle ;
3. sinon construit selon l'id — `0x9C` → `new(0x120)` + `UIHotKeyWnd_ctor`, rangé
   dans `mgr+0x404` ; (`0x9D`→`0x104`/`sub_B3E2B0`, `0x9F`→`0xCC`/`sub_8D7820`,
   `0xB5`→`0x108`/`sub_8D8150`, `0xD1`→`0x3EC`/`sub_B3D810`) ;
4. position : d'abord le **rect sauvegardé** (`std::map` à `mgr+0x4F0F0`,
   alimentée par `UIWindowMgr_SaveRectAndCloseWindow`), sinon `OptionInfo_GetInt`,
   sinon la position Lua ; le tout borné à l'écran.

C'est le point qui explique la remarque de la mémoire « le moteur ne sauve jamais
la position de 0x271E » : la `std::map` de rects ne survit pas à la session, et le
repli `OptionInfo` est appelé avec des noms de clé **vides** dans ce build.

---

## 2. Game Options — `UIEscOptionWnd` (id 155)

### 2.1 Identité

RTTI `.?AVUIEscOptionWnd@@` @ `0x01240040`, COL @ `0x010C50B4`,
**vtable `0x010384A0`**. Objet `0xD8` octets. Pas d'entrée dans le `switch` de
`UIWindowMgr_FindWindow` : on la trouve **uniquement** par `*(void**)0x0131F8F0`
(nul quand le menu est fermé).

| slot | adresse | rôle |
|---|---|---|
| +0x00 | `0x008DB420` | destructeur scalaire |
| +0x10 | `0x00874AF0` | `UIWindow_SetPos` (partagé) → écrit `+0x1C`/`+0x20` |
| +0x38 | `0x005AAD80` | `UIWindow_SetVisible` (écrit `+0x28` en DWORD entier) |
| +0x3C | `0x008E5410` | `OnCreate` |
| +0x50 | `0x008F1D00` | `OnDraw` |
| +0x94 | `0x008FAE60` | **`OnMsg`** (propre à la classe) |

### 2.2 Naissance, taille, position

`MakeWindow` case 155 @ `0x00A3F205` :

- **deux gardes** : `[mgr+0x4F10C] == 0` et `[mgr+0x4F1B0] == 0`, sinon la
  fonction part au `default` et **rien n'est créé** ;
- réutilise `mgr+0x408` s'il est non nul → **jamais deux instances** ;
- `new(0xD8)` + `UIEscOptionWnd_ctor` (0x008D71B0), rangé dans `mgr+0x408` ;
- `UIWindow_SetSize(280, 143)` (`0x118 × 0x8F`) ;
- position par défaut, posée par la queue commune de `MakeWindow` @ `0x00A3B31D` :
  **`x = Screen_CenterXFrom640(185)` = `185 + (écranW − 640)/2`** et
  **`y = UI_ScaleYFrom480(300)`**. Les deux constantes sont donc exprimées dans un
  repère **640×480**, converties à la résolution courante — le 185 n'est pas un x
  absolu. L'appel final est `vtable[+0x10](window, x, y)` : le premier `SetPos`
  d'une instance neuve passe bien **par la vtable**, ce qui rend fiable le hook
  anti-scintillement de `SettingsTweaks` sur cette fenêtre.

⚠ **La hauteur réelle est 139, pas 143** ✅live (`+0x18` = `0x8B`). `OnCreate`
recalcule `SetSize(largeur, this+0xD4 + 122)` = `17 + 122`, ce qui écrase le
`0x8F` de `MakeWindow`. Une mémoire antérieure retenait 143 : c'est la valeur
transitoire.

### 2.3 Champs

Relevés ✅live sur l'instance `0x13DBB8E8` (mode 0, personnage vivant) :

| offset | contenu |
|---|---|
| `+0x14` / `+0x18` | largeur 280 / hauteur **139** |
| `+0x1C` / `+0x20` | x / y écran |
| `+0x28` | visible (DWORD) |
| `+0x2C` | **id de fenêtre = 155** |
| `+0xB4`…`+0xCC` | les **7 boutons** `UIBitmapButton` (0x120 o chacun) |
| `+0xD0` | **disposition** 0/1/2 (posée par le ctor) |
| `+0xD4` | hauteur de la barre de titre = **17** (ctor, réécrit par `OnDraw`) |

### 2.4 Les sept boutons et les trois dispositions

`UIEscOptionWnd_OnCreate` (0x008E5410) crée sept boutons dans une boucle, avec
les bitmaps `<ui>\esc_XX` + suffixes `a.bmp` / `b.bmp` / `c.bmp` (normal / survol
/ pressé), et leur id de commande via `vtable[+0xB4]` :

| i | offset | bitmap | cmd | libellé affiché |
|---|---|---|---|---|
| 0 | `+0xB4` | `esc_01` | **371** | Character Select |
| 1 | `+0xB8` | `esc_001` | **458** | game settings |
| 2 | `+0xBC` | `esc_08` | **370** | Shortcut Configuration |
| 3 | `+0xC0` | `esc_09` | **251** | Exit to Windows |
| 4 | `+0xC4` | `esc_02` | **185** | Return to game |
| 5 | `+0xC8` | `esc_05` | **353** | (résurrection sur place) |
| 6 | `+0xCC` | `esc_10` | **372** | (retour au point de sauvegarde) |

Les ids viennent de `xmmword_1039AF0` = {371, 458, 370, 251} puis des immédiats
185, 353, 372.

🔴 **Les libellés de ces boutons sont des PIXELS, pas des chaînes.** `OnCreate`
n'appelle jamais `UITextButton_SetName` ni `MsgStringTable_GetById` pour eux — il
ne pose que trois bitmaps d'état. Le texte « Character Select », « game settings »,
« Shortcut Configuration », « Exit to Windows », « Return to game »,
« Return to last save point » et « Resurrection » est **peint dans
`esc_XXa/b/c.bmp`**, à l'intérieur du GRF. Deux conséquences :

- c'est ce qui explique la casse incohérente observée en jeu (« game settings » en
  minuscules à côté de « Character Select ») : ce sont des images faites à la main,
  pas des entrées de table ;
- **ces six libellés ne sont pas traduisibles** sans redessiner des bitmaps —
  contrairement à ceux de Game Settings et de Shortcut Settings, qui passent par
  `msgstringtable`. Un panneau ImGui écrit son texte lui-même : **la traduction
  française du menu Échap est gratuite**, et c'est un argument de plus pour
  commencer le portage par lui (§5.7).

⚠ À ne pas confondre avec le titre de la fenêtre, lui bien issu de la table
(`MsgString(1483)`, §2.5).

Le ctor choisit la disposition dans `+0xD0` :

```c
mode = 0;                                     // vivant
if (*(GameMode + 0x250)) {                    // personnage MORT
    mode = (Own_HasResurrectionToken(mode_obj)   // jeton type « Token of Siegfried »
            || StatusEffectList_Has(580))        // ou statut 580
           ? 2 : 1;
}
```

- **mode 0 (vivant)** — boutons 0..4, y = 5, 28, 51, 74, 97 (pas de 23) ;
  hauteur = `17 + 122` = **139**. C'est l'écran de la capture. ✅live
- **mode 1 (mort, sans jeton)** — `esc_10` (y=5) puis `esc_02` (y=28) ;
  hauteur `17 + 53`. ✅**confirmé en jeu** (capture 2026-08-13, personnage mort) :
  deux boutons, *« Return to last save point »* puis *« Return to game »*, dans cet
  ordre. C'est cette capture qui établit que `GameMode + 0x250` ≠ 0 signifie bien
  « mort » — l'inférence venait des trois signaux concordants (jeton de Siegfried,
  point de sauvegarde, `CZ_STANDING_RESURRECTION`).
- **mode 2 (mort, avec jeton/statut 580)** — `esc_05`, `esc_10`, `esc_02` ;
  hauteur `17 + 76`. ✅**confirmé en jeu** (capture 2026-08-13, mort + jeton) :
  trois boutons, *« Resurrection »* / *« Return to last save point »* /
  *« Return to game »*, dans cet ordre.

### 2.4.1 `Own_HasResurrectionToken` (0x00C6B290) — liste EN DUR

Cinq ids d'objet en immédiats, essayés en séquence ; **vérifiés octet par octet en
mémoire vive** ✅live (donc **non repatchés par WARP** — contrôle nécessaire, cf. §0) :

| id | `push` live |
|---|---|
| **7621** (Token of Siegfried) | `68 C5 1D 00 00` |
| **6833** | `68 B1 1A 00 00` |
| **6316** | `68 AC 18 00 00` |
| **6293** | `68 95 18 00 00` |
| **25310** | `68 DE 62 00 00` |

Pour chacun : `ItemSkillMgr_GetInfoByResId_UNIFIED(0x00D5A980)(mgr, &info, id, 0)`
puis test **`info+0x04` (trouvé) ET `info+0x10 > 0` (quantité)** → renvoie 1 à la
première réussite. (Ces deux offsets sont ceux déjà établis dans
`project_shortcut_bar_re` ; ils se recoupent ici.)

⚠ **Garde préalable qui annule tout** : `obj = *(mode + 0xCC)` ; si
`obj+0x48`, `obj+0x4C` **ou** `obj+0x54` est non nul, la fonction renvoie 0
**sans même regarder l'inventaire** — donc pas de bouton *Resurrection*. Sémantique
de ces trois champs **non identifiée** : à connaître avant de reproduire la
condition, sinon on affichera le bouton dans un état où le natif le refuse.

🔴 **Conséquence pour le portage** : ne **jamais** recopier cette liste de cinq ids
dans Bourgeon — **appeler la fonction native** (`feedback_never_hardcode_use_native`).
Une liste recopiée se désynchronise dès que le serveur ajoute un objet de
résurrection, et rate la garde ci-dessus.

📌 **Contradiction de nommage relevée au passage** : `mov ecx, 0x015FA3C0` ici, que
l'IDB nomme `g_UIWindowContextKey` alors que `project_shortcut_bar_re` l'a établi
comme **`g_SkillInfoMgr`** (`CSkillInfoMgr`). Même adresse, deux noms — à trancher
dans `project_address_directory` (ce serait la 8ᵈ du registre).

### 2.5 Barre de titre : d'où vient « Game Options (813-333) »

`UIEscOptionWnd_OnDraw` (0x008F1D00) :

```c
Aid_FormatObfuscated(buf, g_Own_CharId);              // 0x015FB9A8
titre = sprintf("%s (%s)", MsgString(1483), buf);     // MSI_ESC_OPTIONWND = "Game Options"
this[+0xD4] = UIWindow_DrawTitleBar(this, 1, titre, 0);
```

Le `(813-333)` de la capture est donc l'**identifiant de personnage masqué** —
pas des coordonnées, pas l'AID. À reproduire si l'on veut que le joueur retrouve
son repère (c'est le numéro qu'un GM lui demande).

### 2.6 `OnMsg` : la table des commandes

`UIEscOptionWnd_OnMsg` (0x008FAE60), `msg == 6` = clic bouton, `a4` = cmd. Tout
ce qui n'est pas 6 retombe sur `UIWindow_OnMsg_Default` (0x008841D0).
`mode = GameMode_GetActive(0x01213338)` ; `SendMsg` = `mode->vtable[+0x18]` =
`CMode::SendMsg` (0x00C86740).

| cmd | action |
|---|---|
| **371** | `sub_5C4DA0(0x12517A8)` + `sub_5AEB80(0x1251750)` (purges locales) → `SendMsg(25, 1)` → ferme **155**, **164**, **269** |
| **458** | `SaveRectAndCloseWindow(10014)` ; si rien n'était ouvert → `MakeWindow(0x271E)` — bascule |
| **370** | `SaveRectAndCloseWindow(156)` ; si rien n'était ouvert → `MakeWindow(0x9C)` — bascule |
| **251** | ferme **155** puis `SendMsg(88)` |
| **185** | ferme **155** et **rien d'autre** |
| **353** | `SendMsg(250)` puis ferme 155 |
| **372** | modale `MsgString(1548)` ; si le retour vaut **187** (OK) → `SendMsg(25, 0)` puis ferme 155 |
| 457 | bascule la fenêtre **236** (`0xEC`) — **code mort** : aucun bouton n'émet 457 |

⚠ `SaveRectAndCloseWindow` (0x00A2E770) **détruit** la fenêtre après avoir
sauvegardé son rect (cf. `project_address_directory`) : ce n'est pas un masquage.

### 2.7 Les trois actions, jusqu'au paquet

Les numéros de `case` de `CMode::SendMsg` sont **déjà la valeur du message** (pas
un index) — piège documenté dans `reference_cmode_sendmsg_use_skill`.

| bouton | chaîne | paquet |
|---|---|---|
| **Character Select** (371) | `SendMsg(25, type=1)` @ `0x00C884F4` | **`CZ_RESTART` 0x00B2**, `{u16 op, u8 type}`, type **1** |
| **Return to Save Point** (372) | modale → `SendMsg(25, type=0)` | **`CZ_RESTART` 0x00B2**, type **0** |
| **Exit to Windows** (251) | `SendMsg(88)` @ `0x00C884D7` → `SendMsg(128)` @ `0x00C8F66B` | **`CZ_REQ_DISCONNECT` 0x018A** |
| **Résurrection** (353) | `SendMsg(250)` @ `0x00C90F79` | **`CZ_STANDING_RESURRECTION` 0x0292** |
| **Return to game** (185) | — | **aucun paquet** : fermeture pure |

Deux points qui comptent pour un remplacement :

- **`Exit to Windows` ne quitte pas le processus.** Il demande la déconnexion au
  serveur ; la sortie effective arrive avec `ZC_ACK_REQ_DISCONNECT` (0x018B).
  Un remplacement qui appellerait `ExitProcess` sauterait la sortie propre côté
  serveur (et le « stored on the server on normal exit » des raccourcis, cf. 4.8).
- **`Character Select` ferme trois fenêtres**, pas seulement le menu : 155, **164**
  et **269**. Ce sont les devoirs cachés du handler ; les oublier laisse des
  fenêtres orphelines au retour en char-select
  (cf. `feedback_replaced_handler_hidden_duties`).

---

## 3. Game Settings — `CUIGameSettingsUI` (id 0x271E)

Le RE de cette fenêtre existait déjà (mémoire `project_game_settings_ui_re`) ;
cette section le reprend, le corrige sur deux points et complète les trois
onglets pilotés par données.

### 3.1 Identité et naissance

RTTI `.?AVCUIGameSettingsUI@@` @ `0x0123176C`, COL `0x010CC530`,
**vtable `0x01047D7C`** (2ᵈ vtable de la classe : héritage multiple).
Objet `0x100`. `ctor` `0x009E99A0`, `OnCreate` **`0x009D5890`** (slot +0x3C),
`dtor` `0x009EB410`.

Elle **n'est pas** dans le `switch` de `MakeWindow` : `GameSettingsUI_RegisterFactory`
(0x0048C450) l'enregistre dans le registre générique `0x0131EF08` via
`FUN_00991B00(reg, 0x271E)` avec la fonction de création `GameSettingsUI_Create`
(0x009D2CC0). On l'atteint par `UIWindowMgr_FindWindow(mgr, 0x271E)` +
`__RTDynamicCast`.

### 3.2 Squelette

| offset | contenu |
|---|---|
| `+0xD8` | en-tête / barre de titre (`basic_interface\titlebar_*`, bouton réduire) |
| `+0xDC` | page **Basic** (`ctor 0x009E9890`, vtable `0x0104765C`) |
| `+0xE0` | page **Graphics** (`ctor 0x009E9AD0`, vtable `0x01047C98`) |
| `+0xE4`/`+0xE8`/`+0xEC` | `std::vector<page*>` = **Effects, Controls, Other** (3× `GameSettingsUI_ListPage_ctor 0x009EA400`) |
| `+0xF0` | bandeau des 5 boutons d'onglet ; le `+0x66` de chaque bouton pointe **sa** page |
| `+0xF4` | bouton **[Reset]** |
| `+0xF8` | bouton **[Apply]** |
| `+0xFC` | bouton **[close]** |

**Libellés d'onglet** — `msgstringtable`, et non les littéraux coréens (cf. §0) :

| onglet | id | clé | texte |
|---|---|---|---|
| 1 | 4142 | `MSI_GAME_SETTINGS_TAB_BASIC` | Basic |
| 2 | 4143 | `MSI_GAME_SETTINGS_TAB_EFFECT` | Effects |
| 3 | 4144 | `MSI_GAME_SETTINGS_TAB_CONTROL` | Controls |
| 4 | 4145 | `MSI_GAME_SETTINGS_TAB_GRAPHICS` | Graphics |
| 5 | **4217** | `MSI_GAME_SETTINGS_TAB_ETC` | Other |

⚠ Le cinquième id n'est **pas** contigu aux quatre autres (4217, pas 4146) :
`4146` est déjà `MSI_GAME_SETTINGS_EMBLEM_FRAME`. Une boucle `base + i` sur cinq
onglets afficherait « Emblem Border » comme dernier onglet.

### 3.3 Onglet Basic — six groupes fixes

`GameSettingsUI_BasicPage_OnCreate` @ `0x009D5450`, six lignes empilées
(y = 0, 0x24, 0x60, 0x84, 0xC0, 0xE4) :

| offset | classe | ctor | contenu (cf. capture) |
|---|---|---|---|
| `+0x80` | `CUIGroupSkin` | `0x009EA0D0` | **Skin** — combo (`<Basic Skin>`) |
| `+0x84` | `CUIGroupSound` | `0x009EA160` | **Audio Setting** — 2 curseurs BGM/SFX + cases *Off* (h=0x3C) |
| `+0x88` | `CUIGroupEmblem` | `0x009E9B80` | **Emblem Border** — radio Show/Hide |
| `+0x8C` | `CUIGroupRodexSpam` | `0x009EA020` | **Mail** — 2 radios RODEX (h=0x3C) |
| `+0x90` | `CUIGroupProcessPriority` | `0x009E9F60` | **Default Priority** — radio High/Normal/Low |
| `+0x94` | `CUIGroupLogInOut` | `0x009E9EB0` | **Login Notification** — radio On/Off |

### 3.4 Onglet Graphics et le bouton [Apply]

`GameSettingsUI_GraphicsPage_OnCreate` @ `0x009D7610`, six lignes
(y = 0, 0x20, 0x60, 0x80, 0xA0, 0xC0) :

| offset | classe | ctor | cible |
|---|---|---|---|
| `+0x80` | `CUIGroupGraphicsAPI` | `0x009E9E10` | combo RenderSystem |
| `+0x84` | `CUIGroupGraphics` | `0x009E9CE0` | combos carte + résolution (alloc `0x1A8`) |
| `+0x88` | `CUIGroupSpriteDetailLevel` | `0x009EA230` | → `DAT_01602630` |
| `+0x8C` | `CUIGroupTextureDetailLevel` | `0x009EA2C0` | → `DAT_01602634` |
| `+0x90` | `CUIGroupFullscreen` | `0x009E9C30` | Window Mode (4183/4184/4185) |
| `+0x94` | `CUIGroupTrilinear` | `0x009EA350` | → `lpData_01602638` (4186) |

`GameSettingsUI_UpdateApplyButtonState` (0x009EF3A0) **n'active [Apply] que si**
un des trois combos de la page Graphics diffère de la config de rendu vivante :
`SpriteMode(+0x88)` vs `DAT_01602630`, `TextureMode(+0x8C)` vs `DAT_01602634`,
`Trilinear(+0x94)` vs `lpData_01602638`. `GameSettingsUI_RefreshApplyButton`
(0x009EF160) est le point d'entrée appelé par le `OnSelChange` des combos
(`GameSettingsUI_GraphicsCombo_OnSelChange` 0x009ECED0).

**Corps du bouton [Apply]** — `GameSettingsUI_ApplyButton_DoCall` (0x009E6AD0),
`_Do_call` de la lambda dont la vtable est `0x01046950` :

```c
msg = sub_9ED6B0(page_graphics) ? MsgString(3168) : MsgString(3167);
//  3168 MSI_GRAPHIC_SETTING_WARNING_RESTART = "To apply these values, client restart is required, proceed?"
//  3167 MSI_GRAPHIC_SETTING_APPLY           = "Set to these values?"
if (ShowMessageBoxModal(msg, 2 boutons, 280x120) != 188) {   // 188 = annulation
    sub_9EDB30(page_graphics);                               // applique (reset de device)
    GameSettingsUI_UpdateApplyButtonState(fenêtre);
}
```

### 3.5 Effects / Controls / Other — liste virtualisée pilotée par Lua

Les trois pages partagent `CUIListPage` (vtable **`0x01047014`**, `OnCreate`
**`0x009E0720`**). Elles ne contiennent **aucun contrôle par option** :

- `page+0x80` = un `CUIListBoxEx<CUIOptionItem, TALKTYPE>` (`new 0x138`,
  `ctor 0x009D0820`), largeur de ligne **387**, hauteur de ligne **26**,
  boîte 400×286, ascenseur 13×286 à x=387 ;
- `CUIListBoxEx_RebuildVisibleItemPool` (0x009E2740) crée
  `hauteur/hauteurLigne + (reste ≥ 2) + 1` contrôles `CUIOptionItem` (`new 0x100`,
  `ctor 0x009EA480`) — **juste de quoi couvrir la zone visible**, recyclés au
  défilement. Chaque ligne reçoit deux `std::function` posées par `AddNewItem`.

**Le modèle de ligne vient du Lua.** `TT_LoadGameSettingsLua` (0x0068ECA0) lit la
table globale **`OptionTbl`** et remplit un vecteur à **`mgr+0x0C..0x10`**.
**L'enregistrement fait 100 (0x64) octets** — la mémoire disait `0x19`, c'est faux :

🔴 **Deux corrections à la première lecture** (2026-08-14) :

- le vecteur est dans le **`CGameSettingsMgr`**, pas dans la session. Confusion
  facile : `CGameSettingsMgr_LoadTableFromLua` reçoit le manager en `param_1` et
  `CSession_ctor` l'appelle juste après l'avoir créé, si bien que le décompilé
  ressemble à du code de session. C'est bien `mgr+0x0C` que relisent
  `SetOption`, `ExecOption` et `ResetAllToDefault` ;
- le fichier est **`LuaFiles514\Lua Files\OptionInfo\GameSettings.lub`**.
  `GameSettingsUI.lub` est seulement le **titre des `MessageBox` d'erreur**, et
  s'y fier envoie chercher un fichier qui n'existe pas.

| offset | champ Lua | type | requis |
|---|---|---|---|
| `+0x00` | `ID` | int | oui |
| `+0x04` | `Tab` | int | oui |
| `+0x08` | `Type` | int | oui |
| `+0x0C` | `Title` | `std::string` | oui |
| `+0x24` | `Tooltip` | `std::string` | non |
| `+0x3C` | `TipBoxID` | int | non |
| `+0x40` | `Description` | `std::string` | oui |
| `+0x58` | `Default` | bool | si `Type==0` |
| `+0x5C` / `+0x60` | `MSGs[1]` / `MSGs[2]` | ids msgstringtable | si `Type==0` |

Un champ requis manquant lève une `MessageBoxA` titrée `GameSettingsUI.lub` avec
le texte `OptionTbl[ {} ].ID` (et variantes). C'est `Tab` qui répartit les lignes
entre les onglets.

`Type == 0` = **bascule booléenne** : le défaut est enregistré dans
l'`unordered_map` **`0x012515FC`** (valeur à `+0x0C` de l'entrée, clé = `ID`).

**🔴 Piège du `Default` inversé** : pour les `ID` **0x6A, 0x6B, 0x6C, 0xE7,
0x10D**, le drapeau stocké est la **négation** du `Default` Lua (`XOR 1`). Ce sont
les options dont le sens interne est l'inverse du libellé. Recopier `Default`
tel quel pour ces cinq ids inverse le réglage.

#### ✅ Le contenu de `OptionTbl` — EXTRAIT, et lisible (2026-08-14)

`GameSettings.lub` est dans **`moonlight.grf`** (`data.grf` est chiffré « Event
Horizon » et illisible, mais il n'a pas le dernier mot : `DATA.INI` met
`moonlight.grf` en premier). Extrait avec `tools/grf_reader.py`, le fichier est du
**Lua SOURCE en clair**, pas du bytecode — en-tête « Original translation works of
zackdreaver / llchrisll », 14 943 octets, **62 entrées** :

| onglet | bascules | commandes | total |
|---|---|---|---|
| `EFFECT` (1) | 11 | — | 11 |
| `CONTROL` (2) | 11 | — | 11 |
| `ETC` (3) | 21 | 19 | 40 |

📌 **`Tooltip` n'est PAS une infobulle** : le champ contient la **commande slash
équivalente** (`/aura2`, `/noctrl\n/nc`, parfois deux séparées par un saut de
ligne), et le natif ne l'affiche nulle part. C'est une information gratuite pour
un panneau de remplacement.

📌 **`MSGs` contient des CLÉS, pas des ids** (`"MSI_AURA_EFFECT_ON"`) ; le
chargeur les résout en ids par `sub_A9E640` avant de les ranger en `+0x5C`/`+0x60`.

**Les 296 noms d'options sont dans l'exe.** `CGameSettingsMgr_LoadTableFromLua`
pousse d'abord comme globales Lua les constantes `EFFECT=1 CONTROL=2 ETC=3` et
`ONOFF=0 EXE=1`, puis **les 296 chaînes de `off_1008120`, index = valeur** :
c'est l'énumération **`TALKTYPE`** complète, celle-là même qui sert d'`ID`. Elle
recoupe exactement la liste des cinq options inversées — `TT_FULL_AURA_ON_OFF`,
`TT_HIDE_AURA_ON_OFF`, `TT_BOLD_NAME_TYPE_ON_OFF`, `TT_BLOCK_CALL_ON_OFF`,
`TT_HIDE_FOOTPRINT_ON_OFF` : ce sont **les options dont le nom interne dit *hide*
là où le libellé dit *show*.** L'inversion n'est donc pas une bizarrerie, c'est
la trace d'un renommage de libellés sans renommage des drapeaux.
(`src/ragnarok/talktype.h` n'en connaît qu'environ 140, aux noms parfois
différents — `TT_MAP_POS` vs `TT_EX_MAP_POS`. À compléter depuis cette table le
jour où l'énumération servira vraiment.)

#### 🔴 Le vecteur est rempli à l'ENTRÉE EN JEU, pas à l'ouverture de la fenêtre

`CGameSettingsMgr_LoadTableFromLua` (0x0068E510) n'a **qu'un seul appelant** :
`CSession_ctor` @ `0x00D578A2`, qui crée d'abord le manager dans `0x0131EE7C`
(`new(0x58)` + ctor) puis charge la table, et enchaîne sur
`OptionInfo_LoadAndApplyAll`.

C'est **le feu vert du portage** : les données existent même si la fenêtre
`0x271E` n'est jamais créée. Sans cette vérification, un panneau de remplacement
aurait pu s'afficher vide chez tout le monde — le contraire (table chargée par la
fenêtre) était l'hypothèse la plus naturelle.

### 3.5.1 ✅ Lire et écrire une option — le chemin complet (RE du 2026-08-14)

C'était le trou n°1 du §5.8. Il est comblé, et c'est plus simple que prévu : tout
passe par le manager `CGameSettingsMgr` (`0x0131EE7C`), et une seule fonction
suffit pour écrire.

| fonction | adresse | signature |
|---|---|---|
| `GameSettings_GetFlag` | `0x0068EA70` | `char __cdecl(unsigned tt)` — drapeau INTERNE |
| `GameSettings_SetFlagRaw` | `0x0068FD50` | `char __cdecl(unsigned tt, char v)` — écriture BRUTE |
| `GameSettings_IsInvertedOption` | `0x0068EAF0` | `char __cdecl(int tt)` — les 5 ids retournés |
| `CGameSettingsMgr::SetOption` | `0x0068DFD0` | `__thiscall(int tt, char v, char annonce)` |
| `CGameSettingsMgr::ToggleOption` | `0x0068FAA0` | `__thiscall(int tt)` |
| `CGameSettingsMgr::ExecOption` | `0x0068E160` | `__thiscall(int tt)` — lignes `EXE` |
| `CGameSettingsMgr::ResetAllToDefault` | `0x0068F8E0` | `__thiscall()` |

**Lecture** — la valeur est dans l'`unordered_map` `0x012515FC` (hash FNV-1a sur
les quatre octets de l'id, valeur à `node+12`), et l'affichage vaut
`GetFlag(tt) ^ IsInvertedOption(tt)`. C'est exactement ce que fait
`GameSettingsUI_ListPage_RefreshValues` (0x009F0DA0) ligne par ligne.

**Écriture** — `SetOption` cherche le record dans le vecteur, puis un **HANDLER**
dans la table de hachage du manager. S'il en trouve un, **c'est le handler qui
applique l'effet** ; sinon on retombe sur
`GameSettings_ApplyFlagDefaultHandler` (0x00691540), qui ne fait que ranger la
valeur et, si `annonce`, écrire au chat.

🔴 **Écrire le drapeau brut ne suffit donc PAS** pour la plupart des options :
`/fog` éteint garderait son brouillard. Passer par `SetOption` est la seule
façon de rejouer le geste du client sans réimplémenter chaque effet.

📌 **Le paramètre `annonce` distingue deux chemins du même client** : la case à
cocher de la fenêtre passe **0** (`GameSettingsUI_OptionItem_OnCheck` 0x009ECC10),
la commande slash passe **1** (`ToggleOption`), et c'est elle seule qui écrit
« … is On » au chat. Un panneau qui remplace la FENÊTRE suit la fenêtre.

### 3.5.2 ✅ Le groupe Audio de l'onglet Basique

Trou n°2 du §5.8, comblé pour le seul groupe qui vaille le détour — les autres
(skin, priorité, RODEX) restent au natif, cf. §5.4.

`CUIGroupSound` (vtable `0x010471E8`, `OnCreate` `0x009DD840`) : deux
`CUIHScrollBar` de **plage 128** et deux cases « Off ». Ses quatre lambdas :

| contrôle | applique |
|---|---|
| curseur BGM | `Sound_SetBgmVolume(g_SoundMgr, v)` **puis** `Sound_SetEffectVolume(g_SoundMgr, volumeBgm)` |
| case BGM *Off* | `SetFlagRaw(TT_MUSIC_ON_OFF=7, on)` **et, si la valeur CHANGE, `CMode::SendMsg(90)`** |
| curseur Effets | `Sound_SetMaster2DVolume(v)` + `Sound_Set3DVolume(v)` |
| case Effets *Off* | `SetFlagRaw(TT_EFFECT_SOUND_ON_OFF=0xB, on)` |

Champs du gestionnaire (`g_SoundMgr` = `0x01253D0C`, clamp `[0, 0x7F]`) :
`+0xE0` effets, `+0xE4` BGM, `+0xE8` maître 2D, `+0x134` 3D.

⚠ **Deux pièges dans ces quatre lignes.**

1. Le curseur « Effets » écrit `+0xE8` (**maître 2D**) et **pas** `+0xE0`, malgré
   le nom de ce dernier. Lire `+0xE0` pour l'afficher donnerait un curseur qui ne
   suit pas ce que le joueur vient de régler.
2. Le curseur BGM aligne **ensuite** `+0xE0` sur le volume BGM. Le geste
   surprend et ressemble à un bug du client ; il est reproduit tel quel dans
   Bourgeon, parce que s'en écarter ferait diverger notre curseur du sien sur un
   champ dont le rôle exact n'est pas établi.
3. `SendMsg(90)` est un **devoir caché** : sans lui, couper la musique laisse le
   morceau en cours aller au bout, et la rallumer ne relance rien.

Les deux autres bascules de la page Basique sont, elles, de simples `TALKTYPE`
écrits en dur par le client (`0x009EEF50`, `0x009EF000`) :
**`TT_EMBLEM_FRAME_ON_OFF` (0xF3)** = bordure d'emblème,
**`TT_LOGINOUT_ON_OFF` (0xA5)** = notification de connexion.

### 3.6 Le bouton [Reset]

`GameSettingsUI_ResetAllOptionsToDefault` (0x009EC670), atteint par
`GameSettingsUI_ResetButton_DoCall` (0x009E7A70), vtable de lambda `0x01046934` :

```c
if (ShowMessageBoxModal(MsgString(3166), 2 boutons, 280x120) == 188) return;  // annulé
//  3166 MSI_GRAPHIC_SETTING_WARNING_RESET = "Set Basic Settings?"
pour chaque enfant de la page Basic (liste à page+0x50) : enfant->vtable[+0xD8]();
mgr = singleton 0x0131EE7C (créé à la demande, new(0x58) + sub_68D1F0);
sub_68F8E0(mgr);                       // remet TOUTES les options au défaut
pour chaque enfant de la page Basic : enfant->vtable[+0xDC]();
pour chaque page-liste (fenêtre+0xE4..+0xE8) : sub_9F0DA0(page);   // rafraîchit
```

⚠ Le rafraîchissement couvre la page **Basic** et les **trois pages-listes** ;
la page **Graphics** n'y est pas — elle garde ses combos, et c'est [Apply] qui
tranche pour elle.

### 3.7 La couche de stockage : `OptionInfoList` et `CmdOnOffList`

Deux tables Lua distinctes, un seul fichier : **`SaveData\OptionInfo.lua`**.

- **`CmdOnOffList["/xxx"]`** — les bascules de commande slash (39 dans le fichier
  du client : `/notrade`, `/noshift`, `/noctrl`, `/effect`, `/aura`, `/mineffect`,
  `/monsterhp`, `/minimap`, `/emblem`, `/showname`, `/fog`, `/quake`, `/zoom`…).
  C'est le magasin de valeurs des onglets Effects / Controls / Other.
- **`OptionInfoList[clé]`** — les réglages scalaires : `Bgm_Volume`,
  `Effect_Volume`, `Trilinear`, `SkinName`, `Outdoor_ViewLatitude/Distance`,
  `Indoor_View*`, `RENDERSYSTEM`, `DX9DEVICEID`, `DX9DEVICENAME`, `WIDTH`,
  `HEIGHT`, `OLD_WIDTH`, `OLD_HEIGHT`, `BITPERPIXEL`, `ISFULLSCREENMODE`,
  `SPRITEMODE`, `TEXTUREMODE`, `DEVICECNT`, `MODECNT`, `RenderSystemReset`,
  `ChangeChatMode`, `LockMouse`, `MouseExclusive`, `PriorityClass`,
  `Emblem Frame`, `DamageSkin`, `DamageDir`, `Window_XPos`, `Window_YPos`,
  `ChannelCopID`, `AutoOpen1to1Window`, `AutoOpen1to1Window_Friend`,
  `PlaySound_Open1to1Window`, `Simplicity_SkillList`, `Show_SkillDescript`,
  `ShowBattleFieldIcon`, `bLockItemDropFromItemWnd`, `bWarningPriceCheck`,
  `bSoundEffectInItemSellWnd`, `bGuildMemberListSort`, `bBlockPrivateTab`,
  `bShowMenuIcon`, `bItemDefaultSort`, `bItemAscendingOrder`,
  `bItemDescendingOrder`, `bExtendDamage`, `bShowMapName`,
  `bShowOnlyMineFootprint`, `bSummonTranslucent`, `bHidePlayer`, `bQuickSlot`,
  `/zoom`.

API C (this = objet de session, état Lua à `this+0x59B8`, globals Lua
`l_GetOptionValue` / `l_GetDefaultOptionValue`) :

| fonction | adresse | usage |
|---|---|---|
| `OptionInfo_GetInt(nom, défaut)` | `0x00D81A30` | la bête de somme |
| `OptionInfo_GetFloat` | `0x00D81810` | latitudes / distances de vue |
| `OptionInfo_GetString` | `0x00D81AF0` | `SkinName`, `DX9DEVICENAME` |
| `OptionInfo_GetBlob16` | `0x00D81900` | GUID `DX9DEVICEID` |
| `OptionInfo_LoadAndApplyAll` | `0x00D759F0` | charge le fichier **et applique tout** |
| `OptionInfo_SaveToFile` | `0x00D78970` | réécrit `SaveData\OptionInfo.lua` |

`OptionInfo_LoadAndApplyAll` est le « apply général » : `Bgm/Effect_Volume` →
gestionnaire de son `DAT_01253D0C` ; `Outdoor/Indoor_View*` → `DAT_012291D0..DC`
(bornés) ; `SkinName` → `DAT_011FE3AC` ; le bloc rendu → `DAT_016026xx` ; et une
longue série de booléens vers `session+0xNNNN` (`bShowMenuIcon` `+0x7EBC`,
`LockMouse` `+0x5B50`, `DamageSkin` `+0x809C`, `PriorityClass` `+0x7F6C`…).
Écriture : `OptionInfo_AppendIntEntry` `0x00D977A0`, `…Float` `0x00D976B0`,
`…String` `0x00D97810`, `…Blob` `0x00D97730` (format `OptionInfoList["%s"] = …`).

### 3.8 Vocabulaire de la fenêtre (`msgstringtable`)

Utile pour un portage : ce sont les textes exacts à réafficher.

| id | clé | texte |
|---|---|---|
| 4146 | `EMBLEM_FRAME` | Emblem Border |
| 4147 / 4148 | `ON` / `OFF` | On / Off |
| 4149 / 4150 | `SHOW` / `HIDE` | Show / Hide |
| 4151 | `RODEX_SPAM_ON` | Block RODEX from random users |
| 4152 | `RODEX_SPAM_OFF` | Receive RODEX by all users |
| 4153 | `PROCESS_PRIORITY` | Default Priority |
| 4154-4159 | `…HIGH/NORMAL/IDLE(+_TOOLTIP)` | High / Normal / Low + leurs infobulles |
| 4160 | `AURA` | Show Aura |
| 4161-4163 | `SIMPLE_AURA(_OFF/_ON)` | Aura Display : Original / Simple Aura |
| 4164-4166 | `DAMAGE_EXPAND(_ON/_OFF)` | Damage Display : Expand / Default |
| 4167-4169 | `MAP_NAME(_ON/_OFF)` | Map Display on movement |
| 4170 / 4218 / 4219 | `FOG(_ON/_OFF)` | Fog Effect / Fog On / Fog Off |
| 4171 / 4172 | `FOOTPRINT_EFFECT(_ALL)` | Footprint Effect / Show all |
| 4173-4176 | `SNAP(_ATTACK/_SKILL/_ITEM)` | Mouse Snap : Monster / Skill / Pickup Item |
| 4177 / 4178 | `NO_CTRL(_TEXT)` | No Ctrl + « auto attack without holding Ctrl » |
| 4179 / 4180 | `NO_SHIFT(_TEXT)` | No Shift + « cast healing skills on monsters » |
| 4181 | `ZOOM_OUT_TEXT` | When zoomed out, the screen appears slightly wider |
| 4182 | `MOUSE_EXCLUSIVE_TEXT` | In windowed mode, the mouse cursor does not go out of the window |
| 4183-4185 | `SCREEN_MODE`, `FULLSCREEN_MODE`, `WINDOW_MODE` | Window Mode / Fullscreen / Windowed |
| 4186 | `TRILINEAR` | Trilinear Filtering |
| 4221 | `LOGINOUT` | Login Notification |
| 4222 | `TAB_ETC` | Other |
| 4225 / 4226 | `EMBLEM_ON/_OFF` | Guild Emblem On / Off |
| 1483 | `MSI_ESC_OPTIONWND` | Game Options |
| **4232** | `MSI_OPTION_ESC` | Options (ESC) |

#### ✅ D'OÙ VIENT UN ID — CAUSE ÉTABLIE, 2026-08-14

Les ids de ce tableau ont d'abord été relevés en **comptant les lignes de
`data\msgstringtable.csv`**. C'est faux, et le tableau ci-dessus porte les valeurs
corrigées.

**L'id d'un message est son index dans `g_MsgStringSymbolNames` (`0x0104F0A8`),
une table de 4355 pointeurs vers des clés `MSI_*` compilée DANS L'EXE.** Le csv,
lui, ne fait qu'associer une clé à un texte ; son ordre ne l'engage à rien, et le
client s'y retrouve par dictionnaire, pas par rang.

Les deux ordres coïncident dans les petits rangs, ce qui rend l'erreur invisible
longtemps. Mesuré sur le client déployé :

- l'exe connaît **4355** clés (ids 0..4354), le csv livré en a **4360** lignes ;
- ils sont identiques jusqu'à l'id **4074**, puis un bloc de huit clés est permuté
  avec un autre (`MSI_DECOM_*` ↔ `MSI_RESET_CASH_EMOTION_INFO`/`MSI_RUNESYSTEM_*`) ;
- **40 clés du csv sont inconnues de l'exe** (`MSI_AUTO_HUNT_*` à la ligne 4222,
  `MSI_PETINFO_ALERT` et les `..._WITH_ITEM_LINK` à la ligne 4334), et chacune
  décale tout ce qui suit ;
- **36 clés de l'exe sont absentes du csv**, dont `MSI_ACCOUNT_LIMITED_*`.

C'est ce décalage qui expliquait les trois symptômes du portage, tous constatés en
jeu le même jour : 4216/4217 rendaient « NO MSG » (ils visent des
`MSI_ACCOUNT_LIMITED_*` que le csv n'a pas), et le titre demandé à 4241 sortait
« Indoor teleport is not supported. » (`MSI_CANNOT_MOVE_INDOOR_MAP`).

➡ **La table est extraite dans [`tools/lang/msgstring_ids.csv`](../tools/lang/msgstring_ids.csv)** —
4355 lignes `id,clé`. C'est la seule source d'id qui fasse foi ; ne jamais compter
les lignes de `msgstringtable.csv`.

⚠ **Et malgré cela, `msgstr::Utf8Or(id, repli)` reste la règle** — jamais
`msgstr::Utf8(id)` nu pour un libellé de fenêtre. La table extraite vient de l'exe
**vanilla** que désassemble IDA, alors que le client livré est WARP-patché et
pourrait embarquer un autre csv. Sans repli, l'échec est MUET :
`MsgStringTable_GetById` (0x00A9ED30) rend exactement `"NO MSG"` pour un id connu
mais sans texte, `"NO MSG : <id>"` au-delà de 4354, et ça part à l'écran comme un
vrai libellé.

📌 La **traduction** du client, elle, ne dépend d'aucun id : elle s'indexe sur le
TEXTE anglais rendu (`ragnarok/msgstring_override.cc`), ce qui la met hors de
portée de toute question de numérotation.

### 3.9 ✅ Skin, RODEX et priorité — les trois derniers groupes (RE du 2026-08-14)

Ces trois-là étaient restés au natif faute de RE. Ils n'ont **rien en commun** :
ni magasin, ni chemin d'écriture, ni même la question de savoir qui décide. C'est
la seule chose à retenir avant d'y toucher — les traiter comme des options
ordinaires produirait trois bugs différents.

Chaque groupe est une sous-classe de `GameSettingsUI::CUIGroup` et n'écrase que
**trois slots** de sa vtable : `[15] OnCreate` (bâtir les widgets), `[16]`
(réafficher les libellés depuis la `msgstringtable`), `[54] ResetToDefault` et
`[55] RefreshFromState` (recocher les boutons d'après l'état réel).

| groupe | vtable | OnCreate | Reset `[54]` | Refresh `[55]` |
|---|---|---|---|---|
| `CUIGroupSkin` | `0x01047104` | `0x009DCFD0` | `0x009EDA40` | `0x009F09C0` |
| `CUIGroupRodexSpam` | `0x010473B0` | `0x009DC410` | `0x009ED9E0` | `0x009F0980` |
| `CUIGroupProcessPriority` | `0x01047494` | `0x009DB3C0` | `0x009ED9C0` | `0x009F0910` |

#### Priorité du processus — 100 % locale, et persistée en Lua

`0x009EF070` est le handler des trois radios. Il compare le bouton cliqué aux
widgets `this[33]/[35]/[37]` et fait, en tout et pour tout :

```c
dwPriorityClass = X;                              // 0x0160232C
SetPriorityClass(GetCurrentProcess(), X);
```

avec `X` ∈ `HIGH_PRIORITY_CLASS` (0x80) / `NORMAL_PRIORITY_CLASS` (0x20) /
`IDLE_PRIORITY_CLASS` (0x40, que le client intitule « Low »). Aucun paquet, aucune
écriture disque **depuis ce chemin**.

🔴 **Deux choses que la fenêtre ne dit pas, et qui changent la lecture du réglage :**

1. **Il ne vaut que fenêtre au premier plan.** `sub_DB74B0` (le handler
   d'activation, appelé depuis `Game_MainWndProc`) force `IDLE_PRIORITY_CLASS`
   **sans condition** à la perte du focus, et restaure `dwPriorityClass` au
   retour. Un joueur qui teste en ALT-TAB ne mesurera jamais son réglage.
2. **Il EST persistant**, mais par un autre chemin : `OptionInfoList["PriorityClass"]`
   dans `SaveData\OptionInfo.lua`, relu au démarrage (`WinMainCRTStartup_Run`
   applique `dwPriorityClass` juste après le mutex d'instance unique). Écrire le
   global suffit donc à ce que le client sauvegarde la bonne valeur en sortant.

#### RODEX — le serveur décide, le client ne fait que demander

`0x009EF0F0` envoie **`CZ 0x0B93`, 12 octets fixes** — et les champs sont alignés
sur 4, ce n'est pas un `struct` packé :

```c
struct { uint16 opcode; uint16 pad; int32 type; int32 value; }
//        0x0B93                     1 (RODEX)   1 = recevoir de tout le monde
```

🔴 **Le client n'écrit JAMAIS son propre drapeau.** `byte_1602430` n'est touché que
par deux handlers de RÉCEPTION :

| paquet | handler | rôle |
|---|---|---|
| `ZC 0x0B94` (14 o.) | `0x00CF97A0` | un changement : pose le drapeau, annonce au chat `MSI_RODEX_SPAMOPTION_ON/OFF`, et `MSI_RODEX_SPAMOPTION_FAIL` en cas d'échec |
| `ZC 0x0B95` (var.) | `0x00CF8290` | la liste complète à l'entrée en jeu : des couples `{type, valeur}`, dont le type 1 |

➡ Un panneau de remplacement **ne doit donc pas afficher optimiste** : la case ne
bouge qu'au retour du serveur, exactement comme les radios natives. Si le serveur
ignore `0x0B93`, elle ne bouge pas — et c'est la vérité, pas un bug d'affichage.

⚠ **Sens du drapeau, à l'envers du nom du libellé** : `byte_1602430 == 1` veut dire
« recevoir de tout le monde », c'est-à-dire filtre anti-spam **désactivé** — le
libellé `..._RODEX_SPAM_ON` (4151) nomme l'AUTRE bouton.

#### Skin — une combo, et une purge de toutes les textures

Gestionnaire à `0x011FE3A8` :

| adresse | champ | contenu |
|---|---|---|
| `0x011FE3C4` | `+0x1C` | index courant, **−1 = aucun skin** |
| `0x011FE3D4` / `0x011FE3D8` | `+0x2C` / `+0x30` | `vector<std::string>` des noms (24 o. par entrée) |
| `0x007A6F10` | — | `GetSkinName(index)`, `−1` rend le nom par défaut |
| `0x007A7F70` | — | `SetSkin(index)` |

`SetSkin` retient l'index puis appelle `UITextureMgr::PurgeByExtension("bmp")`
(`0x00A8F740`) : **toutes les textures .bmp du client sont détruites** pour se
recharger depuis le dossier du nouveau skin.

🔴 **Conséquence pour nous, et elle n'est pas facultative** : tout cache de
textures côté Bourgeon devient faux — les mêmes chemins désignent d'autres images.
`ro::InvalidateGameTextures()` existe pour ça, et l'appel doit se faire **au tick,
pas en frame** : c'est une commande native (gel muet) *et* une libération de
textures que la frame en cours dessine encore.

La combo native ajoute d'abord `<Basic Skin>` (id 0) puis un item par entrée du
vecteur ; l'id d'item vaut donc `index + 1`, ce que confirme `0x009ED040`
(`item_id − 1` → `SetSkin`) et `0x009F09C0` (sélection = `index + 1`).

---

## 4. Shortcut Settings — `UIHotKeyWnd` (id 156)

Fenêtre **jamais RE jusqu'ici** (la mémoire ne connaissait qu'un
`UIHotkeyGuideWnd` sans xref, qui est une **autre** classe : le pense-bête de
touches, bitmaps `\hotkey_guide\`).

### 4.1 Identité

**vtable `0x010383C8`**, objet **`0x120`**, `ctor` **`0x008D7260`**,
cache `mgr+0x404` (`0x0131F8EC`) ✅live.

| slot | adresse | rôle |
|---|---|---|
| +0x00 | `0x008DB490` | destructeur |
| +0x10 | `0x00874AF0` | `SetPos` (partagé) |
| +0x3C | `0x008E5EF0` | **`OnCreate`** — était étiquetée `UINaviSearchWnd_OnCreate` dans l'IDB : **mauvais nom, corrigé** |
| +0x50 | `0x008F1DE0` | `OnDraw` |
| +0x64 | `0x008F6A60` | `OnLButtonDown` (sélection de ligne) |
| +0x70 | `0x008F76E0` | `OnMouseMove` (infobulle) |
| +0x80 | `0x008F8E40` | `OnRButtonDown` |
| +0x94 | `0x008FB130` | **`OnMsg`** |

### 4.2 Champs (tous relevés ✅live sur `0x4C8B91A0`)

| offset | valeur live | rôle |
|---|---|---|
| `+0xB4` | −1 | **ligne sélectionnée** (index d'affichage ; −1 = aucune) |
| `+0xB8` | 18 | lignes par colonne |
| `+0xBC` | 17 | hauteur de la barre de titre (écrite par `OnDraw`) |
| `+0xC0` | **29** | hauteur du bas de fenêtre (`sub_896800(2,0,1)`) — le ctor l'initialise à 0 |
| `+0xC4` | 20 | hauteur du bandeau d'onglets |
| `+0xC8` | 3 | marge extérieure en x |
| `+0xCC` | 3 | espace entre le bandeau et la liste |
| `+0xD0` | 20 | **hauteur de ligne** |
| `+0xD4` | 80 | largeur de la colonne « touche » |
| `+0xD8` | 135 | largeur de la colonne « libellé » |
| `+0xDC`…`+0xF8` | 4 maps, taille **0** | **4 × `std::map`** d'éditions EN ATTENTE, indexées par **catégorie** |
| `+0xFC` | ptr | `UITransBalloonText` (la bulle « Press a key… ») |
| `+0x100` | ptr | contrôle d'onglets |
| `+0x104` | 0 | **catégorie courante** |
| `+0x108` | ptr | `UIToggleButton` *Enable Battle Mode* |
| `+0x10C` | 1 | page courante (1-based) |
| `+0x110` | 0 | nombre de pages |
| `+0x114` | 0 | octet : pagination active |
| `+0x118` / `+0x11C` | ptr | boutons **Prev** / **Next** |

Les quatre maps vides confirment qu'elles ne contiennent **que le brouillon
d'édition** : rien n'y est écrit tant que le joueur ne remappe pas.

### 4.3 Contenu créé par `OnCreate`

- bulle d'aide (`UITransBalloonText`, `new 0xCC`) à `+0xFC`, largeur de la
  fenêtre × 20, texte `MsgString(1486)` replié à 60 : *« Press a key to assign.
  Pressing 'ESC' will remove the assigned key. »* ;
- contrôle d'onglets à `+0x100` (`new 0xFC`, `sub_837010`), posé en (3, 20),
  largeur `+0xC4 − 3`, hauteur 20 ; **4 onglets** ajoutés par `sub_864690` :

| onglet | id msg | clé | texte |
|---|---|---|---|
| 0 | 1491 | `MSI_HOTKEYWND_TAB1` | Skill Bar |
| 1 | **3595** | `MSI_HOTKEYWND_SKILLBAR2` | Hotkey Bar 2 |
| 2 | 1492 | `MSI_HOTKEYWND_TAB2` | Interface |
| 3 | 1493 | `MSI_HOTKEYWND_TAB3` | Macros |

- quatre `UIBitmapButton` `btn_ok` / `btn_cancel` / `btn_reset` / `btn_close`
  (bitmaps `<nom>.bmp` / `_a.bmp` / `_b.bmp`), ids `xmmword_1039AD0` =
  **{184, 185, 363, 201}**, y = `hauteur − 25`, x selon la commande :
  **363 → x = 5** (Reset, à gauche), **184 → w−139** (OK), **185 → w−92**
  (cancel), **201 → w−46** (close) ;
- `UIToggleButton` à `+0x108` (150×12) en (80, `hauteur − 22`), libellé
  `MsgString(1775)` = **Enable Battle Mode**, état initial = **`g_ChangeChatMode`**
  (`0x0131F50E`) ;
- deux `UIBitmapButton` **Prev** / **Next** à `+0x118` / `+0x11C`, libellés
  `MsgString(1053)` / `MsgString(1054)`, ids **316** / **317**, en
  (`w−80`, 25) et (`w−20`, 25), **masqués** par `vtable[+0xD4](0)`.

`vtable[+0xD4]` d'un bouton = **SetVisible** (prouvé par
`UIHotKeyWnd_UpdatePagingButtons`).

### 4.4 Onglet → catégorie, et la table Lua

`UserHotkey_TabIndexToCategory` (**0x005D4C50**) :

| onglet | libellé | catégorie | table Lua |
|---|---|---|---|
| 0 | Skill Bar | **0** | `USERKEY_1` / `SkillBar_1Tab` |
| 1 | Hotkey Bar 2 | **3** | `USERKEY_4` / `SkillBar_2Tab` |
| 2 | Interface | **1** | `USERKEY_2` / `InterfaceTab` |
| 3 | Macros | **2** | `USERKEY_3` / `EmotionTab` |

L'ordre visuel n'est donc **pas** l'ordre des catégories, et le Lua est 1-based
(`catégorie + 1`). Confirmé par `SaveData\UserKeys.lua`, qui contient bien
`USERKEY_1` et `USERKEY_4`.

`UserHotkey_RowToCommandIndex` (**0x00D83B20**) `(catégorie, ligne) → index de
commande` ou −1 : pour la **catégorie 1 (Interface) uniquement**, il **saute** les
index 30, 33, 34, 36, 43, 45, 53, 58, 62, 64, 65, 66 (`0x1E, 0x21, 0x22, 0x24,
0x2B, 0x2D, 0x35, 0x3A, 0x3E, 0x40, 0x41, 0x42`) — des commandes existantes mais
non remappables. Puis il compare à `GetOriginalHotKeyListSize(catégorie+1)` et
renvoie −1 au-delà.

### 4.5 Le modèle de ligne

`UIHotKeyWnd_GetRowBinding` (**0x008E2650**) `(out, catégorie, ligne)` remplit une
structure de **0x3C octets** :

```
+0x00  int          cmdId          (UserHotkey_RowToCommandIndex, -1 = ligne absente)
+0x04  int          keycode1
+0x08  int          keycode2
+0x0C  std::string  str1 = NOM DE TOUCHE   ("F1", "A", "Shift+F1" — layout-aware)
+0x24  std::string  str2 = LIBELLÉ         ("Hotkey 2-1" — le champ EXE de UserKeys.lua)
```

La valeur affichée est **l'édition en attente si elle existe**, sinon la valeur
vivante : la fonction cherche `cmdId` dans la map de la catégorie et ne retombe
sur `UserHotkey_Lua_GetHotKey` que si la map n'a rien (ou une chaîne vide).

**🔴 Correction d'une erreur de la mémoire.** `UserHotkey_Lua_GetHotKey`
(`0x00D80950`, global Lua `GetHotKey`, format `"dd>ddss"`) remplit
`out+0x00 = keycode1`, `out+0x04 = keycode2`, **`out+0x08` = nom de touche**,
**`out+0x20` = LIBELLÉ de la commande** — et *non* « une deuxième touche » comme
noté auparavant. Trois preuves concordantes : `UIHotKeyWnd_OnDraw` dessine
`out+0x20` dans la colonne des libellés ; `UIHotKeyWnd_CommitPendingBindings`
passe cette même chaîne à `ChangeUserHotKey` ; et `SaveData\UserKeys.lua` stocke
`[9] = { EXE = "Hotkey 2-1", KEY1 = 65 }`. Aucun code de Bourgeon ne dépend de
l'ancienne lecture (`skill_bar.cc` ne lit que `+0x08` et détruit les deux
`std::string`, ce qui reste correct).

Les nœuds des maps d'édition font **0x4C octets** :
`+0x10` cmdId (clé), `+0x14` kc1, `+0x18` kc2, `+0x1C` `std::string` nom de
touche, `+0x34` `std::string` libellé.

### 4.6 Géométrie et rendu (`UIHotKeyWnd_OnDraw` 0x008F1DE0)

Titre = `MsgString(1494)` = **Shortcut Settings**. Ne dessine rien si
`hauteur == +0xBC` (fenêtre repliée).

- liste : `y_haut = 17 + 20 + 3 = 40` ; **deux colonnes de 18 lignes**, hauteur
  de ligne **20** → `y = 40 + 20·(n mod 18)`, colonne = `n / 18` ;
- colonne gauche : libellé à `x = 3`, cellule touche à `x = w/2 − 80` ;
  colonne droite : libellé à `x = w/2`, cellule touche à `x = w − 80 − 3` ;
- **les deux textes sont centrés** dans leur cellule, police 12 ; le libellé est
  **tronqué à 130 px** (`sub_A21F90`) ;
- séparateur horizontal de **215 px** (= 135 + 80) sous chaque ligne, `0xC0C0C0` ;
  filet vertical de 1 px à gauche de la cellule touche ; séparateur vertical
  plein hauteur en `w/2` ;
- **fond de la cellule touche selon la catégorie** (COLORREF `0x00BBGGRR`) :
  catégorie 0 → `0xFFEFD6` (bleu pâle, celui de la capture), catégorie 3 →
  `0xEFD6FF` (rose), catégories 1 et 2 → `0xDFFFFE` (jaune pâle) ;
- surlignage de la ligne sélectionnée sur la **cellule libellé** : `0xE1E1FF` ;
- **`str2` vide ⇒ la ligne n'existe pas** et est sautée (le compteur d'affichage
  n'avance pas) — c'est le test de validité ;
- `str1` vide ⇒ `MsgString(1700)` *« Unspecified value »* si `keycode1 != 0`,
  sinon `MsgString(1518)` *« Not Assigned »* ;
- si la pagination est active, indicateur `page / total` à y = 25 près du bouton
  Prev : la page en `0xFF3C7F`, le `/ total` en `0x666666`, police 11.

### 4.7 Interaction, pagination, boutons

**Sélection d'une ligne** — `UIHotKeyWnd_OnLButtonDown_SelectRow` (0x008F6A60) :
`y < 17` → traitement de barre de titre ; sinon `UIHotKeyWnd_HitTestRow`
(0x008E1CB0) → `+0xB4`. Un clic dont l'index dépasse
`GetOriginalListSize(catégorie)` corrigé de l'offset de page
(`36·(page−1)`) est **ignoré**. Sur une ligne valide : la bulle prend le texte
1486, est **centrée horizontalement** sur la fenêtre à la hauteur de la ligne,
devient visible, et `sub_A39130(mgr, bulle)` lui donne le focus — **c'est ainsi
que la capture de touche est armée**.

**🔑 Capture de la touche — le clavier entier est détourné.**
`UIWindowMgr_OnKeyDown` (0x00A471E0) contient, à **`0x00A47201`**, un test
`FindWindow(0x9C)` : dès que la fenêtre existe et qu'on est en jeu, la frappe part
en `0x00A47235` → composition du combo (`sub_A2D450`) → si `mgr+0x404` est non nul,
**`UIHotKeyWnd_AssignKeyToSelectedRow(wnd, combo)`** (0x008DC180) puis
**`return 0`** : la touche est **consommée**, aucun raccourci ne se déclenche.
C'est pour cela que la fenêtre ouverte gèle les hotkeys du jeu.

`UIHotKeyWnd_AssignKeyToSelectedRow` — l'état de la frappe est lu dans
`*(int*)0x0131F4F8` (touche principale, déréférencée) et `0x0131F4FC`
(modificateur) :

1. repositionne et affiche la bulle (texte 1486) **en `0xFFFFFF`**, centrée
   horizontalement, à la hauteur de la ligne sélectionnée ;
2. si aucune ligne n'est sélectionnée (`+0xB4 < 0`) : sortie ;
3. **cas particulier catégorie 1 / commande 31** : Alt (18), Ctrl (17) ou Shift
   (16) seuls → message **1488** ; Espace (32) ou 144 sans modificateur →
   message **1487** ;
4. validation générale `UIHotKeyWnd_ValidateKeyCombo` (0x008DC890) : `1` =
   ignorer, `2` → **1487** *« Unable to specify a single key. »*, `3` → **1488**
   *« Unable to specify the key assigned. »* — la bulle est recolorée
   (`+144 = 0x00FFFF`) ;
5. sinon `GetRowBinding(catégorie, ligne + 36·page − 36)` — **l'offset de page est
   appliqué ici**, pas dans le hit-test — puis :
   - **ESC (27)** ⇒ nom de touche = **chaîne vide** = **effacement** de
     l'affectation (c'est ce que promet le texte 1486) ;
   - sinon global Lua **`GetKeyDes(kc1, kc2)`** (fmt `"dd>s"`) → le nom lisible ;
   - `UIHotKeyWnd_WritePendingBinding` (0x008DC6C0) écrit dans la map d'édition
     **en attente** — toujours rien de commis avant OK.

Le message **1489** *« This key is already registered to [[%s]]. Bind it to this
function instead? »* n'apparaît pas sur ce chemin : le contrôle de collision entre
deux commandes se fait ailleurs (non localisé).

**Pagination** — `OnMsg` msg **22** (changement d'onglet) : catégorie ←
`TabIndexToCategory`, sélection ← −1, page ← 1, Prev/Next masqués ; puis si
`GetOriginalListSize(catégorie) > 36` → pagination active, `nb pages =
ceil(taille/36)`, bouton Next affiché. **36 lignes par page** (2 × 18).
`UIHotKeyWnd_UpdatePagingButtons` (0x008F99F0) : Prev visible si `page > 1`,
Next visible si `page < total`.

**Les quatre boutons** (`OnMsg` **msg 6**, la commande étant le paramètre
SUIVANT — `UIHotKeyWnd_OnMsg` 0x008FB130) :

| cmd | bouton | effet |
|---|---|---|
| **184** | OK | `++*(g_UserHotkeyMgr+8)` puis `CommitPendingBindings` puis **ferme 156** |
| **185** | cancel | **vide les 4 maps** d'édition et redessine — **ne ferme pas** |
| **201** | close | **ferme 156** sans rien commettre |
| **363** | Reset | `StageDefaultBindings` — met les défauts **en attente**, il faut encore OK |
| 316 / 317 | Prev / Next | page ∓ 1 puis `UpdatePagingButtons` |
| **213** | (bascule) | `g_ChangeChatMode = valeur` + reconfigure `UINewChatWnd` |

⚠ **[Reset] ne réinitialise rien tout seul.** `UIHotKeyWnd_StageDefaultBindings`
(0x008E2930) vide les maps puis les **remplit** avec les défauts lus par
`GetOriginalHotKeyInfo` ; sans un clic sur OK ensuite, `cancel`/`close` annule
tout. (Le texte `MsgString(1490)` *« Stored shortcut key combination will be
initialized. Do you want to continue? »* existe, mais **`OnMsg` case 363 n'affiche
aucune modale** : la confirmation n'est pas sur ce chemin.)

**Enable Battle Mode** — 🔴 **213 est une COMMANDE, pas un message** : comme 184,
185 et 363, elle ne se lit que sous **`msg == 6`** (clic bouton). Envoyée en
message, elle tombe dans `UIWindow_OnMsg_Default`, qui ne fait rien et ne se
plaint pas — la bascule semble alors marcher à l'écran et n'agit jamais (erreur
faite puis corrigée le 2026-08-14). Émise par le toggle (`param_1 == +0x108`) :
`g_ChangeChatMode ← (a5 != 0)`. Si on active, la fenêtre de chat est simplement
invalidée ; si on désactive, le client déploie la barre de saisie
(`g_ChatInputBarDeployed = 1`, `sub_8F9840`), refait le bandeau d'onglets
(`UINewChatWnd_SetTabBarHeight`) et purge les liens d'objets du chat
(`UIItemTagOnChat_ClearLinks`). Un remplacement qui n'écrirait que le booléen
laisserait le chat dans un état incohérent.

### 4.8 Persistance

`UIHotKeyWnd_CommitPendingBindings` (0x008FA220) parcourt les 4 maps (index =
**catégorie**) et, pour chaque nœud, appelle le global Lua :

```
ChangeUserHotKey(catégorie+1, cmdId, libellé_EXE, keycode1, keycode2)   -- fmt "ddsdd"
```

puis `SaveUserHotKeys2("SaveData\UserKeys" + ".lua")` et affiche au chat
`MsgString(3572)` = *« The hotkey settings are stored on the server on normal
exit. »* — d'où la remarque du §2.7 : couper le processus sans passer par
`CZ_REQ_DISCONNECT` perd la sauvegarde serveur.

`SaveData\UserKeys.lua` ne contient **que les surcharges** (le fichier du client
commence à `[9]`, les slots 0-8 = F1..F9 étant restés aux défauts).

#### Les TROIS états d'une commande (vérifié sur le fichier, 2026-08-14)

| état de `UserKeys.lua` | ce qui agit | ce que rend `GetHotKey` |
|---|---|---|
| **aucune entrée** | la touche d'ORIGINE | la touche d'origine |
| entrée **avec** `KEY1` | la touche choisie | la touche choisie |
| entrée **sans** `KEY1` | **plus rien** — commande DÉLIÉE | rien |

Le troisième état est ce qu'écrit l'**Échap** du natif sur une ligne
sélectionnée, et il est bien persisté : `USERKEY_2 = { [21] = { EXE = "Screenshot" } }`
— entrée présente, `KEY1` absent. Vérifié en jeu : Alt+H retiré de la liste
d'amis, la fenêtre ne s'ouvre plus.

⚠ **Piège de lecture payé** : voir `USERKEY_2` **vide** alors que les commandes
d'interface fonctionnent ne prouve PAS qu'on ne peut pas délier — cela prouve
seulement qu'une entrée ABSENTE laisse agir la touche d'origine. J'en avais
conclu à tort que l'effacement était impossible, et fait retomber l'affichage
**et le contrôle de collision** sur `GetOriginalHotKeyInfo` — ce qui déclarait
occupée la touche d'origine d'une commande que le joueur venait justement de
délier. `GetHotKey` rend **déjà** la touche effective ; il n'y a rien à corriger.

Reste vrai, et utile : `UserHotkey_ResolveBehavior` (`0x00A32C10`) interroge le
global Lua **`GetBehaviorOfHotKey2`** (fmt `"ddd>d"`) **à chaque frappe** — la
résolution est VIVANTE, une écriture par `ChangeUserHotKey` agit **immédiatement,
sans relog**.

📌 **Le cas particulier `Screenshot`** : sa touche est Impr. écran, que **Windows**
intercepte avant le jeu (Outil Capture d'écran sur Windows 11). Même déliée, la
capture système part. Elle n'émet d'ailleurs aucun `WM_KEYDOWN`, seulement le
`WM_KEYUP` — d'où son traitement à part dans `Game_MainWndProc`. Une interface de
remappage n'a donc rien à en dire : ni la proposer comme touche, ni montrer la
ligne comme réglable.

Ponts C→Lua voisins (déjà connus) : `UserHotkey_Lua_ChangeHotKey` 0x005D56D0,
`…ClearUserHotKeys` 0x005D4910, `…SaveUserHotKeys2` 0x005D54C0,
`…GetHotKey` 0x00D80950, `…GetOriginalListSize` 0x00D81680 ;
`UserHotkey_SaveToTable` 0x0059EEF0 / `…LoadFromTable` 0x0059E2C0 pour `UIInfo.lua`.

Messages d'erreur prévus par le natif, à reprendre : `MsgString(1487)` *« Unable
to specify a single key. »*, `1488` *« Unable to specify the key assigned. »*,
`1489` *« This key is already registered to [[%s]]. Bind it to this function
instead? »*.

### 4.9 Le chemin d'ÉCRITURE (RE du 2026-08-14)

Trois primitives suffisent, toutes vérifiées au désassemblage :

| fonction | adresse | signature |
|---|---|---|
| `UserHotkey_Lua_ChangeHotKey` | `0x005D56D0` | `int __stdcall(int catégorie, int cmdIdx, int key1, int key2, const std::string* nom)` |
| `UserHotkey_Lua_SaveUserHotKeys2` | `0x005D54C0` | `int __stdcall(void)` |
| `std_string_assign` | `0x004F1940` | `void* __thiscall(void* str, const void* src, size_t len)` |

⚠ **L'ordre des arguments C n'est PAS celui du Lua.** `ChangeUserHotKey` reçoit le
`std::string*` en **dernière** position côté C, alors qu'il est le **3ᵉ** argument
côté Lua : le pont réordonne en poussant `fmt="ddsdd"`, `catégorie+1`, `cmdIdx`,
`c_str`, `key1`, `key2`. Le `c_str` est extrait sur place par le test SSO
classique (`capacité >= 0x10` ⇒ déréférencer).

⚠ `SaveUserHotKeys2` **ne prend aucun argument** : il fabrique lui-même
`"SaveData\UserKeys" + ".lua"` et appelle le global Lua avec le format `"s"`.

**Encodage des touches = VK Windows.** `UserKeys.lua` stocke `KEY1 = 65` pour
« A », et le natif compare la touche pressée à 16 / 17 / 18 (Shift / Ctrl / Alt).
`key2` porte le modificateur, 0 s'il n'y en a pas. La capture maison
(`hotkeys::CaptureMainVk`) rend exactement ces codes — aucune table de conversion.

**La touche pressée** vit dans deux globales écrites par le handler clavier :
`*(int*)0x0131F4F8` (touche principale, déréférencée) et `0x0131F4FC` (modificateur).

#### `UIHotKeyWnd_ValidateKeyCombo` (0x008DC890) = le CONTRÔLE DE COLLISION

Ce n'est pas un validateur de forme (mauvais nom donné en première lecture) : il
balaie les autres commandes à la recherche du même couple de touches.

```
pour chaque catégorie 0..3 :
    si (catégorie éditée == 0 et candidate == 3) -> SAUTER
    si (catégorie éditée == 3 et candidate == 0) -> SAUTER
    pour chaque ligne : si (kc1,kc2) == combo pressé -> collision
```

🔴 **Les deux barres de raccourcis (catégories 0 et 3) ont le DROIT de partager
une touche** — ce sont deux pages de la même barre, permutées par une option. Les
catégories Interface et Macros, elles, entrent en collision avec tout.

Suite du traitement :
- collision sur la ligne en cours d'édition → renvoie **1** (rien à faire) ;
- collision ailleurs → modale `MsgString(1489)` ; réponse ≠ 187 → renvoie **1** =
  **abandonner l'affectation** ; réponse 187 → **efface l'ancienne affectation**
  (`WritePendingBinding` avec key1=key2=0 et un nom vide) puis renvoie **0** ;
- cas particulier : si l'ancienne était en catégorie 0 alors qu'on édite une autre
  catégorie, la même touche est AUSSI effacée en catégorie 3.

**Convention de retour : 1 = abandonner, 0 = poursuivre.**

**La validation de FORME est en amont**, dans `sub_A2D450` (appelé par
`UIWindowMgr_OnKeyDown`), qui rend un « genre » consommé par
`UIHotKeyWnd_AssignKeyToSelectedRow` : 1 = ignorer, 2 → message 1487, 3 → message
1488, 4 → lancer le contrôle de collision ci-dessus.

📌 **Conséquence de conception pour le portage** : il ne faut PAS appeler
`ValidateKeyCombo` — il exige une instance de la fenêtre native, lit les globales
du handler clavier et ouvre une modale bloquante. Un panneau ImGui tient déjà
**toutes** les lignes en mémoire : la collision se cherche en C++, sur ses propres
données, avec sa propre popup — en rejouant les deux règles ci-dessus (exemption
0↔3, et effacement de l'ancienne affectation avant d'écrire la nouvelle).

#### ✅ Écrire par `ChangeUserHotKey` SUFFIT à la synchro web (vérifié 2026-08-14)

Question posée par le `++*(g_UserHotkeyMgr+8)` du bouton OK : faut-il lever un
drapeau pour que le raccourci parte au serveur ? **Non.** `UserHotkey_SaveToTable`
(`0x0059EEF0`, le sérialiseur de la charge `/userconfig/save`) commence par
`UserHotkey_RebuildOverrideListFromLua` (`0x005D5150`), qui **vide puis reconstruit**
la liste des surcharges en interrogeant le Lua ligne par ligne
(`GetUserHotKeyInfo(cat+1, cmdIdx)`, fmt `"dd>ddss"`) et ne garde que celles dont le
nom de touche est non vide. La charge est donc **rebâtie depuis le Lua à chaque
sauvegarde** : ce que `ChangeUserHotKey` a écrit s'y retrouve, sans état
intermédiaire à entretenir. (L'ordre de parcours y est celui des ONGLETS — 0, 3, 1,
2 — d'où les clés `SkillBar_1Tab`, `SkillBar_2Tab`, `InterfaceTab`, `EmotionTab`.)

⚠ **Un devoir caché, tout de même** : `CommitPendingBindings` appelle
`UIWindowMgr_RefreshGameSettingsHotkeyLabels` (`0x00A4CCD0`) après chaque écriture
**de la catégorie 1 (Interface) seulement** — elle rafraîchit les libellés de
touches affichés par Game Settings (`FindWindow(10014)` puis `sub_9F0E20`). En
interface moderne la native `0x271E` n'est jamais ouverte : c'est un no-op. À
rejouer uniquement si l'on garde la fenêtre d'options native.

---

## 5. Blueprint : remplacement en ImGui

### 5.1 Principe

Les trois fenêtres sont de bonnes candidates : peu de logique métier, beaucoup
d'affichage, et **aucune ne pilote un flux serveur continu**. Le seul état
réellement partagé est le magasin d'options (Lua) et la table de raccourcis
(Lua + serveur) — qu'il faut **router vers le natif**, jamais recopier.

**🔴 Détruire, pas masquer.** Le schéma universel du client est
*ferme-si-existe / crée-sinon* : une native laissée vivante mais masquée
**avale un appui sur deux** et **garde le clavier** (Entrée/Espace cliquent son
bouton par défaut). Recette établie (storage, character sheet) : masquer `+0x28`
dans le hook de création (anti-scintillement, on ne peut pas détruire là — le
natif manipule encore la fenêtre), puis **détruire dans `OnTick`** via
`uiwnd::CloseWindow(id)`. Cf. `reference_native_window_toggle_router`,
`feedback_hidden_native_window_keyboard`.

**La question à se poser avant de détruire tôt** — *la fenêtre émet-elle quelque
chose à la naissance ou à la mort ?* Réponse ici, vérifiée : **aucune des trois**
n'envoie de paquet en naissant ni en mourant. Les seuls émetteurs sont les
**boutons** du menu Échap. Donc les trois se détruisent sans devoir rejouer de
devoir de session — contrairement à l'écriture de courrier (0x108) ou au
sertissage (0x4A).

### 5.2 Points d'interception

| fenêtre | intercepter | pourquoi ici |
|---|---|---|
| Game Options 155 | `UIWindowMgr_MakeWindow` case **155** (et/ou le slot `mgr+0x408`) | **tous** les déclencheurs y passent (touche Échap comme icône de menu cmd 193), et la fenêtre est unique par construction |
| Game Settings 0x271E | le hook de création `GameSettingsUI_Create` (0x009D2CC0) ou, plus simplement, **ne jamais l'ouvrir** : notre menu Échap appelle notre propre panneau | seul le bouton 458 l'ouvre — supprimer l'appelant suffit |
| Shortcut Settings 0x9C | idem : seul le bouton 370 l'ouvre | idem |

**Conséquence de conception forte** : si l'on remplace le **menu Échap** en
premier, les deux autres fenêtres n'ont plus **aucun** chemin d'ouverture
(vérifié : 0x271E n'apparaît que sur la commande 458, 0x9C que sur 370 depuis
cette fenêtre ; `0x12F` de la table des icônes est une **autre** fenêtre). Le
remplacement devient donc *additif* et non *défensif* : pas de détour à poser sur
les deux grandes fenêtres tant que le menu est à nous.

Vérifié par recherche d'octets sur toute l'image (`68 1E 27 00 00` et
`68 9C 00 00 00`) : le seul `MakeWindow(0x271E)` est celui de la commande 458
(`0x008FAFBA`) — les autres occurrences de `0x271E` sont la déclaration de fabrique,
des `FindWindow` internes à la fenêtre, et **deux `CMode::SendMsg(10014)`**
(`0x00877597` dans `UIMakeCharWnd_OnCreate`, `0x008E52FB`) qui n'ont rien à voir :
*10014 est aussi un numéro de message de `CMode`*, homonymie pure. De même,
`UIMenuIconWnd_OnMsg` et `UIWindowMgr_DispatchHotkeyBehavior` ne référencent
**jamais** `0x9C`.

✅ **La frappe Échap, elle, est dans `sub_A449A0` @ `0x00A44C40`** (localisée
2026-08-13 en diagnostiquant un vrai bug de portage, cf. §5.6 piège 10). Ce n'est
pas `DispatchHotkeyBehavior`, qui ne référence jamais 155 ; la barre d'icônes a
son propre site (`0x00814CEA`). Le branchement Échap :

```c
mode = GameMode_GetActive();  if (!mode) …
if (g_ReplayActive) return;                     // en replay, Échap ne fait rien
other = FindWindow(0x2728);                     // 10024
if (SaveRectAndCloseWindow(155)) return;        // le menu existait -> FERMÉ, fini
if (other) return;                              // 0x2728 ouverte -> on n'ouvre rien
MakeWindow(155);                                // sinon -> ouverture
```

C'est le *ferme-si-existe / crée-sinon* habituel, et il confirme le choix
d'interception : la fenêtre étant détruite, `SaveRectAndCloseWindow` échoue
toujours et l'ouverture repasse toujours par `MakeWindow`. ⚠ **Le retour de
`MakeWindow` est ignoré sur ce chemin** (`xor al, al` juste après) — mais pas sur
les autres, donc détruire depuis l'intérieur du hook reste exclu.

À ne pas confondre avec la **capture** de touche de la fenêtre de raccourcis,
localisée en §4.7.

### 5.3 Découpage proposé

Conforme à `src/features/<nature>/` (cf. `project_source_layout`) :

```
src/features/windows/game_menu.{h,cc}        // le menu Échap (5 boutons + les 2 de mort)
src/features/windows/game_settings.{h,cc}    // les 5 onglets
src/features/windows/hotkey_settings.{h,cc}  // la table des raccourcis
src/ragnarok/option_info.{h,cc}              // accès TYPÉ au magasin d'options natif
src/ragnarok/user_hotkey.{h,cc}              // accès TYPÉ aux raccourcis (GetHotKey, ChangeUserHotKey…)
```

Les deux derniers sont le vrai gain : ils existent déjà en miettes
(`hotkey_util.cc`, `skill_bar.cc` déclarent chacun `kGetHotKey = 0x00d80950`).
Les mutualiser suit la méthode de `project_address_directory` — **relever par NOM
ET VALEUR**, supprimer les déclarations avant de substituer les noms.

### 5.4 Ce qu'il faut router vers le natif (et ne jamais dupliquer)

| besoin | passer par | ne PAS faire |
|---|---|---|
| valeur d'un réglage | `OptionInfo_GetInt/Float/String` | lire `SaveData\OptionInfo.lua` soi-même |
| écrire un réglage | le setter natif du groupe puis `OptionInfo_SaveToFile` | écrire le `.lua` à la main : `LoadAndApplyAll` ne serait pas rejoué |
| défaut d'une option | l'`unordered_map` `0x012515FC` (clé = `ID`) | recoder une table de défauts (et surtout : ne pas oublier le XOR des 5 ids) |
| libellés / infobulles | `MsgStringTable_GetById` | figer l'anglais en dur — le client a un réglage de langue |
| liste des lignes d'un onglet | le vecteur `OptionTbl` (`mgr+0x0C..0x10`, pas de 100 o) | reconstruire une liste : `GameSettings.lub` est dans le GRF et peut changer |
| valeur d'une bascule | `GameSettings_GetFlag ^ IsInvertedOption` (§3.5.1) | lire le drapeau sans le XOR : cinq options s'afficheraient à l'envers |
| écrire une bascule | `CGameSettingsMgr::SetOption` | `SetFlagRaw` : le handler de l'option ne serait pas appelé, donc l'effet pas appliqué |
| touche d'une commande | `UserHotkey_Lua_GetHotKey` | table de touches en dur (déjà refusé une fois : c'est **layout-aware**, AZERTY/QWERTY) |
| nom lisible d'un combo | global Lua **`GetKeyDes(kc1, kc2)`** (fmt `"dd>s"`) | fabriquer « Ctrl+F1 » soi-même : le natif s'en sert pour écrire dans `UserKeys.lua`, les deux doivent coïncider |
| remapper | Lua `ChangeUserHotKey` + `SaveUserHotKeys2` | écrire `UserKeys.lua` : le serveur ne serait pas informé |
| appliquer la config graphique | `sub_9EDB30(page_graphics)` | tenter un reset de device maison |

**Contrainte de conception** : un panneau ImGui n'a pas de « page Graphics »
native à qui passer `sub_9EDB30`. Deux options — (a) garder la fenêtre 0x271E
vivante et invisible **uniquement** pour son onglet Graphics (mais on retombe sur
le vol de clavier), ou (b) **laisser l'onglet Graphics au natif** en première
étape et ne porter que Basic/Effects/Controls/Other. **(b) est recommandé** :
le changement de résolution est rare, dangereux, et déjà couvert par `Setup.exe`
(cf. `project_dx7_dx9_rendering`).

### 5.5 Les trois actions à répliquer

```c
// Character Select
Vf<SendMsg_t>(mode, 0x18)(mode, 25, /*type*/1, 0, 0, 0);
// + fermer 155, 164, 269 (devoirs cachés du handler natif)

// Return to Save Point (seulement si le personnage est mort)
// confirmation d'abord : MsgString(1548)
Vf<SendMsg_t>(mode, 0x18)(mode, 25, /*type*/0, 0, 0, 0);

// Exit to Windows  →  demande la déconnexion, NE quitte PAS le processus
Vf<SendMsg_t>(mode, 0x18)(mode, 88, 0, 0, 0, 0);   // → 128 → CZ_REQ_DISCONNECT 0x018A

// Résurrection sur place (mort + jeton/statut 580)
Vf<SendMsg_t>(mode, 0x18)(mode, 250, 0, 0, 0, 0);  // → CZ_STANDING_RESURRECTION 0x0292

// Return to game : ne rien envoyer, juste fermer le panneau.
```

`mode = GameMode_GetActive(0x01213338)` — ⚠ **ne pas** substituer une lecture
directe de `0x0121333C` : la garde `+0x58` masque le mode pendant les transitions
(cf. `project_address_directory`).

Le panneau doit reproduire les **trois dispositions** : lire l'état « mort »
(`GameMode + 0x250`), puis `Own_HasResurrectionToken` / statut 580 pour décider
entre 2 et 3 boutons. Un joueur mort qui verrait « Character Select » et pas
« retour au point de sauvegarde » serait bloqué.

### 5.6 Pièges à ne pas repayer

1. **`AlwaysAutoResize` sans largeur explicite** s'emballe : donner des largeurs
   MESURÉES (`feedback_ui_width_measured_not_hardcoded`,
   `feedback_imgui_autoresize_needs_explicit_widths`). Ici c'est critique : la
   table des raccourcis a deux colonnes fixes (135 + 80) et **la police et la
   langue sont des réglages** — donc mesurer, pas figer.
2. **Fenêtre RO = fond CLAIR** : `TextDisabled` y est illisible
   (`feedback_imgui_ro_light_body_colors`). Le natif dessine du texte sombre sur
   cellules pastel.
3. **Aucune commande native pendant une frame ImGui** : `MakeWindow`,
   `CloseWindow`, `OnMsg` déclenchés depuis le rendu → **freeze muet**. Différer
   (`feedback_no_native_cmd_during_imgui_frame`). Or les trois actions font
   exactement cela (fermer 155/164/269, envoyer un paquet) : **les mettre en
   file** et les exécuter au tick.
4. **Pas de dialogue bloquant sur le fil principal** : les confirmations
   (1548, 1490, 3166-3168) doivent être des popups ImGui, pas des `MessageBox`
   (`feedback_no_blocking_dialog_main_thread`).
5. **Capture clavier ciblée** : un panneau qui capture le clavier en permanence
   tue le chat. Utiliser le prédicat `WantsKeyboard` /
   `feedback_imgui_keyboard_targeted_capture` — et pour le remappage, réutiliser
   `hotkeys::CaptureMainVk` / `PingCapture` / `Conflict` qui existent déjà dans
   `src/features/hotkey_util.h` (y compris le contrôle de conflit, que le natif
   annonce avec `MsgString(1489)`).
   ⚠ **Détruire 0x9C supprime aussi le détournement clavier du natif** (§4.7) :
   c'est ce qu'on veut, mais cela transfère la charge. Notre panneau doit donc,
   pendant une capture, **geler les raccourcis** (`PingCapture` à chaque frame,
   ce que `hotkey_util` fait déjà) et traiter **ESC = effacer l'affectation**,
   surtout pas « fermer la fenêtre » — c'est ce que le natif promet au joueur
   dans son propre texte d'aide.
6. **La molette au survol est un verrou** (`feedback_imgui_wheel_scroll_gate`) :
   la liste des raccourcis défile, il faut `mui::RegionWheel`.
7. **Les accents français** : la police ImGui va de `0x20` à `0xFF` et **ne
   contient pas le tiret dans certaines plages** — cf.
   `feedback_imgui_glyph_range_ascii_minus`, `feedback_french_ui_accents`.
8. **Vocabulaire du jeu en FR** : *storage*, *cart*, *shop* restent tels quels
   (`feedback_ui_game_terms_fr`).
9. **Ne pas croire l'IDB sur les libellés** (§0) et **ne pas croire un nom d'IDB
   hérité** : `UIHotKeyWnd_OnCreate` était étiquetée `UINaviSearchWnd_OnCreate`.
10. 🔴 **PIÈGE PAYÉ — la touche qui ouvre le panneau le referme aussitôt.** Vécu en
    jeu sur le menu Échap : il s'ouvrait et se fermait dans la même frappe. Cause :
    Échap est vu **deux fois** sur la frappe d'ouverture. Le jeu le traite (handler
    `0x00A44C40` → `MakeWindow(155)` → notre hook ouvre le panneau) **et** ImGui le
    reçoit, si bien que `ro::ProcessEscapeStack` ferme la fenêtre RO du dessus — la
    nôtre, qui vient de naître. Le WndProc n'avale Échap pour le jeu que si une
    fenêtre RO est **déjà** ouverte : à l'ouverture, personne ne l'avale.
    Correctif : **deux frames de grâce** (`ro::SuppressEscapeStack()` par frame)
    après l'ouverture. Ensuite tout s'enchaîne : panneau ouvert ⇒ le WndProc avale
    Échap pour le jeu, ImGui le reçoit, la pile ferme le panneau, le natif n'est
    jamais recréé. **Vaut pour TOUTE fenêtre Bourgeon ouverte par la même touche
    que celle qui la ferme** — c'est-à-dire aussi pour Game Settings et Shortcut
    Settings s'ils reçoivent un raccourci propre.
    Et pendant une modale, `SuppressEscapeStack()` aussi : sinon un Échap fermerait
    la confirmation **et** le menu derrière.
11. 🔴 **PIÈGE PAYÉ — l'état lu au TICK est périmé à la première frame.** Vécu :
    en mourant, le menu s'affichait d'abord en version « vivant » puis se
    transformait sous les yeux du joueur. Cause : la disposition n'était relue que
    dans `OnTick` (~100 ms), donc la première frame dessinait la valeur de la fois
    précédente. Correctif : **relire à l'OUVERTURE** (avant la première frame) *et*
    au tick. Vaut pour tout panneau dont le contenu dépend d'un état natif — les
    valeurs d'options de Game Settings et les lignes de l'onglet courant de
    Shortcut Settings sont exactement dans ce cas. Et l'inverse est vrai aussi : ne
    pas relire à CHAQUE frame ce qui coûte cher (`Own_HasResurrectionToken` fait
    cinq recherches d'inventaire avec construction de `std::string`).
12. **Titre dynamique = même identifiant.** Le menu affiche « Vous êtes mort ! »
    quand le personnage est mort. Les deux titres DOIVENT garder le même suffixe
    `###bourgeon_game_menu` : ImGui identifie une fenêtre par ce qui suit `###`, et
    deux titres sans suffixe commun donneraient deux fenêtres — position, taille et
    z-order repartis de zéro à chaque mort.

### 5.7 Ordre de livraison conseillé

1. **`src/ragnarok/option_info.{h,cc}` + `user_hotkey.{h,cc}`** — les accès
   typés, sans UI. Mutualise au passage les `kGetHotKey` dupliqués. Testable
   par journalisation seule.
2. **Le menu Échap** (5 boutons + les 2 dispositions de mort). Petit, très
   visible, et — point clé — il **coupe le chemin d'ouverture des deux autres
   fenêtres**, ce qui rend la suite additive.
3. **Shortcut Settings**. Le plus rentable des deux gros : modèle de données
   simple (4 catégories × N lignes), et le natif y est franchement daté
   (pagination à 36 lignes, deux colonnes figées). Une liste ImGui avec filtre
   et recherche est un gain net pour le joueur.
4. **Game Settings sans l'onglet Graphics** (Basic + Effects + Controls + Other),
   piloté par `OptionTbl` pour ne rien figer.
5. **Onglet Graphics** en dernier, ou jamais (§5.4).

### 5.8 ✅ Ce qui restait à RE avant l'étape 4 — soldé (2026-08-14)

- ✅ **le lien `OptionTbl.ID` → valeur** : c'est `GameSettings_GetFlag` /
  `CGameSettingsMgr::SetOption`, §3.5.1. Il n'y a pas de « clé `CmdOnOffList` »
  à retrouver par option — l'id EST la clé, et la persistance vers
  `OptionInfo.lua` se fait en aval, dans le handler ;
- ✅ **la bascule d'un `CUIOptionItem`** : `OnCheck` (0x009ECC10) →
  `SetOption(id, coché ^ inversé, annonce=0)`, §3.5.1 ;
- ✅ **la remise à zéro globale** : `CGameSettingsMgr::ResetAllToDefault`
  (0x0068F8E0) parcourt le vecteur et rejoue chaque `Default` ;
- ✅ **les setters de la page Basic** : faits pour le groupe **Audio** et les deux
  bascules `TALKTYPE` (§3.5.2). Skin, priorité et RODEX restent **délibérément**
  au natif : ils n'écrivent pas dans la table d'options (RODEX est même un
  réglage serveur), et chacun demanderait son propre RE pour un gain nul —
  le bouton « Réglages natifs… » du panneau y mène en un clic ;
- ✅ **le contenu de `OptionTbl`** : extrait, en clair, 62 entrées (§3.5) ;
- ⏳ **comment la page-liste connaît son `Tab`** : toujours pas localisé, et c'est
  désormais **sans objet** — le panneau ImGui filtre lui-même sur le champ `Tab`
  du record, qu'il lit directement.

✅ **Soldé** : le contrôle de collision (`MsgString(1489)`) est
`UIHotKeyWnd_ValidateKeyCombo` `0x008DC890` — §4.9. Le remappage est porté :
`HotkeySettings` écrit par `userhotkey::WriteBinding` + `Save`, contrôle la
collision en C++ (`hotkeys::Conflict`, exemption 0↔3 comprise) et refuse au lieu
de voler la touche. Ne reste au natif que son **[Reset]**, faute d'équivalent à
`GetOriginalHotKeyInfo` de notre côté.

### 5.9 État du portage (2026-08-14)

| fenêtre | état | fichiers |
|---|---|---|
| Game Options 155 | ✅ portée | `features/windows/game_menu.{h,cc}` |
| Shortcut Settings 156 | ✅ portée, écriture comprise | `features/windows/hotkey_settings.{h,cc}`, `ragnarok/user_hotkey.{h,cc}` |
| Game Settings 0x271E | ✅ portée **sauf l'onglet Graphics** — Basique complet, skin / RODEX / priorité compris (§3.9) | `features/windows/game_settings.{h,cc}`, `ragnarok/game_settings.{h,cc}` |

Les trois sont **ON par défaut et HORS du groupe « Interface moderne »** : ce sont
des écrans de réglages, pas des morceaux de HUD, et aucun n'a besoin du reste de
l'interface moderne — tout passe par les ponts du client. Clés yaml :
`escmenu_imgui`, `hotkeywnd_imgui`, `gamesettings_imgui`.

✅ **Soldé** : le contrôle de collision (`MsgString(1489)`) est
`UIHotKeyWnd_ValidateKeyCombo` `0x008DC890` — §4.9. Le remappage est porté :
`HotkeySettings` écrit par `userhotkey::WriteBinding` + `Save`, contrôle la
collision en C++ (`hotkeys::Conflict`, exemption 0↔3 comprise) et refuse au lieu
de voler la touche. Ne reste au natif que son **[Reset]**, faute d'équivalent à
`GetOriginalHotKeyInfo` de notre côté.

---

## Voisins

`project_game_settings_ui_re`, `project_shortcut_bar_re` (la barre d'action, qui
consomme les mêmes raccourcis), `reference_native_window_toggle_router`,
`project_ui_window_manager`, `reference_cmode_sendmsg_use_skill`,
`project_settings_file_layout`, `docs/source_layout.md`.
