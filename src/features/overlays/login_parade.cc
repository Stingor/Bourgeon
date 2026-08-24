#include "features/overlays/login_parade.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>

#include "imgui.h"
#include "ragnarok/audio.h"
#include "ui/game_texture.h"
#include "ui/mob_sprite.h"

namespace {

// ── Sprites : plus une seule structure du client ──────────────────────────────
//
// Toute la chaîne native (résolution de nom -> TexMgr -> atlas de sprites, avec
// la disposition mémoire de CSprite/CAction devinée) a laissé place à
// ui/mob_sprite.h, qui ne garde de natif que la table `jobName.lub`. Ce qui
// disparaît au passage : l'offset du handle GPU selon DX7/DX9, le pas de calque
// 0x24, spr+0x510, Act_GetFrame, et l'heuristique « on ne dessine que le plus
// grand calque » — qui jetait silencieusement l'ombre et les calques d'appoint.

// ── Son de mob (comme les vrais mobs, en respectant la config effets sonores) ──
// Le .act porte une table de noms de wav ; ui/sprite_view.h la lit et la filtre
// (elle contient AUSSI des marqueurs d'animation, pas que des .wav). Il ne reste
// ici que la lecture du wav, qui n'a rien à voir avec le format sprite.
constexpr uintptr_t kSoundMgrGet = 0x005ff990;  // getter/lazy-create du SoundMgr
// ⚠ Au LOGIN, Sound_Play3D (0x00600770) est INUTILISABLE : il est gated par
// OptionInfo_GetValue(0xb) (option effets sonores), or OptionInfo n'est chargé que
// dans CSession_ctor (entrée en JEU) -> l'option renvoie 0 au login -> muet.
// On passe donc par Sound_PlaySample2D, qui NE teste PAS OptionInfo. Le SoundMgr
// (ctor FUN_005ff990) a un volume master 100 + son pool de voix dès le boot.
constexpr uintptr_t kSoundPlay2D  = 0x00600430;  // Sound_PlaySample2D(this,handle,&x,&y,&z,min,max,vol)
constexpr uintptr_t kSoundModeVal = 0x01602674;  // SOUNDMODE (registre, boot) : 0 = son coupé au setup
constexpr int       kWavHandleOff = 0x110;       // objet wav chargé -> handle Miles (= res[0x44])
using PlaySample2DFn = unsigned (__fastcall*)(void*, void*, void*, float*, float*,
                                              float*, int, int, float);  // (this,edx,handle,&x,&y,&z,min,max,vol)
using SndMgrGetFn = int (__cdecl*)();

// Repli : la plupart des mobs de la famille Poring n'ont AUCUN wav dans leur .act
// (table de sons vide), donc au clic il n'y aurait rien à jouer. On joue alors le son
// de COUP générique — comme quand on tape un vrai mob (le son qu'on entend en jeu).
// Nom validé chargeable (roggle joue déjà ce wav). Les mobs qui ONT un son
// dans leur .act (ex. certaines morts) utilisent le leur, pas ce repli.
constexpr const char* kPokeFallbackWav = "effect\\EF_hit2.wav";

// ── Famille Poring : uniquement les class ids que le résolveur natif renvoie sur
// un sprite DISTINCT (RE confirmée). Les variantes exotiques (ghostring, deviling,
// metaling, magmaring, pouring) n'ont d'entrée que dans les .lub externes ; sans
// entrée `jobName`, mob_sprite renvoie false et le membre garde son blob — plutôt
// qu'un Poring de plus déguisé en variante.
constexpr int kFamily[] = {
    1002,  // Poring
    1031,  // Poporing
    1113,  // Drops
    1090,  // Mastering
    1096,  // Angeling
    1242,  // Marin
};
constexpr int kFamilyCount = static_cast<int>(sizeof(kFamily) / sizeof(kFamily[0]));

// ── Cache de sprite par membre de la famille (chargé paresseusement) ──────────
struct FamilyEntry {
  ro::MobSpriteRes res;
  bool snd_done = false;  // « voix » déjà cherchée (elle peut légitimement manquer)
  char snd[96] = {0};     // copiée : le pointeur rendu appartient au cache de sprite_view
};
FamilyEntry g_family[kFamilyCount];

void EnsureSprite(int idx) {
  FamilyEntry& e = g_family[idx];
  // LoadMobSprite est idempotent ET mémorise son échec : pas de tempête de
  // rechargement quand un membre n'a pas d'entrée jobName.
  if (!ro::LoadMobSprite(kFamily[idx], &e.res)) return;
  if (e.snd_done) return;
  e.snd_done = true;
  if (const char* s = ro::SpriteMainSound(e.res.sprite))
    lstrcpynA(e.snd, s, static_cast<int>(sizeof(e.snd)));
}

void DropSprites() {
  for (int i = 0; i < kFamilyCount; ++i) g_family[i] = FamilyEntry{};
}

// ── Orientation ───────────────────────────────────────────────────────────────
// Le .act range ses actions en motion*8 + direction (0 = sud, 2 = ouest,
// 6 = est). On prend donc la POSE orientée plutôt que de retourner l'image.
//
// 🔴 Ce n'est pas un détail de style : l'ancien code ne dessinait qu'UN calque et
// pouvait le retourner en échangeant ses U. Maintenant qu'on compose tous les
// calques, un miroir par calque les retournerait chacun SUR PLACE sans échanger
// leurs positions — l'ombre partirait d'un côté et le corps de l'autre.
//
// Repli sur l'action 0 si la direction n'existe pas : c'est un test à
// l'exécution sur le fichier, pas une supposition sur son contenu.
unsigned IdleAction(const ro::MobSpriteRes& r, bool facing_east) {
  const unsigned dir = facing_east ? 6u : 2u;
  return ro::MobActionFrameCount(r, dir) > 0 ? dir : 0u;
}

// Joue un wav centré, volume plein, AU LOGIN. On charge l'échantillon comme le fait
// Sound_Play3D (UITextureMgr_Load -> handle Miles à res+0x110) puis on appelle
// directement Sound_PlaySample2D (qui, lui, n'est PAS gated par OptionInfo -> marche
// au login). On respecte le on/off GLOBAL du son (SOUNDMODE, chargé au boot). Le son
// suit l'activation de la parade (pas de réglage séparé). Tout SEH-gardé (POD only).
void PlayMobSound(const char* name) {
  if (!name || !name[0]) return;
  __try {
    if (*reinterpret_cast<int*>(kSoundModeVal) == 0) return;  // son coupé au setup (registre)
    void* mgr = *reinterpret_cast<void**>(audio::kSoundMgrPtr);
    if (!mgr) {  // pas encore créé ? getter idempotent (lazy-create) puis relire
      reinterpret_cast<SndMgrGetFn>(kSoundMgrGet)();
      mgr = *reinterpret_cast<void**>(audio::kSoundMgrPtr);
    }
    if (!mgr) return;
    // Le gestionnaire aiguille par EXTENSION : un .wav lui va aussi bien qu'un
    // .bmp. Variante « Raw » = sans résolution de skin, qui ne s'applique qu'aux
    // chemins sous la racine d'interface et serait ici un aller-retour inutile.
    void* res = ro::texmgr::LoadResourceRaw(name);  // wav chargé par NOM (comme Sound_Play3D)
    if (!res) return;
    void* handle = *reinterpret_cast<void**>(
        reinterpret_cast<char*>(res) + kWavHandleOff);
    if (!handle) return;
    float x = 0.0f, y = 0.0f, z = 0.0f;  // centré (dist 0 -> volume plein)
    reinterpret_cast<PlaySample2DFn>(kSoundPlay2D)(
        mgr, nullptr, handle, &x, &y, &z, 250, 40, 1.0f);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── PRNG local (xorshift32) — pas de rand() global, seed dérivée du tick ───────
uint32_t g_rng = 0;
inline uint32_t NextU32() {
  uint32_t x = g_rng ? g_rng : 0x1234567u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  g_rng = x;
  return x;
}
inline float Frand() { return (NextU32() >> 8) * (1.0f / 16777216.0f); }  // [0,1)
inline float FrandRange(float a, float b) { return a + (b - a) * Frand(); }

// ── Un Poring de la parade ─────────────────────────────────────────────────────
// Physique de sautillement : chaque Poring rebondit (hop_y au-dessus de sa ligne
// de balade base_y) façon Poring, avance en glissant, fait des pauses, se retourne
// aux bords. Le clic déclenche un sursaut (grand saut + fuite).
struct Poring {
  int   fam = 0;         // index dans kFamily
  float x = 0, base_y = 0;
  float vx = 0;          // px/s (signe = direction)
  float scale = 1.0f;
  bool  walking = true;
  float state_t = 0;     // temps restant dans l'état courant
  float anim_t = 0;      // horloge d'animation (frames idle)
  float hop_y = 0, hop_vy = 0;  // hauteur/vitesse du saut au-dessus de base_y
  float hop_cd = 0;      // délai avant le prochain rebond
  float startle_t = 0;   // >0 = sursauté (fuite)
  int   last_snd_frame = -1;  // dernière image pour laquelle on a testé un event son
  int   last_snd_action = -1;  // ... et dans quelle action (un demi-tour change d'action)
  float snd_cd = 0;      // throttle anti-spam entre deux sons
};

constexpr int   kMaxPorings   = 6;
Poring g_porings[kMaxPorings];
int    g_count = 0;
bool   g_inited = false;

// Réglages de comportement (à ajuster à l'œil).
constexpr float kWalkSpeedMin = 26.0f, kWalkSpeedMax = 55.0f;  // px/s
constexpr float kFleeSpeed    = 150.0f;
constexpr float kGravity      = 900.0f;   // px/s^2 pour la retombée du saut
constexpr float kHopImpulse   = 190.0f;   // vitesse initiale d'un rebond de balade
constexpr float kHopInterval  = 0.55f;    // s entre deux rebonds en marche
constexpr float kStartleHop   = 340.0f;   // saut de sursaut
constexpr float kIdleFrameMs  = 130.0f;   // ms par image, REPLI si le .act ne déclare rien
constexpr float kBoxPx        = 96.0f;    // côté de la boîte de rendu, avant scale individuel
constexpr float kSndThrottle  = 0.7f;     // s min entre deux sons d'un même Poring

// Zone du formulaire de login (fraction de l'écran, centrée) où on estompe les
// Porings pour ne pas couvrir les champs. Heuristique volontairement large et
// bornée : cosmétique, s'adapte à la résolution. (Le vrai rect natif de UILoginWnd
// demanderait un hook du gestionnaire de fenêtres — future amélioration.)
void FormZone(const ImVec2& disp, ImVec2* c, ImVec2* half) {
  *c = ImVec2(disp.x * 0.5f, disp.y * 0.52f);
  const float hw = disp.x * 0.22f, hh = disp.y * 0.30f;
  *half = ImVec2(hw < 150.0f ? 150.0f : (hw > 280.0f ? 280.0f : hw),
                 hh < 130.0f ? 130.0f : (hh > 260.0f ? 260.0f : hh));
}

void SpawnPoring(Poring& p, const ImVec2& disp, bool anywhere) {
  p.fam    = static_cast<int>(NextU32() % static_cast<uint32_t>(kFamilyCount));
  p.scale  = FrandRange(0.85f, 1.35f);
  p.base_y = FrandRange(disp.y * 0.16f, disp.y * 0.90f);
  p.vx     = FrandRange(kWalkSpeedMin, kWalkSpeedMax) * (Frand() < 0.5f ? -1.0f : 1.0f);
  // Apparaît soit hors écran (entrée par un bord), soit n'importe où (init).
  p.x = anywhere ? FrandRange(disp.x * 0.05f, disp.x * 0.95f)
                 : (p.vx > 0 ? -30.0f : disp.x + 30.0f);
  p.walking = true;
  p.state_t = FrandRange(2.5f, 6.0f);
  p.anim_t  = FrandRange(0.0f, 1.0f);
  p.hop_y = p.hop_vy = 0.0f;
  p.hop_cd = FrandRange(0.0f, kHopInterval);
  p.startle_t = 0.0f;
  p.last_snd_frame = -1;
  p.last_snd_action = -1;
  p.snd_cd = 0.0f;
}

void InitParade(const ImVec2& disp) {
  if (g_rng == 0) g_rng = GetTickCount() | 1u;
  g_count = 4 + static_cast<int>(NextU32() % 3);  // 4..6
  if (g_count > kMaxPorings) g_count = kMaxPorings;
  for (int i = 0; i < g_count; ++i) SpawnPoring(g_porings[i], disp, /*anywhere=*/true);
  g_inited = true;
}

// Repli : un blob Poring dessiné à la main (si le sprite échoue).
void DrawBlob(ImDrawList* dl, ImVec2 c, float r, float alpha) {
  const ImU32 body = IM_COL32(255, 130, 175, static_cast<int>(alpha * 255));
  const ImU32 eye  = IM_COL32(30, 30, 40, static_cast<int>(alpha * 255));
  dl->AddCircleFilled(c, r, body, 20);
  dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.12f), r * 0.13f, eye, 8);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.32f, c.y - r * 0.12f), r * 0.13f, eye, 8);
}

}  // namespace

