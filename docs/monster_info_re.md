# Fenêtre « Monster Info » (skill Sense) — RE de `UIMonsterInfoWnd`

Client cible : `Moonlight-Destiny.exe` (base `0x400000`, build 20250716).
Serveur : fork `moonlight` de rAthena (`src/map/clif.cpp`, `src/map/skills/mage/sense.cpp`).

Objectif du document : décrire **exhaustivement** le chemin natif qui va du lancement du
skill `WZ_ESTIMATION` (« Sense », id 93) jusqu'à l'affichage de la fenêtre d'information de
monstre, afin de pouvoir la remplacer par une fenêtre ImGui — et documenter au passage les
trois pièges du natif (le nom qui ne vient **pas** du paquet, la liste noire de mobs, et
l'illustration qui n'est **pas** animée).

Tout ce qui suit est soit lu au désassemblage, soit vérifié **en live** (x32dbg, client
connecté, fenêtre ouverte sur le mannequin d'entraînement `DUMMY_10DEF_10MDEF`), soit
recoupé avec les sources serveur. Les relevés à chaud sont signalés par ⏱.

---

## 1. Vue d'ensemble du flux

```
le joueur lance Sense (WZ_ESTIMATION, id 93 = 0x5D) sur une cible
        │
        ▼
CZ_USE_SKILL (0x0438) { op:2, skillLv:2, skillId:2, targetGID:4 }  len 10
        │   bloc de construction @ 0x00C8DE46
        │   🔴 EFFET DE BORD : si skillId == 0x5D, le client MÉMORISE le GID de la
        │      cible dans le global g_SenseTargetGID @ 0x015FFFB8 (0x00C8DE89).
        │      C'est de LÀ que viendra le nom affiché — jamais du paquet réponse.
        ▼
            [ SERVEUR : SkillSense::castendNoDamageId  → clif_skill_estimation() ]
            [ échoue sur un joueur (clif_skill_fail), ne part que sur un mob     ]
            [ ⚠ portée d'envoi : PARTY_SAMEMAP si le lanceur est en groupe       ]
        ▼
ZC_MONSTER_INFO (0x018C)  len 29     handler inline @ 0x00CA5E4D
        │
        ├─ class ∈ { 1285, 1286, 1287, 1829, 1830 } ──► PAQUET JETÉ, rien ne s'ouvre
        │      (les 5 Gardiens de forteresse WoE — cf. §5)
        │
        └─ sinon ──► MakeWindow(0x4D)              → UIMonsterInfoWnd (400 × 200)
                     OnMsg(msg = 0x4F, p2 = &paquet)  → copie les 29 octets
        ▼
    [ la fenêtre se redessine ; bouton OK (command id 184) = QueueDestroyWindow ]
```

### Les trois points cruciaux pour le portage

> **1. Le nom du monstre n'est PAS dans le paquet.** `ZC_MONSTER_INFO` ne transporte que
> la *classe* (id de sprite). Le natif va chercher le nom dans le dictionnaire de noms de
> la `CGameMode` (`+0x160`), à la clé `g_SenseTargetGID` — un global écrit par **l'envoi**
> de CZ_USE_SKILL. Conséquence directe : cf. le bug de groupe en §6.
>
> **2. Le natif ne connaît que ce que le serveur lui envoie**, c'est-à-dire 9 champs plus
> 9 résistances élémentaires. Pas d'EXP, pas de drops, pas de spawn, pas de skills : la
> fiche détaillée voulue côté Bourgeon exige un aller-retour serveur supplémentaire.
>
> **3. L'illustration est un sprite de mob figé.** Le natif charge bien
> `몬스터\<resname>.spr` + `.act` mais appelle le rendu avec une période d'animation
> nulle ⇒ **frame 0 de l'action 0**, jamais animé (cf. §4.4).

---

## 2. Identité de la fenêtre

| Élément | Valeur |
|---|---|
| Classe RTTI | `.?AVUIMonsterInfoWnd@@` (`0x0123F080`) |
| vtable | `0x01030750` (COL en `vt-4` = `0x0103074C`) |
| **Window id** | **`0x4D` (77)** |
| Taille de l'instance | `0xF0` (240 octets) |
| Taille de la fenêtre | `400 × 200`, posée par `UIWindow_SetSize` juste après le ctor (`0x00A3DA1F`) ⏱ confirmé : `+0x14 = 0x190`, `+0x18 = 0xC8` |
| Constructeur | `0x0086C3C0` |
| Destructeur | `0x0086E730` |
| Slot manager dédié | **aucun** — elle vit uniquement dans la `std::map<int, UIWindow*>` en `g_UIWindowMgr+8` ⇒ `FindWindow(0x4D)` passe par la branche générique |

### Comment l'id a été établi

`UIWindowMgr_MakeWindow` (`0x00A39340`) est un `switch` à deux étages :

```asm
cmp  esi, 2730h            ; ids « hauts » (CUIGameSettingsUI 0x271E…) traités ailleurs
cmp  esi, 16Ah             ; ja -> défaut
movzx eax, byte [esi + 0A42CA8h]   ; table de remap  windowId -> n° de cas
jmp  ds:0A42904h[eax*4]            ; table de saut
```

L'entrée de la table de saut qui pointe sur le bloc `new(0xF0) + UIMonsterInfoWnd::ctor`
(`0x00A3D9E6`) est l'indice **70** ; le seul `windowId` dont la table de remap vaut 70 est
**`0x4D`**. ⏱ Recoupé en live : la `std::map` du manager contient bien une entrée de clé
`0x4D` pointant sur l'instance, et l'instance porte `0x4D` dans son champ `+0x2C`.

### Table des méthodes virtuelles

La vtable est celle de la base `UIWindow` **sauf quatre slots** (vérifié en comptant les
références de chaque cible : les slots partagés sont référencés par 200 à 600 vtables, ces
quatre-là par une seule) :

| Offset | Adresse | Rôle |
|---|---|---|
| `+0x00` | `0x0086E730` | destructeur |
| `+0x3C` | `0x00877C40` | **OnCreate** — bouton OK + capture du nom (§4.2) |
| `+0x50` | `0x0087F7C0` | **OnRender** — tout le dessin (§4.3) |
| `+0x70` | `0x00880CC0` | **OnMouseMove** — infobulle du nom tronqué (§4.5) |
| `+0x94` | `0x00885C80` | **OnMsg** (§4.1) |

Aucun autre slot n'est surchargé : il n'y a **pas** de `SendPacket` dans cette classe. Elle
peut donc être détruite ou empêchée de naître sans dette protocolaire — contrairement à
`UIRodexWriteWnd` (cf. [[reference_native_window_toggle_router]]).

---

## 3. Le paquet `ZC_MONSTER_INFO` (0x018C)

### 3.1 Côté serveur

`clif_skill_estimation( const map_session_data& sd, mob_data& md )`
— `src/map/clif.cpp:8600`.

```c
struct PACKET_ZC_MONSTER_INFO {   // len = 29 (packed)
    int16  packetType;   // 0x018C
    uint16 class_;       // md.vd->look[LOOK_BASE]  ⚠ classe de VUE, pas mob_id
    uint16 level;
    uint16 size;         // 0 = Small, 1 = Medium, 2 = Large
    uint32 hp;
    int16  def;          // (sense_type&1 ? def : 0) + (sense_type&2 ? def2 : 0)
    uint16 race;         // 0..9
    int16  mdef;         // idem def, avec mdef/mdef2
    uint16 element;      // md.status.def_ele (0..9)
    uint8  water, earth, fire, wind, poison, holy, shadow, ghost, undead;  // % encaissés
};
```

Trois remarques qui comptent pour la fiche Bourgeon :

* **`class_` est l'id de *vue*, pas l'id de base de données.** Pour un mob déguisé
  (`ViewClass` différent dans `mob_db.yml`) ou un mob « slave » qui emprunte l'apparence
  d'un autre, l'id reçu ne permet pas de retrouver la bonne entrée de `mob_db`. Le natif
  s'en moque (il ne s'en sert que pour le sprite) ; nous, non.
