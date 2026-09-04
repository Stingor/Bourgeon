# Build et git sur Bourgeon et moonlight

> Journal du chantier. La fiche de mémoire `feedback_build_and_git` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.


Tout ce qui touche à **produire et livrer** le code. Règle mère en fiche
séparée : [[feedback_dont_relaunch_game]] (ne jamais annoncer « déployé »).

## 1. C'est l'UTILISATEUR qui build

🔴 Tranché en fin de session 2026-08-14 : **je code, je dis que c'est prêt, il
bâtit et me remonte les erreurs.** Ce n'est pas une interdiction de principe (il a
précisé « en général ça ne me dérange pas que tu build ») — ce qui l'agace, c'est
que je fasse tout un plat d'un déploiement raté : l'annoncer, le retenir, et lui
redemander de fermer le jeu à chaque tour. La raison d'origine (2026-07-07) était
le bruit : une sortie de build gonfle le contexte pour rien.

- Un déploiement sauté est un **NON-ÉVÉNEMENT** : il échoue en douceur
  (`|| echo "Deploy skipped"`), le build sort en 0. Ne pas le signaler, ne pas
  demander de fermer le jeu, ne pas y revenir au tour suivant.
- ⚠ Ne jamais annoncer « déployé » sur la foi du journal : `Deployed to` est un
  `COMMENT` affiché AVANT la copie.

**Le SERVEUR moonlight aussi**, ré-appris le 2026-08-12 en l'enfreignant trois
fois : chaque tentative a ramené un mur d'erreurs de lien, pour un travail qu'il
fait en **37 secondes** dans Visual Studio. Deux faits à ne pas re-découvrir :
- 🔴 **il bâtit en `Debug x64`**, pas Release. Les `.lib` partagées
  (`common.lib`, `ryml.lib`) sont donc `MTd_StaticDebug` : une build Release
  échoue en LNK2038 partout. Et les deux configs écrivent le même
  `map-server.exe` à la racine ⇒ bâtir en Release **remplacerait son binaire** ;
- bâtir un `.vcxproj` isolément échoue en C1083 (les chemins d'inclusion sont au
  niveau de la **solution**) — il faut passer par `rAthena.sln`.
- ⚠ `LNK1104: impossible d'ouvrir map-server.exe` ne veut PAS dire « erreur de
  compilation » : le serveur TOURNE et verrouille son binaire.

Reste légitime de mon côté : les vérifications **syntaxiques** isolées du
scratchpad (`cl /c`, aucune DLL produite) — elles ne déploient rien.

## 2. La commande, si elle est demandée : `--config Release`

🔴🔴 **NE RIEN AJOUTER APRÈS LA COMMANDE DANS UNE TÂCHE DE FOND.** Un
`cmake … > log 2>&1; echo "EXIT=$?"` fait rapporter à la tâche le code de sortie
de l'**`echo`**, donc **0**, alors que le build avait échoué (2026-08-24). J'ai
annoncé un build vert sur un build cassé. La commande backgroundée doit être la
DERNIÈRE de la ligne ; le code de retour est celui que la notification donne.
⚠ Et ne jamais conclure sur le seul code de sortie : compter
`grep -c "error C[0-9]\|error LNK"` dans le journal, et vérifier que la DLL
existe avec sa date.

```
cmake --build "d:/Mes documents/GitHub/Bourgeon/build" --config Release
```

Ça suffit — pas besoin de `--target ALL_BUILD -j 20`. Chemin complet si `cmake`
n'est pas dans le PATH :
`C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`.

**Release, JAMAIS Debug ni RelWithDebInfo** : chaque build (toute config) déploie
`ddraw.dll` vers `E:/Nouveau dossier/Moonlight-Destiny` **et** réécrit
`ddraw.dll.sha256`. Bâtir la mauvaise config déploie silencieusement ce
binaire-là et pointe l'empreinte dessus.

🔴🔴 **LE CLIENT CRASHE AVEC UN `ddraw.dll` DEBUG** (dit le 2026-09-02). Ce n'est
donc pas une question de propreté : bâtir en Debug **casse le jeu de
l'utilisateur**, et en silence — le build sort en 0, la DLL est déployée, et le
crash se découvre au lancement suivant. Bâtir « Release puis Debug pour vérifier »
laisse donc le DEBUG en place : c'est la dernière build qui gagne, pas la bonne.

