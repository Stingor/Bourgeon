# Système d'opcodes custom Bourgeon / moonlight

> Journal du chantier. La fiche de mémoire `opcode-system` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

## 🔴🔴 Le tampon du client n'est PAS le fil (piège payé le 2026-08-20)

`SendPacketHook` voit le tampon **avant** le brouillage natif du premier mot. Les opcodes SORTANTS
qu'on y compare sont donc ceux que le CLIENT ÉCRIT, et ils **ne coïncident pas** avec le
`clif_packetdb.hpp` de moonlight, qui attend les valeurs remaniées du bloc `PACKETVER >= 20130320`
(`0x088e` ActionRequest, `0x089b` UseSkillToId). **L'écart est NORMAL — ne rien « corriger ».**

Exemple qui a coûté cher : `CZ:WalkToXY`. Le serveur le lit en `0x0881`, le client l'**écrit** en
`0x035F` (`mov eax, 35Fh`, 0x00C8F865). Un commentaire de `keyboard_move.h` annonçait `0x0881` comme
la valeur construite — c'était celle du packet_db, et elle m'a fait conclure à tort que les opcodes
du tampon étaient remaniés. On ne peut RIEN déduire du packet_db serveur pour ce qu'on observe dans
`SendPacketHook`, ni de la table de longueurs héritée du client (`PacketLenTable_Insert` 0x00AA6C10 —
**une absence n'y prouve rien**, cf. [[feedback_absence_needs_measurement]]).

✅ **La seule preuve qui vaille est l'octet au site de construction** (tous dans le dispatch de
`CGameMode`, `ProcessInput` 0x00C86740) :

| Paquet | Site | Immédiat | Longueur / disposition |
|---|---|---|---|
| CZ_USE_SKILL | `0x00C8DE53` | `mov eax, 438h` | 10 o : +2 niveau, +4 id, +6 GID |
| CZ_REQUEST_ACT | `0x00C8F807` | `mov eax, 437h` | 7 o : +2 GID, +6 action |
| CZ_REQUEST_MOVE | `0x00C8F865` | `mov eax, 35Fh` | 5 o : x/y/dir empaquetés (le serveur, lui, lit `0x0881`) |

Recette : `find_bytes "B8 <lo> <hi> 00 00"` (mov eax, imm32) puis lire les `mov [ebp-...]` qui suivent.

## Custom Opcodes in Use

| Opcode | Direction | Name | Purpose |
|---|---|---|---|
| 0x0BFB | CZ | CZ_BOURGEON_INTEGRITY | Client→server integrity handshake (triggers settings + preset list send) |
| 0x0BFD | CZ | CZ_BOURGEON_SETTING | Client→server: [opcode:2][len:2][id:2][value:2] |
| 0x0BFE | ZC | ZC_BOURGEON_SETTINGS | Server→client: full settings sync, variable-length |
| 0x0C1F | ZC | ZC_BOURGEON_DISCORD_MSG | Server→client: discord relay message |
| 0x0C20 | CZ | CZ_BOURGEON_PRESET_CMD | Client→server: [opcode:2][len:2][cmd:1][no:1][name...] |
| 0x0C21 | ZC | ZC_BOURGEON_PRESET_LIST | Server→client: [opcode:2][len:2][active_no:1][count:1][entries...] |
| 0x0C22 | ZC | ZC_BOURGEON_SKILL_DMG | Server→client SELF-only: [opcode:2][len:2][src_aid:4][damage:4] = 12 bytes; sent when a skill unit hits, for DPS meter attribution |

## Dispatch Table Bounds (20250716 Ragexe)

- Table base VA: `0x00CAA2E0`
- Base opcode: `0x0073`
- Table size: 0xBC3 entries
- Max opcode: `0x0C35`
- Confirmed via Ghidra disassembly of `FUN_00c9df00`:
  ```asm
  00c9e2a3: ADD EAX,-0x73
  00c9e2a6: CMP EAX,0xBC2
  00c9e2ab: JA  0x00caa2b7    ; out of bounds → skip handler
  00c9e2b1: JMP dword ptr [EAX*4 + 0xcaa2e0]
  ```
