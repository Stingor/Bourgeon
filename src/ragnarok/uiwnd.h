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
#include <excpt.h>  // __try/__except (cf. la section « variantes gardées »)

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
// Nom TRANCHÉ au désassemblage (2026-07-31) : les deux camps du registre RE
// avaient chacun une moitié de raison. La fonction (IDB :
// UIWindowMgr_SaveRectAndCloseWindow) sauve d'abord position/taille dans la map
// des rects, PUIS appelle UIWindowMgr_QueueDestroyWindow (0x00a447d0) — la
// fenêtre est bien DÉTRUITE. « SaveWindowRect » décrivait le prologue en
// laissant croire à une opération inoffensive ; `CloseWindow` reste donc le bon
// nom côté Bourgeon.
inline void CloseWindow(int window_id) {
  using CloseWindowFn = void(__fastcall*)(void*, void*, int);
  reinterpret_cast<CloseWindowFn>(kCloseWindowAddr)(Mgr(), nullptr, window_id);
}

// ── Fenêtres publiées dans un global dédié ───────────────────────────────────
// Le client publie certaines fenêtres dans un global à elles, en plus de
// l'arbre du manager (cf. WndAtSlot pour la nuance slot/FindWindow). Chaque duo
// global + vtable était redéclaré à l'identique dans plusieurs fichiers :
//
//   inventaire : CINQ fichiers (storage_window, inventory_viewer,
//                character_sheet, cart_viewer, inventory_tweaks), sous les noms
//                kInvWndGlobal/kInvVTable ;
//   storage    : TROIS fichiers (storage_window, inventory_viewer, cart_viewer),
//                sous kStorageSlot/kStorageVTable.
//
// La vtable sert à valider ce que porte le slot ; non-nul + vtable conforme
// <=> fenêtre ouverte en ce moment.
constexpr uintptr_t kInventoryWndSlot   = 0x0131f6bc;  // inventaire, id 8
constexpr uintptr_t kInventoryWndVTable = 0x0103d460;
constexpr uintptr_t kStorageWndSlot     = 0x0131f770;  // UIItemStoreWnd, id 0x21 (slot = mgr+0x288)
constexpr uintptr_t kStorageWndVTable   = 0x0103ca40;
constexpr uintptr_t kCartWndVTable      = 0x0103d538;  // UIMerchantItemWnd, id 0x28
constexpr uintptr_t kChatWndSlot        = 0x0131f6b0;  // UINewChatWnd
constexpr uintptr_t kMailWriteWndSlot   = 0x0131f940;  // UIMailWriteWnd (rédaction RODEX)
constexpr uintptr_t kItemDescWndSlot    = 0x0131f700;  // mgr+0x218 : desc d'OBJET (classe 0xc)

// ⚠ Le slot de rédaction RODEX est un FILET, pas une poignée : notre fenêtre de
// courrier empêche la native de NAÎTRE (en prenant la place de son unique
// créateur, ZC 0x0A12) plutôt que de la détruire après coup — la détruire
// émettrait CZ_REQ_CANCEL_WRITE_MAIL et annulerait la rédaction. Le slot doit
// donc rester NUL ; le lire, c'est vérifier que le filet tient.

// ⚠ La vtable du chariot sert de SIGNATURE, pas de slot : trois fichiers la
// lisent pour reconnaître une fenêtre dont ils tiennent le pointeur sans en
// connaître l'identifiant. C'est exactement l'usage de `SafeVTableOf`.

// ── Deux méthodes natives que le projet appelait de partout ───────────
// `UIWindowMgr::SendMsg` est le point d'entrée général du gestionnaire. Son
// usage le plus courant chez nous est d'ÉCRIRE UNE LIGNE DANS LE CHAT — d'où
// le nom `kChatAddLine` que lui donnait la banque, et `kChatActionAddr` que lui
// donnait le chat. Deux noms tirés de l'usage, pour une méthode qui n'en est
// pas propriétaire.
constexpr uintptr_t kMgrSendMsgAddr = 0x00a4ad20;

