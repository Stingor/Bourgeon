# Les pièges ImGui de Bourgeon, tous payés en jeu

> Journal du chantier. La fiche de mémoire `feedback_imgui_pitfalls` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.
> Depuis le 2026-09-04, `feedback_imgui_pitfalls` est une fiche-INDEX : chaque piège vit dans sa propre fiche `feedback_imgui_<slug>`.


Quinze pièges ImGui, chacun rencontré en jeu sur Bourgeon. Ils partagent une
famille : **ImGui ne se plaint pas**. Aucun ne produit d'erreur lisible — on
obtient un caractère manquant, une fenêtre qui s'étale, un geste qui ne part
pas, ou un crash dans le pilote.

Restent en fiche séparée parce que leur oubli coûte trop cher :
[[feedback_imgui_treenode_refuses_modified_click]] (écrire dans un `InputText`
ACTIF).

## 1. Rien au-dessus de U+00FF n'est garanti

La police de l'interface est un **réglage du joueur** (`ui_font_family`,
`ro_imgui.cc`) et l'une des options est celle intégrée d'ImGui (ProggyClean, via
`AddFontDefault`) : elle ne bake que **0x20-0xFF**. Tout le reste sort en `?`.
Mesuré : la fiche de perso affichait « STR ? 999 » (le `—` du format).

✅ **Soldé le 2026-08-10 (commit `98055c1`) par un MERGE**, pas par des
remplacements : `MergeTypographyIntoDefault()` (ro_imgui.cc) fusionne une latine
du système (Tahoma → Arial → Segoe UI, via `FindSystemLatinFont()`) JUSTE APRÈS
`AddFontDefault` — `MergeMode` vise la DERNIÈRE police ajoutée, et la source
ajoutée ensuite ne fournit que les points de code manquants, donc le dessin de
ProggyClean reste intact. Plages : 0x0152-0x0153, 0x2010-0x203A, 0x2190-0x2193,
0x25A0-0x25CF, 0xFFFD.

Deux pièges de ce merge :
- **`SizePixels` doit rester à 0** — ImGui *assert* sur une source à taille
  explicite en `MergeMode` ; à zéro elle reprend la taille de sa cible ;
- les **`GlyphRanges` ne servent qu'au DX7** (préchargement legacy) ; en DX9 les
  glyphes se chargent à la demande.

