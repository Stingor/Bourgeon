#include "ragnarok/client_string.h"  // rag::clientstr : la std::string du client
#include "ragnarok/globals.h"
#include "features/systems/native_login.h"

#include "ragnarok/uiwnd.h"
#include <Windows.h>

#include <cstdint>
#include <cstring>

#include "utils/log_console.h"

namespace {

// Adresses natives (client 20250716, base 0x400000, pas d'ASLR).
// Voir docs/login_connect_re.md pour la RE complète (workflow + vérif adversariale).
constexpr uintptr_t kVtblCLoginMode = 0x010932F0;  // garde de mode
constexpr uintptr_t kVtblUILoginWnd = 0x01030168;  // garde de validité de la fenêtre
constexpr uintptr_t kPLoginWnd      = 0x0131F6B4;  // UILoginWnd* (== mgr+0x1cc, FindWindow id 3)
constexpr uintptr_t kSocketFd       = 0x015C5A24;  // g_RagConnection_SocketFd
constexpr uintptr_t kAcctClassNormal = 0x01031264;
constexpr uintptr_t kSetTextAddr    = 0x008303F0;  // CUIEdit_SetText
constexpr uintptr_t kOnMsgAddr      = 0x008848D0;  // UILoginWnd_OnMsg
// DEUX écrans natifs de création de personnage, et il faut connaître les deux.
// 0x116 = UINewMakeCharWnd (« Character Creation », plein écran, ctor 0x0079F890) :
//   celui de l'ÉTAT 8 du mode login (0x00D254BA), donc celui où atterrit un compte
//   SANS personnage — la fenêtre 0x115 n'est alors jamais construite.
// 0xC8  = UIMakeCharWnd (ancien dialogue, ctor 0x0086BC10) : ouvert par le contrôle
//   0x1A0 du char-select (case 416, 0x0079E03A) et par les variantes 4/5 de l'état 7.

// Offsets UILoginWnd (tous prouvés au désasm — cf. login_connect_re.md).
constexpr int kOffEditId    = 0xB4;  // édit ID (SendMsg 0x2718 dans le handler natif)
constexpr int kOffEditPw    = 0xB8;  // édit MOT DE PASSE (SendMsg 0x2717 ; masque +0x84==0x2a)
constexpr int kOffBg        = 0xBC;  // fond (fenêtre id 0x145, non enfant)
constexpr int kOffAcctClass = 0xEC;
// Le texte d'un CUIEdit : la `std::string` que `CUIEdit_SetText` (0x008303F0)
// assigne à `this + 216`. Relu — jamais écrit à la main : l'écriture passe par
// la native, qui prévient aussi le focus et le curseur.
constexpr int kOffEditText  = 0xD8;

using SetText_t = void(__thiscall*)(void* edit, const char* text);
// ⚠ 6 ARGS PILE (la fonction fait RET 0x18) — un typedef à 5 args corrompt ESP
// de +4 par appel => crash. Vérifié au désasm (vérif adversariale du workflow).
using OnMsg_t = int(__thiscall*)(void*, int, int, int, int, int, int);
// CLoginMode_SendMsg (0x00d2a130), vtbl_CLoginMode+0x18. RET 0x14 = 5 args pile
// (this + 5). cmd 0x2713 = sélectionne la connexion `a1`(=index) du service-select.

// Table des connexions (service-select) DANS le CLoginMode : base mode+0x1e8,
// stride 0xa0 ; +0x1e8=IP(u32) +0x1ec=port(u16) +0x1ee=nom +0x204=état. (Prouvé au
// désasm de CLoginMode_SendMsg cmd 0x2713.) NB : cette zone est RÉUTILISÉE après
// login pour la liste char-server (stride 0x20) — donc à ne lire QU'AVANT login.
constexpr int kConnBase   = 0x1E8;  // = IP de la connexion 0
constexpr int kConnStride = 0xA0;

// ── Arbre XML de clientinfo.xml (parsé au boot, gardé en mémoire) ─────────────
// LoadClientInfoXml (0x0171d320) ouvre "clientinfo.xml" via ResFileStream_Open
// (VFS : data\ PUIS les GRF) et laisse le document parsé dans g_ClientInfoXmlDoc.
// Apply_ClientInfoConnection (0x00a72da0) le reparcourt à chaque sélection de
// connexion : l'arbre reste donc vivant toute la session.
constexpr uintptr_t kClientInfoXmlDoc  = 0x0159B8A8;  // racine du document parsé
constexpr uintptr_t kXmlFindChild      = 0x00A98400;  // __thiscall(node, name) -> node
constexpr uintptr_t kXmlFindNextSibling= 0x00A98460;  // __thiscall(node, name) -> node
constexpr uintptr_t kXmlGetText        = 0x00A984C0;  // __fastcall(node) -> std::string*
using XmlFind_t = void*(__thiscall*)(void*, const char*);
using XmlGetText_t = void*(__fastcall*)(void*);
constexpr int kMaxConnections = 8;  // borne du natif (boucle `while (v4 < 8)`)
constexpr int kNameCap = 64;

// Le contenu d'un std::string du client (ici celui que rend XmlNode_GetText)
// se lit avec `rag::clientstr::Data` — le foyer que ce décodage-ci recopiait
// pour la douzième fois, sous un nom de plus (`StdStringData`).

// Remplit `out` avec les <display> des <connection> ; renvoie leur nombre.
// POD uniquement : le SEH interdit les objets à destructeur dans cette fonction.
int ReadConnectionDisplaysRaw(char out[kMaxConnections][kNameCap]) {
  __try {
    auto find_child = reinterpret_cast<XmlFind_t>(kXmlFindChild);
    auto next_sibling = reinterpret_cast<XmlFind_t>(kXmlFindNextSibling);
    auto get_text = reinterpret_cast<XmlGetText_t>(kXmlGetText);
    void* root = find_child(reinterpret_cast<void*>(kClientInfoXmlDoc), "clientinfo");
    if (!root) return 0;
    void* connection = find_child(root, "connection");
    int count = 0;
    while (connection && count < kMaxConnections) {
      out[count][0] = '\0';
      void* display = find_child(connection, "display");
      if (display) {
        const char* name = rag::clientstr::Data(get_text(display));
        if (name) {
          std::strncpy(out[count], name, kNameCap - 1);
          out[count][kNameCap - 1] = '\0';
        }
      }
      ++count;
      connection = next_sibling(connection, "connection");
    }
    return count;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// Renvoie le CMode courant s'il s'agit bien de CLoginMode, sinon nullptr.
void* CurrentLoginMode() {
  __try {
    void* mode = rag::ActiveMode();
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
  if (w) *reinterpret_cast<int*>(reinterpret_cast<char*>(w) + uiwnd::kOffVisible) = v;
}

}  // namespace

bool native_login::AtLoginScreen() { return CurrentLoginMode() != nullptr; }

bool native_login::LoginWindowPresent() {
  return CurrentLoginMode() != nullptr && ValidLoginWnd() != nullptr;
}

std::vector<std::string> native_login::ClientInfoConnectionNames() {
  char raw[kMaxConnections][kNameCap];
  const int count = ReadConnectionDisplaysRaw(raw);
  std::vector<std::string> names;
  names.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) names.emplace_back(raw[i]);
  return names;
}

bool native_login::SelectClientInfoConnection(int index) {
  void* mode = CurrentLoginMode();
  if (!mode || index < 0) return false;
  __try {
    // cmd 0x2723 : DAT_015ff81c=index, mode+0x6f48=index, Apply_ClientInfoConnection
    // (FUN_00a72da0) charge la connexion #index, puis mode+0xc = 3 (écran login)
    // — ou 0xd si servicetype 5/7. Aucune table à valider : les <connection> sont
    // parsées au boot (LoadClientInfoXml), pas dans le mode.
    rag::ModeSendMsg(mode, 0x2723, index);
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
    rag::ModeSendMsg(mode, 0x2713, index);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    LogDiag("[native_login] SelectConnection idx={} : EXCEPTION", index);
    return false;
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
  __try {
    void* d = rag::ActiveMode();
    if (!d) return false;
    for (int slot = 0; slot < 45; ++slot) {
      if (rag::ModeSendMsgPtr(d, 8, slot) != nullptr) return true;
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool native_login::CharServerWindowPresent() {
  // Fenêtre « Select Service » (liste des char-servers, boutons OK/Cancel),
  // construite par l'état 6 du mode login — atteint uniquement quand
  // Net_OnAcceptLogin_ParseAccount (AC_ACCEPT_LOGIN 0x0ac4) pose `mode+0xc = 6`.
  // Sa présence vaut donc « login ACCEPTÉ par le serveur ».
  __try {
    return uiwnd::FindWindow(uiwnd::kCharServerWndId) != nullptr;
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
    return uiwnd::FindWindow(uiwnd::kUINewSelectCharWnd) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool native_login::MakeCharWindowPresent() {
  // ⚠ MESURÉ LE 2026-08-11, et c'était tout le bug : sur un compte SANS
  // personnage, l'inventaire des fenêtres vivantes (uiwnd::ListWindowIds) rend
  // `[0x257, 0x257, 0x116]` pendant que le joueur tape son nom. Ni 0x115 ni 0xC8
  // — l'écran est la fenêtre **0x116**, construite par l'état 8 du mode login.
  // Ne tester que 0xC8 revenait à conclure « aucun écran natif » et à garder le
  // clavier : la saisie du nom était impossible.
  //
  // Le dialogue 0xC8, lui, ne remplace pas le char-select (le contrôle 0x1A0 fait
  // un simple MakeWindow, sans changement d'état) : 0x115 répond encore à côté.
  __try {
    return uiwnd::FindWindow(uiwnd::kUINewMakeCharWnd) != nullptr ||
           uiwnd::FindWindow(uiwnd::kUIMakeCharWnd) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool native_login::DriveLogin(const char* userid, const char* password) {
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

bool native_login::ClearLoginIdIf(const char* userid) {
  if (userid == nullptr || userid[0] == '\0') return false;
  if (!CurrentLoginMode()) return false;
  __try {
    void* login = ValidLoginWnd();
    if (!login) return false;
    void* idEdit =
        *reinterpret_cast<void**>(reinterpret_cast<char*>(login) + kOffEditId);
    if (!idEdit) return false;
    const void* field = reinterpret_cast<char*>(idEdit) + kOffEditText;
    // Comparaison sur la LONGUEUR ANNONCÉE, pas jusqu'au zéro : le texte d'un
    // CUIEdit est une `std::string`, dont la SSO ne garantit rien après la
    // taille. Et l'égalité doit être STRICTE — un identifiant de joueur qui
    // commencerait par le nôtre n'est pas le nôtre.
    const size_t want = std::strlen(userid);
    if (rag::clientstr::Size(field) != want) return false;
    const char* text = rag::clientstr::Data(field);
    if (text == nullptr || std::strncmp(text, userid, want) != 0) return false;
    reinterpret_cast<SetText_t>(kSetTextAddr)(idEdit, "");
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
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
