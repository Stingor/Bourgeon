#include "features/windows/mvp_tracker_window.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>  // std::tolower (filtre insensible à la casse)
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "bourgeon.h"
#include "features/link_gesture.h"               // links:: (le lien de monstre COMPLET)
#include "features/moonlight_ui/moonlight_ui.h"  // HelpMarker, SameLine, section de nav
#include "features/systems/mvp_tracker.h"
#include "imgui.h"
#include "ui/game_texture.h"  // CachedTextureFromGameFile, uipath::kUiRoot
#include "ui/mob_sprite.h"    // LoadMobSprite / DrawMobSprite
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "ragnarok/social.h"  // IsFriendByName / IsGuildMemberByName (invitabilité)
#include "ui/sprite_view.h"    // 🔬 SpriteCacheBytes (diagnostic d'occupation)
#include "ui/table_view.h"     // ro::SortedView : la table des créneaux
#include "ui/window_clamp.h"  // ClampWindowPosToScreen (lignes déplacées à la main)
#include "utils/i18n.h"
#include "utils/log_console.h"  // 🔬 LogDiag (diagnostic temporaire)

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

constexpr unsigned kAlertShowMs = 20000;  // le bandeau reste 20 s

// Côté du sprite d'un rang, en pixels d'interface. FIGÉ, et le pourquoi est dans
// MvpTrackerConfig : c'est cette valeur qui décide combien de sprites sont
// ouverts en même temps, donc si le cache tient (~25 Mo ici) ou part en
// va-et-vient (~59 Mo à 24 px, au-dessus du budget).
constexpr int kMvpSpritePx = 64;

// Libellé court d'une source. L'ordre suit e_mvp_source côté serveur.
const char* SourceLabel(mvp::Source source) {
  switch (source) {
    case mvp::Source::kManual: return i18n::Tr("saisi");
    case mvp::Source::kTomb:   return i18n::Tr("tombe");
    case mvp::Source::kKill:   return i18n::Tr("tué");
    case mvp::Source::kMirror: return i18n::Tr("miroir");
  }
  return "?";
}

// Une durée en secondes, écrite court : « 2h14 », « 47 min », « 12 s ».
void FormatDuration(int64_t seconds, char* out, size_t out_size) {
  if (seconds < 0) seconds = -seconds;
  if (seconds >= 3600)
    std::snprintf(out, out_size, "%lldh%02lld", (long long)(seconds / 3600),
                  (long long)((seconds % 3600) / 60));
  else if (seconds >= 60)
    std::snprintf(out, out_size, i18n::Tr("%lld min"), (long long)(seconds / 60));
  else
    std::snprintf(out, out_size, i18n::Tr("%lld s"), (long long)seconds);
}

// ── Les quatre créneaux SCRIPTÉS, habillés côté client ───────────────────────
//
// Le serveur n'envoie ni nom ni mob pour eux, et il a raison : là-bas le mob est
// tiré à chaque cycle, il n'est donc pas une identité mais un attribut de la
// dernière observation. Restait qu'à l'écran « (créneau scripté) » sur quatre
// lignes ne dit rien à personne.
//
// D'où cette table, et elle est CLIENT parce que c'est de la présentation : un
// libellé et de l'art, rien dont le serveur ait à décider. Les plages d'ids sont
// celles que les scripts tirent (moon/mobs/mvps.npc) — le sprite les parcourt en
// boucle, ce qui montre d'un coup d'œil que le MVP du Bio Lab n'est pas fixe.
//
// ⚠ Noms en ANGLAIS et non traduits : ce sont des noms de MVP, comme partout
// ailleurs dans l'interface.
struct ScriptedSlotArt {
  const char* map;
  const char* label;
  uint16_t    first;  // plage d'ids tirée par le script, bornes comprises
  uint16_t    last;   // == first quand le mob est unique
};

constexpr ScriptedSlotArt kScriptedArt[] = {
    {"lhz_dun03",  "MVP BioLab 3",     1646, 1651},  // rand(B_SEYREN, B_KATRINN)
    {"lhz_dun04",  "MVP BioLab 4",     2235, 2241},  // rand(B_RANDEL, B_TRENTINI)
    {"niflheim",   "Lord of Death",    1373, 1373},
    {"thana_boss", "Thanatos Phantom", 1708, 1708},
};

const ScriptedSlotArt* FindScriptedArt(const char* map) {
  for (const ScriptedSlotArt& art : kScriptedArt) {
    if (std::strcmp(art.map, map) == 0) return &art;
  }
  return nullptr;
}

// L'id de sprite à peindre pour un créneau. Sur un créneau scripté à plusieurs
// issues, il TOURNE : une image par seconde et demie, ce qui se lit comme « l'un
// de ceux-là » sans qu'on ait à l'écrire.
uint16_t SlotSpriteId(const mvp::Slot& slot) {
  if (slot.mob_id != 0) return slot.mob_id;
  const ScriptedSlotArt* art = FindScriptedArt(slot.map);
  if (art == nullptr) return 0;
  const uint16_t span = static_cast<uint16_t>(art->last - art->first + 1);
  if (span <= 1) return art->first;
  const int tick = static_cast<int>(ImGui::GetTime() / 1.5);
  return static_cast<uint16_t>(art->first + (tick % span));
}

// Le monstre que DÉSIGNE la ligne — pour le lien, son menu et sa fiche. Le
// dernier observé quand il y en a un (c'est LUI qui est tombé), sinon la
// première issue du créneau : une cible stable vaut mieux qu'une cible qui
// change sous le curseur au rythme du sprite.
uint16_t SlotLinkId(const mvp::Slot& slot, const mvp::Obs* obs) {
  if (slot.mob_id != 0) return slot.mob_id;
  if (obs != nullptr && obs->mob_id != 0) return obs->mob_id;
  const ScriptedSlotArt* art = FindScriptedArt(slot.map);
  return art != nullptr ? art->first : 0;
}

const char* SlotLabel(const mvp::Slot& slot, const mvp::Obs* /*obs*/) {
  if (slot.name[0] != '\0') return ro::WireToUtf8(slot.name);
  const ScriptedSlotArt* art = FindScriptedArt(slot.map);
  if (art != nullptr) return art->label;
  // Une carte scriptée que la table ignore : on le dit plutôt que d'inventer.
  return i18n::Tr("(créneau scripté)");
}

// La raison du refus, en clair. Un numéro ne dit rien à personne — et il ne
// disait rien non plus à qui devait le diagnostiquer.
const char* ResultText(uint8_t code) {
  switch (static_cast<mvp::Result>(code)) {
    case mvp::Result::kOk:             return "";
    case mvp::Result::kNoAccount:
      return i18n::Tr("Ce compte de jeu n'est rattaché à aucun compte Moonlight.");
    case mvp::Result::kAlreadyMember:
      return i18n::Tr("Vous êtes déjà dans un groupe : quittez-le d'abord.");
    case mvp::Result::kNotMember:
      return i18n::Tr("Vous n'êtes dans aucun groupe de chasse.");
    case mvp::Result::kNotOwner:
      return i18n::Tr("Seul le propriétaire du groupe peut faire cela.");
    case mvp::Result::kNoSuchUser:
      return i18n::Tr("Personnage introuvable : vérifiez l'orthographe.");
    case mvp::Result::kSelf:
      return i18n::Tr("Pas sur vous-même.");
    case mvp::Result::kFull:
      return i18n::Tr("Le groupe est complet.");
    case mvp::Result::kBadName:
      return i18n::Tr("Nom de groupe invalide (1 à 31 caractères).");
    case mvp::Result::kNoInvite:
      return i18n::Tr("Aucune invitation en attente.");
    case mvp::Result::kTargetInGroup:
      return i18n::Tr("Cette personne appartient déjà à un autre groupe.");
    case mvp::Result::kSql:
      return i18n::Tr("Erreur de base de données : rien n'a été changé.");
    case mvp::Result::kTargetSameGroup:
      return i18n::Tr("Ce personnage est déjà dans votre groupe : son compte "
                      "Moonlight y est, avec tous ses personnages.");
    case mvp::Result::kNotInvitable:
      return i18n::Tr("Ce personnage n'est ni dans votre guilde ni dans vos amis.");
  }
  // Un code que cette version ne connaît pas : le serveur est plus récent. On le
  // montre plutôt que de mentir avec un message voisin.
  return nullptr;
}

}  // namespace

void DrawMvpInviteMenuItem(const char* name_utf8) {
  MvpTracker* mvp = Bourgeon::Instance().mvp_tracker();
  if (mvp == nullptr || name_utf8 == nullptr || name_utf8[0] == '\0') return;

  // ⚠ Le pseudo repart dans l'encodage du FIL pour être comparé aux listes du
  // client (amis, roster de guilde, membres du groupe), qui n'y sont pas en
  // UTF-8. L'invitation, elle, part en UTF-8 : c'est `MvpTracker::Send` qui
  // convertit, la règle du projet voulant que la conversion vive chez le
  // producteur du paquet.
  const char* wire = ro::Utf8ToWire(name_utf8);

  // La règle du serveur est « guilde OU amis », et le client sait juger les DEUX
  // — d'où un grisage exact plutôt qu'une invitation vouée au refus.
  const char* why = nullptr;
  if (!mvp->config().enabled) {
    why = i18n::Tr("Le carnet de chasse MVP est éteint dans les réglages.");
  } else if (mvp->group().group_id == 0) {
    why = i18n::Tr("Il faut appartenir à un groupe de chasse pour y inviter "
                   "quelqu'un — créez-en un depuis le carnet.");
  } else if (!rag::social::IsFriendByName(wire) &&
             !rag::social::IsGuildMemberByName(wire)) {
    why = i18n::Tr("Seuls vos amis et vos compagnons de guilde peuvent rejoindre "
                   "votre carnet de chasse.");
  } else {
    for (const mvp::Member& m : mvp->group().members) {
      if (std::strncmp(m.name, wire, sizeof(m.name)) == 0) {
        why = i18n::Tr("Déjà dans votre carnet : son compte Moonlight y est, avec "
                       "tous ses personnages.");
        break;
      }
    }
  }

  if (why != nullptr) ImGui::BeginDisabled();
  if (ImGui::MenuItem(i18n::Tr("Inviter dans mon carnet MVP")))
    mvp->InviteMember(name_utf8);
  if (why != nullptr) {
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", why);
  } else if (ImGui::IsItemHovered()) {
    // Active, mais la règle mérite d'être dite avant le clic.
    ImGui::SetTooltip("%s", i18n::Tr(
              "Guilde et liste d'amis uniquement.\nInviter quelqu'un, c'est inviter "
              "tous ses personnages et tous ses comptes de jeu."));
  }
}

