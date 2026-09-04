# Hygiène du code Bourgeon

> Journal du chantier. La fiche de mémoire `feedback_code_hygiene` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.


Quatre défauts d'écriture qui ne se voient ni à la compilation ni à la relecture,
plus la règle de nommage du projet. Fiche jumelle sur la même famille :
[[feedback_python_write_truncates]] (`open(p,'w')` VIDE le fichier).

## 1. Noms explicites, et zéro constante magique

Les identifiants doivent être **explicites pour un lecteur humain**, pas seulement
corrects. Beaucoup de variables écrites par Claude sont trop vagues (`n`, `v`,
`e`, `t`, `it`, `res`, `tmp`, `data`, `buf`, `val`, `flag`, `ok`) : elles ne
disent ni ce qu'elles contiennent, ni son unité, ni sa provenance. **Renommer au
passage** dès qu'on touche à une zone.

Le projet a dépassé 53 000 lignes et le coût dominant est devenu la *relecture* :
un nom vague oblige à remonter la fonction entière pour comprendre une ligne.
**Le nom est le commentaire qui ne périme jamais.**

- Le nom porte **contenu + unité + origine** : `elapsed_ms` plutôt que `t`,
  `item_id` plutôt que `id`, `slot_count` plutôt que `n`.
- Un nom d'adresse RE dit la **cible** : `kActorDrawAddr`, pas `kFn3`.
- Les booléens se lisent comme une assertion : `is_map_loading`,
  `has_costume_slot`, `should_repaint`.
