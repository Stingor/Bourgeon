#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "features/plugin.h"

// ── ItemDescWindow (SQUELETTE / placeholder, non-intrusif) ──────────────────
//
// But final : enrichir DRASTIQUEMENT la fenêtre de description item/skill avec
// des données techniques récupérées du serveur moonlight (stats réelles, bonus
// script, sources de drop pour les items ; formule de dégâts, coût SP/niveau,
// cast/cooldown réels pour les skills), rendues PAR-DESSUS l'overlay ImGui (la
// fenêtre native se dessine sous ImGui).
//
// ⚠️ ARCHITECTURE RÉELLE (corrigée live 2026-07-01, x32 attaché — cf. mémoire
// project_item_skill_desc_window_re « CORRECTION LIVE ») : il y a DEUX fenêtres
// natives DISTINCTES, pas une fenêtre unifiée, et elles peuvent COEXISTER :
//   - ITEM  = classe 0xc  (vtable 0x01032aac), slot manager mgr+0x218
//             (0x0131f700). id = atoi(std::string @ wnd+0xe4).
//   - SKILL = classe 0x2e (vtable 0x01032e0c), slot manager mgr+0x230
//             (0x0131f718). id = *(int*)(wnd+0x104) (BRUT).
// Signal ouvert/fermé FIABLE = le SLOT MANAGER (non-nul ⇒ ouvert, remis à 0 à
// la fermeture). ⚠️ wnd+0x28 (« visible ») N'EST PAS fiable (reste à 1 après
// fermeture, mémoire périmée). ⚠️ La fenêtre skill 0x2e est RÉALLOUÉE ⇒ relire
// le pointeur FRAIS chaque tick, jamais le cacher. ⚠️ wnd+0x114 N'EST PAS
// isSkill (vaut 1 pour un item) — le type vient de QUELLE fenêtre, pas d'un flag.
//
// ÉTAT ACTUEL = SQUELETTE NON-INTRUSIF :
//   - AUCUN hook installé : polling read-only des 2 slots, sous garde SEH.
//   - AUCUN paquet envoyé / opcode enregistré (couche serveur derrière
//     kEnableServerFetch=false).
//   - AUCUN rendu natif neutralisé (on SUPERPOSE un panneau par fenêtre).
// => Vraie feature : flip kEnableServerFetch, enregistrer les opcodes, et
//    (option) neutraliser le DrawContent natif de chaque fenêtre.

// API PARTAGÉE : contenu de description SIMPLIFIÉ pour un id d'item (nom + icône/
// illustration + lignes de desc, markup ^RRGGBB rendu). Lit la DB de description du
// client (chargement paresseux, cache par id) — aucun paquet, aucune donnée
// dupliquée, et surtout aucune fenêtre native ouverte : l'appelant dessine ça dans
// le conteneur de son choix (panneau RO, tooltip…). Utilisé par l'aperçu au survol
// du viewer storage. `wrap` = largeur de retour à la ligne (0 = région dispo).
namespace itemdesc {
// Random option d'INSTANCE (propre au stack, pas à l'item de base) telle que le
// client la stocke dans l'ItemSkillInfo (+0x9c, entrées de 5 octets).
struct SimpleOpt { int16_t index = 0; int16_t value = 0; uint8_t param = 0; };

// `cards` = 4 slots max (0 = vide) et `opts` = options d'instance : eux non plus
// ne sont pas dans la DB, l'appelant les lit dans SON ItemSkillInfo et les passe
// ici. Passer nullptr/0 pour n'afficher que la description de base.
// `refine` (0 = aucun) préfixe le titre par « +N », comme le nom natif complet.
// `display_name` = nom DÉCORÉ composé par le name-builder natif BuildDisplayName
// (préfixes/suffixes de cartes, refine, forge) — seul l'appelant a l'ItemSkillInfo
// nécessaire. Fourni, il remplace « +N <nom de base> » et le titre devient celui de
// la fenêtre de description complète ; le suffixe d'emplacements « [N] » est ajouté
// ici (BuildDisplayName ne le compose pas). nullptr = repli sur le nom de la DB.
// `damaged` (ItemSkillInfo+0x5d) = équipement CASSÉ : le titre prend le suffixe
// « - Broken » et l'ombre rouge native 0x5050fa, comme le titre de la fenêtre de
// description complète.
void RenderSimpleDesc(uint32_t id, float wrap, const uint32_t* cards = nullptr,
                      int card_count = 0, const SimpleOpt* opts = nullptr,
                      int opt_count = 0, int refine = 0,
                      const char* display_name = nullptr, bool damaged = false);

// ⚠️ Une fenêtre qui ouvre une description n'a RIEN à faire pour le z-order.
//
// Il exista ici un `FocusDescWindow()` que chaque appelant devait penser à appeler
// après itemcell::OpenDesc* — et l'oubli était la règle : une seule des huit
// fenêtres qui ouvrent une desc le faisait. La remontée est désormais réclamée par
// ItemDescWindow lui-même (ItemDescWindow::RaiseItemWindow, posée depuis le hook
// OnMsg 0x18 de la fenêtre native, que TOUTES les ouvertures traversent). N'ajoutez
// pas de second appel de focus à côté : la desc porte
// `ImGuiWindowFlags_NoFocusOnAppearing` et c'est ce drapeau différé, appliqué à la
// frame suivante donc APRÈS tout le monde, qui la remonte.
}  // namespace itemdesc

