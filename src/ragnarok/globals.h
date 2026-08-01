#pragma once

// ── Globales du client (20250716, base 0x400000) ─────────────────────────────
// Point de vérité UNIQUE pour les adresses d'état de jeu les plus copiées du
// projet. Chacune était redéclarée dans 8 à 13 fichiers, sous deux à trois
// orthographes, dans autant de namespaces anonymes — donc invisibles les unes
// aux autres, et introuvables d'un seul grep au moment de porter le client sur
// une autre version d'exe.
//
// En-tête volontairement MINUSCULE (rien que <cstdint>), sur le modèle de
// ragnarok/uiwnd.h. À NE PAS confondre avec ragnarok/session.h, qui est la
// classe proxy Session (yaml-cpp, item_info, talktype) : un tout autre sujet et
// un en-tête bien plus lourd.

#include <cstdint>
#include <excpt.h>  // __try/__except des accesseurs gardés (même choix que uiwnd.h)

namespace rag {

// ── Session ──────────────────────────────────────────────────────────────────
// ⚠ C'est l'OBJET lui-même, PAS un pointeur vers lui : les 13 fichiers qui
// l'utilisent font tous soit `kSessionAddr + offset` pour lire un champ, soit le
// passent en `this`. Aucun ne déréférence. (Même forme que g_UIWindowMgr.)
constexpr uintptr_t kSessionAddr = 0x015fa3c0;

inline void* Session() { return reinterpret_cast<void*>(kSessionAddr); }

// Champ de la session à l'offset donné, ex. rag::SessionField<int>(0x17d0).
template <typename T>
inline T SessionField(int byte_offset) {
  return *reinterpret_cast<T*>(kSessionAddr + byte_offset);
}

// ── Zeny du joueur ───────────────────────────────────────────────────────────
// ⚠ NE PAS confondre avec `ci::kZeny`, qui est un OFFSET dans une structure
// d'info de personnage. Ici c'est une adresse absolue.
constexpr uintptr_t kZenyAddr = 0x015fba90;

inline int Zeny() { return *reinterpret_cast<int*>(kZenyAddr); }

// ── Gestionnaire de modes, et mode actif ─────────────────────────────────────
// Le projet appelait cet objet de TROIS noms — kModeMgr, kModeArg et kDragMgr —
// sans que rien ne signale qu'il s'agit du même. `kDragMgr` est le plus
// trompeur : il laisse croire à un gestionnaire de drag séparé, alors que
// skill_bar l'a simplement nommé d'après ce qu'il lit dans l'objet rendu (la
// charge du drag en cours, à +0x308).
constexpr uintptr_t kModeMgrAddr = 0x01213338;

// L'emplacement du POINTEUR vers le mode de zone actif (0 si aucun) : à
// DÉRÉFÉRENCER, contrairement aux deux adresses ci-dessus. Neuf fichiers le
// lisent sous les noms kUICmdDisp / kDispatcherPtr — c'est le `this` du
// dispatcher CMode::SendMsg.
constexpr uintptr_t kActiveModePtr = 0x0121333c;

inline void* ActiveMode() { return *reinterpret_cast<void**>(kActiveModePtr); }

// ── L'état « une interaction NPC est en cours » du CLIENT ────────────────────
// CGameMode+0x24C, posé à 1 par les handlers de paquet qui ouvrent une interaction
// (ZC_SAY_DIALOG 0x00B4, ZC_MENU_LIST 0x00B7, ZC_SELECT_DEALTYPE 0x00C4…) et remis
// à 0 quand elle se termine. +0x2DC porte le GID du NPC concerné.
//
// 🔴 Pourquoi ces deux champs sont ICI et pas dans un plugin : depuis qu'on prend
// la place de ces handlers, c'est à NOUS de les écrire — sinon le client ne se sait
// plus en conversation, et des chemins natifs qu'on n'a pas inventoriés changent de
// comportement. Deux plugins le font déjà (dialogue NPC, boutique NPC) et un
// troisième suivra ; la recette n'a pas à être recopiée une fois de plus.
//
// ⚠ Appeler depuis le fil RÉSEAU est correct — c'est exactement là que les
// handlers natifs remplacés l'écrivaient.
constexpr int kGameModeNpcDialogFlag = 0x24c;
constexpr int kGameModeNpcGid        = 0x2dc;

// Pose le flag, et le GID s'il est fourni (0 = ne pas y toucher).
//
// 🔴 Ne passer un GID que si le handler natif remplacé en écrivait un. +0x2DC est
// l'identité de la CONVERSATION (le NPC dont le script tourne = sd->npc_id côté
// serveur), pas celle de la fenêtre qu'on ouvre : l'écraser fait partir les
// fermetures au mauvais NPC, le serveur les rejette et le joueur reste bloqué. Sur
// les six sites de RecvLoop_DispatchPackets qui posent +0x24C=1, seuls les quatre
// paquets de DIALOGUE écrivent aussi +0x2DC ; ZC_SELECT_DEALTYPE (boutique,
// 0x00CA0F02) n'y touche pas — il range son npcId dans la fenêtre chooser (+0xB4).
inline void SetNpcInteractionActive(uint32_t npc_gid) {
  __try {
    void* mode = ActiveMode();
    if (!mode) return;
    uint8_t* m = reinterpret_cast<uint8_t*>(mode);
    *reinterpret_cast<int*>(m + kGameModeNpcDialogFlag) = 1;
    if (npc_gid != 0)
      *reinterpret_cast<uint32_t*>(m + kGameModeNpcGid) = npc_gid;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Éteint le flag. À faire à toute fermeture : c'est lui qui « débloque » le client.
inline void ClearNpcInteractionActive() {
  __try {
    void* mode = ActiveMode();
    if (mode)
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(mode) +
                              kGameModeNpcDialogFlag) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Le flag est-il posé ? Sert à reconnaître une interaction NATIVE en cours au
// moment où l'on allume l'interface moderne.
inline bool NpcInteractionActive() {
  __try {
    void* mode = ActiveMode();
    if (!mode) return false;
    return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(mode) +
                                   kGameModeNpcDialogFlag) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// GID du NPC avec qui le CLIENT se croit en interaction, 0 si illisible. Le repli
// quand notre propre modèle est vide — cas du basculement d'interrupteur à chaud,
// où aucun paquet de la conversation n'est passé par nous.
inline uint32_t NpcInteractionGid() {
  __try {
    void* mode = ActiveMode();
    if (!mode) return 0;
    return *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(mode) +
                                        kGameModeNpcGid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Accesseur natif « objet actif du manager ». Dix fichiers l'appellent, sous les
// noms kGameModeGet, kGetMode et kGetDragObj, TOUJOURS sur kModeMgrAddr.
//
// Hypothèse « = *(mgr+4) » RÉFUTÉE au désassemblage (2026-07-31) : le corps est
// `return *(mgr+0x58) == 1 ? *(mgr+4) : 0`. L'appel et la lecture directe de
// kActiveModePtr ne sont donc PAS équivalents — le getter rend 0 tant que l'état
// du manager (+0x58) n'est pas 1 (transitions de mode : login, chargement de
// map…), là où la lecture directe rend le pointeur brut. NE PAS remplacer l'un
// par l'autre : un site qui lit directement pendant un changement de map verrait
// un mode que le getter considère indisponible.
constexpr uintptr_t kModeMgrGetActiveAddr = 0x00a75340;

// ── Stats du personnage, et le TOTAL que le serveur utilise ──────────────────
// Deux blocs de six entiers, pas de pas de 4, dans l'ordre STR AGI VIT INT DEX LUK.
// Déjà lus par la feuille de personnage (donc éprouvés en jeu) — c'est de là qu'ils
// viennent, et ils étaient sur le point d'être recopiés une troisième fois.
//
// 🔴 LE TOTAL EST BASE + BONUS, et c'est cette somme-là qu'il faut pour rejouer un
// calcul serveur : `status->dex` côté rAthena est la stat effective, équipement et
// cartes comprises. Le bloc « base » seul donnerait systématiquement trop bas.
//
// ⚠ NE PAS employer `dex_base_` / `luk_base_` du layout de session (+0x1674/+0x1678) :
// ce sont les bases, et ce fichier de layout porte déjà un offset marqué « CONFIRMED »
// qui a fait planter le client (cf. son entête). Ces deux globales-ci sont lues par du
// code vivant, ce qui est une garantie d'un autre ordre.
constexpr uintptr_t kStatBaseAddr  = 0x015fba24;
constexpr uintptr_t kStatBonusAddr = 0x015fba0c;

enum Stat { kStr = 0, kAgi, kVit, kInt, kDex, kLuk, kStatCount };

inline int StatBase(Stat s) {
  return *reinterpret_cast<int*>(kStatBaseAddr + static_cast<int>(s) * 4);
}
inline int StatBonus(Stat s) {
  return *reinterpret_cast<int*>(kStatBonusAddr + static_cast<int>(s) * 4);
}
// La stat EFFECTIVE — celle que la fenêtre Status affiche comme « base + bonus » et
// que le serveur nomme `status->dex`.
inline int StatTotal(Stat s) { return StatBase(s) + StatBonus(s); }

// ── Niveaux de base et de job ────────────────────────────────────────────────
// Recopiés sous les noms kJobLvl / kBaseLvl (feuille de personnage) et kJobLevel
// (refine, pour les chances de refine).
constexpr uintptr_t kBaseLevelAddr = 0x015fb9f0;
constexpr uintptr_t kJobLevelAddr  = 0x015fb9f8;

inline int BaseLevel() { return *reinterpret_cast<int*>(kBaseLevelAddr); }
inline int JobLevel()  { return *reinterpret_cast<int*>(kJobLevelAddr); }

// ── Code-page EFFECTIVE du client ────────────────────────────────────────────
// 949 (Corée), 1252 (Europe) ou 0 (= CP_ACP), posée au démarrage par
// FUN_00a72440 d'après g_ServiceType. C'est ELLE que le natif passe à
// MultiByteToWideChar avant de dessiner (UIText_GdiTextOut 0x00547600) : tout
// texte venant du client — noms d'objets, descriptions, noms de personnages,
// libellés — est encodé là-dedans, pas en UTF-8.
//
// ⚠ La LIRE plutôt que coder 949 en dur. Le client tourne aussi bien en
// servicetype européen, où la même constante en dur donnerait du mojibake sur
// tout ce qui porte un accent. La conversion elle-même est ro::LocalToUtf8
// (ui/ro_imgui.h), qui garde cette adresse pour source.
constexpr uintptr_t kClientCodePageAddr = 0x0159b818;

// ── Allocateur CRT du client ─────────────────────────────────────────────────
// L'operator new / operator delete du CRT STATIQUE de l'exe (désassemblés :
// 0x00dbbc4f = boucle malloc + _callnewh — ne rend JAMAIS nullptr, il boucle ou
// aborte ; 0x00dbbc7f = free). Tout bloc alloué PAR le client — le vecteur de
// décalages que BuildDisplayName remplit, par exemple — doit être rendu au MÊME
// allocateur, jamais au free() du DLL. Redéclarés dans cinq fichiers sous les
// noms kGameFree, kGameMalloc et kAlloc.
constexpr uintptr_t kGameOperatorNewAddr    = 0x00dbbc4f;  // __cdecl(size) -> void*, jamais nul
constexpr uintptr_t kGameOperatorDeleteAddr = 0x00dbbc7f;  // __cdecl(ptr)

// « Destructeur » d'une std::string MSVC du CLIENT — au sens _Tidy : libère le
// heap au-delà du SSO (via l'operator delete ci-dessus, branche alignée pour
// les capacités >= 0x1000) puis remet la string à l'état vide (size=0, cap=15,
// buf[0]=0). Était déclarée dans QUATRE fichiers sous TROIS noms (kStrFree ×2,
// kStrDtor, kStdStringDtor — dont une graphie 0x004F08F0 en majuscules,
// invisible à un grep sensible à la casse). `this` en ecx ; edx ignoré, les
// appelants passent __fastcall ou __thiscall indifféremment.
//
// Contradiction RE TRANCHÉE au désassemblage (2026-07-31) : 0x004e78c0, que
// basic_info documentait comme un second dtor, est un simple THUNK vers
// celle-ci. Une seule fonction, deux portes d'entrée — tout le monde passe
// désormais par l'adresse réelle.
constexpr uintptr_t kStdStringDtorAddr = 0x004f08f0;

}  // namespace rag
