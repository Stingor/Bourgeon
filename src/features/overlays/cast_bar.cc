#include "ragnarok/globals.h"
#include "features/overlays/cast_bar.h"

#include <windows.h>
#include <mmsystem.h>  // timeGetTime (winmm) — la MÊME horloge que le natif

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "imgui.h"

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "features/overlays/basic_info.h"
#include "features/overlays/chat_balloon.h"
#include "ragnarok/uiwnd.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Adresses natives (client 20250716, no-ASLR : IDB == live) ────────────────
// Tout est documenté dans docs/entity_chat_balloon_re.md §8 et §12.
namespace {

// GameMode_GetActive(mgr) __fastcall : le CGameMode actif, 0 hors jeu.
using GetActiveFn = void*(__fastcall*)(int);

// char* GetSkillName(int id) — wrapper Lua, renvoie « Unknown-Skill » si l'id
// est inconnu. Même source que l'arbre de compétences et l'infobulle native.
constexpr uintptr_t kGetSkillNameLua = 0x0073a1f0;
using GetSkillNameLua_t = char*(__cdecl*)(int);

// Offsets GameMode / gestionnaire d'acteurs / acteur.
constexpr int kGm_ActorMgr  = 0xcc;   // *(gm+0xcc)       = actorMgr
constexpr int kAm_ListHead  = 0x10;   // *(actorMgr+0x10) = sentinelle std::list<Actor*>
constexpr int kAm_OwnPlayer = 0x2c;   // *(actorMgr+0x2c) = acteur du joueur local
constexpr int kNode_Actor   = 0x08;   //  node+8          = pointeur acteur
constexpr int kAct_ScreenX  = 0xac;   //  int   : X écran projeté (pieds)
constexpr int kAct_ScreenY  = 0xb0;   //  int   : Y écran projeté (pieds)
constexpr int kAct_Aid      = 0x110;  //  uint  : AID / GID
constexpr int kAct_BaseJob  = 0x25c;  //  int   : classe/job de base
constexpr int kAct_Height   = 0x5c;   //  float : facteur de hauteur du sprite
// 🔴 Le trio de l'incantation, relevé par RE de Actor_OnMsg_AppearanceEffects
// (cas msg 82, 0x00c4d955) et de CActorSprite_UpdateOverheadWidgets (0x00c46680).
constexpr int kAct_CastGage  = 0x270;  //  UIRechargeGage* : la barre native
constexpr int kAct_CastEnd   = 0x280;  //  uint : timeGetTime de FIN
constexpr int kAct_CastStart = 0x284;  //  uint : timeGetTime de DÉBUT
// La bulle de chat, pour savoir si le sort est DÉJÀ annoncé au-dessus de la tête.
// ⚠ Nulle dès que ChatBalloon a pris la main : il détruit la fenêtre native.
constexpr int kAct_Balloon   = 0x264;  //  UITransBalloonText*

// Modes du nom de sort, cf. CastBar::name_mode_.
constexpr int kName_Never  = 0;
constexpr int kName_IfFree = 1;  // seulement si aucune bulle ne l'annonce déjà
constexpr int kName_Always = 2;

// Garde-fou sur la durée lue : au-delà, c'est de la mémoire recyclée, pas une
// incantation. Deux minutes couvrent largement le plus long sort du jeu.
constexpr uint32_t kMaxCastMs = 120000;

// Écart toléré entre le paquet capté et le début de l'incantation observée sur
// l'acteur. Le paquet précède le msg 82 de quelques instants, jamais plus.
constexpr uint32_t kPacketPairingMs = 1500;

// ZC d'incantation. Les trois portent la MÊME disposition ; seul le dernier
// octet (le drapeau « disposable ») distingue 0x07FB/0x0B1A de 0x013E, et on ne
// s'en sert pas. Offsets APRÈS l'en-tête de 2 octets que retire
// RegisterObserveOpcode.
constexpr uint16_t kOpUseSkillAck  = 0x013E;  // ZC_USESKILL_ACK
constexpr uint16_t kOpUseSkillAck2 = 0x07FB;  // ZC_USESKILL_ACK2
constexpr uint16_t kOpUseSkillAck3 = 0x0B1A;
constexpr uint16_t kCastPayloadLen = 22;      // jusqu'à la durée incluse
constexpr int kPk_SrcGid  = 0x00;  // dword
constexpr int kPk_SkillId = 0x0c;  // u16   (= +0x0E dans le paquet complet)
constexpr int kPk_Length  = 0x12;  // dword (= +0x14 dans le paquet complet)

// Prédicats de type, réimplémentés d'après Job_IsPlayerJobId / Job_IsMonsterId
// (fonctions feuilles) — mêmes bornes que features/overlays/entity_names.cc.
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

inline ImU32 ColU32(const float rgba[4], float alpha_factor) {
  return ImGui::ColorConvertFloat4ToU32(
      ImVec4(rgba[0], rgba[1], rgba[2], rgba[3] * alpha_factor));
}

// « 1,4 s ». La virgule vient du catalogue : le gabarit est traduisible pour que
// l'anglais et l'espagnol posent leur propre séparateur décimal.
void FormatSeconds(int ms, char* out, size_t n) {
  if (ms < 0) ms = 0;
  const int tenths = (ms + 50) / 100;
  std::snprintf(out, n, i18n::Tr("%d,%d s"), tenths / 10, tenths % 10);
}

// Résolution du nom, isolée dans sa propre fonction : le `__try` de MSVC est
// interdit dans une fonction qui déroule des objets C++ (C2712), et l'appelant
// manipule une std::unordered_map.
void ResolveSkillName(int skill_id, char* out, size_t n) {
  if (n == 0) return;
  out[0] = '\0';
  __try {
    const char* s = reinterpret_cast<GetSkillNameLua_t>(kGetSkillNameLua)(skill_id);
    // « Unknown-Skill » est le repli du client : l'afficher serait pire que de
    // ne rien afficher.
    if (s == nullptr || s[0] == '\0' || std::strcmp(s, "Unknown-Skill") == 0) return;
    size_t i = 0;
    for (; i + 1 < n && s[i] != '\0'; ++i) out[i] = s[i];
    out[i] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '\0';
  }
}

}  // namespace

