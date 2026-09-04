# Remplacer une fenêtre ou un handler natif en ImGui

> Journal du chantier. La fiche de mémoire `feedback_native_replacement` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.


Tout ce qui concerne **prendre la place** d'une fenêtre ou d'un handler du
client. Le fil rouge : une native qu'on masque n'est pas partie — elle garde son
rect, son clavier, et ses devoirs.

Pour *poser* un détour, voir [[feedback_native_hooking]].

## 1. Remplacer, c'est COMBLER les manques — pas reproduire

🔴 Dit explicitement par l'utilisateur (2026-08-05), après que j'aie défendu
plusieurs choix par « c'est la condition du natif à l'identique ». **Le
comportement natif est une observation, jamais une justification.** Si on se
contente de reproduire, le remplacement n'apporte rien et hérite des pièges du
client.

- RE le natif pour savoir ce qu'il fait **et où il se trompe**, pas pour le
  copier.
- Une action que le SERVEUR va refuser ne doit pas être cliquable : la griser
  avec la raison exacte en infobulle (cf.
  [[feedback_ui_conventions]]). Vérifier la règle dans la source
  moonlight, pas la deviner.
- Le natif qui *retire* une entrée quand elle n'a pas de sens est un **manque** :
  le joueur ne distingue pas « impossible » de « pas encore chargé ». Griser et
  expliquer, plutôt que faire disparaître.
