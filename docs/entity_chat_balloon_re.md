# Bulle de chat au-dessus des entités — RE de `UITransBalloonText`

Rétro-ingénierie de la « frame » native qui affiche le texte au-dessus de la tête
d'une entité qui parle : quels paquets la déclenchent, qui la crée, où elle vit,
comment elle est dimensionnée, positionnée, dessinée et détruite.

Client `20250716` (Moonlight-Destiny), base `0x00400000`, IDB == live.
Vérifié en live avec x32dbg (chaîne de pointeurs déroulée sur le personnage
propre, aucun `memory_search`). Les relevés à chaud sont signalés par ⏱.

> **TL;DR** — La bulle **n'est pas** un dessin de la passe 3D ni un overlay :
> c'est une vraie `UIWindow` de classe **`UITransBalloonText`** (0xCC octets),
> **possédée par l'acteur** au slot **`acteur+0x264`** et enregistrée dans la
> liste du `UIWindowMgr`. Elle est créée **paresseusement** par le **message 7**
> de l'acteur, vit **exactement 5 000 ms** (horodatage `acteur+0x248`), est
> **repositionnée à chaque frame**, puis **détruite** — le slot est remis à 0.
> Les trois paquets qui envoient ce message 7 sont **ZC 0x008E**
> (= `clif_displaymessage`), **ZC 0x008D** (`ZC_NOTIFY_CHAT`) et **ZC 0x02C1**
> (`ZC_NPC_CHAT`, le seul qui transporte une couleur).

> **Cette bulle est REMPLACÉE chez nous** par un overlay ImGui
> (`src/features/overlays/chat_balloon.{h,cc}`). Le §11 décrit le remplacement,
> ce qu'il a fallu neutraliser et pourquoi il n'était pas optionnel.

---

## 1. Les trois émetteurs — la piste `clif_displaymessage` était la bonne

| Opcode | Nom | Handler | Contenu | Cible du msg 7 | Couleur |
|---|---|---|---|---|---|
| **`0x008E`** | *(ZC_NOTIFY_PLAYERCHAT)* — c'est ce qu'émet **`clif_displaymessage`** | `GameMode_OnRecv_SelfChat_ZC008E` **`0x00ccbc60`** | `len@+2`, texte`@+4` | **son propre acteur**, obtenu par `*(*(GameMode+0xCC) + 0x2C)` — pas par une recherche de GID | aucune → **blanc** |
| **`0x008D`** | `ZC_NOTIFY_CHAT` | `GameMode_OnRecv_EntityChat_ZC008D` **`0x00cc8310`** | `len@+2`, `GID@+4`, texte`@+8` | acteur trouvé par `ActorList_FindByGID(GameMode+0xCC, GID)` | aucune → **blanc** |
| **`0x02C1`** | `ZC_NPC_CHAT` | `GameMode_OnRecv_NpcChat_ZC02C1` **`0x00cce340`** | `len@+2`, `GID@+4`, **`couleurRGB@+8`**, texte`@+12` | acteur par GID | **la couleur du paquet** |

C'est exactement l'observation de départ : **chaque `clif_displaymessage` fait
apparaître la bulle**, parce que le handler de `0x008E` ne se contente pas de
pousser la ligne dans le chat — il termine par un `OnMsg(7, texte)` sur l'acteur
du joueur.

Les trois handlers partagent la même forme :

```
1. copier la charge utile dans un tampon local
2. filtre de gros mots  (BannedWord_ScanClean 0x00a85c00, via 0x00a85be0)
3. pousser la ligne dans le chat   -> UIWindowMgr_ChatAction(mgr, 1, texte, couleur, type)
4. envoyer msg 7 à l'acteur        -> LA BULLE
5. si g_ChatAutoSaveOn : journaliser (0x00c82f10)
```

> 🔴 **Garde-fou `0x0131F764`.** Si ce drapeau est non nul, les handlers `0x008E`
> et `0x008D` partent en `ChatAction` **action 5** (2ᵉ fenêtre chat-like,
> `mgr+0x27C`, cf. `docs/chatbox_re.md` §3.2) et **ne créent aucune bulle**.
> C'est le même global que le dépôt connaît déjà sous le nom **`kNoPathFlag`**
> (`src/features/gameplay/keyboard_move.cc:20`, `docs/address_manifest.md:442`)
> et qui bloque aussi l'échoppe (`docs/vending_window_re.md:254`).

