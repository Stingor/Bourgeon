#include "features/windows/entity_inspector.h"

#include <windows.h>

#include <cfloat>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "bourgeon.h"                            // SendPacket / RegisterRecvOpcode
#include "features/staff_gate.h"
#include "features/systems/bourgeon_opcodes.h"   // CZ 0x0F22 / ZC 0x0F23
#include "ragnarok/globals.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"
#include "utils/i18n.h"

using namespace mui;

// ── Adresses natives (client 20250716, no-ASLR) ──────────────────────────────
namespace {

// `Actor_FindByGid(gid) -> CActor*` : le raccourci global du client, celui-là
// même qu'emploie `EntityName_IsHostileOrSpecialUnit` pour aller lire l'état
// d'acteur. Il fait `GameMode_GetActive(g_ModeMgr)` puis
// `ActorList_FindByGID(gm+0xCC, gid)` — donc il rend nullptr proprement hors
// jeu, là où lire `kActiveModePtr` à la main rendrait un mode de login.
constexpr uintptr_t kActorFindByGid = 0x00d806a0;  // __stdcall(gid)

// Dictionnaire de noms `std::map<GID, CNameInfo>` à `GameMode+0x160`
// (docs/entity_nameplate_re.md §2).
//   · `Contains` ne fait que chercher : AUCUN effet de bord, on peut donc
//     l'appeler à chaque frame pour dire si le client a seulement une entrée ;
//   · `GetEntryOrRequest` rend l'entrée si elle est RENSEIGNÉE (+0x98 == 1) et,
//     sinon, met le GID dans la file de demande au serveur avant de rendre une
//     entrée vide STATIQUE. C'est le seul appel de ce fichier qui puisse
//     produire un paquet — d'où le throttle côté appelant.
constexpr uintptr_t kNameDictContains = 0x005a18e0;  // __thiscall(dict, gid)
constexpr uintptr_t kNameDictGetEntry = 0x005a1460;  // __thiscall(dict, gid)

// `Job_GetDisplayNameOrResName` : id de classe -> nom lisible (table Lua
// jobName/PCJobNameTable). `this` est l'ADRESSE du contexte de session, pas un
// pointeur à déréférencer ; sex = -1 laisse le client trancher lui-même, ce qui
// est valable EN JEU (et cette fenêtre n'existe qu'en jeu).
constexpr uintptr_t kJobDisplayName = 0x00d5bb40;  // __thiscall(ctx, classId, sex)

// Position monde -> cellule de carte (+ sous-position, dont on n'a que faire).
constexpr uintptr_t kWorldToTile = 0x00c6aa80;  // __thiscall(gm, x, z, …)

// CGameMode
constexpr int kGm_NameDict = 0x160;
constexpr int kGm_ActorMgr = 0x0cc;
constexpr int kScene_OwnActor = 0x2c;  // actorMgr -> acteur du joueur

// CNameInfo (docs/entity_nameplate_re.md §2). Chaque chaîne est une
// `std::string` de 0x18 octets : buffer, puis taille +0x10 et capacité +0x14 DU
// CHAMP (pas de l'objet).
constexpr int kName_Str     = 0x04;
constexpr int kName_Party   = 0x1c;
constexpr int kName_Guild   = 0x34;
constexpr int kName_Rank    = 0x4c;
constexpr int kName_Title   = 0x64;
constexpr int kName_Extra   = 0x7c;
constexpr int kName_TitleId = 0x94;
constexpr int kName_Valid   = 0x98;
constexpr int kField_Size   = 0x10;
constexpr int kField_Cap    = 0x14;

// CActor — les seuls champs établis (les autres attendent leur RE ; en montrer
// un « au jugé » serait exactement le contraire de ce que fait cette fenêtre).
constexpr int kActor_PosX      = 0x10;   // float, position monde X
constexpr int kActor_PosY      = 0x14;   // float, HAUTEUR
constexpr int kActor_PosZ      = 0x18;   // float, position monde Z
constexpr int kActor_Gid       = 0x110;  // GID/AID porté par l'acteur lui-même
constexpr int kActor_Type      = 0x314;  // byte ; {1, 6, 12} = hostile/spécial
constexpr int kActor_HeightOff = 0x3f4;  // float, offset de hauteur (le saut)
constexpr int kActorVt_GetGuildId = 0xc4;

// Catégories du quad de picking (docs/entity_context_menu_re.md §3).
// 🔴 La 3, c'est le PET : seul `CActorSprite_SubmitNameplateQuad` @0x00c58c48
// l'écrit, sous `acteur+0x314 == 7` (`CLIF_BL_PET`). Aucun quad ne vaut « objet
// au sol » — un objet au sol porte `objecttype == 2` et sort en catégorie 0.
constexpr int kPickActor      = 0;
constexpr int kPickNpc        = 1;
constexpr int kPickSkillUnit  = 2;
constexpr int kPickPet        = 3;
constexpr int kPickSpecial    = 4;

// Intervalle minimal entre deux passages par le chemin QUI DEMANDE.
//
// `CNameDict_GetEntryOrRequest` déduplique sa propre file d'attente (elle la
// parcourt avant d'y ajouter le GID), donc rappeler la fonction ne l'y met pas
// deux fois. Mais cette file est VIDÉE quand le client émet ses CZ_REQNAME : la
// rappeler après chaque vidage réinscrit le GID, et sur une cible qui n'aura
// JAMAIS de nom (unité de compétence, objet au sol) ça fait une demande par
// cycle de vidage, indéfiniment, tant que la fenêtre est ouverte.
//
// Une demi-seconde : assez lâche pour que ça ne pèse rien, assez serré pour
// qu'un nom qui arrive s'affiche sans qu'on ait l'impression d'attendre.
constexpr unsigned kNameRetryMs = 500;

// Corps clair du skin RO : `TextDisabled` y est illisible, d'où ces trois-là.
const ImVec4 kLabelCol(0.35f, 0.35f, 0.42f, 1.0f);
const ImVec4 kValueCol(0.10f, 0.10f, 0.13f, 1.0f);
const ImVec4 kMissingCol(0.55f, 0.33f, 0.08f, 1.0f);

using FindActorFn  = void*    (__stdcall*)(uint32_t);
using ContainsFn   = bool     (__thiscall*)(void*, uint32_t);
using GetEntryFn   = void*    (__thiscall*)(void*, uint32_t);
using JobNameFn    = const char* (__fastcall*)(void*, void*, unsigned, int);
using GetGuildIdFn = uint32_t (__thiscall*)(void*);
using WorldToTileFn = void (__thiscall*)(void*, float, float, int*, int*,
                                         unsigned*, unsigned*);

template <typename T>
inline T Read(const void* base, int off) {
  return *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + off);
}

