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

// Une TUILE de grille : l'icône à sa taille NATIVE (le natif dessine le bmp 1:1,
// ~24 px dans une tuile de 32), centrée, et réduite SEULEMENT si elle déborde ;
// un « ? » grisé si l'icône manque ; puis, en bas à droite, le refine « +N »
// OU la quantité — jamais les deux, un équipement ne s'empile pas. Ce badge est
// écrit en noir cerné de blanc pour rester lisible sur n'importe quel fond,
// comme le fait le client.
//
// ⚠ C'est la présentation GRILLE. Les viewers en LISTE (vending_window,
// inventory_viewer en mode liste) composent leur ligne autrement — icône +
// libellé en widgets ImGui ou en texte à coordonnées. Les deux ne se ramènent
// pas l'une à l'autre, et ce n'est pas un défaut : ce sont deux mises en page.
//
// `p0`/`p1` sont les coins de la tuile, `cell` son côté.
void DrawTile(ImDrawList* draw_list, const ImVec2& p0, const ImVec2& p1,
              float cell, const ro::IconTex& icon, int refine, int amount);

// Nom d'AFFICHAGE composé par le name-builder natif : refine, préfixes de
// cartes, forge. `wnd` est la fenêtre native qui sert de contexte au builder,
// `info` l'ItemSkillInfo de l'objet. Écrit toujours une chaîne terminée dans
// `out` — vide si tout échoue, jamais d'indéterminé.
//
// ⚠ Le builder n'ajoute PAS le suffixe d'emplacements « [N] » : l'appelant le
// compose lui-même (comme le fait itemdesc::RenderSimpleDesc, pour la même
// raison). ⚠ Repli intégré : si la composition rend une chaîne vide, on retombe
// sur le nom de base. L'ensemble est sous SEH — un ItemSkillInfo à moitié
// initialisé ne doit pas tuer le client.
void BuildDisplayName(void* wnd, void* info, char* out, size_t out_size);

// Nombre TOTAL d'emplacements de carte de l'item décrit par `info`, 0 si aucun
// ou si la lecture échoue. Appel natif sous SEH.
int SlotCount(void* info);

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
// Le résultat est mémorisé dans un cache de processus et reste valide jusqu'à
// la fin de la session : le pointeur peut être gardé d'une frame à l'autre.
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
void DrawTooltip(uint32_t id, const uint32_t* cards, int card_count,
                 const itemdesc::SimpleOpt* opts, int opt_count,
                 int refine = 0, const char* name = nullptr);

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

// ── Retrouver un ItemSkillInfo dans une liste de session ─────────────────────
// Inventaire (0x015fbab0), chariot (0x015fbae0) et storage (0x015fbad8) sont
// TROIS std::list<ItemSkillInfo> de même forme ; `list_head` est l'adresse du
// global qui porte la sentinelle. On parcourt le MODÈLE SESSION et non la liste
// d'affichage de la fenêtre (wnd+0xe8) : cacher le natif vide la seconde, jamais
// le premier. Renvoient nullptr si absent ; SEH-gardées.
void* FindInfoById(uintptr_t list_head, uint32_t id);
void* FindInfoByIndex(uintptr_t list_head, int index);

// ⚠ Un septième site N'EST PAS passé par ici, et c'est délibéré :
// NpcDialogWindow::OpenItemDescById (lien <ITEM> dans un dialogue) écrit l'id
// ENTIER en info+0x00 — le champ que le name-builder lit comme un TYPE — et se
// passe d'EnsureLoaded comme de SetPos. C'est un chemin natif distinct, établi
// par RE sur ce cas précis ; l'aligner sur OpenDescById changerait ce qui part
// au natif sans qu'on sache encore pourquoi il diffère. À reprendre quand ce
// « pourquoi » sera désassemblé, pas avant.

}  // namespace itemcell