* **`def`/`mdef` dépendent d'une conf serveur.** `battle_config.estimation_type` s'appelle
  `sense_type` dans les fichiers de conf (`src/map/battle.cpp:8472`), valeur par défaut
  `1|2` = def **+** def2. Sur Moonlight (pré-renewal), c'est donc bien la somme.
* Les résistances sont bornées à 0 par le serveur (`std::max(..., 0)`) « since the client
  displays them as 255-fix » : un chiffre négatif ferait un `unsigned char` géant.

### 3.2 Handler client

Le handler est **inline** dans `RecvLoop_DispatchPackets`, cas 396 (`0x018C`), en
`0x00CA5E4D`. Le tampon du paquet est le global `0x015E8198`
(`+2` = `class_`, lu en `word ptr 0x015E819A`).

```asm
; 0x00CA5E4D — cas 0x018C
mov  ax, word ptr [15E819Ah]      ; class_
cmp  ax, 505h  / 506h / 507h / 725h / 726h
jz   <fin>                        ; ⇒ paquet JETÉ, aucune fenêtre
push 4Dh
mov  ecx, offset g_UIWindowMgr
call UIWindowMgr_MakeWindow
push 0 / 0 / 0
push offset 015E8198h             ; &paquet
push 4Fh                          ; msg
push 0
call [vtable + 94h]               ; OnMsg
```

Une **seconde copie** de ce même code, autonome, existe en `0x00CE4320`
(`__stdcall(void* packet)`) : même liste noire, même `MakeWindow(0x4D)`, même
`OnMsg(0x4F)`. Elle n'a **aucune référence** dans le binaire — code mort, probablement le
vestige d'un chemin « replay ». Ne pas la hooker en croyant couvrir un second chemin.

---

## 4. Anatomie de `UIMonsterInfoWnd`

### 4.1 `OnMsg` (`0x00885C80`, vtable `+0x94`)

```c
int UIMonsterInfoWnd::OnMsg(int arg0, int msg, int p2, ...) {
  if (msg == 6) {                       // clic sur un contrôle enfant
      if (p2 == 184) UIWindowMgr_QueueDestroyWindow(g_UIWindowMgr, this);
      return 0;                         // 184 = command id du bouton OK
  }
  if (msg == 79) {                      // 0x4F — « voici les données »
      if (p2) {                         // p2 = pointeur sur le PAQUET BRUT
          memcpy(this + 0xB4, p2, 29);  // 16 + 8 + 4 + 1 octets, en 4 mouvements
      }
      (*(vtable + 152))(this);          // UIWindow_PaintDispatch = invalidation
      return 0;
  }
  return UIWindow_OnMsg_Default(...);
}
```