- 0x0C20 → idx 0xBAD = 2989 ✓ (< 0xBC2 = 2994)
- 0x0C21 → idx 0xBAE = 2990 ✓

## Stream Length Parser (`FUN_00c147d0` at `0x00c147d0`)

Called before dispatch. Reads opcode from stream, looks up packet length via `0x00aa7b00` with table at `0x159D68C`, then reads the rest of the packet.

`0x00aa7b00` returns a struct `{type, len}`:
- `type=1,  len=fixed`  → fixed-length packet (hardcoded in table)
- `type=0,  len=offset` → variable-length (reads length from stream)
- `type=-1, len=4`      → **UNKNOWN opcode** (not in client's table)

In `FUN_00c147d0`:
```asm
CMP dword ptr [EAX], 0x1
JNZ 0x00c1484f   ; type=0 AND type=-1 both go here (variable/unknown path)
```

**Key finding:** type=0 and type=-1 (unknown) go through the **same code path**. Both read 4 bytes and take the high word as the packet length. 0x0BFE (ZC_BOURGEON_SETTINGS) is also unknown to the client and demonstrably works — which proves this path correctly handles our custom variable-length packets.

## Collision Safety Rule

For any new custom ZC opcode (server→client):
1. Must be ≤ 0x0C35 (dispatch table bound)
2. Must not have an entry in the client's packet table at `0x159D68C` **with a different fixed length** than what the server sends — if it does, the client uses that hardcoded fixed length, reads the wrong number of bytes, and causes stream desync
3. Both 0x0C20 and 0x0C21 verified safe by the above criteria

**0x0C22 EXCEPTION — confirmed by x32dbg runtime (2026-06-21):** `FUN_00aa7b00(0x0C22)` returns `{flag=1, length=12}` — it IS in the vanilla packet table with fixed-length 12. This makes it work as long as the server also sends exactly 12 bytes. Adding a `skill_id` field (→14 bytes) caused 2 bytes to be left in the recv buffer per hit; after ~7 Storm Gust hits, 14 garbage bytes accumulated, were parsed as a phantom packet, and the stream desynced → game freeze confirmed. Reverted to 12 bytes, freeze gone.

**Key lesson:** An opcode that's in the vanilla table CAN be reused IF the server matches the vanilla fixed length exactly. But the packet cannot be extended without also patching the client's length table (which we don't do). Prefer opcodes that are truly absent from the table (type=-1 path) to avoid this fragility.

The proof that unknown opcodes work: 0x0BFE has been in production use and works correctly. It is equally unknown to the client (type=-1, reads length from stream bytes[2..3]).

**How to apply:** When adding a new custom opcode, verify it is ≤ 0x0C35 and not an opcode the vanilla client already knows. No need to patch the client's packet-length table.

## ⭐ DUMP DÉFINITIF de la table de longueurs (in-DLL, 2026-07-03)
Diagnostic in-DLL (appel FUN_00aa7b00 par opcode, LogDiag) — méthode ROBUSTE (le debugger x32 live est inutilisable car le jeu est relancé en boucle → heap bouge ; pont x32 a une race). flag: **1=fixe (DANGER)**, **0=variable natif (détournable mais NATIF)**, **-1=inconnu (VRAIMENT LIBRE)**.
- **Max opcode de la table de longueurs = 0x0C35** (nœud _Right du map @ 0x0159d68c ; clé max lue live). ⇒ TOUT opcode > 0x0C35 est **flag=-1** (variable, longueur lue du flux = sûr).
- ⚠️ **0x0C23 = fixe len=9**, **0x0C24 = fixe len=979** (NATIFS — surtout PAS pour du custom !). 0x0C22 = fixe 12.
- Seul flag=-1 ≤0x0C35 dans [0x0BF0..0x0C35] : **0x0C1F** (déjà pris = discord). Nos 0x0C20/0x0C21 = flag=0 (variable natif détourné, OK car serveur ne l'émet pas). 0x0BFE = flag=0 len=76.
- **0x0C36..0x0C40 (et au-delà) = tous flag=-1** = zone garantie libre.

## ⭐ ZONE CUSTOM SÛRE + MÉCANISME reader-hook (2026-07-03)
Contrainte : `RegisterRecvOpcode` **patche la dispatch table** → opcode doit être **≤ 0x0C35** (idx < RecvDispatchTableSize=0xBC3), sinon écriture HORS BORNES (corruption). ⇒ les opcodes >0x0C35 ne peuvent PAS passer par la dispatch table.
**Solution (implémentée)** : `RagConnection` — pour opcode dont `idx >= recv_dispatch_table_size_` (0xBC3), on NE patche PAS la table ; on l'ajoute à **`s_reader_dispatch_opcodes_`** et **`PacketBufReaderHook` déclenche `FireRecvPacket` directement** (il voit tous les paquets ; le parser client gère flag=-1 en variable). Config : **`RecvDispatchTableSize: 0xBC3`** ajoutée. Pas de double-fire (≤0x0C35 = dispatch, >0x0C35 = reader-hook, disjoints).
⇒ **ZONE SÛRE = 0x0F00+** (loin du max client 0x0C35 ET de la plage rAthena → zéro collision, future-proof). **Tech infos item/skill : 0x0F00 (CZ req) / 0x0F01 (ZC resp).** (Requête CZ = sortante : aucune contrainte table client, juste enregistrer côté rAthena.)
Note : envoyer une requête CZ custom AVANT que rAthena ne la gère ⇒ **déconnexion** (paquet inconnu) ⇒ garder kEnableServerFetch=false tant que le serveur n'est pas prêt.

## ⭐ DUMP COMPLET de la table opcode (2026-07-03) — `Bourgeon/docs/opcode_map.md` + `.csv`
Cartographie exhaustive générée par RE statique du binaire (lecture directe du .exe sur disque, PE VA→offset) + recoupement packetdb serveur. **1610 opcodes** au total ; **1522** dans la table de longueurs (1186 fixes / 336 variables, bornes 0x0064..0x0C35) ; **847** avec handler de zone réel ; 849 nommés ; 58 divergences longueur client/serveur. Les nôtres sont flagués `[X]`, les vanilla écoutés `[o]`.
- **Table de longueurs** = `std::map g_PacketLenTable @0x0159d68c`, construite par `PacketLenTable_Init @0x00aa0090` via 1522 appels `PacketLenTable_Insert(opcode, lenType, realLen, replayFlag)` : node +0x10=opcode, +0x14=lenType(0xFFFFFFFF=variable sinon FIXE), +0x18=realLen, +0x1c=replayFlag.
- **5e champ = flag REPLAY** (résolu) : lu par `PacketLenTable_GetReplayFlag @0x00aa0040` (renvoie node+0x1c), consommé par l'enregistreur de replay `ReplayRecorder_WritePacket @0x00b1eb40` (flag≠0 ⇒ paquet écrit au replay). flag=1 corrèle fortement avec les ZC reçus en jeu (pas une direction stricte).
- **Table de dispatch** = `g_RecvDispatchTable @0x00caa2e0`, 0xBC3 entrées (op-0x73), défaut-skip `@0x00caa2b7` (2164 slots). Bound-check `@0x00c9e2a3` : `ADD EAX,-0x73 / CMP EAX,0xBC2 / JA / JMP [EAX*4+0xcaa2e0]`.
- **Rappel collision** : la table client n'est lue qu'en RÉCEPTION ⇒ danger seulement pour nos **ZC**. Nos CZ (0x0BFB/0x0BFD/0x0C20/0x0C23/0x0F00) = sans risque côté client. 0x0C22 ZC réutilise le vanilla **CZ_MOVE_ITEM_TO_PERSONAL** (fixe 12). 0x0C23=vanilla fixe 9 (CZ, OK). 0x0BFE/0x0C21=variables (sûrs). 0x0C1F/0x0F00/0x0F01=absents (sûrs).
- **Ghidra consigné** : fonctions renommées (PacketLenTable_Init/Lookup/Insert/GetReplayFlag/LowerBound, PacketLen_Get/IsFixed, RecvLoop_DispatchPackets, ReplayRecorder_WritePacket/IsRecordable) + data g_PacketLenTable/g_RecvDispatchTable + plate-comments + prototype de _Insert.

## ⭐ MIGRATION ZONE SÛRE 0x0F00+ (2026-07-03) — "champ libre"
Tous les opcodes custom déplacés de la plage éparpillée (0x0BFx/0x0C2x) vers un **bloc contigu 0x0F00..0x0F0A**, pour ne plus jamais se poser de question de collision. **Source unique client** : `src/plugins/bourgeon_opcodes.h` (namespace `bopcodes`, prochain libre marqué `kNextFree=0x0F0B`). Les headers de plugins pointent leurs constantes locales dessus (noms locaux conservés → zéro site d'usage touché).
Nouveau mapping : REQ_TECHDATA 0x0F00 / TECHDATA 0x0F01 / INTEGRITY 0x0F02 (ex-0BFB) / KICK_NOTICE 0x0F03 (ex-0BFA) / SETTING 0x0F04 (ex-0BFD) / SETTINGS 0x0F05 (ex-0BFE) / PRESET_CMD 0x0F06 (ex-0C20) / PRESET_LIST 0x0F07 (ex-0C21) / DISCORD_MSG 0x0F08 (ex-0C1F) / SKILL_DMG 0x0F09 (ex-0C22) / CHEAT_REPORT 0x0F0A (ex-0C23).
Serveur (moonlight) : les 9 valeurs `DEFINE_PACKET_HEADER` dans `packets_struct.hpp` (tout le reste = noms symboliques `HEADER_*`, suit automatiquement). Commentaires numériques rafraîchis dans clif.cpp/clif.hpp/script.inc.

### ⚠️ CONTRAINTE À DEUX TÊTES (découverte critique, la vraie zone sûre)
- **Client** : opcode doit être **> 0x0C35** (max table client) pour être flag=-1 (variable, sûr).
- **Serveur rAthena** : opcode doit être **≤ MAX_PACKET_DB** sinon `packetdb_addpacket` (clif.cpp ~27108) **ignore SILENCIEUSEMENT** → CZ jamais parsé. `MAX_PACKET_DB` était **0xCFF** (clif.hpp:62) → 0x0F00+ aurait cassé nos CZ (integrity/setting/preset_cmd/cheat_report) sans erreur visible.
- **Fix** : `MAX_PACKET_DB` remonté à **0xFFF** (safe : seulement dimensionnement tableau +~18KB + bornes `< MAX_PACKET_DB`, aucune autre logique). ⇒ plage **0x0F00..0x0FFF valide des DEUX côtés**. (Note : ZC n'ont pas cette contrainte serveur, seulement les CZ reçus.)
- **Bonus** : SKILL_DMG (ex-0x0C22 fixe-12 qui gelait le jeu si étendu) est maintenant variable ⇒ EXTENSIBLE (ex. ajouter skill_id) sans désync.

### À FAIRE avant de considérer fini
- **Smoke-test reader-hook** : les ZC > 0x0C35 basculent de la dispatch-table (éprouvée) vers `PacketBufReaderHook`/`s_reader_dispatch_opcodes_` (codé, cf. rag_connection.cc, mais **jamais exercé en live** car kEnableServerFetch=false). Vérifier au 1er run que la sync settings (ZC 0x0F05) arrive bien.
- **Cutover coordonné** : client (ddraw.dll) + serveur (map-server) déployés ENSEMBLE. Build = réécrit le hash d'intégrité ⇒ ne PAS builder/déployer en itération sur le serveur prod (cf. [[feedback_build_and_git]], [[feedback_build_and_git]]).
- Non buildé/testé au moment de l'écriture (code-complete des 2 côtés).

## ⭐⭐ PIÈGE CRITIQUE : RESET BUFFER sur opcode HORS-PLAGE (2026-07-04) — la "safe zone" n'était PAS sûre en interleave
**Symptôme** : dépôt storage par drag → l'item déposé RESTE en fantôme dans l'inventaire (confirmé live : le nœud id "1252" est ENCORE dans la liste inventaire client `*(0x0131f6bc)+0xe8`, alors que le serveur l'a bien retiré). Desync client/serveur RÉEL, pas visuel.
**Cause racine** (RE Ghidra) : la boucle recv `RecvLoop_DispatchPackets @0x00c9df00`, pour tout opcode **HORS-PLAGE** (`opcode-0x73 >= 0xbc3`, ie > 0x0C35 = **TOUS nos 0x0F0x**), n'appelle PAS le dispatch mais **`RecvBuffer_ResetAll_OnUnknownOpcode @0x00c148b0`** → `RecvBuffer_ResetCursors @0x00c165c0` remet les curseurs read/write des 3 buffers de connexion à 0 = **VIDE LE BUFFER RECV** → tout paquet encore en attente APRÈS l'opcode custom est **JETÉ**.
Au dépôt le serveur envoie `[storageitemadded][0x0F0F prix][delitem]` dans le même flush → le client lit 0x0F0F (notre `PacketBufReaderHook` capture bien la donnée AVANT le reset), puis reset → **le `delitem` est jeté** → l'item n'est jamais retiré côté client.

**POURQUOI ça n'a jamais posé de problème avant** (réponse à la vraie question) :
- Le reset se déclenchait DÉJÀ à chaque paquet 0x0F0x, mais il **VIDE ce qui SUIT** dans le buffer. Nos ZC custom (settings 0x0F05, presets 0x0F07, skilldmg 0x0F09, discord 0x0F08…) étaient **toujours le DERNIER paquet du flush** → le reset vidait un buffer déjà drainé = AUCUN effet visible.
- Le **prix-à-l'ajout storage** (`clif_bourgeon_storage_prices` dans `clif_storageitemadded`) est le **1ᵉʳ cas où un 0x0F0x est INTERLEAVÉ AVANT un paquet critique** (delitem) dans le même flush → 1ʳᵉ fois que le reset jette quelque chose d'important → 1ᵉʳ bug visible.
- Les anciens custom "vérifiés sûrs" (0x0BFB..0x0C23) sont **IN-PLAGE** (≤0x0C35) → dispatch+return, **jamais** ce chemin de reset. Le passage à 0x0F00+ (pour fuir les collisions) les a mis sur le chemin du reset **sans que ce soit testé en interleave**.

**FIX (client, root cause)** : hook sur `RecvBuffer_ResetAll_OnUnknownOpcode`. `PacketBufReaderHook` pose `g_suppress_buffer_reset=true` quand l'opcode lu est un `s_reader_dispatch_opcodes_` enregistré ; `BufferResetHook` **skippe le reset** dans ce cas (sinon reset natif conservé pour un VRAI opcode inconnu = récup d'erreur). ⇒ nos 0x0F0x n'écrasent plus le flux → **interleave sûr pour TOUS les ZC custom** (pas juste le prix storage). Config : `RecvBufferReset: 0x00c148b0`. Fichiers : rag_connection.cc/.h, configuration.h.
**Ghidra** : `RecvBuffer_ResetAll_OnUnknownOpcode`/`RecvBuffer_ResetCursors` renommés + plate-comments (dont le PIÈGE), commentaire sur la branche hors-plage de la boucle recv.
**Leçon** : ne JAMAIS supposer qu'un opcode >0x0C35 est neutre — il déclenche un vide-buffer. Sans le hook, un 0x0F0x custom ne doit être QUE le dernier paquet d'un flush. Avec le hook, plus de contrainte.

## 🔴🔴 Une branche périmée fabrique de fausses découvertes (2026-08-30)

En prenant trois opcodes pour le [[project_mvp_tracker]], j'ai « découvert » que
`kNextFree` annonçait `0x0F2B` alors que `0x0F2B`..`0x0F2F` étaient pris côté
serveur, et j'ai consigné la leçon « `kNextFree` ment, l'autorité est
`packets_struct.hpp` ». **C'était faux.** `master` déclarait déjà ces cinq
opcodes et disait bien `0x0F30`. Ce n'est pas le fichier qui mentait : ma branche
avait **179 commits et huit jours de retard**.

➡ **Avant d'auditer une ressource PARTAGÉE — opcodes, ids de fenêtres, clés de
réglages, catalogue i18n —, vérifier que la branche est à jour :**
`git rev-list --left-right --count master...HEAD`. Sur une branche périmée,
l'audit ne décrit que sa propre vieillesse, et les « corrections » qu'on en tire
écrasent du travail neuf. Dans la même session, deux autres trouvailles étaient
du même tonneau : l'infrastructure d'icône de menu maison et le correctif
`ReadBossCell` (cellule 0,0) existaient déjà sur `master`, en mieux.

`kNextFree` reste utile et fiable **quand la branche est à jour** ; il vaut
`0x0F33` depuis le carnet de chasse MVP (0x0F30 CZ, 0x0F31 et 0x0F32 ZC).

## 🔴🔴 Une COMMANDE plutôt qu'un opcode, quand l'action se dit en une ligne (2026-09-02)

Livré `CZ_BOURGEON_RELOAD_MAP` 0x0F33 pour que GreyWorld fasse recharger la carte, puis
**retiré le jour même** sur la question de l'utilisateur : « implanter un paquet
était-il vraiment nécessaire ? ». Il avait raison. Remplaçant : une atcommand
`@refreshmap` envoyée par le chat.

✅ **Ce que la commande a de plus, et l'opcode ne pourra jamais avoir :**
1. 🔴 **Un CZ inconnu du serveur DÉCONNECTE la session** (rAthena `set_eof` sur tout
   opcode absent de sa table) — donc client et serveur DOIVENT partir ensemble. Une
   commande inconnue se répond en une ligne de chat : les deux côtés se déploient
   indépendamment.
2. **Elle DIT pourquoi elle refuse** (`clif_displaymessage`), là où un handler de paquet
   échoue en silence et laisse l'interface deviner.
3. **Le joueur peut la taper**, donc la tester sans l'interface — et le staff aussi.
4. Zéro opcode consommé, zéro entrée dans `packets_struct.hpp` / `clif_packetdb.hpp`.

⚠ Le prix : la ligne passe par le pipeline de chat (donc `atcommandlog`), et il faut
ajouter la permission au groupe 0 de **`conf/import/groups.yml`** — sans quoi personne
ne peut l'appeler.

➡ **Règle** : réserver l'opcode custom à ce qu'une commande ne sait pas faire — du
BINAIRE (listes, structures, réponses volumineuses), du PUSH serveur→client, ou une
fréquence qui ne supporte pas le chat. Une ACTION ponctuelle déclenchée par le joueur
est une commande. Côté client, le chemin est `ChatWindow::SendTextNow` (elle ARME,
`FlushPending` envoie hors frame ImGui ; la chatbox est toujours instanciée, même quand
le joueur garde la native).

⚠ À NE PAS CONFONDRE avec [[feedback_player_setting_persistence]], qui rejette la repose
d'un RÉGLAGE par le chat au login : c'était une COURSE au démarrage, pas une action
voulue par le joueur à l'instant où il clique.

## Related

- [[project_20250716_re]] — Ghidra session, function addresses
- [[reference_moonlight_server]] — server-side packet definitions
- [[feedback_build_and_git]] — ne pas bump le hash en dev
- [[feedback_build_and_git]] — build Release deploy + réécrit le hash
