# Portage vers le client kRO 2026-07-07

> Journal du chantier. La fiche de mémoire `project_client_2026_port` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Chantier ouvert le 2026-08-26. 🔴 **Tous les relevés sont dans `docs/2026/`** (voir son `README.md`) — la racine de `docs/` reste au client de PRODUCTION 2025-07-16.

Chantier ouvert le 2026-08-26. Cible : `D:\KRO\Ragexe.exe`, BuildDate **20260707**,
jamais packé (RTTI intacts) — cf. [[reference_client_install_path]].

## Le montage de travail

**DEUX IDA en parallèle** : `mcp__ida-pro-mcp__*` → 13337 = **2025-07-16**
(legacy), `mcp__ida-2026__*` → 13338 = **2026-07-07**. Cf.
[[feedback_workflow_ida_contention]] pour la mise en place.

Branche `client-2026` dans `WARP0716` (remote : `github.com/Stingor/WARP0716`).
🔴 Le garde de `Scripts/Init/Initializer.qjs` refusait tout sauf `20250716` ;
remplacé par un dictionnaire `SUPPORTED_BUILDS` (20250716 + 20260707).

## 🔴🔴 La cause n°1 des échecs de patch : le SAUT en dur

Mesuré sur `RestoreGMWeaponTrail`. Le `head` du motif
(`E8 ?? ?? ?? ?? 83 C4 04 84 C0`) se retrouve sur **les 25 sites** du 2026. Ce
qui casse, c'est le `74 1D` / `74 50` épinglé ensuite : **aucun site 2026 ne
porte ces valeurs**, et beaucoup sont passés du saut COURT au saut LONG
(`74 xx` → `0F 84 xx xx xx xx`) parce que le bloc sauté a dépassé 127 octets.

➡ Couper le motif AVANT le saut, s'ancrer sur ce qui ne bouge pas (offset de
structure poussé, appel suivant, vecteur écrit), JAMAIS sur la longueur d'un
saut. ⚠ Vérifier aussi les offsets de structure eux-mêmes.

## État des trois chantiers

| | ampleur | méthode |
|---|---|---|
| **patchs WARP** | **116 actifs** | motifs ; 126 bornes `>= 20xxxxx` passent ; **5 patchs s'auto-verrouillent** sur 20250716 (`AllowChatWhileBerserk` et `RestoreSkillAttackAnimation` sont ACTIFS) |
| **layout `CSession`** | ~20 champs | 🔴 offsets ONT bougé (accès `hp_` : 12 → 2). Scan mémoire EN JEU, comme la 1ʳᵉ fois |
| **adresses Bourgeon** | 407 / 480 déclarations | diffing ; **9/18 ancres de `configuration.h` portées** |

🔴 **Une signature d'octets ne porte PAS d'un build à l'autre** (4/18). Ce qui
marche : les **chaînes référencées** (5/9 des restantes). Cf.
`docs/2026/client_2026_address_map.md`.

## Le protocole n'est PAS un obstacle

**0 opcode utilisé par Moonlight ne change de longueur** (26 changent au total,
aucun utilisé). Seul point : `0x02F7` `ZC_UPDATE_GDID`, absent du 2026.
Pas de fork de Moonlight à prévoir. Cf. [[reference_native_packet_len_resolver]],
`docs/2026/packet_len_diff.md`, `docs/2026/client_2026_opcodes.md`.

## Les catalogues WARP

`WARP0716` (le tien, 173) et `WARP0219` (`Demonbytes-lab`, 174, **seul à citer
`20260210`**). 🔴 Sens du transfert : **WARP0219 → WARP0716, jamais l'inverse** —
~33 de tes patchs actifs n'existent que chez toi (22 modifiés + 5 exclusifs + 6
créations). Cf. `docs/2026/warp_patches_port.md`.

⚠ 4 patchs actifs (`RestoreGMWeaponTrail`, `DoramHeadPalShared`,
`NoLookalikeNameWarning`, `PetTalkMarker`) vivaient sur des branches NON
fusionnées alors que `save_exe.yml` les active.

**Mis de côté (cosmétique) :** `RestoreGMWeaponTrail`. Ancre déjà résolue si
besoin : `IsGidInGMList` = `0x00A395F0` (2026), 25 appelants ; seules les
`shapes` sont à revoir. Cf. [[reference_weapon_trail_gm_gate]].

## 🔴🔴 Le client 2026 DEMARRE et charge ses donnees (2026-08-26)

Ordre reel des obstacles rencontres, tous leves. Cf. `docs/2026/README.md`.

