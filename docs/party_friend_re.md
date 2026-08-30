# Fenêtre Amis / Groupe — rétro-ingénierie

Client `2025-07-16_Ragexe`. Cible : porter la fenêtre en ImGui (`features/windows/`).
Cette page ne consigne que ce qui a été **mesuré** ; ce qui reste déduit est marqué comme tel.

> ⚠ Deux notes de la première passe de RE étaient **fausses** et ont été corrigées ici
> (voir « Erreurs corrigées » en fin de page). Ne pas les réintroduire depuis d'anciennes notes.

## 1. Une seule classe, deux onglets

`UIMessengerGroupWnd`, window id **0x45**, vtable `0x01010e2c`.

| Fonction | Adresse | Slot |
|---|---|---|
| ctor | `0x00701fc0` | |
| dtor | `0x00702450` | vt+0x00 |
| OnCreate | `0x00702970` | vt+0x3c |
| DrawContent | `0x00703d10` | vt+0x50 |
| OnMsg | `0x00705cc0` | vt+0x94 |
| GetEntryInfoAt | `0x00702880` | |
| UpdateMemberHpGauges | `0x00705b40` | |
| LayoutTopButtons | `0x00705980` | |
| OnLButtonDown | `0x007050a0` | |

### Le mode (`this+0x28C`) ne vaut que 0 ou 1

- **0 = AMIS**, **1 = GROUPE**. `OnCreate` l'initialise à **1** (`0x00702d17`).
- La seule écriture dynamique est le case **0xd7** d'`OnMsg` (`0x00707552`) : il boucle sur
  `this+0xBC` → `this+0xC4` — **exactement deux** onglets — et range l'indice de boucle dans
  le champ. Le mode ne peut donc structurellement pas dépasser 1.
- Mesure : sur les 17 fonctions de la classe, `+0x28C` apparaît 35 fois et n'est **jamais**
  comparé à autre chose que 0 ou 1.
- `DrawContent` n'a que deux branches : `mode == 1` → groupe, sinon → amis.

🔴 Il n'existe **pas** de troisième mode. (L'ancienne note « 2 = liste de sorts » venait du faux
nom `g_SkillInfoMgr` donné au manager, qui n'est qu'un sac d'état d'interface partagé.)

## 2. La source des données : le manager de session, pas la fenêtre

`g_UIWindowContextKey` = **`0x015FA3C0`** (le même objet que partout ailleurs dans Bourgeon).

| Champ | Contenu |
|---|---|
| `+0x0714` | table « member info », indexée par AID, éléments de 12 o |
| `+0x17BC` | liste chaînée circulaire des **membres du groupe** |
| `+0x17C0` | nombre de membres |
| `+0x17C4` | liste chaînée circulaire des **amis** |
| `+0x17C8` | nombre d'amis |
| `+0x54A4` | `std::string` nom du groupe (effacée au départ du groupe) |

Accesseurs (tous `__thiscall`, `ecx` = le manager) :

| Adresse | Rôle |
|---|---|
| `0x00d5cf50` | `Social_GetPartyMemberCount()` → `*(mgr+0x17C0)` |
| `0x00d5da80` | `Social_GetPartyMemberByIndex(out, index)` |
| `0x00d5ce20` | `Social_GetFriendCount()` → `*(mgr+0x17C8)` |
| `0x00d5a0d0` | `Social_GetFriendByIndex(out, index)` |
| `0x00d5d740` | getter membre **par clé `+0x08`** (renvoie une entrée vide si absent) |
| `0x00d5c850` | `Social_GetMemberInfoById(out, aid)` → 12 o depuis `mgr+0x714` |
| `0x00d5bcf0` | `Social_GetMapDisplayName(mapname)` |
| `0x00d5bb40` | `Job_GetDisplayNameOrResName(jobid, 99)` |

🔴 **Point décisif pour le portage.** `DrawContent` appelle ces accesseurs **à chaque frame**,
directement sur le manager. Sa propre liste (`this+0xFC`) n'est qu'un miroir reconstruit au
msg 0x17. Détruire la fenêtre native n'assèche donc **rien** : le manager est alimenté par les
handlers de paquets, qu'on ne touche pas. La fenêtre ImGui lira exactement la même source, au
même endroit — contrairement au cas `trade_window`, où le remplacement des handlers d'opcodes
avait vidé les tableaux globaux et obligé à reconstruire l'état depuis les paquets.

