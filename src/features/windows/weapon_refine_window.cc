#include "features/windows/weapon_refine_window.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bourgeon.h"
#include "features/item_cell.h"
#include "features/moonlight_ui/moonlight_ui.h"  // HelpMarker
#include "features/windows/item_desc_window.h"   // RenderSimpleDesc, FocusDescWindow
#include "imgui.h"
#include "ragnarok/globals.h"
#include "ragnarok/uiwnd.h"
#include "ui/icon_cache.h"
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000) ────────────────────────────
// Tout est établi et justifié dans docs/weapon_refine_re.md ; on ne recopie ici
// que ce qui sert, avec le § qui l'explique.
namespace {

// Fenêtre native UIWeaponRefineWnd (§3). On la retrouve par le GESTIONNAIRE puis
// on vérifie la vtable : un id ne garantit pas la classe si un portage
// renumérote les fenêtres.
constexpr int       kWinRefine    = 111;         // 0x6F
constexpr uintptr_t kRefineVTable = 0x0103ee00;

// CMode::SendMsg : le dispatcher du mode actif, vtable+0x18. La commande 182
// envoie CZ_REQ_WEAPONREFINE (§4). On rejoue ce chemin natif plutôt que de
// fabriquer le paquet — c'est la règle du projet, et ici elle évite en prime de
// dupliquer la construction d'en-tête.
constexpr int kVfDispCmd  = 0x18;
constexpr int kCmdRefine  = 182;   // { index } -> CZ_REQ_WEAPONREFINE 0x0222
constexpr int kCmdUseSkill = 0x45;  // { skillId, cibleGID, niveau } -> lancer un skill
using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);

// Notre propre GID = notre AID : WS_WEAPONREFINE se lance sur soi.
constexpr uintptr_t kOwnAccountId = 0x015fb9a4;

// Le skill lui-même (db/pre-re/skill_db.yml du fork moonlight).
constexpr int kSkillWeaponRefine = 477;  // WS_WEAPONREFINE, MaxLevel 10

// Modèle SESSION de l'inventaire : la std::list que le client tient à jour, quel
// que soit l'état de ses fenêtres. Même source que InventoryViewer.
constexpr uintptr_t kInvListHead = 0x015fbab0;
constexpr int kNodeNext  = 0x00;  // nœud std::list : next
constexpr int kNodeInfo  = 0x08;  // nœud : value = ItemSkillInfo
constexpr int kNodeAmt   = 0x18;  // nœud : quantité
constexpr int kInfoIdStr = 0x2c;  // std::string de l'id (le jeu fait atoi dessus)
constexpr int kInfoIdCap = 0x40;  // capacité SSO de cette std::string (+0x2c+0x14)
constexpr int kMaxInvNodes = 4096;  // garde-fou de parcours

// Minerais de refine, par niveau d'arme (serveur : skill_weaponrefine, §7).
// Ce n'est PAS une table de gameplay inventée ici : c'est la copie exacte du
// tableau `material[]` du serveur, et la seule chose qu'on en fait est
// d'AFFICHER un stock — aucune décision n'en dépend, le serveur reste seul juge.
constexpr uint32_t kOrePhracon      = 1010;
constexpr uint32_t kOreEmveretarcon = 1011;
constexpr uint32_t kOreOridecon     = 984;

// ── Relance automatique de la compétence : ce qu'elle est, et ce qu'elle n'est pas
//
// Le projet refuse l'automatisation non supervisée, et c'est une règle de fond
// ([[project_plugin_architecture]] : l'API Python a été retirée exprès pour ça).
// La relance implémentée ici s'y tient parce qu'elle n'automatise PAS le refine :
// elle rejoue le lancement de la COMPÉTENCE, c'est-à-dire la seule chose que le
// serveur oblige à refaire entre deux tentatives (clif_menuskill_clear). La liste
// revient, et c'est toujours le joueur qui choisit l'arme et qui déclenche.
// Autrement dit : ça supprime un aller-retour vers la barre de skills, pas une
// décision. Aucune tentative ne part sans un clic.
//
// Trois bornes, parce qu'une boucle qui se relance elle-même doit toujours
// pouvoir s'arrêter seule :
// Pas de plafond de relances : il n'y a rien à borner. Une relance ne suit qu'un
// refine, et un refine ne part que sur un clic — la chaîne avance au rythme du
// joueur, pas du client. Un compteur n'aurait protégé de rien et aurait coupé
// une session légitime au 21e refine.
constexpr unsigned kAutoRecastDelayMs = 450;  // laisse passer le délai de cast

// Intervalle minimal entre deux demandes de refine, tous gestes confondus. Une
// touche maintenue répète à la cadence du clavier — bien plus vite qu'un
// aller-retour serveur.
constexpr unsigned kMinSendIntervalMs = 300;

// MsgStringTable : on affiche les libellés EXACTS du client, jamais une
// paraphrase (règle du projet). CP949 -> ro::Cp949ToUtf8 au moment du rendu.
constexpr uintptr_t kMsgStringGet = 0x00a9ed30;
using MsgStringGet_t = const char*(__cdecl*)(int);
constexpr int kMsgCantMakeItem   = 424;  // MSI_CANT_MAKE_ITEM (texte de la modale)
constexpr int kMsgRefineSuccess  = 911;  // MSI_ITEM_REFINE_SUCCEESS
constexpr int kMsgRefineFail     = 912;  // MSI_ITEM_REFINE_FAIL
constexpr int kMsgFailLevel      = 913;  // MSI_ITEM_REFINE_FAIL_LEVEL
constexpr int kMsgFailMaterial   = 914;  // MSI_ITEM_REFINE_FAIL_MATERIAL

// L'arbre de compétences (CPlayerSkillBundle) : on y lit le niveau APPRIS de
// WS_WEAPONREFINE, qui EST le plafond de refine côté serveur
// (`item->refine >= sd.menuskill_val` refuse). Même source que le Grimoire de la
// feuille de personnage — cf. docs/skill_tree_re.md partie II.
constexpr uintptr_t kSkillBundle     = 0x015fa3cc;
constexpr uintptr_t kSkillFlatList   = 0x015fa3e0;  // bundle+0x14 : onglet « divers »
constexpr uintptr_t kSkillGetTabList = 0x00738370;  // __thiscall(bundle, tab) -> std::list*
using GetTabList_t = void* (__fastcall*)(void*, void*, int);
constexpr int kSkNodeValue  = 0x08;
constexpr int kSkOffValid   = 0x04;
constexpr int kSkOffId      = 0x08;
constexpr int kSkOffLvLocal = 0x10;
constexpr int kSkOffLearned = 0x30;  // int16, VÉRITÉ SERVEUR
constexpr int kSkillJobTabs = 4;
constexpr int kSkillMaxNodes = 256;

// Opcodes observés (le handler natif continue de tourner : on ne fait que lire).
constexpr uint16_t kOpRefineList = 0x0221;  // ZC_NOTIFY_WEAPONITEMLIST (VARIABLE)
constexpr uint16_t kOpRefineAck  = 0x0223;  // ZC_ACK_WEAPONREFINE (fixe, 10)
// ZC_ACK_TOUSESKILL — le refus « générique » de compétence. Le serveur l'envoie à
// la place de 0x0223 sur plusieurs sorties de skill_weaponrefine (arme non
// affinable, entrée de refine.yml manquante, coût introuvable, échange en cours) :
// sans l'observer, une tentative refusée laissait la fenêtre « en attente du
// serveur… » jusqu'au délai de garde, pour une réponse déjà arrivée.
//   +0 u16 skillId | +2 i32 btype | +6 u32 itemId | +10 u8 flag | +11 u8 cause
// (offsets APRÈS l'opcode ; total 14 sur le fil, donc 12 transmis).
constexpr uint16_t kOpSkillFail  = 0x0110;
constexpr uint16_t kSkillFailLen = 12;
// Octets transmis à OnRecvPacket pour 0x0223 : la longueur du paquet MOINS son
// opcode, que RegisterObserveOpcode a déjà consommé. Le paquet fait 10 en tout
// (u16 op + u32 result + u32 itemId) : il en reste 8.
constexpr uint16_t kRefineAckLen = 8;
constexpr int kRefineEntrySize = 23;        // §2 : 2 + 4 + 1 + 4*4
constexpr int kMaxRefineEntries = 512;      // garde-fou (un inventaire fait 400 max)

inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

// ── Palette de la fenêtre ────────────────────────────────────────────────────
// Le corps d'une fenêtre RO est CLAIR. Les teintes vives ou pâles s'y délavent :
// le vert de succès du journal, hérité d'un thème sombre, en ressortait
// « éclatant » et à peine lisible. Tout ce qui est coloré ici est donc SATURÉ et
// SOMBRE.
//
// Et rassemblé : le pied de fenêtre et le journal avaient chacun leur jeu de
// couleurs pour dire les mêmes trois choses (ça va / ça a échoué / attention),
// si bien qu'un ajustement n'en corrigeait qu'un sur deux.
constexpr ImU32 kColOk      = IM_COL32( 13, 107,  31, 255);  // succès, stock présent
constexpr ImU32 kColBad     = IM_COL32(166,  38,  38, 255);  // échec, stock à zéro
constexpr ImU32 kColWarn    = IM_COL32(166, 102,   0, 255);  // refus, attente, plafond
constexpr ImU32 kColInfo    = IM_COL32( 30,  90, 175, 255);  // refine courant, relance
constexpr ImU32 kColNeutral = IM_COL32( 60,  60,  60, 255);  // journal sans statut

inline ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// Côté de l'icône d'item : c'est LUI qui fixe la hauteur d'une ligne (le texte
// est plus bas). Et le nombre de lignes VISIBLES — au-delà, la table défile.
// Quatre suffisent : la fenêtre reste compacte au lieu de réserver un grand vide
// pour les inventaires à deux armes.
// 24 px = la taille NATIVE des icônes d'item du client. Toute autre valeur les
// rééchantillonne, et le filtre est imposé à POINT dans tout le projet (les
// textures du jeu sont du pixel-art) : une icône mise à l'échelle y devient
// franchement sale.
constexpr float kIconSize    = 24.0f;
constexpr int   kVisibleRows = 4;

// Icône des liens de minerai, dans le pied de fenêtre. Plus petite que celle des
// lignes d'arme : c'est une note de bas de page, pas la liste elle-même — et
// trois icônes pleine taille y feraient un bandeau.
constexpr float kOreIcon = 16.0f;
// Curseur « main » du client (index vérifié en jeu, cf. project_ro_cursor) :
// ImGui::SetMouseCursor est un no-op ici, io.ConfigFlags portant
// NoMouseCursorChange — il FAUT passer par ro::SetHoverCursor.
constexpr int kRoCursorHand = 2;

// Centre verticalement le texte d'une cellule sur la hauteur de ligne, imposée
// par l'icône. ImGui aligne en HAUT par défaut : sans ça les colonnes courtes
// (« +7 », « 2/3 ») flottent au-dessus du nom, lui centré par
// ImGuiStyleVar_SelectableTextAlign.
void AlignCellTextMiddle() {
  const float slack = kIconSize - ImGui::GetTextLineHeight();
  if (slack > 0.0f)
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + slack * 0.5f);
}

