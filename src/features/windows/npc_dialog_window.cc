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
#include <utility>
#include <vector>

#include "bourgeon.h"        // Bourgeon::Instance().SendPacket
#include "features/link_gesture.h"        // links:: (gestes des balises maison)
#include "features/systems/bug_report.h"  // BugReport::NpcContext
#include "features/systems/image_preview.h"  // imgprev:: (<IMG> web)
#include "features/windows/chat_window.h"  // OwnsEnterKey (à qui appartient Entrée)
#include "d3d9/d3d9_hook.h"  // Overlay_CreateTextureARGB / Overlay_DeviceEpoch (icônes)
#include "imgui.h"
#include "ui/game_texture.h"  // ro::CachedTextureFromGameFile (<IMG> ressource client)
#include "ui/mob_sprite.h"    // ro::LoadMobSprite / DrawMobSprite (<MOBS>, <MOBP>)
#include "ui/ro_imgui.h"     // ro::BeginRoDescWindow (skin desc RO)
#include "ui/ro_widgets.h"   // ro::HelpMarker (section de réglages)
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

// ── Constantes RE (client 20250716, base 0x400000 ; cf. docs/npc_dialog_re.md) ──
namespace {

// UIWindowMgr + factory (SEH-gardé, comme npc_shop_window).

// Dispatcher CMode::SendMsg : *(rag::kActiveModePtr) = mode zone actif (ou 0). vtbl+0x18
// (= index 6) = CMode::SendMsg. cmd 0x28 = ferme l'UI NPC et DÉBLOQUE l'état
// dialogue CLIENT (CZ_CLOSE_DIALOG seul laisse le perso bloqué).
// (Les deux offsets de CGameMode — flag d'interaction +0x24C et GID +0x2DC — sont
// passés dans ragnarok/globals.h, avec leurs accesseurs : la boutique NPC les écrit
// désormais aussi.)
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

// Portée du clavier consommé par le dialogue (lue par EatsKey depuis le WndProc).
// Posées chaque frame par OnRenderUI : le WndProc tourne entre deux frames, il voit
// donc l'état de la frame qui vient d'être rendue.
bool g_kbd_dialog_open = false;  // overlay dialogue rendu cette frame
bool g_kbd_menu_open   = false;  // un menu de choix est affiché (flèches + 1-9 actifs)

// 🔴 ENTRÉE APPARTIENT D'ABORD À LA BARRE DE CHAT. Un `<ITEML>`/`<MOBL>` du script
// se relaie dans le chat d'un Maj+clic : le lien atterrit dans la saisie, et il
// faut pouvoir l'envoyer SANS fermer le script. Tant que la barre est armée — du
// texte dedans, ou le clavier — la touche est à elle, ici comme dans le WndProc
// (cf. ChatWindow::OwnsEnterKey). Barre vide et sans clavier, elle ne réclame
// rien et « Entrée = Suivant » vaut comme avant : confisquer la touche pour toute
// la durée d'un script coûterait bien plus qu'il ne rapporte.
//
// ⚠ N'engage QUE la touche Entrée. Espace continue de valider le bouton par
// défaut : le chat ne le réclame que lorsqu'il a le clavier, et ce cas-là est
// déjà couvert par `WantTextInput`.
bool ChatOwnsEnter() {
  const ChatWindow* chat = Bourgeon::Instance().chat_window();
  return chat != nullptr && chat->OwnsEnterKey();
}

// ── Fenêtres natives (SEH-gardé) ──
void* FindWnd(int id) { return uiwnd::SafeFindWindow(id); }
void CloseWnd(int id) { uiwnd::SafeCloseWindow(id); }
// (Plus de HideWnd/ShowWnd ici : ce plugin ne masque plus aucune native, il les
// détruit — cf. PurgeNativeDialogWindows.)

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
// Les trois accès à l'état « interaction NPC » du client (poser, effacer, relire le
// GID) vivent dans ragnarok/globals.h : la boutique NPC a exactement le même besoin
// depuis qu'elle remplace, elle aussi, le handler qui posait ce flag. Ce qui
// suivait ici en était la première copie ; la seconde n'aura pas lieu.
//
// 🔴 Pourquoi on les POSE au lieu de laisser tomber : cf. le commentaire de
// globals.h. Le flag est lu ailleurs dans le client, par des chemins non
// inventoriés.
using rag::ClearNpcInteractionActive;
using rag::NpcInteractionGid;
using rag::SetNpcInteractionActive;

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

// Les scripts NPC arrivent par le fil ; ImGui veut de l'UTF-8 (sinon les accents
// é/à/œ… cassent).
//
// ⚠ DÉLÈGUE à la porte commune, et ce n'est pas qu'un nettoyage : cette copie
// locale lisait en CP_ACP, la locale non-Unicode du POSTE, alors que l'encodage
// du fil est une propriété du SERVEUR (identique ici sur un Windows français,
// faux chez un joueur dont le système est réglé en coréen pour son client RO).
// Et `ro::WireToUtf8` accepte désormais les DEUX encodages, ce qui fait qu'un
// emoji dans un dialogue s'affiche au lieu de sortir en mojibake.
std::string AnsiToUtf8(const std::string& in) {
  if (in.empty()) return std::string();
  return std::string(ro::WireToUtf8(in.c_str()));
}

// ── Icônes item ImGui (recette inventory_viewer.cc) ──
// BuildItemIconGrfPath(id_str, out[128], identified) __stdcall -> chemin bmp ; TexMgr
// charge le bmp ; colorkey magenta -> alpha ; texture ImGui cachée (jetée au reset device).


inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

// ── Métrique du menu ────────────────────────────────────────────────────────
// Hauteur d'une option. Volontairement plus serrée qu'une ligne ImGui standard :
// un menu NPC est une liste, pas un formulaire. Une SEULE définition, parce que
// deux endroits la lisent — le dessin (DrawMenu) et le calcul de la hauteur du
// groupe (MenuNaturalHeight). Les voir diverger, c'est une boîte qui coupe sa
// dernière option ou qui garde une bande vide sous elle.
inline float MenuRowHeight() { return ImGui::GetFontSize() + 3.0f; }

// Au-delà, la liste scrolle au lieu de grandir : dix options tiennent à l'écran
// sans écraser le texte du NPC, et les menus de warp en comptent parfois trente.
constexpr size_t kMenuMaxRows = 10;
// Seuil d'apparition de la barre de recherche (options réellement affichées).
constexpr size_t kMenuFilterThreshold = 8;

// ── Médias : les bornes ─────────────────────────────────────────────────────
// Le contenu vient du SERVEUR, donc du staff — mais un chiffre mal tapé dans un
// script ne doit pas produire une image plein écran dans une fenêtre de 460 px.
// Ces plafonds sont ceux de l'AFFICHAGE ; ce qui protège le décodage vit dans
// imgprev (taille de téléchargement, dimensions, délai).
constexpr float kMediaMaxH     = 220.0f;  // hauteur max d'un média inline
constexpr float kSpriteInlineH = 48.0f;   // hauteur par défaut d'un `<MOBS>`
constexpr float kPortraitW     = 104.0f;  // largeur de la colonne du portrait
// Cadence d'animation des sprites : celle qu'emploie déjà la fiche de monstre.
constexpr float kSpriteMsPerFrame = 130.0f;

// Gris des cartouches d'état d'un média (« image introuvable », « chargement »).
// 🔴 PAS `ImGuiCol_TextDisabled` : le corps d'une fenêtre RO est CLAIR (skin
// 9-slice crème), et cette couleur, calibrée pour un thème sombre, y devient
// invisible. Gris sombre explicite, celui de la feuille de personnage.
constexpr uint32_t kNoticeColor = IM_COL32(90, 90, 107, 255);

// ── Le vocabulaire Bourgeon ──────────────────────────────────────────────────
//
// Les balises que le client ne connaît PAS, et qui sont donc à nous. La chatbox
// les parle déjà (`<MOBL>`, `<ITMR>`, `<CRAF>`, `<SETL>` — cf. chat_window.cc) ;
// ce fichier les fait parler au dialogue NPC, avec deux ajouts qui n'auraient
// aucun sens dans une ligne de log : `<IMG>` et `<MOBS>`/`<MOBP>`.
//
// 🔴 CE QU'ELLES COÛTENT À QUI NE LES REND PAS. Le dialogue NATIF efface les
// balises qu'il connaît et laisse passer les autres : un joueur resté en natif
// lirait donc `<MOBL>1002:0:Poring</MOBL>` en toutes lettres. C'est précisément ce
// que règle CZ_BOURGEON_UI_CAPS (features/systems/ui_caps.h) — le serveur sait qui
// les rend, et dégrade pour les autres. Le nom en clair transporté dans la balise
// est le deuxième filet : même brute, la ligne reste lisible.
//
// ⚠ Le nom d'un monstre VOYAGE, comme dans le chat, et pour la même raison : le
// client ne sait pas nommer un monstre (ni mob_db ni le paquet de sa fiche ne le
// lui donnent). Une balise qui ne porterait que l'id afficherait un numéro.
//
// Champs séparés par ':' et le champ libre EN DERNIER : un nom de monstre contient
// des espaces et des apostrophes, une adresse web contient des ':'.
//
//   <MOBL>id:rang:nom</MOBL>   lien monstre  (rang 0 normal / 1 boss / 2 MVP)
//   <ITMR>id:nom</ITMR>        lien objet de base (sans refine ni cartes)
//   <CRAF>id:nom</CRAF>        lien recette -> l'Atlas
//   <SETL>clé:libellé</SETL>   lien vers une destination du panneau Moonlight
//   <MOBS>id</MOBS>            sprite de monstre, inline dans le texte
//   <MOBP>id</MOBP>            portrait de PAGE (colonne de gauche)
//   <IMG>source</IMG>          image : ressource du client, ou adresse web
//   <IMG>w:h:source</IMG>      idem, taille imposée (en pixels)

const char* MobRankTag(int rank) {
  if (rank == 2) return "[MVP]";
  if (rank == 1) return "[Boss]";
  return "[Mob]";
}

std::string RecipeLinkLabel(const std::string& product_name) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), i18n::Tr("[Recette: %s]"), product_name.c_str());
  return buf;
}

// Recherche d'un motif dans [begin, end) : le corps d'un paquet n'est pas garanti
// nul-terminé à l'endroit où on le lit.
const char* SearchSub(const char* begin, const char* end, const char* needle) {
  const size_t n = std::strlen(needle);
  if (n == 0 || static_cast<size_t>(end - begin) < n) return nullptr;
  for (const char* p = begin; p + n <= end; ++p)
    if (std::memcmp(p, needle, n) == 0) return p;
  return nullptr;
}

// Le corps d'une balise `<TAG>…</TAG>` ouverte en `p`. Rend false si la balise
// n'est pas celle-là, ou si sa fermante manque — auquel cas rien n'est consommé
// et l'appelant laisse le texte tel quel plutôt que d'avaler la fin de la ligne.
bool TagBody(const char* p, const char* end, const char* open, const char* close,
             const char** body, const char** body_end, const char** after) {
  const size_t olen = std::strlen(open);
  if (static_cast<size_t>(end - p) < olen || std::memcmp(p, open, olen) != 0)
    return false;
  const char* b = p + olen;
  const char* c = SearchSub(b, end, close);
  if (c == nullptr) return false;
  *body = b;
  *body_end = c;
  *after = c + std::strlen(close);
  return true;
}