**1. `steam_api.dll`** liee STATIQUEMENT (7 imports) : le loader refuse de
demarrer sans elle. Patch **`NoSteamAPI`** (retire le descripteur d'import +
court-circuite le bloc Steam de `WinMain`, garde par `strstr(cmdline,"Steam")`
— 9 mots-cles de ligne de commande y sont testes).

**2. ASLR + CFG** : `DllCharacteristics` **0x8100 (2025) → 0xC040 (2026)**.
⚠ **Ni WARP ni IDA ne peuvent l'enlever** : les deux ne travaillent que sur les
SECTIONS, or ces drapeaux sont dans l'en-tete PE (WARP lit mais l'ecriture est
ignoree EN SILENCE ; les segments de l'IDB commencent a `0x00401000` et
`0x400000` rend `ff ff`). D'ou **`fix_aslr.py`**, applique UNE FOIS sur l'exe
SOURCE : WARP copie la source et ne patche que les sections, donc l'en-tete
traverse tel quel et toutes les generations en heritent. Base redevenue
`0x400000` ⇒ **adresse IDB == adresse en memoire**.

**3. Bourgeon tuait le client avant `WinMain`** : `msgoverride::Reload()` posait
son detour a `0x00A9ED30` (l'adresse de `MsgStringTable_GetById` **en 2025**)
DIX-SEPT LIGNES avant `client_.Initialize()`, le seul endroit qui verifie la date
et refuse un exe inconnu. Sur le 2026 cette adresse tombe au milieu d'une
instruction, pile sur le `rel32` d'un `call` du ctor de la table de paquets →
saut vers `0x92B4191D`. ✅ **Corrige et commite** (`584c710`) : le detour est
desormais pose APRES la verification. Ce n'etait donc pas un garde manquant mais
un ORDRE inverse.

**4. `Customize Iteminfo lub`** echouait : le 2026 ne fait plus `push offset`
mais un selecteur `mov eax,<sak> / mov ecx,<main> / cmovnz`. ✅ Patch repare
(`CustomPath.qjs`, cas `PATCH_MOV_IMM_SITES`) — l'immediat d'un `mov r32,imm32`
est en +1 comme celui d'un `push imm32`, mais il y a **DEUX sites**.

**5. API Lua reduite** : `AddItemIsCostume` et `AddItemPackageID` ont ete
RETIREES en 2026. ✅ Garde `and AddItemIsCostume ~= nil` dans
`SystemEN/itemInfo.lua` (sans effet sur le 2025, ou la fonction existe).

**6. `MsgString_KR` → `MsgString_US`** : le 2026 charge `Lua Files\MsgString_US`
(et a abandonne `Lua Files\DataInfo\HelpMsgStr`). Ce fichier definit la table
Lua `MsgStrID` ; absent du GRF 2025, `hotkey.lua:135` mourait. ✅ Resolu en
posant le vrai `msgstring_us.lub` **extrait de `D:\KRO\data\luafiles514`**
(326 fichiers) dans `Moonlight-Destiny/data/luafiles514/lua files/`.
Le patch `UseKrMsgString` existe en repli mais **NE PAS l'activer** : le vrai
fichier vaut mieux qu'un pari sur la compatibilite des IDs.

**7. EOS** — 🔴 **corrige mon affirmation precedente** : EOS bloque bien, mais
de facon contournable. `LoadLibraryW(L"EasyAntiCheat/EasyAntiCheat_x86.dll")`
(DLL absente, chargee DYNAMIQUEMENT d'ou son absence des modules), puis
`EOS_Initialize`/`EOS_Platform_Create` avec les credentials de Gravity
(`Ragnarok_USA`). ✅ Patch **`NoEOSAntiCheat`** : les deux appelants du bootstrap
TESTENT le resultat et continuent en cas d'echec — seul le MessageBox modal
bloquait. NOP des 3 boites `"EOS Error"` (**18 octets**, `push eax` et non
`push 0` : EAX vaut deja 0) + `jnz→jmp` sur le controle FATAL de
`CEOSAntiCheatMgr::CreateInstance()`.

🔴 **Lecon transversale : un motif d'octets se lit dans le BINAIRE, jamais
dans le pseudocode.** Hex-Rays affiche `MessageBoxA(0, ...)` la ou le compilateur
a emis `push eax`. Trois patchs de la soiree ont echoue au premier essai pour
cette raison.

### 🔴 Les fichiers Lua : poser les `luafiles514` du 2026 sur le DISQUE

Le 2026 reclame des fichiers en `_us` que le GRF 2025 n'a pas : `MsgString_US`,
`navigation\navi_npcdistance_us`, etc. ⚠ Le chargeur Lua repond alors
**« not enough memory »** (il alloue sur une taille invalide) et non
« file not found » — message trompeur, chercher le FICHIER, pas la memoire.

✅ **Les 326 fichiers de `D:\KRO\data\luafiles514` ont ete copies dans
`Moonlight-Destiny/data/luafiles514/`** (2026-08-26). Copie SANS ecrasement, et
sans danger : `DataFolderFirst` echouant sur le 2026, les GRF restent
prioritaires — seuls les fichiers ABSENTS des GRF (les nouveaux `_us`) sont donc
pris sur le disque. Les customisations de `SystemEN/` gardent la main via les
patchs `Custom*Lub`.

🔴 Solution durable si le disque ne suffit plus : mettre ces fichiers dans
**`stingor.grf`**, le premier GRF de `DATA.INI`
(`stingor` → `moonlight` → `data`), aujourd'hui quasi vide. Ils gagneraient
alors sur `data.grf` sans dependre de `DataFolderFirst`.

### Les 3 categories de rupture entre 2025 et 2026

| categorie | exemple | ou ca se repare |
|---|---|---|
| **forme d'instruction** | `74 xx` → `0F 84` ; `push` → `mov`+`cmov` | le patch WARP |
| **contenu metier** | la cascade des langtypes (6 patchs) | le patch, mais il faut relire le metier |
| **surface d'API** | `AddItemIsCostume` retiree | les DONNEES (scripts Lua), aucun patch n'y peut rien |

## ✅ 2026-08-26 : le client 2026 ATTEINT L'ECRAN DE LOGIN

Saisie du compte et du mot de passe possible. Reste a valider la connexion au
serveur.

**8. Le tick EOS deréférençait un pointeur NUL** — provoqué par mon propre patch.
Forcer `jnz→jmp` sur le contrôle de `CreateInstance` fait croire au client que le
manager existe ; il le ticke alors à chaque frame :

```
mov  ecx, dword_11EAFA4   ; NULL, l'instance n'a jamais ete creee
call sub_7A29E0
    mov eax, [ecx+0Ch]    ; ACCESS_VIOLATION lecture sur 0x0C
```

Les DEUX appelants font cela sans tester le pointeur. ✅ Corrigé dans
`NoEOSAntiCheat` : `mov eax,[ecx+0Ch]` (3 octets) → `33 C0 C3`
(`xor eax,eax` / `ret`). Les appelants ignorent la valeur de retour.

🔴 **Leçon : ne jamais forcer un `jnz`→`jmp` sans vérifier ce que le chemin
« succès » suppose ensuite.** J'avais écrit « EOS reste non initialisé, c'est le
but » sans regarder si le tick testait son `this`.

🔴🔴 **Et le crash était MASQUÉ** par le `TopLevelExceptionFilter` du client,
qui plante lui-même en formatant son rapport — Windows n'accusait que
`ucrtbase`. Cf. [[feedback_debug_tooling]] pour la marche à suivre (dump
COMPLET + recherche du premier `EXCEPTION_RECORD`).

### Fichiers Lua : ce que le 2026 réclame en plus

`msgstring_us.lub`, `navigation\navi_npcdistance_us.lub`… ⚠ le chargeur Lua
répond **« not enough memory »** quand le fichier est absent (il alloue sur une
taille invalide) — message trompeur, chercher le FICHIER.
✅ Les 326 fichiers de `D:\KRO\data\luafiles514` sont copiés dans
`Moonlight-Destiny/data/luafiles514/`.

⚠ Les patchs `Ignore Resource errors` / `Palette` / `Lua` / `Quest` sont ACTIFS :
**le client se tait sur les fichiers manquants**. Les décocher pour diagnostiquer.

## 🔴🔴 PORTER UNE ADRESSE d'un build a l'autre : ce qui marche

Mesure du 2026-08-26 sur les **784 adresses du manifeste**
(`docs/address_manifest.md`, genere par `tools/gen_address_manifest.py` — NE PAS
en refaire un, il est plus complet qu'une extraction a la main).
Resultat : **157 portees (20 %), 0 divergence**. Detail :
`docs/2026/address_port_2025_2026.md` + `port_2025_2026.json`.

| vecteur | rendement | verdict |
|---|---|---|
| signature d'octets (prologue) | **4/18** | ❌ **NE PORTE PAS** — le compilateur reecrit |
| **RTTI : `(classe, slot)` de vtable** | **67, AUCUN echec** | ✅✅ **le vecteur roi** |
| chaine referencee (≥ 2 communes) | 22 | ✅ solide, mais ~215 fonctions seulement en ont |
| propagation par appels | 61 | ✅ si compte d'appels IDENTIQUE |
| globale par operande memoire | 7 | ⚠ le code derive trop (3 fonctions sur 36) |

🔴 **Toujours essayer le RTTI en premier.** Une methode virtuelle se retrouve
par sa classe et son index, sans dependre d'un seul octet. 1593 vtables en 2025,
1341 en 2026 : `??_7<Classe>@@6B@` puis `get_dword(vt + slot*4)`. C'est ce qui a
permis de nommer 2527 methodes dans l'IDB 2026.

🔴🔴 **NE PAS utiliser `idautils.Strings()` pour comparer deux binaires** : il
ne liste que ce qu'IDA a IDENTIFIE comme chaine, et il en rate (il a rate
`AddItem`). Scanner les octets de `.rdata`/`.data` avec
`re.compile(rb'[\x20-\x7e]{4,80}\x00')`, puis `XrefsTo` sur les adresses
trouvees. Ce seul changement a fait passer le portage par texte de 19 a 26.

⚠ **Gardes indispensables**, sinon la methode fabrique de faux positifs :
- vote par ancre **DISTINCTE**, jamais par xref (une fonction qui appelle 10 fois
  la meme ancre pesait 10 voix → 17 correspondances fausses) ;
- une cible 2026 ne peut correspondre qu'a **UNE** source 2025 (collision = rejet) ;
- rapport de tailles > 0,6 entre les deux fonctions ;
- propagation : **compte d'appels identique**. A la 2e iteration, 83 conflits →
  signal d'arret, au-dela ca produit plus de bruit que de resultat.

➡ **Le controle qui valide tout** : confronter le resultat automatique aux
correspondances trouvees A LA MAIN. 4 concordent, 0 divergent. Une methode qui se
TAIT vaut mieux qu'une qui se trompe — une adresse fausse coute plus cher qu'une
adresse manquante.

**Reste 632 adresses, dont 370 en zone de DONNEES** : ni chaine, ni vtable, ni
appelant ne les atteint. C'est le gros morceau du portage.

## 🔴 Pieges de l'API WARP (mesures, 3 patchs perdus dessus)

- `Exe.SetInt8` / `SetInt16` prennent un **SIGNE** : ils refusent `0xC6` (198) ou
  `0x8000` **EN SILENCE**. Utiliser `SetUint8` / `SetUint32`.
- `Exe.SetHex` n'ecrit ni dans l'en-tete PE, ni sur certains sites de `.text`.
- 🔴🔴 **Ne PAS relire apres ecriture pour verifier** : WARP applique en
  DIFFERE, la relecture rend l'ancienne valeur et fait echouer un patch
  parfaitement applique. (Vrai l'inverse pour un fichier qu'on ecrit soi-meme.)
- Le vrai controle se prend **sur l'exe genere**, pas dans WARP.
- 🔴 Apres avoir modifie un `.qjs`, cliquer **`Load Scripts`** : sinon WARP
  execute encore l'ancienne version.

## ✅ Les QUATRE verrous de demarrage sont tombes (2026-08-27)

Le client 2026 demarre, affiche le login, le Service Select et le choix du
serveur. Dans l'ordre ou ils sont apparus :
`steam_api.dll` → ASLR/CFG → EOS AntiCheat → **nProtect GameGuard**.

## 🔴🔴 GameGuard : le patch NoGGuard se TAIT sur le 2026

| chaine | 2025 | 2026 |
|---|---|---|
| `GameGuard Error: %lu` (ancre de `validate`) | ✅ | ❌ |
| `nProtect GameGuard` (2e ancre) | ✅ | ❌ |
| `npkcrypt.dll` | ❌ | ✅ |

Les deux builds ont **change de mecanisme**, donc `NoGGuard` (2025) et
`NoGGuardLoader` (2026) sont **mutuellement exclusifs par construction** — on
peut laisser les deux coches.

🔴🔴 **Perdre le patch ne donne pas « GameGuard tourne », mais un CRASH.**
Le client embarque le chargeur **DEUX FOIS** et une seule copie est correcte :
`sub_A840E0` sort par `return 0` quand `LoadLibraryA` echoue, `sub_A84B70`
**oublie de sortir**, retombe dans le tronc commun et appelle les 11 pointeurs
jamais resolus → `call esi` avec `esi = 0` a **`0x00A84D96`**.

➡ La coupe se fait sur le **thunk de porte** `sub_A85B10` (14 octets,
`call`/`test`/`jnz`/`retn`) remplace par `33 C0 C3`. Trois raisons de couper LA :
le thunk ne commande **que** GameGuard (`sub_A84500` est une table de capacites
GENERIQUE, `matrix[14 * langtype + feature]` — la forcer deborde) ; un seul
appelant ; et **cet appelant IGNORE la valeur de retour** (l'instruction
suivante est `mov esi, [ebp-738h]`, pas un `test`).

## 🔴🔴 `clientinfo.xml` : il y a DEUX listes de serveurs, pas une

C'est ce qui a coute le plus de temps — j'ai attribue le symptome au mauvais
ecran. Details : `docs/2026/clientinfo_service_select_re.md`.

| fonction | source XML | ecran |
|---|---|---|
| `sub_C36480` | **toutes** les `<connection>` | Service Select, **AVANT** login |
| `sub_C361C0(idx)` | les `<subconnection>` de la connection `idx` | choix du serveur, **APRES** |

→ Un clientinfo **sans `<subconnection>`** laisse le **SECOND** ecran vide,
pas le premier. `sub_A39660` accepte `address`/`port` **aux deux niveaux** :
les mettre partout rend le fichier insensible a la branche empruntee.

🔴🔴 **Quand le clientinfo n'est pas applique, le client NE PROTESTE PAS** :
il part vers `112.175.128.137:6900` (l'IP kRO **en dur**, `off_10DB16C` /
`off_10DB170`). Le serveur local ne voit alors **jamais** la connexion arriver —
indiscernable d'un rejet serveur. Le nom du fichier depend du `<servicetype>`
(`sclientinfo.xml`, `gclientinfo.xml`, `pkclientinfo.xml`…, table a
`0x00FB2490`) ; `korea` = le nom nu.

🔴 `langtype` n'est PAS cosmetique : il indexe la table de capacites de
`sub_A84500`.

⚠ Avec deux services listes, **le premier clic mene a la PRODUCTION**. Un
serveur local muet n'est alors pas un bug.

## 🔴 Rappel de methode, re-verifie a mes depens

Une session x32dbg avec `EIP = 0` est **la session MORTE du crash precedent**.
J'ai failli presenter ses globales comme un releve du run en cours.
**Toujours verifier la fraicheur avant de lire une memoire de debogueur.**

## ✅✅ PORTER PAR LES OPCODES — le vecteur le plus rentable (2026-08-27)

**157 → 200/784 (25,5 %)**, et surtout **`g_session` portee STATIQUEMENT** :
`0x015FA3C0` → `0x014B73B0`. Le chantier « layout `CSession`, a rescanner EN
JEU » n'avait pas besoin du jeu. Details : `docs/2026/opcode_dispatch_port.md`,
scripts rejouables dans `docs/2026/scripts/`.

🔴 **L'idee** : tous les autres vecteurs partent du CODE (octets, vtables,
chaines) — ce que le compilateur reecrit. **L'opcode est impose par le
PROTOCOLE**, donc identique d'un build a l'autre.

Point d'entree : **`RecvLoop_DispatchPackets`** (2025 `0x00C9DF00` / 2026
`0x005095A0`), switch de 3011 / 3029 cases, **meme `lowcase` = 115**,
**775 opcodes communs**.

