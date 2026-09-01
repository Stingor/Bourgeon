#include "features/patches/pick_quad_tweaks.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "bourgeon.h"
#include "features/moonlight_ui/moonlight_ui.h"
#include "imgui.h"
#include "ui/ro_imgui.h"
#include "ragnarok/game_scene.h"  // gamescene::kPickQuadMinSizeAddr
#include "ui/ro_widgets.h"
#include "utils/i18n.h"
#include "utils/log_console.h"
#include "utils/memory_patch.h"  // mem::WriteCode

namespace {

// Le `call` à réécrire, dans CActorSprite_SubmitNameplateQuad, et sa cible.
constexpr uintptr_t kCallSite = 0x00c58d13;  // E8 rel32
constexpr uintptr_t kInsert   = 0x00a79610;  // NameplateQueue_Insert

// Cible du saut final, en mémoire : un `jmp dword ptr [...]` ne consomme aucun
// registre, là où passer par eax obligerait à le libérer.
void* g_real_insert = reinterpret_cast<void*>(kInsert);

bool g_installed = false;
bool g_shrink    = true;
// 25 % par défaut : le clone d'@disguise cesse de voler le clic tout en restant
// attrapable si on le vise vraiment. C'est un correctif livré à tout le monde,
// plus un réglage d'atelier — le staff garde le curseur pour l'ajuster.
int  g_percent   = 25;

// ── Les trois minimums de zone cliquable ─────────────────────────────────────
// Un par famille d'entités, chacun calculé une fois par le client (largeur/640
// × 40 ou × 34) puis plus jamais relu ailleurs que dans la construction du quad
// de sa famille. Ordre = MinAreaFamily.
constexpr uintptr_t kMinAreaGlobals[3] = {
    gamescene::kPickQuadMinSizeAddr,  // acteurs (joueurs, monstres, pets) — facteur 40
    0x015F81D0,                       // PNJ de carte, portails — facteur 34
    0x016025B8,                       // unités de compétence — facteur 34
};

int  g_min_defaults[3] = {0, 0, 0};
int  g_min_shift       = 0;
bool g_min_written     = false;

// Lecture/écriture sous SEH, dans des fonctions à part : MSVC refuse un __try
// dans une fonction qui a des objets C++ à dérouler (C2712).
bool ReadIntSafe(uintptr_t addr, int* out) {
  __try {
    *out = *reinterpret_cast<const int*>(addr);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool WriteIntSafe(uintptr_t addr, int value) {
  __try {
    *reinterpret_cast<int*>(addr) = value;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

std::atomic<unsigned int> g_hits{0};
bool g_first_hit_logged = false;

}  // namespace

// ── Le corps du détour, en C ─────────────────────────────────────────────────
// Appelé une fois par acteur cliquable et par frame : il ne fait qu'un test
// d'entier tant que le réglage est éteint, et rien d'autre.
//
// `extern "C"` pour que le nom soit résoluble depuis l'assembleur en ligne sans
// dépendre de la décoration C++.
extern "C" void __cdecl Bourgeon_AdjustPickQuad(float* quad) {
  if (!g_shrink || quad == nullptr) return;

  // [6] = le GID. Négatif ⇒ acteur fantôme, cf. l'en-tête.
  const int32_t gid = *reinterpret_cast<const int32_t*>(quad + 6);
  if (gid >= 0) return;

  g_hits.fetch_add(1, std::memory_order_relaxed);
  if (!g_first_hit_logged) {
    g_first_hit_logged = true;
    LogInfo("[PickQuad] zone cliquable d'un GID negatif ({}) retrecie a {} %",
            gid, g_percent);
  }

  const float k = static_cast<float>(g_percent) * 0.01f;
  const float cx = (quad[0] + quad[3]) * 0.5f;
  const float cy = (quad[1] + quad[4]) * 0.5f;

  // Plancher d'un demi-pixel de chaque côté : le quadtree reçoit toujours un
  // rectangle bien ordonné (x0 < x1), même à 0 %. Un rectangle dégénéré serait
  // un pari sur du code qu'on n'a pas relu.
  float hw = (quad[3] - quad[0]) * 0.5f * k;
  float hh = (quad[4] - quad[1]) * 0.5f * k;
  if (hw < 0.5f) hw = 0.5f;
  if (hh < 0.5f) hh = 0.5f;

  quad[0] = cx - hw;
  quad[3] = cx + hw;
  quad[1] = cy - hh;
  quad[4] = cy + hh;
}

namespace {

// Le détour proprement dit. Il rend la pile EXACTEMENT comme le `call`
// d'origine l'avait laissée — même adresse de retour, même argument empilé,
// même `ecx` — puis saute dans la vraie fonction, qui fera son `retn 4`.
//
// eax/ecx/edx sont volatils pour la convention C, donc sauvés autour de
// l'appel ; ebx/esi/edi/ebp sont préservés par la fonction C elle-même. Le
// quad est relu sur la PILE plutôt que dans `edi` : le registre est un choix du
// compilateur du client, l'emplacement de l'argument est un contrat.
__declspec(naked) void InsertDetour() {
  __asm {
    push eax
    push ecx
    push edx
    mov  eax, [esp + 16]  // esp: edx, +4 ecx, +8 eax, +12 retour, +16 le quad
    push eax
    call Bourgeon_AdjustPickQuad
    add  esp, 4
    pop  edx
    pop  ecx
    pop  eax
    jmp  dword ptr [g_real_insert]
  }
}

}  // namespace

PickQuadTweaks::PickQuadTweaks() {
  // Le site d'appel doit être CELUI qu'on a lu : un `E8` dont la cible est bien
  // NameplateQueue_Insert. Sur toute autre disposition, on ne patche rien —
  // écrire cinq octets au hasard dans le chemin de rendu tuerait le client.
  const uint8_t* site = reinterpret_cast<const uint8_t*>(kCallSite);
  const int32_t rel = *reinterpret_cast<const int32_t*>(kCallSite + 1);
  if (site[0] != 0xE8 || kCallSite + 5 + rel != kInsert) {
    LogDiag("[PickQuad] site d'appel inattendu en 0x{:08X} : detour NON pose",
            kCallSite);
    return;
  }

  uint8_t patch[5] = {0xE8, 0, 0, 0, 0};
  const int32_t new_rel = static_cast<int32_t>(
      reinterpret_cast<uintptr_t>(&InsertDetour) - (kCallSite + 5));
  std::memcpy(patch + 1, &new_rel, sizeof(new_rel));
  g_installed = mem::WriteCode(kCallSite, patch, sizeof(patch));
  if (!g_installed)
    LogDiag("[PickQuad] page non ouvrable en 0x{:08X} : detour NON pose",
            kCallSite);
}

void PickQuadTweaks::OnRenderUI() {
  // Même porte que le détour : g_installed veut dire « les octets du client
  // sont ceux qu'on a lus », donc ces adresses de globales sont les bonnes.
  if (!g_installed) return;

  for (int i = 0; i < 3; ++i) {
    // Relever le défaut d'abord — c'est ce qui rend le réglage réversible, et
    // c'est la valeur que le témoin staff affiche. Chaque globale naît à la
    // PREMIÈRE soumission d'un quad de sa famille : celle des PNJ reste à zéro
    // tant qu'aucun PNJ n'est à l'écran, d'où la nouvelle tentative par frame.
    if (g_min_defaults[i] <= 0) {
      int v = 0;
      if (!ReadIntSafe(kMinAreaGlobals[i], &v)) continue;
      // Garde-fou de vraisemblance : le calcul du client donne 40..~200 selon
      // la résolution. Une valeur énorme dirait qu'on ne lit pas ce qu'on croit.
      if (v <= 0 || v > 4096) continue;
      g_min_defaults[i] = v;
    }
    if (g_min_shift > 0) {
      int v = g_min_defaults[i] >> g_min_shift;
      if (v < 1) v = 1;
      WriteIntSafe(kMinAreaGlobals[i], v);
    } else if (g_min_written) {
      // Réglage revenu à « origine » : restaurer UNE fois puis ne plus toucher
      // — la leçon du dézoom (un tweak qui réécrit sa base à chaque frame,
      // option éteinte comprise, masque toute autre écriture, patch WARP
      // compris).
      WriteIntSafe(kMinAreaGlobals[i], g_min_defaults[i]);
    }
  }
  g_min_written = (g_min_shift > 0);
}

namespace pick_quad {

int& min_shift() { return g_min_shift; }

int MinAreaDefault(int family) {
  if (family < 0 || family > 2) return 0;
  return g_min_defaults[family];
}

int MinAreaCurrent(int family) {
  const int def = MinAreaDefault(family);
  if (def <= 0) return 0;
  const int v = def >> g_min_shift;
  return (v < 1) ? 1 : v;
}

bool& shrink_negative_gid() { return g_shrink; }
int&  negative_gid_percent() { return g_percent; }
unsigned int negative_gid_hits() {
  return g_hits.load(std::memory_order_relaxed);
}
bool installed() { return g_installed; }

void DrawSettings() {
  if (!g_installed) {
    ImGui::TextDisabled(
        "%s", i18n::Tr("Zones cliquables : détour non posé (client inattendu)."));
    return;
  }

  bool changed =
      ro::RoCheckbox(i18n::Tr("Rétrécir la zone cliquable des GID négatifs"),
                     &g_shrink);
  ImGui::SameLine();
  mui::HelpMarker(i18n::Tr(
      "Un GID négatif n'existe pas dans le protocole : c'est un acteur "
      "FANTÔME, créé par le serveur (le clone d'@disguise de rAthena en "
      "envoie un, avec -bl->id).\n\n"
      "Le serveur ne connaît pas ce GID et jette sans un mot toute action "
      "envoyée vers lui. Mais sa zone cliquable, elle, est bien là et peut "
      "gagner le clic devant la vraie cible — la compétence part alors dans "
      "le vide, et le joueur ne voit qu'une cadence effondrée.\n\n"
      "Cochée, sa zone est rétrécie autour de son centre : le fantôme cesse "
      "de voler le clic, sans rien changer pour les entités normales.\n\n"
      "⚠ Palliatif côté client. Le vrai correctif est de ne plus émettre ce "
      "clone côté serveur."));

  if (g_shrink) {
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    if (mui::WheelSliderInt(i18n::Tr("Taille restante (%)"), &g_percent, 0, 100))
      changed = true;
    ImGui::SameLine();
    mui::HelpMarker(i18n::Tr(
        "Ce qui reste de la zone du fantôme, en pourcentage de sa taille "
        "d'origine, rétréci autour de son centre.\n\n"
        "0 % laisse un pixel : le fantôme devient pratiquement impossible à "
        "viser, ce qui est le but. Monter cette valeur ne sert qu'à le "
        "récupérer volontairement — pour l'observer avec le contour des zones "
        "cliquables, par exemple."));
  }

  // La mesure, pas la conviction : si ce compteur reste à zéro, ce serveur
  // n'émet aucun acteur au GID négatif et le réglage ne fait rien du tout.
  const unsigned int hits = negative_gid_hits();
  if (hits == 0)
    ImGui::TextDisabled("%s", i18n::Tr("Aucun GID négatif rencontré."));
  else
    ImGui::TextDisabled(i18n::Tr("Zones rétrécies : %u"), hits);

  if (changed) {
    if (auto* ui = Bourgeon::Instance().moonlight_ui()) ui->SaveSettings();
  }
}

}  // namespace pick_quad