MvpTrackerWindow::MvpTrackerWindow() = default;

void MvpTrackerWindow::Open() {
  open_ = true;
  snapshot_requested_ = false;  // rouvrir redemande l'état

  // 🔴 OUVRIR, C'EST ALLUMER. L'interrupteur du panneau sert à s'abonner aux
  // deltas du serveur (bit UiCaps) et à recevoir alertes et badge fenêtre
  // fermée : ce n'est pas une permission d'ouvrir. Tant qu'il barrait le rendu,
  // l'icône du menu et le raccourci étaient des CLICS MORTS chez qui n'avait pas
  // d'abord coché la case — un bouton visible qui ne fait rien, le pire des
  // réglages.
  if (MvpTracker* state = Bourgeon::Instance().mvp_tracker()) {
    if (!state->config().enabled) {
      state->config().enabled = true;
      if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    }
  }
}

// 🔴 Le défilement n'est PAS fait ici : à cet instant la table n'existe pas
// encore (elle naît dans le prochain OnRenderUI), et son clipper ne connaît
// donc ni le rang du créneau ni sa hauteur. On pose une INTENTION, le rendu la
// consomme — c'est le même contrat que `OpenSettingTarget` du panneau.
void MvpTrackerWindow::OpenOn(uint16_t slot_id) {
  Open();
  focus_slot_     = slot_id;
  focus_ms_       = GetTickCount();
  focus_scrolled_ = false;
}

void MvpTrackerWindow::Toggle() {
  if (open_) {
    open_ = false;
    return;
  }
  Open();
}

bool MvpTrackerWindow::DrawSettings() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  if (state == nullptr) return false;

  MvpTrackerConfig& cfg = state->config();
  bool changed = false;

  changed |= ro::RoCheckbox(i18n::Tr("Activer le carnet de chasse MVP"), &cfg.enabled);
  SameLine();
  HelpMarker(i18n::Tr(
      "Éteint, le serveur cesse de t'envoyer ce que ton groupe observe — mais tes "
      "propres kills continuent de l'alimenter. Ce n'est donc pas un mode dégradé : "
      "tu nourris le groupe sans rien recevoir."));

  // HORS du BeginDisabled : ouvrir allume la fonction, ce bouton ne peut donc
  // pas dépendre d'elle sans redevenir le clic mort qu'on vient de supprimer.
  if (ro::RoButton(i18n::Tr("Ouvrir le carnet"))) {
    Open();
    changed = true;  // Open() a pu allumer l'interrupteur
  }
  SameLine();
  ImGui::TextDisabled("%s", i18n::Tr("ou une touche à lier : « Carnet de chasse MVP »"));

  ImGui::BeginDisabled(!cfg.enabled);

  changed |= ro::RoCheckbox(i18n::Tr("Alerte sonore sur un favori"), &cfg.alert_sound);

  ImGui::PushItemWidth(160.0f);
  changed |= WheelSliderInt(i18n::Tr("Préavis d'alerte (min)"), &cfg.alert_lead_min, 0, 60);
  ImGui::PopItemWidth();
  SameLine();
  HelpMarker(i18n::Tr(
      "Combien de minutes AVANT l'ouverture de la fenêtre de retour l'alerte se "
      "déclenche.\n\nLes favoris eux-mêmes sont enregistrés sur le serveur : ils "
      "suivent ton compte, donc tous tes personnages et tous tes postes. Seuls ce "
      "préavis et le son restent locaux."));

  changed |= ro::RoCheckbox(i18n::Tr("Sprite du MVP dans la liste"), &cfg.show_sprites);
  SameLine();
  HelpMarker(i18n::Tr(
      "Reconnaître un monstre à sa silhouette va plus vite que lire son nom "
      "anglais dans une table de quatre-vingts lignes.\n\nSurvoler son nom donne "
      "la fiche compacte, un clic droit ouvre les mêmes actions que sur un lien "
      "de monstre du chat — fiche, bestiaire, « où le trouver », @mobinfo, "
      "@whereis. Et survoler la carte montre le plan avec la tombe pointée."));

  ImGui::BeginDisabled(!cfg.show_sprites);
  changed |= ro::RoCheckbox(i18n::Tr("Animer les sprites"), &cfg.animate_sprites);
  SameLine();
  HelpMarker(i18n::Tr(
      "Éteint par défaut : quatre-vingts silhouettes qui bougent en même temps "
      "font de la table un aquarium, et le regard n'accroche plus la ligne qu'on "
      "cherche."));
  ImGui::EndDisabled();

  changed |= ro::RoCheckbox(i18n::Tr("Champ de filtre"), &cfg.show_filter);
  SameLine();
  HelpMarker(i18n::Tr(
      "Ajoute une barre de recherche au-dessus de la table, qui filtre sur le nom "
      "du MVP comme sur celui de la carte.\n\nLe filtre lui-même n'est pas "
      "enregistré : on filtre pour trouver quelque chose maintenant, pas pour "
      "rouvrir demain sur une table amputée sans se rappeler pourquoi.\n\n"
      "⚠ Les COLONNES, elles, se règlent au clic droit sur la ligne d'en-tête — "
      "on y montre « Délai », le temps de retour habituel, masqué par défaut."));

  changed |= ro::RoCheckbox(i18n::Tr("Tombes sur la minimap"), &cfg.show_tombs);
  SameLine();
  HelpMarker(i18n::Tr(
      "Marque l'endroit exact où chaque MVP connu de ton groupe est tombé, sur la "
      "carte concernée. Doré quand l'instant du retour a été mérité au Convex "
      "Mirror, orangé quand on n'a qu'une fenêtre.\n\nIndépendant du marqueur "
      "« Boss (Convex Mirror) » de la minimap, qui vient du client."));

  SeparatorText(i18n::Tr("Lignes détachées"));
  ImGui::TextWrapped("%s", i18n::Tr(
            "Glisser une ligne du carnet par sa poignée, à gauche, et la lâcher HORS de "
            "la fenêtre : elle reste à l'écran et survit à la fermeture du carnet. Les "
            "lignes s'aimantent entre elles. Clic droit sur une ligne pour la remettre "
            "au carnet."));

  changed |= ro::RoCheckbox(i18n::Tr("Sprite dans les lignes détachées"),
                            &cfg.line_show_sprite);
  SameLine();
  HelpMarker(i18n::Tr(
      "Réglage à part de celui de la table, et c'est voulu : une ligne posée sur "
      "le décor du jeu cherche la discrétion, là où la table cherche à faire "
      "reconnaître un monstre d'un coup d'œil."));

  changed |= RoColorSwatch(i18n::Tr("Ligne — texte"), cfg.line_text_col);
  changed |= RoColorSwatch(i18n::Tr("Ligne — fond"), cfg.line_bg_col);
  SameLine();
  HelpMarker(i18n::Tr(
      "Le canal alpha de chaque couleur EST son opacité — les deux sont réglables "
      "séparément, précisément pour obtenir ce qu'on veut d'une ligne discrète : "
      "un fond presque transparent sous un texte resté lisible."));

  if (!lines_.empty()) {
    char label[64];
    std::snprintf(label, sizeof(label), i18n::Tr("Tout remettre au carnet (%d)"),
                  static_cast<int>(lines_.size()));
    if (ro::RoButton(label)) {
      lines_.clear();
      changed = true;
    }
  }

  ImGui::EndDisabled();
  return changed;
}

void MvpTrackerWindow::OnModeSwitch(ModeMgr::ModeType mode_type, const char* /*map_name*/) {
  if (mode_type == ModeMgr::ModeType::kGame) return;
  // MvpTracker a vidé son état : redemander l'instantané à la prochaine ouverture,
  // sinon la fenêtre resterait vide en croyant l'avoir déjà demandé.
  snapshot_requested_ = false;
  fired_count_ = 0;
  alert_slot_ = 0xFFFF;
  confirm_leave_ = false;
}

void MvpTrackerWindow::OnTick() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  if (state == nullptr || !state->config().enabled) return;

  // Le catalogue part une fois par session, sur demande. On la formule au tick
  // et non au rendu : la socket peut n'être pas prête à la première frame.
  if (!snapshot_requested_ && open_) {
    state->RequestSnapshot();
    snapshot_requested_ = true;
  }
}

int64_t MvpTrackerWindow::ParseKillTime(const char* text) const {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  if (state == nullptr || text == nullptr) return 0;

  bool yesterday = false;
  char digits[8] = {};
  int n = 0;

  for (const char* p = text; *p != '\0' && n < 4; ++p) {
    if (*p == '-') {
      yesterday = true;
      continue;
    }
    if (*p >= '0' && *p <= '9') digits[n++] = *p;
    // « h », « : » et les espaces ne sont que du décor : on ne garde que les
    // chiffres, ce qui accepte 1430, 14h30, 14:30 et « 14 30 » sans les
    // énumérer.
  }

  if (n != 4 && n != 3) return 0;

  int hour, minute;
  if (n == 4) {
    hour   = (digits[0] - '0') * 10 + (digits[1] - '0');
    minute = (digits[2] - '0') * 10 + (digits[3] - '0');
  } else {
    hour   = digits[0] - '0';
    minute = (digits[1] - '0') * 10 + (digits[2] - '0');
  }

  if (hour > 23 || minute > 59) return 0;

  const int64_t now = state->ServerNow();
  const std::time_t now_t = static_cast<std::time_t>(now);
  std::tm local{};
  if (localtime_s(&local, &now_t) != 0) return 0;

  local.tm_hour = hour;
  local.tm_min  = minute;
  local.tm_sec  = 0;
  std::time_t when = std::mktime(&local);
  if (when == static_cast<std::time_t>(-1)) return 0;

  // Une mort ne peut pas être dans le futur : une heure « à venir » désigne donc
  // la veille, ce qui rend le « - » facultatif la plupart du temps.
  if (yesterday || static_cast<int64_t>(when) > now) when -= 24 * 3600;

  return static_cast<int64_t>(when);
}

