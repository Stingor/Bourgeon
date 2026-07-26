#pragma once

// ── Gestionnaire de fenêtres natif (g_UIWindowMgr) ───────────────────────────
// Point de vérité UNIQUE pour les deux adresses les plus copiées du projet :
// l'objet g_UIWindowMgr et sa méthode FindWindow. Elles étaient redéclarées à
// l'identique dans 18 fichiers de src/plugins, sous cinq orthographes
// (kUIWindowMgr, kUiWindowMgr, kFindWindow, kFindWindowFn, kFindWindowAddr) et
// parfois en littéral au milieu d'une expression, donc invisibles au grep. Au
// prochain portage de client il y avait 18 endroits à retrouver, sans qu'aucune
// liste n'existe.
//
// En-tête volontairement MINUSCULE : pas de yaml-cpp, pas de proxy.h, rien que
// <cstdint>. C'est ce qui permet de l'inclure dans les ~18 plugins concernés
// sans regonfler leur temps de compilation (cf. chantier 1).
//
// À ne pas confondre avec ragnarok/ui_window_mgr.h, qui est la classe de HOOK
// dérivée du manager natif — un autre sujet, et un en-tête bien plus lourd.

#include <cstdint>

namespace uiwnd {

// Adresses du client 20250716.
constexpr uintptr_t kUIWindowMgrAddr = 0x0131f4e8;  // g_UIWindowMgr (l'OBJET, pas un pointeur vers lui)
constexpr uintptr_t kFindWindowAddr  = 0x00a47b90;  // UIWindowMgr::FindWindow(id) __thiscall

// Identifiants utilisés hors de leur plugin propriétaire.
constexpr int kWorldMapWndId = 0x8c;  // UIWorldViewWnd (plein écran)

// L'objet manager lui-même. Pour les sites qui lisent un de ses slots dédiés
// (+0x408 = fenêtre d'options ESC, +0x1dc = BasicInfo, +0x508 = compteur
// « tout masquer »…) ou qui le passent en `this` à une autre méthode native.
inline void* Mgr() { return reinterpret_cast<void*>(kUIWindowMgrAddr); }

// La fenêtre d'identifiant `window_id`, ou nullptr si elle n'est pas ouverte.
// Le client DÉTRUIT ses fenêtres à la fermeture : un retour non-nul veut donc
// bien dire « ouverte en ce moment », pas « déjà ouverte une fois ».
inline void* FindWindow(int window_id) {
  using FindWindowFn = void*(__thiscall*)(void*, int);
  return reinterpret_cast<FindWindowFn>(kFindWindowAddr)(Mgr(), window_id);
}

// Vrai quand une UI plein écran (la carte du monde) remplace le HUD in-game :
// les overlays Bourgeon qui se superposent au HUD doivent alors s'effacer.
//
// Ce test était copié caractère pour caractère dans trois plugins, chacun dans
// son namespace anonyme — la duplication était même documentée en commentaire
// (« Mirrors MenuIconTweaks::HudReplaced ») plutôt que corrigée, parce qu'une
// fonction libre non qualifiée aurait rendu leurs appels ambigus. Qualifiée,
// elle ne pose plus ce problème.
inline bool IsHudReplaced() { return FindWindow(kWorldMapWndId) != nullptr; }

}  // namespace uiwnd
