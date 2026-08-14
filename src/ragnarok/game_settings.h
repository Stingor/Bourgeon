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
// globales `EFFECT` / `CONTROL` / `ETC` poussées par le chargeur).
// ⚠ Il n'y a PAS de valeur pour l'onglet Basic : ses six groupes sont câblés en
// dur dans la fenêtre native et ne passent pas par cette table.
enum Tab {
  kTabEffect  = 1,
  kTabControl = 2,
  kTabEtc     = 3,
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
void SetOn(int id, bool on);

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

}  // namespace gamesettings
