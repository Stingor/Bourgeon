#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "features/windows/npc_dialog_window.h"

// Icônes d'item : ro::ItemIcon (ui/icon_cache.h). Le chargement, le colorkey
// magenta et l'invalidation au reset de device y sont partagés — ce fichier en
// gardait sa propre copie, comme cinq autres plugins.
#include "ui/icon_cache.h"
#include "ragnarok/uiwnd.h"
#include <Windows.h>
#include <shellapi.h>  // ShellExecuteA (clic <URL> -> navigateur)

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "features/systems/bug_report.h"  // BugReport::NpcContext
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / Overlay_DeviceEpoch (icônes)
#include "imgui.h"
#include "ui/ro_imgui.h"     // ro::BeginRoDescWindow (skin desc RO)
#include "ui/ro_widgets.h"   // ro::HelpMarker (section de réglages)

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000 ; cf. docs/npc_dialog_re.md) ──
namespace {

// UIWindowMgr + factory (SEH-gardé, comme npc_shop_window).

// Dispatcher CMode::SendMsg : *(rag::kActiveModePtr) = mode zone actif (ou 0). vtbl+0x18
// (= index 6) = CMode::SendMsg. cmd 0x28 = ferme l'UI NPC et DÉBLOQUE l'état
// dialogue CLIENT (CZ_CLOSE_DIALOG seul laisse le perso bloqué).
constexpr int kGameModeDialogFlag  = 0x24c;   // CGameMode+0x24C = dialogue actif
constexpr int kSelClose            = 0x28;
using Dispatch_t = int (__thiscall*)(void*, int, int, int, int, int);

// Fenêtres de dialogue NPC (à cacher quand l'overlay est actif).
constexpr int kWinSay   = 0x10;
constexpr int kWinMenu  = 0x11;
constexpr int kWinEditN = 0x38;
constexpr int kWinEditS = 0x64;
constexpr int kWinSay2  = 0xe2;

// Opcodes reçus (ZC).
constexpr uint16_t kZcSay        = 0x00b4;
constexpr uint16_t kZcWait       = 0x00b5;
constexpr uint16_t kZcClose      = 0x00b6;
constexpr uint16_t kZcMenu       = 0x00b7;
constexpr uint16_t kZcEditN      = 0x0142;
constexpr uint16_t kZcEditS      = 0x01d4;
constexpr uint16_t kZcClear      = 0x08d6;
constexpr uint16_t kZcSay2       = 0x0972;
constexpr uint16_t kZcWait2      = 0x0973;
constexpr uint16_t kZcNpcName    = 0x0adf;  // ZC_ACK_REQNAMEALL_NPC (titre)
constexpr uint16_t kZcMapChange  = 0x0091;  // ZC_NPCACK_MAPMOVE (warp)
constexpr uint16_t kZcServerMove = 0x0092;

// Opcodes envoyés (CZ) — cf. docs/npc_dialog_re.md §8 (opcodes vérifiés en live).
constexpr uint16_t kCzNext        = 0x00b9;  // CZ_REQ_NEXT_SCRIPT  {op,GID} 6o
constexpr uint16_t kCzChoose      = 0x00b8;  // CZ_CHOOSE_MENU      {op,GID,choix} 7o
constexpr uint16_t kCzInputN      = 0x0143;  // CZ_INPUT_EDITDLG    {op,GID,int32} 10o
constexpr uint16_t kCzInputS      = 0x01d5;  // CZ_INPUT_EDITDLGSTR {op,len,GID,texte} VAR
constexpr uint16_t kCzCloseDialog = 0x0146;  // CZ_CLOSE_DIALOG     {op,GID} 6o

// Couleur des liens (bleu, comme le natif — il force cette couleur quel que soit le
// ^RRGGBB du contexte), en RGBA ImGui.
constexpr uint32_t kLinkColor = IM_COL32(0x2E, 0x74, 0xD8, 0xFF);

// ── Fenêtres natives (SEH-gardé) ──
void* FindWnd(int id) { return uiwnd::SafeFindWindow(id); }
void CloseWnd(int id) { uiwnd::SafeCloseWindow(id); }
void HideWnd(void* w) { uiwnd::SafeSetVisible(w, false); }
void ShowWnd(void* w) { uiwnd::SafeSetVisible(w, true); }  // dé-cache

// ── Dispatcher CMode + flag dialogue (SEH-gardé) ──
void DispatchNpcCmd(int cmd) {  // CMode::SendMsg(mode, cmd, 0,0,0,0) via vtbl+0x18
  __try {
    void* disp = *reinterpret_cast<void**>(rag::kActiveModePtr);
    if (disp) {
      void** vtbl = *reinterpret_cast<void***>(disp);
      reinterpret_cast<Dispatch_t>(vtbl[6])(disp, cmd, 0, 0, 0, 0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void ForceClearDialogFlag() {  // GameMode+0x24C = 0 (débloque le client, garantie)
  __try {
    void* mode = *reinterpret_cast<void**>(rag::kActiveModePtr);
    if (mode)
      *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(mode) + kGameModeDialogFlag) = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Ouvre une URL http(s) dans le navigateur (comme le clic <URL> natif cmd 0x1B5 ->
// ShellExecute). Restreint à http/https (contenu piloté serveur) par sécurité.
void OpenUrl(const std::string& url) {
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return;
  __try {
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Ouverture de la fenêtre de description (id 0xc) par id d'item ──
// Reproduit le clic lien natif FUN_00803e10 : construit un ItemSkillInfo minimal
// (info[0]=id ; nom laissé vide) et l'envoie à MakeWindow(0xc)->OnMsg(0x18). La
// fenêtre complète le reste depuis la DB client — donc marche pour un item NON possédé.
constexpr int kInfoFlag    = 0x5c;  // ItemSkillInfo+0x5c=1 : desc « standalone » lue depuis la DB
using ItemInfoCtor_t  = void*(__fastcall*)(void*);
using ItemInfoSetId_t = void(__thiscall*)(void*, int);
using DescOnMsg_t     = int(__fastcall*)(void*, void*, int, int, int, int, int, int);

// ── Parsing helpers ──
inline bool IsHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool IsHex6(const char* p) {
  for (int i = 0; i < 6; ++i)
    if (!IsHex(p[i])) return false;
  return true;
}
std::string UpperAscii(const std::string& s) {
  std::string r(s);
  for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return r;
}
std::string LowerAscii(const char* s) {
  std::string r(s ? s : "");
  for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return r;
}

// Le serveur encode les scripts NPC en ANSI (CP_ACP = CP1252 sur Windows fr) ; ImGui
// veut de l'UTF-8 -> conversion (sinon les accents é/à/œ… cassent).
std::string AnsiToUtf8(const std::string& in) {
  if (in.empty()) return std::string();
  const int wn = MultiByteToWideChar(CP_ACP, 0, in.c_str(),
                                     static_cast<int>(in.size()), nullptr, 0);
  if (wn <= 0) return in;
  std::wstring w(static_cast<size_t>(wn), L'\0');
  MultiByteToWideChar(CP_ACP, 0, in.c_str(), static_cast<int>(in.size()), &w[0], wn);
  const int un = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, nullptr, 0, nullptr,
                                     nullptr);
  if (un <= 0) return in;
  std::string out(static_cast<size_t>(un), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), wn, &out[0], un, nullptr, nullptr);
  return out;
}

// ── Icônes item ImGui (recette inventory_viewer.cc) ──
// BuildItemIconGrfPath(id_str, out[128], identified) __stdcall -> chemin bmp ; TexMgr
// charge le bmp ; colorkey magenta -> alpha ; texture ImGui cachée (jetée au reset device).


inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

NpcDialogWindow::NpcDialogWindow() {
  // TOUT en OBSERVE (jamais RegisterRecvOpcode) : le handler natif doit TOUJOURS
  // tourner (fenêtres créées puis cachées) ; sinon le dialogue natif est cassé
  // quand le toggle est OFF. VAR : on forwarde de quoi lire [len:2][GID:4] et on
  // lit le corps (texte/menu) dans le buffer recv live (`data` pointe dedans).
  auto& b = Bourgeon::Instance();
  b.RegisterObserveOpcode(kZcSay,   6);   // {len:2, GID:4, texte...}
  b.RegisterObserveOpcode(kZcSay2,  7);   // {len:2, GID:4, resv:1, texte...}
  b.RegisterObserveOpcode(kZcWait,  4);   // {GID:4}
  b.RegisterObserveOpcode(kZcWait2, 4);   // {GID:4}
  b.RegisterObserveOpcode(kZcClose, 4);   // {GID:4}
  b.RegisterObserveOpcode(kZcMenu,  6);   // {len:2, GID:4, "a:b:c"...}
  b.RegisterObserveOpcode(kZcEditN, 4);   // {GID:4} -> prompt nombre
  b.RegisterObserveOpcode(kZcEditS, 4);   // {GID:4} -> prompt texte
  b.RegisterObserveOpcode(kZcClear, 4);   // efface le texte
  b.RegisterObserveOpcode(kZcNpcName, 32);// gid:4 + groupId:4 + name[24]
  b.RegisterObserveOpcode(kZcMapChange, 4);
  b.RegisterObserveOpcode(kZcServerMove, 4);
}

void NpcDialogWindow::Reset() {
  lines_.clear();
  choices_.clear();
  has_next_ = has_close_ = false;
  start_fresh_ = true;
  input_mode_ = kInputNone;
  gid_ = 0;
  num_buf_[0] = str_buf_[0] = menu_filter_[0] = '\0';
  menu_hot_ = -1;
}

void NpcDialogWindow::PushText(const char* s) {
  if (!s) return;
  // Découpe sur les '\n' bruts (le natif fait pareil via preprocess_escapes).
  std::string cur;
  for (const char* p = s; *p; ++p) {
    if (*p == '\r') continue;
    if (*p == '\n') { lines_.push_back(cur); cur.clear(); }
    else cur += *p;
  }
  lines_.push_back(cur);
  // Borne l'historique de la conversation courante.
  constexpr size_t kMaxLines = 400;
  if (lines_.size() > kMaxLines)
    lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
}

void NpcDialogWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  if (!imgui_enabled_) return;

  if (opcode == kZcMapChange || opcode == kZcServerMove) {
    map_changed_ = true;  // fermé au prochain OnTick (thread principal)
    return;
  }

  if (opcode == kZcNpcName) {
    if (len < 32) return;
    const uint32_t gid = *reinterpret_cast<const uint32_t*>(data);
    char nm[25] = {0};
    std::memcpy(nm, data + 8, 24);
    nm[24] = '\0';
    if (char* h = std::strchr(nm, '#')) *h = '\0';  // tronque la partie cachée
    if (nm[0]) npc_names_[gid] = nm;
    return;
  }

  // NB : OnRecvPacket est appelé pour TOUS les opcodes observés par N'IMPORTE quel
  // plugin (ex. 0x8C8 ZC_NOTIFY_ACT du DPS meter). On ne traite/logge QUE les nôtres
  // (le switch ignore le reste via default: return).
  switch (opcode) {
    case kZcSay: {  // {len:2, GID:4, texte(len-8)}
      if (len < 6) return;
      const uint16_t plen = *reinterpret_cast<const uint16_t*>(data);
      gid_ = *reinterpret_cast<const uint32_t*>(data + 2);
      const int textlen = static_cast<int>(plen) - 8;
      if (start_fresh_) { lines_.clear(); start_fresh_ = false; }  // nouvelle conversation
      if (textlen > 0) {
        std::string t(reinterpret_cast<const char*>(data + 6),
                      static_cast<size_t>(textlen));
        PushText(t.c_str());
      }
      has_next_ = has_close_ = false;  // (re)posés par le WAIT/CLOSE qui suit
      open_ = true;
      return;
    }
    case kZcSay2: {  // {len:2, GID:4, resv:1, texte(len-9)}
      if (len < 7) return;
      const uint16_t plen = *reinterpret_cast<const uint16_t*>(data);
      gid_ = *reinterpret_cast<const uint32_t*>(data + 2);
      const int textlen = static_cast<int>(plen) - 9;
      if (start_fresh_) { lines_.clear(); start_fresh_ = false; }  // nouvelle conversation
      if (textlen > 0) {
        std::string t(reinterpret_cast<const char*>(data + 7),
                      static_cast<size_t>(textlen));
        PushText(t.c_str());
      }
      has_next_ = has_close_ = false;
      open_ = true;
      return;
    }
    case kZcWait:
    case kZcWait2:
      if (len >= 4) gid_ = *reinterpret_cast<const uint32_t*>(data);
      has_next_ = true;
      has_close_ = false;
      open_ = true;
      return;
    case kZcClose:
      if (len >= 4) gid_ = *reinterpret_cast<const uint32_t*>(data);
      // Ignore les CLOSE PARASITES hors session : au login, les scripts
      // OnPCLoginEvent / alootid2 envoient des `close` sans dialogue ouvert. Un vrai
      // close suit toujours un `mes` (open_ déjà posé par le SAY). Le natif les ignore
      // aussi. (`close;` sans `mes` = erreur serveur, donc jamais légitime.)
      if (!open_) return;  // close parasite hors session (login/OnPCLoginEvent/alootid2)
      has_close_ = true;
      has_next_ = false;
      start_fresh_ = true;  // le prochain SAY = nouvelle conversation
      open_ = true;
      return;
    case kZcMenu: {  // {len:2, GID:4, "opt1:opt2:..."(len-8)}
      if (len < 6) return;
      const uint16_t plen = *reinterpret_cast<const uint16_t*>(data);
      gid_ = *reinterpret_cast<const uint32_t*>(data + 2);
      const int mlen = static_cast<int>(plen) - 8;
      choices_.clear();
      if (mlen > 0) {
        std::string m(reinterpret_cast<const char*>(data + 6),
                      static_cast<size_t>(mlen));
        // Séparateur RO = ':' (découpe fidèle au natif). NB : un ':' DANS un libellé
        // scinde l'option (limite RO côté serveur, le natif aussi) -> ne pas mettre de
        // ':' dans un nom d'option/map (à corriger dans la SQL).
        // Les entrées VIDES sont GARDÉES : elles comptent dans l'index envoyé au
        // serveur (menus dynamiques : select("a","","b") -> "b" = 3). Le natif les
        // masque à l'affichage seulement -> DrawMenu les saute.
        size_t start = 0;
        while (start <= m.size()) {
          size_t sep = m.find(':', start);
          std::string opt = (sep == std::string::npos) ? m.substr(start)
                                                        : m.substr(start, sep - start);
          choices_.push_back(opt);
          if (sep == std::string::npos) break;
          start = sep + 1;
        }
      }
      ++menu_gen_;  // nouveau menu -> nouvelle génération (ré-autorise UN envoi)
      has_next_ = false;
      has_close_ = false;
      menu_filter_[0] = '\0';
      menu_hot_ = -1;
      open_ = true;
      return;
    }
    case kZcEditN:
      if (len >= 4) gid_ = *reinterpret_cast<const uint32_t*>(data);
      input_mode_ = kInputNumber;
      num_buf_[0] = '\0';
      input_need_focus_ = true;  // focus auto du champ à l'apparition
      open_ = true;
      return;
    case kZcEditS:
      if (len >= 4) gid_ = *reinterpret_cast<const uint32_t*>(data);
      input_mode_ = kInputString;
      str_buf_[0] = '\0';
      input_need_focus_ = true;  // focus auto du champ à l'apparition
      open_ = true;
      return;
    case kZcClear:
      lines_.clear();
      return;
    default:
      return;
  }
}

// ── Parsing d'une ligne en runs (couleur ^RRGGBB, gras/italique, liens) ──
void NpcDialogWindow::ParseLine(const std::string& raw, std::vector<Run>* out) {
  Run cur;
  cur.color = 0;
  bool in_info = false;  // dans <INFO>…</INFO> : on capture l'id item (pas d'affichage)
  auto flush = [&]() {
    if (!cur.text.empty()) {
      out->push_back(cur);
      cur.text.clear();
    }
  };
  const char* p = raw.c_str();
  const char* end = p + raw.size();
  while (p < end) {
    // Couleur ^RRGGBB (^000000 = couleur par défaut).
    if (*p == '^' && (end - p) >= 7 && IsHex6(p + 1)) {
      flush();
      unsigned v = static_cast<unsigned>(std::strtoul(std::string(p + 1, p + 7).c_str(),
                                                      nullptr, 16));
      cur.color = (v == 0) ? 0
                           : IM_COL32((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF, 0xFF);
      p += 7;
      continue;
    }
    // Icône item ^i[<id décimal>] (rendue en image inline, comme le natif).
    if (*p == '^' && (end - p) >= 3 && (p[1] == 'i' || p[1] == 'I') && p[2] == '[') {
      const char* rb =
          static_cast<const char*>(std::memchr(p + 3, ']', end - (p + 3)));
      if (rb) {
        flush();
        Run icon;
        icon.color = 0;
        icon.icon_id = std::atoi(std::string(p + 3, rb).c_str());
        out->push_back(icon);
        p = rb + 1;
        continue;
      }
    }
    // Balises <...>.
    if (*p == '<') {
      const char* gt = static_cast<const char*>(std::memchr(p, '>', end - p));
      if (gt) {
        std::string up = UpperAscii(std::string(p + 1, gt));
        p = gt + 1;
        if (up == "B") { flush(); cur.bold = true; }
        else if (up == "/B") { flush(); cur.bold = false; }
        else if (up == "I") { flush(); cur.italic = true; }
        else if (up == "/I") { flush(); cur.italic = false; }
        else if (up.rfind("FONT", 0) == 0 || up == "/FONT") { flush(); /* strip */ }
        else if (up == "INFO") { in_info = true; /* capture l'arg, pas de flush */ }
        else if (up == "/INFO") { in_info = false; }
        // link = cmd natif (cf. UIRichTextCtrl_WndProc case 0x62) : détermine l'action au clic.
        else if (up == "ITEM" || up == "ITEML") { flush(); cur.link = 0x1D0; cur.link_arg.clear(); }
        else if (up == "URL")                   { flush(); cur.link = 0x1B5; cur.link_arg.clear(); }
        else if (up == "NAVI" || up == "NAVIL") { flush(); cur.link = 0x1B6; cur.link_arg.clear(); }
        else if (up == "QUEST")                 { flush(); cur.link = 0x21B; cur.link_arg.clear(); }
        else if (up == "TIPBOX" || up == "MSG") { flush(); cur.link = 1;     cur.link_arg.clear(); }
        else if (!up.empty() && up[0] == '/')   { flush(); cur.link = 0;     cur.link_arg.clear(); }
        // (balise inconnue : consommée/masquée)
        continue;
      }
    }
    unsigned char uc = static_cast<unsigned char>(*p);
    if (uc == 0xA0) uc = ' ';  // espace insécable (CP1252) -> espace normal
    if (in_info) {
      cur.link_arg += static_cast<char>(uc);  // contenu <INFO> INTÉGRAL (id d'item OU url)
    } else {
      cur.text += static_cast<char>(uc);
    }
    ++p;
  }
  flush();
}

// ── Rendu word-wrap multi-couleur (ImDrawList) ──
void NpcDialogWindow::DrawRichLines() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const float fsize = ImGui::GetFontSize();
  const float line_h = ImGui::GetTextLineHeightWithSpacing();
  const float wrap = ImGui::GetContentRegionAvail().x;
  const ImU32 def_col = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float space_w = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, " ").x;

  float x = 0.0f, y = 0.0f;
  std::vector<Run> runs;
  for (const std::string& raw : lines_) {
    runs.clear();
    ParseLine(raw, &runs);
    for (const Run& r : runs) {
      // Icône item ^i[id] : hauteur = ligne, largeur au RATIO d'origine (pas de déform).
      if (r.icon_id != 0) {
        ro::IconTex ic = ro::ItemIcon(static_cast<uint32_t>(r.icon_id));
        if (ic.tex && ic.h > 0) {
          const float ih = fsize + 3.0f;
          const float iw = ih * static_cast<float>(ic.w) / static_cast<float>(ic.h);
          if (x > 0.0f && x + iw > wrap) { x = 0.0f; y += line_h; }
          const ImVec2 ip(origin.x + x, origin.y + y);
          dl->AddImage(TexId(ic.tex), ip, ImVec2(ip.x + iw, ip.y + ih));
          x += iw + 3.0f;
        }
        continue;
      }
      // Un lien prend TOUJOURS la couleur de lien (bleu, comme le natif) ; sinon la
      // couleur ^RRGGBB explicite, ou le texte par défaut.
      const ImU32 col = r.link ? kLinkColor : (r.color ? r.color : def_col);
      const std::string u = AnsiToUtf8(r.text);  // scripts serveur = ANSI -> UTF-8
      // Découpe le run en mots (word-wrap manuel). Sûr en UTF-8 : un octet de
      // continuation (0x80-0xBF) n'est jamais 0x20, donc split sur ' ' est correct.
      size_t i = 0;
      while (i < u.size()) {
        const float gap_x = x;      // x AVANT les espaces de tête (zone cliquable du lien)
        const float gap_y = y;
        while (i < u.size() && u[i] == ' ') {  // avale les espaces de tête
          x += space_w;
          ++i;
        }
        size_t j = i;
        while (j < u.size() && u[j] != ' ') ++j;
        if (j == i) break;
        const char* w0 = u.c_str() + i;
        const char* w1 = u.c_str() + j;
        const float ww = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, w0, w1).x;
        bool wrapped = false;
        if (x > 0.0f && x + ww > wrap) { x = 0.0f; y += line_h; wrapped = true; }
        const ImVec2 pos(origin.x + x, origin.y + y);
        dl->AddText(pos, col, w0, w1);
        if (r.bold)  // fake-bold : re-dessine décalé de 1px
          dl->AddText(ImVec2(pos.x + 1.0f, pos.y), col, w0, w1);
        if (r.link) {  // souligne + rend cliquable (item -> desc ; url -> navigateur)
          // Étend à gauche jusqu'au début des espaces de tête (MÊME ligne) pour que TOUT
          // le libellé du lien soit cliquable/souligné en continu, y compris entre 2 mots.
          const float x0 = (!wrapped && gap_y == y) ? (origin.x + gap_x) : pos.x;
          dl->AddLine(ImVec2(x0, pos.y + fsize), ImVec2(pos.x + ww, pos.y + fsize),
                      kLinkColor);
          if (!r.link_arg.empty() &&
              ImGui::IsMouseHoveringRect(ImVec2(x0, pos.y),
                                         ImVec2(pos.x + ww, pos.y + fsize + 2.0f))) {
            ro::SetHoverCursor(2);  // 2 = curseur « main » RO (le jeu dessine son curseur)
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
              pending_link_cmd_ = r.link;      // action décidée au tick (item/url)
              pending_link_arg_ = r.link_arg;
            }
          }
        }
        x += ww;
        i = j;
      }
    }
    x = 0.0f;
    y += line_h;  // fin de ligne source
  }
  ImGui::Dummy(ImVec2(wrap, y));  // réserve la hauteur (scroll)
}

void NpcDialogWindow::DrawMenu(float group_h) {
  if (choices_.empty()) return;
  // Groupe menu à hauteur FIXE (barre de recherche + liste) : ne pousse pas les
  // boutons hors de la fenêtre.
  ImGui::BeginChild("##menugrp", ImVec2(0, group_h), false, ImGuiWindowFlags_NoScrollbar);

  const size_t shown_count = choices_.size() -  // options réellement affichées
      static_cast<size_t>(std::count(choices_.begin(), choices_.end(), std::string()));
  const bool filterable = menu_search_ && shown_count > 8;  // barre de recherche (option)
  bool enter_from_search = false;
  if (filterable) {
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##menufilter", "Rechercher...", menu_filter_,
                                 sizeof(menu_filter_),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
      enter_from_search = true;  // Entrée dans la recherche = valide le choix focus
  }
  const std::string f = LowerAscii(menu_filter_);

  // Navigation clavier : auto-focus de l'option 1 à l'ouverture, flèches haut/bas
  // (avec wrap) sur le nombre d'items VISIBLES de la frame précédente.
  if (menu_hot_ < 0) menu_hot_ = 0;  // auto-focus #1
  bool nav = false;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { ++menu_hot_; nav = true; }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))   { --menu_hot_; nav = true; }
  if (menu_vis_count_ > 0) {
    if (menu_hot_ < 0) menu_hot_ = menu_vis_count_ - 1;   // wrap bas
    if (menu_hot_ >= menu_vis_count_) menu_hot_ = 0;      // wrap haut
  } else {
    menu_hot_ = 0;
  }

  int chosen = -1;              // index ORIGINAL 1-based à envoyer
  std::vector<int> visible;     // index originaux visibles (pour les touches 1-9)

  ImGui::BeginChild("##menu", ImVec2(0, 0), true);  // remplit le reste du groupe (scroll interne)
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const float fsize = ImGui::GetFontSize();
  const ImU32 def_col = ImGui::GetColorU32(ImGuiCol_Text);
  const float row_h = ImGui::GetFontSize() + 3.0f;  // lignes COMPACTES -> plus d'items visibles
  const float pad_x = ImGui::GetStyle().ItemSpacing.x;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(pad_x, 0.0f));  // aucun espace vertical entre items
  for (int i = 0; i < static_cast<int>(choices_.size()); ++i) {
    if (choices_[i].empty()) continue;  // entrée vide = masquée (mais son index compte)
    std::vector<Run> mruns;
    ParseLine(choices_[i], &mruns);  // découpe en runs colorés (^RRGGBB)
    std::string plain;               // texte brut pour le filtre
    for (const Run& mr : mruns) plain += mr.text;
    const std::string uplain = AnsiToUtf8(plain);
    if (!f.empty() && LowerAscii(uplain.c_str()).find(f) == std::string::npos)
      continue;
    const bool hot = (static_cast<int>(visible.size()) == menu_hot_);  // item focus clavier ?
    visible.push_back(i);
    // Selectable transparent (clic + surbrillance ; `hot`=focus clavier) ; le texte
    // MULTICOLORE est dessiné par-dessus (chaque run garde sa couleur).
    char id[16];
    std::snprintf(id, sizeof(id), "##m%d", i);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable(id, hot, ImGuiSelectableFlags_None, ImVec2(0, row_h)))
      chosen = i + 1;
    if (hot && nav) ImGui::SetScrollHereY(0.5f);  // garde le focus visible pendant la nav
    float x = p0.x + pad_x;
    const float ty = p0.y + (row_h - fsize) * 0.5f;
    char num[16];
    std::snprintf(num, sizeof(num), "%d. ", static_cast<int>(visible.size()));
    dl->AddText(ImVec2(x, ty), def_col, num);
    x += font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, num).x;
    for (const Run& r : mruns) {
      if (r.icon_id != 0) {  // icône item ^i[id] inline (ratio d'origine gardé)
        ro::IconTex ic = ro::ItemIcon(static_cast<uint32_t>(r.icon_id));
        if (ic.tex && ic.h > 0) {
          const float ih = row_h - 1.0f;
          const float iw = ih * static_cast<float>(ic.w) / static_cast<float>(ic.h);
          const float iy = p0.y + (row_h - ih) * 0.5f;
          dl->AddImage(TexId(ic.tex), ImVec2(x, iy), ImVec2(x + iw, iy + ih));
          x += iw + 3.0f;
        }
        continue;
      }
      const std::string u = AnsiToUtf8(r.text);
      if (u.empty()) continue;
      const ImU32 col = r.color ? r.color : def_col;
      dl->AddText(ImVec2(x, ty), col, u.c_str());
      if (r.bold) dl->AddText(ImVec2(x + 1.0f, ty), col, u.c_str());
      x += font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, u.c_str()).x;
    }
  }
  ImGui::PopStyleVar();   // ItemSpacing
  ImGui::EndChild();      // ##menu (liste)
  ImGui::EndChild();      // ##menugrp

  // Touches 1-9 : sélectionne le N-ième choix VISIBLE (envoie son index original).
  // Seulement hors saisie clavier (sinon taper "1" dans le filtre choisirait #1) ET
  // sans modificateur : Ctrl/Alt/Shift + chiffre = combo hotkey (skillbar, macro…),
  // à laisser passer au jeu, pas une sélection de menu.
  const ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
    for (int k = 1; k <= 9 && k <= static_cast<int>(visible.size()); ++k) {
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + (k - 1)), false))
        chosen = visible[k - 1] + 1;
    }
  }
  // Entrée = valide l'option focus (depuis la recherche via EnterReturnsTrue, ou
  // globalement hors saisie clavier).
  const bool enter = enter_from_search ||
                     (!ImGui::GetIO().WantTextInput &&
                      (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)));
  if (enter && menu_hot_ >= 0 && menu_hot_ < static_cast<int>(visible.size()))
    chosen = visible[menu_hot_] + 1;

  menu_vis_count_ = static_cast<int>(visible.size());
  // Anti double-envoi : une génération de menu ne peut être répondue qu'UNE fois (sinon
  // le 1er choix fait avancer le serveur et le 2e envoi vise un menu à autre cardinalité
  // -> « Invalid menu selection ... got 14, valid [1..3] »).
  if (chosen > 0 && menu_gen_ != menu_answered_gen_) SendMenuChoice(chosen);
}

