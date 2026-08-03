#include "features/windows/monster_info_window.h"

#include <Windows.h>

#include <algorithm>
#include <cfloat>   // FLT_MAX (contraintes de taille de la fenêtre)
#include <cmath>    // std::sin (sursaut du sprite quand on le titille)
#include <cstdarg>  // va_list (helper Label)
#include <cstdio>
#include <cstring>

#include "bourgeon.h"
#include "features/item_cell.h"
#include "features/systems/bourgeon_opcodes.h"
#include "imgui.h"
#include "ragnarok/item_db.h"   // itemdb::kSkillDesc* (fenêtre native 0x2E)
#include "ragnarok/lua.h"       // lua::State / GetField / PCall (GetSkillDescript)
#include "ragnarok/msgstring.h"
#include "ragnarok/uiwnd.h"     // MakeWindow / OnMsg / SetPos (description de skill)
#include "ui/icon_cache.h"
#include "ui/ro_imgui.h"        // ro::BeginRoDescWindow, ro::LocalToUtf8 (CP949)
#include "ui/sprite_view.h"     // cadence du .act + son du sprite (interaction)

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

// ── Poses d'un .act de monstre ───────────────────────────────────────────────
// Les actions sont rangées `motion * 8 + direction` (0 = sud, 2 = ouest,
// 4 = nord, 6 = est). Les motions d'un monstre, dans l'ordre du format :
// 0 attente, 1 marche, 2 attaque, 3 dégât, 4 mort.
//
// 🔴 Toutes ne sont pas toujours présentes — un .act de monstre custom peut
// n'avoir que l'attente. On TESTE donc le fichier (nombre d'images) au lieu de
// supposer, et on se replie sur l'attente dans la même direction, puis sur
// l'action 0, la seule qui existe forcément.
constexpr unsigned kMotionAttack = 2;
constexpr unsigned kMotionHurt   = 3;
constexpr unsigned kMotionDie    = 4;

// ── Le rythme de la chatouille ───────────────────────────────────────────────
// Temps mort entre deux clics : il fixe aussi la cadence de l'escalade, d'où
// des seuils courts — six clics font douze secondes, et personne ne clique une
// minute pour voir ce qui se passe.
constexpr float kPokeCooldownSec  = 2.0f;
constexpr int   kPokesPerRiposte  = 3;    // une chatouille sur trois : il riposte
constexpr int   kPokesBeforeDeath = 6;    // et à la sixième, il fait le mort
constexpr float kDeathHoldSeconds = 1.6f; // temps passé à terre avant de se relever

unsigned MobAction(const ro::MobSpriteRes& res, unsigned motion, int dir) {
  const unsigned d = static_cast<unsigned>(dir) & 7u;
  const unsigned a = motion * 8u + d;
  if (ro::MobActionFrameCount(res, a) > 0) return a;
  return (ro::MobActionFrameCount(res, d) > 0) ? d : 0u;
}

// ── Ce que l'action a à faire entendre, et QUAND ─────────────────────────────
//
// 🔴 Le son d'un .act n'appartient pas au fichier mais à l'IMAGE : chaque image
// porte un `sound id` qui indexe la table de sons de l'Act (-1 = muette), et une
// action pose son wav sur son image de contact, pas sur la première. Il n'y a
// donc pas de « son du monstre » à jouer au clic — c'est le DESSIN qui déclenche
// le son, quand il atteint l'image qui le porte.
//
// La même table contient aussi des MARQUEURS sans extension, dont « atk » :
// l'image où le coup porte, sans wav associé. Quand c'est tout ce qu'a l'action,
// on tient quand même l'instant juste pour poser le son de coup générique.
enum class PokeAudio {
  kFrameWav,  // vrai wav dans le .act : le dessin le déclenchera, image par image
  kOnMarker,  // pas de wav mais un marqueur : repli calé sur SON image
  kNow,       // ni l'un ni l'autre : repli immédiat, sinon le clic est muet
};