Ce qui reste interdit malgré le merge :
- **`−` U+2212** (Mathematical Operators, hors plage) — écrire le `-` ASCII ;
- **`⚠` U+26A0** — interdit dans une chaîne ImGui, demandé par l'utilisateur le
  2026-07-30. Remplacer par `/!\` (en C : `"/!\\"`). Il reste bienvenu dans les
  COMMENTAIRES, qui ne passent jamais par l'atlas.

⚠ Ne PAS « corriger » en réécrivant les littéraux en ASCII : **si la chaîne
passe par `i18n::Tr`, le français EST la clé** — la retoucher perd
silencieusement sa traduction EN/ES.

## 2. `AlwaysAutoResize` exige une largeur sur CHAQUE widget

Un widget sans largeur explicite en prend une fraction de celle de la fenêtre —
qui se calcule sur la largeur du contenu. La boucle s'emballe, la fenêtre
s'étale jusqu'au bord de l'écran et pousse hors cadre ce qui suit.

Coupables (éditeur de palette, 2026-08-11) : `TextWrapped` (le repli vient de la
largeur de fenêtre), tout `SliderXxx` sans `SetNextItemWidth`, `Selectable` sans
`ImVec2`.

Correctif : `SetNextItemWidth(GetFontSize() * N)`, `Selectable(..., ImVec2(w,0))`,
`PushTextWrapPos(GetCursorPosX() + GetFontSize() * N)`. 🔴 **Toujours dérivé de
`GetFontSize()`, jamais en pixels** — cf.
[[feedback_ui_conventions]].

⚠ Le diagnostic tentant est « le `.ini` garde une vieille taille ». Supprimer
`imgui.ini` le réfute en dix secondes : faire ce test AVANT d'écrire un
correctif.

## 3. Superposer sur un `Selectable` = `ImDrawList`, jamais `SetCursorPos`

Pour peindre icône + texte par-dessus un `Selectable` pleine largeur : capturer
`const ImVec2 scr = GetCursorScreenPos();` AVANT, puis
`GetWindowDrawList()->AddImage/AddText` en coordonnées écran.

`SetCursorPos()` pour revenir en arrière étend les limites de la fenêtre sans
soumettre d'item → assertion « *Code uses SetCursorPos() to extend
window/parent boundaries* ». Le piège est que `SameLine()` après un Selectable
pleine largeur pousse hors vue, ce qui donne envie du `SetCursorPos`.

## 4. Le filtre de texture ambiant est LINEAR

Le backend le repose dans `SetupRenderState`
(`imgui_impl_dx9.cpp:139`). Les blits de skin RO le basculent en POINT chacun de
leur côté (`ImCb_PointFilter`, `ui/ro_imgui.cc`) sans toujours le restaurer :
l'état réel à un endroit donné dépend de ce qui a été dessiné **avant** dans la
même liste.

- Poser le callback **dans les deux cas**, pas seulement pour le mode « non par
  défaut » : c'est le mode NET qui doit être imposé (le client natif ne filtre
  rien). Une case « Lisser les icônes » qui ne posait son callback que cochée
  semblait morte une fois décochée.
- Restaurer par un **callback POINT explicite**, jamais
  `ImDrawCallback_ResetRenderState` (il rend la main en LINEAR).
- Un seul basculement par lot de dessins — le callback coupe le lot.

## 5. Tout cache de texture doit suivre l'epoch du device

Les textures de `Overlay_CreateTextureARGB` / `D3D9_CreateTextureARGB` vivent en
**D3DPOOL_DEFAULT** (device D3D9Ex : MANAGED interdit) et appartiennent à UN
device. Après Reset/ResetEx/CreateDevice/CreateDeviceEx (alt-tab, changement de
résolution, **TDR de contention VRAM** cf. [[user_runs_local_llm]]), les handles
cachés sont morts → `AddImage(handle_mort)` → **0xC0000005 dans ddraw.dll**.

Un epoch global `Overlay_DeviceEpoch()` (d3d9_hook.h/.cc) est bumpé aux 4
événements. **Tout ce qui cache une texture entre frames DOIT le comparer** :

```cpp
static unsigned s_epoch = 0;
const unsigned e = Overlay_DeviceEpoch();
if (e != s_epoch) { g_icon_cache.clear(); s_epoch = e; }
```

La vraie surface de crash n'est pas les icônes mais **`ro_imgui.cc`** : ~40
globals `SkinTex` (le skin de TOUTES les fenêtres `ro::`), champ `epoch` dans
`SkinTex`, invalidation dans `EnsureTex`/`EnsureTexClient`. PostFx a son propre
motif (`PostFx_OnDeviceRecreated()`), appelé aussi sur CreateDevice/CreateDeviceEx
— les chemins qu'un TDR emprunte. On ne fait PAS `Release()` des icônes mortes
(fuite VRAM négligeable sur événement rare, acceptable contre un crash). DX7 non
couvert (surfaces managed, auto-restaurées).

## 6. Jamais de `Release()` de texture pendant une frame

`AddImage` ne rend rien : il note un `ImTextureID`. Le device ne la voit qu'au
**rendu** (`RenderDrawData` → `SetTexture`), en fin de frame. Un
`Overlay_ReleaseTexture` entre les deux détruit l'objet sous les pieds du rendu →
**STATUS_HEAP_CORRUPTION 0xC0000374** dans le pilote.

À distinguer du §5 : là le device a disparu (0xC0000005) ; ici il est vivant et
c'est NOTRE Release qui est prématuré. Crash 2026-08-04 au char-select : le cache
de `ui/sprite_view.cc` plafonnait à 128 entrées, la grille de coiffures en
affiche 80 par sexe, la bascule franchissait le plafond **en cours de frame**.

Deux gardes (`ui/sprite_view.cc`, même motif dans `char_select.cc`) :
- **file de libération différée** — `QueueRelease` note (texture, epoch, frame),
  `FlushPendingReleases` ne libère que si `frame > p.frame + 1`, et **lâche sans
  Release** si l'epoch a changé ;
- **une entrée servie dans la frame courante n'est jamais évincée**
  (`Entry::last_frame`, marqué dans `EnsureLoaded` — pas dans `Acquire` : les
  appelants gardent leur `SpriteRes` et dessinent sans réacquérir). Si tout le
  cache sert dans la frame, on dépasse les bornes plutôt que casser le rendu.

## 7. `BeginDragDropSource` exige un ID

Il rend **`false` sans rien dire** quand le dernier widget soumis n'a pas
d'identifiant — `Text()`, `TextUnformatted()`, `Image()`, `TextColored()`…
L'assertion qui l'expliquerait (`IM_ASSERT(g.LastItemData.ID != 0)`) est
**compilée hors du binaire en Release** : le glisser ne part jamais, sans
message, sans crash.

Soit glisser depuis un vrai widget (`InvisibleButton`, `Selectable`, `Button`),
soit passer **`ImGuiDragDropFlags_SourceAllowNullID`**. Vu le 2026-08-09 :
l'onglet Homoncule glissait depuis un `TextUnformatted`.

## 8. Minimisé : bailler AVANT `NewFrame()`

Minimisé, `ImGui_ImplWin32_NewFrame` fait `GetClientRect` → 0x0 →
`io.DisplaySize == (0,0)`. Chaque `Begin()` clampe alors sa fenêtre dans un
viewport nul (coin 0,0) et **cette position pourrie est mémorisée**.

```cpp
const ImGuiIO& io = ImGui::GetIO();
if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f) return;  // minimisé
ImGui::NewFrame();
```

Appliqué aux DEUX chemins : DX9 `RenderImGuiDX9` (d3d9_hook.cc) et DX7
`Proxy_EndScene` (proxy_idirectdraw.cc). ⚠ Ne pas se contenter de garder
`RenderDrawData` : le clamp arrive dans `Begin()`. Il faut sauter TOUTE la frame.

## 9. Clavier : n'avaler que les touches dont on a besoin

Règle posée par l'utilisateur le 2026-08-01 après l'avoir rencontrée **deux
fois**. Le garde-fou du WndProc avalait `WM_KEYDOWN/UP/CHAR` en bloc dès que
`io.WantCaptureKeyboard` était vrai — or ce drapeau passe à vrai dès qu'un champ
de saisie a le focus. Le joueur clique dans le filtre d'une boutique et toute la
barre d'action meurt.

1. Quand la capture vient d'une **saisie** (`io.WantTextInput`), le WndProc
   relâche ce qu'aucun champ ne peut utiliser : **F1-F12** et les combinaisons
   **Alt**. Ctrl reste avalé (copier/coller).
2. Les prises **volontaires** restent totales (char-select, DOOM : ils demandent
   `SetNextFrameWantCaptureKeyboard` sans saisie) — d'où le test sur
   `WantTextInput` et non sur `WantCaptureKeyboard` seul.
3. Pour les touches qu'une fenêtre pilote vraiment, avalage **ciblé** au cas par
   cas : `NpcDialogWindow::EatsKey(msg, wparam)`.
4. 🔴 **Entrée appartient d'abord à la barre de chat** (2026-08-10) : tester
   `!io.WantTextInput` **ET** `!ChatWindow::OwnsEnterKey()`. Le premier ne suffit
   pas — la barre peut être armée SANS avoir le clavier (`SetKeyboardFocusHere`
   est refusé tant que le bouton de souris est enfoncé), et le lien posé d'un
   Maj+clic reste alors PRISONNIER. Espace, lui, reste à la fenêtre.
   ⚠ Une touche avalée pour le jeu doit être **remise directement**
   (`chat->OnRawKey`), jamais relâchée — sinon elle passe par
   `UIWindowMgr_ActivateDefault`, donc par le bouton par défaut d'une native.

Ne pas confondre avec [[feedback_native_replacement]] : là le but est
inverse (voler la touche au JEU).

## 10. Aperçu à la molette : `NoScrollWithMouse` sur le conteneur

Quand un survol donne la molette à autre chose que le scroll (aperçu de
personnage qui tourne), **ImGui scrolle AVANT** que le consommateur ne voie
l'événement. Consommer ne suffit pas : couper en amont avec
`ImGuiWindowFlags_NoScrollWithMouse` sur la fenêtre ou le `BeginChild`.

Le drapeau se pose en FIN de frame et se lit au DÉBUT de la suivante (le survol
ne se découvre qu'après le `Begin`, où les flags sont figés) — décalage d'une
frame, invisible à la main. 🔴 Le poser **hors** de tout `BeginTabItem` : depuis
l'intérieur, changer d'onglet en pleine rotation le fige à `true` et supprime le
scroll pour de bon.

**⚠ Exception : la molette sous Ctrl est LIBRE.** `UpdateMouseWheel` sort par
`if (g.IO.KeyCtrl) return;` AVANT tout scroll, et le zoom de fenêtre dort tant
que `io.FontAllowUserScaling` est false. Un geste Ctrl+molette n'a rien à
disputer : lire `io.KeyCtrl && io.MouseWheel` sous
`IsWindowHovered(RootAndChildWindows)` (cf. `ChatWindow::HandleFontZoom`).

## 11. Molette au survol : passer par le verrou `mui::`

Une zone qui lit `io.MouseWheel` dès qu'elle est survolée **vole le défilement de
la page qui la contient** : le joueur qui parcourt les réglages voit ses valeurs
bouger sous le curseur.

Le verrou (`ui/ro_widgets.h`) : `mui::LastItemWheel(engaged)` pour le dernier
item soumis, `mui::RegionWheel("clé", hovered, engaged)` pour une zone
hit-testée à la main, `mui::WheelGateNewFrame()` une fois par frame depuis
`Bourgeon::RenderUI`. La molette n'est accordée que si (a) aucun défilement n'est
en cours (salve de 0,35 s) et (b) le curseur est posé depuis 0,30 s.

Deux détails rendent l'écriture naïve fausse :
- 🔴 le défilement est appliqué dans `NewFrame`, donc AVANT tout widget : une
  possession posée au moment du cran vaut pour la frame **suivante**. Il faut la
  poser à CHAQUE frame dès que la zone est prête ;
- 🔴 « un cran a-t-il défilé ? » ne se compte pas, ça se lit :
  `ImGuiContext::WheelingWindowScrolledFrame == FrameCount`.

🔴 **Le verrou n'est PAS pour le conteneur qui défile.** `RegionWheel` sur le
`BeginChild` d'un corps de fenêtre lui fait revendiquer la molette, puis la
consommer : **la fenêtre ne défile plus du tout**, et l'état ne se relâche jamais
(`scroll_until` ne se réarme qu'après un défilement EFFECTIF, qui n'arrive plus).
Symptôme : « la molette ne fonctionne pas toujours ». Vécu dans Game Settings,
corrigé le 2026-08-15 en **retirant** l'appel. Le verrou vit au widget qui PREND
la molette, jamais au conteneur.

## 12. Jamais de commande native depuis `OnRenderUI`

Empiler l'intention et la rejouer dans `Bourgeon::OnProcessInput` (phase d'input
du jeu, hors frame ImGui). File de `{win_id, cmd}` — stocker l'**identifiant** de
fenêtre, jamais le pointeur (le client détruit ses fenêtres → usage après
libération une frame plus tard). Même motif que `MenuIcons::FlushPending`.

`UIWndMgr_ShowMessageBoxModal` (`0x00A31A30`) est **vraiment** bloquante : elle
ne rend pas la main, elle boucle en relançant le tick/rendu du mode courant
jusqu'à la réponse. Reproduit sur `vending_tweaks` : un objet à prix 0 gelait le
client à tous les coups ; le report a supprimé le symptôme.

⚠⚠ **Le mécanisme exact n'est PAS établi.** Test 2026-07-28 : le skill 12802
dans la barre d'action ouvre la modale `0x945` depuis `DrawBar` → `ActivateSlot`,
donc bien en pleine frame ImGui — **et le client ne gèle pas**. Mesuré : la
boucle modale appelle `GameMode_InGame_ProcessFrame 0x00C74A80`, **ni BeginScene
ni EndScene au premier niveau** → la ré-entrance du hook ImGui n'est pas
démontrée. Le report reste bon à prendre (il n'a aucun coût) mais **ne pas le
généraliser sur la foi de cette explication**. Prochaine étape si le sujet
revient : compteur de profondeur + LogDiag dans le hook EndScene.

⚠ **Leçon de l'audit lui-même** : « la fonction OnMsg contient un appel à la
modale » ne suffit pas à conclure — il faut vérifier **quel `case` y mène et sous
quelles gardes**. J'ai signalé `skill_bar` comme défaut confirmé ; l'utilisateur
a objecté « en prod depuis un moment sans problème », et il avait raison (les
appels y sont gardés sur le skill 12802 seul).

## 13. Jamais de dialogue Win32 bloquant sur le thread de rendu

`GetSaveFileNameA`, `MessageBox`, toute boucle de dialogue lancée depuis le
thread de rendu **gèle le rendu ET le réseau** → le serveur timeout la session →
**déconnexion**.

Ouvrir sur un `std::thread` détaché : `CoInitializeEx(APARTMENTTHREADED)`,
`hwndOwner = nullptr` (pas de propriétaire cross-thread), résultat déposé dans
des `std::atomic` + `std::string`, consommé à la frame suivante par le thread
principal pour le travail device-dépendant. **`OFN_NOCHANGEDIR` obligatoire**
(sinon le dialogue change le CWD du process et casse les accès relatifs du jeu).
Strings passées aux API **ANSI** : sans accents (source UTF-8 → octets mal
interprétés en CP1252) ; les strings ImGui gardent les leurs.

Exemple : `CharacterSheet::RequestGifSave` dans [[project_character_sheet]].

## 14. Un bouton ÉTROIT affichait « ... » à la place de son libellé

🔴 Le « + » carré des stats (fiche de personnage), les +/- des boutiques :
tous rendaient **« ... »**. `ro::FitButtonLabel` retirait de la largeur les
**caps de l'art** (6 px de chaque côté, TAILLE FIXE) ; sur un bouton large d'une
hauteur de ligne (~20 px) il ne restait que 6 px, moins que le « + » lui-même
(7 px en ProggyClean) — d'où rétrécissement, puis coupe, puis une ellipse qui ne
gardait **aucun caractère** et qui est plus large que le signe remplacé.

Deux garde-fous, tous deux dans `ro_imgui.cc` :
· **jamais d'ellipse qui ne garde rien**, ni qui ne soit pas plus étroite que le
  texte entier — un débordement d'un ou deux pixels dit encore ce que fait le
  bouton, « ... » non ;
· `ButtonLabelRoom` : la place offerte au libellé ne descend **jamais sous 60 %
  de la largeur** du bouton. Sous ce seuil, le texte a le droit de mordre sur les
  caps : à ces tailles-là, l'art n'est plus qu'un liseré.

⚠ Leçon générale : **une mise en page qui soustrait des marges de taille FIXE
casse aux petites largeurs**, et le symptôme n'apparaît que là — les boutons
larges, eux, allaient bien depuis toujours.

## 15. ImGui tient des raccourcis EN DUR, actifs sans `NavEnableKeyboard`

🔴 **Ctrl+Tab cycle entre nos fenêtres alors que le projet ne pose PAS
`ImGuiConfigFlags_NavEnableKeyboard`.** `NavUpdateWindowing` (imgui.cpp) le dit en
toutes lettres : `start_windowing_with_keyboard` porte le commentaire *« Note:
enabled even without NavEnableKeyboard! »*. Le raisonnement « le flag n'est pas
posé, donc la navigation clavier est éteinte » est donc **faux**, et c'est
exactement le genre de touche qu'on cherche pendant une heure du mauvais côté :
elle n'est ni dans notre catalogue de raccourcis, ni dans `UserKeys.lua`.

Le levier vit dans **`ImGuiContext`, pas dans `ImGuiIO`** (donc
`imgui_internal.h`) : `ConfigNavWindowingKeyNext` / `ConfigNavWindowingKeyPrev`.
À 0 = coupé.

🔴 **Un modificateur est OBLIGATOIRE** si on réaffecte le combo : ImGui « tient »
le cycle tant que le modificateur partagé par Next et Prev reste enfoncé, et
`IM_ASSERT(shared_mods != 0)` tombe sur un combo qui n'en porte aucun.

Réglé le 2026-08-22 : le combo est devenu l'action `ui_cycle_windows` du catalogue
Bourgeon (`hotkeys::ApplyImGuiWindowingChord`, repoussée à chaque tick), avec
Ctrl+Tab comme défaut CONSTATÉ — le combo livré, repris tel quel pour n'avoir
aucune clé à renommer.

⚠ Leçon générale : **ImGui a des gestes que rien n'affiche chez nous**. Avant de
conclure qu'une touche mystérieuse vient du client, chercher dans imgui.cpp.

## 🔴🔴 `WantCaptureKeyboard` EST LEVÉ PAR TOUT WIDGET ACTIF — un GLISSER compris

ImGui : `if ((g.ActiveId != 0) || (modal_window != NULL)) io.WantCaptureKeyboard = true;`
Donc tenir un bouton, tirer un slider ou **glisser un objet** éteint le clavier
du jeu si on s'en sert comme d'un « ImGui a besoin des touches ». Vécu le
2026-08-26 : une émote portant `@storage` en Alt+3 cessait de répondre dès qu'on
avait attrapé un item, alors que le client natif l'acceptait.
➡ Ce qui doit vraiment faire taire un raccourci, c'est **`io.WantTextInput`**
(la frappe est un caractère) et une **MODALE** (`IsPopupOpen(AnyPopupId |
AnyPopupLevel)`) — les deux tests que `quick_cast`, `character_sheet` et
`char_select` employaient déjà. Corrigé aux DEUX étages : le WndProc
(`ragnarok_client.cc`, qui traitait ça comme une prise TOTALE) et
`hotkey_dispatch`.

## 🔴 Contrainte de TAILLE : la poser AVANT `Begin`, sinon on rattrape au lieu de brider

`SetNextWindowSizeConstraints` entre dans le calcul du glisser
(`CalcWindowSizeAfterConstraint`) : **la poignée bute**. Écrire `SizeFull` dans
une passe post-`NewFrame` arrive une frame trop tôt — `UpdateWindowManualResize`
tourne DANS `Begin` et le réécrit depuis la souris.
➡ `ro::ConstrainNextWindowToScreen()` (dans les wrappers `BeginRo*Window`)
**RABAT le plafond de l'appelant** au lieu de lui céder la main : renoncer devant
une contrainte existante protégeait exactement les fautives (atlas, entrepôt,
échoppe, cash shop bornent à `FLT_MAX`). Son MINIMUM, lui, est préservé.
⚠ Une borne NÉGATIVE = « n'y touche pas » chez ImGui : l'axe reste LIBRE.
⚠ `CalcWindowMinSize` s'applique APRÈS le clamp : un `min` à 0 n'ouvre donc rien,
le plancher 32×32 vient du style.
⚠ Une fenêtre `AlwaysAutoResize` SANS largeur imposée se dessine à la largeur par
DÉFAUT à la 1re frame : les `TextWrapped` s'y enroulent sur dix lignes et la
hauteur est rabattue sur `DisplaySize` (le clignotement pleine hauteur du pet).

---

## 🔴🔴 Un art de skin posé à une coordonnée FRACTIONNAIRE bave

L'art du skin RO est du pixel-art échantillonné en POINT. Posé à un x ou un y
non entier, il tombe **entre deux texels** et ses bordures d'un pixel deviennent
floues — le widget paraît « sale » à côté d'un voisin identique dont la largeur
est tombée juste. Or `CalcTextSize` rend presque toujours des fractions, et
elles se propagent dans la largeur puis dans la position.
➡ Tout placement MANUEL (`SetCursorScreenPos`) d'un widget skinné **arrondit** :
largeur au PLAFOND (au plancher, `FitButtonLabel` rognerait le libellé), x et y
au plancher. Le layout ordinaire d'ImGui n'a pas ce problème — c'est le
placement à la main qui l'introduit.
⚠ Le symptôme se déplace avec le LIBELLÉ : un bouton net en français peut baver
une fois traduit. Deux boutons côte à côte, l'un propre l'autre non, c'est CE
piège.

## 🔴 La hauteur d'une barre de titre RO ne se REDEMANDE pas depuis le corps

`BeginRoWindow` peint la barre pendant le `Begin`, sous le style de CE
moment-là. Un `ImGui::GetFrameHeight()` lu plus tard, dans le corps, a déjà
traversé les `PushStyleVar` du skin : centrer dessus donne un widget qui déborde
la barre par le bas.
➡ La géométrie de la barre (haut, hauteur, bord gauche des boutons système) est
RELEVÉE au `Begin` et rendue par `ro::TitleBarButton`. Le bord gauche dépend
aussi de ce que la fenêtre porte — [mini][pin][close] au complet, ou rien du tout
quand elle n'est ni fermable ni repliable (l'écran de connexion) : le déduire
chez l'appelant supposerait de lui faire connaître `p_open` et les drapeaux.

## ⚠ `IsItemHovered()` ignore le DÉSACTIVÉ — donc l'infobulle qui explique le grisage

Sous `BeginDisabled`, le survol ne compte pas par défaut : un bouton grisé reste
inerte ET muet, alors que c'est exactement là que le joueur a besoin du motif.
➡ `IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)`.

## 🔴🔴 `PushID(int)` d'un rang de table ENTRE EN COLLISION avec l'en-tete de la meme colonne

`TableHeadersRow` pose `PushID(column_n)` sur chaque en-tete. Un rang qui pose
`PushID(row_id)` au MEME etage de pile rend donc EXACTEMENT l'identifiant de la
colonne `column_n == row_id` — et le widget du rang entre en conflit avec
l'en-tete des qu'ils partagent un libelle (typiquement `"##fav"`, `"##grip"`).
Le symptome est trompeur : ImGui surligne un rang DIFFERENT a chaque fois (celui
dont l'id vaut l'index de colonne), et l'en-tete en face.
➡ Pousser les rangs sous un NOM d'abord : `PushID("slot"); PushID(id);` — le
sceau des rangs sort de la plage des index de colonnes. Deux `PopID`.
⚠ Vaut pour TOUTE table, y compris celles a 4 colonnes ou le piege dort tant
qu'aucun libelle ne coincide.

## 🔴🔴 Un widget peint a la main s'attrape A TRAVERS une fenetre posee dessus

`IsMouseHoveringRect` ne connait que des COORDONNEES : il ignore totalement le
recouvrement. Notre scrollbar RO (`DrawRoScrollbar`) se laissait donc saisir en
meme temps qu'on deplacait une fenetre de premier plan.
⚠ Une garde `ActiveId != 0` **ne suffit pas** : ImGui ne decide de deplacer une
fenetre qu'a la FIN de la frame du clic (`UpdateMouseMovingWindowEndFrame`), donc
sur la frame ou `IsMouseClicked` est vrai — la seule qui compte — `ActiveId` vaut
encore zero.
➡ Tester `ctx->HoveredWindow->RootWindow == w->RootWindow`, le meme test de
recouvrement qu'ImGui applique a ses propres widgets. Laisser un drag DEJA EN
COURS hors de la garde : il doit survivre a une souris sortie de la fenetre.

---

Voir aussi
---

Voir aussi [[feedback_ui_conventions]] (le corps d'une fenêtre RO est
CLAIR), [[project_ro_imgui_toolkit]], [[feedback_re_method]].
