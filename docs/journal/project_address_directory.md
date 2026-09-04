# Annuaire des constantes natives — les vingt tranches du chantier de déduplication

> Journal du chantier. La fiche de mémoire `project_address_directory` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Chantier mené le 2026-07-28, en préparation du TODO « portage sur une autre
version d'exe ». Mesure de départ : **1 286 occurrences de 672 adresses natives
distinctes** en dur dans `src/features` + `src/ui`, les pires redéclarées dans
15 fichiers sous jusqu'à **onze orthographes**. Après six tranches : ~600 / ~470,
**309 déclarations supprimées**.

**Les en-têtes** (tous MINUSCULES, `<cstdint>` seul — modèle `uiwnd.h`) :
- `ragnarok/uiwnd.h` — MakeWindow/CloseWindow/OnMsg/SetPos, `Vf<>`, kOffVisible/PosX/PosY
- `ragnarok/globals.h` — `rag::` session, zeny, gestionnaire de modes, mode actif
- `ragnarok/item_db.h` — `itemdb::` DB de descriptions, ItemSkillInfo, noms, ids de fenêtres
- `ragnarok/render.h` — `render::` contexte de rendu, atlas de sprites, Act_GetFrame
- `ragnarok/lua.h` — `lua::` API C Lua + enveloppes typées
- `ui/game_texture.h` — `ro::texmgr::` (préexistant, complété)

⚠ NE PAS faire un `addresses.h` fourre-tout : un en-tête inclus partout dont
chaque retouche recompile tout le projet troque une dette contre une autre.

🔴 **LE TRIPTYQUE DE VÉRIFICATION TIENT LIEU DE COMPILATEUR** — validé sur 292
déclarations supprimées dans 60 fichiers, **build vert du premier coup**. Comme
c'est l'utilisateur qui compile, écrire ces trois scripts AVANT de substituer :
1. tout `ns::kNom` employé atteint-il son en-tête **transitivement** (suivre les
   `#include` en chaîne) ? ⚠ `rag::` est PARTAGÉ par plusieurs en-têtes
   (rag::social, rag::pet, rag::homun) : n'exiger l'include que pour les noms
   que CET en-tête déclare, sinon 25 faux positifs.
2. chaque nom employé existe-t-il vraiment dans l'en-tête ?
3. tout nom dont la déclaration a été supprimée a-t-il disparu du fichier ?
   (⚠ faux positifs attendus quand on REDÉCLARE le nom autrement, p. ex.
   `kGuildLen = rag::kGuildObjAddr + 0x10` : les vérifier un par un.)
Plus deux balayages : aucun `(::|.|->)ns::` (qualificateur traversé) et aucune
substitution tombée DANS une chaîne littérale.

**MÉTHODE, à rejouer telle quelle** (chaque tranche = 1 commit + 1 build) :
1. Relever par **NOM ET VALEUR**, jamais par valeur seule — 0xc, 0x18, 0x104,
   0x114 nomment tout autre chose ailleurs (`kBtnDropLock`, `kTabCtrl`…).
2. Supprimer TOUTES les lignes de déclaration **AVANT** de substituer les noms.
   L'ordre inverse réécrit un nom À L'INTÉRIEUR d'une déclaration encore
   présente → `constexpr uintptr_t ro::texmgr::kGet = …` (C2374).
3. Réécrire seulement la **TÊTE** d'un appel, jamais la liste d'arguments ;
   pour les cas résistants, découper à **parenthèses équilibrées** plutôt que
   d'empiler des cas particuliers dans une regex.
4. Contrôle final **insensible à la casse** : `0x00A39340`, `0x012515F8`,
   `0x006A2B50` s'écrivent en majuscules dans certains fichiers.
5. Attention aux constantes déclarées `int`/`uint32_t` au lieu de `uintptr_t`
   (`kModeMgrKey`, `kRenderContextPtrVa`) : un relevé par type les rate.
6. Les adresses écrites **en littéral au milieu d'une expression** sont le vrai
   gisement invisible : `*(void**)0x0121333c`, `{0x015fba90, …}`.

**CINQ CONTRADICTIONS DU REGISTRE RE exhumées** — invisibles tant que chaque
fichier gardait sa copie :
- `skill_bar` avait `kWinItemDesc`/`kWinSkillDesc` et leurs messages **INTERVERTIS**
  (corrigé `f6a58b3`). Appariement vrai : OBJET = fenêtre 0x0c + msg 0x18 +
  `&ItemSkillInfo` ; SKILL = 0x2e + 0x3d + **id BRUT**, id affiché à +0x104.
- `0x00a2e770` : six fichiers la disent `UIWindowMgr::Close`, deux
  `SaveWindowRect`. TRANCHÉ 31/07 (IDB `UIWindowMgr_SaveRectAndCloseWindow`) :
  sauve le rect PUIS `QueueDestroyWindow` 0x00a447d0 → la fenêtre est DÉTRUITE.
  `CloseWindow` est le bon nom côté Bourgeon.
- `0x01213338` portait TROIS noms (`kModeMgr`, `kModeArg`, `kDragMgr`) ;
  `kDragMgr` laissait croire à un gestionnaire de drag qui n'existe pas.
- `0x012515f8` en portait **QUATRE** (`kSceneCtxPtr`, `kViewportPtr`,
  `kRendererObjPtr`, `kRenderContextPtrVa`) : un seul objet, quatre facettes.
- Lua : `lua::Manager()` (= `*0x015ffd78`) et `lua::State()` (= `*Manager()`)
  ne sont PAS redondants. `kExecFileAddr` est `__thiscall` sur le MANAGER ;
  l'API C veut le STATE. Les confondre crashe. Cf. [[reference_lua_c_api]].

**Reliquat SOLDÉ le 2026-07-31** (7e tranche) : inventaire
(`0x0131f6bc`/`0x0103d460`, 5 fichiers) et storage (`0x0131f770`/`0x0103ca40`,
3 fichiers) → `uiwnd::kInventoryWndSlot`/`kStorageWndSlot` + vtables ;
allocateur CRT (`0x00dbbc4f` malloc, `0x00dbbc7f` free) et dtor std::string
(`0x004f08f0`, 4 fichiers / 3 noms) → `rag::kGameMallocAddr`/`kGameFreeAddr`/
`kStdStringDtorAddr` (globals.h, section « Allocateur CRT du client »).

**SIXIÈME CONTRADICTION exhumée puis TRANCHÉE (31/07, au désassemblage)** :
`0x004e78c0` (basic_info, « second » dtor std::string) est un simple THUNK vers
`0x004f08f0` — même fonction, deux portes d'entrée. La déclaration de
basic_info était de toute façon SANS USAGE : supprimée.