// Dernière image de l'action qui dessine réellement quelque chose.
//
// 🔴 Une animation de MORT se termine presque toujours sur des images VIDES :
// le corps disparaît, c'est le fichier qui le veut. Tenir la dernière image du
// .act laisserait donc un cadre vide pendant tout le temps où le monstre est
// censé rester à terre. On remonte jusqu'à la dernière image qui a des calques.
int LastVisibleFrame(const ro::MobSpriteRes& res, unsigned action) {
  const int n = ro::MobActionFrameCount(res, action);
  for (int f = n - 1; f > 0; --f) {
    ro::SpriteQuad quad;  // un seul suffit : on teste la présence, pas le contenu
    if (ro::SpriteResolveFrame(res.sprite, action, static_cast<unsigned>(f),
                               &quad, 1) > 0)
      return f;
  }
  return 0;
}

PokeAudio ClassifyActionAudio(const ro::MobSpriteRes& res, unsigned action) {
  const int n = ro::MobActionFrameCount(res, action);
  bool marker = false;
  for (int f = 0; f < n; ++f) {
    const unsigned uf = static_cast<unsigned>(f);
    if (ro::SpriteFrameSound(res.sprite, action, uf)) return PokeAudio::kFrameWav;
    if (ro::SpriteFrameEvent(res.sprite, action, uf)) marker = true;
  }
  return marker ? PokeAudio::kOnMarker : PokeAudio::kNow;
}

// ── Jouer un wav du client ───────────────────────────────────────────────────
// Sound_Play3D, aux coordonnées de l'auditeur (centré, volume plein). Il honore
// tout seul le réglage « effets sonores » du joueur, et on est EN JEU ici, donc
// OptionInfo est chargé — pas besoin du contournement `Sound_PlaySample2D` de la
// parade de login (features/overlays/login_parade.cc, qui tourne AVANT).
//
// ⚠ La fonction se termine par `RET 0x20` : elle dépile un dword de plus que ses
// sept paramètres visibles. L'oublier corrompt la pile de l'APPELANT, et le
// crash tombe après le retour — cf. le même appel dans minigames/roggle.cc.
constexpr uintptr_t kSoundMgrPtr = 0x01253d0c;  // *ptr = SoundMgr (déréf 1x)
constexpr uintptr_t kSoundPlay3D = 0x00600770;
using PlaySound3DFn = void(__fastcall*)(void*, void*, const char*, float, float,
                                        float, int, int, float, int);