// Le titre de la fenêtre REPLIÉE : la prochaine échéance connue.
//
// « Prochaine » = la borne BASSE de fenêtre la plus proche parmi les créneaux
// que le groupe a observés, en écartant ceux dont la fenêtre est passée depuis
// plus d'une demi-heure — au-delà, le MVP est soit revenu, soit déjà tué par
// quelqu'un d'autre, et l'annoncer serait un mensonge poli.
//
// Un créneau DÉJÀ dans sa fenêtre passe devant : « possible maintenant » vaut
// mieux que « dans 40 min » quand les deux sont vrais.
void MvpTrackerWindow::BuildCollapsedTitle(char* out, size_t cap) {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  const char* plain = i18n::Tr("Carnet de chasse MVP###bourgeon_mvp_tracker");

  if (state == nullptr) {
    std::snprintf(out, cap, "%s", plain);
    return;
  }

  const int64_t now = state->ServerNow();
  const mvp::Slot* best = nullptr;
  const mvp::Obs*  best_obs = nullptr;
  int64_t best_from = 0;

  for (const mvp::Slot& slot : state->slots()) {
    int64_t from = 0, to = 0;
    bool exact = false;
    if (!state->Window(slot.slot_id, &from, &to, &exact)) continue;
    if (!exact && to + 1800 < now) continue;
    if (exact && from + 900 < now) continue;
    if (best == nullptr || from < best_from) {
      best      = &slot;
      best_obs  = state->FindObs(slot.slot_id);
      best_from = from;
    }
  }

  if (best == nullptr) {
    std::snprintf(out, cap, "%s", plain);
    return;
  }

  const char* name = SlotLabel(*best, best_obs);

  if (best_from <= now) {
    std::snprintf(out, cap, i18n::Tr("MVP : %s possible###bourgeon_mvp_tracker"), name);
    return;
  }

  char delay[32];
  FormatDuration(best_from - now, delay, sizeof(delay));
  std::snprintf(out, cap, i18n::Tr("MVP : %s dans %s###bourgeon_mvp_tracker"), name, delay);
}

void MvpTrackerWindow::OnRenderUI() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  if (state == nullptr) return;

  sprite_loads_this_frame_ = 0;

  // L'ALERTE, elle, suit bien l'interrupteur : c'est du bruit non sollicité, et
  // elle a un sens fenêtre fermée. La FENÊTRE, non — on l'a demandée.
  if (state->config().enabled) DrawAlerts();

  // Les lignes détachées AVANT le carnet, et hors du test `open_` : c'est tout
  // leur propos, survivre à sa fermeture.
  if (state->config().enabled) DrawPinnedLines();

  // Le geste de détachement continue même quand le carnet vient de se fermer
  // sous la souris : on le termine ici, pas dans la table.
  FinishRowDrag();

  // 🔴 HORS du test `open_` : une invitation doit s'imposer, pas attendre qu'on
  // pense à ouvrir le carnet. C'est d'ailleurs souvent l'inverse — c'est elle
  // qui donne une raison de l'ouvrir.
  DrawInvitePopup();

  if (!open_) return;

  ImGui::SetNextWindowSize(ImVec2(ro::Px(720), ro::Px(460)), ImGuiCond_FirstUseEver);
  // Bullet de la barre de titre : raccourci vers la section « Carnet de chasse
  // MVP » du panneau Moonlight, comme l'Atlas, l'inventaire et le storage.
  ro::SetNextWindowTitleBullet(i18n::Tr("Réglages du carnet"));
  ro::SetNextWindowPinnable();  // épingle : Échap ne referme plus le carnet

  // ── Le titre porte la prochaine échéance QUAND LA FENÊTRE EST REPLIÉE ──────
  // Repliée, la barre de titre est tout ce qui reste : autant qu'elle dise la
  // seule chose qu'on voulait savoir. Dépliée, la table le dit déjà, et doubler
  // l'information dans le titre ne ferait que la faire clignoter.
  //
  // ⚠ Le pli n'est connu qu'APRÈS Begin, alors que le titre se donne AVANT :
  // on se fie donc à l'état de la frame précédente. Une frame de retard sur la
  // bascule, invisible à l'œil.
  //
  // 🔴 La partie après « ### » ne bouge JAMAIS : c'est elle qui identifie la
  // fenêtre pour ImGui. Un titre changeant sans elle ferait perdre position,
  // taille et épingle à chaque tick.
  char title[128];
  if (was_collapsed_) {
    BuildCollapsedTitle(title, sizeof(title));
  } else {
    std::snprintf(title, sizeof(title), "%s",
                  i18n::Tr("Carnet de chasse MVP###bourgeon_mvp_tracker"));
  }

  bool keep_open = true;
  const bool begun = ro::BeginRoWindow(title, &keep_open);
  was_collapsed_ = !begun && keep_open;
  // ⚠ À lire JUSTE APRÈS BeginRoWindow et hors du `if (begun)` : le drapeau est
  // posé par Begin lui-même et vaut aussi pour une fenêtre repliée.
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceMvpTracker);

  if (!begun) {
    ro::EndRoWindow();
    if (!keep_open) open_ = false;
    return;
  }

  DrawGroupPanel();
  ImGui::Separator();
  DrawTable();
  DrawManualPopup();

  // Le rectangle du carnet, relevé DEDANS : c'est lui qui décide si un lâcher
  // est « dehors », donc s'il détache.
  win_pos_  = ImGui::GetWindowPos();
  win_size_ = ImGui::GetWindowSize();

  ro::EndRoWindow();
  if (!keep_open) open_ = false;
}

// Le lâcher d'un glissement parti d'une poignée. Hors de la fenêtre du carnet :
// on détache. Dedans : on ne fait rien — un glissement maladroit dans la table
// ne doit pas semer une ligne sous le carnet, où elle serait invisible.
void MvpTrackerWindow::FinishRowDrag() {
  if (drag_slot_ == 0xFFFF) return;

  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  // Le fantôme qui suit la souris : sans lui, rien ne dit que le geste a pris.
  if (const mvp::Slot* slot = state != nullptr ? state->FindSlot(drag_slot_) : nullptr) {
    char text[128];
    FormatLineText(*slot, text, sizeof(text));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 p0(mouse.x + 12.0f, mouse.y + 8.0f);
    dl->AddRectFilled(ImVec2(p0.x - 4.0f, p0.y - 2.0f),
                      ImVec2(p0.x + size.x + 4.0f, p0.y + size.y + 2.0f),
                      IM_COL32(10, 10, 15, 200), 3.0f);
    dl->AddText(p0, IM_COL32(245, 237, 209, 255), text);
  }

  if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;

  const bool inside = mouse.x >= win_pos_.x && mouse.x <= win_pos_.x + win_size_.x &&
                      mouse.y >= win_pos_.y && mouse.y <= win_pos_.y + win_size_.y;
  if (!inside && state != nullptr) {
    if (const mvp::Slot* slot = state->FindSlot(drag_slot_)) PinSlot(*slot, mouse);
  }
  drag_slot_ = 0xFFFF;
}

// ── L'invitation REÇUE, en modale ───────────────────────────────────────────
//
// C'était un bandeau DANS le carnet, et deux choses n'allaient pas :
//
// 1. 🔴 Le doré était illisible. Le corps d'une fenêtre RO est BEIGE CLAIR, et
//    le projet a déjà payé cette leçon ailleurs (le marqueur MVP de la fenêtre
//    Navigation, qui a fini en pastille pour la même raison). On s'en remet donc
//    à la couleur de texte du skin, qui est calibrée pour ce fond.
// 2. Une invitation qu'on ne voit QUE si le carnet est ouvert n'est pas une
//    invitation. La modale, elle, s'impose — et c'est bien ce qu'on veut d'une
//    demande à laquelle il faut répondre.
//
// ⚠ MÊME identifiant des deux côtés : `BeginRoPopupModal` prend le titre pour
// ID, donc `OpenPopup` doit recevoir exactement la même chaîne.
void MvpTrackerWindow::DrawInvitePopup() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  const uint32_t invite = state->pending_invite();

  // Ouverte une seule fois par invitation : réémettre OpenPopup à chaque frame
  // empêcherait de la fermer.
  if (invite != 0 && invite_shown_ != invite) {
    ImGui::OpenPopup(i18n::Tr("Invitation reçue"));
    invite_shown_ = invite;
  }
  if (invite == 0) invite_shown_ = 0;

  if (!ro::BeginRoPopupModal(i18n::Tr("Invitation reçue"))) return;

  if (invite == 0) {
    // Répondu ailleurs, ou changement de personnage : plus rien à décider.
    ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
    return;
  }

  // 🔴 `Text` et NON `TextWrapped` : dans une modale à taille automatique, le
  // wrap fixe la largeur au lieu de la suivre — la phrase se pliait en trois
  // lignes dans une boîte étroite. Sans wrap, la boîte s'élargit à la phrase,
  // comme le fait la demande d'ami juste à côté. Le nom de groupe est borné à
  // 32 octets, la largeur ne peut donc pas s'emballer.
  ImGui::Text(i18n::Tr("Le groupe de chasse « %s » vous invite à partager "
                       "son carnet MVP."),
              ro::WireToUtf8(state->pending_invite_name()));
  ImGui::Spacing();

  // Accepter en étant déjà membre est REFUSÉ par le serveur, jamais une sortie
  // silencieuse : autant le dire ici et griser plutôt que d'essuyer le refus.
  const bool already = state->group().group_id != 0;
  if (already) {
    ImGui::TextDisabled(
        "%s", i18n::Tr("Quittez d'abord votre groupe actuel pour pouvoir accepter."));
    ImGui::Spacing();
  }

  ImGui::BeginDisabled(already);
  if (ro::RoButton(i18n::Tr("Accepter"))) {
    state->AcceptInvite();
    // Accepter, c'est vouloir voir ce que le groupe sait : on ouvre le carnet
    // dans la foulée plutôt que de laisser le joueur le chercher.
    Open();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Refuser"))) {
    state->DeclineInvite();
    ImGui::CloseCurrentPopup();
  }

  ro::EndRoPopupModal();
}

