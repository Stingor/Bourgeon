# Découvrir Bourgeon — comment fabriquer le contenu

Ce dossier est livré tel quel dans `<jeu>\data\bourgeon\tutorial\`. Il porte le
texte de la visite guidée (`tutorial.yaml`) et ses animations (`*.gif`).

La fenêtre qui les affiche est `src/features/windows/tutorial_window.cc`.

## Dans le GRF ou sur le disque, indifféremment

La fenêtre lit ces fichiers **par le VFS du client**, celui-là même qui charge
les sprites : il regarde le **disque d'abord**, puis les **GRF montés**. Le
patcher peut donc packer le dossier dans `moonlight.grf` — c'est ce qui arrive
chez le joueur — sans que rien ne change pour qui travaille avec le dossier
ouvert sur son disque, qui garde la priorité.

Deux conséquences pour les noms de fichiers :

- **minuscules et ASCII.** Une entrée de GRF est stockée en minuscules ; le
  reste du VFS est en CP949 quand nos sources sont en UTF-8, donc un `é` dans un
  nom de gif ne désignerait la même entrée dans aucun des deux.
- **le `gif:` d'une page est un nom, pas un chemin.** Il est résolu dans ce
  dossier-ci.

## Tourner un GIF

Les animations se filment **avec le jeu**, par l'enregistreur de zone de
Bourgeon — l'outil a été écrit pour ça, et c'est pour cette raison qu'il
capture APRÈS notre overlay : le sujet du tutoriel, c'est justement notre
interface.

1. Panneau Bourgeon → **Gameplay → Enregistreur de zone**.
2. Tracer la zone à filmer (la touche « retracer la zone » évite d'avoir à
   rouvrir le panneau, qui recouvre justement ce qu'on veut cadrer).
3. Se placer, puis lancer l'enregistrement à la touche configurée.
4. Le gif atterrit dans `<jeu>\screenshot\`. Le renommer selon le `gif:` de sa
   page et le déposer ici.

### Le format que la fenêtre sait afficher

Les réglages **d'usine** de l'enregistreur sont exactement ce qu'il faut :

| Réglage | Valeur | Ce qui se passe au-delà |
|---|---|---|
| Durée | 6 s | tronqué à 6,4 s (64 images à 10 i/s) |
| Images par seconde | 10 | moins d'images tiennent dans la durée |
| Largeur | 640 px | réduit pour tenir dans le budget mémoire |

Ces plafonds sont dans `kGifLimits` (`tutorial_window.cc`) et ils ne sont pas
décoratifs : une page charge son animation **entière** en mémoire et en VRAM,
dans un processus 32 bits. C'est aussi pourquoi une seule animation vit à la
fois — changer de page décharge la précédente.

**La taille d'affichage s'ajuste toute seule** : le décodeur calcule la
réduction qui fait tenir les N images dans le budget, plutôt que de refuser
l'animation. Le cadrage décide donc de la définition finale — un clip 16:9 sort
à 360 px de large, un clip CARRÉ de même durée à ~320 px, parce qu'une image
carrée couvre 1,75 fois plus de surface à côté égal. Cadrer serré sur la fenêtre
montrée est ce qui rend l'image nette.

### Ce qui fait une bonne animation

- **Un seul geste par page.** Ouvrir la fenêtre, faire la chose, refermer.
- **Partir d'un écran calme** : pas de combat derrière, pas de chat qui défile —
  le GIF est en palette réduite, un fond agité le fait grossir et scintiller.
- **Cadrer serré** sur la fenêtre concernée. Le reste de l'écran ne raconte rien
  et coûte des pixels.
- Laisser une seconde d'immobilité à la fin : l'animation boucle, et la coupure
  se voit moins.

## Écrire une page

```yaml
  - id: chat                 # identifiant STABLE : c'est la clé de reprise.
    title: "La chatbox"      #   Le renommer renvoie au début les joueurs qui
    gif: "02_chat.gif"       #   s'étaient arrêtés là.
    body: |
      Un paragraphe, puis un autre. Les **mots importants** en gras.
      La touche du joueur s'écrit {touche:win_inventory}, son nom {perso}.
    bullets:
      - "Une idée par puce."
    tip: "Une remarque de fin, encadrée."
```

- **Toute valeur de texte se met entre guillemets** (`title`, `tip`, chaque
  puce). Sans eux, YAML lit du balisage dans la phrase : une valeur ouverte
  par `**gras**` est prise pour un *alias* et fait refuser le fichier entier ;
  une valeur contenant ` : ` devient une paire clé/valeur et s'affiche **vide**,
  sans erreur. Les blocs `body: |` sont à l'abri : tout y est du texte.
- Dans un bloc `body: |`, les retours à la ligne simples sont **recollés** au
  rendu — c'est la ligne vide qui sépare deux paragraphes.
- `gif`, `bullets` et `tip` sont facultatifs. Une page sans gif reste lisible.
- Les identifiants d'action utilisables dans `{touche:...}` sont ceux du
  catalogue, dans `src/features/hotkey_actions.cc` (`win_inventory`,
  `win_sheet_stats`, `tool_craft_atlas`…). Un id inconnu s'affiche
  « (action inconnue) » — donc ça se voit en jeu, et ça se corrige.
- Une action sans touche affiche « (aucune touche) » : c'est voulu, ça dit au
  joueur qu'il peut lui en donner une.

## Ajouter une page à une visite déjà publiée

Incrémenter `version:` en tête du fichier. C'est ce nombre, comparé à
`tutorial_seen_version` du `bourgeon_settings.yaml` de chaque joueur, qui fait
**rouvrir** la visite chez ceux qui l'avaient déjà parcourue. Corriger une faute
de frappe ne le justifie pas ; ajouter une nouveauté, si.

## Traduire

Déposer `tutorial.en.yaml` (ou `.es.yaml`) à côté : la langue de l'interface
choisit le fichier, et retombe sur `tutorial.yaml` si la traduction manque. Le
catalogue `SaveData\lang\*.yaml` ne convient pas ici — il traduit des littéraux
du code, pas des données.