// Découpe `n` champs séparés par ':' — le DERNIER prend tout ce qui reste, ':'
// compris (nom de monstre, adresse web).
bool SplitFields(const char* b, const char* e, int n, std::string* out) {
  for (int i = 0; i < n - 1; ++i) {
    const char* colon = static_cast<const char*>(std::memchr(b, ':', e - b));
    if (colon == nullptr) return false;
    out[i].assign(b, colon);
    b = colon + 1;
  }
  out[n - 1].assign(b, e);
  return true;
}

bool AllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s)
    if (c < '0' || c > '9') return false;
  return true;
}

// Une source d'image désigne-t-elle le WEB ? Le seul discriminant dont on a
// besoin : tout le reste est un chemin de ressource du client.
bool IsWebSource(const std::string& s) {
  return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

// Place occupée par une image WEB dont on ne connaît pas encore les dimensions.
// Ce que le script écrit en `w:h:source` vaut toujours mieux : c'est la seule
// forme dont la mise en page ne bouge pas quand l'image arrive.
constexpr float kWebPlaceholderW = 160.0f;
constexpr float kWebPlaceholderH = 120.0f;

// À l'échelle, sans jamais déformer ni dépasser.
ImVec2 FitBox(float w, float h, float max_w, float max_h) {
  if (w <= 0.0f || h <= 0.0f) return ImVec2(0.0f, 0.0f);
  float s = 1.0f;
  if (w > max_w) s = max_w / w;
  if (h * s > max_h) s = max_h / h;
  return ImVec2(w * s, h * s);
}

// Le cartouche des états où il n'y a pas d'image à montrer mais quelque chose à
// DIRE (hôte non autorisé, fichier absent, échec). 🔴 On ne se tait pas : une
// image manquante muette est indiscernable d'un script qui ne l'a jamais posée,
// et c'est le staff qui écrit ces balises — il doit voir son erreur.
ImVec2 NoticeBox(float max_w, float line_h) {
  return ImVec2((max_w < 300.0f) ? max_w : 300.0f, line_h + 6.0f);
}

// La taille à RÉSERVER pour une image, avant même de savoir si elle arrivera.
//
// ⚠ DÉTERMINISTE pour un état donné, et c'est ce qui la rend sûre : le rendu la
// rappelle et compare au fragment déjà placé pour savoir si l'image web est
// arrivée entre-temps (auquel cas il redemande une mise en page). Deux appels dans
// le même état doivent donc rendre exactement la même chose, sinon la page se
// remettrait en page à chaque frame.
ImVec2 ImageBoxSize(const std::string& src, float want_w, float want_h,
                    float wrap, float line_h) {
  const float max_w = (wrap > 8.0f) ? wrap : 8.0f;
  // Taille imposée par le script : elle fait foi (bornée quand même — un zéro de
  // trop dans un script ne doit pas produire une image plein écran).
  if (want_w > 0.0f && want_h > 0.0f)
    return FitBox(want_w, want_h, max_w, kMediaMaxH);

  if (IsWebSource(src)) {
    if (!imgprev::IsPreviewable(src.c_str()))
      return NoticeBox(max_w, line_h);  // hôte hors liste : on propose, on n'impose pas
    const imgprev::Preview p = imgprev::Get(src.c_str());  // sans effet de bord
    if (p.state == imgprev::Preview::kReady && p.w > 0 && p.h > 0)
      return FitBox(static_cast<float>(p.w), static_cast<float>(p.h), max_w, kMediaMaxH);
    if (p.state == imgprev::Preview::kFailed) return NoticeBox(max_w, line_h);
    return FitBox(kWebPlaceholderW, kWebPlaceholderH, max_w, kMediaMaxH);
  }

  // Ressource du client : le chargement est synchrone et mémorisé (échec compris),
  // donc la taille est connue dès la mise en page.
  const ro::GameTexture t = ro::CachedTextureFromGameFile(src.c_str());
  if (t.tex != nullptr && t.w > 0 && t.h > 0)
    return FitBox(static_cast<float>(t.w), static_cast<float>(t.h), max_w, kMediaMaxH);
  return NoticeBox(max_w, line_h);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────

NpcDialogWindow::NpcDialogWindow() {
  // ── Les paquets de dialogue : on prend la PLACE du handler natif ─────────────
  //
  // 🔴 REMPLACEMENT, plus observation. En observant, les cinq fenêtres natives
  // naissaient à chaque `mes` et on les masquait juste après ; or une native
  // masquée garde le CLAVIER, et son bouton par défaut se déclenche à la touche
  // Entrée ou Espace. C'est exactement ce qui détruisait une arme sur le refine
  // (docs/weapon_refine_re.md §10), et ici le pansement coûtait deux détours : la
  // destruction de la fenêtre menu après chaque choix, et le filtre
  // ShouldSuppressNativeDialogSend, qui JETAIT les CZ émis dans notre dos par une
  // fenêtre qu'on croyait éteinte. Aucune de ces fenêtres ne naît plus.
  //
  // Le prédicat est relu à chaque paquet et vaut exactement ce qui gate notre
  // overlay : toggle coupé, le dialogue natif reprend la main à l'octet près.
  //
  // ⚠ Le PARSEUR ne change pas d'un régime à l'autre : les deux transmettent les
  // octets qui suivent l'opcode (+2). Un paquet variable commence donc toujours
  // par son champ de longueur, un paquet fixe par son GID — ce que le switch
  // d'OnRecvPacket lit déjà. Seul `len` grandit : il vaut désormais le paquet
  // entier au lieu des quelques octets qu'on se faisait forwarder.
  auto& b = Bourgeon::Instance();
  const auto claim = [this] { return imgui_enabled_; };
  b.RegisterReplaceOpcode(kZcSay,   claim);  // {len:2, GID:4, texte...}     VAR
  b.RegisterReplaceOpcode(kZcSay2,  claim);  // {len:2, GID:4, resv:1, txt}  VAR
  b.RegisterReplaceOpcode(kZcMenu,  claim);  // {len:2, GID:4, "a:b:c"...}   VAR
  b.RegisterReplaceOpcode(kZcWait,  claim);  // {GID:4}                      fixe
  b.RegisterReplaceOpcode(kZcWait2, claim);  // {GID:4}                      fixe
  b.RegisterReplaceOpcode(kZcClose, claim);  // {GID:4}                      fixe
  b.RegisterReplaceOpcode(kZcEditN, claim);  // {GID:4} -> prompt nombre     fixe
  b.RegisterReplaceOpcode(kZcEditS, claim);  // {GID:4} -> prompt texte      fixe
  b.RegisterReplaceOpcode(kZcClear, claim);  // efface le texte              fixe
  // Ces trois-là restent OBSERVÉS : leur handler natif fait un travail qui n'est
  // pas le nôtre (le nom du NPC alimente aussi les nameplates ; le changement de
  // map reconstruit tout le HUD). On ne fait que les lire au passage.
  b.RegisterObserveOpcode(kZcNpcName, 32);// gid:4 + groupId:4 + name[24]
  b.RegisterObserveOpcode(kZcMapChange, 4);
  b.RegisterObserveOpcode(kZcServerMove, 4);
}

void NpcDialogWindow::Reset() {
  lines_.clear();
  menu_opts_.clear();
  pending_lines_.clear();
  pending_replaces_ = false;
  last_dialog_ms_ = GetTickCount();
  scroll_top_ = false;
  frags_.clear();
  targets_.clear();
  img_srcs_.clear();
  portrait_mob_ = 0;
  link_menu_request_ = false;
  layout_wrap_ = layout_font_ = -1.0f;
  layout_h_ = 0.0f;
  ++page_gen_;
  has_next_ = has_close_ = false;
  start_fresh_ = true;
  input_mode_ = kInputNone;
  gid_ = 0;
  num_buf_[0] = str_buf_[0] = menu_filter_[0] = '\0';
  menu_hot_ = -1;
  rendered_ = false;  // la prochaine conversation repart sans cadre vide
  awaiting_reply_ = false;
}

// Ajoute une ligne à la page en cours de RÉCEPTION — pas à celle qui est affichée.
// Cf. CommitPage : le corps ne s'étale plus au fil des paquets pour être raboté à
// l'arrivée du menu.
void NpcDialogWindow::PushText(const char* s) {
  if (!s) return;
  // Découpe sur les '\n' bruts (le natif fait pareil via preprocess_escapes).
  std::string cur;
  for (const char* p = s; *p; ++p) {
    if (*p == '\r') continue;
    if (*p == '\n') { pending_lines_.push_back(cur); cur.clear(); }
    else cur += *p;
  }
  pending_lines_.push_back(cur);
}

// Publie la page reçue. Appelée par le paquet qui la TERMINE — WAIT (`next`),
// CLOSE, MENU ou un prompt — donc dans le MÊME drain, donc dans la même frame que
// le menu : corps, liste de choix et boutons apparaissent d'un seul tenant, à leur
// taille définitive. C'est l'effet « sans couture » du natif.
void NpcDialogWindow::CommitPage() {
  // 🔴 La page tenue affichée pendant l'aller-retour serveur meurt ICI, et pas au
  // premier paquet de la réponse : l'effacer plus tôt escamotait le menu et laissait
  // le texte se ré-étaler sur toute la fenêtre le temps que la suite arrive — un
  // aller-retour de réarrangement pour rien. Elle reste donc entière, simplement
  // grisée (awaiting_reply_), jusqu'à l'instant où la suivante la remplace.
  if (awaiting_reply_) {
    awaiting_reply_ = false;
    menu_opts_.clear();
    menu_filter_[0] = '\0';
    menu_hot_ = -1;
    input_mode_ = kInputNone;
  }
  // Rien de neuf à publier : un terminateur sans `mes` (un `menu` qui suit un choix,
  // un `close` sec) garde la page affichée telle quelle — le natif aussi.
  if (pending_lines_.empty() && !pending_replaces_) return;
  if (pending_replaces_) {
    lines_.swap(pending_lines_);
    scroll_top_ = true;  // nouvelle page : on la lit par le HAUT, pas par sa fin
  } else {
    lines_.insert(lines_.end(), pending_lines_.begin(), pending_lines_.end());
  }
  pending_lines_.clear();
  pending_replaces_ = false;
  // Garde-fou anti-emballement, PAS un budget d'affichage. L'ancien plafond de 400
  // lignes tronquait pour de bon des pages légitimes : l'agent de warp, détail des
  // cartes activé, en écrit ~700 (trois donjons, monstre par monstre) — le début de
  // la liste, en-tête de la première carte compris, partait à la poubelle. Ça ne se
  // voyait pas tant qu'on déroulait automatiquement jusqu'en bas ; maintenant que la
  // page s'ouvre par le HAUT, c'est la première chose que le joueur lirait. Le coût
  // d'une longue page a par ailleurs changé de nature — mise en page calculée une
  // seule fois, rendu borné à la bande visible.
  constexpr size_t kMaxLines = 4000;
  if (lines_.size() > kMaxLines)
    lines_.erase(lines_.begin(), lines_.begin() + (lines_.size() - kMaxLines));
  ScanPageDirectives();
  ++page_gen_;  // invalide le cache de mise en page
}

// Ce qui vaut pour la PAGE et non pour une ligne. Aujourd'hui : le portrait.
//
// 🔴 Il ne peut pas être relevé par la mise en page, alors que c'est là que
// vivent toutes les autres balises. La mise en page se refait à chaque changement
// de largeur du corps — or c'est le portrait qui DÉCIDE de cette largeur (il
// occupe une colonne). L'y lire ferait dépendre la largeur d'un calcul qui dépend
// de la largeur. Ici, il est fixé une fois par page, avant tout rendu.
//
// Le DERNIER `<MOBP>` de la page gagne : un script qui en pose deux a changé d'avis
// en cours de route, et c'est sa dernière phrase qui décrit l'écran affiché.
void NpcDialogWindow::ScanPageDirectives() {
  portrait_mob_ = 0;
  for (const std::string& raw : lines_) {
    const char* p = raw.c_str();
    const char* end = p + raw.size();
    while (p < end) {
      const char* open = SearchSub(p, end, "<MOBP>");
      if (open == nullptr) break;
      const char* body = open + 6;
      const char* close = SearchSub(body, end, "</MOBP>");
      if (close == nullptr) break;
      const std::string id(body, close);
      if (AllDigits(id)) portrait_mob_ = std::atoi(id.c_str());
      p = close + 7;
    }
  }
}

// Fil RÉSEAU : on COPIE, rien de plus (cf. features/net_inbox.h). Les paquets de
// dialogue (SAY, MENU) sont à longueur ANNONCÉE et leur décodage lit le corps
// entier : PushAnnounced. Les autres sont fixes.
void NpcDialogWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
  if (!imgui_enabled_) return;
  switch (opcode) {
    case kZcSay:
    case kZcSay2:
    case kZcMenu:
      net_inbox_.PushAnnounced(opcode, data, len);
      break;
    default:
      net_inbox_.Push(opcode, data, len);
      break;
  }
}

// Fil PRINCIPAL : le décodage, rejoué à chaque frame, dans l'ordre d'arrivée.
// L'ordre compte ici plus qu'ailleurs — un dialogue est une CONVERSATION, et un
// SAY rejoué après le MENU qui le suit afficherait le mauvais écran.
void NpcDialogWindow::HandlePacket(uint16_t opcode, const uint8_t* data,
                                   uint16_t len) {
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

  // Le serveur PARLE : la page n'a pas fini d'arriver, l'échéance du filet
  // d'OnRenderUI est repoussée. (La page qu'on tenait affichée en attendant, elle,
  // n'est effacée qu'à la publication de la suivante — cf. CommitPage.)
  //
  // ⚠ Restreint à NOS opcodes : OnRecvPacket est appelé pour tous ceux qu'observe
  // n'importe quel plugin (ex. 0x8C8 ZC_NOTIFY_ACT du DPS meter), et l'un d'eux
  // pendant l'attente repousserait l'échéance à la place d'un vrai paquet.
  switch (opcode) {
    case kZcSay: case kZcSay2: case kZcWait: case kZcWait2: case kZcClose:
    case kZcMenu: case kZcEditN: case kZcEditS: case kZcClear:
      last_dialog_ms_ = GetTickCount();
      break;
    default:
      break;
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
      // Saut de page : la page en RÉCEPTION remplacera l'affichée à sa publication.
      if (start_fresh_) {
        pending_lines_.clear();
        pending_replaces_ = true;
        start_fresh_ = false;
      }
      if (textlen > 0) {
        std::string t(reinterpret_cast<const char*>(data + 6),
                      static_cast<size_t>(textlen));
        PushText(t.c_str());
      }
      has_next_ = has_close_ = false;  // (re)posés par le WAIT/CLOSE qui suit
      open_ = true;
      SetNpcInteractionActive(gid_);  // ce que le handler natif écrivait ici
      return;
    }
    case kZcSay2: {  // {len:2, GID:4, resv:1, texte(len-9)}
      if (len < 7) return;
      const uint16_t plen = *reinterpret_cast<const uint16_t*>(data);
      gid_ = *reinterpret_cast<const uint32_t*>(data + 2);
      const int textlen = static_cast<int>(plen) - 9;
      if (start_fresh_) {  // saut de page (cf. kZcSay)
        pending_lines_.clear();
        pending_replaces_ = true;
        start_fresh_ = false;
      }
      if (textlen > 0) {
        std::string t(reinterpret_cast<const char*>(data + 7),
                      static_cast<size_t>(textlen));
        PushText(t.c_str());
      }
      has_next_ = has_close_ = false;
      open_ = true;
      SetNpcInteractionActive(gid_);
      return;
    }
    case kZcWait:
    case kZcWait2:
      if (len >= 4) gid_ = *reinterpret_cast<const uint32_t*>(data);
      CommitPage();  // `next` TERMINE la page : texte et bouton d'un seul tenant
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
      CommitPage();        // `close` TERMINE la page
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
      // 🔴 Le menu TERMINE la page : on publie le corps MAINTENANT, dans la même
      // frame que la liste de choix. C'est tout l'objet de la mise en attente —
      // sinon le corps s'étalait pendant l'arrivée des `mes` puis se faisait raboter
      // ici, à l'instant même où le joueur commençait à lire.
      CommitPage();
      menu_opts_.clear();
      if (mlen > 0) {
        std::string m(reinterpret_cast<const char*>(data + 6),
                      static_cast<size_t>(mlen));
        // Le corps est une chaîne C : rAthena copie strlen+1 octets, donc mlen
        // INCLUT le NUL final. Sans troncature, une option vide en DERNIÈRE
        // position devient "\0" (non vide) et s'affiche en ligne blanche.
        const size_t nul = m.find('\0');
        if (nul != std::string::npos) m.resize(nul);
        // Séparateur RO = ':' (découpe fidèle au natif). NB : un ':' DANS un libellé
        // scinde l'option (limite RO côté serveur, le natif aussi) -> ne pas mettre de
        // ':' dans un nom d'option/map (à corriger dans la SQL).
        // 🔴 Les entrées VIDES ne comptent PAS dans l'index envoyé au serveur.
        // rAthena compte les options avec menu_countoptions() (script.cpp), qui
        // SAUTE les vides : sd->npc_menu = nombre d'options NON VIDES, et le
        // contrôle anti-triche de clif_parse_NpcSelectMenu est `select >
        // npc_menu`. L'octet du paquet est donc le RANG parmi les options
        // affichées. C'est le serveur qui le retraduit ensuite en position
        // ABSOLUE (compteur `total` de menu_countoptions) pour la valeur de
        // retour de select() — d'où la confusion : select("a","","b") RENVOIE 3
        // pour "b", mais le client ENVOIE 2.
        // Le rang est donc FIGÉ ici, à la réception : les entrées vides le font
        // avancer sans donner d'option, et le rendu n'a plus à le recompter (il
        // le comptait avant filtre, sous peine d'envoyer le mauvais choix).
        int rank = 0;
        size_t start = 0;
        while (start <= m.size()) {
          size_t sep = m.find(':', start);
          std::string opt = (sep == std::string::npos) ? m.substr(start)
                                                        : m.substr(start, sep - start);
          if (!opt.empty()) {
            MenuOption mo;
            mo.rank = ++rank;
            ParseLine(opt, &mo.runs);  // texte riche (^RRGGBB, ^i[id]…) en UTF-8
            std::string plain;         // même texte, à plat, pour la recherche
            for (const Run& r : mo.runs) plain += r.text;
            mo.search = LowerAscii(plain.c_str());
            menu_opts_.push_back(std::move(mo));
          }
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
      SetNpcInteractionActive(gid_);  // §5.1 : le recv natif du menu posait +0x24C
      return;
    }
    case kZcEditN:
      if (len >= 4) gid_ = *reinterpret_cast<const uint32_t*>(data);
      CommitPage();  // le prompt TERMINE la page (texte + champ d'un seul tenant)
      input_mode_ = kInputNumber;
      num_buf_[0] = '\0';
      input_need_focus_ = true;  // focus auto du champ à l'apparition
      open_ = true;
      return;
    case kZcEditS:
      if (len >= 4) gid_ = *reinterpret_cast<const uint32_t*>(data);
      CommitPage();  // idem kZcEditN
      input_mode_ = kInputString;
      str_buf_[0] = '\0';
      input_need_focus_ = true;  // focus auto du champ à l'apparition
      open_ = true;
      return;
    case kZcClear:
      // Effacement PARESSEUX : on arme start_fresh_ au lieu de vider tout de suite,
      // et c'est le prochain SAY qui remplace la page. Vider ici laissait le modèle
      // ENTIÈREMENT vide — plus de texte, pas encore de bouton — et la fenêtre se
      // démontait le temps que le `mes` suivant arrive : un clignotement à chaque
      // `clear`, c'est-à-dire à chaque écran de dialogue un peu bavard.
      //
      // ⚠ Écart assumé avec le natif, qui vide le richtext immédiatement : un
      // `clear` NON suivi d'un `mes` laisse l'ancienne page affichée au lieu d'un
      // cadre vide. Le motif est introuvable dans les scripts (`clear` sert
      // toujours à réécrire par-dessus), et l'ancienne page est de toute façon plus
      // lisible qu'un cadre vide. C'est exactement la mécanique déjà retenue pour
      // `next` et pour un choix de menu, qui sont aussi des sauts de page.
      //
      // 🔴 Et il ne PUBLIE rien — surtout pas. `clear` est le PREMIER paquet de la
      // réponse du serveur (l'agent de warp l'envoie avant ses ~700 `mes`,
      // moon/warp_agent.npc:794). Y publier faisait tomber le menu de la page
      // précédente à cet instant précis : le corps encore affiché s'étalait aussitôt
      // dans la place libérée, restait ainsi toute la durée de la rafale, puis se
      // faisait raboter à l'arrivée du nouveau menu. Deux réarrangements au lieu de
      // zéro. La page affichée doit rester ENTIÈRE — texte ET menu, grisés — jusqu'à
      // la seconde où la suivante la remplace.
      start_fresh_ = true;
      return;
    default:
      return;
  }
}

// ── Balises MAISON ──────────────────────────────────────────────────────────
// Le vocabulaire est décrit en tête de fichier. Ici : la reconnaissance, et rien
// d'autre — le texte affiché d'un lien est composé LOCALEMENT (donc dans la langue
// du lecteur pour ce qui est traduisible), le nom transporté ne servant qu'aux
// clients qui ne rendent pas la balise.
//
// Une balise mal formée (fermante absente, champs manquants, id nul) n'est PAS
// consommée ici : elle retombe sur le parseur générique, qui masque les chevrons
// et laisse le corps en texte. C'est le bon échec — un script fautif se voit,
// là où l'escamotage complet se cherche pendant une heure.
const char* NpcDialogWindow::TryOwnTag(const char* p, const char* end, Run* out) {
  const char *b = nullptr, *be = nullptr, *after = nullptr;
  std::string f[3];

  if (TagBody(p, end, "<MOBL>", "</MOBL>", &b, &be, &after)) {
    if (!SplitFields(b, be, 3, f)) return nullptr;
    const uint32_t id = static_cast<uint32_t>(std::strtoul(f[0].c_str(), nullptr, 10));
    int rank = std::atoi(f[1].c_str());
    if (rank < 0 || rank > 2) rank = 0;
    if (id == 0 || f[2].empty()) return nullptr;
    const std::string name = AnsiToUtf8(f[2]);
    out->target = links::FromMob(id, rank, name.c_str());
    out->text   = "<" + std::string(MobRankTag(rank)) + " " + name + ">";
    return after;
  }
  if (TagBody(p, end, "<ITMR>", "</ITMR>", &b, &be, &after)) {
    if (!SplitFields(b, be, 2, f)) return nullptr;
    const uint32_t id = static_cast<uint32_t>(std::strtoul(f[0].c_str(), nullptr, 10));
    if (id == 0 || f[1].empty()) return nullptr;
    // Le nom TRANSPORTÉ, pas celui de notre DB : l'écrivain du script sait de quel
    // objet il parle, et un client dans une autre langue ne doit pas le renommer.
    const std::string name = AnsiToUtf8(f[1]);
    out->target = links::FromItemId(id, name.c_str());
    out->text   = "<" + name + ">";
    return after;
  }
  if (TagBody(p, end, "<CRAF>", "</CRAF>", &b, &be, &after)) {
    if (!SplitFields(b, be, 2, f)) return nullptr;
    const uint32_t id = static_cast<uint32_t>(std::strtoul(f[0].c_str(), nullptr, 10));
    if (id == 0 || f[1].empty()) return nullptr;
    const std::string name = AnsiToUtf8(f[1]);
    // 🔴 Cible VIDE si l'objet n'a pas de recette (links::FromRecipe le décide) :
    // le fragment reste alors du texte ordinaire. Un lien qui n'ouvre rien vaut
    // moins que pas de lien — même règle que dans le chat.
    out->target = links::FromRecipe(id, name.c_str());
    out->text   = RecipeLinkLabel(name);
    return after;
  }
  if (TagBody(p, end, "<SETL>", "</SETL>", &b, &be, &after)) {
    if (!SplitFields(b, be, 2, f)) return nullptr;
    if (f[0].empty()) return nullptr;
    out->target = links::FromSetting(f[0].c_str());
    // Destination inconnue de CETTE version (ou indisponible pour ce joueur) : le
    // libellé transporté par le script reste, en texte simple. Il dit encore de
    // quoi on parle, il ne prétend plus mener quelque part.
    const std::string label = links::SettingLabel(f[0].c_str());
    out->text = label.empty() ? AnsiToUtf8(f[1]) : label;
    if (out->text.empty()) return nullptr;
    return after;
  }
  if (TagBody(p, end, "<MOBS>", "</MOBS>", &b, &be, &after)) {
    const std::string id(b, be);
    if (!AllDigits(id)) return nullptr;
    out->media  = Run::kMediaMobSprite;
    out->mob_id = std::atoi(id.c_str());
    if (out->mob_id <= 0) return nullptr;
    return after;
  }
  if (TagBody(p, end, "<MOBP>", "</MOBP>", &b, &be, &after)) {
    // Le portrait ne produit AUCUN fragment : il vaut pour la page entière et
    // c'est ScanPageDirectives qui le relève. Ici on ne fait que l'effacer du
    // texte, pour qu'il n'y laisse pas ses chevrons.
    const std::string id(b, be);
    if (!AllDigits(id)) return nullptr;
    return after;
  }
  if (TagBody(p, end, "<IMG>", "</IMG>", &b, &be, &after)) {
    // Deux écritures : `source` seule, ou `w:h:source`. On ne tente la seconde que
    // si les deux premiers champs sont des NOMBRES — sans quoi « https » et son
    // ':' feraient passer une adresse pour une taille.
    std::string src(b, be);
    float want_w = 0.0f, want_h = 0.0f;
    if (SplitFields(b, be, 3, f) && AllDigits(f[0]) && AllDigits(f[1])) {
      want_w = static_cast<float>(std::atoi(f[0].c_str()));
      want_h = static_cast<float>(std::atoi(f[1].c_str()));
      src    = f[2];
    }
    if (src.empty()) return nullptr;
    out->media  = Run::kMediaImage;
    out->src    = src;
    out->want_w = want_w;
    out->want_h = want_h;
    return after;
  }
  return nullptr;
}

// ── Parsing d'une ligne en runs (couleur ^RRGGBB, gras/italique, liens) ──
// Les segments sortent en UTF-8 : la conversion depuis l'ANSI des scripts serveur
// se fait ICI, une fois par ligne et par page, plutôt qu'à chaque frame de rendu.
// (`link_arg` reste brut : un id d'item ou une URL sont de l'ASCII.)
void NpcDialogWindow::ParseLine(const std::string& raw, std::vector<Run>* out) {
  Run cur;
  cur.color = 0;
  bool in_info = false;  // dans <INFO>…</INFO> : on capture l'id item (pas d'affichage)
  auto flush = [&]() {
    if (!cur.text.empty()) {
      out->push_back(cur);
      out->back().text = AnsiToUtf8(cur.text);
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
    // Balises MAISON (`<MOBL>`, `<IMG>`…) : essayées EN PREMIER, parce qu'elles
    // ont un corps structuré et une fermante explicite — le traitement générique
    // ci-dessous, lui, ne lit qu'un nom entre chevrons et jetterait le reste.
    if (*p == '<') {
      Run made;
      made.color = cur.color;  // un média hérite du contexte, un lien reprendra le sien
      if (const char* np = TryOwnTag(p, end, &made)) {
        flush();
        // Un lien d'OBJET porte son icône, comme dans la chatbox : c'est ce qui rend
        // une liste de composants lisible d'un coup d'œil. Les autres genres n'en ont
        // pas (une recette n'est pas l'objet, un monstre a son sprite).
        if (made.target.kind == links::Target::kItem && made.target.item.id != 0) {
          Run icon;
          icon.color   = 0;
          icon.icon_id = static_cast<int>(made.target.item.id);
          out->push_back(icon);
        }
        // Un `<MOBP>` ne produit rien à afficher : il a déjà été relevé pour la page.
        if (made.media != Run::kMediaNone || !made.text.empty()) {
          // Déjà en UTF-8 (converti dans TryOwnTag) : ne pas repasser par le flush,
          // qui reconvertirait — un texte UTF-8 relu comme de l'ANSI donne du mojibake.
          made.bold   = cur.bold;
          made.italic = cur.italic;
          out->push_back(std::move(made));
        }
        p = np;
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

// ── Mise en page du corps (word-wrap multi-couleur) ──
// Place TOUTES les lignes en coordonnées LOCALES au corps. C'est le calcul cher —
// parsing des balises, conversion ANSI->UTF-8, mesure de chaque mot — et il était
// refait à chaque frame : une page de deux cents lignes payait son propre découpage
// soixante fois par seconde PENDANT qu'elle arrivait, d'où son apparition poussive
// à côté du natif (dont UIRichTextCtrl::AddLine ne place la ligne qu'une fois).
// Ici il ne repart qu'au changement de page, de largeur ou de taille de police.
void NpcDialogWindow::BuildLayout(float wrap, float fsize, float line_h) {
  frags_.clear();
  targets_.clear();
  img_srcs_.clear();
  ImFont* font = ImGui::GetFont();
  const float space_w = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, " ").x;

  float x = 0.0f, y = 0.0f;
  // ── Lignes à hauteur VARIABLE ──────────────────────────────────────────────
  // Un sprite ou une image fait plusieurs lignes de haut, et le texte posé à côté
  // doit rester lisible : on ferme donc chaque ligne VISUELLE en connaissant sa
  // hauteur réelle, puis on centre son contenu dessus. Sans média, `line_max`
  // reste `line_h` et le décalage vaut zéro — le rendu des pages ordinaires ne
  // bouge pas d'un pixel.
  size_t line_start = 0;      // premier fragment de la ligne visuelle en cours
  float  line_max   = line_h;  // sa hauteur (le plus haut de ses fragments)
  auto end_line = [&]() {
    for (size_t k = line_start; k < frags_.size(); ++k) {
      Frag& g = frags_[k];
      // Le texte se centre sur la bande de texte (sa hauteur nominale est celle
      // d'une ligne, pas celle du glyphe) ; un média sur sa propre hauteur.
      const float ref = (g.media != 0) ? g.h : line_h;
      g.y += (line_max - ref) * 0.5f;
    }
    y += line_max;
    line_start = frags_.size();
    line_max   = line_h;
    x = 0.0f;
  };

  std::vector<Run> runs;
  for (const std::string& raw : lines_) {
    runs.clear();
    ParseLine(raw, &runs);
    for (const Run& r : runs) {
      // ── Médias (image, sprite de monstre) ────────────────────────────────
      if (r.media != Run::kMediaNone) {
        ImVec2 box(0.0f, 0.0f);
        int img_idx = -1;
        if (r.media == Run::kMediaImage) {
          box = ImageBoxSize(r.src, r.want_w, r.want_h, wrap, line_h);
          img_idx = static_cast<int>(img_srcs_.size());
          img_srcs_.push_back(r.src);
        } else {
          const float side = (r.want_h > 0.0f) ? r.want_h : kSpriteInlineH;
          box = ImVec2(side, side);
        }
        // Jamais plus large que le corps — et à l'échelle, sans déformer (le
        // ratio d'un sprite ou d'une image est une information).
        if (box.x > wrap && box.x > 0.0f) {
          const float s = wrap / box.x;
          box.x *= s;
          box.y *= s;
        }
        if (x > 0.0f && x + box.x > wrap) end_line();
        Frag f;
        f.x = x; f.y = y; f.w = box.x; f.h = box.y;
        f.media  = r.media;
        f.img    = img_idx;
        f.mob_id = r.mob_id;
        if (r.target.valid()) {
          f.target = static_cast<int>(targets_.size());
          targets_.push_back(r.target);
        }
        frags_.push_back(std::move(f));
        x += box.x + 4.0f;
        if (box.y > line_max) line_max = box.y;
        continue;
      }
      // Un lien maison : sa cible est mémorisée UNE fois pour le run, et chacun de
      // ses mots y renvoie — c'est la zone entière qui est cliquable, pas un mot.
      int target_idx = -1;
      if (r.target.valid()) {
        target_idx = static_cast<int>(targets_.size());
        targets_.push_back(r.target);
      }
      // Icône item ^i[id] : hauteur = ligne, largeur au RATIO d'origine (pas de déform).
      // ⚠ On ne garde QUE ses dimensions : une texture ne survit pas à un reset de
      // device (cf. ui/icon_cache.h), le handle est redemandé au rendu. La résolution
      // du chemin, elle, est mémorisée par le cache — l'appel ici est définitif.
      if (r.icon_id != 0) {
        ro::IconTex ic = ro::ItemIcon(static_cast<uint32_t>(r.icon_id));
        if (ic.tex && ic.h > 0) {
          const float ih = fsize + 3.0f;
          const float iw = ih * static_cast<float>(ic.w) / static_cast<float>(ic.h);
          if (x > 0.0f && x + iw > wrap) end_line();
          Frag f;
          f.x = x; f.y = y; f.w = iw; f.h = ih;
          f.icon_id = r.icon_id;
          f.target  = target_idx;
          frags_.push_back(std::move(f));
          x += iw + 3.0f;
        }
        continue;
      }
      const std::string& u = r.text;  // déjà en UTF-8 (cf. ParseLine)
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
        if (x > 0.0f && x + ww > wrap) { end_line(); wrapped = true; }
        Frag f;
        f.x = x; f.y = y; f.w = ww; f.h = fsize;
        f.text.assign(w0, w1);
        f.color  = r.color;  // 0 = couleur par défaut, résolue au rendu (thème)
        f.bold   = r.bold;
        f.link   = r.link;
        f.target = target_idx;
        if (r.link || target_idx >= 0) {
          if (r.link) f.link_arg = r.link_arg;
          // Étend à gauche jusqu'au début des espaces de tête (MÊME ligne) pour que TOUT
          // le libellé du lien soit cliquable/souligné en continu, y compris entre 2 mots.
          // Vaut pour les liens maison aussi : « <[Mob] Poring> » fait deux mots, et
          // sans cela le soulignement du second repartirait de la marge gauche.
          f.ux0 = (!wrapped && gap_y == y) ? gap_x : x;
        }
        frags_.push_back(std::move(f));
        x += ww;
        i = j;
      }
    }
    end_line();  // fin de ligne source
  }
  layout_h_    = y;
  layout_wrap_ = wrap;
  layout_font_ = fsize;
  frags_gen_   = page_gen_;
}

// ── Rendu du corps (ImDrawList) ──
void NpcDialogWindow::DrawRichLines() {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float fsize = ImGui::GetFontSize();
  const float line_h = ImGui::GetTextLineHeightWithSpacing();
  const float wrap = ImGui::GetContentRegionAvail().x;
  const ImU32 def_col = ImGui::GetColorU32(ImGuiCol_Text);
  const ImVec2 origin = ImGui::GetCursorScreenPos();

  if (frags_gen_ != page_gen_ || layout_wrap_ != wrap || layout_font_ != fsize)
    BuildLayout(wrap, fsize, line_h);

  // Bande réellement visible du corps. Hors d'elle, un fragment ne produirait que
  // des sommets pour rien — et une liste de deux cents monstres en compte des
  // milliers, tous empilés dans la même ImDrawList à chaque frame.
  const float view_top = ImGui::GetWindowPos().y;
  const float view_bot = view_top + ImGui::GetWindowSize().y;

  // Un média peut dépasser la ligne : la marge évite qu'un fragment haut, dont le
  // haut est déjà sorti par le bas de la bande, soit coupé alors qu'il reste
  // visible. Le tri par y n'est plus strict depuis le centrage vertical des lignes
  // (± une demi-hauteur de ligne), et le plafond des médias borne l'écart.
  const float slack = kMediaMaxH;
  bool relayout = false;

  for (const Frag& f : frags_) {
    const ImVec2 pos(origin.x + f.x, origin.y + f.y);
    if (pos.y > view_bot + slack) break;
    if (pos.y + f.h + line_h < view_top) continue;
    if (f.media != 0) {
      // L'image web arrivée depuis la mise en page a maintenant ses vraies
      // dimensions : on refait la page UNE fois (ImageBoxSize est déterministe,
      // donc l'égalité s'établit et la boucle ne se répète pas).
      if (f.media == Run::kMediaImage && f.img >= 0 &&
          f.img < static_cast<int>(img_srcs_.size())) {
        const ImVec2 now = ImageBoxSize(img_srcs_[f.img], 0.0f, 0.0f, wrap, line_h);
        // Une taille imposée par le script ne se recalcule pas : `now` vaudrait la
        // taille naturelle. On ne compare donc que si la boîte placée vient bien
        // d'un calcul sans consigne — repérable au fait qu'elle valait l'un des
        // états de `ImageBoxSize` (placeholder ou cartouche).
        const ImVec2 ph = FitBox(kWebPlaceholderW, kWebPlaceholderH, wrap, kMediaMaxH);
        const bool was_provisional =
            (f.w == ph.x && f.h == ph.y) ||
            (f.w == NoticeBox(wrap, line_h).x && f.h == NoticeBox(wrap, line_h).y);
        if (was_provisional && (now.x != f.w || now.y != f.h)) relayout = true;
      }
      DrawMedia(dl, f, pos);
      const bool hovered = ImGui::IsMouseHoveringRect(
          pos, ImVec2(pos.x + f.w, pos.y + f.h));
      if (f.target >= 0 && f.target < static_cast<int>(targets_.size()))
        LinkGestures(targets_[f.target], hovered);
      continue;
    }
    if (f.icon_id != 0) {
      ro::IconTex ic = ro::ItemIcon(static_cast<uint32_t>(f.icon_id));
      if (ic.tex)
        dl->AddImage(TexId(ic.tex), pos, ImVec2(pos.x + f.w, pos.y + f.h));
      continue;
    }
    // Un lien prend TOUJOURS la couleur de lien (bleu, comme le natif) ; sinon la
    // couleur ^RRGGBB explicite, ou le texte par défaut. Les liens MAISON suivent la
    // même règle : dans une page de dialogue, ce qui est cliquable se voit.
    const bool own_link = f.target >= 0 && f.target < static_cast<int>(targets_.size());
    const ImU32 col =
        (f.link || own_link) ? kLinkColor : (f.color ? f.color : def_col);
    const char* t0 = f.text.c_str();
    const char* t1 = t0 + f.text.size();
    dl->AddText(pos, col, t0, t1);
    if (f.bold)  // fake-bold : re-dessine décalé de 1px
      dl->AddText(ImVec2(pos.x + 1.0f, pos.y), col, t0, t1);
    if (f.link || own_link) {  // souligne + rend cliquable
      const float x0 = origin.x + f.ux0;
      dl->AddLine(ImVec2(x0, pos.y + fsize), ImVec2(pos.x + f.w, pos.y + fsize),
                  kLinkColor);
      const bool hovered = ImGui::IsMouseHoveringRect(
          ImVec2(x0, pos.y), ImVec2(pos.x + f.w, pos.y + fsize + 2.0f));
      if (own_link) {
        // 🔴 Les gestes passent par links:: — le MÊME module que la chatbox et la
        // table des drops. Gauche ouvre la description, droite le menu, Maj+clic
        // pose le lien dans la barre de chat : la convention ne se réinvente pas
        // par surface (cf. features/link_gesture.h).
        LinkGestures(targets_[f.target], hovered);
      } else if (!f.link_arg.empty() && hovered) {
        ro::SetHoverCursor(2);  // 2 = curseur « main » RO (le jeu dessine son curseur)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          pending_link_cmd_ = f.link;      // action décidée au tick (item/url)
          pending_link_arg_ = f.link_arg;
        }
      }
    }
  }
  if (relayout) frags_gen_ = 0xFFFFFFFFu;  // remise en page à la frame suivante
  ImGui::Dummy(ImVec2(wrap, layout_h_));   // réserve la hauteur (scroll)
}

// Le dessin d'un média DÉJÀ placé. Il ne décide de rien — la mise en page a fixé
// le rectangle — mais il connaît les états dégradés : une image dont l'hôte n'est
// pas autorisé se propose au clic, une image absente le DIT.
void NpcDialogWindow::DrawMedia(ImDrawList* dl, const Frag& f, ImVec2 p0) {
  const ImVec2 p1(p0.x + f.w, p0.y + f.h);
  const ImU32 dim = kNoticeColor;

  if (f.media == Run::kMediaMobSprite) {
    ro::MobSpriteRes* res = MobSprite(f.mob_id);
    if (res == nullptr) return;
    ro::DrawMobSprite(dl, *res, p0, p1,
                      static_cast<float>(ImGui::GetTime()), /*action=*/0,
                      kSpriteMsPerFrame, /*allow_upscale=*/false);
    return;
  }

  if (f.img < 0 || f.img >= static_cast<int>(img_srcs_.size())) return;
  const std::string& src = img_srcs_[f.img];

  if (!IsWebSource(src)) {
    const ro::GameTexture t = ro::CachedTextureFromGameFile(src.c_str());
    if (t.tex != nullptr) {
      dl->AddImage(TexId(t.tex), p0, p1);
    } else {
      dl->AddRect(p0, p1, dim);
      dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 3.0f), dim,
                  i18n::Tr("[image introuvable]"));
    }
    return;
  }

  // Web. 🔴 L'hôte non autorisé ne charge RIEN tout seul : c'est la règle du
  // module d'aperçu (image_preview.h), et elle vaut ici aussi — l'adresse vient du
  // serveur, mais le joueur reste celui qui décide de contacter un tiers.
  if (!imgprev::IsPreviewable(src.c_str())) {
    dl->AddRect(p0, p1, dim);
    const std::string host = imgprev::HostOfUrl(src.c_str());
    char label[160];
    std::snprintf(label, sizeof(label), i18n::Tr("Cliquer pour afficher (%s)"),
                  host.empty() ? i18n::Tr("site inconnu") : host.c_str());
    dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 3.0f), dim, label);
    if (ImGui::IsMouseHoveringRect(p0, p1)) {
      ro::SetHoverCursor(2);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        imgprev::AllowOnce(src.c_str());  // cette image-ci, une fois
    }
    return;
  }
  imgprev::Request(src.c_str());  // idempotent
  const imgprev::Preview p = imgprev::Get(src.c_str());
  if (p.state == imgprev::Preview::kReady && p.tex != nullptr) {
    dl->AddImage(TexId(p.tex), p0, p1);
    return;
  }
  dl->AddRect(p0, p1, dim);
  // Mêmes libellés que l'aperçu d'image du chat : c'est le même mécanisme, il n'a
  // pas à se nommer autrement ici (et les deux sont déjà traduits).
  dl->AddText(ImVec2(p0.x + 4.0f, p0.y + 3.0f), dim,
              (p.state == imgprev::Preview::kFailed)
                  ? i18n::Tr("Aperçu indisponible.")
                  : i18n::Tr("Chargement de l'aperçu..."));
}

