#pragma once

// ── La caméra du monde 3D (client 20250716) ──────────────────────────────────
// RO est un vrai moteur 3D : terrain .gnd/.rsw et sprites billboardés. Sa caméra
// n'est « isométrique » que par convention — c'est un rig sphérique tout à fait
// ordinaire, verrouillé sur un tilt et une distance. La reparamétrer suffit donc
// à obtenir n'importe quel cadrage, sans rien re-rendre.
//
// ── 🔴 LE RIG A DEUX ÉTATS, ET UN SEUL EST UN LEVIER ─────────────────────────
// L'objet caméra tient une pose COURANTE qui LERPE chaque frame vers une pose
// CIBLE (lissage `FUN_00a7ab90`, builder `FUN_00a7ae20` = vtable[4]) :
//
//     COURANT  +0x2c pitch   +0x30 yaw   +0x34 distance   ← écrire ici est VAIN
//     CIBLE    +0x44 pitch   +0x48 yaw   +0x4c distance   ← les vrais leviers
//
// Écrire dans la pose courante ne tient pas une frame : le lissage la ramène
// vers la cible. Ce sont les CIBLES qui persistent — mesuré en live (x32dbg) le
// 2026-07-01, après une première série d'offsets qui, eux, étaient faux.
// Corollaire agréable : tout mouvement écrit dans les cibles est lissé PAR LE
// MOTEUR, donc fluide sans qu'on ait à interpoler quoi que ce soit.
//
// ── Pourquoi ce module existe ────────────────────────────────────────────────
// L'objet caméra n'est pas à une adresse fixe : c'est `*(CGameMode+0xd0)`, et le
// seul moyen sûr de l'atteindre est de le cueillir dans un hook. Ce hook tient
// en 5 octets sur `Camera_ApplyViewDistanceClamp` — et 5 octets ne se posent
// qu'UNE fois. FpsView les avait pris le premier ; le second module à vouloir la
// caméra (AfkScreen) aurait écrasé son détour. D'où cette capture unique et
// partagée, dont les deux se servent.
//
// ⚠ `Camera_ApplyViewDistanceClamp` NE TOURNE PAS à chaque frame — seulement sur
// les chemins où le joueur agit sur la caméra. En veille elle ne tourne plus du
// tout : d'une part `Get()` rend alors la DERNIÈRE caméra vue (ce qui est bien
// la bonne, elle ne change pas), d'autre part plus personne ne réécrit les
// cibles — c'est ce qui rend un mouvement de caméra en veille aussi simple.

#include <cstdint>

namespace ro::camera {

// ── Offsets de la pose ───────────────────────────────────────────────────────
// Angles en DEGRÉS (tilt de repos = 45.0, yaw de repos = -50.0), distance dans
// l'unité de la vue (repos ≈ 278, plafond outdoor 400 en vanilla).
constexpr int kCurPitch  = 0x2c;
constexpr int kCurYaw    = 0x30;
constexpr int kCurDist   = 0x34;
constexpr int kTgtPitch  = 0x44;
constexpr int kTgtYaw    = 0x48;
constexpr int kTgtDist   = 0x4c;

// Pose l'unique hook de capture. Idempotent, et à appeler INCONDITIONNELLEMENT :
// le timestamp du client n'est pas encore connu au chargement des modules (il
// est posé plus tard, dans RagnarokClient::Initialize), si bien qu'un install
// gaté sur lui ne se ferait jamais — la leçon coûteuse du bug F9 de FpsView.
void Install();

// La dernière caméra vue, ou nullptr tant qu'on n'est pas entré dans le monde —
// et aussi, désormais, dès que ce qu'elle désigne n'est plus une caméra : la
// vtable est revérifiée à chaque appel, parce qu'un pointeur mis en cache ne
// survit pas à la destruction de son CGameMode.
void* Get();

// ── Lire / écrire la pose ────────────────────────────────────────────────────
// Sans caméra, la lecture rend `fallback` et l'écriture ne fait rien : aucun
// appelant n'a à tester quoi que ce soit avant d'appeler.
float Read(int offset, float fallback = 0.0f);
void  Write(int offset, float value);

}  // namespace ro::camera
