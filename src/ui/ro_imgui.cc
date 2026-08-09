#include "ui/game_texture.h"
#include "ui/ro_imgui.h"

#include <Windows.h>

#include <cfloat>
#include <cstdio>
#include <cstring>  // std::strlen (validation UTF-8 du texte venu du fil)
#include <fstream>  // lecture directe du yaml (glyphes coréens, cf. plus bas)
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "imgui_freetype.h"  // ImGuiFreeTypeLoaderFlags_LoadColor (emoji en couleur)
#include "imgui_internal.h"  // ImGui::GetActiveID, GetCurrentWindow, TitleBarRect

#include "d3d9/d3d9_hook.h"   // Overlay_CreateTextureARGB, Overlay_SetTextureFilter
#include "ragnarok/globals.h"  // rag::kClientCodePageAddr (code-page du client)
#include "utils/game_paths.h"  // paths::SettingsPath (réglage des glyphes coréens)
#include "ui/ro_skin_blobs.hpp"  // dimensions des pièces (pixels chargés du client)
#include "ui/ro_widgets.h"  // WheelSliderFloat/Int (sliders ajustables à la molette)
#include "utils/i18n.h"

using namespace mui;  // enveloppes ImGui du toolkit (ui/ro_widgets.h)

namespace ro {
namespace {

constexpr UINT kCp949 = 949;  // Unified Hangul Code (client wire/text encoding)

// Malgun Gothic ships with every Windows 10/11 SKU regardless of system locale,
// so a French Windows running a Korean client still has it. Covers latin + full
// modern hangul, so a single font handles both the UI and server strings.
constexpr char kKoreanFontPath[] = "C:\\Windows\\Fonts\\malgun.ttf";

// Rotating thread-local scratch so several converted strings can coexist within
// one frame (e.g. two TextCp949 calls in a row) without clobbering each other.
std::string& NextScratch() {
  constexpr int kSlots = 8;
  thread_local std::string bufs[kSlots];
  thread_local int idx = 0;
  std::string& s = bufs[idx];
  idx = (idx + 1) % kSlots;
  return s;
}

// imgui_stdlib-style resize callback so InputText can grow a std::string.
struct InputTextUserData {
  std::string* str;
};

int InputTextResizeCb(ImGuiInputTextCallbackData* data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto* ud = static_cast<InputTextUserData*>(data->UserData);
    std::string* str = ud->str;
    IM_ASSERT(data->Buf == str->c_str());
    str->resize(data->BufTextLen);
    data->Buf = str->data();
  }
  return 0;
}

}  // namespace

const char* Cp949ToUtf8(const char* cp949) {
  std::string& out = NextScratch();
  out.clear();
  if (!cp949 || !*cp949) return out.c_str();

  int wlen = MultiByteToWideChar(kCp949, 0, cp949, -1, nullptr, 0);
  if (wlen <= 1) return out.c_str();  // includes the null terminator
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(kCp949, 0, cp949, -1, wide.data(), wlen);

  int ulen = WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, nullptr, 0,
                                 nullptr, nullptr);
  if (ulen <= 1) return out.c_str();
  out.resize(ulen - 1);  // drop the trailing null from the buffer size
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), -1, out.data(), ulen, nullptr,
                      nullptr);
  return out.c_str();
}

// La code-page que le client s'est posée d'après son servicetype. SEH : appelée
// depuis des chemins de rendu, et l'adresse n'est peuplée qu'après FUN_00a72440.
static UINT ClientCodePage() {
  __try {
    return *reinterpret_cast<const UINT*>(rag::kClientCodePageAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return CP_ACP; }
}

// Corps commun des deux sens : `from` -> UTF-16 -> `to`. Repli sur les octets
// bruts si une des deux code-pages refuse la chaîne — mieux vaut du texte
// douteux qu'une chaîne vide, on perdrait l'information.
static const char* Recode(const char* in, UINT from, UINT to) {
  std::string& out = NextScratch();
  out.clear();
  if (!in || !*in) return out.c_str();

  const int wlen = MultiByteToWideChar(from, 0, in, -1, nullptr, 0);
  if (wlen <= 1) { out = in; return out.c_str(); }
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(from, 0, in, -1, wide.data(), wlen);

  const int olen =
      WideCharToMultiByte(to, 0, wide.data(), -1, nullptr, 0, nullptr, nullptr);
  if (olen <= 1) { out = in; return out.c_str(); }
  out.resize(olen - 1);
  WideCharToMultiByte(to, 0, wide.data(), -1, out.data(), olen, nullptr, nullptr);
  return out.c_str();
}

const char* LocalToUtf8(const char* local) {
  return Recode(local, ClientCodePage(), CP_UTF8);
}

// ⚠ 1252 EN DUR, et c'est délibéré.
//
// Le texte qui arrive par le réseau est du latin-1 : « à » y est l'octet 0xE0, un
// seul octet. Le raisonnement tient en une phrase : **l'encodage du fil est une
// propriété du SERVEUR**, pas de la machine qui lit. Les deux autres candidats
// décrivent autre chose, et le mesurent donc mal :
//   · la code-page du CLIENT (LocalToUtf8) suit son servicetype — 949 en coréen,
//     où 0xE0 est un octet de TÊTE ;
//   · CP_ACP est la locale non-Unicode du SYSTÈME du joueur, qui vaut 949 chez
//     tous ceux qui l'ont réglée en coréen pour leur client RO.
// Aucune des deux ne dit ce que le serveur a émis ; sur une machine réglée
// autrement, elles se mettraient à « marcher » par coïncidence.
//
// ⚠ Rectification à ne pas perdre : le U+FFFD qui avait lancé cette chasse ne
// venait PAS de la code-page. Le chat neutralisait l'octet 0xA0 (NBSP latin-1)
// APRÈS conversion, où c'est l'octet de continuation du « à » (C3 A0) — les
// « é » (C3 A9), eux, passaient. Le bug était dans le parseur (chat_window.cc,
// ParseText). 1252 reste le bon choix, pour la raison ci-dessus, pas pour
// celle-là.
constexpr UINT kWireCodePage = 1252;

// ── … MAIS LE FIL N'EST PLUS 1252 TOUT SEUL ─────────────────────────────────
// Il porte désormais les DEUX encodages, et c'est voulu. 1252 ne sait pas écrire
// un emoji — il n'a que 256 caractères — donc tout ce qui vient de Discord par le
// relais, ou d'un joueur qui en tape un, doit voyager en UTF-8. Basculer le fil
// d'un bloc était exclu : les scripts NPC, les msg_conf du serveur et
// l'historique de chat déjà écrit sont en 1252, et ils auraient tous perdu leurs
// accents d'un coup.
//
// D'où la lecture TOLÉRANTE : on regarde ce que la chaîne EST, au lieu de le
// décréter. Le test n'est pas une heuristique floue — c'est la validité stricte
// de l'UTF-8, que Windows vérifie pour nous (MB_ERR_INVALID_CHARS) :
//   · « é » en 1252 est l'octet 0xE9 SEUL, ce qui n'est pas de l'UTF-8 valide
//     (0xE9 y annonce trois octets et rien ne suit) → décodé en 1252 ;
//   · « é » en UTF-8 est C3 A9, séquence valide → pris tel quel.
// Les accents des textes existants sont donc conservés à l'octet près, sans
// qu'on ait à toucher au serveur.
//
// ⚠ Le seul faux positif possible est une chaîne 1252 qui serait ELLE-MÊME de
// l'UTF-8 valide : « Ã© », « Ã¨ »… Autrement dit du mojibake déjà écrit. Personne
// ne tape ça, et le confondre avec l'original qu'il représente est même plutôt
// une réparation.
static bool LooksLikeUtf8(const char* s) {
  if (s == nullptr || *s == '\0') return false;
  bool has_high = false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p;
       ++p)
    if (*p >= 0x80) { has_high = true; break; }
  // ASCII pur : les deux tables coïncident, autant garder le chemin habituel.
  if (!has_high) return false;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0) >
         0;
}

// ── Une séquence UTF-8 COUPÉE en fin de chaîne : combien d'octets ? ──────────
// 0 = la fin est propre, ou l'erreur est ailleurs.
//
// 🔴 Ce n'est pas une précaution théorique. Le serveur remplit des champs de
// TAILLE FIXE et coupe à l'octet près, sans savoir ce qu'il coupe — le relais
// Discord tronque à 243 octets (clif_bourgeon_discord_msg_all), et un emoji en
// pèse quatre. Une coupe au mauvais endroit rend la chaîne invalide, et sans ce
// rattrapage c'est le message ENTIER qui repart en 1252 : le joueur ne perd pas
// le dernier caractère, il perd la phrase entière en mojibake.
static size_t DanglingUtf8TailLen(const char* s, size_t n) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  // Un caractère UTF-8 fait au plus quatre octets : la coupure, s'il y en a une,
  // est dans les trois derniers.
  for (size_t back = 1; back <= 3 && back <= n; ++back) {
    const unsigned char c = p[n - back];
    if (c < 0x80) return 0;            // de l'ASCII : rien n'est coupé
    if ((c & 0xC0) == 0x80) continue;  // octet de continuation : on remonte
    // Octet de TÊTE. Combien d'octets annonce-t-il, et en reste-t-il assez ?
    const size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
    return (need > back) ? back : 0;
  }
  return 0;
}

bool IsUtf8(const char* s) { return LooksLikeUtf8(s); }

// ── Les sélecteurs de variante, retirés à l'entrée ───────────────────────────
// U+FE0F (« affiche l'emoji en couleur ») suit presque tous les emoji envoyés
// par Discord ou par le panneau de Windows : « ❤️ » est DEUX points de code.
//
// 🔴 Il ne se contente pas d'être invisible, il PREND DE LA PLACE. Un moteur de
// texte complet l'absorbe pendant le shaping ; ImGui n'en fait pas et dessine
// bêtement son glyphe — qui, dans Segoe UI Emoji, est vide mais large de 2812
// unités sur 2048 (MESURÉ dans la hmtx). Chaque cœur serait donc suivi d'un
// blanc plus large que lui.
//
// On les retire donc du texte à l'affichage. Rien ne se perd : la couleur ne
// dépend pas d'eux ici, elle vient de la police (une seule est chargée).
static void StripVariationSelectors(std::string* s) {
  // U+FE00..U+FE0F = EF B8 80 .. EF B8 8F en UTF-8.
  size_t w = 0;
  for (size_t r = 0; r < s->size(); ++r) {
    const unsigned char c0 = static_cast<unsigned char>((*s)[r]);
    if (c0 == 0xEF && r + 2 < s->size() &&
        static_cast<unsigned char>((*s)[r + 1]) == 0xB8) {
      const unsigned char c2 = static_cast<unsigned char>((*s)[r + 2]);
      if (c2 >= 0x80 && c2 <= 0x8F) {
        r += 2;  // la boucle avance du troisième octet
        continue;
      }
    }
    (*s)[w++] = (*s)[r];
  }
  s->resize(w);
}

const char* WireToUtf8(const char* ansi) {
  if (LooksLikeUtf8(ansi)) {
    // Déjà de l'UTF-8 : le repasser par 1252 le doublerait (« é » -> « Ã© »).
    std::string& out = NextScratch();
    out = ansi;
    StripVariationSelectors(&out);
    return out.c_str();
  }

  // Deuxième chance : de l'UTF-8 que le serveur a coupé au milieu d'un caractère
  // (cf. DanglingUtf8TailLen). On jette la queue orpheline — un caractère perdu
  // vaut mieux qu'une phrase entière en mojibake — et on revalide le reste.
  //
  // ⚠ Aucun risque pour un vrai texte 1252 finissant par un accent : le préfixe
  // qui resterait devrait être de l'UTF-8 valide ET contenir du non-ASCII pour
  // passer, ce qu'une phrase latine ordinaire ne fait jamais.
  if (ansi != nullptr && *ansi != '\0') {
    const size_t len  = std::strlen(ansi);
    const size_t tail = DanglingUtf8TailLen(ansi, len);
    if (tail > 0) {
      std::string head(ansi, len - tail);
      if (LooksLikeUtf8(head.c_str())) {
        std::string& out = NextScratch();
        out.swap(head);
        StripVariationSelectors(&out);
        return out.c_str();
      }
    }
  }

  return Recode(ansi, kWireCodePage, CP_UTF8);
}

const char* Utf8ToWire(const char* utf8) {
  return Recode(utf8, CP_UTF8, kWireCodePage);
}

// ── Le retour, pour du TEXTE (et lui seul) ───────────────────────────────────
// 🔴 CE N'EST PAS UN REMPLAÇANT D'`Utf8ToWire`, et les mélanger casserait des
// choses silencieusement. Un nom de personnage, une cible de chuchotement, un
// nom de canal sont des IDENTIFIANTS : le serveur les compare octet par octet à
// ce que sa base contient, en 1252. Les envoyer en UTF-8 ferait échouer la
// recherche sur le premier accent. Ceux-là passent par `Utf8ToWire`, toujours.
//
// Ici, on ne parle que de PHRASES — une ligne de chat, un courrier — dont le
// serveur ne fait que relayer les octets.
//
// La règle est « 1252 tant que possible » plutôt que « UTF-8 partout », pour que
// la migration ne se voie nulle part ailleurs : une phrase accentuée ordinaire
// part exactement comme avant (mêmes octets en base, dans les logs, pour les
// commandes @), et seule celle qui contient vraiment un caractère hors 1252 —
// un emoji — bascule. Le lecteur, lui, accepte les deux (cf. LooksLikeUtf8).
//
// `WC_NO_BEST_FIT_CHARS` est indispensable : sans lui Windows remplace en
// silence ce qu'il ne sait pas écrire par un caractère « approchant », et l'on
// enverrait un « e » là où le joueur a tapé autre chose, sans jamais basculer.
const char* Utf8ToWireText(const char* utf8) {
  if (utf8 == nullptr || *utf8 == '\0') {
    std::string& out = NextScratch();
    out.clear();
    return out.c_str();
  }

  const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (wlen > 1) {
    std::wstring wide(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), wlen);

    const char fallback = '?';
    const int olen = WideCharToMultiByte(kWireCodePage, WC_NO_BEST_FIT_CHARS,
                                         wide.data(), -1, nullptr, 0, &fallback,
                                         nullptr);
    if (olen > 1) {
      // ⚠ Le drapeau se lit sur la CONVERSION, pas sur la mesure : en mode
      // « calcule-moi la taille » (cbMultiByte = 0), Windows ne garantit pas de
      // le renseigner. On convertit donc pour de bon, puis on décide — quitte à
      // jeter le résultat et à repartir sur un autre emplacement du scratch.
      std::string& out = NextScratch();
      out.resize(olen - 1);
      BOOL used_default = FALSE;
      WideCharToMultiByte(kWireCodePage, WC_NO_BEST_FIT_CHARS, wide.data(), -1,
                          out.data(), olen, &fallback, &used_default);
      if (!used_default) return out.c_str();
    }
  }

  // Hors 1252 : l'UTF-8 part tel quel.
  std::string& out = NextScratch();
  out = utf8;
  return out.c_str();
}

const char* Utf8ToLocal(const char* utf8) {
  return Recode(utf8, CP_UTF8, ClientCodePage());
}

int Utf8ToCp949(const char* utf8, char* out, size_t out_size) {
  if (!out || out_size == 0) return -1;
  out[0] = '\0';
  if (!utf8 || !*utf8) return 0;

  int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  if (wlen <= 1) return 0;
  std::wstring wide(wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), wlen);

  // '?' for glyphs that have no CP949 representation, so nothing is silently
  // dropped (the server would otherwise receive a truncated string).
  const char fallback = '?';
  int written = WideCharToMultiByte(kCp949, 0, wide.data(), -1, out,
                                    static_cast<int>(out_size), &fallback,
                                    nullptr);
  if (written <= 0) {
    out[0] = '\0';
    return -1;  // out too small (ERROR_INSUFFICIENT_BUFFER) or failure
  }
  return written - 1;  // exclude the null terminator
}