// Les gestes d'un lien maison. L'ouverture du menu est DIFFÉRÉE d'une frame :
// l'identifiant d'un popup se hache avec la pile d'ID de la fenêtre où l'OpenPopup
// est appelé, et nous sommes ici dans un child. Même mécanique que la chatbox.
void NpcDialogWindow::LinkGestures(const links::Target& target, bool hovered) {
  if (!target.valid()) return;
  if (hovered) links::HoverPreview(target);
  if (links::Gestures(target, hovered)) {
    link_menu_ = target;
    link_menu_request_ = true;
  }
}

ro::MobSpriteRes* NpcDialogWindow::MobSprite(int class_id) {
  if (class_id <= 0) return nullptr;
  auto it = mob_sprites_.find(class_id);
  if (it == mob_sprites_.end())
    it = mob_sprites_.emplace(class_id, ro::MobSpriteRes{}).first;
  // Idempotent : LoadMobSprite ne recharge pas ce qu'il a déjà, et retient l'échec
  // (`failed`) pour ne pas retenter un monstre sans sprite à chaque frame.
  ro::LoadMobSprite(class_id, &it->second);
  return it->second.failed ? nullptr : &it->second;
}

size_t NpcDialogWindow::MenuVisibleCount() const {
  if (menu_filter_[0] == '\0') return menu_opts_.size();
  const std::string f = LowerAscii(menu_filter_);
  size_t n = 0;
  for (const MenuOption& o : menu_opts_)
    if (o.search.find(f) != std::string::npos) ++n;
  return n;
}

