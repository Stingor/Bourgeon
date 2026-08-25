#include "features/windows/macro_window.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/windows/hotkey_settings.h"
#include "imgui.h"
#include "imgui_internal.h"  // ClearActiveID (cf. SetRowText)
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"
#include "ragnarok/user_hotkey.h"
#include "ui/game_emotes.h"  // le catalogue d'émotes du sélecteur de la chatbox
#include "ui/ro_imgui.h"
#include "utils/i18n.h"
#include "utils/log_console.h"

namespace {

// Le titre de la fenêtre, tel que le client le dessine : MSI_SHORTCUT_LIST.
constexpr int kMsgWindowTitle = 574;

// L'onglet « Macros » de l'écran des raccourcis. 🔴 C'est un rang d'ONGLET, pas
// une catégorie : l'ordre visuel du client ne suit pas la numérotation Lua
// (onglet 3 -> catégorie 2). Cf. `userhotkey::CategoryForTab`.
constexpr int kHotkeyTabMacros = 3;

// 🔴 Texte secondaire : couleur EXPLICITE, jamais `ImGui::TextDisabled`. Le corps
// d'une fenêtre RO est CLAIR, et le gris de TextDisabled y est illisible
// (feedback_imgui_ro_light_body_colors).
const ImVec4 kSecondaryText(0.42f, 0.38f, 0.32f, 1.0f);
// Le compteur quand la ligne est pleine : la troncature du client est silencieuse,
// c'est le seul endroit où elle peut se voir.
const ImVec4 kFullText(0.70f, 0.20f, 0.15f, 1.0f);

// ── Le catalogue de commandes du menu contextuel ─────────────────────────────
//
// 🔴 RELEVÉ SUR LE SERVEUR, PAS DEVINÉ. Sources, dans le dépôt `moonlight` :
//   · `conf/atcommands.yml`        — les commandes de rAthena et leurs alias
//     (`gstorage` = `guildstorage`, `noks` = `ksprotection`) ;
//   · `conf/import/atcommands.yml` — les alias PROPRES à Moonlight, dont
//     `storage1..5` = `storagealt1..5` (« storage alternatif »), le 5 servant
//     aux cartes ;
//   · `src/custom/atcommand.inc`   — les commandes maison : `@storeall` y prend
//     un numéro de storage 0..5, `@autolootpognon` un PRIX PLANCHER (« reset »
//     l'annule), `@autolootrare` ne ramasse que les objets d'une liste serveur.
// ⚠ Le fichier de base ment quand `conf/import/` le surcharge — c'est le cas ici
// pour les storages alternatifs (cf. feedback_rathena_conf_import_overrides).
//
// Ce ne sont pas des commandes du CLIENT : elles dépendent du serveur et des
// permissions du joueur. On ne les filtre pas — préremplir n'est pas exécuter, et
// un refus serveur se lit dans le chat.
struct ServerCmd {
  const char* text;  // ce qui atterrit dans la macro
  const char* desc;  // description FR, passée à i18n::Tr
};

const ServerCmd kLootCmds[] = {
    {"@autoloot", "Les objets ramassés vont droit dans l'inventaire."},
    {"@autolootmvp", "Ramasse aussi les récompenses des MVP."},
    {"@autolootpognon 20000", "Ne ramasse que les objets valant au moins ce prix."},
    {"@autolootrare", "Ne ramasse que les objets rares listés par le serveur."},
};

// Le fourre-tout : trois commandes de confort qui n'ont rien à voir entre elles.
// Elles ont d'abord été rangées sous « Déplacement », ce qui ne valait que pour
// `@load` — `@noks` est une règle de combat et `@refresh` un rafraîchissement
// d'affichage.
const ServerCmd kOtherCmds[] = {
    {"@load", "Téléporte à votre point de sauvegarde."},
    {"@noks", "Bascule la protection contre le vol de kill."},
    {"@refresh", "Resynchronise votre position à l'écran."},
};

const ServerCmd kInfoCmds[] = {
    {"@uptime", "Depuis combien de temps le serveur tourne."},
    {"@time", "Date et heure du serveur."},
    {"@who", "Liste les joueurs connectés, avec leur groupe et leur guilde."},
};

// Le storage principal et celui de guilde. Les cinq alternatifs sont ajoutés
// juste après, par une boucle : leurs libellés ne diffèrent que par le numéro.
const ServerCmd kStorageCmds[] = {
    {"@storage", "Ouvre le storage."},
    {"@storeall", "Envoie tout l'inventaire au storage."},
    {"@gstorage", "Ouvre le storage de guilde."},
};

// Le storage alternatif 5 est celui des cartes (alias `storagecard` côté serveur).
constexpr int kAltStorageCount = 5;
constexpr int kCardStorage = 5;

// ── Émote → commande `/xx` du client ─────────────────────────────────────────
//
// 🔴 UNE MACRO DOIT CONTENIR CE QUE LE CLIENT D'ORIGINE COMPREND. Le jeton
// `:sweat:` de la chatbox ne convient PAS : il n'est du texte rendu en image que
// chez Bourgeon, donc une macro qui l'emploie n'affiche rien chez les autres.
// Ce qui joue la bulle au-dessus de la tête, partout, c'est la commande `/swt`.
//
// La table de messages du client porte ces commandes dans un bloc de TRENTE ids
// CONTIGUS, 544 à 573 (`/!` … `/ok`). Le sprite `emotion.act`, lui, compte quatre
// actions de plus dans cet intervalle : scissor, rock, wrap et flag — le
// pierre-feuille-ciseaux et le drapeau, qu'aucune commande ne déclenche. D'où
// deux plages, et non un décalage unique :
//
//     sprite 0..10   ->  message 544 + id
//     sprite 11..14  ->  AUCUNE commande (exclus du menu)
//     sprite 15..33  ->  message 540 + id
//     sprite  > 33   ->  aucune commande (au-delà du bloc)
//
// Établi par recoupement, pas deviné : les cardinalités coïncident exactement
// (11 + 19 = 30 commandes pour 30 ids), et les noms concordent sur toute la
// longueur — `/swt`↔sweat, `/ic`↔aha, `/ag`↔anger, `/$`↔money, `/...`↔think,
// `/sry`↔sorry, `/swt2`↔profusely_sweat, `/kis`↔chup, `/kis2`↔chupchup,
// `/ok`↔ok — et `/??`↔stare_about est commenté tel quel dans l'énumération
// `emotion_type` du serveur.
// ⚠ Les deux appariements les moins évidents sont `/ho`↔whistle (id 2) et
// `/lv`↔delight (id 3) : ce sont eux qu'il faut regarder d'abord si une émote
// sortait de travers.
//
// ⚠ Ces trente messages ne doivent JAMAIS être traduits : ce sont des commandes.
// Le garde « commence par / » ci-dessous refuse d'écrire autre chose dans une
// macro si la table venait à l'être.
constexpr int kEmoteCmdMsgFirst = 544;  // « /! »
constexpr int kEmoteCmdMsgLast = 573;   // « /ok »

int EmoteCommandMsgId(int emote_id) {
  if (emote_id >= 0 && emote_id <= 10) return kEmoteCmdMsgFirst + emote_id;
  if (emote_id >= 15 && emote_id <= 33) return 540 + emote_id;
  return -1;
}

const char* TargetLabel(emohotkey::Target target) {
  switch (target) {
    case emohotkey::Target::kParty: return i18n::Tr("Groupe");
    case emohotkey::Target::kGuild: return i18n::Tr("Guilde");
    case emohotkey::Target::kClan:  return i18n::Tr("Clan");
    default:                        return i18n::Tr("Public");
  }
}

}  // namespace

