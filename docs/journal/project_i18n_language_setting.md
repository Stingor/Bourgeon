# Réglage Langue de Bourgeon (i18n)

> Journal du chantier. La fiche de mémoire `project_i18n_language_setting` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Périmètre : **UNIQUEMENT l'interface Bourgeon** (src/). Les fichiers du jeu
(itemInfo*.lua, msgstringtable, luafiles514) sont HORS périmètre — décision explicite
du user, ne pas les rouvrir.

🔴🔴 **UNE MIGRATION PAR SCRIPT A CORROMPU DU CODE — vérifier avant de recommencer.**
`item_desc_window.cc` : la table du colorateur était devenue `kw[] = {")if", ...}`,
une parenthèse fermante posée À L'INTÉRIEUR du littéral. Réparé (commit e0729e2),
et ce fichier a été RECULÉ à l'état d'avant la passe par contexte — son colorateur
(littéraux de caractère + chaînes imbriquées) n'est pas analysable de façon fiable.
⚠ Les tests d'équilibre de parenthèses et de « littéral commençant par ) » sont
INEXPLOITABLES sur ces fichiers : le détecteur se désynchronise comme le migrateur.
Seule la COMPILATION tranche.

🔴🔴 **LIMITE YAML DES CLÉS : 1024 CARACTÈRES — et c'est TOUT le fichier qui saute.**
Une clé IMPLICITE (la forme `clé: valeur`) ne peut pas dépasser 1024 caractères ;
yaml-cpp l'applique (`simplekey.cpp`, `VerifySimpleKey` : `INPUT.pos() - key.mark.pos
> 1024`). Au-delà il invalide la clé, bute sur le `:` (« illegal map value ») et
`YAML::Load` LÈVE — on perd le FICHIER ENTIER, pas la ligne fautive. Symptôme :
interface intégralement en français malgré un catalogue installé, avec pour seul
indice une ligne de log. Vécu le 2026-08-08 sur le pavé d'aide des raccourcis
storage (1 284 octets, storage_window.cc:1761).
Parade = la forme EXPLICITE, sans limite : une ligne `? "clé"`, une ligne
`: "valeur"`. Vérifié dans CE yaml-cpp (scanner.cpp:228 remet `m_simpleKeyAllowed`
à vrai après un saut de ligne en contexte bloc, donc `ScanValue` passe).
Traité aux TROIS portes : `en.yaml`, l'export de `i18n.cc`
(`kMaxImplicitKeyBytes = 1000` + `WriteYamlEntry`), et `check_catalog.ps1` qui
lisait la ligne sans broncher là où yaml-cpp la rejette.
⚠ Corollaire : le parseur d'un outil PLUS PERMISSIF que celui du jeu ne prouve rien.

🔴🔴 **L'HEURISTIQUE DE LANGUE A UN TROU ÉNORME — ne plus jamais s'en servir pour DÉCIDER.**
`migrate_tr.ps1`, `migrate_rest.ps1` ET `audit_untranslated.ps1` partagent le même
filtre « un accent, OU deux mots séparés par une espace ». Conséquence : **tout libellé
d'UN SEUL mot non accentué était invisible** — « Fermer », « Objet », « Prix », « Oui »,
« Total », « Enregistrer », « Acheter », « Banque »… **423 sites**, et l'audit annonçait
zéro problème. C'est le user qui l'a vu à l'œil (`ro::RoButton("Enregistrer")`).
Le bon critère est VÉRIFIABLE et non heuristique : partir de la **liste d'appels qui
dessinent du texte** ; tout littéral qu'ils reçoivent est un défaut, quelle que soit sa
langue. Outils : `tools/lang/audit_display_calls.ps1` (détecteur) et
`migrate_display_calls.ps1` (enveloppe locale, équilibrée PAR CONSTRUCTION — `i18n::Tr(`
devant le groupe de littéraux, `)` derrière ; diff à 1:1 en nombre de lignes).
Trois exclusions à garder dans ces deux outils :
- `ImGui::OpenPopup` + `BeginPopupModal` + `BeginPopup` **ensemble ou aucun** — c'est la
  même chaîne qui les apparie ;
- un identifiant ImGui nu (`picker`, `ctx`, `skctx`, `cart_ctx`) = tout en minuscules,
  d'un seul tenant ⇒ écarté (⚠ 2 faux négatifs connus : « aucun », « passif ») ;
- 🔴 un nom de **commande serveur** (`@autolootid`, `@mobinfo`) ne se traduit JAMAIS —
  `panel_commands` en fait des `TreeNode`, dont l'état ouvert/fermé est indexé sur le libellé.

🔴 **LES CINQ ANGLES MORTS, dans l'ordre où ils ont été trouvés — TOUS par le user
à l'écran, aucun par un outil.** Un libellé peut être français à l'affichage ET
absent du gabarit d'export : c'est le cas dès que `Tr` n'est jamais appelé.
1. **Un seul mot sans accent** (« Fermer », « Objet ») → l'heuristique de langue.
2. **Une fonction MAISON** (`add_staff`, `pct`, `row`) → le test des voisines.
3. **Un libellé COMPOSÉ** par `snprintf("%s / %s")` → traduire les MORCEAUX.
4. **Un argument VARIABLE** : `static const char kLabel[]` puis
   `RoSmallButton(kLabel)`, `kStgCats[c].label`, `kPluginUnavailable`. Invisible à
   l'audit (ce n'est pas un littéral) ET à l'export (Tr n'est jamais appelé).
   ⚠ Traduire UNE fois dans une locale, et s'en servir pour MESURER **et** dessiner.
5. **Un TERNAIRE** : `RoButton(cond ? "Grille" : "Liste")`. Les migrateurs exigent
   le littéral collé à la parenthèse ouvrante. Traduire CHAQUE branche.
   Détection : `grep -rnoE '(RoButton|ImGui::Text…)\([^;]*\? *"[^"]*" *: *"'`.

🔴 **UN EXEMPLE DE CODE CITÉ DANS UN COMMENTAIRE DEVENAIT UNE CLÉ À TRADUIRE.**
`check_catalog.ps1` neutralisait les littéraux de CARACTÈRE mais ne blanchissait PAS
les commentaires — contrairement aux trois audits. Un `// CollapsingHeader(i18n::Tr("…"))`
écrit deux fois pour documenter `LinkableHeader` (moonlight_ui.h, panel_interface.cc)
sortait donc en « SANS traduction », entrée IMPOSSIBLE à satisfaire qui tenait la CI
au rouge ; et `// SeparatorText(i18n::Tr("SPR Lab"));` (code commenté) faisait passer
son entrée pour vivante. ✅ Corrigé le 2026-08-09 : le script dot-source `_scan.ps1`
et lit `Get-ScannableText` comme les audits.
⚠ Le dot-source s'écrit `. ([IO.Path]::Combine($PSScriptRoot, '_scan.ps1'))` et NON
`. "$PSScriptRoot\_scan.ps1"` (l'idiome des trois audits, qui eux ne tournent pas en
CI) : ce script tourne aussi sur le runner Linux, où `\` ne sépare rien.

🔴 **UN `/*` DANS UNE CHAÎNE CASSAIT MES TROIS OUTILS.** Ils blanchissaient les
commentaires par regex AVANT toute conscience des chaînes ; `item_desc_window.cc`
contient le littéral `"/*"` (son colorateur de script), `image_preview.cc` un
`Accept: */*`. Tout le code jusqu'au `*/` suivant disparaissait de l'analyse.
Corrigé par un **tokeniseur en une passe** — `tools/lang/_scan.ps1`, dot-sourcé par
les trois — où la branche « chaîne » vient EN PREMIER dans l'alternation.

⚠ **Ne jamais envelopper** : une **URL** (le test « est-ce une phrase ? » la sauve à
tort, `moonlight-destiny.fr` = « mot . mot ») ; un gabarit qui ne laisse qu'un
identifiant minuscule après retrait des `%s` (`tabh%s` nomme une texture, `%s_db`
un widget) ; un **message de log**.

🔴 **LE TEST DES VOISINES — le seul détecteur qui trouve les fonctions MAISON.**
`tools/lang/audit_mixed_args.ps1` (rapport **et** `-Apply`, dans le MÊME script
exprès). Principe, non heuristique : si une fonction reçoit **à la même position
d'argument** des littéraux tantôt enveloppés tantôt nus, les nus sont suspects.
A sorti `add_staff`, `add`, `slider`, `pct`, `row`, `label`, `bw`, `flat`,
`preset` — des lambdas LOCALES qu'aucune liste écrite à l'avance ne contiendrait.
⚠ Exclure `std::string`/`std::wstring` : trop générique, il a enveloppé un LOG.
⚠ Les clés de `$slots` sont un hashtable PowerShell, donc **insensible à la
casse** : `label` et `Label` se confondent.

🔴 **UNE BARRE OBLIQUE DANS UN LIBELLÉ LE REND INVISIBLE AUX DEUX AUDITS.**
`Test-Translatable` écarte comme TECHNIQUE tout littéral contenant `/` (regex
chemin/fichier), et le test de sauvetage « est-ce une phrase ? » ne le rattrape pas :
sa classe de ponctuation `[ ,:;.!?]` **ne contient pas `/`**, donc « Images / s » n'a
jamais deux lettres + séparateur + lettre à franchir. Vécu le 2026-08-08 dans
`zone_recorder` — `slider("Images / s")` et le gabarit `"● REC  %.1f / %d s  (%d img)"`
sortaient en français, les deux audits à zéro. C'est le user qui l'a vu à l'écran.
Contrôle mécanique, à refaire après toute migration :
`grep -rnoE '[A-Za-z_][\w:]*\(\s*"[^"]*[A-Za-zÀ-ÿ]{2,} / [^"]*"' src/ | grep -v i18n::Tr`
⚠ Il rate le gabarit en 3e argument (`snprintf(buf, n, "…")`) : ceux-là se relèvent
à part. Passe faite ce jour sur tout `src/` — 0 restant, les candidats survivants
étant des continuations de `Tr` multi-lignes.

🔴 **UN ÉCHAPPEMENT `\xNN` FAIT DIVERGER LE SOURCE DU RUNTIME.**
`i18n::Tr("expir\xC3\xA9")` : la clé vaut « expiré » en octets réels à
l'exécution, mais tout outil lisant le source voit la séquence d'échappement — le
catalogue ne peut JAMAIS correspondre, sans la moindre erreur. Écrire les
littéraux en **UTF-8 réel**. Contrôle : `i18n::Tr\(\s*"(?:[^"\\]|\\.)*\\x`.

✅ **`item_desc_window.cc` EST MIGRÉ** (commit 09288b1, 70 sites, diff 78/78 relu
en entier). L'exclusion datait des migrateurs qui *balayaient librement* ; les
nouveaux sont **ancrés sur un nom d'appel** et n'insèrent qu'une parenthèse
ouvrante et une fermante — équilibré par construction.

⚠ Ne PAS envelopper : un gabarit qui construit un nom de TEXTURE (`tabh%s`) ou un
identifiant ImGui (`%s_db`, `#%u##sc%d_%s`) ; un titre de fenêtre SANS `###`
(l'ID vit dans imgui.ini) — avec `###`, c'est sûr et souhaitable.

🔴 **UN MÊME MOT FRANÇAIS À DEUX SENS = UNE SEULE CLÉ, DONC UNE TRADUCTION FAUSSE.**
C'est le prix du choix « FR = la clé », et rien ne le signale : ni compilateur, ni
`check_catalog` (la clé est traduite, l'entrée n'est pas orpheline, la valeur n'est
pas vide). Vécu le 2026-08-08 : `"Enregistrer"` = **« Save »**, posé par les trois
boutons de sauvegarde du projet (chat_bg, character_sheet, char_select)… et par le
bouton qui LANCE une capture dans `zone_recorder` — lequel affichait donc « Save »
en anglais. `TrId` ne rattrape PAS ce cas : il traduit le même premier argument et
n'ajoute qu'un `###id` pour ImGui. **Seule issue : changer le libellé FR du site
minoritaire** (ici « Filmer » → Record / Grabar). Détection : une clé d'UN SEUL mot
courant utilisée par ≥2 fichiers de familles différentes mérite un coup d'œil aux
sites d'appel — `grep -rn 'i18n::Tr("<mot>")' src/`.

🔴 **UN LIBELLÉ COMPOSÉ SE TRADUIT PAR MORCEAUX, AVANT LA COMPOSITION.**
`character_sheet` fabrique ses onglets de compétences par
`snprintf(label, "%s / %s", a, b)`. Après la composition, « 1re classe / 2e classe »
n'est plus une clé de catalogue et ne le sera jamais. Symptôme trompeur : **français
à l'écran ET absent du gabarit** — donc invisible aux DEUX outils. Traduire dans
`tab_label`, à la lecture des morceaux. (La table `kFallback` reste NUE : statique.)
⚠ Un nom venu du **Lua du client** (« Ninja »…) ne se traduit pas — il sort déjà de
la langue du client.

🔴 **`ro::RoCombo` TRADUIT DÉJÀ SES ITEMS** (à la lecture, `ro_imgui.cc`) — c'est ce
qui couvre tous les combos du projet sans toucher un appel. Les `items` se passent
donc en **français NU** ; les envelopper chez l'appelant double la traduction.
Documenté sur la déclaration dans `ui/ro_imgui.h`.

⚠ **`Tr(Tr(x))` est indolore à l'écran mais pollue le gabarit** : l'intérieur rend
l'anglais, l'extérieur cherche cet anglais et l'inscrit « à traduire ». 7 cas produits
par le migrateur (cart_viewer, panel_interface). Détecteur :
`grep -rE "i18n::Tr\(\s*i18n::Tr\("`.

🔴 **Les clés s'extraient du SOURCE en UTF-8, jamais d'une sortie console** : la page de
code abîme `—`, `…` et les accents, et une clé de travers ne matche plus rien SANS
erreur. Vérifier ensemble-contre-ensemble (attendues vs traduites), pas à l'œil.

✅ **ESPAGNOL LIVRÉ (2026-08-08, commit 9deff56) : `es.yaml` complet, 2 195 entrées,
0 vide.** L'espagnol n'est plus grisé au combo. Méthode à REPRENDRE pour toute
langue suivante :
🔴 **traduire PAR INDEX, ne jamais recopier une clé.** Un script sort la tranche en
anglais dans un fichier UTF-8 (lu avec l'outil Read, PAS via la console qui abîme
les accents) ; on rend « numéro|texte » ; le fusionneur rattache chaque valeur à la
clé exacte lue dans `en.yaml`. Il refuse d'écrire tant que la couverture n'est pas
complète, puis relit son résultat avec un vrai analyseur YAML et confronte les
ensembles de clés.
⚠ Le quotage n'échappe RIEN : les valeurs arrivent déjà échappées comme le
catalogue anglais (`\n` en deux caractères). Les ré-échapper donne `\\n`, et les
infobulles sortent sur une seule ligne avec des `\n` visibles.
⚠ Contrôle indispensable : comparer la SUITE des spécificateurs de format entre le
français et la traduction (un `%s` perdu fait lire à printf un argument absent).
Il produit des FAUX POSITIFS — « 100 % directamente » se lit comme `% d` : vérifier
au site d'appel si la chaîne part en `TextUnformatted` ou en argument d'un `%s`.
Vérifier aussi que les identifiants `###`/`##` sont identiques des deux côtés.

✅ **ÉTAT (2026-08-08, commit 32e10be) : 2 195 entrées, COMPILÉ ET VALIDÉ EN
JEU par le user**, qui ne trouve plus de français à l'écran. Branche
`feat/i18n-migration`, 18 commits, 77 fichiers. `check_catalog` : 0 sans traduction
/ 0 vide / 0 doublon. `item_desc_window.cc` EST migré. Reliquat nu = 20 littéraux,
tous légitimes et vérifiés un à un (9 commandes serveur `@…`, 11 identifiants de
popup). Catalogue et squelette `es.yaml` installés dans
`E:\…\Moonlight-Destiny\SaveData\lang\`.
⚠ **La branche n'est pas fusionnée sur master** au moment où ceci est écrit.
Reste ouvert : `es.yaml` (2 195 valeurs vides, non installé dans le jeu pour que
l'espagnol reste grisé) et [[project_ro_skinning]] (chantier distinct).

**ÉTAT (2026-08-08) : 1 723 entrées, catalogue installé et CHARGÉ.** `check_catalog`
sort à 0 sans traduction / 0 vide / 0 doublon (131 orphelines = tables statiques
traduites à leur point d'usage, légitimes). Squelette `tools/lang/es.yaml` généré par
`tools/lang/new_language.ps1` : 1 723 entrées à valeur VIDE (= repli français), chacune
précédée en commentaire de sa traduction anglaise. NON installé dans le jeu, pour que
l'espagnol reste grisé au combo au lieu d'afficher du français.

**ÉTAT (2026-08-07) : 1 612 entrées au catalogue, installé.** Reste du français
(dont 51 dans item_desc_window). État antérieur ~71 % ; la suite ci-dessous.

**ÉTAT antérieur : étape 3 FAITE à ~71 %.** Branche `feat/i18n-migration`,
4 commits. **1 442 libellés migrés ET traduits** ; catalogue installé dans
`E:\Nouveau dossier\Moonlight-Destiny\SaveData\lang\en.yaml` (1 443 entrées, 182 Ko).
🔴 **JAMAIS COMPILÉ** — 51 fichiers modifiés par script, le user doit builder.
Reste **590 libellés en français**, surtout des tables statiques à reprendre à leur
point d'usage (character_sheet 129, chat_window 62, ro_imgui 36, char_select 31).

✅ **LE CONTRÔLE EST EN CI (2026-08-08)** : `.github/workflows/i18n.yml` lance
`check_catalog.ps1` sur `en.yaml` PUIS `es.yaml` (`if: always()` sur le second, pour
avoir les deux rapports d'un coup), à chaque push touchant `src/**` ou
`tools/lang/*.yaml`. `pwsh` est préinstallé sur `ubuntu-latest` ; le script a été
rendu portable (`[IO.Path]::Combine` — une chaîne `"$PSScriptRoot\..\..\src"` ne
résout RIEN sur Linux, où `\` n'est pas un séparateur).
🔴 C'est ce qui rend le choix « FR = la clé » tenable : une retouche de libellé sort
en « SANS traduction » et met la CI au rouge. Les ORPHELINES, elles, ne font PAS
échouer — 110 légitimes (tables statiques traduites à leur point d'usage).

**Outils dans `tools/lang/`** : `migrate_tr.ps1` (enveloppe), `extract_tr.ps1`
(gabarit), `check_catalog.ps1` (les 3 pannes muettes — sort à zéro),
`audit_untranslated.ps1` (le reste, par appelant). ⚠ Tous en ASCII pur, regex en
`\uXXXX` : `powershell -File` (5.1) lit un .ps1 sans BOM en ANSI et casse sur le
premier accent d'une chaîne.

🔴 **Deux pièges d'analyse, corrigés — ne pas les réintroduire** :
- un littéral de CARACTÈRE contenant un guillemet (`c == '"'`, colorateur de
  `item_desc_window.cc`) ouvre une fausse chaîne et décale la lecture de TOUT le
  fichier — du code C++ ressortait en « libellé à traduire ». Neutraliser les
  littéraux de caractère AVANT toute recherche, à longueur constante ;
- une raw string donne une clé à VRAIS sauts de ligne : artefact, à écarter (un
  libellé C++ écrit toujours `\n` en échappé).

🔴 **L'ESPACE DE BORD SE DÉCALE À LA TRADUCTION — c'est ARRIVÉ sur TOUT `es.yaml`.**
Des clés commencent ou finissent par une ESPACE : membre gauche de concaténation
(« Conversation avec » + nom, « Succès — » + message de refine), ou retrait
d'affichage (`"  Verrouillé"`). L'espace fait PARTIE de la clé ET de la traduction.
Vécu le 2026-08-08 : `es.yaml` avait **2 202 valeurs sur 2 269 précédées d'un espace
parasite**, et les **16 clés à espace final** l'avaient PERDU — le même espace,
déplacé de la fin vers le début. À l'écran : tout le texte espagnol décalé d'un cran
à droite (symptôme rapporté par le user), et « Correo aStingor » collé. `en.yaml`
avait 3 de ces espaces finaux perdus (`Success —`, `FAILED — weapon destroyed —`).
🔴 **La règle est mécanique et VÉRIFIABLE, s'en servir comme contrôle après toute
traduction** : la valeur doit avoir exactement le même préfixe et le même suffixe
d'espaces que sa clé. Un catalogue sain sort au même compte que `en.yaml` — 28
valeurs à espace initial, 16 à espace final, et AUCUNE dont la clé n'en a pas.
⚠ Le défaut ne vient PAS de `new_language.ps1` ni de l'extracteur, mais de la
traduction elle-même (valeur rendue avec un espace après le guillemet ouvrant) : le
compilateur ne le voit pas, et un éditeur qui rogne les fins de ligne le produit tout
seul.
✅ **`check_catalog.ps1` LE DÉTECTE MAINTENANT** (section « ESPACES DE BORD », sortie
en code 1) — il compare le préfixe et le suffixe d'espaces de la valeur à ceux de sa
clé, sur les valeurs NON vides seulement.
Corrigé en réécrivant les catalogues en BINAIRE (CRLF à préserver, cf.
[[feedback_code_hygiene]]) ; attention au format explicite `? "clé"` / `: "valeur"`
sur deux lignes, qui échappe à une regex écrite pour `"clé": "valeur"`.

**ÉTAT antérieur : étapes 1 (socle) et 2 (IDs stables) FAITES. Socle validé en
jeu — « [i18n] langue « en » : 2 entrées chargées ». Reste l'étape 3, la migration.**

⚠ L'étape 2 valait **25 sites, pas les ~191 annoncés** — l'estimation initiale
comptait des sites qui n'avaient aucun besoin d'être touchés. Vérifié dans le code,
et à ne pas recompter à la hausse :
- `TableSetupColumn` : **RIEN à faire**. ImGui persiste les colonnes par INDEX
  (`column_settings->Index = n` dans imgui_tables.cpp), jamais par nom. Traduire un
  en-tête de colonne ne perd ni largeur ni tri.
- `BeginTable` : rien non plus, tous les ID de table du projet sont déjà techniques
  (`cs_skill_tbl`, `storage_items`, `rodex_table`…).
- Popups contextuels : déjà en ID techniques (`ctx`, `skctx`, `cart_ctx`, `stgtab_cfg`).
- 6 des 9 `ro::BeginRoWindow` avaient déjà leur `###bourgeon_*`.
- 🔴 La vraie raison du `###` sur une modale n'est PAS la persistance mais
  l'APPARIEMENT `ImGui::OpenPopup` ↔ `ro::BeginRoPopupModal` : les deux prennent la
  même chaîne, donc traduire l'une sans l'autre donne un popup qui ne s'ouvre plus,
  en silence. Avec `###`, un oubli de migration devient inoffensif.
- Non touchée volontairement : la fenêtre `Connexion Moonlight` (moonlight_auth.cc)
  porte `NoSavedSettings` + position forcée `ImGuiCond_Always` — aucun état à
  préserver, un `###` n'y servirait à rien.

**L'API, à utiliser telle quelle pour toute la migration** (`utils/i18n.h`) :
- `i18n::Tr("Fermer")` — le cas courant. Texte, boutons, Selectable : tout ce dont
  aucun état ne survit à la frame.
- 🔴 `i18n::TrId("Langue", "bourgeon_language")` — DÈS QU'IMGUI DÉRIVE UN ÉTAT DU
  LIBELLÉ : `Begin` (pos/taille dans imgui.ini), `TableSetupColumn` (largeurs, tri),
  `OpenPopup`/`BeginPopup`, `BeginTabItem`, et `ro::RoBeginCombo` qui fait
  `PushID(label)`. Rend « traduction###stable_id » ; vérifié dans le imgui.cpp du
  projet : `ImHashStr` réinitialise son CRC sur `###`, donc l'ID ne bouge pas d'une
  langue à l'autre. `stable_id` se passe NU, sans `###`.
- ⚠ `Tr` attend un LITTÉRAL. Pour un texte à trous, c'est le gabarit qu'on traduit :
  `snprintf(buf, n, Tr("%d objets vendus"), count)`.
- 🔴 `ro::RoEndCombo()` et JAMAIS `ImGui::EndCombo()` après `RoBeginCombo` (il ouvre
  un `BeginPopup` à la main). ⚠ `panel_commands.cc:74` a ce défaut préexistant, non
  corrigé — hors périmètre, mais à traiter un jour.
- Le nom est `Tr` et pas `T` : 11 fichiers déclarent `template <typename T>`, un
  `using i18n::T` y aurait été capturé par le paramètre de template.

**RÈGLES DE MIGRATION** (fixées sur cart_viewer.cc, 1er fichier migré, branche
`feat/i18n-migration`) :
- 🔴 **Une table statique ne se traduit PAS à sa définition** (`const Cat kCats[]`).
  Agrégat construit au chargement de la DLL, avant le catalogue : un `Tr` posé là
  serait figé en français pour toujours. Traduire à CHAQUE point d'usage — y
  compris `ImGui::CalcTextSize`, sinon on mesure le français et dessine l'anglais.
- 🔴 Ces chaînes-là sont **invisibles à l'extraction statique** (argument = variable).
  Seul l'export runtime les attrape. Les deux outils se complètent.
- On enveloppe le FRANÇAIS. `Cart`, `storage`, `shop`, `alootid` restent nus — termes
  du jeu, cf. [[feedback_ui_conventions]].
- Un identifiant ImGui ne se traduit jamais : `InputTextWithHint("##cart_filter",
  Tr("Filtrer..."))`.
- Un ternaire se traduit dans CHAQUE branche, jamais autour.
- Traduire même ce qui est identique en anglais (« Description ») : la règle doit
  rester mécanique, et l'espagnol diffère.

## 🔴 LE CATALOGUE DÉRIVE À CHAQUE FONCTIONNALITÉ LIVRÉE — rattrapé le 2026-08-24

`check_catalog.ps1` sortait **66 SANS TRADUCTION** dans en.yaml ET es.yaml : 62 du
chantier Groupe/Amis livré le 2026-08-23 (la fenêtre `party_friend_window` et le
HUD `party_frames`, réglages compris) et 4 des titres de modales du même jour.
Rien ne l'avait signalé en jeu — `Tr` retombe en français **sans un mot**, et
c'est précisément le prix du choix « le français EST la clé ».
⇒ **Lancer `check_catalog.ps1` à la fin de toute fonctionnalité qui affiche du
texte**, pas seulement quand on pense à la traduction. La CI l'exécute
(`.github/workflows/i18n.yml`), mais entre deux passages le code part devant.

**Méthode rejouée à l'identique, et elle tient** : un script sort les clés
MANQUANTES depuis le SOURCE avec le même tokeniseur que `check_catalog`
(`Get-ScannableText` de `_scan.ps1` — les relever depuis un rapport console
abîmerait les accents) ; on rend « numéro|texte » ; le fusionneur rattache par
INDEX, refuse d'écrire si la couverture est incomplète, et **vérifie que les
identifiants `###` sont identiques des deux côtés**. Résultat relu par un vrai
analyseur YAML : 3 412 entrées, 0 sans traduction, 0 vide, 0 doublon.

🔴 **Une clé qui gagne un `###` doit être RENOMMÉE, pas doublée** : l'entrée
`"Client Update Required"` serait restée ORPHELINE à côté de sa remplaçante
`"Client Update Required###bourgeon_integrity_outdated"`. Le fusionneur retire
l'ancienne ligne dans le même geste.

⚠ Conventions confirmées par la mesure, à ne pas réinventer : l'espagnol
**TUTOIE** (96 occurrences de `tu/tus/estás`, zéro `usted`) et garde **« party »**
en terme de jeu (`Invitar a la party`), comme l'anglais.

**Outillage versionné** : `tools/lang/extract_tr.ps1` (gabarit YAML d'un fichier) et
`tools/lang/check_catalog.ps1` (confronte tout src/ au catalogue ; attrape les trois
pannes MUETTES — clé migrée sans entrée, entrée orpheline, valeur vide). 🔴 Le
catalogue s'EXTRAIT du source, jamais ne se recopie : une espace insécable relevée de
travers donne une clé qui ne matche pas, sans la moindre erreur — `Tr` retombe en
français et on cherche longtemps.

**Fichiers du socle** : `utils/i18n.{h,cc}` · `utils/game_paths.{h,cc}`
(`LangPath`/`LangMissingPath` → `SaveData\lang\<code>.yaml`) · clé `language` dans
`MoonlightUiOwnSettings::kHeader` (moonlight_ui.cc, résolveur maison vers
`i18n::MutableLanguageCode()`, suivi de `i18n::ReloadCatalog()` dans LoadSettings) ·
combo + compteur/export staff dans `panel_interface.cc` · catalogues versionnés
dans `tools/lang/*.yaml`.

✅ **LE DÉPLOIEMENT DES CATALOGUES EST AUTOMATIQUE (2026-08-09)** — plus rien à
copier à la main. `cmake/deploy_lang.cmake` + cible `bourgeon_deploy_lang` (ALL,
dans `src/CMakeLists.txt`, sous `if(RO_DEPLOY_DIR)`) poussent `tools/lang/*.yaml`
vers `<client>\SaveData\lang\` à chaque build, en best-effort (disque absent =
message, pas d'échec) et par comparaison de CONTENU (SHA-256), donc rien n'est
réécrit si c'est identique.
🔴 **Ce n'est PAS un POST_BUILD de `bourgeon`, et c'est le fond du problème** :
MSBuild ne relance les étapes post-build que si la cible a été RELINKÉE — or
retoucher une traduction ne recompile rien, donc le cas d'usage le plus courant
aurait été le seul à ne rien déployer. Une `add_custom_target` sans OUTPUT est
toujours tenue pour périmée : elle tourne à chaque build.
⚠ Le glob vit dans le script, pas au configure : un `de.yaml` neuf part sans
reconfigurer. `*.missing.yaml` est exclu (gabarit de traducteur, valeurs vides).

⚠ Ce qui a rendu le choix « FR = la clé » viable : les sources sont **UTF-8 sans
BOM** et le projet n'a **pas `/utf-8`** — MSVC lit et réémet en CP1252, round-trip
identité, donc les octets UTF-8 des littéraux survivent tels quels dans le binaire et
matchent, octet pour octet, un catalogue YAML UTF-8. Vérifié, pas supposé.

**Inventaire mesuré** (script d'analyse sur les 246 fichiers de src/) :
2 452 sites d'appel · 2 160 chaînes uniques · ~13 900 mots · 88 fichiers.
`features/windows/` = 67 % du volume ; les 5 plus gros font 40 % à eux seuls :
character_sheet.cc (430), make_item_window.cc (204), char_select.cc (125),
chat_window.cc (123), vending_window.cc (105).
184 chaînes de format, dont 75 à ≥2 substitutions.

**Décisions**
- **EN oui, ES en attente.** Le client Moonlight est DÉJÀ anglophone (dossier
  `SystemEN`, itemInfomoon.lua = 704 lignes EN / 1 FR) : Bourgeon est l'îlot
  français. ES n'apporterait qu'un vernis sur ~10 % de ce que lit le joueur.
- **Le français EST la clé** — `T("Fermer")`, catalogue FR→EN. Migration quasi
  mécanique par regex, code reste lisible. Mitiger le risque « texte FR retouché =
  traduction perdue » par un audit au démarrage qui journalise les clés orphelines.
- **Réglage LOCAL**, pas de pont serveur : clé `language` dans settings_table.h,
  combo dans panel_interface.cc. Cf. [[project_settings_file_layout]] pour SaveData\.
- Catalogue en YAML sur disque (`SaveData\lang\en.yaml`, yaml-cpp déjà présent),
  repli FR compilé.

**Pièges (vérifiés dans le code, pas supposés)**
- 🔴 Passer les ~191 `OpenPopup`/`BeginPopupModal`/`TableSetupColumn`/`BeginTabItem`
  en `###id_stable` AVANT de traduire — sinon largeurs de colonnes et états de tri
  sont perdus à la première bascule. 109 `###` existent déjà ; une dizaine de
  `ImGui::Begin("Titre")` en clair restent à corriger (leur titre = l'ID de fenêtre
  dans imgui.ini).
- ✅ **L'atlas de police n'est PAS un problème pour EN/ES** : ro_imgui.cc bake déjà
  Latin-1 + ponctuation. Aucun re-bake, bascule à chaud possible. (Ce serait bloquant
  pour RU/JA/ZH/TH — atlas statique, contrainte DX7.)
- ✅ Largeur : EN 10-15 % plus court que FR, ES équivalent → pas de débordement du
  9-slice. Cf. [[project_ro_imgui_toolkit]].
- ✅ Les 75 formats multi-args : FR/EN/ES partagent l'ordre SVO, `printf` suffit ;
  ne convertir vers `std::format` indexé que les cas exceptionnels.
- Vocabulaire : garder les termes du jeu, cf. [[feedback_ui_conventions]].
  Le FR source doit rester accentué, cf. [[feedback_ui_conventions]].

**Charge estimée** : 6-9 j tout Bourgeon ; 5-7,5 j en excluant le périmètre staff
(spr_effect_lab, panel_commands, zone_recorder, minigames = ~500 chaînes que les
joueurs ne voient jamais).

**Nuance à 20 lignes, optionnelle** : le serveur tourne avec `map_msg_frn.conf`, ses
messages resteront FR. Pas besoin d'un ZC custom pour lire `langtype` — il suffit
d'ÉMETTRE `@langtype english` à la bascule du réglage. Le serveur a déjà
map_msg.conf (EN) et map_msg_spn.conf (ES) remplis, et persiste la langue par compte
via `pc_setaccountreg(LANGTYPE_VAR)`. Cf. [[reference_moonlight_server]].

**Plan** : 1) socle `T()` + catalogue + réglage · 2) IDs ImGui stables ·
3) migration par fichier, par ordre de volume · 4) traduction EN + QA visuelle.
Le socle est autonome : un fichier migré marche à côté de 87 qui ne le sont pas.

**Branche dédiée** (décidé le 2026-08-07) : EXCEPTION ponctuelle à
[[feedback_build_and_git]], valable POUR CE CHANTIER SEULEMENT — le travail
courant continue de se committer sur master. Découpage recommandé : étapes 1 et 2
sur master (inoffensives seules — sans catalogue, `T()` rend la clé FR telle quelle,
et les `###id_stable` corrigent un bug latent qu'on veut de toute façon), branche
réservée à l'étape 3, fusionnée par paquets de fichiers et non en un seul gros merge
(la migration réécrit des lignes dans 88 fichiers : toute avance de master sur un
fichier déjà migré devient un conflit).