void PlayRoSound(const char* name) {
  if (name == nullptr || name[0] == '\0') return;
  __try {
    void* mgr = *reinterpret_cast<void**>(kSoundMgrPtr);
    if (mgr != nullptr)
      reinterpret_cast<PlaySound3DFn>(kSoundPlay3D)(mgr, nullptr, name, 0.0f,
                                                    0.0f, 0.0f, 250, 40, 1.0f, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Repli quand le .act ne déclare aucun wav — le cas de la plupart des monstres.
// C'est le son de COUP du jeu : celui qu'on entend en tapant le monstre.
constexpr const char* kPokeFallbackWav = "effect\\EF_hit2.wav";

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
// Nom : wrapper Lua natif `GetSkillName(id)` (`__cdecl`), la source qu'utilisent
// la fenêtre de skills et le tooltip natif — localisée, donc préférée. Mais elle
// ne couvre que les compétences de JOUEUR et rend « Unknown-Skill » sur tout le
// reste : les `NPC_*` d'un monstre n'y sont pas. 🔴 C'est pour cela que le
// paquet transporte AUSSI le nom vu par skill_db (serveur), en repli.
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

// 🔴 DEUX FICHIERS, PAS UN. Le nom et la description d'une compétence ne vivent
// pas au même endroit, et ajouter l'un n'ajoute pas l'autre :
//   * le NOM         -> skillinfoz\skillinfolist.lub (`SKILL_INFO_LIST[..].SkillName`),
//                       ce que lit `GetSkillName` ci-dessus ;
//   * la DESCRIPTION -> skillinfoz\skilldescript.lub (`SKILL_DESCRIPT[..]`), ce que
//                       lit le global Lua `GetSkillDescript`.
// Les compétences ajoutées à la main — typiquement les `NPC_*` — n'ont souvent que
// la seconde. Gater le lien sur le NOM les rendait donc muettes alors que leur
// description existait.
//
// On interroge donc la source qui décide vraiment : la fenêtre native 0x2E
// n'affiche que ce que ce global lui rend (RE sub_A198C0, qui l'appelle puis
// découpe le résultat sur \r\n). Chaîne vide = rien à montrer, pas de lien.
//
// 🔴 ORDRE DES ARGUMENTS : `GetSkillDescript(jobId, skillId, 0)` — le JOB
// D'ABORD, contrairement à ce que le nom laisse croire. Lu dans les push du
// natif en 0x00A199[71..8D] (cdecl, donc droite→gauche : `push &out`, `push 0`,
// `push <skillId>`, `push g_Own_JobId`, `push "ddd>s"`). Inverser les deux fait
// chercher SKILL_DESCRIPT[<job>] : nil pour tout le monde, donc AUCUNE ligne
// cliquable — et rien qui ressemble à une erreur.
//
// API C Lua BRUTE et nom STATIQUE : le wrapper varargs du client détruit la
// std::string qu'on lui passe par valeur. Cf. `ResolveOptName` (item_desc_window).
//
// Le job : la MÊME source que le natif — le global qu'il pousse lui-même, pas le
// getter de session. Répondre « la fenêtre 0x2E aura-t-elle quelque chose à
// dire ? » n'a de sens que si on l'interroge avec ses propres entrées.
constexpr uintptr_t kOwnJobIdAddr = 0x015fb9c8;  // g_Own_JobId

// SEH ISOLÉ : pas de temporaire C++ dans cette fonction (C2712).
bool SkillHasDescSEH(int skill_id) {
  bool ok = false;
  __try {
    void* L = lua::State();
    if (L) {
      const int job = *reinterpret_cast<const int*>(kOwnJobIdAddr);
      lua::GetField(L, lua::kGlobalsIndex, "GetSkillDescript");
      lua::PushNumber(L, static_cast<double>(job));       // 1er : le JOB
      lua::PushNumber(L, static_cast<double>(skill_id));  // 2e  : la compétence
      lua::PushNumber(L, 0.0);
      if (lua::PCall(L, 3, 1, 0) == 0) {
        size_t len = 0;
        const char* s = lua::ToLString(L, -1, &len);
        ok = (s != nullptr && len > 0);
      }
      lua::SetTop(L, -2);  // dépile le résultat, ou le message d'erreur
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
  poke_action_ = -1;
  poke_freeze_ = false;
  poke_ready_at_ = 0.0f;
  poke_count_ = 0;
  cache_.clear();
}

void MonsterInfoWindow::Open(uint32_t mob_id, bool by_view) {
  if (mob_id == 0) return;
  // Changement de monstre : l'animation ponctuelle en vol ne vaut plus rien —
  // son numéro d'action désigne une pose de l'ANCIEN .act. L'orientation, elle,
  // survit : c'est un réglage de lecture, pas un état du monstre.
  if (mob_id != current_id_) {
    poke_action_ = -1;
    poke_freeze_ = false;
    poke_ready_at_ = 0.0f;  // absolu : le garder ferait taire le nouveau monstre
    poke_count_ = 0;
  }
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
      if (!need(5)) break;  // [id:2][lv:2][namelen:1]
      MobSkill s;
      s.id = u16();
      s.lv = u16();
      const uint8_t namelen = u8();
      if (!need(namelen)) break;
      // Nom du SERVEUR (skill_db : `desc`, sinon l'AegisName). ASCII, tel quel.
      std::string from_server(reinterpret_cast<const char*>(p), namelen);
      p += namelen;

      // 🔴 AUCUNE compétence n'est écartée. Trois sources, par ordre de
      // préférence : le nom du client (localisé, celui de la fenêtre de skills),
      // puis celui du serveur, puis l'id brut. Le client ne sait nommer que les
      // compétences de JOUEUR — les `NPC_*`, qui font l'essentiel de l'arsenal
      // d'un monstre, ne vivent que dans skill_db côté serveur.
      char cp949[128] = {0};
      s.client_named = SkillNameCp949(s.id, cp949, sizeof(cp949));
      if (s.client_named) {
        const char* utf8 = ro::LocalToUtf8(cp949);
        s.name = (utf8 && *utf8) ? utf8 : cp949;
      }
      if (s.name.empty()) s.name = from_server;
      if (s.name.empty()) {
        char fallback[32];
        _snprintf_s(fallback, sizeof(fallback), _TRUNCATE, "#%u", s.id);
        s.name = fallback;
      }
      // Résolu ici, pas au rendu : c'est un appel Lua, il n'a rien à faire dans
      // une frame ImGui — et il ne dépend que de l'id.
      s.has_desc = SkillHasDescSEH(s.id);
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
  const bool have_sprite = ro::LoadMobSprite(static_cast<int>(sprite_class), &sprite_);

  const ImVec2 box(116.0f, 132.0f);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + box.x, p0.y + box.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(24, 22, 20, 90), 3.0f);

  const float now = static_cast<float>(ImGui::GetTime());

  // Pose par défaut : l'attente, dans l'orientation choisie à la molette.
  unsigned action = MobAction(sprite_, /*motion=*/0u, sprite_dir_);
  float clock = animate_ ? now : 0.0f;
  float ms    = animate_ ? 130.0f : 0.0f;
  float shake = 0.0f;

  // Chatouille en cours : elle PRIME sur l'attente. Son horloge repart du clic
  // (image 0) et on coupe à la fin de l'action — `DrawSprite` boucle, c'est nous
  // qui décidons que celle-ci ne se joue qu'une fois. Elle s'anime même quand
  // l'animation d'attente est coupée : c'est une réaction, pas une décoration.
  if (poke_action_ >= 0) {
    if (now < poke_end_) {
      action = static_cast<unsigned>(poke_action_);
      clock  = now - poke_start_;
      ms     = 130.0f;

      const float anim_len = poke_anim_end_ - poke_start_;
      // Mort : passé la fin de l'animation, on TIENT l'image du corps à terre
      // (`poke_freeze_clock_`, calculée au clic). Sans ce gel, `DrawSprite`
      // reboucle et le cadavre se relève tout seul, en boucle.
      if (poke_freeze_ && clock >= anim_len) clock = poke_freeze_clock_;

      // Sursaut : oscillation horizontale qui s'éteint avec l'animation. Elle
      // s'arrête AVEC elle — un corps à terre ne tremble plus.
      if (now < poke_anim_end_) {
        const float left = (poke_anim_end_ - now) / anim_len;
        shake = std::sin((now - poke_start_) * 47.0f) * 3.5f * left;
      }

      // ── Son SYNCHRONISÉ à l'image ────────────────────────────────────────
      // C'est l'IMAGE qui porte le son, pas l'action : on demande l'index que
      // `DrawSprite` va dessiner (mêmes paramètres -> même calcul, sprite_view
      // n'en a qu'un) et on ne teste qu'au CHANGEMENT d'image. Le wav de
      // l'attaque part ainsi sur son image de contact, et pas au clic.
      //
      // ⚠ Uniquement pendant la réaction : des .act portent aussi un son sur
      // leur animation d'ATTENTE, et la fiche reste ouverte — elle se mettrait
      // à couiner toute seule, en boucle, tant qu'on la regarde.
      const unsigned frame =
          ro::SpriteFrameIndex(sprite_.sprite, action, clock, ms);
      if (static_cast<int>(frame) != poke_frame_) {
        poke_frame_ = static_cast<int>(frame);
        if (const char* wav = ro::SpriteFrameSound(sprite_.sprite, action, frame)) {
          PlayRoSound(wav);
        } else if (poke_hit_pending_ &&
                   ro::SpriteFrameEvent(sprite_.sprite, action, frame)) {
          // Action muette mais marquée (« atk ») : le son de coup part sur
          // l'image du contact, une seule fois par chatouille.
          poke_hit_pending_ = false;
          PlayRoSound(kPokeFallbackWav);
        }
      }
    } else {
      poke_action_ = -1;
    }
  }

  // allow_upscale = false : taille RÉELLE du sprite, le cadre ne sert qu'à borner
  // ce qui dépasse. Les petits monstres restent petits — c'est une information de
  // gabarit, pas un défaut de cadrage.
  const ImVec2 s0(p0.x + shake, p0.y);
  const ImVec2 s1(p1.x + shake, p1.y);
  const bool drawn = ro::DrawMobSprite(dl, sprite_, s0, s1, clock, action, ms,
                                       /*allow_upscale=*/false);
  // 🔴 Le placeholder ne dépend PAS de `drawn` : `DrawSprite` rend aussi false
  // quand l'image est simplement VIDE (aucun calque), et une animation de mort
  // se termine justement sur des images vides — le corps disparaît, c'est le
  // fichier qui le veut. Seul un .spr/.act absent (mob custom sans art) mérite
  // une explication ; une image vide se dessine… vide.
  if (!drawn && !have_sprite) {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    dl->AddCircleFilled(c, 18.0f, IM_COL32(120, 110, 100, 160), 16);
    dl->AddText(ImVec2(p0.x + 6.0f, p1.y - 18.0f), IM_COL32(200, 190, 170, 200),
                "pas de sprite");
  }

  // ── Le cadre est une ZONE ACTIVE ──────────────────────────────────────────
  // `InvisibleButton` plutôt que `Dummy` : il avance le curseur pareil, mais il
  // fait du cadre un item — donc survolable, cliquable, et propriétaire de la
  // molette. Il est déclaré APRÈS le dessin, ce qui ne change rien à l'ordre de
  // rendu (le sprite est déjà dans la liste de dessin de la fenêtre) et évite
  // d'avoir à repositionner le curseur.
  ImGui::InvisibleButton("##mi_sprite", box);
  if (ImGui::IsItemHovered()) {
    ro::SetHoverCursor(2);  // main : ça se manipule
    // Molette = rotation, huit directions avec repli d'un bout à l'autre. On la
    // CONSOMME, sinon la fiche défile en même temps qu'on tourne le monstre.
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      sprite_dir_ = (sprite_dir_ + (wheel > 0.0f ? 1 : 7)) & 7;
      ImGui::GetIO().MouseWheel = 0.0f;
    }
  }
  // Le temps mort est SILENCIEUX : un clic trop tôt ne fait rien, sans message
  // ni curseur barré. C'est une réaction, pas une commande — la refuser bruyamment
  // attirerait l'attention sur une mécanique qui doit rester discrète.
  if (ImGui::IsItemClicked() && now >= poke_ready_at_) PokeSprite();
  // Aide au survol, temporisée : elle ne doit pas masquer le sprite dès qu'on
  // passe la souris dessus.
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("Molette : tourner le monstre\nClic : le titiller");
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

void MonsterInfoWindow::PokeSprite() {
  // ── Ce que le monstre répond ──────────────────────────────────────────────
  // Il encaisse, riposte une fois sur trois, et finit par faire le mort si on
  // s'acharne — après quoi il se relève et le compteur repart de zéro. C'est
  // toute la règle du jeu.
  ++poke_count_;

  // 🔴 On ne le fait mourir que si son .act a VRAIMENT une animation de mort :
  // `MobAction` se replie silencieusement sur l'attente quand la motion manque,
  // et on aurait alors un monstre « mort » debout, figé une seconde et demie.
  const unsigned dir_bits   = static_cast<unsigned>(sprite_dir_) & 7u;
  const unsigned die_action = MobAction(sprite_, kMotionDie, sprite_dir_);
  const bool     dies       = (die_action == kMotionDie * 8u + dir_bits) &&
                              poke_count_ >= kPokesBeforeDeath;

  unsigned action;
  if (dies) {
    action = die_action;
    poke_count_ = 0;  // il repart intact en se relevant
  } else {
    action = MobAction(
        sprite_,
        (poke_count_ % kPokesPerRiposte) == 0 ? kMotionAttack : kMotionHurt,
        sprite_dir_);
  }

  // Durée = ce que déclare le .act, pas une constante : d'un monstre à l'autre,
  // l'animation de dégât va de deux à une dizaine d'images, à des cadences qui
  // n'ont rien à voir. Le plancher évite qu'un .act à une seule image ne rende
  // la réaction invisible.
  const int frames = (std::max)(1, ro::MobActionFrameCount(sprite_, action));
  float interval_ms = ro::SpriteFrameIntervalMs(sprite_.sprite, action);
  if (interval_ms <= 0.0f) interval_ms = 130.0f;  // même repli que le dessin
  const float anim_len = (std::max)(0.20f, frames * interval_ms / 1000.0f);

  poke_action_   = static_cast<int>(action);
  poke_start_    = static_cast<float>(ImGui::GetTime());
  poke_anim_end_ = poke_start_ + anim_len;
  // La mort ne se termine pas quand l'animation s'arrête : le monstre reste à
  // terre — dernière image tenue — le temps qu'on comprenne ce qu'on a fait.
  poke_freeze_ = dies;
  poke_end_    = poke_anim_end_ + (dies ? kDeathHoldSeconds : 0.0f);
  // Sur QUELLE image on le tient : la dernière VISIBLE, pas la dernière du
  // fichier. Le « + 0,5 » vise le milieu de son créneau, donc `SpriteFrameIndex`
  // rendra bien cette image-là (même intervalle que le dessin, même calcul).
  poke_freeze_clock_ =
      (LastVisibleFrame(sprite_, action) + 0.5f) * interval_ms / 1000.0f;
  // Temps mort avant le clic suivant. Le `max` couvre la mort d'un seul tenant :
  // on ne tape pas un monstre à terre, et la reprise se fait après qu'il s'est
  // relevé, pas pendant.
  poke_ready_at_ = (std::max)(poke_start_ + kPokeCooldownSec, poke_end_ + 0.30f);
  // -1 et non 0 : la toute première image dessinée doit compter comme un
  // CHANGEMENT d'image, sinon le son qu'elle porte serait sauté.
  poke_frame_ = -1;

  // On ne joue RIEN ici quand l'action a ses propres sons : c'est le dessin qui
  // les déclenchera, sur la bonne image. Le son de coup générique — celui qu'on
  // entend en tapant le monstre — ne sert que de repli : un clic sans retour ne
  // se lit pas comme une réaction.
  poke_hit_pending_ = false;
  switch (ClassifyActionAudio(sprite_, action)) {
    case PokeAudio::kFrameWav: break;                          // le dessin s'en charge
    case PokeAudio::kOnMarker: poke_hit_pending_ = true; break;  // à l'image « atk »
    case PokeAudio::kNow:      PlayRoSound(kPokeFallbackWav); break;
  }
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

    _snprintf_s(a, sizeof(a), _TRUNCATE, "%u%% (+%u)", mob.def, mob.def2);
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%u%% (+%u)", mob.mdef, mob.mdef2);
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

    // Les noms sont déjà résolus (au décodage du paquet) : il ne reste ici qu'à
    // trier et à afficher.
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
      // Cliquable = le client a une DESCRIPTION (skilldescript.lub), pas
      // seulement un nom : la fenêtre native 0x2E n'affiche rien d'autre. Les
      // autres s'affichent en texte simple — elles sont listées, elles ne
      // mentent juste pas sur ce qu'un clic donnerait.
      if (!s->has_desc) {
        ImGui::TextUnformatted(s->name.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", s->lv);
        ImGui::TableNextColumn(); Label("%u", s->id);
        continue;
      }
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