namespace {
ImFont* g_font_default = nullptr;  // police intégrée ImGui (ProggyClean) = repli
ImFont* g_font_malgun = nullptr;   // Malgun Gothic (null si absente du système)
// ── Gras et italique ────────────────────────────────────────────────────────
// ImGui ne synthétise NI l'un NI l'autre : il faut de vraies polices, bakées
// dans le même atlas. Nulles quand le fichier système manque — l'appelant
// retombe alors sur la police normale, et le texte reste lisible.
//
// ⚠ MALGUN N'A PAS D'ITALIQUE, et aucune police coréenne courante n'en fournit.
// L'italique vient donc d'Arial : le latin est couvert, le coréen resterait
// droit. Écart assumé sur un serveur francophone.
ImFont* g_font_bold   = nullptr;
ImFont* g_font_italic = nullptr;

// ── Familles au choix, pour l'INTERFACE comme pour la CHATBOX ───────────────
// Bakées au démarrage, comme tout le reste : basculer ensuite ne coûte rien —
// le rendu du chat prend un `ImFont*` en argument, et l'interface ne fait que
// changer `io.FontDefault`. Charger une police arbitraire à chaud demanderait de
// reconstruire l'atlas et de recréer la texture du backend — ce que DX7 ne sait
// pas faire.
//
// Chacune pèse ~275 glyphes. C'est négligeable DÈS LORS que le hangul n'est plus
// baké par défaut : à lui seul, il en coûtait 11 172, soit quarante familles.
//
// 🔴 L'ORDRE DE LA TABLE EST FIGÉ : l'index est PERSISTÉ dans le yaml
// (« chatwnd_font_family » pour le chat, « ui_font_family » pour l'interface).
// Une nouvelle famille s'ajoute EN FIN DE TABLE, jamais au milieu — sinon le
// réglage d'un joueur désigne une AUTRE police au prochain lancement.
//
// ⚠ Le fichier TTF reste EN MÉMOIRE tant que l'atlas vit (depuis ImGui 1.92 les
// glyphes se chargent à la demande, donc la source garde son tampon) : une
// famille coûte le poids de ses trois fichiers. D'où l'absence de Calibri et de
// Courier New — 4 et 2 Mio de plus dans un processus 32 bits, pour des dessins
// que Segoe UI et Consolas rendent déjà.
struct ChatFamily {
  const char* label;
  const char* regular;
  const char* bold;
  const char* italic;
};
const ChatFamily kChatFamilies[] = {
    {"Système (défaut)", nullptr, nullptr, nullptr},  // la police de base
    // ⚠ Tahoma n'a PAS d'italique — elle ne livre que normal et gras. Ce n'est
    // pas un oubli : le fichier n'existe pas. Cf. le repli dans ChatFamilyFont.
    {"Tahoma",   "C:\\Windows\\Fonts\\tahoma.ttf",
                 "C:\\Windows\\Fonts\\tahomabd.ttf",  nullptr},
    {"Segoe UI", "C:\\Windows\\Fonts\\segoeui.ttf",
                 "C:\\Windows\\Fonts\\segoeuib.ttf",
                 "C:\\Windows\\Fonts\\segoeuii.ttf"},
    {"Verdana",  "C:\\Windows\\Fonts\\verdana.ttf",
                 "C:\\Windows\\Fonts\\verdanab.ttf",
                 "C:\\Windows\\Fonts\\verdanai.ttf"},
    {"Consolas", "C:\\Windows\\Fonts\\consola.ttf",
                 "C:\\Windows\\Fonts\\consolab.ttf",
                 "C:\\Windows\\Fonts\\consolai.ttf"},
    // ── Ajoutées pour l'interface (toujours EN FIN DE TABLE) ────────────────
    // Toutes livrées avec Windows 10/11 sans installation ; une absente est
    // simplement masquée du menu (cf. le filtre sur ChatFamilyFont == nullptr).
    {"Arial",    "C:\\Windows\\Fonts\\arial.ttf",
                 "C:\\Windows\\Fonts\\arialbd.ttf",
                 "C:\\Windows\\Fonts\\ariali.ttf"},
    {"Trebuchet MS", "C:\\Windows\\Fonts\\trebuc.ttf",
                 "C:\\Windows\\Fonts\\trebucbd.ttf",
                 "C:\\Windows\\Fonts\\trebucit.ttf"},
    {"Candara",  "C:\\Windows\\Fonts\\candara.ttf",
                 "C:\\Windows\\Fonts\\candarab.ttf",
                 "C:\\Windows\\Fonts\\candarai.ttf"},
    {"Corbel",   "C:\\Windows\\Fonts\\corbel.ttf",
                 "C:\\Windows\\Fonts\\corbelb.ttf",
                 "C:\\Windows\\Fonts\\corbeli.ttf"},
    // ⚠ Franklin Gothic Medium EST le gras de sa famille : Windows ne livre que
    // « Medium » et « Medium Italic », pas de fichier plus gras encore.
    {"Franklin Gothic", "C:\\Windows\\Fonts\\framd.ttf", nullptr,
                 "C:\\Windows\\Fonts\\framdit.ttf"},
    {"Georgia",  "C:\\Windows\\Fonts\\georgia.ttf",
                 "C:\\Windows\\Fonts\\georgiab.ttf",
                 "C:\\Windows\\Fonts\\georgiai.ttf"},
    {"Times New Roman", "C:\\Windows\\Fonts\\times.ttf",
                 "C:\\Windows\\Fonts\\timesbd.ttf",
                 "C:\\Windows\\Fonts\\timesi.ttf"},
    {"Comic Sans MS", "C:\\Windows\\Fonts\\comic.ttf",
                 "C:\\Windows\\Fonts\\comicbd.ttf",
                 "C:\\Windows\\Fonts\\comici.ttf"},
};
constexpr int kChatFamilyCount = IM_ARRAYSIZE(kChatFamilies);

// 🔴 GARDE-FOU D'ATLAS. En mode « glyphes coréens » (débogage staff), le hangul
// reprend ses 11 172 glyphes ; le backend DX7 bake TOUT d'avance dans une texture
// unique, et douze familles latines par-dessus (~9 000 glyphes) la feraient
// déborder. Or un atlas qui déborde, ce n'est pas du texte moche : c'est du texte
// ABSENT, partout. Dans ce mode on s'en tient donc aux quatre familles
// historiques, et les suivantes restent nulles — le menu les masque tout seul.
constexpr int kFamilyCountWithKorean = 5;

ImFont* g_chat_fonts[kChatFamilyCount][3] = {};  // [famille][0=normal,1=gras,2=ital]

// Famille de l'INTERFACE : -1 = police intégrée d'ImGui (l'ancien « Malgun OFF »),
// 0 = Malgun Gothic, >0 = index dans kChatFamilies. Mémorisé même avant le
// chargement de l'atlas, comme l'était le booléen qu'il remplace.
int g_ui_family = 0;

// La police d'une famille pour l'INTERFACE, ou nullptr s'il faut retomber sur
// Malgun (famille absente du système, ou index hors table).
ImFont* UiFamilyFont(int style) {
  if (g_ui_family <= 0 || g_ui_family >= kChatFamilyCount) return nullptr;
  return g_chat_fonts[g_ui_family][style];
}

// (Re)sélectionne la police active selon le réglage. Immédiat (pris en compte au
// prochain NewFrame), sans rebuild d'atlas.
void ApplyFontSelection() {
  ImFont* font = nullptr;
  if (g_ui_family >= 0) {
    font = UiFamilyFont(0);          // la famille choisie…
    if (font == nullptr) font = g_font_malgun;  // …ou Malgun si elle manque
  }
  ImGui::GetIO().FontDefault = font ? font : g_font_default;
}

// ── Les EMOJI, en couleur, dans CHACUNE des polices ──────────────────────────
// Segoe UI Emoji est livrée avec Windows 10 (1809+) et 11, comme Malgun : rien à
// embarquer ni à télécharger.
//
// 🔴 IL FAUT LA FUSIONNER DANS TOUTES LES POLICES, pas seulement dans la
// principale. Une police ImGui ne connaît que SES sources : le chat, qui est
// précisément là où les emoji arrivent, se dessine avec la famille choisie par
// le joueur (Tahoma, Consolas…) et bascule sur les variantes grasse/italique au
// balisage `**…**`. Chacune est une ImFont SÉPARÉE — sans cette fusion, un emoji
// dans un passage en gras, ou dans un chat réglé sur Verdana, sortirait en
// losange, et le joueur croirait à un bug intermittent.
//
// ⚠ D'où le chargement EN MÉMOIRE plutôt que par chemin : `AddFontFromFileTTF`
// relit et duplique le fichier à chaque source, et seguiemj.ttf pèse 12,4 Mio.
// Les trente et quelques polices de l'atlas, c'est plus de 400 Mio dans un
// processus 32 bits — largement de quoi manquer d'adressable. Ici le tampon est
// chargé UNE fois et partagé, d'où
// `FontDataOwnedByAtlas = false` : sans lui, l'atlas le libérerait une fois par
// source, soit treize libérations de trop.
//
// 🔴 AUCUNE PLAGE N'EST DÉCLARÉE, ET C'EST DÉLIBÉRÉ. Depuis ImGui 1.92 les
// glyphes se chargent À LA DEMANDE quand le backend sait mettre sa texture à jour
// (le DX9 le sait) : la source couvre alors tout ce que la police contient, sans
// rien préparer. Lister les blocs emoji ne servirait qu'au backend DX7, resté en
// atlas figé — et lui ferait précharger près de deux mille glyphes COULEUR d'un
// coup, au risque de faire déborder sa texture. Or un atlas qui déborde, ce n'est
// pas du texte moche : c'est du texte ABSENT, partout. En DX7 les emoji
// manqueront donc, et rien d'autre.
//
// ⚠ `LoadColor` n'a d'effet qu'avec le rasteriseur FreeType, activé GLOBALEMENT
// (IMGUI_ENABLE_FREETYPE, thirdparty/imgui/CMakeLists.txt) : stb_truetype ne lit
// que les contours et sortirait le dessin AU TRAIT de chaque emoji. Le brancher
// sur cette seule source aurait été préférable — ImGui expose bien un loader par
// source — mais il le REFUSE pour FreeType (« FIXME-NEWATLAS: Unsupported yet. »)
// et déréférencerait un pointeur nul en Release. Le détail est dans le CMakeLists.
// Conséquence à connaître : tout le texte de l'interface est rastérisé par
// FreeType, donc son dessin bouge très légèrement.
//
// Le flag, lui, est bien PAR SOURCE : `InitFont` fait
// `src->FontLoaderFlags | atlas->FontLoaderFlags`. Seuls les emoji sont donc
// chargés en couleur, pas le reste du texte.
// ⚠ FUITE VOLONTAIRE, jamais libérée. FreeType garde un FT_Face ouvert SUR ce
// tampon pour chaque source, et l'atlas ImGui vit aussi longtemps que la DLL.
// Un `std::vector` global serait détruit par les destructeurs statiques au
// déchargement, dont rien ne garantit qu'ils passent après la destruction du
// contexte ImGui : ce serait un plantage à la fermeture, du genre qu'on ne
// reproduit qu'une fois sur cinq. Douze mégaoctets rendus au processus une
// milliseconde plus tôt ne valent pas ça.
std::vector<char>* g_emoji_ttf = nullptr;  // le fichier, chargé une seule fois

// À appeler JUSTE APRÈS l'ajout de la police à enrichir : `MergeMode` fusionne
// dans la DERNIÈRE police ajoutée à l'atlas, pas dans une police nommée.
void MergeEmoji(float size_px, const ImFontConfig& base) {
  static bool tried = false;
  if (!tried) {
    tried = true;
    std::ifstream f("C:\\Windows\\Fonts\\seguiemj.ttf",
                    std::ios::binary | std::ios::ate);
    if (f) {
      const std::streamoff size = f.tellg();
      if (size > 0) {
        auto* buf = new std::vector<char>(static_cast<size_t>(size));
        f.seekg(0);
        f.read(buf->data(), size);
        if (f)
          g_emoji_ttf = buf;
        else
          delete buf;  // lecture partielle : ne rien fusionner
      }
    }
  }
  // Windows trop ancien, ou police absente : pas d'emoji, et rien d'autre ne change.
  if (g_emoji_ttf == nullptr || g_emoji_ttf->empty()) return;

  ImFontConfig emoji = base;
  emoji.MergeMode = true;
  emoji.GlyphExcludeRanges = nullptr;  // l'antislash-won ne la concerne pas
  emoji.FontDataOwnedByAtlas = false;  // 🔴 tampon partagé, cf. ci-dessus
  emoji.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;  // par source
  ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
      g_emoji_ttf->data(), static_cast<int>(g_emoji_ttf->size()), size_px,
      &emoji, nullptr);
}
}  // namespace

// ── Le réglage « glyphes coréens », lu AVANT que les plugins n'existent ──────
//
// L'atlas se construit à l'init, bien avant que MoonlightUi n'ait chargé le yaml.
// On lit donc la clé directement, à la main — c'est laid, mais c'est le seul
// moment où la question se pose, et ça évite d'inventer un second mécanisme de
// configuration pour un booléen.
//
// 🔴 Changer ce réglage EXIGE un redémarrage du client : l'atlas est baké une
// fois, et DX7 n'a aucun chemin de texture dynamique pour le refaire. Le libellé
// du réglage doit le dire.
bool KoreanGlyphsWanted() {
  static int cached = -1;
  if (cached >= 0) return cached != 0;
  cached = 0;
  try {
    const std::string path = paths::SettingsPath();
    std::ifstream in(path.c_str());
    if (in) {
      std::string line;
      while (std::getline(in, line)) {
        if (line.find("korean_glyphs") == std::string::npos) continue;
        // « korean_glyphs: true » — on ne cherche pas plus loin qu'un `true`.
        if (line.find("true") != std::string::npos) cached = 1;
        break;
      }
    }
  } catch (...) {
    cached = 0;  // illisible = défaut, jamais d'exception à l'init du rendu
  }
  return cached != 0;
}

// Les variantes, ou nullptr quand le système ne les a pas. L'appelant DOIT
// retomber sur sa police courante dans ce cas : un texte non gras vaut mieux
// qu'un texte absent.
//
// 🔴 Elles SUIVENT la famille choisie pour l'interface. Sans ça, une interface en
// Georgia aurait mis ses passages en gras dans le Malgun gras générique : deux
// dessins qui n'ont rien à voir, sur la même ligne. Repli sur la variante
// générique quand la famille ne fournit pas ce style.
ImFont* FontBold() {
  ImFont* font = UiFamilyFont(1);
  return font ? font : g_font_bold;
}
ImFont* FontItalic() {
  ImFont* font = UiFamilyFont(2);
  return font ? font : g_font_italic;
}

int         ChatFamilyCount() { return kChatFamilyCount; }
const char* ChatFamilyLabel(int i) {
  return (i >= 0 && i < kChatFamilyCount) ? i18n::Tr(kChatFamilies[i].label) : "";
}

// La police d'une famille, pour un style donné. Repli en cascade — famille
// absente du système, ou style qu'elle ne fournit pas : on redescend vers la
// variante générique puis vers nullptr, et l'appelant garde sa police courante.
ImFont* ChatFamilyFont(int i, bool bold, bool italic) {
  if (i <= 0 || i >= kChatFamilyCount) {  // 0 = « Système » : rien d'imposé
    // Rien d'imposé = la police de l'INTERFACE, donc ses variantes à elle (et
    // non les génériques) : c'est FontBold/FontItalic qui savent laquelle.
    if (bold)   return FontBold();
    if (italic) return FontItalic();
    return nullptr;
  }
  if (bold) {
    if (g_chat_fonts[i][1]) return g_chat_fonts[i][1];
    if (g_font_bold)        return g_font_bold;
  } else if (italic) {
    if (g_chat_fonts[i][2]) return g_chat_fonts[i][2];
    if (g_font_italic)      return g_font_italic;
  }
  // 🔴 STYLE MANQUANT : on emprunte la variante GÉNÉRIQUE plutôt que de rendre le
  // normal. Tahoma, par exemple, n'a pas d'italique du tout — et rendre le normal
  // faisait que `*texte*` ne produisait RIEN de visible : le joueur écrit un
  // balisage, rien ne change, il conclut que c'est cassé. Un italique d'Arial à
  // côté d'un normal de Tahoma se remarque à peine à cette taille ; une emphase
  // absente, elle, se remarque tout de suite.
  return g_chat_fonts[i][0];
}

ImFont* LoadKoreanFont(float size_px) {
  ImGuiIO& io = ImGui::GetIO();
  // Les DEUX polices sont bakées dans l'atlas au init → bascule gratuite ensuite.
  g_font_default = io.Fonts->AddFontDefault();

  if (GetFileAttributesA(kKoreanFontPath) != INVALID_FILE_ATTRIBUTES) {
    ImFontConfig cfg;
    cfg.OversampleH = 1;  // keep the pre-baked hangul atlas within DX7 limits
    cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    // Explicit ranges = glyphs are baked into the static atlas at build time,
    // which is what the DX7 backend needs (it has no dynamic-texture path).
    // = GetGlyphRangesKorean() + la PONCTUATION TYPOGRAPHIQUE (0x2010-0x203A),
    // qu'ImGui n'inclut pas dans ses plages coréennes : les textes du jeu (livres,
    // descriptions) contiennent des « … », tirets cadratins et apostrophes
    // courbes, qui sortaient en losange « glyphe manquant ». 43 glyphes de plus,
    // négligeable à côté des 11 172 syllabes hangul déjà bakées.
    // ── Les plages, et la plus lourde est OPTIONNELLE ───────────────────────
    //
    // 🔴 LE HANGUL N'EST PLUS BAKÉ PAR DÉFAUT. Il pesait 11 172 glyphes sur ~11
    // 500, soit 97 % de l'atlas — pour des caractères que PERSONNE ne voit : les
    // joueurs sont francophones et le jeu est en français/anglais. Les
    // conversions CP949 qu'on voit partout traduisent l'ENCODAGE DU FIL, pas un
    // contenu coréen ; elles restent nécessaires, les glyphes non.
    //
    // ⚠ SAUF pour le staff qui débogue avec les fichiers du jeu : là, les chemins
    // sont bel et bien coréens (« 유저인터페이스\… ») et la console doit rester
    // lisible. D'où le réglage — voir KoreanGlyphsWanted().
    static const ImWchar kRangesLatin[] = {
        0x0020, 0x00FF,  // latin de base + supplément (accents)
        0x2010, 0x203A,  // tirets, apostrophes/guillemets courbes, points de
                         // suspension (U+2026), puce, pour mille
        0xFFFD, 0xFFFD,  // glyphe « caractère manquant » (le carré propre)
        0,
    };
    static const ImWchar kRangesKorean[] = {
        0x0020, 0x00FF,
        0x2010, 0x203A,
        0x3131, 0x3163,  // jamos coréens
        0xAC00, 0xD7A3,  // syllabes coréennes — les 11 172
        0xFFFD, 0xFFFD,
        0,
    };
    const ImWchar* const kRanges =
        KoreanGlyphsWanted() ? kRangesKorean : kRangesLatin;

    // ── L'antislash : le seul glyphe qu'on ne prend PAS chez Malgun ──────────
    // Les polices coréennes dessinent U+005C comme le WON « ₩ » — héritage de la
    // code-page 949, où l'octet 0x5C porte le symbole monétaire. MESURÉ dans
    // malgun.ttf : le glyphe de U+005C (#63) a QUATRE contours, dans exactement la
    // même boîte que le won U+20A9 (#524) ; un vrai antislash n'en a qu'un (cf.
    // U+002F #18 et U+FF3C #12564). Résultat en jeu : un message serveur du genre
    // « You can be \"called\" » sortait « ₩"called"₩ », alors que le chat natif —
    // rendu avec une police latine — montrait de vrais antislashs.
    //
    // Impossible à corriger côté texte (l'octet EST un antislash) : c'est le
    // dessin qui diffère. On EXCLUT donc ce point de code de la source Malgun et
    // on le fait fournir par une police latine du système.
    //
    // 🔴 `GlyphExcludeRanges`, PAS un trou dans `GlyphRanges`. Depuis ImGui 1.92
    // les glyphes se chargent à la demande et `GlyphRanges` est *LEGACY* : il ne
    // restreint plus la source. Découper les plages autour de 0x5C n'excluait donc
    // rien du tout — Malgun le fournissait quand même, et comme elle est la
    // première source, son won gagnait. C'est le piège de cette version.
    //
    // La police latine est choisie AVANT le chargement : sans elle, personne ne
    // fournirait le glyphe et on afficherait un losange « absent », ce qui est
    // pire qu'un won. Dans ce cas on n'exclut rien.
    static const ImWchar kBackslashOnly[] = {0x005C, 0x005C, 0};
    const char* const kLatinFonts[] = {"C:\\Windows\\Fonts\\tahoma.ttf",
                                       "C:\\Windows\\Fonts\\arial.ttf",
                                       "C:\\Windows\\Fonts\\segoeui.ttf"};
    const char* latin_font = nullptr;
    for (const char* path : kLatinFonts) {
      if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        latin_font = path;
        break;
      }
    }
    if (latin_font) cfg.GlyphExcludeRanges = kBackslashOnly;

    g_font_malgun =
        io.Fonts->AddFontFromFileTTF(kKoreanFontPath, size_px, &cfg, kRanges);

    if (latin_font && g_font_malgun) {
      ImFontConfig merge = cfg;
      merge.MergeMode = true;
      merge.GlyphExcludeRanges = nullptr;  // c'est ELLE qui doit fournir 0x5C
      io.Fonts->AddFontFromFileTTF(latin_font, size_px, &merge, kBackslashOnly);
    }

    MergeEmoji(size_px, cfg);

    // ── Les variantes, bakées MAINTENANT ────────────────────────────────────
    // Même taille et même configuration que la normale : une variante mesurée
    // autrement décalerait les retours à la ligne, puisque c'est la police du
    // fragment qui sert à la MESURE comme au dessin.
    //
    // Le premier fichier trouvé gagne. Malgun en gras couvre latin ET coréen ;
    // pour l'italique, aucune coréenne n'existe et on prend une latine.
    auto load_variant = [&](const char* const* paths, size_t n,
                            const ImWchar* ranges) -> ImFont* {
      for (size_t i = 0; i < n; ++i) {
        if (GetFileAttributesA(paths[i]) == INVALID_FILE_ATTRIBUTES) continue;
        ImFontConfig vc = cfg;
        vc.GlyphExcludeRanges = nullptr;  // pas de won à écarter hors de Malgun
        ImFont* font =
            io.Fonts->AddFontFromFileTTF(paths[i], size_px, &vc, ranges);
        // 🔴 Les emoji dans CELLE-CI aussi. C'est par ici que passent les deux
        // variantes ET les quatre familles de la chatbox : sans cette ligne, un
        // emoji dans un passage en gras, ou un chat réglé sur Verdana, n'aurait
        // que des losanges. Et l'appel doit suivre IMMÉDIATEMENT l'ajout —
        // `MergeMode` fusionne dans la dernière police de l'atlas.
        if (font != nullptr) MergeEmoji(size_px, vc);
        return font;
      }
      return nullptr;
    };
    static const char* const kBoldFonts[] = {"C:\\Windows\\Fonts\\malgunbd.ttf",
                                             "C:\\Windows\\Fonts\\arialbd.ttf",
                                             "C:\\Windows\\Fonts\\tahomabd.ttf"};
    static const char* const kItalicFonts[] = {"C:\\Windows\\Fonts\\ariali.ttf",
                                               "C:\\Windows\\Fonts\\seguiit.ttf"};
    // 🔴 LES DEUX VARIANTES SONT LIMITÉES AU LATIN, et c'est une contrainte
    // d'ATLAS, pas un choix esthétique. La plage coréenne pèse 11 172 syllabes ;
    // l'atlas de base est déjà calibré pour tenir dans les limites de texture du
    // backend DX7 (cf. OversampleH plus haut), qui n'a aucun chemin dynamique.
    // Baker le hangul une deuxième fois pour du gras aurait pu faire déborder
    // l'atlas — et un atlas qui déborde, ce n'est pas du texte moche, c'est du
    // texte ABSENT, partout.
    //
    // Le coût serait de toute façon injustifiable : gras et italique ne servent
    // qu'au balisage `**…**` du chat, tapé en français. Un fragment coréen mis en
    // gras s'affichera donc en normal — invisible en pratique ici.
    // (0xFFFD : le carré « caractère manquant ». Depuis que ces polices servent
    // aussi à TOUTE l'interface, c'est elles qui doivent le fournir — la plage
    // latine seule laissait un trou là où Malgun affichait un carré propre.)
    static const ImWchar kLatinRanges[] = {0x0020, 0x00FF, 0x2010, 0x203A,
                                           0xFFFD, 0xFFFD, 0};
    g_font_bold   = load_variant(kBoldFonts, IM_ARRAYSIZE(kBoldFonts),
                                 kLatinRanges);
    g_font_italic = load_variant(kItalicFonts, IM_ARRAYSIZE(kItalicFonts),
                                 kLatinRanges);

    // ── Les familles au choix pour l'interface et la chatbox ───────────────
    // Entrée 0 = la police de base, laissée nulle : la chatbox garde alors la
    // sienne, et c'est le seul cas où le coréen s'affiche si le réglage est
    // actif. Les autres sont latines — on ne met pas un chemin de sprite en
    // Consolas.
    const int family_count =
        KoreanGlyphsWanted() ? kFamilyCountWithKorean : kChatFamilyCount;
    for (int f = 1; f < family_count; ++f) {
      const ChatFamily& fam = kChatFamilies[f];
      const char* const paths_r[] = {fam.regular};
      const char* const paths_b[] = {fam.bold};
      const char* const paths_i[] = {fam.italic};
      if (fam.regular) g_chat_fonts[f][0] = load_variant(paths_r, 1, kLatinRanges);
      // Gras et italique PROPRES à la famille : passer d'Arial gras sur du
      // Consolas donnerait deux dessins qui n'ont rien à voir. Absents, on
      // retombe sur les variantes génériques, puis sur le normal.
      if (fam.bold)    g_chat_fonts[f][1] = load_variant(paths_b, 1, kLatinRanges);
      if (fam.italic)  g_chat_fonts[f][2] = load_variant(paths_i, 1, kLatinRanges);
    }
  }
  ApplyFontSelection();
  return io.FontDefault;
}