Deux messages seulement. Le `memcpy` recopie le paquet **entier, en-tête compris** : le
champ `+0xB4` de l'objet contient donc littéralement `0x018C`.

### 4.2 `OnCreate` (`0x00877C40`, vtable `+0x3C`)

Exécuté **une seule fois, à la construction**. Dans l'ordre :

1. `new UIBitmapButton` + les trois états de bitmap
   `유저인터페이스\btn_ok.bmp` / `btn_ok_a.bmp` / `btn_ok_b.bmp` ;
2. positionnement en `(largeurBouton - 46, hauteurBouton - 24)`, **command id 184**,
   `AddChildControl` ;
3. `this+0xD4 = g_SenseTargetGID` (`0x015FFFB8`) — snapshot du GID ;
4. `GameMode_CopyEntityName(GameMode_GetActive(), &tmp, g_SenseTargetGID)`
   → `CNameDict_GetName(gameMode + 0x160, out, gid)` : **c'est ici que le nom arrive** —
   d'un **cache**, pas du réseau (cf. §4.2 bis) ;
5. coupe du nom au premier caractère de la chaîne statique `0x01006CD4` (séparateur de
   suffixe de nom) ;
6. **troncature à 100 px** en police 12 avec ellipse (`0x00A21F90`), puis stockage dans la
   `std::string` en `this+0xD8`.

### 4.2 bis — D'où vient VRAIMENT le nom

`GameMode+0x160` est le **dictionnaire de noms `CNameInfo`**, un `std::map` indexé par
**GID** — un cache, pas une source. Le nom y entre par le serveur, jamais autrement
(détail complet dans `docs/entity_nameplate_re.md` §2-3) :

| Chemin | Handler | Effet |
|---|---|---|
| **ZC 0x0A30** `ZC_ACK_REQNAMEALL2` | `Actor_ApplyNameAllWithTitle_ZC0A30` `0x00CF2A50` | `CNameDict_SetFullNameInfo` (nom + party + guilde + rang + titre) |
| **spawn nommé** (`ZC_NOTIFY_STANDENTRY`, nom à `pkt+0x4B`) | `GameMode_OnRecv_ActorSpawn_Named` `0x00CC99A0` | `CNameDict_SetName` (nom seul) |

Et c'est le CLIENT qui réclame : `CNameDict_GetEntryOrRequest` (`0x005A1460`) met le GID
dans la file d'attente `dict+0x0C`, vidée en `CZ_REQNAME`. C'est le chemin des **plaques
de nom** — survoler ou cibler une entité déclenche la demande.

> 🔴 **`CNameDict_GetName` (`0x005A1640`) ne demande RIEN.** Il passe par
> `CNameDict_GetOrCreateEntry` (`0x005A0D30`), pas par `GetEntryOrRequest` : sur un GID
> absent, il **insère une entrée vide** (`new(0xB0)` + init des `std::string`) et renvoie
> une chaîne **vide**. Aucune requête n'est émise, ni à cet instant ni plus tard.
>
> Autrement dit, la fenêtre native n'affiche un nom que si le joueur avait DÉJÀ survolé
> ou ciblé ce monstre. Pour le lanceur c'est presque toujours le cas — il faut cibler
> pour lancer Sense. Pour un coéquipier qui reçoit le paquet en `PARTY_SAMEMAP`, non :
> et comme la clé lue est de toute façon SON `g_SenseTargetGID` à lui, il n'y a même pas
> de bon GID à chercher (§6.1).

⏱ Relevé en live sur le mannequin : la `std::string` contient `"10Def 10Mdef S..."`
(17 caractères), alors que le nom complet en base est `10Def 10Mdef Small Norm`. La
troncature est donc bien **destructive et persistée** — l'infobulle de §4.5 re-interroge le
dictionnaire pour retrouver le nom entier.

### 4.3 Champs d'instance et rendu (`0x0087F7C0`, vtable `+0x50`)

Disposition de l'objet (240 octets) :

| Offset | Type | Contenu | Champ paquet |
|---|---|---|---|
| `+0xB4` | `u16` | `packetType` (toujours `0x018C`) | `+0` |
| `+0xB6` | `s16` | **classe / id de sprite** | `+2` |
| `+0xB8` | `s16` | niveau | `+4` |
| `+0xBA` | `s16` | taille (0/1/2) | `+6` |
| `+0xBC` | `s32` | PV max | `+8` |
| `+0xC0` | `s16` | DEF | `+12` |
| `+0xC2` | `s16` | race (0..9) | `+14` |
| `+0xC4` | `s16` | MDEF | `+16` |
| `+0xC6` | `s16` | élément (0..9) | `+18` |
| `+0xC8`..`+0xD0` | `u8 × 9` | eau, terre, feu, vent, poison, saint, ténèbres, spectre, mort-vivant | `+20`..`+28` |
| `+0xD1`..`+0xD3` | — | bourrage, jamais écrit (contient des résidus de tas) | — |
| `+0xD4` | `u32` | GID de la cible (copie de `g_SenseTargetGID`) | *(hors paquet)* |
| `+0xD8` | `std::string` | nom déjà **tronqué à 100 px** (SSO 16 o, taille `+0xE8`, capacité `+0xEC`) | *(hors paquet)* |

