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
// (« Mirrors MenuIcons::HudReplaced ») plutôt que corrigée, parce qu'une
// fonction libre non qualifiée aurait rendu leurs appels ambigus. Qualifiée,
// elle ne pose plus ce problème.
inline bool IsHudReplaced() { return FindWindow(kWorldMapWndId) != nullptr; }

// ── Fabrique et fermeture ────────────────────────────────────────────────────
// kMakeWindowAddr était redéclarée dans 11 fichiers (dont une en 0x00A39340,
// majuscules — invisible à un grep sensible à la casse), kCloseWindowAddr dans 8.
constexpr uintptr_t kMakeWindowAddr  = 0x00a39340;  // UIWindowMgr::MakeWindow(id)
constexpr uintptr_t kCloseWindowAddr = 0x00a2e770;  // UIWindowMgr::Close(id)

// Crée la fenêtre `window_id` si elle n'existe pas, et la rend. IDEMPOTENT :
// appelée sur une fenêtre déjà ouverte, elle rend l'existante sans en créer une
// seconde. L'id part en TROISIÈME argument et à la taille d'un pointeur — c'est
// bien ce qu'attend le natif, pas une coquille.
inline void* MakeWindow(int window_id) {
  using MakeWindowFn = void*(__fastcall*)(void*, void*, void*);
  return reinterpret_cast<MakeWindowFn>(kMakeWindowAddr)(
      Mgr(), nullptr, reinterpret_cast<void*>(static_cast<uintptr_t>(window_id)));
}

// Ferme la fenêtre `window_id` en PERSISTANT sa position, exactement comme un
// clic sur son bouton X. ⚠ Le destructeur natif retire la fenêtre des slots
// dédiés du manager : ne jamais garder un pointeur au-delà.
//
// ⚠ NOM À TRANCHER : le registre RE du projet se contredit sur cette adresse.
// Six fichiers la documentent `UIWindowMgr::Close(mgr, edx, id)`, deux
// (rodex_window, trade_window) `UIWindowMgr::SaveWindowRect(mgr, id)` — et
// rodex précise « fermeture propre (persiste la position, comme le X) ». Les
// huit appellent la même chose avec les mêmes arguments et en attendent le même
// effet, donc rien n'est cassé ; c'est le NOM qui n'est pas établi. Le nommer
// d'après son effet observé en attendant un désassemblage : si elle s'avère
// n'être QUE SaveWindowRect, la fermeture viendrait d'ailleurs et ce helper
// serait à renommer, pas à corriger.
inline void CloseWindow(int window_id) {
  using CloseWindowFn = void(__fastcall*)(void*, void*, int);
  reinterpret_cast<CloseWindowFn>(kCloseWindowAddr)(Mgr(), nullptr, window_id);
}

// ── Méthodes virtuelles d'une UIWindow ───────────────────────────────────────
constexpr int kVfSetPos = 0x10;  // vtable+0x10 : SetPos(x, y)
constexpr int kVfOnMsg  = 0x94;  // vtable+0x94 : OnMsg(...)

// Récupère la méthode virtuelle à l'offset `byte_offset` (en OCTETS) de `self`.
// Ce petit patron était recopié à l'identique dans onze fichiers.
template <typename Fn>
inline Fn Vf(void* self, int byte_offset) {
  return reinterpret_cast<Fn>(
      (*reinterpret_cast<uintptr_t**>(self))[byte_offset / 4]);
}

// Envoie un message à la fenêtre. Le natif prend SIX entiers ; `msg` est le
// DEUXIÈME. Le premier vaut 0 sur les 18 sites d'appel du projet et son rôle
// n'est pas établi — il est donc exposé en dernier sous le nom `arg0` plutôt
// que masqué : le jour où l'un des ~63 OnMsg en demande un autre, il est là.
// Les paramètres qui transportent un POINTEUR (p2 = &ItemSkillInfo pour la
// fenêtre 0xc, par exemple) passent par un int : x86, donc même largeur.
inline int OnMsg(void* wnd, int msg, int p2 = 0, int p3 = 0, int p4 = 0,
                 int p5 = 0, int arg0 = 0) {
  using OnMsgFn = int(__fastcall*)(void*, void*, int, int, int, int, int, int);
  return Vf<OnMsgFn>(wnd, kVfOnMsg)(wnd, nullptr, arg0, msg, p2, p3, p4, p5);
}

// Déplace la fenêtre en coordonnées écran.
inline void SetPos(void* wnd, int x, int y) {
  using SetPosFn = void(__fastcall*)(void*, void*, int, int);
  Vf<SetPosFn>(wnd, kVfSetPos)(wnd, nullptr, x, y);
}

// ── Champs d'instance d'une UIWindow ─────────────────────────────────────────
// ⚠ NE PAS confondre kOffPosX/Y avec les offsets homonymes d'un ACTEUR
// (player_jump.cc : +0x10/+0x14, position MONDE en float). Même nom, structure
// différente : c'est précisément pour ça qu'ils sont qualifiés `uiwnd::` ici.
constexpr int kOffVisible = 0x28;  // int : 0 = hors rendu ET hors hit-test
constexpr int kOffPosX    = 0x1c;  // int : x écran
constexpr int kOffPosY    = 0x20;  // int : y écran

// Visibilité. C'est le drapeau que les modules de features/windows/ mettent à 0
// pour cacher la native sans la détruire : elle continue de recevoir ses paquets
// et de tenir son modèle à jour, elle ne se voit et ne se clique simplement plus.
//
// ⚠ Accès en `int`, PAS en octet. Ce n'est pas un choix esthétique : les 28
// sites du projet déréfèrent tous `*reinterpret_cast<int*>(wnd + 0x28)`, et
// c'est cette forme-là qui est éprouvée en jeu. Une écriture d'un seul octet
// laisserait +0x29..+0x2b intacts — comportement différent si le champ fait
// bien 4 octets, ce que le désassemblage de Show/Hide (0x005aad80) reste à
// confirmer. Tant que ce n'est pas tranché, on copie ce qui marche.
inline bool IsVisible(const void* wnd) {
  return *reinterpret_cast<const int*>(
             reinterpret_cast<const uint8_t*>(wnd) + kOffVisible) != 0;
}
inline void SetVisible(void* wnd, bool visible) {
  *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(wnd) + kOffVisible) =
      visible ? 1 : 0;
}

inline int PosX(const void* wnd) {
  return *reinterpret_cast<const int*>(
      reinterpret_cast<const uint8_t*>(wnd) + kOffPosX);
}
inline int PosY(const void* wnd) {
  return *reinterpret_cast<const int*>(
      reinterpret_cast<const uint8_t*>(wnd) + kOffPosY);
}

}  // namespace uiwnd