void NpcDialogWindow::DrawInput() {
  if (input_mode_ == kInputNone) return;
  if (input_need_focus_) {  // 1re frame : SetKeyboardFocusHere cible le prochain widget = le champ
    ImGui::SetKeyboardFocusHere();
    input_need_focus_ = false;
  }
  if (input_mode_ == kInputNumber) {
    ImGui::SetNextItemWidth(160.0f);
    const bool enter = ImGui::InputText(
        "##num", num_buf_, sizeof(num_buf_),
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ro::RoButton("OK") || enter) SendNumber(std::atoi(num_buf_));
  } else {  // kInputString
    ImGui::SetNextItemWidth(-60.0f);
    const bool enter = ImGui::InputText("##str", str_buf_, sizeof(str_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ro::RoButton("OK") || enter) SendString(str_buf_);
  }
}

// ── Section « Fenêtre NPC » du panneau Moonlight ─────────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent que
// l'état de CE plugin, ils appartiennent donc à ce fichier. MoonlightUi ne garde
// que l'appel et la décision de sauvegarder.
bool NpcDialogWindow::DrawSettings() {
  bool changed = false;
  changed |= ro::RoCheckbox("Dialogue NPC ImGui", &imgui_enabled_);
  ImGui::SameLine();
  HelpMarker(
      "Remplace le dialogue / menu / prompt NPC natif par un overlay ImGui "
      "(texte en couleur, menu à navigation clavier : flèches + Entrée, "
      "touches 1-9). Opt-in ; la fenêtre native est cachée quand c'est actif.");

  ImGui::BeginDisabled(!imgui_enabled_);
  changed |= ro::RoCheckbox("Barre de recherche du menu", &menu_search_);
  ImGui::SameLine();
  HelpMarker(
      "Affiche un champ de recherche au-dessus des longs menus (plus de 8 "
      "choix) pour filtrer les options. Décoche pour un menu épuré.");
  ImGui::EndDisabled();
  return changed;
}

void NpcDialogWindow::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;
  // Rien à afficher (transitoire entre paquets) : pas de fenêtre vide, sauf si un
  // bouton Next/Close est demandé (pour pouvoir cliquer Fermer).
  if (lines_.empty() && choices_.empty() && input_mode_ == kInputNone && !has_next_ &&
      !has_close_)
    return;

  const bool opening = need_pos_;  // 1re frame de cette ouverture (z-order à forcer)
  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(280.0f, 360.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowFocus();  // effectif car NoBringToFrontOnFocus est OMIS cette frame
    need_pos_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(460.0f, 300.0f), ImGuiCond_FirstUseEver);

  char title[96];
  auto nit = npc_names_.find(gid_);
  const std::string name = AnsiToUtf8(
      (nit != npc_names_.end() && !nit->second.empty()) ? nit->second : std::string("PNJ"));
  std::snprintf(title, sizeof(title), "%s###bourgeon_npc_dialog", name.c_str());

  // Skin « fenêtre de description » (barre claire skill_upbar + cadre sysbox, fond
  // crème) — même habillage que les panneaux item/skill. Échap géré par la pile RO
  // (auto-enregistrée via &show_panel_).
  // Fermeture possible seulement quand elle a un sens côté serveur : `close` (fermeture
  // réelle) ou `menu` (annulation -> CZ_CHOOSE_MENU 0xFF). PAS pendant un `next` : y
  // envoyer CZ_CLOSE_DIALOG ferait AVANCER le script (npc_scriptcont reprend) au lieu de
  // fermer -> le natif n'offre d'ailleurs que « Suivant » à ce moment. Donc pas de croix
  // ⊗ ni d'Échap pendant un `next`.
  const bool can_close = has_close_ || !choices_.empty();
  // État menu/input CAPTURÉ AVANT DrawMenu/DrawInput : ces fonctions, sur Entrée,
  // répondent PUIS vident choices_/input_mode_. Sans ce snapshot, le footer verrait
  // choices_ déjà vide et piloterait « Annuler » sur la MÊME touche -> double envoi
  // (choix de menu + annulation). Donc : clavier du footer actif seulement s'il n'y a
  // NI menu NI input en cours.
  const bool footer_kbd_ok = choices_.empty() && input_mode_ == kInputNone;
  // NoBringToFrontOnFocus : ne PAS repasser au 1er plan quand on clique l'overlay (sinon
  // un clic sur un lien re-focus l'overlay et masque la fenêtre de description qui vient
  // d'ouvrir). MAIS on l'OMET sur la 1re frame d'ouverture : avec ce flag posé,
  // FocusWindow saute BringWindowToDisplayFront -> SetNextWindowFocus n'a aucun effet sur
  // le z-order et le dialogue s'ouvrait enterré derrière charsheet/inventaire. En
  // l'omettant juste cette frame, le SetNextWindowFocus ci-dessus le remonte réellement ;
  // les frames suivantes le reposent pour figer le comportement au clic de lien.
  ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoCollapse |
                               ImGuiWindowFlags_NoScrollbar |
                               ImGuiWindowFlags_NoScrollWithMouse;
  if (!opening) win_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
  const bool begun = ro::BeginRoDescWindow(
      title, can_close ? &show_panel_ : nullptr,  // ⊗ + Échap seulement si fermable
      win_flags);
  if (!show_panel_) {
    CloseDialog();
    show_panel_ = true;
    ro::EndRoDescWindow();
    return;
  }
  if (begun) {
    // Capture le clavier pour l'overlay tant qu'il est ouvert : Entrée/Espace/1-9/flèches
    // servent à piloter le dialogue et NE DOIVENT PAS fuir vers le JEU (sinon Entrée ouvre
    // le chat). Le hook WndProc (ragnarok_client) avale les touches quand
    // io.WantCaptureKeyboard est vrai ; une fenêtre à boutons ne le pose pas seule.
    // EXCEPTION : si un modificateur est tenu (Ctrl/Alt/Shift), on NE capture PAS -> les
    // combos (skillbar, macros) passent au jeu. La saisie d'un champ garde sa propre
    // capture via WantTextInput, donc Maj+lettre y fonctionne toujours.
    const ImGuiIO& io_kb = ImGui::GetIO();
    if (!io_kb.KeyCtrl && !io_kb.KeyAlt && !io_kb.KeyShift)
      ImGui::SetNextFrameWantCaptureKeyboard(true);

    // Layout à FOOTER FIXE : texte (flexible, scroll interne) + menu (borné, scroll
    // interne) + input, puis les boutons ÉPINGLÉS en bas. La fenêtre est NoScrollbar
    // -> jamais de grand scrollbar extérieur qui emporterait le bouton Annuler.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float row = ImGui::GetFrameHeightWithSpacing();
    const float sep = st.ItemSpacing.y + 1.0f;               // ~hauteur d'un Separator
    const float avail = ImGui::GetContentRegionAvail().y;
    const float btm_pad = 8.0f;                              // marge sous les boutons
    const float footer_h = sep + row + btm_pad;              // séparateur + boutons + marge bas
    const float input_h = (input_mode_ != kInputNone) ? (sep + row) : 0.0f;
    float menu_grp = 0.0f;                                    // hauteur du groupe menu
    if (!choices_.empty()) {
      const float body = avail - footer_h - input_h;
      menu_grp = std::max(90.0f, std::min(body * 0.5f, 200.0f));
      if (menu_grp > body - 48.0f) menu_grp = body - 48.0f;  // garde ~48px de texte
      if (menu_grp < 0.0f) menu_grp = 0.0f;
    }
    const float menu_total = choices_.empty() ? 0.0f : (sep + menu_grp);
    float text_h = avail - footer_h - input_h - menu_total;
    if (text_h < 1.0f) text_h = 1.0f;

    ImGui::BeginChild("##npctext", ImVec2(0, text_h), false);
    DrawRichLines();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0f)
      ImGui::SetScrollHereY(1.0f);  // auto-scroll en bas
    ImGui::EndChild();

    DrawMenu(menu_grp);
    DrawInput();

    // Footer ÉPINGLÉ : « Suivant » pour un `next` ; « Fermer »/« Annuler » seulement si
    // fermable (close/menu). Pas de fermeture pendant un `next` (le serveur avancerait).
    ImGui::Separator();
    // Entrée/Espace valident le bouton principal — SEULEMENT hors menu (qui consomme
    // Entrée pour son option focus) et hors saisie (l'input a son propre OK). Sans
    // repeat pour qu'un appui maintenu ne re-déclenche pas.
    const bool kbd_ok = footer_kbd_ok &&
                        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
                         ImGui::IsKeyPressed(ImGuiKey_Space, false));
    bool shown = false;
    if (has_next_) {
      if (ro::RoButton("Suivant") || kbd_ok) SendNext();
      shown = true;
    }
    if (can_close) {
      if (shown) ImGui::SameLine();
      if (ro::RoButton(has_close_ ? "Fermer" : "Annuler") || (kbd_ok && !shown))
        CloseDialog();
      shown = true;
    }
    // Rapport de bug contextuel : GID + nom du PNJ joints automatiquement.
    if (auto* br = Bourgeon::Instance().bug_report()) {
      auto nit = npc_names_.find(gid_);
      const std::string npc_name =
          (nit != npc_names_.end()) ? nit->second : std::string();
      if (shown) ImGui::SameLine();
      br->Button(BugReport::NpcContext(gid_, npc_name), "npc_bug");
    }
  }
  ro::EndRoDescWindow();
}