// `UIWindow_BlitImageToNode` __thiscall(this, x, y, image, flag) : compose une
// image dans la passe de dessin de la fenêtre qui la porte. C'est par elle que
// passent les retouches de fenêtres natives (chat, inventaire, statut), et les
// trois la déclaraient chacune de leur côté (kBlit, kBlitImageToNode).
constexpr uintptr_t kBlitImageToNodeAddr = 0x00a1d260;

// ── Écrire et mesurer du texte dans une fenêtre native ──────────────────────
// Les deux faces d'un même geste, et le projet les séparait : les retouches
// d'inventaire et de statut déclaraient chacune `kDrawText`, l'inventaire seul
// `kMeasureW`, et le chat écrivait l'adresse de mesure EN LITTÉRAL au milieu
// d'une expression — donc invisible à tout relevé par nom.
constexpr uintptr_t kDrawTextAddr    = 0x00a25a70;  // __thiscall(this,x,y,s,len,face,size,color,gras,ital)
constexpr uintptr_t kDrawTextRightAddr = 0x00a27b50;  // idem, ALIGNÉ À DROITE sur x
constexpr uintptr_t kMeasureTextWidthAddr = 0x00a21c90;  // __thiscall(this,s,len,face,size,_,_) -> largeur

// ── `UIWnd_SetVisible` NATIVE, à distinguer de notre `SetVisible` ───────────
// __thiscall(this, visible). Deux fichiers la déclaraient, l'un sous
// `kSetVisibleFn`, l'autre sous `kHideNative`.
//
// 🔴 CE QU'ELLE FAIT DE PLUS QUE `uiwnd::SetVisible` (plus bas), au
// désassemblage (2026-08-24) — parce que les deux écrivent le MÊME champ et
// qu'on pourrait les croire interchangeables :
//
//     *(this + 0x28) = visible;              // identique à notre SetVisible
//     si visible == 0 :  un appel de plus (0x00a2e5c0)
//     sinon           :  OnMsg(msg 23) via vtable+0x94, puis vtable+0x98,
//                        puis 0x00a4ccf0
//
// Autrement dit : écrire le champ suffit à faire DISPARAÎTRE une fenêtre, mais
// la NATIVE prévient en plus la fenêtre de son changement d'état. C'est
// pourquoi la barre de raccourcis DÉTOURNE cette fonction au lieu de forcer le
// champ — ce qu'elle veut intercepter, c'est justement la notification par
// laquelle le client re-lie la barre et ses boutons.
//
// ⚠ Pas d'enveloppe ici, volontairement : ses deux appelants ne l'APPELLENT pas
// de la même façon — l'un pose un détour dessus, l'autre l'invoque. Une
// enveloppe ne servirait qu'au second, et son nom se heurterait à `SetVisible`.
constexpr uintptr_t kSetVisibleAddr = 0x009030c0;

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
constexpr int kOffWndId   = 0x2c;  // int : identifiant de fenêtre (celui de MakeWindow)
constexpr int kOffPosX    = 0x1c;  // int : x écran
constexpr int kOffPosY    = 0x20;  // int : y écran

// La position ÉCRAN courante d'une fenêtre, en un appel.
//
// 🔴 Ces deux offsets étaient RE-DÉCLARÉS sous `kWinX`/`kWinY` dans TROIS
// fichiers, alors qu'ils existent ici depuis longtemps — et le fichier qui
// s'appelle `window_pos_tweaks` écrivait en plus `+ 0x1c` / `+ 0x20` en dur, à
// trois cent soixante lignes de sa propre constante. C'est la forme que le
// relevé de doublons ne voit pas : une expression, pas une fonction.
//
// ⚠ Aucun SEH ici, comme les autres accesseurs de cet en-tête : les appelants
// lisent depuis leur propre `__try` (détour de handler de messages), et en
// ajouter un second interdirait à l'appelant d'y tenir des objets (C2712).
inline void LivePos(const void* wnd, int* x, int* y) {
  const uint8_t* b = static_cast<const uint8_t*>(wnd);
  if (x) *x = *reinterpret_cast<const int*>(b + kOffPosX);
  if (y) *y = *reinterpret_cast<const int*>(b + kOffPosY);
}