void MvpTrackerWindow::DrawGroupPanel() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  const mvp::Group& group = state->group();

  // Le dernier refus du serveur, affiché quelques secondes.
  if (state->last_result() != 0 &&
      GetTickCount() - state->last_result_ms() < 6000) {
    // Rouge FONCÉ, même raison que le vert de la colonne « Retour » : le fond
    // de cette fenêtre est clair.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.12f, 0.12f, 1.0f));
    if (const char* why = ResultText(state->last_result()))
      ImGui::TextWrapped("%s", why);
    else
      ImGui::TextWrapped(i18n::Tr("Refusé par le serveur (code %u)."),
                         state->last_result());
    ImGui::PopStyleColor();
  }

  if (group.group_id == 0) {
    ImGui::TextDisabled("%s", i18n::Tr("Vous n'êtes dans aucun groupe de chasse."));
    ImGui::SetNextItemWidth(ro::Px(200));
    ImGui::InputTextWithHint("##mvp_group_name", i18n::Tr("Nom du groupe"),
                             group_name_buf_, sizeof(group_name_buf_));
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Créer")) && group_name_buf_[0] != '\0') {
      state->CreateGroup(group_name_buf_);
      group_name_buf_[0] = '\0';
    }
    return;
  }

  char header[96];
  // 🔴 On compte les COMPTES, pas les lignes : depuis que chaque personnage en
  // ligne a la sienne, un joueur en multi-client en produit trois à lui seul, et
  // « 3 membres » pour une personne serait faux.
  int account_count = 0;
  for (size_t i = 0; i < group.members.size(); ++i) {
    if (i == 0 || group.members[i].user_id != group.members[i - 1].user_id)
      ++account_count;
  }

  std::snprintf(header, sizeof(header), i18n::Tr("Groupe « %s » — %d membres###mvp_group"),
                ro::WireToUtf8(group.name), account_count);

  if (!ImGui::CollapsingHeader(header)) return;

  const uint32_t owner_id = group.owner_user_id;

  if (ImGui::BeginTable("##mvp_members", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn(i18n::Tr("Membre"));
    ImGui::TableSetupColumn(i18n::Tr("Niveau"), ImGuiTableColumnFlags_WidthFixed, ro::Px(50));
    ImGui::TableSetupColumn(i18n::Tr("État"), ImGuiTableColumnFlags_WidthFixed, ro::Px(70));
    ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, ro::Px(70));
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < group.members.size(); ++i) {
      const mvp::Member& member = group.members[i];
      ImGui::TableNextRow();
      // Sous un NOM, pour la même raison que les rangs de créneaux plus bas :
      // à la racine de la fenêtre, `PushID(i)` rendrait l'identifiant que
      // `TableHeadersRow` pose déjà sur la colonne i.
      ImGui::PushID("member");
      ImGui::PushID(static_cast<int>(i));

      // La PREMIÈRE ligne d'un compte porte ce qui appartient au COMPTE — le
      // marqueur de propriétaire et le bouton d'exclusion. Les suivantes sont
      // d'autres têtes du même compte : les répéter donnerait trois boutons
      // « Exclure » pour une seule personne, dont un seul geste suffit à sortir.
      const bool first_of_account =
          i == 0 || group.members[i - 1].user_id != member.user_id;

      ImGui::TableSetColumnIndex(0);
      if (!first_of_account) ImGui::Indent(ro::Px(12.0f));
      ImGui::TextUnformatted(member.name[0] != '\0' ? ro::WireToUtf8(member.name)
                                                    : i18n::Tr("(compte sans personnage)"));
      if (!first_of_account) ImGui::Unindent(ro::Px(12.0f));
      if (first_of_account && member.user_id == owner_id) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", i18n::Tr("(propriétaire)"));
      }

      ImGui::TableSetColumnIndex(1);
      ImGui::Text("%d", member.level);

      ImGui::TableSetColumnIndex(2);
      if (member.online)
        ImGui::TextUnformatted(i18n::Tr("en ligne"));
      else
        ImGui::TextDisabled("%s", i18n::Tr("absent"));

      ImGui::TableSetColumnIndex(3);
      if (first_of_account && member.user_id != owner_id &&
          ro::RoSmallButton(i18n::Tr("Exclure"))) {
        // Exclut le COMPTE, donc toutes ses têtes d'un coup — l'appartenance n'a
        // jamais été au personnage.
        state->KickMember(member.name);
      }

      ImGui::PopID();  // i
      ImGui::PopID();  // "member"
    }
    ImGui::EndTable();
  }

  ImGui::SetNextItemWidth(ro::Px(180));
  ImGui::InputTextWithHint("##mvp_invite", i18n::Tr("Nom du personnage"),
                           invite_buf_, sizeof(invite_buf_));
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Inviter")) && invite_buf_[0] != '\0') {
    state->InviteMember(invite_buf_);
    invite_buf_[0] = '\0';
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "%s", i18n::Tr("Guilde et liste d'amis uniquement.\n"
                       "Inviter quelqu'un, c'est inviter tous ses personnages et tous "
                       "ses comptes de jeu."));
  }

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Quitter"))) confirm_leave_ = true;

  if (confirm_leave_) {
    ImGui::TextWrapped(
        "%s", i18n::Tr("Quitter le groupe ? Si vous en êtes le dernier membre, il sera "
                       "détruit avec ses invitations."));
    if (ro::RoButton(i18n::Tr("Confirmer"))) {
      state->LeaveGroup();
      confirm_leave_ = false;
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"))) confirm_leave_ = false;
  }
}

// Le sprite du monstre dans son rang, en pose d'attente orientée sud.
//
// `LoadMobSprite` mémorise ses ressources, échec compris : une classe sans .spr
// ne relance donc pas un chargement à chaque frame. Les MVP rendus par un
// MODÈLE 3D (Emperium et consorts) ressortent en `is_model` et n'ont aucun
// sprite — on laisse simplement la place vide plutôt que d'annoncer une absence
// d'art qui serait un mensonge.
void MvpTrackerWindow::DrawRowSprite(uint16_t mob_id, int size_px, bool animate) {
  const float side = ro::Px(static_cast<float>(size_px));

  // 🔴🔴 AU PLUS DEUX CHARGEMENTS PAR FRAME.
  //
  // Le cache de sprites plafonne à 48 Mo et évince (`TrimFor`, sprite_view.cc).
  // Un sprite de MVP pèse lourd : faire défiler la table fait entrer des dizaines
  // d'entrées neuves d'un coup, chacune chassant les précédentes — le cache bat
  // la campagne et le défilement s'effondre. C'est exactement le cas que décrit
  // le commentaire de `kMaxCached` : quatre-vingts nouvelles têtes pendant que
  // les quatre-vingts d'avant sont encore là.
  //
  // On borne donc l'ENTRÉE, pas l'affichage : les lignes déjà chargées se
  // dessinent toutes, les nouvelles arrivent en deux ou trois frames. À l'œil,
  // les sprites se remplissent pendant qu'on relâche la molette.
  auto known = sprites_.find(mob_id);
  const bool already_loaded =
      known != sprites_.end() && known->second.class_id == static_cast<int>(mob_id);

  if (!already_loaded) {
    if (sprite_loads_this_frame_ >= kMaxSpriteLoadsPerFrame) {
      ImGui::Dummy(ImVec2(side, side));
      return;
    }
    ++sprite_loads_this_frame_;
  }

  // Sa propre poignée, gardée d'une frame à l'autre.
  ro::MobSpriteRes& res = sprites_[mob_id];

  if (!ro::LoadMobSprite(static_cast<int>(mob_id), &res)) {
    ImGui::Dummy(ImVec2(side, side));
    return;
  }

  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  // ⚠ Les DEUX paramètres, pas un : `anim_seconds` avance l'horloge et
  // `ms_per_frame` décide de la cadence. Laisser la cadence à 0 fige l'image
  // quelle que soit l'horloge, et c'est bien ce qu'on veut à l'arrêt.
  ro::DrawMobSprite(ImGui::GetWindowDrawList(), res, p0,
                    ImVec2(p0.x + side, p0.y + side),
                    animate ? static_cast<float>(ImGui::GetTime()) : 0.0f,
                    /*action=*/0u, animate ? 130.0f : 0.0f);
  ImGui::Dummy(ImVec2(side, side));
}

// La carte, en infobulle, avec la tombe pointée dessus.
//
// Le bitmap est celui du radar, servi par le cache de textures du jeu — le même
// que la minimap et que les miniatures de la fenêtre Navigation.
//
// 🔴 La projection est celle de la minimap : une FRACTION DE CARTE, et
// `(cells_h - y) / cells_h` sur l'axe vertical parce que le repère du jeu monte
// tandis que celui de l'image descend.
//
// 🔴🔴 Et la fraction se prend sur la taille EN CELLULES (`slot.map_xs/ys`,
// envoyée par le serveur), jamais sur celle du BITMAP. Une première version
// supposait « un texel = une cellule » faute d'avoir la mesure : c'est faux, les
// plans de radar n'ont aucune raison d'être à l'échelle du .gat, et la tombe
// tombait à côté. Le client ne peut pas mesurer une carte où il n'est pas — la
// donnée devait donc venir du serveur, qui l'a.
void MvpTrackerWindow::DrawMapPreview(const mvp::Slot& slot, const mvp::Obs* obs) {
  char path[192];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\map\\%s.bmp",
              ro::uipath::kUiRoot, slot.map);
  const ro::GameTexture tex = ro::CachedTextureFromGameFile(path);

  ImGui::BeginTooltip();
  ImGui::TextUnformatted(slot.map);

  if (!tex.tex) {
    ImGui::TextDisabled("%s", i18n::Tr("(pas de miniature)"));
    ImGui::EndTooltip();
    return;
  }

  const float side = ro::Px(180.0f);
  const float w = static_cast<float>(tex.w), h = static_cast<float>(tex.h);
  const float scale = w > h ? side / w : side / h;  // garder les proportions
  const ImVec2 draw(w * scale, h * scale);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();

  ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex.tex)), draw);

  // -1 veut dire « position inconnue » ; 0,0 n'a jamais ce sens ici, le serveur
  // convertit précisément pour ne pas reproduire le bug du Convex Mirror natif.
  if (obs != nullptr && obs->tomb_x >= 0 && obs->tomb_y >= 0 &&
      slot.map_xs > 0 && slot.map_ys > 0) {
    const float fx = static_cast<float>(obs->tomb_x) / static_cast<float>(slot.map_xs);
    const float fy = (static_cast<float>(slot.map_ys) - static_cast<float>(obs->tomb_y)) /
                     static_cast<float>(slot.map_ys);
    if (fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f) {
      const ImVec2 at(p0.x + fx * draw.x, p0.y + fy * draw.y);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      // Même code couleur que la couche de minimap : doré quand l'instant a été
      // mérité au Convex Mirror, orangé quand on n'a qu'une fenêtre.
      const ImU32 tint = obs->exact_respawn != 0 ? IM_COL32(255, 216, 96, 255)
                                                 : IM_COL32(232, 140, 60, 255);
      dl->AddCircleFilled(at, ro::Px(4.0f), tint);
      dl->AddCircle(at, ro::Px(4.0f), IM_COL32(20, 20, 20, 220), 0, 1.5f);
    }
    ImGui::Text(i18n::Tr("Tombe en (%d,%d)."), obs->tomb_x, obs->tomb_y);
  } else {
    ImGui::TextDisabled("%s", i18n::Tr("Aucune tombe connue."));
  }

  ImGui::EndTooltip();
}