- Ajouter ce que le natif ne disait pas (qui est visé, pourquoi c'est refusé).
- **Fidélité au MÉCANISME, liberté sur l'ERGONOMIE** : rejouer les actions par le
  dispatcher du client plutôt que refabriquer des paquets.

## 2. Un handler remplacé hérite de TOUS ses devoirs

Avant `RegisterReplaceOpcode`, répondre aux **trois** questions — pas seulement à
la première. Règle dégagée le 2026-08-01 après l'avoir apprise quatre fois de
suite, chaque fois par un bug en jeu.

1. **Qu'est-ce qu'il ÉCRIT ?** Globales, drapeaux, messages de chat.
   *Ex. :* `GameMode+0x24C` (interaction NPC), `g_BankVault`.
   ⚠ Et symétriquement **ce qu'il n'écrit pas** : la boutique NPC écrasait
   `GameMode+0x2DC`, le GID de la CONVERSATION, et la fermeture partait au
   mauvais NPC.
2. **Qu'est-ce qu'il EMPÊCHE ?** Un handler porte souvent un garde-fou purement
   client, que le serveur n'applique pas. *Ex. :* le verrou « ne pas vendre les
   favoris » vivait dans le ctor de la liste de vente NPC (0x00CD0F00) ; le
   plafond de 10 objets de l'échange vivait dans `Session_AddMyDealItem` — et le
   serveur sort SANS RIEN DIRE au onzième, son propre commentaire disant que le
   message incombe au client.
3. **Qu'est-ce que le SERVEUR suppose qu'il a fait ?** La plus coûteuse, parce
   qu'elle ne se voit pas dans le client. *Ex. :* le handler d'acquittement d'un
   ajout à l'échange RETIRAIT l'objet du sac ; au commit `pc_delitem(..., type=1)`
   ne notifie pas, et à l'annulation `trade_tradecancel` renvoie `clif_additem`.
   Ne pas retirer donnait un stack fantôme dans un cas et des objets EN DOUBLE
   dans l'autre.
   ⚠ J'avais choisi une déduction « à l'affichage seulement », plus simple et
   réversible — incompatible avec le protocole. **Quand le natif fait une
   mutation, la refaire ailleurs ou plus tard n'est pas équivalent.**

**Méthode :** décompiler le handler remplacé ET ses callees (ce sont elles qui
portent les effets), puis lire le chemin serveur correspondant. Les commentaires
rAthena disent souvent explicitement ce qui est laissé au client.

## 3. Jamais de hardcode, même « en repli »

Le hardcode dérive des mises à jour du client (nouvelles options, textes
localisés, renommages) et devient faux **silencieusement** ; l'utilisateur l'a
explicitement rejeté (« codé en dur c'est pas idéal pour les mises à jour »).

- Quand un affichage natif existe, **remonter le getter natif et l'appeler**, ne
  pas recopier ses tables.
- Si l'appel « standard » échoue (ex. `Lua_CallGlobal_va` qui détruit une
  `std::string` byval > 15 caractères → crash), **descendre d'un cran** vers
  l'API sous-jacente qui marche (API C Lua brute).
- Repli acceptable = **brut/générique** (« Option #id : valeur N »), jamais une
  copie figée des données natives.
- Un dump ponctuel (lub, db) sert à comprendre/vérifier, jamais à être recopié
  dans le binaire.

## 4. Une native cachée n'est PAS une cible de drop (souris)

En « Interface moderne », les natives (inventaire 8, storage 0x21, cart 0x28)
sont masquées par `wnd+0x28 = 0` mais **gardent leur rect**. Un hit-test qui ne
lit que `+0x1c/+0x20/+0x14/+0x18` répond « oui » sur un emplacement fantôme — et
les lâchers faits sur un viewer ImGui partaient à la mauvaise fenêtre (« je
déplace une tuile dans l'inventaire et l'objet file au storage »).

Dans tout `MouseOverXxx` visant une native :
`if (*(int*)(w + 0x28) == 0) return false;` **avant** le test de rect. À
l'inverse, un `XxxOpen()` (« la fenêtre existe-t-elle ? ») ne doit PAS tester la
visibilité. Router un lâcher : tester d'abord `over_self`, sinon une fenêtre en
dessous capte le rangement interne.

## 5. …mais elle garde le CLAVIER

Côté clavier la visibilité **n'est jamais consultée** :

```
UIWindowMgr_OnKeyDown                 0x00A471E0   if (key == 13 || key == 32)
 -> UIWindowMgr_ActivateDefaultButton 0x00A2E270   OnMsg(0) sur la fenêtre prioritaire
 -> UIWindow_OnMsg_Default            0x008841D0   msg 0 -> OnMsg(6, this+0x8C) = CLIC
                                                   (msg 1 -> +0x90 = Annuler)
```

Le prédicat interrogé, `vt+8`, est un `return 1` **en dur**
(`Stub_ReturnTrue_Folded 0x005A5D90`, partagé par ICF entre toutes les
`UIWindow`).

🔴 **Et ce n'est pas que Entrée/Espace.** `UIWindowMgr_OnKeyDown` retombe sur
`sub_A449A0`, un switch sur le code virtuel dont plusieurs cases envoient un
OnMsg sans regarder la visibilité : Page↑/↓ (33/34), Fin/Origine (35/36),
**← (37 → `sub_A38EF0`, OnMsg 20)**, **→ (39 → `sub_A4AB70`, OnMsg 21)**, ↑/↓
(38/40). Les deux fonctions de flèches essaient deux fenêtres privilégiées
(`mgr+0x3DC`, `mgr+0x3B0`) puis **parcourent la liste ENTIÈRE** (`mgr+0x17C`).

Cas réels : la fenêtre livre masquée tournait sa page ET se ré-affichait
(clignotement) ; la fenêtre 79 masquée fabriquait **sa** sélection sur Entrée
(ligne 0 de sa listbox = l'ordre du paquet, pas notre tri) et le serveur faisait
son `clif_menuskill_clear`, après quoi nos envois tombaient dans le vide ; la 111
(refine) a `+0x8C = 184` → Entrée y lance un refine réel sur une arme non choisie.

🔴🔴 **Confisquer au WndProc ne suffit pas : nos propres modules lisent
ailleurs.** Le `return 0` protège ce qui lit les messages Windows — le client.
Mais `ImGui_ImplWin32_WndProcHandler` a déjà reçu le message plus haut, donc
`ImGui::IsKeyDown` reste VRAI. Le déplacement clavier ZQSD faisait marcher le
personnage pendant que la flèche tournait une page. ➡ La fenêtre qui réclame la
touche expose un prédicat **sans effet de bord**
(`ItemDescWindow::WantsSideArrows`) que les lecteurs d'état interrogent — le
prédicat d'avalage, lui, ARME l'action et ne peut pas servir à poser la question.
**Check-list : qui d'autre lit cette touche par `IsKeyDown` / `GetAsyncKeyState` ?**

**Deux remèdes, dans cet ordre :**
1. **Détruire** ce qu'on masque, via `uiwnd::CloseWindow`, depuis
   `FlushPending`/`OnProcessInput` — **jamais** à la création (l'appelant natif
   se sert encore du pointeur → UAF) et **jamais** par le bouton Annuler (qui
   ENVOIE un paquet de désarmement).
2. Si la native est indispensable (position, session), confisquer la touche dans
   le `WindowProcHook`, **Entrée ET Espace**, indépendamment de tout réglage
   « Entrée déclenche l'action » — ce sont deux questions distinctes.

🔴 **Après un passage en « replace », auditer tout critère fondé sur la présence
de la native** (`FindWindow(id) != nullptr`) : elle ne naît plus, ces tests valent
« faux » pour toujours. Le refine conditionnait l'envoi du désarmement à
`RefineWnd()` → plus aucune compétence ne partait. Le critère doit être un état à
NOUS, calé sur celui du serveur.

🔑 **Rendre une session native qu'on n'a pas vue passer : piloter son bouton
ANNULER**, `uiwnd::OnMsg(wnd, 6, 185)` (185 = Annuler, 184 = OK ; aussi `+0x90` /
`+0x8C`). C'est un clic réel : la fenêtre envoie **son** paquet de désarmement,
sans qu'on connaisse son protocole, puis se détruit. Pour la fenêtre 80 de
fabrication c'est le SEUL chemin qui re-crédite les matériaux déjà posés.
⚠ Ne PAS désarmer sur une perte de focus : ces listes sont des `menuskill` payés
d'un consommable, un clic ailleurs brûlerait un objet. Le danger est l'existence
de la fenêtre, pas le focus.

## 5bis. Une native appelée depuis ImGui n'a PAS de fenêtre focalisée

🔴 Beaucoup de fonctions du client agissent « sur la fenêtre focalisée » —
`UIWindowMgr_GetFocusedWnd` (`0x00A33100`) rend simplement `mgr + 416`. Elles ont
été écrites pour être appelées depuis l'`OnMsg` d'une fenêtre native, qui a
forcément le focus à cet instant. **Appelées depuis un clic dans notre overlay
ImGui, ce champ vaut `0`** : la fonction déréférence un pointeur nul.

Le symptôme n'est pas un plantage visible mais **« le bouton ne fait rien »** —
l'exception part dans le `__try/__except` dont on entoure tout appel natif.

La parade tient en une ligne, AVANT l'appel :
`UIWindowMgr_SetFocusedWnd` (`0x00A4B760`, `__thiscall(mgr, wnd)`), en visant la
fenêtre que la fonction s'attend à trouver.

Vécu sur le bouton « Partager » de la navigation
([[project_navigation_re]]) : `Navi_ShareTargetToChatBar` écrit sa balise dans la
fenêtre focalisée, qu'elle suppose être la barre de saisie du chat
(`g_pNewChatWnd 0x0131F6B0`, enfant `+0xBC`). Lui donner le focus d'abord, et
elle refait exactement son travail — dépliage de la barre compris.

**Le réflexe de diagnostic** : un appel natif qui « ne fait rien » alors que le
décompilé semble correct, chercher d'abord s'il lit un ÉTAT GLOBAL d'interface
(focus, fenêtre courante, sélection) que seul le contexte natif renseigne.

## 6. Jamais de masquage par déplacement hors écran

Retour utilisateur ferme (2026-07-04) : « il ne faut pas cacher les fenêtres hors
d'écran, mauvaise idée ». Les fenêtres qui persistent leur position (barre
d'action `UIShortCutWnd` 0x24 → QUICKSLOTWNDINFO) écrivent leur `+0x1c/+0x20`
courant dans le fichier de sauvegarde en quittant. Épinglées à −10000, la
sauvegarde garde −10000 et **le joueur ne peut plus les faire revenir sans
supprimer le fichier de données**.

Utiliser le masquage natif (drapeau de visibilité, `SetVisible` vtable+0x38 →
`this+0x28 = 0`) ou la commande de la fenêtre elle-même. Filet ajouté dans
SkillBarTweaks : au ré-affichage, si la barre native lit < −5000, la replacer en
(200,100) pour qu'une sauvegarde corrompue se répare seule.

## 7. Le clavier du parcours de login appartient à ImGui — par PRÉDICAT

Bug 2026-08-05 : marteler Entrée pendant le login amenait sur l'écran de sélection
NATIF (slot occupé ⇒ entrée en jeu sur un perso jamais choisi ; slot vide ⇒
fenêtre native de création).

**Règle : du formulaire web jusqu'au char-select, le clavier appartient à ImGui.
On ne le rend qu'en sortant du parcours ou en jeu.**

🔴 **Par un prédicat, jamais `io.WantCaptureKeyboard`.** La capture ImGui est un
état de RENDU : pendant `kDriveLogin` MoonlightAuth ne dessine rien, et une modale
native lance sa propre pompe de messages — le rendu s'arrête exactement quand la
touche est la plus dangereuse. Et `SetNextFrameWantCaptureKeyboard` ne vaut que
pour la frame SUIVANTE (la première frappe passait toujours). Implémenté en
`MoonlightAuth::WantsKeyboard()`, testé dans `WindowProcHook` — **sauf
combinaisons Alt** (Alt+F4 reste au système).

🔑 Nos propres frappes passent par `RagnarokClient::PostGameKey` (lParam marqué,
`repeat count` NUL = impossible pour une vraie frappe), court-circuit placé
**avant** `ImGui_ImplWin32_WndProcHandler`.

🔴 **Corrigé le 2026-08-11 : le clavier suit L'ÉCRAN, pas une déduction.** La
version déductive s'est révélée fausse dès qu'on rendait la main sans l'avoir
prévu (compte neuf, voile expiré, fenêtre native de création ouverte par une
Entrée résiduelle : natif visible, cliquable, et MUET au clavier). Trois règles
la remplacent :
1. `CharSelect::Covering()` — un **compteur de frames**, jamais un booléen remis
   à zéro en tête de rendu : le prédicat est interrogé depuis le WndProc, donc
   possiblement AU MILIEU d'une frame ;
2. `native_login::MakeCharWindowPresent()` testé EN PREMIER, et il teste **`0x116`
   ET `0xC8`** — un compte SANS personnage ouvre `UINewMakeCharWnd 0x116`, pas
   l'ancien dialogue. Ma première correction ne testait que `0xC8` et n'a donc
   RIEN changé pour le joueur ;
3. **un filet** : salve du char-server passée + on ne couvre pas ⇒ le natif a le
   clavier, quel que soit l'écran. **Une sonde qui ne reconnaît pas une fenêtre
   doit SE TAIRE, pas confisquer.**

🔴 Rendre le clavier au natif ne suffit pas : il faut le **retirer à ImGui**. La
seconde garde du hook (`io.WantCaptureKeyboard`) avale tout indépendamment du
prédicat, et après avoir couvert ImGui garde une fenêtre focalisée. Tout chemin
qui rend l'écran au natif doit appeler
`ImGui::SetNextFrameWantCaptureKeyboard(false)`.

**Couvrir vaut mieux que rendre la main** : `CharSelect::DrawWaitCover` tient un
voile plein écran tant que la native 0x115 est là sans que nos données soient
prêtes, **borné à 3 s** — un compte sans perso ne doit jamais rester enfermé
derrière.

🔎 **Méthode qui a tranché : énumérer les fenêtres vivantes**
(`uiwnd::ListWindowIds`, liste `mgr+0x17C`) dans la trace, au lieu de deviner un
id et de le tester. `FindWindow(id)` ne répond qu'à qui connaît déjà l'id — elle
ne peut pas répondre à « quel écran natif est là ? ».

---

🔴🔴 **DIRECTIVE (2026-08-17) : l'interface moderne est un TOUT.** « Quasiment
toute l'interface ImGui devra fonctionner ensemble ou pas du tout » — la
coexistence pièce par pièce n'est plus la cible : navigation, chat et le reste
sont câblés sur l'interface moderne. **Conséquence pratique** : quand une de nos
fenêtres doit en toucher une autre, le chemin ImGui est LE chemin, et le chemin
natif n'est qu'un repli pour le mode natif — jamais l'inverse. Écrit après le
bouton « Partager » qui visait `g_pNewChatWnd` : la chatbox ImGui **DÉTRUIT** la
fenêtre de chat native (cf. [[reference_native_window_toggle_router]]), donc le
pointeur est nul et l'appel natif échoue en silence. Remède : composer la balise
nous-mêmes (`ChatWindow::AppendNaviLink`, balise `<NAVIL>` qui est NATIVE, donc
suivie même par un joueur sans Bourgeon) et ne retomber sur la native que si la
chatbox ImGui n'est pas active.

🔴🔴 **Une native rend souvent un VERDICT — l'ignorer fabrique un bouton muet.**
Deuxième panne « le bouton ne fait rien » de la même session, cause différente :
`CNavigation_SearchRoute` rend `1`/`0` selon qu'un itinéraire a été construit, et
notre enveloppe ne renvoyait que « aucune exception levée ». Le clic partait, la
native refusait, personne ne le voyait. **Toute enveloppe `__try` autour d'une
native doit propager sa valeur de retour**, avec un 3ᵉ état pour l'exception
elle-même (`-1`) — sinon le `__except` avale l'échec ET le diagnostic.

Corollaire : quand le natif fait ce qu'on n'arrive pas à faire, **la différence
est dans les ARGUMENTS, pas dans la fonction**. Ici le natif n'appelait même pas
`SearchRoute` (son « Find » passe par `SelectResult`), et notre `type = 0` exigeait
une cellule praticable qu'on remplissait de `(0,0)` — un coin de mur. Chercher
d'abord ce que le natif passe, avant de soupçonner les données : elles étaient
saines, le générateur serveur aussi (cf. [[project_navigation_re]]).

Voir aussi [[reference_native_window_toggle_router]] (DÉTRUIRE, pas masquer),
[[feedback_re_method]], [[feedback_state_lifetime]].