// ── Cycle de vie ─────────────────────────────────────────────────────────────

void MacroWindow::HandleNativeCreation(void* win) {
  if (!imgui_enabled_) return;

  uiwnd::SafeSetVisible(win, false);

  // Le client fait *ferme-si-existe / crée-sinon* : comme la native est détruite
  // au tick, elle n'existe jamais et TOUTE demande repasse ici. C'est donc bien
  // ici qu'on bascule, et un seul point suffit.
  if (open_) {
    Close();
  } else {
    Open();
  }
}

void MacroWindow::Open() {
  open_ = true;
  need_pos_ = true;
  show_panel_ = true;
  loaded_ = false;  // relire AVANT la première frame
  esc_grace_frames_ = 2;
}

void MacroWindow::Close() {
  open_ = false;
  confirm_reset_ = false;
  // Les macros sont déjà dans le vecteur du client (elles y sont parties à la
  // frappe) ; il ne reste qu'à graver. C'est la fermeture qui perdait tout chez le
  // natif — ici elle est justement le moment où l'on force l'écriture.
  if (dirty_) idle_ticks_ = kSaveIdleTicks;
}

void MacroWindow::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type != ModeMgr::ModeType::kGame) {
    // Pas de `Save()` ici : le client en joue un lui-même sur ce chemin
    // (`CMode_SendMsg_Base`, `UINewSelectCharWnd_OnMsg`), et notre vecteur est
    // déjà à jour. Appeler le nôtre en plus écrirait deux fois les mêmes fichiers.
    open_ = false;
    loaded_ = false;
    dirty_ = false;
    pending_send_ = -1;
    pending_reset_ = false;
    pending_open_hotkeys_ = false;
  }
}

