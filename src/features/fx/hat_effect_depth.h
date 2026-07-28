#pragma once

#include "features/plugin.h"

// ── HatEffect world-depth oracle (port du patch WARP AllHatEffectsWorldDepthOracle) ──
//
// Les costumes « hatEffectID » rendus en .str (screen-STR) sont dessinés par le
// client 20250716 avec une profondeur constante « toujours au-dessus » : ils
// traversent le décor au lieu d'être occultés. Ce plugin donne à chaque STR
// hat-effect une profondeur dérivée du monde (axe Y-up de Ragnarok) tout en
// conservant sa position écran, son échelle, son animation et son blending.
//
// Portage fidèle du patch statique WARP (Scripts/Patches/
// AllHatEffectsWorldDepthOracle.qjs, bytes validés en jeu) : au lieu de patcher
// l'exe, on reproduit EXACTEMENT le même code machine à l'exécution —
//   1. un « wrapper » (32 o) qui arme un flag autour des deux appels de rendu
//      HatEffect (0x00AFE41C, 0x00AFE52B → boucle STR 0x00AD8750) ;
//   2. un « depth bridge » (228 o) branché sur le chargement de profondeur
//      réciproque (0x00AD93D4) : flag=0 → profondeur native ; flag=1 →
//      profondeur monde reconstruite (inverse algébrique de la projection Y
//      native 0x005541B0) avec biais de contact borné, puis normalisée par
//      Depth_NormalizeToClip (0x00554040).
//
// Tout est protégé par vérification de signature d'octets : sur tout autre
// build — ou si l'exe est DÉJÀ patché par WARP — le plugin ne fait rien.
class HatEffectDepthTweaks : public Plugin {
 public:
  HatEffectDepthTweaks();

  const char* name() const override { return "HatEffectDepth"; }

 private:
  // Applique le patch de profondeur monde (wrapper + depth bridge). Appelé par
  // le ctor quand HATFX_DIAGNOSTIC == 0. En mode diagnostic il n'est pas appelé.
  void InstallDepthPatch();
};
