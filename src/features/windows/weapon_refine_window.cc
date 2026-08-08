#include "features/windows/weapon_refine_window.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bourgeon.h"
#include "features/craft_data.h"  // niveau d'arme + taux de refine (données serveur)
#include "features/item_cell.h"
#include "features/moonlight_ui/moonlight_ui.h"  // HelpMarker
#include "features/windows/item_desc_window.h"   // RenderSimpleDesc
#include "imgui.h"
#include "ragnarok/msgstring.h"  // msgstr:: (libellés natifs du client)
#include "ragnarok/globals.h"
#include "ragnarok/uiwnd.h"
#include "ui/icon_cache.h"
// (Le module ui/native_modal a été SUPPRIMÉ : la modale « liste vide » venait du
// handler NATIF, qui ne tourne plus. Cf. RefineWnd et docs §3.1 bis.)
#include "ui/ro_imgui.h"
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"

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

// Piloter les BOUTONS de la fenêtre native : `OnMsg(6, id)` est un clic réel.
// C'est le `case 6` de son OnMsg (`0x0096AAB0`) qui les reçoit, et les deux
// identifiants sont ceux du code natif — pas des suppositions :
//   184 = OK      -> SendMsg(182, sélection de SA listbox) puis fermeture
//   185 = Annuler -> SendMsg(182, -1) = le désarmement, puis fermeture
// (Ce sont aussi les valeurs de `+0x8C` / `+0x90`, les boutons « par défaut » que
// le gestionnaire déclenche sur Entrée / Échap.)
constexpr int kMsgUiAction = 6;
constexpr int kBtnCancelId = 185;

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
// Job level du personnage. Le MÊME global que celui dont UIBasicInfoWnd tire son
// « Job Lv. » (déjà employé par features/overlays/basic_info.cc) : c'est le
// dernier terme qui manquait pour calculer une chance de refine côté client.
constexpr uintptr_t kOwnJobLevel = 0x015fb9f8;

