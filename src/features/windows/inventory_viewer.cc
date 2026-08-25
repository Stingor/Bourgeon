#include "features/item_cell.h"
#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "ragnarok/item_info.h"  // rag::itemlist : le layout du noeud
#include "features/windows/inventory_viewer.h"
#include "ui/game_texture.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h). Le chargement, le colorkey
// magenta et l'invalidation au reset de device y sont partagés — ce fichier en
// gardait sa propre copie, comme cinq autres plugins.
#include "ui/icon_cache.h"
#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cmath>    // std::fabs (recalage de la taille verrouillée sur les tuiles)
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "features/windows/bank_window.h"   // ToggleFromUi (bouton banque du footer)
#include "features/windows/chat_window.h"   // AppendItemLink (Maj+clic = lien d'objet)
#include "features/windows/trade_window.h"  // « Vers l'échange » (AddItemToTrade / active)
#include "features/windows/rodex_window.h"  // « Joindre au courrier » (AttachItem / composing)
#include "features/systems/bourgeon_opcodes.h"  // bopcodes::kReqCompatCards / kCompatCards (sertissage rapide)
#include "features/windows/item_desc_window.h"  // itemdesc::RenderSimpleDesc (aperçu au survol)
#include "features/moonlight_ui/moonlight_ui.h"  // API alootid (IsAlootId/AddAlootId/RemoveAlootId) + DrawSortModeCombo
#include "features/windows/storage_window.h"  // PointOverViewer (dépôt par glisser vers le viewer storage)
#include "features/windows/cart_viewer.h"     // PointOverViewer (dépôt par glisser vers le viewer cart)
#include "features/windows/vending_window.h"  // IsComposing (échoppe en cours -> transferts figés)
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB
#include "imgui.h"
#include "ui/qty_prompt.h"   // ro::QuantityPrompt (dialogue « combien ? » partagé)
#include "ui/ro_imgui.h"     // skin RO (BeginRoWindow / RoButton / RoCheckbox / DrawBar)
#include "utils/i18n.h"
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "features/windows/viewer_probes.h"  // etat des fenetres voisines
#include "ui/item_grid_chrome.h"  // ro::grid : le decor commun aux grilles
#include "ui/ro_widgets.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000) ─────────────────────────────
// Voir project_inventory_viewer_wip + project_inventory_window + workflow slim RE.
namespace {

constexpr uintptr_t kCntEquipped = 0x00d9aa70;  // __fastcall(session) : nb items ÉQUIPÉS distincts (10 slots @+0x17d4)
constexpr uintptr_t kCntCostume  = 0x00d9a960;  // __fastcall(session) : nb items COSTUME distincts (10 slots @+0x2b34)
using rag::itemlist::kInfoIndex;
using rag::itemlist::kInfoIdStr;
constexpr int kInvBase = 200;  // moonlight INVENTORY_BASE_SIZE ; max = expansion + 200

// Nom de base + nom complet (refine/cartes/enchant), comme le storage.
using GetBaseName_t = size_t(__thiscall*)(void*, char*, size_t*, char);

// Construit le nom d'affichage d'un item sous SEH ISOLÉ. Un item dont BuildDisplayName
// plante avortait TOUT l'Extract (d'où des items manquants vs le natif) ; ici le
// plantage est confiné -> l'énumération continue (repli GetBaseName / nom vide).

// Icône d'item : BuildItemIconGrfPath(id_str, out[128], identified) __stdcall (RET 0xc).
using FmtComma_t = char*(__cdecl*)(int, char*, int);  // FUN_00a948d0 séparateur milliers

// Fenêtre de description (id 0xc) : MakeWindow + OnMsg(0x18, &ItemSkillInfo).
constexpr uintptr_t kToggleWndById = 0x00812e60;  // FUN_00812e60(id) __stdcall (RET 0x4, vérifié désasm) : bascule fenêtre (ferme si ouverte via SaveWindowRect, sinon ouvre) = chemin de l'icône de menu
// Placement et taille par défaut du viewer, à la toute 1re ouverture seulement
// (avant, ils étaient lus sur la fenêtre native, qui ne naît plus).
constexpr float kSpawnX = 700.0f, kSpawnY = 400.0f;
constexpr float kSpawnW = 300.0f, kSpawnH = 360.0f;
using ToggleById_t   = int (__stdcall*)(int);  // FUN_00812e60(id) : ferme la fenêtre si ouverte

// Dispatcher (CMode) : FUN_00a75340(0x1213338) renvoie l'objet mode actif (ou 0 hors
// jeu — c'est *(0x0121333c) gardé). Son vtbl+0x18 = CMode::SendMsg (le gros switch).
// Commandes (confirmées via le double-clic natif 0x00949fc0 et le clic-droit 0x0094f380) :
//   use conso 0x1b / équiper 0x13 / carte 0x7b / munition-costume-ombre 0x57 ;
//   transfert vers cart 0x4c / vers storage Kafra 0x37 (0x33 = guilde, fenêtre 0x271b).
constexpr int kCmdUse       = 0x1b;
constexpr int kCmdEquip     = 0x13;
constexpr int kCmdCard      = 0x7b;
constexpr int kCmdAmmo      = 0x57;
constexpr int kCmdEquipAlt  = 0x12e;  // Ctrl+double-clic : équipe en MAIN GAUCHE (dual-wield).
constexpr uintptr_t kLeftHandEquipOpt = 0x01602278;  // DAT_01602278 : option client "équip main gauche" active ?
constexpr int kCmdToCart    = 0x4c;
constexpr int kCmdCartToBody = 0x4d;  // cart -> inventaire (retrait) ; RE UIInventoryWnd_OnMsg case 0x26 (contexte cart).
constexpr int kCmdToStorage = 0x37;  // storage KAFRA.
                                     // 0x33 = guilde (fenêtre 0x271b), 0x4c = cart.
                                     // RE UIInventoryWnd_OnRButtonDown : le natif choisit selon
                                     // la fenêtre ouverte ; nous, via viewers::StorageOpen() — qui
                                     // interroge StorageWindow, la fenêtre native n'existant
                                     // plus en mode ImGui.

// Opcodes client->serveur (bloc PACKETVER 20250716 ACTIF ; RE workflow confirmée).
//   Drop     : CZ_ITEM_THROW 0x0438  [op:2][index:2][amount:2] (amount 16-bit)
//   Favorite : CZ_INVENTORY_TAB 0x0907 [op:2][index:2][fav:1]
//     fav=0 => AJOUTE aux favoris ; fav=1 => RETIRE (seulement si déjà favori).
//     Donc bascule : envoyer (déjà favori ? 1 : 0). index = index CLIENT (info+4).
constexpr uint16_t kOpDrop     = 0x0363;  // CZ_ITEM_THROW. ATTENTION SHUFFLE : 0x0438 reecrit -> UseSkillToId (len 10) = disconnect ; drop shuffle = 0x0363 (clif_shuffle.hpp bloc > 20180307). Format [op:2][index:2][amount:2] inchange.
constexpr uint16_t kOpFavorite = 0x0907;
// (Pas de CZ_REQ_TAKEOFF_EQUIP ici : le déséquipement part de la FICHE DE
// PERSONNAGE, y compris quand le geste consiste à lâcher une pièce portée sur
// l'inventaire — c'est elle qui possède le doll et donc l'index à retirer.)

// Fenêtres cible d'un transfert (cart / storage), pour le drag-out + menu.
// Le cart se cherche par ID au gestionnaire (cf. CartWnd plus bas) ; le storage
// garde son global dédié, lui bien référencé par le client.

// ── Sertissage de cartes : ex-popup natif UIItemCompositionWnd (id 0x4A) ─────
// RE complète : docs/card_insert_re.md.
//
// 🔴 Le popup natif ne naît PLUS. On prend la place du handler de ZC 0x017B, qui
// était son SEUL créateur (`MakeWindow(0x4A)` puis remplissage par OnMsg), et on
// tient la liste des candidats nous-mêmes. Ce que faisait ce handler, et qui doit
// donc être repris ici — ou constaté sans objet :
//   - il ÉCRIT : uniquement dans la fenêtre 0x4A (vidage 0x4B, index de carte
//     0x4D, une ligne par candidat 0x1F). Elle n'existe plus : rien à reprendre.
//   - il EMPÊCHE : rien.
//   - le SERVEUR suppose : rien non plus. Il attend un CZ 0x017C, ou rien.
// L'index de la carte source, lui, N'EST PAS écrit par ce handler : c'est le
// sélecteur 0x7B de CMode::SendMsg qui le pose en CMode+0x45c au moment où on
// double-clique la carte (cf. §2.1 du doc). Il survit donc intact, et reste la
// source de vérité — on le relit plutôt que d'en tenir une copie.
constexpr uint16_t kOpCompList = 0x017B;  // ZC_ITEMCOMPOSITION_LIST (variable)
constexpr int kOffModeCardIndex = 0x45c;  // CMode+0x45c : index inv. de la carte

// Où la fenêtre apparaît la première fois (le natif donnait sa propre position ;
// il n'y a plus de natif à interroger). Ensuite ImGui la garde où on la pose.
constexpr float kCiSpawnX = 620.0f, kCiSpawnY = 200.0f;

// Sélecteur CMode::SendMsg du sertissage : son bloc (0x00c8f59d) construit
// CZ_REQ_ITEMCOMPOSITION — [op:2][cardIndex:2][equipIndex:2]. On passe par LUI plutôt
// que de fabriquer le paquet, pour que le format ne puisse pas diverger. (Les helpers
// SertirTimes/CancelComposition sont définis plus bas : ils ont besoin de SendCmd.)
constexpr int kCmdComposition = 0x7c;

// Index d'inventaire de la carte en cours de sertissage, lu dans CMode+0x45c —
// c'est-à-dire là où le natif l'a posé lui-même en émettant CZ 0x017A. 0 = aucun.
// (Défini plus bas, il a besoin de rag::ActiveModeSafe().)
int ReadModeCardIndex();

// Fiche POD d'un candidat, extraite sous SEH pour un rendu hors __try.
// (Le lecteur ReadCompItem vit plus bas : il a besoin de FindInfoByIndex.)
struct CompItem {
  int      index = 0;        // index inventaire (argument du paquet)
  uint32_t id = 0;           // nameid (icône)
  int      refine = 0;
  uint8_t  identified = 0;
  int      used_slots = 0;   // nb de cartes DÉJÀ serties
  int      total_slots = 0;  // nb d'emplacements de l'item (borne le « sertir ×N »)
  bool     forged = false;   // item forgé/créé : +0x1c n'est PAS une liste de cartes
  char     name[64] = {0};
  // Données d'INSTANCE (cartes déjà serties + random options) pour l'aperçu au survol,
  // mêmes offsets que la grille d'inventaire (info+0x1c / info+0x9c).
  uint32_t cards[4] = {0};
  int      opt_count = 0;
  struct Opt { int16_t index; int16_t value; uint8_t param; };
  Opt      opts[5] = {};
};

// Total d'un nameid présent dans l'inventaire (somme des quantités de tous ses stacks).
// Lu frais depuis le modèle session à chaque appel -> se met à jour tout seul après un
// sertissage (le handler natif de ZC 0x017D décrémente le stack de la carte). SEH, POD.
int CountCardStock(uint32_t id) {
  return itemcell::CountById(rag::kInventoryListAddr, id);
}

// ── Helpers vtable ──────────────────────────────────────────────────────────

// CMode+0x45c : le sélecteur 0x7B y écrit l'index d'inventaire de la carte juste
// avant d'émettre CZ 0x017A (`MOV [EDI+0x45c],EDX` @0x00c8f556), et le handler natif
// de 0x017B le relit tel quel (@0x00ca5b15) pour titrer son popup.
//
// C'est BIEN le même objet que rag::ActiveModeSafe() : nos demandes de liste passent par
// SendCmd, donc par `d->vf+0x18(d, 0x7B, index, …)` — l'`EDI` de l'écriture ci-dessus
// EST le `d` que nous venons de passer. On lit donc exactement ce qu'on a écrit, y
// compris pour la liste re-demandée après un lot de sertissages. D'où la lecture
// plutôt qu'une copie locale, qui pourrait diverger.
int ReadModeCardIndex() {
  void* mode = rag::ActiveModeSafe();
  if (!mode) return 0;
  __try {
    return *reinterpret_cast<int*>(static_cast<uint8_t*>(mode) + kOffModeCardIndex);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Items PORTÉS (équipés + costume) : la liste session 0x015fbab0 les EXCLUT, mais le
// compteur du footer NATIF les compte dans le total (Inventory_GetCount + FUN_00d9aa70
// équipés + FUN_00d9a960 costume). On appelle les deux (items distincts, 10 slots chacun)
// pour que notre « N/max » matche le natif. SEH (POD).
using WornCountFn_t = int(__fastcall*)(int);
int WornItemCount() {
  __try {
    return reinterpret_cast<WornCountFn_t>(kCntEquipped)(static_cast<int>(rag::kSessionAddr)) +
           reinterpret_cast<WornCountFn_t>(kCntCostume)(static_cast<int>(rag::kSessionAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Envoie une commande UI native (use/equip/transfer...) via le dispatcher.
// Défini plus bas (il dépend des lecteurs de fenêtres) ; déclaré ici pour le
// garde-fou de SendCmd.
void SendCmd(int cmd, int index, int arg2) {
  // Garde-fou pour les seuls TRANSFERTS (les raccourcis double-clic / Alt+clic
  // droit ne passent pas par un widget désactivé). Volontairement limité à ces
  // trois commandes : utiliser ou équiper reste permis pendant une composition.
  if ((cmd == kCmdToCart || cmd == kCmdCartToBody || cmd == kCmdToStorage) &&
      viewers::VendingComposing())
    return;
  __try {
    void* d = rag::ActiveModeSafe();
    if (d) rag::ModeSendMsg(d, cmd, index, arg2, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Utiliser ou équiper l'item selon son type (miroir du double-clic natif 0x00949fc0).
// arg2 = masque d'emplacement (info+8, `loc`) pour ÉQUIP/MUNITION — le natif le passe ;
// arg2=0 => le serveur reçoit position 0 => l'équip échoue (c'était LE bug). arg2=0 pour
// conso/carte (le natif le force à 0). left_hand (Ctrl+double-clic) = cmd 0x12e (main
// gauche dual-wield) si l'option client DAT_01602278 est active.
void UseOrEquip(int index, int type, uint32_t loc, bool left_hand) {
  int cmd = 0, arg2 = 0;
  switch (type) {
    case 0: case 1: case 2: case 0x12: cmd = kCmdUse; break;  // consommables (arg2=0)
    case 4: case 5: case 8: case 9:
    case 0xb: case 0xc: case 0xd: case 0xe: case 0xf:
      cmd = kCmdEquip; arg2 = static_cast<int>(loc); break;  // équipement
    case 6: cmd = kCmdCard; break;                           // carte (arg2=0)
    case 0xa: case 0x10: case 0x11: case 0x13:
      cmd = kCmdAmmo; arg2 = static_cast<int>(loc); break;   // munition/costume/ombre
    default: return;  // etc/divers : le double-clic ne fait rien
  }
  // Ctrl = main gauche (équip/munition), si l'option client est active (comme le natif).
  if (left_hand && (cmd == kCmdEquip || cmd == kCmdAmmo)) {
    bool opt = false;
    __try { opt = *reinterpret_cast<uint8_t*>(kLeftHandEquipOpt) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (opt) cmd = kCmdEquipAlt;
  }
  SendCmd(cmd, index, arg2);
}

// Valide le sertissage EXACTEMENT comme le bouton OK natif : le sélecteur 0x7C émet
// CZ_REQ_ITEMCOMPOSITION et referme le popup. Ordre des arguments repris du natif
// (SendMsg(0x7C, equipIdx, cardIdx)) — c'est le bloc 0x00c8f59d qui les remet dans
// l'ordre du paquet (cardIndex d'abord).
// Sertit `times` fois d'affilée le MÊME équipement, puis garde la fenêtre ouverte en
// re-demandant une liste fraîche (0x017A via kCmdCard), tant qu'il reste des cartes.
// `stockBefore` = nb de cartes en stock AVANT ce lot ; l'appelant garantit
// times <= min(slots libres, stockBefore), donc chaque paquet reste valide (un slot
// libre + une carte à consommer). Une fois le stock épuisé, l'index de la carte
// devient invalide -> rien à re-demander.
// Renvoie true si la session continue (une liste fraîche est en route), false si
// l'appelant doit refermer.
bool SertirTimes(int cardIndex, int equipIndex, int times, int stockBefore) {
  if (times < 1) times = 1;
  for (int i = 0; i < times; ++i)
    SendCmd(kCmdComposition, equipIndex, cardIndex);  // 0x017C ; ne ferme PAS
  if (stockBefore - times > 0) {
    SendCmd(kCmdCard, cardIndex, 0);  // 0x017A : le serveur renvoie une liste à jour
    return true;
  }
  return false;
}

// Annulation : même sélecteur avec (-1, -1) — le bloc natif n'émet alors aucun paquet
// (c'est le chemin du bouton « cancel »). On l'appelle quand même plutôt que de ne
// rien faire : c'est le geste exact du natif, et lui seul sait ce qu'il remet à zéro.
void CancelComposition() { SendCmd(kCmdComposition, -1, -1); }

// Type équipable (équipement OU munition/costume/ombre) : pour le drop sur la fenêtre
// Équipement — un consommable lâché dessus ne doit PAS être consommé.
bool IsEquippable(int type) {
  switch (type) {
    case 4: case 5: case 8: case 9:
    case 0xb: case 0xc: case 0xd: case 0xe: case 0xf:
    case 0xa: case 0x10: case 0x11: case 0x13:
      return true;
    default: return false;
  }
}

void SendDrop(int index, int amount) {
  if (amount <= 0) return;
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpDrop;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  *reinterpret_cast<uint16_t*>(pkt + 4) = static_cast<uint16_t>(amount);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// (Le décodage du glisser NATIF — ReadDraggedItem, CancelNativeDrag et les
// offsets de sa charge — a disparu avec les fenêtres qui pouvaient en émettre.)

// (Pas de déduction « échange en cours » ici : les objets mis en échange sont
// RÉELLEMENT retirés du modèle de session dès l'acquittement du serveur, comme le
// faisait le client officiel — le protocole l'exige, cf. trade_window.cc.)

// Bascule le favori d'un item : envoie son état ACTUEL (le serveur toggle).
void SendFavoriteToggle(int index, bool currently_fav) {
  uint8_t pkt[5];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpFavorite;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(index);
  pkt[4] = currently_fav ? 1 : 0;  // 1=retire (si déjà favori) / 0=ajoute
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// ── Verrous footer (RE workflow, 100% CLIENT-SIDE, aucun paquet) ────────────────
// Drop lock (cmd natif 0xd5, byte @0x015fffa0) : testé par le drag/drop natif AU SOL ;
// on le teste AUSSI dans notre "Jeter". Deal lock (cmd 0x1fb, byte @0x01600553) :
// FUN_00cd0f00 (liste de vente NPC) exclut les favoris quand ON.
constexpr uintptr_t kDropLockGlobal = 0x015fffa0;
inline bool ReadLock(uintptr_t g) {
  __try { return *reinterpret_cast<uint8_t*>(g) != 0; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline void ToggleLock(uintptr_t g) {
  __try { *reinterpret_cast<uint8_t*>(g) ^= 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Description (clic-droit) : l'ItemSkillInfo COMPLET du nœud à la fenêtre 0xc ──
// On retrouve le nœud par son INDEX inventaire dans la liste session, et on passe
// SON info — celle que le serveur a remplie, avec cartes/refine/enchants.
// item_desc_window détecte la fenêtre 0xc et rend sa version enrichie.
//
// ⚠ Par INDEX, jamais par id : l'id ne distingue pas deux exemplaires du même
// objet. Trois Knife (une avec carte, une +1, une +3) sont trois nœuds de même id
// et une recherche par id rendrait toujours le PREMIER — la description complète
// se figeait sur lui pour les trois cases. L'index, lui, est unique par slot.
//
// DIFFÉRÉE au relâchement du bouton (itemcell::FlushDeferredDesc) : ouverte dès
// le clic, un appui PROLONGÉ faisait passer la description DERRIÈRE nous.
void OpenItemDesc(int index, int mx, int my) {
  itemcell::DeferDescFromIndex(rag::kInventoryListAddr, index, mx, my);
}

// Le même nœud, mais retrouvé par son INDEX inventaire.
void* FindInfoByIndex(int index) {
  return itemcell::FindInfoByIndex(rag::kInventoryListAddr, index);
}

// ── Retrouver un équipement PORTÉ par son index d'inventaire ────────────────
// La liste session (0x015fbab0) EXCLUT les pièces portées, alors que le serveur les
// propose bien au sertissage (on sertit couramment l'arme qu'on a en main). Le natif
// s'en sortait en appelant Session_GetEquipInfoByInvIndex, qui remplit un
// ItemSkillInfo — un objet C++ avec une std::string, qu'on ne peut pas fabriquer ici
// sans risque. On va donc chercher la pièce là où le client la RANGE : le tableau
// equip de la session, dont chaque entrée est un ItemSkillInfo au même layout. On
// rend un pointeur VIVANT dessus, ce qui est mieux que l'instantané que la liste du
// popup natif nous donnait (il ne reflétait plus l'item après un sertissage).
constexpr uintptr_t kEquipArrayBase   = 0x17d0;  // équipement normal (session+)
constexpr uintptr_t kCostumeArrayBase = 0x2b30;  // costume
constexpr uintptr_t kEquipSlotStride  = rag::itemlist::kInfoSize;
constexpr int kEquipSlotCount = 10;   // slots 0..9 (cf. character_sheet.cc)
constexpr int kOffEquipPresent = 0x10;  // == 1 si le slot est occupé

void* FindWornInfoByIndex(int index) {
  if (index <= 0) return nullptr;
  __try {
    for (int pass = 0; pass < 2; ++pass) {
      const uintptr_t base = pass ? kCostumeArrayBase : kEquipArrayBase;
      for (int slot = 0; slot < kEquipSlotCount; ++slot) {
        uint8_t* e = reinterpret_cast<uint8_t*>(
            rag::kSessionAddr + base + static_cast<uintptr_t>(slot) * kEquipSlotStride);
        if (*reinterpret_cast<int*>(e + kOffEquipPresent) != 1) continue;
        if (*reinterpret_cast<int*>(e + kInfoIndex) == index) return e;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
  return nullptr;
}

// L'ItemSkillInfo VIVANT d'un index d'inventaire, porté ou non (nullptr si l'item
// n'existe plus — vendu, consommé, ou index périmé d'une liste que le serveur n'a
// pas rafraîchie).
void* FindLiveInfoByIndex(int index) {
  void* info = FindInfoByIndex(index);
  return info ? info : FindWornInfoByIndex(index);
}

// Remplit une fiche CompItem depuis un ItemSkillInfo. Cf. struct CompItem plus haut.
bool ReadCompItemFromInfo(uint8_t* info, CompItem* out) {
  if (!info) return false;
  __try {
    out->index = *reinterpret_cast<int*>(info + kInfoIndex);
    const char* ids = rag::clientstr::Data(info + kInfoIdStr);
    out->id = ids ? static_cast<uint32_t>(atoi(ids)) : 0;
    out->refine = *reinterpret_cast<int*>(info + rag::itemlist::kInfoRefine);
    out->identified = *reinterpret_cast<uint8_t*>(info + rag::itemlist::kInfoIdent);
    // Slots cartes : info+0x1c, 4 entrées. ⚠ Sur un item FORGÉ/CRÉÉ ces mêmes mots
    // portent les données du forgeron (charid scindé, star crumbs, élément) et non
    // des cartes — même critère que item_desc_window.cc:419 (id <= 500).
    const uint32_t c0 = *reinterpret_cast<uint32_t*>(info + 0x1c);
    out->forged = (c0 != 0 && c0 <= 500);
    if (!out->forged) {
      for (int k = 0; k < 4; ++k)
        out->cards[k] =
            *reinterpret_cast<uint32_t*>(info + rag::itemlist::kInfoCards + k * 4);
    }
    // Random options d'instance, pour l'aperçu de description au survol.
    int nopt = *reinterpret_cast<int*>(info + rag::itemlist::kInfoOptCount);
    if (nopt < 0) nopt = 0;
    if (nopt > 5) nopt = 5;
    out->opt_count = nopt;
    for (int k = 0; k < nopt; ++k) {
      const uint8_t* e = info + rag::itemlist::kInfoOpts + k * 5;
      out->opts[k].index = *reinterpret_cast<const int16_t*>(e);
      out->opts[k].value = *reinterpret_cast<const int16_t*>(e + 2);
      out->opts[k].param = e[4];
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  out->total_slots = itemcell::SlotCount(info);  // hors __try (appel natif, déjà SEH-gardé)
  // Emplacements réellement OCCUPÉS PAR UNE CARTE : seules les `total_slots` premières
  // entrées de info+0x1c comptent. Les enchantements (type carte, sous-type enchant)
  // sont écrits par le serveur dans les entrées HAUTES (card[3], puis card[2]…) même
  // sur un item à 2 emplacements ; les compter faisait croire l'item plein et grisait
  // « Sertir », alors que pc_insert_card ne cherche un emplacement libre que dans
  // [0, slots) — d'où le double-clic qui sertissait quand même.
  if (!out->forged) {
    for (int k = 0; k < out->total_slots && k < 4; ++k)
      if (out->cards[k] != 0) ++out->used_slots;
  }
  itemcell::BuildDisplayName(info, out->name, sizeof(out->name));
  if (out->name[0] == '\0') {
    __try {
      size_t cap = sizeof(out->name);
      reinterpret_cast<GetBaseName_t>(itemdb::kBaseNameFallbackAddr)(info, out->name, &cap, 0);
      out->name[sizeof(out->name) - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) { out->name[0] = '\0'; }
  }
  return true;
}

// Variante « par index d'inventaire », pour la CARTE source (elle, est bien dans
// l'inventaire puisqu'on vient de double-cliquer dessus).
bool ReadCompItemByIndex(int index, CompItem* out) {
  if (index <= 0) return false;
  return ReadCompItemFromInfo(static_cast<uint8_t*>(FindInfoByIndex(index)), out);
}

// Aperçu de description RO au survol (le MÊME que la grille d'inventaire) : tooltip
// couche-avant, fond blanc arrondi + cadre sysbox peint derrière via un split de
// canaux. `cards`/`opts` = données d'instance du stack survolé (la DB ne les connaît
// pas), `name` = nom déjà décoré par BuildDisplayName (préfixes/suffixes de cartes),
// pour que le titre soit celui de la description complète.
// No-op si id == 0. Appelé À L'EXTÉRIEUR de toute fenêtre (crée son popup).

// Maj + clic G : le LIEN de l'item part dans la barre de saisie du chat, que le
// joueur envoie ensuite avec Entrée — le geste de `UIInventoryWnd_OnLButtonDown
// 0x0094afb0` (branche `GetAsyncKeyState(VK_SHIFT)`).
//
// 🔴 Le chemin NATIF est mort avec la chatbox. Il regardait la fenêtre qui a le
// focus (`g_UIWindowMgr+0x1a0`, type à `wnd+0x2c` : 0x1ea/0x1ee = un edit de chat,
// 0x1ed = la chatbox, dont l'edit est à `+0xbc`) et appelait
// `UIChatWnd_InsertItemLink 0x008217f0`. Aucun de ces trois types n'existe plus
// depuis que la chatbox ImGui détruit la native : la fonction ne faisait donc
// plus RIEN, en silence. On adresse maintenant notre propre barre de saisie, qui
// tient le lien de côté et le résout à l'envoi (cf. ChatWindow::AppendItemLink).
//
// L'ancien chemin natif est conservé pour le joueur resté en chat NATIF (la
// chatbox ImGui est un réglage, pas une fatalité).
using ChatInsertLink_t = void(__thiscall*)(void*, void*);
void PostItemLinkToChat(int index) {
  // `FindLive…` et non `FindInfoByIndex` : la fiche de personnage relaie ici le
  // Maj+clic sur un slot ÉQUIPÉ, dont l'item n'est plus dans la liste inventaire.
  void* info = FindLiveInfoByIndex(index);
  if (!info) return;

  if (auto* chat = Bourgeon::Instance().chat_window()) {
    if (chat->AppendItemLink(info)) return;
    // Chatbox ImGui active mais refus (plafond de trois liens, barre masquée) :
    // s'en remettre au natif n'aurait aucun sens, il n'existe plus.
    if (chat->imgui_enabled_) return;
  }

  __try {
    void* focused = *reinterpret_cast<void**>(uiwnd::kUIWindowMgrAddr + 0x1a0);
    if (!focused) return;
    const int type = *reinterpret_cast<int*>(
        reinterpret_cast<uint8_t*>(focused) + uiwnd::kOffWndId);
    auto insert = reinterpret_cast<ChatInsertLink_t>(0x008217f0);
    if (type == 0x1ea || type == 0x1ee) {   // input chat (focus direct)
      insert(focused, info);
    } else if (type == 0x1ed) {              // input via fenêtre dédiée (DAT_0131f6b0+0xbc)
      void* base = *reinterpret_cast<void**>(uiwnd::kChatWndSlot);
      if (base) insert(*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + 0xbc), info);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Lit un pointeur de fenêtre valide depuis un slot (vtable vérifiée). SEH.
uint8_t* ReadValidWnd(uintptr_t slot, uintptr_t expected_vtable) {
  return uiwnd::WndAtSlot(slot, expected_vtable);
}

// Échange joueur-joueur ImGui actif (TradeWindow) : cible de « Vers l'échange ».
bool TradeOpen() {
  auto* tt = Bourgeon::Instance().trade_window();
  return tt && tt->active();
}
// Écriture d'un courrier ImGui en cours (RodexWindow) : cible de « Joindre au
// courrier ». Les pièces jointes n'existent QUE pendant une écriture.
bool MailComposing() {
  auto* rodex = Bourgeon::Instance().rodex_window();
  return rodex && rodex->composing();
}

// (Plus de MouseOverEquip / EquipWnd : la fenêtre Équipement native 0xa ne naît
// plus, la feuille de personnage la remplace. Y déposer un objet pour l'équiper
// reste possible — c'est la feuille elle-même qui accepte le payload « INV_ITEM »
// sur ses slots, et qui renvoie ici par EquipDraggedItem.)

// Lecture SEH (POD only) des globals du footer -> hors OnRenderUI, qui contient des
// objets C++ (vector/filter) et ne peut donc pas héberger de __try (C2712).
struct FooterVals { int wmax = 0, wcur = 0, zeny = 0, expansion = 0, overPct = 0; };
FooterVals ReadFooterVals() {
  FooterVals v;
  __try {
    v.wmax = *reinterpret_cast<int*>(rag::kWeightMaxAddr);
    v.wcur = *reinterpret_cast<int*>(rag::kWeightCurAddr);
    v.zeny = *reinterpret_cast<int*>(rag::kZenyAddr);
    v.expansion = *reinterpret_cast<int*>(rag::kInventoryExpansionAddr);
    v.overPct = *reinterpret_cast<int*>(rag::kOverweightPctAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return v;
}

// ── Onglets de catégorie (par type d'item, repris du filtre natif case 0x17) ──
// Use {0,1,2,0x12} / Equip {4,5,8,9,0xb-0xf} / Etc {3,7,10,0x10,0x11,0x13} /
// Cartes {6} / Favoris (flag). "Tout" = tout. (Les cartes ont leur onglet -> exclues d'Etc.)
// img = base name des .bmp d'onglet (basic_interface\<img>1.bmp actif / <img>2.bmp
// inactif), repris de inventory_tweaks (onglets images du natif). nullptr => texte.
struct Cat { const char* label; const int* types; int n; bool fav; const char* img; };
const int kUse[]   = {0, 1, 2, 0x12};
const int kEquip[] = {4, 5, 8, 9, 0xb, 0xc, 0xd, 0xe, 0xf};
// Munitions (IT_AMMO 10 + variantes) SORTIES d'Etc : elles ont leur propre onglet,
// sinon elles apparaîtraient dans les deux (ItemInCat s'arrête au 1er type qui
// matche, mais chaque onglet est testé indépendamment).
const int kEtc[]   = {3, 7};
const int kAmmo[]  = {10, 0x10, 0x11, 0x13};
const int kCard[]  = {6};
const Cat kCats[] = {
    {"Tout", nullptr, 0, false, "tab_all"},
    {"Conso", kUse, 4, false, "tab_use"},
    {"Equip", kEquip, 9, false, "tab_cos"},
    {"Ammo", kAmmo, 4, false, "tab_ammo"},
    {"Etc", kEtc, 2, false, "tab_etc"},
    {"Cartes", kCard, 1, false, "tab_card"},
    {"Favoris", nullptr, 0, true, "tab_fav"},
};
constexpr int kNumCats = 7;

// ── Assets natifs : barre 3-slice + fond de tuile + icônes du footer ───────────
// Barre custom btnbar_left3/mid3/right3.bmp + fond de tuile itemwin_mid.bmp (tuile
// native 32px) + icônes du footer natif icon_weight/icon_num.bmp. Chargés en textures
// ImGui. Préfixe CP949 pris sur les strings exe (basic_interface\ pour barre/tuile ;
// inventory\ pour les icônes, qui ont leur propre string).
// Le bouton « banque » du footer est de l'art AJOUTÉ (styleshop\btn_bank_*.bmp) :
// il n'a donc pas de string dans l'exe. On emprunte le préfixe CP949 à une string
// styleshop native plutôt que de le réécrire à la main.
constexpr uintptr_t kStyleshopPath  = 0x010265e8;  // "유저인터페이스\styleshop\btn_buy_out.bmp"

using BarTex = ro::GameTexture;  // (même forme ; le chargeur est partagé)
BarTex g_tile_lock;    // itemwin_mid_lock.bmp : fond quand deal-lock actif sur Favoris
BarTex g_tab[kNumCats][2];   // onglets VERTICAUX   [cat][0=actif(1.bmp), 1=inactif(2.bmp)]
BarTex g_tabh[kNumCats][2];  // onglets HORIZONTAUX : mêmes noms en tabh_* (jeu dédié)
BarTex g_btn_drop[2];  // item_drop_lock [0=off/déverrouillé, 1=on/verrouillé]
BarTex g_btn_deal[2];  // bt_itemDeal_lock [0=off, 1=on/verrouillé (anti-vente NPC)]
BarTex g_btn_sort[2];  // bt_sort [0=off (_off.bmp), 1=on (.bmp) = vue triée]
BarTex g_btn_bank[3];  // styleshop\btn_bank_* [0=out, 1=over, 2=down] — 19x24
bool   g_assets_tried = false;

void InventoryPath(const char* file, char* out, size_t out_sz) {
  ro::uipath::WithFileName(ro::uipath::kIconWeight, file, out, out_sz);
}
void StyleshopPath(const char* file, char* out, size_t out_sz) {
  ro::uipath::WithFileName(kStyleshopPath, file, out, out_sz);
}

void LoadFooterAssets() {
  if (g_assets_tried) return;
  g_assets_tried = true;
  char path[160];
  // Variante « verrouillée » du fond de tuile (onglet Favoris + deal-lock actif) : le
  // natif remplace itemwin_mid par itemwin_mid_lock (\inventory\, RE UIInventoryWnd_DrawContent).
  InventoryPath("itemwin_mid_lock.bmp", path, sizeof(path));
  g_tile_lock = ro::TextureFromGameFile(path);
  // Onglets images (basic_interface\<img>1.bmp actif / <img>2.bmp inactif).
  for (int c = 0; c < kNumCats; ++c) {
    const char* base = kCats[c].img;
    if (!base) continue;
    char nm[48];
    std::snprintf(nm, sizeof(nm), "%s1.bmp", base);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", base);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tab[c][1] = ro::TextureFromGameFile(path);
    // Jeu HORIZONTAL : même nom avec un « h » après « tab » (tab_use -> tabh_use).
    char hbase[40];
    std::snprintf(hbase, sizeof(hbase), "tabh%s", base + 3);  // saute "tab"
    std::snprintf(nm, sizeof(nm), "%s1.bmp", hbase);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][0] = ro::TextureFromGameFile(path);
    std::snprintf(nm, sizeof(nm), "%s2.bmp", hbase);
    ro::grid::BasicInterfacePath(nm, path, sizeof(path)); g_tabh[c][1] = ro::TextureFromGameFile(path);
  }
  // Boutons footer natifs (\inventory\..., préfixe CP949 via InventoryPath). idx1 =
  // état ACTIF (verrouillé / trié). Noms exacts = strings exe (RE 2026-07-09).
  InventoryPath("item_drop_lock_off.bmp", path, sizeof(path)); g_btn_drop[0] = ro::TextureFromGameFile(path);
  InventoryPath("item_drop_lock_on.bmp",  path, sizeof(path)); g_btn_drop[1] = ro::TextureFromGameFile(path);
  InventoryPath("bt_itemDeal_lock_off.bmp", path, sizeof(path)); g_btn_deal[0] = ro::TextureFromGameFile(path);
  InventoryPath("bt_itemdeal_lock_on.bmp",  path, sizeof(path)); g_btn_deal[1] = ro::TextureFromGameFile(path);
  InventoryPath("bt_sort_off.bmp", path, sizeof(path)); g_btn_sort[0] = ro::TextureFromGameFile(path);
  InventoryPath("bt_sort.bmp",     path, sizeof(path)); g_btn_sort[1] = ro::TextureFromGameFile(path);
  StyleshopPath("btn_bank_out.bmp",  path, sizeof(path)); g_btn_bank[0] = ro::TextureFromGameFile(path);
  StyleshopPath("btn_bank_over.bmp", path, sizeof(path)); g_btn_bank[1] = ro::TextureFromGameFile(path);
  StyleshopPath("btn_bank_down.bmp", path, sizeof(path)); g_btn_bank[2] = ro::TextureFromGameFile(path);
}

// Hauteur de la barre = hauteur du morceau milieu (repli 22 px si non chargé).
int FooterBarHeight() {
  const int h = ro::grid::Assets().bar[1].h;
  return h > 0 ? h : 22;
}

// Teinte skin (luminosité + opacité) pour les AddImage. Définie plus bas (avant
// DrawTiledBg) ; déclarée ici pour que DrawFooterBar/DrawFooterIcon la voient.

// Bouton-bascule IMAGE du footer : dessine le bmp natif on/off à sa taille native,
// centré verticalement sur cyc, + InvisibleButton (survol = pleine luminosité). Repli
// carré+glyphe si le bmp n'a pas chargé. Renvoie true au clic ; *out_w = largeur posée.
bool FooterImgToggle(const char* id, float x, float cyc, const BarTex& on,
                     const BarTex& off, bool active, const char* glyph, const char* tip,
                     float* out_w) {
  const BarTex& t = active ? on : off;
  const bool haveTex = t.tex && t.w > 0 && t.h > 0;
  const float w = haveTex ? static_cast<float>(t.w) : 18.0f;
  const float h = haveTex ? static_cast<float>(t.h) : 18.0f;
  const float y = cyc - h * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0(x, y), p1(x + w, y + h);
  if (haveTex) {
    const ImU32 tint = hov ? IM_COL32(255, 255, 255, 255) : ro::SkinImageTint();
    dl->AddImage(TexId(t.tex), p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);
  } else {  // repli glyphe (bmp absent)
    const ImU32 bg = active ? IM_COL32(120, 165, 225, 220)
                            : (hov ? IM_COL32(205, 205, 205, 210)
                                   : IM_COL32(185, 185, 185, 140));
    dl->AddRectFilled(p0, p1, bg, 2.0f);
    dl->AddRect(p0, p1, IM_COL32(110, 110, 110, 220), 2.0f);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f),
                active ? IM_COL32(255, 255, 255, 255) : IM_COL32(45, 45, 45, 255), glyph);
  }
  if (hov && tip) ImGui::SetTooltip(" %s ", tip);
  if (out_w) *out_w = w;
  return clicked;
}

// Bouton IMAGE à TROIS états (out / over / down), à la mode des boutons du client.
// FooterImgToggle, lui, est une BASCULE à deux images (on/off) : il n'a pas d'état
// « enfoncé », et son image dépend d'un booléen d'état qu'un simple bouton d'action
// n'a pas. Centré verticalement sur `cyc` mais BORNÉ dans la barre [bar_y0, bar_y1] :
// les bmps natifs (24 px) sont plus hauts qu'une demi-ligne de footer et déborderaient.
// `scale` réduit l'image par rapport à sa taille native, en gardant le ratio (1 =
// taille native). Renvoie true au clic ; *out_w = largeur posée (déjà mise à l'échelle).
bool FooterImgButton3(const char* id, float x, float cyc, float bar_y0, float bar_y1,
                      const BarTex states[3], const char* glyph, const char* tip,
                      float* out_w, float scale = 1.0f) {
  const BarTex& out_tex = states[0];
  const bool haveTex = out_tex.tex && out_tex.w > 0 && out_tex.h > 0;
  // `scale` ne s'applique qu'à l'ART : le repli glyphe garde ses 18 px, sinon un
  // bmp manquant laisserait une pastille trop petite pour être cliquée ou lue.
  const float w = haveTex ? static_cast<float>(out_tex.w) * scale : 18.0f;
  const float h = haveTex ? static_cast<float>(out_tex.h) * scale : 18.0f;
  float y = cyc - h * 0.5f;
  if (y < bar_y0 + 1.0f) y = bar_y0 + 1.0f;
  if (y + h > bar_y1 - 1.0f) y = bar_y1 - 1.0f - h;
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
  const bool hov = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p0(x, y), p1(x + w, y + h);
  if (haveTex) {
    const BarTex& shown = (held && states[2].tex)  ? states[2]
                        : (hov  && states[1].tex)  ? states[1]
                                                   : out_tex;
    dl->AddImage(TexId(shown.tex), p0, p1, ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
  } else {  // repli glyphe (bmp absent du GRF)
    dl->AddRectFilled(p0, p1,
                      held ? IM_COL32(150, 150, 150, 220)
                           : (hov ? IM_COL32(205, 205, 205, 210)
                                  : IM_COL32(185, 185, 185, 140)), 2.0f);
    dl->AddRect(p0, p1, IM_COL32(110, 110, 110, 220), 2.0f);
    const ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(x + (w - ts.x) * 0.5f, y + (h - ts.y) * 0.5f),
                IM_COL32(45, 45, 45, 255), glyph);
  }
  if (hov && tip) ImGui::SetTooltip(" %s ", tip);
  if (out_w) *out_w = w;
  return clicked;
}

// Largeur du strip d'onglets = plus grande largeur d'image d'onglet (repli 22 px).
// 🔴 Mise à l'échelle de l'interface, comme tout le chrome : « jamais étirée »
// veut dire « pas déformée », pas « figée en pixels d'écran ». Un strip resté à
// 22 px à côté d'une grille agrandie, c'est le défaut qu'on corrige ici.
float TabStripWidth() {
  float w = 0.0f;
  for (int c = 0; c < kNumCats; ++c)
    for (int s = 0; s < 2; ++s)
      if (g_tab[c][s].w > w) w = static_cast<float>(g_tab[c][s].w);
  return ro::Px(w > 0.0f ? w : 22.0f);
}

// Hauteur de la rangée d'onglets HORIZONTALE = plus grande hauteur du jeu tabh_*
// (repli 22 px). Comme pour la largeur du strip vertical : c'est la dimension
// transverse, jamais étirée — l'autre se déduit du ratio de chaque image.
float TabStripHeightH() {
  float h = 0.0f;
  for (int c = 0; c < kNumCats; ++c)
    for (int s = 0; s < 2; ++s)
      if (g_tabh[c][s].h > h) h = static_cast<float>(g_tabh[c][s].h);
  return ro::Px(h > 0.0f ? h : 22.0f);  // à l'échelle, cf. TabStripWidth
}

// Teinte des AddImage (icônes/tuiles/onglets/footer) = luminosité + opacité du skin RO,
// pour que ces réglages s'appliquent AUSSI aux images du jeu (dessinées en draw-list
// brut). b>1 ne peut pas sur-exposer via col -> capé à 1 ; a = style.Alpha du skin.

// Vide les caches de textures quand le device D3D a été reset/recréé (handles morts).
unsigned g_tex_epoch = 0;
void MaybeFlushTextures() {
  const unsigned e = Overlay_DeviceEpoch();
  if (e == g_tex_epoch) return;
  g_tex_epoch = e;
  g_tile_lock = BarTex{};
  for (auto& row : g_tab) for (auto& b : row) b = BarTex{};
  for (auto& row : g_tabh) for (auto& b : row) b = BarTex{};
  for (auto& b : g_btn_drop) b = BarTex{};
  for (auto& b : g_btn_deal) b = BarTex{};
  for (auto& b : g_btn_sort) b = BarTex{};
  for (auto& b : g_btn_bank) b = BarTex{};
  g_assets_tried = false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════
InventoryViewer::InventoryViewer() {
  // Sertissage rapide : on reçoit la liste des cartes compatibles (calculée serveur).
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kCompatCards);
  // 🔴 Sertissage : on prend la place du handler natif de ZC_ITEMCOMPOSITION_LIST,
  // SEUL créateur du popup 0x4A. Révocable : en mode natif le prédicat dit non et
  // le paquet repart chez lui à l'octet près, popup compris.
  Bourgeon::Instance().RegisterReplaceOpcode(kOpCompList,
                                             [this] { return imgui_enabled_; });
}

// Demande au serveur les cartes de l'inventaire sertissables sur `equipInvIndex`
// (index CLIENT, = info+0x04). No-op si c'est déjà l'équipement en cours -> une seule
// requête par ouverture de sous-menu. La réponse (OnRecvPacket) remplit qs_cards_.
void InventoryViewer::RequestCompatCards(int equipInvIndex) {
  if (equipInvIndex == qs_equip_index_) return;  // déjà demandé pour cet équip
  qs_equip_index_ = equipInvIndex;
  qs_card_count_ = 0;  // vidé jusqu'à la réponse -> le sous-menu affiche « … »
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = bopcodes::kReqCompatCards;
  *reinterpret_cast<uint16_t*>(pkt + 2) = 6;  // longueur fixe
  *reinterpret_cast<uint16_t*>(pkt + 4) = static_cast<uint16_t>(equipInvIndex);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

// Fil RÉSEAU : on COPIE, rien de plus (cf. features/net_inbox.h). Décoder ici
// écrirait dans nos tableaux pendant que le rendu les parcourt.
void InventoryViewer::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // ⚠ PushAnnounced UNIQUEMENT sur l'opcode REMPLACÉ : là, `data` commence bien sur
  // le champ longueur du paquet (régime « replace » = tout ce qui suit l'opcode).
  // Sur un opcode CUSTOM le dispatcher a déjà consommé l'en-tête, donc ces deux
  // premiers octets sont une DONNÉE (ici l'index d'équipement) — la prendre pour une
  // longueur ferait copier n'importe quoi.
  if (opcode == kOpCompList) net_inbox_.PushAnnounced(opcode, data, len);
  else                       net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué au tick dans l'ordre d'arrivée.
void InventoryViewer::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // ── ZC_ITEMCOMPOSITION_LIST 0x017B (revendiqué) ─────────────────────────────
  // data = [len:2] puis count * [equipInvIndex:2]. Le SERVEUR a déjà appliqué
  // toutes les règles de compatibilité : on prend sa liste telle quelle, on n'en
  // filtre aucune entrée (le client ne connaît pas ces règles).
  if (opcode == kOpCompList) {
    ci_cand_count_ = 0;
    for (int i = 0; i < kCiMaxCands; ++i) {
      const size_t off = 2 + static_cast<size_t>(i) * 2;
      if (off + 2 > len) break;
      ci_cands_[ci_cand_count_++] = *reinterpret_cast<const uint16_t*>(data + off);
    }
    // La carte source vient de CMode+0x45c, posé par le sélecteur 0x7B — donc
    // valable aussi pour la liste re-demandée après un lot de sertissages.
    ci_card_ = ReadModeCardIndex();
    ci_open_ = true;
    ci_sel_ = -1;  // la liste a changé : une sélection d'avant n'a plus de sens
    return;
  }

  // ── ZC_BOURGEON_COMPAT_CARDS : [equip:2][count:2] puis count*[cardIdx:2] ────
  // On ne garde la réponse que si elle concerne l'équipement encore demandé (une
  // réponse en retard pour un ancien équipement est ignorée).
  if (opcode != bopcodes::kCompatCards) return;
  if (len < 4) return;
  const uint16_t equip = *reinterpret_cast<const uint16_t*>(data);
  if (static_cast<int>(equip) != qs_equip_index_) return;  // réponse périmée
  int count = *reinterpret_cast<const int16_t*>(data + 2);
  if (count < 0) count = 0;
  int n = 0;
  for (int i = 0; i < count && n < kQsMaxCards; ++i) {
    const size_t off = 4 + static_cast<size_t>(i) * 2;
    if (off + 2 > len) break;
    qs_cards_[n++] = *reinterpret_cast<const uint16_t*>(data + off);
  }
  qs_card_count_ = n;
}

// La fenêtre native de l'inventaire vient de naître : c'est une DEMANDE du joueur
// (icône de menu, raccourci, ou le X de notre propre viewer). On la masque
// sur-le-champ — sans quoi une frame native passe à l'écran — et on bascule le
// viewer ; OnTick la détruira, le natif la manipulant encore ici.
void InventoryViewer::HandleNativeCreation(void* win) {
  HandleNativeToggle(win, uiwnd::kInventoryWndVTable);
}

// Popup de sertissage (id 0x4A) : FILET DE SÉCURITÉ. En mode moderne son unique
// créateur — le handler de ZC 0x017B — ne tourne plus, donc on ne devrait jamais
// passer ici. Si ça arrive (bascule de mode en plein sertissage, paquet arrivé
// pendant que le prédicat disait encore non), on le masque avant sa première frame
// et OnTick le détruit : masqué il resterait vivant et volerait le clavier — Entrée
// validerait son bouton OK invisible.
void InventoryViewer::HandleCardInsertCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  __try {
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Remplit items_/item_count_ depuis le MODÈLE SESSION (0x015fbab0), donc marche même
// fenêtre native cachée. POD-only sous SEH ; le nom complet est bâti via BuildDisplayName
// (repli GetBaseName), qui résout seul son contexte natif.
void InventoryViewer::Extract() {
  item_count_ =
      itemcell::ExtractList(rag::kInventoryListAddr, items_, kMaxItems);
}

void InventoryViewer::OnTick() {
  // (Le décodage des paquets ne se fait plus ici : Bourgeon draine la file de tous
  // les modules à chaque frame, cf. Bourgeon::DrainNetInboxes. Le faire au tick
  // retardait une liste de sertissage jusqu'à 100 ms.)

  // `open_` n'est plus déduit de la présence de la native : elle ne vit plus. Il
  // est posé par HandleNativeCreation (la demande du joueur) et levé par elle.
  const bool mode_changed = (imgui_enabled_ != prev_imgui_enabled_);
  prev_imgui_enabled_ = imgui_enabled_;
  if (!imgui_enabled_) {
    // Retour au natif : le viewer s'efface. La native n'existe plus, le client la
    // recréera donc à la prochaine demande.
    open_ = false;
    hover_desc_id_ = 0; hover_desc_idx_ = -1;
    // Un sertissage en cours au moment de la bascule n'a plus de fenêtre pour le
    // porter : on l'annule comme le ferait le bouton « cancel » du natif, sinon le
    // serveur resterait à nous attendre.
    if (mode_changed && ci_open_) { CancelComposition(); CloseCardInsert(); }
    return;
  }
  if (!Bourgeon::Instance().IsMapLoading()) {
    // 🔴 DÉTRUIRE, pas masquer : toute bascule du client fait « ferme si elle
    // existe, sinon crée » (cf. reference_native_window_toggle_router). Une native
    // seulement masquée existe, donc la demande suivante la fermerait sans
    // repasser par MakeWindow — un appui sur deux serait avalé — et elle
    // garderait le clavier. Couvre aussi la bascule de mode et la
    // reconstruction du HUD au changement de map.
    if (ReadValidWnd(uiwnd::kInventoryWndSlot, uiwnd::kInventoryWndVTable)) {
      // Sa présence PROUVE que l'inventaire était ouvert : on adopte l'état avant
      // de la détruire, sinon activer le mode moderne le ferait disparaître.
      if (mode_changed && !open_) { open_ = true; show_panel_ = true; need_pos_ = true; }
      __try {
      uiwnd::CloseWindow(uiwnd::kUIInventoryWnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    }
  }
  if (open_) Extract();
  // Aperçu de description : purgé dès que le viewer ne dessine plus (fenêtre fermée
  // ou viewer désactivé), sinon il resterait affiché sans rien pour l'effacer.
  if (!open_) { hover_desc_id_ = 0; hover_desc_idx_ = -1; }

  // Popup de sertissage : son unique créateur natif ne tourne plus, mais s'il en
  // naissait un (cf. HandleCardInsertCreation) on le DÉTRUIT — masqué, il garderait
  // le clavier et validerait son bouton OK sur Entrée.
  if (uiwnd::SafeFindWindow(uiwnd::kCardInsertWndId))
    uiwnd::SafeCloseWindow(uiwnd::kCardInsertWndId);
}

// (Plus de OnMouseDown / HandleNativeDrop : ils accueillaient un glisser NATIF
// venu de la fenêtre Équipement ou du cart. Les deux sont maintenant des viewers
// ImGui — la feuille de personnage pour l'équipement, CartViewer pour le cart —
// et leurs fenêtres natives ne naissent plus. Les deux gestes ont leur équivalent
// ImGui : la feuille renvoie ici par EquipDraggedItem pour équiper, et détecte
// elle-même le lâcher sur ce viewer pour déséquiper.)

// Équipe l'item d'inventaire actuellement glissé : drag_index_/type_/loc_ sont les
// valeurs SERVEUR stables (it.index/type/loc, rafraîchies chaque frame par la source du
// drag dont l'ID est semé par l'index stable) -> robuste à une renumérotation d'items_
// pendant le glisser, et indépendant de l'ordre de rendu des fenêtres. Utilisé par le
// drag-drop cross-plugin de character_sheet.
bool InventoryViewer::EquipDraggedItem(bool left_hand) {
  if (!drag_active_ || !IsEquippable(drag_type_)) return false;
  UseOrEquip(drag_index_, drag_type_, drag_loc_, left_hand);
  return true;
}

// Ajoute l'item ACTUELLEMENT GLISSÉ à l'échange en cours (drag-drop cross-plugin :
// lâcher un item de l'inventaire sur « Mon offre » dans la fenêtre d'échange ImGui).
// Même politique de quantité que le menu contextuel : une PILE ouvre le prompt de
// quantité, un item seul part directement. No-op si aucun glisser ou aucun échange.
// Arme le prompt de quantité sur l'item ACTUELLEMENT GLISSÉ. `action` dit ce
// qu'on en fera une fois le nombre choisi.
//
// ⚠ Les quatre champs partent ensemble ou pas du tout : un prompt ouvert sur un
// `pend_index_` périmé agirait sur le mauvais objet. Ils étaient écrits à
// l'identique dans les deux destinations (échange, courrier) — deux occasions
// d'en oublier un le jour où une troisième s'ajoute.
void InventoryViewer::ArmDragQuantityPrompt(int action) {
  pend_id_ = drag_index_;
  pend_index_ = drag_index_;
  pend_max_ = drag_amount_;
  pend_action_ = action;
  pend_open_prompt_ = true;
}

bool InventoryViewer::TradeDraggedItem() {
  if (!drag_active_) return false;
  auto* tt = Bourgeon::Instance().trade_window();
  if (!tt || !tt->active()) return false;
  if (drag_amount_ > 1) ArmDragQuantityPrompt(kPendToTrade);  // pile -> combien ?
  else                  tt->AddItemToTrade(drag_index_, 1);
  return true;
}

// Même chemin que TradeDraggedItem, vers le courrier en cours d'écriture : le
// serveur borne à 5 pièces jointes, on ne double donc pas ce contrôle ici.
bool InventoryViewer::MailDraggedItem() {
  if (!drag_active_) return false;
  auto* rodex = Bourgeon::Instance().rodex_window();
  if (!rodex || !rodex->composing()) return false;
  if (drag_amount_ > 1) ArmDragQuantityPrompt(kPendToMail);  // pile -> combien ?
  else                  rodex->AttachItem(drag_index_, 1);
  return true;
}

// Wrapper public sur le helper interne PostItemLinkToChat (insère le lien dans l'input
// chat focalisé). Réutilisé par character_sheet (Maj+clic gauche sur un slot équipé).
void InventoryViewer::LinkItemToChat(int invIndex) { PostItemLinkToChat(invIndex); }

// Fin de la session de sertissage. Il n'y a plus rien à fermer côté natif : le
// popup 0x4A ne naît plus (son handler créateur est à nous). On oublie simplement
// l'état, et le front montant se réarme pour la prochaine ouverture.
void InventoryViewer::CloseCardInsert() {
  ci_open_ = false;
  ci_card_ = 0;
  ci_cand_count_ = 0;
  ci_sel_ = -1;
  ci_was_open_ = false;
}

// ── Fenêtre de sertissage de cartes (remplace le popup natif id 0x4A) ─────────
// On ne rejoue PAS les règles : la liste des candidats est celle que le SERVEUR a
// envoyée dans ZC 0x017B, prise telle quelle (le client ne connaît AUCUNE règle de
// compatibilité). On la dessine, et on émet CZ_REQ_ITEMCOMPOSITION à la validation.
void InventoryViewer::RenderCardInsert() {
  if (!imgui_enabled_) return;
  if (!ci_open_) {  // pas de sertissage en cours
    // Réarmé ici aussi (pas seulement à la fermeture) : le rendu tourne à chaque
    // frame, donc un cycle fermeture/réouverture entre deux ticks garde le premier plan.
    ci_was_open_ = false;
    return;
  }

  constexpr int kMaxCands = kCiMaxCands;
  const int cardIndex = ci_card_;
  const int n = ci_cand_count_;

  CompItem card{};
  const bool has_card = ReadCompItemByIndex(cardIndex, &card);

  // Les fiches sont lues VIVANTES à chaque frame, jamais recopiées : le serveur
  // n'envoie une nouvelle liste que s'il en a une à envoyer — quand plus aucun
  // équipement n'est compatible il émet MSI_FAIL_ITEMCOMPOSITION_LIST à la place.
  // Un instantané resterait donc figé sur l'état d'avant le sertissage.
  // ⚠ FindLiveInfoByIndex, pas FindInfoByIndex : la liste session EXCLUT les pièces
  // PORTÉES, et le serveur propose couramment l'arme qu'on a en main.
  CompItem cands[kMaxCands];
  int cn = 0;
  int found = 0;  // entrées dont on a RETROUVÉ la fiche (pleines comprises)
  for (int i = 0; i < n; ++i) {
    // Remise à zéro obligatoire : la fiche est remplie EN PLACE et `used_slots` est
    // incrémenté, donc un `continue` qui laisse `cn` inchangé polluerait le candidat
    // suivant écrit au même rang.
    cands[cn] = CompItem{};
    uint8_t* live = static_cast<uint8_t*>(FindLiveInfoByIndex(ci_cands_[i]));
    if (!live) continue;  // item disparu depuis la réponse serveur
    if (!ReadCompItemFromInfo(live, &cands[cn])) continue;
    ++found;
    // Entrée PÉRIMÉE : plus un seul emplacement libre. Ce n'est pas un filtrage de
    // compatibilité (interdit — le serveur en est seul juge), c'est la MÊME borne que
    // clif_use_card côté serveur : il ne proposerait plus cet item.
    if (!cands[cn].forged && cands[cn].total_slots > 0 &&
        cands[cn].used_slots >= cands[cn].total_slots)
      continue;
    ++cn;
  }

  // Plus AUCUN candidat exploitable alors qu'on a bien retrouvé leurs fiches : ils
  // sont tous pleins. Le serveur ne renverra pas de liste vide (il émet
  // MSI_FAIL_ITEMCOMPOSITION_LIST), donc personne ne refermera à notre place.
  // ⚠ On exige `found > 0` et pas seulement `n > 0` : si on n'a retrouvé AUCUNE
  // fiche, c'est un échec de lecture de notre côté, pas un « tout est plein » — on
  // laisse alors la fenêtre ouverte pour que l'utilisateur puisse annuler lui-même.
  if (found > 0 && cn == 0) {
    CancelComposition();
    CloseCardInsert();
    return;
  }

  // La sélection mémorisée peut avoir disparu (item consommé, liste rafraîchie).
  if (ci_sel_ >= 0) {
    bool still = false;
    for (int i = 0; i < cn; ++i) if (cands[i].index == ci_sel_) { still = true; break; }
    if (!still) ci_sel_ = -1;
  }

  // Position de départ à nous : il n'y a plus de fenêtre native dont on héritait la
  // place. FirstUseEver, donc elle reste ensuite là où l'utilisateur la pose.
  ImGui::SetNextWindowPos(ImVec2(kCiSpawnX, kCiSpawnY), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(300, 320), ImGuiCond_FirstUseEver);
  // Premier plan À L'OUVERTURE seulement : le double-clic vient de l'inventaire, qui
  // a donc le focus — sans ça le popup se retrouve DERRIÈRE lui. On ne le force pas
  // à chaque frame, sinon la fenêtre deviendrait impossible à passer en arrière-plan.
  if (!ci_was_open_) {
    ImGui::SetNextWindowFocus();
    ci_was_open_ = true;
  }

  // Stock de la carte dans l'inventaire (somme des stacks du même nameid). Lu frais
  // -> se met à jour tout seul après chaque sertissage. Détermine aussi combien de
  // fois on peut enchaîner.
  const int stock = has_card ? CountCardStock(card.id) : 0;

  // Candidat survolé ce frame (aperçu de description au survol, peint après la fenêtre).
  const CompItem* hover = nullptr;

  bool open = true;
  const bool begun = ro::BeginRoWindow(i18n::Tr("Sertir une carte###bourgeon_card_insert"), &open,
                                       ImGuiWindowFlags_NoCollapse);
  if (begun) {
    // En-tête : la carte que l'on sertit + son total en inventaire.
    if (has_card) {
      ro::IconTex ic = ro::ItemIcon(card.id, card.identified);
      if (ic.tex) {
        ImGui::Image(TexId(ic.tex), ImVec2(24, 24));
        ImGui::SameLine();
      }
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(card.name[0] ? card.name : "Carte");
      ImGui::SameLine();
      ImGui::TextDisabled("(x%d)", stock);  // stock restant, mis à jour à chaque sertissage
    } else {
      ImGui::TextUnformatted(i18n::Tr("Carte introuvable"));
    }
    ImGui::Separator();

    if (cn == 0) {
      // On n'arrive ici qu'avec `found == 0` (le cas « tous pleins » a déjà refermé) :
      // soit le serveur a listé des index qu'on ne retrouve pas, soit sa liste
      // était vide.
      ImGui::TextWrapped(n > 0
          ? i18n::Tr("Impossible de retrouver les équipements proposés par le serveur.") : i18n::Tr("Aucun équipement compatible avec un emplacement libre."));
    } else {
      ImGui::TextDisabled(i18n::Tr("Choisissez l'équipement à sertir :"));
      const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
      if (ImGui::BeginChild("##ci_list", ImVec2(0, -footer), true)) {
        constexpr float kRowH = 26.0f;
        for (int i = 0; i < cn; ++i) {
          const CompItem& it = cands[i];
          ImGui::PushID(it.index);
          const bool sel = (ci_sel_ == it.index);

          // Le Selectable prend TOUTE la largeur (zone de clic confortable) ; l'icône
          // et le texte sont peints PAR-DESSUS via le draw list, en coordonnées écran.
          // Surtout pas de SetCursorPos() pour cela : déplacer le curseur hors du flux
          // étend les limites de la fenêtre sans soumettre d'item, ce qui déclenche
          // l'assertion ImGui « use Dummy() to grow window boundaries ».
          const ImVec2 scr = ImGui::GetCursorScreenPos();
          if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowDoubleClick,
                                ImVec2(0, kRowH))) {
            ci_sel_ = it.index;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && cardIndex > 0) {
              // 1 sertissage ; la session continue tant qu'il reste des cartes.
              if (!SertirTimes(cardIndex, it.index, 1, stock)) CloseCardInsert();
              else ci_sel_ = -1;
            }
          }
          if (ImGui::IsItemHovered()) hover = &it;  // aperçu de description au survol

          ImDrawList* dl = ImGui::GetWindowDrawList();
          float tx = scr.x + 2.0f;
          ro::IconTex ic = ro::ItemIcon(it.id, it.identified);
          if (ic.tex) {
            dl->AddImage(TexId(ic.tex), ImVec2(tx, scr.y + 1.0f),
                         ImVec2(tx + 24.0f, scr.y + 25.0f));
            tx += 28.0f;
          }
          // Nom natif (il inclut DÉJÀ le refine « +10 » -> ne pas le re-préfixer) + le
          // nombre d'emplacements « [N] », comme dans l'inventaire.
          char line[96];
          const char* nm = it.name[0] ? it.name : "(?)";
          if (it.total_slots > 0)
            std::snprintf(line, sizeof(line), "%s [%d]", nm, it.total_slots);
          else
            std::snprintf(line, sizeof(line), "%s", nm);
          const float ty = scr.y + (kRowH - ImGui::GetTextLineHeight()) * 0.5f;
          dl->AddText(ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text), line);
          // Slots déjà occupés : information utile pour choisir, jamais un filtre
          // (le serveur a déjà écarté les items sans emplacement libre).
          if (!it.forged && it.used_slots > 0) {
            char sl[32];
            std::snprintf(sl, sizeof(sl), i18n::Tr("  (%d sertie%s)"), it.used_slots,
                          it.used_slots > 1 ? "s" : "");
            const float w = ImGui::CalcTextSize(line).x;
            dl->AddText(ImVec2(tx + w, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), sl);
          }

          ImGui::PopID();
        }
      }
      ImGui::EndChild();
    }

    // Combien on peut sertir d'affilée sur l'équipement SÉLECTIONNÉ : borné par les
    // emplacements encore libres ET le stock de cartes.
    const CompItem* selItem = nullptr;
    for (int i = 0; i < cn; ++i) if (cands[i].index == ci_sel_) { selItem = &cands[i]; break; }
    int freeSlots = 0;
    if (selItem) {
      freeSlots = selItem->total_slots - selItem->used_slots;
      if (freeSlots < 0) freeSlots = 0;
    }
    int maxK = freeSlots < stock ? freeSlots : stock;  // sertissages possibles d'un coup
    if (maxK < 0) maxK = 0;

    // Boutons en largeur AUTO (w=0 = texte + marges natives).
    const bool can_ok = (selItem != nullptr && cardIndex > 0 && maxK >= 1);
    if (!can_ok) ImGui::BeginDisabled();
    if (ro::RoButton(i18n::Tr("Sertir"))) {
      if (!SertirTimes(cardIndex, ci_sel_, 1, stock)) CloseCardInsert();
      else ci_sel_ = -1;
    }
    // Sertir 2× à maxK× d'affilée sur le même équipement (remplit plusieurs slots d'un
    // coup). Chaque bouton « xK » est proposé uniquement s'il est réalisable.
    for (int k = 2; k <= maxK && k <= 4; ++k) {
      ImGui::SameLine();
      char lbl[8];
      std::snprintf(lbl, sizeof(lbl), "x%d", k);
      if (ro::RoButton(lbl)) {
        if (!SertirTimes(cardIndex, ci_sel_, k, stock)) CloseCardInsert();
        else ci_sel_ = -1;
      }
    }
    if (!can_ok) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Fermer"))) {
      CancelComposition();
      CloseCardInsert();
    }
  }
  ro::EndRoWindow();

  // Aperçu de description au survol, APRÈS la fenêtre (crée son propre popup) : le MÊME
  // que la grille d'inventaire quand l'option est active, sinon un tooltip texte simple.
  if (hover) {
    if (show_desc_tooltip_) {
      itemdesc::SimpleOpt sopts[5];
      for (int k = 0; k < hover->opt_count && k < 5; ++k) {
        sopts[k].index = hover->opts[k].index;
        sopts[k].value = hover->opts[k].value;
        sopts[k].param = hover->opts[k].param;
      }
      itemcell::DrawTooltip(hover->id, hover->forged ? nullptr : hover->cards,
                        hover->forged ? 0 : 4, sopts, hover->opt_count, hover->refine,
                        hover->name);
    } else {
      ImGui::BeginTooltip();
      const char* hn = hover->name[0] ? hover->name : "(?)";
      if (hover->total_slots > 0) ImGui::Text(" %s [%d] ", hn, hover->total_slots);
      else                        ImGui::Text(" %s ", hn);
      if (!hover->forged && hover->used_slots > 0)
        ImGui::TextDisabled(i18n::Tr(" %d carte(s) sertie(s) "), hover->used_slots);
      ImGui::EndTooltip();
    }
  }

  // Croix de la barre de titre = même effet qu'Annuler (aucun paquet émis).
  if (!open) { CancelComposition(); CloseCardInsert(); }
}

// ── Section « InventoryViewer » du panneau Moonlight ───────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent
// que l'état de CE plugin. MoonlightUi ne garde que l'appel et la décision
// de sauvegarder. Rend true si un réglage a changé.
bool InventoryViewer::DrawSettings() {
  bool changed = false;
  // Fenêtre membre du groupe « Interface moderne » (tout-ImGui ou tout-natif, plus
  // de mixe) : `SetModernInterface` écrit `imgui_enabled_` avec les autres. Ce qui
  // suit ne dit que ce que la bascule change pour l'inventaire.
  // 🔴 Plus de CASE ici (cf. skill_bar.cc et moonlight_ui.h) : l'interrupteur du
  // groupe est unique, en tête de « Interface de jeu ». On garde la DESCRIPTION.
  //
  // ⚠ Cette fenêtre reste l'ANCRE du groupe — `ModernInterfaceEnabled()` lit
  // `imgui_enabled_` ici. Ne pas renommer ni supprimer ce membre sans reprendre
  // cette fonction.
  ImGui::TextDisabled(i18n::Tr("Fenêtre du groupe « Interface moderne »"));
  SameLine(); HelpMarker(
      i18n::Tr("ON : inventaire ImGui moderne (grille d'icônes, onglets, recherche, "
      "double-clic utiliser/équiper, clic-droit, drag) et la fenêtre native "
      "est cachée.\nOFF (défaut) : inventaire natif classique, aucun viewer.\n\n"
      "Inclut la fenêtre de SERTISSAGE de cartes (double-clic sur une carte) : "
      "elle remplace le popup natif « Insert Card ». La liste des équipements "
      "compatibles reste calculée par le serveur, donc identique au natif."));

  ImGui::BeginDisabled(!imgui_enabled_);

  changed |= ro::RoCheckbox(i18n::Tr("Description au survol"), &desc_tooltip());
  SameLine(); HelpMarker(
      i18n::Tr("Survoler un item affiche un aperçu SIMPLIFIÉ (nom, illustration, "
      "texte, cartes et options) dans un panneau au skin RO, à la place du "
      "petit tooltip nom + quantité.\n"
      "La description COMPLÈTE reste accessible au Ctrl + clic droit / "
      "menu contextuel."));

  changed |= ro::RoCheckbox(i18n::Tr("Champ de filtre"), &show_filter());
  SameLine(); HelpMarker(
      i18n::Tr("Affiche la barre de recherche par nom au-dessus de la grille.\n"
      "Décoche pour gagner une ligne (le filtre est alors vidé)."));

  changed |= ro::RoCheckbox(i18n::Tr("Onglets verticaux (à gauche)"), &tabs_vertical());
  SameLine(); HelpMarker(
      i18n::Tr("ON (défaut) : onglets en colonne à gauche de la grille, comme la "
      "fenêtre native (images tab_*).\n"
      "OFF : rangée horizontale au-dessus de la grille (images tabh_*)."));

  changed |= ro::RoCheckbox(i18n::Tr("Verrouiller la taille"), &lock_size());
  SameLine(); HelpMarker(
      i18n::Tr("La fenêtre ne peut plus être redimensionnée (elle reste déplaçable)."));

  // Le placement libre EXIGE la taille verrouillée : une case est un index
  // absolu (ligne × colonnes), donc changer la largeur change le nombre de
  // colonnes et mélangerait toutes les positions mémorisées.
  ImGui::BeginDisabled(!lock_size());
  Indent();
    changed |= ro::RoCheckbox(i18n::Tr("Placement libre des items"), &free_layout());
    SameLine(); HelpMarker(
        i18n::Tr("Glisse un item sur une case vide pour l'y fixer ; sur une case "
        "occupée, les deux s'échangent. Les items sans case attribuée "
        "remplissent les trous restants, donc un nouvel objet ramassé ne "
        "bouscule plus ta disposition.\n\n"
        "L'onglet « Tout » n'est PAS concerné : il mélange les catégories, "
        "donc une case n'y désigne pas le même emplacement que dans "
        "l'onglet d'origine de l'item. Il garde le remplissage automatique.\n\n"
        "Nécessite « Verrouiller la taille » : les cases sont repérées par "
        "un index absolu, qu'un changement de largeur décalerait."));
  Unindent();
  ImGui::EndDisabled();

  ImGui::EndDisabled();

  // ── Tri serveur (mêmes combos que « Commands Settings ») ────────────────────
  // HORS du BeginDisabled ci-dessus : ce n'est pas un réglage du viewer mais un
  // réglage SERVEUR (@tri_inventaire / @tri_cart), qui vaut aussi pour les
  // fenêtres natives. Le serveur retrie et renvoie la liste ; le viewer comme le
  // natif l'affichent dans l'ordre reçu.
  // Pas de `changed` : l'état vit dans MoonlightUi et le serveur en est la source
  // (aucun réglage yaml de CE plugin n'a bougé).
  SeparatorText(i18n::Tr("Tri serveur"));
  ImGui::BeginDisabled(free_layout() && lock_size()); // placement libre = tri ImGui, pas serveur
  if (auto* mu = Bourgeon::Instance().moonlight_ui())
    mu->DrawSortModeCombo(MoonlightUi::kSortInventory);
  ImGui::EndDisabled();

  return changed;
}

void InventoryViewer::OnRenderUI() {
  // Le popup de sertissage est INDÉPENDANT du viewer d'inventaire : il est ouvert par
  // un paquet serveur et survit à la fermeture de l'inventaire. Donc AVANT le
  // early-return ci-dessous (ResolveIcon gère lui-même l'epoch du device).
  RenderCardInsert();

  // Pas dessinee ce frame => elle n a plus de rect : un depot lache sur sa
  // derniere position connue ne doit pas lui etre route.
  if (!open_ || !imgui_enabled_) { win_rect_.Invalidate(); return; }
  MaybeFlushTextures();  // device reset/TDR -> lâche les handles morts

  if (need_pos_) {
    // FirstUseEver : simple DÉFAUT de première ouverture ; ensuite ImGui garde la
    // position déplacée par le joueur. Ce défaut se lisait sur la fenêtre native,
    // qui ne naît plus — on le fixe, rabattu dans l'écran sur petite résolution.
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(std::min(kSpawnX, std::max(0.0f, screen.x - kSpawnW)),
               std::min(kSpawnY, std::max(0.0f, screen.y - kSpawnH))),
        ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(kSpawnW, kSpawnH), ImGuiCond_FirstUseEver);
  // Resize par PALIER de tuile (chrome mesuré la frame précédente) : largeur/hauteur
  // saute d'une colonne/ligne de tuiles -> jamais de colonne partielle (fix overflow).
  // Inutile quand la taille est verrouillée (plus aucun redimensionnement).
  if (ro::grid::Snap().valid && !lock_size_) {
    const float minGrid = 5.0f * (ro::grid::Snap().cell + ro::grid::Snap().gap) - ro::grid::Snap().gap;  // min 5 tuiles
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(ro::grid::Snap().chromew + minGrid, ro::grid::Snap().chromeh + minGrid),
        ImVec2(10000.0f, 10000.0f), ro::grid::SnapWindowSize);
  } else if (ro::grid::Snap().valid && win_rect_.valid()) {
    // Taille VERROUILLÉE : le callback de snap ne tourne plus (il n'agit que pendant
    // un redimensionnement), donc la fenêtre reste figée sur la hauteur qu'elle avait
    // — presque jamais un multiple exact de tuiles, d'où une dernière ligne coupée,
    // items ou pas. On la recale une fois sur le palier le plus proche ; la frame
    // suivante la taille correspond déjà et plus rien n'est forcé.
    const float step = ro::grid::Snap().cell + ro::grid::Snap().gap;
    int cols = static_cast<int>((win_rect_.w() - ro::grid::Snap().chromew + ro::grid::Snap().gap) / step + 0.5f);
    int rows = static_cast<int>((win_rect_.h() - ro::grid::Snap().chromeh + ro::grid::Snap().gap) / step + 0.5f);
    if (cols < 5) cols = 5;
    if (rows < 5) rows = 5;
    const ImVec2 snapped(ro::grid::Snap().chromew + cols * step - ro::grid::Snap().gap,
                         ro::grid::Snap().chromeh + rows * step - ro::grid::Snap().gap);
    if (std::fabs(snapped.x - win_rect_.w()) > 0.5f ||
        std::fabs(snapped.y - win_rect_.h()) > 0.5f)
      ImGui::SetNextWindowSize(snapped, ImGuiCond_Always);
  }

  // Bullet de la barre de titre = raccourci vers la config de CETTE fenêtre
  // (panneau Moonlight > Interface de jeu > Inventaire), comme le storage.
  ro::SetNextWindowTitleBullet(i18n::Tr("Options de l'inventaire"));
  // Pas de NoCollapse -> le skin RO affiche le bouton minimiser (repli barre de titre),
  // comme le natif ; clic dessus = SetWindowCollapsed (géré par BeginRoWindow).
  const bool begun = ro::BeginRoWindow(
      i18n::Tr("Inventaire###bourgeon_inventory"), &show_panel_,
      lock_size_ ? ImGuiWindowFlags_NoResize : 0);
  // À appeler que Begin ait renvoyé true ou non : la barre de titre existe même
  // fenêtre repliée, et la demande est consommée par BeginRoWindow.
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceInventory);
  // X du viewer : l'état d'ouverture est le NÔTRE maintenant, il n'y a plus de
  // fenêtre native à fermer. Réarme show_panel_ pour la prochaine ouverture.
  if (!show_panel_) { open_ = false; show_panel_ = true; }
  // Repliee ou clippee : ImGui ne l a pas dessinee, son rect deplie ne vaut
  // plus rien comme cible de depot.
  if (!begun) { win_rect_.Invalidate(); ro::EndRoWindow(); return; }

  // Bandeau pendant la composition d'un shop, comme dans les viewers cart et
  // storage. Il est ENCORE plus nécessaire ici : les entrées grisées « Vers le
  // cart » / « Vers le storage » n'existent que si la fenêtre correspondante est
  // ouverte, donc sans lui l'inventaire n'avertissait de rien du tout.
  if (viewers::VendingComposing())
    ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                       i18n::Tr("Shop en composition : les transferts sont figés."));

  const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
  win_rect_.Capture(wp.x, wp.y, ws.x, ws.y);

  // ── Action en attente (drop/transfert d'une pile) -> prompt quantité ──
  auto do_move = [this](int amount) {
    switch (pend_action_) {
      case kPendDrop:      SendDrop(pend_index_, amount); break;
      case kPendToCart:    SendCmd(kCmdToCart, pend_index_, amount); break;
      case kPendToStorage: SendCmd(kCmdToStorage, pend_index_, amount); break;
      case kPendToTrade:
        if (auto* tt = Bourgeon::Instance().trade_window())
          tt->AddItemToTrade(pend_index_, amount);
        break;
      case kPendToMail:
        if (auto* rodex = Bourgeon::Instance().rodex_window())
          rodex->AttachItem(pend_index_, amount);
        break;
      default: break;
    }
  };
  if (pend_id_ != 0) {
    if (pend_open_prompt_) { ro::OpenQuantityPrompt(this); pend_open_prompt_ = false; }
    else if (pend_max_ <= 1) { do_move(1); pend_id_ = 0; }
  }
  // Dialogue « combien ? » PARTAGÉ (ui/qty_prompt) : habillé RO, identique dans
  // l'inventaire, le storage et le cart.
  {
    const char* verb = pend_action_ == kPendDrop      ? "Jeter"
                     : pend_action_ == kPendToCart     ? i18n::Tr("Vers le cart")
                     : pend_action_ == kPendToStorage  ? i18n::Tr("Vers le storage")
                     : pend_action_ == kPendToMail     ? i18n::Tr("Joindre au courrier") : i18n::Tr("Déplacer");
    bool cancelled = false;
    const int qty = ro::QuantityPrompt(this, verb, pend_max_, &cancelled);
    if (qty > 0) { do_move(qty); pend_id_ = 0; }
    else if (cancelled) pend_id_ = 0;
  }

  // Aide raccourcis : le texte est construit ici, mais le "(?)" est émis dans le
  // FOOTER (cf. plus bas) pour ne pas manger une ligne au-dessus de la grille.
  std::string desc = i18n::Tr("Raccourcis inventaire\n\n"
                     "- Double-clic gauche : utiliser / équiper\n"
                     "- Ctrl + double-clic gauche : équiper en main gauche\n"
                     "- Maj + clic gauche : lien de l'item dans l'input du chat (puis Entrée)\n"
                     "- Clic droit : menu contextuel\n"
                     "- Ctrl + clic droit : description\n"
                     "- Maj + clic droit : (dé)favori\n"
                     "- Alt + clic droit : transfert rapide (storage / cart si ouvert)\n"
                     "- Glisser : cart / storage / équipement / barre d'action / sol\n"
                     "- Glisser un favori sur un autre onglet : le retirer des favoris");
  static ImGuiTextFilter filter;
  if (show_filter_) {
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##inv_filter", i18n::Tr("Filtrer..."), filter.InputBuf,
                                 IM_ARRAYSIZE(filter.InputBuf)))
      filter.Build();
  } else if (filter.InputBuf[0]) {
    // Filtre masqué : on le vide, sinon il continuerait de cacher des items sans
    // que rien à l'écran ne l'explique.
    filter.Clear();
  }

  // ── Dimensions communes (footer réservé + snap) ──
  LoadFooterAssets();
  const float lineH = ImGui::GetTextLineHeight();
  const float footerH = 2.0f * lineH + 6.0f;  // 2 lignes compactes (barre btnbar étirée)
  const ImGuiStyle& style = ImGui::GetStyle();
  const float mainW = ImGui::GetWindowWidth();
  const float mainH = ImGui::GetWindowHeight();
  const float childH = -(footerH + style.ItemSpacing.y);

  // ── Onglets IMAGES (comme inventory_tweaks : tab_use/cos/etc/card/fav ; actif =
  //    <img>1.bmp, inactif = <img>2.bmp). Deux dispositions au choix : strip
  //    VERTICAL à gauche (défaut, comme le natif) ou rangée HORIZONTALE au-dessus
  //    de la grille, qui utilise alors le jeu d'images tabh_* (mêmes noms avec un
  //    « h » après « tab »). Sans image chargée -> libellé texte.
  const bool vtabs = tabs_vertical_;
  const float tabW = TabStripWidth();
  const float tabH = TabStripHeightH();
  // Rect (écran) de l'onglet ACTIF, capturé dans la boucle -> sert à "manger" le bord
  // C5C5C5 entre le strip et la grille (passage blanc continu = souligne l'onglet actif).
  ImVec2 activeTabMin(0, 0), activeTabMax(0, 0);
  bool haveActiveTab = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));  // strip sans marge
  ImGui::BeginChild("inv_tabs", vtabs ? ImVec2(tabW, childH) : ImVec2(0.0f, tabH),
                    false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  // Jointifs : sur l'axe de la rangée (vertical ou horizontal selon la disposition).
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      vtabs ? ImVec2(0.0f, -1.0f) : ImVec2(-1.0f, 0.0f));
  {
    ImDrawList* tdl = ImGui::GetWindowDrawList();
    for (int c = 0; c < kNumCats; ++c) {
      const bool sel = (cur_tab_ == c);
      ImGui::PushID(c);
      // Jeu d'images selon la disposition ; repli sur l'autre état si un seul des
      // deux .bmp a pu être chargé.
      const BarTex (&set)[2] = vtabs ? g_tab[c] : g_tabh[c];
      const BarTex& img = set[sel ? 0 : 1].tex ? set[sel ? 0 : 1] : set[sel ? 1 : 0];
      if (!vtabs && c) ImGui::SameLine();
      if (img.tex && img.w > 0 && img.h > 0) {
        // On fixe la dimension TRANSVERSE (largeur en vertical, hauteur en
        // horizontal) et on déduit l'autre du ratio : jamais d'étirement.
        const float iw = vtabs ? tabW : tabH * img.w / static_cast<float>(img.h);
        const float ih = vtabs ? tabW * img.h / static_cast<float>(img.w) : tabH;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("tab", ImVec2(iw, ih))) cur_tab_ = c;
        const ImVec2 pe(p.x + iw, p.y + ih);
        // L'image active/inactive indique déjà la sélection -> pas de cadre jaune.
        tdl->AddImage(TexId(img.tex), p, pe, ImVec2(0, 0), ImVec2(1, 1), ro::SkinImageTint());
        if (!sel && ImGui::IsItemHovered())
          tdl->AddRectFilled(p, pe, IM_COL32(255, 255, 255, 45));  // survol : éclaircir (pas de bleu)
      } else {
        const ImVec2 sz = vtabs
            ? ImVec2(tabW, 0.0f)
            : ImVec2(ImGui::CalcTextSize(i18n::Tr(kCats[c].label)).x +
                         ImGui::GetStyle().FramePadding.x * 2.0f, tabH);
        if (ImGui::Selectable(i18n::Tr(kCats[c].label), sel, 0, sz)) cur_tab_ = c;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(" %s ", i18n::Tr(kCats[c].label));
      // Glisser un item sur un onglet : sur Favoris = l'AJOUTE aux favoris ; hors de
      // Favoris (item déjà favori) = le RETIRE (comme le natif).
      if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("INV_ITEM")) {
          const int di = *static_cast<const int*>(pl->Data);
          if (di >= 0 && di < item_count_) {
            if (kCats[c].fav && !items_[di].favorite)
              SendFavoriteToggle(items_[di].index, false);  // -> ajoute aux favoris
            else if (!kCats[c].fav && items_[di].favorite)
              SendFavoriteToggle(items_[di].index, true);   // -> retire des favoris
          }
        }
        ImGui::EndDragDropTarget();
      }
      if (sel) {  // onglet actif -> mémorise son rect pour le passage blanc vers la grille
        activeTabMin = ImGui::GetItemRectMin();
        activeTabMax = ImGui::GetItemRectMax();
        haveActiveTab = true;
      }
      ImGui::PopID();
    }
  }
  ImGui::PopStyleVar();  // ItemSpacing
  ImGui::EndChild();
  ImGui::PopStyleVar();  // WindowPadding (strip)

  // ── Vue filtrée (onglet courant + recherche) ──
  // ORDRE D'AFFICHAGE : par défaut on garde l'ORDRE DE LA LISTE SESSION, tel quel.
  // C'est exactement ce que fait le natif (UIInventoryWnd_BuildFavList 0x0095af80
  // parcourt la liste 0..count-1 via Inventory_CopyItemAt et ne la trie QUE si
  // g_inv_sortEnabled). Cet ordre est celui d'arrivée des paquets : le serveur
  // moonlight vide la liste (ZC_INVENTORY_START type 0 -> 0x00cd8d10) puis la
  // renvoie triée à chaque @tri_inventaire / changement du réglage « Tri
  // Inventaire ». Re-trier par index d'inventaire, comme on le faisait, écrasait
  // ce tri (les non-empilables arrivent APRÈS les empilables, donc index croissant
  // != ordre serveur) et l'inventaire moderne semblait ignorer le réglage.
  auto in_tab = [](int tab, const Item& it) -> bool {
    const Cat& c = kCats[tab];
    if (c.fav) return it.favorite != 0;   // onglet Favoris : uniquement les favoris
    if (it.favorite != 0) return false;   // ailleurs : favoris MASQUÉS (comme le natif)
    if (!c.types) return true;            // Tout (hors favoris)
    for (int i = 0; i < c.n; ++i) if (c.types[i] == it.type) return true;
    return false;
  };
  std::vector<int> view;
  view.reserve(item_count_);
  for (int i = 0; i < item_count_; ++i)
    if (in_tab(cur_tab_, items_[i]) && filter.PassFilter(items_[i].name))
      view.push_back(i);
  // L'onglet « Tout » est EXCLU du placement libre : il mélange toutes les
  // catégories, donc une même case n'y désigne pas le même emplacement que dans
  // l'onglet d'origine de l'item (détail plus bas, à la construction de cell_of).
  const bool tabAll = (kCats[cur_tab_].types == nullptr && !kCats[cur_tab_].fav);
  const bool freeLayoutActive = free_layout_ && lock_size_ && !tabAll;
  // Tri CLIENT (bouton « T » du footer) : uniquement là où le bouton existe, comme
  // le natif — g_inv_sortEnabled ne gouverne QUE la liste des Favoris
  // (UIInventoryWnd_BuildFavList), pas les autres onglets. Même condition que
  // `showSort` plus bas (Favoris + taille non verrouillée), sinon un tri invisible
  // continuerait d'écraser l'ordre serveur sur tous les onglets.
  const bool clientSort = sort_enabled_ && kCats[cur_tab_].fav && !lock_size_;
  if (clientSort) {
    std::sort(view.begin(), view.end(), [this](int a, int b) {
      if (items_[a].type != items_[b].type) return items_[a].type < items_[b].type;
      return _stricmp(items_[a].name, items_[b].name) < 0;
    });
  } else if (freeLayoutActive) {
    // Placement libre : la disposition appartient au joueur (layout_), donc c'est
    // le SEUL mode où l'ordre serveur est ignoré. Les items sans case attribuée
    // sont rangés par index d'inventaire — ordre stable, qu'un tri serveur ne
    // fera pas sauter d'une case à l'autre.
    std::sort(view.begin(), view.end(),
              [this](int a, int b) { return items_[a].index < items_[b].index; });
  }
  // Sinon : aucun tri — l'ordre de la liste session est conservé tel quel.

  // ── Grille de tuiles 32px (fond itemwin_mid) : à DROITE des onglets en
  //    disposition verticale, EN DESSOUS en horizontale. Défile. ──
  if (vtabs) {
    ImGui::SameLine(0.0f, 0.0f);
  } else {
    // Collée à la rangée d'onglets : on reprend l'ItemSpacing vertical que le child
    // du strip vient d'insérer.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y);
  }
  // Style du MENU CONTEXTUEL, mémorisé AVANT les push de la grille : celle-ci
  // tourne en WindowPadding 0 / ItemSpacing jointif (tuiles collées), et un popup
  // ouvert dans ce scope en hérite — entrées serrées, sans marge. Le menu du
  // storage, lui, est rendu au style normal de la fenêtre : c'est cette référence
  // qu'on repousse autour du popup (cf. plus bas).
  const ImVec2 menu_pad = style.WindowPadding;
  const ImVec2 menu_spacing = style.ItemSpacing;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));  // grille sans marge
  // En horizontal, la rangée d'onglets a déjà consommé sa hauteur : la grille prend
  // le reste (hauteur négative = « place restante moins le footer »).
  ImGui::BeginChild("invgrid", ImVec2(0.0f, childH), true,
                    ImGuiWindowFlags_AlwaysVerticalScrollbar);
  {
    // Tuiles de 32 px (la taille native du client), jointives (sans padding) —
    // et À L'ÉCHELLE de l'interface, sinon toute la grille reste minuscule
    // pendant que le texte des onglets et du footer grandit autour d'elle.
    // Le fond pavé suit le même facteur (DrawTiledBg), donc les tuiles du fond
    // restent alignées sur les cases.
    const float cell = ro::Px(32.0f), gap = 0.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const float availw = ImGui::GetContentRegionAvail().x;  // exclut déjà la scrollbar
    const float availh = ImGui::GetContentRegionAvail().y;
    // Mesure du chrome (fenêtre - zone grille) pour le snap de resize (frame +1).
    ro::grid::Snap().cell = cell; ro::grid::Snap().gap = gap;
    ro::grid::Snap().chromew = mainW - availw;
    ro::grid::Snap().chromeh = mainH - availh;
    ro::grid::Snap().valid = true;
    int cols = static_cast<int>((availw + gap) / (cell + gap));
    if (cols < 1) cols = 1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Fond itemwin_mid PAVÉ, ALIGNÉ sur la grille d'items (même marge que les tuiles).
    {
      const ImVec2 gridOrigin = ImGui::GetCursorScreenPos();  // = position de la 1re tuile
      const ImVec2 gmn = ImGui::GetWindowPos();
      const ImVec2 gsz = ImGui::GetWindowSize();
      // Onglet Favoris + deal-lock actif -> fond « verrouillé » (itemwin_mid_lock), comme
      // le natif (UIInventoryWnd_DrawContent : this+0x10c==3 && g_inv_dealLock). Repli sur
      // le fond normal si le bmp verrouillé n'a pas chargé.
      const bool tilesLocked = kCats[cur_tab_].fav && ReadLock(rag::kFavoriteModeFlagAddr);
      const BarTex& bg = (tilesLocked && g_tile_lock.tex) ? g_tile_lock : ro::grid::Assets().tile;
      ro::grid::DrawTiledBg(dl, bg, gridOrigin, gmn, ImVec2(gmn.x + gsz.x, gmn.y + gsz.y));
    }
    // ── Placement LIBRE (option, exige la taille verrouillée) ────────────────
    // Chaque item mémorise un index de case ABSOLU (ligne × cols + colonne). On
    // pose d'abord ceux qui ont une case libre, puis les autres dans les premières
    // cases restantes -> un item nouvellement ramassé ne bouscule personne. Sans
    // l'option : remplissage séquentiel comme avant (cell_of[k] = k).
    // L'onglet « Tout » est EXCLU du placement libre (tabAll, calculé plus haut avec
    // l'ordre d'affichage) : il mélange toutes les catégories, donc une même case
    // n'y désigne pas le même emplacement que dans l'onglet d'origine de l'item — y
    // déplacer quoi que ce soit casserait la disposition des autres onglets. Il
    // garde le remplissage automatique.
    const bool freeGrid = freeLayoutActive;
    std::vector<int> cell_of;  // case -> rang dans `view` (-1 = case vide)
    if (freeGrid) {
      const int nitems = static_cast<int>(view.size());
      int ncells = ((nitems + cols - 1) / cols) * cols;  // lignes pleines
      for (int k = 0; k < nitems; ++k) {                 // étendre si une case assignée est au-delà
        auto a = layout_.find(items_[view[k]].id);
        if (a != layout_.end() && a->second >= ncells) ncells = a->second + 1;
      }
      // Toute la surface VISIBLE doit être posable : sans ça, les lignes vides sous
      // le dernier item n'étaient que le pavage de fond — aucune case, donc aucune
      // cible de dépose. On complète jusqu'à remplir la hauteur affichée, sans
      // JAMAIS dépasser : une ligne de marge en trop suffit à rendre la grille
      // défilante et à réveiller la scrollbar.
      const int rows_visible = static_cast<int>((availh + gap) / (cell + gap));
      if (ncells < rows_visible * cols) ncells = rows_visible * cols;
      if (ncells < cols) ncells = cols;
      cell_of.assign(ncells, -1);
      std::vector<bool> placed(nitems, false);
      for (int k = 0; k < nitems; ++k) {  // 1) cases assignées
        auto a = layout_.find(items_[view[k]].id);
        if (a == layout_.end()) continue;
        const int c = a->second;
        if (c >= 0 && c < ncells && cell_of[c] < 0) { cell_of[c] = k; placed[k] = true; }
      }
      int next = 0;
      for (int k = 0; k < nitems; ++k) {  // 2) le reste dans les cases libres
        if (placed[k]) continue;
        while (next < ncells && cell_of[next] >= 0) ++next;
        if (next >= ncells) break;
        cell_of[next] = k;
      }
    }
    // Dépose d'un item sur la case `c` : lui assigne cette case, et rend la sienne
    // à l'occupant éventuel (échange) plutôt que de l'écraser.
    auto drop_on_cell = [&](int c) {
      if (!ImGui::BeginDragDropTarget()) return;
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("INV_ITEM")) {
        const int di = *static_cast<const int*>(pl->Data);
        if (di >= 0 && di < item_count_) {
          const uint32_t moved = items_[di].id;
          int prev = -1;
          auto old = layout_.find(moved);
          if (old != layout_.end()) prev = old->second;
          if (c >= 0 && c < static_cast<int>(cell_of.size()) && cell_of[c] >= 0) {
            const uint32_t occupant = items_[view[cell_of[c]]].id;
            if (occupant != moved) {
              if (prev >= 0) layout_[occupant] = prev;
              else           layout_.erase(occupant);
            }
          }
          layout_[moved] = c;
          if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
        }
      }
      ImGui::EndDragDropTarget();
    };

    const int ncells = freeGrid ? static_cast<int>(cell_of.size())
                                : static_cast<int>(view.size());
    // Aperçu de description au survol : recalculé à chaque frame (0 = aucune case).
    hover_desc_id_ = 0;
    hover_desc_idx_ = -1;
    for (int k = 0; k < ncells; ++k) {
      if (k % cols != 0) ImGui::SameLine();
      const int rank = freeGrid ? cell_of[k] : k;
      if (rank < 0) {  // case VIDE : simple cible de dépose (le fond est déjà pavé)
        ImGui::PushID(-1 - k);
        ImGui::InvisibleButton("empty", ImVec2(cell, cell));
        drop_on_cell(k);
        ImGui::PopID();
        continue;
      }
      const int idx = view[rank];
      const Item& it = items_[idx];
      // ID semé par l'index serveur STABLE (pas la position volatile) : si l'inventaire
      // est renuméroté pendant un glisser (conso/autoloot serveur), le drag reste collé
      // au bon item et le payload de position s'auto-corrige à la frame suivante.
      ImGui::PushID(it.index);
      const ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("cell", ImVec2(cell, cell));
      const bool hovered = ImGui::IsItemHovered();
      const ImVec2 p1 = ImVec2(p0.x + cell, p0.y + cell);

      // Fond = pavage itemwin_mid dessiné globalement (DrawTiledBg). Ici : juste la
      // surbrillance au survol (pas de cadre favori : les favoris sont sur leur onglet).
      if (hovered)  // survol : léger éclaircissement (le HeaderHovered du skin est BLEU)
        dl->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 55), 0.0f);

      // Tuile de grille (icône centrée + badge coin) : brique partagée,
      // cf. features/item_cell.h — même rendu ici et dans le chariot.
      const ro::IconTex ic = ro::ItemIcon(it.id, it.identified);
      itemcell::DrawTile(dl, p0, p1, cell, ic, it.refine, it.amount,
                         it.damaged != 0);

      // Survol : tooltip + double-clic = utiliser/équiper.
      if (hovered) {
        // Option « Description au survol » : on retient la case survolée, l'aperçu RO
        // est dessiné après la fenêtre (cf. fin de OnRenderUI) et REMPLACE le tooltip
        // texte. Pas pendant un glisser : l'aperçu masquerait la cible du drop.
        if (show_desc_tooltip_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            ImGui::GetDragDropPayload() == nullptr && !desc_lock_.BlocksHover()) {
          hover_desc_id_ = it.id;
          hover_desc_idx_ = idx;
        } else if (!show_desc_tooltip_) {
          ImGui::BeginTooltip();
          char lbl[96], padded[100];
          std::snprintf(padded, sizeof(padded), " %s ",
                        itemcell::Label(lbl, sizeof(lbl), it.name,
                                        it.total_slots));
          itemcell::NameText(padded, it.damaged != 0);
          if (it.amount > 1) ImGui::TextDisabled(i18n::Tr(" Quantité : %d "), it.amount);
          ImGui::EndTooltip();
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
          UseOrEquip(it.index, it.type, it.loc, ImGui::GetIO().KeyCtrl);  // Ctrl = main gauche
      }

      // Raccourcis clavier+souris natifs (RE UIInventoryWnd_OnRButtonDown) :
      //   Shift + clic GAUCHE  = poster le lien de l'item dans le chat (0x14e) ;
      //   Ctrl  + clic DROIT   = ouvrir la description directement (sans menu) ;
      //   Shift + clic DROIT   = (dé)favori ;
      //   Alt   + clic DROIT   = transfert rapide vers storage (sinon cart, sinon échange) si ouvert ;
      //   clic DROIT seul      = menu contextuel.
      const ImGuiIO& mods = ImGui::GetIO();
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && mods.KeyShift)
        PostItemLinkToChat(it.index);  // Shift+clic G : lien item -> input chat focus

      // Menu contextuel : clic DROIT sur la case (pas sur le fond vide) -> popup
      if (IsLastItemRightClicked()) {
        if (mods.KeyCtrl) {
          desc_lock_.Arm();  // bloque l'aperçu au survol jusqu'à la fenêtre de desc
          POINT pt; if (GetCursorPos(&pt)) OpenItemDesc(it.index, pt.x, pt.y);
        } else if (mods.KeyShift) {
          SendFavoriteToggle(it.index, it.favorite != 0);
        } else if (mods.KeyAlt) {
          if (viewers::StorageOpen())   SendCmd(kCmdToStorage, it.index, it.amount);
          else if (viewers::CartOpen()) SendCmd(kCmdToCart, it.index, it.amount);
          else if (TradeOpen())
            if (auto* tt = Bourgeon::Instance().trade_window())
              tt->AddItemToTrade(it.index, it.amount);
        } else {
          ImGui::OpenPopup("ctx");
        }
      }

      // Source de drag (transfert cart/storage ou jet au sol selon la cible).
      // Marge interne du fantôme : la grille pousse WindowPadding à 0 (tuiles
      // jointives), et la tooltip de drag d'ImGui hérite de ce style au Begin ->
      // on la surcharge le temps du bloc pour aérer l'icône + le nom.
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
      if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        drag_active_ = true;
        drag_index_ = it.index; drag_amount_ = it.amount; drag_type_ = it.type; drag_loc_ = it.loc;
        drag_id_ = it.id;  // nameid : cible d'un dépôt sur la barre d'action (skill_bar)
        ImGui::SetDragDropPayload("INV_ITEM", &idx, sizeof(idx));
        if (ic.tex) { ImGui::Image(TexId(ic.tex), ImVec2(24, 24)); ImGui::SameLine(); }
        ImGui::TextUnformatted(it.name[0] ? it.name : "(?)");
        // Survol d'une cible qui sera refusée : on le dit PENDANT le glisser, seul
        // moment où l'on peut encore renoncer (storage ouvert = pas de
        // inventaire -> cart ; shop en composition = plus rien ne bouge).
        const ImVec2 drag_mouse = ImGui::GetMousePos();
        if (viewers::VendingComposing())
          ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                             i18n::Tr("Shop en composition : les transferts sont figés"));
        else if (viewers::StorageOpen() && viewers::MouseOverCart(drag_mouse.x, drag_mouse.y))
          ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                             i18n::Tr("Storage ouvert : vers le cart impossible"));
        ImGui::EndDragDropSource();
      }
      ImGui::PopStyleVar();  // WindowPadding (marge du fantôme de drag)
      // Placement libre : une case OCCUPÉE est aussi une cible -> lâcher dessus
      // échange les deux positions (drop_on_cell rend sa case à l'occupant).
      if (freeGrid) drop_on_cell(k);
      // (Le déséquip par glisser-vers-l'inventaire est détecté côté character_sheet via
      //  PointOverViewer -> couvre TOUTE la fenêtre, pas seulement les cases avec item.)

      // Menu contextuel : toutes les actions. Style NORMAL de la fenêtre (marges +
      // espacement), pas le style jointif de la grille dans laquelle il est ouvert.
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, menu_pad);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, menu_spacing);
      if (ImGui::BeginPopup("ctx")) {
        char lbl[96];
        // En-tête grisé comme avant, mais avec l'ombre rouge si l'item est cassé
        // (NameText prend la couleur du style courant).
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        itemcell::NameText(itemcell::Label(lbl, sizeof(lbl), it.name,
                                           it.total_slots),
                           it.damaged != 0);
        ImGui::PopStyleColor();
        ImGui::Separator();
        if (ImGui::MenuItem(i18n::Tr("Description"))) {
          // Le menu se ferme AVANT que la fenêtre de description n'apparaisse :
          // sans ce verrou, l'aperçu au survol se rouvre entre les deux (flicker).
          desc_lock_.Arm();
          POINT pt; if (GetCursorPos(&pt)) OpenItemDesc(it.index, pt.x, pt.y);
        }
        // Une carte (type 6) n'est ni « utilisée » ni « équipée » : le double-clic
        // natif ouvre le sertissage (UseOrEquip -> kCmdCard 0x7b). On l'étiquette donc
        // « Sertir » — même chemin de code, libellé juste.
        const char* act = (it.type == 0 || it.type == 1 || it.type == 2 ||
                           it.type == 0x12) ? "Utiliser"
                        : (it.type == 6)    ? "Sertir" : i18n::Tr("Équiper");
        const bool usable = it.type <= 2 || it.type == 0x12 ||
                            (it.type >= 4 && it.type <= 0xf) || it.type == 6 ||
                            it.type == 0xa || it.type == 0x10 || it.type == 0x11 ||
                            it.type == 0x13;
        if (usable && ImGui::MenuItem(act)) UseOrEquip(it.index, it.type, it.loc, false);

        // ── Sertissage rapide : sous-menu listant les cartes de l'inventaire
        // sertissables sur CET équipement (arme/armure avec un slot libre). La liste
        // est calculée par le SERVEUR (pc_can_insert_card) -> exacte. Sur un item
        // forgé, cards[0] porte les données du forgeron (id <= 500) : pas de slot réel.
        if (it.type == 4 || it.type == 5) {
          const bool forged = (it.cards[0] != 0 && it.cards[0] <= 500);
          void* einfo = FindInfoByIndex(it.index);
          const int total = einfo ? itemcell::SlotCount(einfo) : 0;
          // Ne compter QUE les `total` premières entrées : les enchantements occupent
          // les entrées hautes (card[3], card[2]…) sans consommer d'emplacement de
          // carte. Cf. ReadCompItemFromInfo pour le détail.
          int used = 0;
          if (!forged)
            for (int k = 0; k < total && k < 4; ++k) if (it.cards[k]) ++used;
          if (!forged && total > used) {  // au moins un emplacement libre
            if (ImGui::BeginMenu(i18n::Tr("Sertissage rapide"))) {
              RequestCompatCards(it.index);  // no-op si déjà demandé pour cet équip
              if (qs_equip_index_ == it.index && qs_card_count_ > 0) {
                for (int c = 0; c < qs_card_count_; ++c) {
                  CompItem cd{};
                  if (!ReadCompItemByIndex(qs_cards_[c], &cd)) continue;
                  ImGui::PushID(qs_cards_[c]);
                  if (ImGui::MenuItem(cd.name[0] ? cd.name : "(carte)")) {
                    // Sertit directement (aucun popup en jeu) : juste le paquet 0x017C.
                    // NE PAS passer par SertirTimes (qui rouvrirait/fermerait le popup).
                    SendCmd(kCmdComposition, it.index, qs_cards_[c]);
                    qs_equip_index_ = -1;  // liste périmée après sertissage -> re-demande
                  }
                  ImGui::PopID();
                }
              } else if (qs_equip_index_ == it.index) {
                ImGui::TextDisabled(i18n::Tr("Aucune carte compatible"));
              } else {
                ImGui::TextDisabled(i18n::Tr("Chargement…"));
              }
              ImGui::EndMenu();
            }
          }
        }

        if (ImGui::MenuItem(it.favorite ? i18n::Tr("Retirer des favoris") : i18n::Tr("Ajouter aux favoris")))
          SendFavoriteToggle(it.index, it.favorite != 0);
        // alootid : ramassage auto par ID (via MoonlightUi, comme le bouton d'item_desc).
        if (auto* mui = Bourgeon::Instance().moonlight_ui()) {
          const bool inAloot = mui->IsAlootId(it.id);
          if (ImGui::MenuItem(inAloot ? i18n::Tr("Retirer de l'alootid") : i18n::Tr("Ajouter à l'alootid"))) {
            if (inAloot) mui->RemoveAlootId(it.id); else mui->AddAlootId(it.id);
          }
        }
        ImGui::Separator();
        // Jeter (au sol) — grisé si le verrou drop (bouton footer) est actif.
        const bool dropLocked = ReadLock(kDropLockGlobal);
        if (dropLocked) ImGui::BeginDisabled();
        if (it.amount <= 1) {
          if (dropLocked) 
            ImGui::MenuItem(i18n::Tr("Jeter - verrouillé"));
          else
            if (ImGui::MenuItem(i18n::Tr("Jeter"))) SendDrop(it.index, 1);
        }
        else
          if (dropLocked) 
            ImGui::MenuItem(i18n::Tr("Jeter - verrouillé"));
          else if (ImGui::MenuItem(i18n::Tr("Jeter..."))) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendDrop; pend_open_prompt_ = true;
          }
        if (dropLocked) ImGui::EndDisabled();
        // Transferts (si la fenêtre cible est ouverte). Comme « Vers l'échange... »,
        // une PILE ouvre le prompt de quantité : ces deux entrées envoyaient la pile
        // ENTIÈRE sans rien demander, alors que tous leurs voisins demandent.
        if (viewers::CartOpen() || viewers::StorageOpen() || TradeOpen()) ImGui::Separator();
        if (viewers::CartOpen()) {
          // ⚠ RÈGLE SERVEUR : tant que l'entrepôt est ouvert, le serveur REFUSE tout
          // mouvement inventaire <-> cart — clif_parse_PutItemToCart (CZ 0x0126)
          // passe par pc_cant_act2(), qui inclut state.storage_flag. Le paquet part
          // mais est jeté en silence : on grise l'entrée en le disant.
          const bool vending_lock = viewers::VendingComposing();
          const bool blocked_by_storage = viewers::StorageOpen();
          const bool to_cart_off = blocked_by_storage || vending_lock;
          if (it.amount <= 1) {
            if (ImGui::MenuItem(i18n::Tr("Vers le cart"), nullptr, false, !to_cart_off))
              SendCmd(kCmdToCart, it.index, 1);
          } else if (ImGui::MenuItem(i18n::Tr("Vers le cart..."), nullptr, false,
                                     !to_cart_off)) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendToCart; pend_open_prompt_ = true;
          }
          if (to_cart_off &&
              ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                vending_lock
                    ? i18n::Tr("Impossible pendant la composition d'un shop (règle du\n"
                      "serveur). Ouvrez ou annulez le shop d'abord.") : i18n::Tr("Impossible tant que le storage est ouvert (règle du serveur).\n"
                      "Fermez le storage, ou faites transiter l'objet par le storage."));
        }
        if (viewers::StorageOpen()) {
          // Ici le serveur accepterait : c'est NOUS qui figeons, pour qu'une
          // composition en cours ne voie pas son stock bouger. Le tooltip ne
          // prétend donc pas à une règle serveur.
          const bool vending_lock = viewers::VendingComposing();
          if (it.amount <= 1) {
            if (ImGui::MenuItem(i18n::Tr("Vers le storage"), nullptr, false, !vending_lock))
              SendCmd(kCmdToStorage, it.index, 1);
          } else if (ImGui::MenuItem(i18n::Tr("Vers le storage..."), nullptr, false,
                                     !vending_lock)) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendToStorage; pend_open_prompt_ = true;
          }
          if (vending_lock &&
              ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                i18n::Tr("Figé pendant la composition d'un shop, pour que le stock\n"
                "ne bouge pas sous la fenêtre en cours."));
        }
        // Échange joueur-joueur : stack -> prompt quantité (comme « Jeter... »),
        // sinon ajout direct d'1 unité.
        if (TradeOpen()) {
          if (it.amount <= 1) {
            if (ImGui::MenuItem(i18n::Tr("Vers l'échange")))
              if (auto* tt = Bourgeon::Instance().trade_window())
                tt->AddItemToTrade(it.index, 1);
          } else if (ImGui::MenuItem(i18n::Tr("Vers l'échange..."))) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendToTrade; pend_open_prompt_ = true;
          }
        }
        // Courrier en cours d'écriture : même politique de quantité que l'échange.
        if (MailComposing()) {
          if (it.amount <= 1) {
            if (ImGui::MenuItem(i18n::Tr("Joindre au courrier")))
              if (auto* rodex = Bourgeon::Instance().rodex_window())
                rodex->AttachItem(it.index, 1);
          } else if (ImGui::MenuItem(i18n::Tr("Joindre au courrier..."))) {
            pend_id_ = it.index; pend_index_ = it.index; pend_max_ = it.amount;
            pend_action_ = kPendToMail; pend_open_prompt_ = true;
          }
        }
        ImGui::EndPopup();
      }
      ImGui::PopStyleVar(2);  // WindowPadding + ItemSpacing du menu

      ImGui::PopID();
    }
    ImGui::PopStyleVar();
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();  // WindowPadding (grille)

