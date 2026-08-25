#include "features/windows/pet_window.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>  // atoi : le client range l'ITID en TEXTE (cf. EggItid)
#include <cstring>

#include "bourgeon.h"
#include "features/item_cell.h"     // FindInfoByIndex : l'ITID de l'œuf du pet
#include "features/link_gesture.h"  // gestes communs des liens (desc / menu / chat)
#include "imgui.h"
#include "ragnarok/globals.h"
#include "ragnarok/msgstring.h"
#include "ragnarok/ui_window_mgr.h"  // ligne de chat (UIM_PUSHINTOCHATHISTORY)
#include "ragnarok/uiwnd.h"
#include "ui/icon_cache.h"  // ro::ItemIcon (icône des matériaux)
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"
#include "utils/log_console.h"  // LogDiag
#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ui/ui_palette.h"  // ro::pal : la palette de l'UI

namespace {

// `ZC_PETEGG_LIST` 0x01A6, à longueur VARIABLE : `[op:2][len:2][index:2] × n`.
// C'est l'unique créateur de la fenêtre 90 (case 422 du dispatch, @0x00CA6539) et
// son handler ne fait rien d'autre que la créer et la remplir : le remplacer ne
// laisse donc aucun devoir orphelin derrière lui.
constexpr uint16_t kOpPetEggList = 0x01a6;

// ── msgstringtable : les libellés EXACTS du client ──────────────────────────
// Règle du projet : on affiche le texte du client, jamais une paraphrase — sans
// quoi la fiche Bourgeon et la native diraient deux choses pour la même erreur.
// (Le msg 591 « pas de nourriture » n'est PAS repris ici : c'est
// `SendMsg(150, 1)` qui l'émet lui-même, dans le chat, au moment où il refuse
// d'envoyer le paquet. Le doubler l'afficherait deux fois.)
constexpr int kMsiFeedPet        = 592;   // 0x250
constexpr int kMsiPerformance    = 593;   // 0x251
constexpr int kMsiReturnToEgg    = 594;   // 0x252
constexpr int kMsiAccessoryOff   = 595;   // 0x253
constexpr int kMsiFeedConfirm    = 601;   // 0x259 « nourrir ? »
constexpr int kMsiNameTooLong    = 682;   // 0x2AA
constexpr int kMsiEvolutionEntry = 2568;  // 0xA08 — gabarit d'une ligne d'évolution
constexpr int kMsiEvolutionLow   = 2576;  // 0xA10 « intimité insuffisante »
constexpr int kMsiAutoFeeding    = 2577;  // 0xA11
constexpr int kMsiNameForbidden  = 2812;  // 0xAFC
constexpr int kMsiRenameConfirm  = 2931;  // 0xB73
// ⚠ 2948, pas 2932 : le commentaire 0xB84 disait déjà 2948, la valeur décimale
// avait glissé. 2932 est `MSI_STORE_ASSISTANT_TRADE_DATE` (« Trade Date: ») — un
// refus de nom se serait affiché comme un en-tête d'échoppe. Cf.
// tools/lang/msgstring_ids.csv, la seule source d'id qui fasse foi.
constexpr int kMsiNameRejected   = 2948;  // 0xB84 MSI_CANNOT_USE_NAME
constexpr int kMsiFeedNoMail     = 2985;  // 0xBA9 « ferme le courrier d'abord »
constexpr int kMsiFeedBlocked    = 3550;  // 0xDDE

// 🔴 Couleur des refus, reprise du natif : `UIWindowMgr_ChatAction(mgr, 1, msg,
// 0xFF, 0)`, soit du BLEU. Ce n'est pas l'ocre « Bourgeon » du DPS meter et du
// menu contextuel, et c'est délibéré — ces phrases-là sont le CLIENT qui parle,
// mot pour mot, pas nous.
constexpr uint32_t kClientRefusalRgb = 0x0000FF;

// Taille de première ouverture. La native fait 280×180 (relevé live) ; on est
// plus large parce qu'on affiche EN PLUS ce qu'elle cachait derrière son menu :
// les cinq commandes, l'accessoire et les évolutions.
constexpr float kSpawnW = 330.0f;
constexpr float kSpawnH = 260.0f;
constexpr float kSpawnX = 1054.0f;  // la position native, relevée live
constexpr float kSpawnY = 369.0f;

// Largeur du champ de renommage. Fixe et non négative : cf. `DrawRenameRow`.
// 23 caractères tiennent largement dedans, et c'est la borne du client.
constexpr float kRenameFieldW = 160.0f;

// Combien de `OnTick` (~100 ms chacun) sans acteur de pet avant de refermer la
// fiche. Une seconde : assez pour couvrir l'aller-retour réseau qui suit un warp
// (les spawns arrivent APRÈS la fin du chargement), assez court pour que la fiche
// ne survive pas visiblement au pet.
constexpr int kPetGoneTicks = 10;

// La liste d'éclosion, à la place où la native s'ouvrait (relevé live).
constexpr float kHatchSpawnX = 760.0f;
constexpr float kHatchSpawnY = 412.0f;
constexpr float kHatchRowW   = 250.0f;

// Le corps d'une fenêtre RO est CLAIR : le texte par défaut d'ImGui y est
// illisible, et `TextDisabled` encore plus.

// Écrit une ligne dans le chat du jeu, par la voie unique de Bourgeon. C'est ce
// que fait le natif de ses trois refus — `UIWindowMgr_ChatAction(mgr, 1, …)` —
// et `UIM_PUSHINTOCHATHISTORY` est la seule commande native sans danger en
// pleine frame ImGui : elle empile une ligne et n'ouvre aucune modale.
void SayToChat(const char* utf8) {
  if (!utf8 || !*utf8) return;
  const char* wire = ro::Utf8ToWireText(utf8);
  if (!wire || !*wire) return;
  UIWindowMgr::SendMsg(UIMessage::UIM_PUSHINTOCHATHISTORY,
                       reinterpret_cast<int>(wire),
                       static_cast<int>(kClientRefusalRgb), 0, 0);
}

// Un libellé de la table du client, prêt pour un widget ImGui : sans code
// couleur (ImGui afficherait « ^ff0000 » tel quel) et sans saut de ligne (le
// client en met dans ce qu'il peint sur des boutons de deux lignes, et un « \n »
// entre dans l'identifiant du widget).
// ⚠ Tampon rotatif sur quatre emplacements : à consommer immédiatement.
// ⚠ `Flatten` plafonne à 127 octets — bon pour un libellé, PAS pour une phrase :
// les messages longs passent par `msgstr::StripColors` seul.
const char* Btn(int msg_id) {
  return msgstr::StripColors(msgstr::Flatten(msgstr::Utf8(msg_id)));
}

// Un libellé du client qui porte un « %s », rempli avec `arg`.
//
// 🔴 PAS de `snprintf(out, template, arg)` : le gabarit vient de
// `msgstringtable.txt`, un fichier de DONNÉES — le passer comme chaîne de format
// laisserait n'importe quel « %d » ou « %n » du fichier lire la pile. On coupe
// donc autour du premier « %s » à la main, et on ignore les suivants.
// Sans « %s » dans le gabarit, il n'a rien à dire de plus que l'argument
// lui-même : on rend l'argument seul plutôt qu'un libellé qui perdrait le nom.
void FormatMsgArg(char* out, size_t out_size, const char* tmpl, const char* arg) {
  const char* slot = (tmpl != nullptr) ? std::strstr(tmpl, "%s") : nullptr;
  if (slot == nullptr) {
    std::snprintf(out, out_size, "%s", arg ? arg : "");
    return;
  }
  std::snprintf(out, out_size, "%.*s%s%s", static_cast<int>(slot - tmpl), tmpl,
                arg ? arg : "", slot + 2);
}

// La jauge de la fiche, calquée sur celle de l'onglet Homoncule : libellé dessiné
// à la main, centré, en blanc ombré — celui d'ImGui suit le bord du remplissage
// et finit illisible sur le fond sombre dès qu'une jauge est à moitié pleine.
void Gauge(const char* label, int cur, int max, const ImVec4& col) {
  const float ratio =
      max > 0 ? std::clamp(static_cast<float>(cur) / static_cast<float>(max), 0.0f, 1.0f)
              : 0.0f;
  char text[96];
  std::snprintf(text, sizeof(text), "%s  %d / %d", label, cur, max);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
  ImGui::ProgressBar(ratio, ImVec2(-1.0f, 15.0f), "");
  ImGui::PopStyleColor();
  const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
  const ImVec2 ts = ImGui::CalcTextSize(text);
  const ImVec2 at((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f - 2.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(ImVec2(at.x + 1.0f, at.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
  dl->AddText(at, IM_COL32(255, 255, 255, 255), text);
}

// ⚠ L'ITID est rangé en TEXTE dans une `std::string` — c'est pour ça que le natif
// fait un `atoi` dessus (`UIPetInfoWnd_OnCreate` @0x00879ad1). `rag::itemlist::ItemId`
// fait exactement ça, sous SEH ; ce fichier en avait sa propre copie.

// Les quatre slots d'un ItemInfo. Sur un ŒUF ce ne sont pas des cartes mais la
// fiche du pet, réécrite par le serveur avant l'envoi (cf. ragnarok/pet.h).
bool ReadInfoCards(void* info, uint32_t* out4) {
  if (!info || !out4) return false;
  __try {
    auto* p = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<uint8_t*>(info) + rag::itemlist::kInfoCards);
    for (int i = 0; i < 4; ++i) out4[i] = p[i];
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Liens d'objet du panneau d'évolution ────────────────────────────────────
// La fenêtre native en avait déjà : les DEUX noms d'espèce renvoyaient à la
// description de l'ŒUF correspondant, et chaque matériau à la sienne. C'est une
// vraie commodité — « il me manque quoi, au juste ? » — et la perdre aurait été
// exactement le genre de régression que « remplacer le natif, c'est combler ses
// manques » interdit.
//
// Le menu s'ouvre HORS de la zone qui l'a demandé (l'identifiant d'un popup se
// hache avec la pile d'ids courante) : on met la cible de côté ici et le popup
// est ouvert en fin de panneau. Même montage que la table des drops de la fiche
// de monstre.
links::MenuAnchor g_link_menu;

// Une CELLULE d'objet cliquable : icône optionnelle + libellé, avec la totale —
// aperçu de description au survol, description au clic gauche, menu contextuel
// au clic droit, lien de chat au Maj+clic.
//
// `itid` est ce que la cellule DÉSIGNE, `shown` ce qu'on LIT. Les deux diffèrent
// pour une espèce, dont on affiche le nom du mob alors que le lien mène à son
// ŒUF — exactement ce que faisait la fenêtre native.
//
// 🔴 Gestes de LIEN, pas de cellule d'inventaire : ces objets ne sont pas dans
// le sac (c'est même le cas intéressant — il en MANQUE), donc le clic gauche n'a
// rien à sélectionner ni à glisser et sert la description (cf. link_gesture.h).
void ItemLinkCell(uint32_t itid, const char* shown, const ImVec4& color,
                  bool with_icon) {
  ImGui::PushID(static_cast<int>(itid));
  // Le libellé de la CIBLE est le nom RÉEL de l'objet, pas le texte affiché :
  // c'est lui qui coiffe le menu et qui part dans le chat, et « MASTERING »
  // n'aurait rien dit de l'œuf qu'on aurait collé.
  const char* real = itemcell::NameById(itid);
  links::LabelOpts opts;
  opts.Color(color);
  if (with_icon) opts.Icon(itid);
  links::Label(links::FromItemId(itid, (real && real[0]) ? real : shown), shown,
               g_link_menu, opts);
  ImGui::PopID();
}

}  // namespace

PetWindow::PetWindow() {
  // 🔴 La liste d'éclosion ne NAÎT PLUS : on prend la place du seul handler qui
  // la crée. Une native seulement masquée garde le CLAVIER, et celle-ci a un
  // bouton par défaut qui ENGAGE l'œuf sélectionné — le même piège que la banque
  // et le cash shop. Le prédicat est relu à chaque paquet : interrupteur éteint,
  // le handler natif reprend la main et la liste classique s'ouvre comme avant.
  Bourgeon::Instance().RegisterReplaceOpcode(kOpPetEggList,
                                             [this] { return imgui_enabled_; });
}

// ── L'œuf porté ─────────────────────────────────────────────────────────────
int PetWindow::EggItid(const rag::pet::State& pet) {
  if (pet.egg_index < 0) return 0;
  return static_cast<int>(
      rag::itemlist::ItemId(itemcell::FindInfoByIndex(rag::kInventoryListAddr,
                                                      pet.egg_index)));
}

// ── Cycle de vie ────────────────────────────────────────────────────────────

void PetWindow::HandleNativeCreation(void* win, int window_id) {
  if (!win || !imgui_enabled_) return;
  __try {
    *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(win) + uiwnd::kOffVisible) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}

  // 🔴 Le menu de commandes (260) et la fenêtre d'évolution (261) ne basculent
  // RIEN : ce n'étaient que des dépendances de la fiche, dont nos boutons et
  // notre panneau prennent la place. Le masquer suffit — ouvrir la fiche ici
  // rouvrirait une fenêtre que le joueur regarde déjà.
  if (window_id == uiwnd::kPetMenuWndId || window_id == uiwnd::kUIPetEvolutionWnd) return;

  // Une création survenue PENDANT un changement de map n'est pas une demande du
  // joueur : c'est l'interface qui se reconstruit. On masque, mais sans toucher
  // à l'état de la fenêtre — sinon elle s'ouvrirait seule à chaque warp.
  if (Bourgeon::Instance().IsMapLoading()) return;

  // ⚠ Ce chemin sert DEUX demandes qu'il ne faut pas confondre :
  //   · le joueur clique « Statut du pet » -> c'est un BASCULEUR, comme toute
  //     entrée du client (« ferme si elle existe, sinon crée ») ; puisque la
  //     native est détruite, elle n'existe jamais et la bascule nous revient ;
  //   · l'intimité tombe sous 100 et `GameMode_PetIntimacyWarnAndOpenInfo`
  //     l'ouvre de FORCE. Là, refermer serait exactement le contraire de ce que
  //     le client veut dire.
  // Rien dans l'appel ne les distingue — mais l'ouverture forcée est un
  // COUP UNIQUE : le client tient une table des pets déjà avertis (hachée sur
  // l'AID, CGameMode+394) et n'y repasse qu'une fois, jusqu'à ce que l'intimité
  // remonte au-dessus de 99, où l'entrée est retirée. On tient la même, sinon
  // le clic « Statut du pet » cesserait de refermer la fiche pendant tout le
  // temps où le pet boude — c'est-à-dire précisément quand on la consulte.
  rag::pet::State pet{};
  // Aucun pet lisible : rien à basculer. Le client ne réclame cette fenêtre que
  // lorsqu'il en a un ; l'ouvrir quand même afficherait une fiche vide que le
  // tick suivant refermerait aussitôt.
  if (!rag::pet::ReadState(&pet)) return;
  const bool low = pet.intimacy < 100 && rag::pet::IntimacyRank(pet.intimacy) == 0;
  if (!low) low_intimacy_warned_aid_ = 0;
  bool forced = false;
  if (low && low_intimacy_warned_aid_ != pet.aid) {
    low_intimacy_warned_aid_ = pet.aid;
    forced = true;
  }
  if (open_ && !forced) { open_ = false; return; }
  Open();
}

void PetWindow::Open() {
  open_ = true;
  need_focus_ = true;
  status_[0] = '\0';
  status_error_ = false;
  confirm_feed_ = false;
  confirm_egg_ask_ = false;
  rename_edit_ = false;
  // Le panneau d'évolution se replie : rouvrir la fiche ne doit pas la rendre
  // dépliée sur un choix fait il y a une heure.
  evo_target_ = 0;
}

void PetWindow::OnTick() {
  if (!imgui_enabled_) {
    // Retour au natif : nos fenêtres s'effacent et le client recrée les siennes à
    // la prochaine demande (elles n'existent plus). Rien à restaurer — et la
    // file réseau est vidée, ses paquets appartenaient à l'autre régime.
    open_ = false;
    hatch_open_ = false;
    net_inbox_.Clear();
    return;
  }
  // ⚠ Le compteur d'absence repart de zéro tant que la map charge : c'est
  // précisément le moment où la liste d'acteurs est vide sans que le pet soit
  // parti (cf. plus bas).
  if (Bourgeon::Instance().IsMapLoading()) { pet_gone_ticks_ = 0; return; }
  // 🔴 DÉTRUIRE, pas masquer : `GameMode_PetIntimacyWarnAndOpenInfo` rouvre la 88
  // de force, et une native seulement masquée garde le clavier. Ce filet couvre
  // aussi la bascule de mode alors qu'une des deux est déjà à l'écran.
  if (uiwnd::FindWindow(uiwnd::kUIPetInfoWnd)) {
    __try { uiwnd::CloseWindow(uiwnd::kUIPetInfoWnd); } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
  if (uiwnd::FindWindow(uiwnd::kPetMenuWndId)) {
    __try { uiwnd::CloseWindow(uiwnd::kPetMenuWndId); } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
  // La fenêtre d'évolution (261) : notre panneau la remplace. Elle ne devrait
  // même plus naître — son unique créateur était le msg 39/471 de la fiche, qui
  // n'existe plus — mais la bascule de mode en plein milieu la laisserait à
  // l'écran, avec ses deux boutons coréens indéboguables (docs §7.4).
  if (uiwnd::FindWindow(uiwnd::kUIPetEvolutionWnd)) {
    __try {
      uiwnd::CloseWindow(uiwnd::kUIPetEvolutionWnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
  }
  // La liste d'éclosion (90) : son paquet créateur nous revient, elle ne devrait
  // donc plus naître. Le filet couvre la bascule d'interrupteur alors qu'elle est
  // déjà à l'écran — sinon elle resterait là, avec son clavier.
  if (uiwnd::FindWindow(uiwnd::kUIPetEggListWnd)) {
    __try { uiwnd::CloseWindow(uiwnd::kUIPetEggListWnd); } __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
  // ── Le pet est parti (retour à l'œuf, intimité tombée à zéro, capture) ────
  // 🔴 AUCUN paquet ne le dit. `pet_return_egg` envoie bien `ZC_CHANGESTATE_PET`
  // sous-type 6, mais avec `data = (intimate == 1) ? 0 : 1` — donc **1** pour un
  // pet d'intimité normale, et la branche 1 du client ne fait quelque chose QUE
  // si la fenêtre 90 est ouverte. Rien n'est réinitialisé : ni l'AID, ni la
  // classe. Le sous-type 6 est de surcroît envoyé À L'ÉCLOSION aussi — il ne veut
  // donc même pas dire « parti ». C'est pour ça que la fiche NATIVE reste elle
  // aussi ouverte sur un pet absent ; on ne reproduit pas ce défaut-là
  // (cf. [[feedback_replace_native_fill_gaps]]).
  //
  // Le seul observable est la disparition de l'ACTEUR (`unit_free` CLR_OUTSIGHT).
  // ⚠ Mais un changement de map vide la liste d'acteurs le temps d'un aller-retour
  // réseau, et le chargement se termine AVANT que les spawns n'arrivent : une
  // absence d'un seul tick ne prouve donc rien. On exige une absence CONTINUE —
  // le compteur est remis à zéro par le chargement de map juste au-dessus.
  const bool present = rag::pet::Present();
  if (present) pet_gone_ticks_ = 0;
  else if (pet_gone_ticks_ < kPetGoneTicks) ++pet_gone_ticks_;
  if (open_ && pet_gone_ticks_ >= kPetGoneTicks) open_ = false;

  // …et à l'inverse, un pet SORTI veut dire que l'éclosion a abouti : la liste
  // n'a plus rien à proposer (`@hatch` exige de toute façon `pet_id <= 0`).
  // Immédiat, celui-là : la liste n'est jamais ouverte à cheval sur un warp.
  if (hatch_open_ && present) hatch_open_ = false;
}

// ── La liste d'éclosion : le paquet ─────────────────────────────────────────

void PetWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // 🔴 Fil RÉSEAU : on COPIE, on ne décode rien. Le handler natif remplacé
  // n'écrivait aucune globale — il ne faisait que créer et remplir sa fenêtre —
  // donc il n'y a rien à faire ici tout de suite (cf. features/net_inbox.h).
  // `PushAnnounced` : paquet à longueur VARIABLE, `data` commence sur le champ
  // longueur et le dispatcher n'en transmet que le début.
  if (opcode != kOpPetEggList || !imgui_enabled_) return;
  net_inbox_.PushAnnounced(opcode, data, len);
}

void PetWindow::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode != kOpPetEggList || !imgui_enabled_) return;
  if (!data || len < 2) return;
  // `[len:2][index:2] × n`, l'opcode déjà consommé. La longueur annoncée COMPTE
  // l'opcode : le corps fait donc `annoncé - 4` octets, jamais plus que ce que la
  // file a réellement copié.
  const uint16_t announced = *reinterpret_cast<const uint16_t*>(data);
  int body = (announced >= 4) ? static_cast<int>(announced) - 4 : 0;
  if (body > static_cast<int>(len) - 2) body = static_cast<int>(len) - 2;
  int count = (body > 0) ? body / 2 : 0;
  if (count > kMaxEggs) count = kMaxEggs;

  hatch_count_ = 0;
  for (int i = 0; i < count; ++i) {
    // Index d'inventaire CLIENT (le serveur envoie `client_index(i)` = i+2), le
    // même que celui de `ItemInfo+0x04` — c'est lui qu'on renverra tel quel.
    const int idx = *reinterpret_cast<const int16_t*>(data + 2 + i * 2);
    if (idx < 0) continue;
    hatch_index_[hatch_count_++] = idx;
  }
  hatch_sel_ = -1;
  hatch_open_ = true;
  hatch_focus_ = true;
}

void PetWindow::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
  open_ = false;
  pending_ = Pending::kNone;
  rename_edit_ = false;
  confirm_feed_ = confirm_egg_ask_ = false;
  evo_target_ = 0;
  // La liste d'éclosion appartient à une session de `@hatch` que le changement de
  // mode a terminée : la garder proposerait des index d'un autre inventaire.
  hatch_open_ = false;
  hatch_count_ = 0;
  hatch_sel_ = -1;
  pet_gone_ticks_ = 0;
  net_inbox_.Clear();
  status_[0] = '\0';
  status_error_ = false;
  sprite_ = ro::MobSpriteRes{};
  evo_src_sprite_ = ro::MobSpriteRes{};
  evo_dst_sprite_ = ro::MobSpriteRes{};
}

// ── Rejeu hors frame ImGui ──────────────────────────────────────────────────

void PetWindow::FlushPending() {
  const Pending what = pending_;
  if (what == Pending::kNone) return;
  pending_ = Pending::kNone;

  switch (what) {
    case Pending::kCommand:
      // 🔴 C'est `SendMsg(150, cmd)` qui porte le gate « pas de nourriture » :
      // sur `cmd == 1` sans nourriture en sac, il affiche le msg 591 et n'envoie
      // RIEN. On ne le double pas — le doubler afficherait deux fois le refus.
      if (!rag::pet::SendCommand(pending_arg_))
        LogDiag("[PetWindow] commande pet {} injouable", pending_arg_);
      return;
    case Pending::kRename:
      if (!rag::pet::SendRename(pending_name_))
        LogDiag("[PetWindow] renommage refuse");
      return;
    case Pending::kEvolution:
      // ⚠ Contrairement aux autres, celui-ci n'ouvre plus rien : la fenêtre 261
      // est remplacée par notre panneau, donc l'évolution part directement en
      // `CZ_PET_EVOLUTION`. Il reste différé pour une seule raison — le refus du
      // serveur revient par ZC 0x09FC, dont le handler NATIF écrit dans le chat.
      if (!rag::pet::SendEvolution(pending_arg_))
        LogDiag("[PetWindow] evolution non envoyee (cible {})", pending_arg_);
      return;
    case Pending::kSelectEgg:
      // Le natif n'en faisait pas davantage : msg 6 / id 184 -> SendMsg(146,
      // index). Différé comme les autres — `SendMsg` peut ouvrir une native.
      if (!rag::pet::SendSelectEgg(pending_arg_))
        LogDiag("[PetWindow] eclosion non envoyee (index {})", pending_arg_);
      return;
    case Pending::kNone:
      return;
  }
}

// ── Rendu ───────────────────────────────────────────────────────────────────

void PetWindow::OnRenderUI() {
  if (!imgui_enabled_) return;
  // La liste d'éclosion vit sa vie : elle apparaît justement quand on n'a PAS de
  // pet, donc quand la fiche n'a rien à montrer.
  DrawHatchWindow();
  if (!open_) return;
  // L'acteur du pet n'est plus là : on ne dessine pas une fiche sur un fantôme,
  // et le retour à l'œuf se voit donc tout de suite.
  // ⚠ On ne FERME pas pour autant : c'est `OnTick` qui tranche, après une absence
  // CONFIRMÉE. Un warp fait disparaître l'acteur le temps d'un aller-retour, et
  // une fiche refermée pour ça ne se rouvrirait pas toute seule.
  if (!rag::pet::Present()) return;

  rag::pet::State pet{};
  if (!rag::pet::ReadState(&pet)) { open_ = false; return; }

  if (need_focus_) { ImGui::SetNextWindowFocus(); need_focus_ = false; }
  if (need_pos_) {
    // FirstUseEver : simple DÉFAUT de première ouverture, repris de la position
    // native relevée live, rabattu dans l'écran sur petite résolution.
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(std::min(kSpawnX, std::max(0.0f, screen.x - kSpawnW)),
               std::min(kSpawnY, std::max(0.0f, screen.y - kSpawnH))),
        ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  // 🔴 Ni taille imposée ni poignée de redimensionnement : le contenu varie de
  // beaucoup (confirmations, liste d'évolutions, panneau d'évolution déplié) et
  // une taille FIXE tronquerait le cas long ou laisserait un grand vide dans le
  // cas court. `AlwaysAutoResize` colle à ce qui est dessiné, comme la native —
  // qui n'était pas redimensionnable non plus.

  // Le titre porte le NOM du pet, comme la native. L'id ### fige l'identité
  // ImGui : renommer le pet ne doit pas réinitialiser position et taille.
  char title[96];
  const char* shown = pet.name[0] ? ro::WireToUtf8(pet.name) : i18n::Tr("Pet");
  std::snprintf(title, sizeof(title), "%s###petwnd", shown);

  const ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_AlwaysAutoResize;
  if (ro::BeginRoWindow(title, &open_, kFlags)) {
    DrawHeader(pet);
    ImGui::Separator();
    DrawGauges(pet);
    ImGui::Separator();
    DrawActions(pet);
    if (status_[0]) {
      ImGui::PushStyleColor(ImGuiCol_Text, status_error_ ? ro::pal::kRed : ro::pal::kLabel);
      ImGui::TextWrapped("%s", status_);
      ImGui::PopStyleColor();
    }
  }
  ro::EndRoWindow();
}

void PetWindow::DrawHeader(const rag::pet::State& pet) {
  // Sprite du pet plutôt que l'illustration native (`illust\pet_noimage.bmp` en
  // repli) : c'est la créature telle qu'elle est dans le monde, accessoire
  // compris, et Bourgeon sait déjà la dessiner.
  if (sprite_.class_id != pet.cls) {
    sprite_ = ro::MobSpriteRes{};
    ro::LoadMobSprite(pet.cls, &sprite_);
  }
  const float kBox = 64.0f;
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + kBox, p0.y + kBox);
  ro::DrawMobSprite(ImGui::GetWindowDrawList(), sprite_, p0, p1,
                    animate_ ? static_cast<float>(ImGui::GetTime()) : 0.0f);
  ImGui::Dummy(ImVec2(kBox, kBox));
  ImGui::SameLine();

  ImGui::BeginGroup();
  DrawRenameRow(pet);
  const char* species = rag::pet::MobDisplayNameUtf8(pet.cls);
  ImGui::TextColored(ro::pal::kLabel, "%s  ·  %s %d",
                     (species && species[0]) ? species : "?",
                     i18n::Tr("niv."), pet.level);
  // L'accessoire : le natif n'en disait jamais rien, il proposait seulement de
  // le retirer — y compris quand il n'y en avait pas.
  if (pet.accessory != 0) {
    const char* acc = itemcell::NameById(static_cast<uint32_t>(pet.accessory));
    ImGui::TextColored(ro::pal::kValue, "%s : %s", i18n::Tr("Accessoire"),
                       (acc && acc[0]) ? acc : "?");
  } else {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucun accessoire."));
  }
  ImGui::EndGroup();
}

void PetWindow::DrawRenameRow(const rag::pet::State& pet) {
  if (rename_edit_) {
    // 🔴 Largeur FIXE, jamais négative. Sous `AlwaysAutoResize`, une largeur
    // relative (« -70 » = tout sauf 70 px) est une boucle de rétroaction : l'item
    // se dimensionne sur la place disponible, la fenêtre se dimensionne sur son
    // contenu, et comme les deux boutons qui suivent s'ajoutent à chaque tour, la
    // fenêtre grandit d'une frame à l'autre jusqu'à remplir l'écran — puis
    // redescend lentement à la réouverture, ce qui la trahit.
    ImGui::SetNextItemWidth(kRenameFieldW);
    ImGui::InputText("##pet_name", rename_buf_, sizeof(rename_buf_));
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Valider"), 62.0f, 0.0f) && rename_buf_[0]) {
      // 🔴 Les gardes du msg 6/184, dans le MÊME ordre que le natif. Le nom part
      // en encodage RÉSEAU : c'est là-dessus que portent les deux tests du
      // client, pas sur notre UTF-8.
      const char* wire = ro::Utf8ToWire(rename_buf_);
      if (!wire) wire = "";
      if (std::strlen(wire) >= rag::pet::kNameMaxBytes) {
        std::snprintf(status_, sizeof(status_), "%s",
                      msgstr::StripColors(msgstr::Utf8(kMsiNameTooLong)));
        status_error_ = true;
      } else if (rag::pet::NameHasForbiddenWord(wire)) {
        std::snprintf(status_, sizeof(status_), "%s",
                      msgstr::StripColors(msgstr::Utf8(kMsiNameForbidden)));
        status_error_ = true;
      } else if (rag::pet::NameHasBannedWord(wire)) {
        std::snprintf(status_, sizeof(status_), "%s",
                      msgstr::StripColors(msgstr::Utf8(kMsiNameRejected)));
        status_error_ = true;
      } else {
        std::snprintf(pending_name_, sizeof(pending_name_), "%s", wire);
        pending_ = Pending::kRename;
        std::snprintf(status_, sizeof(status_), "%s", i18n::Tr("Renommage envoyé."));
        status_error_ = false;
        rename_edit_ = false;
      }
    }
    ImGui::SameLine();
    // 🔴 Suffixe `##` obligatoire : « Annuler » apparaît AUSSI dans les deux
    // confirmations plus bas, et rien n'empêche qu'elles soient à l'écran en même
    // temps. Deux boutons de même libellé partageraient leur identifiant ImGui.
    char cancel[48];
    std::snprintf(cancel, sizeof(cancel), "%s##pet_rename_cancel", i18n::Tr("Annuler"));
    if (ro::RoSmallButton(cancel, 62.0f, 0.0f)) rename_edit_ = false;
    // Le natif demandait confirmation par une boîte modale (msg 2931) ; ici
    // l'avertissement est PERMANENT sous le champ, ce qui vaut mieux qu'une boîte
    // qu'on valide sans lire : le renommage est définitif.
    // ⚠ Le msg 2931 est un GABARIT — « Pet name will be changed to %s » — que le
    // natif remplissait avec le contenu de son champ. Affiché brut, il montrait
    // « %s » au joueur ; ici il suit la saisie, lettre à lettre.
    char confirm[224];
    FormatMsgArg(confirm, sizeof(confirm),
                 msgstr::StripColors(msgstr::Utf8(kMsiRenameConfirm)), rename_buf_);
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kRed);
    ImGui::TextWrapped("%s", confirm);
    ImGui::PopStyleColor();
    return;
  }

  const char* shown = pet.name[0] ? ro::WireToUtf8(pet.name) : "?";
  if (pet.renameable) {
    // Le NOM EST le bouton, tant que le serveur accepte encore. Le suffixe ##
    // fige l'identifiant ImGui, que le libellé ferait sinon changer avec le nom.
    char btn[80];
    std::snprintf(btn, sizeof(btn), "%s##pet_rename", shown);
    if (ro::RoSmallButton(btn, 0.0f, 0.0f)) {
      std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", shown);
      rename_edit_ = true;
    }
    mui::Tooltip(i18n::Tr("Cliquer pour renommer — une seule fois, définitivement."));
  } else {
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ro::pal::kValue, "%s", shown);
    // 🔴 Ce que le natif ne disait pas : il gardait son bouton « rewrite »
    // cliquable une fois le droit consommé, et le serveur jetait en silence.
    mui::Tooltip(i18n::Tr("Ce pet a déjà été renommé : le serveur n'accepte qu'une fois."));
  }
}

void PetWindow::DrawGauges(const rag::pet::State& pet) {
  // Faim : maximum 100, comme le client et le serveur. Le seuil de couleur suit
  // le palier 0 de `Pet_GetHungerRank` (0..10), celui à partir duquel le pet
  // commence à perdre de l'intimité.
  Gauge(i18n::Tr("Faim"), pet.hunger, 100,
        rag::pet::HungerRank(pet.hunger) == 0 ? ImVec4(0.75f, 0.20f, 0.20f, 1.0f)
                                              : ImVec4(0.35f, 0.60f, 0.25f, 1.0f));
  // 🔴 Ce que le natif ne montrait pas : le SENS de la variation. Le client garde
  // la faim précédente (0x015FB3F4) sans jamais l'afficher — or c'est elle qui
  // dit si le pet est en train de descendre.
  if (pet.prev_hunger != pet.hunger) {
    const bool down = pet.hunger < pet.prev_hunger;
    ImGui::TextColored(down ? ro::pal::kRed : ro::pal::kLabel, "%s %d",
                       down ? i18n::Tr("en baisse, précédemment")
                            : i18n::Tr("en hausse, précédemment"),
                       pet.prev_hunger);
  }

  Gauge(i18n::Tr("Intimité"), pet.intimacy, 1000, ImVec4(0.55f, 0.35f, 0.55f, 1.0f));
  ImGui::TextColored(ro::pal::kValue, "%s",
                     Btn(rag::pet::IntimacyMsgId(pet.intimacy, pet.cls)));
}

void PetWindow::DrawActions(const rag::pet::State& pet) {
  const float w = 96.0f;

  // ── Nourrir ───────────────────────────────────────────────────────────────
  // 🔴 Première des deux gardes natives : la rédaction de courrier ouverte
  // refuse AVANT la confirmation (msg 39 / id 467).
  if (ro::RoSmallButton(Btn(kMsiFeedPet), w, 0.0f)) {
    if (rag::pet::MailWriteOpen()) {
      SayToChat(msgstr::StripColors(msgstr::Utf8(kMsiFeedNoMail)));
    } else {
      confirm_feed_ = true;
    }
  }
  // ⚠ PAS le msg 591 en infobulle : c'est un REFUS, pas une explication du
  // bouton — l'afficher en permanence dirait « tu n'as pas de nourriture » même
  // quand le sac en est plein. Le client l'émet lui-même, dans le chat, au
  // moment où il refuse.
  mui::Tooltip(i18n::Tr(
      "Nourrir un pet qui n'a pas faim lui fait PERDRE de l'intimité. "
      "Sans nourriture en sac, le client refuse sans rien envoyer."));
  ImGui::SameLine();
  if (ro::RoSmallButton(Btn(kMsiPerformance), w, 0.0f)) {
    pending_ = Pending::kCommand;
    pending_arg_ = rag::pet::kCmdPerform;
  }
  ImGui::SameLine();
  // 🔴 Ce que le natif ne faisait pas : il proposait « Unequip Accessory » même
  // sans accessoire (relevé live). Grisé, avec la raison.
  ImGui::BeginDisabled(pet.accessory == 0);
  if (ro::RoSmallButton(Btn(kMsiAccessoryOff), w, 0.0f)) {
    pending_ = Pending::kCommand;
    pending_arg_ = rag::pet::kCmdAccessoryOff;
  }
  ImGui::EndDisabled();
  if (pet.accessory == 0) mui::Tooltip(i18n::Tr("Ce pet ne porte aucun accessoire."));

  if (confirm_feed_) {
    // Corps de fenêtre RO = fond CLAIR : le texte par défaut d'ImGui y est
    // illisible, il faut donc pousser la couleur sombre à chaque bloc.
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kValue);
    ImGui::TextWrapped("%s", msgstr::StripColors(msgstr::Utf8(kMsiFeedConfirm)));
    ImGui::PopStyleColor();
    char ok[48], cancel[48];
    std::snprintf(ok, sizeof(ok), "%s##pet_feed_ok", i18n::Tr("Confirmer"));
    std::snprintf(cancel, sizeof(cancel), "%s##pet_feed_cancel", i18n::Tr("Annuler"));
    if (ro::RoSmallButton(ok, 80.0f, 0.0f)) {
      confirm_feed_ = false;
      // 🔴 Seconde garde native, à la CONFIRMATION et pas avant (msg 6 / id 488).
      if (rag::pet::FeedBlockerWindowOpen()) {
        SayToChat(msgstr::StripColors(msgstr::Utf8(kMsiFeedBlocked)));
      } else {
        pending_ = Pending::kCommand;
        pending_arg_ = rag::pet::kCmdFeed;
      }
    }
    ImGui::SameLine();
    if (ro::RoSmallButton(cancel, 80.0f, 0.0f)) confirm_feed_ = false;
  }

  // ── Remettre dans l'œuf ───────────────────────────────────────────────────
  ImGui::Spacing();
  if (ro::RoSmallButton(Btn(kMsiReturnToEgg), 2.0f * w, 0.0f)) {
    if (confirm_egg_) confirm_egg_ask_ = true;
    else { pending_ = Pending::kCommand; pending_arg_ = rag::pet::kCmdToEgg; }
  }
  if (confirm_egg_ask_) {
    ImGui::TextColored(ro::pal::kValue, "%s", i18n::Tr("Ranger le pet dans son œuf ?"));
    char cancel[48];
    std::snprintf(cancel, sizeof(cancel), "%s##pet_egg_cancel", i18n::Tr("Annuler"));
    if (ro::RoSmallButton(i18n::Tr("Ranger"), 80.0f, 0.0f)) {
      confirm_egg_ask_ = false;
      pending_ = Pending::kCommand;
      pending_arg_ = rag::pet::kCmdToEgg;
    }
    ImGui::SameLine();
    if (ro::RoSmallButton(cancel, 80.0f, 0.0f)) confirm_egg_ask_ = false;
  }

  // ── Auto-alimentation ─────────────────────────────────────────────────────
  // 🔴 Le bouton natif n'existe QUE si l'œuf figure dans `AutoFeedingPetList` —
  // pas s'il a une recette d'évolution (docs §8.1). On garde ce droit, mais on
  // arrête de faire disparaître la ligne sans un mot.
  const int egg = EggItid(pet);
  if (egg != 0 && rag::pet::EggAutoFeedListed(egg)) {
    bool on = pet.auto_feed;
    if (ro::RoCheckbox(Btn(kMsiAutoFeeding), &on)) rag::pet::SendAutoFeed(on);
    mui::Tooltip(i18n::Tr(
        "Le serveur nourrit le pet tout seul sous un certain seuil de faim. "
        "L'état affiché est celui que le SERVEUR confirme, pas la case cochée."));
  }

  // ── Évolution ─────────────────────────────────────────────────────────────
  // 🔴 Le natif faisait DISPARAÎTRE ces lignes dans trois cas indiscernables :
  // intimité trop basse, œuf inconnu du client, ou aucune recette. Ici la
  // section reste, et dit lequel des trois.
  ImGui::Spacing();
  if (pet.intimacy <= rag::pet::kEvolutionMinIntimacy) {
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kLabel);
    ImGui::TextWrapped("%s", msgstr::StripColors(msgstr::Utf8(kMsiEvolutionLow)));
    ImGui::PopStyleColor();
    return;
  }
  if (egg == 0) {
    // Le cas ORDINAIRE pet sorti : `g_Own_PetEggInvIndex` vaut -1, le client ne
    // sait pas de quel œuf ce pet est sorti. Le natif se taisait.
    ImGui::TextColored(ro::pal::kLabel, "%s",
                       i18n::Tr("Évolution : le client ne connaît pas l'œuf de ce pet."));
    return;
  }
  int targets[rag::pet::kMaxEvolutions] = {};
  const int count = rag::pet::EvolutionTargets(egg, targets, rag::pet::kMaxEvolutions);
  if (count == 0) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucune évolution pour ce pet."));
    return;
  }
  for (int i = 0; i < count; ++i) {
    // Le msg 2568 est un GABARIT — « Evolution - %s » — que le natif remplit
    // avec le nom du mob cible, jamais avec celui de l'œuf. On le remplit pareil,
    // sans jamais le passer comme chaîne de format (cf. FormatMsgArg).
    const int mob = rag::pet::EggItidToMobClass(targets[i]);
    const char* mob_name = rag::pet::MobDisplayNameUtf8(mob);
    char text[128];
    FormatMsgArg(text, sizeof(text), Btn(kMsiEvolutionEntry),
                 (mob_name && mob_name[0]) ? mob_name : "?");
    char label[160];
    std::snprintf(label, sizeof(label), "%s##pet_evo%d", text, i);
    // Bascule : recliquer la même cible replie le panneau.
    if (ro::RoSmallButton(label, 0.0f, 0.0f))
      evo_target_ = (evo_target_ == targets[i]) ? 0 : targets[i];
  }
  if (evo_target_ != 0) DrawEvolutionPanel(pet, egg);
}

