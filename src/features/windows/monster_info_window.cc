#include "features/windows/monster_info_window.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>   // FLT_MAX (contraintes de taille de la fenêtre)
#include <cstdarg>  // va_list (helper Label)
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/item_cell.h"
#include "features/systems/bourgeon_opcodes.h"
#include "imgui.h"
#include "ragnarok/item_db.h"   // itemdb::kSkillDesc* (fenêtre native 0x2E)
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"     // MakeWindow / OnMsg / SetPos (description de skill)
#include "ui/icon_cache.h"
#include "ui/ro_imgui.h"        // ro::BeginRoDescWindow, ro::LocalToUtf8 (CP949)

namespace {

// ── Le paquet du skill Sense ─────────────────────────────────────────────────
// ZC_MONSTER_INFO, longueur FIXE 29 (en-tête compris). Notre `data` commence
// juste après l'opcode -> 27 octets utiles. Cf. docs/monster_info_re.md §3.
constexpr uint16_t kOpcodeMonsterInfo = 0x018C;
constexpr int      kMonsterInfoBody   = 27;

constexpr uint16_t kOpcodeReqMobInfo = bopcodes::kReqMobInfo;  // CZ 0x0F1F
constexpr uint16_t kOpcodeMobInfo    = bopcodes::kMobInfo;     // ZC 0x0F20

// Les cinq Gardiens de forteresse que le handler natif refuse d'afficher. Ce
// n'est pas un défaut d'interface : c'est le client qui protège les PV des
// gardiens pendant la Guerre d'Emperium. On le reproduit par défaut — lever le
// filtre est un choix de serveur, exposé en réglage. Cf. docs §5.
constexpr uint32_t kGuardianClasses[] = {1285, 1286, 1287, 1829, 1830};

bool IsGuardianClass(uint32_t cls) {
  for (uint32_t g : kGuardianClasses)
    if (g == cls) return true;
  return false;
}

// ── Libellés : ceux du CLIENT, jamais une paraphrase ─────────────────────────
// Mêmes ids que ceux qu'utilise le rendu natif (docs §4.3), donc mêmes mots que
// la fenêtre d'origine, dans la langue du client.
constexpr int kMsiTribeBase    = 0x0F2F;  // + race    (10 entrées)
constexpr int kMsiPropertyBase = 0x0F39;  // + élément (10 entrées)
constexpr int kMsiSizeBase     = 0x0F43;  // + taille  (3 entrées)
constexpr int kMsiDef          = 0x0F46;
constexpr int kMsiName         = 0x0197;
constexpr int kMsiLevel        = 0x0198;
constexpr int kMsiHp           = 0x0199;
constexpr int kMsiSize         = 0x019A;
constexpr int kMsiRaceType     = 0x019B;
constexpr int kMsiMdef         = 0x019C;
constexpr int kMsiProperty     = 0x019D;

const char* RaceName(uint8_t race) {
  return (race < 10) ? msgstr::Utf8(kMsiTribeBase + race) : "?";
}
const char* ElementName(uint8_t ele) {
  // Le natif indexe par `élément % 20` — reste du codage historique
  // « élément + 20 × niveau ». Le serveur n'envoie que 0..9, l'opération est
  // neutre ; on la garde par sécurité, exactement comme lui.
  const uint8_t e = static_cast<uint8_t>(ele % 20);
  return (e < 10) ? msgstr::Utf8(kMsiPropertyBase + e) : "?";
}
const char* SizeName(uint8_t size) {
  return (size < 3) ? msgstr::Utf8(kMsiSizeBase + size) : "?";
}

// Ordre des résistances = ordre des constantes ELE_* du serveur.
const char* ResistLabel(int index) {
  return (index >= 0 && index < 10) ? msgstr::Utf8(kMsiPropertyBase + index) : "?";
}

// ── Palette ──────────────────────────────────────────────────────────────────
// 🔴 Le corps d'une fenêtre RO est CLAIR (skin 9-slice du client) : les couleurs
// vives et les teintes pâles y sont illisibles, et `ImGui::TextDisabled` —
// calibré pour un thème sombre — s'y efface presque complètement. La première
// version de cette fenêtre est tombée dans les deux pièges (nom en jaune pâle,
// résistances « normales » en beige sur beige).
//
// Valeurs alignées sur celles déjà employées par la feuille de personnage, qui
// vit sur le même fond.
const ImVec4 kLabel(0.35f, 0.35f, 0.42f, 1.0f);  // libellé (remplace TextDisabled)
const ImVec4 kTitle(0.45f, 0.24f, 0.02f, 1.0f);  // nom du monstre
const ImVec4 kGreen(0.10f, 0.50f, 0.15f, 1.0f);
const ImVec4 kRed(0.60f, 0.12f, 0.12f, 1.0f);
const ImVec4 kBlue(0.15f, 0.25f, 0.60f, 1.0f);
const ImVec4 kAmber(0.60f, 0.40f, 0.05f, 1.0f);

// Libellé lisible sur fond clair. `ImGui::TextDisabled` est proscrit dans cette
// fenêtre — d'où ce raccourci, pour qu'on n'ait pas à y repenser.
void Label(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  ImGui::PushStyleColor(ImGuiCol_Text, kLabel);
  ImGui::TextV(fmt, args);
  ImGui::PopStyleColor();
  va_end(args);
}

// Vert = le monstre encaisse peu, rouge = il encaisse plein (ou plus).
// Le cas « normal » (100 %) prend la couleur de texte COURANTE : sur fond clair,
// toute teinte pâle disparaîtrait.
ImVec4 ResistColor(int pct) {
  if (pct <= 0)  return kGreen;                        // immunise, ou soigne
  if (pct < 50)  return ImVec4(0.20f, 0.45f, 0.15f, 1.0f);
  if (pct <= 100) return ImGui::GetStyleColorVec4(ImGuiCol_Text);
  return kRed;                                         // > 100 % = faiblesse
}

// Bits de `status_data.mode` (e_mode, src/map/status.hpp). On ne nomme que ceux
// qui changent la façon de combattre le monstre — un dump de 32 bits n'aiderait
// personne.
struct ModeBit { uint32_t bit; const char* label; };
constexpr ModeBit kModeBits[] = {
    {0x0004, "Agressif"},        // MD_AGGRESSIVE
    {0x0008, "Assist"},          // MD_ASSIST
    {0x0010, "Ramasse"},         // MD_CASTSENSOR_IDLE / loot selon build
    {0x0020, "Boss"},            // MD_BOSS
    {0x0040, "Groupé"},          // MD_PLANT
    {0x0080, "Insensible aux altérations"},
    {0x0100, "Insensible au recul"},
    {0x0400, "Détecteur"},       // MD_DETECTOR (voit le camouflage)
    {0x2000, "Change de cible en chasse"},
};

// Sépare les milliers, comme le client (« 1,234,567 ») — même convention que
// `FormatZeny` de l'échoppe, pour que les gros nombres se lisent pareil partout.
// Les PV d'un MVP et l'EXP de base se comptent en millions : sans séparateur,
// personne ne distingue 1200000 de 12000000 d'un coup d'œil.
const char* Grouped(uint32_t v, char* out, size_t cap) {
  char raw[16];
  _snprintf_s(raw, sizeof(raw), _TRUNCATE, "%u", v);
  const int len = static_cast<int>(std::strlen(raw));
  int lead = (len % 3) ? (len % 3) : 3;
  size_t o = 0;
  for (int i = 0; i < len && o + 2 < cap; ++i) {
    if (i == lead && i != 0) { out[o++] = ','; lead += 3; }
    out[o++] = raw[i];
  }
  out[o] = '\0';
  return out;
}

// ── Précision, esquive, critique : calculés ici ───────────────────────────────
//
// Le paquet ne les transporte pas, et c'est volontaire : `status_calc_misc` ne
// tourne qu'à l'APPARITION du monstre, la ligne de mob_db ne les porte donc pas.
// Mais les formules PRE-RENEWAL ne dépendent que de champs qu'on a déjà.
//
//   status.cpp:2706-2713 (branche #else = PRE-RE, la seule qui vaut pour
//   Moonlight — cf. [[project_moonlight_is_prerenewal]]) :
//     HIT  = min(niveau, maxstatlevelcalc) + DEX
//     FLEE = min(niveau, maxstatlevelcalc) + AGI
//   `maxstatlevelcalc` vaut 600 sur Moonlight (conf/import/battle_conf.txt) :
//   aucun monstre ne s'en approche, la borne ne joue jamais.
//
// 🔴 Un monstre ne fait JAMAIS de critique sur ce serveur. `status_calc_misc`
// ne calcule `cri` que si `bl->type & battle_config.enable_critical`, et la conf
// vaut **17 = BL_PC | BL_MER** — BL_MOB (0x02) en est absent, donc `cri = 0`.
// Même chose pour l'esquive parfaite : `enable_perfect_flee: 1` = BL_PC seul.
// Afficher un « CRIT du monstre » serait donc faux ; on affiche pourquoi.

// Petit « (?) » cliquable, façon mui::HelpMarker mais avec NOTRE couleur : le
// corps de la fiche est clair, et `TextDisabled` y est illisible
// (cf. [[feedback_imgui_ro_light_body_colors]]).
void Help(const char* desc) {
  ImGui::SameLine();
  ImGui::TextColored(kLabel, "(?)");
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) && ImGui::BeginTooltip()) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
    ImGui::TextUnformatted(desc);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

