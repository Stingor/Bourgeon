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

}  // namespace lua