---

## 2. Le message 7 — trois étages de `OnMsg` avant d'arriver au bon endroit

L'appel est un `OnMsg` d'acteur classique : `this` en `ecx`, **13 dwords pile**
(`retn 34h`), les arguments allant par **paires 64 bits**.

```
acteur->vtable[+0x08]  = Actor_OnMsg                      0x00d473b0
   msg 7 tombe dans le grand groupe de fall-through
   -> Actor_OnMsg_Base                                    0x00d3c500
        switch msgs 3..0x89 (table d'octets 0xd3d5d5, table de sauts 0xd3d594)
        msg 7 -> cas par DÉFAUT
   -> Actor_OnMsg_AppearanceEffects                       0x00c4aea0
        switch msgs 0..0xa0 (table d'octets 0xc55098, table de sauts 0xc54f40)
        msg 7 -> 0x00c4dace   <-- LA BULLE
```

> ⚠ Ne pas se fier aux commentaires « case N » d'IDA sur les cibles de saut de
> `Actor_OnMsg_Base` : ils numérotent **l'index de la table de sauts**, pas la
> valeur du message. La table d'octets à `0xd3d5d5` est la seule source fiable
> (`msg 3→0, 4→1, 5→2, 6→3, 7..→défaut`). Le cas *index 4* (`0x00d3c5de`) est le
> **message 34**, c'est-à-dire la **jauge de vie** — pas la bulle. Piège coûteux.

### Arguments (offsets pile, `ebx` = base des arguments dans `0x00c4aea0`)

