# Méthode et rigueur de RE : les leçons payées

> Journal du chantier. La fiche de mémoire `feedback_re_method` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.
> Depuis le 2026-09-04, `feedback_re_method` est une fiche-INDEX : chaque leçon vit dans sa propre fiche `feedback_re_<slug>`.


Onze règles de méthode, chacune payée par un correctif faux livré à l'utilisateur.
Le fil rouge : **une conclusion tirée d'une lecture n'est pas une mesure.**

La règle mère est en fiche séparée :
[[feedback_absence_needs_measurement]] (une recherche vide ne prouve PAS
l'absence).

## 1. Pas de vérification circulaire

🔴 **Une vérification qui réutilise la valeur à expliquer ne vérifie RIEN.**

Vécu le 2026-08-02 (zeny encaissé de l'échoppe) : `MyShop+0xF0` valait 7 pour
30 555 z encaissés. Hypothèse « c'est la DERNIÈRE vente », « confirmée » par
`25 498 + 5 050 + 7 = 30 555`. Sauf que **25 498 n'était pas une observation** :
je l'avais obtenu en faisant `30 555 − 5 050 − 7`. La somme ne pouvait que
retomber juste. J'ai présenté ça comme une preuve (« l'arithmétique ferme le
dossier »), livré un correctif, et l'utilisateur l'a démenti en une vente : après
un objet vendu 250 000 z, le champ valait **toujours 7**.

- Avant d'écrire « ça confirme », demander **d'où vient chaque nombre**. Si l'un
  est calculé à partir du résultat attendu, l'accord est mécanique.
- Un test valable **fait varier** la chose : « refaire une vente et regarder si le
  champ bouge » l'aurait tranché en 10 secondes — c'est exactement ce qu'a fait
  l'utilisateur.
- Sur une valeur d'état, préférer **deux observations à des instants différents**
  à une reconstitution arithmétique d'un instantané.
- Quand on est démenti, reprendre depuis la **mesure**, pas depuis l'hypothèse
  suivante qui « colle » aussi.

## 2. Une affirmation d'audit qui justifie une SUPPRESSION se revérifie

`docs/moonlight_ui_audit.md` affirmait qu'au chargement des réglages « aucune
fenêtre de chat n'existe encore ». J'ai retiré le parcours sur cette base : le
fond du chat a cessé de se colorer au login. C'était faux —
`MoonlightUi::LoadSettings` n'a qu'UN appelant, `OnModeSwitch` à l'entrée en jeu,
donc après construction du HUD.

**Les audits produits par des agents qui n'exécutent pas le code** sont fiables
sur les constats de fait (adresses, comptages, lignes) et **pas** sur l'ORDRE
D'EXÉCUTION ni le cycle de vie des objets — or ce sont justement ceux-là qui
servent à justifier des suppressions.

🔴 **Corollaire : un scan n'est une preuve d'absence que s'il est ENTIÈREMENT
dépouillé.** En cherchant l'appelant de `CMode::SendMsg(149)` (capture de pet),
j'ai balayé `push 95h`, obtenu **trois** hits, ouvert **le premier** (faux
positif), classé les deux autres sans les regarder, et écrit « aucun appelant
trouvé ». Le deuxième, `0x008B997D`, ÉTAIT l'appelant
(`UIPetTamingDeceiveWnd_SendCatchRequest`). Le breakpoint l'a prouvé en trois
minutes.

⇒ Quand un scan rend N résultats : soit on ouvre les N, soit on écrit « N hits,
1 vérifié » — **jamais « aucun »**. Un doute sur un chemin d'exécution se tranche
au débogueur, pas par relecture.

⚠ La pile vue depuis une fonction hookée par Bourgeon renvoie d'abord dans
`ddraw.dll` (~0x56xxxxxx) — remonter un cadre de plus pour l'appelant réel.

## 3. Code mort = offsets non vérifiés

🔴 **Avant de bâtir sur un helper existant, vérifier qu'il a DÉJÀ un appelant.**
Sans appelant il n'a jamais tourné : ses offsets sont des hypothèses.

Cas prouvé : `RagnarokClient::UseItemById` → `Session::GetItemInfoById` →
`Session::item_list()`. Crash immédiat, `mov eax,[esi]` avec **`esi = 0`**.
`item_list_` est déclaré `+0x16D8` avec l'annotation « *LIKELY, xref pattern
matches std::list usage* » — une déduction jamais confirmée, qui a survécu des
années parce que la chaîne entière était morte.

Symptôme vicieux à reconnaître : sur un **hook d'envoi de paquet**, l'exception
coupe le hook AVANT l'appel natif → l'action du joueur ne fait plus rien *du
tout* (« le clic ne marche plus »), en plus du plantage.