// ── Inventaire des fenêtres VIVANTES ─────────────────────────────────────────
// `FindWindow(id)` ne répond qu'à qui connaît DÉJÀ l'identifiant cherché. Elle ne
// sait donc pas répondre à « quelle fenêtre native est à l'écran en ce moment ? »
// — la question qu'on se pose quand un écran natif inattendu prend la main.
//
// Le manager tient une liste circulaire doublement chaînée dont la SENTINELLE est
// pointée par `mgr+0x17C` ; chaque nœud vaut {suivant, précédent, fenêtre}, la
// fenêtre étant à +8. Relevé dans `UIWindowMgr_DestroyAllWindows 0x00a482f0`, qui
// la parcourt exactement ainsi (`v5 = *v4`, fenêtre en `v5[2]`, jusqu'au retour à
// la sentinelle).
//
// Écrit les identifiants (`+0x2c`) dans `out_ids` et renvoie le nombre écrit.
// Borné par `max_ids` ET par un garde-fou de parcours : une liste corrompue ne
// doit pas faire tourner le client en rond.
constexpr int kOffWindowList = 0x17C;

inline int ListWindowIds(int* out_ids, int max_ids) {
  int n = 0;
  __try {
    void* sentinel = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(Mgr()) + kOffWindowList);
    if (!sentinel) return 0;
    void* node = *reinterpret_cast<void**>(sentinel);
    for (int guard = 0; node && node != sentinel && n < max_ids && guard < 256;
         ++guard) {
      void* wnd = *(reinterpret_cast<void**>(node) + 2);
      if (wnd)
        out_ids[n++] =
            *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(wnd) + kOffWndId);
      node = *reinterpret_cast<void**>(node);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return n;
  }
  return n;
}

// Visibilité. C'est le drapeau que les modules de features/windows/ mettent à 0
// pour cacher la native sans la détruire : elle continue de recevoir ses paquets
// et de tenir son modèle à jour, elle ne se voit et ne se clique simplement plus.
//
// ⚠ Accès en `int`, PAS en octet — CONFIRMÉ au désassemblage (2026-07-31) :
// UIWindow_SetVisible (0x005aad80) écrit `*((_DWORD*)this + 10)`, soit +0x28 en
// DWORD entier. Une écriture d'un seul octet laisserait +0x29..+0x2b porter un
// vestige non nul, que le natif relirait comme « visible ».
//
// ⚠ À ne pas confondre avec `kSetVisibleAddr` (0x009030c0) plus haut : elle
// écrit le même champ de la même façon, mais NOTIFIE en plus la fenêtre. Celle-ci
// est le geste nu, celle-là le geste complet du client.
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

// ── Variantes GARDÉES ────────────────────────────────────────────────────────
// Les fonctions ci-dessus touchent le natif sans filet : c'est voulu, elles sont
// aussi appelées depuis des chemins où une exception doit remonter. Mais les
// modules de features/windows/ tournent, eux, dans une frame de rendu — un
// manager pas encore construit ou une fenêtre détruite entre deux ticks ne doit
// pas tuer le client, seulement rendre « pas là ».
//
// Chacun de ces modules s'était donc écrit son propre `__try/__except` autour du
// même appel : `FindWnd` dans cinq fichiers, `CloseWnd` dans quatre, `HideWnd`
// dans cinq, `ReadValidWnd` dans quatre, `VTableOf` dans deux. Vingt copies du
// même geste. Les voici une fois.
//
// Ces fonctions restent volontairement PAUVRES en types (pointeurs nus, entiers)
// : le SEH de MSVC est interdit dans une fonction qui doit dérouler des objets
// C++, et c'est cette contrainte-là, pas l'esthétique, qui dicte leur signature.

