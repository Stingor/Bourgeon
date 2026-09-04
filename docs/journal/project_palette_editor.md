# Éditeur de palette in-game (branche palette-editing)

> Journal du chantier. La fiche de mémoire `project_palette_editor` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

# Éditeur de palette in-game

Branche **`palette-editing`**, ouverte le 2026-08-11 après le rejet de la
solution à 149 fichiers de [[project_4th_job_body_palettes]]. Idée de
l'utilisateur : un éditeur qui recolore le personnage **en direct**, puis envoie
la palette au serveur qui la propage aux autres clients (qui la mettent en
cache).

## 🔴 La contrainte qui cadre tout

**Le système de palettes natif doit continuer à fonctionner quoi qu'il arrive,
sans modifier les palettes existantes** (utilisateur, 2026-08-11). Donc : notre
code ne dévie QUE pour un acteur qui a une recette ; tout le reste emprunte le
chemin natif intact ; aucun `.pal` n'est écrasé, aucun patch WARP retiré.

⚠ Conséquence assumée : un joueur qui n'ouvre pas l'éditeur **garde son corps
noir**. Le dossier des 4e classes n'est donc PAS résolu pour tous — seulement
pour qui personnalise. Corriger le défaut serait une décision SÉPARÉE.

## Décisions produit (utilisateur, 2026-08-11)

1. **Le corps seul** (la tête garde `head_<n>.pal`, qui marche).
2. **Teinte par rampe, en HSV** — 3 curseurs par rampe, décalages RELATIFS.
3. **Une recette compacte** sur le réseau, PAS la palette : chaque client la
   ré-applique sur la palette interne du `.spr` qu'il possède déjà.

## 🔴 Le mécanisme d'injection — SANS AUCUN FICHIER

Tout repose sur un fait vérifié **en mémoire live** (jeu attaché à x32dbg) :

| Quoi | Adresse / offset | Note |
|---|---|---|
| `Palette_ConvertRgbaToArgb1555` | **0x566770** | `(dst, srcRGBA, 256)`. A1 R5 G5 B5. Magenta pur → **`0xFC1F`** |
| `CPaletteRes_Load` | 0x725b60 | `.pal` → **+0x110** (RGBA) puis **+0x510** (1555) |
| `CSprRes_Load` | 0x7282d0 | palette INTERNE du `.spr` → **+0x110** (déjà en 1555) |
| `CActorSprite_GrayscalePalette` | 0xc46550 | **modèle** : relit la table vivante, la grise, la réécrit EN PLACE |
| Point d'injection | **0x6060b1** | `add eax, 0x510` dans `CActorSprite_BuildPartQuads` |
| `SpriteAtlas_GetCachedTexture` | 0x566b70 | clé = **paire {frame, palette}**, hash FNV |

**Les trois branches de `CActorSprite_BuildPartQuads` (0x605c30)**, d'où tout
part :
* partie 0 (CORPS) et `acteur+72` (couleur de vêtement) > 0 → charge `acteur+0x1C`
* partie 1 (TÊTE) et `acteur+1084` > 0 → charge `acteur+0x470`
* **sinon → `param_5 + 272`, la palette INTERNE du sprite**

⇒ Le rendu ne connaît qu'un **pointeur brut** vers une table ARGB1555, et ce
pointeur sert AUSSI de clé de cache. Donc **une palette par joueur = un bloc
mémoire par joueur = une texture d'atlas distincte, gratuitement**. Il suffit de
fabriquer 1024 o RGBA, d'appeler `Palette_ConvertRgbaToArgb1555`, et de fournir
le pointeur. Zéro fichier, zéro déploiement.

Mesures live du 2026-08-11 (perso Creator) : acteur vtable **0x01094810**,
`+0x1C` = `몸\body_56.pal` (std::string SSO), `+0x48` = **56**, slots `.spr` à
`+0x4AC` (slot 0 = corps), `CSprRes+0x110` = 256 dwords à 16 bits hauts nuls.
`add eax, 0x510` **intact en RAM** ⇒ WARP ne patche pas ce chemin, l'IDB fait foi
ici (contrairement à [[reference_ida_is_vanilla_warp_patches]]).

⚠ Ceci explique le crash du dossier précédent : vider le chemin donnait une
ressource nulle, donc `nullptr + 0x510` = l'adresse `0x510` relevée. La branche
« palette interne » se déclenche sur `acteur+72 == 0`, PAS sur un chemin vide.

## 🔴🔴 Le point d'injection : ce que la reconnaissance a tranché (2026-08-11)

**Le design « hooker `UITextureMgr_LoadResourceByName` » est MAUVAIS.** Trois faits
mesurés au désassemblage l'écartent :

1. **81 sites d'appel**, 20 familles de ressources (sprites, textures, `.str`,
   emblèmes, cartes), sous section critique. **Seuls 4 concernent une palette** —
   repérables au `add eax, 0x510` qui suit : `0x0060562c`, `0x006060ac`,
   `0x007acdb1`, `0x007ae132`. ⇒ ~95 % de trafic étranger intercepté pour rien.
