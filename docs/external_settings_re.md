# RE — `ExternalSettings_kr.lub` : ce que le client en fait (et le service web « Assist »)

Client `20250716` (`Ragexe` 175220998), base `0x00400000`, **pas d'ASLR**
(adresses IDA == live). Fichier étudié :
`data\luafiles514\lua files\service_korea\ExternalSettings_kr.lub`
(présent en clair dans le client Moonlight-Destiny ; les clients officiels le
livrent en bytecode Lua 5.1 dans le GRF).

> ⚠ **Deux binaires distincts.** L'IDB (`2025-07-16_Ragexe_175220998_clientinfo.exe`)
> est le **Ragexe brut, non patché**. Le client déployé
> (`Moonlight-Destiny.exe`) est le **même binaire patché par WARP0716**
> (585 blocs, ~9,2 Ko modifiés). Voir §1.3 : **une seule** des fonctions
> décrites ici est patchée (4 octets dans `ExternalSettings_LoadKorea`) ; tout
> le reste — service web, emblèmes — est **byte-identique** entre les deux.

**Résumé en une phrase** : ce script sert à **deux choses sans rapport** — les
plafonds de niveau / auras (lus une fois au démarrage), et l'adresse du
**service web HTTP « Assist »** (lue à la demande), dont dépend entre autres
**tout le système d'emblèmes de guilde moderne**.

---

## 1. Quand et comment il est chargé

`Lua_LoadAllScriptFiles` `0x00d646c0` (≈ 150 `Lua_ExecuteScriptFile` à la
suite, exécuté une fois au boot) appelle en fin de liste :

```
0x00d650aa  call ExternalSettings_LoadKorea   ; ecx = objet global 0x015FA3C0
```

`ExternalSettings_LoadKorea` `0x00d74870` fait deux choses :

1. **choisit puis exécute** le bon script selon le service / le type de serveur ;
2. **relit** immédiatement les valeurs via les trois helpers Lua définis en bas
   du fichier (`GetTableIntValueForC`, `GetTableStringValueForC`,
   `GetTableBoolValueForC`) et les recopie dans des globales C.

### Résolution du chemin — `Lua_ExecuteScriptFile` `0x00a9bc90`

Appelée ici avec `(path, useDataRoot=1, noPrefix=0)` :

| Étape | Effet |
|-------|-------|
| préfixe | `LuaFiles514\` (car `noPrefix == 0`) |
| suffixe | `.lub` d'abord |
| repli | si absent, même chemin en `.lua` |
| recherche | `Res_FileExistsUnderDataRoot` `0x00573270` → racine `data\` (sur le client déployé le patch WARP `DataFolderFirst` fait gagner les fichiers en vrac de `data\` sur les GRF — c'est ce qui permet d'éditer le `.lub` en clair) |
| exécution | `luaL_loadbuffer` + `lua_pcall` dans l'état Lua global (`objet+0x59B8`) |

Chemin final effectif :
`data\LuaFiles514\Lua Files\service_korea\ExternalSettings_kr.lub`.

Le script **n'a pas de `return`** : il pose des globales dans `_G`, qui restent
disponibles pour tout le reste de la session — c'est ce qui permet de relire
`AssistAddr` bien plus tard.

### 1.1. Choix du fichier : `g_ServiceType` × `g_ServerType` (exe d'origine)

* `g_ServiceType` `0x0159B810` ← `<langtype>` de `clientinfo.xml`
  (`Apply_ClientInfoConnection` `0x00a72da0`).
* `g_ServerType` `0x0159B814` ← option de ligne de commande
  (`Config_ParseServiceAndServerType` `0x00a732b0`).

| `langtype` | dossier | variantes |
|---|---|---|
| 0 | `service_korea` | `_sak` (ServerType 1), `_sak_qm` (1 + `"QmSakray"` en cmdline), `_indoor` (2), `_qm` (0 + `"Dev"` en cmdline), sinon `ExternalSettings_kr` |
| 1 | `service_usa` | `_sak` |
| 2 | `service_japan` | `_sak` |
| 3 | `service_china` | `_sak` |
| 4 | `service_taiwan` | `_sak` |
| 5 | `service_thailand` | `_sak`, `_stage` (ServerType 13) |
| 6 | `service_indonesia` | `_sak` |
| 7 | `service_philippine` | `_sak` |
| 12 | `service_brazil` | `_sak` |
| 14 | `service_ru` | `_sak` |
| 15 | `service_vietnam` | `_sak` |
| 18 | `service_france` | `_sak` |
| 20 | `service_eu` | `_sak` |

> ⚠ **Piège (exe non patché uniquement)** : tout autre `langtype` (8-11, 13, 16,
> 17, 19, > 20) tombe dans le `default` du `switch` → **aucun script n'est
> exécuté**, mais les lectures qui suivent ont quand même lieu. Résultat : toutes
> les valeurs valent `-1` et le client crache
> `Debug_Print("Error ExternSettings Value")`. **Ce piège n'existe pas sur le
> client déployé**, voir §1.2.

### 1.2. Le patch WARP `LoadKrExtSettings` — le `switch` est court-circuité

Le client Moonlight applique le patch WARP **`LoadKrExtSettings`**
(« Always load korea ExternalSettings lua file »). Il écrit **4 octets** à
`0xd7489c`, là où se trouve la lecture de `g_ServiceType` :

```
avant   0xd7489c   a1 10 b8 59 01     mov  eax, ds:g_ServiceType
                   83 f8 14           cmp  eax, 14h
                   0f 87 ...          ja   default
                   ff 24 85 ...       jmp  ds:jpt_D748AA[eax*4]