// Statistiques du PERSONNAGE, telles que le natif les tient pour sa fenêtre de
// statut. Ce sont les valeurs FINALES (équipement et buffs compris), donc la
// seule bonne source pour une simulation.
//
// 🔴 Unités : `hit` et `flee` sont des ENTIERS bruts, mais `crit` et `pdodge`
// sont en DIXIÈMES de pour cent (346 = 34,6 %). Constaté en jeu — et à ne PAS
// « corriger » d'après le source du serveur (`cri/10`) ni d'après le natif (qui
// les formate en « %d ») : les deux laissent croire à un entier, les deux
// trompent. Les prendre pour des entiers donnait « 314,8 % de critique ».
struct OwnCombatStats {
  bool valid = false;
  int  hit = 0, flee = 0, crit = 0, pdodge = 0;
};
constexpr uintptr_t kOwnHit    = 0x015fba7c;
constexpr uintptr_t kOwnFlee   = 0x015fba80;
constexpr uintptr_t kOwnCrit   = 0x015fba84;
constexpr uintptr_t kOwnPdodge = 0x015fba88;

OwnCombatStats ReadOwnCombatStats() {
  OwnCombatStats s;
  __try {
    s.hit    = *reinterpret_cast<const int*>(kOwnHit);
    s.flee   = *reinterpret_cast<const int*>(kOwnFlee);
    s.crit   = *reinterpret_cast<const int*>(kOwnCrit);
    s.pdodge = *reinterpret_cast<const int*>(kOwnPdodge);
    s.valid  = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { s.valid = false; }
  // Avant le premier ZC_STATUS, tout est à zéro : une simulation sur des zéros
  // n'apprendrait rien, autant ne rien afficher.
  if (s.hit <= 0 && s.flee <= 0) s.valid = false;
  return s;
}

// Probabilité de toucher, en %. `battle_calc_attack_hit` (battle.cpp:3265-3319) :
// base 80 en pré-renewal, puis `+ HIT attaquant - FLEE cible`, borné par
// min_hitrate (5) et max_hitrate (100).
int HitChancePct(int attacker_hit, int defender_flee) {
  int rate = 80 + attacker_hit - defender_flee;
  if (rate < 5) rate = 5;
  if (rate > 100) rate = 100;
  return rate;
}

const char* BossLabel(uint8_t boss) {
  switch (boss) {
    case 1:  return "Mini-boss";
    case 2:  return "MVP";
    default: return nullptr;
  }
}

// ── Compétences de monstre ───────────────────────────────────────────────────
// Le paquet ne transporte QUE (skill_id, skill_lv) : le nom et la description
// sont côté client, et c'est très bien — ils y sont localisés.
//
// Nom : wrapper Lua natif `GetSkillName(id)` (`__cdecl`), la source qu'utilisent
// la fenêtre de skills et le tooltip natif. Rend « Unknown-Skill » sur un id
// inconnu — on retombe alors sur « #id », qui reste une information.
// Description : la fenêtre native 0x2E, pilotée par l'id BRUT (pas un
// ItemSkillInfo, contrairement aux objets) — même chemin que la feuille de
// personnage, cf. `CharacterSheet::OpenSkillDesc`.
constexpr uintptr_t kGetSkillNameLua = 0x0073a1f0;
using GetSkillNameLua_t = char* (__cdecl*)(int);

// SEH ISOLÉ dans sa propre fonction : l'appelant manipule des std::string.
bool SkillNameCp949(int id, char* out, size_t out_size) {
  bool ok = false;
  __try {
    const char* n = reinterpret_cast<GetSkillNameLua_t>(kGetSkillNameLua)(id);
    if (n && *n && std::strcmp(n, "Unknown-Skill") != 0) {
      strncpy_s(out, out_size, n, _TRUNCATE);
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

void OpenSkillDesc(int skill_id, int mx, int my) {
  if (skill_id <= 0) return;
  __try {
    void* wnd = uiwnd::MakeWindow(itemdb::kSkillDescWndId);
    if (!wnd) return;
    // Re-clic sur le même skill = referme, comme le natif (id affiché à +0x104).
    if (*reinterpret_cast<int*>(reinterpret_cast<char*>(wnd) +
                                itemdb::kSkillDescShownId) == skill_id) {
      uiwnd::CloseWindow(itemdb::kSkillDescWndId);
      return;
    }
    uiwnd::OnMsg(wnd, itemdb::kSkillDescMsgSet, skill_id, 0, 0, 0);
    uiwnd::SetPos(wnd, mx, my);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void RateText(char* out, size_t n, uint32_t rate) {
  // rate est en 1/100 de %, comme la fiche d'item et le bestiaire web.
  _snprintf_s(out, n, _TRUNCATE, "%.2f%%", rate / 100.0f);
}

}  // namespace

MonsterInfoWindow::MonsterInfoWindow() {
  // Notre réponse custom.
  Bourgeon::Instance().RegisterRecvOpcode(kOpcodeMobInfo);

  // 🔴 Le prédicat DÉCIDE, et il est interrogé à chaque paquet sur le fil réseau.
  // Faux -> le handler natif se déroule normalement (la fenêtre 0x4D s'ouvre).
  // Vrai -> il est sauté et nous recevons les octets : la native ne naît jamais.
  //
  // Aucun devoir caché à reprendre : la classe native n'émet aucun paquet et le
  // handler n'écrit aucun global (le seul du flux, g_SenseTargetGID, est écrit à
  // l'ENVOI du skill, pas à la réception). Cf. docs/monster_info_re.md §7.1.
  Bourgeon::Instance().RegisterReplaceOpcode(
      kOpcodeMonsterInfo, [this]() { return imgui_enabled_; });
}

void MonsterInfoWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                     uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void MonsterInfoWindow::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // Changement de map / retour au char-select : les ressources de sprite ont pu
  // être libérées et les taux de drop peuvent dépendre du serveur. On repart
  // propre plutôt que d'afficher un état d'une autre session.
  open_ = false;
  current_id_ = 0;
  sense_ = SenseSnapshot{};
  sprite_ = ro::MobSpriteRes{};
  cache_.clear();
}

void MonsterInfoWindow::Open(uint32_t mob_id, bool by_view) {
  if (mob_id == 0) return;
  current_id_ = mob_id;
  open_ = true;
  need_focus_ = true;
  RequestInfo(mob_id, by_view);
}

void MonsterInfoWindow::RequestInfo(uint32_t mob_id, bool by_view) {
  MobInfo& e = cache_[mob_id];
  const uint32_t now = GetTickCount();
  // Une fiche est statique (mob_db) : une fois prête, on n'y revient pas.
  if (e.state == Fetch::kReady || e.state == Fetch::kUnknown) return;
  // Requête en vol depuis moins de 5 s : on attend.
  if (e.state == Fetch::kPending && (now - e.requested_tick) < 5000) return;

  uint8_t pkt[9];  // [op:2][len:2][mob_id:4][by_view:1]
  *reinterpret_cast<uint16_t*>(pkt + 0) = kOpcodeReqMobInfo;
  *reinterpret_cast<uint16_t*>(pkt + 2) = static_cast<uint16_t>(sizeof(pkt));
  *reinterpret_cast<uint32_t*>(pkt + 4) = mob_id;
  pkt[8] = by_view ? 1 : 0;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  e.state = Fetch::kPending;
  e.requested_tick = now;
}

// Décodage sur le FIL PRINCIPAL (cf. features/net_inbox.h).
void MonsterInfoWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                     uint16_t len) {
  // ── Le paquet du skill Sense, revendiqué ──────────────────────────────────
  if (opcode == kOpcodeMonsterInfo) {
    if (len < kMonsterInfoBody) return;
    const uint16_t cls = *reinterpret_cast<const uint16_t*>(data + 0);
    if (!show_guardians_ && IsGuardianClass(cls)) return;  // filtre natif

    SenseSnapshot s;
    s.valid        = true;
    s.sprite_class = cls;
    s.level   = *reinterpret_cast<const uint16_t*>(data + 2);
    s.size    = *reinterpret_cast<const uint16_t*>(data + 4);
    s.hp      = *reinterpret_cast<const uint32_t*>(data + 6);
    s.def     = *reinterpret_cast<const uint16_t*>(data + 10);
    s.race    = *reinterpret_cast<const uint16_t*>(data + 12);
    s.mdef    = *reinterpret_cast<const uint16_t*>(data + 14);
    s.element = *reinterpret_cast<const uint16_t*>(data + 16);
    memcpy(s.resist, data + 18, 9);
    sense_ = s;
    Open(cls, /*by_view=*/true);
    return;
  }

  if (opcode != kOpcodeMobInfo) return;
  if (len < 5) return;  // [mob_id:4][status:1]
  const uint32_t req_id = *reinterpret_cast<const uint32_t*>(data);
  MobInfo& m = cache_[req_id];
  if (data[4] != 0) {  // status != 0 -> monstre inconnu du serveur
    m.state = Fetch::kUnknown;
    return;
  }

  // Parseur BORNÉ : `p` avance, `end` garde-fou. Toute donnée manquante laisse
  // la fiche partiellement remplie plutôt que de lire hors tampon.
  const uint8_t* p   = data + 5;
  const uint8_t* end = data + len;
  auto need = [&](int n) { return (end - p) >= n; };
  auto u8   = [&]() -> uint8_t  { const uint8_t  v = *p; p += 1; return v; };
  auto u16  = [&]() -> uint16_t { const uint16_t v = *reinterpret_cast<const uint16_t*>(p); p += 2; return v; };
  auto u32  = [&]() -> uint32_t { const uint32_t v = *reinterpret_cast<const uint32_t*>(p); p += 4; return v; };

  // Bloc fixe, dans l'ordre d'écriture du serveur :
  //   sprite 4 + level 2 + hp/sp 8 + exp 12 + atk/matk 8 + def/mdef 8
  // + stats 12 + portée 2 + octets 6 + mode 4 + vitesse 2 + résistances 20 = 88.
  //
  // Les portées de vue et de poursuite, le délai d'attaque et les deux durées
  // d'animation ont été RETIRÉS du paquet : ils n'apprenaient rien au joueur et
  // encombraient la fiche. `speed` reste — c'est lui qui donne les cases/s.
  if (!need(88)) { m.state = Fetch::kUnknown; return; }
  m.sprite_class = u32();
  m.level = u16();
  m.hp = u32();  m.sp = u32();
  m.base_exp = u32(); m.job_exp = u32(); m.mvp_exp = u32();
  m.atk_min = u16(); m.atk_max = u16(); m.matk_min = u16(); m.matk_max = u16();
  m.def = u16(); m.def2 = u16(); m.mdef = u16(); m.mdef2 = u16();
  m.str = u16(); m.agi = u16(); m.vit = u16();
  m.int_ = u16(); m.dex = u16(); m.luk = u16();
  m.range_atk = u16();
  m.size = u8(); m.race = u8(); m.element = u8(); m.element_lv = u8();
  m.boss = u8(); m.klass = u8();
  m.mode = u32();
  m.speed = u16();
  for (int i = 0; i < 10; ++i) m.resist[i] = static_cast<int16_t>(u16());

  m.name.clear();
  if (need(1)) {
    const uint8_t namelen = u8();
    if (need(namelen)) {
      m.name.assign(reinterpret_cast<const char*>(p), namelen);
      p += namelen;
    }
  }

  m.drops.clear();
  if (need(1)) {
    const uint8_t count = u8();
    for (uint8_t i = 0; i < count; ++i) {
      if (!need(10)) break;  // [nameid:4][rate:4][kind:1][namelen:1]
      Drop d;
      d.nameid = u32();
      d.rate   = u32();
      d.kind   = u8();
      const uint8_t namelen = u8();
      if (!need(namelen)) break;
      d.name.assign(reinterpret_cast<const char*>(p), namelen);
      p += namelen;
      m.drops.push_back(std::move(d));
    }
  }

  m.spawns.clear();
  if (need(1)) {
    const uint8_t count = u8();
    for (uint8_t i = 0; i < count; ++i) {
      if (!need(3)) break;  // [qty:2][maplen:1]
      Spawn s;
      s.qty = u16();
      const uint8_t maplen = u8();
      if (!need(maplen)) break;
      s.map.assign(reinterpret_cast<const char*>(p), maplen);
      p += maplen;
      m.spawns.push_back(std::move(s));
    }
  }

  m.skills.clear();
  if (need(1)) {
    const uint8_t count = u8();
    for (uint8_t i = 0; i < count; ++i) {
      if (!need(4)) break;
      MobSkill s;
      s.id = u16();
      s.lv = u16();
      // 🔴 Filtre à l'ENTRÉE : une compétence que le client ne sait pas nommer
      // est écartée ici, pas au rendu. mob_db contient des entrées de contrôle
      // d'IA (NPC_EMOTION, NPC_*_ATTACK…) et des skills custom sans entrée Lua ;
      // les afficher sous forme d'« id brut » ne renseigne personne et laisse
      // croire à un défaut d'affichage. Écarter ici garde aussi le compteur de
      // l'onglet — « Skills (n) » — cohérent avec ce qu'on montre vraiment.
      char cp949[128] = {0};
      if (!SkillNameCp949(s.id, cp949, sizeof(cp949))) continue;
      const char* utf8 = ro::LocalToUtf8(cp949);
      s.name = (utf8 && *utf8) ? utf8 : cp949;
      if (s.name.empty()) continue;
      m.skills.push_back(std::move(s));
    }
  }

  m.state = Fetch::kReady;
}

MonsterInfoWindow::MobInfo* MonsterInfoWindow::Current() {
  if (current_id_ == 0) return nullptr;
  auto it = cache_.find(current_id_);
  return (it == cache_.end()) ? nullptr : &it->second;
}

void MonsterInfoWindow::OnRenderUI() {
  if (!imgui_enabled_ || !open_) return;

  if (need_focus_) {
    ImGui::SetNextWindowFocus();
    need_focus_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(430.0f, 300.0f),
                                      ImVec2(FLT_MAX, FLT_MAX));

  // ── Habillage : celui de la FENÊTRE DE DESCRIPTION, pas celui d'une fenêtre
  // RO ordinaire ─────────────────────────────────────────────────────────────
  // `BeginRoDescWindow` = barre de titre claire (skill_upbar) + cadre boîte
  // sysbox. C'est le costume des panneaux de description (objet, compétence), et
  // cette fiche en est un : on la lit, on ne la manipule pas. Les quatre couleurs
  // et les trois arrondis ci-dessous sont EXACTEMENT ceux d'ItemDescWindow —
  // deux panneaux de description côte à côte doivent être indiscernables.
  ImGui::SetNextWindowBgAlpha(1.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg,      IM_COL32(245, 243, 232, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBg,       IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(120, 110, 90, 255));
  ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(0, 0, 0, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);

  // Titre = le NOM du monstre, comme la desc d'objet porte celui de l'objet.
  // L'id ### fige l'identité ImGui : changer de monstre ne doit pas réinitialiser
  // la position ni la taille de la fenêtre.
  const MobInfo* titled = Current();
  char title[160];
  _snprintf_s(title, sizeof(title), _TRUNCATE, "%s###monsterinfo",
              (titled != nullptr && titled->state == Fetch::kReady &&
               !titled->name.empty())
                  ? titled->name.c_str()
                  : "Fiche de monstre");

  const ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoFocusOnAppearing;
  const bool begun = ro::BeginRoDescWindow(title, &open_, kFlags);
  if (begun) {
    MobInfo* mob = Current();
    if (mob == nullptr || mob->state == Fetch::kPending ||
        mob->state == Fetch::kIdle) {
      Label("Interrogation du serveur...");
    } else if (mob->state == Fetch::kUnknown) {
      ImGui::TextColored(kRed, "Monstre inconnu du serveur (classe %u).",
                         current_id_);
      // On a peut-être quand même le relevé de Sense : mieux que rien.
      if (sense_.valid && sense_.sprite_class == current_id_) {
        ImGui::Separator();
        ImGui::Text("Relevé Sense : niveau %u, PV %u, DEF %u, MDEF %u",
                    sense_.level, sense_.hp, sense_.def, sense_.mdef);
      }
    } else {
      DrawHeader(*mob);
      ImGui::Separator();
      if (ImGui::BeginTabBar("##monsterinfo_tabs")) {
        if (ImGui::BeginTabItem("Stats")) {
          DrawStatsTab(*mob);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Résistances")) {
          DrawResistTab(*mob);
          ImGui::EndTabItem();
        }
        char drop_label[32];
        _snprintf_s(drop_label, sizeof(drop_label), _TRUNCATE, "Drops (%d)",
                    static_cast<int>(mob->drops.size()));
        if (ImGui::BeginTabItem(drop_label)) {
          DrawDropsTab(*mob);
          ImGui::EndTabItem();
        }
        char spawn_label[32];
        _snprintf_s(spawn_label, sizeof(spawn_label), _TRUNCATE, "Spawns (%d)",
                    static_cast<int>(mob->spawns.size()));
        if (ImGui::BeginTabItem(spawn_label)) {
          DrawSpawnsTab(*mob);
          ImGui::EndTabItem();
        }
        if (!mob->skills.empty()) {
          char skill_label[32];
          _snprintf_s(skill_label, sizeof(skill_label), _TRUNCATE, "Skills (%d)",
                      static_cast<int>(mob->skills.size()));
          if (ImGui::BeginTabItem(skill_label)) {
            DrawSkillsTab(*mob);
            ImGui::EndTabItem();
          }
        }
        ImGui::EndTabBar();
      }
    }
  }
  // ⚠ EndRoDescWindow s'appelle TOUJOURS, même repliée/masquée — comme
  // EndRoWindow et pour la même raison (règle Begin/End d'ImGui).
  ro::EndRoDescWindow();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(4);
  // ⚠ Pas de FlushDeferredDesc ici : c'est Bourgeon::OnProcessInput qui le fait,
  // une fois par frame et HORS frame ImGui (cf. features/item_cell.h).
}

void MonsterInfoWindow::DrawHeader(MobInfo& mob) {
  // ── Le sprite, animé ──────────────────────────────────────────────────────
  // Chargé depuis la classe de VUE renvoyée par le serveur, pas depuis l'id de
  // base : un monstre déguisé porte le sprite d'un autre.
  const uint32_t sprite_class =
      (mob.sprite_class != 0) ? mob.sprite_class : current_id_;
  ro::LoadMobSprite(static_cast<int>(sprite_class), &sprite_);

  const ImVec2 box(116.0f, 132.0f);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + box.x, p0.y + box.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(24, 22, 20, 90), 3.0f);
  const float clock = animate_ ? static_cast<float>(ImGui::GetTime()) : 0.0f;
  // allow_upscale = false : taille RÉELLE du sprite, le cadre ne sert qu'à borner
  // ce qui dépasse. Les petits monstres restent petits — c'est une information de
  // gabarit, pas un défaut de cadrage.
  if (!ro::DrawMobSprite(dl, sprite_, p0, p1, clock, /*action=*/0,
                         animate_ ? 130.0f : 0.0f, /*allow_upscale=*/false)) {
    // Repli : le .spr/.act n'existe pas (mob custom sans art). On ne laisse pas
    // un carré vide sans explication.
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddCircleFilled(c, 18.0f, IM_COL32(120, 110, 100, 160), 16);
    dl->AddText(ImVec2(p0.x + 6.0f, p1.y - 18.0f), IM_COL32(200, 190, 170, 200),
                "pas de sprite");
  }
  ImGui::Dummy(box);
  ImGui::SameLine();

  ImGui::BeginGroup();
  {
    ImGui::TextColored(kTitle, "%s",
                       mob.name.empty() ? "(sans nom)" : mob.name.c_str());
    ImGui::SameLine();
    Label("#%u", current_id_);
    if (const char* badge = BossLabel(mob.boss)) {
      ImGui::SameLine();
      ImGui::TextColored(mob.boss == 2 ? kRed : kAmber, "[%s]", badge);
    }

    ImGui::Text("%s : %u", msgstr::Utf8(kMsiLevel), mob.level);
    ImGui::Text("%s : %s", msgstr::Utf8(kMsiRaceType), RaceName(mob.race));
    ImGui::Text("%s : %s", msgstr::Utf8(kMsiProperty), ElementName(mob.element));
    ImGui::SameLine();
    // Le niveau élémentaire, que ZC_MONSTER_INFO ne transporte pas (docs §4.3).
    Label("niv. %u", mob.element_lv);
    ImGui::Text("%s : %s", msgstr::Utf8(kMsiSize), SizeName(mob.size));
    // Pas de ligne PV ici : l'onglet Stats l'affiche déjà, en tête de tableau.

    // ── Les modes, en badges ───────────────────────────────────────────────────
    // C'est ce qui décide comment on aborde le monstre (attaque-t-il à vue,
    // encaisse-t-il le recul…). En LIGNE continue, un monstre qui en cumule
    // cinq ou six — « Insensible aux altérations », « Change de cible en
    // chasse »… — débordait largement de la fenêtre. Trois par ligne, en table
    // sans bordure : ça reste compact et ça ne pousse plus la largeur.
    int mode_count = 0;
    for (const ModeBit& b : kModeBits)
      if (mob.mode & b.bit) ++mode_count;

    if (mode_count == 0) {
      Label("[Passif]");
    } else if (ImGui::BeginTable("##mi_modes", 3,
                                 ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_NoSavedSettings)) {
      for (const ModeBit& b : kModeBits) {
        if ((mob.mode & b.bit) == 0) continue;
        ImGui::TableNextColumn();
        ImGui::TextColored(kBlue, "[%s]", b.label);
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndGroup();

  DrawSenseNote(mob);
}

void MonsterInfoWindow::DrawSenseNote(MobInfo& mob) {
  if (!sense_.valid || sense_.sprite_class != mob.sprite_class) return;
  // Le relevé de Sense porte l'état RÉEL du monstre croisé ; mob_db porte la
  // référence. Un écart est une information (buff de serveur, WoE, mob modifié),
  // pas un bug — on ne l'affiche donc QUE s'il y en a un.
  const bool differs = sense_.hp != mob.hp || sense_.def != mob.def ||
                       sense_.mdef != mob.mdef || sense_.level != mob.level;
  if (!differs) return;
  ImGui::TextColored(ImVec4(0.70f, 0.55f, 0.20f, 1.0f),
                     "Relevé Sense (exemplaire croisé) : niveau %u, PV %u, %s %u, %s %u",
                     sense_.level, sense_.hp, msgstr::Utf8(kMsiDef), sense_.def,
                     msgstr::Utf8(kMsiMdef), sense_.mdef);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Ces chiffres viennent du monstre effectivement visé par Sense.\n"
        "Ils peuvent différer de la base de données (modificateurs de serveur,\n"
        "Guerre d'Emperium, monstre invoqué par script).");
}

void MonsterInfoWindow::DrawStatsTab(MobInfo& mob) {
  if (ImGui::BeginTable("##mi_stats", 4,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
    auto row = [](const char* l1, const char* v1, const char* l2,
                  const char* v2) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); Label("%s", l1);
      ImGui::TableNextColumn(); ImGui::Text("%s", v1);
      ImGui::TableNextColumn(); Label("%s", l2);
      ImGui::TableNextColumn(); ImGui::Text("%s", v2);
    };
    char a[64], b[64];

    // Séparateurs de milliers sur les seuls champs qui atteignent le million :
    // PV, SP et EXP. Les stats (STR 1..255), les portées et les millisecondes
    // n'en ont pas besoin, et en mettre partout ferait du bruit.
    row("PV", Grouped(mob.hp, a, sizeof(a)), "SP", Grouped(mob.sp, b, sizeof(b)));
    row("EXP de base", Grouped(mob.base_exp, a, sizeof(a)),
        "EXP de job", Grouped(mob.job_exp, b, sizeof(b)));

    _snprintf_s(a, sizeof(a), _TRUNCATE, "%u ~ %u", mob.atk_min, mob.atk_max);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%u ~ %u", mob.matk_min, mob.matk_max);
    row("ATK", a, "MATK", b);

    _snprintf_s(a, sizeof(a), _TRUNCATE, "%u (+%u)", mob.def, mob.def2);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%u (+%u)", mob.mdef, mob.mdef2);
    row(msgstr::Utf8(kMsiDef), a, msgstr::Utf8(kMsiMdef), b);

    _snprintf_s(a, sizeof(a), _TRUNCATE, "%u", mob.str);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%u", mob.agi);
    row("STR", a, "AGI", b);
    _snprintf_s(a, sizeof(a), _TRUNCATE, "%u", mob.vit);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%u", mob.int_);
    row("VIT", a, "INT", b);
    _snprintf_s(a, sizeof(a), _TRUNCATE, "%u", mob.dex);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%u", mob.luk);
    row("DEX", a, "LUK", b);

    // `speed` est le temps de marche d'UNE case, en millisecondes : plus il est
    // petit, plus le monstre est rapide. Affiché tel quel, le chiffre se lit à
    // l'envers de l'intuition. On le retourne en cases par seconde, l'unité dans
    // laquelle un joueur pense sa fuite.
    _snprintf_s(a, sizeof(a), _TRUNCATE, "%u case(s)", mob.range_atk);
    if (mob.speed > 0) {
      const unsigned tenths = (10000u + mob.speed / 2) / mob.speed;
      _snprintf_s(b, sizeof(b), _TRUNCATE, "%u,%u case/s", tenths / 10,
                  tenths % 10);
    } else {
      _snprintf_s(b, sizeof(b), _TRUNCATE, "immobile");
    }
    row("Portée d'attaque", a, "Vitesse de dépl.", b);

    if (mob.mvp_exp > 0)
      row("EXP MVP", Grouped(mob.mvp_exp, a, sizeof(a)), "", "");
    ImGui::EndTable();
  }

  // ── Dérivés + simulation contre le personnage ─────────────────────────────
  const int mob_hit  = static_cast<int>(mob.level) + static_cast<int>(mob.dex);
  const int mob_flee = static_cast<int>(mob.level) + static_cast<int>(mob.agi);

  ImGui::Spacing();
  ImGui::TextColored(kTitle, "Précision et esquive");
  if (ImGui::BeginTable("##mi_hitflee", 4,
                        ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn(); Label("HIT");
    Help("Précision du monstre, comparée à votre FLEE.\n\n"
         "HIT = niveau du monstre + sa DEX.");
    ImGui::TableNextColumn(); ImGui::Text("%d", mob_hit);
    ImGui::TableNextColumn(); Label("FLEE");
    Help("Esquive du monstre, comparée à votre HIT.\n\n"
         "FLEE = niveau du monstre + son AGI.");
    ImGui::TableNextColumn(); ImGui::Text("%d", mob_flee);

    ImGui::TableNextRow();
    ImGui::TableNextColumn(); Label("Critique");
    Help("Sur ce serveur, un monstre ne fait JAMAIS de coup critique : la "
         "mécanique est réservée aux joueurs et aux mercenaires. Ce n'est pas "
         "une donnée manquante, c'est zéro par conception.");
    ImGui::TableNextColumn(); ImGui::TextColored(kGreen, "aucun");
    ImGui::TableNextColumn(); Label("Esquive parfaite");
    Help("De même, un monstre n'a pas d'esquive parfaite : elle est réservée "
         "aux joueurs.");
    ImGui::TableNextColumn(); ImGui::TextColored(kGreen, "aucune");
    ImGui::EndTable();
  }

  const OwnCombatStats own = ReadOwnCombatStats();
  if (own.valid) {
    ImGui::Spacing();
    ImGui::TextColored(kTitle, "Face à votre personnage");
    if (ImGui::BeginTable("##mi_versus", 2,
                          ImGuiTableFlags_SizingStretchProp |
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
      auto line = [](const char* label, const char* help, const char* value,
                     ImVec4 color) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); Label("%s", label); Help(help);
        ImGui::TableNextColumn(); ImGui::TextColored(color, "%s", value);
      };
      char v[64];

      const int you_hit = HitChancePct(own.hit, mob_flee);
      _snprintf_s(v, sizeof(v), _TRUNCATE, "%d %%", you_hit);
      line("Vous le touchez",
           "Chance qu'une attaque normale porte.\n\n"
           "80 % + votre HIT - le FLEE du monstre, bornée entre 5 % et 100 %.\n\n"
           "Votre HIT vaut votre NIVEAU DE BASE + votre DEX : la moitié du "
           "calcul vient donc de votre niveau, pas de vos points. Le serveur "
           "plafonne la part du niveau (600 actuellement), au-delà seule la DEX "
           "compte encore.\n\n"
           "Ne tient compte ni des compétences, ni des cartes conditionnelles.",
           v, you_hit >= 90 ? kGreen : (you_hit <= 40 ? kRed : kAmber));

      // ── Critique ──────────────────────────────────────────────────────────
      // `own.crit` est en POUR CENT ENTIERS (le serveur envoie `cri/10`) ; la
      // formule du serveur travaille en DIXIÈMES. D'où le ×10 avant de
      // soustraire `LUK * 2` (battle.cpp:3068 — le facteur 3 ne vaut que dans
      // l'autre sens, monstre -> joueur).
      //
      // 🔴 Borné à 100 %. Le jet est `rnd()%1000 < cri` : au-delà de 1000
      // dixièmes il est toujours vrai, et afficher « 314 % » laisserait croire
      // à un multiplicateur de DÉGÂTS, ce que ce chiffre n'est pas.
      int crit_tenths = own.crit * 10 - static_cast<int>(mob.luk) * 2;
      if (crit_tenths < 0) crit_tenths = 0;
      if (crit_tenths > 1000) crit_tenths = 1000;
      _snprintf_s(v, sizeof(v), _TRUNCATE, "%d,%d %%", crit_tenths / 10,
                  crit_tenths % 10);
      line("Vos critiques sur lui",
           "Chance qu'une attaque soit un coup critique. Ce n'est PAS un "
           "multiplicateur de dégâts.\n\n"
           "Votre CRIT moins 0,2 point par LUK du monstre. Un critique "
           "remplace le jet de précision (il ne rate jamais) et ignore la DEF.",
           v, crit_tenths > 0 ? kBlue : kLabel);

      // ── Être touché ───────────────────────────────────────────────────────
      // L'esquive parfaite est intégrée DANS ce chiffre plutôt qu'affichée à
      // part : elle se joue avant tout le reste (battle.cpp:5451) et annule
      // l'attaque, donc « la probabilité d'être touché » n'a de sens qu'une
      // fois qu'elle est déduite. Deux lignes séparées obligeaient le lecteur à
      // faire la multiplication lui-même.
      // `own.pdodge` est en POUR CENT ENTIERS, comme `own.crit`.
      const int raw = HitChancePct(mob_hit, own.flee);
      int pdodge = own.pdodge;
      if (pdodge < 0) pdodge = 0;
      if (pdodge > 100) pdodge = 100;
      const int net = raw * (100 - pdodge) / 100;
      _snprintf_s(v, sizeof(v), _TRUNCATE, "%d %%", net);
      line("Il vous touche",
           "Chance qu'une de ses attaques vous atteigne réellement, esquive "
           "parfaite déduite.\n\n"
           "80 % + son HIT - votre FLEE, bornée entre 5 % et 100 %, puis "
           "amputée de votre esquive parfaite, qui annule l'attaque avant tout "
           "jet (elle bloque même un critique).\n\n"
           "Votre FLEE vaut votre NIVEAU DE BASE + votre AGI : monter de niveau "
           "vous fait esquiver davantage, sans dépenser un point. Le serveur "
           "plafonne la part du niveau (600 actuellement), au-delà seule l'AGI "
           "compte encore.\n\n"
           "Votre FLEE chute par ailleurs de 10 % par monstre au-delà du "
           "deuxième qui vous prend pour cible : en pack, le chiffre réel est "
           "plus élevé.",
           v, net <= 20 ? kGreen : (net >= 80 ? kRed : kAmber));
      ImGui::EndTable();
    }
  }
}

void MonsterInfoWindow::DrawResistTab(MobInfo& mob) {
  Label("Pourcentage de dégâts ENCAISSÉS par élément d'attaque. 100 %% = normal, "
        "0 %% = immunisé, au-dessus de 100 %% = faiblesse.");
  ImGui::Text("Élément du monstre : %s niveau %u", ElementName(mob.element),
              mob.element_lv);
  ImGui::Separator();
  if (ImGui::BeginTable("##mi_resist", 5,
                        ImGuiTableFlags_SizingStretchSame |
                            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    for (int i = 0; i < 10; ++i) {
      ImGui::TableNextColumn();
      Label("%s", ResistLabel(i));
      ImGui::TextColored(ResistColor(mob.resist[i]), "%d %%", mob.resist[i]);
    }
    ImGui::EndTable();
  }
}

void MonsterInfoWindow::DrawDropsTab(MobInfo& mob) {
  if (mob.drops.empty()) {
    Label("Ce monstre ne laisse rien tomber.");
    return;
  }
  static ImGuiTextFilter s_filter;
  ImGui::SetNextItemWidth(180.0f);
  s_filter.Draw("Filtrer (objet)");

  const ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_Sortable |
                                ImGuiTableFlags_SizingStretchProp;
  if (ImGui::BeginTable("##mi_drops", 3, flags)) {
    ImGui::TableSetupColumn("Objet", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Taux", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_PreferSortDescending |
                                        ImGuiTableColumnFlags_DefaultSort,
                            70.0f);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 95.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Vue triée : on ne touche PAS au modèle (l'ordre du serveur reste la
    // référence, et le paquet peut le réécrire à tout moment).
    std::vector<const Drop*> view;
    view.reserve(mob.drops.size());
    for (const Drop& d : mob.drops)
      if (s_filter.PassFilter(d.name.c_str())) view.push_back(&d);
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
      if (sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(view.begin(), view.end(), [&](const Drop* a, const Drop* b) {
          int c;
          if (sp.ColumnIndex == 1)
            c = (a->rate < b->rate) ? -1 : (a->rate > b->rate ? 1 : 0);
          else if (sp.ColumnIndex == 2)
            c = (a->kind < b->kind) ? -1 : (a->kind > b->kind ? 1 : 0);
          else
            c = _stricmp(a->name.c_str(), b->name.c_str());
          return asc ? c < 0 : c > 0;
        });
      }
    }

    for (const Drop* dp : view) {
      const Drop& d = *dp;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();

      // Icône, puis nom cliquable -> fiche de l'objet (le pendant du lien
      // inverse posé dans la table des sources de la fiche d'item).
      const ro::IconTex icon = ro::ItemIcon(d.nameid);
      const float line = ImGui::GetTextLineHeight();
      if (icon.tex != nullptr) {
        ImGui::Image(reinterpret_cast<ImTextureID>(icon.tex),
                     ImVec2(line * 1.4f, line * 1.4f));
        ImGui::SameLine();
      }
      const ImVec4 kLink(0.10f, 0.30f, 0.85f, 1.0f);
      ImGui::TextColored(kLink, "%s",
                         d.name.empty() ? "(objet inconnu)" : d.name.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y),
                                            ImVec2(mx.x, mx.y),
                                            ImGui::GetColorU32(kLink));
      }
      // Clic droit = description, comme partout ailleurs dans le projet.
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right) ||
          ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        itemcell::DeferDescById(d.nameid, /*view=*/0, /*location=*/0,
                                static_cast<int>(m.x), static_cast<int>(m.y));
      }

      ImGui::TableNextColumn();
      char rate[32];
      RateText(rate, sizeof(rate), d.rate);
      ImGui::Text("%s", rate);

      ImGui::TableNextColumn();
      if (d.kind == 1)
        ImGui::TextColored(kAmber, "Récompense MVP");
      else
        Label("Drop normal");
    }
    ImGui::EndTable();
  }
}

void MonsterInfoWindow::DrawSpawnsTab(MobInfo& mob) {
  if (mob.spawns.empty()) {
    Label("Aucun spawn permanent connu : ce monstre n'apparaît que par script "
          "(invocation, quête, instance) ou n'est plus placé sur aucune carte.");
    return;
  }
  if (ImGui::BeginTable("##mi_spawns", 2,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Carte", ImGuiTableColumnFlags_WidthStretch |
                                         ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Nombre",
                            ImGuiTableColumnFlags_WidthFixed |
                                ImGuiTableColumnFlags_PreferSortDescending,
                            80.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    std::vector<const Spawn*> view;
    view.reserve(mob.spawns.size());
    for (const Spawn& s : mob.spawns) view.push_back(&s);
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
      if (sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(view.begin(), view.end(), [&](const Spawn* a, const Spawn* b) {
          const int c = (sp.ColumnIndex == 1)
                            ? ((a->qty < b->qty) ? -1 : (a->qty > b->qty ? 1 : 0))
                            : _stricmp(a->map.c_str(), b->map.c_str());
          return asc ? c < 0 : c > 0;
        });
      }
    }
    for (const Spawn* s : view) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%s", s->map.c_str());
      ImGui::TableNextColumn(); ImGui::Text("%u", s->qty);
    }
    ImGui::EndTable();
  }
  Label("Les spawns d'instance et les invocations de script ne sont pas comptés.");
}