### L'entrée sociale (0x50 octets)

Même type des deux côtés (amis et groupe) ; copy-ctor `sub_701DF0` @ `0x00701df0`. Les listes
sont circulaires, la **donnée est à `nœud+8`**.

| Offset | Type | Contenu | Comment c'est établi |
|---|---|---|---|
| +0x00 | u32 | validité / type | testé non nul avant usage (`DrawContent`, `UpdateMemberHpGauges`) |
| +0x04 | u32 | **GID / AID** | comparé à `g_Account_Aid` ; passé à `ActorMgr_FindByGidOrSelf` |
| +0x08 | u32 | second id (char id — **déduit**) | clé du getter `0x00d5d740` ; passé à `ChatItemLinkInfoCache_Get` |
| +0x0C | `std::string` | **nom** | size/cap lus en +0x1C/+0x20 |
| +0x24 | `std::string` | **nom de map** | passée à `Social_GetMapDisplayName` |
| +0x3C | u32 | **chef — 0 = chef** | `ico_partyCrown.bmp` dessinée si `== 0` |
| +0x40 | u32 | **hors-ligne** | `icon_party_off.bmp` + texte grisé + jauge masquée |
| +0x44 | u32 | couleur du nom | passée telle quelle à `UIWindow_DrawText` |
| +0x48 | u16 | **job** | `Job_GetDisplayNameOrResName` |
| +0x4A | u16 | **niveau** | `movzx eax, word ptr [esi+52h]` poussé dans `"Lv.%d"` @`0x0070433d` |
| +0x4C | u32 | non identifié | |

Le niveau et les PV passent par un `sprintf` **variadique**, dont le décompilateur ne montre pas
les arguments — d'où le passage par le désassemblage, qui tranche les deux :

- **niveau** = `[nœud+0x52]` = `data+0x4A` (u16), site `0x0070433d` ;
- **PV** = lus sur la **jauge enfant**, pas sur l'entrée : `mov eax, [edi+eax*4+128h]` (donc
  `this+0x128[i]`, les 40 `UIPcGage`), puis `push [eax+0A4h]` / `push [eax+0A0h]` dans `"%d/%d"`
  @`0x00704622`. Ce sont les MÊMES offsets `+0xA0`/`+0xA4` que ceux lus sur l'acteur — notre
  chemin (acteur `+0x488` → `+0xA0`/`+0xA4`) donne donc exactement la même donnée sans dépendre
  d'une jauge native. Le garde `cmp [eax+1Ch], -100` confirme au passage qu'une jauge parquée
  hors écran = pas de texte de PV.

## 3. Le HP des membres vient de l'ACTEUR, pas du réseau

`UpdateMemberHpGauges` (`0x00705b40`) :

```
if (entry[+0x04] == g_Account_Aid)  pc = gameMode[11];               // moi
else pc = dynamic_cast<CPc*>(ActorList_FindByGID(gameMode+204, aid)); // un autre
if (!pc || entry[+0x40])  → jauge déplacée en (-100, -100)            // masquée
```

🔴 Conséquence : **pas d'acteur chargé ⇒ pas de HP**. Le client natif ne peut pas afficher le HP
d'un membre hors de portée, parce qu'il le lit sur le `CPc` de l'acteur et nulle part ailleurs.
C'est une limite du **client**, pas de la fenêtre : la reproduire à l'identique en ImGui
reproduirait le trou.

⇒ Piste d'amélioration (« combler, pas reproduire ») : écouter **`ZC_NOTIFY_HP_TO_GROUPM`
(0x0106)**, que le serveur envoie pour les membres hors écran, et tenir notre propre cache de HP.
C'est le seul endroit identifié où le paquet apporte ce que le natif ne stocke pas.

## 3 bis. 🔴 `g_Own_InParty` n'est PAS fiable

`0x015FF804` (dword). Le switch natif s'en sert comme garde avant « quitter » (case 0x3D
@`0x00c8d48a`) et « expulser » (case 0x3E @`0x00c8d627`), et une quinzaine de fenêtres le
testent. Il serait donc tentant d'y lire « suis-je en groupe ? ».