void MacroWindow::OnTick() {
  if (!imgui_enabled_) {
    if (open_) Close();
    return;
  }
  if (!Bourgeon::Instance().IsGameActive()) {
    if (open_) open_ = false;
    return;
  }

  // La native est masquée dès sa naissance ; on la DÉTRUIT ici. Masquée, elle
  // continuerait d'avaler un appui sur deux (le client ferme-si-existe) et
  // garderait le clavier (feedback_hidden_native_window_keyboard).
  if (uiwnd::FindWindow(uiwnd::kMacroWndId)) uiwnd::CloseWindow(uiwnd::kMacroWndId);

  if (pending_reset_) {
    pending_reset_ = false;
    for (int i = 0; i < emohotkey::kSlotCount; ++i) {
      const char* def = emohotkey::DefaultLocal(i);
      emohotkey::WriteLocal(i, def ? def : "");
    }
    loaded_ = false;  // relire les tampons depuis le client
    dirty_ = true;
    idle_ticks_ = kSaveIdleTicks;
  }

  if (pending_open_hotkeys_) {
    pending_open_hotkeys_ = false;
    if (auto* hs = Bourgeon::Instance().hotkey_settings())
      hs->OpenFromMenu(kHotkeyTabMacros);
  }

  if (pending_send_ >= 0) {
    const int slot = pending_send_;
    pending_send_ = -1;
    if (!emohotkey::Send(slot))
      LogDiag("MacroWindow: envoi refuse (slot={})", slot);
  }

  // Écriture disque différée. Le vecteur du client, lui, est à jour depuis la
  // frappe : si le joueur quitte avant l'échéance, les sauvegardes du CLIENT
  // enregistrent quand même la bonne valeur.
  if (dirty_) {
    if (++idle_ticks_ >= kSaveIdleTicks) {
      dirty_ = false;
      idle_ticks_ = 0;
      if (!emohotkey::Save()) LogDiag("MacroWindow: echec de la sauvegarde");
    }
  }
}

// ── Données ──────────────────────────────────────────────────────────────────

void MacroWindow::ReloadFromClient() {
  for (int i = 0; i < emohotkey::kSlotCount; ++i) {
    rows_[i][0] = '\0';
    emohotkey::ReadLocal(i, rows_[i], sizeof(rows_[i]));
  }
  loaded_ = true;
}

