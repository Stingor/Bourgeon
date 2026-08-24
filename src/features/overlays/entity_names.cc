#include "ragnarok/globals.h"
#include "features/overlays/entity_names.h"

#include <windows.h>

#include <cfloat>
#include <cstdint>
#include <cstring>

#include "imgui.h"

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ragnarok/game_scene.h"
#include "ui/ro_imgui.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Adresses natives (client 20250716, no-ASLR : Ghidra == live) ─────────────
// Documenté dans docs/entity_nameplate_re.md.
namespace {

// GameMode_GetActive(mgr) __fastcall : renvoie le CGameMode actif, ou 0 hors
// jeu (login/char-select) — donc jamais de pointeur périmé.

// Signature de `gamescene::kNameDictGetEntryOrRequestAddr` (cf. game_scene.h
// pour ce qu'elle déclenche : elle DEMANDE les noms inconnus au serveur).
using GetNameEntryFn = void*(__thiscall*)(void*, unsigned);

// Offsets d'un ACTEUR (la navigation gm -> gestionnaire -> acteur est dans
// ragnarok/game_scene.h ; cf. docs/entity_nameplate_re.md).
constexpr int kAct_Nameplate = 0xa5;   //  byte : l'acteur participe au nameplate (visible/vivant)
constexpr int kAct_BaseJob   = 0x25c;  //  int  : classe/job de base
constexpr int kAct_ScreenX   = 0xac;   //  int  : X écran projeté (pieds)
constexpr int kAct_ScreenY   = 0xb0;   //  int  : Y écran projeté (pieds)
constexpr int kAct_Aid       = 0x110;  //  uint : AID (clé du dictionnaire de noms)

// Taille et capacité de la std::string du NOM, mesurées depuis le début du
// CNameInfo — donc la position du champ PLUS l'offset interne de la string.
constexpr int kName_Size = gamescene::kNameStr + gamescene::kStrSize;
constexpr int kName_Cap  = gamescene::kNameStr + gamescene::kStrCap;

// Prédicats de type réimplémentés d'après Job_IsPlayerJobId / Job_IsMonsterId
// (fonctions feuilles) — évite tout appel natif pour la classification.
inline bool IsPlayerJob(unsigned id) {
  return (id <= 0x1e) || (id - 0xfa1u <= 0x7ceu);
}
inline bool IsMonsterJob(unsigned id) {
  return (static_cast<int>(id) >= 0x3e9 && static_cast<int>(id) <= 0xf9e) ||
         (id - 0x4e35u <= 0x2ecau);
}

template <typename T>
inline T Read(const void* base, int off) {
  return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + off);
}

}  // namespace

void EntityNames::OnRenderUI() {
  if (!enabled_) return;
  // Réservé au staff : niveau de groupe serveur >= 80 reçu au login (setting id
  // 26). Gate de confiance côté client — de toute façon le client possède déjà
  // tous les noms (paquets normaux), donc aucune donnée n'est protégeable côté
  // serveur pour cette fonctionnalité.
  if (!IsStaff()) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  // RenderUI() garde déjà le hors-jeu / chargement de map / UI native cachée.
  __try {
    DrawNames();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Une entité à moitié construite/libérée a fait faute : on ignore cette
    // frame plutôt que de crasher. Rien de natif n'a été modifié.
  }
}