void MonsterInfoWindow::DrawSkillsTab(MobInfo& mob) {
  Label("Clic sur une compétence : sa description (fenêtre du client).");
  if (ImGui::BeginTable("##mi_skills", 3,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Sortable |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Compétence", ImGuiTableColumnFlags_WidthStretch |
                                              ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Niveau",
                            ImGuiTableColumnFlags_WidthFixed |
                                ImGuiTableColumnFlags_PreferSortDescending,
                            60.0f);
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    // Les noms sont déjà résolus (au décodage du paquet) et les compétences
    // innommables déjà écartées : il ne reste ici qu'à trier et à afficher.
    std::vector<const MobSkill*> view;
    view.reserve(mob.skills.size());
    for (const MobSkill& s : mob.skills) view.push_back(&s);
    if (ImGuiTableSortSpecs* sort = ImGui::TableGetSortSpecs()) {
      if (sort->SpecsCount > 0) {
        const ImGuiTableColumnSortSpecs& sp = sort->Specs[0];
        const bool asc = sp.SortDirection == ImGuiSortDirection_Ascending;
        std::sort(view.begin(), view.end(),
                  [&](const MobSkill* a, const MobSkill* b) {
                    int c;
                    if (sp.ColumnIndex == 1)
                      c = (a->lv < b->lv) ? -1 : (a->lv > b->lv ? 1 : 0);
                    else if (sp.ColumnIndex == 2)
                      c = (a->id < b->id) ? -1 : (a->id > b->id ? 1 : 0);
                    else
                      c = _stricmp(a->name.c_str(), b->name.c_str());
                    return asc ? c < 0 : c > 0;
                  });
      }
    }

    for (const MobSkill* s : view) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      const ImVec4 kLink(0.10f, 0.30f, 0.85f, 1.0f);
      ImGui::TextColored(kLink, "%s", s->name.c_str());
      if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 mn = ImGui::GetItemRectMin();
        const ImVec2 mx = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(mn.x, mx.y), ImVec2(mx.x, mx.y),
                                            ImGui::GetColorU32(kLink));
      }
      // On ENREGISTRE au clic, on OUVRE au relâchement (FlushPending) : ouvrir
      // tout de suite ferait passer la description derrière la fiche, qui garde
      // le focus tant que le bouton est enfoncé.
      if (ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
          ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        pending_skill_desc_ = s->id;
        pending_skill_x_ = static_cast<int>(m.x);
        pending_skill_y_ = static_cast<int>(m.y);
      }
      ImGui::TableNextColumn(); ImGui::Text("%u", s->lv);
      ImGui::TableNextColumn(); Label("%u", s->id);
    }
    ImGui::EndTable();
  }
}