**Signature de co-occurrence** : pour chaque opcode, relever ce que son case
touche (globales + fonctions), **en suivant les appels d'UN cran** — le case
delegue au handler ; sans ce cran on ne voit que 196 globales au lieu de 643.
La signature d'un objet = l'ensemble des opcodes qui le touchent.
Signature identique **ET unique des deux cotes** ⇒ paire ; partagee par
plusieurs objets d'un meme cote ⇒ **on ne conclut pas**.

✅ Ce refus ecarte d'office `memcpy`/`sprintf` **sans liste noire**, et rend la
**collision impossible par construction** (un objet n'a qu'une signature).

🔴 **Une signature d'UN SEUL opcode est presque aussi sure qu'a 3+**
(mediane de ratio 0,975 contre 1,000) : ce qui compte est l'**unicite**, pas la
taille de la signature.

⚠ **Limite** : ne voit que ce qui est atteignable depuis le dispatch de
paquets — ni rendu, ni UI pure, ni audio. D'ou 623 paires pour seulement 43
adresses du manifeste.

### Le layout de `g_session` a ete REORGANISE, pas deplace

49 membres, **14 paliers distincts** — d'ou l'echec de toute approche par delta
constant. Les membres d'un **meme sous-systeme** partagent exactement le meme
ecart : pet **-56** (11 membres), homoncule **-1108** (8), etat/view-equip
**-1660** (9). Cette coherence par blocs **vaut validation**.