| arg | offset | contenu pour msg 7 |
|---|---|---|
| 1 | `[ebx+0x08]` | 0 |
| 2 / 3 | `[ebx+0x0C]` / `[ebx+0x10]` | **msg = 7** (paire 64 bits ; l'étage `Base` teste la partie haute) |
| 4 / 5 | `[ebx+0x14]` / `[ebx+0x18]` | **`char* texte`** + extension de signe |
| 6 / 7 | `[ebx+0x1C]` / `[ebx+0x20]` | **couleur RGB** (0 = laisser le défaut) |
| 9 | `[ebx+0x28]` | testé conjointement à l'arg 6 pour décider d'appliquer la couleur |

---

## 3. `0x00c4dace` — ce que fait le message 7, ligne à ligne

```c
std::string s(texte);

// (a) transformation des balises / liens
if (g_pNewChatWnd)                       // fenêtre de chat vivante
    s = ChatText_TransformTagLinks(g_pNewChatWnd, s);   // 0x00c4db0b
s = CTagMgr(0x0131ED60)->Transform(s);   // 0x007fbfc0 — balises globales

if (s.empty()) return;                   // texte vide = la bulle existante n'est PAS touchée

acteur[0x248] = timeGetTime();           // horodatage -> expiration à +5000 ms

if (acteur[0x264] == nullptr) {                       // création PARESSEUSE
    w = new UITransBalloonText;                       // operator new(0xCC), ctor 0x00818b90
    w->SetSize(8, 8);                                 // taille bidon, écrasée par l'auto-dim.
    UIWindowMgr_AddWindowToList(g_UIWindowMgr, w);    // 0x00a2d240 -> liste mgr+0x17C
    acteur[0x264] = w;
}

// (b) police du LOCUTEUR
w->[0x9A] = (acteur[0x110] == g_Account_Aid)          // AID de l'acteur == le mien ?
              ? (int16)(int8)g_Own_ChatFontId         // 0x015FB2D0
              : *(uint16*)(acteur + 0xFC);

// (c) texte + auto-dimensionnement
UIBalloonText_SetTextWrapped(w, s.c_str(), 35);       // 0x00830240, 35 caractères/ligne

// (d) couleur (uniquement ZC_NPC_CHAT)
if (arg6 | arg9) { w->[0x90] = arg6; w->[0x94] = 0; }

// (e) position initiale
acteur[0x288] = w->width;                             // mémorisées pour le suivi par frame
acteur[0x28C] = w->height;
w->SetPos(acteur[0xAC] - w->width/2,                  // vtable+0x10
          acteur[0xB0] - UI_ScaleYFrom480(110));      // 0x008d0e60 : n * hauteurEcran / 480
```

Conséquences utiles :

- **Les balises et liens sont résolus avant l'affichage** : la bulle affiche le
  même texte transformé que la fenêtre de chat (`CTagMgr` + le transformateur de
  la chat). Un lien d'objet inséré dans un message apparaît donc aussi au-dessus
  de la tête.
- **Un texte vide ne fait rien** — il ne masque pas la bulle en cours.
- La bulle est **créée une seule fois par acteur** et **réutilisée** tant qu'elle
  n'a pas expiré ; une nouvelle réplique ne fait que remplacer le texte et
  repousser l'horodatage.

---

## 4. La classe `UITransBalloonText`

`RTTI .?AVUITransBalloonText@@` · **vftable `0x010293dc`** · **0xCC octets** ·
ctor `0x00818b90` → base `UIBalloonText` (vftable `0x01029220`, ctor `0x00817170`)
→ `UIWindow` (`UIWindow_base_ctor 0x00a1b190`).

> Même classe utilisée par **`UIWindow_ShowHoverTooltip` `0x00a753d0`** : la
> bulle de chat et l'infobulle de survol sont le **même widget**.

### Champs (offsets absolus)

| Offset | Rôle | Posé par |
|---|---|---|
| `+0x14` / `+0x18` | largeur / hauteur | auto-dimensionnement |
| `+0x1C` / `+0x20` | x / y écran | `SetPos` (vtable+0x10) |
| `+0x24` | surface de dessin (son vtable+0x2C = FillRect) | base `UIWindow` |
| `+0x80` | **taille de police** = **12** | `UIBalloonText_ctor` |
| `+0x84` / `+0x88` / `+0x8C` | **`std::vector<std::string>` des LIGNES** (24 o/élément) | `SetTextWrapped` |
| `+0x90` | **couleur par défaut du texte** = `0xFFFFFF` | ctor, écrasée par `ZC_NPC_CHAT` |
| `+0x94` | couleur d'ombre / 2ᵉ passe | ctor = 0 |
| `+0x9A` (WORD) | **index de POLICE du locuteur** (0..9) — *pas* une couleur | msg 7 |
| `+0x9C` (BYTE) | 0 → rendu **ombré**, ≠0 → rendu **coloré** ; choisit aussi l'algo de coupure | ctor = 0 |
| `+0xA8` / `+0xAC` | copie de largeur / hauteur | `UITransBalloonText_SetSize` |
| `+0xBC` / `+0xC0` / `+0xC4` | vecteur de **couleurs par ligne** (vide → `+0x90`) | ctor = vide |
| `+0xC8` | **couleur du contour = `0x5C5C5C`** | `UITransBalloonText_ctor` |

### Méthodes redéfinies par rapport à `UIBalloonText`

| Slot | `UITransBalloonText` | Rôle |
|---|---|---|
| `+0x00` | `0x0081b590` | destructeur |
| `+0x04` | `0x0082b5f0` | `SetSize` — appelle la base puis recopie w/h en `+0xA8/+0xAC` |
| `+0x10` | `0x00822d90` | `SetPos` |
| `+0x1C` | `0x00821280` | — |
| `+0x3C` | `0x00823330` | — |
| `+0x50` | `0x008263a0` | **`Paint`** (§6) |

---

## 5. Coupure de lignes et auto-dimensionnement

**`UIBalloonText_SetTextWrapped(this, texte, 35)` `0x00830240`**

```
si strlen(texte) == 0 -> ne touche à rien
vider le vecteur de lignes (+0x84..+0x88)
si (+0x9C)  sub_97A420(texte, &lignes, 35, 0)     // variante « coloré »
sinon       sub_979F10(texte, &lignes, 35)        // variante « ombré »
si le vecteur est non vide -> UIBalloonText_AutoSizeToLines(this)
```

**`UIBalloonText_AutoSizeToLines` `0x0081b8b0`** mesure chaque ligne avec la
police sélectionnée par `+0x9A`, puis :

```
largeur = max(largeur de ligne) + 12          // marge de 6 px de chaque côté
hauteur = 4 + Σ (hauteur de ligne + 4)
SetSize(largeur, hauteur)   puis   repaint (vtable+0x98)
```

### Table des polices (`+0x9A` → identifiant de police du moteur de texte)

| `+0x9A` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| id police | *défaut (0)* | 21 | 20 | 22 | 23 | 24 | 25 | 26 | 27 | 28 |

Pour `+0x9A != 0`, la taille de police est **forcée à 12** au lieu de `+0x80`.

**D'où vient cet index ?**

- **`ZC_FONT 0x02EF`** → `GameMode_OnRecv_Font_ZC02EF` **`0x00cc8b50`** :
  `{ AID@+2, police@+6 }`. Si `AID == g_Account_Aid` → **`g_Own_ChatFontId`
  `0x015FB2D0`** (octet) ; sinon → **`acteur+0xFC`** (WORD).
  Côté serveur c'est l'atcommand `@font`.
- **À l'entrée en map** : `g_Own_ChatFontId` est aussi posé depuis l'octet
  `+0x0B` du paquet d'acceptation (`0x00cb10b9` et `0x00d2bcc8`).

---

## 6. Le rendu — `UITransBalloonText_Paint` `0x008263a0`

```c
if (lignes.empty()) return;                       // rien à peindre

UIWindow_ClearSurface(this, 0xFFFF00FF);          // magenta = COULEUR-CLÉ -> transparent
UIWindow_DrawRoundedRectOutline(this, 0, 0, w, h, this->[0xC8]);   // 0x00a1d760

y = 4;
pour chaque ligne i :
    couleur = (i < nb couleurs par ligne && couleurLigne[i] != 0) ? couleurLigne[i] : this->[0x90];
    si (this->[0x9C])  UIText_DrawColored     (6, y, ligne, 0, &couleur, idPolice, taille, 1);
    sinon              UIText_DrawShadowedRun (this, 6, y, ligne, 0, couleur, this->[0x94], idPolice, taille, 0);
    y += hauteurLigne + 4;
```

**Le « cadre » n'est qu'un liseré.** `UIWindow_DrawRoundedRectOutline`
`0x00a1d760` empile **12 rectangles pleins** (4 arêtes + 8 marches de coin) sur
la surface via `surface->vtable[+0x2C]` — il n'y a **aucun remplissage** :
l'intérieur reste en couleur-clé, donc transparent. C'est pour ça que le fond de
la bulle laisse voir la scène. La couleur du liseré est **`0x5C5C5C`**, en dur
dans le constructeur (`this[50] = 0x5C5C5C`).

---

## 7. Cycle de vie et positionnement — `CActorSprite_UpdateOverheadWidgets` `0x00c46680`

Appelée par frame pour chaque acteur. Premier bloc = la bulle (`acteur+0x264`) :

```c
if (acteur[0x264]) {
    if (timeGetTime() >= acteur[0x248] + 5000) {          // <-- DURÉE DE VIE : 5 000 ms
        UIWindowMgr_QueueDestroyWindow(g_UIWindowMgr, acteur[0x264]);
        acteur[0x264] = 0;
    } else {
        x = acteur[0xAC] - acteur[0x288] / 2;                                  // centrée sur l'acteur
        y = acteur[0xB0] - g_OverheadWidgetYScale * acteur[0x5C] - acteur[0x28C];
        x = clamp(x, -3, largeurEcran  - w + 3);          // jamais complètement hors-écran
        y = clamp(y, -3, hauteurEcran  - h + 3);
        bulle->SetPos(x, y);                              // vtable+0x10, CHAQUE FRAME
    }
}
```

- `acteur+0xAC` / `acteur+0xB0` = position **écran** de l'acteur ;
  `acteur+0x5C` = facteur de hauteur (float) du sprite.
- **`g_OverheadWidgetYScale` `0x015e5b48`** = `81 * hauteurEcran / 480`, calculé
  **une seule fois** (magic static thread-safe, garde `0x015e5b4c`) → **il ne
  suit pas un changement de résolution en cours de session**.
- La bulle est en plus détruite quand l'acteur disparaît :
  **`ActorAiClass_DestroyAttachedUI` `0x00c459e0`** vide tous les slots d'UI
  attachés (voir §8).

---

## 8. La famille des widgets « au-dessus de la tête »

Tous détruits ensemble par `ActorAiClass_DestroyAttachedUI` `0x00c459e0`.

| Slot acteur | Widget | Créé par | Position (par frame) |
|---|---|---|---|
| **`+0x264`** | **`UITransBalloonText` — la bulle de chat** | **msg 7** `0x00c4dace` | centrée, au-dessus de la tête ; **expire à `+0x248 + 5000 ms`** |
| `+0x268` | *(widget de tête)* | — | `(x-70, y-17)` |
| `+0x26C` | *(widget de tête)* | — | `(x-70, y-34)` |
| `+0x270` | **barre d'INCANTATION** — `UIRechargeGage` (vftable `0x0102bbc8`, 0xA4 o), 60×6 px, `UIRechargeGage_SetProgress(écoulé, total)`. **RE complète : [`cast_bar_re.md`](cast_bar_re.md)** | **msg 82** `0x00c4d955` (retirée par **msg 83** à l'expiration de `+0x280`) | `(x-30, y - échelle×hauteur)` = **au-dessus de la tête** |
| `+0x274` | *(widget de tête)* | — | `(x-70, y-34)` |
| **`+0x488`** | **`UIPcGage` — la jauge de vie 60×5 px** | **msg 34** `0x00d3c5de` (détruite par **msg 35** `0x00d3c5a7`) | `(x-30, y + scale(12))` |