CastBar::CastBar() {
  auto& b = Bourgeon::Instance();
  // Observation pure : le client garde ses handlers, on ne fait que lire le
  // skillId qu'il jette (cf. l'en-tête).
  b.RegisterObserveOpcode(kOpUseSkillAck,  kCastPayloadLen);
  b.RegisterObserveOpcode(kOpUseSkillAck2, kCastPayloadLen);
  b.RegisterObserveOpcode(kOpUseSkillAck3, kCastPayloadLen);
}

void CastBar::OnModeSwitch(ModeMgr::ModeType mode_type, const char*) {
  if (mode_type == ModeMgr::ModeType::kGame) return;
  wire_casts_.clear();
  own_cast_ = OwnCast();
  // Les fenêtres natives partent avec le monde : rien à restaurer, et leurs
  // pointeurs seraient périmés. Baisser le drapeau évite qu'un retour en jeu
  // déclenche une restauration sur des acteurs qui n'ont jamais été masqués.
  natives_hidden_ = false;
}

// ── Réseau ───────────────────────────────────────────────────────────────────
void CastBar::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void CastBar::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode != kOpUseSkillAck && opcode != kOpUseSkillAck2 &&
      opcode != kOpUseSkillAck3)
    return;
  if (data == nullptr || len < kCastPayloadLen) return;

  uint32_t gid = 0;
  uint16_t skill_id = 0;
  uint32_t length_ms = 0;
  std::memcpy(&gid,       data + kPk_SrcGid,  sizeof(gid));
  std::memcpy(&skill_id,  data + kPk_SkillId, sizeof(skill_id));
  std::memcpy(&length_ms, data + kPk_Length,  sizeof(length_ms));
  // Durée nulle = incantation instantanée : le client ne crée alors AUCUNE barre
  // (cf. Effect_ApplySkillCastVisual). Retenir l'entrée nommerait à tort la barre
  // du sort suivant.
  if (gid == 0 || length_ms == 0) return;

  WireCast wc;
  wc.stamp_ms = ::timeGetTime();
  ResolveSkillName(static_cast<int>(skill_id), wc.name, sizeof(wc.name));
  wire_casts_[gid] = wc;

  // Purge d'entretien : une entrée ne sert qu'à nommer la barre du moment.
  if (wire_casts_.size() > 96) {
    const uint32_t now = wc.stamp_ms;
    for (auto it = wire_casts_.begin(); it != wire_casts_.end();)
      it = (now - it->second.stamp_ms > kMaxCastMs) ? wire_casts_.erase(it)
                                                    : std::next(it);
  }
}

