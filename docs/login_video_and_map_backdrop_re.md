# Vidéo en fond d'écran de login, et map « visitée » derrière le formulaire

Client `20250716`, base `0x400000`, sans ASLR (adresse IDA == adresse live).
Étude menée le **2026-08-25**, **entièrement statique** (IDB IDA + inspection du
dossier client). Rien n'a été mesuré sur un client en cours d'exécution : chaque
affirmation qui l'exigerait est signalée comme telle au § 6.

Deux demandes distinctes, qui se rejoignent à l'arrivée :

1. **jouer une vidéo en fond de l'écran de login** ;
2. **charger une map et y promener la caméra**, comme si un joueur s'y baladait.

---

## 0. Verdict en une page

| Question | Réponse |
|---|---|
| Le client sait-il décoder une vidéo ? | **Oui, nativement.** Il est lié **statiquement** à `binkw32.dll` (RAD Bink **1.100r**, présent dans le dossier client). Le client ne démarre pas sans elle. |
| Existe-t-il un lecteur vidéo plein écran ? | **Oui** : `UIVideoWnd`, **fenêtre 304**, complète (letterbox, bouton SKIP, Échap, coupe la BGM, se ferme en fin de vidéo). |
| Est-elle utilisée par le client ? | **Non — fenêtre morte.** Aucun `MakeWindow(304)` littéral hors de la fabrique elle-même (§ 6.1 pour la réserve). Elle est donc *disponible*, comme les autres natives mortes du projet. |
| Peut-on jouer une vidéo au login ? | **Très probablement oui**, et sans écrire de décodeur : toute la chaîne dont elle dépend (fabrique de fenêtres, file de rendu 2D, fabrique de textures, son Bink) est **initialisée au boot**, avant le login. À valider en live (§ 6). |
| Peut-on charger une vraie map derrière le login ? | **Pas en restant au login.** Le chargeur de monde interroge le *mode de jeu actif* à deux endroits ; sous `CLoginMode` il lirait un pointeur arbitraire. Il faut **basculer réellement en `CGameMode`** — ce qui est un `CModeMgr_RequestSwitch(1, "prontera.rsw")`, mais implique de neutraliser tout ce qui attend un serveur. |
| Y a-t-il un précédent natif de « CGameMode sans serveur » ? | **Oui : le mode Replay.** Lancé avec `Replay` en ligne de commande, le client **ne monte aucune connexion** et se nourrit d'un fichier de paquets. Des dizaines de fenêtres respectent déjà le drapeau `g_ReplayActive`. |
| Recommandation | **Faire du volet 2 un cas du volet 1** : produire *hors ligne* un survol de map (caméra pilotée par `ro::camera`, capture par le pipeline de `ZoneRecorder`), l'encoder en `.bik`, et le jouer en fond de login par la voie native. On obtient l'effet demandé sans faire tourner le moteur 3D au login. |

🔴 **Le piège d'entrée** : la chaîne `login.rsw` existe dans le binaire, et
`WinMain` la passe au gestionnaire de modes. On croit tenir une fonctionnalité
native de « map au login ». **C'est un vestige** : `CLoginMode` n'en fait
strictement rien (§ 3.2).

---

# Volet A — la vidéo

## A.1 Le décodeur est dans le client, en dur