2. 🔴 **Son 2e argument est une `std::string` PAR VALEUR** (24 o, `retn 18h` à
   `0x00a8dbe2`), **détruite par le CALLEE**. Lui passer une chaîne construite par
   NOTRE runtime = `operator delete` du CRT du jeu sur notre tas = corruption.
   Tout chemin de palette dépasse 15 caractères, donc jamais en SSO. Hex-Rays
   n'expose qu'un `int *param_1` : encore un cas où le `ret N` fait foi.
3. 🔴 **Elle a un JUMEAU** : `UITextureMgr_Load` (`0x00a8d4a0`, `retn 4`, un seul
   pointeur) charge AUSSI des palettes. Hooker l'une sans l'autre = borgne.
   ⇒ Le point commun des deux est **`UITextureMgr_FindCachedRes` (0x00a8d3e0)**,
   qui reçoit le chemin déjà normalisé (4 appelants au total).

**🔴 LE VRAI OBSTACLE, et il est structurel** : le cache de ressources est indexé
par **CHEMIN normalisé**, et le chemin ne dépend que de (job, sexe, couleur de
vêtement, bodyStyle). **Deux joueurs identiques partagent donc forcément la même
`CPaletteRes`** — écrire dedans les repeindrait tous, ce qui viole la contrainte.
Il faut donc DEUX accroches : une qui rend le chemin **unique par recette**, une
qui **fabrique** la ressource pour ce nom synthétique. Le producteur unique du
chemin est `Job_BuildBodyPalettePath_impl` (0xb42580, **2** appelants seulement).

⚠ Mais `Job_BuildBodyPalettePath_impl` ne reçoit PAS l'acteur (juste job/sexe/
couleur) : elle ne peut pas savoir DE QUI il s'agit. L'identité n'est connue que
plus haut — `CActorSprite_RebuildBodyPalettePath` (0xd3dc90) a `this` = l'acteur
et écrit `acteur+0x1C`. C'est là qu'il faut poser le chemin synthétique.

Autres faits durs de cette passe :
* **Muter une palette EN PLACE ne se voit pas** : la texture d'atlas est cuite une
  fois pour la clé {frame, palette} et n'est refaite qu'à l'éviction, purement
  temporelle. ⇒ **changer le POINTEUR à chaque version de recette**, jamais les
  octets d'un bloc déjà affiché.
* **Aucun comptage de références** : `UITextureMgr_Release` (0xa8f4b0) DÉTRUIT.
  Et la purge `sub_A8F810` détruit toute ressource dont **+0x04 == 0**.
  ⇒ **ne rien inscrire dans le cache natif** ; substituer seulement la valeur de
  retour.
* Le pointeur `+0x510` est **retenu dans la file de rendu différée** jusqu'au
  flush ⇒ notre bloc doit survivre à toute la frame (au moins).
* `sizeof(CPaletteRes)` = **0x910** ; vtable **0x01011dbc** (8 entrées) ; les
  consommateurs n'appellent **aucun virtuel** sur l'objet — un bloc à nous, jamais
  inscrit au cache, n'a donc pas besoin d'une vtable valide (mais la poser ne coûte
  rien et rend l'objet compatible avec toutes les routes).
* Chaîne **vide ⇒ retour NULL immédiat** (`0x00a8da30`) — l'explication définitive
  du crash de [[project_4th_job_body_palettes]]. Et le `+0x510` est appliqué
  **sans test de nullité** sur presque tous les sites ⇒ notre détour ne doit
  **JAMAIS** rendre 0 ; en cas d'échec, retomber sur le retour natif.
* 🔴 **`acteur+0x48` (couleur de vêtement) == 0 ⇒ AUCUNE palette externe** : le
  natif prend la palette interne du `.spr`. Un porteur de recette dont la couleur
  vaut 0 serait donc ignoré. Parade : lui forcer une couleur non nulle, ou
  intervenir aussi sur la branche de repli.
* La couche **ARME** (`CActorSprite_RenderWeaponLayerBillboard` 0xd3b48c) lit le
  MÊME `acteur+0x1C` que le corps : une recette de corps repeint aussi l'arme.
  C'est déjà le comportement natif, donc le préserver ne demande rien.
* Les fenêtres d'aperçu qui **rangent** le chemin (`UIMakeCharWnd_ctor` 0x86c027,
  `UINewMakeCharWnd_ctor` 0x79fca2, `UINewSelectCharWnd_BuildPage` 0x79cd29,
  `sub_8EE980` 0x8ef457) ne le reconstruisent PAS chaque frame ⇒ il faudra forcer
  leur rafraîchissement quand la recette change. L'`Actor` d'aperçu (152 o, sur la
  PILE, `Actor_Init` 0x7ac210 → `Actor_DrawSprites` → `Actor_Dtor`), lui, le
  reconstruit à chaque frame : champs +0x30 couleur vêtement, +0x50 chemin `.pal`
  du corps.

## ✅ Mesures en mémoire live (2026-08-11, jeu attaché)

**L'index de type de l'extension `pal` vaut 16**, confirmé TROIS fois : rang dans
le vecteur des prototypes, préfixe correspondant, et champ `+0x0c` d'un objet réel.