- Tolérés : indices de boucle courts sur quelques lignes, et les conventions déjà
  en place dans le fichier (**rester cohérent l'emporte**).
- Renommage **opportuniste** : on renomme ce qu'on touche, pas de passe globale
  qui polluerait le diff.

🔴 **Pas que les noms — le CODE doit rester lisible** (2026-08-03). « Permet moi
de tirer la sonette d'alarme : c'est quoi tous ces chiffres ? » — j'avais écrit
une clé d'invalidation de cache en empilant des multiplicateurs de hachage
(`2654435761u`, `3266489917u`…). Illisible, et **faux par principe** : un hachage
peut entrer en collision, donc deux équipements différents pouvaient passer pour
le même. Remplacé par une petite `struct` comparée champ à champ.

- **Zéro constante magique** : un nombre qui n'est ni une adresse RE, ni un offset
  documenté, ni une unité évidente doit être une constante nommée — ou disparaître.
- **Ne pas hacher pour comparer** : comparer des champs coûte moins à écrire, à
  relire et à corriger, et ne collisionne jamais. Le hachage ne se justifie que
  sur un volume qui l'exige, jamais sur huit champs.
- **Pas d'astuce là où une ligne banale suffit.**
- Si une mécanique reste subtile parce que le natif l'est (z-order, ancrage), ce
  n'est pas le code qu'on complique : c'est le commentaire qui porte la MESURE.

## 1 bis. Un commentaire dit la RÈGLE, pas l'HISTOIRE du chantier

🔴 Repris le 2026-09-01, en pleine standardisation de `panel_interface.cc` :
« commenter sur pourquoi le code est arrivé là, c'est valable tout de suite, mais
si on le lit dans 1 mois on se demandera l'intérêt de savoir ce qui s'est passé ».

J'avais rédigé des en-têtes qui racontaient le refactor — « il vivait EN DUR dans
panel_interface.cc, 217 lignes qui tripotaient `pf->col_hp_high_` depuis
l'extérieur », « trois plugins portaient EN PLUS leur propre notice ». Vrai le
jour même, inerte une semaine plus tard : personne ne relit un fichier pour
apprendre ce qu'il N'EST PLUS.

**Le test** : ce commentaire aide-t-il quelqu'un qui n'a jamais vu l'ancien code ?

- ✅ Ce qui reste : la règle en vigueur (« le test du groupe est fait au site
  d'appel unique »), la contrainte qui a encore un effet (« le blocage NPC doit
  rester réglable hors interface moderne, sinon plus moyen de débloquer un NPC »),
  le piège qui peut mordre demain (la collision de `PushID` par la traduction).
- ❌ Ce qui part : « il vivait avant à tel endroit », « X lignes qui faisaient Y »,
  « le passage précédent avait laissé », les dates de déménagement.
- ⚠ Nuance : le POURQUOI d'une décision reste, c'est son RÉCIT qui part. « Hors du
  groupe : elle ne remplace aucune fenêtre » se garde ; « elle en a été retirée le
  18/08 après discussion » non.

Ça vaut aussi pour les messages de commit, qui eux SONT le bon support du récit —
c'est là que « ce bloc vivait dans panel_interface.cc » a sa place, pas dans le
code.

## 2. 🔴 Un commentaire `//` qui FINIT par `\` avale la ligne suivante

La continuation de ligne (phase 2 de traduction) est appliquée **AVANT** la
reconnaissance des commentaires (phase 3) : le compilateur voit une seule ligne de
commentaire, et **la ligne suivante disparaît du programme**.

Vécu le 2026-08-11 dans mon propre patch `M_GetSaveGameDir` (m_config.c) :

```c
// first. eg. doom\.savegame\doom2.wad\      <- l'antislash final
topdir = M_StringJoin(configdir, ".savegame", NULL);   <- JAMAIS COMPILÉE
```

⇒ `topdir` non initialisé, `M_MakeDirectory(topdir)` sur une valeur de pile
(0x30), `c0000005` dans `mkdir` dès l'ouverture de DOOM. L'utilisateur avait juste
posé son doom.wad.

Rien ne le signale : MSVC a bien le warning **C4010**, mais
`thirdparty/doomgeneric` est compilé avec **`/w`** — tous les warnings éteints. Le
code manquant ne laisse **aucune trace, pas même la chaîne littérale dans le
binaire**.

- Ne jamais terminer un `//` par `\`. Un chemin Windows en commentaire se
  reformule ou se met entre guillemets sans antislash final.
- Contrôle mécanique après toute écriture de commentaire, y compris dans le
  vendored : `git diff -U0 | grep -nE '^\+.*\\\s*$'` — zéro ligne attendue.
- Symptôme à reconnaître : une variable locale « impossible » à ne pas être
  initialisée, **et la chaîne littérale absente du binaire**
  (`[IO.File]::ReadAllBytes` + `IndexOf` sur la DLL déployée l'a prouvé en une
  commande).
- Même piège en `#define` et dans les macros multi-lignes.

## 2 bis. 🔴🔴 Un `__try` qui couvre PLUSIEURS étapes cache laquelle a échoué

Le projet garde ses chaînes de pointeurs natives sous `__try/__except` — c'est
juste, elles ne sont valides qu'une fois dans le monde. Mais un bloc qui couvre
une SUITE d'étapes transforme la moindre lecture fautive d'une étape en
**abandon silencieux de toutes les suivantes**. Pas de crash, pas de journal :
la fonctionnalité ne fait simplement rien, et on cherche la cause ailleurs.

Vécu le 2026-09-01 ([[project_grey_world]]) : une fonction déréférençait
`C3dGround15 + 8` en croyant y trouver un pointeur — ce champ vaut une LARGEUR.
La lecture partait dans le vide, `__except` l'avalait, et l'appel VRAIMENT
important, placé en dernier, n'a jamais tourné. Quatre hypothèses fausses et
trois correctifs inutiles ont suivi, jusqu'à ce qu'x32dbg montre la structure
en mémoire.

➡ **Mettre EN TÊTE du bloc ce qui doit aboutir coûte que coûte**, et n'exiger
pour lui que ce dont il a besoin — pas la chaîne complète.
➡ Se méfier d'un offset DÉDUIT d'un désassemblage (« la fonction lit `this+8`,
donc c'est un pointeur ») : lire la mémoire une fois coûte moins qu'une
supposition, et [[feedback_debug_tooling]] dit par où.
➡ Symptôme typique : « mon correctif ne change RIEN au symptôme ». Avant
d'empiler une hypothèse de plus, vérifier qu'il s'EXÉCUTE.

## 3. `memset(0)` sur une struct à sentinelles demande « la valeur 0 »

`std::memset(&x, 0, sizeof(x))` écrit des ZÉROS. Si la struct porte des champs
dont l'initialiseur par défaut est **-1** (« rien d'imposé », « garder
l'existant »), le memset ne les remet pas à leur défaut : il demande explicitement
**la valeur 0**, en général légitime et différente.

Payé le 2026-08-12 sur `ro::PaletteRecipe` (`palette_id = -1`,
`hair_palette_id = -1`) : « tout réinitialiser » faisait un memset ⇒ palette 0. Le
symptôme n'accusait rien — les curseurs retombaient bien à « d'origine », mais le
personnage ne changeait pas, la BASE restant fusionnée sur l'ancienne palette. Un
bouton « réinitialiser » qui ne réinitialise rien de visible, sans erreur.

⚠ L'affichage `valeur > 0 ? valeur : 0` **masque la différence entre 0 et -1** :
le curseur ment de bonne foi. **Ne jamais conclure de l'état affiché que la donnée
est correcte.**

- Remise à zéro d'un agrégat C++ = `x = T();`, **jamais** `memset`.
- `memset` reste bon pour un tampon d'octets réseau, ou une struct dont tous les
  défauts sont réellement 0.
- Corollaire : quand une valeur « change » de sentinelle, grepper TOUS les
  `memset` de cette struct — il y en avait trois, dont un dans le chemin
  « changement de classe » qui n'aurait sauté qu'en montant à cheval.

## 4. Un script Python qui réécrit des sources : deux pièges

**Les fins de ligne.** `io.open(p, encoding="utf-8")` traduit les CRLF en `\n` À
LA LECTURE ; réécrire avec `newline=""` produit un fichier tout en LF. Le diff
passe de 3 lignes à 318 et noie la vraie modification. Le dépôt est **mixte** — la
plupart des fichiers sont en LF, mais certains (`src/utils/log_console.cc`) sont
en CRLF, et rien ne le signale avant le `git diff --numstat`.

➡ Après tout script qui réécrit des sources, contrôler `git diff --numstat` : un
nombre de lignes proche du total du fichier = fins de ligne converties. Mieux :
**lire ET écrire en binaire** quand on ne fait que des substitutions ASCII.

🔴 **Les séquences d'échappement** (découvert le 2026-07-28, deux fois dans la
même session). Un `\n` écrit dans un heredoc Python lancé via l'outil Bash
(`python - <<'PY'`) arrive **converti en vrai saut de ligne** dans le fichier,
malgré le heredoc quoté. Résultat : `error C2001: saut de ligne dans une chaîne
littérale`, et l'utilisateur découvre l'erreur à SA build.

➡ Ne JAMAIS écrire de séquence d'échappement dans un littéral C++ via un heredoc —
utiliser l'outil Edit, qui écrit littéralement. Après tout script d'insertion,
compter les guillemets non échappés par ligne : un nombre **impair** = littéral
coupé.

🔴🔴 **Reproduit une TROISIÈME fois le 2026-08-17** (`ShowWarning(... '%s'.\n")`
dans `naviregisterwarp`), toujours par heredoc. La règle ci-dessus ne suffit
manifestement pas à me retenir, donc la voici en positif — **la parade employée
sans incident tout le reste de cette session** : quand un script doit insérer du
code, l'écrire dans un **FICHIER** avec l'outil Write puis lancer `python
mon_script.py`. Le heredoc `python - <<'PY'` est le seul coupable ; passer par un
fichier n'a jamais mangé un échappement. Réserver le heredoc aux scripts SANS
antislash, et sinon : Write + exécution, ou Edit.
🔴🔴 **ET IL DOUBLE AUSSI** (2026-08-25, règle ci-dessus ENFREINTE puis repayée).
Un heredoc générant du Python a transformé `r"\w+"` en `r"\\w+"`. Dans une
chaîne **raw**, `\\w` ne veut pas dire « \w » : il veut dire « un antislash
littéral, puis w ». La regex devient valide et **ne matche JAMAIS**. Le relevé a
rendu 0 partout, sans une erreur, et seul un `assert` a empêché d'écrire.
➡ Le symptôme d'un antislash doublé n'est pas un plantage, c'est un **résultat
vide**. Devant un relevé qui rend 0 alors qu'il devrait trouver, **afficher le
`.pattern` de la regex** avant de soupçonner les données.
➡ Et pour corriger un script déjà écrit : `Write` du fichier ENTIER. `Edit` bute
sur le même mur (l'échappement JSON de `old_string` doit lui aussi doubler).


🔴🔴 **LE HEREDOC EST DÉSORMAIS BLOQUÉ PAR UN HOOK** (2026-08-29). Après une
QUATRIÈME occurrence dans la même journée (un `\n` de `ShowInfo(...)` devenu un
vrai saut de ligne, erreur `C2001` à la build de l'utilisateur), il a demandé :
« tu peux pas bannir heredoc de tes outils, ça arrive sans arrêt ce problème ».

La règle ne vit plus dans cette fiche mais dans le **harnais** :
`~/.claude/hooks/no_heredoc.py`, branché en `PreToolUse` / matcher `Bash` dans
`~/.claude/settings.json`. Il refuse toute commande contenant `<<EOF`, `<<'EOF'`,
`<<"EOF"` ou `<<-EOF`, et rend le motif de remplacement dans son message.

⚠ Un refus de ce hook n'est **pas une panne** : c'est le garde-fou qui joue son
rôle. Ne pas chercher à le contourner — écrire le script avec **Write** dans le
scratchpad puis l'exécuter, et pour un message de commit, Write + `git commit -F`.

⚠ Il ne matche pas `<<<` (here-string, une seule ligne) ni `a << b` (espace
avant le délimiteur), pour ne pas refuser des commandes légitimes.

➡ Contrôle mécanique après coup, qui aurait attrapé les trois fois :
`re.finditer(r'ShowWarning\("[^"]*\n', src)` — ou plus large, toute chaîne
ouverte qui rencontre une fin de ligne.

🔴🔴 **Troisième piège de la même famille (2026-08-22) : un backtick dans une
commande Bash entre guillemets DOUBLES.** `python -c "…"` dont le texte contient
une portion en `` `code` `` (Markdown !) déclenche la **substitution de
commande** de Bash. Vécu : le remplacement inséré dans un `.md` contenait
`` `docs/mvp_tracker_blueprint.md` `` — Bash a **exécuté le fichier Markdown
ligne par ligne**, la substitution a rendu du vide, et le lien a atterri amputé
(`**Le comment est dans ** :`).

⚠ Ce n'est pas qu'une perte de texte : les lignes de citation Markdown
commencent par `>`, que Bash lit comme une **redirection** — donc un tel
accident peut CRÉER ou TRONQUER des fichiers portant le nom du mot suivant. Le
seul symptôme visible était un déluge de `command not found`… suivi d'un `ok`
final, l'exécution ayant « réussi ».

➡ Même parade que ci-dessus : **Write + `python script.py`**, ou l'outil Edit.
Et pour tout texte destiné à du Markdown (donc plein de backticks), ne jamais
passer par `-c "…"`.

## 5. pymysql : `DictCursor` est global, pas de dépaquetage en tuple

Dans `groq_service.py` (relais Discord), `DB_CONFIG` pose
`cursorclass=pymysql.cursors.DictCursor` **globalement**. Dépaqueter un dict dans
`for a, b, c in rows:` rend les **clés** (« id », « player », « message »), pas
les valeurs.

Ça a fait apparaître les messages en jeu littéralement comme « player » et
« message » sur Discord, et `UPDATE WHERE id='id'` ne correspondait à rien, donc
`sent` restait à 0 → **boucle de repost infinie**.

```python
for row in rows:
    row_id  = row["id"]
    player  = row["player"]
    message = row["message"]
```

## 🔴🔴 `grep -c $'
'` dans Git Bash MATCHE TOUTES LES LIGNES

Contrôle de fins de ligne écrit trois fois dans la session du 2026-08-24 :

```sh
l=$(wc -l < "$f"); c=$(grep -c $'
' "$f")
[ "$l" = "$c" ] && echo ok || echo MIXTE
```

Il affichait **ok** sur tous les fichiers — et il ne testait **RIEN** : le motif
part vide, et un motif vide matche chaque ligne, donc `c` vaut toujours `l`. Un
fichier entièrement converti en LF serait passé pour intact.

⇒ Compter les CR par **octet** : `tr -cd '
' < "$f" | wc -c`. Zéro = LF pur.
⚠ Corollaire mesuré : sur ce dépôt les sources ET les catalogues sont en **LF**,
et l'avertissement git « LF will be replaced by CRLF » est le comportement NORMAL
de `core.autocrlf=true` — pas le signe d'un fichier abîmé.
🔴 La leçon générale : **un contrôle qui passe toujours ne prouve rien.** Le
soumettre à un cas qu'il DOIT rejeter avant de lui faire confiance.

🔴🔴 **LE CONTRÔLE DE FINS DE LIGNE EN BASH NE PROUVE RIEN** (re-payé le
2026-08-25). `cr=$(grep -c $'\r$' f); tot=$(wc -l < f); [ "$cr" = "$tot" ]`
affiche « OK » sur TOUS les fichiers, CRLF ou LF — le motif ne s'interprète pas
comme espéré et matche tout. J'ai déclaré quatre fichiers homogènes alors que je
ne mesurais rien.
➡ **Compter les OCTETS, en Python** : `b.count(b"\r\n")` et
`b.count(b"\n") - b.count(b"\r\n")`. Trois classes à distinguer : CRLF pur,
LF pur, **MIXTE** (le seul vraiment grave).
➡ ⚠ Et un fichier en LF pur n'est PAS forcément ma faute : `git diff --stat` le
tranche en une commande — une conversion d'eol ferait apparaître le fichier
ENTIER comme modifié. Diff normal = l'état était préexistant.

---

Voir aussi
---

Voir aussi [[feedback_build_and_git]], [[feedback_re_method]],
[[project_discord_relay]].