Champs annexes de l'acteur : `+0x248` horodatage de la bulle · `+0x288`/`+0x28C`
largeur/hauteur de la bulle · `+0x280`/`+0x284` fin/début de la jauge temporisée.

**Deux barres, deux classes, deux places — confirmé à l'écran (2026-08-08) :** la
**vie** est `UIPcGage` à `+0x488`, **sous les pieds** (`y + échelle(12)`) ;
l'**incantation** est `UIRechargeGage` à `+0x270`, **au-dessus de la tête**. La
géométrie du code le disait déjà, mais l'inverse se plaide bien : ne pas s'y fier
de mémoire.

✅ **L'incantation est REMPLACÉE en ImGui** (2026-08-09) :
`src/features/overlays/cast_bar.{h,cc}`, plus la septième barre `kCast` de
`BasicInfo` pour la sienne en HUD. 🔴 Contrairement à la bulle, la fenêtre native
n'est **pas détruite** mais **masquée** (`+0x28`) : c'est son horloge
(`+0x280`/`+0x284`) qui alimente la nôtre, et le msg 83 a un devoir caché
(`acteur+0x70 == 8` → `vtable+60`). Tout est dans
[`cast_bar_re.md`](cast_bar_re.md).

⚠ **Ce qui alimente `UIPcGage` (msg 34) reste À TROUVER.** Le candidat évident,
`ZC_HP_INFO 0x0977` (handler `sub_CF33F0` `0x00cf33f0`), est **écarté** : il
envoie le **msg 145**, pas le 34. Piste à creuser :
`Entity_OnAIDPacket_HPUpdate` `0x00c6c838`, déjà nommée dans l'IDB.

