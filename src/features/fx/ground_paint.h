#pragma once

// ── Sol uni (fond de capture) ─────────────────────────────────────────────────
// Repeint TOUT le terrain .gnd d'une couleur unie réglable, pour isoler un sprite
// ou un effet sur un fond neutre lors d'une capture d'écran.
//
// Historiquement logé dans le SPR Effect Lab (features/fx/spr_effect_lab.cc), dont
// il n'a jamais rien partagé : le lab capture des primitives d'effets EZ, alors que
// ce module encadre la passe TERRAIN du renderer DX9. Le lab étant aujourd'hui
// dormant (son panneau et son RenderFrame sont commentés dans moonlight_ui), l'outil
// était devenu injoignable — d'où sa sortie en module autonome, piloté depuis la
// section « Staff Tools ».
//
// La géométrie et le z-buffer du sol restent INTACTS : on ne change que l'étage de
// couleur du stage 0 autour du draw natif, donc l'occlusion par le terrain reste
// correcte. L'eau, le ciel et le brouillard ne sont pas touchés.
//
// DX9 uniquement : le chemin de rendu DX7 est une autre famille de fonctions (cf. le
// piège documenté dans le .cc).
namespace ground_paint {

// Installe les hooks de la passe terrain (idempotent, sûr à appeler chaque frame).
// À n'appeler que lorsque le réglage est actif : inutile de poser quatre JMP pour
// une fonctionnalité éteinte.
void EnsureInstalled();

// ── État persistable ──────────────────────────────────────────────────────────
// Exposés par référence pour que MoonlightUi::LoadSettings/SaveSettings les sérialise
// dans bourgeon_settings.yaml, selon le même pattern que les autres plugins.
// color() pointe sur 4 floats RGBA (ordre ImGui), à convertir en ARGB hex.
bool&  enabled();
float* color();

// ── Peinture DEMANDÉE PAR UN AUTRE MODULE ────────────────────────────────────
// Le « Sol uni » a deux commanditaires : la case des Staff Tools (un fond de
// capture) et GreyWorld (un sol lisible en jeu). Ils veulent la même chose du
// renderer, et deux modules ne peuvent pas détourner les mêmes quatre fonctions.
//
// D'où cette entrée : GreyWorld ne repeint pas, il DEMANDE. La case du staff
// reste maîtresse — quand les deux sont actifs, c'est sa couleur qui s'affiche,
// parce qu'un outil de capture d'écran ne doit pas voir sa teinte changer sous
// lui à cause d'un réglage de confort. `rgba` est copié, pas retenu.
void SetExternalPaint(bool on, const float rgba[4]);

// Contrôles ImGui (section « Staff Tools » de MoonlightUi).
bool DrawSettings();

}  // namespace ground_paint