// Largeurs des boutons du pied de fenêtre. SOURCE UNIQUE : la LARGEUR de la
// fenêtre s'en déduit (cf. OnRenderUI ; la hauteur, elle, suit le contenu), donc
// les changer ici suffit — rien à réaccorder à la main.
// Largeur des deux boutons de la modale de confirmation, et largeur de repli de
// son texte. Cette dernière n'est pas décorative : elle FIXE la largeur de la
// modale, donc sa taille, donc sa position (cf. le pavé dans OnRenderUI).
constexpr float kBtnConfirmW  = 110.0f;
constexpr float kConfirmWrapW = 300.0f;

constexpr float kBtnRefineW = 100.0f;
constexpr float kBtnRecastW = 150.0f;
constexpr float kBtnCloseW  = 90.0f;

// Nombre de CARTES réellement serties.
//
// Le paquet livre les 4 emplacements bruts, mais tous ne portent pas des cartes :
//   - card[0] à 254/255/256 (CARD0_CREATE / CARD0_FORGE / CARD0_PET) signale que
//     les quatre entrées décrivent une forge ou une création, pas des cartes ;
//   - les ENCHANTEMENTS occupent les emplacements du HAUT (les scripts serveur
//     les posent en partant de card[3]) et ne consomment pas un slot de carte.
// D'où le comptage BORNÉ au nombre d'emplacements réels de l'item — sans quoi
// une arme 2 slots portant 1 carte et 3 enchantements s'affiche « 4/2 ».
//
// C'est le mieux qu'on puisse faire côté client sans deviner le type de chaque
// id : la borne ne peut plus surestimer le total, seulement, au pire, compter un
// enchantement logé sous la borne.
int CountRealCards(const uint32_t card[4], int slots) {
  if (card[0] == 254 || card[0] == 255 || card[0] == 256) return 0;
  const int bound = slots < 0 ? 0 : (slots > 4 ? 4 : slots);
  int cards = 0;
  for (int i = 0; i < bound; ++i)
    if (card[i]) ++cards;
  return cards;
}

// Description complète de l'item (fenêtre native 0x0c), depuis l'ItemSkillInfo
// VIVANT de l'inventaire : cartes, refine et enchantements compris.
void OpenItemDesc(int inventory_index, int mx, int my) {
  itemcell::OpenDescFromInfo(itemcell::FindInfoByIndex(kInvListHead, inventory_index),
                             mx, my);
  // Sinon le panneau s'ouvre DERRIÈRE notre fenêtre, qui a le focus : il porte
  // NoFocusOnAppearing, donc ImGui ne le remonte pas de lui-même. Ici c'est un
  // clic délibéré, on le demande explicitement (cf. itemdesc::FocusDescWindow).
  itemdesc::FocusDescWindow();
}

const char* MsgString(int id) {
  __try {
    const char* s = reinterpret_cast<MsgStringGet_t>(kMsgStringGet)(id);
    return s ? s : "";
  } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; }
}

// ── Escamotage ONE-SHOT de la modale native « liste vide » ───────────────────
// UIWndMgr_ShowMessageBoxModal a ~250 sites d'appel dans le client : un détour
// qui se tromperait de cible casserait tout, du login aux boutiques. D'où DEUX
// verrous cumulés, et un désarmement inconditionnel :
//
//   1. le drapeau n'est armé QUE dans OnRecvPacket(0x0221) quand la liste est
//      vide — et le handler natif appelle la modale synchroniquement juste
//      après, sur le même thread, sans rien exécuter entre les deux ;
//   2. on exige en plus que le TEXTE reçu soit exactement le pointeur que
//      MsgStringTable_GetById(424) vient de rendre (la table rend un pointeur
//      stable par id) : même armé, un autre message passe.
//
// Le drapeau est remis à faux à CHAQUE appel, armé ou non : il ne peut donc
// jamais survivre à la fenêtre d'une seule réception. Et le détour reste
// transparent tant que rien ne l'arme — c'est-à-dire toujours, si le joueur
// garde l'interface native.
constexpr uintptr_t kShowMessageBoxModal = 0x00a31a30;
using ShowModal_t = int(__fastcall*)(void*, void*, const char*, int, int*, int,
                                     int, const char*, int, int, int*);
ShowModal_t g_orig_show_modal = nullptr;
bool        g_swallow_next_modal = false;
const char* g_swallow_text = nullptr;

// 185 est ce que le natif renvoie quand il n'affiche RIEN (mode inhibé, ou une
// modale déjà à l'écran) : c'est donc le « rien ne s'est passé » que ses
// appelants savent déjà encaisser.
constexpr int kModalNotShown = 185;

int __fastcall ShowMessageBoxModalHook(void* self, void* edx, const char* text,
                                       int p2, int* p3, int p4, int p5,
                                       const char* title, int w, int h, int* p9) {
  const bool swallow = g_swallow_next_modal && text && text == g_swallow_text;
  g_swallow_next_modal = false;
  g_swallow_text = nullptr;
  if (swallow) return kModalNotShown;
  return g_orig_show_modal(self, edx, text, p2, p3, p4, p5, title, w, h, p9);
}