## 🔴🔴 Le portage precedent contenait 11 entrees FAUSSES sur 157

Son « 0 divergence » reposait sur **4** correspondances manuelles verifiables.
Deux **COLLISIONS** y avaient survecu — une impossibilite logique :

`0x00a3c3d0` (**0x0D octets**) revendiquee a la fois par
`Arrow_SpawnProjectileToTarget` (0xB0) **et** `ItemSkillMgr_GetInfoByResId`
(0xBE). Deux fonctions de cette taille ne deviennent pas le meme stub de 13
octets.

➡ **Deux controles a passer sur TOUT portage d'adresses**, ils coutent
quelques lignes : **(1) collision** — deux sources pour une cible = au moins une
erreur ; **(2) ratio de tailles** entre les deux fonctions. Liste dans
`docs/2026/port_suspects.json`.

## ✅ Second identifiant stable : l'ID DE FENÊTRE

`UIWindowMgr_MakeWindow` **`0x00A39340`** (2025) ↔ **`sub_A07BC0`**
`0x00A07BC0` (2026) — non nommee en 2026, retrouvee en listant les fonctions a
gros switch (elle ressort par nombre de cases + taille). **363 ids (0..362)** en
2025, **370 (0..369)** en 2026 : les 7 nouveaux sont ajoutes A LA FIN, les ids
sont **STABLES** (verifie par correlation de rang, cf. [[feedback_re_method]]).

