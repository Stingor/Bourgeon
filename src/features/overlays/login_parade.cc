#include "ragnarok/render.h"
#include "ui/game_texture.h"
#include "features/overlays/login_parade.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "imgui.h"

// Le chemin « vrai sprite Poring » passe par l'atlas D3D9 ; sous DX7 le handle GPU
// de la CTexture est à un autre offset. Défini dans le backend d3d9.
extern bool g_imgui_dx7_active;

namespace {

// ── Adresses / offsets natifs (client 20250716, base 0x400000) ────────────────
// Chaîne de chargement + rendu d'un sprite de MONSTRE, identique à celle validée
// dans roggle.cc (RE dédiée, renommée/commentée dans Ghidra). Toutes les
// fonctions de la chaîne sont __thiscall (émulées en __fastcall, EDX ignoré) et
// gardées SEH ; tout échec bascule sur le blob dessiné à la main.
constexpr uintptr_t kMonResName     = 0x00d824c0;  // Mob_ClassIdToResName(classId) -> resname
constexpr uintptr_t kFmtSpr         = 0x0103181c;  // "몬스터\\%s.spr" (CP949)
constexpr uintptr_t kFmtAct         = 0x0103182c;  // "몬스터\\%s.act" (CP949)
constexpr uintptr_t kActFrameCount  = 0x0070f6b0;  // __thiscall(act, action) -> int (#frames)
constexpr int       kCTexDX9Handle  = 0x12c;       // CTexture -> IDirect3DTexture9*
constexpr int       kCTexDX7Handle  = 0x128;       // CTexture -> handle GPU (chemin DX7)

// ⚠ Mob_ClassIdToResName est __stdcall (RET 4), PAS __cdecl : roggle le
// déclare __cdecl et ne « marche » que parce que l'épilogue restaure esp=ebp
// (déséquilibre de pile masqué). On le déclare correctement ici.
using MonResNameFn  = const char* (__stdcall*)(int);
using TexMgrGetFn   = void* (__cdecl*)();
using MakeKeyFn     = void* (__cdecl*)(const char*);
using LoadResFn     = void* (__fastcall*)(void*, void*, void*);               // (mgr, edx, key)
using ActGetFrameFn = void* (__fastcall*)(void*, void*, unsigned, unsigned);  // (act, edx, action, frame)
using ActFrameCntFn = int   (__fastcall*)(void*, void*, unsigned);            // (act, edx, action)
using AtlasFn       = void* (__fastcall*)(void*, void*, void*, int, int*);    // (atlas, edx, cell, pal, geom)

// ── Son de mob (comme les vrais mobs, en respectant la config effets sonores) ──
// Le .act porte un tableau de noms de wav @act+0x120 (stride 0x18) ; chaque frame
// référence un index à frame+0x30 (0xffffffff = aucun). Act_GetSoundName(act,idx)
// résout l'index -> char* du nom. Sound_Play3D joue le wav ET est gated par
// OptionInfo_GetValue(0xb) = l'option « effets sonores » du joueur -> config
// respectée automatiquement (cf. Actor_PlayFrameSounds 0x00c47200 qui fait pareil).
constexpr uintptr_t kSoundMgrPtr    = 0x01253d0c;  // *ptr = SoundMgr (déréf 1×)
constexpr uintptr_t kSoundMgrGet    = 0x005ff990;  // getter/lazy-create du SoundMgr (renseigne 0x01253d0c)
constexpr uintptr_t kActGetSndName  = 0x0070f420;  // Act_GetSoundName(act, idx) -> char* (SSO/heap)
// ⚠ Au LOGIN, Sound_Play3D (0x00600770) est INUTILISABLE : il est gated par
// OptionInfo_GetValue(0xb) (option effets sonores), or OptionInfo n'est chargé que
// dans CSession_ctor (entrée en JEU) -> l'option renvoie 0 au login -> muet.
// On passe donc par Sound_PlaySample2D, qui NE teste PAS OptionInfo. Le SoundMgr
// (ctor FUN_005ff990) a un volume master 100 + son pool de voix dès le boot.
constexpr uintptr_t kSoundPlay2D    = 0x00600430;  // Sound_PlaySample2D(this,handle,&x,&y,&z,min,max,vol)
constexpr uintptr_t kSoundModeVal   = 0x01602674;  // SOUNDMODE (registre, boot) : 0 = son coupé au setup
constexpr int       kWavHandleOff   = 0x110;       // objet wav chargé -> handle Miles (= res[0x44])
using PlaySample2DFn = unsigned (__fastcall*)(void*, void*, void*, float*, float*,
                                              float*, int, int, float);  // (this,edx,handle,&x,&y,&z,min,max,vol)
using SndMgrGetFn    = int (__cdecl*)();
using GetSndNameFn   = const char* (__fastcall*)(void*, void*, unsigned);  // (act, edx, idx)

// Repli : la plupart des mobs de la famille Poring n'ont AUCUN wav dans leur .act
// (tableau son vide), donc au clic il n'y aurait rien à jouer. On joue alors le son
// de COUP générique — comme quand on tape un vrai mob (le son qu'on entend en jeu).
// Nom validé chargeable (roggle joue déjà ce wav). Les mobs qui ONT un son
// dans leur .act (ex. certaines morts) utilisent le leur, pas ce repli.
constexpr const char* kPokeFallbackWav = "effect\\EF_hit2.wav";

// ── Famille Poring : uniquement les class ids que le résolveur natif renvoie sur
// un sprite DISTINCT (RE confirmée). Les variantes exotiques (ghostring, deviling,
// metaling, magmaring, pouring) retombent nativement sur « poring » — leur nom de
// dossier n'existe que dans les .lub externes, donc on ne les inclut pas (elles
// n'apporteraient qu'un Poring de plus). Résolution du nom = 100 % native, aucun
// nom de fichier coréen hardcodé.
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
struct MobSprite {
  void* spr = nullptr;   // CSprite* (cellules @+0x510, palette @+0x110)
  void* act = nullptr;   // CAction* (frames via Act_GetFrame)
  int   fails = 0;
  bool  gave_up = false;
  char  snd[96] = {0};   // wav « voix » du mob (1er nom non vide du tableau son du .act)
};
MobSprite g_sprites[kFamilyCount];

// Résout le wav principal du mob = 1er nom non vide du tableau son du .act
// (act+0x120 begin / +0x124 end, stride 0x18 std::string). Sert au son « au clic ».
void ResolvePrimarySound(MobSprite& s) {
  if (!s.act || s.snd[0]) return;
  __try {
    char* a = reinterpret_cast<char*>(s.act);
    const int begin = *reinterpret_cast<int*>(a + 0x120);
    const int end   = *reinterpret_cast<int*>(a + 0x124);
    const int count = (begin && end) ? (end - begin) / 0x18 : 0;
    for (int i = 0; i < count && i < 64; ++i) {
      const char* nm = reinterpret_cast<GetSndNameFn>(kActGetSndName)(
          s.act, nullptr, static_cast<unsigned>(i));
      // ⚠ Le tableau d'events du .act contient AUSSI des marqueurs d'animation
      // (ex. "atk" = frame d'attaque), PAS que des wav. Le natif ne joue un event
      // que s'il contient ".wav" (Actor_PlayFrameSounds : test memchr('.') + ".wav").
      // On applique le même filtre, sinon on tente de charger "atk" -> échec.
      if (nm && nm[0] && (std::strstr(nm, ".wav") || std::strstr(nm, ".WAV"))) {
        std::strncpy(s.snd, nm, sizeof(s.snd) - 1);
        break;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { s.snd[0] = '\0'; }
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
    void* mgr = *reinterpret_cast<void**>(kSoundMgrPtr);
    if (!mgr) {  // pas encore créé ? getter idempotent (lazy-create) puis relire
      reinterpret_cast<SndMgrGetFn>(kSoundMgrGet)();
      mgr = *reinterpret_cast<void**>(kSoundMgrPtr);
    }
    if (!mgr) return;
    void* tex = reinterpret_cast<TexMgrGetFn>(ro::texmgr::kGet)();
    void* res = reinterpret_cast<LoadResFn>(ro::texmgr::kLoad)(
        tex, nullptr, const_cast<char*>(name));  // wav chargé par NOM (comme Sound_Play3D)
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

// Son SYNCHRONISÉ à la frame (exactement comme Actor_PlayFrameSounds) : si la frame
// courante porte un event son (frame+0x30 != -1), joue le wav correspondant. Renvoie
// true si un son a été émis. La plupart des .act de poring n'ont d'event que sur les
// frames d'attaque/dégâts, donc l'idle reste souvent muet (fidèle aux vrais mobs).
bool MaybePlayFrameSound(void* act, unsigned action, unsigned frameIdx) {
  bool played = false;
  __try {
    void* frame = reinterpret_cast<ActGetFrameFn>(render::kActionGetFrameAddr)(
        act, nullptr, action, frameIdx);
    if (frame) {
      const unsigned sidx =
          *reinterpret_cast<unsigned*>(reinterpret_cast<char*>(frame) + 0x30);
      if (sidx != 0xffffffffu) {
        const char* nm = reinterpret_cast<GetSndNameFn>(kActGetSndName)(
            act, nullptr, sidx);
        // Idem : seuls les events ".wav" sont des sons (ex. "atk" = marqueur, pas un son).
        if (nm && nm[0] && (std::strstr(nm, ".wav") || std::strstr(nm, ".WAV"))) {
          PlayMobSound(nm);
          played = true;
        }
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { played = false; }
  return played;
}

void EnsureSprite(int idx) {
  MobSprite& s = g_sprites[idx];
  if (s.act || s.gave_up) return;
  bool ok = false;
  __try {
    const char* res = reinterpret_cast<MonResNameFn>(kMonResName)(kFamily[idx]);
    if (!res || !*res) res = "poring";
    char sprpath[160], actpath[160];
    std::snprintf(sprpath, sizeof(sprpath),
                  reinterpret_cast<const char*>(kFmtSpr), res);
    std::snprintf(actpath, sizeof(actpath),
                  reinterpret_cast<const char*>(kFmtAct), res);
    void* mgr = reinterpret_cast<TexMgrGetFn>(ro::texmgr::kGet)();
    void* sk  = reinterpret_cast<MakeKeyFn>(ro::texmgr::kMakeKey)(sprpath);
    void* ak  = reinterpret_cast<MakeKeyFn>(ro::texmgr::kMakeKey)(actpath);
    s.spr = reinterpret_cast<LoadResFn>(ro::texmgr::kLoad)(mgr, nullptr, sk);
    s.act = reinterpret_cast<LoadResFn>(ro::texmgr::kLoad)(mgr, nullptr, ak);
    ok = (s.spr && s.act);
    if (!ok) s.spr = s.act = nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    s.spr = s.act = nullptr;
  }
  if (ok) ResolvePrimarySound(s);  // wav « voix » (peut rester vide si le .act n'en a pas)
  if (!ok && ++s.fails >= 300) s.gave_up = true;  // ~5 s puis on abandonne CE membre
}

void DropSprites() {
  for (int i = 0; i < kFamilyCount; ++i) g_sprites[i] = MobSprite{};
}

// Résout la cellule « corps » de la frame demandée en texture native + UVs atlas.
// Ré-résolu chaque frame : le GetCached maintient la cellule vivante et rend le
// handle de page à jour (survit au device churn). cw/ch = taille pixel native de
// la cellule (pour le dimensionnement écran + le hit-test souris). false = repli.
bool ResolveQuad(int idx, unsigned action, unsigned frameIdx, void** out_tex,
                 ImVec2* uv0, ImVec2* uv1, float* cw, float* ch) {
  MobSprite& s = g_sprites[idx];
  if (!s.spr || !s.act) return false;
  const int handle_off = g_imgui_dx7_active ? kCTexDX7Handle : kCTexDX9Handle;
  bool ok = false;
  __try {
    void* frame = reinterpret_cast<ActGetFrameFn>(render::kActionGetFrameAddr)(
        s.act, nullptr, action, frameIdx);
    if (!frame) return false;
    char* fr = reinterpret_cast<char*>(frame);
    char* lbegin = *reinterpret_cast<char**>(fr + 0x20);  // vecteur ActLayer
    char* lend   = *reinterpret_cast<char**>(fr + 0x24);
    const int nlayers = static_cast<int>((lend - lbegin) / 0x24);
    if (nlayers <= 0 || nlayers > 64) return false;

    char* spr = reinterpret_cast<char*>(s.spr);
    void* atlas = reinterpret_cast<void*>(
        *reinterpret_cast<uintptr_t*>(render::kContextPtr) + 0xc0);

    // Plus grande cellule valide = le corps (on saute la petite ombre).
    void*  best = nullptr;
    ImVec2 b0, b1;
    float  bw = 0.0f, bh = 0.0f;
    long   best_area = -1;
    for (int i = 0; i < nlayers; ++i) {
      int* L = reinterpret_cast<int*>(lbegin + i * 0x24);
      const int sprNo = L[2], sprType = L[8];
      if (sprNo < 0 || sprType >= 2) continue;  // -1 = vide ; >=2 = RGBA (pas le corps)
      const int cellBase = *reinterpret_cast<int*>(spr + 0x510 + sprType * 0xc);
      const int cellEnd  = *reinterpret_cast<int*>(spr + 0x514 + sprType * 0xc);
      if (static_cast<unsigned>(sprNo) >=
          static_cast<unsigned>((cellEnd - cellBase) >> 2)) continue;
      short* cell = *reinterpret_cast<short**>(cellBase + sprNo * 4);
      if (!cell) continue;
      const int palette = static_cast<int>(reinterpret_cast<uintptr_t>(spr + 0x110));
      int geom[12] = {0};
      void* ctex = reinterpret_cast<AtlasFn>(render::kAtlasGetCachedAddr)(
          atlas, nullptr, cell, palette, geom);
      if (!ctex) ctex = reinterpret_cast<AtlasFn>(render::kAtlasBuildAddr)(
          atlas, nullptr, cell, palette, geom);
      if (!ctex) continue;
      const long area = static_cast<long>(cell[0]) * static_cast<long>(cell[1]);
      if (area > best_area) {
        best_area = area;
        best = *reinterpret_cast<void**>(
            reinterpret_cast<char*>(ctex) + handle_off);
        bw = static_cast<float>(cell[0]);
        bh = static_cast<float>(cell[1]);
        b0 = ImVec2(*reinterpret_cast<float*>(&geom[3]),
                    *reinterpret_cast<float*>(&geom[4]));
        b1 = ImVec2(*reinterpret_cast<float*>(&geom[5]),
                    *reinterpret_cast<float*>(&geom[6]));
      }
    }
    if (best) {
      *out_tex = best; *uv0 = b0; *uv1 = b1; *cw = bw; *ch = bh; ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return ok;
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
  int   last_snd_frame = -1;  // dernière frame pour laquelle on a testé un event son
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
constexpr float kIdleFrameMs  = 130.0f;   // ms par image de l'anim idle
constexpr float kBaseScale    = 1.6f;     // agrandissement de base des sprites
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
  p.snd_cd = 0.0f;
}

void InitParade(const ImVec2& disp) {
  if (g_rng == 0) g_rng = GetTickCount() | 1u;
  g_count = 4 + static_cast<int>(NextU32() % 3);  // 4..6
  if (g_count > kMaxPorings) g_count = kMaxPorings;
  for (int i = 0; i < g_count; ++i) SpawnPoring(g_porings[i], disp, /*anywhere=*/true);
  g_inited = true;
}

// Repli : un blob Poring dessiné à la main (si l'atlas/sprite échoue).
void DrawBlob(ImDrawList* dl, ImVec2 c, float r, float alpha) {
  const ImU32 body = IM_COL32(255, 130, 175, static_cast<int>(alpha * 255));
  const ImU32 eye  = IM_COL32(30, 30, 40, static_cast<int>(alpha * 255));
  dl->AddCircleFilled(c, r, body, 20);
  dl->AddCircleFilled(ImVec2(c.x - r * 0.32f, c.y - r * 0.12f), r * 0.13f, eye, 8);
  dl->AddCircleFilled(ImVec2(c.x + r * 0.32f, c.y - r * 0.12f), r * 0.13f, eye, 8);
}

}  // namespace

void LoginParade::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // Un changement de mode a pu réinitialiser le device / vider le cache ressource :
  // on lâche les pointeurs sprite (rechargés proprement au prochain login) et on
  // réarme la parade.
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

  // Charge paresseusement quelques membres par frame (évite un pic au 1er affichage).
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

    // ── Résolution de la cellule -> texture ─────────────────────────────────
    void*  tex = nullptr;
    ImVec2 uv0, uv1;
    float  cw = 0, ch = 0;
    bool   have = false;
    {  // ResolveQuad gère lui-même l'offset du handle GPU (DX7 vs DX9).
      MobSprite& s = g_sprites[p.fam];
      unsigned frameIdx = 0;
      if (s.act) {
        __try {
          const int nf = reinterpret_cast<ActFrameCntFn>(kActFrameCount)(s.act, nullptr, 0);
          if (nf > 1) {
            const float cyc = nf * (kIdleFrameMs / 1000.0f);
            frameIdx = static_cast<unsigned>(std::fmod(p.anim_t, cyc) / cyc * nf);
            if (static_cast<int>(frameIdx) >= nf) frameIdx = nf - 1;
          }
        } __except (EXCEPTION_EXECUTE_HANDLER) { frameIdx = 0; }
      }
      have = ResolveQuad(p.fam, 0, frameIdx, &tex, &uv0, &uv1, &cw, &ch);

      // Son SYNCHRONISÉ à la frame (comme les vrais mobs) : on ne teste qu'au
      // CHANGEMENT de frame, et on throttle. Souvent muet en idle (fidèle).
      if (s.act && static_cast<int>(frameIdx) != p.last_snd_frame) {
        p.last_snd_frame = static_cast<int>(frameIdx);
        if (p.snd_cd <= 0.0f && MaybePlayFrameSound(s.act, 0, frameIdx))
          p.snd_cd = kSndThrottle;
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

    // ── Hit-test souris (sursaut au clic) ───────────────────────────────────
    const float half_w = (have ? cw : 22.0f) * p.scale * kBaseScale * 0.5f;
    const float half_h = (have ? ch : 22.0f) * p.scale * kBaseScale * 0.5f;
    if (clicked && p.startle_t <= 0.0f &&
        mouse.x >= cx - half_w && mouse.x <= cx + half_w &&
        mouse.y >= cy - half_h && mouse.y <= cy + half_h) {
      p.startle_t = FrandRange(0.8f, 1.4f);
      p.walking = true;
      p.hop_vy = kStartleHop;                     // grand saut
      const float dir = (cx >= mouse.x) ? 1.0f : -1.0f;  // détale à l'opposé du clic
      p.vx = dir * kFleeSpeed;
      // Son au clic : la « voix » du mob si son .act en a une, sinon le son de coup
      // générique (le Poring n'a pas de wav propre -> sinon clic muet). Config respectée.
      const char* snd = g_sprites[p.fam].snd;
      PlayMobSound((snd && snd[0]) ? snd : kPokeFallbackWav);
      p.snd_cd = kSndThrottle;
    }

    // ── Dessin ──────────────────────────────────────────────────────────────
    const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(alpha * 255));
    if (have && tex) {
      const float w = cw * p.scale * kBaseScale;
      const float h = ch * p.scale * kBaseScale;
      const ImVec2 a(cx - w * 0.5f, cy - h * 0.5f);
      const ImVec2 b(cx + w * 0.5f, cy + h * 0.5f);
      // Miroir horizontal selon la direction de déplacement (échange des U).
      ImVec2 t0 = uv0, t1 = uv1;
      if (p.vx > 0.0f) { const float tmp = t0.x; t0.x = t1.x; t1.x = tmp; }
      dl->AddImage((ImTextureID)(uintptr_t)tex, a, b,
                   ImVec2(t0.x, t0.y), ImVec2(t1.x, t1.y), tint);
    } else {
      DrawBlob(dl, ImVec2(cx, cy), 14.0f * p.scale * (kBaseScale * 0.5f), alpha);
    }
  }
}
