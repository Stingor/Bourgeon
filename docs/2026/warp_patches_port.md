# Porter les patchs WARP sur le client 2026-07-07

Relevé du 2026-08-26. Périmètre : **les patchs réellement activés**, lus dans
la section `patches:` de `E:\Nouveau dossier\save_exe.yml` — pas le catalogue complet.

## 🔴 Pourquoi c'est faisable

Les `.qjs` **ne contiennent pas d'adresses de destination** : ils localisent leur
cible par `Exe.FindText(...)` (une chaîne) et `Exe.FindHex(...)` (un motif
d'instructions), puis patchent relativement. C'est ce qui rend WARP portable
d'un client à l'autre — et un échec lève `throw Error("Could not find ...")`.

➡ **Le test qui chiffre tout, en une passe : charger `D:\KRO\Ragexe.exe` dans
WARP, cocher ces 116 patchs, et relever ceux qui échouent.** Inutile de les
auditer un par un avant : le classement ci-dessous ne sert qu'à savoir *par où*
commencer si beaucoup échouent.

⚠⚠ **Ne PAS conclure « tout est passé » depuis l'absence de message.** Un patch
non sélectionné, ou dont le `validate()` rend faux sans `Cancel("raison")`, est
muet. Comparer la liste appliquée à `warp_active.txt`, patch par patch.
Cf. `docs/warp_patches.md` et sa section « pièges ».

## 🔴 Le sens du transfert : WARP0219 → WARP0716, jamais l'inverse

Deux catalogues sur le disque (relevé 2026-08-26) :

| dépôt | patchs | dernière version de client citée |
|---|---|---|
| `WARP0716` — **le tien** (`github.com/Stingor/WARP0716`) | 173 | `20250716` |
| `WARP0219` (`Demonbytes-lab/WARP0219`) | 174 | **`20260210`** ← seul à viser 2026 |

Sur tes **116 patchs actifs**, comparaison fichier à fichier :

| | n |
|---|---|
| identiques à WARP0219 | 26 |
| **modifiés par toi** | **22** |
| exclusifs à WARP0716 | 5 |
| tes créations (2 sur `main`, 4 sur branches) | 6 |
| dans un `.qjs` partagé (non comparables ainsi) | 62 |

➡ **~33 patchs n'existent que chez toi.** Remplacer ton catalogue par WARP0219
les détruirait. Le bon geste est l'inverse : **garder WARP0716 comme base et n'y
rapatrier une version WARP0219 que pour les patchs qui échouent sur le 2026.**

Les 22 que tu as modifiés — `DeleteCharWithEmail`, `TranslateClient`,
`CallKoreaClientInfo`, `DataFolderFirst`, `SendClientFlags`, `GuildBrackets`,
`FixChatAt`, `PreviewInShop`/`InTrader`… — portent des adaptations Moonlight.
Toute reprise depuis WARP0219 doit être un **merge**, pas un écrasement.

## ⚠⚠ AVANT tout portage : 4 patchs actifs absents de `main`

`RestoreGMWeaponTrail`, `DoramHeadPalShared`, `NoLookalikeNameWarning`,
`PetTalkMarker` sont **activés dans `save_exe.yml`** mais **absents du working
tree et de `main`**. Ils vivent sur des branches non fusionnées
(`patches/pet-talk-marker`, `patches/no-lookalike-name-warning`,
`doram-palette-tweak`) et l'un porte un `Revert` sur `main`.

🔴 Ils ne s'appliquent donc **probablement pas aujourd'hui**, sur le client
actuel — c'est le piège documenté dans [[reference_warp0716]] : un patch
introuvable échoue sans bruit. À vérifier et à fusionner **avant** d'aborder le
2026, sinon le portage se fera sur un catalogue amputé.

## 🔴🔴 L'inconnue à lever EN PREMIER : EOS AntiCheat

Tu actives `NoGGuard`, qui traite **nProtect GameGuard**. Le build 2026 embarque
en plus **EOS AntiCheat** (Epic Online Services) : `CEOSAntiCheatMgr::CreateInstance()`,
`_EOS_AntiCheatClient_BeginSession@8`, aux côtés de `EasyAntiCheat.exe` et du
lanceur `Ragnarok.exe`.

**Aucun patch du catalogue ne le connaît** — mesuré : les 172 `.qjs` ne
mentionnent ni `EOS`, ni `EasyAntiCheat`, ni `EOS_Platform`. Le catalogue n'a que
`NoGGuard` (nProtect) et `SkipCheatCheck`.

Deux issues possibles, et il faut savoir laquelle **avant** de planifier le reste :
1. sans credentials Epic configurés, l'init EOS échoue proprement et le client
   continue — rien à faire ;
2. elle bloque le démarrage hors kRO — il faut alors écrire un patch neuf.

➡ C'est le **premier** test à faire : le client patché démarre-t-il ? Tant que la
réponse est non, ni le scan mémoire de `CSession` ni le portage des adresses
Bourgeon ne peuvent avancer — les deux exigent un client qui tourne.

## 🔴🔴 La cause n°1 des échecs : le DÉPLACEMENT DE SAUT en dur

Mesuré sur `RestoreGMWeaponTrail` (2026-08-26), et la leçon vaut pour tous :

- Le `head` du motif — `E8 ?? ?? ?? ?? 83 C4 04 84 C0` (`call` / `add esp,4` /
  `test al,al`) — se retrouve sur **les 25 sites** du build 2026. ✅
- Ce qui casse, c'est la suite : le patch épingle `74 1D` et `74 50`. **Aucun
  site 2026 ne porte ces valeurs**, et beaucoup sont passés du saut COURT au
  saut LONG :

```
0x004c76e3   84 c0  0f 85 e2 00 00 00    <- jnz long
0x004c8f7a   84 c0  75 74                <- jnz court, autre delta
0x00c44b7c   84 c0  74 0e                <- jz  court, autre delta
```

➡ **Un `74 xx` / `75 xx` écrit en dur dans un motif ne survit pas à un
changement de build** : dès que le bloc sauté grossit au-delà de 127 octets, le
compilateur émet `0F 84`/`0F 85` sur 6 octets. Le motif ne matche plus, et le
patch échoue — bruyamment, heureusement.

**Comment réparer, en général :** couper le motif AVANT le saut (le `head` seul
suffit souvent à localiser), puis lire le saut à l'exécution au lieu de le
coder. Et quand il faut discriminer plusieurs sites, s'ancrer sur ce qui ne
bouge pas — l'offset de structure poussé (`[reg+110h]`), l'appel suivant, le
vecteur écrit (`[reg+4ACh]`) — jamais sur la longueur d'un saut.

⚠ Les offsets de structure eux-mêmes peuvent avoir bougé : vérifier
`[reg+4ACh]` / `[reg+260h]` sur le 2026 avant de s'y fier.

### Ancre déjà résolue pour ce patch (mis de côté : cosmétique)

`IsGidInGMList` : `0x00A395F0` dans le build 2026 (2 occurrences du motif :
`0x00A395F0` et `0x00A39630`), **25 appelants**. Le `lookup` du patch le trouve
déjà sans modification — seules les `shapes` sont à revoir.

## Récapitulatif

| classe | nombre | ce que ça veut dire |
|---|---|---|
| ✅ Sûr | **10** | localisé par `Exe.FindText` seul — une chaîne traverse les builds |
| 🟡 Moyen | **26** | ancre texte **et** motif d'instructions : l'ancre tient, le motif est à vérifier |
| 🟠 À vérifier | **54** | uniquement `Exe.FindHex` : le motif d'instructions peut ne plus exister |
| 🔴 Prioritaire | **21** | contient des littéraux d'adresse — à relire ligne à ligne |
| ❔ À inspecter | **1** | ni texte ni motif détecté |
| ❌ Script non localisé | **4** | le `.qjs` n'a pas été retrouvé automatiquement |

⚠ **Réserve de méthode** : quand plusieurs patchs partagent un `.qjs`
(`SharedPal.qjs`, `CustomPath.qjs`, `EnableSlashCmd.qjs`…), les compteurs
portent sur **tout le fichier**, pas sur la fonction du patch : le risque de
ceux-là est **surestimé**. Et le critère « adresse en dur » compte les littéraux
`0x00xxxxxx`, dont certains sont des constantes ordinaires. Ce tableau oriente,
il ne remplace pas la lecture du script.

## ✅ Sûr (10)

Localisé par `Exe.FindText` seul — une chaîne traverse les builds.

| patch | script | text | hex | adr. en dur |
|---|---|---|---|---|
| `CustomFontCharset` | `CustomFont.qjs` | 1 | 0 | 0 |
| `GuildBrackets` | `GuildBrackets.qjs` | 1 | 0 | 0 |
| `HideBuildInfo` | `HideBuildInfo.qjs` | 1 | 0 | 0 |
| `NoBardFrostJoke` | `NoSkillChatter.qjs` | 2 | 0 | 0 |
| `NoCameraLock` | `NoCameraLock.qjs` | 1 | 0 | 0 |
| `NoDancerScream` | `NoSkillChatter.qjs` | 2 | 0 | 0 |
| `NoGravityAds` | `NoGravityImages.qjs` | 1 | 0 | 0 |
| `NoGravityLogo` | `NoGravityImages.qjs` | 1 | 0 | 0 |
| `NoNagle` | `NoNagle.qjs` | 1 | 0 | 0 |
| `NoSwearFilter` | `NoSwearFilter.qjs` | 1 | 0 | 0 |

## 🟡 Moyen (26)

Ancre texte **et** motif d'instructions : l'ancre tient, le motif est à vérifier.

| patch | script | text | hex | adr. en dur |
|---|---|---|---|---|
| `AddLuaOverrides` | `AddLuaOverrides.qjs` | 1 | 8 | 0 |
| `BodyPalUnisex` | `SharedPal.qjs` | 3 | 1 | 0 |
| `BorderlessFSW` | `WindowFrame.qjs` | 3 | 1 | 0 |
| `CallKoreaClientInfo` | `CallKoreaClientInfo.qjs` | 1 | 13 | 0 |
| `CustomWinTitle` | `WindowFrame.qjs` | 3 | 1 | 0 |
| `DataFolderFirst` | `DataFolderFirst.qjs` | 1 | 6 | 0 |
| `DisConnToLogin` | `ReturnToLogin.qjs` | 2 | 23 | 0 |
| `DisableDoram` | `DisableDoram.qjs` | 4 | 2 | 0 |
| `DisableTraitStatusButton` | `TraitStatusButton.qjs` | 5 | 16 | 0 |
| `EnableSysMenu` | `WindowFrame.qjs` | 3 | 1 | 0 |
| `FixLatestNCWin` | `FixLatestNCWin.qjs` | 1 | 2 | 0 |
| `HeadPalUnisex` | `SharedPal.qjs` | 3 | 1 | 0 |
| `HideNewButtons` | `NewButtonVisibility.qjs` | 1 | 13 | 0 |
| `HideTraitStatusButton` | `TraitStatusButton.qjs` | 5 | 16 | 0 |
| `IncrViewID` | `IncrViewID.qjs` | 1 | 2 | 0 |
| `LoadKrExtSettings` | `LoadKrExtSettings.qjs` | 2 | 2 | 0 |
| `No1and1Arg` | `ClientArgs.qjs` | 2 | 5 | 0 |
| `NoEquipWinTitle` | `CustomEquipWin.qjs` | 2 | 8 | 0 |
| `NoGGuard` | `DisableProtect.qjs` | 3 | 12 | 0 |
| `RestoreBattlegroundUI` | `RestoreBattlegroundUI.qjs` | 2 | 9 | 0 |
| `SendClientFlags` | `SendClientFlags.qjs` | 1 | 3 | 0 |
| `ShowNewButtons` | `NewButtonVisibility.qjs` | 1 | 13 | 0 |
| `TildeForMatk` | `TildeForMatk.qjs` | 2 | 1 | 0 |
| `TranslateClient` | `TranslateClient.qjs` | 1 | 2 | 0 |
| `UseOldLogin` | `LoginMode.qjs` | 1 | 15 | 0 |
| `ZeroCinShop` | `CashShop.qjs` | 2 | 5 | 0 |

## 🟠 À vérifier (54)

Uniquement `Exe.FindHex` : le motif d'instructions peut ne plus exister.

| patch | script | text | hex | adr. en dur |
|---|---|---|---|---|
| `AllianceChatHotkeySelector` | `AllianceChatHotkeySelector.qjs` | 0 | 1 | 0 |
| `AllowPL2Leave` | `AllowPL2Leave.qjs` | 0 | 2 | 0 |
| `AllowSkillSpam` | `AllowSkillSpam.qjs` | 0 | 7 | 0 |
| `AlwaysAscii` | `AlwaysAscii.qjs` | 0 | 1 | 0 |
| `AlwaysOfficialBG` | `AlwaysOfficialBG.qjs` | 0 | 2 | 0 |
| `CustomBarterZenySep` | `CustomBarterZenySep.qjs` | 0 | 2 | 0 |
| `CustomFadeDelay` | `CustomFadeDelay.qjs` | 0 | 7 | 0 |
| `CustomInventoryExpandingLimit` | `CustomInventoryLimit.qjs` | 0 | 3 | 0 |
| `CustomInventoryLimit` | `CustomInventoryLimit.qjs` | 0 | 3 | 0 |
| `CustomPartyLimit` | `CustomFriendWin.qjs` | 0 | 2 | 0 |
| `CustomQSDelay` | `CustomQSDelay.qjs` | 0 | 9 | 0 |
| `CustomWalkDelay` | `WalkDelay.qjs` | 0 | 2 | 0 |
| `DeleteCharWithEmail` | `DeleteCharWithEmail.qjs` | 0 | 4 | 0 |
| `Enable44kHzAudio` | `CustomSound.qjs` | 0 | 6 | 0 |
| `EnableFlagEmotes` | `EnableFlagEmotes.qjs` | 0 | 4 | 0 |
| `EnableNpcDialogueScrolling` | `NpcDialogueScrolling.qjs` | 0 | 1 | 0 |
| `EnableShowName` | `EnableSlashCmd.qjs` | 0 | 8 | 0 |
| `EnableUnknCmds` | `EnableSlashCmd.qjs` | 0 | 8 | 0 |
| `EnableWho` | `EnableSlashCmd.qjs` | 0 | 8 | 0 |
| `FixArrowsCharset` | `FixArrowsCharset.qjs` | 0 | 2 | 0 |
| `FixChatAt` | `FixChatAt.qjs` | 0 | 2 | 0 |
| `FixFontsCharset` | `FixFontsCharset.qjs` | 0 | 2 | 0 |
| `FixSelectMenuReset` | `FixSelectMenuReset.qjs` | 0 | 3 | 0 |
| `FixSelectMenuWidthExpansion` | `FixSelectMenuWidthExpansion.qjs` | 0 | 1 | 0 |
| `FixSkillbarReset` | `FixSkillbarReset.qjs` | 0 | 6 | 0 |
| `GRFsFromIni` | `MultiGRFs.qjs` | 0 | 5 | 0 |
| `HideZeroDateInGuildWin` | `HideZeroDateInGuildWin.qjs` | 0 | 1 | 0 |
| `HighCamAngle` | `IncrCamAngle.qjs` | 0 | 6 | 0 |
| `IncrNewCharHairs` | `IncrNewCharHairs.qjs` | 0 | 6 | 0 |
| `InsensitiveShopSearch` | `CaseInsensitiveSearch.qjs` | 0 | 8 | 0 |
| `InsensitiveStorageSearch` | `CaseInsensitiveSearch.qjs` | 0 | 8 | 0 |
| `NoCharnameLimit` | `NoLetterLimit.qjs` | 0 | 4 | 0 |
| `NoGMSprite` | `NoGMSprite.qjs` | 0 | 7 | 0 |
| `NoHelpMsg` | `NoHelpMsg.qjs` | 0 | 3 | 0 |
| `NoHourly` | `NoHourly.qjs` | 0 | 2 | 0 |
| `NoPassEncr` | `DisableEncr.qjs` | 0 | 4 | 0 |
| `NoPasswordLimit` | `NoLetterLimit.qjs` | 0 | 4 | 0 |
| `NoUsernameLimit` | `NoLetterLimit.qjs` | 0 | 4 | 0 |
| `OpenToServiceSelect` | `OpenToServiceSelect.qjs` | 0 | 2 | 0 |
| `PlainTextDesc` | `PlainTextDesc.qjs` | 0 | 1 | 0 |
| `PreviewInShop` | `PreviewInShop.qjs` | 0 | 8 | 0 |
| `RemoveMaxNumbersLimit` | `RemoveMaxNumbersLimit.qjs` | 0 | 2 | 0 |
| `RemovePremiumService` | `RemovePremiumService.qjs` | 0 | 1 | 0 |
| `ResizeChatBox` | `ResizeBox.qjs` | 0 | 6 | 0 |
| `ResizeChatRoomBox` | `ResizeBox.qjs` | 0 | 6 | 0 |
| `ResizePMBox` | `ResizeBox.qjs` | 0 | 6 | 0 |
| `RestoreChatFocus` | `RestoreChatFocus.qjs` | 0 | 1 | 0 |
| `RestoreModelCulling` | `RestoreModelCulling.qjs` | 0 | 6 | 0 |
| `RestoreSongsEffect` | `RestoreSongsEffect.qjs` | 0 | 3 | 0 |
| `ShortcutForAll` | `ShortcutForAll.qjs` | 0 | 1 | 0 |
| `SkipInFriendList` | `SkipCheatCheck.qjs` | 0 | 2 | 0 |
| `SkipInGuildList` | `SkipCheatCheck.qjs` | 0 | 2 | 0 |
| `UnlimitedChatRepeat` | `ChatRepeat.qjs` | 0 | 1 | 0 |
| `ZoomMax` | `IncrZoom.qjs` | 0 | 1 | 0 |

## 🔴 Prioritaire (21)

Contient des littéraux d'adresse — à relire ligne à ligne.

| patch | script | text | hex | adr. en dur |
|---|---|---|---|---|
| `Allow65kHairs` | `IncrHairs.qjs` | 4 | 8 | 4 |
| `AllowChatWhileBerserk` | `AllowChatWhileBerserk.qjs` | 0 | 0 | 5 |
| `CustomAchieveLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomChangeMatLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomCheckAttLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomItemInfoLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomMapInfoLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomMonSizeEffLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomOngQuestInfoLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomPrivAirplaneLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomQuestQualiLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomTipboxLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomTownInfoLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `CustomspopupLub` | `CustomPath.qjs` | 8 | 24 | 4 |
| `IgnoreEntryQueueErr` | `IgnoreErrors.qjs` | 4 | 14 | 1 |
| `IgnoreLuaErr` | `IgnoreErrors.qjs` | 4 | 14 | 1 |
| `IgnorePalErr` | `IgnoreErrors.qjs` | 4 | 14 | 1 |
| `IgnoreQuestErr` | `IgnoreErrors.qjs` | 4 | 14 | 1 |
| `IgnoreRsrcErr` | `IgnoreErrors.qjs` | 4 | 14 | 1 |
| `PreviewInTrader` | `PreviewInTrader.qjs` | 0 | 3 | 1 |
| `RestoreSkillAttackAnimation` | `RestoreSkillAttackAnimation.qjs` | 0 | 1 | 7 |

## ❔ À inspecter (1)

Ni texte ni motif détecté.

| patch | script | text | hex | adr. en dur |
|---|---|---|---|---|
| `RestoreIcon` | `EnableIcon.qjs` | 0 | 0 | 0 |

## ❌ Script non localisé (4)

Le `.qjs` n'a pas été retrouvé automatiquement.

| patch | script | text | hex | adr. en dur |
|---|---|---|---|---|
| `DoramHeadPalShared` | — | 0 | 0 | 0 |
| `NoLookalikeNameWarning` | — | 0 | 0 | 0 |
| `PetTalkMarker` | — | 0 | 0 | 0 |
| `RestoreGMWeaponTrail` | — | 0 | 0 | 0 |

## 🔴🔴 GameGuard : le 2026 a CHANGE de mecanisme, et `NoGGuard` se TAIT

Mesure du 2026-08-27, par recherche d'**octets bruts** dans les deux IDB (pas
`idautils.Strings()`, qui rate des chaines) :

| chaine | 2025-07-16 | 2026-07-07 |
|---|---|---|
| `GameGuard Error: %lu` | ✅ 2 occurrences | ❌ **absente** |
| `nProtect GameGuard` | ✅ | ❌ **absente** |
| `npkcrypt.dll` | ❌ **absente** | ✅ |

`NoGGuard.validate()` s'ancre sur la premiere chaine. Sur le 2026 elle renvoie
donc **faux**, et WARP retire le patch **sans un mot** — le piege de
[[reference_warp0716]], vu en vrai.

🔴 Et l'absence de patch ne donne pas « GameGuard tourne », elle donne
**un crash**. Le client embarque le chargeur **DEUX FOIS**, et une seule copie
est correcte :

| fonction | quand `LoadLibraryA("npkcrypt.dll")` echoue |
|---|---|
| `sub_A840E0` | `GetLastError` → rapport → **`return 0`** ✅ |
| `sub_A84B70` | `GetLastError` → rapport → **rien**, on continue ❌ |

La seconde retombe dans le tronc commun et appelle les 11 pointeurs jamais
resolus : `call esi` avec `esi = 0`, access violation a **`0x00A84D96`**.
Mesure prise sur client vivant (x32dbg) **et** confirmee par le rapport de
crash du client lui-meme (`esi 0`, `edx 5Ch`, retour `00A84D96` en tete de pile).

✅ Les **quatre** consommateurs des pointeurs ont ete verifies un par un :
`sub_A85B20` se garde par `if (hLibModule)`, `sub_A84E50` par
`if (!dword_146EB7C) return 1;`, `sub_A840E0` sort proprement. **`sub_A84B70`
est le seul site non garde** — donc le correctif est LOCAL, pas une
neutralisation large.

### Ou couper, et pourquoi la

Le patch `NoGGuardLoader` (`Scripts/Patches/NoGGuardLoader.qjs`) coupe **la
porte**, un thunk de 14 octets en queue d'appel :

```
A85B10  e8 eb e9 ff ff      call sub_A84500      ; GameGuard actif ?
A85B15  84 c0               test al, al
A85B17  0f 85 53 f0 ff ff   jnz  sub_A84B70      ; tail call vers le chargeur
A85B1D  c3                  retn
```
→ remplace par `33 C0 C3` (`xor eax, eax` / `retn`).

Trois raisons de couper **la** plutot que dans le chargeur :
1. le thunk ne commande **que** GameGuard — `sub_A84500` est une table de
   capacites generique (`matrix[14 * service + feature]`), la forcer aurait des
   effets de bord ailleurs ;
2. il n'a **qu'un** appelant ;
3. 🔴 **ce appelant IGNORE la valeur de retour** — mesure : l'instruction
   qui suit le `call` est `mov esi, [ebp-738h]`, pas un `test`. Donc renvoyer 0
   ne coute rien et **aucune branche d'erreur n'est a simuler**.

### Les deux patchs ne se marchent pas dessus

Leurs `validate()` sont mutuellement exclusifs **par construction** : `NoGGuard`
ne s'active que sur 2025, `NoGGuardLoader` que sur 2026. On peut laisser les
deux coches sans risque pour le client de production.