⏱ Vérification en live, instance `0x153043D0`, mannequin d'entraînement :

```
+0xB4  8C 01        0x018C
+0xB6  68 09        classe 2408   → db/import/mob_db.yml : DUMMY_10DEF_10MDEF
+0xB8  0A 00        niveau 10                              Level: 10
+0xBA  00 00        Small
+0xBC  00 E1 F5 05  100 000 000 PV                         Hp: 100000000
+0xC0  0A 00        DEF 10                                 Defense: 10
+0xC2  00 00        race 0 (Formless)
+0xC4  0A 00        MDEF 10                                MagicDefense: 10
+0xC6  00 00        élément 0 (Neutre)
+0xC8  64 64 64 64 64 64 64 19 64   100 % partout SAUF spectre 25 %
                                    ← table élémentaire Neutre niv. 1, exacte
+0xD4  2C 11 2E 00  GID 0x002E112C  == g_SenseTargetGID lu au même instant
```

Le rendu ne crée **aucun contrôle enfant** (hormis le bouton OK) : tout est dessiné à la
main dans la surface, à chaque frame invalidée.

Gabarit exact (coordonnées locales à la fenêtre, police 12) :

```
 (2,19)  ┌──────────┐   Nom      : <this+0xD8>     Taille   : <MSI_MONSTERINFO_SIZE_*>
         │          │   (120,30)   (160,30)          (260,30)  (300,30)
         │  sprite  │   Niveau   : <+0xB8>         Race     : <MSI_MONSTERINFO_TRIBE_*>
         │ 116×152  │   (120,50)   (160,50)          (260,50)  (300,50)
         │  FIGÉ    │   PV       : <+0xBC>         Élément  : <MSI_MONSTERINFO_PROPERTY_*>
         │          │   (120,70)   (160,70)          (260,70)  (300,70)
         │          │   DEF      : <+0xC0>         MDEF     : <+0xC4>
         └──────────┘   (120,90)   (160,90)          (260,90)  (300,90)

   eau      terre    feu          (120,110) (120,128) (120,146)
   vent     poison   saint        (200,110) (200,128) (200,146)
   ténèbres spectre  mort-vivant  (280,110) (280,128) (280,146)
```

Chaque case de résistance passe par `DrawResistCell` (`0x00872B70`) : un cadre `70×16`
couleur palette `(22,2)`, un intérieur `68×14` couleur de fond `(2,2)`, puis le texte
`"<libellé>:<valeur>"` centré. Le libellé réutilise les mêmes `MSI_MONSTERINFO_PROPERTY_*`
que la ligne « Élément ».

**Le titre de la fenêtre est dessiné deux fois** : `MSI_MONSTER_INFO_WINDOW` en blanc à
`(17,3)` puis en noir à `(16,2)` — c'est l'ombre portée, pas un doublon.

### Table des chaînes (`msgstringtable`)

Le tableau des noms symboliques est en `0x0104F0A8`, **indexé par l'id** (vérifié sur trois
points indépendants : `MSI_MONSTER_INFO_WINDOW` = 0x196, `MSI_MONSTERINFO_TRIBE_UNKNOWN` =
0x0F2F, `MSI_MONSTERINFO_DEF` = 0x0F46, tous trois retrouvés tels quels dans le code de
rendu).

| Id | Clé | Usage |
|---|---|---|
| `0x0196` | `MSI_MONSTER_INFO_WINDOW` | titre |
| `0x0197` | `MSI_NAME` | libellé « Nom » |
| `0x0198` | `MSI_LEVEL` | libellé « Niveau » |
| `0x0199` | `MSI_HP` | libellé « PV » |
| `0x019A` | `MSI_SIZE` | libellé « Taille » |
| `0x019B` | `MSI_RACETYPE` | libellé « Race » |
| `0x019C` | `MSI_MDEFPOWER` | libellé « MDEF » |
| `0x019D` | `MSI_PROPERTY` | libellé « Élément » |
| `0x0F2F`..`0x0F38` | `MSI_MONSTERINFO_TRIBE_{UNKNOWN,UNDEAD,ANIMAL,PLANT,INSECT,MARINE,DEVIL,HUMAN,ANGEL,DRAGON}` | valeurs de **race** (`+0xC2`), indexées directement |
| `0x0F39`..`0x0F42` | `MSI_MONSTERINFO_PROPERTY_{NEUTURAL,WATER,EARTH,FIRE,WIND,POISON,SAINT,DARK,MENTAL,UNDEAD}` | valeurs d'**élément** (`+0xC6`), indexées par `élément % 20` |
| `0x0F43`..`0x0F45` | `MSI_MONSTERINFO_SIZE_{SMALL,MIDDLE,BIG}` | valeurs de **taille** (`+0xBA`) |
| `0x0F46` | `MSI_MONSTERINFO_DEF` | libellé « DEF » |