// FindWindow, mais « pas là » plutôt qu'une exception.
//
// ⚠ Le test `window_id < 0` vient de trade_window, seul appelant à en avoir eu
// besoin : sa fenêtre d'échange a un id -1 tant qu'il n'est pas retrouvé dans la
// map du manager. Généralisé ici parce qu'un id négatif ne désigne jamais une
// fenêtre valide — le natif, lui, irait le chercher dans son arbre.
inline void* SafeFindWindow(int window_id) {
  if (window_id < 0) return nullptr;
  __try {
    return FindWindow(window_id);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// CloseWindow, sans propager. Comme la version nue, elle DÉTRUIT la fenêtre :
// aucun pointeur vers elle ne survit à l'appel.
inline void SafeCloseWindow(int window_id) {
  __try {
    CloseWindow(window_id);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// SetVisible, sans propager, et tolérante au pointeur nul — c'est le cas normal
// quand la fenêtre à masquer n'est pas ouverte.
inline void SafeSetVisible(void* wnd, bool visible) {
  __try {
    if (wnd) SetVisible(wnd, visible);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// La vtable d'une fenêtre, 0 si illisible. Sert à IDENTIFIER une fenêtre dont on
// tient le pointeur mais pas l'id.
inline uintptr_t SafeVTableOf(const void* wnd) {
  __try {
    return wnd ? *reinterpret_cast<const uintptr_t*>(wnd) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// La fenêtre publiée dans un GLOBAL dédié, validée par sa vtable.
//
// ⚠ C'est un chemin DIFFÉRENT de FindWindow, pas un raccourci : certaines
// fenêtres (inventaire, chariot, storage, sertissage…) sont publiées par le
// client dans un global à elles, et `slot_addr` est l'adresse de ce global —
// donc un pointeur VERS un pointeur. La vtable attendue est ce qui distingue
// « la fenêtre est ouverte » de « le global porte encore un vestige ».
//
// Renvoie nullptr si le global est vide ou si la vtable ne correspond pas.
inline uint8_t* WndAtSlot(uintptr_t slot_addr, uintptr_t expected_vtable) {
  __try {
    auto* wnd = *reinterpret_cast<uint8_t**>(slot_addr);
    if (!wnd) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(wnd) != expected_vtable) return nullptr;
    return wnd;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// La même chose, mais par IDENTIFIANT de fenêtre — la moitié qui manquait.
//
// 🔴 LA VTABLE N'EST PAS UNE PRÉCAUTION DÉCORATIVE. Un identifiant ne garantit
// pas la classe : un portage de client peut renuméroter ses fenêtres, et la
// même valeur désignerait alors autre chose. C'est exactement le genre d'erreur
// qui ne se voit pas — on lirait des champs à des décalages qui ont un sens dans
// une autre structure.
//
// Le client DÉTRUIT ses fenêtres à la fermeture : un retour non nul signifie donc
// « ouverte en ce moment », pas « déjà ouverte une fois ».
//
// Ce corps était recopié dans QUATRE fichiers — banque, chariot, raffinage,
// livre — à l'identique, chacun avec son propre `__try`. Un cinquième
// (`cashshop`) en portait une variante. Il vit ici, à côté de sa jumelle par
// slot, pour que les deux chemins se lisent au même endroit.
inline uint8_t* WndOfClass(int window_id, uintptr_t expected_vtable) {
  __try {
    auto* wnd = reinterpret_cast<uint8_t*>(FindWindow(window_id));
    if (!wnd) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(wnd) != expected_vtable) return nullptr;
    return wnd;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

}  // namespace uiwnd
