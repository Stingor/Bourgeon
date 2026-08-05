# Patchs WARP appliqués à `Moonlight-Destiny.exe`

> 🔴 **L'IDB IDA est le Ragexe VANILLA.** Aucun de ces patchs n'y est visible.
> Ce fichier est le **delta** entre ce que le désassemblage raconte et ce que le
> client exécute. Quand une constante, un gabarit de chaîne, un saut ou une
> valeur de retour lus dans IDA ne collent pas avec le jeu, **c'est ici qu'il
> faut regarder AVANT de re-RE la fonction**.

- Source de vérité : `E:\Nouveau dossier\save_exe.yml` (section `patches:` +
  section `inputs:`). C'est le fichier de session que WARP rejoue pour produire
  l'exe ; il change quand l'utilisateur repatche.
- Scripts : `D:\Mes documents\GitHub\WARP0716\Scripts\Patches\<Nom>.qjs`.
  Plusieurs patchs partagent un fichier (les « wrappers »).
- **110 patchs actifs** au relevé du 2026-08-04.

## 🔴 Le meilleur endroit pour cet avertissement, c'est l'IDB

Un document ne se lit que si l'on pense à l'ouvrir ; un commentaire IDA, lui,
tombe sous les yeux au moment exact où l'on décompile la fonction. Les patchs
dont le site a pu être **résolu sans ambiguïté** (motif hexadécimal littéral du
`.qjs`, une seule occurrence dans le binaire) portent donc leur avertissement
directement sur la fonction concernée, préfixé `⚠⚠ PATCHÉ EN JEU PAR WARP` :

