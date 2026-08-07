#include "features/windows/make_item_window.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bourgeon.h"
#include "features/craft_data.h"  // WeaponLevel : « ce produit est-il un équipement ? »
#include "features/item_cell.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/systems/bourgeon_opcodes.h"  // kCookMastery (ZC 0x0F1C)
#include "features/windows/item_desc_window.h"
// CloseForOtherCraft : le serveur n'a qu'un `menuskill`, notre liste évince la sienne.
#include "features/windows/weapon_refine_window.h"
#include "imgui.h"
#include "ragnarok/globals.h"
#include "ragnarok/msgstring.h"  // msgstr:: (libellés natifs du client)
#include "ragnarok/player_skills.h"    // LearnedSkillLevel (bonus de maîtrise)
#include "ragnarok/ragnarok_client.h"  // UseItemById (relance par OBJET)
#include "ragnarok/uiwnd.h"
#include "ui/icon_cache.h"
// (Le module ui/native_modal a été SUPPRIMÉ : la modale « liste vide » venait du
// handler NATIF du 0x018D, qui ne tourne plus. Cf. docs §3.1 bis, qui conserve le
// mécanisme au cas où.)
#include "ui/ro_imgui.h"
#include "utils/log_console.h"  // LogDiag — journal staff en jeu
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000) ────────────────────────────
// Tout est établi et justifié dans docs/make_item_list_re.md ; on ne recopie ici
// que ce qui sert, avec le § qui l'explique.
namespace {

// Les deux fenêtres natives de LISTE qu'on remplace (§4 et §5). On vérifie la
// VTABLE en plus de l'id : un id ne garantit pas la classe si un portage
// renumérote les fenêtres.
constexpr int       kWinMakingArrow  = 94;          // UIMakingArrowListWnd « LIST »
constexpr uintptr_t kVTableMakingArrow = 0x010345ac;
constexpr int       kWinMakeTarget   = 79;          // UIMakeTargetListWnd
constexpr uintptr_t kVTableMakeTarget  = 0x0103ec50;
// La fenêtre 80 reste NATIVE — on ne fait que la déclencher (§6, et l'en-tête).
// ⚠ Gardées pour la DOCUMENTATION et pour le masquage de sécurité, plus pour un
// appel : on n'ouvre plus jamais la 80 (cf. SendConfirm). `kMsgSetProduct` était
// la seule chose qu'on lui envoyait.
constexpr int       kWinMakeProcess  = 80;          // UIMakeTargetProcessWnd
constexpr int       kVTableMakeProcess = 0x0103eed8;  // ⏱ confirmé sur 2 instances
constexpr int       kMsgSetProduct   = 40;          // OnMsg(40, itemId) de la 80

// Produits SANS emplacement de matériau optionnel. Ce n'est pas une table de
// gameplay inventée ici : c'est la borne exacte du test de
// `UIMakeTargetProcessWnd::OnDraw` (`(unsigned)(produit - 994) > 6`), c'est-à-dire
// la fonte de métaux — Flame Heart 994, Mystic Frozen 995, Rough Wind 996,
// Great Nature 997, Iron 998, Steel 999, Star Crumb 1000.
constexpr uint32_t kNoSlotProductFirst = 994;
constexpr uint32_t kNoSlotProductLast  = 1000;

// ── Matériaux optionnels de forge ─────────────────────────────────────────────
// Ce ne sont PAS des données de gameplay inventées ici : ce sont les constantes
// que `skill_produce_mix` teste nommément (`src/map/itemdb.hpp`), au même titre
// qu'un opcode. Le serveur ne consomme QUE celles-là parmi les trois
// emplacements ; tout autre objet y serait ignoré.
//   - Star Crumb  : jusqu'à 3, `sc++`, inscrit dans `card[1] = ((sc*5) << 8) + ele` ;
//   - pierre élémentaire : la PREMIÈRE seulement (`ele == 0` en garde).
// Les NOMS, eux, ne sont jamais codés : ils viennent de la DB client.
constexpr uint32_t kMatStarCrumb   = 1000;  // ITEMID_STAR_CRUMB
constexpr uint32_t kMatElemFirst   = 994;   // ITEMID_FLAME_HEART
constexpr uint32_t kMatElemLast    = 997;   // ITEMID_GREAT_NATURE
constexpr int      kMaxStarCrumb   = 3;
// Pénalité de réussite par Star Crumb : `make_per -= sc * 1500` sur une échelle
// de 100000, soit −15 % chacun (skill.cpp, §7.3). Le natif ne le dit NULLE PART,
// alors que son écran invite à remplir les trois emplacements.
constexpr int      kStarCrumbMalusPercent = 15;
// Gain d'attaque par Star Crumb : `card[1] = ((sc * 5) << 8) + ele` (§7.4).
constexpr int      kStarCrumbAtkBonus = 5;
// La pierre élémentaire coûte 25 points de réussite (`if (ele) make_per -= 2500`).
// Le natif ne le dit pas — et ma première rédaction l'affichait en VERT, comme si
// elle était gratuite. C'est le plus lourd des deux malus.
constexpr int      kElementMalusPercent = 25;

// ── Enclumes ─────────────────────────────────────────────────────────────────
// Ce ne sont PAS des matériaux : elles n'entrent dans aucune recette et ne sont
// pas consommées. Il suffit de les POSSÉDER — `pc_search_inventory` — et elles
// ajoutent au taux de réussite. Le serveur ne retient que la MEILLEURE (chaîne de
// `else if`), donc en cumuler plusieurs ne sert à rien : information invisible
// partout ailleurs, alors qu'elle décide d'un forge raté ou réussi.
// Constantes serveur (itemdb.hpp), du meilleur au moins bon.
//
// `percent` est pour l'AFFICHAGE, `raw` pour le CALCUL : sur l'échelle de make_per
// (10000 = 100 %), l'enclume d'Oridecon vaut 250, soit 2,5 % — un nombre que le
// pourcentage arrondi ne peut pas porter. Les deux champs coexistent pour cette
// seule raison.
struct AnvilBonus { uint32_t id; int percent; int raw; };
constexpr AnvilBonus kAnvils[] = {
    {989, 10, 1000},  // ITEMID_EMPERIUM_ANVIL : +10 %
    {988,  5,  500},  // ITEMID_GOLDEN_ANVIL   : +5 %
    {987,  2,  250},  // ITEMID_ORIDECON_ANVIL : +2,5 % (arrondi à l'affichage)
    {986,  0,    0},  // ITEMID_ANVIL          : +0 %
};

// Ce produit ouvre-t-il la fenêtre native 80 ? Dit UNE fois, parce que trois
// endroits en dépendent et qu'ils doivent répondre pareil : l'envoi (SendConfirm),
// l'attente d'un résultat serveur (RequestMake) et la fermeture de notre fenêtre
// (FlushPending). Les laisser tester chacun de leur côté, c'est se garantir qu'un
// jour l'un des trois sera oublié.
// Compétences de FORGE D'ARME — plage CONTIGUË `BS_DAGGER`..`BS_SPEAR`, établie
// sur les commentaires de `db/pre-re/produce_db.txt` : BS_DAGGER 98, BS_SWORD 99,
// BS_TWOHANDSWORD 100, BS_AXE 101, donc BS_MACE 102, BS_KNUCKLE 103, BS_SPEAR 104.
// Ce sont les seules dont le produit est un ÉQUIPEMENT, et donc les seules où les
// matériaux optionnels servent à quelque chose.
constexpr int kSkillForgeFirst = 98;
constexpr int kSkillForgeLast  = 104;

// ⏱ `AC_MAKINGARROW` = 147 (db/pre-re/skill_db.yml). C'est la SEULE compétence
// dont la liste énumère des MATÉRIAUX à transformer ; toutes les autres listent
// des produits. La distinction n'est pas cosmétique : le fichier de recettes est
// une table unique indexée par id, et un même id peut être à la fois produit de
// forge et source de flèches (⏱ constaté en jeu : la liste de flèches affichait
// des recettes de forge). C'est donc la LISTE OUVERTE qui doit décider quelle
// forme de recette est valide, et le seul discriminant fiable est la compétence.
constexpr int kSkillMakingArrow = 147;

// Renommée depuis `ProductUsesNativeSlots` : plus rien ne « passe au natif », la
// question est devenue « ce produit accepte-t-il des matériaux OPTIONNELS ? ».
// Elle ne commande donc que l'AFFICHAGE des trois emplacements, plus l'envoi.
//
// 🔴 Le critère du natif NE SUFFIT PAS, et le laisser tel quel était dangereux :
// `skill_produce_mix` boucle sur les trois emplacements et fait `pc_delitem` sur
// tout Star Crumb ou pierre élémentaire qu'il y trouve — AVANT de savoir à quoi
// ils serviront. Or `sc` et `ele` ne sont exploités que dans la branche
// ÉQUIPEMENT (`card[1] = ((sc*5) << 8) + ele`). Un Star Crumb posé sur une potion
// est donc DÉTRUIT pour rien. Le natif offre pourtant ces emplacements sur toute
// fabrication hors fonte de métaux, pharmacie comprise : un défaut à ne pas
// recopier.
bool ProductAcceptsForgeSlots(uint32_t id, int skill_id, bool from_item) {
  // Fonte de métaux : la fenêtre native elle-même ne dessine aucun emplacement.
  if (id >= kNoSlotProductFirst && id <= kNoSlotProductLast) return false;
  // Compétence connue : on tranche sur ELLE, seule source fiable de « le produit
  // est-il un équipement ». La pharmacie et les convertisseurs n'en veulent pas.
  if (!from_item)
    return skill_id >= kSkillForgeFirst && skill_id <= kSkillForgeLast;
  // Liste ouverte par un SCRIPT D'OBJET (marteau de forge…) : la compétence
  // qu'il émule n'est nulle part — `menuskill_id` vaut -1 côté serveur. On
  // retombe alors sur la règle du natif, faute de mieux. Le cas est bénin : ces
  // objets-là produisent justement des armes.
  return true;
}

constexpr int kWndVisible = 0x28;  // UIWindow : flag « visible »

// CMode::SendMsg : le dispatcher du mode actif, vtable+0x18. On rejoue les
// chemins natifs plutôt que de fabriquer les paquets — règle du projet, et ici
// ça évite en prime de dupliquer trois constructions d'en-tête différentes.
constexpr int kVfDispCmd = 0x18;
constexpr int kCmdProduce   = 130;  // { id, ItemSkillInfo[3] } -> CZ_REQMAKINGITEM   0x018E
constexpr int kCmdMakeArrow = 153;  // { id }                   -> CZ_REQ_MAKINGARROW 0x01AE
constexpr int kCmdMakeItem  = 207;  // { id, mk_type }          -> CZ_REQ_MAKINGITEM  0x025B

// Piloter les BOUTONS d'une fenêtre native : `OnMsg(6, id)` est un clic réel, reçu
// par leur `case 6`. Les trois fenêtres partagent les mêmes identifiants (§4.6,
// §5.3, §6) — ce sont ceux du code natif, pas des suppositions :
//   184 = OK       185 = Annuler
//
// 🔴 L'Annuler est infiniment préférable à une fermeture pour la fenêtre 80 : c'est
// LUI qui re-crédite les matériaux déjà posés dans ses emplacements, un par un
// (`Inventory_AddOrStackItem`). La détruire les perdrait.
constexpr int kMsgUiAction = 6;
constexpr int kBtnCancelId = 185;

// ── Traitement des métaux : les trois compétences dont on sait calculer la chance ──
// Ce sont les `req_skill` que porte notre YAML de recettes (donc le `produce_db` du
// serveur), et ce sont eux que `skill_produce_mix` retrouve via
// `if (!skill_id) skill_id = skill_produce_db[idx].req_skill;`.
constexpr int kSkillIronTempering  = 94;  // BS_IRON
constexpr int kSkillSteelTempering = 95;  // BS_STEEL
constexpr int kSkillEnchantedStone = 96;  // BS_ENCHANTEDSTONE
// (Star Crumb : `kMatStarCrumb` existe déjà plus haut — le serveur force
// `make_per = 100000` sur ce produit, soit une réussite certaine.)

// Les deux compétences PASSIVES qui bonifient la forge d'arme, en plus du métier.
// Ids relevés dans db/pre-re/skill_db.yml.
constexpr int kSkillWeaponResearch   = 107;  // BS_WEAPONRESEARCH : +1 % par niveau
constexpr int kSkillOrideconResearch = 97;   // BS_ORIDEOCON : +1 %/niv, armes lv >= 3,
                                             // et SEULEMENT si oridecon_research_fix

// ── Pharmacy ────────────────────────────────────────────────────────────────
constexpr int kSkillPharmacy        = 228;  // AM_PHARMACY : +3 % par niveau (!)
constexpr int kSkillLearningPotion  = 227;  // AM_LEARNINGPOTION : +0,5 % par niveau
// Bonus/malus PAR PRODUIT du `switch (nameid)` de la branche AM_PHARMACY. Ce sont
// les seuls termes aléatoires de cette formule — il n'y a pas de tirage de base ici,
// contrairement aux métaux et à la forge.
//   `base` s'ajoute tel quel ; `roll_max` est l'amplitude du tirage `(1+rnd%N)*10`,
//   compté NÉGATIVEMENT quand `subtract` est vrai.
struct PotionBonus { uint32_t id; int base; int roll_max; bool subtract; };
constexpr PotionBonus kPotionBonuses[] = {
    {501,  2000, 100, false},  // Red Potion
    {503,  2000, 100, false},  // Yellow Potion
    {504,  2000, 100, false},  // White Potion
    {970,  1000, 100, false},  // Alcohol
    {7135,    0, 100, false},  // Fire Bottle
    {7136,    0, 100, false},  // Acid Bottle
    {7137,    0, 100, false},  // Man Eater Bottle
    {7138,    0, 100, false},  // Mini Bottle
    {546,     0,  50, true },   // Yellow Slim Potion : -(1+rnd%50)*10
    {547,     0, 100, true },   // White Slim Potion  : -(1+rnd%100)*10
    {7139,    0, 100, true },   // Coating Bottle     : idem
};
using DispCmd_t = void(__thiscall*)(void*, int, int, int, int, int);

// Modèle SESSION de l'inventaire : la std::list que le client tient à jour quel
// que soit l'état de ses fenêtres. Même source qu'InventoryViewer.
constexpr uintptr_t kInvListHead = 0x015fbab0;
constexpr int kNodeNext   = 0x00;
constexpr int kNodeInfo   = 0x08;
constexpr int kNodeAmt    = 0x18;
constexpr int kInfoIdStr  = 0x2c;  // std::string de l'id — le jeu fait atoi dessus (§4.4)
constexpr int kInfoIdCap  = 0x40;  // capacité SSO de cette std::string (+0x2c + 0x14)
constexpr int kMaxInvNodes = 4096; // garde-fou de parcours

// ItemSkillInfo : ctor/dtor natifs. Nécessaires UNIQUEMENT pour la commande 130,
// qui attend un tableau de TROIS structures (§3.4). On les construit vides : le
// natif en fait `atoi("")` = 0, c'est-à-dire « aucun matériau optionnel », ce que
// le natif lui-même envoie pour les jobs qui court-circuitent la fenêtre 80.
constexpr uintptr_t kItemSkillInfoCtor = 0x006a1b20;
constexpr uintptr_t kItemSkillInfoDtor = 0x005a4300;
constexpr size_t    kItemSkillInfoSize = 0xf8;
using InfoCtor_t = void*(__thiscall*)(void*);
using InfoDtor_t = void (__thiscall*)(void*);

// MsgStringTable : on affiche les libellés EXACTS du client, jamais une
// paraphrase (règle du projet). Conversion CP949 -> UTF-8 dans msgstr::Utf8.
constexpr int kMsgCantMakeItem = 424;  // MSI_CANT_MAKE_ITEM
constexpr int kMsgMakeList     = 425;  // MSI_MAKE_LIST « Manufacturing List »
constexpr int kMsgRequireForMake = 427;  // MSI_REQUIRE_FOR_MAKE_TARGET « 's required materials »
constexpr int kMsgMakeFail     = 430;  // MSI_MAKE_TARGET_FAIL_MSG
constexpr int kMsgMakeSuccess  = 431;  // MSI_MAKE_TARGET_SUCCEESS_MSG

// Opcodes observés (le handler natif continue de tourner : on ne fait que lire).
constexpr uint16_t kOpMakableList  = 0x018d;  // ZC_MAKABLEITEMLIST   (VARIABLE) -> fen. 79
constexpr uint16_t kOpMakeResult   = 0x018f;  // ZC_ACK_REQMAKINGITEM (fixe, 8)
constexpr uint16_t kOpArrowList    = 0x01ad;  // ZC_MAKINGARROW_LIST  (VARIABLE) -> fen. 94
constexpr uint16_t kOpMakingList   = 0x025a;  // ZC_MAKINGITEM_LIST   (VARIABLE) -> fen. 94
constexpr uint16_t kOpSkillFail    = 0x0110;  // ZC_ACK_TOUSESKILL

// Octets transmis à OnRecvPacket pour les paquets FIXES : la longueur du paquet
// MOINS son opcode, que RegisterObserveOpcode a déjà consommé.
constexpr uint16_t kMakeResultLen = 6;   // 8 - 2 : u16 result + u32 itemId
constexpr uint16_t kSkillFailLen  = 12;

// Tailles d'entrée des trois listes (§3.1 à §3.3).
constexpr int kEntryMakable = 16;  // { u32 itemId ; u32 material[3] } — les 3 sont à ZÉRO
constexpr int kEntrySimple  = 4;   // { u32 itemId }
constexpr int kMaxEntries   = 512; // garde-fou

// Intervalle minimal entre deux demandes, tous gestes confondus. Une touche
// maintenue répète à la cadence du clavier — bien plus vite qu'un aller-retour
// serveur.
constexpr unsigned kMinSendIntervalMs = 300;
// Fenêtre pendant laquelle un ZC_ACK_TOUSESKILL est considéré comme la réponse à
// NOTRE demande. C'est une borne temporelle faute de mieux : l'id de compétence
// n'est dans aucun des paquets de liste (cf. le parseur de kOpSkillFail).
constexpr unsigned kSkillFailWindowMs = 3000;
// Laisse passer le délai de cast avant de relancer la compétence.
//
// ⏱ 400 ms côté REFINE après réglage en jeu : à 300 la chaîne se coupait par moments
// avec Entrée maintenue. Et le facteur qu'on oublie, c'est qu'`OnTick` est limité à
// ~100 ms : l'échéance vaut donc « valeur + 0 à 100 ms ». Si la même coupure apparaît
// ici, c'est le premier levier à remonter (cf. weapon_refine_window.cc).
constexpr unsigned kAutoRecastDelayMs = 300;
// 🔴 Chien de garde de la RELANCE : la compétence (ou l'objet) est partie et AUCUNE
// liste ne revient. Le serveur peut la jeter sur son délai de lancement, et le chemin
// natif peut la refuser côté client — dans les deux cas personne ne nous l'apprend.
// Sans ce garde-fou la chaîne s'arrête SANS UN MOT, fenêtre grisée, alors que toutes
// les conditions sont réunies. ⏱ Constaté en jeu sur le refine, avec Entrée maintenue.
constexpr unsigned kRelaunchNoListMs  = 1000;
constexpr int      kMaxRelaunchRetries = 3;
// Délai maximal entre un lancement de compétence observé et la liste qu'il
// provoque. Au-delà, c'est qu'aucune compétence n'est en cause : la liste vient
// d'un script d'OBJET (Mini Furnace, marteaux — `produce N;`).
constexpr unsigned kSkillCastWindowMs = 3000;
// Idem côté OBJET : au-delà, le dernier CZ_USE_ITEM observé n'a rien à voir avec
// la liste qui arrive (une potion bue entre-temps, par exemple) et ne doit
// surtout pas devenir la cible d'une relance.
constexpr unsigned kItemUseWindowMs = 3000;
// 🔴 Délai au-delà duquel on cesse d'attendre le serveur. Ce n'est PAS une
// précaution théorique : `skill_produce_mix` fait `return false` SANS émettre le
// moindre paquet quand `skill_can_produce_mix` échoue à la revalidation, et
// `clif_parse_SelectArrow` ignore cette valeur de retour (skill.cpp:13383,
// clif.cpp:15826). Le serveur peut donc légitimement ne RIEN répondre.
// Le natif ne le voyait pas — il referme sa fenêtre à l'envoi ; la nôtre lui
// survit, et restait « en attente du serveur… » pour toujours.
constexpr unsigned kAwaitResultTimeoutMs = 6000;
// Délai avant de CONSTATER le résultat dans l'inventaire faute de paquet. Assez
// long pour laisser passer un vrai 0x018F (qui arrive en quelques dizaines de
// ms), assez court pour que la chaîne reste fluide.
constexpr unsigned kNoAnswerProbeMs = 300;
// Produits cités au plus dans le diagnostic de liste vide. Borné : la liste
// précédente peut en compter trente, et un mur de texte ne se lit pas.
constexpr int kMaxStaleShown = 8;
// Délai avant de RÉ-UTILISER l'objet. Plus long que la relance de compétence :
// le serveur vient de supprimer l'exemplaire précédent et d'envoyer la mise à
// jour d'inventaire ; ré-utiliser trop tôt ferait résoudre l'identifiant sur un
// inventaire encore périmé.
constexpr unsigned kAutoReuseDelayMs = 300;

// Notre propre GID = notre AID : toutes ces compétences se lancent sur soi.
constexpr uintptr_t kOwnAccountId = 0x015fb9a4;
constexpr int       kCmdUseSkill  = 0x45;  // { skillId, cibleGID, niveau }

uint32_t OwnAid() {
  __try {
    return *reinterpret_cast<const uint32_t*>(kOwnAccountId);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ── Palette ──────────────────────────────────────────────────────────────────
// Le corps d'une fenêtre RO est CLAIR : tout ce qui est coloré ici est SATURÉ et
// SOMBRE, sinon ça se délave (leçon du plugin de refine).
constexpr ImU32 kColOk   = IM_COL32( 13, 107,  31, 255);
constexpr ImU32 kColBad  = IM_COL32(166,  38,  38, 255);
constexpr ImU32 kColWarn = IM_COL32(166, 102,   0, 255);
constexpr ImU32 kColDim  = IM_COL32(110, 110, 110, 255);

// Curseur « main » RO. `ImGui::SetMouseCursor` est un no-op dans ce client
// (io.ConfigFlags porte NoMouseCursorChange) : il FAUT passer par
// ro::SetHoverCursor, qui demande une valeur de *(CursorMgr+0x50).
constexpr int kRoCursorHand = 2;

// Côté de l'icône dans la liste. Le natif dessine le bmp 1:1 (~24 px) ; on garde
// la même échelle pour que l'œil retrouve les mêmes objets.
constexpr float kIconSize = 24.0f;

// Largeur des deux boutons du pied. Fixée pour qu'ils s'alignent quelle que soit
// la longueur du libellé — « Fabriquer » et « Fermer » n'ont pas la même mesure.
constexpr float kBtnW = 78.0f;

// Largeur de la fenêtre, épinglée : seule la hauteur suit le contenu.
constexpr float kWindowW = 320.0f;

// Icône d'un matériau dans la recette : plus petite que celle des produits, la
// ligne y est du texte et non une cellule cliquable de tableau.
constexpr float kMatIcon = 18.0f;

// ImU32 -> ImVec4, pour les widgets qui prennent une couleur flottante.
inline ImVec4 V4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

// Envoie une commande au dispatcher du mode actif (le chemin des boutons natifs).
void SendModeCmd(int cmd, int a, int b = 0, int c = 0, int d = 0) {
  __try {
    void* mode = rag::ActiveMode();
    if (!mode) return;
    auto fn = reinterpret_cast<DispCmd_t>(
        (*reinterpret_cast<uintptr_t**>(mode))[kVfDispCmd / 4]);
    fn(mode, cmd, a, b, c, d);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Commande 130 : elle attend un TABLEAU de trois ItemSkillInfo et en lit le
// champ +0x2c par atoi (§3.4). On les construit vides -> trois matériaux à 0.
//
// ⚠ Le tableau vit sur la PILE et le natif ne le garde pas : la commande est
// synchrone (elle construit et envoie le paquet avant de rendre la main).
// ⏱ `ItemSkillInfo_SetId(void* this, int id)` — __thiscall, écrit `itoa(id)` dans
// la std::string à +0x2C (décompilé : `std_string_assign(this + 44, itoa(id))`).
// C'est LE point qui rend la fenêtre native 80 inutile : les trois matériaux
// optionnels de forge voyagent en PARAMÈTRE de la commande 130, ils ne sont pas
// lus dans cette fenêtre.
constexpr uintptr_t kItemSkillInfoSetId = 0x006a6570;
using InfoSetId_t = int(__thiscall*)(void*, int);

// `mats` : trois identifiants d'objet, 0 = emplacement vide. Exactement ce que la
// 79 envoie pour les Rune Knight (trois ItemSkillInfo par défaut) et ce que la 80
// envoie après remplissage — même paquet, même commande, sans la fenêtre.
void SendProduceCmd(int item_id, const uint32_t mats[3] = nullptr) {
  __try {
    void* mode = rag::ActiveMode();
    if (!mode) return;
    alignas(8) unsigned char slots[kItemSkillInfoSize * 3];
    auto ctor = reinterpret_cast<InfoCtor_t>(kItemSkillInfoCtor);
    auto dtor = reinterpret_cast<InfoDtor_t>(kItemSkillInfoDtor);
    auto set_id = reinterpret_cast<InfoSetId_t>(kItemSkillInfoSetId);
    for (int i = 0; i < 3; ++i) ctor(slots + i * kItemSkillInfoSize);
    // Remplissage APRÈS construction : SetId fait un std::string::assign, il lui
    // faut une chaîne déjà initialisée.
    if (mats)
      for (int i = 0; i < 3; ++i)
        if (mats[i]) set_id(slots + i * kItemSkillInfoSize,
                            static_cast<int>(mats[i]));
    auto fn = reinterpret_cast<DispCmd_t>(
        (*reinterpret_cast<uintptr_t**>(mode))[kVfDispCmd / 4]);
    fn(mode, kCmdProduce, item_id,
       static_cast<int>(reinterpret_cast<intptr_t>(slots)), 0, 0);
    for (int i = 2; i >= 0; --i) dtor(slots + i * kItemSkillInfoSize);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Id d'objet d'un ItemSkillInfo. ⚠ Il est stocké en TEXTE décimal dans une
// std::string à +0x2c (§4.4) : SSO tant que la capacité tient dans 15.
uint32_t InfoItemId(const void* info) {
  __try {
    const auto base = reinterpret_cast<uintptr_t>(info);
    const auto cap  = *reinterpret_cast<const uint32_t*>(base + kInfoIdCap);
    const char* s = (cap >= 16)
                        ? *reinterpret_cast<const char* const*>(base + kInfoIdStr)
                        : reinterpret_cast<const char*>(base + kInfoIdStr);
    if (!s) return 0;
    return static_cast<uint32_t>(std::atoi(s));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Quantité totale d'un objet en inventaire, lue dans le MODÈLE SESSION (et non
// dans la liste d'affichage d'une fenêtre : cacher le natif vide la seconde,
// jamais le premier).
int OwnedCount(uint32_t item_id) {
  int total = 0;
  __try {
    // 🔴 `kInvListHead` est l'adresse du GLOBAL ; la SENTINELLE est ce qu'il
    // contient. Écrit d'abord `node != kInvListHead`, ce parcours ne rencontrait
    // jamais sa condition d'arrêt : la liste circulaire rebouclait et resommait
    // l'inventaire jusqu'au garde-fou des 4096 nœuds — d'où un « possédé » à
    // 28300 pour 200 objets réels. La sentinelle est le seul repère valide.
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    int guard = 0;
    while (node && node != head && guard++ < kMaxInvNodes) {
      const uint8_t* info = node + kNodeInfo;
      const int amount = *reinterpret_cast<const int*>(node + kNodeAmt);
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      if (amount > 0 && InfoItemId(info) == item_id) total += amount;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return total; }
  return total;
}

// ── Index d'inventaire ⇄ identifiant ─────────────────────────────────────────
// 🔴 NE PAS passer par `Session::GetItemInfoById` / `RagnarokClient::UseItemById`
// pour ça. Leur `item_list_` est déclaré « LIKELY » dans
// object_layouts/session/20250716.h : l'offset +0x16D8 n'a jamais été confirmé
// sur ce build, et il est FAUX. La chaîne entière était du code mort (aucun
// appelant), donc personne n'avait payé l'erreur ; le premier usage réel a fait
// planter le client sur une tête de liste nulle (`mov eax,[esi]`, esi = 0).
// On repasse donc par le MÊME parcours que la colonne « Possédé », lui vérifié
// en jeu, et par le global qui est la vraie tête d'inventaire.
//
// `ItemInfo::item_index_` est à +0x08 dans l'info, soit node + 0x10 — cohérent
// avec `num_` (+0x10 dans l'info = node + 0x18 = kNodeAmt), ce qui confirme au
// passage que le layout d'ItemInfo, LUI, est juste.
// ⏱ Relevé en jeu : `node+0x10` vaut **0** sur un nœud pourtant valide (id 7144,
// amt 1) — donc `item_index_` n'est PAS là, contrairement à ce que laissait
// croire le mirroir `ItemInfo` de ragnarok/item_info.h. Le layout de cette
// structure n'est juste que là où il a été confirmé (`num_` à info+0x10,
// `item_name_` à info+0x2C) ; le reste est du remplissage hérité.
//
// Plutôt que d'ajouter une supposition à la précédente, on DÉTECTE l'offset. Le
// levier est solide : le paquet CZ_USE_ITEM que le joueur vient d'émettre porte
// un index dont on sait qu'il désigne un objet ENCORE présent. Il suffit donc de
// chercher à quel offset **une seule** ligne d'inventaire porte cette valeur —
// l'unicité est la garde qui écarte les coïncidences (un champ à 0, un montant
// qui vaudrait l'index par hasard).
constexpr int kIndexCandidates[] = {0x10, 0x0c, 0x14, 0x08, 0x1c, 0x20};
constexpr int kIndexCandidateCount =
    static_cast<int>(sizeof(kIndexCandidates) / sizeof(kIndexCandidates[0]));

// ⏱ CONFIRMÉ en jeu (2026-07-29, client 20250716) : **node+0x0C**.
//   « offset d'index DETECTE : node+0x0C (index 20 -> id 612) », Mini Furnace,
//   suivi d'une ré-utilisation acceptée par le serveur.
//
// Soit, l'info commençant à node+0x08, `item_index_` à **info+0x04** — c'est-à-dire
// LÀ OÙ ragnarok/item_info.h déclare `location_`. Les deux champs sont intervertis
// dans ce mirroir ; seuls `num_` (info+0x10) et `item_name_` (info+0x2C) y étaient
// justes. On garde donc l'offset ici, en constante vérifiée, plutôt que de dériver
// d'une structure dont on sait maintenant qu'elle ment.
//
// Le détecteur reste en place : si un autre build donne un layout différent, la
// résolution échouera et il rétablira la valeur au premier objet utilisé, au lieu
// de désactiver silencieusement la relance.
int g_index_offset = 0x0c;

// Renvoie l'offset gagnant, ou -1 si aucun candidat n'est UNIQUEMENT porté par
// une seule ligne. `hits_out` reçoit le compte par candidat, pour le journal.
int DetectIndexOffset(unsigned wanted, int hits_out[kIndexCandidateCount]) {
  for (int i = 0; i < kIndexCandidateCount; ++i) hits_out[i] = 0;
  if (!wanted) return -1;
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return -1;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    int guard = 0;
    while (node && node != head && guard++ < kMaxInvNodes) {
      const int amount = *reinterpret_cast<const int*>(node + kNodeAmt);
      if (amount > 0) {
        for (int i = 0; i < kIndexCandidateCount; ++i)
          if (*reinterpret_cast<const unsigned*>(node + kIndexCandidates[i]) ==
              wanted)
            ++hits_out[i];
      }
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }

  for (int i = 0; i < kIndexCandidateCount; ++i)
    if (hits_out[i] == 1) return kIndexCandidates[i];
  return -1;
}

uint32_t InvIdByIndex(unsigned item_index) {
  const int kNodeIndex = g_index_offset;
  if (kNodeIndex < 0) return 0;  // offset pas encore établi
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    int guard = 0;
    while (node && node != head && guard++ < kMaxInvNodes) {
      const uint8_t* info = node + kNodeInfo;
      const unsigned idx  = *reinterpret_cast<const unsigned*>(node + kNodeIndex);
      const int amount    = *reinterpret_cast<const int*>(node + kNodeAmt);
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      if (amount > 0 && idx == item_index) return InfoItemId(info);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  return 0;
}

// 0 = « plus en inventaire ». Les index natifs commencent à 2 (le serveur fait
// `n = index - 2`), donc 0 est une sentinelle sûre et non un index valide.
unsigned InvIndexById(uint32_t item_id) {
  const int kNodeIndex = g_index_offset;
  if (kNodeIndex < 0) return 0;  // offset pas encore établi
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    int guard = 0;
    while (node && node != head && guard++ < kMaxInvNodes) {
      const uint8_t* info = node + kNodeInfo;
      const unsigned idx  = *reinterpret_cast<const unsigned*>(node + kNodeIndex);
      const int amount    = *reinterpret_cast<const int*>(node + kNodeAmt);
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      if (amount > 0 && InfoItemId(info) == item_id) return idx;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  return 0;
}

// Relevé BRUT du premier nœud d'inventaire, pour lever le doute sur l'offset de
// `item_index_`. C'est le seul champ de ce parcours qui n'a jamais été confirmé
// en jeu : il est DÉDUIT du layout d'ItemInfo (info+0x08, soit node+0x10), là où
// `amount` et l'id, eux, sont vérifiés. Si la résolution échoue, on affiche ces
// valeurs dans le journal : l'index natif est un petit entier ≥ 2, il se
// reconnaît d'un coup d'œil parmi ses voisines.
struct InvProbe {
  int      ok  = 0;
  uint32_t id  = 0;
  unsigned v0c = 0, v10 = 0, v14 = 0;
  int      amt = 0;
};

InvProbe ProbeFirstInvNode() {
  InvProbe p;
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return p;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    if (!node || node == head) return p;
    p.id  = InfoItemId(node + kNodeInfo);
    p.v0c = *reinterpret_cast<const unsigned*>(node + 0x0c);
    p.v10 = *reinterpret_cast<const unsigned*>(node + 0x10);
    p.v14 = *reinterpret_cast<const unsigned*>(node + 0x14);
    p.amt = *reinterpret_cast<const int*>(node + kNodeAmt);
    p.ok  = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { p.ok = 0; }
  return p;
}

// Envoi de CZ_USE_ITEM2 (0x0439) : { u16 opcode ; u16 index ; u32 aid }.
// On forge le paquet plutôt que d'appeler `UseItemById` — voir ci-dessus, sa
// résolution d'index est cassée. L'envoi passe par RagConnection::SendPacket,
// donc par SendPacketRef : il CONTOURNE notre propre hook d'observation, ce qui
// évite tout rebouclage.
bool SendUseItemPacket(unsigned item_index, uint32_t aid) {
  if (!item_index || !aid) return false;
  uint8_t packet[8];
  *reinterpret_cast<uint16_t*>(packet + 0) = 0x0439;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(item_index);
  *reinterpret_cast<uint32_t*>(packet + 4) = aid;
  return Bourgeon::Instance().client().rag_connection().SendPacket(
      sizeof(packet), reinterpret_cast<char*>(packet));
}

// ── La recette, côté CLIENT ──────────────────────────────────────────────────
// `MetalProcessRecipe_GetLines(itemId)` rend un `std::vector<std::string>*` : les
// lignes de recette telles qu'elles seront affichées (« 1 Iron Ore »), lues par le
// client dans `MetalProcessItemList.txt` au chargement. Jamais nul — vecteur vide
// si l'id est absent.
//
// C'est la seule source de recette accessible au client, et elle a ses limites
// (texte non structuré, 90 produits sur 254, peut dériver du serveur) : cf.
// docs/make_item_list_re.md §6.1. On l'affiche telle quelle, sans rien en déduire.
//
// ⚠ La lecture est BRUTE et sous SEH, donc dans une fonction SANS objet C++ à
// destructeur (C2712 se juge sur la fonction entière) : on copie dans des tampons
// de pile, l'appelant convertit ensuite.
constexpr uintptr_t kRecipeGetLines = 0x006a3f20;
using RecipeGetLines_t = const void*(__cdecl*)(int);
constexpr int kRecipeLineMax  = 96;  // une ligne de recette est courte
constexpr int kMaxRecipeLines = 8;   // la plus longue du fichier en fait 3
//
// ⚠ C'est un `std::vector<char*>`, PAS un `std::vector<std::string>` : élément de
// **4 octets**, pointeur brut vers une chaîne terminée par NUL dans le tampon de
// texte du fichier. Écrit d'abord avec une foulée de 24 (la disposition d'un
// std::string MSVC), ce code lisait une entrée sur six et déréférençait du
// contenu de chaîne comme un pointeur. Deux choses le disaient, et je ne les
// avais pas lues :
//   - `UIMakeTargetProcessWnd_OnDraw` divise par 4 (`(v10[1] - *v10) >> 2`) puis
//     déréférence l'élément en `char*` ;
//   - ⏱ en mémoire, l'entrée 1201 (Knife) a `end - begin = 8` pour DEUX lignes
//     (« 1 Iron », « 10 Jellopy »), ce qui ne laisse aucune place au doute.
constexpr int kRecipeStride = 4;

// 🔴 Biais de clé des recettes de FLÈCHES. Un même id est souvent à la fois
// produit de `produce_db` et source de flèches (Phracon, Coal, Elunium, Green
// Live…), or la table du client est indexée par id : l'un écrasait l'autre, et la
// liste du Hunter affichait des recettes de FORGE (⏱ Phracon montrait « 45 Spawn,
// 40 Glass Bead »). Les deux jeux cohabitent donc dans des espaces séparés, le
// générateur écrivant les flèches sous `id + 1000000`. Le natif n'interroge jamais
// ces clés — il ne cherche que l'id du produit visé dans sa fenêtre 80 — et les
// ids d'objets plafonnent très en dessous du million.
constexpr uint32_t kArrowRecipeKeyBias = 1000000;

int ReadRecipeLines(uint32_t item_id, char out[][kRecipeLineMax], int max_lines) {
  int written = 0;
  __try {
    const auto* vec = reinterpret_cast<const uintptr_t*>(
        reinterpret_cast<RecipeGetLines_t>(kRecipeGetLines)(
            static_cast<int>(item_id)));
    if (!vec) return 0;
    const uintptr_t begin = vec[0], end = vec[1];
    if (!begin || end < begin) return 0;
    const int count = static_cast<int>((end - begin) / kRecipeStride);
    const auto* lines = reinterpret_cast<const char* const*>(begin);
    for (int i = 0; i < count && written < max_lines; ++i) {
      const char* text = lines[i];
      if (!text || !*text) continue;
      int n = 0;
      while (n < kRecipeLineMax - 1 && text[n]) ++n;
      std::memcpy(out[written], text, n);
      out[written][n] = '\0';
      ++written;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return written; }
  return written;
}

// ── « Combien puis-je en faire ? » ───────────────────────────────────────────
// La recette n'existe côté client que sous forme de TEXTE (« 10 Green Live »),
// et il n'y a aucune table id→quantité lisible (§6.1). On la calcule donc en
// résolvant chaque ligne par son NOM, contre l'inventaire :
//
//   « 10 Green Live » -> quantité 10, nom « Green Live »
//   -> somme des objets d'inventaire dont MoonlightUi::ItemName vaut ce nom
//   -> possible = somme / 10 ; le minimum sur toutes les lignes fait le résultat.
//
// 🔴 Et surtout : le résultat est VÉRIFIÉ par le serveur avant d'être affiché.
// Si un produit figure dans la liste, c'est que `skill_can_produce_mix` a déjà
// constaté qu'on a de quoi en faire au moins UN. Un calcul qui rend 0 sur un
// produit listé est donc forcément FAUX chez nous — nom localisé, orthographe
// différente, ligne non conforme — et dans ce cas on n'affiche RIEN plutôt qu'un
// « 0 » mensonger sur une fenêtre de fabrication.

// Instantané brut de l'inventaire : (id, quantité). Séparé parce qu'il porte le
// __try et ne doit contenir aucun objet C++ (cf. le pavé C2712).
int SnapshotInventory(uint32_t* ids, int* amounts, int max) {
  int n = 0;
  __try {
    uint8_t* head = *reinterpret_cast<uint8_t**>(kInvListHead);
    if (!head) return 0;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head + kNodeNext);
    int guard = 0;
    while (node && node != head && guard++ < kMaxInvNodes && n < max) {
      const uint8_t* info = node + kNodeInfo;
      const int amount = *reinterpret_cast<const int*>(node + kNodeAmt);
      node = *reinterpret_cast<uint8_t**>(node + kNodeNext);
      if (amount <= 0) continue;
      ids[n] = InfoItemId(info);
      amounts[n] = amount;
      ++n;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { return n; }
  return n;
}

// Une ligne de recette -> quantité, nom, et id si la ligne le porte.
//
//   « 10 Green Live »          -> qty 10, nom « Green Live », id 0
//   « 1 Iron Ore [1002] »      -> qty 1,  nom « Iron Ore »,   id 1002
//
// La forme `[id]` en fin de ligne est une EXTENSION locale de
// MetalProcessItemList.txt. Elle est facultative et rétro-compatible : le natif
// rend la ligne verbatim (« 1 Iron Ore [1002] », lisible), et nous elle nous
// évite toute résolution par le nom — donc toute dépendance à la langue du
// serveur, à l'orthographe, et aux homonymes. Quand elle est absente on retombe
// sur la recherche par nom, qui marche mais peut échouer.
//
// `name_out` reçoit une COPIE (le nom s'arrête avant le crochet, on ne peut donc
// pas rendre un pointeur dans la ligne d'origine).
bool ParseRecipeLine(const char* line, int* qty, char* name_out,
                     size_t name_size, uint32_t* id_out,
                     bool* not_consumed_out, bool* is_output_out) {
  if (!line || !name_out || name_size == 0) return false;
  *id_out = 0;
  *not_consumed_out = false;
  *is_output_out    = false;
  while (*line == ' ' || *line == '\t') ++line;
  // Quantité NÉGATIVE = ce que la fabrication PRODUIT, et non ce qu'elle exige.
  // Extension maison du format, employée pour les recettes de flèches : celles-ci
  // vivent dans `create_arrow_db.yml` côté serveur, où la clé est le MATÉRIAU et
  // où il y a un rendement à décrire (« -40 Arrow »). Écrire ce rendement en
  // positif en ferait une exigence, et plafonnerait la faisabilité au stock de
  // flèches déjà possédées. Sans danger pour le natif : il ne lit ce fichier que
  // depuis sa fenêtre 80, que les flèches n'ouvrent jamais.
  bool is_output = false;
  if (*line == '-') { is_output = true; ++line; }
  if (*line < '0' || *line > '9') return false;
  int n = 0;
  while (*line >= '0' && *line <= '9') { n = n * 10 + (*line - '0'); ++line; }
  // ⚠ Une quantité de ZÉRO est LÉGALE : c'est le marqueur de `produce_db` pour
  // « doit être possédé mais n'est pas consommé » (les guides de fabrication).
  // La rejeter faisait perdre la ligne entière.
  if (n <= 0 && is_output) return false;  // « -0 » n'a pas de sens
  if (n == 0) { *not_consumed_out = true; n = 1; }
  *is_output_out = is_output;
  if (*line != ' ' && *line != '\t') return false;
  while (*line == ' ' || *line == '\t') ++line;
  if (!*line) return false;
  *qty = n;

  // Un « [ » suivi UNIQUEMENT de chiffres puis « ] ». Ce sont les chiffres qui
  // font la garde contre un nom d'objet contenant un crochet — pas la position.
  // 🔴 La version d'origine exigeait le « ] » en FIN de ligne, et perdait donc
  // toute ligne suivie d'une mention (⏱ « 1 Condensed Potion Creation Guide
  // [7133] (non consomme) » : id non résolu, crochets affichés dans le nom, et
  // « Faisable » impossible à calculer faute d'une recette complète).
  const char* name_end = line + std::strlen(line);
  const char* bracket = std::strrchr(line, '[');
  if (bracket && bracket > line) {
    const char* p = bracket + 1;
    uint32_t parsed = 0;
    bool digits = false;
    while (*p >= '0' && *p <= '9') { parsed = parsed * 10 + (*p - '0'); ++p; digits = true; }
    if (digits && *p == ']') {
      *id_out = parsed;
      name_end = bracket;
      while (name_end > line && (name_end[-1] == ' ' || name_end[-1] == '\t'))
        --name_end;
      // Compatibilité avec les fichiers déjà distribués, qui portent la mention
      // en clair après les crochets. Le marqueur CANONIQUE est la quantité 0 ;
      // celui-ci n'est qu'un repli, et le libellé affiché vient de l'UI.
      if (std::strstr(p, "non consom")) *not_consumed_out = true;
    }
  }

  size_t len = static_cast<size_t>(name_end - line);
  if (len >= name_size) len = name_size - 1;
  std::memcpy(name_out, line, len);
  name_out[len] = '\0';
  return len > 0;
}

// Masque une fenêtre native par son flag +0x28 (JAMAIS hors écran : cf.
// feedback_no_offscreen_hide). Vérifie la vtable avant d'écrire.
void HideIfClass(void* win, uintptr_t expected_vtable) {
  if (!win) return;
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) != expected_vtable) return;
    *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(win) + kWndVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// 🔴 DÉTRUIT une fenêtre native de fabrication, vtable vérifiée.
//
// Masquer ne suffit PAS : une fenêtre native invisible garde le CLAVIER.
//
// ⏱ CONSTATÉ en jeu, puis remonté dans le binaire :
//     UIWindowMgr_OnKeyDown @0x00A471E0        Entrée (13) OU Espace (32)
//  -> UIWindowMgr_ActivateDefaultButton @0x00A2E270   OnMsg(msg = 0)
//  -> UIWindow_OnMsg_Default @0x008841D0       OnMsg(6, this+0x8C)
//  -> case 6 / id 184 = le bouton OK           SendMsg(130, …) = CZ 0x018E
// et rien, sur tout ce chemin, ne consulte la visibilité : le prédicat vt+8 que
// le gestionnaire interroge est un `return 1` en dur (@0x005A5D90). C'est la même
// leçon que le drop sur une fenêtre cachée — +0x28 masque le RENDU, pas la
// réception des évènements.
//
// Le OK natif fabrique donc SA sélection (sa ligne 0, c'est-à-dire l'ordre du
// paquet — d'où « Failed to create Iron. » pendant que notre table montrait Star
// Crumb). Il consomme les matériaux, et surtout le serveur fait son
// `clif_menuskill_clear` : notre propre envoi suivant part alors dans le vide,
// sans le moindre paquet de réponse (« Le serveur n'a pas répondu »).
//
// ⚠ Et le réglage « Entrée lance la fabrication » n'en protège pas : décoché, il
// laisse volontairement la touche filer vers le jeu — qui la donne à cette
// fenêtre. Sa promesse (« Entrée revient au chat ») ne pouvait pas tenir tant que
// cette fenêtre existait.
//
// 🔴 Et on la NEUTRALISE par son bouton Annuler, jamais par `UIWindowMgr::Close`.
// Une première rédaction fermait par le gestionnaire, au motif que l'Annuler ENVOIE
// un paquet — précisément ce qu'il faut, en fait : c'est le désarmement du
// `menuskill` (§4.6). Fermer sans l'envoyer laissait le serveur armé, et pour la Mini
// Furnace (`menuskill_id == -1`) `clif_skill_produce_mix_list` sort alors aussitôt
// sur son `if (menuskill_id == skill_id) return;` — la compétence ne renvoie plus
// jamais rien. ⏱ Constaté en jeu. Cf. CancelNativeIfClass.
bool NativeAlive(int window_id, uintptr_t expected_vtable);

bool NativeAlive(int window_id, uintptr_t expected_vtable) {
  void* w = uiwnd::FindWindow(window_id);
  if (!w) return false;
  __try {
    return *reinterpret_cast<uintptr_t*>(w) == expected_vtable;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Pilote le bouton ANNULER d'une fenêtre native de fabrication.
//
// C'est le seul chemin qui désarme proprement une session qu'on n'a PAS vue passer
// (interrupteur basculé alors que la fenêtre était déjà ouverte) : nous ignorons son
// protocole de réponse — 130 avec itemId 0, ou 153/207 avec -1 — mais la fenêtre,
// elle, le connaît. Elle envoie son paquet, rend les matériaux s'il y en a, puis se
// détruit. Nous n'avons rien à deviner.
void CancelNativeIfClass(int window_id, uintptr_t expected_vtable) {
  if (!NativeAlive(window_id, expected_vtable)) return;
  void* w = uiwnd::FindWindow(window_id);
  if (!w) return;
  __try {
    uiwnd::OnMsg(w, kMsgUiAction, kBtnCancelId);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

// ── Lecteurs de paquet, déportés ─────────────────────────────────────────────
// 🔴 C2712 se juge sur la fonction ENTIÈRE : un `__try` est interdit dès qu'un
// objet à destructeur non trivial vit quelque part dans la même fonction. Or
// `OnRecvPacket` pousse dans un `std::vector` et construit des `std::string`
// (l'argument de `Log`) — ses itérateurs ont un destructeur dès que
// `_ITERATOR_DEBUG_LEVEL > 0`. Les lectures brutes vivent donc ICI, dans des
// fonctions sans le moindre objet C++. Même leçon que WeaponRefineWindow.

bool ReadU16(const void* p, uint16_t* out) {
  __try {
    *out = *reinterpret_cast<const uint16_t*>(p);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return true;
}

// Recopie jusqu'à `max` ids d'objet depuis la charge utile, en avançant de
// `stride` octets par entrée. Renvoie le nombre réellement lu — une lecture qui
// déborde s'arrête sur ce qui a été obtenu au lieu de tout perdre.
int ReadEntryIds(const uint8_t* payload, int count, int stride, uint32_t* out,
                 int max) {
  int read = 0;
  __try {
    for (; read < count && read < max; ++read)
      out[read] = *reinterpret_cast<const uint32_t*>(payload + read * stride);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return read; }
  return read;
}

// ZC_ACK_REQMAKINGITEM. ⚠ `data` commence APRÈS l'opcode : paquet +2 `result`
// -> data +0, paquet +4 `itemId` -> data +2. (Le piège qui avait laissé le
// journal du refine muet : y écrire les offsets du PAQUET lit à cheval sur les
// deux champs.)
bool ReadMakeResult(const uint8_t* data, uint16_t* result, uint32_t* nameid) {
  __try {
    *result = *reinterpret_cast<const uint16_t*>(data);
    *nameid = *reinterpret_cast<const uint32_t*>(data + 2);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  return true;
}

}  // namespace

// ── Construction ─────────────────────────────────────────────────────────────

MakeItemWindow::MakeItemWindow() {
  // ── Les TROIS listes : on prend la place du handler natif ──────────────────
  //
  // 🔴 REMPLACEMENT, plus observation. Observer laissait le handler natif ouvrir
  // sa fenêtre, qu'on masquait puis détruisait derrière — et entre les deux, cette
  // fenêtre restait une cible clavier : Entrée ou Espace validaient son bouton par
  // défaut et lançaient une fabrication sur SA sélection (docs §12.5). On empêche
  // donc la fenêtre de NAÎTRE, ce qui est le seul état vraiment sûr.
  //
  // Le prédicat est relu à CHAQUE paquet, et il est exactement celui qui gate
  // notre fenêtre : plugin coupé, le handler natif reprend la main à l'octet près,
  // et les six métiers retrouvent leur interface d'origine. C'est ce qui rend
  // l'interrupteur « Interface moderne » encore honnête.
  //
  // ⚠ Le prédicat ne doit RIEN tester d'autre — surtout pas un état de session ou
  // de rendu. Un « non » sur un paquet que le natif ne traitera pas non plus (il
  // aura déjà rendu la main) ferait disparaître la liste pour tout le monde : le
  // joueur lance sa compétence et rien n'apparaît.
  const auto claim = [this] { return imgui_enabled_; };
  Bourgeon::Instance().RegisterReplaceOpcode(kOpMakableList, claim);
  Bourgeon::Instance().RegisterReplaceOpcode(kOpArrowList, claim);
  Bourgeon::Instance().RegisterReplaceOpcode(kOpMakingList, claim);
  // Ces deux-là restent en OBSERVATION : ce sont des RÉSULTATS, pas des listes.
  // Le natif y affiche un message de chat qu'on veut garder (et que notre fenêtre
  // ne duplique pas), et ils n'ouvrent aucune fenêtre.
  Bourgeon::Instance().RegisterObserveOpcode(kOpMakeResult, kMakeResultLen);
  Bourgeon::Instance().RegisterObserveOpcode(kOpSkillFail, kSkillFailLen);
  // Maîtrise culinaire (ZC 0x0F1C) : la SEULE donnée du calcul de cuisine que le
  // client ne peut pas déduire. Opcode custom > 0x0C35, donc hors table du client
  // et livré par le reader-hook. Poussé au login vérifié et à chaque changement.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kCookMastery);
}

// ── Capture ──────────────────────────────────────────────────────────────────

// Fil RÉSEAU : on copie, rien de plus (cf. features/net_inbox.h). Les trois listes
// de fabrication sont à longueur ANNONCÉE — c'est elle qui fait foi, pas `len`.
void MakeItemWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  if (opcode == kOpMakableList || opcode == kOpArrowList ||
      opcode == kOpMakingList)
    net_inbox_.PushAnnounced(opcode, data, len);
  else
    net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
void MakeItemWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                  uint16_t len) {
  // ── Maîtrise culinaire (ZC 0x0F1C) ────────────────────────────────────────
  // `[len:2][mastery:2]` — `data` commence APRÈS l'opcode, comme partout ici.
  // Reçue au login vérifié puis à chaque changement : le serveur ne l'émet que
  // si la valeur a bougé, donc pas de filtrage à faire de notre côté.
  if (opcode == bopcodes::kCookMastery) {
    uint16_t mastery = 0;
    if (len >= 4 && ReadU16(data + 2, &mastery)) {
      const int previous = cook_mastery_;
      cook_mastery_ = static_cast<int>(mastery);
      // Le premier push est l'état initial, pas une progression : on ne le
      // commente pas. Les suivants, si — c'est justement ce que le jeu ne dit
      // nulle part, et ce qui explique qu'un même plat réussisse mieux ce soir
      // qu'hier.
      if (previous >= 0 && previous != cook_mastery_) {
        char moved[160];
        std::snprintf(moved, sizeof(moved),
                      i18n::Tr("Maîtrise culinaire : %d -> %d."), previous,
                      cook_mastery_);
        Log(moved, (cook_mastery_ > previous) ? kColOk : kColWarn);
      }
    }
    return;
  }

  const bool is_list = (opcode == kOpMakableList || opcode == kOpArrowList ||
                        opcode == kOpMakingList);
  if (is_list) {
    uint16_t total = 0;
    if (!ReadU16(data, &total)) return;

    // En-tête et taille d'entrée, par opcode (§3.1 à §3.3). Le mk_type de 0x01AD
    // est FORCÉ à 2 par le client lui-même — ce n'est pas une supposition, c'est
    // un `push 2` en dur à 0x00CA692C.
    // 🔴 Protocole et mk_type en LOCALES, et pas encore dans les membres : on ne
    // sait PAS, avant d'avoir lu le compte, si cette liste remplace vraiment la
    // session en cours (cf. le garde « liste vide » juste en dessous). Les écrire
    // ici était le bogue « la furnace pendant une fabrication continue bloque tous
    // les skills » : le protocole d'annulation de la seule session que le serveur
    // honorait encore était écrasé par celui d'une liste qui n'armait rien.
    int header = 4, entry_size = kEntrySimple;
    Proto    incoming_proto   = Proto::kProduce;
    uint16_t incoming_mk_type = 0;
    if (opcode == kOpMakableList) {
      entry_size = kEntryMakable;
    } else if (opcode == kOpArrowList) {
      incoming_proto   = Proto::kArrow;
      incoming_mk_type = 2;
    } else {  // 0x025A : en-tête + u16 mk_type
      header = 6;
      if (!ReadU16(data + 2, &incoming_mk_type)) incoming_mk_type = 0;
      incoming_proto = (incoming_mk_type == 2) ? Proto::kArrow : Proto::kMaking;
    }
    if (total < header) return;

    // Lecture brute d'abord (fonction déportée, cf. le pavé C2712), remplissage
    // du modèle ensuite : aucune structure C++ ne vit pendant la lecture.
    uint32_t ids[kMaxEntries];
    const int count = (total - header) / entry_size;

    // ── 🔴 Une liste VIDE arrivée sur une session ARMÉE ne change RIEN ─────────
    //
    // Le serveur n'arme `menuskill_id` que `if (count > 0)`, mais il envoie la
    // liste INCONDITIONNELLEMENT (`clif_send` avant le test, clif.cpp:8508). Une
    // liste vide veut donc dire « refusé » — et la session précédente est TOUJOURS
    // vivante côté serveur, avec son propre `menuskill_id`.
    //
    // Comment on en reçoit une : `skill_can_produce_mix` contient
    //     if (j > 0 && sd->menuskill_id > 0 && sd->menuskill_id != j) continue;
    // (skill.cpp:13304, commentée « special case »). Toute recette dont le
    // `req_skill` diffère du menuskill ARMÉ est écartée — donc utiliser un Mini
    // Furnace pendant une session d'Arrow Crafting ou de Prepare Potion vide
    // intégralement la liste des métaux. ⏱ Constaté en jeu.
    //
    // Ce que faisait la version précédente, et pourquoi ça bloquait TOUT :
    //   1. elle adoptait `proto_ = kProduce` (la fenêtre 79 du `produce`) ;
    //   2. elle posait `skill_id_ = 0` (filet anti-lancement fantôme) ;
    //   3. à la fermeture, l'annulation partait donc en CZ 0x018E — dont le
    //      parseur `clif_parse_ProduceMix` n'efface le menuskill que pour
    //      -1 / AM_PHARMACY / RK_RUNEMASTERY / GC_RESEARCHNEWPOISON et sort par
    //      `default: return` sinon. Avec AC_MAKINGARROW armé : AUCUN effacement.
    //   4. `menuskill_id` restait armé pour de bon, et
    //      `clif_parse_skill_toid` refuse tout lancement dans ce cas
    //      (« Can't use skills while a menu is open »). Plus un seul skill ne
    //      partait jusqu'au changement de carte.
    //
    // On ne touche donc RIEN : ni `proto_`, ni `skill_id_`, ni `list_armed_`, ni
    // `entries_` (la table affichée appartient à la session vivante et reste
    // valable), ni le refine — `CloseForOtherCraft` fermerait une session que le
    // serveur honore encore.
    // 🔴 EXCEPTION, et elle ne vaut QUE pour la cuisine : `clif_cooking_list` arme
    // sur `count > 0 || skill_id == AM_PHARMACY` (clif.cpp:8555). Or la cuisine
    // passe TOUJOURS par AM_PHARMACY (`clif_cooking_list(sd, trigger, AM_PHARMACY,
    // 1, 1)`), donc une liste de cuisine VIDE arme quand même — contrairement aux
    // deux autres chemins, où `if (count > 0)` est sans échappatoire.
    //
    // Conséquence si on l'ignore, et elle est brutale : on croit la session morte,
    // on n'annule donc jamais, et la prochaine utilisation d'un kit se heurte au
    // « Avoid resending the menu » (`if (menuskill_id == skill_id) return;`, en
    // TÊTE de la fonction) — plus aucun paquet, l'ustensile consommé pour rien, et
    // aucun message nulle part.
    //
    // `mk_type == 1` est le discriminant : c'est le `list_type` que le buildin
    // `cooking` passe, et lui seul correspond à AM_PHARMACY (les GN_*/MT_*/BO_*
    // emploient 4 à 8).
    const bool arms_even_when_empty =
        (opcode == kOpMakingList && incoming_mk_type == 1);

    if (count <= 0 && list_armed_ && !arms_even_when_empty) {
      const MoonlightUi* mui = Bourgeon::Instance().moonlight_ui();
      // L'objet, lui, est bel et bien PERDU : `pc_useitem` fait son
      // `pc_delitem(LOG_TYPE_CONSUME)` AVANT d'exécuter le script. Le dire est le
      // minimum — c'est une dépense que le joueur ne verra nulle part ailleurs.
      if (item_id_ != 0 && GetTickCount() - item_use_at_ <= kItemUseWindowMs) {
        const char* used = mui ? mui->ItemName(item_id_) : nullptr;
        char lost[224];
        std::snprintf(lost, sizeof(lost),
                      i18n::Tr("%s consommé sans effet : une session de fabrication est "
                      "déjà ouverte, le serveur n'en garde qu'une. Terminez-la "
                      "ou fermez-la d'abord."),
                      (used && *used) ? used : "Objet");
        Log(lost, kColWarn);
      } else {
        Log("Aucun produit proposé : une session de fabrication est déjà ouverte "
            "(le serveur n'en garde qu'une). La liste affichée reste valable.",
            kColWarn);
      }
      LogDiag(
          "[make] liste VIDE opcode=0x{:04X} ignoree : session armee conservee "
          "(proto={} skill={} from_item={})",
          opcode, static_cast<int>(proto_), skill_id_, from_item_ ? 1 : 0);
      // L'inventaire a bougé (l'objet vient d'être consommé) : les stocks et la
      // faisabilité affichés se rafraîchissent, la session ne bouge pas.
      RebuildOwnedCounts();
      return;
    }

    proto_   = incoming_proto;
    mk_type_ = incoming_mk_type;

    const uint8_t* payload = data + (header - 2);  // `data` commence APRÈS l'opcode
    // Les 3 `material` de 0x018D sont volontairement ignorés : le serveur les
    // remplit de ZÉROS (clif.cpp:8500) et le natif les jette aussi — seul le
    // premier u32 de chaque entrée nous intéresse, d'où la foulée `entry_size`.
    const int read = ReadEntryIds(payload, count, entry_size, ids, kMaxEntries);

    // ⚠ DÉDOUBLONNAGE, et ce n'est pas de la cosmétique.
    //
    // Le serveur envoie réellement le même produit plusieurs fois :
    // `clif_skill_produce_mix_list` itère `produce_db` PAR INDEX mais interroge
    // `skill_can_produce_mix` PAR NAMEID — laquelle re-cherche la PREMIÈRE entrée
    // portant ce nameid. Deux lignes de `produce_db` pour un même produit (ex.
    // Steel 999 : entrées 113 et 215) valident donc toutes deux, et le produit
    // part en double. ⏱ Observé en jeu : deux « Steel » dans la liste de la
    // Mini Furnace, sur le natif comme chez nous.
    //
    // On ne perd RIEN en dédoublonnant : le client ne renvoie qu'un nameid, il
    // est incapable de désigner l'une plutôt que l'autre, et c'est le serveur qui
    // choisira de toute façon. Deux lignes identiques n'offrent donc aucun choix
    // — seulement une ambiguïté (et une collision d'ID ImGui).
    // Liste vide qui succède à une liste pleine : on GARDE l'ancienne de côté.
    // C'est le seul ensemble de produits dont on sache qu'il était réalisable il
    // y a un instant, donc le bon candidat pour expliquer ce qui manque
    // maintenant. Une liste non vide la périme aussitôt.
    if (read == 0) {
      if (!entries_.empty()) stale_entries_ = entries_;
    } else {
      stale_entries_.clear();
    }

    entries_.clear();
    entries_.reserve(read);
    for (int i = 0; i < read; ++i) {
      bool already = false;
      for (const Entry& seen : entries_)
        if (seen.id == ids[i]) { already = true; break; }
      if (already) continue;
      Entry e;
      e.id = ids[i];
      entries_.push_back(e);
    }

    // ── D'où vient cette liste : compétence, ou OBJET ? ──────────────────────
    // ⚠ DÉTERMINÉ ICI, avant toute résolution de recette. Rien dans le paquet ne
    // le dit : une liste ouverte par une compétence suit forcément de près l'envoi
    // qu'on a observé ; sans envoi récent, c'est un script d'objet (Mini Furnace,
    // marteaux — `produce N;`). La distinction n'est pas cosmétique — un objet
    // `Usable` est CONSOMMÉ à chaque usage, donc aucune relance automatique n'est
    // acceptable de ce côté.
    //
    // 🔴 L'ordre compte : ce calcul vivait APRÈS la boucle de résolution, si bien
    // que celle-ci choisissait son espace de clés d'après l'état de la liste
    // PRÉCÉDENTE. Un défaut invisible tant que rien ne dépendait de `from_item_`
    // pendant la résolution.
    // 🔴 « Un skill a été lancé récemment » NE SUFFIT PAS, et cette version-là a
    // produit un vrai dégât. Ancienne règle :
    //     from_item_ = pas de lancement récent
    // Elle prenait N'IMPORTE QUEL lancement pour l'origine de la liste. Enchaînement
    // constaté en jeu : le joueur lance le REFINE, puis utilise une Mini Furnace dans
    // les 3 secondes -> `from_item_` faux -> la relance emprunte le chemin
    // « compétence » et `SendRecast()` relance… **WS_WEAPONREFINE**. La fenêtre de
    // refine se réouvrait toute seule, sans qu'aucun bouton ait été touché, et le
    // cycle de fabrication s'effondrait.
    //
    // Vérification POSITIVE désormais : la compétence observée doit pouvoir ouvrir
    // CETTE liste, c'est-à-dire être le `req_skill` d'au moins un produit qu'elle
    // contient (donnée du YAML, elle-même issue de `produce_db`). Sinon, la liste
    // vient d'un objet — ce qui est le cas par défaut, et le cas SÛR : le chemin
    // objet ne relance rien sans le réglage dédié.
    //
    // ⚠ `AC_MAKINGARROW` reste testée à part : sa liste énumère les MATÉRIAUX à
    // transformer, pas des produits, donc aucun `req_skill` n'y correspondrait.
    //
    // ⚠ DÉPENDANCE ASSUMÉE au YAML de recettes : s'il manque, `RecipeSkill` rend 0
    // partout, aucune correspondance n'est trouvée et TOUTE liste passe pour venue
    // d'un objet. La relance par compétence est alors perdue — mais rien de faux
    // n'est lancé, ce qui est le bon sens de la dégradation.
    const bool recent_cast =
        skill_cast_at_ != 0 &&
        GetTickCount() - skill_cast_at_ <= kSkillCastWindowMs;
    bool cast_opened_this_list = false;
    if (recent_cast) {
      if (skill_id_ == kSkillMakingArrow) {
        cast_opened_this_list = true;
      } else {
        for (const Entry& e : entries_)
          if (craftdata::RecipeSkill(e.id) == skill_id_) {
            cast_opened_this_list = true;
            break;
          }
      }
    }
    from_item_ = !cast_opened_this_list;
    // `AC_MAKINGARROW` est la seule compétence dont la liste énumère des MATÉRIAUX
    // à transformer, et non des produits. Elle commande l'espace de clés des
    // recettes ET le masquage de la colonne « Faisable » (qui y répéterait
    // « Possédé » : une fabrication consomme exactement un matériau).
    arrow_list_ = (skill_id_ == kSkillMakingArrow) && !from_item_;

    // 🔴 FILET, et il vaut à lui seul le bogue ci-dessus : liste venue d'un OBJET =
    // on ne connaît AUCUNE compétence pour elle, donc on en oublie une. `SendRecast`
    // sort sur `skill_id_ <= 0`, et plus aucun lancement fantôme n'est possible même
    // si une autre règle se trompait plus tard.
    //
    // Ce n'est pas une perte : quand `from_item_` est vrai, la relance passe par
    // l'OBJET (`kReuseItem`), qui n'a que faire de `skill_id_`. Et la prochaine
    // observation de lancement le remplira de nouveau.
    if (from_item_) {
      skill_id_ = 0;
      skill_lv_ = 0;
    }

    // Libellés et stocks : la DB client sait tout ça, gratuitement.
    auto* ui = Bourgeon::Instance().moonlight_ui();
    for (Entry& e : entries_) {
      const char* n = ui ? ui->ItemName(e.id) : nullptr;
      if (n && *n)
        std::snprintf(e.name, sizeof(e.name), "%s", n);
      else  // sans DB chargée on affiche l'id : faux jamais, muet parfois
        std::snprintf(e.name, sizeof(e.name), "#%u", e.id);
      ResolveMaterials(e);
    }
    RebuildOwnedCounts();

    // 🔴 Calé sur le COMPTE DU PAQUET, pas sur `entries_.empty()`.
    //
    // Ce drapeau commande `list_armed_`, donc l'envoi de l'annulation à la
    // fermeture — c'est-à-dire le désarmement du `menuskill` serveur. Il doit donc
    // dire « le serveur a-t-il armé ? », et le serveur arme exactement sur
    // `if (count > 0)` (`clif_skill_produce_mix_list`, et à l'identique pour les
    // flèches et la liste de fabrication) alors qu'il envoie la liste MÊME vide
    // (`clif_send` inconditionnel).
    //
    // `entries_.empty()` pouvait s'en désynchroniser : `ReadEntryIds` s'arrête sur
    // une lecture qui déborde et peut rendre 0 sur un paquet qui, lui, annonçait des
    // entrées. On croirait alors que rien n'est armé, l'annulation ne partirait pas,
    // et le personnage resterait bloqué — plus aucune compétence ne passe. ⏱ C'est
    // exactement ce qui est arrivé au refine, par un autre chemin.
    empty_list_      = (count <= 0);
    awaiting_result_ = false;
    // Une nouvelle liste chasse le résultat précédent — SAUF si c'est nous qui
    // l'avons provoquée. Pendant une chaîne, les listes s'enchaînent en une demi-
    // seconde : effacer le résultat à chaque tour ne laissait pas le temps de lire
    // ce qui venait d'être fabriqué. Il reste donc affiché jusqu'au résultat
    // SUIVANT, qui le remplacera de toute façon.
    if (!auto_ours_) last_result_.clear();
    auto_recast_at_  = 0;
    // La liste EST la preuve que la relance a abouti : plus rien en vol, et le
    // compteur d'essais repart de zéro pour le tour suivant.
    relaunch_sent_at_   = 0;
    relaunch_retries_   = 0;
    reuse_owned_before_ = -1;

    // ── Les emplacements de forge tiennent d'une relance à l'autre… ──────────
    // …mais pas au-delà du stock. Un emplacement dont l'objet a disparu ne serait
    // plus consommé par le serveur (`pc_search_inventory` rend -1 et il passe),
    // donc le garder ferait afficher un malus qui ne s'appliquerait PAS — et la
    // chance annoncée deviendrait fausse, ce qui est le seul défaut qu'on ne
    // s'autorise pas ici.
    for (int slot = 0; slot < 3; ++slot) {
      if (forge_slot_[slot] == 0) continue;
      if (OwnedCount(forge_slot_[slot]) > 0) continue;
      // Nom via la DB client, jamais une constante (règle du projet) ; à défaut
      // l'id, qui est faux nulle part et muet parfois.
      const MoonlightUi* mu = Bourgeon::Instance().moonlight_ui();
      const char* gone_name = mu ? mu->ItemName(forge_slot_[slot]) : nullptr;
      char gone[160];
      if (gone_name && *gone_name)
        std::snprintf(gone, sizeof(gone),
                      i18n::Tr("Emplacement %d vidé : plus de %s en réserve."), slot + 1,
                      gone_name);
      else
        std::snprintf(gone, sizeof(gone),
                      i18n::Tr("Emplacement %d vidé : plus d'objet #%u en réserve."),
                      slot + 1, forge_slot_[slot]);
      Log(gone, kColWarn);
      forge_slot_[slot] = 0;
    }

    // 🔴 UNE SEULE session de fabrication à la fois, parce que le serveur n'a qu'un
    // `menuskill_id` : cette liste vient de l'écraser, donc la session de refine — si
    // elle existait — est morte à l'instant. Sa fenêtre resterait pourtant à l'écran,
    // liste et bouton actifs, et le joueur cliquerait « Refine » dans le vide.
    //
    // C'est le refine qui se retire, jamais l'inverse : le serveur refuse déjà de
    // lancer WS_WEAPONREFINE pendant une session de fabrication. ⏱ Vérifié en jeu.
    if (auto* refine = Bourgeon::Instance().weapon_refine_window())
      refine->CloseForOtherCraft();

    // ── Et si c'est un objet : LEQUEL ? ──────────────────────────────────────
    // Même raisonnement temporel, sur l'autre observation (CZ_USE_ITEM). On fige
    // l'identifiant ici et pas plus tard : c'est le seul instant où « le dernier
    // objet utilisé » désigne encore à coup sûr celui qui a ouvert CETTE liste.
    source_item_id_ = 0;
    source_item_name_[0] = '\0';
    if (from_item_ && item_id_ != 0 &&
        GetTickCount() - item_use_at_ <= kItemUseWindowMs) {
      source_item_id_ = item_id_;
      const char* on = ui ? ui->ItemName(source_item_id_) : nullptr;
      std::snprintf(source_item_name_, sizeof(source_item_name_), "%s",
                    (on && *on) ? on : "cet objet");
    }

    // Traçage de l'ORIGINE : c'est la donnée qui décide de toute la relance, et
    // elle est entièrement déduite (aucun paquet ne la porte). Sans cette ligne,
    // un échec de relance ne dit pas LEQUEL des trois maillons a cédé —
    // l'observation, la résolution d'index, ou le réglage.
    LogDiag(
        "[make] liste opcode=0x{:04X} entrees={} | from_item={} skill={} niv={} "
        "(il y a {} ms) | item_id={} index={} (il y a {} ms) | source={} \"{}\" "
        "| sonde={}",
        opcode, entries_.size(), from_item_ ? 1 : 0, skill_id_, skill_lv_,
        skill_cast_at_ ? GetTickCount() - skill_cast_at_ : 0u, item_id_,
        item_use_index_, item_use_at_ ? GetTickCount() - item_use_at_ : 0u,
        source_item_id_, source_item_name_,
        // Pas de std::string temporaire ici : OnRecvPacket a déjà coûté un C2712
        // (`__try` interdit dès qu'un objet doit être déroulé), et il n'y a
        // aucune raison de réintroduire le risque pour un préfixe.
        (from_item_ && source_item_id_ == 0 && item_probe_[0]) ? item_probe_
                                                               : "");

    // ── Cette liste, est-ce NOUS qui l'avons provoquée ? ─────────────────────
    // Sans cette distinction le compteur de chaîne ne redescendait jamais : il
    // s'incrémentait à chaque relance et survivait à la fermeture de la fenêtre,
    // si bien qu'une chaîne de 1 pouvait s'afficher « (7) ». Une liste que le
    // joueur a ouverte lui-même REPART de zéro — y compris le compte
    // d'exemplaires consommés, qui ne doit décrire QUE la chaîne en cours.
    if (auto_ours_) {
      auto_ours_ = false;
      // Série en cours : cette liste est le tour suivant. On ne déclenche pas
      // ici — on est dans un handler de PAQUET, et RequestMake finit par toucher
      // le natif. OnTick s'en charge, comme pour la relance.
      if (batch_left_ > 0 && !empty_list_) batch_fire_ = true;
    } else {
      auto_chain_       = 0;
      auto_items_used_  = 0;
      // Liste ouverte à la main : toute série précédente est caduque. Sans ça,
      // une série interrompue (relance coupée, refus serveur) repartirait toute
      // seule à la prochaine ouverture — une action que le joueur n'a pas
      // demandée, sur des matériaux bien réels.
      batch_left_  = 0;
      batch_fire_  = false;
    }

    // Une liste VIDE est le vrai « il n'y a plus rien à faire », dit par le
    // SERVEUR plutôt que deviné : c'est la condition d'arrêt de la chaîne.
    if (empty_list_) batch_left_ = 0;  // plus rien à faire : la série est close
    if (empty_list_ && auto_chain_ > 0)
      auto_stop_reason_ = "Plus rien à fabriquer : relance arrêtée.";
    if (!empty_list_) auto_stop_reason_.clear();

    // ── Que devient la sélection quand une nouvelle liste arrive ? ───────────
    // Elle est RECONDUITE si le produit visé y figure encore — on enchaîne
    // presque toujours le même. Sinon elle est VIDÉE : pas de repli sur la
    // première entrée, ce serait déplacer silencieusement la cible d'une action
    // qui CONSOMME des matériaux. Seule exception, la toute première liste d'une
    // session : il n'y avait rien de visé, donc rien à perdre.
    const int prev = sel_id_;
    sel_id_ = -1;
    if (prev < 0) {
      if (!entries_.empty()) sel_id_ = static_cast<int>(entries_.front().id);
    } else {
      for (const Entry& e : entries_)
        if (static_cast<int>(e.id) == prev) { sel_id_ = prev; break; }
    }

    // Le serveur vient d'armer un nouveau `menuskill` : on a de nouveau le droit
    // d'envoyer. C'est le pendant exact du désarmement fait à l'envoi.
    //
    // ⚠ SAUF si la liste est vide : le serveur ne pose `menuskill_id` que quand
    // il a proposé au moins un produit (`if (count > 0)` dans
    // clif_elementalconverter_list / clif_skill_produce_mix_list). Une liste vide
    // n'arme donc rien — et la marquer armée laissait « Fabriquer » grisé sans
    // aucun moyen de repartir, alors que c'est précisément le moment où l'on veut
    // « Relancer ».
    // ⚠ `|| arms_even_when_empty` : voir le pavé du garde ci-dessus. Une liste de
    // CUISINE vide arme quand même côté serveur, donc la nôtre doit se déclarer
    // armée — sinon on n'enverrait pas l'annulation, et le kit suivant se ferait
    // avaler en silence par le « Avoid resending the menu ».
    list_armed_ = !empty_list_ || arms_even_when_empty;
    // C'est CE paquet qui ouvre notre fenêtre, pas la création de la native :
    // celle-ci est DÉTRUITE dès la première demande, et nous devons lui survivre.
    ui_open_ = true;

    // 🔴 Plus d'escamotage de modale ICI. Cette modale bloquante au libellé recyclé
    // (« You can't create items yet. », §3.1) était affichée par le handler NATIF
    // du 0x018D — celui qui ne tourne plus dès qu'on prend la main. Il n'y a donc
    // plus rien à escamoter.
    //
    // Le garder aurait même été NUISIBLE : l'escamotage se désarmait au prochain
    // appel de la fonction native, escamoté ou non — sans appel, le drapeau restait
    // armé et guettait la prochaine modale portant ce même texte. Un armement qui ne
    // peut plus être consommé est un piège en attente. C'est pourquoi le module a
    // été supprimé et non simplement laissé en place (docs §3.1 bis le conserve).
    return;
  }

  if (opcode == kOpMakeResult) {
    if (len < kMakeResultLen) return;
    uint16_t result = 0;
    uint32_t nameid = 0;
    if (!ReadMakeResult(data, &result, &nameid)) return;
    const bool was_ours = awaiting_result_;
    awaiting_result_ = false;
    sent_owned_      = -1;  // le serveur a parlé : la sonde d'inventaire est sans objet
    sent_mat_id_     = 0;
    sent_mat_owned_  = -1;
    // Un ÉCHEC compte aussi : il a consommé les matériaux, c'est une tentative
    // faite. Décompter les seules réussites relancerait indéfiniment sur une
    // recette malchanceuse — l'inverse de ce que « ×20 » demande.
    if (was_ours && batch_left_ > 0) --batch_left_;
    LogResult(result, nameid);
    RebuildOwnedCounts();
    // ⚠ La garde utile n'est pas un plafond de relances, c'est la CAUSALITÉ :
    // on ne relance que sur un résultat qui répond à une demande de NOUS. Un
    // 0x018F provoqué par autre chose ne doit rien déclencher.
    if (was_ours) ScheduleAutoRecast();
    return;
  }

  if (opcode == kOpSkillFail) {
    // Plusieurs sorties serveur répondent par ZC_ACK_TOUSESKILL au lieu de
    // 0x018F. Sans l'observer, une demande refusée laisserait la fenêtre « en
    // attente du serveur… » pour une réponse déjà arrivée.
    //
    // ⚠ On ne peut PAS filtrer sur l'id de compétence : elle n'est nulle part
    // dans les paquets de liste (un même 0x01AD sert quatre métiers, cf. §2.2) et
    // on ne la capte pas encore. La causalité tenue ici est donc TEMPORELLE — ce
    // refus doit suivre de près une demande de NOUS. Sans cette borne, un échec
    // de compétence sans rapport (un sort raté pendant qu'on attend) viderait
    // l'attente et écrirait une ligne mensongère dans le journal.
    if (!awaiting_result_) return;
    if (!sent_at_ || GetTickCount() - sent_at_ > kSkillFailWindowMs) return;
    awaiting_result_ = false;
    Log("Demande refusée par le serveur.", kColWarn);
    // Un REFUS n'est pas une tentative : la condition qui l'a causé ne changera
    // pas d'elle-même, relancer tournerait en rond en brûlant du SP.
    auto_recast_at_   = 0;
    // La série tombe avec la relance : sans nouvelle liste, il n'y a plus de tour
    // possible, et laisser un reste-à-faire ferait repartir la série à la
    // prochaine ouverture manuelle.
    batch_left_       = 0;
    batch_fire_       = false;
    if (auto_chain_ > 0) auto_stop_reason_ = "Refus serveur : relance arrêtée.";
    return;
  }
}

void MakeItemWindow::ResolveMaterials(Entry& e) const {
  e.mat_count     = 0;
  e.mats_resolved = false;

  // La liste ouverte décide de l'espace de clés à interroger (cf. `arrow_list_`,
  // posé à l'arrivée du paquet) : sans ce choix, la fenêtre du Hunter affichait la
  // recette de FORGE d'un id qui se trouve être aussi une source de flèches.
  const uint32_t key = arrow_list_ ? e.id + kArrowRecipeKeyBias : e.id;

  char lines[kMaxRecipeLines][kRecipeLineMax];
  const int count = ReadRecipeLines(key, lines, kMaxRecipeLines);
  if (count == 0) return;  // recette inconnue du client

  const MoonlightUi* ui = Bourgeon::Instance().moonlight_ui();
  bool all_resolved = true;
  for (int i = 0; i < count && e.mat_count < kMaxMaterials; ++i) {
    int      qty = 0;
    uint32_t explicit_id = 0;
    char     mat_name[sizeof(Material::name)];
    // Les lignes qui ne commencent pas par un nombre sont du texte de titre
    // (« Iron's required materials ») : on les saute sans rien en conclure.
    bool     not_consumed = false;
    bool     is_output    = false;
    if (!ParseRecipeLine(lines[i], &qty, mat_name, sizeof(mat_name), &explicit_id,
                         &not_consumed, &is_output))
      continue;

    Material& m = e.mats[e.mat_count++];
    m.qty          = qty;
    m.not_consumed = not_consumed;
    m.is_output    = is_output;
    std::snprintf(m.name, sizeof(m.name), "%s", mat_name);
    // L'id ÉCRIT dans la recette fait foi ; la recherche par nom n'est qu'un
    // repli, et elle dépend de la langue du serveur et de l'orthographe.
    m.id = explicit_id ? explicit_id : (ui ? ui->ItemIdByName(mat_name) : 0);
    if (!m.id) all_resolved = false;
  }
  // ⚠ Une recette de flèches n'a AUCUN matériau à contrainte : la ligne de
  // consommation vaut 1 et le reste est du rendement. `mats_resolved` reste donc
  // vrai (les ids sont là) et ComputeCraftable rendra le stock du matériau, ce
  // qui est exactement la bonne réponse — une fabrication en consomme un.
  e.mats_resolved = (e.mat_count > 0) && all_resolved;
}

void MakeItemWindow::RebuildOwnedCounts() {
  for (Entry& e : entries_) {
    e.owned     = OwnedCount(e.id);
    e.craftable = ComputeCraftable(e);
  }
}

int MakeItemWindow::ComputeCraftable(const Entry& e) const {
  // Sans TOUS les matériaux résolus, aucun calcul n'est honnête : il manquerait
  // une contrainte, et le minimum serait surestimé.
  if (!e.mats_resolved) return -1;

  int best = -1;
  for (int i = 0; i < e.mat_count; ++i) {
    const Material& m = e.mats[i];
    if (m.qty <= 0) continue;
    // Un RENDEMENT n'est pas une exigence : il ne borne rien.
    if (m.is_output) continue;
    // 🔴 Un matériau NON CONSOMMÉ ne borne PAS le nombre de fabrications : le
    // serveur exige seulement de le posséder (`pc_search_inventory < 0` → refus),
    // il ne le retire pas. Un seul guide de fabrication autorise donc une
    // infinité de potions — le compter comme une contrainte plafonnait le
    // « Faisable » à 1 sur toutes les recettes à guide.
    if (m.not_consumed) {
      if (OwnedCount(m.id) <= 0) return -1;  // absent : le serveur refusera
      continue;
    }
    const int possible = OwnedCount(m.id) / m.qty;
    if (best < 0 || possible < best) best = possible;
  }
  if (best < 0) return -1;
  // 🔴 Le serveur a déjà validé qu'on peut en faire au moins UN — sinon le produit
  // ne serait pas dans la liste (`skill_can_produce_mix` vérifie les quantités).
  // Un 0 vient donc de NOUS : recette client périmée, ou nom résolu vers le mauvais
  // objet. Se taire vaut mieux que d'annoncer « 0 » sur une fabrication que le
  // joueur peut manifestement lancer.
  if (best == 0) return -1;
  return best;
}

// ── Cycle de vie ─────────────────────────────────────────────────────────────

void MakeItemWindow::OnModeSwitch(ModeMgr::ModeType mode_type,
                                  const char* map_name) {
  // Quitter le monde de jeu referme tout et jette le modèle : les ids d'objet et
  // le menuskill armé n'ont plus aucun sens ailleurs.
  if (mode_type == ModeMgr::ModeType::kGame) return;
  ui_open_ = false;
  entries_.clear();
  proto_ = Proto::kNone;
  awaiting_result_ = false;
  // On n'envoie RIEN ici, contrairement à la bascule d'interrupteur : quitter le
  // monde de jeu passe par `unit_remove_map_`, qui remet lui-même `menuskill_id` à
  // zéro côté serveur. Le drapeau doit juste cesser de mentir.
  list_armed_ = false;
  relaunch_sent_at_   = 0;
  relaunch_retries_   = 0;
  reuse_owned_before_ = -1;
}

void MakeItemWindow::OnTick() {
  // 🔴 BASCULE de l'interrupteur : désarmer AVANT de jeter la session.
  //
  // Testé AVANT le retour anticipé ci-dessous, sinon le cas « on vient de nous
  // désactiver » — le seul qui compte ici — ne serait jamais vu.
  //
  // Sans ce -1, le serveur garde son `menuskill` armé et le personnage ne peut plus
  // lancer AUCUNE compétence, ni en moderne ni en natif ; rebasculer l'interrupteur
  // n'y change rien, puisque le blocage est côté serveur. Avant le remplacement
  // d'opcode le cas était masqué : la fenêtre native existait encore et reprenait la
  // main. Elle ne naît plus. (Même correctif que WeaponRefineWindow, où le cas a
  // été constaté en jeu.)
  //
  // `list_armed_` est le bon drapeau, et le seul : il est posé sur le COMPTE du
  // paquet (le serveur n'arme que `if (count > 0)`) et retiré à l'envoi (le serveur
  // fait alors son propre `clif_menuskill_clear`). On POSE l'action — FlushPending,
  // qui tourne même plugin coupé, l'enverra hors frame ImGui.
  if (prev_enabled_ != imgui_enabled_) {
    prev_enabled_ = imgui_enabled_;
    if (!imgui_enabled_ && list_armed_) pending_ = Pending::kCancel;
    // (Le sens INVERSE — natif -> moderne avec une fenêtre native déjà ouverte —
    // n'a RIEN à faire ici : c'est FlushPending qui rend cette session, à chaque
    // frame et sans drapeau. Le poser depuis ce tick-ci était le bogue : limité à
    // 100 ms, il arrivait toujours après FlushPending, qui avait déjà détruit la
    // fenêtre sans rien envoyer.)
  }
  if (!imgui_enabled_) return;
  // 🔴 PLUS DE MASQUAGE DE RATTRAPAGE ICI, et c'est délibéré.
  //
  // Il n'y a plus rien à masquer : une fenêtre de liste native ne peut plus vivre
  // qu'une seule frame, celle qui sépare sa découverte du `CancelNativeIfClass` de
  // FlushPending. Et surtout, masquer était le PIRE des deux échecs possibles — si
  // le pilotage de l'Annuler ratait, on retombait sur le fantôme invisible tenant
  // une session armée, exactement le bug qu'on vient de corriger deux fois. Une
  // fenêtre native VISIBLE une frame de trop est un défaut cosmétique ; un fantôme
  // bloque le personnage.

  // ── Sonde d'INVENTAIRE : constater ce que le serveur ne dit pas ───────────
  // 🔴 `skill_produce_mix` n'appelle `clif_produceeffect` que pour six
  // compétences (RK_RUNEMASTERY, GN_MIX_COOKING, GN_MAKEBOMB, GN_S_PHARMACY,
  // MT_M_MACHINE, BO_BIONIC_PHARMACY). SA_CREATECON, AM_PHARMACY, la forge et
  // les flèches n'y figurent pas : l'objet est bel et bien créé (`pc_additem`),
  // et AUCUN 0x018F ne part. ⏱ Vérifié en jeu — le chat écrit « You got Stingor's
  // Flame Elemental Converter (1) » pendant que la fenêtre attend encore.
  //
  // On constate donc dans l'INVENTAIRE ce que le protocole ne rapporte pas. La
  // référence est le stock relevé avant l'envoi ; la mise à jour d'inventaire,
  // elle, arrive toujours.
  if (awaiting_result_ && sent_at_ && sent_owned_ >= 0 && last_sent_id_ &&
      static_cast<int>(GetTickCount() - sent_at_) >
          static_cast<int>(kNoAnswerProbeMs)) {
    const int now_owned = OwnedCount(last_sent_id_);
    // 🔴 Le stock de l'id envoyé peut varier dans les DEUX SENS, parce que cet id
    // ne désigne pas la même chose selon la compétence :
    //   - pharmacie, convertisseurs, forge : c'est le PRODUIT → le stock MONTE ;
    //   - fabrication de FLÈCHES : c'est le MATÉRIAU → `skill_arrow_create` fait
    //     `pc_delitem` dessus, donc le stock BAISSE (et le produit obtenu n'est
    //     nulle part dans notre demande : il vit dans `skill_arrow_db`).
    // Guetter une seule hausse faisait donc conclure « le serveur n'a pas
    // répondu » sur un craft de flèches parfaitement réussi. Ce qui prouve que le
    // serveur a agi, c'est le CHANGEMENT, pas son signe.
    if (now_owned != sent_owned_) {
      // ⚠ La différence se calcule AVANT la remise à zéro. Écrit dans l'autre
      // ordre, le message annonçait « +12 » pour un objet produit à 11
      // exemplaires : il soustrayait -1, c'est-à-dire le stock total plus un.
      const int gained = now_owned - sent_owned_;
      const int before = sent_owned_;
      awaiting_result_ = false;
      sent_owned_      = -1;
      sent_mat_id_     = 0;
      sent_mat_owned_  = -1;
      RebuildOwnedCounts();
      // On dit d'où vient l'information : ce n'est PAS le serveur qui l'annonce,
      // et un joueur qui compare avec le natif doit pouvoir comprendre pourquoi
      // le message diffère.
      // Le nombre OBTENU mérite d'être écrit tel quel : sur ce serveur, une
      // fabrication peut rendre plusieurs exemplaires d'un coup (bonus aléatoire
      // maison sur AM_PHARMACY / SA_CREATECON / AL_HOLYWATER / ASC_CDP, jusqu'à
      // +4). Le natif ne l'a jamais dit — et sans paquet de résultat, l'inventaire
      // est même la SEULE source qui puisse le rapporter.
      char line[192];
      const char* subject = last_sent_name_[0] ? last_sent_name_ : "Objet";
      // ⚠ Pas de « ! » : un rendement supérieur à 1 n'est pas toujours un coup de
      // chance. `clif_parse_Cooking` fabrique `menuskill_val2` exemplaires d'un
      // seul coup — 10 pour GN_MAKEBOMB et GN_MIX_COOKING au-delà du niveau 1 —
      // et rien ne distingue ce lot NORMAL du tirage aléatoire maison. On énonce
      // donc le nombre sans l'interpréter.
      if (gained > 1)
        std::snprintf(line, sizeof(line), i18n::Tr("%s créé ×%d"), subject, gained);
      else if (gained == 1)
        std::snprintf(line, sizeof(line), i18n::Tr("%s créé."), subject);
      else
        // Stock en BAISSE : l'id envoyé était le matériau (flèches). On ne peut
        // pas nommer le rendement — le produit n'est ni dans notre demande ni
        // dans la réponse — donc on ne l'invente pas.
        std::snprintf(line, sizeof(line), i18n::Tr("%s transformé."), subject);
      last_result_       = line;
      last_result_color_ = kColOk;
      Log(line, kColOk);
      LogDiag("[make] succès CONSTATÉ (aucun 0x018F) : id={} {} -> {} (+{})",
              last_sent_id_, before, now_owned, gained);
      // 🔴 Décompter la série ICI AUSSI. Le décompte ne vivait que dans le
      // handler de 0x018F — or ce paquet n'arrive JAMAIS sur ces compétences :
      // `batch_left_` restait à sa valeur de départ et la chaîne ne s'arrêtait
      // plus, même avec une quantité de 1.
      // Un tirage généreux ne compte pas plusieurs tours : c'est le nombre de
      // FABRICATIONS demandées qui est en jeu, pas le nombre d'objets obtenus.
      if (batch_left_ > 0) --batch_left_;
      // Et c'est ce qui rend la relance et la série possibles sur ces
      // compétences : elles s'armaient jusqu'ici sur un paquet qui n'arrive pas.
      ScheduleAutoRecast();
    } else if (sent_mat_id_ && sent_mat_owned_ >= 0 &&
               OwnedCount(sent_mat_id_) < sent_mat_owned_) {
      // ── ÉCHEC CONSTATÉ ───────────────────────────────────────────────────
      // Les matériaux ont baissé mais le produit n'a pas bougé : le serveur a
      // bel et bien traité la demande, et le tirage a raté. C'est une CONCLUSION,
      // pas une supposition — et elle tombe en même temps que le succès plutôt
      // qu'au bout du délai de garde.
      //
      // ⚠ Ce délai reste indispensable pour l'autre silence : une demande REFUSÉE
      // à la revalidation ne consomme rien, donc rien ne bouge et rien ne prouve
      // quoi que ce soit. Les deux cas se distinguent enfin.
      const int spent = sent_mat_owned_ - OwnedCount(sent_mat_id_);
      awaiting_result_ = false;
      sent_owned_      = -1;
      RebuildOwnedCounts();
      char line[192];
      const char* subject = last_sent_name_[0] ? last_sent_name_ : "Objet";
      std::snprintf(line, sizeof(line), i18n::Tr("%s : échec, matériaux perdus."), subject);
      last_result_       = line;
      last_result_color_ = kColWarn;
      Log(line, kColWarn);
      LogDiag("[make] échec CONSTATÉ (aucun paquet) : produit {} inchangé, "
              "matériau {} -{}",
              last_sent_id_, sent_mat_id_, spent);
      sent_mat_id_    = 0;
      sent_mat_owned_ = -1;
      // 🔴 Un échec ne CLÔT PAS la série : contrairement au silence d'un refus,
      // sa cause est un tirage, et elle change au coup suivant. S'arrêter là
      // reviendrait à interrompre une série de 20 au premier raté — c'est-à-dire
      // précisément quand enchaîner a le plus de sens.
      if (batch_left_ > 0) --batch_left_;
      ScheduleAutoRecast();
    }
  }

  // ── Le serveur peut ne jamais répondre ────────────────────────────────────
  // Comparaison par SOUSTRACTION, comme partout ailleurs : GetTickCount reboucle
  // au bout de 49 jours, et un « >= » laisserait alors l'échéance ne jamais
  // tomber sur un client resté ouvert si longtemps.
  if (awaiting_result_ && sent_at_ &&
      static_cast<int>(GetTickCount() - sent_at_) >
          static_cast<int>(kAwaitResultTimeoutMs)) {
    awaiting_result_ = false;
    sent_owned_      = -1;
    sent_mat_id_     = 0;
    sent_mat_owned_  = -1;
    // Une série ou une chaîne ne se poursuit PAS sur un silence : la condition
    // qui l'a causé ne changera pas d'elle-même.
    auto_recast_at_ = 0;
    batch_left_     = 0;
    batch_fire_     = false;
    // 🔴 Ce message accusait un REFUS, et c'était le mauvais coupable dans la
    // plupart des cas. Le serveur est muet sur DEUX issues bien distinctes :
    //   • la fabrication a ÉCHOUÉ (le tirage a raté) — de loin le cas le plus
    //     fréquent dès que la chance affichée est basse. Les matériaux sont
    //     consommés, rien n'est créé, et aucun paquet ne part : pour la cuisine
    //     `skill_produce_mix` se contente d'un `clif_specialeffect(EF_COOKING_FAIL)`
    //     et d'une baisse de maîtrise ;
    //   • la demande a été REFUSÉE à la revalidation — alors rien n'est consommé.
    // Les deux se ressemblent d'ici, mais elles n'appellent pas la même réaction :
    // on nomme donc les deux, la plus probable en premier, plutôt que d'envoyer le
    // joueur inspecter un inventaire qui n'a rien à se reprocher.
    const int dish_lv = craftdata::RecipeItemLevel(last_sent_id_);
    const bool is_dish = dish_lv >= 11 && dish_lv <= 20;
    auto_stop_reason_ =
        is_dish
            ? i18n::Tr("Pas de réponse : la fabrication a probablement ÉCHOUÉ. La cuisine "
              "ne renvoie aucun message quand elle rate - seulement un effet "
              "visuel. Les matériaux sont consommés et la maîtrise culinaire "
              "baisse. Regarde les chances annoncées avant de recommencer. (Une "
              "demande refusée à la revalidation aurait le même silence, mais "
              "n'aurait rien consommé.)") : i18n::Tr("Pas de réponse : la fabrication a probablement ÉCHOUÉ - ces "
              "compétences ne renvoient aucun message, ni en cas de succès ni en "
              "cas d'échec. Autre possibilité : une demande refusée à la "
              "revalidation des matériaux, qui elle n'aurait rien consommé.");
    Log(is_dish ? i18n::Tr("Échec probable — la cuisine ne dit rien quand elle rate.") : i18n::Tr("Aucune réponse du serveur — échec probable."),
        kColWarn);
    LogDiag("[make] TIMEOUT: aucun 0x018F ni 0x0110 après {} ms (produit {})",
            kAwaitResultTimeoutMs, last_sent_id_);
  }

  // Relance automatique : différée ici, jouée hors frame ImGui par FlushPending.
  if (auto_recast_at_ && GetTickCount() >= auto_recast_at_) {
    auto_recast_at_ = 0;
    // Deux relances, deux natures : re-lancer une compétence ne coûte que du SP,
    // ré-utiliser l'objet le DÉTRUIT. C'est `from_item_` qui tranche, décidé à
    // l'arrivée de la liste.
    pending_ = from_item_ ? Pending::kReuseItem : Pending::kRecast;
  }

  // ── Chien de garde de la relance ───────────────────────────────────────────
  // La relance est partie et AUCUNE liste n'est revenue. Contrairement au refine, on
  // ne peut pas s'appuyer sur le ZC 0x0110 pour le savoir : son paquet ne porte pas
  // l'identifiant de compétence exploitable ici (cf. le parseur de kOpSkillFail), et
  // un refus sans rapport passerait pour le nôtre. Le temps, lui, ne se trompe pas.
  if (relaunch_sent_at_ &&
      static_cast<int>(GetTickCount() - relaunch_sent_at_) >
          static_cast<int>(kRelaunchNoListMs)) {
    RetryRelaunch();
  }

  // Tour suivant d'une série. Ici et pas dans le handler de paquet : RequestMake
  // aboutit à un envoi natif, qui doit rester hors du chemin de réception comme
  // hors de la frame ImGui (il ne fait que poser `pending_`, que FlushPending
  // jouera). `awaiting_result_` évite de doubler une demande encore en vol.
  if (batch_fire_ && !awaiting_result_) {
    batch_fire_ = false;
    RequestMake();
  }
}

void MakeItemWindow::HideNativeAtCreation(void* win) {
  if (!imgui_enabled_) return;
  // On ne sait pas ici QUEL id nous vaut cet appel : on teste les deux vtables,
  // ce qui est de toute façon la garde qu'il faut (§ « vérifier la classe »).
  HideIfClass(win, kVTableMakingArrow);
  HideIfClass(win, kVTableMakeTarget);
  HideIfClass(win, kVTableMakeProcess);
}

bool MakeItemWindow::WantsEnterKey() const {
  // ⚠ Confisquer la touche et autoriser l'action sont deux questions DISTINCTES.
  // Ce prédicat ne regarde QUE l'ouverture de la fenêtre — c'est la règle d'Échap :
  // une fenêtre RO ouverte s'approprie la touche, point. Le calquer sur l'état du
  // bouton laisserait repasser la touche dans tous les creux du cycle (demande en
  // vol, liste consommée, relance en cours) et le chat s'ouvrirait par à-coups.
  // Et il est SANS état de rendu : un drapeau posé à la frame précédente resterait
  // vrai pendant un chargement de carte, confisquant la touche pour un client qui
  // n'affiche plus rien.
  //
  // `enter_key_` en tête : quand le réglage est décoché, la touche n'est PAS
  // confisquée du tout et le chat reste accessible pendant qu'on fabrique. C'est
  // le défaut depuis que la série ×N existe — enchaîner ne demande plus de
  // marteler Entrée, donc la confiscation coûtait plus qu'elle ne rapportait.
  if (!enter_key_) return false;
  const Bourgeon& app = Bourgeon::Instance();
  return imgui_enabled_ && ui_open_ && app.IsGameActive() && !app.IsMapLoading();
}

// ── Envois natifs (TOUS depuis FlushPending, jamais depuis OnRenderUI) ────────

void MakeItemWindow::SendConfirm(uint32_t item_id) {
  // ⏱ Le journal traçait la RÉCEPTION des listes et rien de l'ÉMISSION, si bien
  // qu'un « le serveur n'a pas répondu » ne disait pas si la demande était partie,
  // ni avec quels paramètres. Or c'est exactement ce qu'il faut savoir : les trois
  // protocoles n'envoient pas la même chose, et `mk_type` décide à lui seul de la
  // branche que prendra `clif_parse_Cooking`.
  LogDiag("[make] envoi proto={} id={} mk_type={} (forge {}/{}/{})",
          static_cast<int>(proto_), item_id, mk_type_, forge_slot_[0],
          forge_slot_[1], forge_slot_[2]);
  switch (proto_) {
    case Proto::kArrow:
      SendModeCmd(kCmdMakeArrow, static_cast<int>(item_id));
      break;
    case Proto::kMaking:
      SendModeCmd(kCmdMakeItem, static_cast<int>(item_id), mk_type_);
      break;
    case Proto::kProduce:
      // ⚠ Ici le natif BIFURQUE, et le critère qu'il emploie est le MAUVAIS : la
      // fenêtre 79 décide d'ouvrir la 80 selon le JOB (variantes de Rune Knight ou
      // pas). Or c'est le PRODUIT qui commande — la fenêtre 80 elle-même ne
      // dessine ses trois emplacements que si `(unsigned)(produit - 994) > 6`.
      //
      // 🔴 ON N'OUVRE PLUS LA FENÊTRE 80. Première rédaction : on lui passait la
      // main pour les armes, au motif qu'elle seule savait porter les Star Crumb
      // et les pierres élémentaires. C'était FAUX, et la décompilation de la 79 le
      // dit noir sur blanc : `CMode::SendMsg(130, itemId, ItemSkillInfo mats[3])`
      // reçoit les trois matériaux **en paramètre**. La 80 ne fait que les
      // collecter à l'écran avant d'appeler la même commande.
      //
      // Ne pas l'ouvrir est même PLUS SÛR que le natif : les matériaux qu'on y
      // dépose sortent RÉELLEMENT de l'inventaire client (son Annuler les rend un
      // par un, `Inventory_AddOrStackItem`), si bien que toute sortie non prévue —
      // warp, fermeture par le gestionnaire, plantage — perd leur affichage
      // jusqu'au prochain rafraîchissement. En envoyant nous-mêmes, rien ne bouge
      // côté client : c'est le SERVEUR qui consomme, comme pour tout le reste.
      //
      // Bénéfice second, celui que l'utilisateur constatait : la 80 ne vient plus
      // parasiter la chaîne de relance en s'ouvrant entre deux fabrications.
      SendProduceCmd(static_cast<int>(item_id), forge_slot_);
      break;
    default:
      return;
  }
}

void MakeItemWindow::SendCancel() {
  // L'annulation est OBLIGATOIRE : c'est elle qui désarme le `menuskill` côté
  // serveur (clif_menuskill_clear n'est appelé qu'à réception de la réponse).
  //
  // 🔴 Et les trois protocoles n'ont PAS la même force de désarmement :
  //   • CZ 0x01AE (kArrow)  : `clif_parse_SelectArrow` efface INCONDITIONNELLEMENT,
  //     quel que soit le menuskill armé — c'est un désarmeur universel ;
  //   • CZ 0x025B (kMaking) : `clif_parse_Cooking` efface aussi sans condition
  //     (hors `type == 6`, qui n'est pas un de nos chemins) ;
  //   • CZ 0x018E (kProduce): `clif_parse_ProduceMix` n'efface QUE pour
  //     -1 / AM_PHARMACY / RK_RUNEMASTERY / GC_RESEARCHNEWPOISON, et sort par
  //     `default: return` sinon.
  //
  // Ce dernier reste néanmoins suffisant, et pour une raison qu'il faut écrire :
  // les SEULES sessions qui ouvrent la fenêtre 79 sont précisément celles-là —
  // `produce N;` d'un script d'objet (menuskill = -1), AM_PHARMACY
  // (preparepotion.cpp), GC_CREATENEWPOISON (renommé GC_RESEARCHNEWPOISON à
  // l'envoi) et RK_RUNEMASTERY. Donc `proto_ == kProduce` implique un menuskill
  // dans la liste blanche… À CONDITION que `proto_` décrive bien la session
  // ARMÉE, ce qui est exactement l'invariant que défend le garde « liste vide »
  // d'OnRecvPacket. C'est là, et nulle part ici, que se joue le désarmement.
  LogDiag("[make] annulation proto={} mk_type={}", static_cast<int>(proto_),
          mk_type_);
  switch (proto_) {
    case Proto::kArrow:  SendModeCmd(kCmdMakeArrow, -1); break;
    case Proto::kMaking: SendModeCmd(kCmdMakeItem, -1, mk_type_); break;
    case Proto::kProduce: SendProduceCmd(0); break;  // itemId = 0 = annulation
    default: break;
  }
}

void MakeItemWindow::NotifySkillCast(int skill_id, int skill_lv) {
  // Un id nul ou négatif signe une lecture ratée (0x71 dont la CSkillInfo n'a pas
  // pu être lue) : mieux vaut garder l'ancien que d'écraser avec du bruit.
  if (skill_id <= 0) return;
  skill_id_      = skill_id;
  skill_lv_      = (skill_lv > 0) ? skill_lv : 1;
  skill_cast_at_ = GetTickCount();
}

void MakeItemWindow::NotifyItemUse(unsigned item_index) {
  // Le paquet ne porte QUE l'index ; on résout ici, tant que l'objet existe
  // encore — le serveur est sur le point de le détruire, après quoi l'index
  // désignera autre chose. La résolution passe par le parcours d'inventaire
  // vérifié (cf. InvIdByIndex), pas par la Session.
  // Première tentative avec l'offset connu ; s'il ne l'est pas encore (ou s'il
  // ne donne rien), on le fait établir par l'usage qu'on est en train d'observer.
  uint32_t item_id = InvIdByIndex(item_index);
  if (item_id == 0) {
    int hits[kIndexCandidateCount] = {0};
    const int found = DetectIndexOffset(item_index, hits);
    if (found >= 0) {
      g_index_offset = found;
      item_id = InvIdByIndex(item_index);
      LogDiag("[make] offset d'index DÉTECTÉ : node+0x{:02X} (index {} -> id {})",
              found, item_index, item_id);
    } else {
      LogDiag("[make] offset d'index INTROUVABLE pour index={} — occurrences "
              "0x10:{} 0x0C:{} 0x14:{} 0x08:{} 0x1C:{} 0x20:{}",
              item_index, hits[0], hits[1], hits[2], hits[3], hits[4], hits[5]);
    }
  }

  // Observation pure, comme pour les compétences : on note, on ne juge pas ici.
  // Tout usage d'objet passe par là (potions comprises) — c'est l'arrivée d'une
  // liste, et elle seule, qui décidera si celui-ci comptait.
  //
  // ⚠ On enregistre l'horodatage MÊME si la résolution a échoué : sans ça, un
  // échec d'offset se confondrait avec « aucun objet utilisé », et le journal ne
  // pourrait pas faire la différence entre les deux — c'est-à-dire précisément la
  // question qu'on cherche à trancher.
  item_id_        = item_id;
  item_use_index_ = item_index;
  item_use_at_    = GetTickCount();

  item_probe_[0] = '\0';
  if (item_id == 0) {
    const InvProbe p = ProbeFirstInvNode();
    if (p.ok)
      std::snprintf(item_probe_, sizeof(item_probe_),
                    i18n::Tr("1er nœud : id=%u +0x0C=%u +0x10=%u +0x14=%u amt(+0x18)=%d"),
                    p.id, p.v0c, p.v10, p.v14, p.amt);
    else
      std::snprintf(item_probe_, sizeof(item_probe_),
                    i18n::Tr("inventaire illisible (tête nulle ou accès refusé)"));
  }

  // On ne journalise QUE l'anomalie. Tracer chaque usage d'objet noyait la
  // console sous les potions ; une ligne qui sort toujours ne se lit plus. En
  // échec, en revanche, elle porte la sonde — c'est ce qui a permis d'identifier
  // l'offset en un seul aller-retour.
  if (item_id == 0)
    LogDiag("[make] CZ_USE_ITEM index={} NON RESOLU | {}", item_index,
            item_probe_);
}

void MakeItemWindow::SendRecast() {
  if (skill_id_ <= 0) return;
  const uint32_t aid = OwnAid();
  if (!aid) return;
  // Cible = soi : toutes les compétences concernées sont des self-cast, et 0x45
  // impose la cible (⚠ à ne PAS employer pour une compétence ciblée, cf.
  // reference_cmode_sendmsg_use_skill — Arrow Vulcan partait sur soi-même).
  // Le NIVEAU est celui du lancement observé, pas 1 : c'est lui que le serveur
  // range dans `menuskill_val` et qui décide de ce que la liste contiendra.
  SendModeCmd(kCmdUseSkill, skill_id_, static_cast<int>(aid), skill_lv_);
  // 🔴 On remet l'horloge à l'heure NOUS-MÊMES, exactement comme SendReuseItem le
  // fait côté objet. Notre envoi ne repasse PAS par l'observation : le hook de
  // CMode::SendMsg ne note qu'au niveau le plus externe (`g_send_msg_depth == 1`)
  // et cet appel-ci part d'OnProcessInput, donc imbriqué.
  // Sans ça, `skill_cast_at_` vieillissait pendant toute la chaîne : passé les
  // 3 s de kSkillCastWindowMs, la liste suivante était prise pour un script
  // d'OBJET (⏱ vu en jeu : « from_item=1 … skill=1007 il y a 4719 ms »), et la
  // relance s'arrêtait sur « objet d'origine inconnu ».
  skill_cast_at_ = GetTickCount();
}

void MakeItemWindow::SendReuseItem() {
  if (source_item_id_ == 0) return;

  // ⚠ On repart de l'IDENTIFIANT, jamais de l'index capté à l'observation :
  // l'exemplaire précédent vient d'être détruit et l'inventaire a pu se
  // compacter, si bien que le même index désignerait maintenant un AUTRE objet —
  // qu'on consommerait à la place. On refait donc la résolution sur l'inventaire
  // COURANT : soit l'objet y est encore, soit on n'envoie rien.
  // ⚠ Garde de STOCK, faite AVANT l'envoi et sur un champ vérifié (l'id et la
  // quantité, pas l'index) : si la pile est épuisée, son index a pu être réattribué
  // à un AUTRE objet, et envoyer là-dessus consommerait n'importe quoi. C'est le
  // seul scénario où la relance pouvait détruire autre chose que ce que le joueur
  // a demandé — il est fermé ici, indépendamment de la résolution d'index.
  if (OwnedCount(source_item_id_) <= 0) {
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  i18n::Tr("Plus de %s en inventaire : relance arrêtée (%d consommés)."),
                  source_item_name_, auto_items_used_);
    auto_stop_reason_ = msg;
    auto_ours_        = false;
    LogDiag("[make] SendReuseItem: stock épuisé pour id={}", source_item_id_);
    return;
  }

  const unsigned idx  = InvIndexById(source_item_id_);
  const bool     sent = SendUseItemPacket(idx, OwnAid());
  LogDiag("[make] SendReuseItem: id={} -> index={} aid={} envoyé={}",
          source_item_id_, idx, OwnAid(), sent ? 1 : 0);
  if (!sent) {
    // Stock épuisé : c'est la fin NORMALE d'une chaîne par objet, et le seul
    // arrêt qui compte vraiment pour le joueur — on le nomme.
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  i18n::Tr("Plus de %s en inventaire : relance arrêtée (%d consommés)."),
                  source_item_name_, auto_items_used_);
    auto_stop_reason_ = msg;
    auto_ours_        = false;
    return;
  }

  ++auto_items_used_;
  // Notre envoi contourne le hook d'observation (il passe par SendPacketRef) :
  // sans cette remise à l'heure, la liste suivante paraîtrait n'avoir suivi aucun
  // usage d'objet et la chaîne se couperait après un seul tour.
  item_use_at_ = GetTickCount();

  char line[192];
  if (auto_reuse_max_ > 0)
    std::snprintf(line, sizeof(line), "Relance : %s consommé (%d/%d).",
                  source_item_name_, auto_items_used_, auto_reuse_max_);
  else
    std::snprintf(line, sizeof(line), i18n::Tr("Relance : %s consommé (%d au total)."),
                  source_item_name_, auto_items_used_);
  Log(line, kColWarn);
}

void MakeItemWindow::ScheduleAutoRecast() {
  // ── Deux réglages ORTHOGONAUX, à ne pas fusionner ─────────────────────────
  // 🔴 Une version a barré la relance quand la série était finie (`batch_left_
  // <= 0`). C'était une sur-correction : elle rendait « 2 fabrications » = TROIS
  // clics (Fabriquer, Relancer, Fabriquer), alors que la case « relancer
  // automatiquement » existe précisément pour épargner celui du milieu.
  //   - « relancer automatiquement » ROUVRE la liste après chaque fabrication ;
  //   - la QUANTITÉ dit combien de fois la fabrication part TOUTE SEULE.
  // À 1 avec la case cochée : une fabrication automatique, puis la liste revient
  // et le joueur reclique — c'est le comportement attendu, et c'est ce que le
  // vrai défaut (le décompte manquant sur le chemin sans paquet) masquait.
  // ── Liste ouverte par un OBJET ───────────────────────────────────────────
  // Relancer est ici DESTRUCTIF : le Mini Furnace et les marteaux sont des
  // `Usable`, et `pc_useitem` fait `pc_delitem(..., LOG_TYPE_CONSUME)` AVANT
  // d'exécuter le script — donc chaque ouverture de liste coûte un exemplaire,
  // même si la fabrication est ensuite annulée ou ratée. Ce n'est pas une raison
  // d'interdire, c'en est une d'exiger un réglage à part, un plafond et un
  // compteur visible. Le réglage « relancer la compétence » ne suffit JAMAIS à
  // autoriser ça.
  LogDiag("[make] ScheduleAutoRecast: from_item={} auto_recast={} "
          "auto_reuse_item={} source_id={} utilises={} max={}",
          from_item_ ? 1 : 0, auto_recast_ ? 1 : 0, auto_reuse_item_ ? 1 : 0,
          source_item_id_, auto_items_used_, auto_reuse_max_);

  // Une série ne survit pas à un refus de relance : sans nouvelle liste il n'y a
  // plus de tour possible, et un reste-à-faire oublié repartirait à la prochaine
  // ouverture manuelle. Chaque sortie sans programmation la solde donc.
  if (from_item_) {
    if (!auto_reuse_item_) {  // réglage non coché : silence, la série s'éteint
      batch_left_ = 0;
      return;
    }
    if (source_item_id_ == 0) {
      auto_stop_reason_ =
          "Objet d'origine inconnu : relance impossible. (La liste a été ouverte "
          "avant que le client ne puisse l'observer.)";
      batch_left_ = 0;
      return;
    }
    // auto_reuse_max_ == 0 : aucune limite demandée. La chaîne s'arrêtera sur une
    // condition RÉELLE (stock épuisé, liste vide, refus serveur, fermeture) —
    // pas sur un compte arbitraire.
    if (auto_reuse_max_ > 0 && auto_items_used_ >= auto_reuse_max_) {
      char msg[192];
      std::snprintf(msg, sizeof(msg),
                    i18n::Tr("Plafond atteint : %d %s consommés. Relance arrêtée."),
                    auto_items_used_, source_item_name_);
      auto_stop_reason_ = msg;
      batch_left_ = 0;
      return;
    }
    ++auto_chain_;
    auto_recast_at_ = GetTickCount() + kAutoReuseDelayMs;
    return;
  }

  if (!auto_recast_) {
    batch_left_ = 0;
    return;
  }
  if (skill_id_ <= 0) {
    auto_stop_reason_ = "Compétence inconnue : relance impossible.";
    batch_left_ = 0;
    return;
  }
  ++auto_chain_;
  auto_recast_at_ = GetTickCount() + kAutoRecastDelayMs;
}

// Ré-arme une relance dont le LANCEMENT n'a rien donné : la compétence (ou l'usage de
// l'objet) est partie et aucune liste n'est revenue.
//
// À ne pas confondre avec ScheduleAutoRecast, qui traite le RÉSULTAT d'une
// fabrication. Ici il n'y a jamais eu de fabrication : c'est l'ouverture de liste qui
// a échoué.
//
// 🔴 Le chemin par OBJET a une règle en plus, et elle n'est pas négociable : on ne
// réessaie QUE si le stock n'a pas bougé. Un Mini Furnace est consommé par
// `pc_useitem` AVANT l'exécution du script ; si l'exemplaire est bel et bien parti et
// que la liste ne revient pas, réessayer brûlerait le sac exemplaire par exemplaire
// pour un défaut qu'on ne comprend pas. Stock intact = l'usage a été refusé, rien
// n'est perdu, on peut reprendre.
void MakeItemWindow::RetryRelaunch() {
  const int owned_now = source_item_id_ ? OwnedCount(source_item_id_) : 0;
  relaunch_sent_at_ = 0;
  if (!imgui_enabled_ || !ui_open_) return;
  if (pending_ != Pending::kNone) return;  // une action est déjà posée

  if (from_item_) {
    if (!auto_reuse_item_ || source_item_id_ == 0) return;
    if (reuse_owned_before_ >= 0 && owned_now < reuse_owned_before_) {
      char msg[192];
      std::snprintf(msg, sizeof(msg),
                    i18n::Tr("%s consommé mais aucune liste n'est revenue : relance "
                    "arrêtée (le stock ne sera pas entamé davantage)."),
                    source_item_name_);
      auto_stop_reason_ = msg;
      batch_left_ = 0;
      return;
    }
  } else if (!auto_recast_) {
    return;
  }

  // Borné, et pas par `auto_chain_` : celui-ci compte les tours RÉUSSIS. Une
  // collision avec un délai de lancement se résorbe en un ou deux essais ; au-delà,
  // insister masquerait le vrai motif d'arrêt.
  if (++relaunch_retries_ > kMaxRelaunchRetries) {
    auto_stop_reason_ =
        "La liste ne revient pas (délai de lancement) : relance arrêtée.";
    Log(auto_stop_reason_, kColWarn);
    batch_left_ = 0;
    return;
  }
  // 🔴 `.clear()`, JAMAIS `= nullptr`. Ici `auto_stop_reason_` est une `std::string`
  // (alors que son homonyme du plugin de refine est un `const char*`, où `= nullptr`
  // est correct). Assigner un pointeur nul à une std::string appelle
  // `operator=(char const*)` avec 0 : comportement indéfini, et en pratique un crash
  // net sur `mov cl, byte ptr [eax]` avec eax = 0.
  // ⏱ Constaté en jeu, débogueur attaché — c'était exactement ça.
  auto_stop_reason_.clear();
  auto_recast_at_ =
      GetTickCount() + (from_item_ ? kAutoReuseDelayMs : kAutoRecastDelayMs);
  char line[160];
  std::snprintf(line, sizeof(line),
                i18n::Tr("Relance sans réponse : nouvel essai (%d/%d)."),
                relaunch_retries_, kMaxRelaunchRetries);
  Log(line, kColWarn);
}

void MakeItemWindow::FlushPending() {
  // ── FILET : toute fenêtre de liste native encore vivante, on la REND ──────
  //
  // ⚠ Ce n'est pas le mécanisme principal. Depuis que les trois listes passent par
  // `RegisterReplaceOpcode`, le handler natif ne tourne plus et la fenêtre ne NAÎT
  // plus : dans le cas normal, ce bloc ne trouve rien.
  //
  // Il reste pour le seul cas qui échappe au remplacement : l'interrupteur
  // « Interface moderne » basculé pendant qu'une fenêtre native était déjà ouverte
  // (le paquet, lui, est passé quand le plugin était coupé).
  //
  // ⚠ ICI et pas à la création : l'appelant natif continue de se servir du pointeur
  // que MakeWindow vient de lui rendre (OnMsg 0x4B pour purger, puis un 0x1F par
  // entrée, puis 0x22) — y toucher serait un use-after-free. Ici, le paquet est
  // entièrement traité depuis longtemps.
  //
  // 🔴 ANNULER, et non DÉTRUIRE. Première rédaction : `UIWindowMgr::Close`, qui
  // n'envoie RIEN — donc le `menuskill` restait armé côté serveur. Et pour la Mini
  // Furnace il vaut `-1`, or `clif_skill_produce_mix_list` commence par
  // `if (menuskill_id == skill_id) return;` : la compétence ne renvoyait plus jamais
  // rien, définitivement. ⏱ Constaté en jeu (natif -> moderne, fenêtre ouverte).
  //
  // Le bogue était une COURSE que je perdais toujours : le pilotage de l'Annuler
  // était armé depuis `OnTick` (limité à 100 ms) et la destruction exécutée ici, à
  // CHAQUE frame. La fenêtre mourait donc avant que le tick ne l'ait vue vivante.
  // D'où le remède : plus de drapeau, plus de tick, plus de course — on annule
  // directement, ici, et l'Annuler ferme la fenêtre de toute façon.
  //
  // Toute fenêtre de liste native vivante alors que nous sommes actifs est
  // forcément une session que nous n'avons PAS ouverte : depuis le remplacement
  // d'opcode, aucune ne peut plus naître sous notre garde. Son Annuler est donc
  // toujours le bon geste — il désarme, il ferme, et pour la 80 il RE-CRÉDITE les
  // matériaux déjà posés (`Inventory_AddOrStackItem`), ce qu'aucune fermeture ne
  // fait. La 80 d'abord, pour cette raison.
  if (imgui_enabled_) {
    CancelNativeIfClass(kWinMakeProcess, kVTableMakeProcess);
    CancelNativeIfClass(kWinMakeTarget, kVTableMakeTarget);
    CancelNativeIfClass(kWinMakingArrow, kVTableMakingArrow);
  }

  // La position n'est écrite qu'à la FERMETURE, pas à chaque frame de glissement :
  // MoonlightUi possède le fichier de réglages et la table kMakeItemSettings y
  // range déjà makeitem_pos_x/y — on ne fait que demander l'écriture. (Ce flush
  // était le bug de la première rédaction : `pos_dirty_` était posé et jamais
  // consommé, donc la position n'était jamais persistée.)
  if (!ui_open_ && pos_dirty_) {
    pos_dirty_ = false;
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }

  // (L'ouverture de description différée qui vivait ici a été GÉNÉRALISÉE :
  //  c'est désormais itemcell::FlushDeferredDesc, appelé par
  //  Bourgeon::OnProcessInput pour les huit viewers — la course de focus de
  //  l'appui long, diagnostiquée ici, est expliquée dans item_cell.h.)

  const Pending todo = pending_;
  if (todo == Pending::kNone) return;
  pending_ = Pending::kNone;

  switch (todo) {
    case Pending::kConfirm: {
      SendConfirm(pending_id_);
      // La liste est CONSOMMÉE à l'ENVOI, pas en constatant après coup la
      // disparition de la fenêtre native : le serveur efface son `menuskill` à
      // cet instant précis, et l'observation indirecte rate sa cible dès qu'une
      // nouvelle liste recrée la fenêtre avant le tick suivant — la liste
      // fantôme laissait alors envoyer dans le vide indéfiniment.
      //
      // Mais « consommée » veut dire « on n'a plus le droit d'envoyer », PAS « il
      // n'y a plus rien à montrer ». On désarme donc, sans vider : la table reste
      // à l'écran, grisée, le temps de l'aller-retour. C'est ce qui supprime le
      // clignotement (table qui disparaît, fenêtre qui se rétracte, tout qui
      // revient 500 ms plus tard) et laisse le temps de lire le résultat.
      list_armed_ = false;
      empty_list_ = false;
      // (Il n'y a plus de cas « on passe la main au natif » : la fenêtre 80 n'est
      // jamais ouverte, donc notre fenêtre ne se retire jamais — elle reste le
      // seul endroit où l'action se joue, du choix du produit au résultat.)
      //
      // ── Les emplacements de forge SURVIVENT à l'envoi ────────────────────
      //
      // Première rédaction : on les vidait, au motif qu'ils décrivent UNE
      // fabrication et que les reconduire dépenserait un Star Crumb de plus par
      // tour « sans que le joueur l'ait demandé ». Retourné à la demande de
      // l'utilisateur, et c'est le bon arbitrage : forger une série d'armes
      // toutes identiques est justement le cas d'usage, et refaire les trois
      // menus déroulants à chaque tour est le vrai contresens. Le joueur qui
      // pose un Star Crumb pour une série SAIT qu'il en consomme un par arme —
      // la ligne de malus au-dessus des emplacements le lui dit à chaque frame.
      //
      // Ce qui les vide toujours : changer de produit, fermer la fenêtre, et la
      // perte de stock (contrôlée à l'arrivée de chaque liste, pour que la
      // chance affichée ne promette pas un malus qui ne s'appliquera pas).
      break;
    }
    case Pending::kCancel:
      SendCancel();
      // Le serveur vient de désarmer : le drapeau doit le dire, sinon une seconde
      // fermeture (ou une bascule d'interrupteur) renverrait une annulation pour
      // personne.
      list_armed_ = false;
      entries_.clear();
      ui_open_ = false;
      break;
    case Pending::kRecast:
      // Posé AVANT l'envoi : la liste qui suivra est la nôtre, et c'est ce qui
      // permet au compteur de chaîne de repartir de zéro quand le joueur, lui,
      // rouvre une liste à la main.
      auto_ours_ = true;
      SendRecast();
      // Relance EN VOL : si aucune liste ne revient, le chien de garde d'OnTick
      // réessaiera au lieu de laisser la chaîne mourir en silence.
      relaunch_sent_at_ = GetTickCount();
      if (relaunch_sent_at_ == 0) relaunch_sent_at_ = 1;
      break;
    case Pending::kReuseItem:
      auto_ours_ = true;
      // Stock relevé AVANT l'usage : c'est lui qui dira au chien de garde si
      // l'exemplaire est parti (on s'arrête) ou si l'usage a été refusé (on peut
      // réessayer sans rien perdre).
      reuse_owned_before_ =
          source_item_id_ ? OwnedCount(source_item_id_) : -1;
      SendReuseItem();  // remet auto_ours_ à faux s'il n'a rien pu envoyer
      if (auto_ours_) {  // quelque chose est bien parti
        relaunch_sent_at_ = GetTickCount();
        if (relaunch_sent_at_ == 0) relaunch_sent_at_ = 1;
      }
      break;
    default:
      break;
  }
}

// ── Actions ──────────────────────────────────────────────────────────────────

void MakeItemWindow::RequestMake() {
  // POINT D'ENTRÉE UNIQUE du bouton, du double-clic et d'Entrée. Chacun portait
  // sa copie des conditions dans le plugin de refine, et seul le bouton avait la
  // garde anti-rafale — d'où une touche maintenue qui expédiait deux demandes.
  // `list_armed_` et non `entries_.empty()` : la table reste AFFICHÉE après un
  // envoi (pour ne pas clignoter), mais le serveur, lui, a déjà oublié la liste.
  // C'est le drapeau qui porte cette règle, plus le contenu de la table.
  if (!ui_open_ || !list_armed_ || entries_.empty()) return;
  if (awaiting_result_) return;
  if (sel_id_ < 0 || !sel_visible_) return;
  const unsigned now = GetTickCount();
  if (sent_at_ && now - sent_at_ < kMinSendIntervalMs) return;

  const Entry* chosen = nullptr;
  for (const Entry& e : entries_)
    if (static_cast<int>(e.id) == sel_id_) { chosen = &e; break; }
  if (!chosen) return;

  // Départ d'une série : on ne fixe le reste-à-faire QUE si aucune n'est en
  // cours, sinon chaque tour le remettrait à la cible et la série ne finirait
  // jamais.
  if (batch_left_ == 0) batch_left_ = batch_target_;

  sent_at_      = now;
  last_sent_id_ = chosen->id;
  // Relevé AVANT l'envoi : c'est la référence qui permettra de constater la
  // création quand le serveur ne dit rien (cf. la sonde d'inventaire d'OnTick).
  sent_owned_   = OwnedCount(chosen->id);
  // ── Et le stock d'un MATÉRIAU, qui est le signal de l'ÉCHEC ────────────────
  // Le stock du produit ne monte qu'en cas de succès ; les matériaux, eux, sont
  // consommés dans les DEUX cas. Leur baisse prouve donc que le serveur a traité
  // la demande, et le produit inchangé dit alors que le tirage a raté — sans
  // attendre le délai de garde, et sans qu'aucun paquet ne soit nécessaire.
  //
  // On prend le premier matériau réellement CONSOMMÉ : un guide (quantité 0) ne
  // bouge jamais, et une ligne de rendement (flèches) monterait au lieu de
  // baisser. Sans matériau utilisable, la sonde reste inactive et on retombe sur
  // le délai de garde — dégradation silencieuse mais correcte.
  sent_mat_id_    = 0;
  sent_mat_owned_ = -1;
  for (int i = 0; i < chosen->mat_count; ++i) {
    const Material& m = chosen->mats[i];
    if (m.id == 0 || m.qty <= 0 || m.not_consumed || m.is_output) continue;
    sent_mat_id_    = m.id;
    sent_mat_owned_ = OwnedCount(m.id);
    break;
  }
  std::snprintf(last_sent_name_, sizeof(last_sent_name_), "%s", chosen->name);
  // On n'attend une réponse serveur que si on a réellement ENVOYÉ. Quand la main
  // passe à la fenêtre native 80, c'est elle qui enverra — parfois bien plus tard,
  // parfois jamais (le joueur peut annuler).
  awaiting_result_ =
      // Depuis qu'on n'ouvre plus la 80, TOUT envoi part de nous : on attend donc
      // toujours un résultat. C'est ce qui rend la relance automatique et la
      // série disponibles pour la FORGE, ce qui n'était pas le cas tant que la
      // fenêtre native emportait l'interaction.
      true;
  pending_id_ = chosen->id;
  pending_    = Pending::kConfirm;
}

void MakeItemWindow::CloseAndCancel() {
  // Fermer, c'est ARRÊTER. Une relance déjà programmée qui survivrait à la
  // fermeture partirait après que le joueur a dit stop — et, du côté objet,
  // consommerait un exemplaire pour rouvrir une fenêtre qu'il vient de fermer.
  auto_recast_at_ = 0;
  auto_ours_      = false;
  batch_left_     = 0;
  batch_fire_     = false;
  // Plus de relance en vol : fermer, c'est arrêter — y compris le chien de garde.
  relaunch_sent_at_   = 0;
  relaunch_retries_   = 0;
  reuse_owned_before_ = -1;
  if (pending_ == Pending::kRecast || pending_ == Pending::kReuseItem)
    pending_ = Pending::kNone;

  // 🔴 ON ENVOIE TOUJOURS L'ANNULATION, dès qu'une fenêtre était ouverte.
  //
  // Version précédente : seulement si `list_armed_` (ou une native vivante). C'était
  // « propre » — ne pas émettre un paquet inutile — mais ça pariait sur l'exactitude
  // d'un drapeau, et l'asymétrie des deux erreurs est écrasante :
  //   • une annulation EN TROP est inoffensive : `clif_parse_ProduceMix` teste
  //     `switch (sd->menuskill_id)` et sort par son `default:` sans rien effacer dès
  //     que rien de compatible n'est armé ;
  //   • une annulation MANQUANTE laisse le `menuskill` armé, et là tout se bloque :
  //     le refine refuse de se lancer, ET la fabrication suivante bute sur le
  //     « Avoid resending the menu » (`if (menuskill_id == skill_id) return;`) qui
  //     n'envoie RIEN. Plus aucune liste, jamais, jusqu'à un changement de carte.
  // ⏱ Constaté en jeu : liste ouverte vide, fermée, puis plus rien ne repartait.
  //
  // Autrement dit on ne cherche plus à savoir si le serveur a quelque chose d'armé —
  // question à laquelle nos drapeaux répondaient parfois faux. On le lui dit, il en
  // fait ce qu'il veut.
  if (ui_open_ || list_armed_) {
    pending_ = Pending::kCancel;
  } else {
    ui_open_ = false;
    entries_.clear();
  }
}

// ── Journal ──────────────────────────────────────────────────────────────────

void MakeItemWindow::Log(const std::string& text, uint32_t color) {
  LogLine line;
  if (log_time_) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[16];
    std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", st.wHour, st.wMinute,
                  st.wSecond);
    line.text = stamp;
  }
  // L'horodatage est calculé à l'INSERTION et toujours stocké ; ne le composer
  // qu'à la demande priverait de leur heure toutes les lignes déjà écrites au
  // moment où on active le réglage.
  line.text += text;
  line.color = color;
  history_.push_back(std::move(line));
  if (history_.size() > 200) history_.erase(history_.begin());
}

void MakeItemWindow::LogResult(int result, uint32_t nameid) {
  const bool success = (result % 2) == 0;  // cf. le § du parseur : 6 compris
  // Libellé EXACT du client, jamais une paraphrase. Le natif compose la même
  // chaîne… puis la JETTE sans l'afficher (§3.7) — c'est précisément ce trou
  // qu'on bouche ici.
  const char* utf8 = msgstr::Utf8(success ? kMsgMakeSuccess : kMsgMakeFail);
  const char* tmpl = utf8[0] ? utf8 : nullptr;

  // Le nom : l'id du paquet d'abord, le nom mémorisé à l'envoi en repli.
  char item_name[128];
  auto* ui = Bourgeon::Instance().moonlight_ui();
  const char* n = ui ? ui->ItemName(nameid) : nullptr;
  if (n && *n)
    std::snprintf(item_name, sizeof(item_name), "%s", n);
  else if (nameid == last_sent_id_ && last_sent_name_[0])
    std::snprintf(item_name, sizeof(item_name), "%s", last_sent_name_);
  else
    std::snprintf(item_name, sizeof(item_name), "#%u", nameid);

  // ⚠ On ne passe JAMAIS `tmpl` comme format à snprintf : c'est une chaîne de
  // données (msgstringtable.csv, modifiable par le serveur), pas un littéral. Un
  // second `%s` — ou un `%n` — y lirait la pile. On substitue donc la PREMIÈRE
  // occurrence de « %s » à la main, et on se rabat sur un libellé maison si le
  // gabarit n'en contient pas.
  char line[256];
  const char* hole = tmpl ? std::strstr(tmpl, "%s") : nullptr;
  if (hole) {
    const int head_len = static_cast<int>(hole - tmpl);
    std::snprintf(line, sizeof(line), "%.*s%s%s", head_len, tmpl, item_name,
                  hole + 2);
  } else if (tmpl) {
    std::snprintf(line, sizeof(line), "%s (%s)", tmpl, item_name);
  } else {
    std::snprintf(line, sizeof(line), "%s : %s", success ? i18n::Tr("Succès") : i18n::Tr("Échec"),
                  item_name);
  }
  last_result_       = line;
  last_result_color_ = success ? kColOk : kColBad;
  Log(line, success ? kColOk : kColBad);
}

// ── Rendu ────────────────────────────────────────────────────────────────────

const char* MakeItemWindow::ProtoTitle() const {
  // La compétence n'est PAS dans le paquet (un même 0x01AD sert quatre métiers).
  // À défaut de l'avoir captée, on titre par ce que le paquet dit vraiment —
  // jamais par une devinette sur le contenu de la liste.
  if (proto_ == Proto::kProduce) {
    const char* label = msgstr::Utf8(kMsgMakeList);  // « Manufacturing List »
    if (label[0]) return label;
    return "Fabrication";
  }
  return "Fabrication";
}

void MakeItemWindow::OnRenderUI() {
  if (!imgui_enabled_ || !ui_open_) return;

  char title[128];
  std::snprintf(title, sizeof(title), "%s###makeitem", ProtoTitle());

  if (pos_x_ != INT_MIN && pos_y_ != INT_MIN) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(pos_x_),
                                   static_cast<float>(pos_y_)),
                            ImGuiCond_FirstUseEver);
  }
  // ⚠ `SetNextWindowSize(…, FirstUseEver)` ne suffisait PAS : il ne s'applique
  // qu'à la toute première ouverture, après quoi ImGui garde la taille mémorisée.
  // Combiné à NoResize, la fenêtre restait figée à sa hauteur d'origine — d'où le
  // grand vide sous les boutons dès que la liste raccourcissait.
  //
  // `AlwaysAutoResize` recalcule la hauteur À CHAQUE frame sur le contenu réel, et
  // les contraintes épinglent la largeur : on obtient une fenêtre qui colle
  // toujours à ce qu'elle affiche, et qui n'a plus besoin de poignée.
  ImGui::SetNextWindowSizeConstraints(ImVec2(kWindowW, 0.0f),
                                      ImVec2(kWindowW, FLT_MAX));

  // Bullet de la barre de titre : raccourci vers la section « Fabrication » du
  // panneau Moonlight, comme le refine, l'inventaire, le cart, le storage et la
  // banque. Les réglages qui pilotent CETTE fenêtre doivent être atteignables
  // DEPUIS elle — en particulier la relance automatique, qu'on veut couper ou
  // ajuster au moment précis où elle tourne.
  ro::SetNextWindowTitleBullet(i18n::Tr("Options de fabrication"));
  bool open = true;
  hover_valid_ = false;  // relevé pendant le rendu, consommé juste après
  const bool begun =
      // NoCollapse : pas de bouton « réduire » sur une fenêtre de fabrication.
      // Elle vit le temps d'une session de craft et porte un état qui expire —
      // liste armée côté serveur, série en cours, matériaux choisis. La replier
      // en barre de titre reviendrait à cacher une action en cours sans
      // l'interrompre. Même choix que la fenêtre de refine.
      // (Le drapeau retire aussi l'art sys_mini : cf. `show_mini` dans ro_imgui.)
      ro::BeginRoWindow(title, &open,
                        ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoCollapse);
  // ⚠ À lire JUSTE APRÈS BeginRoWindow et hors du `if (begun)` : le drapeau est
  // posé par Begin lui-même et vaut aussi pour une fenêtre repliée.
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceMakeItem);
  if (begun) {
    const ImVec2 p = ImGui::GetWindowPos();
    const int nx = static_cast<int>(p.x), ny = static_cast<int>(p.y);
    if (nx != pos_x_ || ny != pos_y_) { pos_x_ = nx; pos_y_ = ny; pos_dirty_ = true; }

    // Liste désarmée : elle reste affichée mais GRISÉE, le temps de l'aller-retour
    // serveur. Ce n'est pas un ornement — c'est ce qui distingue « le contenu que
    // tu regardes est encore actionnable » de « il est là pour référence ». Sans
    // le signal visuel, garder la table à l'écran laisserait croire qu'un
    // deuxième envoi va partir.
    const bool stale = !list_armed_ && !entries_.empty();
    if (stale) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
    DrawList();
    DrawRecipe();
    DrawForgeSlots();
    if (stale) ImGui::PopStyleVar();
    DrawFooter();
    DrawHistory();
  }
  ro::EndRoWindow();

  // Hors de toute fenêtre ImGui : c'est la condition d'emploi de DrawTooltip.
  if (hover_valid_ && desc_tooltip_)
    itemcell::DrawTooltip(hover_id_, nullptr, 0, nullptr, 0, 0, hover_name_);

  if (!open) CloseAndCancel();
}

void MakeItemWindow::DrawList() {
  if (empty_list_ && entries_.empty()) {
    // Le natif n'a qu'un message, recyclé et hors sujet (« You can't create
    // items yet. »), et seulement sur 0x018D — sur 0x01AD/0x025A il ouvre une
    // fenêtre vide sans un mot. On énumère les vraies causes (§7.2).
    const char* warn = msgstr::Utf8(kMsgCantMakeItem);
    if (warn[0]) {
      ImGui::PushStyleColor(ImGuiCol_Text, kColWarn);
      TextWrapped(warn);
      ImGui::PopStyleColor();
    }
    Spacing();
    TextWrapped(i18n::Tr("Le serveur n'a proposé aucun produit. Causes possibles :"));
    BulletWrapped(i18n::Tr("il te manque un matériau (ou la quantité) ;"));
    BulletWrapped(i18n::Tr("ton inventaire est plein — le serveur refuse ce qu'il ne peut pas te remettre ;"));
    BulletWrapped(i18n::Tr("le niveau de compétence requis n'est pas atteint."));
    Spacing();

    // ── Ce qu'on faisait juste avant, et ce qui manque ────────────────────────
    // Le serveur ne dit JAMAIS pourquoi il n'a rien proposé — il se contente de
    // ne rien envoyer. Mais la liste précédente, elle, nous est connue : c'est
    // exactement l'ensemble des produits que le joueur pouvait faire il y a un
    // instant, donc le bon ensemble à examiner. Avec les recettes complètes
    // (fichier régénéré), on peut nommer ce qui a manqué.
    //
    // ⚠ Ce n'est PAS l'atlas complet des formules : le getteur natif est une
    // lookup par clé (`MetalProcessRecipe_GetLines(id)`), la table n'est pas
    // énumérable, et rien côté client ne dit quels produits appartiennent à cette
    // compétence. Énumérer tout demanderait un index dans le fichier.
    if (!stale_entries_.empty()) {
      SeparatorText(i18n::Tr("Ce que tu pouvais faire juste avant"));
      const MoonlightUi* ui = Bourgeon::Instance().moonlight_ui();
      int shown = 0;
      for (const Entry& e : stale_entries_) {
        if (shown >= kMaxStaleShown) break;
        if (!e.mats_resolved) continue;

        // On ne cite QUE les lignes réellement bloquantes : un matériau qu'on a
        // en quantité n'apprend rien, et la liste doit rester lisible.
        char missing[256] = {0};
        int  written = 0;
        for (int i = 0; i < e.mat_count; ++i) {
          const Material& m = e.mats[i];
          if (m.is_output || !m.id) continue;
          const int have = OwnedCount(m.id);
          const int need = m.not_consumed ? 1 : m.qty;
          if (have >= need) continue;
          const char* db_name = ui ? ui->ItemName(m.id) : nullptr;
          written += std::snprintf(
              missing + written, sizeof(missing) - written, "%s%d %s",
              written ? ", " : "", need - have,
              (db_name && *db_name) ? db_name : m.name);
          if (written >= static_cast<int>(sizeof(missing)) - 1) break;
        }
        ++shown;
        ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
        if (missing[0])
          Text(i18n::Tr("%s : il manque %s"), e.name, missing);
        else
          // Rien ne manque et le serveur refuse quand même : c'est une des deux
          // AUTRES causes (sac plein, niveau de compétence). Le dire évite de
          // chercher un matériau qui est là.
          Text(i18n::Tr("%s : matériaux au complet — sac plein ou niveau insuffisant"),
               e.name);
        ImGui::PopStyleColor();
      }
      if (static_cast<int>(stale_entries_.size()) > shown) {
        ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
        Text("(+%d autres)", static_cast<int>(stale_entries_.size()) - shown);
        ImGui::PopStyleColor();
      }
      Spacing();
    }
    return;
  }

  // ── Après une fabrication : plus de liste, et il ne faut RIEN dessiner ──────
  // Le serveur a fait son `clif_menuskill_clear`, il n'y a plus rien à choisir.
  // Laisser le filtre et un tableau aux en-têtes vides donnait « une liste vide
  // inutile » — trois widgets qui promettent une action impossible. La différence
  // avec le cas ci-dessus est réelle : `empty_list_` veut dire « le SERVEUR n'a
  // rien proposé » (il faut l'expliquer), tandis qu'ici la liste a été consommée
  // par notre propre envoi (il n'y a rien à dire, seulement le résultat).
  if (entries_.empty()) return;

  if (show_filter_) {
    PushItemWidth(-1.0f);
    // 🔴 PAS de `SetKeyboardFocusHere()` ici, et ce n'est pas un oubli.
    //
    // Cette fenêtre ne s'ouvre pas sur un geste d'interface : elle s'ouvre parce
    // que le joueur a lancé une compétence ou consommé un objet, EN JEU, personnage
    // debout au milieu de la carte. Lui prendre le clavier à cet instant, c'est
    // faire taper ses touches de déplacement dans un champ de recherche et rendre
    // Entrée inopérante sur la fabrication.
    //
    // Et le défaut se répétait à CHAQUE liste reçue, donc à chaque tour d'une
    // chaîne de relance : le clavier était confisqué en boucle, sans que rien ne
    // l'explique. Le focus automatique sur une recherche ne se justifie que
    // lorsque l'utilisateur a demandé la recherche — jamais quand la fenêtre
    // s'ouvre toute seule.
    ImGui::InputTextWithHint("##makefilter", "Filtrer…", filter_, sizeof(filter_));
    PopItemWidth();
  }

  // Le tri vit dans l'EN-TÊTE de la table (Sortable + Tristate) : les colonnes
  // triables se voient en permanence, le 2e clic inverse le sens et le 3e retire
  // le tri — l'ordre redevient alors celui du paquet, sans qu'une entrée « ordre
  // d'origine » ait besoin d'exister quelque part.
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
      ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
      ImGuiTableFlags_SortTristate | ImGuiTableFlags_SizingStretchProp;

  // Hauteur ADAPTATIVE. Une hauteur fixe donnait une fenêtre de la même taille
  // pour trois potions que pour cinquante — et la native ne fait que 280×150.
  // On dimensionne donc au contenu, borné : au moins 3 lignes (sinon le tableau
  // se réduit à son en-tête et paraît cassé), au plus 12 (au-delà, on défile).
  // Le filtre n'entre pas dans le calcul : la hauteur sauterait à chaque frappe.
  // La ligne fait la hauteur de l'ICÔNE (c'est elle et le Selectable qui la
  // donnent), plus la marge de cellule des deux côtés. L'estimer à partir de la
  // hauteur du texte donnait un tableau trop court de plusieurs pixels par ligne,
  // donc une barre de défilement qui apparaissait sans raison.
  const float row_h    = kIconSize + ImGui::GetStyle().CellPadding.y * 2.0f;
  const float header_h = ImGui::GetFrameHeight();
  const int   rows     = static_cast<int>(entries_.size());
  const int   shown    = (rows < 3) ? 3 : ((rows > 12) ? 12 : rows);
  const float list_h   = header_h + shown * row_h;
  sel_visible_ = false;

  // « Faisable » est MASQUÉE sur une liste de flèches : elle y répéterait mot pour
  // mot « Possédé ». Une fabrication consomme exactement un matériau
  // (`pc_delitem(sd, j, 1, ...)`), donc les deux colonnes portent le même nombre —
  // deux fois la même donnée, une colonne de moins pour le nom.
  const bool show_craftable = show_owned_ && !arrow_list_;
  const int  col_count = 2 + (show_owned_ ? 1 : 0) + (show_craftable ? 1 : 0);

  if (ImGui::BeginTable("##makelist", col_count, kFlags,
                        ImVec2(0.0f, list_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    // La colonne d'icône PORTE le tri par id : l'id lui-même n'a pas sa place à
    // l'écran (l'aperçu au survol et la description le donnent déjà) et une
    // colonne de plus rognait le nom, qui est la seule chose que le joueur lit.
    // Trier par id reste utile — c'est l'ordre « par famille d'objet ».
    ImGui::TableSetupColumn("##icone", ImGuiTableColumnFlags_WidthFixed, 28.0f);
    ImGui::TableSetupColumn("Produit", ImGuiTableColumnFlags_DefaultSort);
    if (show_owned_)
      ImGui::TableSetupColumn(i18n::Tr("Possédé"), ImGuiTableColumnFlags_WidthFixed, 58.0f);
    if (show_craftable)
      ImGui::TableSetupColumn("Faisable", ImGuiTableColumnFlags_WidthFixed, 60.0f);
    ImGui::TableHeadersRow();

    // Vue triée : on ne touche JAMAIS à `entries_`, qui reste dans l'ordre du
    // paquet (c'est lui que le 3e clic doit pouvoir rendre).
    std::vector<const Entry*> view;
    view.reserve(entries_.size());
    for (const Entry& e : entries_) {
      if (filter_[0]) {
        char hay[160];
        std::snprintf(hay, sizeof(hay), "%s %u", e.name, e.id);
        std::string h(hay), n(filter_);
        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        if (h.find(n) == std::string::npos) continue;
      }
      view.push_back(&e);
    }

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
      if (specs->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
        std::sort(view.begin(), view.end(),
                  [&](const Entry* a, const Entry* b) {
                    int cmp = 0;
                    switch (s.ColumnIndex) {
                      case 0: cmp = (a->id < b->id) ? -1 : (a->id > b->id); break;
                      case 1: cmp = _stricmp(a->name, b->name); break;
                      case 2: cmp = (a->owned < b->owned) ? -1 : (a->owned > b->owned); break;
                      case 3: cmp = (a->craftable < b->craftable) ? -1
                                                                 : (a->craftable > b->craftable); break;
                      default: break;
                    }
                    // ⚠ std::sort n'est PAS stable : sans ce départage par id,
                    // deux produits homonymes (les quatre « Elemental Converter »)
                    // échangeraient leur place d'une frame à l'autre et la ligne
                    // sauterait sous le curseur.
                    if (cmp == 0) cmp = (a->id < b->id) ? -1 : (a->id > b->id);
                    return s.SortDirection == ImGuiSortDirection_Ascending ? cmp < 0
                                                                          : cmp > 0;
                  });
      }
    }

    int row_index = 0;
    for (const Entry* e : view) {
      // ⚠ Pas de hauteur imposée ici : ce sont l'icône et le Selectable qui la
      // donnent, et ils doivent faire EXACTEMENT la même. Forcer la ligne à 26 px
      // en laissant le Selectable à sa hauteur par défaut (une ligne de texte,
      // ~17) donnait une bande de sélection plus courte que l'icône — le « fond
      // bleu pas raccord ». Même composition que la table du refine.
      ImGui::TableNextRow();
      // ⚠ L'ID vient de l'INDEX de ligne, pas de l'id d'objet. Le dédoublonnage
      // ci-dessus rend les ids uniques, mais s'appuyer dessus pour l'identité
      // ImGui, c'est faire dépendre le rendu d'une invariante du serveur : le
      // jour où deux lignes repassent avec le même id, ImGui hurle
      // « conflicting ID » et les deux lignes réagissent au même clic. L'index
      // est unique par construction.
      ImGui::PushID(row_index++);

      ImGui::TableSetColumnIndex(0);
      // ⚠ ro::IconTex (= ui/game_texture.h GameTexture) expose `tex`, `w`, `h` —
      // pas d'UV : la texture EST l'icône, on la dessine telle quelle.
      const ro::IconTex icon = ro::ItemIcon(e->id, 1);
      if (icon.tex)
        ImGui::Image(TexId(icon.tex), ImVec2(kIconSize, kIconSize));
      else
        ImGui::Dummy(ImVec2(kIconSize, kIconSize));  // garde la hauteur de ligne

      ImGui::TableSetColumnIndex(1);
      // Le Selectable est posé dans la colonne du NOM avec SpanAllColumns :
      // ImGui bascule alors son fond dans le canal d'arrière-plan de la table, ce
      // qui fait passer le surlignage DERRIÈRE l'icône (colonne 0, soumise avant)
      // au lieu de la couvrir.
      // ⚠ La HAUTEUR explicite `ImVec2(0, kIconSize)` n'est pas cosmétique : sans
      // elle le Selectable prend la hauteur d'une ligne de texte (~17 px) et sa
      // bande de sélection est plus courte que l'icône de 24 — décalage visible
      // dès qu'une ligne est surlignée.
      const bool selected = (static_cast<int>(e->id) == sel_id_);
      if (ImGui::Selectable(e->name, selected,
                            ImGuiSelectableFlags_SpanAllColumns |
                                ImGuiSelectableFlags_AllowDoubleClick,
                            ImVec2(0.0f, kIconSize))) {
        sel_id_ = static_cast<int>(e->id);
        // 🔴 `sel_visible_` DOIT être relevé ici, pas seulement plus bas via
        // `selected`. `selected` a été calculé AVANT le clic, à partir de l'ANCIEN
        // `sel_id_` : sur un double-clic qui change de ligne, il vaut encore faux,
        // et `RequestMake` — qui exige une sélection visible — sortait sans rien
        // faire. Le double-clic ne lançait donc la fabrication que sur une ligne
        // DÉJÀ sélectionnée. Même correctif que la table du refine.
        sel_visible_ = true;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) RequestMake();
      }
      // Clic gauche = sélectionner, clic DROIT = consulter : c'est le geste
      // partout ailleurs dans le client, et l'ouvrir à gauche volerait la
      // sélection (feedback_right_click_opens_description).
      // ⚠ Aucun appel de focus ici : la remontée du panneau de description est
      // réclamée par ItemDescWindow lui-même (RaiseItemWindow, posée depuis le
      // hook OnMsg 0x18 que TOUTES les ouvertures traversent). Un second appel
      // ferait doublon — c'est écrit tel quel dans item_desc_window.h.
      if (IsLastItemRightClicked()) {
        const ImVec2 m = ImGui::GetMousePos();
        itemcell::DeferDescById(e->id, 0, 0, static_cast<int>(m.x),
                                static_cast<int>(m.y));
      }
      if (IsLastItemHovered()) ro::SetHoverCursor(kRoCursorHand);
      if (selected) sel_visible_ = true;
      const bool hovered = IsLastItemHovered();

      if (show_owned_) {
        ImGui::TableSetColumnIndex(2);
        if (e->owned > 0)
          ImGui::Text("%d", e->owned);
        else
          ImGui::TextDisabled("0");
      }
      if (show_craftable) {
        ImGui::TableSetColumnIndex(3);
        // -1 = « on ne sait pas », et ça se dit par un tiret. Afficher « 0 »
        // serait faux : le serveur n'aurait pas listé ce produit s'il n'y avait
        // pas de quoi en faire au moins un.
        if (e->craftable > 0)
          ImGui::Text("%d", e->craftable);
        else
          ImGui::TextDisabled("—");
      }

      ImGui::PopID();

      // ⚠ On MÉMORISE le survol, on ne peint rien ici : `itemcell::DrawTooltip`
      // crée son PROPRE popup et doit sortir hors de toute fenêtre ImGui (cf.
      // features/item_cell.h). Appelé depuis la boucle du tableau, il ne
      // s'affichait tout simplement pas. Le rendu a lieu après EndRoWindow.
      if (hovered && desc_tooltip_) {
        hover_valid_ = true;
        hover_id_    = e->id;
        std::snprintf(hover_name_, sizeof(hover_name_), "%s", e->name);
      }
    }
    ImGui::EndTable();
  }

  // Entrée valide la sélection. L'ACTION garde ses verrous : hors saisie — tant
  // que le champ de filtre a le focus, Entrée lui appartient.
  //
  // RÉPÉTITION ACTIVÉE (le `false` d'origine la coupait), et pavé numérique
  // inclus — c'est le comportement du refine, et il n'y avait aucune raison d'en
  // diverger : maintenir Entrée enchaîne les fabrications.
  //
  // Aucun garde-fou n'est perdu au passage, ils vivent tous dans RequestMake :
  // `list_armed_` (le serveur a-t-il encore une liste ?), `awaiting_result_` (une
  // demande est en vol) et l'intervalle minimal de 300 ms — précisément parce que
  // la répétition clavier est plus rapide qu'un aller-retour serveur. La touche
  // ne fait donc que retenter ; c'est le réarmement de la liste qui cadence.
  if (enter_key_ &&
      (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) &&
      !ImGui::IsAnyItemActive())
    RequestMake();
}

// ── Chance de réussite ───────────────────────────────────────────────────────
//
// 🔴 ERREUR À NE PAS REFAIRE. Une première rédaction annonçait « 50 % à plat » sur une
// liste ouverte par un objet, au motif que `clif_parse_ProduceMix` appelle
// `skill_produce_mix(sd, **0**, …)` : `skill_id = 0` ne matche aucune case du grand
// `switch`, donc `default: make_per = 5000`. C'était FAUX, et l'utilisateur l'a
// démenti avec les descriptions de compétences du client. Trente lignes plus haut :
//
//     if (!skill_id) //A skill can be specified for some override cases.
//         skill_id = skill_produce_db[idx].req_skill;
//
// Le zéro est REMPLACÉ par le `req_skill` de la recette. La branche `BS_*` est donc
// bien atteinte, maîtrises comprises. Vérification croisée qui ne laisse aucun doute,
// le bonus valant `base + niveau * 500` sur une échelle de 10000 :
//     Iron Tempering        4000 + i*500  ->  45 %…65 %   ✔ description client
//     Steel Tempering       3000 + i*500  ->  35 %…55 %   ✔
//     Enchanted Stone Craft 1000 + i*500  ->  15 %…35 %   ✔
//
// La vraie formule (non-équipement, cf. skill.cpp) est donc :
//     make_per = job_level*20 + dex*10 + luk*10 + rnd_value(1,100)*10
//              + (base + pc_checkskill(req_skill) * 500)
//     succès si rnd() % 10000 < make_per        (et Star Crumb : make_per = 100000)
//
// Elle est EXACTEMENT calculable malgré son terme aléatoire : `rnd_value(1,100)` est
// uniforme sur 100 valeurs, donc la probabilité est la moyenne des 100 issues. Mon
// autre affirmation — « le terme aléatoire interdit d'annoncer un chiffre ferme »,
// écrite dans craft_data.h — était donc fausse aussi.
//
// Le test « est-ce un équipement » passe par la table d'armes du YAML (celle des
// chances de refine) : en pré-renewal les recettes `produce` ne rendent que des
// matériaux ou des armes, donc « connu comme arme » vaut « équipement » ici.
//
// ⏱ COUVERTURE ACTUELLE : le traitement des métaux seulement (BS_IRON 94, BS_STEEL 95,
// BS_ENCHANTEDSTONE 96) — c'est la famille testée, et la seule dont la formule tienne
// en trois lignes. Les autres branches du switch serveur (AM_PHARMACY avec son bonus
// d'homoncule, la cuisine avec `cook_mastery`, GC_RESEARCHNEWPOISON, GN_CHANGEMATERIAL,
// la forge d'arme) demandent chacune leur propre transcription. Hors couverture, on se
// TAIT — un nombre faux est pire que pas de nombre, c'est la leçon de cette fonction.
// (Second bloc anonyme, fusionné avec celui du haut de fichier : cette aide n'a rien
// à faire à portée EXTERNE — le namespace principal se referme ligne ~857, bien avant
// ici.)
namespace {

int MetalCraftChancePercent(uint32_t product_id, int skill_id) {
  // Star Crumb : le serveur force `make_per = 100000` dans son `switch (nameid)`,
  // donc réussite certaine quoi que valent stats et niveau.
  if (product_id == kMatStarCrumb) return 100;

  // Base par PRODUIT, exactement comme le `switch (nameid)` du serveur — et les
  // valeurs se recoupent avec les descriptions de compétences du client
  // (45…65 %, 35…55 %, 15…35 % pour les niveaux 1 à 5).
  int base = 1000;                                  // pierres élémentaires
  if (skill_id == kSkillIronTempering)  base = 4000;  // Iron
  else if (skill_id == kSkillSteelTempering) base = 3000;  // Steel

  const int level = rag::LearnedSkillLevel(skill_id);
  // Le déterministe, hors tirage.
  const int fixed = rag::JobLevel() * 20 +
                    rag::StatTotal(rag::kDex) * 10 +
                    rag::StatTotal(rag::kLuk) * 10 +
                    base + level * 500;

  // 🔴 Le terme aléatoire est DANS le seuil : `make_per += rnd_value(1, 100) * 10`,
  // puis `rnd() % 10000 < make_per`. La probabilité n'en est pas indéterminable pour
  // autant — le tirage est uniforme sur 100 valeurs, donc c'est la MOYENNE des 100
  // issues. Cent itérations, résultat exact.
  double sum = 0.0;
  for (int roll = 1; roll <= 100; ++roll) {
    int per = fixed + roll * 10;
    if (per < 1) per = 1;                    // le serveur borne aussi par le bas
    if (per > 10000) per = 10000;
    sum += per / 10000.0;
  }
  // ⚠ `sum` EST déjà le pourcentage, et ce n'est pas un oubli de division : la
  // probabilité vaut (1/100) x somme des 100 probabilités, et le pourcentage vaut
  // 100 x cette probabilité. Les deux facteurs 100 s'annulent. Ajouter un « / 100 »
  // ici — le réflexe naturel en lisant « moyenne » — diviserait le résultat par cent.
  return static_cast<int>(sum + 0.5);
}

// ── Chance de la FORGE D'ARME ────────────────────────────────────────────────
//
// Branche `else // Weapon Forging` de skill_produce_mix, transcrite ligne à ligne.
// Elle n'a presque rien de commun avec celle des métaux, d'où une fonction à part.
//
// 🔴 Le multiplicateur serveur `weapon_produce_rate` décide de tout : il vaut **500**
// sur Moonlight, donc `make_per` est QUINTUPLÉ. L'ignorer donnerait un chiffre cinq
// fois trop bas. Il arrive par le YAML ; s'il manque, on rend -1 et l'appelant se
// TAIT plutôt que d'annoncer un nombre faux.
//
// ⚠ Trois pièges de transcription, tous vérifiés sur le source :
//   • `(4 / wlv) * 1000` est une division ENTIÈRE (lv1 +40 %, lv2 +20 %, lv3 +10 %)
//     et le test est `wlv < 4` : une arme de niveau 4 n'a AUCUN bonus de base ;
//   • la pierre élémentaire compte pour -25 % UNE SEULE FOIS, quel qu'en soit le
//     nombre posé — le serveur garde `ele == 0` en garde et ne consomme que la
//     première ; les Star Crumbs, eux, cumulent (-15 % chacun) ;
//   • le multiplicateur s'applique APRÈS le tirage aléatoire, et le plancher
//     `make_per < 1 -> 1` APRÈS le multiplicateur. Inverser l'ordre change le
//     résultat sur les cas très pénalisés, justement les plus intéressants.
//
// ⏱ NON transcrit : la pénalité de -30 % des classes BABY. Le masque `JOBL_BABY` est
// une notion SERVEUR (`sd->class_`), et l'id de job côté client est un id de SPRITE —
// les confondre donnerait un faux. Un forgeron baby verra donc un chiffre trop
// optimiste ; c'est le seul écart connu, et il est nommé.
int ForgeChancePercent(uint32_t product_id, int skill_id, int star_crumbs,
                       bool element) {
  const int rate = craftdata::WeaponProduceRate();
  const int wlv  = craftdata::WeaponLevel(product_id);
  if (rate < 0 || wlv <= 0 || skill_id <= 0) return -1;

  int fixed = rag::JobLevel() * 20 +
              rag::StatTotal(rag::kDex) * 10 +
              rag::StatTotal(rag::kLuk) * 10;
  if (wlv < 4) fixed += (4 / wlv) * 1000;
  fixed += rag::LearnedSkillLevel(skill_id) * 500;
  fixed += rag::LearnedSkillLevel(kSkillWeaponResearch) * 100;
  if (craftdata::OrideconResearchFix() && wlv >= 3)
    fixed += rag::LearnedSkillLevel(kSkillOrideconResearch) * 100;
  if (element) fixed -= 2500;
  fixed -= star_crumbs * 1500;
  // Une seule enclume compte, la MEILLEURE : la table est rangée dans cet ordre et
  // le serveur enchaîne des `else if`. En porter plusieurs ne sert à rien.
  for (const AnvilBonus& a : kAnvils)
    if (OwnedCount(a.id) > 0) { fixed += a.raw; break; }

  double sum = 0.0;
  for (int roll = 1; roll <= 100; ++roll) {
    int per = fixed + roll * 10;
    per = per * rate / 100;   // APRÈS le tirage, comme le serveur
    if (per < 1) per = 1;     // …et le plancher APRÈS le multiplicateur
    if (per > 10000) per = 10000;
    sum += per / 10000.0;
  }
  return static_cast<int>(sum + 0.5);  // cf. MetalCraftChancePercent : sum EST le %
}

// ── Chance de PHARMACY ───────────────────────────────────────────────────────
//
// Branche `case AM_PHARMACY` de skill_produce_mix. Sa forme diffère des deux autres
// sur un point qui simplifie tout : **il n'y a AUCUN tirage de base**. Le seul hasard
// vient du bonus par produit, et seulement pour onze objets. Un Blue Potion, un
// Anodyne ou un Aloebera ont donc une chance parfaitement DÉTERMINÉE.
//
//   make_per = LearningPotion x 50 + Pharmacy x 300 + job_level x 20
//            + (INT / 2) x 10 + DEX x 10 + LUK x 10
//            [+ bonus/malus du produit, avec son tirage]
//            puis x potion_produce_rate / 100
//
// ⚠ `(INT / 2) * 10` : division ENTIÈRE avant la multiplication. Un INT impair perd
// donc 5 points de make_per — écrire `INT * 5` donnerait un autre résultat.
//
// ⏱ NON transcrit, et nommé dans l'infobulle : le bonus d'HOMONCULE
// (`HVAN_INSTRUCT`, +1 % par niveau, uniquement sur un Vanilmirth actif). Lire les
// compétences de l'homoncule côté client est un chantier à part ; en attendant, un
// alchimiste accompagné verra un chiffre légèrement PESSIMISTE — le bon sens de
// l'erreur. Idem pour la pénalité de 30 % des classes baby (cf. la forge).
int PharmacyChancePercent(uint32_t product_id) {
  const int rate = craftdata::PotionProduceRate();
  if (rate < 0) return -1;

  const int fixed = rag::LearnedSkillLevel(kSkillLearningPotion) * 50 +
                    rag::LearnedSkillLevel(kSkillPharmacy) * 300 +
                    rag::JobLevel() * 20 +
                    (rag::StatTotal(rag::kInt) / 2) * 10 +
                    rag::StatTotal(rag::kDex) * 10 +
                    rag::StatTotal(rag::kLuk) * 10;

  const PotionBonus* bonus = nullptr;
  for (const PotionBonus& b : kPotionBonuses)
    if (b.id == product_id) { bonus = &b; break; }

  // Produit sans bonus : aucun aléa, une seule issue à évaluer.
  const int rolls = bonus ? bonus->roll_max : 1;
  double sum = 0.0;
  for (int roll = 1; roll <= rolls; ++roll) {
    int per = fixed;
    if (bonus) {
      const int drawn = roll * 10;  // (1 + rnd % N) * 10, parcouru exhaustivement
      per += bonus->base + (bonus->subtract ? -drawn : drawn);
    }
    per = per * rate / 100;
    if (per < 1) per = 1;
    if (per > 10000) per = 10000;
    sum += per / 10000.0;
  }
  // Moyenne des issues, exprimée en pourcentage : les deux facteurs se simplifient
  // quand `rolls` vaut 100 (cf. MetalCraftChancePercent), pas dans le cas général —
  // d'où la division explicite ici, où `rolls` peut valoir 1 ou 50.
  return static_cast<int>(sum * 100.0 / rolls + 0.5);
}

// ── Cuisine ──────────────────────────────────────────────────────────────────
//
// La quatrième formule, et la plus dépaysante des quatre. Elle vit dans le
// `default:` du switch de `skill_produce_mix`, sous la condition
// `menuskill_id == AM_PHARMACY && menuskill_val > 10 && <= 20` — donc sans le
// moindre `case` nommé, ce qui la rend facile à manquer en lecture.
//
//   kit >= 15  ->  make_per = 10000, RÉUSSITE GARANTIE (aucun tirage)
//   sinon      ->  1200*(kit-10) + 20*(base_lv+1) + 20*(dex+1)
//                  + 100*(rnd()%span + lo)      lo = 6 + cm/80
//                                               span = 30 + 5*(cm/400) - lo
//                  - 400*(itemlv-10) - 10*(101-luk) - 500*(num-1)
//                  - 100*(rnd()%4 + 1)
//
// 🔴 TROIS singularités par rapport aux trois autres fabrications :
//  1. **aucun multiplicateur**. `pp_rate` est à l'intérieur du `case AM_PHARMACY:`
//     et `wp_rate` dans la branche « Weapon Forging » — ni l'un ni l'autre
//     n'atteint ce `default:`. Sur ce serveur où tout est ×5, la cuisine est la
//     seule fabrication qui tourne à taux nu ;
//  2. **pas de pénalité baby** non plus, pour la même raison de portée ;
//  3. **aucune compétence** n'entre en jeu : les 60 recettes ont `req_skill = 0`,
//     donc n'importe quelle classe cuisine. Ce qui décide, c'est le KIT.
//
// `num` (quantité) vaut toujours 1 ici : le buildin `cooking` passe `qty = 1`, que
// `clif_parse_Cooking` relit dans `menuskill_val2`. Le terme `-500*(num-1)` est
// donc nul — on le laisse écrit pour que la transcription reste vérifiable.
//
// Deux tirages indépendants, tous deux parcourus exhaustivement : celui gouverné
// par la maîtrise (au plus 24 valeurs) et le `rnd()%4` final. Au plus 96 issues.
//
// Rend -1 si le kit ou l'itemlv sont inconnus, ou si la maîtrise n'est pas encore
// arrivée (ZC 0x0F1C). Se taire vaut mieux qu'un chiffre à 20 points près.
int CookingChancePercent(uint32_t product_id, int kit_level, int cook_mastery) {
  if (kit_level < 11 || kit_level > 20) return -1;  // pas un kit de cuisine
  // 15+ : le serveur pose `make_per = 10000` sans rien tirer. Pas besoin de la
  // maîtrise, ni de l'itemlv, ni de quoi que ce soit d'autre.
  if (kit_level >= 15) return 100;
  if (cook_mastery < 0) return -1;

  const int item_lv = craftdata::RecipeItemLevel(product_id);
  if (item_lv < 11 || item_lv > 20) return -1;

  const int fixed = 1200 * (kit_level - 10)
                  + 20 * (rag::BaseLevel() + 1)
                  + 20 * (rag::StatTotal(rag::kDex) + 1)
                  - 400 * (item_lv - 10)
                  - 10 * (101 - rag::StatTotal(rag::kLuk));

  // ⚠ Divisions ENTIÈRES, et elles ne se simplifient pas : `cm/80` et `cm/400`
  // sautent par paliers, si bien que `span` ne décroît pas régulièrement (24 le
  // plus souvent, 20 tout en haut de la plage). Écrire `cm/80.0` donnerait un
  // autre résultat.
  const int lo   = 6 + cook_mastery / 80;
  const int span = 30 + 5 * (cook_mastery / 400) - lo;
  if (span <= 0) return -1;  // défensif : jamais atteint sur [0,1999]

  double sum = 0.0;
  for (int mastery_roll = 0; mastery_roll < span; ++mastery_roll) {
    for (int penalty_roll = 0; penalty_roll < 4; ++penalty_roll) {
      int per = fixed + 100 * (lo + mastery_roll) - 100 * (penalty_roll + 1);
      if (per < 1) per = 1;          // le floor du serveur, appliqué au même endroit
      if (per > 10000) per = 10000;  // `rnd()%10000 < per` sature à 100 %
      sum += per / 10000.0;
    }
  }
  return static_cast<int>(sum * 100.0 / (span * 4) + 0.5);
}

}  // namespace

void MakeItemWindow::DrawSuccessChance(const Entry& chosen) {
  // Les listes de flèches ne passent pas par `skill_produce_mix` du tout
  // (`skill_arrow_create`) : aucune chance à annoncer, la conversion est certaine.
  if (arrow_list_) return;

  const bool is_weapon = craftdata::WeaponLevel(chosen.id) > 0;
  const int  skill     = craftdata::RecipeSkill(chosen.id);
  const bool metal     = !is_weapon && (skill == kSkillIronTempering ||
                                        skill == kSkillSteelTempering ||
                                        skill == kSkillEnchantedStone);
  const bool potion    = !is_weapon && skill == kSkillPharmacy;
  // Un plat se reconnaît à son `itemlv` (11..20) et non à sa compétence : les 60
  // recettes de cuisine ont `req_skill = 0`, donc ni `metal` ni `potion` ne les
  // attrape. C'est aussi ce qui fait que n'importe quelle classe peut cuisiner.
  const int  dish_lv   = craftdata::RecipeItemLevel(chosen.id);
  const bool cooking   = !is_weapon && skill == 0 && dish_lv >= 11 && dish_lv <= 20;

  if (cooking) {
    // Le kit vient de l'OBJET consommé : son niveau n'est dans aucun paquet, et
    // c'est lui qui commande tout le reste.
    const int kit = craftdata::CookingKitLevel(source_item_id_);
    if (kit >= 11 && kit <= 20) {
      // ── Les ENTRÉES du calcul, montrées telles quelles ────────────────────
      // Pas du débogage : c'est ce qui rend le chiffre vérifiable par le joueur,
      // et actionnable — voir « DEX 13 » à côté d'un terme qui vaut 20 points par
      // point dit immédiatement où porter l'effort. Accessoirement, c'est la seule
      // façon de repérer un accesseur mémoire qui déraille sans debugger attaché.
      char inputs[224];
      std::snprintf(inputs, sizeof(inputs),
                    i18n::Tr("\n\nEntrées du calcul : kit niveau %d, plat niveau %d, "
                    "base level %d, DEX %d, LUK %d, maîtrise %s."),
                    kit, dish_lv, rag::BaseLevel(),
                    rag::StatTotal(rag::kDex), rag::StatTotal(rag::kLuk),
                    (cook_mastery_ >= 0) ? i18n::Tr("reçue") : i18n::Tr("NON REÇUE"));
      const int chance = CookingChancePercent(chosen.id, kit, cook_mastery_);
      if (chance >= 0) {
        ImGui::TextColored(V4(chance >= 80 ? kColOk : kColWarn),
                           "Chances : %d %%", chance);
        if (cook_mastery_ >= 0 && kit < 15) {
          ImGui::SameLine();
          ImGui::TextDisabled("(maîtrise %d / 1999)", cook_mastery_);
        }
        ImGui::SameLine();
        char tip[1024];
        std::snprintf(
            tip, sizeof(tip), "%s%s",
            (kit >= 15)
                ? i18n::Tr("Kit de niveau 15 : le serveur pose directement 100 %, sans "
                  "aucun tirage. Ni vos statistiques, ni le plat visé, ni votre "
                  "maîtrise culinaire n'entrent en jeu.") : i18n::Tr("Calcul du serveur, rejoué à l'identique :\n"
                  "  12 % par palier de kit au-dessus du premier (c'est le terme "
                  "qui pèse le plus)\n"
                  "  - 4 % par palier de difficulté du plat\n"
                  "  + base level x 20 + DEX x 20, et LUK qui réduit un malus fixe\n"
                  "  + un tirage gouverné par la MAÎTRISE CULINAIRE, puis un second "
                  "tirage de -1 à -4 %\n"
                  "\n"
                  "Le chiffre tient compte des deux tirages : c'est la moyenne "
                  "exacte de toutes leurs issues.\n"
                  "\n"
                  "La maîtrise monte à chaque plat réussi et redescend à chaque "
                  "échec, d'un pas d'autant plus grand que le plat est difficile. "
                  "Le jeu ne l'affiche nulle part : elle arrive ici par un paquet "
                  "propre au serveur Moonlight.\n"
                  "\n"
                  "La cuisine est la seule fabrication que les réglages serveur ne "
                  "multiplient PAS, et la seule qui n'exige aucune compétence."),
            inputs);
        HelpMarker(tip);
        return;
      }
      // ── Maîtrise absente : on BORNE au lieu de se taire ─────────────────────
      // Seule cause possible ici (le kit et l'itemlv sont connus, on vient de le
      // vérifier) : aucun ZC 0x0F1C reçu — serveur sans l'opcode, ou session non
      // reconnue comme Bourgeon.
      //
      // ⚠ Ce n'est PAS le cas « le joueur n'a jamais cuisiné » : `pc_readregistry`
      // rend 0 pour une variable absente, donc une maîtrise nulle est une valeur
      // reçue comme une autre, et le calcul exact s'applique.
      //
      // La maîtrise ne peut que déplacer le résultat entre ses deux extrêmes, et
      // ces extrêmes-là se calculent sans elle. Une fourchette vaut mieux qu'un
      // silence : elle borne pour de vrai, et elle montre au passage ce que la
      // maîtrise rapporte. (Elle serait un mauvais affichage PRINCIPAL — une
      // vingtaine de points de large — mais c'est un excellent repli.)
      const int worst = CookingChancePercent(chosen.id, kit, 0);
      const int best  = CookingChancePercent(chosen.id, kit, 1999);
      if (worst >= 0 && best >= 0) {
        // Bornes confondues : la maîtrise ne change rien ici (les termes fixes
        // saturent déjà, ou s'effondrent). Annoncer « entre 100 % et 100 % »
        // serait absurde — on donne le chiffre ferme, qui est exact.
        if (worst == best)
          ImGui::TextColored(V4(worst >= 80 ? kColOk : kColWarn),
                             "Chances : %d %%", worst);
        else
          ImGui::TextColored(V4(worst >= 80 ? kColOk : kColWarn),
                             "Chances : entre %d %% et %d %%", worst, best);
        ImGui::SameLine();
        char range_tip[1024];
        std::snprintf(
            range_tip, sizeof(range_tip), "%s%s",
            "La fourchette couvre toute la plage de maîtrise culinaire, de zéro "
            "au maximum : votre valeur exacte place le résultat quelque part "
            "entre ces deux bornes.\n"
            "\n"
            "Elle n'a pas été transmise. C'est une donnée que le client ne peut "
            "pas deviner - elle vit côté serveur et ne circule dans aucun paquet "
            "du jeu d'origine ; ce serveur doit la pousser explicitement. Une "
            "maîtrise de zéro, elle, s'afficherait normalement : c'est une valeur "
            "comme une autre.",
            inputs);
        HelpMarker(range_tip);
        return;
      }
      ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
      TextWrapped(
          i18n::Tr("Chances : non calculées (maîtrise culinaire non transmise par le "
          "serveur)."));
      ImGui::PopStyleColor();
      return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(
        i18n::Tr("Chances : non calculées (le kit utilisé n'a pas été identifié ; son "
        "niveau ne circule dans aucun paquet et se déduit de l'objet consommé)."));
    ImGui::PopStyleColor();
    return;
  }

  if (potion) {
    const int chance = PharmacyChancePercent(chosen.id);
    if (chance >= 0) {
      ImGui::TextColored(V4(chance >= 80 ? kColOk : kColWarn), "Chances : %d %%",
                         chance);
      ImGui::SameLine();
      HelpMarker(
          i18n::Tr("Calcul du serveur, rejoué à l'identique :\n"
          "  3 % par niveau de Pharmacy (le terme qui pèse le plus)\n"
          "  + 0,5 % par niveau de Potion Research\n"
          "  + job level x 20 + (INT / 2) x 10 + DEX x 10 + LUK x 10\n"
          "  + bonus du produit : +20 % pour les potions rouge/jaune/blanche, "
          "+10 % pour l'alcool, un tirage seul pour les bouteilles ; MALUS pour "
          "les potions slim et la bouteille de revêtement\n"
          "  puis multiplié par le réglage serveur potion_produce_rate.\n"
          "\n"
          "Contrairement aux métaux et à la forge, il n'y a PAS de tirage de base "
          "ici : un produit sans bonus (Blue Potion, Anodyne, Aloebera) a une "
          "chance parfaitement déterminée.\n"
          "\n"
          "Non pris en compte : le bonus d'un homoncule Vanilmirth avec Instruct "
          "(+1 % par niveau) et la pénalité de 30 % des classes baby. Le chiffre "
          "est donc légèrement pessimiste dans ces deux cas."));
      return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(
        i18n::Tr("Chances : non calculées (le fichier de recettes ne porte pas le réglage "
        "serveur potion_produce_rate, qui multiplie le résultat)."));
    ImGui::PopStyleColor();
    return;
  }

  if (metal) {
    const int chance = MetalCraftChancePercent(chosen.id, skill);
    // Vert au-dessus de 80 %, ambre en dessous : à ce jeu-là un échec consomme les
    // matériaux sans rien rendre, la couleur doit dire le risque et pas décorer.
    ImGui::TextColored(V4(chance >= 80 ? kColOk : kColWarn), "Chances : %d %%",
                       chance);
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("Calcul du serveur, rejoué à l'identique :\n"
        "  job level x 20 + DEX x 10 + LUK x 10\n"
        "  + bonus de la compétence de la recette (45 à 65 % pour Iron "
        "Tempering, 35 à 55 % pour Steel, 15 à 35 % pour Enchanted Stone)\n"
        "  + un tirage aléatoire de 0,1 à 10 %\n"
        "\n"
        "Le chiffre affiché tient compte de ce tirage : c'est la moyenne de ses "
        "100 issues possibles, donc la probabilité exacte.\n"
        "\n"
        "DEX et LUK sont les valeurs EFFECTIVES, équipement et cartes comprises — "
        "celles que montre ta feuille de personnage.\n"
        "\n"
        "Un échec consomme les matériaux sans rien produire."));
    return;
  }

  if (is_weapon) {
    // Les emplacements pèsent LOURD sur cette chance (-15 % par Star Crumb, -25 %
    // pour une pierre) : le chiffre doit donc se recalculer à chaque changement, et
    // c'est tout l'intérêt de l'afficher ICI, au-dessus des emplacements.
    int star_crumbs = 0;
    bool element = false;
    for (int i = 0; i < 3; ++i) {
      if (forge_slot_[i] == kMatStarCrumb) ++star_crumbs;
      else if (forge_slot_[i] >= kMatElemFirst && forge_slot_[i] <= kMatElemLast)
        element = true;  // une seule compte, cf. ForgeChancePercent
    }
    const int chance =
        ForgeChancePercent(chosen.id, skill, star_crumbs, element);
    if (chance >= 0) {
      ImGui::TextColored(V4(chance >= 80 ? kColOk : kColWarn), "Chances : %d %%",
                         chance);
      ImGui::SameLine();
      HelpMarker(
          i18n::Tr("Calcul du serveur, rejoué à l'identique :\n"
          "  job level x 20 + DEX x 10 + LUK x 10\n"
          "  + niveau de l'arme (+40 % en lv1, +20 % lv2, +10 % lv3, RIEN en lv4)\n"
          "  + 5 % par niveau de ta compétence de forge\n"
          "  + 1 % par niveau de Weaponry Research\n"
          "  - 25 % si une pierre élémentaire est posée (une seule compte)\n"
          "  - 15 % par Star Crumb posé\n"
          "  + enclume la MEILLEURE portée (Emperium +10, Golden +5, Oridecon +2,5)\n"
          "  + un tirage aléatoire de 0,1 à 10 %\n"
          "  puis multiplié par le réglage serveur weapon_produce_rate.\n"
          "\n"
          "Le chiffre tient compte du tirage : c'est la moyenne de ses 100 issues, "
          "donc la probabilité exacte. Il se recalcule quand tu changes les "
          "emplacements ci-dessous.\n"
          "\n"
          "Non pris en compte : la pénalité de 30 % des classes baby."));
      return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(
        i18n::Tr("Chances : non calculées (le fichier de recettes ne porte pas le réglage "
        "serveur weapon_produce_rate, qui multiplie le résultat)."));
    ImGui::PopStyleColor();
    return;
  }

  // Reste les métiers non transcrits : potions (bonus d'homoncule), cuisine
  // (`cook_mastery`), poisons, GN_CHANGEMATERIAL… chacun a sa propre branche dans
  // `skill_produce_mix`. On le dit plutôt que de laisser un blanc.
  ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
  TextWrapped(
      i18n::Tr("Chances : non calculées pour cette compétence (chaque métier a sa propre "
      "formule côté serveur)."));
  ImGui::PopStyleColor();
}

void MakeItemWindow::DrawRecipe() {
  if (sel_id_ < 0 || entries_.empty()) return;

  const Entry* chosen = nullptr;
  for (const Entry& e : entries_)
    if (static_cast<int>(e.id) == sel_id_) { chosen = &e; break; }
  if (!chosen) return;

  Separator();
  if (chosen->mat_count == 0) {
    // Dire l'ABSENCE plutôt que de ne rien afficher : sur ce serveur, 166 des 254
    // produits fabricables n'ont aucune recette dans le fichier client. Un blanc
    // se lirait comme « pas de matériaux requis », ce qui est faux.
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(i18n::Tr("Recette inconnue du client pour ce produit."));
    ImGui::PopStyleColor();
    return;
  }

  if (arrow_list_) {
    // Sur une liste de FLÈCHES, l'entrée choisie EST le matériau : accoler
    // « 's required materials » à son nom serait un contresens (la recette
    // liste le matériau et son rendement). Pas de MsgString client pour ce
    // cas — sa fenêtre 94 n'affiche jamais de recette — d'où le libellé maison.
    SeparatorText(i18n::Tr("Crafting result"));
  } else {
    // Libellé EXACT du client : MsgString 427 = « 's required materials », que le
    // natif accole au nom du produit dans sa fenêtre 80.
    const char* raw_suffix = msgstr::Utf8(kMsgRequireForMake);
    const char* suffix = raw_suffix[0] ? raw_suffix : nullptr;
    if (suffix)
      SeparatorText((std::string(chosen->name) + suffix).c_str());
    else
      SeparatorText(i18n::Tr("Matériaux requis"));
  }

  DrawSuccessChance(*chosen);

  const ImGuiStyle& style = ImGui::GetStyle();
  for (int i = 0; i < chosen->mat_count; ++i) {
    const Material& m = chosen->mats[i];
    const int have = m.id ? OwnedCount(m.id) : 0;
    // Un matériau non consommé est satisfait dès qu'on en a UN : exiger `qty`
    // l'aurait affiché en rouge alors que la fabrication est possible.
    const bool enough = m.not_consumed ? (have >= 1) : (have >= m.qty);

    // Nom d'AFFICHAGE : celui de la DB client quand l'id est résolu (il suit la
    // langue du serveur), sinon la chaîne brute de la recette. Jamais une
    // constante — cf. la règle « jamais de données codées en dur ».
    const MoonlightUi* ui = Bourgeon::Instance().moonlight_ui();
    const char* db_name = (m.id && ui) ? ui->ItemName(m.id) : nullptr;
    char label[192];
    if (m.id && m.is_output)
      // Le rendement se lit d'un coup d'œil comme tel : une flèche, pas une
      // quantité de plus dans une liste d'exigences.
      std::snprintf(label, sizeof(label), "-> %d %s", m.qty,
                    (db_name && *db_name) ? db_name : m.name);
    else if (m.id && m.not_consumed)
      // Le libellé appartient à l'UI, pas au fichier de recettes : celui-ci n'a
      // qu'à porter le marqueur (quantité 0). Écrit ainsi, il dit la SEULE chose
      // qui compte pour le joueur — l'objet est exigé mais ne sera pas perdu.
      std::snprintf(label, sizeof(label), i18n::Tr("%s  (requis, non consommé)"),
                    (db_name && *db_name) ? db_name : m.name);
    else if (m.id && !enough)
      // Le MANQUE plutôt que le seul stock : « (3) » oblige à faire la
      // soustraction de tête pour chaque ligne rouge, et c'est justement le
      // chiffre qu'on va chercher — combien aller ramasser.
      std::snprintf(label, sizeof(label), i18n::Tr("%d %s  (%d — il en manque %d)"), m.qty,
                    (db_name && *db_name) ? db_name : m.name, have,
                    m.qty - have);
    else if (m.id)
      std::snprintf(label, sizeof(label), "%d %s  (%d)", m.qty,
                    (db_name && *db_name) ? db_name : m.name, have);
    else  // non résolu : on montre la ligne telle que le client l'a écrite
      std::snprintf(label, sizeof(label), "%d %s", m.qty, m.name);

    ImGui::BeginGroup();
    if (m.id) {
      const ro::IconTex ic = ro::ItemIcon(m.id, 1);
      if (ic.tex) ImGui::Image(TexId(ic.tex), ImVec2(kMatIcon, kMatIcon));
      else        ImGui::Dummy(ImVec2(kMatIcon, kMatIcon));
      ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    }
    // Le stock qui NE SUFFIT PAS est l'information la plus utile de cette liste :
    // il dit exactement ce qui bloque. Il est donc en rouge, pas grisé.
    const ImVec4 col = enough ? V4(kColOk) : V4(kColBad);
    const ImVec2 text_min = ImGui::GetCursorScreenPos();
    ImGui::TextColored(col, "%s", label);
    const ImVec2 text_max = ImGui::GetItemRectMax();
    ImGui::EndGroup();

    if (!m.id) continue;  // rien à ouvrir sans id : pas de faux lien

    // Après EndGroup, « le dernier item » EST le groupe : survol et clic portent
    // sur l'icône ET le texte. Ni Image ni Text ne consomment d'entrée, donc rien
    // à l'intérieur ne brouille le test.
    if (ImGui::IsItemHovered()) {
      // Souligné + curseur main : c'est ce qui fait lire « lien » plutôt que
      // « étiquette ». Le trait suit le TEXTE seul — sous l'icône il serait dans
      // le vide.
      ImGui::GetWindowDrawList()->AddLine(
          ImVec2(text_min.x, text_max.y), ImVec2(text_max.x, text_max.y),
          ImGui::ColorConvertFloat4ToU32(col));
      ro::SetHoverCursor(kRoCursorHand);
      // Même mécanique que les lignes de produit : on MÉMORISE, le popup est
      // peint après EndRoWindow. C'est ce qui manquait ici — les liens avaient
      // le souligné et le curseur, mais aucun aperçu.
      if (desc_tooltip_) {
        hover_valid_ = true;
        hover_id_    = m.id;
        std::snprintf(hover_name_, sizeof(hover_name_), "%s",
                      (db_name && *db_name) ? db_name : m.name);
      }
    }
    // Gauche comme droit ouvrent la description : un matériau ne se sélectionne
    // pas, il n'y a donc aucun geste concurrent à voler (contrairement aux lignes
    // de produit, où le gauche sélectionne).
    // ⚠ Par ID : le matériau peut ne pas être en inventaire — c'est même le cas
    // intéressant — donc il n'y a pas toujours d'ItemSkillInfo vivant à passer.
    if (ImGui::IsItemClicked() || IsLastItemRightClicked()) {
      const ImVec2 mouse = ImGui::GetMousePos();
      itemcell::DeferDescById(m.id, 0, 0, static_cast<int>(mouse.x),
                              static_cast<int>(mouse.y));
    }
  }
}

void MakeItemWindow::DrawForgeSlots() {
  if (proto_ != Proto::kProduce || sel_id_ < 0) return;
  if (!ProductAcceptsForgeSlots(static_cast<uint32_t>(sel_id_), skill_id_,
                                from_item_))
    return;

  // Changer de produit remet les emplacements à zéro : emporter un Star Crumb
  // choisi pour une épée sur une arme qu'on vient de sélectionner serait une
  // dépense que personne n'a demandée.
  if (forge_for_id_ != sel_id_) {
    forge_for_id_  = sel_id_;
    forge_slot_[0] = forge_slot_[1] = forge_slot_[2] = 0;
  }

  const MoonlightUi* ui = Bourgeon::Instance().moonlight_ui();
  auto NameOf = [ui](uint32_t id) -> const char* {
    const char* n = ui ? ui->ItemName(id) : nullptr;
    return (n && *n) ? n : "?";
  };

  SeparatorText(i18n::Tr("Matériaux optionnels"));

  // Largeur DÉDUITE de la place réelle, pas fixée : trois combos de 150 px font
  // 460 px dans une fenêtre qui en fait ~360, et c'est le troisième emplacement
  // — celui qu'on remplit en dernier — qui sortait du cadre. Troisième fois que
  // la largeur fixe mord aujourd'hui ; on ne pose plus de constante en dur dans
  // cette fenêtre.
  const float slot_w =
      (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) /
      3.0f;

  for (int i = 0; i < 3; ++i) {
    ImGui::PushID(i);
    ImGui::SetNextItemWidth(slot_w);
    const char* preview = forge_slot_[i] ? NameOf(forge_slot_[i]) : "(vide)";
    // ro::RoBeginCombo, pas ImGui::BeginCombo : même raison que les boutons, les
    // cases et le curseur du pied — la fenêtre porte le skin RO de bout en bout,
    // et un widget ImGui nu y devient l'exception qu'on remarque.
    if (ro::RoBeginCombo("##forgeslot", preview)) {
      if (ImGui::Selectable("(vide)", forge_slot_[i] == 0)) forge_slot_[i] = 0;

      // ── Ce qu'on propose, et ce qu'on ne propose PAS ─────────────────────
      // Les deux familles n'obéissent pas à la même règle serveur, et les
      // présenter à l'identique laissait composer des sélections sans effet :
      //   - Star Crumb : CUMULATIF (`sc++` à chaque emplacement) — donc offert
      //     dans les trois, mais jamais au-delà du stock réel ;
      //   - pierre élémentaire : seule la PREMIÈRE compte (`ele == 0` en garde),
      //     les suivantes sont ignorées ET non consommées. En proposer une
      //     seconde, c'était proposer un geste nul. Dès qu'une pierre est posée,
      //     les autres emplacements n'en offrent plus.
      // Rendre l'état absurde impossible vaut mieux que l'avertir après coup.
      for (uint32_t id = kMatElemFirst; id <= kMatStarCrumb; ++id) {
        // 998/999 (Iron/Steel) sont dans l'intervalle mais ne sont PAS des
        // matériaux optionnels : le serveur ne teste que les pierres
        // élémentaires et le Star Crumb.
        if (id > kMatElemLast && id != kMatStarCrumb) continue;
        const int have = OwnedCount(id);
        if (have <= 0) continue;

        // Combien d'exemplaires les AUTRES emplacements réclament-ils déjà ?
        // Le serveur résout chaque emplacement séparément : trois Star Crumb
        // demandés avec un seul en sac, ce sont deux emplacements sans effet.
        int used_elsewhere = 0;
        bool stone_elsewhere = false;
        for (int j = 0; j < 3; ++j) {
          if (j == i) continue;
          if (forge_slot_[j] == id) ++used_elsewhere;
          if (forge_slot_[j] >= kMatElemFirst && forge_slot_[j] <= kMatElemLast)
            stone_elsewhere = true;
        }
        if (used_elsewhere >= have) continue;
        const bool is_stone = (id >= kMatElemFirst && id <= kMatElemLast);
        if (is_stone && stone_elsewhere) continue;

        char label[128];
        // Le stock affiché est celui qui reste DISPONIBLE pour cet emplacement,
        // pas le stock brut : avec 2 Star Crumb dont 1 déjà posé ailleurs, lire
        // « (2) » ici inviterait à en poser un troisième.
        std::snprintf(label, sizeof(label), "%s  (%d)", NameOf(id),
                      have - used_elsewhere);
        if (ImGui::Selectable(label, forge_slot_[i] == id)) forge_slot_[i] = id;
      }
      ro::RoEndCombo();
    }
    ImGui::PopID();
    if (i < 2) SameLine();
  }

  // ── Ce que le natif ne dit NULLE PART ─────────────────────────────────────
  // Son écran aligne trois emplacements vides et invite à les remplir, sans
  // jamais mentionner que chaque Star Crumb fait BAISSER le taux de réussite.
  int      star_crumbs = 0;
  uint32_t element_id  = 0;
  for (int i = 0; i < 3; ++i) {
    if (forge_slot_[i] == kMatStarCrumb) ++star_crumbs;
    else if (forge_slot_[i] >= kMatElemFirst && forge_slot_[i] <= kMatElemLast &&
             element_id == 0)
      element_id = forge_slot_[i];
  }

  if (star_crumbs > 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColWarn);
    // ⚠ Libellé COURT à dessein. Cette ligne se termine par un « (?) » posé en
    // SameLine, et la fenêtre a une largeur fixe : toute phrase un peu longue
    // pousse le marqueur hors du cadre, où il se fait rogner. Même cause que le
    // débordement du pied — dans une largeur contrainte, ce qui suit un texte de
    // longueur variable est toujours le premier à sauter.
    // ⚠ Trait d'union ASCII, PAS le signe moins typographique U+2212. Les glyphes
    // chargés couvrent la ponctuation générale (d'où les « — » et « … » qui
    // passent ailleurs) mais pas le bloc Mathematical Operators : U+2212 sort en
    // caractère manquant.
    Text(i18n::Tr("%d Star Crumb : +%d ATK, -%d %% de réussite"), star_crumbs,
         star_crumbs * kStarCrumbAtkBonus,
         star_crumbs * kStarCrumbMalusPercent);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("Le compromis que le natif ne montre jamais.\n\n"
        "Chaque Star Crumb ajoute +5 ATK à l'arme forgée "
        "(card[1] = ((sc*5) << 8) + élément) et retranche 1500 au taux de "
        "réussite — lequel se compte sur 10000, donc 15 points de pourcentage "
        "(make_per -= sc * 1500 ; le tirage est rnd()%10000 < make_per).\n\n"
        "Trois Star Crumb, c'est donc +15 ATK contre 45 points de réussite en "
        "moins. L'écran natif aligne trois emplacements et n'en dit rien."));
  }
  if (element_id != 0) {
    // ⚠ En couleur d'AVERTISSEMENT, pas en vert : la pierre donne l'élément mais
    // coûte 25 points de réussite, davantage qu'un Star Crumb. L'afficher comme un
    // gain pur était trompeur.
    ImGui::PushStyleColor(ImGuiCol_Text, kColWarn);
    Text(i18n::Tr("Élément %s : -%d %% de réussite"), NameOf(element_id),
         kElementMalusPercent);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("Une seule pierre élémentaire par arme : le serveur ne retient que la "
        "première (`ele == 0` en garde) et ne consomme même pas les suivantes.\n\n"
        "C'est pourquoi les autres emplacements n'en proposent plus dès qu'une "
        "est posée — un second choix n'aurait aucun effet.\n\n"
        "Elle coûte 25 points de réussite (make_per -= 2500 sur une échelle de "
        "10000), soit plus qu'un Star Crumb. Le natif ne le dit nulle part."));
  }

  // Le TOTAL, dès qu'il y a deux contributions : c'est le chiffre qu'on cherche,
  // et le laisser à additionner de tête était le même défaut que le « (3) » des
  // matériaux manquants. ⚠ Les trois emplacements bornent le cumul : au pire
  // 2 Star Crumb + 1 pierre = -55 %, ou 3 Star Crumb = -45 %. Il n'existe pas de
  // combinaison à quatre objets.
  const int total_malus =
      star_crumbs * kStarCrumbMalusPercent +
      (element_id ? kElementMalusPercent : 0);
  if (star_crumbs > 0 && element_id != 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColBad);
    Text(i18n::Tr("Total : -%d %% de réussite"), total_malus);
    ImGui::PopStyleColor();
  }

  // ── L'ENCLUME ────────────────────────────────────────────────────────────
  // Elle n'apparaît dans aucune recette et n'est jamais consommée : le serveur
  // vérifie seulement sa PRÉSENCE en inventaire. C'est donc une information que
  // rien, dans le jeu, ne rattache à la forge — alors qu'elle vaut jusqu'à
  // 10 points de réussite. On la remonte ici, avec ce qu'elle rapporte.
  {
    // ⚠ La table kAnvils est rangée du MEILLEUR au moins bon, et on s'arrête au
    // premier trouvé : c'est la transcription littérale de la chaîne de
    // `else if` du serveur. Ce qu'on nomme est donc bien celle qui comptera —
    // toute la valeur de cet affichage est là, un joueur qui en porte plusieurs
    // n'a aucun moyen de le savoir autrement.
    const AnvilBonus* best_anvil = nullptr;
    int other_anvils = 0;
    for (const AnvilBonus& a : kAnvils) {
      if (OwnedCount(a.id) <= 0) continue;
      if (!best_anvil) best_anvil = &a;
      else ++other_anvils;
    }
    if (best_anvil) {
      ImGui::PushStyleColor(ImGuiCol_Text,
                            best_anvil->percent > 0 ? kColOk : kColDim);
      Text(i18n::Tr("Enclume retenue : %s (+%d %%)"), NameOf(best_anvil->id),
           best_anvil->percent);
      ImGui::PopStyleColor();
      if (other_anvils > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
        Text(i18n::Tr("(%d autre%s en sac, sans effet)"), other_anvils,
             other_anvils > 1 ? "s" : "");
        ImGui::PopStyleColor();
      }
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, kColBad);
      TextUnformatted(i18n::Tr("Aucune enclume en sac."));
      ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("L'enclume n'est PAS un matériau : elle n'entre dans aucune recette et "
        "n'est jamais consommée. Le serveur vérifie seulement que tu en portes "
        "une (pc_search_inventory) et ajoute au taux de réussite :\n\n"
        "  Emperium Anvil  +10 %\n"
        "  Golden Anvil    +5 %\n"
        "  Oridecon Anvil  +2,5 %\n"
        "  Anvil           +0 %\n\n"
        "Seule la MEILLEURE compte (chaîne de « else if » côté serveur) : en "
        "cumuler plusieurs n'apporte rien de plus."));
  }

  // Un même objet placé deux fois exige deux exemplaires. Le serveur cherche
  // chaque emplacement séparément : sans ce contrôle, on enverrait une demande
  // dont une partie serait silencieusement sans effet.
  for (int i = 0; i < 3; ++i) {
    if (!forge_slot_[i]) continue;
    int used = 0;
    for (int j = 0; j < 3; ++j) if (forge_slot_[j] == forge_slot_[i]) ++used;
    const int have = OwnedCount(forge_slot_[i]);
    if (used > have) {
      ImGui::PushStyleColor(ImGuiCol_Text, kColBad);
      Text(i18n::Tr("%s : %d demandés, %d en sac"), NameOf(forge_slot_[i]), used, have);
      ImGui::PopStyleColor();
      break;  // un seul message suffit à dire que la sélection est intenable
    }
  }
}

void MakeItemWindow::DrawFooter() {
  Separator();

  // `list_armed_` : depuis que la table survit à l'envoi, une liste affichée ne
  // veut plus dire « actionnable ». Le bouton doit se griser pendant l'aller-
  // retour, exactement comme RequestMake refuse d'envoyer.
  const bool can_make = ui_open_ && list_armed_ && !entries_.empty() &&
                        !awaiting_result_ && sel_id_ >= 0 && sel_visible_;
  // ── « Fabriquer » et « Relancer » sont EXCLUSIFS ──────────────────────────
  // Une fois la liste consommée, le serveur a effacé son `menuskill` : fabriquer
  // n'a plus de sens, et rien dans la fenêtre ne permettait de repartir — il
  // fallait la fermer et relancer la compétence à la barre d'action. Le bouton
  // prend donc la place du premier, comme dans la fenêtre de refine. Les afficher
  // côte à côte suggérerait deux façons de faire la même chose, alors que ce sont
  // deux étapes successives.
  //
  // ⚠ Et il n'apparaît QUE si rien ne ramènera la liste tout seul. Le critère est
  // « une relance est-elle ARMÉE », et non « le réglage est-il coché » : c'est plus
  // juste dans les deux sens — relance armée, la liste revient en quelques
  // centaines de ms et le bouton ne ferait que clignoter ; chaîne STOPPÉE alors
  // que le réglage reste coché (refus serveur, stock épuisé, plafond atteint), et
  // le bouton reparaît, ce qu'un test sur le seul réglage aurait empêché — la
  // fenêtre serait restée sans issue.
  const bool relaunch_coming = auto_recast_at_ != 0 ||
                               pending_ == Pending::kRecast ||
                               pending_ == Pending::kReuseItem;
  const bool spent =
      ui_open_ && !list_armed_ && !awaiting_result_ && !relaunch_coming;
  if (spent) {
    // Ce qu'il faut pour repartir dépend de l'origine de la liste — et du côté
    // OBJET, ça COÛTE un exemplaire. Le bouton le dit avant le clic.
    const bool     by_item = from_item_;
    const bool     have_item =
        by_item && source_item_id_ != 0 && OwnedCount(source_item_id_) > 0;
    const bool     can_relaunch = by_item ? have_item : (skill_id_ > 0);
    ImGui::BeginDisabled(!can_relaunch);
    if (ro::RoButton("Relancer", kBtnW)) {
      // Geste MANUEL : la chaîne repart de zéro. Sans cette remise à plat, le
      // compteur d'une série précédente s'afficherait sur la suivante.
      auto_chain_      = 0;
      auto_items_used_ = 0;
      batch_left_      = 0;
      batch_fire_      = false;
      auto_stop_reason_.clear();
      auto_ours_       = true;  // la liste qui suivra est la nôtre
      pending_ = by_item ? Pending::kReuseItem : Pending::kRecast;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
      if (!can_relaunch)
        ImGui::SetTooltip(
            by_item ? i18n::Tr("Il ne te reste plus l'objet qui a ouvert cette liste.") : i18n::Tr("Compétence d'origine inconnue : relance impossible."));
      else if (by_item)
        ImGui::SetTooltip(
            i18n::Tr("Ré-utilise %s pour rouvrir une liste.\n"
            "\n"
            "L'objet est CONSOMMÉ : le serveur le détruit avant d'exécuter son "
            "script, même si tu annules ensuite."),
            source_item_name_);
      else
        ImGui::SetTooltip(
            i18n::Tr("Relance la compétence pour obtenir une nouvelle liste.\n"
            "\n"
            "Le serveur n'autorise qu'une fabrication par lancement : après "
            "chaque objet produit, il faut relancer."));
    }
  } else {
    // ⚠ ro::RoButton, PAS ImGui::Button : le corps d'une fenêtre Bourgeon porte le
    // skin RO (art clair, 9-slice), et un bouton ImGui nu y détonne.
    ImGui::BeginDisabled(!can_make);
    if (ro::RoButton("Fabriquer", kBtnW)) RequestMake();
    ImGui::EndDisabled();
  }

  SameLine();
  if (ro::RoButton("Fermer", kBtnW)) CloseAndCancel();

  // ── Quantité voulue — SUR SA PROPRE LIGNE ─────────────────────────────────
  // 🔴 Ces widgets étaient à la suite des deux boutons. La barre débordait alors
  // la largeur fixe de la fenêtre : « série : 4/50 » sortait tronqué, et surtout
  // le bloc d'explication qui suit (un TextWrapped après SameLine) héritait d'une
  // largeur d'enroulement quasi nulle — chaque glyphe sur sa ligne, toutes
  // rognées hors du cadre. D'où un grand vide et une fenêtre qui grandissait d'un
  // coup entre deux fabrications.
  //
  // La règle qui en sort : **jamais de TextWrapped après un SameLine** dans une
  // fenêtre à largeur contrainte. Le texte prend sa ligne.
  //
  // Le serveur n'accepte pas de quantité : « ×20 » veut dire vingt tours
  // complets. Le champ n'est donc qu'une CIBLE.
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled(i18n::Tr("Quantité"));
  SameLine();
  // ro::RoSmallButton, pas les flèches d'ImGui::InputInt : celles-ci sont des
  // boutons ImGui nus au milieu d'un pied entièrement habillé RO.
  if (ro::RoSmallButton("-##batchdec") && batch_target_ > 1) --batch_target_;
  SameLine();
  ImGui::SetNextItemWidth(56.0f);
  // step = 0 : on supprime les flèches natives d'InputInt, les nôtres les
  // remplacent de part et d'autre.
  if (ImGui::InputInt("##batch", &batch_target_, 0, 0)) {
    if (batch_target_ < 1) batch_target_ = 1;
    if (batch_target_ > 999) batch_target_ = 999;
  }
  SameLine();
  if (ro::RoSmallButton("+##batchinc") && batch_target_ < 999) ++batch_target_;
  SameLine();
  if (batch_left_ > 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColWarn);
    Text("série : %d/%d", batch_target_ - batch_left_, batch_target_);
    ImGui::PopStyleColor();
  } else {
    HelpMarker(
        i18n::Tr("Combien de fabrications partent TOUTES SEULES.\n\n"
        "À ne pas confondre avec « relancer automatiquement », qui rouvre la "
        "liste après chaque fabrication : à 1 avec cette case cochée, une "
        "fabrication part seule, la liste revient, et le clic suivant est à "
        "toi.\n\n"
        "Le serveur n'accepte AUCUNE quantité : une liste = une fabrication "
        "(clif_menuskill_clear). « ×20 » signifie donc vingt tours complets — "
        "relance, nouvelle liste, nouvelle demande.\n\n"
        "Il faut par conséquent que la relance automatique correspondante soit "
        "active, sinon aucune liste ne revient et la série s'arrête au premier "
        "tour. La fenêtre le dit si le cas se présente.\n\n"
        "Même effet qu'Entrée maintenue, mais la cible est CHIFFRÉE : on peut "
        "lâcher le clavier, et ça s'arrête au compte demandé plutôt qu'au moment "
        "où l'on pense à relâcher."));
  }

  // La série ne peut pas avancer sans la relance qui va la nourrir. Le dire AVANT
  // qu'elle s'arrête toute seule, plutôt que de laisser conclure à une panne.
  if (batch_target_ > 1) {
    const bool relance_ok = from_item_ ? auto_reuse_item_ : auto_recast_;
    if (!relance_ok) {
      ImGui::PushStyleColor(ImGuiCol_Text, kColWarn);
      TextWrapped(from_item_
                      ? i18n::Tr("Série sans effet : la relance par OBJET est désactivée, "
                        "aucune nouvelle liste ne reviendra après la première "
                        "fabrication.") : i18n::Tr("Série sans effet : la relance automatique de la "
                        "compétence est désactivée, aucune nouvelle liste ne "
                        "reviendra après la première fabrication."));
      ImGui::PopStyleColor();
    }
  }

  // Dire POURQUOI c'est grisé, plutôt que de laisser deviner. Le cas dangereux
  // est le troisième : une sélection filtrée hors de la vue serait une cible
  // invisible pour une action qui consomme des matériaux.
  //
  // ⚠ PAS de SameLine ici : ces messages sont enroulés, et un TextWrapped placé
  // en fin de barre reçoit une largeur d'enroulement quasi nulle (cf. le pavé
  // au-dessus de « Quantité »). Ils prennent leur ligne.
  if (!can_make) {
    if (awaiting_result_) {
      ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
      TextUnformatted(i18n::Tr("Demande envoyée — en attente du serveur…"));
      ImGui::PopStyleColor();
    } else if (entries_.empty() || !list_armed_) {
      // Liste consommée : on affiche le RÉSULTAT, pas un « aucune liste ». C'est
      // tout l'intérêt de survivre à la fenêtre native — le client, lui, compose
      // ce message puis le jette sans jamais l'écrire (§3.7).
      if (!last_result_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, last_result_color_);
        TextWrapped(last_result_.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
        TextUnformatted(i18n::Tr("Aucune liste en cours."));
        ImGui::PopStyleColor();
      }
    } else {
      ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
      if (sel_id_ < 0)
        TextUnformatted(i18n::Tr("Choisis un produit."));
      else
        TextUnformatted(i18n::Tr("Le produit visé est masqué par le filtre."));
      ImGui::PopStyleColor();
    }
  }

  // Une action que le client prend de lui-même DOIT se voir, et la raison de son
  // arrêt doit rester à l'écran.
  if (auto_chain_ > 0 && auto_recast_at_) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColWarn);
    // Une relance par OBJET dépense du stock : elle dit CE qu'elle consomme et
    // où en est le plafond. Une relance par compétence ne coûte que du SP, le
    // simple compteur suffit.
    if (from_item_ && source_item_id_ != 0 && auto_reuse_max_ > 0)
      Text(i18n::Tr("Relance automatique… %s consommé %d/%d"), source_item_name_,
           auto_items_used_, auto_reuse_max_);
    else if (from_item_ && source_item_id_ != 0)
      Text(i18n::Tr("Relance automatique… %s consommé ×%d"), source_item_name_,
           auto_items_used_);
    else
      Text(i18n::Tr("Relance automatique… (%d)"), auto_chain_);
    ImGui::PopStyleColor();
  } else if (!auto_stop_reason_.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kColDim);
    TextWrapped(auto_stop_reason_.c_str());
    ImGui::PopStyleColor();
  }
}

bool MakeItemWindow::DrawSettings() {
  bool changed = false;

  // ⚠ ro::RoCheckbox, PAS ImGui::Checkbox : le panneau Moonlight porte le skin RO
  // comme le reste, et un widget ImGui nu y détonne. Même remarque que pour les
  // boutons du pied de la fenêtre.
  ImGui::TextDisabled(
      i18n::Tr("Remplace les DEUX listes natives : « LIST » (flèches, convertisseurs, "
      "poison, leurres, cuisine, bombes) et « Manufacturing List » "
      "(pharmacie, runes, forge)."));
  ImGui::TextDisabled(
      i18n::Tr("Clic droit : description · double-clic ou Entrée : fabriquer."));
  ImGui::TextDisabled(
      i18n::Tr("En-têtes de colonne : trier (3e clic = ordre du serveur)."));

  changed |= ro::RoCheckbox(i18n::Tr("Colonnes « Possédé » et « Faisable »"), &show_owned_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Le stock en inventaire, et le nombre de fabrications possibles déduit de "
      "la recette du client.\n\n« — » signifie « on ne sait pas » : la recette "
      "manque au fichier client, ou un nom de matériau n'a pas pu être résolu. "
      "Jamais « 0 » — le serveur n'aurait pas proposé le produit s'il n'y avait "
      "pas de quoi en faire au moins un."));

  changed |= ro::RoCheckbox(i18n::Tr("Champ de filtre"), &show_filter_);
  changed |= ro::RoCheckbox(i18n::Tr("Aperçu au survol"), &desc_tooltip_);
  changed |= ro::RoCheckbox(i18n::Tr("Journal de session"), &show_history_);
  if (show_history_)
    changed |= ro::RoCheckbox(i18n::Tr("Horodater le journal"), &log_time_);

  changed |= ro::RoCheckbox(i18n::Tr("Entrée lance la fabrication"), &enter_key_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Décoché (défaut), la touche Entrée reste au CHAT pendant que la fenêtre "
      "est ouverte.\n\n"
      "Cochée, elle déclenche la fabrication du produit sélectionné — et la "
      "maintenir enchaîne. Mais la fenêtre confisque alors la touche tant "
      "qu'elle est ouverte : impossible d'ouvrir la saisie du chat.\n\n"
      "Depuis que le champ Quantité existe, marteler Entrée n'a plus grand "
      "intérêt : « ×20 » fait le même travail sans occuper le clavier."));

