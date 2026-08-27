# Les 19 adresses qui font démarrer Bourgeon sur le client 2026

Relevé du **2026-08-27**. `RagnarokClient::Initialize` ne construit que six
objets : c'est le seul jeu d'adresses qui décide si Bourgeon s'initialise ou
refuse le client. **8 étaient déjà portées, 11 manquaient** — pas 424.

Chacune est vérifiée par un critère **indépendant de la méthode qui l'a
trouvée**. Un nom déjà posé par un portage antérieur ne compte pas comme preuve :
ce portage a déjà produit 11 entrées fausses.

## Le tableau

| champ | 2025-07-16 | 2026-07-07 | preuve indépendante |
|---|---|---|---|
| `CSession` | `0x00d57780` | `0x00c6a730` | déjà porté · `retn 0` des deux côtés |
| `GetTalkType` | `0x00D5E590` | `0x00c71310` | déjà porté · `retn 0Ch` |
| `UIWindowMgr` | `0x00a29ba0` | **`0x009f8ed0`** | écrit `??_7UIWindowMgr@@6B@` dans `[edi]` ; 2 écrivains des deux côtés, ratios de taille concordants (0,88 et 0,87) |
| `ProcessPushButton` | `0x00a471e0` | **`0x00a15160`** | son appelant `sub_CC7240` contient `DefWindowProcA` et fait **exactement 3 appels**, comme `Game_MainWndProc` |
| `SendMsg` | `0x00a4ad20` | `0x00a18a20` | même nom `UIWindowMgr_ChatAction`, ratio 1,03, 316 appelants — ⚠ voir réserves |
| `CConnection` | `0x00c13fc0` | `0x00bdeca0` | `retn 4` · 3× `ResetCursors(0xA000)` sur +0x3C/+0x24/+0x54, motif dont la paire est prouvée à part |
| `SendPacket` | `0x00c14920` | `0x00bdf440` | `retn 8` ; c'est la fonction qui ajoute l'octet de contrôle, analysée le même jour |
| `RecvDispatchTable` | `0x00caa2e0` | **`0x0051610c`** | `get_switch_info` : `jumps`, `ncases` 3029, `lowcase` 115 |
| `RecvOpcodeBase` | `0x73` | `0x73` | `lowcase` = 115 = 0x73, identique |
| `RecvDispatchTableSize` | `0xBC3` | **`0xBD5`** | `ncases` = 3029 |
| `RecvDispatchLoopHead` | `0x00c9e1dd` | **`0x005096f9`** | code **identique instruction par instruction** (voir plus bas) |
| `RecvOpcodeReader` | `0x00c144b0` | **`0x00bdee70`** | 13 o, signature exacte, **mêmes deux appelants** qu'en 2025 |
| `RecvBufferReset` | `0x00c148b0` | **`0x00bdf3d0`** | 44 o, mêmes deux appelants |
| `PacketLenLookup` | `0x00aa7b00` | `0x00aa4290` | déjà porté · **119 o des deux côtés**, `retn 8` |
| `PacketLenTable` | `0x0159d68c` | **`0x0146edfc`** | `g_PacketLenTable`, seule valeur chargée dans `ecx` par les 4 appelants du lookup |
| `CModeMgr::Switch` | `0x00a756e0` | **`0x00a3c8c0`** | même suite d'appels virtuels (vt+8, vt+4, vt+0Ch), seul appelant `WinMain` avec `"login.rsw"`, `retn 8` |
| `CLoginMode::OnUpdate` | `0x00d272e0` | **`0x00c3a300`** | slot #4 de `??_7CLoginMode@@6B@` ; témoin d'alignement : le slot #7 est la plus grosse fonction des deux côtés |
| `CGameMode::OnUpdate` | `0x00c74a80` | `0x004cef40` | déjà porté · `retn 0` |
| `ProcessInput` | `0x00c86740` | `0x004e37a0` | déjà porté · `retn 14h` = 5 args, ce qui confirme `ProcessInputArgs: 5` |
| `PostActorClickAction` | `0x00c753a0` | **`0x004cfad0`** | `push 133h` avec `2710h` **et** `9C40h` à portée : un seul site dans l'image |
| `CScene::RenderCells…` | `0x00a7b0a0` | **`0x00a41a00`** | slot #3 de `??_7CView@@6B@` |

## 🔴 Réserves

**`SendMsg` a gagné un argument — TRAITÉ le 2026-08-27.** `retn 18h` en 2026
contre `retn 14h` en 2025. La paire est bonne — même fonction, même nom, ratio
1,03, 316 appelants — c'est **l'API du client qui a changé**.

Le nouvel argument s'ajoute **en fin de liste**, deux mesures concordantes :

- le même appelant (`PostActorClickAction`, paire établie) pousse `0,0,0,0,0,3`
  là où le 2025 pousse `0,0,0,0,3`, et `0,0,0,0FFh,<texte>,1` contre
  `0,0,0FFh,<texte>,1` — un `push 0` de plus **en tête**, donc un paramètre de
  plus **en queue** (les push sont en ordre inverse) ;
- sur 150 sites d'appel, le pic des appels complets passe de **5 à 6** push,
  avec un biais de mesure identique des deux côtés (le pic parasite à 2 push
  vaut 57 % ici comme là-bas), et le 2026 n'a plus aucun appel à 3 ou 4 push.