après   0xd7489c   e9 10 00 00 00     jmp  0xd748b1      ; = case 0 (korea)
```

`0xd748b1` est exactement l'entrée `case 0` de la table de saut. Conséquences
sur le client déployé :

* le `<langtype>` de `clientinfo.xml` **n'a plus aucun effet** sur le choix du
  fichier : c'est **toujours** `Lua Files\service_korea\ExternalSettings_*` ;
* le sous-choix `_sak` / `_sak_qm` / `_indoor` / `_qm` par `g_ServerType` +
  ligne de commande **reste actif** (le code à partir de `0xd748b1` est intact).
  Avec `ServerType 0` et une ligne de commande sans `"Dev"`, on obtient bien
  `ExternalSettings_kr` ;
* la table `langtype` du §1.1 ne sert donc plus qu'à lire l'exe d'origine.

Le reste de `ExternalSettings_LoadKorea` (toutes les lectures de tables) est
**inchangé**.

### 1.3. Périmètre du patch WARP vis-à-vis de ce document

Diff `Ragexe` brut ↔ `Moonlight-Destiny.exe` sur les 31 fonctions citées ici :

| Fonction | État |
|---|---|
| `ExternalSettings_LoadKorea` `0x00d74870` | **4 octets** (`LoadKrExtSettings`, §1.2) |
| `Lua_LoadAllScriptFiles` `0x00d646c0` | **380 octets**, mais uniquement les hooks `Custom*Lub` / `AddLuaOverrides` qui détournent d'autres scripts vers `SystemEN\`. Le `call ExternalSettings_LoadKorea` à `0x00d650aa` est **intact**. |
| Toutes les autres (`GetAssistAddr`, les 6 `*_BuildWebUrls`, toute la chaîne emblème, `Lua_ExecuteScriptFile`, `Apply_ClientInfoConnection`, `Job_GetMax*Level`, `UIMakeCharWnd_OnMsg`, …) | **byte-identiques** |

Autrement dit : **tout ce qui suit (§2 à §6) s'applique tel quel au client
déployé**, à la seule exception de la sélection par `langtype`.

---

## 2. Ce que le client lit VRAIMENT dans ce fichier

`ExternalSettings_LoadKorea` ne lit que **4 tables**. Tout est recopié dans des
globales contiguës à partir de `0x01602288` (= objet global `0x015FA3C0` + `0x7EC8`).

### `MaxLevelTable` (via `GetTableIntValueForC`, `ss>d`)

| Clé | Globale | Adresse |
|---|---|---|
| `BaseLevel` | `g_ES_MaxBaseLevel` | `0x01602288` |
| `BaseLevel3rd` | `g_ES_MaxBaseLevel3rd` | `0x0160228C` |
| `BaseLevelExtend2` | `g_ES_MaxBaseLevelExtend2` | `0x01602290` |
| `BaseLevelUpperJob` | `g_ES_MaxBaseLevelUpperJob` | `0x01602294` |
| `BaseLevelHomun` | `g_ES_MaxBaseLevelHomun` | `0x01602298` |
| `JobLevelNovice` | `g_ES_MaxJobLevelNovice` | `0x0160229C` |
| `JobLevelSuperNovice` | `g_ES_MaxJobLevelSuperNovice` | `0x016022A0` |
| `JobLevelBase` | `g_ES_MaxJobLevelBase` | `0x016022A4` |
| `JobLevel2nd` | `g_ES_MaxJobLevel2nd` | `0x016022A8` |
| `JobLevel3rd` | `g_ES_MaxJobLevel3rd` | `0x016022AC` |
| `JobLevelExtend2` | `g_ES_MaxJobLevelExtend2` | `0x016022B0` |
| `JobLevelUpperJob` | `g_ES_MaxJobLevelUpperJob` | `0x016022B4` |
| `BaseLevelDoram` | `g_ES_MaxBaseLevelDoram` | `0x016022B8` |
| `JobLevelDoram` | `g_ES_MaxJobLevelDoram` | `0x016022BC` |
| `BaseLevel4th` | `g_ES_MaxBaseLevel4th` | `0x01602304` |
| `JobLevel4th` | `g_ES_MaxJobLevel4th` | `0x01602308` |

Trois contrôles distincts : si l'un des 12 premiers vaut `-1`, ou si
`BaseLevel4th`/`JobLevel4th` valent `-1`, ou si `JobLevelDoram` vaut `-1`,
le client logge `Error ExternSettings Value` (et continue avec `-1`).

**Consommateurs** :
`Job_GetMaxBaseLevel` `0x00d99ca0` / `Job_GetMaxJobLevel` `0x00d99d30`
(dispatch par classe de job), `UIBasicInfoWnd_DrawContent` `0x0095e620`
(pourcentage d'EXP), `CActorSprite_ApplyLevelJobAura` `0x00c41950`,
`Actor_OnMsg_AppearanceEffects` `0x00c4aea0`.

> Conséquence concrète : ces valeurs sont **purement cosmétiques côté client**
> (barre d'EXP, aura). Elles ne bornent rien côté serveur, mais si elles ne
> correspondent pas à `conf/battle/exp.conf`, la barre d'EXP et l'aura de niveau
> max seront fausses.

### `MakeableRace`

| Clé | Globale | Adresse | Consommateur |
|---|---|---|---|
| `Doram` (bool, `ss>b`) | `g_ES_MakeableRace_Doram` | `0x01602314` | `UIMakeCharWnd_OnCreate_0` `0x007a0180`, `UIMakeCharWnd_OnMsg` `0x007a1470` |

C'est le **seul** interrupteur qui active/désactive l'onglet Doram à la
création de personnage. Moonlight le laisse à `false`.

### `Level99AuraTable` → `0x016022C0` … `0x016022E0`

`Default99LvAura`, `Default99LvAura_sub`, `Baby99LvAura`, `Baby99LvAura_sub`,
`SecondHigh99LvAura`, `SecondHigh99LvAura_sub`, `Homun99LvAura`,
`Homun99LvAura_sub`, `Boss99LvAura_sub` (dans cet ordre).

### `MaxLevelAuraTable` → `0x016022E4` … `0x01602300` + `0x0160230C`/`0x01602310`

`Default150LvAura`, `_sub`, `Default160LvAura`, `_sub`, `HomunMaxLvAura`,
`_sub`, `Default185LvAura`, `_sub`, puis `MaxLevelEffect4th` (`0x0160230C`) et
`MaxLevelEffect4th_sub` (`0x01602310`).

Les valeurs sont des **IDs d'effet** (`EF_*`) consommés par
`Actor_OnMsg_AppearanceEffects` et `ActorAiClass_ClearCommonEffects`
`0x00c41fa0`. Voir `docs/effect_system_re.md`.

---

## 3. Ce que le client NE lit PAS dans ce fichier

Le fichier Moonlight définit aussi :

```lua
Url = { TwitterUrl = ... }
AccountLinkedUserDataUrl = { Save = ..., Load = ... }
TwitterDataUrl = { Auth = ..., Upload = ... }
EmblemDataUrl  = { Upload = ..., Download = ... }
```

**Aucune de ces quatre tables n'est lue par le client `20250716`.** Les chaînes
`"EmblemDataUrl"`, `"TwitterDataUrl"`, `"AccountLinkedUserDataUrl"`, `"Url"`,
`"TwitterUrl"` **n'existent pas** dans le binaire (seules `"AssistAddr"` et
`"AssistAddrTbl"` y sont). Ce sont des vestiges des clients ~2017-2018 ; le
client moderne **reconstruit les URLs lui-même** à partir de `AssistAddr` en y
concaténant des chemins **codés en dur**.

Autrement dit : **modifier `EmblemDataUrl` ne sert à rien**, seul `AssistAddr`
(ou `AssistAddrTbl`) compte.

---

## 4. `AssistAddr` : résolution de l'adresse du service web

`ExternalSettings_GetAssistAddr` `0x00d59060` — appelée **à la demande**, bien
après le boot :

```
1. Lua_GetGlobalString(L, "AssistAddr")            ; _G.AssistAddr  (0x00a9b140)
   -> si non vide : renvoyée telle quelle. FIN.