// ── Le panneau d'évolution (remplace UIPetEvolutionWnd, id 261) ─────────────
void PetWindow::DrawEvolutionPanel(const rag::pet::State& pet, int src_egg) {
  rag::pet::EvolutionMaterial mats[rag::pet::kMaxEvolutionMaterials] = {};
  const int nmat = rag::pet::EvolutionMaterials(src_egg, evo_target_, mats,
                                                rag::pet::kMaxEvolutionMaterials);

  ImGui::Separator();

  // ── Les deux espèces, sprite à sprite ────────────────────────────────────
  const int dst_mob = rag::pet::EggItidToMobClass(evo_target_);
  if (evo_src_sprite_.class_id != pet.cls) {
    evo_src_sprite_ = ro::MobSpriteRes{};
    ro::LoadMobSprite(pet.cls, &evo_src_sprite_);
  }
  if (evo_dst_sprite_.class_id != dst_mob) {
    evo_dst_sprite_ = ro::MobSpriteRes{};
    ro::LoadMobSprite(dst_mob, &evo_dst_sprite_);
  }
  const float kBox = 72.0f;
  const float anim = animate_ ? static_cast<float>(ImGui::GetTime()) : 0.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  ImVec2 p = ImGui::GetCursorScreenPos();
  ro::DrawMobSprite(dl, evo_src_sprite_, p, ImVec2(p.x + kBox, p.y + kBox), anim);
  ImGui::Dummy(ImVec2(kBox, kBox));
  ImGui::SameLine();
  // La flèche du natif était un bitmap ; ici deux traits suffisent.
  // ⚠ Couleur EXPLICITE, pas `GetColorU32(ImGuiCol_Text)` : le corps d'une
  // fenêtre RO est CLAIR, et la couleur de texte par défaut d'ImGui y disparaît.
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float ay = a.y + kBox * 0.5f;
  const ImU32 arrow = ImGui::GetColorU32(ro::pal::kValue);
  dl->AddLine(ImVec2(a.x + 4.0f, ay), ImVec2(a.x + 26.0f, ay), arrow, 2.0f);
  dl->AddTriangleFilled(ImVec2(a.x + 32.0f, ay), ImVec2(a.x + 22.0f, ay - 6.0f),
                        ImVec2(a.x + 22.0f, ay + 6.0f), arrow);
  ImGui::Dummy(ImVec2(36.0f, kBox));
  ImGui::SameLine();
  p = ImGui::GetCursorScreenPos();
  ro::DrawMobSprite(dl, evo_dst_sprite_, p, ImVec2(p.x + kBox, p.y + kBox), anim);
  ImGui::Dummy(ImVec2(kBox, kBox));

  // Les deux noms d'espèce sont des LIENS vers l'ŒUF correspondant — c'est ce
  // que faisait la native, et c'est la seule façon de savoir ce qu'on vise.
  // ⚠ `MobDisplayNameUtf8` rend un tampon ROTATIF : on recopie le premier avant
  // de demander le second.
  const char* src_name = rag::pet::MobDisplayNameUtf8(pet.cls);
  char src_copy[64];
  std::snprintf(src_copy, sizeof(src_copy), "%s",
                (src_name && src_name[0]) ? src_name : "?");
  ItemLinkCell(static_cast<uint32_t>(src_egg), src_copy, links::LinkColor(), false);
  ImGui::SameLine();
  ImGui::TextColored(ro::pal::kValue, "  →  ");
  ImGui::SameLine();
  const char* dst_name = rag::pet::MobDisplayNameUtf8(dst_mob);
  ItemLinkCell(static_cast<uint32_t>(evo_target_),
               (dst_name && dst_name[0]) ? dst_name : "?", links::LinkColor(), false);

  // ── Les matériaux ────────────────────────────────────────────────────────
  // 🔴 Ce que la fenêtre native ne disait pas : elle listait « Yggdrasil Leaf
  // 10ea » sans jamais dire combien on en avait. Le joueur cliquait, et le
  // serveur répondait FAIL_MATERIAL sans préciser lequel manquait.
  bool have_all = true;
  if (nmat == 0) {
    ImGui::TextColored(ro::pal::kLabel, "%s", i18n::Tr("Aucun matériau connu du client."));
  } else {
    ImGui::TextColored(ro::pal::kValue, "%s", i18n::Tr("Matériaux requis :"));
    for (int i = 0; i < nmat; ++i) {
      const uint32_t itid = static_cast<uint32_t>(mats[i].itid);
      const int have = InventoryCount(itid);
      const bool ok = have >= mats[i].amount;
      if (!ok) have_all = false;
      const char* nm = itemcell::NameById(itid);
      char label[160];
      std::snprintf(label, sizeof(label), "%s  %d / %d",
                    (nm && nm[0]) ? nm : "?", have, mats[i].amount);
      // Manquant = ROUGE, et le rouge l'emporte sur le bleu de lien : savoir ce
      // qui bloque prime sur signaler que c'est cliquable — le soulignement au
      // survol et le curseur main le disent déjà.
      ImGui::Indent(8.0f);
      ItemLinkCell(itid, label, ok ? links::LinkColor() : ro::pal::kRed, /*with_icon=*/true);
      ImGui::Unindent(8.0f);
    }
  }

  // ── Les refus, DITS avant le clic ────────────────────────────────────────
  // Le natif laissait cliquer et se faisait renvoyer un code que le client
  // traduit par un message d'intimité — y compris quand la vraie cause était
  // l'accessoire (docs §7.3). Les trois causes sont ici, à leur seuil SERVEUR.
  const char* refusal = nullptr;
  if (pet.intimacy < rag::pet::kEvolutionServerMinIntimacy)
    refusal = i18n::Tr("Le serveur exige 910 d'intimité, pas 901.");
  else if (pet.accessory != 0)
    refusal = i18n::Tr("Le pet doit retirer son accessoire avant d'évoluer.");
  else if (!have_all)
    refusal = i18n::Tr("Il manque des matériaux.");

  if (refusal != nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kRed);
    ImGui::TextWrapped("%s", refusal);
    ImGui::PopStyleColor();
  }

  char ok_label[48], cancel_label[48];
  std::snprintf(ok_label, sizeof(ok_label), "%s##pet_evo_ok", i18n::Tr("Faire évoluer"));
  std::snprintf(cancel_label, sizeof(cancel_label), "%s##pet_evo_cancel",
                i18n::Tr("Annuler"));
  ImGui::BeginDisabled(refusal != nullptr);
  if (ro::RoSmallButton(ok_label, 110.0f, 0.0f)) {
    pending_ = Pending::kEvolution;
    pending_arg_ = evo_target_;
    evo_target_ = 0;
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ro::RoSmallButton(cancel_label, 80.0f, 0.0f)) evo_target_ = 0;

  // 🔴 Le menu des liens s'ouvre ICI, hors des cellules qui l'ont demandé :
  // l'identifiant d'un popup se hache avec la pile d'ids courante, et un
  // `OpenPopup` posé sous un `PushID(itid)` ne serait pas retrouvé par un
  // `DrawMenu` appelé ailleurs.
  g_link_menu.Draw("##pet_link_menu");
}