// Le bouton FAVORI, en art quand il est fourni.
//
// Deux bitmaps sous `유저인터페이스\menu_icon\`, mêmes conventions que le reste
// de l'art d'interface — le chargeur applique la clé magenta `FF 00 FF`
// (ui/game_texture.cc) et mémorise le résultat PAR CHEMIN, échec compris : un
// fichier absent ne relance donc pas un chargement à chaque frame.
//
//   bt_mvpfav.bmp      état ÉTEINT (non favori)
//   bt_mvpfav_on.bmp   état ALLUMÉ
//
// ⚠ Tant que les fichiers n'existent pas, on retombe sur le petit bouton texte :
// la fonction doit marcher avant que l'art n'arrive, sinon on ne peut plus
// marquer un favori en attendant.
bool MvpTrackerWindow::DrawFavoriteButton(bool favorite) {
  char path[192];
  _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\menu_icon\\bt_mvpfav%s.bmp",
              ro::uipath::kUiRoot, favorite ? "_on" : "");
  const ro::GameTexture tex = ro::CachedTextureFromGameFile(path);

  // « ### » : l'identifiant du repli ne doit pas dépendre de l'état du favori,
  // sans quoi le bouton change d'identité sous le doigt à l'instant du clic —
  // et c'est le même identifiant que celui du bouton texturé juste en dessous.
  if (!tex.tex) return ro::RoSmallButton(favorite ? "*###fav" : " ###fav");

  const float side = ro::Px(static_cast<float>(tex.h > 0 ? tex.h : 16));
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##fav", ImVec2(side, side));
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  // Un éteint survolé s'éclaircit, faute d'un troisième bitmap : c'est le retour
  // visuel minimum, et il évite d'exiger un art de survol.
  const ImU32 tint = (hovered && !favorite) ? IM_COL32(255, 255, 255, 255)
                                            : IM_COL32(230, 230, 230, 255);
  ImGui::GetWindowDrawList()->AddImage(
      static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(tex.tex)), p0,
      ImVec2(p0.x + side, p0.y + side), ImVec2(0, 0), ImVec2(1, 1), tint);

  return clicked;
}

// ── Les lignes détachées ─────────────────────────────────────────────────────

void MvpTrackerWindow::ResolvePinnedSlots() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  if (state == nullptr) return;

  for (PinnedLine& line : lines_) {
    line.slot_id = 0xFFFF;
    for (const mvp::Slot& slot : state->slots()) {
      // Le mob ET la carte : le même MVP vit sur quatre cartes, et sur les
      // créneaux scriptés c'est l'inverse — mob_id 0, la carte identifie.
      if (slot.mob_id == line.mob_id && std::strcmp(slot.map, line.map) == 0) {
        line.slot_id = slot.slot_id;
        break;
      }
    }
  }
  resolved_for_ = state->slots().size();
}

void MvpTrackerWindow::PinSlot(const mvp::Slot& slot, ImVec2 at) {
  for (const PinnedLine& line : lines_) {
    if (line.mob_id == slot.mob_id && std::strcmp(line.map, slot.map) == 0) return;
  }

  PinnedLine line;
  line.mob_id  = slot.mob_id;
  std::snprintf(line.map, sizeof(line.map), "%s", slot.map);
  line.x       = static_cast<int>(at.x);
  line.y       = static_cast<int>(at.y);
  line.slot_id = slot.slot_id;
  lines_.push_back(line);

  if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
}

float MvpTrackerWindow::SnapLine(float v, float ext, int self, bool y_axis) const {
  constexpr float kSnap = 10.0f;  // rayon de magnétisme, en pixels
  float best = v, best_dist = kSnap;

  for (int j = 0; j < static_cast<int>(lines_.size()); ++j) {
    if (j == self) continue;
    const float opos = y_axis ? static_cast<float>(lines_[j].y)
                              : static_cast<float>(lines_[j].x);
    // 🔴 La taille de la VOISINE, relevée à son dernier dessin — pas la mienne.
    // Quatre candidats, comme l'aimantage des icônes de menu : aligner les
    // débuts, aligner les fins, me poser juste APRÈS elle, me poser juste AVANT.
    const float oext = y_axis ? lines_[j].measured_h : lines_[j].measured_w;
    const float cands[4] = {opos, opos + oext - ext, opos + oext, opos - ext};
    for (float c : cands) {
      float d = c - v;
      if (d < 0.0f) d = -d;
      if (d < best_dist) { best_dist = d; best = c; }
    }
  }
  return best;
}

void MvpTrackerWindow::FormatLineText(const mvp::Slot& slot, char* out, size_t cap) {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  const mvp::Obs* obs = state->FindObs(slot.slot_id);
  const int64_t now = state->ServerNow();

  // 🔴 La CARTE fait partie du nom sur une ligne détachée. Quatre créneaux
  // portent « Doppelganger » et trois « Dark Lord » : sans la carte, deux
  // lignes posées côte à côte sur le bureau sont rigoureusement identiques et
  // on ne sait plus laquelle surveille quoi. Le carnet, lui, a une colonne
  // pour ça ; la ligne détachée n'a que ce texte.
  char name[96];
  std::snprintf(name, sizeof(name), "%s (%s)", SlotLabel(slot, obs), slot.map);

  int64_t from = 0, to = 0;
  bool exact = false;
  if (!state->Window(slot.slot_id, &from, &to, &exact)) {
    std::snprintf(out, cap, "%s — %s", name, i18n::Tr("jamais vu"));
    return;
  }

  char a[32];
  if (exact) {
    FormatDuration(from - now, a, sizeof(a));
    std::snprintf(out, cap, "%s — %s",
                  name, from <= now ? i18n::Tr("de retour") : a);
    return;
  }
  if (to <= now) {
    std::snprintf(out, cap, "%s — %s", name, i18n::Tr("fenêtre passée"));
    return;
  }
  if (from <= now) {
    FormatDuration(to - now, a, sizeof(a));
    std::snprintf(out, cap, "%s — %s", name, i18n::Tr("possible"));
    return;
  }
  char b[32];
  FormatDuration(from - now, a, sizeof(a));
  FormatDuration(to - now, b, sizeof(b));
  std::snprintf(out, cap, "%s — %s – %s", name, a, b);
}

// Chaque ligne dans SA fenêtre : c'est ce qui lui donne son fond, son opacité et
// son propre rectangle de survol, sans rien devoir au carnet — dont elle survit
// à la fermeture, ce qui est tout le propos.
//
// ⚠ La position est posée à CHAQUE frame depuis nos coordonnées, et le
// déplacement est joué à la main (comme les icônes de menu) : laisser ImGui
// bouger la fenêtre nous ferait perdre l'aimantage, qui doit corriger la
// position AVANT qu'elle ne soit appliquée.
void MvpTrackerWindow::DrawPinnedLines() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  if (state == nullptr || lines_.empty()) return;

  if (resolved_for_ != state->slots().size()) ResolvePinnedSlots();

  const MvpTrackerConfig& cfg = state->config();
  const ImVec4 text_col(cfg.line_text_col[0], cfg.line_text_col[1],
                        cfg.line_text_col[2], cfg.line_text_col[3]);
  const ImVec4 bg_col(cfg.line_bg_col[0], cfg.line_bg_col[1],
                      cfg.line_bg_col[2], cfg.line_bg_col[3]);

  bool dirty = false;

  for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
    PinnedLine& line = lines_[i];
    const mvp::Slot* slot = state->FindSlot(line.slot_id);
    if (slot == nullptr) continue;  // catalogue pas encore reçu

    char text[128];
    FormatLineText(*slot, text, sizeof(text));

    char id[48];
    std::snprintf(id, sizeof(id), "##mvpline_%u_%s", line.mob_id, line.map);

    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(line.x),
                                   static_cast<float>(line.y)), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg_col);
    ImGui::PushStyleColor(ImGuiCol_Text, text_col);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    if (ImGui::Begin(id, nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav)) {
      const uint16_t sprite_mob = SlotSpriteId(*slot);
      if (cfg.line_show_sprite && sprite_mob != 0) {
        DrawRowSprite(sprite_mob, kMvpSpritePx, cfg.animate_sprites);
        ImGui::SameLine();
      }
      ImGui::TextUnformatted(text);

      const ImVec2 size = ImGui::GetWindowSize();
      // Relevée pour l'aimantage des AUTRES lignes : sans ça, chacune ignore la
      // hauteur réelle de ses voisines et laisse un trou en se collant.
      line.measured_w = size.x;
      line.measured_h = size.y;
      const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        line_drag_ = i;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        line_drag_off_ = ImVec2(mouse.x - static_cast<float>(line.x),
                                mouse.y - static_cast<float>(line.y));
      }
      if (line_drag_ == i && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        float nx = mouse.x - line_drag_off_.x;
        float ny = mouse.y - line_drag_off_.y;
        nx = SnapLine(nx, size.x, i, false);
        ny = SnapLine(ny, size.y, i, true);
        const ImVec2 clamped = ro::ClampWindowPosToScreen(ImVec2(nx, ny), size);
        line.x = static_cast<int>(clamped.x + 0.5f);
        line.y = static_cast<int>(clamped.y + 0.5f);
      }
      if (line_drag_ == i && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        line_drag_ = -1;
        dirty = true;
      }

      // Clic droit : la remettre au carnet. Pas de croix : elle mangerait la
      // moitié d'une ligne qu'on veut justement minuscule.
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        lines_.erase(lines_.begin() + i);
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
        return;  // la liste a bougé : on reprendra à la frame suivante
      }
      if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
  }

  if (dirty) {
    if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
  }
}