void MonsterInfoWindow::FlushPending() {
  // 🔴 HORS frame ImGui. Ouvrir la fenêtre de description rejoue un chemin NATIF
  // (MakeWindow + OnMsg) : le faire entre NewFrame() et Render() est proscrit
  // (cf. feedback_no_native_cmd_during_imgui_frame). Appelée par
  // Bourgeon::OnProcessInput, comme WeaponRefineWindow::FlushPending.
  if (pending_skill_desc_ == 0) return;
  // 🔴 Et pas seulement hors frame : il faut aussi que le bouton soit RELÂCHÉ.
  // Tant qu'il est enfoncé, la fiche de monstre garde le focus et repasse devant
  // la description à la frame suivante — un clic LONG ouvrait donc la description
  // derrière la fenêtre. Même condition que `itemcell::FlushDeferredDesc`.
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
      ImGui::IsMouseDown(ImGuiMouseButton_Right))
    return;
  const int id = pending_skill_desc_;
  pending_skill_desc_ = 0;
  OpenSkillDesc(id, pending_skill_x_, pending_skill_y_);
}

bool MonsterInfoWindow::DrawSettings() {
  bool changed = false;
  if (ImGui::Checkbox("Animer le sprite du monstre", &animate_)) changed = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "La fenêtre native n'affiche que la première image de l'animation "
        "d'attente.\nDécoché, on la reproduit à l'identique.");
  if (ImGui::Checkbox("Afficher les Gardiens de forteresse", &show_guardians_))
    changed = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        "Le client d'origine refuse d'ouvrir la fiche des cinq Gardiens\n"
        "(Archer, Knight, Soldier, Sword, Bow) : leurs PV resteraient cachés\n"
        "pendant la Guerre d'Emperium. Coché, la fiche s'ouvre quand même.");
  return changed;
}