// ── La liste d'éclosion (remplace UIPetEggListWnd, id 90) ───────────────────
void PetWindow::DrawHatchWindow() {
  if (!hatch_open_) return;

  if (hatch_focus_) { ImGui::SetNextWindowFocus(); hatch_focus_ = false; }
  if (hatch_need_pos_) {
    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(
        ImVec2(std::min(kHatchSpawnX, std::max(0.0f, screen.x - kHatchRowW)),
               std::min(kHatchSpawnY, std::max(0.0f, screen.y - 200.0f))),
        ImGuiCond_FirstUseEver);
    hatch_need_pos_ = false;
  }

  char title[96];
  std::snprintf(title, sizeof(title), "%s###pethatch", i18n::Tr("Éclore un œuf"));
  const ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_AlwaysAutoResize;
  bool blank_selected = false;
  bool missing_selected = false;
  if (ro::BeginRoWindow(title, &hatch_open_, kFlags)) {
    if (hatch_count_ == 0) {
      // Le serveur envoie la liste même vide (`clif_sendegg` n'a pas de sortie
      // anticipée) : la native affichait alors un cadre muet.
      ImGui::TextColored(ro::pal::kLabel, "%s",
                         i18n::Tr("Aucun œuf de familier dans le sac."));
    }
    for (int i = 0; i < hatch_count_; ++i) {
      const int idx = hatch_index_[i];
      void* info = itemcell::FindInfoByIndex(rag::kInventoryListAddr, idx);
      const uint32_t itid = rag::itemlist::ItemId(info);
      uint32_t cards[4] = {};
      ReadInfoCards(info, cards);
      rag::pet::EggCards egg{};
      rag::pet::DecodeEggCards(cards, 4, &egg);

      ImGui::PushID(i);
      const float side = ImGui::GetTextLineHeight();
      const ro::IconTex ic = ro::ItemIcon(itid, 1);
      if (ic.tex) ImGui::Image(TexId(ic.tex), ImVec2(side, side));
      else        ImGui::Dummy(ImVec2(side, side));
      ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);

      const char* raw = itemcell::NameById(itid);
      const char* nm = (raw && raw[0]) ? raw : "?";
      char label[160];
      std::snprintf(label, sizeof(label), "%s##egg", nm);
      // 🔴 CELLULE, pas lien : le clic gauche a déjà un métier ici — choisir
      // l'œuf. La description passe donc par le survol et par le clic DROIT
      // (cf. [[feedback_right_click_opens_description]]).
      ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kValue);
      if (ImGui::Selectable(label, hatch_sel_ == idx, 0,
                            ImVec2(kHatchRowW - side, 0.0f)))
        hatch_sel_ = idx;
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered() && itid != 0) {
        const links::Target target = links::FromItemId(itid, nm);
        links::HoverPreview(target);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
          links::OpenDescription(target);  // ARME, joué hors frame
      }

      // La seule chose que la native ne disait pas, et qui décide de tout.
      ImGui::Indent(side + 4.0f);
      if (info == nullptr) {
        // Le serveur a listé un index que l'inventaire local ne connaît pas. Le
        // natif refusait déjà en silence dans ce cas (`Session_GetEquipInfoBy…`
        // échoue, aucun `SendMsg(146)` n'est émis) ; ici il est dit.
        ImGui::TextColored(ro::pal::kRed, "%s", i18n::Tr("Introuvable dans le sac."));
        if (hatch_sel_ == idx) missing_selected = true;
      } else if (!egg.has_pet) {
        ImGui::TextColored(ro::pal::kRed, "%s", i18n::Tr("Œuf vierge : rien à éclore."));
        if (hatch_sel_ == idx) blank_selected = true;
      } else {
        const int msg = rag::pet::IntimacyRankMsgId(egg.intimacy_rank);
        ImGui::TextColored(ro::pal::kLabel, "%s%s", msg ? msgstr::Utf8(msg) : "",
                           egg.renamed ? i18n::Tr("  ·  déjà renommé") : "");
      }
      ImGui::Unindent(side + 4.0f);
      ImGui::PopID();
    }

    // ── Les refus, DITS avant le clic ───────────────────────────────────────
    // ⚠ `clif_sendegg` ne filtre que sur le TYPE d'objet : un œuf vierge est bien
    // listé, mais `pet_select_egg` le refuse sans un mot. Le natif laissait
    // cliquer, et il ne se passait rien.
    const char* refusal = nullptr;
    if (hatch_sel_ < 0) {
      // Liste vide : la ligne au-dessus l'a déjà dit, on ne le répète pas — mais
      // le bouton reste hors service (chaîne VIDE, donc pas affichée).
      refusal = (hatch_count_ == 0) ? ""
                                    : i18n::Tr("Choisis l'œuf à faire éclore.");
    } else if (missing_selected) {
      refusal = i18n::Tr("Cet œuf n'est plus dans le sac.");
    } else if (blank_selected) {
      refusal = i18n::Tr("Cet œuf est vide : le serveur refusera de l'éclore.");
    }

    ImGui::Separator();
    if (refusal != nullptr && refusal[0]) {
      ImGui::PushStyleColor(ImGuiCol_Text, ro::pal::kLabel);
      ImGui::TextWrapped("%s", refusal);
      ImGui::PopStyleColor();
    }
    char ok[48], cancel[48];
    std::snprintf(ok, sizeof(ok), "%s##pet_hatch_ok", i18n::Tr("Faire éclore"));
    std::snprintf(cancel, sizeof(cancel), "%s##pet_hatch_cancel", i18n::Tr("Annuler"));
    ImGui::BeginDisabled(refusal != nullptr);
    if (ro::RoSmallButton(ok, 110.0f, 0.0f)) {
      pending_ = Pending::kSelectEgg;
      pending_arg_ = hatch_sel_;
      hatch_open_ = false;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Fermer sans répondre laisse le serveur dans son état `SA_TAMINGMONSTER`,
    // exactement comme le bouton « Annuler » du natif (id 185) : il ferme la
    // fenêtre 90 et n'envoie rien.
    if (ro::RoSmallButton(cancel, 80.0f, 0.0f)) hatch_open_ = false;
  }
  ro::EndRoWindow();
}

int PetWindow::InventoryCount(uint32_t itid) {
  // 🔴 Surtout PAS un quatrième parcours écrit à la main : la première version
  // de cette fonction comparait le nœud courant à l'ADRESSE du global au lieu du
  // nœud SENTINELLE qu'il contient, donc ne s'arrêtait jamais et rendait la
  // somme de la liste multipliée par le nombre de tours (315 en jeu : « 9450 »
  // pour 30 Yggdrasil Leaf). Le parcours vit à un seul endroit, avec les deux
  // recherches qui le partagent déjà.
  return itemcell::CountById(rag::kInventoryListAddr, itid);
}

// ── Réglages ────────────────────────────────────────────────────────────────

bool PetWindow::DrawSettings() {
  bool changed = false;
  if (ro::RoCheckbox(i18n::Tr("Confirmer « remettre dans l'œuf »"), &confirm_egg_))
    changed = true;
  mui::Tooltip(i18n::Tr(
      "Le client ne demandait rien : l'entrée était voisine de « Spectacle » dans "
      "le même menu, et se tromper range le pet pour de bon."));
  if (ro::RoCheckbox(i18n::Tr("Animer le sprite du pet"), &animate_)) changed = true;
  return changed;
}