Gestionnaire singleton **0x0159D450** (adresse fixe, PAS un pointeur) :
`+0x04` = **20** types ; `+0x08` vecteur des préfixes (20 × 4 o) ; `+0x14` vecteur
des prototypes (20 × 4 o) ; `+0x20` vecteur des sets de cache (20 × 8 o) ;
`+0x2c` CRITICAL_SECTION. Prototype `pal` = `0x013178E8`, dont le 1ᵉʳ dword est
bien la vtable `0x01011dbc` ; préfixe[16] = `0x01011ddc` = `"palette\"`.

**Disposition d'un `CRes`, lue sur une `CPaletteRes` VIVANTE** (0x4942ae60) :
```
+0x00 vtable (0x01011dbc)   +0x04 pin anti-purge (=0 ⇒ purgeable)
+0x08 horodatage            +0x0c index de TYPE (=16)
+0x10 empreinte du chemin   +0x14 chemin résolu en clair
+0x110 RGBA (1024 o)        +0x510 ARGB1555 (256 dwords)
```
Le nœud du set de cache porte `+0x10` = pointeur vers `res+0x10` (la clé pointe
DANS la ressource) et `+0x14` = la ressource.

**Conversion vérifiée arithmétiquement sur données réelles** : RGB(200,248,211)
⇒ `R>>3=25, G>>3=31, B>>3=26` ⇒ `1 11001 11111 11010` = **0xE7FA**, exactement
l'octet lu à `+0x510`.

## ✅ Le diagnostic des corps noirs, PROUVÉ en live

`body_56.pal` lue en RAM (c'est la palette que l'acteur portait réellement) :
**126 de ses 255 index sont noirs ou quasi noirs**. Part des pixels peints qui
tombent sur un de ces index :

| Corps | % noirci |
|---|---|
| 크리에이터 (le perso en jeu) | 10,9 % |
| 기사 (classique) | 18,2 % |
| arch_mage | 38,3 % |
| dragon_knight | 40,7 % |
| **imperial_guard** | **62,2 %** |

⇒ La cause n'est pas « la palette est mauvaise » mais **« la palette externe ne
définit qu'une partie des index, et les 4e classes en utilisent bien d'autres »**.
C'est la confirmation directe, chiffrée et LIVE de [[project_4th_job_body_palettes]].

## L'algorithme de rampes — et pourquoi il doit être déterministe

Une palette RO est une suite de **dégradés** (un par pièce du costume), du clair
au foncé dans une teinte stable. Une rampe se coupe sur DEUX critères : écart de
teinte > 40° **ou** remontée de luminosité > 0,08. Plus : longueur ≥ 3, ≥ 200 px,
index réellement utilisés, tri **stable** par surface décroissante.

🔴 La recette ne porte **aucune couleur** — seulement des plages d'index et des
décalages. Deux clients doivent donc trouver EXACTEMENT les mêmes rampes à partir
des mêmes octets. `tools/palette_ramps.py` est la RÉFÉRENCE ; `src/ui/palette_ramps.{h,cc}`
en est la transcription. Toute modification doit être faite DES DEUX CÔTÉS.

**Validation croisée sur la chaîne ENTIÈRE** (2026-08-11) — fusion, détection,
recette dans les deux modes — sur 8 corps contrastés appariés à 8 `.pal`
différents : **sorties byte-exactes**, 2 Kio de palette + les rampes en texte par
cas. Le harnais a des dents (vérifié) : la fusion change 10 à 176 entrées, la
recette 31 à 57. Méthode de [[project_spr_act_own_parser]].

🔴 **Piège d'arrondi**, trouvé à cette occasion : `round()` de Python fait de
l'arrondi **bancaire** (`round(0.5) == 0`) là où `static_cast<int>(x + 0.5)`
arrondit toujours au-dessus. Les deux côtés utilisent désormais la seconde règle.
Vérifié au passage : l'aller-retour RGB→HSV→RGB **est** l'identité avec cette
règle (0 divergence sur 256×37×24 couleurs), donc un réglage neutre est un vrai
no-op — mais les deux côtés le **sautent** explicitement (`IsNeutral()`), pour que
la parité tienne par construction et pas par chance.

**Mesuré sur les 421 corps** : 0 sans rampe, couverture moyenne **86 %** (médiane
88 %) au plafond de 8. Le plafond est le coude de la courbe : 4 → 71 %, 6 → 81 %,
**8 → 86 %**, 10 → 88 %, 12 → 89 %. ⇒ `kMaxRamps = 8`, ce qui **dimensionne le
protocole**. Pires cas : les montures et `rebellion` (~49-57 %). 🔴 Les index n'ont
AUCUNE signification commune d'un corps à l'autre (Dragon Knight 7-73, Creator
112-223) : une recette ne vaut que pour SON sprite.

⛔ **Question CLOSE le 2026-08-12 — ne pas rouvrir sans un signalement de joueur.**
93 corps gardent une couleur vive hors des 8 rampes retenues, **médiane 1,6 % du
corps**. Les deux parades coûtent plus qu'elles ne rapportent : monter à 10 rampes
impose une **v7 du protocole** pour 2 points de couverture, et repondérer le
classement ferait passer des paillettes DEVANT des tuniques. Le classement v6
(`pixels × (256 + saturation moyenne)`) est le compromis retenu. C'est le bouton
de rapport de bug de la fenêtre de style qui rouvrira le dossier, si besoin.