**SEPTIÈME CONTRADICTION (02/08) — un NOM de l'IDB, pas une adresse dupliquée** :
toute la famille `Robe_*` désignait le **BOUCLIER**. Preuve : les gabarits
qu'elle compose sont en `방패\…` (bouclier) et non `로브\…` (robe), et son seul
appelant construit le **slot 7** = le bouclier. Renommées `Shield_*` ; la table
`0x015FF664` est celle des noms de bouclier, PAS de cape. De même
`Job_GetGarmentPalettePath` → `Job_GetGarmentActPath` (elle rend un `.act` ; la
cape n'a **pas** de palette). Détail complet dans [[project_doll_composer]].
🔴 Leçon : un nom d'IDB hérité vaut une adresse dupliquée — le vérifier sur les
DONNÉES qu'il manipule (ici les gabarits CP949), pas sur sa vraisemblance.

**Verdicts de désassemblage du 31/07** (commentés dans l'IDB + les en-têtes) :
- `0x00dbbc4f`/`0x00dbbc7f` = **operator new / operator delete** du CRT
  statique (pas malloc/free) ; new ne rend JAMAIS nullptr (boucle _callnewh).
  → `rag::kGameOperatorNewAddr` / `kGameOperatorDeleteAddr`.
- `0x004f08f0` = _Tidy de std::string (libère hors-SSO, remet size=0/cap=15)
  → `rag::kStdStringDtorAddr`.
- `0x00a75340` GameMode_GetActive = `*(mgr+0x58)==1 ? *(mgr+4) : 0` :
  hypothèse « = lecture directe de 0x0121333c » RÉFUTÉE — la garde +0x58 masque
  le mode pendant les transitions. NE PAS substituer l'une à l'autre.
- `0x005aad80` UIWindow_SetVisible écrit +0x28 en DWORD ENTIER : l'accès `int`
  d'uiwnd.h est confirmé, le doute octet-vs-int est clos.

**HUITIÈME TRANCHE — 2026-08-23, COMPILÉE VERTE** (commits `87cd130` +
`1b91e60`). Mesure faite EN CODE SEUL (les commentaires
citent l'adresse comme PREUVE de RE : les y substituer détruit la trace — le
relevé doit donc ignorer tout ce qui suit `//`). Départ 79 adresses partagées par
≥2 fichiers / 214 occurrences → arrivée **51 / 107**, **292 déclarations
supprimées dans 60 fichiers**. Le reste est à 2 fichiers : mutualiser n'y paie
plus.

**QUATRE EN-TÊTES DE PLUS** (même moule : `<cstdint>` seul) :
- `ragnarok/game_scene.h` — namespace **`gamescene`** (PAS `scene` : quatre
  fichiers ont déjà une variable locale `scene`, qui masquerait le namespace).
  CGameMode→actorMgr (0xcc), →dict de noms (0x160), champs CNameInfo, les DEUX
  FindByGid, PostActorClickAction.
- `ragnarok/audio.h` (`audio::`) — g_SoundMgr + Sound_Play3D.
- `ragnarok/file_mgr.h` (`filemgr::`) — FileMgr + LoadToMemory + FreeBuffer.
- `ragnarok/navigation.h` (`navi::`) — g_Navigation + SearchRoute.

🔴 **LE GISEMENT LE PLUS GROS ÉTAIT INVISIBLE : ce sont des OFFSETS.** `0xcc`
(CGameMode→actorMgr) était redéclaré dans **ONZE fichiers sous CINQ noms**,
`0x160` dans six sous trois — et aucun relevé d'ADRESSES ne les voit passer.
⚠ Les relever par VALEUR est interdit : `0xcc` nomme aussi un vecteur d'onglets,
un champ de saisie d'échoppe et un octet de quête.

**CONTRADICTIONS TRANCHÉES CE JOUR** (au désassembleur / décompilateur) :
- `0x00d5a720` **BuildItemIconGrfPath** portait QUATRE signatures. Vérité :
  `void __stdcall(const char* id_str, char* out, int identified)` — `retn 0Ch`,
  et `arg_8` EST lu (`[eax+8]` si non nul, `[eax+0x1C]` sinon). 🔴 **BUG
  CORRIGÉ** : skill_bar la déclarait à DEUX arguments — elle lisait donc son
  drapeau dans de la pile NON INITIALISÉE. Le SEH masquait la moitié du symptôme
  en perdant simplement l'icône. Catalogue : `ro::texmgr::kBuildItemIconPath`.
  ✅ TRANCHÉ (2026-08-24, par l'utilisateur) : chat.cc et roggle passaient `0`,
  donc le nom de ressource NON identifié — alignés sur **1**. 🔴 La règle, valable
  partout : **quand l'état d'identification est inconnu, passer 1.** Un lien de
  chat ne le transporte PAS (ni le jeton `^i[id]`, ni `ChatLink` qui n'a pas le
  champ), et **sur Moonlight tout objet est identifié**, sauf création délibérée
  par `item2` / `item3` / `@item2` — l'identification est de toute façon une
  propriété de l'INSTANCE (`ItemSkillInfo+0x5c`), pas de l'id. Ne garder le
  drapeau variable que là où l'instance est sous la main (`ro::ItemIcon(id,
  it.identified)` : inventory_viewer, cart_viewer, storage, rodex, toast).
- `0x00d7fa90` n'est NI `kGetInvItemAddr` NI `kSkillEntryFill` : c'est un
  **aiguilleur par PLAGE d'id** (`itemdb::kFillInfoByIdAddr`) — 8000-8060,
  8200-8241 ∪ 8400-8457, 10000-10019, sinon la liste des compétences APPRISES.
  C'est cette dernière branche qui explique l'icône perdue d'une compétence
  d'une AUTRE classe.
- `0x015fa3c0` portait **CINQ** métiers apparents (kUIWindowContextKey,
  kOptionContextAddr, kUiCtx, kJobNameCtx, kSessionAddr) — un seul objet.
- `0x015fb9a4` (AID) portait **SIX** orthographes.
- `0x00d5bb40` : un seul getter qui rend TANTÔT un libellé TANTÔT un nom de
  FICHIER — d'où kJobDisplayName/kJobNameAddr/kJobResName.
- `0x01600553` : `kFavFlag` et `kDealLockGlobal` sont le MÊME octet — le mode
  Favoris ajoute un onglet ET exclut les favoris de la liste vendable.
- `0x015fba54` : `kAspdRaw` ment, c'est l'**amotion** (ms). `rag::AspdFromAmotion`
  factorise la formule `(2000-x)/10`, qui était recopiée dans cinq fichiers.
- Séquelle réparée : `rag::rag::kGameOperatorDeleteAddrAddr` (double
  substitution d'une passe antérieure, 3 sites, en commentaire).

**CONSTANTE OU ACCESSEUR ?** Ne pas se contenter de PUBLIER des accesseurs : les
EMPLOYER, sinon l'en-tête ship de l'API morte (23 sites convertis après remarque
de l'utilisateur, `if (aid == rag::OwnAccountId())`). La règle de partage :
- un `__try` **ENGLOBANT** couvre aussi un accesseur inline → la garde SURVIT,
  on peut convertir ;
- un **lecteur-aide qui porte lui-même le `__try`** (`ReadInt`, `ReadGlobalU32`,
  `ReadIntSEH`) → convertir lui ferait perdre sa garde : ces sites gardent la
  CONSTANTE. C'est pourquoi `kStatRaiseCostAddr` n'a **pas** d'accesseur.
⚠ Vérifier aussi le TYPE : `OwnAccountId()` rend `uint32_t` ; un site qui
comparait en `int` (ez_effect_capture) est laissé tel quel plutôt que d'ouvrir un
avertissement signé/non signé.

🔴 **PIÈGE D'OUTILLAGE PAYÉ DEUX FOIS** : une substitution par mot ne doit
JAMAIS traverser un qualificateur. `kStatBonus` désignait à la fois une globale
de stats ET l'opcode `bopcodes::kStatBonus` → la regex a fabriqué
`bopcodes::rag::kStatBonusAddr`. Motif correct : `(?<![:\w])NOM\b`.

---

## NEUVIÈME TRANCHE — 2026-08-24 (commit `f93b5dd`, 79 fichiers, +740/−576)

🔴🔴 **« Il ne reste plus rien » VOULAIT DIRE « je ne mesure plus la bonne
chose ».** La 8e tranche concluait « le reste est à 2 fichiers, mutualiser n'y
paie plus ». C'était vrai des ADRESSES et faux de tout le reste. Résultat :
**27 adresses partagées → 3, et ces trois-là sont des COULEURS** (0xFFFFFF,
0xC0C0C0, 0xFFAA00) que la fourchette d'adresses attrapait à tort.

🔴 **L'ANGLE MORT ÉTAIT DANS LA MESURE.** Le relevé « partagée par ≥2 fichiers »
EXCLUAIT le dossier du catalogue. Une adresse **déjà cataloguée et redéclarée
dans UN SEUL fichier** n'apparaissait donc nulle part — alors que c'est
exactement la dette cherchée. Un relevé CROISÉ catalogue↔copies en a exhumé
**17**. ⇒ Toujours croiser les deux ensembles, jamais mesurer un seul côté.

🔴🔴 **LE VRAI GISEMENT N'ÉTAIT PAS LES ADRESSES, C'ÉTAIT LES SIGNATURES.** Une
convention d'appel fausse dans UNE copie ne se voit qu'à l'exécution, sur un seul
écran (bug `BuildItemIconGrfPath` de la 8e). Quatre familles : `TexMgr` (13
fichiers → `ro::texmgr::LoadResource`) · getter « mode actif » (9 fichiers, 4
noms, 29 sites → `rag::ActiveModeIfReady`) · `CMode::SendMsg`, slot écrit de
**CINQ façons dans 16 fichiers** (0x18 sous trois noms, l'index 6, deux littéraux
nus) → `rag::ModeSendMsg` (+ `…Ptr`) · les 2 getters Lua de nom de compétence.
⇒ **Relever aussi les `using …_t = …(__…*)(…)`, pas seulement les adresses.**

🔴 **Le littéral CP949 `유저인터페이스` était recopié dans 11 fichiers sous 6 noms**
(`kUIDir`, `kUiRoot`, `kUiDirCp949`, 3 anonymes en pleine expression) →
`ro::uipath::kUiRoot`, vérifié octet à octet contre `0x010253b4`. Un octet faux
donnait un chemin inexistant dans tout GRF, visible seulement en image manquante.

**DEUX NOMS QUI MENTAIENT, TRANCHÉS AU DÉSASSEMBLEUR :**
- 🔴🔴 `0x00a9f030` **n'est PAS un « MakeKey »** : elle rend un `const char*` et
  fait la **RÉSOLUTION DE SKIN** (racine d'interface → dossier du skin actif,
  repli « UI\ », réécriture gardée seulement si la ressource EXISTE ; sinon elle
  rend le pointeur d'ENTRÉE tel quel — d'où le fait qu'on puisse la sauter).
  ⚠ **NON RÉENTRANTE** : le chemin réécrit pointe dans l'un de DEUX `std::string`
  globaux (0x0159d654 / 0x0159d670). Devenue `kResolveSkinPath`.
- 🔴🔴 `0x00a8d4a0` « UITextureMgr_Load » est un gestionnaire de **RESSOURCES** :
  il aiguille **PAR EXTENSION** vers un chargeur par type. C'est pourquoi la
  parade lui demande un `.wav` et l'aperçu de couvre-chef un `.str` — usage
  PRÉVU, pas détournement. Chemin **tronqué à 260**, passé dans un tolower ASCII
  (0x012154c8 — `/` **n'est PAS** converti en `\`), résultat **mémorisé sans
  AddRef** (d'où le `kAddRefAddr` voisin, obligatoire pour garder une ressource).
- `0x012135f0` s'appelait « kNameplateQuadTree » dans un fichier : c'est le
  quadtree de **PICKING** ; les étiquettes n'en sont qu'un consommateur.

**AUCUN NOUVEL EN-TÊTE** — les 10 existants ont suffi, étoffés (`game_scene`,
`item_db`, `uiwnd`, `globals`, `camera`, `render`, `social`, `pet`, `own_actor`,
`user_hotkey`, `game_settings`).

🔴 **RIEN N'EST OFFERT SANS ÊTRE EMPLOYÉ** (la leçon de la 8e, appliquée) :
vérification entrée par entrée en fin de tranche. **Trois étaient mortes**
(`rag::BattleModeOn`, les deux bases d'inventaire) → appelants routés ; une
quatrième, purement spéculative (`kResourceExists`), **retirée**.
⚠ Un accesseur employé UNIQUEMENT dans son propre en-tête (`texmgr::Mgr`,
`kVfModeSendMsg`) compte comme vivant — le compter par occurrence QUALIFIÉE le
déclare mort à tort.

**SIX VÉRIFICATEURS** (scripts dans le scratchpad de la session) : atteignabilité
transitive · existence du nom dans l'en-tête · aucun nom supprimé qui subsiste ·
aucun qualificateur traversé · aucune substitution dans une chaîne · aucun
namespace redoublé. ⚠ Faux positifs ATTENDUS : un même mot dans un autre
namespace (`ci::kHair` = offset 0x56 de CHARACTER_INFO, sans rapport avec
`rag::kOwnHairStyleAddr`), et les signatures multi-lignes que l'analyseur ne lit
pas.

🔴🔴 **LE TROU DE CE TRIPTYQUE : LA COLLISION DE NOMS.** Build en échec sur une
seule erreur (**C2084**, `uiwnd::SetVisible` a déjà un corps) : j'avais ajouté une
enveloppe dont le nom existait **quinze lignes plus bas dans le même en-tête**.
Aucun des six vérificateurs ne pouvait le voir, et pire, le contrôle « aucune
entrée morte » l'a déclarée VIVANTE — il comptait les occurrences du NOM, et
c'étaient celles de l'homonyme préexistant.
⇒ **Avant d'ajouter une fonction à un en-tête du catalogue, grepper CE nom DANS
CET en-tête.** Un compte d'occurrences ne prouve jamais qu'une entrée est
employée quand un homonyme existe.
⇒ Corollaire de RE : les deux `SetVisible` écrivent le MÊME champ (+0x28). La
native (0x009030c0) y ajoute la NOTIFICATION de la fenêtre (`OnMsg` 23 via
vtable+0x94, puis vtable+0x98) — c'est cette notification que la barre de
raccourcis intercepte en la détournant. Corrigé en `c438209`.

### ÉTAT MESURÉ AU 2026-08-24 (après `ede3d8f`) — le chantier est TERMINÉ

- **166 adresses au catalogue**, sur 15 en-têtes (`globals` 77, `uiwnd` 18,
  `item_db` 14, `lua` 13, `ui/game_texture` 10, `game_scene` 9, `render` 5…).
- **569 adresses encore en dur, 597 occurrences — mais ZÉRO partagée entre deux
  fichiers, et ZÉRO qui double une entrée du catalogue.** Le reliquat est
  entièrement à UN SEUL lecteur. ⇒ **Ne pas le mutualiser** : éloigner une
  constante du commentaire qui l'explique ne rapporte rien. Les gros porteurs
  pilotent une fenêtre native entière (`status_tweaks` 45, `game_settings.cc` 45,
  `character_sheet` 43, `inventory_tweaks` 31) et n'en partagent la carte.

🔴 **DEUX RELIQUATS TROUVÉS PAR L'ÉTAT DES LIEUX, invisibles au relevé par NOM :**
- un **littéral nu dans un tableau** (`0x015E5B40` au milieu de trois adresses,
  pick_quad_tweaks) — d'où l'obligation de relever aussi les LITTÉRAUX, pas
  seulement les `constexpr … = 0x…` ;
- 🔴🔴 **promouvoir une constante d'un `.cc` vers son `.h` laisse une duplication
  si l'on ne pense qu'aux AUTRES fichiers** : `pet.cc` gardait son `kAid` alors
  que la même adresse venait d'être publiée dans `pet.h` pour un consommateur
  externe. La passe avait nettoyé le consommateur et oublié la SOURCE.

🔴 **`sed -i` a PERDU des lignes insérées, deux fois** (les commentaires ajoutés
à `chat.cc` et `roggle.cc` ont disparu après coup, alors que la substitution
d'un caractère sur la même ligne, elle, a tenu). Pour la chirurgie de source :
**Edit, ou un script Python temp + `os.replace`** — pas `sed -i`.

**Ce qui reste** : table des messages, puis les adresses à 1-2 fichiers où
mutualiser n'apporte rien.

---

## DIXIÈME TRANCHE — 2026-08-24 (commit `c01d7fe`, 91 fichiers, +1258/−1869)

🔴🔴 **CE N'ÉTAIT PLUS LES ADRESSES, C'ÉTAIENT LES FONCTIONS.** Le catalogue
disait « zéro adresse partagée », et c'était vrai. Un comparateur des **4 022
fonctions** du dépôt (corps normalisé) : **39 groupes identiques, 62 exemplaires
en trop, 51 fichiers** → 5 groupes (−92 %), puis 0 à la 11ᵉ tranche.

🔴🔴 **QUATRE NIVEAUX D'AVEUGLEMENT, chacun découvert en corrigeant le précédent :**
1. Le relevé d'ADRESSES ne voit pas les fonctions recopiées.
2. Un comparateur LITTÉRAL ne voit pas une copie RENOMMÉE (`BIPatchPtr` était la
   5ᵉ `PatchValue` ; seuls `old`/`val` différaient). Remède : neutraliser les
   identifiants **déclarés dans la fonction** en jetons positionnels. +4 groupes.
3. 🔴 **Un comparateur de FONCTIONS ne voit pas une duplication d'EXPRESSION.**
   Le décodage de la `std::string` MSVC était écrit **34 fois**, dont **22 en
   ternaires au milieu d'une fonction plus grande** — et sous **trois
   orthographes du même test** (`cap > 15`, `>= 16`, `> 0xf`).
4. 🔴🔴 **Le SEUIL de similarité cache la copie qui a le plus DÉRIVÉ.** Trois
   familles persistaient le même bloc yaml ; le relevé n'en appariait que DEUX
   (la 3ᵉ portait 2 champs de plus). ⇒ ouvrir la paire signalée et regarder
   AUTOUR, jamais traiter la liste ligne à ligne.

**SIX FOYERS** (mêmes règles : minuscules, `<excpt.h>` et jamais `<Windows.h>`) :
`utils/memory_patch.h` (`mem::WriteCode`/`PatchValue`, 10 copies, 4 noms) ·
`ragnarok/client_string.h` (`rag::clientstr::Data/Size/Copy/CopyTruncating`) ·
`ragnarok/job_ids.h` · `features/windows/viewer_probes.h` (`viewers::`, 7 sondes)
· `ui/item_grid_chrome.h` (`ro::grid::`) · `utils/text.h`.
Et dans l'existant : `rag::ActiveModeSafe`, `OwnAccountIdSafe`, `ReadInt`,
`Read<T>` ; `gamescene::FindActorByGid`, `ActorGuildId` ;
`ro::uipath::WithFileName` ; `TexId` (9 copies) ; `ro::RgbToF3`/`F3ToRgb` ;
`audio::Play3D` ; `paths::GameDirW` ; `rag::OwnActor` ; `links::RecipeLinkLabel`.

**TROIS BUGS, tous nés d'une copie qui avait dérivé :**
- `storage_window::CartOpen()` cherchait la fenêtre NATIVE du chariot, détruite
  en interface moderne ⇒ l'entrepôt ne proposait **jamais** « Vers le cart ».
- `party_friend_window::JobNameSEH` passait `-1` (sexe du JOUEUR LOCAL) là où
  `social.cc` passe `99` ⇒ libellé faux pour tout membre du sexe opposé.
- `menu_icons::PatchValue` avait perdu son `FlushInstructionCache`.

🔴 **UN NOM QUI MENT COÛTE PLUS CHER QU'UNE COPIE.** Deux divergences RENDUES
VISIBLES au lieu d'être corrigées (intention indécidable) :
`entity_inspector::ActiveGameMode` lisait le pointeur BRUT quand huit homonymes
passaient par le getter gaté → renommé **`ActiveModeRaw()`**, question posée en
commentaire. Idem pour les `ModeSendMsg` de la chatbox.

🔴 **`kInfoIdCap = 0x40` redéclaré dans ONZE fichiers**, chacun refaisant
`0x2c + 0x14` à la main. `clientstr::Data` les a tous rendus inutiles.

## ONZIÈME TRANCHE — 2026-08-24 : les 5 restants soldés, **0 groupe**

Le relevé sort désormais **0 groupe** sur les deux passes (3930 fonctions).
Foyers ajoutés : `ui/viewer_rect.h`, `ui/desc_pending_lock.*`,
`utils/game_focus.*`, `features/windows/item_viewer_base.h`, plus
`rag::ActiveModeSendMsgSafe` / `rag::RawModeSendMsgSafe` dans `globals.h`.

🔴🔴 **UN RELEVÉ PAR CORPS DE FONCTION EST AVEUGLE AUX EXPRESSIONS NUES.** Le
test d'appartenance au rect d'un viewer était écrit SIX fois, pas trois : trois
`PointOverViewer` (vus) **et trois `over_self` en expression inline** dans les
`OnRenderUI` (invisibles — ce ne sont pas des fonctions). ⇒ Après avoir soldé un
doublon, **grepper les MEMBRES qu'il touchait**, pas seulement son nom.

🔴 **CINQ COPIES, TROIS NOMS, DEUX SÉMANTIQUES QUE RIEN NE DISAIT.** La famille
`ModeCmd`/`SendModeCmd` : rodex+trade+game_menu passaient par le getter GATÉ,
weapon_refine+make_item lisaient le pointeur BRUT (en deux orthographes). Fondre
les cinq aurait changé le comportement pendant un changement de carte ⇒ **deux**
fonctions dont le NOM porte la distinction.

🔴 **BUG TROUVÉ EN ÉCRIVANT L'INVARIANT** : `ImGui::Begin` rend false quand la
fenêtre est REPLIÉE ou clippée, et les trois `OnRenderUI` sortaient avant la
capture du rect ⇒ le rect gardait sa taille DÉPLIÉE et un objet lâché sur cette
surface fantôme partait quand même vers cette fenêtre. Les trois sorties
invalident désormais. Une **copie MORTE** aussi : `inventory_viewer::SendUnequip`
n'était appelée nulle part (supprimée, pas déplacée).

🔴 **`ItemViewerBase`** (`features/windows/item_viewer_base.h`) : **21 des 22
membres de `CartViewer`** étaient déjà dans les deux autres. Un seul défaut
divergeait (`tabs_vertical_`, faux pour l'entrepôt seul → posé dans son ctor).
Les trois `enum PendAction` commencent tous à 0, donc `pend_action_ = 0` convient
aux trois : **`static_assert` dans chaque dérivée** pour que la fusion casse à la
compilation si quelqu'un réordonne. ⚠ Un membre resté déclaré dans une dérivée
**MASQUERAIT** celui de la base **en silence, sans erreur ni avertissement** —
le vérifier par grep après extraction, jamais par « ça compile ».

🔴🔴 **OUTILLAGE : le heredoc de Bash MANGE les antislashs**, même avec
délimiteur quoté — `'\\'` devient `'\'`, et `'\0'` devient un **OCTET NUL réel**
dans la source (MSVC dit alors « constante caractère vide »). Payé trois fois
dans la session. ⇒ Pour tout script ou toute source contenant un antislash :
**l'outil Write**, jamais un heredoc.

## DOUZIÈME TRANCHE — 2026-08-25 : **l'ANNUAIRE des fenêtres** (`6650daf`, `7ae3ad2`)

**83 fenêtres natives** — id + vtable + nom de classe — dans la section
« L'ANNUAIRE » de `ragnarok/uiwnd.h`, **triée par VALEUR**. 87 déclarations ôtées
de 28 fichiers. Reste 20 constantes dehors : aucune n'est un identifiant de
fenêtre (offsets de sous-classe, ids MSI, codes de commande).

🔴🔴 **UN FILTRE PAR NOM RÉINTRODUIT L'AVEUGLEMENT DE NIVEAU 2**, même dans
l'outil censé le traquer. Mon détecteur ne retenait que `Win|Wnd|Window` : il a
donc compté `kCmdCartToBody = 0x4d` pour un singleton alors qu'il vit dans DEUX
fichiers — seul l'un avait le mot. ⇒ **Grouper par valeur, sans filtre
sémantique, PUIS lire.** L'inventaire sans filtre a sorti **62 doublons francs
et 49 noms ambigus** là où le filtré en montrait 8.

🔴 **CE QUE LE TRI PAR VALEUR A TROUVÉ, ET RIEN D'AUTRE N'AURAIT TROUVÉ** :
`kFeedBlockerWndId = 10011` (pet.cc) et `kExchangeId = 0x271b` (trade_window.cc)
sont **la même valeur**. `pet.cc` écrivait « le client la cherche par id, sans
jamais nommer la classe » — faux : `trade_window` l'avait RE'ée en live, c'est
**`CUIExchangeUI`**. Deux fichiers savaient chacun une moitié ; l'annuaire les a
mis à la même ligne. (Le code était juste — on ne nourrit pas son familier en
plein échange — seule l'étiquette manquait.)

🔴 **UNE CLASSE NE VAUT PAS UN IDENTIFIANT.** `UIMerchantItemPurchaseWnd`
(vtable `0x0103D2B0`) sert **à la fois 44 et 178** ; `UIMerchantMirrorItemWnd`
sert 42 et 179. La règle « `k` + nom de classe » ne vaut donc que pour une
classe qui désigne UNE fenêtre ; sinon nom de RÔLE, classe en commentaire. Et
quand aucune classe n'a été relevée, le nom reste **descriptif et le DIT** —
inventer refait l'erreur de `UIWorldViewWnd`, un nom qui n'existait nulle part.

🔴 **MIGRER UNE MOITIÉ DE FAMILLE EN CRÉE UNE.** Premier tour : les
IDENTIFIANTS. Les VTABLES restées sur place (`kCashVTable`,
`kParamCompareVTable`) sont devenues des doublons de ce que je venais d'écrire.
Idem `kMsgUiAction` fusionné depuis 2 fichiers sur 4. ⇒ Après une fusion,
**relancer le relevé** au lieu de croire le terrain balayé.

🔴 **LES COMMENTAIRES ORPHELINS SONT DE TROIS NATURES**, et seule l'ouverture les
distingue : **dupliqué** (le texte est parti au foyer sans être ôté d'ici — pire
qu'orphelin, deux exemplaires qui divergeront), **dérivé** (« les quatre
fenêtres » n'en montre plus que trois — le détecteur ne le signale même pas,
puisqu'il reste des déclarations derrière), **orphelin vrai**. Règle unique :
**le fait d'IDENTITÉ part au foyer, le fait de COMPORTEMENT reste où il gouverne
du code.**

⚠ `awk 'length($0)>80'` compte des **OCTETS** : en UTF-8 un accent en vaut 2, `─`
3, un emoji 4 — une trentaine de faux positifs sur des commentaires français. Et
🔴 **une table alignée à la main** (`kActions[]`, 96 colonnes, toutes ses lignes
EXACTEMENT à 96) : le débordement était assumé, l'alignement était le choix. Un
renommage la rend irrégulière ⇒ **réaligner, pas ramener à 80**, et surtout ne
pas lâcher `clang-format` sur le fichier.

**RESTE À FAIRE (62 doublons francs, hors périmètre)** : `kInfo*` — la structure
`ItemSkillInfo` éclatée sur **SEPT** fichiers (`kInfoDamaged`, `kInfoRefine` : 7
copies chacun) · `kNode*` — la marche de `std::map`, sous **DEUX conventions**
(`kNodeLeft` / `kNode_Left`) dans 5 fichiers · `kCmd*`, `kOp*`, `kOffWidth`.
⚠ Deux contradictions VÉRIFIÉES et INOFFENSIVES, ne pas les « corriger » :
`kItemSkillInfoSize` 0xf8 vs 0x100 (vraie taille vs allocation arrondie, dit
dans `item_cell.cc`) et `kTypeMob`/`kTypeNpc` inversés entre `target_frame` et
`navigation_window` (**deux énumérations différentes** : ZC 0x0F2A d'un côté,
types de résultat de navigation de l'autre).

## VINGTIÈME TRANCHE — 2026-08-26 : les `IM_COL32` (`92ae5ec`)

**432 littéraux → 334.** Classement par **RÔLE** et non par valeur : le rôle se
lit dans l'appel qui consomme la couleur (`AddRectFilled` = fond, `AddRect` =
trait, `AddText` = texte, `PushStyleColor(ImGuiCol_X)` = style). 🔴 Sans lui,
`IM_COL32(0,0,0,200)` — ombre de texte ICI, fond de minimap LÀ — paraît un
doublon alors que rien ne les rapprochera jamais.

**Rangé parce que DÉJÀ identique** : 73 littéraux qu'**ImGui nomme lui-même**
(`IM_COL32_WHITE` / `_BLACK` / `_BLACK_TRANS` — les chercher AVANT d'inventer un
nom) · `kColOk`/`kColBad`/`kColWarn`, déjà nommées mais déclarées **trois fois**
(une par fenêtre de fabrication) · le chrome des fenêtres de description (13
sites) · la teinte de survol des 3 viewers · `kTextShadow` (6 sites).

🔴🔴 **LE LIVRABLE EST LE CONSTAT, PAS LA FUSION.** Les ~334 restants ne se
rangent PAS : leurs teintes divergent pour un même rôle — **60 nuances de fond**
(7 noirs : α 205 200 180 150 45 40 30), **45 de trait** (8 noirs), **35 de
texte** (4 noirs). Les ramener à deux ou trois valeurs changerait l'apparence de
la moitié des fenêtres : refonte visuelle, pas rangement. Le relevé chiffré est
**écrit dans `ui/ui_palette.h` avec sa date**, pour que la décision se prenne sur
des nombres. ⇒ Quand un relevé ne débouche pas sur une fusion, **le relevé
lui-même est ce qu'on livre** — dans le code, pas dans un message.

⚠ Les `ImU32` ajoutés ne valent PAS leurs homonymes `ImVec4` : (13,107,31) n'est
pas `kGreen` (≈25,127,38), (166,102,0) n'est pas `kWarn` (≈140,84,20). Deux
familles voisines et distinctes, dans le même en-tête — c'est écrit au foyer.

## DIX-NEUVIÈME TRANCHE — 2026-08-25 : le GESTE de rendu (`5294f82`)

✅ **VÉRIFIÉ EN JEU par l'utilisateur le 2026-08-26** (« fonctionne ») sur les
trois commits `eaf67f2` / `6653a34` / `5294f82`.

⚠ **CE QUE CE TEST NE PROUVE PAS.** Deux des trois commits n'étaient censés RIEN
changer à l'écran : « fonctionne » y confirme l'absence de régression, pas un
comportement neuf. Restent hors de portée d'un test ordinaire :
- le correctif d'échelle du strip d'onglets du CHARIOT — invisible tant que
  l'échelle de l'interface vaut 100 % (`ro::Px(22) == 22`). Il ne se voit qu'avec
  une interface AGRANDIE ;
- le salon de chat pendant un CHANGEMENT DE CARTE — le seul endroit où la
  lecture brute et `ActiveModeIfReady()` ne répondent pas pareil ;
- le filtre de recherche du chat sur un texte ACCENTUÉ (le `std::tolower` retiré
  ne divergeait que sur les octets ≥ 0x80).

Quatrième matière : ni une valeur, ni une fonction nommée, mais un **geste** —
plusieurs lignes qui, ensemble, font une chose. Ici « écrire deux fois le même
texte, la première décalée ». **27 groupes d'`AddText` répétés → 9**, les neuf
restants étant l'implémentation des helpers ou des gestes réellement différents.

Deux fonctions dans `ro_imgui` :
- 🔴 **`ro::AddTextHalo`** — le cerne **8 voisins**, écrit SIX fois à la boucle
  près (`item_cell`, feuille de personnage ×3, `skill_bar` ×2, `status_icon_bar`).
  Les deux dernières y ajoutaient un faux gras, lui aussi recopié.
- **`ro::AddTextRelief`** — une passe décalée puis le texte, DOUZE sites. Couleur
  et décalage restent chez l'appelant : **le rendu est identique partout**, les
  alphas divergents (160/180/200/210) sont conservés. Un décalage **nul** vaut
  « pas de relief » : les trois `if` d'ombre conditionnelle disparaissent.
  ⚠ Le même helper couvre le **faux gras seul** (relief nul + `bold`).

🔴🔴 **LIRE LES LIGNES AUTOUR DE CE QU'UN RELEVÉ SIGNALE.** Mon afficheur montrait
la ligne trouvée, pas la **boucle qui l'englobe** : j'ai pris pendant plusieurs
tours un halo 8 directions pour « une ombre décalée », et conçu un helper qui ne
le couvrait pas. Un relevé donne un POINT, jamais une structure.

⛔ **NE PAS ramener les cernes à QUATRE passes sur le helper à huit** — deux dans
la feuille de personnage (en croix), un dans `entity_names` (en diagonale). Ils
sont volontairement plus légers ; huit passes les épaissiraient. C'est écrit dans
le commentaire du helper pour qu'un prochain relevé ne les reprenne pas.

## DIX-HUITIÈME TRANCHE — 2026-08-25 : **la PALETTE** (`6653a34`)

Troisième matière, après les valeurs et les fonctions : les **couleurs**.
`ui/ui_palette.h` (`ro::pal`), 8 entrées nommées par leur RÔLE. **58 déclarations
`ImVec4` → 13**, 19 noms → 9, et plus aucune valeur ne porte deux noms.
Le gris de libellé en portait **quatre** pour **dix-sept** déclarations ;
`kGreen` avait dérivé en **quatre nuances**, `kRed`/`kBlack`/`kAmber` en deux.
⚠ Une CONVENTION D'ÉCRITURE documentée (« déclarez ces cinq ImVec4 ») **est** une
duplication : elle prescrit la recopie. La fiche a été corrigée en même temps —
sinon la fenêtre suivante refait le problème.

🔴 **AUCUN CHANGEMENT VISUEL, et c'était la condition.** On ne fusionne que
l'IDENTIQUE ; les 7 nuances minoritaires restent, chacune avec une note disant
de quelle entrée du foyer elle s'écarte. Aligner des teintes est un choix
d'interface, pas un rangement — il revient à qui regarde l'écran.

**TROIS FAUTES D'OUTILLAGE, toutes rattrapées par le compilateur** :
- 🔴🔴 **NE JAMAIS TRANSPORTER UN INDEX À TRAVERS UNE MUTATION.** Le lot A avait
  relevé ses numéros de ligne AVANT que le lot B ne supprime les siennes : chaque
  suppression décalait tout d'un cran, et le lot A ôtait ensuite la ligne
  VOISINE de sa cible (139 erreurs). ⇒ **Substituer d'abord, supprimer ensuite
  par CONTENU** (ici : toute ligne devenue `ImVec4 ro::pal::kXxx(...)`), ce qui
  est insensible au décalage.
- 🔴 **La portée d'une déclaration n'est pas son indentation.** Une constante en
  colonne 0 vaut pour TOUT le fichier ; ma fonction `portee()` lui trouvait quand
  même des bornes (le `namespace {` englobant), plus étroites que la vérité —
  d'où neuf usages orphelins dans deux fichiers.
- 🔴 Une regex de suppression finissant par `;\s*$` rate une déclaration suivie
  d'un **commentaire de fin de ligne**. Elle survit, et redéfinit le symbole.

🔴 **UNE CONSTANTE LOCALE MASQUE LA GLOBALE DE MÊME NOM** — et c'est ce qui rend
une substitution par nom dangereuse : `character_sheet` déclare `kBlack` en
portée fichier à (0,0,0) et, dans une fonction, un AUTRE `kBlack` à
(0.1,0.1,0.13). Substituer sur tout le fichier y changerait une couleur **en
silence** : aucune erreur, juste un noir de trop.

⚠ **Ce qui n'est PAS traité** : les ~240 nuances de `IM_COL32` posées en
draw-list (ombres, fonds, bordures) — **17 noirs translucides distincts**. Elles
n'ont pas de nom à fusionner et leurs alphas divergent par site : sujet
d'apparence, pas de rangement.

## DIX-SEPTIÈME TRANCHE — 2026-08-25 : les FONCTIONS, par leur NOM (`eaf67f2`)

Après les VALEURS, les **fonctions**. 34 fichiers, +340/−355. Le relevé change
de nature : on ne compare plus des corps, **on groupe par NOM**.

🔴🔴 **RELEVER PAR NOM, PAS PAR SIMILARITÉ DE CORPS.** Un seuil de similarité
rate par construction la copie qui a le plus dérivé — celle qui compte. Le NOM,
lui, survit à la dérive. 82 noms définis dans ≥ 2 fichiers, 15 familles fusionnées.
⚠ Grouper ensuite par SIGNATURE pour séparer les doublons des homonymes
(`ReadInt(void*, int)` vs `ReadInt(uintptr_t)` : rien à voir).
⚠ Mais une signature différente peut CACHER le même concept : `ContainsNoCase`
avait une 3ᵉ écriture prenant `std::string`, invisible au groupage ;
`ElementName(uint8_t)` / `(int)` de même. Après le tri par signature, **relire la
liste des « homonymes »** — c'est là que se cachent les surcharges.

**TROIS DÉFAUTS, tous « un correctif qui n'atteint qu'une copie sur N »** :
- 🔴 le **chariot** n'avait pas reçu `ro::Px` sur son strip d'onglets, quand
  l'inventaire ET l'entrepôt l'avaient — le commentaire de l'inventaire décrivait
  pourtant le défaut corrigé. Visible dès que l'interface est agrandie ;
- 🔴 le filtre du chat comparait par `std::tolower` (donc la LOCALE) **sous un
  commentaire annonçant « (ASCII) »**, sur des noms de joueurs en UTF-8. Idem
  `LowerAscii` du dialogue NPC et `IsHex6` (`std::isxdigit`). `utils/text.h`
  proscrit ce chemin depuis sa création, raison à l'appui ;
- 🔴 trois ponts vers le mode de zone : deux écrivent « lecture BRUTE, PAS
  `ActiveModeIfReady()` », le troisième (le plus récent) appelait le getter GATÉ
  sans le dire. ⇒ **Deux choix argumentés contre un silence : le silence perd.**

🔴 **UN WRAPPER DOCUMENTÉ N'EST PAS UN DOUBLON.** Beaucoup de « copies » étaient
des délégations explicites, vestiges de fusions passées (`char_select::LocalToUtf8`
→ `ro::`, `minimap::MapDisplayName` → `rag::`). **Lire le corps avant de conclure**
— et `rag::ActiveMode()` existait dans `globals.h` sans qu'aucun des 11 sites ne
l'appelle : le foyer peut exister et n'être utilisé par personne.

**CE QU'ON NE FUSIONNE PAS, et qui le dit dans le code** : les noms d'élément /
taille / race d'un monstre (fiche = traduits par msgstring, navigation =
littéraux anglais) — fusionner déciderait à la place du joueur ; `HideWnd`, qui
nomme une INTENTION ; `TabStripWidth`, dont les 3 corps lisent chacun leur `g_tab`.

**PIÈGES D'OUTILLAGE PAYÉS** :
- 🔴🔴 un heredoc qui **DOUBLE** les antislashs (cf. [[feedback_code_hygiene]]) ;
- 🔴 **ne jamais associer le n-ième corps au n-ième fichier** dans un relevé :
  mon afficheur triait les corps par `sorted(set(...))`, décorrélés de la liste
  au-dessus. J'ai cru voir l'inventaire lire la liste du CHARIOT — c'était
  l'affichage. **Vérifier au fichier avant d'annoncer un bug** ;
- 🔴 substituer les USAGES par MOTIF, n'ancrer que les SUPPRESSIONS : un
  remplacement par ancre exacte a raté un second appel dans le même fichier.

## TRANCHES 13-16 — 2026-08-25 : **le chantier est SOLDE**, 4 doublons restants

✅ **VÉRIFIÉ EN JEU par l'utilisateur le 2026-08-25** : aucun problème observé sur l'ensemble des neuf commits. ⚠ Ce que ce test NE couvre pas forcément : un warp *pendant* qu'une boutique/entrepôt est ouvert (la détection fusionnée), et l'étiquette de l'inspecteur sur un objet au sol (le seul comportement qui devait CHANGER).

Suite de la 12e, même journée. **62 doublons francs → 4**, et les quatre restants
portent dans le code la raison qui les y laisse.

**QUATRE FOYERS DE PLUS** : `ragnarok/stl_node.h` (`rag::treenode` / `rag::listnode`
— **DEUX espaces de noms, jamais un préfixe commun** : `kValue` vaut 0x10 dans un
arbre et 0x08 dans une liste) · `ragnarok/skill_info.h` (`rag::skillinfo`) · la
disposition d'`ItemSkillInfo` dans `rag::itemlist` · l'annuaire des 83 fenêtres
dans `uiwnd`.

🔴🔴 **DEUX STRUCTURES QUE L'IDB NOMME PAREIL**, et c'est la confusion la plus
chère de ce coin du client. `ItemSkillInfo` = ctor `0x006a1b20`, deux
`std::string` (+0x2c, +0x44), dtor **`0x005a4300`**, `sizeof` **0xF8** (mesuré :
dans la pile de `sub_5A75A0`, la locale suivant la 2e string est à +0xB4 d'elle).
`CSkillInfo` = ctor `0x00739700`, **vtable en +0**, `const char*` « Unknown-Skill »
en +0x20, dtor `0x00739cd0`. ⚠ +0x04 = « index d'inventaire » ici, « fiche
utilisable » là ; +0x10 = « quantité » ici, « niveau appris » là. Aucun
compilateur ne le dira.

🔴🔴 **LE NIVEAU QUE L'OUTILLAGE NE COUVRAIT PAS : LES LITTÉRAUX.** Un `constexpr`
se relève, une EXPRESSION non. Recette qui marche : lire les valeurs d'un foyer,
chercher `+ 0xNN` sur une ligne qui manipule un pointeur, **ET NE GARDER QUE LES
FICHIERS QUI INCLUENT DÉJÀ CE FOYER** — sans ce dernier filtre, `char buf[32]`
noie tout (1re version : 22 sites dont 2 vrais). 25 sites trouvés, dont la
disposition d'`ItemSkillInfo` **dans les fichiers que je venais de migrer** :
la migration ne réécrit que les usages NOMMÉS.
⇒ **Après avoir CRÉÉ un foyer, rejouer le relevé de littéraux sur SES valeurs** :
un catalogue neuf ne certifie que ce qui existait avant lui.

🔴 **UN CORRECTIF QUI N'ATTEINT QU'UNE COPIE SUR TROIS** (le vrai bug de la
journée) : la catégorie 1 du quad de picking est l'**OBJET AU SOL** (RE live
2026-08-19 : quad `CItem`, +0x1c = constante `0x7D03`, +0x20 = 1).
`entity_context_menu` le savait ; `entity_inspector` affichait « NPC de carte »
sur un objet au sol, et `char_diagnostics` le colorait sur `case 1:` **en
littéral**. Trois noms différents pour une valeur ⇒ invisible à toute recherche
par nom.

**AUTRES LEÇONS PAYÉES :**
- 🔴🔴 **Sur N copies, LIRE LES N COMMENTAIRES avant d'en promouvoir un.** En
  publiant `kCursorHand` j'ai emporté « à confirmer en jeu » (ro_imgui.cc) alors
  que `weapon_refine_window` disait « **vérifié en jeu** ».
- 🔴 **Migrer une MOITIÉ de famille en crée une** : premier tour = les ids, les
  vtables restées derrière sont devenues des doublons de ce que je venais
  d'écrire. Après une fusion, **relancer le relevé**.
- 🔴 **Le SEUIL décide de ce qu'on trouve** (3e fois) : le relevé de littéraux
  ignorait < 0x20, il a donc signalé `+0x20` (`kOffPosY`) et manqué `+0x1c`
  (`kOffPosX`), son voisin dans les 5 sites.
- 🔴 Les **zéros de tête** (`0x0cc`, `0x0a0`) et le partage **décimal/hexa**
  (`0x17` vs `23`, `248` vs `0xf8`) battent toute recherche textuelle. Un
  commentaire de foyer proclamait « nettoyé de ONZE fichiers » pendant que deux
  copies survivaient dessous, en `0x0cc`.
- 🔴 Un **lookbehind `(?<![:\w])`** protège des `ns::ns::` mais protège AUSSI les
  usages QUALIFIÉS d'un autre fichier (`itemdb::kItemDescWndId`,
  `gamescene::kNodeActor`). Après migration, grepper le nom **qualifié**.
- 🔴 **En CRLF, `re.M` ne sauve pas `$`** : un motif finissant `;$` matche après le
  `\r` et ne trouve RIEN. Découper en LIGNES, jamais regexer le texte entier.
- ⚠ Ce qu'on NE fusionne PAS : une adresse citée dans un message de diagnostic ou
  un bloc de RE est une **TRACE**, pas un doublon.

**RESTE (4, documentés sur place)** : `kSlotCount` (macros ≠ équipement),
`kSlots` (`thread_local` ≠ `static`), `kOffResPath` (1 membre sur 9 d'une famille
non homogène), `kMsiLevel` (2 entrées ne valent pas un foyer).

**Why:** sans annuaire, porter le client demandait de retrouver 672 constantes
éparpillées, dont certaines invisibles au grep (casse, littéraux inline).
**How to apply:** cf. `docs/source_layout.md` (section renommage en masse) et
[[reference_source_layout]]. 🔴 Piège payé : supprimer une LIGNE n'est pas
supprimer un NOM — une déclaration groupée emporte des constantes vivantes
(`7f15bae`).
