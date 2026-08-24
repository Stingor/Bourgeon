#pragma once

// ── Briques partagées de la « cellule d'item » ───────────────────────────────
// Icône + nom composé + aperçu au survol + clic droit vers la description : ce
// motif est réécrit dans chaque viewer (inventaire, chariot, entrepôt, échoppe,
// boutique NPC, cash shop, fiche de perso). `VendingWindow::DrawItemCell` en est
// la version la plus aboutie et sert de modèle ; ce fichier généralise ses
// briques, une par une, à mesure qu'on les vérifie identiques.
//
// Pourquoi ici et pas dans `ui/` : le tooltip s'appuie sur
// `itemdesc::RenderSimpleDesc`, qui vit dans features/windows/item_desc_window.h.
// Le mettre dans `ui/` créerait l'inversion de couches que docs/source_layout.md
// interdit (ui/ ne connaît pas features/). Il vit donc à la racine de features/,
// comme plugin.h, staff_gate.h et hotkey_util.h.
//
// L'ouverture de la description est ici aussi, en DEUX entrées (cf. plus bas) :
// ce que l'appelant possède — un ItemSkillInfo vivant, ou un simple id — change
// ce que la fenêtre peut rendre, et l'API le dit au lieu de le cacher.

#include <cstddef>
#include <cstdint>

#include "features/windows/item_desc_window.h"  // itemdesc::SimpleOpt
#include "imgui.h"
#include "ui/icon_cache.h"  // ro::IconTex