> 🔴 **`UIPcGage` n'est pas la bulle.** Classe distincte (`0x0102bca0`, 0xB0 o,
> ctor `0x00836440`), deux jauges (`+0xA0/+0xA4` haute, `+0xA8/+0xAC` basse,
> `+0x9C` = mode double barre), peinte par `UIPcGage_Paint 0x008549a0` — que des
> rectangles, **aucun texte**. C'est le piège dans lequel on tombe si on suit la
> mauvaise entrée de la table de sauts (§2).

---

## 9. Vérification en direct ⏱

Chaîne de pointeurs utilisée (aucun `memory_search` — cf.
`feedback_x32dbg_no_large_memory_search`) :

```
g_ModeMgr           0x01213338
GameMode            = (*(mgr+0x58) == 1) ? *(mgr+0x04) : 0      ⏱ 0x48242208
ActorMgr            = *(GameMode + 0xCC)                        ⏱ 0x4840D5D8
mon acteur          = *(ActorMgr + 0x2C)                        ⏱ 0x482DB9C0
   vtable                                                       ⏱ 0x01094810  (acteur joueur ✔)
bulle               = *(acteur + 0x264)                         ⏱ 0x488367C8 puis 0x00000000
   largeur / hauteur = acteur+0x288 / +0x28C                    ⏱ 186 × 23
```

