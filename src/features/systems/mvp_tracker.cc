#include "features/systems/mvp_tracker.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include "bourgeon.h"
#include "features/systems/bourgeon_opcodes.h"
#include "ui/ro_imgui.h"       // ro::Utf8ToWire (les noms partent dans l'encodage du client)
#include "utils/log_console.h"

namespace {

// Tailles des entrées, miroir EXACT de clif.cpp côté moonlight. Une seule des
// deux qui bouge, et tous les champs suivants glissent sans que rien ne le dise.
constexpr int kCatalogEntryLen = 2 + 2 + 1 + 4 + 4 + 2 + 2 + 16 + 24;  // 57
// Le NOM au bout d'une observation, c'est QUI l'affirme. Le serveur le tenait
// depuis le début et ne l'envoyait à personne : la colonne Source disait
// « tué » sans jamais dire par qui.
constexpr int kObsEntryLen     = 2 + 1 + 2 + 8 + 8 + 2 + 2 + 4 + 8 + 24;  // 61
constexpr int kFavEntryLen     = 2;
constexpr int kMemberEntryLen  = 4 + 2 + 1 + 24;  // 31

// `RegisterRecvOpcode` livre les octets APRÈS [opcode:2][len:2] : l'offset 0
// est donc `kind`, pas l'en-tête. (cf. features/plugin.h)
constexpr int kStateBodyHeader = 1 + 8 + 2;   // kind, server_time, count
constexpr int kGroupBodyHeader = 1 + 1 + 4 + 4 + 32 + 1;

template <typename T>
T Read(const uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

void CopyFixed(char* dst, size_t dst_size, const uint8_t* src, size_t src_size) {
  const size_t n = src_size < dst_size - 1 ? src_size : dst_size - 1;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace

MvpTracker::MvpTracker() {
  auto& b = Bourgeon::Instance();
  b.RegisterRecvOpcode(bopcodes::kMvpState);
  b.RegisterRecvOpcode(bopcodes::kMvpGroup);
}

// Fil RÉSEAU : on ne fait que copier. Le décodage est dans HandlePacket.
void MvpTracker::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

void MvpTracker::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode == bopcodes::kMvpState) {
    HandleState(data, len);
  } else if (opcode == bopcodes::kMvpGroup) {
    HandleGroup(data, len);
  }
}

void MvpTracker::OnModeSwitch(ModeMgr::ModeType mode_type, const char* /*map_name*/) {
  // Changer de personnage change de compte de jeu, donc potentiellement de
  // compte Moonlight : tout ce qui vient du serveur redevient inconnu. Le
  // catalogue aussi — un @reloadscript entre-temps l'aurait renuméroté.
  if (mode_type == ModeMgr::ModeType::kGame) return;
  slots_.clear();
  obs_.clear();
  favorites_.clear();
  group_ = mvp::Group{};
  catalog_known_ = false;
  clock_known_ = false;
  invite_id_ = 0;
  invite_name_[0] = '\0';
}

void MvpTracker::NoteServerTime(int64_t server_time) {
  if (server_time <= 0) return;
  clock_offset_ = server_time - static_cast<int64_t>(std::time(nullptr));
  clock_known_ = true;
}

int64_t MvpTracker::ServerNow() const {
  return static_cast<int64_t>(std::time(nullptr)) + clock_offset_;
}

void MvpTracker::HandleState(const uint8_t* data, uint16_t len) {
  if (len < kStateBodyHeader) return;

  const uint8_t kind = data[0];
  NoteServerTime(Read<int64_t>(data + 1));
  const uint16_t count = Read<uint16_t>(data + 9);
  const uint8_t* body = data + kStateBodyHeader;
  const int avail = static_cast<int>(len) - kStateBodyHeader;

  switch (kind) {
    case 0: {  // CATALOGUE
      if (avail < static_cast<int>(count) * kCatalogEntryLen) return;
      slots_.clear();
      slots_.reserve(count);
      for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* e = body + i * kCatalogEntryLen;
        mvp::Slot slot;
        slot.slot_id   = Read<uint16_t>(e + 0);
        slot.mob_id    = Read<uint16_t>(e + 2);
        slot.kind      = static_cast<mvp::SlotKind>(e[4]);
        slot.delay1_ms = Read<uint32_t>(e + 5);
        slot.delay2_ms = Read<uint32_t>(e + 9);
        slot.map_xs    = Read<uint16_t>(e + 13);
        slot.map_ys    = Read<uint16_t>(e + 15);
        CopyFixed(slot.map, sizeof(slot.map), e + 17, 16);
        CopyFixed(slot.name, sizeof(slot.name), e + 33, 24);
        slots_.push_back(slot);
      }
      catalog_known_ = true;
      LogInfo("[MvpTracker] catalogue : {} creneaux", slots_.size());
      break;
    }

    case 1:    // INSTANTANÉ — remplace tout
    case 2: {  // DELTA — complète
      if (avail < static_cast<int>(count) * kObsEntryLen) return;
      if (kind == 1) obs_.clear();
      for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* e = body + i * kObsEntryLen;
        const uint16_t slot_id = Read<uint16_t>(e + 0);
        mvp::Obs obs;
        obs.source        = static_cast<mvp::Source>(e[2]);
        obs.mob_id        = Read<uint16_t>(e + 3);
        obs.kill_time     = Read<int64_t>(e + 5);
        obs.exact_respawn = Read<int64_t>(e + 13);
        obs.tomb_x        = Read<int16_t>(e + 21);
        obs.tomb_y        = Read<int16_t>(e + 23);
        obs.by_user_id    = Read<uint32_t>(e + 25);
        obs.reported_at   = Read<int64_t>(e + 29);
        // NUL-paddé par le serveur, mais on borne quand même : le tampon vient
        // du fil, et un nom sans terminaison déborderait à la première lecture.
        std::memcpy(obs.by_name, e + 37, sizeof(obs.by_name) - 1);
        obs.by_name[sizeof(obs.by_name) - 1] = '\0';
        // L'arbitrage a déjà eu lieu côté serveur : ce qui arrive fait autorité.
        obs_[slot_id] = obs;
      }
      break;
    }