### La fusion sprite + serveur (`MergeServerPalette`)

L'éditeur ne part **ni** de la palette du sprite **ni** de celle du serveur, mais
de leur fusion — et les rampes se détectent sur le RÉSULTAT, jamais sur le sprite
nu (les frontières ne sont pas les mêmes des deux côtés). 🔴 Le critère de « trou »
n'est pas « l'index est noir » — un noir est le plus souvent voulu, les sprites RO
ayant des contours noirs — mais **« TOUTE la plage contiguë est noire »**. Mesuré
sur les 421 corps : sans fusion 20,04 % des pixels tombent sur un index noir,
2,24 % avec le critère naïf (quatre octets nuls), **1,26 %** avec celui-ci.

## Pièges de terrain déjà payés

* 🔴 **`palettes.grf` n'est PAS chargé** (`DATA.INI` ne liste que `moonlight.grf`
  et `data.grf`). Y lire des `.spr` ne prouve rien — erreur commise puis
  corrigée ; l'utilisateur l'a depuis supprimé.
* `data.grf` est chiffré (« Event Horizon ») ⇒ les sprites doivent être
  **extraits** ; `tools/grf_reader.py` lit les GRF standard et règle
  disque-d'abord + les deux orthographes de dossier coréen
  ([[reference_data_folder_cp949_encoding]]).
* `max_cloth_color` = **553**, pas 7 — cf. [[feedback_rathena_conf_import_overrides]].
* `spract::Resource` est dans `ro::spract`, pas `ro`.
* ⛔ **Le magenta pur n'est PAS un danger ici** (question posée le 2026-08-11, et
  tranchée) : le convertisseur pose le bit d'alpha **inconditionnellement**
  (`(B | 0x40000) >> 3`, ce `0x40000` devenant le bit 15), donc magenta rend
  `0xFC1F`, opaque. Le seul chemin qui traite `(255,0,255)` comme transparent est
  `game_texture.cc` — le chargeur de **BMP d'interface**, où un sprite ne passe
  jamais. ⇒ Aucune raison de retirer cette couleur au joueur. Si un garde-fou doit
  exister, sa place est **côté serveur, à la validation de la recette diffusée**.

## ✅ Le protocole de partage (choix : le FLUX DE JEU, pas le web)

**v7, 56 o** (2026-08-15) : `[version:1][drapeaux:1][corps:u32][palette:i16][cheveux:i16][coiffure:i16][réglages:8×5]`.
**CZ 0x0F26** = ce bloc. **ZC 0x0F27** = `[nombre:2]` + N × `[gid:4]` + ce bloc.

🔴🔴 **UNE RECETTE PAR CORPS.** `corps` = `ro::BodySpriteKey` (FNV-1a du chemin du
`.spr`, repli ASCII seul — `tolower` sur du CP949 est UB). Le serveur ne calcule
JAMAIS cette clé et ne sait pas ce qu'elle désigne : lui ne voit pas les sprites.
Il range 4 variantes (`bourgeon_style$` + `2$`/`3$`/`4$`, la 1ʳᵉ = le repli,
`kFlagDefault`), les rediffuse TOUTES, et le client — seul à voir le corps monté
— choisit. Un lot ZC REDÉFINIT l'ensemble des variantes des GID qu'il mentionne.
Drapeaux : `CLEAR` (ce corps) / `CLEAR_ALL` (tout) / `DEFAULT` (le repli).
Un corps sans variante propre reprend le repli, sinon monter déshabillerait tout.
**ZC 0x0F28** = ouverture de la fenêtre par un NPC.
Un réglage = `[teinte:int16 LE][sat:int8][lum:int8][absolu:uint8]`. Drapeau bit 0 =
EFFACER. Source : `src/features/fx/style_sync.{h,cc}` ↔ moonlight
`packets_struct.hpp` / `clif.cpp` / `clif_packetdb.hpp`.

🔴 **AUCUNE migration** : seule la version courante est acceptée, tout le reste est
JETÉ. Décision de l'utilisateur (2026-08-12, « v5 c'est poubelle ») — avant la
prod, une recette périmée ne vaut pas une ligne de code de compatibilité, et un
changement de détection rend de toute façon les anciennes ininterprétables.

La **coiffure** voyage dans la même trame mais reste un ÉTAT SERVEUR : le serveur
la borne puis fait `pc_changelook(LOOK_HAIR)`, et c'est le ZC_SPRITE_CHANGE natif
qui l'annonce — pas nous. Frontière de nature, cf. section TÊTE.

* 🔴 **JAMAIS de memcpy de `ro::RampAdjust`** : elle porte un octet de bourrage, et
  le serveur n'est pas compilé par le même compilateur. Champ par champ. Deux
  `static_assert` sur `kCzBytes`/`kZcEntryBytes` transforment un `kMaxRamps` modifié
  en échec de COMPILATION — sinon la trame changerait de taille en silence et
  rAthena la rejetterait sans un mot.