> Le `% 20` sur l'élément est un reste du codage historique `élément + 20 × niveauÉlément`.
> Le serveur Moonlight n'envoie que `def_ele` (0..9), l'opération est donc neutre — mais
> **le niveau élémentaire est perdu** : le natif ne peut pas afficher « Eau 3 ».
>
> Attention à la dissymétrie : la **race** est indexée sans modulo et sans `default`
> (`switch` 0..9). Une race hors bornes ⇒ la ligne « Race » n'est simplement pas dessinée.

### 4.4 L'illustration du monstre

```asm
Job_GetDisplayNameOrResName(g_UIWindowContextKey, class_ /*+0xB6*/, -1)  → resname
snprintf(argSpr,  128, "몬스터\%s.spr", resname)     ; fmt @ 0x0103181C (CP949)
snprintf(argAct,  128, "몬스터\%s.act", resname)     ; fmt @ 0x0103182C (CP949)
UITextureMgr_ResourceExists(act) && UITextureMgr_ResourceExists(spr)  ; sinon on ne dessine rien
UIWindow_DrawActSprite(this, spr, act, x=2, y=19, w=116, h=152, periodMs=0, fit=1, overlay=0)
```

`UIWindow_DrawActSprite` = `0x00A1DC80`. Son corps calcule la boîte englobante de tous les
calques de la frame, la met à l'échelle **vers le bas uniquement** pour tenir dans `w×h`, et
la centre. Le choix de la frame :

```c
if (periodMs > 0)  frame = (timeGetTime() % periodMs) * (nbFrames / (float)periodMs);
else               frame = 0;
```

`periodMs` vaut **0** au site d'appel ⇒ **frame 0 de l'action 0**, statique. L'action 0 d'un
`.act` de monstre est l'idle vers le bas ; le natif en montre donc la toute première image,
sans jamais la faire tourner. Le rendu animé demandé côté Bourgeon est un **ajout**, pas une
reproduction.

#### 🔴 Le résolveur de nom : `jobName.lub`, et rien d'autre

Le client a **deux** fonctions qui rendent un « nom de ressource » pour une classe, et
elles ne sont pas interchangeables :

| Adresse | Nom | Ce que c'est |
|---|---|---|
| `0x00D824C0` | `Monster_GetResNameById` (`__stdcall`) | un `switch` **EN DUR** d'environ 130 monstres, `default:` → **`"poring"`** |
| `0x00D5BB40` | `Job_GetDisplayNameOrResName` (`__thiscall(ctx, classId, sex)`) | indexe le vecteur bâti depuis **`jobName.lub`** ; c'est ce que la fenêtre native appelle |

Le vecteur vit à `g_UIWindowContextKey + 0xF88` (`this[994]` = begin, `this[995]` = end),
indexé directement par la classe :

```c
if ((this[995] - this[994]) >> 2 <= classId) return "";          // hors table
if (classId - 4001 > 0x7CE && classId > 0x1E)
    return *(char**)(this[994] + 4 * classId);                   // <- les monstres
// (4001..5999 = jobs joueurs : sexe / tenue alternative, branche sans objet ici)
```

Il est rempli au boot par `Lua_LoadAllScriptFiles` (`0x00D646C0`), qui exécute
`Lua Files\DataInfo\jobName_F` **puis** `Lua Files\DataInfo\jobName`.

⚠ **Le nom de dossier n'est pas l'AegisName du serveur.** `jobName.lub` porte
`[jobtbl.JT_CHONCHON] = "Chocho"` — le serveur envoie la classe du Chonchon, le fichier
s'appelle `Chocho.spr`. Aucune règle de transformation ne relie les deux : seule la table
fait foi.

⚠ Et il faut passer par le **natif**, pas relire le `.lub` soi-même : le client charge
d'abord `jobName_F.lub`, et un fichier posé dans `data\` prime sur le GRF
([[reference_grf_loading_patcher]]). Seule la table du client reflète ce qui a réellement
été chargé.

> `src/features/overlays/login_parade.cc` (parade de Porings) et `roggle.cc` utilisent,
> eux, `Monster_GetResNameById` — sans dommage : leurs six ids de la famille Poring sont
> tous dans le `switch`. Le `default:` n'y est jamais atteint. Sur une fiche qui vise
> n'importe quel monstre, il l'est presque toujours.

### 4.5 `OnMouseMove` (`0x00880CC0`, vtable `+0x70`)

```c
void OnMouseMove(int x, int y) {
  if ((unsigned)(x - 0xA1) > 0x62) return;   // x ∈ [161, 259]
  if ((unsigned)(y - 0x1F) > 0x0A) return;   // y ∈ [31, 41]   ← la zone du NOM
  GameMode_CopyEntityName(GameMode_GetActive(), &name, g_SenseTargetGID);
  ... coupe au séparateur ...
  if (UIText_MeasureWidth(name, 12) > 100)   // seulement si le nom a été tronqué
      UIWindow_ShowHoverTooltip(name, ...);
}
```

C'est la seule interaction de la fenêtre en dehors du bouton OK. Elle **relit le
dictionnaire de noms à chaque survol** — donc, contrairement au champ `+0xD8` figé à la
création, l'infobulle suit les changements de nom… **mais toujours pour `g_SenseTargetGID`,
pas pour le monstre du paquet** (§6).

---

## 5. La liste noire des Gardiens

`{ 1285, 1286, 1287, 1829, 1830 }` — testés sur `class_` **avant** toute création de
fenêtre, dans le handler *et* dans la copie morte `0x00CE4320`.