198 paires, et le meilleur test de tailles du lot (mediane 1,000, 100 % > 0,6).
Croise avec le vecteur opcodes : **9/9, 0 divergence**.

**Total consolide : 812 paires, 201/784 adresses (25,6 %)** —
`docs/2026/merged_pairs.json`, `via: opcode+fenetre` = confirmee deux fois.

⚠ **La fenetre 275 (banque) tombe sur le `defjump` en 2026** alors qu'elle a un
vrai case en 2025 : peut-etre plus construite par la fabrique. A verifier avant
de porter ce qui en depend ([[reference_bank_zeny_re]]).

➡ Restent a exploiter, meme methode, paires deja reperees par (cases, taille) :
`Effect_ResolveResourceName` 0xAF0900 ↔ 0xACDA40 (2410/2398, low 13),
`Skill_ResolveCastMotion` 0xD84890 ↔ 0xC97C30 (409/409),
`EffectUpdate_DispatchByType` 0xBC4C70 ↔ 0xB8FD80 (491/491, low 0),
0xBB9260 ↔ 0xB84AB0 (1947/1923, low 491).

## ✅ Bilan du portage au 2026-08-27 : **205/784 (26,1 %)**

**ONZE tables** appariees, toutes indexees par un identifiant stable. Cinq sont
**rigoureusement identiques** des deux cotes (meme nombre de valeurs ET de
cibles) : `effectupd` (491, effect id), `sw478` (478, skill id), `skillcast`
(409, skill id), `weaponcombo` (315, item id), `sw289` (289, skill id).
Les autres : `dispatch` (opcode), `effectres`/`sw491`/`effectsnd` (effect id),
`makewindow`/`saverect` (id de fenetre).