* 🔴 **Le bloc de palette est PÉRISSABLE** (corrigé le 2026-08-15) : il ne vaut
  que pour le sprite sur lequel il a été calculé. Rien ne le recalculait quand le
  corps changeait — enfourcher une monture laissait la table de l'ancien corps
  s'appliquer au nouveau (couleurs délavées jusqu'à une revalidation manuelle),
  et cela chez TOUS les clients. `palette_inject` date donc chaque bloc de son
  `.spr` (lu sur l'acteur, jamais déduit) et `PollStaleSprites` signale l'écart ;
  `StyleSync::RefreshChangedBodies` remet en file. 🔴 À la FRAME, pas au tick :
  les ~100 ms de `OnTick` se voyaient comme un éclair de palette d'origine.
* **Stockage serveur** : variable de PERSONNAGE `palette_recipe$` = `"<version>:<80
  hex>"`, table `char_reg_str`. Aucune migration de schéma. La version est DANS la
  valeur : un changement de détection rend les anciennes recettes ininterprétables,
  et elles doivent être JETÉES (sinon le serveur les retamponnerait à la version
  courante et tout le monde verrait des couleurs au hasard).
* 🔴🔴 **La diffusion demande QUATRE points, pas un** (soldé et validé en jeu le
  2026-08-12, après deux pannes distinctes prises d'abord pour une seule) :

  | Mouvement | Fonction |
  |---|---|
  | J'arrive et je découvre les autres | `clif_getareachar` → `clif_getareachar_unit` |
  | Quelqu'un entre dans ma vue en MARCHANT | `clif_insight` (rejoue les DEUX sens) |
  | Les styles déjà posés, après MA vérification | `clif_bourgeon_style_area` |
  | J'APPARAIS devant ceux déjà présents | `clif_bourgeon_style_spawn` |

  **La leçon, valable au-delà du style** : `clif_getareachar_unit` ne dit à un
  joueur que ce qu'il **découvre**, jamais ce qu'il **émet**. Toute donnée
  cosmétique à nous a donc besoin du pendant dans `clif_spawn` — et le natif le
  dit déjà : `clif_refresh_clothcolor` est branchée aux DEUX endroits, en `SELF`
  dans l'un et en `AREA_WOS` dans l'autre. Lire le natif AVANT de conclure.
  ⚠ Le symptôme n'était PAS systématique — deux joueurs qui marchent l'un vers
  l'autre se voient très bien — d'où des observations contradictoires en
  apparence. Il faut trois joueurs pour le lire.
  🔴 Et surtout : ne PAS garder sur le `has_bourgeon` de l'ÉMETTEUR au spawn (son
  client n'est jamais encore vérifié à cet instant) ; filtrer par DESTINATAIRE.
* ⚠ **Déploiement CONJOINT obligatoire.** Client v2 + serveur v1 = rejet SILENCIEUX.

## 🔴🔴 Quatre pièges de synchronisation, tous payés en live (2026-08-11)

1. **Le forçage de couleur se mord la queue.** Couleur de vêtement 0 ⇒ le natif
   n'écrit AUCUNE palette externe ; on force 1 pour débloquer le rendu. À la
   reconstruction suivante, le natif relit NOTRE 1, produit `몸\body_1.pal`, et on
   le recapturait comme s'il venait du serveur. La base devenait
   `sprite ⊕ body_1.pal` sur ce client et le sprite nu sur l'autre. ⇒ drapeau
   `NativeLook::color_forced` : plus aucune recapture après un forçage.
2. **Un re-partage n'était jamais appliqué.** La garde testait « cet acteur a-t-il
   DÉJÀ une recette », vrai dès la première. Les observateurs restaient figés sur la
   toute première version. ⇒ `Remote::applied`, remis à false à chaque paquet.
3. **Personnage 100 % NOIR après suppression.** Sans chemin natif capturé (couleur
   0), `ClearRecipe` ne défaisait rien : l'acteur gardait `bourgeon\<gid>.pal` alors
   que le bloc venait de disparaître ⇒ table vide ⇒ tout noir. ⇒ écrire un chemin
   VIDE + rendre la couleur d'origine, l'état exact que le natif produit seul.
4. **« Supprimer mes couleurs » ne supprimait rien de durable.** Notre propre
   recette dormait dans le registre de propagation depuis le login ; `HasRecipe`
   retombant, la boucle la REPOSAIT au tick suivant. ⇒ `ForgetLocal()` + un
   effacement reçu est honoré MÊME l'éditeur ouvert (il n'apporte aucune couleur,
   il ne peut donc écraser aucun réglage en cours).

5. **Première ouverture : les curseurs annonçaient la bonne palette, le corps
   portait celle du sprite** (2026-08-12). `Reload()` construisait la base AVANT
   d'amorcer la recette, or `recipe_.palette_id` CHOISIT le fichier de palette sur
   lequel les rampes se détectent. Signature à l'écran : **cheveux teints, corps
   d'origine** — la tête ne dépend que d'un chemin de fichier, pas de la base. Et
   « Réglable : 97 % » au lieu de 100 % : ce ne sont pas les mêmes octets sous les
   rampes. ⇒ `SeedFromShared()` séparée, appelée AVANT `Reload()`.

**Leçon commune** : une recette ne porte AUCUNE couleur. Tout ce qui fait diverger
la BASE d'un client à l'autre fait diverger l'apparence — y compris nos propres
artefacts de rendu.

## 🔴🔴 L'éditeur ne touche PLUS au personnage (2026-08-12, choix utilisateur)

**Aperçu dans le PANTIN, action à la VALIDATION seule** (« Valider et partager
mon style »). Curseurs, préréglages, codes collés, coiffures essayées : tout cela
ne fait que recalculer 1024 octets pour le pantin. Le sprite en scène, la feuille
de perso et les autres ne voient que ce qui a été validé.

Le modèle d'avant — pousser chaque mouvement dans le rendu — paraissait plus
fidèle et a coûté cher : ouvrir la fenêtre changeait déjà l'apparence, chaque
réglage fabriquait un bloc de palette DÉFINITIF, un « annuler » laissait des
restes à moitié posés sur l'acteur, et il fallait défaire à la fermeture ce qu'on
avait posé à l'ouverture. Supprimés du même coup : le débounce, `ForgetHairStyle`,
et toute la machinerie d'injection de coiffure.

⚠ `DrawPreviewDoll` RECALCULE la palette (`ApplyRecipe` sur `base_`) au lieu de
lire `InjectedPalette` : celle-ci montrerait l'apparence VALIDÉE, c'est-à-dire
tout sauf ce que le joueur est en train de régler.

**Sélecteurs = grilles de vignettes**, pas des curseurs (`GrillePicker`, calqué
sur la création de personnage) : « coiffure 47 » ne veut rien dire. Coiffure et
couleur de cheveux réutilisent `ro::DrawHeadIcon` ; la teinte de base montre la
couleur MOYENNE des index que nos rampes couvrent, lue dans le seul `.pal`
(passer par `BuildForGid` relirait le `.spr` 553 fois), mémoïsée et sous budget
de 24 fichiers par frame.

## ✅ La TÊTE : couleur (recette) et COIFFURE (état serveur) — 2026-08-12

🔴🔴 **Frontière de NATURE, et c'est la leçon de cette passe.** Une recette est
une fiction de RENDU : le serveur la range sans la comprendre, chaque client la
retraduit. La COIFFURE, elle, est un vrai état de personnage. Les mettre dans la
même trame revenait à entretenir la même chose en double, avec deux sources de
vérité qui divergent.

**Couleur de cheveux** = un vrai fichier ⇒ rien à fabriquer : on écrit le chemin
dans l'acteur (`+0x470`, garde `+1084 > 0` comme le corps). Elle reste dans la
recette : c'est bien une fiction de rendu, le serveur n'en sait rien.

🔴 **Il faut son PROPRE détour** : `CActorSprite_RebuildHeadPalette`
(**0x00D3DED0**, prologue `55 8B EC 6A FF`, `retn` nu, 5 vtables → même adresse)
fait `std_string_assign(this + 284, Job_GetHeadPalettePath(...))`, soit
**acteur+0x470** — donc elle ÉCRASE ce qu'on vient d'y poser, sous la garde
`this[271]` = acteur+1084. Se greffer sur le détour du CORPS ne suffisait pas :
une reconstruction de tête seule ne passe pas par lui.

⚠ **La panne était ASYMÉTRIQUE, et c'est ce qui la rendait illisible** (2026-08-12,
« les autres voient le style correct, pas moi ») : après un changement de
coiffure, les AUTRES clients réappliquent la recette reçue au tick suivant —
donc APRÈS l'écrasement — tandis que le joueur, lui, avait injecté AVANT, et rien
ne repassait derrière (sa propre recette est ignorée tant que
`g_local_editing`). ⇒ devant « eux oui, moi non », chercher un ORDRE, pas une
donnée manquante.