Le 2026-09-02 j'ai fait ça toute la journée sans le savoir ; l'utilisateur
rebâtissait en Release derrière moi à chaque fois, donc rien n'a été faussé — mais
c'est lui qui payait ma vérification inutile. ⚠ Et ne pas conclure trop vite que
des essais bizarres viennent de là : le lui DEMANDER (il l'a corrigé lui-même,
« j'ai rebuild derrière en release à chaque fois »).

➡ **Une seule commande, une seule config.** `--config Release`, point. Ne pas
« bâtir les deux pour vérifier » : ça ne vérifie rien de plus et ça déploie la
mauvaise.

⚠ **`error C1083 … 'xxx.obj' : Permission denied` n'est PAS une erreur de code** :
c'est un SECOND build qui écrit dans le même dossier intermédiaire. Si
l'utilisateur bâtit de son côté, mes builds entrent en collision avec les siens et
sortent des erreurs qui n'ont rien à voir avec les sources. Une autorisation
ponctuelle (« tu peux build ») ne vaut pas pour la session entière : dès qu'il
rebâtit derrière moi, je m'arrête.

- Le dossier `build/` est configuré avec le générateur **« Visual Studio 18
  2026 »** (multi-config, d'où `--config Release`). ⚠ `build_configure.bat` dit
  « NMake Makefiles » : se fier au générateur du `build/` réel, pas au `.bat`.
- **Reconfigure automatique** : ZERO_CHECK relance cmake quand un `CMakeLists.txt`
  change. Pas de reconfiguration manuelle.
- Sortie : `build/src/Release/ddraw.dll` (OUTPUT_NAME « ddraw »). Le déploiement
  est SKIPPÉ si le jeu tourne (DLL verrouillée).
- **Ajouter un plugin = 3 endroits** : les `.cc`/`.h` ; la liste de
  `src/CMakeLists.txt` (sources EXPLICITES, pas de glob) ; `src/bourgeon.cc`
  (`#include` + `plugins_.emplace_back(...)` dans `LoadPlugins()`).

## 3. Clean-first dès qu'un en-tête bouge — et même sans

✅ **L'utilisateur clean-builde systématiquement** depuis le 2026-07-30 (« je
clean build à chaque fois maintenant »). **Ne plus le lui rappeler** : c'est du
bruit, et ça revient à douter d'une discipline qu'il a adoptée après en avoir payé
le prix. Ce qui reste utile, c'est le réflexe **pour moi** : crash mémoire bizarre
→ suspecter l'ODR AVANT l'analyse.

Modifier un en-tête très inclus (avant tout `src/bourgeon.h`) change la
disposition de l'objet et de tout ce qui suit. Si seuls certains `.obj` sont
recompilés, les périmés accèdent aux ANCIENS offsets → **violation d'ODR** → un
champ lit du n'importe quoi.

🔴 **Le déclencheur n'est PAS seulement « j'ai touché un en-tête ».** Rechute le
2026-07-29 : je n'avais modifié qu'un `.cc`, mais l'ARBRE contenait le travail non
committé d'un autre agent, dont `bourgeon.h`. **Avec un second agent dans le
dépôt, tout build est `--clean-first`** — ce sont les en-têtes des AUTRES qui
décident. Un `git status` suffit à le voir.

Les trois symptômes déjà vus, tous très loin du code édité :
- crash au démarrage, `0xC0000005` dans `RagnarokClient::Initialize`, `bourgeon.log`
  arrêté juste avant « Bourgeon initialized successfully! » ;
- crash au RENDU dans un plugin non touché (`MenuIconTweaks::BuildIconList`,
  `movups [ecx],xmm0` avec `ecx` pourri — un `std::vector` écrit à travers un
  mauvais pointeur parce que la disposition de classe diffère entre `.obj`) ;
- crash NATIF (incident du 2026-07-28, chantier « annuaire d'adresses », 115
  fichiers déplacés + 6 en-têtes neufs) dans `Job_BuildHeadgearSpritePath_impl`,
  avec un sexe lu à 0 sur un personnage mâle : des champs de l'acteur corrompus
  par les plugins qui y écrivent.

⚠ **Un `git bisect` sur des builds incrémentaux ne prouve RIEN** : le bisect du
2026-07-28 a rendu « good » partout, et le crash a disparu dès le premier
`--clean-first`, y compris sur les commits qui plantaient une heure plus tôt.
Coût : plusieurs heures au débogueur et deux conclusions fausses données à
l'utilisateur (« ce ne sont pas nos modifications », « il n'y a pas de
corruption ») — il avait raison sur les deux.

⚠ Un SHA différent entre deux builds ne prouve rien non plus : MSVC embarque un
horodatage dans l'en-tête PE.

**Recette de diagnostic** : x32dbg doit casser sur le crash — onglet
**Exceptions** (pas Events) sur `0xC0000409`/`0xC0000005`. Puis mapper
`EIP − base_module` (RVA) contre `build/src/Release/ddraw.map` (base préférée
0x10000000, plus grand symbole ≤ RVA). Sans débogueur, l'Observateur
d'événements (Application, id 1000) donne le module fautif + l'offset.

## 4. Ne jamais toucher à l'intégrité

Ne pas éditer `moonlight/conf/bourgeon_integrity.conf` (`enforce` / `hash`), ne
pas pousser une nouvelle empreinte au serveur, **et ne pas continuer à le
proposer**. L'utilisateur fait tourner un serveur de PRODUCTION avec de vrais
joueurs où `enforce: 1` + l'empreinte approuvée sont porteurs.