1. `grep` les appelants AVANT d'utiliser. Zéro appelant = zéro garantie.
2. Lire les annotations du layout : `CONFIRMED` ≠ `LIKELY` ≠ `ESTIMATE`.
3. Préférer un chemin **déjà exercé en jeu** (ici le global d'inventaire
   `0x015FBAB0`, utilisé par la colonne « Possédé » de `make_item_window.cc`).
4. Tout hook sur un chemin natif chaud doit être **SEH** et purement
   observationnel : il ne doit jamais empêcher l'action native de partir.
5. Un helper cassé s'**annote à tous ses points d'entrée** — le prochain n'aura
   pas le crash pour indice.

⚠ `ItemInfo` (ragnarok/item_info.h) est lui aussi partiellement faux : `num_`
(+0x10) et `item_name_` (+0x2C) sont confirmés, mais l'index d'inventaire est à
**+0x04** (les deux premiers champs sont intervertis), PAS à +0x08.

**Technique pour trouver un offset sans deviner** : chercher un fait que le client
fournit lui-même. Le `CZ_USE_ITEM` émis par le joueur porte un index désignant un
objet encore présent ; on teste chaque offset candidat et on retient celui où
**une seule** ligne porte cette valeur — l'unicité écarte les coïncidences.

🔴 **Variante RE : du code lu dans le désassemblage ne prouve RIEN sur ce que le
joueur voit.** J'ai annoncé qu'un `ChatAction("[ (val1 == 20001) ]")` @0x00C8DB24
était « visible en jeu ». L'utilisateur a objecté que personne ne l'avait jamais
vu — il avait raison : la branche n'est atteinte que sur réception de **ZC
0x01F8**, or côté moonlight cet opcode n'est que *déclaré*, sans émetteur.
➡ Avant d'écrire qu'un message/effet/fenêtre « apparaît », remonter jusqu'à un
**émetteur serveur vivant** : `grep` l'opcode dans `moonlight/src/map` et vérifier
qu'une fonction l'ENVOIE (pas seulement que `packet(...)` le déclare, et pas dans
un bloc commenté). Sinon écrire « présent dans le binaire, chemin non atteint ».

## 4. Un doc de RE corrigé laisse le code FAUX

`docs/entity_context_menu_re.md` avait reçu un encadré « Correction — la
catégorie 3 est le PET, pas un objet au sol ». Le doc était juste, le mémo
aussi… mais `entity_context_menu.cc` gardait `constexpr int kPickGroundItem = 3`
avec son test placé **avant** celui du pet. Les cinq commandes de pet ont disparu
du menu pendant des mois.

La correction avait été écrite là où la RE se range, pas là où elle s'exécute.
Une constante mal nommée survit à la correction du doc : le compilateur est
content, le nom se lit bien, et le bug ne se voit que sur une entité rarement
testée.

➡ Après tout encadré 🔴 « Correction » dans un `docs/*_re.md`, **`grep` le code
sur ce que la correction invalide** — la constante, la valeur, le nom. Et dans
l'autre sens : une catégorie relevée du binaire doit citer **l'adresse qui
l'écrit**, pour que la revérifier soit un seul `decompile`.

## 5. Aller voir là où ça MARCHE DÉJÀ

Reproche formulé deux fois le 2026-08-02 : « encore une fois tu bricoles au lieu
d'aller voir où ça fonctionne déjà », puis « le doll de la character sheet a
résolu tous ces problèmes d'animation, d'ancre etc, donc tu peux simplement
étudier le code là-bas et regarder le binaire quand c'est du hook ».

**Où ça marche déjà, dans ce projet :**
1. **`BasicInfo::RenderPlayerAvatar` / la fiche de personnage** — ⚠ elle ne
   COMPOSE pas, elle CAPTURE les quads d'un acteur rendu par le client. La
   logique n'est donc pas dans notre code : elle est dans la fonction native
   hookée, **`Actor_DrawSprites` 0x007AC820**. C'est ÇA qu'il faut lire.
2. **Le site Moonlight** (`functions_moonlight.php`, `getcharimage`) — composeur
   maison écrit par l'utilisateur. ⚠ Image FIXE : bon pour les chemins, les
   palettes, les ancres au repos ; muet sur l'animation.
3. **GRFEditor / ActEditor** pour les FORMATS — cf.
   [[reference_grf_act_spr_reference_impl]].

Ce que la lecture de `Actor_DrawSprites` a donné : les coiffes (parties 3-6)
prennent pour référence la **partie 2 = la TÊTE**, pas le corps, et comparent les
ancres des images COURANTES ; le natif passe l'action **telle quelle** à
`Act_GetFrame` (pas de `action %= action_count` comme le site).