void LoginParade::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // Réarme la parade. Les textures, elles, n'ont plus besoin d'être lâchées à la
  // main : sprite_view suit Overlay_DeviceEpoch() et se recharge tout seul.
  DropSprites();
  g_inited = false;
}

void LoginParade::OnRenderLoginUI() {
  if (!enabled_) return;  // (déjà garanti login/char-select par le dispatch)

  const ImVec2 disp = ImGui::GetIO().DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;  // garde minimize (cf. feedback)

  float dt = ImGui::GetIO().DeltaTime;
  if (dt <= 0.0f) dt = 1.0f / 60.0f;
  if (dt > 0.1f) dt = 0.1f;  // clamp (alt-tab / hitch) pour ne pas téléporter

  if (!g_inited) InitParade(disp);

  // Charge paresseusement les membres de la famille (idempotent, cache derrière).
  for (int i = 0; i < kFamilyCount; ++i) EnsureSprite(i);

  ImVec2 fz_c, fz_half;
  FormZone(disp, &fz_c, &fz_half);

  // Clic observé (non consommé) : le clic passe quand même à l'UI de login native.
  // On l'ignore si une fenêtre ImGui capte la souris (menu Moonlight ouvert, etc.).
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                       !ImGui::GetIO().WantCaptureMouse;
  const ImVec2 mouse = ImGui::GetIO().MousePos;

  // On dessine dans le background draw list : par-dessus le fond de login natif,
  // mais SOUS les fenêtres ImGui (menu Moonlight, etc.).
  ImDrawList* dl = ImGui::GetBackgroundDrawList();

  for (int i = 0; i < g_count; ++i) {
    Poring& p = g_porings[i];
    FamilyEntry& fam = g_family[p.fam];

    // ── Machine à états : marche <-> pause ──────────────────────────────────
    p.state_t -= dt;
    if (p.startle_t > 0.0f) {
      p.startle_t -= dt;
      if (p.startle_t <= 0.0f) { p.walking = true; p.state_t = FrandRange(2.5f, 6.0f);
                                 p.vx = (p.vx > 0 ? 1.0f : -1.0f) *
                                        FrandRange(kWalkSpeedMin, kWalkSpeedMax); }
    } else if (p.state_t <= 0.0f) {
      p.walking = !p.walking;
      p.state_t = p.walking ? FrandRange(3.0f, 7.0f) : FrandRange(1.0f, 3.0f);
      if (p.walking && Frand() < 0.4f) p.vx = -p.vx;  // parfois demi-tour au redémarrage
    }

    // ── Sautillement (physique verticale) ───────────────────────────────────
    p.hop_vy -= kGravity * dt;
    p.hop_y  += p.hop_vy * dt;
    if (p.hop_y <= 0.0f) {
      p.hop_y = 0.0f; p.hop_vy = 0.0f;
      if (p.walking) {
        p.hop_cd -= dt;
        if (p.hop_cd <= 0.0f) {         // rebond de balade au sol
          p.hop_vy = kHopImpulse * (0.85f + 0.3f * Frand());
          p.hop_cd = kHopInterval * (0.8f + 0.5f * Frand());
        }
      }
    }

    // ── Déplacement horizontal ──────────────────────────────────────────────
    if (p.walking || p.startle_t > 0.0f) p.x += p.vx * dt;

    // Rebond aux bords (reste à l'écran, façon flânerie).
    const float margin = 24.0f * p.scale;
    if (p.x < margin)            { p.x = margin;            if (p.vx < 0) p.vx = -p.vx; }
    if (p.x > disp.x - margin)   { p.x = disp.x - margin;   if (p.vx > 0) p.vx = -p.vx; }

    // ── Animation idle (fait « respirer » le sprite) ────────────────────────
    p.anim_t += dt;
    if (p.snd_cd > 0.0f) p.snd_cd -= dt;

    const bool     have   = (fam.res.sprite.res != nullptr);
    const unsigned action = have ? IdleAction(fam.res, p.vx > 0.0f) : 0u;

    // ── Son SYNCHRONISÉ à l'image ───────────────────────────────────────────
    // Même index que celui que DrawSprite va dessiner (sprite_view n'a qu'UN
    // calcul d'image), et on ne teste qu'au CHANGEMENT d'image. Souvent muet en
    // idle : la plupart des .act ne portent d'event que sur l'attaque et les
    // dégâts — c'est fidèle aux vrais mobs.
    if (have) {
      const unsigned f = ro::SpriteFrameIndex(fam.res.sprite, action, p.anim_t,
                                              kIdleFrameMs);
      if (static_cast<int>(f) != p.last_snd_frame ||
          static_cast<int>(action) != p.last_snd_action) {
        p.last_snd_frame  = static_cast<int>(f);
        p.last_snd_action = static_cast<int>(action);
        if (p.snd_cd <= 0.0f) {
          if (const char* nm = ro::SpriteFrameSound(fam.res.sprite, action, f)) {
            PlayMobSound(nm);
            p.snd_cd = kSndThrottle;
          }
        }
      }
    }

    // ── Fondu au-dessus du panneau de login ─────────────────────────────────
    const float cx = p.x;
    const float cy = p.base_y - p.hop_y;
    float alpha = 1.0f;
    {
      const float dx = std::fabs(cx - fz_c.x) / fz_half.x;
      const float dy = std::fabs(cy - fz_c.y) / fz_half.y;
      const float d  = dx > dy ? dx : dy;         // distance Chebyshev normalisée
      if (d < 1.0f) alpha = 0.22f + 0.78f * d;    // s'estompe vers le centre, jamais 0
    }

    // ── Boîte de rendu ──────────────────────────────────────────────────────
    // Le sprite est cadré DEDANS par sprite_view (ratio préservé, boîte de
    // l'action entière donc taille stable d'une image à l'autre). C'est aussi la
    // zone de clic.
    const float half = kBoxPx * p.scale * 0.5f;
    const ImVec2 a(cx - half, cy - half);
    const ImVec2 b(cx + half, cy + half);

    // ── Hit-test souris (sursaut au clic) ───────────────────────────────────
    if (clicked && p.startle_t <= 0.0f &&
        mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y) {
      p.startle_t = FrandRange(0.8f, 1.4f);
      p.walking = true;
      p.hop_vy = kStartleHop;                     // grand saut
      const float dir = (cx >= mouse.x) ? 1.0f : -1.0f;  // détale à l'opposé du clic
      p.vx = dir * kFleeSpeed;
      // Son au clic : la « voix » du mob si son .act en a une, sinon le son de coup
      // générique (le Poring n'a pas de wav propre -> sinon clic muet). Config respectée.
      PlayMobSound(fam.snd[0] ? fam.snd : kPokeFallbackWav);
      p.snd_cd = kSndThrottle;
    }

    // ── Dessin ──────────────────────────────────────────────────────────────
    // allow_upscale : les sprites de la famille Poring font quelques dizaines de
    // pixels, sans agrandissement ils seraient minuscules à l'écran.
    if (!have || !ro::DrawMobSprite(dl, fam.res, a, b, p.anim_t, action,
                                    kIdleFrameMs, /*allow_upscale=*/true, alpha))
      DrawBlob(dl, ImVec2(cx, cy), 14.0f * p.scale * 0.8f, alpha);
  }
}