class ItemDescWindow : public Plugin {
 public:

  // Contenu de sa section dans le panneau Moonlight. Rend true si un réglage
  // a changé — l'appelant décide de sauvegarder, une seule fois.
  bool DrawSettings();
  ItemDescWindow();

  const char* name() const override { return "ItemDescWindow"; }

  void OnTick() override;       // capture item ET skill courants (polling read-only)
  void OnRenderUI() override;   // dessine un panneau placeholder par fenêtre ouverte
  void OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // Toggles runtime indépendants (persistés par MoonlightUi) : panneau item
  // et/ou panneau skill peuvent être désactivés séparément.
  bool& show_item_panel()  { return show_item_panel_; }
  bool& show_skill_panel() { return show_skill_panel_; }
  // Fenêtre de LIVRE reproduite en ImGui (sinon : fenêtre native, sous l'overlay).
  bool& show_book_panel()  { return show_book_panel_; }
  // Ouvrir les livres à la page 1 plutôt qu'au signet de page natif.
  bool& book_reset_page()  { return book_reset_page_; }
  // État de la checkbox « Comparer » (persisté par MoonlightUi).
  bool& cmp_show_equipped() { return cmp_show_equipped_; }
  // Ouverture de la desc : true = près de la souris, false = dernière position
  // connue (persisté par MoonlightUi).
  bool& desc_spawn_at_cursor() { return desc_spawn_at_cursor_; }
  // Ancrage (0=HG 1=HD 2=BG 3=BD 4=centre) + offset X/Y de l'ouverture « près de la
  // souris » (persistés par MoonlightUi).
  int& desc_anchor()   { return desc_anchor_; }
  int& desc_offset_x() { return desc_offset_x_; }
  int& desc_offset_y() { return desc_offset_y_; }

  // Cache IMMÉDIATEMENT le rendu natif des fenêtres desc (item 0xc + comparaison
  // 0xea) depuis leurs slots manager (appelé depuis le hook OnMsg de MoonlightUi
  // au moment de l'ouverture -> zéro flicker). No-op si le panneau item enrichi
  // est désactivé. SEH-gardé.
  void HideNativeDescWindows();
  // Cache la fenêtre skill (0x2e) — appelée depuis le hook OnMsg 0x3d (zéro
  // flicker). No-op si le panneau skill est désactivé. SEH-gardé.
  void HideNativeSkillWindow();

  // Demande que la fenêtre de description d'ITEM repasse au premier plan à la
  // prochaine frame rendue (elle et ses panneaux satellites).
  //
  // Appelée par le hook OnMsg de la fenêtre native sur le message « set item »
  // (0x18) : c'est le seul point qui voit TOUTES les ouvertures — clic droit natif
  // comme itemcell::OpenDesc* de nos propres fenêtres — et le seul qui voie le
  // chargement d'un autre objet dans une desc DÉJÀ ouverte, cas sans front montant
  // où elle restait sous la fenêtre d'où venait le clic.
  //
  // Simple drapeau : aucun appel ImGui ici, le hook tourne hors frame de rendu.
  void RaiseItemWindow() { item_need_raise_ = true; }

  // Instantané read-only d'une fenêtre native de description (public : lu par
  // un helper libre dans le .cc).
  struct DescWindow {
    bool      open = false;
    uint32_t  id = 0;
    bool      is_skill = false;
    int       x = 0, y = 0, w = 0, h = 0;  // pos/taille écran (base UIWindow)
  };

  // Instantané read-only de la fenêtre LIVRE native (UIBookWnd, id 0x6a), celle
  // qu'ouvrent les boutons « Lire » / « Lecture auto » de la description.
  // `open` = ouverte ET en mode lecture normale : en mode LECTURE AUTO le client
  // cache lui-même la fenêtre et récite le texte dans le chat, il n'y a alors
  // rien à reproduire.
  struct BookWindow {
    bool open = false;
    bool auto_read = false;
    int  page = 1;
    int  pages = 1;
    char title[64] = {};  // nom du livre (wnd+0xbc), = le titre natif
  };