`binkw32.dll` n'est pas chargée à la demande : elle a un **descripteur d'import
statique** (`__IMPORT_DESCRIPTOR_binkw32` @ `0x017170dc`). Le client ne se lance
pas sans elle — et elle est bien là, dans `E:\Nouveau dossier\Moonlight-Destiny\` :

```
binkw32.dll   184 Ko   FileVersion 1.100r   RAD Game Tools, Inc.
```

**Bink 1**, donc : les fichiers doivent être des **`.bik`** (encodés avec RAD
Video Tools), pas des `.bk2` (Bink 2, qui exige `bink2w32.dll` — absente).

Les 13 entrées importées, et leur adresse de trampoline :

| Import | IAT | Rôle |
|---|---|---|
| `BinkOpen` | `0xfc1be4` | ouvre un `.bik` (chemin **ou** mémoire) |
| `BinkClose` | `0xfc1bf0` | ferme |
| `BinkWait` | `0xfc1c10` | « trop tôt pour la frame suivante ? » |
| `BinkDoFrame` | `0xfc1c14` | décode la frame courante |
| `BinkNextFrame` | `0xfc1c0c` | avance d'une frame |
| `BinkShouldSkip` | `0xfc1be8` | rattrapage si on est en retard |
| `BinkCopyToBuffer` | `0xfc1c08` | recopie la frame décodée dans une surface |
| `BinkGoto` | `0xfc1bfc` | saut à une frame |
| `BinkPause` | `0xfc1c00` | pause / reprise |
| `BinkSetVolume` | `0xfc1c04` | volume de la piste |
| `BinkOpenDirectSound` | `0xfc1bf4` | fournisseur audio |
| `BinkSetSoundSystem` | `0xfc1bf8` | branche le fournisseur |
| `BinkGetError` | `0xfc1bec` | dernier message d'erreur |

**L'audio Bink est armé dès le boot** : `WinMain` (`0x00db9060`) fait, à
`0x00db917a`, `BinkSetSoundSystem(BinkOpenDirectSound, 0)` — bien avant l'écran
de login. Rien à initialiser de notre côté.

### Le layout de la structure `BINK`, tel que le client le lit

Déduit des accès du client (et conforme au `bink.h` public) :

| Offset | Champ | Où le client s'en sert |
|---|---|---|
| `+0x00` | `Width` | `UIVideoWnd_OnRender` (ratio d'affichage) |
| `+0x04` | `Height` | idem |
| `+0x08` | `Frames` | `CBinkVideo_Advance` : fin de vidéo |
| `+0x0c` | `FrameNum` | idem — `FrameNum == Frames` ⇒ terminé |
| `+0xec` | état de pause | `UIVideoWnd_OnMsg` msg 151 (bascule) |

## A.2 `CBinkVideo` — l'enveloppe du client (5 méthodes)

Objet de **20 octets** (`operator new(0x14)`), construit par
`CBinkVideo_ctor` **`0x00775500`** :

| Offset | Contenu |
|---|---|
| `+0x00` | `HBINK` (le handle Bink) |
| `+0x04` | état : `0` vide, `1` en lecture, `2` **terminé** |
| `+0x08` | mode de fin : `0` = s'arrêter, `1` = **boucler** |
| `+0x10` | `CFile*` (flux de lecture, voie ressource) |

### `CBinkVideo_Open` **`0x00775670`** — `__thiscall(this, path, loopMode, useResStream)`

```c
if (useResStream == 1) {                       // ── voie RESSOURCE
    file = new CFile(0x2c);                    // vtable CFile 0x00fd3074
    ResFileStream_Open(file, path, 0, 1, 0);   // 0x00573750 — disque PUIS grf
    BinkOpen(*(file + 8), 0x04104000);         // BINKFROMMEMORY|BINKALPHA|BINKSNDTRACK
} else {                                       // ── voie CHEMIN DIRECT
    BinkOpen(path, 0x00104000);                //   BINKALPHA|BINKSNDTRACK
}
this[0] = handle;  this[2] = loopMode;  this[1] = 1;
```

Deux enseignements qui comptent :

- **le client sait lire un `.bik` depuis le GRF** (`useResStream = 1` passe par
  `ResFileStream_Open`, le même chargeur disque-d'abord-puis-GRF que le reste
  des ressources) **ou depuis un simple chemin disque** (`useResStream = 0`) ;
- **l'alpha est demandé à l'ouverture** (`BINKALPHA`). Une vidéo Bink encodée
  avec canal alpha est donc supportée de bout en bout — utile pour une
  incrustation qui ne serait pas un rectangle plein.

### `CBinkVideo_Advance` **`0x007757c0`** — un tick, à appeler par frame

```c
if (state != 1) return 0;
if (BinkWait(h)) return 0;                    // pas encore l'heure : rien à faire
BinkDoFrame(h);
while (BinkShouldSkip(h)) { BinkNextFrame(h); BinkDoFrame(h); }   // rattrapage
if (h->FrameNum == h->Frames) {               // fin atteinte
    if (loopMode == 0) { Stop(); state = 2; return 0; }
    if (loopMode == 1) h->FrameNum = 0;       // ← LA BOUCLE EST NATIVE
}
BinkNextFrame(h);
return 1;
```

🔴 **La lecture en boucle ne demande aucun code de notre part** : c'est le mode
`1`. Pour un fond de login, c'est exactement ce qu'il faut — et c'est d'ailleurs
le mode que le client utilise pour les vidéos de map (§ A.3).

### `CBinkVideo_BlitToTexture` **`0x007755d0`** — `__thiscall(this, texture)`

```c
texture->vt[0x10](&lockInfo);                  // Lock : renvoie ptr, pitch, w, h
if (!lockInfo.ok) return 0;
BinkCopyToBuffer(h, ptr, pitch, height, 0, 0, 5);   // 5 = BINKSURFACE32A (BGRA)
texture->vt[0x14]();                           // Unlock
SetRect(&rc, 0, 0, w, h);
texture->vt[0x30](&rc);                        // marque la zone à réenvoyer au GPU
```

La cible est donc **n'importe quelle texture verrouillable** du moteur —
c'est ce qui rend la brique réutilisable ailleurs qu'au login.

### Les deux terminaisons

`CBinkVideo_Close` **`0x00775520`** (ferme et libère le flux) et
`CBinkVideo_Stop` **`0x00775590`**.

## A.3 Le gestionnaire de vidéos du monde — et le déclencheur `.bik`

`VideoMgr_GetOrCreate` **`0x00775f20`** tient une `std::list` d'entrées de
**40 octets** indexées par **nom de fichier** :

| Offset | Champ |
|---|---|
| `+0x00` | `std::string` — le nom (clé de recherche) |
| `+0x18` | `CBinkVideo*` |
| `+0x1c` | `float` rayon audible (défaut **150.0**) |
| `+0x20` | liste des positions monde où la vidéo « sonne » |
| `+0x24` | drapeau audio spatialisé |

Le chemin construit est **`video\` + nom**, passé à `CBinkVideo_Open` avec
`useResStream = 1` — donc **`data\video\<nom>.bik` sur disque, ou `video\<nom>.bik`
dans un GRF**.

### 🔴 Le déclencheur : une texture dont le nom contient `.bik`

`SpriteRes_LoadFromFile` **`0x005688f0`** — le chargeur de texture du monde —
commence par ceci :

```c
if (!strstr(path, ".bik")) {
    … chargement d'image normal (bmp / tga / jpg) …
} else {
    if (!GameMode_GetActive()) → texture vide 1×1
    if (!*(mode + 0xcc))        → texture vide 1×1     // le CWorld
    if (!*(*(mode + 0xcc) + 0xe0)) → texture vide 1×1  // son VideoMgr
    entry = VideoMgr_GetOrCreate(videoMgr, path, /*loop=*/1);
    return SpriteTexFactory_NewEmptySprite(w = bink->Width, h = bink->Height, fmt = 4);
}
```

Autrement dit : **toute texture de map nommée `…​.bik` est automatiquement jouée
comme une vidéo Bink, en boucle.** C'est le mécanisme des écrans/panneaux vidéo
des maps modernes. Il est **entièrement conditionné à l'existence d'un
`CGameMode` et de son `CWorld`** : au login, les trois gardes échouent et la
texture sort en 1×1 vide. **Cette voie-là est donc inutilisable pour un fond de
login** — mais elle est *parfaite* pour une vidéo **dans le jeu**, sans une ligne
de code : il suffit qu'une map référence une texture `.bik`.

`VideoMgr_UpdateAllAndSpatialVolume` **`0x00776320`**, appelée depuis
`EffectList_UpdateTick`, fait avancer chaque vidéo, la blitte dans la texture
retrouvée par nom (`SpriteRes_GetOrLoadByName`), puis **atténue le volume selon
la distance** entre les positions déclarées et le joueur
(`*(CWorld + 0x2c)` = l'acteur du joueur) :

```c
if (radius <= dist) BinkSetVolume(h, 0, 0);
else BinkSetVolume(h, 0, (int)((masterVolume / radius) * (radius - dist)) << 8);
```

## A.4 `UIVideoWnd` — la fenêtre 304, complète et inemployée

RTTI `.?AVUIVideoWnd@@` `0x0124ac6c` → **vtable `0x0104d19c`**.
Créée par le case `0xa41280` de `UIWindowMgr_MakeWindow` **`0x00a39340`**,
sous l'**identifiant de fenêtre 304** (`0x130`).

### Champs

| Offset | Contenu |
|---|---|
| `+0x7c` (`this[31]`) | `CBinkVideo*`, alloué dans le constructeur |
| `+0x80` (`this[32]`) | la texture cible du blit |
| `+0x84` (`this[33]`) | volume BGM mémorisé (`-1` = non touché) |

### Les cinq entrées de `UIVideoWnd_OnMsg` **`0x00a56a20`** (vtable `+0x94`)

| msg | Effet |
|---|---|
| **150** (`0x96`) | **Charger une vidéo** : `CBinkVideo_Open(bink, path, loop=0, useRes=0)` — chemin **disque direct** — puis libère l'ancienne texture et en alloue une neuve (`SpriteTexFactory_NewTexture` `0x00568620`). |
| **151** (`0x97`) | Bascule **pause / reprise**. |
| **159** (`0x9f`) | `BinkSetVolume(h, 0, vol << 8)` ; si le 4ᵉ paramètre est nul, **mémorise puis coupe la BGM** (`Sound_SetBgmVolume` `0x00600be0`). |
| **5** | Demande de fermeture → boîte modale (msgstring **`0xC46`**) ; réponse `187` ⇒ ferme la fenêtre 304. |
| **6** avec commande **521** | Le bouton **SKIP** ⇒ ferme. |
| **0** avec touche **27** | **Échap** ⇒ renvoie vers le msg 5. |

### Rendu et cycle de vie

`UIVideoWnd_OnRender` **`0x00a56950`** (vtable `+0x50`) :

```c
Render2D_FillRect(w, h, 0xFF000000);                       // fond noir plein cadre
vh = min(videoH / videoW * w, h);                          // ratio conservé
Render2D_SubmitTexturedQuad(0, (h - vh) / 2, texture, -1, w, vh);   // ← letterbox
```

`UIVideoWnd_OnUpdate` **`0x00a569d0`** (vtable `+0x4c`) :

```c
if (CBinkVideo_Advance(bink) == 1 && CBinkVideo_BlitToTexture(bink, tex) == 1)
    this->vt[0x98]();                                      // invalide / redessine
