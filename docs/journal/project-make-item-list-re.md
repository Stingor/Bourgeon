# Fenêtres de fabrication (« LIST »)

> Journal du chantier. La fiche de mémoire `project-make-item-list-re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Les compétences de fabrication (Create Arrow, Elemental Converter, Pharmacy, poison GC,
leurre NC, cuisine, bombes) ouvrent **trois fenêtres natives distinctes**, pas une seule
malgré le titre commun perçu :

| id | Classe | Ouverte par | Réponse |
|---|---|---|---|
| **94** | `UIMakingArrowListWnd` (vt `0x010345AC`) | `ZC 0x01AD`, `ZC 0x025A` | `CZ 0x01AE` (mk_type==2) ou `CZ 0x025B` |
| **79** | `UIMakeTargetListWnd` (vt `0x0103EC50`) | `ZC 0x018D` | `CZ 0x018E` |
| **80** | `UIMakeTargetProcessWnd` (vt `0x0103EED8`) | par la 79 (matériaux de forge) | `CZ 0x018E` |

**Pharmacy (`AM_PHARMACY`) ouvre la 79, PAS la 94** — contrairement à Create Arrow et
Elemental Converter (`SA_CREATECON`, id **1007** dans le fork), qui passent tous deux par
`0x01AD` → fenêtre 94.

Faits durs à ne pas re-dériver :
- 🔴 le titre **`"LIST"` est un littéral ASCII codé en dur** dans
  `UIMakingArrowListWnd::OnDraw` (`0x008B4C30`) — pas une `MsgString`, donc ni traduisible
  ni contextuel pour les 6 métiers ;
- `UIMakingArrowListWnd` **dérive de `UIItemIdentifyWnd`** (ctor `0x0088E210` construit la
  loupe puis écrase la vtable ; `OnCreate` `0x008A8470` est LA MÊME fonction). Le tampon
  SSO du titre vaut `"LIST\0Appraisal\0\0"` en mémoire ;
- `OnMsg(0x1F)` de la 94 reçoit un **POINTEUR** sur l'entrée du paquet, pas l'itemId (la 79
  reçoit la valeur) ;
- `ItemSkillInfo` stocke **l'id d'objet en TEXTE décimal** dans une `std::string` à
  **`+0x2C`** ; `ItemSkillInfo_GetId` (`0x005D98A0`) = `atoi(info+0x2C)`. Taille `0xF8` ;
- la 94 n'affiche que **4 lignes** (`columns=1 +0xC0`, `rows=4 +0xC4`), `mk_type` en `u16`
  à **`+0xF0`**, liste = `std::list` (head `+0xCC`, size `+0xD0`), sélection `+0xBC` ;
- `ZC 0x018F` **compose « Successfully created %s. » puis jette la chaîne** : aucun chat,
  aucun `sprintf`. Et `result = 6` (succès serveur) prend le libellé d'ÉCHEC — bug client à
  ne pas recopier ;
- annuler **doit** envoyer (`itemId = -1`, ou `0` pour `0x018E`), sinon le `menuskill` reste
  armé côté serveur ;
- ⚠ la fenêtre 80 **sort réellement les matériaux de l'inventaire client** ; un
  remplacement ImGui ne doit jamais piloter ses emplacements.

Défaut phare relevé en jeu : les 4 convertisseurs **12114/12115/12116/12117** portent tous
`Name = "Elemental Converter"` dans `itemInfomoon.lua` → 4 lignes identiques. L'élément est
pourtant dans le champ `Desc` de la même DB. (Le joueur a entrepris de corriger l'itemInfo ;
le portage ne doit pas en dépendre.)

🔴 **Le client CONNAÎT des recettes** (corrigé après coup — j'avais d'abord écrit le
contraire). `MetalProcessRecipe_GetLines` (`0x006A3F20`) lit
`g_MetalProcessRecipeMap` (`std::map<int, std::vector<std::string>>` @ `0x01255118`),
peuplée par `ItemInfoDB_LoadFromTextFiles` depuis **`data/MetalProcessItemList.txt`**.
Malgré son nom le fichier couvre armes ET potions (90 produits). Seul lecteur natif :
`UIMakeTargetProcessWnd_OnDraw`, donc visible seulement APRÈS validation.
- ⚠ c'est du **texte** (« 1 Iron Ore »), pas de la donnée structurée ;
- ⚠ **dérive mesurée** : 166 des 254 produits fabricables du fork n'ont aucune recette
  client, et 2 recettes client ne correspondent à rien ;
- ⚠ `MetalProcessItemTable.txt` (structuré, `id#qté#`) existe à côté mais **ce client ne le
  lit pas** — la chaîne est absente de tout le binaire. Fichier mort. Aucune liaison Lua
  n'écrit dans la map : ici Gravity n'a PAS migré vers le Lua.

