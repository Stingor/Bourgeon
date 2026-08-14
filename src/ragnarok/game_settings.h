#pragma once

#include <cstdint>

// ── game_settings : les OPTIONS du client, en accès typé ─────────────────────
// (client 20250716, base 0x400000 — RE : docs/game_option_re.md §3)
//
// Le client tient ses options dans un singleton `CGameSettingsMgr`
// (`0x0131EE7C`) : un vecteur de descriptions lues depuis un fichier Lua, et une
// table de drapeaux indexée par `TALKTYPE`. Ce module l'enveloppe — SEH,
// `std::string` du client, conversion d'encodage — et rien de plus.
//
// 🔴 CE MODULE NE DÉCIDE DE RIEN. Il n'a ni liste d'options en dur, ni table de
// valeurs par défaut, ni libellés. Tout vient du client, à chaque appel. C'est la
// condition pour qu'un panneau ImGui reste juste quand le serveur change son
// `GameSettings.lub` — fichier qui vit dans le GRF et que Moonlight édite.
//
// ── POURQUOI C'EST DISPONIBLE SANS LA FENÊTRE NATIVE ────────────────────────
// Le vecteur est rempli par `CGameSettingsMgr::LoadTableFromLua` (0x0068E510),
// dont l'unique appelant est `CSession_ctor` (0x00D578A2) : le chargement a lieu
// à l'ENTRÉE EN JEU, pas à l'ouverture de la fenêtre `0x271E`. Un panneau de
// remplacement a donc ses données même si la native n'est jamais créée — ce qui
// n'allait pas de soi et a été vérifié au désassemblage avant d'écrire une ligne.
//
// ── LES DEUX PIÈGES DE CE MAGASIN ───────────────────────────────────────────
// 1. **Cinq options sont stockées à l'ENVERS.** Pour `TT_FULL_AURA_ON_OFF`,
//    `TT_HIDE_AURA_ON_OFF`, `TT_BOLD_NAME_TYPE_ON_OFF`, `TT_BLOCK_CALL_ON_OFF` et
//    `TT_HIDE_FOOTPRINT_ON_OFF`, le drapeau interne est la NÉGATION de ce que voit
//    le joueur — ce sont celles dont le nom interne dit *hide* là où le libellé dit
//    *show*. `GameSettings_IsInvertedOption` (0x0068EAF0) tient la liste ; on
//    l'interroge au lieu de la recopier, et `IsOn`/`SetOn` rendent et prennent la
//    valeur AFFICHÉE. Recopier `Default` tel quel inverserait ces cinq réglages.
// 2. **Écrire le drapeau ne suffit pas.** La plupart des options ont un HANDLER
//    enregistré dans le manager, et c'est lui qui applique l'effet. On passe donc
//    par `CGameSettingsMgr::SetOption` (0x0068DFD0), jamais par le setter brut.
//
// ⚠ Le manager n'existe qu'en JEU. `Available()` est à tester avant tout le reste.