| Id | AegisName | Nom |
|---|---|---|
| 1285 | `ARCHER_GUARDIAN` | Archer Guardian |
| 1286 | `KNIGHT_GUARDIAN` | Knight Guardian |
| 1287 | `SOLDIER_GUARDIAN` | Soldier Guardian |
| 1829 | `SWORD_GUARDIAN` | Sword Master (Sword Guardian) |
| 1830 | `BOW_GUARDIAN` | Bow Master (Bow Guardian) |

Ce sont les cinq Gardiens de forteresse de la Guerre d'Emperium : le client refuse
d'afficher leurs PV. Le blocage est **purement client** — le serveur, lui, envoie le paquet
normalement. Une fenêtre ImGui qui remplace le natif **rétablit donc l'information** ; si on
veut conserver le comportement historique, c'est à nous de refiltrer explicitement.

---

## 6. Les défauts du natif (et ce qu'ils imposent au portage)

### 6.1 🔴 Le nom est faux quand un coéquipier lance Sense

`clif_skill_estimation` termine par :

```c
clif_send( &packet, sizeof( packet ), &sd, (sd.status.party_id > 0) ? PARTY_SAMEMAP : SELF );
```

Si le lanceur est en groupe, **tous les membres du groupe sur la carte** reçoivent le
paquet. Chez eux, `g_SenseTargetGID` porte encore *leur* dernière cible de Sense (ou zéro
s'ils n'ont jamais lancé le skill) : la fenêtre s'ouvre avec les bons chiffres et le **mauvais
nom** — ou un nom vide.

Deux défauts se cumulent ici, et il faut les distinguer :

1. **La clé est fausse.** On lit le nom du GID que *ce client-là* a visé la dernière
   fois, pas celui du monstre décrit par le paquet — que le paquet ne porte même pas.
2. **La lecture ne répare rien.** `CNameDict_GetName` n'émet aucune requête (§4.2 bis) :
   sur un GID jamais survolé, le nom sort vide et le reste.

La fenêtre Bourgeon doit donc résoudre le nom **depuis `class_`** (base de données), et
n'utiliser le dictionnaire de noms d'entité que comme complément facultatif.

### 6.2 Le nom n'est jamais rafraîchi sur une fenêtre déjà ouverte

`OnCreate` (§4.2) est le seul écrivain de `+0xD8` et de `+0xD4`. `OnMsg(0x4F)` ne touche que
les 29 octets du paquet. Or le handler appelle `MakeWindow(0x4D)` **inconditionnellement**.

Le cas `0x4D` de `MakeWindow` n'a pas de slot dédié dans le manager, donc pas de garde
« déjà ouverte ? » en tête de cas (contrairement à, par exemple, le cas 7 qui teste
`mgr+0x1BC`). Le chemin commun construit un objet, puis insère dans la `std::map` en
`mgr+8` : à la fin (`0x00A426A2`), si la clé existe déjà, il fait `node->second = nouveau`
— **l'ancienne instance est écrasée dans la map**.

> ⚠ Lecture statique uniquement : je n'ai pas pu observer deux instances simultanées en
> mémoire (la recherche de motif x32dbg a échoué sur cette session). Ce qui est certain et
> suffisant pour nous : **la fenêtre affichée après un second Sense ne réutilise pas l'état
> nominal de la première**, et il n'existe aucun chemin qui remette `+0xD8` à jour sur une
> instance survivante.

### 6.3 Ce que le natif ne montre pas du tout