// Le créneau passe-t-il le filtre ? Vrai quand le filtre est vide ou éteint.
//
// Comparaison sans casse sur le NOM et la CARTE : c'est par l'un ou par l'autre
// qu'on cherche un MVP (« bapho », « gef_dun »), et demander au joueur de savoir
// lequel avant de taper serait pénible pour rien. Un créneau scripté n'a pas de
// nom de catalogue : son libellé maison est testé à sa place.
bool MvpTrackerWindow::MatchesFilter(const mvp::Slot& slot) const {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  if (state == nullptr || !state->config().show_filter) return true;
  if (filter_buf_[0] == '\0') return true;

  const char* name = slot.name[0] != '\0' ? slot.name : SlotLabel(slot, nullptr);

  auto contains_ci = [](const char* hay, const char* needle) {
    if (hay == nullptr || needle == nullptr) return false;
    for (; *hay != '\0'; ++hay) {
      const char* h = hay;
      const char* n = needle;
      while (*n != '\0' && *h != '\0' &&
             std::tolower(static_cast<unsigned char>(*h)) ==
                 std::tolower(static_cast<unsigned char>(*n))) {
        ++h;
        ++n;
      }
      if (*n == '\0') return true;
    }
    return false;
  };

  return contains_ci(name, filter_buf_) || contains_ci(slot.map, filter_buf_);
}