void SetUiFontFamily(int index) {
  // Un index venu d'un yaml trafiqué ou d'une version future ne doit pas sortir
  // de la table : on retombe sur Malgun, jamais sur une lecture hors bornes.
  if (index < -1 || index >= kChatFamilyCount) index = 0;
  g_ui_family = index;
  if (ImGui::GetCurrentContext()) ApplyFontSelection();
}

int UiFontFamily() { return g_ui_family; }

// Ancien booléen « police Malgun », conservé pour la clé yaml du même nom :
// décocher = police intégrée d'ImGui, recocher = retour à Malgun. Recocher ne
// touche PAS à une famille déjà choisie (Georgia reste Georgia).
void SetFontEnabled(bool enabled) {
  // Toujours en passant par SetUiFontFamily, même quand la valeur ne change pas :
  // c'est lui qui applique la sélection, et un appelant qui rallume la police
  // attend un effet, pas un no-op silencieux.
  if (!enabled)
    SetUiFontFamily(-1);
  else
    SetUiFontFamily(g_ui_family < 0 ? 0 : g_ui_family);
}

bool IsFontEnabled() { return g_ui_family >= 0; }

void TextCp949(const char* cp949) {
  ImGui::TextUnformatted(Cp949ToUtf8(cp949));
}

// ── Skin RO ───────────────────────────────────────────────────────────────────
namespace {

// (Plus de g_skin_enabled : le skin RO est l'habillage standard, toujours actif.)
int g_skin_colors = 0;  // combien de PushStyleColor à dépiler dans EndRoWindow
int g_skin_vars = 0;

// Type de curseur RO "main" (index d'action du sprite curseur). À CONFIRMER en jeu
// (si ce n'est pas une main, tester d'autres index : 1..6).
constexpr int kRoCursorHand = 2;
int g_hover_cursor = 0;  // curseur RO demandé cette frame par un widget survolé

RoSkinConfig g_cfg;  // leviers de customisation (persistés par l'appelant)

// Applique luminosité (rgb*brightness) + opacité (a*alpha) globales à une couleur.
// Utilisé par TOUTES les pièces dessinées main (images + texte) pour qu'elles
// suivent les réglages, qu'ImGuiStyleVar_Alpha ne touche pas (rendu manuel).
ImU32 ApplySkinTint(ImU32 c) {
  float b = g_cfg.title_brightness;
  if (b < 0.0f) b = 0.0f;
  if (b > 2.0f) b = 2.0f;
  const float a = g_cfg.alpha;
  int r = (int)(((c >> IM_COL32_R_SHIFT) & 0xFF) * b);
  int g = (int)(((c >> IM_COL32_G_SHIFT) & 0xFF) * b);
  int bl = (int)(((c >> IM_COL32_B_SHIFT) & 0xFF) * b);
  int al = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * a);
  if (r > 255) r = 255;
  if (g > 255) g = 255;
  if (bl > 255) bl = 255;
  if (al > 255) al = 255;
  return IM_COL32(r, g, bl, al);
}

struct SkinTex {
  void* tex = nullptr;
  int w = 0, h = 0;
  unsigned epoch = 0;  // device epoch sous lequel tex a été créée (cf Overlay_DeviceEpoch)
};
SkinTex g_tl, g_tm, g_tr, g_close, g_close_on, g_mini, g_mini_on;
SkinTex g_base, g_base_on;  // bullet sys_base devant le titre (décoratif ou bouton)
// Bullet cliquable demandé pour la PROCHAINE fenêtre RO (SetNextWindowTitleBullet),
// puis résultat du clic pour la fenêtre courante (TitleBulletClicked).
bool g_next_bullet = false;
const char* g_next_bullet_tip = nullptr;
bool g_bullet_clicked = false;
// Couleur de corps demandée pour la PROCHAINE fenêtre RO (SetNextWindowBodyColor).
bool g_next_body_set = false;
unsigned int g_next_body_col = 0;
// Placement/voile demandés pour la PROCHAINE modale RO (SetNextRoModalPos).
bool g_next_modal_pos_set = false;
float g_next_modal_x = 0.0f, g_next_modal_y = 0.0f;
bool g_next_modal_dim = true;
SkinTex g_btn_out_l, g_btn_out_m, g_btn_out_r;
SkinTex g_btn_over_l, g_btn_over_m, g_btn_over_r;
SkinTex g_btn_press_l, g_btn_press_m, g_btn_press_r;
SkinTex g_sbtn_out_l, g_sbtn_out_m, g_sbtn_out_r;
SkinTex g_sbtn_over_l, g_sbtn_over_m, g_sbtn_over_r;
SkinTex g_sbtn_press_l, g_sbtn_press_m, g_sbtn_press_r;
SkinTex g_resize;
SkinTex g_tb_btn_a, g_tb_btn_b, g_tb_btn_c;  // bouton flèche du combo (txtbox_btn_*)
SkinTex g_cb0, g_cb1;
SkinTex g_s0up, g_s0down, g_s0mid, g_s0bar_up, g_s0bar_mid, g_s0bar_down;
// Scrollbar HORIZONTALE du client (scroll1*) : piste+flèches et thumb. Sert de
// base au slider RO (RO n'a pas de « slider », son équivalent visuel est celle-ci).
SkinTex g_s1l, g_s1m, g_s1r, g_s1bar_l, g_s1bar_m, g_s1bar_r;
SkinTex g_bar_l, g_bar_m, g_bar_r, g_iconnum;
SkinTex g_up_l, g_up_m, g_up_r;                    // barre de titre desc (skill_upbar)
SkinTex g_sb_lm, g_sb_rm, g_sb_ld, g_sb_md, g_sb_rd;  // cadre boîte desc (sysbox)
SkinTex g_sb_lu, g_sb_mu, g_sb_ru;                    // haut du sysbox (panels sans titre)
SkinTex g_tab_l_on, g_tab_m_on, g_tab_r_on;     // onglet ACTIF   (tabh_*_on)
SkinTex g_tab_l_off, g_tab_m_off, g_tab_r_off;  // onglet inactif (tabh_*_off)
bool g_skin_active = false;  // BeginRoWindow a pris la branche skin (pour EndRoWindow)
bool g_collapse_allowed = true;  // faux hors du jeu (cf. SetWindowCollapseAllowed)

// ── Loader natif du client (conventions menu_icons.cc / status_tweaks.cc) ──────
// Charge un bmp d'UI depuis le VFS du jeu (GRF + overrides data\) → un joueur qui
// remplace le bmp dans son GRF/data voit son skin custom appliqué, à la RO.
using TexMgr_t = void*(__cdecl*)();
using MakeKey_t = void*(__cdecl*)(const char*);
using LoadTex_t = void*(__fastcall*)(void*, void*, void*);
constexpr int kOffW = 0x114, kOffH = 0x118, kOffPix = 0x11c;  // UITexture fields
// "유저인터페이스\" en CP949 (bytes verbatim, cf. menu_icons.cc).
const char kUIDir[] = "\xC0\xAF\xC0\xFA\xC0\xCE\xC5\xCD\xC6\xE4\xC0\xCC\xBD\xBA";

// Charge un bmp d'UI (chemin RELATIF sous 유저인터페이스\) via le loader natif,
// décode BGRA->A8R8G8B8 avec magenta #FF00FF -> alpha. null si absent/échec.
void* LoadClientBmp(const char* rel_path, int* out_w, int* out_h) {
  void* mgr = reinterpret_cast<TexMgr_t>(ro::texmgr::kGet)();
  if (!mgr) return nullptr;
  char full[192];
  std::snprintf(full, sizeof(full), "%s\\%s", kUIDir, rel_path);
  void* key = reinterpret_cast<MakeKey_t>(ro::texmgr::kMakeKey)(full);
  if (!key) return nullptr;
  void* tex = reinterpret_cast<LoadTex_t>(ro::texmgr::kLoad)(mgr, nullptr, key);
  if (!tex) return nullptr;
  const int w = *reinterpret_cast<int*>(static_cast<char*>(tex) + kOffW);
  const int h = *reinterpret_cast<int*>(static_cast<char*>(tex) + kOffH);
  void* bgra = *reinterpret_cast<void**>(static_cast<char*>(tex) + kOffPix);
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !bgra) return nullptr;
  std::vector<unsigned char> argb(static_cast<size_t>(w) * h * 4);
  const unsigned char* src = static_cast<const unsigned char*>(bgra);
  for (int i = 0; i < w * h; ++i) {
    const unsigned char b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
    const bool keyed = (r == 0xFF && g == 0x00 && b == 0xFF);
    argb[i * 4 + 0] = b;
    argb[i * 4 + 1] = g;
    argb[i * 4 + 2] = r;
    argb[i * 4 + 3] = keyed ? 0 : 0xFF;
  }
  *out_w = w;
  *out_h = h;
  return Overlay_CreateTextureARGB(argb.data(), w, h);
}

// Charge la pièce depuis les fichiers du client (GRF/data) via le loader natif.
// Les dimensions (b.w/b.h) sont connues d'avance → layout stable même avant que la
// texture soit chargée. Retente tant que la texture n'est pas prête (device pas
// prêt) ; si le fichier manque vraiment côté client, la pièce ne se dessine pas.
void* EnsureTex(const char* rel_path, const skin::Blob& b, SkinTex& out) {
  // Texture D3DPOOL_DEFAULT : morte après reset/recréation du device -> on lâche
  // le handle mort pour forcer un rechargement (sinon BlitStretch/AddImage plante).
  const unsigned dev_e = Overlay_DeviceEpoch();
  if (out.epoch != dev_e) { out.tex = nullptr; out.epoch = dev_e; }
  out.w = b.w;
  out.h = b.h;
  if (out.tex) return out.tex;
  int w = 0, h = 0;
  void* t = LoadClientBmp(rel_path, &w, &h);
  if (t) {
    out.tex = t;
    out.w = w;
    out.h = h;
  }
  return out.tex;
}

// Charge une pièce UNIQUEMENT depuis le client (pas de blob de repli embarqué) :
// pour les ressources natives toujours présentes (txtbox_btn_*). out.tex reste nul
// si le BMP manque -> le widget dessine un repli à plat.
void* EnsureTexClient(const char* rel_path, SkinTex& out) {
  const unsigned dev_e = Overlay_DeviceEpoch();
  if (out.epoch != dev_e) { out.tex = nullptr; out.epoch = dev_e; }  // device changé -> recharge
  if (out.tex) return out.tex;
  int w = 0, h = 0;
  void* t = LoadClientBmp(rel_path, &w, &h);
  if (t) { out.tex = t; out.w = w; out.h = h; }
  return out.tex;
}

// Callback ImDrawList : bascule l'échantillonnage en POINT (pixel-art net).
void ImCb_PointFilter(const ImDrawList*, const ImDrawCmd*) {
  Overlay_SetTextureFilter(false);
}

// Dessine une pièce dans le rect donné (uv plein, teinte optionnelle). Renvoie
// true si la texture est prête.
bool BlitStretch(ImDrawList* dl, const SkinTex& t, ImVec2 p0, ImVec2 p1,
                 ImU32 col = IM_COL32_WHITE) {
  if (!t.tex) return false;
  // Luminosité + opacité globales appliquées à chaque pièce.
  dl->AddImage((ImTextureID)t.tex, p0, p1, ImVec2(0, 0), ImVec2(1, 1),
               ApplySkinTint(col));
  return true;
}

// Comme BlitStretch, mais ne prend qu'une PARTIE de la pièce (rect source en
// PIXELS, pas en uv). Sert aux pièces qu'il faut garder NETTES à une taille que
// l'art ne prévoit pas : on blitte les coins à l'échelle 1:1 et on n'étire que
// les bords — uniformes le long de l'axe étiré, donc sans déformation visible.
bool BlitPart(ImDrawList* dl, const SkinTex& t, ImVec2 p0, ImVec2 p1,
              float sx0, float sy0, float sx1, float sy1,
              ImU32 col = IM_COL32_WHITE) {
  if (!t.tex || t.w <= 0 || t.h <= 0) return false;
  const ImVec2 uv0(sx0 / (float)t.w, sy0 / (float)t.h);
  const ImVec2 uv1(sx1 / (float)t.w, sy1 / (float)t.h);
  dl->AddImage((ImTextureID)t.tex, p0, p1, uv0, uv1, ApplySkinTint(col));
  return true;
}

// Bouton système (11x11) dessiné à (cx,cy) top-left. Renvoie true si cliqué.
// Position arrondie à l'entier : sinon (sur une barre de titre de hauteur impaire
// vs bouton pair) le Y tombe en x.5 → sampling POINT → bouton « botched ».
bool SysButton(ImDrawList* dl, const SkinTex& off, const SkinTex& on, ImVec2 tl) {
  tl.x = ImFloor(tl.x);
  tl.y = ImFloor(tl.y);
  const ImVec2 br(tl.x + (float)off.w, tl.y + (float)off.h);
  const bool hovered = ImGui::IsMouseHoveringRect(tl, br, false);
  const SkinTex& t = (hovered && on.tex) ? on : off;
  BlitStretch(dl, t, tl, br);
  return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// Peint la scrollbar verticale RO par-dessus la scrollbar ImGui (transparente) et
// possède entièrement l'interaction (drag du thumb + flèches), en écrivant le
// scroll directement (annule la cible ImGui) → autorité totale, thumb immédiat.
void DrawRoScrollbar(ImGuiWindow* w) {
  if (!w || !w->ScrollbarY) return;
  EnsureTex("scroll0up.bmp", skin::kScroll0Up, g_s0up);
  EnsureTex("scroll0down.bmp", skin::kScroll0Down, g_s0down);
  EnsureTex("scroll0mid.bmp", skin::kScroll0Mid, g_s0mid);
  EnsureTex("scroll0bar_up.bmp", skin::kScroll0BarUp, g_s0bar_up);
  EnsureTex("scroll0bar_mid.bmp", skin::kScroll0BarMid, g_s0bar_mid);
  EnsureTex("scroll0bar_down.bmp", skin::kScroll0BarDown, g_s0bar_down);

  const ImRect bb = ImGui::GetWindowScrollbarRect(w, ImGuiAxis_Y);
  const float x0 = bb.Min.x;
  // Largeur visuelle RO fixe (13px). Si ImGui a réservé un slot PLUS LARGE (fenêtre
  // desc : ScrollbarSize inclut l'épaisseur du cadre sysbox), on garde la scrollbar
  // à 13px À GAUCHE du slot : la marge droite est occupée par le cadre -> scrollbar
  // DANS le cadre, pas par-dessus le bord.
  float x1 = bb.Max.x;
  const float kRoScrollW = 13.0f;
  if (x1 - x0 > kRoScrollW) x1 = x0 + kRoScrollW;
  const float y0 = bb.Min.y;
  float y1 = bb.Max.y;
  // Fenêtre principale redimensionnable : on raccourcit la scrollbar en bas pour
  // laisser la place au grip de resize (sinon il est mangé par la scrollbar). Les
  // child/table windows n'ont pas de grip → pas de raccourci.
  const bool has_grip = !(w->Flags & ImGuiWindowFlags_ChildWindow) &&
                        !(w->Flags & ImGuiWindowFlags_NoResize) &&
                        !(w->Flags & ImGuiWindowFlags_AlwaysAutoResize);
  if (has_grip) y1 -= (float)skin::kBtnResize.h;
  const float arrow = (float)skin::kScroll0Up.h;  // 13
  const float track_top = y0 + arrow, track_bot = y1 - arrow;
  const float track_h = track_bot - track_top;
  const float smax = w->ScrollMax.y;
  ImDrawList* dl = w->DrawList;

  // Écrit le scroll immédiatement + annule toute cible ImGui (FLT_MAX = "pas de
  // cible") pour que notre valeur soit autoritaire ce frame et le suivant.
  auto set_scroll = [&](float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > smax) s = smax;
    w->Scroll.y = s;
    w->ScrollTarget.y = FLT_MAX;
  };

  // Taille du thumb, dans la piste ENTRE les flèches.
  float grab_h = track_h;
  const float size_avail = w->InnerRect.GetHeight();
  const float size_contents = w->ContentSize.y + w->WindowPadding.y * 2.0f;
  const bool scrollable = (size_contents > size_avail && track_h > 0.0f);
  if (scrollable) {
    grab_h = track_h * (size_avail / size_contents);
    const float gmin = ImGui::GetStyle().GrabMinSize;
    if (grab_h < gmin) grab_h = gmin;
    if (grab_h > track_h) grab_h = track_h;
  }

  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  float sratio = smax > 0.0f ? ImSaturate(w->Scroll.y / smax) : 0.0f;
  float grab_y = track_top + sratio * (track_h - grab_h);

  // Drag du thumb (possédé par nous, keyé par fenêtre).
  static ImGuiID s_drag = 0;
  static float s_off = 0.0f;
  if (scrollable && smax > 0.0f && track_h > grab_h) {
    const bool over_thumb = ImGui::IsMouseHoveringRect(
        ImVec2(x0, grab_y), ImVec2(x1, grab_y + grab_h), false);
    if (s_drag == 0 && over_thumb && clicked) {
      s_drag = w->ID;
      s_off = mouse.y - grab_y;
    }
    if (s_drag == w->ID) {
      if (!down) {
        s_drag = 0;
      } else {
        const float r = (mouse.y - s_off - track_top) / (track_h - grab_h);
        set_scroll(ImSaturate(r) * smax);
      }
    }
  }
  // Flèches (clic = 1 pas, maintien = défilement continu).
  const float step = ImGui::GetTextLineHeightWithSpacing() * 3.0f;
  if (scrollable && down &&
      ImGui::IsMouseHoveringRect(ImVec2(x0, y0), ImVec2(x1, y0 + arrow), false))
    set_scroll(w->Scroll.y - (clicked ? step : step * 0.2f));
  if (scrollable && down &&
      ImGui::IsMouseHoveringRect(ImVec2(x0, y1 - arrow), ImVec2(x1, y1), false))
    set_scroll(w->Scroll.y + (clicked ? step : step * 0.2f));

  // Recalcule la position du thumb après interaction.
  sratio = smax > 0.0f ? ImSaturate(w->Scroll.y / smax) : 0.0f;
  grab_y = track_top + sratio * (track_h - grab_h);

  // ── Dessin ──
  dl->PushClipRect(bb.Min, bb.Max, false);
  dl->AddCallback(ImCb_PointFilter, nullptr);
  // Piste : chevauche les flèches de 2px (elles sont peintes par-dessus) → jointure
  // sans trou (seamless).
  BlitStretch(dl, g_s0mid, ImVec2(x0, track_top - 2.0f),
              ImVec2(x1, track_bot + 2.0f));
  BlitStretch(dl, g_s0up, ImVec2(x0, y0), ImVec2(x1, y0 + arrow));
  BlitStretch(dl, g_s0down, ImVec2(x0, y1 - arrow), ImVec2(x1, y1));
  if (scrollable && track_h > grab_h) {
    const float cap = (float)skin::kScroll0BarUp.h;  // 4
    const float gy = ImFloor(grab_y);
    BlitStretch(dl, g_s0bar_up, ImVec2(x0, gy), ImVec2(x1, gy + cap));
    BlitStretch(dl, g_s0bar_down, ImVec2(x0, gy + grab_h - cap),
                ImVec2(x1, gy + grab_h));
    if (grab_h > cap * 2.0f)
      BlitStretch(dl, g_s0bar_mid, ImVec2(x0, gy + cap),
                  ImVec2(x1, gy + grab_h - cap));
  }
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  dl->PopClipRect();

  // Curseur main sur toute la scrollbar (comme le natif).
  if (ImGui::IsMouseHoveringRect(bb.Min, bb.Max, false))
    SetHoverCursor(kRoCursorHand);
}

}  // namespace

