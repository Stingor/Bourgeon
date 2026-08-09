# Barre d'incantation — RE de `UIRechargeGage` et remplacement Bourgeon

Client **20250716** (`Moonlight-Destiny.exe`, imagebase `0x400000`, pas d'ASLR :
les adresses de l'IDB sont celles du processus vivant).

La barre qui se remplit au-dessus d'une entité pendant qu'elle lance un sort.
Document frère de [`entity_chat_balloon_re.md`](entity_chat_balloon_re.md), dont
la §8 recense toute la famille des widgets « au-dessus de la tête ».

> **En une phrase.** Le widget natif est un `UIRechargeGage` de **60×6 pixels**
> qui ne peint que **trois rectangles plats** — ni nom de compétence, ni durée —
> mais l'acteur qui le porte garde, lui, les **deux horodatages** qui décrivent
> entièrement l'incantation. Tout le reste en découle.

---

## 1. La classe

| | |
|---|---|
| Nom RTTI | **`UIRechargeGage`** (dérive de `UIGage`, puis de `UIWindow`) |
| vftable | `0x0102bbc8` |
| Taille | **0xA4** octets |
| Constructeur | `UIRechargeGage_ctor` `0x008366c0` |
| Destructeur | `UIRechargeGage_scalar_dtor` `0x0083c100` |
| Paint | `UIRechargeGage_Paint` `0x00855910` (slot de vtable **20**, offset `0x50`) |
| Progression | `UIRechargeGage_SetProgress` `0x008637b0` |

Champs propres :

| Offset | Rôle |
|---|---|
| `+0x7C` | couleur de **fond** (passée à `ClearSurface`) |
| `+0x80` | couleur de **cuvette** |
| `+0x84` | couleur de **remplissage** |
| `+0x9C` | progression **courante** (init 0) |
| `+0xA0` | progression **max** (init 100) |

🔴 **Un seul site de création dans tout le binaire** — le cas msg 82 de
`Actor_OnMsg_AppearanceEffects` (`0x00c4d955`) — et un seul destructeur. Un
détour posé sur cette classe ne peut donc toucher **que** la barre de cast. C'est
la différence avec `UIBalloonText_SetTextWrapped`, partagé avec les infobulles,
qui obligeait la bulle de chat à discriminer sur l'acteur propriétaire.

---

## 2. Le rendu — trois rectangles, rien d'autre

`UIRechargeGage_Paint 0x00855910`, en entier :

```
UIWindow_ClearSurface(this, +0x7C)                                  ← fond
UIWindow_FillRectClamped(1, 1, w-2, h-2, +0x80)                     ← cuvette
pct = 100 * (+0x9C) / (+0xA0)
si pct != 0 :
    UIWindow_FillRectClamped(1, 1, w*pct/100 - 2, h-2, +0x84)       ← remplissage
```

`UIWindow_FillRectClamped 0x00a1d460` borne le rectangle aux dimensions de la
surface (`+0x14` largeur, `+0x18` hauteur) puis délègue à `surface->vtable+44`.

**Aucun bitmap, aucun sprite, aucun 9-slice, aucun texte.** Le client n'affiche
ni le nom de la compétence ni le temps restant.

---

## 3. Naissance — le message 82

`Effect_ApplySkillCastVisual 0x00cee6e0` est la source unique :

```c
if (skillId == 62 || skillId == 57 || duree == 0)
    /* effet seul, PAS de barre */
else
    OnMsg(acteur, /*msg*/ 82, /*p4*/ duree);
```

Deux conséquences utiles :

- **cast instantané (`duree == 0`) ⇒ aucune barre** — inutile de filtrer nous-mêmes ;
- **les compétences 57 et 62 n'en ont jamais**, en dur.

🔴 **Le `skillId` n'est PAS transmis au widget.** Le msg 82 ne porte que la
durée. C'est le point qui décide de toute l'architecture du remplacement : pour
nommer une incantation, il faut capter le paquet soi-même (cf. §5).

Le cas msg 82, à `0x00c4d955` :

```
si acteur+0x270 != 0        -> on saute la création (un second cast recycle la fenêtre)
sinon :
    operator_new(0xA4) -> UIRechargeGage_ctor -> acteur+0x270
    UIWindow_SetSize(60, 6)                        <<< 60×6 PIXELS
    vtable+0x10 Move(acteur+0xAC - 30,
                     (float)acteur+0xB0 - g_OverheadWidgetYScale2 * acteur+0x5C)
    UIWindowMgr_AddWindowToList(0x0131f4e8, fenêtre)
dans les deux cas :
    acteur+0x280 = timeGetTime() + duree     (FIN)
    acteur+0x284 = timeGetTime()             (DÉBUT)
```

⚠ `g_OverheadWidgetYScale2 0x015e5b50` est un **doublon** de
`g_OverheadWidgetYScale 0x015e5b48` : même formule `81 × hauteurÉcran / 480`,
même patron de magic static, sa propre garde (`0x015e5b54`). Comme son jumeau, il
est calculé **une seule fois** et ne suit donc pas un changement de résolution en
cours de session.

---

## 4. Vie et mort — `CActorSprite_UpdateOverheadWidgets 0x00c46680`

Chaque frame, pour l'acteur dont `+0x270` est non nul :

```c
si (timeGetTime() <= acteur+0x280) {
    Move(acteur+0xAC - 30, acteur+0xB0 - échelle * acteur+0x5C);
    total   = acteur+0x280 - acteur+0x284;
    écoulé  = min(timeGetTime() - acteur+0x284, total);
    UIRechargeGage_SetProgress(écoulé, total);
} else {
    OnMsg(acteur, 83);                       // retire la barre
    si (acteur+0x70 == 8) vtable+60(0, 1);   // ⚠ devoir CACHÉ du msg 83
}
```

🔴 **C'est pour ce `vtable+60` que Bourgeon ne détruit PAS la fenêtre.** On ne
sait pas ce que fait cette branche ; la supprimer reviendrait à reprendre les
devoirs du msg 83 sans les connaître (cf. la règle
« remplacer le natif = combler ses manques »). On **masque** la fenêtre par son
drapeau natif `+0x28` (`uiwnd::SetVisible`) et toute la machinerie de temps du
client continue de tourner — c'est elle qui nous alimente.

Que le masquage suffise se lit dans `UIWindow_Render 0x00a1ce10` : la boucle
d'enfants ne rend que ceux dont `+0x28 == 1`. (Le `+0x58` voisin est le drapeau
« sale » que lève `UIWindow_PaintDispatch`.)