### Trois faits établis en direct le 2026-08-08

1. 🔴 **Le joueur local n'est PAS dans la `std::list` d'acteurs.** Relevé sur un
   client où la sentinelle (`*(actorMgr+0x10)`) pointait sur elle-même — liste
   vide — alors que l'acteur propre existait bien à `actorMgr+0x2C` avec la
   vtable joueur `0x01094810`. Tout parcours qui n'itère que la liste ignore
   donc le joueur. Conséquence observée : sa bulle restait native quand toutes
   les autres étaient reprises.
   ⚠ **`EntityNames` a exactement le même angle mort** : son option « Ton propre
   nom » compare `actor == own_actor` à l'intérieur du parcours de liste, donc la
   condition n'est jamais vraie et la case ne fait rien.
2. **`acteur+0x264` est remis à 0 proprement à l'expiration.** Vérifié après les
   5 s : le champ vaut 0, l'acteur ne garde donc **aucun pointeur pendouillant**.
   Lire la fenêtre depuis le champ est sûr tant qu'on le fait dans la frame.
3. **Effacer la surface à la couleur-clé ne suffit PAS à faire disparaître la
   fenêtre.** `UIWindow_Render 0x00a1ce10` appelle `Paint` (vt+0x50) quand
   `fenêtre+0x58 == 1`, puis **blitte la surface dans tous les cas**. Un
   rectangle subsistait à l'écran. La seule voie propre est de détruire l'objet.

> 🔴 **Piège de lecture live.** Entre deux appels MCP espacés de quelques
> secondes, la bulle **expire**. Le second appel lit alors de la mémoire
> **libérée** dont la vtable vaut **`0x01022f68` = `UIRPData`** — la classe
> **racine** de la hiérarchie `UIWindow` (son destructeur, `0x007db280`, est le
> dernier de la chaîne MSVC et laisse sa vtable derrière lui). Une vtable
> `UIRPData` sur un widget de jeu **ne veut pas dire « mauvaise classe »**, elle
> veut dire **« objet détruit »**. Pour observer une bulle vivante il faut
> enchaîner les deux lectures dans la fenêtre de **5 s**.

---

## 10. Table d'adresses