**Coiffure** = **CZ 0x0F29 → `pc_changelook(sd, LOOK_HAIR, n)`**. Le serveur
borne sur `min/max_hair_style` (🔴 **1..80 dans `conf/import/`**, pas 42 comme le
dit `conf/battle/client.conf` — cf. [[feedback_rathena_conf_import_overrides]]),
écrit `sd->status.hair`, met à jour la fenêtre de guilde et diffuse le
**ZC_SPRITE_CHANGE natif**. Donc : sauvegardé avec le personnage, retrouvé à la
connexion suivante, vu par les clients VANILLA, et juste au char-select sans que
nous n'y touchions. Aucun ZC à nous en retour — le client apprend la valeur
RETENUE, pas celle qu'il a demandée.

### L'aperçu local, et le seul usage du natif

Pour que le joueur essaie des coupes avant de valider, on appelle
**`CActorSprite_BuildHead_Slot1` (0x00D3F4F0)** : elle range la coupe en
**acteur+1080 (0x438)** puis rebâtit l'emplacement 1. Dessous,
`Job_BuildBodyOrHeadSpritePath_impl` (0xb433b0) traverse `g_HairSpriteNum_*` —
remap des 12 premières coupes, numérotation Doram, repli hors bornes — puis race
et sexe. Appel DIRECT malgré le virtuel : les **cinq** vtables qui la portent
pointent toutes sur la même adresse.