void MacroWindow::SetRowText(int slot, const char* local) {
  if (slot < 0 || slot >= emohotkey::kSlotCount) return;
  std::snprintf(rows_[slot], sizeof(rows_[slot]), "%s", local ? local : "");
  CommitRow(slot);
}

void MacroWindow::CommitRow(int slot) {
  if (slot < 0 || slot >= emohotkey::kSlotCount) return;
  if (!emohotkey::WriteLocal(slot, rows_[slot])) return;
  dirty_ = true;
  idle_ticks_ = 0;  // repousser l'écriture tant que le joueur tape
}

void MacroWindow::KeyLabel(int slot, char* out, size_t out_size) const {
  if (!out || out_size == 0) return;
  out[0] = '\0';

  // 1. La surcharge du joueur, si `UserKeys.lua` en porte une pour cette ligne.
  //    C'est aussi ce que le natif tente en premier — et il en reste au libellé en
  //    dur dès que la commande n'a pas d'entrée, ce que les deux replis corrigent.
  userhotkey::Binding binding;
  if (userhotkey::ReadBinding(userhotkey::kMacros, slot, &binding) &&
      binding.assigned && binding.key_name[0]) {
    std::snprintf(out, out_size, "%s", binding.key_name);
    return;
  }

  // 2. Le raccourci D'ORIGINE du client pour cette commande.
  const int cmd = userhotkey::CommandIndexAt(userhotkey::kMacros, slot);
  if (cmd >= 0) {
    userhotkey::Binding original;
    if (userhotkey::ReadDefaultBinding(userhotkey::kMacros, cmd, &original) &&
        original.key_name[0]) {
      std::snprintf(out, out_size, "%s", original.key_name);
      return;
    }
  }

  // 3. Le libellé en dur du natif : « Alt + 1 » … « Alt + 9 », puis « Alt + 0 ».
  std::snprintf(out, out_size, "Alt + %d", (slot + 1) % 10);
}

// ── Menu contextuel ──────────────────────────────────────────────────────────

