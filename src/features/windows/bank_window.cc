#include "ragnarok/globals.h"
#include "features/windows/bank_window.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>    // std::floor (largeurs de colonne en pixels entiers)
#include <cstdio>
#include <cstring>

#include "bourgeon.h"              // Bourgeon::Instance().SendPacket / session
#include "d3d9/d3d9_hook.h"        // Overlay_DeviceEpoch (invalidation du fond)
#include "imgui.h"
#include "features/moonlight_ui/moonlight_ui.h"  // OpenInterfaceSection + HelpMarker
#include "ragnarok/uiwnd.h"        // uiwnd::FindWindow
#include "ui/game_texture.h"       // ro::TextureFromGameFile (fond bg_bank_moon.bmp)
#include "ui/ro_imgui.h"           // skin RO (BeginRoWindow / RoButton / RoCheckbox)

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000) ────────────────────────────
// Tout est détaillé dans docs/bank_zeny_re.md ; on ne recopie ici que ce qui sert.
namespace {

// Fenêtre native UIBank_NewWnd. On la retrouve par le GESTIONNAIRE puis on vérifie
// la vtable : un id ne garantit pas la classe si un portage renumérote les fenêtres.
constexpr int       kWinBank    = 275;         // 0x113
constexpr uintptr_t kBankVTable = 0x01030fd4;
constexpr int kOffWidth   = 0x14;
constexpr int kOffHeight  = 0x18;

// Globales des soldes. g_BankVault est un s64 côté client alors que le serveur
// stocke un int32 (MAX_BANK_ZENY = SINT32_MAX) : on lit bien les 8 octets, mais
// tous les plafonds restent à INT32_MAX.
constexpr uintptr_t kBankVault  = 0x015fffc0;  // s64
constexpr long long kZenyMax    = 2147483647LL;  // INT32_MAX = plafond client ET serveur

// Formateurs à séparateurs de milliers du client (ceux qu'emploie la fenêtre
// native) : 64 bits pour la banque, 32 bits pour les zeny en poche.
constexpr uintptr_t kFmtComma64 = 0x00a94870;
constexpr uintptr_t kFmtComma32 = 0x00a948d0;
using FmtComma64_t = char*(__cdecl*)(long long, char*, int);
using FmtComma32_t = char*(__cdecl*)(int, char*, int);

// MsgStringTable : source des libellés/erreurs du client (CP949).
constexpr uintptr_t kMsgStringGet = 0x00a9ed30;
using MsgStringGet_t = const char*(__cdecl*)(int);

// Ids MsgStringTable utilisés par la banque native (cf. docs/bank_zeny_re.md §7-8).
constexpr int kMsgDepositNoMoney  = 2456;  // MSI_BANK_DEPOSIT_NO_MONEY
constexpr int kMsgOverIntMax      = 2459;  // MSI_BANK_OVER_INT_MAX
constexpr int kMsgWithdrawNoMoney = 2455;  // MSI_BANK_WITHDRAW_NO_MONEY
constexpr int kMsgProhibit        = 2489;  // MSI_BANK_PROHIBIT
constexpr int kMsgMinMoney        = 2769;  // MSI_BANK_MIN_MONEY
constexpr int kMsgMaxMoney        = 2768;  // MSI_BANK_MAX_MONEY
constexpr int kMsgZeroMoney       = 2770;  // MSI_BANK_0_MONEY

// Fenêtres qui BLOQUENT dépôt/retrait côté client (RE Bank_IsBlockedByOpenWindow
// 0x00c86150) : refine, grade-enchant, enchant, barter étendu, runes.
// ⚠ Garde PUREMENT client : le serveur, lui, accepterait. On la reproduit pour
// rester fidèle au natif, pas parce qu'elle protège quoi que ce soit.
constexpr int kBlockingWindows[] = {290, 302, 348, 10006, 361};

// Opcodes. Voir docs/bank_zeny_re.md §2 pour les layouts (celui de 0x09A6 diffère).
constexpr uint16_t kOpDeposit  = 0x09A7;  // CZ_REQ_BANKING_DEPOSIT
constexpr uint16_t kOpWithdraw = 0x09A9;  // CZ_REQ_BANKING_WITHDRAW
constexpr uint16_t kOpCheck    = 0x09AB;  // CZ_REQ_BANKING_CHECK (= ce que fait Ctrl+B)

// OnMsg natif : rejouer le chemin du X plutôt que d'appeler nous-mêmes une
// fonction du gestionnaire (aucune convention d'appel à deviner).
constexpr int kMsgUiAction = 0x06;  // OnMsg : action de contrôle…
constexpr int kActionClose = 201;   // …201 = fermeture (RE UIBankWnd_OnMsg case 6)

// La fenêtre banque ouverte, ou nullptr. Le client DÉTRUIT ses fenêtres à la
// fermeture, donc non-nul == « ouverte en ce moment ».
uint8_t* BankWnd() {
  __try {
    auto* w = reinterpret_cast<uint8_t*>(uiwnd::FindWindow(kWinBank));
    if (!w) return nullptr;
    if (*reinterpret_cast<uintptr_t*>(w) != kBankVTable) return nullptr;
    return w;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Ferme la banque comme le X natif : OnMsg(6, 201) -> SaveRectAndCloseWindow(275).
void CloseBank() {
  uint8_t* wnd = BankWnd();
  if (!wnd) return;
  __try {
    uiwnd::OnMsg(wnd, kMsgUiAction, kActionClose, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Position/taille de la native (pour poser la nôtre à côté). Renvoie false si
// illisible.
struct NativeRect { int x = 0, y = 0, w = 0, h = 0; };
bool ReadNativeRect(uint8_t* wnd, NativeRect* out) {
  __try {
    out->x = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosX);
    out->y = *reinterpret_cast<int*>(wnd + uiwnd::kOffPosY);
    out->w = *reinterpret_cast<int*>(wnd + kOffWidth);
    out->h = *reinterpret_cast<int*>(wnd + kOffHeight);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void SetNativeVisible(uint8_t* wnd, bool visible) {
  __try {
    uiwnd::SetVisible(wnd, visible);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Masque `win` APRÈS avoir vérifié sa vtable. Le hook MakeWindow nous passe la
// fenêtre par son seul id : on ne peut pas encore la retrouver par le gestionnaire
// (elle n'y est pas enregistrée à cet instant), donc c'est la vtable qui sert de
// contrôle de classe. Renvoie true si on l'a bien masquée.
bool HideIfBankWindow(void* win) {
  __try {
    if (*reinterpret_cast<uintptr_t*>(win) != kBankVTable) return false;
    uiwnd::SetVisible(win, false);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Soldes. Lecture SEH isolée : OnTick/OnRenderUI manipulent des objets C++ et ne
// peuvent pas héberger de __try (C2712).
struct Balances { long long vault = 0; int zeny = 0; };
Balances ReadBalances() {
  Balances b;
  __try {
    b.vault = *reinterpret_cast<long long*>(kBankVault);
    b.zeny  = *reinterpret_cast<int*>(rag::kZenyAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return b;
}

// Une des fenêtres bloquantes est-elle ouverte ? (miroir de Bank_IsBlockedByOpenWindow)
bool TransferBlocked() {
  __try {
    for (int id : kBlockingWindows)
      if (uiwnd::FindWindow(id) != nullptr) return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  return false;
}

// Formatage à séparateurs de milliers par les fonctions DU CLIENT : la banque
// affiche donc exactement les mêmes nombres que la fenêtre native.
const char* Grouped64(long long value, char* buf, int size) {
  buf[0] = '\0';
  __try {
    reinterpret_cast<FmtComma64_t>(kFmtComma64)(value, buf, size);
  } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
  return buf;
}
const char* Grouped32(int value, char* buf, int size) {
  buf[0] = '\0';
  __try {
    reinterpret_cast<FmtComma32_t>(kFmtComma32)(value, buf, size);
  } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = '\0'; }
  return buf;
}

// Libellé natif (CP949), ou nullptr. L'appelant convertit en UTF-8.
const char* MsgStringCp949(int id) {
  __try {
    return reinterpret_cast<MsgStringGet_t>(kMsgStringGet)(id);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Paquet de transfert, tel qu'il part sur le fil (10 octets, cf. §2 de la doc).
#pragma pack(push, 1)
struct BankTransferWire {
  uint16_t opcode;
  uint32_t aid;
  uint32_t amount;
};
#pragma pack(pop)
static_assert(sizeof(BankTransferWire) == 10,
              "CZ_REQ_BANKING_DEPOSIT/WITHDRAW font 10 octets");

// Demande d'ouverture (6 octets) : le serveur repond par ZC_BANKING_CHECK 0x09A6.
#pragma pack(push, 1)
struct BankCheckWire {
  uint16_t opcode;
  uint32_t aid;
};
#pragma pack(pop)
static_assert(sizeof(BankCheckWire) == 6, "CZ_REQ_BANKING_CHECK fait 6 octets");

// Montants maximaux transférables, mêmes formules que UIBankWnd_ValidateAmount en
// mode MAX : le plafond INT32 s'applique à la DESTINATION.
long long MaxDeposit(long long vault, int zeny) {
  const long long room = kZenyMax - vault;
  return (std::max)(0LL, (std::min)(static_cast<long long>(zeny), room));
}
long long MaxWithdraw(long long vault, int zeny) {
  const long long room = kZenyMax - zeny;
  return (std::max)(0LL, (std::min)(vault, room));
}

inline long long ClampAmount(long long v) {
  return (std::max)(0LL, (std::min)(v, kZenyMax));
}

// ── Fond de fenêtre : le bg_bank.bmp du serveur ───────────────────────────────
// « 유저인터페이스\ » en CP949, octets verbatim : la source est en UTF-8 alors que
// le TexMgr attend du CP949. ⚠ NE PAS concaténer un littéral qui commence par un
// caractère hexadécimal juste après « \xBA » — l'échappement hex est glouton en
// C++ ; on passe donc toujours par un %s de snprintf.
constexpr char kUiDirCp949[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";
// Nom DISTINCT du bg_bank.bmp d'origine : la native garde le sien intact — elle est
// masquée, pas détruite, et redevient visible dès que le groupe « Interface
// moderne » repasse à OFF. Le fond retaillé pour l'ImGui vit donc à côté, sans
// écraser l'art du client.
constexpr char kBgRelPath[]  = "bank\\bg_bank_moon.bmp";

// Géométrie MESURÉE sur le bg_bank_moon.bmp de Moonlight (345 x 187) :
//   · sac de zeny cuit dans l'image : x 7..35, y 93..123
//   · bande grise « ligne montant » : y 86..133 (bordure 198,198,198 aux deux bouts)
// Le champ de saisie démarre donc après le sac, qui lui sert de libellé, et le
// reste de la bande accueille le message d'état.
constexpr float kBagRightPx    = 35.0f;
constexpr float kAmountInputW  = 120.0f;

ro::GameTexture g_bg;             // fond, chargé paresseusement
bool     g_bg_tried = false;
unsigned g_bg_epoch = 0;

// Le fond, ou une texture nulle s'il est absent du GRF. Rechargé quand le device
// D3D a été reset (les textures vivent en D3DPOOL_DEFAULT : leurs handles meurent).
const ro::GameTexture& BackgroundTexture() {
  const unsigned epoch = Overlay_DeviceEpoch();
  if (epoch != g_bg_epoch) {
    g_bg = ro::GameTexture{};
    g_bg_tried = false;
    g_bg_epoch = epoch;
  }
  if (!g_bg_tried) {
    g_bg_tried = true;
    char full[128];
    std::snprintf(full, sizeof(full), "%s\\%s", kUiDirCp949, kBgRelPath);
    g_bg = ro::TextureFromGameFile(full);
  }
  return g_bg;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

void BankWindow::SetStatus(const char* utf8, bool is_error) {
  std::snprintf(status_, sizeof(status_), "%s", utf8 ? utf8 : "");
  status_error_ = is_error;
}

void BankWindow::SetStatusFromMsgString(int msg_id, bool is_error) {
  const char* cp949 = MsgStringCp949(msg_id);
  // Les libellés du client sont en CP949 ; ImGui veut de l'UTF-8. Le buffer rendu
  // par Cp949ToUtf8 est rotatif -> on RECOPIE tout de suite.
  SetStatus(cp949 ? ro::Cp949ToUtf8(cp949) : nullptr, is_error);
}

void BankWindow::OnTick() {
  open_ = false;
  uint8_t* wnd = BankWnd();
  if (wnd) {
    open_ = true;
    if (!was_open_) {
      // 1re ouverture : on se pose là où la native se serait ouverte (elle est
      // masquée, mais son rect reste renseigné).
      NativeRect r;
      if (ReadNativeRect(wnd, &r)) {
        spawn_x_ = r.x + r.w + 8;
        spawn_y_ = r.y;
      }
      need_pos_ = true;
      amount_ = 0;
      status_[0] = '\0';
      status_error_ = false;
    }
    // La native est masquée tant que le viewer la remplace. On ne la REMET visible
    // qu'une seule fois, quand le groupe « Interface moderne » repasse à OFF alors
    // que c'est nous qui l'avions baissée : forcer le flag à 1 à chaque tick se
    // battrait avec le « tout masquer » natif (behavior 116).
    const bool want_hidden = imgui_enabled_;
    if (want_hidden) {
      SetNativeVisible(wnd, false);
      native_hidden_ = true;
    } else if (native_hidden_) {
      SetNativeVisible(wnd, true);
      native_hidden_ = false;
    }
  } else {
    native_hidden_ = false;  // fenêtre détruite : le prochain create repart neuf
  }
  was_open_ = open_;

  const Balances balances = ReadBalances();
  vault_ = balances.vault;
  zeny_  = balances.zeny;
}

void BankWindow::HideNativeAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  // OnTick ne remettra la native visible qu'une fois, et seulement si c'est bien
  // nous qui l'avions baissée : on le lui dit dès maintenant.
  if (HideIfBankWindow(win)) native_hidden_ = true;
}

void BankWindow::ToggleFromUi() {
  // Anti-double-clic : 0x09A6 BASCULE la fenêtre, donc deux demandes coup sur coup
  // l'ouvriraient puis la refermeraient.
  const unsigned long now = GetTickCount();
  if (last_toggle_tick_ != 0 && now - last_toggle_tick_ < 400) return;
  last_toggle_tick_ = now;

  // Déjà ouverte : on ferme côté client, sans paquet — exactement ce que fait le
  // raccourci natif quand g_pUIBankWnd est non nul.
  if (BankWnd()) { CloseBank(); return; }

  const uint32_t aid = Bourgeon::Instance().client().session().aid();
  if (aid == 0) return;
  BankCheckWire pkt{};
  pkt.opcode = kOpCheck;
  pkt.aid    = aid;
  Bourgeon::Instance().SendPacket(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
}

void BankWindow::SendTransfer(bool deposit) {
  // Anti-double-envoi (double-clic sur le bouton).
  const unsigned long now = GetTickCount();
  if (last_send_tick_ != 0 && now - last_send_tick_ < 300) return;

  // Revalidation client, dans le même ordre que UIBankWnd_OnMsg / ValidateAmount.
  // Le serveur revalide de toute façon ; ici on évite juste un aller-retour et on
  // affiche le MÊME libellé que la fenêtre native.
  if (TransferBlocked())      { SetStatusFromMsgString(kMsgProhibit, true);  return; }
  if (amount_ <= 0)           { SetStatusFromMsgString(kMsgMinMoney, true);  return; }
  if (amount_ > kZenyMax)     { SetStatusFromMsgString(kMsgMaxMoney, true);  return; }

  if (deposit) {
    if (zeny_ <= 0)              { SetStatusFromMsgString(kMsgZeroMoney, true); return; }
    if (amount_ > zeny_)         { SetStatusFromMsgString(kMsgDepositNoMoney, true); return; }
    if (vault_ + amount_ > kZenyMax) { SetStatusFromMsgString(kMsgOverIntMax, true); return; }
  } else {
    if (vault_ <= 0)             { SetStatusFromMsgString(kMsgZeroMoney, true); return; }
    if (amount_ > vault_)        { SetStatusFromMsgString(kMsgWithdrawNoMoney, true); return; }
    if (zeny_ + amount_ > kZenyMax) { SetStatusFromMsgString(kMsgOverIntMax, true); return; }
  }

  const uint32_t aid = Bourgeon::Instance().client().session().aid();
  if (aid == 0) return;

  BankTransferWire pkt{};
  pkt.opcode = deposit ? kOpDeposit : kOpWithdraw;
  pkt.aid    = aid;
  pkt.amount = static_cast<uint32_t>(amount_);
  Bourgeon::Instance().SendPacket(reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  last_send_tick_ = now;

  // Le natif vide son champ dès la validation : on fait pareil. La confirmation
  // arrive par ZC 0x09A8/0x09AA, que le handler natif applique aux globales — nos
  // soldes se remettent donc à jour tout seuls au tick suivant.
  amount_ = 0;
  SetStatus(deposit ? "Dépôt envoyé..." : "Retrait envoyé...", false);
}

void BankWindow::OnRenderUI() {
  if (!open_ || !imgui_enabled_) return;

  if (need_pos_) {
    ImGui::SetNextWindowPos(ImVec2(static_cast<float>(spawn_x_),
                                   static_cast<float>(spawn_y_)),
                            ImGuiCond_FirstUseEver);
    need_pos_ = false;
  }
  // Taille NON redimensionnable : largeur imposée, hauteur auto-ajustée à chaque
  // frame (SetNextWindowSize avec y <= 0 => auto-fit permanent sur cet axe).
  //
  // La largeur est celle du FOND quand il est présent : bg_bank.bmp a été redessiné
  // à la taille exacte de cette fenêtre, c'est donc lui qui commande — le blit est
  // 1:1, sans étirement, et les bandes tombent pile sur les lignes de widgets.
  // Sans fond (bmp absent du GRF), on retombe sur une largeur dérivée de la police :
  // le facteur 23 est le rapport largeur/corps-de-police du bitmap (345 / 15).
  const ro::GameTexture& bg = BackgroundTexture();
  const float window_w = bg.tex ? static_cast<float>(bg.w)
                               : (std::max)(300.0f, ImGui::GetFontSize() * 23.0f);
  ImGui::SetNextWindowSize(ImVec2(window_w, 0.0f), ImGuiCond_Always);

  ro::SetNextWindowTitleBullet("Options de la banque");
  // Ni poignée de redimensionnement ni bouton « réduire » : la fenêtre n'a qu'une
  // taille utile, et un repli n'apporterait rien sur trois lignes de contenu.
  const bool begun = ro::BeginRoWindow(
      "Banque###bourgeon_bank", &show_panel_,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
  if (ro::TitleBulletClicked())
    if (auto* mu = Bourgeon::Instance().moonlight_ui())
      mu->OpenInterfaceSection(MoonlightUi::kIfaceBank);
  // X du viewer -> ferme la banque native (les deux fenêtres partent ensemble).
  if (!show_panel_) { CloseBank(); show_panel_ = true; }
  if (!begun) { ro::EndRoWindow(); return; }

  // Fond blitté 1:1, ancré en HAUT du corps (juste sous la barre de titre RO).
  // Ancré en haut et non étiré : si la fenêtre grandit (message d'état sur une
  // ligne de plus), c'est le BAS qui déborde du bitmap — les bandes du haut, elles,
  // ne bougent pas d'un pixel. Un étirement, lui, décalerait tout.
  if (bg.tex) {
    const ImVec2 win_pos = ImGui::GetWindowPos();
    const float body_top =
        ImGui::GetCursorScreenPos().y - ImGui::GetStyle().WindowPadding.y;
    // ⚠ ro::SkinImageTint() et NON une teinte reconstruite ici : le helper
    // multiplie aussi par ImGui::GetStyle().Alpha. Une teinte à alpha = 1 en dur
    // laissait ce fond OPAQUE pendant que le reste de l'UI s'estompait.
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(bg.tex)),
        ImVec2(win_pos.x, body_top),
        ImVec2(win_pos.x + bg.w, body_top + bg.h), ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f), ro::SkinImageTint());
  }

  // Relecture par FRAME (et pas seulement au tick de 100 ms) : après un dépôt, le
  // solde doit bouger dès que le ZC d'acquittement est traité, sans latence visible.
  const Balances live = ReadBalances();
  vault_ = live.vault;
  zeny_  = live.zeny;

  char buf[64];

  // ── Soldes ────────────────────────────────────────────────────────────────
  // Alignés sur une colonne pour que les montants se lisent d'un bloc, ce que le
  // natif ne fait pas (il colle le nombre juste après le libellé). La colonne est
  // calée sur le PLUS LONG libellé pour qu'aucun ne morde sur les chiffres.
  const float value_col =
      ImGui::CalcTextSize("Montant").x + ImGui::GetFontSize() * 1.5f;

  ImGui::TextUnformatted("Banque");
  ImGui::SameLine(value_col);
  ImGui::Text("%s z", Grouped64(vault_, buf, sizeof(buf)));

  ImGui::TextUnformatted("Poche");
  ImGui::SameLine(value_col);
  ImGui::Text("%s z", Grouped32(zeny_, buf, sizeof(buf)));

  {
    // 🔴 INCONDITIONNEL, et ce n'est pas un oubli : le fond de cette fenêtre est
    // un bitmap du client pré-composé à hauteur FIXE. Un contenu qu'on peut
    // masquer ferait glisser tout ce qui suit hors du fond peint. Cf. DrawSettings.
    const long long total = vault_ + static_cast<long long>(zeny_);
    ImGui::TextUnformatted("Total");
    ImGui::SameLine(value_col);
    ImGui::Text("%s z", Grouped64(total, buf, sizeof(buf)));
    SameLine(); HelpMarker(
        "La banque est plafonnée à 2 147 483 647 z (INT32), côté client comme "
        "côté serveur (MAX_BANK_ZENY). Un dépôt qui ferait dépasser ce plafond "
        "est refusé en bloc, pas tronqué.");
    // Jauge de remplissage de la banque vis-à-vis du plafond INT32 : le natif ne
    // dit rien tant qu'on ne bute pas dessus, et l'erreur tombe alors sans prévenir.
    const float ratio = static_cast<float>(static_cast<double>(vault_) /
                                           static_cast<double>(kZenyMax));
    std::snprintf(buf, sizeof(buf), "%.1f %% du plafond", ratio * 100.0f);
    // Hauteur 0 = hauteur de cadre standard (police + 2×FramePadding.y) : c'est la
    // seule qui laisse ImGui centrer verticalement le texte du calque. Une barre
    // plus fine le rogne par le bas — elle est dessinée avant le texte, mais le
    // clip, lui, est celui de la barre.
    ImGui::ProgressBar(ratio, ImVec2(-1.0f, 0.0f), buf);
  }

  Separator();

  // ── Montant (la bande grise du fond) ──────────────────────────────────────
  // Avec le fond, le sac de zeny CUIT dans l'image tient lieu de libellé : on ne
  // écrit pas « Montant » par-dessus, et le champ démarre juste après lui. Sans
  // fond, on retombe sur un libellé texte et un champ pleine largeur.
  const bool blocked = TransferBlocked();
  // Colonne des montants : le sac occupe le début de la bande, donc TOUT ce qui
  // porte un montant (le champ ET la rangée d'incréments) démarre après lui, sur la
  // même verticale. Une seule variable pour les deux — sinon ils dérivent.
  const float amount_col_x = kBagRightPx + ImGui::GetStyle().ItemSpacing.x;
  if (bg.tex) {
    ImGui::SetCursorPosX(amount_col_x);
    ImGui::SetNextItemWidth(kAmountInputW);
  } else {
    ImGui::TextUnformatted("Montant");
    ImGui::SameLine(value_col);
    ImGui::SetNextItemWidth(-1.0f);
  }
  if (ImGui::InputScalar("##bank_amount", ImGuiDataType_S64, &amount_, nullptr,
                         nullptr, "%lld", ImGuiInputTextFlags_CharsDecimal))
    amount_ = ClampAmount(amount_);

  // Message d'état dans la partie libre de la bande, à DROITE du champ. C'est là
  // que le natif l'affiche aussi (UIBankWnd_DrawContent, y=112), et surtout ça
  // garde la hauteur de la fenêtre constante — donc le fond reste aligné. Pas de
  // retour à la ligne : un texte trop long est simplement rogné au bord.
  const char* banner = nullptr;
  bool banner_is_error = false;
  if (blocked) {
    // Libellé EXACT du client (MSI_BANK_PROHIBIT), pas une paraphrase à nous.
    const char* cp949 = MsgStringCp949(kMsgProhibit);
    if (cp949) { banner = ro::Cp949ToUtf8(cp949); banner_is_error = true; }
  } else if (status_[0]) {
    banner = status_;
    banner_is_error = status_error_;
  }
  if (banner) {
    SameLine();
    ImGui::TextColored(banner_is_error ? ImVec4(0.85f, 0.15f, 0.15f, 1.0f)
                                       : ImVec4(0.15f, 0.55f, 0.20f, 1.0f),
                       "%s", banner);
  }

  const long long max_deposit  = MaxDeposit(vault_, zeny_);
  const long long max_withdraw = MaxWithdraw(vault_, zeny_);

  {
    // Inconditionnel pour la même raison que le bloc « Total » plus haut : la
    // hauteur du contenu doit rester constante, le fond bitmap ne s'adapte pas.
    // Les incréments du natif (btn_10mil/100mil/1000mil valent en réalité 1e5/1e6/1e7)
    // plus les paliers intermédiaires. Pas de « +1k » : Moonlight est un serveur
    // high rate, mille zeny n'y représente rien.
    struct Step { const char* label; long long value; };
    static constexpr Step kSteps[] = {
        {"+10k",  10000LL},     {"+100k", 100000LL},
        {"+1M",   1000000LL},   {"+10M",  10000000LL},  {"+100M", 100000000LL},
    };
    const ImGuiStyle& style = ImGui::GetStyle();
    // Alignée à gauche sur le CHAMP, pas sur le bord du corps : la colonne des
    // montants est la même du haut en bas de la bande.
    if (bg.tex) ImGui::SetCursorPosX(amount_col_x);
    // GetContentRegionAvail part du curseur : la rangée a perdu la largeur du sac,
    // et les boutons doivent rentrer dans ce qui reste. Plutôt que de figer une
    // largeur, on prend l'espacement standard TANT QU'IL TIENT et on le resserre
    // seulement si le plus large libellé déborderait de sa case. La rangée reste
    // donc correcte même si la police grossit (le fond, lui, impose 345 px).
    // `GetFontSize()` approxime les coiffes gauche+droite de l'art sbtn_* (~12 px
    // au réglage par défaut) : c'est la part de la case que le texte n'a pas.
    const int   count = IM_ARRAYSIZE(kSteps);
    float widest_label = 0.0f;
    for (const Step& step : kSteps)
      widest_label = (std::max)(widest_label, ImGui::CalcTextSize(step.label).x);
    const float needed = (widest_label + ImGui::GetFontSize()) * count;
    const float avail = ImGui::GetContentRegionAvail().x;
    float gap = style.ItemSpacing.x;
    if (needed + gap * (count - 1) > avail)
      gap = (std::max)(0.0f, (avail - needed) / (count - 1));
    // Entiers obligatoires : un espacement ou une largeur fractionnaire décale les
    // boutons d'un demi-pixel et le 3-slice se met à baver au raccord des tranches.
    gap = std::floor(gap);
    const float step_w = std::floor((avail - gap * (count - 1)) / count);
    for (int i = 0; i < count; ++i) {
      if (i) ImGui::SameLine(0.0f, gap);
      if (ro::RoSmallButton(kSteps[i].label, step_w))
        amount_ = ClampAmount(amount_ + kSteps[i].value);
    }
  }

  // ── Deux colonnes, en pixels ENTIERS ──────────────────────────────────────
  // Elles portent les deux rangées du bas : les remplissages « tout » (ce que le
  // bouton MAX natif calcule, mais explicité en deux boutons — le natif n'en a
  // qu'un, dont le sens dépend du bouton cliqué ensuite) puis les actions.
  //
  // (avail - gap) / 2 tombe sur un DEMI-PIXEL dès que la différence est impaire
  // (329 - 8 = 321 -> 160,5) : le 3-slice des boutons se retrouve alors
  // échantillonné à cheval sur deux texels — bords baveux et couture visible au
  // raccord des trois tranches. On plancherise donc la largeur de colonne, et tout
  // le reste (origine de ligne, espacement) est déjà entier.
  const float col_gap = ImGui::GetStyle().ItemSpacing.x;
  const float col_w =
      std::floor((ImGui::GetContentRegionAvail().x - col_gap) * 0.5f);

  // ⚠ La colonne de DROITE est posée par SetCursorScreenPos, PAS par SameLine :
  // ro::RoSmallButton se resserre de 3 px quand il est placé en SameLine (l'art a
  // déjà sa marge), alors que ro::RoButton ne le fait pas. Avec SameLine, la rangée
  // « Tout … » et la rangée d'actions se décalaient donc de 3 px l'une par rapport à
  // l'autre. Un placement absolu les met sur la même colonne, quel que soit le
  // widget — et comme IsSameLine est déjà retombé, le resserrage ne s'applique plus.
  ImVec2 row = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(max_deposit <= 0);
  if (ro::RoSmallButton("Tout déposer", col_w)) amount_ = max_deposit;
  ImGui::EndDisabled();
  if (max_deposit > 0 && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s z", Grouped64(max_deposit, buf, sizeof(buf)));
  ImGui::SetCursorScreenPos(ImVec2(row.x + col_w + col_gap, row.y));
  ImGui::BeginDisabled(max_withdraw <= 0);
  if (ro::RoSmallButton("Tout retirer", col_w)) amount_ = max_withdraw;
  ImGui::EndDisabled();
  if (max_withdraw > 0 && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s z", Grouped64(max_withdraw, buf, sizeof(buf)));

  Separator();

  // ── Actions ───────────────────────────────────────────────────────────────
  // Le natif garde ses boutons cliquables et ne dit « non » qu'après coup ; ici on
  // grise ce qui ne peut pas marcher, et la raison s'affiche dans la bande.
  const bool can_deposit  = !blocked && amount_ > 0 && amount_ <= max_deposit;
  const bool can_withdraw = !blocked && amount_ > 0 && amount_ <= max_withdraw;

  // Mêmes colonnes que la rangée « Tout … » ci-dessus, au pixel près.
  row = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!can_deposit);
  if (ro::RoButton("Déposer", col_w)) SendTransfer(true);
  ImGui::EndDisabled();
  ImGui::SetCursorScreenPos(ImVec2(row.x + col_w + col_gap, row.y));
  ImGui::BeginDisabled(!can_withdraw);
  if (ro::RoButton("Retirer", col_w)) SendTransfer(false);
  ImGui::EndDisabled();

  // (Les messages — refus et confirmations — sont rendus plus haut, dans la bande
  // grise à droite du champ Montant : ils n'ajoutent donc aucune ligne ici, ce qui
  // garde la hauteur de la fenêtre alignée sur le bitmap de fond.)

  ro::EndRoWindow();
}

bool BankWindow::DrawSettings() {
  bool changed = false;

  // Fenêtre membre du groupe « Interface moderne » (tout-ImGui ou tout-natif, plus
  // de mixe) : `SetModernInterface` écrit `imgui_enabled_` avec les autres. Ce qui
  // suit ne dit que ce que la bascule change pour la banque.
  // 🔴 Plus de CASE ici (cf. skill_bar.cc et moonlight_ui.h) : l'interrupteur du
  // groupe est unique, en tête de « Interface de jeu ». On garde la DESCRIPTION.
  ImGui::TextDisabled("Fenêtre du groupe « Interface moderne »");
  SameLine(); HelpMarker(
      "ON : banque ImGui au skin RO (fond du client, incréments rapides, "
      "« tout déposer / tout retirer », plafond INT32 affiché), et la fenêtre "
      "native est cachée.\n"
      "OFF (défaut) : banque native classique, aucun viewer.\n\n"
      "Dans les deux cas c'est le SERVEUR qui ouvre la banque : Ctrl+B (ou le "
      "bouton sac de zeny du footer de l'inventaire) lui envoie une demande.");

  // 🔴 AUCUN réglage de contenu ici, et c'est une contrainte de la fenêtre, pas un
  // manque d'idées.
  //
  // Cette banque n'est pas dessinée sur un fond ImGui : elle est peinte sur le
  // BITMAP du client, pré-composé et teinté (ro::SkinImageTint), donc à hauteur
  // FIXE. Tout élément qu'on pourrait masquer — la ligne « Total », la rangée
  // d'incréments — décalerait verticalement tout ce qui le suit et ferait sortir
  // les actions du fond peint. Le réglage n'aurait pas simplement « moins
  // d'options » comme effet : il casserait l'habillage.
  //
  // Les deux anciennes cases (`bank_quick_amounts`, `bank_show_total`) ont donc
  // été retirées, et leur contenu rendu PERMANENT. Une fenêtre dont la géométrie
  // est imposée par une image ne peut pas offrir de contenu optionnel — c'est le
  // prix du skin natif, et il est assumé.
  ImGui::Spacing();
  ImGui::TextDisabled(
      "La banque ne s'ouvre JAMAIS depuis le client : Ctrl+B envoie une demande "
      "et c'est le serveur qui l'ouvre. Cette fenêtre suit donc la native — il "
      "n'y a volontairement pas de bouton « ouvrir » ni « rafraîchir » (une "
      "demande de rafraîchissement refermerait la banque).");

  return changed;
}