Le dev local est exempté par `exempt_ip: 127.0.0.1` : les rebuilds marchent sans
aucun changement d'intégrité. Le build imprime l'empreinte et écrit le sidecar
`ddraw.dll.sha256` — il ne réécrit pas la conf serveur, et ne doit pas.

## 5. Commits : français, sur `master` — sauf dépôt communautaire

L'utilisateur committe **directement sur `master`** et ne veut pas être basculé
sur une branche de feature (dépôt perso, workflow solo). Ne PAS appliquer la
règle par défaut « brancher avant de committer sur la branche par défaut ».

**Messages en français**, préfixe conventional-commit gardé (`fix(inventory):`) —
il est neutre — plus le trailer Co-Authored-By.

🔴 **Exception : les dépôts COMMUNAUTAIRES restent en ANGLAIS.** Le français vaut
pour Bourgeon, moonlight, moonlightsite. **Pas pour [[reference_warp0716]]** ni
tout dépôt destiné à une PR en amont. Erreur commise le 2026-08-15
(`RestoreGMWeaponTrail`), **refaite le 2026-08-16** (`RestoreSkillInfoCheckbox`)
parce que le rappel ne citait que « français + Discord » : l'exception était
invisible avant d'ouvrir le fichier. ➡ **Avant de rédiger, regarder DANS QUEL
DÉPÔT on commite**, pas seulement quelle langue on parle.

🔴 **Le corps du commit est relayé sur DISCORD** (règle 2026-08-01) et lu comme du
markdown Discord. Écrire en **texte brut**. Les deux vrais coupables, capture à
l'appui :
1. **les backticks s'apparient À TRAVERS LES LIGNES** — un paragraphe au nombre
   impair fusionne avec le suivant ;
2. **les underscores des identifiants passent en italique** — `CZ_CLOSE_DIALOG`
   s'affiche « CZ_CLOSE*DIALOG* ».

➡ Identifiants **nus**, sans backticks ni emphase : CZ_CLOSE_DIALOG, gid_,
sd->npc_id. À éviter aussi : `#` en début de ligne, `>`, `~~`, et les tableaux
markdown (Discord ne les rend pas). Ce qui passe : tirets de liste, flèches `->`,
guillemets français, puces `·`, blocs indentés.

## 6. Fichier partagé avec une autre session : stager SES SEULS hunks

Une autre session Claude Code travaille souvent en parallèle dans le même dépôt.
`moonlight_ui.cc`, `tools/lang/en.yaml` / `es.yaml`, les panneaux sont partagés.
`git commit --only -- <chemin>` **ne suffit pas** : il prend le fichier ENTIER du
disque, donc leurs lignes avec.

🔴 **Le danger n'est pas social, il casse master** : une entrée de table qui
référence un champ ajouté par l'autre session à un header **non committé** ne
compile pas. Vérifier : `git show HEAD:<leur_header> | grep <le_champ>` — vide =
ne pas committer ce fichier tel quel.

1. **Repérer** : `git diff -U0 -- <f> | grep -E "^[+-]" | grep -v "^[+-][+-]"` —
   tout ce qui ne parle pas de ton sujet est à quelqu'un d'autre.
2. **Code, hunks séparables** : extraire le diff, retirer leurs hunks,
   réappliquer à l'index seul — `git apply --cached --check --recount` d'abord,
   puis sans `--check`. ⚠ Ne pas relire le patch avec `ReadAllLines` : ça mange
   les CR et il ne s'applique plus.
3. **Données en AJOUT pur** (catalogues i18n) : les hunks s'entremêlent,
   reconstruire le blob — `git show HEAD:<f> > base` (⚠ le blob est en LF même si
   le disque est CRLF), concaténer son bloc, `git hash-object -w --no-filters`,
   `git update-index --cacheinfo`.
4. **Contrôler avant ET APRÈS.** 🔴 Le contrôle d'avant ne suffit pas :
   **l'index est PARTAGÉ**, rien n'est atomique entre le `diff --cached` et le
   `commit`. Vécu le 2026-08-10 : index vérifié à 5 fichiers, commit sorti à 9 —
   l'autre session avait fait son `git add` dans l'intervalle. **Toujours relire
   `git show --stat HEAD` juste après.** Réparation en UNE commande pour réduire
   la fenêtre : `git reset --soft HEAD~1 && git restore --staged <leurs fichiers>
   && git commit -F msg` (ni l'un ni l'autre ne touche au working tree).
5. **Vérifier l'arbre committé sans salir le sien** : `git worktree add --detach`
   sur le commit, y lancer les vérificateurs, puis `git worktree remove --force`.

---

Voir aussi [[feedback_code_hygiene]], [[feedback_debug_tooling]],
[[reference_moonlight_server]].