🔴🔴 **Uniquement depuis `OnTick`** (`palette_inject::FlushHairStyles`) : la
fonction native fait `UITexture_Release`, et le faire en frame ImGui lève
0xC0000374 — cf. [[feedback_imgui_pitfalls]].

⚠ Deux sorties d'aperçu, à ne pas confondre : `ClearHairStyle` REND la coupe
d'avant (fermeture sans valider) ; `ForgetHairStyle` lâche sans restaurer (le
serveur vient de prendre la valeur à son compte). Se tromper rendrait au joueur
la coiffure qu'il vient justement de remplacer pour de bon.

## ✅ Réparation AUTOMATIQUE des corps noirs (2026-08-11)

Le dossier des 4e classes est enfin résolu **pour tous**, pas seulement pour qui
ouvre l'éditeur. `PaletteSync::AutoRepair` (un acteur examiné par tick) impose la
base fusionnée aux corps que les palettes de vêtement abîment — recette NEUTRE,
donc aucune couleur choisie n'est changée.

🔴 **Le critère est mesuré, pas seuillé au doigt** : on ne dévie du natif que si
la fusion récupère ≥ 2 % des pixels peints (`pixels_black_native −
pixels_black_merged`). Un seuil sur la seule noirceur serait faux — beaucoup de
sprites ont des contours noirs voulus. Jamais pour un porteur de recette (ses
couleurs priment), ni pour un GID dont une recette attend d'être appliquée.

Distribution réelle (421 corps × 3 palettes) : avec `body_0`/`body_56`, **environ
la moitié des corps restent strictement natifs** ; avec `body_255`, 385 sur 421
sont réparés. Pires cas récupérés : `hyper_novice_riding`, `troubadour`,
`imperial_guard_chicken` — jusqu'à **84 % des pixels**.

L'énumération d'acteurs vient de `palette_inject::KnownGids` : le détour de
reconstruction voit tout ce qui apparaît, c'est gratuit et il n'y a pas à
traverser les conteneurs natifs.

## ✅ Char-select (2026-08-11)

Deux moitiés. (a) `DollLook::body_palette` + `body_palette_key` : le composeur
accepte 1024 o RGBA au lieu d'un chemin — 🔴 la clé doit hacher le CONTENU, le
cache de teintes partageant ses textures entre appels de même clé. (b)
`fx::palette_cache` → `SaveData\bourgeon_palettes.yaml`, une entrée par
personnage, même encodage `"<version>:<hex>"` que le serveur (donc invalidé
ensemble). Clé = `g_Own_CharId` **0x015FB9A8** en jeu, `CHARACTER_INFO+0x00` au
char-select : le même identifiant, c'est ce qui rend le pont possible.

🔴 Piège payé : le char-select mémoïse la palette composée (analyser un `.spr` par
pantin et par frame est impensable) et sa signature avait OUBLIÉ la recette — un
« pas de couleurs » calculé au premier passage n'était plus jamais revérifié, et
les couleurs n'apparaissaient qu'après avoir relancé le jeu. D'où
`palette_cache::Generation()`, à mettre dans toute signature de cache dérivé.

⚠ Ici le sprite est forcément DÉDUIT (pas d'acteur) : sur un corps où la
déduction diverge, l'aperçu diffère du rendu en jeu.

## Réglages mesurés, pas devinés

`kMinLength` est passé de 3 à **1** (2026-08-11). « Deux teintes ne font pas un
dégradé » était une intuition fausse : beaucoup de palettes sont ÉPARSES et peignent
des aplats de 1-2 couleurs, qui couvrent de vraies pièces du costume.

| seuil | couverture moy. | pire cas | corps < 70 % |
|---|---|---|---|
| 3 | 85,7 % | 48,9 % | **36** |
| 2 | 87,7 % | 57,2 % | 14 |
| **1** | **91,0 %** | 57,2 % | **9** |

Sans contrepartie : la couverture est calculée sur les 8 pièces RETENUES, donc
accepter les aplats ne chasse aucune vraie pièce du classement. Le vrai filtre
anti-bruit est `kMinPixels`. Cas réel (`shadow_cross_남`) : 79,7 % → **96,7 %**.

## ✅ État au 2026-08-12 — SOLDÉ, éprouvé en jeu des deux côtés du fil

`ui/palette_ramps` (v6, validé croisé byte-exact sur 421 corps) ·
`fx/palette_inject` (blocs JAMAIS libérés) · `fx/palette_base` (base commune
éditeur + réseau, **cache par couple sprite|palette**) · `fx/style_sync`
(protocole) · `fx/palette_cache` (char-select + brouillon) ·
`windows/palette_editor` (Alt+P) · serveur moonlight (stockage + les quatre
points de diffusion) · char-select · propagation entre joueurs.

🔴 L'éditeur LIT le sprite sur l'acteur (`ActorSlotSpritePath`, emplacement 0 =
corps) au lieu de le déduire de (classe, sexe, monture) : la déduction rejoue une
résolution native pleine de cas particuliers, et quand elle diverge les rampes
restent valides mais désignent les MAUVAIS index.

⚠ **Deux latences distinctes, deux causes sans rapport** — les confondre a coûté
du temps :
* *les autres apparaissent 1 s en couleurs d'origine* = NOTRE boucle
  d'application, qui budgétait des APPELS au lieu du COÛT réel ⇒ cache de bases +
  `Body::cached` pour ne compter que ce qui construit vraiment.
* *la palette n'arrive JAMAIS* = le serveur ne l'avait pas envoyée ⇒ les quatre
  points de diffusion ci-dessus.

Reste ouvert, **hors périmètre du style** : ce que devient le pointeur d'acteur
mémorisé après un changement de carte.

## 🔴🔴 TROIS consommateurs, pas deux : le SITE en est un (2026-08-22)

`tools/palette_ramps.py` n'est pas seulement la référence de validation croisée
du C++ : **moonlightsite l'EMBARQUE** (`ressources/bourgeon/palette_ramps.py`,
appelé en `--palette <spr> <pal> <recette>` par `bourgeon_body_palette`) pour
peindre les vignettes de personnages. Toute évolution du protocole doit donc être
répercutée **à trois endroits** — client C++, `tools/palette_ramps.py`, et la
copie vendue au site (identique octet pour octet).