**Mesuré en jeu le 2026-08-23 : il reste à 0 pour un membre qui a REJOINT un groupe** — un
bouton conditionné par ce drapeau n'apparaît jamais pour un simple membre. Six sites l'écrivent
(`0x00cb6146`, `0x00ced926`, `0x00cf2790` posent 1 ; `0x00cba855`, `0x00cbb6e4`, `0x00cbfbdd`
écrivent une valeur calculée) ; lequel devrait couvrir la jonction, et pourquoi il ne se
déclenche pas sur Moonlight, n'est **pas tranché**.

⇒ Pour savoir si l'on est en groupe, utiliser le **nombre de membres**
(`Social_GetPartyMemberCount`, ou la liste elle-même) : c'est la source qui alimente l'affichage,
donc l'interface reste cohérente avec ce que le joueur voit.

## 4. Cycle de vie

- `sub_D56530` (`0x00d56530`) — **départ du groupe** : vide `+0x17BC`, remet le compteur à 0,
  efface le nom du groupe, et retire l'effet **160** de chaque acteur membre.
- `sub_D70220` (`0x00d70220`) — **reset de session** (char-select) : vide les deux listes et
  tous les compteurs, même retrait d'effet. C'est le point de purge à respecter côté ImGui.

## 5. Assets et libellés