RoSkinConfig& SkinConfig() { return g_cfg; }

unsigned int ListBodyColorU32() {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(g_cfg.list_col[0], g_cfg.list_col[1],
                                               g_cfg.list_col[2], g_cfg.list_col[3]));
}

// ── Échap centralisé ──
// On ne stocke QUE des ImGuiWindow* (persistants) — jamais de bool* (souvent un
// local d'OnRenderUI → pendouillant après retour). La fermeture se fait dans
// RegisterEscapeWindow (appelé pendant le Begin, où p_open est valide) au frame
// suivant : ProcessEscapeStack désigne la fenêtre-cible, Register la ferme.
namespace {
std::vector<ImGuiWindow*> g_esc_list;         // fenêtres fermables visibles ce frame
ImGuiWindow* g_esc_close_target = nullptr;    // à fermer au prochain Begin
bool g_esc_any = false;                        // ≥1 ouverte (lu par le WndProc)
bool* g_esc_min_request = nullptr;             // flag « replier » de la fenêtre principale
bool g_esc_suppress = false;                   // un popup modal capte Échap ce frame
}  // namespace

void RegisterEscapeWindow(bool* p_open) {
  if (!p_open || !*p_open) return;
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (!w) return;
  g_esc_list.push_back(w);
  if (w == g_esc_close_target) {  // cible désignée à la frame précédente
    *p_open = false;
    g_esc_close_target = nullptr;
  }
}

void RegisterEscapeMinimizeWindow(bool* p_request_collapse) {
  g_esc_min_request = p_request_collapse;
}

void SuppressEscapeStack() { g_esc_suppress = true; }

void ProcessEscapeStack() {
  // La fenêtre principale (repli) compte comme « ouverte » pour l'avalage, mais reste
  // le tout dernier recours : on ne la minimise que si plus AUCUNE fenêtre fermable.
  g_esc_any = !g_esc_list.empty() || (g_esc_min_request != nullptr);
  // Un popup modal a capté Échap ce frame -> on ne ferme AUCUNE fenêtre RO derrière
  // (sinon Échap fermerait la modale ET la desc). Le flag est consommé ici.
  const bool suppressed = g_esc_suppress;
  g_esc_suppress = false;
  if (!suppressed && g_esc_any &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false)) {
    if (!g_esc_list.empty()) {
      // Désigne la plus au-dessus (FocusOrder max = plus récemment devant) ; fermée
      // au prochain Begin (p_open valide à ce moment-là).
      ImGuiWindow* top = nullptr;
      for (ImGuiWindow* w : g_esc_list)
        if (!top || w->FocusOrder > top->FocusOrder) top = w;
      g_esc_close_target = top;
    } else if (g_esc_min_request) {
      // Seule la fenêtre principale reste : Échap la MINIMISE (elle se replie +
      // persiste au rendu suivant), puis le prochain Échap ira au jeu.
      *g_esc_min_request = true;
    }
  }
  g_esc_list.clear();
  g_esc_min_request = nullptr;
}

bool AnyEscapeWindowOpen() { return g_esc_any; }

float SkinImageBrightness() {
  float b = g_cfg.title_brightness;
  if (b < 0.0f) b = 0.0f;
  if (b > 2.0f) b = 2.0f;
  return b;
}

ImU32 SkinImageTint() {
  float b = SkinImageBrightness();
  if (b > 1.0f) b = 1.0f;  // AddImage ne sur-expose pas : borné à 1 comme les copies
  const int c = static_cast<int>(b * 255.0f + 0.5f);
  const int a = static_cast<int>(ImGui::GetStyle().Alpha * 255.0f + 0.5f);
  return IM_COL32(c, c, c, a);
}

void SetHoverCursor(int ro_cursor_type) { g_hover_cursor = ro_cursor_type; }
int TakeHoverCursor() {
  const int t = g_hover_cursor;
  g_hover_cursor = 0;
  return t;
}

// Latch « curseur plein écran » : on mémorise le n° de frame de la dernière
// assertion ; actif si c'est la frame courante ou la précédente (tolérance 1
// frame pour absorber l'ordre d'exécution hook/rendu).
static int g_fs_cursor_frame = -1000;
void SetFullscreenCursorActive() {
  if (ImGui::GetCurrentContext()) g_fs_cursor_frame = ImGui::GetFrameCount();
}
bool FullscreenCursorActive() {
  return ImGui::GetCurrentContext() &&
         (ImGui::GetFrameCount() - g_fs_cursor_frame) <= 1;
}

// Pousse les 24 couleurs de style communes aux fenêtres RO (corps, texte, onglets,
// scrollbar transparente, table, popups clairs…). Partagé par BeginRoWindow et
// BeginRoDescWindow. Renvoie le nombre de PushStyleColor (à dépiler par End*).
static int PushSkinColors() {
  const ImU32 body = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.body_col[0], g_cfg.body_col[1], g_cfg.body_col[2],
             g_cfg.body_col[3]));
  const ImU32 border = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.border_col[0], g_cfg.border_col[1], g_cfg.border_col[2],
             g_cfg.border_col[3]));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, body);
  ImGui::PushStyleColor(ImGuiCol_TitleBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TitleBgActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, border);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(ImVec4(
                                           g_cfg.body_text[0], g_cfg.body_text[1],
                                           g_cfg.body_text[2], g_cfg.body_text[3])));
  ImGui::PushStyleColor(ImGuiCol_ResizeGrip, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ResizeGripActive, IM_COL32(0, 0, 0, 0));
  const ImU32 inputc = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.input_col[0], g_cfg.input_col[1], g_cfg.input_col[2],
             g_cfg.input_col[3]));
  auto lighten = [](ImU32 c, int d) {
    int r = ((c >> IM_COL32_R_SHIFT) & 0xFF) + d;
    int g = ((c >> IM_COL32_G_SHIFT) & 0xFF) + d;
    int b = ((c >> IM_COL32_B_SHIFT) & 0xFF) + d;
    int a = (c >> IM_COL32_A_SHIFT) & 0xFF;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return IM_COL32(r, g, b, a);
  };
  ImGui::PushStyleColor(ImGuiCol_FrameBg, inputc);
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, lighten(inputc, 10));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, lighten(inputc, 22));
  const ImU32 tabc = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.tab_col[0], g_cfg.tab_col[1], g_cfg.tab_col[2],
             g_cfg.tab_col[3]));
  const ImU32 tabi = ImGui::ColorConvertFloat4ToU32(
      ImVec4(g_cfg.tab_inact[0], g_cfg.tab_inact[1], g_cfg.tab_inact[2],
             g_cfg.tab_inact[3]));
  ImGui::PushStyleColor(ImGuiCol_Tab, tabi);
  ImGui::PushStyleColor(ImGuiCol_TabHovered, lighten(tabi, 14));
  ImGui::PushStyleColor(ImGuiCol_TabSelected, tabc);
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_TableHeaderBg,
                        ImGui::ColorConvertFloat4ToU32(ImVec4(
                            g_cfg.header_col[0], g_cfg.header_col[1],
                            g_cfg.header_col[2], g_cfg.header_col[3])));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0x9C, 0xB8, 0xEA, 160));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0x9C, 0xB8, 0xEA, 110));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0x7E, 0xA0, 0xE0, 210));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0xF2, 0xF3, 0xF6, 255));
  return 24;
}

void SetNextWindowTitleBullet(const char* tooltip) {
  g_next_bullet = true;
  g_next_bullet_tip = tooltip;
}

bool TitleBulletClicked() { return g_bullet_clicked; }

void SetNextWindowBodyColor(unsigned int argb) {
  g_next_body_set = true;
  g_next_body_col = argb;
}

void SetWindowCollapseAllowed(bool allowed) { g_collapse_allowed = allowed; }

bool BeginRoWindow(const char* title, bool* p_open, int imgui_window_flags) {
  // Consommé quoi qu'il arrive : la demande ne doit pas fuiter sur la fenêtre
  // suivante si celle-ci n'est pas peinte (fenêtre masquée…).
  const bool bullet_btn = g_next_bullet;
  const char* bullet_tip = g_next_bullet_tip;
  g_next_bullet = false;
  g_next_bullet_tip = nullptr;
  g_bullet_clicked = false;
  const bool body_set = g_next_body_set;
  const unsigned int body_col = g_next_body_col;
  g_next_body_set = false;

  // Hors du monde de jeu (login, char-select) : aucune fenêtre ne se replie. Le
  // flag NoCollapse fait tout — il retire le bouton sys_mini (test `show_mini`
  // plus bas), neutralise le double-clic sur le titre, et redéplie une fenêtre
  // qui aurait été laissée repliée en jeu.
  if (!g_collapse_allowed) imgui_window_flags |= ImGuiWindowFlags_NoCollapse;

  g_skin_active = true;

  // On garde la mécanique ImGui (drag/resize/collapse) mais on peint nous-mêmes la
  // barre de titre et les boutons système → title bar native transparente, close
  // natif désactivé (on dessine sys_close). p_open est géré manuellement.
  g_skin_colors = PushSkinColors();
  // Corps forcé par l'appelant (SetNextWindowBodyColor) : s'ajoute aux 24 pushes et
  // écrase WindowBg, comme le fait BeginRoDescWindow pour son fond blanc.
  if (body_set) {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, body_col);
    ++g_skin_colors;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  // Arrondi bas fixe ~3px (le haut est couvert par l'art titre).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.alpha);  // opacité globale
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);  // inputs arrondis ~3
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 13.0f);  // largeur pièces RO
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 2.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 6.0f);

  // Hauteur de barre de titre ImGui = FontSize + FramePadding.y*2. On règle
  // FramePadding.y pour qu'elle vaille EXACTEMENT la hauteur de l'art (17px),
  // sinon l'art est étiré verticalement (plus haut que le natif, dégradé déformé).
  float pad_y = ((float)skin::kTitlebarLeft.h - ImGui::GetFontSize()) * 0.5f;
  if (pad_y < 0.0f) pad_y = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, pad_y));
  g_skin_vars = 9;

  // ⚠ Bullet cliquable : ImGui pose SON bouton de repli à gauche de la barre de
  // titre (title_bar_rect.Min.x + FramePadding.x), c'est-à-dire tout juste sous
  // notre bullet — il capte donc le clic et replie la fenêtre au lieu d'ouvrir la
  // config. On le supprime pour CETTE fenêtre en neutralisant sa position.
  // PAS via ImGuiWindowFlags_NoCollapse : ImGui force alors Collapsed=false a
  // chaque frame, ce qui casserait le bouton minimiser dessine par le skin.
  const ImGuiDir menu_btn_backup = ImGui::GetStyle().WindowMenuButtonPosition;
  if (bullet_btn) ImGui::GetStyle().WindowMenuButtonPosition = ImGuiDir_None;
  const bool open = ImGui::Begin(title, nullptr, imgui_window_flags);
  ImGui::GetStyle().WindowMenuButtonPosition = menu_btn_backup;
  RegisterEscapeWindow(p_open);

  // On dessine la barre de titre RO même quand la fenêtre est repliée (Begin
  // renvoie false dans ce cas) — sinon le titre replié garde le chrome ImGui.
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w && !w->Hidden) {
    EnsureTex("basic_interface\\titlebar_left.bmp", skin::kTitlebarLeft, g_tl);
    EnsureTex("basic_interface\\titlebar_mid.bmp", skin::kTitlebarMid, g_tm);
    EnsureTex("basic_interface\\titlebar_right.bmp", skin::kTitlebarRight, g_tr);
    EnsureTex("basic_interface\\sys_close_off.bmp", skin::kSysCloseOff, g_close);
    EnsureTex("basic_interface\\sys_close_on.bmp", skin::kSysCloseOn, g_close_on);
    EnsureTex("basic_interface\\sys_mini_off.bmp", skin::kSysMiniOff, g_mini);
    EnsureTex("basic_interface\\sys_mini_on.bmp", skin::kSysMiniOn, g_mini_on);
    EnsureTex("basic_interface\\sys_base_off.bmp", skin::kSysBaseOff, g_base);
    if (bullet_btn)
      EnsureTex("basic_interface\\sys_base_on.bmp", skin::kSysBaseOn, g_base_on);

    // Repliée : le rect visible EST la barre de titre ; sinon TitleBarRect().
    const ImRect tb = w->Collapsed ? w->Rect() : w->TitleBarRect();
    ImDrawList* dl = w->DrawList;
    const float y0 = tb.Min.y, y1 = tb.Max.y;
    const float capL = (float)g_tl.w, capR = (float)g_tr.w;

    // Après Begin, la clip rect du draw list est réduite à la zone de contenu
    // (sous le titre) → tout dessin dans la barre de titre serait découpé.
    // On élargit la clip à la barre de titre le temps de la peindre.
    dl->PushClipRect(tb.Min, tb.Max, false);

    if (!g_tl.tex) {
      // Repli visible : textures pas encore prêtes / échec de création. Barre bleue
      // pleine (≠ chrome sombre par défaut) pour diagnostiquer d'un coup d'œil.
      dl->AddRectFilledMultiColor(tb.Min, tb.Max, IM_COL32(126, 158, 224, 255),
                                  IM_COL32(126, 158, 224, 255),
                                  IM_COL32(86, 122, 200, 255),
                                  IM_COL32(86, 122, 200, 255));
    }
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, g_tl, ImVec2(tb.Min.x, y0), ImVec2(tb.Min.x + capL, y1));
    BlitStretch(dl, g_tr, ImVec2(tb.Max.x - capR, y0), ImVec2(tb.Max.x, y1));
    BlitStretch(dl, g_tm, ImVec2(tb.Min.x + capL, y0), ImVec2(tb.Max.x - capR, y1));

    // Bullet sys_base devant le titre : décoratif (comme le natif RO), ou bouton
    // si SetNextWindowTitleBullet a été appelé — art « on » au survol, curseur
    // main, et le clic est remonté à l'appelant via TitleBulletClicked().
    const float base_sz = (float)g_base.w;  // 11
    const float base_x = tb.Min.x + 5.0f;
    const float base_y = y0 + (tb.GetHeight() - base_sz) * 0.5f;
    const ImVec2 base_tl(base_x, base_y);
    const ImVec2 base_br(base_x + base_sz, base_y + base_sz);
    bool bullet_hovered = false;
    if (bullet_btn) {
      // Cible élargie de 2px : 11px est trop petit pour viser confortablement.
      bullet_hovered = ImGui::IsMouseHoveringRect(
          ImVec2(base_tl.x - 2.0f, base_tl.y - 2.0f),
          ImVec2(base_br.x + 2.0f, base_br.y + 2.0f), false);
      if (bullet_hovered) {
        SetHoverCursor(kRoCursorHand);
        g_bullet_clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      }
    }
    const SkinTex& base_tex =
        (bullet_hovered && g_base_on.tex) ? g_base_on : g_base;
    if (base_tex.tex) BlitStretch(dl, base_tex, base_tl, base_br);
    const float text_x = base_x + base_sz + 4.0f;

    // Titre par-dessus (couleur configurable ; coupe le "##id").
    char nbuf[128];
    const char* end = ImGui::FindRenderedTextEnd(title);
    size_t n = (size_t)(end - title);
    if (n >= sizeof(nbuf)) n = sizeof(nbuf) - 1;
    memcpy(nbuf, title, n);
    nbuf[n] = '\0';
    const ImU32 title_tx = ImGui::ColorConvertFloat4ToU32(
        ImVec4(g_cfg.title_text[0], g_cfg.title_text[1], g_cfg.title_text[2],
               g_cfg.title_text[3] * g_cfg.alpha));  // suit l'opacité
    const ImVec2 ts = ImGui::CalcTextSize(nbuf);
    dl->AddText(ImVec2(text_x, y0 + (tb.GetHeight() - ts.y) * 0.5f - 1.5f),
                title_tx, nbuf);

    // Boutons système à droite : close (seulement si la fenêtre est fermable,
    // p_open != null) collé au bord droit ; mini à sa gauche, masqué si NoCollapse.
    const bool show_mini = !(imgui_window_flags & ImGuiWindowFlags_NoCollapse);
    const float by = y0 + (tb.GetHeight() - (float)g_close.h) * 0.5f;
    float bx = tb.Max.x - 4.0f;  // curseur depuis le bord droit
    bool close_clicked = false;
    if (p_open) {
      ImVec2 close_tl(bx - (float)g_close.w, by);
      close_clicked = SysButton(dl, g_close, g_close_on, close_tl);
      bx = close_tl.x - 2.0f;
    }
    bool mini_clicked = false;
    if (show_mini) {
      ImVec2 mini_tl(bx - (float)g_mini.w, by);
      mini_clicked = SysButton(dl, g_mini, g_mini_on, mini_tl);
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();

    // Tooltip du bullet APRÈS le PopClipRect : ImGui::SetTooltip ouvre une autre
    // fenêtre (donc une autre draw list) — la peinture du titre doit être finie.
    if (bullet_hovered && bullet_tip && bullet_tip[0])
      ImGui::SetTooltip("%s", bullet_tip);

    if (close_clicked && p_open) *p_open = false;
    if (mini_clicked) ImGui::SetWindowCollapsed(w, !w->Collapsed);

    // Grip de resize RO en bas-à-droite (si redimensionnable). Le grip natif
    // ImGui reste actif pour le drag (juste rendu transparent) ; on peint l'image.
    if (!w->Collapsed && !(w->Flags & ImGuiWindowFlags_NoResize) &&
        !(w->Flags & ImGuiWindowFlags_AlwaysAutoResize)) {
      EnsureTex("btn_resize.bmp", skin::kBtnResize, g_resize);
      const float rw = (float)g_resize.w, rh = (float)g_resize.h;
      const ImVec2 br(w->Pos.x + w->Size.x - 2.0f, w->Pos.y + w->Size.y - 2.0f);
      const ImVec2 tl(br.x - rw, br.y - rh);
      dl->PushClipRect(tl, br, false);
      dl->AddCallback(ImCb_PointFilter, nullptr);
      BlitStretch(dl, g_resize, tl, br);
      dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
      dl->PopClipRect();
      // Curseur main au survol du grip (via le curseur RO natif).
      if (ImGui::IsMouseHoveringRect(tl, br, false))
        SetHoverCursor(kRoCursorHand);
    }
  }
  return open;
}

void EndRoWindow() {
  // Scrollbar RO peinte pendant que la fenêtre est encore courante. On repeint
  // la fenêtre ET ses descendantes (child windows + fenêtres internes de tables
  // ScrollY, ex. le storage) : leur scrollbar ImGui a été rendue transparente par
  // le style poussé, donc sans ça elle serait invisible.
  if (g_skin_active) {
    ImGuiWindow* main = ImGui::GetCurrentWindow();
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (main && g) {
      for (ImGuiWindow* cw : g->Windows) {
        if (cw && cw->Active && cw->ScrollbarY && cw->RootWindow == main)
          DrawRoScrollbar(cw);
      }
    }
    g_skin_active = false;
  }
  ImGui::End();
  if (g_skin_vars) {
    ImGui::PopStyleVar(g_skin_vars);
    g_skin_vars = 0;
  }
  if (g_skin_colors) {
    ImGui::PopStyleColor(g_skin_colors);
    g_skin_colors = 0;
  }
}