    case 3: {  // FAVORIS
      if (avail < static_cast<int>(count) * kFavEntryLen) return;
      favorites_.clear();
      favorites_.reserve(count);
      for (uint16_t i = 0; i < count; ++i)
        favorites_.push_back(Read<uint16_t>(body + i * kFavEntryLen));
      break;
    }

    default:
      break;
  }
}

void MvpTracker::HandleGroup(const uint8_t* data, uint16_t len) {
  if (len < kGroupBodyHeader) return;

  const uint8_t kind   = data[0];
  const uint8_t result = data[1];

  if (kind == 2) {  // RÉSULTAT
    last_result_ = result;
    last_result_ms_ = GetTickCount();
    return;
  }

  const uint32_t group_id = Read<uint32_t>(data + 2);

  if (kind == 1) {  // INVITATION en attente
    invite_id_ = group_id;
    CopyFixed(invite_name_, sizeof(invite_name_), data + 10, 32);
    return;
  }

  // kind 0 : LE GROUPE. group_id 0 veut dire « dans aucun groupe » — c'est une
  // réponse, pas un silence : on vide, sinon le panneau garderait l'ancien.
  group_ = mvp::Group{};
  group_.group_id      = group_id;
  group_.owner_user_id = Read<uint32_t>(data + 6);
  CopyFixed(group_.name, sizeof(group_.name), data + 10, 32);

  const uint8_t count = data[42];
  const uint8_t* body = data + kGroupBodyHeader;
  if (static_cast<int>(len) - kGroupBodyHeader < static_cast<int>(count) * kMemberEntryLen)
    return;

  group_.members.reserve(count);
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t* e = body + i * kMemberEntryLen;
    mvp::Member member;
    member.user_id = Read<uint32_t>(e + 0);
    member.level   = Read<int16_t>(e + 4);
    member.online  = e[6] != 0;
    CopyFixed(member.name, sizeof(member.name), e + 7, 24);
    group_.members.push_back(member);
  }

  // Sortir d'un groupe rend caduc tout ce qu'il savait.
  if (group_.group_id == 0) obs_.clear();
}

const mvp::Slot* MvpTracker::FindSlot(uint16_t slot_id) const {
  for (const mvp::Slot& slot : slots_)
    if (slot.slot_id == slot_id) return &slot;
  return nullptr;
}

const mvp::Obs* MvpTracker::FindObs(uint16_t slot_id) const {
  auto it = obs_.find(slot_id);
  return it != obs_.end() ? &it->second : nullptr;
}

bool MvpTracker::IsFavorite(uint16_t slot_id) const {
  for (uint16_t id : favorites_)
    if (id == slot_id) return true;
  return false;
}

bool MvpTracker::Window(uint16_t slot_id, int64_t* from, int64_t* to, bool* exact) const {
  const mvp::Obs* obs = FindObs(slot_id);
  if (obs == nullptr) return false;

  if (obs->exact_respawn != 0) {
    // Mérité : un instant, pas une plage.
    if (from)  *from = obs->exact_respawn;
    if (to)    *to   = obs->exact_respawn;
    if (exact) *exact = true;
    return true;
  }

  const mvp::Slot* slot = FindSlot(slot_id);
  if (slot == nullptr || obs->kill_time == 0) return false;

  // La LOI, jamais le tirage.
  const int64_t start = obs->kill_time + slot->delay1_ms / 1000;
  if (from)  *from = start;
  if (to)    *to   = start + slot->delay2_ms / 1000;
  if (exact) *exact = false;
  return true;
}