  changed |= ro::RoCheckbox(i18n::Tr("Relancer la compétence automatiquement"),
                            &auto_recast_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Le serveur n'autorise qu'UNE fabrication par lancement de compétence "
      "(clif_menuskill_clear). Sans ça, enchaîner impose de retourner à la barre "
      "de raccourcis à chaque fois.\n\n"
      "Ce qui se relance seul, c'est le LANCEMENT DE LA COMPÉTENCE — jamais la "
      "fabrication : le choix du produit et le déclenchement restent des clics. "
      "Aucune tentative ne part sans un geste de ta part.\n\n"
      "La chaîne s'arrête d'elle-même sur une liste vide (le serveur dit qu'il "
      "n'y a plus rien) ou sur un refus de compétence.\n\n"
      "NE S'APPLIQUE PAS aux listes ouvertes par un OBJET (Mini Furnace, "
      "marteaux de forge : leur script fait « produce N; »). Ces objets sont "
      "CONSOMMÉS à chaque usage : ils ont leur propre réglage, juste en dessous."));

  // ── Relance par OBJET : réglage SÉPARÉ, et il doit le rester ──────────────
  // Cocher « relancer la compétence » ne peut pas valoir permission de dépenser
  // du stock. Le libellé annonce la dépense AVANT la case, pas dans une bulle
  // d'aide qu'on peut ne jamais ouvrir.
  changed |= ro::RoCheckbox(i18n::Tr("Ré-utiliser l'OBJET automatiquement (le consomme)"),
                            &auto_reuse_item_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Pour les listes ouvertes par un objet — Mini Furnace, marteaux de forge — "
      "dont le script fait « produce N; ».\n\n"
      "/!\\ CHAQUE RELANCE DÉTRUIT UN EXEMPLAIRE. Le serveur supprime l'objet "
      "(pc_delitem, LOG_TYPE_CONSUME) AVANT d'exécuter son script : l'exemplaire "
      "est perdu même si tu annules ensuite, et même si la fabrication rate.\n\n"
      "Comme pour la compétence, ce qui se relance est l'OUVERTURE de la liste, "
      "jamais la fabrication : choisir le produit et déclencher restent des "
      "clics.\n\n"
      "La chaîne s'arrête sur une liste vide, sur un refus du serveur, dès qu'il "
      "ne te reste plus l'objet, ou si tu fermes la fenêtre. Le nombre "
      "d'exemplaires consommés reste affiché pendant toute la chaîne."));

  if (auto_reuse_item_) {
    // ── Pourquoi une case ET un curseur, plutôt que « 0 = illimité » ─────────
    // Encoder l'illimité dans la valeur 0 du curseur obligeait à y afficher un
    // libellé à la place du nombre : le texte débordait sur l'étiquette et la
    // poignée restait collée à gauche, sans rien à régler. Un état qui n'a PAS de
    // valeur ne doit pas occuper une position de curseur — il lui faut sa propre
    // case. Le modèle, lui, garde `0 = illimité` : c'est la présentation qui
    // change, pas la donnée.
    bool unlimited = (auto_reuse_max_ == 0);
    if (ro::RoCheckbox(i18n::Tr("Sans limite (jusqu'à épuisement du stock)"), &unlimited)) {
      // En repassant à une limite, on repart du dernier plafond choisi plutôt
      // que d'un nombre arbitraire : décocher puis recocher ne doit pas effacer
      // un réglage.
      auto_reuse_max_ = unlimited ? 0 : auto_reuse_cap_;
      changed = true;
    }
    if (!unlimited) {
      // ro::RoSliderInt, pas ImGui::SliderInt : même raison que les cases à
      // cocher — le panneau porte le skin RO de bout en bout.
      ImGui::SetNextItemWidth(160.0f);
      if (ro::RoSliderInt("Exemplaires au maximum", &auto_reuse_max_, 1, 50)) {
        auto_reuse_cap_ = auto_reuse_max_;  // mémorisé pour le prochain décochage
        changed = true;
      }
    }
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("Sans limite, la chaîne va jusqu'à épuisement du stock. C'est le "
        "défaut — consommer toute une pile est ton choix.\n\n"
        "Le plafond n'existe que pour le seul cas qu'il couvre vraiment : avoir "
        "oublié ce réglage coché. Il ne protège de rien d'autre.\n\n"
        "Une chaîne illimitée ne peut pas s'emballer : chaque tour exige un "
        "résultat du serveur pour armer le suivant. Si le serveur ignore l'usage "
        "(objet en cooldown, condition non remplie), aucune liste n'arrive et la "
        "chaîne s'arrête d'elle-même."));
  }

  return changed;
}