void EntityNames::DrawNames() {
  void* gm = rag::ActiveModeIfReady();
  if (!gm) return;
  void* actor_mgr = Read<void*>(gm, gamescene::kGmActorMgr);
  if (!actor_mgr) return;
  void* sentinel = Read<void*>(actor_mgr, gamescene::kAmListHead);
  if (!sentinel) return;
  void* own_actor = Read<void*>(actor_mgr, gamescene::kAmOwnPlayer);
  void* dict = reinterpret_cast<uint8_t*>(gm) + gamescene::kGmNameDict;

  const ImGuiIO& io = ImGui::GetIO();
  const float disp_w = io.DisplaySize.x;
  const float disp_h = io.DisplaySize.y;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();  // au-dessus du jeu, sous les fenêtres ImGui
  ImFont* font = ImGui::GetFont();
  const float font_px = ImGui::GetFontSize() *
                        (font_scale_ < 0.5f ? 0.5f : (font_scale_ > 2.0f ? 2.0f : font_scale_));

  auto get_entry = reinterpret_cast<GetNameEntryFn>(gamescene::kNameDictGetEntryOrRequestAddr);

  // 🔴 Le corps est une lambda, et pas le corps d'une boucle, parce qu'il faut
  // l'appliquer à DEUX sources : la std::list<Actor*>, et le joueur local qui
  // n'y figure pas.
  auto draw_one = [&](void* actor) {
    if (!actor) return;

    // Éligible au nameplate (l'acteur est visible/vivant, comme pour le picking).
    if (Read<uint8_t>(actor, kAct_Nameplate) == 0) return;

    if (actor == own_actor && !show_self_) return;

    const unsigned job = Read<uint32_t>(actor, kAct_BaseJob);
    const bool is_mob = IsMonsterJob(job);
    const bool is_ply = !is_mob && IsPlayerJob(job);
    const bool is_npc = !is_mob && !is_ply;
    if (is_mob && !show_monsters_) return;
    if (is_ply && !show_players_) return;
    if (is_npc && !show_npcs_) return;

    // Position écran (pieds) déjà projetée par le moteur cette frame.
    const int sx = Read<int32_t>(actor, kAct_ScreenX);
    const int sy = Read<int32_t>(actor, kAct_ScreenY);
    if (sx <= 0 || sy <= 0 || sx >= static_cast<int>(disp_w) ||
        sy >= static_cast<int>(disp_h))
      return;

    // Nom depuis le dictionnaire natif (et demande au serveur si inconnu).
    const unsigned aid = Read<uint32_t>(actor, kAct_Aid);
    void* entry = get_entry(dict, aid);
    if (!entry) return;
    const unsigned size = Read<uint32_t>(entry, kName_Size);
    if (size == 0 || size > 63) return;  // inconnu (requête en file) ou aberrant
    const unsigned cap = Read<uint32_t>(entry, kName_Cap);
    const char* data = (cap < 16)
                           ? reinterpret_cast<const char*>(
                                 reinterpret_cast<uint8_t*>(entry) + gamescene::kNameStr)
                           : Read<const char*>(entry, gamescene::kNameStr);
    if (!data) return;

    char buf[64];
    std::memcpy(buf, data, size);
    buf[size] = '\0';

    // Couleur par type (palette native, cf. EntityName_ResolveColorByType).
    unsigned rgb = is_mob ? 0xC3C3FF : 0xFFFFFF;   // mob bleu clair / joueur+npc blanc
    if (actor == own_actor) rgb = 0xC8C8C7;
    const ImU32 col =
        IM_COL32((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, 255);

    // Centré horizontalement, juste sous les pieds — comme le nom natif.
    const ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, buf);
    const float px = static_cast<float>(sx) - ts.x * 0.5f;
    const float py = static_cast<float>(sy) + static_cast<float>(y_offset_);

    if (outline_) {
      const ImU32 sh = IM_COL32(0, 0, 0, 210);
      dl->AddText(font, font_px, ImVec2(px + 1, py + 1), sh, buf);
      dl->AddText(font, font_px, ImVec2(px - 1, py + 1), sh, buf);
      dl->AddText(font, font_px, ImVec2(px + 1, py - 1), sh, buf);
      dl->AddText(font, font_px, ImVec2(px - 1, py - 1), sh, buf);
    }
    dl->AddText(font, font_px, ImVec2(px, py), col, buf);
  };

  // Parcours de la std::list<Actor*> : node{next@0, prev@4, value@8}. Garde-fou
  // sur le nombre d'itérations au cas où la liste serait corrompue.
  int guard = 0;
  for (void* node = Read<void*>(sentinel, 0);
       node && node != sentinel && guard < 4096;
       node = Read<void*>(node, 0), ++guard) {
    draw_one(Read<void*>(node, gamescene::kNodeActor));
  }

  // 🔴 Le joueur local N'EST PAS dans cette liste — vérifié en live le
  // 2026-08-08 : la sentinelle pointait sur elle-même (liste vide) alors que
  // l'acteur propre vivait bien à `actorMgr+0x2C`, vtable joueur 0x01094810.
  // Sans cette ligne, l'option « Ton propre nom » ne pouvait pas fonctionner :
  // le test `actor == own_actor` du corps n'était jamais atteint pour soi.
  // Découvert en reprenant les bulles de chat, qui butaient sur le même angle
  // mort (cf. docs/entity_chat_balloon_re.md §9).
  //
  // ⚠ Le garde-fou `+0xa5` s'applique aussi à lui. S'il s'avérait ne jamais être
  // armé sur le joueur local, c'est LUI qu'il faudrait relâcher ici — pas la
  // ligne ci-dessous.
  if (show_self_) draw_one(own_actor);
}

void EntityNames::DrawSettings() {
  bool save = false;
  if (ro::RoCheckbox(i18n::Tr("Afficher les noms en permanence"), &enabled_)) save = true;
  ImGui::TextDisabled(i18n::Tr("Affiche le nom au-dessus des entités sans avoir à les survoler."));

  if (enabled_) {
    Spacing();
    if (ro::RoCheckbox(i18n::Tr("Joueurs"), &show_players_)) save = true;
    SameLine();
    if (ro::RoCheckbox(i18n::Tr("Monstres"), &show_monsters_)) save = true;
    SameLine();
    if (ro::RoCheckbox(i18n::Tr("NPC"), &show_npcs_)) save = true;
    if (ro::RoCheckbox(i18n::Tr("Ton propre nom"), &show_self_)) save = true;
    if (ro::RoCheckbox(i18n::Tr("Contour noir (lisibilité)"), &outline_)) save = true;

    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderInt(i18n::Tr("Décalage vertical"), &y_offset_, -30, 30)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Taille du texte"), &font_scale_, 0.7f, 1.6f)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;

    ImGui::TextDisabled(i18n::Tr("Les monstres déclenchent une requête de nom au serveur : sur une "
             "map très peuplée, cela génère du trafic réseau."));
  }

  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
