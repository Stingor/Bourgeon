# WARP sur le client 2026-07-07 : résultat du test

Passe complète du 2026-08-26 sur le client 2026-07-07, branche `client-2026`
de `WARP0716`, **116 patchs actifs**.

| | n | |
|---|---|---|
| ✅ **fonctionnent tels quels** | **65** | 56 % |
| 🟠 échecs | 43 | 37 % |
| ⚪ ignorés (borne de version / auto-verrouillage) | 7 | 6 % |
| 🐞 bug de script | 1 | 1 % |

🔴 **56 % passent sans rien toucher, après un an de dérive.** C'est la mesure
qui valide la faisabilité du portage : il n'y a pas de mur, il y a une liste.

## 🔴 Les trois faux échecs — à régler sans toucher à un motif

| patch | vrai problème |
|---|---|
| `Translate client` | *Unable to create FailedTranslations Output file* — un **fichier de sortie**, pas un motif. Droits ou chemin. |
| `Enable shared head palettes for Doram` | *Enable a Shared Head Palette patch first* — une **dépendance**. `HeadPalUnisex` passe : c'est un ordre d'application. |
| `Increase char create Hair limits` | `TypeError: Cannot call method 'getReg' of undefined` — **bug du script**, pas le binaire. |

## 🔴🔴 Le gisement « Langtype » : 6 patchs, une seule cause

Ces six échouent tous sur *Langtype comparison (not found|missing)* :

- `Always enable '/who' command`
- `Use Old Login Packet`
- `Use plain text descriptions`
- `Use official login BG`
- `Always use email for Char Deletion`
- `No Help Message on Login`

Le motif attend la cascade `cmp eax,4 / 8 / 9 / 0Eh / 3 / 0Ah / 1 / 0Bh`.
**Elle n'existe plus.** Ce que porte le build 2026 :

```
0x00b9e7e4   cmp eax,4 / jz / cmp eax,8 / jz / cmp eax,0Ch      <- 0x0C
0x00cc86c2   cmp eax,4 / jz / cmp eax,8 / jz / cmp eax,9 / jz / cmp eax,6   (_WinMain)
0x00cd5fb0   cmp eax,4 / jz / cmp eax,8 / jz / cmp eax,10h      <- 0x10
```

⚠ **Ce n'est pas un problème de saut, c'est le contenu métier** : Gravity a
changé la liste des langtypes supportés (`0x0C`, `0x10`, `6` apparaissent ; la
queue `0xE, 3, 0xA, 1, 0xB` a disparu). Un simple ajustement de motif ne suffit
donc pas — chaque patch doit retrouver **sa** cascade par sa fonction cible
(chaîne voisine, appel voisin), puis lire les langtypes présents au lieu de les
coder en dur. La cause est commune, la réparation ne l'est pas.

## Échecs (43)

| patch | message |
|---|---|
| `Restore GM weapon trails` | No weapon trail (slot 6) site found |
| `Allow party leader to leave` | Reference pattern missing |
| `Skip Friend list cheat check` | Comparisons not found |
| `Skip Guild Member cheat check` | Comparisons not found |
| `Alliance Chat Hotkey Selector` | Alliance chat pattern not found |
| `Restore Bard/Dancer Song Effects` | Borrow code not found |
| `Always enable '/who' command` | Langtype comparison not found |
| `Allow unknown '/command's` | Function start missing |
| `Enable equipment preview in Cash Shop` | message coreen illisible |
| `Increase Camera Angles (HIGH)` | Function CALL missing |
| `Increase Zoom Out (to Max)` | Pattern '00 00 F0 43 00 00' not found |
| `Use Old Login Packet` | Langtype comparison missing |
| `Allow unlimited chat repeat` | ChatRepeat pattern not found |
| `Read data folder first` | New DataFolderFirst pattern not found (2025+) |
| `Use plain text descriptions` | Langtype comparison missing |
| `Always load korea ExternalSettings lua file` | 'ExternalSettings_kr' reference missing |
| `Translate arrows to English` | First set of arrows missing |
| `Translate client` | Unable to create FailedTranslations Output file |
| `Customize Iteminfo lub` | System\iteminfo.lub not used |
| `Enable shared head palettes for Doram` | dependance : activer HeadPalUnisex d abord |
| `Disable Login password encryption` | Pattern2 (cmp ecx,4 -> jmp) not found in any fallback |
| `Add Chris' lua overrides` | LUA - ESP allocation not found |
| `Use 'Opening' for service selection` | MsgString ID Missing |
| `Use official login BG` | Langtype comparison missing |
| `Remove Hourly announcement` | PlayTime JLE comparison not found |
| `Enable flag emoticons` | Switch not found |
| `Custom inventory limit` | Item count function missing |
| `Always use email for Char Deletion` | Langtype comparison not found |
| `No Help Message on Login` | Langtype comparison not found |
| `NPC Dialog Scroll` | NpcDialogueScrolling pattern not found |
| `Customize fade in/out delay` | pattern not found |
| `Restore chat focus` | SetFocusEdit CALL missing |
| `Hide Zero Date in Guild Window` | Function not found |
| `Allow all items in Shortcut` | Expected 2 matches for switch |
| `Restore Battleground UI` | Could not find the window instructions pattern |
| `Disable Doram` | mov edi, doram_on not found |
| `Fix NPC dialogue Select Menu Reset` | FixSelectMenuReset pattern1 not found |
| `Fix loading shortcuts / skill bar state` | Reference pattern missing (A2 ItemSlotActive) |
| `Format Barter Zeny with Commas` | Number formatter function not found |
| `Case-Insensitive storage search` | String search not found |
| `Case-Insensitive cash shop search` | Register is incorrect |
| `Remove gravity ads` | Suffix No. 1 not found |
| `Remove Eq Window title` | Pattern#2 not found |

