#include "features/windows/entity_context_menu.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"  // SaveSettings (case de blocage)
#include "features/staff_gate.h"
#include "features/systems/bourgeon_opcodes.h"  // CZ 0x0F25 (NPC admin), 0x0F2B (joueur)
#include "features/item_cell.h"     // ChatLink / DeferDescFromChatLink (objet au sol)
#include "features/link_gesture.h"  // CanPostToChat / NaviKind (liens de chat)
#include "features/overlays/target_frame.h"  // masquer la fenêtre de cible
#include "features/windows/chat_window.h"  // TargetWhisper / OpenWhisperWindowByAid
#include "features/windows/entity_inspector.h"
#include "features/windows/monster_info_window.h"
#include "features/windows/view_equip_window.h"
#include "ragnarok/globals.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/ui_window_mgr.h"  // UIM_PUSHINTOCHATHISTORY (avis de blocage)
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "utils/i18n.h"
#include "ragnarok/pet.h"  // rag::pet::kOwnPetAidAddr
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/job_ids.h"  // rag::IsPlayerJob / IsMonsterJob

using namespace mui;

// ── Adresses natives (client 20250716, no-ASLR) ──────────────────────────────
// Tout est établi dans docs/entity_context_menu_re.md ; les numéros de section
// ci-dessous y renvoient.
namespace {

// §4 — le constructeur du menu natif du monde. Notre unique point d'interception.
constexpr uintptr_t kShowEntityContextMenu = 0x00c6e990;

// §5/§7 — le routeur survol/clic du monde, appelé JUSTE APRÈS le menu dans la
// même passe souris (`GameMode_ProcessMouseWorldInput` 0x00c76400) :
//     v10 = GameMode_RouteHoverAndClick(this, blocked, quad);
//     GameMode_GroundClick_RequestMove(this, v10);
// C'est le seul endroit qui voie le quad de pick APRÈS le menu contextuel et qui
// commande AUSSI le clic sol — les deux choses qu'il faut tenir en même temps
// pour qu'un NPC bloqué ne déclenche ni dialogue ni déplacement (cf. le détour).
constexpr uintptr_t kRouteHoverAndClick = 0x00c756a0;

// ── Plage réservée aux NPC à identifiant FIXE ────────────────────────────────
// `moon/npc_fixed_id.yml` côté serveur : FIXED_NPC_NUM..FIXED_NPC_NUM_LAST de
// src/map/npc.hpp. `npc_get_new_npc_id()` n'alloue jamais sous START_NPC_NUM,
// donc aucun mob, pet, homoncule ou NPC dynamique ne peut porter un GID d'ici —
// le test tient sans rien demander au serveur. Élargir la plage demande de
// recompiler le map-server ; c'est pourquoi elle est en dur des deux côtés.
constexpr uint32_t kFixedNpcGidFirst = 3000000;
constexpr uint32_t kFixedNpcGidLast  = 3000099;

// Couleur des lignes que Bourgeon écrit lui-même dans le chat (celle du DPS
// meter) : elles se distinguent de ce que dit le serveur.
constexpr uint32_t kOwnChatRgb = 0xFFAA00;

// §6.2 — CMode::SendMsg, message 24 = « la ligne N du menu a été cliquée ».
constexpr int kMsgMenuItemChosen = 24;

// §2 — état des boutons de la frame. 3 = relâché CETTE frame.
constexpr uintptr_t kMouseLButtonState = 0x011e40e4;
constexpr uintptr_t kMouseRButtonState = 0x011e40e8;
constexpr int       kBtnPressed        = 1;  // appui NEUF
constexpr int       kBtnHeld           = 2;  // maintenu (drag)
constexpr int       kBtnReleased       = 3;  // relâché CETTE frame
constexpr int       kBtnRepeat         = 4;  // appui trop rapproché du précédent

// §4.1 — les gardes que le natif applique avant d'ouvrir : on les reprend telles
// quelles, sinon notre menu apparaîtrait là où le client refusait le sien.

// §4.3 — les trois champs du CGameMode que le menu écrit et que le dispatch relit.
constexpr int kGm_MenuCodes  = 0x1cc;  // std::vector<int> : begin/end/cap
constexpr int kGm_MenuTarget = 0x2e0;  // uint32 : AID de la cible
constexpr int kGm_ActorMgr   = 0x0cc;

// Mode de ciblage courant. La valeur 1 est le ciblage AU SOL : `RouteHoverAndClick`
// sort alors immédiatement et c'est `GroundClick_RequestMove` qui lance le sort.
constexpr int kGm_TargetingMode = 0x408;
constexpr int kTargetingGround  = 1;

// « Ce clic est déjà consommé, ne demande pas de déplacement » — la convention du
// client, lue dans le garde de `GameMode_GroundClick_RequestMove` :
// `param_1 && param_1 != 2 -> return param_1`. Le natif lui-même rend 1 sur un de
// ses chemins (`if (LButtonState == 2) return 1`), c'est donc une valeur qu'il
// sait déjà recevoir.
constexpr int kClickConsumed = 1;

// §8 — helpers natifs réutilisés.
constexpr uintptr_t kStdVectorIntPushBack = 0x007a7fa0;  // __thiscall(vec*, int*)
constexpr uintptr_t kActiveIdSetContains  = 0x00a727f0;  // __cdecl(aid) -> bool
// La liste d'amis, interrogée par NOM — le prédicat que le natif consulte lui
// aussi (@0x00c6f699) pour ne pas proposer « ajouter en ami » deux fois.
// ⚠ `this` est l'ADRESSE du global, pas son contenu (`mov ecx, offset …`).
constexpr uintptr_t kFriendListContains   = 0x00a388f0;  // __thiscall(mgr, name)

// ── La liste d'ignorés du chat : `std::set<std::string>` ─────────────────────
// Derrière le pointeur global 0x01251824. `ChatBlockList_Contains` (0x005ee940)
// la consulte pour choisir entre « Block » et « Unblock » — c'est donc bien UN
// ÉTAT, et le menu natif l'affichait déjà comme tel.
// On la LIT au lieu de l'appeler : le prédicat natif prend sa `std::string` PAR
// VALEUR (0x18 octets sur la pile, détruits par l'appelée), ce qui obligerait à
// fabriquer une chaîne du client — et à allouer sur SON tas dès 16 caractères.
// Parcourir l'arbre ne coûte rien et ne peut rien casser.
//   objet+0x18 = `_Myhead` (l'arbre commence là : `sub_5EE730` insère dans
//   `this+24`) ; racine = `_Myhead->_Parent`.
//   nœud : _Left@0, _Parent@4, _Right@8, _Isnil@0x0D, `std::string`@0x10
//   (taille +0x20, capacité +0x24) — offsets lus dans `sub_5EE3A0`.
constexpr uintptr_t kChatBlockListPtr = 0x01251824;
constexpr int kSet_Head     = 0x18;
constexpr int kNode_Left    = 0x00;
constexpr int kNode_Parent  = 0x04;
constexpr int kNode_Right   = 0x08;
constexpr int kNode_IsNil   = 0x0d;
constexpr int kNode_Val     = 0x10;
constexpr int kTreeWalkGuard = 512;
// Le prédicat d'adoption du client (`sub_D99860`) : niveau >= 70, non monté, en
// couple, cible éligible… C'est LUI qui décide de l'entrée « Adopter » dans le
// menu natif, et le dispatcher ne le rejoue pas — sans ce test, l'entrée partirait
// au serveur pour se faire refuser.
constexpr uintptr_t kAdoptionEligible     = 0x00d99860;  // __stdcall(aid) -> bool
// « Ceci n'est pas un vrai joueur » : type d'acteur ∈ {1, 6, 12} ou job de
// NPC/portail. Le natif s'en sert pour REFUSER tout menu (docs §4.1) — sans ce
// test, un PNJ scripté sous forme de joueur se verrait proposer « échanger ».
constexpr uintptr_t kIsHostileOrSpecial   = 0x00d9d220;  // __stdcall(aid, job) -> bool
// ── CNameInfo : ce que la plaque de nom sait déjà ────────────────────────────
// Le dictionnaire `std::map<GID, CNameInfo>` de GameMode+0x160 est rempli par
// ZC 0x0A30 et porte, pour CHAQUE joueur croisé, bien plus que son pseudo :
// c'est lui qui compose la plaque « [titre] pseudo (groupe) » / « guilde [rang] »
// (`UIActorNameLabel_SetNameFromInfo` 0x0082e1d0, docs/entity_nameplate_re.md).
// Champs, tous vérifiés live (2026-08-05) : +0x04 nom, +0x1C groupe, +0x34
// guilde, +0x4C rang, +0x64 titre. Chacun est une `std::string` de 0x18 octets :
// buffer, puis taille à +0x10 et capacité à +0x14 DU CHAMP.
// ⚠ Le dictionnaire n'a PAS d'entrée pour soi-même : `CNameDict_GetEntryOrRequest`
// rend alors un objet vide STATIQUE (0x01251678), tous champs à "". Ne jamais s'y
// fier pour lire ses propres infos — cf. docs §9.6.

constexpr uintptr_t kInPartyFlag   = 0x015ff804;
// ⚠ Ces deux offsets sont ceux des comparaisons `*((int*)session + N)` du client,
// pas les noms qu'IDA leur a donnés : 5462*4 = 0x5558 et 5506*4 = 0x5608.
constexpr int kSess_HomunAid = 0x5558;  // GameMode_IsCurrentId5558
constexpr int kSess_MercAid  = 0x5608;  // GameMode_IsCurrentId5608

// ── Le groupe : `std::list<PartyMember>` à session+0x17B8 ────────────────────
// +0x17BC = le nœud sentinelle (`_Myhead`), +0x17C0 = la taille, celle que rend
// `Social_GetPartyMemberCount` (0x00d5cf50) et dont ChatWindow se sert déjà.
// Nœud `{next@0, prev@4, valeur@8}`.
//
// ✅ Relevé live (2026-08-05, groupe d'un seul membre) — nœud @0x471BA010 :
//     +0x08 = 1          (valeur+0x00, drapeau)
//     +0x0C = 2000001    (valeur+0x04) == g_Account_Aid : L'AID DU MEMBRE
//     +0x10 = 150000     (valeur+0x08)
//     +0x14 = "Stingor"  (valeur+0x0C, std::string ; taille +0x24, cap +0x28)
//     +0x2C = "gonryun.rsw" (valeur+0x24, la map)
// C'est donc bien la clé de `Social_FindPartyMemberByAid` (0x00d5d650), dont les
// appelants sont des handlers de paquets qui lui passent `pkt+2`. Le natif du
// menu, lui, cherche par NOM (`sub_D5D960`) — clé plus fragile, on garde l'AID.
constexpr int kSess_PartyListHead = 0x17bc;
constexpr int kPartyNode_Aid      = 0x0c;  // nœud+0x0C = valeur+0x04
constexpr int kPartyWalkGuard     = 64;    // un groupe plafonne à 12 : garde-fou

// Acteur : `vtable+0xC4` rend l'**id de guilde**. C'est l'appel exact que
// `GameMode_ShowEntityContextMenu` fait (`call [edx+0C4h]` @0x00c6f4e2) pour
// décider de proposer, ou non, l'invitation en guilde.

// Type d'acteur (`acteur+0x314`) : le champ `objecttype` du paquet de spawn,
// que les handlers recopient tel quel (`mov [ebx+314h], al`). Deux valeurs sont
// PROUVÉES, et ce sont les seules dont ce fichier a besoin :
//   · **7 = PET** — `mov byte ptr [edi+314h], 7` @0x00cbab7d, dans le sous-type 0
//     de `ZC_CHANGESTATE_PET`, le paquet qui déclare « cette entité est ton
//     pet » ; recoupé live (docs/pet_re.md §2.2) ;
//   · {1, 6, 12} = hostile / unité spéciale, le test dont le client se sert pour
//     REFUSER son menu joueur (`EntityName_IsHostileOrSpecialUnit` 0x00d9d220).
// ⚠ La correspondance des AUTRES valeurs avec `clif_bl_type` est déduite du
// paquet, pas vérifiée : ne pas s'en servir pour trancher quoi que ce soit ici.
constexpr int kActor_Type = 0x314;

// §3 — catégories du quad de picking. La catégorie 0 (acteur ordinaire : joueur,
// monstre, PNJ scripté) n'a pas de constante : c'est le cas par défaut, tranché
// sur le job faute de mieux.
//
// 🔴 La catégorie 3, c'est le PET — pas un objet au sol. Il n'existe que TROIS
// producteurs de quads (les trois xrefs de `NameplateQueue_Insert` 0x00a79610),
// et le seul qui écrive 3 est `CActorSprite_SubmitNameplateQuad` @0x00c58c48,
// sous la condition `acteur+0x314 == 7`, c'est-à-dire `CLIF_BL_PET`.
//
// 🔴 La catégorie 1, c'est l'OBJET AU SOL — pas le NPC de map (RE live
// 2026-08-19, x32dbg sur une Red Potion jetée). Le producteur 0x00d1da70, que la
// doc appelait « NpcActor_SubmitNameplateQuad », est en réalité `vt+0x14` de la
// classe **CItem** (RTTI `.?AVCItem@@`, vtable 0x010932AC) — les vrais NPC
// (CNpc, vtable 0x010939D4) portent le producteur de CActorSprite et sortent en
// catégorie 0, classés ensuite au job. Le quad d'un CItem est sans ambiguïté :
//   +0x18 (aid) = CItem+0x17C (l'AID du flooritem, alloué sous 2 000 000)
//   +0x1c (job) = la CONSTANTE 0x7D03 — jamais un job réel
//   +0x20 (cat) = 1
// C'est cette confusion qui donnait aux objets au sol le menu d'un NPC,
// « Interagir » et outillage admin compris.
constexpr int kPickGroundItem = 1;
constexpr int kPickSkillUnit  = 2;
constexpr int kPickPet        = 3;
constexpr int kPickSpecial    = 4;  // homoncule / mercenaire / élémentaire

// ── L'objet au sol : la classe CItem (RE live 2026-08-19) ────────────────────
// Les CItem vivent dans leur PROPRE liste du gestionnaire d'acteurs — pas celle
// que parcourt `ActorListFindByGid` (+0x10, NPC/mobs), ni la liste de rendu
// (+0x08, tout ce qui se dessine). Liste à sentinelle MSVC : nœud {next@0,
// prev@4, CItem*@8}, tête = le pointeur stocké à mgr+0x18.
// Champs du CItem, remplis par sa méthode d'init (0x00d1d390, vérifiés live sur
// une Red Potion : 501 / 1 / 501 / 4796) :
//   +0x170 = nameid tronqué en uint16 (le client fait `atoi` du nom reçu)
//   +0x174 = octet « identifié » (un équipement tombé d'un monstre vaut 0)
//   +0x178 = nameid COMPLET — celui qu'on lit
//   +0x17c = AID du flooritem, celui que porte le quad de pick
constexpr int kActorMgr_ItemList = 0x18;
constexpr int kItem_NameId       = 0x178;
constexpr int kItem_Identified   = 0x174;
constexpr int kItem_Aid          = 0x17c;
constexpr int kItemWalkGuard     = 256;  // MAX_FLOORITEM par carte est bien plus bas

// L'acteur du JOUEUR, celui à qui le client adresse ses ordres de déplacement.
// C'est `[[gm+0xCC]+0x2C]` — l'adresse exacte que lit la branche « clic gauche
// sur la catégorie 1 » de `CursorMgr_UpdateHover` (@0x00c792e8).
constexpr int kActorMgr_OwnActor = 0x2c;

// « Va jusqu'à cette cible et agis » : le message 0x12 de l'IA de l'acteur
// (vtable+0x8), avec l'AID en quatrième argument et tout le reste à zéro —
// mot pour mot ce qu'émet le clic gauche natif sur un objet au sol
// (@0x00c792d6). C'est l'IA qui fait MARCHER le personnage jusqu'à l'objet
// avant d'envoyer CZ_ITEM_PICKUP : rejouer ce message rejoue tout le trajet.
constexpr int kOwnActorMsg_GoAct = 0x12;

// (Le nom AFFICHÉ vient d'itemcell::NameById — la DB de descriptions du client.
// ⚠ Ne pas employer `ItemIdStr_ToResourceName` 0x00d5afc0, que l'init de CItem
// appelle : elle rend le nom de RESSOURCE, celui du sprite, en coréen.)

// §6.3 — codes d'action natifs (ceux qu'on rejoue).
constexpr int kCodeDeal        = 4;
constexpr int kCodePartyInvite = 5;
constexpr int kCodeAddFriend   = 10;
constexpr int kCodeBlockChat   = 12;
constexpr int kCodeUnblockChat = 13;
constexpr int kCodeWhisper     = 20;
constexpr int kCodeGuildInvite = 22;
constexpr int kCodeMannerPlus  = 23;
constexpr int kCodeMannerMinus = 24;
constexpr int kCodeGuildAlly   = 25;
constexpr int kCodeGuildFoe    = 26;
constexpr int kCodeKick        = 28;
constexpr int kCodePetFeed     = 29;
constexpr int kCodePetPerform  = 30;
constexpr int kCodePetToEgg    = 31;
constexpr int kCodePetUnequip  = 32;
constexpr int kCodePetStatus   = 33;
// ⚠ Pas de constante pour le code **34** (« Afficher le GID dans le chat ») : ce
// menu ne le porte pas. « Copier l'identité de pick » dit déjà tout ce qu'il
// disait, et davantage. Il était en plus IRREJOUABLE — cf. l'en-tête, section
// « Rejouer plutôt que réécrire », et docs §10.4.
constexpr int kCodeChatBanLog  = 35;
constexpr int kCodeAdopt       = 36;
constexpr int kCodeHomunStatus = 37;
constexpr int kCodeHomunFeed   = 38;
constexpr int kCodeHomunStandBy = 39;
constexpr int kCodeMercStatus  = 40;
constexpr int kCodeMercStandBy = 41;
constexpr int kCodeViewEquip   = 42;
constexpr int kCodeFullStrip   = 44;
// ⚠ Pas de constante pour le code **57** (« Copier le C-Code ») non plus. Il
// était rejouable, lui — mesuré : `ActorList_FindByGID(gm+0x2E0)` → RTTI vers
// `CPc` → `vt+0xA8` → `Aid_FormatObfuscated` → presse-papier. Mais ce qu'il
// copie n'est que l'AID OBSCURCI, la forme entre crochets que le client affiche
// à côté des noms ; « Copier l'AID » donne le vrai, juste au-dessus.
constexpr int kCodeSendMail    = 58;
constexpr int kCodeReportUser  = 63;
// ⛔ Code 65 (« Close stall », CZ 0x0AF9) VOLONTAIREMENT absent : moonlight ne
// déclare pas ce paquet, et clif_parse DÉCONNECTE sur un opcode inconnu — le GM
// qui cliquerait cette entrée se ferait kicker lui-même (docs §6.3).

// CZ_CONTACTNPC : parler à un NPC. 7 octets, comme le natif l'émet lui-même dans
// la branche « entité hostile/spéciale » de CursorMgr_UpdateHover (docs §7).
constexpr uint16_t kCzContactNpc = 0x0090;

// ── CZ_BOURGEON_NPC_ADMIN (0x0F25) ───────────────────────────────────────────
// [op:2][len:2][gid:4][action:1]. Miroir EXACT de `e_bourgeon_npc_admin_action`
// côté moonlight (packets_struct.hpp) : les deux listes bougent ensemble et les
// valeurs ne se réordonnent pas.
constexpr uint8_t kNpcAdminReloadFile = 0;
constexpr uint8_t kNpcAdminUnload     = 1;
constexpr uint8_t kNpcAdminMoveToMe   = 2;

// ── CZ_BOURGEON_PLAYER_ADMIN (0x0F2B) ────────────────────────────────────────
// [op:2][len:2][aid:4][action:1][param:4]. Miroir EXACT de
// `e_bourgeon_player_admin_action` côté moonlight (packets_struct.hpp) : les deux
// listes bougent ensemble et les valeurs ne se réordonnent pas.
constexpr uint8_t kPlayerAdminComeHere    = 0;
constexpr uint8_t kPlayerAdminSitStand    = 1;
constexpr uint8_t kPlayerAdminEventPoints = 2;
constexpr uint8_t kPlayerAdminMute        = 3;
constexpr uint8_t kPlayerAdminUnmute      = 4;
constexpr uint8_t kPlayerAdminJailToggle  = 5;
constexpr uint8_t kPlayerAdminNuke        = 6;
constexpr uint8_t kPlayerAdminBlock       = 7;

// Le pas d'un don de points d'event, repris tel quel du NPC qu'il remplace. Le
// serveur reborne de son côté : ceci ne fait que cadrer la saisie.
constexpr int kEventPointsStep = 50;

// Les deux couleurs du menu, définies UNE fois parce qu'elles servent désormais
// à trois endroits (les lignes, le titre du sous-menu, la modale) : l'ocre de ce
// qui n'est pas offert au joueur, et le rouge de ce qui retire quelque chose au
// monde. Le rouge l'emporte sur l'ocre.
const ImVec4 kStaffColor (0.62f, 0.28f, 0.10f, 1.0f);
const ImVec4 kDangerColor(0.75f, 0.13f, 0.13f, 1.0f);


void SendNpcAdmin(uint32_t gid, uint8_t action) {
  uint8_t packet[9];
  *reinterpret_cast<uint16_t*>(packet + 0) = bopcodes::kNpcAdmin;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(sizeof(packet));
  *reinterpret_cast<uint32_t*>(packet + 4) = gid;
  packet[8] = action;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

// `param` ne sert aujourd'hui qu'au delta de points d'event ; il part quand même
// à chaque action, pour que la taille du paquet reste FIXE — c'est ainsi que le
// serveur le déclare (`parseable_packet(..., sizeof(...))`), et une taille
// variable aurait imposé de le déclarer autrement des deux côtés.
void SendPlayerAdmin(uint32_t aid, uint8_t action, int32_t param = 0) {
  uint8_t packet[13];
  *reinterpret_cast<uint16_t*>(packet + 0) = bopcodes::kPlayerAdmin;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(sizeof(packet));
  *reinterpret_cast<uint32_t*>(packet + 4) = aid;
  packet[8] = action;
  *reinterpret_cast<int32_t*>(packet + 9) = param;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
}

using PushBackFn   = void   (__thiscall*)(void*, const int*);
using ContainsNameFn = int  (__thiscall*)(void*, const char*);
// ⚠ Deux conventions différentes, et confondre les deux décale la pile de 4
// octets à chaque appel : ActiveIdSet_Contains est __cdecl (l'appelant dépile),
// le prédicat d'adoption est __stdcall (l'appelée dépile).
using ContainsFn   = bool   (__cdecl*)(uint32_t);
using PredicateFn  = bool   (__stdcall*)(uint32_t);
using Predicate2Fn = int    (__stdcall*)(uint32_t, uint32_t);
using GetEntryFn   = void*  (__thiscall*)(void*, uint32_t);
using DispatchFn   = int    (__thiscall*)(void*, int, int, int, int, int);
using ClickFn      = int    (__thiscall*)(void*, uint32_t, int);
using FindActorFn  = void*  (__thiscall*)(void*, uint32_t);
// L'OnMsg de l'IA d'un acteur (vtable+0x8) : treize entiers après `this`, comme
// au site d'appel natif — le client pousse treize zéros et n'en relit que
// quelques-uns selon le message.
using ActorOnMsgFn = int    (__thiscall*)(void*, int, int, int, int, int, int,
                                          int, int, int, int, int, int, int);
using ShowMenuFn   = int    (__fastcall*)(void*, void*, int, int);
// ⚠ Ordre des arguments : le natif est `__thiscall(this, blocked, quad)` — le
// drapeau « une fenêtre native mange la souris » d'abord, le quad de pick
// ENSUITE (`GameMode_RouteHoverAndClick(this, v2, Point)` chez l'appelant).
// Les intervertir rendrait un quad de pick au client sous forme de drapeau.
using RouteHoverFn = int*   (__fastcall*)(void*, void*, int*, int*);

// Un champ à un offset : la lecture est celle de tout le monde (globals.h),
// et le `using` garde les points d'appel de ce fichier tels quels.
using rag::Read;
inline int  ReadGlobalInt(uintptr_t a) { return *reinterpret_cast<int*>(a); }
inline void WriteGlobalInt(uintptr_t a, int v) { *reinterpret_cast<int*>(a) = v; }
inline void* ReadGlobalPtr(uintptr_t a) { return *reinterpret_cast<void**>(a); }

// L'AID est-il dans la liste `<admin>` du clientinfo.xml (docs §4.2) ? C'est ce
// que le DISPATCHER natif exige pour les codes 23/24/44 : sans cela, l'action
// est rejetée en silence, et proposer l'entrée serait mentir au joueur.
bool ClientAdminListContains(uint32_t aid) {
  __try {
    return reinterpret_cast<ContainsFn>(kActiveIdSetContains)(aid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool AdoptionEligible(uint32_t aid) {
  __try {
    return reinterpret_cast<PredicateFn>(kAdoptionEligible)(aid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool IsHostileOrSpecialUnit(uint32_t aid, uint32_t job) {
  __try {
    return reinterpret_cast<Predicate2Fn>(kIsHostileOrSpecial)(aid, job) == 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Recopie une `std::string` du client, SSO comprise. Rend false si elle est vide
// ou trop longue pour le tampon — dans les deux cas, l'appelant n'a rien à en
// tirer.
// ⚠ SEH ⇒ AUCUN objet C++ dans cette fonction (C2712 : « __try dans une fonction
// qui exige un déroulement d'objet »). C'est la raison de ces buffers bruts, pas
// un goût pour le C.
// Un champ BRUT de la plaque de nom d'une entité (pseudo, groupe, guilde…), tel
// que le dictionnaire du client le porte. Comme le natif, un GID inconnu
// déclenche la demande au serveur (l'info arrivera plus tard, et le menu
// affichera l'AID d'ici là).
bool ReadNameFieldRaw(void* game_mode, uint32_t aid, int field, char* out,
                      size_t out_size) {
  const void* str = nullptr;
  __try {
    void* dict = reinterpret_cast<uint8_t*>(game_mode) + gamescene::kGmNameDict;
    void* entry = reinterpret_cast<GetEntryFn>(gamescene::kNameDictGetEntryOrRequestAddr)(dict, aid);
    if (!entry) return false;
    str = reinterpret_cast<const uint8_t*>(entry) + field;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return rag::clientstr::Copy(str, out, out_size);
}

std::string EntityName(void* game_mode, uint32_t aid) {
  char buffer[64] = {};
  if (!ReadNameFieldRaw(game_mode, aid, gamescene::kNameStr, buffer, sizeof(buffer)))
    return std::string();
  // Les noms voyagent dans l'encodage du client : on affiche en UTF-8.
  const char* utf8 = ro::WireToUtf8(buffer);
  return utf8 ? std::string(utf8) : std::string();
}

void* FindActor(void* game_mode, uint32_t aid) {
  __try {
    void* actor_mgr = Read<void*>(game_mode, kGm_ActorMgr);
    if (!actor_mgr) return nullptr;
    return reinterpret_cast<FindActorFn>(gamescene::kActorListFindByGidAddr)(actor_mgr, aid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Ce joueur est-il déjà dans notre liste d'ignorés ? Parcours complet de
// l'arbre plutôt qu'une descente : la liste plafonne à une vingtaine de noms, et
// une descente supposerait de rejouer EXACTEMENT le comparateur du client
// (`sub_4DCBA0`) — une erreur de casse ou d'ordre y répondrait « absent » sur un
// nom pourtant présent.
// ⚠ SEH ⇒ aucun objet C++ ici.
bool ChatBlockListContains(const char* wire_name) {
  if (!wire_name || !*wire_name) return false;
  __try {
    const uint8_t* obj = *reinterpret_cast<const uint8_t**>(kChatBlockListPtr);
    if (!obj) return false;
    const uint8_t* head = Read<const uint8_t*>(obj, kSet_Head);
    if (!head) return false;
    const uint8_t* stack[64];
    int top = 0;
    const uint8_t* node = Read<const uint8_t*>(head, kNode_Parent);  // la racine
    for (int guard = 0; guard < kTreeWalkGuard; ++guard) {
      const bool real = node && Read<uint8_t>(node, kNode_IsNil) == 0;
      if (real) {
        if (top >= 64) return false;  // arbre incohérent : on renonce
        stack[top++] = node;
        node = Read<const uint8_t*>(node, kNode_Left);
        continue;
      }
      if (top == 0) return false;  // parcours terminé, rien trouvé
      node = stack[--top];
      const unsigned size = rag::clientstr::Size(node + kNode_Val);
      const char* name    = rag::clientstr::Data(node + kNode_Val);
      if (name && size != 0 && _stricmp(name, wire_name) == 0) return true;
      node = Read<const uint8_t*>(node, kNode_Right);
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// La cible est-elle déjà dans notre liste d'amis ? Le natif retirait alors
// l'entrée ; on la grise, avec sa raison.
bool FriendListContainsName(const char* wire_name) {
  if (!wire_name || !*wire_name) return false;
  __try {
    return reinterpret_cast<ContainsNameFn>(kFriendListContains)(
               reinterpret_cast<void*>(uiwnd::kUIWindowMgrAddr), wire_name) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// La cible appartient-elle DÉJÀ à un groupe — le nôtre ou celui d'un autre ?
// C'est la question qui décide si l'invitation a la moindre chance d'aboutir :
// le serveur refuse tout invité déjà associé à un groupe (`party_invite`,
// PARTY_REPLY_JOIN_OTHER_PARTY). Le natif, lui, ne testait que « pas dans NOTRE
// groupe » et laissait donc cliquer une invitation vouée au refus.
//
// La réponse est dans la plaque de nom : `CNameInfo+0x1C` porte le nom du groupe
// de la cible — c'est ce que le client AFFICHE entre parenthèses derrière le
// pseudo. Non vide ⇒ elle est en groupe. Si sa plaque n'est pas encore arrivée,
// tout est vide et on ne grise rien : on ne bloque jamais sur une ignorance.
bool TargetHasParty(void* game_mode, uint32_t aid) {
  char party[64] = {};
  return ReadNameFieldRaw(game_mode, aid, gamescene::kNameParty, party, sizeof(party));
}

// La cible est-elle DÉJÀ dans notre groupe ? Le natif se posait la question
// (docs §5.4a) et retirait l'entrée ; on préfère la griser, mais la source est
// la même liste. Comparaison par AID, relevée live (cf. le bloc d'offsets) :
// exacte, là où le nom de groupe de la plaque confondrait deux groupes
// homonymes. Lecture seule, sans appel natif.
// ⚠ SEH ⇒ aucun objet C++ ici.
bool OwnPartyContains(uint32_t aid) {
  __try {
    void* session = reinterpret_cast<void*>(rag::kSessionAddr);
    void** head = Read<void**>(session, kSess_PartyListHead);
    if (!head) return false;
    void** node = reinterpret_cast<void**>(*head);
    for (int guard = 0; node && node != head && guard < kPartyWalkGuard; ++guard) {
      if (Read<uint32_t>(node, kPartyNode_Aid) == aid) return true;
      node = reinterpret_cast<void**>(*node);
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Rejouer une entrée de menu par le dispatcher du client (docs §9.2) ───────
// Les trois gestes du natif, puis « la ligne 0 a été cliquée ». Rien n'est
// réécrit : c'est le client qui décide encore de tout.
// ⚠ SEH ⇒ aucun objet C++ ici non plus.
bool RunNativeMenuCode(int code, uint32_t aid) {
  __try {
    void* game_mode = ReadGlobalPtr(rag::kActiveModePtr);
    if (!game_mode) return false;
    auto* gm = reinterpret_cast<uint8_t*>(game_mode);

    // 1. la cible — le dispatcher la relit dans CE champ, pas dans nos membres
    *reinterpret_cast<uint32_t*>(gm + kGm_MenuTarget) = aid;
    // 2. vider le vecteur de codes : end = begin, exactement comme le natif
    //    (`mov [edi+1D0h], eax` de son case 101). Aucune libération, donc rien
    //    à réallouer et aucun pointeur à invalider.
    void** begin = reinterpret_cast<void**>(gm + kGm_MenuCodes);
    void** end   = reinterpret_cast<void**>(gm + kGm_MenuCodes + 4);
    *end = *begin;
    // 3. y pousser NOTRE code, et un seul
    reinterpret_cast<PushBackFn>(kStdVectorIntPushBack)(gm + kGm_MenuCodes, &code);
    // 4. « la ligne 0 du menu a été cliquée »
    void** vtable = *reinterpret_cast<void***>(game_mode);
    reinterpret_cast<DispatchFn>(vtable[6])(game_mode, kMsgMenuItemChosen, 0, 0,
                                            0, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le clic gauche du client sur un acteur, à l'identique : c'est LUI qui tranche
// entre attaquer, suivre et refuser (surcharge, consentement PvP).
bool RunNativeActorClick(uint32_t aid) {
  __try {
    void* game_mode = ReadGlobalPtr(rag::kActiveModePtr);
    if (!game_mode) return false;
    reinterpret_cast<ClickFn>(gamescene::kPostActorClickActionAddr)(game_mode, aid, 1);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le CItem que désigne cet AID, lu dans la liste des objets au sol. Rend false
// si l'objet n'y est plus — quelqu'un a pu le ramasser entre le clic et cette
// frame, et le menu doit alors le dire plutôt que d'afficher n'importe quoi.
// ⚠ SEH ⇒ aucun objet C++ ici.
bool ReadGroundItem(void* game_mode, uint32_t aid, uint32_t* nameid,
                    bool* identified) {
  __try {
    const uint8_t* mgr = Read<const uint8_t*>(game_mode, kGm_ActorMgr);
    if (!mgr) return false;
    void* const* sentinel = Read<void* const*>(mgr, kActorMgr_ItemList);
    if (!sentinel) return false;
    void* const* node = reinterpret_cast<void* const*>(*sentinel);
    for (int guard = 0; node && node != sentinel && guard < kItemWalkGuard;
         ++guard) {
      const uint8_t* item = reinterpret_cast<const uint8_t*>(node[2]);
      if (item && Read<uint32_t>(item, kItem_Aid) == aid) {
        *nameid     = Read<uint32_t>(item, kItem_NameId);
        *identified = Read<uint8_t>(item, kItem_Identified) != 0;
        return true;
      }
      node = reinterpret_cast<void* const*>(*node);
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Le clic gauche du client sur un objet au sol, à l'identique : « va jusqu'à
// lui et agis » à l'IA de NOTRE acteur, qui gère l'approche puis émet
// CZ_ITEM_PICKUP. Même garde que le natif : pas pendant un ciblage — le clic
// appartiendrait alors au sort, pas au ramassage.
bool RunNativePickupItem(uint32_t aid) {
  __try {
    void* game_mode = ReadGlobalPtr(rag::kActiveModePtr);
    if (!game_mode) return false;
    if (Read<int>(game_mode, kGm_TargetingMode) != 0) return false;
    const uint8_t* mgr = Read<const uint8_t*>(game_mode, kGm_ActorMgr);
    if (!mgr) return false;
    void* own = Read<void*>(mgr, kActorMgr_OwnActor);
    if (!own) return false;
    void** vtable = *reinterpret_cast<void***>(own);
    reinterpret_cast<ActorOnMsgFn>(vtable[2])(own, 0, kOwnActorMsg_GoAct, 0,
                                              static_cast<int>(aid), 0, 0, 0, 0,
                                              0, 0, 0, 0, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Écrit une ligne dans le chat du jeu, par la voie unique de Bourgeon
// (`UIWindowMgr::SendMsg`, qui aiguille vers la chatbox ImGui quand elle vit et
// vers la native sinon).
//
// Encodage : `Utf8ToWireText`, qui rend du CP1252 tant que la phrase y rentre —
// c'est le cas de tous nos messages, qui n'ont que des accents — et de l'UTF-8
// sinon. C'est avec `WireToUtf8` que la chatbox ImGui relit ce qu'on lui donne
// (`ChatWindow::Parse`), et elle accepte les deux ; sans conversion du tout, nos
// accents partiraient en octets UTF-8 bruts vers la chatbox NATIVE, qui ne sait
// lire que du 1252, et reviendraient en mojibake.
//
// ⚠ Appelée depuis le rendu, comme le DPS meter et le refus de la boutique NPC :
// UIM_PUSHINTOCHATHISTORY empile une ligne et n'ouvre aucune modale — c'est la
// seule commande native sans danger en pleine frame ImGui.
void SayToChat(const char* utf8) {
  if (!utf8 || !*utf8) return;
  const char* wire = ro::Utf8ToWireText(utf8);
  if (!wire || !*wire) return;
  UIWindowMgr::SendMsg(UIMessage::UIM_PUSHINTOCHATHISTORY,
                       reinterpret_cast<int>(wire), kOwnChatRgb, 0, 0);
}

// De quoi nommer un NPC bloqué dans une phrase. Le nom est celui relevé au
// moment du blocage ; il peut manquer si la plaque de nom n'était pas encore
// arrivée, auquel cas le GID vaut mieux qu'une phrase sans sujet.
void FormatNpcLabel(uint32_t gid, const char* name, char* out, size_t out_size) {
  if (name == nullptr || *name == '\0') {
    std::snprintf(out, out_size, i18n::Tr("Le NPC %u"), gid);
  } else {
    std::snprintf(out, out_size, "%s", name);
  }
}

// Le module, pour le détour (fonction libre : il tourne avant tout objet).
EntityContextMenu* g_owner = nullptr;
void* g_trampoline = nullptr;
void* g_trampoline_route = nullptr;

// Corps du détour, isolé dans sa propre fonction : __try/__except est interdit
// dans une fonction qui doit dérouler des objets C++, et OnNativeContextMenu en
// manipule (std::string, std::vector).
bool SafeOnNativeContextMenu(void* game_mode, const int* quad, int blocked) {
  __try {
    return g_owner->OnNativeContextMenu(game_mode, quad, blocked);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Une entité à moitié construite a fait faute : on laisse tomber CE clic.
    // Rien de natif n'a été modifié, et le natif ne construira rien non plus —
    // ce qui vaut mieux que de le laisser lire le même quad douteux.
    return true;
  }
}

// ⚠ ABI : le natif est `__thiscall(this, quad, blocked)` avec `ret 8` ; un
// `__fastcall(this, edx, quad, blocked)` a exactement la même convention côté
// appelant ET côté pile. Le trampoline se rappelle donc sous la même signature.
int __fastcall ShowEntityContextMenuDetour(void* game_mode, void* edx, int quad,
                                           int blocked) {
  if (g_owner && g_owner->imgui_enabled_) {
    if (SafeOnNativeContextMenu(game_mode, reinterpret_cast<const int*>(quad),
                                blocked)) {
      return 0;  // le natif ne construit rien : sa fenêtre 0x12 ne naît jamais
    }
  }
  // Sans trampoline, il n'y a rien vers quoi revenir — et le détour ne devrait
  // même pas tourner. On rend la main sans rien faire plutôt que de sauter dans
  // le vide (le menu natif manquerait, le client survivrait).
  if (!g_trampoline) return 0;
  return reinterpret_cast<ShowMenuFn>(g_trampoline)(game_mode, edx, quad, blocked);
}

// Même raison que SafeOnNativeContextMenu : le prédicat manipule des objets C++
// (une std::map), donc son __try vit ici.
bool SafeShouldIgnoreWorldClick(const int* quad) {
  __try {
    return g_owner->ShouldIgnoreWorldClick(quad);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Quad illisible : on ne s'en mêle pas. Rendre `true` ferait disparaître une
    // entité du routage pour une raison qui n'a rien à voir avec le blocage.
    return false;
  }
}

// 🔴 NE PAS avaler le clic pendant un ciblage AU SOL. Le joueur qui vise une
// zone (Storm Gust…) et clique la case où se tient un NPC bloqué doit lancer son
// sort : c'est `GroundClick_RequestMove` qui s'en charge, et notre « clic
// consommé » l'en empêcherait. Aucun risque de dialogue par ailleurs — dans ce
// mode `RouteHoverAndClick` sort AVANT d'atteindre `CursorMgr_UpdateHover`.
// En cas de lecture douteuse on répond « oui, ciblage » : ne rien avaler est le
// repli inoffensif.
bool IsGroundTargeting(void* game_mode) {
  __try {
    if (!game_mode) return true;
    return Read<int>(game_mode, kGm_TargetingMode) == kTargetingGround;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// Détour du routeur survol/clic. Sur un NPC bloqué, et seulement sur les frames
// où un bouton agit, il fait DEUX choses :
//   · il efface le quad de pick -> le natif ne route rien vers le NPC, donc ni
//     dialogue ni ordre d'approche (`OwnActor::OnMsg(18)`) ;
//   · il rend « clic consommé » -> `GroundClick_RequestMove` sort sur son garde
//     `param_1 && param_1 != 2` sans demander de déplacement.
// Le clic ne produit alors STRICTEMENT AUCUN paquet : ni CZ_CONTACTNPC, ni
// marche. Le NPC devient une zone morte, ce qu'attend un joueur qui vient de
// dire « celui-là, je ne veux plus le cliquer ».
// Le natif est appelé quand même, avec un quad nul : c'est lui qui tient à jour
// le curseur et ses cibles de suivi, et « quad nul » est exactement la vérité de
// la situation — le curseur ne survole plus rien de cliquable.
int* __fastcall RouteHoverAndClickDetour(void* game_mode, void* edx, int* blocked,
                                         int* quad) {
  // `blocked` non nul = une fenêtre native mange déjà la souris : le clic ne va
  // pas au monde, il n'y a rien à avaler.
  const bool swallow = g_owner && quad && !blocked &&
                       !IsGroundTargeting(game_mode) &&
                       SafeShouldIgnoreWorldClick(quad);
  if (swallow) quad = nullptr;

  // Sans trampoline, rendre `blocked` : c'est ce que le natif rend lui-même sur
  // tous ses chemins « rien à router », donc le clic sol garde son sens.
  int* const routed =
      g_trampoline_route ? reinterpret_cast<RouteHoverFn>(g_trampoline_route)(
                               game_mode, edx, blocked, quad)
                         : blocked;
  return swallow ? reinterpret_cast<int*>(kClickConsumed) : routed;
}

}  // namespace

EntityContextMenu::EntityContextMenu() {
  g_owner = this;
  // Posé inconditionnellement, comme FpsView : le timestamp du client n'est pas
  // encore connu au moment de LoadPlugins(). Le détour, lui, ne fait rien tant
  // que `imgui_enabled_` est faux.
  using namespace hooking;
  g_trampoline = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kShowEntityContextMenu),
      reinterpret_cast<uint8_t*>(&ShowEntityContextMenuDetour));
  if (!g_trampoline) {
    LogDiag("[EntityContextMenu] detour NON pose (prologue non relocalisable)");
  }
  // 🔴 Ce second détour N'EST PAS gardé par `imgui_enabled_` : le blocage d'un
  // NPC est une préférence de jeu, pas un habillage, et l'éteindre avec
  // l'interface moderne ferait « oublier » au client des NPC que le joueur avait
  // rendus sourds. Il ne coûte rien tant que la liste est vide (cf.
  // ShouldIgnoreWorldClick), et le panneau garde la liste débrayable.
  g_trampoline_route = HookManager::Instance().SetHook(
      HookType::kJmpHook, reinterpret_cast<uint8_t*>(kRouteHoverAndClick),
      reinterpret_cast<uint8_t*>(&RouteHoverAndClickDetour));
  if (!g_trampoline_route) {
    LogDiag(
        "[EntityContextMenu] detour clic-monde NON pose : le blocage des NPC "
        "reste sans effet");
  }
}

void EntityContextMenu::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  // Un changement de map/mode invalide la cible : le menu ne doit pas survivre
  // à l'entité qu'il désigne.
  open_ = request_open_ = false;
  items_.clear();
  target_aid_ = 0;
  pending_code_ = 0;
  pending_local_ = Local::kNone;
  // Une question posée sur une entité d'une autre carte n'a plus de réponse
  // sensée : le GID peut déjà désigner autre chose. (La modale, elle, se ferme
  // d'elle-même : `confirm_local_` remis à zéro, DrawConfirmModal la referme.)
  confirm_request_ = false;
  confirm_local_   = Local::kNone;
  confirm_aid_     = 0;

  // 🔴 Les rappels de blocage se rearment en QUITTANT le monde, pas à chaque
  // carte. Cet événement est émis pour les deux, et vider ici sans distinguer
  // ferait réexpliquer le même NPC à chaque aller-retour de warp — soit tout
  // sauf « une fois par session ».
  if (mode_type != ModeMgr::ModeType::kGame) warned_this_session_.clear();
}

// ── Interception ─────────────────────────────────────────────────────────────

bool EntityContextMenu::OnNativeContextMenu(void* game_mode, const int* quad,
                                            int blocked) {
  if (!game_mode) return true;

  // Les gardes de CONTEXTE du natif (docs §4.1), moins celle sur l'état du
  // bouton : celle-là vient plus bas, parce qu'on a désormais deux choses à
  // faire à deux instants différents du même clic. Elles ne sont pas
  // cosmétiques : sans elles, le menu s'ouvrirait pendant un drag, par-dessus
  // une fenêtre native, ou pendant un replay.
  if (ReadGlobalInt(rag::kReplayActiveAddr)) return true;
  if (ReadGlobalInt(kMouseLButtonState) == kBtnHeld) return true;
  if ((GetAsyncKeyState(VK_SHIFT) >> 8) != 0) return true;  // Maj = attaque forcée
  if (!quad) return true;
  if (blocked) return true;
  if (ReadGlobalPtr(uiwnd::kStorageWndSlot)) return true;

  // Ce détour tourne à CHAQUE frame, y compris quand la souris ne fait que
  // survoler. Trois états du bouton droit nous concernent : l'appui (qu'il faut
  // désarmer), sa répétition, et le relâchement (qui ouvre). Pour tous les
  // autres, on sort avant de classifier — ce qui évite d'appeler des prédicats
  // natifs soixante fois par seconde pour rien.
  const int rbutton = ReadGlobalInt(kMouseRButtonState);
  if (rbutton != kBtnPressed && rbutton != kBtnRepeat && rbutton != kBtnReleased)
    return true;

  const uint32_t aid = static_cast<uint32_t>(quad[6]);
  const uint32_t job = static_cast<uint32_t>(quad[7]);
  const int      cat = quad[8];

  const Kind kind = ClassifyTarget(game_mode, aid, job, cat);
  if (kind == Kind::kNone) return true;

  // Les entités que le client n'a JAMAIS servies restent derrière leur opt-in.
  const bool native_served = (kind == Kind::kPlayer || kind == Kind::kPet ||
                              kind == Kind::kHomunculus || kind == Kind::kMercenary);
  // 🔴 Exception : un NPC à identifiant fixe ouvre son menu même sans « toutes
  // les entités ». Ce menu est le SEUL endroit d'où l'on coche et décoche le
  // blocage du clic ; le laisser derrière un autre réglage rendrait la case
  // introuvable — et pire, un NPC bloqué le resterait sans aucun moyen visible
  // de revenir en arrière depuis le monde.
  const bool blockable_npc =
      (kind == Kind::kNpc) && npc_block_enabled_ && IsFixedIdNpc(aid);
  // 🔴 Le staff outillé ouvre sur TOUT, sans passer par « toutes les entités ».
  // La section staff porte l'inspecteur de propriétés, et un inspecteur qu'il
  // faut armer d'un SECOND réglage — un réglage de joueur, en plus — est un
  // inspecteur qu'on ne trouve pas : on clique droit sur le monstre qui se
  // comporte mal, il ne se passe rien, et rien ne dit pourquoi. C'est d'ailleurs
  // ce que l'infobulle de « Outils du staff » promettait déjà.
  const bool staff_tools = staff_extras_ && IsStaff();
  if (!native_served && !all_entities_ && !blockable_npc && !staff_tools)
    return true;

  // 🔴 SOI-MÊME : le menu n'y porte que « Copier mon nom » (plus les
  // identifiants sous le réglage staff). Or on se clique dessus sans le vouloir
  // — en pleine foule, ou à côté de la cible visée — et le clic droit part
  // alors dans un menu d'une ligne. Décoché, on sort ICI, c'est-à-dire AVANT la
  // neutralisation du bouton droit plus bas : le clic repart au client intact,
  // rien n'est avalé.
  if (kind == Kind::kSelf && !self_menu_) return true;

  // 🔴 Une unité de compétence, le pet d'autrui ou une entité non classée n'ont
  // AUCUNE action de jeu : il n'en reste que de l'identité brute (nom, GID, quad
  // de pick), c'est-à-dire un outil de débogage. Pour un joueur, ce menu ne
  // ferait qu'avaler son clic droit sans rien lui offrir — il est donc réservé
  // au réglage staff, et le clic repart au natif pour tout le monde d'autre.
  const bool diagnostic_only = (kind == Kind::kSkillUnit || kind == Kind::kOther);
  if (diagnostic_only && !staff_tools) return true;

  // 🔴 Alt + clic droit sur un MONSTRE = ordre à l'homoncule (Alt+Maj = au
  // mercenaire) : `CursorMgr_UpdateHover` lit l'appui droit pour ça
  // (docs §7). On ne s'en mêle pas — ni menu, ni neutralisation.
  if ((GetAsyncKeyState(VK_MENU) >> 8) != 0 && kind == Kind::kMonster) return true;

  // ── 🔴 Le clic droit ne peut pas faire DEUX choses à la fois ────────────────
  // Le menu s'ouvre au RELÂCHEMENT (état 3) ; l'interaction du monde, elle, part
  // à l'APPUI (état 1) — une frame plus tôt, dans `CursorMgr_UpdateHover`, qui
  // tourne APRÈS nous dans la même passe souris. Sans ce qui suit, un clic droit
  // sur un PNJ ouvrait le dialogue ET notre menu (bug remonté en jeu).
  //
  // On efface donc l'état du bouton droit pour le RESTE de la frame, et
  // seulement quand ce clic nous appartient. Portée exacte, vérifiée sur les
  // seize lecteurs de ce global :
  //   · `UIWindowMgr_DispatchMouseInput` et `Camera_DragControl` l'ont déjà lu
  //     (ils passent AVANT nous) -> l'interface native et la rotation de caméra
  //     ne changent pas d'un pouce ;
  //   · `CursorMgr_UpdateHover`, `GroundClick_RequestMove`, `RepeatActorAction`
  //     passent APRÈS -> c'est eux, et eux seuls, qu'on désarme ;
  //   · on n'efface QUE l'appui (1 et 4). L'état « maintenu » (2) reste intact,
  //     donc rien de ce qui dépend d'un droit tenu ne bouge.
  // Le prochain `Mouse_UpdateFrameState` recalcule tout depuis les octets BRUTS
  // du WndProc : notre écriture ne survit pas à la frame, et le relâchement
  // produira bien son état 3.
  if (rbutton == kBtnPressed || rbutton == kBtnRepeat) {
    WriteGlobalInt(kMouseRButtonState, 0);
    return true;
  }
  // Reste le seul état encore possible : le relâchement, qui ouvre le menu.
  FillTargetAndOpen(game_mode, aid, job, cat, kind);
  return true;
}

// Ouvre le menu sur une entité DÉSIGNÉE AUTREMENT QUE PAR LA SOURIS — le clic
// droit sur un cadre du HUD de cible, qui se comporte comme l'entité elle-même.
//
// 🔴 Les gardes de CONTEXTE de `OnNativeContextMenu` (état des boutons, quad de
// pick, fenêtre native sous le curseur, neutralisation du clic droit) n'ont
// aucun sens ici : elles servent à décider si un clic droit du MONDE nous
// appartient. Celui-ci nous appartient par construction — il a été reçu par
// notre propre cadre, et ImGui l'a déjà retiré au jeu.
//
// 🔴 Le gate « toutes les entités » est également sauté, et c'est délibéré : il
// existe pour ne pas AVALER un clic droit du monde là où le client n'ouvrait
// rien. Sur le HUD, le geste est sans ambiguïté — on vise un cadre qui n'a pas
// d'autre usage — et le refuser laisserait un clic droit mort sans rien dire
// pourquoi. Reste `imgui_enabled_` : sans lui, il n'y a pas de menu du tout.
bool EntityContextMenu::OpenForEntity(void* game_mode, uint32_t aid,
                                      uint32_t job, int cat) {
  if (!imgui_enabled_ || !game_mode || aid == 0) return false;
  const Kind kind = ClassifyTarget(game_mode, aid, job, cat);
  if (kind == Kind::kNone) return false;
  if (kind == Kind::kSelf && !self_menu_) return false;
  FillTargetAndOpen(game_mode, aid, job, cat, kind);
  return request_open_;
}

// Le corps commun aux deux entrées : relever ce qu'il faut savoir de la cible,
// construire les entrées, demander l'ouverture. Extrait pour que les deux
// chemins ne puissent pas diverger — c'est ici que vivent le nom, l'objet au
// sol, l'appartenance et la liste d'amis.
void EntityContextMenu::FillTargetAndOpen(void* game_mode, uint32_t aid,
                                          uint32_t job, int cat, Kind kind) {
  target_aid_  = aid;
  target_job_  = job;
  target_cat_  = cat;
  kind_        = kind;
  target_item_id_         = 0;
  target_item_identified_ = false;
  if (kind == Kind::kGroundItem) {
    // 🔴 Pas de EntityName ici : le dictionnaire des plaques ne connaît que des
    // acteurs, et un GID inconnu déclencherait une DEMANDE au serveur pour un
    // flooritem. Le nom vient de la DB de descriptions (itemcell::NameById,
    // UTF-8, jamais nul) — mais SEULEMENT si l'objet est identifié : c'est le
    // VRAI nom, et le jeu cache encore celui d'un équipement tombé non
    // identifié. En-tête anonyme sinon (« Objet au sol (aid) »), comme pour
    // toute entité sans nom.
    ReadGroundItem(game_mode, aid, &target_item_id_, &target_item_identified_);
    target_name_.clear();
    if (target_item_id_ != 0 && target_item_identified_)
      target_name_ = itemcell::NameById(target_item_id_);
  } else {
    target_name_ = EntityName(game_mode, aid);
  }

  // Ce que la cible est DÉJÀ : le menu grise ce qui n'aurait aucun sens plutôt
  // que de le retirer (le natif, lui, retirait l'entrée — le joueur ne pouvait
  // pas distinguer « pas invitable » de « pas encore chargé »). Lu ici, une
  // seule fois par ouverture, et seulement quand la question se pose.
  target_in_party_ = false;
  target_guild_id_ = 0;
  if (kind == Kind::kPlayer) {
    target_in_party_  = OwnPartyContains(aid);
    target_has_party_ = target_in_party_ || TargetHasParty(game_mode, aid);
    target_guild_id_  = gamescene::ActorGuildId(FindActor(game_mode, aid));
    // Amis et ignorés se cherchent par nom BRUT, pas par sa conversion UTF-8.
    char wire_name[64] = {};
    ReadNameFieldRaw(game_mode, aid, gamescene::kNameStr, wire_name, sizeof(wire_name));
    target_is_friend_    = FriendListContainsName(wire_name);
    target_chat_blocked_ = ChatBlockListContains(wire_name);
  }

  BuildItems();
  if (items_.empty()) return;

  request_open_ = true;
  request_tick_ = GetTickCount();
}

EntityContextMenu::Kind EntityContextMenu::ClassifyTarget(void* game_mode,
                                                          uint32_t aid,
                                                          uint32_t job,
                                                          int category) const {
  if (aid == 0) return Kind::kNone;

  // Le pick tranche en premier : il sait des choses que le job ignore (un NPC
  // scripté sous forme de joueur porte un job de joueur — le cas qui a servi de
  // référence pendant la RE).
  if (category == kPickSkillUnit) return Kind::kSkillUnit;
  // 🔴 Seul CItem émet la catégorie 1 (cf. le bloc des catégories) : c'est un
  // objet au sol, PAS un NPC — l'ancienne lecture donnait aux drops le menu
  // d'un NPC, « Interagir » et outillage admin compris. Les vrais NPC arrivent
  // en catégorie 0 et se classent plus bas, au job ou au prédicat natif.
  if (category == kPickGroundItem) return Kind::kGroundItem;

  // Ses propres compagnons, avant tout test de job : ce sont les seuls cas où le
  // natif ouvrait autre chose qu'un menu de joueur.
  {
    void* session = reinterpret_cast<void*>(rag::kSessionAddr);
    if (aid == static_cast<uint32_t>(ReadGlobalInt(rag::pet::kOwnPetAidAddr))) {
      // La condition d'entrée du menu pet natif est, mot pour mot,
      // `quad[6] == g_Own_PetAid && acteur[0x314] == 7` (@0x00c6ecdb). Sa
      // seconde moitié, le quad la porte DÉJÀ : la catégorie 3 n'est écrite que
      // sous ce même octet. On la lit donc d'abord, et on ne va chercher
      // l'acteur qu'en second recours — un pointeur de moins à obtenir, c'est
      // une façon de moins de perdre le menu en silence.
      if (category == kPickPet) return Kind::kPet;
      void* actor = FindActor(game_mode, aid);
      if (actor && Read<uint8_t>(actor, kActor_Type) == 7) return Kind::kPet;
    }
    if (aid == Read<uint32_t>(session, kSess_HomunAid)) return Kind::kHomunculus;
    if (aid == Read<uint32_t>(session, kSess_MercAid)) return Kind::kMercenary;
  }

  // 🔴 Le pet d'AUTRUI arrive ici, en catégorie 3 lui aussi. Il porte un job de
  // MONSTRE (le pet EST un mob id) et se ferait donc classer « monstre » un peu
  // plus bas — avec un « Attaquer » que le serveur refusera, et une fiche de
  // monstre sur une créature apprivoisée. Le natif ne lui ouvrait rien du tout ;
  // on n'en garde que l'identité, c'est-à-dire le menu de diagnostic du staff.
  if (category == kPickPet) return Kind::kOther;

  if (aid == rag::OwnAccountId()) return Kind::kSelf;

  // 🔴 Un GID de la plage réservée ne peut être qu'un NPC épinglé, QUEL QUE SOIT
  // le sprite qu'il porte : un NPC déguisé (`@eventdisguise`) garde un job de
  // monstre et se ferait sinon classer « monstre » — donc sans sa case de
  // blocage, et avec un « Attaquer » qui n'a aucun sens sur un script. Le test
  // vient après soi-même et les compagnons, les seules identités qu'on ne veut
  // en aucun cas voir réécrites.
  if (IsFixedIdNpc(aid)) return Kind::kNpc;

  if (category == kPickSpecial || rag::IsSpecialUnitJob(job)) return Kind::kOther;
  if (rag::IsMonsterJob(job)) return Kind::kMonster;
  // 🔴 APRÈS le monstre, AVANT le joueur. Un PNJ scripté porte souvent une
  // classe de JOUEUR (kafra, marchand d'événement, PNJ de quête) : c'est ce
  // prédicat que le natif consulte pour REFUSER son menu joueur (docs §4.1).
  // Le placer après `IsMonsterJob` garantit qu'un monstre reste un monstre —
  // ce prédicat est vrai aussi pour des types d'acteur particuliers.
  if (IsHostileOrSpecialUnit(aid, job)) return Kind::kNpc;
  if (rag::IsNpcOrPortalJob(job)) return Kind::kNpc;
  if (rag::IsPlayerJob(job)) return Kind::kPlayer;
  return Kind::kOther;
}

// ── Blocage du clic sur un NPC à identifiant fixe ────────────────────────────

bool EntityContextMenu::IsFixedIdNpc(uint32_t gid) {
  return gid >= kFixedNpcGidFirst && gid <= kFixedNpcGidLast;
}

bool EntityContextMenu::IsNpcClickBlocked(uint32_t gid) const {
  if (!npc_block_enabled_ || blocked_npcs_.empty()) return false;
  return blocked_npcs_.find(gid) != blocked_npcs_.end();
}

bool EntityContextMenu::ShouldIgnoreWorldClick(const int* quad) {
  // Le cas courant — aucun NPC bloqué — sort en deux tests, sans toucher au
  // quad : ce détour tourne à chaque frame de la passe souris.
  if (!npc_block_enabled_ || blocked_npcs_.empty() || !quad) return false;

  // 🔴 Seuls les états qui DÉCLENCHENT une action sont neutralisés. Le survol
  // (état 0) et le relâchement (état 3) passent intacts, donc la plaque de nom
  // et le curseur du NPC restent ceux du client : un NPC bloqué se voit et se
  // vise toujours, il ne répond simplement plus. Côté gauche on prend aussi le
  // maintien (2) et la répétition (4), sans quoi garder le bouton enfoncé en
  // passant sur le NPC rendrait la marche saccadée ; côté droit on prend l'appui
  // et sa répétition, les deux états sur lesquels `CursorMgr_UpdateHover` émet
  // CZ_CONTACTNPC pour une entité « hostile/spéciale » (docs §7.1) — le menu
  // contextuel efface déjà cet état quand il prend le clic, ceci couvre les cas
  // où il décline (Maj enfoncée, par exemple).
  const int lbutton = ReadGlobalInt(kMouseLButtonState);
  const int rbutton = ReadGlobalInt(kMouseRButtonState);
  const bool acting =
      lbutton == kBtnPressed || lbutton == kBtnHeld || lbutton == kBtnRepeat ||
      rbutton == kBtnPressed || rbutton == kBtnRepeat;
  if (!acting) return false;

  const uint32_t gid = static_cast<uint32_t>(quad[6]);
  if (!IsNpcClickBlocked(gid)) return false;

  // 🔴 Dire POURQUOI il ne se passe rien. Un blocage survit au fichier de
  // réglages : le joueur peut cliquer, des semaines plus tard, un NPC muet dont
  // il a oublié qu'il l'avait coché — sans un mot, c'est un client qui a l'air
  // cassé. Une ligne au premier clic avalé de la session, par NPC.
  //
  // Seulement sur l'APPUI NEUF du bouton gauche : un maintien qui traverse le
  // NPC en marchant n'est pas une tentative d'interaction, et le clic droit
  // ouvre de toute façon le menu où la case se voit cochée.
  //
  // ⚠ On est dans la passe souris du client, PAS dans une frame ImGui : c'est
  // exactement d'ici que le natif écrit ses propres refus (msg 4027 de
  // CursorMgr_UpdateHover). Rien à différer.
  if (lbutton == kBtnPressed && warned_this_session_.insert(gid).second) {
    const auto entry = blocked_npcs_.find(gid);
    char who[96];
    FormatNpcLabel(gid,
                   entry != blocked_npcs_.end() ? entry->second.c_str() : nullptr,
                   who, sizeof(who));
    char line[256];
    std::snprintf(line, sizeof(line),
                  i18n::Tr("%s : vous avez bloqué le clic gauche sur ce NPC. "
                           "Clic droit dessus pour le débloquer."),
                  who);
    SayToChat(line);
  }
  return true;
}

void EntityContextMenu::ToggleNpcBlock(uint32_t gid) {
  if (!IsFixedIdNpc(gid)) return;

  char who[96];
  FormatNpcLabel(gid, target_name_.c_str(), who, sizeof(who));

  char line[256];
  const auto it = blocked_npcs_.find(gid);
  if (it != blocked_npcs_.end()) {
    blocked_npcs_.erase(it);
    warned_this_session_.erase(gid);
    std::snprintf(line, sizeof(line),
                  i18n::Tr("%s : le clic gauche est de nouveau actif."), who);
  } else {
    blocked_npcs_[gid] = target_name_;
    // Marqué comme déjà expliqué : le joueur vient de cocher la case, la ligne
    // ci-dessous lui dit tout. Sans ça, son premier clic de vérification
    // rejouerait la même explication deux secondes plus tard.
    warned_this_session_.insert(gid);
    std::snprintf(line, sizeof(line),
                  i18n::Tr("%s : le clic gauche ne fait plus rien sur lui. Clic "
                           "droit pour le débloquer ou lui parler."),
                  who);
  }
  SayToChat(line);
  if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
}

void EntityContextMenu::ToggleAloot(uint32_t nameid) {
  auto* mu = Bourgeon::Instance().moonlight_ui();
  if (!mu || nameid == 0) return;
  // Aucune ligne de chat ici : MoonlightUi notifie le serveur, dont la réponse
  // (ajouté / retiré / liste pleine) revient déjà par le canal des atcommands.
  if (mu->IsAlootId(nameid)) mu->RemoveAlootId(nameid);
  else                       mu->AddAlootId(nameid);
}

// ── Construction des entrées ─────────────────────────────────────────────────

void EntityContextMenu::BuildItems() {
  items_.clear();
  const bool staff = staff_extras_ && IsStaff();
  const uint32_t own_aid = rag::OwnAccountId();
  const bool client_admin = ClientAdminListContains(own_aid);

  auto add = [&](const char* label, int code, Local local = Local::kNone,
                 bool separator = false, bool is_staff = false,
                 const char* tip = nullptr) {
    Item item;
    item.label = label;
    item.code = code;
    item.local = local;
    item.separator = separator;
    item.staff = is_staff;
    if (tip) item.tip = tip;
    items_.push_back(std::move(item));
  };
  // Grise la dernière entrée ajoutée, avec la raison en infobulle.
  auto disable_last = [&](const char* why) {
    items_.back().disabled = true;
    items_.back().tip = why;
  };

  switch (kind_) {
    case Kind::kPlayer: {
      const uint32_t own_guild = static_cast<uint32_t>(rag::OwnGuildId());
      const bool in_guild  = own_guild != 0;
      const bool is_master = in_guild && ReadGlobalInt(rag::kGuildIsMasterAddr) != 0;
      const bool in_party  = ReadGlobalInt(kInPartyFlag) != 0;

      add(i18n::Tr("Voir l'équipement"), kCodeViewEquip);
      add(i18n::Tr("Proposer un échange"), kCodeDeal);
      // Les deux invitations restent VISIBLES et grisées quand elles n'ont pas
      // de sens : le natif les faisait disparaître, ce qui laissait croire que
      // le client n'en était pas capable. La raison part en infobulle.
      if (in_party) {
        add(i18n::Tr("Inviter dans le groupe"), kCodePartyInvite);
        // Deux refus différents, deux phrases différentes : « il est déjà avec
        // moi » n'appelle pas la même réaction que « il faudra qu'il quitte son
        // groupe ».
        if (target_in_party_) {
          disable_last(i18n::Tr("Déjà membre de votre groupe."));
        } else if (target_has_party_) {
          disable_last(
              i18n::Tr("Ce joueur appartient déjà à un groupe : le serveur refuse "
              "l'invitation tant qu'il ne l'a pas quitté."));
        }
      }
      if (in_guild) {
        add(i18n::Tr("Inviter dans la guilde"), kCodeGuildInvite);
        // Le serveur refuse un invité qui a déjà une guilde ; le natif le savait
        // et retirait l'entrée. (Il exigeait en plus le droit d'invitation,
        // `dword_159C234` — non repris : ce flag n'est pas tranché en RE, et
        // s'en servir à tort masquerait l'entrée à qui y a droit. À mesurer.)
        if (target_guild_id_ != 0) disable_last(i18n::Tr("Ce joueur a déjà une guilde."));
      }
      if (is_master) {
        // Les deux visent la GUILDE de la cible : sans guilde, il n'y a rien à
        // allier ni à déclarer ennemi, et sur la nôtre ça n'a aucun sens. Le
        // natif ne testait que « pas notre guilde » et laissait donc l'alliance
        // cliquable sur un joueur sans guilde.
        const char* guild_target_issue =
            (target_guild_id_ == 0)          ? i18n::Tr("Ce joueur n'a pas de guilde.")
            : (target_guild_id_ == own_guild) ? i18n::Tr("C'est votre propre guilde.")
                                              : nullptr;
        add(i18n::Tr("Proposer une alliance"), kCodeGuildAlly, Local::kNone, true);
        if (guild_target_issue) disable_last(guild_target_issue);
        add(i18n::Tr("Déclarer la guilde ennemie"), kCodeGuildFoe);
        if (guild_target_issue) disable_last(guild_target_issue);
      }
      // ── Chuchoter, en DEUX gestes ─────────────────────────────────────────
      // Le premier ne fait que préparer l'envoi : le nom va dans la box
      // destinataire de la barre de chat, et on écrit. Le second — juste en
      // dessous, parce que c'est le même geste en plus engageant — ouvre la
      // conversation dans sa propre fenêtre.
      //
      // 🔴 L'ordre n'est pas cosmétique. Ouvrir une fenêtre était le comportement
      // par défaut, y compris chez qui avait DÉCOCHÉ les fenêtres individuelles
      // du « Friend Setup » : ces cases ne gouvernaient que les chuchotements
      // REÇUS, et un clic de menu passait outre. La fenêtre est désormais une
      // entrée à part, qu'on choisit.
      add(i18n::Tr("Chuchoter"), kCodeWhisper, Local::kWhisperBar, true);
      add(i18n::Tr("Chuchoter dans une fenêtre"), kCodeWhisper, Local::kWhisperWindow,
          false, false,
          i18n::Tr("Ouvre une conversation à part, avec son propre historique et sa "
                   "propre ligne de saisie."));
      add(i18n::Tr("Ajouter en ami"), kCodeAddFriend);
      if (target_is_friend_) disable_last(i18n::Tr("Déjà dans votre liste d'amis."));
      add(i18n::Tr("Envoyer un courrier"), kCodeSendMail);
      // Même condition que le natif : le dispatcher, lui, ne la rejoue pas, et
      // la demande partirait au serveur pour se faire refuser.
      if (AdoptionEligible(target_aid_)) add(i18n::Tr("Adopter"), kCodeAdopt);
      // UNE bascule, pas deux entrées : le blocage est un ÉTAT, et on sait le
      // lire (la liste d'ignorés du client). En proposer deux, c'était en offrir
      // une qui ne fait jamais rien.
      // L'infobulle dit ce que le libellé natif cache : ça n'agit QUE sur les
      // chuchotements. Le client émet CZ_SETTING_WHISPER_PC (0x00CF) et le
      // serveur ne consulte `sd->ignore[]` que sur le chemin du chuchotement
      // (moonlight clif.cpp:14795, intif.cpp:1301).
      add(target_chat_blocked_ ? i18n::Tr("Autoriser les chuchotements") : i18n::Tr("Bloquer les chuchotements"),
          target_chat_blocked_ ? kCodeUnblockChat : kCodeBlockChat,
          Local::kNone, true, false,
          i18n::Tr("Liste d'ignorés du compte : ses chuchotements ne vous parviennent "
          "plus. Le chat public, le groupe et la guilde ne sont pas filtrés."));
      add(i18n::Tr("Signaler ce joueur"), kCodeReportUser);
      add(i18n::Tr("Copier le nom"), 0, Local::kCopyName, true);
      break;
    }
    case Kind::kSelf:
      add(i18n::Tr("Copier mon nom"), 0, Local::kCopyName);
      break;
    case Kind::kMonster:
      add(i18n::Tr("Attaquer"), 0, Local::kAttack);
      add(i18n::Tr("Fiche du monstre"), 0, Local::kMonsterInfo, true);
      // Le lien de chat n'est proposé que s'il y a une barre pour l'accueillir :
      // promettre un geste qui ne peut rien faire est pire que de se taire.
      if (links::CanPostToChat())
        add(i18n::Tr("Linker ce monstre"), 0, Local::kChatLinkMob);
      add(i18n::Tr("Copier le nom"), 0, Local::kCopyName);
      break;
    case Kind::kNpc: {
      // Pas d'« Interagir » sur un PORTAIL : le natif sort avant toute action
      // dès que le job vaut 45 (docs §7), et un warp n'a pas de dialogue.
      // ⚠ Cette entrée-là RESTE active sur un NPC bloqué, et c'est voulu : le
      // blocage vise le clic accidentel, pas la volonté de parler. Elle envoie
      // CZ_CONTACTNPC directement, sans repasser par le routeur détourné.
      if (target_job_ != rag::kJobPortal) add(i18n::Tr("Interagir"), 0, Local::kTalkToNpc);
      // ⚠ Pas sur un PORTAIL, et pas sur un anonyme : le lien est une RECHERCHE
      // par le nom, et un warp n'en a pas d'utile. Chercher « » ouvrirait le
      // panneau sur rien.
      if (target_job_ != rag::kJobPortal && !target_name_.empty() &&
          links::CanPostToChat())
        add(i18n::Tr("Linker ce NPC"), 0, Local::kChatLinkNpc);
      add(i18n::Tr("Copier le nom"), 0, Local::kCopyName, true);
      // La case de blocage, seulement sur les NPC dont le GID est épinglé : les
      // autres changent d'identifiant à chaque redémarrage du map-server, une
      // case cochée sur eux désignerait n'importe qui le lendemain.
      if (npc_block_enabled_ && IsFixedIdNpc(target_aid_)) {
        Item item;
        item.label     = i18n::Tr("Bloquer le clic gauche###ctxmenu_npcblock");
        item.toggle    = true;
        item.checked   = IsNpcClickBlocked(target_aid_);
        item.separator = true;
        item.tip = i18n::Tr(
            "Coché, le clic gauche sur ce NPC ne fait plus rien du tout : ni "
            "dialogue, ni déplacement. Le menu du clic droit reste disponible, "
            "« Interagir » compris, et viser une zone avec un sort continue de "
            "marcher par-dessus lui.\n"
            "Réglage du client, gardé sur cette machine : il ne suit pas le "
            "personnage, et un script déclenché en MARCHANT dessus s'exécutera "
            "toujours.");
        items_.push_back(std::move(item));
      }
      break;
    }
    case Kind::kGroundItem: {
      // Le natif n'a JAMAIS eu de menu ici : son clic droit sur un CItem ne
      // fait rien du tout. Tout est donc local — et calqué sur le menu d'un
      // LIEN d'item (table des drops de la fiche de monstre), qui est la même
      // question posée ailleurs : « cet objet, qu'est-ce que j'en fais ? ».
      add(i18n::Tr("Ramasser"), 0, Local::kPickupItem, false, false,
          i18n::Tr("Comme le clic gauche : le personnage s'approche et ramasse. "
                   "Le serveur revalide la distance et le droit de butin."));
      // 🔴 Les entrées qui RÉVÈLENT l'identité sont grisées quand elle n'est
      // pas encore connue : un équipement tombé d'un monstre arrive NON
      // IDENTIFIÉ, sa plaque montre le nom générique, et le menu n'a pas à
      // trahir ce que le jeu cache encore. Grisées et non retirées — la règle
      // de ce menu : dire pourquoi plutôt que faire disparaître.
      const bool known = target_item_id_ != 0 && target_item_identified_;
      const char* why_hidden =
          (target_item_id_ == 0)
              ? i18n::Tr("Objet illisible : il vient sans doute d'être ramassé.")
              : i18n::Tr("Objet non identifié : le jeu ne dit pas encore ce que c'est.");
      add(i18n::Tr("Description"), 0, Local::kItemDesc);
      if (!known) disable_last(why_hidden);
      if (links::CanPostToChat()) {
        add(i18n::Tr("Linker cet objet"), 0, Local::kChatLinkItem);
        if (!known) disable_last(why_hidden);
      }
      // Ramassage automatique : la MÊME liste que l'overlay de description et
      // le menu des liens (elle vit dans MoonlightUi, synchronisée serveur) —
      // surtout pas une seconde copie qui divergerait.
      if (Bourgeon::Instance().moonlight_ui()) {
        Item aloot;
        aloot.label     = i18n::Tr("Ramassage automatique###ctxmenu_aloot");
        aloot.local     = Local::kAlootToggle;
        aloot.toggle    = true;
        aloot.checked   = known && Bourgeon::Instance().moonlight_ui()->IsAlootId(
                                       target_item_id_);
        aloot.separator = true;
        aloot.disabled  = !known;
        aloot.tip = known
                        ? i18n::Tr("La liste @alootid du compte — la même que dans la "
                                   "description et les liens : ces objets vont droit "
                                   "dans l'inventaire quand ils tombent pour vous.")
                        : why_hidden;
        items_.push_back(std::move(aloot));
      }
      // Commandes serveur, par le pipeline COMPLET du chat (mêmes règles qu'une
      // ligne tapée) : leur réponse revient dans le chat.
      add("@iteminfo", 0, Local::kCmdItemInfo, true);
      if (!known) disable_last(why_hidden);
      add("@whodrops", 0, Local::kCmdWhoDrops);
      if (!known) disable_last(why_hidden);
      break;
    }
    case Kind::kPet:
      add(i18n::Tr("Statut du pet"), kCodePetStatus);
      add(i18n::Tr("Nourrir"), kCodePetFeed);
      add(i18n::Tr("Spectacle"), kCodePetPerform);
      add(i18n::Tr("Retirer l'accessoire"), kCodePetUnequip);
      add(i18n::Tr("Remettre dans l'œuf"), kCodePetToEgg, Local::kNone, true);
      break;
    case Kind::kHomunculus:
      add(i18n::Tr("Statut de l'homoncule"), kCodeHomunStatus);
      add(i18n::Tr("Nourrir"), kCodeHomunFeed);
      add(i18n::Tr("En attente"), kCodeHomunStandBy);
      break;
    case Kind::kMercenary:
      add(i18n::Tr("Statut du mercenaire"), kCodeMercStatus);
      add(i18n::Tr("En attente"), kCodeMercStandBy);
      break;
    case Kind::kSkillUnit:
    case Kind::kOther:
      // Ces deux-là ne s'ouvrent que pour le staff (cf. `diagnostic_only` dans
      // OnNativeContextMenu) : le reste du menu est la section staff ci-dessous.
      add(i18n::Tr("Copier le nom"), 0, Local::kCopyName);
      break;
    default:
      break;
  }

  // ── Fenêtre de cible ───────────────────────────────────────────────────────
  // Proposée UNIQUEMENT quand la fenêtre est réellement affichée pour cette
  // entité : une entrée « masquer » sur une fenêtre absente n'apprendrait rien
  // et ferait douter de ce qui est ouvert. Elle ne coupe pas la fonction — le
  // prochain ciblage la ramène ; l'interrupteur, lui, est dans les réglages.
  if (auto* target_frame = Bourgeon::Instance().target_frame()) {
    if (target_frame->IsShownFor(target_aid_)) {
      add(i18n::Tr("Masquer la fenêtre de cible"), 0, Local::kHideTargetFrame, true,
          false,
          i18n::Tr("Pour cette cible seulement : elle revient dès que tu en "
                   "désignes une autre."));
    }
  }

  if (!staff) return;

  // ── Section staff, dans son SOUS-MENU ──────────────────────────────────────
  // Gate SERVEUR (niveau de groupe >= 80). Les trois entrées marquées « client
  // admin » ont EN PLUS une garde dans le dispatcher natif : sans notre AID dans
  // le clientinfo.xml, le client les rejette en silence — on ne les propose donc
  // pas plutôt que d'offrir un bouton mort (docs §4.2).
  //
  // Tout ce qui suit porte `submenu` : le rendu ouvre « Outils du staff » à la
  // première de ces lignes et le referme à la dernière. Elles se suivent donc
  // sans trou — une entrée de joueur glissée au milieu casserait la traînée et
  // se retrouverait DANS le sous-menu.
  //
  // ⚠ Le séparateur d'une entrée staff sépare À L'INTÉRIEUR du sous-menu ; c'est
  // le sous-menu lui-même qui porte celui du corps du menu. `sep` arme donc le
  // suivant, au lieu du `first` d'avant qui n'en posait qu'un seul, tout en haut.
  bool sep = false;
  auto add_staff = [&](const char* label, int code, Local local = Local::kNone,
                       const char* tip = nullptr, bool danger = false,
                       bool confirm = false) {
    add(label, code, local, sep, true, tip);
    items_.back().submenu = true;
    items_.back().danger  = danger;
    items_.back().confirm = confirm;
    sep = false;
  };
  // Ouvre un groupe : la prochaine entrée staff sera précédée d'un trait.
  auto staff_group = [&]() { sep = true; };

  // L'identifiant seul, puis l'identité brute de ce que le pick a désigné : AID,
  // job et catégorie. C'est exactement ce qu'on va chercher au débogueur quand
  // une entité se comporte mal — et ça vaut pour TOUTES les entités, y compris
  // celles que le client n'ouvrait pas. Un identifiant nu ne dit rien à un
  // joueur : ces deux lignes vivent ici, et nulle part ailleurs.
  const char* copy_id_label = (kind_ == Kind::kSelf)   ? i18n::Tr("Copier mon AID")
                              : (kind_ == Kind::kPlayer) ? i18n::Tr("Copier l'AID") : i18n::Tr("Copier le GID");
  // L'inspecteur EN PREMIER : c'est lui qu'on ouvre quand on ne sait pas encore
  // ce qu'on regarde, donc avant les entrées qui supposent de le savoir déjà.
  // Valable pour TOUTES les familles, sans exception — un inspecteur qui
  // refuserait les cibles bizarres manquerait les seules qui le méritent.
  add_staff(i18n::Tr("Propriétés…"), 0, Local::kInspect,
            i18n::Tr("Tout ce que le client sait de cette entité : plaque de "
                     "nom, acteur, position, adresses mémoire."));
  add_staff(copy_id_label, 0, Local::kCopyId);
  add_staff(i18n::Tr("Copier l'identité de pick"), 0, Local::kCopyPickInfo);

  if (kind_ == Kind::kPlayer) {
    add_staff(i18n::Tr("Journal des blocages de chat"), kCodeChatBanLog);

    // ── Le confort : déplacer et poser la cible ─────────────────────────────
    // Rien de puni ici. C'est ce qu'on fait pendant un event : rassembler du
    // monde, asseoir une file d'attente. Le serveur emprunte leur droit à
    // `recall` et `sitstand` — voir clif_parse_bourgeon_player_admin.
    staff_group();
    add_staff(i18n::Tr("Le faire venir ici"), 0, Local::kPlayerComeHere,
              i18n::Tr("Il MARCHE jusqu'à vous — il n'est pas téléporté : le "
                       "trajet reste visible pour lui comme pour les autres.\n"
                       "Sur votre carte seulement, et seulement s'il existe un "
                       "chemin. Le serveur le relève d'abord s'il est assis."));
    add_staff(i18n::Tr("L'asseoir ou le relever"), 0, Local::kPlayerSitStand,
              i18n::Tr("Une bascule : le serveur regarde dans quelle position il "
                       "est et pose l'autre."));
    add_staff(i18n::Tr("Points d'event…"), 0, Local::kPlayerEventPoints,
              i18n::Tr("Donne ou retire des points d'event (la monnaie kafra), au "
                       "plus 50 à la fois.\n"
                       "Le solde actuel n'est PAS ici : il se lit dans "
                       "« Propriétés… », que le serveur renseigne."),
              false, /*confirm=*/true);

    // ── La parole ───────────────────────────────────────────────────────────
    // 🔴 Deux entrées, pas une bascule. Le client ignore si la cible porte
    // SC_NOCHAT, et un joueur muet reste dans le monde à côté de celui qui
    // clique : une bascule aurait rendu la parole à qui venait d'être puni par
    // quelqu'un d'autre. `@mute 60` n'est d'ailleurs pas l'inverse d'`@unmute` —
    // elle AJOUTE 60 minutes. (La prison, elle, tient en UNE entrée : voir
    // l'en-tête.)
    staff_group();
    add_staff(i18n::Tr("Le rendre muet (60 min)"), 0, Local::kPlayerMute,
              i18n::Tr("Ajoute 60 minutes de silence. Appliqué deux fois, il "
                       "prolonge — ce n'est pas un interrupteur."));
    add_staff(i18n::Tr("Lui rendre la parole"), 0, Local::kPlayerUnmute);
    if (client_admin) {
      add_staff(i18n::Tr("Retirer tout l'équipement"), kCodeFullStrip);
      add_staff(i18n::Tr("Point de manière +"), kCodeMannerPlus);
      add_staff(i18n::Tr("Point de manière -"), kCodeMannerMinus);
    }

    // ── Ce qui punit ────────────────────────────────────────────────────────
    // En rouge, et en dernier : on ne clique pas là par erreur en visant la
    // ligne du dessus. « Expulser » est le code NATIF 28 — `clif_parse_GMKick`
    // rejoue déjà `@kick`, inutile d'ouvrir un second chemin.
    staff_group();
    add_staff(i18n::Tr("Le foudroyer"), 0, Local::kPlayerNuke,
              i18n::Tr("@nuke : une explosion sur sa case. Il meurt, avec la "
                       "pénalité de mort habituelle."),
              /*danger=*/true);
    add_staff(i18n::Tr("Expulser (kick)"), kCodeKick, Local::kNone,
              i18n::Tr("Le serveur revalide la permission avant d'agir."),
              /*danger=*/true);
    add_staff(i18n::Tr("L'emprisonner ou le libérer"), 0, Local::kPlayerJail,
              i18n::Tr("Une bascule : le serveur regarde s'il est déjà en prison "
                       "et fait l'inverse.\n"
                       "Un joueur emprisonné vit sur sec_pri — hors de cette "
                       "carte, cette entrée ne peut que l'y envoyer. Sans durée : "
                       "pour une peine bornée, @jailfor."),
              /*danger=*/true);
    add_staff(i18n::Tr("Bannir le compte…"), 0, Local::kPlayerBlock,
              i18n::Tr("@block : bannissement DÉFINITIF du compte, pas du "
                       "personnage. Tous ses personnages tombent avec lui."),
              /*danger=*/true, /*confirm=*/true);
  }

  // ── Outillage NPC de l'administrateur ──────────────────────────────────────
  // 🔴 `IsAdmin()` (niveau de groupe >= 99), pas `IsStaff()` : tout ce qui
  // précède AFFICHE, ces trois-là MODIFIENT le serveur pour tous les joueurs
  // connectés. Le serveur refait le test — le bouton absent n'est pas une garde.
  //
  // Un portail (job 45) est un NPC comme un autre pour ces actions : c'est même
  // le cas où l'on veut le plus pouvoir recharger le fichier sans redémarrer.
  if (kind_ == Kind::kNpc && IsAdmin()) {
    // Le rechargement en premier : c'est le geste courant (on vient d'éditer le
    // script), et le seul des trois qui ne retire rien.
    staff_group();
    add(i18n::Tr("Recharger son fichier de script"), 0, Local::kNpcReloadFile,
        sep, true,
        i18n::Tr("Recharge le FICHIER d'où vient ce NPC, pas seulement lui : tous "
                 "les NPC déclarés dans ce fichier sont déchargés puis relus, et "
                 "leurs événements OnInit rejoués.\n"
                 "Les mapflags et les monstres posés directement par le script ne "
                 "sont PAS retirés — ils s'ajouteront à ceux déjà en place.\n"
                 "Le serveur dit dans le chat ce qu'il a fait."));
    items_.back().submenu = true;
    add(i18n::Tr("Déplacer ici"), 0, Local::kNpcMoveHere, false, true,
        i18n::Tr("Pose le NPC sur votre case. Rien n'est enregistré : il "
                 "retrouvera sa position d'origine au prochain rechargement de "
                 "son fichier ou du serveur."));
    items_.back().submenu = true;
    Item unload;
    unload.label   = i18n::Tr("Décharger ce NPC…");
    unload.local   = Local::kNpcUnload;
    unload.staff   = true;
    unload.danger  = true;
    unload.confirm = true;
    unload.submenu = true;
    unload.tip = i18n::Tr(
        "Retire ce NPC du monde pour TOUS les joueurs connectés, avec ses "
        "duplicates. Il faudra recharger son fichier pour le faire revenir.");
    items_.push_back(std::move(unload));
  }
}

const char* EntityContextMenu::KindLabel(Kind kind) {
  switch (kind) {
    case Kind::kSelf:       return "Moi";
    case Kind::kPlayer:     return "Joueur";
    case Kind::kMonster:    return "Monstre";
    case Kind::kNpc:        return "NPC";
    case Kind::kPet:        return "Pet";
    case Kind::kHomunculus: return "Homoncule";
    case Kind::kMercenary:  return "Mercenaire";
    case Kind::kSkillUnit:  return i18n::Tr("Unité");
    case Kind::kGroundItem: return i18n::Tr("Objet au sol");
    default:                return i18n::Tr("Entité");
  }
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void EntityContextMenu::OnRenderUI() {
  if (!imgui_enabled_) return;

  if (request_open_) {
    const bool stale = (GetTickCount() - request_tick_) > 500u;
    request_open_ = false;
    // Interface masquée au moment du clic : la demande a péri. 🔴 On ne SORT pas
    // pour autant — la modale de confirmation vit plus bas et sauter sa frame la
    // fermerait en silence.
    //
    // 🔴 Et on n'ouvre RIEN tant qu'une confirmation est à l'écran. Le menu naît
    // de la passe souris du CLIENT, qu'une modale ImGui ne bloque pas : sans ce
    // test, un clic droit dans le monde appellerait `OpenPopup` au même niveau de
    // pile et REMPLACERAIT la modale — la question posée disparaîtrait sans
    // réponse, et sans que rien ne le dise.
    if (!stale && !ImGui::IsPopupOpen(ConfirmModalId(confirm_local_))) {
      open_ = true;
      ImGui::OpenPopup("##bourgeon_entity_ctx");
      // 🔴 La position vient d'ImGui, PAS des globaux souris du client
      // (`g_MouseScreenX/Y`, docs §2) : ceux-là sont en pixels CLIENT, et rien ne
      // garantit qu'ils partagent l'échelle du DisplaySize d'ImGui. Le menu
      // s'ouvre au relâchement du bouton, donc le curseur n'a pas bougé entre le
      // tick qui l'a décidé et cette frame.
      ImGui::SetNextWindowPos(ImGui::GetIO().MousePos, ImGuiCond_Always);
    }
  }
  if (open_) DrawPopup();
  // 🔴 Hors du popup, et appelée à CHAQUE frame : le menu est déjà fermé quand la
  // question se pose, et une modale qu'on cesse de dessiner disparaît.
  DrawConfirmModal();
}

void EntityContextMenu::DrawPopup() {
  // Serré : un menu contextuel se lit d'un coup d'œil, il ne se contemple pas.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 3.0f));
  const bool visible = ImGui::BeginPopup(
      "##bourgeon_entity_ctx",
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoSavedSettings);
  if (visible) {
    // En-tête : QUI est visé. Le natif ne le disait pas — il fallait se souvenir
    // de ce qu'on avait cliqué.
    const char* kind_label = KindLabel(kind_);
    if (target_name_.empty()) {
      ImGui::Text("%s (%u)", kind_label, target_aid_);
    } else {
      ImGui::Text("%s", target_name_.c_str());
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.35f, 0.35f, 0.35f, 1.0f), "· %s", kind_label);
    }
    ImGui::Separator();

    // Le corps du menu d'abord, puis le sous-menu staff d'un bloc : les entrées
    // qui le composent se suivent (cf. BuildItems), donc dès qu'on en rencontre
    // une, on les tient toutes jusqu'à la fin.
    bool close = false;
    for (size_t i = 0; i < items_.size() && !close;) {
      if (!items_[i].submenu) {
        if (items_[i].separator && i != 0) ImGui::Separator();
        close = DrawItem(i);
        ++i;
        continue;
      }

      // ── « Outils du staff » ────────────────────────────────────────────────
      // 🔴 `BeginMenu` et pas un second popup : ImGui gère lui-même l'ouverture
      // au survol, le placement à droite (ou à gauche si l'écran manque) et la
      // fermeture en chaîne — `CloseCurrentPopup` appelée depuis l'intérieur
      // referme AUSSI le menu parent, ce qui est exactement ce qu'on veut quand
      // une action est choisie.
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, kStaffColor);
      const bool opened = ImGui::BeginMenu(i18n::Tr("Outils du staff"));
      ImGui::PopStyleColor();
      size_t j = i;
      if (opened) {
        for (; j < items_.size() && items_[j].submenu && !close; ++j) {
          // Pas de garde `j != 0` ici : la première entrée du sous-menu n'est
          // jamais marquée (BuildItems part de `sep = false`), et si elle
          // l'était le trait serait de toute façon inoffensif en tête de menu.
          if (items_[j].separator) ImGui::Separator();
          close = DrawItem(j);
        }
        ImGui::EndMenu();
      }
      // Replié — ou interrompu par un clic — il reste à sauter ce qu'on n'a pas
      // dessiné : sans ça, la traînée staff repasserait dans la boucle et
      // rouvrirait un second sous-menu à chaque ligne restante.
      while (j < items_.size() && items_[j].submenu) ++j;
      i = j;
    }
    ImGui::EndPopup();
  } else {
    // ImGui a fermé le popup (clic hors, Échap) : on suit.
    open_ = false;
  }
  ImGui::PopStyleVar(2);
}

// Le dessin d'UNE ligne, partagé par le corps du menu et le sous-menu staff.
// Rend true quand le menu doit se fermer — c'est-à-dire quand une action a été
// choisie. Le séparateur, lui, appartient à l'appelant : lui seul sait si la
// ligne ouvre son bloc ou le menu tout entier.
bool EntityContextMenu::DrawItem(size_t index) {
  const Item& item = items_[index];
  // Une case à cocher bascule un ÉTAT : elle ne se « choisit » pas et ne ferme
  // donc pas le menu — on doit pouvoir cocher, puis parler au NPC dans la
  // foulée. Rien n'est différé ici : ToggleNpcBlock n'écrit qu'une ligne de chat
  // (la seule commande native sans danger en pleine frame) et ToggleAloot un
  // paquet Bourgeon — une écriture socket, comme le fait déjà le menu des liens
  // d'item au même endroit d'une frame.
  if (item.toggle) {
    bool checked = item.checked;
    if (item.disabled) ImGui::BeginDisabled();
    const bool flipped = ro::RoCheckbox(item.label.c_str(), &checked);
    if (item.disabled) ImGui::EndDisabled();
    if (flipped) {
      if (item.local == Local::kAlootToggle) {
        ToggleAloot(target_item_id_);
        // L'état vrai est celui de la LISTE, pas celui de la case : l'ajout
        // peut être refusé (liste pleine), et la case doit alors retomber.
        if (auto* mu = Bourgeon::Instance().moonlight_ui())
          items_[index].checked = mu->IsAlootId(target_item_id_);
      } else {
        ToggleNpcBlock(target_aid_);
        items_[index].checked = checked;
      }
    }
    if (!item.tip.empty() &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", item.tip.c_str());
    return false;
  }
  // Le rouge l'emporte sur l'ocre du staff : une entrée qui RETIRE quelque
  // chose au monde doit se distinguer de celles qui ne font que lire.
  if (item.danger) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDangerColor);
  } else if (item.staff) {
    ImGui::PushStyleColor(ImGuiCol_Text, kStaffColor);
  }
  if (item.disabled) ImGui::BeginDisabled();
  const bool clicked = ImGui::Selectable(
      item.label.c_str(), false, ImGuiSelectableFlags_NoAutoClosePopups);
  if (item.disabled) ImGui::EndDisabled();
  if (item.danger || item.staff) ImGui::PopStyleColor();
  // 🔴 `AllowWhenDisabled` : sans ce drapeau, une entrée grisée ne compte pas
  // comme survolée — et c'est justement celle dont il faut dire POURQUOI.
  if (!item.tip.empty() &&
      ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("%s", item.tip.c_str());
  }
  if (!clicked) return false;
  Choose(item);
  // 🔴 Depuis un sous-menu, `CloseCurrentPopup` remonte la chaîne et referme
  // AUSSI le menu parent : c'est le comportement d'ImGui pour les popups de
  // type menu, et c'est celui qu'on veut — le geste est terminé.
  ImGui::CloseCurrentPopup();
  open_ = false;
  return true;
}

// ── Exécution ────────────────────────────────────────────────────────────────

void EntityContextMenu::Choose(const Item& item) {
  // Une action à confirmer ne s'arme pas : elle pose sa question d'abord. La
  // cible est recopiée dès maintenant — le menu peut se rouvrir sur autre chose
  // pendant que la modale est à l'écran, et c'est bien le NPC nommé DANS la
  // question qui doit disparaître.
  if (item.confirm) {
    confirm_local_   = item.local;
    confirm_aid_     = target_aid_;
    confirm_name_    = target_name_;
    // 🔴 La saisie repart de zéro à chaque ouverture. Gardée d'une fois sur
    // l'autre, un « +50 » resterait armé et partirait sur le joueur suivant
    // d'un simple « Appliquer » — un don qu'on n'a pas voulu.
    confirm_points_  = 0;
    confirm_request_ = true;
    return;
  }

  // Rien n'est joué ICI : on est entre NewFrame() et Render(), et plusieurs de
  // ces actions ouvrent une modale native BLOQUANTE qui relance le tick du mode.
  pending_aid_   = target_aid_;
  pending_code_  = item.code;
  pending_local_ = item.local;
  // L'argument accompagne l'AID : le job pour la fiche de monstre et les liens,
  // le NAMEID pour un objet au sol — son « job » de quad est la sentinelle
  // 0x7D03, qui ne désigne rien.
  pending_arg_   = (kind_ == Kind::kGroundItem) ? target_item_id_ : target_job_;
}

// ── Confirmation ─────────────────────────────────────────────────────────────
// Une seule modale ImGui pour toutes les questions : seul le titre VISIBLE change
// (cf. `ConfirmModalId`), et le `switch` sur `confirm_local_` décide de ce qui
// s'y dessine. Trois questions à ce jour — décharger un NPC, bannir un compte,
// et la saisie d'un don de points d'event, qui n'est pas une confirmation mais
// emprunte le même mécanisme parce qu'elle pose la même sorte de pause.
const char* EntityContextMenu::ConfirmModalId(Local which) {
  switch (which) {
    case Local::kPlayerBlock:
      return i18n::Tr("Bannir ce compte###bourgeon_ctxmenu_confirm");
    case Local::kPlayerEventPoints:
      return i18n::Tr("Points d'event###bourgeon_ctxmenu_confirm");
    default:
      return i18n::Tr("Décharger ce NPC###bourgeon_ctxmenu_confirm");
  }
}

void EntityContextMenu::DrawConfirmModal() {
  if (confirm_request_) {
    confirm_request_ = false;
    ImGui::OpenPopup(ConfirmModalId(confirm_local_));
  }
  if (!ro::BeginRoPopupModal(ConfirmModalId(confirm_local_))) return;

  switch (confirm_local_) {
    case Local::kNpcUnload: {
      const ImVec4& red = kDangerColor;
      const ImVec4 gray(0.35f, 0.35f, 0.35f, 1.0f);
      char who[96];
      FormatNpcLabel(confirm_aid_,
                     confirm_name_.empty() ? nullptr : confirm_name_.c_str(),
                     who, sizeof(who));
      ImGui::TextColored(red, i18n::Tr("Décharger %s ?"), who);
      ImGui::Spacing();
      // Ce que ça coûte VRAIMENT, dit avant le clic : le serveur, lui, ne pose
      // aucune question et n'en reposera pas.
      // ⚠ Sauts de ligne EXPLICITES, pas `TextWrapped` : la modale est en
      // AlwaysAutoResize, et un texte qui se replie sur une largeur qu'il fixe
      // lui-même fait osciller la fenêtre d'une frame à l'autre.
      ImGui::Text("%s",
          i18n::Tr("Il disparaît immédiatement pour TOUS les joueurs\n"
                   "connectés, avec ses duplicates. Une conversation en\n"
                   "cours avec lui reste ouverte sur un NPC qui n'existe plus."));
      ImGui::Spacing();
      ImGui::TextColored(gray, "%s",
          i18n::Tr("Pour le faire revenir : clic droit sur un autre NPC du même\n"
                   "fichier puis « Recharger son fichier de script », ou\n"
                   "@reloadnpcfile. Rien n'est perdu sur le disque."));
      ImGui::Spacing();
      if (ro::RoButton(i18n::Tr("Décharger"), 110.0f, 0.0f)) {
        pending_aid_   = confirm_aid_;
        pending_code_  = 0;
        pending_local_ = Local::kNpcUnload;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
      break;
    }
    // ── Bannir un compte ────────────────────────────────────────────────────
    // La SEULE action du sous-menu staff qui ne se défait pas depuis le jeu :
    // le mute expire, la prison s'ouvre, le kick se relogue. Celle-ci retire un
    // compte entier, et le serveur ne posera aucune question de son côté.
    case Local::kPlayerBlock: {
      const ImVec4& red = kDangerColor;
      const ImVec4 gray(0.35f, 0.35f, 0.35f, 1.0f);
      const char* who = confirm_name_.empty() ? i18n::Tr("(nom inconnu)")
                                              : confirm_name_.c_str();
      ImGui::TextColored(red, i18n::Tr("Bannir le compte de %s ?"), who);
      ImGui::Spacing();
      // ⚠ Sauts de ligne EXPLICITES, pas `TextWrapped` : la modale est en
      // AlwaysAutoResize, et un texte qui se replie sur une largeur qu'il fixe
      // lui-même fait osciller la fenêtre d'une frame à l'autre.
      ImGui::Text("%s",
          i18n::Tr("C'est le COMPTE qui tombe, pas ce personnage : tous\n"
                   "les autres personnages du même compte deviennent\n"
                   "inaccessibles, et le bannissement n'a pas de fin."));
      ImGui::Spacing();
      ImGui::TextColored(gray, "%s",
          i18n::Tr("Pour le lever : @unblock <nom du personnage>.\n"
                   "Pour une sanction qui expire, préférez @ban."));
      ImGui::Spacing();
      if (ro::RoButton(i18n::Tr("Bannir"), 110.0f, 0.0f)) {
        pending_aid_   = confirm_aid_;
        pending_code_  = 0;
        pending_local_ = Local::kPlayerBlock;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
      break;
    }
    // ── Points d'event ──────────────────────────────────────────────────────
    // Pas une confirmation mais une SAISIE : elle emprunte le même mécanisme
    // parce qu'elle pose la même sorte de pause — le menu se ferme, la question
    // reste. Le solde actuel n'est volontairement pas affiché : le client ne le
    // connaît pas, et l'aller-retour qu'il faudrait pour l'obtenir ferait
    // attendre devant une modale vide. Il se lit dans « Propriétés… ».
    case Local::kPlayerEventPoints: {
      const ImVec4 gray(0.35f, 0.35f, 0.35f, 1.0f);
      const char* who = confirm_name_.empty() ? i18n::Tr("(nom inconnu)")
                                              : confirm_name_.c_str();
      ImGui::Text(i18n::Tr("Points d'event de %s"), who);
      ImGui::Spacing();
      ro::RoSliderInt(i18n::Tr("à donner###ctxmenu_points"), &confirm_points_,
                      -kEventPointsStep, kEventPointsStep, "%+d");
      ImGui::Spacing();
      ImGui::TextColored(gray, "%s",
          i18n::Tr("Négatif pour retirer. Ctrl+clic sur la barre pour taper\n"
                   "la valeur. Le serveur reborne, et refuse de faire\n"
                   "descendre le solde sous zéro.\n"
                   "Le solde actuel se lit dans « Propriétés… »."));
      ImGui::Spacing();
      // Zéro ne fait rien : le bouton reste visible et grisé plutôt que de
      // partir en paquet que le serveur refusera avec « indiquez un montant ».
      const bool nothing = (confirm_points_ == 0);
      if (nothing) ImGui::BeginDisabled();
      if (ro::RoButton(i18n::Tr("Appliquer"), 110.0f, 0.0f)) {
        pending_aid_   = confirm_aid_;
        pending_code_  = 0;
        pending_local_ = Local::kPlayerEventPoints;
        pending_param_ = confirm_points_;
        ImGui::CloseCurrentPopup();
      }
      if (nothing) ImGui::EndDisabled();
      ImGui::SameLine();
      if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) ImGui::CloseCurrentPopup();
      break;
    }
    default:
      // Sécurité : une confirmation sans question n'a rien à faire à l'écran.
      ImGui::CloseCurrentPopup();
      break;
  }
  ro::EndRoPopupModal();
}

void EntityContextMenu::FlushPending() {
  const int   code  = pending_code_;
  const Local local = pending_local_;
  if (code == 0 && local == Local::kNone) return;
  pending_code_  = 0;
  pending_local_ = Local::kNone;

  const uint32_t aid = pending_aid_;
  const uint32_t arg = pending_arg_;
  // Consommé comme le reste : une valeur qui survivrait à son action repartirait
  // avec la suivante.
  const int32_t param = pending_param_;
  pending_param_ = 0;

  switch (local) {
    case Local::kCopyName:
      ImGui::SetClipboardText(target_name_.empty() ? "" : target_name_.c_str());
      return;
    case Local::kCopyId: {
      char buffer[24];
      snprintf(buffer, sizeof(buffer), "%u", aid);
      ImGui::SetClipboardText(buffer);
      return;
    }
    case Local::kCopyPickInfo: {
      char buffer[192];
      snprintf(buffer, sizeof(buffer),
               i18n::Tr("%s | AID %u (0x%08X) | job %u"),
               target_name_.empty() ? i18n::Tr("(nom inconnu)") : target_name_.c_str(),
               aid, aid, arg);
      ImGui::SetClipboardText(buffer);
      return;
    }
    case Local::kInspect:
      // ⚠ `target_cat_` et `kind_` ne sont pas recopiés dans les champs
      // `pending_*` : ils ne changent qu'à la PROCHAINE ouverture de menu, qui
      // écraserait de toute façon l'action en attente. Même raisonnement que
      // `kCopyPickInfo` juste au-dessus.
      if (auto* insp = Bourgeon::Instance().entity_inspector())
        insp->Open(aid, arg, target_cat_, KindLabel(kind_));
      return;
    case Local::kHideTargetFrame:
      if (auto* tf = Bourgeon::Instance().target_frame()) tf->HideForGid(aid);
      return;
    case Local::kMonsterInfo:
      // `by_view` : le quad porte une classe de SPRITE, pas un id de mob_db —
      // exactement le cas du skill Sense, que la fiche sait déjà résoudre.
      if (auto* mi = Bourgeon::Instance().monster_info()) mi->Open(arg, true);
      return;
    case Local::kChatLinkMob:
      // ⚠ L'id posté est la classe de SPRITE (`arg`), la seule identité que
      // porte un acteur à l'écran — le client n'a pas mob_db. Les deux
      // coïncident pour l'écrasante majorité des monstres ; pour ceux qui
      // empruntent l'apparence d'un autre, le lien désignera le monstre DE
      // L'APPARENCE. Le corriger imposerait un aller-retour serveur avant de
      // pouvoir poser un lien, pour un cas marginal.
      //
      // Rang 0 : un acteur du monde ne dit pas s'il est MVP (le drapeau est
      // dans mob_db, côté serveur). Le badge apparaîtra chez le lecteur s'il
      // ouvre la fiche, qui, elle, le sait.
      if (auto* chat = Bourgeon::Instance().chat_window())
        chat->AppendMobLink(arg, 0, target_name_.c_str());
      return;
    case Local::kChatLinkNpc:
      // Une RECHERCHE, pas un lieu : les coordonnées de CET exemplaire ne
      // valent que pour lui, et beaucoup de PNJ sont posés à plusieurs endroits
      // sous le même nom. C'est aussi la seule forme qui survive à un
      // redémarrage du map-server, où les GID changent.
      // 🔴 AVEC LA CARTE. Un nom de PNJ n'est pas une identité, c'est un RÔLE :
      // le serveur pose trente-huit « Warp Agent », et un lien qui ne porte que
      // le nom les désigne tous à la fois. Le lecteur recevait donc une liste
      // où rien ne distinguait celui dont on lui parlait.
      //
      // La carte COURANTE est la bonne référence : on partage ce qu'on a sous
      // les yeux. Illisible (écran de chargement, hors du monde), le lien part
      // sans contexte plutôt que de mentir sur le lieu.
      if (auto* chat = Bourgeon::Instance().chat_window()) {
        char map[64];
        const bool has_map = rag::CurrentMapName(map, sizeof(map));
        chat->AppendNaviSearchLink(static_cast<uint8_t>(links::NaviKind::kNpc),
                                   target_name_.c_str(),
                                   has_map ? map : nullptr);
      }
      return;
    case Local::kTalkToNpc: {
      uint8_t packet[7] = {};
      packet[0] = static_cast<uint8_t>(kCzContactNpc & 0xff);
      packet[1] = static_cast<uint8_t>(kCzContactNpc >> 8);
      memcpy(packet + 2, &aid, 4);
      packet[6] = 0;
      Bourgeon::Instance().SendPacket(packet, sizeof(packet));
      return;
    }
    case Local::kAttack:
      // 🔴 Ordre EXPLICITE : il doit frapper même si « le clic cible sans
      // attaquer » est coché. Sans cette dispense, l'entrée de menu emprunterait
      // le même chemin natif que le clic et serait annulée avec lui — ce qui
      // s'est vu : « Attaquer » ne faisait plus rien.
      if (auto* tf = Bourgeon::Instance().target_frame())
        tf->NoteExplicitAttack(aid);
      if (!RunNativeActorClick(aid))
        LogDiag("[EntityContextMenu] clic acteur refuse (cible {})", aid);
      return;
    // ── Outillage NPC de l'administrateur (CZ 0x0F25) ─────────────────────────
    // Aucun compte rendu ici : c'est le SERVEUR qui dit dans le chat ce qu'il a
    // fait — y compris le refus, s'il juge que le compte n'y a pas droit. Lui
    // seul sait si le script s'est rechargé.
    case Local::kNpcReloadFile:
      SendNpcAdmin(aid, kNpcAdminReloadFile);
      return;
    case Local::kNpcUnload:
      SendNpcAdmin(aid, kNpcAdminUnload);
      return;
    case Local::kNpcMoveHere:
      SendNpcAdmin(aid, kNpcAdminMoveToMe);
      return;
    // ── Outillage JOUEUR du staff (CZ 0x0F2B) ─────────────────────────────────
    // Aucun compte rendu ici non plus : le serveur rejoue l'atcommand
    // correspondante et SON verdict — agi, ou refusé faute de permission de
    // groupe — revient dans le chat. Le dire nous-mêmes, c'est promettre une
    // action avant de savoir si elle a eu lieu.
    case Local::kPlayerComeHere:
      SendPlayerAdmin(aid, kPlayerAdminComeHere);
      return;
    case Local::kPlayerSitStand:
      SendPlayerAdmin(aid, kPlayerAdminSitStand);
      return;
    case Local::kPlayerEventPoints:
      SendPlayerAdmin(aid, kPlayerAdminEventPoints, param);
      return;
    case Local::kPlayerMute:
      SendPlayerAdmin(aid, kPlayerAdminMute);
      return;
    case Local::kPlayerUnmute:
      SendPlayerAdmin(aid, kPlayerAdminUnmute);
      return;
    case Local::kPlayerJail:
      SendPlayerAdmin(aid, kPlayerAdminJailToggle);
      return;
    case Local::kPlayerNuke:
      SendPlayerAdmin(aid, kPlayerAdminNuke);
      return;
    case Local::kPlayerBlock:
      SendPlayerAdmin(aid, kPlayerAdminBlock);
      return;
    // ── Objet au sol ──────────────────────────────────────────────────────────
    case Local::kPickupItem:
      if (!RunNativePickupItem(aid))
        LogDiag("[EntityContextMenu] ramassage refuse (objet {})", aid);
      return;
    case Local::kItemDesc: {
      // Le même chemin qu'un lien de chat : ARMÉ ici, joué par
      // itemcell::FlushDeferredDesc — qui tourne APRÈS nous dans le même
      // OnProcessInput (bourgeon.cc), donc au relâchement du clic, comme les
      // viewers. L'id de BASE seulement : un objet au sol n'a ni refine ni
      // cartes à montrer — le serveur ne les transmet pas dans le spawn.
      itemcell::ChatLink link;
      link.id = arg;
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      itemcell::DeferDescFromChatLink(link, static_cast<int>(mouse.x),
                                      static_cast<int>(mouse.y));
      return;
    }
    case Local::kChatLinkItem: {
      // La balise est POSÉE dans la barre, jamais envoyée : le joueur relit et
      // valide — même règle que « Linker ce monstre ».
      itemcell::ChatLink link;
      link.id = arg;
      if (auto* chat = Bourgeon::Instance().chat_window())
        chat->AppendItemLinkFromLink(link);
      return;
    }
    case Local::kCmdItemInfo:
    case Local::kCmdWhoDrops: {
      // Par le pipeline COMPLET du client (mêmes règles qu'une ligne tapée) :
      // la réponse du serveur revient dans le chat.
      char cmd[48];
      std::snprintf(cmd, sizeof(cmd), "%s %u",
                    local == Local::kCmdItemInfo ? "@iteminfo" : "@whodrops",
                    arg);
      if (auto* chat = Bourgeon::Instance().chat_window())
        chat->QueueCommand(cmd);
      return;
    }
    // ── Les deux chuchotements ────────────────────────────────────────────────
    // 🔴 Pas de `return` quand le chat moderne refuse : on TOMBE sur le code natif
    // 20, que ces deux entrées portent justement pour ça. Sans lui, « Chuchoter »
    // ne ferait plus rien du tout chez un joueur resté sur la chatbox du client.
    //
    // ⚠ `target_name_` est de l'UTF-8 (EntityName le convertit) et le chat, comme
    // tout ce qui touche au client, parle la code-page du FIL : un pseudo accentué
    // ne trouverait personne sans cette conversion.
    case Local::kWhisperBar:
      if (auto* chat = Bourgeon::Instance().chat_window())
        if (!target_name_.empty() &&
            chat->TargetWhisper(ro::Utf8ToWire(target_name_.c_str())))
          return;
      break;
    case Local::kWhisperWindow:
      if (auto* chat = Bourgeon::Instance().chat_window())
        if (!target_name_.empty() &&
            chat->OpenWhisperWindowByAid(ro::Utf8ToWire(target_name_.c_str()), aid))
          return;
      break;
    case Local::kAlootToggle:
      // Une CASE, jouée dans DrawPopup — jamais empilée. `return` et non
      // `break` : la sortie du switch rejoue un code NATIF, et celui d'une
      // case vaut 0.
      return;
    case Local::kNone:
      break;
  }

  // 🔴 La réponse du serveur (`ZC_EQUIPWIN_MICROSCOPE`) ne porte QUE le nom du
  // joueur — jamais son AID. La fenêtre d'inspection ne saurait donc à qui
  // redemander sa fiche, et deux homonymes seraient indiscernables. Nous, ici,
  // le savons : on le lui dit avant de rejouer le code natif.
  //
  // ⚠ C'est une demande, pas une cible : trois des quatre refus du serveur sont
  // SILENCIEUX (cible partie, autre map). L'AID n'est promu qu'à la réception.
  if (code == kCodeViewEquip) {
    if (auto* ve = Bourgeon::Instance().view_equip_window())
      ve->NotePendingTarget(aid);
  }

  if (!RunNativeMenuCode(code, aid))
    LogDiag("[EntityContextMenu] action {} injouable (cible {})", code, aid);
}

// ── Réglages ─────────────────────────────────────────────────────────────────

bool EntityContextMenu::DrawSettings() {
  // (Aucune case « Menu contextuel ImGui » ici : `imgui_enabled_` appartient au
  // groupe « Interface moderne », dont l'interrupteur unique vit en tête du
  // panneau. Une case locale rouvrirait l'état mixte que ce groupe existe pour
  // interdire — c'est la règle suivie par toutes les fenêtres du groupe.)
  bool changed = false;
  changed |= ro::RoCheckbox(i18n::Tr("Sur toutes les entités###ctxmenu_all"), &all_entities_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Le client n'ouvrait de menu que sur un joueur, son pet, son homoncule ou "
      "son mercenaire. Coché, le menu s'ouvre aussi sur les monstres, les NPC "
      "et les objets au sol (ramasser, description, ramassage automatique)."));

  // Sous-réglage : sans « toutes les entités », le menu ne s'ouvre de toute
  // façon jamais sur soi. Grisée plutôt que masquée — une case qui disparaît ne
  // dit pas ce qu'elle attendait ; l'infobulle, elle, reste lisible (elle est
  // posée HORS du grisage, sinon ImGui ne la montrerait plus).
  ImGui::Indent();
  ImGui::BeginDisabled(!all_entities_);
  changed |= ro::RoCheckbox(i18n::Tr("Sur soi-même###ctxmenu_self"), &self_menu_);
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Le menu sur son propre personnage ne porte que « Copier mon nom » (plus "
      "les identifiants sous « Outils du staff »). Décoché, le clic droit sur "
      "soi-même repart au client sans rien ouvrir ni rien avaler.\n"
      "Sans « Sur toutes les entités », ce menu ne s'ouvre de toute façon "
      "jamais : le client n'en a jamais eu sur soi."));
  ImGui::Unindent();

  // ── Blocage du clic sur les NPC de service ─────────────────────────────────
  // La case elle-même se coche dans le menu, sur le NPC. Ce qui vit ICI, c'est
  // l'interrupteur général et la LISTE — sans quoi un joueur qui éteint
  // l'interface moderne n'aurait plus aucun moyen de débloquer un NPC : le menu,
  // et donc la case, disparaissent avec elle.
  changed |= ro::RoCheckbox(
      i18n::Tr("Blocage du clic sur les NPC de service###ctxmenu_npcblock_opt"),
      &npc_block_enabled_);
  ImGui::SameLine();
  HelpMarker(i18n::Tr(
      "Les NPC de la capitale (kafra, job master, styliste, warpers…) se "
      "tiennent au milieu du passage et s'ouvrent quand on cherchait à "
      "marcher. Clic droit sur l'un d'eux, puis « Bloquer le clic gauche » : "
      "il devient une zone morte, le clic ne déclenche plus rien.\n"
      "Décoché ici, la case n'est plus proposée et les NPC déjà bloqués "
      "redeviennent cliquables — la liste, elle, est conservée."));

  ImGui::Indent();
  if (blocked_npcs_.empty()) {
    ImGui::TextDisabled("%s", i18n::Tr("Aucun NPC bloqué."));
  } else {
    // Une ligne par NPC, avec de quoi le débloquer sans avoir à le retrouver en
    // jeu (il peut être sur une autre carte).
    uint32_t to_unblock = 0;
    for (const auto& entry : blocked_npcs_) {
      ImGui::PushID(static_cast<int>(entry.first));
      if (ro::RoSmallButton(i18n::Tr("Débloquer"))) to_unblock = entry.first;
      ImGui::SameLine();
      if (entry.second.empty()) {
        ImGui::Text(i18n::Tr("NPC %u"), entry.first);
      } else {
        ImGui::Text("%s", entry.second.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%u)", entry.first);
      }
      ImGui::PopID();
    }
    if (to_unblock != 0) {
      blocked_npcs_.erase(to_unblock);
      // Le rappel de session suit le blocage : rebloqué plus tard, ce NPC doit
      // pouvoir réexpliquer son silence.
      warned_this_session_.erase(to_unblock);
      changed = true;
    }
  }
  ImGui::Unindent();

  if (IsStaff()) {
    changed |= ro::RoCheckbox(i18n::Tr("Outils du staff###ctxmenu_staff"), &staff_extras_);
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("Ajoute au menu les actions et les identifiants réservés au staff, dont "
        "« Propriétés… » — tout ce que le client sait d'une entité.\n"
        "Coché, le menu s'ouvre alors sur TOUTES les entités sans attendre le "
        "réglage du dessus : monstres, NPC, unités de compétence, objets au "
        "sol. C'est justement sur les cibles inattendues qu'un inspecteur "
        "sert.\n"
        "Certaines entrées exigent en plus que l'AID du compte figure dans le "
        "clientinfo.xml du client : elles n'apparaissent que dans ce cas."));
  }
  return changed;
}
