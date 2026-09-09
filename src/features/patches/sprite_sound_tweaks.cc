#include "features/patches/sprite_sound_tweaks.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bourgeon.h"
#include "imgui.h"
#include "ui/ro_imgui.h"    // ro::RoCheckbox
#include "ui/ro_widgets.h"  // mui::Tooltip
#include "utils/hooking/hook_manager.h"
#include "utils/i18n.h"
#include "utils/memory_patch.h"  // mem::WriteCode

// ── Adresses (client 20250716, no-ASLR : addr Ghidra == live) ────────────────
namespace {

// `Actor_PlayFrameSounds` + 0x54 : le rembobinage du curseur d'images
// sonorisées, fait quand l'action de l'acteur vient de changer. Le récit complet
// est dans le header.
constexpr uintptr_t kFrameSoundRewind = 0x00c47254;

// `Sound_Play3D` — le seul chemin par lequel le client joue un `.wav` positionné.
constexpr uintptr_t kSoundPlay3D = 0x00600770;

// MOV DWORD PTR [ESI+44],0 | XOR EDI,EDI — les octets d'origine.
const uint8_t kRewindToZero[9] = {0xC7, 0x46, 0x44, 0x00, 0x00,
                                  0x00, 0x00, 0x33, 0xFF};

// OR DWORD PTR [ESI+44],-1 | OR EDI,-1 | NOP | NOP
// Neuf octets pour neuf : rien ne bouge en aval. Le OR écrit les drapeaux là où
// le XOR les écrivait, sans conséquence — le branchement suivant est un CMP deux
// instructions plus loin.
const uint8_t kRewindToMinusOne[9] = {0x83, 0x4E, 0x44, 0xFF, 0x83,
                                      0xCF, 0xFF, 0x90, 0x90};

// `Sound_Play3D` est un __thiscall qui dépile HUIT dwords (RET 0x20) : nom, x,
// y, z, distMax, distMin, volume, et un huitième que le natif passe toujours à
// 0. On l'émule en __fastcall avec un EDX factice, comme partout ailleurs dans
// ce projet (cf. ragnarok/audio.h, qui a payé une fois l'oubli du huitième).
using Play3DFn = int(__fastcall*)(void* self, void* edx, const char* wav_name,
                                  float x, float y, float z, int max_dist,
                                  int min_dist, float volume, int reserved);
Play3DFn g_play3d_orig = nullptr;

// Minuscules ASCII seulement : un nom de ressource coréen est en CP949, dont le
// second octet peut tomber dans A-Z — le toucher casserait le nom.
std::string LowerAscii(const char* text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  }
  return out;
}

int __fastcall Hooked_Play3D(void* self, void* edx, const char* wav_name,
                             float x, float y, float z, int max_dist,
                             int min_dist, float volume, int reserved) {
  if (g_play3d_orig == nullptr) return 0;
  auto* sprite_sounds = Bourgeon::Instance().sprite_sound_tweaks();
  if (sprite_sounds != nullptr && sprite_sounds->IsMuted(wav_name)) return 0;
  return g_play3d_orig(self, edx, wav_name, x, y, z, max_dist, min_dist, volume,
                       reserved);
}

// ── Ce que le correctif réveille, et que le joueur voudra peut-être taire ────
// Les noms sont ceux des `.act`, à la lettre — fautes de Gravity comprises
// (« doppleganger », « elder_wilow »). Le compte est celui des sprites de
// monstres du data.grf du client qui portent ce son sur l'image 0.
struct CandidateSound {
  const char* wav;
  const char* who;      // qui le porte, pour que le nom ne soit pas une énigme
  int sprite_count;
};

// Sur l'animation de MORT. Trois d'entre eux sont des sons de dégât que Gravity
// a posés là ; le correctif joue ce que disent les données, pas autre chose.
constexpr CandidateSound kDeathSounds[] = {
    {"doppleganger_die.wav", "Bio Lab, Doppelganger", 14},
    {"baphomet_die.wav",     "Baphomet",               4},
    {"ork_warrior_die1.wav", "Orc Warrior, Orc Archer", 4},
    {"osiris_die.wav",       "Osiris",                 3},
    {"worm_tail_die.wav",    "Wormtail, Stem Worm",    3},
    {"baphomet__die.wav",    "Baphomet d'event",       2},
    {"obeaune_die.wav",      "Iara",                   2},
    {"orc_baby_damage.wav",  "Orc Baby",               2},
    {"pupa_die.wav",         "Pupa, Thief Bug Egg",    2},
    {"wolf_die.wav",         "Vagabond Wolf",          2},
    {"creamy_die.wav",       "Creamy Fear",            1},
    {"elder_wilow_die.wav",  "Elder Willow",           1},
    {"obeaune_damage.wav",   "Obeaune",                1},
    {"thanatos_damage.wav",  "Memory of Thanatos",     1},
};