➡ Avant de toucher à un réglage : « ce comportement marche-t-il DÉJÀ quelque
part ? ». Si oui, lire cette implémentation — et si c'est un hook, lire la
fonction **native**, pas notre code. Si deux causes restent possibles, poser une
**mesure** plutôt que corriger l'une des deux. **Chaque essai coûte un build à
l'utilisateur.**

## 6. Lire TOUTE la sortie d'un vérificateur

🔴 Filtrer la sortie sur les motifs qu'on attend revient à ne vérifier que ce à
quoi on pensait déjà — or un vérificateur ne sert qu'à trouver ce qu'on n'avait
pas prévu.

`tools/lang/check_catalog.ps1` affiche `SANS traduction`, `ORPHELINES`,
`valeurs VIDES` **et `DOUBLONS`**. J'ai filtré sur les trois premiers toute une
session et déployé un `en.yaml` avec deux clés répétées. **yaml-cpp refuse un
doublon et rejette le fichier ENTIER** : toute l'interface est repassée en
français. L'outil le disait, ligne 119.

⚠ Deux pièges aggravants : `exit 1` était permanent (110 orphelines légitimes),
donc le code de retour ne signalait rien de neuf ; et **PyYAML accepte les clés
répétées** en gardant la dernière — une validation `yaml.safe_load` ne voit RIEN
de ce que yaml-cpp refusera. Vrai pour tous les YAML du projet.

➡ Lancer sans filtre, ou n'exclure que le détail volumineux
(`Where-Object { $_ -notmatch '^\s+- ' }` garde les compteurs, coupe les listes).

## 7. Une question à l'utilisateur TERMINE le tour

🔴 Reproché le 2026-08-03 : « Tu me pose des question sans me laisser y répondre,
puis tu repart coder des choses, ça ne fais que provoquer des imbroglios ».

Le coût est réel : la réponse arrive dans un contexte qui a déjà bougé ; je
produis des correctifs fondés sur l'hypothèse que la question devait justement
tester ; l'utilisateur doit arbitrer entre plusieurs fils au lieu de répondre à
un seul.

➡ Ne poser une question QUE si la réponse change ce que je vais faire — sinon
choisir une hypothèse, l'annoncer, et continuer. Et si je la pose, m'arrêter.

## 8. Penser comme un joueur ÉTOURDI ou CURIEUX

Règle posée par l'utilisateur le 2026-08-01, après deux bugs de ce type
d'affilée. Les deux vraies pannes de la campagne « tuer le natif » ne venaient pas
du chemin nominal, qui marchait du premier coup, mais de manipulations latérales :
basculer « Interface moderne » **pendant** un dialogue NPC (personnage bloqué en
script serveur), l'éteindre **pendant** une boutique (plus aucune fenêtre).

Check-list qui a effectivement trouvé des bugs :
1. **Basculer chaque réglage dans les DEUX SENS, au pire moment.** Le sens
   OFF → ON est le point aveugle : on n'a vu aucun paquet de la session en cours,
   tout notre modèle est vide.
   🔴 **Sur une bascule, FERMER — jamais rouvrir l'équivalent.** Tranché par
   l'utilisateur : « que le joueur se retrouve devant rien en basculant, pour moi
   c'est plus safe que tenter de rouvrir l'équivalent ». Rouvrir signifie
   fabriquer une session qu'on n'a jamais observée. C'est sa CONSTANCE qui rend la
   règle fiable (j'avais fait « reprendre » la banque, soldes à jour : refusé, et
   à raison). Ce qu'il faut garantir : qu'aucun état ne reste BLOQUÉ, des deux
   côtés.
2. **Cliquer deux fois, vite** — anti-double-envoi partout où un paquet part.
3. **Fermer / warp / changer de personnage au milieu** : qui remet l'état à zéro,
   client ET serveur ?
4. **Qui d'autre utilise le même opcode ?** CZ_CLOSE_DIALOG 0x0146 sert au
   dialogue ET à la boutique : un filtre posé « en permanence » sabotait l'autre.
5. Un état qui n'est plus adossé à une fenêtre native que le client détruisait
   pour nous, **c'est à nous de le nettoyer**.

## 9. Le client voit des données RÉÉCRITES par `clif_*`

**Lire la structure de stockage de rAthena ne prouve rien sur ce qui arrive au
client.** Le sérialiseur a le droit de réécrire les champs — et il le fait.

Cas vécu (pet, 2026-08-10) : `item.card[0..3]` d'un œuf portent côté serveur
`CARD0_PET` + les deux mots du `pet_id` + `renommé | rang << 1`. J'ai décodé
exactement ça côté client… et tout est faux : `clif_addcards`
(`moonlight/src/map/clif.cpp:2864`) met `card[0]`/`card[1]` à **zéro** et ne
transmet que `card[3] >> 1` et `card[3] & 1`. Livré au joueur : « œuf vierge :
aucun familier dedans » sur un pet nommé, et un rang d'intimité affiché comme un
id d'objet.

