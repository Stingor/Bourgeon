#pragma once

// ── CActorSprite : la structure d'une ENTITÉ à l'écran ───────────────────────
// (client 20250716, base 0x400000)
//
// Tout ce qui vit sur la carte — le joueur, les autres joueurs, les monstres,
// les PNJ, les familiers — est un `CActorSprite`. Les surcouches qui dessinent
// AU-DESSUS d'une entité (nom, bulle, barre d'incantation, cadre de cible) en
// lisent toutes les mêmes champs.
//
// ── Pourquoi cet en-tête ─────────────────────────────────────────────────────
// Ces offsets vivaient dans ONZE fichiers, sous QUATRE conventions de nommage —
// `kAct_*`, `kActor_*`, `kActorPos*`, `kOffActor*` — donc introuvables en
// cherchant l'un des noms. Relevé par valeur (`scratchpad/dup/konst.py`) :
//
//   0x10  position X       : 4 noms, 6 fichiers
//   0x110 AID/GID          : 3 noms, 7 fichiers
//   0x70  état de mouvement: 3 noms, 4 fichiers
//   0xac  X écran          : 1 nom,  4 fichiers
//   0x43c couleur de cheveux : écrite `0x43C` ici et `1084` EN DÉCIMAL là
//
// C'est le deuxième niveau d'aveuglement de `project_address_directory`, dans sa
// forme pure : la même valeur sous des noms différents. Aucun relevé de CORPS ne
// pouvait la voir.
//
// ⚠⚠ NE PAS CONFONDRE AVEC LES OFFSETS DE LA RESSOURCE `.act`.
// `char_diagnostics.cc` emploie le MÊME préfixe `kAct_` pour une TOUTE AUTRE
// structure — le fichier d'animation : `kAct_ActionsBegin = 0x110`,
// `kAct_SpeedsBegin = 0x12c`… Sa valeur 0x110 n'a rien à voir avec l'AID d'un
// acteur, qui vaut aussi 0x110. Le préfixe `rag::actor::` existe pour que cette
// collision ne puisse plus se produire en silence.
//
// ⚠ Rien ici n'appelle le natif : ce sont des LECTURES de mémoire. L'appelant
// pose son propre `__try` — un acteur détruit entre deux frames ne doit pas
// faire tomber le client.

#include <cstdint>