const char* CastBar::SkillNameForGid(uint32_t gid, uint32_t cast_start) const {
  auto it = wire_casts_.find(gid);
  if (it == wire_casts_.end() || it->second.name[0] == '\0') return nullptr;
  // Appariement au démarrage observé : le paquet arrive juste AVANT le msg 82,
  // donc son horodatage encadre `cast_start` à quelques dizaines de ms près. Une
  // soustraction non signée dans les deux sens couvre le débordement de
  // timeGetTime aussi bien que l'ordre d'arrivée.
  const uint32_t d = (it->second.stamp_ms > cast_start)
                         ? (it->second.stamp_ms - cast_start)
                         : (cast_start - it->second.stamp_ms);
  return (d <= kPacketPairingMs) ? it->second.name : nullptr;
}

bool CastBar::EntityHasBalloon(void* actor) const {
  // Bulle ImGui : la fenêtre native a été DÉTRUITE, `acteur+0x264` ne dit plus
  // rien — seul le plugin sait ce qu'il affiche.
  const ChatBalloon* cb = Bourgeon::Instance().chat_balloon();
  if (cb != nullptr && cb->Active()) return cb->HasBalloonFor(actor);
  // Chatbox native : la fenêtre est encore accrochée à l'acteur.
  return Read<void*>(actor, kAct_Balloon) != nullptr;
}

// ── Frame ────────────────────────────────────────────────────────────────────
bool CastBar::NeedsSync() const {
  if (enabled_ || hide_own_) return true;
  // Le remplacement est éteint, mais la barre HUD de BasicInfo se nourrit du
  // même relevé : sans lui elle resterait vide.
  const BasicInfo* bi = Bourgeon::Instance().basic_info();
  return bi != nullptr && bi->bars_visible_ && bi->bars_[BasicInfo::kCast].show;
}

void CastBar::OnGameFramePulse() {
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  if (!NeedsSync()) {
    // On vient peut-être d'être décochés en pleine incantation : rendre leur
    // visibilité aux fenêtres qu'on masquait, sinon elles resteraient invisibles
    // jusqu'à leur expiration.
    if (natives_hidden_) RestoreNatives();
    own_cast_ = OwnCast();
    return;
  }
  SyncGuarded();
}

// ⚠ Le `__try` vit SEUL dans sa fonction : MSVC refuse de mêler gestion
// structurée d'exceptions et déroulement d'objets C++ (C2712), et le relevé
// touche à `wire_casts_`.
void CastBar::SyncGuarded() {
  __try {
    SyncWithActors();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Acteur à moitié construit ou déjà libéré : on saute ce battement.
  }
}