**Union : 1181 paires, 0 conflit.** Livrables : `docs/2026/merged_pairs.json`
(champ `tables` = par quels identifiants), `opcode_dispatch_port.md`, scripts
dans `docs/2026/scripts/`.

⚠ **42 paires seulement** sont confirmees par des familles de tables
REELLEMENT independantes (cf. [[feedback_re_method]]) — pas les 756 annoncees
par un comptage naif. La confiance vient surtout du temoin aleatoire.

➡ **Prochaine piste** : la methode ne voit que ce qui est atteignable depuis un
gros switch. Restent hors d'atteinte l'audio, le rendu et les fichiers. Pour
elargir : chercher d'autres tables (`find_big_switches.py` en liste 137 en 2025
et 138 en 2026, seules 11 sont exploitees).

## Livrables du portage (docs/2026/)

| fichier | quoi |
|---|---|
| `merged_pairs.json` | **1181 paires**, champ `tables` = par quels identifiants |
| `apply_plan.md` | **le plan d'application : 277 sites, 70 fichiers** — rien n'est applique |
| `port_suspects.json` | les 11 entrees douteuses du portage precedent |
| `port_corrections.json` | **2 corrigees** (ratio 1,00) + **1 innocentee** |
| `monotonic_outliers.json` | les 8 paires qui cassent l'ordre (aucune utilisee par Bourgeon) |
| `scripts/` | tout est rejouable |

✅✅ **Les 3 listes d'objets sont MESUREES** (accesseurs triviaux, 14 sur 14
a `-0x38`) : storage `0x014B8A90`, cart `0x014B8A98` **confirmes** ; inventaire
`0x014B8A68` **tres probable** (4 octets sous le plus petit deplacement releve).
`Cart_GetCount` `0x00d5ce50` -> `0x00c6fea0`. Details :
`docs/2026/session_lists_confirmed.json`. ⚠ La tentative par PROFIL de
deplacements avait echoue — cf. [[feedback_re_method]], un deplacement ne porte
pas sa base.