**La panne du 2026-08-22** : la v7 (2026-08-15) a inséré la clé de corps dans la
forme stockée, or l'outil et le site étaient restés en v6. Le site refusait donc
**toutes** les recettes et rendait les couleurs d'origine — panne silencieuse
côté joueur, une ligne dans le journal d'Apache côté exploitant. Rien n'avait
bougé dans l'algorithme de rampes lui-même (`palette_ramps.cc` inchangé depuis la
v6) : seule la FORME avait changé.

**La leçon** : quand une version de protocole bouge, ce n'est pas « le client et
le serveur » qu'il faut lister, c'est **tout ce qui LIT la valeur stockée**.

### Ce que le site sait faire depuis

* **Il choisit la bonne variante.** Il calcule `ro::BodySpriteKey` en PHP —
  FNV-1a sur `data\sprite\<race>\몸통\<sexe>\<nom>_<sexe>` **en CP949**, ASCII
  seul en minuscules — puis applique la règle du jeu : correspondance exacte,
  sinon le repli. 🔴 Ce calcul n'est possible QUE parce que le site nomme ses
  sprites **comme le client** (coréen pour les classes anciennes, latin pour les
  4ᵉ). Vérifié contre les vraies clés du cache local du joueur
  (`SaveData/bourgeon_palettes.yaml`) : 5/5, dont un nom coréen.
* Sur le moindre doute (PHP 32 bits, mbstring absent, conversion CP949 qui ne
  revient pas à l'identique) il rend **0** = « corps inconnu » ⇒ repli. Il dégrade,
  il ne ment pas.
* L'empreinte de cache (`charsheet_hash`, **v4**) couvre les QUATRE variantes :
  restyler le corps qu'on porte peut n'écrire que l'emplacement 3, et l'empreinte
  ne verrait rien changer pendant sept jours.

⚠ Le site ne dessine jamais les montures : un personnage stylé À CHEVAL verra sa
vignette prendre le repli, pas sa recette de monture. C'est voulu.

## 🔴🔴 Un pantin d'AUTRUI lit `InjectedPalette`, il ne RECALCULE jamais (2026-08-29)

Deux vues montrent un AUTRE joueur : la fiche « Voir l'équipement »
(`windows/view_equip_window.cc`) et le portrait du cadre de cible
(`overlays/target_frame.cc`). Les deux étaient fausses, chacune à sa façon :

* view_equip **recalculait** (`RemoteRecipe` + `palette_base` + `ApplyRecipe`) ;
* target_frame ne posait **aucune** palette.

La source est `fx::palette_inject::InjectedPalette(gid, …)` — les 1024 octets que
le rendu applique au personnage en scène. `palette_inject.h` le prescrit noir sur
blanc (« feuille de perso, portrait, aperçu d'équipement, enregistreur GIF ») et
`FillOwnDollPalette` (basic_info) le faisait déjà pour NOUS. Recalculer ouvre un
chemin PARALLÈLE — il faut y redeviner le `.pal` de vêtement que le natif avait
choisi pour cet acteur — et deux chemins finissent par ne plus dire la même chose.

🔴 **Le témoin qui rend le défaut visible** : le corps de 4e classe en SILHOUETTE
NOIRE. Le `.pal` de serveur seul laisse la moitié de ses index vides ; ce que
l'écran montre de correct vient de la **réparation automatique** de `StyleSync`,
une recette NEUTRE posée sur l'acteur. Elle n'est dans aucune recette de joueur et
dans aucun fichier ⇒ **aucune reconstruction ne peut la retrouver**. Un pantin qui
recalcule noircit donc des joueurs qui n'ont même pas de style.

⚠ La couleur de CHEVEUX n'est pas dans ce bloc (il ne teint que le corps) et
`SetHairPalette` n'a **pas de lecteur** ⇒ elle se prend dans
`style_sync::RemoteRecipe(gid, BodySpriteKey(corps), …)`. Le champ de l'acteur,
lui, porte la couleur du SERVEUR.

⚠ L'exception reste `DrawPreviewDoll` de l'éditeur : lui doit montrer ce que le
joueur RÈGLE, pas ce qui est validé (voir plus haut). Le recalcul y est juste.