void CastBar::SyncWithActors() {
  void* gm = reinterpret_cast<GetActiveFn>(rag::kModeMgrGetActiveAddr)(
      static_cast<int>(rag::kModeMgrAddr));
  if (!gm) { own_cast_ = OwnCast(); return; }
  void* actor_mgr = Read<void*>(gm, kGm_ActorMgr);
  if (!actor_mgr) { own_cast_ = OwnCast(); return; }
  void* sentinel = Read<void*>(actor_mgr, kAm_ListHead);
  void* own_actor = Read<void*>(actor_mgr, kAm_OwnPlayer);

  const uint32_t now = ::timeGetTime();

  // Masquage de la fenêtre native. 🔴 On la MASQUE, on ne la détruit pas : c'est
  // elle qui porte l'horloge dont on se sert, et son retrait (msg 83) a des
  // effets de bord que le natif se réserve. Voir l'en-tête.
  //
  // ⚠ Le masquage est GLOBAL dès que le remplacement est actif, indépendamment
  // des filtres d'affichage : décocher « Monstres » veut dire « je ne veux pas
  // voir ces barres », pas « rends-moi celles du client ».
  //
  // ⚠ On écrit le drapeau dans les DEUX sens à chaque frame, plutôt que de tenir
  // une liste de ce qu'on a masqué. Décocher un réglage doit rendre sa barre au
  // client immédiatement, et une remise à `visible` sur une fenêtre déjà visible
  // ne coûte qu'un stockage — là où une comptabilité oublierait toujours un cas.
  auto apply_visibility = [&](void* actor, bool is_own) {
    void* gage = Read<void*>(actor, kAct_CastGage);
    if (gage == nullptr) return;
    const bool hide = enabled_ || (is_own && hide_own_);
    uiwnd::SetVisible(gage, !hide);
    if (hide) natives_hidden_ = true;
  };

  int guard = 0;
  for (void* node = sentinel ? Read<void*>(sentinel, 0) : nullptr;
       node && node != sentinel && guard < 4096;
       node = Read<void*>(node, 0), ++guard) {
    void* actor = Read<void*>(node, kNode_Actor);
    if (actor) apply_visibility(actor, false);
  }
  // 🔴 Le joueur local n'est PAS dans la std::list : il vit à part, à
  // `actorMgr+0x2C`. C'est l'angle mort qui avait laissé sa bulle de chat en
  // natif ; il vaut tout autant ici.
  if (own_actor) apply_visibility(own_actor, true);

  // ── Instantané de NOTRE incantation ────────────────────────────────────────
  own_cast_ = OwnCast();
  if (!own_actor) return;
  void* own_gage = Read<void*>(own_actor, kAct_CastGage);
  if (own_gage == nullptr) return;
  const uint32_t start = Read<uint32_t>(own_actor, kAct_CastStart);
  const uint32_t end   = Read<uint32_t>(own_actor, kAct_CastEnd);
  const uint32_t total = end - start;
  if (total == 0 || total > kMaxCastMs) return;
  if (static_cast<int32_t>(end - now) < 0) return;  // expirée, le natif va la retirer

  uint32_t elapsed = now - start;
  if (elapsed > total) elapsed = total;
  own_cast_.active       = true;
  own_cast_.total_ms     = static_cast<int>(total);
  own_cast_.elapsed_ms   = static_cast<int>(elapsed);
  own_cast_.remaining_ms = static_cast<int>(total - elapsed);
  own_cast_.frac         = static_cast<float>(elapsed) / static_cast<float>(total);
  const char* nm = SkillNameForGid(Read<uint32_t>(own_actor, kAct_Aid), start);
  if (nm) std::snprintf(own_cast_.name, sizeof(own_cast_.name), "%s", nm);
}