float NpcDialogWindow::MenuNaturalHeight() const {
  if (menu_opts_.empty()) return 0.0f;
  // 🔴 Nombre de lignes RÉELLEMENT à l'écran, calculé MAINTENANT — filtre de
  // recherche compris. C'est ce qui permet de dimensionner le menu AVANT le corps
  // (OnRenderUI l'appelle en premier) : quand il était repris de la frame
  // précédente, la boîte rattrapait sa taille avec un temps de retard et le texte
  // au-dessus sautait d'autant.
  size_t rows = MenuVisibleCount();
  if (rows < 1) rows = 1;                 // « aucun résultat » garde une ligne de haut
  if (rows > kMenuMaxRows) rows = kMenuMaxRows;  // au-delà : scrollbar interne

  const ImGuiStyle& st = ImGui::GetStyle();
  // Le child liste est bordé : son padding compte deux fois. Les deux pixels de
  // rabiot absorbent l'arrondi du cadre — les oublier rogne la dernière option
  // d'un cheveu, ce qui fait apparaître une scrollbar sur un menu qui tenait.
  float h = static_cast<float>(rows) * MenuRowHeight() +
            st.WindowPadding.y * 2.0f + 2.0f;
  if (menu_search_ && menu_opts_.size() > kMenuFilterThreshold)  // barre de recherche
    h += ImGui::GetFrameHeight() + st.ItemSpacing.y;
  return h;
}