void MakeItemWindow::DrawHistory() {
  // Le journal est dessiné APRÈS le pied dans TOUS les cas, et jamais depuis une
  // branche d'affichage : le plugin de refine l'avait oublié dans la branche
  // « plus rien à fabriquer », c'est-à-dire au moment précis où l'on veut relire
  // ce qui vient de se passer. Sortir le journal des branches supprime la classe
  // de bug au lieu d'ajouter un troisième appel qu'une quatrième branche
  // oublierait à son tour.
  if (!show_history_ || history_.empty()) return;
  Separator();
  // ⚠ `-FLT_MIN` et non `0` pour la largeur. Dans une fenêtre `AlwaysAutoResize`,
  // un enfant de largeur 0 (« prends ce qui reste ») entre en boucle avec
  // l'auto-dimensionnement : la fenêtre se dimensionne sur l'enfant, qui se
  // dimensionne sur la fenêtre. C'est ce qui faisait osciller la taille d'une
  // frame à l'autre — le journal apparaissait haut puis redescendait. `-FLT_MIN`
  // est la forme définie pour « remplir » sans participer à la mesure.
  if (ImGui::BeginChild("##makelog", ImVec2(-FLT_MIN, 90.0f), true)) {
    for (const LogLine& l : history_) {
      ImGui::PushStyleColor(ImGuiCol_Text, l.color);
      TextWrapped(l.text.c_str());
      ImGui::PopStyleColor();
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f)
      ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
}
