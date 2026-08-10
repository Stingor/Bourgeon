#include "features/windows/chat_window.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

#include "yaml-cpp/yaml.h"       // disposition des onglets (SaveData\bourgeon_chat.yaml)

#include "bourgeon.h"            // Bourgeon::Instance().IsGameActive / IsMapLoading
#include "d3d9/d3d9_hook.h"      // Overlay_DeviceEpoch (invalidation des textures)
#include "features/craft_data.h" // recette d'un lien <CRAF>
#include "features/item_cell.h"  // itemcell::NameById / liens <ITEML>
#include "features/moonlight_ui/moonlight_ui.h"   // iface:: (sections de <SETL>)
#include "features/staff_gate.h" // IsStaff (export des emotes)
#include "features/systems/bourgeon_opcodes.h"   // kChannelList (ZC 0x0F21)
#include "features/systems/image_preview.h"      // hôtes autorisés par le joueur
#include "imgui.h"
#include "imgui_internal.h"      // GetInputTextState (défilement interne du champ)
#include "ragnarok/globals.h"    // kModeMgrAddr / kModeMgrGetActiveAddr (dict de noms)
#include "ragnarok/msgstring.h"  // msgstr::Utf8 (refus du filtre de mots)
#include "ragnarok/uiwnd.h"      // uiwnd::SafeFindWindow / CloseWindow (natif détruit)
#include "ui/emoji_set.h"        // ro::emoji (la palette Unicode, 2e onglet)
#include "ui/game_emotes.h"      // ro::emote (emotion.act : les emotes du jeu)
#include "ui/game_texture.h"     // ro::TextureFromGameFile (bitmaps du client)
#include "ui/color_codec.h"      // ro::ArgbFromPicker / PickerFromArgb
#include "ui/icon_cache.h"       // ro::ItemIcon (icônes d'objets, cache partagé)
#include "ui/ro_imgui.h"         // BeginRoChatWindow / RoCheckbox / WireToUtf8
#include "ui/ro_widgets.h"       // HelpMarker
#include "ui/window_clamp.h"     // SetWindowMagnet (aimantation des chatbox)
#include "utils/game_paths.h"    // paths::ChatLayoutPath
#include "utils/hooking/hook_manager.h"
#include "utils/log_console.h"
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace {

// ── Adresses RE (client 20250716, base 0x400000 ; cf. docs/chatbox_re.md) ─────
//
// LE chokepoint d'ingestion : `UIWindowMgr_ChatAction(mgr, action, texte,
// couleurRGB, sender, TYPE)`, __thiscall, 5 arguments pile. action 1 = ajouter
// une ligne, 0x13 = la même chose par le msg 0x73 (voie morte côté natif, mais
// elle porte du texte).
constexpr uintptr_t kChatActionAddr = 0x00a4ad20;

// Pointeur direct vers la UINewChatWnd vivante (nul = pas de fenêtre native).
// C'est lui qui arbitre laquelle des deux sources d'ingestion est en service.
constexpr uintptr_t kNewChatWndPtr = 0x0131f6b0;

// Modèle SESSION de l'inventaire — la std::list que le client tient à jour quel
// que soit l'état de ses fenêtres (même source que les viewers). Sert ici à une
// seule question : possède-t-on l'objet dont on pose le lien ? Le client bloque
// l'envoi d'un `<ITEML>` sinon.
constexpr uintptr_t kInvListHead = 0x015fbab0;

// Le libellé visible d'un lien de RECETTE. Composé LOCALEMENT à partir du seul
// nom transporté, jamais transmis tout fait : chacun le lit ainsi dans SA langue,
// et le « [Recette: ] » d'un expéditeur anglophone n'impose rien au lecteur.
std::string RecipeLinkLabel(const std::string& product_name) {
  char buf[256];
  std::snprintf(buf, sizeof(buf), i18n::Tr("[Recette: %s]"), product_name.c_str());
  return buf;
}

// Registres de canaux : std::map<int, {nom, filtre[25]}>. Nœud MSVC :
// +0x0D isnil, +0x10 clé (index), +0x14 nom (std::string SSO 0x18 octets),
// +0x2C table de filtre BYTE[25] (un octet par TYPE de message).
constexpr uintptr_t kChannelRegistryAddr  = 0x015faadc;
constexpr uintptr_t kDetachedRegistryAddr = 0x015faae4;
constexpr size_t    kNodeIsNilOff  = 0x0D;
constexpr size_t    kNodeKeyOff    = 0x10;
constexpr size_t    kNodeNameOff   = 0x14;
constexpr size_t    kNodeFilterOff = 0x2C;
constexpr size_t    kStringSizeOff = 0x10;  // std::string : taille après le buffer SSO
constexpr size_t    kStringCapOff  = 0x14;
constexpr size_t    kSsoCapacity   = 15;

// ── Envoi (copie fidèle de ChatMacro_SendEmotionHotkeySlot 0x00a47400) ───────
// Le seul chemin d'envoi du client qui ne dépende d'AUCUNE fenêtre : il lit le
// texte, applique le filtre de mots, route les `/commandes`, et finit sur
// CMode::SendMsg. C'est exactement ce qu'il nous faut, et ça évite de
// réimplémenter la moindre règle de jeu.
constexpr uintptr_t kPendingSendText    = 0x0131f9c4;  // std::string
constexpr uintptr_t kStdStringAssign    = 0x004f1940;  // __thiscall(this, src, len)
constexpr uintptr_t kStdStringDtor      = 0x004f08f0;  // __thiscall(this)
// 🔴 CE GARDE N'EST PAS CELUI DE L'ENTRÉE — il appartient aux MACROS, et le
// copier ici refusait tout message contenant un lien d'objet.
//
// `ChatMacro_SendEmotionHotkeySlot 0x00a47400` — dont notre envoi est la copie,
// parce que c'est le seul chemin qui ne dépende d'aucune fenêtre — commence par
// `if (g_ChatWordFilterEnabled) { if (Chat_ContainsForbiddenWord(texte)) →
// msgstring 0xE53 }`. Or `0x00a23180` n'est pas un filtre de gros mots : il
// cherche SIX littéraux de balise dans le texte, et 0xE53 se lit « It cannot be
// used because it contains an Item Tag ». C'est une règle propre aux macros : une
// macro est une chaîne enregistrée, elle n'a pas d'objet à désigner.
//
// L'ENTRÉE du chat natif (`WndProc` case 6/0xB8) ne l'appelle JAMAIS. Elle ne
// refuse les balises que pour TROIS commandes slash (ids 18, 26, 55), et par un
// simple `_mbsstr` sur `<ITEML>`/`<ITEM>`, avec msgstring 0xAFC. C'est cette
// règle-là — et elle seule — que nous rejouons, en C++ (§5.1 de la doc).
//
// ⚠ Et le patch WARP `NoSwearFilter` ne sauve pas : il zérote seulement la chaîne
// `manner.txt` (la liste d'insultes ne se charge plus), mais le gestionnaire
// `0x0131F6C4` reste NON NUL — le garde s'ouvre donc, et le test de balises
// s'exécute. Les six littéraux qu'il cherche sont `<URL>`, `<NAVI>`, `<ITEM>`,
// `<ITEML>` et deux voisins : rien à voir avec un gros mot.
constexpr uintptr_t kCmdHandlerMap      = 0x00d7f1a0;  // __thiscall(ctxKey, texte)
constexpr uintptr_t kUIWindowContextKey = 0x015fa3c0;
// 🔴 __thiscall, PAS __stdcall : le natif charge `ecx = g_UIWindowContextKey`
// juste avant l'appel (0x00a4769c). Ce n'est pas décoratif — quand le nom tapé
// n'est PAS dans la table statique, la fonction retombe sur la table dynamique
// (`0x00d60804` → `sub_D5CD30(ecx, nom)`) et déréférence ce contexte. Appelée
// sans, elle part sur un ecx quelconque et lève une exception.
constexpr uintptr_t kLookupSlashCmd     = 0x00d5e590;  // __thiscall(ctx, texte, &id, args[3])
constexpr uintptr_t kCurrentModePtr     = 0x0121333c;  // CMode* courant
constexpr uintptr_t kInputTargetMode    = 0x015ff838;  // 0 public 1 groupe 2 guilde 3 clan 4 alliés
constexpr uintptr_t kOwnGuildId         = 0x0159c230;
constexpr uintptr_t kPartyMemberCount   = 0x00d5cf50;  // __thiscall(ctxKey)
constexpr uintptr_t kClanStatePtr       = 0x0159c07c;  // *(byte*)(*ptr + 0x5C) = clan
constexpr int       kSendMsgVtOff       = 0x18;        // CMode::SendMsg (vtbl+0x18)
// Les commandes de CMode::SendMsg utilisées ici (§5.2 de la doc).
constexpr int kMsgPublic  = 6;
constexpr int kMsgWhisper = 11;
constexpr int kMsgParty   = 66;   // 0x42
constexpr int kMsgGuild   = 129;  // 0x81
constexpr int kMsgClan    = 289;  // 0x121
constexpr int kMsgCommand = 42;   // 0x2A -> Chat_HandleChatMessage
// « Cette commande n'accepte pas de lien d'objet » — le refus de l'ENTRÉE
// native, réservé aux commandes slash 18/26/55.
constexpr int kMsgCmdRejectsItemTag = 0xAFC;

// Le client refuse plus de 10 canaux (principaux + détachés confondus) : au-delà,
// c'est qu'on ne lit pas un registre mais autre chose. La borne protège le
// parcours d'arbre autant que l'affichage.
constexpr int kMaxChannels = 10;

// 25 types dans la table de filtre NATIVE, plus le broadcast (0x19), qui n'y est
// pas : le client l'affiche partout, sans jamais demander son avis à personne.
constexpr int kTypeCount     = 25;
constexpr int kTypeBroadcast = 0x19;
constexpr int kTypeWhisper   = 2;

// 🔴 NOTRE table à nous en compte une de plus, et c'est toute la différence : la
// case d'index `kTypeBroadcast` (25) filtre les annonces serveur, que le natif
// laissait passer de force. Elle n'a AUCUN octet en face dans le nœud du registre
// — `WriteChannelFilter` refuse d'aller au-delà de 25, et déborder d'un octet là
// écrirait dans le champ suivant du nœud client.
constexpr int kFilterCount = kTypeCount + 1;

// L'emote qui sert d'étiquette au bouton du sélecteur : `ET_SMILE`, la seule qui
// annonce sans ambiguïté ce qu'on va trouver derrière. Déclarée ICI, avec les
// autres constantes : le bouton existe à DEUX endroits — la barre principale et
// chaque conversation 1:1 — et la première est dessinée bien avant la grille.
constexpr int kEmotePickerIcon = 18;

// ── « Friend Setup » du client (UIFriendOptionWnd, Alt+I) ────────────────────
// Les deux cases qui décident si une conversation privée s'ouvre en fenêtre, et
// la troisième qui la fait sonner. Relevées dans `UIFriendOptionWnd_OnCreate`
// 0x00701270, où chacune est posée avec son libellé : MsgString 0x169 « Open 1:1
// Chat between Strangers », 0x167 « … between Friends », 0x16A « Alarm when
// receive a 1:1 Chat ».
//
// 🔴 Ce sont les réglages du JOUEUR, pas des nôtres : reprendre une fenêtre
// native, c'est reprendre ce qui la gouverne. Qui a coupé les popups ne doit pas
// les voir revenir sous nos couleurs.
constexpr uintptr_t kFriendOptOpenFromStranger = 0x015fb2f8;
constexpr uintptr_t kFriendOptAlarm1on1        = 0x015fb2fc;
constexpr uintptr_t kFriendOptOpenFromFriend   = 0x015fb300;

// « ce nom est-il dans ma liste d'amis ? ». __thiscall, et le `this` est
// l'ADRESSE de la clé de contexte : le pivot fait `mov ecx, offset
// g_UIWindowContextKey` juste avant l'appel (0x00a2cc50).
constexpr uintptr_t kFriendListHasName = 0x00d715f0;

// ── Actions sur un joueur, par son NOM ───────────────────────────────────────
// Le champ de nom de ces paquets fait 24 octets (NAME_LENGTH côté serveur), non
// terminés par convention : c'est une taille FIXE, pas une chaîne.
constexpr size_t kNameFieldLen = 24;

// `SendMsg(0x3B, nom)` — invitation dans le groupe. C'est ce que joue le code 5
// du menu contextuel d'entité (docs/entity_context_menu_re.md §6.3), et il passe
// bien le NOM, pas l'AID.
constexpr int kMsgPartyInvite = 0x3b;

// `FriendList_AddByName(nom24)` __stdcall : construit et envoie CZ_ADD_FRIENDS.
// Désassemblée à 0x00a2c600 — `Src = 514` (0x0202), 24 octets de nom recopiés,
// longueur lue dans la table du client. ⚠ Elle lit les 24 octets d'un bloc.
using FriendAddFn = int(__stdcall*)(const void*);
constexpr uintptr_t kFriendListAddByName = 0x00a2c600;

// CZ_REQ_JOIN_GUILD2 {op, nom[24]} — invitation en guilde PAR NOM. Le menu du
// client, lui, n'invite que par AID : sans équivalent natif, on l'envoie
// nous-mêmes. Sûr : le serveur l'enregistre hors de ses blocs shuffle.
constexpr uint16_t kOpGuildInviteByName = 0x0916;

// ── Messages système masqués ─────────────────────────────────────────────────
// Liste historique (autrefois `InstallChatMessageFilter` dans bourgeon.cc, migrée
// ici avec son détour). Match par sous-chaîne : ajouter une entrée suffit à
// masquer un autre message système.
const char* const kBlockedMsgs[] = {
    "Command List: /h | /help",
    "error when loading the data account settings",
    "current shop display function is in",
};

// « No Msg » : le natif jette ces lignes silencieusement (ChatAction action 1).
// On fait pareil, sinon la fenêtre ImGui montre ce que la native n'a jamais eu.
const char* const kNoMsg[] = {"No Msg", "NO MSG"};

// Une adresse pointe-t-elle sur une image de NOTRE miroir de relais ? C'est la
// seule famille d'adresses qu'on accepte d'afficher en miniature — voir le
// commentaire au point de dessin pour la raison, qui n'est pas cosmétique.
//
// Le préfixe est écrit en dur, et c'est assumé : c'est notre propre domaine, pas
// une donnée de configuration. Le jour où il change, il change ici comme il
// changera dans groq_service.py, qui fabrique ces adresses.
bool IsMirrorImage(const std::string& url) {
  static const char kPrefix[] = "https://moonlight-destiny.fr/images/relay/";
  constexpr size_t  kLen = sizeof(kPrefix) - 1;
  return url.size() > kLen && url.compare(0, kLen, kPrefix) == 0;
}

ChatWindow* g_chat_window = nullptr;
void*       g_tramp_chat_action = nullptr;

// Copie POD des deux chaînes du client. Le tampon de texte est large : un
// broadcast dépasse allègrement une ligne de chat ordinaire.
struct RawChatLine {
  char text[1024];
  char sender[64];
};

// Recopie une chaîne du client sans jamais faire confiance à sa terminaison.
void CopyBounded(char* dst, size_t dst_size, const char* src) {
  size_t i = 0;
  if (src != nullptr) {
    for (; i + 1 < dst_size && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

// SEH pur — aucun objet C++ ici (règle MSVC C2712) : on met les chaînes du client
// à l'abri, et TOUT le reste du traitement travaille ensuite sur nos tampons.
bool SafeCopyChatStrings(const char* text, const char* sender, RawChatLine* out) {
  bool ok = false;
  __try {
    CopyBounded(out->text, sizeof(out->text), text);
    CopyBounded(out->sender, sizeof(out->sender), sender);
    ok = out->text[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    ok = false;
  }
  return ok;
}

// La chatbox NATIVE existe-t-elle encore ? C'est la question qui décide, partout,
// si c'est à nous d'ingérer : tant qu'elle vit, son WndProc (`case 0x25`) nous
// alimente et ingérer en plus doublerait la ligne. SEH pur (règle C2712).
bool NativeChatAlive() {
  bool alive = false;
  __try {
    alive = *reinterpret_cast<void**>(kNewChatWndPtr) != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    alive = false;
  }
  return alive;
}

// Le corps du détour. Renvoie non-nul pour que le stub neutralise l'action : la
// ligne n'est alors ajoutée NULLE PART (ni natif, ni chez nous).
//
// 🔴 ORDRE DES DEUX DERNIERS ARGUMENTS : c'est **(…, TYPE, sender)**, et non
// « (…, sender, TYPE) » comme l'annonçait le commentaire de l'IDB. La preuve est
// dans le relais vers le WndProc (`0x00A4B245`) :
//
//     push [ebp+arg_10]  ; -> p5
//     push [ebp+var_94]  ; -> p4
//     push [ebp+var_90]  ; -> p3  (couleur)
//     push esi           ; -> p2  (texte)
//
// et `chat.cc`, dont l'ingestion donnait bien des types VARIÉS, lit **p4** comme
// le type. Donc `var_94` (le 4ᵉ argument pile) est le TYPE, et `arg_10` (le 5ᵉ)
// le sender. Les avoir intervertis nous faisait lire un POINTEUR en guise de
// type ; l'écrêtage de `Ingest` le ramenait à 0, d'où « tout arrive en t00 ».
// Le symptôme n'apparaissait qu'une fois la native détruite, puisque tant qu'elle
// vivait c'est son WndProc — donc le bon ordre — qui nous alimentait.
// L'AID que le client affiche entre crochets est OBFUSQUÉ : `Aid_FormatObfuscated
// 0x00d56e60` déroule les dix chiffres décimaux et substitue chacun, supprime les
// zéros de tête, et insère un « - » avant les trois derniers. On rejoue la table
// à l'envers, parce que l'ouverture par le menu contextuel ne nous donne QUE
// cette chaîne — et que sans l'AID réel, la guilde du correspondant resterait
// introuvable.
//
// Substitution du client : 0->'3' 1->'8' 2->'6' 3->'7' 4->'0' 5->'1' 6->'2'
// 7->'4' 8->'9' 9->'5'. Vérifié : « 6333-317 » redonne 2000053.
//
// Rend 0 si la chaîne n'a pas cette forme — un AID nul est traité partout comme
// « inconnu », ce qui est exactement le bon repli.
uint32_t DeobfuscateAid(const char* display) {
  if (display == nullptr) return 0;
  // index = chiffre AFFICHÉ, valeur = chiffre RÉEL. '0'->4 '1'->5 '2'->6 '3'->0
  // '4'->7 '5'->9 '6'->2 '7'->3 '8'->1 '9'->8.
  static const char kInverse[] = "4560792318";
  uint32_t value = 0;
  int digits = 0;
  for (const char* p = display; *p != '\0'; ++p) {
    if (*p == '-') continue;
    if (*p < '0' || *p > '9') return 0;
    if (++digits > 10) return 0;  // au-delà, ce n'est plus un AID
    value = value * 10u + static_cast<uint32_t>(kInverse[*p - '0'] - '0');
  }
  return digits != 0 ? value : 0;
}

// ── Bavardage automatique des pets : marqué AILLEURS ─────────────────────────
// Une réplique de pet n'a aucune signature dans ce qui arrive à `ChatAction` : le
// type vaut 0 comme le système, le sender est vide, la couleur 0xFAFAFA n'a rien
// d'exclusif. On ne peut donc PAS la reconnaître ici — seule la pile la trahit
// (on est alors dans `PetAct_OnPacket 0x00cd13f0`).
//
// Le marquage est fait par le patch WARP **PetTalkMarker**, qui redirige le
// format `"%s : "` de `PetTalk_FormatChatLine 0x00d83560` vers une chaîne portant
// le marqueur. Il vaut pour tous les joueurs, DLL ou pas — c'est ce qui a
// tranché. Voir docs/chatbox_re.md §12 pour tout le chemin.
int __cdecl ChatActionFilter(int action, const char* text, int color,
                             int type, const char* sender) {
  // 🔴 Action 3 = `ToggleWindow(mgr, 1)` + msg 0x10 : le client RECRÉE sa chatbox
  // pour déployer sa barre de saisie. La détruire après coup depuis OnProcessInput
  // laissait la native visible quelques frames, le temps d'un aller-retour. On
  // l'empêche donc de naître — c'est tout ce que fait ce `return 1`.
  //
  // 🔴 ET ON N'OUVRE RIEN CHEZ NOUS. L'intention semble pourtant claire (« déplie
  // la saisie »), mais deux règles se croisent : une barre ouverte a le clavier
  // (l'invariant du battle mode, cf. chat_window.h), et le client émet cette
  // action à tout bout de champ — le handler d'annonce `sub_00CB7510` l'appelle
  // (0x00cb7957) comme des dizaines d'autres handlers de paquets. Obéir, c'est
  // donc voler les touches de déplacement du joueur à chaque annonce du serveur ;
  // ouvrir sans le clavier, c'est fabriquer l'état « ouverte sans focus » qui
  // coûte une Entrée à la sortie. Il ne reste qu'à ne pas ouvrir, et rien n'est
  // perdu : le battle mode ne masque QUE la ligne de saisie, le log continue
  // d'afficher l'annonce.
  if (action == 3 && g_chat_window != nullptr && g_chat_window->imgui_enabled_)
    return 1;
  // 🔴 Action 14 = `UIM_MAKE_WHISPER_WINDOW` : LE SECOND chemin d'ouverture d'une
  // conversation 1:1, et il ne passe PAS par le pivot que nous détournons. Le
  // « Chuchoter » du menu contextuel d'entité appelle `ChatAction(mgr, 14, …)`
  // directement (0x00c888c3, `mov ecx, 0131F4E8h` / `push esi` / `push eax` /
  // `push 0Eh`) — d'où une fenêtre NATIVE qui revenait par-dessus la nôtre.
  //
  // Les arguments ne portent pas ici le sens que leurs noms annoncent, qui est
  // celui d'une ligne de chat : p2 = le NOM, p3 = un suffixe (« GuildMember » ou
  // rien), p4 = l'AID déjà OBFUSQUÉ en chaîne. Vérifié : aucun des deux appelants
  // ne teste la valeur de retour de ce case-là.
  if (action == 14 && g_chat_window != nullptr && g_chat_window->imgui_enabled_) {
    g_chat_window->OpenWhisperWindow(text, reinterpret_cast<const char*>(type));
    return 1;
  }
  if (action != 1 && action != 0x13) return 0;

  RawChatLine raw;
  if (!SafeCopyChatStrings(text, sender, &raw)) return 0;

  for (const char* pattern : kBlockedMsgs)
    if (std::strstr(raw.text, pattern) != nullptr) return 1;
  for (const char* pattern : kNoMsg)
    if (std::strcmp(raw.text, pattern) == 0) return 0;  // jetée, mais pas bloquée au natif

  // Ingestion SEULEMENT si la fenêtre native n'existe pas : sinon c'est son
  // WndProc qui nous alimente (cf. chatwnd::IngestNativeLine), et ingérer des
  // deux côtés doublerait chaque ligne.
  const bool native_alive = NativeChatAlive();
  if (!native_alive && g_chat_window != nullptr) {
    // 🔴 L'ACTION 0x13 EST UNE VOIE MORTE, ET C'EST ELLE QUI DOUBLAIT LES ANNONCES.
    // `ChatAction` traite 1 et 0x13 par le même code, à un détail près : 1 envoie le
    // msg 0x25 (add-line) à la chatbox, 0x13 le msg 0x73 — que le WndProc ignore
    // (`if (msg == 0x73) return 0`). Or le handler d'annonce `sub_00CB7510` appelle
    // les DEUX d'affilée sur le même texte :
    //     0x00cb7983  ChatAction(mgr, 1,    txt, couleur, type 0x19)
    //     0x00cb79a9  ChatAction(mgr, 0x13, txt, couleur, type 0x19)
    // Le natif n'en affiche donc qu'une, alors que nous ingérions les deux — d'où
    // des doublons sur le SEUL type 25 (broadcast), et nulle part ailleurs.
    // On la laisse muette, mais on la BLOQUE quand même juste en dessous : sans
    // chatbox native, l'une comme l'autre empile dans la file `mgr+0x4C4`, qui
    // n'est plus jamais drainée.
    if (action != 0x13) {
      ++g_chat_window->ingest_seen_;
      g_chat_window->Ingest(raw.text, static_cast<uint32_t>(color), raw.sender, type,
                            'A');
    }
    // 🔴 BLOQUER, mais seulement quand NOTRE fenêtre a pris le relais. Sans
    // fenêtre native pour la consommer, `ChatAction` empile la ligne dans la file
    // `mgr+0x4C4`, qui n'est drainée qu'à la CRÉATION d'une fenêtre — donc jamais
    // si l'on empêche celle-ci de naître. C'est une fuite mémoire sans plafond,
    // et elle grossit d'autant plus vite que le joueur est dans une ville bavarde.
    //
    // Si l'interface moderne est éteinte, on laisse passer : la native est peut-
    // être seulement pas encore créée (écran de chargement), et la file lui
    // rendra ses lignes en naissant.
    if (g_chat_window->imgui_enabled_) return 1;
  }
  return 0;
}

// ── Le crash des balises quand la chatbox native n'existe plus ───────────────
// `ChatText_TransformTagLinks 0x008e1730` transforme les balises/liens d'un texte
// et prend la UINewChatWnd pour `this` — elle porte la liste des transformateurs à
// `+0xF4`. Ses QUINZE appelants la joignent en lisant `g_pNewChatWnd 0x0131f6b0`
// SANS le tester : fenêtre détruite, `this` vaut 0, et `mov edx,[eax+0xF4]` fait
// sauter le client. Le premier rencontré est le message de bienvenue du serveur
// (ZC_BROADCAST2 0x01C3), qui arrive à chaque entrée en jeu — d'où un crash
// systématique dès que notre chatbox remplace la native. Les quatorze autres
// (effets d'apparence, suivi de quête, les cinq UIRichTextBox_Layout*) étaient
// autant de bombes à retardement.
//
// 🔴 Le correctif ne peut pas se contenter de « ne rien faire » : la chaîne de
// SORTIE arrive NON INITIALISÉE. La laisser telle quelle fait planter le
// destructeur que l'appelant exécute juste après ; la rendre vide afficherait un
// message vide, l'appelant déplaçant le résultat dans son tampon. On construit
// donc la sortie comme une COPIE de l'entrée — le texte passe sans ses balises
// transformées, ce qui est exactement le comportement dégradé attendu.
constexpr uintptr_t kChatTagTransform  = 0x008e1730;  // __thiscall(this, out, in), retn 8
constexpr uintptr_t kStdStringCopyCtor = 0x004e52a0;  // __thiscall(dst, src), retn 4
void* g_tramp_chat_tags = nullptr;

__declspec(naked) void ChatTagTransformStub() {
  __asm {
    test ecx, ecx
    jnz  tags_chain           // fenêtre vivante : rien à faire, on chaîne
    // À l'entrée : [esp+4] = sortie (non construite), [esp+8] = entrée.
    mov  eax, [esp+8]         // src
    mov  ecx, [esp+4]         // dst  (this du copy-ctor)
    push ecx                  // on garde dst pour le rendre en eax
    push eax                  // argument pile du copy-ctor
    mov  edx, kStdStringCopyCtor
    call edx                  // retn 4 : il dépile son propre argument
    pop  eax                  // la fonction rend la chaîne de SORTIE
    ret  8                    // même nettoyage que l'originale
  tags_chain:
    jmp  [g_tramp_chat_tags]
  }
}

// Deuxième famille du même défaut. `UINewChatWnd_ToggleInputBar 0x008dc0d0`
// déplie/replie la ligne de saisie (c'est le mécanisme du battle mode) et
// déréférence `this` dès sa première ligne — `this+0xBC`, la boîte de saisie. Ses
// SIX appelants lisent `g_pNewChatWnd` sans le tester :
// `UIWindowMgr_DispatchHotkeyBehavior`, `UIWindowMgr_DispatchMouseInput`,
// `CCashEmotion_OnClickEmotionButton`, `sub_5AB550` (×2) et la commande `/bm`.
// Autrement dit : une touche de raccourci ou un clic suffisait à faire sauter le
// client dès que la native n'existe plus.
//
// Ici, rien à reconstruire : la valeur de retour est ignorée par tous les
// appelants. On rend simplement zéro.
// Drapeau du client, persisté sous le nom `"ChangeChatMode"` : **1 = battle mode**
// (barre masquée, Entrée l'ouvre, envoi à vide la referme), 0 = barre permanente.
// Basculé par la case 135 de `Chat_HandleChatMessage` (`/bm`, `/battlemode`) au
// moyen d'un `setz` — c'est une vraie bascule, un « on »/« off » en argument est
// ignoré.
constexpr uintptr_t kBattleModeFlag     = 0x0131f50e;
constexpr uintptr_t kChatToggleInputBar = 0x008dc0d0;  // __thiscall(this), retn 0
void* g_tramp_chat_togglebar = nullptr;

__declspec(naked) void ChatToggleInputBarStub() {
  __asm {
    test ecx, ecx
    jnz  bar_chain
    xor  eax, eax
    ret                       // l'originale ne dépile aucun argument
  bar_chain:
    jmp  [g_tramp_chat_togglebar]
  }
}

// Détour d'entrée. Sauve eax/ecx/edx, empile les cinq arguments pile de
// ChatAction pour ChatActionFilter (__cdecl), et si celui-ci veut bloquer, écrit
// 0x7fffffff sur `action` : le switch du natif tombe dans son default no-op et
// fait son propre épilogue (RET N) — zéro risque ABI.
//
// Après les trois push : action@esp+0x10, texte@+0x14, couleur@+0x18,
// sender@+0x1c, type@+0x20. Chaque push décale esp de 4 et l'argument suivant est
// 4 plus loin : les cinq lectures se font donc toutes à +0x20.
__declspec(naked) void ChatActionStub() {
  __asm {
    push eax
    push ecx
    push edx
    mov  eax, [esp+0x20]   // type
    push eax
    mov  eax, [esp+0x20]   // sender
    push eax
    mov  eax, [esp+0x20]   // couleur
    push eax
    mov  eax, [esp+0x20]   // texte
    push eax
    mov  eax, [esp+0x20]   // action
    push eax
    call ChatActionFilter
    add  esp, 0x14
    test eax, eax
    jz   chat_pass
    mov  dword ptr [esp+0x10], 0x7fffffff  // -> switch default (aucune ligne)
  chat_pass:
    pop  edx
    pop  ecx
    pop  eax
    jmp  [g_tramp_chat_action]
  }
}

// ── Chuchotement 1:1 : le pivot du client ────────────────────────────────────
// `UIWindowMgr_OnWhisperReceived(mgr, nom, texte, couleur, aid)` — __thiscall,
// 4 arguments pile, `retn 0x10`. C'est LE point de passage des conversations
// privées, dans les deux sens : le handler de ZC_WHISPER l'appelle pour ce qu'on
// reçoit, et l'acquittement de nos propres envois (`Whisper_DispatchSendResult`
// 0x00c9d030) pour l'écho de ce qu'on envoie. Les deux sens arrivent donc ici
// encore séparés et proprement typés — c'est pour ça qu'on se branche là plutôt
// que sur le texte déjà mis en forme.
//
// 🔴 SA VALEUR DE RETOUR EST UN CONTRAT, pas un état : 1 = « une fenêtre 1:1 a
// consommé la ligne », et l'appelant s'abstient alors de l'écrire dans la
// chatbox. Le rendre nous-mêmes évite le doublon sans avoir à filtrer quoi que
// ce soit plus loin. ⚠ Le reste de ce que fait l'appelant sur un retour 1 — la
// réponse automatique d'absence (MsgString 0x3AD, drapeau 0x015ffa58) — vit chez
// LUI, pas ici : il continue de tourner, on ne lui retire rien.
constexpr uintptr_t kWhisperPivotAddr = 0x00a2cc20;
void* g_tramp_whisper = nullptr;

// Les deux couleurs du client pour une ligne de conversation privée, relevées à
// 0x008cdd5a (`cmp [ebp+14h], 7800h` / `mov eax, 0E8DDB6h` / `mov ebx, 0FFFFh` /
// `cmovnz`). Ce sont des COLORREF 0x00BBGGRR, comme toute couleur de LIGNE ici.
constexpr int      kWhisperEchoTag  = 0x7800;    // couleur-marqueur de NOTRE envoi
constexpr uint32_t kWhisperRecvRgb  = 0xE8DDB6;  // reçu : bleu-gris pâle
constexpr uint32_t kWhisperEchoRgb  = 0x00FFFF;  // envoyé : jaune

// Notre propre nom de personnage, dans la code-page du fil. C'est le getter que
// le client utilise lui-même pour composer l'écho d'un chuchotement
// (`Whisper_DispatchSendResult`, `Own_GetCharName(g_UIWindowContextKey)`).
// POD + SEH : aucun objet à destructeur ici (C2712).
constexpr uintptr_t kOwnGetCharName = 0x00d7fe40;  // __thiscall(ctxKey) -> char*

bool ReadOwnCharName(char* out, size_t out_size) {
  using OwnNameFn = const char*(__thiscall*)(void*);
  __try {
    const char* name = reinterpret_cast<OwnNameFn>(kOwnGetCharName)(
        reinterpret_cast<void*>(kUIWindowContextKey));
    if (name == nullptr || name[0] == '\0') return false;
    size_t n = 0;
    for (; n + 1 < out_size && name[n] != '\0'; ++n) out[n] = name[n];
    out[n] = '\0';
    return n != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Le client ouvrirait-il une popup pour ce correspondant ? Rejoue exactement le
// test du pivot : ami ⇒ la case « Friends », inconnu ⇒ la case « Strangers ».
bool WhisperPopupWanted(const char* name_wire) {
  using HasNameFn = int(__thiscall*)(void*, const char*);
  __try {
    const bool is_friend =
        reinterpret_cast<HasNameFn>(kFriendListHasName)(
            reinterpret_cast<void*>(kUIWindowContextKey), name_wire) != 0;
    const uint32_t* option = reinterpret_cast<const uint32_t*>(
        is_friend ? kFriendOptOpenFromFriend : kFriendOptOpenFromStranger);
    return *option != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Les deux cases du client, lues et écrites là où il les garde. Rien n'est
// recopié chez nous : c'est le même mot de quatre octets des deux côtés.
//
// ⚠ Ce sont des DWORD, pas des booléens d'un octet — `UIFriendOptionWnd_OnMsg`
// leur assigne l'argument entier de la case (cmd 213), et son OnCreate les
// repasse tels quels à `UIToggleButton_SetState`. Écrire un seul octet
// laisserait les trois autres tels quels.
//
// Vérifié : cocher la case native n'écrit QUE ce mot — aucune sauvegarde, aucun
// paquet, aucun effet de bord. Notre écriture lui est donc strictement
// équivalente. (En contrepartie le client ne persiste pas ces trois réglages : ni
// les siens ni les nôtres ne survivent à une fermeture.)
bool ReadWhisperPopupOptions(bool* from_stranger, bool* from_friend) {
  __try {
    *from_stranger = *reinterpret_cast<const uint32_t*>(kFriendOptOpenFromStranger) != 0;
    *from_friend   = *reinterpret_cast<const uint32_t*>(kFriendOptOpenFromFriend) != 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void WriteWhisperPopupOption(bool stranger, bool on) {
  __try {
    *reinterpret_cast<uint32_t*>(stranger ? kFriendOptOpenFromStranger
                                          : kFriendOptOpenFromFriend) = on ? 1u : 0u;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

int __cdecl WhisperFilter(const char* name, const char* text, int color,
                          unsigned int aid) {
  // Interface moderne éteinte : on ne touche à rien. Le joueur est alors sur la
  // chatbox native, et ses popups natives doivent continuer de s'ouvrir — les
  // priver de leur pivot le laisserait sans conversation privée du tout.
  if (g_chat_window == nullptr || !g_chat_window->imgui_enabled_) return 0;
  if (name == nullptr || name[0] == '\0' || text == nullptr) return 0;

  // La couleur-marqueur dit le SENS, et c'est la seule chose qui le dise : le
  // texte, lui, est écrit pour la fenêtre 1:1 (« <qui parle> : … ») et ne porte
  // aucun « To »/« From ». Le journal en a besoin pour son en-tête.
  const bool     outgoing = (color == kWhisperEchoTag);
  const uint32_t rgb      = outgoing ? kWhisperEchoRgb : kWhisperRecvRgb;
  return g_chat_window->IngestWhisper(name, text, rgb, aid, outgoing) ? 1 : 0;
}

// Même construction que ChatActionStub : après les trois push de sauvegarde, un
// push de plus décale d'autant l'argument suivant — les quatre lectures se font
// donc toutes à [esp+0x1C]. Si le filtre a pris la ligne, on rend 1 et on fait
// l'épilogue nous-mêmes (`ret 0x10`, les quatre arguments pile) sans jamais
// entrer dans l'originale : c'est elle qui créerait la popup native.
__declspec(naked) void WhisperPivotStub() {
  __asm {
    push eax
    push ecx
    push edx
    mov  eax, [esp+0x1C]   // aid
    push eax
    mov  eax, [esp+0x1C]   // couleur
    push eax
    mov  eax, [esp+0x1C]   // texte
    push eax
    mov  eax, [esp+0x1C]   // nom
    push eax
    call WhisperFilter
    add  esp, 0x10
    test eax, eax
    jz   whisper_chain
    pop  edx
    pop  ecx
    pop  eax
    mov  eax, 1            // « une fenêtre 1:1 l'a prise »
    ret  0x10
  whisper_chain:
    pop  edx
    pop  ecx
    pop  eax
    jmp  [g_tramp_whisper]
  }
}

// ── Le « détecteur de sosies » du client : neutralisé AILLEURS ───────────────
// `Name_IsLookalike 0x00d56bf0` devait repérer les usurpations par homoglyphes (le
// `I` majuscule qui imite un `l` minuscule) mais son seuil — `min(len) - 2`
// correspondances de position — le fait crier sur des noms sans rapport, à peu
// près tout le temps. C'est la source des MsgString 0x395 et 0x397 sur les
// chuchotements.
//
// La neutralisation est faite par le patch WARP **NoLookalikeNameWarning**, pas
// ici : elle ne dépend pas de notre chatbox, et sous forme de patch elle profite
// à tous les joueurs. Voir docs/chatbox_re.md §11.8 pour la portée vérifiée.

// ── Envoi natif ──────────────────────────────────────────────────────────────
// Une std::string MSVC telle que le CLIENT les manipule : buffer SSO de 16
// octets, taille, capacité. Construite vide (capacité 15) et détruite par le
// destructeur du client — si elle a dû allouer, c'est SON allocateur qui a servi.
struct NativeString {
  char     buf[16];
  uint32_t size;
  uint32_t capacity;
};

void NativeStringInit(NativeString* s) {
  std::memset(s, 0, sizeof(*s));
  s->capacity = static_cast<uint32_t>(kSsoCapacity);
}

using StdStringAssign_t = void*(__thiscall*)(void*, const char*, size_t);
using StdStringDtor_t   = void(__thiscall*)(void*);
using CmdHandlerMap_t   = int(__thiscall*)(void*, const char*);
using PartyCount_t      = int(__thiscall*)(void*);
using LookupSlashCmd_t  = int(__thiscall*)(void*, const char*, int*, void*);
using SendMsg_t         = void(__thiscall*)(void*, int, int, int, int, int);

// Le texte porte-t-il une balise d'objet ? Le natif fait exactement ce test, au
// `_mbsstr`, sur les deux littéraux — et seulement pour trois commandes slash.
bool ContainsItemTag(const char* text) {
  return std::strstr(text, "<ITEML>") != nullptr ||
         std::strstr(text, "<ITEM>") != nullptr;
}

// Les trois commandes qui refusent un lien d'objet (`WndProc` case 6/0xB8).
bool CommandRejectsItemTag(int cmd_id) {
  return cmd_id == 18 || cmd_id == 26 || cmd_id == 55;
}

// `g_ChatPendingSendText = text` : le tampon que TOUS les envois relisent.
void SetPendingSendText(const char* text) {
  reinterpret_cast<StdStringAssign_t>(kStdStringAssign)(
      reinterpret_cast<void*>(kPendingSendText), text, std::strlen(text));
}

// CMode::SendMsg(cmd, p2..p5) sur le mode de zone courant. Rend false si aucun
// mode n'est actif (écran de login, changement de carte).
bool ModeSendMsg(int cmd, int p2 = 0, int p3 = 0, int p4 = 0, int p5 = 0) {
  void* mode = *reinterpret_cast<void**>(kCurrentModePtr);
  if (mode == nullptr) return false;
  void** vt = *reinterpret_cast<void***>(mode);
  reinterpret_cast<SendMsg_t>(vt[kSendMsgVtOff / 4])(mode, cmd, p2, p3, p4, p5);
  return true;
}

// Envoie `text` (code-page du fil) exactement comme l'ENTER natif : commandes,
// modes d'envoi, chuchotement. `whisper_target` non vide = chuchotement, comme la
// box destinataire du chat natif. Rend un message d'erreur à afficher, ou nullptr.
// Code de la dernière exception attrapée dans le chemin d'envoi. Hors du __try :
// une variable locale modifiée dans le filtre SEH n'est pas fiable.
DWORD g_last_send_fault = 0;

const char* NativeSendChatText(const char* text, const char* whisper_target) {
  if (text == nullptr || text[0] == '\0') return nullptr;

  const char* error = nullptr;
  __try {
    if (text[0] == '/') {
      // Commandes : d'abord la map de handlers (commandes désactivables par le
      // serveur) ; si elle a traité, c'est fini. Sinon la table slash historique
      // donne l'id et jusqu'à trois arguments, et Chat_HandleChatMessage exécute.
      void* ctx = reinterpret_cast<void*>(kUIWindowContextKey);
      if (reinterpret_cast<CmdHandlerMap_t>(kCmdHandlerMap)(ctx, text) == 0) {
        NativeString args[3];
        for (NativeString& arg : args) NativeStringInit(&arg);
        int cmd_id = 0;
        const int arg_off = reinterpret_cast<LookupSlashCmd_t>(kLookupSlashCmd)(
            ctx, text, &cmd_id, args);
        // Trois commandes refusent un lien d'objet — c'est le SEUL endroit où
        // l'ENTRÉE native teste les balises, et le refus est un message de chat,
        // pas une modale (qui relancerait le rendu ; cf. l'en-tête).
        if (CommandRejectsItemTag(cmd_id) && ContainsItemTag(text)) {
          error = msgstr::Utf8(kMsgCmdRejectsItemTag);
        } else {
          if (arg_off != -1) SetPendingSendText(text + arg_off);
          ModeSendMsg(kMsgCommand, cmd_id,
                      static_cast<int>(reinterpret_cast<intptr_t>(args)));
        }
        for (NativeString& arg : args)
          reinterpret_cast<StdStringDtor_t>(kStdStringDtor)(&arg);
      }
    } else if (whisper_target != nullptr && whisper_target[0] != '\0') {
      // Chuchotement : même chemin que la box destinataire du chat natif —
      // texte en attente, puis SendMsg(11, nom).
      SetPendingSendText(text);
      ModeSendMsg(kMsgWhisper,
                  static_cast<int>(reinterpret_cast<intptr_t>(whisper_target)));
    } else {
      SetPendingSendText(text);
      // Mode d'envoi : le client REFUSE de basculer vers un canal auquel on
      // n'appartient pas (pas de groupe, pas de guilde, pas de clan) et retombe
      // sur le public. On rejoue ses trois gardes.
      const int mode = *reinterpret_cast<int*>(kInputTargetMode);
      bool sent = false;
      if (mode == 2) {
        if (*reinterpret_cast<uint32_t*>(kOwnGuildId) != 0)
          sent = ModeSendMsg(kMsgGuild);
      } else if (mode == 1) {
        void* ctx = reinterpret_cast<void*>(kUIWindowContextKey);
        if (reinterpret_cast<PartyCount_t>(kPartyMemberCount)(ctx) != 0)
          sent = ModeSendMsg(kMsgParty);
      } else if (mode == 3) {
        const uint8_t* clan = *reinterpret_cast<const uint8_t**>(kClanStatePtr);
        if (clan != nullptr && clan[0x5C] != 0) sent = ModeSendMsg(kMsgClan);
      }
      if (!sent) ModeSendMsg(kMsgPublic);
    }
  } __except (g_last_send_fault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    // 🔴 Le code de l'exception, PAS un message générique. « L'envoi a échoué »
    // tout seul a coûté une session entière : il ne dit ni où, ni quoi. Avec le
    // code (0xC0000005 = déréférencement, 0xC0000409 = pile corrompue = mauvaise
    // convention d'appel), la panne se lit sans debugger.
    static char buffer[80];
    std::snprintf(buffer, sizeof(buffer), i18n::Tr("L'envoi a échoué (chemin natif, 0x%08X)."),
                  g_last_send_fault);
    error = buffer;
  }
  return error;
}

// ── Lecture des registres de canaux ──────────────────────────────────────────
// Forme POD, pour rester sous SEH d'un bout à l'autre du parcours d'arbre.
struct RawChannel {
  uintptr_t node;
  int       index;
  char      name[64];
  uint8_t   filter[kTypeCount];
};

// Lit une std::string MSVC (SSO 15 caractères, sinon pointeur) dans `out`.
void ReadStdString(const uint8_t* str, char* out, size_t out_size) {
  const uint32_t capacity = *reinterpret_cast<const uint32_t*>(str + kStringCapOff);
  const uint32_t size     = *reinterpret_cast<const uint32_t*>(str + kStringSizeOff);
  const char* data = (capacity > kSsoCapacity)
                         ? *reinterpret_cast<const char* const*>(str)
                         : reinterpret_cast<const char*>(str);
  size_t n = 0;
  if (data != nullptr) {
    for (; n + 1 < out_size && n < size; ++n) out[n] = data[n];
  }
  out[n] = '\0';
}

// ── Guilde d'un correspondant ────────────────────────────────────────────────
// Sonde POD, pour rester sous SEH d'un bout à l'autre : `__try` et les objets à
// destructeur ne cohabitent pas (C2712), et la conversion des chaînes se fait
// donc chez l'appelant.
struct GuildProbe {
  uint32_t aid;
  char     guild[64];
};

// Le dictionnaire de noms du client (`GameMode+0x160`), celui-là même qui nourrit
// les noms flottants au-dessus des personnages — docs/entity_nameplate_re.md.
// `CNameDict_GetEntryOrRequest` rend le bloc CNameInfo s'il est connu, et sinon
// met le GID en file de REQUÊTE serveur : c'est ce qui fait apparaître la guilde
// quelques frames plus tard, sans que nous ayons de paquet à écrire.
constexpr uintptr_t kNameDictGetEntryOrRequest = 0x005a1460;
constexpr int       kGmNameDict   = 0x160;
constexpr int       kNameInfoGuild = 0x34;  // nom +0x04, party +0x1C, guilde +0x34

bool ProbeGuildsFromNameDict(GuildProbe* items, int count) {
  if (count <= 0) return false;
  using GetActiveFn    = void*(__fastcall*)(int);
  using GetNameEntryFn = void*(__thiscall*)(void*, unsigned);
  __try {
    void* gm = reinterpret_cast<GetActiveFn>(rag::kModeMgrGetActiveAddr)(
        static_cast<int>(rag::kModeMgrAddr));
    if (gm == nullptr) return false;
    void* dict = reinterpret_cast<uint8_t*>(gm) + kGmNameDict;
    auto get_entry = reinterpret_cast<GetNameEntryFn>(kNameDictGetEntryOrRequest);
    for (int i = 0; i < count; ++i) {
      void* entry = get_entry(dict, items[i].aid);
      if (entry == nullptr) continue;
      ReadStdString(reinterpret_cast<const uint8_t*>(entry) + kNameInfoGuild,
                    items[i].guild, sizeof(items[i].guild));
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Dictionnaire indisponible (changement de carte) : on retentera.
    return false;
  }
}

// Parcours de l'arbre rouge-noir d'un std::map MSVC : l'objet map porte
// {_Myhead, _Mysize} ; _Myhead->parent (+4) est la racine, et la sentinelle
// boucle sur elle-même. On borne à `out_max` — au-delà, ce n'est pas un registre
// de chat, et une structure inattendue ne doit pas faire tourner la boucle.
int ReadRegistry(uintptr_t registry_addr, RawChannel* out, int out_max) {
  int count = 0;
  __try {
    const uint8_t* head = *reinterpret_cast<const uint8_t* const*>(registry_addr);
    if (head == nullptr) return 0;
    const uint8_t* stack[kMaxChannels * 2 + 4];
    int depth = 0;
    stack[depth++] = *reinterpret_cast<const uint8_t* const*>(head + 4);  // racine
    while (depth > 0 && count < out_max) {
      const uint8_t* node = stack[--depth];
      if (node == nullptr || node == head) continue;
      if (node[kNodeIsNilOff] != 0) continue;  // sentinelle, pas une valeur
      out[count].node  = reinterpret_cast<uintptr_t>(node);
      out[count].index = *reinterpret_cast<const int*>(node + kNodeKeyOff);
      ReadStdString(node + kNodeNameOff, out[count].name, sizeof(out[count].name));
      for (int i = 0; i < kTypeCount; ++i)
        out[count].filter[i] = node[kNodeFilterOff + i];
      ++count;
      if (depth + 2 <= static_cast<int>(_countof(stack))) {
        stack[depth++] = *reinterpret_cast<const uint8_t* const*>(node + 0);  // gauche
        stack[depth++] = *reinterpret_cast<const uint8_t* const*>(node + 8);  // droite
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return count;  // ce qu'on a pu lire reste valable
  }
  return count;
}

// Pose l'octet de filtre d'un canal, exactement comme le fait la fenêtre native
// d'options de log (UIBattleMsgOptionWnd_OnClickCheckbox écrit node+0x2C+type).
//
// 🔴 La borne `kTypeCount` — et NON `kFilterCount` — est ce qui protège le nœud :
// notre case broadcast n'a pas d'équivalent chez le client, et l'octet qui la
// suivrait à node+0x2C+25 appartient à autre chose. Le refus est silencieux parce
// que l'appelant, lui, coche les 26 cases sans avoir à savoir laquelle est nôtre.
void WriteChannelFilter(uintptr_t node, int type, bool on) {
  if (node == 0 || type < 0 || type >= kTypeCount) return;
  __try {
    *reinterpret_cast<uint8_t*>(node + kNodeFilterOff + type) = on ? 1 : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── Bitmaps du client utilisés par la chatbox ────────────────────────────────
// Les noms viennent de `UINewChatWnd_Create` : les boutons du chat sont des
// UIBitmapButton à deux états (`_a` normal, `_b` survol/pressé).
const char kUiDirCp949[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";

ro::GameTexture ChatBitmap(const char* rel_path) {
  struct Entry {
    ro::GameTexture tex;
    unsigned        epoch = 0;
    bool            tried = false;
  };
  static std::unordered_map<std::string, Entry> cache;
  Entry& entry = cache[rel_path];
  const unsigned epoch = Overlay_DeviceEpoch();
  if (entry.tried && entry.epoch == epoch) return entry.tex;

  char full[260];
  std::snprintf(full, sizeof(full), "%s\\%s", kUiDirCp949, rel_path);
  entry.tex   = ro::TextureFromGameFile(full);
  entry.epoch = epoch;
  entry.tried = true;
  return entry.tex;
}

// Mode d'envoi COURANT du client (0 public, 1 groupe, 2 guilde, 3 clan,
// 4 alliés). On lit le global plutôt que d'en tenir un second : les deux chats
// doivent envoyer au même endroit.
int ReadSendMode() {
  __try {
    return *reinterpret_cast<int*>(kInputTargetMode);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

void WriteSendMode(int mode) {
  __try {
    *reinterpret_cast<int*>(kInputTargetMode) = mode;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// ── Petits outils de texte ───────────────────────────────────────────────────
bool IsHex6(const char* s) {
  for (int i = 0; i < 6; ++i)
    if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
  return true;
}

// Recherche d'une sous-chaîne dans un intervalle NON terminé par un zéro : le
// texte d'une ligne est parcouru par pointeurs, et `strstr` lirait au-delà.
const char* SearchSub(const char* begin, const char* end, const char* needle) {
  const size_t n = std::strlen(needle);
  if (static_cast<size_t>(end - begin) < n) return nullptr;
  for (const char* p = begin; p + n <= end; ++p)
    if (std::memcmp(p, needle, n) == 0) return p;
  return nullptr;
}

// Badge de rang d'un monstre. Mêmes valeurs que la table des drops de la fenêtre
// de description (`boss` : 2 = MVP, 1 = mini-boss), pour qu'un même monstre ne
// change pas d'étiquette selon l'endroit d'où le lien a été posé.
const char* MobRankTag(uint8_t rank) {
  if (rank == 2) return "[MVP]";
  if (rank == 1) return "[Boss]";
  return "[Mob]";
}

// Début d'adresse web reconnu. Volontairement limité à ces trois amorces : tout
// ce qui ressemble de loin à un domaine (« truc.fr ») transformerait en lien la
// moitié des phrases, y compris les fins de phrase (« ...voilà.Et »).
bool IsUrlStart(const char* p, const char* end) {
  const size_t left = static_cast<size_t>(end - p);
  return (left >= 7 && _strnicmp(p, "http://", 7) == 0) ||
         (left >= 8 && _strnicmp(p, "https://", 8) == 0) ||
         (left >= 4 && _strnicmp(p, "www.", 4) == 0);
}

// Fin de l'adresse. On s'arrête à l'espace, et on REND la ponctuation finale au
// texte : « regarde https://moonlight-destiny.fr. » ne doit pas ouvrir une URL
// terminée par un point. La parenthèse fermante n'est rendue que si l'adresse
// n'en contient pas d'ouvrante (les URL de wiki en ont).
const char* UrlEnd(const char* p, const char* end) {
  const char* stop = p;
  while (stop < end && static_cast<unsigned char>(*stop) > ' ' && *stop != '<' &&
         *stop != '"')
    ++stop;
  while (stop > p) {
    const char c = stop[-1];
    if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?') {
      --stop;
      continue;
    }
    if (c == ')' && std::memchr(p, '(', stop - p) == nullptr) {
      --stop;
      continue;
    }
    break;
  }
  return stop;
}

// Recherche insensible à la casse (ASCII) — le filtre de la barre de recherche.
bool ContainsNoCase(const std::string& haystack, const char* needle) {
  if (needle == nullptr || needle[0] == '\0') return true;
  const size_t n = std::strlen(needle);
  if (haystack.size() < n) return false;
  for (size_t i = 0; i + n <= haystack.size(); ++i) {
    size_t j = 0;
    while (j < n && std::tolower(static_cast<unsigned char>(haystack[i + j])) ==
                        std::tolower(static_cast<unsigned char>(needle[j])))
      ++j;
    if (j == n) return true;
  }
  return false;
}

// (Le base62 des liens d'items vit désormais dans `itemcell::ParseChatLink`, avec
//  l'encodeur qui lui répond : une balise se lit là où elle s'écrit.)

inline ImTextureID TexId(void* t) { return reinterpret_cast<ImTextureID>(t); }

// ── Les DEUX couleurs du chat n'ont pas le même ordre d'octets ───────────────
// La couleur d'une LIGNE est un COLORREF Windows — `0x00BBGGRR`, rouge en octet
// de poids faible — parce que c'est ce que le client passe à GDI. Le rouge de
// « Bienvenue sur Moonlight-Destiny » arrive donc en 0x0000FF, et le lire comme
// du RGB le rendait BLEU à l'écran.
inline ImU32 LineColorToImU32(uint32_t colorref) {
  return IM_COL32(colorref & 0xFF, (colorref >> 8) & 0xFF, (colorref >> 16) & 0xFF,
                  0xFF);
}

// Les codes `^RRGGBB` écrits DANS le texte, eux, sont en RGB : ce sont des
// chiffres hexadécimaux lus de gauche à droite, pas un mot machine.
inline ImU32 HexRgbToImU32(uint32_t rgb) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
}

inline ImU32 Lighten(ImU32 col, int amount) {
  const int r = std::min(255, static_cast<int>((col >> IM_COL32_R_SHIFT) & 0xFF) + amount);
  const int g = std::min(255, static_cast<int>((col >> IM_COL32_G_SHIFT) & 0xFF) + amount);
  const int b = std::min(255, static_cast<int>((col >> IM_COL32_B_SHIFT) & 0xFF) + amount);
  return IM_COL32(r, g, b, (col >> IM_COL32_A_SHIFT) & 0xFF);
}

// Couleur de ligne du chat natif quand l'appelant n'en donne pas.
constexpr uint32_t kDefaultRgb = 0xFFFFFF;
constexpr ImU32 kStampCol   = IM_COL32(150, 150, 150, 255);
constexpr ImU32 kDiagCol    = IM_COL32(0xFF, 0xB0, 0x40, 255);  // marqueur de type
constexpr ImU32 kLinkCol    = IM_COL32(0xF4, 0x93, 0x4A, 0xFF);  // liens du client
constexpr ImU32 kTabTextCol = IM_COL32(0x14, 0x14, 0x14, 255);   // texte des onglets
constexpr ImU32 kDarkText   = IM_COL32(0x14, 0x14, 0x14, 255);   // sur champ clair

// Redimensionnement par RANGÉES, comme le chat natif : il n'affiche jamais une
// ligne coupée en deux, parce que sa hauteur ne prend que des valeurs « chrome +
// N lignes ». On reproduit ça avec une contrainte de taille ImGui.
struct RowSnap {
  float line_h   = 0.0f;  // hauteur d'une ligne de log
  float chrome_h = 0.0f;  // tout ce qui n'est pas la zone de log
  float min_h    = 0.0f;  // bornes, re-appliquées APRÈS l'arrondi
  float max_h    = 0.0f;
};
RowSnap g_row_snap;

void SnapHeightToRows(ImGuiSizeCallbackData* data) {
  const RowSnap* snap = static_cast<const RowSnap*>(data->UserData);
  if (snap == nullptr || snap->line_h <= 1.0f) return;
  float rows = (data->DesiredSize.y - snap->chrome_h) / snap->line_h;
  rows = std::floor(rows + 0.5f);
  if (rows < 1.0f) rows = 1.0f;
  // 🔴 Ce rappel s'exécute APRÈS le bornage min/max d'ImGui : arrondir ici peut
  // repasser sous le minimum ou au-dessus du maximum. On re-borne donc à la
  // RANGÉE — en montant pour le plancher, en descendant pour le plafond — sinon
  // les deux règles se contredisent silencieusement.
  if (snap->min_h > 0.0f)
    while (snap->chrome_h + rows * snap->line_h < snap->min_h) rows += 1.0f;
  if (snap->max_h > 0.0f)
    while (rows > 1.0f && snap->chrome_h + rows * snap->line_h > snap->max_h)
      rows -= 1.0f;
  data->DesiredSize.y = snap->chrome_h + rows * snap->line_h;
}

}  // namespace

// Ingestion depuis le WndProc natif (case 0x25). Mêmes protections que le détour
// de ChatAction : les chaînes du client sont recopiées sous SEH avant tout.
// ⚠ L'ordre des arguments n'est PAS celui de ChatAction : ici c'est
// (texte, couleur, TYPE, sender) — le type et le sender sont intervertis.
void chatwnd::IngestNativeLine(const char* text, uint32_t rgb, int type,
                               const char* sender) {
  if (g_chat_window == nullptr) return;
  RawChatLine raw;
  if (!SafeCopyChatStrings(text, sender, &raw)) return;
  ++g_chat_window->ingest_seen_;
  g_chat_window->Ingest(raw.text, rgb, raw.sender, type, 'W');
}

// ── L'angle mort : nos PROPRES lignes ────────────────────────────────────────
//
// `UIM_PUSHINTOCHATHISTORY` est la voie par laquelle Bourgeon écrit dans le chat
// (relais Discord, DPS meter — les deux seuls). Mesuré en jeu : elle NE PASSE PAS
// par `ChatAction`, elle atteint la chatbox native directement. Tant que celle-ci
// vivait, son WndProc nous relayait la ligne (source 'W') et personne n'avait
// remarqué la différence ; une fois la native détruite, la ligne tombait dans la
// file `mgr+0x4C4`, qui n'est drainée qu'à la CRÉATION d'une fenêtre — donc plus
// jamais. Nos deux plugins parlaient à une fenêtre morte, en silence.
//
// Renvoie true quand la chatbox ImGui a pris la ligne : l'appelant ne doit alors
// PAS la passer au natif, sous peine de faire grossir cette file sans plafond.
bool chatwnd::IngestPluginLine(const char* text, uint32_t rgb) {
  if (g_chat_window == nullptr || !g_chat_window->imgui_enabled_) return false;
  // 🔴 La native vit encore : c'est son WndProc qui nous alimentera, et ingérer
  // ici doublerait la ligne. Même règle que le détour de ChatAction.
  if (NativeChatAlive()) return false;
  RawChatLine raw;
  if (!SafeCopyChatStrings(text, nullptr, &raw)) return false;
  ++g_chat_window->ingest_seen_;
  // Type 0 : c'est celui sous lequel le natif les affichait (relevé en jeu,
  // « t00W » sur les lignes du DPS meter et du relais). Source 'P' pour les
  // distinguer en mode diagnostic — ni le serveur ('A'/'W'), ni le natif : nous.
  g_chat_window->Ingest(raw.text, rgb, raw.sender, 0, 'P');
  return true;
}

// ── Libellés des 25 types (msgstringtable du client, §3.1.1 de la doc) ───────
const char* chatwnd::TypeLabel(int type) {
  static const char* const kLabels[kTypeCount] = {
      "Public",                            // 0  — système / défaut
      "Public Chat",                       // 1
      "Whisper",                           // 2
      "Party Chat",                        // 3
      "Guild Chat",                        // 4
      "Alliance Chat",                     // 5
      "Item get/drop",                     // 6
      "Equipment on/off",                  // 7
      "Abnormal status",                   // 8
      "Party member's obtained item",      // 9
      "Party member's abnormal status",    // 10
      "Skill failure",                     // 11
      "Party configuration",               // 12
      "Damaged equipment",                 // 13
      "WOE information",                   // 14
      "Search message for party members",  // 15
      "Battle message",                    // 16
      "Party member's battle message",     // 17
      "Experience message",                // 18
      "Quest information",                 // 19
      "Battlefield message",               // 20
      "Clan Chat",                         // 21
      "Call messages",                     // 22
      "Repayment-exp",                     // 23
      "Equip attribute changes",           // 24
  };
  if (type == kTypeBroadcast) return "Broadcast";
  if (type < 0 || type >= kTypeCount) return "?";
  return kLabels[type];
}

ChatWindow::ChatWindow() {
  g_chat_window = this;
  // Le détour porte DEUX besoins (filtre système + ingestion) parce qu'il n'y a
  // qu'un seul jeu d'octets à détourner à cette adresse — cf. l'en-tête.
  g_tramp_chat_action = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kChatActionAddr),
      reinterpret_cast<uint8_t*>(&ChatActionStub));
  if (g_tramp_chat_action == nullptr)
    LogError("[chat] detour ChatAction 0x{:08x} NON pose", kChatActionAddr);

  // Garde-fou permanent, posé même si la chatbox ImGui est éteinte : il ne coûte
  // qu'un `test ecx, ecx` quand la native est vivante, et il protège quinze
  // appelants dont on ne maîtrise pas le déclenchement.
  g_tramp_chat_tags = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kChatTagTransform),
      reinterpret_cast<uint8_t*>(&ChatTagTransformStub));
  if (g_tramp_chat_tags == nullptr)
    LogError("[chat] detour TransformTagLinks 0x{:08x} NON pose — le client "
             "plantera au premier texte balisé sans chatbox native",
             kChatTagTransform);

  // Conversations 1:1. Le détour se pose toujours mais ne mord que si la chatbox
  // ImGui est active (cf. WhisperFilter) : en mode natif, ce sont les popups du
  // client qui doivent continuer de s'ouvrir.
  g_tramp_whisper = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kWhisperPivotAddr),
      reinterpret_cast<uint8_t*>(&WhisperPivotStub));
  if (g_tramp_whisper == nullptr)
    LogError("[chat] detour OnWhisperReceived 0x{:08x} NON pose — les popups "
             "1:1 NATIVES reviendront par-dessus l'interface moderne",
             kWhisperPivotAddr);

  g_tramp_chat_togglebar = hooking::HookManager::Instance().SetHook(
      hooking::HookType::kJmpHook, reinterpret_cast<uint8_t*>(kChatToggleInputBar),
      reinterpret_cast<uint8_t*>(&ChatToggleInputBarStub));
  if (g_tramp_chat_togglebar == nullptr)
    LogError("[chat] detour ToggleInputBar 0x{:08x} NON pose — le client "
             "plantera au premier raccourci ou clic sans chatbox native",
             kChatToggleInputBar);

  // Canaux du serveur (ZC 0x0F21), poussés au login. Écoutés MÊME en mode natif :
  // le handler ne fait que remplir un tableau, et le jour où le joueur bascule en
  // ImGui la liste est déjà là — la même règle que la liste des storages.
  Bourgeon::Instance().RegisterRecvOpcode(bopcodes::kChannelList);
}

// Fil RÉSEAU : copier, rien d'autre (features/net_inbox.h). Le décodage repart
// sur le fil principal dans HandlePacket.
void ChatWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

// ZC_BOURGEON_CHANNEL_LIST : [count:1] puis count fois
// [flags:1][color:4][name:20][alias:20]. `data` commence APRÈS [op:2][len:2].
// REMPLACE la liste : les droits d'un joueur peuvent changer en cours de session
// (@adjgroup), et une entrée survivant à sa permission proposerait un canal que
// le serveur refuserait ensuite en silence.
void ChatWindow::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  if (opcode != bopcodes::kChannelList || data == nullptr || len < 1) return;
  constexpr size_t kNameLen  = 20;  // CHAN_NAME_LENGTH côté serveur
  constexpr size_t kEntryLen = 5 + 2 * kNameLen;
  const int count = data[0];
  server_channels_.clear();
  size_t off = 1;
  for (int i = 0; i < count && off + kEntryLen <= len; ++i, off += kEntryLen) {
    const uint8_t  flags = data[off];
    const uint32_t bgr   = *reinterpret_cast<const uint32_t*>(data + off + 1);
    auto bounded = [](const uint8_t* p, size_t cap) {
      size_t n = 0;
      while (n < cap && p[n] != '\0') ++n;
      return std::string(reinterpret_cast<const char*>(p), n);
    };
    const std::string name = bounded(data + off + 5, kNameLen);
    if (name.empty()) continue;  // une entrée sans nom est inatteignable
    ServerChannel channel;
    // Le serveur range le nom SANS son '#' (`struct Channel`, et
    // `channel_name2channel` compare toujours sur `chname + 1`) : c'est ici qu'on
    // le remet, une fois pour toutes — c'est la chaîne qui ira dans la box
    // destinataire.
    channel.name  = "#" + name;
    channel.alias = ro::WireToUtf8(bounded(data + off + 5 + kNameLen, kNameLen).c_str());
    // 🔴 Le serveur STOCKE ses couleurs en BGR (channel.cpp, « RGB to BGR » à la
    // lecture de channels.conf) : c'est cette valeur-là qui voyage, et la remettre
    // à l'endroit est notre travail — la lire comme du RGB donnerait un rouge
    // partout où la conf dit bleu.
    const uint32_t r = bgr & 0xFF, g = (bgr >> 8) & 0xFF, b = (bgr >> 16) & 0xFF;
    channel.color = IM_COL32(r, g, b, 255);
    channel.require_guild = (flags & 0x01) != 0;
    channel.can_chat      = (flags & 0x02) != 0;
    server_channels_.push_back(std::move(channel));
  }
}

bool ChatWindow::IsOwnName(const char* utf8) const {
  if (utf8 == nullptr || utf8[0] == '\0') return false;
  // Lecture sous SEH dans un tampon POD, comparaison ensuite : `__try` et les
  // objets à destructeur ne cohabitent pas (C2712).
  char own[32] = {};
  if (!ReadOwnCharName(own, sizeof(own))) return false;
  return _stricmp(ro::WireToUtf8(own), utf8) == 0;
}

bool ChatWindow::InParty() const {
  __try {
    return reinterpret_cast<PartyCount_t>(kPartyMemberCount)(
               reinterpret_cast<void*>(kUIWindowContextKey)) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool ChatWindow::InGuild() const {
  __try {
    return *reinterpret_cast<uint32_t*>(kOwnGuildId) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Dernière chance d'écrire : une fermeture par la croix ne passe pas forcément
// par un changement de mode. Le détour, lui, n'est PAS retiré — il reste posé pour
// la durée du processus ; on se contente de couper le pointeur qu'il consulte.
ChatWindow::~ChatWindow() {
  if (layout_dirty_) SaveLayout();
  SaveHistory();
  if (g_chat_window == this) g_chat_window = nullptr;
}

void ChatWindow::OnModeSwitch(ModeMgr::ModeType mode_type, const char* map_name) {
  // Changement de personnage / retour au login : l'historique appartenait à la
  // session précédente, et les canaux seront relus au prochain rendu.
  if (mode_type != ModeMgr::ModeType::kGame) {
    // 🔴 Enregistrer AVANT de vider : c'est le dernier instant où la disposition
    // existe encore. C'est aussi le moment que choisit le client pour écrire son
    // propre `ChatWndInfo_U.lua` — la déconnexion, pas chaque modification : une
    // écriture par ligne de chat serait exactement le genre de coût qui a déjà
    // gelé cette fenêtre.
    if (layout_dirty_) {
      SaveLayout();
      layout_dirty_ = false;
    }
    SaveHistory();  // avant ClearHistory, évidemment : après, il n'y a plus rien
    ClearHistory();
    channels_.clear();
    channels_stamp_  = 0;
    structure_owned_ = false;  // on repartira du fichier, sinon du registre
    has_pending_ = false;
    ingest_seen_ = 0;
    ingest_kept_ = 0;
  } else {
    // Entrée en jeu : la disposition du JOUEUR d'abord. Si le fichier n'existe pas
    // encore, `RefreshChannels` amorcera depuis le registre du client.
    LoadLayout();
    LoadHistory();
  }
}

uint64_t ChatWindow::LastLineSeq() const {
  std::lock_guard<std::mutex> lock(lines_mutex_);
  // La dernière du tampon porte le rang le plus élevé : les rangs suivent l'ordre
  // d'insertion, et `LoadHistory` — le seul à insérer en tête — renumérote tout
  // derrière lui (cf. TrimLines).
  return lines_.empty() ? 0 : lines_.back().seq;
}

void ChatWindow::ClearHistory() {
  std::lock_guard<std::mutex> lock(lines_mutex_);
  lines_.clear();
  // 🔴 Les compteurs d'éviction partent avec : ils décrivent le contenu du
  // tampon, et les laisser pleins ferait évincer les premières lignes reçues
  // ensuite — le log se remettrait à se vider tout seul, sans rien pour
  // l'expliquer (cf. TrimLines).
  std::memset(type_count_, 0, sizeof(type_count_));
  counted_lines_ = 0;
}

// L'AID OBFUSQUÉ que le client colle dans le préfixe d'une ligne de chuchotement.
// Il apparaît sous DEUX habillages selon le sens, et il a fallu les deux :
//   envoi crochets  « [ To Nom (813-524) ] : … »   `Whisper_DispatchSendResult`
//   envoi parenth.  « ( To Nom (813-524) : … »     idem, branche hors guilde
//   reçu            « ( From Nom [813-524] ) : … » `sub_CAFD00`
// Ce nombre ne dit rien à personne : ce n'est même pas l'identifiant réel, qui
// est chiffré par substitution (cf. Aid_FormatObfuscated).
//
// 🔴 DEUX PIÈGES, tous deux payés en jeu :
//
// 1. L'AID est tantôt entre PARENTHÈSES, tantôt entre CROCHETS. Ne chercher que
//    les parenthèses laissait le code visible sur tout ce qu'on RECEVAIT.
//
// 2. La forme « ( To Nom (aid) : » du client est DÉJÀ déséquilibrée — deux
//    ouvrantes, une seule fermante, celle de l'AID. La retirer emportait donc la
//    seule parenthèse fermante de la ligne, et l'on se retrouvait avec
//    « ( To Nom : … ». On rééquilibre le préfixe après coup plutôt que de
//    reproduire le déséquilibre du client.
//
// On ne touche qu'au PRÉFIXE, borné au premier « : ». Ce qui suit est le MESSAGE,
// et l'amputer de ce qui ressemble à un groupe de chiffres reviendrait à réécrire
// ce qu'un joueur a tapé — « rendez-vous à Prontera (11-42) » y perdrait ses
// coordonnées.
namespace {
std::string StripWhisperAidTag(const char* wire_text) {
  std::string out = (wire_text != nullptr) ? wire_text : "";
  const size_t body  = out.find(" : ");
  const size_t limit = (body == std::string::npos) ? out.size() : body;
  std::string prefix = out.substr(0, limit);

  // Retire du préfixe tout groupe « (…) » ou « […] » qui a la SIGNATURE d'un AID
  // obfusqué. Celle-ci est étroite, et volontairement : `Aid_FormatObfuscated`
  // insère son tiret quand le diviseur vaut 100, donc il y a TOUJOURS exactement
  // trois chiffres après — et au moins un avant. « Prontera (11-42) » n'en a que
  // deux et sort donc indemne, alors qu'un simple « chiffres et tirets » l'aurait
  // mutilé.
  bool removed = false;
  for (size_t i = 0; i + 1 < prefix.size();) {
    const char opener = prefix[i];
    if (opener != '(' && opener != '[') { ++i; continue; }
    const char closer = (opener == '(') ? ')' : ']';
    const size_t close = prefix.find(closer, i + 1);
    if (close == std::string::npos) { ++i; continue; }
    const size_t dash = prefix.find('-', i + 1);
    bool looks_like_aid = dash != std::string::npos && dash < close &&
                          dash > i + 1 && close == dash + 4;
    for (size_t k = i + 1; k < close && looks_like_aid; ++k)
      if (k != dash && (prefix[k] < '0' || prefix[k] > '9')) looks_like_aid = false;
    if (!looks_like_aid) { ++i; continue; }
    // L'espace qui précède part avec le groupe, sinon « Nom  ] » garde un trou.
    const size_t from = (i > 0 && prefix[i - 1] == ' ') ? i - 1 : i;
    prefix.erase(from, close - from + 1);
    removed = true;
    i = from;
  }
  // 🔴 Rien retiré ⇒ rien touché. C'est ce qui rend l'appel sûr sur N'IMPORTE
  // quelle ligne : sans cette sortie, le rééquilibrage ci-dessous irait
  // « corriger » les parenthèses d'un message que personne ne lui a demandé de
  // relire.
  if (!removed) return out;

  // Rééquilibrage : le client ouvre parfois sans fermer — « ( To Nom (aid) : »
  // n'a qu'une fermante pour deux ouvrantes, et c'est celle de l'AID qu'on vient
  // d'emporter. On n'ajoute jamais d'ouvrante, seulement les fermantes qui
  // manquent, et à la fin du préfixe, là où le client les aurait mises.
  auto balance = [&prefix](char opener, char closer) {
    int depth = 0;
    for (const char c : prefix) {
      if (c == opener) ++depth;
      else if (c == closer && depth > 0) --depth;
    }
    while (depth-- > 0) {
      while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
      prefix += ' ';
      prefix += closer;
    }
  };
  balance('(', ')');
  balance('[', ']');

  return prefix + out.substr(limit);
}

// Le PSEUDO caché dans le libellé de tête d'une ligne, et le SENS de l'échange.
//
// Le client compose plusieurs formes selon le chemin qu'a pris la ligne, et le
// pseudo n'y est pas toujours en tête — c'est ce qui le rendait à la fois
// incliquable et absent des destinataires récents :
//   « Nom »                    la fenêtre 1:1 (le pseudo nu)
//   « ( From Nom [… »          reçu, écrit dans la chatbox  (0x010922ec)
//   « ( To Nom ) » / « [ To Nom ] »   l'écho de notre envoi (0x01091a64 / 0x01091a54)
//
// Rend une chaîne vide quand il n'y a rien de sûr à en tirer : mieux vaut pas de
// lien du tout qu'un menu qui propose d'inviter « [ To Nom ] » dans sa guilde.
std::string WhisperPeerFromLabel(const std::string& label, bool* outgoing) {
  *outgoing = false;
  size_t begin = 0, end = label.size();
  // `delimited` : le libellé était-il entre parenthèses ou crochets ? C'est ce
  // qui distingue une forme COMPOSÉE par le client d'un pseudo nu — et depuis que
  // les espaces sont admis dans les noms, la distinction compte : sans elle, un
  // joueur nommé « To ta » se ferait amputer de son « To » par le retrait de
  // préfixe ci-dessous.
  bool delimited = false;
  while (begin < end && (label[begin] == ' ' || label[begin] == '(' ||
                         label[begin] == '[')) {
    if (label[begin] != ' ') delimited = true;
    ++begin;
  }
  while (end > begin && (label[end - 1] == ' ' || label[end - 1] == ')' ||
                         label[end - 1] == ']')) --end;
  if (begin >= end) return std::string();

  std::string inner = label.substr(begin, end - begin);
  auto strip = [&inner](const char* prefix, bool value, bool* out) {
    const size_t n = std::strlen(prefix);
    if (inner.size() <= n || inner.compare(0, n, prefix) != 0) return false;
    inner.erase(0, n);
    *out = value;
    return true;
  };
  // Les quatre en-têtes du client, relevés côte à côte en mémoire :
  //   « [ Friend <nom> … ] : »  0x010922d4  l'expéditeur est dans vos amis
  //   « [ Member <nom> … ] : »  0x010922e0  … dans votre guilde
  //   « ( From <nom> … ) : »    0x010922ec  ni l'un ni l'autre
  //   « ( To <nom> … ) : »      0x01091a64  l'écho de votre envoi
  // C'est pourquoi deux joueurs voient la MÊME conversation sous deux formes
  // différentes : la relation n'est pas symétrique.
  // ⚠ Uniquement sur un libellé DÉLIMITÉ : le client n'écrit jamais ces mots
  // devant un pseudo nu, alors qu'un pseudo, lui, peut commencer par n'importe
  // quoi — espaces compris.
  if (delimited && !strip("From ", false, outgoing) &&
      !strip("To ", true, outgoing) && !strip("Friend ", false, outgoing))
    strip("Member ", false, outgoing);

  // Ce qui suit le pseudo (« [ Member … », « ) : ») n'en fait pas partie.
  const size_t cut = inner.find_first_of("[]()");
  if (cut != std::string::npos) inner.erase(cut);
  while (!inner.empty() && inner.back() == ' ') inner.pop_back();

  // 🔴 UN PSEUDO PEUT CONTENIR DES ESPACES. On refusait tout libellé qui en
  // portait, en croyant qu'un nom de personnage n'en a jamais — c'est faux, et
  // c'est une propriété du SERVEUR, pas du jeu : `char_name_option: 1` avec un
  // espace dans `char_name_letters` (conf/char_athena.conf, où le commentaire
  // insiste même sur le fait que l'espace y est délibéré). Des joueurs comme
  // « .S T N. » n'étaient donc jamais cliquables.
  //
  // Il reste une borne, celle du serveur : NAME_LENGTH. Au-delà, ce n'est plus
  // un pseudo mais un libellé qu'on n'a pas su découper, et mieux vaut ne rien
  // proposer que d'ouvrir un menu sur une phrase.
  if (inner.empty() || inner.size() >= kNameFieldLen) return std::string();
  return inner;
}
// Un chuchotement du STAFF : le client le trahit par son TYPE.
//
// Le serveur lève `isAdmin` dans ZC_WHISPER (0x0097) quand l'expéditeur est du
// staff — chez nous à partir du group level 80 (moonlight, clif_wis_message). Le
// client, lui, ne l'affiche nulle part : il se contente d'écrire la ligne en type
// **0x19 (broadcast)** au lieu de 2 (`sub_CAFD00`, 0x00cb0ade).
//
// On récupère donc les deux choses qu'il perd :
//   * le TYPE — une conversation privée reste privée, et doit obéir au filtre
//     « Whisper » des onglets, pas passer partout comme une annonce ;
//   * l'INFORMATION — devenue le marqueur « (GM) », posé après le pseudo.
//
// 🔴 La reconnaissance ne se fait PAS sur le seul type : un vrai broadcast serait
// alors pris pour un chuchotement. Il faut un en-tête de chuchotement reconnu
// (`WhisperPeerFromLabel`), ce qu'aucune annonce serveur ne porte.
//
// Rend true si la ligne est un chuchotement de GM.
bool TagGmWhisper(std::string* text, uint8_t* type) {
  if (*type != kTypeBroadcast || text->empty()) return false;
  const size_t body  = text->find(" : ");
  const size_t limit = (body == std::string::npos) ? text->size() : body;
  bool outgoing = false;
  const std::string peer = WhisperPeerFromLabel(text->substr(0, limit), &outgoing);
  if (peer.empty()) return false;  // une vraie annonce : on n'y touche pas

  *type = static_cast<uint8_t>(kTypeWhisper);
  const size_t at = text->find(peer);
  if (at != std::string::npos && at < limit)
    text->insert(at + peer.size(), " (GM)");
  return true;
}
}  // namespace

// Un chuchotement REÇU entre dans les destinataires récents. C'est le geste
// qu'on veut ensuite neuf fois sur dix : répondre. Le natif ne remplissait cette
// liste qu'à l'ENVOI, et nous non plus — d'où un correspondant qui vient
// d'écrire mais reste introuvable dans le menu de la box destinataire.
//
// Rien à faire pour l'écho de nos propres envois : `QueueSend` l'a déjà noté au
// moment où le joueur a validé.
void ChatWindow::RememberWhisperPeer(const Line& line) {
  if (line.type != kTypeWhisper) return;
  bool outgoing = false;
  const std::string peer = WhisperPeerFromLabel(line.sender, &outgoing);
  if (peer.empty() || outgoing) return;
  if (IsOwnName(peer.c_str())) return;  // notre propre écho relu à l'envers
  PushWhisperHistory(peer.c_str());
}

// Rend cliquable le pseudo de tête, où qu'il soit dans le libellé.
void ChatWindow::MarkSenderAsPlayerLink(Line* line) const {
  bool outgoing = false;
  const std::string name = WhisperPeerFromLabel(line->sender, &outgoing);
  if (name.empty()) return;
  for (size_t i = 0; i < line->runs.size(); ++i) {
    Run& run = line->runs[i];
    if (run.text.empty()) continue;
    if (run.kind != Run::kNone) return;  // déjà un lien : on n'y touche pas
    // Borné au PRÉFIXE : un pseudo qui se trouve aussi dans le corps du message
    // (« dis à Filip que… ») ne doit pas devenir un second lien — celui-là, on ne
    // sait pas s'il désigne un joueur ou un mot.
    const size_t body  = run.text.find(" : ");
    const size_t limit = (body == std::string::npos) ? run.text.size() : body;
    const size_t at    = run.text.find(name);
    if (at == std::string::npos || at + name.size() > limit) return;

    const std::string before = run.text.substr(0, at);
    const std::string after  = run.text.substr(at + name.size());
    Run middle = run;  // hérite couleur, gras, italique
    middle.text = name;
    middle.kind = Run::kPlayer;

    // Reconstruit sur place : au plus trois fragments, dont seuls les non vides
    // sont conservés — un fragment vide se traînerait dans tout le rendu.
    std::vector<Run> parts;
    if (!before.empty()) {
      Run head = run;
      head.text = before;
      parts.push_back(std::move(head));
    }
    parts.push_back(std::move(middle));
    if (!after.empty()) {
      Run tail = run;
      tail.text = after;
      parts.push_back(std::move(tail));
    }
    line->runs.erase(line->runs.begin() + static_cast<ptrdiff_t>(i));
    line->runs.insert(line->runs.begin() + static_cast<ptrdiff_t>(i), parts.begin(),
                      parts.end());
    return;
  }
}

// ── Ingestion ────────────────────────────────────────────────────────────────
void ChatWindow::Ingest(const char* text, uint32_t rgb, const char* sender,
                        int type, char source) {
  Line line;
  line.source = source;
  line.rgb  = (rgb != 0) ? (rgb & 0xFFFFFF) : kDefaultRgb;
  line.type = static_cast<uint8_t>(
      (type == kTypeBroadcast || (type >= 0 && type < kTypeCount)) ? type : 0);

  SYSTEMTIME now;
  GetLocalTime(&now);
  line.hour   = static_cast<uint8_t>(now.wHour);
  line.minute = static_cast<uint8_t>(now.wMinute);
  line.second = static_cast<uint8_t>(now.wSecond);

  // L'AID obfusqué du correspondant : ôté AVANT l'extraction du sender, qui
  // sinon le garderait dans son libellé.
  //
  // 🔴 PAS conditionné au type 2. Le client écrit un chuchotement en type 0x19
  // (broadcast) au lieu de 2 quand l'expéditeur est un GM (`sub_CAFD00`, à
  // 0x00cb0ade) — c'est ce qui laissait le code visible sur les lignes venant du
  // staff, et seulement sur celles-là. La fonction ne touche de toute façon
  // qu'aux lignes où elle reconnaît la signature exacte d'un AID obfusqué, et
  // rend les autres intactes.
  std::string cleaned = StripWhisperAidTag(text);
  // Chuchotement du staff : le client l'a écrit en broadcast et n'en dit rien.
  // On lui rend son type et on pose le marqueur — AVANT l'extraction du sender,
  // qui doit voir le libellé définitif.
  TagGmWhisper(&cleaned, &line.type);
  text = cleaned.c_str();

  // Sender : donné par l'appelant, sinon extrait du texte comme le natif le fait
  // pour les formats « Nom : msg » (types public/groupe/guilde/clan) — il cherche
  // le mot 16 bits ` :` et borne le nom à 24 caractères.
  const char* raw_sender = sender;
  char extracted[32] = {};
  if ((raw_sender == nullptr || raw_sender[0] == '\0') &&
      (line.type == 1 || line.type == 3 || line.type == 4 || line.type == 0x15)) {
    const char* separator = std::strstr(text, " :");
    if (separator != nullptr && (separator - text) <= 24) {
      CopyBounded(extracted, sizeof(extracted), text);
      extracted[separator - text] = '\0';
      raw_sender = extracted;
    }
  }
  if (raw_sender != nullptr && raw_sender[0] != '\0')
    line.sender = ro::WireToUtf8(raw_sender);

  ParseText(text, &line);
  MarkSenderAsPlayerLink(&line);
  RememberWhisperPeer(line);

  std::lock_guard<std::mutex> lock(lines_mutex_);
  ++ingest_kept_;
  lines_.push_back(std::move(line));
  TrimLines();
}

// ── Chuchotement 1:1 ─────────────────────────────────────────────────────────
// Nombre de conversations ouvertes simultanément. Ce n'est pas une contrainte du
// client (sa map n'a pas de plafond) mais la nôtre : chaque conversation est une
// fenêtre à l'écran, et au-delà d'une poignée elles se recouvrent en s'empilant
// sans que le joueur puisse rien y faire. Le plus ANCIEN sans ligne récente cède
// sa place — jamais le plus actif.
constexpr size_t kMaxWhisperWindows = 8;

int ChatWindow::FindWhisperChannel(const std::string& with_utf8) const {
  if (with_utf8.empty()) return -1;
  for (size_t i = 0; i < channels_.size(); ++i)
    if (channels_[i].whisper_with == with_utf8) return static_cast<int>(i);
  return -1;
}

int ChatWindow::FindOrCreateWhisper(const std::string& with_utf8, uint32_t aid) {
  if (with_utf8.empty()) return -1;
  for (size_t i = 0; i < channels_.size(); ++i) {
    if (channels_[i].whisper_with == with_utf8) {
      // L'AID n'accompagne pas toujours la ligne (l'écho de nos envois l'a, le
      // reçu aussi, mais on ne parie pas dessus) : on ne l'écrase jamais par zéro.
      if (aid != 0) channels_[i].whisper_aid = aid;
      channels_[i].whisper_stamp = GetTickCount();
      return static_cast<int>(i);
    }
  }

  size_t open = 0;
  for (const Channel& channel : channels_)
    if (!channel.whisper_with.empty()) ++open;
  if (open >= kMaxWhisperWindows) {
    // Plein : on ferme la conversation la plus ancienne au sens de sa dernière
    // ligne. Faute de quoi la nouvelle n'apparaîtrait nulle part, et le joueur
    // croirait avoir raté le message.
    int      oldest       = -1;
    uint32_t oldest_stamp = 0;
    for (size_t i = 0; i < channels_.size(); ++i) {
      if (channels_[i].whisper_with.empty()) continue;
      if (oldest < 0 || channels_[i].whisper_stamp < oldest_stamp) {
        oldest       = static_cast<int>(i);
        oldest_stamp = channels_[i].whisper_stamp;
      }
    }
    if (oldest < 0) return -1;
    channels_.erase(channels_.begin() + oldest);
    if (active_channel_ > oldest) --active_channel_;
  }

  Channel channel;
  channel.id           = next_channel_id_++;
  channel.name         = with_utf8;
  channel.whisper_with = with_utf8;
  channel.whisper_aid  = aid;
  // 🔴 `detached` ET `detach_owned` : la fusion périodique avec le registre natif
  // remettrait sinon ce canal « docké » — il n'a pas d'entrée là-bas, donc rien
  // ne le contredirait jamais, et il finirait comme un onglet fantôme.
  // Une conversation naît dans SA fenêtre à elle ; on l'y groupera ensuite si le
  // joueur le veut.
  SetChannelGroup(channel, NewGroupId());
  channel.detach_owned  = true;
  channel.whisper_stamp = GetTickCount();
  // Une conversation privée accepte tout ce qu'on lui range explicitement : c'est
  // `whisper_with` qui filtre, pas la table de types (cf. ChannelAccepts).
  std::memset(channel.filter, 1, sizeof(channel.filter));
  channels_.push_back(std::move(channel));
  return static_cast<int>(channels_.size() - 1);
}

bool ChatWindow::OpenWhisperWindow(const char* name_wire, const char* aid_display) {
  return OpenWhisperWindowByAid(name_wire, DeobfuscateAid(aid_display));
}

bool ChatWindow::OpenWhisperWindowByAid(const char* name_wire, uint32_t aid) {
  if (name_wire == nullptr || name_wire[0] == '\0') return false;
  // 🔴 Interface moderne éteinte : `OnRenderUI` sort avant tout dessin, et le
  // canal créé ici ne serait JAMAIS peint. Une fenêtre invisible est pire qu'un
  // refus — l'appelant doit pouvoir le dire au joueur.
  if (!imgui_enabled_) return false;
  const int index = FindOrCreateWhisper(ro::WireToUtf8(name_wire), aid);
  if (index < 0) return false;
  // Le clavier va à la saisie : ici le joueur a CLIQUÉ « chuchoter », il veut
  // écrire. C'est la différence avec une conversation qui s'ouvre parce qu'on
  // vient de recevoir un message — celle-là ne doit rien voler.
  channels_[index].whisper_focus = true;
  return true;
}

// Le chuchotement ORDINAIRE : on prépare l'envoi dans la barre principale, on
// n'ouvre rien. C'est ce que fait le bouton « Select Receiver » du chat natif —
// remplir la box destinataire — et c'est ce que le joueur attend d'un menu
// « Chuchoter » quand il a refusé les fenêtres individuelles.
bool ChatWindow::TargetWhisper(const char* name_wire) {
  if (name_wire == nullptr || name_wire[0] == '\0') return false;
  // Interface moderne éteinte : la barre n'est pas dessinée, écrire dedans ne se
  // verrait nulle part. L'appelant retombe sur le chemin natif.
  if (!imgui_enabled_) return false;
  if (!input_bar_) return false;  // barre masquée par le joueur : même raison

  CopyBounded(whisper_, sizeof(whisper_), ro::WireToUtf8(name_wire));
  // 🔴 Le champ peut être ACTIF (le joueur y avait laissé son curseur) : sans ce
  // rappel, sa copie interne réécrirait l'ancien texte par-dessus le nom qu'on
  // vient d'y poser, à la frame suivante.
  NotifyWhisperEdited();
  // Barre repliée par le battle mode : la déplier, sinon le nom part dans un
  // champ que personne ne voit.
  if (battle_mode_) input_open_ = true;
  // Le clavier va à la SAISIE, pas au pseudo : le destinataire est choisi, ce
  // qu'il reste à faire, c'est écrire.
  focus_input_next_ = true;
  return true;
}

bool ChatWindow::IngestWhisper(const char* with_wire, const char* text_wire,
                               uint32_t rgb, uint32_t aid, bool outgoing) {
  const std::string with = ro::WireToUtf8(with_wire);

  // Ouverture d'une conversation : gouvernée par les MÊMES cases que le client,
  // celles de son « Friend Setup » (Alt+I). Remplacer une fenêtre native, c'est
  // aussi reprendre ses réglages — sans quoi le joueur qui a coupé les popups les
  // verrait revenir sous une autre forme.
  //
  // ⚠ Une conversation DÉJÀ ouverte reste alimentée quoi qu'il arrive : les cases
  // décident d'OUVRIR, elles ne décident pas de museler ce qui est là.
  const int index = (FindWhisperChannel(with) >= 0 || WhisperPopupWanted(with_wire))
                        ? FindOrCreateWhisper(with, aid)
                        : -1;
  // Personne pour l'afficher : on ne prend pas la ligne, et le client la met dans
  // la chatbox par son chemin habituel — exactement ce que fait sa popup absente.
  if (index < 0) return false;

  Line line;
  line.source       = 'P';  // ni ChatAction ni WndProc : notre propre branchement
  line.rgb          = rgb & 0xFFFFFF;
  line.type         = static_cast<uint8_t>(kTypeWhisper);
  line.whisper_with = with;

  SYSTEMTIME now;
  GetLocalTime(&now);
  line.hour   = static_cast<uint8_t>(now.wHour);
  line.minute = static_cast<uint8_t>(now.wMinute);
  line.second = static_cast<uint8_t>(now.wSecond);

  // L'AID obfusqué que le client colle dans l'écho (« [ To Nom (813-524) ] : ») :
  // ôté ici aussi, et AVANT l'extraction du sender.
  const std::string cleaned = StripWhisperAidTag(text_wire);
  const char* body = cleaned.c_str();

  // Le sender du natif est ici toujours en tête, séparé par « : ». On le laisse
  // extraire comme pour les autres types plutôt que d'imposer `with` : à l'aller,
  // le client écrit « ( To cible ) », et c'est bien CE libellé-là que le joueur
  // doit lire, pas le nom nu.
  const char* separator = std::strstr(body, " :");
  if (separator != nullptr && (separator - body) <= 40) {
    char extracted[64] = {};
    CopyBounded(extracted, sizeof(extracted), body);
    extracted[separator - body] = '\0';
    line.sender = ro::WireToUtf8(extracted);
  }
  ParseText(body, &line);
  MarkSenderAsPlayerLink(&line);
  RememberWhisperPeer(line);

  // ── La copie du JOURNAL ─────────────────────────────────────────────────────
  // 🔴 UN DEVOIR REPRIS AU CLIENT. Le pivot qu'on détourne ne nourrissait pas que
  // la popup : c'est le même geste qui posait la ligne dans le chat log, avec son
  // en-tête. En prenant la ligne (retour 1), on a supprimé les DEUX — et le
  // chuchotement se retrouvait dans le journal sous sa forme de CONVERSATION,
  // « Nom : texte », impossible à distinguer d'une parole publique. Mesuré en
  // jeu : `t02P` chez nous, là où le client écrivait `t02A ( To Nom ) : …`.
  //
  // On recompose donc l'en-tête, avec les formes du client (§11.7 de
  // docs/chatbox_re.md) — moins l'AID obfusqué, qui ne dit rien à personne et que
  // `StripWhisperAidTag` retire déjà partout ailleurs.
  //
  // ⚠ Le client choisit entre QUATRE en-têtes selon la relation (ami, membre de
  // guilde, ni l'un ni l'autre, écho). On n'en emploie que deux, les neutres :
  // connaître la relation demanderait d'interroger la liste d'amis et la guilde à
  // chaque ligne, pour une nuance que le journal ne doit pas porter — il doit
  // dire QUI et DANS QUEL SENS, et c'est tout.
  //
  // Le corps commence après le « : » du libellé de conversation ; sans séparateur
  // reconnu, on reprend le texte entier plutôt que de le tronquer au hasard.
  const char* message = body;
  if (separator != nullptr && (separator - body) <= 40) {
    message = separator + 2;  // au-delà de « :»
    while (*message == ' ') ++message;
  }
  std::string header = outgoing ? "( To " : "( From ";
  header += (with_wire != nullptr) ? with_wire : "";
  header += " )";

  Line note;
  note.source = 'P';
  note.rgb    = line.rgb;
  note.type   = line.type;
  note.hour   = line.hour;
  note.minute = line.minute;
  note.second = line.second;
  // 🔴 `whisper_with` reste VIDE : c'est ce qui l'envoie dans les onglets et
  // l'écarte des conversations. La ligne d'à côté fait exactement l'inverse.
  note.sender = ro::WireToUtf8(header.c_str());
  ParseText((header + " : " + message).c_str(), &note);
  MarkSenderAsPlayerLink(&note);  // le nom DANS le libellé reste cliquable

  std::lock_guard<std::mutex> lock(lines_mutex_);
  ++ingest_seen_;
  ingest_kept_ += 2;
  lines_.push_back(std::move(line));
  lines_.push_back(std::move(note));
  TrimLines();
  return true;
}

// ── Éviction : les `cap` dernières lignes de CHAQUE type ─────────────────────
// 🔴 APPELÉE SOUS `lines_mutex_`, et elle ne lit QUE `lines_` et ses compteurs.
// Surtout pas `channels_` : le rendu le remanie (onglets déplacés, fermés,
// regroupés), et l'ingestion ne tourne pas au même moment.
void ChatWindow::TrimLines() {
  const size_t cap =
      static_cast<size_t>(std::max(100, std::min(history_cap_, 5000)));

  // Les lignes qui viennent d'entrer, comptées ICI plutôt qu'à chacun des trois
  // sites d'ingestion : compte et éviction ne peuvent alors pas diverger.
  //
  // 🔴 C'est aussi ici, et NULLE PART AILLEURS, que se pose le rang d'une ligne
  // (`Line::seq`) : ce parcours est le seul qui voie chaque ligne neuve une fois
  // et une seule, sous le verrou, quel que soit le site qui l'a poussée. Un
  // quatrième site d'ingestion en hériterait sans rien avoir à savoir.
  //
  // ⚠ `LoadHistory` remet `counted_lines_` à zéro après avoir inséré EN TÊTE :
  // le tampon entier est alors renuméroté dans son ordre, ce qui remet les
  // lignes restaurées — les plus anciennes — devant celles de la session. Sans
  // ça, une ligne d'hier porterait un rang plus élevé que sa cadette.
  for (size_t i = counted_lines_; i < lines_.size(); ++i) {
    lines_[i].seq = next_line_seq_++;
    ++type_count_[TypeBucket(lines_[i].type)];
  }
  counted_lines_ = lines_.size();

  // Un type en surnombre perd sa PLUS VIEILLE ligne. Le parcours part du début et
  // s'arrête donc presque aussitôt : dans une rafale, la doyenne du type qui
  // déborde est justement en tête du tampon.
  for (int bucket = 0; bucket < kTypeBuckets; ++bucket) {
    while (type_count_[bucket] > cap) {
      bool removed = false;
      for (size_t i = 0; i < lines_.size(); ++i) {
        if (TypeBucket(lines_[i].type) != bucket) continue;
        lines_.erase(lines_.begin() + static_cast<ptrdiff_t>(i));
        --type_count_[bucket];
        --counted_lines_;
        removed = true;
        break;
      }
      // Compteur en avance sur la réalité (une purge a vidé le tampon sans
      // passer par ici) : on le recale plutôt que de tourner à vide.
      if (!removed) {
        type_count_[bucket] = 0;
        break;
      }
    }
  }

  // ⚠ Plafond DUR, et il ne sert qu'à ça : borner la mémoire si un serveur se met
  // à employer vingt types à la fois. Le quota par type suffit en pratique — six
  // types vivants à cinq cents lignes font trois mille lignes, quelques centaines
  // de kio. Au-delà, on évince à l'ancienne, du plus vieux.
  const size_t hard = std::max<size_t>(cap * 8, 4000);
  while (lines_.size() > hard) {
    --type_count_[TypeBucket(lines_.front().type)];
    lines_.pop_front();
    --counted_lines_;
  }
}

// Découpe une ligne en fragments : couleurs ^RRGGBB, icônes ^i[id], liens
// <ITEML>. Fait UNE fois, à l'ingestion — le rendu ne reparse rien.
void ChatWindow::ParseText(const char* local_text, Line* out) const {
  // 🔴 L'espace insécable se neutralise AVANT la conversion, sur les octets du
  // FIL — et surtout pas après. En latin-1 l'octet 0xA0 est un NBSP (les liens
  // natifs en sèment) ; en UTF-8 c'est un octet de CONTINUATION, celui du « à »
  // (C3 A0). Le remplacer après conversion cassait « à » en une séquence invalide
  // que l'atlas rendait en losange — d'où l'illusion d'un problème de code-page,
  // alors que les « é » (C3 A9) passaient très bien.
  //
  // 🔴 ET SEULEMENT SI LA LIGNE EST EN 1252. Depuis que le fil porte aussi de
  // l'UTF-8 (les emoji n'existent pas en 1252, cf. ro::WireToUtf8), la même
  // substitution appliquée à une ligne UTF-8 casserait précisément ce que le
  // commentaire ci-dessus décrit : le 0xA0 y est l'octet de continuation du
  // « à », et le remplacer par un espace laisserait un « Ã » orphelin.
  std::string wire = (local_text != nullptr) ? local_text : "";
  if (!ro::IsUtf8(wire.c_str()))
    for (char& ch : wire)
      if (static_cast<unsigned char>(ch) == 0xA0) ch = ' ';
  ParseUtf8(ro::WireToUtf8(wire.c_str()), out);
}

// La MOITIÉ balisage du parse, séparée de la conversion d'encodage. C'est par ici
// que rentre une ligne rechargée depuis notre historique : elle est déjà en UTF-8
// (on l'a écrite ainsi), et la repasser par `WireToUtf8` la corromprait.
void ChatWindow::ParseUtf8(const std::string& text, Line* out) const {
  out->raw = text;  // le balisage INTACT : c'est lui qu'on persiste
  Run current;
  auto flush = [&]() {
    if (!current.text.empty()) {
      out->runs.push_back(current);
      current.text.clear();
    }
  };

  const char* p   = text.c_str();
  const char* end = p + text.size();
  // UNE emote rendue par ligne, pas plus. Ce n'est pas une limite de place : le
  // relais Discord ne sait embarquer qu'une image par message (l'aperçu ne
  // remplace le lien que si celui-ci est TOUT le message), et une ligne qui
  // montrerait trois emotes en jeu en montrerait trois « :nom: » sur Discord.
  // Les suivantes restent donc du texte, des deux côtés du pont.
  bool emote_used = false;
  while (p < end) {
    // Couleur ^RRGGBB (^000000 = retour à la couleur de la ligne).
    if (*p == '^' && (end - p) >= 7 && IsHex6(p + 1)) {
      flush();
      const unsigned v = static_cast<unsigned>(
          std::strtoul(std::string(p + 1, p + 7).c_str(), nullptr, 16));
      current.color = (v == 0) ? 0 : HexRgbToImU32(v);
      p += 7;
      continue;
    }
    // ── **gras** et *italique* ────────────────────────────────────────────────
    //
    // La syntaxe de Discord : un message relayé se met donc en forme tout seul,
    // et un joueur peut l'écrire de la même façon.
    //
    // 🔴 UNE BASCULE NE S'OUVRE QUE SI SA FERMETURE EXISTE. Sans ce contrôle, un
    // « 3*4 » ou un « *soupir » sans fin passerait TOUTE la suite de la ligne en
    // italique — et le chat est plein d'astérisques isolées. On exige aussi que
    // le délimiteur ouvrant colle à son texte (« *mot », jamais « * mot ») : c'est
    // ce qui distingue une mise en forme d'une multiplication.
    //
    // ⚠ Volontairement PAS de `_italique_` : les tirets bas pullulent dans les
    // pseudos et les adresses, et le taux de faux positifs serait ingérable.
    if (*p == '*') {
      const bool  dbl = (end - p) >= 2 && p[1] == '*';
      const char* tok_end = p + (dbl ? 2 : 1);
      bool&       state = dbl ? current.bold : current.italic;
      bool        toggle = false;
      if (state) {
        // Fermeture : elle doit coller au texte qu'elle termine.
        toggle = (p > text.c_str()) && p[-1] != ' ';
      } else if (tok_end < end && *tok_end != ' ' && *tok_end != '*') {
        // Ouverture : il faut une fermeture plus loin, sinon on ne bascule pas.
        const char* q = tok_end;
        while (q < end && !toggle) {
          const char* hit = static_cast<const char*>(
              std::memchr(q, '*', end - q));
          if (hit == nullptr) break;
          const bool hit_dbl = (end - hit) >= 2 && hit[1] == '*';
          if (hit_dbl == dbl && hit > tok_end && hit[-1] != ' ') toggle = true;
          else q = hit + (hit_dbl ? 2 : 1);
        }
      }
      if (toggle) {
        flush();
        state = !state;
        p = tok_end;
        continue;
      }
      // Pas un délimiteur : l'astérisque est du texte ordinaire, elle tombera
      // dans l'accumulation générale plus bas.
    }
    // Icône d'objet ^i[<id décimal>] : le moteur natif la rend dans toutes ses
    // fenêtres TextLayout, un joueur peut donc en taper une.
    if (*p == '^' && (end - p) >= 4 && (p[1] == 'i' || p[1] == 'I') && p[2] == '[') {
      const char* rb =
          static_cast<const char*>(std::memchr(p + 3, ']', end - (p + 3)));
      if (rb != nullptr) {
        flush();
        Run icon;
        icon.item_id =
            static_cast<uint32_t>(std::atoi(std::string(p + 3, rb).c_str()));
        if (icon.item_id != 0) out->runs.push_back(icon);
        p = rb + 1;
        continue;
      }
    }
    // Emote Discord : <:nom:id> — ou <a:nom:id> quand elle est animée. C'est la
    // forme brute que Discord met dans le contenu du message, et le relais nous
    // la livre telle quelle.
    //
    // On la remplace par « :nom: » ET on note l'adresse du fichier sur le CDN,
    // qu'on RECONSTRUIT depuis l'identifiant. Le repli textuel compte autant que
    // l'image : lisible tout de suite, sans requête, et suffisant si le joueur a
    // coupé les images.
    if (*p == '<' && (end - p) >= 6 &&
        (p[1] == ':' || ((p[1] == 'a' || p[1] == 'A') && p[2] == ':'))) {
      const bool  animated = (p[1] != ':');
      const char* n0 = p + (animated ? 3 : 2);          // début du nom
      const char* c2 = static_cast<const char*>(std::memchr(n0, ':', end - n0));
      const char* gt = (c2 != nullptr)
                           ? static_cast<const char*>(std::memchr(c2, '>', end - c2))
                           : nullptr;
      // L'identifiant doit être un nombre : sans ce test, « <a:b:c> » tapé par un
      // joueur produirait une adresse absurde et une requête pour rien.
      bool numeric = (gt != nullptr) && (gt > c2 + 1);
      for (const char* q = c2 + 1; numeric && q < gt; ++q)
        if (*q < '0' || *q > '9') numeric = false;
      if (numeric && n0 < c2) {
        flush();
        Run emote;
        emote.text.assign(":").append(n0, c2).append(":");
        const std::string id(c2 + 1, gt);
        emote.emote_url = "https://cdn.discordapp.com/emojis/" + id +
                          (animated ? ".gif" : ".png");
        out->runs.push_back(emote);
        p = gt + 1;
        continue;
      }
    }
    // Emote du JEU : `:nom:`. Le nom doit être dans la table — sans ce test,
    // « 13:10 » ou « lui : ok » deviendraient des emotes, et le chat est plein de
    // deux-points. On accepte donc un nom de la table, et rien d'autre.
    //
    // 🔴 C'est cette forme-là qui part au serveur : les joueurs sans Bourgeon
    // liront « :sweat: », ce qui se comprend. Un index nu (« ^e[4] ») aurait été
    // plus court mais illisible pour eux, et le token natif du client ne sert de
    // toute façon que dans les fenêtres TextLayout, dont la chatbox ne fait pas
    // partie.
    if (*p == ':' && (end - p) >= 3 && !emote_used) {
      const char* q = p + 1;
      while (q < end && q - p <= 24 &&
             ((*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') || *q == '_'))
        ++q;
      if (q < end && *q == ':' && q > p + 1) {
        const int id = ro::emote::Find(p + 1, static_cast<size_t>(q - p - 1));
        if (id >= 0) {
          flush();
          Run em;
          em.game_emote = static_cast<int16_t>(id);
          em.text.assign(p, q + 1);  // le repli, deux-points compris
          out->runs.push_back(std::move(em));
          emote_used = true;
          p = q + 1;
          continue;
        }
      }
    }
    // Lien d'objet du chat : <ITEML>[5c equip b62][1c type décoré][nameid b62]
    // [champs facultatifs]</ITEML>. Le tag ne porte AUCUN texte lisible — c'est au
    // lecteur de composer le libellé, et il faut le composer à partir de TOUT ce
    // que la balise transporte : refine, grade, cartes, forgeron. Ne lire que le
    // nameid affichait « Axe » là où le natif écrit « Test's Axe ».
    if (*p == '<' && (end - p) >= 7 && std::strncmp(p, "<ITEML>", 7) == 0) {
      itemcell::ChatLink item;
      const char* tag_end = end;
      if (itemcell::ParseChatLink(p, end, &item, &tag_end)) {
        flush();
        Run icon;
        icon.item_id = item.id;
        icon.item    = item;
        out->runs.push_back(icon);
        Run link;
        link.item_id = item.id;
        link.item    = item;
        link.kind    = Run::kItem;
        // Le name-builder NATIF, sur un ItemSkillInfo fabriqué depuis la balise :
        // c'est le seul moyen d'obtenir mot pour mot ce qu'affiche le client (il
        // va jusqu'à demander au serveur le nom du forgeron qu'il ne connaît pas).
        // Il rend la code-page du client, d'où la conversion.
        char composed[192];
        itemcell::BuildChatLinkName(item, composed, sizeof(composed));
        const std::string name = (composed[0] != '\0')
                                     ? ro::WireToUtf8(composed)
                                     : std::string(itemcell::NameById(item.id));
        // Le format du natif, chevrons compris : `<+7 Sword [3]>`. Le nombre
        // d'emplacements est déjà dans le nom composé (BuildChatLinkName).
        link.text = "<" + name + ">";
        out->runs.push_back(link);
      }
      p = tag_end;
      continue;
    }
    // Lien de MONSTRE — balise à NOUS : `<MOBL>id:rang:nom</MOBL>`.
    //
    // 🔴 Le nom voyage DANS la balise, et ce n'est pas de la commodité : le client
    // ne sait pas nommer un monstre. Il n'a pas mob_db, et le nom n'est même pas
    // dans le paquet de la fiche (cf. project_monster_info_window) — c'est le
    // serveur qui le lui donne, à la demande. Un lien qui ne porterait que l'id
    // obligerait CHAQUE client recevant la ligne à interroger le serveur : un lien
    // posté dans `#global`, et c'est toute la population qui envoie un paquet.
    //
    // Champs séparés par ':' et le nom EN DERNIER, donc libre de contenir tout ce
    // qu'un nom de monstre contient (espaces, apostrophes, ponctuation).
    if (*p == '<' && (end - p) >= 7 && std::strncmp(p, "<MOBL>", 6) == 0) {
      const char* body  = p + 6;
      const char* close = SearchSub(body, end, "</MOBL>");
      if (close != nullptr) {
        const char* c1 = static_cast<const char*>(std::memchr(body, ':', close - body));
        const char* c2 = (c1 != nullptr)
                             ? static_cast<const char*>(std::memchr(c1 + 1, ':', close - (c1 + 1)))
                             : nullptr;
        if (c2 != nullptr) {
          const uint32_t id = static_cast<uint32_t>(
              std::strtoul(std::string(body, c1).c_str(), nullptr, 10));
          const int rank = std::atoi(std::string(c1 + 1, c2).c_str());
          const std::string name(c2 + 1, close);
          if (id != 0 && !name.empty()) {
            flush();
            Run link;
            link.kind     = Run::kMob;
            link.mob_id   = id;
            link.mob_rank = static_cast<uint8_t>((rank < 0 || rank > 2) ? 0 : rank);
            link.mob_name = name;
            link.text     = "<" + std::string(MobRankTag(link.mob_rank)) + " " + name + ">";
            out->runs.push_back(link);
          }
          p = close + 7;
          continue;
        }
      }
    }
    // RÉFÉRENCE d'objet — balise à NOUS : `<ITMR>id:nom</ITMR>`.
    //
    // 🔴 Pourquoi une seconde balise d'objet alors que `<ITEML>` existe : le
    // client REFUSE d'envoyer un `<ITEML>` portant un objet absent du sac
    // (« Item tags can only tag items you own. », en rouge, à l'envoi). La garde
    // est native et locale — le paquet ne part même pas, donc ni nous ni le
    // serveur ne pouvons l'assouplir. Or parler d'un objet qu'on n'a PAS est le
    // cas courant : « il me faut ça », « ça se fabrique avec quoi ? », toute
    // conversation partant de l'Atlas des recettes. Le client ne connaît pas
    // `<ITMR>`, donc il ne la filtre pas — exactement le détour déjà pris pour les
    // monstres avec `<MOBL>`.
    //
    // ⚠ Ce que ça COÛTE, et c'est assumé : un client sans Bourgeon affiche la
    // balise telle quelle. D'où le nom EN CLAIR dedans (comme `<MOBL>`) — la
    // ligne reste lisible, au prix des chevrons. `<ITEML>`, lui, est rendu par
    // tout le monde : il reste donc le choix par défaut dès que l'objet est en
    // sac (cf. AppendItemLinkFromLink).
    //
    // ⚠ Ce lien décrit l'objet de BASE, jamais une instance : ni refine, ni
    // cartes, ni forgeron. C'est cohérent avec ce qu'il désigne — une référence
    // au catalogue, pas l'objet de quelqu'un.
    if (*p == '<' && (end - p) >= 7 && std::strncmp(p, "<ITMR>", 6) == 0) {
      const char* body  = p + 6;
      const char* close = SearchSub(body, end, "</ITMR>");
      if (close != nullptr) {
        const char* c1 = static_cast<const char*>(std::memchr(body, ':', close - body));
        if (c1 != nullptr) {
          const uint32_t id = static_cast<uint32_t>(
              std::strtoul(std::string(body, c1).c_str(), nullptr, 10));
          const std::string name(c1 + 1, close);
          if (id != 0 && !name.empty()) {
            flush();
            // Un `Run::kItem` comme les autres : les gestes, l'aperçu au survol et
            // le menu contextuel marchent alors sans une ligne de plus. Seul le
            // `ChatLink` est réduit à son nameid — il n'y a rien d'autre à en
            // dire, et `link_gesture` sait déjà traiter ce cas (FromItemId).
            itemcell::ChatLink item;
            item.id = id;
            Run icon;
            icon.item_id = id;
            icon.item    = item;
            out->runs.push_back(icon);
            Run link;
            link.kind    = Run::kItem;
            link.item_id = id;
            link.item    = item;
            // Le nom TRANSPORTÉ, pas celui de notre DB : l'expéditeur peut jouer
            // dans une autre langue, et c'est ce qu'il a écrit qui fait foi.
            link.text = "<" + name + ">";
            out->runs.push_back(link);
          }
          p = close + 7;
          continue;
        }
      }
    }
    // RECETTE de fabrication — balise à NOUS : `<CRAF>id:nom</CRAF>`.
    //
    // Ce n'est pas un lien d'objet : il ne désigne pas l'objet mais la FAÇON DE
    // LE FAIRE. D'où un libellé qui l'annonce (« [Recette: Acid Bottle] »), un
    // aperçu qui montre métier et composants au lieu des stats, et un clic qui
    // ouvre l'Atlas plutôt que la description. C'est ce qu'on veut poster quand
    // on explique à quelqu'un comment fabriquer quelque chose — un `<ITEML>` ne
    // dit rien de tout cela.
    //
    // ⚠ Le nom voyage, comme dans `<ITMR>` : un client sans Bourgeon voit la
    // balise brute, et il vaut mieux qu'elle reste lisible.
    if (*p == '<' && (end - p) >= 7 && std::strncmp(p, "<CRAF>", 6) == 0) {
      const char* body  = p + 6;
      const char* close = SearchSub(body, end, "</CRAF>");
      if (close != nullptr) {
        const char* c1 = static_cast<const char*>(std::memchr(body, ':', close - body));
        if (c1 != nullptr) {
          const uint32_t id = static_cast<uint32_t>(
              std::strtoul(std::string(body, c1).c_str(), nullptr, 10));
          const std::string name(c1 + 1, close);
          if (id != 0 && !name.empty()) {
            flush();
            Run link;
            link.kind    = Run::kRecipe;
            link.item_id = id;
            link.item.id = id;
            link.text    = RecipeLinkLabel(name);
            out->runs.push_back(link);
          }
          p = close + 7;
          continue;
        }
      }
    }
    // DESTINATION DE RÉGLAGES — balise à NOUS : `<SETL>clé:libellé</SETL>`.
    // La clé désigne un en-tête du panneau (« graphics ») ou une section de sa nav
    // (« item_toast ») : un seul espace de clés pour les deux étages.
    //
    // Le premier lien qui ne parle pas du monde mais du CLIENT. « Va voir le
    // réglage Objet obtenu » est ce qu'on répond vingt fois par jour dans un chat
    // d'entraide, et le décrire par un chemin (« panneau Moonlight, en-tête
    // Interface de jeu, huitième entrée ») ne marche jamais : le lien, lui, ouvre
    // le panneau déjà déplié au bon endroit.
    //
    // 🔴 UNE CLÉ, PAS UN NUMÉRO. Un numéro de section décrit l'ordre d'UNE version
    // de Bourgeon : une entrée insérée entre-temps et le lecteur atterrirait en
    // silence sur le réglage voisin. La clé, elle, désigne la destination —
    // inconnue chez le lecteur, elle ne résout rien, ce qui est le bon échec. Une
    // destination qui n'existe pas CHEZ LUI (« Staff Tools » hors staff) échoue
    // pareillement, et pour la même raison : le lien n'aurait rien à ouvrir.
    //
    // ⚠ Le libellé transporté ne sert QU'À un client sans Bourgeon, qui verra la
    // balise brute : ici c'est le libellé LOCAL qui gagne, donc traduit dans la
    // langue du lecteur (même règle que `<CRAF>`).
    if (*p == '<' && (end - p) >= 7 && std::strncmp(p, "<SETL>", 6) == 0) {
      const char* body  = p + 6;
      const char* close = SearchSub(body, end, "</SETL>");
      if (close != nullptr) {
        const char* c1 = static_cast<const char*>(std::memchr(body, ':', close - body));
        if (c1 != nullptr) {
          const std::string key(body, c1);
          const std::string fallback(c1 + 1, close);
          if (iface::DestLabel(key.c_str()) != nullptr) {
            flush();
            Run link;
            link.kind        = Run::kSetting;
            link.setting_key = key;
            link.text        = links::SettingLabel(key.c_str());
            out->runs.push_back(link);
          } else if (!fallback.empty()) {
            // Destination inconnue ou indisponible : le fragment reste du TEXTE
            // ORDINAIRE, avec le libellé que l'expéditeur a transporté. Il dit
            // encore de quoi on parle, il ne prétend simplement plus mener quelque
            // part. Accumulé dans `current` et non poussé à part, pour qu'il garde
            // la couleur et la graisse en cours — c'est du texte, il doit se
            // comporter comme tel.
            current.text += '[';
            current.text += fallback;
            current.text += ']';
          }
          p = close + 7;
          continue;
        }
      }
    }
    // Adresse web. Rien à transporter : le joueur tape son URL, tout le monde
    // reçoit le même texte — nous sommes seulement les seuls à la rendre
    // cliquable. La chatbox NATIVE, elle, n'en fait rien : son seul détecteur de
    // liens (`UISubChatWnd_AppendDrawnLine 0x0083d840`) ne teste que `<ITEML>`,
    // vérifié sur l'initialiseur du littéral (0x00475fe0 → « <ITEML> »).
    // Début de MOT exigé : sans ça, « voirhttps://… » ouvrirait un lien au beau
    // milieu d'un mot et le couperait en deux à l'affichage.
    const bool at_word_start =
        (p == text.c_str()) || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n' ||
        p[-1] == '\r' || p[-1] == '(';
    if (at_word_start && (*p == 'h' || *p == 'H' || *p == 'w' || *p == 'W') &&
        IsUrlStart(p, end)) {
      const char* stop = UrlEnd(p, end);
      if (stop > p) {
        flush();
        Run link;
        link.kind = Run::kUrl;
        link.url.assign(p, stop);
        link.text = link.url;
        out->runs.push_back(link);
        p = stop;
        continue;
      }
    }
    current.text += *p;  // (le NBSP a déjà été neutralisé avant la conversion)
    ++p;
  }
  flush();

  out->plain.clear();
  for (const Run& run : out->runs) out->plain += run.text;
}

void ChatWindow::ParseWireLine(const char* wire, Line* out) const {
  if (out == nullptr) return;
  *out = Line();
  if (wire == nullptr || *wire == '\0') return;
  // `WireToUtf8` rend un tampon statique : on le copie avant d'appeler quoi que
  // ce soit d'autre qui pourrait le réutiliser.
  const char* utf8 = ro::WireToUtf8(wire);
  if (utf8 == nullptr) return;
  ParseUtf8(std::string(utf8), out);
}

std::string ChatWindow::PlainTextFromWire(const char* wire) const {
  Line line;
  ParseWireLine(wire, &line);
  return line.plain;
}

// ── Canaux ───────────────────────────────────────────────────────────────────
bool ChatWindow::ChannelAccepts(const Channel& channel, const Line& line) const {
  // ── « Vider cet onglet », AVANT toute autre règle ───────────────────────────
  // C'est un geste du joueur sur SA vue, et il vaut pour une conversation 1:1
  // comme pour un onglet ordinaire. Il masque, il ne détruit pas : les lignes
  // restent dans le tampon partagé, les autres onglets les gardent, et
  // « Réafficher » les ramène toutes.
  //
  // 🔴 La garde sur zéro n'est pas décorative : tant que rien n'a été vidé, la
  // règle ne doit pas s'appliquer DU TOUT. Sans elle, une ligne dont le rang
  // n'aurait pas encore été posé (seq 0) disparaîtrait de tous les onglets.
  if (channel.clear_seq != 0 && line.seq <= channel.clear_seq) return false;
  // 🔴 Une conversation 1:1 filtre par CORRESPONDANT, avant tout le reste — le
  // mode diagnostic compris. Y laisser passer quoi que ce soit d'autre serait
  // pire qu'inutile : c'est la fenêtre où le joueur répond sans relire la cible.
  if (!channel.whisper_with.empty())
    return line.whisper_with == channel.whisper_with;
  // 🔴 ET SYMÉTRIQUEMENT : une ligne qui APPARTIENT à une conversation n'entre
  // jamais dans un onglet ordinaire. Elle est écrite pour SA fenêtre, en style
  // conversation — « Gettar : salut », « Stingor : salut » — où le libellé se
  // devine du titre. Dans le journal, la même ligne est indiscernable d'une
  // parole publique : c'est exactement ce qu'on lisait, un chuchotement affiché
  // comme si on l'avait crié sur la place.
  //
  // Le journal n'est pas privé de chuchotements pour autant : `IngestWhisper` lui
  // en pose une copie À LUI, avec l'en-tête du client (« ( To Nom ) »,
  // « ( From Nom ) ») et SANS `whisper_with` — donc prise ici, et soumise comme
  // avant à la case « Whisper » de l'onglet.
  //
  // Placé AVANT le diagnostic, comme la règle du dessus : c'est du ROUTAGE, pas
  // du filtrage. Le diagnostic désactive les filtres de type, il ne renvoie pas
  // une ligne dans une fenêtre qui n'est pas la sienne.
  if (!line.whisper_with.empty()) return false;
  // Nos propres repères ne sont d'aucun type : ils décrivent le journal, ils n'y
  // participent pas. Avant le diagnostic pour la même raison que ci-dessus.
  if (line.pinned) return true;
  if (diagnostic_) return true;  // diagnostic : on ne filtre rien
  // 🔴 Le broadcast (t25) EST filtré, contrairement au natif : c'est la 26e case,
  // celle que le client ne pouvait pas offrir faute d'octet pour la ranger. Tout
  // ce qui dépasserait encore reste affiché — un type qu'un serveur inventerait
  // sans nous prévenir doit se voir, pas se perdre.
  static_assert(sizeof(Channel::filter) == static_cast<size_t>(kFilterCount),
                "la table de filtre du .h et kFilterCount ont divergé");
  if (line.type >= kFilterCount) return true;
  return channel.filter[line.type] != 0;
}

void ChatWindow::RefreshChannels() {
  // 🔴 Le registre natif est un AMORÇAGE, pas une source permanente. Dès que le
  // joueur a modifié la structure chez nous, le relire ne pourrait que défaire son
  // travail : ressusciter l'onglet fermé, rendre son ancien nom à celui qu'il a
  // renommé, redocker celui qu'il a arraché. Il faudra écrire dans les registres
  // pour rendre tout ça visible du client — c'est le chantier suivant ; d'ici là,
  // notre liste fait foi et le registre ne sert plus qu'à situer les nœuds de
  // filtre déjà connus.
  if (structure_owned_) return;

  const uint32_t now = GetTickCount();
  if (!channels_.empty() && (now - channels_stamp_) < 2000) return;
  channels_stamp_ = now;

  RawChannel raw[kMaxChannels * 2];
  const int main_count = ReadRegistry(kChannelRegistryAddr, raw, kMaxChannels);
  const int total =
      main_count + ReadRegistry(kDetachedRegistryAddr, raw + main_count, kMaxChannels);

  // 🔴 On FUSIONNE avec la liste existante, on ne la reconstruit PAS. Le rebuild
  // remplaçait le vecteur entier toutes les deux secondes : tout ce que nous
  // portons nous-mêmes sur un canal — identifiant stable, réglages, état détaché —
  // était effacé sans trace, y compris deux secondes après le geste du joueur.
  // C'est le préalable à tout ce qui rend les onglets vivants.
  const uint32_t active_id =
      (active_channel_ >= 0 && active_channel_ < static_cast<int>(channels_.size()))
          ? channels_[active_channel_].id
          : 0;
  std::vector<Channel> merged;
  std::vector<bool>    taken(channels_.size(), false);

  for (int i = 0; i < total; ++i) {
    const bool detached = (i >= main_count);
    // Les noms d'onglets viennent du .lua de sauvegarde relu par le CLIENT :
    // c'est sa code-page, pas celle du fil (cf. ro_imgui.h).
    std::string name = ro::LocalToUtf8(raw[i].name);
    if (name.empty()) name = "Chat";

    // Appariement, du plus sûr au plus faible. L'ADRESSE du nœud est stable tant
    // que l'entrée vit ; l'index ne l'est qu'entre deux renumérotations ; le nom
    // ne départage pas deux homonymes — d'où l'ordre, et le « premier non pris ».
    // 🔴 Les conversations 1:1 sont HORS appariement : elles ne viennent d'aucun
    // registre. Sans cette exclusion, le troisième recours — l'appariement par
    // NOM — livrerait la conversation ouverte avec « Filip » au premier onglet
    // que le joueur aurait nommé « Filip », et la transformerait en onglet.
    auto eligible = [&](size_t k) {
      return !taken[k] && channels_[k].whisper_with.empty();
    };
    int found = -1;
    for (size_t k = 0; k < channels_.size() && found < 0; ++k)
      if (eligible(k) && raw[i].node != 0 && channels_[k].node == raw[i].node)
        found = static_cast<int>(k);
    for (size_t k = 0; k < channels_.size() && found < 0; ++k)
      if (eligible(k) && channels_[k].detached == detached &&
          channels_[k].index == raw[i].index)
        found = static_cast<int>(k);
    for (size_t k = 0; k < channels_.size() && found < 0; ++k)
      if (eligible(k) && channels_[k].name == name) found = static_cast<int>(k);

    Channel channel;
    if (found >= 0) {
      channel = channels_[found];  // on GARDE ce qui est à nous, à commencer par l'id
      taken[found] = true;
    } else {
      channel.id = next_channel_id_++;
    }
    channel.index    = raw[i].index;
    // 🔴 Notre état gagne dès que le joueur y a touché : tant que le déplacement
    // de l'entrée entre les deux registres natifs n'est pas écrit, le registre
    // continuerait d'affirmer le contraire à chaque fusion.
    //
    // Le registre natif ne connaît que « principal » ou « détaché » — il n'a
    // aucune idée de nos GROUPES. Un canal détaché qu'il nous apprend naît donc
    // dans sa fenêtre à lui ; c'est notre fichier de disposition, lui, qui sait
    // les réunir (et dès qu'il fait autorité, cette fusion ne tourne plus).
    if (!channel.detach_owned)
      SetChannelGroup(channel, detached ? (channel.group != 0 ? channel.group
                                                              : NewGroupId())
                                        : 0u);
    channel.node     = raw[i].node;
    channel.name     = std::move(name);
    // 🔴 La taille de la SOURCE, jamais celle de la destination : le POD relevé
    // dans le registre n'a que 25 octets, notre table en a 26. `sizeof` sur la
    // destination lirait un octet au-delà de `raw[i]` — et écraserait au passage
    // notre case broadcast avec ce qu'il aurait trouvé là.
    std::memcpy(channel.filter, raw[i].filter, sizeof(raw[i].filter));
    // Le registre natif ignore tout du broadcast : un canal qu'il vient de nous
    // apprendre le laisse donc passer, comme le client l'a toujours fait. Un
    // canal déjà connu, lui, garde le choix du joueur — il est déjà dans
    // `channel`, recopié de `channels_[found]` un peu plus haut.
    if (found < 0) channel.filter[kTypeBroadcast] = 1;
    merged.push_back(std::move(channel));
  }

  if (merged.empty()) {
    // Registre illisible (avant l'entrée en jeu, p. ex.) : un canal qui accepte
    // tout vaut mieux qu'une fenêtre vide sans explication. S'il est DÉJÀ en place
    // (node nul = c'est le nôtre), on le laisse tel quel plutôt que d'en fabriquer
    // un neuf toutes les deux secondes — son identifiant doit rester stable lui
    // aussi, sinon ses réglages repartiraient de zéro en boucle.
    //
    // ⚠ Le compte se fait hors conversations 1:1 : elles ne sont pas des onglets,
    // et une conversation ouverte ne doit pas faire croire que la bande est
    // pourvue — la fenêtre dockée resterait alors vide, sans rien pour l'expliquer.
    int tabs = 0, own_fallback = -1;
    for (size_t k = 0; k < channels_.size(); ++k) {
      if (!channels_[k].whisper_with.empty()) continue;
      ++tabs;
      if (channels_[k].node == 0) own_fallback = static_cast<int>(k);
    }
    if (tabs == 1 && own_fallback >= 0) return;
    Channel fallback;
    fallback.id   = next_channel_id_++;
    fallback.name = "Public";
    std::memset(fallback.filter, 1, sizeof(fallback.filter));
    merged.push_back(std::move(fallback));
  }

  // 🔴 Les conversations 1:1 ne viennent d'AUCUN registre : rien ne les apparie,
  // et la fusion les effacerait donc toutes les deux secondes — fenêtre ouverte
  // comprise, en pleine conversation. On les reporte telles quelles, et APRÈS le
  // repli ci-dessus : celui-ci peut encore renoncer par un `return`, qui laisserait
  // sinon derrière lui des canaux vidés par le déplacement.
  for (size_t k = 0; k < channels_.size(); ++k)
    if (!taken[k] && !channels_[k].whisper_with.empty())
      merged.push_back(std::move(channels_[k]));

  channels_.swap(merged);

  // Rester sur le MÊME canal, pas au même rang : la fusion peut réordonner, et
  // suivre le rang ferait sauter le joueur d'un onglet à l'autre tout seul.
  active_channel_ = 0;
  for (size_t k = 0; k < channels_.size(); ++k) {
    if (channels_[k].id == active_id) {
      active_channel_ = static_cast<int>(k);
      break;
    }
  }
}

// ── Rendu ────────────────────────────────────────────────────────────────────
// Une fenêtre par canal DÉTACHÉ, plus la fenêtre dockée qui porte les autres en
// onglets. C'est la répartition du client : sa `UIChatWnd` détachée est une
// fenêtre à part entière, pas un onglet déplacé.
void ChatWindow::OnRenderUI() {
  if (!imgui_enabled_) return;
  // Hôtes autorisés par le joueur : la liste vivante est celle d'imgprev, ce champ
  // n'en est que la forme persistée. On les resynchronise par COMPARAISON plutôt
  // qu'à un moment précis du chargement — l'ordre d'initialisation des réglages
  // n'est pas quelque chose sur quoi il faut parier.
  if (url_hosts_ != url_hosts_seen_) {
    imgprev::SetUserHostsCsv(url_hosts_);
    url_hosts_seen_ = url_hosts_;
  }
  // Relevée ICI, hors de toute fenêtre : c'est la seule taille de police qui ne
  // porte l'échelle d'aucune d'entre elles. Tout le log s'en déduit.
  base_font_size_ = ImGui::GetFontSize();
  // L'aimant vit dans ui/window_clamp : sa passe tourne bien avant nous, en tête
  // de frame. On lui repousse le réglage à chaque frame plutôt qu'au moment où il
  // change — c'est une case, pas un événement, et l'ordre de chargement des
  // réglages n'est pas quelque chose sur quoi il faut parier.
  ro::SetWindowMagnet(magnet_);
  // Le battle mode appartient au CLIENT : on le relit à chaque frame plutôt que
  // d'en tenir une copie. En sortir doit rendre la barre tout de suite ; y entrer
  // doit la replier, sinon elle resterait ouverte jusqu'au prochain Échap.
  const bool battle_now = ReadNativeBattleMode();
  if (battle_now != battle_mode_) {
    battle_mode_ = battle_now;
    input_open_  = false;
  }
  RefreshChannels();
  // L'onglet actif ne peut pas désigner un canal détaché : il n'est plus dans la
  // bande. Sans ce recalage, détacher l'onglet courant laisserait la fenêtre
  // dockée pointer un canal qu'elle n'affiche plus.
  if (active_channel_ >= 0 && active_channel_ < static_cast<int>(channels_.size()) &&
      channels_[active_channel_].detached) {
    active_channel_ = 0;
    for (size_t i = 0; i < channels_.size(); ++i) {
      if (!channels_[i].detached) {
        active_channel_ = static_cast<int>(i);
        break;
      }
    }
  }

  // La bande n'est valide que si la fenêtre dockée a été dessinée cette frame :
  // repliée ou masquée, elle ne peut pas servir de cible de recollage — et une
  // cible invisible qui accepte quand même est pire que pas de cible du tout.

  // 🔴 ENTRÉE se traite AVANT le dessin, et pas dans la ligne de saisie : en
  // battle mode celle-ci n'est pas dessinée du tout, donc rien n'y consommerait la
  // touche — la barre ne se serait jamais ouverte.
  // La touche est relevée au clavier, la décision se prend ICI : c'est le seul
  // endroit où l'on sait qu'une AUTRE zone de texte a déjà le focus (recherche,
  // renommage, panneau de réglages). Sans ce test, chaque Entrée le lui volerait.
  //
  // 🔴 ET ELLE NE SERT PLUS À REPRENDRE LE CLAVIER. C'était le nœud : tant
  // qu'Entrée devait d'abord rendre le focus, elle ne pouvait pas refermer, et une
  // barre ouverte sans clavier devenait un piège — plus moyen d'en sortir ni d'y
  // écrire sans aller cliquer dedans à la souris. Depuis que TAPER rend le clavier
  // tout seul (cf. `WantsTypedKeys`), Entrée retrouve son seul sens natif, et il
  // vaut que la barre ait le focus ou non :
  //   • fermée        → on l'ouvre, avec le clavier ;
  //   • ouverte, vide → on SORT, en une frappe ;
  //   • ouverte, pleine → on ENVOIE, et on garde la main.
  if (enter_pending_) {
    enter_pending_ = false;
    // Si une zone de texte écrit — la nôtre comprise — c'est elle qui voit la
    // touche : la ligne de saisie referme et envoie déjà par son propre chemin.
    if (!ImGui::GetIO().WantTextInput) {
      // 🔴 « OUVERTE » NE VEUT PAS DIRE LA MÊME CHOSE DANS LES DEUX MODES. En
      // battle mode c'est le dépliement (`input_open_`) ; hors battle mode la
      // barre est là en permanence, et exiger `input_open_` renvoyait TOUJOURS
      // vers la branche « on l'ouvre » — une barre pleine mais sans clavier
      // demandait donc DEUX Entrée pour envoyer, la première ne servant qu'à
      // reprendre le focus. C'est ce qui bloquait un lien posé pendant un
      // dialogue NPC, où la touche est confisquée puis remise ici.
      const bool row_open = battle_mode_ ? input_open_ : InputRowVisible();
      if (row_open) {
        if (input_[0] != '\0') {
          QueueSend();
          focus_input_next_ = true;
        } else if (battle_mode_) {
          input_open_ = false;
        } else {
          focus_input_next_ = true;  // rien à envoyer : la touche rend le clavier
        }
      } else {
        if (battle_mode_) input_open_ = true;
        focus_input_next_ = true;
      }
    }
  }
  // ── ÉCHAP referme la barre, avec ou sans clavier ────────────────────────────
  // 🔴 Traité ICI et pas dans la ligne de saisie, pour la même raison qu'Entrée :
  // la saisie ne voit Échap que si elle a le focus (`IsItemDeactivated`). Une
  // barre ouverte que le joueur a quittée d'un clic ne se refermait donc plus du
  // tout — il fallait deux Entrée, une pour reprendre la main et une pour la ligne
  // vide. C'EST LA SORTIE À UNE FRAPPE, à n'importe quel moment, et c'est ce qui
  // permet à la barre de rendre le clavier à ImGui sans piéger le joueur.
  //
  // Rien si une AUTRE zone de texte écrit : Échap lui appartient (elle annule sa
  // saisie). Et si c'est NOTRE champ qui l'a, la ligne de saisie s'en charge —
  // elle seule sait restaurer le texte en même temps.
  // ── « Taper écrit dans la barre » ───────────────────────────────────────────
  // 🔴 CAPTURE D'ABORD, RESTITUTION ENSUITE, et cet ordre-là compte : la
  // restitution passe par `AddInputCharacter`, qui écrit dans la MÊME file que
  // celle qu'on lit ici. L'inverse relirait ce qu'on vient d'y remettre, en
  // boucle.
  //
  // On ne capture pas tant qu'une demande de focus est en vol : le champ
  // s'active à la frame suivante et reprend alors la file tout seul.
  ImGuiIO& io = ImGui::GetIO();
  if (WantsTypedKeys() && typed_pending_.empty() && !focus_input_next_ &&
      !focus_whisper_next_ && io.InputQueueCharacters.Size > 0) {
    for (ImWchar ch : io.InputQueueCharacters) {
      // Seuls les caractères IMPRIMABLES ouvrent la saisie. Entrée (0x0D) a son
      // propre chemin, Retour arrière et Tabulation n'ont rien à effacer ni à
      // parcourir dans un champ qui n'est pas encore là.
      if (ch >= 0x20 && ch != 0x7F) typed_pending_.push_back(ch);
    }
    if (!typed_pending_.empty()) {
      if (focus_on_whisper_)
        focus_whisper_next_ = true;
      else
        focus_input_next_ = true;
    }
  }
  // Rendues dès que la demande de focus est partie (les drapeaux sont retombés) :
  // le champ s'active à CETTE frame-ci, et il lit la file au moment où il est
  // soumis — donc après nous.
  if (!typed_pending_.empty() && !focus_input_next_ && !focus_whisper_next_) {
    // Ce que le joueur a tapé PENDANT le battement passe derrière : on vide la
    // file et on la réécrit dans l'ordre, sinon la deuxième lettre arriverait
    // avant la première.
    for (ImWchar ch : io.InputQueueCharacters) typed_pending_.push_back(ch);
    io.InputQueueCharacters.resize(0);
    // Barre disparue entre-temps (réglage, sortie du battle mode) : on jette
    // plutôt que de garder des frappes qui ressortiraient bien plus tard.
    if (InputRowVisible())
      for (ImWchar ch : typed_pending_) io.AddInputCharacter(ch);
    typed_pending_.clear();
  }

  if (escape_pending_) {
    escape_pending_ = false;
    if (battle_mode_ && input_open_ && !ImGui::GetIO().WantTextInput) {
      input_open_ = false;
      // La touche est CONSOMMÉE : sans ça elle refermerait aussi la fenêtre RO du
      // dessus (`ro::ProcessEscapeStack`, après tous les OnRenderUI).
      ro::SuppressEscapeStack();
    }
  }

  // ── ⛔ IL N'Y A PLUS DE REPRISE AUTOMATIQUE DU CLAVIER, ET IL NE PEUT PAS Y EN
  // AVOIR. Elle a existé ici, pour tenir la règle du chat natif : « une barre
  // ouverte ne lâche jamais le clavier ». Elle tenait — au prix de TOUTES les
  // interactions à la souris du client, et ce n'est pas réparable.
  //
  // La cause est dans le modèle d'ImGui, pas dans notre code : tant qu'un widget
  // détient l'`ActiveId`, TOUT le reste est réputé non survolable. Quatre portes
  // le disent, toutes vérifiées dans imgui.cpp :
  //   • `ItemHoverable` (4982) et `ButtonBehavior` (4894) : aucun autre widget
  //     n'est survolable ⇒ le PREMIER clic ailleurs ne sert qu'à défocaliser ;
  //   • `IsWindowHovered` (8507) : la fenêtre visée n'est même pas focalisée par
  //     ce clic ;
  //   • le repli au double-clic sur une barre de titre (7715) exige
  //     `g.ActiveId == 0` — un double-clic ne pouvait donc JAMAIS aboutir, la
  //     reprise reprenant l'`ActiveId` entre les deux clics.
  // Et `ActiveIdAllowOverlap`, qui lèverait ces refus, n'est réglable que depuis
  // le glisser-déposer : aucune API publique ne l'expose.
  //
  // La saisie ne prend donc le clavier QUE sur un geste : Entrée, un clic dedans,
  // un envoi, un lien posé. C'est le comportement de n'importe quelle application
  // ImGui, et le joueur le connaît sans l'avoir appris.
  //
  // 🔴 CE QUI COMPTAIT VRAIMENT EST PRÉSERVÉ — la sortie à UNE frappe, à
  // n'importe quel moment : Échap referme la barre avec ou sans clavier (juste
  // au-dessus), et Entrée sur un texte vide la referme quand elle l'a. Le seul
  // renoncement est de taper SANS avoir repris la main d'abord ; une Entrée la
  // rend, et elle ne se perd pas — elle ouvre la saisie.

  // Les cibles de dépôt se reconstruisent à chaque frame : une fenêtre repliée ou
  // fermée ne doit pas rester une cible. Et la désignation du lâcher avec elles.
  strips_.clear();
  drop_valid_ = false;

  DrawDockedWindow();
  // 🔴 UNE FENÊTRE PAR GROUPE, et non par canal : c'est tout le groupage. Les
  // groupes sont relevés d'abord, parce que dessiner peut en changer l'occupation
  // (un onglet lâché ailleurs) et qu'un parcours qui découvrirait les groupes au
  // fil de l'eau en sauterait un.
  uint32_t seen[kMaxChannels] = {};
  int seen_n = 0;
  for (const Channel& channel : channels_) {
    if (channel.group == 0) continue;
    bool known = false;
    for (int k = 0; k < seen_n; ++k) known = known || (seen[k] == channel.group);
    if (!known && seen_n < kMaxChannels) seen[seen_n++] = channel.group;
  }
  for (int k = 0; k < seen_n; ++k) DrawGroupWindow(seen[k]);

  // 🔴 ICI, et pas dans une fenêtre : une modale ImGui doit s'ouvrir et se dessiner
  // au même niveau de pile, sinon l'identifiant qu'`OpenPopup` enregistre n'est pas
  // celui que `BeginPopupModal` cherche — et la modale n'apparaît jamais. Toutes
  // les fenêtres du chat sont refermées à ce point.
  DrawCloseConfirmPopup();

  // Fermeture différée : cf. `close_channel_id_`. L'historique, lui, RESTE — une
  // conversation se rouvrira avec ce qui a déjà été dit, ce qui est le seul
  // comportement raisonnable quand on referme par erreur.
  if (close_channel_id_ != 0) {
    for (size_t i = 0; i < channels_.size(); ++i) {
      if (channels_[i].id != close_channel_id_) continue;
      // Jamais le dernier canal, ni le dernier onglet de la fenêtre principale :
      // elle porte la saisie et ne doit pas rester sans rien à afficher. C'est la
      // même règle que le menu contextuel, ici pour le clic molette.
      const bool docked_last =
          channels_[i].group == 0 && GroupSize(0) <= 1;
      if (channels_.size() > 1 && !docked_last) CloseChannel(static_cast<int>(i));
      break;
    }
    close_channel_id_ = 0;
  }

  // ── Le lâcher d'un onglet, une fois TOUTES les bandes dessinées ─────────────
  // Ici seulement : c'est la dernière position où l'on connaît les cibles de la
  // frame, et où plus aucune fenêtre ne parcourt `channels_` par indice — le
  // déplacement réordonne le vecteur.
  if (drag_tab_ >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    const int dragged = drag_tab_;
    drag_tab_ = -1;
    if (dragged < static_cast<int>(channels_.size())) {
      if (drop_valid_) {
        MoveChannelToGroup(dragged, drop_group_, drop_slot_);
      } else if (channels_[dragged].group != 0 || GroupSize(0) > 1) {
        // Lâché dans le vide : il fonde SA fenêtre, sous le curseur. Le dernier
        // onglet de la principale, lui, ne part pas — elle porte la saisie.
        const uint32_t group = NewGroupId();
        MoveChannelToGroup(dragged, group, 0);
        pending_pos_id_ = group;
        pending_pos_ = ImVec2(ImGui::GetIO().MousePos.x - 20.0f,
                              ImGui::GetIO().MousePos.y - 6.0f);
      }
    }
  }

  // Filet de sécurité, en FIN de frame et pas au début : un geste dont la fenêtre
  // a cessé d'être dessinée (repli, canal disparu) laisserait sinon un glissement
  // fantôme, actif jusqu'au prochain clic. Le placer au début casserait le lâcher,
  // qui se produit précisément sur la frame où le bouton n'est plus enfoncé.
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) drag_tab_ = -1;
  // La demande de focus est consommée par la ligne de saisie ; si celle-ci est
  // désactivée dans les réglages, personne ne la consommerait et elle
  // s'appliquerait au premier réaffichage de la barre, longtemps après la touche.
  enter_pending_ = false;

  // 🔴 HORS de toute fenêtre : une modale est une fenêtre à elle seule, et
  // l'ouvrir depuis l'intérieur de la chatbox l'imbriquerait dans sa pile d'ID.
  // C'est la chatbox qui l'héberge parce que c'est la seule surface qui produit
  // des liens d'adresse (`Run::kUrl`) — si une autre s'y met, elle devra appeler
  // ceci elle aussi, une fois par frame.
  links::DrawUrlConfirm();
}

// Le skin d'un canal. `channel` peut être nul (registre pas encore lisible) : la
// fenêtre s'habille quand même, elle sera simplement vide.
ro::RoChatSkin ChatWindow::MakeSkin(const Channel* channel) const {
  ro::RoChatSkin skin;
  skin.body_col   = ro::ImU32FromPicker(EffBody(channel));
  skin.border_col = ro::ImU32FromPicker(border_rgba_);
  // L'échelle de la FENÊTRE n'habille plus que l'habillage : onglets, boutons,
  // ligne de saisie. Le log, lui, se dessine à taille explicite (LogFontSize).
  skin.font_scale = static_cast<float>(ui_scale_pct_) / 100.0f;
  skin.padding    = static_cast<float>(EffPadding(channel));
  skin.line_gap   = static_cast<float>(EffLineGap(channel));
  // 🔴 La MÊME règle de rangées que la contrainte de taille. Les deux doivent la
  // connaître : la contrainte corrige ce qui entre par ailleurs (restauration de
  // position, changement d'interligne), le redimensionnement par les bords doit
  // produire d'emblée une hauteur conforme — sinon la correction d'ImGui, qui ne
  // touche que la taille, fait dériver le bord d'en face.
  // Les métriques sont celles de la frame PRÉCÉDENTE, et elles vivent sur le
  // CANAL : deux fenêtres n'ont ni la même hauteur de ligne ni le même chrome.
  skin.snap_step = (channel != nullptr) ? channel->line_h : 0.0f;
  skin.snap_base = (channel != nullptr) ? channel->chrome_h : 0.0f;
  // Verrouillage : ni déplacement ni redimensionnement. Une chatbox bien réglée se
  // déplace ensuite par accident, en visant un onglet — c'est précisément ce que
  // cette option évite.
  //
  // 🔴 UN VERROU PAR FENÊTRE. Un canal détaché — conversation 1:1 comprise — porte
  // le sien ; la fenêtre principale a `locked_`. Les confondre, c'était clouer au
  // sol une flottante qu'on venait d'arracher parce que le chat principal, lui,
  // était bien placé.
  const bool frozen = (channel != nullptr && channel->detached) ? channel->locked
                                                                : locked_;
  skin.movable   = !frozen;
  skin.resizable = !frozen;
  return skin;
}

// Contraintes de taille de la fenêtre qu'on s'apprête à ouvrir. Le maximum est
// relatif à l'écran plutôt que figé, pour suivre un changement de résolution.
void ChatWindow::ApplySizeConstraints(const ro::RoChatSkin& skin) {
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  const ImVec2 max_size(display.x > 0.0f ? display.x * 0.8f : FLT_MAX,
                        display.y > 0.0f ? display.y * 0.8f : FLT_MAX);
  g_row_snap.line_h   = skin.snap_step;
  g_row_snap.chrome_h = skin.snap_base;
  g_row_snap.min_h    = skin.min_h;
  g_row_snap.max_h    = max_size.y;
  // 🔴 L'arrondi par rangées ne s'applique QUE pendant un redimensionnement. En
  // permanence, il corrigeait la hauteur à chaque frame à partir de métriques
  // mesurées à la frame précédente — d'où le frémissement du bas de la fenêtre —
  // et surtout il la redimensionnerait à chaque changement d'onglet, maintenant
  // que la taille de police est propre à chaque canal. Le geste, lui, produit déjà
  // une hauteur conforme : c'est là que la règle doit vivre, et nulle part ailleurs.
  const bool snap = (skin.snap_step > 1.0f) && ro::RoChatWindowIsResizing();
  ImGui::SetNextWindowSizeConstraints(ImVec2(skin.min_w, skin.min_h), max_size,
                                      snap ? SnapHeightToRows : nullptr, &g_row_snap);
}

void ChatWindow::DrawDockedWindow() {
  // Le skin est calculé AVANT Begin, donc sur le canal actif de la frame
  // précédente. Un changement d'onglet se voit à la frame suivante : sans
  // importance tant que les réglages sont globaux, à revoir quand ils seront par
  // canal (cf. le TODO sur les réglages par onglet).
  const Channel* previous =
      (active_channel_ >= 0 && active_channel_ < static_cast<int>(channels_.size()))
          ? &channels_[active_channel_]
          : nullptr;
  ro::RoChatSkin skin = MakeSkin(previous);
  // Bornes du redimensionnement par les bords : jamais moins de 400×200, jamais
  // plus de 80 % de l'écran.
  skin.min_w = 400.0f;
  skin.min_h = 200.0f;

  ImGui::SetNextWindowSize(ImVec2(620.0f, 220.0f), ImGuiCond_FirstUseEver);
  ApplySizeConstraints(skin);
  if (ro::BeginRoChatWindow("###bourgeon_chat", skin)) {
    DrawTabStrip();

    if (show_search_) {
      // 🔴 Le champ ET le bouton sont des bitmaps CLAIRS, alors que le cadre du
      // chat pousse un texte clair pour son fond sombre : clair sur clair, on ne
      // lit rien. Le texte repasse en sombre le temps de ces deux widgets — et
      // seulement eux : le libellé de la case, lui, est sur le fond sombre.
      ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
      ImGui::SetNextItemWidth(180.0f);
      ImGui::InputTextWithHint("##chat_search", i18n::Tr("Rechercher…"), search_,
                               sizeof(search_));
      ImGui::SameLine();
      if (ro::RoSmallButton(i18n::Tr("Effacer"))) search_[0] = '\0';
      ImGui::PopStyleColor();
      ImGui::SameLine();
      ro::RoCheckbox(i18n::Tr("Sélection###chatwnd_selmode"), &select_mode_);
      if (ImGui::IsItemHovered()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
        ImGui::SetTooltip(
            i18n::Tr("Change le log en texte sélectionnable : glisser pour sélectionner,\n"
            "Ctrl+A tout prendre, Ctrl+C copier. Les couleurs et les icônes\n"
            "disparaissent le temps de la sélection — c'est du texte nu."));
        ImGui::PopStyleColor();
      }
    }

    // La zone de log prend tout ce qui reste, moins la ligne de saisie.
    float log_h = ImGui::GetContentRegionAvail().y;
    if (InputRowVisible())
      log_h -= ImGui::GetFrameHeightWithSpacing();
    if (log_h < LogFontSize(previous)) log_h = LogFontSize(previous);  // une ligne de LOG

    // Métriques pour la contrainte de taille de la PROCHAINE frame. Le « chrome »
    // n'est pas seulement ce qui entoure la zone de log : dedans, la marge de
    // l'enfant et le débord du dernier glyphe ne sont pas non plus du texte. Les
    // oublier, c'était contraindre la fenêtre sur une grille décalée — donc
    // continuer à couper une ligne alors même que le pas était juste.
    // Relu APRÈS la bande d'onglets : un clic vient peut-être de changer d'onglet.
    Channel* channel =
        (active_channel_ >= 0 && active_channel_ < static_cast<int>(channels_.size()))
            ? &channels_[active_channel_]
            : nullptr;
    if (channel != nullptr) {
      // AVANT les métriques : elles doivent décrire la taille qu'on va dessiner,
      // pas celle d'avant le cran de molette.
      HandleFontZoom(*channel);
      channel->line_h   = LineHeight(channel);
      channel->chrome_h = ImGui::GetWindowSize().y - log_h +
                          2.0f * ImGui::GetStyle().WindowPadding.y + LineOverhang(channel);
      DrawChannel(*channel, log_h);
    }
    if (InputRowVisible()) DrawInputRow();
    DrawLogOptionsPopup();
  }
  ro::EndRoChatWindow();
}

int ChatWindow::GroupActiveIndex(uint32_t group) const {
  int first = -1;
  const auto it = group_active_.find(group);
  const uint32_t want = (it != group_active_.end()) ? it->second : 0;
  for (size_t i = 0; i < channels_.size(); ++i) {
    if (channels_[i].group != group) continue;
    if (first < 0) first = static_cast<int>(i);
    if (want != 0 && channels_[i].id == want) return static_cast<int>(i);
  }
  // Entrée périmée (l'onglet est parti ailleurs, ou fermé) : le premier reprend.
  return first;
}

// ── Une fenêtre FLOTTANTE, et ses onglets ────────────────────────────────────
// Elle remplace les deux fonctions d'avant — une pour les canaux arrachés, une
// pour les conversations 1:1 — parce qu'elles ne différaient plus que par deux
// choses : le libellé de l'onglet et la présence d'une saisie. Or depuis qu'une
// fenêtre peut porter PLUSIEURS canaux, les deux peuvent s'y côtoyer, et deux
// fonctions séparées n'auraient plus su laquelle dessiner.
//
// 🔴 Pas de ligne de saisie GÉNÉRALE ici, jamais : il n'y a qu'un texte en cours
// de frappe et il appartient au chat principal. Une conversation, elle, garde la
// sienne — destinataire figé, tampon à elle — et c'est justement ce qui la
// distingue d'un onglet. Elle n'apparaît donc que quand l'onglet ACTIF en est une.
void ChatWindow::DrawGroupWindow(uint32_t group) {
  const int active = GroupActiveIndex(group);
  if (active < 0) return;  // fenêtre vide : elle n'existe plus
  Channel& channel = channels_[active];

  ro::RoChatSkin skin = MakeSkin(&channel);
  // Bornes du client pour ses flottantes : largeur 280..512, hauteur 74..384. On
  // reprend le plancher, pas le plafond — le nôtre est déjà relatif à l'écran.
  skin.min_w = 280.0f;
  skin.min_h = 120.0f;

  // 🔴 L'identifiant vient du GROUPE. Le titre ne sert qu'au débogage (le skin
  // pose `NoTitleBar`), mais ce qui suit `###` DOIT être stable : le canal qui a
  // fondé la fenêtre peut la quitter ou se fermer, et elle perdrait alors position
  // et taille si elle portait son nom.
  char window_id[192];
  std::snprintf(window_id, sizeof(window_id), i18n::Tr("%s###bourgeon_chat_grp_%u"),
                channel.whisper_with.empty() ? channel.name.c_str()
                                             : channel.whisper_with.c_str(),
                group);

  // Cascade de 17 px comme le client, pour que deux fenêtres ouvertes coup sur
  // coup ne se recouvrent pas exactement.
  const float step = 17.0f * static_cast<float>(group % 8);
  ImGui::SetNextWindowSize(ImVec2(320.0f, 190.0f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(180.0f + step, 140.0f + step), ImGuiCond_FirstUseEver);
  // La fenêtre qu'on vient d'arracher naît SOUS le curseur, là où le joueur l'a
  // lâchée. `FirstUseEver` ne suffirait pas : ImGui se souvient d'une position
  // précédente pour cet identifiant, et la fenêtre semblerait sauter ailleurs.
  if (pending_pos_id_ == group) {
    ImGui::SetNextWindowPos(pending_pos_, ImGuiCond_Always);
    pending_pos_id_ = 0;
  }
  ApplySizeConstraints(skin);
  if (ro::BeginRoChatWindow(window_id, skin)) {
    DrawGroupStrip(group);
    // La saisie n'occupe une rangée que si l'onglet actif est une conversation.
    const bool has_input = !channel.whisper_with.empty();
    const float input_h  = has_input ? ImGui::GetFrameHeightWithSpacing() : 0.0f;
    float log_h = ImGui::GetContentRegionAvail().y - input_h;
    if (log_h < LogFontSize(&channel)) log_h = LogFontSize(&channel);
    HandleFontZoom(channel);  // cf. la fenêtre principale : avant les métriques
    channel.line_h   = LineHeight(&channel);
    channel.chrome_h = ImGui::GetWindowSize().y - log_h +
                       2.0f * ImGui::GetStyle().WindowPadding.y + LineOverhang(&channel);
    DrawChannel(channel, log_h);
    if (has_input) DrawWhisperInput(active);
    DrawLogOptionsPopup();
  }
  ro::EndRoChatWindow();
}

// La bande d'onglets d'une fenêtre flottante. Mêmes mesures que celle de la
// principale (`DrawTabStrip`) : c'est le MÊME objet, un onglet, qu'il soit resté
// dans la bande ou parti avec sa fenêtre.
//
// ⚠ Le vide à droite reste VIDE, sans widget : la fenêtre n'ayant pas de barre de
// titre, c'est par là qu'ImGui la déplace. Un bouton invisible qui couvrirait la
// bande entière lui retirerait sa poignée. Seule exception, tolérée parce qu'elle
// ne mord qu'un carré au coin : la croix de fermeture d'une conversation 1:1.
void ChatWindow::DrawGroupStrip(uint32_t group) {
  const float h = ImGui::GetFontSize() + 6.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin  = ImGui::GetCursorScreenPos();
  const float  strip_w = ImGui::GetContentRegionAvail().x;
  const ImU32 tab_idle   = ro::ImU32FromPicker(tab_rgba_);
  const ImU32 tab_active = Lighten(tab_idle, 58);
  const ImU32 tab_edge   = IM_COL32(0x30, 0x30, 0x30, 200);
  const int   active     = GroupActiveIndex(group);

  float slot_x0[kMaxChannels] = {};
  float slot_x1[kMaxChannels] = {};
  int   slot_n = 0;
  float x = 0.0f;

  // ── La croix de fermeture, en haut à DROITE ─────────────────────────────────
  // 🔴 Elle n'apparaît que pour une conversation 1:1. Une fenêtre flottante n'a
  // PAS de barre de titre — le skin chat pose NoTitleBar, c'est ce qui lui donne
  // son allure de chatbox du client — et une conversation privée n'avait donc
  // aucun moyen VISIBLE de se fermer : rien qu'un clic MOLETTE sur son onglet,
  // geste que seule annonce une infobulle qu'il faut déjà survoler l'onglet pour
  // lire. Un onglet de CANAL, lui, n'en porte pas : le fermer retire un canal du
  // chat, ce qui n'a rien d'un geste de fenêtre, et reste au clic molette comme au
  // menu contextuel de l'onglet.
  //
  // Elle ferme l'onglet ACTIF, celui qu'on a sous les yeux — la seule lecture sans
  // ambiguïté quand la fenêtre en porte plusieurs. Elle passe par
  // `close_channel_id_`, le même chemin différé que le clic molette : ses gardes
  // (jamais le dernier canal, jamais le dernier onglet de la fenêtre principale)
  // valent donc ici sans être réécrites.
  //
  // 🔴 SOUMISE AVANT les onglets, PEINTE APRÈS — et les deux moitiés comptent, car
  // une bande bien remplie fait passer un onglet dessous. Le survol va au PREMIER
  // élément soumis (`ItemHoverable` refuse tout candidat suivant tant que
  // `g.HoveredId` est pris) ; le dessin, lui, va au dernier tracé. Les inverser,
  // c'était une croix visible mais morte, ou vivante mais recouverte.
  const bool close_shown = (active >= 0 && !channels_[active].whisper_with.empty());
  const ImVec2 close_p0(origin.x + strip_w - h, origin.y);
  const ImVec2 close_p1(close_p0.x + h, close_p0.y + h);
  bool close_hovered = false;
  if (close_shown) {
    ImGui::SetCursorScreenPos(close_p0);
    char close_id[40];
    std::snprintf(close_id, sizeof(close_id), "##gclose%u", group);
    ImGui::InvisibleButton(close_id, ImVec2(h, h));
    close_hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      confirm_close_id_   = channels_[active].id;
      confirm_close_open_ = true;
    }
  }

  for (size_t i = 0; i < channels_.size(); ++i) {
    Channel& channel = channels_[i];
    if (channel.group != group) continue;
    // Une conversation s'annonce par le nom de son correspondant : c'est ce que le
    // joueur cherche des yeux, pas le nom de canal qu'on lui a donné à la création.
    const char* label = channel.whisper_with.empty() ? channel.name.c_str()
                                                     : channel.whisper_with.c_str();
    const float w = ImGui::CalcTextSize(label).x + 14.0f;

    ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y));
    char id[48];
    std::snprintf(id, sizeof(id), "##gtab%u_%d", group, static_cast<int>(i));
    ImGui::InvisibleButton(id, ImVec2(w, h),
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
      group_active_[group] = channel.id;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      logopt_channel_ = static_cast<int>(i);
      ImGui::OpenPopup("##chat_logopt_popup");
    }
    // Clic MOLETTE = fermer, le geste des onglets partout ailleurs. Il remplace la
    // croix que portait l'en-tête d'une conversation : avec plusieurs onglets dans
    // la fenêtre, une croix unique ne saurait plus lequel elle ferme.
    //
    // 🔴 Il DEMANDE la fermeture, il ne la fait plus : c'est le geste qui part le
    // plus facilement tout seul — la molette sert aussi à faire défiler le log
    // juste en dessous, et un cran de trop haut visait un onglet.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle) && channels_.size() > 1) {
      confirm_close_id_   = channel.id;
      confirm_close_open_ = true;
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f))
      drag_tab_ = static_cast<int>(i);
    if (hovered && drag_tab_ < 0) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
      if (channel.whisper_with.empty())
        ImGui::SetTooltip(i18n::Tr("Clic droit : options du log de « %s »\n"
                          "Glisser : vers une autre fenêtre, ou dehors\n"
                          "Clic molette : fermer"),
                          label);
      else if (channel.whisper_guild.empty())
        ImGui::SetTooltip(i18n::Tr("Conversation privée avec %s.\n"
                          "Glisser : vers une autre fenêtre, ou dehors\n"
                          "Clic molette : fermer"),
                          label);
      else
        ImGui::SetTooltip(i18n::Tr("Conversation privée avec %s [%s].\n"
                          "Glisser : vers une autre fenêtre, ou dehors\n"
                          "Clic molette : fermer"),
                          label, channel.whisper_guild.c_str());
      ImGui::PopStyleColor();
    }

    const ImVec2 p0(origin.x + x, origin.y);
    const ImVec2 p1(p0.x + w, p0.y + h);
    if (slot_n < kMaxChannels) {
      slot_x0[slot_n] = p0.x;
      slot_x1[slot_n] = p1.x;
      ++slot_n;
    }
    dl->AddRectFilled(p0, p1, (static_cast<int>(i) == active)
                                  ? tab_active
                                  : (hovered ? Lighten(tab_idle, 24) : tab_idle));
    dl->AddRect(p0, p1, tab_edge);
    dl->AddText(ImVec2(p0.x + 7.0f, p0.y + 3.0f), kTabTextCol, label);
    x += w + 2.0f;
  }

  // La croix, PAR-DESSUS les onglets (cf. le commentaire de sa soumission).
  if (close_shown) {
    // Survol en rouge CLAIR, pas sombre : la croix est tracée en sombre
    // (kTabTextCol, comme le libellé des onglets) et disparaîtrait sur un fond foncé.
    dl->AddRectFilled(close_p0, close_p1,
                      close_hovered ? IM_COL32(0xE0, 0x92, 0x92, 255) : tab_idle);
    dl->AddRect(close_p0, close_p1, tab_edge);
    // Croix TRACÉE, pas le glyphe « X » : il n'est pas centré dans sa cellule et se
    // lirait de travers dans un carré de cette taille.
    const float pad = 5.0f;
    dl->AddLine(ImVec2(close_p0.x + pad, close_p0.y + pad),
                ImVec2(close_p1.x - pad, close_p1.y - pad), kTabTextCol, 1.6f);
    dl->AddLine(ImVec2(close_p1.x - pad, close_p0.y + pad),
                ImVec2(close_p0.x + pad, close_p1.y - pad), kTabTextCol, 1.6f);
    if (close_hovered && drag_tab_ < 0) {
      ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
      ImGui::SetTooltip(i18n::Tr("Ferme l'onglet affiché (%s).\nL'historique est "
                        "conservé : rouvrir la conversation le retrouve."),
                        channels_[active].whisper_with.c_str());
      ImGui::PopStyleColor();
    }
  }

  // La bande devient une CIBLE DE DÉPÔT pour la frame.
  StripRect rect;
  rect.group = group;
  rect.min   = origin;
  rect.max   = ImVec2(origin.x + strip_w, origin.y + h);
  strips_.push_back(rect);

  // Le trait d'insertion, peint par la bande SURVOLÉE — elle seule connaît ses
  // onglets. Le lâcher, lui, est traité après toutes les fenêtres : à ce
  // moment-là, on ne saurait plus dans quelle bande on est.
  if (drag_tab_ >= 0) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x >= rect.min.x && mouse.x <= rect.max.x && mouse.y >= rect.min.y &&
        mouse.y <= rect.max.y) {
      int slot = slot_n;
      for (int k = 0; k < slot_n; ++k)
        if (mouse.x < (slot_x0[k] + slot_x1[k]) * 0.5f) {
          slot = k;
          break;
        }
      drop_valid_ = true;
      drop_group_ = group;
      drop_slot_  = slot;
      const float caret_x = (slot < slot_n)
                                ? slot_x0[slot] - 1.0f
                                : (slot_n > 0 ? slot_x1[slot_n - 1] + 1.0f : origin.x);
      dl->AddLine(ImVec2(caret_x, origin.y), ImVec2(caret_x, origin.y + h), kLinkCol,
                  2.0f);
    }
  }

  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h + 2.0f));
}

// ── ⛔ DrawDetachedWindow / DrawDetachedHeader / DrawWhisperWindow /
// DrawWhisperHeader ONT DISPARU ICI. Elles dessinaient « une fenêtre = un canal »,
// avec un en-tête maison pour déplacer la fenêtre et une croix pour la fermer.
// `DrawGroupWindow` + `DrawGroupStrip` les remplacent toutes les quatre : une
// fenêtre porte désormais N canaux, donc son en-tête EST une bande d'onglets, et
// c'est l'onglet qui se ferme (clic molette) ou se déplace — pas la fenêtre.
//
// Le déplacement de la fenêtre revient à ImGui, qui le fait très bien par le vide
// de la bande : nos en-têtes ne le prenaient à leur charge que parce qu'ils
// couvraient toute la largeur avec un bouton invisible.

// La saisie propre à une conversation. Volontairement nue : pas de box
// destinataire (elle est FIGÉE, c'est tout l'intérêt), pas de sélecteur de mode
// (on chuchote, point), pas d'historique de noms.
void ChatWindow::DrawWhisperInput(int index) {
  Channel& channel = channels_[index];
  // 🔴 AVANT la copie dans le tampon, jamais après : c'est tout l'intérêt de
  // l'attente. Un emoji piqué dans la palette a été mis de côté pendant la frame
  // précédente (la grille se dessine plus bas, une fois `buffer` déjà rempli) ;
  // c'est ici, et seulement ici, qu'il peut rejoindre la saisie sans être écrasé
  // par la réécriture de fin de fonction.
  if (!channel.whisper_pending_insert.empty()) {
    channel.whisper_input += channel.whisper_pending_insert;
    channel.whisper_pending_insert.clear();
  }
  char buffer[256];
  CopyBounded(buffer, sizeof(buffer), channel.whisper_input.c_str());

  // ── Le sélecteur d'emotes, ici aussi ────────────────────────────────────────
  // Même bouton que la barre principale, même grille. Une conversation est
  // justement l'endroit où l'on répond d'un sourire plutôt que d'une phrase, et
  // aller le chercher dans le chat principal l'enverrait au mauvais destinataire.
  //
  // 🔴 Le popup porte le MÊME nom que celui de la barre principale, et c'est
  // voulu : ImGui hache l'identifiant avec la pile de la fenêtre courante, donc
  // chaque conversation a le sien. Ce qui compte, c'est que le bouton et
  // `DrawEmotePicker` soient dans la MÊME fenêtre — sinon deux conversations
  // ouvertes se partageraient une grille, et la mauvaise recevrait l'emote.
  const float  pick_side = ImGui::GetFrameHeight();
  const ImVec2 pick_pos  = ImGui::GetCursorScreenPos();
  ImGui::PushID(static_cast<int>(channel.id));
  if (ImGui::Button("##whisper_emote_btn", ImVec2(pick_side, pick_side)))
    ImGui::OpenPopup("##chat_emote_grid");
  const bool pick_hovered = ImGui::IsItemHovered();
  {
    const ImVec2 in_min(pick_pos.x + 2.0f, pick_pos.y + 2.0f);
    const ImVec2 in_max(pick_pos.x + pick_side - 2.0f, pick_pos.y + pick_side - 2.0f);
    if (!ro::emote::Draw(ImGui::GetWindowDrawList(), kEmotePickerIcon, in_min, in_max,
                         static_cast<float>(ImGui::GetTime()), true)) {
      ImGui::GetWindowDrawList()->AddText(in_min, kDarkText, ":)");
    }
  }
  if (pick_hovered) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
    ImGui::SetTooltip(i18n::Tr("Emotes et emoji.\nUne emote part à %s aussitôt, "
                      "seule ; un emoji s'ajoute à ta réponse."),
                      channel.whisper_with.c_str());
    ImGui::PopStyleColor();
  }
  DrawEmotePicker(pick_pos, ImVec2(pick_pos.x + pick_side, pick_pos.y + pick_side),
                  index);
  ImGui::PopID();
  ImGui::SameLine();

  // 🔴 Texte de saisie en SOMBRE, comme la barre principale (qui pousse la même
  // couleur autour de son champ). Sans ça, le texte hérite du blanc que
  // `BeginRoChatWindow` pose pour les lignes de LOG — lisible sur le corps sombre
  // de la chatbox, mais le champ, lui, a un fond CLAIR (ImGuiCol_FrameBg 0xCECECE
  // poussé par le même skin). On tapait donc en clair sur clair.
  // L'invite (`InputTextWithHint`) suit : elle se peint en `ImGuiCol_TextDisabled`,
  // calibré pour un fond sombre — sur le gris du champ elle s'efface presque.
  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
  ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(0x6A, 0x6A, 0x72, 255));
  ImGui::SetNextItemWidth(-FLT_MIN);
  char field_id[64];
  std::snprintf(field_id, sizeof(field_id), "##whisper_input_%u", channel.id);
  char hint[96];
  std::snprintf(hint, sizeof(hint), i18n::Tr("Répondre à %s"), channel.whisper_with.c_str());
  // Le focus se rend APRÈS un envoi : sans ça, la conversation se poursuit au
  // clavier une seule fois, puis la frappe repart dans le jeu.
  if (channel.whisper_focus) {
    ImGui::SetKeyboardFocusHere();
    channel.whisper_focus = false;
  }
  if (ImGui::InputTextWithHint(field_id, hint, buffer, sizeof(buffer),
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
    channel.whisper_input = buffer;
    QueueWhisperSend(channel);
    channel.whisper_focus = true;
  } else {
    channel.whisper_input = buffer;
  }
  ImGui::PopStyleColor(2);
}

// Arme l'envoi, joué par FlushPending hors frame. Le texte part TEL QUEL : la
// résolution des liens d'objets appartient à la saisie principale, qui seule
// reçoit les dépôts du Maj+clic — la refaire ici viderait `item_links_` sous les
// pieds de l'autre.
//
// 🔴 RIEN N'EST POUSSÉ DANS L'HISTORIQUE DE SAISIE. Il est partagé avec la
// chatbox : une flèche du haut y ressortirait un message privé, dans une ligne
// qui part en PUBLIC au prochain Entrée. Une confidence divulguée par un
// raccourci clavier vaut bien mieux que le confort de la rappeler.
void ChatWindow::QueueWhisperSend(Channel& channel) {
  if (channel.whisper_input.empty()) return;
  // 🔴 DEUX fonctions différentes, et pas par inadvertance : le TEXTE peut
  // basculer en UTF-8 s'il porte un emoji, le NOM du correspondant jamais — le
  // serveur le cherche octet par octet dans sa base, en 1252.
  pending_text_    = ro::Utf8ToWireText(channel.whisper_input.c_str());
  pending_whisper_ = ro::Utf8ToWire(channel.whisper_with.c_str());
  has_pending_     = true;
  channel.whisper_stamp = GetTickCount();
  channel.whisper_input.clear();
}

// Complète le titre des conversations avec la guilde du correspondant, lue dans
// le dictionnaire de noms du client — le même que celui des noms flottants
// au-dessus des personnages (docs/entity_nameplate_re.md).
//
// 🔴 HORS FRAME ImGui. Une entrée inconnue ne renvoie pas simplement « rien » :
// elle met le GID en file de requête et le client ÉMET un paquet. Ce n'est pas
// une commande native qui relance le rendu, mais un effet de bord réseau n'a
// rien à faire au milieu d'un dessin.
//
// ⚠ Peut ne JAMAIS aboutir, et c'est normal : rAthena ne répond à une requête de
// nom que pour une unité de la MÊME CARTE (`clif_parse_GetCharNameRequest` ->
// `map_id2bl`). Un correspondant à l'autre bout du monde restera donc sans
// guilde ; le titre s'en passe plutôt que d'afficher un vide inquiétant. C'est
// aussi pour ça qu'on relance : il peut arriver sur notre carte en cours de
// conversation, et l'information apparaît alors toute seule.
void ChatWindow::ResolveWhisperGuilds() {
  bool any = false;
  for (const Channel& channel : channels_)
    if (!channel.whisper_with.empty() && channel.whisper_aid != 0) any = true;
  if (!any) return;

  // Une tentative par seconde suffit largement : la réponse du serveur met un
  // aller-retour, et l'AID d'une conversation ne change jamais.
  const uint32_t now = GetTickCount();
  if (whisper_guild_stamp_ != 0 && (now - whisper_guild_stamp_) < 1000) return;
  whisper_guild_stamp_ = now;

  // La lecture native se fait sur des POD dans une fonction à part : `__try` et
  // les objets à destructeur ne cohabitent pas (C2712), et une std::string dans
  // cette boucle suffirait à refuser la compilation.
  GuildProbe probe[kMaxWhisperWindows];
  int count = 0;
  for (const Channel& channel : channels_) {
    if (channel.whisper_with.empty() || channel.whisper_aid == 0) continue;
    if (count >= static_cast<int>(kMaxWhisperWindows)) break;
    probe[count].aid      = channel.whisper_aid;
    probe[count].guild[0] = '\0';
    ++count;
  }
  if (!ProbeGuildsFromNameDict(probe, count)) return;

  for (int i = 0; i < count; ++i) {
    // 🔴 On n'EFFACE jamais une guilde déjà connue : l'entrée peut redevenir vide
    // (le correspondant quitte notre carte, le dictionnaire se recycle) et le
    // titre se mettrait à clignoter au rythme du dictionnaire.
    if (probe[i].guild[0] == '\0') continue;
    for (Channel& channel : channels_)
      if (channel.whisper_aid == probe[i].aid)
        channel.whisper_guild = ro::WireToUtf8(probe[i].guild);
  }
}

// Bande d'onglets façon client : petits rectangles gris, texte sombre, l'actif
// éclairci. À droite, les boutons bitmap du chat natif. Le vide entre les deux
// reste VIDE exprès : c'est par là qu'on déplace la fenêtre (elle n'a pas de
// barre de titre), et un widget invisible mangerait le glissement.
float ChatWindow::DrawTabStrip() {
  const float h = ImGui::GetFontSize() + 6.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  // Mesurée AVANT de déplacer le curseur : après le premier onglet, l'espace
  // « restant » ne serait plus celui de la bande mais celui d'après.
  const float strip_w = ImGui::GetContentRegionAvail().x;
  const ImU32 tab_idle = ro::ImU32FromPicker(tab_rgba_);
  const ImU32 tab_active = Lighten(tab_idle, 58);
  const ImU32 tab_edge = IM_COL32(0x30, 0x30, 0x30, 200);

  // Bords de chaque onglet DESSINÉ, relevés au passage : c'est ce qui dit, au
  // moment du dépôt, entre quels deux voisins le curseur se trouve. Un tableau
  // fixe plutôt qu'un vecteur — la bande ne peut pas porter plus de canaux que le
  // plafond, et c'est une mesure de frame, pas de l'état. Le RANG suffit : c'est
  // dans cette numérotation-là que `MoveChannelToGroup` prend son argument.
  float slot_x0[kMaxChannels] = {};
  float slot_x1[kMaxChannels] = {};
  int   slot_n = 0;

  float x = 0.0f;
  for (size_t i = 0; i < channels_.size(); ++i) {
    const Channel& channel = channels_[i];
    if (channel.detached) continue;  // arraché : il a sa propre fenêtre
    const float text_w = ImGui::CalcTextSize(channel.name.c_str()).x;
    const float w = text_w + 14.0f;

    ImGui::SetCursorScreenPos(ImVec2(origin.x + x, origin.y));
    char id[32];
    std::snprintf(id, sizeof(id), "##tab%d", static_cast<int>(i));
    // Un bouton invisible plutôt qu'un test de survol : sans lui, glisser un
    // onglet déplacerait la fenêtre au lieu de le sélectionner.
    ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
      active_channel_ = static_cast<int>(i);
    // Clic droit = les options de log DE CET onglet. C'est leur place : le filtre
    // appartient au canal, pas à la fenêtre. Le popup s'ouvre sous le curseur, donc
    // à l'onglet désigné — et il configure celui-là, pas l'onglet actif.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      logopt_channel_ = static_cast<int>(i);
      ImGui::OpenPopup("##chat_logopt_popup");
    }
    // Arrachage : le seuil de 6 px distingue le GLISSEMENT du simple clic, qui
    // sélectionne. Sans lui, sélectionner un onglet d'une main un peu tremblante
    // le détacherait.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6.0f))
      drag_tab_ = static_cast<int>(i);
    if (hovered && drag_tab_ < 0) {
      // Un menu contextuel sans indice ne se découvre pas. Le fond de l'infobulle
      // est clair : le texte doit repasser en sombre pour rester lisible.
      ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
      ImGui::SetTooltip(
          i18n::Tr("Clic droit : options du log de « %s »\n"
          "Glisser : réordonner, ou vers une autre fenêtre, ou dehors"),
          channel.name.c_str());
      ImGui::PopStyleColor();
    }

    const bool active = (active_channel_ == static_cast<int>(i));
    const ImVec2 p0(origin.x + x, origin.y);
    const ImVec2 p1(p0.x + w, p0.y + h);
    if (slot_n < kMaxChannels) {
      slot_x0[slot_n] = p0.x;
      slot_x1[slot_n] = p1.x;
      ++slot_n;
    }
    dl->AddRectFilled(p0, p1, active ? tab_active
                                     : (hovered ? Lighten(tab_idle, 24) : tab_idle));
    dl->AddRect(p0, p1, tab_edge);
    dl->AddText(ImVec2(p0.x + 7.0f, p0.y + 3.0f), kTabTextCol, channel.name.c_str());
    x += w + 2.0f;
  }

  // La bande, mémorisée pour la frame — et enregistrée comme CIBLE DE DÉPÔT au
  // même titre que celle de n'importe quelle flottante. `strip_*` reste pour le
  // recollage historique d'une fenêtre entière.
  strip_min_   = origin;
  strip_max_   = ImVec2(origin.x + strip_w, origin.y + h);

  StripRect rect;
  rect.group = 0;
  rect.min   = strip_min_;
  rect.max   = strip_max_;
  strips_.push_back(rect);

  // ── Où l'onglet tombera-t-il, s'il tombe ici ? ──────────────────────────────
  // Le rang visé se lit au MILIEU de chaque onglet, pas à son bord : c'est le
  // point de bascule naturel — dépasser la moitié d'un voisin, c'est passer
  // devant lui. Comparer aux bords ferait une zone morte large d'un onglet entre
  // deux positions, où le trait ne bougerait pas.
  //
  // 🔴 Le rang est compté dans la bande TELLE QU'ELLE EST DESSINÉE, celui qu'on
  // déplace COMPRIS. C'est ce que le joueur voit, et c'est donc la seule
  // numérotation qui puisse correspondre à son geste ; `MoveChannelToGroup` fait
  // la conversion. Le LÂCHER, lui, est traité une fois toutes les fenêtres
  // dessinées (OnRenderUI) : à ce moment-là seulement on sait laquelle était sous
  // le curseur, et plus personne ne parcourt `channels_` par indice.
  if (drag_tab_ >= 0) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x >= strip_min_.x && mouse.x <= strip_max_.x &&
        mouse.y >= strip_min_.y && mouse.y <= strip_max_.y) {
      int slot = slot_n;  // au-delà du dernier milieu : tout au bout
      for (int k = 0; k < slot_n; ++k) {
        if (mouse.x < (slot_x0[k] + slot_x1[k]) * 0.5f) {
          slot = k;
          break;
        }
      }
      drop_valid_ = true;
      drop_group_ = 0;
      drop_slot_  = slot;
      // Le trait d'insertion, entre les deux voisins. Sans lui le joueur lâche à
      // l'aveugle et découvre l'ordre obtenu après coup.
      const float caret_x = (slot < slot_n)
                                ? slot_x0[slot] - 1.0f
                                : (slot_n > 0 ? slot_x1[slot_n - 1] + 1.0f : origin.x);
      dl->AddLine(ImVec2(caret_x, origin.y), ImVec2(caret_x, origin.y + h), kLinkCol,
                  2.0f);
    }
  }

  // Le fantôme suit le curseur, sur le calque de PREMIER PLAN : la fenêtre
  // détachée qu'on survole passerait sinon par-dessus.
  if (drag_tab_ >= 0 && drag_tab_ < static_cast<int>(channels_.size())) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const char*  label = channels_[drag_tab_].name.c_str();
    const float  gw    = ImGui::CalcTextSize(label).x + 14.0f;
    ImDrawList*  fg    = ImGui::GetForegroundDrawList();
    const ImVec2 g0(mouse.x - 20.0f, mouse.y - 6.0f);
    const ImVec2 g1(g0.x + gw, g0.y + h);
    fg->AddRectFilled(g0, g1, tab_active);
    fg->AddRect(g0, g1, tab_edge);
    fg->AddText(ImVec2(g0.x + 7.0f, g0.y + 3.0f), kTabTextCol, label);
  }

  // Boutons du client, alignés à droite. On ne dessine QUE ceux qui agissent —
  // un bouton natif décoratif qui ne fait rien est un piège pour le joueur.
  struct StripButton {
    const char* bitmap;
    const char* id;
    const char* tip;
  };
  // Le bouton « options du log » a été RETIRÉ : ces options sont une propriété du
  // canal, et un bouton unique dans la bande ne dit pas lequel il configure. Elles
  // vivent maintenant au clic droit sur l'onglet concerné, où l'ambiguïté n'existe
  // pas. Il ne reste donc que la recherche, qui, elle, porte bien sur la fenêtre.
  const StripButton buttons[] = {
      // sys_base = le petit rond des boutons de filtre du chat natif.
      // Volontairement NEUTRE : wnd_mini (le « - ») dirait « fermer l'onglet »,
      // ce que ce bouton-ci ne fait pas.
      {"basic_interface\\sys_base", "##chat_search_btn", "Rechercher / copier"},
  };
  float bx = strip_w;
  for (int i = static_cast<int>(_countof(buttons)) - 1; i >= 0; --i) {
    // Deux familles de suffixes chez le client : les UIBitmapButton du chat sont
    // en `_a`/`_b` (normal/survol), les boutons système en `_off`/`_on`.
    const bool sys = std::strstr(buttons[i].bitmap, "sys_") != nullptr;
    char path[96];
    std::snprintf(path, sizeof(path), sys ? "%s_off.bmp" : "%s_a.bmp",
                  buttons[i].bitmap);
    ro::GameTexture normal = ChatBitmap(path);
    std::snprintf(path, sizeof(path), sys ? "%s_on.bmp" : "%s_b.bmp",
                  buttons[i].bitmap);
    ro::GameTexture over = ChatBitmap(path);
    const float bw = (normal.w > 0) ? static_cast<float>(normal.w) : 12.0f;
    const float bh = (normal.h > 0) ? static_cast<float>(normal.h) : 12.0f;
    bx -= bw + 3.0f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + bx, origin.y + (h - bh) * 0.5f));
    ImGui::InvisibleButton(buttons[i].id, ImVec2(bw, bh));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
      ro::SetHoverCursor(2);  // curseur « main » RO
      // Le cadre du chat pousse un texte CLAIR : dans une infobulle au fond
      // clair, il faut le repasser en sombre, sinon elle est illisible.
      ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
      ImGui::SetTooltip("%s", buttons[i].tip);
      ImGui::PopStyleColor();
    }
    const ImVec2 p0 = ImGui::GetItemRectMin();
    const ImVec2 p1 = ImGui::GetItemRectMax();
    ro::GameTexture draw = (hovered && over.tex) ? over : normal;
    if (draw.tex != nullptr)
      dl->AddImage(TexId(draw.tex), p0, p1);
    else  // bitmap absent du GRF : un carré plutôt qu'un trou
      dl->AddRectFilled(p0, p1, Lighten(tab_idle, 30));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) show_search_ = !show_search_;
  }

  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + h + 2.0f));
  return h;
}

// Le pas vertical du log. Le `- 6` est le calage historique du skin : l'interligne
// réglable démarre volontairement plus serré que la hauteur de texte d'ImGui, qui
// est très aérée pour du chat. Le plancher évite qu'un interligne à 0 rende le pas
// dégénéré (et la division par lui, absurde).
// 🔴 Depuis la taille de RÉFÉRENCE, pas depuis `GetFontSize()` de la fenêtre
// courante. Le log se dessine dans une fenêtre ENFANT, qui n'hérite pas de
// l'échelle de sa parente : mesurer dehors et dessiner dedans donnait deux tailles
// différentes, donc une grille de rangées qui ne correspondait pas au texte.
// ── Apparence effective ──────────────────────────────────────────────────────
// Un canal sans style propre SUIT les réglages généraux — il ne les copie pas.
// La différence se voit le jour où le joueur change le réglage général : les
// onglets qui n'ont rien demandé doivent bouger avec lui.
int ChatWindow::EffFontPct(const Channel* channel) const {
  return (channel != nullptr && channel->style_own) ? channel->font_pct
                                                    : font_scale_pct_;
}
int ChatWindow::EffPadding(const Channel* channel) const {
  return (channel != nullptr && channel->style_own) ? channel->padding : padding_px_;
}
int ChatWindow::EffLineGap(const Channel* channel) const {
  return (channel != nullptr && channel->style_own) ? channel->line_gap
                                                    : line_gap_px_;
}
const float* ChatWindow::EffBody(const Channel* channel) const {
  return (channel != nullptr && channel->style_own) ? channel->body : body_rgba_;
}

float ChatWindow::LogFontSize(const Channel* channel) const {
  const float base = (base_font_size_ > 1.0f) ? base_font_size_ : ImGui::GetFontSize();
  return base * static_cast<float>(EffFontPct(channel)) / 100.0f;
}

float ChatWindow::LineHeight(const Channel* channel) const {
  const float h = LogFontSize(channel) + static_cast<float>(EffLineGap(channel)) - 6.0f;
  return (h < 4.0f) ? 4.0f : h;
}

// +1 px : le soulignement des liens est tracé sur la ligne de base + hauteur de
// texte, donc un pixel SOUS le glyphe.
float ChatWindow::LineOverhang(const Channel* channel) const {
  const float over = LogFontSize(channel) + 1.0f - LineHeight(channel);
  return (over > 0.0f) ? over : 0.0f;
}

// ── Ctrl + molette = zoom du texte, comme partout ailleurs ───────────────────
// Le geste que tout le monde connaît du navigateur, au même endroit : la molette
// seule fait défiler le log, Ctrl enfoncé change la taille du texte.
//
// Rien à disputer à ImGui : `UpdateMouseWheel` rend la main dès que Ctrl est tenu
// (et son vieux zoom de fenêtre dort, `io.FontAllowUserScaling` restant à false).
// Sous Ctrl la molette est donc LIBRE — le log ne défilera pas en même temps
// qu'il grossit. On la consomme quand même : d'autres surfaces lisent
// `io.MouseWheel` sans regarder qui est survolé.
//
// La cible est celle de `EffFontPct`, et c'est la seule qui ne mente pas : un
// onglet qui a ses réglages propres se zoome seul ; un onglet qui suit les
// réglages généraux DÉPLACE les réglages généraux — exactement ce que ferait le
// curseur du panneau, qui affichera d'ailleurs la valeur qu'on vient de poser.
void ChatWindow::HandleFontZoom(Channel& channel) {
  // `RootAndChildWindows` : le log est une fenêtre ENFANT, et c'est justement
  // au-dessus de lui qu'on fait le geste. Le hover est refusé tant qu'un popup
  // est ouvert par-dessus (menu d'onglet, options), ce qui est bien ce qu'on
  // veut — là, la molette appartient au menu.
  if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) return;

  ImGuiIO& io       = ImGui::GetIO();
  const float wheel = io.MouseWheel;
  if (io.KeyCtrl && wheel != 0.0f) {
    io.MouseWheel = 0.0f;
    const int step = (wheel > 0.0f) ? kFontZoomStep : -kFontZoomStep;
    int* target    = channel.style_own ? &channel.font_pct : &font_scale_pct_;
    const int before = *target;
    *target = ImClamp(before + step, kFontPctMin, kFontPctMax);
    // Posé même à la butée : c'est là qu'il sert le plus, pour dire que le texte
    // ne bougera plus.
    zoom_hint_until_ = ImGui::GetTime() + 1.2;
    if (*target != before) {
      InvalidateLineLayout();
      // L'onglet range ses réglages avec la disposition, les généraux avec ceux
      // de Bourgeon. `SaveSettings` a son anti-rebond (400 ms) : un cran de
      // molette n'écrit pas un fichier, et un geste continu n'en écrit qu'un.
      if (channel.style_own) layout_dirty_ = true;
      else if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
    }
  }

  // Le fond de l'infobulle est CLAIR, alors que le cadre du chat pousse un texte
  // clair pour son fond sombre : sans ce passage en sombre, le pourcentage se lit
  // à peine (même règle que l'infobulle des onglets).
  if (ImGui::GetTime() < zoom_hint_until_) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
    ImGui::SetTooltip("%d %%", EffFontPct(&channel));
    ImGui::PopStyleColor();
  }
}

// Les hauteurs de repli mémorisées valent pour UNE mise en page : changer la
// taille du texte ou l'interligne les périme toutes. Les lignes visibles se
// remesurent d'elles-mêmes à la frame suivante, mais celles qui sont hors écran
// seraient SAUTÉES sur leur ancienne hauteur (cf. `DrawLines`) — donc une hauteur
// totale fausse, et un défilement qui ne tombe plus en face du texte. Une passe
// sur le tampon, une seule fois par changement.
void ChatWindow::InvalidateLineLayout() {
  std::lock_guard<std::mutex> lock(lines_mutex_);
  for (Line& line : lines_) line.cached_wrap = -1.0f;
}

// Mode « sélection » : le log devient une zone de texte en LECTURE SEULE. C'est
// la seule façon d'avoir une vraie sélection à la souris et un Ctrl+C — notre
// rendu normal peint des glyphes dans un ImDrawList, il n'y a rien à sélectionner
// dedans. Le prix est assumé : plus de couleurs, plus d'icônes, du texte nu — ce
// qui est de toute façon ce qu'on veut coller ailleurs.
void ChatWindow::RefreshSelectBuffer(const Channel& channel) {
  std::lock_guard<std::mutex> lock(lines_mutex_);
  // Clé de fraîcheur : reconstruire à chaque frame coûterait une concaténation de
  // plusieurs dizaines de kilo-octets par frame, et le chat est précisément là où
  // ce genre de coût a déjà gelé le client. `ingest_kept_` ne recule jamais, donc
  // toute ligne nouvelle change la clé, y compris quand l'anneau est plein et que
  // la TAILLE, elle, ne bouge plus.
  // Le canal DESSINÉ, pas l'onglet actif : une fenêtre détachée n'est jamais
  // l'onglet actif, et deux fenêtres partagent ce même tampon.
  uint32_t key = ingest_kept_;
  key = key * 31u + channel.id;
  key = key * 31u + (timestamps_ ? 1u : 0u);
  // Le vidage de l'onglet EN FAIT PARTIE : il ne fait entrer aucune ligne, donc
  // `ingest_kept_` ne bouge pas et le tampon de sélection resterait celui d'avant
  // le geste — on copierait des lignes qu'on ne voit plus.
  key = key * 31u + static_cast<uint32_t>(channel.clear_seq);
  for (const char* p = search_; *p != '\0'; ++p)
    key = key * 31u + static_cast<unsigned char>(*p);
  if (key == select_key_) return;
  select_key_ = key;

  select_buf_.clear();
  for (const Line& line : lines_) {
    if (!ChannelAccepts(channel, line)) continue;
    if (search_[0] != '\0' && !ContainsNoCase(line.plain, search_) &&
        !ContainsNoCase(line.sender, search_))
      continue;
    if (timestamps_) {
      char stamp[16];
      std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", line.hour, line.minute,
                    line.second);
      select_buf_ += stamp;
    }
    select_buf_ += line.plain;
    select_buf_ += '\n';
  }
}

void ChatWindow::DrawChannel(const Channel& channel, float height) {
  if (select_mode_) {
    RefreshSelectBuffer(channel);
    // `&buf[0]` et non `.data()` : valable même sur une chaîne vide (l'accès à
    // l'index size() est garanti et rend le zéro terminal). ReadOnly ⇒ ImGui ne
    // touche jamais au tampon, la taille+1 ne sert qu'à lui donner la fin.
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
    // Ce champ affiche du LOG : il suit l'échelle du log, pas celle de l'habillage.
    // Restaurée juste après, sinon la ligne de saisie — dessinée ensuite dans la
    // même fenêtre — hériterait de la taille du chat.
    ImGui::SetWindowFontScale(static_cast<float>(EffFontPct(&channel)) / 100.0f);
    ImGui::InputTextMultiline("##chat_select", &select_buf_[0], select_buf_.size() + 1,
                              ImVec2(-FLT_MIN, height), ImGuiInputTextFlags_ReadOnly);
    ImGui::SetWindowFontScale(static_cast<float>(ui_scale_pct_) / 100.0f);
    ImGui::PopStyleColor();
    return;
  }

  ImGui::BeginChild("##chat_log", ImVec2(0.0f, height), false);
  DrawLines(channel);

  // 🔴 Le collage au bas se décide SANS variable d'état. `GetScrollMaxY()` porte
  // le contenu de la frame PRÉCÉDENTE : « ScrollY >= ScrollMaxY » veut donc dire
  // « j'étais en bas au début de cette frame », ce qui est exactement la question.
  // La version d'avant retenait la réponse dans un booléen mis à jour APRÈS le
  // SetScrollHereY — donc avec une frame de retard : le chat se recollait en bas à
  // chaque tentative de remonter, et il fallait insister pour s'en détacher.
  const float line_h = LineHeight(&channel);
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
    ImGui::SetScrollHereY(1.0f);
  } else if (line_h > 1.0f) {
    // Ailleurs qu'en bas : aligner le défilement sur une ligne entière. Toutes les
    // lignes VISUELLES font exactement `line_h` (le repli en produit plusieurs,
    // jamais des hauteurs différentes), donc un scroll multiple de line_h ne coupe
    // jamais rien — ni en haut, ni en bas, comme le natif.
    const float y = ImGui::GetScrollY();
    const float snapped = std::floor(y / line_h + 0.5f) * line_h;
    if (std::fabs(snapped - y) > 0.5f) ImGui::SetScrollY(snapped);
  }
  ImGui::EndChild();
}

// Word-wrap multi-couleur à la main (même moteur que le dialogue PNJ) : ImGui ne
// sait pas enchaîner plusieurs couleurs sur une ligne qui se replie.
//
// Le coût par frame est le vrai sujet ici — c'est le chat qui a déjà offert au
// projet une famille de freezes. Deux garde-fous : le parse est fait à
// l'ingestion, et la HAUTEUR de chaque ligne est mémorisée pour la largeur
// courante, ce qui permet de sauter d'un bloc toute ligne hors écran.
void ChatWindow::DrawLines(const Channel& channel) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImFont* font   = ImGui::GetFont();
  // 🔴 Taille EXPLICITE, de bout en bout : mesure (`CalcTextSizeA`) comme dessin
  // (`AddText` avec police et taille). C'est ce qui rend le log indépendant de
  // l'échelle de la fenêtre — celle-ci n'habille plus que les onglets, les boutons
  // et la ligne de saisie.
  const float fsize   = LogFontSize(&channel);
  const float line_h  = LineHeight(&channel);
  const float wrap    = ImGui::GetContentRegionAvail().x;
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float space_w = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, " ").x;

  // Bornes de la zone visible, en coordonnées locales de la liste.
  const float view_top    = ImGui::GetScrollY() - line_h;
  const float view_bottom = view_top + ImGui::GetWindowSize().y + 2.0f * line_h;

  const uint8_t layout_flags = static_cast<uint8_t>((timestamps_ ? 1 : 0) |
                                                    (item_icons_ ? 2 : 0) |
                                                    (diagnostic_ ? 4 : 0) |
                                                    // Une emote passe de « :nom: »
                                                    // à une image : la largeur
                                                    // change, donc le repli aussi.
                                                    (url_preview_ ? 8 : 0));
  const bool hovering_log = ImGui::IsWindowHovered();
  links::Target click_target;        // invalide = aucun lien cliqué cette frame
  bool click_shift  = false;         // Maj enfoncé au moment du clic
  bool menu_request = false;         // clic droit sur un lien : ouvrir le menu

  float x = 0.0f, y = 0.0f;
  std::unique_lock<std::mutex> lock(lines_mutex_);
  for (Line& line : lines_) {
    if (!ChannelAccepts(channel, line)) continue;
    if (search_[0] != '\0' && !ContainsNoCase(line.plain, search_) &&
        !ContainsNoCase(line.sender, search_))
      continue;

    // Hauteur connue et ligne hors écran : rien à peindre, rien à mesurer.
    if (line.cached_wrap == wrap && line.cached_flags == layout_flags &&
        (y + line.cached_height < view_top || y > view_bottom)) {
      y += line.cached_height;
      continue;
    }

    const float line_top = y;
    const ImU32 def_col  = LineColorToImU32(line.rgb);
    // 🔴 Une emote encore en cours de téléchargement occupe la place de « :nom: »,
    // et celle de l'IMAGE une fois arrivée. Mémoriser la hauteur maintenant la
    // figerait sur la mauvaise : la ligne resterait mal repliée jusqu'à ce que
    // quelque chose d'autre invalide le cache — c'est-à-dire, en pratique, jamais.
    bool emote_pending = false;
    // Hauteur de la RANGEE courante. Elle vaut `line_h` par defaut, et GRANDIT
    // quand une vignette depasse : sans ca une image de 128 px mordrait sur la
    // ligne suivante. Remise a `line_h` a chaque repli — c'est une propriete de
    // la rangee, pas de la ligne.
    float row_h = line_h;
    // 🔴 La visibilité se juge RANGÉE PAR RANGÉE, pas une fois pour la ligne. Elle
    // l'était, avec la hauteur d'UNE rangée : une ligne repliée sur cinq
    // disparaissait d'un bloc dès que son sommet passait au-dessus de la vue, et
    // ses quatre autres rangées avec — alors qu'elles occupaient tout l'écran. On
    // ne le voyait pas tant qu'un message tenait sur une ou deux rangées.
    auto row_visible = [&] { return y + row_h >= view_top && y <= view_bottom; };
    // Hauteur des vignettes et des emotes. La case dit S'IL Y EN A, le curseur
    // dit LAQUELLE — les deux questions sont séparées. Bornage quand même : une
    // valeur aberrante venue d'un yaml édité à la main ne doit pas produire une
    // rangée absurde.
    const float img_h =
        !thumbs_ ? 0.0f
                 : static_cast<float>(thumb_px_ < 24 ? 24
                                     : (thumb_px_ > 128 ? 128 : thumb_px_));

    if (timestamps_) {
      char stamp[16];
      std::snprintf(stamp, sizeof(stamp), "[%02d:%02d:%02d] ", line.hour, line.minute,
                    line.second);
      const float w = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, stamp).x;
      if (row_visible())
        dl->AddText(font, fsize, ImVec2(origin.x + x, origin.y + y), kStampCol, stamp);
      x += w;
    }
    if (diagnostic_) {  // le type tel qu'il nous est parvenu, avant tout filtre,
                        // et par QUELLE de nos deux sources il est entré
      char tag[12];
      std::snprintf(tag, sizeof(tag), "t%02d%c ", line.type, line.source);
      const float w = font->CalcTextSizeA(fsize, FLT_MAX, 0.0f, tag).x;
      if (row_visible())
        dl->AddText(font, fsize, ImVec2(origin.x + x, origin.y + y), kDiagCol, tag);
      x += w;
    }

    for (const Run& run : line.runs) {
      // Emote du JEU : elle sort du GRF, donc elle est là ou elle ne sera jamais.
      // Ni attente, ni place à réserver, ni repli progressif — les trois raisons
      // qui compliquent le bloc suivant ne s'appliquent pas ici.
      //
      // Le réglage la gouverne quand même : sa case dit « Images et emotes », et
      // un joueur qui la décoche s'attend à retrouver du texte. Il lit alors le
      // « :nom: » que le fragment porte déjà, c'est-à-dire exactement ce que voit
      // un joueur sans Bourgeon.
      //
      // 🔴 L'EXISTENCE SE TESTE AVANT DE TOUCHER À LA GÉOMÉTRIE. Replier la ligne
      // puis se rabattre sur le texte laisserait un repli décidé pour une image
      // qui n'arrive jamais — et une largeur mesurée qui ne correspond à rien.
      // Une entrée de la table peut manquer au fichier : la liste des emotes
      // suit le protocole, le GRF d'un client donné s'arrête où il s'arrête.
      if (run.game_emote >= 0 && url_preview_ && img_h > 0.0f &&
          ro::emote::Exists(run.game_emote)) {
        const float ih = img_h;
        if (x > 0.0f && x + ih > wrap) { x = 0.0f; y += row_h; row_h = line_h; }
        if (ih > row_h) row_h = ih;  // la rangée s'ouvre à la hauteur de l'emote
        if (row_visible()) {
          const ImVec2 p(origin.x + x, origin.y + y);
          ro::emote::Draw(dl, run.game_emote, p, ImVec2(p.x + ih, p.y + ih),
                          static_cast<float>(ImGui::GetTime()));
        }
        x += ih + 2.0f;
        continue;  // l'image REMPLACE le repli textuel
      }
      // Emote Discord : l'image à hauteur de ligne quand elle est arrivée, sinon
      // le « :nom: » qu'on a mis dans `text` — qui sera dessiné par le chemin
      // normal juste en dessous. Aucun trou, aucune attente visible.
      //
      // Le réglage d'images gouverne le TÉLÉCHARGEMENT, pas la lisibilité : coupé,
      // on garde « :nom: » et rien ne part sur le réseau.
      if (!run.emote_url.empty() && url_preview_ && img_h > 0.0f) {
        imgprev::Request(run.emote_url.c_str());
        const imgprev::Preview em = imgprev::Get(run.emote_url.c_str());
        const bool ready = (em.state == imgprev::Preview::kReady &&
                            em.tex != nullptr && em.h > 0);
        if (em.state == imgprev::Preview::kNone ||
            em.state == imgprev::Preview::kPending)
          emote_pending = true;  // pas de mise en cache de la hauteur

        // 🔴 RÉSERVER LA PLACE DE L'IMAGE DÈS MAINTENANT, même si elle n'est pas
        // encore là. Le repli « :nom: » est trois à quatre fois plus large qu'une
        // emote : l'afficher pendant le téléchargement faisait replier la ligne,
        // puis tout se réorganisait à l'arrivée du fichier. Une ligne de chat qui
        // bouge toute seule sous les yeux est pire qu'une case vide un quart de
        // seconde. Le carré occupe donc la place, et l'image s'y pose.
        //
        // Le repli textuel garde tout son sens quand les images sont ÉTEINTES :
        // là, rien n'arrivera jamais, et « :nom: » est la seule lecture possible.
        if (ready || em.state != imgprev::Preview::kFailed) {
          const float ih = img_h;
          const float iw = ready
              ? ih * static_cast<float>(em.w) / static_cast<float>(em.h)
              : ih;  // carré par défaut : une emote Discord l'est
          if (x > 0.0f && x + iw > wrap) { x = 0.0f; y += row_h; row_h = line_h; }
          if (ih > row_h) row_h = ih;  // la rangée s'ouvre à la hauteur de l'image
          if (row_visible() && ready) {
            const ImVec2 p(origin.x + x, origin.y + y);
            dl->AddImage(TexId(em.tex), p, ImVec2(p.x + iw, p.y + ih));
          }
          x += iw + 2.0f;
          continue;  // l'image (ou sa place) REMPLACE le repli textuel
        }
        // Échec définitif : on retombe sur « :nom: », dessiné plus bas.
      }
      // Icône d'objet : hauteur de ligne, largeur au ratio d'origine (pas de
      // déformation).
      if (run.item_id != 0 && run.text.empty()) {
        if (!item_icons_) continue;
        ro::IconTex icon = ro::ItemIcon(run.item_id);
        if (icon.tex != nullptr && icon.h > 0) {
          const float ih = fsize + 2.0f;
          const float iw = ih * static_cast<float>(icon.w) / static_cast<float>(icon.h);
          if (x > 0.0f && x + iw > wrap) { x = 0.0f; y += row_h; row_h = line_h; }
          if (row_visible()) {
            const ImVec2 p(origin.x + x, origin.y + y);
            dl->AddImage(TexId(icon.tex), p, ImVec2(p.x + iw, p.y + ih));
          }
          x += iw + 2.0f;
        }
        continue;
      }

      const ImU32 col = run.is_link() ? kLinkCol : (run.color != 0 ? run.color : def_col);
      const std::string& u = run.text;
      // ── La police de CE fragment ─────────────────────────────────────────
      // 🔴 Elle sert à la MESURE autant qu'au dessin. Le gras est plus large que
      // le normal : mesurer avec l'une et dessiner avec l'autre ferait tomber
      // les retours à la ligne à côté, et le survol d'un lien avec.
      //
      // Absente du système = on garde la police courante. Un texte non gras vaut
      // mieux qu'un texte manquant.
      //
      // ⚠ Gras ET italique à la fois : le gras l'emporte, faute d'une police
      // grasse-italique. Le cas est rare et la perte, discrète.
      ImFont* rfont = font;
      if (ImFont* fam = ro::ChatFamilyFont(font_family_, run.bold, run.italic))
        rfont = fam;
      // Largeur d'espace DE CE FRAGMENT : elle varie avec la police, et c'est
      // elle qui espace les mots ci-dessous.
      const float rspace = (rfont == font)
                               ? space_w
                               : rfont->CalcTextSizeA(fsize, FLT_MAX, 0.0f, " ").x;
      // ── Zone cliquable d'un lien : le RUN ENTIER, espaces compris ───────────
      // Elle était posée mot à mot, ce qui laissait un trou à chaque espace :
      // « [Test's Axe] » n'était survolable que sur « [Test's » et « Axe] », et le
      // curseur retombait dans le vide entre les deux — juste là où l'on vise
      // quand on pointe un nom. On accumule donc l'étendue du lien sur la rangée
      // courante et on ne la teste qu'une fois refermée (fin du run, repli ou
      // saut de ligne), ce qui la rend continue.
      float seg_x0 = -1.0f, seg_x1 = 0.0f, seg_y = 0.0f;  // -1 = rien d'ouvert
      // 🔴 HAUTEUR du segment survolable. Elle était codée en dur à celle d'une
      // ligne de texte : sur une vignette de 128 px, seuls les 14 premiers
      // pixels réagissaient, et le reste de l'image semblait mort. Elle suit
      // désormais ce qu'on a réellement dessiné.
      float seg_h = fsize + 2.0f;
      auto flush_link_hit = [&]() {
        if (!run.is_link() || seg_x0 < 0.0f) return;
        const ImVec2 a(origin.x + seg_x0, origin.y + seg_y);
        const ImVec2 b(origin.x + seg_x1, origin.y + seg_y + seg_h);
        seg_x0 = -1.0f;
        if (!hovering_log || !ImGui::IsMouseHoveringRect(a, b)) return;
        ro::SetHoverCursor(2);  // curseur « main » RO
        // La description simple SOUS LA SOURIS, comme sur une cellule d'objet :
        // lire un lien ne devrait pas obliger à cliquer.
        links::HoverPreview(TargetOf(run));
        // Convention commune à tout le client (features/link_gesture.h) : gauche
        // = description, droite = menu, Maj+clic = lien dans la barre.
        //
        // ⚠ L'ACTION est retenue et jouée après le déverrouillage de `lines_` :
        // elle peut ouvrir un navigateur, et le faire sous le verrou bloquerait
        // l'ingestion le temps que le shell démarre. On copie donc la cible —
        // pointer dans le deque serait pire encore, l'ingestion y pousse depuis
        // le fil du jeu.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
          click_target = TargetOf(run);
          click_shift  = ImGui::GetIO().KeyShift;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
          link_menu_   = TargetOf(run);
          menu_request = true;
        }
      };
      // ── Miniature d'une image de NOTRE miroir ────────────────────────────
      //
      // L'adresse est remplacée par l'image elle-même. Une ligne de chat n'a rien
      // à gagner à afficher soixante caractères de hash — l'image, si.
      //
      // 🔴 UNIQUEMENT NOTRE DOMAINE, et ce n'est pas une commodité. Une miniature
      // se charge à l'AFFICHAGE, pas au survol : pour un hébergeur tiers, la
      // requête partirait à la seconde où la ligne apparaît, sans que personne
      // n'ait rien demandé. Ce serait la fuite d'IP contre laquelle tout ce module
      // est bâti, en pire. Sur notre miroir, il n'y a rien à fuiter.
      //
      // Elle reste un LIEN : survol = aperçu en grand, clic = ouvrir, clic droit =
      // menu. On lui donne le même segment de survol que du texte.
      if (run.kind == Run::kUrl && url_preview_ && img_h > 0.0f &&
          IsMirrorImage(run.url)) {
        imgprev::Request(run.url.c_str());
        const imgprev::Preview th = imgprev::Get(run.url.c_str());
        const bool ready = (th.state == imgprev::Preview::kReady &&
                            th.tex != nullptr && th.h > 0);
        if (th.state == imgprev::Preview::kNone ||
            th.state == imgprev::Preview::kPending)
          emote_pending = true;  // hauteur pas encore stable (cf. plus haut)

        // Place réservée dès maintenant, même raison que pour les emotes — et le
        // besoin est ici plus criant : le repli est une adresse de soixante
        // caractères, qui replie la ligne sur deux rangées avant de se réduire à
        // une vignette. La ligne aurait bougé à chaque image reçue.
        if (ready || th.state != imgprev::Preview::kFailed) {
          const float ih = img_h;
          const float iw = ready
              ? ih * static_cast<float>(th.w) / static_cast<float>(th.h)
              : ih;  // proportion inconnue tant que rien n'est décodé
          if (x > 0.0f && x + iw > wrap) { x = 0.0f; y += row_h; row_h = line_h; }
          if (ih > row_h) row_h = ih;
          if (row_visible() && ready) {
            const ImVec2 p(origin.x + x, origin.y + y);
            dl->AddImage(TexId(th.tex), p, ImVec2(p.x + iw, p.y + ih));
          }
          seg_x0 = x;
          seg_x1 = x + iw;
          seg_y  = y;
          seg_h  = ih;  // toute l'image, pas sa première ligne
          x += iw + 2.0f;
          flush_link_hit();
          continue;  // l'image (ou sa place) REMPLACE l'adresse
        }
        // Échec définitif : l'adresse redevient du texte cliquable, dessiné plus bas.
      }
      size_t i = 0;
      while (i < u.size()) {
        // 🔴 Le SAUT DE LIGNE est un séparateur, au même titre que l'espace. Sans
        // ce cas, le mot qui suit un `\n` était passé tel quel à `AddText` : la
        // mesure (`CalcTextSizeA`) s'arrête au `\n` et rend une largeur NULLE,
        // tandis que le rendu, lui, honore le saut et dessine le mot une rangée
        // plus bas — au `x` courant, donc en plein milieu du vide. Le mot semblait
        // « lâché » à droite et tout ce qui suivait se réempilait au même endroit.
        // Vu avec les messages relayés depuis Discord, qui portent de vrais
        // retours à la ligne.
        if (u[i] == '\n') { flush_link_hit(); x = 0.0f; y += line_h; ++i; continue; }
        if (u[i] == '\r') { ++i; continue; }
        if (u[i] == ' ') {
          // L'espace d'un lien APPARTIENT au lien : sans ça, « [Test's Axe] »
          // laissait un trou pile entre ses deux mots — là où le curseur passe
          // forcément en visant le milieu du nom. Il n'agrandit la zone que si un
          // mot du lien la précède sur cette rangée (sinon on collerait au lien la
          // marge qui traîne devant lui).
          if (seg_x0 >= 0.0f) seg_x1 += rspace;
          x += rspace;
          ++i;
          continue;
        }
        size_t j = i;
        while (j < u.size() && u[j] != ' ' && u[j] != '\n' && u[j] != '\r') ++j;
        // 🔴 UN MOT PEUT ÊTRE PLUS LARGE QUE TOUTE LA ZONE. Le repli mot à mot
        // n'a alors rien à couper : il descend le mot d'une rangée, où il déborde
        // exactement pareil, et la fin du texte sort de la fenêtre. C'est ce que
        // font un « FAAAA…AAA » de trois cents caractères, une adresse sans
        // espace ou un nom de fichier — tous les jours dans un chat public.
        //
        // Le mot est donc découpé AU CARACTÈRE quand il ne tient pas : chaque
        // tour de boucle pose ce qui rentre dans la place restante, replie, et
        // recommence sur le reste.
        while (i < j) {
          const char* w0   = u.c_str() + i;
          const char* wend = u.c_str() + j;
          // Une seule mesure, BORNÉE à la place restante : elle répond aux deux
          // questions d'un coup — la largeur à dessiner, et par `stop` le point
          // où le mot cesse de rentrer. `stop` tombe sur une frontière de
          // CODEPOINT, jamais au milieu d'un accent ni d'un caractère CP949.
          const char* stop = wend;
          float ww = rfont->CalcTextSizeA(fsize, wrap - x, 0.0f, w0, wend, &stop).x;
          // Ça ne rentre pas, mais la rangée a déjà du contenu : on descend
          // d'abord — le mot a droit à la largeur entière avant qu'on envisage de
          // le couper. Le repli COUPE la zone cliquable : la partie déjà posée se
          // referme sur sa rangée, la suite en ouvrira une autre plus bas.
          if (stop != wend && x > 0.0f) {
            flush_link_hit();
            x = 0.0f;
            y += row_h;
            row_h = line_h;
            stop = wend;
            ww   = rfont->CalcTextSizeA(fsize, wrap, 0.0f, w0, wend, &stop).x;
          }
          // ⚠ Toute la largeur et même pas un glyphe (fenêtre réduite à quelques
          // pixels) : `stop` ne bouge pas, `i` non plus, et la boucle tourne pour
          // toujours — un FREEZE, pas un défaut d'affichage. On pose un caractère
          // quoi qu'il arrive, ses octets de continuation avec lui.
          if (stop <= w0) {
            stop = w0 + 1;
            while (stop < wend && (static_cast<unsigned char>(*stop) & 0xC0) == 0x80) ++stop;
            ww = rfont->CalcTextSizeA(fsize, FLT_MAX, 0.0f, w0, stop).x;
          }
          const char* w1 = stop;
          const ImVec2 pos(origin.x + x, origin.y + y);
          if (row_visible()) {
            // Équipement CASSÉ : l'OMBRE rouge du natif (DrawName 0x008972c0),
            // décalée +1,+1 sous un texte inchangé — le même rendu que dans les
            // viewers, et la raison d'être du champ privé de la balise.
            if (run.kind == Run::kItem && run.item.broken)
              dl->AddText(rfont, fsize, ImVec2(pos.x + 1.0f, pos.y + 1.0f),
                          itemcell::kDamagedShadow, w0, w1);
            dl->AddText(rfont, fsize, pos, col, w0, w1);
            // Pas de soulignement : le lien se reconnaît déjà à sa couleur, à ses
            // crochets et à son icône, et le trait salissait une ligne de chat
            // dense (il n'y en a pas non plus dans le chat natif). Au survol, le
            // curseur « main » suffit à dire que c'est cliquable.
            if (run.is_link()) {
              if (seg_x0 < 0.0f) { seg_x0 = x; seg_y = y; seg_h = fsize + 2.0f; }
              seg_x1 = x + ww;
            }
          }
          x += ww;
          i = static_cast<size_t>(w1 - u.c_str());
          // Mot coupé en plein milieu : la suite descend d'une rangée. Le repli
          // est fait ICI, sans attendre le tour suivant — la place restante ne
          // vaut plus qu'un caractère au mieux, et le tour suivant repartirait de
          // toute façon sur une mesure vide.
          if (w1 != wend) { flush_link_hit(); x = 0.0f; y += row_h; row_h = line_h; }
        }
      }
      flush_link_hit();
    }
    x = 0.0f;
    y += row_h;  // dernière rangée : elle aussi peut avoir grandi
    if (!emote_pending) {
      line.cached_wrap   = wrap;
      line.cached_flags  = layout_flags;
      line.cached_height = y - line_top;
    } else {
      line.cached_wrap = -1.0f;  // à remesurer quand l'image sera là
    }
  }
  lock.unlock();  // le verrou ne couvre QUE le parcours des lignes
  // 🔴 Réserver `y` seul CROIT la dernière ligne d'un demi-caractère : `y` cumule
  // des PAS, et le pas est plus court que le texte dès que l'interligne est serré.
  // Le contenu s'arrêtait donc au pas de la dernière ligne, pas sous son glyphe —
  // et comme le bas de contenu borne le défilement, la ligne la plus récente était
  // rognée en permanence, quel que soit le réglage.
  if (y > 0.0f) y += LineOverhang(&channel);
  ImGui::Dummy(ImVec2(wrap, y));  // réserve la hauteur pour le scroll

  // Le geste retenu plus haut, joué ici — hors du verrou. (La description, elle,
  // reste ARMÉE : elle passe par le natif, proscrit pendant une frame ImGui.)
  if (click_target.valid()) {
    if (click_shift) links::PostToChat(click_target);
    else             links::OpenDescription(click_target);
  }
  // ⚠ Ouverture et rendu du popup dans la MÊME fenêtre ImGui (ici l'enfant du
  // log) : son identifiant se hache avec la pile d'ids de la fenêtre courante.
  if (menu_request) ImGui::OpenPopup("##chat_link_menu_log");
  links::DrawMenu("##chat_link_menu_log", link_menu_);
}

// 🔴 Les couleurs de `channels.conf` sont choisies pour le fond SOMBRE du chat —
// `White`, `Yellow`, `LightGreen` y sont lisibles. Le corps d'une fenêtre RO, lui,
// est CLAIR : posées telles quelles dans la liste déroulante, la moitié d'entre
// elles disparaîtrait purement et simplement. On garde la teinte (c'est elle qui
// identifie le canal) et on plafonne sa LUMINANCE — un jaune reste jaune, mais
// assez sombre pour se lire. Coefficients de luminance perçue (Rec. 601), les
// mêmes que partout ailleurs pour ce genre d'arbitrage.
static uint32_t DarkenForLightBody(uint32_t col) {
  const float r = static_cast<float>((col >> IM_COL32_R_SHIFT) & 0xFF);
  const float g = static_cast<float>((col >> IM_COL32_G_SHIFT) & 0xFF);
  const float b = static_cast<float>((col >> IM_COL32_B_SHIFT) & 0xFF);
  const float lum = 0.299f * r + 0.587f * g + 0.114f * b;
  constexpr float kMaxLum = 140.0f;  // sur 255 : le seuil où le texte reste net
  if (lum <= kMaxLum) return col;
  const float k = kMaxLum / lum;
  return IM_COL32(static_cast<int>(r * k), static_cast<int>(g * k),
                  static_cast<int>(b * k), 255);
}

// Ligne de saisie, disposée comme celle du client : box du destinataire à
// gauche (« Pseudo »), puis la saisie. Les champs sont CLAIRS (le cadre pousse
// FrameBg pour ça) — le texte doit donc y être sombre.

// ── Les deux grilles du sélecteur ────────────────────────────────────────────
// Dimensions COMMUNES : sans ça le popup change de taille en passant d'un onglet
// à l'autre, et les onglets se dérobent sous le curseur au moment même où l'on
// cherche à revenir.
namespace {
constexpr int   kPickerCols = 10;
constexpr float kPickerCell = 30.0f;
constexpr float kPickerRows = 6.0f;

ImVec2 PickerGridSize() {
  const float pad = ImGui::GetStyle().ItemSpacing.x;
  const float bar = ImGui::GetStyle().ScrollbarSize;
  return ImVec2(kPickerCols * kPickerCell + (kPickerCols - 1) * pad + bar + pad,
                kPickerRows * (kPickerCell + pad));
}
}  // namespace

void ChatWindow::DrawEmotePicker(const ImVec2& btn_min, const ImVec2& btn_max,
                                 int whisper_index) {
  // ── La grille s'ouvre AU-DESSUS du bouton, jamais en travers de la barre ────
  // Par défaut ImGui pose un popup au point de clic : la grille recouvrait donc
  // la saisie et les dernières lignes du log — c'est-à-dire exactement ce qu'on
  // relit en choisissant quoi répondre.
  //
  // 🔴 Poser la position à la main DÉSACTIVE le garde-fou d'ImGui : le clamp
  // « la fenêtre reste visible » de `Begin()` est conditionné à
  // `!window_pos_set_by_api`. Une chatbox remontée en haut de l'écran enverrait
  // donc la grille hors champ, sans rattrapage — d'où le repli en dessous et le
  // bornage horizontal ci-dessous, qu'il faut faire nous-mêmes.
  {
    const ImVec2 fallback(PickerGridSize().x + 24.0f,
                          PickerGridSize().y + 64.0f);  // 1re ouverture : estimé
    const ImVec2 size = (picker_size_.y > 0.0f) ? picker_size_ : fallback;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 lo = vp->WorkPos;
    const ImVec2 hi(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y);
    constexpr float kGap = 4.0f;

    float y = btn_min.y - kGap - size.y;
    if (y < lo.y) y = btn_max.y + kGap;  // pas la place au-dessus : en dessous
    y = ImClamp(y, lo.y, ImMax(lo.y, hi.y - size.y));
    const float x = ImClamp(btn_min.x, lo.x, ImMax(lo.x, hi.x - size.x));
    ImGui::SetNextWindowPos(ImVec2(x, y));
  }

  if (!ImGui::BeginPopup("##chat_emote_grid")) {
    // 🔴 Demande de fermeture devenue caduque, à jeter — sinon elle referme la
    // grille SUIVANTE au moment même où on l'ouvre. Le cas arrive quand Échap
    // tombe juste après une fermeture au clic : `OnRawKey` arme encore le
    // drapeau (le prédicat tolère une frame de retard, il est lu hors frame) et
    // plus personne n'est là pour le consommer. `PickerOpen()` garde le
    // nettoyage inoffensif tant qu'UNE grille est ouverte quelque part — celle
    // d'une conversation, alors que c'est la principale, fermée, qui passe ici.
    if (!PickerOpen()) picker_close_ = false;
    // Le popup a disparu (Échap, clic au dehors) après une pioche d'emoji : on
    // rend le clavier à la saisie D'OÙ L'ON EST PARTI, comme après un envoi.
    // 🔴 Le test sur la cible n'est pas une précaution de style : cette fonction
    // tourne pour CHAQUE conversation ouverte plus la barre principale, et sans
    // lui le premier appel venu emporterait le focus.
    if (picker_picked_ && picker_picked_target_ == whisper_index) {
      picker_picked_ = false;
      if (whisper_index >= 0 &&
          whisper_index < static_cast<int>(channels_.size()))
        channels_[whisper_index].whisper_focus = true;
      else if (focus_on_whisper_)
        focus_whisper_next_ = true;
      else
        focus_input_next_ = true;
    }
    return;
  }
  // Elle est à l'écran : c'est ce numéro de frame que lisent `WantsEscapeKey` et
  // `OnRawKey`, appelés depuis le WndProc entre deux frames.
  picker_open_frame_ = ImGui::GetFrameCount();
  // 🔴 Et la pile Échap des fenêtres RO est neutralisée tant qu'elle est ouverte.
  // Sans ça, un Échap tapé avec une autre fenêtre RO derrière (l'inventaire, le
  // storage…) fermerait LES DEUX d'un coup : la grille par le chemin ci-dessous,
  // la fenêtre par `ro::ProcessEscapeStack`. Une touche, une fermeture.
  ro::SuppressEscapeStack();
  // Échap reçu pendant que la grille était ouverte. 🔴 ImGui ne ferme PAS les
  // popups sur Échap de lui-même ici : ce chemin appartient à la navigation
  // clavier, que le projet n'active pas (ConfigFlags, ragnarok_client). Sans ce
  // rappel, la touche ne refermerait rien du tout.
  if (picker_close_) {
    picker_close_ = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);

  // Deux natures, deux onglets — et deux gestes différents au clic, expliqués
  // dans chacune des grilles.
  if (ro::RoBeginTabBar("##chat_picker_tabs")) {
    if (ImGui::BeginTabItem(i18n::Tr("Emotes"))) {
      DrawGameEmoteGrid(whisper_index);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(i18n::Tr("Emoji"))) {
      DrawEmojiGrid(whisper_index);
      ImGui::EndTabItem();
    }
    ro::RoEndTabBar();
  }

  // ── La croix, en haut à droite ──────────────────────────────────────────────
  // Dessinée EN DERNIER pour passer par-dessus la barre d'onglets, dans l'espace
  // vide qu'elle laisse à sa droite. Un popup n'a pas de barre de titre — donc
  // pas de bouton système — et se ferme d'ordinaire par un clic au dehors : le
  // geste existe, mais rien ne le montre, et une palette qu'on ne sait pas
  // refermer est une palette qui reste dans les jambes.
  {
    const float side = ImGui::GetFontSize();
    const float pad  = ImGui::GetStyle().WindowPadding.x;
    const ImVec2 pos(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - pad - side,
                     ImGui::GetWindowPos().y + ImGui::GetStyle().WindowPadding.y);
    if (ImGui::CloseButton(ImGui::GetID("##picker_close"), pos))
      ImGui::CloseCurrentPopup();
  }

  // Mesurée pour la PROCHAINE ouverture : c'est elle qui permet de poser la
  // grille au-dessus du bouton (cf. l'en-tête de la fonction).
  picker_size_ = ImGui::GetWindowSize();

  ImGui::PopStyleColor();
  ImGui::EndPopup();
}

// ── L'onglet EMOTES ──────────────────────────────────────────────────────────
// Ne montre QUE ce que le GRF de ce client contient : la table des noms est celle
// du protocole, plus longue que le fichier sur la plupart des installations.
// Proposer une case vide serait promettre une emote qui ne s'afficherait chez
// personne — pas même chez celui qui l'a écrite.
void ChatWindow::DrawGameEmoteGrid(int whisper_index) {
  // 🔴 La zone DÉFILE, et sa barre reste visible : il y a plus de quatre-vingts
  // emotes pour six rangées affichées. Masquer la barre laisserait croire que ce
  // qu'on voit est tout ce qu'il y a — la molette seule ne se devine pas.
  constexpr int   kCols = kPickerCols;
  constexpr float kCell = kPickerCell;
  ImGui::BeginChild("##chat_emote_scroll", PickerGridSize());

  ImDrawList* dl  = ImGui::GetWindowDrawList();
  const float now = static_cast<float>(ImGui::GetTime());
  int col = 0;
  for (int id = 0; id < ro::emote::Count(); ++id) {
    if (!ro::emote::Exists(id)) continue;
    if (col != 0) ImGui::SameLine();
    ImGui::PushID(id);
    const ImVec2 p       = ImGui::GetCursorScreenPos();
    const bool   clicked = ImGui::InvisibleButton("##e", ImVec2(kCell, kCell));
    const ImVec2 q(p.x + kCell, p.y + kCell);
    ro::emote::Draw(dl, id, p, q, now, true);
    if (ImGui::IsItemHovered()) {
      dl->AddRect(p, q, IM_COL32(40, 40, 40, 160));
      ImGui::SetTooltip(":%s:", ro::emote::Name(id));
    }
    if (clicked) {
      char code[40];
      std::snprintf(code, sizeof(code), ":%s:", ro::emote::Name(id));
      // 🔴 ENVOI DIRECT, sans passer par la barre de saisie. Ce n'est pas un
      // raccourci de confort : le relais Discord ne remplace le lien du GIF par
      // son aperçu que si ce lien est TOUT le message. La moindre lettre autour
      // — un « lol » resté dans la saisie, un espace — et l'emote redevient une
      // adresse en clair sur Discord. Envoyer seul est donc la seule façon de
      // garantir le rendu des deux côtés du pont.
      //
      // Ce que le joueur avait déjà tapé n'est pas touché : il le retrouve
      // intact en revenant au champ.
      Channel* target =
          (whisper_index >= 0 && whisper_index < static_cast<int>(channels_.size()))
              ? &channels_[whisper_index]
              : nullptr;
      SendTextNow(code, (target != nullptr) ? target->whisper_with.c_str() : nullptr);
      // 🔴 ET LE CLAVIER REVIENT, comme après un envoi ordinaire — à la saisie
      // D'OÙ L'ON PART. Le geste est une SOURIS, mais ce qui suit reste du
      // clavier : sans ça le joueur doit recliquer dans le champ pour continuer
      // sa phrase, et il ne comprend pas pourquoi.
      if (target != nullptr) {
        target->whisper_focus = true;
        // La conversation vient de servir : elle ne doit pas être celle qu'on
        // sacrifie quand le plafond de fenêtres est atteint.
        target->whisper_stamp = GetTickCount();
      } else if (focus_on_whisper_) {
        focus_whisper_next_ = true;
      } else {
        focus_input_next_ = true;
      }
      // Fermer sur le clic : c'est un envoi, pas une palette où l'on pioche.
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopID();
    if (++col == kCols) col = 0;
  }

  ImGui::EndChild();

  // ── Export, sur demande explicite ───────────────────────────────────────────
  // Ce n'est pas une fonction de jeu mais un outil ponctuel : sortir les emotes
  // en GIF pour les héberger, là où le relais Discord ira les chercher par leur
  // nom. Un joueur n'a rien à en faire, et même le staff n'en a besoin qu'une
  // fois — d'où la case à armer dans les réglages plutôt qu'un bouton permanent
  // au milieu de la grille. Et seulement depuis la barre principale : dans une
  // conversation, un outil de maintenance n'a rien à faire sous le nez du joueur.
  if (whisper_index < 0 && IsStaff() && emote_export_) {
    ImGui::Separator();
    if (ImGui::SmallButton(i18n::Tr("Exporter en GIF"))) {
      const std::string dir = paths::GameDir() + "emotes_export";
      const int written = ro::emote::ExportGifs(dir.c_str(), 2);
      if (written < 0)
        LogError("[chat] export des emotes : sprite illisible");
      else
        LogInfo("[chat] export des emotes : {} fichiers dans {}", written, dir);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(i18n::Tr("Ecrit un GIF par emote dans « emotes_export »,\n"
                        "a cote de l'executable. Pour Discord."));
  }
}

// ── L'onglet EMOJI ───────────────────────────────────────────────────────────
// Le geste n'est pas celui des emotes, et la différence tient à la nature de ce
// qu'on pose :
//   · une emote est une IMAGE que le relais Discord ne sait montrer que si elle
//     est tout le message — d'où l'envoi immédiat, seul ;
//   · un emoji est du TEXTE, qui s'affiche partout au milieu d'une phrase. On
//     l'INSÈRE donc dans la saisie, et la palette reste ouverte : on en pioche
//     volontiers deux ou trois d'affilée.
void ChatWindow::DrawEmojiGrid(int whisper_index) {
  ImGui::BeginChild("##chat_emoji_scroll", PickerGridSize());

  // La police du chat est calibrée pour du texte ; à cette taille un emoji est
  // un timbre-poste. On agrandit pour la grille seulement — `PushFont(nullptr,
  // …)` garde la police courante et ne change que la taille, et ImGui 1.92 cuit
  // la nouvelle taille à la demande (le backend DX9 sait mettre son atlas à
  // jour). En DX7, où l'atlas est figé, ImGui met simplement à l'échelle ce
  // qu'il a : c'est moins net, et de toute façon les emoji y manquent.
  //
  // ⚠ La taille normale est relevée AVANT le push, pour les intertitres. Elle ne
  // se retrouve pas avec `PushFont(nullptr, 0.0f)` : ce zéro-là veut dire
  // « garde la taille COURANTE », donc la grande — les titres seraient géants.
  const float title_size = ImGui::GetFontSize();
  ImGui::PushFont(nullptr, kPickerCell * 0.72f);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 hover_col = IM_COL32(40, 40, 40, 160);
  for (int c = 0; c < ro::emoji::CategoryCount(); ++c) {
    const ro::emoji::Category& cat = ro::emoji::CategoryAt(c);
    // 🔴 L'intertitre se traduit ICI : la table les garde en français nu, comme
    // les items de `ro::RoCombo` (les envelopper à la source les traduirait deux
    // fois et polluerait le gabarit d'export).
    ImGui::PushFont(nullptr, title_size);
    ImGui::SeparatorText(i18n::Tr(cat.label));
    ImGui::PopFont();

    ImGui::PushID(c);
    for (int i = 0; i < cat.count; ++i) {
      if (i % kPickerCols != 0) ImGui::SameLine();
      ImGui::PushID(i);
      const ImVec2 p       = ImGui::GetCursorScreenPos();
      const bool   clicked =
          ImGui::InvisibleButton("##x", ImVec2(kPickerCell, kPickerCell));
      const ImVec2 q(p.x + kPickerCell, p.y + kPickerCell);
      // Centré dans sa case : les glyphes emoji n'ont pas tous la même chasse,
      // et alignés à gauche ils donnent une grille qui tremble.
      const ImVec2 sz = ImGui::CalcTextSize(cat.items[i]);
      // ⚠ `kDarkText`, pas du blanc : un glyphe en COULEUR ignore la teinte
      // (ImGui le dessine « non teinté »), mais si la police emoji manque du
      // système, le repli est un losange monochrome — en blanc, il serait
      // invisible sur le corps clair du popup.
      dl->AddText(ImVec2(p.x + (kPickerCell - sz.x) * 0.5f,
                         p.y + (kPickerCell - sz.y) * 0.5f),
                  kDarkText, cat.items[i]);
      if (ImGui::IsItemHovered()) dl->AddRect(p, q, hover_col);
      if (clicked) {
        AppendToInput(cat.items[i], whisper_index);
        // Retenu pour rendre le clavier quand la palette se refermera : on ne
        // peut pas le faire maintenant sans la fermer sous les doigts du joueur.
        picker_picked_        = true;
        picker_picked_target_ = whisper_index;
      }
      ImGui::PopID();
    }
    ImGui::PopID();
  }

  ImGui::PopFont();
  ImGui::EndChild();
}

// Pose du texte à la fin de la saisie visée. Rien n'est envoyé : c'est au joueur
// de valider, comme pour tout ce qu'il écrit.
void ChatWindow::AppendToInput(const char* utf8, int whisper_index) {
  if (utf8 == nullptr || utf8[0] == '\0') return;
  const size_t add = std::strlen(utf8);

  if (whisper_index >= 0 &&
      whisper_index < static_cast<int>(channels_.size())) {
    Channel& channel = channels_[whisper_index];
    if (channel.whisper_input.size() + channel.whisper_pending_insert.size() +
            add + 1 > Channel::kInputBufSize)
      return;  // plein : mieux vaut ne rien poser qu'un emoji coupé en deux
    // 🔴 EN ATTENTE, pas directement : `DrawWhisperInput` a déjà recopié
    // `whisper_input` dans son tampon local pour cette frame et le réécrira par
    // -dessus en sortant. Voir `whisper_pending_insert`.
    channel.whisper_pending_insert += utf8;
    // La conversation vient de servir : elle ne doit pas être celle qu'on
    // sacrifie quand le plafond de fenêtres est atteint.
    channel.whisper_stamp = GetTickCount();
    return;
  }

  // La barre principale, elle, écrit dans `input_` — le tampon que l'InputText
  // utilise directement, d'où le `NotifyInputEdited` obligatoire.
  const size_t used = std::strlen(input_);
  if (used + add + 1 > sizeof(input_)) return;  // plein : on ne tronque pas un emoji
  std::memcpy(input_ + used, utf8, add + 1);
  NotifyInputEdited();  // 🔴 sinon le champ ACTIF réécrit son propre texte par-dessus
  // La barre repliée (battle mode) doit s'ouvrir, sinon le joueur ne voit pas où
  // son emoji est parti.
  if (battle_mode_) input_open_ = true;
}

// Envoie un texte court TOUT DE SUITE, sans le faire transiter par la saisie.
//
// 🔴 Ni historique de saisie, ni historique de destinataires : ce n'est pas une
// ligne que le joueur a écrite. La flèche du haut doit lui rendre ses phrases,
// pas la liste des emotes qu'il a cliquées.
//
// Le destinataire courant est respecté — une emote part là où l'on parle, canal
// ou conversation privée comprise, exactement comme un message ordinaire. Et le
// départ est DIFFÉRÉ comme tous les autres : `FlushPending` tourne hors frame,
// une commande native jouée pendant le rendu gèle le client.
bool ChatWindow::SendTextNow(const char* utf8, const char* whisper_utf8) {
  if (utf8 == nullptr || utf8[0] == '\0') return false;
  // Un seul envoi peut attendre : écraser celui qui est là perdrait la phrase
  // que le joueur vient de valider. La fenêtre ne dure qu'une frame.
  if (has_pending_) return false;
  pending_text_ = ro::Utf8ToWireText(utf8);
  // 🔴 Le destinataire est celui de la fenêtre D'OÙ L'ON PART, pas celui de la
  // barre principale : une emote cliquée dans une conversation 1:1 doit partir à
  // ce correspondant-là, même si la box destinataire du chat dit autre chose.
  pending_whisper_ =
      ro::Utf8ToWire((whisper_utf8 != nullptr) ? whisper_utf8 : whisper_);
  has_pending_ = true;
  return true;
}

void ChatWindow::DrawInputRow() {
  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
  ImGui::SetNextItemWidth(90.0f);
  if (focus_whisper_next_) {
    ImGui::SetKeyboardFocusHere();
    // Consommée seulement le geste fini, exactement comme pour la saisie plus
    // bas : `SetKeyboardFocusHere` refuse tant qu'un bouton est enfoncé ou qu'un
    // glisser court. La reprise du clavier vient justement d'un CLIC — l'effacer
    // pendant le clic, c'est perdre la demande.
    if (!ImGui::IsAnyMouseDown() && !ImGui::IsDragDropActive())
      focus_whisper_next_ = false;
  }
  // 🔴 ENTRÉE VAUT DEPUIS ICI AUSSI. Le natif ne demande pas au joueur de savoir
  // dans laquelle des deux boxes il a laissé son curseur : Entrée envoie, ou
  // referme si le texte est vide — que le pseudo soit rempli ou non. Sans
  // `EnterReturnsTrue`, la touche se perdait et la barre restait ouverte.
  const bool whisper_submitted = ImGui::InputTextWithHint(
      "##chat_whisper", i18n::Tr("Pseudo"), whisper_, sizeof(whisper_),
      ImGuiInputTextFlags_EnterReturnsTrue);
  // L'id du champ, pour que `TargetWhisper` puisse prévenir ImGui depuis un menu
  // contextuel qu'on a écrit dans son buffer (cf. NotifyWhisperEdited).
  whisper_field_id_ = ImGui::GetItemID();
  // Le focus vit sur DEUX champs, et le battle mode ne doit se refermer que
  // lorsqu'il a quitté les deux (cf. la fin de cette fonction).
  const bool whisper_active      = ImGui::IsItemActive();
  // Relevé ICI, collé au widget : plus bas, « l'item » désignerait le dernier
  // dessiné (chips de liens, menu contextuel), et le test d'Échap porterait sur
  // autre chose.
  const bool whisper_deactivated = ImGui::IsItemDeactivated();
  // TAB → la saisie. Elle est dessinée APRÈS dans la même frame, donc le focus
  // bascule immédiatement, sans le battement d'une frame.
  if (whisper_active && ImGui::IsKeyPressed(ImGuiKey_Tab, false))
    focus_input_next_ = true;
  ImGui::PopStyleColor();
  // Clic droit = les destinataires récents, ce que le bouton natif « Select
  // Receiver » (msg 0xE1) fait avec une combo. Le geste est le même que sur les
  // onglets, et il ne coûte pas un pixel d'une rangée déjà serrée.
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
    ImGui::OpenPopup("##chat_whisper_hist");
  if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("##chat_whisper_hist")) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
    ImGui::SetTooltip(i18n::Tr("Destinataire du chuchotement.\nClic droit : les récents."));
    ImGui::PopStyleColor();
  }
  DrawWhisperHistoryPopup();
  ImGui::SameLine();

  // Le mode d'envoi est celui du CLIENT (g_ChatInputTargetMode) : le lire plutôt
  // que d'en tenir un second, sinon les deux chats divergent.
  int mode = ReadSendMode();
  static const char* const kModes[] = {"Tous", "Groupe", "Guilde", "Clan", "Alliés"};
  if (mode < 0 || mode >= static_cast<int>(_countof(kModes))) mode = 0;
  // 🔴 L'APERÇU dit où le texte PART, pas quel mode est armé — et ce n'est pas la
  // même chose : un destinataire non vide court-circuite le mode d'envoi (cf.
  // NativeSendChatText). Un « #canal » dans la box l'emporte donc sur « Tous », et
  // l'aperçu doit le dire, sans quoi le joueur croit parler à la carte alors qu'il
  // parle au canal. Le cas du chuchotement à un PSEUDO reste affiché comme le mode,
  // lui : le nom est déjà LU dans la box juste à côté, et le répéter mangerait la
  // seule information que la combo apporte.
  const bool channel_selected = whisper_[0] == '#';
  const char* preview = channel_selected ? whisper_ : kModes[mode];
  ImGui::SetNextItemWidth(80.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
  if (ro::RoBeginCombo("##chat_mode", preview)) {
    for (int i = 0; i < static_cast<int>(_countof(kModes)); ++i) {
      if (ImGui::Selectable(kModes[i], !channel_selected && mode == i)) {
        WriteSendMode(i);
        // Choisir un mode natif, c'est choisir de NE PLUS parler au canal : sans
        // ce nettoyage la box garderait son « #canal », qui l'emporte sur le mode
        // qu'on vient de désigner — le message partirait au canal en silence.
        // Vaut aussi pour un pseudo : la combo est le sélecteur de destination.
        whisper_[0] = '\0';
      }
    }
    // ── Les canaux du serveur, poussés par ZC 0x0F21 ────────────────────────
    // Ils ne sont PAS des modes d'envoi du client : parler dans un canal, c'est
    // chuchoter à « #nom » (routage rAthena). D'où le choix d'écrire le nom dans
    // la box destinataire plutôt que de toucher au mode natif — la combo n'invente
    // aucun chemin d'envoi, elle remplit un champ que le joueur pourrait taper.
    bool separator_drawn = false;
    for (const ServerChannel& channel : server_channels_) {
      if (channel.require_guild && !InGuild()) continue;  // #ally hors guilde
      if (!separator_drawn) {
        ImGui::Separator();
        separator_drawn = true;
      }
      ImGui::PushStyleColor(ImGuiCol_Text, DarkenForLightBody(channel.color));
      const bool current = channel_selected && std::strcmp(whisper_, channel.name.c_str()) == 0;
      if (ImGui::Selectable(channel.name.c_str(), current))
        CopyBounded(whisper_, sizeof(whisper_), channel.name.c_str());
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered() && !channel.alias.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
        // Le canal peut être en LECTURE seule (CHAN_OPT_CAN_CHAT absent) : le dire
        // ici plutôt que de masquer l'entrée — le joueur doit comprendre pourquoi
        // son message n'est pas parti, et le serveur, lui, refusera poliment.
        if (channel.can_chat)
          ImGui::SetTooltip("%s", channel.alias.c_str());
        else
          ImGui::SetTooltip(i18n::Tr("%s\nLecture seule."), channel.alias.c_str());
        ImGui::PopStyleColor();
      }
    }
    ro::RoEndCombo();
  }
  ImGui::PopStyleColor();
  ImGui::SameLine();

  // ── Le sélecteur d'emotes ───────────────────────────────────────────────────
  // Un carré de la hauteur de la rangée, qui porte pour étiquette une emote plutôt
  // qu'un caractère : l'atlas de police est borné à l'ASCII (pas de ☺ à espérer),
  // et un `:)` textuel dirait mal ce que le bouton ouvre.
  const float  pick_side = ImGui::GetFrameHeight();
  const ImVec2 pick_pos  = ImGui::GetCursorScreenPos();
  if (ImGui::Button("##chat_emote_btn", ImVec2(pick_side, pick_side)))
    ImGui::OpenPopup("##chat_emote_grid");
  const bool pick_hovered = ImGui::IsItemHovered();
  {
    // Marge d'un pixel : l'emote ne doit pas mordre le cadre du bouton.
    const ImVec2 in_min(pick_pos.x + 2.0f, pick_pos.y + 2.0f);
    const ImVec2 in_max(pick_pos.x + pick_side - 2.0f, pick_pos.y + pick_side - 2.0f);
    if (!ro::emote::Draw(ImGui::GetWindowDrawList(), kEmotePickerIcon, in_min, in_max,
                         static_cast<float>(ImGui::GetTime()), true)) {
      // Sprite absent : plutôt qu'un bouton vide, le repli ASCII.
      ImGui::GetWindowDrawList()->AddText(in_min, kDarkText, ":)");
    }
  }
  if (pick_hovered) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
    ImGui::SetTooltip(i18n::Tr("Emotes et emoji.\nUne emote part SEULE, en clair "
                      "(« :smile: ») : tout le monde la lit.\nUn emoji, lui, "
                      "s'ajoute à ta phrase."));
    ImGui::PopStyleColor();
  }
  DrawEmotePicker(pick_pos,
                  ImVec2(pick_pos.x + pick_side, pick_pos.y + pick_side));
  ImGui::SameLine();

  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 4.0f);
  // Géométrie du champ relevée AVANT sa soumission (le curseur aura avancé au
  // retour) ; le repeint des liens, lui, se fait APRÈS — il RECOUVRE le texte.
  const ImVec2 field_pos = ImGui::GetCursorScreenPos();
  const float  field_w   = ImGui::CalcItemWidth();
  const float  field_h   = ImGui::GetFrameHeight();
  if (focus_input_next_) {
    ImGui::SetKeyboardFocusHere();
    // 🔴 LA DEMANDE N'EST CONSOMMÉE QU'UNE FOIS LE GESTE TERMINÉ. `SetKeyboardFocusHere`
    // commence par un refus SEC — « ignored while DragDropActive » — dès qu'un
    // glisser est en cours ou qu'une fenêtre est déplacée (imgui.cpp, tout en
    // haut de la fonction). Or la demande est justement posée EN PLEIN GESTE :
    // le Maj+clic qui pose un lien part d'une cellule d'inventaire, laquelle est
    // une source de glisser. La demande était pourtant effacée dans la foulée —
    // le champ n'avait donc jamais le focus, et la PREMIÈRE Entrée servait à
    // l'ouvrir au lieu d'envoyer : « il faut appuyer deux fois pour envoyer un
    // lien ». La re-poser jusqu'à la fin du geste suffit, et ça ne peut pas
    // boucler : le relâchement, lui, arrive toujours.
    if (!ImGui::IsAnyMouseDown() && !ImGui::IsDragDropActive())
      focus_input_next_ = false;
  }
  // ↑/↓ rappellent l'historique de saisie. ImGui n'expose ça QUE par callback :
  // le tampon appartient au widget pendant l'édition, l'écrire à côté (dans
  // `input_`) serait écrasé par l'état interne à la frame suivante.
  const auto history_cb = [](ImGuiInputTextCallbackData* data) -> int {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
      auto* self = static_cast<ChatWindow*>(data->UserData);
      self->RecallHistory(data->EventKey == ImGuiKey_UpArrow ? -1 : 1, data);
    }
    return 0;
  };
  const bool submitted = ImGui::InputText(
      "##chat_input", input_, sizeof(input_),
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory,
      history_cb, this);
  // L'id du champ, pour que les Append*Link puissent prévenir ImGui depuis une
  // AUTRE fenêtre qu'on a modifié son buffer (cf. NotifyInputEdited).
  input_field_id_ = ImGui::GetItemID();
  // TAB → le champ « Pseudo ». Celui-ci étant dessiné AVANT, la bascule prend
  // effet à la frame suivante : un battement, invisible à la frappe.
  const bool input_active      = ImGui::IsItemActive();
  const bool input_deactivated = ImGui::IsItemDeactivated();
  const bool input_hovered     = ImGui::IsItemHovered();
  if (input_active && ImGui::IsKeyPressed(ImGuiKey_Tab, false))
    focus_whisper_next_ = true;
  ImGui::PopStyleColor();
  // Les liens posés reprennent ici leur couleur et leurs crochets, par-dessus le
  // texte que le champ vient de peindre.
  DrawInputLinkChips(field_pos, field_w, field_h, input_active, input_hovered);
  links::DrawMenu("##chat_link_menu_input", link_menu_);
  // 🔴 LA PERTE DE FOCUS NE REFERME RIEN. DEUX sorties, toutes deux explicites :
  // Entrée sur un texte VIDE et ÉCHAP. Rien d'autre : ni la combo de mode, ni la
  // liste des destinataires, ni un onglet, ni le log, ni une autre fenêtre, ni un
  // clic dans le décor.
  //
  // Le pendant de cette règle est en haut de la fonction : ce qui fait perdre le
  // focus ne referme pas la barre, DONC la barre doit reprendre le clavier — sans
  // quoi elle reste ouverte et sourde, et la sortie coûte deux frappes.
  //
  // Il y avait ici une branche « désactivée sans envoi ⇒ on referme », calquée
  // sur le natif — mais « désactivée » attrape des gestes qui n'ont rien d'un
  // départ : ouvrir une liste déroulante désactive la saisie exactement comme un
  // clic dehors, et la barre se refermait sous le doigt à l'instant où la liste
  // s'ouvrait. D'où le test sur la TOUCHE et non sur la désactivation seule ;
  // distinguer autrement les bonnes désactivations des mauvaises reviendrait à
  // courir après chaque nouveau widget de la ligne.
  //
  // 🔴 LA DÉCISION NE REGARDE QUE LE TEXTE, jamais la box d'où vient la touche :
  // c'est la règle du natif, et c'est aussi la seule qui se retienne.
  if (submitted || whisper_submitted) {
    if (input_[0] != '\0') {
      QueueSend();          // on GARDE la main, comme le natif
      // ... et on la rend à la box d'où l'on a validé : le natif ne déplace pas
      // le curseur du joueur pour lui.
      if (whisper_submitted && !submitted)
        focus_whisper_next_ = true;
      else
        focus_input_next_ = true;
    } else if (battle_mode_) {
      input_open_ = false;  // texte vide : la sortie, en UNE frappe
    }
  } else if (battle_mode_ && (input_deactivated || whisper_deactivated) &&
             ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    // L'autre sortie. La désactivation + la touche, et pas la touche seule :
    // c'est bien une des deux boxes qu'Échap vient de quitter (ImGui la désactive
    // et restaure son texte dans la même frame), pas un Échap tapé ailleurs, qui
    // appartient au jeu.
    input_open_ = false;
    // 🔴 ET LA TOUCHE EST CONSOMMÉE. `ro::ProcessEscapeStack` tourne après tous les
    // OnRenderUI et referme la fenêtre RO la plus au-dessus : sans ce mot-là, une
    // seule frappe refermait la barre ET une fenêtre derrière. Même mécanique que
    // les modales, qui s'en servent pour ne pas fermer ce qu'elles recouvrent.
    ro::SuppressEscapeStack();
  }

  // Laquelle des deux boxes portait le clavier en dernier. Elle SURVIT à la perte
  // du focus — c'est tout son intérêt : un envoi, une emote cliquée ou un lien
  // posé rendent le clavier là où le joueur avait laissé son curseur, et pas
  // d'office dans la saisie.
  if (whisper_active) focus_on_whisper_ = true;
  if (input_active) focus_on_whisper_ = false;
  // Le clavier est-il DANS la ligne, cette frame ? Relevé ici pour `OwnsEnterKey`,
  // que le WndProc interroge entre deux frames — il ne peut pas appeler
  // `IsItemActive` lui-même.
  if (input_active || whisper_active) input_focus_frame_ = ImGui::GetFrameCount();
}

// ── Persistance de la disposition ────────────────────────────────────────────
// 🔴 Le fichier fait autorité dès qu'il existe : le relire, c'est reprendre la
// main sur le registre natif, qui décrit encore la disposition d'avant. D'où
// `structure_owned_` posé ici — sans lui, la fusion périodique écraserait deux
// secondes plus tard tout ce qu'on vient de recharger.
void ChatWindow::LoadLayout() {
  std::ifstream file(paths::ChatLayoutPath());
  if (!file) return;  // premier lancement : le registre natif fera l'amorçage
  std::vector<Channel> loaded;
  try {
    const YAML::Node root = YAML::Load(file);
    // Le verrou de la fenêtre PRINCIPALE. Il vit ici plutôt que dans les réglages
    // généraux parce que c'est de la géométrie, comme les autres — et parce qu'il
    // a des frères : un par canal détaché, quelques lignes plus bas.
    locked_ = root["locked"].as<bool>(false);
    const YAML::Node list = root["channels"];
    if (!list || !list.IsSequence()) return;
    for (const YAML::Node& node : list) {
      if (static_cast<int>(loaded.size()) >= kMaxChannels) break;
      Channel channel;
      channel.name = node["name"].as<std::string>("");
      if (channel.name.empty()) continue;  // un onglet sans nom est inattrapable
      channel.id       = node["id"].as<unsigned>(0);
      // 🔴 `group` fait foi, `detached` n'est plus qu'un repli pour les fichiers
      // écrits avant le groupage : un canal détaché sans groupe s'en voit
      // attribuer un plus bas, une fois tous les identifiants connus.
      const bool was_detached = node["detached"].as<bool>(false);
      SetChannelGroup(channel, node["group"].as<unsigned>(was_detached ? 0xFFFFFFFFu
                                                                      : 0u));
      // Défaut FAUX, et c'est le bon sens de l'erreur : une fenêtre libre se
      // reverrouille d'un clic, une fenêtre figée par accident donne l'impression
      // d'être cassée.
      channel.locked   = node["locked"].as<bool>(false);
      // ⚠ Un canal rechargé n'a PAS de nœud de registre, et n'en aura pas : la
      // fusion est coupée dès que notre fichier fait autorité. Ses filtres
      // vivent donc UNIQUEMENT chez nous — ce qui suffit, puisque c'est nous qui
      // filtrons l'affichage. Le chat natif, lui, garde sa propre copie jusqu'à ce
      // qu'on écrive vraiment dans les deux registres.
      channel.detach_owned = true;
      // 🔴 « TOUT COCHÉ » D'ABORD, le fichier ensuite — et pas l'inverse. Une
      // disposition écrite avant la case broadcast ne porte que 25 valeurs ; les
      // cases qu'elle ne nomme pas garderaient le défaut du `Channel`, qui est
      // ZÉRO. Le joueur qui met à jour verrait alors les annonces serveur
      // disparaître de tous ses onglets d'un coup, sans avoir rien décoché et
      // sans un mot pour l'expliquer. Ce qu'un fichier ne dit pas, il l'accepte.
      std::memset(channel.filter, 1, sizeof(channel.filter));
      const YAML::Node filter = node["filter"];
      if (filter && filter.IsSequence()) {
        for (size_t i = 0; i < filter.size() && i < kFilterCount; ++i)
          channel.filter[i] = filter[i].as<bool>(true) ? 1 : 0;
      }
      // L'absence de bloc « style » VEUT DIRE « suit les réglages généraux » :
      // c'est l'état par défaut, pas une valeur manquante à combler.
      if (const YAML::Node style = node["style"]) {
        channel.style_own = true;
        channel.font_pct  = style["font"].as<int>(100);
        channel.padding   = style["padding"].as<int>(3);
        channel.line_gap  = style["line_gap"].as<int>(2);
        const YAML::Node body = style["body"];
        if (body && body.IsSequence()) {
          for (size_t i = 0; i < body.size() && i < 4; ++i)
            channel.body[i] = body[i].as<float>(0.0f);
        }
      }
      loaded.push_back(std::move(channel));
    }
  } catch (const std::exception& e) {
    LogError("[Chat] disposition illisible ({}) -> on repart du registre client",
             e.what());
    return;
  }
  if (loaded.empty()) return;

  // Les identifiants doivent rester UNIQUES après rechargement, sinon deux canaux
  // partageraient leurs réglages le jour où ceux-ci seront par onglet.
  uint32_t next = 1;
  for (Channel& channel : loaded) {
    if (channel.id == 0) channel.id = 0xFFFFFFFFu;  // marqué à réattribuer
    next = (channel.id != 0xFFFFFFFFu && channel.id >= next) ? channel.id + 1 : next;
  }
  for (Channel& channel : loaded)
    if (channel.id == 0xFFFFFFFFu) channel.id = next++;
  next_channel_id_ = next;

  // Même travail pour les identifiants de FENÊTRE, et une reprise au passage :
  // un fichier écrit avant le groupage ne porte pas de `group`, ses canaux
  // détachés sont marqués 0xFFFFFFFF plus haut. Chacun reçoit sa propre fenêtre —
  // c'est exactement ce que l'ancienne disposition décrivait, une par canal.
  uint32_t next_group = 1;
  for (const Channel& channel : loaded)
    if (channel.group != 0xFFFFFFFFu && channel.group >= next_group)
      next_group = channel.group + 1;
  for (Channel& channel : loaded)
    if (channel.group == 0xFFFFFFFFu) SetChannelGroup(channel, next_group++);
  next_group_id_ = next_group;

  channels_.swap(loaded);
  structure_owned_ = true;
  active_channel_  = 0;
  for (size_t i = 0; i < channels_.size(); ++i) {
    if (!channels_[i].detached) {
      active_channel_ = static_cast<int>(i);
      break;
    }
  }
  LogDiag("[Chat] disposition rechargée : {} canaux", channels_.size());
}

void ChatWindow::SaveLayout() const {
  if (channels_.empty()) return;  // rien à dire vaut mieux qu'écraser par du vide
  YAML::Emitter out;
  out << YAML::BeginMap;
  out << YAML::Key << "locked" << YAML::Value << locked_;  // fenêtre principale
  out << YAML::Key << "channels" << YAML::Value << YAML::BeginSeq;
  for (const Channel& channel : channels_) {
    out << YAML::BeginMap;
    out << YAML::Key << "id" << YAML::Value << channel.id;
    out << YAML::Key << "name" << YAML::Value << channel.name;
    // `detached` reste écrit pour qu'une version antérieure relise le fichier
    // sans tout perdre ; c'est `group` qui porte la vérité, et lui seul sait que
    // deux canaux partagent une fenêtre.
    out << YAML::Key << "detached" << YAML::Value << channel.detached;
    out << YAML::Key << "group" << YAML::Value << channel.group;
    // Le verrou de SA fenêtre. Écrit même à faux : c'est un état de géométrie, et
    // le fichier est fait pour se relire à l'œil.
    out << YAML::Key << "locked" << YAML::Value << channel.locked;
    out << YAML::Key << "filter" << YAML::Value << YAML::Flow << YAML::BeginSeq;
    // 26 valeurs : la dernière est la case broadcast, qui n'existe que chez nous
    // et n'a donc nulle part ailleurs où survivre à la fermeture du jeu.
    for (int i = 0; i < kFilterCount; ++i) out << (channel.filter[i] != 0);
    out << YAML::EndSeq;
    // L'apparence n'est écrite QUE si elle est propre à ce canal. Sérialiser une
    // copie des réglages généraux les figerait : à la relecture, l'onglet cesserait
    // de suivre un changement du réglage général sans que rien ne l'ait demandé.
    if (channel.style_own) {
      out << YAML::Key << "style" << YAML::Value << YAML::BeginMap;
      out << YAML::Key << "font" << YAML::Value << channel.font_pct;
      out << YAML::Key << "padding" << YAML::Value << channel.padding;
      out << YAML::Key << "line_gap" << YAML::Value << channel.line_gap;
      out << YAML::Key << "body" << YAML::Value << YAML::Flow << YAML::BeginSeq;
      for (int i = 0; i < 4; ++i) out << channel.body[i];
      out << YAML::EndSeq;
      out << YAML::EndMap;
    }
    out << YAML::EndMap;
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;

  std::ofstream file(paths::ChatLayoutPath(), std::ios::trunc);
  if (!file) {
    LogError("[Chat] impossible d'écrire {}", paths::ChatLayoutPath());
    return;
  }
  file << "# Bourgeon — disposition de la chatbox : onglets, fenêtres détachées,\n"
       << "# filtres et verrouillage de chacun. L'« id » est un identifiant STABLE :\n"
       << "# c'est lui qui porte les réglages, pas le nom (renommable) ni l'ordre.\n"
       << "# Le « locked » de la racine est celui de la fenêtre principale ; chaque\n"
       << "# canal porte celui de SA fenêtre, qui ne vaut que s'il est détaché.\n"
       << out.c_str() << "\n";
}

// ── Historique conservé d'une session à l'autre ──────────────────────────────
// On écrit la ligne AVANT analyse (`raw`, balisage intact) plus sa couleur, son
// TYPE et son expéditeur. Le texte rendu seul perdrait les couleurs et les liens
// d'objets — et surtout le type, que les filtres par canal consultent : des lignes
// restaurées sans type ne sauraient plus dans quel onglet aller.
void ChatWindow::SaveHistory() const {
  if (!keep_history_) return;
  const int keep = (keep_lines_ < 20) ? 20 : ((keep_lines_ > 1000) ? 1000 : keep_lines_);

  YAML::Emitter out;
  out << YAML::BeginMap;
  SYSTEMTIME now;
  GetLocalTime(&now);
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d", now.wYear,
                now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  out << YAML::Key << "saved_at" << YAML::Value << stamp;
  out << YAML::Key << "lines" << YAML::Value << YAML::BeginSeq;
  {
    std::lock_guard<std::mutex> lock(lines_mutex_);
    size_t first = 0;
    if (lines_.size() > static_cast<size_t>(keep)) first = lines_.size() - keep;
    for (size_t i = first; i < lines_.size(); ++i) {
      const Line& line = lines_[i];
      out << YAML::BeginMap;
      out << YAML::Key << "t" << YAML::Value << static_cast<int>(line.type);
      out << YAML::Key << "rgb" << YAML::Value << line.rgb;
      char hms[16];
      std::snprintf(hms, sizeof(hms), "%02d:%02d:%02d", line.hour, line.minute,
                    line.second);
      out << YAML::Key << "at" << YAML::Value << hms;
      if (!line.sender.empty())
        out << YAML::Key << "from" << YAML::Value << line.sender;
      // 🔴 LE CORRESPONDANT, sans quoi la ligne perd sa fenêtre à la relecture.
      // Une ligne de conversation restaurée sans lui n'est plus routée nulle part
      // — donc reprise par les onglets ordinaires, en style conversation et sans
      // en-tête : exactement le défaut que la copie de journal corrige, revenu
      // par la porte de derrière.
      if (!line.whisper_with.empty())
        out << YAML::Key << "with" << YAML::Value << line.whisper_with;
      out << YAML::Key << "raw" << YAML::Value << line.raw;
      out << YAML::EndMap;
    }
  }
  out << YAML::EndSeq;
  out << YAML::EndMap;

  std::ofstream file(paths::ChatHistoryPath(), std::ios::trunc);
  if (!file) {
    LogError("[Chat] impossible d'écrire {}", paths::ChatHistoryPath());
    return;
  }
  file << "# Bourgeon — historique du chat conservé entre deux sessions.\n"
       << "# ATTENTION : ce fichier contient les CHUCHOTEMENTS EN CLAIR.\n"
       << "# L'option qui l'écrit est dans les réglages de la chatbox ImGui.\n"
       << out.c_str() << "\n";
}

void ChatWindow::LoadHistory() {
  if (!keep_history_) return;
  std::ifstream file(paths::ChatHistoryPath());
  if (!file) return;
  std::vector<Line> restored;
  std::string saved_at;
  try {
    const YAML::Node root = YAML::Load(file);
    saved_at = root["saved_at"].as<std::string>("");
    const YAML::Node list = root["lines"];
    if (!list || !list.IsSequence()) return;
    for (const YAML::Node& node : list) {
      Line line;
      line.type = static_cast<uint8_t>(node["t"].as<int>(0));
      line.rgb  = node["rgb"].as<unsigned>(0xFFFFFF);
      const std::string at = node["at"].as<std::string>("");
      if (at.size() == 8) {  // « HH:MM:SS »
        line.hour   = static_cast<uint8_t>(std::atoi(at.substr(0, 2).c_str()));
        line.minute = static_cast<uint8_t>(std::atoi(at.substr(3, 2).c_str()));
        line.second = static_cast<uint8_t>(std::atoi(at.substr(6, 2).c_str()));
      }
      line.sender = node["from"].as<std::string>("");
      // Absent = la ligne n'appartient à aucune conversation. C'est le cas de
      // l'immense majorité, et c'est aussi ce que dit un historique écrit par une
      // version d'avant : elles repartent dans les onglets, comme avant.
      line.whisper_with = node["with"].as<std::string>("");
      // 🔴 `ParseUtf8` et NON `ParseText` : le texte est déjà en UTF-8 dans le
      // fichier, le repasser par la conversion depuis la code-page du fil le
      // corromprait — c'est le même piège que les accents, à l'envers.
      ParseUtf8(node["raw"].as<std::string>(""), &line);
      if (line.runs.empty() && line.plain.empty()) continue;
      restored.push_back(std::move(line));
    }
  } catch (const std::exception& e) {
    LogError("[Chat] historique illisible ({}) -> ignoré", e.what());
    return;
  }
  if (restored.empty()) return;

  // Un séparateur, parce que l'heure affichée ment sur la date : une ligne d'hier
  // soir se réaffiche avec une heure parfaitement plausible. Visible dans TOUS les
  // onglets quel que soit leur filtre — et c'est `pinned` qui le garantit
  // désormais, plus le type broadcast : celui-ci a sa case depuis qu'on peut
  // taire les annonces serveur, et le repère aurait disparu avec elles.
  Line mark;
  mark.pinned = true;
  mark.type   = static_cast<uint8_t>(kTypeBroadcast);
  mark.rgb    = 0x9B9B9B;  // COLORREF gris, comme les en-têtes du client
  Run run;
  // 🔴 PAS de ro::LocalToUtf8 ici : le catalogue i18n est déjà en UTF-8 et `saved_at`
  // est une date ASCII. Convertir une seconde fois depuis la code-page du client
  // encodait les accents DEUX fois — « sesión » sortait en « sesiÃ³n ». Invisible en
  // français, dont le repère est écrit sans accent : seule une traduction accentuée
  // le révélait. Même piège qu'au chargement des lignes plus haut, dans l'autre sens.
  run.text = saved_at.empty()
                 ? std::string(i18n::Tr("---- session precedente ----"))
                 : (i18n::Tr("---- session precedente (") + saved_at + ") ----");
  mark.runs.push_back(run);
  mark.plain = run.text;
  restored.push_back(std::move(mark));

  std::lock_guard<std::mutex> lock(lines_mutex_);
  // Devant : ce sont les lignes les plus ANCIENNES. Les mettre à la suite les
  // ferait passer pour ce qui vient d'arriver.
  lines_.insert(lines_.begin(), restored.begin(), restored.end());
  // 🔴 Insérées EN TÊTE, donc le repère de comptage ne vaut plus rien : il
  // désigne un nombre de lignes depuis le début, et le début a bougé. On repart
  // d'un compte neuf, que `TrimLines` refera intégralement.
  std::memset(type_count_, 0, sizeof(type_count_));
  counted_lines_ = 0;
  TrimLines();
  LogDiag("[Chat] historique rechargé : {} lignes", restored.size() - 1);
}

// Nouvel onglet. Le nom par défaut est celui du client (`NewTab_N`) : ce nom finit
// dans SON fichier de session, autant qu'il y trouve ce qu'il sait écrire.
void ChatWindow::CreateChannel() {
  Channel channel;
  channel.id = next_channel_id_++;
  // Les homonymes sont PERMIS — c'est pour ça que la clé est un identifiant et pas
  // le nom — mais en proposer un d'office serait juste désagréable.
  for (int n = 2; n < 100; ++n) {
    char name[32];
    std::snprintf(name, sizeof(name), i18n::Tr("NewTab_%d"), n);
    bool taken = false;
    for (const Channel& other : channels_)
      if (other.name == name) taken = true;
    if (!taken) {
      channel.name = name;
      break;
    }
  }
  if (channel.name.empty()) channel.name = "NewTab";
  // Un onglet neuf qui n'afficherait rien passerait pour cassé : il accepte tout,
  // et le joueur retire ce qu'il ne veut pas.
  std::memset(channel.filter, 1, sizeof(channel.filter));
  channels_.push_back(std::move(channel));
  structure_owned_ = true;
  layout_dirty_    = true;
  active_channel_  = static_cast<int>(channels_.size()) - 1;
}

// Fermeture. Appelée APRÈS `EndPopup` : retirer l'élément pendant que le menu tient
// encore un pointeur dessus le rendrait pendant.
void ChatWindow::CloseChannel(int index) {
  if (index < 0 || index >= static_cast<int>(channels_.size())) return;
  const uint32_t active_id =
      (active_channel_ >= 0 && active_channel_ < static_cast<int>(channels_.size()))
          ? channels_[active_channel_].id
          : 0;
  const bool closed_active = (channels_[index].id == active_id);
  channels_.erase(channels_.begin() + index);
  structure_owned_ = true;
  layout_dirty_    = true;

  // Suivre le canal, pas le rang : fermer le troisième onglet ne doit pas faire
  // sauter le joueur ailleurs si ce n'est pas celui qu'il lisait.
  active_channel_ = 0;
  if (!closed_active) {
    for (size_t i = 0; i < channels_.size(); ++i)
      if (channels_[i].id == active_id) active_channel_ = static_cast<int>(i);
  }
  for (size_t i = 0; i < channels_.size(); ++i) {
    if (!channels_[i].detached) {  // la dockée doit pointer un onglet, pas une flottante
      if (closed_active || channels_[active_channel_].detached)
        active_channel_ = static_cast<int>(i);
      break;
    }
  }
}

// ── Réordonnancement de la bande ─────────────────────────────────────────────
// `dest_slot` est le rang visé PARMI LES ONGLETS DOCKÉS tels qu'ils sont
// dessinés, celui qu'on déplace compris : c'est ce que le joueur a sous les yeux
// quand il lâche, donc la seule numérotation qui puisse traduire son geste.
void ChatWindow::SetChannelGroup(Channel& channel, uint32_t group) {
  channel.group    = group;
  channel.detached = (group != 0);
}

int ChatWindow::GroupSize(uint32_t group) const {
  int n = 0;
  for (const Channel& channel : channels_)
    if (channel.group == group) ++n;
  return n;
}

void ChatWindow::MoveChannelToGroup(int from, uint32_t group, int dest_slot) {
  if (from < 0 || from >= static_cast<int>(channels_.size())) return;

  // Les rangs du groupe CIBLE dans l'ordre d'affichage, SANS le canal déplacé.
  std::vector<size_t> slots;
  slots.reserve(channels_.size());
  for (size_t i = 0; i < channels_.size(); ++i)
    if (channels_[i].group == group && static_cast<int>(i) != from)
      slots.push_back(i);

  // 🔴 Le rang vient de la bande TELLE QU'ELLE EST DESSINÉE, celui qu'on déplace
  // COMPRIS quand il est déjà dans cette fenêtre. Il faut donc le retirer de la
  // numérotation avant de s'en servir — et sortir si le lâcher retombe à
  // l'endroit d'où l'on part : sans ça un glissement de deux pixels marquerait la
  // structure comme nôtre et déclencherait une écriture du fichier pour rien.
  if (channels_[from].group == group) {
    int src = -1, k = 0;
    for (size_t i = 0; i < channels_.size(); ++i) {
      if (channels_[i].group != group) continue;
      if (static_cast<int>(i) == from) { src = k; break; }
      ++k;
    }
    if (src >= 0) {
      if (dest_slot == src || dest_slot == src + 1) return;
      if (dest_slot > src) --dest_slot;
    }
  }
  if (dest_slot < 0) dest_slot = 0;
  if (dest_slot > static_cast<int>(slots.size()))
    dest_slot = static_cast<int>(slots.size());

  // Le voisin devant lequel on s'insère, retenu par son IDENTIFIANT : les indices
  // ne survivront pas à la suppression qui suit.
  const uint32_t anchor_id =
      (dest_slot < static_cast<int>(slots.size())) ? channels_[slots[dest_slot]].id
                                                   : 0;
  const uint32_t active_id =
      (active_channel_ >= 0 && active_channel_ < static_cast<int>(channels_.size()))
          ? channels_[active_channel_].id
          : 0;

  Channel moved = std::move(channels_[from]);
  SetChannelGroup(moved, group);
  const uint32_t moved_id = moved.id;
  channels_.erase(channels_.begin() + from);

  size_t at = channels_.size();
  if (anchor_id != 0) {
    for (size_t i = 0; i < channels_.size(); ++i)
      if (channels_[i].id == anchor_id) { at = i; break; }
  } else {
    // Au bout du groupe : juste après son dernier canal. Groupe vide (on vient de
    // le créer) : à la fin du vecteur, l'ordre entre fenêtres ne se voyant nulle
    // part.
    bool found = false;
    for (size_t i = 0; i < channels_.size(); ++i)
      if (channels_[i].group == group) { at = i + 1; found = true; }
    if (!found) at = channels_.size();
  }
  channels_.insert(channels_.begin() + static_cast<ptrdiff_t>(at), std::move(moved));

  // 🔴 LE VERROU DÉCRIT UNE FENÊTRE, pas un canal — il faut donc que tous ceux
  // d'un même groupe en portent la même copie. Sans ça la géométrie se figerait
  // ou se libérerait selon l'onglet actif, `MakeSkin` lisant celui-là. Le nouveau
  // venu adopte celui de la fenêtre qu'il rejoint ; une fenêtre qui vient de
  // naître, elle, est LIBRE — on vient de la poser à la souris, et la première
  // chose qu'on en fera est de la déplacer.
  bool group_lock = false;
  for (const Channel& other : channels_)
    if (other.group == group && other.id != moved_id) {
      group_lock = other.locked;
      break;
    }
  for (Channel& other : channels_) {
    if (other.group != group) continue;
    other.locked = group_lock;
    // 🔴 Notre état gagne sur le registre natif : sans ce drapeau, la fusion
    // périodique remettrait le canal là où le client le croit — deux secondes
    // après le geste du joueur (cf. RefreshChannels).
    if (other.id == moved_id) other.detach_owned = true;
  }

  // L'onglet déposé devient l'ACTIF de sa nouvelle fenêtre : le joueur vient de le
  // désigner du doigt, le cacher derrière un autre serait absurde.
  if (group != 0) group_active_[group] = moved_id;

  // 🔴 `active_channel_` suit le CANAL et non le rang : ils viennent tous de
  // changer de sens. Sans ce recalage, déplacer un onglet ferait sauter le joueur
  // sur un autre canal.
  const uint32_t want = (group == 0) ? moved_id : active_id;
  active_channel_ = 0;
  for (size_t i = 0; i < channels_.size(); ++i)
    if (channels_[i].id == want) { active_channel_ = static_cast<int>(i); break; }
  // La principale doit pointer un de SES onglets : le canal actif a pu la quitter.
  if (active_channel_ < static_cast<int>(channels_.size()) &&
      channels_[active_channel_].group != 0) {
    for (size_t i = 0; i < channels_.size(); ++i)
      if (channels_[i].group == 0) { active_channel_ = static_cast<int>(i); break; }
  }
  // Le menu contextuel désigne par indice, lui aussi.
  logopt_channel_ = -1;

  // 🔴 Sans ça, la fusion périodique du registre natif remettrait la bande dans
  // SON ordre deux secondes plus tard, et le geste du joueur disparaîtrait sans
  // un mot (cf. RefreshChannels).
  structure_owned_ = true;
  layout_dirty_    = true;
}

// ── Confirmation avant de fermer un onglet ───────────────────────────────────
// 🔴 CE QUI EST PERDU, ET CE QUI NE L'EST PAS — et il fallait vérifier avant de
// l'écrire. Fermer un onglet ne touche PAS aux messages : ils vivent dans
// `lines_`, un tampon commun à toutes les fenêtres, et tout onglet qui les
// accepte continue de les montrer. Une conversation rouverte retrouve même ce qui
// y a été dit (cf. la fermeture différée). Ce qui disparaît, ce sont les RÉGLAGES
// du canal : ses 26 filtres, son apparence propre, son nom, son identifiant — et
// un onglet, contrairement à une conversation, ne se rouvre pas tout seul.
//
// Annoncer la perte des messages aurait fait renoncer le joueur pour une raison
// fausse, ce qui est pire qu'un menu sans garde-fou : une confirmation qui se
// trompe apprend à ne plus lire les confirmations.
void ChatWindow::DrawCloseConfirmPopup() {
  // 🔴 L'ID est le SEUL lien entre l'ouverture et le dessin, et il doit survivre à
  // la traduction du titre : d'où le `###`, dont ImGui ne hache que la fin.
  static constexpr const char* kId = "###chat_close_confirm";
  if (confirm_close_open_) {
    ImGui::OpenPopup(kId);
    confirm_close_open_ = false;
  }

  // Le canal peut s'être volatilisé entre la question et la réponse — fusion du
  // registre, changement de personnage. Par IDENTIFIANT et non par indice, pour
  // cette raison exacte : un indice aurait désigné le voisin.
  const Channel* channel = nullptr;
  for (const Channel& other : channels_)
    if (other.id == confirm_close_id_) channel = &other;
  const bool whisper = (channel != nullptr) && !channel->whisper_with.empty();

  if (!ro::BeginRoPopupModal(whisper
                                 ? i18n::Tr("Fermer la conversation ?###chat_close_confirm")
                                 : i18n::Tr("Fermer l'onglet ?###chat_close_confirm"))) {
    // Refermée autrement que par ses boutons (Échap, notamment) : le geste est
    // ABANDONNÉ. Sans cette remise à zéro, la demande resterait armée et la
    // prochaine ouverture croirait avoir déjà reçu son oui.
    confirm_close_id_ = 0;
    return;
  }
  if (channel == nullptr) {  // disparu : la question n'a plus d'objet
    confirm_close_id_ = 0;
    ImGui::CloseCurrentPopup();
    ro::EndRoPopupModal();
    return;
  }

  // Le nom que le joueur a sous les yeux dans la bande, pas le nom de canal qu'on
  // a donné à une conversation à sa création.
  const std::string& label = whisper ? channel->whisper_with : channel->name;
  if (whisper)
    ImGui::Text(i18n::Tr("Fermer la conversation avec %s ?"), label.c_str());
  else
    ImGui::Text(i18n::Tr("Fermer l'onglet « %s » ?"), label.c_str());
  ImGui::Spacing();

  // Gris explicite : sur le corps CLAIR d'une fenêtre RO, `TextDisabled` est
  // illisible — c'est une couleur pensée pour un fond sombre.
  const ImVec4 kGray(0.35f, 0.35f, 0.42f, 1.0f);
  if (whisper) {
    ImGui::TextColored(kGray,
                       i18n::Tr("Rien n'est perdu : le journal garde les messages, et\n"
                                "rouvrir la conversation les remontrera."));
  } else {
    ImGui::TextColored(kGray,
                       i18n::Tr("Ses réglages partent avec lui : les filtres du log, son\n"
                                "apparence propre, son nom. Un onglet ne se rouvre pas —\n"
                                "il faudra le recréer et le régler à nouveau."));
    ImGui::Spacing();
    ImGui::TextColored(kGray,
                       i18n::Tr("Les messages, eux, RESTENT : ils vivent dans un journal\n"
                                "commun à toutes les fenêtres, et les autres onglets\n"
                                "continuent de les afficher."));
  }
  ImGui::Spacing();

  if (ro::RoButton(i18n::Tr("Fermer"), 110.0f, 0.0f)) {
    // 🔴 On ne ferme pas ICI : on passe par `close_channel_id_`, consommé quelques
    // lignes plus bas dans la frame. C'est lui qui porte les gardes communes aux
    // trois gestes — jamais le dernier canal, jamais le dernier onglet de la
    // fenêtre principale — et les réécrire ici, c'était les voir diverger.
    close_channel_id_ = confirm_close_id_;
    confirm_close_id_ = 0;
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Annuler"), 100.0f, 0.0f)) {
    confirm_close_id_ = 0;
    ImGui::CloseCurrentPopup();
  }
  ro::EndRoPopupModal();
}

// Les 26 cases d'options de log du canal courant. Les 25 premières écrivent
// DIRECTEMENT l'octet du registre (node+0x2C+type), exactement comme la fenêtre
// native 0x84 : le registre reste la source de vérité, et le chat natif suit le
// même filtre. La 26e — le broadcast — n'a pas d'octet là-bas et ne vit que chez
// nous, dans la disposition sauvegardée.
void ChatWindow::DrawLogOptionsPopup() {
  // Marge du popup resserrée : celle du cadre du chat (réglable, jusqu'à 12 px)
  // vaut pour une fenêtre, pas pour un menu — un menu contextuel doit se lire d'un
  // coup d'œil, pas s'étaler.
  // 🔴 La marge reste POUSSÉE pendant tout le corps, pas seulement le temps du
  // Begin : les sous-menus ouvrent leurs propres fenêtres plus bas, et ils
  // reprendraient sinon la marge du cadre du chat — réglable jusqu'à 12 px, ce qui
  // convient à une fenêtre mais étale un menu.
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
  if (!ImGui::BeginPopup("##chat_logopt_popup")) {
    ImGui::PopStyleVar();
    return;
  }
  // L'onglet DÉSIGNÉ au clic droit. Il peut avoir disparu entre-temps (le registre
  // natif est relu périodiquement) : on revalide l'index à chaque frame plutôt que
  // de garder un pointeur, qui serait pendant au premier rafraîchissement.
  const int target = (logopt_channel_ >= 0) ? logopt_channel_ : active_channel_;
  Channel* channel =
      (target >= 0 && target < static_cast<int>(channels_.size()))
          ? &channels_[target]
          : nullptr;
  if (channel == nullptr) {
    ImGui::EndPopup();
    ImGui::PopStyleVar();  // la marge resserrée du menu
    return;
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);

  // Renommage sur place. Le tampon est réamorcé quand le menu change de canal —
  // sinon on éditerait le nom du précédent sans s'en apercevoir.
  if (rename_id_ != channel->id) {
    rename_id_ = channel->id;
    CopyBounded(rename_buf_, sizeof(rename_buf_), channel->name.c_str());
  }
  ImGui::SetNextItemWidth(170.0f);
  if (ImGui::InputText("##chat_rename", rename_buf_, sizeof(rename_buf_),
                       ImGuiInputTextFlags_EnterReturnsTrue)) {
    // Un nom vide rendrait l'onglet inattrapable : on refuse en silence plutôt que
    // d'ouvrir une modale pour si peu.
    if (rename_buf_[0] != '\0') {
      channel->name    = rename_buf_;
      structure_owned_ = true;
      layout_dirty_    = true;
    }
    ImGui::CloseCurrentPopup();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(i18n::Tr("Renommer le canal — Entrée pour valider."));
  ImGui::Separator();
  // Détacher / rattacher. 🔴 `detach_owned` fait gagner NOTRE état sur celui du
  // registre au prochain rafraîchissement : tant que le déplacement de l'entrée
  // entre les deux registres natifs n'est pas écrit, la fusion remettrait le canal
  // là où le client le croit — deux secondes après le geste du joueur.
  if (channel->detached) {
    if (ImGui::Selectable(i18n::Tr("Rattacher à la fenêtre principale"))) {
      SetChannelGroup(*channel, 0);
      channel->detach_owned = true;
      structure_owned_      = true;
      layout_dirty_         = true;
      active_channel_       = target;
    }
  } else {
    // Le dernier onglet docké ne peut pas partir : la fenêtre principale porte la
    // saisie, elle ne doit jamais se retrouver sans canal à afficher.
    int docked = 0;
    for (const Channel& other : channels_)
      if (!other.detached) ++docked;
    const bool can_detach = (docked > 1);
    if (!can_detach) ImGui::BeginDisabled();
    if (ImGui::Selectable(i18n::Tr("Détacher dans sa propre fenêtre"))) {
      SetChannelGroup(*channel, NewGroupId());
      channel->detach_owned = true;
      channel->locked       = false;  // une flottante naît libre (cf. l'arrachage)
      structure_owned_      = true;
      layout_dirty_         = true;
    }
    if (!can_detach) {
      ImGui::EndDisabled();
      // `AllowWhenDisabled` : sans ce drapeau, un item grisé n'est jamais survolé,
      // et l'infobulle qui EXPLIQUE le grisé ne s'afficherait justement jamais.
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(i18n::Tr("Le dernier onglet ne peut pas être détaché."));
    }
  }

  // ── Verrouillage de la géométrie ────────────────────────────────────────────
  // 🔴 SA PLACE EST ICI, et plus dans le panneau de réglages. Il y a un verrou PAR
  // FENÊTRE, et le panneau ne sait pas de laquelle on parle : une case unique y
  // figeait les trois d'un coup, y compris la flottante qu'on venait d'arracher.
  // Ce menu, lui, s'ouvre TOUJOURS depuis un onglet ou un en-tête — donc depuis
  // une fenêtre précise. Le libellé la nomme, parce que « verrouiller » tout court
  // ne veut rien dire quand trois fenêtres sont ouvertes.
  //
  // ⚠ Le verrou vit sur le CANAL alors qu'il décrit une FENÊTRE. Depuis qu'une
  // fenêtre en porte plusieurs, la case l'écrit donc sur TOUS les canaux du
  // groupe, et le rendu lit celui de l'onglet actif (`MakeSkin`). Une copie par
  // onglet plutôt qu'une table de groupes à ranger, à relire et à purger : le
  // verrou est un booléen, la redondance ne coûte rien et rien ne peut diverger
  // tant que les deux écritures passent par ici.
  const uint32_t group = channel->group;
  bool* const lock = (group != 0) ? &channel->locked : &locked_;
  if (ro::RoCheckbox(channel->detached ? i18n::Tr("Verrouiller cette fenêtre###chat_lock")
                                       : i18n::Tr("Verrouiller la fenêtre principale###chat_lock"),
                     lock)) {
    if (group != 0)
      for (Channel& other : channels_)
        if (other.group == group) other.locked = *lock;
    layout_dirty_ = true;  // le verrou se range avec la géométrie, pas avec les réglages
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        i18n::Tr("Fige la position et la taille de cette fenêtre-là. Les onglets, les "
        "menus et l'arrachage continuent de fonctionner —\nc'est la géométrie "
        "qui est verrouillée, pas la fenêtre.\n\n"
        "Chaque fenêtre a le sien : une flottante qu'on détache naît libre."));
  ImGui::Separator();

  // Créer / fermer. 🔴 Le plafond de 10 canaux n'est pas décoratif : le CHARGEUR
  // natif (`Lua_SetSubChatWndList 0x00a9cf70`) refuse au-delà, donc un 11ᵉ canal ne
  // planterait rien — il disparaîtrait à la reconnexion suivante, sans un mot.
  const bool can_create = static_cast<int>(channels_.size()) < kMaxChannels;
  if (!can_create) ImGui::BeginDisabled();
  if (ImGui::Selectable(i18n::Tr("Nouvel onglet"))) CreateChannel();
  if (!can_create) {
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          i18n::Tr("Maximum atteint : %d canaux, onglets et fenêtres détachées confondus.\n"
          "C'est la limite du client, pas la nôtre."),
          kMaxChannels);
  }

  // Fermeture : jamais le dernier canal, et jamais le dernier onglet DOCKÉ — la
  // fenêtre principale porte la saisie, elle ne doit pas rester sans rien à
  // afficher.
  int docked = 0;
  for (const Channel& other : channels_)
    if (!other.detached) ++docked;
  const bool can_close =
      channels_.size() > 1 && (channel->detached || docked > 1);
  if (!can_close) ImGui::BeginDisabled();
  // 🔴 Par IDENTIFIANT, et la fermeture attend la modale : `channel` pointe DANS
  // `channels_`, et fermer pendant que le menu tient encore ce pointeur le rendrait
  // pendant. C'était le rôle de l'ancien `close_request`, joué après `EndPopup` ;
  // la confirmation le remplace en repoussant la fermeture bien plus loin — à la
  // racine du rendu, hors de toute fenêtre, une fois la modale acquittée.
  if (ImGui::Selectable(i18n::Tr("Fermer l'onglet"))) {
    confirm_close_id_   = channel->id;
    confirm_close_open_ = true;
  }
  if (!can_close) {
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(i18n::Tr("Il doit rester au moins un onglet dans la fenêtre principale."));
  }
  ImGui::Separator();

  // Apparence PROPRE à cet onglet, dans son sous-menu. Le libellé annonce l'état :
  // « suit les réglages généraux » ou « réglages propres », pour qu'on sache avant
  // d'ouvrir si cet onglet a été personnalisé.
  char style_label[80];
  std::snprintf(style_label, sizeof(style_label), i18n::Tr("Apparence  (%s)###chatstyle_menu"),
                channel->style_own ? i18n::Tr("propre à cet onglet") : i18n::Tr("générale"));
  if (ImGui::BeginMenu(style_label)) {
    // Menu contextuel = place comptée. Le style compact resserre les hauteurs, et
    // les curseurs sont bornés en largeur : leur largeur par défaut est celle du
    // contenu disponible, ce qui étire le popup pour rien.
    PushStyleCompact();
    ImGui::PushItemWidth(140.0f);
    bool own = channel->style_own;
    if (ro::RoCheckbox(i18n::Tr("Réglages propres à cet onglet###chatstyle_own"), &own)) {
      // 🔴 En ACTIVANT, on part des valeurs générales : sinon le premier clic
      // ferait sauter l'onglet vers des valeurs par défaut sans rapport avec ce
      // que le joueur avait sous les yeux.
      if (own && !channel->style_own) {
        channel->font_pct = font_scale_pct_;
        channel->padding  = padding_px_;
        channel->line_gap = line_gap_px_;
        std::memcpy(channel->body, body_rgba_, sizeof(channel->body));
      }
      channel->style_own = own;
      layout_dirty_      = true;
      // La bascule change la taille EFFECTIVE de l'onglet (les siennes ou les
      // générales) : les hauteurs mémorisées ne valent plus.
      InvalidateLineLayout();
    }
    if (!channel->style_own) ImGui::BeginDisabled();
    bool touched  = false;
    bool relayout = WheelSliderInt(i18n::Tr("Taille du texte###chatstyle_font"),
                                   &channel->font_pct, kFontPctMin, kFontPctMax, "%d %%");
    touched |= WheelSliderInt(i18n::Tr("Marges###chatstyle_pad"), &channel->padding, 0, 12,
                              "%d px");
    relayout |= WheelSliderInt(i18n::Tr("Interligne###chatstyle_gap"), &channel->line_gap, 0, 16,
                               "%d px");
    touched |= RoColorSwatch("Fond###chatstyle_body", channel->body);
    if (relayout) InvalidateLineLayout();  // le PAS des lignes a changé
    if (touched || relayout) layout_dirty_ = true;
    if (!channel->style_own) ImGui::EndDisabled();
    ImGui::PopItemWidth();
    PopStyleCompact();
    ImGui::EndMenu();
  }
  ImGui::Separator();

  // 🔴 Les filtres vivent dans un SOUS-MENU. À plat, ils enterraient sous
  // vingt-six lignes les deux ou trois actions qu'on vient chercher — et un menu
  // qu'on ne lit plus est un menu qu'on n'utilise plus. Le compte des types actifs
  // est affiché dans le libellé : c'est l'information qu'on voulait obtenir en
  // ouvrant, dans neuf cas sur dix, sans avoir à dérouler.
  int active_filters = 0;
  for (int i = 0; i < kFilterCount; ++i)
    if (channel->filter[i] != 0) ++active_filters;
  char menu_label[64];
  std::snprintf(menu_label, sizeof(menu_label), i18n::Tr("Filtres du log  (%d/%d)###chatflt_menu"),
                active_filters, kFilterCount);
  if (ImGui::BeginMenu(menu_label)) {
    // Vingt-six cases : c'est ici que le style compact rend le plus.
    PushStyleCompact();
    // 🔴 Les boucles vont jusqu'à `kFilterCount`, donc la case broadcast suit ses
    // sœurs — « tout décocher » qui laisserait passer les annonces serveur serait
    // un mensonge. C'est `WriteChannelFilter` qui écarte tout seul l'index 25 du
    // registre natif, où il n'a pas d'octet.
    if (ro::RoSmallButton(i18n::Tr("Tout cocher"))) {
      for (int i = 0; i < kFilterCount; ++i) {
        channel->filter[i] = 1;
        layout_dirty_      = true;
        WriteChannelFilter(channel->node, i, true);
      }
    }
    ImGui::SameLine();
    if (ro::RoSmallButton(i18n::Tr("Tout décocher"))) {
      for (int i = 0; i < kFilterCount; ++i) {
        channel->filter[i] = 0;
        layout_dirty_      = true;
        WriteChannelFilter(channel->node, i, false);
      }
    }
    ImGui::Separator();
    for (int i = 0; i < kFilterCount; ++i) {
      bool on = channel->filter[i] != 0;
      // Le numéro de type en tête — mais SEULEMENT en mode diagnostic : c'est là
      // qu'il sert (relier une ligne préfixée « t07 » à la case qui la laisse
      // passer). Hors diagnostic, il n'ajouterait que du bruit à une liste que le
      // joueur lit pour son sens, pas pour ses index.
      // L'identifiant, lui, ne bouge PAS avec l'affichage (###chatflt<N>) : basculer
      // le diagnostic ne doit pas réinitialiser l'état des cases.
      char label[96];
      if (diagnostic_)
        std::snprintf(label, sizeof(label), i18n::Tr("t%02d  %s###chatflt%d"), i,
                      chatwnd::TypeLabel(i), i);
      else
        std::snprintf(label, sizeof(label), i18n::Tr("%s###chatflt%d"), chatwnd::TypeLabel(i), i);
      if (ro::RoCheckbox(label, &on)) {
        channel->filter[i] = on ? 1 : 0;
        layout_dirty_      = true;
        WriteChannelFilter(channel->node, i, on);
      }
    }
    PopStyleCompact();
    ImGui::EndMenu();
  }

  // ── Vider CET onglet ────────────────────────────────────────────────────────
  // Sa place est ici, avec les filtres : les deux décident de ce que l'onglet
  // MONTRE. Et surtout pas à côté de « Fermer l'onglet » — deux gestes qui vident
  // l'écran, voisins d'un pixel dans un menu, finissent par se confondre.
  ImGui::Separator();
  if (ImGui::Selectable(i18n::Tr("Vider cet onglet"))) channel->clear_seq = LastLineSeq();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(
        i18n::Tr("Masque ce que cet onglet affiche aujourd'hui ; la suite s'affichera "
                 "normalement.\n\nRien n'est détruit : les autres onglets gardent ces "
                 "lignes, l'historique enregistré aussi,\net « Réafficher tout » les "
                 "ramène ici."));
  // Proposée seulement s'il y a quelque chose à défaire — le geste est réversible,
  // encore faut-il que le joueur le voie, et un menu qui propose toujours d'annuler
  // ne dit plus rien de l'état de l'onglet.
  if (channel->clear_seq != 0) {
    if (ImGui::Selectable(i18n::Tr("Réafficher tout"))) channel->clear_seq = 0;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          i18n::Tr("Annule le vidage : cet onglet remontre tout ce que ses filtres "
                   "acceptent.\n\nSauf ce que l'éviction a évacué entre-temps — le "
                   "tampon ne garde qu'un nombre de lignes borné."));
  }

  ImGui::PopStyleColor();
  ImGui::EndPopup();
  ImGui::PopStyleVar();  // la marge resserrée du menu
}

// ── Envoi ────────────────────────────────────────────────────────────────────
// Entrée dans l'historique de saisie. Deux règles reprises du comportement usuel
// d'une ligne de commande : on n'empile pas deux fois de suite la même chose, et
// le rappel repart TOUJOURS du bas (le joueur vient d'envoyer, sa prochaine flèche
// haut doit lui rendre ce qu'il vient d'écrire, pas là où il en était resté).
void ChatWindow::PushInputHistory(const char* utf8) {
  if (utf8 == nullptr || utf8[0] == '\0') return;
  if (input_history_.empty() || input_history_.back() != utf8)
    input_history_.push_back(utf8);
  while (static_cast<int>(input_history_.size()) > kInputHistoryMax)
    input_history_.erase(input_history_.begin());
  history_index_ = static_cast<int>(input_history_.size());
  input_draft_.clear();
}

// Rappel ↑/↓, calqué sur `UIChatEditCtrl::OnMsg` (msg 18/19). L'index vaut `size()`
// quand on est sur le brouillon — c'est ce qui permet de RESSORTIR de l'historique
// par le bas et de retrouver la phrase commencée, au lieu d'une ligne vide.
void ChatWindow::RecallHistory(int direction, ImGuiInputTextCallbackData* data) {
  const int count = static_cast<int>(input_history_.size());
  if (count == 0) return;
  if (direction < 0) {  // ↑ : vers le passé
    if (history_index_ == count) input_draft_ = data->Buf;  // mise de côté
    if (history_index_ > 0) --history_index_;
  } else {  // ↓ : vers le présent
    if (history_index_ >= count) return;
    ++history_index_;
  }
  const std::string& text =
      (history_index_ >= count) ? input_draft_ : input_history_[history_index_];
  data->DeleteChars(0, data->BufTextLen);
  data->InsertChars(0, text.c_str());
  // Pas de sélection : `InsertChars` laisse le curseur en fin de ligne, donc la
  // phrase rappelée se COMPLÈTE. Une sélection totale la ferait disparaître à la
  // première touche, ce qui est exactement le contraire de l'usage — on rappelle
  // pour corriger une faute de frappe ou changer un mot.
}

// Un destinataire entre dans la liste QUAND ON LUI PARLE, pas quand on tape son
// nom : une frappe à moitié finie n'est pas un interlocuteur. S'il y est déjà, il
// remonte en tête plutôt que d'être dupliqué.
void ChatWindow::PushWhisperHistory(const char* utf8) {
  if (utf8 == nullptr || utf8[0] == '\0') return;
  for (auto it = whisper_history_.begin(); it != whisper_history_.end(); ++it) {
    if (*it == utf8) {
      whisper_history_.erase(it);
      break;
    }
  }
  whisper_history_.insert(whisper_history_.begin(), utf8);
  if (static_cast<int>(whisper_history_.size()) > kWhisperHistoryMax)
    whisper_history_.resize(kWhisperHistoryMax);
}

void ChatWindow::DrawWhisperHistoryPopup() {
  if (!ImGui::BeginPopup("##chat_whisper_hist")) return;
  ImGui::PushStyleColor(ImGuiCol_Text, kDarkText);
  if (whisper_history_.empty()) {
    ImGui::TextUnformatted(i18n::Tr("Aucun destinataire récent"));
  } else {
    for (const std::string& name : whisper_history_) {
      if (ImGui::Selectable(name.c_str())) {
        CopyBounded(whisper_, sizeof(whisper_), name.c_str());
        focus_input_next_ = true;  // le nom est choisi : on veut écrire, pas cliquer
      }
    }
    ImGui::Separator();
    if (ImGui::Selectable(i18n::Tr("Vider la liste"))) whisper_history_.clear();
  }
  ImGui::PopStyleColor();
  ImGui::EndPopup();
}

// ✅ Le drapeau du battle mode est LOCALISÉ (2026-08-04) : `g_BattleModeOn`
// **0x0131F50E**, basculé par la case 135 de `Chat_HandleChatMessage` (`setz`,
// donc une VRAIE bascule — le client ignore un éventuel « on »/« off ») et
// persisté par `OptionInfo_SaveToFile`. On le LIT, on ne le déduit plus : c'est la
// règle du projet, et elle évite deux désaccords que le suivi de commande ne
// savait pas couvrir — la valeur restaurée à la connexion, et une bascule venue
// d'ailleurs que de notre barre de saisie.
// 🔴 POLARITÉ, tranchée par DEUX preuves indépendantes plutôt que par un ressenti :
//
//  1. Le NOM de l'option persistée. `OptionInfo_SaveToFile` (0x00D78D59) l'écrit
//     sous `"ChangeChatMode"` — 1 = mode de chat changé, donc battle mode.
//  2. La LOGIQUE de `UINewChatWnd_ToggleInputBar`. Avec le drapeau à 1 : barre
//     déployée + texte vide ⇒ on referme. Avec le drapeau à 0 : on tombe TOUJOURS
//     dans la branche « ouvre », et rien ne referme jamais sur un texte vide.
//     Or « envoyer à vide referme » EST la définition du battle mode.
//
// 1 = battle mode. Le symptôme d'inversion observé venait d'ailleurs : on SUIVAIT
// la commande en partant de `false`, alors que le client restaure sa valeur depuis
// l'OptionInfo à la connexion. Un joueur déjà en battle mode nous trouvait donc
// inversés dès la première frame — c'est précisément ce que lire le drapeau règle.
bool ChatWindow::ReadNativeBattleMode() const {
  bool battle = false;
  __try {
    battle = *reinterpret_cast<const uint8_t*>(kBattleModeFlag) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    battle = false;  // en cas de doute, barre visible : jamais de chat perdu
  }
  return battle;
}

// ── Liens d'objets (Maj + clic gauche) ───────────────────────────────────────
// Le natif tient DEUX chaînes en parallèle dans l'accessoire `UIItemTagOnChat`
// (`edit+0x144`) : le texte affiché à `+0x128` et le texte résolu à `+0x140`,
// que la touche ENTRÉE préfère à ce que contient l'edit (`WndProc` case 6/0xB8,
// branche `if (accessoire && *(accessoire+0x150))`). On fait la même chose, mais
// en tenant les liens séparément plutôt qu'une deuxième copie de toute la ligne :
// notre saisie reste une simple `char[]` qu'ImGui édite librement.
std::string ChatWindow::ResolveItemLinks(const char* utf8) const {
  if (item_links_.empty()) return utf8 ? utf8 : "";
  const std::string src = utf8 ? utf8 : "";
  std::string out;
  size_t pos = 0;
  for (const PendingLink& link : item_links_) {
    if (link.display.empty()) continue;
    const size_t at = src.find(link.display, pos);
    if (at == std::string::npos) continue;  // effacé depuis la pose : on saute
    out.append(src, pos, at - pos);
    out += link.wire;
    pos = at + link.display.size();
  }
  out.append(src, pos, std::string::npos);
  return out;
}

// Le natif ne met pas du TEXTE dans sa ligne de saisie : il y accroche un vrai
// bouton par lien (`UIItemTagButton`, cf. UIWnd_AppendItemLinkButton), coloré et
// cliquable avant même l'envoi. Un `InputText` ImGui n'a pas de texte riche : la
// seule façon d'y colorer un fragment est de le RECOUVRIR du fond du champ, puis
// de le réécrire dans la couleur des liens — d'où le passage APRÈS la soumission
// du widget, et le fond OPAQUE (le skin de la chatbox pousse des FrameBg pleins,
// 0xCE/0xDE/0xEE : sans opacité, le texte d'origine transparaîtrait dessous).
//
// ⚠ Le champ DÉFILE quand la ligne dépasse sa largeur, et ce décalage n'est nulle
// part ailleurs que dans l'état interne du widget (`ImGuiInputTextState::Scroll`,
// accessible par son id, et qui n'existe QUE tant qu'il est actif — d'où le 0 par
// défaut, qui est justement le décalage d'un champ inactif). L'ignorer collerait
// les pastilles sur du texte qui a glissé, d'autant plus visiblement que la
// saisie est longue — c'est-à-dire précisément quand on pose plusieurs liens.
void ChatWindow::DrawInputLinkChips(const ImVec2& field_pos, float field_w,
                                    float field_h, bool field_active,
                                    bool field_hovered) {
  if (item_links_.empty() || input_[0] == '\0') return;
  const ImGuiStyle& style = ImGui::GetStyle();
  float scroll_x = 0.0f;
  if (ImGuiWindow* win = ImGui::GetCurrentWindow()) {
    if (const ImGuiInputTextState* st =
            ImGui::GetInputTextState(win->GetID("##chat_input")))
      scroll_x = st->Scroll.x;
  }
  const ImU32 bg = ImGui::GetColorU32(field_active    ? ImGuiCol_FrameBgActive
                                      : field_hovered ? ImGuiCol_FrameBgHovered
                                                      : ImGuiCol_FrameBg);
  const float x0 = field_pos.x + style.FramePadding.x - scroll_x;
  const float y0 = field_pos.y + style.FramePadding.y;
  const ImVec2 clip_min(field_pos.x + 1.0f, field_pos.y + 1.0f);
  const ImVec2 clip_max(field_pos.x + field_w - 1.0f, field_pos.y + field_h - 1.0f);
  const bool over_field = ImGui::IsMouseHoveringRect(clip_min, clip_max);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(clip_min, clip_max, true);
  const std::string src = input_;
  size_t from = 0;
  // Même balayage que ResolveItemLinks — de gauche à droite, un lien consommé à
  // la fois : deux exemplaires du même objet gardent chacun leur pastille.
  for (const PendingLink& link : item_links_) {
    if (link.display.empty()) continue;
    const size_t at = src.find(link.display, from);
    if (at == std::string::npos) continue;
    from = at + link.display.size();
    const char* base = src.c_str();
    const float pre = ImGui::CalcTextSize(base, base + at).x;
    const float wdt = ImGui::CalcTextSize(base + at, base + from).x;
    const ImVec2 p(x0 + pre, y0);
    const ImVec2 a(p.x - 1.0f, field_pos.y + 2.0f);
    const ImVec2 b(p.x + wdt + 1.0f, field_pos.y + field_h - 2.0f);
    dl->AddRectFilled(a, b, bg);  // efface le texte que le champ vient d'écrire
    dl->AddText(p, kLinkCol, base + at, base + from);
    // Mêmes gestes que dans le log — et c'est le même code qui les joue. Le
    // survol, lui, est à nous : ces pastilles sont peintes à la main par-dessus
    // un `InputText`, ce ne sont pas des items ImGui.
    const bool hovered = over_field && ImGui::IsMouseHoveringRect(a, b);
    const links::Target target = TargetOf(link);
    if (hovered) links::HoverPreview(target);
    if (links::Gestures(target, hovered)) {
      link_menu_ = target;
      ImGui::OpenPopup("##chat_link_menu_input");
    }
  }
  dl->PopClipRect();
}

// Ce qu'un fragment du log DÉSIGNE, dans le vocabulaire commun des liens
// (features/link_gesture.h). La chatbox n'a plus ni gestes ni menu à elle : elle
// dit ce qu'elle montre, le module fait le reste.
links::Target ChatWindow::TargetOf(const Run& run) const {
  switch (run.kind) {
    case Run::kItem: return links::FromItem(run.item, run.text.c_str());
    case Run::kRecipe: return links::FromRecipe(run.item_id, run.text.c_str());
    case Run::kMob:
      return links::FromMob(run.mob_id, run.mob_rank, run.mob_name.c_str());
    case Run::kUrl: return links::FromUrl(run.url.c_str());
    case Run::kPlayer: return links::FromPlayer(run.text.c_str());
    case Run::kSetting: return links::FromSetting(run.setting_key.c_str());
    default: return links::Target{};
  }
}

links::Target ChatWindow::TargetOf(const PendingLink& link) const {
  if (link.kind == Run::kRecipe) {
    links::Target t = links::FromRecipe(link.item.id, link.display.c_str());
    t.label = link.display;  // le libellé POSÉ, celui que le joueur a sous les yeux
    return t;
  }
  if (link.kind == Run::kMob) {
    links::Target t = links::FromMob(link.mob_id, link.mob_rank, link.mob_name.c_str());
    t.label = link.display;  // le libellé POSÉ, celui que le joueur a sous les yeux
    return t;
  }
  if (link.kind == Run::kSetting) return links::FromSetting(link.setting_key.c_str());
  return links::FromItem(link.item, link.display.c_str());
}

// 🔴 Armer, jamais envoyer : `NativeSendChatText` est un appel natif, proscrit
// pendant une frame ImGui (cf. l'en-tête). FlushPending, lui, tourne depuis
// OnProcessInput.
void ChatWindow::QueueCommand(const char* utf8) {
  if (utf8 == nullptr || utf8[0] == '\0') return;
  pending_text_ = ro::Utf8ToWireText(utf8);
  pending_whisper_.clear();
  has_pending_ = true;
}

void ChatWindow::QueueNameAction(NameAction action, const char* name_wire) {
  if (action == NameAction::kNone || name_wire == nullptr || name_wire[0] == '\0')
    return;
  pending_name_        = name_wire;
  pending_name_action_ = action;
}

// Joue l'action armée. Appelée par FlushPending, donc hors frame ImGui.
void ChatWindow::FlushNameAction() {
  const NameAction action = pending_name_action_;
  if (action == NameAction::kNone) return;
  pending_name_action_ = NameAction::kNone;
  // Le champ de nom des trois paquets fait 24 octets, non terminés par
  // convention : on part d'un tampon zéro et on tronque, plutôt que de laisser
  // filer ce qui traînait derrière une chaîne plus courte.
  char name[kNameFieldLen] = {};
  CopyBounded(name, sizeof(name), pending_name_.c_str());
  pending_name_.clear();

  switch (action) {
    case NameAction::kPartyInvite:
      // 🔴 Chemin NATIF, et c'est délibéré : `clif_parse_PartyInvite2` est un
      // paquet SHUFFLE côté serveur (0x02c4, 0x088d, 0x0929, 0x091c, 0x0802,
      // 0x086d selon la version). Coder l'opcode en dur, c'est parier sur la
      // version ; laisser le client le choisir, c'est avoir raison par
      // construction. C'est exactement ce que fait son menu contextuel (code 5).
      ModeSendMsg(kMsgPartyInvite,
                  static_cast<int>(reinterpret_cast<intptr_t>(name)));
      return;
    case NameAction::kFriendAdd: {
      // Le client a sa propre fonction pour ça, et elle prend un NOM : elle
      // construit `CZ_ADD_FRIENDS` (0x0202, désassemblé à 0x00a2c600 — `Src =
      // 514`, 24 octets de nom, longueur via sa table) et l'envoie. Passer par
      // elle évite de trancher entre les cinq opcodes que le serveur accepte.
      // ⚠ Elle lit 24 octets d'affilée : le tampon doit les avoir.
      // Message de static_assert : un LITTÉRAL, jamais i18n::Tr — il est lu à la
      // compilation, et il s'adresse au développeur, pas au joueur.
      static_assert(sizeof(name) >= 24, "sub_A2C600 lit 24 octets de nom");
      __try {
        reinterpret_cast<FriendAddFn>(kFriendListAddByName)(name);
      } __except (EXCEPTION_EXECUTE_HANDLER) {
      }
      return;
    }
    case NameAction::kGuildInvite: {
      // Pas d'équivalent natif par nom — le menu du client n'invite que par AID,
      // qu'une ligne de chat ne porte pas. `CZ_REQ_JOIN_GUILD2` (0x0916) est en
      // revanche enregistré HORS des blocs shuffle du serveur (clif_packetdb.hpp
      // :1538), donc son opcode ne dépend pas de la version.
      uint8_t packet[2 + kNameFieldLen] = {};
      *reinterpret_cast<uint16_t*>(packet) = kOpGuildInviteByName;
      std::memcpy(packet + 2, name, kNameFieldLen);
      Bourgeon::Instance().SendPacket(packet, sizeof(packet));
      return;
    }
    case NameAction::kNone:
      return;
  }
}

// 🔴 Voir l'en-tête : sans ce rappel, le lien posé pendant que la saisie a le
// focus est écrasé par l'état interne du widget à la frame suivante — il n'aura
// jamais été affiché. `MoveToEnd` parce qu'on AJOUTE en fin de ligne : c'est là
// que le curseur doit se retrouver pour continuer à écrire.
void ChatWindow::NotifyInputEdited() {
  if (input_field_id_ == 0) return;
  // Rend nullptr quand le champ n'est pas actif — c'est-à-dire quand il n'y a
  // justement rien à resynchroniser.
  if (ImGuiInputTextState* state = ImGui::GetInputTextState(input_field_id_))
    state->ReloadUserBufAndMoveToEnd();
}

void ChatWindow::NotifyWhisperEdited() {
  if (whisper_field_id_ == 0) return;
  if (ImGuiInputTextState* state = ImGui::GetInputTextState(whisper_field_id_))
    state->ReloadUserBufAndMoveToEnd();
}

void ChatWindow::PruneItemLinks() {
  if (item_links_.empty()) return;
  const std::string src = input_;
  std::vector<PendingLink> kept;
  size_t pos = 0;
  for (PendingLink& link : item_links_) {
    if (link.display.empty()) continue;
    const size_t at = src.find(link.display, pos);
    if (at == std::string::npos) continue;
    pos = at + link.display.size();
    kept.push_back(std::move(link));
  }
  item_links_.swap(kept);
}

bool ChatWindow::AppendItemLink(void* info) {
  if (!imgui_enabled_ || !input_bar_ || info == nullptr) return false;

  char wire[192];
  if (!itemcell::BuildChatLink(info, wire, sizeof(wire))) return false;

  // 🔴 Le libellé posé est composé DEPUIS LA BALISE, pas depuis l'item : on relit
  // ce qu'on vient d'écrire et on le rend exactement comme le fera le log une
  // fois la ligne partie. C'est la seule façon de garantir que ce que le joueur
  // voit AVANT l'envoi est ce que tout le monde verra APRÈS.
  itemcell::ChatLink link;
  if (!itemcell::ParseChatLink(wire, wire + std::strlen(wire), &link)) return false;
  char name[192];
  itemcell::BuildChatLinkName(link, name, sizeof(name));
  if (name[0] == '\0') return false;
  // Format du natif, chevrons compris : `<+7 Sword [3]>` (le compte
  // d'emplacements est déjà dans le nom composé).
  // (⚠ `WireToUtf8` rend un `const char*` : concaténer avec des littéraux, c'est
  //  de l'arithmétique de pointeurs — on passe par la chaîne.)
  std::string display = "<";
  display += ro::WireToUtf8(name);
  display += ">";
  if (display.size() <= 2) return false;  // nom vide : rien à poser

  PruneItemLinks();
  if (static_cast<int>(item_links_.size()) >= kMaxItemLinks) return false;

  // Un séparateur entre deux liens collés : sans lui, deux noms bout à bout ne se
  // distinguent plus à la lecture, et la substitution retrouverait le second au
  // milieu du premier si l'un est le préfixe de l'autre.
  const size_t used = std::strlen(input_);
  std::string insert = display;
  insert += ' ';
  if (used + insert.size() + 1 > sizeof(input_)) return false;
  std::memcpy(input_ + used, insert.c_str(), insert.size() + 1);
  NotifyInputEdited();  // 🔴 sinon le champ ACTIF réécrit son propre texte par-dessus

  PendingLink pending;
  pending.wire    = wire;
  pending.item    = link;  // la balise relue : même source que le libellé ci-dessus
  pending.display = display;
  item_links_.push_back(std::move(pending));
  // Le geste vient d'une AUTRE fenêtre (inventaire, fiche de personnage) : sans
  // ça, le joueur devrait encore cliquer dans la barre avant de pouvoir taper.
  if (battle_mode_) input_open_ = true;
  focus_input_next_ = true;
  return true;
}

// RELAYER un lien d'objet : le reposer dans la saisie sans posséder l'objet. La
// balise est ré-encodée depuis le lien relu, donc refine, cartes, grade, options
// et forgeron survivent au relais — c'est bien le MÊME objet qu'on repasse, pas
// l'item de base qui porte son nom.
bool ChatWindow::AppendItemLinkFromLink(const itemcell::ChatLink& link) {
  if (!imgui_enabled_ || !input_bar_ || link.id == 0) return false;

  // 🔴 L'objet est-il en sac ? Le client REFUSE d'envoyer un `<ITEML>` qui n'y est
  // pas — la ligne entière est bloquée à l'envoi, avec un « Item tags can only tag
  // items you own. » en rouge. On bascule donc sur notre propre balise plutôt que
  // de laisser le geste échouer : le lien part, il est rendu chez tout joueur
  // Bourgeon, et il reste lisible ailleurs.
  //
  // Le test porte sur le MODÈLE SESSION (même liste que les viewers), pas sur une
  // fenêtre : cacher l'inventaire natif vide la seconde, jamais le premier.
  if (itemcell::FindInfoById(kInvListHead, link.id) == nullptr)
    return AppendItemRefLink(link.id, itemcell::NameById(link.id));

  char wire[192];
  if (!itemcell::BuildChatLinkFromLink(link, wire, sizeof(wire))) return false;
  char name[192];
  itemcell::BuildChatLinkName(link, name, sizeof(name));
  if (name[0] == '\0') return false;
  std::string display = "<";
  display += ro::WireToUtf8(name);
  display += ">";
  if (display.size() <= 2) return false;

  PruneItemLinks();
  if (static_cast<int>(item_links_.size()) >= kMaxItemLinks) return false;

  const size_t used = std::strlen(input_);
  std::string insert = display;
  insert += ' ';
  if (used + insert.size() + 1 > sizeof(input_)) return false;
  std::memcpy(input_ + used, insert.c_str(), insert.size() + 1);
  NotifyInputEdited();  // 🔴 sinon le champ ACTIF réécrit son propre texte par-dessus

  PendingLink pending;
  pending.wire    = wire;
  pending.item    = link;
  pending.display = display;
  pending.kind    = Run::kItem;
  item_links_.push_back(std::move(pending));
  if (battle_mode_) input_open_ = true;
  focus_input_next_ = true;
  return true;
}

// Poser une RÉFÉRENCE d'objet — l'objet de base, sans instance. C'est le chemin
// des objets qu'on ne possède PAS, que le client refuse de taguer en `<ITEML>`
// (cf. AppendItemLinkFromLink et le décodeur de `<ITMR>`).
//
// ⚠ Le nom part DANS la balise : c'est ce qui garde la ligne lisible chez un
// joueur sans Bourgeon, qui verra la balise brute. Un nom porteur de `<` ou `>`
// la couperait en deux à la relecture — on refuse alors plutôt que d'émettre une
// balise qui se relira de travers.
bool ChatWindow::AppendItemRefLink(uint32_t item_id, const char* name_utf8) {
  if (!imgui_enabled_ || !input_bar_ || item_id == 0) return false;
  const std::string name = (name_utf8 && name_utf8[0]) ? name_utf8
                                                       : itemcell::NameById(item_id);
  if (name.empty()) return false;
  if (name.find('<') != std::string::npos || name.find('>') != std::string::npos)
    return false;

  PruneItemLinks();
  if (static_cast<int>(item_links_.size()) >= kMaxItemLinks) return false;

  const std::string display = "<" + name + ">";
  char wire[256];
  std::snprintf(wire, sizeof(wire), "<ITMR>%u:%s</ITMR>", item_id, name.c_str());

  const size_t used = std::strlen(input_);
  std::string insert = display;
  insert += ' ';
  if (used + insert.size() + 1 > sizeof(input_)) return false;
  std::memcpy(input_ + used, insert.c_str(), insert.size() + 1);
  NotifyInputEdited();  // 🔴 sinon le champ ACTIF réécrit son propre texte par-dessus

  PendingLink pending;
  pending.wire    = wire;
  pending.display = display;
  pending.kind    = Run::kItem;
  pending.item.id = item_id;
  item_links_.push_back(std::move(pending));
  if (battle_mode_) input_open_ = true;
  focus_input_next_ = true;
  return true;
}

// Poser le lien d'une RECETTE — « [Recette: Acid Bottle] ». Ce n'est pas un lien
// d'objet : il désigne la façon de le FAIRE, et c'est ce qu'on veut poster quand
// on explique une fabrication à quelqu'un.
//
// 🔴 Refusé si l'objet n'a pas de recette. Poser un lien qui ouvrirait une fiche
// vide ferait perdre son temps au lecteur, et à l'expéditeur sa crédibilité.
bool ChatWindow::AppendRecipeLink(uint32_t item_id, const char* name_utf8) {
  if (!imgui_enabled_ || !input_bar_ || item_id == 0) return false;
  if (craftdata::RecipeOf(item_id) == nullptr) return false;

  const std::string name = (name_utf8 && name_utf8[0]) ? name_utf8
                                                       : itemcell::NameById(item_id);
  if (name.empty()) return false;
  // Un nom porteur de chevrons couperait la balise en deux à la relecture.
  if (name.find('<') != std::string::npos || name.find('>') != std::string::npos)
    return false;

  PruneItemLinks();
  if (static_cast<int>(item_links_.size()) >= kMaxItemLinks) return false;

  const std::string display = RecipeLinkLabel(name);
  char wire[256];
  std::snprintf(wire, sizeof(wire), "<CRAF>%u:%s</CRAF>", item_id, name.c_str());

  const size_t used = std::strlen(input_);
  std::string insert = display;
  insert += ' ';
  if (used + insert.size() + 1 > sizeof(input_)) return false;
  std::memcpy(input_ + used, insert.c_str(), insert.size() + 1);
  NotifyInputEdited();  // 🔴 sinon le champ ACTIF réécrit son propre texte par-dessus

  PendingLink pending;
  pending.wire    = wire;
  pending.display = display;
  pending.kind    = Run::kRecipe;
  pending.item.id = item_id;
  item_links_.push_back(std::move(pending));
  if (battle_mode_) input_open_ = true;
  focus_input_next_ = true;
  return true;
}

// Poser le lien d'une DESTINATION DE RÉGLAGES — « [Réglage: Objet obtenu] ». Même
// mécanique que les autres : le libellé lisible dans la saisie, la balise mise de
// côté et substituée à l'envoi.
//
// 🔴 C'est la CLÉ qui part sur le fil, jamais un numéro de section — un numéro
// décrit l'ordre d'une version de Bourgeon, et le lecteur atterrirait sur la
// section d'à côté à la première insertion (cf. iface::DestLabel). Le libellé
// voyage avec, uniquement pour rester lisible chez qui n'a pas Bourgeon : à
// l'affichage c'est le libellé LOCAL, donc traduit, qui gagne.
bool ChatWindow::AppendSettingLink(const char* key) {
  if (!imgui_enabled_ || !input_bar_ || key == nullptr) return false;
  const char* label = iface::DestLabel(key);
  if (label == nullptr) return false;

  PruneItemLinks();
  if (static_cast<int>(item_links_.size()) >= kMaxItemLinks) return false;

  const std::string display = links::SettingLabel(key);
  if (display.empty()) return false;
  char wire[192];
  // Le libellé de repli part NON TRADUIT : c'est le nom de la destination dans la
  // langue de référence du projet, celui qui a le plus de chances de parler au
  // lecteur d'un client sans Bourgeon — et il ne dépend ainsi pas de la langue
  // dans laquelle l'expéditeur joue.
  std::snprintf(wire, sizeof(wire), "<SETL>%s:%s</SETL>", key, label);

  const size_t used = std::strlen(input_);
  std::string insert = display;
  insert += ' ';
  if (used + insert.size() + 1 > sizeof(input_)) return false;
  std::memcpy(input_ + used, insert.c_str(), insert.size() + 1);
  NotifyInputEdited();  // 🔴 sinon le champ ACTIF réécrit son propre texte par-dessus

  PendingLink pending;
  pending.wire        = wire;
  pending.display     = display;
  pending.kind        = Run::kSetting;
  pending.setting_key = key;
  item_links_.push_back(std::move(pending));
  if (battle_mode_) input_open_ = true;
  focus_input_next_ = true;
  return true;
}

// Poser le lien d'un MONSTRE. Même mécanique que pour un objet : le libellé
// lisible dans la saisie, la balise mise de côté et substituée à l'envoi.
//
// ⚠ Le nom vient de l'APPELANT et repart tel quel dans la balise. C'est voulu :
// le client est incapable de nommer un monstre — ni mob_db ni le paquet de la
// fiche ne le lui donnent — donc seul celui qui affiche déjà le nom (la fiche,
// la table des drops) peut le fournir, et il doit voyager avec le lien.
bool ChatWindow::AppendMobLink(uint32_t mob_id, int rank, const char* name_utf8) {
  if (!imgui_enabled_ || !input_bar_) return false;
  if (mob_id == 0 || name_utf8 == nullptr || name_utf8[0] == '\0') return false;
  if (rank < 0 || rank > 2) rank = 0;
  // Un nom qui porterait la fin de balise la couperait en deux à la relecture ;
  // le reste (espaces, apostrophes, ponctuation) est libre puisque le nom est le
  // DERNIER champ.
  const std::string name(name_utf8);
  if (name.find('<') != std::string::npos || name.find('>') != std::string::npos)
    return false;

  PruneItemLinks();
  if (static_cast<int>(item_links_.size()) >= kMaxItemLinks) return false;

  const std::string display =
      "<" + std::string(MobRankTag(static_cast<uint8_t>(rank))) + " " + name + ">";
  char wire[256];
  std::snprintf(wire, sizeof(wire), "<MOBL>%u:%d:%s</MOBL>", mob_id, rank,
                name.c_str());

  const size_t used = std::strlen(input_);
  std::string insert = display;
  insert += ' ';
  if (used + insert.size() + 1 > sizeof(input_)) return false;
  std::memcpy(input_ + used, insert.c_str(), insert.size() + 1);
  NotifyInputEdited();  // 🔴 sinon le champ ACTIF réécrit son propre texte par-dessus

  PendingLink pending;
  pending.wire     = wire;
  pending.display  = display;
  pending.kind     = Run::kMob;
  pending.mob_id   = mob_id;
  pending.mob_rank = static_cast<uint8_t>(rank);
  pending.mob_name = name;
  item_links_.push_back(std::move(pending));
  if (battle_mode_) input_open_ = true;
  focus_input_next_ = true;
  return true;
}

void ChatWindow::QueueSend() {
  if (input_[0] == '\0') return;
  PushInputHistory(input_);
  // Même condition que le chemin d'envoi : une ligne qui commence par `/` est une
  // commande, la box destinataire n'est PAS consultée — enregistrer le nom alors
  // qu'on n'a chuchoté à personne salirait la liste.
  if (input_[0] != '/') PushWhisperHistory(whisper_);
  // Les liens d'objets reprennent leur forme longue AVANT la conversion : le
  // `<ITEML>` est de l'ASCII pur, il traverse `Utf8ToWire` inchangé, alors que le
  // NOM qu'il remplace, lui, ne survivrait pas forcément à l'aller-retour.
  // L'historique de saisie, lui, garde la version LISIBLE : c'est ce que le
  // joueur a écrit, et c'est ce qu'il veut retrouver à la flèche du haut.
  const std::string resolved = ResolveItemLinks(input_);
  item_links_.clear();
  // La saisie ImGui est en UTF-8 ; le fil, lui, est en 1252 — sauf quand la
  // phrase contient quelque chose que 1252 ne sait pas écrire (un emoji), auquel
  // cas elle part en UTF-8 (cf. ro::Utf8ToWireText). On convertit ICI, pas au
  // moment de l'envoi : FlushPending tourne hors frame et ne doit plus faire que
  // des appels natifs.
  pending_text_    = ro::Utf8ToWireText(resolved.c_str());
  pending_whisper_ = ro::Utf8ToWire(whisper_);
  has_pending_     = true;
  input_[0] = '\0';
}

// 🔴 DÉTRUIRE, pas masquer — la règle du projet pour toute native remplacée. Le
// destructeur de la chatbox (`UINewChatWnd_dtor 0x008db7f0`) n'émet AUCUN paquet ;
// il n'écrit que `ChatWndInfo_U.lua`, ce qui est sans conséquence. La fenêtre 0x84
// (options de log du natif) part avec elle : notre menu contextuel la remplace.
//
// Appelée depuis `OnProcessInput`, donc hors de toute frame ImGui : une commande
// native jouée pendant le rendu peut ouvrir une modale bloquante qui relance le
// rendu — le gel muet classique.
void ChatWindow::SuppressNativeChat() {
  constexpr int kNativeChatWndId   = 1;
  constexpr int kNativeLogOptWndId = 0x84;
  // Rien en dehors du jeu : à l'écran de login ou pendant un chargement de carte,
  // ni détruire ni recréer n'a de sens, et créer une fenêtre pendant un warp est
  // précisément ce qui a valu au projet une famille de crashes.
  const Bourgeon& app = Bourgeon::Instance();
  if (!app.IsGameActive() || app.IsMapLoading()) return;

  const bool native_alive = uiwnd::SafeFindWindow(kNativeChatWndId) != nullptr;
  if (imgui_enabled_) {
    if (native_alive) uiwnd::CloseWindow(kNativeChatWndId);
    if (uiwnd::SafeFindWindow(kNativeLogOptWndId) != nullptr)
      uiwnd::CloseWindow(kNativeLogOptWndId);
    return;
  }
  // 🔴 Bascule INVERSE : éteindre l'interface moderne doit RENDRE la chatbox
  // native, pas laisser le joueur sans aucun chat. Le client ne la recrée que
  // lors du « case 0 » (entrée en jeu) — sans ce rappel, il faudrait se
  // reconnecter pour la revoir. La file `mgr+0x4C4` se draine d'elle-même à la
  // création : les lignes accumulées entre-temps réapparaissent.
  if (!native_alive) uiwnd::MakeWindow(kNativeChatWndId);
}

bool ChatWindow::WantsTypedKeys() const {
  // `!WantTextInput` : si une zone de texte écrit déjà — la nôtre une fois
  // focalisée, ou celle d'une autre fenêtre — elle reçoit les caractères par le
  // chemin normal d'ImGui et nous n'avons rien à faire. Le prédicat ne vaut que
  // pour l'état intermédiaire : barre ouverte, personne au clavier.
  return imgui_enabled_ && battle_mode_ && InputRowVisible() &&
         !ImGui::GetIO().WantTextInput;
}

// La grille d'emotes/emoji est-elle à l'écran ? Lu depuis le WndProc, donc ENTRE
// deux frames : d'où la tolérance d'une frame, sans laquelle le prédicat serait
// faux une fois sur deux.
bool ChatWindow::PickerOpen() const {
  if (picker_open_frame_ < 0 || ImGui::GetCurrentContext() == nullptr)
    return false;
  return ImGui::GetFrameCount() - picker_open_frame_ <= 1;
}

// Cf. l'en-tête pour le POURQUOI. Ici, le seul point délicat est l'ordre des
// tests : le TEXTE d'abord, parce qu'un lien fraîchement posé arme la barre à
// l'instant même où le geste s'achève — bien avant que le focus demandé ne se
// soit matérialisé (`SetKeyboardFocusHere` est refusé tant que le bouton de la
// souris est enfoncé, cf. DrawInputRow). Attendre le focus, c'est laisser une
// fenêtre de plusieurs frames où le dialogue NPC reprendrait la touche.
bool ChatWindow::OwnsEnterKey() const {
  if (!imgui_enabled_ || !InputRowVisible()) return false;
  // 🔴 EN BATTLE MODE, LE DÉPLIEMENT SUFFIT : la barre n'est là que parce que le
  // joueur l'a ouverte, et Entrée est justement ce qui la referme quand elle est
  // vide. La laisser filer au dialogue, c'est un geste pour deux effets — la barre
  // se refermait ET le script avançait d'une page. (`InputRowVisible` a déjà
  // vérifié `input_open_` : en battle mode elle ne vaut que dépliée.)
  if (battle_mode_) return true;
  // Hors battle mode la barre est là en PERMANENCE : son existence ne prouve donc
  // aucune intention, et il faut un signe — du texte, ou le clavier.
  if (input_[0] != '\0') return true;  // un lien posé, une phrase en cours
  // Focus EN VOL : la demande est posée, le champ s'active à la frame suivante.
  if (focus_input_next_ || focus_whisper_next_) return true;
  if (input_focus_frame_ < 0 || ImGui::GetCurrentContext() == nullptr) return false;
  return ImGui::GetFrameCount() - input_focus_frame_ <= 1;
}

bool ChatWindow::WantsEscapeKey() const {
  if (!imgui_enabled_) return false;
  // 🔴 La grille ouverte confisque Échap À ELLE SEULE. Sans cette ligne, la
  // touche partirait au JEU dès que la barre de saisie est fermée (ou hors
  // battle mode) et ouvrirait son menu — alors que le joueur voulait juste
  // refermer la palette.
  if (PickerOpen()) return true;
  // La ligne de saisie doit être RÉELLEMENT à l'écran : coupée dans les réglages,
  // rien ne se refermerait et la touche serait confisquée pour rien.
  return battle_mode_ && InputRowVisible();
}

void ChatWindow::OnRawKey(unsigned long vkey) {
  if (!imgui_enabled_) return;
  if (vkey == VK_RETURN) enter_pending_ = true;
  // 🔴 ÉCHAP AUSSI, et pas seulement dans le champ. Une barre ouverte peut ne pas
  // avoir le clavier — c'est même le cas dès qu'on a cliqué ailleurs — et aucun
  // widget ne verrait alors la touche. C'est LA sortie à une frappe, celle qui
  // permet à la saisie de rendre le clavier à ImGui sans piéger le joueur.
  if (vkey == VK_ESCAPE) {
    // 🔴 UNE SEULE CHOSE À LA FOIS. La grille ouverte prend la touche pour elle
    // et s'arrête là : sinon le même Échap refermait la palette ET la barre de
    // saisie derrière, et le joueur qui voulait renoncer à une emote se
    // retrouvait sans chat, à retaper sa phrase.
    if (PickerOpen()) {
      picker_close_ = true;
      return;
    }
    escape_pending_ = true;
  }
}

// Le chemin ORDINAIRE, quand la touche a atteint le jeu. Il ne suffit pas à lui
// seul : cf. `OnRawKey`, que le WndProc appelle pour celles qu'il nous confisque.
// Poser deux fois le même drapeau est sans effet — il n'est consommé qu'une fois,
// à la frame suivante.
void ChatWindow::OnKeyDown(unsigned long vkey, int, int) { OnRawKey(vkey); }

void ChatWindow::FlushPending() {
  SuppressNativeChat();
  // Ici et pas dans la frame : interroger le dictionnaire de noms peut faire
  // ÉMETTRE une requête au client (cf. ResolveWhisperGuilds).
  ResolveWhisperGuilds();
  FlushNameAction();
  if (!has_pending_) return;
  has_pending_ = false;
  const char* error =
      NativeSendChatText(pending_text_.c_str(), pending_whisper_.c_str());
  pending_text_.clear();
  pending_whisper_.clear();
  // Un refus (mot interdit) s'affiche dans NOTRE chat : le natif ouvrirait une
  // modale bloquante, qui relance le rendu du mode courant — proscrit ici.
  if (error != nullptr) {
    Line line;
    // COLORREF, comme toute couleur de ligne ici : 0x6060FF = rouge clair
    // (R=255, G=96, B=96), pas l'inverse.
    line.rgb = 0x6060FF;
    Run run;
    run.text = error;
    line.runs.push_back(run);
    line.plain = run.text;
    SYSTEMTIME now;
    GetLocalTime(&now);
    line.hour   = static_cast<uint8_t>(now.wHour);
    line.minute = static_cast<uint8_t>(now.wMinute);
    line.second = static_cast<uint8_t>(now.wSecond);
    std::lock_guard<std::mutex> lock(lines_mutex_);
    lines_.push_back(std::move(line));
    TrimLines();
  }
}

// ── Réglages ─────────────────────────────────────────────────────────────────
bool ChatWindow::DrawSettings() {
  bool changed = false;
  // ⚠ Suffixes « ###chatwnd_… » sur TOUS les libellés : cette section est dessinée
  // dans la MÊME fenêtre ImGui que celle de ChatTweaks (le chat natif), qui a
  // ses propres « Icônes d'objets » et son horodatage. Deux widgets de même
  // libellé dans une même fenêtre, c'est le même ID — ImGui le signale par une
  // fenêtre d'erreur rouge, et l'un des deux devient inutilisable.
  changed |= ro::RoCheckbox(i18n::Tr("Chatbox ImGui###chatwnd_on"), &imgui_enabled_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Remplacement de la chatbox : mêmes canaux, mêmes filtres et même chemin "
      "d'envoi que le client. La fenêtre native reste ouverte à côté tant que la "
      "bascule complète n'est pas faite."));
  changed |= ro::RoCheckbox(i18n::Tr("Ligne de saisie###chatwnd_input"), &input_bar_);
  // ⚠ Le verrouillage de la géométrie N'EST PLUS ICI : il y en a un par fenêtre,
  // et ce panneau ne sait pas de laquelle il parlerait. Il vit dans le menu
  // contextuel d'un onglet ou d'un en-tête — cf. DrawLogOptionsPopup.
  changed |= ro::RoCheckbox(i18n::Tr("Avertir avant d'ouvrir un lien###chatwnd_urlwarn"),
                            &url_confirm_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Une adresse postée dans le chat vient d'un autre joueur, et le texte "
      "affiché n'a aucun rapport obligé avec la destination réelle. La "
      "confirmation montre l'adresse COMPLÈTE avant d'ouvrir le navigateur.\n\n"
      "En la décochant, un clic sur un lien ouvre directement le navigateur."));
  changed |= ro::RoCheckbox(i18n::Tr("Aperçu des images au survol###chatwnd_urlprev"),
                            &url_preview_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Survoler un lien d'image en montre le contenu, sans ouvrir le "
      "navigateur.\n\n"
      "Les images ne sont chargées que depuis des hébergeurs connus (Discord, "
      "imgur, le site Moonlight). C'est ce qui rend l'aperçu sûr : sur ces "
      "serveurs-là, celui qui a posté le lien ne peut pas savoir qui l'a "
      "regardé. Un lien vers n'importe quel autre site reste un lien "
      "ordinaire, à ouvrir soi-même."));

  // 🔴 Une liste qu'on ne peut pas INSPECTER ni DÉFAIRE n'aurait pas dû exister.
  // Le joueur accorde depuis le menu contextuel d'un lien ; c'est ici qu'il voit
  // ce qu'il a accordé, et qu'il le reprend.
  // ⚠ PAS derrière `url_preview_` : on doit pouvoir REPRENDRE un accord même
  // après avoir éteint l'aperçu, sinon éteindre le réglage enfermerait le joueur
  // avec une liste qu'il ne peut plus défaire. La liste ne s'affiche donc que si
  // elle n'est pas vide — discrète pour qui n'a jamais rien accordé.
  {
    const std::vector<std::string> hosts = imgprev::UserHosts();
    if (!hosts.empty()) {
      ImGui::TextDisabled(i18n::Tr("  Vos sites autorisés :"));
      for (const std::string& h : hosts) {
        char rm[96];
        std::snprintf(rm, sizeof(rm), i18n::Tr("Retirer###chatwnd_rmhost_%s"), h.c_str());
        ImGui::Bullet();
        ImGui::TextUnformatted(h.c_str());
        ImGui::SameLine();
        if (ro::RoSmallButton(rm)) {
          imgprev::ForgetHost(h.c_str());
          url_hosts_      = imgprev::UserHostsCsv();
          url_hosts_seen_ = url_hosts_;  // déjà appliqué : pas de resynchro
          changed = true;
        }
      }
    }
  }
  // Famille de police du log. Les familles sont bakées au démarrage : le choix
  // s'applique donc immédiatement, sans redémarrage.
  ImGui::SetNextItemWidth(160.0f);
  if (ro::RoBeginCombo(i18n::Tr("Police###chatwnd_family"),
                       ro::ChatFamilyLabel(font_family_))) {
    for (int f = 0; f < ro::ChatFamilyCount(); ++f) {
      // On n'offre que ce qui a VRAIMENT été chargé : proposer une famille
      // absente du système donnerait un choix sans effet, donc une panne.
      if (f != 0 && ro::ChatFamilyFont(f, false, false) == nullptr) continue;
      if (ImGui::Selectable(ro::ChatFamilyLabel(f), font_family_ == f)) {
        font_family_ = f;
        changed = true;
      }
    }
    ro::RoEndCombo();
  }
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Police du fil de discussion. « Système » garde celle du reste de "
      "l'interface.\n\n"
      "Les autres sont latines : un caractère coréen y apparaîtrait en carré. "
      "Sans effet en jeu, où tout est en français ou en anglais."));

  // ── Vignettes : la case dit S'IL Y EN A, le curseur dit LAQUELLE ───────────
  // Les deux étaient confondus dans un seul curseur où zéro valait « aucune ».
  // Une commande qui répond à deux questions se lit mal : on ne sait plus si
  // l'on règle une taille ou si l'on éteint quelque chose.
  changed |= ro::RoCheckbox(i18n::Tr("Images et emotes###chatwnd_thumbs"), &thumbs_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Affiche les images et les emotes du fil sous forme de vignettes.\n\n"
      "Décoché, un lien reste une adresse cliquable et une emote son "
      "« :nom: » — et rien n'est téléchargé."));
  // Curseur inactif tant que la case est décochée : il reste VISIBLE, donc on
  // voit la taille qui s'appliquera, mais il n'invite pas à régler ce qui ne
  // s'affiche pas.
  ImGui::BeginDisabled(!thumbs_);
  changed |= WheelSliderInt(i18n::Tr("Taille des images###chatwnd_thumb"), &thumb_px_,
                            24, 128, "%d px");
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Hauteur des vignettes, de 24 à 128 pixels.\n\n"
      "Au-delà d'une hauteur de ligne, la ligne s'agrandit pour accueillir "
      "l'image : le fil reste lisible, il s'aère."));
  // ── Outil ponctuel, sur demande explicite ─────────────────────────────────
  // L'export ÉCRIT des dizaines de fichiers sur le disque : c'est une action, pas
  // un affichage, donc elle s'active plutôt qu'elle ne se cache. Le bouton reste
  // introuvable tant que cette case est décochée — on ne le déclenche pas d'un
  // clic distrait en cherchant une emote.
  //
  // Live, non persisté, comme les autres réglages fins du staff : c'est un outil
  // qu'on ouvre pour une manipulation puis qu'on referme.
  if (IsStaff()) {
    ro::RoCheckbox(i18n::Tr("Export des emotes en GIF###chatwnd_emote_export"),
                   &emote_export_);
    ImGui::SameLine();
    HelpMarker(
        i18n::Tr("Fait apparaître un bouton au bas de la grille d'emotes.\n\n"
        "Il écrit un GIF par emote dans « emotes_export », à côté de "
        "l'exécutable, sous le nom que le relais Discord attend. À déposer "
        "ensuite dans images/smilies/ du site.\n\n"
        "Réservé au staff, et éteint à chaque session : c'est une manipulation "
        "ponctuelle, pas un réglage."));
  }
  // ── Conversations privées ─────────────────────────────────────────────────
  // 🔴 Ces deux cases ne sont PAS à nous : elles écrivent directement les
  // octets du client, ceux de son « Friend Setup » (Alt+I). Une copie persistée
  // de notre côté ferait deux sources de vérité pour un même réglage, qui se
  // contrediraient dès que le joueur toucherait l'autre fenêtre. Elles sont ici
  // parce que la native qui les portait n'est pas toujours sous la main, et
  // qu'une fenêtre qui ne s'ouvre jamais sans qu'on sache pourquoi est pire
  // qu'une option de plus.
  bool from_stranger = false, from_friend = false;
  const bool opts_ok = ReadWhisperPopupOptions(&from_stranger, &from_friend);
  ImGui::BeginDisabled(!opts_ok);
  if (ro::RoCheckbox(i18n::Tr("Fenêtre individuelle pour un inconnu###chatwnd_wh_stranger"), &from_stranger))
    WriteWhisperPopupOption(true, from_stranger);
  if (ro::RoCheckbox(i18n::Tr("Fenêtre individuelle pour un ami###chatwnd_wh_friend"), &from_friend))
    WriteWhisperPopupOption(false, from_friend);
  ImGui::EndDisabled();
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Ouvre une fenêtre de conversation séparée quand un joueur chuchote.\n\n"
      "Ce sont les réglages du CLIENT (Alt+I, « Friend Setup ») : les changer "
      "ici les change là-bas, et inversement.\n\n"
      "Décochés, les chuchotements restent de simples lignes dans le chat."));

  changed |= ro::RoCheckbox(i18n::Tr("Horodatage###chatwnd_stamp"), &timestamps_);
  changed |= ro::RoCheckbox(i18n::Tr("Icônes d'objets###chatwnd_icons"), &item_icons_);
  changed |= ro::RoCheckbox(i18n::Tr("Diagnostic : tout afficher + type###chatwnd_diag"),
                            &diagnostic_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Ignore les filtres de canal et préfixe chaque ligne du type que le client "
      "nous a transmis (t00 à t24). Une ligne visible ici mais absente d'un onglet "
      "a été écartée par un filtre ; une ligne absente même ici n'est jamais "
      "arrivée jusqu'à nous."));
  if (diagnostic_) {
    size_t held = 0;
    {
      std::lock_guard<std::mutex> lock(lines_mutex_);
      held = lines_.size();
    }
    ImGui::Text(i18n::Tr("Lignes vues par le détour : %u · retenues : %u · en mémoire : %d"),
                ingest_seen_, ingest_kept_, static_cast<int>(held));
  }
  // WheelSliderInt, pas ro::RoSliderInt : même habillage RO (il l'enveloppe),
  // mais avec l'ajustement à la molette au survol, le clamp et l'infobulle —
  // c'est la brique des panneaux de réglages, partout ailleurs dans Bourgeon.
  changed |= WheelSliderInt(i18n::Tr("Lignes conservées par type###chatwnd_cap"),
                            &history_cap_, 100, 5000, "%d");
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Compté PAR TYPE de message — parole, combat, guilde, groupe, "
      "chuchotement… — et non pour l'ensemble du chat.\n\n"
      "C'est ce qui empêche un donjon d'effacer vos conversations : une rafale de "
      "lignes de dégâts n'évince que des lignes de dégâts, et ce qui a été dit il "
      "y a dix minutes reste là.\n\n"
      "Le tampon est commun à tous les onglets — une même ligne s'affiche dans "
      "plusieurs à la fois — mais chaque type y garde sa place."));

  changed |= ro::RoCheckbox(i18n::Tr("Garder l'historique entre les sessions###chatwnd_keep"),
                            &keep_history_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Réaffiche les dernières lignes de la session précédente à la reconnexion, "
      "précédées d'un séparateur.\n\n"
      "ATTENTION : elles sont écrites en clair dans SaveData\\bourgeon_chat_history"
      ".yaml, CHUCHOTEMENTS COMPRIS. Sur une machine partagée, n'importe qui peut "
      "les lire."));
  if (keep_history_) {
    changed |= WheelSliderInt(i18n::Tr("Lignes gardées d'une session à l'autre###chatwnd_keep_n"),
                              &keep_lines_, 20, 1000, "%d");
  }

  // ── Aimantation ───────────────────────────────────────────────────────────
  // Le réglage est ici, avec les autres comportements de fenêtre, et non dans le
  // menu d'une chatbox : il vaut pour TOUTES, et une case par fenêtre laisserait
  // croire qu'on peut aimanter l'une sans l'autre — un aimant, ça se prend à deux.
  changed |= ro::RoCheckbox(i18n::Tr("Aimanter les fenêtres entre elles###chatwnd_magnet"),
                            &magnet_);
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Une chatbox traînée à la souris colle aux bords de l'écran et à ceux "
      "des autres chatbox dès qu'elle en approche : elles se rangent bord à "
      "bord sans avoir à viser le pixel.\n\n"
      "Rien n'est attaché pour autant — éloigner la souris décolle la fenêtre, "
      "et déplacer la voisine n'entraîne pas celle qui lui était collée."));

  SeparatorText(i18n::Tr("Apparence de la chatbox ImGui"));
  ImGui::TextDisabled(i18n::Tr("Réglages généraux — un onglet peut avoir les siens"));
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Clic droit sur un onglet → « Apparence ». Un onglet qui n'a pas ses "
      "propres réglages SUIT ceux-ci : le changer ici le déplace aussi."));
  changed |= RoColorSwatch("Fond###chatwnd_body", body_rgba_);
  changed |= RoColorSwatch("Bordure###chatwnd_border", border_rgba_);
  changed |= RoColorSwatch("Onglets###chatwnd_tab", tab_rgba_);
  // `relayout` = ce qui change le PAS des lignes, donc ce qui périme les hauteurs
  // de repli mémorisées (cf. InvalidateLineLayout). Ni les couleurs ni l'échelle
  // de l'habillage n'en font partie.
  bool relayout = WheelSliderInt(i18n::Tr("Taille du texte du chat###chatwnd_font"),
                                 &font_scale_pct_, kFontPctMin, kFontPctMax, "%d %%");
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Ctrl + molette au-dessus d'une fenêtre de chat fait la même chose, sans "
      "ouvrir ce panneau. Un onglet qui a ses propres réglages zoome seul."));
  changed |= WheelSliderInt(i18n::Tr("Taille de l'interface###chatwnd_uifont"), &ui_scale_pct_,
                            70, 160, "%d %%");
  ImGui::SameLine();
  HelpMarker(
      i18n::Tr("Onglets, boutons et ligne de saisie. Séparée de la taille du chat : "
      "grossir le texte qu'on lit ne doit pas faire enfler la bande d'onglets, "
      "qui mangerait la fenêtre."));
  changed |= WheelSliderInt(i18n::Tr("Marges###chatwnd_pad"), &padding_px_, 0, 12, "%d px");
  relayout |= WheelSliderInt(i18n::Tr("Interligne###chatwnd_gap"), &line_gap_px_, 0, 16, "%d px");
  if (relayout) InvalidateLineLayout();
  changed |= relayout;
  if (ro::RoButton(i18n::Tr("Couleurs du client###chatwnd_reset"))) {
    ro::PickerFromArgb(body_rgba_, 0x96000000);    // le fond natif, un peu plus dense
    ro::PickerFromArgb(border_rgba_, 0xFFC5C5C5);
    ro::PickerFromArgb(tab_rgba_, 0xFF8E938E);     // gris de l'UITabStrip
    changed = true;
  }
  ImGui::SameLine();
  if (ro::RoButton(i18n::Tr("Vider l'historique###chatwnd_clear"))) {
    ClearHistory();
    // Les points de coupe par onglet partent avec le tampon qu'ils découpaient :
    // les laisser ne masquerait plus rien (les rangs à venir sont plus élevés),
    // mais chaque onglet continuerait de proposer « Réafficher » pour du vide.
    for (Channel& channel : channels_) channel.clear_seq = 0;
  }
  return changed;
}