// ── Envois (thread principal uniquement) ──
bool NpcDialogWindow::ShouldSuppressNativeDialogSend(uint16_t opcode) const {
  if (!imgui_enabled_) return false;  // toggle OFF : le natif garde la main
  switch (opcode) {
    case kCzNext:         // 0x00B9 CZ_REQ_NEXT_SCRIPT
    case kCzChoose:       // 0x00B8 CZ_CHOOSE_MENU  (dont le « choix 1 » parasite)
    case kCzInputN:       // 0x0143 CZ_INPUT_EDITDLG
    case kCzInputS:       // 0x01D5 CZ_INPUT_EDITDLGSTR
    case kCzCloseDialog:  // 0x0146 CZ_CLOSE_DIALOG
      return true;
    default:
      return false;
  }
}

void NpcDialogWindow::SendNext() {
  if (gid_ == 0) return;
  uint8_t pkt[6];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kCzNext;
  *reinterpret_cast<uint32_t*>(pkt + 2) = gid_;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  has_next_ = false;    // attend la suite du serveur
  start_fresh_ = true;  // `next` = saut de PAGE -> le prochain mes vide l'ancienne page
}

void NpcDialogWindow::SendMenuChoice(int one_based) {
  if (gid_ == 0 || one_based <= 0 || one_based > 0xFF) return;
  uint8_t pkt[7];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kCzChoose;
  *reinterpret_cast<uint32_t*>(pkt + 2) = gid_;
  pkt[6] = static_cast<uint8_t>(one_based);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  menu_answered_gen_ = menu_gen_;  // cette génération de menu est désormais répondue
  // Détruit la fenêtre menu native (0x11) qu'on a répondue : sinon elle reste vivante
  // (cachée) et peut ré-émettre un CZ_CHOOSE_MENU parasite plus tard (via cmd 0x28) ->
  // « Invalid menu selection ... got 1, valid [1..0] ».
  CloseWnd(kWinMenu);
  choices_.clear();
  menu_filter_[0] = '\0';
  start_fresh_ = true;  // un choix de menu = saut de PAGE -> vide l'ancienne page
}

