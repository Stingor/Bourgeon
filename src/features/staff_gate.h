#pragma once

// ── Gate « staff » ───────────────────────────────────────────────────────────
// Extrait de features/moonlight_ui/moonlight_ui.h : plusieurs plugins n'incluaient ce header de
// 400 lignes que pour cette seule fonction (char_select.cc le dit dans son propre
// commentaire d'include).
//
// L'implémentation vit dans moonlight_ui.cc, auprès du niveau de groupe qu'elle
// teste : celui-ci est renseigné par la réception du setting id 26, que MoonlightUi
// est seul à traiter.

// Vrai si le compte est STAFF : niveau de groupe serveur >= 80 reçu au login
// (setting id 26, rempli depuis pc_get_group_level(sd) côté moonlight). Gate
// PUREMENT serveur : tant que le serveur n'envoie pas l'id 26, la fonctionnalité
// reste masquée. Sert à réserver au staff les fonctionnalités « ESP-like »
// (p.ex. EntityNames). Reste un gate de CONFIANCE côté client — pour une
// donnée déjà présente sur chaque client (les noms), aucun enforcement serveur
// n'est possible.
bool IsStaff();

// Vrai si le compte est ADMINISTRATEUR : niveau de groupe serveur >= 99, la même
// source que IsStaff() (setting id 26). Seuil distinct parce que les deux gates
// ne protègent pas la même chose : `IsStaff()` ouvre ce qui AFFICHE (noms des
// entités, inspecteur, SPR Lab), `IsAdmin()` ouvre ce qui MODIFIE l'état du
// serveur pour tous les joueurs connectés — décharger un NPC, recharger son
// fichier de script.
//
// ⚠ Comme IsStaff(), c'est un gate de CONFIANCE côté client : il décide de ce qui
// s'affiche, pas de ce qui est permis. Chaque action ainsi gatée doit avoir sa
// propre revalidation SERVEUR (le handler de CZ 0x0F25 refait le test).
bool IsAdmin();