| Adresse | Nom (IDB) | Rôle |
|---|---|---|
| `0x00ccbc60` | `GameMode_OnRecv_SelfChat_ZC008E` | ZC 0x008E = `clif_displaymessage` |
| `0x00cc8310` | `GameMode_OnRecv_EntityChat_ZC008D` | ZC_NOTIFY_CHAT |
| `0x00cce340` | `GameMode_OnRecv_NpcChat_ZC02C1` | ZC_NPC_CHAT (avec couleur) |
| `0x00cc8b50` | `GameMode_OnRecv_Font_ZC02EF` | ZC_FONT → police du locuteur |
| `0x00d473b0` | `Actor_OnMsg` | vtable acteur `+0x08` |
| `0x00d3c500` | `Actor_OnMsg_Base` | 2ᵉ étage (msg 3..0x89) |
| `0x00c4aea0` | `Actor_OnMsg_AppearanceEffects` | 3ᵉ étage (msg 0..0xa0) |
| **`0x00c4dace`** | *(cas msg 7)* | **création / mise à jour de la bulle** |
| `0x00c46680` | `CActorSprite_UpdateOverheadWidgets` | expiration 5 s + suivi par frame |
| `0x00c459e0` | `ActorAiClass_DestroyAttachedUI` | destruction sur despawn |
| `0x00818b90` | `UITransBalloonText_ctor` | 0xCC o, contour `0x5C5C5C` |
| `0x00817170` | `UIBalloonText_ctor` | base : lignes, police 12, blanc |
| `0x0081b590` | `UITransBalloonText_dtor` | vtable `+0x00` |
| `0x00830240` | `UIBalloonText_SetTextWrapped` | coupure à 35 caractères |
| `0x0081b8b0` | `UIBalloonText_AutoSizeToLines` | `w = max+12`, `h = 4 + Σ(l+4)` |
| `0x008263a0` | `UITransBalloonText_Paint` | **le cadre + le texte** |
| `0x0082b5f0` | `UITransBalloonText_SetSize` | vtable `+0x04` |
| `0x00822d90` | `UITransBalloonText_SetPos` | vtable `+0x10` |
| `0x00a1d760` | `UIWindow_DrawRoundedRectOutline` | contour à coins arrondis (12 rects) |
| `0x00a1cb30` | `UIWindow_ClearSurface` | ici avec `0xFFFF00FF` (transparent) |
| `0x00a2d240` | `UIWindowMgr_AddWindowToList` | `push_back` dans `mgr+0x17C` |
| `0x00a447d0` | `UIWindowMgr_QueueDestroyWindow` | destruction différée |
| `0x008d0e60` | `UI_ScaleYFrom480` | `n * hauteurEcran / 480` |
| `0x00836440` | `UIPcGage_ctor` | jauge de vie 0xB0 o |
| `0x008549a0` | `UIPcGage_Paint` | deux barres, **aucun texte** |
| `0x010293dc` | `UITransBalloonText::vftable` | — |
| `0x01029220` | `UIBalloonText::vftable` | — |
| `0x0102bca0` | `UIPcGage::vftable` | — |
| `0x01022f68` | `UIRPData::vftable` | **racine** — vue sur un objet **détruit** |
| `0x015fb2d0` | `g_Own_ChatFontId` | ma police de chat |
| `0x015e5b48` | `g_OverheadWidgetYScale` | `81 * hauteurEcran / 480` (calculé 1 fois) |
| `0x0131f764` | *(alias `kNoPathFlag`)* | ≠ 0 ⇒ **aucune bulle** |
| `0x0131ed60` | `CTagMgr` (singleton) | transformation de balises du texte |

---

## 11. Le remplacement Bourgeon — `src/features/overlays/chat_balloon.{h,cc}`

**Pourquoi il n'est pas optionnel.** Le chemin natif n'appelle
`ChatText_TransformTagLinks` que sous `if (g_pNewChatWnd)`. Dès que la chatbox
ImGui est active, la fenêtre native est détruite, ce pointeur vaut **0**
(vérifié en live), et **plus aucune balise n'est résolue** — pas seulement nos
`<MOBL>` / `<ITMR>` / `<CRAF>` / `<SETL>`, mais **`<ITEML>` lui-même**, pourtant une balise
du client. C'est pour ça que l'activation suit `chatwnd_imgui` et n'a pas de
réglage propre : en chatbox native, le client sait de nouveau résoudre ses liens
et sa bulle est correcte.

**Deux détours de fonction entière**, aucun patch en milieu de fonction :

| Fonction | Rôle |
|---|---|
| `UIBalloonText_SetTextWrapped 0x00830240` | **observateur** : seul point commun aux DEUX créateurs (acteur et unité de sol), texte encore brut. On copie, on relaie. |
| `UITransBalloonText_Paint 0x008263a0` | **silence** de la frame de naissance, le temps que la destruction parte. |

**La fenêtre native est DÉTRUITE dès que son texte est adopté**, via
`UIWindowMgr::QueueDestroyWindow 0x00a447d0` — la fonction que le natif
s'applique à lui-même, donc aucune étape de démontage sautée.

🔴 **L'adoption ET la destruction se font dans `Bourgeon::OnGameFrame`, pas dans
`OnRenderUI`** — et pour DEUX raisons, dont la seconde est la moins évidente :