void NpcDialogWindow::DrawMenu(float group_h) {
  if (menu_opts_.empty()) return;
  // Groupe menu à hauteur BORNÉE (barre de recherche + liste) : il s'ajuste au
  // nombre d'options (cf. MenuNaturalHeight) et ne pousse jamais les boutons hors
  // de la fenêtre.
  ImGui::BeginChild("##menugrp", ImVec2(0, group_h), false, ImGuiWindowFlags_NoScrollbar);

  const bool filterable = menu_search_ && menu_opts_.size() > kMenuFilterThreshold;
  bool enter_from_search = false;
  if (filterable) {
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##menufilter", i18n::Tr("Rechercher..."), menu_filter_,
                                 sizeof(menu_filter_),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
      enter_from_search = true;  // Entrée dans la recherche = valide le choix focus
  }
  const std::string f = LowerAscii(menu_filter_);

  // Navigation clavier : auto-focus de l'option 1 à l'ouverture, flèches haut/bas
  // (avec wrap) sur le nombre d'items VISIBLES — connu avant de dessiner, donc plus
  // de bouclage sur le compte de la frame précédente.
  const int vis_count = static_cast<int>(MenuVisibleCount());
  if (menu_hot_ < 0) menu_hot_ = 0;  // auto-focus #1
  bool nav = false;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) { ++menu_hot_; nav = true; }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))   { --menu_hot_; nav = true; }
  if (vis_count > 0) {
    if (menu_hot_ < 0) menu_hot_ = vis_count - 1;   // wrap bas
    if (menu_hot_ >= vis_count) menu_hot_ = 0;      // wrap haut
  } else {
    menu_hot_ = 0;
  }

  int chosen = -1;              // RANG 1-based (options non vides) à envoyer
  // Rangs des options actuellement visibles, dans l'ordre d'affichage (pour les
  // touches 1-9 et la nav clavier). On y stocke le RANG, pas l'index brut : sous
  // filtre, la position à l'écran n'a plus rien à voir avec ce qu'attend le serveur.
  std::vector<int> visible;

  ImGui::BeginChild("##menu", ImVec2(0, 0), true);  // remplit le reste du groupe (scroll interne)
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font = ImGui::GetFont();
  const float fsize = ImGui::GetFontSize();
  const ImU32 def_col = ImGui::GetColorU32(ImGuiCol_Text);
  const float row_h = MenuRowHeight();  // même mesure que MenuNaturalHeight
  const float pad_x = ImGui::GetStyle().ItemSpacing.x;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(pad_x, 0.0f));  // aucun espace vertical entre items
  // 🔴 Le rang ne se compte PAS ici : il est figé à la réception (HandlePacket) et
  // vaut la position parmi TOUTES les options non vides du menu, filtrées ou non —
  // exactement ce que compte menu_countoptions() côté serveur. Le recompter après
  // filtre donnerait le numéro de la ligne à l'écran, et un menu filtré enverrait le
  // mauvais choix.
  for (size_t i = 0; i < menu_opts_.size(); ++i) {
    const MenuOption& o = menu_opts_[i];
    if (!f.empty() && o.search.find(f) == std::string::npos) continue;
    const bool hot = (static_cast<int>(visible.size()) == menu_hot_);  // item focus clavier ?
    visible.push_back(o.rank);
    // Selectable transparent (clic + surbrillance ; `hot`=focus clavier) ; le texte
    // MULTICOLORE est dessiné par-dessus (chaque run garde sa couleur).
    char id[16];
    std::snprintf(id, sizeof(id), "##m%d", static_cast<int>(i));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable(id, hot, ImGuiSelectableFlags_None, ImVec2(0, row_h)))
      chosen = o.rank;
    if (hot && nav) ImGui::SetScrollHereY(0.5f);  // garde le focus visible pendant la nav
    float x = p0.x + pad_x;
    const float ty = p0.y + (row_h - fsize) * 0.5f;
    char num[16];
    std::snprintf(num, sizeof(num), "%d. ", static_cast<int>(visible.size()));
    dl->AddText(ImVec2(x, ty), def_col, num);
    x += font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, num).x;
    for (const Run& r : o.runs) {
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
      // ⚠ Un média dans une OPTION reste à la hauteur de sa ligne — une option de
      // menu est une ligne cliquable d'un seul tenant, pas un paragraphe : y
      // laisser grandir une image ferait une liste dont chaque entrée aurait sa
      // taille. Un sprite de monstre y devient donc une vignette, ce qui est
      // exactement l'usage attendu (« choisir un monstre dans une liste »).
      if (r.media != Run::kMediaNone) {
        const float mh = row_h - 1.0f;
        const float my = p0.y + (row_h - mh) * 0.5f;
        Frag mf;
        mf.w = mh; mf.h = mh;
        mf.media = r.media;
        mf.mob_id = r.mob_id;
        if (r.media == Run::kMediaImage) {
          // Le dessin d'image lit `img_srcs_`, qui appartient à la mise en page du
          // CORPS : on y passe par une entrée temporaire plutôt que d'ouvrir un
          // second chemin de rendu.
          mf.img = static_cast<int>(img_srcs_.size());
          img_srcs_.push_back(r.src);
        }
        DrawMedia(dl, mf, ImVec2(x, my));
        if (r.media == Run::kMediaImage) img_srcs_.pop_back();
        x += mh + 3.0f;
        continue;
      }
      if (r.text.empty()) continue;  // déjà en UTF-8 (cf. ParseLine)
      // 🔴 Un lien dans une option de menu se voit (couleur), mais ne se CLIQUE
      // pas : le clic appartient au choix. Le rectangle d'un Selectable couvre
      // toute la ligne, et disputer ce clic à links:: ferait qu'un joueur visant
      // « 3. <Poring> » ouvrirait une fiche au lieu de répondre au NPC. Le lien
      // reste utile — on lit de quoi parle l'option — sans piéger le geste.
      const bool own_link = r.target.valid();
      const ImU32 col = own_link ? kLinkColor : (r.color ? r.color : def_col);
      dl->AddText(ImVec2(x, ty), col, r.text.c_str());
      if (r.bold) dl->AddText(ImVec2(x + 1.0f, ty), col, r.text.c_str());
      x += font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, r.text.c_str()).x;
    }
  }
  ImGui::PopStyleVar();   // ItemSpacing
  ImGui::EndChild();      // ##menu (liste)
  ImGui::EndChild();      // ##menugrp

  // Touches 1-9 : sélectionne le N-ième choix VISIBLE (envoie son RANG).
  // Seulement hors saisie clavier (sinon taper "1" dans le filtre choisirait #1) ET
  // sans modificateur : Ctrl/Alt/Shift + chiffre = combo hotkey (skillbar, macro…),
  // à laisser passer au jeu, pas une sélection de menu.
  const ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
    for (int k = 1; k <= 9 && k <= static_cast<int>(visible.size()); ++k) {
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + (k - 1)), false))
        chosen = visible[k - 1];
    }
  }
  // Entrée = valide l'option focus (depuis la recherche via EnterReturnsTrue, ou
  // globalement hors saisie clavier ET hors barre de chat armée — cf.
  // ChatOwnsEnter). Le chemin `enter_from_search`, lui, n'est PAS soumis à cette
  // dernière condition : la touche vient alors du filtre, qui a le clavier, et
  // c'est bien lui qui valide — pas une frappe libre qu'on disputerait au chat.
  const bool enter = enter_from_search ||
                     (!ImGui::GetIO().WantTextInput && !ChatOwnsEnter() &&
                      (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                       ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)));
  if (enter && menu_hot_ >= 0 && menu_hot_ < static_cast<int>(visible.size()))
    chosen = visible[menu_hot_];

  // Anti double-envoi : une génération de menu ne peut être répondue qu'UNE fois (sinon
  // le 1er choix fait avancer le serveur et le 2e envoi vise un menu à autre cardinalité
  // -> « Invalid menu selection ... got 14, valid [1..3] »).
  if (chosen > 0 && menu_gen_ != menu_answered_gen_) SendMenuChoice(chosen);
}

