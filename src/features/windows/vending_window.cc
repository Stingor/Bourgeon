#include "features/item_cell.h"
#include "ragnarok/item_db.h"
#include "ragnarok/globals.h"
#include "features/windows/vending_window.h"

#include <Windows.h>

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "bourgeon.h"  // Bourgeon::Instance().SendPacket
#include "features/moonlight_ui/moonlight_ui.h"  // SaveSettings (case Grille)
#include "imgui.h"
#include "ragnarok/game_scene.h"
#include "ragnarok/uiwnd.h"
#include "ui/icon_cache.h"
#include "ui/ro_imgui.h"
#include "ui/ro_widgets.h"        // mui::IsLastItemRightClicked
#include "utils/i18n.h"

// ── Constantes RE (client 20250716, base 0x400000) ───────────────────────────
// Source : docs/vending_window_re.md. Tout ce qui suit a été relu sur objet
// vivant, une fois en mode vente et une fois en mode échoppe d'achat.
namespace {

// Fenêtres de composition (UIMerchantShopMakeWnd).
constexpr int kWinVending     = 0x29;  // « Opening a stall »
constexpr int kWinBuyingStore = 0xAE;  // « Buying Store Window »
// Grilles « objets disponibles » (UIMerchantMirrorItemWnd), cachées elles aussi.
constexpr int kWinVendingMirror = 0x2A;  // « Available Items for Vending »
constexpr int kWinBuyingMirror  = 0xAF;  // « Available items: »
constexpr int kOffMirrorList    = 0xE8;  // std::list, MÊME layout de nœud

// « My Shop » = UIMerchantItemMyShopWnd (vtable 0x0103D100), la vue VENDEUR de sa
// propre échoppe, ouverte une fois la boutique lancée — donc APRÈS que les
// fenêtres de composition ont disparu, d'où son cycle de vie distinct.
// ⚠ À ne PAS confondre avec UIMerchantItemShopWnd (id 0x2B) ni avec
// UIMerchantItemPurchaseWnd (id 0x2C, l'achat CHEZ un vendeur) : trois classes
// voisines, trois rôles. Identifiée par RTTI sur session vivante.
constexpr int kWinMyShop       = 0x2D;  // vente
constexpr int kWinMyShopBuying = 0xB0;  // échoppe d'achat
constexpr int kOffMyShopList = 0xE8;   // même famille -> même offset de liste
// ⚠ Le MÊME champ ne dit pas la même chose selon le mode (posé par OnMsg 119) :
//   vente         -> zeny ENCAISSÉ depuis l'ouverture (part de 0, monte)
//   buying store  -> zeny RESTANT de la cagnotte (part de la limite, descend)
// D'où un libellé qui suit kOffMyShopMode ; « Encaissé » sur une échoppe d'achat
// était un contresens (c'est ce qu'il reste À DÉPENSER, pas ce qui est rentré).
// ⚠ CE N'EST PAS LE CUMUL EN VENTE. Le champ est POSÉ par OnMsg 119 à chaque
// vente : il ne contient que le montant de la DERNIÈRE. Relevé en jeu — shop à
// 30 555 z encaissés, champ à 7 (le prix du dernier objet vendu). En échoppe
// d'ACHAT en revanche, c'est bien un état : ce qu'il reste à dépenser.
constexpr int kOffMyShopLastSale = 0xF0;
constexpr int kOffMyShopMode = 0x100;  // 0 = vente, != 0 = échoppe d'achat
// Bouton « close ». ⚠ Il ne se contente PAS de fermer : sauf cas particulier, il
// dispatche CMode::SendMsg 81 (vente) / 270 (achat) — c'est-à-dire qu'il FERME LA
// BOUTIQUE. D'où un libellé explicite et une confirmation côté ImGui.
constexpr int kCmdMyShopClose = 201;

// « Item Sell History » = UIMerchantItemLogWnd (vtable 0x0103EB50, ctor
// 0x00963EF0), 450x152, ouverte PAR le bouton close de « Ma boutique ». Son propre
// bouton close porte aussi la cmd 201, mais chez elle c'est une fermeture PURE
// (aucun CMode::SendMsg derrière) -> on peut la câbler sans confirmation.
// Sa liste d'affichage suit la convention de la famille (+0xE8) ; elle est remplie
// ligne par ligne (OnMsg 23 = AJOUT d'une vente, pas une reconstruction).
constexpr int kWinSellLog       = 0x101;  // vente
constexpr int kWinSellLogBuying = 0x102;  // échoppe d'achat
constexpr int kOffSellLogList   = 0xE8;

// Largeur minimale des panneaux en AlwaysAutoResize : sans elle, un contenu plus
// court que le titre (« Aucune vente. ») fait rétrécir la fenêtre sous la barre
// de titre, qui se retrouve rognée.
constexpr float kMinPanelW = 260.0f;
// ... et largeur MAXIMALE, parce que la colonne « Objet » de ces mêmes panneaux
// s'auto-ajuste au nom le plus long (cf. plus bas) : un nom composé démesuré
// (« +10 Bloodlust Sword [Master Krishna] [3] ») étalerait sinon la fenêtre sur
// tout l'écran. Au-delà, le nom est rogné — mais le survol donne le nom complet.
constexpr float kMaxPanelW = 900.0f;

// ── Largeurs de la fenêtre de composition ────────────────────────────────────
// La fenêtre est REDIMENSIONNABLE, pas en AlwaysAutoResize. C'est ce qui permet
// aux colonnes « Objet » d'être en WidthStretch et à tout le reste de suivre la
// largeur réelle.
//
// La version auto-resize d'avant calait chaque section sur des constantes que je
// devais faire coïncider à la main — en oubliant que ImGui ajoute CellPadding.x
// DE CHAQUE CÔTÉ DE CHAQUE COLONNE : un tableau est plus large que la somme de
// ses colonnes, d'un montant qui dépend de leur NOMBRE. D'où une fenêtre calée
// sur le tableau le plus large, un panneau du haut plus étroit, et du contenu
// rogné à droite. Rien à recalculer ici : plus de pixel compté à la main.
constexpr float kColAct   = 26.0f;
constexpr float kColTotal = 88.0f;
constexpr float kColPrice = 96.0f;
constexpr float kColStock = 46.0f;
constexpr float kColQty   = 56.0f;   // échoppe d'achat uniquement
constexpr float kAvailStock = 46.0f;
constexpr float kAvailQty   = 56.0f;
constexpr float kAvailAct   = 52.0f;
// Champ « combien en poser » de la fenêtre surgissante du mode grille : assez
// large pour un lot à quatre chiffres plus les deux flèches d'InputInt.
constexpr float kAskFieldW  = 120.0f;
constexpr float kNameLabelW = 45.0f;
// Taille d'ouverture seulement (ImGuiCond_FirstUseEver) : ensuite c'est au joueur.
constexpr float kComposeW = 560.0f;
constexpr float kComposeH = 430.0f;
// Plancher de redimensionnement. En dessous, la fenêtre ne peut plus rien
// montrer d'utile : les colonnes chiffrées sont à largeur FIXE (prix, total,
// bouton), donc sous ~430 px la colonne « Objet » se réduit à rien ; et en
// hauteur il faut le nom, deux panneaux à quelques lignes, les totaux et la
// rangée de boutons. La fenêtre ne défilant plus, trop petite = contenu perdu
// et non plus simplement à faire défiler.
constexpr float kComposeMinW = 430.0f;
constexpr float kComposeMinH = 300.0f;

// Vente à un buying store : MÊME raisonnement que la composition, et pour une
// raison encore plus visible ici — les trois listes se vident et se remplissent
// l'une l'autre à chaque objet déplacé. En AlwaysAutoResize la fenêtre RÉTRÉCIT
// dès que « Mes objets » passe à « Rien à proposer. », et grandit au retrait :
// elle saute sous le curseur, et le bouton visé se dérobe.
constexpr float kBsW = 520.0f;
constexpr float kBsH = 500.0f;
constexpr float kBsPaneRows = 5.0f;  // hauteur des deux listes du haut

// ── Achat chez un vendeur : REDIMENSIONNABLE ─────────────────────────────────
// Elle était en AlwaysAutoResize, comme les deux petits panneaux du dessus. Ça ne
// tenait plus : cette fenêtre-là porte deux tableaux (l'offre et le panier) dont
// la hauteur suit le NOMBRE D'OBJETS. Une échoppe bien remplie la faisait
// descendre sous le bas de l'écran, sans le moindre moyen de la raccourcir — et
// l'échelle de l'interface a rendu le débordement systématique.
//
// Le passage en redimensionnable apporte aussi la barre de défilement (ImGui la
// pose dès que le contenu dépasse) et permet aux colonnes de nom d'être en
// WidthStretch : c'est le même raisonnement que la fenêtre de composition, et il
// est détaillé au-dessus de kColAct.
//
// Taille d'OUVERTURE seulement (FirstUseEver) : ensuite la fenêtre appartient au
// joueur, et ImGui la mémorise. Plus de largeur MAXIMALE — elle n'existait que
// pour empêcher l'auto-resize de s'étaler sur le nom d'objet le plus long, et
// c'est désormais le joueur qui décide.
constexpr float kBuyW    = 560.0f;
constexpr float kBuyH    = 460.0f;
constexpr float kBuyMinW = 380.0f;  // sous ça, les colonnes chiffrées mangent le nom
constexpr float kBuyMinH = 240.0f;  // un tableau, les totaux et la rangée de boutons

// ── Côté CLIENT : acheter chez un vendeur ────────────────────────────────────
// Deux fenêtres ouvertes ensemble quand on clique sur une échoppe (RTTI relu en
// live, jeu running) :
//   0x2B UIMerchantItemShopWnd     (vtable 0x0103D028) = l'offre du vendeur
//   0x2C UIMerchantItemPurchaseWnd (vtable 0x0103D2B0) = le panier + Total/buy/cancel
// Ce sont bien les fenêtres de l'ACHETEUR, pas la vue vendeur (celle-là est
// UIMerchantItemMyShopWnd 0x2D, cf. plus haut) — vérifié par RTTI sur session
// vivante, la déduction statique inverse était fausse.
constexpr int kWinVendorShop   = 0x2B;
constexpr int kWinVendorBasket = 0x2C;
constexpr int kOffVendorList   = 0xE8;   // offre (0x2B) ET panier (0x2C)
constexpr int kOffVendorGid    = 0x100;  // 0x2B : GID du vendeur (posé par OnMsg 28)
// (kOffBasketMode 0x108 retiré avec la fenêtre : le mode se lit sur les ids ouverts.)
// (kCmdBuyCancel 185 n'est plus pilotable : sa fenêtre est détruite. Ses deux effets
//  non-visuels sont rejoués par EndVendorDeal.)

// ── 🔴 Côté ACHETEUR : les deux natives ne survivent plus au tick ────────────
// Contrairement au reste de ce plugin — qui PILOTE ses natives et ne peut donc pas
// s'en passer — l'achat chez un vendeur émet déjà son paquet lui-même. Il ne restait
// que deux dépendances, toutes deux remplaçables :
//   1. la liste d'offres, lue dans 0x2B+0xE8 -> lue dans la SESSION (voir plus bas) ;
//   2. l'AID et l'identifiant d'échoppe, posés dans 0x2C par le handler du paquet
//      d'ouverture -> lus dans CE paquet, à la source.
//
// Paquet d'ouverture : ZC_PC_PURCHASE_ITEMLIST_FROMMC. ⚠ C'est bien 0x0b3d et PAS
// 0x0800 : le client ne dispatche que celui-là (l'autre n'a aucune entrée dans sa
// table). En régime OBSERVÉ, `data` = paquet+2, donc len@0, AID@2, venderId@6.
constexpr uint16_t kZcVendingList = 0x0b3d;

// ── Rapport de VENTE (le seul endroit où le cumul encaissé existe) ──────────
// Le serveur prévient le vendeur à CHAQUE vente : ZC_DELETEITEM_FROM_MCSTORE2,
// 18 octets `{op, index:2, amount:2, buyerCID:4, date:4, zeny:4}`. C'est bien
// 0x09E5 et pas 0x0137 pour ce PACKETVER (moonlight, packets.hpp : la variante
// courte s'arrête à PACKETVER < 20141016). En régime OBSERVÉ, `data` = paquet+2,
// donc le zeny de la vente est à data+12.
//
// 🔴 Pourquoi accumuler nous-mêmes plutôt que lire un champ : AUCUN champ de la
// fenêtre « Mon shop » ne porte le cumul. `+0xF0`, longtemps pris pour lui, vaut
// 7 en permanence — vérifié en jeu, inchangé après une vente à 250 000 z.
constexpr uint16_t kZcVendingReport = 0x09E5;
constexpr int      kVrZeny   = 12;  // dans `data`
constexpr int      kVrMinLen = 16;
constexpr int kVlAid    = 2;
constexpr int kVlVender = 6;
constexpr int kVlMinLen = 10;

// Listes de la SESSION. Décodées sur les getters natifs VendingXxx_GetCount
// (0x00d5ce40..0x00d5ce90), qui rendent tous `*(session + count)` : une std::list
// MSVC étant {head, size}, la tête est 4 octets AVANT le compteur.
// ⚠ Même contenu que la liste de la fenêtre, prix EFFECTIF compris :
// VendingOffer_GetAt ne fait que COPIER le nœud (aucun calcul de remise au moment
// de remplir la fenêtre), donc la remise est déjà dans la session.
constexpr int kOffSessOfferList = 0x1728;  // offre du vendeur (compteur @0x172C)

// Ce que la cmd 185 native faisait EN PLUS de fermer ses trois fenêtres — et qu'une
// simple destruction ne déclencherait donc jamais (relevé à l'instruction près dans
// UIMerchantItemPurchaseWnd_OnMsg @0x00956c70 et @0x00956c78) :
constexpr int       kCmdEndDeal         = 0x28;        // CMode::SendMsg : fin d'interaction
constexpr uintptr_t kVendingBasketClear = 0x00d56300;  // __thiscall(&session)
using BasketClear_t = void(__thiscall*)(void*);

// ── Côté VENDEUR : je vends à l'échoppe d'ACHAT d'un autre joueur ────────────
// TROIS fenêtres ouvertes ensemble quand on clique sur un buying store. Ce sont
// les MÊMES CLASSES que le mode achat, instanciées sous d'autres identifiants —
// d'où la symétrie complète des offsets (RTTI relu en live, jeu running) :
//   0xB1 UIMerchantItemShopWnd     (vt 0x0103D028) « Wanted items - <acheteur> »
//   0xB2 UIMerchantItemPurchaseWnd (vt 0x0103D2B0) « Selling Items » + sell/cancel
//   0xB3 UIMerchantMirrorItemWnd   (vt 0x0103D610) « Available items: »
//
// ⚠ Ma lecture précédente était fausse : je croyais que ce mode réutilisait
// 0x2B/0x2C avec `kOffBasketMode != 0`. Non — le natif ouvre 0xB1/0xB2/0xB3, et
// c'est UIMerchantItemPurchaseWnd_OnMsg qui le prouve : ses cmd 184/185 ferment
// 177/178/179 quand le mode vaut 1, et 43/44 sinon. Le test sur 0x2C ne pouvait
// donc jamais se déclencher, et le repli « rendre la main au natif » était mort.
constexpr int kWinBsWanted   = 0xB1;
constexpr int kWinBsSellList = 0xB2;
constexpr int kWinBsMirror   = 0xB3;
constexpr int kOffBsBuyerAid = 0x100;  // 0xB1 : AID de l'acheteur
// 0xB1+0x108 : ce que l'acheteur peut ENCORE payer (part de sa limite, descend à
// chaque achat). C'est exactement le `*(g_BuyingStoreWantedWnd+264)` que teste le
// bouton « sell » natif avant d'émettre.
constexpr int kOffBsZenyLeft = 0x108;
constexpr int kCmdBsSell     = 184;
constexpr int kCmdBsCancel   = 185;
// Plafond natif par ligne, appliqué dans UIMerchantItemPurchaseWnd_OnMsg msg 38.
constexpr int kBsMaxAmount = 30000;

// ⚠ DEUX prix par ligne, et le natif les distingue :
//   nœud+0x1C = prix de BASE, nœud+0x20 = prix EFFECTIF (après Discount).
// UIMerchantItemPurchaseWnd_DrawContent affiche « base -> effectif » quand ils
// diffèrent, et le total natif se calcule sur l'EFFECTIF (qty * nœud+0x20).
// Lire +0x1C ici afficherait un prix trop élevé et un total faux.
constexpr int kNodePriceEff = 0x20;

// CZ_PC_PURCHASE_ITEMLIST_FROMMC. Layout relevé sur le constructeur de paquet
// natif (0x00C8E4C0 environ) : en-tête de 12 octets puis 4 octets par ligne.
//   +0 opcode:2 | +2 len:2 | +4 AID:4 | +8 UniqueID:4 | [ amount:2, index:2 ]*
// `len` = 12 + 4*n, et le natif REFUSE au-delà de 0x800 — même garde ici.
constexpr uint16_t kCzPurchaseFromMc = 0x0801;
constexpr int      kPurchaseHdr      = 12;
constexpr int      kPurchaseMaxLen   = 0x800;

// CZ_REQ_BUY_FROMMC 0x0130 {AID:4} — 6 octets. C'est la requête envoyée par le
// clic sur l'échoppe ; le serveur répond par la liste complète.
//
// ⚠ Après un achat le serveur ne renvoie RIEN au client (vérifié dans
// `vending_purchasereq` : seul `clif_buyvending` = ZC 0x0135 {index,amount,result}
// part, et il ne porte pas la liste). Le client natif s'en tire en FERMANT
// l'échoppe dès le clic « buy » — le commentaire rAthena le dit noir sur blanc :
// « this was automatically done by the client ». Comme on garde la fenêtre
// ouverte pour enchaîner les achats, c'est à nous de redemander la liste.
constexpr uint16_t kCzVendingListReq = 0x0130;

// ── Nom d'affichage complet (refine, cartes, slots, enchant) ──────────────
// Le nom brut de la DB ne dit rien d'une arme +10 sertie : c'est
// BuildDisplayName qui compose « +10 Hydra Sword [3] ». Même appel que les
// viewers inventaire / cart / storage, avec la fenêtre native pour contexte.

// SEH ISOLÉ : un item dont BuildDisplayName plante ne doit pas avorter TOUTE
// l'énumération (leçon de l'inventaire, où un seul item fautif faisait
// disparaître la moitié de la liste).

// ── Description d'objet ──────────────────────────────────────────────────────
// Le nœud porte un ItemSkillInfo à +0x08 ; cartes, refine et options
// d'INSTANCE n'existent que là (pas dans la DB client), il faut donc les lire au
// nœud pour un aperçu fidèle. Offsets identiques à ceux des autres viewers, ici
// exprimés en NŒUD (= ItemSkillInfo + 0x08).
constexpr int kNodeCards    = 0x24;  // ISI+0x1C : 4 cartes
constexpr int kNodeIdent    = 0x64;  // ISI+0x5C : identifié ? (change le nom)
constexpr int kNodeDamaged  = 0x65;  // ISI+0x5D : équipement CASSÉ (rendu rouge, cf. itemcell)
constexpr int kNodeRefine   = 0x68;  // ISI+0x60
constexpr int kNodeOptCount = 0xA0;  // ISI+0x98
constexpr int kNodeOpts     = 0xA4;  // ISI+0x9C, entrées de 5 octets
constexpr int kMaxOpts      = 5;

// Fenêtre de description native (id 0xC) : MakeWindow puis OnMsg 0x18 avec le
// POINTEUR vers l'ItemSkillInfo — c'est le chemin du clic droit natif, et
// item_desc_window reconnaît la fenêtre pour en rendre sa version enrichie.

// Résolution GID -> nom, pour le titre (cf. docs/entity_nameplate_re.md).
using GameModeGetActive_t = void*(__fastcall*)(int);
using NameDictGetEntry_t  = void*(__thiscall*)(void*, unsigned);
constexpr uintptr_t kNameDictGetEntry  = 0x005A1460;
constexpr int       kNameInfoStr       = 0x04;   // std::string du nom

// ── Bouton « Import » (cmd 560) ──────────────────────────────────────────────
// « Get the item information registered in the previous stall » : recharge la
// dernière échoppe montée par ce personnage. cmd 560 déclenche
// VendingSnapshot_LoadForChar(aid, cid, monde, mode), puis
// UIMerchantShopMakeWnd_ImportSavedShop repose les objets dans la session et
// recopie prix/quantités dans les edits natifs.
//
// On clique donc le bouton natif, et on lit les prix à la SOURCE qu'il utilise
// lui-même : le vecteur snapshot. Aucun besoin de savoir lire le texte d'un
// UIEdit — c'était le blocage supposé.
//
// ⚠ Lecture AVANT le clic, obligatoirement : le handler s'en fait une copie
// locale puis VIDE le vecteur en sortant (`sub_5E2A60` : `end = begin`).
//
// ⚠ MAIS le snapshot N'EST PAS LOCAL, et ne passe pas non plus par le protocole
// de jeu : VendingSnapshot_LoadForChar poste un AsyncWork
// « MerchantStoreInformation_Load » (CMCStoreInfoBackupLoadAsyncWork) vers le
// SERVICE WEB (AssistAddr d'ExternalSettings, couche libcurl), avec AID + CID +
// nom de monde + store_type + une clé « b8e5c779ed77e055 ». La sauvegarde
// symétrique part à la fermeture de la boutique et sérialise l'échoppe en JSON
// (jsoncpp, Json::StyledWriter) sous la clé « data ».
// Côté serveur ça atterrit dans la table `merchant_configs`
// (world_name, account_id, char_id, store_type, data longtext).
//
// Conséquence concrète : ce bouton dépend d'un ENDPOINT HTTP, pas du serveur de
// jeu. S'il n'est pas servi, l'appel échoue en silence et l'import ne ramène
// rien — d'où le repli sur ~30 frames plutôt qu'une attente bloquante. Le code
// ci-dessous reste correct et sans risque dans les deux cas.
constexpr int kCmdImport = 560;
constexpr uintptr_t kSnapBegin  = 0x01602400;  // vecteur d'ItemSkillInfo
constexpr uintptr_t kSnapEnd    = 0x01602404;
constexpr uintptr_t kSnapName   = 0x016023E4;  // std::string : nom de la boutique
constexpr int kSnapStride = 0xF8;
constexpr int kSnapQty    = 0x10;
constexpr int kSnapPrice  = 0x14;
constexpr int kSnapId     = 0x2C;  // std::string = itemId en texte (cf. nœuds)

// Champs de UIMerchantShopMakeWnd.
constexpr int kOffNameEdit  = 0xCC;   // UIEdit nom de la boutique
constexpr int kOffPrice0    = 0xE0;   // UIEdit prix, tableau de `slots` entrées
constexpr int kOffQty0      = 0x114;  // UIEdit quantité (échoppe d'achat seule)
constexpr int kOffZenyLimit = 0x128;  // UIEdit limite de zeny (achat seul)
constexpr int kOffSlots     = 0x12C;  // nombre de lignes créées
constexpr int kOffMode      = 0x130;  // 0 = vente, 1 = échoppe d'achat
constexpr int kOffList      = 0x148;  // std::list des objets posés (sentinelle)
constexpr int kOffCount     = 0x14C;  // nombre d'objets posés

// Nœud de la liste : payload ItemSkillInfo à nœud+8.
constexpr int kNodeIndex = 0x0C;  // index source (cart en vente)
constexpr int kNodeQty   = 0x18;  // quantité posée
constexpr int kNodePrice = 0x1C;  // prix unitaire (0 tant que l'échoppe n'est pas ouverte)
constexpr int kNodeName  = 0x34;  // std::string = l'itemId EN TEXTE ("714")
constexpr int kNodeSlots = 0x90;  // short : nombre de cartes

// ── API de session (g_session) ───────────────────────────────────────────────
// TOUTES __thiscall avec ecx = &g_session. Conventions relevées sur des sites
// d'appel réels (UIMerchantShopMakeWnd_ImportSavedShop, les deux OnMsg case 38),
// pas déduites : c'est ce qui autorise à les appeler sans crasher.
using SessGetAt_t   = void(__thiscall*)(void*, void*, int);
using SessAmount_t  = int (__thiscall*)(void*, int);
using SessMerge_t   = char(__thiscall*)(void*, void*, int, int);
using SessConsume_t = char(__thiscall*)(void*, void*, int, int);
using SessConsume2_t = void(__thiscall*)(void*, void*, int);
constexpr uintptr_t kAvailGetAt   = 0x00D5C160;
constexpr uintptr_t kShopGetAt    = 0x00D5BEA0;

// ⚠ PIÈGE DE NOMMAGE — ces deux-là ne font PAS ce que leur position suggère, et
// les prendre pour un « puis-je ajouter ? » suivi d'un « ajoute » produit des
// quantités délirantes (l'objet s'empile dans l'échoppe sans jamais être retiré
// du stock disponible). Décompilées, elles disent :
//
//   0x00D54C40(session, rec, no_merge, refresh)  -> liste ÉCHOPPE (session+0x1748)
//       Ajoute un nœud, ou CUMULE la quantité si l'index source y est déjà.
//       Renvoie 1 si un NOUVEAU nœud a été créé, 0 s'il a fusionné/refusé.
//       C'est déjà l'ajout : il n'y a rien à « autoriser » après.
//   0x00D57C60(session, rec, whole_node, refresh) -> liste DISPONIBLES (+0x172C)
//       DÉCRÉMENTE la quantité disponible de rec+0x10 (ou supprime le nœud).
//
// Les deux doivent donc être appelées à la SUITE, sans tester le retour de la
// première — c'est exactement ce que fait le handler de dépôt natif en vente.
constexpr uintptr_t kShopAddOrMerge  = 0x00D54C40;
constexpr uintptr_t kAvailConsume    = 0x00D57C60;
// Chemin retour, strictement symétrique (mêmes conventions, mêmes pièges).
constexpr uintptr_t kAvailAddOrMerge = 0x00D54D80;
constexpr uintptr_t kShopConsume     = 0x00D57AC0;

// ── Panier de vente à un buying store (une TROISIÈME liste de session) ───────
// Ni la liste « échoppe » ni la liste « disponibles » : VendingBasket_*, à
// session+0x1740. C'est elle que « Selling Items » (0xB2) recopie à chaque
// OnMsg 23, et c'est elle que le bouton « sell » natif sérialise.
constexpr uintptr_t kBasketGetAt      = 0x00D5C580;  // (session, out_rec, i)
constexpr uintptr_t kBasketAddOrMerge = 0x00D54EA0;  // (session, rec) — 2 args !
constexpr uintptr_t kBasketRemove     = 0x00D57E40;  // (session, rec) — 2 args !
using SessRec2_t = void*(__thiscall*)(void*, void*);

// ⚠ Le 3e paramètre de VendingAvail_{Consume,AddOrMerge}Item n'est PAS un simple
// mode : décompilées, elles s'en servent DEUX fois.
//   - non nul  -> on retire/rend le NŒUD ENTIER (et pas seulement rec+0x10) ;
//   - et il choisit la fenêtre miroir à re-notifier : 1 = composition d'échoppe
//     d'achat (0xAF), 2 = « Available items » du buying store (0xB3), 0 = miroir
//     de vente (0x2A).
// Le natif passe 2 ici. Conséquence VISIBLE et voulue : mettre ne serait-ce
// qu'une unité d'une pile en vente fait disparaître la pile ENTIÈRE des objets
// disponibles — un index d'inventaire ne peut figurer qu'une fois dans la vente
// (le serveur refuse les doublons, cf. buyingstore_trade).
constexpr int kBsMirrorMode = 2;
// Quantité par index source, dans l'une ou l'autre liste. `kAvailAmountBySrc` est
// LA source de vérité du « reste à poser » : la liste des disponibles EST
// décrémentée à chaque pose, sa quantité est donc déjà le reste.
constexpr uintptr_t kAvailAmountBySrc = 0x00D5C200;
constexpr uintptr_t kShopAmountBySrc  = 0x00D5BF40;

// L'enregistrement d'objet manipulé par cette API (ItemSkillInfo). Il porte DEUX
// std::string qu'il faut détruire après usage — le natif le fait à chaque tour de
// boucle, on fait pareil, sinon on fuit un buffer par objet manipulé.
constexpr int    kRecSize    = 0x100;
constexpr int    kRecSrcIdx  = 0x04;  // index source (cart / inventaire)
constexpr int    kRecQty     = 0x10;  // quantité — modifiable AVANT de poser
constexpr int    kRecString1 = 0x2C;
constexpr int    kRecString2 = 0x44;
using StrDtor_t = void(__fastcall*)(void*);

// Commandes du msg 6 (clic bouton) — cf. §5 de la doc.
constexpr int kMsgButton   = 6;
constexpr int kMsgRebuild  = 23;  // reconstruit la liste d'affichage depuis la session
// Charges de glisser INTERNES à cette fenêtre (ImGui, pas le drag natif — celui-ci
// transporte un ItemSkillInfo complet dans le mode de jeu et n'est pas reproductible).
// Elles ne portent qu'un INDEX de ligne, relu juste avant la mutation.
constexpr const char* kDndAvail = "VEND_AVAIL";  // stock -> échoppe
constexpr const char* kDndRow   = "VEND_ROW";    // échoppe -> stock

constexpr int kCmdOk       = 184;  // valide et ouvre l'échoppe
constexpr int kCmdCancel   = 185;
constexpr int kCmdSafeChk  = 213;  // bascule « safe check for over 10 mil zeny »
constexpr int kVfEditSetText = 212;  // UIEdit::SetText(const char*)

// Globals.
constexpr uintptr_t kSafeCheckFlag = 0x015FFFA1;  // octet, persistant

using EditSetText_t = void(__thiscall*)(void*, const char*);


// ── Accès natif, tout sous SEH et POD uniquement ─────────────────────────────

void* FindWnd(int id) { return uiwnd::SafeFindWindow(id); }

// Le pointeur `p` est-il ENCORE l'une des fenêtres natives du plugin ?
//
// Sert à revalider un pointeur mémorisé pendant le rendu et consommé plus tard
// (cf. l'ouverture de description différée dans FlushPending). Ici on ne peut
// pas ranger un simple identifiant comme le fait QueueCommand : les cellules
// reçoivent le pointeur DÉJÀ résolu par leur panneau, et plusieurs panneaux
// partagent le même id selon le mode vente/achat. On revalide donc le pointeur
// contre les fenêtres VIVANTES — une fenêtre détruite entre-temps n'est plus
// dans la liste, et l'ouverture est simplement abandonnée (pas d'usage après
// libération).
bool IsLiveShopWnd(void* p) {
  if (!p) return false;
  static const int kAll[] = {
      kWinVending,   kWinBuyingStore,   kWinVendingMirror, kWinBuyingMirror,
      kWinMyShop,    kWinMyShopBuying,  kWinSellLog,       kWinSellLogBuying,
      kWinVendorShop, kWinVendorBasket, kWinBsWanted,      kWinBsSellList,
      kWinBsMirror};
  for (int id : kAll)
    if (FindWnd(id) == p) return true;
  return false;
}

void HideWnd(void* w) { uiwnd::SafeSetVisible(w, false); }
// ⚠ DÉTRUIT la fenêtre (la destruction est mise en file, d'où l'intérêt de masquer
// d'abord : sans ça une frame native passerait avant qu'elle ne disparaisse).
void CloseWnd(int id) { uiwnd::SafeCloseWindow(id); }

// Rend une fenêtre qu'on avait masquée. Nécessaire côté acheteur : le mode
// (achat chez un vendeur / vente à une échoppe d'achat) n'est connu qu'APRÈS la
// création, quand OnMsg 28 le pose — on masque d'abord, et on rend la main au
// natif si on tombe sur le mode qu'on ne sait pas encore remplacer.
void ShowWnd(void* w) {
  __try {
    if (w) *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(w) + uiwnd::kOffVisible) = 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

int ReadInt(void* base, int off, int fallback = 0) {
  __try {
    return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(base) + off);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}

int PlayerZeny() {
  __try {
    return *reinterpret_cast<int*>(rag::kZenyAddr);
  } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool SafeCheckOn() {
  __try {
    return *reinterpret_cast<uint8_t*>(kSafeCheckFlag) != 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Snapshot décodé : id d'objet + prix + quantité, dans l'ordre du JASON stocké.
// On lit l'ID en plus des prix parce que le natif peut ne PAS reposer toutes les
// lignes (un objet vendu depuis, ou sorti du cart, est simplement sauté) : caler
// nos prix sur l'INDICE de ligne les décalerait tous. On les recale par ID.
int ReadSnapshot(uint32_t* ids, int* prices, int* amounts, int max) {
  int n = 0;
  __try {
    auto* b = *reinterpret_cast<uint8_t**>(kSnapBegin);
    auto* e = *reinterpret_cast<uint8_t**>(kSnapEnd);
    if (b && e && e > b) {
      const int count = static_cast<int>((e - b) / kSnapStride);
      for (int i = 0; i < count && i < max; ++i) {
        const uint8_t* el = b + i * kSnapStride;
        amounts[i] = *reinterpret_cast<const int*>(el + kSnapQty);
        prices[i]  = *reinterpret_cast<const int*>(el + kSnapPrice);
        // Même convention que les nœuds de liste : l'itemId est une std::string.
        const char* sb = reinterpret_cast<const char*>(el + kSnapId);
        const uint32_t capa = *reinterpret_cast<const uint32_t*>(sb + 0x14);
        const char* s = (capa > 15) ? *reinterpret_cast<const char* const*>(sb) : sb;
        ids[i] = s ? static_cast<uint32_t>(atoi(s)) : 0;
        ++n;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { /* snapshot absent : on garde ce qu'on a */ }
  return n;
}

// Nom de boutique mémorisé (std::string MSVC : taille +0x10, capacité +0x14).
void ReadSnapshotName(char* out, size_t cap) {
  out[0] = '\0';
  __try {
    const char* base = reinterpret_cast<const char*>(kSnapName);
    const uint32_t size = *reinterpret_cast<const uint32_t*>(base + 0x10);
    const uint32_t capa = *reinterpret_cast<const uint32_t*>(base + 0x14);
    const char* s = (capa > 15) ? *reinterpret_cast<const char* const*>(base) : base;
    if (s && size > 0 && size < cap) {
      std::memcpy(out, s, size);
      out[size] = '\0';
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Ligne brute lue dans la liste native. POD : traversable sous __try.
struct RawRow {
  uint32_t id;
  int      index;
  int      amount;
  int      slots;
  int      price;      // nœud+0x1C : prix de BASE
  int      price_eff;  // nœud+0x20 : prix EFFECTIF (Discount appliqué)
  uint32_t cards[4];
  int      refine;
  int      opt_count;
  int16_t  opt_index[kMaxOpts];
  int16_t  opt_value[kMaxOpts];
  uint8_t  opt_param[kMaxOpts];
  int      ident;      // identifié ? (change le nom affiché)
  uint8_t  damaged;    // ISI+0x5D : équipement cassé (rendu rouge)
  void*    node;       // nœud d'origine, pour composer le nom hors du __try
  char     name[64];   // nom d'AFFICHAGE complet (BuildDisplayName)
};

// Cache des noms composés. BuildDisplayName alloue et compose : le rappeler pour
// chaque ligne à chaque frame, sur une liste qui peut faire 128 objets, c'est le
// même piège que les mesures GDI qui gelaient le chat. La clé est la signature
// d'INSTANCE (id, refine, cartes, slots, identifié) — tout ce dont le nom
// dépend ; les options aléatoires n'y entrent pas.
std::unordered_map<uint64_t, std::string> g_display_name_cache;

uint64_t DisplayNameKey(const RawRow& r) {
  uint64_t k = r.id;
  k = k * 1000003u + static_cast<uint64_t>(r.refine);
  k = k * 1000003u + static_cast<uint64_t>(r.slots);
  k = k * 1000003u + static_cast<uint64_t>(r.ident != 0);
  for (int c = 0; c < 4; ++c) k = k * 1000003u + r.cards[c];
  return k;
}

// Passe de nommage, VOLONTAIREMENT hors du __try de ReadRows : la table est un
// conteneur C++, et MSVC interdit les objets à destructeur dans une fonction qui
// contient __try. itemcell::BuildDisplayName porte sa propre garde.
void ResolveDisplayNames(RawRow* rows, int count) {
  for (int i = 0; i < count; ++i) {
    if (rows[i].id == 0 || !rows[i].node) continue;
    const uint64_t key = DisplayNameKey(rows[i]);
    auto it = g_display_name_cache.find(key);
    if (it == g_display_name_cache.end()) {
      // Borne simple : les instances distinctes se comptent en centaines, mais
      // rien ne garantit qu'un serveur exotique n'en génère pas beaucoup plus.
      if (g_display_name_cache.size() > 2048) g_display_name_cache.clear();
      char buf[64];
      itemcell::BuildDisplayName(reinterpret_cast<uint8_t*>(rows[i].node) + 0x08, buf,
                    sizeof(buf));
      it = g_display_name_cache.emplace(key, buf).first;
    }
    std::strncpy(rows[i].name, it->second.c_str(), sizeof(rows[i].name) - 1);
    rows[i].name[sizeof(rows[i].name) - 1] = '\0';
  }
}

// Parcourt la std::list circulaire des objets posés. Renvoie le nombre de lignes
// écrites dans `out`. Une liste incohérente (elle mute pendant un glisser) laisse
// ce qui a déjà été lu — c'est la même garde que les autres viewers.
int ReadRows(void* wnd, int list_off, RawRow* out, int max) {
  int n = 0;
  __try {
    auto* base = reinterpret_cast<uint8_t*>(wnd);
    void* head = *reinterpret_cast<void**>(base + list_off);
    if (!head) return 0;
    void* node = *reinterpret_cast<void**>(head);
    // `guard` borne le parcours si la liste est corrompue (elle peut muter sous
    // nos pieds) ; `max` borne l'écriture. Les deux sont nécessaires.
    int guard = 0;
    while (node && node != head && n < max && guard < 512) {
      auto* p = reinterpret_cast<uint8_t*>(node);
      RawRow r;
      std::memset(&r, 0, sizeof(r));
      for (int c = 0; c < 4; ++c)
        r.cards[c] = *reinterpret_cast<uint32_t*>(p + kNodeCards + c * 4);
      r.refine = *reinterpret_cast<int*>(p + kNodeRefine);
      int nopt = *reinterpret_cast<int*>(p + kNodeOptCount);
      if (nopt < 0) nopt = 0;
      if (nopt > kMaxOpts) nopt = kMaxOpts;
      r.opt_count = nopt;
      for (int k = 0; k < nopt; ++k) {
        const uint8_t* e = p + kNodeOpts + k * 5;
        r.opt_index[k] = *reinterpret_cast<const int16_t*>(e);
        r.opt_value[k] = *reinterpret_cast<const int16_t*>(e + 2);
        r.opt_param[k] = e[4];
      }
      r.index  = *reinterpret_cast<int*>(p + kNodeIndex);
      r.amount = *reinterpret_cast<int*>(p + kNodeQty);
      r.price  = *reinterpret_cast<int*>(p + kNodePrice);
      r.price_eff = *reinterpret_cast<int*>(p + kNodePriceEff);
      r.slots  = *reinterpret_cast<short*>(p + kNodeSlots);
      // nœud+0x34 = std::string MSVC (+0x14 = capacité) : au-delà de 15 octets le
      // buffer est déporté. Son contenu est l'itemId en TEXTE, pas le nom.
      const char* sbase = reinterpret_cast<const char*>(p + kNodeName);
      const uint32_t cap = *reinterpret_cast<const uint32_t*>(sbase + 0x14);
      const char* str = (cap > 15) ? *reinterpret_cast<const char* const*>(sbase)
                                   : sbase;
      r.id = str ? static_cast<uint32_t>(atoi(str)) : 0;
      // Le nom composé se fait APRÈS, hors du __try (cf. ResolveDisplayNames) :
      // il passe par un cache, donc par un conteneur C++.
      r.ident = *reinterpret_cast<uint8_t*>(p + kNodeIdent);
      r.damaged = *reinterpret_cast<uint8_t*>(p + kNodeDamaged);
      r.node = node;
      out[n++] = r;
      node = *reinterpret_cast<void**>(node);
      ++guard;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
  ResolveDisplayNames(out, n);
  return n;
}

void SetEditText(void* wnd, int edit_off, const char* text) {
  __try {
    void* edit = *reinterpret_cast<void**>(
        reinterpret_cast<uint8_t*>(wnd) + edit_off);
    if (edit) uiwnd::Vf<EditSetText_t>(edit, kVfEditSetText)(edit, text);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void SendButton(void* wnd, int cmd) {
  __try {
    uiwnd::OnMsg(wnd, kMsgButton, cmd, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// msg 23 : la fenêtre reconstruit sa liste d'affichage depuis la session. p1 = 0
// -> reconstruction simple (p1 > 0 signifierait « la ligne p1-1 vient d'être
// retirée », ce qui déclencherait EN PLUS le décalage des edits natifs — on n'en
// veut pas, nos prix sont à nous).
void SendRebuild(void* wnd) {
  __try {
    uiwnd::OnMsg(wnd, kMsgRebuild, 0, 0, 0, 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── API de session, tout sous SEH ────────────────────────────────────────────
// `rec` est un buffer de pile de kRecSize octets. On le met à zéro avant chaque
// GetAt (un std::string tout-à-zéro est un SSO vide valide) et on détruit ses deux
// chaînes après usage.

// ⚠ `SessionBase` et surtout PAS `Session` : une CLASSE `Session` existe au niveau
// global. Depuis une méthode de VendingWindow, elle et ce helper sont trouvés en
// même temps -> « symbole ambigu ». Le nom court ne marchait que par chance, tant
// qu'on ne l'appelait que depuis cet espace de noms anonyme.
void* SessionBase() { return reinterpret_cast<void*>(rag::kSessionAddr); }

void RecDtor(uint8_t* rec) {
  __try {
    reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(rec + kRecString2);
    reinterpret_cast<StrDtor_t>(rag::kStdStringDtorAddr)(rec + kRecString1);
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Pose l'objet disponible n° `index`. `qty` <= 0 = tout ce qui reste.
//
// La quantité posée est bornée par ce qui reste RÉELLEMENT disponible, relu dans
// la liste des disponibles juste avant l'appel : c'est la seule valeur qui fasse
// autorité (l'affichage, lui, peut dater d'une frame).
bool SessionPlace(int index, int qty, bool buying) {
  bool ok = false;
  uint8_t rec[kRecSize];
  std::memset(rec, 0, sizeof(rec));
  __try {
    reinterpret_cast<SessGetAt_t>(kAvailGetAt)(SessionBase(), rec, index);
    const int src = *reinterpret_cast<int*>(rec + kRecSrcIdx);
    const int remaining =
        reinterpret_cast<SessAmount_t>(kAvailAmountBySrc)(SessionBase(), src);
    if (remaining > 0) {
      const int want = (qty > 0 && qty < remaining) ? qty : remaining;
      *reinterpret_cast<int*>(rec + kRecQty) = want;
      const int mode = buying ? 1 : 0;
      // En VENTE les deux appels s'enchaînent sans condition (cf. le pavé sur le
      // piège de nommage). En ÉCHOPPE D'ACHAT, un retour 0 veut dire « cet objet
      // y est déjà » : le natif refuse alors, et ne consomme rien.
      const char added =
          reinterpret_cast<SessMerge_t>(kShopAddOrMerge)(SessionBase(), rec, mode, 1);
      if (!buying || added) {
        reinterpret_cast<SessConsume_t>(kAvailConsume)(SessionBase(), rec, mode, 1);
        ok = true;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  RecDtor(rec);
  return ok;
}

// Retire l'objet posé n° `row` : il repart dans les disponibles.
//
// Chemin strictement symétrique de la pose, avec le MÊME piège : `kAvailAddOrMerge`
// rend déjà l'objet au stock (et renvoie 0 quand il a fusionné avec une pile
// existante), `kShopConsume` le retire de l'échoppe. Tester le retour de la
// première pour décider d'appeler la seconde laissait l'objet dans l'échoppe TOUT
// EN le rendant au stock — d'où les quantités qui gonflaient à chaque retrait.
bool SessionTakeBack(int row, bool buying) {
  bool ok = false;
  uint8_t rec[kRecSize];
  std::memset(rec, 0, sizeof(rec));
  __try {
    reinterpret_cast<SessGetAt_t>(kShopGetAt)(SessionBase(), rec, row);
    const int src = *reinterpret_cast<int*>(rec + kRecSrcIdx);
    const int mode = buying ? 1 : 0;
    // Garde du natif : rien à rendre si cet index source n'est pas posé.
    if (reinterpret_cast<SessAmount_t>(kShopAmountBySrc)(SessionBase(), src) > 0) {
      const char given =
          reinterpret_cast<SessMerge_t>(kAvailAddOrMerge)(SessionBase(), rec, mode, 1);
      if (!buying || given) {
        reinterpret_cast<SessConsume2_t>(kShopConsume)(SessionBase(), rec, mode);
        ok = true;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  RecDtor(rec);
  return ok;
}

// Met `qty` unités de l'objet disponible n° `index` dans « Selling Items ».
//
// Copie EXACTE de ce que fait UIMerchantItemPurchaseWnd_OnMsg msg 38 en mode
// buying store : on ajoute au panier la quantité voulue, puis on retire des
// disponibles le nœud ENTIER (cf. kBsMirrorMode). Ne surtout pas « améliorer »
// en ne consommant que `qty` : la liste des disponibles serait alors désaccordée
// du panier, et un second dépôt du même index produirait un doublon que le
// serveur rejette pour toute la vente.
bool SessionBsAdd(int index, int qty) {
  bool ok = false;
  uint8_t rec[kRecSize];
  std::memset(rec, 0, sizeof(rec));
  __try {
    reinterpret_cast<SessGetAt_t>(kAvailGetAt)(SessionBase(), rec, index);
    const int have = *reinterpret_cast<int*>(rec + kRecQty);
    const int want = (qty > 0 && qty < have) ? qty : have;
    if (want > 0) {
      *reinterpret_cast<int*>(rec + kRecQty) = want;
      reinterpret_cast<SessRec2_t>(kBasketAddOrMerge)(SessionBase(), rec);
      // La quantité du rec sert AUSSI de décrément ici, mais kBsMirrorMode != 0
      // court-circuite ce chemin et supprime le nœud entier.
      reinterpret_cast<SessConsume_t>(kAvailConsume)(SessionBase(), rec,
                                                     kBsMirrorMode, 1);
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  RecDtor(rec);
  return ok;
}

// Rend au stock la ligne n° `index` de « Selling Items ».
//
// Symétrique du dépôt, dans l'ordre du natif (UIMerchantMirrorItemWnd_OnMsg
// msg 38, branche « source = panier de vente ») : on rend d'abord, on retire
// ensuite. Le rec vient du PANIER, donc sa quantité est celle mise en vente —
// c'est bien elle qui revient aux disponibles.
bool SessionBsRemove(int index) {
  bool ok = false;
  uint8_t rec[kRecSize];
  std::memset(rec, 0, sizeof(rec));
  __try {
    reinterpret_cast<SessGetAt_t>(kBasketGetAt)(SessionBase(), rec, index);
    if (*reinterpret_cast<int*>(rec + kRecQty) > 0) {
      reinterpret_cast<SessMerge_t>(kAvailAddOrMerge)(SessionBase(), rec,
                                                      kBsMirrorMode, 1);
      reinterpret_cast<SessRec2_t>(kBasketRemove)(SessionBase(), rec);
      ok = true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
  RecDtor(rec);
  return ok;
}

// Le nom composé (refine + cartes) vient de BuildDisplayName et se cache dans
// g_display_name_cache plus haut ; itemcell::NameById n'est que le REPLI, pour
// une ligne dont la composition a échoué.

// Nom du vendeur pour la barre de titre. Le titre natif passe par un buffer
// GLOBAL partagé (0x00FCE968) réécrit avant chaque dessin : inexploitable ici,
// puisqu'on masque justement les fenêtres qui le remplissent. On repasse donc par
// le dictionnaire de noms, qui met le GID en file de requête serveur s'il est
// encore inconnu — d'où une chaîne vide sur les toutes premières frames.
void VendorName(uint32_t gid, char* out, size_t cap) {
  out[0] = '\0';
  if (!gid || cap < 2) return;
  __try {
    void* game_mode =
        reinterpret_cast<GameModeGetActive_t>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
    if (!game_mode) return;
    void* name_dict = reinterpret_cast<uint8_t*>(game_mode) + gamescene::kGmNameDict;
    void* info =
        reinterpret_cast<NameDictGetEntry_t>(kNameDictGetEntry)(name_dict, gid);
    if (!info) return;
    const char* sbase = reinterpret_cast<const char*>(info) + kNameInfoStr;
    const uint32_t scap = *reinterpret_cast<const uint32_t*>(sbase + 0x14);
    const char* str =
        (scap > 15) ? *reinterpret_cast<const char* const*>(sbase) : sbase;
    if (!str) return;
    std::strncpy(out, str, cap - 1);
    out[cap - 1] = '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
}

// Les DEUX gestes que la cmd 185 native faisait en plus de fermer ses fenêtres, et
// qu'une destruction directe ne déclenche donc jamais (elle ne passe pas par OnMsg) :
// la fin d'interaction marchande sur le bus du mode de jeu, puis le vidage du panier
// de session. Sans le premier, le client se croit encore devant le vendeur.
void EndVendorDeal() {
  __try {
    void* game_mode =
        reinterpret_cast<GameModeGetActive_t>(rag::kModeMgrGetActiveAddr)(static_cast<int>(rag::kModeMgrAddr));
    if (game_mode) {
      void** vt = *reinterpret_cast<void***>(game_mode);
      using SendMsg_t = int(__thiscall*)(void*, int, int, int, int, int);
      reinterpret_cast<SendMsg_t>(vt[6])(game_mode, kCmdEndDeal, 0, 0, 0, 0);
    }
    reinterpret_cast<BasketClear_t>(kVendingBasketClear)(SessionBase());
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Recopie ce qui sert à décrire l'objet. Les cartes/options viennent du nœud, pas
// de la DB : sans elles l'aperçu d'un équipement serti serait faux.
void FillDesc(VendingWindow::DescInfo& out, const RawRow& raw) {
  out.id = raw.id;
  out.index = raw.index;  // ce qui identifie l'EXEMPLAIRE (cf. OpenDescFromList)
  std::strncpy(out.name, raw.name, sizeof(out.name) - 1);
  out.name[sizeof(out.name) - 1] = '\0';
  for (int c = 0; c < 4; ++c) out.cards[c] = raw.cards[c];
  out.refine = raw.refine;
  out.damaged = raw.damaged;
  out.opt_count = raw.opt_count;
  for (int k = 0; k < raw.opt_count && k < kMaxOpts; ++k) {
    out.opts[k].index = raw.opt_index[k];
    out.opts[k].value = raw.opt_value[k];
    out.opts[k].param = raw.opt_param[k];
  }
}

// Ouvre la description COMPLÈTE du client pour l'objet d'index `index` (id `id`)
// de la liste `wnd + list_off`. On repasse le POINTEUR de l'ItemSkillInfo du
// nœud, comme le clic droit natif : c'est ce qui donne cartes, options et refine
// exacts — une description reconstruite depuis l'id seul serait forcément
// appauvrie.
//
// ⚠ On identifie le nœud par le COUPLE (index, id), pas par l'id seul : plusieurs
// exemplaires du même objet coexistent dans une échoppe (même id, refines ou
// cartes différents) et l'id seul rendait toujours le premier — la description
// se figeait sur lui pour toutes leurs lignes. L'id reste dans le critère comme
// garde : si la liste a muté depuis l'extraction, l'index seul pourrait désigner
// un tout autre objet.
//
// Repli sur le premier nœud de même id si le couple ne matche pas : les listes
// natives ne portent pas toutes un index significatif à +0x0c (l'historique de
// ventes, notamment), et mieux vaut la description approchée d'avant que rien.
void OpenDescFromList(void* wnd, int list_off, int index, uint32_t id, int mx,
                      int my) {
  if (!wnd || id == 0) return;
  __try {
    auto* base = reinterpret_cast<uint8_t*>(wnd);
    uint8_t* head = *reinterpret_cast<uint8_t**>(base + list_off);
    if (!head) return;
    uint8_t* found = nullptr;
    uint8_t* first_by_id = nullptr;
    uint8_t* node = *reinterpret_cast<uint8_t**>(head);
    for (int guard = 0; node && node != head && guard < 512; ++guard) {
      const char* sbase = reinterpret_cast<const char*>(node + kNodeName);
      const uint32_t cap = *reinterpret_cast<const uint32_t*>(sbase + 0x14);
      const char* str = (cap > 15) ? *reinterpret_cast<const char* const*>(sbase)
                                   : sbase;
      if (str && static_cast<uint32_t>(atoi(str)) == id) {
        if (!first_by_id) first_by_id = node + 0x08;  // ISI à nœud+0x08
        if (*reinterpret_cast<int*>(node + kNodeIndex) == index) {
          found = node + 0x08;
          break;
        }
      }
      node = *reinterpret_cast<uint8_t**>(node);
    }
    if (!found) found = first_by_id;
    if (!found) return;
    void* mgr = uiwnd::Mgr();
    void* dwnd = uiwnd::MakeWindow(itemdb::kItemDescWndId);
    if (dwnd) {
      uiwnd::OnMsg(dwnd, itemdb::kItemDescMsgSet,
          static_cast<int>(reinterpret_cast<uintptr_t>(found)), 0, 0, 0);
      uiwnd::SetPos(dwnd, mx, my);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Aperçu RO au survol : tooltip fond blanc + cadre sysbox peint derrière via un
// split de canaux. À appeler HORS de toute fenêtre ImGui (il crée son popup).

// Sépare les milliers, comme le client (« 666,666 »).
void FormatZeny(long long v, char* out, size_t cap) {
  char raw[32];
  std::snprintf(raw, sizeof(raw), "%lld", v);
  const int len = static_cast<int>(std::strlen(raw));
  int lead = len % 3 ? len % 3 : 3;
  size_t o = 0;
  for (int i = 0; i < len && o + 2 < cap; ++i) {
    if (i == lead && i != 0) { out[o++] = ','; lead += 3; }
    out[o++] = raw[i];
  }
  out[o] = '\0';
}

}  // namespace

// ── Cycle de vie ─────────────────────────────────────────────────────────────

void VendingWindow::HideNativeAtCreation(void* win) {
  if (!win || !imgui_enabled_) return;
  // L'appelant a déjà filtré sur l'id — il n'y a plus rien à discriminer ici :
  // chacune des familles (composition, « Mon shop », historique, achat chez un
  // vendeur, vente à un buying store) a son panneau ImGui.
  HideWnd(win);
}

void VendingWindow::ItemHover(const DescInfo& desc, void* wnd, int list_off) {
  if (desc.id == 0 || !ImGui::IsItemHovered()) return;
  hover_desc_ = desc;
  hover_valid_ = true;
  // Clic DROIT = description complète, comme partout ailleurs dans le client et
  // dans les autres viewers. Le gauche reste libre pour les actions de la ligne.
  //
  // On MÉMORISE, on n'ouvre pas : l'appel part de FlushPending, hors frame ImGui
  // et une fois le bouton RELÂCHÉ. Ouverte ici, un appui PROLONGÉ faisait sortir
  // la description DERRIÈRE nous — le focus reste acquis à la fenêtre cliquée
  // tant que le bouton est enfoncé, et la remontée du panneau est différée d'une
  // frame (cf. features/item_cell.h, qui détaille la course).
  // ⚠ Ce site ne peut pas utiliser itemcell::DeferDesc* : il ouvre la
  // description depuis un nœud de la liste d'une fenêtre NATIVE (wnd+list_off),
  // pas depuis une liste de session ni un simple id.
  if (mui::IsLastItemRightClicked() && wnd) {
    POINT pt;
    if (GetCursorPos(&pt)) {
      pending_desc_wnd_   = wnd;
      pending_desc_off_   = list_off;
      pending_desc_index_ = desc.index;
      pending_desc_id_    = desc.id;
      pending_desc_x_     = pt.x;
      pending_desc_y_     = pt.y;
    }
    // (Il y avait ici un ImGui::SetWindowFocus(nullptr) censé « défocaliser notre
    // fenêtre pour que la description ne passe pas derrière ». Il ne pouvait pas
    // marcher : FocusWindow(NULL) sort AVANT BringWindowToDisplayFront, donc il ne
    // touche à aucun z-order — il ne faisait que vider le focus clavier et fermer
    // les popups ouverts. La remontée est désormais réclamée par ItemDescWindow
    // lui-même, depuis le hook OnMsg 0x18 que cette ouverture traverse.)
  }
}

void VendingWindow::DrawItemCell(const DescInfo& desc, int slots, void* wnd,
                                 int list_off) {
  ro::IconTex ic = ro::ItemIcon(desc.id);
  if (ic.tex) {
    // Cassé = icône teintée du rouge natif (cf. itemcell::kDamagedShadow).
    // ImageWithBg et non Image : depuis ImGui 1.91.9 c'est elle qui porte le
    // paramètre tint_col.
    const ImVec4 tint = desc.damaged
                            ? ImGui::ColorConvertU32ToFloat4(itemcell::kDamagedShadow)
                            : ImVec4(1, 1, 1, 1);
    ImGui::ImageWithBg(reinterpret_cast<ImTextureID>(ic.tex), ImVec2(20, 20),
                       ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
    ItemHover(desc, wnd, list_off);  // l'icône réagit comme le nom
    ImGui::SameLine();
  }
  // ⚠ BuildDisplayName compose le refine et le préfixe de carte, mais PAS le
  // suffixe d'emplacements (cf. itemdesc::RenderSimpleDesc, qui l'ajoute de son
  // côté pour la même raison). On l'ajoute donc dans les deux cas.
  const char* label = desc.name[0] != '\0' ? desc.name : itemcell::NameById(desc.id);
  char lbl[96];
  if (slots > 0) {
    std::snprintf(lbl, sizeof(lbl), "%s [%d]", label, slots);
    label = lbl;
  }
  itemcell::NameText(label, desc.damaged != 0);
  ItemHover(desc, wnd, list_off);
}

bool VendingWindow::IsComposing() const {
  // Sans état exprès : la fenêtre native fait foi, qu'on la remplace ou non.
  return FindWnd(kWinVending) != nullptr || FindWnd(kWinBuyingStore) != nullptr;
}

void VendingWindow::OnTick() {
  // (Le décodage des paquets ne se fait plus ici : Bourgeon draine la file de tous
  // les modules à chaque frame, cf. Bourgeon::DrainNetInboxes. Au tick, l'ouverture
  // d'une échoppe pouvait attendre 100 ms.)

  if (!imgui_enabled_) {
    wnd_ = nullptr;
    mirror_ = nullptr;
    myshop_wnd_ = nullptr;
    open_ = false;
    myshop_open_ = false;
    vendor_open_ = false;
    was_open_ = false;
    return;
  }

  // « Ma boutique » : cycle de vie INDÉPENDANT de la composition (elle s'ouvre
  // quand celle-ci est déjà fermée), donc traitée avant tout retour anticipé.
  myshop_wnd_ = FindWnd(kWinMyShop);
  if (!myshop_wnd_) myshop_wnd_ = FindWnd(kWinMyShopBuying);
  if (myshop_wnd_) {
    if (!myshop_open_) {  // front montant
      myshop_panel_ = true;
      myshop_earned_ = 0;  // nouvelle échoppe : le compteur repart de zéro
    }
    myshop_open_ = true;
    HideWnd(myshop_wnd_);
  } else {
    myshop_open_ = false;
    myshop_.clear();
  }

  // Historique des ventes. ⚠ La fenêtre native existe DÈS LA PREMIÈRE VENTE :
  // c'est ELLE qui accumule les lignes (son OnMsg 23 AJOUTE une ligne, il ne
  // reconstruit rien depuis une liste de session). Le close du shop se contente
  // de la faire remonter — `MakeWindow(0x101)` rend l'existante.
  //
  // On n'affiche donc notre panneau qu'une fois le shop TERMINÉ, comme le natif :
  // le montrer dès la première vente le ferait surgir en pleine activité, et sa
  // croix (cmd 201) DÉTRUIRAIT l'accumulateur — donc tout l'historique.
  log_wnd_ = FindWnd(kWinSellLog);
  log_buying_ = false;
  if (!log_wnd_) {
    log_wnd_ = FindWnd(kWinSellLogBuying);
    log_buying_ = (log_wnd_ != nullptr);
  }
  if (log_wnd_) {
    log_open_ = true;
    if (!myshop_open_ && !log_shown_) {  // front montant « shop terminé »
      log_panel_ = true;
      log_shown_ = true;
    }
    HideWnd(log_wnd_);
  } else {
    log_open_ = false;
    log_shown_ = false;
    log_.clear();
  }

  // Côté acheteur : 0x2B (offre) + 0x2C (panier). 🔴 Elles ne portent PLUS l'état —
  // c'est le paquet d'ouverture qui l'ouvre (HandlePacket) et notre fermeture qui le
  // referme. On se contente de les détruire : masquées, elles resteraient vivantes,
  // et le natif les recréerait sans repasser par nous.
  if (void* w = FindWnd(kWinVendorShop))   { HideWnd(w); CloseWnd(kWinVendorShop); }
  if (void* w = FindWnd(kWinVendorBasket)) { HideWnd(w); CloseWnd(kWinVendorBasket); }

  // Côté vendeur face à un buying store : TROIS fenêtres (0xB1 recherche, 0xB2
  // vente, 0xB3 stock proposable), ouvertes ensemble par un clic sur l'échoppe
  // d'achat d'un autre joueur.
  bs_wanted_ = FindWnd(kWinBsWanted);
  bs_sell_   = FindWnd(kWinBsSellList);
  bs_mirror_ = FindWnd(kWinBsMirror);
  if (bs_wanted_ && bs_sell_ && bs_mirror_) {
    if (!bs_open_) {  // front montant : nouvelle session de vente
      bs_panel_ = true;
      bs_buyer_[0] = '\0';
      bs_qty_.clear();
    }
    bs_open_ = true;
    HideWnd(bs_wanted_);
    HideWnd(bs_sell_);
    HideWnd(bs_mirror_);
  } else {
    bs_open_ = false;
    bs_wanted_ = nullptr;
    bs_sell_ = nullptr;
    bs_mirror_ = nullptr;
    bs_wanted_rows_.clear();
    bs_avail_.clear();
    bs_sell_rows_.clear();
    bs_qty_.clear();
  }

  // La fenêtre de composition existe -> l'échoppe est en cours de montage. Le
  // client DÉTRUIT ses fenêtres à la fermeture, donc un pointeur non nul veut
  // bien dire « ouverte maintenant ».
  void* vending = FindWnd(kWinVending);
  void* buying  = vending ? nullptr : FindWnd(kWinBuyingStore);
  wnd_    = vending ? vending : buying;
  buying_ = (vending == nullptr) && (buying != nullptr);
  open_   = (wnd_ != nullptr);
  mirror_ = open_ ? FindWnd(buying_ ? kWinBuyingMirror : kWinVendingMirror)
                  : nullptr;

  if (!open_) {
    was_open_ = false;
    import_used_ = false;  // nouveau montage -> l'import redevient offert
    rows_.clear();
    avail_.clear();
    avail_qty_.clear();
    return;
  }

  // Le natif peut remettre son flag de visibilité à 1 (relayout, msg 23) : on
  // recache les DEUX à chaque tick, comme les autres remplacements complets.
  HideWnd(wnd_);
  HideWnd(mirror_);

  slots_ = ReadInt(wnd_, kOffSlots);
  if (slots_ < 0) slots_ = 0;
  if (slots_ > kMaxRows) slots_ = kMaxRows;
  count_ = ReadInt(wnd_, kOffCount);

  // Front montant : nouveau montage d'échoppe -> formulaire vierge.
  if (!was_open_) {
    was_open_ = true;
    show_panel_ = true;
    for (int i = 0; i < kMaxRows; ++i) { prices_[i] = 0; amounts_[i] = 0; }
    zeny_limit_ = 0;
    // Le nom, lui, est CONSERVÉ d'une échoppe à l'autre : c'est ce que fait le
    // client avec son snapshot (g_VendingSnapshot_ShopName), et le joueur qui
    // rouvre boutique veut presque toujours le même intitulé.
  }
}

void VendingWindow::Refresh() {
  // Objets POSÉS dans l'échoppe (fenêtre de composition).
  RawRow raw[kMaxRows];
  const int n = wnd_ ? ReadRows(wnd_, kOffList, raw, kMaxRows) : 0;
  rows_.clear();
  rows_.reserve(n);
  for (int i = 0; i < n; ++i) {
    Row r;
    r.id     = raw[i].id;
    r.index  = raw[i].index;
    r.amount = raw[i].amount;
    r.slots  = raw[i].slots;
    FillDesc(r.desc, raw[i]);
    rows_.push_back(r);
  }

  // Objets DISPONIBLES (grille native cachée). La liste peut être plus longue que
  // le nombre d'emplacements : c'est le stock, pas l'échoppe.
  RawRow avail_raw[kMaxAvail];
  const int m =
      mirror_ ? ReadRows(mirror_, kOffMirrorList, avail_raw, kMaxAvail) : 0;
  avail_.clear();
  avail_.reserve(m);
  for (int i = 0; i < m; ++i) {
    Row r;
    r.id     = avail_raw[i].id;
    r.index  = avail_raw[i].index;
    r.amount = avail_raw[i].amount;
    r.slots  = avail_raw[i].slots;
    FillDesc(r.desc, avail_raw[i]);
    avail_.push_back(r);
  }
  // Les quantités à poser suivent la liste : elle est reconstruite à chaque
  // mutation, donc on repart du lot complet plutôt que de conserver une saisie
  // qui pointerait vers un autre objet.
  if (avail_qty_.size() != avail_.size()) {
    avail_qty_.assign(avail_.size(), 0);
    for (size_t i = 0; i < avail_.size(); ++i) avail_qty_[i] = avail_[i].amount;
  }
}

void VendingWindow::RefreshMyShop() {
  RawRow raw[kMaxAvail];
  const int n =
      myshop_wnd_ ? ReadRows(myshop_wnd_, kOffMyShopList, raw, kMaxAvail) : 0;
  myshop_.clear();
  myshop_.reserve(n);
  for (int i = 0; i < n; ++i) {
    Row r;
    r.id     = raw[i].id;
    r.index  = raw[i].index;
    r.amount = raw[i].amount;
    r.slots  = raw[i].slots;
    r.price  = raw[i].price;
    FillDesc(r.desc, raw[i]);
    myshop_.push_back(r);
  }
  myshop_zeny_ = myshop_wnd_ ? ReadInt(myshop_wnd_, kOffMyShopLastSale) : 0;
  myshop_buying_ =
      myshop_wnd_ && ReadInt(myshop_wnd_, kOffMyShopMode) != 0;
}

void VendingWindow::RefreshSellLog() {
  RawRow raw[kMaxAvail];
  const int n =
      log_wnd_ ? ReadRows(log_wnd_, kOffSellLogList, raw, kMaxAvail) : 0;
  log_.clear();
  log_.reserve(n);
  for (int i = 0; i < n; ++i) {
    Row r;
    r.id     = raw[i].id;
    r.index  = raw[i].index;
    r.amount = raw[i].amount;
    r.slots  = raw[i].slots;
    r.price  = raw[i].price;
    FillDesc(r.desc, raw[i]);
    log_.push_back(r);
  }
}

void VendingWindow::RefreshVendorShop() {
  RawRow raw[kMaxAvail];
  // Source = la liste de la SESSION, plus la fenêtre native (détruite). Même
  // structure de nœuds, mêmes offsets, et les DEUX prix y sont déjà (cf. la note
  // sur kOffSessOfferList).
  const int n = ReadRows(SessionBase(), kOffSessOfferList, raw, kMaxAvail);
  offers_.clear();
  offers_.reserve(n);
  for (int i = 0; i < n; ++i) {
    BuyRow r;
    r.id         = raw[i].id;
    r.index      = raw[i].index;
    r.stock      = raw[i].amount;
    r.slots      = raw[i].slots;
    // Le natif paie et totalise sur le prix EFFECTIF ; le prix de base ne sert
    // qu'à afficher la remise. Un fallback si +0x20 est nul (pas de remise
    // calculée) évite d'annoncer « gratuit ».
    r.base_price = raw[i].price;
    r.price      = raw[i].price_eff ? raw[i].price_eff : raw[i].price;
    FillDesc(r.desc, raw[i]);
    offers_.push_back(r);
  }
  offer_qty_.resize(offers_.size(), 1);

  // (GID/UniqueID ne sont plus lus ici : ils viennent du paquet d'ouverture, à la
  // source — c'est HandlePacket qui les pose. Le panier natif qui les portait
  // n'existe plus.)
  // Le dictionnaire répond vide tant que le serveur n'a pas renvoyé le nom : on
  // ne réinterroge que tant qu'on n'a rien, sans écraser un nom déjà obtenu.
  if (vendor_name_[0] == '\0')
    VendorName(vendor_gid_, vendor_name_, sizeof(vendor_name_));
}

// ── Vente à un buying store ──────────────────────────────────────────────────

void VendingWindow::RefreshBuyingStoreSell() {
  RawRow raw[kMaxAvail];

  // Ce que l'acheteur recherche (0xB1). `amount` y est la quantité qu'il veut
  // ENCORE, déjà décrémentée de ce qu'il a acheté — pas la quantité d'origine.
  int n = bs_wanted_ ? ReadRows(bs_wanted_, kOffVendorList, raw, kMaxAvail) : 0;
  bs_wanted_rows_.clear();
  bs_wanted_rows_.reserve(n);
  for (int i = 0; i < n; ++i) {
    BuyRow r;
    r.id         = raw[i].id;
    r.index      = raw[i].index;
    r.stock      = raw[i].amount;
    r.slots      = raw[i].slots;
    r.base_price = raw[i].price;
    r.price      = raw[i].price_eff ? raw[i].price_eff : raw[i].price;
    FillDesc(r.desc, raw[i]);
    bs_wanted_rows_.push_back(r);
  }

  // Mon stock proposable (0xB3). Le natif l'a déjà filtré sur les objets
  // recherchés — rien à retrier ici.
  n = bs_mirror_ ? ReadRows(bs_mirror_, kOffMirrorList, raw, kMaxAvail) : 0;
  bs_avail_.clear();
  bs_avail_.reserve(n);
  for (int i = 0; i < n; ++i) {
    Row r;
    r.id     = raw[i].id;
    r.index  = raw[i].index;
    r.amount = raw[i].amount;
    r.slots  = raw[i].slots;
    FillDesc(r.desc, raw[i]);
    bs_avail_.push_back(r);
  }

  // Le panier natif (0xB2). On le LIT au lieu d'en tenir un à nous : c'est lui
  // que le bouton « sell » sérialise, donc l'afficher tel quel est la seule
  // façon de garantir que l'écran dit ce qui partira.
  n = bs_sell_ ? ReadRows(bs_sell_, kOffVendorList, raw, kMaxAvail) : 0;
  bs_sell_rows_.clear();
  bs_sell_rows_.reserve(n);
  for (int i = 0; i < n; ++i) {
    Row r;
    r.id     = raw[i].id;
    r.index  = raw[i].index;
    r.amount = raw[i].amount;
    r.slots  = raw[i].slots;
    r.price  = raw[i].price_eff ? raw[i].price_eff : raw[i].price;
    FillDesc(r.desc, raw[i]);
    bs_sell_rows_.push_back(r);
  }

  // Quantités saisies : la liste des disponibles est reconstruite à chaque
  // mutation, on repart donc du lot complet plutôt que de garder une saisie qui
  // désignerait un autre objet (même raison qu'en composition).
  if (bs_qty_.size() != bs_avail_.size()) {
    bs_qty_.assign(bs_avail_.size(), 0);
    for (size_t i = 0; i < bs_avail_.size(); ++i)
      bs_qty_[i] = BsSellableQty(static_cast<int>(i));
  }

  bs_zeny_left_ = bs_wanted_ ? ReadInt(bs_wanted_, kOffBsZenyLeft) : 0;
  if (bs_buyer_[0] == '\0' && bs_wanted_) {
    const uint32_t aid =
        static_cast<uint32_t>(ReadInt(bs_wanted_, kOffBsBuyerAid));
    VendorName(aid, bs_buyer_, sizeof(bs_buyer_));
  }
}

// Ce que l'acheteur veut ENCORE de cet objet, borné par ce que j'en ai et par le
// plafond natif. Reproduit le clamp de UIMerchantItemPurchaseWnd_OnMsg msg 38 :
// sans lui on proposerait des quantités que le natif rognerait en silence.
int VendingWindow::BsSellableQty(int avail_index) const {
  if (avail_index < 0 || avail_index >= static_cast<int>(bs_avail_.size()))
    return 0;
  const Row& a = bs_avail_[avail_index];
  int wanted = 0;
  for (const BuyRow& w : bs_wanted_rows_)
    if (w.id == a.id) { wanted = w.stock; break; }
  // Déjà en vente pour le même objet : ça vient en déduction de ce qu'il veut.
  for (const Row& s : bs_sell_rows_)
    if (s.id == a.id) wanted -= s.amount;

  int qty = a.amount;
  if (wanted < qty) qty = wanted;
  if (qty > kBsMaxAmount) qty = kBsMaxAmount;
  return qty > 0 ? qty : 0;
}

// Prix unitaire proposé par l'acheteur pour cet objet, 0 s'il n'en veut pas.
int VendingWindow::BsPriceOf(uint32_t item_id) const {
  for (const BuyRow& w : bs_wanted_rows_)
    if (w.id == item_id) return w.price;
  return 0;
}

void VendingWindow::BsAddToSellList(int avail_index, int qty) {
  if (avail_index < 0 || avail_index >= static_cast<int>(bs_avail_.size()))
    return;
  if (!SessionBsAdd(avail_index, qty)) return;
  // Les trois fenêtres recopient les listes de session : sans ce renvoi, nos
  // lectures POD montreraient encore l'état d'avant.
  if (bs_sell_) SendRebuild(bs_sell_);
  if (bs_mirror_) SendRebuild(bs_mirror_);
  bs_qty_.clear();  // force le recalage des quantités au prochain Refresh
  RefreshBuyingStoreSell();
}

void VendingWindow::BsRemoveFromSellList(int sell_index) {
  if (sell_index < 0 || sell_index >= static_cast<int>(bs_sell_rows_.size()))
    return;
  if (!SessionBsRemove(sell_index)) return;
  if (bs_sell_) SendRebuild(bs_sell_);
  if (bs_mirror_) SendRebuild(bs_mirror_);
  bs_qty_.clear();
  RefreshBuyingStoreSell();
}

void VendingWindow::SendPurchase(const BasketLine* lines, int count) {
  if (!lines || count <= 0 || vendor_gid_ == 0) return;
  const int len = kPurchaseHdr + 4 * count;
  if (len > kPurchaseMaxLen) return;  // même refus que le constructeur natif

  std::vector<uint8_t> pkt(static_cast<size_t>(len), 0);
  uint8_t* p = pkt.data();
  std::memcpy(p + 0, &kCzPurchaseFromMc, 2);
  const uint16_t len16 = static_cast<uint16_t>(len);
  std::memcpy(p + 2, &len16, 2);
  std::memcpy(p + 4, &vendor_gid_, 4);
  std::memcpy(p + 8, &vendor_uid_, 4);
  for (int i = 0; i < count; ++i) {
    const uint16_t amount = static_cast<uint16_t>(lines[i].amount);
    const uint16_t index  = static_cast<uint16_t>(lines[i].index);
    std::memcpy(p + kPurchaseHdr + 4 * i,     &amount, 2);
    std::memcpy(p + kPurchaseHdr + 4 * i + 2, &index,  2);
  }
  Bourgeon::Instance().SendPacket(p, static_cast<size_t>(len));

  // TCP conserve l'ordre : le serveur traite l'achat AVANT cette requête, donc la
  // liste qu'il renvoie est déjà celle d'après. C'est ce qui rafraîchit la fenêtre
  // (le natif, lui, la fermait au lieu de la rafraîchir).
  bought_once_ = true;
  RequestVendorList();
}

void VendingWindow::RequestVendorList() {
  if (vendor_gid_ == 0) return;
  uint8_t pkt[6];
  std::memcpy(pkt + 0, &kCzVendingListReq, 2);
  std::memcpy(pkt + 2, &vendor_gid_, 4);
  Bourgeon::Instance().SendPacket(pkt, sizeof(pkt));
}

void VendingWindow::SendVendingBuy() {
  if (basket_.empty()) return;
  SendPurchase(basket_.data(), static_cast<int>(basket_.size()));
  basket_.clear();
}

void VendingWindow::QuickBuy(const BuyRow& offer, int qty) {
  if (qty <= 0) return;
  BasketLine line;
  line.id = offer.id;
  line.index = offer.index;
  line.amount = qty;
  line.price = offer.price;
  line.max = offer.stock;
  SendPurchase(&line, 1);
}

void VendingWindow::FireCommand(int cmd) {
  if (wnd_) SendButton(wnd_, cmd);
}

VendingWindow::VendingWindow() {
  // OBSERVÉ et non remplacé : le handler natif doit continuer de tourner, c'est lui
  // qui remplit la liste d'offres de la SESSION — celle qu'on lit maintenant à la
  // place de la fenêtre. On ne fait qu'écouter pour relever AID et UniqueID.
  Bourgeon::Instance().RegisterObserveOpcode(kZcVendingList, kVlMinLen);
  // Rapport de vente : c'est LUI qui porte le zeny encaissé, vente par vente.
  Bourgeon::Instance().RegisterObserveOpcode(kZcVendingReport, kVrMinLen);
}

// Fil RÉSEAU : on COPIE, rien de plus (cf. features/net_inbox.h).
void VendingWindow::OnRecvPacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  net_inbox_.Push(opcode, data, len);
}

// Fil PRINCIPAL : le décodage, rejoué au tick.
void VendingWindow::HandlePacket(uint16_t opcode, const uint8_t* data, uint16_t len) {
  // Une vente vient d'avoir lieu : on cumule son montant. Le compteur est remis à
  // zéro à l'ouverture d'une échoppe (front montant de `myshop_open_`).
  if (opcode == kZcVendingReport) {
    if (len < kVrMinLen) return;
    myshop_earned_ += *reinterpret_cast<const int32_t*>(data + kVrZeny);
    return;
  }
  if (opcode != kZcVendingList || len < kVlMinLen) return;
  vendor_gid_ = *reinterpret_cast<const uint32_t*>(data + kVlAid);
  vendor_uid_ = *reinterpret_cast<const uint32_t*>(data + kVlVender);
  if (!vendor_open_) {  // front montant : nouvelle session d'achat
    vendor_panel_ = true;
    bought_once_ = false;
    vendor_name_[0] = '\0';
    basket_.clear();
    offer_qty_.clear();
  }
  vendor_open_ = true;
}

// Fermeture d'une session d'achat. Le natif passait par la cmd 185 de sa fenêtre de
// panier ; elle ne se contentait PAS de fermer, et c'est tout l'enjeu ici.
void VendingWindow::CloseVendorSession() {
  // Filet : si une native avait survécu à un tick manqué, la détruire d'abord.
  if (FindWnd(kWinVendorShop))   CloseWnd(kWinVendorShop);
  if (FindWnd(kWinVendorBasket)) CloseWnd(kWinVendorBasket);
  EndVendorDeal();
  vendor_open_ = false;
  vendor_gid_ = 0;
  vendor_uid_ = 0;
  offers_.clear();
  basket_.clear();
  offer_qty_.clear();
}

void VendingWindow::QueueCommand(int win_id, int cmd) {
  if (pending_count_ >= kMaxPending) return;  // saturée : le clic est perdu,
                                              // jamais un débordement
  pending_[pending_count_].win_id = win_id;
  pending_[pending_count_].cmd = cmd;
  ++pending_count_;
}

// Rejoue les commandes empilées pendant le rendu. Appelée depuis la phase
// d'input du jeu — donc hors frame ImGui, ce qui est TOUT l'intérêt : une
// commande native peut ouvrir une modale bloquante qui relance le rendu, et la
// déclencher entre NewFrame() et Render() fige le client (cf. l'en-tête).
void VendingWindow::FlushPending() {
  // ── Description : hors frame ImGui, ET bouton RELÂCHÉ ──────────────────────
  // Le « relâché » est la clé : tant qu'un bouton est enfoncé, le focus reste
  // acquis à la fenêtre cliquée, qui repasserait devant la description remontée
  // à la frame suivante — d'où le symptôme « clic bref devant, appui prolongé
  // derrière ». ⚠ Le pointeur de fenêtre est REVALIDÉ (IsLiveShopWnd) : entre le
  // clic et le relâchement, l'échoppe a pu être fermée et la fenêtre détruite.
  if (pending_desc_id_ != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
    void* const   dwnd  = pending_desc_wnd_;
    const int     doff  = pending_desc_off_;
    const int     didx  = pending_desc_index_;
    const uint32_t did  = pending_desc_id_;
    const int     dx    = pending_desc_x_;
    const int     dy    = pending_desc_y_;
    pending_desc_wnd_ = nullptr;
    pending_desc_id_  = 0;
    if (IsLiveShopWnd(dwnd)) {
      OpenDescFromList(dwnd, doff, didx, did, dx, dy);
      // La fenêtre de description est reprise en ImGui par ItemDescWindow, qui
      // masque la native au passage. On le redemande explicitement : notre appel
      // à MakeWindow/OnMsg ne suit pas forcément le chemin qui déclenche son
      // hook, et une native laissée visible se dessine SOUS l'overlay — donc
      // derrière cette fenêtre.
      if (auto* desc_window = Bourgeon::Instance().item_desc())
        desc_window->HideNativeDescWindows();
    }
  }

  if (pending_submit_) {
    pending_submit_ = false;
    // Re-résolution : la fenêtre a pu être détruite depuis le clic.
    wnd_ = FindWnd(buying_ ? kWinBuyingStore : kWinVending);
    if (wnd_) SubmitToNative();
  }
  const int count = pending_count_;
  pending_count_ = 0;  // vidée AVANT exécution : une commande qui rouvrirait une
                       // fenêtre ne doit pas rejouer la file.
  for (int i = 0; i < count; ++i) {
    void* win = FindWnd(pending_[i].win_id);
    if (win) SendButton(win, pending_[i].cmd);
  }
}

void VendingWindow::RefreshNativeLists() {
  // Les deux fenêtres reconstruisent leur liste d'affichage depuis la session ;
  // sans ça nos lectures POD montreraient l'état d'avant la mutation.
  if (wnd_) SendRebuild(wnd_);
  if (mirror_) SendRebuild(mirror_);
  avail_qty_.clear();  // force le recalage des quantités au prochain Refresh
  Refresh();
}

void VendingWindow::PlaceItem(int avail_index, int qty) {
  if (avail_index < 0 || avail_index >= static_cast<int>(avail_.size())) return;
  if (static_cast<int>(rows_.size()) >= slots_) return;  // plus d'emplacement
  SessionPlace(avail_index, qty, buying_);
  RefreshNativeLists();
}

void VendingWindow::TakeBackItem(int row) {
  if (row < 0 || row >= static_cast<int>(rows_.size())) return;
  if (!SessionTakeBack(row, buying_)) return;
  // Nos prix/quantités sont indexés par ligne : on décale comme le natif décale
  // ses edits, sinon le prix de l'objet retiré resterait collé au suivant.
  for (int i = row; i + 1 < kMaxRows; ++i) {
    prices_[i]  = prices_[i + 1];
    amounts_[i] = amounts_[i + 1];
  }
  prices_[kMaxRows - 1] = 0;
  amounts_[kMaxRows - 1] = 0;
  RefreshNativeLists();
}

void VendingWindow::SubmitToNative() {
  if (!wnd_) return;

  // Nom de la boutique. Le natif refuse un nom vide (MsgString 0xE1) et filtre les
  // mots interdits — on le laisse faire, on ne pré-valide rien ici.
  SetEditText(wnd_, kOffNameEdit, name_);

  const int rows = static_cast<int>(rows_.size());
  char buf[32];
  for (int i = 0; i < slots_; ++i) {
    const int price = (i < rows) ? prices_[i] : 0;
    std::snprintf(buf, sizeof(buf), "%d", price);
    SetEditText(wnd_, kOffPrice0 + 4 * i, buf);

    if (buying_) {
      const int qty = (i < rows) ? amounts_[i] : 0;
      std::snprintf(buf, sizeof(buf), "%d", qty);
      SetEditText(wnd_, kOffQty0 + 4 * i, buf);
    }
  }
  if (buying_) {
    std::snprintf(buf, sizeof(buf), "%d", zeny_limit_);
    SetEditText(wnd_, kOffZenyLimit, buf);
  }

  // Clic sur OK natif : toute la validation, la taxe, les confirmations « safe
  // check » et l'envoi (CMode::SendMsg cmd 82 / 271, ou 296 / 298 si le serveur a
  // activé le mode « poser sur une case ») partent de là.
  FireCommand(kCmdOk);
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void VendingWindow::OnRenderUI() {
  if (!imgui_enabled_) return;
  // Le survol est réévalué à chaque frame ; l'aperçu lui-même est dessiné TOUT à
  // la fin, hors de toute fenêtre (un tooltip crée son propre popup).
  hover_valid_ = false;

  // ── « Ma boutique » ────────────────────────────────────────────────────────
  // Rendue AVANT le retour anticipé sur `open_` : elle vit sa propre vie, une
  // fois l'échoppe lancée et les fenêtres de composition détruites.
  if (myshop_open_) {
    myshop_wnd_ = FindWnd(kWinMyShop);  // re-résolution par frame (cf. plus bas)
    if (!myshop_wnd_) myshop_wnd_ = FindWnd(kWinMyShopBuying);
    if (!myshop_wnd_) {
      myshop_open_ = false;
      myshop_.clear();
    } else if (myshop_panel_) {
      RefreshMyShop();
      ro::SetNextWindowBodyColor(ro::ListBodyColorU32());
      // Même garde que l'historique : « Tout est vendu. » est plus court que le
      // titre, et l'autoresize seul rognerait la barre de titre.
      ImGui::SetNextWindowSizeConstraints(ImVec2(ro::Px(kMinPanelW), 0.0f),
                                          ImVec2(ro::Px(kMaxPanelW), FLT_MAX));
      // La croix de la barre de titre fait ce que fait la croix native : elle MET
      // FIN à la boutique. La faire simplement masquer laissait l'échoppe tourner
      // sans plus aucune UI pour la reprendre — un cul-de-sac.
      bool keep_open = true;
      if (ro::BeginRoWindow(i18n::Tr("Mon shop###bourgeon_vending_shop"), &keep_open,
                            ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoCollapse)) {
        if (myshop_.empty()) {
          ImGui::TextDisabled(i18n::Tr("Tout est vendu."));
        } else if (ImGui::BeginTable("##t_myshop", 4,
                                     ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_RowBg)) {
          // Colonne « Objet » SANS largeur : sous SizingFixedFit, une colonne
          // WidthFixed dont la largeur initiale est nulle s'ajuste au contenu.
          // Les noms composés (refine, cartes, emplacements) sont de longueur
          // très variable ; toute valeur en dur rognait les plus longs.
          ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableSetupColumn(i18n::Tr("Reste"), ImGuiTableColumnFlags_WidthFixed, ro::Px(45.0f));
          ImGui::TableSetupColumn(i18n::Tr("Prix"), ImGuiTableColumnFlags_WidthFixed, ro::Px(90.0f));
          ImGui::TableSetupColumn(i18n::Tr("Total"), ImGuiTableColumnFlags_WidthFixed, ro::Px(90.0f));
          ImGui::TableHeadersRow();
          for (size_t i = 0; i < myshop_.size(); ++i) {
            const Row& r = myshop_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawItemCell(r.desc, r.slots, myshop_wnd_, kOffMyShopList);

            ImGui::TableNextColumn();
            ImGui::Text("%d", r.amount);

            char cell[32];
            ImGui::TableNextColumn();
            FormatZeny(r.price, cell, sizeof(cell));
            ImGui::TextUnformatted(cell);

            ImGui::TableNextColumn();
            FormatZeny(static_cast<long long>(r.price) * r.amount, cell,
                       sizeof(cell));
            ImGui::TextUnformatted(cell);
          }
          ImGui::EndTable();
        }
        ImGui::Separator();
        char zbuf[32];
        // Deux sens, deux SOURCES. En échoppe d'achat, +0xF0 est bien ce qu'il
        // reste à dépenser — une valeur d'état, lue chez le natif. En VENTE, il ne
        // porte que la DERNIÈRE vente : le cumul vient de l'historique, seul
        // endroit où le client l'accumule.
        FormatZeny(myshop_buying_ ? myshop_zeny_ : myshop_earned_, zbuf,
                   sizeof(zbuf));
        ImGui::Text(myshop_buying_ ? i18n::Tr("Fonds disponibles : %s z") : i18n::Tr("Encaissé : %s z"), zbuf);

        // ⛔ PAS de case « Notify when item sells out » ici, et ce n'est pas un
        // oubli : le natif l'a lui-même RETIRÉE des deux modes (0x0095A9A0,
        // appelée juste après MakeWindow 0x2D comme 0xB0, fait SetVisible(case
        // +0x104, false) et remet g_MyShopNotifySellOut à 0). Son libellé mentait
        // de surcroît — les deux handlers de rapport (0x00C9D710 vente,
        // 0x00C9DC60 achat) ne font que jouer effect\ef_steal.wav à CHAQUE
        // transaction, épuisé ou non, et aux coordonnées monde 0,0,0 plutôt qu'à
        // la position du joueur. La ressusciter n'apportait qu'une promesse non
        // tenue. Cf. project_vending_window_re.
        const int myshop_id = myshop_buying_ ? kWinMyShopBuying : kWinMyShop;

        // ⚠ Le « close » natif ne ferme pas que la fenêtre : il dispatche
        // CMode::SendMsg 81 (vente) / 270 (achat), c'est-à-dire qu'il MET FIN à la
        // boutique. Le libellé le dit, et la confirmation évite le clic malheureux
        // — que le clic vienne du bouton ou de la croix.
        const bool close_clicked = ro::RoButton(i18n::Tr("Fermer le shop"));
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(i18n::Tr("Met fin à la vente et récupère les objets invendus."));
        if (close_clicked || !keep_open) ImGui::OpenPopup(i18n::Tr("Fermer ?###bourgeon_vending_close"));
        if (ro::BeginRoPopupModal(i18n::Tr("Fermer ?###bourgeon_vending_close"))) {
          ImGui::TextUnformatted(i18n::Tr("Mettre fin au shop ?"));
          ImGui::Separator();
          if (ro::RoButton(i18n::Tr("Oui"))) {
            QueueCommand(myshop_id, kCmdMyShopClose);
            ImGui::CloseCurrentPopup();
          }
          ImGui::SameLine();
          if (ro::RoButton(i18n::Tr("Non"))) ImGui::CloseCurrentPopup();
          ro::EndRoPopupModal();
        }
      }
      ro::EndRoWindow();
    }
  }

  // ── Historique des ventes ──────────────────────────────────────────────────
  // `!myshop_open_` : tant que le shop tourne, la fenêtre native n'est qu'un
  // accumulateur invisible (cf. OnTick). On ne la montre qu'à la fin.
  if (log_open_ && !myshop_open_) {
    log_wnd_ = FindWnd(kWinSellLog);
    if (!log_wnd_) log_wnd_ = FindWnd(kWinSellLogBuying);  // log_buying_ : OnTick
    if (!log_wnd_) {
      log_open_ = false;
      log_.clear();
    } else if (log_panel_) {
      RefreshSellLog();
      ro::SetNextWindowBodyColor(ro::ListBodyColorU32());
      // AlwaysAutoResize dimensionne sur le CONTENU seul : sur une liste vide
      // (« Aucune vente. ») la fenêtre devient plus étroite que son propre titre,
      // qui se retrouve rogné. La contrainte de largeur minimale règle le cas.
      ImGui::SetNextWindowSizeConstraints(ImVec2(ro::Px(kMinPanelW), 0.0f),
                                          ImVec2(ro::Px(kMaxPanelW), FLT_MAX));
      // Croix = vraie fermeture. Sans danger ICI seulement parce qu'on n'affiche
      // ce panneau qu'une fois le shop fini : la cmd 201 passe par
      // UIWindowMgr_SaveRectAndCloseWindow, qui DÉTRUIT la fenêtre native — et
      // avec elle l'historique accumulé. La déclencher en pleine vente vidait
      // l'historique sans un mot.
      bool keep_log = true;
      bool close_log = false;
      // Une échoppe d'ACHAT accumule dans la même classe (0x102 au lieu de
      // 0x101) : les lignes y sont des achats, pas des ventes. Le libellé suit,
      // sinon le récapitulatif ment sur le sens de l'opération.
      if (ro::BeginRoWindow(log_buying_ ? "Historique des achats###vending_log"
                                        : "Historique des ventes###vending_log",
                            &keep_log,
                            ImGuiWindowFlags_AlwaysAutoResize |
                            ImGuiWindowFlags_NoCollapse)) {
        if (log_.empty()) {
          ImGui::TextDisabled(log_buying_ ? i18n::Tr("Aucun achat.") : i18n::Tr("Aucune vente."));
        } else if (ImGui::BeginTable("##t_log", 4,
                                     ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthFixed);
          ImGui::TableSetupColumn(i18n::Tr("Qté"), ImGuiTableColumnFlags_WidthFixed, ro::Px(45.0f));
          ImGui::TableSetupColumn(i18n::Tr("Prix"), ImGuiTableColumnFlags_WidthFixed, ro::Px(90.0f));
          ImGui::TableSetupColumn(i18n::Tr("Montant"), ImGuiTableColumnFlags_WidthFixed, ro::Px(90.0f));
          ImGui::TableHeadersRow();
          long long total = 0;
          for (size_t i = 0; i < log_.size(); ++i) {
            const Row& r = log_[i];
            // ⚠ Dans CETTE fenêtre, nœud+0x1C n'est PAS un prix unitaire mais le
            // MONTANT de la ligne. UIMerchantItemLogWnd_DrawContent l'affiche tel
            // quel (« %10s Zeny ») sans jamais le multiplier par la quantité — et
            // c'est bien ce qu'on observe : 50 golds à 100 z y valent 5 000.
            // Le remultiplier gonflait chaque ligne et le total (250 600 z pour
            // 5 400 z réels), au point de dépasser le budget alloué au shop.
            const long long amount = r.price;
            total += amount;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawItemCell(r.desc, r.slots, log_wnd_, kOffSellLogList);

            ImGui::TableNextColumn();
            ImGui::Text("%d", r.amount);

            char cell[32];
            // Le prix unitaire, lui, n'existe nulle part : on le déduit. Le natif
            // ne l'affiche pas du tout, mais c'est l'information utile quand on
            // relit son historique.
            ImGui::TableNextColumn();
            if (r.amount > 0) {
              FormatZeny(amount / r.amount, cell, sizeof(cell));
              ImGui::TextUnformatted(cell);
            } else {
              ImGui::TextDisabled("-");
            }

            ImGui::TableNextColumn();
            FormatZeny(amount, cell, sizeof(cell));
            ImGui::TextUnformatted(cell);
          }
          ImGui::EndTable();
          ImGui::Separator();
          char tbuf[32];
          FormatZeny(total, tbuf, sizeof(tbuf));
          ImGui::Text(log_buying_ ? i18n::Tr("Total des achats : %s z") : i18n::Tr("Total des ventes : %s z"), tbuf);
        }
        // Pas de confirmation : contrairement au cmd 201 de « Mon shop », celui-ci
        // ne dispatche aucun CMode::SendMsg — il ne met fin à rien, il ferme.
        close_log = ro::RoButton(i18n::Tr("Fermer"));
      }
      ro::EndRoWindow();
      // Hors du Begin/End : la croix peut être cliquée sur une fenêtre que Begin
      // rendrait false (repliée, clippée) ; SendButton n'a besoin d'aucun état ImGui.
      if (close_log || !keep_log)
        QueueCommand(log_buying_ ? kWinSellLogBuying : kWinSellLog,
                     kCmdMyShopClose);
    }
  }

  // ── Achat chez un vendeur (0x2B offre + 0x2C panier) ───────────────────────
  if (vendor_open_) {
    if (vendor_panel_) {
      RefreshVendorShop();
      ro::SetNextWindowBodyColor(ro::ListBodyColorU32());
      ImGui::SetNextWindowSize(ImVec2(ro::Px(kBuyW), ro::Px(kBuyH)),
                               ImGuiCond_FirstUseEver);
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(ro::Px(kBuyMinW), ro::Px(kBuyMinH)), ImVec2(FLT_MAX, FLT_MAX));
      bool keep_vendor = true;
      bool close_vendor = false;
      // « ### » fige l'identifiant ImGui : le nom du vendeur arrive une frame
      // après l'ouverture, et sans ça la fenêtre serait recréée (position perdue).
      char title[96];
      if (vendor_name_[0] != '\0')
        std::snprintf(title, sizeof(title), i18n::Tr("Shop de %s###vending_buy"),
                      vendor_name_);
      else
        std::snprintf(title, sizeof(title), i18n::Tr("Shop###vending_buy"));
      // Ni AlwaysAutoResize ni NoResize : la fenêtre se redimensionne à la
      // poignée, et BeginRoWindow y peint le grip RO du coin bas-droit.
      if (ro::BeginRoWindow(title, &keep_vendor, ImGuiWindowFlags_NoCollapse)) {
        char cell[48];
        if (offers_.empty()) {
          ImGui::TextDisabled(i18n::Tr("Le shop est vide."));
        } else if (ImGui::BeginTable("##t_offer", 5,
                                     ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_RowBg)) {
          // 🔴 « Objet » en WidthStretch, les autres à largeur fixe : c'est ce
          // qui rend le redimensionnement utile. En WidthFixed la colonne se
          // calait sur le nom le plus long et la place gagnée en élargissant la
          // fenêtre partait en vide à droite.
          ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn(i18n::Tr("Stock"), ImGuiTableColumnFlags_WidthFixed, ro::Px(45.0f));
          ImGui::TableSetupColumn(i18n::Tr("Prix"), ImGuiTableColumnFlags_WidthFixed, ro::Px(110.0f));
          ImGui::TableSetupColumn(i18n::Tr("Qté"), ImGuiTableColumnFlags_WidthFixed, ro::Px(60.0f));
          ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, ro::Px(60.0f));
          ImGui::TableHeadersRow();
          for (size_t i = 0; i < offers_.size(); ++i) {
            const BuyRow& o = offers_[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawItemCell(o.desc, o.slots, SessionBase(), kOffSessOfferList);

            ImGui::TableNextColumn();
            ImGui::Text("%d", o.stock);

            // Remise (Discount) : le natif écrit « base -> effectif ». On garde
            // la même lecture, sinon le prix affiché ne serait pas celui payé.
            ImGui::TableNextColumn();
            FormatZeny(o.price, cell, sizeof(cell));
            if (o.base_price != o.price && o.base_price > 0) {
              char base[32];
              FormatZeny(o.base_price, base, sizeof(base));
              ImGui::TextDisabled("%s", base);
              ImGui::SameLine(0.0f, 4.0f);
              ImGui::Text("> %s", cell);
            } else {
              ImGui::TextUnformatted(cell);
            }

            ImGui::TableNextColumn();
            if (i < offer_qty_.size()) {
              ImGui::SetNextItemWidth(ro::Px(56.0f));
              ImGui::InputInt("##q", &offer_qty_[i], 0, 0);
              if (offer_qty_[i] < 1) offer_qty_[i] = 1;
              if (offer_qty_[i] > o.stock) offer_qty_[i] = o.stock;
            }

            ImGui::TableNextColumn();
            if (ro::RoSmallButton(i18n::Tr("Ajouter"))) {
              if (ImGui::GetIO().KeyCtrl) {
                // Ctrl+clic = achat IMMÉDIAT de toute la ligne, panier ignoré.
                QuickBuy(o, o.stock);
              } else {
                const int qty = (i < offer_qty_.size()) ? offer_qty_[i] : 1;
                // Une ligne par index d'échoppe : le natif n'en envoie pas deux
                // pour le même index, on fusionne donc au lieu d'empiler.
                bool merged = false;
                for (BasketLine& b : basket_) {
                  if (b.index != o.index) continue;
                  b.amount = (b.amount + qty > o.stock) ? o.stock : b.amount + qty;
                  merged = true;
                  break;
                }
                if (!merged) {
                  BasketLine b;
                  b.id = o.id;
                  b.index = o.index;
                  b.amount = qty;
                  b.price = o.price;
                  b.max = o.stock;
                  b.desc = o.desc;
                  basket_.push_back(b);
                }
              }
            }
            ImGui::PopID();
          }
          ImGui::EndTable();
        }

        if (!offers_.empty())
          ImGui::TextDisabled(i18n::Tr("Ctrl+clic sur « Ajouter » : achète toute la ligne "
                              "immédiatement."));

        ImGui::Separator();
        long long total = 0;
        for (const BasketLine& b : basket_)
          total += static_cast<long long>(b.price) * b.amount;

        if (basket_.empty()) {
          ImGui::TextDisabled(i18n::Tr("Panier vide."));
        } else if (ImGui::BeginTable("##t_basket", 4,
                                     ImGuiTableFlags_SizingFixedFit |
                                     ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn(i18n::Tr("Panier"), ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn(i18n::Tr("Qté"), ImGuiTableColumnFlags_WidthFixed, ro::Px(45.0f));
          ImGui::TableSetupColumn(i18n::Tr("Total"), ImGuiTableColumnFlags_WidthFixed, ro::Px(110.0f));
          ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, ro::Px(60.0f));
          ImGui::TableHeadersRow();
          int remove_at = -1;
          for (size_t i = 0; i < basket_.size(); ++i) {
            const BasketLine& b = basket_[i];
            ImGui::PushID(static_cast<int>(1000 + i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            // La ligne du panier décrit le MÊME objet que l'offre : on retrouve
            // donc son nœud dans la liste du vendeur.
            DrawItemCell(b.desc, 0, SessionBase(), kOffSessOfferList);

            ImGui::TableNextColumn();
            ImGui::Text("%d", b.amount);

            ImGui::TableNextColumn();
            FormatZeny(static_cast<long long>(b.price) * b.amount, cell,
                       sizeof(cell));
            ImGui::TextUnformatted(cell);

            ImGui::TableNextColumn();
            if (ro::RoSmallButton(i18n::Tr("Retirer"))) remove_at = static_cast<int>(i);
            ImGui::PopID();
          }
          ImGui::EndTable();
          if (remove_at >= 0)
            basket_.erase(basket_.begin() + remove_at);
        }

        FormatZeny(total, cell, sizeof(cell));
        ImGui::Text(i18n::Tr("Total : %s z"), cell);
        const int zeny = PlayerZeny();
        const bool affordable = total <= static_cast<long long>(zeny);
        if (!affordable) {
          char have[32];
          FormatZeny(zeny, have, sizeof(have));
          ImGui::TextDisabled(i18n::Tr("Zeny insuffisant (%s z)."), have);
        }

        // Le serveur revalide de toute façon ; griser sert juste à ne pas
        // envoyer une requête qu'on sait perdue d'avance.
        ImGui::BeginDisabled(basket_.empty() || !affordable);
        if (ro::RoButton(i18n::Tr("Acheter"))) SendVendingBuy();
        ImGui::EndDisabled();
        ImGui::SameLine();
        close_vendor = ro::RoButton(i18n::Tr("Fermer"));
      }
      ro::EndRoWindow();
      // L'échoppe s'est vidée sous nos achats : plus rien à faire ici, on ferme
      // — c'est aussi ce que fait le client natif quand tout est vendu. Le garde
      // `bought_once_` évite de fermer sur une liste simplement pas encore reçue.
      if (offers_.empty() && bought_once_) close_vendor = true;
      if (close_vendor || !keep_vendor) CloseVendorSession();
    }
  }

  // ── Vente à un buying store (0xB1 recherche + 0xB2 vente + 0xB3 stock) ─────
  if (bs_open_) {
    bs_wanted_ = FindWnd(kWinBsWanted);   // re-résolution par frame
    bs_sell_   = FindWnd(kWinBsSellList);
    bs_mirror_ = FindWnd(kWinBsMirror);
    if (!bs_wanted_ || !bs_sell_ || !bs_mirror_) {
      bs_open_ = false;
      bs_wanted_rows_.clear();
      bs_avail_.clear();
      bs_sell_rows_.clear();
    } else if (bs_panel_) {
      RefreshBuyingStoreSell();
      ro::SetNextWindowBodyColor(ro::ListBodyColorU32());
      // Fenêtre REDIMENSIONNABLE, taille seulement à la première ouverture (même
      // remède qu'en composition) : les listes se vident l'une dans l'autre, une
      // fenêtre auto-resize sauterait à chaque objet déplacé.
      ImGui::SetNextWindowSize(ImVec2(kBsW, kBsH), ImGuiCond_FirstUseEver);
      bool keep_bs = true;
      bool close_bs = false;
      // « ### » fige l'identifiant : le nom de l'acheteur arrive une frame après
      // l'ouverture, et sans ça la fenêtre serait recréée (position perdue).
      char title[96];
      if (bs_buyer_[0] != '\0')
        std::snprintf(title, sizeof(title), i18n::Tr("Vendre à %s###buying_store_sell"),
                      bs_buyer_);
      else
        std::snprintf(title, sizeof(title), i18n::Tr("Vendre###buying_store_sell"));
      if (ro::BeginRoWindow(title, &keep_bs, ImGuiWindowFlags_NoCollapse)) {
        char cell[48];
        // Les deux listes du haut ont une hauteur FIXE et défilent : elles
        // changent de taille sans cesse, et c'est « À vendre » qui doit profiter
        // de l'agrandissement de la fenêtre.
        const float pane_h = ImGui::GetTextLineHeightWithSpacing() * kBsPaneRows;
        // Pied réservé en dur (total, fonds, avertissement, boutons) pour que le
        // panneau « À vendre » ne pousse jamais les boutons hors de la fenêtre.
        const float footer_h = ImGui::GetTextLineHeightWithSpacing() * 3.0f +
                               ImGui::GetFrameHeightWithSpacing();

        // ── Ce que l'acheteur recherche ──────────────────────────────────────
        ImGui::TextUnformatted(i18n::Tr("Objets recherchés"));
        ImGui::BeginChild("##bs_wanted_pane", ImVec2(-1.0f, pane_h),
                          ImGuiChildFlags_Borders);
        if (bs_wanted_rows_.empty()) {
          ImGui::TextDisabled(i18n::Tr("Cette échoppe ne demande plus rien."));
        } else if (ImGui::BeginTable("##t_bs_wanted", 3,
                                     ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn(i18n::Tr("Voulu"), ImGuiTableColumnFlags_WidthFixed, ro::Px(52.0f));
          ImGui::TableSetupColumn(i18n::Tr("Prix"), ImGuiTableColumnFlags_WidthFixed, ro::Px(110.0f));
          ImGui::TableHeadersRow();
          for (size_t i = 0; i < bs_wanted_rows_.size(); ++i) {
            const BuyRow& w = bs_wanted_rows_[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawItemCell(w.desc, w.slots, bs_wanted_, kOffVendorList);

            ImGui::TableNextColumn();
            ImGui::Text("%d", w.stock);

            ImGui::TableNextColumn();
            FormatZeny(w.price, cell, sizeof(cell));
            ImGui::TextUnformatted(cell);
          }
          ImGui::EndTable();
        }
        ImGui::EndChild();

        // ── Mon stock proposable ─────────────────────────────────────────────
        ImGui::TextUnformatted(i18n::Tr("Mes objets"));
        int add_index = -1, add_qty = 0;
        ImGui::BeginChild("##bs_avail_pane", ImVec2(-1.0f, pane_h),
                          ImGuiChildFlags_Borders);
        if (bs_avail_.empty()) {
          ImGui::TextDisabled(i18n::Tr("Rien à proposer."));
        } else if (ImGui::BeginTable("##t_bs_avail", 4,
                                     ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn(i18n::Tr("Stock"), ImGuiTableColumnFlags_WidthFixed, ro::Px(46.0f));
          ImGui::TableSetupColumn(i18n::Tr("Qté"), ImGuiTableColumnFlags_WidthFixed, ro::Px(56.0f));
          ImGui::TableSetupColumn("##add", ImGuiTableColumnFlags_WidthFixed, ro::Px(60.0f));
          ImGui::TableHeadersRow();
          for (size_t i = 0; i < bs_avail_.size(); ++i) {
            const Row& a = bs_avail_[i];
            const int sellable = BsSellableQty(static_cast<int>(i));
            ImGui::PushID(static_cast<int>(2000 + i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawItemCell(a.desc, a.slots, bs_mirror_, kOffMirrorList);

            ImGui::TableNextColumn();
            ImGui::Text("%d", a.amount);

            ImGui::TableNextColumn();
            if (i < bs_qty_.size()) {
              ImGui::SetNextItemWidth(ro::Px(52.0f));
              ImGui::InputInt("##q", &bs_qty_[i], 0, 0);
              if (bs_qty_[i] < 1) bs_qty_[i] = 1;
              if (bs_qty_[i] > sellable) bs_qty_[i] = sellable;
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(sellable <= 0);
            if (ro::RoSmallButton(i18n::Tr("Vendre"))) {
              add_index = static_cast<int>(i);
              // Ctrl+clic : tout ce que l'acheteur peut encore prendre.
              add_qty = ImGui::GetIO().KeyCtrl
                            ? sellable
                            : (i < bs_qty_.size() ? bs_qty_[i] : sellable);
            }
            ImGui::EndDisabled();
            if (sellable <= 0 && ImGui::IsItemHovered(
                                     ImGuiHoveredFlags_AllowWhenDisabled))
              ImGui::SetTooltip(i18n::Tr("L'acheteur n'en veut plus."));
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
        ImGui::EndChild();
        // Hors du panneau défilant : ces deux lignes doivent rester lisibles même
        // quand la liste déborde.
        ImGui::TextDisabled(i18n::Tr("Ctrl+clic sur « Vendre » : propose tout ce que "
                            "l'acheteur peut encore prendre."));
        // ⚠ Comportement natif reproduit tel quel : mettre une PARTIE d'une pile
        // en vente en retire la pile ENTIÈRE d'ici. Le dire évite de le prendre
        // pour une perte d'objets.
        ImGui::TextDisabled(i18n::Tr("Une pile entamée quitte cette liste : le reste "
                            "revient si vous retirez la ligne."));

        // ── Ce que je mets en vente ──────────────────────────────────────────
        ImGui::TextUnformatted(i18n::Tr("À vendre"));
        long long total = 0;
        for (const Row& s : bs_sell_rows_)
          total += static_cast<long long>(s.price ? s.price : BsPriceOf(s.id)) *
                   s.amount;

        int remove_index = -1;
        // Hauteur = tout ce qui reste MOINS le pied : c'est cette liste qui
        // profite de l'agrandissement, et les boutons ne sortent jamais.
        ImGui::BeginChild("##bs_sell_pane", ImVec2(-1.0f, -footer_h),
                          ImGuiChildFlags_Borders);
        if (bs_sell_rows_.empty()) {
          ImGui::TextDisabled(i18n::Tr("Rien en vente."));
        } else if (ImGui::BeginTable("##t_bs_sell", 4,
                                     ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_RowBg)) {
          ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn(i18n::Tr("Qté"), ImGuiTableColumnFlags_WidthFixed, ro::Px(46.0f));
          ImGui::TableSetupColumn(i18n::Tr("Total"), ImGuiTableColumnFlags_WidthFixed, ro::Px(110.0f));
          ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, ro::Px(60.0f));
          ImGui::TableHeadersRow();
          for (size_t i = 0; i < bs_sell_rows_.size(); ++i) {
            const Row& s = bs_sell_rows_[i];
            const int unit = s.price ? s.price : BsPriceOf(s.id);
            ImGui::PushID(static_cast<int>(3000 + i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            DrawItemCell(s.desc, s.slots, bs_sell_, kOffVendorList);

            ImGui::TableNextColumn();
            ImGui::Text("%d", s.amount);

            ImGui::TableNextColumn();
            FormatZeny(static_cast<long long>(unit) * s.amount, cell,
                       sizeof(cell));
            ImGui::TextUnformatted(cell);

            ImGui::TableNextColumn();
            if (ro::RoSmallButton(i18n::Tr("Retirer"))) remove_index = static_cast<int>(i);
            ImGui::PopID();
          }
          ImGui::EndTable();
        }
        ImGui::EndChild();

        FormatZeny(total, cell, sizeof(cell));
        ImGui::Text(i18n::Tr("Total : %s z"), cell);
        char left[32];
        FormatZeny(bs_zeny_left_, left, sizeof(left));
        ImGui::Text(i18n::Tr("Fonds de l'acheteur : %s z"), left);

        // Les deux refus que le natif oppose en boîte de dialogue, dits AVANT
        // le clic plutôt qu'après (MsgString 0x6CC et 0x2B6). La ligne est
        // TOUJOURS émise (vide si tout va bien) : le pied a une hauteur réservée
        // en dur, une ligne qui apparaît décalerait tout le reste.
        const bool over_funds = total > static_cast<long long>(bs_zeny_left_);
        const long long kZenyMax = 0x7FFFFFFFLL;
        const bool overflow =
            static_cast<long long>(PlayerZeny()) + total > kZenyMax;
        if (over_funds)
          ImGui::TextDisabled(i18n::Tr("L'acheteur n'a pas de quoi tout payer."));
        else if (overflow)
          ImGui::TextDisabled(i18n::Tr("Cette vente dépasserait le zeny maximum."));
        else
          ImGui::TextDisabled(" ");

        ImGui::BeginDisabled(bs_sell_rows_.empty() || over_funds || overflow);
        // cmd 184 : le natif vérifie une dernière fois, émet la transaction et
        // ferme les trois fenêtres. On ne fabrique PAS le paquet nous-mêmes ici
        // (contrairement à l'achat) : l'index qui part dans CZ 0x0819 est celui
        // de l'inventaire, et c'est la liste de session qui le tient.
        if (ro::RoButton(i18n::Tr("Vendre"))) QueueCommand(kWinBsSellList, kCmdBsSell);
        ImGui::EndDisabled();
        ImGui::SameLine();
        close_bs = ro::RoButton(i18n::Tr("Fermer"));

        // Mutations après les tables : elles reconstruisent les listes.
        if (add_index >= 0) BsAddToSellList(add_index, add_qty);
        else if (remove_index >= 0) BsRemoveFromSellList(remove_index);
      }
      ro::EndRoWindow();
      // cmd 185 = annulation native : rects sauvés, SendMsg 40, panier de
      // session vidé — et les objets retenus reviennent. C'est la vraie sortie.
      if (close_bs || !keep_bs) QueueCommand(kWinBsSellList, kCmdBsCancel);
    }
  }

  // ⚠ L'aperçu se dessine à la toute fin d'OnRenderUI. Ce retour anticipé le
  // sautait dès qu'on n'était PAS en composition — donc tout le côté acheteur,
  // « Mon shop » et l'historique n'avaient jamais de survol.
  if (!open_) {
    DrawHoverDesc();
    return;
  }

  // OnTick ne passe que toutes les ~100 ms : la fenêtre native peut avoir été
  // DÉTRUITE depuis (le joueur valide, le serveur refuse...). On re-résout donc le
  // pointeur à chaque frame — sans ça on lirait de la mémoire libérée, et le SEH
  // ne rattrape rien quand elle a déjà été réallouée.
  wnd_ = FindWnd(buying_ ? kWinBuyingStore : kWinVending);
  if (!wnd_) { open_ = false; rows_.clear(); avail_.clear(); return; }
  mirror_ = FindWnd(buying_ ? kWinBuyingMirror : kWinVendingMirror);

  Refresh();

  // Suite d'un clic sur « Importer » : dès que le natif a reposé les objets, on
  // récupère prix, quantités et nom à la MÊME source que lui (le vecteur snapshot).
  if (import_frames_ > 0) {
    --import_frames_;
    if (!rows_.empty()) {
      import_frames_ = 0;
      // ⚠ On applique la copie prise AVANT le clic : le handler natif VIDE le
      // vecteur snapshot en sortant (sub_5E2A60 fait `end = begin`), donc le
      // relire ici ne ramènerait que des zéros — c'était le bug « les objets
      // arrivent mais pas les prix ». Le NOM, lui, survit (autre variable), ce
      // qui masquait le problème.
      //
      // Recalage par ID, pas par indice : le natif saute les objets du snapshot
      // qui ne sont plus dans le cart, et un décalage collerait alors le prix
      // d'un objet sur un autre. `used` gère le cas de deux piles du même id.
      bool used[kMaxRows] = {false};
      const int rows_n = static_cast<int>(rows_.size());
      for (int i = 0; i < rows_n && i < kMaxRows; ++i) {
        for (int j = 0; j < import_count_; ++j) {
          if (used[j] || import_ids_[j] != rows_[i].id) continue;
          prices_[i]  = import_prices_[j];
          amounts_[i] = import_amounts_[j];
          used[j] = true;
          break;
        }
      }
      if (import_name_[0] != '\0') {
        std::strncpy(name_, import_name_, sizeof(name_) - 1);
        name_[sizeof(name_) - 1] = '\0';
      }
      import_count_ = 0;
    }
  }

  // Le X de notre fenêtre = le bouton Annuler natif (il réinitialise l'état
  // client ET prévient le serveur ; fermer sans ça laisserait le montage en
  // suspens, comme le cmd 0x28 du shop NPC).
  if (!show_panel_) {
    QueueCommand(buying_ ? kWinBuyingStore : kWinVending, kCmdCancel);
    show_panel_ = true;
    return;
  }

  const char* title = buying_ ? i18n::Tr("Buying store") : i18n::Tr("Ouvrir un shop");
  ro::SetNextWindowBodyColor(ro::ListBodyColorU32());
  // Fenêtre REDIMENSIONNABLE, taille seulement à la première ouverture. Elle était
  // en AlwaysAutoResize, ce qui imposait des largeurs en dur partout et donc de
  // faire coïncider à la main des sections qui ne se comptent pas pareil (ImGui
  // ajoute CellPadding.x de chaque côté de CHAQUE colonne) : d'où un panneau plus
  // étroit que son tableau, et du contenu rogné. En redimensionnable, WidthStretch
  // et « -1 » sont sûrs — l'oscillation n'existait qu'en auto-resize.
  // NoCollapse : rien à replier, et le toolkit retire alors le bouton sys_mini.
  // 🔴 La fenêtre elle-même ne défile JAMAIS. Sans ces deux drapeaux, un contenu
  // plus haut qu'elle lui donnait sa propre barre : la molette faisait alors
  // sortir par le haut le champ Nom et l'en-tête du stock (avec sa case Grille),
  // et deux barres imbriquées se disputaient le geste. Ce qui doit défiler, ce
  // sont les DEUX panneaux ci-dessous ; le nom, les totaux et les boutons
  // restent visibles en toute circonstance.
  ImGui::SetNextWindowSize(ImVec2(kComposeW, kComposeH), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(kComposeMinW, kComposeMinH),
                                      ImVec2(FLT_MAX, FLT_MAX));
  if (ro::BeginRoWindow(title, &show_panel_,
                        ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse)) {
    // ── Nom du shop ──
    ImGui::TextUnformatted(i18n::Tr("Nom"));
    ImGui::SameLine(kNameLabelW);
    ImGui::SetNextItemWidth(-1.0f);
    ro::InputTextCp949("##nom_boutique", name_, sizeof(name_));

    ImGui::Separator();

    const int rows = static_cast<int>(rows_.size());
    const int avail = static_cast<int>(avail_.size());
    // Les actions MUTENT les deux listes : on ne peut pas les exécuter au milieu
    // d'un parcours. On note l'intention et on l'applique après les tables.
    int place_index = -1, take_row = -1;

    // ── Objets disponibles ────────────────────────────────────────────────────
    ImGui::TextUnformatted(buying_ ? i18n::Tr("Objets achetables") : i18n::Tr("Objets du cart"));
    ImGui::SameLine();
    ImGui::TextDisabled("(%d)", avail);
    // Bascule liste/grille, à DROITE de l'en-tête : le réglage ne concerne que ce
    // panneau, il vit donc là où il agit plutôt que dans le panneau Moonlight (il
    // y est seulement PERSISTÉ). La grille reprend la présentation du natif, où
    // l'on glisse des icônes ; la liste garde les quantités éditables en ligne.
    {
      const char* kGridLabel = "Grille";
      const float bw = ImGui::CalcTextSize(kGridLabel).x +
                       ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
      ImGui::SameLine(ImGui::GetContentRegionAvail().x - bw);
      if (ro::RoCheckbox(kGridLabel, &compose_grid_))
        if (auto* mu = Bourgeon::Instance().moonlight_ui()) mu->SaveSettings();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(i18n::Tr("Affiche le stock en grille d'icônes, comme le client "
                          "d'origine.\nLe glisser fonctionne dans les deux modes."));
    }
    // Répartition de la hauteur entre les deux panneaux défilants.
    //
    // Le SURPLUS va au stock, pas aux objets posés : ces derniers sont bornés
    // par les emplacements (12 en vente, 5 en achat) et n'ont donc jamais besoin
    // de plus que leur contenu, tandis que le cart peut aligner cent lots. Une
    // part fixe donnait le résultat inverse — quatre lignes de stock visibles au
    // milieu d'un grand panneau « Dans le shop » vide.
    //
    // La hauteur qu'il FAUT aux objets posés est mesurée à la frame précédente
    // (même procédé que le pied de page) : elle dépend de la hauteur réelle des
    // lignes, qui portent des icônes et non du simple texte.
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    const float panes_h =
        ImGui::GetContentRegionAvail().y - compose_footer_h_ - line_h;
    const float pane_min = line_h * 4.0f;   // stock : moins, ça ne se consulte plus
    const float rows_min = line_h * 3.0f;   // posés : de quoi voir l'invite
    float rows_want = compose_rows_content_h_;
    if (rows_want < rows_min) rows_want = rows_min;
    if (rows_want > panes_h - pane_min) rows_want = panes_h - pane_min;
    float pane_h = panes_h - rows_want;
    if (pane_h < pane_min) pane_h = pane_min;
    if (ImGui::BeginChild("##dispo", ImVec2(-1.0f, pane_h),
                          ImGuiChildFlags_Borders)) {
      if (avail == 0) {
        ImGui::TextDisabled(i18n::Tr("Aucun objet proposable."));
      } else if (compose_grid_) {
        // ── Présentation GRILLE (celle du natif) ──────────────────────────────
        // Une tuile par lot, l'icône à sa taille naturelle et la quantité en
        // badge — c'est itemcell::DrawTile qui fait les deux, comme la grille de
        // l'inventaire.
        // La tuile n'a pas de champ quantité : on la DEMANDE après le geste (cf.
        // grid_ask_src_), exactement comme le natif ouvre une boîte de saisie
        // quand on y glisse une pile. En vente la pose est bien PARTIELLE — la
        // liste des disponibles n'est décrémentée que de la quantité posée, le
        // reste continue de s'afficher et peut être ajouté plus tard (le natif
        // cumule alors dans la ligne existante). Le « une seule fois » ne vaut
        // que pour l'échoppe d'ACHAT (kBsMirrorMode).
        constexpr float kTile = 32.0f;
        const float step = kTile + ImGui::GetStyle().ItemSpacing.x;
        const float availw = ImGui::GetContentRegionAvail().x;
        int per_row = static_cast<int>(availw / step);
        if (per_row < 1) per_row = 1;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool full = rows >= slots_;
        for (int i = 0; i < avail; ++i) {
          const Row& a = avail_[i];
          if (i % per_row != 0) ImGui::SameLine();
          ImGui::PushID(20000 + i);
          const ImVec2 p0 = ImGui::GetCursorScreenPos();
          // Le bouton invisible porte le clic, le survol ET le glisser ; la tuile
          // est peinte par-dessus au draw list (jamais de SetCursorPos, qui
          // étendrait les limites de la fenêtre sans soumettre d'item).
          ImGui::InvisibleButton("##tile", ImVec2(kTile, kTile));
          const bool hovered = ImGui::IsItemHovered();
          if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload(kDndAvail, &i, sizeof(int));
            ImGui::TextUnformatted(a.desc.name[0] ? a.desc.name
                                                  : itemcell::NameById(a.desc.id));
            ImGui::EndDragDropSource();
          }
          // Double-clic = poser, pour qui préfère ne pas glisser.
          if (!full && a.amount > 0 &&
              ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered)
            place_index = i;
          const ImVec2 p1(p0.x + kTile, p0.y + kTile);
          if (hovered)
            dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_HeaderHovered));
          ro::IconTex ic = ro::ItemIcon(a.desc.id);
          itemcell::DrawTile(dl, p0, p1, kTile, ic, 0, a.amount,
                             a.desc.damaged != 0);
          if (hovered) ItemHover(a.desc, mirror_, kOffMirrorList);
          ImGui::PopID();
        }
      } else if (ImGui::BeginTable("##t_dispo", 4,
                                   ImGuiTableFlags_SizingStretchProp |
                                   ImGuiTableFlags_RowBg)) {
        // « Objet » absorbe la largeur restante : c'est la colonne qui porte les
        // noms composés (« +10 Bloodlust Sword Master Krishna »), donc celle qui
        // profite le mieux d'un agrandissement.
        ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(i18n::Tr("Reste"), ImGuiTableColumnFlags_WidthFixed, kAvailStock);
        ImGui::TableSetupColumn(i18n::Tr("Qté"), ImGuiTableColumnFlags_WidthFixed, kAvailQty);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, kAvailAct);
        ImGui::TableHeadersRow();

        const bool full = rows >= slots_;
        for (int i = 0; i < avail; ++i) {
          const Row& a = avail_[i];
          // La liste des disponibles EST décrémentée à chaque pose (c'est le rôle
          // de kAvailConsume) : sa quantité est donc déjà le reste à poser. Ne
          // surtout PAS en retrancher ce qui est dans l'échoppe, ce serait
          // soustraire deux fois.
          const int remaining = a.amount;

          ImGui::TableNextRow();
          ImGui::PushID(10000 + i);

          ImGui::TableNextColumn();
          DrawItemCell(a.desc, a.slots, mirror_, kOffMirrorList);
          // Le glisser marche AUSSI en mode liste : c'est le même geste, seule la
          // présentation change. La source, c'est la cellule de nom.
          if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload(kDndAvail, &i, sizeof(int));
            ImGui::TextUnformatted(a.desc.name[0] ? a.desc.name
                                                  : itemcell::NameById(a.desc.id));
            ImGui::EndDragDropSource();
          }

          ImGui::TableNextColumn();
          ImGui::Text("%d", remaining);

          // Quantité à poser. Le natif ouvre une modale pour la demander ; ici
          // elle est éditable sur la ligne, ce qui évite un aller-retour.
          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-1.0f);
          if (i < static_cast<int>(avail_qty_.size())) {
            ImGui::InputInt("##q", &avail_qty_[i], 0, 0);
            if (avail_qty_[i] > remaining) avail_qty_[i] = remaining;
            if (avail_qty_[i] < 1) avail_qty_[i] = (remaining > 0) ? 1 : 0;
          }

          ImGui::TableNextColumn();
          const bool blocked = full || remaining <= 0;
          if (blocked) ImGui::BeginDisabled();
          if (ro::RoSmallButton(i18n::Tr("Poser"))) place_index = i;
          if (blocked) ImGui::EndDisabled();
          if (blocked && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", full ? i18n::Tr("Tous les emplacements sont pris.") : i18n::Tr("Tout le lot est déjà dans le shop."));

          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
    ImGui::EndChild();
    // Lâcher une ligne de l'échoppe ICI = la retirer. Le panneau tout entier sert
    // de cible : après EndChild, l'enfant se comporte comme un item ordinaire.
    if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kDndRow))
        take_row = *static_cast<const int*>(pl->Data);
      ImGui::EndDragDropTarget();
    }

    // ── Objets posés dans l'échoppe ───────────────────────────────────────────
    ImGui::TextUnformatted(i18n::Tr("Dans le shop"));
    ImGui::SameLine();
    ImGui::TextDisabled("(%d/%d)", rows, slots_);

    // Zone de dépôt, affichée SEULEMENT pendant qu'on glisse un objet du stock.
    // La faire apparaître en permanence coûterait une bande de vide dans une
    // fenêtre déjà dense ; la montrer au bon moment la rend au contraire
    // évidente. Le mode LISTE en profite autant que la grille.
    {
      const ImGuiPayload* drag = ImGui::GetDragDropPayload();
      if (drag && drag->IsDataType(kDndAvail)) {
        const bool full = rows >= slots_;
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(
            full ? ImGuiCol_FrameBg : ImGuiCol_HeaderHovered));
        ImGui::Button(full ? i18n::Tr("Tous les emplacements sont pris") : i18n::Tr("Déposer ici pour mettre en vente"),
                      ImVec2(-1.0f, ImGui::GetFrameHeight()));
        ImGui::PopStyleColor();
        if (!full && ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload(kDndAvail))
            place_index = *static_cast<const int*>(pl->Data);
          ImGui::EndDragDropTarget();
        }
      }
    }
    // Second panneau défilant : il prend TOUT le reste, moins le pied de page.
    // Cette hauteur-là est MESURÉE à la frame précédente (compose_footer_h_) :
    // le pied varie — avertissement de plafond, ligne de blocage, champ propre à
    // l'échoppe d'achat — et une réserve en dur finirait tôt ou tard par rogner
    // le bouton « Ouvrir le shop » ou par laisser un trou.
    float rows_h = ImGui::GetContentRegionAvail().y - compose_footer_h_;
    if (rows_h < line_h * 3.0f) rows_h = line_h * 3.0f;
    ImGui::BeginChild("##poses", ImVec2(-1.0f, rows_h));
    if (rows == 0) {
      ImGui::TextDisabled(compose_grid_
                              ? i18n::Tr("Glisse un objet ci-dessus, ou double-clique-le.") : i18n::Tr("Choisis un objet ci-dessus et clique « Poser »."));
    } else {
      const int cols = buying_ ? 6 : 5;
      // Mêmes règles que le panneau du haut : « Objet » s'étire, les colonnes
      // chiffrées restent fixes. Le mode achat a une colonne de plus, elle se
      // prend simplement sur l'étirement — plus rien à faire coïncider.
      if (ImGui::BeginTable("##lignes_echoppe", cols,
                            ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn(i18n::Tr("Objet"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(i18n::Tr("Stock"), ImGuiTableColumnFlags_WidthFixed, kColStock);
        if (buying_)
          ImGui::TableSetupColumn(i18n::Tr("Qté"), ImGuiTableColumnFlags_WidthFixed, kColQty);
        ImGui::TableSetupColumn(i18n::Tr("Prix"), ImGuiTableColumnFlags_WidthFixed, kColPrice);
        ImGui::TableSetupColumn(i18n::Tr("Total"), ImGuiTableColumnFlags_WidthFixed, kColTotal);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, kColAct);
        ImGui::TableHeadersRow();

        for (int i = 0; i < rows; ++i) {
          const Row& r = rows_[i];
          ImGui::TableNextRow();
          ImGui::PushID(i);

          ImGui::TableNextColumn();
          DrawItemCell(r.desc, r.slots, wnd_, kOffList);
          // Glisser une ligne vers le panneau du haut = la retirer de l'échoppe.
          if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload(kDndRow, &i, sizeof(int));
            ImGui::TextUnformatted(r.desc.name[0] ? r.desc.name
                                                  : itemcell::NameById(r.desc.id));
            ImGui::EndDragDropSource();
          }

          ImGui::TableNextColumn();
          ImGui::Text("%d", r.amount);

          // En échoppe d'ACHAT le joueur choisit combien il veut acheter ; en
          // vente la quantité est FIXÉE À LA POSE (comme le natif) : pour en
          // vendre plus, on repose du même objet, la ligne cumule.
          if (buying_) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt("##qte", &amounts_[i], 0, 0);
            if (amounts_[i] < 0) amounts_[i] = 0;
            if (amounts_[i] > 9999) amounts_[i] = 9999;  // plafond natif
          }

          ImGui::TableNextColumn();
          ImGui::SetNextItemWidth(-1.0f);
          ImGui::InputInt("##prix", &prices_[i], 0, 0);
          if (prices_[i] < 0) prices_[i] = 0;
          if (prices_[i] > 1000000000) prices_[i] = 1000000000;  // plafond natif

          ImGui::TableNextColumn();
          const long long qty = buying_ ? amounts_[i] : r.amount;
          char total[32];
          FormatZeny(static_cast<long long>(prices_[i]) * qty, total, sizeof(total));
          ImGui::TextUnformatted(total);

          ImGui::TableNextColumn();
          if (ro::RoSmallButton("x")) take_row = i;
          if (ImGui::IsItemHovered()) ImGui::SetTooltip(i18n::Tr("Retirer du shop"));

          ImGui::PopID();
        }
        ImGui::EndTable();
      }
    }
    // Hauteur qu'il aurait fallu à ce panneau, pour la répartition de la frame
    // suivante. Le cadre l'entoure des deux côtés, d'où la marge comptée deux fois.
    compose_rows_content_h_ =
        ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    ImGui::EndChild();
    // Tout ce qui suit est le PIED DE PAGE, mesuré ici pour la frame suivante.
    const float footer_y0 = ImGui::GetCursorPosY();

    // Application différée (voir plus haut) : une seule mutation par frame.
    if (place_index >= 0) {
      const Row& a = avail_[place_index];
      // En GRILLE, le geste ne dit pas COMBIEN : on ouvre la demande, et la pose
      // attend la réponse. Une unité seule ne mérite pas la question.
      if (compose_grid_ && a.amount > 1) {
        grid_ask_src_ = a.index;
        grid_ask_qty_ = a.amount;
        ImGui::OpenPopup("##qte_pose");
      } else if (compose_grid_) {
        PlaceItem(place_index, a.amount);
      } else {
        const int q = (place_index < static_cast<int>(avail_qty_.size()))
                          ? avail_qty_[place_index] : 0;
        PlaceItem(place_index, q);
      }
    } else if (take_row >= 0) {
      TakeBackItem(take_row);
    }

    // ── Combien en poser ? (mode grille) ──────────────────────────────────────
    // Le lot est retrouvé par son index SOURCE : entre l'ouverture et la
    // validation, `avail_` a pu être reconstruite et réordonnée.
    if (grid_ask_src_ >= 0) {
      int rank = -1;
      for (int i = 0; i < static_cast<int>(avail_.size()); ++i)
        if (avail_[i].index == grid_ask_src_) { rank = i; break; }
      if (rank < 0) {
        grid_ask_src_ = -1;  // le lot a disparu du stock : plus rien à demander
      } else if (ImGui::BeginPopup("##qte_pose")) {
        const Row& a = avail_[rank];
        ImGui::TextUnformatted(a.desc.name[0] ? a.desc.name
                                              : itemcell::NameById(a.desc.id));
        ImGui::TextDisabled(i18n::Tr("Reste : %d"), a.amount);
        ImGui::Separator();
        ImGui::SetNextItemWidth(kAskFieldW);
        // Le champ prend le focus au premier passage : la quantité est la seule
        // chose à saisir, et Entrée valide — le geste se termine au clavier.
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool entered = ImGui::InputInt(
            "##qte_grille", &grid_ask_qty_, 1, 10,
            ImGuiInputTextFlags_EnterReturnsTrue);
        if (grid_ask_qty_ > a.amount) grid_ask_qty_ = a.amount;
        if (grid_ask_qty_ < 1) grid_ask_qty_ = 1;
        if (ro::RoSmallButton(i18n::Tr("Tout"))) grid_ask_qty_ = a.amount;
        ImGui::SameLine();
        const bool ok = ro::RoSmallButton(i18n::Tr("Poser")) || entered;
        ImGui::SameLine();
        if (ro::RoSmallButton(i18n::Tr("Annuler"))) {
          grid_ask_src_ = -1;
          ImGui::CloseCurrentPopup();
        } else if (ok) {
          const int qty = grid_ask_qty_;
          grid_ask_src_ = -1;
          ImGui::CloseCurrentPopup();
          PlaceItem(rank, qty);
        }
        ImGui::EndPopup();
      } else {
        grid_ask_src_ = -1;  // fermée en cliquant ailleurs = renoncement
      }
    }

    // ── Totaux ──
    // `rows` date d'AVANT la mutation différée ci-dessus : on relit la taille
    // réelle, sinon un retrait ferait déborder l'indexation de rows_.
    const int rows_now = static_cast<int>(rows_.size());
    long long grand = 0;
    for (int i = 0; i < rows_now; ++i)
      grand += static_cast<long long>(prices_[i]) *
               (buying_ ? amounts_[i] : rows_[i].amount);

    ImGui::Separator();
    char buf[32];
    FormatZeny(grand, buf, sizeof(buf));
    ImGui::Text(buying_ ? i18n::Tr("Coût total : %s z") : i18n::Tr("Recette brute : %s z"), buf);
    ImGui::SameLine();
    ImGui::TextDisabled(i18n::Tr("(hors taxe serveur)"));

    FormatZeny(PlayerZeny(), buf, sizeof(buf));
    ImGui::Text(i18n::Tr("Zeny : %s z"), buf);
    ImGui::SameLine();
    ImGui::TextDisabled(i18n::Tr("| %d/%d emplacement(s)"), rows_now, slots_);

    // ⚠ Contrôle d'overflow du natif (étape 9 du chemin OK, cf. la doc RE) :
    // en VENTE il refuse si `zeny + Σ(qté × prix) > 0x7FFFFFFF`. Le refus est
    // muet côté joueur, et comme on pilote le bouton natif on héritait du même
    // silence : le shop ne s'ouvrait pas, sans un mot. On teste donc AVANT, en
    // 64 bits, et on dit ce qui coince.
    const long long kZenyMax = 0x7FFFFFFFLL;
    const long long zeny_now = PlayerZeny();
    const bool overflow = !buying_ && (zeny_now + grand > kZenyMax);
    if (overflow) {
      char cap[32], over[32];
      FormatZeny(kZenyMax, cap, sizeof(cap));
      FormatZeny(zeny_now + grand - kZenyMax, over, sizeof(over));
      ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f),
                         i18n::Tr("Zeny + recette dépasse le plafond de %s z (excédent : %s z)."),
                         cap, over);
      ImGui::TextDisabled(
          i18n::Tr("Le client refuse d'ouvrir le shop dans ce cas. Baisse un prix ou une\n"
          "quantité, ou dépose des zeny au storage."));
    }

    if (buying_) {
      ImGui::SetNextItemWidth(ro::Px(160.0f));
      ImGui::InputInt(i18n::Tr("Limite de zeny d'achat"), &zeny_limit_, 0, 0);
      if (zeny_limit_ < 0) zeny_limit_ = 0;
      ImGui::SameLine();
      if (ro::RoSmallButton(i18n::Tr("= total"))) zeny_limit_ = static_cast<int>(
          grand > 2147483647LL ? 2147483647LL : grand);
    } else {
      // Case native « Safe check for over 10 mil zeny » : on la bascule PAR le
      // natif (cmd 213) pour qu'il garde son état persistant cohérent, et on relit
      // toujours la valeur chez lui.
      bool safe = SafeCheckOn();
      if (ro::RoCheckbox(i18n::Tr("Confirmer les prix élevés"), &safe))
        QueueCommand(kWinVending, kCmdSafeChk);
      ImGui::SameLine();
      ImGui::TextDisabled("(?)");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            i18n::Tr("Case native « Safe check for over 10 mil zeny » : demande une "
            "confirmation dès qu'un prix dépasse le seuil du serveur."));
    }

    // ⚠ Validations du chemin OK natif en ÉCHOPPE D'ACHAT (étapes 5 et 6 de la
    // doc RE). Contrairement à la vente — où un prix nul ouvre une simple
    // CONFIRMATION (MsgString 0x25C, bouton 187) — l'achat REFUSE sèchement :
    //   prix 0            -> MsgString 0x6BD
    //   quantité 0        -> MsgString 0x6BF
    //   prix > 99 999 984 -> MsgString 0x6BE
    //   quantité > 9999   -> MsgString 0x6C0
    //   limite > zeny     -> MsgString 0xE63
    // Laisser cliquer dans ces cas envoyait le joueur droit sur un refus natif,
    // pour une ligne qu'il avait simplement oublié de renseigner.
    const int kBuyPriceMax = 0x5F5B9F0;  // 99 999 984
    const int kBuyQtyMax   = 9999;
    const char* blocker = nullptr;
    // Le prix 0 est interdit dans LES DEUX modes. En achat c'est la règle native
    // (refus sec). En vente le natif se contenterait d'une confirmation — mais
    // vendre à 0 z n'a aucun intérêt, et surtout cette confirmation est une
    // modale BLOQUANTE : autant ne jamais l'atteindre (la file différée reste la
    // vraie protection, ceci évite simplement le cas le plus courant).
    for (int i = 0; i < rows_now && i < kMaxRows && !blocker; ++i) {
      if (prices_[i] <= 0) {
        blocker = i18n::Tr("Un objet est à prix 0.");
      } else if (buying_) {
        if (amounts_[i] <= 0)              blocker = i18n::Tr("Une ligne est à quantité 0.");
        else if (amounts_[i] > kBuyQtyMax) blocker = i18n::Tr("Quantité maximale : 9999 par ligne.");
        else if (prices_[i] > kBuyPriceMax)
          blocker = i18n::Tr("Prix maximal : 99 999 984 z par objet.");
      }
    }
    if (buying_ && !blocker) {
      if (zeny_limit_ <= 0)
        blocker = i18n::Tr("Renseigne la limite de zeny d'achat.");
      else if (static_cast<long long>(zeny_limit_) > zeny_now)
        blocker = i18n::Tr("La limite dépasse tes zeny.");
    }

    // Dit à l'écran, pas seulement au survol d'un bouton grisé : sans ça le
    // joueur cherche pourquoi le bouton ne répond pas.
    if (blocker && rows_now > 0 && name_[0] != '\0')
      ImGui::TextColored(ImVec4(0.85f, 0.15f, 0.15f, 1.0f), "%s", blocker);

    ImGui::Separator();

    const bool can_open =
        rows_now > 0 && name_[0] != '\0' && !overflow && blocker == nullptr;
    if (!can_open) ImGui::BeginDisabled();
    // ⚠ DIFFÉRÉ, jamais appelé ici : le OK natif peut ouvrir une modale
    // bloquante qui relance le rendu en pleine frame ImGui (cf. FlushPending).
    if (ro::RoButton(buying_ ? i18n::Tr("Ouvrir le buying store") : i18n::Tr("Ouvrir le shop")))
      QueueSubmit();
    if (!can_open) ImGui::EndDisabled();
    if (!can_open && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(overflow        ? i18n::Tr("Zeny + recette dépasserait le plafond.")
                        : rows_now == 0 ? i18n::Tr("Pose au moins un objet.")
                        : name_[0] == '\0' ? i18n::Tr("Donne un nom au shop.")
                                           : blocker);
    ImGui::SameLine();
    // « Import » natif : recharge le dernier shop monté par ce personnage. Le
    // bouton natif fait tout le travail (chargement, repose des objets, edits).
    //
    // ⚠ ORDRE CRITIQUE : on lit le snapshot AVANT de cliquer. Le handler natif
    // s'en sert puis le VIDE (sub_5E2A60 : `end = begin`) — après le clic il ne
    // reste rien à lire. Le nom, lui, vit dans une variable distincte qui
    // survit, d'où le symptôme trompeur « les objets et le nom arrivent, les
    // prix non ».
    // Le natif conditionne son bouton au NOM du snapshot (grisé s'il est vide) et
    // le désactive une fois servi — parce qu'un second import repartirait d'un
    // vecteur vide et VIDERAIT le shop au lieu de le remplir. On applique la même
    // règle, à la même source.
    char snap_name[sizeof(name_)];
    ReadSnapshotName(snap_name, sizeof(snap_name));
    const bool can_import = !import_used_ && snap_name[0] != '\0';
    ImGui::BeginDisabled(!can_import);
    if (ro::RoButton(i18n::Tr("Importer"))) {
      import_count_ = ReadSnapshot(import_ids_, import_prices_, import_amounts_,
                                   kMaxRows);
      ReadSnapshotName(import_name_, sizeof(import_name_));
      QueueCommand(buying_ ? kWinBuyingStore : kWinVending, kCmdImport);
      import_frames_ = 30;
      import_used_ = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip(
          can_import ? i18n::Tr("Recharge les objets et les prix de ton dernier shop.")
          : import_used_ ? i18n::Tr("Déjà importé pour ce shop.") : i18n::Tr("Aucun shop précédent enregistré pour ce personnage."));
    ImGui::SameLine();
    if (ro::RoButton(i18n::Tr("Annuler")))
      QueueCommand(buying_ ? kWinBuyingStore : kWinVending, kCmdCancel);

    // Hauteur réelle du pied, pour la réserve de la frame suivante. Un seul
    // frame de décalage à l'ouverture, invisible ; en échange le pied ne peut
    // plus être rogné, quelle que soit la combinaison de messages affichés.
    compose_footer_h_ = ImGui::GetCursorPosY() - footer_y0;
  }
  ro::EndRoWindow();

  DrawHoverDesc();
}

// Aperçu de l'objet survolé. Rendu APRÈS toutes les fenêtres, et hors de
// n'importe quel Begin/End : un tooltip crée son propre popup, l'imbriquer le
// clipperait dans la fenêtre courante.
void VendingWindow::DrawHoverDesc() {
  if (!hover_valid_) return;
  itemcell::DrawTooltip(hover_desc_.id, hover_desc_.cards, 4, hover_desc_.opts,
                    hover_desc_.opt_count, hover_desc_.refine, hover_desc_.name,
                    hover_desc_.damaged != 0);
}