**ATLAS DES RECETTES — chantier en cours (étape 1/3 faite)**. Décision : DEUX fichiers, deux
publics — le plugin est opt-in, donc le natif garde son fichier.
1. ✅ le générateur émet aussi **`SystemEN\bourgeon_recipes.yaml`** (`paths::RecipesPath()`,
   résout SystemEN\ puis System\) : `recipes` (id, lv=itemlv, skill, mats [[id,qty]]),
   `arrows` (src → yields), et surtout **`by_skill`** — l'index sans lequel on ne peut PAS
   énumérer les formules d'un métier, le getteur natif étant une lookup par clé. YAML et non
   Lua : yaml-cpp est déjà lié (le Lua d'itemInfo est parsé à la main, cf. LoadItemNames).
2. ⬜ chargeur yaml-cpp partagé + la fenêtre de fabrication l'utilise → retire alors DEUX
   bidouilles du .txt : le biais de clé `+1000000` (flèches) et la quantité 0 détournée.
3. ⬜ la fenêtre Atlas elle-même (navigation par compétence, recherche, « il manque », taux).

**Générateur** : `moonlight/tools/gen_metalprocess.py` régénère `MetalProcessItemList.txt`
depuis `produce_db.txt` (254 recettes, ids entre crochets). ⚠ Deux pièges vécus :
- **quantité 0 = « exigé mais NON consommé »** (les guides) — c'est le marqueur du format,
  documenté dans l'en-tête de `produce_db.txt`. Ne PAS l'encoder en texte (une v1 écrivait
  « 1 … (non consomme) » : suffixe retiré à la main = marqueur perdu, et « Faisable » plafonné
  à 1). Le libellé appartient à l'UI. Conséquence côté calcul : un matériau non consommé ne
  BORNE PAS le nombre de fabrications, il est seulement exigé en 1 exemplaire ;
- **régénérer ≠ déployer** : le client lit `<client>\data\MetalProcessItemList.txt` (disque
  avant GRF) et **au démarrage seulement**. Toujours annoncer la copie à faire.

Pour du calculable/fiable (« combien puis-je en faire », taux de réussite) : opcode custom,
jamais de table codée en dur.

RE complète, adresses, layouts, règles serveur et cahier des charges QOL :
**`docs/make_item_list_re.md`**. IDB renommée et commentée (21 fonctions).
Voir [[reference_weapon_refine_re]] (même patron de portage), [[project_item_skill_desc_window_re]],
[[feedback_imgui_pitfalls]], [[feedback_native_replacement]].

**Plugin ImGui LIVRÉ** : `src/features/windows/make_item_window.{h,cc}`, groupe « Interface
moderne » (`makeitem_*`). Remplace les DEUX listes (94 et 79) ; ajoute id, stock possédé,
filtre, tri par en-tête, description au clic droit, et **écrit le résultat** (que le natif
compose puis jette). Le bug `result = 6` n'est pas recopié.

**Relance automatique : DEUX mécanismes**, parce que rien dans les paquets ne dit ce qui a
ouvert la liste (prouvé en live : `AC_MAKINGARROW` et `SA_CREATECON` donnent le même
`mk_type = 2`) :
- **compétence** — observée sur `CMode::SendMsg` cmd `0x45`/`0x71` (`game_mode.cc`) ;
- **OBJET** — observée sur l'envoi de **`CZ_USE_ITEM` `0x0439`** (`rag_connection.cc`), seul
  passage commun à tous les chemins d'usage. ⚠ le paquet ne porte que l'**index** : résoudre
  l'id À CET INSTANT, et relancer par **id** — jamais par l'index capté, il sera périmé.
  ⏱ **Index d'inventaire = `node+0x0C`** (= `ItemInfo+0x04`), CONFIRMÉ en jeu ; `item_info.h`
  ment en le déclarant à +0x08. Cf. [[feedback_re_method]].
  ✅ **Validé en jeu** : Mini Furnace (id 612) reconsommé, chaîne complète.

🔴 Relancer un OBJET le **DÉTRUIT** : `pc_useitem` fait `pc_delitem(..., LOG_TYPE_CONSUME)`
AVANT le script, donc chaque ouverture coûte un exemplaire même en cas d'annulation ou
d'échec. D'où réglage **séparé** (`makeitem_auto_reuse_item`, OFF), compteur à l'écran, arrêt
à la fermeture. ⚠ **PAS de plafond imposé** : `makeitem_auto_reuse_max` = **0 = illimité par
défaut** (le plafond est offert, pas subi — vider une pile est le choix du joueur). Une chaîne
illimitée ne peut pas s'emballer : chaque tour exige un résultat serveur pour armer le suivant,
donc elle CALE si le serveur ignore l'usage.
⚠ `RagnarokClient::UseItemById` n'avait **aucun appelant** avant ça : paquet forgé à valider
en jeu.

Captures live faites : fenêtre 94 sur `SA_CREATECON` (1 entrée, `12114`) **et** sur
`AC_MAKINGARROW` (5 entrées : 994/911/713/904/990, `rows=4` → `scroll_needed=1`, formule de
`UpdateScrollRange` vérifiée au chiffre près) ; fenêtre 79 sur `AM_PHARMACY` (3 entrées :
501/504/505 Red/White/Blue Potion, vecteur d'`int32` bruts) ; modale « liste vide » 0x018D.

🔴 **LE SERVEUR NE RÉPOND PAS sur la plupart des compétences.** Le `switch` de succès de
`skill_produce_mix` (skill.cpp ~13934) n'appelle `clif_produceeffect` que pour RK_RUNEMASTERY,
GN_MIX_COOKING, GN_MAKEBOMB, GN_S_PHARMACY, MT_M_MACHINE, BO_BIONIC_PHARMACY. **SA_CREATECON,
AM_PHARMACY, la forge BS_* et les flèches n'y sont PAS** → objet créé (`pc_additem`), AUCUN
`ZC 0x018F`. Et un refus est muet aussi (`return false` sans paquet quand
`skill_can_produce_mix` échoue à la revalidation, `clif_parse_SelectArrow` ignorant le retour).
⏱ Vérifié en jeu. Le natif ne le montrait pas : il referme sa fenêtre à l'envoi.
→ Le plugin relève le stock du produit AVANT l'envoi et le RELIT après 700 ms : hausse =
succès, ce qui arme la relance. Timeout 6 s sinon. **Correctif serveur souhaitable** (ajouter
les cases manquantes ou un `default:`), sans quoi un ÉCHEC reste indiscernable d'un refus.

🔴 **`make_per` est sur une échelle de 10000** (`rnd()%10000 < make_per`), PAS 100000 : un
`-= 2500` retire 25 points de %. Forge : Star Crumb −15 % chacun (+5 ATK), pierre élémentaire
**−25 %**, enclume **+10/+5/+2,5/+0 %** selon la meilleure possédée (`else if` en chaîne, jamais
un matériau — présence en sac suffit, ids 989/988/987/986). Max 3 emplacements ⇒ pire cas
2 Star Crumb + 1 pierre = −55 %.

🔴 **CUISINE : les 5 kits (12125-12129, `cooking 11;`..`15;`) NE sont PAS interchangeables.**
La LISTE est la même (le filtre traite `trigger` 11-20 en bloc) mais le TAUX dépend du kit —
`skill.cpp:13706`, dans le `default:` du switch (facile à manquer, pas de `case` nommé) :
`1200*(val-10) + 20*(base_lv+1) + 20*(dex+1) + 100*(…cook_mastery…) - 400*(itemlv-11+1)
- 10*(100-luk+1) - 500*(num-1) - 100*(rnd()%4+1)`, et **val ≥ 15 ⇒ 10000 = 100 % GARANTI**.
`cook_mastery` persisté (`COOKMASTERY_VAR`, 0-1999, `pc.cpp:2385`), ±au succès/échec (13879/14047),
exposé par `SP_COOKMASTERY`. ⚠ Leçon : j'avais vérifié le FILTRE puis conclu sur le TAUX.
→ **C'est le seul taux entièrement calculable côté client** (tous les termes sont accessibles ;
seul `rnd()%4` manque ⇒ afficher une fourchette). Meilleur candidat de l'Atlas ; demande la
table `kit → niveau` dans le YAML, extractible des scripts `cooking N;`.

✅ **CUISINE — chances LIVRÉES** (à tester). Trois singularités vérifiées, toutes contre-intuitives :
**aucun multiplicateur** (`pp_rate` est dans le `case AM_PHARMACY:`, `wp_rate` dans la branche forge —
ni l'un ni l'autre n'atteint le `default:` de la cuisine : seule fabrication à taux nu sur un serveur
tout-en-×5), **pas de pénalité baby**, et **aucune compétence** (les 60 recettes ont `req_skill = 0`,
d'où la chute dans le `default:`). Kits : 12125→11, 12126→12, 12127→13, 12128→14, **12129 (Fantastic)
→15 = 100 % garanti** (⚠ pas le « Royal », et le commentaire serveur « Legendary Cooking Set » ne
correspond à aucun item). ⚠ **12849 Combination Kit → `cooking 30;`** : hors [11,20] ET aucune recette
d'itemlv 30 → liste toujours vide, objet perdu. Objet mort.
🔴 Deux données hors paquets : le **niveau du kit** (serveur le garde dans `menuskill_val`, les 5 kits
donnent le même `mk_type = 1`) → déduit de l'objet consommé via `cooking_kits` du YAML ; et
**`cook_mastery`** (char reg, aucun chemin vanilla) → **opcode custom ZC 0x0F1C** créé, poussé au login
vérifié et à chaque `pc_setparam`. Il fallait : 22 points de % d'écart entre maîtrise 0 et 1999 (terme
`100*(rnd()%span + lo)`, `lo = 6 + cm/80`, `span = 30 + 5*(cm/400) - lo`, divisions ENTIÈRES à paliers).
Simulé : 29 % (kit 11, maîtrise 0) → 51 % (même perso, maîtrise 1999) ; 83 % (kit 14, plat moyen).
La maîtrise est affichée telle quelle — elle bouge à chaque plat et le jeu ne la montre NULLE PART.

**CUISINE (RE d'origine)** : `mk_type = 1` par `ZC 0x025A`
→ fenêtre 94 → `SendMsg(207, itemId, mk_type)`, recettes déjà dans `produce_db`. Elle vient
d'un **script d'OBJET** (l'ustensile) et pose `menuskill_id = AM_PHARMACY` → c'est la relance
par objet qui s'applique. ⚠ `mixcooking` ET `specialpharmacy` émettent **6** (le commentaire
annonçant 4 est périmé ; `clif_parse_Cooking` teste `== 6`). ⚠ Une requête peut fabriquer
`menuskill_val2` exemplaires d'un coup (**10** pour bombes/mix-cooking niv. > 1) : un rendement
> 1 n'est PAS forcément le tirage aléatoire maison. ⚠ `clif_cooking_list` refuse de renvoyer la
liste si `menuskill_id == skill_id` (« avoid resending »).

✅ **Fenêtre 80 SUPPRIMÉE** (et non « remplacée ») : `CMode::SendMsg(130, itemId,
ItemSkillInfo mats[3])` prend les 3 matériaux optionnels **en PARAMÈTRE** — la 80 ne faisait
que les collecter. On les remplit via `ItemSkillInfo_SetId` (**`0x006A6570`**, `__thiscall`,
`std_string_assign(this+44, itoa(id))`) et on n'ouvre plus jamais la fenêtre.
🔴 Correction d'une position antérieure de ce document : « la piloter ferait disparaître des
objets » était vrai, mais visait le mauvais verbe — ne PAS l'ouvrir est **plus sûr** que le
natif, dont les emplacements sortent réellement les objets de l'inventaire client (Annuler les
rend un par un ; tout autre chemin perd leur affichage). En envoyant nous-mêmes, rien ne bouge
côté client.
⚠ Matériaux acceptés (constantes serveur `itemdb.hpp`) : Star Crumb **1000** (max 3) et pierres
élémentaires **994-997** (la PREMIÈRE seulement, `ele == 0` en garde ; les suivantes ne sont
même pas consommées). Compromis affiché, que le natif tait : **+5 ATK et −15 % de réussite par
Star Crumb**. Effet de bord : la forge hérite de la relance auto et de la série ×N.

## 🔴 « Quelle compétence a ouvert cette liste ? » — heuristique temporelle INTERDITE

Rien dans les paquets de liste ne porte l'identifiant de compétence. La première règle
était « un lancement observé il y a moins de 3 s ⇒ c'est lui ». **Faux, et destructeur** :
⏱ constaté en jeu — le joueur lance le REFINE, utilise une Mini Furnace dans les 3 s,
`from_item_` devient faux, et la relance automatique rejoue `skill_id_` = **WS_WEAPONREFINE**.
La fenêtre de refine se réouvrait seule, sans qu'aucun bouton soit touché, et le cycle de
fabrication s'effondrait (chien de garde à 3 essais, chacun relançant le refine).

Règle correcte : **vérification POSITIVE** — la compétence observée doit être le `req_skill`
d'au moins un produit de la liste reçue (donnée du YAML, issue de `produce_db`). Sinon la
liste vient d'un OBJET, qui est le cas par défaut ET le cas sûr (le chemin objet ne relance
rien sans son réglage dédié). `AC_MAKINGARROW` se teste à part : sa liste énumère les
matériaux, pas des produits. Et quand `from_item_` est vrai, **`skill_id_` est remis à 0** —
plus aucun lancement fantôme n'est possible même si une autre règle se trompait.