void NpcDialogWindow::SendMenuCancel() {
  if (gid_ == 0) return;
  uint8_t pkt[7];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kCzChoose;
  *reinterpret_cast<uint32_t*>(pkt + 2) = gid_;
  pkt[6] = 0xFF;  // ESC / annulation menu
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  choices_.clear();
}

void NpcDialogWindow::SendNumber(int value) {
  if (gid_ == 0) return;
  uint8_t pkt[10];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kCzInputN;
  *reinterpret_cast<uint32_t*>(pkt + 2) = gid_;
  *reinterpret_cast<int32_t*>(pkt + 6) = value;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  input_mode_ = kInputNone;
}

void NpcDialogWindow::SendString(const char* text) {
  if (gid_ == 0) return;
  const char* s = text ? text : "";
  size_t tlen = std::strlen(s) + 1;  // le serveur attend le \0 final
  if (tlen > 254) tlen = 254;
  const uint16_t total = static_cast<uint16_t>(8 + tlen);
  std::vector<uint8_t> pkt(total, 0);
  *reinterpret_cast<uint16_t*>(pkt.data() + 0) = kCzInputS;
  *reinterpret_cast<uint16_t*>(pkt.data() + 2) = total;
  *reinterpret_cast<uint32_t*>(pkt.data() + 4) = gid_;
  std::memcpy(pkt.data() + 8, s, tlen - 1);  // le buffer est déjà nul-terminé
  Bourgeon::Instance().SendPacket(pkt.data(), pkt.size());
  input_mode_ = kInputNone;
}