// La fenêtre native de refine, ou nullptr. Le client DÉTRUIT ses fenêtres à
// la fermeture : non-nul == « ouverte en ce moment ».
uint8_t* RefineWnd() {
  __try {
    auto* w = reinterpret_cast<uint8_t*>(uiwnd::FindWindow(kWinRefine));
    if (!w) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(w) != kRefineVTable) return nullptr;
    return w;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Position écran de la fenêtre native.
//
// ⚠ Fonction SÉPARÉE, et ce n'est pas du zèle : C2712 (« cannot use __try in
// functions that require object unwinding ») se juge sur la FONCTION ENTIÈRE,
// pas sur le bloc. OnRenderUI parcourt des std::vector — dont les itérateurs ont
// un destructeur non trivial dès que _ITERATOR_DEBUG_LEVEL > 0 — donc y laisser
// un __try casserait la compilation à la première build non-Release.
bool ReadWndPos(uint8_t* wnd, int* x, int* y) {
  __try {
    *x = uiwnd::PosX(wnd);
    *y = uiwnd::PosY(wnd);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Notre AID (= notre GID), lu sous SEH. Même raison de vivre à part.
uint32_t OwnAid() {
  __try {
    return *reinterpret_cast<const uint32_t*>(kOwnAccountId);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Envoie une commande au dispatcher du mode actif (le chemin des boutons natifs).
void SendModeCmd(int cmd, int a, int b = 0, int c = 0, int d = 0) {
  __try {
    void* mode = *reinterpret_cast<void**>(rag::kActiveModePtr);
    if (!mode) return;
    auto fn = reinterpret_cast<DispCmd_t>(
        (*reinterpret_cast<uintptr_t**>(mode))[kVfDispCmd / 4]);
    fn(mode, cmd, a, b, c, d);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Libellé COMPLET d'une arme (préfixes de cartes, refine, forge).
//
// ⚠ Le `this` de BuildDisplayName n'est pas décoratif, et ce n'est PAS une
// fenêtre : c'est le GESTIONNAIRE. Le paramètre ne sert qu'à un seul usage dans
// toute la fonction — `UIWindowMgr_FindOrQueueNameRequest(this)` dans les branches « objet forgé dont le
// nom de créateur est introuvable » — et le seul appel DIRECT de UIWindowMgr_FindOrQueueNameRequest dans
// le client (UIMerchantItemShopWnd_DrawContent @0x00948041) charge explicitement
// `mov ecx, offset 0x131F4E8`, c'est-à-dire g_UIWindowMgr. Vérifié en mémoire :
// mgr+0x18C porte bien une std::list {sentinelle, taille}.
//
// Ça règle deux choses d'un coup :
//   - la fenêtre de refine ne pouvait PAS servir de contexte (l'objet
//     UIWeaponRefineWnd fait 0xD0 octets, +0x18C est hors de ses bornes) ;
//   - la fenêtre inventaire, elle, marchait — mais seulement quand elle était
//     OUVERTE, d'où des noms qui changeaient sous les yeux du joueur selon qu'il
//     avait son inventaire à l'écran ou non.
// Le gestionnaire, lui, est un objet statique : toujours là, jamais nul.
void SafeName(void* info, char* out, size_t out_size) {
  if (!out || out_size == 0) return;
  out[0] = '\0';
  if (!info) return;
  itemcell::BuildDisplayName(uiwnd::Mgr(), info, out, out_size);
}

// Le nom rendu par le name-builder natif est DÉCORÉ : il porte déjà « +N » en
// tête dès que l'arme porte un refine (c'est ce qui fait le titre de la fenêtre
// de description, et c'est pour ça qu'on le garde tel quel pour le survol).
// Mais la liste, elle, a une COLONNE dédiée au refine — celle qui affiche
// l'information que le natif jette. Garder le préfixe y écrirait deux fois la
// même chose : « +2   +2 Triple Explosive Twin Edge… ».
//
// On ne saute donc le préfixe QUE s'il correspond exactement au refine de
// l'entrée. Un objet dont le nom commencerait vraiment par « + », ou un préfixe
// qui ne collerait pas au paquet, ressort intact — mieux vaut un doublon
// qu'un nom tronqué.
const char* SkipRefinePrefix(const char* name, int refine) {
  if (!name || refine <= 0 || name[0] != '+') return name;
  const char* digits = name + 1;
  const char* p = digits;
  int parsed = 0;
  while (*p >= '0' && *p <= '9') {
    parsed = parsed * 10 + (*p - '0');
    ++p;
  }
  if (p == digits || parsed != refine) return name;
  while (*p == ' ') ++p;
  return *p ? p : name;
}

// Lit l'id d'un ItemSkillInfo (std::string SSO à +0x2c).
uint32_t InfoId(const uint8_t* info) {
  __try {
    const uint32_t cap = *reinterpret_cast<const uint32_t*>(info + kInfoIdCap);
    const char* s = (cap >= 16)
                        ? *reinterpret_cast<const char* const*>(info + kInfoIdStr)
                        : reinterpret_cast<const char*>(info + kInfoIdStr);
    if (!s) return 0;
    return static_cast<uint32_t>(std::strtoul(s, nullptr, 10));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

}  // namespace

WeaponRefineWindow::WeaponRefineWindow() {
  // OBSERVATION, pas remplacement : le handler natif continue de tourner, sinon
  // désactiver le plugin laisserait le skill sans aucune fenêtre.
  //
  // ⚠ 0x0221 est un paquet à longueur VARIABLE et RegisterObserveOpcode ne sait
  // transmettre qu'un nombre FIXE d'octets. On n'en demande donc que 2 : le
  // champ `packetLength` lui-même, qui est la vraie borne. `data` pointe dans le
  // tampon de réception juste après l'opcode, et le paquet entier y est déjà —
  // lire jusqu'à `packetLength` reste dans les octets que le client vient de
  // recevoir. Le `len` du callback est donc IGNORÉ pour cet opcode, et c'est
  // délibéré (cf. le commentaire du parseur dans OnRecvPacket).
  Bourgeon::Instance().RegisterObserveOpcode(kOpRefineList, 2);
  Bourgeon::Instance().RegisterObserveOpcode(kOpRefineAck, kRefineAckLen);
  Bourgeon::Instance().RegisterObserveOpcode(kOpSkillFail, kSkillFailLen);

  // Détour de la modale (voir le pavé au-dessus de ShowMessageBoxModalHook).
  g_orig_show_modal = reinterpret_cast<ShowModal_t>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kShowMessageBoxModal),
          reinterpret_cast<uint8_t*>(&ShowMessageBoxModalHook)));
}

// ── Capture ──────────────────────────────────────────────────────────────────

void WeaponRefineWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                      uint16_t len) {
  if (opcode == kOpRefineList) {
    // `data` = le paquet À PARTIR de son champ longueur (l'opcode est déjà
    // consommé). La longueur ANNONCÉE fait foi, pas `len` (cf. le ctor).
    uint16_t total = 0;
    __try {
      total = *reinterpret_cast<const uint16_t*>(data);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (total < 4) return;

    entries_.clear();
    const int count = (total - 4) / kRefineEntrySize;
    const uint8_t* p = data + 2;  // début de la charge utile
    __try {
      for (int i = 0; i < count && i < kMaxRefineEntries; ++i) {
        Entry e;
        e.index  = *reinterpret_cast<const int16_t*>(p);      // +0 : déjà i+2
        e.nameid = *reinterpret_cast<const uint32_t*>(p + 2); // +2 : 4 octets
        e.refine = *(p + 6);                                  // +6 : jeté par le natif
        for (int c = 0; c < 4; ++c)                           // +7 : jeté aussi
          e.card[c] = *reinterpret_cast<const uint32_t*>(p + 7 + c * 4);
        entries_.push_back(e);
        p += kRefineEntrySize;
      }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    empty_list_      = entries_.empty();
    awaiting_result_ = false;
    consumed_        = false;
    // La liste est revenue : la relance armée a fait son office (ou le serveur a
    // devancé le délai). Une liste VIDE termine naturellement la chaîne — c'est
    // le vrai « tant qu'il reste des armes », dit par le serveur lui-même plutôt
    // que deviné côté client.
    auto_recast_at_ = 0;
    if (empty_list_ && auto_chain_ > 0)
      auto_stop_reason_ = "Plus aucune arme à refine : relance arrêtée.";
    // ── Que devient la sélection quand une nouvelle liste arrive ? ────────────
    //
    // Elle est RECONDUITE si l'arme visée est encore là : un refine RÉUSSI ne
    // déplace pas l'objet en inventaire, son index revient à l'identique, et
    // c'est presque toujours la même arme qu'on veut continuer à monter.
    //
    // 🔴 Si l'index NE RESSORT PLUS, on ne re-sélectionne rien. Pas de repli sur
    // la première entrée : ce serait déplacer silencieusement la cible d'une
    // action destructrice, et le geste suivant (Entrée, ou un clic sur un bouton
    // qu'on croit encore armé sur l'arme d'avant) jouerait une AUTRE arme. Le
    // bouton « Refine » reste grisé tant que le joueur n'a pas re-désigné une
    // ligne lui-même. C'est exactement le cas d'un échec : l'arme vient d'être
    // détruite, plus rien ne doit être armé.
    //
    // Seule exception, la toute première liste d'une session : il n'y avait
    // aucune arme visée, donc rien à perdre — la première ligne prend la main.
    const int prev_sel = sel_index_;
    sel_index_ = -1;
    if (prev_sel < 0) {
      if (!entries_.empty()) sel_index_ = entries_.front().index;
    } else {
      for (const Entry& e : entries_)
        if (e.index == prev_sel) { sel_index_ = prev_sel; break; }
    }
    need_focus_      = true;
    // C'est CE paquet qui ouvre notre fenêtre, pas la création de la native :
    // elle nous survit ensuite à chaque tentative (cf. ui_open_).
    ui_open_         = true;

    // Liste vide : le natif s'apprête à afficher sa modale recyclée. On l'arme
    // pour l'escamoter — mais seulement si on prend la main sur cette fenêtre.
    if (empty_list_ && imgui_enabled_) {
      g_swallow_text       = MsgString(kMsgCantMakeItem);
      g_swallow_next_modal = (g_swallow_text != nullptr);
    }
    return;
  }

  if (opcode == kOpRefineAck) {
    if (len < kRefineAckLen) return;
    int      result = 0;
    uint32_t nameid = 0;
    __try {
      // ⚠ `data` commence APRÈS l'opcode (contrat de RegisterObserveOpcode), donc
      // il est décalé de 2 par rapport aux offsets du §5 de la doc :
      //   paquet +2 `result` -> data +0 ;  paquet +6 `itemId` -> data +4.
      // Écrit un temps avec les offsets du PAQUET, ce qui lisait à cheval sur les
      // deux champs : `result` valait alors l'itemId décalé de 16 bits, donc
      // jamais 0..3 — le switch tombait dans son `default`, et le journal de
      // session restait vide sans que rien ne le signale.
      result = *reinterpret_cast<const int32_t*>(data);
      nameid = *reinterpret_cast<const uint32_t*>(data + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // Ce résultat répond-il à une tentative de NOUS ? C'est la seule causalité
    // qui autorise une relance automatique — cf. ScheduleAutoRecast.
    const bool was_ours = awaiting_result_;
    awaiting_result_ = false;
    LogServerResult(result, nameid);
    if (was_ours) ScheduleAutoRecast(result);
    return;
  }

  // Refus générique : le serveur a répondu, mais par ZC_ACK_TOUSESKILL. On ne
  // réagit que si on attendait VRAIMENT un résultat de refine et que c'est bien
  // notre compétence — 0x0110 sert à tous les skills du jeu.
  if (opcode == kOpSkillFail) {
    if (!awaiting_result_ || len < 2) return;
    uint16_t skill_id = 0;
    __try {
      skill_id = *reinterpret_cast<const uint16_t*>(data);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (skill_id != kSkillWeaponRefine) return;
    awaiting_result_ = false;
    PushLog("Tentative refusée par le serveur (aucun minerai consommé).",
            kColWarn);
    // Chaîne coupée : un refus de condition ne se règle pas en relançant.
    auto_recast_at_   = 0;
    auto_stop_reason_ = "Le serveur a refusé la tentative : relance arrêtée.";
  }
}

// Arme (ou refuse) une relance automatique après un résultat de tentative.
// N'ENVOIE RIEN : on est dans un handler de paquet, l'envoi passe par
// FlushPending comme tout le reste.
//
// ⚠ Appelée UNIQUEMENT sur un 0x0223 qui répond à une tentative de nous. C'est
// ce lien de causalité qui borne la chaîne, et non un compteur : une relance ne
// peut suivre qu'un refine, et un refine ne part que sur un geste du joueur.
// Chaque tour coûte donc un clic, un minerai et un cast — le rythme est celui du
// joueur, pas celui du client, et rien ne peut s'emballer.
void WeaponRefineWindow::ScheduleAutoRecast(int result) {
  auto_recast_at_ = 0;
  if (!auto_recast_ || !imgui_enabled_ || !ui_open_) return;

  // result 2 (« niveau de compétence insuffisant ») et 3 (« minerai manquant »)
  // ne sont pas des tentatives : ce sont des refus de condition, et cette
  // condition ne changera pas d'elle-même. Relancer là-dessus tournerait en rond
  // en brûlant du SP à chaque tour.
  if (result != 0 && result != 1) {
    auto_stop_reason_ = "Le serveur a refusé la condition : relance arrêtée.";
    return;
  }
  // Plus un seul minerai des trois : aucune arme ne peut plus entrer dans la
  // liste (clif_item_refine_list exige celui du niveau de l'arme). Inutile de
  // payer un cast pour se le faire dire.
  if (OreCount(kOrePhracon) == 0 && OreCount(kOreEmveretarcon) == 0 &&
      OreCount(kOreOridecon) == 0) {
    auto_stop_reason_ = "Plus aucun minerai : relance arrêtée.";
    return;
  }
  auto_stop_reason_ = nullptr;
  ++auto_chain_;
  auto_recast_at_ = GetTickCount() + kAutoRecastDelayMs;
  if (auto_recast_at_ == 0) auto_recast_at_ = 1;  // 0 = « rien d'armé »
}

void WeaponRefineWindow::LogServerResult(int result, uint32_t nameid) {
  // Le client compose « <libellé serveur> » avec %s = le nom de l'objet. On fait
  // pareil, en gardant SON libellé (911..914) — jamais une paraphrase à nous.
  int msg_id = 0;
  uint32_t color = kColNeutral;
  const char* prefix = "";
  switch (result) {
    case 0:
      msg_id = kMsgRefineSuccess;
      color  = kColOk;
      prefix = "Succès — ";
      break;
    case 1:
      // Le seul cas destructeur, et celui que le client rend indiscernable du
      // succès (911 == 912 dans msgstringtable.csv). D'où le préfixe explicite.
      msg_id = kMsgRefineFail;
      color  = kColBad;
      prefix = "ÉCHEC — arme détruite — ";
      break;
    case 2:
      msg_id = kMsgFailLevel;
      color  = kColWarn;
      break;
    case 3:
      msg_id = kMsgFailMaterial;
      color  = kColWarn;
      break;
    default:
      return;
  }

  // ── De QUEL objet parle ce résultat ? ──────────────────────────────────────
  // ⚠ Le `nameid` du paquet ne désigne pas la même chose selon le résultat :
  // pour 0/1/2 c'est l'ARME (`clif_upgrademessage(&sd, r, item->nameid)`), mais
  // pour 3 c'est le MINERAI manquant (`… , material[weapon_level - 1]`) — cf.
  // skill.cpp. Les confondre faisait dire « Knife is required to upgrade this
  // weapon » au lieu de nommer l'Oridecon.
  //
  // Et pour l'arme, l'id ne SUFFIT pas : quatre Knife en inventaire partagent le
  // même. C'est l'index envoyé qui identifie celle qu'on a jouée.
  char name[128] = {0};
  if (result == 3) {
    // Le minerai manque, donc il n'est pas en inventaire : la DB client est la
    // seule source possible pour son nom.
    if (const MoonlightUi* mu = Bourgeon::Instance().moonlight_ui())
      if (const char* db = mu->ItemName(nameid))
        std::snprintf(name, sizeof(name), "%s", db);
  } else {
    // L'arme, par son INDEX : l'état est à jour (refine incrémenté en cas de
    // réussite). Détruite par un échec, elle n'y est plus — on retombe alors sur
    // le nom capturé juste avant l'envoi.
    SafeName(itemcell::FindInfoByIndex(kInvListHead, sent_index_), name,
             sizeof(name));
    if (!name[0] && sent_name_[0])
      std::snprintf(name, sizeof(name), "%s", sent_name_);
  }

  // Le libellé du client est un FORMAT (« Refined weapon: %s », « %s is required
  // to upgrade this weapon. ») : son %s doit recevoir le nom de l'objet.
  //
  // ⚠ On ne le passe SURTOUT pas à printf comme chaîne de format : elle vient
  // d'un fichier de données (data/msgstringtable.csv), donc un %d ou un %n mal
  // placé y ferait lire la pile. On substitue à la main, littéralement, la
  // PREMIÈRE occurrence de « %s » — et on ignore les suivantes.
  char subject[160];
  if (name[0]) std::snprintf(subject, sizeof(subject), "%s", name);
  else         std::snprintf(subject, sizeof(subject), "id %u", nameid);

  const char* tmpl = ro::Cp949ToUtf8(MsgString(msg_id));
  char body[288];
  const char* slot = std::strstr(tmpl, "%s");
  if (slot) {
    const int head = static_cast<int>(slot - tmpl);
    std::snprintf(body, sizeof(body), "%.*s%s%s", head, tmpl, subject,
                  slot + 2);
  } else {
    // Libellé sans marqueur (traduction remaniée) : on annexe le sujet plutôt
    // que de le perdre.
    std::snprintf(body, sizeof(body), "%s [%s]", tmpl, subject);
  }

  char line[320];
  std::snprintf(line, sizeof(line), "%s%s", prefix, body);
  PushLog(line, color);
}

void WeaponRefineWindow::PushLog(const char* text, uint32_t color) {
  LogLine l;
  l.text  = text ? text : "";
  l.color = color;
  // Heure LOCALE via l'API Win32 : pas de dépendance à la locale du CRT, et la
  // même que celle affichée par le chat du client.
  SYSTEMTIME now;
  GetLocalTime(&now);
  std::snprintf(l.time, sizeof(l.time), "%02u:%02u:%02u", now.wHour, now.wMinute,
                now.wSecond);
  history_.push_back(std::move(l));
  // Journal de SESSION : borné, il n'a pas vocation à croître sans fin.
  if (history_.size() > 200) history_.erase(history_.begin());
}

// ── Cycle de vie ─────────────────────────────────────────────────────────────

void WeaponRefineWindow::ResetSession() {
  entries_.clear();
  ui_open_         = false;
  consumed_        = false;
  hover_valid_     = false;
  sel_index_       = -1;
  awaiting_result_ = false;
  empty_list_      = false;
  confirm_index_   = -1;
  confirm_open_    = false;
  filter_[0]       = 0;
  // Une chaîne de relances ne survit JAMAIS à la fermeture de la session : elle
  // se réarme à la main, jamais toute seule.
  auto_recast_at_   = 0;
  auto_chain_       = 0;
  auto_stop_reason_ = nullptr;
}

void WeaponRefineWindow::OnModeSwitch(ModeMgr::ModeType mode_type,
                                      const char* /*map_name*/) {
  // Sortir du monde de jeu efface le menuskill serveur : garder la fenêtre
  // ouverte laisserait croire qu'une session est encore armée. L'historique, lui,
  // n'a plus de sens hors session.
  if (mode_type != ModeMgr::ModeType::kGame) {
    // Écrire AVANT de refermer : quitter le monde de jeu est aussi une fin de
    // session, et une position déplacée puis jamais « fermée » se perdrait.
    FlushWindowPos();
    ResetSession();
    history_.clear();
  }
}

void WeaponRefineWindow::HideNativeAtCreation(void* win) {
  if (!imgui_enabled_ || !win) return;
  __try {
    // La fenêtre n'est pas encore enregistrée au gestionnaire à cet instant : la
    // vtable est le seul contrôle de classe possible (même patron que BankWindow).
    if (*reinterpret_cast<uintptr_t*>(win) != kRefineVTable) return;
    uiwnd::SetVisible(win, false);
    native_hidden_ = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void WeaponRefineWindow::OnTick() {
  // Bascule du toggle : on rend sa visibilité à la native une seule fois, sinon
  // on se battrait chaque tick avec le « tout masquer » natif (behavior 116).
  if (prev_enabled_ != imgui_enabled_) {
    prev_enabled_ = imgui_enabled_;
    if (!imgui_enabled_ && native_hidden_) {
      if (uint8_t* w = RefineWnd()) uiwnd::SetVisible(w, true);
      native_hidden_ = false;
    }
    ResetSession();
  }

  uint8_t* wnd = RefineWnd();
  open_ = (wnd != nullptr);

  if (!imgui_enabled_) return;

  if (wnd) {
    // Re-masquage idempotent : la fenêtre peut avoir été rendue visible par un
    // chemin natif (behavior « tout afficher »), et le hook MakeWindow ne joue
    // qu'à la création.
    if (uiwnd::IsVisible(wnd)) {
      uiwnd::SetVisible(wnd, false);
      native_hidden_ = true;
    }
  } else if (was_open_) {
    // La native a disparu — mais surtout PAS notre fenêtre. Le client la détruit
    // dès la tentative envoyée, et c'est précisément là que le joueur veut voir
    // le résultat et enchaîner (cf. le commentaire de ui_open_ dans l'en-tête).
    // On invalide donc la LISTE (elle n'existe plus côté serveur) sans toucher à
    // ui_open_.
    entries_.clear();
    consumed_      = true;
    confirm_index_ = -1;
    confirm_open_  = false;
    // 🔴 `sel_index_` est GARDÉ, et c'est tout l'enjeu de l'enchaînement.
    //
    // Il est effacé ici pendant un temps, ce qui cassait silencieusement la
    // reconduction de la sélection : la native meurt dès la tentative envoyée,
    // donc bien AVANT que la nouvelle liste n'arrive — quand le 0x0221 revenait,
    // il ne trouvait plus aucune arme visée et retombait sur la première entrée.
    // Le joueur qui montait la 4e arme se retrouvait pointé sur la 1re.
    //
    // Le garder ne réarme rien : `sel_visible_` est recalculé sur les lignes
    // rendues, et sans liste il n'y a pas de ligne — donc bouton grisé et Entrée
    // inerte jusqu'à la liste suivante.
  }

  // Une tentative sans réponse au bout de 10 s : le serveur ne répondra plus
  // (menuskill effacé, déconnexion…). On débloque l'UI plutôt que de la laisser
  // grisée pour toujours.
  if (awaiting_result_ && awaiting_since_ &&
      GetTickCount() - awaiting_since_ > 10000) {
    awaiting_result_ = false;
  }

  // Échéance de relance automatique. On ne fait que POSER l'action : c'est
  // FlushPending, appelé depuis OnProcessInput, qui la joue hors frame ImGui —
  // le lancement d'un skill peut ouvrir une modale native, et le faire entre
  // NewFrame() et Render() gèlerait le client.
  // Comparaison par SOUSTRACTION, pas par « >= » : GetTickCount reboucle au bout
  // de 49 jours, et un client resté ouvert si longtemps verrait sinon l'échéance
  // ne jamais tomber. C'est déjà la forme retenue pour awaiting_since_ au-dessus.
  if (auto_recast_at_ &&
      static_cast<int>(GetTickCount() - auto_recast_at_) >= 0) {
    auto_recast_at_ = 0;
    // Le joueur reste prioritaire : fenêtre fermée, option coupée ou autre action
    // déjà posée entre-temps, la relance est simplement abandonnée. Et si une
    // liste est déjà revenue (le serveur peut devancer le délai), il n'y a rien
    // à relancer.
    if (auto_recast_ && ui_open_ && entries_.empty() && pending_ == kActNone)
      pending_ = kActRecast;
  }

  was_open_ = open_;
}

// ── Actions différées (hors frame ImGui) ─────────────────────────────────────

void WeaponRefineWindow::FlushPending() {
  const PendingAction act = pending_;
  const int idx = pending_index_;
  pending_ = kActNone;
  if (act == kActNone) return;

  switch (act) {
    case kActRefine:
      // Identité de l'arme visée, capturée AVANT l'envoi : c'est le dernier
      // instant où elle existe à coup sûr (un échec la détruit), et le paquet de
      // résultat, lui, ne portera qu'un itemId — insuffisant pour désigner LA
      // bonne parmi plusieurs exemplaires du même objet.
      sent_index_ = idx;
      SafeName(itemcell::FindInfoByIndex(kInvListHead, idx), sent_name_,
               sizeof(sent_name_));
      // Le chemin EXACT du bouton OK natif : cmd 182 avec l'index reçu du
      // serveur, tel quel, puis fermeture de la fenêtre (le natif enchaîne les
      // deux dans son OnMsg case 6).
      SendModeCmd(kCmdRefine, idx);
      uiwnd::CloseWindow(kWinRefine);
      awaiting_result_ = true;
      awaiting_since_  = GetTickCount();
      // 🔴 La liste est MORTE, ici et maintenant. `clif_parse_WeaponRefine`
      // termine par `clif_menuskill_clear` : la tentative suivante trouvera
      // `menuskill_id != WS_WEAPONREFINE` et sera jetée **sans le moindre
      // paquet de réponse**. Garder les entrées à l'écran laissait donc envoyer
      // dans le vide, indéfiniment — c'est ce qui donnait « plus aucun refine ne
      // fonctionne » après avoir spammé Entrée.
      //
      // On ne s'en remettait qu'à OnTick, qui vide la liste en CONSTATANT la
      // disparition de la fenêtre native : une observation indirecte, qui rate
      // sa cible dès qu'un nouveau 0x0221 recrée la fenêtre avant le tick
      // suivant. Le vider ICI est déterministe et dit la même chose que le
      // serveur.
      entries_.clear();
      consumed_ = true;
      break;

    case kActCancel:
      // ⚠ Annuler DOIT envoyer : c'est ce -1 qui fait appeler
      // clif_menuskill_clear côté serveur. Sans lui le personnage reste avec un
      // menuskill armé (cf. l'en-tête, piège n°2).
      //
      // …mais SEULEMENT si une session est encore armée, c'est-à-dire si la
      // fenêtre native vit toujours. Après un refine, le serveur a déjà fait
      // son clif_menuskill_clear et le client a détruit la 111 : un -1 de plus
      // serait un paquet inutile, et notre fenêtre reste ouverte bien après ce
      // moment-là (c'est tout l'intérêt de ui_open_).
      if (RefineWnd()) {
        SendModeCmd(kCmdRefine, -1);
        uiwnd::CloseWindow(kWinRefine);
      }
      break;

    case kActRecast: {
      // Relance du skill par le chemin natif (barre de cast, contrôles SP et
      // cooldown côté client compris) — un CZ_USE_SKILL fabriqué à la main les
      // sauterait tous.
      const int level = std::max(1, RefineSkillLevel());
      const uint32_t self = OwnAid();
      if (self)
        SendModeCmd(kCmdUseSkill, kSkillWeaponRefine, static_cast<int>(self),
                    level);
      break;
    }
    default:
      break;
  }
}

// ── Lectures du monde ────────────────────────────────────────────────────────

int WeaponRefineWindow::OreCount(uint32_t nameid) const {
  int total = 0;
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    int guard = 0;
    while (node && node != head && guard++ < kMaxInvNodes) {
      const uint8_t* info = node + kNodeInfo;
      const int amount = *reinterpret_cast<const int*>(node + kNodeAmt);
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      if (InfoId(info) == nameid && amount > 0) total += amount;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return total;
}

int WeaponRefineWindow::RefineSkillLevel() {
  int found = 0;
  __try {
    // Les cinq listes du bundle : quatre onglets de job + la liste plate.
    for (int tab = -1; tab < kSkillJobTabs && !found; ++tab) {
      uint8_t* list_obj = reinterpret_cast<uint8_t*>(kSkillFlatList);
      if (tab >= 0)
        list_obj = reinterpret_cast<uint8_t*>(reinterpret_cast<GetTabList_t>(
            kSkillGetTabList)(reinterpret_cast<void*>(kSkillBundle), nullptr, tab));
      if (!list_obj) continue;
      uint8_t* head = *reinterpret_cast<uint8_t**>(list_obj);
      if (!head) continue;
      uint8_t* node = *reinterpret_cast<uint8_t**>(head);
      int guard = 0;
      while (node && node != head && guard++ < kSkillMaxNodes) {
        const uint8_t* v = node + kSkNodeValue;
        node = *reinterpret_cast<uint8_t**>(node);  // avancer AVANT de lire
        if (*reinterpret_cast<const int*>(v + kSkOffValid) == 0) continue;
        if (*reinterpret_cast<const int*>(v + kSkOffId) != kSkillWeaponRefine)
          continue;
        // +0x30 (int16) fait foi — c'est la vérité serveur ; +0x10 sert de repli.
        int lv = *reinterpret_cast<const int16_t*>(v + kSkOffLearned);
        if (lv <= 0) lv = *reinterpret_cast<const int*>(v + kSkOffLvLocal);
        found = lv;
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return found;
}

// ── Rendu ────────────────────────────────────────────────────────────────────

// Réponse au hook de WndProc. Sans état de rendu : la déduire d'un drapeau posé
// à la frame précédente la rendrait FAUSSE dès que le rendu s'arrête sans que la
// fenêtre se ferme — pendant un chargement de carte, typiquement — et la touche
// resterait avalée pour un client qui n'affiche plus rien.
bool WeaponRefineWindow::WantsEnterKey() const {
  return imgui_enabled_ && ui_open_ &&
         !Bourgeon::Instance().IsMapLoading() &&
         Bourgeon::Instance().IsGameActive();
}

void WeaponRefineWindow::OnRenderUI() {
  if (!imgui_enabled_) return;
  // ⚠ On suit ui_open_, PAS la présence de la fenêtre native : celle-ci est
  // détruite dès la tentative envoyée (cf. le commentaire de ui_open_ dans
  // l'en-tête). S'y calquer ferait disparaître la fenêtre pile au moment où le
  // résultat arrive.
  if (!ui_open_) return;

  hover_valid_ = false;
  // Faux par DÉFAUT, relevé par DrawList quand la sélection est bel et bien
  // rendue. Ce sens-là compte : si la liste n'est pas dessinée du tout (session
  // consommée, liste vide), rien ne doit pouvoir déclencher un refine.
  sel_visible_ = false;

  // ── Où s'ouvre la fenêtre ──────────────────────────────────────────────────
  // La position que le joueur lui a donnée d'abord, persistée d'une session à
  // l'autre (`refine_pos_x/y`). Elle n'est demandée qu'à l'apparition : après,
  // la fenêtre est à lui.
  //
  // À défaut seulement — première utilisation — on se cale là où la native se
  // serait affichée, tant qu'elle est encore là pour le dire. C'était l'unique
  // comportement, et il rejouait à CHAQUE ouverture : la fenêtre retournait se
  // poser sur la native, effaçant le déplacement du joueur à chaque lancement du
  // skill.
  if (pos_x_ != INT_MIN && pos_y_ != INT_MIN) {
    ImGui::SetNextWindowPos(
        ImVec2(static_cast<float>(pos_x_), static_cast<float>(pos_y_)),
        ImGuiCond_Appearing);
  } else if (uint8_t* wnd = RefineWnd()) {
    int px = 0, py = 0;
    if (ReadWndPos(wnd, &px, &py))
      ImGui::SetNextWindowPos(ImVec2(static_cast<float>(px),
                                     static_cast<float>(py)),
                              ImGuiCond_Appearing);
  }
  // Taille FIGÉE, calée sur la rangée de boutons du pied : c'est la seule
  // largeur utile, et un redimensionnement n'apporterait rien sur une liste
  // d'une poignée d'armes. La largeur se déduit des trois boutons (kBtn*W), donc
  // elle suit automatiquement si on les retouche.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float window_w = style.WindowPadding.x * 2.0f + kBtnRefineW +
                         kBtnRecastW + kBtnCloseW + style.ItemSpacing.x * 2.0f;
  // La HAUTEUR se déduit du contenu (un 0 sur un axe veut dire « ajuste-toi »
  // pour ImGui — c'est ce que fait déjà la banque). La liste ayant une hauteur
  // bornée à kVisibleRows lignes, la fenêtre ne peut plus ni bâiller sur un
  // message de trois lignes, ni s'étirer sur un gros inventaire. Bonus : c'est ce
  // qui rend au pied de fenêtre sa marge basse, que la hauteur fixe mangeait.
  ImGui::SetNextWindowSize(ImVec2(window_w, 0.0f), ImGuiCond_Always);
  if (need_focus_) {
    ImGui::SetNextWindowFocus();
    need_focus_ = false;
  }

  ro::SetNextWindowTitleBullet("Options du refine");
  bool open = true;
  // NoScrollbar/NoScrollWithMouse : le contenu tient toujours (la liste a son
  // propre enfant scrollable), la barre de la fenêtre était parasite.
  const bool begun = ro::BeginRoWindow(
      "Refine Weapon###bourgeon_weapon_refine", &open,
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceRefine);
  if (begun) {
    // Suivi de la position pour la persistance. On marque simplement « à
    // écrire » : sauvegarder ici réécrirait le yaml à chaque frame d'un
    // glissement. L'écriture a lieu à la fermeture (FlushWindowPos).
    const ImVec2 wp = ImGui::GetWindowPos();
    const int px = static_cast<int>(wp.x + 0.5f);
    const int py = static_cast<int>(wp.y + 0.5f);
    if (px != pos_x_ || py != pos_y_) {
      pos_x_ = px;
      pos_y_ = py;
      pos_dirty_ = true;
    }

    if (empty_list_) {
      // Ce que la modale native AURAIT dû dire. Le libellé natif (« You can't
      // create items yet. ») ne parle ni d'arme, ni de refine, ni de minerai :
      // on énumère les vraies causes, celles du filtre serveur
      // (clif_item_refine_list, cf. docs/weapon_refine_re.md §7).
      ImGui::TextColored(V4(kColWarn), "Aucune arme refinable.");
      ImGui::Spacing();
      ImGui::TextWrapped("Le serveur ne propose une arme que si TOUT est vrai :");
      BulletWrapped("elle est identifiée et a un niveau d'arme (1 à 4) ;");
      BulletWrapped("elle n'est PAS portée (déséquipe-la d'abord) ;");
      BulletWrapped("son refine est encore sous le plafond de ta compétence ;");
      BulletWrapped("tu as le minerai correspondant à son niveau d'arme.");
      ImGui::Spacing();
      DrawFooter();
    } else if (entries_.empty()) {
      // Tentative partie : la liste n'existe plus côté serveur (menuskill
      // effacé) et le client a détruit sa fenêtre. C'est l'écran que le natif
      // n'a pas du tout — il se contente de tout refermer.
      if (!awaiting_result_ && consumed_) {
        ImGui::TextWrapped(
            "Session terminée : le serveur n'autorise qu'un refine par "
            "lancement de la compétence.");
      }
      ImGui::Spacing();
      DrawFooter();
    } else {
      // Hauteur de la table = son en-tête + kVisibleRows lignes, calculée depuis
      // le style courant (le skin RO retouche CellPadding) plutôt que devinée :
      // une constante en dur laisserait une demi-ligne dépasser au premier
      // changement de thème.
      const float row_h  = kIconSize + style.CellPadding.y * 2.0f;
      const float head_h = ImGui::GetTextLineHeight() + style.CellPadding.y * 2.0f;
      DrawList(head_h + row_h * kVisibleRows);
      DrawFooter();
    }

    // ── Journal : APRÈS le pied, et dans TOUS les cas ──────────────────────────
    // 🔴 Il était dessiné dans deux branches sur trois, et la manquante était la
    // pire : « aucune arme refinable », c'est-à-dire justement la fin de partie,
    // quand le joueur veut relire ce qui vient de se passer. Pire encore avec un
    // ÉCHEC sur la dernière arme — la ligne « arme détruite » était bien écrite
    // dans le journal, mais la relance ramenait une liste vide, donc cette
    // branche, donc pas de journal : le seul message qui comptait ne s'affichait
    // jamais.
    //
    // Le sortir des branches supprime la classe de bug entière, au lieu d'ajouter
    // un troisième appel qu'une quatrième branche oublierait à nouveau.
    if (show_history_ && !history_.empty())
      DrawHistory(ImGui::GetTextLineHeightWithSpacing() * 5.0f);

    // ── Entrée = jouer l'arme sélectionnée ────────────────────────────────────
    // Armé seulement si le clic sur « Refine » l'aurait été aussi : aucune
    // tentative en vol, aucune confirmation déjà ouverte, et surtout une
    // sélection VISIBLE à l'écran.
    //
    // 🔴 Ce dernier point a été un vrai défaut : la sélection était cherchée dans
    // `entries_`, donc dans TOUTE la liste du serveur. Avec un filtre actif,
    // Entrée jouait une arme masquée — celle qu'une nouvelle liste avait
    // sélectionnée par défaut — sans que rien ne soit surligné à l'écran. Un
    // échec détruisait alors une arme que le joueur n'avait jamais vue.
    //
    // Et hors saisie : tant que le champ de filtre a le focus, Entrée lui
    // appartient — elle le referme, l'action attend la frappe suivante.
    //
    // ⚠ Ce test conditionne l'ACTION, pas la confiscation de la touche au jeu :
    // celle-là ne regarde que l'ouverture de la fenêtre (cf. WantsEnterKey). Les
    // avoir confondus faisait clignoter le chat entre deux refines enchaînés.
    const bool can_refine =
        sel_visible_ && !ImGui::IsAnyItemActive() &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (can_refine && (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))) {
      // Le reste des conditions (tentative en vol, action déjà posée, cadence)
      // vit dans RequestRefine — un seul endroit pour les trois gestes.
      RequestRefine(sel_index_);
    }
  }
  ro::EndRoWindow();

  // Aperçu de description au survol : APRÈS EndRoWindow, jamais dedans. Il crée
  // son propre popup, et le peindre à l'intérieur d'une fenêtre l'y enfermerait
  // (contrainte documentée d'itemcell::DrawTooltip).
  DrawHoverTooltip();

  // ── Confirmation ────────────────────────────────────────────────────────────
  // ImGui gère la modalité et le voile tout seul. Rien de natif n'est appelé ici
  // — la seule modale native du chemin de refine est celle qu'on escamote, et
  // elle vient du réseau, pas d'un clic.
  if (confirm_open_) {
    ImGui::OpenPopup("Confirmer le refine###bourgeon_refine_confirm");
    confirm_open_ = false;
    // Frame d'ouverture, retenue pour que la MÊME frappe d'Entrée ne traverse pas
    // la confirmation (cf. plus bas) : la modale s'ouvre et se dessine dans la
    // frame courante, où IsKeyPressed(Enter) est encore vrai.
    confirm_frame_ = ImGui::GetFrameCount();
  }

  if (ro::BeginRoPopupModal("Confirmer le refine###bourgeon_refine_confirm")) {
    // ── 🔴 LA cause de la dérive : TextWrapped dans une fenêtre auto-dimensionnée
    //
    // La modale est en `AlwaysAutoResize` (défaut de BeginRoPopupModal) et son
    // texte était rendu par `ImGui::TextWrapped`, qui se replie sur la largeur de
    // la région de contenu — laquelle dépend de la largeur de la fenêtre, qui
    // dépend du contenu. La boucle ne converge pas : la TAILLE change à chaque
    // frame, et tout centrage qui s'appuie dessus fait remonter la fenêtre
    // frame après frame. C'est exactement le symptôme observé — elle apparaît en
    // bas puis grimpe jusqu'en haut — et ça explique pourquoi aucun travail sur
    // la POSITION n'y changeait rien : le problème n'était pas là.
    //
    // Une largeur de repli EXPLICITE ferme la boucle. C'est ce que fait déjà la
    // seule autre modale de texte du projet (features/systems/dx7_warning.cc),
    // et c'est la recommandation d'ImGui pour toute fenêtre auto-dimensionnée.
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + kConfirmWrapW);

    // ── Centrage ───────────────────────────────────────────────────────────────
    // `ImGui::SetWindowPos` depuis l'INTÉRIEUR du popup, pas `SetNextWindowPos` :
    // ce dernier (et donc ro::SetNextRoModalPos) s'applique sous
    // `ImGuiCond_Appearing`, et un popup a des chemins de placement AUTOMATIQUE —
    // recentrage modal, FindBestWindowPosForPopup — qui reprennent la main dès que
    // la condition ne mord pas.
    //
    // ⚠ Et il faut la taille RÉELLE, que la frame d'apparition n'a pas : la trace
    // a montré (16,33) sur celle-ci contre (300,133) sur la suivante — d'où un
    // coin haut-gauche posé pile au centre au lieu de la fenêtre elle-même.
    // `Appearing` étant FAUX dès la frame suivante (vérifié, contrairement à ce
    // que je supposais), il n'y a pas de « seconde chance » à saisir.
    //
    // On mémorise donc la taille stabilisée pour l'ouverture SUIVANTE. Corriger
    // la position à la frame 2 serait pire : ImGui y a déjà émis le fond de la
    // fenêtre, et le déplacer après coup le fait « baver » le temps d'une frame
    // (c'est écrit tel quel dans le code d'ImGui). La frame d'apparition, elle,
    // n'est pas dessinée du tout — on peut y placer sans rien salir.
    if (ImGui::IsWindowAppearing()) {
      const ImVec2 size =
          confirm_size_.x > 0.0f ? confirm_size_ : ImGui::GetWindowSize();
      const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
      ImGui::SetWindowPos(
          ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f));
    } else {
      // Auto-dimensionnement stabilisé : c'est LA taille. Elle ne varie plus
      // qu'avec le nombre de lignes du nom d'arme.
      confirm_size_ = ImGui::GetWindowSize();
    }

    const Entry* target = nullptr;
    for (const Entry& e : entries_)
      if (e.index == confirm_index_) { target = &e; break; }

    if (!target) {
      ImGui::CloseCurrentPopup();
      confirm_index_ = -1;
    } else {
      char name[128] = {0};
      void* info = itemcell::FindInfoByIndex(kInvListHead, target->index);
      if (info) SafeName(info, name, sizeof(name));
      // WRAPPÉ, pas TextUnformatted : un nom décoré (« +9 Double Explosive
      // Superbia String [2] ») élargirait la modale à sa seule mesure, et la
      // largeur ne serait plus celle qu'on a fixée.
      ImGui::TextWrapped("%s", name[0] ? name : "(arme inconnue)");
      ImGui::Spacing();
      ImGui::TextColored(V4(kColBad), "Un échec DÉTRUIT l'arme.");
      ImGui::TextWrapped(
          "Le minerai est consommé dans tous les cas. En cas de réussite "
          "l'arme passe de +%d à +%d.",
          target->refine, target->refine + 1);
      ImGui::Spacing();

      // Entrée valide la confirmation, comme dans la liste — mais JAMAIS dans la
      // frame qui vient de l'ouvrir : la modale est dessinée dans cette même
      // frame, où la frappe est encore « pressed ». Sans ce verrou, Entrée
      // depuis la liste traverserait la confirmation d'un seul geste, ce qui la
      // viderait entièrement de son sens sur une action destructrice.
      const bool enter_ok = ImGui::GetFrameCount() > confirm_frame_ &&
                            (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
      // Rien à armer pour le jeu ici : la modale n'existe que par-dessus notre
      // fenêtre, et celle-ci confisque déjà Entrée du seul fait d'être ouverte.
      if (ro::RoButton("Refine", kBtnConfirmW) || enter_ok) {
        pending_       = kActRefine;
        pending_index_ = confirm_index_;
        confirm_index_ = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ro::RoButton("Annuler", kBtnConfirmW)) {
        confirm_index_ = -1;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::PopTextWrapPos();
    ro::EndRoPopupModal();
  }

  // Clic sur le X de NOTRE fenêtre = annuler, comme le bouton natif.
  if (!open) RequestClose();
}

void WeaponRefineWindow::RequestRefine(int inventory_index) {
  if (inventory_index < 0) return;
  // Une action déjà posée, ou une tentative en vol : on ne superpose pas. Le
  // serveur n'accepte qu'UN refine par lancement de compétence, tout envoi
  // supplémentaire est jeté en silence.
  if (awaiting_result_ || pending_ != kActNone || confirm_index_ >= 0) return;
  // Garde anti-rafale, désormais sur TOUS les chemins : une touche maintenue
  // répète à la cadence du clavier, bien plus vite qu'un aller-retour serveur.
  const unsigned now = GetTickCount();
  if (now - last_send_tick_ <= kMinSendIntervalMs) return;
  last_send_tick_ = now;

  if (confirm_) {
    confirm_index_ = inventory_index;
    confirm_open_  = true;
  } else {
    pending_       = kActRefine;
    pending_index_ = inventory_index;
  }
}

void WeaponRefineWindow::FlushWindowPos() {
  if (!pos_dirty_) return;
  pos_dirty_ = false;
  // MoonlightUi possède le fichier de réglages ; la table kRefineSettings y
  // range déjà refine_pos_x/y. On ne fait que demander l'écriture.
  if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
}

void WeaponRefineWindow::RequestClose() {
  // L'envoi du -1 (et la destruction de la native) est décidé dans FlushPending,
  // qui sait si une session est encore armée. Ici on ne fait que refermer NOTRE
  // fenêtre — hors de toute frame ImGui pour le reste.
  pending_    = kActCancel;
  ui_open_    = false;
  sel_index_  = -1;
  confirm_index_ = -1;
  confirm_open_  = false;
  // La fenêtre se referme : c'est le moment d'écrire sa position, une fois.
  FlushWindowPos();
}

void WeaponRefineWindow::DrawHoverTooltip() {
  if (!hover_valid_ || !desc_tooltip_) return;
  itemcell::DrawTooltip(hover_id_, hover_cards_, hover_card_count_, nullptr, 0,
                        hover_refine_, hover_name_[0] ? hover_name_ : nullptr);
}

void WeaponRefineWindow::DrawList(float list_h) {
  // Filtre : ce que la UIListBox native ne sait pas faire. Optionnel — sur deux
  // ou trois armes il ne sert à rien et mange une ligne de la fenêtre.
  if (show_filter_) {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##refine_filter", "Filtrer…", filter_,
                             sizeof(filter_));
  } else if (filter_[0]) {
    // Masquer le champ ne doit pas laisser un filtre invisible cacher des armes.
    filter_[0] = '\0';
  }

  // Vue triée/filtrée : on ne touche JAMAIS à entries_, dont l'ordre est celui
  // du serveur et sert de référence.
  struct Row {
    const WeaponRefineWindow::Entry* e;
    char name[128];
    int  slots;
  };
  std::vector<Row> rows;
  rows.reserve(entries_.size());

  char filter_lc[64];
  std::snprintf(filter_lc, sizeof(filter_lc), "%s", filter_);
  for (char* c = filter_lc; *c; ++c)
    *c = static_cast<char>(::tolower(static_cast<unsigned char>(*c)));

  for (const Entry& e : entries_) {
    Row r;
    r.e = &e;
    r.name[0] = 0;
    r.slots = 0;
    void* info = itemcell::FindInfoByIndex(kInvListHead, e.index);
    if (info) {
      // Nom COMPOSÉ par le name-builder natif : refine, préfixes de cartes,
      // forge. C'est ce que le natif affiche… sauf qu'il ne le compose pas ici,
      // il se contente du nom de base reconstruit depuis l'id.
      SafeName(info, r.name, sizeof(r.name));
      r.slots = itemcell::SlotCount(info);
    }
    if (!r.name[0])
      std::snprintf(r.name, sizeof(r.name), "id %u", e.nameid);

    if (filter_lc[0]) {
      char lower[128];
      std::snprintf(lower, sizeof(lower), "%s", r.name);
      for (char* c = lower; *c; ++c)
        *c = static_cast<char>(::tolower(static_cast<unsigned char>(*c)));
      if (!std::strstr(lower, filter_lc)) continue;
    }
    rows.push_back(r);
  }

  const int cap = RefineSkillLevel();

  // ── La liste, en TABLE triable ───────────────────────────────────────────────
  // Le tri était un combo à trois entrées figées, sans sens de tri. Un en-tête de
  // table dit la même chose en montrant les colonnes disponibles, donne le
  // croissant/décroissant d'un second clic, et n'occupe pas une ligne de plus.
  //
  // SortTristate : un troisième clic retire le tri, et l'ordre affiché redevient
  // celui du paquet — c'est-à-dire l'ordre d'inventaire, que le combo devait
  // proposer comme une entrée à part. C'est aussi l'état de départ.
  //
  // L'icône a sa PROPRE colonne (non triable) : ça évite d'avoir à repositionner
  // le curseur sous un Selectable pour peindre par-dessus. Le Selectable de la
  // ligne, lui, est posé dans la colonne du nom avec SpanAllColumns — ImGui
  // bascule alors son fond dans le canal d'arrière-plan de la table, donc le
  // surlignage passe DERRIÈRE l'icône et les autres colonnes au lieu de les
  // recouvrir.
  // Index des colonnes, dérivés d'un compteur : « Slots » est optionnel, écrire
  // 0/1/2/3 en dur donnerait un tri sur la mauvaise colonne dès qu'on la masque.
  int col = 0;
  const int kColIcon = col++;
  const int kColName = col++;
  const int kColRef  = col++;
  const int kColSlot = show_cards_ ? col++ : -1;
  // Ces deux-là ne sont pas testés (l'icône ne trie pas, le nom est le cas par
  // défaut) : nommés quand même pour que la numérotation se lise d'un bloc.
  (void)kColIcon;
  (void)kColName;

  const ImGuiTableFlags table_flags =
      ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate |
      ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
      ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("refine_items", col, table_flags,
                        ImVec2(0.0f, list_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);  // l'en-tête reste visible au défilement
    ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed |
                                          ImGuiTableColumnFlags_NoSort |
                                          ImGuiTableColumnFlags_NoHeaderLabel,
                            kIconSize + 2.0f);
    ImGui::TableSetupColumn("Arme", ImGuiTableColumnFlags_WidthStretch);
    // Le refine et les emplacements se lisent « du plus au moins » : premier clic
    // décroissant, c'est ce qu'on cherche (quelle arme est la plus montée).
    ImGui::TableSetupColumn("+", ImGuiTableColumnFlags_WidthFixed |
                                     ImGuiTableColumnFlags_PreferSortDescending,
                            26.0f);
    if (show_cards_)
      ImGui::TableSetupColumn("Slots",
                              ImGuiTableColumnFlags_WidthFixed |
                                  ImGuiTableColumnFlags_PreferSortDescending,
                              38.0f);
    ImGui::TableHeadersRow();

    // Le tri VIENT de changer ? On ramène alors la sélection sur la première
    // ligne, et le défilement avec.
    //
    // Ce n'est pas du confort : après un tri, l'arme sélectionnée est ailleurs
    // dans la liste, souvent hors écran — et Entrée, elle, joue toujours la
    // sélection. Garder une sélection invisible reviendrait à laisser un geste
    // destructeur pointer sur une arme que le joueur ne voit plus. La première
    // ligne, elle, est toujours sous les yeux et vient d'être désignée par le tri
    // qu'il a demandé.
    //
    // `SpecsDirty` couvre AUSSI le troisième clic (retour à l'ordre du paquet),
    // qui ne trie rien mais réordonne bel et bien l'affichage. On le remet à
    // faux : c'est à l'appelant de le faire, sinon on rejouerait ça chaque frame.
    bool sort_changed = false;
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
      if (sort->SpecsDirty) {
        sort_changed = true;
        sort->SpecsDirty = false;
      }
      if (sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
          int cmp;
          if (sp.ColumnIndex == kColRef) {
            cmp = a.e->refine - b.e->refine;
          } else if (kColSlot >= 0 && sp.ColumnIndex == kColSlot) {
            // À nombre d'emplacements égal, ce sont les cartes qui départagent.
            cmp = a.slots - b.slots;
            if (cmp == 0)
              cmp = CountRealCards(a.e->card, a.slots) -
                    CountRealCards(b.e->card, b.slots);
          } else {
            // Tri sur le nom AFFICHÉ, préfixe de refine ôté : sur le nom décoré,
            // tout ce qui porte un refine se regrouperait en tête (« + » précède
            // toute lettre) — ce n'est pas un ordre alphabétique, et ça double la
            // colonne « + ».
            cmp = _stricmp(SkipRefinePrefix(a.name, a.e->refine),
                           SkipRefinePrefix(b.name, b.e->refine));
          }
          // Départage STABLE : sans ça, deux armes identiques changent de place
          // d'une frame à l'autre (std::sort n'est pas stable) et la ligne
          // survolée saute sous le curseur.
          if (cmp == 0) cmp = a.e->index - b.e->index;
          return asc ? cmp < 0 : cmp > 0;
        });
      }
    }

    const ImVec4 col_ref = V4(kColInfo);
    const ImVec4 col_cap = V4(kColWarn);

    // Le nom est centré dans la hauteur de ligne, sinon il flotte en haut d'une
    // ligne dont l'icône fixe la hauteur.
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    // 🔴 Rien ne peut déclencher un refine sur une ligne qu'on ne VOIT pas.
    //
    // `entries_` porte toute la liste du serveur ; le filtre n'en montre qu'une
    // partie. Quand la sélection tombe dans la part masquée, l'écran n'a AUCUNE
    // ligne surlignée — et Entrée jouait pourtant cette arme fantôme, qu'un échec
    // aurait détruite sans que le joueur ait jamais vu ce qu'il visait.
    //
    // Le verrou est ce drapeau, pas un repli automatique : sélection invisible =
    // « Refine » grisé et Entrée inerte, jusqu'à ce que le joueur désigne une
    // ligne. Déplacer la cible à sa place serait précisément ce qu'il ne faut pas
    // faire sur une action destructrice.
    sel_visible_ = false;
    for (const Row& r : rows)
      if (r.e->index == sel_index_) { sel_visible_ = true; break; }

    // SEULE exception : un changement de tri, qui est un geste EXPLICITE du
    // joueur sur la table. Il réordonne ce qu'il a sous les yeux, la première
    // ligne reprend la main — et elle est, elle, bien visible. Testé ICI, après
    // le filtre ET le tri, pour que « première ligne » désigne la première ligne
    // RENDUE.
    if (sort_changed && !rows.empty()) {
      sel_index_   = rows.front().e->index;
      sel_visible_ = true;
      // …et le défilement la suit : une sélection au-dessus de la zone visible ne
      // vaut rien. Viser la ligne 0, c'est remonter la table en haut — et avec
      // ScrollY, la fenêtre courante EST la zone défilante de la table, donc
      // SetScrollY s'y applique directement (pas besoin d'un SetScrollHereY, qui
      // dépendrait de la position du curseur dans la cellule).
      ImGui::SetScrollY(0.0f);
    }
    for (const Row& r : rows) {
      const Entry& e = *r.e;
      ImGui::PushID(e.index);
      ImGui::TableNextRow();

      // ── Icône ──
      ImGui::TableNextColumn();
      const ro::IconTex ic = ro::ItemIcon(e.nameid, 1);
      if (ic.tex)
        ImGui::Image(TexId(ic.tex), ImVec2(kIconSize, kIconSize));
      else
        ImGui::Dummy(ImVec2(kIconSize, kIconSize));  // garde la hauteur de ligne

      // ── Nom : c'est LUI qui porte le Selectable de toute la ligne ──
      ImGui::TableNextColumn();
      // Nom SANS son « +N » de tête : la colonne « + » le porte déjà.
      // r.name reste décoré, lui — c'est ce qu'attendent l'aperçu au survol (dont
      // le titre doit être celui de la fenêtre de description) et le filtre.
      const bool dbl =
          ImGui::Selectable(SkipRefinePrefix(r.name, e.refine),
                            sel_index_ == e.index,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0.0f, kIconSize)) &&
          ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
      // Gauche = SÉLECTIONNER, droit = CONSULTER. C'est la convention du client
      // partout ailleurs (inventaire, chariot, storage, équipement) : ouvrir la
      // description sur le clic gauche volait le geste de sélection.
      // `sel_visible_` est relevé ICI aussi : il a été calculé AVANT la boucle,
      // donc sans ça un clic laisserait le bouton « Refine » gris pendant une
      // frame — un clignotement pour rien.
      if (ImGui::IsItemClicked()) {
        sel_index_   = e.index;
        sel_visible_ = true;
      }
      if (IsLastItemRightClicked()) {
        sel_index_   = e.index;
        sel_visible_ = true;
        // Description complète : fenêtre native 0x0c, enrichie par
        // item_desc_window — c'est elle qui dit ce qu'on risque à jouer l'arme.
        POINT pt;
        if (GetCursorPos(&pt)) OpenItemDesc(e.index, pt.x, pt.y);
      }
      if (ImGui::IsItemHovered()) {
        // On MÉMORISE, on ne peint pas : l'aperçu crée son propre popup et doit
        // sortir hors de toute fenêtre ImGui (cf. DrawHoverTooltip).
        hover_valid_  = true;
        hover_id_     = e.nameid;
        hover_refine_ = e.refine;
        hover_card_count_ = 0;
        for (uint32_t c : e.card)
          if (c) hover_cards_[hover_card_count_++] = c;
        std::snprintf(hover_name_, sizeof(hover_name_), "%s", r.name);
      }
      if (dbl) {
        sel_index_   = e.index;
        sel_visible_ = true;
        RequestRefine(e.index);
      }

      // ── Refine courant : l'information que le natif reçoit et jette ──
      // Rien d'affiché à +0 : « +0 » est du bruit, la colonne ne parle que quand
      // elle a quelque chose à dire.
      ImGui::TableNextColumn();
      if (e.refine > 0) {
        AlignCellTextMiddle();
        // Ambre quand la tentative atteindrait le plafond : au-delà, l'arme ne
        // sera plus proposée au prochain lancement de la compétence.
        const bool at_cap = cap > 0 && e.refine + 1 >= cap;
        ImGui::TextColored(at_cap ? col_cap : col_ref, "+%d", e.refine);
      }

      // ── Cartes / emplacements ──
      if (kColSlot >= 0) {
        ImGui::TableNextColumn();
        if (r.slots > 0) {
          AlignCellTextMiddle();
          ImGui::TextDisabled("%d/%d", CountRealCards(e.card, r.slots), r.slots);
        }
      }
      ImGui::PopID();
    }
    ImGui::PopStyleVar();  // SelectableTextAlign
    ImGui::EndTable();
  }
}

// Les trois minerais, en LIENS d'item : icône + nom + stock, cliquables vers la
// description — le pendant ImGui d'un `<ITEM>` de chat.
//
// Le nom vient de la DB client (itemInfoMerged.lua, via MoonlightUi::ItemName),
// jamais d'une constante : « Phracon » était écrit en dur ici, ce qui aurait
// menti sur tout serveur qui renomme ses objets ou tourne dans une autre langue
// (cf. la règle « jamais de données codées en dur »). Sans DB chargée on affiche
// l'id — faux jamais, muet parfois.
void WeaponRefineWindow::DrawOreLinks() {
  const uint32_t kOres[3] = {kOrePhracon, kOreEmveretarcon, kOreOridecon};
  const MoonlightUi* mu = Bourgeon::Instance().moonlight_ui();
  const ImGuiStyle& style = ImGui::GetStyle();
  // Bord droit UTILE, pour décider du retour à la ligne. Les trois noms plus
  // leurs icônes ne tiennent pas toujours sur une ligne (ils dépendent de la DB
  // et de la police) : sans ce calcul, le dernier sortait du cadre. Mesuré ICI,
  // au début — `GetContentRegionAvail` donne la place restante DEPUIS le curseur,
  // donc le relire dans la boucle rétrécirait la limite à chaque lien.
  const float right =
      ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;

  bool first = true;
  for (uint32_t id : kOres) {
    const int n = OreCount(id);
    const char* db_name = mu ? mu->ItemName(id) : nullptr;
    char label[96];
    if (db_name && db_name[0])
      std::snprintf(label, sizeof(label), "%s %d", db_name, n);
    else
      std::snprintf(label, sizeof(label), "#%u %d", id, n);

    const float w = kOreIcon + style.ItemInnerSpacing.x +
                    ImGui::CalcTextSize(label).x;
    if (!first) {
      // On reste sur la ligne SI ça tient encore ; sinon on laisse le flux
      // descendre tout seul (pas de SameLine).
      const float x_next = ImGui::GetItemRectMax().x + style.ItemSpacing.x;
      if (x_next + w <= right) ImGui::SameLine();
    }
    first = false;

    ImGui::BeginGroup();
    const ro::IconTex ic = ro::ItemIcon(id, 1);
    if (ic.tex) ImGui::Image(TexId(ic.tex), ImVec2(kOreIcon, kOreIcon));
    else        ImGui::Dummy(ImVec2(kOreIcon, kOreIcon));
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    // « 0 » n'est pas une information neutre — c'est la raison pour laquelle une
    // arme de ce niveau n'apparaît pas dans la liste — donc il est marqué en
    // rouge, pas grisé.
    const ImVec4 col = V4(n > 0 ? kColOk : kColBad);
    const ImVec2 text_min = ImGui::GetCursorScreenPos();
    ImGui::TextColored(col, "%s", label);
    const ImVec2 text_max = ImGui::GetItemRectMax();
    ImGui::EndGroup();

    // Après EndGroup, « le dernier item » EST le groupe : le survol et le clic
    // portent donc sur l'icône ET le texte, pas sur le seul libellé. Rien
    // d'interactif à l'intérieur ne vient brouiller ce test — une Image et un
    // Text ne consomment aucune entrée.
    if (ImGui::IsItemHovered()) {
      // Souligné + curseur main : c'est ce qui fait lire « lien » plutôt que
      // « étiquette ». Le souligné suit le TEXTE seul, pas le groupe — souligner
      // sous l'icône ferait un trait dans le vide.
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(text_min.x, text_max.y), ImVec2(text_max.x, text_max.y),
          ImGui::ColorConvertFloat4ToU32(col));
      ro::SetHoverCursor(kRoCursorHand);
      if (desc_tooltip_) {
        // Même mécanique que les lignes d'arme : on MÉMORISE, on peint après
        // EndRoWindow (itemcell::DrawTooltip crée son propre popup).
        hover_valid_      = true;
        hover_id_         = id;
        hover_refine_     = 0;
        hover_card_count_ = 0;
        hover_name_[0]    = '\0';
      }
    }
    // Un lien s'active au clic GAUCHE — c'est ce que fait un `<ITEM>` de chat, et
    // ici il n'y a pas de geste concurrent à lui voler : un minerai ne se
    // sélectionne pas. Le clic droit ouvre la même chose, par cohérence avec le
    // reste du client.
    if (ImGui::IsItemClicked() || IsLastItemRightClicked()) {
      POINT pt;
      // Par ID : le minerai peut ne pas être en inventaire (stock 0), il n'y a
      // donc pas toujours d'ItemSkillInfo vivant à passer.
      if (GetCursorPos(&pt)) {
        itemcell::OpenDescById(id, 0, 0, pt.x, pt.y);
        itemdesc::FocusDescWindow();  // sinon elle s'ouvre derrière nous
      }
    }
  }
}

void WeaponRefineWindow::DrawFooter() {
  ImGui::Separator();

  // Stock de minerai. Le serveur ne propose une arme que si SON minerai est là :
  // ces trois compteurs disent lesquels manquent, ce que le natif ne montre nulle
  // part. On ne DÉCIDE rien avec — le serveur reste seul juge.
  ImGui::TextDisabled("Minerai :");
  ImGui::SameLine();
  DrawOreLinks();

  const int cap = RefineSkillLevel();
  if (cap > 0) {
    ImGui::TextDisabled("Plafond : +%d", cap);
    ImGui::SameLine();
    HelpMarker(
        "Le niveau appris de la compétence Upgrade Weapon EST le plafond : le "
        "serveur refuse toute arme déjà à ce refine (et jamais au-delà de "
        "+10).");
  }

  if (awaiting_result_) {
    ImGui::TextColored(V4(kColWarn), "Tentative envoyée — en attente du serveur…");
    ImGui::Spacing();
  }

  // État de la relance automatique. Une action que le client prend de lui-même
  // doit se VOIR pendant qu'elle a lieu, et dire pourquoi elle s'arrête — sans
  // ça le joueur constate juste que sa fenêtre se rouvre ou ne se rouvre plus.
  if (auto_recast_at_) {
    ImGui::TextColored(V4(kColInfo), "Relance automatique… (%d)", auto_chain_);
    ImGui::Spacing();
  } else if (auto_stop_reason_ && auto_recast_) {
    ImGui::TextColored(V4(kColWarn), "%s", auto_stop_reason_);
    ImGui::Spacing();
  }

  // 🔴 `sel_visible_`, pas `sel_index_ >= 0` : le bouton ne s'arme que sur une
  // arme AFFICHÉE. Sélection perdue (arme détruite, disparue de la liste) ou
  // masquée par le filtre = bouton grisé, jusqu'à ce que le joueur re-désigne une
  // ligne. Rien ne re-cible à sa place sur une action qui détruit l'arme.
  const bool has_sel = sel_visible_;
  const bool busy    = awaiting_result_;

  if (!entries_.empty()) {
    // Dire POURQUOI le bouton est gris, sinon il a juste l'air cassé.
    if (!has_sel && !busy) {
      ImGui::TextDisabled("Sélectionne une arme dans la liste.");
      ImGui::Spacing();
    }
    ImGui::BeginDisabled(!has_sel || busy);
    if (ro::RoButton("Refine", kBtnRefineW)) RequestRefine(sel_index_);
    ImGui::EndDisabled();
    ImGui::SameLine();
  }

  // LE bouton qui manque au natif : enchaîner. Le serveur efface le menuskill
  // après CHAQUE tentative (clif_menuskill_clear) et le client referme sa
  // fenêtre — relancer la compétence est donc obligatoire, et c'était jusqu'ici
  // un aller-retour par la barre d'action.
  //
  // ⚠ Affiché SEULEMENT quand il n'y a plus de liste. Tant qu'une liste vivante
  // est à l'écran, relancer ne ferait que redemander la MÊME liste en payant le
  // SP : « Refine » et « Relancer » côte à côte n'étaient pas deux façons de
  // faire la même chose, mais deux étapes successives — et rien ne le disait.
  // Les rendre mutuellement exclusifs supprime l'ambiguïté à la racine.
  //
  // Un BOUTON par défaut. La relance automatique, elle, est opt-in et ne
  // relance que la COMPÉTENCE — jamais le refine, qui reste un clic (cf. le pavé
  // au-dessus de ScheduleAutoRecast).
  if (entries_.empty()) {
    ImGui::BeginDisabled(busy);
    if (ro::RoButton("Relancer le skill", kBtnRecastW)) {
      pending_ = kActRecast;
      // Relance MANUELLE : nouvelle chaîne, compteur remis à zéro.
      auto_chain_       = 0;
      auto_recast_at_   = 0;
      auto_stop_reason_ = nullptr;
    }
    ImGui::EndDisabled();
    // Infobulle SUR le bouton (pas un « (?) » à côté) : c'est le bouton qui a
    // besoin d'être expliqué, et il est déjà à l'étroit dans le pied.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(
          "Relance la compétence Upgrade Weapon pour obtenir une nouvelle liste.\n"
          "\n"
          "Le serveur n'autorise QU'UNE tentative par lancement : après chaque\n"
          "refine il faut relancer, et c'est ce que fait ce bouton — sans\n"
          "repasser par la barre d'action.");
    }
    ImGui::SameLine();
  }

  if (ro::RoButton("Fermer", kBtnCloseW)) RequestClose();
}

void WeaponRefineWindow::DrawHistory(float h) {
  ImGui::Separator();
  if (ImGui::BeginChild("##refine_history", ImVec2(0, h), true)) {
    for (const LogLine& l : history_) {
      const ImVec4 col(((l.color >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f,
                       ((l.color >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f,
                       ((l.color >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f, 1.0f);
      if (log_time_ && l.time[0]) {
        // L'heure en retrait : elle situe, elle ne se lit pas. La couleur reste
        // pour le message, qui porte le statut.
        ImGui::TextDisabled("%s", l.time);
        ImGui::SameLine();
      }
      ImGui::TextColored(col, "%s", l.text.c_str());
    }
    // Le journal suit toujours la dernière ligne.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
      ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
}

// ── Panneau de réglages ──────────────────────────────────────────────────────

bool WeaponRefineWindow::DrawSettings() {
  bool changed = false;
  ImGui::TextDisabled(
      "Remplace la fenêtre « Upgradeable weapons » du skill Upgrade Weapon.");
  ImGui::TextDisabled(
      "Clic droit : description · double-clic ou Entrée : refine.");
  ImGui::TextDisabled(
      "En-têtes de colonne : trier (3e clic = ordre d'inventaire).");
  changed |= ro::RoCheckbox("Confirmer avant un refine", &confirm_);
  ImGui::SameLine();
  HelpMarker(
      "Un échec DÉTRUIT l'arme, et le client affiche le MÊME message pour un "
      "succès et un échec. La confirmation rappelle l'arme visée et le risque.");
  changed |= ro::RoCheckbox("Cartes et emplacements", &show_cards_);
  ImGui::SameLine();
  HelpMarker(
      "Le paquet du serveur porte les 4 cartes de chaque arme — la fenêtre "
      "native les jette, deux armes identiques dont une sertie y sont donc "
      "indistinguables.");
  changed |= ro::RoCheckbox("Champ de filtre", &show_filter_);
  ImGui::SameLine();
  HelpMarker(
      "Utile sur un gros inventaire ; sur deux ou trois armes il ne fait que "
      "prendre une ligne. Le décocher efface aussi le filtre en cours, pour "
      "qu'aucune arme ne reste masquée par un champ invisible.");
  changed |= ro::RoCheckbox("Description au survol", &desc_tooltip_);
  changed |= ro::RoCheckbox("Relancer la compétence automatiquement",
                            &auto_recast_);
  ImGui::SameLine();
  HelpMarker(
      "Après chaque tentative, relance Upgrade Weapon pour rouvrir la liste — "
      "le serveur n'en autorise qu'une par lancement.\n"
      "\n"
      "Ne refine RIEN tout seul : le choix de l'arme et le déclenchement "
      "restent des clics. La chaîne s'arrête d'elle-même quand la liste revient "
      "vide, quand il n'y a plus de minerai, quand le serveur refuse une "
      "condition, ou au bout de 20 relances.");
  changed |= ro::RoCheckbox("Journal de session", &show_history_);
  ImGui::SameLine();
  HelpMarker(
      "Garde la trace des tentatives de la session, avec le libellé EXACT du "
      "serveur. N'apparaît qu'à partir du premier résultat — avant, il n'y a "
      "rien à montrer. C'est le seul endroit où succès et échec se distinguent : "
      "le client leur donne le MÊME texte (MsgString 911 et 912).");
  if (show_history_) {
    ImGui::Indent();
    changed |= ro::RoCheckbox("Horodater les lignes", &log_time_);
    ImGui::Unindent();
  }
  return changed;
}
