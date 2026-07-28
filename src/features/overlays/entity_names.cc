#include "ragnarok/globals.h"
#include "features/overlays/entity_names.h"

#include <windows.h>

#include <cfloat>
#include <cstdint>
#include <cstring>

#include "imgui.h"

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "ui/ro_imgui.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Adresses natives (client 20250716, no-ASLR : Ghidra == live) ─────────────
// Documenté dans docs/entity_nameplate_re.md.
namespace {

// GameMode_GetActive(mgr) __fastcall : renvoie le CGameMode actif, ou 0 hors
// jeu (login/char-select) — donc jamais de pointeur périmé.
using GetActiveFn = void*(__fastcall*)(int);

// CNameDict_GetEntryOrRequest(dict, gid) __thiscall : renvoie le bloc CNameInfo
// pour ce GID si connu+valide, sinon met le GID en file de requête serveur et
// renvoie une entrée statique vide. C'est aussi ce qui déclenche le chargement
// des noms encore inconnus (comme le survol natif).
using GetNameEntryFn = void*(__thiscall*)(void*, unsigned);
constexpr uintptr_t kCNameDict_GetEntryOrRequest = 0x005a1460;

// Offsets GameMode / actor manager / acteur (cf. docs/entity_nameplate_re.md).
constexpr int kGm_ActorMgr   = 0xcc;   // *(gm+0xcc)      = actorMgr
constexpr int kGm_NameDict    = 0x160;  //  gm+0x160        = dictionnaire de noms (objet embarqué)
constexpr int kAm_ListHead   = 0x10;   // *(actorMgr+0x10) = nœud sentinelle std::list<Actor*>
constexpr int kAm_OwnPlayer  = 0x2c;   // *(actorMgr+0x2c) = acteur du joueur local
constexpr int kNode_Actor    = 0x08;   //  node+8          = pointeur acteur (std::list value)
constexpr int kAct_Nameplate = 0xa5;   //  byte : l'acteur participe au nameplate (visible/vivant)
constexpr int kAct_BaseJob   = 0x25c;  //  int  : classe/job de base
constexpr int kAct_ScreenX   = 0xac;   //  int  : X écran projeté (pieds)
constexpr int kAct_ScreenY   = 0xb0;   //  int  : Y écran projeté (pieds)
constexpr int kAct_Aid       = 0x110;  //  uint : AID (clé du dictionnaire de noms)

// CNameInfo (bloc valeur renvoyé par GetEntryOrRequest) : std::string nom à
// +0x04 (MSVC : buf/ptr @+0, size @+0x10, cap @+0x14).
constexpr int kName_Str  = 0x04;
constexpr int kName_Size = 0x14;  // = str+0x10
constexpr int kName_Cap  = 0x18;  // = str+0x14

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
  void* gm = reinterpret_cast<GetActiveFn>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
  if (!gm) return;
  void* actor_mgr = Read<void*>(gm, kGm_ActorMgr);
  if (!actor_mgr) return;
  void* sentinel = Read<void*>(actor_mgr, kAm_ListHead);
  if (!sentinel) return;
  void* own_actor = Read<void*>(actor_mgr, kAm_OwnPlayer);
  void* dict = reinterpret_cast<uint8_t*>(gm) + kGm_NameDict;

  const ImGuiIO& io = ImGui::GetIO();
  const float disp_w = io.DisplaySize.x;
  const float disp_h = io.DisplaySize.y;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();  // au-dessus du jeu, sous les fenêtres ImGui
  ImFont* font = ImGui::GetFont();
  const float font_px = ImGui::GetFontSize() *
                        (font_scale_ < 0.5f ? 0.5f : (font_scale_ > 2.0f ? 2.0f : font_scale_));

  auto get_entry = reinterpret_cast<GetNameEntryFn>(kCNameDict_GetEntryOrRequest);

  // Parcours de la std::list<Actor*> : node{next@0, prev@4, value@8}. Garde-fou
  // sur le nombre d'itérations au cas où la liste serait corrompue.
  int guard = 0;
  for (void* node = Read<void*>(sentinel, 0);
       node && node != sentinel && guard < 4096;
       node = Read<void*>(node, 0), ++guard) {
    void* actor = Read<void*>(node, kNode_Actor);
    if (!actor) continue;

    // Éligible au nameplate (l'acteur est visible/vivant, comme pour le picking).
    if (Read<uint8_t>(actor, kAct_Nameplate) == 0) continue;

    if (actor == own_actor && !show_self_) continue;

    const unsigned job = Read<uint32_t>(actor, kAct_BaseJob);
    const bool is_mob = IsMonsterJob(job);
    const bool is_ply = !is_mob && IsPlayerJob(job);
    const bool is_npc = !is_mob && !is_ply;
    if (is_mob && !show_monsters_) continue;
    if (is_ply && !show_players_) continue;
    if (is_npc && !show_npcs_) continue;

    // Position écran (pieds) déjà projetée par le moteur cette frame.
    const int sx = Read<int32_t>(actor, kAct_ScreenX);
    const int sy = Read<int32_t>(actor, kAct_ScreenY);
    if (sx <= 0 || sy <= 0 || sx >= static_cast<int>(disp_w) ||
        sy >= static_cast<int>(disp_h))
      continue;

    // Nom depuis le dictionnaire natif (et demande au serveur si inconnu).
    const unsigned aid = Read<uint32_t>(actor, kAct_Aid);
    void* entry = get_entry(dict, aid);
    if (!entry) continue;
    const unsigned size = Read<uint32_t>(entry, kName_Size);
    if (size == 0 || size > 63) continue;  // inconnu (requête en file) ou aberrant
    const unsigned cap = Read<uint32_t>(entry, kName_Cap);
    const char* data = (cap < 16)
                           ? reinterpret_cast<const char*>(
                                 reinterpret_cast<uint8_t*>(entry) + kName_Str)
                           : Read<const char*>(entry, kName_Str);
    if (!data) continue;

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
  }
}

void EntityNames::DrawSettings() {
  bool save = false;
  if (ro::RoCheckbox("Afficher les noms en permanence", &enabled_)) save = true;
  ImGui::TextDisabled("Affiche le nom au-dessus des entités sans avoir à les survoler.");

  if (enabled_) {
    Spacing();
    if (ro::RoCheckbox("Joueurs", &show_players_)) save = true;
    SameLine();
    if (ro::RoCheckbox("Monstres", &show_monsters_)) save = true;
    SameLine();
    if (ro::RoCheckbox("NPC", &show_npcs_)) save = true;
    if (ro::RoCheckbox("Ton propre nom", &show_self_)) save = true;
    if (ro::RoCheckbox("Contour noir (lisibilité)", &outline_)) save = true;

    ImGui::SetNextItemWidth(160.0f);
    if (WheelSliderInt("Décalage vertical", &y_offset_, -30, 30)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(160.0f);
    if (WheelSliderFloat("Taille du texte", &font_scale_, 0.7f, 1.6f)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;

    ImGui::TextDisabled("Les monstres déclenchent une requête de nom au serveur : sur une "
             "map très peuplée, cela génère du trafic réseau.");
  }

  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