## ✅ RTTI exploite a fond : 4327 methodes, mais **1 seule** adresse utile

`docs/2026/vtable_port.md`. 1594 classes en 2025 / 1332 en 2026, **964
communes**, 4327 methodes appariees par **(classe, slot)**.

🔴🔴 **LE test de validation, a reutiliser** : dans l'IDB 2026 les methodes
anonymes portent un nom auto **`Classe__vf_NN`** ou `NN` est **l'offset du slot
en hexa**. Mon index vient d'un parcours de vtable independant, sans lire ce nom.
Confrontation : **2164 paires, 2164 concordent, 0 divergence (100 %)** — et
uniquement sur le mode « prefixe » (nombre de slots different), c'est-a-dire
celui dont on doutait. Plus : tailles mediane 1,000 (temoin 0,171), monotonie
5,5 % (temoin 50 %), croisements 28/28 et 4/4.

⚠ **L'apport au portage est PRESQUE NUL : 1 adresse sur 4327 paires.** Raison
structurelle — **Bourgeon n'appelle pas les methodes virtuelles par adresse en
dur**, il passe par la vtable a l'execution. Ne pas recommencer ce calcul en
esperant du portage : le RTTI sert a LIRE, pas a porter.

✅ **Le vrai gain : 561 noms propages dans l'IDB 2026**, qui etait quasi anonyme
(24 fonctions nommees sur 4327, contre 440 cote 2025). Reversible :
`docs/2026/scripts/names_backup_2026.json` + `undo_names.py`. IDB sauvegarde.

⚠ Les « 615 noms differents » ne sont pas des erreurs : nos noms semantiques
face aux noms auto (`UIWindow_OnMsg` ↔ `UIWindow__vf_94`), plus le **COMDAT
folding** de la STL (plusieurs fonctions identiques a une seule adresse, IDA en
choisit une).

## 🔴 Ce qui RESTE : 518 adresses, et pourquoi c'est dur

| zone | portees | reste |
|---|---|---|
| `.text` (code) | 181 | **240** |
| `.data`/`.bss` (donnees) | 84 | **269** |
| autres | 1 | 9 |

101 des restantes sont des **litteraux** (adresses de patch, pas des symboles).
Fichiers les plus concernes : `game_settings.cc` (48), `status_tweaks.cc` (43),
`configuration.h` (42), `basic_info.cc` (30), `character_sheet.cc` (28).
Liste : `docs/2026/reste_a_porter.json`.

## ✅✅ Les GLOBALES par POSITION : **312/784 (39,8 %)**

`docs/2026/global_position_port.md`. Le vecteur qui atteint enfin les **zones de
DONNEES** (moitie de ce qui restait) : ni switch, ni RTTI, ni chaine n'y touche.

🔴 **Principe** : deux fonctions appariees referencent les memes globales
**DANS LE MEME ORDRE**. Meme longueur de sequence ⇒ la i-eme globale de l'une
correspond a la i-eme de l'autre.

⚠ **Ce vecteur n'est PAS utilisable en premier** : il faut d'abord beaucoup de
fonctions appariees. Il n'a donne que 909 globales parce que les passes
precedentes en avaient fourni **6297**. L'ordre des vecteurs compte.

**Gardes** : vote a 80 % mini ; une cible pour une seule source ; **longueurs
differentes IGNOREES** (1099 paires sacrifiees — une seule reference ajoutee
decale toute la sequence).

✅ **Validation enfin substantielle** — les vecteurs precedents ne se
recoupaient pas (4/623, puis 4/4327). Ici : **231 recouvrements avec les tables,
230 concordent** (99,6 %), 5/5 avec le portage initial, monotonie 5,8 % contre
49,8 %. L'unique divergence est un ecart de **2 octets** (champ 16 bits dans la
meme structure), pas une erreur.

**Bilan de TOUS les vecteurs : 6971 paires, 7 conflits, 312/784 (39,8 %)**,
474 sites dans 76 fichiers — `docs/2026/all_pairs_final.json` et `apply_plan.md`.

| vecteur | paires | apport manifeste |
|---|---|---|
| portage initial | 157 | — |
| identifiants stables (225 tables) | 1827 | +114 |
| RTTI (classe, slot) | 4327 | **+1** |
| **globales par position** | 909 | **+45** |
| accesseurs triviaux | 41 | 0 |

🔴 **Le rendement d'un vecteur en RE n'a RIEN a voir avec son rendement en
PORTAGE** : le RTTI donne 4327 paires pour 1 adresse utile, les globales par
position 909 pour 45. Choisir le vecteur d'apres ce que le code CIBLE utilise.