// Sur l'animation de COUP REÇU. Le correctif ne les REVEILLE pas — ils sonnent
// deja, des le deuxieme coup encaisse (cf. le header) — mais ils reviennent en
// boucle pendant un combat : ce sont les premiers qu'on voudra taire. Les neuf
// plus repandus des 28.
constexpr CandidateSound kHitSounds[] = {
    {"monster_feather.wav",  "Condor, Guard Dog, Desert Wolf", 19},
    {"monster_clothes.wav",  "Goblin, Gros Mummy",              9},
    {"monster_flesh.wav",    "chair",                           8},
    {"monster_poring.wav",   "Angeling et la famille Poring",   7},
    {"monster_plant.wav",    "champignons, plantes",            6},
    {"monster_reptiles.wav", "Anacondaq et les reptiles",       5},
    {"monster_insect.wav",   "Fabre, Hornet, Dustiness",        5},
    {"monster_shell.wav",    "Ant Egg et les carapaces",        4},
    {"monster_skelton.wav",  "les squelettes",                  4},
};

// Le nom saisi à la main, entre deux frames. Une seule zone de saisie à l'écran,
// donc un seul tampon.
char g_manual_wav[64] = "";

}  // namespace

SpriteSoundTweaks::SpriteSoundTweaks() {
  // On ne patchera QUE le site exact. Si les neuf octets ne sont pas ceux qu'on
  // connaît — autre build du client, ou un patch WARP posé au même endroit — on
  // renonce en silence plutôt que d'écrire au milieu d'autre chose.
  site_verified_ =
      std::memcmp(reinterpret_cast<const void*>(kFrameSoundRewind),
                  kRewindToZero, sizeof(kRewindToZero)) == 0;

  // Le détour du son est posé une fois pour toutes : sans nom dans la liste il
  // ne coûte rien, et le poser plus tard demanderait de le retirer aussi.
  g_play3d_orig = reinterpret_cast<Play3DFn>(
      hooking::HookManager::Instance().SetHook(
          hooking::HookType::kJmpHook,
          reinterpret_cast<uint8_t*>(kSoundPlay3D),
          reinterpret_cast<uint8_t*>(&Hooked_Play3D)));
}

SpriteSoundTweaks::~SpriteSoundTweaks() {
  hooking::HookManager::Instance().UnsetHook(
      reinterpret_cast<uint8_t*>(kSoundPlay3D));
  g_play3d_orig = nullptr;
  if (patch_applied_) SetFirstFrameSounds(false);
}

void SpriteSoundTweaks::OnTick() {
  if (patch_applied_ != first_frame_enabled_)
    SetFirstFrameSounds(first_frame_enabled_);
}

void SpriteSoundTweaks::SetFirstFrameSounds(bool on) {
  if (!site_verified_) return;
  const uint8_t* bytes = on ? kRewindToMinusOne : kRewindToZero;
  // L'échec laisse patch_applied_ inchangé : le prochain tick réessaiera, et le
  // panneau continuera d'afficher l'état RÉEL du client.
  if (!mem::WriteCode(kFrameSoundRewind, bytes, sizeof(kRewindToZero))) return;
  patch_applied_ = on;
}

bool SpriteSoundTweaks::IsMuted(const char* wav_name) const {
  if (muted_wavs_.empty() || wav_name == nullptr || wav_name[0] == '\0')
    return false;

  const std::string name = LowerAscii(wav_name);
  if (muted_wavs_.count(name) != 0) return true;

  // Le client demande aussi des sons par chemin (« effect\\ef_hit2.wav ») : le
  // joueur qui n'a tapé que le nom de fichier doit quand même être entendu.
  const size_t sep = name.find_last_of("\\/");
  return sep != std::string::npos &&
         muted_wavs_.count(name.substr(sep + 1)) != 0;
}

// ── Le panneau ───────────────────────────────────────────────────────────────
namespace {

// Une ligne « taire ce son ». Rend true quand la case a changé.
bool DrawMuteRow(const CandidateSound& sound, std::set<std::string>* muted) {
  bool is_muted = muted->count(sound.wav) != 0;
  bool changed = false;
  if (ro::RoCheckbox(sound.wav, &is_muted)) {
    if (is_muted) {
      muted->insert(sound.wav);
    } else {
      muted->erase(sound.wav);
    }
    changed = true;
  }
  ImGui::SameLine();
  // 🔴 `who` vient d'une table CONSTEXPR : la traduire à sa définition la figerait
  // avant que le catalogue soit chargé. Elle se traduit ICI, au point d'usage.
  // Les entrées purement anglaises (noms de monstres) n'ont pas de traduction au
  // catalogue et ressortent telles quelles ; seules celles qui portent du français
  // (« chair », « les squelettes »…) en ont une.
  ImGui::TextDisabled("%s — %d %s", i18n::Tr(sound.who), sound.sprite_count,
                      sound.sprite_count > 1 ? i18n::Tr("sprites")
                                             : i18n::Tr("sprite"));
  return changed;
}

bool IsCandidate(const std::string& wav) {
  for (const CandidateSound& sound : kDeathSounds)
    if (wav == sound.wav) return true;
  for (const CandidateSound& sound : kHitSounds)
    if (wav == sound.wav) return true;
  return false;
}

}  // namespace