bool MacroWindow::DrawPrefillMenu(int slot) {
  bool changed = false;

  // Rappel de la ligne visée et de ce qu'on va écraser : le menu s'ouvre au clic
  // droit, donc sur une ligne qu'on ne regardait pas forcément.
  char key[64];
  KeyLabel(slot, key, sizeof(key));
  ImGui::PushStyleColor(ImGuiCol_Text, kSecondaryText);
  if (rows_[slot][0]) {
    ImGui::Text("%s  %s", key, ro::LocalToUtf8(rows_[slot]));
  } else {
    ImGui::Text("%s  %s", key, i18n::Tr("(vide)"));
  }
  ImGui::PopStyleColor();
  ImGui::Separator();

  // Un groupe = un sous-menu. Le second argument de MenuItem est rendu grisé et
  // aligné à droite : c'est la colonne prévue pour un raccourci, elle sert ici de
  // colonne d'explication, sans widget supplémentaire.
  struct Group {
    const char* label;
    const ServerCmd* items;
    int count;
  };
  const Group groups[] = {
      {"Ramassage", kLootCmds, IM_ARRAYSIZE(kLootCmds)},
      {"Informations", kInfoCmds, IM_ARRAYSIZE(kInfoCmds)},
      {"Autre", kOtherCmds, IM_ARRAYSIZE(kOtherCmds)},
  };

  for (const Group& group : groups) {
    if (!ImGui::BeginMenu(i18n::Tr(group.label))) continue;
    for (int i = 0; i < group.count; ++i) {
      if (ImGui::MenuItem(group.items[i].text, i18n::Tr(group.items[i].desc))) {
        SetRowText(slot, group.items[i].text);
        changed = true;
      }
    }
    ImGui::EndMenu();
  }

  // Le storage a son propre sous-menu : le principal, celui de guilde, puis les
  // cinq alternatifs par paires « ouvrir / tout envoyer », dans cet ordre parce
  // que c'est ainsi qu'on s'en sert.
  if (ImGui::BeginMenu(i18n::Tr("Storage"))) {
    for (const ServerCmd& cmd : kStorageCmds) {
      if (ImGui::MenuItem(cmd.text, i18n::Tr(cmd.desc))) {
        SetRowText(slot, cmd.text);
        changed = true;
      }
    }
    ImGui::Separator();
    for (int n = 1; n <= kAltStorageCount; ++n) {
      char cmd[32];
      char desc[128];

      std::snprintf(cmd, sizeof(cmd), "@storage%d", n);
      if (n == kCardStorage) {
        std::snprintf(desc, sizeof(desc), "%s",
                      i18n::Tr("Ouvre le storage alternatif 5, celui des cartes."));
      } else {
        std::snprintf(desc, sizeof(desc), i18n::Tr("Ouvre le storage alternatif %d."), n);
      }
      if (ImGui::MenuItem(cmd, desc)) {
        SetRowText(slot, cmd);
        changed = true;
      }

      std::snprintf(cmd, sizeof(cmd), "@storeall %d", n);
      std::snprintf(desc, sizeof(desc),
                    i18n::Tr("Envoie tout l'inventaire au storage alternatif %d."), n);
      if (ImGui::MenuItem(cmd, desc)) {
        SetRowText(slot, cmd);
        changed = true;
      }
    }
    ImGui::EndMenu();
  }

  // ── Les émotes du jeu ─────────────────────────────────────────────────────
  //
  // Les SPRITES viennent du catalogue de la chatbox (`ro::emote`), avec son
  // filtre `Exists(id)` — celui qui écarte les émotes exclues à la main et celles
  // absentes de l'`emotion.act` du joueur.
  //
  // 🔴 Mais ce qu'on ÉCRIT est la commande `/xx` du client, pas le jeton
  // `:sweat:` de la chatbox : une macro doit marcher chez tout le monde, y compris
  // sur le chat d'origine, où `:sweat:` ne serait que du texte. Le menu ne montre
  // donc que les émotes qui ONT une commande — voir `EmoteCommandMsgId` : les
  // quatre du pierre-feuille-ciseaux et le drapeau n'en ont pas, et n'ont rien à
  // faire dans une macro.
  if (ImGui::BeginMenu(i18n::Tr("Émotions"))) {
    // 🔴 La grille est dimensionnée sur SON CONTENU, et rien d'autre. Il y a au
    // plus trente émotes commandables — le bloc de messages en compte trente —
    // donc ça tient toujours en quelques rangées : ni zone défilante, ni barre,
    // ni rangées réservées pour du vide. Une première version en réservait six
    // sur dix colonnes : la moitié basse du panneau restait blanche.
    //
    // Chaque case porte la COMMANDE sous le sprite. C'est elle qui atterrira
    // dans la macro, et beaucoup d'émotes ne se reconnaissent pas à leur seule
    // image en 30 pixels — il fallait survoler chacune pour savoir ce qu'on
    // allait écrire.
    constexpr int kCols = 6;
    const float kSprite = 30.0f;
    const float text_h = ImGui::GetTextLineHeight();
    // Largeur : la plus longue commande du bloc fait cinq caractères (« /swt2 »,
    // « /kis2 »). Mesurée, jamais estimée — la police est un réglage
    // (feedback_ui_width_measured_not_hardcoded).
    const float text_w = ImGui::CalcTextSize("/swt2").x;
    const float cell_w = (text_w > kSprite ? text_w : kSprite) + 6.0f;
    const float cell_h = kSprite + text_h + 2.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 text_col = ImGui::GetColorU32(ImGuiCol_Text);
    const float now = static_cast<float>(ImGui::GetTime());
    int col = 0;
    for (int id = 0; id < ro::emote::Count(); ++id) {
      if (!ro::emote::Exists(id)) continue;
      const int msg_id = EmoteCommandMsgId(id);
      if (msg_id < 0) continue;
      // La commande est lue dans la table du CLIENT, jamais recopiée ici : elle
      // suit ses correctifs et sa langue. Le garde `/` refuse d'écrire dans une
      // macro autre chose qu'une commande, si jamais ces messages étaient
      // traduits par erreur.
      const char* cmd = msgstr::Utf8(msg_id);
      if (!cmd || cmd[0] != '/') continue;

      if (col != 0) ImGui::SameLine();
      ImGui::PushID(id);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const bool clicked = ImGui::InvisibleButton("##e", ImVec2(cell_w, cell_h));

      // Sprite centré en haut de la case, commande centrée dessous.
      const ImVec2 s0(p.x + (cell_w - kSprite) * 0.5f, p.y);
      ro::emote::Draw(dl, id, s0, ImVec2(s0.x + kSprite, s0.y + kSprite), now, true);
      const ImVec2 ts = ImGui::CalcTextSize(cmd);
      dl->AddText(ImVec2(p.x + (cell_w - ts.x) * 0.5f, p.y + kSprite + 1.0f),
                  text_col, cmd);

      if (ImGui::IsItemHovered()) {
        dl->AddRect(p, ImVec2(p.x + cell_w, p.y + cell_h), IM_COL32(40, 40, 40, 160));
        // Le nom du sprite en infobulle : la commande est déjà lisible sous
        // l'image, c'est l'émote elle-même qu'on peut avoir besoin de nommer.
        ImGui::SetTooltip("%s", ro::emote::Name(id));
      }
      if (clicked) {
        SetRowText(slot, cmd);
        changed = true;
      }
      ImGui::PopID();
      if (++col == kCols) col = 0;
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  if (ImGui::MenuItem(i18n::Tr("Vider la ligne"), nullptr, false, rows_[slot][0] != '\0')) {
    SetRowText(slot, "");
    changed = true;
  }

  return changed;
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void MacroWindow::DrawRow(int slot) {
  ImGui::TableNextRow();

  char menu_id[32];
  std::snprintf(menu_id, sizeof(menu_id), "##macro_menu_%d", slot);

  // Ouvre le menu contextuel de la ligne, depuis n'importe lequel de ses items.
  //
  // 🔴 `ClearActiveID()` avant d'ouvrir, et ce n'est PAS décoratif :
  // `ro::InputTextCp949` tient SON PROPRE tampon UTF-8 et ne le re-sème depuis le
  // nôtre que `if (GetActiveID() != id)`. Sur un champ resté actif, le
  // préremplissage serait donc invisible — et la frappe suivante réécrirait
  // l'ancien texte par-dessus le nôtre.
  // ⚠ La recette habituelle (`ReloadUserBufAndMoveToEnd`,
  // feedback_imgui_treenode_refuses_modified_click) ne s'applique pas ici : le
  // tampon qu'ImGui édite n'est pas le nôtre, c'est celui du wrapper.
  auto open_menu_on_right_click = [&]() {
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      ImGui::ClearActiveID();
      ImGui::OpenPopup(menu_id);
    }
  };

  // ── Colonne 1 : la touche, et la poignée de réorganisation ────────────────
  ImGui::TableSetColumnIndex(0);
  char key[64];
  KeyLabel(slot, key, sizeof(key));
  char key_id[80];
  std::snprintf(key_id, sizeof(key_id), "%s###macro_key_%d", key, slot);

  // 🔴 Une source de glisser DOIT porter un identifiant ImGui : depuis un simple
  // `Text()` elle échoue EN SILENCE en Release
  // (feedback_imgui_dragsource_needs_id). D'où le Selectable.
  ImGui::Selectable(key_id, false, ImGuiSelectableFlags_None,
                    ImVec2(0.0f, ImGui::GetFrameHeight()));

  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
    ImGui::SetDragDropPayload("MACROROW", &slot, sizeof(int));
    ImGui::TextUnformatted(rows_[slot][0] ? ro::LocalToUtf8(rows_[slot])
                                          : i18n::Tr("(vide)"));
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MACROROW")) {
      const int from = *static_cast<const int*>(payload->Data);
      if (from >= 0 && from < emohotkey::kSlotCount && from != slot) {
        // Les TEXTES s'échangent, pas les touches : Alt+3 reste la 3ᵉ ligne. Le
        // joueur réordonne ses macros, il ne remappe pas son clavier par mégarde.
        char tmp[sizeof(rows_[0])];
        std::memcpy(tmp, rows_[from], sizeof(tmp));
        std::memcpy(rows_[from], rows_[slot], sizeof(tmp));
        std::memcpy(rows_[slot], tmp, sizeof(tmp));
        CommitRow(from);
        CommitRow(slot);
      }
    }
    ImGui::EndDragDropTarget();
  }
  open_menu_on_right_click();
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Glisser sur une autre ligne pour échanger les deux macros.\n"
                               "Clic droit : commandes du serveur."));
  }

  // ── Colonne 2 : la macro ──────────────────────────────────────────────────
  ImGui::TableSetColumnIndex(1);
  ImGui::SetNextItemWidth(-1.0f);
  char field_id[32];
  std::snprintf(field_id, sizeof(field_id), "##macro_text_%d", slot);
  // Le tampon est en code-page du client d'un bout à l'autre : c'est cette forme
  // qui part dans son `std::string`, sans aller-retour supplémentaire.
  if (ro::InputTextCp949WithHint(field_id,
                                 i18n::Tr("Texte, /commande ou @commande..."),
                                 rows_[slot], sizeof(rows_[slot]))) {
    CommitRow(slot);
  }
  open_menu_on_right_click();

  // ── Colonne 3 : la longueur ───────────────────────────────────────────────
  // Le champ natif tronque à 50 octets sans rien dire ; ici la limite se voit
  // arriver.
  ImGui::TableSetColumnIndex(2);
  const size_t len = std::strlen(rows_[slot]);
  const bool full = (len >= emohotkey::kMaxBytes);
  ImGui::PushStyleColor(ImGuiCol_Text, full ? kFullText : kSecondaryText);
  ImGui::Text("%zu/%zu", len, emohotkey::kMaxBytes);
  ImGui::PopStyleColor();

  // ── Colonne 4 : l'essai ───────────────────────────────────────────────────
  ImGui::TableSetColumnIndex(3);
  ImGui::BeginDisabled(rows_[slot][0] == '\0');
  char send_id[48];
  std::snprintf(send_id, sizeof(send_id), "%s###macro_send_%d",
                i18n::Tr("Essayer"), slot);
  if (ro::RoButton(send_id)) pending_send_ = slot;
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered() && rows_[slot][0])
    ImGui::SetTooltip("%s", i18n::Tr("Envoie la macro tout de suite, sans fermer la fenêtre"));

  // Le menu de la ligne. Rendu en DERNIER, une fois tous ses points d'ouverture
  // soumis — un popup se dessine là où il est appelé, pas là où il est ouvert.
  if (ImGui::BeginPopup(menu_id)) {
    if (DrawPrefillMenu(slot)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void MacroWindow::OnRenderUI() {
  if (!imgui_enabled_ || !open_) return;

  if (esc_grace_frames_ > 0) {
    --esc_grace_frames_;
    ro::SuppressEscapeStack();
  }

  if (!loaded_) {
    if (!emohotkey::Ready()) return;  // pas encore en jeu : rien à montrer
    ReloadFromClient();
  }

  if (need_pos_) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ro::Px(470.0f), ro::Px(360.0f)),
                             ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }

  // Titre : celui du client (« Shortcut List »), suffixe ### figé pour que la
  // fenêtre garde sa position d'une langue à l'autre.
  char title[128];
  std::snprintf(title, sizeof(title), "%s###bourgeon_macro_list",
                msgstr::Utf8Or(kMsgWindowTitle, i18n::Tr("Liste des raccourcis")));

  const bool begun = ro::BeginRoWindow(title, &show_panel_);
  if (!show_panel_) { Close(); show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  // ── Où part une macro ──────────────────────────────────────────────────────
  // Le renseignement que la fenêtre native ne donne nulle part.
  ImGui::PushStyleColor(ImGuiCol_Text, kSecondaryText);
  ImGui::Text("%s", i18n::Tr("Ces macros partent sur :"));
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextUnformatted(TargetLabel(emohotkey::CurrentTarget()));
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("La destination suit l'onglet de chat courant. Une macro qui "
                               "commence par / ou @ n'est pas concernée : c'est une commande."));
  }

  ImGui::Separator();

  // ── Les dix lignes ─────────────────────────────────────────────────────────
  const float footer_h = ImGui::GetFrameHeightWithSpacing() +
                         ImGui::GetStyle().ItemSpacing.y * 2.0f;
  const ImVec2 table_size(0.0f, ImGui::GetContentRegionAvail().y - footer_h);

  const float key_w  = ImGui::CalcTextSize("Ctrl+Shift+F12").x +
                       ImGui::GetStyle().FramePadding.x * 4.0f;
  const float len_w  = ImGui::CalcTextSize("00/00").x +
                       ImGui::GetStyle().FramePadding.x * 4.0f;
  const float send_w = ro::MaxButtonWidth({i18n::Tr("Essayer")});

  if (ImGui::BeginTable("##macro_rows", 4,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_ScrollY,
                        table_size)) {
    ImGui::TableSetupColumn(i18n::Tr("Touche"), ImGuiTableColumnFlags_WidthFixed, key_w);
    ImGui::TableSetupColumn(i18n::Tr("Macro"),  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("##len",  ImGuiTableColumnFlags_WidthFixed, len_w);
    ImGui::TableSetupColumn("##send", ImGuiTableColumnFlags_WidthFixed, send_w);
    ImGui::TableHeadersRow();

    for (int i = 0; i < emohotkey::kSlotCount; ++i) DrawRow(i);
    ImGui::EndTable();
  }

  // ── Pied ───────────────────────────────────────────────────────────────────
  const float keys_w  = ro::MaxButtonWidth({i18n::Tr("Modifier les touches...")});
  const float reset_w = ro::MaxButtonWidth({i18n::Tr("Valeurs par défaut...")});
  const float close_w = ro::MaxButtonWidth({i18n::Tr("Fermer")});

  if (ro::RoButton(i18n::Tr("Modifier les touches..."), keys_w))
    pending_open_hotkeys_ = true;
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("%s",
                      i18n::Tr("Ouvre l'écran des raccourcis à l'onglet Macros, où ces dix "
                               "touches se réaffectent."));
  }

  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Valeurs par défaut..."), reset_w))
    confirm_reset_ = true;

  ImGui::SameLine(ImGui::GetContentRegionMax().x - close_w);
  if (ro::RoButton(i18n::Tr("Fermer"), close_w)) Close();

  // ── Confirmation de la réinitialisation ────────────────────────────────────
  const char* kResetPopup = i18n::Tr("Valeurs par défaut");
  if (confirm_reset_) {
    ImGui::OpenPopup(kResetPopup);
    confirm_reset_ = false;
  }
  if (ro::BeginRoPopupModal(kResetPopup)) {
    ro::SuppressEscapeStack();
    ImGui::TextUnformatted(
        i18n::Tr("Les dix macros vont reprendre les valeurs d'origine du client.\n"
                 "Ce que vous avez écrit sera perdu."));
    ImGui::Separator();
    const float ok_w = ro::MaxButtonWidth({i18n::Tr("OK"), i18n::Tr("Annuler")});
    if (ro::RoButton(i18n::Tr("OK"), ok_w)) {
      // Différé au tick : l'écriture passe par le `std::string::assign` du client.
      // Le modal a déjà retiré le focus au champ de saisie, donc le rechargement
      // des tampons ne se fera pas sous un InputText actif
      // (feedback_imgui_treenode_refuses_modified_click).
      pending_reset_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler"), ok_w)) ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
  }

  ro::EndRoWindow();
}