Corollaires du même fait (un seul `menuskill_id` par personnage côté serveur) :
- une liste de fabrication chasse la session de refine → `CloseForOtherCraft()`, qui **ENVOIE**
  son `182 / -1` (rédaction antérieure : « sans envoyer, la session est déjà perdue » — FAUX,
  cf. la règle ci-dessous : une liste vide n'a rien écrasé, donc le refine est encore armé et
  lui seul peut s'effacer) ;
- l'inverse n'existe pas : le serveur refuse de lancer le refine pendant une fabrication.

## 🔴 Une liste `count == 0` n'a RIEN armé — ne rien lui laisser écraser

`clif_send` est **avant** le `if (count > 0)` : le serveur envoie donc des listes VIDES, et
celles-là ne posent aucun `menuskill_id`. La session précédente reste **vivante**, avec son
protocole d'annulation. ⏱ Bogue vécu : *« utiliser la furnace durant un skill de craft continu
bloque le fonctionnement des skills après fermeture »* — Arrow Crafting armé (`menuskill_id`
147), Mini Furnace → liste vide (le `// special case` écarte tous les métaux), le client adopte
`proto_ = kProduce`, l'annulation part en `CZ 0x018E` dont le parseur ne connaît pas
`AC_MAKINGARROW` (`default: return`, **aucun clear**) ⇒ menuskill armé à vie ⇒
`clif_parse_skill_toid` refuse **tout** lancement.

Garde en tête d'`OnRecvPacket`, **avant** toute écriture de membre (d'où `proto_`/`mk_type_`
calculés en locales) : `if (count <= 0 && list_armed_) return;` — on ne touche ni `proto_`, ni
`skill_id_`, ni `list_armed_`, ni `entries_`, ni le refine.