// Rend leur visibilité aux barres natives encore en vol. Sans objet C++ local :
// le `__try` est ici directement, pas dans une enveloppe.
void CastBar::RestoreNatives() {
  natives_hidden_ = false;
  __try {
    void* gm = reinterpret_cast<GetActiveFn>(rag::kModeMgrGetActiveAddr)(
        static_cast<int>(rag::kModeMgrAddr));
    if (!gm) return;
    void* actor_mgr = Read<void*>(gm, kGm_ActorMgr);
    if (!actor_mgr) return;
    void* sentinel = Read<void*>(actor_mgr, kAm_ListHead);
    int guard = 0;
    for (void* node = sentinel ? Read<void*>(sentinel, 0) : nullptr;
         node && node != sentinel && guard < 4096;
         node = Read<void*>(node, 0), ++guard) {
      void* actor = Read<void*>(node, kNode_Actor);
      if (!actor) continue;
      void* gage = Read<void*>(actor, kAct_CastGage);
      if (gage) uiwnd::SetVisible(gage, true);
    }
    void* own_actor = Read<void*>(actor_mgr, kAm_OwnPlayer);
    if (own_actor) {
      void* gage = Read<void*>(own_actor, kAct_CastGage);
      if (gage) uiwnd::SetVisible(gage, true);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Acteur libéré entre-temps : sa fenêtre est partie avec lui.
  }
}

void CastBar::OnRenderUI() {
  if (!enabled_) return;
  if (Bourgeon::Instance().client().timestamp() != 20250716) return;
  __try {
    DrawBars();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Une entité à moitié construite/libérée a fait faute : on saute la frame.
    // Rien de natif n'a été modifié — le masquage a eu lieu avant.
  }
}

void CastBar::DrawBars() {
  void* gm = reinterpret_cast<GetActiveFn>(rag::kModeMgrGetActiveAddr)(
      static_cast<int>(rag::kModeMgrAddr));
  if (!gm) return;
  void* actor_mgr = Read<void*>(gm, kGm_ActorMgr);
  if (!actor_mgr) return;
  void* sentinel = Read<void*>(actor_mgr, kAm_ListHead);
  void* own_actor = Read<void*>(actor_mgr, kAm_OwnPlayer);

  const ImGuiIO& io = ImGui::GetIO();
  const float disp_w = io.DisplaySize.x;
  const float disp_h = io.DisplaySize.y;
  // 🔴 Fond de z-index : derrière TOUTES nos fenêtres ImGui, jamais de
  // SetCursorPos (convention maison, cf. feedback_imgui_overlay_drawlist).
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  ImFont* font = ImGui::GetFont();
  const float font_px =
      ImGui::GetFontSize() *
      (font_scale_ < 0.5f ? 0.5f : (font_scale_ > 2.0f ? 2.0f : font_scale_));

  const uint32_t now = ::timeGetTime();
  const float bw = static_cast<float>(width_);
  const float bh = static_cast<float>(height_);
  // Le natif fige `g_OverheadWidgetYScale` à la première frame (magic static),
  // donc il ne suit pas un changement de résolution en cours de session. On la
  // recalcule, comme pour les bulles.
  const float y_scale = 81.0f * disp_h / 480.0f;

  auto draw_one = [&](void* actor) {
    if (actor == nullptr) return;
    void* gage = Read<void*>(actor, kAct_CastGage);
    if (gage == nullptr) return;  // pas d'incantation en cours

    const uint32_t start = Read<uint32_t>(actor, kAct_CastStart);
    const uint32_t end   = Read<uint32_t>(actor, kAct_CastEnd);
    const uint32_t total = end - start;
    if (total == 0 || total > kMaxCastMs) return;
    if (static_cast<int32_t>(end - now) < 0) return;

    const bool is_own = (actor == own_actor);
    const unsigned job = Read<uint32_t>(actor, kAct_BaseJob);
    const bool is_mob = !is_own && IsMonsterJob(job);
    const bool is_ply = !is_own && !is_mob && IsPlayerJob(job);
    if (is_own) {
      if (hide_own_) return;
    } else {
      if (is_mob && !show_monsters_) return;
      if (is_ply && !show_players_) return;
      if (!is_mob && !is_ply && !show_npcs_) return;
    }

    const int sx = Read<int32_t>(actor, kAct_ScreenX);
    const int sy = Read<int32_t>(actor, kAct_ScreenY);
    if (sx <= 0 || sy <= 0 || sx >= static_cast<int>(disp_w) ||
        sy >= static_cast<int>(disp_h))
      return;  // hors champ : pas de barre collée au bord

    uint32_t elapsed = now - start;
    if (elapsed > total) elapsed = total;
    const float frac = static_cast<float>(elapsed) / static_cast<float>(total);

    // Même ancrage que le natif : `y = yPieds - échelle × hauteurSprite`, la
    // barre poussant vers le bas depuis là. La bulle de chat, elle, se place
    // au-dessus de ce même point — les deux ne se marchent pas dessus.
    const ImVec2 p0(static_cast<float>(sx) - bw * 0.5f,
                    static_cast<float>(sy) - y_scale * Read<float>(actor, kAct_Height) +
                        static_cast<float>(y_offset_));
    const ImVec2 p1(p0.x + bw, p0.y + bh);

    const ImU32 bg   = ColU32(bg_color_, opacity_);
    const ImU32 fill = ColU32(is_mob ? mob_fill_color_ : fill_color_, opacity_);

    dl->AddRectFilled(p0, p1, bg, rounding_);
    const float fillpx = bw * frac;
    if (fillpx >= 1.0f) {
      // Arrondir seulement les coins d'attaque : le front de progression doit
      // rester une arête franche, sinon il « fond » à mesure qu'il avance.
      const ImDrawFlags fl = (frac >= 0.999f) ? ImDrawFlags_RoundCornersAll
                                              : ImDrawFlags_RoundCornersLeft;
      dl->AddRectFilled(p0, ImVec2(p0.x + fillpx, p1.y), fill, rounding_, fl);
    }
    if (border_)
      dl->AddRect(p0, p1, IM_COL32(0, 0, 0, static_cast<int>(170.0f * opacity_)),
                  rounding_);

    // ── L'étiquette, que le natif n'a jamais eue ──────────────────────────────
    //
    // 🔴 Une bulle occupe la place JUSTE AU-DESSUS de la barre, au pixel près :
    // c'est le même ancrage, la bulle poussant vers le haut et la barre vers le
    // bas. Y écrire par-dessus donnait deux textes superposés et illisibles.
    // Quand une bulle est là, l'étiquette passe donc à DROITE de la barre — un
    // espace toujours libre, et qui se lit comme un chronomètre.
    const bool balloon = EntityHasBalloon(actor);
    const bool want_name =
        (name_mode_ == kName_Always) || (name_mode_ == kName_IfFree && !balloon);

    char label[96];
    label[0] = '\0';
    const char* nm =
        want_name ? SkillNameForGid(Read<uint32_t>(actor, kAct_Aid), start) : nullptr;
    char secs[24];
    secs[0] = '\0';
    if (show_time_)
      FormatSeconds(static_cast<int>(total - elapsed), secs, sizeof(secs));
    if (nm != nullptr && secs[0] != '\0')
      std::snprintf(label, sizeof(label), "%s  %s", nm, secs);
    else if (nm != nullptr)
      std::snprintf(label, sizeof(label), "%s", nm);
    else if (secs[0] != '\0')
      std::snprintf(label, sizeof(label), "%s", secs);

    if (label[0] == '\0') return;
    const ImVec2 ts = font->CalcTextSizeA(font_px, FLT_MAX, 0.0f, label);
    const float tx = balloon ? p1.x + 3.0f : static_cast<float>(sx) - ts.x * 0.5f;
    const float ty = balloon ? p0.y + (bh - ts.y) * 0.5f   // centrée sur la barre
                             : p0.y - ts.y - 1.0f;         // posée SUR la barre
    const int a255 = static_cast<int>(255.0f * opacity_);
    dl->AddText(font, font_px, ImVec2(tx + 1.0f, ty + 1.0f),
                IM_COL32(0, 0, 0, static_cast<int>(210.0f * opacity_)), label);
    dl->AddText(font, font_px, ImVec2(tx, ty), IM_COL32(255, 255, 255, a255), label);
  };

  int guard = 0;
  for (void* node = sentinel ? Read<void*>(sentinel, 0) : nullptr;
       node && node != sentinel && guard < 4096;
       node = Read<void*>(node, 0), ++guard) {
    draw_one(Read<void*>(node, kNode_Actor));
  }
  draw_one(own_actor);  // hors liste, cf. SyncWithActors
}

void CastBar::OwnCastLabel(char* out, size_t n) const {
  if (out == nullptr || n == 0) return;
  out[0] = '\0';
  if (!own_cast_.active) return;
  char secs[24];
  FormatSeconds(own_cast_.remaining_ms, secs, sizeof(secs));
  if (own_cast_.name[0] != '\0')
    std::snprintf(out, n, "%s  %s", own_cast_.name, secs);
  else
    std::snprintf(out, n, "%s", secs);
}

// ── Réglages ─────────────────────────────────────────────────────────────────
void CastBar::DrawSettings() {
  bool save = false;

  ImGui::TextDisabled(i18n::Tr(
      "La barre du client fait 60×6 pixels, sans nom de compétence ni durée. "
      "Elle n'est pas détruite mais masquée : c'est son horloge qui alimente "
      "celle-ci."));
  Spacing();

  if (ro::RoCheckbox(i18n::Tr("Remplacer les barres d'incantation"), &enabled_)) save = true;

  ImGui::BeginDisabled(!enabled_);
  Indent();
    if (ro::RoCheckbox(i18n::Tr("Joueurs"), &show_players_)) save = true;
    SameLine();
    if (ro::RoCheckbox(i18n::Tr("Monstres"), &show_monsters_)) save = true;
    SameLine();
    if (ro::RoCheckbox(i18n::Tr("PNJ"), &show_npcs_)) save = true;
    SameLine();
    HelpMarker(i18n::Tr(
        "Ce que tu veux VOIR. Décocher ne rend pas la barre du client : le "
        "remplacement reste global, seule la barre affichée disparaît."));

    // Libellés BRUTS : RoCombo les traduit à la lecture (un i18n::Tr posé sur un
    // tableau statique serait figé au chargement de la DLL).
    const char* name_modes[] = {"Jamais", "Si pas déjà annoncé", "Toujours"};
    ImGui::SetNextItemWidth(ro::Px(200.0f));
    if (ro::RoCombo(i18n::Tr("Nom de la compétence"), &name_mode_, name_modes,
                    IM_ARRAYSIZE(name_modes)))
      save = true;
    SameLine();
    HelpMarker(i18n::Tr(
        "Le client annonce déjà le sort dans une bulle au-dessus de la tête — "
        "pour les joueurs, pas pour les monstres. « Si pas déjà annoncé » ne "
        "l'écrit donc que là où l'information manque.\n"
        "Le nom lui-même vient du paquet d'incantation, que le client jette sans "
        "le transmettre à sa barre : sans paquet capté, seul le temps s'affiche."));

    if (ro::RoCheckbox(i18n::Tr("Temps restant"), &show_time_)) save = true;
    SameLine();
    HelpMarker(i18n::Tr(
        "Sous une bulle, l'étiquette passe à droite de la barre : la bulle "
        "occupe exactement la place au-dessus."));

    if (ro::RoCheckbox(i18n::Tr("Bordure"), &border_)) save = true;

    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderInt(i18n::Tr("Largeur"), &width_, 30, 300)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderInt(i18n::Tr("Hauteur"), &height_, 3, 40)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderInt(i18n::Tr("Décalage vertical"), &y_offset_, -60, 60)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Arrondi"), &rounding_, 0.0f, 12.0f)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Taille du texte"), &font_scale_, 0.6f, 1.6f)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (WheelSliderFloat(i18n::Tr("Opacité"), &opacity_, 0.25f, 1.0f)) save = true;
    if (ImGui::IsItemDeactivatedAfterEdit()) save = true;

    if (ColorEdit4WithAlphaBar(i18n::Tr("Fond / Opacité"), bg_color_)) save = true;
    if (ColorEdit4WithAlphaBar(i18n::Tr("Remplissage"), fill_color_)) save = true;
    if (ColorEdit4WithAlphaBar(i18n::Tr("Remplissage (monstres)"), mob_fill_color_))
      save = true;
  Unindent();
  ImGui::EndDisabled();

  Spacing();
  // Volontairement HORS du bloc désactivé : masquer la sienne doit marcher même
  // remplacement éteint — c'est alors la fenêtre NATIVE qu'on masque, sinon
  // décocher le remplacement ferait revenir la barre qu'on voulait cacher.
  if (ro::RoCheckbox(i18n::Tr("Masquer la mienne au-dessus de ma tête"), &hide_own_))
    save = true;
  SameLine();
  HelpMarker(i18n::Tr(
      "Pour qui préfère la suivre dans son interface. La barre « Cast » se règle "
      "avec les autres barres, dans la section « Basic Info »."));

  if (save) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}
