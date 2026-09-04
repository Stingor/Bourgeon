# Conventions d'interface de Bourgeon

> Journal du chantier. La fiche de mémoire `feedback_ui_conventions` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.


Les dix conventions d'interface du projet. Elles viennent presque toutes d'une
correction de l'utilisateur : ce sont des décisions, pas des préférences.

## 1. Les accents français sont OBLIGATOIRES

Écrire les chaînes UI françaises AVEC leurs accents : « empêche » pas
« empeche », « Équiper » pas « Equiper », « Quantité » pas « Quantite ».
Demandé explicitement (« tu met pas d'accent… pourquoi ? ») — j'ASCII-fiais par
excès de prudence, à tort.

Les sources `.cc` sont en **UTF-8 sans BOM** ; la cible compile en `/utf-8`
(propagé en PUBLIC par spdlog). MSVC fait passer l'octet UTF-8 tel quel dans le
binaire, ImGui le décode, et la police couvre le Latin-1 (U+00A0–U+00FF) = tous
les accents FR. **Ne pas ajouter de BOM.**

⚠ Ne vaut pas pour les chaînes passées aux API Win32 **ANSI** (dialogues de
fichier) : là, sans accents. Pour ce qui dépasse U+00FF, voir
[[feedback_imgui_pitfalls]] §1.

## 2. Les CONSTANTES du jeu restent en anglais, les libellés en français

Règle utilisateur du 2026-07-30 : « je préfère garder les termes anglais pour les
*constantes* du jeu ». Les joueurs de RO lisent ces mots partout ailleurs (client
natif, guides, chat, commandes `@`) — une traduction « propre » mais absente du
vocabulaire du jeu les oblige à faire la correspondance eux-mêmes.

| ✅ | ❌ |
|---|---|
| **storage** | entrepôt, kafra |
| **cart** | chariot |
| **shop** | échoppe, boutique |

Ça dépasse ces trois mots : tout ce qui désigne une valeur de l'itemdb ou une
énumération du client. Les **slots d'équipement** notamment, dont le vocabulaire
de référence est `character_sheet::SlotAbbrev` — `Head top` · `Head mid` ·
`Head bot` · `Armor` · `Garment` · `Shoes` · `Accessory` · `Shield` · `Weapon` ·
`Ammunition` · `Costume head` · `Costume garment` · `Shadow` · `Other`. Idem pour
les types d'arme et de munition (`Sword 1H`, `Katar`, `Cannonball`).

⚠ **La frontière** : ce sont les CONSTANTES qui restent en anglais, pas les
libellés de présentation. Les onglets du cash shop (« Nouveautés »,
« Populaire », « Consommables ») sont en français, exprès.

## 3. Toute l'interface est RO-skinnée

**Un `ImGui::Button`/`Checkbox` nu est un oubli, pas un choix** — sauf exceptions
listées. Widgets : `ro::RoButton`, `ro::RoSmallButton`, `ro::RoCheckbox`,
`ro::RoSliderFloat`, `ro::RoBeginCombo`/**`ro::RoEndCombo`**,
`ro::RoBeginTabBar`/`RoEndTabBar` (`ui/ro_imgui.h`).

⚠ Les signatures ne sont pas identiques : `ImGui::Button(l, ImVec2(w,h))` devient
`ro::RoButton(l, w, h)` — **deux floats, pas un ImVec2**. Et `RoBeginCombo`
n'appelle pas `BeginCombo` (il dessine le champ à la main et ouvre un
`BeginPopup`) : le fermer par `ImGui::EndCombo()` laisse **5 `PushStyleColor` +
1 `PushID`** sur la pile à chaque frame déroulée.

🔴 **Trois familles de sites qu'un grep de widgets ne voit pas** :
- **le CADRE** — `ImGui::Begin` / `BeginPopupModal` nus (barre de titre bleue au
  milieu d'une interface RO). Les cadres du toolkit : `ro::BeginRoWindow`,
  `ro::BeginRoPopupModal`, `BeginRoDescWindow`, `BeginRoDescPanel`,
  `BeginRoChatWindow` ;
- **les ONGLETS** — `ImGui::BeginTabBar`. `RoBeginTabBar` se greffe sur le vrai
  TabBar (les `BeginTabItem` ne changent pas) ; une barre **imbriquée** doit être
  convertie aussi, sinon elle devient INVISIBLE sous le style transparent ;
- **les couleurs poussées à la main** — `RoButton` dessine son 9-slice et
  **IGNORE `ImGuiCol_Button`** : la couleur devient un `PushStyleColor` MORT
  tandis que le `ImGuiCol_Text` blanc qui l'accompagne, lui, continue de
  s'appliquer (blanc sur art clair = illisible). Un `ImGuiCol_Button` autour d'un
  bouton à convertir est soit à supprimer, soit la marque d'une exception.

**Exclus de la règle, décidé par l'utilisateur** : les overlays volontairement nus
(fps, dps meter, skill bar, status icons, basic info, menu icons, alootid,
char-select plein écran, sélection de zone) ; la fenêtre de LOGS (outil de dev) ;
les onglets d'inventaire/storage/cart, qui ont leur propre jeu d'onglets IMAGES
par catégorie. Détail et reliquat dans [[project_ro_skinning]].

## 4. Le corps d'une fenêtre RO est CLAIR

Deux réflexes d'ImGui « thème sombre » y produisent du texte illisible :
**`ImGui::TextDisabled`** (calibré pour un fond sombre, il s'efface sur le beige
RO) et **les teintes pâles ou vives**. Signalé en jeu sur la fiche de monstre —
nom en jaune pâle et résistances « 100 % » en beige, les deux invisibles. Le
défaut se répète à chaque nouvelle fenêtre parce que les valeurs par défaut
d'ImGui suggèrent l'inverse.

🔴🔴 **CES COULEURS NE SE RECOPIENT PLUS : elles sont dans `ui/ui_palette.h`**
(`ro::pal::`, depuis le 2026-08-25, commit `6653a34`). Ce paragraphe donnait
autrefois les cinq `ImVec4` à écrire en tête de fichier — et c'est exactement ce
qui a produit **58 déclarations pour 19 noms**, dont *dix-sept* pour le seul gris
de libellé, sous **quatre noms** (`kGray`, `kLabel`, `kLabelCol`, `kDimOnLight`).
`kGreen` avait dérivé en **quatre nuances**.

```cpp
#include "ui/ui_palette.h"

ImGui::TextColored(ro::pal::kLabel, "%s", libelle);   // libellé / texte secondaire
ImGui::TextColored(ro::pal::kValue, "%s", valeur);    // la valeur en regard
// kBlack · kSecondaryText (réglages) · kGreen · kRed · kBlue · kWarn
```

⚠ Pour du texte NORMAL, toujours `ImGui::GetStyleColorVec4(ImGuiCol_Text)` : il
suit le skin. `ro::pal` ne porte que les ÉCARTS voulus par rapport à ce texte.

**Et quand il faut vraiment qu'une mention SAILLE** (un badge « MVP », « NEW »,
« ÉPUISÉ »), la couleur de texte ne suffit pas : sur un fond clair, une teinte
assez vive pour attirer l'œil est justement celle qui s'y noie. Il faut
**peindre une pastille** — `AddRectFilled` d'un ton soutenu, puis `AddText` d'un
ton presque noir par-dessus, et un `Dummy` pour réserver la place :

```cpp
const ImVec2 p0 = ImGui::GetCursorScreenPos();          // après un SameLine()
const ImVec2 sz = ImGui::CalcTextSize(label);
ImDrawList* dl = ImGui::GetWindowDrawList();
dl->AddRectFilled(p0, ImVec2(p0.x + sz.x + 2*pad, p0.y + sz.y),
                  IM_COL32(198, 146, 12, 255), 3.0f);
dl->AddText(ImVec2(p0.x + pad, p0.y), IM_COL32(28, 20, 0, 255), label);
ImGui::Dummy(ImVec2(sz.x + 2*pad, sz.y));
```

Vécu sur la navigation : le badge MVP, d'abord posé en `TextColored` doré
(`1.00, 0.82, 0.25`), était **invisible** sur le beige — la règle ci-dessus
existait pourtant déjà.

Pour une valeur « normale », prendre `ImGui::GetStyleColorVec4(ImGuiCol_Text)`
plutôt qu'une couleur en dur : elle suit le skin. Poser un helper `Label(...)` qui
pousse `kGray` évite d'y repenser ligne par ligne.

## 5. Les largeurs se MESURENT, elles ne se figent pas

**La police de l'interface est un réglage du joueur** (`ui_font_family`), et
**ProggyClean est la plus large du menu** — une mise en page qui tient avec elle
tient avec toutes. Constaté sur la feuille de perso, dont les volets étaient figés
à 280 et 240 px : la valeur de chaque stat passait SOUS ses boutons « Max / + ».
Rien ne le signalait — juste du texte coupé.

- **Mesurer** : `ImGui::CalcTextSize(<gabarit>)` + `GetStyle()` (FramePadding,
  ItemSpacing, WindowPadding, **ScrollbarSize** si ça défile) + ce qui est en
  pixels d'IMAGE. Cf. `DollPaneW()` / `StatsPaneW()` dans `character_sheet.cc`.
- **Garder l'ancienne valeur comme PLANCHER** (`std::max(kAncienW, mesure)`).
- Ces mesures ne lisent **jamais la largeur de la fenêtre** : c'est ce qui autorise
  leur usage dans `SetNextWindowSizeConstraints` (appelé AVANT `Begin`) sans
  rétroaction d'une frame sur l'autre.
- **Mesurer les libellés TRADUITS** (`i18n::Tr(...)`).
- 🔴 **Un cache de mesure doit avoir la LANGUE dans sa clé**, pas seulement la
  police : `i18n::CatalogEpoch()` avance à chaque changement de langue, comme
  `Overlay_DeviceEpoch()` pour les textures. Sans lui, la largeur reste calibrée
  sur la langue affichée en PREMIER et le texte est coupé net, sans ellipse — et
  changer de police semble « réparer » le bug (elle, elle invalide), ce qui égare
  le diagnostic. Vécu : en → es, « Seguimiento de misiones » tronqué à
  « Seguimiento de m ».
- Pour les cas extrêmes (perso GM à six chiffres), **replier plutôt qu'élargir** :
  dimensionner sur le pire cas rend la fenêtre énorme pour tout le monde.

## 6. Dans une CELLULE : clic droit = description

**Clic DROIT = ouvrir la description. Clic GAUCHE = sélectionner / glisser.**
C'est la convention du client RO, et l'utilisateur la fait respecter : câbler la
description sur le clic gauche « vole » le geste de sélection (relevé sur la
fenêtre de refine, 2026-07-29). Portée : inventaire, storage, chariot, équipement,
boutiques — partout où le clic gauche a déjà un métier.

`mui::IsLastItemRightClicked()` (`ui/ro_widgets.h`) dit les deux orthographes
équivalentes. Ne s'applique PAS à un rect hit-testé à la main (motif ImDrawList :
il n'y a pas d'« item » ImGui). Et `IsMouseReleased(Right)` n'est PAS la même
chose (menu contextuel : on peut sortir de la zone pour annuler) — là où le projet
teste le relâchement, c'est délibéré.

⚠ `ui/ro_widgets.h` (namespace `mui`) **n'est pas dans le PCH** et **n'est pas
tiré par `ui/ro_imgui.h`** : un fichier qui veut `mui::` doit l'inclure lui-même.

## 7. Dans un LIEN : la convention INVERSE

Fixée par l'utilisateur le 2026-08-05 :
- **clic GAUCHE** → la description (objet : la fenêtre ; monstre : sa fiche ;
  adresse : le navigateur) ;
- **clic DROIT** → le menu contextuel, le même partout ;
- **MAJ + clic** → le lien dans la barre de chat.

Ctrl+clic (« description complète ») a été **abandonné** : il n'existe pas de
seconde vue à ouvrir.

🔴 **Un LIEN n'est pas une CELLULE.** Un lien est une RÉFÉRENCE : nom d'objet dans
une ligne de chat, nom de monstre dans une table de drops, adresse web. Les
cellules gardent le §6.

`features/link_gesture.h` (namespace `links`) — la surface décrit CE QU'ELLE
MONTRE, le module fait le reste :

```cpp
const links::Target t = links::FromMob(id, rank, nom_utf8);   // ou FromItem/FromItemId/FromUrl
if (links::Gestures(t, hovered)) { menu_ = t; ImGui::OpenPopup("##mon_menu"); }
...  // hors table, MÊME fenêtre ImGui que l'OpenPopup
links::DrawMenu("##mon_menu", menu_);
```

`hovered` est fourni par l'appelant (toutes les surfaces ne sont pas des items
ImGui). **La cible du menu doit être MÉMORISÉE** — le popup s'ouvre à la frame
suivante. ⚠ Le nom d'un monstre arrive dans la **code-page du client** :
`ro::LocalToUtf8` avant `links::FromMob`.

Pour une surface qui a DÉJÀ un métier au clic gauche (en-tête repliable, onglet,
bouton) : `links::ShiftClickedLastItem()` / `links::HoveredForLinkTooltip()`. **Ne
pas réécrire la lecture du geste** — elle a échoué deux fois par drapeaux
d'`IsItemHovered` avant de tenir ; elle lit la géométrie du dernier item + le
bouton BRUT de l'IO.

## 8. Afficher le message d'erreur EXACT du serveur

Un message vague (« Échec : nom pris/invalide/… ») fait que des joueurs signalent
des « bugs » là où le comportement est normal (perso en guilde, coupon absent,
config serveur). Le message précis coupe court aux faux rapports.

Quand on SUPPRIME une modale native sous notre UI plein écran (détour
`Detour_ShowModal` 0x00a31a30), on **capture son texte** (code-page client →
`LocalToUtf8`) et on l'affiche tel quel. Repli générique seulement si aucun message
(timeout = serveur muet). Réflexe à garder pour toute nouvelle action pilotée par
paquet dont le refus passe par une modale native.

## 9. « Opt-in, OFF par défaut » vise ce qui AGIT

L'opt-in protège d'un comportement **subi**. Il s'applique aux relances
automatiques, à la ré-utilisation d'objets (qui les CONSOMME), à la capture d'une
touche au détriment du chat, et à l'interrupteur de fenêtre lui-même.

**Il ne s'applique PAS :**
- aux réglages d'**affichage** (colonnes, filtre, infobulles, horodatage) — ils ne
  déclenchent rien. Les mettre OFF livrerait une fenêtre amputée de ce qui
  justifie le portage ;
- aux **garde-fous**, dont `refine_confirm`. Un échec de refine **détruit
  l'arme** : la confirmation n'est pas une fonctionnalité qu'on active, c'est une
  protection qu'on retire.

Devant une demande « tout OFF par défaut », classer d'abord en *agit* / *affiche*
/ *protège*, présenter le tableau, puis n'appliquer qu'à la première catégorie.

Cas voisin — **contenu non optionnel** : la banque n'a aucun réglage de contenu,
non par politique mais parce que son fond est un bitmap du client à hauteur FIXE ;
masquer un bloc décale tout hors du fond peint. Une fenêtre au skin natif
pré-composé ne peut pas offrir de contenu masquable.

## 10. Changer un défaut déjà livré = RENOMMER la clé

🔴 `bourgeon_settings.yaml` est **reconstruit EN ENTIER** à chaque sauvegarde
(`moonlight_ui::WriteSettings` émet chaque clé, même inchangée). Tout joueur ayant
lancé le client une fois porte donc **toutes** les clés, avec leur valeur d'alors.

⇒ Passer `MLUI_LITERAL(bool, false)` à `true` **ne change rien pour personne** :
un défaut n'est lu qu'à défaut de valeur enregistrée, et il y en a toujours une.
Le nouveau défaut ne vaut que pour une installation neuve.

➡ Pour imposer un nouveau défaut à tout le monde, **renommer la clé**. La clé
neuve n'existe dans aucun fichier ⇒ chacun prend le défaut une fois, puis décide ;
l'ancienne disparaît d'elle-même à la première sauvegarde. Vécu le 2026-08-15 :
`weapon_dual_sprites` (défaut `false`) est devenue **`weapon_dual_sprites_staff`**
(défaut `true`).

⚠ **Ne s'applique PAS aux clés de LIEN** (`links::FromSetting`, `gslink`), qui
voyagent vers d'autres clients dans les lignes de chat et **ne se renomment
jamais**.

---


## 🔴🔴 Une couleur de texte PASTEL est calibrée pour un fond SOMBRE

Le corps d'une fenêtre RO est **CLAIR**. Un vert `(0.5, 1.0, 0.5)` ou un rouge
`(1.0, 0.45, 0.4)` — les valeurs qu'on écrit d'instinct — y sont presque
blancs : relevé en jeu par l'utilisateur le 2026-08-31, la seule ligne du
carnet MVP qui doive sauter aux yeux en était la moins lisible.
➡ La paire ÉTABLIE du projet, sur fond clair (`character_sheet.cc`) :
**vert `0.10/0.50/0.15`**, **rouge `0.60/0.12/0.12`**. La reprendre, ne pas en
inventer une troisième.
⚠ L'inverse vaut pour le LOG DE CHAT, dont le fond est sombre : y éteindre un
fragment se fait en désaturant sa propre couleur (orange des liens →
`0x96,0x70,0x52`), pas en le grisant.
⚠ `ImGui::TextDisabled` et `ImGui::TextColored` par défaut sont calibrés pour
un thème sombre — idem sur le beige d'une infobulle (cf. `kDimText`).


Voir aussi [[feedback_imgui_pitfalls]], [[project_ro_imgui_toolkit]],
[[project_i18n_language_setting]], [[feedback_re_method]].
## Rejoindre un groupe : le double devoir du membre à défaut true (2026-08-18)

Quand une fenêtre à défaut **true** rejoint un groupe à défaut **off** (les
descriptions item/skill/livre entrant dans « Interface moderne ») :

1. **renommer la clé** (la règle du défaut livré changé) — sinon les yaml
   existants à true laissent un morceau moderne dans une interface native ;
2. 🔴 **ne JAMAIS la faire voter à la réconciliation en OU** — son ancien
   true (défaut, pas choix !) aurait basculé TOUT LE MONDE en moderne au
   premier chargement. Seules les fenêtres qui ont eu une vraie case opt-in
   (chatbox, dialogue NPC) votent.

**Pourquoi :** dans un OU de réconciliation, un défaut true est un faux
témoignage — il dit « le joueur l'a voulu » alors qu'il dit « personne n'a
rien touché ».

**Comment l'appliquer :** clé neuve à défaut false + défaut header false + le
membre suit le groupe sans voter. Cf. SetModernInterface / LoadSettings
(moonlight_ui.cc) et [[feedback_ui_conventions]].

## 🔴 Le format d'un slider RO est une VALEUR, jamais une phrase (2026-08-23)

`"%d s sans rien toucher"` a fait DISPARAÎTRE la piste et les flèches du slider.
`RoSliderScalar` (ui/ro_imgui.cc) réserve la largeur du texte de valeur en
**formatant les BORNES avec ce format** — le gabarit devenait « 900 s sans rien
toucher », la piste tombait à zéro et passait sous le seuil de `has_arrows`.

**Comment l'appliquer :** format court (`"%d s"`, `"%.1f °/s"`, `"%.2f x"`) ;
l'explication va dans le `HelpMarker`. Le LIBELLÉ s'affiche à DROITE de la valeur
(« 90 s Délai »), donc il se choisit pour se lire dans cet ordre.

**Et prendre `mui::WheelSliderFloat/Int` plutôt que `ro::RoSliderFloat/Int`** :
même widget, plus le réglage à la molette au survol (Shift = pas large). C'est la
forme attendue partout où l'on ajuste au degré près.
