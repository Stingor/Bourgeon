# Le layout de CSession sur le client 2026

Relevé du **2026-08-27**. Complète [boot_addresses.md](boot_addresses.md), dont
c'était la seconde réserve.

## 🔴 `g_session` 2026 = `0x014B73B0`

Contre `0x015FA3C0` en 2025. Trois chemins indépendants y mènent, ce qui rend
l'adresse sûre :

- le portage de positions globales l'apparie directement ;
- c'est le `dword_14B73B0` que le constructeur du `CZ_ENTER` charge dans `ecx`
  (`mov ecx, offset dword_14B73B0` avant `PacketLen_GetFixed`) ;
- c'est aussi le premier argument de `sub_C92920` dans le handler
  `ZC_ACH_UPDATE`.

## Les champs mesurés

13 champs retrouvés en appariant `g_session + offset` comme **adresse absolue** :
`g_session` étant un global à adresse fixe, chacun de ses champs apparaît en
clair dans le code, et le portage en a déjà apparié des milliers.

| champ | 2025 | 2026 | palier |
|---|---|---|---|
| `cur_map_type_` | `+0x0000` | `+0x0000` | 0 |
| `aid_` | `+0x15E4` | **`+0x15AC`** | −56 |
| `STR` … `LUK` | `+0x1664`…`+0x1678` | **`+0x162C`…`+0x1640`** | −56 |
| `hp_` `max_hp_` `sp_` `max_sp_` | `+0x5548`…`+0x5554` | **`+0x50F4`…`+0x5100`** | −1108 |
| `char_name_` | `+0x81A8` | **`+0x7828`** | −2432 |

**Ce qui rend ces valeurs sûres, ce n'est pas la méthode, c'est la redondance :**
les six statistiques sont contiguës par pas de 4 des deux côtés et donnent toutes
exactement le même écart ; les quatre champs HP/SP aussi. Leurs paliers reposent
respectivement sur 62 et 17 mesures, celui de `char_name_` sur 4.

**Recoupement extérieur.** `aid_` tombe sur `0x14B895C`, c'est-à-dire le global
d'où le constructeur du `CZ_ENTER` tire l'account id (`mov eax, ArgList`, écrit
en `+2` du paquet). Ce chemin-là a été établi le matin même, sans rapport avec le
layout : deux mesures indépendantes concordent.

## 🔴 Deux champs INDÉTERMINÉS — ne pas les deviner

Les paliers **ne sont pas linéaires** : 24 marches sur la plage, de 0 à −2432,
sans régularité. Interpoler entre deux bornes qui ne concordent pas serait une
invention, pas une mesure.

| champ | 2025 | borne inférieure | borne supérieure | verdict |
|---|---|---|---|---|
| `mkcount_` | `+0xAFC` | −40 à `+0x78C` | −44 à `+0xE7C` | indéterminé |
| `talk_type_table_` | `+0x51F8` | −1260 à `+0x51F0` | −1256 à `+0x5420` | indéterminé |

⚠ `talk_type_table_` est **à 8 octets** de sa borne inférieure : −1260 est
probable, donc `+0x4D0C`. Ce n'est pas prouvé — la marche de +4 tombe quelque
part entre `+0x51F0` et `+0x5420`, et rien ne dit qu'elle est après.

## ⚠ Une paire fausse détectée au passage

`+0x7EAC` porte un shift de **−12060**, isolé au milieu d'un palier à −2380 /
−2384. Une seule mesure, aberrante de deux ordres de grandeur : c'est une entrée
fausse du portage de globaux. À écarter, et à traiter si elle sert ailleurs.

## 🔴 Les deux champs indéterminés n'ont aucune portée

Vérification faite : `SESSION_IMPLEMENTATION` n'expose que **sept accesseurs** —
`aid_`, `hp_`, `max_hp_`, `sp_`, `max_sp_`, `char_name_` et `item_list_`. Tous
les autres champs du layout 20250716, `mkcount_` et `talk_type_table_` compris,
**ne sont lus par personne**. Ce sont des déclarations décoratives.

Or les six champs vivants sont précisément ceux qui sont mesurés. Le layout
[20260707.h](../../src/ragnarok/object_layouts/session/20260707.h) a donc été
écrit, et il ne déclare **que ce qui est mesuré et réellement lu** : le reste est
du remplissage, pour ne pas propager des offsets que rien ne vérifierait.

⚠ Seule exception, assumée : `item_list_` est placé par report du palier −56,
sans mesure. Sans conséquence — le champ était **déjà faux en 2025** (tête de
liste lue à 0, crash au premier parcours) et son unique consommateur,
`GetItemInfoById`, n'a aucun appelant. La macro exige le champ, donc il est
déclaré ; il ne doit pas être lu.

Les données chiffrées restent dans
[session_layout_2026.json](session_layout_2026.json), paliers compris.

## Ce que la méthode ne pouvait pas donner

Les **accesseurs triviaux** (`mov eax, [ecx+disp] / retn`) ne couvrent que la
plage `0x16xx`–`0x17xx` : 14 fonctions, toutes déjà dans
[accessor_pairs.json](accessor_pairs.json). Les champs lointains n'ont pas de
getter, le client y accède directement. C'est une limite de la méthode, pas un
échec du relevé — et c'est pourquoi il a fallu passer par les globaux.

De même, `GetTalkType` n'accède **pas** à `talk_type_table_` par un déplacement
constant (aucun déplacement positif dans la plage `0x100`–`0x20000` sur ses
9305 octets) : la table lui est passée. La voie qui semblait la plus directe ne
mesurait rien.