  // Onglet actif "mange" le bord entre le strip et la grille : petit pont sur son bord
  // droit -> passage continu = souligne l'onglet actif. Couleur = corps de l'onglet
  // actif : BLANC pour la plupart, mais D1DCE8 (gris-bleu) pour Favoris (dont l'image a
  // ce corps) -> le pont se fond au lieu de trancher en blanc.
  // Le pont suit le bord qui touche la grille : bord DROIT en disposition
  // verticale, bord BAS en horizontale (la grille est alors juste dessous).
  if (haveActiveTab) {
    const ImU32 pont = kCats[cur_tab_].fav ? IM_COL32(0xD1, 0xDC, 0xE8, 255)
                                           : IM_COL32(255, 255, 255, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (vtabs)
      dl->AddRectFilled(ImVec2(activeTabMax.x - 1.0f, activeTabMin.y + 1.0f),
                        ImVec2(activeTabMax.x + 2.0f, activeTabMax.y - 1.0f), pont);
    else
      dl->AddRectFilled(ImVec2(activeTabMin.x + 1.0f, activeTabMax.y - 1.0f),
                        ImVec2(activeTabMax.x - 1.0f, activeTabMax.y + 2.0f), pont);
  }

  // ── Drag terminé : router selon la cible (cart / storage / sol) ──
  if (drag_active_) {
    const ImGuiPayload* pl = ImGui::GetDragDropPayload();
    if (pl && pl->IsDataType("INV_ITEM")) {
      const ImVec2 m = ImGui::GetMousePos();
      drag_mx_ = m.x; drag_my_ = m.y;
    } else {
      int action = -1;
      if (drag_index_ > 0) {
        const bool over_self = win_rect_.Contains(drag_mx_, drag_my_);
        // Lâcher DANS l'inventaire : le placement libre et les onglets ont déjà tout
        // traité, il n'y a plus rien à router. Testé EN PREMIER pour qu'une autre
        // fenêtre posée dessous (viewer storage, native cachée) ne puisse jamais
        // capter un rangement interne — c'était le « je déplace une tuile et l'objet
        // part au storage ».
        if (over_self) {
          // rien
        }
        // (Le drop sur la fenêtre Équipement a disparu avec elle : c'est la feuille
        // de personnage qui accepte désormais le payload sur ses slots.)
        // Storage ouvert => le serveur refuse inventaire -> cart (storage_flag, cf.
        // le menu contextuel) : on n'arme RIEN, plutôt que d'ouvrir un prompt de
        // quantité dont la validation partirait à la poubelle.
        else if (viewers::MouseOverCart(drag_mx_, drag_my_)) {
          if (!viewers::StorageOpen()) action = kPendToCart;
        }
        else if (viewers::MouseOverStorage(drag_mx_, drag_my_)) action = kPendToStorage;
        // (over_self est déjà écarté plus haut : ici on est forcément HORS de la
        // fenêtre.) Verrou drop actif -> pas de jet au sol.
        else if (!ImGui::GetIO().WantCaptureMouse && !ReadLock(kDropLockGlobal))
          action = kPendDrop;
      }
      if (action != -1) {
        pend_id_ = drag_index_; pend_index_ = drag_index_;
        pend_max_ = drag_amount_ > 0 ? drag_amount_ : 1;
        pend_action_ = action;
        pend_open_prompt_ = (pend_max_ > 1);
      }
      drag_active_ = false;
    }
  }

  // ── Footer : barre 3-slice (btnbar_left3/mid3/right3.bmp) portant poids / items /
  //    zeny. Épinglée au bas de la fenêtre en coords ABSOLUES (hors flux ImGui), donc
  //    jamais recouverte par la grille (footerH réservé dans BeginChild ci-dessus).
  const FooterVals fv = ReadFooterVals();
  const int maxSlots = fv.expansion + kInvBase;
  const int pct = fv.wmax > 0
                      ? static_cast<int>(static_cast<long long>(fv.wcur) * 100 / fv.wmax)
                      : 0;
  const bool over = fv.wmax > 0 && pct >= fv.overPct;

  const ImVec2 fwp = ImGui::GetWindowPos(), fws = ImGui::GetWindowSize();
  const float fx0 = fwp.x + style.WindowPadding.x;
  const float fx1 = fwp.x + fws.x - style.WindowPadding.x;
  const float fy1 = fwp.y + fws.y - style.WindowPadding.y;
  const float fy0 = fy1 - footerH;
  ImDrawList* fdl = ImGui::GetWindowDrawList();
  ro::grid::DrawFooterBar(fdl, fx0, fy0, fx1, footerH);

  // Deux lignes avec les icônes NATIVES du footer : ligne 1 = icône poids + "cur/max
  // (pct%)" (zeny à droite) ; ligne 2 = icône compteur + "N/max".
  const ImU32 colText = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 colOver = IM_COL32(230, 60, 60, 255);
  const float th = ImGui::GetTextLineHeight();
  const float cy1 = fy0 + footerH * 0.25f;  // centre ligne 1
  const float cy2 = fy0 + footerH * 0.75f;  // centre ligne 2
  // ── Groupe de boutons NATIFS (bmp \inventory\...) sur la LIGNE 2 (celle du compteur),
  //    collé à droite, ordre natif G->D : Drop (verrou drop, TOUS onglets), puis Deal
  //    (verrou vente NPC) + Tri (vue) sur les Favoris. Sur la ligne 2 -> le zeny garde
  //    TOUTE la ligne 1 (plus poussé sur le poids). idx1 = état actif.
  const bool favTab = kCats[cur_tab_].fav;
  // Bouton de TRI masqué quand la taille est verrouillée : c'est le mode où la
  // disposition est censée être maîtrisée par le joueur (placement libre), et un
  // tri viendrait la remettre en cause.
  const bool showSort = favTab && !lock_size_;
  const bool dropOn = ReadLock(kDropLockGlobal);
  const bool dealOn = ReadLock(rag::kFavoriteModeFlagAddr);
  auto bwidth = [](const BarTex& on, const BarTex& off, bool a) -> float {
    const BarTex& t = a ? on : off;
    return (t.tex && t.w > 0) ? static_cast<float>(t.w) : 18.0f;
  };
  auto bheight = [](const BarTex& on, const BarTex& off, bool a) -> float {
    const BarTex& t = a ? on : off;
    return (t.tex && t.h > 0) ? static_cast<float>(t.h) : 18.0f;
  };
  const float bgap = 4.0f, gripM = 16.0f;
  const float wDrop = bwidth(g_btn_drop[1], g_btn_drop[0], dropOn);
  const float wDeal = favTab ? bwidth(g_btn_deal[1], g_btn_deal[0], dealOn) : 0.0f;
  const float wSort = showSort ? bwidth(g_btn_sort[1], g_btn_sort[0], sort_enabled_) : 0.0f;
  const float groupW = wDrop + (favTab ? wDeal + bgap : 0.0f) +
                       (showSort ? wSort + bgap : 0.0f);
  const float grpL = fx1 - gripM - groupW;   // bord gauche du groupe
  // Centré verticalement sur la ligne 2 (cy2), mais borné dans la barre (les bmps natifs
  // ~20px peuvent dépasser une demi-ligne -> on remonte juste assez pour ne pas clipper).
  float maxBtnH = bheight(g_btn_drop[1], g_btn_drop[0], dropOn);
  if (favTab) {
    const float hDeal = bheight(g_btn_deal[1], g_btn_deal[0], dealOn);
    if (hDeal > maxBtnH) maxBtnH = hDeal;
  }
  if (showSort) {
    const float hSort = bheight(g_btn_sort[1], g_btn_sort[0], sort_enabled_);
    if (hSort > maxBtnH) maxBtnH = hSort;
  }
  const float halfH = maxBtnH * 0.5f;
  float cyc = cy2;
  if (cyc + halfH > fy1 - 1.0f) cyc = fy1 - 1.0f - halfH;
  if (cyc - halfH < fy0 + 1.0f) cyc = fy0 + 1.0f + halfH;

  // Ligne 1 : poids (icône + valeur) à gauche, zeny à droite (ligne 1 entièrement libre).
  char wbuf[48];
  std::snprintf(wbuf, sizeof(wbuf), "%d/%d (%d%%)", fv.wcur, fv.wmax, pct);
  float x = fx0 + 6.0f;
  x += ro::grid::DrawFooterIcon(fdl, ro::grid::Assets().weight, x, cy1) + 3.0f;
  fdl->AddText(ImVec2(x, cy1 - th * 0.5f), over ? colOver : colText, wbuf);
  char zbuf[40];
  reinterpret_cast<FmtComma_t>(rag::kFormatThousandsAddr)(fv.zeny, zbuf, sizeof(zbuf));
  char zline[56];
  std::snprintf(zline, sizeof(zline), "%sz", zbuf);
  const float zw = ImGui::CalcTextSize(zline).x;
  const float zx = fx1 - gripM - zw;                                        // plein droite
  // Bouton BANQUE (sac de zeny, styleshop\btn_bank_*) collé à gauche du montant :
  // le zeny déjà affiché devient le point d'entrée de la banque, sans coûter ni une
  // ligne de footer ni une fenêtre de plus. Même effet que Ctrl+B (cf. ToggleFromUi :
  // c'est le SERVEUR qui ouvre la fenêtre, le client se contente de demander).
  if (auto* bank = Bourgeon::Instance().bank_window()) {
    // Demi-taille : l'art fait 19x24, plus haut que la ligne de footer (~21 px) et
    // franchement plus gros que les boutons natifs voisins (~18 px). À 0.5 il
    // redevient une pastille discrète à côté du montant — et le facteur exact ½
    // donne un sous-échantillonnage propre (moyenne de 2x2) au filtrage bilinéaire.
    constexpr float kBankBtnScale = 0.5f;
    const float bankW = (g_btn_bank[0].tex && g_btn_bank[0].w > 0)
                            ? static_cast<float>(g_btn_bank[0].w) * kBankBtnScale
                            : 18.0f * kBankBtnScale;
    if (FooterImgButton3("##inv_bank", zx - bankW - 4.0f, cy1, fy0, fy1, g_btn_bank,
                         "Z", i18n::Tr("Ouvrir la banque de zeny (Ctrl+B)"), nullptr,
                         kBankBtnScale))
      bank->ToggleFromUi();
  }
  fdl->AddText(ImVec2(zx, cy1 - th * 0.5f), colText, zline);
  // Ligne 2 : compteur d'items (icône + N/max).
  char cbuf[32];
  std::snprintf(cbuf, sizeof(cbuf), "%d/%d", item_count_ + WornItemCount(), maxSlots);
  x = fx0 + 6.0f;
  x += ro::grid::DrawFooterIcon(fdl, ro::grid::Assets().num, x, cy2) + 3.0f;
  fdl->AddText(ImVec2(x, cy2 - th * 0.5f), colText, cbuf);

  // Boutons (dessinés par-dessus la barre) : Drop, puis Deal + Tri sur les Favoris.
  float bx = grpL, bwOut = 0.0f;
  if (FooterImgToggle("##inv_droplock", bx, cyc, g_btn_drop[1], g_btn_drop[0], dropOn, "D",
                      i18n::Tr("Verrou drop : empêche de jeter des items (tous onglets)"), &bwOut))
    ToggleLock(kDropLockGlobal);
  bx += bwOut + bgap;
  if (favTab) {
    if (FooterImgToggle("##inv_deallock", bx, cyc, g_btn_deal[1], g_btn_deal[0], dealOn, "V",
                        i18n::Tr("Verrou vente : les favoris ne peuvent pas être vendus aux NPC"), &bwOut))
      ToggleLock(rag::kFavoriteModeFlagAddr);
    bx += bwOut + bgap;
  }
  if (showSort) {
    if (FooterImgToggle("##inv_sort", bx, cyc, g_btn_sort[1], g_btn_sort[0], sort_enabled_, "T",
                        i18n::Tr("Trier la vue (type puis nom) ; sinon ordre du serveur"), &bwOut))
      sort_enabled_ = !sort_enabled_;
  }

  // "(?)" des raccourcis : dans le FOOTER, juste à gauche du groupe de boutons —
  // il ne mange plus une ligne au-dessus de la grille. Positionné en coordonnées
  // écran comme les boutons (le curseur de layout est ailleurs à ce stade).
  {
    const float hw = ImGui::CalcTextSize("(?)").x;
    ImGui::SetCursorScreenPos(ImVec2(grpL - hw - 10.0f, cyc - ImGui::GetTextLineHeight() * 0.5f));
    HelpMarker(desc.c_str());
  }

  // (Le redessin de l'icône d'un glisser NATIF survolant le viewer a disparu avec
  // le reste du pont natif : plus aucune fenêtre native ne peut en émettre.)

  ro::EndRoWindow();

  // ── Aperçu de description au SURVOL : TOOLTIP habillé RO ───────────────────
  // Identique au storage : un vrai tooltip (couche avant, toujours au premier plan
  // et recadré près des bords), cadre sysbox peint à la main derrière le contenu.
  if (show_desc_tooltip_ && hover_desc_id_ != 0) {
    // Cartes/options du stack survolé (la DB ne les connaît pas). Index revalidé :
    // items_ est reconstruit à chaque tick.
    itemdesc::SimpleOpt sopts[5];
    const uint32_t* pcards = nullptr;
    const char* hname = nullptr;
    int ncards = 0, nopts = 0, hrefine = 0;
    bool hdamaged = false;
    if (hover_desc_idx_ >= 0 && hover_desc_idx_ < item_count_) {
      const Item& hit = items_[hover_desc_idx_];
      pcards = hit.cards;
      ncards = 4;
      nopts = hit.opt_count;
      hrefine = hit.refine;
      hdamaged = hit.damaged != 0;
      hname = hit.name;  // nom décoré (BuildDisplayName) : préfixes/suffixes de cartes
      for (int k = 0; k < nopts && k < 5; ++k) {
        sopts[k].index = hit.opts[k].index;
        sopts[k].value = hit.opts[k].value;
        sopts[k].param = hit.opts[k].param;
      }
    }
    itemcell::DrawTooltip(hover_desc_id_, pcards, ncards, sopts, nopts, hrefine, hname,
                          hdamaged);
  }
}