2. sinon : clé = <domain> de clientinfo.xml  (g_ClientInfo_Domain 0x01212CEC)
           sinon "<address>:<port>"          (0x01212CE4 / 0x01212CE8)
   Lua : GetTableStringValueForC("AssistAddrTbl", clé)      ; "ss>s"

3. échec / vide -> std::string vide.
```

Points importants :

* **Aucun schéma n'est ajouté.** `AssistAddr = "192.168.1.13:8888"` produit
  l'URL `192.168.1.13:8888/emblem/download` ; libcurl suppose alors `http://`.
  Mettre `"http://host:port"` fonctionne aussi (c'est ce que faisait
  `EmblemDataUrl` à l'époque).
* Les valeurs de `clientinfo.xml` viennent de `Apply_ClientInfoConnection`
  `0x00a72da0` : `<address>`, `<port>`, `<domain>`, `<langtype>`,
  `<registrationweb>`, `<version>`, `<taaddress>`, `<aid>/<admin>`, `<yellow>`.
* Si `AssistAddr` est vide **et** que `AssistAddrTbl` n'existe pas, la fonction
  renvoie `""` et **tous les appelants sautent l'initialisation de leurs URLs**
  (voir §5) — le sous-système concerné devient inerte, sans message d'erreur.

### Les 6 appelants (toutes les URLs du client)

| Fonction | Endpoints construits (`AssistAddr` + chemin **codé en dur**) |
|---|---|
| `AccountLinkedUserData_BuildWebUrls` `0x0059ec70` | `/userconfig/save`, `/userconfig/load` |
| `CharacterLinkedUserData_BuildWebUrls` `0x005c5260` | `/charconfig/save`, `/charconfig/load` |
| `AdventurerAgency_BuildWebUrls` `0x005ac1a0` | `/party/add`, `/list`, `/search`, `/del`, `/get`, `/change`, `/info` |
| `MerchantStoreBackup_BuildWebUrls` `0x005e3c80` | `/MerchantStore/save`, `/MerchantStore/load` |
| `TwitterData_BuildWebUrls` `0x005ecc10` | `/twitter/upload`, `/twitter/user-auth` |
| **`CEmblemDataMgr_BuildWebUrls` `0x005c8ac0`** | **`/emblem/upload`, `/emblem/download`** |

Le socle HTTP est le wrapper libcurl multipart POST déjà documenté
(`HttpMultipartPost_ctor` `0x005c71c0`, `g_CurlApiTable` `0x012517B4`).

> ⚠ `CEmblemDataMgr_BuildWebUrls` est appelée **une seule fois**, depuis
> `CLoginMode_OnStateEnter` `0x00d24080`. Si `AssistAddr` est vide à cet
> instant, les deux URLs restent vides **pour toute la session** : aucune
> nouvelle tentative n'est faite plus tard.

---

## 5. Focus emblèmes de guilde

### 5.1 Le cache disque `_tmpEmblem\`

`CGuildEmblemMgr_BuildEbmPath` `0x0061dc20` :

```
nom = <g_WorldName> "_" <guildId> "_" <version + 0x7FFFFFFF> ".ebm"
```

* `g_WorldName` `0x015FAB00` = nom du monde (ex. `Moonlight-Destiny`).
* **Le biais `+0x7FFFFFFF` est réel** : c'est pourquoi les fichiers sur disque
  s'appellent `Moonlight-Destiny_490_2147483649.ebm` — version **2**, pas
  2147483649. (`2147483649 - 2147483647 = 2`.)
* Le dossier `..\_tmpEmblem\` est ajouté par la res-factory
  `EmblemImage_RegisterResFactory` `0x00445870`, donc `BuildEbmPath` ne rend que
  le nom de fichier.
* Contenu du `.ebm` = **zlib(BMP)**, décompressé par
  `GuildEmblem_LoadTextureOrRequest` `0x00714e20` qui charge ensuite le BMP sous
  le nom logique `_emblem.bmp`.

### 5.2 Chemin de lecture (affichage)

`CGuildEmblemMgr_GetEmblemPath` `0x0061d370` :

1. `version <= 0` → chaîne vide, **pas d'emblème** (rien n'est demandé).
2. sinon → `BuildEbmPath` ;
3. si `UITextureMgr` n'arrive pas à charger ce fichier **et** qu'on n'est pas en
   replay → `GuildEmblemReqCache_Insert` `0x0061b9b0` (mise en file).

Chaque frame, `GameMode_InGame_ProcessFrame` `0x00c74a80` appelle
`CGuildEmblemMgr_PruneReqCache` `0x0061dd60` → `GuildEmblemReqCache_Prune`
`0x0061bab0` : purge les requêtes en vol de plus de **60 s**, dépile une demande
et lance `CEmblemDataMgr_RequestDownload` `0x005c87a0`.

`CEmblemDataDownloadAsyncWork_DoWork` `0x005c7ec0` — POST multipart vers
`<AssistAddr>/emblem/download` :

| Champ | Source |
|---|---|
| `AID` | `g_Account_Aid` `0x015FB9A4` |
| `AuthToken` | membre du work (`+0x44`) |
| `WorldName` | `g_WorldName`, encodé UTF-8 (`LocalCPToUtf8` `0x005c6ba0`) |
| `GDID` | guildId |
| `Version` | version d'emblème |

Réponse (le BMP) → `work+124`, code HTTP → `work+148`.
Sur succès + HTTP 200, `CEmblemDataDownloadAsyncWork_OnComplete` `0x005c7df0`
relaie l'image au map-server en morceaux via **CZ `0x0B36`**
(`GuildEmblem_SendImageChunks_0B36` `0x005c8c00`, morceaux de 1010 octets).

**C'est le SEUL chemin web de récupération d'emblème.** Il n'y a pas de repli
automatique : si `AssistAddr` est vide/injoignable, `GetEmblemPath` re-empile la
demande, `Prune` la relance, l'HTTP échoue, et l'emblème n'apparaît jamais.

### 5.3 Chemin d'écriture (le serveur pousse l'image)

Deux paquets alimentent le cache `.ebm` **sans passer par le web** :

| Paquet | Handler | Nom de fichier écrit |
|---|---|---|
| **ZC `0x0152`** `ZC_GUILD_EMBLEM_IMG` — `<len>.W <GDID>.L <emblemId>.L <img>` | `GuildNet_OnEmblemImg` `0x00d76b10` | `<World>_<gid>_<emblemId>.ebm` — **version BRUTE, sans le biais `+0x7FFFFFFF`** |
| **ZC `0x0B36`** flux découpé — en-tête 14 o : `<len>.W <type>.W <GDID>.L <version>.L <data>` ; `type` 0 = début, 1 = morceau, 2 = fin | `GuildNet_EmblemDownloadStream` `0x00cec810` → `CGuildEmblemMgr_SaveEbmFramesFromBmp` `0x0061d7b0` | `BuildEbmPath` → **avec** le biais `+0x7FFFFFFF` |

> 🔴 **Incohérence de nommage à connaître** : le chemin `0x0152` écrit
> `<gid>_<emblemId>.ebm` alors que le lecteur (`GetEmblemPath` → `BuildEbmPath`)
> cherche `<gid>_<emblemId + 0x7FFFFFFF>.ebm`. Les deux ne se rejoignent que si
> le serveur envoie un `emblemId` déjà biaisé. Les fichiers présents dans
> `_tmpEmblem\` de Moonlight sont **tous** au format biaisé → ils ont été écrits
> par le chemin `0x0B36`, pas par `0x0152`.

`SaveEbmFramesFromBmp` supprime d'abord les anciens `<World>_<gid>_*.ebm`,
zlib-compresse le BMP reçu, écrit le nouveau `.ebm` puis
`CGuildEmblemMgr_AddEmblemFile` `0x0061ced0`.

### 5.4 Changement d'emblème (upload)

1. `UIGuildTotalInfoWnd_OnMsg` `0x00927700` construit `"emblem\<fichier>"` et
   appelle `CEmblemDataMgr_RequestUpload` `0x005c8950` ; `false` → MessageBox.
2. `CEmblemDataUploadAsyncWork_DoWork` `0x005c9960` — POST multipart vers
   `<AssistAddr>/emblem/upload`, champs `AID`, `AuthToken`, `WorldName`,
   `GDID`, `ImgType`, `IMG`.
3. `GuildEmblem_OnUploadResponse_Send0B46` `0x005c8ed0` parse le **JSON** de
   réponse :
   * `Type == 1` → envoie **CZ `0x0B46`** `{op.W, guildId.L, version.L}` au
     map-server pour publier la nouvelle version ;
   * `Type` 2/3/4 → MessageBox `MSG 0xE02` (échec).
4. Le serveur rediffuse la version (**ZC `0x0B47`**,
   `GuildNet_OnUpdateGdIdEmblem` `0x00cfa6a0`, ou les paquets d'info de guilde)
   → `CGuildEmblemMgr_SetEmblemVersion` `0x0061ddc0` sur tous les clients.

### 5.5 Ce que ça implique pour un serveur rAthena / Moonlight

* rAthena n'implémente **ni `0x0B36` ni `0x0B46`/`0x0B47`** et n'a pas de service
  web « Assist ». Le seul chemin qu'il connaît est `ZC_GUILD_EMBLEM_IMG 0x0152`.
* Il faut donc, **soit** :
  * héberger un service HTTP minimal exposant `/emblem/upload` et
    `/emblem/download` (multipart, réponse JSON `{"Type":1,"version":N}` pour
    l'upload, corps binaire BMP pour le download) et pointer `AssistAddr`
    dessus ; **soit**
  * rester sur `0x0152` — mais alors il faut tenir compte du biais
    `+0x7FFFFFFF` sur la version (§5.3) pour que le fichier écrit soit celui que
    le client relit.
* Les autres endpoints (`/userconfig`, `/charconfig`, `/party/*`,
  `/MerchantStore/*`, `/twitter/*`) sont inertes tant qu'`AssistAddr` n'est pas
  servi.

> ✅ **SUR MOONLIGHT, ILS NE LE SONT PAS** (vérifié le 2026-08-15). L'adresse
> vient du **Lua**, pas de `clientinfo.xml` — qui n'a aucun `<taaddress>` :
> `data\luafiles514\lua files\service_korea\ExternalSettings_kr.lub` porte
> `AssistAddr = "192.168.1.13:8888"`. Un `web-server` rAthena y écoute
> (`~/moonlight/src/web/`), avec ses contrôleurs compilés : `userconfig`,
> `charconfig`, `emblem`, `merchantstore`, `partybooking`.
> ⇒ La sauvegarde serveur des raccourcis et des positions de fenêtres **par
> compte** fonctionne, et le client la fait tout seul à la sortie propre. Le
> paragraphe qui précède décrit le cas d'un serveur qui ne sert pas l'adresse —
> ce n'est pas le nôtre.

---

## 6. Table d'adresses (client `20250716`)

### Chargement / lecture du script

| Adresse | Symbole |
|---|---|
| `0x00d646c0` | `Lua_LoadAllScriptFiles` |
| `0x00d74870` | `ExternalSettings_LoadKorea` |
| `0x00a9bc90` | `Lua_ExecuteScriptFile(path, useDataRoot, noPrefix)` |
| `0x00a9a7d0` | `Lua_CallGlobal_va` (format `"ss>d"` / `"ss>b"` / `"ss>s"`) |
| `0x00a9b140` | `Lua_GetGlobalString` |
| `0x00d59060` | `ExternalSettings_GetAssistAddr` |
| `0x0059ce30` | `StdString_ConcatCStr` |
| `0x0159B810` / `0x0159B814` | `g_ServiceType` / `g_ServerType` |
| `0x01212CE4` / `0x01212CE8` / `0x01212CEC` | `g_ClientInfo_Address` / `_Port` / `_Domain` |
| `0x015FA3C0` | objet global (Lua state à `+0x59B8`, valeurs ES à `+0x7EC8`) |
| `0x01602288`…`0x01602314` | `g_ES_*` (cf. §2) |

### Service web

| Adresse | Symbole |
|---|---|
| `0x005c8ac0` | `CEmblemDataMgr_BuildWebUrls` |
| `0x0059ec70` | `AccountLinkedUserData_BuildWebUrls` |
| `0x005c5260` | `CharacterLinkedUserData_BuildWebUrls` |
| `0x005ac1a0` | `AdventurerAgency_BuildWebUrls` |
| `0x005e3c80` | `MerchantStoreBackup_BuildWebUrls` |
| `0x005ecc10` | `TwitterData_BuildWebUrls` |
| `0x005c71c0` / `0x005c7520` / `0x005c75e0` | `HttpMultipartPost_ctor` / `_AddField` / `_Perform` |
| `0x012517B4` | `g_CurlApiTable` |

### Emblèmes

| Adresse | Symbole |
|---|---|
| `0x012517B8` | `g_CEmblemDataMgr` |
| `0x01254D70` | `g_CGuildMgr` |
| `0x015FAB00` | `g_WorldName` |
| `0x005c87a0` / `0x005c8950` | `CEmblemDataMgr_RequestDownload` / `_RequestUpload` |
| `0x005c7ec0` / `0x005c9960` | `CEmblemDataDownloadAsyncWork_DoWork` / `CEmblemDataUploadAsyncWork_DoWork` |
| `0x005c7df0` / `0x005c98a0` | `..._OnComplete` (download / upload) |
| `0x005c8c00` | `GuildEmblem_SendImageChunks_0B36` |
| `0x005c8ed0` | `GuildEmblem_OnUploadResponse_Send0B46` |
| `0x0061dc20` | `CGuildEmblemMgr_BuildEbmPath` (**biais `+0x7FFFFFFF`**) |
| `0x0061d370` | `CGuildEmblemMgr_GetEmblemPath` |
| `0x0061d610` | `CGuildEmblemMgr_HasEmblemFile` |
| `0x0061ddc0` | `CGuildEmblemMgr_SetEmblemVersion` |
| `0x0061bab0` / `0x0061dd60` | `GuildEmblemReqCache_Prune` / `CGuildEmblemMgr_PruneReqCache` |
| `0x0061d7b0` | `CGuildEmblemMgr_SaveEbmFramesFromBmp` |
| `0x00714e20` | `GuildEmblem_LoadTextureOrRequest` |
| `0x00445870` | `EmblemImage_RegisterResFactory` (préfixe `..\_tmpEmblem\`) |
| `0x00d76b10` | `GuildNet_OnEmblemImg` (ZC `0x0152`) |
| `0x00cec810` | `GuildNet_EmblemDownloadStream` (ZC `0x0B36`) |
| `0x00cfa6a0` | `GuildNet_OnUpdateGdIdEmblem` (ZC `0x0B47`) |

---

## 7. Annexe — comparaison KRO officiel / Moonlight

Le `ExternalSettings_tw.lub` officiel (extrait du bytecode) ne contient **que**
`MaxLevelTable`, `MakeableRace`, `AssistAddr`, `LEVELAURA`, `Level99AuraTable`,
`MaxLevelAuraTable` et les trois helpers — **pas** de `Url` / `EmblemDataUrl` /
`TwitterDataUrl` / `AccountLinkedUserDataUrl`, ce qui confirme que ces tables
sont mortes. `AssistAddr` y vaut `twro-assist.gnjoy.com.tw:3000`.

La variante `_sak` (serveur de test) est le seul fichier qui porte encore les
anciennes tables `Url` / `TwitterDataUrl` / `AccountLinkedUserDataUrl` — c'est
de là que vient le copier-coller présent dans le fichier Moonlight.
