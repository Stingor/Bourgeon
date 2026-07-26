#include "plugins/native_login.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <cstdint>

#include "utils/log_console.h"

namespace {

// Adresses natives (client 20250716, base 0x400000, pas d'ASLR).
// Voir docs/login_connect_re.md pour la RE complète (workflow + vérif adversariale).
constexpr uintptr_t kPCurrentMode   = 0x0121333C;  // void** -> CMode actif
constexpr uintptr_t kVtblCLoginMode = 0x010932F0;  // garde de mode
constexpr uintptr_t kVtblUILoginWnd = 0x01030168;  // garde de validité de la fenêtre
constexpr uintptr_t kPLoginWnd      = 0x0131F6B4;  // UILoginWnd* (== mgr+0x1cc, FindWindow id 3)
constexpr uintptr_t kSocketFd       = 0x015C5A24;  // g_RagConnection_SocketFd
constexpr uintptr_t kAcctClassNormal = 0x01031264;
constexpr uintptr_t kSetTextAddr    = 0x008303F0;  // CUIEdit_SetText
constexpr uintptr_t kGetTextAddr    = 0x008210A0;  // UIEdit_GetTextPtr
constexpr uintptr_t kOnMsgAddr      = 0x008848D0;  // UILoginWnd_OnMsg
constexpr int       kCharSelectWndId = 0x115;      // UINewSelectCharWnd (277)

// Offsets UILoginWnd (tous prouvés au désasm — cf. login_connect_re.md).
constexpr int kOffEditId    = 0xB4;  // édit ID (SendMsg 0x2718 dans le handler natif)
constexpr int kOffEditPw    = 0xB8;  // édit MOT DE PASSE (SendMsg 0x2717 ; masque +0x84==0x2a)
constexpr int kOffBg        = 0xBC;  // fond (fenêtre id 0x145, non enfant)
constexpr int kOffAcctClass = 0xEC;
constexpr int kOffVisible   = 0x28;  // flag visibilité UIWindow

using SetText_t = void(__thiscall*)(void* edit, const char* text);
using GetText_t = const char*(__thiscall*)(void* edit);
// ⚠ 6 ARGS PILE (la fonction fait RET 0x18) — un typedef à 5 args corrompt ESP
// de +4 par appel => crash. Vérifié au désasm (vérif adversariale du workflow).
using OnMsg_t = int(__thiscall*)(void*, int, int, int, int, int, int);
// CLoginMode_SendMsg (0x00d2a130), vtbl_CLoginMode+0x18. RET 0x14 = 5 args pile
// (this + 5). cmd 0x2713 = sélectionne la connexion `a1`(=index) du service-select.
using SendMsg_t = int(__thiscall*)(void*, int, const void*, int, int, int);

// Table des connexions (service-select) DANS le CLoginMode : base mode+0x1e8,
// stride 0xa0 ; +0x1e8=IP(u32) +0x1ec=port(u16) +0x1ee=nom +0x204=état. (Prouvé au
// désasm de CLoginMode_SendMsg cmd 0x2713.) NB : cette zone est RÉUTILISÉE après
// login pour la liste char-server (stride 0x20) — donc à ne lire QU'AVANT login.
constexpr int kConnBase   = 0x1E8;  // = IP de la connexion 0
constexpr int kConnStride = 0xA0;

// Renvoie le CMode courant s'il s'agit bien de CLoginMode, sinon nullptr.
void* CurrentLoginMode() {
  __try {
    void* mode = *reinterpret_cast<void**>(kPCurrentMode);
    if (mode && *reinterpret_cast<uintptr_t*>(mode) == kVtblCLoginMode) return mode;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return nullptr;
}

// UILoginWnd* seulement s'il est VIVANT : le cache mgr+0x1cc (0x0131f6b4) est
// invalidé quand la fenêtre est détruite (OnMsg 0xBA la ferme au tir ; le
// char-select la détruit aussi), mais un slot peut rester pendouillant. On valide
// donc la vtable avant tout déréférencement/écriture (anti-UAF — le seul garde
// CurrentLoginMode ne suffit PAS : le char-select est le MÊME CLoginMode).
void* ValidLoginWnd() {
  __try {
    void* w = *reinterpret_cast<void**>(kPLoginWnd);
    if (w && *reinterpret_cast<uintptr_t*>(w) == kVtblUILoginWnd) return w;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return nullptr;
}

inline void SetVis(void* w, int v) {
  if (w) *reinterpret_cast<int*>(reinterpret_cast<char*>(w) + kOffVisible) = v;
}

}  // namespace

bool native_login::AtLoginScreen() { return CurrentLoginMode() != nullptr; }

bool native_login::LoginWindowPresent() {
  return CurrentLoginMode() != nullptr && ValidLoginWnd() != nullptr;
}

bool native_login::SelectClientInfoConnection(int index) {
  void* mode = CurrentLoginMode();
  if (!mode || index < 0) return false;
  __try {
    auto fn = reinterpret_cast<SendMsg_t>(
        (*reinterpret_cast<uintptr_t**>(mode))[0x18 / 4]);
    // cmd 0x2723 : DAT_015ff81c=index, mode+0x6f48=index, Apply_ClientInfoConnection
    // (FUN_00a72da0) charge la connexion #index, puis mode+0xc = 3 (écran login)
    // — ou 0xd si servicetype 5/7. Aucune table à valider : les <connection> sont
    // parsées au boot (LoadClientInfoXml), pas dans le mode.
    LogDiag("[native_login] SelectClientInfoConnection idx={} : tir 0x2723", index);
    fn(mode, 0x2723, reinterpret_cast<const void*>(index), 0, 0, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogDiag("[native_login] SelectClientInfoConnection idx={} : EXCEPTION", index);
    return false;
  }
}

bool native_login::SelectConnection(int index) {
  void* mode = CurrentLoginMode();
  if (!mode || index < 0) return false;
  __try {
    // La connexion doit être chargée (IP non nulle) avant sélection : sinon on
    // tirerait 0x2713 sur une entrée vide (connexion vers 0.0.0.0). Tant que
    // l'IP est nulle on renvoie false -> l'appelant réessaie au frame suivant.
    char* m = reinterpret_cast<char*>(mode);
    uint32_t ip = *reinterpret_cast<uint32_t*>(m + kConnBase + index * kConnStride);
    if (ip == 0) return false;
    // cmd 0x2713 = sélectionne la connexion `index` (pose l'état de connexion,
    // comme un clic/Entrée sur la fenêtre service-select mais instantané).
    auto fn = reinterpret_cast<SendMsg_t>(
        (*reinterpret_cast<uintptr_t**>(mode))[0x18 / 4]);
    LogDiag("[native_login] SelectConnection idx={} : tir 0x2713 (mode=0x{:x})",
            index, reinterpret_cast<uintptr_t>(mode));
    fn(mode, 0x2713, reinterpret_cast<const void*>(index), 0, 0, 0);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogDiag("[native_login] SelectConnection idx={} : EXCEPTION", index);
    return false;
  }
}

void native_login::LogConnectionTable(int count, const char* tag) {
  void* mode = CurrentLoginMode();
  if (!mode) {
    LogDiag("[native_login] table connexions ({}) : PAS en CLoginMode", tag);
    return;
  }
  if (count < 0) count = 0;
  if (count > 8) count = 8;  // garde-fou (dev n'a jamais 8 connexions)
  __try {
    char* m = reinterpret_cast<char*>(mode);
    for (int i = 0; i < count; ++i) {
      char* e = m + kConnBase + i * kConnStride;              // base entrée i
      uint32_t ip = *reinterpret_cast<uint32_t*>(e);          // +0x1e8 IP (u32)
      uint16_t port = *reinterpret_cast<uint16_t*>(e + 4);    // +0x1ec port (u16)
      int16_t state = *reinterpret_cast<int16_t*>(e + 0x1c);  // +0x204 état
      char nm[24] = {0};
      lstrcpynA(nm, e + 6, 20);  // +0x1ee nom
      const unsigned char* b = reinterpret_cast<const unsigned char*>(&ip);
      // NB: caster les octets en int -> fmt formaterait un `unsigned char` comme
      // un CARACTÈRE (illisible), pas comme un nombre.
      LogDiag(
          "[native_login] table({}) conn[{}] ip={}.{}.{}.{} port={} etat={} nom='{}'",
          tag, i, static_cast<int>(b[0]), static_cast<int>(b[1]),
          static_cast<int>(b[2]), static_cast<int>(b[3]), static_cast<int>(port),
          static_cast<int>(state), nm);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogDiag("[native_login] table connexions ({}) : EXCEPTION (offset/pas prêt)", tag);
  }
}

int native_login::SocketFd() {
  __try {
    return *reinterpret_cast<int*>(kSocketFd);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

bool native_login::CharListLoaded() {
  // g_pCurrentMode (dispatcher CLoginMode) vtbl+0x18 cmd 8 = get CHARACTER_INFO
  // par slot ; renvoie nullptr tant que la liste n'est pas arrivée (HC_ACCEPT_ENTER).
  // ⚠ On SCANNE plusieurs slots (pas seulement le 0) : un compte dont le 1er perso est
  // dans un slot > 0 (création dans un siège libre) renverrait nullptr pour le slot 0
  // MÊME liste chargée -> l'appelant croirait à tort la liste absente (boucle d'Entrée
  // au char-select). Dès qu'UN slot répond non-null, la liste est là.
  using DispCmd_t = void*(__thiscall*)(void*, int, int, int, int, int);
  __try {
    void* d = *reinterpret_cast<void**>(kPCurrentMode);
    if (!d) return false;
    auto fn = reinterpret_cast<DispCmd_t>(
        (*reinterpret_cast<uintptr_t**>(d))[0x18 / 4]);
    for (int slot = 0; slot < 45; ++slot) {
      if (fn(d, 8, slot, 0, 0, 0) != nullptr) return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool native_login::CharSelectWindowPresent() {
  // Fenêtre native du char-select (UINewSelectCharWnd, id 0x115) vivante dans le
  // manager. Sonde PRÉFÉRABLE à CharListLoaded() pour « on est arrivé au
  // char-select » : les CHARACTER_INFO SURVIVENT à un retour à l'écran de connexion
  // (elles restent lisibles par cmd 8), alors que les fenêtres sont TOUTES purgées à
  // chaque changement d'état (UIWindowMgr_DestroyAllWindows 0x00a482f0, appelée en
  // tête de CLoginMode_OnStateEnter). Aucun résidu, donc.
  __try {
    return uiwnd::FindWindow(kCharSelectWndId) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool native_login::DriveLogin(const char* userid, const char* password,
                              char* readback_id, char* readback_pw,
                              unsigned bufsz) {
  if (!CurrentLoginMode()) return false;
  bool fired = false;
  __try {
    void* login = ValidLoginWnd();
    if (!login) return false;  // écran pas prêt (service-select / non construit)
    char* L = reinterpret_cast<char*>(login);
    // Mapping FIXE prouvé au désasm : le handler natif lit +0xB4 -> SendMsg 0x2718
    // (=ID) et +0xB8 -> SendMsg 0x2717 (=MOT DE PASSE). Plus fiable qu'une
    // heuristique de masque à une seule face (qui, si fausse, échangerait
    // silencieusement les credentials).
    void* idEdit = *reinterpret_cast<void**>(L + kOffEditId);
    void* pwEdit = *reinterpret_cast<void**>(L + kOffEditPw);
    if (!idEdit || !pwEdit) return false;

    reinterpret_cast<SetText_t>(kSetTextAddr)(idEdit, userid);
    reinterpret_cast<SetText_t>(kSetTextAddr)(pwEdit, password);

    // Read-back diagnostic : relit les champs natifs APRÈS SetText (avant l'envoi).
    if (readback_id && bufsz) {
      const char* r = reinterpret_cast<GetText_t>(kGetTextAddr)(idEdit);
      lstrcpynA(readback_id, r ? r : "", static_cast<int>(bufsz));
    }
    if (readback_pw && bufsz) {
      const char* r = reinterpret_cast<GetText_t>(kGetTextAddr)(pwEdit);
      lstrcpynA(readback_pw, r ? r : "", static_cast<int>(bufsz));
    }
    // Évite un +0xEC (classe de compte) non initialisé si le combo n'a rien posé.
    *reinterpret_cast<void**>(L + kOffAcctClass) =
        reinterpret_cast<void*>(kAcctClassNormal);
    // Déclenche le bouton Start à l'identique (msg=6, cmd=0xBA). 6 args pile.
    // ⚠ Ce OnMsg DÉTRUIT la fenêtre id 3 en interne (UIWindowMgr_Close) et
    // invalide le cache mgr+0x1cc : ne JAMAIS re-toucher la fenêtre après le tir
    // (MaskLoginWindow / ValidLoginWnd renverront nullptr = no-op de toute façon).
    reinterpret_cast<OnMsg_t>(kOnMsgAddr)(login, 0, 6, 0xBA, 0, 0, 0);
    fired = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
  return fired;
}

void native_login::MaskLoginWindow(bool hide) {
  if (!CurrentLoginMode()) return;
  __try {
    void* login = ValidLoginWnd();  // no-op si la fenêtre a été détruite (anti-UAF)
    if (!login) return;
    const int v = hide ? 0 : 1;
    SetVis(login, v);  // fenêtre login (id 3) + tous ses enfants (combo, édits…)
    SetVis(*reinterpret_cast<void**>(reinterpret_cast<char*>(login) + kOffBg), v);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}