// ── 3e style : la CHATBOX ────────────────────────────────────────────────────
// Ni barre de titre, ni corps clair (cf. le commentaire de ro_imgui.h). Compteurs
// dédiés — ce style ne passe PAS par PushSkinColors, dont le corps clair et le
// texte sombre sont exactement ce qu'il ne faut pas ici.
static int g_chat_colors = 0;
static int g_chat_vars = 0;
// Bornes du redimensionnement, recopiées du skin par Begin pour End (qui, lui, ne
// reçoit pas le skin).
static ImVec2 g_chat_min(400.0f, 200.0f);
static ImVec2 g_chat_max(0.0f, 0.0f);
static float  g_chat_snap_step = 0.0f;
static float  g_chat_snap_base = 0.0f;
static bool   g_chat_resizable = true;
static bool   g_chat_resizing  = false;  // un bord est activement tiré

bool RoChatWindowIsResizing() { return g_chat_resizing; }

bool BeginRoChatWindow(const char* id, const RoChatSkin& skin,
                       int imgui_window_flags) {
  // Marque la fenêtre comme « habillée RO » : c'est ce drapeau que lit la
  // repeinture des scrollbars, partagée avec BeginRoWindow.
  g_skin_active = true;

  ImGui::PushStyleColor(ImGuiCol_WindowBg, skin.body_col);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_Border, skin.border_col);
  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
  // Champs de saisie CLAIRS, comme les UIEditWnd du chat natif (la ligne de
  // saisie et la box du destinataire y sont blanches sur le cadre sombre).
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0xCE, 0xCE, 0xCE, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0xDE, 0xDE, 0xDE, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0xEE, 0xEE, 0xEE, 255));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(0xF2, 0xF3, 0xF6, 255));
  ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(0x9C, 0xB8, 0xEA, 160));
  // Scrollbar ImGui rendue transparente : c'est DrawRoScrollbar qui la peint.
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(0, 0, 0, 0));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(0, 0, 0, 0));
  g_chat_colors = 13;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, skin.rounding);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 13.0f);  // largeur des pièces RO
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                      ImVec2(skin.padding, skin.padding));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                      ImVec2(ImGui::GetStyle().ItemSpacing.x, skin.line_gap));
  g_chat_vars = 8;

  // ⚠ PAS d'opacité globale (ImGuiStyleVar_Alpha) : l'opacité de CETTE fenêtre est
  // dans l'alpha de `body_col`. Les cumuler rendrait le texte translucide en même
  // temps que le fond, ce que le chat natif ne fait pas.
  // 🔴 `NoResize` coupe le redimensionnement d'ImGui — poignées de COIN comprises,
  // qu'aucun drapeau ne sait désactiver séparément. Ce sont nos quatre bords, posés
  // par EndRoChatWindow, qui redimensionnent : eux seuls peuvent se désactiver
  // individuellement quand ils touchent le bord de l'écran.
  imgui_window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoResize;
  g_chat_min = ImVec2(skin.min_w, skin.min_h);
  g_chat_max = ImVec2(skin.max_w, skin.max_h);
  g_chat_snap_step = skin.snap_step;
  g_chat_snap_base = skin.snap_base;
  g_chat_resizable = skin.resizable;
  if (!skin.movable) imgui_window_flags |= ImGuiWindowFlags_NoMove;
  const bool open = ImGui::Begin(id, nullptr, imgui_window_flags);
  if (skin.font_scale > 0.0f && skin.font_scale != 1.0f)
    ImGui::SetWindowFontScale(skin.font_scale);

  return open;
}

// Redimensionnement par les QUATRE BORDS, à la place des poignées d'ImGui.
//
// Pourquoi le faire nous-mêmes : ImGui ne sait pas désactiver un bord en
// particulier, et surtout pas selon la position de la fenêtre. Or un bord collé
// au bord de l'écran ne peut pas s'écarter — la fenêtre grandit alors du côté
// OPPOSÉ, ce qui donne exactement le contraire du geste demandé. Ces bords-là sont
// donc inertes tant qu'ils touchent l'écran, et se réactivent dès que la fenêtre
// s'en éloigne.
//
// À poser en FIN de fenêtre : à hit-test égal, c'est le dernier widget soumis qui
// gagne le survol — les bords passent ainsi devant le contenu, pas derrière.
static void ChatEdgeResize(ImGuiWindow* w) {
  ImGuiIO& io = ImGui::GetIO();
  const ImVec2 disp = io.DisplaySize;
  if (disp.x <= 0.0f || disp.y <= 0.0f) return;

  ImVec2 min_size = g_chat_min;
  ImVec2 max_size = g_chat_max;
  if (max_size.x <= 0.0f) max_size.x = disp.x * 0.8f;  // défaut : 80 % de l'écran
  if (max_size.y <= 0.0f) max_size.y = disp.y * 0.8f;
  if (max_size.x < min_size.x) max_size.x = min_size.x;
  if (max_size.y < min_size.y) max_size.y = min_size.y;

  const float kBand = 8.0f;  // épaisseur de la zone de saisie du bord
  const float kTol  = 2.0f;  // « collé à l'écran » à deux pixels près
  const ImVec2 pos = w->Pos, size = w->SizeFull;
  const bool live[4] = {
      pos.x > kTol,                        // gauche
      pos.x + size.x < disp.x - kTol,      // droite
      pos.y > kTol,                        // haut
      pos.y + size.y < disp.y - kTol,      // bas
  };

  const ImVec2 rect_min[4] = {
      ImVec2(pos.x, pos.y + kBand),
      ImVec2(pos.x + size.x - kBand, pos.y + kBand),
      ImVec2(pos.x + kBand, pos.y),
      ImVec2(pos.x + kBand, pos.y + size.y - kBand),
  };
  const ImVec2 rect_size[4] = {
      ImVec2(kBand, size.y - 2.0f * kBand),
      ImVec2(kBand, size.y - 2.0f * kBand),
      ImVec2(size.x - 2.0f * kBand, kBand),
      ImVec2(size.x - 2.0f * kBand, kBand),
  };
  static const char* const kIds[4] = {"##ro_rs_l", "##ro_rs_r", "##ro_rs_t",
                                      "##ro_rs_b"};

  // Le curseur de mise en page ET la borne de contenu sont restaurés : ces bandes
  // ne sont pas du contenu. Les laisser étendre `CursorMaxPos` donnerait à la
  // fenêtre quelques pixels de défilement fantôme — assez pour que la molette
  // fasse glisser tout l'habillage de trois pixels.
  const ImVec2 saved_cursor = ImGui::GetCursorScreenPos();
  const ImVec2 saved_max    = w->DC.CursorMaxPos;
  // 🔴 Le geste se calcule depuis la POSITION ABSOLUE de la souris, pas en cumulant
  // `MouseDelta` — c'est la méthode d'ImGui lui-même, et pour une bonne raison.
  // Cumuler des deltas sur une taille déjà arrondie à la rangée PERD tout mouvement
  // plus court qu'une demi-rangée : le geste ne produisait rien tant qu'on bougeait
  // doucement, puis sautait d'un cran dès qu'une frame dépassait le seuil. La
  // largeur, elle, n'est pas quantifiée — d'où une largeur impeccable et une
  // hauteur inutilisable, avec le même code.
  //
  // Deux repères pris au clic et tenus jusqu'au relâchement : `grab` = l'écart
  // entre la souris et le bord saisi (sans lui, le bord saute sous le curseur au
  // premier pixel), `fixed` = le bord OPPOSÉ, qui ne doit pas bouger du geste.
  static ImVec2 g_drag_grab;
  static ImVec2 g_drag_fixed;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImVec2 want = size;
  bool changed = false, dragged_left = false, dragged_top = false;

  for (int i = 0; i < 4; ++i) {
    // Un bord qu'on est EN TRAIN de tirer reste posé même s'il vient de toucher le
    // bord de l'écran : le retirer en pleine frame perdrait l'ActiveId, et le
    // glissement s'interromprait au milieu du geste.
    const bool dragging_this = (g.ActiveId == w->GetID(kIds[i]));
    if ((!live[i] && !dragging_this) || rect_size[i].x <= 0.0f ||
        rect_size[i].y <= 0.0f)
      continue;
    ImGui::SetCursorScreenPos(rect_min[i]);
    // 🔴 `FlattenChildren` est indispensable : la zone de log est une fenêtre
    // ENFANT qui vient jusqu'à la marge, et un widget du parent n'est pas
    // survolable là où c'est l'enfant qui est sous la souris. Sans ce drapeau, la
    // bande utile se réduirait à la seule épaisseur du filet.
    ImGui::InvisibleButton(kIds[i], rect_size[i], ImGuiButtonFlags_FlattenChildren);
    if (ImGui::IsItemActivated()) {  // repères du geste, figés au clic
      switch (i) {
        case 0:
          g_drag_grab.x  = io.MousePos.x - pos.x;
          g_drag_fixed.x = pos.x + size.x;
          break;
        case 1:
          g_drag_grab.x  = io.MousePos.x - (pos.x + size.x);
          g_drag_fixed.x = pos.x;
          break;
        case 2:
          g_drag_grab.y  = io.MousePos.y - pos.y;
          g_drag_fixed.y = pos.y + size.y;
          break;
        default:
          g_drag_grab.y  = io.MousePos.y - (pos.y + size.y);
          g_drag_fixed.y = pos.y;
          break;
      }
    }
    // Le bouton actif prend l'ActiveId : sans lui, le glissement DÉPLACERAIT la
    // fenêtre, qui n'a pas de barre de titre et se traîne donc par son corps.
    const bool active = ImGui::IsItemActive();
    if (active || ImGui::IsItemHovered()) SetHoverCursor(kRoCursorHand);
    if (!active) continue;
    changed = true;
    const ImVec2 edge(io.MousePos.x - g_drag_grab.x, io.MousePos.y - g_drag_grab.y);
    switch (i) {
      case 0: want.x = g_drag_fixed.x - edge.x; dragged_left = true; break;
      case 1: want.x = edge.x - g_drag_fixed.x; break;
      case 2: want.y = g_drag_fixed.y - edge.y; dragged_top = true; break;
      default: want.y = edge.y - g_drag_fixed.y; break;
    }
  }
  // 🔴 Restauration par écriture DIRECTE, surtout pas `SetCursorScreenPos` :
  // ImGui note « le curseur a été déplacé » et exige un item derrière pour
  // valider l'agrandissement. Un appel en dernière position n'en a aucun, et il
  // lève « Code uses SetCursorPos() to extend window boundaries ». Ici on ne
  // demande rien à ImGui, on lui rend son état d'avant.
  w->DC.CursorPos    = saved_cursor;
  w->DC.CursorMaxPos = saved_max;
  g_chat_resizing    = changed;
  if (!changed) return;

  // La taille voulue passe par les bornes puis par la quantification ; c'est ce
  // RÉSULTAT qui devient la fenêtre. Le calcul, lui, repart de la souris à chaque
  // frame — rien ne s'accumule, donc rien ne dérive.
  ImVec2 new_pos  = pos;
  ImVec2 new_size = want;
  new_size.x = ImClamp(new_size.x, min_size.x, max_size.x);
  new_size.y = ImClamp(new_size.y, min_size.y, max_size.y);

  // 🔴 Quantifier la hauteur ICI, avec exactement la règle que l'appelant a mise
  // dans ses contraintes. Sans ça on pose une hauteur libre, ImGui l'arrondit à la
  // frame suivante, et cet arrondi ne touche QUE la taille : le bord opposé à
  // celui qu'on tire se déplace tout seul. C'est ce qui donnait, en hauteur, un
  // redimensionnement qui semblait déplacer la fenêtre en même temps.
  if (g_chat_snap_step > 1.0f) {
    float rows = ImFloor((new_size.y - g_chat_snap_base) / g_chat_snap_step + 0.5f);
    if (rows < 1.0f) rows = 1.0f;
    while (g_chat_snap_base + rows * g_chat_snap_step < min_size.y) rows += 1.0f;
    while (rows > 1.0f && g_chat_snap_base + rows * g_chat_snap_step > max_size.y)
      rows -= 1.0f;
    new_size.y = g_chat_snap_base + rows * g_chat_snap_step;
  }

  // 🔴 La position se déduit du bord FIGÉ, jamais du delta : c'est ce qui garantit
  // que le bord d'en face ne bouge pas d'un pixel, y compris quand une borne ou
  // l'arrondi à la rangée refusent la taille demandée.
  if (dragged_left) new_pos.x = g_drag_fixed.x - new_size.x;
  if (dragged_top)  new_pos.y = g_drag_fixed.y - new_size.y;

  ImGui::SetWindowPos(w, new_pos);
  ImGui::SetWindowSize(w, new_size);
}

void EndRoChatWindow() {
  if (g_skin_active) {
    ImGuiWindow* main = ImGui::GetCurrentWindow();
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (main && g) {
      for (ImGuiWindow* cw : g->Windows) {
        if (cw && cw->Active && cw->ScrollbarY && cw->RootWindow == main)
          DrawRoScrollbar(cw);
      }
      // Verrouillée : on ne pose PAS les bandes. Les laisser en les rendant
      // inertes coûterait le même curseur « main » au survol, donc la promesse
      // muette d'un geste qui ne se produira pas.
      if (g_chat_resizable && !main->Collapsed && !main->Hidden)
        ChatEdgeResize(main);
    }
    g_skin_active = false;
  }
  ImGui::End();
  if (g_chat_vars) {
    ImGui::PopStyleVar(g_chat_vars);
    g_chat_vars = 0;
  }
  if (g_chat_colors) {
    ImGui::PopStyleColor(g_chat_colors);
    g_chat_colors = 0;
  }
}

// ── Boîte de dialogue MODALE façon RO ──────────────────────────────────────────
// Même style que BeginRoWindow (PushSkinColors -> corps clair via PopupBg, texte
// sombre, champ blanc + barre de titre 3-slice titlebar_*) mais via BeginPopupModal :
// ImGui bloque/assombrit l'arrière-plan lui-même. Aucun bouton système (un dialogue
// se ferme par ses propres boutons). Compteurs dédiés (g_modal_*) pour ne pas marcher
// sur ceux de BeginRoWindow. Symétrie du pop : si la popup n'est pas ouverte, End ne
// sera pas appelé -> on dépile ICI.
static int g_modal_colors = 0;
static int g_modal_vars = 0;

// Verrou : le défaut de BeginRoPopupModal dans ro_imgui.h est le littéral 64 (le
// header n'inclut pas imgui.h). Si ImGui renumérote ce flag, on casse ici, pas en
// silence.
static_assert(ImGuiWindowFlags_AlwaysAutoResize == 64,
              "MAJ le défaut de BeginRoPopupModal dans ro_imgui.h");

void SetNextRoModalPos(float x, float y, bool dim_background) {
  g_next_modal_pos_set = true;
  g_next_modal_x = x;
  g_next_modal_y = y;
  g_next_modal_dim = dim_background;
}

bool BeginRoPopupModal(const char* title, int imgui_window_flags) {
  // Consommé quoi qu'il arrive (comme le bullet de BeginRoWindow) : la demande ne
  // doit pas fuiter sur la modale suivante si celle-ci n'est pas ouverte.
  const bool pos_set = g_next_modal_pos_set;
  const float pos_x = g_next_modal_x, pos_y = g_next_modal_y;
  const bool dim_bg = !pos_set || g_next_modal_dim;
  g_next_modal_pos_set = false;
  g_next_modal_dim = true;

  g_modal_colors = PushSkinColors();
  // ImGui fige la couleur du voile dans Begin (window->DC.ModalDimBgColor) : la
  // pousser ICI suffit, et elle se dépile avec les couleurs du skin.
  if (!dim_bg) {
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 0, 0, 0));
    ++g_modal_colors;
  }
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 3.0f);
  // Barre de titre = hauteur EXACTE de l'art (sinon l'art titlebar est étiré).
  float pad_y = ((float)skin::kTitlebarLeft.h - ImGui::GetFontSize()) * 0.5f;
  if (pad_y < 0.0f) pad_y = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, pad_y));
  g_modal_vars = 6;

  if (pos_set)
    ImGui::SetNextWindowPos(ImVec2(pos_x, pos_y), ImGuiCond_Appearing);
  else
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  const bool open = ImGui::BeginPopupModal(title, nullptr, imgui_window_flags);
  if (!open) {
    ImGui::PopStyleVar(g_modal_vars);     g_modal_vars = 0;
    ImGui::PopStyleColor(g_modal_colors); g_modal_colors = 0;
    return false;
  }

  // Peinture de la barre de titre RO par-dessus le chrome ImGui (rendu transparent
  // par PushSkinColors : TitleBg = 0). Bullet sys_base décoratif + titre ; pas de
  // boutons système (dialogue modal).
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w && !w->Hidden) {
    EnsureTex("basic_interface\\titlebar_left.bmp", skin::kTitlebarLeft, g_tl);
    EnsureTex("basic_interface\\titlebar_mid.bmp", skin::kTitlebarMid, g_tm);
    EnsureTex("basic_interface\\titlebar_right.bmp", skin::kTitlebarRight, g_tr);
    EnsureTex("basic_interface\\sys_base_off.bmp", skin::kSysBaseOff, g_base);
    const ImRect tb = w->TitleBarRect();
    ImDrawList* dl = w->DrawList;
    const float y0 = tb.Min.y, y1 = tb.Max.y;
    const float capL = (float)g_tl.w, capR = (float)g_tr.w;
    dl->PushClipRect(tb.Min, tb.Max, false);
    if (!g_tl.tex)
      dl->AddRectFilledMultiColor(tb.Min, tb.Max, IM_COL32(126, 158, 224, 255),
                                  IM_COL32(126, 158, 224, 255),
                                  IM_COL32(86, 122, 200, 255),
                                  IM_COL32(86, 122, 200, 255));
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, g_tl, ImVec2(tb.Min.x, y0), ImVec2(tb.Min.x + capL, y1));
    BlitStretch(dl, g_tr, ImVec2(tb.Max.x - capR, y0), ImVec2(tb.Max.x, y1));
    BlitStretch(dl, g_tm, ImVec2(tb.Min.x + capL, y0), ImVec2(tb.Max.x - capR, y1));
    const float base_sz = (float)g_base.w;
    const float base_x = tb.Min.x + 5.0f;
    const float base_y = y0 + (tb.GetHeight() - base_sz) * 0.5f;
    if (g_base.tex)
      BlitStretch(dl, g_base, ImVec2(base_x, base_y),
                  ImVec2(base_x + base_sz, base_y + base_sz));
    const float text_x = base_x + base_sz + 4.0f;
    char nbuf[128];
    const char* end = ImGui::FindRenderedTextEnd(title);
    size_t n = (size_t)(end - title);
    if (n >= sizeof(nbuf)) n = sizeof(nbuf) - 1;
    memcpy(nbuf, title, n);
    nbuf[n] = '\0';
    const ImU32 title_tx = ImGui::ColorConvertFloat4ToU32(
        ImVec4(g_cfg.title_text[0], g_cfg.title_text[1], g_cfg.title_text[2],
               g_cfg.title_text[3] * g_cfg.alpha));
    const ImVec2 ts = ImGui::CalcTextSize(nbuf);
    dl->AddText(ImVec2(text_x, y0 + (tb.GetHeight() - ts.y) * 0.5f - 1.5f),
                title_tx, nbuf);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();
  }
  return true;
}

void EndRoPopupModal() {
  ImGui::EndPopup();
  if (g_modal_vars) {
    ImGui::PopStyleVar(g_modal_vars);
    g_modal_vars = 0;
  }
  if (g_modal_colors) {
    ImGui::PopStyleColor(g_modal_colors);
    g_modal_colors = 0;
  }
}