 private:
  // 🔴 Le décodage, sur le FIL PRINCIPAL. OnRecvPacket (fil réseau) ne fait que
  // copier : `script_cache_` est une table de hachage de chaînes et de vecteurs,
  // remplie pendant que le rendu affichait l'entrée qu'on était en train de
  // réécrire. Cf. features/net_inbox.h.
  void HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) override;

  // État d'une entrée de cache de données techniques (par (is_skill,id)).
  enum class FetchState : uint8_t { kNone, kPending, kReady, kFailed };

  // Une source de drop (item) : mob + taux réel calculé serveur + type de boss.
  struct DropSrc {
    uint32_t    mob_id = 0;
    uint32_t    rate = 0;       // en 0.01% (pour 100.00% = 10000), tel que @whodrops
    uint8_t     boss = 0;       // 0=normal, 1=mini-boss, 2=MVP (get_bosstype)
    uint8_t     src = 0;        // 0=drop normal, 1=MVP reward
    std::string name;
  };
  // Un combo auquel l'item participe : membres (id + nom résolu serveur) + script
  // source brut du combo.
  struct ComboInfo {
    std::vector<std::pair<uint32_t, std::string>> members;  // (item id, nom)
    std::string script;                                      // texte source du combo
  };
  // Script source d'un item (onglets « Script » + « Combos »), renvoyé par le
  // serveur (0x0F12) et mis en cache par id. Le texte brut est conservé au
  // chargement du YAML serveur (le bytecode compilé n'est pas décompilable).
  struct ScriptData {
    FetchState state = FetchState::kNone;
    uint32_t   requested_tick = 0;
    uint8_t    status = 0;   // 0 = ok, 1 = item introuvable côté serveur
    std::string main;        // script principal (Script)
    std::string equip;       // EquipScript
    std::string unequip;     // UnEquipScript
    std::vector<ComboInfo> combos;
  };

  // Une ligne du tableau de cast d'un skill (par niveau).
  struct SkillLv {
    int32_t     cast_var = 0;   // variable cast (ms)
    int32_t     cast_fixed = 0; // fixed cast (ms)
    int32_t     cooldown = 0;   // cooldown (ms)
    int32_t     delay = 0;      // after-cast delay (ms)
  };

  struct TechData {
    FetchState           state = FetchState::kNone;
    uint32_t             requested_tick = 0;  // GetTickCount au moment de la requête
    bool                 is_skill = false;
    // --- item ---
    std::vector<DropSrc> drops;
    bool                 truncated = false;   // plus de sources que renvoyées
    uint8_t              treasure_excluded = 0;  // nb de coffres au trésor filtrés
    // --- skill ---
    uint8_t              max_lv = 0;
    std::vector<SkillLv> levels;
  };

  // Estimation de dégâts d'un sort (contre une cible côté serveur).
  struct DamageEst {
    FetchState state = FetchState::kNone;
    uint32_t   requested_tick = 0;
    uint8_t    status = 0;    // 0=ok, 1=sort non offensif, 2=cible introuvable
    uint8_t    atk_type = 0;  // BF_WEAPON=1 / BF_MAGIC=2 / BF_MISC=4
    uint16_t   hits = 0;      // div_
    uint16_t   skill_lv = 0;
    uint32_t   target = 0;    // 0=neutre / 0xFFFFFFFF=soi / sinon id du monstre
    std::string target_name;  // nom du monstre (vrai mob uniquement)
    int64_t    dmg_min = 0, dmg_max = 0, dmg_avg = 0;
  };

  // Scope d'une requête item : quels mobs on veut (le scan MVP côté serveur ne
  // tourne que si demandé). Ignoré pour les skills.
  static constexpr uint8_t kScopeNormal = 0;  // mobs à drop normal
  static constexpr uint8_t kScopeMvp    = 1;  // MVP rewards (mvpitem)

  // Clé de cache combinant type + scope + id (les espaces d'id item/skill se
  // chevauchent ; drops normaux et MVP sont cachés séparément).
  static uint32_t CacheKey(uint32_t id, bool is_skill, uint8_t scope) {
    return (is_skill ? 0x80000000u : 0u) | (scope ? 0x40000000u : 0u) |
           (id & 0x3fffffffu);
  }

  // Envoie une requête serveur (0x0F0B) pour cet id/scope si pas déjà en cache
  // frais. Déclenché par les boutons du panneau (aucune requête automatique).
  void RequestTechData(uint32_t id, bool is_skill, uint8_t scope);
  // Envoie une requête d'estimation de dégâts (0x0F0D) pour ce sort (niveau 0 =
  // niveau appris) contre target_mob_id (0 = cible neutre). Déclenché par le
  // bouton du panneau.
  void RequestDamage(uint32_t skill_id, uint32_t target_mob_id);
  // Envoie une requête de script/combos (0x0F11) pour cet item si pas déjà en
  // cache frais. Déclenché par l'ouverture d'un onglet Script/Combos.
  void RequestItemScript(uint32_t id);
  // Reproduit la fenêtre de description d'ITEM en ImGui (Option A : native
  // cachée, redessinée à EndScene).
  void RenderItemWindow();
  // Reproduit la fenêtre de description de SKILL (classe 0x2e) en ImGui.
  void RenderSkillWindow();
  // Reproduit la fenêtre LIVRE (UIBookWnd 0x6a) en ImGui : page courante, texte
  // coloré sélectionnable, pagination rejouée sur la fenêtre native.
  void RenderBookWindow();
  // Onglets d'infos techniques (émis dans le TabBar de la fenêtre, après
  // l'onglet Description). Aucune requête tant qu'un onglet data n'est pas actif.
  void RenderTechTabs(const DescWindow& w);
  // Rend une table de sources de drop (filtre + tri + liens). show_type ajoute
  // une colonne mécanisme (drop normal / MVP reward), utile pour le bucket MVP.
  void RenderDropTable(const TechData& td, const char* table_id,
                       uint32_t filter_key, bool show_type);

  bool       show_item_panel_  = true;  // panneau technique pour les items
  bool       show_skill_panel_ = true;  // panneau technique pour les skills
  bool       show_book_panel_  = true;  // fenêtre de livre reproduite en ImGui
  bool       book_reset_page_  = true;  // livre ouvert page 1 (sinon : signet natif)
  bool       cmp_show_equipped_ = true; // toggle : afficher la colonne « Équipé »
  bool       desc_spawn_at_cursor_ = true;  // ouverture : true = près de la souris,
                                            // false = dernière position connue
  int        desc_anchor_ = 3;    // ancrage souris : 0=haut-G 1=haut-D 2=bas-G
                                  // 3=bas-D (defaut, descend a droite) 4=centre
  int        desc_offset_x_ = 12; // offset X/Y depuis la souris (mode près souris)
  int        desc_offset_y_ = 12;
  DescWindow item_;             // fenêtre item candidate (classe 0xc)
  DescWindow compare_;          // fenêtre équipé/comparaison (classe 0xea)
  // Placement de la fenêtre item reproduite : au 1er frame d'ouverture on la
  // pose près du curseur, puis on laisse ImGui la mémoriser (déplaçable).
  bool       item_was_open_ = false;
  bool       item_need_pos_ = false;
  int        item_spawn_x_ = 0, item_spawn_y_ = 0;
  // Remontée au 1er plan demandée (consommée par un SetNextWindowFocus au rendu).
  // DISTINCTE de `*_need_pos_` : elle vaut aussi pour un changement d'objet dans une
  // desc déjà ouverte, et ne doit pas être perdue quand GetCursorPos échoue.
  bool       item_need_raise_ = false;
  uint32_t   item_last_id_ = 0;   // id affiché au tick précédent (0 = fermée)
  DescWindow skill_;            // fenêtre skill (classe 0x2e)
  bool       skill_was_open_ = false;
  bool       skill_need_pos_ = false;
  int        skill_spawn_x_ = 0, skill_spawn_y_ = 0;
  bool       skill_need_raise_ = false;
  uint32_t   skill_last_id_ = 0;
  BookWindow book_;             // fenêtre livre (classe 0x6a)
  bool       book_need_pos_ = false;
  int        book_spawn_x_ = 0, book_spawn_y_ = 0;
  bool       book_need_raise_ = false;

  std::unordered_map<uint32_t, TechData> cache_;  // clé = CacheKey(id,is_skill)
  std::unordered_map<uint32_t, DamageEst> dmg_cache_;  // clé = skill_id
  std::unordered_map<uint32_t, ScriptData> script_cache_;  // clé = item id
  int        dmg_target_input_ = 0;      // champ "ID monstre" du panneau dégâts
  bool       dmg_target_self_  = false;  // cible = soi-même (miroir PvP)
  // Carte/enchant à ouvrir en desc complète au prochain tick (clic droit dans le
  // panneau « Cartes / Enchants »). 0 = rien en attente. On diffère l'appel natif
  // (MakeWindow 0xc + OnMsg 0x18) hors du rendu ImGui -> timing sûr.
  uint32_t   pending_card_open_ = 0;
};