namespace rag::actor {

// La vtable de `CActorSprite`, pour qui doit reconnaître la classe d'un objet.
constexpr uintptr_t kVTable = 0x01094810;

// ── Position dans le MONDE (float) ──────────────────────────────────────────
// ⚠ Y est la HAUTEUR, Z la profondeur — convention du moteur, pas celle d'une
// carte vue de dessus. Un déplacement au sol change X et Z.
//
// ⚠⚠ RIEN À VOIR avec `uiwnd::kOffPosX` (+0x1c), qui est le x ÉCRAN d'une
// FENÊTRE. Deux structures, deux sens ; l'avertissement était écrit dans
// `player_jump.cc` et vaut pour tout le monde.
constexpr int kPosX = 0x10;
constexpr int kPosY = 0x14;  // hauteur (le saut la modifie)
constexpr int kPosZ = 0x18;

// Chemin de palette de CORPS, relatif à `palette\` (std::string du client).
// 🔴 Vide ⇒ AUCUNE palette externe : le sprite garde la sienne.
constexpr int kBodyPalettePath = 0x1c;

// ── Animation ────────────────────────────────────────────────────────────────
constexpr int kActionBase  = 0x34;  // CActorSprite_StartAction : action × 8
constexpr int kActionPlay   = 0x38;  // + la direction ; l'index LU dans le .act
constexpr int kFrameIndex   = 0x3c;  // CActorSprite_AdvanceAnimState
constexpr int kClothesColor = 0x48;  // 🔴 0 ⇒ aucune palette externe

// Angle en DEGRÉS dont le natif fait pivoter la flèche de la minimap. Écrit par
// `CActorSprite_SetFacingTowardXZ`, relu par la sélection d'animation, et lu par
// `GameMode_DrawMiniMapMarker` genre 4 (0x00c685c0).
//
// 🔴 On lit le MÊME champ que le natif plutôt que de chercher « l'angle de la
// caméra » : ce qui fait tourner sa flèche fera tourner la nôtre, quelle que
// soit la nature exacte de cet angle.
constexpr int kFacing = 0x4c;
constexpr int kFrameDelay   = 0x58;  // float — CActorSprite_SetFrameDelay
constexpr int kHeight       = 0x5c;  // float : facteur de hauteur du sprite
constexpr int kMotionFactor = 0x64;  // float — SetAttackMotionFactor

// État de mouvement. Trois lectures coexistaient, chacune ne connaissant que
// SES valeurs : 3 = mort (cadre de cible), 6 = pas de pathfinding (déplacement
// au clavier). Le champ est le même ; la liste de valeurs reste incomplète.
constexpr int kMotionState = 0x70;

constexpr int kAnimStart = 0x8c;  // timeGetTime du dernier changement d'action

// ── Projection ÉCRAN (int, recalculée à chaque frame) ────────────────────────
// L'ancre est aux PIEDS de l'acteur. C'est ce que toutes les surcouches suivent.
//
// La navigation `CGameMode -> gestionnaire d'acteurs -> acteur` n'est PAS ici :
// elle vit dans `ragnarok/game_scene.h`. Détail complet dans
// docs/entity_nameplate_re.md.
constexpr int kNameplate = 0xa5;  // octet : participe au nameplate (visible/vivant)
constexpr int kScreenX   = 0xac;
constexpr int kScreenY   = 0xb0;

// ── Ressources et identité ───────────────────────────────────────────────────
constexpr int kSprRes = 0x104;  // ressource .spr du CORPS
constexpr int kActRes = 0x108;  // ressource .act du CORPS (celle que l'anim consomme)

// L'identifiant réseau porté par l'acteur lui-même. AID pour un joueur, GID pour
// tout le reste — les deux noms circulaient, c'est le même champ.
constexpr int kGid = 0x110;

// 🔴 CHAMP AMBIGU, NON TRANCHÉ. Trois fichiers l'appellent « classe de BASE »
// (`kAct_BaseJob`), un quatrième « job AFFICHÉ, déguisement compris »
// (`kActor_JobShown`). Les deux ne peuvent pas être vrais en même temps : sur un
// joueur déguisé, l'un rendrait sa vraie classe et l'autre son apparence.
// Personne ne s'en est plaint, ce qui suggère que le cas ne s'est pas présenté —
// pas que la question soit réglée. Nom NEUTRE en attendant la mesure.
constexpr int kJobId = 0x25c;

constexpr int kSex = 0x260;  // 0 = femme

// UITransBalloonText* — LA bulle de chat de cet acteur.
//
// ⚠ Nulle dès que `ChatBalloon` a pris la main : notre surcouche DÉTRUIT la
// fenêtre native. Un lecteur qui s'en sert pour savoir si un texte est déjà
// annoncé au-dessus de la tête doit donc traiter le nul comme « rien
// d'annoncé », pas comme « pas encore construit ».
constexpr int kBalloon = 0x264;

// ── Incantation ──────────────────────────────────────────────────────────────
// 🔴 Le trio, relevé par RE de `Actor_OnMsg_AppearanceEffects` (cas msg 82,
// 0x00c4d955) et de `CActorSprite_UpdateOverheadWidgets` (0x00c46680).
constexpr int kCastGage  = 0x270;  // UIRechargeGage* : la barre native
constexpr int kCastEnd   = 0x280;  // uint : timeGetTime de FIN
constexpr int kCastStart = 0x284;  // uint : timeGetTime de DÉBUT

// AID du MAÎTRE d'un familier, homoncule ou mercenaire. C'est par lui que le
// natif reconnaît « un compagnon à moi » (`0x00C787CC`).
constexpr int kOwnerAid = 0x2ec;

// ── Le masque d'OPTIONS (les OPTION_* du serveur) ────────────────────────────
// Témoin de l'indexation : dans le même pseudo-code, `*((_DWORD*)this + 68)`
// est le GID — soit +0x110, exactement `kGid` ci-dessous.
//
// 🔴🔴 CE N'EST PAS UN INTERRUPTEUR DE VISIBILITÉ POUR UN JOUEUR, et l'erreur
// est facile : `CActorSprite_RenderDispatch` (0x00d32100) ouvre bien par
// `Option_IsCloak` / `Option_IsInvisible` (bit 0x40) / `Option_IsHide` et sort
// sans rien dessiner… mais cette fonction sert une AUTRE vtable (0x01093b54,
// son unique référence). Le joueur est un `CPlayer` (vtable 0x01094810) dont le
// rendu est `Actor_RenderMainSprite` (slot +0x0c) — et celle-là ne sort JAMAIS
// sur l'option invisible. Mesuré : poser le bit sur l'acteur du joueur ne change
// rien à l'écran. Pour le faire disparaître, c'est `kDrawEnabled` juste dessous.
//
// ⚠ Champ du SERVEUR : en jeu, le prochain paquet d'état le réécrit.
constexpr int kOptions = 0x2c0;
constexpr uint32_t kOptionInvisible = 0x40;

// ── « Dessine ce sprite » ────────────────────────────────────────────────────
// 🔴 L'interrupteur qui marche, pour un joueur. `Actor_RenderMainSprite`
// (0x00d3a220, slot +0x0c de `CPlayer`) teste cet octet AVANT tout dessin :
//
//     if (!this[0xA0]) return CActorSprite_RenderSimpleProjected(this, ...);
//     ... ChildSprite_DrawShadow(...) ...      // l'ombre, APRÈS le test
//
// et `CActorSprite_RenderSimpleProjected` (0x00c5a010) ne dessine RIEN : elle
// projette la position monde à l'écran et la range (vtable+24). D'où l'acteur
// entier qui disparaît — sprite ET ombre — sans que rien ne perde le fil de sa
// position (nameplate, caméra qui le suit, picking).
//
// ⚠ Le voisin +0xA1 n'a PAS le même effet : testé plus bas, il retire le corps
// mais laisse l'ombre au sol.
constexpr int kDrawEnabled = 0xa0;  // octet : 0 = ne rien dessiner

// Type d'acteur : le champ `objecttype` du paquet de spawn, que les handlers
// recopient tel quel (`mov [ebx+314h], al`). Deux valeurs sont PROUVÉES :
//   · 7 = PET — `mov byte ptr [edi+314h], 7` @0x00cbab7d, dans le sous-type 0 de
//     `ZC_CHANGESTATE_PET`, le paquet qui déclare « cette entité est ton pet » ;
//     recoupé live (docs/pet_re.md §2.2) ;
//   · {1, 6, 12} = hostile / unité spéciale, le test dont le client se sert pour
//     REFUSER son menu joueur (`EntityName_IsHostileOrSpecialUnit` 0x00d9d220).
//
// ⚠ La correspondance des AUTRES valeurs avec `clif_bl_type` est DÉDUITE du
// paquet, pas vérifiée : ne pas s'en servir pour trancher quoi que ce soit.
constexpr int kType = 0x314;

constexpr int kHeightOff = 0x3f4;  // float : offset de hauteur (le saut)

// ── Apparence (les « look » que le serveur pousse) ──────────────────────────
// Relevée dans `CActorSprite_SetSexAndRebuildLook` (0x00d36280), qui passe TOUS
// ces champs à vt+76 dans l'ordre, et recoupée avec les constructeurs de couches
// (BuildHead_Slot1 0x00d3f4f0, BuildHeadgear*_Slot2/3/4, SetClothesColor,
// SetHairColor). C'est ce qui permet de composer le portrait d'une entité TIERCE.
constexpr int kHairStyle       = 0x438;
constexpr int kHairColor       = 0x43c;
constexpr int kWeaponView      = 0x440;
constexpr int kShieldView      = 0x444;
constexpr int kHeadTop         = 0x448;
constexpr int kHeadMid         = 0x44c;
constexpr int kHeadLow         = 0x450;
constexpr int kGarment = 0x454;

// Chemin de palette de CHEVEUX, et la « couleur de cheveux » qui l'ACTIVE.
// 🔴 Relevés dans `CActorSprite_BuildPartQuads` (0x605c30) : « partie 1 (TÊTE)
// et acteur+1084 > 0 → charge acteur+0x470 ». Même schéma que le corps, garde à
// zéro comprise — `kHairColor` nul veut dire « aucune palette externe ».
constexpr int kHairPalettePath = 0x470;  // std::string du client

// 🔴 `+0x4C8` est le BODY STYLE (LOOK_BODY2), PAS le sexe : le setter vt+176
// s'appelait `SetSexAndRebuildEquip` par mislabel, et il est appelé au case 0x0D
// de ZC_SPRITE_CHANGE. Le sexe, lui, est en `+0x260` (`kSex` plus haut) — c'est
// lui qui choisit `g_HairSpriteNum_Male` vs `_Female` dans
// `Job_BuildBodyOrHeadSpritePath_impl`.
// ── Les COUCHES de sprite, en vecteurs ───────────────────────────────────────
// Deux `std::vector` MSVC {begin, end, capacity} : une ressource par couche
// (corps, tête, couvre-chefs, arme, bouclier…). L'index d'une couche est son
// SLOT — l'arme est en 5, le bouclier en 6, d'où le `end - begin >= 0x1C` que
// vérifie le patch des sprites duals avant d'y toucher.
constexpr int kActVec = 0x4ac;  // std::vector<ActRes*>
constexpr int kSprVec = 0x4b8;  // std::vector<SprRes*>

constexpr int kBodyStyle    = 0x4c8;
constexpr int kDisplayClass = 0x4cc;  // celle qui nomme le chemin du CORPS

// ── Les deux JAUGES d'un acteur ──────────────────────────────────────────────
// Repli quand le serveur ne répond pas (vieux serveur, ou joueur adverse dont
// les PV sont tus) :
//   · `kMonsterGage` : `UIMonsterGage`, alimentée par ZC_HP_INFO (0x0977), donc
//     les PV d'un monstre QU'ON A FRAPPÉ ;
//   · `kHeadGage`    : `UIPcGage`, posée par le msg 34 de l'acteur (membres de
//     groupe).
// Dans les deux, PV courants en +0xA0 et maximum en +0xA4.
//
// 🔴 Les champs de SP (+0xA8/+0xAC) existent aussi mais le client ne les remplit
// JAMAIS : c'est exactement le trou que ZC 0x0F2A vient combler.
constexpr int kMonsterGage = 0x300;
constexpr int kHeadGage    = 0x488;
constexpr int kGageHp      = 0xa0;  // relatif à la JAUGE, pas à l'acteur
constexpr int kGageHpMax   = 0xa4;

// ── Envoyer un MESSAGE à un acteur (Actor_OnMsg, vtable +8) ──────────────────
//
// Le seul moyen de faire agir un acteur comme le ferait le joueur : marcher vers
// une cellule, armer une incantation, poser un niveau de sort. Le natif passe par
// là pour chacun de ces gestes.
//
// 🔴 CE CORPS EST EN ASSEMBLEUR ET IL N'EN EXISTE QU'UN. Il a été écrit deux fois
// — `ActorSendMsg` dans quick_cast.cc, `ActorSendWalkMsg` dans keyboard_move.cc —
// sous deux signatures dont la seconde n'était qu'un cas particulier de la
// première (x/y en p1/p2, p3 à zéro). Une divergence entre deux copies
// d'assembleur écrit à la main ne produit pas un affichage faux : elle
// déséquilibre la pile. Ne pas en refaire une locale.
//
// ── La forme de l'appel ─────────────────────────────────────────────────────
// Le natif empile TOUJOURS 13 dwords, `this` = l'acteur dans ECX :
//
//   (0, msg_lo, msg_hi, p1lo, p1hi, p2lo, p2hi, p3lo, p3hi, 0, 0, 0, 0)
//
// c'est-à-dire un mot de tête toujours nul, le message en 64 bits, puis CINQ
// paramètres 64 bits dont les inutilisés restent à zéro. Vérifié sur les quatre
// messages de QuickCast et sur le 0x11 de KeyboardMove.
//
// ⚠ Les paramètres voyagent en 64 BITS, d'où les dwords de poids fort. Pour un
// entier signé le mot haut est l'extension de signe (`v >> 31`) ; pour un GID il
// est ZÉRO — c'est ce que fait le natif, et une extension de signe rendrait
// négatif tout AID dont le bit 31 est armé.
//
// ⚠ La convention de nettoyage de pile du natif est INCONNUE : Ghidra ne
// récupère pas les 13 paramètres, donc on ne sait pas s'il rend en `ret 0x34` ou
// laisse l'appelant nettoyer. Le corps restaure donc ESP lui-même, ce qui est
// correct dans les deux cas.
//
// ⚠ Aucun SEH ici : un `__try` ne cohabite pas avec un bloc `__asm`. Les deux
// appelants encadrent déjà leur appel.
__declspec(noinline) void SendMsg(void* actor, int msg,
                                  int p1lo, int p1hi,
                                  int p2lo, int p2hi,
                                  int p3lo, int p3hi);

}  // namespace rag::actor
