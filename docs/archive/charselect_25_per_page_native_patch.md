# ARCHIVE — 25 slots/page par patch .text natif (ABANDONNÉ)

Approche testée puis **abandonnée** (2026-07-21) pour afficher 25 persos/page
(5×5) sur l'écran de sélection, en patchant les immédiats de layout du client
natif. **On refera en ImGui** (overlay dessiné par-dessus, découplé de la grille
native). Le RE complet reste dans [../charselect_re.md](../charselect_re.md).

## Pourquoi abandonné

Mur **physique** : la carte de slot est un bitmap de taille fixe **~197 px de
haut**, et `UIWindow_SetSize` la **coupe** (elle ne la met pas à l'échelle). Cinq
lignes de cartes pleines = ~985 px, mais il n'y a que ~780 px sous le haut de la
fenêtre (origine ~214). Donc, à position de fenêtre inchangée, **5 lignes ne
tiennent pas sans couper OU chevaucher** les cartes :
- pas/hauteur compressés (130-155) → **cartes coupées** en bas + noms/sprites à
  recaler (offsets 0xbd/0x9e codés pour 197 px) — testé, rendu jugé insuffisant.
- hauteur native 197 + pas réduit → **cartes qui se chevauchent** (~50 px).
- seul remède « propre » : remonter TOUTE la fenêtre en haut de l'écran (~180 px
  regagnés) → cartes quasi pleines, mais déplace cadre/panneau info/pager et
  désaligne l'art de fond. Trop de constantes couplées à patcher.

Conclusion : le layout natif est trop rigide/couplé. **Un overlay ImGui** qui
dessine sa propre grille (icônes/sprites + noms) par-dessus, avec sa propre
pagination, contourne tout ça (taille libre, pas de bitmap fixe, pas de coupe).

## Sites de patch identifiés (référence pour l'ImGui / si repris un jour)

`slots_per_page` a une source unique : ctor `MOV [EDI+0x128],15` @ `0x0079a103`
(imm32 @ `0x0079a109`). Tout en dérive (cf. charselect_re.md). Layout (client
20250716) :

| Site imm | Natif | Rôle |
|----------|-------|------|
| `0x0079a109` | 15 | slots_per_page (ctor) |
| `0x0079b3c3` | 0xC5 (197) | hauteur carte (`PUSH 0xc5`, SetSize) |
| `0x0079b3ee` | 0xC3 (195) | pas Y carte (BuildPage `IMUL …,0xc3`) |
| `0x0079c7f0` | 0xC3 | pas Y sprite (BuildPage) |
| `0x0079cfe5` | 0xC3 | pas Y label nom (BuildPage) |
| `0x0079e00f`/`0x0079e4ee`/`0x0079e83e` | 0xC3 | pas Y label (OnMsg reflow) |
| `0x0079c7fc` | 0x9E (158) | offset Y sprite (BuildPage `ADD ECX,0x9e`, imm@+2) |
| `0x0079d0e9` | 0xBD (189) | offset Y nom (BuildPage `ADD EAX,0xbd`, imm@+1) |
| `0x0079e017`/`0x0079e4f6`/`0x0079e846` | 0xBD | offset Y nom (OnMsg, imm@+1) |

Colonnes **codées en dur à 5** (`%5`,`/5`, pas X `0x9d`, nav clavier ±5) — non
touchées (5×5 les garde). Dernier jeu testé en live : pas/hauteur 155, offset
sprite 132, offset nom 150 — grille dense OK (2 pages pour 45 persos) mais cartes
coupées.

## Code retiré (src/ragnarok/char_select.h + bloc de ragnarok_client.cc)

```cpp
// char_select.h
namespace charselect {
void SetTwentyFivePerPage(bool enable);  // apply/revert patch .text
bool IsTwentyFivePerPage();
}  // namespace charselect

// ragnarok_client.cc (anon namespace) — helper de patch + apply
void PatchTextImm32(uintptr_t imm_addr, uint32_t value) {
  auto* const p = reinterpret_cast<void*>(imm_addr);
  DWORD old = 0;
  if (!VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old)) return;
  *reinterpret_cast<uint32_t*>(imm_addr) = value;
  VirtualProtect(p, 4, old, &old);
  FlushInstructionCache(GetCurrentProcess(), p, 4);
}
void ApplyCharSelectLayout(bool enable) {
  const uint32_t slots  = enable ? 25u  : 15u;
  const uint32_t height = enable ? 155u : 0xC5u;   // 197 natif
  const uint32_t pitch  = enable ? 155u : 0xC3u;   // 195 natif
  const uint32_t sprY   = enable ? 132u : 0x9Eu;   // 158 natif
  const uint32_t nameY  = enable ? 150u : 0xBDu;   // 189 natif
  PatchTextImm32(0x0079a109, slots);
  PatchTextImm32(0x0079b3c3, height);
  for (uintptr_t s : {0x0079b3eeu,0x0079c7f0u,0x0079cfe5u,
                      0x0079e00fu,0x0079e4eeu,0x0079e83eu}) PatchTextImm32(s, pitch);
  PatchTextImm32(0x0079c7fc, sprY);
  for (uintptr_t s : {0x0079d0e9u,0x0079e017u,0x0079e4f6u,0x0079e846u}) PatchTextImm32(s, nameY);
}
```

Persistance : flag `moonlight_ui.char_select_25_per_page` dans
`bourgeon_settings.yaml` + case à cocher (« Interface de jeu ») — **tout retiré**.