namespace gamesettings {

// Les trois onglets pilotés par données, tels que le Lua les numérote (constantes
// globales `EFFECT` / `CONTROL` / `ETC` poussées dans l'état Lua par le chargeur,
// `0x0068E520` — ce sont des DOUBLES, lus à `0x00FD4420`, `0x0100A0A8`,
// `0x01006BE8`).
//
// 🔴 **`ETC` VAUT 4, PAS 3.** La numérotation n'est pas contiguë : 3 est
// `GRAPHIC`, un onglet qui n'a aucune ligne dans la table (sa page est câblée en
// dur). Supposer la suite 1/2/3 rendait l'onglet « Divers » VIDE — ses 40 lignes
// s'affichaient pourtant dans la vue « Tout », qui ne filtre rien. Constaté en
// jeu ; les valeurs ci-dessous sont désormais LUES dans l'exe.
//
// ⚠ Il n'y a PAS de valeur pour l'onglet Basic : ses six groupes sont câblés en
// dur dans la fenêtre native et ne passent pas par cette table.
enum Tab {
  kTabEffect  = 1,
  kTabControl = 2,
  kTabEtc     = 4,
};

// `ONOFF` / `EXE` côté Lua.
enum Type {
  kTypeToggle  = 0,  // bascule booléenne
  kTypeCommand = 1,  // commande à exécuter (le natif dessine un bouton)
};

// Une ligne de la table d'options, telle que le client l'a chargée.
//
// Les trois textes sont convertis en UTF-8 et RECOPIÉS : le `std::string` source
// appartient au client, et le vecteur peut être réalloué. Les tailles couvrent le
// pire cas observé dans `GameSettings.lub` avec de la marge.
struct Option {
  int  id = 0;         // TALKTYPE — la clé de tout le reste
  int  tab = 0;        // kTabEffect / kTabControl / kTabEtc
  int  type = 0;       // kTypeToggle / kTypeCommand
  int  tipbox_id = 0;  // fenêtre d'aide 0x13B du client ; 0 = aucune
  bool default_on = false;  // défaut AFFICHÉ (l'inversion est déjà défaite)
  char title[128] = {0};
  char tooltip[128] = {0};      // la commande slash équivalente, quand il y en a une
  char description[512] = {0};
};

// Le manager existe et sa table est chargée. Faux au login, en char-select, et
// pendant les transitions.
bool Available();

// Nombre de lignes de la table. 0 si indisponible.
int Count();

// Remplit `out` pour la n-ième ligne. Faux si l'index sort de la table.
bool At(int index, Option* out);

// Valeur AFFICHÉE de la bascule (l'inversion des cinq ids est défaite ici).
bool IsOn(int id);

// Écrit la valeur AFFICHÉE. Passe par le manager, donc par le handler natif de
// l'option : c'est ce qui APPLIQUE l'effet, pas seulement ce qui range un booléen.
//
// ⚠ N'annonce RIEN au chat, exactement comme la case à cocher de la fenêtre
// native. La commande slash équivalente, elle, annonce — ce sont deux chemins
// distincts du même client, et un panneau qui remplace la fenêtre suit la fenêtre.
//
// 🔴 AIGUILLAGE AUTOMATIQUE, et il est indispensable. `SetOption` ne sait écrire
// que les options présentes dans la table `OptionTbl` — pour les autres, elle sort
// **sans rien faire et sans se plaindre**. Les bascules câblées en dur de la page
// Basique (bordure d'emblème, notification de connexion) sont dans ce cas. `SetOn`
// vérifie donc la présence réelle de l'id dans la table et retombe sur l'écriture
// directe quand il n'y est pas. Cf. le commentaire du .cc — bug vécu en jeu.
void SetOn(int id, bool on);

// L'id est-il décrit dans `OptionTbl` ? Exposé parce que la réponse change le
// comportement de `SetOn`, et qu'un appelant peut vouloir le savoir avant.
bool InTable(int id);

// ── Écrire une bascule que le client ne connaît PAS ENCORE ───────────────────
//
// 🔴 IL Y A UN TROISIÈME CAS, et il est vicieux. `GameSettings_SetFlagRaw` ne
// MET À JOUR que des clés existantes : si l'option n'est pas déjà dans la table
// des drapeaux, elle **ne fait rien et rend 0**. Or cette table n'est peuplée que
// par deux chemins — les options de `OptionTbl`, et les commandes slash listées
// dans `CmdOnOffList` de `SaveData\OptionInfo.lua`. Une option qui n'est dans
// aucun des deux est donc **impossible à écrire**, y compris pour le client
// lui-même.
//
// C'est le cas, vérifié en jeu le 2026-08-14, de la **bordure d'emblème**
// (`TT_EMBLEM_FRAME_ON_OFF`) : `/frame` est absent de `CmdOnOffList`, et le
// réglage est donc inerte DANS LA FENÊTRE NATIVE AUSSI — ce n'est pas une
// régression du portage, c'est un réglage mort chez le client.
//
// `sub_68FC70` est la seule fonction qui INSÈRE : elle résout le nom de commande
// (« /frame ») en identifiant, puis écrit sans exiger que la clé préexiste. On
// passe donc par elle pour ces options-là, ce qui les fait marcher là où le natif
// échoue.
//
// `slash_command` est le nom EXACT de la commande de chat, barre oblique comprise.
// Renvoie false si le client ne connaît pas cette commande.
bool SetOnByCommand(const char* slash_command, bool on);

// Exécute une option `kTypeCommand` (les `/sit`, `/where`, `/memo`… de l'onglet
// Divers). Sans effet sur une bascule.
void Exec(int id);

// Le [Reset] de la fenêtre native : remet TOUTES les options à leur défaut Lua.
// ⚠ Ne touche ni au son, ni à la page Graphics — comme le natif.
void ResetAllToDefault();

// ── Son ─────────────────────────────────────────────────────────────────────
// Le groupe « Audio Setting » de l'onglet Basic. Les volumes vivent dans le
// gestionnaire de son (`g_SoundMgr`), pas dans la table d'options ; seules les
// deux cases « Off » sont des TALKTYPE (7 et 0xB).

constexpr int kVolumeMax = 127;  // 0x7F, le clamp du client

int  BgmVolume();
void SetBgmVolume(int volume);

// ⚠ « Volume des effets » = le volume MAÎTRE 2D (`g_SoundMgr+0xE8`), pas le champ
// nommé « effect volume » (`+0xE0`) — c'est le curseur du natif qui en décide
// ainsi, et s'en écarter donnerait deux réglages qui ne se répondent pas.
int  EffectVolume();
void SetEffectVolume(int volume);

bool BgmEnabled();
// 🔴 Ne se résume PAS à écrire le drapeau : quand la valeur CHANGE, le natif
// envoie `CMode::SendMsg(90)`, qui (re)démarre ou coupe la musique. Sans lui, le
// morceau en cours continue de jouer par-dessus un réglage éteint.
void SetBgmEnabled(bool on);

bool EffectSoundEnabled();
void SetEffectSoundEnabled(bool on);

// ── Les trois derniers groupes de la page Basique ────────────────────────────
// (RE : docs/game_option_re.md §3.9 — `CUIGroupSkin`, `CUIGroupRodexSpam`,
// `CUIGroupProcessPriority`)
//
// Aucun des trois ne passe par la table d'options : chacun a son propre magasin,
// et c'est pour cela qu'ils étaient restés au natif. Ils n'ont en commun que leur
// place dans la fenêtre — leurs trois mécaniques n'ont rien à voir entre elles,
// et c'est justement ce qu'il faut savoir avant de les toucher.

// ── Priorité du processus ───────────────────────────────────────────────────
// Purement locale, purement Windows : le natif appelle `SetPriorityClass` sur son
// propre processus et retient la valeur dans un global. Rien n'est envoyé au
// serveur, rien n'est écrit sur le disque — le réglage ne survit PAS à la
// fermeture du client, exactement comme dans la fenêtre native.

enum Priority {
  kPriorityHigh   = 0x80,  // HIGH_PRIORITY_CLASS
  kPriorityNormal = 0x20,  // NORMAL_PRIORITY_CLASS — le défaut du client
  kPriorityLow    = 0x40,  // IDLE_PRIORITY_CLASS ; le client l'appelle « Low »
};

// La classe de priorité que le CLIENT croit avoir posée. On lit son global plutôt
// que `GetPriorityClass` : c'est lui que la fenêtre native consulte pour cocher
// ses boutons, et deux sources qui divergeraient donneraient deux écrans en
// désaccord.
int  ProcessPriority();

// Applique et mémorise, dans cet ordre — le global est ce que le natif relira.
void SetProcessPriority(int priority_class);

// ── RODEX : accepter le courrier de n'importe qui ───────────────────────────
//
// 🔴 CE RÉGLAGE APPARTIENT AU SERVEUR. Le client n'écrit JAMAIS son propre
// drapeau : il envoie `CZ 0x0B93` et attend que le serveur le lui renvoie
// (`ZC 0x0B94` pour un changement, `ZC 0x0B95` pour la liste complète à
// l'entrée en jeu). Un affichage optimiste mentirait donc — si le serveur ignore
// le paquet, la case doit RESTER où elle est. C'est ce que fait le natif.

// Vrai = « recevoir le courrier de tout le monde » (le filtre anti-spam est
// DÉSACTIVÉ). C'est le sens du drapeau du client, pas celui de son libellé
// `..._RODEX_SPAM_ON`, qui nomme l'autre bouton radio.
bool RodexAcceptsEveryone();

// Demande le changement au serveur. Ne modifie rien localement, et n'a aucun
// effet visible tant que le serveur n'a pas répondu.
void RequestRodexAcceptsEveryone(bool accept);

// ── Skin de l'interface ─────────────────────────────────────────────────────
// Le gestionnaire (`0x011FE3A8`) tient la liste des skins installés et l'index
// courant. Le natif remplit sa liste déroulante avec, sans rien filtrer.

constexpr int kSkinDefault = -1;  // « <Basic Skin> » : aucun skin appliqué

// Nombre de skins INSTALLÉS, sans compter l'entrée « par défaut ».
int SkinCount();

// Nom du skin `index` (`kSkinDefault` pour le nom de l'entrée par défaut),
// converti en UTF-8. Faux si l'index sort de la liste.
bool SkinName(int index, char* out, int out_size);

// Index courant. `kSkinDefault` quand aucun skin n'est appliqué.
int CurrentSkin();

// 🔴 PURGE TOUTES LES TEXTURES .bmp DU CLIENT. Le natif vide le gestionnaire de
// textures pour que les images se rechargent depuis le dossier du nouveau skin :
// tout handle de texture gardé au-delà de cet appel est mort. L'appelant DOIT
// donc invalider ses propres caches (`ro::InvalidateGameTextures`), et ne jamais
// appeler ceci au milieu d'une frame ImGui.
void SetSkin(int index);

}  // namespace gamesettings