Le symptôme (« la donnée est vide / absurde ») ressemble à un mauvais offset côté
client et fait chercher au mauvais endroit longtemps.

➡ Avant de décoder quoi que ce soit, **remonter au `clif_*` qui l'émet** et lire
ce qu'il écrit dans le buffer, champ par champ. `grep` le nom de la constante
serveur dans `clif.cpp` : si elle y apparaît, c'est justement qu'elle est traitée
à part. Si la valeur attendue n'arrive jamais, **ne pas « assouplir » le test —
corriger le format**. Et ce qui n'est pas sur le fil n'existe pas pour le client
(ici le `pet_id`) : ne pas prévoir d'affichage pour ça.

## 10. Documenter dans IDA : renommer ET commenter

⚠ Outil de RE de référence = **IDA Pro** (MCP `ida-pro-mcp`). Ghidra n'est plus
utilisé.

Toute trouvaille se matérialise par **deux** gestes : (1) **renommer** le symbole
(`sub_*` → nom parlant, `dword_*`/`byte_*` → nom parlant) ET (2) **commenter**
(`set_comments`, qui pose à la fois en désassemblage et en décompilateur) avec
rôle + source de la valeur + callers/leviers. L'IDB est le dépôt durable ; le chat
est perdu au prochain contexte. Un symbole renommé sans commentaire perd le
« pourquoi » ; un commentaire sans renommage laisse le symbole illisible ailleurs.

🔴 **`set_comments` TRONQUE à 1024 caractères, EN SILENCE** (le tool rend
`{"addr": ...}` comme si tout allait bien) — et c'est justement la fin, donc la
conclusion, qui saute. Deux règles : **conclusion et avertissement EN TÊTE**, et
relire la longueur après la pose (`len(idc.get_cmt(ea, 0))` ; `>= 1024` =
tronqué). Viser ~950.

🔴 **Un commentaire d'IDB peut être FAUX.** Vérifié le 2026-08-08 : celui de
`RecvLoop_DispatchPackets` (0xc9df00) annonçait un tail-call, et c'était
l'inverse — il décrivait le bug de *notre* hook, pas le client. Coût : 1-2 s de
latence sur `@storeall`. Un commentaire d'IDB est une note d'une session passée,
pas une mesure.

⚠ Le MCP IDA peut **timeout** sur `search_text` pleine largeur : scoper avec
start/end, ou préférer `func_query` / `find_regex` / `xrefs_to` / `decompile`.
`server_health` vérifie que le pont répond.

🔴🔴 **`decompile` sur une adresse de `case` décompile TOUT le dispatcher.**
Mesuré le 2026-09-02 : `decompile(0x00ca89e0)` — un corps de `case` de
`RecvLoop_DispatchPackets` (**0xc3dd octets**) — a fait **expirer le pont**, qui
est resté occupé plusieurs minutes ensuite (`server_health` lui-même expirait).
`lookup_funcs` sur l'adresse dit à quelle fonction elle appartient : le faire
**avant**. Sur un corps de `case`, désassembler une **plage bornée**
(`ida_lines.generate_disasm_line` via `py_eval`) — jamais `decompile`.
⚠ L'utilisateur l'a rappelé le même jour : **jamais plus d'un intervenant sur
IDA**, sinon ça expire. Donc pas de sous-agent sur l'IDB, et pas d'appels MCP
en parallèle.

## 11. 🔴🔴 DEUX correctifs faux = on passe au DÉBOGUEUR

Posé le 2026-08-22, après **trois** correctifs faux d'affilée sur « masquer la
barre HP/SP sous le personnage » — trois builds coûtés à l'utilisateur, qui a
fini par dire « à mon avis tu regardes pas au bon endroit ». Il avait raison à
chaque coup.

Chaque hypothèse était bien construite ET fausse :

1. « le slot est `+0x488` » — vrai pour la classe de BASE, mais le joueur n'y est
   pas ;
2. « il y a TROIS classes d'acteur, sondons les trois slots » — vrai aussi
   (`+0x428`, `+0x470`, `+0x488`), et toujours à côté ;
3. « la visibilité est réaffirmée chaque frame, il faut un détour » — vrai pour
   les jauges d'**acteur** (`+0x300`, reposées par
   `CActorSprite_UpdateAttachedSprite` `0x00C46B60`), sans rapport avec celle-ci.