if (bink->state == 2)                                      // vidéo terminée
    UIWindowMgr_SaveRectAndCloseWindow(g_UIWindowMgr, 304); // ← auto-fermeture
```

`UIVideoWnd_OnCreate` **`0x00a566e0`** (vtable `+0x3c`) ajoute un bouton
`127 × 43` (commande **521**) ancré en bas à droite ; son rendu
(`UIVideoSkipButton_OnDraw` `0x00a567b0`) écrit littéralement **`SKIP>`**, en
blanc, centré.

`UIVideoWnd_dtor` **`0x00a56610`** **restaure le volume BGM** s'il avait été
mémorisé. Rien ne fuit.

### 🔴 Ce que cela implique pour un fond de login

Le comportement natif est celui d'une **cinématique**, pas d'un fond :
plein écran, noir autour, `loop = 0` (msg 150 ouvre sans boucle), **auto-fermeture
en fin**, et bouton SKIP par-dessus. Pour un **fond de login** il faudra donc,
selon la voie retenue :

- soit accepter la fenêtre telle quelle et **la faire réémettre** (rouvrir en fin
  de vidéo), ou l'ouvrir nous-mêmes avec `loop = 1` en appelant
  `CBinkVideo_Open` directement plutôt que par le msg 150 ;
- soit ne prendre **que la brique `CBinkVideo`** (Open/Advance/Blit) et dessiner
  nous-mêmes le quad — en ImGui ou via `Render2D_SubmitTexturedQuad`
  **`0x007dbb20`**, qui soumet un quad texturé dans la file de rendu 2D et
  fonctionne dans tous les modes.

Et il faudra régler la **profondeur** : le formulaire natif doit rester
au-dessus. Le projet a déjà `ui/window_zorder.cc` pour ce genre d'arbitrage ;
la fenêtre 304 se place, elle, dans la liste des fenêtres du gestionnaire.

## A.5 Le lecteur d'intro `openning.bik`

Une seconde implémentation, indépendante de `UIVideoWnd`, vit dans la région
**`0x00a86a00 – 0x00a87000`** (que IDA laisse non analysée ; les fonctions y
sont lisibles en désassemblage mais refusent d'être définies) :

- **`0x00a86c6f`** — `Open()` : `BinkSetSoundSystem(BinkOpenDirectSound, 0)` puis
  `BinkOpen("openning.bik", 0)`. Chemin **relatif au dossier du client**, sans
  `video\`, sans GRF.
- **`0x00a86ca0`** — la lecture **bloquante** plein écran : elle lit d'abord la
  valeur de registre **`BINKMODE`** sous `HKLM\<clé du client>`
  (`RegOpenKeyExA` + `RegQueryValueExA` sur la chaîne `"BINKMODE"` `0x0104e488`),
  puis pompe elle-même la file de messages Windows (`PeekMessage` /
  `TranslateMessage` / `DispatchMessage`) jusqu'à la fin de la vidéo.
- **`0x00a86bef`** — un gestionnaire de touche : la touche `0x67` déclenche
  `BinkGoto(h, 1500, 1)`.
- **`0x00a86c4f`** — `Close()`.

Aucun `openning.bik` n'existe dans le dossier client. Ce lecteur est donc **inerte
en pratique**, et de toute façon inadapté : il **bloque** le fil principal, et il
s'exécute avant que quoi que ce soit d'autre ne soit à l'écran.

Il reste utile comme **preuve de vie du décodeur** : si l'on doute que Bink
fonctionne dans ce build, déposer un `openning.bik` et poser la clé `BINKMODE`
est le test le moins invasif qui soit.

## A.6 Pourquoi la voie native devrait marcher au login

Tout ce dont `UIVideoWnd` dépend est **initialisé au boot**, avant l'entrée en
mode login — c'est vérifiable dans `WinMain` et dans `CLoginMode_OnUpdate` :

| Dépendance | Où elle est prête | Preuve |
|---|---|---|
| Fabrique de fenêtres `g_UIWindowMgr` `0x0131f4e8` | boot | `UIConfig_LoadWindowRectsFromRegistry` appelée dans `WinMain` avant `CModeMgr_Run` |
| File de rendu `g_SceneRenderQueue` `0x012515f8` | boot | `mov g_SceneRenderQueue, eax` @ `0x00db9d34` |
| Rendu des fenêtres au login | oui | `UIWindowMgr_RenderWindows` est appelée dans `CLoginMode_OnUpdate` `0x00d272e0` |
| Fabrique de textures | boot | `SpriteTexFactory_GetSingleton` utilisée dans `CLoginMode_OnUpdate` |
| Son Bink | boot | `BinkSetSoundSystem` @ `0x00db9180` |
| Frame ImGui vivante au login | oui | déjà exploité par `LoginParade` via `OnRenderLoginUI` |

Aucune de ces briques ne consulte le mode actif. À l'inverse, la voie « texture
`.bik` » (§ A.3) le fait explicitement — c'est elle, et elle seule, qui est
barrée au login.

## A.7 Repli : décoder nous-mêmes

Si la fenêtre 304 s'avérait inutilisable au login (voir § 6), le repli ne coûte
pas cher : `CBinkVideo` n'est qu'un enrobage de treize appels importés. On peut
appeler `BinkOpen` / `BinkDoFrame` / `BinkCopyToBuffer` directement (par
`GetProcAddress` sur `binkw32.dll`, déjà chargée), écrire dans une texture D3D9
dynamique et la dessiner en ImGui — le projet sait déjà faire cela
(`ui/game_texture.cc`, cf. `project_imgui_game_textures`).

⚠ Sous le proxy **DX7**, le chemin ImGui n'existe pas (`g_imgui_dx7_active`) :
la voie native, qui passe par la file de rendu du client, y reste la seule.

---

# Volet B — une map, et la caméra qui s'y promène

## B.1 L'architecture réelle des modes

`WinMain` **`0x00db9060`** se termine par une seule chose :

```asm
push offset aLoginRsw       ; "login.rsw"
push 0                      ; type de mode : 0 = login
mov  ecx, offset 0x1213338  ; g_ModeMgr
call CModeMgr_Run           ; 0x00a756e0  ← toute la vie du jeu est là-dedans
```

`CModeMgr_Run` **`0x00a756e0`** est un boucleur de quinze lignes :

```c
mgr[22] = mgr[23] = modeType;   strcpy(mgr+8, map);   strcpy(mgr+0x30, map);
while (mgr[0]) {                                  // 0 ⇒ quitter le jeu
    if (g_QuitFlag /* 0x01602ad8 */) return;
    mgr[22] = mgr[23];  strcpy(mgr+8, mgr+0x30);  // on adopte la demande
    mode = (mgr[22] == 0) ? new CLoginMode(0x6F7C)  // ctor 0x00d1e260
                          : new CGameMode (0x670);  // ctor 0x00c63570
    mgr[1] = mode;
    mode->vt[0x08](mapName);   // ← EnterWorld(map)
    mode->vt[0x04]();          // ← Run : la boucle interne du mode
    mode->vt[0x0c]();          // ← OnExit
    mode->vt[0x00](1);         // ← destruction
}
```

### Carte du `CModeMgr` (`g_ModeMgr` = `0x01213338`)

| Offset | Contenu |
|---|---|
| `+0x00` | drapeau « continuer » — `0` ⇒ on quitte le jeu |
| `+0x04` | **le mode actif** (`CLoginMode*` ou `CGameMode*`) |
| `+0x08` | nom de map **courant** (`char[40]`) |
| `+0x30` | nom de map **demandé** |
| `+0x58` | type de mode **courant** (`0` login, `1` jeu) |
| `+0x5c` | type de mode **demandé** |

Cela recoupe exactement `GameMode_GetActive` **`0x00a75340`** :
`return *(mgr+0x58) == 1 ? *(mgr+4) : 0;` — d'où le fait, déjà connu du projet,
qu'elle renvoie `0` hors du jeu.

### Les deux tables virtuelles, alignées

| Slot | `CLoginMode` (`0x010932f0`) | `CGameMode` (`0x010904b8`) |
|---|---|---|
| `+0x00` dtor | `0x00d1e900` | `0x00c65010` |
| `+0x04` **Run** | `0x00d27190` `CLoginMode_Run_StateLatch` | `0x00c74850` |
| `+0x08` **EnterWorld(map)** | `0x00d26a70` `CLoginMode_EnterMode` | **`0x00c733d0` `CGameMode_EnterWorld`** |
| `+0x0c` OnExit | `0x00d26850` | `0x00c73130` |
| `+0x10` **OnUpdate / frame** | `0x00d272e0` | `0x00c74a80` `GameMode_InGame_ProcessFrame` |
| `+0x18` **SendMsg** | `0x00d2a130` | `0x00c86740` |

### Demander une bascule : une seule fonction

`CModeMgr_RequestSwitch` **`0x00a764e0`** — `__thiscall(mgr, modeType, mapName)` :

```c
*(mgr[1] + 20) = 0;          // mode->m_running = 0  ⇒ sort de sa boucle Run
strcpy(mgr + 0x30, mapName); // map demandée
mgr[23] = modeType;          // mode demandé
```

C'est **tout** ce que fait le client à l'entrée en jeu : les handlers
`ZC_ACCEPT_ENTER` (`0x00d2bc20` et `0x00d2bf90`) construisent `"<map>.rsw"` avec
`sprintf`, appellent `CModeMgr_RequestSwitch(1, buffer)`, puis ouvrent la fenêtre
de chargement (`MakeWindow(6)`).

> Le hook `ModeMgr::SwitchHook(ModeType, map_name)` de Bourgeon
> (`src/ragnarok/mode_mgr.h`) est posé sur cette fonction. Le levier existe donc
> déjà côté projet.

## B.2 🔴 `login.rsw` est un vestige — ne pas s'y fier

`CLoginMode_EnterMode` **`0x00d26a70`** reçoit bien `"login.rsw"`… et **ne
l'utilise jamais**. Elle initialise les états de la machine à états de login,
précharge les sprites communs (`Preload_CommonSprites` `0x00d745b0`), et joue
**`bgm\01.mp3`** si l'option BGM (`GameSettings_GetFlag(7)`) est active. Aucun
chargement de terrain, aucune caméra, aucun monde.

La chaîne `login.rsw` `0x010916d0` existe (et le fichier existe dans les GRF de
kRO), mais **aucun code de ce build ne la charge**. C'est un reliquat d'une
époque où l'écran de login était une scène 3D.

## B.3 🔴 Le pipeline 3D tourne DÉJÀ pendant le login

C'est le fait le plus contre-intuitif de cette étude, et le plus utile.

La file de rendu `g_SceneRenderQueue` (`0x012515f8`) expose, dans sa table
virtuelle (`0x00fd6b08`) :

| Slot | Fonction | Appelée au login ? | En jeu ? |
|---|---|---|---|
| `+0x14` | `Renderer_ClearBackbuffer` `0x00551080` | **oui** (`Clear(1)` = noir) | non |
| `+0x18` | `Renderer_ClearWithSkyColor` `0x005510b0` | non | **oui** (couleur de ciel de la map) |
| `+0x1c` | **`Renderer_RenderFrame` `0x00551c10`** | **OUI** | **OUI** |
| `+0x20` | présentation `0x00551d30` | **oui** | **oui** |

Et `Renderer_RenderFrame` n'est rien d'autre que :

```c
BeginScene();
World_RenderScene(this);      // 0x00552fa0
EndScene();
```

`World_RenderScene` **ne dessine que des seaux de primitives** déjà soumises
(`this+348/352` terrain, `+432/436`, `+456/460` …). Au login ces seaux sont
vides : le pipeline tourne à vide, et c'est pour cela qu'on ne voit rien.

⇒ **Il ne manque pas un chemin de rendu au login. Il manque quelqu'un pour
remplir les seaux.**

## B.4 Qui remplit les seaux : `CScene` et `CWorld`

En jeu, le remplissage vient d'un unique appel dans
`GameMode_InGame_ProcessFrame` :

```c
(*(scene_vt + 0x0c))(scene);   // scene = *(CGameMode + 0xd0)
```

soit **`CScene_RenderCellsAndCursor` `0x00a7b0a0`** : elle calcule le frustum
(`Camera_ComputeFrustumCorners` `0x00a79f90`), parcourt les nœuds visibles, les
fait se soumettre, dessine la traînée de navigation, le curseur au sol, les
acteurs et les effets attachés.

L'objet à `CGameMode + 0xd0` (`0x1b8` octets, vtable `0x0104dee4`, construit par
`CCamera_ctor` `0x00a79b30` **depuis `CGameMode_EnterWorld` et nulle part
ailleurs**) est donc **la scène**, dont le « rig caméra » documenté dans
[camera_and_afk_re.md](camera_and_afk_re.md) n'est qu'une partie :

| Offset | Contenu |
|---|---|
| `+0x20..0x28` | point visé (recopié de la position du joueur) |
| `+0x2c/0x30/0x34` | pose **courante** — pitch / yaw / distance |
| `+0x44/0x48/0x4c` | pose **cible** — les vrais leviers |
| `+0x98` | contexte de rendu passé à tous les nœuds |
| `+0xf8`, `+0x100` | listes de nœuds visibles (reconstruites par frame) |
| **`+0x1b0`** | **le `CWorld`** |

Et sa vtable : `+0x0c` = rendu, `+0x10` = construction de la vue (le lissage).

### 🔴 Précision de nommage : `CGameMode + 0xcc` **est le `CWorld`**

Le projet nomme ce champ `gamescene::kGmActorMgr` (« gestionnaire d'acteurs »).
Les offsets qui en dépendent sont **justes**, mais le nom raconte une histoire
incomplète — `SpriteRes_LoadFromFile` traite le même pointeur comme le monde, et
`CWorld_ctor` **`0x00a682d0`** (vtable `0x0104d598`) le confirme :

| Offset | Contenu | Nom actuel côté projet |
|---|---|---|
| `+0x08`, `+0x10`, `+0x18`, `+0x20` | quatre listes de nœuds | `kAmListHead = 0x10` (l'une d'elles) |
| `+0x28` | le terrain (`0xAC` octets, ctor `0x00a5f720`) | — |
| `+0x2c` | **l'acteur du joueur** | `kAmOwnPlayer = 0x2c` ✔ |
| `+0x30` | le `.gat` (attributs de cellules) | — |
| `+0x58` | quadtree | — |
| `+0xe0` | **le `VideoMgr` de la map** | — |

Rien à corriger dans le code ; c'est une **carte à compléter** le jour où l'on
touchera à ce champ.

## B.5 `CWorld_Load` — et les deux clous qui ferment la porte

`CWorld_Load` **`0x00a6aff0`**, appelée par `CGameMode_EnterWorld` :

```c
mapName = world[1]->vt[0x18](9, 0,0,0,0);        // ← ① le MODE ACTIF fournit le nom
rsw = UITextureMgr_Load(UITextureMgr_Get(), mapName);
    ├ échec → "world에 NULL값이 들어왔습니다"