// ── Commandes ────────────────────────────────────────────────────────────────

void MvpTracker::Send(uint8_t cmd, uint32_t a, uint32_t b, const char* text_utf8) {
  // Le texte part dans l'ENCODAGE DU CLIENT : le serveur le compare à des noms
  // stockés tels quels (table `char`). La conversion appartient au producteur.
  const char* wire = (text_utf8 != nullptr && *text_utf8 != '\0')
                         ? ro::Utf8ToWire(text_utf8)
                         : "";
  const size_t text_len = std::strlen(wire);
  const size_t capped = text_len > 64 ? 64 : text_len;
  const uint16_t len = static_cast<uint16_t>(13 + capped);

  uint8_t packet[13 + 64];
  *reinterpret_cast<uint16_t*>(packet + 0) = bopcodes::kMvpCmd;
  *reinterpret_cast<uint16_t*>(packet + 2) = len;
  packet[4] = cmd;
  *reinterpret_cast<uint32_t*>(packet + 5) = a;
  *reinterpret_cast<uint32_t*>(packet + 9) = b;
  if (capped > 0) std::memcpy(packet + 13, wire, capped);

  Bourgeon::Instance().SendPacket(packet, len);
}

void MvpTracker::RequestSnapshot()                       { Send(1, 0, 0, nullptr); }
void MvpTracker::CreateGroup(const char* name)           { Send(2, 0, 0, name); }
void MvpTracker::DissolveGroup()                         { Send(3, 0, 0, nullptr); }
void MvpTracker::InviteMember(const char* char_name)     { Send(4, 0, 0, char_name); }
void MvpTracker::AcceptInvite()                          { clear_pending_invite(); Send(5, 0, 0, nullptr); }
void MvpTracker::DeclineInvite()                         { clear_pending_invite(); Send(6, 0, 0, nullptr); }
void MvpTracker::LeaveGroup()                            { Send(7, 0, 0, nullptr); }
void MvpTracker::KickMember(const char* char_name)       { Send(8, 0, 0, char_name); }

void MvpTracker::SetFavorite(uint16_t slot_id, bool on) {
  Send(9, slot_id, on ? 1u : 0u, nullptr);
}

// 🔴 La tombe voyage dans le champ TEXTE de la commande, pas dans un paquet
// neuf. `a` porte déjà le créneau et `b` l'heure de mort ; le texte, lui, ne
// sert à rien pour cette commande-là (il porte un nom de groupe ou de
// personnage pour les autres). Y écrire « x,y » évite un troisième opcode pour
// deux entiers, et le serveur sait déjà lire ce champ.
//
// Vide = pas de tombe, ce que le serveur traite comme l'ancien comportement.
void MvpTracker::ReportManual(uint16_t slot_id, int64_t kill_time,
                              int16_t tomb_x, int16_t tomb_y,
                              const char* shared_by_utf8) {
  // « x,y|Pseudo », les deux moitiés facultatives. Le nom vient EN DERNIER et
  // le serveur ne l'analyse pas : un pseudo peut porter n'importe quoi sauf la
  // barre qui le précède.
  //
  // 🔴🔴 LE PSEUDO RESTE EN UTF-8 ICI. `Send` convertit DÉJÀ tout son champ
  // texte vers l'encodage du fil — le convertir une seconde fois relisait des
  // octets CP949 comme de l'UTF-8 et détruisait le nom. Le piège est invisible
  // tant que tous les pseudos sont ASCII, qui traverse les deux conversions
  // sans bouger : c'est exactement le motif « la conversion appartient au
  // PRODUCTEUR, et à lui seul ».
  //
  // D'où la taille : un pseudo de 23 caractères peut peser jusqu'à ~69 octets
  // en UTF-8 avant que `Send` ne le ramène à la code-page.
  char extra[96] = {};
  char* p = extra;
  size_t left = sizeof(extra);

  if (tomb_x >= 0 && tomb_y >= 0) {
    const int n = std::snprintf(p, left, "%d,%d", static_cast<int>(tomb_x),
                                static_cast<int>(tomb_y));
    if (n > 0 && static_cast<size_t>(n) < left) { p += n; left -= n; }
  }
  if (shared_by_utf8 != nullptr && shared_by_utf8[0] != '\0')
    std::snprintf(p, left, "|%s", shared_by_utf8);

  Send(10, slot_id, static_cast<uint32_t>(kill_time),
       extra[0] != '\0' ? extra : nullptr);
}

const mvp::Slot* MvpTracker::FindSlotFor(uint16_t mob_id, const char* map) const {
  if (map == nullptr || map[0] == '\0') return nullptr;
  for (const mvp::Slot& slot : slots_) {
    if (slot.mob_id != mob_id) continue;
    if (_stricmp(slot.map, map) == 0) return &slot;
  }
  return nullptr;
}