// Fenêtre de description : design distinct (barre de titre skill_upbar claire +
// cadre boîte sysbox), même config/couleurs/scrollbar que le reste du skin.
bool BeginRoDescWindow(const char* title, bool* p_open, int imgui_window_flags,
                       unsigned int title_shadow) {
  g_skin_active = true;

  g_skin_colors = PushSkinColors();
  // Desc : fond BLANC + bordure 1px c2c2c2 (continuité avec le titre). Ces 2
  // pushes s'ajoutent aux 24 de PushSkinColors et écrasent WindowBg/Border.
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(255, 255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0xC2, 0xC2, 0xC2, 255));
  g_skin_colors += 2;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);  // bordure 1px c2c2c2
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.alpha);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
  // Scrollbar 13px + épaisseur du cadre sysbox (14px) réservée : ImGui garde le
  // contenu à l'écart du cadre ET la scrollbar (dessinée à 13px à gauche du slot,
  // cf. DrawRoScrollbar) se retrouve DANS le cadre au lieu de par-dessus le bord.
  ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,
                      6.0f + (float)skin::kSysboxLm.w);
  ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 2.0f);
  // Contenu : côtés = épaisseur sysbox +2px, peu en haut/bas.
  const float e = (float)skin::kSysboxLm.w;  // 14
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(e + 2.0f, 7.0f));
  // Barre de titre = hauteur de l'art skill_upbar (20px).
  float pad_y = ((float)skin::kUpbarLeft.h - ImGui::GetFontSize()) * 0.5f;
  if (pad_y < 0.0f) pad_y = 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                      ImVec2(ImGui::GetStyle().FramePadding.x, pad_y));
  g_skin_vars = 9;

  const bool open = ImGui::Begin(title, nullptr, imgui_window_flags);
  RegisterEscapeWindow(p_open);

  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (w && !w->Hidden) {
    EnsureTex("basic_interface\\skill_upbar_left.bmp", skin::kUpbarLeft, g_up_l);
    EnsureTex("basic_interface\\skill_upbar_mid.bmp", skin::kUpbarMid, g_up_m);
    EnsureTex("basic_interface\\skill_upbar_right.bmp", skin::kUpbarRight, g_up_r);
    EnsureTex("sysbox_lm.bmp", skin::kSysboxLm, g_sb_lm);
    EnsureTex("sysbox_rm.bmp", skin::kSysboxRm, g_sb_rm);
    EnsureTex("sysbox_ld.bmp", skin::kSysboxLd, g_sb_ld);
    EnsureTex("sysbox_md.bmp", skin::kSysboxMd, g_sb_md);
    EnsureTex("sysbox_rd.bmp", skin::kSysboxRd, g_sb_rd);

    const ImRect tb = w->Collapsed ? w->Rect() : w->TitleBarRect();
    const ImRect wr = w->Rect();
    ImDrawList* dl = w->DrawList;
    const float y0 = tb.Min.y, y1 = tb.Max.y;
    const float ucL = (float)g_up_l.w, ucR = (float)g_up_r.w;

    dl->PushClipRect(wr.Min, wr.Max, false);  // couvre toute la fenêtre (titre+cadre)
    dl->AddCallback(ImCb_PointFilter, nullptr);
    // Barre de titre skill_upbar (3-slice).
    BlitStretch(dl, g_up_l, ImVec2(tb.Min.x, y0), ImVec2(tb.Min.x + ucL, y1));
    BlitStretch(dl, g_up_r, ImVec2(tb.Max.x - ucR, y0), ImVec2(tb.Max.x, y1));
    BlitStretch(dl, g_up_m, ImVec2(tb.Min.x + ucL, y0), ImVec2(tb.Max.x - ucR, y1));
    // Cadre sysbox (côtés + bas ; le haut est couvert par le titre).
    if (!w->Collapsed) {
      const float fx0 = wr.Min.x, fx1 = wr.Max.x, fby = wr.Max.y;
      BlitStretch(dl, g_sb_lm, ImVec2(fx0, y1), ImVec2(fx0 + e, fby - e));
      BlitStretch(dl, g_sb_rm, ImVec2(fx1 - e, y1), ImVec2(fx1, fby - e));
      BlitStretch(dl, g_sb_ld, ImVec2(fx0, fby - e), ImVec2(fx0 + e, fby));
      BlitStretch(dl, g_sb_rd, ImVec2(fx1 - e, fby - e), ImVec2(fx1, fby));
      BlitStretch(dl, g_sb_md, ImVec2(fx0 + e, fby - e), ImVec2(fx1 - e, fby));
    }

    // Titre (couleur configurable, coupe le "##id").
    char nbuf[128];
    const char* end = ImGui::FindRenderedTextEnd(title);
    size_t n = (size_t)(end - title);
    if (n >= sizeof(nbuf)) n = sizeof(nbuf) - 1;
    memcpy(nbuf, title, n);
    nbuf[n] = '\0';
    const ImU32 ttx = ImGui::ColorConvertFloat4ToU32(
        ImVec4(g_cfg.title_text[0], g_cfg.title_text[1], g_cfg.title_text[2],
               g_cfg.title_text[3] * g_cfg.alpha));
    const ImVec2 ts = ImGui::CalcTextSize(nbuf);
    const ImVec2 tpos(tb.Min.x + 8.0f,
                      y0 + (tb.GetHeight() - ts.y) * 0.5f - 1.0f);
    // Ombre optionnelle du titre (ex. rouge 0x5050fa pour un item cassé), décalée
    // +1,+1 sous le texte du titre.
    if (title_shadow)
      dl->AddText(ImVec2(tpos.x + 1.0f, tpos.y + 1.0f), title_shadow, nbuf);
    dl->AddText(tpos, ttx, nbuf);

    // Bouton close (seulement si fermable), collé au bord droit du titre.
    bool close_clicked = false;
    if (p_open) {
      EnsureTex("basic_interface\\sys_close_off.bmp", skin::kSysCloseOff, g_close);
      EnsureTex("basic_interface\\sys_close_on.bmp", skin::kSysCloseOn, g_close_on);
      const float by = y0 + (tb.GetHeight() - (float)g_close.h) * 0.5f;
      ImVec2 ctl(tb.Max.x - (float)g_close.w - 5.0f, by);
      close_clicked = SysButton(dl, g_close, g_close_on, ctl);
    }
    // Grip de resize RO dans le coin bas-droite (si redimensionnable). Le grip
    // ImGui natif reste actif pour le drag (rendu transparent) ; on peint l'image.
    if (!w->Collapsed && !(w->Flags & ImGuiWindowFlags_NoResize) &&
        !(w->Flags & ImGuiWindowFlags_AlwaysAutoResize)) {
      EnsureTex("btn_resize.bmp", skin::kBtnResize, g_resize);
      const float rw = (float)g_resize.w, rh = (float)g_resize.h;
      const ImVec2 rbr(wr.Max.x - 4.0f, wr.Max.y - 4.0f);
      const ImVec2 rtl(rbr.x - rw, rbr.y - rh);
      BlitStretch(dl, g_resize, rtl, rbr);
      if (ImGui::IsMouseHoveringRect(rtl, rbr, false))
        SetHoverCursor(kRoCursorHand);
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();

    if (close_clicked && p_open) *p_open = false;
  }
  return open;
}

void EndRoDescWindow() { EndRoWindow(); }  // même teardown (scrollbar + pop)

// Panneau de description SANS barre de titre : cadre boîte sysbox complet (9-slice
// avec le haut), fond blanc + bordure. Pour les sous-panneaux (cartes, options).
bool BeginRoDescPanel(const char* id, int imgui_window_flags) {
  g_skin_active = true;
  g_skin_colors = PushSkinColors();
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(255, 255, 255, 255));
  ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0xC2, 0xC2, 0xC2, 255));
  g_skin_colors += 2;
  // Bordure 1px ARRONDIE (comme BeginRoDescWindow) EN PLUS du cadre sysbox : donne
  // aux sous-panneaux le même liseré arrondi que la fenêtre de description parente
  // (le sysbox seul rend un cadre carré/parfois non visible sur ces petits panneaux).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_cfg.alpha);
  const float e = (float)skin::kSysboxLm.w;  // 14
  // Côtés = e+2 (marge intérieure vs cadre sysbox) ; haut/bas = 6px (panneaux
  // cartes/options COMPACTS, demandé).
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(e + 2.0f, 6.0f));
  g_skin_vars = 4;

  const bool open = ImGui::Begin(id, nullptr, imgui_window_flags);
  ImGuiWindow* w = ImGui::GetCurrentWindow();
  if (open && w && !w->Hidden && !w->Collapsed) {
    EnsureTex("sysbox_lu.bmp", skin::kSysboxLu, g_sb_lu);
    EnsureTex("sysbox_mu.bmp", skin::kSysboxMu, g_sb_mu);
    EnsureTex("sysbox_ru.bmp", skin::kSysboxRu, g_sb_ru);
    EnsureTex("sysbox_lm.bmp", skin::kSysboxLm, g_sb_lm);
    EnsureTex("sysbox_rm.bmp", skin::kSysboxRm, g_sb_rm);
    EnsureTex("sysbox_ld.bmp", skin::kSysboxLd, g_sb_ld);
    EnsureTex("sysbox_md.bmp", skin::kSysboxMd, g_sb_md);
    EnsureTex("sysbox_rd.bmp", skin::kSysboxRd, g_sb_rd);
    const ImRect wr = w->Rect();
    ImDrawList* dl = w->DrawList;
    const float fx0 = wr.Min.x, fy0 = wr.Min.y, fx1 = wr.Max.x, fy1 = wr.Max.y;
    dl->PushClipRect(wr.Min, wr.Max, false);
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, g_sb_lu, ImVec2(fx0, fy0), ImVec2(fx0 + e, fy0 + e));
    BlitStretch(dl, g_sb_ru, ImVec2(fx1 - e, fy0), ImVec2(fx1, fy0 + e));
    BlitStretch(dl, g_sb_ld, ImVec2(fx0, fy1 - e), ImVec2(fx0 + e, fy1));
    BlitStretch(dl, g_sb_rd, ImVec2(fx1 - e, fy1 - e), ImVec2(fx1, fy1));
    BlitStretch(dl, g_sb_mu, ImVec2(fx0 + e, fy0), ImVec2(fx1 - e, fy0 + e));
    BlitStretch(dl, g_sb_md, ImVec2(fx0 + e, fy1 - e), ImVec2(fx1 - e, fy1));
    BlitStretch(dl, g_sb_lm, ImVec2(fx0, fy0 + e), ImVec2(fx0 + e, fy1 - e));
    BlitStretch(dl, g_sb_rm, ImVec2(fx1 - e, fy0 + e), ImVec2(fx1, fy1 - e));
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    dl->PopClipRect();
  }
  return open;
}

void EndRoDescPanel() { EndRoWindow(); }

float DescPanelEdge() { return (float)skin::kSysboxLm.w; }

// Cadre « panneau desc » (fond clair + sysbox 9-slice) dessiné à la main dans un
// rect arbitraire, sur un ImDrawList arbitraire. Même art que BeginRoDescPanel,
// mais sans créer de fenêtre ImGui -> l'appelant peut le poser sur la draw list
// de sa fenêtre parente (=> suit son z-order). L'appelant gère le clip global.
void DrawDescPanelFrame(ImDrawList* dl, float x0, float y0, float x1, float y1,
                        bool fill_bg) {
  if (!dl || x1 <= x0 || y1 <= y0) return;
  const ImVec2 p0(x0, y0), p1(x1, y1);
  // Fond clair (suit alpha/luminosité du skin comme les autres pièces main). Sauté
  // quand l'appelant a déjà peint un fond ARRONDI : ce rect-ci est à angles droits
  // et recouvrirait ses coins.
  if (fill_bg)
    dl->AddRectFilled(p0, p1, ApplySkinTint(IM_COL32(255, 255, 255, 255)), 0.0f);
  EnsureTex("sysbox_lu.bmp", skin::kSysboxLu, g_sb_lu);
  EnsureTex("sysbox_mu.bmp", skin::kSysboxMu, g_sb_mu);
  EnsureTex("sysbox_ru.bmp", skin::kSysboxRu, g_sb_ru);
  EnsureTex("sysbox_lm.bmp", skin::kSysboxLm, g_sb_lm);
  EnsureTex("sysbox_rm.bmp", skin::kSysboxRm, g_sb_rm);
  EnsureTex("sysbox_ld.bmp", skin::kSysboxLd, g_sb_ld);
  EnsureTex("sysbox_md.bmp", skin::kSysboxMd, g_sb_md);
  EnsureTex("sysbox_rd.bmp", skin::kSysboxRd, g_sb_rd);
  const float e = (float)skin::kSysboxLm.w;
  dl->PushClipRect(p0, p1, false);
  dl->AddCallback(ImCb_PointFilter, nullptr);
  BlitStretch(dl, g_sb_lu, ImVec2(x0, y0), ImVec2(x0 + e, y0 + e));
  BlitStretch(dl, g_sb_ru, ImVec2(x1 - e, y0), ImVec2(x1, y0 + e));
  BlitStretch(dl, g_sb_ld, ImVec2(x0, y1 - e), ImVec2(x0 + e, y1));
  BlitStretch(dl, g_sb_rd, ImVec2(x1 - e, y1 - e), ImVec2(x1, y1));
  BlitStretch(dl, g_sb_mu, ImVec2(x0 + e, y0), ImVec2(x1 - e, y0 + e));
  BlitStretch(dl, g_sb_md, ImVec2(x0 + e, y1 - e), ImVec2(x1 - e, y1));
  BlitStretch(dl, g_sb_lm, ImVec2(x0, y0 + e), ImVec2(x0 + e, y1 - e));
  BlitStretch(dl, g_sb_rm, ImVec2(x1 - e, y0 + e), ImVec2(x1, y1 - e));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  dl->PopClipRect();
}

// Faux-gras : ImGui n'a qu'une graisse chargée, on re-dessine le texte décalé d'un
// pixel. Même recette que les textes du chatbox NPC et de la barre de skills.
namespace {
// Posé par RoToggleButton le temps d'un appel : le bouton se dessine alors enfoncé
// (art « press » + libellé gras) sans que la souris ait à le tenir.
bool g_force_button_active = false;

void DrawButtonLabel(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* label, bool bold) {
  const char* end = ImGui::FindRenderedTextEnd(label);
  dl->AddText(pos, color, label, end);
  if (bold) dl->AddText(ImVec2(pos.x + 1.0f, pos.y), color, label, end);
}
}  // namespace

// `active` = bouton « enclenché » (outil courant, option retenue…) : le libellé passe
// en gras et le fond garde l'art « pressé », même quand la souris est ailleurs.
bool RoToggleButton(const char* label, bool active, float w, float h) {
  g_force_button_active = active;
  const bool clicked = RoButton(label, w, h);
  g_force_button_active = false;
  return clicked;
}