// Le portrait de page. Il n'est PAS cliquable : la balise ne transporte qu'un id,
// et le client ne sait pas nommer un monstre — un menu contextuel s'ouvrirait donc
// sur un libellé vide. Un script qui veut un lien pose un `<MOBL>` à côté, qui,
// lui, porte le nom.
void NpcDialogWindow::DrawPortrait(float col_w, float col_h) {
  ro::MobSpriteRes* res = MobSprite(portrait_mob_);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  // Ancré en HAUT de la colonne : le texte commence en haut lui aussi, et un
  // portrait centré sur une page longue flotterait au milieu de rien.
  float h = col_h;
  if (h > 200.0f) h = 200.0f;
  if (res != nullptr) {
    ro::DrawMobSprite(ImGui::GetWindowDrawList(), *res, p0,
                      ImVec2(p0.x + col_w, p0.y + h),
                      static_cast<float>(ImGui::GetTime()), /*action=*/0,
                      kSpriteMsPerFrame, /*allow_upscale=*/false);
  }
  ImGui::Dummy(ImVec2(col_w, col_h));  // réserve la colonne (le corps suit en SameLine)
}

void NpcDialogWindow::DrawInput() {
  if (input_mode_ == kInputNone) return;
  if (input_need_focus_) {  // 1re frame : SetKeyboardFocusHere cible le prochain widget = le champ
    ImGui::SetKeyboardFocusHere();
    input_need_focus_ = false;
  }
  if (input_mode_ == kInputNumber) {
    ImGui::SetNextItemWidth(ro::Px(160.0f));
    const bool enter = ImGui::InputText(
        "##num", num_buf_, sizeof(num_buf_),
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("OK")) || enter) SendNumber(std::atoi(num_buf_));
  } else {  // kInputString
    ImGui::SetNextItemWidth(-60.0f);
    const bool enter = ImGui::InputText("##str", str_buf_, sizeof(str_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("OK")) || enter) SendString(str_buf_);
  }
}

// ── Section « Fenêtre NPC » du panneau Moonlight ─────────────────────────────
// Déplacée depuis moonlight_ui/panel_interface.cc : ces widgets ne pilotent que
// l'état de CE plugin, ils appartiennent donc à ce fichier. MoonlightUi ne garde
// que l'appel et la décision de sauvegarder.
bool NpcDialogWindow::DrawSettings() {
  bool changed = false;
  // Plus de case d'activation : le dialogue NPC suit le groupe « Interface
  // moderne » depuis le 2026-08-18 (ses menus déclenchent shops et fabrications,
  // des fenêtres du groupe). Le réglage fin reste, grisé hors groupe —
  // imgui_enabled_ est écrit par SetModernInterface, donc le tester revient à
  // tester le groupe.
  ImGui::TextDisabled(
      "%s", i18n::Tr("Suivent l'interface moderne — l'interrupteur est en tête "
                     "d'« Interface de jeu »."));
  ImGui::BeginDisabled(!imgui_enabled_);
  changed |= ro::RoCheckbox(i18n::Tr("Barre de recherche du menu"), &menu_search_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Affiche un champ de recherche au-dessus des longs menus (plus de 8 "
      "choix) pour filtrer les options. Décoche pour un menu épuré."));
  ImGui::EndDisabled();
  return changed;
}