bool SpriteSoundTweaks::DrawSettings() {
  bool changed = false;

  ImGui::TextWrapped(
      "%s", i18n::Tr(
                "Quand un monstre meurt, le client saute la première image de "
                "son animation — et avec elle le son qui y est posé. Quarante-"
                "sept monstres meurent ainsi en silence, dont tout le Bio Lab "
                "et le Doppelganger, qui partagent le même cri."));
  ImGui::Spacing();

  if (ro::RoCheckbox(i18n::Tr("Rétablir les sons manquants"),
                     &first_frame_enabled_)) {
    changed = true;
  }
  mui::Tooltip(
      i18n::Tr("Rend audibles les 47 sons de mort que le client porte sans "
               "jamais les jouer.\n"
               "Les sons de coup reçu et d'attaque, eux, s'entendent déjà dès "
               "le deuxième coup d'affilée : le réglage ne leur rattrape que le "
               "premier, ce qui ne s'entend pas.\n"
               "Prend effet aussitôt, sans relancer le jeu. Rien n'est envoyé "
               "au serveur : le réglage n'est que pour toi."));

  if (!site_verified_) {
    ImGui::TextDisabled(
        "%s", i18n::Tr("(le client n'a pas la forme attendue à cet endroit — "
                       "le correctif ne sera pas posé)"));
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  ImGui::TextWrapped(
      "%s",
      i18n::Tr("Un son de trop ? Coche-le pour le taire. Le silence vaut pour "
               "tout le jeu, que le rétablissement ci-dessus soit actif ou non."));
  ImGui::Spacing();

  // TrId et non Tr : ImGui hache le libelle pour identifier le repli, donc un
  // libelle traduit rouvrirait un AUTRE repli, referme par defaut.
  if (ImGui::TreeNodeEx(i18n::TrId("Sons de mort", "spritesound_death"),
                        ImGuiTreeNodeFlags_DefaultOpen)) {
    for (const CandidateSound& sound : kDeathSounds) {
      if (DrawMuteRow(sound, &muted_wavs_)) changed = true;
    }
    ImGui::TreePop();
  }

  if (ImGui::TreeNodeEx(i18n::TrId("Sons de coup reçu", "spritesound_hit"))) {
    ImGui::TextWrapped(
        "%s",
        i18n::Tr("Ceux-là, le jeu les joue déjà — dès le deuxième coup encaissé "
                 "par le même monstre, et donc en boucle pendant un combat. "
                 "Rien à rétablir ici : ils sont là pour être tus."));
    for (const CandidateSound& sound : kHitSounds) {
      if (DrawMuteRow(sound, &muted_wavs_)) changed = true;
    }
    ImGui::TreePop();
  }

  ImGui::Spacing();

  // ── Un nom à la main, pour tout ce qui n'est pas dans les deux listes ──────
  ImGui::PushItemWidth(200.0f);
  const bool submitted = ImGui::InputTextWithHint(
      "##spritesound_manual", i18n::Tr("nom du fichier .wav"), g_manual_wav,
      IM_ARRAYSIZE(g_manual_wav), ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopItemWidth();
  ImGui::SameLine();
  if ((ro::RoButton(i18n::Tr("Taire ce son")) || submitted) &&
      g_manual_wav[0] != '\0') {
    muted_wavs_.insert(LowerAscii(g_manual_wav));
    g_manual_wav[0] = '\0';
    changed = true;
  }
  mui::Tooltip(
      i18n::Tr("Le nom tel qu'il est dans les données du client, par exemple "
               "« doppleganger_die.wav ». Un chemin fonctionne aussi."));

  // Les noms tus qui ne sont dans aucune des deux listes : sans cette ligne, un
  // nom saisi à la main deviendrait invisible et impossible à retirer.
  std::vector<std::string> extra;
  for (const std::string& wav : muted_wavs_) {
    if (!IsCandidate(wav)) extra.push_back(wav);
  }
  if (!extra.empty()) {
    ImGui::Spacing();
    ImGui::TextDisabled("%s", i18n::Tr("Ajoutés à la main :"));
    for (const std::string& wav : extra) {
      ImGui::TextUnformatted(wav.c_str());
      ImGui::SameLine();
      ImGui::PushID(wav.c_str());
      if (ImGui::SmallButton(i18n::Tr("retirer"))) {
        muted_wavs_.erase(wav);
        changed = true;
      }
      ImGui::PopID();
    }
  }

  return changed;
}
