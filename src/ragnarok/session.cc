#include "ragnarok/session.h"


#include "bourgeon.h"
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"

// Pointer to the game's Session singleton instance
std::atomic<Session*> Session::g_session_ptr(nullptr);

Session::Session(const YAML::Node& session_configuration) {
  using namespace hooking;

  // Hooks
  const auto session_addr = session_configuration["CSession"];
  if (!session_addr.IsDefined()) {
    throw std::exception("Missing required field 'CSession' for Session");
  }
  Session::SessionRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(session_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&Session::SessionHook)));

  const auto getalktype_addr = session_configuration["GetTalkType"];
  if (!getalktype_addr.IsDefined()) {
    throw std::exception("Missing required field 'GetTalkType' for Session");
  }
  Session::GetTalkTypeRef = HookManager::Instance().SetHook(
      HookType::kJmpHook,
      reinterpret_cast<uint8_t*>(getalktype_addr.as<uint32_t>()),
      reinterpret_cast<uint8_t*>(void_cast(&Session::GetTalkTypeHook)));
}

std::string Session::GetCharName() const {
  const char* raw = char_name();
  if (!raw || raw[0] == '\0') return "";
  // 20250716 client stores names as plain ANSI — return raw bytes directly.
  return std::string(raw);
}

bool Session::GetItemInfoById(int nameid, ItemInfo& item_info) const {
  const auto& ilist = item_list();

  for (const auto& iinfo : ilist) {
    if (atoi(iinfo.item_name_.c_str()) == nameid) {
      item_info = iinfo;
      return true;
    }
  }

  return false;
}

std::string Session::GetItemNameById(int id) const {
  ItemInfo iinfo;

  if (!GetItemInfoById(id, iinfo)) {
    return "Unknown item";
  }

  return iinfo.item_name_;
}

void Session::SessionHook() {
  // LogInfo("Session ctor this=0x{:x}", reinterpret_cast<uintptr_t>(this));
  g_session_ptr.store(this);
  SessionRef(this);
}

int Session::GetTalkTypeHook(char const* chat_buffer, TalkType* talk_type,
                             void* param) {
  Bourgeon::Instance().FireTalkType(chat_buffer);

  return GetTalkTypeRef(this, chat_buffer, talk_type, param);
}

// References
MethodRef<Session, void (Session::*)()> Session::SessionRef;
MethodRef<Session, int (Session::*)(const char* chatBuf,
                                    enum TalkType* talkType, void* param)>
    Session::GetTalkTypeRef;