// Appelé par le hook WndProc (ragnarok_client) : vrai si cette touche pilote le
// dialogue et ne doit PAS atteindre le jeu (Entrée ouvrirait le chat, Échap le menu
// RO). Tout le reste — F1-F9, lettres… — passe au jeu, comme en natif : la skillbar
// et les hotkeys restent utilisables pendant un dialogue NPC.
//
// ⚠ ENTRÉE RESTE AVALÉE MÊME QUAND LA BARRE DE CHAT LA RÉCLAME, et c'est voulu :
// la laisser filer au jeu la ferait passer par `UIWindowMgr_ActivateDefault`, donc
// par le bouton par défaut de la fenêtre native prioritaire. Le WndProc la remet
// DIRECTEMENT à la chatbox (`OnRawKey`) au lieu de la relâcher — même recette
// qu'Échap, et le seul moyen de ne pas affamer notre propre handler.
bool NpcDialogWindow::EatsKey(unsigned msg, unsigned long wparam) {
  if (!g_kbd_dialog_open) return false;
  // Combo avec modificateur = hotkey jeu (skillbar, macro…) : on laisse passer.
  if ((GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000) ||
      (GetKeyState(VK_SHIFT) & 0x8000))
    return false;
  switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
      if (wparam == VK_RETURN || wparam == VK_SPACE || wparam == VK_ESCAPE)
        return true;  // Next/Close/annulation
      return g_kbd_menu_open &&
             (wparam == VK_UP || wparam == VK_DOWN ||
              (wparam >= '1' && wparam <= '9'));
    case WM_CHAR: case WM_UNICHAR:
      // TranslateMessage émet le WM_CHAR même quand le WM_KEYDOWN est avalé.
      if (wparam == '\r' || wparam == ' ') return true;
      return g_kbd_menu_open && wparam >= '1' && wparam <= '9';
  }
  return false;
}

void NpcDialogWindow::OnRenderUI() {
  g_kbd_dialog_open = g_kbd_menu_open = false;  // recalculé chaque frame
  if (!open_ || !imgui_enabled_) return;

  // Filet de la mise en attente : une page dont le paquet TERMINAL n'arrive jamais
  // doit finir par s'afficher (un script qui écrit puis dort, ou qui s'arrête sur
  // `end` sans `close`). Il se mesure en SILENCE — temps écoulé depuis le DERNIER
  // paquet de dialogue — et non en durée totale : une page qui met une seconde à
  // traverser le réseau repousse l'échéance à chaque paquet et reste donc entière.
  //
  // 🔴 Le seuil est volontairement LARGE. Le script engine de rAthena ne rend jamais
  // la main entre deux `mes` : tous les paquets d'une page partent dans le même
  // envoi, et seul TCP peut les espacer — de quelques frames, jamais d'un tiers de
  // seconde. Un vrai `sleep` de script, lui, dure au moins une seconde. Le seuil
  // sépare donc proprement les deux. (Cf. l'agent de warp, détail des cartes
  // ACTIVÉ : ~700 `mes` d'affilée, ~40 Ko — c'est là que le filet trop nerveux
  // publiait une demi-page.)
  constexpr unsigned long kPageIdleMs = 400;
  if (!pending_lines_.empty() &&
      static_cast<unsigned long>(GetTickCount() - last_dialog_ms_) >= kPageIdleMs)
    CommitPage();

  // Rien à afficher (transitoire entre paquets) : pas de fenêtre vide, sauf si un
  // bouton Next/Close est demandé (pour pouvoir cliquer Fermer).
  //
  // 🔴 Mais UNIQUEMENT tant que la fenêtre n'est pas encore apparue. Une fois
  // montée, un creux d'une seule frame la démontait et la remontait aussitôt — un
  // clignotement, alors qu'ImGui n'a besoin que de la voir déclarée pour la garder
  // en place. Le serveur n'a aucune obligation de faire tenir sa réponse dans un
  // seul segment : ce trou est normal, ce n'est pas une fin de conversation. Seul
  // CloseDialog (ou un warp) ferme, en repassant par Reset.
  const bool nothing_to_show = lines_.empty() && menu_opts_.empty() &&
                               input_mode_ == kInputNone && !has_next_ && !has_close_;
  if (nothing_to_show && !rendered_) return;
  rendered_ = true;
  g_kbd_dialog_open = true;
  g_kbd_menu_open = !menu_opts_.empty();

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
      (nit != npc_names_.end() && !nit->second.empty()) ? nit->second : std::string(i18n::Tr("PNJ")));
  std::snprintf(title, sizeof(title), i18n::Tr("%s###bourgeon_npc_dialog"), name.c_str());

  // Skin « fenêtre de description » (barre claire skill_upbar + cadre sysbox, fond
  // crème) — même habillage que les panneaux item/skill. Échap géré par la pile RO
  // (auto-enregistrée via &show_panel_).
  // Fermeture possible seulement quand elle a un sens côté serveur : `close` (fermeture
  // réelle) ou `menu` (annulation -> CZ_CHOOSE_MENU 0xFF). PAS pendant un `next` : y
  // envoyer CZ_CLOSE_DIALOG ferait AVANCER le script (npc_scriptcont reprend) au lieu de
  // fermer -> le natif n'offre d'ailleurs que « Suivant » à ce moment. Donc pas de croix
  // ⊗ ni d'Échap pendant un `next`.
  const bool can_close = has_close_ || !menu_opts_.empty();
  // État menu/input CAPTURÉ AVANT DrawMenu/DrawInput : ces fonctions, sur Entrée,
  // répondent PUIS vident menu_opts_/input_mode_. Sans ce snapshot, le footer verrait
  // menu_opts_ déjà vide et piloterait « Annuler » sur la MÊME touche -> double envoi
  // (choix de menu + annulation). Donc : clavier du footer actif seulement s'il n'y a
  // NI menu NI input en cours.
  const bool footer_kbd_ok = menu_opts_.empty() && input_mode_ == kInputNone;
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
  // Rapport de bug : petit bouton posé DANS la barre de titre, entre le nom du PNJ
  // et la croix de fermeture. Il était auparavant dans le footer, où il partageait
  // la ligne des boutons de dialogue — qui apparaissent et disparaissent à chaque
  // étape du script (`next` vs `menu` vs `close`). À chaque transition il glissait à
  // gauche pour occuper la place libérée, et atterrissait sous le curseur : saut
  // visuel, plus son infobulle qui s'ouvrait toute seule. Dans la barre de titre, sa
  // position ne dépend plus du contenu.
  auto* br = Bourgeon::Instance().bug_report();
  const bool bug_btn = br != nullptr && br->enabled();
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
    // Clavier : PAS de capture ImGui globale (elle avalait AUSSI F1-F9 -> skillbar
    // morte pendant un dialogue, contrairement au natif). Le WndProc consulte
    // EatsKey() et n'avale que les touches que le dialogue pilote (Entrée/Espace/
    // Échap, + flèches/1-9 quand un menu est affiché) — cf. g_kbd_dialog_open posé
    // en tête d'OnRenderUI. La saisie d'un champ (input/filtre) garde sa propre
    // capture via WantTextInput (ImGui pose WantCaptureKeyboard tout seul).

    // Layout à FOOTER FIXE : texte (flexible, scroll interne) + menu (borné, scroll
    // interne) + input, puis les boutons ÉPINGLÉS en bas. La fenêtre est NoScrollbar
    // -> jamais de grand scrollbar extérieur qui emporterait le bouton Annuler.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float row = ImGui::GetFrameHeightWithSpacing();
    const float sep = st.ItemSpacing.y + 1.0f;               // ~hauteur d'un Separator
    const float avail = ImGui::GetContentRegionAvail().y;
    const float btm_pad = 8.0f;                              // marge sous les boutons
    // Le trait au-dessus des boutons n'est dessiné que SANS menu : la liste porte
    // déjà son propre cadre, et deux lignes horizontales à quelques pixels l'une de
    // l'autre se lisaient comme un défaut d'alignement. Hors menu, il sépare
    // utilement le texte du NPC de ses boutons — d'où la condition plutôt que la
    // suppression.
    const bool  footer_sep = menu_opts_.empty();
    const float footer_h = (footer_sep ? sep : 0.0f) + row + btm_pad;
    const float input_h = (input_mode_ != kInputNone) ? (sep + row) : 0.0f;
    // 🔴 Le MENU se mesure en PREMIER, avant que le corps ne prenne le reste : sa
    // hauteur est entièrement calculable (options, filtre, plafond) sans rien
    // dessiner. Et il arrive dans la même frame que le texte, puisque la page n'est
    // publiée qu'à son paquet terminal — d'où la disparition du réarrangement.
    float menu_grp = 0.0f;                                    // hauteur du groupe menu
    if (!menu_opts_.empty()) {
      const float body = avail - footer_h - input_h;
      // La boîte prend la hauteur de ses options (bornée à kMenuMaxRows, au-delà
      // elle scrolle) au lieu d'une fraction de la fenêtre : un menu de trois
      // choix traînait sinon une grande zone vide sous lui.
      menu_grp = MenuNaturalHeight();
      if (menu_grp > body - 48.0f) menu_grp = body - 48.0f;  // garde ~48px de texte
      if (menu_grp < 0.0f) menu_grp = 0.0f;
    }
    const float menu_total = menu_opts_.empty() ? 0.0f : (sep + menu_grp);
    float text_h = avail - footer_h - input_h - menu_total;
    if (text_h < 1.0f) text_h = 1.0f;

    // 🔴 Nouvelle page = on la lit par le HAUT. L'ancien auto-défilement vers le bas
    // déposait le joueur à la FIN du texte : sur une page qui déborde — la liste des
    // monstres d'un donjon, un panneau de règles — il tombait sur les dernières
    // lignes et devait remonter pour comprendre ce qu'il lisait. Le natif, lui,
    // affiche toujours une page neuve depuis son début.
    //
    // ⚠ SetNextWindowScroll et pas SetScrollY : ce dernier ne pose qu'une CIBLE,
    // appliquée au Begin() SUIVANT — la page neuve se serait affichée une frame au
    // défilement de l'ancienne (et clampé sur SA hauteur de contenu, donc n'importe
    // où). Posé avant BeginChild, il vaut dès cette frame. (-1 = axe X inchangé.)
    // Portrait de page (`<MOBP>`) : une colonne à gauche, réservée AVANT le corps
    // pour que le word-wrap voie la largeur réellement disponible. Rien n'est
    // réservé si le sprite est introuvable — mieux vaut le texte pleine largeur
    // qu'une colonne vide.
    //
    // ⚠ Avant le SetNextWindowScroll ci-dessous : ce réglage vaut pour la PROCHAINE
    // fenêtre ouverte, et le corps doit être celle-là.
    const bool has_portrait =
        portrait_mob_ != 0 && MobSprite(portrait_mob_) != nullptr;
    if (has_portrait) {
      DrawPortrait(kPortraitW, text_h);
      ImGui::SameLine();
    }
    if (scroll_top_) {
      ImGui::SetNextWindowScroll(ImVec2(-1.0f, 0.0f));
      scroll_top_ = false;
    }
    ImGui::BeginChild("##npctext", ImVec2(0, text_h), false);
    DrawRichLines();
    ImGui::EndChild();

    // Menu contextuel d'un lien maison (clic droit dans le corps). Ouvert ICI, dans
    // la pile d'ID de la FENÊTRE : posé depuis le child du texte, l'identifiant du
    // popup ne serait pas celui que `links::DrawMenu` recherche.
    if (link_menu_request_) {
      ImGui::OpenPopup("##npc_link_menu");
      link_menu_request_ = false;
    }
    links::DrawMenu("##npc_link_menu", link_menu_);

    // En attente de la réponse serveur, la page reste à l'identique mais devient
    // INERTE : les widgets gardent leur place (aucun saut de footer) et le joueur ne
    // peut pas répondre deux fois. Sans le grisage, un second clic sur « Suivant »
    // ferait AVANCER le script d'une page de plus.
    ImGui::BeginDisabled(awaiting_reply_);
    DrawMenu(menu_grp);
    DrawInput();

    // Footer ÉPINGLÉ : « Suivant » pour un `next` ; « Fermer »/« Annuler » seulement si
    // fermable (close/menu). Pas de fermeture pendant un `next` (le serveur avancerait).
    if (footer_sep) ImGui::Separator();  // cf. le calcul de footer_h
    // Entrée/Espace valident le bouton principal — SEULEMENT hors menu (qui consomme
    // Entrée pour son option focus) et hors saisie (l'input a son propre OK). Sans
    // repeat pour qu'un appui maintenu ne re-déclenche pas. BeginDisabled ne couvre
    // PAS le clavier : il faut l'exclure explicitement pendant l'attente.
    //
    // 🔴 `WantTextInput` MANQUAIT ICI, et c'était le trou : n'importe quelle zone de
    // texte à l'écran — la barre de chat, le filtre d'une autre fenêtre — voyait sa
    // frappe faire AUSSI avancer le script, et la moindre espace tapée valait un
    // « Suivant ». Le clavier du footer ne vaut que si personne n'écrit.
    // Et Entrée cède en plus à la barre de chat ARMÉE, qui n'a pas forcément le
    // clavier quand elle porte un lien fraîchement posé (cf. ChatOwnsEnter).
    const bool kbd_free = !ImGui::GetIO().WantTextInput;
    const bool kbd_ok =
        footer_kbd_ok && !awaiting_reply_ && kbd_free &&
        ((!ChatOwnsEnter() && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) ||
         ImGui::IsKeyPressed(ImGuiKey_Space, false));
    bool shown = false;
    if (has_next_) {
      if (ro::RoButton(i18n::Tr("Suivant")) || kbd_ok) SendNext();
      shown = true;
    }
    if (can_close) {
      if (shown) ImGui::SameLine();
      if (ro::RoButton(has_close_ ? i18n::Tr("Fermer") : i18n::Tr("Annuler")) || (kbd_ok && !shown))
        CloseDialog();
    }
    ImGui::EndDisabled();

    // Rapport de bug (GID + nom du PNJ joints) : dans la barre de titre. EN DERNIER
    // — cf. BugReport::TitleBarButton, qui ne restaure pas le curseur de layout.
    if (bug_btn) {
      auto bit = npc_names_.find(gid_);
      br->TitleBarButton(BugReport::NpcContext(
          gid_, bit != npc_names_.end() ? bit->second : std::string()));
    }
  }
  ro::EndRoDescWindow();
}