int OwnJobLevel() {
  __try {
    return *reinterpret_cast<const int*>(kOwnJobLevel);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// SP courant. Le MÊME global que la barre de SP de UIBasicInfoWnd
// (features/overlays/basic_info.cc le lit déjà, confirmé par RE de son
// DrawContent @0x0095e620) — donc la valeur qu'affiche le client, en INT32.
//
// Il n'est là que pour BORNER la chaîne automatique, jamais pour être affiché :
// le client a déjà sa jauge, et « il reste N lancements » serait un chiffre de
// plus à lire pendant que des armes se jouent. La chaîne tourne jusqu'à ne plus
// pouvoir, et c'est à ce moment-là qu'elle le dit.
constexpr uintptr_t kOwnSpCur = 0x015ff910;

int OwnSp() {
  __try {
    return *reinterpret_cast<const int*>(kOwnSpCur);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

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
constexpr unsigned kAutoRecastDelayMs = 400; // laisse passer le délai de cast
// Relance JETÉE par le serveur (ou restée sans réponse) : on réessaie, un nombre
// borné de fois. Même délai que l'armement normal — c'est le RÉESSAI qui compte, pas
// un délai plus long : le refus vient d'une collision ponctuelle avec le délai de
// lancement de la tentative précédente, pas d'un état durable.
constexpr unsigned kRecastRetryDelayMs = 400;
constexpr unsigned kRecastNoListMs     = 1000;  // chien de garde : rien n'est revenu
constexpr int      kMaxRecastRetries   = 3;

// ── Refine AUTOMATIQUE : le délai entre l'arrivée de la liste et l'envoi ──────
//
// Il n'est pas là pour ménager le serveur (la tentative part sur un menuskill
// tout frais, rien ne peut la refuser pour cause de délai) mais pour le JOUEUR :
// c'est la fenêtre pendant laquelle la liste est à l'écran, l'arme visée
// surlignée, et le bouton « Arrêter » cliquable. Sans elle, la chaîne défile sans
// que rien ne soit lisible ni interruptible.
//
// ⚠ OnTick est limité à ~100 ms : l'attente réelle vaut 350 à 450 ms, comme pour
// kAutoRecastDelayMs. Un tour complet coûte donc ~1 s (relance + liste + refine),
// ce qui laisse le temps de lire le résultat de chaque tentative.
constexpr unsigned kAutoRefineDelayMs = 350;

// ⏱ VALEURS RÉGLÉES EN JEU (2026-07-30). À 300 ms, la chaîne se coupait par moments
// avec Entrée maintenue ; à 400 ms, plus du tout.
//
// 🔴 Et le facteur qu'on oublie en lisant ces constantes : `OnTick` est LIMITÉ À
// ~100 ms. L'échéance n'est donc pas « 400 ms » mais « 400 à 500 ms » selon la frame
// où le tick tombe. À 300, l'attente réelle oscillait entre 300 et 400 ms et
// chevauchait la limite du serveur — d'où un défaut INTERMITTENT, la signature d'un
// seuil frôlé plutôt que d'une valeur fausse.
//
// Côté serveur, `WS_WEAPONREFINE` ne déclare aucun `AfterCastActDelay` dans
// `db/pre-re/skill_db.yml` : c'est donc le plancher global qui s'applique
// (`min_skill_delay_limit: 100` dans `conf/battle/skill.conf`), auquel s'ajoutent
// l'aller-retour du `menuskill` et la latence. Baisser ces constantes en dessous de
// 400 demande de remonter ces deux leviers ensemble, pas l'un sans l'autre.

// Intervalle minimal entre deux demandes de refine, tous gestes confondus. Une
// touche maintenue répète à la cadence du clavier — bien plus vite qu'un
// aller-retour serveur.
constexpr unsigned kMinSendIntervalMs = 400;

// MsgStringTable : on affiche les libellés EXACTS du client, jamais une
// paraphrase (règle du projet). Conversion CP949 -> UTF-8 dans msgstr::Utf8.
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
// Coût SP au niveau courant, écrit par le PAQUET serveur (docs/skill_tree_re.md
// §9.1). C'est la seule source honnête : le recopier depuis skill_db.yml en ferait
// une constante à nous, fausse le jour où le serveur change son coût.
constexpr int kSkOffSp      = 0x14;
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

// (L'escamotage de la modale native « liste vide » a été SUPPRIMÉ avec son module
// `ui/native_modal` : cette modale venait du handler natif du 0x0221, qui ne tourne
// plus. Le mécanisme — détour sur 0x00A31A30, renvoi de 185, deux verrous — est
// conservé dans docs/make_item_list_re.md §3.1 bis au cas où un autre chemin en
// aurait besoin.)

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

// Pilote le bouton Annuler de la fenêtre native de refine (id 185).
//
// Fonction SÉPARÉE pour la même raison que ReadWndPos juste en dessous : elle porte
// un `__try`, interdit dans toute fonction abritant un objet à destructeur non
// trivial (C2712), et FlushPending en manipule.
void CancelNativeRefine() {
  uint8_t* wnd = RefineWnd();  // vérifie déjà la vtable
  if (!wnd) return;
  __try {
    uiwnd::OnMsg(wnd, kMsgUiAction, kBtnCancelId);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
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
  itemcell::BuildDisplayName(info, out, out_size);
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
  // ── La liste : on prend la place du handler natif ──────────────────────────
  //
  // 🔴 REMPLACEMENT, plus observation — et ici l'enjeu n'est pas le confort mais
  // l'ARME. Observer laissait naître la fenêtre native 111, qu'on masquait
  // ensuite ; or une native masquée garde le clavier, et son bouton par défaut
  // (`+0x8C = 184` = OK) envoie `SendMsg(182)` sur la sélection de SA listbox.
  // Entrée ou Espace refinaient donc une arme que le joueur n'avait pas choisie,
  // qu'un échec DÉTRUIT (docs/weapon_refine_re.md §10). La fenêtre ne naît plus.
  //
  // Le prédicat est relu à chaque paquet et vaut exactement ce qui gate notre
  // fenêtre : plugin coupé, le handler natif reprend la main à l'octet près.
  //
  // ⚠ Effet de bord assumé : la position « là où la native se serait ouverte »
  // (repli de première utilisation dans OnRenderUI) devient inatteignable, puisque
  // la native ne s'ouvre plus. ImGui place alors la fenêtre lui-même, et la
  // position est persistée dès le premier déplacement — perte cosmétique, et
  // seulement au tout premier usage.
  //
  // ⚠ 0x0221 est un paquet à longueur VARIABLE. Les deux régimes transmettent les
  // octets à partir du champ `packetLength` (+2), qui est la vraie borne : le
  // parseur ne change donc pas d'un régime à l'autre, et le `len` du callback
  // reste IGNORÉ pour cet opcode (cf. le commentaire du parseur dans
  // OnRecvPacket).
  Bourgeon::Instance().RegisterReplaceOpcode(kOpRefineList,
                                             [this] { return imgui_enabled_; });
  Bourgeon::Instance().RegisterObserveOpcode(kOpRefineAck, kRefineAckLen);
  Bourgeon::Instance().RegisterObserveOpcode(kOpSkillFail, kSkillFailLen);
}

// ── Capture ──────────────────────────────────────────────────────────────────

// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h). La liste d'armes
// est à longueur ANNONCÉE — c'est elle qui fait foi, pas `len` : PushAnnounced.
void WeaponRefineWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                      uint16_t len) {
  if (opcode == kOpRefineList) net_inbox_.PushAnnounced(opcode, data, len);
  else                         net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
void WeaponRefineWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
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

    // 🔴 Calé sur le COMPTE DU PAQUET, pas sur `entries_.empty()`.
    //
    // Ce drapeau ne dit pas « la table est vide à l'écran », il dit « le serveur
    // n'a rien armé » — et il porte cette responsabilité parce que c'est lui qui
    // décide d'envoyer ou non l'annulation à la fermeture. Or le serveur arme
    // exactement sur `if (count > 0)` (clif_upgrade_list, et à l'identique dans
    // clif_skill_produce_mix_list / la liste de flèches) : se caler sur le même
    // compte, c'est ne PAS pouvoir se désynchroniser de lui.
    //
    // `entries_.empty()` pouvait, lui : une exception dans la boucle de parsing
    // au tout premier élément laisse le vecteur vide alors que le serveur a bel et
    // bien armé — on n'enverrait alors jamais le -1, et le personnage resterait
    // bloqué (aucune compétence ne passe plus).
    empty_list_      = (count <= 0);
    // Le serveur vient d'armer son menuskill si et seulement si count > 0.
    session_armed_   = (count > 0);
    awaiting_result_ = false;
    consumed_        = false;
    // Le résultat précédent ne survit qu'à une liste que la CHAÎNE a provoquée :
    // pendant un enchaînement les listes se succèdent en une demi-seconde, et
    // l'effacer à chaque tour ne laissait pas le temps de lire ce qui venait
    // d'arriver à l'arme. Une session ouverte à la main, elle, repart propre.
    if (auto_chain_ == 0) last_result_.clear();
    // La liste est revenue : la relance armée a fait son office (ou le serveur a
    // devancé le délai). Une liste VIDE termine naturellement la chaîne — c'est
    // le vrai « tant qu'il reste des armes », dit par le serveur lui-même plutôt
    // que deviné côté client.
    auto_recast_at_ = 0;
    // La liste EST la preuve que la compétence est repartie : plus de relance en
    // vol, et le compteur d'essais repart de zéro pour le tour suivant.
    recast_sent_at_ = 0;
    recast_retries_ = 0;
    if (empty_list_ && (auto_chain_ > 0 || auto_refine_count_ > 0))
      auto_stop_reason_ = i18n::Tr("Plus aucune arme à refine : chaîne arrêtée.");
    // Nouvelle liste = nouvelle cible à établir, et personne ne l'a encore vue.
    // Ces deux lignes SONT le garde-fou du refine automatique : tant que DrawList
    // n'a pas dessiné la liste, la chaîne n'a aucun index à jouer et attend.
    first_visible_index_ = -1;
    list_drawn_          = false;
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

    // 🔴 Plus d'escamotage de modale ICI, et c'est le remplacement du handler qui
    // le permet : la modale « liste vide » était affichée par le handler NATIF,
    // qui ne tourne plus quand on prend la main. Il n'y a donc plus rien à
    // escamoter.
    //
    // Le garder aurait même été NUISIBLE : l'escamotage se désarmait au prochain
    // appel de la fonction native, escamoté ou non — sans appel, le drapeau restait
    // armé et guettait la prochaine modale portant ce même texte. Un armement qui ne
    // peut plus être consommé est un piège en attente. C'est pourquoi le module a
    // été supprimé (docs make_item_list_re.md §3.1 bis le conserve).

    // La liste est en place : si le refine automatique est demandé, c'est ici que
    // le tour suivant s'arme. Rien n'est envoyé depuis un handler de paquet — on
    // pose une échéance, OnTick la transforme en action, FlushPending l'envoie.
    ScheduleAutoRefine();
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
    if (len < 2) return;
    uint16_t skill_id = 0;
    __try {
      skill_id = *reinterpret_cast<const uint16_t*>(data);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (skill_id != kSkillWeaponRefine) return;

    // 🔴 DEUX refus très différents portent le même paquet, et les confondre coûtait
    // la chaîne.
    if (awaiting_result_) {
      // (a) La TENTATIVE de refine est refusée. Une condition manque, et elle ne
      // changera pas d'elle-même : relancer tournerait en rond en brûlant du SP.
      awaiting_result_ = false;
      PushLog(i18n::Tr("Tentative refusée par le serveur (aucun minerai consommé)."),
              kColWarn);
      auto_recast_at_   = 0;
      auto_stop_reason_ = i18n::Tr("Le serveur a refusé la tentative : relance arrêtée.");
      return;
    }
    // (b) C'est notre RELANCE de compétence que le serveur a jetée — délai de cast,
    // `canact_tick`. Rien à voir avec une condition manquante : il suffit
    // d'attendre un peu plus et de recommencer.
    //
    // ⏱ C'était le bogue « Entrée maintenue » : le handler sortait sur
    // `!awaiting_result_` et IGNORAIT ce paquet. La chaîne s'arrêtait alors sans un
    // mot, fenêtre grisée sur « Session terminée », alors que l'arme était intacte
    // et toutes les conditions réunies — seul un clic sur « Relancer le skill »
    // repartait. Et cela n'arrivait QU'avec Entrée maintenue, parce que
    // `IsKeyPressed` répète ~20 fois par seconde : c'est le seul régime assez rapide
    // pour que la relance tombe dans le délai de cast de la tentative précédente.
    RetryRecast();
  }
}

// Arme (ou refuse) une relance automatique après un résultat de tentative.
// N'ENVOIE RIEN : on est dans un handler de paquet, l'envoi passe par
// FlushPending comme tout le reste.
//
// ⚠ Appelée UNIQUEMENT sur un 0x0223 qui répond à une tentative de nous. C'est
// ce lien de causalité qui borne la chaîne, et non un compteur : une relance ne
// peut suivre qu'un refine.
//
// 🔴 Et « un refine ne part que sur un geste du joueur » a cessé d'être vrai le
// jour où `refine_auto_refine` est apparu : sous cette option, c'est le tour
// précédent qui déclenche le suivant, et la boucle se referme sur elle-même. Ce
// n'est plus la main du joueur qui donne le rythme, ce sont les BORNES —
// AutoStopCause (minerai, SP) et la liste vide du serveur. Elles doivent donc
// rester exactes : ce sont les seules choses qui arrêtent la chaîne.
void WeaponRefineWindow::ScheduleAutoRecast(int result) {
  auto_recast_at_ = 0;
  if (!AutoChain() || auto_paused_ || !imgui_enabled_ || !ui_open_) return;

  // result 2 (« niveau de compétence insuffisant ») et 3 (« minerai manquant »)
  // ne sont pas des tentatives : ce sont des refus de condition, et cette
  // condition ne changera pas d'elle-même. Relancer là-dessus tournerait en rond
  // en brûlant du SP à chaque tour.
  if (result != 0 && result != 1) {
    auto_stop_reason_ = i18n::Tr("Le serveur a refusé la condition : chaîne arrêtée.");
    return;
  }
  // Minerai épuisé, SP épuisé : les bornes qui valent aussi bien pour la relance
  // seule que pour la chaîne complète, réunies en un seul endroit.
  if (const char* stop = AutoStopCause()) {
    auto_stop_reason_ = stop;
    return;
  }
  auto_stop_reason_ = nullptr;
  ++auto_chain_;
  auto_recast_at_ = GetTickCount() + kAutoRecastDelayMs;
  if (auto_recast_at_ == 0) auto_recast_at_ = 1;  // 0 = « rien d'armé »
}

// ── Les bornes de la chaîne automatique ──────────────────────────────────────
//
// Ce que le joueur a demandé en cochant l'option, c'est « ça tourne tant que le
// sort a du SP ». Ces deux tests sont donc la condition d'arrêt principale, et il
// n'y en a pas de troisième inventée ici : ni compteur d'armes, ni plafond de
// tours. Le reste des arrêts vient du SERVEUR (liste vide, refus de condition) et
// arrive par paquet.
//
// Rend le motif à afficher tel quel, ou nullptr si la chaîne peut continuer.
const char* WeaponRefineWindow::AutoStopCause() const {
  // Plus un seul minerai des trois : aucune arme ne peut plus entrer dans la
  // liste (clif_item_refine_list exige celui du niveau de l'arme). Inutile de
  // payer un cast pour se le faire dire.
  if (OreCount(kOrePhracon) == 0 && OreCount(kOreEmveretarcon) == 0 &&
      OreCount(kOreOridecon) == 0)
    return i18n::Tr("Plus aucun minerai : chaîne arrêtée.");

  // ── Le SP, la borne que l'option demande explicitement ────────────────────
  //
  // Le coût vient de la FICHE de compétence, que le serveur a envoyée
  // (CSkillInfo+0x14) : c'est le chiffre du serveur, pas une constante recopiée
  // de skill_db.yml.
  //
  // ⚠ Coût inconnu (0) = on se TAIT et on laisse passer. Bloquer sur une donnée
  // qu'on n'a pas ferait passer une absence pour un manque de SP ; et le cas est
  // couvert de toute façon — sans SP le client refuse le lancement, le chien de
  // garde d'OnTick s'en aperçoit et la chaîne s'arrête là.
  //
  // ⚠ Comparaison STRICTE (`<`) : le serveur exige `sp >= cost`, un personnage
  // pile au coût peut donc encore lancer. S'arrêter à l'égalité gaspillerait le
  // dernier cast, celui-là même que le joueur a payé en attendant sa régénération.
  int sp_cost = 0;
  RefineSkillLevel(&sp_cost);
  if (sp_cost > 0 && OwnSp() < sp_cost)
    return i18n::Tr("Plus assez de SP pour relancer la compétence : chaîne arrêtée.");
  return nullptr;
}

// Arme le refine automatique de la liste qui vient d'arriver.
//
// N'envoie RIEN et ne choisit PAS encore l'arme : la cible est établie par OnTick,
// à l'échéance, à partir de ce que DrawList a réellement affiché. Le décalage est
// délibéré — cf. first_visible_index_ dans l'en-tête.
void WeaponRefineWindow::ScheduleAutoRefine() {
  auto_refine_at_ = 0;
  if (!auto_refine_ || auto_paused_ || !imgui_enabled_ || !ui_open_) return;
  // Liste vide, ou déjà consommée : rien à jouer. Le motif d'arrêt, lui, a déjà
  // été posé par le handler (« plus aucune arme à refine »).
  if (entries_.empty() || consumed_ || awaiting_result_) return;
  if (const char* stop = AutoStopCause()) {
    auto_stop_reason_ = stop;
    return;
  }
  auto_stop_reason_ = nullptr;
  auto_refine_at_ = GetTickCount() + kAutoRefineDelayMs;
  if (auto_refine_at_ == 0) auto_refine_at_ = 1;  // 0 = « rien d'armé »
}

// Ré-arme une relance dont le LANCEMENT n'a rien donné : soit le serveur l'a jetée
// (ZC 0x0110 hors tentative), soit rien n'est revenu du tout (chien de garde).
//
// À ne pas confondre avec ScheduleAutoRecast, qui traite le RÉSULTAT d'une tentative.
// Ici la tentative n'a jamais eu lieu : la compétence elle-même n'est pas partie.
void WeaponRefineWindow::RetryRecast() {
  recast_sent_at_ = 0;
  // Le joueur reste prioritaire, exactement comme pour l'armement initial.
  if (!AutoChain() || auto_paused_ || !imgui_enabled_ || !ui_open_ || !consumed_)
    return;
  if (pending_ != kActNone) return;  // une action est déjà posée

  // Borné, et pas par le compteur de chaîne : celui-ci compte les tours RÉUSSIS.
  // Un délai de cast se résorbe en une ou deux tentatives ; au-delà, c'est autre
  // chose et insister ne ferait que masquer le vrai motif d'arrêt.
  if (++recast_retries_ > kMaxRecastRetries) {
    auto_stop_reason_ =
        i18n::Tr("La compétence ne repart pas (délai de lancement) : relance arrêtée.");
    PushLog(auto_stop_reason_, kColWarn);
    return;
  }
  // Délai PLUS LONG que l'armement normal : la cause est précisément qu'on est
  // arrivé trop tôt. Réessayer au même rythme rejouerait le même refus.
  auto_recast_at_ = GetTickCount() + kRecastRetryDelayMs;
  if (auto_recast_at_ == 0) auto_recast_at_ = 1;
  auto_stop_reason_ = nullptr;
  char line[128];
  std::snprintf(line, sizeof(line),
                i18n::Tr("Relance jetée par le délai de lancement : nouvel essai (%d/%d)."),
                recast_retries_, kMaxRecastRetries);
  PushLog(line, kColWarn);
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
      prefix = i18n::Tr("Succès — ");
      break;
    case 1:
      // Le seul cas destructeur, et celui que le client rend indiscernable du
      // succès (911 == 912 dans msgstringtable.csv). D'où le préfixe explicite.
      msg_id = kMsgRefineFail;
      color  = kColBad;
      prefix = i18n::Tr("ÉCHEC — arme détruite — ");
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
  if (!name[0]) {
    std::snprintf(subject, sizeof(subject), i18n::Tr("id %u"), nameid);
  } else if (result == 0 && sent_refine_ >= 0) {
    // 🔴 Une réussite doit énoncer un PASSAGE, pas un état. « Refined weapon:
    // +4 Knife » est lu comme « elle est à +4 » — donc comme un échec — alors que
    // l'arme vient de passer à +5. On écrit les deux bornes.
    //
    // ⚠ De quel refine le nom est-il décoré ? Indécidable : selon que
    // l'inventaire a déjà été rafraîchi ou qu'on soit retombé sur le nom capturé
    // à l'envoi, son préfixe porte l'ANCIEN ou le NOUVEAU niveau. On tente donc
    // les deux, et le pire cas est un préfixe conservé — jamais un nom tronqué
    // (cf. SkipRefinePrefix, qui ne coupe que sur correspondance exacte).
    const char* bare = SkipRefinePrefix(name, sent_refine_);
    if (bare == name) bare = SkipRefinePrefix(name, sent_refine_ + 1);
    std::snprintf(subject, sizeof(subject), "%s +%d -> +%d", bare, sent_refine_,
                  sent_refine_ + 1);
  } else {
    // Échec, mauvais niveau, minerai manquant : le nom décoré suffit et dit vrai
    // (l'arme détruite l'était bien à ce refine-là).
    std::snprintf(subject, sizeof(subject), "%s", name);
  }

  const char* tmpl = msgstr::Utf8(msg_id);
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
  // Gardé aussi hors du journal : le pied de fenêtre l'affiche jusqu'à la liste
  // suivante. Sans ça, le seul endroit où lire « arme détruite » était un panneau
  // optionnel, replié par défaut.
  last_result_       = line;
  last_result_color_ = color;
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
  last_result_.clear();
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
  recast_sent_at_   = 0;
  recast_retries_   = 0;
  // Idem pour le refine automatique — et l'arrêt demandé par le joueur ne survit
  // pas non plus : une nouvelle session repart d'une page blanche, sinon un
  // « Arrêter » cliqué la veille bloquerait une chaîne qu'on croit relancée.
  auto_refine_at_    = 0;
  auto_refine_count_ = 0;
  auto_paused_       = false;
  first_visible_index_ = -1;
  list_drawn_          = false;
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
    // Ici on n'envoie RIEN, et c'est la différence avec la bascule d'interrupteur :
    // quitter le monde de jeu passe par `unit_remove_map_`, qui remet lui-même
    // `menuskill_id` à zéro côté serveur. Un -1 partirait dans le vide, sur une
    // connexion qui n'a d'ailleurs peut-être plus de socket.
    session_armed_ = false;
    ResetSession();
    history_.clear();
  }
}

void WeaponRefineWindow::CloseForOtherCraft() {
  // Rien d'ouvert ni d'armé : il n'y a rien à évincer, et poser un message serait du
  // bruit à chaque fabrication.
  if (!ui_open_ && !session_armed_) return;

  // 🔴 ON DÉSARME POUR DE VRAI — `182 / -1`. Une première rédaction ne faisait que
  // fermer, en supposant que la liste de fabrication avait écrasé notre `menuskill`.
  // C'est vrai quand cette liste est PLEINE… et faux quand elle est VIDE, puisque le
  // serveur n'arme que `if (count > 0)`. Or elle arrive justement vide dans ce cas,
  // et pour une raison en boucle : `skill_can_produce_mix` écarte TOUTE recette dont
  // le `req_skill` ne correspond pas à un `menuskill_id` positif déjà en place
  // (skill.cpp, `// special case`). Notre session de refine encore armée vidait donc
  // la liste de fabrication, qui du coup n'armait rien, qui du coup ne nous
  // désarmait pas… et plus rien ne repartait jamais.
  //
  // ⚠ Et c'est BIEN à nous de l'envoyer : l'annulation de la fabrication (CZ 0x018E,
  // itemId 0) ne peut PAS effacer un menuskill de refine — `clif_parse_ProduceMix`
  // sort par son `default:` sans rien toucher. Seul `clif_parse_WeaponRefine` efface
  // un `WS_WEAPONREFINE`.
  //
  // On POSE l'action (FlushPending l'enverra hors frame ImGui) et on ne touche PAS à
  // `session_armed_` : c'est lui que FlushPending consulte pour décider d'envoyer,
  // et ResetSession juste en dessous ne l'écrase pas — c'est écrit dans son
  // commentaire d'en-tête, et c'est exactement pour ce genre de cas.
  //
  // Un `182 / -1` de trop est inoffensif : si le serveur a déjà autre chose d'armé,
  // `clif_parse_WeaponRefine` sort immédiatement, sans rien effacer.
  pending_ = kActCancel;
  auto_recast_at_ = 0;
  auto_refine_at_ = 0;
  recast_sent_at_ = 0;

  PushLog(i18n::Tr("Session de refine abandonnée : une fabrication a été lancée (le serveur "
          "n'en garde qu'une)."), kColWarn);
  FlushWindowPos();  // la fenêtre se referme : c'est le moment d'écrire sa position
  ResetSession();
}

void WeaponRefineWindow::OnTick() {
  if (prev_enabled_ != imgui_enabled_) {
    prev_enabled_ = imgui_enabled_;
    // (Plus rien à « rendre visible » à la coupure : on ne masque plus jamais cette
    // fenêtre. La 111 ne naît que si nous sommes coupés, et alors elle est à elle.)
    //
    // 🔴 DÉSARMER AVANT DE JETER. La bascule referme notre interface, mais le
    // serveur, lui, garde son `menuskill` : sans ce -1 le personnage ne peut plus
    // lancer AUCUNE compétence, ni en moderne ni en natif, et rebasculer n'y change
    // rien puisque le blocage n'est pas chez nous.
    //
    // ⏱ Constaté en jeu : « j'ai lancé refine, désactivé l'interface moderne, le
    // skill ne part plus ». Avant le remplacement d'opcode le cas était masqué — la
    // fenêtre native existait encore et reprenait la main ; elle ne naît plus.
    //
    // On POSE l'action (FlushPending l'enverra hors frame ImGui) et on ne se fie
    // qu'à `session_armed_`, qui parle du serveur : ResetSession, juste après, remet
    // à zéro tout le reste sans y toucher.
    if (!imgui_enabled_ && session_armed_) pending_ = kActCancel;

    // (Le sens INVERSE — natif -> moderne, native déjà ouverte — n'a RIEN à faire
    // ici : c'est FlushPending qui rend cette session, à chaque frame et sans
    // drapeau. L'armer depuis ce tick était le bogue de la fabrication : limité à
    // 100 ms, il arrivait après FlushPending, qui avait déjà escamoté la fenêtre.)
    ResetSession();
  }

  uint8_t* wnd = RefineWnd();
  open_ = (wnd != nullptr);

  if (!imgui_enabled_) return;

  // 🔴 PLUS DE MASQUAGE DE LA NATIVE, nulle part. Une fenêtre invisible garde le
  // clavier ET sa session : c'est le fantôme qui a bloqué le personnage deux fois
  // (§10 du doc). Elle ne peut de toute façon plus vivre qu'une frame, celle qui
  // sépare sa découverte du `CancelNativeRefine()` de FlushPending. Et des deux
  // échecs possibles, une native VISIBLE une frame de trop est cosmétique.
  if (!wnd && was_open_) {
    // La native a disparu — mais surtout PAS notre fenêtre. Le client la détruit
    // dès la tentative envoyée, et c'est précisément là que le joueur veut voir
    // le résultat et enchaîner (cf. le commentaire de ui_open_ dans l'en-tête).
    // On invalide donc la LISTE (elle n'existe plus côté serveur) sans toucher à
    // ui_open_.
    //
    // ⚠ Cette branche ne s'atteint pratiquement plus : la native ne naissant plus,
    // `open_` reste faux et `was_open_` avec lui. Ce n'est PAS une perte — la
    // véritable invalidation est posée à l'ENVOI (`consumed_ = true` dans
    // FlushPending), de façon déterministe, et le `entries_.clear()` d'ici est même
    // contraire à ce qu'on veut désormais (« on MARQUE, on ne VIDE PLUS » : la table
    // reste à l'écran, grisée, le temps de lire le résultat).
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
    // `consumed_` et non `entries_.empty()` : la table reste maintenant affichée
    // après une tentative, donc sa présence ne dit plus rien de l'état serveur.
    if (AutoChain() && !auto_paused_ && ui_open_ && consumed_ &&
        pending_ == kActNone) {
      // Le SP a pu fondre entre l'armement et maintenant (un tour complet dure
      // près d'une seconde, et rien n'interdit de se faire taper dessus pendant
      // ce temps-là). On revérifie donc AVANT d'envoyer, sinon la chaîne paie un
      // lancement que le client refusera.
      if (const char* stop = AutoStopCause()) {
        auto_stop_reason_ = stop;
      } else {
        pending_ = kActRecast;
      }
    }
  }

  // ── Échéance du refine AUTOMATIQUE ─────────────────────────────────────────
  // Même mécanique que la relance : on POSE l'action, FlushPending l'envoie hors
  // frame ImGui.
  if (auto_refine_at_ &&
      static_cast<int>(GetTickCount() - auto_refine_at_) >= 0) {
    // Tout ce qui a pu changer depuis l'armement. La confirmation ouverte compte :
    // le joueur a demandé une tentative à la main, la chaîne ne lui passe pas
    // devant.
    if (!auto_refine_ || auto_paused_ || !imgui_enabled_ || !ui_open_ ||
        consumed_ || awaiting_result_ || pending_ != kActNone ||
        confirm_index_ >= 0 || entries_.empty()) {
      auto_refine_at_ = 0;
    } else if (!list_drawn_) {
      // 🔴 La liste n'a pas encore été DESSINÉE : on n'a donc aucune cible, et on
      // ATTEND (l'échéance reste armée). C'est le garde-fou central de cette
      // option — aucune arme n'est détruite sans être passée à l'écran d'abord.
      // La frame suivante renseignera first_visible_index_.
    } else if (const char* stop = AutoStopCause()) {
      auto_refine_at_   = 0;
      auto_stop_reason_ = stop;
    } else {
      // La cible : l'arme SÉLECTIONNÉE si elle est visible — c'est la reconduction
      // de sélection du handler de liste, qui remet la main sur l'arme qu'on était
      // en train de monter — sinon la première ligne AFFICHÉE, filtre et tri
      // appliqués. Jamais entries_.front(), qui peut être masquée par le filtre.
      const int target = sel_visible_ ? sel_index_ : first_visible_index_;
      auto_refine_at_ = 0;
      if (target < 0) {
        // Liste non vide mais rien d'affiché : un filtre exclut tout. S'arrêter en
        // le disant vaut mieux que tourner en rond sur une liste invisible.
        auto_stop_reason_ =
            i18n::Tr("Aucune arme ne correspond au filtre : chaîne arrêtée.");
      } else {
        sel_index_        = target;
        pending_          = kActRefine;
        pending_index_    = target;
        last_send_tick_   = GetTickCount();  // même cadence que les gestes manuels
        auto_stop_reason_ = nullptr;
        ++auto_refine_count_;
      }
    }
  }

  // ── Chien de garde de la relance ───────────────────────────────────────────
  // La compétence est partie mais AUCUNE liste n'est revenue. Deux cas : le serveur
  // l'a jetée sans le dire (tous les refus n'envoient pas de ZC 0x0110), ou le
  // chemin natif de lancement l'a refusée côté client — dans les deux cas, aucun
  // paquet ne nous l'apprendra. Sans ce garde-fou la chaîne restait bloquée sur
  // « Session terminée », arme intacte, jusqu'à un clic manuel.
  if (recast_sent_at_ &&
      static_cast<int>(GetTickCount() - recast_sent_at_) >
          static_cast<int>(kRecastNoListMs)) {
    RetryRecast();
  }

  was_open_ = open_;
}

// ── Actions différées (hors frame ImGui) ─────────────────────────────────────

void WeaponRefineWindow::FlushPending() {
  // (L'ouverture de description différée au relâchement qui ouvrait cette
  //  fonction a été GÉNÉRALISÉE : c'est désormais itemcell::FlushDeferredDesc,
  //  appelé par Bourgeon::OnProcessInput pour les huit viewers.)

  // ── Rendre toute session NATIVE encore vivante ────────────────────────────
  // ⚠ Placé AVANT le retour anticipé sur kActNone : ce n'est pas une PendingAction,
  // et l'y soumettre l'aurait rendu muet la plupart du temps.
  //
  // SANS ÉTAT, et c'est tout l'intérêt : une native vivante alors que nous sommes
  // actifs est forcément une session que nous n'avons PAS ouverte — depuis le
  // remplacement du 0x0221, aucune ne peut plus naître sous notre garde. Son Annuler
  // est donc toujours le bon geste, et l'appeler à chaque frame supprime la course
  // qui a cassé la fabrication (drapeau posé par OnTick, limité à 100 ms, contre un
  // FlushPending par frame).
  //
  // `OnMsg(6, 185)` = un clic RÉEL sur son bouton Annuler. La fenêtre envoie alors
  // elle-même `SendMsg(182, -1)` — donc `clif_menuskill_clear` côté serveur — puis
  // `SaveRectAndCloseWindow(111)`. Vérifié dans son `OnMsg` (`0x0096AAB0`,
  // `case 6` / `Value == 185`) : l'identifiant du bouton vient du code natif, pas
  // d'une supposition. `CancelNativeRefine` vérifie la vtable avant d'agir, donc
  // l'appel est inoffensif quand il n'y a rien.
  if (imgui_enabled_) CancelNativeRefine();

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
      // Le refine d'avant vient de la LISTE SERVEUR, pas du nom : le préfixe
      // décoratif peut manquer (+0) et l'inventaire, lui, aura déjà bougé quand
      // le résultat arrivera.
      sent_refine_ = -1;
      for (const Entry& e : entries_)
        if (e.index == idx) { sent_refine_ = e.refine; break; }
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
      // suivant. Le marquer ICI est déterministe et dit la même chose que le
      // serveur.
      //
      // ⚠ On MARQUE, on ne VIDE PLUS. « La liste est morte » veut dire « on n'a
      // plus le droit d'envoyer », pas « il n'y a plus rien à montrer » — et
      // `consumed_` porte déjà exactement cette règle. Vider `entries_` faisait
      // en plus disparaître la table, rétracter la fenêtre, puis tout revenir
      // ~500 ms plus tard : un clignotement à chaque tentative, sans le temps de
      // lire le résultat. La table reste donc à l'écran, GRISÉE, et c'est
      // `consumed_` qui verrouille l'envoi (RequestRefine, bouton, OnTick).
      consumed_ = true;
      // Et le serveur, lui, vient de faire son propre `clif_menuskill_clear` en
      // recevant cette tentative (un seul refine par lancement) : il n'attend donc
      // plus rien, et une annulation ultérieure serait un paquet pour personne.
      session_armed_ = false;
      break;

    case kActCancel:
      // ⚠ Annuler DOIT envoyer : c'est ce -1 qui fait appeler
      // clif_menuskill_clear côté serveur. Sans lui le personnage reste avec un
      // menuskill armé (cf. l'en-tête, piège n°2).
      //
      // …mais SEULEMENT si une session est encore armée. Après un refine, le
      // serveur a déjà fait son clif_menuskill_clear : un -1 de plus serait un
      // paquet inutile, et notre fenêtre reste ouverte bien après ce moment-là
      // (c'est tout l'intérêt de ui_open_).
      //
      // 🔴 Ce critère était `RefineWnd()` — la PRÉSENCE de la fenêtre native.
      // ⏱ Cassé net par le remplacement d'opcode : la native ne naît plus, donc le
      // -1 ne partait plus JAMAIS, le serveur gardait son menuskill armé, et plus
      // aucune compétence ne passait ensuite. Constaté en jeu (« j'ai fermé la
      // fenêtre, maintenant le skill ne part plus »).
      //
      // Le critère est donc NOTRE état, `consumed_`, qui dit exactement la même
      // chose sans dépendre du client : faux = le serveur attend encore une
      // réponse, vrai = il a déjà refermé sa session lui-même.
      //
      // ⚠ Et l'asymétrie compte : un -1 en trop est INOFFENSIF
      // (`clif_parse_WeaponRefine` sort aussitôt quand `menuskill_id` ne
      // correspond pas), un -1 manquant BLOQUE le personnage. En cas de doute, on
      // envoie.
      //
      // `!empty_list_` en second : le serveur envoie la liste MÊME vide
      // (`clif_send` inconditionnel) mais ne l'arme que `if (count > 0)` — vérifié
      // dans clif_upgrade_list. Sur une liste vide il n'y a donc rien à désarmer, et
      // `empty_list_` est justement calé sur ce même compte (cf. OnRecvPacket).
      if (session_armed_) {
        SendModeCmd(kCmdRefine, -1);
        session_armed_ = false;
      }
      // Filet du basculement d'interrupteur : si une native traîne (elle n'est
      // créée que quand le plugin était coupé au moment du paquet), on la referme.
      if (RefineWnd()) uiwnd::CloseWindow(kWinRefine);
      break;

    case kActRecast: {
      // Relance du skill par le chemin natif (barre de cast, contrôles SP et
      // cooldown côté client compris) — un CZ_USE_SKILL fabriqué à la main les
      // sauterait tous.
      const int level = std::max(1, RefineSkillLevel());
      const uint32_t self = OwnAid();
      if (self) {
        SendModeCmd(kCmdUseSkill, kSkillWeaponRefine, static_cast<int>(self),
                    level);
        // Relance EN VOL. Le serveur peut la jeter (délai de cast) sans qu'aucune
        // liste ne revienne : c'est ce marqueur qui permet de s'en apercevoir, via
        // le ZC 0x0110 s'il en envoie un, ou via le chien de garde d'OnTick sinon.
        recast_sent_at_ = GetTickCount();
        if (recast_sent_at_ == 0) recast_sent_at_ = 1;
      }
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

int WeaponRefineWindow::RefineSkillLevel(int* sp_cost) {
  int found = 0;
  int sp    = 0;
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
        // Coût SP au niveau courant, lu sur la MÊME fiche : le chercher à part
        // rejouerait tout le parcours pour retomber sur ce nœud-ci.
        sp = *reinterpret_cast<const int*>(v + kSkOffSp);
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  if (sp_cost) *sp_cost = sp > 0 ? sp : 0;  // négatif ou absent = « je ne sais pas »
  return found;
}

// ── Rendu ────────────────────────────────────────────────────────────────────

// Réponse au hook de WndProc. Sans état de rendu : la déduire d'un drapeau posé
// à la frame précédente la rendrait FAUSSE dès que le rendu s'arrête sans que la
// fenêtre se ferme — pendant un chargement de carte, typiquement — et la touche
// resterait avalée pour un client qui n'affiche plus rien.
bool WeaponRefineWindow::WantsEnterKey() const {
  const bool alive = imgui_enabled_ && ui_open_ &&
                     !Bourgeon::Instance().IsMapLoading() &&
                     Bourgeon::Instance().IsGameActive();
  if (!alive) return false;
  // ⚠ La CONFIRMATION garde la touche quoi qu'il arrive : « Entrée = OK » est la
  // convention d'une modale, et la laisser passer au jeu ouvrirait le chat en
  // même temps qu'elle valide une action qui peut DÉTRUIRE l'arme. La
  // confiscation est donc limitée au moment où elle se justifie.
  if (confirm_index_ >= 0) return true;

  // Hors modale : décoché, la touche n'est PAS confisquée et le chat reste
  // accessible pendant qu'on refine.
  //
  // ⏱ Cette ligne a été, un temps, un `return true` inconditionnel — et c'était
  // justifié À L'ÉPOQUE : la native 111 vivait masquée derrière nous, et une native
  // invisible garde le CLAVIER. La chaîne est établie sur le binaire :
  //     UIWindowMgr_OnKeyDown @0x00A471E0   Entrée (13) ou Espace (32)
  //  -> @0x00A2E270                          OnMsg(msg = 0) sur la prioritaire
  //  -> UIWindow_OnMsg_Default @0x008841D0   OnMsg(6, this+0x8C)
  //  -> la 111 a `+0x8C = default_id = 184` = son bouton OK
  //  -> OnMsg case 6 / 184 : SendMsg(182, liste_native[sélection native])
  // …soit un refine RÉEL sur l'arme choisie par le CLIENT, qu'un échec DÉTRUIT.
  // Aucune étape ne consulte la visibilité (le prédicat vt+8 est un `return 1` en
  // dur, @0x005A5D90).
  //
  // 🔴 Ce qui a changé : cette fenêtre ne NAÎT PLUS (`RegisterReplaceOpcode` sur
  // 0x0221) et celle qu'on hérite d'un basculement d'interrupteur est ANNULÉE dans
  // la frame (`CancelNativeRefine`). Il n'y a donc plus de fenêtre fantôme à qui la
  // touche pourrait profiter, et confisquer coûterait le chat pour rien.
  //
  // ⚠ Si un jour on remet une native masquée en vie derrière cette fenêtre, il
  // faudra RÉTABLIR le `return true`. Le danger n'est pas théorique : il a été
  // constaté en jeu sur la fabrication (79), dont l'OnMsg a la même structure.
  return enter_key_;
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

  ro::SetNextWindowTitleBullet(i18n::Tr("Options du refine"));
  bool open = true;
  // NoScrollbar/NoScrollWithMouse : le contenu tient toujours (la liste a son
  // propre enfant scrollable), la barre de la fenêtre était parasite.
  const bool begun = ro::BeginRoWindow(
      i18n::Tr("Refine Weapon###bourgeon_weapon_refine"), &open,
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
      ImGui::TextColored(V4(kColWarn), i18n::Tr("Aucune arme refinable."));
      ImGui::Spacing();
      ImGui::TextWrapped(i18n::Tr("Le serveur ne propose une arme que si TOUT est vrai :"));
      BulletWrapped(i18n::Tr("elle est identifiée et a un niveau d'arme (1 à 4) ;"));
      BulletWrapped(i18n::Tr("elle n'est PAS portée (déséquipe-la d'abord) ;"));
      BulletWrapped(i18n::Tr("son refine est encore sous le plafond de ta compétence ;"));
      BulletWrapped(i18n::Tr("tu as le minerai correspondant à son niveau d'arme."));
      ImGui::Spacing();
      DrawFooter();
    } else if (entries_.empty()) {
      // Tentative partie : la liste n'existe plus côté serveur (menuskill
      // effacé) et le client a détruit sa fenêtre. C'est l'écran que le natif
      // n'a pas du tout — il se contente de tout refermer.
      // Même règle qu'au pied : muet tant qu'une relance est armée (cf. le pavé
      // de DrawFooter). Sans ça le message clignotait entre deux refines
      // enchaînés, en annonçant une fin qui n'arrivait pas.
      if (!awaiting_result_ && consumed_ && auto_recast_at_ == 0 &&
          pending_ != kActRecast) {
        ImGui::TextWrapped(
            i18n::Tr("Session terminée : le serveur n'autorise qu'un refine par "
            "lancement de la compétence."));
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
      // Liste morte côté serveur mais toujours affichée : GRISÉE. Ce n'est pas un
      // ornement — c'est ce qui distingue « actionnable » de « là pour
      // référence ». Sans le signal, garder la table laisserait croire qu'une
      // seconde tentative peut partir, alors que le serveur la jetterait sans
      // même répondre.
      const bool stale = consumed_;
      if (stale)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
      DrawList(head_h + row_h * kVisibleRows);
      if (stale) ImGui::PopStyleVar();
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
        enter_key_ && sel_visible_ && !ImGui::IsAnyItemActive() &&
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
    ImGui::OpenPopup(i18n::Tr("Confirmer le refine###bourgeon_refine_confirm"));
    confirm_open_ = false;
    // Frame d'ouverture, retenue pour que la MÊME frappe d'Entrée ne traverse pas
    // la confirmation (cf. plus bas) : la modale s'ouvre et se dessine dans la
    // frame courante, où IsKeyPressed(Enter) est encore vrai.
    confirm_frame_ = ImGui::GetFrameCount();
  }

  if (ro::BeginRoPopupModal(i18n::Tr("Confirmer le refine###bourgeon_refine_confirm"))) {
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
      ImGui::TextWrapped("%s", name[0] ? name : i18n::Tr("(arme inconnue)"));
      ImGui::Spacing();
      ImGui::TextColored(V4(kColBad), i18n::Tr("Un échec DÉTRUIT l'arme."));
      ImGui::TextWrapped(
          i18n::Tr("Le minerai est consommé dans tous les cas. En cas de réussite "
          "l'arme passe de +%d à +%d."),
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
      if (ro::RoButton(i18n::Tr("Refine"), kBtnConfirmW) || enter_ok) {
        pending_       = kActRefine;
        pending_index_ = confirm_index_;
        confirm_index_ = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ro::RoButton(i18n::Tr("Annuler"), kBtnConfirmW)) {
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
  // `consumed_` : depuis que la table survit à la tentative, une liste affichée
  // ne prouve plus que le serveur attend une réponse — c'est ce drapeau, et lui
  // seul, qui dit si le `menuskill` est encore armé.
  if (awaiting_result_ || consumed_ || pending_ != kActNone ||
      confirm_index_ >= 0)
    return;
  // Garde anti-rafale, désormais sur TOUS les chemins : une touche maintenue
  // répète à la cadence du clavier, bien plus vite qu'un aller-retour serveur.
  const unsigned now = GetTickCount();
  if (now - last_send_tick_ <= kMinSendIntervalMs) return;
  last_send_tick_ = now;

  // Un refine demandé À LA MAIN lève l'arrêt de la chaîne automatique : le joueur
  // vient de reprendre les commandes, et c'est exactement le geste qui dit « on
  // repart ». Le réglage, lui, n'a jamais bougé — cf. le bouton « Arrêter ».
  auto_paused_ = false;

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
  // Fermer, c'est arrêter : une chaîne automatique ne doit pas survivre à la
  // fenêtre qui l'affiche. (OnTick le verrait aussi par `ui_open_`, mais compter
  // sur un effet de bord pour couper une boucle qui détruit des armes serait une
  // mauvaise façon d'écrire ça.)
  auto_refine_at_ = 0;
  auto_recast_at_ = 0;
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
    ImGui::InputTextWithHint("##refine_filter", i18n::Tr("Filtrer…"), filter_,
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
      std::snprintf(r.name, sizeof(r.name), i18n::Tr("id %u"), e.nameid);

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
  const int kColRef  = col++;
  const int kColName = col++;
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
    // « + » AVANT le nom : c'est le refine qui distingue deux lignes portant la
    // même arme, et il se lit du même côté que l'icône — la colonne étirable,
    // elle, doit rester la dernière avant les emplacements pour ne pas repousser
    // les chiffres au loin.
    //
    // Le refine et les emplacements se lisent « du plus au moins » : premier clic
    // décroissant, c'est ce qu'on cherche (quelle arme est la plus montée).
    ImGui::TableSetupColumn("+", ImGuiTableColumnFlags_WidthFixed |
                                     ImGuiTableColumnFlags_PreferSortDescending,
                            26.0f);
    ImGui::TableSetupColumn(i18n::Tr("Arme"), ImGuiTableColumnFlags_WidthStretch);
    if (show_cards_)
      ImGui::TableSetupColumn(i18n::Tr("Slots"),
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
    // Première ligne RÉELLEMENT affichée (filtre appliqué, tri appliqué) : c'est
    // la cible de repli du refine automatique quand la sélection n'est plus
    // visible. Relevée ici, après le tri, pour qu'elle désigne bien la ligne du
    // haut telle que le joueur la voit.
    first_visible_index_ = rows.empty() ? -1 : rows.front().e->index;

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

      // ── Nom : c'est LUI qui porte le Selectable de toute la ligne ──
      // Le Selectable est soumis APRÈS l'icône et le « + », et pourtant il ne les
      // recouvre pas : `SpanAllColumns` bascule son fond dans le canal d'arrière-
      // plan de la table (TablePushBackgroundChannel), quelle que soit la colonne
      // d'où il part.
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
        // Par INDEX (l'ItemSkillInfo vivant rend cartes/refine/enchants), et
        // DIFFÉRÉE au relâchement (itemcell::FlushDeferredDesc) : ouverte ici,
        // un appui PROLONGÉ faisait ressortir la description DERRIÈRE nous.
        POINT pt;
        if (GetCursorPos(&pt))
          itemcell::DeferDescFromIndex(kInvListHead, e.index, pt.x, pt.y);
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
  // La liste courante est passée à l'écran. POSÉ HORS de la table, pour que même
  // un BeginTable en échec (fenêtre repliée, hauteur nulle) le relève : sans ça,
  // le refine automatique attendrait indéfiniment une cible qui ne viendrait
  // jamais — first_visible_index_ resterait alors à -1 et la chaîne s'arrêterait
  // en le disant, ce qui est le bon échec.
  list_drawn_ = true;
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
      // Différée comme les lignes d'arme (itemcell::FlushDeferredDesc).
      if (GetCursorPos(&pt))
        itemcell::DeferDescById(id, 0, 0, pt.x, pt.y);
    }
  }
}

void WeaponRefineWindow::DrawFooter() {

  // Stock de minerai. Le serveur ne propose une arme que si SON minerai est là :
  // ces trois compteurs disent lesquels manquent, ce que le natif ne montre nulle
  // part. On ne DÉCIDE rien avec — le serveur reste seul juge.
  ImGui::SeparatorText(i18n::Tr("Minerais"));
  DrawOreLinks();

  const int cap = RefineSkillLevel();
  if (cap > 0) {
    ImGui::TextDisabled(i18n::Tr("Plafond : +%d"), cap);
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("Le niveau appris de la compétence Upgrade Weapon EST le plafond : le "
        "serveur refuse toute arme déjà à ce refine (et jamais au-delà de "
        "+10)."));
  }

  // ── La CHANCE de la tentative, à côté du plafond ───────────────────────────
  // 🔴 C'est le seul taux du jeu qu'on puisse annoncer FERMEMENT. Le serveur fait
  //     per = Rate/100 + (classe 3 ? +10 : (job_level - 50) / 2)
  //     succès si per > rnd() % 100
  // et `rnd()%100` étant uniforme sur 0..99, la probabilité de succès EST `per` %.
  // Aucun tirage n'entre dans le calcul — là où `make_per` de la forge contient un
  // `rnd_value(1, 100) * 10`, ce qui interdit d'y afficher autre chose qu'une
  // fourchette. Ici, un chiffre exact est honnête.
  //
  // Les deux données qui manquaient au client (le Rate par niveau d'arme, et le
  // niveau d'arme de l'objet — absent du paquet 0x0221 comme de l'itemInfo)
  // viennent du fichier généré. Sans lui, on se TAIT : afficher 0 % ferait passer
  // une absence pour une certitude.
  const Entry* aimed = nullptr;
  for (const Entry& e : entries_)
    if (e.index == sel_index_) { aimed = &e; break; }

  if (aimed && craftdata::Available()) {
    const int chance = craftdata::RefineChancePercent(
        aimed->nameid, aimed->refine, OwnJobLevel(), false);
    if (chance >= 0) {
      const uint32_t col =
          (chance >= 90) ? kColOk : (chance >= 50 ? kColWarn : kColBad);
      ImGui::SameLine();
      ImGui::TextColored(V4(col), i18n::Tr("· Chances : %d %%"), chance);
      ImGui::SameLine();
      HelpMarker(
          i18n::Tr("Probabilité EXACTE de cette tentative, pas une estimation.\n"
          "\n"
          "Le serveur calcule per = Rate/100 + (job_level - 50) / 2, puis réussit "
          "si per > rnd()%%100. Comme le tirage est uniforme sur 0..99, la "
          "probabilité vaut exactement per.\n"
          "\n"
          "Le Rate de base vient de la table de refine du serveur (niveau d'arme "
          "× refine visé). Un job level inférieur à 50 donne un bonus NÉGATIF : "
          "ce n'est pas une erreur, le serveur fait bien cela."));
    }
  }

  // (Pas de compteur de SP ici : le client en a déjà un, et la chaîne n'a pas
  // besoin d'être annoncée pour s'arrêter — quand le SP manque, le motif d'arrêt
  // s'affiche plus bas, à sa place et au bon moment.)

  if (awaiting_result_) {
    ImGui::TextColored(V4(kColWarn), i18n::Tr("Tentative envoyée — en attente du serveur…"));
    ImGui::Spacing();
  }

  // État de la chaîne automatique. Une action que le client prend de lui-même
  // doit se VOIR pendant qu'elle a lieu, et dire pourquoi elle s'arrête — sans
  // ça le joueur constate juste que sa fenêtre se rouvre ou ne se rouvre plus.
  //
  // ⚠ Le refine automatique passe en ROUGE, pas en bleu comme la relance : la
  // relance ne fait que redemander une liste, celui-ci va DÉTRUIRE une arme si le
  // tirage tombe mal. Deux gravités différentes ne peuvent pas porter la même
  // couleur.
  if (auto_refine_at_) {
    // Le numéro de la tentative QUI VA PARTIR (donc jamais « 0 »), pas le compte
    // de celles qui sont derrière : c'est celle-là que le joueur peut encore
    // arrêter, et c'est la seule qui l'intéresse à cet instant.
    ImGui::TextColored(V4(kColBad), i18n::Tr("Refine automatique imminent… (n° %d)"),
                       auto_refine_count_ + 1);
    ImGui::Spacing();
  } else if (auto_recast_at_) {
    ImGui::TextColored(V4(kColInfo),
                       auto_refine_ ? i18n::Tr("Chaîne automatique… (%d)") : i18n::Tr("Relance automatique… (%d)"),
                       auto_chain_);
    ImGui::Spacing();
  } else if (auto_paused_) {
    ImGui::TextColored(V4(kColWarn),
                       i18n::Tr("Chaîne arrêtée. Un clic sur « Refine » ou « Relancer le "
                       "skill » la reprend."));
    ImGui::Spacing();
  } else if (auto_stop_reason_ && AutoChain()) {
    ImGui::TextColored(V4(kColWarn), "%s", auto_stop_reason_);
    ImGui::Spacing();
  }

  // 🔴 `sel_visible_`, pas `sel_index_ >= 0` : le bouton ne s'arme que sur une
  // arme AFFICHÉE. Sélection perdue (arme détruite, disparue de la liste) ou
  // masquée par le filtre = bouton grisé, jusqu'à ce que le joueur re-désigne une
  // ligne. Rien ne re-cible à sa place sur une action qui détruit l'arme.
  const bool has_sel = sel_visible_;
  // `consumed_` compte comme occupé : la liste est encore à l'écran mais le
  // serveur ne l'honore plus. Un bouton actionnable sur une liste morte
  // enverrait dans le vide — c'est le défaut d'origine, sous une autre forme.
  const bool busy    = awaiting_result_ || consumed_;

  // Le résultat de la dernière tentative, gardé à l'écran tant qu'une nouvelle
  // liste n'est pas arrivée. Il était jusqu'ici relégué au journal de session
  // (opt-in) : sur une chaîne automatique, la seule chose que le joueur voulait
  // lire — « Succès » ou « arme détruite » — était précisément celle qui ne
  // s'affichait pas.
  if (!last_result_.empty() && !awaiting_result_) {
    ImGui::TextColored(V4(last_result_color_), "%s", last_result_.c_str());
    ImGui::Spacing();
  }

  // ── « Session terminée » : seulement si RIEN ne va relancer ────────────────
  // 🔴 Le critère n'est pas « la relance auto est-elle cochée » mais « une relance
  // est-elle ARMÉE » (`auto_recast_at_`). C'est plus juste dans les deux sens :
  //  - relance armée → la liste revient dans quelques centaines de ms, le message
  //    ne ferait que clignoter en annonçant une fin qui n'arrive pas ;
  //  - chaîne STOPPÉE alors que le réglage reste coché (refus serveur, plus
  //    d'arme, fermeture) → `auto_recast_at_` est remis à zéro, et le message
  //    reparaît, ce qu'un test sur le seul réglage aurait empêché.
  const bool relaunch_coming = auto_recast_at_ != 0 || pending_ == kActRecast;
  if (consumed_ && !awaiting_result_ && !relaunch_coming) {
    ImGui::TextDisabled(
        i18n::Tr("Session terminée : le serveur n'autorise qu'un refine par lancement de "
        "la compétence."));
    ImGui::Spacing();
  }

  if (!entries_.empty()) {
    // Dire POURQUOI le bouton est gris, sinon il a juste l'air cassé.
    if (!has_sel && !busy) {
      ImGui::TextDisabled(i18n::Tr("Sélectionne une arme dans la liste."));
      ImGui::Spacing();
    }
    ImGui::BeginDisabled(!has_sel || busy);
    if (ro::RoButton(i18n::Tr("Refine"), kBtnRefineW)) RequestRefine(sel_index_);
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
  // `|| consumed_` : la liste reste affichée après une tentative, mais elle est
  // morte côté serveur — c'est exactement le moment où « Relancer » doit
  // apparaître. L'exclusivité avec « Refine » est préservée, puisque celui-ci est
  // grisé par `busy` dès que `consumed_`.
  // Et le bouton suit la même règle : inutile de proposer un geste que la relance
  // automatique est en train de faire — il n'aurait pas le temps d'être cliqué et
  // ne ferait que clignoter. Il reparaît dès que la chaîne s'arrête.
  //
  // ⚠ Et il cède sa place au bouton d'ARRÊT quand une chaîne automatique tourne.
  // Les deux ne peuvent pas être utiles en même temps (relancer à la main pendant
  // que le client relance tout seul n'a aucun sens), et la largeur de la fenêtre
  // est calée sur TROIS boutons — en ajouter un quatrième le ferait déborder.
  //
  // La chaîne « tourne » dès qu'une de ses étapes est en cours, échéance armée
  // comme action posée : c'est exactement pendant ces creux que le joueur veut
  // pouvoir l'arrêter, et un bouton qui clignote entre deux tours ne serait pas
  // cliquable.
  const bool chain_running =
      auto_refine_ && !auto_paused_ &&
      (auto_refine_at_ != 0 || auto_recast_at_ != 0 || awaiting_result_ ||
       pending_ == kActRefine || pending_ == kActRecast);

  if (chain_running) {
    if (ro::RoButton(i18n::Tr("Arrêter"), kBtnRecastW)) {
      // ⚠ On ne touche PAS à `auto_refine_`, qui est le RÉGLAGE persistant : un
      // bouton de fenêtre ne décoche pas une case du panneau d'options. C'est
      // `auto_paused_` qui tient la chaîne, jusqu'à un geste manuel.
      auto_paused_      = true;
      auto_refine_at_   = 0;
      auto_recast_at_   = 0;
      // Une action POSÉE n'est pas encore partie : on peut encore la retirer.
      // Une tentative déjà envoyée (`awaiting_result_`), elle, ira à son terme —
      // le serveur a l'arme, on ne la reprend pas. La chaîne s'arrêtera à son
      // résultat, ScheduleAutoRecast testant `auto_paused_`.
      if (pending_ == kActRefine || pending_ == kActRecast) pending_ = kActNone;
      auto_stop_reason_ = nullptr;
      PushLog(i18n::Tr("Chaîne automatique arrêtée à la demande."), kColWarn);
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip(
          i18n::Tr("Arrête le refine automatique tout de suite.\n"
          "\n"
          "Une tentative DÉJÀ envoyée ira à son terme : le serveur a l'arme,\n"
          "elle ne se reprend pas. Rien ne repartira ensuite.\n"
          "\n"
          "Le réglage reste coché : un clic sur « Refine » ou « Relancer le\n"
          "skill » reprend la chaîne."));
    }
    ImGui::SameLine();
  } else if ((entries_.empty() || consumed_) && !relaunch_coming) {
    ImGui::BeginDisabled(awaiting_result_);
    if (ro::RoButton(i18n::Tr("Relancer le skill"), kBtnRecastW)) {
      pending_ = kActRecast;
      // Relance MANUELLE : nouvelle chaîne, compteurs remis à zéro — y compris les
      // essais de relance, sinon un blocage précédent laisserait le quota épuisé.
      auto_chain_       = 0;
      auto_recast_at_   = 0;
      auto_stop_reason_ = nullptr;
      recast_retries_   = 0;
      // …et c'est le geste manuel qui LÈVE l'arrêt demandé plus tôt : reprendre la
      // main, c'est reprendre la chaîne. Le réglage n'a jamais été décoché.
      auto_paused_      = false;
    }
    ImGui::EndDisabled();
    // Infobulle SUR le bouton (pas un « (?) » à côté) : c'est le bouton qui a
    // besoin d'être expliqué, et il est déjà à l'étroit dans le pied.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      ImGui::SetTooltip(
          i18n::Tr("Relance la compétence Upgrade Weapon pour obtenir une nouvelle liste.\n"
          "\n"
          "Le serveur n'autorise QU'UNE tentative par lancement : après chaque\n"
          "refine il faut relancer, et c'est ce que fait ce bouton — sans\n"
          "repasser par la barre d'action."));
    }
    ImGui::SameLine();
  }

  if (ro::RoButton(i18n::Tr("Fermer"), kBtnCloseW)) RequestClose();
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
      i18n::Tr("Remplace la fenêtre « Upgradeable weapons » du skill Upgrade Weapon."));
  ImGui::TextDisabled(
      i18n::Tr("Clic droit : description · double-clic ou Entrée : refine."));
  ImGui::TextDisabled(
      i18n::Tr("En-têtes de colonne : trier (3e clic = ordre d'inventaire)."));
  changed |= ro::RoCheckbox(i18n::Tr("Confirmer avant un refine"), &confirm_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Un échec DÉTRUIT l'arme, et le client affiche le MÊME message pour un "
      "succès et un échec. La confirmation rappelle l'arme visée et le risque."));
  changed |= ro::RoCheckbox(i18n::Tr("Cartes et emplacements"), &show_cards_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Le paquet du serveur porte les 4 cartes de chaque arme — la fenêtre "
      "native les jette, deux armes identiques dont une sertie y sont donc "
      "indistinguables."));
  changed |= ro::RoCheckbox(i18n::Tr("Champ de filtre"), &show_filter_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Utile sur un gros inventaire ; sur deux ou trois armes il ne fait que "
      "prendre une ligne. Le décocher efface aussi le filtre en cours, pour "
      "qu'aucune arme ne reste masquée par un champ invisible."));
  changed |= ro::RoCheckbox(i18n::Tr("Description au survol"), &desc_tooltip_);

  changed |= ro::RoCheckbox(i18n::Tr("Entrée lance le refine"), &enter_key_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Cochée, Entrée refine l'arme sélectionnée, et la maintenir enchaîne. La "
      "fenêtre confisque alors la touche tant qu'elle est ouverte : impossible "
      "d'ouvrir la saisie du chat.\n"
      "\n"
      "Décoché (défaut), la touche Entrée reste au CHAT.\n"
      "\n"
      "La fenêtre de CONFIRMATION garde Entrée dans tous les cas : « Entrée = "
      "OK » y est la convention, et elle valide une action qui peut détruire "
      "l'arme."));

  changed |= ro::RoCheckbox(i18n::Tr("Relancer la compétence automatiquement"),
                            &auto_recast_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Après chaque tentative, relance Upgrade Weapon pour rouvrir la liste — "
      "le serveur n'en autorise qu'une par lancement.\n"
      "\n"
      "Ne refine RIEN tout seul : le choix de l'arme et le déclenchement "
      "restent des clics. La chaîne s'arrête d'elle-même quand la liste revient "
      "vide, quand il n'y a plus de minerai, ou quand le serveur refuse une "
      "condition.\n"
      "\n"
      "Si la compétence est jetée par son délai de lancement, la relance est "
      "réessayée un peu plus tard (3 fois au plus) au lieu de s'arrêter en "
      "silence."));

  // ── La seule option du plugin qui AGISSE à la place du joueur ──────────────
  // Elle est donc présentée comme telle : l'avertissement est SOUS la case, en
  // rouge, et pas caché dans une infobulle qu'on peut ne jamais ouvrir. Un joueur
  // doit pouvoir mesurer ce qu'il coche sans avoir à survoler quoi que ce soit.
  changed |= ro::RoCheckbox(i18n::Tr("Refiner automatiquement (chaîne complète)"),
                            &auto_refine_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Enchaîne TOUT SEUL : la première arme de la liste est jouée, la "
      "compétence relancée, et ainsi de suite jusqu'à ne plus pouvoir — c'est "
      "le SP qui borne la chaîne, et elle s'arrête quand il manque.\n"
      "\n"
      "Implique la relance automatique (sans elle le serveur n'enverrait plus de "
      "liste). La confirmation est IGNORÉE : une chaîne qui demande son accord à "
      "chaque tour n'en est pas une.\n"
      "\n"
      "L'arme visée est celle qui est sélectionnée si elle est encore là (on "
      "continue donc de monter la même), sinon la première ligne AFFICHÉE — "
      "filtre et tri compris. Rien n'est joué avant que la liste n'ait été "
      "affichée au moins une fois.\n"
      "\n"
      "S'arrête seule : plus de SP, plus de minerai, plus d'arme dans la liste, "
      "ou refus du serveur. Un bouton « Arrêter » apparaît dans la fenêtre "
      "pendant toute la chaîne."));
  if (auto_refine_) {
    ImGui::Indent();
    ImGui::PushStyleColor(ImGuiCol_Text, V4(kColBad));
    ImGui::TextWrapped(
        i18n::Tr("Chaque tentative peut DÉTRUIRE l'arme, et elles partent sans "
        "confirmation. La chaîne joue les armes de la liste jusqu'à épuisement "
        "du SP."));
    ImGui::PopStyleColor();
    ImGui::Unindent();
  }

  changed |= ro::RoCheckbox(i18n::Tr("Journal de session"), &show_history_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Garde la trace des tentatives de la session, avec le libellé EXACT du "
      "serveur. N'apparaît qu'à partir du premier résultat — avant, il n'y a "
      "rien à montrer. C'est le seul endroit où succès et échec se distinguent : "
      "le client leur donne le MÊME texte (MsgString 911 et 912)."));
  if (show_history_) {
    ImGui::Indent();
    changed |= ro::RoCheckbox(i18n::Tr("Horodater les lignes"), &log_time_);
    ImGui::Unindent();
  }
  return changed;
}