bool RoButton(const char* label, float w, float h) {
  EnsureTex("basic_interface\\btn_out_left.bmp",    skin::kBtnOutLeft,    g_btn_out_l);
  EnsureTex("basic_interface\\btn_out_mid.bmp",     skin::kBtnOutMid,     g_btn_out_m);
  EnsureTex("basic_interface\\btn_out_right.bmp",   skin::kBtnOutRight,   g_btn_out_r);
  EnsureTex("basic_interface\\btn_over_left.bmp",   skin::kBtnOverLeft,   g_btn_over_l);
  EnsureTex("basic_interface\\btn_over_mid.bmp",    skin::kBtnOverMid,    g_btn_over_m);
  EnsureTex("basic_interface\\btn_over_right.bmp",  skin::kBtnOverRight,  g_btn_over_r);
  EnsureTex("basic_interface\\btn_press_left.bmp",  skin::kBtnPressLeft,  g_btn_press_l);
  EnsureTex("basic_interface\\btn_press_mid.bmp",   skin::kBtnPressMid,   g_btn_press_m);
  EnsureTex("basic_interface\\btn_press_right.bmp", skin::kBtnPressRight, g_btn_press_r);

  const float capL = (float)skin::kBtnOutLeft.w;
  const float capR = (float)skin::kBtnOutRight.w;
  const float nativeH = (float)skin::kBtnOutLeft.h;
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  if (w <= 0.0f) w = ts.x + capL + capR + 12.0f;
  if (h <= 0.0f) h = nativeH;

  ImGui::PushID(label);
  const bool clicked = ImGui::InvisibleButton("##rb", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive() || g_force_button_active;
  if (hovered) SetHoverCursor(kRoCursorHand);
  const ImVec2 p0 = ImGui::GetItemRectMin();
  const ImVec2 p1 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Etat desactive (BeginDisabled) : ImGui ne modifie PAS le visuel des widgets
  // dessines main -> on grise nous-memes (art estompe + texte grise).
  const bool disabled =
      ImGui::GetCurrentContext() &&
      (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const ImU32 tint = disabled ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;

  const SkinTex *l, *m, *r;
  if (held) { l = &g_btn_press_l; m = &g_btn_press_m; r = &g_btn_press_r; }
  else if (hovered) { l = &g_btn_over_l; m = &g_btn_over_m; r = &g_btn_over_r; }
  else { l = &g_btn_out_l; m = &g_btn_out_m; r = &g_btn_out_r; }

  if (l->tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, *l, p0, ImVec2(p0.x + capL, p1.y), tint);
    BlitStretch(dl, *r, ImVec2(p1.x - capR, p0.y), p1, tint);
    BlitStretch(dl, *m, ImVec2(p0.x + capL, p0.y), ImVec2(p1.x - capR, p1.y), tint);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {
    dl->AddRectFilled(p0, p1,
                      disabled ? IM_COL32(210, 216, 228, 110)
                               : IM_COL32(210, 216, 228, 255),
                      2.0f);
    dl->AddRect(p0, p1, IM_COL32(96, 112, 152, 255), 2.0f);
  }

  const ImVec2 tp(p0.x + (w - ts.x) * 0.5f,
                  p0.y + (h - ts.y) * 0.5f + (held ? 1.0f : 0.0f));
  // Bouton enfoncé : libellé en gras. L'art « press » se distingue mal de l'état
  // survolé sur les petites tailles, la graisse tranche tout de suite.
  DrawButtonLabel(dl, tp,
                  disabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                           : ImGui::GetColorU32(ImGuiCol_Text),
                  label, held && !disabled);
  ImGui::PopID();
  return clicked;
}
// Petit bouton (ex. pour les + - x ) : même design que RoButton mais plus petit
bool RoSmallButton(const char* label, float w, float h) {
  EnsureTex("basic_interface\\sbtn_out_left.bmp",    skin::ksBtnOutLeft,    g_sbtn_out_l);
  EnsureTex("basic_interface\\sbtn_out_mid.bmp",     skin::ksBtnOutMid,     g_sbtn_out_m);
  EnsureTex("basic_interface\\sbtn_out_right.bmp",   skin::ksBtnOutRight,   g_sbtn_out_r);
  EnsureTex("basic_interface\\sbtn_over_left.bmp",   skin::ksBtnOverLeft,   g_sbtn_over_l);
  EnsureTex("basic_interface\\sbtn_over_mid.bmp",    skin::ksBtnOverMid,    g_sbtn_over_m);
  EnsureTex("basic_interface\\sbtn_over_right.bmp",  skin::ksBtnOverRight,  g_sbtn_over_r);
  EnsureTex("basic_interface\\sbtn_press_left.bmp",  skin::ksBtnPressLeft,  g_sbtn_press_l);
  EnsureTex("basic_interface\\sbtn_press_mid.bmp",   skin::ksBtnPressMid,   g_sbtn_press_m);
  EnsureTex("basic_interface\\sbtn_press_right.bmp", skin::ksBtnPressRight, g_sbtn_press_r);

  const float capL = (float)skin::ksBtnOutLeft.w;
  const float capR = (float)skin::ksBtnOutRight.w;
  const float nativeH = (float)skin::ksBtnOutLeft.h;
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  if (w <= 0.0f) w = ts.x + capL + capR; // +12px pour RoButton, pas pour le petit bouton
  if (h <= 0.0f) h = nativeH;

  // Resserre le bouton contre le widget qui le précède SUR LA MÊME LIGNE (le skin
  // RO a déjà sa propre marge dans l'art, l'ItemSpacing d'ImGui l'éloigne trop).
  // Uniquement en SameLine : quand le bouton OUVRE la ligne, il n'y a rien à
  // resserrer et ces 3 px le font mordre sur la marge gauche — hors clip rect de
  // la fenêtre (ou de la colonne), donc rogné à gauche.
  if (ImGui::GetCurrentWindow()->DC.IsSameLine)
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - 3.0f);

  // Le petit bouton est plus court que du texte : on le pose 3 px sous le haut
  // de la ligne pour le recentrer. Surtout PAS en déplaçant le curseur Y : quand
  // le bouton OUVRE la ligne, ce décalage devient l'origine de la ligne, et les
  // boutons suivants (posés en SameLine, qui les ramène à cette origine) se
  // décalent encore de 3 px — le premier bouton apparaissait alors 3 px plus
  // haut que ses voisins. On réserve donc un item 3 px plus haut que l'art et on
  // dessine l'art dans sa partie basse : l'origine de la ligne reste intacte.
  const float art_drop_y = 3.0f;

  ImGui::PushID(label);
  const bool clicked = ImGui::InvisibleButton("##rb", ImVec2(w, h + art_drop_y));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive() || g_force_button_active;
  if (hovered) SetHoverCursor(kRoCursorHand);
  const ImVec2 p0(ImGui::GetItemRectMin().x,
                  ImGui::GetItemRectMin().y + art_drop_y);
  const ImVec2 p1 = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // Etat desactive (BeginDisabled) : ImGui ne modifie PAS le visuel des widgets
  // dessines main -> on grise nous-memes (art estompe + texte grise).
  const bool disabled =
      ImGui::GetCurrentContext() &&
      (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const ImU32 tint = disabled ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;

  const SkinTex *l, *m, *r;
  if (held) { l = &g_sbtn_press_l; m = &g_sbtn_press_m; r = &g_sbtn_press_r; }
  else if (hovered) { l = &g_sbtn_over_l; m = &g_sbtn_over_m; r = &g_sbtn_over_r; }
  else { l = &g_sbtn_out_l; m = &g_sbtn_out_m; r = &g_sbtn_out_r; }

  if (l->tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, *l, p0, ImVec2(p0.x + capL, p1.y), tint);
    BlitStretch(dl, *r, ImVec2(p1.x - capR, p0.y), p1, tint);
    BlitStretch(dl, *m, ImVec2(p0.x + capL, p0.y), ImVec2(p1.x - capR, p1.y), tint);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {
    dl->AddRectFilled(p0, p1,
                      disabled ? IM_COL32(210, 216, 228, 110)
                               : IM_COL32(210, 216, 228, 255),
                      2.0f);
    dl->AddRect(p0, p1, IM_COL32(96, 112, 152, 255), 2.0f);
  }

  const ImVec2 tp(p0.x + (w - ts.x) * 0.5f,
                  p0.y + (h - ts.y) * 0.5f - 1.0f);  // -1 pour centrer le texte correctement dans la case
  DrawButtonLabel(dl, tp,
                  disabled ? ImGui::GetColorU32(ImGuiCol_TextDisabled)
                           : ImGui::GetColorU32(ImGuiCol_Text),
                  label, held && !disabled);
  ImGui::PopID();
  return clicked;
}

bool RoCheckbox(const char* label, bool* v) {
  if (!v) return false;
  EnsureTex("checkbox_0.bmp", skin::kCheckbox0, g_cb0);
  EnsureTex("checkbox_1.bmp", skin::kCheckbox1, g_cb1);
  const float sz = (float)skin::kCheckbox0.w;  // 10x10
  const float gapx = 4.0f;
  ImGui::PushID(label);
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  const float h = sz > ts.y ? sz : ts.y;
  const bool pressed = ImGui::InvisibleButton("##cb", ImVec2(sz + gapx + ts.x, h));
  if (ImGui::IsItemHovered()) SetHoverCursor(kRoCursorHand);
  if (pressed) *v = !*v;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  // Position arrondie à l'entier : sinon (h-sz)/2 fractionnaire + sampling POINT
  // coupe la dernière ligne de pixels (bas de la case « manquant »).
  // +2 : la case et le label sont posés sur la ligne de base naturelle (haut de
  // ligne) plutôt que 2px au-dessus, sinon le « (?) » d'un HelpMarker en SameLine
  // (dessiné à start.y) se retrouve décalé sous le label.
  const ImVec2 bmin(ImFloor(start.x), ImFloor(start.y + (h - sz) * 0.5f + 2.0f));
  const ImVec2 bmax(bmin.x + sz, bmin.y + sz);
  if (g_cb0.tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, *v ? g_cb1 : g_cb0, bmin, bmax);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {
    dl->AddRect(bmin, bmax, IM_COL32(96, 112, 152, 255));
    if (*v) dl->AddRectFilled(ImVec2(bmin.x + 2, bmin.y + 2),
                              ImVec2(bmax.x - 2, bmax.y - 2),
                              IM_COL32(96, 112, 152, 255));
  }
  dl->AddText(ImVec2(start.x + sz + gapx, start.y + (h - ts.y) * 0.5f), // aligné case + HelpMarker (voir bmin)
              ImGui::GetColorU32(ImGuiCol_Text), label,
              ImGui::FindRenderedTextEnd(label));
  ImGui::PopID();
  return pressed;
}

// Bouton radio skinné : pièces client radiobtn_on/off.bmp (chargées UNIQUEMENT depuis
// le client, pas de blob de repli -> repli dessiné à plat si absent). Même structure
// que RoCheckbox (image + label cliquable), mais on/off = deux images pleines et le
// repli est un cercle (vocabulaire radio). Renvoie true si cliqué.
bool RadioImage(const char* label, bool selected) {
  static SkinTex s_on, s_off;
  EnsureTexClient("radiobtn_on.bmp", s_on);
  EnsureTexClient("radiobtn_off.bmp", s_off);
  const SkinTex& t = selected ? s_on : s_off;
  const float sz = (t.h > 0) ? (float)t.h : 11.0f;  // taille native, repli 11px
  const float gapx = 4.0f;
  ImGui::PushID(label);
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  const float h = sz > ts.y ? sz : ts.y;
  const bool pressed = ImGui::InvisibleButton("##rb", ImVec2(sz + gapx + ts.x, h));
  if (ImGui::IsItemHovered()) SetHoverCursor(kRoCursorHand);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 bmin(ImFloor(start.x), ImFloor(start.y + (h - sz) * 0.5f + 2.0f));
  const ImVec2 bmax(bmin.x + sz, bmin.y + sz);
  if (t.tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, t, bmin, bmax);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {  // repli à plat : cercle vide / point plein
    const ImVec2 c(bmin.x + sz * 0.5f, bmin.y + sz * 0.5f);
    dl->AddCircle(c, sz * 0.5f, IM_COL32(96, 112, 152, 255), 16, 1.5f);
    if (selected)
      dl->AddCircleFilled(c, sz * 0.30f, IM_COL32(96, 112, 152, 255), 16);
  }
  dl->AddText(ImVec2(start.x + sz + gapx, start.y + (h - ts.y) * 0.5f),
              ImGui::GetColorU32(ImGuiCol_Text), label,
              ImGui::FindRenderedTextEnd(label));
  ImGui::PopID();
  return pressed;
}

// ── Slider = scrollbar RO horizontale ─────────────────────────────────────────
// RO n'a pas de « slider » : le vocabulaire visuel équivalent est la scrollbar
// HORIZONTALE du client — piste/flèches scroll1left|mid|right + thumb
// scroll1bar_left|mid|right. On réutilise EXACTEMENT ces pièces (aucune rotation :
// elles sont déjà horizontales). Le comportement reste celui d'ImGui
// (SliderBehavior + Ctrl+clic = saisie directe) : seules l'interaction des flèches
// et la peinture sont à nous.
// Les tailles ne sont pas dans ro_skin_blobs (pièces non utilisées par ailleurs) :
// on charge via EnsureTexClient et on lit w/h de la texture, avec un repli 13/4px
// (dimensions de la scrollbar verticale) tant qu'elle n'est pas prête.
static bool RoSliderScalar(const char* label, ImGuiDataType dt, void* p_data,
                           const void* p_min, const void* p_max,
                           const char* format, ImGuiSliderFlags flags,
                           float arrow_step) {
  ImGuiWindow* win = ImGui::GetCurrentWindow();
  if (!win || win->SkipItems) return false;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiStyle& style = g.Style;
  const ImGuiID id = win->GetID(label);

  EnsureTexClient("scroll1left.bmp", g_s1l);
  EnsureTexClient("scroll1mid.bmp", g_s1m);
  EnsureTexClient("scroll1right.bmp", g_s1r);
  EnsureTexClient("scroll1bar_left.bmp", g_s1bar_l);
  EnsureTexClient("scroll1bar_mid.bmp", g_s1bar_m);
  EnsureTexClient("scroll1bar_right.bmp", g_s1bar_r);

  if (format == nullptr) format = ImGui::DataTypeGetInfo(dt)->PrintFmt;
  char value_buf[64];
  const char* value_end =
      value_buf + ImGui::DataTypeFormatString(value_buf, IM_COUNTOF(value_buf),
                                              dt, p_data, format);

  const char* label_end = ImGui::FindRenderedTextEnd(label);
  const ImVec2 label_size = ImGui::CalcTextSize(label, label_end, false);
  // La valeur est affichée À DROITE de la barre (une scrollbar RO fait 13px de
  // haut : le texte n'y tient pas centré comme sur un slider ImGui). Largeur
  // réservée avec un plancher, sinon la barre se raccourcit/rallonge à chaque
  // changement de nombre de chiffres (barre qui « respire »).
  const float value_w =
      ImMax(ImGui::CalcTextSize(value_buf, value_end).x,
            ImGui::CalcTextSize("000000").x);
  const float barh = g_s1l.tex ? (float)g_s1l.h : 13.0f;  // hauteur de l'art
  const float w = ImGui::CalcItemWidth();
  const float frame_h =
      ImMax(barh, label_size.y + style.FramePadding.y * 2.0f);
  // Additions écrites à la main : les opérateurs ImVec2 (IMGUI_DEFINE_MATH_OPERATORS)
  // ne sont pas activés dans cette unité de compilation.
  const ImVec2 pos = win->DC.CursorPos;
  const ImRect frame_bb(pos, ImVec2(pos.x + w, pos.y + frame_h));
  const float label_extra =
      label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f;
  const ImRect total_bb(frame_bb.Min,
                        ImVec2(frame_bb.Max.x + label_extra, frame_bb.Max.y));

  const bool temp_input_allowed = (flags & ImGuiSliderFlags_NoInput) == 0;
  ImGui::ItemSize(total_bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(total_bb, id, &frame_bb,
                      temp_input_allowed ? ImGuiItemFlags_Inputable : 0))
    return false;

  // Géométrie : barre (hors zone valeur) = flèche gauche + piste + flèche droite.
  const float bary = ImFloor(frame_bb.Min.y + (frame_h - barh) * 0.5f);
  const ImRect bar(frame_bb.Min.x, bary,
                   frame_bb.Max.x - value_w - style.ItemInnerSpacing.x,
                   bary + barh);
  const float arrow = g_s1l.tex ? (float)g_s1l.w : barh;
  const bool has_arrows = (bar.GetWidth() > arrow * 2.0f + 8.0f);
  const ImRect track(has_arrows ? bar.Min.x + arrow : bar.Min.x, bar.Min.y,
                     has_arrows ? bar.Max.x - arrow : bar.Max.x, bar.Max.y);

  const bool hovered = ImGui::ItemHoverable(frame_bb, id, g.LastItemData.ItemFlags);
  if (hovered) SetHoverCursor(kRoCursorHand);
  const bool over_left =
      has_arrows && hovered &&
      ImGui::IsMouseHoveringRect(bar.Min, ImVec2(track.Min.x, bar.Max.y), false);
  const bool over_right =
      has_arrows && hovered &&
      ImGui::IsMouseHoveringRect(ImVec2(track.Max.x, bar.Min.y), bar.Max, false);

  // Activation (calquée sur ImGui::SliderScalar), MAIS un clic sur une flèche ne
  // saisit pas le thumb : il ne doit faire qu'un pas.
  bool temp_input_is_active = temp_input_allowed && ImGui::TempInputIsActive(id);
  if (!temp_input_is_active) {
    const bool clicked = hovered && !over_left && !over_right &&
                         ImGui::IsMouseClicked(0, ImGuiInputFlags_None, id);
    const bool make_active = (clicked || g.NavActivateId == id);
    if (make_active && clicked) ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
    if (make_active && temp_input_allowed)
      if ((clicked && g.IO.KeyCtrl) ||
          (g.NavActivateId == id && (g.NavActivateFlags & ImGuiActivateFlags_PreferInput)))
        temp_input_is_active = true;
    if (make_active && !temp_input_is_active) {
      ImGui::SetActiveID(id, win);
      ImGui::SetFocusID(id, win);
      ImGui::FocusWindow(win);
      g.ActiveIdUsingNavDirMask |= (1 << ImGuiDir_Left) | (1 << ImGuiDir_Right);
    }
  }
  if (temp_input_is_active) {  // Ctrl+clic : saisie directe
    const bool clamp_enabled = (flags & ImGuiSliderFlags_ClampOnInput) != 0;
    return ImGui::TempInputScalar(frame_bb, id, label, dt, p_data, format,
                                  clamp_enabled ? p_min : nullptr,
                                  clamp_enabled ? p_max : nullptr);
  }

  // Flèches : ajustement À L'UNITÉ (même pas que la molette : 1 pour un entier,
  // 0.01 pour un flottant, sauf pas explicite ; Shift = pas ×10). Clic = 1 pas,
  // maintien = répétition, comme la scrollbar native.
  bool value_changed = false;
  if ((over_left || over_right) &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left, /*repeat=*/true)) {
    const float dir = over_left ? -1.0f : 1.0f;
    const float shift = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
    if (dt == ImGuiDataType_Float) {
      float* v = (float*)p_data;
      const float lo = *(const float*)p_min, hi = *(const float*)p_max;
      const float st = (arrow_step > 0.0f ? arrow_step : 0.01f) * shift;
      const float nv = ImClamp(*v + dir * st, lo, hi);
      if (nv != *v) { *v = nv; value_changed = true; }
    } else if (dt == ImGuiDataType_S32) {
      int* v = (int*)p_data;
      const int lo = *(const int*)p_min, hi = *(const int*)p_max;
      int st = (int)(arrow_step > 0.0f ? arrow_step : 1.0f) * (int)shift;
      if (st <= 0) st = 1;  // pas mini = l'unité
      const int nv = ImClamp(*v + (int)dir * st, lo, hi);
      if (nv != *v) { *v = nv; value_changed = true; }
    }
  }

  // Comportement slider sur la PISTE (entre les flèches) : le thumb ne passe
  // jamais sous une flèche, comme sur une scrollbar.
  ImRect grab_bb;
  ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 16.0f);  // thumb « scrollbar »
  if (ImGui::SliderBehavior(track, id, dt, p_data, p_min, p_max, format, flags,
                            &grab_bb))
    value_changed = true;
  ImGui::PopStyleVar();
  if (value_changed) ImGui::MarkItemEdited(id);

  // ── Dessin ──
  ImDrawList* dl = win->DrawList;
  if (g_s1m.tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    // Piste débordant de 2px sous les flèches -> jointure sans trou (cf. la
    // scrollbar verticale).
    BlitStretch(dl, g_s1m, ImVec2(track.Min.x - 2.0f, bar.Min.y),
                ImVec2(track.Max.x + 2.0f, bar.Max.y));
    if (has_arrows) {
      BlitStretch(dl, g_s1l, bar.Min, ImVec2(track.Min.x, bar.Max.y));
      BlitStretch(dl, g_s1r, ImVec2(track.Max.x, bar.Min.y), bar.Max);
    }
    const float cap = g_s1bar_l.tex ? (float)g_s1bar_l.w : 4.0f;
    const float gx0 = ImFloor(grab_bb.Min.x), gx1 = ImFloor(grab_bb.Max.x);
    if (gx1 > gx0) {
      BlitStretch(dl, g_s1bar_l, ImVec2(gx0, bar.Min.y),
                  ImVec2(gx0 + cap, bar.Max.y));
      BlitStretch(dl, g_s1bar_r, ImVec2(gx1 - cap, bar.Min.y),
                  ImVec2(gx1, bar.Max.y));
      if (gx1 - gx0 > cap * 2.0f)
        BlitStretch(dl, g_s1bar_m, ImVec2(gx0 + cap, bar.Min.y),
                    ImVec2(gx1 - cap, bar.Max.y));
    }
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {  // repli : textures pas (encore) disponibles
    dl->AddRectFilled(bar.Min, bar.Max, ImGui::GetColorU32(ImGuiCol_FrameBg));
    dl->AddRect(bar.Min, bar.Max, ImGui::GetColorU32(ImGuiCol_Border));
    if (grab_bb.Max.x > grab_bb.Min.x)
      dl->AddRectFilled(ImVec2(grab_bb.Min.x, bar.Min.y),
                        ImVec2(grab_bb.Max.x, bar.Max.y),
                        ImGui::GetColorU32(ImGuiCol_SliderGrab), 2.0f);
  }

  // Valeur (alignée à droite) puis label, comme un slider ImGui.
  const ImVec2 vs = ImGui::CalcTextSize(value_buf, value_end);
  dl->AddText(ImVec2(frame_bb.Max.x - vs.x,
                     frame_bb.Min.y + (frame_h - vs.y) * 0.5f),
              ImGui::GetColorU32(ImGuiCol_Text), value_buf, value_end);
  if (label_size.x > 0.0f)
    ImGui::RenderText(ImVec2(frame_bb.Max.x + style.ItemInnerSpacing.x,
                             frame_bb.Min.y + style.FramePadding.y),
                      label, label_end, false);
  return value_changed;
}

bool RoSliderFloat(const char* label, float* v, float lo, float hi,
                   const char* format, float arrow_step, int flags) {
  return RoSliderScalar(label, ImGuiDataType_Float, v, &lo, &hi, format, flags,
                        arrow_step);
}

bool RoSliderInt(const char* label, int* v, int lo, int hi, const char* format,
                 int arrow_step, int flags) {
  return RoSliderScalar(label, ImGuiDataType_S32, v, &lo, &hi, format, flags,
                        (float)arrow_step);
}

bool RoBeginCombo(const char* label, const char* preview_value) {
  EnsureTexClient("basic_interface\\txtbox_btn_a.bmp", g_tb_btn_a);
  EnsureTexClient("basic_interface\\txtbox_btn_b.bmp", g_tb_btn_b);
  EnsureTexClient("basic_interface\\txtbox_btn_c.bmp", g_tb_btn_c);

  const float arrow_w = g_tb_btn_a.tex ? (float)g_tb_btn_a.w : 16.0f;
  const float h = g_tb_btn_a.tex ? (float)g_tb_btn_a.h : ImGui::GetFrameHeight();
  const float w = ImGui::CalcItemWidth();

  ImGui::PushID(label);
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const bool clicked = ImGui::InvisibleButton("##rcb", ImVec2(w, h));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  if (hovered) SetHoverCursor(kRoCursorHand);
  const ImVec2 p1(p0.x + w, p0.y + h);
  const ImVec2 arrowMin(p1.x - arrow_w, p0.y);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  const RoSkinConfig& c = g_cfg;
  // Etat desactive (BeginDisabled) : ImGui ne modifie PAS les widgets dessines
  // main (ColorConvertFloat4ToU32 ignore style.Alpha) -> on grise nous-memes.
  const bool disabled =
      ImGui::GetCurrentContext() &&
      (ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const float da = disabled ? 0.4f : 1.0f;
  const auto U32 = [da](const float* a) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(a[0], a[1], a[2], a[3] * da));
  };
  // Champ : fond « input » + bordure (le txtbox natif est une simple boîte bordée).
  dl->AddRectFilled(p0, ImVec2(arrowMin.x, p1.y), U32(c.input_col));
  dl->AddRect(p0, p1, U32(c.border_col));
  // Bouton flèche : texture native txtbox_btn (états normal/survol/pressé), teintée
  // (alpha réduit) quand désactivé pour signaler l'état grisé.
  const ImU32 tint = disabled ? IM_COL32(255, 255, 255, 90) : IM_COL32_WHITE;
  const SkinTex& btn = held ? g_tb_btn_c : (hovered ? g_tb_btn_b : g_tb_btn_a);
  if (btn.tex) {
    dl->AddCallback(ImCb_PointFilter, nullptr);
    BlitStretch(dl, btn, arrowMin, p1, tint);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  } else {  // repli : triangle vers le bas
    const float cx = (arrowMin.x + p1.x) * 0.5f, cy = (p0.y + p1.y) * 0.5f;
    dl->AddTriangleFilled(ImVec2(cx - 3, cy - 2), ImVec2(cx + 3, cy - 2),
                          ImVec2(cx, cy + 3), U32(c.body_text));
  }
  // Texte de sélection (clippé au champ).
  if (preview_value && preview_value[0]) {
    const char* end = ImGui::FindRenderedTextEnd(preview_value);
    const float th = ImGui::CalcTextSize(preview_value, end).y;
    dl->PushClipRect(p0, ImVec2(arrowMin.x, p1.y), true);
    dl->AddText(ImVec2(p0.x + 4.0f, p0.y + (h - th) * 0.5f), U32(c.body_text),
                preview_value, end);
    dl->PopClipRect();
  }

  // Label à droite de la boîte (comme un combo ImGui standard) : partie visible
  // avant « ## », centrée verticalement sur le champ. Dessiné en VRAI item ImGui
  // pour que le layout réserve sa largeur : un SameLine() côté appelant (ex.
  // HelpMarker) démarre alors APRÈS le label et non par-dessus.
  const char* label_end = ImGui::FindRenderedTextEnd(label);
  if (label != label_end) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    ImGui::SetCursorScreenPos(ImVec2(
        p1.x + style.ItemInnerSpacing.x,
        p0.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImVec4(c.body_text[0], c.body_text[1], c.body_text[2],
                                 c.body_text[3] * da));
    ImGui::TextUnformatted(label, label_end);
    ImGui::PopStyleColor();
  }

  if (clicked) ImGui::OpenPopup("##rcb_pop");

  // Liste en popup : largeur mini = champ, fond « corps » RO, sélection bleue
  // (onglet actif) + texte corps foncé.
  // ⚠ Les contraintes de taille AVANT le calcul de position : c'est la taille
  // CONTRAINTE que le placement doit connaître (même ordre que BeginComboPopup).
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 0), ImVec2(FLT_MAX, FLT_MAX));

  // 🔴 SOUS le champ quand il y a la place, AU-DESSUS sinon. Un `SetNextWindowPos`
  // en dur ne peut pas le faire : Begin() ne recadre QUE les positions qu'il a
  // choisies lui-même (`window_pos_set_by_api` court-circuite `ClampWindowPos` et
  // `FindBestWindowPosForPopup`), si bien que la liste d'une chatbox posée en bas
  // de l'écran se dépliait dans le vide. On reprend donc la recette d'ImGui
  // lui-même (`BeginComboPopup`) : aller lire la taille que la popup AURA à la
  // frame suivante, puis lui chercher un côté — la politique « ComboBox » essaie
  // dessous, puis au-dessus, en gardant un bord commun avec le champ.
  // La popup n'a pas encore de fenêtre à sa toute première ouverture : on garde
  // alors le placement sous le champ, invisible de toute façon (ImGui saute le
  // rendu de la frame où il mesure).
  const ImRect combo_bb(p0, p1);
  char popup_name[24];
  ImFormatString(popup_name, IM_COUNTOF(popup_name), "##Popup_%08x",
                 ImGui::GetCurrentWindow()->GetID("##rcb_pop"));
  ImGuiWindow* popup_win = ImGui::FindWindowByName(popup_name);
  if (popup_win != nullptr && popup_win->WasActive) {
    const ImVec2 size_expected = ImGui::CalcWindowNextAutoFitSize(popup_win);
    // Réinitialisé à chaque frame, comme ImGui : sans ça le côté retenu la fois
    // précédente est re-tenté en premier et la liste reste collée en bas.
    popup_win->AutoPosLastDirection = ImGuiDir_Down;
    const ImRect r_outer = ImGui::GetPopupAllowedExtentRect(popup_win);
    ImGui::SetNextWindowPos(ImGui::FindBestWindowPosForPopupEx(
        combo_bb.GetBL(), size_expected, &popup_win->AutoPosLastDirection, r_outer,
        combo_bb, ImGuiPopupPositionPolicy_ComboBox));
  } else {
    ImGui::SetNextWindowPos(ImVec2(p0.x, p1.y + 1.0f));
  }
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(c.body_col[0], c.body_col[1],
                                                 c.body_col[2], c.body_col[3]));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(c.border_col[0], c.border_col[1],
                                                c.border_col[2], c.border_col[3]));
  ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(c.tab_col[0], c.tab_col[1],
                                                c.tab_col[2], c.tab_col[3]));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(c.tab_col[0], c.tab_col[1],
                                                       c.tab_col[2], 0.6f));
  ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.body_text[0], c.body_text[1],
                                              c.body_text[2], c.body_text[3]));
  const bool open = ImGui::BeginPopup("##rcb_pop");
  if (!open) {
    ImGui::PopStyleColor(5);
    ImGui::PopID();
  }
  return open;
}