namespace itemcell {

// ── L'objet s'empile-t-il ? ──────────────────────────────────────────────────
// Réplique exacte de `item_data::isStackable()` du serveur (itemdb.cpp) : les
// cinq types exclus sont des INSTANCES uniques — deux marteaux identiques
// restent deux objets, ils ne font jamais « x2 ».
//   4 = ARMOR · 5 = WEAPON · 7 = PETEGG · 8 = PETARMOR · 12 = SHADOWGEAR
//
// `type` est l'octet que le SERVEUR met dans ses paquets (échange, liste
// d'achat…), après passage par `itemtype()` de clif.cpp — qui replie PETEGG sur
// ARMOR et SHADOWGEAR sur WEAPON/ARMOR. 7 et 12 n'arrivent donc jamais par ce
// canal ; ils restent listés parce que la règle, elle, les compte.
//
// ⚠ Le client natif est PLUS LARGE : la fenêtre d'achat (UIItemPurchaseWnd_OnMsg
// 0x00951BD0, case 38) refuse un second exemplaire au panier pour les types
// {4,5,8,9,11,12,13,14,15} — donc aussi 11 (DELAYCONSUME), que le serveur
// empile sans broncher. On suit le SERVEUR : c'est lui qui tranche, et brider un
// consommable empilable serait un faux positif visible.
inline bool TypeIsStackable(uint8_t type) {
  return type != 4 && type != 5 && type != 7 && type != 8 && type != 12;
}

// ── Équipement CASSÉ ─────────────────────────────────────────────────────────
// Le flag d'instance vit à ItemSkillInfo+0x5d (octet, entre « identifié » +0x5c
// et le refine +0x60). Le rendu natif de référence (DrawName 0x008972c0) est une
// OMBRE rouge 0x5050fa (COLORREF BGR → RGB 250,80,80) décalée +1,+1 SOUS le nom,
// le texte lui-même inchangé — c'est l'ombre qui fait « paraître » le nom rouge.
// item_desc_window reproduit déjà ce rendu ; ces briques le portent dans les
// viewers (nom ombré + icône teintée).
inline constexpr unsigned int kDamagedShadow = IM_COL32(0xFA, 0x50, 0x50, 0xFF);

// Le nom d'un item en widget texte, avec l'ombre « cassé » du natif si
// `damaged`. Remplace un ImGui::TextUnformatted — la couleur du texte reste
// celle du style courant (PushStyleColor(ImGuiCol_Text, …) autour pour un nom
// grisé/noir).
void NameText(const char* utf8, bool damaged);

// Une TUILE de grille : l'icône à sa taille NATIVE (le natif dessine le bmp 1:1,
// ~24 px dans une tuile de 32), centrée, et réduite SEULEMENT si elle déborde ;
// un « ? » grisé si l'icône manque ; puis, en bas à droite, le refine « +N »
// OU la quantité — jamais les deux, un équipement ne s'empile pas. Ce badge est
// écrit en noir cerné de blanc pour rester lisible sur n'importe quel fond,
// comme le fait le client.
//
// `damaged` teinte l'icône en rouge (décision d'affichage des viewers ImGui,
// plus lisible en grille que la seule ombre du nom — le natif, lui, ne marque
// que le nom).
//
// ⚠ C'est la présentation GRILLE. Les viewers en LISTE (vending_window,
// inventory_viewer en mode liste) composent leur ligne autrement — icône +
// libellé en widgets ImGui ou en texte à coordonnées. Les deux ne se ramènent
// pas l'une à l'autre, et ce n'est pas un défaut : ce sont deux mises en page.
//
// `p0`/`p1` sont les coins de la tuile, `cell` son côté.
void DrawTile(ImDrawList* draw_list, const ImVec2& p0, const ImVec2& p1,
              float cell, const ro::IconTex& icon, int refine, int amount,
              bool damaged = false);

// Nom d'AFFICHAGE composé par le name-builder natif : refine, préfixes de
// cartes, forge. `info` = l'ItemSkillInfo de l'objet. Écrit toujours une chaîne
// terminée dans `out` — vide si tout échoue, jamais d'indéterminé.
//
// 🔴 PLUS DE PARAMÈTRE `wnd`, et ce n'est pas un nettoyage cosmétique. Le `this`
// du builder natif ne sert QU'À UN endroit : une requête de nom de créateur
// (branche « objet forgé dont le créateur est inconnu ») qui va lire une
// std::list à `this+0x18C`. Il lui faut donc le GESTIONNAIRE de fenêtres, pas
// une fenêtre :
//   - `uiwnd::Mgr()` est un objet statique — il marche toujours ;
//   - une FENÊTRE ne marchait que tant qu'elle était ouverte (sinon le nom perd
//     ses préfixes de cartes), et depuis que les natives sont détruites la
//     plupart des appelants passaient un pointeur NUL ;
//   - une PETITE fenêtre lit carrément hors de ses bornes (UIWeaponRefineWnd
//     fait 0xD0 octets — le bug d'origine, cf. docs/weapon_refine_re.md §9).
// Le contexte est donc résolu ICI, une fois pour toutes : aucun appelant n'a plus
// à connaître cette subtilité, ni à pouvoir se tromper.
//
// ⚠ Le builder n'ajoute PAS le suffixe d'emplacements « [N] » : l'appelant le
// compose lui-même (comme le fait itemdesc::RenderSimpleDesc, pour la même
// raison). ⚠ Repli intégré : si la composition rend une chaîne vide, on retombe
// sur le nom de base. L'ensemble est sous SEH — un ItemSkillInfo à moitié
// initialisé ne doit pas tuer le client.
void BuildDisplayName(void* info, char* out, size_t out_size);

// Nombre TOTAL d'emplacements de carte de l'item décrit par `info`, 0 si aucun
// ou si la lecture échoue. Appel natif sous SEH.
int SlotCount(void* info);

// Le LIEN de chat de l'objet — `<ITEML>…</ITEML>`, le texte que les autres
// clients savent rendre en icône + nom cliquable. Écrit dans `out` ; renvoie
// false (et `out` vide) si l'item est illisible ou sans nameid, jamais une balise
// tronquée. 128 octets suffisent largement (4 cartes + 5 options ≈ 90).
//
// C'est le geste Maj+clic gauche du client, réécrit : le natif le forge dans
// `UIWnd_AppendItemLinkButton 0x00865230`, indissociable de la création d'un
// bouton accroché à sa chatbox — qui n'existe plus. Le format et la table de
// séparateurs sont vérifiés des DEUX côtés (client 20250716 et
// `ItemDatabase::create_item_link` de rAthena) ; détail dans item_cell.cc.
bool BuildChatLink(void* info, char* out, size_t out_size);

// ── Le lien de chat RELU — tout ce que la balise transporte ──────────────────
// Le pendant de `BuildChatLink`. C'est la SEULE source d'information sur l'objet
// d'un AUTRE joueur : il n'est ni dans notre sac ni dans aucune liste de session,
// la balise est tout ce qu'on aura jamais de lui. En jeter le contenu pour ne
// garder que le nameid, c'est afficher « Axe » là où le natif écrit « Test's
// Axe » et ouvrir une description sans cartes ni refine.
//
// POD pur : se copie et se conserve (la chatbox en garde un par lien affiché).
struct ChatLink {
  uint32_t id     = 0;  // nameid
  uint32_t equip  = 0;  // emplacement d'équipement (masque EQP_*)
  uint32_t refine = 0;
  uint32_t view   = 0;  // viewID
  uint32_t grade  = 0;
  // ⚠ Sur un objet FORGÉ ce ne sont pas des cartes : `cards[2] | cards[3] << 16`
  // est l'id de personnage du forgeron, celui que le client résout en « Test's ».
  uint32_t cards[4]     = {0, 0, 0, 0};
  uint16_t opt_id[5]    = {0, 0, 0, 0, 0};
  uint16_t opt_value[5] = {0, 0, 0, 0, 0};
  uint8_t  opt_param[5] = {0, 0, 0, 0, 0};
  int      opt_count    = 0;
  // 6e caractère de la balise : le booléen « type décoré » (= itemdb_isequip2).
  // C'est la SEULE information de type que le lien transporte.
  bool     equipable    = false;
  // 🔴 CHAMP PRIVÉ MOONLIGHT (séparateur `!`) — l'équipement CASSÉ. Le format
  // officiel ne le transporte pas : l'encodeur natif
  // (`UIWnd_AppendItemLinkButton 0x00865230`) n'écrit que refine/viewID/grade/
  // cartes/options, et `ItemSkillInfo+0x5d` n'y figure nulle part. On l'ajoute
  // donc nous-mêmes, et c'est SANS RISQUE pour un client qui ne le connaît pas :
  // son décodeur (`sub_0097E200`) range chaque champ dans une table indexée par
  // le RANG du séparateur dans `"!#$%&'()*+,-/"` — `!` vaut 0, un rang que rien
  // ne lit jamais. Le champ est donc lu, rangé, puis ignoré ; les autres ne
  // bougent pas. (Il est écrit EN DERNIER, après les options, pour qu'un lecteur
  // qui s'arrêterait avant ait quand même tout le standard.)
  bool     broken       = false;
};

// Relit une balise `<ITEML>…</ITEML>`. `tag` pointe sur le `<`, `end` sur la fin
// du texte. `tag_end` (facultatif) reçoit ce qui SUIT la fermante — ou `end` si
// elle manque, pour que l'appelant n'ait pas à re-chercher. Renvoie false sur une
// balise illisible ou sans nameid, `out` alors remis à neuf.
bool ParseChatLink(const char* tag, const char* end, ChatLink* out,
                   const char** tag_end = nullptr);

// Nom d'AFFICHAGE d'un lien, composé par le name-builder NATIF depuis un
// ItemSkillInfo fabriqué à partir de la balise : refine, grade, préfixes de
// cartes et « <forgeron>'s » compris — exactement ce qu'affiche le chat natif.
// Vide si la balise ne porte pas d'objet. ⚠ Rendu dans la CODE-PAGE DU CLIENT,
// comme BuildDisplayName : à convertir (`ro::WireToUtf8`) avant ImGui.
void BuildChatLinkName(const ChatLink& link, char* out, size_t out_size);

// Ré-encode une balise `<ITEML>` depuis un lien DÉJÀ relu. Il n'existe pas de
// second encodeur : l'ItemSkillInfo que la balise décrit est refabriqué, puis
// `BuildChatLink` fait le reste — un seul endroit connaît le format. C'est ce qui
// permet de RELAYER le lien d'un objet qu'on ne possède pas (Maj+clic sur le lien
// d'un autre joueur). Renvoie false si la balise ne porte pas d'objet.
bool BuildChatLinkFromLink(const ChatLink& link, char* out, size_t out_size);

// Description d'un lien de chat, armée pour le prochain OnProcessInput (même
// différé que DeferDescById, cf. plus bas). Contrairement à `DeferDescById`, la
// fenêtre reçoit les cartes, le refine, le grade et les options du lien : c'est
// la description de L'OBJET du joueur qui l'a posté, pas celle de l'item de base.
// Rien à maintenir en vie côté appelant — le lien est copié.
void DeferDescFromChatLink(const ChatLink& link, int mx, int my);

// Nom de BASE d'un item par son id, lu dans la DB de descriptions du client
// (chargement paresseux + recherche, cf. ragnarok/item_db.h). Jamais nul :
// « #<id> » quand la DB ne connaît pas l'objet, ce qui vaut mieux qu'un vide
// pour diagnostiquer.
//
// ⚠ C'est le nom NU. Il n'a ni refine, ni préfixes de cartes, ni suffixe
// « [N] » — pour ça il faut un ItemSkillInfo et BuildDisplayName ci-dessus.
// À utiliser quand on ne tient QU'un id : boutique NPC, cash shop, pièces
// jointes de courrier, matériaux de fabrication.
//
// Le nom est converti de CP949 vers UTF-8, prêt pour ImGui. Le résultat est
// mémorisé dans un cache de processus et reste valide jusqu'à la fin de la
// session : le pointeur peut être gardé d'une frame à l'autre.
//
// ⚠ Ne pas confondre avec `MoonlightUi::ItemName`, qui lit l'AUTRE source —
// `itemInfoMerged.lua` reparsé par nos soins, en CP949 — et qui rend nullptr
// pour un id inconnu. Celle-ci interroge la DB que le client a lui-même
// chargée ; c'est elle qu'il faut, sauf à vouloir précisément le fichier.
const char* NameById(uint32_t id);

// ── Le libellé d'un objet, UNE bonne fois ────────────────────────────────────
// Convention du projet, identique partout : `+refine préfixe_carte nom
// suffixe_carte [emplacements]`.
//
// Le début est déjà composé par le name-builder natif (cf. BuildDisplayName) :
// refine et affixes de cartes sont DANS `name`, il ne faut surtout pas les
// re-préfixer. Seul le suffixe « [N] » manque — le builder ne le compose pas —
// et c'est ce que cette fonction ajoute, quand N > 0.
//
// Écrit dans `out` et le renvoie, pour s'utiliser directement dans un
// `ImGui::TextUnformatted(itemcell::Label(buf, sizeof(buf), it.name, n))`.
// `name` vide ou nul -> « (?) », le repli déjà employé partout.
const char* Label(char* out, size_t out_size, const char* name, int slots);

// Aperçu RO au survol : tooltip fond blanc + cadre sysbox peint derrière par un
// split de canaux. À appeler HORS de toute fenêtre ImGui (il crée son popup).
//
// `cards` (4 max, 0 = vide) et `opts` sont des données d'INSTANCE : elles ne
// sont pas dans la DB client, l'appelant les lit dans SON ItemSkillInfo.
// `name` = nom déjà composé (cf. BuildDisplayName) ; nullptr = repli sur la DB.
// `damaged` = équipement cassé : titre « - Broken » + ombre rouge (cf. la
// section « Équipement CASSÉ » ci-dessus), comme la desc complète.
void DrawTooltip(uint32_t id, const uint32_t* cards, int card_count,
                 const itemdesc::SimpleOpt* opts, int opt_count,
                 int refine = 0, const char* name = nullptr,
                 bool damaged = false);

// ── Ouvrir la fenêtre de description native (0x0c) ───────────────────────────
// DEUX entrées, et la distinction n'est pas cosmétique : elle dit ce que la
// fenêtre POURRA rendre.
//
//   OpenDescFromInfo — on tient l'ItemSkillInfo que le SERVEUR a rempli (nœud de
//     liste inventaire/chariot/storage, slot d'équipement). Description
//     COMPLÈTE : cartes, refine, enchantements, options aléatoires.
//
//   OpenDescById — on n'a qu'un id (boutique NPC, cash shop, case munition).
//     La fenêtre reconstruit l'item de BASE depuis la DB client : le texte est
//     bon, mais il ne peut pas y avoir de cartes ni de refine, l'appelant
//     n'en connaît pas.
//
// ⚠ OnMsg 0x18 COPIE l'info qu'on lui passe (dans wnd+0xb8) : on ne cède la
// propriété de rien, et le tampon pile d'OpenDescById peut mourir au retour.

// Ouvre la description sur un ItemSkillInfo VIVANT. `info` nul = ne fait rien
// (c'est le cas normal quand la recherche dans la liste n'a rien trouvé, p. ex.
// l'item vient d'être consommé) — d'où l'usage direct
// `OpenDescFromInfo(FindInfoById(head, id), mx, my)`.
void OpenDescFromInfo(const void* info, int mx, int my);

// Ouvre la description à partir d'un id seul, en FABRIQUANT un ItemSkillInfo sur
// la pile (ctor + SetId + EnsureLoaded + « identifié »).
//
// `view` (viewID) et `location` (equip point, masque EQP_*) ne servent qu'à
// déverrouiller le bouton « aperçu » de la fenêtre — la description est correcte
// sans eux ; 0/0 pour un item non portable.
//
// `src`, si fourni, est un ItemSkillInfo vivant dont on recopie les champs
// non-string (type, cartes, refine, grade, options) : c'est le pont entre les
// deux familles, pour l'appelant qui a un vrai item mais préfère ne pas exposer
// l'objet du jeu. Il rend alors la même chose qu'OpenDescFromInfo. Les deux
// std::string membres (id @0x2c, resname @0x44) sont volontairement SAUTÉES —
// celles que SetId vient de construire sont gardées, sans quoi deux
// ItemSkillInfo partageraient un même tampon heap.
void OpenDescById(uint32_t id, uint16_t view, uint32_t location, int mx, int my,
                  const void* src = nullptr);

// ── Ouverture DIFFÉRÉE au relâchement du bouton ──────────────────────────────
// Les viewers n'appellent PAS OpenDesc* pendant leur rendu : ils ARMENT ici, et
// Bourgeon::OnProcessInput consomme via FlushDeferredDesc(), hors frame ImGui.
// Deux raisons, aucune cosmétique :
//  1. OpenDesc* rejoue un OnMsg NATIF ; l'appeler entre ImGui::NewFrame() et
//     Render() est proscrit (feedback_no_native_cmd_during_imgui_frame) ;
//  2. le focus de fenêtre reste acquis à la fenêtre CLIQUÉE tant que le bouton
//     est enfoncé. La remontée de la description est différée d'une frame
//     (drapeau posé par le hook OnMsg 0x18, consommé par ItemDescWindow →
//     SetNextWindowFocus) : ouvrir dès le clic, c'est remonter PUIS se faire
//     repasser devant si l'appui dure. Un clic BREF sortait la description
//     devant, un appui PROLONGÉ la faisait passer DERRIÈRE — c'est cette
//     asymétrie qui a longtemps rendu le bug insaisissable. En attendant la fin
//     du geste il n'y a plus de course, et un clic bref part immédiatement.
//
// Une seule demande en vol (une souris, un geste) : la dernière gagne.

// Par INDEX dans une liste de session (inventaire/chariot/storage, cf.
// FindInfoByIndex) : description COMPLÈTE, cartes/refine/enchants compris.
// L'ItemSkillInfo est re-résolu AU FLUSH, pas au clic — le nœud peut mourir
// entre les deux (consommation, transfert), un pointeur mémorisé serait périmé.
void DeferDescFromIndex(uintptr_t list_head, int index, int mx, int my);

// Par ID seul (`view`/`location`/`src` : mêmes rôles que dans OpenDescById).
// ⚠ `src` doit rester valide jusqu'au relâchement : un slot d'équipement de la
// session convient (adresse fixe), un tampon de pile non.
void DeferDescById(uint32_t id, uint16_t view, uint32_t location, int mx, int my,
                   const void* src = nullptr);

// Joue la demande en vol dès que les DEUX boutons sont relâchés. Appelé chaque
// frame par Bourgeon::OnProcessInput — nulle part ailleurs.
void FlushDeferredDesc();

// ── Retrouver un ItemSkillInfo dans une liste de session ─────────────────────
// Inventaire (0x015fbab0), chariot (0x015fbae0) et storage (0x015fbad8) sont
// TROIS std::list<ItemSkillInfo> de même forme ; `list_head` est l'adresse du
// global qui porte la sentinelle. On parcourt le MODÈLE SESSION et non la liste
// d'affichage de la fenêtre (wnd+0xe8) : cacher le natif vide la seconde, jamais
// le premier. Renvoient nullptr si absent ; SEH-gardées.
void* FindInfoById(uintptr_t list_head, uint32_t id);

// Le TOTAL d'un objet dans la liste, toutes piles confondues — ce que
// `FindInfoById` ne peut pas dire, puisqu'il s'arrête à la première.
// C'est la question « en ai-je assez ? », et c'est ainsi que le serveur y répond
// (`pet_evolution_requirements_check` additionne les piles avant de comparer).
// 0 si l'objet est absent.
int CountById(uintptr_t list_head, uint32_t id);
void* FindInfoByIndex(uintptr_t list_head, int index);

// ── Extraire TOUTE la liste en POD, une bonne fois ──────────────────────────
//
// Les trois viewers d'objets lisaient chacun leur liste avec leur propre boucle
// et leur propre `struct Item` — trois copies d'une quinzaine de lectures de
// champs, aux mêmes offsets, jusqu'aux cartes et aux options aléatoires. Les
// trois structures étaient déjà compatibles NOM POUR NOM : chariot et entrepôt
// n'étaient que des sous-ensembles de celle de l'inventaire.
//
// 🔴🔴 ET LA CORRECTION N'AVAIT ÉTÉ APPLIQUÉE QU'À DEUX DES TROIS. L'inventaire
// et le chariot encadrent CHAQUE ITEM d'un `__try` séparé, parce qu'un seul item
// fautif sous un `__try` GLOBAL fait avorter toute l'énumération — c'est le bug
// « 16 objets au lieu de 22 », documenté dans inventory_viewer. L'entrepôt, lui,
// gardait le `__try` unique autour de la boucle entière : un item corrompu y
// tronquait silencieusement la fin de la liste. La forme partagée est celle qui
// isole, donc l'entrepôt est corrigé du même coup.
//
// ⚠ Le SUIVANT est lu AVANT le contenu, et sous sa propre garde : un nœud
// corrompu arrête net, sans boucle infinie et sans perdre ce qui précède.
struct ItemRow {
  uint32_t id = 0;          // atoi de la std::string à info+0x2c — la SOURCE du jeu
  int      amount = 0;      // node+0x18 (= info+0x10)
  int      index = 0;       // info+4 : l'index CLIENT, argument des commandes
  uint32_t loc = 0;         // info+8 : masque d'emplacement d'équipement
  int      refine = 0;      // info+0x60
  int      type = 0;        // info+0 : type d'item (onglets de catégorie)
  uint8_t  identified = 0;  // info+0x5c (résolution d'icône)
  uint8_t  damaged = 0;     // info+0x5d : équipement CASSÉ (rendu rouge)
  uint8_t  favorite = 0;    // info+0x74 (onglet Favoris de l'inventaire)
  // Nom COMPLET (refine, cartes, forge), **EN UTF-8** — prêt pour ImGui.
  //
  // 🔴 96 et non 64 : `BuildDisplayName` écrit jusqu'à 64 octets dans la
  // code-page du CLIENT, et la conversion en UTF-8 fait GROSSIR le texte (un
  // accent latin passe de 1 à 2 octets, un caractère coréen de 2 à 3).
  char     name[96] = {0};
  // Données d'INSTANCE du stack (pas de la DB) : elles alimentent l'aperçu de
  // description au survol, aux offsets de la fenêtre de description native.
  uint32_t cards[4] = {0};  // info+0x1c : 4 emplacements carte/enchant (0 = vide)
  int      opt_count = 0;   // info+0x98 : nombre de random options
  int      total_slots = 0; // emplacements de carte (itemcell::SlotCount)
  struct Opt { int16_t index; int16_t value; uint8_t param; };
  Opt      opts[5] = {};    // info+0x9c : entrées de 5 octets
};

// Remplit `out` (au plus `max` entrées) et rend le nombre lu. Ne lève jamais :
// une liste en cours de remaniement rend simplement moins d'entrées.
//
// ⚠ Trois champs ne concernent qu'une fenêtre sur trois — `loc` et `favorite`
// (l'inventaire) — mais les lire coûte deux déréférencements et les porter, cinq
// octets par ligne. Les rendre optionnels aurait coûté un paramètre de plus et
// une branche dans la boucle, pour rien.
int ExtractList(uintptr_t list_head, ItemRow* out, int max);

// ⚠ Un septième site N'EST PAS passé par ici, et c'est délibéré :
// NpcDialogWindow::OpenItemDescById (lien <ITEM> dans un dialogue) écrit l'id
// ENTIER en info+0x00 — le champ que le name-builder lit comme un TYPE — et se
// passe d'EnsureLoaded comme de SetPos. C'est un chemin natif distinct, établi
// par RE sur ce cas précis ; l'aligner sur OpenDescById changerait ce qui part
// au natif sans qu'on sache encore pourquoi il diffère. À reprendre quand ce
// « pourquoi » sera désassemblé, pas avant.

}  // namespace itemcell