EXP base / EXP job, mode (agressif, assist, boss…), vitesse de déplacement, ATK/MATK,
portée, taux de drop, cartes, cartes de spawn, skills du mob, niveau élémentaire. **Rien de
tout cela n'est dans `ZC_MONSTER_INFO`** : une fiche complète impose un opcode custom
supplémentaire (client → serveur → client), sur le modèle de `0x0F0B`/`0x0F0C`
(« tech data » de la fiche d'item).

---

## 7. Plan de remplacement côté Bourgeon

### 7.1 Mise à mort de la native

Aucune dette protocolaire (§2) : la classe n'émet aucun paquet, ni à la création ni à la
destruction. Le geste correct est donc **d'empêcher la fenêtre de naître**, en revendiquant
le paquet :

```cpp
Bourgeon::Instance().RegisterReplaceOpcode(0x018C, [] { return MonsterInfoWindow::Claim(); });
```

`RegisterReplaceOpcode` court-circuite le handler natif ; `0x018C` est un paquet de
**longueur fixe**, ce qui est précisément le cas que le résolveur de longueurs
(`PacketLenTable_Lookup`, cf. [[reference_native_packet_len_resolver]]) rend utilisable.

Devoirs cachés du handler remplacé (cf. [[feedback_replaced_handler_hidden_duties]]) —
inventaire complet, et il est court :

| Devoir | Verdict |
|---|---|
| écrit un global ? | **non** (le seul global du flux, `g_SenseTargetGID`, est écrit à l'**envoi** de CZ_USE_SKILL, pas à la réception) |
| empêche quelque chose ? | **oui** : la liste noire des Gardiens (§5) — à reproduire ou à assumer |
| le serveur suppose quelque chose ? | **non** : `ZC_MONSTER_INFO` est terminal, aucune réponse attendue |

### 7.2 Ce qu'il faut aller chercher en plus — LIVRÉ

Couple d'opcodes custom (zone sûre `0x0F00+`, cf. `features/systems/bourgeon_opcodes.h`) :

| Opcode | Nom | Forme |
|---|---|---|
| `0x0F1F` | `CZ_BOURGEON_REQ_MOBINFO` | fixe 9 : `[type:2][len:2][mob_id:4][by_view:1]` |
| `0x0F20` | `ZC_BOURGEON_MOBINFO` | variable : bloc fixe de 98 o + nom + drops + spawns + skills + identité |

Le détail des champs est en commentaire au-dessus des deux structures
(`moonlight/src/map/packets_struct.hpp`), et le handler est
`clif_parse_bourgeon_reqmobinfo` (`src/map/clif.cpp`).

⚠ Trois points qui n'étaient pas évidents à l'écriture :

* **`by_view`.** L'id envoyé doit être l'id de base de données, pas la classe de vue
  reçue en `+0xB6` (§3.1). Le client ne peut pas faire la correspondance — il n'a pas
  `mob_db`. Le serveur essaie donc `mob_db.find(id)` **d'abord** (vue == id pour
  l'immense majorité des monstres) et ne balaie le `ViewClass` qu'en cas d'échec :
  l'ordre inverse ferait perdre le cas normal, un monstre déguisé empruntant
  l'apparence d'un autre monstre **existant**.
* **HIT / FLEE / CRIT ne sont pas envoyés.** `mob_db` ne les porte pas : ce sont des
  dérivés que `status_calc_misc()` ne calcule qu'au SPAWN, sur un `block_list`.
  L'entrée de base les laisse à zéro — les envoyer afficherait des zéros crédibles.
* **Les résistances repartent SIGNÉES.** `clif_skill_estimation` les borne à 0 parce
  que le natif les lit en octet non signé (`-25` s'y afficherait « 231 »). Notre paquet
  n'a pas cette contrainte, donc une absorption (< 0) s'affiche telle quelle.

### 7.2bis 🔴 L'identité : de QUEL monstre cette fiche parle-t-elle

Des dizaines de monstres partagent **exactement** le nom affiché et l'apparence d'un
autre — versions d'événement, d'invocation, d'instance. Le joueur qui ouvre l'un d'eux
voit un monstre familier sans butin ni carte de spawn et conclut que la fiche est
buguée. C'est un faux positif que la fiche doit désamorcer elle-même.

La queue du paquet porte donc
`[aegislen:1][aegis:N][summoned:1][namesake_count:1][namesake_ref:4]`, calculée une
fois au premier appel (`mob_db` ne bouge plus après le chargement).

**Ce sont des FAITS, jamais un drapeau « variante ».** Les deux raccourcis qu'on
serait tenté de prendre sont faux, mesures à l'appui sur ce `mob_db` :

| Raccourci tentant | Pourquoi il est faux |
|---|---|
| « le préfixe de l'AegisName » | des noms légitimes commencent par `ORC_`, `KOBOLD_`, `GOBLIN_`, `THIEF_`, `SOLDIER_`, `TREASURE_`… et de vraies variantes portent au contraire un underscore **final** (`FABRE_`, `CHONCHON_`) |
| « pas de butin / pas de spawn » | **204 des 421** homonymes ONT du butin, et **51 monstres de base** n'en ont aucun — la règle se tromperait dans les deux sens |

Sur les 266 noms partagés, **88 doublons ne sont pas des variantes du tout** :
`GOBLIN_2..5`, `DIMIK_1..4`, `VENATU_1..4`, `PICKY_`, `PETIT_` sont des monstres à part
entière, spawnés et avec butin. Un drapeau unique les aurait mélangés aux versions
d'événement.

**Ce que le client en fait** (`VariantQualifier`, monster_info_window.cc) : un
qualificatif accolé au titre, du plus certain au moins certain, et **rien** au-delà.

1. `EVENT_` — le nom se déclare lui-même ⇒ « (événement) » ;
2. `summoned` — le serveur a CONSTATÉ un `NPC_SUMMONSLAVE` visant cet id. C'est ce qui
   couvre la famille `G_` (88 des 213) sans rien supposer de son préfixe ⇒ « (invoqué) » ;
3. `E_` — convention, mais aucun des 21 n'a de spawn ni d'instance ⇒ « (événement) ».

`META_`, `A_`, `R_`, `M_`, `W_`, `B_` n'ont **pas** de sens établi sur ce serveur — et
`B_SEYREN` & co sont de vrais boss d'instance avec butin. On se tait. Le bandeau
d'homonymie, lui, reste affiché dans tous les cas : il n'énonce que du vérifiable
(combien portent ce nom, lequel est celui-ci, raccourci vers le plus ancien) et ne dit
jamais un mot du butin ni des spawns — les onglets les montrent déjà.

### 7.3 Points d'entrée de la nouvelle fenêtre

1. **Sense** — via le paquet revendiqué en §7.1. Le relevé du paquet est conservé à
   part (`SenseSnapshot`) et affiché **uniquement s'il diffère** de `mob_db` : c'est
   alors une information (modificateur de serveur, GdE, monstre invoqué), pas un bug.
2. **Table des sources de la fiche d'item** — `item_desc_window.cc`. Le nom de monstre
   y était un lien vers le bestiaire du site ; clic gauche ouvre désormais la fiche en
   jeu, clic droit garde le site. Groupe « Interface moderne » coupé, le clic gauche
   retombe sur l'ancien comportement.
3. À venir : plaques de nom, journal de quête, tout endroit qui affiche un id de mob.

### 7.4 Fichiers

| Fichier | Rôle |
|---|---|
| `src/features/windows/monster_info_window.{h,cc}` | la fenêtre ImGui, le parseur, la revendication du 0x018C |
| — habillage | `ro::BeginRoDescWindow` + les mêmes 4 couleurs / 3 arrondis qu'`ItemDescWindow` : c'est un panneau de **description**, pas une fenêtre RO ordinaire |
| — lisibilité | corps CLAIR ⇒ `ImGui::TextDisabled` proscrit (helper `Label()`, palette sombre du projet) |
| — compétences | nom par `GetSkillName` (`0x0073A1F0`, Lua, CP949), description = fenêtre native `0x2E` jouée depuis `FlushPending` (hors frame ImGui) |
| `src/ui/mob_sprite.{h,cc}` | brique partagée « .spr/.act → ImGui », **tous** les calques (login_parade n'en dessine qu'un) |
| — géométrie | `Plane.FromLayer` de GRF Editor fait foi : centré → échelle → **rotation** → offset (l'offset n'est PAS mis à l'échelle). Rotation ignorée = ailes décrochées du corps (Chonchon) et cadrage décentré ; d'où `AddImageQuad` et une boîte englobante calculée sur les 4 coins |
| — cadence | `Act_GetFrameDelay` (`0x0070F3D0`) → ticks de **25 ms** (`AnimationSpeed * 25`, cf. `GRF/FileFormats/ActFormat/Action.cs`). Une constante en dur était trop lente |
| `moonlight/src/map/clif.cpp` | `clif_parse_bourgeon_reqmobinfo` |
| `moonlight/src/map/packets_struct.hpp` | les deux structures + leur documentation de champs |

Réglages persistés (`bourgeon_settings.yaml`) : `monsterinfo_imgui` (basculé en GROUPE
par `SetModernInterface`, jamais isolément — la fenêtre revendique un paquet natif),
`monsterinfo_animate` (défaut ON), `monsterinfo_guardians` (défaut OFF, cf. §5).

---

## 8. Annuaire des adresses (client 20250716, base `0x400000`)

| Adresse | Symbole | Note |
|---|---|---|
| `0x0103074C` | COL de `UIMonsterInfoWnd` | |
| `0x01030750` | vtable de `UIMonsterInfoWnd` | |
| `0x0086C3C0` | `UIMonsterInfoWnd::ctor` | `new(0xF0)` |
| `0x0086E730` | `UIMonsterInfoWnd::dtor` | |
| `0x00877C40` | `UIMonsterInfoWnd::OnCreate` | vtable `+0x3C` |
| `0x0087F7C0` | `UIMonsterInfoWnd::OnRender` | vtable `+0x50` |
| `0x00880CC0` | `UIMonsterInfoWnd::OnMouseMove` | vtable `+0x70` |
| `0x00885C80` | `UIMonsterInfoWnd::OnMsg` | vtable `+0x94` |
| `0x00872B70` | `UIMonsterInfoWnd::DrawResistCell` | `(x, y, libellé, valeur, cadre, fond)` |
| `0x00CA5E4D` | handler inline `ZC_MONSTER_INFO` | dans `RecvLoop_DispatchPackets` |
| `0x00CE4320` | copie morte du handler | **sans référence** |
| `0x00C8DE46` | construction de `CZ_USE_SKILL` | écrit `g_SenseTargetGID` si skill 93 |
| `0x015FFFB8` | `g_SenseTargetGID` | `u32`, GID de la dernière cible de Sense |
| `0x015E8198` | tampon de réception | `+2` = `class_` |
| `0x0104F0A8` | table des clés `msgstringtable` | indexée par id |
| `0x00A1DC80` | `UIWindow_DrawActSprite` | `(this, spr, act, x, y, w, h, periodMs, fit, overlay)` |
| `0x00A21F90` | troncature de texte à N px + ellipse | |
| `0x00D5BB40` | `Job_GetDisplayNameOrResName` | `(ctx, classId, sex)` |
| `0x00D824C0` | `Mob_ClassIdToResName` | `__stdcall`, celui utilisé par `login_parade` |
| `0x0103181C` | `"몬스터\%s.spr"` | CP949 |
| `0x0103182C` | `"몬스터\%s.act"` | CP949 |

---

## 9. Références croisées

* [[reference_native_window_toggle_router]] — règle « détruire, pas masquer », et
  l'exception des fenêtres qui émettent un paquet en mourant.
* [[feedback_replaced_handler_hidden_duties]] — la grille des devoirs cachés, appliquée
  en §7.1.
* [[reference_native_packet_len_resolver]] — ce qui rend `RegisterReplaceOpcode` utilisable
  sur un paquet de longueur fixe comme `0x018C`.
* [[project_sprite_rendering_re]] et `docs/sprite_rendering_re.md` — la chaîne SPR/ACT.
* `src/features/overlays/login_parade.cc` — implémentation ImGui de référence pour
  l'affichage d'un sprite de monstre.