Dossier `\renewalparty\` (sous la racine d'interface CP949 `유저인터페이스\`) :
`bg_partymember.bmp`, `ico_partyCrown.bmp`, `icon_party_me/on/off.bmp`,
`img_friend1.bmp`, `img_friend2.bmp`.

**L'icône de classe**, celle qui occupe la gauche de chaque ligne :

```
sprintf("%sicon_jobs_%d.bmp", "\renewalparty\", job)   @0x0070622a  (msg 0x17)
       variante "_die" quand le membre est mort
movzx edx, word ptr [esi+50h]      ← nœud+0x50 = data+0x48, le JOB
UIBitmapButton_SetStateBitmap(bouton, chemin, état 0/1/2)  ← même image aux 3 états
```

Le `%d` est donc le **job id brut** de l'entrée. ⚠ Le flag qui déclenche la variante `_die` est
distinct de « hors ligne » (`+0x40`) et n'est **pas** identifié.

**Le nom de classe** : `Job_GetDisplayNameOrResName(mgr, job, sex)` @`0x00d5bb40`. Malgré son
nom, elle rend un **nom affichable** pris dans une table `mgr+0xF88` indexée par classe. Le 3e
paramètre est un SEXE : `-1` = celui du joueur courant, et la valeur `99` que passe cette fenêtre
ne vaut ni 0 ni 1, donc le code retombe sur le nom de la **classe de base**, sans variante de
sexe. Le natif pose ce nom sur le bouton d'icône (`UITextButton_SetName`), pas dans la ligne.
Libellés d'onglets : msgstring **0x66** et **0x67** (`0x67` est déjà utilisé par le bouton
« party » du menu d'icônes, cf. `features/overlays/menu_icons.cc`). Compteur bas de fenêtre :
msgstring **0xC9F** suivi de `"%d/%d"`.

## 6. Les actions du hub `OnMsg`

Relevé exhaustif des appels du hub (`0x00705cc0`, 0x2181 o). Trois familles :

**a) Commandes `CMode::SendMsg`** (`g_pCurrentMode`, vt+0x18) — seulement quatre sites :

| Commande | Args | Site | Paquet émis | Sens |
|---|---|---|---|---|
| `0x104` | 1 (AID) | `0x00706ea3` | **CZ 0x07DA** | passer chef |
| `0x0B0` | 2 (AID, CID) | `0x00707379` | **CZ 0x0203** | retirer un ami |
| `0x03D` | 0 | `0x007074e6`, `0x0070790c` | **CZ 0x0100** | quitter le groupe |

Vérifié de façon **non circulaire** contre Hercules (`src/map/packets.h`), qui donne la même
forme d'arguments que le désassemblage :

```
packet(0x0100, clif->pLeaveParty, 0);            // aucun argument
packet(0x0203, clif->pFriendsListRemove, 2, 6);  // AID @2, CID @6
packet(0x07da, clif->pPartyChangeLeader, 2);     // AID @2
```

🔴 **`0x3D` n'est pas un simple envoi** (`0x00c8d44e`). Avant de quitter, il :
1. boucle sur les membres et cherche un autre membre **en ligne** (`+0x40 == 0`) dont la **map
   est la mienne** (comparaison de la string `+0x24`) ;
2. si un tel membre existe, lui **transfère le leadership** — `SendMsg(0x104, membre+0x04)` ;
3. sinon, affiche la modale msgstring **0xCB9** et n'abandonne que si la réponse ≠ 0xBB ;
4. ferme la fenêtre 0xD0 si elle est ouverte, puis envoie **CZ 0x0100**.

Reproduire l'étape 2 est indispensable : sans elle, un chef qui quitte laisse le groupe sans
chef alors que le client officiel passe la main.

**a bis) L'EXPULSION n'est pas dans le hub** — elle passe par une commande à part :

| Commande | Site | Paquet | Forme |
|---|---|---|---|
| `CMode::SendMsg` **0x3E** (62) | `0x00c8d627` | **CZ 0x0103** | `{opcode:2, AID:4 @+2, nom[24] @+6}` = 30 o |

Gardée par `cmp g_Own_InParty, 0`. L'AID est résolu par `sub_D5D960` juste avant l'envoi, le nom
copié par `strncpy(dst, src, 0x18)`. Confirmé par Hercules : `packet(0x0103,
clif->pRemovePartyMember, 2, 6)`.

**a ter) La CRÉATION d'un groupe** — encore une commande à part :

| Commande | Site | Paquet | Forme |
|---|---|---|---|
| `CMode::SendMsg` **0xA8** (168) | `0x00c8d066` | **CZ 0x01E8** | `{opcode:2, nom[24] @+2, exp:1 @+26, item:1 @+27}` = 28 o |

Le nom vient d'une globale (`0x015FF84C`) et les deux drapeaux de partage des paramètres de la
commande. Hercules : `packet(0x01e8, clif->pCreateParty2, 2)`.

**a quater) Inviter, répondre, se lier d'amitié** — toutes vérifiées contre Hercules :

| Commande | Site | Paquet | Forme |
|---|---|---|---|
| `SendMsg` **0x3B** (59) | `0x00c8d0d5` | **CZ 0x02C4** | `{nom[24] @+2}` — invitation PAR NOM |
| `SendMsg` **0x3C** (60) | `0x00c8d244` | **CZ 0x02C7** | `{partyid @+2, réponse @+6}` |
| `SendMsg` **0xAF** (175) | `0x00c90714` | **CZ 0x0208** | `{AID @+2, CID @+6, réponse @+10}` |
| `FriendList_AddByName` | `0x00a2c600` | **CZ 0x0202** | `{nom[24] @+2}` — ⚠ lit **24 octets d'affilée** |

```
packet(0x02c4, clif->pPartyInvite2, 2);
packet(0x02c7, clif->pReplyPartyInvite2, 2, 6);
packet(0x0208, clif->pFriendsListReply, 2, 6, 10);
packet(0x0202, clif->pFriendsListAdd, 2);
```

🔴 Le case 175 (accepter un ami) **ouvre la fenêtre lui-même** : `MakeWindow(0x22)` puis
`OnMsg(6, 0xD7, 1)` pour se placer sur l'onglet Amis. C'est un troisième chemin qui fabrique la
0x22 sans geste du joueur — d'où l'importance de ne pas basculer sur ces appels-là (cf. §7).

**a quinquies) Les RÉGLAGES du groupe** :

| Commande | Site | Paquet | Forme |
|---|---|---|---|
| `SendMsg` **0x103** (259) | `0x00c910d4` | **CZ 0x07D7** | `{exp:4 @+2, pickup:1 @+6, share:1 @+7}` |
| `SendMsg` **0x60** (96) | `0x00c8e6e3` | **CZ 0x0102** | `{exp:4 @+2}` — variante exp seul |

L'état courant vit dans trois globales, écrites par les handlers de paquets :

| Globale | Clé msgstring des radios | Sens |
|---|---|---|
| `0x015FF840` | `MSI_EXPDIV` (0x11F / 0x120) | partage d'**EXP** |
| `0x015FF844` | `MSI_ITEMCOLLECT` (0x121 / 0x122) | **ramassage** |
| `0x015FF848` | `MSI_ITEMDIV` (0x2E3 / 0x2E4) | **partage** des objets |

🔴 L'ordre a été tranché par les **clés** msgstring (que `OnCreate` crée dans cet ordre), pas
déduit : une note antérieure avait les deux dernières inversées. Il se recoupe avec l'appel natif
`SendMsg(0x103, this+0xBC, this+0xC8, this+0xD4)` (@`0x008c684e`), dont les trois arguments sont
comparés à ces mêmes globales, et avec le serveur.

⚠ Côté Moonlight, `clif_parse_PartyChangeOption` **exige d'être chef** et recompose
`itemflag = pickup | share<<1`. Un non-chef est ignoré en silence.

⚠ La CRÉATION native se fait en deux temps : case **0x11D** mémorise le nom saisi dans
`0x015FF84C` (ce n'est pas un renommage — il n'y a pas de renommage de groupe), puis case
**0xA8** le valide (`sub_A85BE0`) et envoie CZ 0x01E8.

**Les cases du menu contextuel** (le `case 0x30` route le choix vers `OnMsg(0xE7…0xEC)`) :

| case | rôle |
|---|---|
| 0xE8 | **chuchoter** — `ChatAction` action **0x0E** = `UIM_MAKE_WHISPER_WINDOW` |
| 0xEB | **retirer un ami** — modale msgstring 0x164, puis `SendMsg 0xB0` |
| 0xEC | **quitter le groupe** — modale msgstring 0x165, puis `SendMsg 0x3D` |
| 0xE9 / 0xEA | dialogues (msgstring 0xBF ; `MakeWindow`) — non portés, cf. lot 2 |

🔴 `0xEB` et `0xEC` ouvrent une **modale native** (`UIWndMgr_ShowMessageBoxModal`). Une modale
déclenchée entre `NewFrame()` et `Render()` ne rend pas la main : toute action doit être ARMÉE
pendant la frame et exécutée depuis `OnProcessInput` (`FlushPending`), comme la chatbox et
l'échoppe joueur.

**b) Actions déléguées à d'autres fenêtres** — c'est par là que passent « inviter » et
« ajouter en ami » : `UIWindowMgr_MakeWindow` (×5) ouvre le dialogue, et c'est **le dialogue**
qui émet le CZ (ex. `UIPartyInvitationToWnd` → cmd 0x3b). Cela confirme le découpage en lots :
le lot 1 peut déléguer ces actions aux dialogues natifs, le lot 2 les portera.
`UIWndMgr_ShowMessageBoxModal` (×7) sert aux confirmations.

**c) Helpers directement réutilisables côté ImGui :**

| Fonction | Usage dans le hub |
|---|---|
| `UIWindowMgr_ChatAction` (×3) | ouvrir un whisper vers le membre |
| `FriendList_ContainsName` | griser « ajouter en ami » si déjà ami |
| `Guild_IsMemberAid` | savoir si la cible est de la guilde |
| `Aid_FormatObfuscated` | affichage de l'AID |
| `Own_GetCharName` (×4) | comparaison avec soi-même (interdit de s'inviter) |
| `UIMiniParty_SpawnMemberWnd` | détacher un membre en barre HUD (chantier séparé) |

## 7. 🔴 DEUX ids, et ils ne servent pas à la même chose

Le piège central de ce chantier. `UIMessengerGroupWnd` répond à **deux** ids :

| id | rôle | preuve |
|---|---|---|
| **0x22** (34) | **fabrique ET slot** | case 34 @`0x00a3aeac` — le seul endroit qui construise la classe : le ctor `0x00701fc0` n'a **qu'un appelant** dans tout le binaire, `UIWindowMgr_MakeWindow`. Il range l'objet dans **`mgr+0x2C8`** |
| **0x45** (69) | **point d'entrée** | aiguilleur d'ouverture (onglet) et de fermeture ; il ne possède rien |

### 🔴🔴 Ce que chaque id sait faire (MESURÉ, 2026-08-30 — la ligne précédente disait faux)

| opération | 69 (0x45) | 34 (0x22) |
|---|---|---|
| `FindWindow` | ❌ **rend TOUJOURS `nullptr`** — aucun case pour 69 dans `UIWindowMgr_FindWindow` `0x00a47b90` ; il retombe sur la map générique (`this+8`), où cette fenêtre n'est **jamais** insérée : le case 34 la range dans `mgr+0x2C8` et la pousse dans la liste de rendu (`sub_A2D220` → `mgr+380`) | ✅ `case 34` → `*(this + 178*4)` = **`mgr+0x2C8`**, exactement ce slot |
| `CloseWindow` | ✅ `case 69` @`0x00a2f387` : appelle `vt+0x2C` sur la fenêtre puis **délègue à `SaveRectAndCloseWindow(0x22)`** | ✅ `case 34` @`0x00a2f3ba` : `QueueDestroyWindow`, remet `mgr+0x2C8` à 0, puis ferme les satellites 53 et 68 |

⇒ **DÉTECTER avec 34, FERMER avec 69.**

🔴 Le bug que ça a coûté : `KillNative` gardait sa destruction derrière `FindWindow(0x45)`, qui ne
rend jamais rien. La native n'était donc **jamais détruite** — seulement masquée à la naissance.
Or « masquée » veut dire « existe », et toute bascule du client (`ToggleWindowById` `0x00812e60`,
`DispatchHotkeyBehavior` `0x00a451e0`) **ferme si ça existe et ne crée que sinon** : un appui sur
deux partait fermer une fenêtre invisible sans jamais atteindre notre `HandleNativeCreation`.
D'où « deux pressions pour ouvrir, deux pour fermer », et une native qui pouvait réapparaître.

⚠ La contradiction était **déjà dans cette page** : la note de bas de section signale que le
client lui-même cherche cette fenêtre par `FindWindow(0x22)` (`UIMessengerGroupWnd_OnMemberDrop`).
Une note qui contredit la table à trois lignes d'écart n'a pas suffi à faire relire la table.

Le case 69 n'est **pas** une fabrique — c'est un point d'entrée « ouvrir sur le bon onglet » :

```
case 69 @0x00a3ae55:
    cmp  [mgr+740h], 0
    push 22h ; call UIWindowMgr_MakeWindow    ← rappelle la VRAIE fabrique
    OnMsg(win, 6, 0xD7, 1 ou 2)               ← bascule d'onglet selon mgr+0x740
    jmp  default                              ← sort SANS retourner la fenêtre
```

⇒ Un hook de création posé sur 0x45 **ne voit jamais de fenêtre** : le case sort par le
`default`. Il faut accrocher **0x22**. Comme `MakeWindow(0x45)` rappelle `MakeWindow(0x22)`, un
hook sur la fabrique est réentrant et c'est la passe imbriquée qui porte l'objet.

⚠ Reste à vérifier en jeu : le case 34 ne construit que si **`mgr+0x2C8`** est nul. Il appelle
`SaveRectAndCloseWindow(0x45)` juste avant, ce qui devrait le remettre à zéro — si ce n'était pas
le cas, la fenêtre ne renaîtrait pas après la première destruction.

Note : c'est l'id `0x22` que `UIMessengerGroupWnd_OnMemberDrop` (0x00702500) cherche déjà par
`FindWindow` lors du glisser d'un membre — la coexistence des deux ids était donc visible dès la
première passe de RE, mais restée non tranchée.

## 8. Point d'accroche : tuer la native

Rien de spécifique à inventer — le motif du projet s'applique tel quel (cf.
`reference_native_window_toggle_router`, et l'implantation de `features/windows/cart_viewer.cc`) :

- **DÉTRUIRE, pas masquer.** Toute bascule du client fait « ferme si elle existe, sinon crée » :
  une native seulement masquée existe encore, donc la demande suivante la fermerait sans
  repasser par `MakeWindow` (un appui sur deux avalé) et elle garderait le clavier.
- L'API est déjà factorisée dans `ragnarok/uiwnd.h` : `uiwnd::FindWindow(0x45)` pour détecter,
  `uiwnd::CloseWindow(0x45)` pour détruire (sous `__try`), `uiwnd::MakeWindow` pour rendre la
  main au natif. `CloseWindow` pointe sur `0x00a2e770`, la même fonction que le hub natif
  utilise (`UIWindowMgr_SaveRectAndCloseWindow`).
- Respecter la barrière `Bourgeon::Instance().IsMapLoading()`.
- Si la native est présente au moment où l'on active le mode ImGui, **adopter son état ouvert**
  avant de la détruire, sinon la fenêtre disparaîtrait sous le joueur.
- Entrée côté joueur : le bouton « party » du menu d'icônes (id 0xC7,
  `features/overlays/menu_icons.cc`) et le raccourci natif correspondant.

## 8bis. 🔴 La fenêtre se rouvrait à chaque map — deux naissances, pas une (2026-08-30)

Symptôme : la fenêtre **se rouvrait à chaque changement de map**, quoi qu'ait fait le joueur —
sa fermeture ne « tenait » jamais.

### La garde qui manquait (le vrai correctif)

🔴🔴 **Un PRÉCÉDENT existait dans le projet et n'a pas été consulté** : le grimoire — et les cinq
autres natives de `CharacterSheet` — porte cette garde depuis longtemps, avec le même commentaire
(`CharacterSheet::HandleReplacedNativeCreation`) :

```cpp
if (Bourgeon::Instance().IsMapLoading()) return;   // après avoir masqué
```

Pendant un changement de map le **HUD natif est démonté puis RECONSTRUIT**, et le client rouvre au
passage les fenêtres qu'il croit ouvertes. `PartyFriendWindow::HandleNativeCreation` prenait cette
naissance pour une demande et posait `open_ = true`. `HandleReplacedNativeCreation` avait déjà été
corrigée pour ça ; la nôtre ne l'avait jamais été.

**La leçon** (déjà écrite dans `feedback_re_method` : « voir où ça MARCHE déjà ») : sur un symptôme
qu'un autre module du projet a déjà rencontré, lire d'abord ce module. Une passe de RE menée à la
place a produit une cause plausible, mesurée, réelle — et qui n'était pas celle-là.

### L'autre naissance, réelle mais insuffisante à elle seule

Raisonnement d'élimination : `open_` ne peut passer à vrai que par `HandleNativeCreation`, donc
une native `0x22` **naissait** à chaque map. Or les créateurs de `0x22` sont énumérables (le ctor
`0x00701fc0` n'a qu'un appelant, le case 34 de `MakeWindow`) :

| Site | Ce que c'est |
|---|---|
| `0x00a3ae62` | le case 69, point d'entrée du JOUEUR (imbriqué → `g_party_entry_depth`) |
| `0x00ca23db` | `ZC_ACK_MAKE_GROUP` résultat 0 (msgstring 0x4D) — création d'un groupe |
| `0x00c90725` | case 175 — acceptation d'un ami |
| `0x00c8d261` | case 60 |
| **`UIWindowMgr_RestoreWindowLayout` `0x00a4a930`** | **le seul lié à l'entrée sur map** |

`RestoreWindowLayout(blob)` (`__thiscall(mgr, blob)`) rejoue le layout mémorisé : entrées
`{id:2, taille:4}`, marqueur `20000`, fin `20001`, et pour chaque id dans `20100..20499` un
`MakeWindow(id - 20100)` suivi d'un `OnMsg(0, 123, …)`. Elle n'a **qu'un appelant dans tout le
binaire** : `CGameMode_EnterWorld` `0x00c733d0` (appel en `0x00c744b5`), qui tourne à chaque
entrée dans le monde — donc à **chaque changement de map**, pas seulement à la connexion.

🔴 **La barrière `IsMapLoading()` ne l'attrape pas** : la restauration s'exécute *après*
`GameMode_OnEnterMapSetup`, appelée plus haut dans la MÊME fonction, qui envoie
`CZ_NOTIFY_ACTORINIT` (0x007d) — c'est-à-dire le paquet sur lequel Bourgeon *lève* la barrière.
Une garde temporelle serait donc muette ; il faut le hook.

🔴 **La boucle s'entretenait seule** : rouverte à l'entrée sur map, la fenêtre était de nouveau
enregistrée « ouverte » dans le layout, qui la rouvrait à la map suivante. Aucune fermeture ne
pouvait survivre.

Correction : `features/patches/window_pos_tweaks.cc` détourne cette fonction pour poser
`g_layout_restore_depth` le temps de son exécution (même patron que `g_party_entry_depth`), et
`HandleNativeCreation` reçoit un troisième argument `layout_restore` — dans ce cas elle masque la
native (le tick la détruira) et **ne touche pas à `open_`**. Adresse : `uiwnd::kRestoreWindowLayoutAddr`.

⚠ Ce chemin concerne **toutes** les fenêtres dont on a détruit la native, pas seulement celle-ci :
un module qui bascule ou ouvre sur `HandleNativeCreation` sans consulter ce drapeau subira le même
rejeu à chaque map. Le drapeau est disponible pour eux le jour où le symptôme apparaît.

## 9. Erreurs corrigées de la première passe

| Ancienne note | Réalité mesurée |
|---|---|
| `+0x28C == 2` = « liste de sorts » | il n'y a que 2 modes (0 amis / 1 groupe) |
| `0x015FF908/90C` = « mes coords écran » | **`g_Own_Hp` / `g_Own_MaxHp`** |

Leçon : les deux venaient d'un **nom de symbole faux** (`g_SkillInfoMgr`) propagé sans mesure.
