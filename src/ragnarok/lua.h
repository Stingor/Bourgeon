#pragma once

// ── API C Lua 5.1 du client ──────────────────────────────────────────────────
// (client 20250716, base 0x400000 ; carte complète dans reference_lua_c_api)
//
// Quatre fichiers appelaient cette API, chacun avec son propre jeu de constantes
// et de typedefs — au point que deux orthographes coexistaient pour CHAQUE
// fonction, l'une suffixée `B` (kLuaGetField / kLuaGetFieldB), et TROIS pour
// LUA_GLOBALSINDEX (kLuaGlobals, kLuaGlobalsB, kLuaGlobalsIdx). Les signatures,
// elles, étaient rigoureusement identiques partout : elles deviennent donc de
// vraies enveloppes, et les `reinterpret_cast` de trois lignes disparaissent.

#include <cstddef>
#include <cstdint>
#include <excpt.h>  // __try / __except

namespace lua {

// ── Le state ─────────────────────────────────────────────────────────────────
// 🔴 PIÈGE, LA RAISON D'ÊTRE DE CET EN-TÊTE : 0x015ffd78 n'est PAS le
// lua_State, ni même un pointeur vers lui. C'est un HOLDER : le client y range
// un pointeur M, et le vrai state est `*M`. Il faut donc DEUX
// déréférencements — `L = *(void**)(*(void**)0x015ffd78)`. Passer M à la place
// de *M ne produit pas une erreur propre : ça crashe.
//
// Les quatre appelants le faisaient correctement, mais chacun de son côté, en
// deux lignes recopiées. Encodé ici une fois pour toutes.
// Les DEUX niveaux ont chacun leur usage, et ce n'est pas une redondance :
//   Manager() = *(0x015ffd78)  — l'objet GESTIONNAIRE Lua du client. C'est lui
//               le `this` des méthodes du client, par exemple kExecFileAddr.
//   State()   = *Manager()     — le lua_State brut, seul argument valide de
//               toute l'API C de Lua.
// Confondre les deux ne donne pas une erreur propre : ça crashe.
constexpr uintptr_t kStateHolderAddr = 0x015ffd78;

inline void* Manager() { return *reinterpret_cast<void**>(kStateHolderAddr); }

inline void* State() {
  void* manager = Manager();
  return manager ? *reinterpret_cast<void**>(manager) : nullptr;
}

// Pseudo-index de la table des globaux (LUA_GLOBALSINDEX). Lua 5.1 : -10002.
constexpr int kGlobalsIndex = -10002;

// ── Adresses ─────────────────────────────────────────────────────────────────
constexpr uintptr_t kGetFieldAddr   = 0x00519df0;  // lua_getfield(L, idx, k)
constexpr uintptr_t kPCallAddr      = 0x0051a290;  // lua_pcall(L, nargs, nres, errfunc)
constexpr uintptr_t kPushNumberAddr = 0x0051a4b0;  // lua_pushnumber(L, double)
constexpr uintptr_t kSetTopAddr     = 0x0051aab0;  // lua_settop(L, idx)
constexpr uintptr_t kToBooleanAddr  = 0x0051abf0;  // lua_toboolean(L, idx)
constexpr uintptr_t kToLStringAddr  = 0x0051aca0;  // lua_tolstring(L, idx, &len)
constexpr uintptr_t kToNumberAddr   = 0x0051ad20;  // lua_tonumber(L, idx)
constexpr uintptr_t kCheckStackAddr = 0x0051b570;  // lua_checkstack(L, extra)

// Deux wrappers du CLIENT (pas de l'API Lua), laissés aux appelants : leurs
// signatures ne se ramènent pas à une enveloppe commune.
//   0x00a9a7d0 Lua_CallGlobal_va — ⚠ DÉTRUIT sa std::string passée par valeur,
//              donc réservé aux noms de ≤15 caractères (SSO).
//   0x00a9bc90 exécution d'un fichier .lub, __thiscall.
constexpr uintptr_t kCallGlobalVaAddr = 0x00a9a7d0;
constexpr uintptr_t kExecFileAddr     = 0x00a9bc90;

// ── Deux raccourcis natifs vers les tables de compétences ────────────────────
// Le client se donne à lui-même des accesseurs C sur ses tables Lua, ce qui
// évite d'avoir à monter un appel `lua_pcall` pour une simple lecture. Ils sont
// `__cdecl(int id) -> char*`, et rendent une chaîne STATIQUE du client (à
// recopier, pas à conserver).
//
// 🔴 CE SONT DEUX TABLES DIFFÉRENTES, et c'est la distinction qui compte :
//   * kGetSkillNameAddr   -> le LIBELLÉ affiché (« Blessing ») ;
//   * kGetSkillIdNameAddr -> l'IDENTIFIANT (« AL_BLESSING »), qui est aussi le
//     nom du .bmp d'icône.
// Les deux lisent la DB Lua `SkillInfoList`, donc INDÉPENDAMMENT de ce que le
// personnage a appris — contrairement aux getters qui passent par la liste
// apprise et rendent du vide pour la compétence d'une autre classe.
//
// ⚠ Sentinelles à rejeter plutôt qu'à afficher : « Zero Skill », et tout ce qui
// contient « nknown ».
constexpr uintptr_t kGetSkillNameAddr   = 0x0073a1f0;  // GetSkillName(id)
constexpr uintptr_t kGetSkillIdNameAddr = 0x0073a140;  // GetSkillIdName(id)

// Les deux appels typés. Six fichiers recopiaient la signature sous deux noms
// (`GetSkillNameLua_t`, `GetSkillIdNameLua_t`) et l'un d'eux redéclarait en plus
// l'adresse pour son compte. Le retour est `const char*` ici alors que le natif
// rend un `char*` : la chaîne appartient au CLIENT, l'écrire serait une faute
// que le type interdit maintenant.
inline const char* SkillName(int skill_id) {
  return reinterpret_cast<const char* (__cdecl*)(int)>(kGetSkillNameAddr)(skill_id);
}
inline const char* SkillIdName(int skill_id) {
  return reinterpret_cast<const char* (__cdecl*)(int)>(kGetSkillIdNameAddr)(skill_id);
}

// ── Enveloppes ───────────────────────────────────────────────────────────────
// Toute l'API C de Lua est __cdecl. Signatures vérifiées identiques dans les
// quatre appelants avant extraction.
inline void GetField(void* L, int idx, const char* key) {
  reinterpret_cast<void(__cdecl*)(void*, int, const char*)>(kGetFieldAddr)(L, idx, key);
}
inline void PushNumber(void* L, double value) {
  reinterpret_cast<void(__cdecl*)(void*, double)>(kPushNumberAddr)(L, value);
}
inline int PCall(void* L, int nargs, int nresults, int errfunc) {
  return reinterpret_cast<int(__cdecl*)(void*, int, int, int)>(kPCallAddr)(
      L, nargs, nresults, errfunc);
}
inline void SetTop(void* L, int idx) {
  reinterpret_cast<void(__cdecl*)(void*, int)>(kSetTopAddr)(L, idx);
}
inline int ToBoolean(void* L, int idx) {
  return reinterpret_cast<int(__cdecl*)(void*, int)>(kToBooleanAddr)(L, idx);
}
inline const char* ToLString(void* L, int idx, size_t* len) {
  return reinterpret_cast<const char*(__cdecl*)(void*, int, size_t*)>(kToLStringAddr)(
      L, idx, len);
}
inline double ToNumber(void* L, int idx) {
  return reinterpret_cast<double(__cdecl*)(void*, int)>(kToNumberAddr)(L, idx);
}
inline int CheckStack(void* L, int extra) {
  return reinterpret_cast<int(__cdecl*)(void*, int)>(kCheckStackAddr)(L, extra);
}

// Dépile les `count` valeurs du sommet (lua_pop de la vraie API est une macro).
inline void Pop(void* L, int count) { SetTop(L, -count - 1); }

// ── Appeler un global Lua avec UN argument numérique ─────────────────────────
// Le motif « champ global, argument, appel protégé, lecture, dépilage » était
// écrit QUATRE fois : `ResolveConcreteId` (SPR Lab), `HatLuaNum` et `HatLuaBool`
// (basic_info), et `HatEffectResName` juste en dessous. C'est le même geste, à la
// lecture du résultat près.
//
// ⚠ ET `HatLuaBool` N'AVAIT PAS LE `CheckStack`. Le commentaire de
// `HatEffectResName` raconte déjà l'avoir récupéré sur une copie sur deux ; une
// TROISIÈME copie s'en passait encore. On empile deux valeurs quatre lignes plus
// bas : sur un état Lua dont la pile est pleine, l'omission donne une corruption
// silencieuse au lieu d'un échec propre. Les trois l'ont désormais.
//
// `def` est rendu tel quel si Lua n'est pas prêt ou si l'appel échoue.
inline double CallGlobalNum(const char* fn, int arg, double def = 0.0) {
  double r = def;
  __try {
    void* L = State();
    if (L) {
      CheckStack(L, 3);
      GetField(L, kGlobalsIndex, fn);
      PushNumber(L, static_cast<double>(arg));
      if (PCall(L, 1, 1, 0) == 0) r = ToNumber(L, -1);
      Pop(L, 1);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { r = def; }
  return r;
}

inline bool CallGlobalBool(const char* fn, int arg, bool def = false) {
  bool r = def;
  __try {
    void* L = State();
    if (L) {
      CheckStack(L, 3);
      GetField(L, kGlobalsIndex, fn);
      PushNumber(L, static_cast<double>(arg));
      if (PCall(L, 1, 1, 0) == 0) r = ToBoolean(L, -1) != 0;
      Pop(L, 1);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { r = def; }
  return r;
}

// ── `GetHatEfResName(ordinal)` : le nom de ressource d'un effet de couvre-chef ──
//
// La séquence complète — champ global, argument, appel protégé, lecture de la
// chaîne, dépilage — était écrite DEUX fois, sous deux noms
// (`HatOrdinalToResNameRaw` dans basic_info, `ResolveResName` dans le SPR Lab).
//
// ⚠ Le `CheckStack` ne venait que d'une des deux copies. Il est repris : quatre
// lignes plus bas on empile deux valeurs, et un état Lua dont la pile est déjà
// pleine ferait autrement une corruption silencieuse plutôt qu'un échec propre.
//
// ⚠ NE PAS confondre avec le `ResolveResName` de ui/icon_cache : celui-là prend
// un NAMEID D'OBJET et passe par la DB d'items, pas par Lua.
inline void HatEffectResName(int ordinal, char* out, int cap) {
  if (!out || cap <= 0) return;
  out[0] = '\0';
  __try {
    void* L = State();
    if (!L) return;
    CheckStack(L, 3);
    GetField(L, kGlobalsIndex, "GetHatEfResName");
    PushNumber(L, static_cast<double>(ordinal));
    if (PCall(L, 1, 1, 0) == 0) {
      const char* s = ToLString(L, -1, nullptr);
      if (s && s[0]) {
        int n = 0;
        while (n < cap - 1 && s[n]) { out[n] = s[n]; ++n; }
        out[n] = '\0';
      }
    }
    Pop(L, 1);
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

}  // namespace lua