world[56] = new VideoMgr();                       // le gestionnaire de vidéos de la map
g_light[0x0159b1a4 .. 0x0159b1c4] = rsw[101..109];// ambiante / diffuse / direction
renderQueue->vt[0x08](&light…);                   // pose l'éclairage global
gat = UITextureMgr_Load(rsw.attrFile);            // ← .gat
    ├ absent → "attr 파일이 rsw 내에 지정되어…"
gnd = UITextureMgr_Load(rsw.gndFile);             // ← .gnd (terrain)
    ├ absent → "gnd …" / échec → "ground"
world[10] = new Terrain(); terrain->Build(gnd, lights);
pour chaque modèle du .rsw :
    UIWindowMgr_ChatAction(mgr, 4 /*UIM_LOADINGPERCENT*/, pct)  // barre de progression
    … + rendu partiel de l'UI à chaque palier ⇒ le chargement est BLOQUANT
    dist = distance au joueur via Camera_ProjectTileToScreen(GameMode_GetActive(), …)
                                                  // ← ② le MODE ACTIF, encore
    si dist > 200 → différé ; sinon charge le .rsm et crée son nœud
QuadTree_Rebuild(world);
CreateThread(… 0x00a6a840 …)                      // le reste des modèles, en fond
world[11] = new OwnPlayerActor(0x540);            // ← l'acteur du joueur est créé ICI
```

Trois choses à retenir :

1. **Tout passe par `UITextureMgr_Load` `0x00a8d4a0`** — la même porte que
   `LoginParade` emploie déjà pour ses `.spr`/`.act`, et dont on sait donc
   qu'**elle fonctionne au login**. Le `.rsw`, le `.gat`, le `.gnd` et les `.rsm`
   ne sont, pour le chargeur de ressources, que des fichiers de plus.
2. 🔴 **Deux dépendances au mode actif** (① et ②) condamnent la voie « charger un
   monde en restant sous `CLoginMode` » : ① `SendMsg(9)` sur un `CLoginMode` ne
   rend pas un nom de map, et ② `Camera_ProjectTileToScreen` déréférencerait
   `CLoginMode + 0xd0`, qui n'est pas une scène mais un champ quelconque d'une
   structure de `0x6F7C` octets. Corruption ou plantage silencieux.
3. Le chargement **crée l'acteur du joueur** et **bloque** le fil principal
   pendant plusieurs centaines de millisecondes (à quelques secondes sur une map
   dense) — pendant lesquelles il rend lui-même une barre de progression.

## B.6 Trois voies, et leur verdict

### V1 — basculer réellement en `CGameMode`, hors ligne

Un appel : `CModeMgr_RequestSwitch(g_ModeMgr, 1, "prontera.rsw")`. Le monde se
charge complètement — terrain, modèles, eau, ciel, brouillard, lumière, effets —
et `ro::camera` pilote la caméra comme en jeu.

Ce qu'il faut neutraliser, en revanche :

- `GameMode_InGame_ProcessFrame` ouvre chaque frame par
  `RecvLoop_DispatchPackets` `0x00c9df00`, qui teste `CRagConnection_IsAlive`
  `0x00c14720` — sans connexion, le client conclut à une déconnexion ;
- `CGameMode_EnterWorld` **émet des paquets** (`CRagConnection_SendPacket` figure
  parmi ses appelées) et lit l'identité du personnage (`Own_GetCharName`) ;
- l'écran de login disparaît : il faudrait redessiner le formulaire par-dessus le
  monde (faisable — Bourgeon a déjà tout pour un formulaire ImGui au login, cf.
  [login_auth_imgui_design.md](login_auth_imgui_design.md)) ;
- l'acteur du joueur est créé sans apparence utile.

**Verdict : le plus proche de la demande, et de loin le plus risqué.** C'est un
chantier, pas un réglage.

### V2 — le mode Replay : le précédent natif d'un `CGameMode` sans serveur

Le client **sait déjà** faire tourner le jeu sans serveur. `WinMain` @ `0x00db91ab` :

```asm
strstr(cmdline, "Replay") → byte_15BEED0        ; « lancé en mode replay »
```

et ce drapeau **saute l'initialisation réseau** (`0x00db9d2d` :
`cmp byte_15BEED0, 0` ⇒ `sub_C14B10` n'est appelée que si nul). Le second drapeau,
**`g_ReplayActive` `0x015beecc`**, est consulté par **des dizaines de fenêtres**
(navigation, boutique, échoppe, raccourcis, chat…) pour désactiver tout ce qui
enverrait un paquet.

C'est donc **le drapeau natif « il n'y a pas de serveur »**, déjà respecté
partout — un allié précieux pour V1, et la preuve que le scénario n'est pas
absurde. Un `.rep` de « promenade » enregistré une fois rejouerait la map *avec*
ses acteurs.

**Verdict : à considérer sérieusement si V1 est retenue** — non pas comme
mécanisme d'affichage, mais comme *interrupteur hors-ligne* prêt à l'emploi.
Reste à mesurer si le mode replay traverse ou court-circuite l'écran de login.

### V3 — un survol pré-rendu, joué comme vidéo ★ recommandée

Puisque le volet A donne un lecteur vidéo natif, et que la demande est
« *comme si* un joueur se promenait », le rendu **n'a pas besoin d'être calculé
au login**. On produit le survol une fois, hors ligne :

1. **en jeu**, sur la map choisie, piloter la caméra avec **`ro::camera`**
   (`src/ragnarok/camera.h`) — le module partagé qu'utilisent déjà `FpsView` et
   `AfkScreen`. On écrit les **cibles** `+0x44` pitch / `+0x48` yaw / `+0x4c`
   distance, et **le moteur lisse tout seul** (un dixième de l'écart par frame) :
   les accélérations et les arrivées en douceur sont gratuites ;
   🔴 rappel du piège maison : **le pitch est NÉGATIF** (repos `-45`, plage
   `[-65, -25]` en extérieur) ; le mauvais signe ne donne pas une vue basculée
   mais une vue à ras du sol, identique pour toute valeur ;
2. **effacer les deux interfaces** exactement comme le fait `AfkScreen` (veto du
   site d'appel `0x00c74fd6` pour l'UI native, coupure au dispatch pour la nôtre,
   alpha nul pour les icônes d'état, quad du curseur hors viewport) ;
3. **capturer** : le pipeline de `ZoneRecorder` (`src/features/fx/zone_recorder.cc`)
   relit déjà le backbuffer DX9 dans `Present` ; il faut lui ajouter une sortie
   en images séquentielles plutôt qu'un GIF quantifié ;
4. **encoder** en `.bik` avec **RAD Video Tools** (Bink 1 — c'est la version
   qu'embarque le client) ;
5. **jouer** au login par le volet A.

**Ce qu'on y gagne** : aucun moteur 3D à réveiller au login, aucun paquet, aucun
`CGameMode` fantôme, aucune barre de chargement, un coût CPU constant et connu,
et un contrôle artistique total sur le trajet (on peut monter plusieurs plans).
**Ce qu'on y perd** : le fond n'est pas interactif et ne change pas — mais un
fond d'écran de login n'a pas à l'être.

---

## 5. Plan de mise en œuvre recommandé

**Étape 1 — prouver le décodeur (une heure).** Poser un `.bik` court dans
`<jeu>\data\video\`, ouvrir la fenêtre 304 depuis le panneau Moonlight :

```
void* w = uiwnd::MakeWindow(304);
uiwnd::OnMsg(w, 150, (int)"data\\video\\test.bik");   // charger
uiwnd::OnMsg(w, 159, 60, 0, 0);                       // volume, coupe la BGM
```

à faire **en jeu d'abord** (contexte le plus permissif), puis **au login**. Le
résultat de ce seul test tranche entre la voie native et le repli § A.7.

**Étape 2 — le fond de login.** Selon le résultat : soit la fenêtre 304 rouverte
en boucle et repoussée sous le formulaire, soit un module `login_backdrop`
utilisant directement `CBinkVideo` + `Render2D_SubmitTexturedQuad`. Réglages
attendus dans le panneau : activation, fichier, volume, et respect de l'option
BGM du joueur.

**Étape 3 — le survol.** Sortie « séquence d'images » dans `ZoneRecorder`, puis
un petit module de trajectoire de caméra (points de passage + durée) réutilisant
`ro::camera` et l'effacement d'interface d'`AfkScreen`.

**Étape 4 — seulement si le fond fixe ne suffit plus.** Reprendre V1/V2, en
commençant par mesurer ce que devient un `CGameMode` sans socket.

---

## 6. Ce qui reste à mesurer en live

Cette étude est **statique**. Les points suivants ne peuvent pas être tranchés
par la lecture du binaire, et sont ceux qui feraient échouer une implémentation
naïve :

1. **6.1 — La fenêtre 304 est-elle vraiment inemployée ?** Aucun `push 130h`
   littéral n'existe hors de la fabrique. Mais un appelant qui passerait
   l'identifiant **par registre ou par variable** échapperait à cette recherche.
   ⚠ Le projet a déjà payé cette leçon : *une recherche vide ne prouve pas
   l'absence*. À confirmer par un point d'arrêt sur `UIWindowMgr_MakeWindow`.
2. **La fenêtre 304 s'ouvre-t-elle au login ?** Rien dans son constructeur ni
   dans son rendu ne consulte le mode actif — mais le case de la fabrique
   (`0xa41280`) n'a pas été décompilé en entier.
3. **La texture cible se dimensionne-t-elle toute seule ?** Le msg 150 alloue une
   texture *sans* passer les dimensions de la vidéo, alors que la voie
   « texture `.bik` » les passe explicitement. Soit le premier `Lock` la
   redimensionne, soit il manque un message. À observer.
4. **Le blit tient-il la cadence ?** `BinkCopyToBuffer` en `BINKSURFACE32A` sur
   une vidéo plein écran, chaque frame, dans un processus 32 bits déjà chargé.
5. **Le son Bink au login** — armé au boot, mais jamais exercé dans ce build.
6. **La cohabitation avec la BGM `bgm\01.mp3`** que `CLoginMode_EnterMode` lance.
7. **DX7** : tout le volet ImGui du repli § A.7 y est inopérant.
8. **Pour V1/V2** : ce que devient `RecvLoop_DispatchPackets` sans socket, et si
   le mode Replay traverse l'écran de login.

---

## 7. Table des adresses

### Vidéo — Bink

| Adresse | Nom |
|---|---|
| `0x00775500` | `CBinkVideo_ctor` |
| `0x00775520` | `CBinkVideo_Close` |
| `0x00775590` | `CBinkVideo_Stop` |
| `0x007755d0` | `CBinkVideo_BlitToTexture` (`BINKSURFACE32A`) |
| `0x00775670` | `CBinkVideo_Open(this, path, loop, useResStream)` |
| `0x007757c0` | `CBinkVideo_Advance` |
| `0x00775980` | `VideoMgr_ctor` |
| `0x00775f20` | `VideoMgr_GetOrCreate(name, loop)` — préfixe `video\` |
| `0x00776320` | `VideoMgr_UpdateAllAndSpatialVolume` |
| `0x005688f0` | `SpriteRes_LoadFromFile` — **le test `.bik`** |
| `0x00a86c6f` | lecteur d'intro : `Open("openning.bik")` |
| `0x00a86ca0` | lecteur d'intro : lecture bloquante + clé `BINKMODE` |
| `0x0104e478` | chaîne `"openning.bik"` |
| `0x0104e488` | chaîne `"BINKMODE"` |
| `0x01018dc8` | chaîne `"video\"` |