## Ignorés (7)

Sans effet sur le client : bornes `Exe.BuildDate` ou auto-verrouillage sur
`20250716` (cf. `warp_patches_port.md`).

| patch | message |
|---|---|
| `Disable Game Guard` | — |
| `Customize PrivateAirplane lub` | — |
| `Customize MapInfo lub` | — |
| `Customize ChangeMaterial lub` | — |
| `Customize QuestClassificationInfo lub` | — |
| `Allow Chatting While Berserk` | — |
| `Restore Skill Attack Animation` | — |

## Fonctionnent déjà (65)

`Disable GM sprite`, `Restore model culling`, `Customize Quick Switch delay`, `Allow spam skills by hotkey`, `Allow 65k Hair Styles`, `Customize Walk Delay`, `Always enable '/showname' command`, `Remove Dancer's Scream chatter`, `Remove Bard's Frost Joke chatter`, `Resize Chat Box`, `Resize Chat Room Box`, `Resize PM Box`, `Enforce 0 C in Cash Shop`, `Enable equipment preview in Trader Shop`, `Fix charset for Fonts`, `Use Ascii on All`, `Always call SelectKoreaClientInfo()`, `Increase headgear viewID limit`, `Hide build info`, `Disable camera lock`, `Customize AchievementList lub`, `Customize MonsterSizeEffect lub`, `Customize Towninfo lub`, `Customize Tipbox lub`, `Customize CheckAttendance lub`, `Customize OngoingQuestInfoList lub`, `Customize spopup lub`, `Enable shared body palettes - Unisex`, `Enable shared head palettes - Unisex`, `Enable Multiple GRFs ( INI )`, `Remove Premium Service text`, `Enable title buttons`, `Customize Window title`, `Enable Borderless full screen`, `Disable '1*1' arguments`, `Restore Inbuilt App icon`, `Disable nagle algorithm`, `Send client flags`, `Enable 44.1 kHz audio sampling frequency`, `Remove the limit of max number displayed`, `No bogus not your guildsman whisper warning`, `Use Tilde for Matk`, `Disable Swear Filter`, `Use normal guild brackets`, `@ Bug fix`, `Mark pet chatter in chat`, `Change limit for cash inventory expanding value`, `Fix NPC dialogue Select Menu Width Expansion`, `Remove gravity logo`, `Customize Font charset`, `Ignore Resource errors`, `Ignore Palette errors`, `Ignore Lua errors`, `Ignore Quest errors`, `Ignore Entry Queue errors`, `Hide Buttons (New UI)`, `Show Buttons (New UI)`, `Fix latest new char window`, `Customize party limit`, `Disconnect to Login Window`, `Remove 4/6 letter Character Name limit`, `Remove 4/6 letter User Name limit`, `Remove 4/6 letter Password limit`, `Disable Trait Status Button`, `Hide Trait Status Button`
