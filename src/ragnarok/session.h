#pragma once

#include <atomic>
#include <list>
#include <memory>

#include "ragnarok/item_info.h"
#include "ragnarok/talktype.h"
#include "utils/hooking/proxy.h"
#include "yaml-cpp/yaml.h"

class Session {
 public:
  using Pointer = std::unique_ptr<Session>;

  Session(const YAML::Node &session_configuration);
  virtual ~Session() = default;

  virtual uint32_t aid() const = 0;
  virtual int max_hp() const = 0;
  virtual int hp() const = 0;
  virtual int max_sp() const = 0;
  virtual int sp() const = 0;
  virtual const char *char_name() const = 0;

  std::string GetCharName() const;
  // 🔴 CES DEUX-LÀ FONT PLANTER LE CLIENT 20250716 — NE PAS APPELER.
  // Elles parcourent `item_list()`, dont l'offset est marqué « LIKELY » dans
  // object_layouts/session/20250716.h (+0x16D8) et est FAUX : la tête de liste
  // lue vaut 0, d'où un `mov eax,[esi]` avec esi = 0. Le défaut est resté
  // invisible des années parce que toute la chaîne était morte (GetItemInfoById
  // n'était appelée que par RagnarokClient::UseItemById, elle-même sans aucun
  // appelant) ; le premier usage réel, en juillet 2026, a planté immédiatement.
  // Pour parcourir l'inventaire, utiliser le GLOBAL 0x015FBAB0 (tête de la liste
  // circulaire), comme le fait features/windows/make_item_window.cc — ce chemin,
  // lui, est vérifié en jeu. À rétablir seulement quand l'offset aura été
  // CONFIRMÉ (et non « déduit d'un motif d'xref »).
  bool GetItemInfoById(int id, ItemInfo &item_info) const;
  std::string GetItemNameById(int id) const;

  // Hooks
  void SessionHook();
  int GetTalkTypeHook(char const *chat_buffer, TalkType *talk_type,
                      void *param);

 protected:
  virtual const std::list<ItemInfo> &item_list() const = 0;

  static MethodRef<Session, void (Session::*)()> SessionRef;
  static MethodRef<Session,
                   int (Session::*)(const char *chatBuf,
                                    enum TalkType *talkType, void *param)>
      GetTalkTypeRef;

  static std::atomic<Session *> g_session_ptr;
};