void RoEndCombo() {
  ImGui::EndPopup();
  ImGui::PopStyleColor(5);
  ImGui::PopID();
}

bool RoCombo(const char* label, int *current_item, const char* const items[], int items_count) {
  bool changed = false;
  // 🔴 LES LIBELLÉS SE TRADUISENT ICI, ET C'EST LE SEUL ENDROIT POSSIBLE.
  // Les appelants passent presque tous un tableau STATIQUE de `const char*` :
  // un i18n::Tr posé à sa définition serait évalué une fois, au chargement de la
  // DLL, et resterait figé en français pour toujours. En traduisant à la lecture,
  // un seul point de code rend traduisibles tous les combos du projet — et le
  // français ne paie rien, Tr rendant alors son argument.
  if (ro::RoBeginCombo(label, i18n::Tr(items[*current_item]))) {
    for (int i = 0; i < items_count; ++i) {
      const bool selected = (*current_item == i);
      // `changed` ne doit être levé QUE sur un vrai changement de sélection. Il était
      // auparavant posé dans le corps du if, donc vrai à chaque frame où le popup était
      // ouvert : les appelants font `changed |= RoCombo(...)` puis `if (changed)
      // SaveSettings()`, ce qui réécrivait bourgeon_settings.yaml à 60 Hz tant que le
      // menu restait déroulé. Re-cliquer l'entrée déjà active n'est pas un changement.
      if (ImGui::Selectable(i18n::Tr(items[i]), selected) && !selected) {
        *current_item = i;
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ro::RoEndCombo();
  }
  return changed;
}

// ── Onglets (tab bar) ─────────────────────────────────────────────────────────
// Pièces du client : tabh_{l,m,r}_{on,off}.bmp (5x20, basic_interface). Ce n'est
// pas un fond plein mais un CONTOUR d'1 px (#ADADAD) à intérieur transparent :
// `l`/`r` portent le coin haut arrondi (3x3) puis le bord vertical, `m` la ligne
// du haut. L'état « on » (actif) n'a PAS de ligne du bas — l'onglet s'ouvre sur
// le contenu ; « off » la porte et s'en referme. C'est ce pont ouvert qui dit la
// sélection dans le client, pas la couleur.
// L'intérieur étant creux, c'est nous qui le peignons : avec `tab_col` /
// `tab_inact` du skin, qui restent donc les leviers du joueur.
//
// Le rendu se GREFFE sur le vrai TabBar d'ImGui (sélection, ordre, scroll,
// clavier, drag&drop : rien n'est réimplémenté) — on rend ses fonds transparents
// et on peint l'art à leur place, d'après le layout de la frame PRÉCÉDENTE
// (`Offset`/`Width` ne sont recalculés qu'au premier BeginTabItem). Aucun retard
// visible pour autant : à sa toute première frame, ImGui ne dessine pas non plus
// ses onglets (TabItemEx sort avant le rendu quand `tab_appearing`).
namespace {

// Largeur du coin de l'art (px). Au-delà, `l`/`r` ne sont plus qu'un bord
// vertical d'1 px, uniforme -> étirable à n'importe quelle hauteur d'onglet
// (17 px dans une fenêtre RO, contre 20 px à l'art) sans le déformer.
constexpr float kTabCorner = 3.0f;
constexpr int kTabBarColors = 7;  // à dépiler dans RoEndTabBar

void EnsureTabTex() {
  EnsureTex("basic_interface\\tabh_l_on.bmp",   skin::kTabhLOn,   g_tab_l_on);
  EnsureTex("basic_interface\\tabh_m_on.bmp",   skin::kTabhMOn,   g_tab_m_on);
  EnsureTex("basic_interface\\tabh_r_on.bmp",   skin::kTabhROn,   g_tab_r_on);
  EnsureTex("basic_interface\\tabh_l_off.bmp",  skin::kTabhLOff,  g_tab_l_off);
  EnsureTex("basic_interface\\tabh_m_off.bmp",  skin::kTabhMOff,  g_tab_m_off);
  EnsureTex("basic_interface\\tabh_r_off.bmp",  skin::kTabhROff,  g_tab_r_off);
}

// Peint UN onglet dans son rect écran : intérieur plein puis contour RO.
void DrawRoTab(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool selected, bool hovered) {
  const SkinTex& l = selected ? g_tab_l_on : g_tab_l_off;
  const SkinTex& m = selected ? g_tab_m_on : g_tab_m_off;
  const SkinTex& r = selected ? g_tab_r_on : g_tab_r_off;

  ImVec4 fill(g_cfg.tab_inact[0], g_cfg.tab_inact[1], g_cfg.tab_inact[2],
              g_cfg.tab_inact[3]);
  if (selected) {
    fill = ImVec4(g_cfg.tab_col[0], g_cfg.tab_col[1], g_cfg.tab_col[2],
                  g_cfg.tab_col[3]);
  } else if (hovered) {  // éclaircissement du survol, comme le thème ImGui
    fill.x = ImMin(fill.x + 0.055f, 1.0f);
    fill.y = ImMin(fill.y + 0.055f, 1.0f);
    fill.z = ImMin(fill.z + 0.055f, 1.0f);
  }
  // L'onglet ACTIF descend jusqu'en bas du rect (il se fond dans le contenu) ;
  // l'inactif s'arrête 1 px plus haut, là où son art referme le bord.
  const float fill_y1 = selected ? p1.y : p1.y - 1.0f;
  if (fill_y1 > p0.y + 1.0f)
    dl->AddRectFilled(ImVec2(p0.x + 1.0f, p0.y + 1.0f),
                      ImVec2(p1.x - 1.0f, fill_y1), ImGui::GetColorU32(fill),
                      2.0f, ImDrawFlags_RoundCornersTop);

  if (!l.tex || !m.tex || !r.tex) return;  // bmp absent du client -> fond seul
  const float lh = (float)l.h;
  const float mw = (float)m.w, mh = (float)m.h;
  const float rw = (float)r.w, rh = (float)r.h;
  // Coins à l'échelle 1:1, bords étirés (uniformes -> aucun artefact).
  BlitPart(dl, l, p0, ImVec2(p0.x + kTabCorner, p0.y + kTabCorner),
           0.0f, 0.0f, kTabCorner, kTabCorner);
  BlitPart(dl, l, ImVec2(p0.x, p0.y + kTabCorner), ImVec2(p0.x + 1.0f, p1.y),
           0.0f, kTabCorner, 1.0f, lh);
  BlitPart(dl, r, ImVec2(p1.x - kTabCorner, p0.y), ImVec2(p1.x, p0.y + kTabCorner),
           rw - kTabCorner, 0.0f, rw, kTabCorner);
  BlitPart(dl, r, ImVec2(p1.x - 1.0f, p0.y + kTabCorner), ImVec2(p1.x, p1.y),
           rw - 1.0f, kTabCorner, rw, rh);
  BlitPart(dl, m, ImVec2(p0.x + kTabCorner, p0.y),
           ImVec2(p1.x - kTabCorner, p0.y + 1.0f), 0.0f, 0.0f, mw, 1.0f);
  if (!selected)  // ligne du bas : l'onglet inactif se ferme côté contenu
    BlitPart(dl, m, ImVec2(p0.x, p1.y - 1.0f), ImVec2(p1.x, p1.y),
             0.0f, mh - 1.0f, mw, mh);
}

// Peint tous les onglets de la barre + le trait qui prolonge la rangée.
void DrawRoTabBarArt(ImGuiTabBar* tb) {
  if (!tb) return;
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImRect bar = tb->BarRect;
  if (bar.Max.y - bar.Min.y < 4.0f || bar.Max.x <= bar.Min.x) return;
  EnsureTabTex();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(bar.Min, bar.Max, true);
  dl->AddCallback(ImCb_PointFilter, nullptr);  // pixel-art net
  // Bornes ramenées à l'entier : un contour d'1 px posé sur une coordonnée
  // fractionnaire ressort décalé d'un pixel en échantillonnage POINT (le piège
  // qui « abîmait » déjà les boutons système d'une barre de titre impaire).
  const float y0 = ImFloor(bar.Min.y), y1 = ImFloor(bar.Max.y);
  float last_x = ImFloor(bar.Min.x);
  for (const ImGuiTabItem& tab : tb->Tabs) {
    // Seulement les onglets soumis à la frame précédente : une entrée survit un
    // moment à l'onglet qui a cessé d'être émis, et la peindre laisserait un
    // onglet fantôme (ImGui, lui, ne la dessine plus).
    if (tab.LastFrameVisible + 1 < g.FrameCount) continue;
    if (tab.Flags & ImGuiTabItemFlags_Invisible) continue;
    // Mêmes coordonnées que TabItemEx, à l'entier près : un bord d'1 px posé sur
    // une abscisse fractionnaire ressort épaissi ou effacé en filtrage POINT.
    const float x0 = ImFloor(bar.Min.x + IM_TRUNC(tab.Offset - tb->ScrollingAnim));
    const float x1 = ImFloor(x0 + tab.Width);
    if (x1 - x0 < kTabCorner * 2.0f) continue;  // trop étroit pour les 2 coins
    // VisibleTabId (et non SelectedTabId) : c'est l'onglet dont le CONTENU est
    // affiché, y compris pendant un aperçu Ctrl+Tab.
    const bool sel = (tb->VisibleTabId == tab.ID);
    // Survol de la frame précédente — celle dont on peint le layout.
    const bool hov = (g.HoveredIdPreviousFrame == tab.ID);
    DrawRoTab(dl, ImVec2(x0, y0), ImVec2(x1, y1), sel, hov);
    if (hov) SetHoverCursor(kRoCursorHand);
    if (x1 > last_x) last_x = x1;
  }
  // La rangée se prolonge jusqu'au bord du panneau, comme dans le client : les
  // onglets inactifs portent déjà cette ligne dans leur art, il ne manque que
  // l'après-dernier-onglet.
  if (last_x < bar.Max.x && g_tab_m_off.tex)
    BlitPart(dl, g_tab_m_off, ImVec2(last_x, y1 - 1.0f),
             ImVec2(bar.Max.x, y1), 0.0f, (float)g_tab_m_off.h - 1.0f,
             (float)g_tab_m_off.w, (float)g_tab_m_off.h);
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  dl->PopClipRect();
}

}  // namespace

bool RoBeginTabBar(const char* str_id, int tab_bar_flags) {
  // Fonds ImGui neutralisés : l'onglet est entièrement peint par DrawRoTabBarArt.
  // (Le séparateur de bas de barre d'ImGui reprend ImGuiCol_TabSelected : il
  // disparaît donc avec eux, et c'est notre trait qui le remplace.)
  const ImU32 kNone = IM_COL32(0, 0, 0, 0);
  ImGui::PushStyleColor(ImGuiCol_Tab, kNone);
  ImGui::PushStyleColor(ImGuiCol_TabHovered, kNone);
  ImGui::PushStyleColor(ImGuiCol_TabSelected, kNone);
  ImGui::PushStyleColor(ImGuiCol_TabDimmed, kNone);
  ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, kNone);
  ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, kNone);
  ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, kNone);
  if (!ImGui::BeginTabBar(str_id, tab_bar_flags)) {
    ImGui::PopStyleColor(kTabBarColors);
    return false;
  }
  DrawRoTabBarArt(ImGui::GetCurrentTabBar());
  return true;
}

void RoEndTabBar() {
  ImGui::EndTabBar();
  ImGui::PopStyleColor(kTabBarColors);
}

bool ShowRoSkinSettings() {
  bool ch = false;
  ch |= WheelSliderFloat(i18n::Tr("Luminosité"), &g_cfg.title_brightness, 0.5f, 1.5f);
  SameLine(); HelpMarker(
    i18n::Tr("N'affecte que les images (barre de titre, boutons, scrollbar, footer,\n"
    "icones) - pas le texte ni les fonds (régles par les couleurs ci-dessous)."));
  ch |= WheelSliderFloat(i18n::Tr("Opacité"), &g_cfg.alpha, 0.3f, 1.0f, "%.2f");
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Corps"), g_cfg.body_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Bordure"), g_cfg.border_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Texte titre"), g_cfg.title_text);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Texte corps"), g_cfg.body_text);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Onglet actif"), g_cfg.tab_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Onglet inactif"), g_cfg.tab_inact);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Champ de saisie"), g_cfg.input_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("En-tête tableau"), g_cfg.header_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Fond cases (feuille perso)"), g_cfg.slot_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Fond doll (feuille perso)"), g_cfg.doll_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Fond carte item"), g_cfg.card_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Bandeau carte"), g_cfg.card_head_col);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Texte bandeau carte"), g_cfg.card_head_text);
  ch |= ColorEdit4WithAlphaBar(i18n::Tr("Fond fenêtre de liste (storage)"), g_cfg.list_col);
  if (ImGui::Button(i18n::Tr("Réinitialiser le skin"))) {
    g_cfg = RoSkinConfig();
    ch = true;
  }
  return ch;
}

void DrawBar(float x0, float y0, float x1, float y1) {
  EnsureTex("basic_interface\\btnbar_left.bmp", skin::kBtnbarLeft, g_bar_l);
  EnsureTex("basic_interface\\btnbar_mid.bmp", skin::kBtnbarMid, g_bar_m);
  EnsureTex("basic_interface\\btnbar_right.bmp", skin::kBtnbarRight, g_bar_r);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float cap = (float)skin::kBtnbarLeft.w;  // 21
  dl->AddCallback(ImCb_PointFilter, nullptr);
  BlitStretch(dl, g_bar_l, ImVec2(x0, y0), ImVec2(x0 + cap, y1));
  BlitStretch(dl, g_bar_r, ImVec2(x1 - cap, y0), ImVec2(x1, y1));
  BlitStretch(dl, g_bar_m, ImVec2(x0 + cap, y0), ImVec2(x1 - cap, y1));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

float DrawIconNum(float x, float y) {
  EnsureTex("inventory\\icon_num.bmp", skin::kIconNum, g_iconnum);
  if (!g_iconnum.tex) return 0.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCallback(ImCb_PointFilter, nullptr);
  BlitStretch(dl, g_iconnum, ImVec2(ImFloor(x), ImFloor(y)),
              ImVec2(ImFloor(x) + g_iconnum.w, ImFloor(y) + g_iconnum.h));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  return (float)g_iconnum.w;
}

// Shared body of both CP949 input widgets: `hint` null selects plain InputText.
static bool InputTextCp949Impl(const char* label, const char* hint, char* cp949_buf,
                               size_t buf_size, int imgui_input_flags) {
  if (!cp949_buf || buf_size == 0) return false;

  // One persistent UTF-8 edit buffer per widget id. Kept across frames so the
  // cursor/selection survive; re-seeded from cp949_buf only while NOT editing so
  // external changes are reflected without stomping in-progress input.
  static std::unordered_map<ImGuiID, std::string> store;
  const ImGuiID id = ImGui::GetID(label);
  std::string& utf8 = store[id];
  if (ImGui::GetActiveID() != id) utf8 = Cp949ToUtf8(cp949_buf);

  InputTextUserData ud{&utf8};
  const int flags = imgui_input_flags | ImGuiInputTextFlags_CallbackResize;
  const bool edited =
      hint ? ImGui::InputTextWithHint(label, hint, utf8.data(), utf8.capacity() + 1,
                                      flags, InputTextResizeCb, &ud)
           : ImGui::InputText(label, utf8.data(), utf8.capacity() + 1, flags,
                              InputTextResizeCb, &ud);

  if (edited) Utf8ToCp949(utf8.c_str(), cp949_buf, buf_size);
  return edited;
}

bool InputTextCp949(const char* label, char* cp949_buf, size_t buf_size,
                    int imgui_input_flags) {
  return InputTextCp949Impl(label, nullptr, cp949_buf, buf_size, imgui_input_flags);
}

bool InputTextCp949WithHint(const char* label, const char* hint, char* cp949_buf,
                            size_t buf_size, int imgui_input_flags) {
  return InputTextCp949Impl(label, hint, cp949_buf, buf_size, imgui_input_flags);
}

}  // namespace ro