| Fonction (IDB) | Patch | Effet réel |
|---|---|---|
| `Chat_SameSentenceWarn_PATCHED_OUT 0x00c861c0` | `UnlimitedChatRepeat` | `JL` → `JMP` en `0x00c861d5` : le garde ne se déclenche **jamais**, `$chatFloodLimit` est mort |
| `FileMgr_LoadToMemory 0x00a88ab0` | `DataFolderFirst` | `data\` toujours lu avant les GRF |
| `sub_D00B00 0x00d00b00` | `CustomInventoryLimit` | `mov ecx, 100` → 200 |
| `0x00c8de46` | `AllowSkillSpam` | écriture forcée de `g_session.field_5ADC` |
| `Chat_ContainsBannedTag 0x00a23180` | `NoSwearFilter` | ⚠ **ne désarme PAS** ce test (cf. §1.1) |

Les autres patchs construisent leur motif par les helpers WARP (`ROC`, `Instr`,
`Exe.FindFunc`, chaînes assemblées) : les résoudre demanderait de réimplémenter
WARP. Pour ceux-là, ce document reste la seule référence — d'où la section
« pièges » ci-dessous. **Quand un nouveau site est résolu, poser le commentaire
dans l'IDB et l'ajouter au tableau ci-dessus.**

## Comment régénérer cette liste

```bash
python - <<'PY'
import io,re,glob,os
save=io.open("E:/Nouveau dossier/save_exe.yml",encoding='utf-8',errors='replace').read()
i=save.find('patches:')
names=[l.strip()[2:].strip() for l in save[i:].splitlines()[1:] if l.strip().startswith('- ')]
for n in names: print(n)
PY
```

---

## 1. 🔴 Les pièges — patchs qui font MENTIR le désassemblage

Ceux-ci ont déjà coûté du temps, ou en coûteront : ils changent le
**comportement** d'une fonction qu'on lit dans l'IDB, pas seulement un réglage.

### 1.1 `NoSwearFilter` — le piège inverse (2026-08-04)

Le nom promet plus qu'il ne fait : il **zérote seulement la chaîne
`manner.txt`**, pour que la liste d'insultes ne se charge jamais. Le
gestionnaire `g_ChatWordFilterEnabled 0x0131F6C4` **reste non nul**, donc le
garde qui le teste s'ouvre quand même — et la fonction qu'il protège
(`Chat_ContainsBannedTag 0x00a23180`) ne lit aucune liste : elle cherche six
littéraux de BALISE en dur (`<URL>`, `<NAVI>`, `<ITEM>`, `<ITEML>`, +2).

> **La leçon** : « ce filtre est patché, ça ne peut pas venir de là » est un
> raisonnement à vérifier, pas à croire. Lire le `.qjs` prend trente secondes.

Détail complet : [chatbox_re.md §5.4bis](chatbox_re.md).

### 1.2 `BodyPalUnisex` / `HeadPalUnisex` / `DoramHeadPalShared` — les palettes

`SharedPal.qjs` réécrit les gabarits avec `%.s` (précision NULLE : l'argument
est consommé sans rien imprimer). Ce que dit IDA n'existe pas sur disque :

| | Vanilla (IDA) | Réel (Moonlight) |
|---|---|---|
| Tête | `머리\머리<coif>_<sexe>_<n>.pal` | `머리\head_<n>.pal` |
| Corps | `몸\<job>_<sexe>_<n>.pal` | `몸\body_<n>.pal` |

### 1.3 Les autres à surveiller de près

| Patch | Ce qu'il change vraiment | Nous concerne |
|---|---|---|
| `UnlimitedChatRepeat` | force un `JMP` dans `IsSameSentence` (limite de répétition d'envoi, `$chatFloodLimit` ignoré) | chatbox |
| `RestoreChatFocus` | **NOP sur l'appel `SetFocusEdit`** : la saisie garde le focus même en cliquant ailleurs | ⚠ notre gestion du focus ENTRÉE / battle mode |
| `EnableUnknCmds` | `CSession::GetNoParamTalkType` rend `TT_NORMAL` au lieu de `TT_UNKNOWN`, + `chatStartOffset` | ⚠ nos `/commandes` |
| `EnableWho` / `EnableShowName` | sauts de langtype forcés dans `CGameMode::SendMsg` et `CSession::SetTextType` | `/w`, `/who`, `/showname` |
| `FixChatAt` | la fonction qui cherche `@` dans le texte rend **1 en cas d'échec** (faux positif volontaire) | commandes `@` |
| `ShortcutForAll` | tous les `switch` de type d'objet renvoyés sur le MÊME case | barre de raccourcis |
| `IncrViewID` | limite de la table de préfixes de coiffes portée à **32000** | coiffes / hat effects |
| `TildeForMatk` | gabarit d'affichage du MATK remplacé par la version à tilde | MATK |
| `InsensitiveStorageSearch` / `InsensitiveShopSearch` | `StrStrIA` à la place de `_mbsstr` / `memchr` | recherche storage & shop |
| `DataFolderFirst` | tous les `JZ/JNZ/CMOVNZ` après `g_readFolderFirst` neutralisés | VFS disque-d'abord |
| `LoadKrExtSettings` | `ExternalSettings_kr` chargé quel que soit `g_serviceType` | ExternalSettings |
| `DeleteCharWithEmail` | sauts de langtype forcés dans la suppression de personnage | suppression par e-mail |
| `UseOldLogin` | rétablit le paquet de login **0x64** | login |
| `NoPassEncr` | sauts forcés après les comparaisons langtype 7 et 4 | login |
| `IgnoreRsrcErr` … `IgnoreEntryQueueErr` | les MessageBox d'erreur deviennent des `OutputDebugStringA` | ⚠ une erreur ressource ne se VOIT plus : la chercher au débogueur |
| `CustomFadeDelay` | délai de fondu des warps même-carte mis à **0** (`$warpFadeOutDelay`) | chargement de carte |
| `AllowSkillSpam` | force l'écriture de `g_session.field_5ADC` | latence de compétence |
| `SendClientFlags` | l'adresse de « version » est déplacée, drapeau maison au nouvel emplacement | paquets de login |

---

## 2. Liste complète, par thème

### Chat
`ResizeChatBox` · `ResizeChatRoomBox` · `ResizePMBox` (longueurs à 234) ·
`UnlimitedChatRepeat` · `NoSwearFilter` · `FixChatAt` · `AllowChatWhileBerserk` ·
`RestoreChatFocus` · `AllianceChatHotkeySelector` · `EnableWho` ·
`EnableShowName` · `EnableUnknCmds` · `GuildBrackets` (`「」` → `( )`) ·
`NoDancerScream` · `NoBardFrostJoke`

### Ressources, chemins et Lua
`DataFolderFirst` · `GRFsFromIni` (`$dataINI = DATA.INI`) · `AddLuaOverrides` ·
`LoadKrExtSettings` · `CallKoreaClientInfo` · `TranslateClient` ·
`CustomItemInfoLub` (`SystemEN\iteminfo.lua`) · `CustomAchieveLub` ·
`CustomMonSizeEffLub` · `CustomTownInfoLub` · `CustomTipboxLub` ·
`CustomCheckAttLub` · `CustomPrivAirplaneLub` · `CustomMapInfoLub` ·
`CustomOngQuestInfoLub` · `CustomspopupLub` · `CustomChangeMatLub` ·
`CustomQuestQualiLub`

### Sprites et palettes
`BodyPalUnisex` · `HeadPalUnisex` · `DoramHeadPalShared` · `Allow65kHairs` ·
`IncrNewCharHairs` · `IncrViewID` · `NoGMSprite` · `RestoreModelCulling` ·
`DisableDoram`

### Caméra et rendu
`HighCamAngle` · `ZoomMax` · `NoCameraLock` · `CustomFadeDelay`

### Interface
`ShortcutForAll` · `FixSkillbarReset` · `FixSelectMenuReset` ·
`FixSelectMenuWidthExpansion` · `EnableNpcDialogueScrolling` · `NoEquipWinTitle` ·
`DisableTraitStatusButton` · `HideTraitStatusButton` · `HideZeroDateInGuildWin` ·
`CustomPartyLimit` (20) · `CustomInventoryLimit` / `CustomInventoryExpandingLimit`
(200) · `RemoveMaxNumbersLimit` · `CustomBarterZenySep` · `HideNewButtons` ·
`ShowNewButtons` · `FixLatestNCWin` · `RestoreBattlegroundUI` ·
`InsensitiveStorageSearch` · `InsensitiveShopSearch` · `TildeForMatk` ·
`EnableFlagEmotes`

### Boutique et cash shop
`ZeroCinShop` · `PreviewInShop` · `PreviewInTrader` · `RemovePremiumService`

### Login, réseau, compte
`UseOldLogin` · `NoPassEncr` · `SendClientFlags` · `OpenToServiceSelect` ·
`DisConnToLogin` · `DeleteCharWithEmail` · `NoCharnameLimit` ·
`NoUsernameLimit` · `NoPasswordLimit` · `NoNagle` · `NoGGuard`

### Texte et polices
`FixFontsCharset` · `CustomFontCharset` (`ANSI`) · `FixArrowsCharset` ·
`AlwaysAscii` · `PlainTextDesc`

### Fenêtre et démarrage
`EnableSysMenu` · `CustomWinTitle` (« Moonlight-Destiny ») · `BorderlessFSW` ·
`RestoreIcon` · `No1and1Arg` · `HideBuildInfo` · `NoGravityAds` ·
`NoGravityLogo` · `AlwaysOfficialBG` · `NoHourly` · `NoHelpMsg`

### Erreurs silencieuses
`IgnoreRsrcErr` · `IgnorePalErr` · `IgnoreLuaErr` · `IgnoreQuestErr` ·
`IgnoreEntryQueueErr`

### Gameplay
`AllowSkillSpam` · `AllowPL2Leave` · `CustomQSDelay` · `CustomWalkDelay`
(`$walkDelay` / `$walkDelay2` = 100) · `SkipInFriendList` · `SkipInGuildList`

---

## 3. Valeurs choisies (`inputs:`)

| Clé | Valeur |
|---|---|
| `$chatBoxLen` / `$chatRoomBoxLen` / `$pmBoxLen` | 234 |
| `$chatFloodLimit` | 5 (**sans effet** : `UnlimitedChatRepeat` court-circuite le test) |
| `$mainChatColor` / `$whisperChatColor` / `$detachedChatColor` | 1413894 / 7211280 / 993434 |
| `$mainChatAlpha` / `$whisperChatAlpha` / `$detachedChatAlpha` | 255 |
| `$viewIDLimit` | 32000 |
| `$MaxItemCount` / `$InventoryExpandMax` | 200 / 200 |
| `$maxParty` | 20 |
| `$walkDelay` / `$walkDelay2` | 100 / 100 |
| `$warpFadeOutDelay` | 0 |
| `$customWindowTitle` | « Moonlight-Destiny » |
| `$newFontCharset` | ANSI |
| `$dataINI` | `DATA.INI` |
| `$newItemInfo` | `SystemEN\iteminfo.lua` |
| `$AllianceChatHotkey` | `^` (0x5E) |