## ✅✅ BILAN 2026-08-27 : **360/784 (45,9 %)**, 8645 paires, 7 conflits

| vecteur | paires | apport manifeste |
|---|---|---|
| portage initial | 157 | — |
| identifiants stables (225 tables) | 1827 | **+114** |
| RTTI (classe, slot) | 4327 | **+1** |
| globales par POSITION | 1032 | **+45** |
| propagation par APPELS (2 tours) | 1577 | **+39** |
| accesseurs triviaux | 41 | 0 |

🔴 **L'ORDRE des vecteurs compte** : les deux derniers (globales par position,
propagation par appels) ont besoin d'une base large de fonctions deja appariees
et ne peuvent PAS venir en premier. Il a fallu 6971 paires pour les amorcer.

🔴 **La propagation par appels CONVERGE** : 1100 nouvelles paires au 1er tour,
477 au 2e. Un 3e tour ne rapporterait plus grand-chose. Reinjecter les fonctions
dans le vecteur des globales : 909 → 1032.

✅ **Le controle interne de la propagation** : une paire proposee ne doit pas
contredire une paire etablie. **7 puis 10 contradictions** sur des milliers de
votes — c'est ce taux qui dit qu'elle ne derive pas.

✅ `kMakeWindowAddr 0x00a39340 -> 0x00a07bc0` retrouve par propagation
EXACTEMENT l'appariement fait a la main pour la fabrique de fenetres.

🔴 **`0x005c5950 -> 0x00cca703` (`SkillMgr_SetOption`) du PORTAGE INITIAL est
condamnee par DEUX tests independants** (ratio de tailles 0xFA vs 0xE, et
monotonie) : tres probablement fausse. Cf. `port_suspects.json`.

Plan d'application : **530 sites dans 83 fichiers** (`apply_plan.md`).
Restent **424** adresses.

## 🔴🔴 PROTOCOLE : une LONGUEUR inchangee ne dit RIEN du TRAITEMENT

Le piege le plus couteux du chantier (2026-08-27).
`docs/2026/protocol_regressions.md`.

J'avais conclu « aucun opcode utilise par Moonlight ne change de longueur » :
**exact et trompeur**. `0x0AC4` declare la meme longueur, son `case` existe
toujours, il appelle bien la fonction de parsing — mais dedans les **cases 4 a 9
sont devenus un `break` nu**. Le client recoit, ne fait rien, n'affiche AUCUNE
erreur, et le serveur log « Authentication accepted ». **Symptome = liste VIDE,
pas un echec.**

➡ **Le detecteur** : mesurer pour chaque opcode le VOLUME DE CODE atteint depuis
son case (instructions + poids des fonctions appelees) dans les deux builds et
signaler les effondrements. `docs/2026/scripts/login_case_weight.py`,
`login_regress.py`, `game_regress.py`.

| dispatch | 2025 | 2026 |
|---|---|---|
| login/char | `LoginCharMode_RecvDispatch` `0x00D27560` | `sub_C3A670` `0x00C3A670` |
| jeu | `RecvLoop_DispatchPackets` `0x00C9DF00` | `0x005095A0` |

### ✅ Les 2 correctifs SERVEUR (branche `dev` de moonlight)

| paquet | 2025 | 2026 | commit |
|---|---|---|---|
| `AC_ACCEPT_LOGIN` | `0x0AC4`, entete 64, 160 o/srv | **`0x0069`**, entete 47, **32** o/srv | `6e1407d45` |
| `HC_ACCEPT_ENTER` | `(len-27)/175` | **`(len-7)/175`** | `e893c8103` |

Le 2e = le **`extension[20]`** que rAthena ajoute pour les clients recents. La
taille de `CHARACTER_INFO` est BONNE (175, recalculee champ par champ) : seul
l'**en-tete** etait en cause.

🔴 Les deux sont conditionnes a **`PACKETVER < 20260707`** ; `defines_pre.hpp`
passe a 20260707 sur `dev` uniquement (`8a4875160`). ⚠ **Cette ligne ne doit PAS
remonter vers la prod** (qui tourne en 20250716).

### ⚠ Restent, sans bloquer la connexion

`0x0071` s'effondre mais **`0x0AC5` est conservee** et c'est celle qu'envoie
rAthena a ce PACKETVER (156 octets, identique des deux cotes) — rien a faire.
Cote JEU : 23 opcodes effondres dont **4 declares par Moonlight** : `0x0101`,
`0x0229` (`ZC_STATE_CHANGE3`), `0x08FE`, `0x02D9`. **Candidats, pas verdicts** :
le poids est approximatif, a confirmer en lisant le calcul.