void NpcDialogWindow::CloseDialog() {
  const uint32_t gid = gid_;
  // 1. SERVEUR : abandon adapté à l'état (sinon sd->npc_id reste -> perso figé côté
  //    serveur). Menu ouvert -> CZ_CHOOSE_MENU 0xFF (le script reçoit 255 puis
  //    termine) ; sinon CZ_CLOSE_DIALOG.
  if (gid != 0) {
    if (!choices_.empty()) {
      uint8_t pkt[7];
      *reinterpret_cast<uint16_t*>(pkt + 0) = kCzChoose;
      *reinterpret_cast<uint32_t*>(pkt + 2) = gid;
      pkt[6] = 0xFF;
      Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
    } else {
      uint8_t pkt[6];
      *reinterpret_cast<uint16_t*>(pkt + 0) = kCzCloseDialog;
      *reinterpret_cast<uint32_t*>(pkt + 2) = gid;
      Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
    }
  }
  // 2. Détruit les fenêtres natives D'ABORD : sinon cmd 0x28 (ci-dessous) re-déclenche
  //    l'annulation d'une fenêtre menu native résiduelle -> CZ_CHOOSE_MENU parasite.
  CloseWnd(kWinSay); CloseWnd(kWinMenu); CloseWnd(kWinEditN);
  CloseWnd(kWinEditS); CloseWnd(kWinSay2);
  // 3. CLIENT : débloque l'état dialogue (cmd 0x28 + garantie +0x24C=0).
  DispatchNpcCmd(kSelClose);
  ForceClearDialogFlag();
  open_ = false;
  was_open_ = false;
  Reset();
}