// Recopie une `std::string` du client, SSO comprise.
// ⚠ SEH ⇒ AUCUN objet C++ dans cette fonction (C2712).
bool CopyClientString(const void* str, char* out, size_t out_size) {
  __try {
    if (!str) return false;
    const unsigned size = Read<unsigned>(str, kField_Size);
    const unsigned cap  = Read<unsigned>(str, kField_Cap);
    if (size == 0 || size >= out_size) return false;
    const char* src = (cap >= 16) ? Read<const char*>(str, 0)
                                  : reinterpret_cast<const char*>(str);
    if (!src) return false;
    memcpy(out, src, size);
    out[size] = '\0';
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ⚠ PAS `GameMode()` : `bourgeon.h` amène la classe `GameMode` du client, et un
// appel non qualifié devient alors ambigu (C2872) — le compilateur hésite entre
// notre fonction et le constructeur de la classe.
void* ActiveGameMode() {
  __try {
    return rag::ActiveMode();
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool NameDictContains(void* game_mode, uint32_t gid) {
  __try {
    if (!game_mode) return false;
    void* dict = reinterpret_cast<uint8_t*>(game_mode) + kGm_NameDict;
    return reinterpret_cast<ContainsFn>(kNameDictContains)(dict, gid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// 🔴 Peut DEMANDER le nom au serveur (cf. le bloc d'adresses). L'appelant décide
// quand il a le droit de le faire.
void* NameDictEntry(void* game_mode, uint32_t gid) {
  __try {
    if (!game_mode) return nullptr;
    void* dict = reinterpret_cast<uint8_t*>(game_mode) + kGm_NameDict;
    return reinterpret_cast<GetEntryFn>(kNameDictGetEntry)(dict, gid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool NameEntryValid(const void* entry) {
  __try {
    return entry && Read<uint8_t>(entry, kName_Valid) == 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void ReadNameField(const void* entry, int field, char* out, size_t out_size) {
  const void* str = nullptr;
  __try {
    if (!entry) return;
    str = reinterpret_cast<const uint8_t*>(entry) + field;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
  CopyClientString(str, out, out_size);
}

int ReadTitleId(const void* entry) {
  __try {
    return entry ? Read<int>(entry, kName_TitleId) : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void* FindActor(uint32_t gid) {
  __try {
    return reinterpret_cast<FindActorFn>(kActorFindByGid)(gid);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* OwnActor(void* game_mode) {
  __try {
    if (!game_mode) return nullptr;
    void* scene = Read<void*>(game_mode, kGm_ActorMgr);
    if (!scene) return nullptr;
    return Read<void*>(scene, kScene_OwnActor);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

uint32_t ActorGuildId(void* actor) {
  __try {
    if (!actor) return 0;
    void** vtable = *reinterpret_cast<void***>(actor);
    return reinterpret_cast<GetGuildIdFn>(
        vtable[kActorVt_GetGuildId / sizeof(void*)])(actor);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool ActorCell(void* game_mode, void* actor, int* cx, int* cy) {
  __try {
    if (!game_mode || !actor) return false;
    unsigned sub_x = 0, sub_y = 0;
    reinterpret_cast<WorldToTileFn>(kWorldToTile)(
        game_mode, Read<float>(actor, kActor_PosX),
        Read<float>(actor, kActor_PosZ), cx, cy, &sub_x, &sub_y);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Nom lisible d'une classe (joueur comme monstre). Chaîne dans la code-page du
// CLIENT — pas celle du fil : elle vient de ses tables Lua, pas du serveur.
bool JobDisplayName(uint32_t job, char* out, size_t out_size) {
  const char* name = nullptr;
  __try {
    name = reinterpret_cast<JobNameFn>(kJobDisplayName)(
        rag::Session(), nullptr, job, -1);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  if (!name) return false;
  bool ok = false;
  __try {
    if (*name) {
      lstrcpynA(out, name, static_cast<int>(out_size));
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  return ok;
}

const char* PickCategoryLabel(int category) {
  switch (category) {
    case kPickActor:      return i18n::Tr("acteur (joueur, monstre, PNJ scripté)");
    case kPickNpc:        return i18n::Tr("NPC de carte");
    case kPickSkillUnit:  return i18n::Tr("unité de compétence");
    case kPickPet:        return i18n::Tr("pet");
    case kPickSpecial:    return i18n::Tr("homoncule / mercenaire / élémentaire");
    default:              return i18n::Tr("inconnue");
  }
}

// ── Rendu : une ligne « libellé / valeur » ───────────────────────────────────
// L'alignement est en unités de POLICE, pas en pixels : les douze familles
// proposées par le réglage d'interface n'ont pas la même chasse.
void Row(const char* label, const char* value, bool missing = false) {
  ImGui::TextColored(kLabelCol, "%s", label);
  ImGui::SameLine(ImGui::GetFontSize() * 9.5f);
  ImGui::TextColored(missing ? kMissingCol : kValueCol, "%s", value);
}

void RowFmt(const char* label, const char* fmt, ...) {
  char buffer[256];
  va_list args;
  va_start(args, fmt);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, args);
  va_end(args);
  Row(label, buffer);
}

// Une chaîne venue du FIL, ou le tiret qui dit « le serveur n'a rien envoyé ».
void RowWire(const char* label, const char* wire) {
  if (wire == nullptr || *wire == '\0') {
    Row(label, i18n::Tr("—"), /*missing=*/true);
    return;
  }
  const char* utf8 = ro::WireToUtf8(wire);
  Row(label, (utf8 && *utf8) ? utf8 : wire);
}

}  // namespace

// ── Cycle de vie ─────────────────────────────────────────────────────────────

EntityInspector::EntityInspector() {
  // Notre réponse custom. Au-dessus de l'opcode max du client (0x0C35), donc
  // livrée par le reader-hook et non par la table de dispatch.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kEntityProps);
}

void EntityInspector::Open(uint32_t gid, uint32_t job, int category,
                           const char* family) {
  gid_ = gid;
  job_ = job;
  cat_ = category;
  family_ = (family != nullptr) ? family : "";
  // Recibler, c'est repartir de zéro sur le nom : le GID précédent pouvait être
  // résolu et le nouveau pas du tout.
  name_resolved_ = false;
  last_name_request_ = 0;
  // Le volet serveur repart de zéro : les propriétés affichées sont celles d'une
  // AUTRE entité, et rien ne dit que la nouvelle en aura les mêmes.
  server_state_ = Fetch::kIdle;
  server_props_.clear();
  server_gid_ = 0;
  server_requested_tick_ = 0;
  open_ = true;
  need_focus_ = true;
}

void EntityInspector::OnModeSwitch(ModeMgr::ModeType, const char*) {
  // Les GID ne survivent pas à un changement de carte : garder la fenêtre
  // ouverte la ferait pointer, au mieux, sur rien — au pire sur une autre
  // entité qui a hérité du numéro.
  open_ = false;
  gid_ = job_ = 0;
  cat_ = -1;
  name_resolved_ = false;
  server_state_ = Fetch::kIdle;
  server_props_.clear();
  server_gid_ = 0;
}

// ── Le volet serveur ─────────────────────────────────────────────────────────

void EntityInspector::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void EntityInspector::HandlePacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  if (opcode != bopcodes::kEntityProps) return;
  // `data` commence APRÈS [opcode:2][len:2] : c'est la convention de
  // RegisterRecvOpcode (cf. features/plugin.h).
  if (len < 6) return;  // [gid:4][status:1][count:1]
  const uint32_t gid = *reinterpret_cast<const uint32_t*>(data);
  // 🔴 Une réponse qui arrive après un reciblage décrit l'ANCIENNE entité. On la
  // jette : l'afficher sous le nouveau nom serait un mensonge silencieux, et
  // c'est exactement le genre de confusion qu'un inspecteur doit éviter.
  if (gid != gid_) return;

  const uint8_t status = data[4];
  server_gid_ = gid;
  server_props_.clear();
  if (status == 1) { server_state_ = Fetch::kMissing; return; }
  if (status == 2) { server_state_ = Fetch::kDenied;  return; }
  if (status != 0) { server_state_ = Fetch::kMissing; return; }

  // Parseur BORNÉ : toute donnée manquante arrête la liste là où elle est
  // plutôt que de lire hors tampon. Une fiche tronquée reste lisible.
  const uint8_t  count = data[5];
  const uint8_t* p     = data + 6;
  const uint8_t* end   = data + len;
  for (uint8_t i = 0; i < count; ++i) {
    if (p >= end) break;
    const uint8_t klen = *p++;
    if (end - p < klen) break;
    Prop prop;
    if (klen > 0) {
      const std::string raw(reinterpret_cast<const char*>(p), klen);
      const char* utf8 = ro::WireToUtf8(raw.c_str());
      prop.key = (utf8 != nullptr) ? utf8 : raw;
    }
    p += klen;
    if (p >= end) { server_props_.push_back(std::move(prop)); break; }
    const uint8_t vlen = *p++;
    if (end - p < vlen) { server_props_.push_back(std::move(prop)); break; }
    if (vlen > 0) {
      const std::string raw(reinterpret_cast<const char*>(p), vlen);
      const char* utf8 = ro::WireToUtf8(raw.c_str());
      prop.value = (utf8 != nullptr) ? utf8 : raw;
    }
    p += vlen;
    server_props_.push_back(std::move(prop));
  }
  server_state_ = Fetch::kReady;
}

void EntityInspector::RequestServer() {
  if (gid_ == 0) return;
  const unsigned now = GetTickCount();
  // Anti-double-clic, et anti-« je reclique parce que ça ne répond pas ».
  if (server_state_ == Fetch::kPending && (now - server_requested_tick_) < 5000)
    return;

  uint8_t packet[8];  // [op:2][len:2][gid:4]
  *reinterpret_cast<uint16_t*>(packet + 0) = bopcodes::kReqEntityProps;
  *reinterpret_cast<uint16_t*>(packet + 2) = static_cast<uint16_t>(sizeof(packet));
  *reinterpret_cast<uint32_t*>(packet + 4) = gid_;
  Bourgeon::Instance().SendPacket(packet, sizeof(packet));
  server_state_ = Fetch::kPending;
  server_requested_tick_ = now;
}

// ── Relecture ────────────────────────────────────────────────────────────────

void EntityInspector::Refresh(Snapshot* out) {
  void* game_mode = ActiveGameMode();
  if (!game_mode || gid_ == 0) return;

  // ── Plaque de nom ──────────────────────────────────────────────────────────
  out->in_dict = NameDictContains(game_mode, gid_);

  // 🔴 `GetEntryOrRequest` n'est appelée que si elle ne peut RIEN demander (le
  // nom est déjà résolu, elle sort tout de suite) ou si le throttle l'autorise.
  // Sans ce garde, une cible sans nom — une unité de compétence, typiquement —
  // enverrait un CZ_REQNAME à chaque frame tant que la fenêtre est ouverte.
  const unsigned now = GetTickCount();
  const bool may_request =
      name_resolved_ ||
      last_name_request_ == 0 ||
      (now - last_name_request_) >= kNameRetryMs;
  if (!may_request) return;
  if (!name_resolved_) last_name_request_ = now;

  void* entry = NameDictEntry(game_mode, gid_);
  out->resolved = NameEntryValid(entry);
  if (out->resolved) {
    name_resolved_ = true;
    ReadNameField(entry, kName_Str,   out->name,  sizeof(out->name));
    ReadNameField(entry, kName_Party, out->party, sizeof(out->party));
    ReadNameField(entry, kName_Guild, out->guild, sizeof(out->guild));
    ReadNameField(entry, kName_Rank,  out->rank,  sizeof(out->rank));
    ReadNameField(entry, kName_Title, out->title, sizeof(out->title));
    ReadNameField(entry, kName_Extra, out->extra, sizeof(out->extra));
    out->title_id = ReadTitleId(entry);
  } else {
    name_resolved_ = false;
  }
}

void EntityInspector::ReadActor(Snapshot* out) {
  void* actor = FindActor(gid_);
  if (!actor) return;
  out->actor_found = true;
  out->actor_addr  = reinterpret_cast<uint32_t>(actor);
  __try {
    out->actor_vt   = *reinterpret_cast<uint32_t*>(actor);
    out->actor_gid  = Read<uint32_t>(actor, kActor_Gid);
    out->actor_type = Read<uint8_t>(actor, kActor_Type);
    out->pos_x      = Read<float>(actor, kActor_PosX);
    out->pos_y      = Read<float>(actor, kActor_PosY);
    out->pos_z      = Read<float>(actor, kActor_PosZ);
    out->height_off = Read<float>(actor, kActor_HeightOff);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Acteur en cours de destruction : on garde ce qui a été lu et on n'ira pas
    // chercher le reste. `actor_found` et l'adresse restent posés — c'est
    // justement l'information qu'un staff veut dans ce cas.
    return;
  }

  void* game_mode = ActiveGameMode();
  out->guild_id = ActorGuildId(actor);
  out->cell_ok  = ActorCell(game_mode, actor, &out->cell_x, &out->cell_y);
  out->own_cell_ok = ActorCell(game_mode, OwnActor(game_mode), &out->own_cell_x,
                               &out->own_cell_y);
}

// ── Le corps de la fenêtre ───────────────────────────────────────────────────

void EntityInspector::DrawBody(const Snapshot& snap) {
  // ── Le pick : ce que le clic a désigné ────────────────────────────────────
  SeparatorText(i18n::Tr("Pick"));
  RowFmt(i18n::Tr("GID"), "%u  (0x%08X)", gid_, gid_);

  char job_name[128] = {};
  if (JobDisplayName(job_, job_name, sizeof(job_name))) {
    const char* utf8 = ro::LocalToUtf8(job_name);
    char line[192];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%u  (0x%04X)  %s", job_,
                job_, (utf8 && *utf8) ? utf8 : job_name);
    Row(i18n::Tr("Classe"), line);
  } else {
    // Pas de repli inventé : une classe absente de jobName n'a pas de nom, et
    // en fabriquer un ferait croire à une table trouée là où il n'y a rien.
    char line[64];
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                i18n::Tr("%u  (0x%04X)  — hors table jobName"), job_, job_);
    Row(i18n::Tr("Classe"), line);
  }

  RowFmt(i18n::Tr("Catégorie"), "%d  (%s)", cat_, PickCategoryLabel(cat_));
  if (!family_.empty()) Row(i18n::Tr("Famille"), family_.c_str());

  // ── La plaque de nom ──────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Plaque de nom (CNameInfo)"));
  if (!snap.in_dict) {
    ImGui::TextColored(kMissingCol, "%s",
                       i18n::Tr("Le client n'a aucune entrée pour ce GID."));
    ImGui::TextColored(
        kLabelCol, "%s",
        i18n::Tr("(demande envoyée au serveur ; la plaque arrivera, ou pas — "
                 "les unités de compétence et les objets au sol n'en ont "
                 "jamais)"));
  } else if (!snap.resolved) {
    ImGui::TextColored(kMissingCol, "%s",
                       i18n::Tr("Entrée présente mais non renseignée "
                                "(CNameInfo+0x98 = 0) : réponse en attente."));
  } else {
    RowWire(i18n::Tr("Nom"),    snap.name);
    RowWire(i18n::Tr("Groupe"), snap.party);
    RowWire(i18n::Tr("Guilde"), snap.guild);
    RowWire(i18n::Tr("Rang"),   snap.rank);
    RowWire(i18n::Tr("Titre"),  snap.title);
    RowWire(i18n::Tr("Extra"),  snap.extra);
    RowFmt(i18n::Tr("Id de titre"), "%d", snap.title_id);
  }

  // ── L'acteur ──────────────────────────────────────────────────────────────
  SeparatorText(i18n::Tr("Acteur"));
  if (!snap.actor_found) {
    ImGui::TextColored(kMissingCol, "%s",
                       i18n::Tr("Aucun acteur vivant pour ce GID."));
    ImGui::TextColored(
        kLabelCol, "%s",
        i18n::Tr("(entité détruite depuis le clic, ou catégorie de pick sans "
                 "acteur)"));
  } else {
    RowFmt(i18n::Tr("Adresse"), "0x%08X", snap.actor_addr);
    RowFmt(i18n::Tr("vtable"),  "0x%08X", snap.actor_vt);
    // Le GID que porte l'acteur LUI-MÊME : s'il diffère de celui du pick, c'est
    // que la liste d'acteurs a recyclé le numéro entre le clic et maintenant.
    if (snap.actor_gid == gid_) {
      RowFmt(i18n::Tr("GID (+0x110)"), "%u", snap.actor_gid);
    } else {
      char line[96];
      _snprintf_s(line, sizeof(line), _TRUNCATE,
                  i18n::Tr("%u — ne correspond PAS au pick"), snap.actor_gid);
      Row(i18n::Tr("GID (+0x110)"), line, /*missing=*/true);
    }
    // Deux certitudes établies sur ce champ, et deux seulement : {1, 6, 12} =
    // « hostile ou unité spéciale », le test dont le client se sert pour REFUSER
    // son menu joueur (EntityName_IsHostileOrSpecialUnit 0x00d9d220), et 7 =
    // PET, écrit par `ZC_CHANGESTATE_PET` sous-type 0 @0x00cbab7d — c'est lui
    // qui met le pet en catégorie de pick 3. Les autres valeurs attendent leur
    // RE : on montre le nombre, pas une étiquette inventée.
    const bool hostile = (snap.actor_type == 1 || snap.actor_type == 6 ||
                          snap.actor_type == 12);
    const char* type_note = hostile ? i18n::Tr("  (hostile / unité spéciale)")
                          : snap.actor_type == 7 ? i18n::Tr("  (pet)")
                          : "";
    RowFmt(i18n::Tr("Type (+0x314)"), "%u%s",
           static_cast<unsigned>(snap.actor_type), type_note);
    RowFmt(i18n::Tr("Id de guilde"), "%u", snap.guild_id);
    RowFmt(i18n::Tr("Position"), "x %.2f   y %.2f   z %.2f", snap.pos_x,
           snap.pos_y, snap.pos_z);
    if (snap.cell_ok) {
      if (snap.own_cell_ok) {
        // Distance de Chebyshev : c'est celle dont le serveur se sert pour ses
        // portées, donc la seule qui se compare à une portée de compétence.
        const int dx = snap.cell_x - snap.own_cell_x;
        const int dy = snap.cell_y - snap.own_cell_y;
        const int dist = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                             ? (dx < 0 ? -dx : dx)
                             : (dy < 0 ? -dy : dy);
        RowFmt(i18n::Tr("Cellule"), i18n::Tr("%d, %d   (à %d cases de vous)"),
               snap.cell_x, snap.cell_y, dist);
      } else {
        RowFmt(i18n::Tr("Cellule"), "%d, %d", snap.cell_x, snap.cell_y);
      }
    }
    // Non nul = quelqu'un a soulevé le sprite — nous (PlayerJump) ou le moteur.
    if (snap.height_off != 0.0f)
      RowFmt(i18n::Tr("Offset de hauteur"), "%.2f", snap.height_off);
  }

  // ── Ce que seul le serveur sait ───────────────────────────────────────────
  // En DERNIER, et dans le MÊME panneau : c'est un complément de ce qui précède,
  // pas une vue concurrente. Rien n'en part tant qu'on n'a pas cliqué.
  DrawServerSection();
}

void EntityInspector::DrawServerSection() {
  SeparatorText(i18n::Tr("Serveur"));

  switch (server_state_) {
    case Fetch::kIdle:
      // 🔴 Rien n'est demandé tant qu'on ne clique pas. Ouvrir l'inspecteur pour
      // lire un GID ne doit coûter aucun paquet.
      if (ro::RoButton(i18n::Tr("Demander au serveur"))) RequestServer();
      ImGui::SameLine();
      HelpMarker(i18n::Tr(
          "Ce que le CLIENT ne peut pas savoir : le fichier de script d'un NPC, "
          "le spawn d'un monstre, qui a lancé une unité de compétence, à qui "
          "est réservé un objet au sol.\n"
          "Réservé au staff, et c'est le SERVEUR qui le vérifie."));
      return;
    case Fetch::kPending: {
      // 🔴 Une attente sans fin ne dit pas ce qui se passe — et le cas est
      // RÉEL : tant que le map-server n'a pas redémarré avec le handler,
      // CZ 0x0F22 tombe dans le vide et rien ne revient JAMAIS. On le dit au
      // bout de cinq secondes plutôt que de laisser tourner « interrogation… ».
      const unsigned waited = GetTickCount() - server_requested_tick_;
      if (waited < 5000) {
        ImGui::TextColored(kLabelCol, "%s",
                           i18n::Tr("Interrogation du serveur…"));
        return;
      }
      ImGui::TextColored(kMissingCol, "%s",
                         i18n::Tr("Pas de réponse du serveur."));
      ImGui::TextColored(
          kLabelCol, "%s",
          i18n::Tr("(le map-server ne connaît peut-être pas encore ce paquet : "
                   "il attend un redémarrage)"));
      if (ro::RoButton(i18n::Tr("Redemander"))) RequestServer();
      return;
    }
    case Fetch::kDenied:
      ImGui::TextColored(
          kMissingCol, "%s",
          i18n::Tr("Le serveur a refusé : ce compte n'est pas staff."));
      return;
    case Fetch::kMissing:
      ImGui::TextColored(
          kMissingCol, "%s",
          i18n::Tr("Le serveur ne connaît aucune entité sous ce GID."));
      ImGui::TextColored(
          kLabelCol, "%s",
          i18n::Tr("(elle vient de mourir ou de disparaître, ou bien ce GID "
                   "n'existe que côté client)"));
      if (ro::RoButton(i18n::Tr("Redemander"))) RequestServer();
      return;
    case Fetch::kReady:
      break;
  }

  // Les libellés viennent du SERVEUR et s'affichent tels quels : ils ne passent
  // par aucun catalogue. Une valeur vide est un titre de section — c'est la
  // convention du paquet, elle évite d'avoir à le structurer en groupes.
  for (const Prop& prop : server_props_) {
    if (prop.value.empty()) {
      SeparatorText(prop.key.c_str());
    } else {
      Row(prop.key.c_str(), prop.value.c_str());
    }
  }
  if (server_props_.empty()) {
    ImGui::TextColored(kLabelCol, "%s",
                       i18n::Tr("Le serveur n'a rien à dire sur cette entité."));
  }
  if (ro::RoButton(i18n::Tr("Actualiser"))) RequestServer();
}

std::string EntityInspector::BuildReport(const Snapshot& snap) const {
  char job_name[128] = {};
  JobDisplayName(job_, job_name, sizeof(job_name));

  // 🔴 Chaque champ du FIL est converti puis COPIÉ tout de suite : `WireToUtf8`
  // rend un tampon rotatif, et en enchaîner cinq dans un seul appel de
  // formatage recyclerait les premiers avant leur lecture.
  auto wire = [](const char* raw) {
    if (raw == nullptr || *raw == '\0') return std::string();
    const char* utf8 = ro::WireToUtf8(raw);
    return std::string((utf8 != nullptr) ? utf8 : raw);
  };
  const std::string name  = wire(snap.name);
  const std::string party = wire(snap.party);
  const std::string guild = wire(snap.guild);
  const std::string rank  = wire(snap.rank);
  const std::string title = wire(snap.title);

  char buffer[2048];
  int n = _snprintf_s(
      buffer, sizeof(buffer), _TRUNCATE,
      "GID %u (0x%08X) | classe %u (0x%04X) %s | pick cat %d | famille %s\n"
      "nom \"%s\" groupe \"%s\" guilde \"%s\" rang \"%s\" titre \"%s\" "
      "titleId %d | dict %s resolu %s\n",
      gid_, gid_, job_, job_, job_name, cat_,
      family_.empty() ? "?" : family_.c_str(), name.c_str(), party.c_str(),
      guild.c_str(), rank.c_str(), title.c_str(), snap.title_id,
      snap.in_dict ? "oui" : "non", snap.resolved ? "oui" : "non");
  if (n < 0) return std::string();

  if (snap.actor_found) {
    _snprintf_s(buffer + n, sizeof(buffer) - n, _TRUNCATE,
                "acteur 0x%08X vtable 0x%08X gid+0x110 %u type+0x314 %u "
                "guilde %u | pos %.2f %.2f %.2f | cellule %d,%d | "
                "offset hauteur %.2f\n",
                snap.actor_addr, snap.actor_vt, snap.actor_gid,
                static_cast<unsigned>(snap.actor_type), snap.guild_id,
                snap.pos_x, snap.pos_y, snap.pos_z, snap.cell_x, snap.cell_y,
                snap.height_off);
  } else {
    _snprintf_s(buffer + n, sizeof(buffer) - n, _TRUNCATE,
                "acteur : aucun\n");
  }

  std::string report(buffer);
  // Le volet serveur n'est recopié que s'il a été demandé ET répondu : un
  // rapport qui dirait « serveur : rien » alors qu'on n'a simplement pas cliqué
  // ferait croire à une entité sans propriétés serveur.
  if (server_state_ == Fetch::kReady && server_gid_ == gid_) {
    report += "-- serveur --\n";
    for (const Prop& prop : server_props_) {
      report += prop.key;
      if (prop.value.empty()) {
        report += " :\n";  // titre de section
      } else {
        report += " = " + prop.value + "\n";
      }
    }
  }
  return report;
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void EntityInspector::OnRenderUI() {
  if (!open_) return;
  // 🔴 Revérifié CHAQUE frame, pas seulement à l'ouverture : un compte qui perd
  // le staff en cours de session ne doit pas garder sous les yeux une fenêtre
  // d'adresses mémoire.
  if (!IsStaff()) { open_ = false; return; }

  Snapshot snap;
  Refresh(&snap);
  ReadActor(&snap);

  if (need_focus_) {
    ImGui::SetNextWindowFocus();
    need_focus_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(440.0f, 480.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(360.0f, 200.0f),
                                      ImVec2(FLT_MAX, FLT_MAX));

  // Titre = le NOM de la cible quand on l'a, sinon son GID.
  // 🔴 Le « ### » vit dans le GABARIT, hors de `Tr` : il fige l'identité ImGui
  // (donc la position et la taille dans imgui.ini) d'une langue à l'autre, et un
  // catalogue qui l'aurait perdu en traduisant ferait renaître la fenêtre
  // ailleurs.
  char title[160];
  const char* who = (snap.resolved && snap.name[0] != '\0')
                        ? ro::WireToUtf8(snap.name)
                        : nullptr;
  if (who != nullptr && *who != '\0') {
    _snprintf_s(title, sizeof(title), _TRUNCATE,
                "%s — %s###bourgeon_entity_inspector",
                i18n::Tr("Propriétés"), who);
  } else {
    char gid_text[32];
    _snprintf_s(gid_text, sizeof(gid_text), _TRUNCATE, "GID %u", gid_);
    _snprintf_s(title, sizeof(title), _TRUNCATE,
                "%s — %s###bourgeon_entity_inspector",
                i18n::Tr("Propriétés"), gid_text);
  }

  const bool begun = ro::BeginRoWindow(title, &open_);
  if (begun) {
    DrawBody(snap);
    ImGui::Separator();
    if (ro::RoButton(i18n::Tr("Copier le rapport"))) {
      const std::string report = BuildReport(snap);
      ImGui::SetClipboardText(report.c_str());
    }
    ImGui::SameLine();
    HelpMarker(i18n::Tr(
        "Tout ce que le CLIENT sait de cette entité, relu à chaque image. Les "
        "champs de la plaque de nom viennent du serveur : vides, ils veulent "
        "dire « pas encore reçu », pas « vides côté serveur ».\n"
        "Les adresses et la vtable se collent telles quelles dans IDA ou "
        "x32dbg."));
  }
  // ⚠ EndRoWindow s'appelle TOUJOURS, même repliée ou masquée (règle Begin/End).
  ro::EndRoWindow();
}