### Vidéo — `UIVideoWnd`

| Adresse | Nom |
|---|---|
| `0x0104d19c` | vtable `UIVideoWnd` |
| `0x0124ac6c` | RTTI `.?AVUIVideoWnd@@` |
| `0x00a56410` | `UIVideoWnd_ctor` |
| `0x00a56610` | `UIVideoWnd_dtor` (restaure le volume BGM) |
| `0x00a566e0` | `UIVideoWnd_OnCreate` (bouton SKIP, commande 521) |
| `0x00a567b0` | `UIVideoSkipButton_OnDraw` |
| `0x00a56950` | `UIVideoWnd_OnRender` (letterbox) |
| `0x00a569d0` | `UIVideoWnd_OnUpdate` (avance + blit + auto-fermeture) |
| `0x00a56a20` | `UIVideoWnd_OnMsg` (msg 150 / 151 / 159 / 5 / 6) |
| **304** | identifiant de fenêtre |
| `0x00568620` | `SpriteTexFactory_NewTexture` |
| `0x007dbb20` | `Render2D_SubmitTexturedQuad(x, y, tex, color, w, h)` |
| `0x00600be0` | `Sound_SetBgmVolume` |

### Modes, monde, rendu

| Adresse | Nom |
|---|---|
| `0x01213338` | `g_ModeMgr` |
| `0x00a756e0` | `CModeMgr_Run(mode, map)` |
| `0x00a764e0` | **`CModeMgr_RequestSwitch(mgr, mode, map)`** |
| `0x00a756a0` | `CModeMgr_QuitGame` |
| `0x00a75340` | `GameMode_GetActive` (`0` hors jeu) |
| `0x010932f0` | vtable `CLoginMode` |
| `0x010904b8` | vtable `CGameMode` |
| `0x00d1e260` | `CLoginMode_ctor` (`0x6F7C` octets) |
| `0x00c63570` | `CGameMode_ctor` (`0x670` octets) |
| `0x00d26a70` | `CLoginMode_EnterMode` — **ignore le nom de map** |
| `0x00d27190` | `CLoginMode_Run_StateLatch` |
| `0x00d272e0` | `CLoginMode_OnUpdate` |
| `0x00c733d0` | `CGameMode_EnterWorld` |
| `0x00c74a80` | `GameMode_InGame_ProcessFrame` |
| `0x00a682d0` | `CWorld_ctor` (vtable `0x0104d598`) |
| `0x00a6aff0` | **`CWorld_Load`** |
| `0x00a7b0a0` | `CScene_RenderCellsAndCursor` (vtable scène `+0x0c`) |
| `0x0104dee4` | vtable `CScene` / caméra |
| `0x012515f8` | `g_SceneRenderQueue` (vtable `0x00fd6b08`) |
| `0x00551080` | `Renderer_ClearBackbuffer` (login) |
| `0x005510b0` | `Renderer_ClearWithSkyColor` (jeu) |
| `0x00551c10` | `Renderer_RenderFrame` — **appelée dans les deux modes** |
| `0x00552fa0` | `World_RenderScene` |
| `0x00a8d4a0` | `UITextureMgr_Load` — la porte unique des ressources |
| `0x015beed0` | « lancé en mode Replay » |
| `0x015beecc` | `g_ReplayActive` — le drapeau « pas de serveur » |
| `0x01602ad8` | drapeau de sortie du jeu |
| `0x010916d0` | chaîne `"login.rsw"` (vestige) |

---

Voir aussi : [camera_and_afk_re.md](camera_and_afk_re.md) (le rig de caméra et
l'effacement des interfaces), [login_flow_re.md](login_flow_re.md) (la machine à
états du login), [login_auth_imgui_design.md](login_auth_imgui_design.md) (le
substrat ImGui disponible au login), [sprite_rendering_re.md](sprite_rendering_re.md).