Ce qui a tranché en dix minutes : **lire le processus vivant**. Parcourir la liste
de fenêtres du `UIWindowMgr` (`+0x17C`), relever vtables et positions. Deux faits
qu'aucune lecture statique n'aurait donnés : l'acteur du joueur ne porte **aucune**
jauge (tous les slots nuls — l'objet propriétaire est une autre instance), et la
barre est une **fenêtre autonome** d'une TROISIÈME classe, `UIPlayerGage`
(vtable `0x0102BD78`, ctor `0x00836530`, 60x9). Puis **écrire** son `+0x28` à 0
en direct pour prouver le remède AVANT de coder — l'utilisateur a confirmé à
l'écran.

➡ **Règle : au DEUXIÈME échec sur le même symptôme, on n'émet pas une troisième
hypothèse — on MESURE.** x32dbg s'ouvre en deux minutes ; un build coûte bien
plus, et il le coûte à l'utilisateur. Corollaires :

- **Une déduction qui « se tient » n'est pas une mesure** — les trois se
  tenaient. C'est [[feedback_absence_needs_measurement]] appliqué aux correctifs.
- **Prouver le remède en direct** (écriture mémoire) avant d'écrire la ligne de
  code : ça transforme « je crois » en « l'utilisateur l'a vu ».
- **Un précédent qui marche ne prouve RIEN pour le voisin** : la barre
  d'incantation (`acteur+0x270`) se masque par simple écriture de `+0x28`, ce qui
  m'a fait croire que la recette valait partout. Personne ne réaffirme celle-là.
- **Une garde de type doit couvrir les DÉRIVÉES** : tester
  `vtable == UIPcGage` rejetait `UIMonsterGage`, qui en hérite. Une garde trop
  étroite ne protège pas, elle aveugle.
- **Ne pas présenter une inférence comme un fait.** J'avais annoncé
  « l'interrupteur natif Alt+End » : c'était une ligne d'aide du catalogue
  msgstring, jamais reliée au drapeau `GameSettings_GetFlag(0xD5)`. L'utilisateur
  a relevé le « ?? ».

⏱ **Et le JOURNAL vaut le débogueur quand le défaut est dans NOTRE logique.**
Sur le « cercle de visée qui reste armé » (même jour, QuickCast), deux hypothèses
fausses, puis une trace `LogDiag` une-fois-par-cause a nommé le coupable au
premier essai : un appui MAINTENU produit un second `0x48` ~500 ms plus tard
(auto-répétition système) et `TakePendingKey` ne rend la touche qu'au PREMIER —
alors que l'en-tête du module affirmait depuis toujours que « le client ignore
l'auto-répétition clavier ». **Un commentaire de notre PROPRE code est une note,
pas une mesure** — exactement comme un commentaire d'IDB (§10).

## 🔴🔴 AVANT D'ANNONCER UN VOLUME : OUVRIR CINQ CANDIDATS

Le 2026-08-26, **trois détecteurs annoncés coup sur coup, trois gisements qui
n'existaient pas** :

| annoncé | réel | ce que le détecteur comptait en trop |
|---|---|---|
| « 920 adresses en dur » | **407** | les adresses CITÉES en commentaire (475 le sont uniquement là) |
| « 149 sites d'adresses nues » | **1** | commentaires de fin de ligne, chaînes de log, entrées de tableau documentées, `kNom[4]` que le motif ne reconnaissait pas |
| assertions techniques (« dit SEH, a-t-il `__try` ? ») | 0 exploitable | un `.h` déclare, le `.cc` implémente — 110 faux positifs structurels ; et une logique inversée sur « ASCII » |

Le point commun n'est pas la regex : c'est d'avoir **donné le chiffre du relevé
brut comme s'il était le chiffre du gisement**. Un détecteur neuf n'a aucun taux
de vrais positifs connu.

➡ **La règle** : entre le relevé et l'annonce, **ouvrir cinq candidats au hasard
et compter les vrais**. Puis annoncer « N candidats, dont ~X % de vrais d'après
un échantillon de 5 », jamais « N sites à corriger ».
➡ Corollaire de mesure : **neutraliser commentaires ET chaînes** avant de
compter quoi que ce soit dans du code — sinon on compte la documentation.

⚠ Ce que ces trois échecs disent du projet, et qui est une bonne nouvelle :
après vingt tranches, **les chantiers mécaniques sont épuisés**. Ce qui reste
demande du jugement, donc une passe humaine sur une liste courte — pas un
détecteur de plus.

## OÙ VA LA CONNAISSANCE : code, `docs/`, ou IDB (mesuré le 2026-08-26)

Question de l'utilisateur : « pourquoi ne pas tout mettre dans une documentation,
et l'IDB ne suffit-elle pas ? ». Réponse chiffrée, pour ne pas refaire l'analyse.

**Le projet le fait DÉJÀ** : `docs/` = **53 documents, 32 391 lignes**, dont
**37 cités depuis 104 fichiers source** — ~37 % de la documentation totale est
déjà hors du code, avec le bon patron (RE détaillée dans `docs/<sujet>_re.md`,
renvoi d'une ligne depuis le code). ⚠ **16 documents sur 53 ne sont cités par
aucune source** : à trier un jour (blueprints légitimes, ou orphelins).

**Nature des 54 895 lignes de commentaire de `src/`** — explication simple
23 920 (44 %) · contrat ⚠/⛔ 14 624 (27 %) · contrat+RE 4 491 (8 %) ·
**RE pure 3 758 (7 %)** · histoire 5 102 (9 %).

🔴 **L'IDB ne remplacerait que les 7 %.** Elle dit *ce que fait le natif* ; le
code dit *pourquoi on le patche ici plutôt qu'ailleurs*. Elle confirme qu'à
`0x0093f1f9` il y a `SUB EAX,0x26` ; elle ne dit pas que c'est **l'un des huit
sites à patcher ENSEMBLE**. Et [[reference_ida_is_vanilla_warp_patches]] :
l'IDB est l'exe VANILLA — elle ne décrit même pas le binaire qui tourne.

➡ **LE CRITÈRE**, déjà formulé pour les patchs WARP et qui vaut partout :
**« un document ne se lit que si l'on pense à l'ouvrir, un commentaire tombe
sous les yeux au bon moment. »** Le CONTRAT reste au code (lu par qui modifie la
ligne) · la RE DÉTAILLÉE va dans `docs/` · l'HISTOIRE ne se justifie que si elle
empêche de refaire l'erreur.

⚠ En déplacer plus se paie : **doc corrigée ≠ code corrigé**, leçon déjà payée
ici. Et le vrai volume — les 44 % d'explication simple — ne se déplace pas : il
se juge, fichier par fichier.


---

Voir aussi [[feedback_absence_needs_measurement]],
[[feedback_debug_tooling]], [[feedback_native_hooking]].

🔴🔴 **`find type=immediate` d'IDA ne prouve RIEN sur les accès à un champ.**
Il ne voit **que les vrais opérandes immédiats** — `mov [ecx+1344h], eax` et
`mov eax, [esi+1180h]` lui sont **invisibles**. J'en ai conclu à tort que
personne ne lisait un tableau de `CNavigation`, alors que son lecteur était
appelé à chaque frame. **Utiliser `search_text` sur le listing rendu, borné par
plage** (`start`/`end`) : rapide et complet ; sans bornes il expire, et une
expiration laisse le serveur MCP occupé pour l'appel suivant.
Deux corollaires : le compilateur **replie les index** (`this + 24*i + 4480`
devient `this + 8*(3*i + 560)`, où `4480` n'apparaît plus), et la vraie porte
d'entrée est souvent la **liste des xrefs au singleton** — elle est bornée, elle
est complète, et elle nomme d'un coup les sous-systèmes qui le touchent.
**Voir aussi [[feedback_absence_needs_measurement]] : une recherche vide ne
prouve pas l'absence, surtout quand l'outil ne cherche pas ce qu'on croit.**

## 🔴🔴 Un chiffre « plausible » ne prouve rien sans TÉMOIN NÉGATIF

Mesure du 2026-08-27 (portage 2025→2026 par opcodes). J'allais annoncer
« mediane du ratio de tailles = 0,98, donc les paires sont bonnes ». Ca ne
prouve RIEN tant qu'on ignore ce que donnerait un appariement **au hasard**.

En melangeant les memes adresses (20 tirages) :

| | mediane | > 0,8 |
|---|---|---|
| paires proposees | **0,981** | **86,3 %** |
| **temoin aleatoire** | 0,291 | 10,4 % |

➡ **C'est l'ECART qui fait la preuve, jamais la valeur seule.** Construire le
temoin coute 5 lignes (`random.shuffle` sur les cibles) et transforme une
impression en mesure. A faire pour tout score, toute heuristique, tout
appariement. Meme famille que « un controle qui passe TOUJOURS ne prouve rien »
(cf. [[feedback_code_hygiene]]).

## 🔴 Valider par recoupement : verifier d'abord qu'il Y A recouvrement

Toujours le 2026-08-27 : j'ai voulu valider 623 paires en les confrontant a un
portage anterieur de 157 adresses. **Recouvrement : 4.** Un accord sur 4 cas ne
valide pas 623 — et un desaccord n'aurait pas plus infirme.

➡ **Mesurer la TAILLE du recouvrement avant d'en tirer une conclusion.** S'il
est faible, ce n'est pas une validation mais un apport complementaire : il faut
alors un test independant (ici : tailles contre temoin aleatoire, et noms
presents des deux cotes).

## 🔴 Chercher l'IDENTIFIANT QUI NE BOUGE PAS

Pour rapprocher deux versions d'un binaire, ne pas partir de ce que le
compilateur reecrit (octets, prologues, adresses). Chercher ce qu'une contrainte
EXTERNE fige : un opcode de protocole, un index de vtable (RTTI), un id de
message, un nom de fichier. Le dispatcher de paquets a ainsi donne 623 paires la
ou la signature d'octets en donnait 4 sur 18.

## 🔴🔴 Un seuil mal place ne rend pas un test muet, il le rend MENTEUR

Mesure du 2026-08-27. Question : les ids de fenetre sont-ils stables entre deux
builds ? Test = « proportion d'ids dont le case a une taille comparable
(ratio > 0,8) », balaye sur plusieurs decalages.

Verdict rendu : **le bon alignement (decalage 0) scorait 1,9 %, contre ~20 %
pour TOUS les autres decalages.** Conclusion apparente : « les ids sont
decales » — exactement l'inverse de la verite.

Cause : le code 2026 est plus compact (mediane **131** instructions par case
contre **187**), donc le ratio vaut ~0,70 pour *tous* les ids et passe sous le
seuil de 0,8 — mais seulement quand l'alignement est BON (aux mauvais
alignements, on compare au hasard et quelques paires depassent 0,8).

➡ **Un seuil arbitraire sur une grandeur dont on n'a pas mesure la
DISTRIBUTION peut inverser la conclusion.** Regarder la distribution (mediane)
avant de poser un seuil — et si le test ne discrimine pas, en changer.

Ici le bon test etait **l'ORDRE**, pas la taille : le compilateur emet les cases
dans l'ordre du source, donc la correlation de rang des adresses de case donne
**0,90 au decalage 0 contre ~0,50 ailleurs**. Sans ambiguite.

## ✅ Deux identifiants stables valent mieux qu'un : ils se VALIDENT

Toujours le 2026-08-27. Deux tables sans le moindre rapport — opcodes de
protocole (`RecvLoop_DispatchPackets`) et ids de fenetre (`MakeWindow`) —
appariees separement par co-occurrence, puis croisees :
**9 recouvrements, 9 concordances, 0 divergence.**

➡ Quand une methode n'a pas de reference externe pour se valider, **en
appliquer une SECONDE sur un identifiant independant** et croiser les
intersections. C'est ce qui manquait au premier vecteur, dont le recoupement
avec un portage anterieur ne portait que sur 4 adresses.

🔴 **Piege d'API IDA** : un switch a table d'INDIRECTION (N valeurs vers M
cibles, M < N) a un `si.lowcase` **inexploitable** (il rend une adresse).
Utiliser **`idaapi.calc_switch_cases(head, si)`**, qui rend les vraies valeurs
de case quel que soit le type de switch.

## 🔴🔴 Confirmation croisée : compter les sources INDEPENDANTES, pas les passes

Suite du 2026-08-27. Trois passes d'appariement fusionnees : **756 paires
« confirmées par au moins deux passes, 0 conflit »**. J'allais l'annoncer comme
une validation massive. C'est faux.

La troisieme passe **CONTENAIT** les tables des deux premieres. Un accord entre
elles ne teste que la **robustesse aux parametres** (profondeur d'appel, taille
de cache) — pas l'independance. C'est de la **REDONDANCE deguisee en
confirmation**.

En ne comptant que les accords entre **familles de tables disjointes**
(reseau / ui / effet / skill / item) : **42 paires, 0 divergence** — 4 %, pas
64 %.

➡ Avant d'annoncer « confirme par N methodes », **verifier que les N methodes ne
partagent pas leur source**. Deux mesures tirees de la meme donnee ne se
valident pas l'une l'autre, quelle que soit la difference de traitement.
Meme famille que la « verification circulaire » deja notee plus haut.

## ✅ Le 3e controle d'un appariement de binaires : la MONOTONIE

Ni les tailles, ni les noms, ni les recoupements n'y entrent — c'est ce qui en
fait un test independant. Le compilateur reordonne des fonctions d'un build a
l'autre, mais pas au hasard : **l'ordre est largement preserve**.

Mesure du 2026-08-27 sur 1181 paires :

| | paires proposees | temoin aleatoire |
|---|---|---|
| inversions d'ordre entre voisines | **4,1 %** | 50,2 % |

En fenetre glissante (4 voisines de chaque cote) : **8 paires sur 1181 sortent
de l'enveloppe (0,7 %)**, aucune utilisee par Bourgeon.

➡ **La batterie complete pour valider un appariement**, chaque test aveugle a
ce que les autres mesurent : (1) **ratio de tailles** contre temoin aleatoire ;
(2) **monotonie** contre temoin aleatoire ; (3) **noms** presents des deux cotes ;
(4) **accord entre familles de sources disjointes**. Plus (5) **collision** :
deux sources pour une cible = au moins une erreur.

## 🔴 Une collision accuse DEUX entrees : les tailles disent laquelle tombe

Corollaire mesure le 2026-08-27. `operator_delete` (0xE) et `SkillMgr_SetOption`
(0xFA) revendiquaient la meme cible de **0xE octets**. La collision les signalait
toutes les deux ; seule la comparaison des tailles montre que la premiere est
JUSTE et la seconde fausse.

➡ Ne jamais rejeter les deux membres d'une collision en bloc — en general une
seule est fausse.

## ✅✅ Les ACCESSEURS TRIVIAUX : l'offset d'une structure, EN CLAIR

Mesure du 2026-08-27. Pour savoir si une zone d'une structure s'est decalee
entre deux builds, le meilleur levier est une micro-fonction de 7 octets :

```
8B 81 dd dd dd dd    mov eax, [ecx+disp32]
C3                   retn
```

Le compilateur en emet **beaucoup** (141 en 2025, 127 en 2026), **contigues** et
**dans l'ordre de declaration**. Donc :

1. le **deplacement est en clair** dans le code ;
2. la suite des **ecarts** entre deplacements d'un bloc est une **empreinte
   invariante au decalage**, tres discriminante ;
3. apparier deux blocs par cette empreinte donne d'un coup **tous** leurs
   accesseurs ET le **decalage de la structure** (0 si elle n'a pas bouge).

Mesure : 8 blocs apparies, 41 accesseurs, **0 divergence**. **6 structures sur
8 n'avaient PAS bouge** entre 2025 et 2026.

➡ C'est ce qui a **mesure** les trois listes d'objets de la session
(`-0x38` confirme sur 14 accesseurs consecutifs) la ou tout le reste
extrapolait. Motif a chercher : `8B 81 ?? ?? ?? ?? C3` et `8B 41 ?? C3`.

## 🔴🔴 Un DEPLACEMENT NE PORTE PAS SA BASE

Meme jour, la tentative qui a echoue AVANT de trouver la bonne. Pour dater le
decalage d'une structure, j'ai compare le profil des deplacements `[reg+disp]`
chez les fonctions qui referencent la globale :

| plage | meilleur decalage | attendu |
|---|---|---|
| 0x1000..0x1500 | −8 | **−56** ❌ |
| 0x1500..0x1800 | −0x38 | −56 ✅ |

Une plage donnait la bonne reponse, l'autre non. **Cause : ces fonctions
manipulent PLUSIEURS structures** (celle de l'acteur est decalee de −8), et un
deplacement ne dit pas a quelle base il se rapporte. Le profil melangeait tout ;
le « pic » pouvait n'etre qu'un artefact.

➡ **Ne jamais agreger des deplacements sans savoir de quelle BASE ils
partent.** Passer par des fonctions ou la base est certaine — les accesseurs
triviaux le garantissent (`ecx` = l'objet, rien d'autre dans la fonction).

## ✅✅ Valider un appariement de vtables : le nom auto `Classe__vf_NN`

Mesure du 2026-08-27. Apparier des methodes virtuelles par **(classe, slot)** est
sur quand les deux vtables ont le meme nombre de slots ; ca devient risque quand
il differe (une methode inseree au milieu decale tout ce qui suit).

🔴 **IDA nomme les methodes anonymes `Classe__vf_NN`, ou `NN` est l'offset du
slot en HEXA.** Si l'index de slot vient d'un parcours independant de la vtable,
confronter les deux est un test **gratuit et parfaitement independant** :

> **2164 paires testables, 2164 concordent, 0 divergence** — et exclusivement
> sur le mode risque.

➡ Chercher toujours si l'outil a deja encode l'information qu'on veut verifier
sous une autre forme. Ici le verificateur etait deja dans l'IDB.

## 🔴🔴 Comparer des TABLES ne dit rien des TRAITEMENTS

Mesure du 2026-08-27, sur un portage de client entre deux versions.

J'avais compare les tables de longueurs de paquets et conclu « aucun opcode
utilise ne change ». Exact. Et parfaitement trompeur : l'opcode fautif avait la
meme longueur, son `case` existait toujours, il appelait bien sa fonction de
parsing — mais dans cette fonction, les branches concernees etaient devenues un
**`break` nu**. Recu, ignore, aucune erreur affichee.

➡ **Quand on valide une compatibilite, mesurer ce qui est EXECUTE, pas ce qui
est DECLARE.** Le detecteur qui marche : pour chaque entree d'un dispatch,
comparer le VOLUME DE CODE atteint (instructions + poids des fonctions appelees)
entre les deux versions, et signaler les effondrements.

⚠ Et ne pas confondre le detecteur avec une preuve : le poids est approximatif
(un case qui delegue a une grosse fonction pese peu sans rien perdre). Il donne
des CANDIDATS a verifier en lisant le calcul, pas des verdicts.