void MvpTrackerWindow::DrawTable() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();

  if (!state->catalog_known()) {
    // Une table vide voudrait dire « rien à chasser » : on dit plutôt la vérité.
    ImGui::TextDisabled("%s", i18n::Tr("Catalogue non reçu — en attente du serveur."));
    return;
  }

  // 🔴 SANS GROUPE, PAS DE TABLE.
  //
  // Les observations appartiennent à un GROUPE : sans lui, aucune ligne ne se
  // remplira jamais. Afficher les quatre-vingts créneaux en « jamais vu »
  // promettait donc un outil qui marche tout seul — et laissait le joueur
  // attendre des heures une colonne qui ne bougerait pas.
  //
  // On montre à la place ce qu'il faut faire. Le panneau de groupe juste
  // au-dessus porte déjà le champ de création : on n'en fait pas un second, on
  // explique pourquoi il est là.
  if (state->group().group_id == 0) {
    ImGui::Spacing();
    ImGui::TextWrapped("%s", i18n::Tr(
              "Le carnet se remplit de ce que VOTRE GROUPE observe : ses kills, les "
              "tombes qu'il lit, les Convex Mirror qu'il porte. Sans groupe, aucune "
              "ligne ne se remplira."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", i18n::Tr(
              "Créez-en un ci-dessus — vous pouvez y chasser seul, et inviter plus "
              "tard vos amis et vos compagnons de guilde."));
    return;
  }

  if (state->config().show_filter) {
    ImGui::SetNextItemWidth(ro::Px(220));
    ImGui::InputTextWithHint("##mvp_filter", i18n::Tr("Filtrer : nom ou carte"),
                             filter_buf_, sizeof(filter_buf_));
    if (filter_buf_[0] != '\0') {
      SameLine();
      if (ro::RoSmallButton(i18n::Tr("Effacer"))) filter_buf_[0] = '\0';
    }
  }

  const int64_t now = state->ServerNow();

  // `Hideable` : le clic droit sur la ligne d'en-tête donne la liste des
  // colonnes, à cocher. C'est ce qui rend « Délai » optionnel sans inventer un
  // réglage de plus — et ça vaut pour toutes les colonnes, pas seulement elle.
  // ImGui persiste ces choix dans imgui.ini avec la position de la fenêtre.
  constexpr ImGuiTableFlags kFlags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
      ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY |
      ImGuiTableFlags_Hideable | ImGuiTableFlags_Reorderable |
      ImGuiTableFlags_SizingStretchProp;

  // ⚠ HUIT colonnes, et ce nombre doit s'accorder EXACTEMENT avec la suite des
  // TableSetupColumn : une de trop et ImGui hurle « called too many times »,
  // après quoi tous les TableSetColumnIndex partent en « invalid column index ».
  //
  // « Délai » est DÉCLARÉE mais masquée par défaut : une colonne cachée garde
  // son index, ce qui évite de renuméroter les suivantes selon un réglage — la
  // renumérotation manuelle est précisément ce qui a produit l'erreur ci-dessus.
  if (!ImGui::BeginTable("##mvp_slots", 8, kFlags)) return;

  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("##grip", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, ro::Px(14));
  // Assez large pour l'art du favori (20 px) et sa marge.
  // TRIABLE : cliquer son en-tête remonte les favoris. C'est le tri le plus
  // utile de la table — « qu'est-ce que je surveille, déjà ? ».
  ImGui::TableSetupColumn("##fav", ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthFixed, ro::Px(26));
  ImGui::TableSetupColumn(i18n::Tr("MVP"), ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_DefaultSort);
  ImGui::TableSetupColumn(i18n::Tr("Carte"), ImGuiTableColumnFlags_NoHide);
  // Le délai HABITUEL, c'est-à-dire la LOI : `delay1` et `delay1 + delay2`, les
  // deux chiffres écrits dans le script de spawn et que le serveur publie sans
  // rien révéler du tirage. Masquée par défaut — c'est une donnée de référence,
  // pas une échéance ; on l'affiche quand on prépare une chasse.
  ImGui::TableSetupColumn(i18n::Tr("Délai"),
                          ImGuiTableColumnFlags_WidthFixed |
                              ImGuiTableColumnFlags_DefaultHide,
                          ro::Px(100));
  ImGui::TableSetupColumn(i18n::Tr("Retour"), ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthFixed, ro::Px(150));
  ImGui::TableSetupColumn(i18n::Tr("Âge"), ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthFixed, ro::Px(70));
  ImGui::TableSetupColumn(i18n::Tr("Source"), ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_WidthFixed, ro::Px(70));
  ImGui::TableHeadersRow();

  // Un index trié plutôt qu'une copie du catalogue : le tri d'ImGui remplace à
  // la fois l'ORDER BY du prototype et son bricolage de ré-affichage d'en-tête
  // tous les 31 rangs.
  // Vue mémorisée d'une frame à l'autre (ui/table_view.h) : le filtrage reste
  // refait à chaque frame — il est O(n) et change la LISTE, qui est comparée en
  // entier — mais le tri, lui, ne l'est que quand quelque chose a bougé.
  //
  // 🔴 L'empreinte doit porter les colonnes qui ne sont PAS dans le catalogue :
  // le favori (réglage local), la borne basse de la fenêtre de retour et la date
  // de la dernière observation, que le serveur pousse à tout moment. Ce sont
  // trois lectures de plus par ligne et par frame, contre les O(n log n) que le
  // tri en faisait — le compte est largement gagnant.
  const std::vector<mvp::Slot>& catalogue = state->slots();
  static ro::SortedView s_view;
  s_view.Begin(catalogue.size());
  for (size_t i = 0; i < catalogue.size(); ++i) {
    const mvp::Slot& slot = catalogue[i];
    // Le filtre porte sur le NOM et la CARTE, sans casse : c'est par l'un ou
    // l'autre qu'on cherche un MVP, et exiger de savoir lequel serait pénible.
    if (!MatchesFilter(slot)) continue;
    int64_t window_from = 0;
    state->Window(slot.slot_id, &window_from, nullptr, nullptr);
    const mvp::Obs* obs = state->FindObs(slot.slot_id);
    s_view.Push(static_cast<int>(i),
                ro::Fingerprint(slot.slot_id, slot.delay1_ms,
                                state->IsFavorite(slot.slot_id) ? 1 : 0,
                                window_from,
                                obs != nullptr ? obs->reported_at : 0));
  }

  // ⚠ `End` mémorise la passe : évalué à CHAQUE frame, donc en tête du `&&`.
  ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs();
  if (s_view.End(ro::TableSortKey(specs)) && specs != nullptr &&
      specs->SpecsCount > 0) {
      const ImGuiTableColumnSortSpecs& spec = specs->Specs[0];
      const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;

      // 🔴 On trie sur le nom AFFICHÉ, jamais sur `slot.name`. Celui du
      // catalogue est VIDE pour les quatre créneaux scriptés — Bio Lab 3 et 4,
      // Lord of Death, Thanatos — parce que leur monstre change à chaque
      // cycle : c'est `SlotLabel` qui leur donne un nom, depuis la table d'art
      // du client. Comparer le champ brut mettait donc ces quatre-là en tête
      // de toute la table (la chaîne vide précède tout), à un endroit où rien
      // ne les explique.
      //
      // ⚠ Deux appels vivants dans la même expression : c'est permis parce que
      // `WireToUtf8` écrit dans un anneau de HUIT tampons. Avec un tampon
      // unique, on comparerait la chaîne à elle-même et le tri serait muet.
      auto by_label = [&](const mvp::Slot* a, const mvp::Slot* b) {
        return std::strcmp(SlotLabel(*a, nullptr), SlotLabel(*b, nullptr));
      };
      std::vector<int>& order = s_view.mutable_order();
      std::stable_sort(
          order.begin(), order.end(),
          [&](int ia, int ib) {
            const mvp::Slot* const a = &catalogue[ia];
            const mvp::Slot* const b = &catalogue[ib];
            int cmp = 0;
            switch (spec.ColumnIndex) {
              case 1: {
                // Favoris. Ascendant = favoris EN TÊTE : c'est le sens qu'on
                // attend d'un premier clic, et l'inverse serait une curiosité.
                const bool fa = state->IsFavorite(a->slot_id);
                const bool fb = state->IsFavorite(b->slot_id);
                if (fa != fb) return asc ? fa : fb;
                cmp = by_label(a, b);  // départage stable
                break;
              }
              case 4:
                // Délai : la borne BASSE, celle qui dit « au plus tôt ».
                cmp = a->delay1_ms < b->delay1_ms ? -1
                                                  : (a->delay1_ms > b->delay1_ms ? 1 : 0);
                break;
              case 3:
                cmp = std::strcmp(a->map, b->map);
                break;
              case 5: {
                // Trier par « retour » = trier par la borne BASSE de la fenêtre.
                // Un créneau sans observation part en fin de liste, quel que soit
                // le sens : il n'a pas d'échéance, pas une échéance infinie.
                int64_t fa = 0, fb = 0;
                const bool ka = state->Window(a->slot_id, &fa, nullptr, nullptr);
                const bool kb = state->Window(b->slot_id, &fb, nullptr, nullptr);
                if (!ka || !kb) return ka && !kb;
                cmp = fa < fb ? -1 : (fa > fb ? 1 : 0);
                break;
              }
              case 6: {
                const mvp::Obs* oa = state->FindObs(a->slot_id);
                const mvp::Obs* ob = state->FindObs(b->slot_id);
                if (oa == nullptr || ob == nullptr) return oa != nullptr && ob == nullptr;
                cmp = oa->reported_at > ob->reported_at ? -1
                      : (oa->reported_at < ob->reported_at ? 1 : 0);
                break;
              }
              default:
                cmp = by_label(a, b);
                break;
            }
            return asc ? cmp < 0 : cmp > 0;
          });
  }
  // L'ordre d'affichage : des INDICES dans le catalogue, jamais des pointeurs
  // (la vue survit d'une frame à l'autre, cf. ui/table_view.h).
  const std::vector<int>& rows = s_view.order();

  // 🔴🔴 DÉCOUPAGE OBLIGATOIRE, et pas pour la seule vitesse d'affichage.
  //
  // Le cache de sprites est global et mémorisé par chemin, mais il a un PLAFOND
  // — 48 Mo, `kCacheBudget` dans ui/sprite_view.cc — avec éviction. Dessiner les
  // quatre-vingts MVP à chaque frame le fait déborder en permanence : il évince
  // puis recharge en boucle, et le jeu se fige. Ce n'est donc pas une
  // optimisation, c'est la condition pour que la colonne de sprites existe.
  //
  // Hauteur donnée explicitement : elle est uniforme (le sprite la fixe), ce qui
  // évite au clipper sa passe de mesure et garde le défilement exact.
  const float row_h =
      (state->config().show_sprites ? ro::Px(static_cast<float>(kMvpSpritePx))
                                    : ImGui::GetTextLineHeight()) +
      ImGui::GetStyle().CellPadding.y * 2.0f;

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(rows.size()), row_h);

  // 🔴 Le rang visé par `OpenOn` doit Être SOUMIS même hors écran : un clipper
  // ne dessine que le visible, et `SetScrollHereY` ne peut pas viser un rang
  // qui n'a pas été dessiné. `IncludeItemByIndex` l'exempte pour cette frame,
  // le temps qu'il donne sa position au défilement.
  int focus_row = -1;
  if (focus_slot_ != 0xFFFF && !focus_scrolled_) {
    for (size_t i = 0; i < rows.size(); ++i) {
      if (catalogue[rows[i]].slot_id == focus_slot_) {
        focus_row = static_cast<int>(i);
        clipper.IncludeItemByIndex(focus_row);
        break;
      }
    }
    // Introuvable (filtre actif, catalogue plus court) : on n'insiste pas.
    if (focus_row < 0) focus_slot_ = 0xFFFF;
  }
  while (clipper.Step())
  for (int row_index = clipper.DisplayStart; row_index < clipper.DisplayEnd; ++row_index) {
    const mvp::Slot* slot = &catalogue[rows[static_cast<size_t>(row_index)]];
    const mvp::Obs* obs = state->FindObs(slot->slot_id);
    ImGui::TableNextRow();
    // 🔴 Les noms de MVP sont dupliqués (Atroce sur quatre cartes) : sans un ID
    // par rang, deux lignes partageraient leurs widgets.
    //
    // 🔴🔴 Et le rang est poussé sous un NOM avant son numéro. `PushID(int)`
    // mélange l'entier au sceau courant ; à la racine de la fenêtre ce sceau est
    // le même que celui sous lequel `TableHeadersRow` pose son propre
    // `PushID(column_n)`. Le slot 1 rendait donc EXACTEMENT l'identifiant de la
    // colonne 1, et son étoile « ##fav » entrait en collision avec l'en-tête des
    // favoris (idem slot 0 contre la poignée). Le nom déplace le sceau des rangs
    // hors de portée des index de colonnes.
    ImGui::PushID("slot");
    ImGui::PushID(static_cast<int>(slot->slot_id));

    // Le créneau qu'un lien de chat vient de désigner : on l'amène à l'écran
    // une seule fois, puis on le teinte quelques secondes pour que l'œil le
    // retrouve. Au-delà, la mise en avant s'efface d'elle-même : elle répond à
    // un geste, elle n'est pas un état du carnet.
    if (focus_slot_ != 0xFFFF && slot->slot_id == focus_slot_) {
      if (!focus_scrolled_) {
        ImGui::SetScrollHereY(0.4f);
        focus_scrolled_ = true;
      }
      const unsigned age = GetTickCount() - focus_ms_;
      if (age < 6000u) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                               ImGui::GetColorU32(ImVec4(0.85f, 0.72f, 0.30f, 0.22f)));
      } else {
        focus_slot_ = 0xFFFF;
      }
    }

    ImGui::TableSetColumnIndex(0);
    // ── La POIGNÉE de détachement ──────────────────────────────────────────
    // Une colonne à elle, et pas le nom du MVP : celui-ci est un LIEN, dont le
    // clic gauche ouvre la fiche. Les deux gestes se disputeraient le même
    // bouton, et le perdant serait toujours le même — celui qu'on vient de
    // faire par erreur.
    {
      const float grip_h = ImGui::GetTextLineHeight();
      const ImVec2 g0 = ImGui::GetCursorScreenPos();
      ImGui::InvisibleButton("##grip", ImVec2(ro::Px(10.0f), grip_h));
      const bool grip_hovered = ImGui::IsItemHovered();
      if (grip_hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        drag_slot_ = slot->slot_id;

      // Six points, peints à la main : aucun glyphe au-dessus de U+00FF ne
      // traverse la police du projet, et « :: » serait illisible à cette taille.
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImU32 dot = ImGui::GetColorU32(grip_hovered ? ImGuiCol_Text
                                                        : ImGuiCol_TextDisabled);
      const float step = grip_h / 4.0f;
      for (int r = 1; r <= 3; ++r) {
        dl->AddCircleFilled(ImVec2(g0.x + ro::Px(3.0f), g0.y + step * r), 1.2f, dot);
        dl->AddCircleFilled(ImVec2(g0.x + ro::Px(7.0f), g0.y + step * r), 1.2f, dot);
      }
      if (grip_hovered)
        ImGui::SetTooltip("%s", i18n::Tr("Glisser hors du carnet pour détacher la ligne."));
    }

    ImGui::TableSetColumnIndex(1);
    if (DrawFavoriteButton(state->IsFavorite(slot->slot_id)))
      state->SetFavorite(slot->slot_id, !state->IsFavorite(slot->slot_id));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", i18n::Tr("Favori : alerte à l'ouverture de la fenêtre."));

    ImGui::TableSetColumnIndex(2);
    // Deux ids et pas un : celui qu'on PEINT tourne sur un créneau scripté,
    // celui qu'on DÉSIGNE reste stable. Cliquer une cible qui change sous le
    // curseur serait le meilleur moyen d'ouvrir la fiche d'un autre monstre.
    const uint16_t sprite_mob = SlotSpriteId(*slot);
    const uint16_t link_mob   = SlotLinkId(*slot, obs);

    if (state->config().show_sprites && sprite_mob != 0) {
      DrawRowSprite(sprite_mob, kMvpSpritePx, state->config().animate_sprites);
      ImGui::SameLine();
    }

    if (link_mob != 0) {
      // 🔴 Un lien de monstre COMPLET, pas un texte : on hérite d'un coup de
      // l'aperçu au survol (sprite, niveau, race, élément, PV), du clic gauche
      // qui ouvre la fiche, du Maj+clic qui pose le lien dans le chat, et du
      // menu contextuel — « Fiche du monstre », « Bestiaire du site »,
      // « Où le trouver », @mobinfo, @whereis. Rien de tout cela n'est réécrit
      // ici : c'est le même code que les liens du chat et des descriptions,
      // donc les mêmes gestes partout.
      // 🔴 Le lien reste un lien de MONSTRE — on garde ses six actions — mais
      // il PORTE en plus l'observation du créneau. C'est ce qui fait apparaître
      // « Partager le respawn » dans son menu, et uniquement ici : partout
      // ailleurs (chat, table des drops, fiche) ces champs valent zéro et
      // l'entrée n'existe pas. Le carnet est le seul endroit qui SAIT une heure
      // de mort ; c'est donc le seul d'où l'on puisse la partager.
      links::Target name_link = links::FromMob(link_mob, /*rank=*/2,
                                               SlotLabel(*slot, obs));
      name_link.navi_map   = slot->map;
      name_link.mvp_d1_min = static_cast<uint16_t>(slot->delay1_ms / 60000u);
      name_link.mvp_d2_min = static_cast<uint16_t>(slot->delay2_ms / 60000u);
      if (obs != nullptr) {
        name_link.mvp_kill   = obs->kill_time;
        name_link.mvp_resp   = obs->exact_respawn;
        name_link.mvp_tomb_x = obs->tomb_x;
        name_link.mvp_tomb_y = obs->tomb_y;
      }
      links::Label(name_link, SlotLabel(*slot, obs), menu_);
    } else {
      // Créneau scripté jamais observé : aucun mob à désigner, donc aucun lien
      // à promettre. Un lien mort serait pire qu'un libellé simple.
      ImGui::TextUnformatted(SlotLabel(*slot, obs));
    }

    ImGui::TableSetColumnIndex(3);
    ImGui::TextUnformatted(slot->map);
    // 🔴 `LabelHovered()` ne vaut que pour le dernier LIEN : ici on est sur un
    // texte ordinaire, `IsItemHovered` est donc le bon test.
    if (ImGui::IsItemHovered()) DrawMapPreview(*slot, obs);

    // Le DÉLAI HABITUEL — la loi, pas le tirage. Colonne masquée par défaut :
    // `TableSetColumnIndex` rend false quand elle l'est, et on ne dessine rien.
    if (ImGui::TableSetColumnIndex(4)) {
      const int lo = static_cast<int>(slot->delay1_ms / 60000);
      const int hi = static_cast<int>((slot->delay1_ms + slot->delay2_ms) / 60000);
      if (lo == hi) ImGui::Text(i18n::Tr("%d min"), lo);
      else          ImGui::Text("%d-%d min", lo, hi);
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", i18n::Tr(
                  "Le délai écrit dans le script de spawn : au plus tôt, au plus tard.\n\n"
                  "C'est une donnée publique, et la seule que le serveur ait le droit de "
                  "publier — le tirage exact à l'intérieur de cette plage, lui, ne sort "
                  "que si un membre l'a payé avec un Convex Mirror."));
      }
    }

    ImGui::TableSetColumnIndex(5);
    int64_t from = 0, to = 0;
    bool exact = false;
    const bool window_known = state->Window(slot->slot_id, &from, &to, &exact);
    if (!window_known) {
      // Rien d'observé : on n'écrit RIEN, alors même que le serveur connaît le
      // tirage. C'est la règle de non-triche, visible à l'œil nu.
      ImGui::TextDisabled("%s", i18n::Tr("jamais vu"));
    } else if (exact) {
      char buf[32];
      FormatDuration(from - now, buf, sizeof(buf));
      if (from <= now)
        ImGui::TextUnformatted(i18n::Tr("de retour"));
      else
        ImGui::Text(i18n::Tr("dans %s (exact)"), buf);
    } else if (to <= now) {
      ImGui::TextUnformatted(i18n::Tr("fenêtre passée"));
    } else if (from <= now) {
      char buf[32];
      FormatDuration(to - now, buf, sizeof(buf));
      // 🔴 Vert FONCÉ, pas pastel. Le corps d'une fenêtre RO est CLAIR : un
      // (0.5, 1.0, 0.5) y est presque blanc, et la seule ligne qui doive sauter
      // aux yeux était la moins lisible de la table. Même paire que la fiche de
      // personnage — vert 0.10/0.50/0.15, rouge 0.60/0.12/0.12.
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.10f, 0.50f, 0.15f, 1.0f));
      ImGui::Text(i18n::Tr("possible (jusqu'à %s)"), buf);
      ImGui::PopStyleColor();
    } else {
      char a[32], b[32];
      FormatDuration(from - now, a, sizeof(a));
      FormatDuration(to - now, b, sizeof(b));
      ImGui::Text("%s – %s", a, b);
    }

    ImGui::TableSetColumnIndex(6);
    if (obs != nullptr) {
      char buf[32];
      FormatDuration(now - obs->reported_at, buf, sizeof(buf));
      ImGui::TextDisabled("%s", buf);
    }

    ImGui::TableSetColumnIndex(7);
    // Un bouton plutôt qu'un clic droit sur du texte : quand aucune observation
    // n'existe, la cellule serait vide et `IsItemHovered` désignerait l'item
    // PRÉCÉDENT — donc une autre colonne, voire une autre ligne.

    // 🔴 Une observation DÉPENSÉE — fenêtre refermée, ou instant exact
    // dépassé — n'attend plus qu'on la lise : elle attend qu'on la REMPLACE.
    // Le MVP est revenu, et il est peut-être déjà retombé. Annoncer « tué »
    // sur une mort dont plus rien ne découle donne à la ligne un air de
    // renseignement à jour ; ce bouton est le seul geste qui la remette en
    // état, autant qu'il le dise.
    //
    // Ce que l'observation FUT n'est pas perdu pour autant : l'infobulle le
    // porte, et c'est sa place — une archive se consulte, elle n'invite pas.
    // La colonne « Retour » dit déjà « fenêtre passée » juste à côté, et
    // « Âge » l'ancienneté : la ligne reste lisible d'un bout à l'autre.
    //
    // ⚠ Sans effet sur le TRI : la colonne Source n'a pas de cas dans le
    // comparateur, elle retombe sur le nom. Un libellé qui change ne
    // réordonne donc rien.
    const bool spent = window_known && (exact ? from <= now : to <= now);

    if (ro::RoSmallButton((obs != nullptr && !spent) ? SourceLabel(obs->source)
                                                     : i18n::Tr("saisir"))) {
      manual_slot_ = slot->slot_id;
      manual_time_buf_[0] = '\0';
      open_manual_popup_ = true;
    }
    if (ImGui::IsItemHovered()) {
      // QUI l'affirme, quand le serveur nous l'a dit. C'est ce qui sépare une
      // observation d'une rumeur : « tué » ne veut pas dire grand-chose, « tué
      // par Stingor » se vérifie.
      //
      // 🔴 La SOURCE est nommée ici dès qu'une observation existe, et pas
      // seulement quand un nom l'accompagne : c'est devenu la seule place où
      // elle se lit une fois la fenêtre passée, le bouton disant alors
      // « saisir ».
      if (obs == nullptr) {
        ImGui::SetTooltip("%s", i18n::Tr("Saisir l'heure de mort observée."));
      } else if (obs->by_name[0] != '\0') {
        ImGui::SetTooltip(i18n::Tr("%s — d'après %s.\nCliquer pour saisir une heure."),
                          SourceLabel(obs->source), ro::WireToUtf8(obs->by_name));
      } else {
        ImGui::SetTooltip(i18n::Tr("%s.\nCliquer pour saisir une heure."),
                          SourceLabel(obs->source));
      }
    }

    ImGui::PopID();  // slot_id
    ImGui::PopID();  // "slot"
  }

  ImGui::EndTable();

  // 🔴 OpenPopup APRÈS EndTable et HORS du PushID des rangs : l'identifiant d'un
  // popup incorpore la pile d'ID courante, si bien qu'ouvert sous PushID(slot),
  // il ne serait jamais retrouvé par le BeginPopup d'en dessous. Le menu des
  // liens obéit à la même contrainte, d'où son ancre différée dessinée ici.
  menu_.Draw("##mvp_link_menu");

  if (open_manual_popup_) {
    ImGui::OpenPopup("##mvp_manual");
    open_manual_popup_ = false;
  }
}