1. `QueueDestroyWindow` est un appel natif, interdit pendant une frame ImGui
   (freeze muet) ;
2. surtout, **`OnRenderUI` arrive APRÈS que le jeu a dessiné**. Détruire depuis
   là laissait la fenêtre native visible une frame entière : un rectangle qui
   clignotait derrière la bulle à chaque réplique. Depuis `OnTick` (bridé à
   ~100 ms) c'était plusieurs frames. `OnGameFrame` est le battement par frame
   qui précède le dessin — c'est le seul endroit qui satisfasse les deux
   contraintes.

Corollaire : la présence d'une entité ne peut plus se déduire de « elle porte une
fenêtre », puisqu'on la lui retire.

**Le rendu part des fragments du chat, pas d'un texte plat.** `ChatWindow::Run`
et `Line` sont publics, `ParseWireLine` les produit, et `ChatBalloon::LayoutRuns`
calque la boucle de `ChatWindow::DrawLines` : couleur par fragment, emotes du jeu
(`ro::emote::Draw`, qui accepte n'importe quel `ImDrawList`), icônes d'objet
(`ro::ItemIcon`), `kLinkCol` pour les liens. Passer par `Line::plain` ne marche
pas : ce n'est que la concaténation des textes, donc une emote y reste écrite
« :nom: » et une icône disparaît.

Rendu sur `ImGui::GetBackgroundDrawList()` — derrière toutes les fenêtres ImGui.

**Ce que le remplacement corrige** : durée proportionnelle à la longueur (le
natif fige 5 s pour tout le monde), fondu de fin, anti-chevauchement (le natif
n'en a aucun), coupure par largeur réelle au lieu de 35 caractères, fond lisible,
et le facteur vertical recalculé chaque frame au lieu du magic static figé.

Les emotes **Discord** s'affichent quand elles sont **déjà en cache** : on lit
`imgprev::Get`, on n'appelle jamais `Request`. Une bulle vit cinq secondes, elle
n'a pas le temps d'attendre un téléchargement, et un overlay n'a pas à ouvrir de
connexion. En pratique le chat les a déjà demandées en affichant la même ligne ;
sinon le fragment retombe sur son `:nom:`.

⛔ **Le clic sur les liens est ÉCARTÉ — décision, pas oubli.** Une zone cliquable
qui suit une entité en mouvement volerait des clics au sol en plein jeu, pour un
gain marginal puisque la même ligne est cliquable dans la chatbox, à un endroit
stable. Ne pas le réimplémenter en croyant combler un manque.

### Points d'attention (valables pour toute reprise)

1. **Le slot est l'acteur, pas le gestionnaire.** Pour supprimer/remplacer la
   bulle native il faut agir sur `acteur+0x264` **et** sur `acteur+0x248` : le
   simple fait de détruire la fenêtre sans remettre le slot à 0 laisserait un
   pointeur pendouillant que la passe par frame déréférencerait.
2. **La durée de vie est en dur (5 000 ms)** dans
   `CActorSprite_UpdateOverheadWidgets` — pas d'option, pas de réglage lua.
3. **Le texte affiché est déjà transformé** (balises/liens `CTagMgr` + chat) :
   un remplaçant qui repartirait de la charge utile brute du paquet
   n'afficherait **pas** la même chose que le natif.
4. **Trois émetteurs, pas un seul.** Remplacer uniquement le chemin `0x008D`
   laisserait passer la bulle de `clif_displaymessage` (`0x008E`) et celle des
   PNJ (`0x02C1`) — cf. `feedback_replace_native_fill_gaps`.
5. **La couleur n'existe que sur `ZC_NPC_CHAT`** ; les deux autres chemins sont
   blancs, et la « couleur » qu'on croit lire en `+0x9A` est en fait un **index
   de police**.
6. Le liseré `0x5C5C5C` et la couleur-clé `0xFFFF00FF` sont en dur : reproduire
   l'apparence en ImGui demande un rectangle **non rempli** aux coins arrondis,
   pas un fond semi-transparent.