---

## 5. Les paquets — d'où vient le nom de la compétence

Trois opcodes, repérés par leurs cas dans `RecvLoop_DispatchPackets` :

| Opcode | Cas | Site | Décodeur |
|---|---|---|---|
| **`0x013E`** `ZC_USESKILL_ACK` | 318 | `0x00ca4a41` | `SkillCast_DispatchVisualByType(pkt, 0)` |
| **`0x07FB`** `ZC_USESKILL_ACK2` | 2043 | `0x00ca4a54` | `SkillCast_DispatchVisualByType(pkt, 1)` |
| **`0x0B1A`** | 2842 | `0x00ca4a65` | `SkillCast_ApplyVisualFromPacket` `0x00cf9970` |

**Même disposition dans les trois cas** (offsets depuis l'opcode) :

| Offset | Champ |
|---|---|
| `+0x02` | srcGID (dword) |
| `+0x06` | dstGID (dword) |
| `+0x0A` | x (u16) |
| `+0x0C` | y (u16) |
| **`+0x0E`** | **skillId (u16)** |
| `+0x10` | property (dword) |
| **`+0x14`** | **durée d'incantation en ms (dword)** |
| `+0x18` | drapeau (octet ; absent de `0x013E`) |

> ⚠ Une note de RE de juillet 2026 plaçait le `skillId` en `+0x0A`. **C'est faux**
> — `SkillCast_DispatchVisualByType` le lit bien en `+0x0E`, et la disposition
> correspond exactement à `PACKET_ZC_USESKILL_ACK2` (25 octets).

Le nom se résout ensuite par `Lua_GetSkillName 0x0073a1f0`
(`char* GetSkillName(int id)`, `« Unknown-Skill »` si l'id est inconnu) — la même
source que l'arbre de compétences et l'infobulle native.

---

## 6. Le remplacement Bourgeon — `src/features/overlays/cast_bar.{h,cc}`

**Pas de détour de fonction.** Le constructeur ne fait qu'observer les trois
opcodes ; tout le reste se lit sur les acteurs.

**Battement par frame — `Bourgeon::OnGameFrame`, pas `OnRenderUI`.** Même leçon
que pour les bulles : `OnRenderUI` arrive **après** que le jeu a dessiné, donc y
masquer une fenêtre native la laisse visible une frame entière.

Ce que fait le battement (`CastBar::SyncWithActors`, sous `__try`) :

1. parcourt la `std::list<Actor*>` **puis** le joueur local — 🔴 qui n'y figure
   pas, il vit à `actorMgr+0x2C` ;
2. écrit `+0x28` **dans les deux sens** sur chaque `UIRechargeGage` rencontrée.
   Pas de comptabilité de ce qui a été masqué : décocher un réglage doit rendre
   sa barre au client tout de suite, et remettre `visible` sur une fenêtre déjà
   visible ne coûte qu'un stockage — là où une liste oublierait toujours un cas ;
3. relève l'instantané de **notre** incantation pour la barre HUD (§7), parce que
   `BasicInfo` dessine dans une frame ImGui sans garde d'exception et ne doit pas
   déréférencer l'acteur lui-même.

Le dessin (`OnRenderUI`) recalcule tout depuis les acteurs, sur
`ImGui::GetBackgroundDrawList()` — derrière toutes nos fenêtres. L'ancrage est
celui du natif : `y = yPieds − échelle × hauteurSprite`, la barre poussant vers
le bas ; l'étiquette se pose juste au-dessus.

**Appariement du nom.** Une entrée `GID → nom` n'est retenue que si son
horodatage tombe à moins de **1,5 s** du `+0x284` observé. Sans cette garde, un
paquet vieux de trente secondes nommerait la barre d'un sort suivant dont on
aurait raté le paquet — une erreur affichée avec aplomb, pire qu'une barre
anonyme.

**Réglages** : remplacement on/off · filtres joueurs / monstres / PNJ · nom ·
temps restant · bordure · largeur · hauteur · décalage vertical · arrondi ·
taille du texte · opacité · trois couleurs (fond, remplissage, remplissage
**monstres**) · masquer la sienne au-dessus de sa tête.

⚠ Les filtres disent ce qu'on veut **voir** : le masquage du natif, lui, reste
global dès que le remplacement est actif. Décocher « Monstres » ne rend pas au
client ses barres de monstres.

---

## 7. La barre HUD — septième barre de `BasicInfo`

`BasicInfo::kCast` rejoint HP/SP/EXP/Zeny/Poids : même déplacement, même
redimensionnement, même verrou, même aimantation, même grille, même persistance
(`expbar_cast_*`, générée depuis `kBarKeys`). C'est précisément la raison de la
loger là plutôt que dans le plugin.

Deux écarts, assumés :

- elle ne lit **aucune globale** (son entrée dans `kSrc` porte deux adresses
  nulles, jamais déréférencées) : ses valeurs viennent de `CastBar::own_cast()` ;
- elle **ne se dessine pas** hors incantation — une jauge vide en permanence
  prendrait la place sans rien dire.

Son texte passe par le nouveau paramètre `label_override` de `DrawBar`, qui
court-circuite `text_mode_` : « Storm Gust 1,4 s » plutôt que « Cast 42,00 % ».

---

## 8. Ce qu'on ne peut PAS afficher

⛔ **Le délai d'après-incantation** (`canact_tick` côté serveur) **n'est jamais
communiqué au client** — c'est déjà consigné dans
`src/features/gameplay/quick_cast.h`. Un second segment « délai » sur la barre
serait donc une invention. Ne pas le promettre.

---

## 9. Table d'adresses

| Adresse | Nom | Rôle |
|---|---|---|
| `0x0102bbc8` | `UIRechargeGage::vftable` | la classe de la barre |
| `0x008366c0` | `UIRechargeGage_ctor` | 0xA4 o, pose `+0x9C=0` / `+0xA0=100` |
| `0x00855910` | `UIRechargeGage_Paint` | trois rectangles plats |
| `0x008637b0` | `UIRechargeGage_SetProgress` | `+0x9C`/`+0xA0` puis repeinte |
| `0x0083c100` | `UIRechargeGage_scalar_dtor` | `UIRechargeGage` → `UIGage` → `UIWindow` |
| `0x00a1d460` | `UIWindow_FillRectClamped` | rectangle plein borné à la surface |
| `0x00a1ce10` | `UIWindow_Render` | ne rend un enfant que si `+0x28 == 1` |
| `0x00c4d955` | *(cas msg 82)* | création, `SetSize(60,6)`, horodatages |
| `0x00c46680` | `CActorSprite_UpdateOverheadWidgets` | progression par frame, msg 83 |
| `0x00cee6e0` | `Effect_ApplySkillCastVisual` | envoie le msg 82 (durée seule) |
| `0x00d1c480` | `SkillCast_DispatchVisualByType` | `0x013E` (type 0) / `0x07FB` (type 1) |
| `0x00cf9970` | `SkillCast_ApplyVisualFromPacket` | `0x0B1A` |
| `0x0073a1f0` | `Lua_GetSkillName` | `char* GetSkillName(int id)` |
| `0x015e5b48` | `g_OverheadWidgetYScale` | `81 × H / 480`, figé à la 1re frame |
| `0x015e5b50` | `g_OverheadWidgetYScale2` | **doublon** du précédent, garde à `+4` |

Champs d'acteur : `+0x5C` hauteur du sprite (float) · `+0xAC`/`+0xB0` position
écran des pieds · `+0x110` AID/GID · `+0x25C` job de base ·
**`+0x270`** la barre · **`+0x280`** fin · **`+0x284`** début.