void MvpTrackerWindow::DrawManualPopup() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();

  if (!ImGui::BeginPopup("##mvp_manual")) return;

  const mvp::Slot* slot = state->FindSlot(manual_slot_);
  if (slot == nullptr) {
    ImGui::EndPopup();
    return;
  }

  ImGui::TextUnformatted(i18n::Tr("Heure de mort observée"));
  ImGui::SetNextItemWidth(ro::Px(120));
  ImGui::InputTextWithHint("##mvp_manual_time", "1430 / 14h30 / -2350",
                           manual_time_buf_, sizeof(manual_time_buf_));
  ImGui::TextDisabled("%s", i18n::Tr("Un « - » de tête désigne la veille."));

  const int64_t when = ParseKillTime(manual_time_buf_);
  if (manual_time_buf_[0] != '\0' && when == 0)
    ImGui::TextDisabled("%s", i18n::Tr("Heure illisible."));

  if (ro::RoButton(i18n::Tr("Envoyer")) && when != 0) {
    // La source « saisie » est la plus faible : elle n'écrasera jamais un kill
    // ni un miroir. L'arbitrage est au serveur, pas ici.
    state->ReportManual(manual_slot_, when);
    manual_time_buf_[0] = '\0';
    ImGui::CloseCurrentPopup();
  }

  ImGui::EndPopup();
}

void MvpTrackerWindow::DrawAlerts() {
  MvpTracker* state = Bourgeon::Instance().mvp_tracker();
  const int64_t now = state->ServerNow();
  const int64_t lead = static_cast<int64_t>(state->config().alert_lead_min) * 60;

  for (uint16_t slot_id : state->favorites()) {
    int64_t from = 0;
    bool exact = false;
    if (!state->Window(slot_id, &from, nullptr, &exact)) continue;
    if (now + lead < from) continue;   // pas encore
    if (now > from + 3600) continue;   // trop vieux pour être une nouvelle

    const mvp::Obs* obs = state->FindObs(slot_id);
    if (obs == nullptr) continue;

    // Une seule sonnerie par OBSERVATION : une observation plus fraîche (un
    // miroir qui précise un kill) a le droit de re-sonner, la même non.
    bool already = false;
    for (int i = 0; i < fired_count_; ++i) {
      if (fired_[i].slot_id == slot_id && fired_[i].reported_at == obs->reported_at) {
        already = true;
        break;
      }
    }
    if (already) continue;

    if (fired_count_ < kMaxAlerts) {
      fired_[fired_count_++] = {slot_id, obs->reported_at};
    } else {
      // Table pleine : on écrase la plus ancienne entrée, la fenêtre d'alerte
      // ne dure de toute façon qu'une heure.
      std::memmove(fired_, fired_ + 1, sizeof(FiredAlert) * (kMaxAlerts - 1));
      fired_[kMaxAlerts - 1] = {slot_id, obs->reported_at};
    }

    alert_slot_ = slot_id;
    alert_ms_ = GetTickCount();
    if (state->config().alert_sound) MessageBeep(MB_ICONASTERISK);
  }

  if (alert_slot_ == 0xFFFF || GetTickCount() - alert_ms_ > kAlertShowMs) return;

  const mvp::Slot* slot = state->FindSlot(alert_slot_);
  if (slot == nullptr) return;

  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(
      ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
             viewport->WorkPos.y + ro::Px(90)),
      ImGuiCond_Always, ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.85f);

  if (ImGui::Begin("##mvp_alert", nullptr,
                   ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_NoNav)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
    ImGui::Text(i18n::Tr("%s (%s) entre dans sa fenêtre."),
                SlotLabel(*slot, state->FindObs(alert_slot_)), slot->map);
    ImGui::PopStyleColor();
  }
  ImGui::End();
}
