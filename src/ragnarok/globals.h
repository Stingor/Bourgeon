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
#include <cstdio>   // _snprintf_s (MapDisplayName recolle l'extension .rsw)
#include <cstring>  // strncpy_s (idem : on recopie la chaîne du client)
#include <excpt.h>  // __try/__except des accesseurs gardés (même choix que uiwnd.h)

namespace rag {

// ── Session ──────────────────────────────────────────────────────────────────
// ⚠ C'est l'OBJET lui-même, PAS un pointeur vers lui : les 13 fichiers qui
// l'utilisent font tous soit `kSessionAddr + offset` pour lire un champ, soit le
// passent en `this`. Aucun ne déréférence. (Même forme que g_UIWindowMgr.)
//
// 🔴 UN SEUL OBJET, CINQ MÉTIERS APPARENTS. Il était redéclaré sous les noms
// `kUIWindowContextKey` (chat, macros d'émotion), `kOptionContextAddr` (réglages
// du jeu), `kUiCtx` (homoncule), `kJobNameCtx` (sprites de monstre) et
// `kSessionAddr` — cinq façons de nommer la MÊME adresse, selon la méthode
// native qu'on venait lui demander. Chacune laissait croire à un objet distinct,
// et personne ne pouvait voir que les cinq allaient bouger ensemble au portage.
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

// L'appel typé. Il était recopié dans NEUF fichiers sous quatre noms de type
// (GetActiveFn, GetMode_t, GameModeGetActive_t, et une variante à deux
// paramètres dans damage_name_fix), chacun redonnant sa propre convention
// d'appel à la même adresse — la classe d'erreur qui ne se voit qu'en jeu.
//
// 🔴 CE N'EST PAS `ActiveMode()`. Le nom porte la différence, qui est réelle :
// celui-ci est GATÉ (rend 0 tant que le manager n'est pas en état 1), l'autre
// lit le pointeur brut. Pendant un changement de map, les deux ne disent pas la
// même chose — voir le bloc ci-dessus.
inline void* ActiveModeIfReady() {
  return reinterpret_cast<void* (__fastcall*)(int)>(kModeMgrGetActiveAddr)(
      static_cast<int>(kModeMgrAddr));
}

// Le même, sous SEH. Ces trois lignes étaient recopiées dans HUIT fichiers sous
// trois noms concurrents — ActiveGameMode(), Dispatcher(), GetGameMode() — et un
// neuvième portait le même nom en appelant `ActiveMode()`, l'autre porte : à
// lecture rapide rien ne distinguait les neuf.
inline void* ActiveModeSafe() {
  __try {
    return ActiveModeIfReady();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Un `int` à une adresse ABSOLUE, sous SEH. Six fichiers portaient ces trois
// lignes, sous quatre noms — ReadInt, ReadIntSEH, ReadCount, ReadOptSEH.
inline int ReadInt(uintptr_t addr) {
  __try {
    return *reinterpret_cast<const int*>(addr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Un champ à un offset d'octets, SANS protection : réservé aux lectures déjà
// prises dans le `__try` de l'appelant. Sept fichiers en portaient leur propre
// copie, tous sous le nom `Read` — d'où le `using rag::Read;` qui les remplace,
// et qui laisse les deux cents points d'appel intacts.
template <typename T>
inline T Read(const void* base, int off) {
  return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + off);
}

// ── CMode::SendMsg — le dispatcher de commandes du mode de zone ──────────────
// C'est par lui que passent presque toutes nos actions rejouées : utiliser une
// compétence, retirer du chariot, monter une stat, basculer la musique…
//
// LE SLOT ÉTAIT ÉCRIT DE QUATRE FAÇONS dans SEIZE fichiers — `kVfDispCmd`,
// `kSendMsgVtOff`, `kVfModeSendMsg` (tous `0x18`), `kVtblSendMsgIndex` (`6`), et
// deux fichiers l'écrivaient en littéral nu (`vt[6]`, `Vf<>(disp, 0x18)`). Rien
// ne reliait ces cinq écritures entre elles : chercher « 0x18 » dans le projet
// ramène surtout des offsets de champ sans rapport, et chercher « 6 » ne veut
// rien dire. Le slot n'a donc qu'un seul nom désormais.
constexpr int kVfModeSendMsg = 0x18;  // vtable +0x18, soit l'index 6

// ⚠ Le retour est déclaré `int` alors que le natif ne rend rien d'utile sur la
// plupart des commandes : sur x86, un appelant qui ignore EAX est correct dans
// les deux sens. C'est ce qui permet d'avoir UNE signature là où le projet en
// portait trois (`void`, `void*`, `int`) pour la même méthode.
//
// Les paramètres qui transportent un POINTEUR passent par un int — x86, donc
// même largeur (c'est le cas du titre de salon dans chat_room_window).
// ── Les COMMANDES de ce dispatcher ───────────────────────────────────────────
// 🔴 Le dispatcher avait son foyer depuis longtemps ; son VOCABULAIRE, non.
// `kCmdUseSkill` vivait dans TROIS fichiers, les deux autres dans deux chacun.
constexpr int kCmdUseSkill      = 0x45;  // { skillId, cibleGID, niveau }
constexpr int kCmdUseSkillSlot  = 0x71;  // lancer, routé par l'INF de la compétence
constexpr int kCmdCartToBody    = 0x4d;  // chariot -> inventaire
constexpr int kCmdCartToStorage = 0x4f;  // chariot -> entrepôt (entrepôt ouvert)

inline int ModeSendMsg(void* mode, int cmd, int p2 = 0, int p3 = 0, int p4 = 0,
                       int p5 = 0) {
  if (!mode) return 0;
  using SendMsgFn = int(__thiscall*)(void*, int, int, int, int, int);
  const uintptr_t* vt = *reinterpret_cast<uintptr_t**>(mode);
  return reinterpret_cast<SendMsgFn>(vt[kVfModeSendMsg / 4])(mode, cmd, p2, p3,
                                                             p4, p5);
}

// Une commande au mode de zone courant, sous SEH. Rend false si aucun mode
// n'est actif — écran de login, changement de carte.
//
// 🔴 LECTURE BRUTE (`ActiveMode`), PAS `ActiveModeIfReady()`. Les deux ne disent
// pas la même chose pendant un changement de carte : le getter est GATÉ et rend
// 0 tant que le manager n'est pas en état 1. Ce pont existait en TROIS copies ;
// deux portaient ce choix par écrit, la troisième prenait l'autre porte sans le
// dire. Le choix majoritaire — et le seul argumenté — est celui-ci.
//
// ⚠ Le SEH est ici et non chez l'appelant : `rag::ModeSendMsg` déréférence une
// vtable du client. Une des trois copies s'en passait, en comptant sur un `__try`
// englobant qu'elle ne pouvait pas garantir.
inline bool SendToActiveMode(int cmd, int p2 = 0, int p3 = 0, int p4 = 0,
                             int p5 = 0) {
  __try {
    void* mode = ActiveMode();
    if (mode == nullptr) return false;
    ModeSendMsg(mode, cmd, p2, p3, p4, p5);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}


// La même, sur le mode actif — la forme de LOIN la plus fréquente. Rend 0 sans
// rien envoyer si aucun mode n'est disponible, ce que tous les appelants
// vérifiaient déjà à la main.
inline int ActiveModeSendMsg(int cmd, int p2 = 0, int p3 = 0, int p4 = 0,
                             int p5 = 0) {
  return ModeSendMsg(ActiveModeIfReady(), cmd, p2, p3, p4, p5);
}

// ── Les deux formes SOUS SEH, et pourquoi il y en a DEUX ─────────────────────
// Presque toutes nos fenêtres envoient leurs commandes depuis un rendu ImGui :
// une exception qui traverserait la frame laisserait la pile de dessin à moitié
// empilée. Elles enveloppaient donc l'envoi dans un `__try` — CINQ fichiers en
// portaient leur propre copie, sous TROIS noms (`ModeCmd`, `SendModeCmd`, et une
// variante rendant un bool).
//
// 🔴 Mais ces cinq copies ne faisaient pas toutes la même chose, et RIEN dans
// leur nom ne le disait : rodex, trade et game_menu passaient par le getter GATÉ
// (`ActiveModeIfReady`), tandis que weapon_refine et make_item lisaient le
// pointeur BRUT — l'un écrivant `*(void**)kActiveModePtr`, l'autre
// `ActiveMode()`, deux orthographes de la même chose. Les deux dernières
// portaient un commentaire « ⚠ lecture brute » : l'intention était donc
// délibérée, et les fondre aurait changé leur comportement pendant un
// changement de carte (cf. le bloc ActiveModeIfReady plus haut).
//
// D'où deux fonctions et non une, dont le NOM porte enfin la distinction.
// Le bool rend « un mode était là et la commande est partie » — game_menu s'en
// sert pour ne pas refermer son menu quand il n'y a personne à qui parler ; les
// autres appelants l'ignorent, ce qui est correct.

// Mode actif GATÉ sur l'état du gestionnaire — la forme à utiliser par défaut.
inline bool ActiveModeSendMsgSafe(int cmd, int p2 = 0, int p3 = 0, int p4 = 0,
                                  int p5 = 0) {
  __try {
    void* mode = ActiveModeIfReady();
    if (!mode) return false;
    ModeSendMsg(mode, cmd, p2, p3, p4, p5);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Pointeur du mode lu BRUT, sans la garde d'état. Ne l'employer que là où c'est
// un choix assumé : elle répond encore pendant les instants où la gatée rend 0.
inline bool RawModeSendMsgSafe(int cmd, int p2 = 0, int p3 = 0, int p4 = 0,
                               int p5 = 0) {
  __try {
    void* mode = ActiveMode();
    if (!mode) return false;
    ModeSendMsg(mode, cmd, p2, p3, p4, p5);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Quelques commandes RENDENT un objet plutôt qu'un code — la 8 du mode de login
// rend le CHARACTER_INFO d'un slot, ou nullptr si le slot est vide ou la liste
// pas encore arrivée. Même appel, seul le type de retour déclaré change : sur
// x86 les deux lisent EAX, et cette variante évite un aller-retour par `int`
// pour un pointeur.
inline void* ModeSendMsgPtr(void* mode, int cmd, int p2 = 0, int p3 = 0,
                            int p4 = 0, int p5 = 0) {
  if (!mode) return nullptr;
  using SendMsgPtrFn = void*(__thiscall*)(void*, int, int, int, int, int);
  const uintptr_t* vt = *reinterpret_cast<uintptr_t**>(mode);
  return reinterpret_cast<SendMsgPtrFn>(vt[kVfModeSendMsg / 4])(mode, cmd, p2,
                                                                p3, p4, p5);
}

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

// ── Le reste du bloc « personnage LOCAL » (g_Own_*) ──────────────────────────
// Le client tient un cache plat de l'identité et des stats du joueur, semé au
// char-select (CharSelectMode_OnMsg case 0x2719, entrée de 0xAF octets) puis
// tenu à jour paquet par paquet par le dispatcher ZC_PAR_CHANGE
// (FUN_00cceff0). Ce cache est la source de TOUT ce que le HUD natif affiche.
//
// Le projet le recopiait par morceaux dans une vingtaine de fichiers, chacun
// rebaptisant ce qu'il prenait — l'AID à lui seul portait SIX noms
// (kAccountAid, kOwnAccountAid, kOwnAccountId, kOwnAid, kOwnAidAddr,
// kOwnHandlePtr), tous sur 0x015fb9a4, aucun ne sachant les autres. Les voici
// une fois, sous les noms de l'IDB.
//
// ⚠ Ces globales n'existent QU'EN JEU. Au char-select et à l'écran de login
// elles portent un vestige de la session précédente : un lecteur qui tourne
// hors map doit d'abord se demander s'il est en jeu, pas y croire sur parole.

// Identité. kOwnAccountIdAddr est l'AID du compte, qui sert AUSSI de GID à
// notre propre acteur — c'est par lui qu'on se reconnaît dans une liste
// d'acteurs ou dans un paquet de zone. kOwnCharIdAddr est le CID du
// personnage, un tout autre nombre : les confondre fait échouer en silence
// tout ce qui s'adresse au personnage (RODEX, guilde, groupe).
constexpr uintptr_t kOwnAccountIdAddr = 0x015fb9a4;  // g_Account_Aid
constexpr uintptr_t kOwnCharIdAddr    = 0x015fb9a8;  // g_Own_CharId
constexpr uintptr_t kOwnCharNameAddr  = 0x01602568;  // char[] NU, pas une std::string
constexpr uintptr_t kOwnJobIdAddr     = 0x015fb9c8;  // g_Own_JobId (le job RÉEL)

inline uint32_t OwnAccountId() { return *reinterpret_cast<uint32_t*>(kOwnAccountIdAddr); }
// Le même, sous SEH : cinq fichiers le portaient, sous trois noms (OwnAid,
// OwnGid, OwnAidSEH). C'est le GID du joueur autant que son AID — le client
// n'en fait qu'un (cf. la fiche « durée de vie d'un état »).
inline uint32_t OwnAccountIdSafe() {
  __try {
    return OwnAccountId();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
inline uint32_t OwnCharId()    { return *reinterpret_cast<uint32_t*>(kOwnCharIdAddr); }
inline int      OwnJobId()     { return *reinterpret_cast<int*>(kOwnJobIdAddr); }

// ── L'APPARENCE du personnage, telle que le client la tient ─────────────────
// Trois globales voisines, lues par l'éditeur de style ET par le panneau
// d'infos de base — chacun les déclarait pour son compte.
//
// ⚠ `kOwnClothesColorAddr` est un CACHE MORT côté client (cf.
// project_own_look_globals) : il n'est PAS la source de vérité du rendu, qui
// vient de l'acteur. Le lire renseigne sur ce que le client CROIT, pas sur ce
// qu'il affiche. Le style de tête, lui, est fiable.
constexpr uintptr_t kOwnHairStyleAddr   = 0x015fb278;
constexpr uintptr_t kOwnClothesColorAddr = 0x015fb28c;
constexpr uintptr_t kOwnHairColorAddr   = 0x015fb290;

// Le SEXE du personnage, par appel natif : `Session_GetSex()` — les sprites de
// corps et les palettes en dépendent, et la globale équivalente n'est pas
// exposée. Deux fichiers la déclaraient.
constexpr uintptr_t kOwnSexAddr = 0x00d84760;

// Expérience. Quatre INT64 ; la plupart des lecteurs n'en prennent que le mot
// bas, ce qui suffit tant que la valeur tient sur 31 bits — au-delà (serveurs à
// très haut taux) il faut lire les huit octets.
constexpr uintptr_t kOwnBaseExpAddr     = 0x015fb9d0;
constexpr uintptr_t kOwnBaseExpNextAddr = 0x015fb9d8;
constexpr uintptr_t kOwnJobExpNextAddr  = 0x015fb9e0;  // ⚠ NEXT avant COURANT
constexpr uintptr_t kOwnJobExpAddr      = 0x015fb9e8;

// Points non dépensés.
constexpr uintptr_t kOwnStatusPointsAddr = 0x015fb9f4;
constexpr uintptr_t kOwnSkillPointsAddr  = 0x015fb9fc;

// Coût de la PROCHAINE montée de chaque stat, même ordre et même pas de 4 que
// kStatBaseAddr — le serveur l'envoie, le client ne le recalcule pas.
// ⚠ Pas d'accesseur ici, contrairement à StatBase/StatBonus : les trois
// appelants lisent ce bloc à travers LEUR lecteur gardé par SEH, et un accesseur
// nu leur ferait perdre cette garde. C'est l'adresse qu'ils partagent.
constexpr uintptr_t kStatRaiseCostAddr = 0x015fba3c;

// Sous-stats de combat, telles que la fenêtre Status les montre. Le bloc
// atk/def/matk/mdef était recopié À L'IDENTIQUE dans trois fichiers (la fenêtre
// Status retouchée, la feuille de personnage et le diagnostic), aux mêmes
// valeurs et sous les mêmes noms — trois occasions de se tromper au portage.
//
// ⚠ Chaque paire est {partie de l'ÉQUIPEMENT, partie des STATS} : la fenêtre
// native les affiche « a + b » et non additionnées.
constexpr uintptr_t kOwnAtk1Addr  = 0x015fba58;  // ATK équipement
constexpr uintptr_t kOwnAtk2Addr  = 0x015fba6c;  // ATK stats
constexpr uintptr_t kOwnMdefSoftAddr = 0x015fba5c;
constexpr uintptr_t kOwnDefSoftAddr  = 0x015fba64;
constexpr uintptr_t kOwnDefHardAddr  = 0x015fba68;
constexpr uintptr_t kOwnMatkMaxAddr  = 0x015fba70;
constexpr uintptr_t kOwnMatkMinAddr  = 0x015fba74;
constexpr uintptr_t kOwnMdefHardAddr = 0x015fba78;

constexpr uintptr_t kOwnHitAddr          = 0x015fba7c;
constexpr uintptr_t kOwnFleeAddr         = 0x015fba80;
constexpr uintptr_t kOwnCritAddr         = 0x015fba84;
constexpr uintptr_t kOwnPerfectDodgeAddr = 0x015fba88;

// 🔴 CE N'EST PAS L'ASPD — c'est l'amotion, le délai entre deux coups en
// millisecondes, tel que le SERVEUR le calcule et l'envoie. status_tweaks
// l'appelait `kAspdRaw`, ce qui laissait croire à une ASPD qu'il suffirait
// d'afficher : le nombre que le joueur lit est DÉRIVÉ, et plus l'amotion est
// GRANDE plus l'ASPD est petite. La conversion est ci-dessous ; elle était
// recopiée à l'identique dans cinq fichiers.
constexpr uintptr_t kOwnAttackDelayAddr = 0x015fba54;  // g_Own_AttackDelay (ms)

inline int AspdFromAmotion(int amotion_ms) { return (2000 - amotion_ms) / 10; }

// Divers champs du même bloc.
constexpr uintptr_t kAmmoEquippedInvIndexAddr = 0x015fba8c;  // 0 = aucune munition
constexpr uintptr_t kOwnWalkSpeedAddr         = 0x015fba94;  // ms par cellule
constexpr uintptr_t kOwnMannerAddr            = 0x015fba98;  // négatif = muet
constexpr uintptr_t kWeightMaxAddr            = 0x015fba9c;
constexpr uintptr_t kWeightCurAddr            = 0x015fbaa0;

// Seuil de SURCHARGE, en pourcentage : au-delà, le client teinte le poids en
// rouge — et le serveur commence à refuser certaines actions. C'est une valeur
// envoyée par le serveur, pas une constante : la lire évite de la supposer à 50.
constexpr uintptr_t kOverweightPctAddr = 0x01602324;

// Extension d'inventaire envoyée par le serveur.
//
// 🔴 ELLE NE VAUT PAS LE NOMBRE DE CASES. Le client calcule `100 + extension`,
// avec une base de 100 CÂBLÉE EN DUR, là où Moonlight définit
// `INVENTORY_BASE_SIZE = 200` et envoie donc `cases - 200`. Pour un personnage
// ordinaire l'extension vaut 0 et le compteur natif afficherait « X / 100 »,
// rouge dès 101. Tout lecteur doit ajouter le décalage des deux bases.
constexpr uintptr_t kInventoryExpansionAddr = 0x01602354;
constexpr int kInventoryClientBase = 100;  // la base câblée dans le client
constexpr int kInventoryServerBase = 200;  // INVENTORY_BASE_SIZE côté Moonlight

// Vitalité. Ces quatre-là sont dans un bloc SÉPARÉ (0x015ff9xx), pas avec les
// stats — c'est pour ça qu'on les retrouvait recopiées même dans les fichiers
// qui tenaient déjà une partie du reste.
constexpr uintptr_t kOwnHpAddr    = 0x015ff908;
constexpr uintptr_t kOwnMaxHpAddr = 0x015ff90c;
constexpr uintptr_t kOwnSpAddr    = 0x015ff910;
constexpr uintptr_t kOwnMaxSpAddr = 0x015ff914;

inline int OwnHp()    { return *reinterpret_cast<int*>(kOwnHpAddr); }
inline int OwnMaxHp() { return *reinterpret_cast<int*>(kOwnMaxHpAddr); }
inline int OwnSp()    { return *reinterpret_cast<int*>(kOwnSpAddr); }
inline int OwnMaxSp() { return *reinterpret_cast<int*>(kOwnMaxSpAddr); }

// ── Modèles SESSION des trois conteneurs ─────────────────────────────────────
// Inventaire, chariot et storage sont TROIS `std::list<ItemSkillInfo>` de même
// forme, chacune publiée dans un global qui porte sa sentinelle. C'est le modèle
// que le client tient à jour sur les paquets serveur, QUEL QUE SOIT l'état de ses
// fenêtres — à ne pas confondre avec la liste d'AFFICHAGE d'une fenêtre native
// (wnd+0xe8), que masquer la fenêtre vide.
//
// `kInventoryListAddr` était redéclarée sous le nom `kInvListHead` dans NEUF
// fichiers, à l'identique. C'était la plus recopiée du projet après la session
// elle-même.
//
// 🔴 CES CONSTANTES SONT L'ADRESSE DU GLOBAL, PAS LA SENTINELLE. La sentinelle
// est ce que le global CONTIENT, et c'est à elle qu'il faut comparer pour
// terminer le parcours : la liste est CIRCULAIRE, donc s'arrêter sur l'adresse du
// global ne rencontre jamais la condition de fin et resomme le conteneur jusqu'au
// garde-fou. (Piège payé par craft_atlas.)
//
// ⚠ La liste d'inventaire EXCLUT les pièces portées ; le serveur, lui, les
// compte. Un total calculé dessus est donc plus petit que celui du serveur — cf.
// inventory_viewer, qui recolle les deux.
//
// Sources natives : `Inventory_GetCount` = *(session+0x16f4) et `FUN_00d5acb0`
// qui parcourt la liste ; côté chariot `Cart_GetCount` = *(session+0x1724) et
// `Cart_CopyItemAt`.
constexpr uintptr_t kInventoryListAddr  = 0x015fbab0;  // session+0x16f0
constexpr uintptr_t kInventoryCountAddr = 0x015fbab4;  // _Mysize de la liste
constexpr uintptr_t kStorageListAddr    = 0x015fbad8;  // session+0x1718
constexpr uintptr_t kCartListAddr       = 0x015fbae0;  // session+0x1720
constexpr uintptr_t kCartCountAddr      = 0x015fbae4;  // session+0x1724

// ── Guilde du joueur ─────────────────────────────────────────────────────────
// L'objet CGuild du client (relevé x32dbg 2026-07-11) : +0 = nom de guilde en
// std::string MSVC (donc SSO — buffer en place tant que la capacité < 0x10,
// pointeur au-delà), +0xA8 = l'id.
//
// ⚠ Le label Ghidra « ActiveTabIndex » sur 0x0159c230 est FAUX : c'est bien
// l'id de guilde. Quatre fichiers le lisaient déjà sous ce nom-là sans le
// savoir.
constexpr uintptr_t kGuildObjAddr      = 0x0159c188;
constexpr uintptr_t kGuildLevelAddr    = 0x0159c1e8;
constexpr uintptr_t kOwnGuildIdAddr    = 0x0159c230;
constexpr uintptr_t kGuildIsMasterAddr = 0x0159c23c;
constexpr int       kGuildNameOffset   = 0x00;  // std::string, relatif à kGuildObjAddr
constexpr int       kGuildListHeadOff  = 0xdc;  // sentinelle du roster, idem

inline int OwnGuildId() { return *reinterpret_cast<int*>(kOwnGuildIdAddr); }

// ── Nom d'une CLASSE, et la classe ajustée par la monture ────────────────────
// `Job_GetDisplayNameOrResName` : __thiscall(session, classId, sex) -> const
// char*, dans la CODE-PAGE du client (cf. kClientCodePageAddr) — jamais en
// UTF-8. Chaîne vide si l'id est hors de la table `jobName.lub`.
//
// 🔴 SON NOM DIT VRAI : elle rend TANTÔT un libellé à afficher, TANTÔT un nom de
// FICHIER, et c'est la même table qui porte les deux. Le projet l'avait donc
// déclarée sous `kJobDisplayName` dans cinq fichiers, `kJobNameAddr` dans un
// sixième et `kJobResName` dans un septième, chacun croyant nommer un getter
// différent. Ce n'en est qu'un — et le nom qu'il rend ne coïncide PAS avec
// l'AegisName du serveur (`JT_CHONCHON` -> « Chocho », fichier `Chocho.spr`).
//
// ⚠ Le résultat pointe dans la table du client : le RECOPIER, ne pas garder le
// pointeur. Et l'appel traverse une table Lua — les appelants qui le font par
// entité et par frame doivent mettre en cache (cf. ragnarok/social.h).
constexpr uintptr_t kJobNameOrResNameAddr = 0x00d5bb40;

// `Job_ResolveMountedClassFromOption` : __thiscall(session) -> int. La classe
// telle que le client l'AFFICHE, monture comprise — elle lit l'état d'option du
// joueur (peco, dragon, madogear…) et rend la classe montée correspondante.
//
// 🔴 À NE PAS CONFONDRE avec `kOwnJobIdAddr`, qui porte la classe BRUTE. Les deux
// sont légitimes et ne s'échangent pas : le nom affiché au joueur veut celle-ci,
// le sprite de CORPS veut l'autre (`Job_ResolveBodyClass` refait lui-même le
// remap à partir des deux).
constexpr uintptr_t kJobResolveMountedClassAddr = 0x00d5b580;

// ── L'appel typé, et le PIÈGE de cette famille : le troisième argument ───────
//
// HUIT fichiers refaisaient cet appel, sous six noms (`ClassName`,
// `ClassNameSEH`, `JobName`, `JobNameSEH`, `JobDisplayName`, `JobNameAnsi`) et
// deux orthographes de convention : `__thiscall(session, job, sex)` d'un côté,
// `__fastcall(session, nullptr, job, sex)` de l'autre. Les deux marchent — EDX
// n'est pas lu, donc les arguments tombent aux mêmes emplacements de pile — mais
// rien ne le disait, et il fallait le vérifier à chaque lecture.
//
// 🔴🔴 CE QUI DIVERGEAIT VRAIMENT, C'EST `sex`, ET AUCUN NOM NE LE PORTAIT.
// `-1` laisse le client trancher AVEC LE SEXE DU JOUEUR LOCAL ; `99` demande le
// nom de la classe de BASE, sans variante de sexe. Nommer un TIERS avec `-1`
// donne donc un libellé faux dès que ce tiers n'a pas notre sexe — c'est
// exactement le défaut trouvé dans la fenêtre de groupe le 2026-08-24, et deux
// autres appelants le portaient encore. D'où deux fonctions au nom explicite
// plutôt qu'un paramètre que tout le monde recopie de travers.
//
// ⚠ Le résultat pointe dans la table du client : le RECOPIER. Et l'appel
// traverse une table Lua — qui nomme par entité et par frame doit mettre en
// cache (`rag::social::JobName` le fait, avec en prime un repli « Classe %d »).
constexpr int kJobSexBase = 99;  // nom de classe de base, sans variante de sexe
constexpr int kJobSexSelf = -1;  // « celui du joueur local », décidé par le client

inline const char* JobNameForSex(int job_id, int sex) {
  __try {
    using JobNameFn = const char*(__thiscall*)(void*, unsigned, int);
    const char* n = reinterpret_cast<JobNameFn>(kJobNameOrResNameAddr)(
        reinterpret_cast<void*>(kSessionAddr), static_cast<unsigned>(job_id),
        sex);
    return n ? n : "";
  } __except (EXCEPTION_EXECUTE_HANDLER) { return ""; }
}

// Le nom d'une classe QUELCONQUE — celle d'un autre joueur, d'une ligne de
// groupe, d'un courrier, d'une liste. C'est la forme par défaut.
inline const char* JobName(int job_id) {
  return JobNameForSex(job_id, kJobSexBase);
}

// Le nom tel que le client le dirait POUR NOUS. À n'employer que sur notre
// propre classe : sur celle d'un tiers, il ment.
inline const char* JobNameMySex(int job_id) {
  return JobNameForSex(job_id, kJobSexSelf);
}

// Notre classe telle qu'AFFICHÉE, monture comprise, et son nom. Deux fichiers
// enchaînaient ces deux appels natifs à l'identique.
inline int OwnDisplayedJobId() {
  __try {
    using ResolveFn = int(__thiscall*)(void*);
    return reinterpret_cast<ResolveFn>(kJobResolveMountedClassAddr)(
        reinterpret_cast<void*>(kSessionAddr));
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
inline const char* OwnClassName() {
  return JobNameMySex(OwnDisplayedJobId());
}

// `Social_GetPartyMemberCount` : __thiscall(session) -> int. Zéro quand on n'est
// dans aucun groupe — c'est le test « suis-je en groupe ? » que le client se pose
// lui-même. Redéclarée dans quatre fichiers sous deux noms.
constexpr uintptr_t kPartyMemberCountAddr = 0x00d5cf50;

// ── Position du curseur, en pixels ÉCRAN ─────────────────────────────────────
// Ce que le client a retenu du dernier WM_MOUSEMOVE, et donc ce sur quoi ses
// propres hit-tests raisonnent.
//
// ⚠ La lire ICI plutôt que d'appeler GetCursorPos : en plein écran, avec un
// device redimensionné, les deux ne coïncident pas — et c'est CELLE-CI qui
// décide de ce que le natif croit survoler.
constexpr uintptr_t kMouseScreenXAddr = 0x011e40d4;
constexpr uintptr_t kMouseScreenYAddr = 0x011e40d8;

// ── Le formateur à séparateurs de milliers du client ─────────────────
// __cdecl(valeur, tampon, taille) : écrit « 1,234,567 ». Le prendre au client
// plutôt que de le réécrire garantit que nos chiffres se lisent comme les
// siens — même séparateur, même regroupement.
constexpr uintptr_t kFormatThousandsAddr = 0x00a948d0;

// ── Le mode FAVORIS de l'inventaire ────────────────────────────
// Un OCTET, basculé par le bouton « Deal » du pied de l'inventaire. Purement
// client : le serveur l'ignore complètement.
//
// 🔴 UN SEUL DRAPEAU, DEUX CONSÉQUENCES — et c'est pour ça que le projet le
// déclarait sous deux noms sans voir qu'il s'agissait du même octet :
//   * `kFavFlag` (retouche d'inventaire) : il ajoute la catégorie « favoris »
//     aux onglets, d'où le `3 + drapeau` du compte de catégories ;
//   * `kDealLockGlobal` (viewer, boutique NPC) : il EXCLUT les favoris de la
//     liste vendable — `if (trouvé && (!favori || !drapeau))` dans
//     `NpcSell_BuildSellableList`.
// Reprendre la place du handler natif oblige donc à rejouer ce filtre
// soi-même, sinon plus personne ne le lit et les favoris redeviennent vendables.
constexpr uintptr_t kFavoriteModeFlagAddr = 0x01600553;

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

// `std::string::assign(const char*)` du CLIENT — __thiscall(this, src) -> this.
// Le pendant du _Tidy ci-dessus : c'est par elle qu'on REMPLIT une std::string
// que le natif nous a donnée à remplir (le tampon de saisie du chat, le champ de
// destination d'un lien de navigation…). Elle était redéclarée dans cinq
// fichiers sous deux noms, `kStdStringAssign` et `kStdStringAssignAddr`.
//
// ⚠ Elle alloue avec l'operator new du CLIENT : la string reste cohérente pour
// lui, et c'est bien à `kStdStringDtorAddr` qu'il faudra la rendre.
constexpr uintptr_t kStdStringAssignAddr = 0x004f1940;

// `std::string::string(const char*)` du CLIENT — __thiscall(this, src) -> this.
// Construit EN PLACE dans un tampon non initialisé, là où `assign` remplit une
// string déjà construite : deux gestes distincts, et confondre les deux laisse
// soit une string jamais initialisée, soit une fuite. Deux fichiers la
// déclaraient (dont une graphie en majuscules, invisible à un grep sensible à
// la casse). Ce qu'elle construit se rend à `kStdStringDtorAddr`.
constexpr uintptr_t kStdStringCtorCStrAddr = 0x004e5330;

// ── Mode combat (`/bm`, `/battlemode`) ──────────────────────────────────────
// Un OCTET : 1 = la barre de saisie du chat est masquée et Entrée l'ouvre,
// 0 = barre permanente. Basculé par la commande 213 de UIHotKeyWnd et par la
// case 135 de `Chat_HandleChatMessage`, au moyen d'un `setz` — c'est une vraie
// bascule, un « on »/« off » en argument est ignoré.
//
// Deux surfaces le LISENT (la chatbox, pour savoir si sa barre doit se montrer ;
// le panneau de raccourcis, pour afficher l'état de la case) et chacune le
// déclarait, l'une sous `kBattleModeFlag`, l'autre sous `kChangeChatModeAddr`.
constexpr uintptr_t kBattleModeFlagAddr = 0x0131f50e;

// Vrai si le mode combat est actif. En cas de doute (lecture impossible) rend
// FAUX — donc barre visible : jamais de ligne de chat perdue.
inline bool BattleModeOn() {
  __try {
    return *reinterpret_cast<const uint8_t*>(kBattleModeFlagAddr) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ── Carte COURANTE, telle que le client la nomme ─────────────────────────────
// Le global que le client injecte lui-même dans son gabarit de minimap
// (`유저인터페이스\map\%s.bmp`, UIMiniMapWnd_DrawContent @0x00962441). Rendu SANS
// extension : le global la porte parfois (`prontera.rsw`), et aucun appelant
// n'en veut.
//
// ⚠ C'est la source du CLIENT, pas la nôtre. La minimap lui préfère le
// `map_name` que lui passe OnModeSwitch — sa propre plomberie, dont elle répond
// — et ne retombe ici qu'à défaut. Un appelant qui n'a pas ce fil (la
// navigation) se contente de celle-ci.
constexpr uintptr_t kCurrentMapNameAddr = 0x015fb9ac;

inline bool CurrentMapName(char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  __try {
    const char* name = reinterpret_cast<const char*>(kCurrentMapNameAddr);
    if (!name || !name[0]) return false;
    strncpy_s(out, cap, name, _TRUNCATE);
    if (char* dot = strrchr(out, '.')) *dot = '\0';
    return out[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '\0';
    return false;
  }
}

// ── Nom LISIBLE d'un lieu ────────────────────────────────────────────────────
// « Prontera » pour `prontera`, « PvP : Room Copass » pour `pvp_n_3-5` — le nom
// que la grande carte native met en titre. `Social_GetMapDisplayName` est
// __stdcall et prend un `const char*` : rien à marshaler, contrairement à la
// plupart des accesseurs du client.
//
// 🔴 Elle attend le nom AVEC son extension `.rsw` — c'est `sub_D9AB80`
// (0x00d9ab80) qui la recolle avant d'appeler, et sans elle la table ne trouve
// rien. Le résultat pointe dans la DB de cartes du client : on le RECOPIE tout
// de suite plutôt que de garder le pointeur.
//
// Ici plutôt que chez son premier appelant (la minimap) parce que le second est
// arrivé : un lien de navigation posté dans le chat doit s'afficher avec le nom
// que le joueur lit partout ailleurs, pas avec l'identifiant de la carte.
constexpr uintptr_t kMapDisplayNameAddr = 0x00d5bcf0;

inline bool MapDisplayName(const char* map_no_ext, char* out, size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  __try {
    if (!map_no_ext || !map_no_ext[0]) return false;
    char with_ext[80];
    _snprintf_s(with_ext, sizeof(with_ext), _TRUNCATE, "%s.rsw", map_no_ext);
    using Fn = const char*(__stdcall*)(const char*);
    const char* name = reinterpret_cast<Fn>(kMapDisplayNameAddr)(with_ext);
    if (!name || !name[0]) return false;
    strncpy_s(out, cap, name, _TRUNCATE);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '\0';
    return false;
  }
}

// ── Trois drapeaux d'état que plusieurs surfaces consultent ─────────────────
// Chacun était déclaré dans DEUX fichiers, dont un dans `ragnarok/` — ce qui
// les rendait invisibles au relevé des adresses « partagées par au moins deux
// fichiers de features », qui excluait ce dossier. L'angle mort a été comblé le
// 2026-08-24 par un relevé croisé catalogue/copies.

// Cible d'envoi de la ligne de chat : 0 public, 1 groupe, 2 guilde, 3 clan,
// 4 alliés. Lue par la chatbox (pour teinter la saisie) et par l'envoi de macro.
constexpr uintptr_t kInputTargetModeAddr = 0x015ff838;

// L'état de clan du joueur, via un POINTEUR : l'appartenance se lit à
// `*(byte*)(*kClanStatePtrAddr + 0x5C)`. Sert de garde aux deux chemins qui
// peuvent viser le clan — sans elle, choisir la cible « clan » sans en avoir un
// envoie une ligne que le serveur jette sans un mot.
constexpr uintptr_t kClanStatePtrAddr = 0x0159c07c;

// Une relecture de REPLAY est en cours. Deux conséquences sans rapport
// apparent, et c'est pour ça que les deux fichiers ne se savaient pas voisins :
// l'horloge du jeu vient alors du gestionnaire de réassemblage et non de
// `timeGetTime` (comparer les deux donne des cooldowns fantômes de plusieurs
// heures), et le client REFUSE d'ouvrir un menu contextuel d'entité.
constexpr uintptr_t kReplayActiveAddr = 0x015beecc;

// ── Le filtre de mots interdits du client ───────────────────────────────────
// `BannedWord_Contains` __thiscall(table, texte_cp949) -> bool : vrai si le
// texte contient un mot interdit. ⚠ C'est la NÉGATION de « propre » — un nom de
// fonction en `Scan…` inviterait à s'y tromper.
//
// Deux chemins la consultent avant d'envoyer quoi que ce soit au serveur : le
// nom qu'on donne à un familier, et le titre d'un salon de chat. Le texte doit
// être en CP949, pas en UTF-8.
constexpr uintptr_t kBannedWordContainsAddr = 0x00a85be0;

}  // namespace rag