⇒ `ChatAction(this, action, texte, couleur, TYPE, sender, <nouveau>)`. Le 6e
vaut 0 sur tous les sites relevés, et Hex-Rays ne le lit jamais dans le corps.

Corrigé par un champ `SendMsgArgs: 6` qui fait installer un hook `__fastcall` de
signature exacte — même patron que `ProcessInputArgs` dans `game_mode.cc`. Le
champ est absent de l entrée 20250716, qui garde donc son hook membre.
⚠ Non testé en jeu : le build appartient à l utilisateur.

**Le layout de `CSession` est celui du 2025** — RELEVÉ le 2026-08-27, voir
[session_layout_2026.md](session_layout_2026.md). `g_session` 2026 vaut
`0x014B73B0` et 13 champs sont mesurés (`aid_`, les six statistiques, HP/SP,
`char_name_`), mais `mkcount_` et `talk_type_table_` restent indéterminés : les
paliers ne sont pas linéaires (24 marches) et leurs bornes ne concordent pas.

Aucun `20260707.h` n'a donc été écrit : un seul offset faux dans une structure à
padding décale tout ce qui suit et se lit comme une valeur plausible. Le
démarrage ne lit pas ce layout ; les lectures de session, si.

## La tête de boucle recv, identique des deux côtés

C'est l'appariement le plus net du lot, et il en offre trois en prime :

```
2025 (0x00C9E1DD)                  2026 (0x005096F9)
cmp  g_ReplayActive, 0             cmp  dword_1490524, 0
jnz  <sortie>                      jnz  <sortie>
call CRagConnection_GetInstance    call sub_BE1F70
lea  ecx, [ebp+param_2]            lea  ecx, [ebp+var_4BCC]
push ecx                           push ecx
push offset _Dst_015e8198          push offset word_11117B8
mov  ecx, eax                      mov  ecx, eax
call RecvBuffer_ReadPacket         call RecvBuffer_ReadPacket
test al, al / jz                   test al, al / jz
```

⇒ `CRagConnection_GetInstance` → `sub_BE1F70`, `g_ReplayActive` →
`dword_1490524`, et le tampon de réception → `word_11117B8`. Ce dernier recoupe
le crash `ZC_ACH_UPDATE` analysé le même jour, où le client indexait
`word_11117B8 + 2`, c'est-à-dire le corps du paquet après l'opcode.

## Ce qui a marché, ce qui a échoué

**Le contrôle `retn N` est le plus rentable du lot.** Il ne coûte rien et il a
attrapé la seule vraie anomalie (`SendMsg`), qu'aucun autre critère — nom,
taille, appelants — ne trahissait.

**Un ratio de taille aberrant n'est pas une preuve de faute.** `SendPacket` passe
de 95 à 5071 octets, soit 53×. C'est pourtant la bonne paire : tout le code de
hachage de l'octet de contrôle est dedans. À l'inverse `CConnection` tombe à
0,28, sans explication trouvée — signalé, pas résolu.

🔴 **Le nom d'une table de vtable peut mentir.** `CScene_RenderCellsAndCursor`
était relevé comme slot #3 de `g_CCamera_vtable`. Le RTTI juste avant la table
dit `??_R4CView@@6B@` : la classe est **CView**. Lire le RTTI, pas le nom posé.

🔴 **Ne pas fonder une recherche sur un commentaire de configuration.** La
première tentative pour `PostActorClickAction` cherchait « les fonctions qui
écrivent `+0x500` et `+0x514` », déplacements lus dans un commentaire de
`configuration.h`. Le **témoin 2025 a rendu 0** alors que la fonction y est : la
fonction ne touche ni l'un ni l'autre. Lue pour de vrai, elle offre des repères
bien plus discriminants (`push 133h`, seuils `2710h`/`9C40h`) qui donnent **un
seul candidat** dans toute l'image.

⚠ Un balayage de tout `.text` fait couper le pont MCP. Borner la plage — mais
alors vérifier que la plage contient la réponse : ici la première recherche
bornée rendait 0 pour une tout autre raison, et seul le témoin l'a montré.

## 🔴 Démarrer n'est PAS fonctionner — 430 adresses restent en 2025

Les 19 adresses ci-dessus décident seulement si Bourgeon **s'initialise**. La
configuration YAML ne porte que 22 champs ; tout le reste du projet appelle des
adresses **codées en dur**, toutes propres au 20250716.

Recensement du 2026-08-27 (commentaires neutralisés, et seuls comptés les
littéraux tombant dans le  du client 2025) :

| | |
|---|---|
| occurrences | 452 |
| **adresses distinctes** | **430** |
| fichiers concernés | 84 |

Réparties surtout dans les fonctionnalités :  98,
 94,  44,  39.

⇒ Sur le client 2026, Bourgeon démarrera, mais **chaque fonctionnalité qui touche
une de ces adresses lira du code 2025 dans un binaire 2026**. Il ne faut donc pas
lire « Bourgeon démarre » comme « Bourgeon marche » : c'est la porte d'entrée du
chantier, pas sa fin.

Le relevé est rejouable : [scripts/hardcoded_census.py](scripts/hardcoded_census.py).

⚠  /  est un **champ mort** de la configuration :
personne ne lit cette clé du YAML. Son adresse 2026 (`0x00a41a00`) a été relevée
et vérifiée, mais elle ne sert à rien pour le démarrage. La fonction est bien
utilisée par le projet — par une adresse en dur, comme les 430 autres.