// ── Envois (thread principal uniquement) ──
// Ce filtre a perdu son gros gibier : les fenêtres de dialogue ne naissant plus,
// plus personne n'émet ces CZ dans notre dos en régime normal. Il est GARDÉ pour
// la seule fenêtre de tir qui subsiste — l'interface moderne allumée alors qu'un
// dialogue natif est déjà à l'écran : entre cet instant et la purge du tick
// suivant, une native encore vivante peut répondre à une touche.
//
// 🔴 Il ne se déclenche QUE pendant une de NOS conversations (`open_`), et pas en
// permanence comme avant. La raison est un dégât croisé : CZ_CLOSE_DIALOG 0x0146
// n'appartient pas au dialogue, la BOUTIQUE NPC s'en sert aussi pour se fermer.
// Un joueur qui allumait l'interface moderne pendant une boutique NATIVE, puis la
// fermait, voyait donc sa fermeture partir à la poubelle — et restait bloqué
// serveur, npc_id jamais nettoyé. Hors session, ces paquets ne peuvent venir que
// d'un chemin natif légitime : on les laisse passer.
bool NpcDialogWindow::ShouldSuppressNativeDialogSend(uint16_t opcode) const {
  if (!imgui_enabled_) return false;  // toggle OFF : le natif garde la main
  if (!open_) return false;           // hors conversation : ce n'est pas pour nous
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
  // has_next_ reste POSÉ : le bouton garde sa place, grisé, jusqu'à la réponse. Le
  // remettre à false ici le faisait disparaître pendant tout l'aller-retour.
  awaiting_reply_ = true;
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
  // Reste de l'époque où la fenêtre menu native (0x11) naissait à chaque menu : une
  // fois répondue elle survivait, cachée, et ré-émettait un CZ_CHOOSE_MENU parasite
  // via cmd 0x28 -> « Invalid menu selection ... got 1, valid [1..0] ». Elle ne naît
  // plus ; l'appel reste comme filet pour le menu déjà ouvert au moment où le joueur
  // allume l'interface moderne, et ne coûte qu'un FindWindow à vide.
  CloseWnd(kWinMenu);
  // La liste reste AFFICHÉE (grisée) jusqu'à la réponse : la vider ici escamotait le
  // menu et le bouton « Annuler » pendant l'aller-retour. Un second envoi est déjà
  // impossible — menu_answered_gen_ vient d'être posé.
  awaiting_reply_ = true;
  start_fresh_ = true;  // un choix de menu = saut de PAGE -> vide l'ancienne page
}

void NpcDialogWindow::SendMenuCancel() {
  if (gid_ == 0) return;
  uint8_t pkt[7];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kCzChoose;
  *reinterpret_cast<uint32_t*>(pkt + 2) = gid_;
  pkt[6] = 0xFF;  // ESC / annulation menu
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  menu_opts_.clear();
}

void NpcDialogWindow::SendNumber(int value) {
  if (gid_ == 0) return;
  uint8_t pkt[10];
  *reinterpret_cast<uint16_t*>(pkt + 0) = kCzInputN;
  *reinterpret_cast<uint32_t*>(pkt + 2) = gid_;
  *reinterpret_cast<int32_t*>(pkt + 6) = value;
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
  awaiting_reply_ = true;  // le champ reste en place, grisé, jusqu'à la réponse
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
  awaiting_reply_ = true;  // le champ reste en place, grisé, jusqu'à la réponse
}

void NpcDialogWindow::CloseDialog() {
  // Le GID de NOTRE modèle, ou à défaut celui que le CLIENT porte (+0x2DC). Le
  // défaut n'est pas théorique : c'est le cas de l'interrupteur allumé en plein
  // dialogue natif, où nous n'avons vu passer aucun paquet. Sans ce repli, la
  // fermeture ne partait pas et le personnage restait bloqué en script serveur.
  const uint32_t gid = gid_ != 0 ? gid_ : NpcInteractionGid();
  // Un menu est-il en attente d'une réponse ? Le nôtre, ou — même cas que le GID —
  // une fenêtre menu NATIVE encore à l'écran. Le distinguer compte : un script
  // arrêté sur un `select` attend un CZ_CHOOSE_MENU, et c'est lui qui le termine
  // proprement.
  //
  // 🔴 `menu_opts_` non vide ne suffit PLUS : depuis qu'un menu répondu reste affiché
  // (grisé) le temps de l'aller-retour, il faut aussi qu'il n'ait pas DÉJÀ reçu sa
  // réponse — sinon fermer pendant l'attente enverrait une annulation par-dessus le
  // choix, sur un script qui n'attend plus rien.
  const bool menu_pending =
      (!menu_opts_.empty() && menu_gen_ != menu_answered_gen_) ||
      FindWnd(kWinMenu) != nullptr;
  // 1. SERVEUR : abandon adapté à l'état (sinon sd->npc_id reste -> perso figé côté
  //    serveur). Menu ouvert -> CZ_CHOOSE_MENU 0xFF (le script reçoit 255 puis
  //    termine) ; sinon CZ_CLOSE_DIALOG.
  if (gid != 0) {
    if (menu_pending) {
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
  //    Il n'y en a plus qu'après un basculement d'interrupteur en pleine
  //    conversation ; l'ordre reste, il ne coûte rien.
  PurgeNativeDialogWindows();
  // 3. CLIENT : débloque l'état dialogue (cmd 0x28 + +0x24C=0 — c'est NOUS qui
  //    l'avons posé à l'ouverture, cf. SetNpcInteractionActive).
  DispatchNpcCmd(kSelClose);
  ClearNpcInteractionActive();
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
  return rag::NpcInteractionActive();
}

bool NpcDialogWindow::AnyNativeDialogWindow() const {
  return FindWnd(kWinSay) || FindWnd(kWinMenu) || FindWnd(kWinEditN) ||
         FindWnd(kWinEditS) || FindWnd(kWinSay2);
}

void NpcDialogWindow::PurgeNativeDialogWindows() {
  CloseWnd(kWinSay);
  CloseWnd(kWinMenu);
  CloseWnd(kWinEditN);
  CloseWnd(kWinEditS);
  CloseWnd(kWinSay2);
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

  // Changement du toggle de config (ON<->OFF) EN COURS de dialogue : fermeture
  // PROPRE (débloque serveur + client, détruit les fenêtres natives restantes).
  //
  // Le dé-masquage de sécurité qui suivait a disparu avec le masquage lui-même :
  // on ne cache plus aucune native, donc il n'y en a plus à ré-afficher. La
  // conversation est close des deux côtés, et la suivante repart sur le régime
  // choisi — natif entier, ou overlay entier.
  if (imgui_enabled_ != prev_imgui_enabled_) {
    prev_imgui_enabled_ = imgui_enabled_;
    // Trois signaux, parce qu'aucun ne suffit seul : notre session (rien à
    // l'allumage à chaud, où nous n'avons vu passer aucun paquet), le flag client
    // (qu'un prompt d'entrée sans `mes` préalable n'arme pas toujours), et les
    // fenêtres natives elles-mêmes.
    //
    // 🔴 Rater ce test, c'est laisser le personnage bloqué en script côté serveur :
    // plus aucune compétence ne part jusqu'au changement de carte. Et en régime
    // moderne, les fenêtres natives survivantes ne sont pas une porte de sortie —
    // ShouldSuppressNativeDialogSend jette justement ce qu'elles envoient.
    if (open_ || DialogActiveNative() || AnyNativeDialogWindow()) CloseDialog();
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
    ClearNpcInteractionActive();
    PurgeNativeDialogWindows();
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
    // 🔴 On DÉTRUIT les résidus au lieu de les masquer. Depuis le remplacement des
    // handlers, ces fenêtres ne naissent plus du tout : il n'en reste que si le
    // joueur a allumé l'interface moderne au milieu d'un dialogue déjà ouvert. Les
    // masquer serait alors le pire choix — une native invisible garde le clavier,
    // et Entrée ou Espace y cliqueraient le bouton par défaut, dans le dos du
    // joueur (c'est le bug du refine, docs/weapon_refine_re.md §10).
    PurgeNativeDialogWindows();
    if (!was_open_) need_pos_ = true;
  }
  was_open_ = open_;
}