void NpcDialogWindow::OpenItemDescById(uint32_t id) {
  if (id == 0) return;
  __try {
    // ItemSkillInfo minimal sur la pile (comme FUN_00803e10 : info[0]=id ; les 2
    // std::string membres restent SSO vides -> aucun heap alloué -> pas de dtor à
    // appeler, le buffer pile est simplement abandonné). La fenêtre 0xc complète le
    // reste depuis la DB client par id (marche même sans posséder l'item).
    uint8_t info[256];
    std::memset(info, 0, sizeof(info));
    reinterpret_cast<ItemInfoCtor_t>(itemdb::kInfoCtorAddr)(info);            // init std::string SSO
    reinterpret_cast<ItemInfoSetId_t>(itemdb::kInfoSetIdAddr)(info, static_cast<int>(id));  // id-str @0x2c
    *reinterpret_cast<uint32_t*>(info) = id;  // id entier @0 (chemin fenêtre natif)
    info[kInfoFlag] = 1;  // « standalone » : la desc est lue depuis la DB (rec+0x0c), item non possédé
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (dwnd) {
      void** vt = *reinterpret_cast<void***>(dwnd);
      reinterpret_cast<DescOnMsg_t>(vt[uiwnd::kVfOnMsg / 4])(
          dwnd, nullptr, 0, itemdb::kItemDescMsgSet,
          static_cast<int>(reinterpret_cast<uintptr_t>(info)), 0, 0, 0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

bool NpcDialogWindow::DialogActiveNative() const {
  __try {
    void* mode = *reinterpret_cast<void**>(rag::kActiveModePtr);
    if (!mode) return false;
    return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(mode) +
                                   kGameModeDialogFlag) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void NpcDialogWindow::HideNativeWindows() {
  HideWnd(FindWnd(kWinSay));
  HideWnd(FindWnd(kWinMenu));
  HideWnd(FindWnd(kWinEditN));
  HideWnd(FindWnd(kWinEditS));
  HideWnd(FindWnd(kWinSay2));
}

void NpcDialogWindow::ShowNativeWindows() {
  ShowWnd(FindWnd(kWinSay));
  ShowWnd(FindWnd(kWinMenu));
  ShowWnd(FindWnd(kWinEditN));
  ShowWnd(FindWnd(kWinEditS));
  ShowWnd(FindWnd(kWinSay2));
}

void NpcDialogWindow::HideNativeAtCreation(void* win, int window_id) {
  if (!win || !imgui_enabled_) return;
  if (window_id == kWinSay || window_id == kWinMenu || window_id == kWinEditN ||
      window_id == kWinEditS || window_id == kWinSay2)
    HideWnd(win);
}

void NpcDialogWindow::OnTick() {
  // Clic sur un lien d'item (<ITEM>) posé pendant le rendu : ouvre la desc au tick
  // (hors de l'arbre ImGui, plus sûr pour créer une fenêtre native).
  //
  // ⚠ ET seulement une fois le bouton RELÂCHÉ. Différer d'un tick ne suffisait
  // pas : le focus de fenêtre reste acquis à la nôtre tant que le bouton est
  // enfoncé, or la remontée du panneau de description est elle-même différée
  // d'une frame (hook OnMsg 0x18 → SetNextWindowFocus). Un appui PROLONGÉ voyait
  // donc la description remonter, puis repasser DERRIÈRE le dialogue ; un clic
  // bref, non. C'est le même correctif que les autres viewers, cf.
  // features/item_cell.h — mais posé ICI parce que ce site a son propre chemin
  // natif (OpenItemDescById écrit l'id ENTIER en info+0x00), pas celui
  // d'itemcell::OpenDescById.
  if (pending_link_cmd_ != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    const int cmd = pending_link_cmd_;
    const std::string arg = pending_link_arg_;
    pending_link_cmd_ = 0;
    pending_link_arg_.clear();
    if (cmd == 0x1D0) {  // <ITEM> -> fenêtre de description (par id, depuis la DB)
      OpenItemDescById(static_cast<uint32_t>(std::atoi(arg.c_str())));
    } else if (cmd == 0x1B5) {  // <URL> -> navigateur
      OpenUrl(arg);
    }
    // 0x1B6 <NAVI> / 0x21B <QUEST> : non gérés (P3)
  }

  // Changement du toggle de config (ON<->OFF) EN COURS de dialogue : fermeture PROPRE
  // (débloque serveur + client, détruit les fenêtres natives) + dé-masquage de
  // sécurité. Sinon, OFF laisse les fenêtres natives CACHÉES + overlay éteint =
  // interaction bloquée (reload obligatoire).
  if (imgui_enabled_ != prev_imgui_enabled_) {
    prev_imgui_enabled_ = imgui_enabled_;
    if (open_ || DialogActiveNative()) CloseDialog();
    if (!imgui_enabled_) ShowNativeWindows();
  }
  if (!imgui_enabled_) {
    if (open_) { open_ = false; was_open_ = false; }
    return;
  }

  if (map_changed_) {
    map_changed_ = false;
    // Warp / changement de map : la session dialogue est morte côté serveur.
    // Nettoyage CLIENT uniquement (pas de paquet) : force le flag à 0 + détruit les
    // fenêtres orphelines, sinon la détection les verrait encore -> réouverture vide
    // en boucle.
    ForceClearDialogFlag();
    CloseWnd(kWinSay); CloseWnd(kWinMenu); CloseWnd(kWinEditN);
    CloseWnd(kWinEditS); CloseWnd(kWinSay2);
    open_ = false; was_open_ = false;
    Reset();
    return;
  }

  // Signal d'ouverture = FLAG dialogue client (CGameMode+0x24C), posé par les
  // handlers natifs (SAY/WAIT/MENU) et remis à 0 à la fermeture/au dtor. Plus fiable
  // que FindWnd (les fenêtres cachées PERSISTENT -> détection collante = réouverture).
  // Un prompt input sans 'mes' préalable n'arme pas toujours le flag -> on retient
  // aussi input_mode_ (remis à none à la soumission).
  // open_ = flag de SESSION, posé par les paquets de dialogue (OnRecvPacket) et remis
  // à 0 UNIQUEMENT par CloseDialog (clic Fermer) ou un warp. On ne le recalcule PAS
  // depuis +0x24C : ce flag natif retombe à 0 ~quelques frames après un `close` alors
  // que le dialogue doit RESTER affiché jusqu'au clic (le natif fait pareil). Un
  // `close` parasite hors session est déjà filtré au recv.
  if (open_) {
    HideNativeWindows();
    if (!was_open_) need_pos_ = true;
  }
  was_open_ = open_;
}