Force de désarmement des trois parseurs, la donnée qui décide de tout :
- `CZ 0x01AE` et `CZ 0x025B` : `clif_menuskill_clear` **hors** switch ⇒ effacent TOUJOURS ;
- `CZ 0x018E` : seulement `-1` / `AM_PHARMACY` / `RK_RUNEMASTERY` / `GC_RESEARCHNEWPOISON`.
  Suffisant quand même, car ce sont les **seules** sessions qui ouvrent la fenêtre 79 — à
  condition que `proto_` décrive la session ARMÉE. ⚠ Ne PAS envoyer un `0x01AE` « au cas où » :
  il effacerait un `MC_IDENTIFY` / `BS_REPAIRWEAPON` / `WS_WEAPONREFINE` / `SA_AUTOSPELL` en cours.

⚠ Les 4 compétences de forge (`BS_IRON` 94 … `BS_ORIDEOCON` 97) sont **passives** : jamais un
`menuskill_id`. Sur ce chemin c'est toujours `-1` (le `produce N;` du script d'objet) qu'on trouve
armé. C'est la raison d'être de la Mini Furnace.

🔴 **RIEN à patcher côté serveur, et c'est un revirement assumé.** Le natif est **immunisé** :
sur liste vide il affiche sa modale et n'ouvre AUCUNE fenêtre, donc il n'émet jamais le
`CZ 0x018E` qui bloque. Le bogue est entièrement de NOTRE fait (on ouvre une fenêtre annulable
là où il se contente d'un message). Le `default: return` est un idiome anti-paquet-forgé partagé
par 4 autres parseurs, et effacer dans le `else` détruirait des sessions légitimes
(`AM_PHARMACY` + furnace) tout en rendant le garde client mensonger. Détail : docs §7.0 bis.
Corollaire : `MC_IDENTIFY` / `BS_REPAIRWEAPON` **ne bloquent pas** — leur native est vivante et
leur propre parseur efface. Ce qui distinguait `AC_MAKINGARROW` : sa session était la nôtre et
sa native n'existe plus, donc plus rien ne pouvait l'effacer.

⚠ Gate serveur appliqué en revanche : `BaseJob == Job_Blacksmith` sur 612/613/614/615
(`db/import/items/item_db_usable.yml`). Vérifié : **aucune** recette d'ItemLv 1..3 (52, skills
98-104) ni 21 (7, skills 94-96) n'a `req_skill = 0` — un non-forgeron n'avait rien à fabriquer.
Effet de bord utile : `AC_MAKINGARROW`, `AM_PHARMACY` et `SA_CREATECON` deviennent inatteignables
en même temps qu'une furnace. Cf. [[reference_rathena_basejob_baseclass]].

⚠ `auto_stop_reason_` est une **`std::string`** ici et un **`const char*`** dans le refine :
`= nullptr` y est un crash (`std::string::operator=(char const*)` avec eax = 0). Utiliser
`.clear()`.
