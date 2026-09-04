# Cash Emotion (emotes payantes)

> Journal du chantier. La fiche de mémoire `project_cash_emotion_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

# Cash Emotion (emotes payantes) — RE client 20250716

Objectif : debloquer les cash emotes aux joueurs sans achat. **Conclusion : le gate reel est SERVEUR (moonlight), le client n'affiche que l'ownership.**

## Classes / assets
RTTI `CCashEmotionMgr` (instance globale `DAT_0125165c`), `UICashEmotionListWnd` (window id **0x57**, mini id 0x56), `UICashEmotionPack/Piece/SprBtn`, `UIEmotionWnd`. Lua : `Lua Files\CashEmotion\CashEmotionList(_F)`, `CashEmotionListDefine`, `CashEmotionClientSetting`. Sprites : `\cashemotion\*.bmp` (UI) + `\emotion.spr/.act` (frames). Global fenetre : `DAT_0131f86c` (UICashEmotionListWnd), `LAB_0131f4e8` = g_UIWindowMgr.

## Layout CCashEmotionMgr (offsets cles)
- `+0x10` : `map<u16 packId, PackNode>` (tous les packs definis, charges du Lua). Champs node (base = node+0xc region) : `+0x24` reqSkillLevel, `+0x2c` startTime, `+0x30` **purchasedDays** (jours d'usage restants), `+0x1c/+0x20` fenetre de vente (sale start/end).
- `+0x50` : `map<u16 packId, vector<u32 emoteIdx>>` (indices d'emotes du pack).
- `+0x1a4/1a8/1ac` : `vector<u16>` packs possedes.
- `+0x1b0/1b4` : list (packId -> schedule d'expiration).
- `+0x1b8/1bc` : vector packs non-possedes.
- `+0x1c4` : packId selectionne courant ; `+0x1c8` : charId/skillId ; `+0x1cc/+0x1d0` : base-temps serveur.

## Accesseurs (renommes dans Ghidra)
- `CCashEmotionMgr_GetPackPurchasedCount` **0x00592440** — node+0x30. **GATE MAITRE client.**
- `CCashEmotionMgr_GetPackReqSkillLevel` 0x00591810 — node+0x24.
- `CCashEmotionMgr_GetPackEmoteIndices` 0x005914e0 — map+0x50.
- `CCashEmotionMgr_GetPackName` 0x005917c0 — node+0xc (std::string).
- `CCashEmotionMgr_GetPackStartTime` 0x00591cb0 — node+0x2c.
- `CCashEmotionMgr_GetServerTimeNow` 0x00597350 — time32()-mgr+0x1d0-mgr+0x1cc.
- `CCashEmotionMgr_CheckPackDateRange` 0x00593b10 ; `_GetEmotionsForPack` 0x005918c0 ; `_MarkPackPurchased` 0x00594790 ; `_AddPackToPurchasedVector` 0x005937a0.

## Flux UI ownership
`UI_CashEmotion_RebuildPanel_Usable` (0x0078ae90) : pour chaque emote, `*(bool*)(piece+0x37) = GetPackPurchasedCount(packId) > 0`. `FUN_0078e0c0` (0x0078e0c0) affiche periode-d'usage si count>0. Clic pièce = `UICashEmotionSprBtn_OnMsg` 0x00791fd0 (msg6/0xBD -> remonte msg 0xAE au parent avec emoteIdx en +0x7c/+0x80). `CCashEmotion_OnClickEmotionButton` 0x005896b0 : gate `CheckPackDateRange` avant d'ouvrir le pack.

## Opcodes (client 20250716)
- **CZ 0x0BEC** = requete d'ACHAT. Emis dans `UICashEmotionListWnd_OnMsg` 0x0078e2d0 case **0x20e** (bouton buy) apres dialog result 0xbb : `{u16 0xBEC, u16 packId, u16 charId, u8 reqSkillLvl}` via `FUN_00c14920(obj,7,&buf)`. Si reqSkillLevel==0 -> message "FREE_PURCHASE_QUESTION", sinon "PURCHASE_QUESTION" (+ check monnaie Nyangvine `FUN_00737d00(&LAB_015fa3cc, itemId 0x139a)`).
- **ZC buy-success** = `NET_CashEmotion_RecvBuySuccess` 0x00cb13a0 : `+2 u16 packId, +4 u8 hasExpire, +5 u32 expire` -> MarkPackPurchased.
- **ZC owned-pack-list** (login) = `NET_CashEmotion_RecvOwnedPackList` 0x00cb8e20 : `+2 len; count=(len-10)/7; +4 base-temps; +8 short;` puis N records 7o `{u16 packId, u8 flag, u32 expire}` -> AddPackToPurchasedVector. **Source autoritaire d'ownership.**
- **ZC use-fail** = `NET_CashEmotion_RecvUseFail` 0x00cbe930 : `+6` code 1=UNPURCHASED / 2=SKILL_LEVEL / 0=DATE / autre=UNKNOWN. **Le serveur valide l'USAGE, pas que l'achat.**
- Dispatcher cash-emotion non-analyse par Ghidra : region ~0x00ca0800-0x00ca0d00 (appelle MarkPackPurchased 0x00ca0c58, FUN_00cad2d0 x3). FUN_00cad2d0 = setup schedule d'expiration/usable-time apres achat/login.

## LEVIERS pour debloquer sans payer (recommande = SERVEUR)
1. **[SERVEUR #1, propre]** A la connexion, faire emettre par moonlight le paquet owned-pack-list (handler client 0x00cb8e20) avec TOUS les packId (flag=0 => permanent). Debloque partout, UI + usage.
2. **[SERVEUR #2]** Traiter la requete d'achat CZ 0x0BEC comme gratuite (ignorer cout/Nyangvine) et renvoyer buy-success.
3. **[SERVEUR #3]** Cote validation d'usage : ne jamais renvoyer use-fail (skip check ownership) et diffuser l'emote normalement.
4. **[CLIENT, INSUFFISANT SEUL]** Patch `GetPackPurchasedCount` 0x00592440 -> retourner >0 : rend tout "possede" dans l'UI (boutons actifs, selectionnables) MAIS le serveur renverra use-fail=UNPURCHASED a l'usage reel. A combiner avec le serveur.

## Etat cote SERVEUR moonlight (verifie 2026-07-05)
- **L'achat est DEJA gratuit** : `clif_parse_buy_cash_emotion` (src/map/clif.cpp:13183) ACK immediatement `ZC_ACK_BUY_CASH_EMOTION 0x0BED` (has_count=0=permanent) sans cout. Match parfait avec le handler client `NET_CashEmotion_RecvBuySuccess` 0x00cb13a0 (=> confirme opcode buy-success = **0x0BED**).
- Paquets definis (packets.hpp) : CZ_REQ_EMOTION_EXPANSION **0xBE9** (route vers clif_parse_Emotion arg4, ~stub), CZ_REQ_BUY_CASH_EMOTION **0xBEC**, ZC_ACK_BUY_CASH_EMOTION **0xBED**.
- **Le serveur ne gate PAS l'usage** : `clif_parse_Emotion` (clif.cpp:13128) diffuse tout emoticon `< ET_MAX` via clif_emotion, AUCUN check d'ownership, n'envoie JAMAIS de use-fail. `ET_MAX = 108` (enum clif.hpp:269, inclut ET_CUSTOM_1..15 = idx 93-107, probables cash emotes).
- **PAS implemente cote serveur** : le paquet owned-list (handler client 0x00cb8e20) ni le use-fail (0x00cbe930). Leurs opcodes ZC ne sont PAS dans moonlight ; a lire dans le dispatcher client NON-analyse par Ghidra (~0x00ca0800-0x00ca0d00, appelle MarkPackPurchased@0x00ca0c58) => necessite x32dbg attache (client lance) car Ghidra MCP ne desassemble que les fonctions definies.

## Spam chat "Successfully purchased emotion." au login (RESOLU 2026-07-06)
`clif_grant_all_cash_emotions` (LoadEndAck) envoie 16x ZC **0x0BED** au login/warp ; le handler
`NET_CashEmotion_RecvBuySuccess 0x00cb13a0` affiche **inconditionnellement** EMSG_EMOTION_EXPANTION_BUY_SUCCESS
=> spam d'une ligne/pack. Desasm : `MarkPackPurchased(0x00594790)` PUIS bloc message
[**0x00cb13c6..0x00cb13eb** 38o] = lookup EMSG (FUN_00771110) + ajout chat (FUN_00a4ad20, __thiscall
ECX=0x131f4e8) PUIS refresh FUN_00cad2d0. Aucun champ paquet ne conditionne le message.
- **⚠ DEUX emetteurs de la string** (@0x01091ae8, xrefs confirmes) : (1) 0x00cb13c6 dans
  NET_CashEmotion_RecvBuySuccess 0x00cb13a0 = **PAS** le chemin du grant login ; (2) **0x00ca0c5d** dans le
  dispatcher cash-emotion INLINE (0x00ca08xx, juste apres CALL MarkPackPurchased @0x00ca0c58) = **LE VRAI
  handler 0x0BED au login**. Patcher seulement 0x00cb13a0 ne change RIEN (verifie live : bloc bien NOP mais
  spam persiste). Il FAUT patcher 0x00ca0c5d (Ghidra n'a pas analyse cette region -> desasm via x32dbg).
- **Fix RETENU (client Bourgeon)** : `PatchSilenceEmotePurchaseMsg()` (bourgeon.cc Initialize) NOP les **2**
  blocs (0x00cb13c6 ET 0x00ca0c5d), 38o chacun (les 2 CALL __thiscall nettoient leur pile => pas de
  desequilibre), garde ownership + refresh. Sanity p[0]==0x8B (MOV ECX,[imm]) & p[0x21]==0xE8 (CALL).
  => emotes OK, zero chat, meme a l'achat reel.
- **Fix serveur (optionnel)** : gater le grant sur `sd->state.connect_new` (1x/connexion) -> moins de paquets.
- Le levier owned-list silencieux (0x00cb8e20) reste ideal mais son opcode ZC n'est toujours pas resolu.
- Hook login dispo : `clif_parse_LoadEndAck` (clif.cpp:12206).

## RECOMMANDATION FINALE (2 chemins propres)
- **A) CLIENT (Bourgeon DLL) — le plus simple, self-contained, RECOMMANDE** : hooker `GetPackPurchasedCount` 0x00592440 pour retourner une grande valeur => tous les packs "possedes" dans l'UI, joueur selectionne+utilise librement. Le serveur diffuse sans gate (idx cash < 108). Aucun opcode a resoudre. C'est leur propre client.
- **B) SERVEUR auto-grant au login** : emettre le paquet owned-list (tous packs, flag=0) dans clif_parse_LoadEndAck => deblocage silencieux au login. NECESSITE de resoudre l'opcode ZC (attacher x32dbg au client lance, lire dispatcher). Alternative sans opcode = envoyer 0x0BED par pack, MAIS ca spam une messagebox "BUY_SUCCESS" par pack (handler 0x00cb13a0) => a eviter.
- Etat actuel : un joueur peut DEJA tout debloquer gratuitement en ouvrant la fenetre cash-emotion et cliquant "acheter" (0 cout). A/B servent a rendre ca automatique/sans clic.

## VRAI BLOCAGE identifie (test live 2026-07-05) — RELAIS SERVEUR CASSE
Symptome joueur : apres achat, UTILISER une cash emote affiche l'emote de BASE, pas la nouvelle.
- Cause : les cash emotes sont jouees via **CZ_REQ_EMOTION_EXPANSION (0xBE9)**. moonlight route 0xBE9 vers `clif_parse_Emotion` avec pos[0]=4 (clif_packetdb.hpp:2030) => lit le MAUVAIS octet comme un index d'emote basique et rediffuse une emote de base. C'est un STUB casse, pas un souci d'ownership.
- Wire : id d'emote = **u8 (0-255)**. `CZ_REQ_EMOTION 0xBF {i16, u8 id}` (packets.hpp:1406) ; `ZC_EMOTION 0xC0 {i16, i32 GID, u8 type}` (packets.hpp:1973, clif.cpp:10818). `ET_MAX=108`. Les index cash sont probablement >107 => rejetes/tronques par le check `>= ET_MAX` (clif.cpp:13137).
- Achat live confirme : passe par la zone `0xca08xx` (retour capture = **0xca0c5d**, appelle MarkPackPurchased@0x594790), PAS par 0xcb13a0. Le client GATE l'achat par la monnaie Nyangvine (item **6909 Nyangvine_Fruit**) AVANT d'envoyer 0xBEC.
- Debug live RISQUE : figer le client >qq s => **deconnexion serveur** (timeout). Utiliser BP conditionnel + resume immediat, ou capture log.

## CAPTURE LIVE du paquet SEND 0xBE9 (2026-07-05, confirme)
Envoi via helper FUN_00c14920 (HOOKE par Bourgeon : commence par E9=jmp). Layout **6 octets** :
`{ u16 op=0x0BE9, u16 packId, u16 emoteId }`
- 3 captures pack "2023thanksgiving" : packId=**0x0003** (constant), emoteId = 0x2B(43)/0x27(39)/0x28(40).
- emoteId 39/40/43 = slots emotion.spr des emotes "etendues" (merong/shy/sexy) MAIS le pack 3 a son PROPRE spr (via CCashEmotionMgr::GetSprFilePath(packId)) => frame 43 du pack3 = dinde, frame 43 base = sexy.
- Fonction d'envoi (~0xc88300, zone non-analysee) : bâtit le paquet PUIS met `[mgr/wnd+0x284]=1` = flag "EN ATTENTE REPONSE SERVEUR". **Le client N'AFFICHE RIEN en local, il attend.** RecvUseFail (0xcbe930) remet +0x284=0 sur echec.
- moonlight route 0xBE9 -> clif_parse_Emotion (pos[0]=4) => lit emoteId (43) et rediffuse ZC_EMOTION 0xC0 type=43 = emote de BASE (sexy), SANS le packId => le client rend le sprite de base, pas la dinde. **C'EST LE BUG.**

## Confirmation 2 comportements (2026-07-05)
- Cash emote emoteId <108 (ex thanksgiving 39/40/43) => moonlight rediffuse 0xC0 basique => emote de BASE affichee (pas le skin pack).
- Cash emote emoteId >=108 (ex chibi rose) => moonlight DROP (check `>=ET_MAX` clif.cpp:13137) => RIEN ne s'affiche.
- => confirme : le client N'AFFICHE JAMAIS en local, il attend le serveur ; et le vrai sprite (dinde/chibi) vient d'un spr PAR PACK (GetSprFilePath(packId)), donc le packId DOIT etre diffuse. Rediffuser juste l'emoteId brut (meme sans cap) ne suffira PAS a afficher le skin => il faut le ZC cash-play {GID, packId, emoteId}.

## CHAINE DE RENDU CLIENT TROUVEE (live 2026-07-05)
- **Rendu UNIFIE** : `FUN_00c6e310(this=acteur, param_1, packId, emoteId, param_4)` @0x00c6e310. Fait GetEmoteEntry(mgr,packId,emoteId)=FUN_00591400 -> spr=FUN_00592340(GetSprFilePath) + act=FUN_00590fc0(GetActFilePath) -> cree sprite (op new 0x1b8) attache a l'acteur (this+0xcc list) via msg 0x16/0xe. **packId=0 => pack de BASE ; packId=N => pack cash.**
- `FUN_00591400(mgr, packId, emoteId)` = GetEmoteEntry -> entry: sprResId+0x28, actResId+0x2c, name+8, +0x20, +0x24(reqSkill). `FUN_00592340`=GetSprFilePath(sprResId), `FUN_00590fc0`=GetActFilePath(actResId).
- `FUN_00c4aea0` = methode VIRTUELLE acteur `Actor::PlayEmote(this, ..., packId@[ebx+0x14], emoteId@[ebx+0x1c], ...)` (vtables 0x0108f834/0x0109016c) ; appelle FUN_00c6e310.
- Appelant du rendu = handler recv emote a **0xc4b404/0xc4b45e** (retour 0xc4b409), dans le gros dispatcher recv NON-analyse ~0xc4aea0+. `FUN_00591c90` = setup sprite pieces UI (meme GetEmoteEntry/GetSprFilePath).
- **2 captures live (BP sur 0xc6e310)** : emote basique -> packId=0/emoteId=0x1d(29) ; /twirl chibi -> packId=0/emoteId=0x5f(95). **TOUJOURS packId=0** car moonlight envoie ZC_EMOTION 0xC0 (pas de champ packId) => le client rend le frame base, pas le skin pack. CONFIRME le fix = serveur doit fournir packId.

## ✅ RESOLU cote affichage (2026-07-05) — opcode ZC cash-play = **0x0BEA**
Fix moonlight applique+teste live : le client rend la cash emote avec le bon packId.
- **ZC_PLAY_CASH_EMOTION opcode 0x0BEA**, layout `{u16 op, u32 GID, u16 pack_id, u16 emotion_id}` (10 octets). Confirme : BP sur rendu 0x00c6e310 -> packId=3, emoteId=0x2b(43) (au lieu de packId=0). Pas de deco => 0x0BEA correct du 1er coup.
- Patch moonlight (src/map/) :
  - packets.hpp : struct PACKET_ZC_PLAY_CASH_EMOTION + DEFINE_PACKET_HEADER(ZC_PLAY_CASH_EMOTION, 0xBEA).
  - clif.cpp : `clif_play_cash_emotion(block_list& bl, u16 pack, u16 emo)` (clif_send AREA) + `clif_parse_cash_emotion_use(fd,sd)` (RFIFOW pack@2/emo@4 -> broadcast *sd). NB: map_session_data herite de block_list (pas de .bl) => passer *sd.
  - clif_packetdb.hpp:2030 : 0x0BE9 route vers clif_parse_cash_emotion_use (au lieu du stub clif_parse_Emotion).
## ✅✅ DEBLOCAGE COMPLET RESOLU (2026-07-05) — grant serveur au login
LE VRAI GATE d'ownership = **vecteur `mgr+0x1a4` + liste `mgr+0x1b0`** (rempli UNIQUEMENT par MarkPackPurchased). FUN_00593ca0 categorise achete/non a partir de la liste. `GetPackPurchasedCount` (node+0x30) N'EST PAS ce gate => le hook client seul NE debloque PAS (teste : hook installe E9 a 0x592440 mais packs restent locked).
FIX QUI MARCHE = **serveur rejoue le buy-success 0x0BED par pack au login** :
- moonlight clif.cpp : `clif_grant_all_cash_emotions(sd)` boucle pack 1..16, envoie PACKET_ZC_ACK_BUY_CASH_EMOTION {op=0xBED, pack_id, has_count=0, count=0} (has_count=0=permanent) via clif_send SELF. Appelee dans clif_parse_LoadEndAck (apres warping=0) + forward-decl avant la fn. Idempotent (client dedupe le vecteur). Bumper kMaxCashEmotionPack si +de packs.
- Confirme live : packs apparaissent possedes/utilisables. Combine au relais 0x0BEA => skins rendus.
- Le hook client GetPackPurchasedCount (plugin CashEmotionTweaks) est desormais REDONDANT (le serveur fait tout) et peut afficher "periode ~416j" cosmetique ; a retirer optionnellement.

## (obsolete/insuffisant) plugin CashEmotionTweaks
Partie A faite : **plugin Bourgeon `CashEmotionTweaks`** (src/plugins/cash_emotion_tweaks.cc/.h, registre bourgeon.cc + src/CMakeLists.txt). JMP-hook sur GetPackPurchasedCount 0x00592440 (__thiscall via __fastcall shim, trampoline HookManager kJmpHook) : retourne owned>0?owned:9999 => tous les packs possedes => cash emotes utilisables gratuitement en permanence pour tous. Compile OK (ddraw.dll). Deploy quand jeu ferme.
- Data : les packs sont definis dans data\luafiles514\lua files\CashEmotion\cashemotionlistdefine.lua : EMOTION_PACK_INFO {packId, saleType(0=basic/1=cash), prix(10 nyangvine), ?, dateVente, ...}. Editer le Lua NE peut PAS donner ownership (node+0x30 est server-set) NI garder le skin si on passe en basic (routerait via 0xbf/packId0=base). D'ou le hook client.
- Note integrity : le POST_BUILD reecrit le hash ddraw.dll dans la conf serveur => joueurs doivent avoir le nouveau dll (patcher) sinon kick (127.0.0.1 exempt).

## (historique) CE QU'IL MANQUAIT : l'opcode ZC "cash-emote-play"
Le serveur doit diffuser un ZC portant {GID, packId, emoteId} pour que les clients (proprios du pack) rendent le skin via GetSprFilePath(packId). Cet opcode/layout = le handler de RECEPTION cash-play du client (rend l'emote acteur + clear +0x284). PAS trouve : dispatch reception = jump-table indexee (opcode-base), non-analysee par Ghidra ~0x00ca08xx. Pistes : tracer CCashEmotionMgr::GetSprFilePath (lambda RTTI 0122d098) et son appelant (rendu acteur) ; ou casser la jump-table. moonlight NE l'envoie pas donc capture live impossible tant que non implemente.

## PLAN pour que ca marche VRAIMENT (2 parties)
1. **Relais serveur (le vrai fix)** : ecrire un vrai handler pour CZ_REQ_EMOTION_EXPANSION 0xBE9 dans moonlight : lire le vrai id cash et rediffuser ZC_EMOTION 0xC0 avec cet id (contourner le cap ET_MAX sur la route expansion). **A FAIRE : capturer le layout exact du paquet 0xBE9** (BP conditionnel sur FUN_00c14920 helper d'envoi, cond `([[esp+8]]&0xFFFF)==0xBE9`, lire buffer & resume vite) OU trouver le SEND statiquement (perform-emotion, sibling de FUN_0078df60 qui envoie 0xBEC).
2. **Ownership/selection** : soit owned-list serveur au login (opcode non resolu, dispatch jump-table indexee, handler client 0x00cb8e20), soit patch client GetPackPurchasedCount 0x00592440 (Bourgeon DLL). Contourne aussi le gate Nyangvine.
- NB : le patch client GetPackPurchasedCount SEUL ne suffit PAS a corriger l'affichage basique — le relais serveur 0xBE9 doit etre corrige aussi.

Voir [[project_opcode_system]] pour les regles opcodes custom, [[reference_moonlight_server]] pour le cote serveur, [[reference_ui_window_manager]] (MakeWindow id 0x57), [[feedback_debug_tooling]] pour resoudre l'opcode via x32dbg.

## Fusionne depuis project_cash_emotion.md (memo du 2026-06-21, supprime)

Trois details que ce fichier-ci n'avait pas :

- `c_InsertPackInfo` **0x005983E0** (__cdecl, lua_state) — callback Lua appele une
  fois par pack au demarrage ; lit pack_id depuis l'arg 1 et l'insere dans
  l'arbre rouge-noir a mgr+0x10. N'est appele QUE si le serveur envoie les
  donnees de packs disponibles.
- **Fichiers serveur modifies** (fork moonlight/rAthena) : `src/map/clif.cpp`
  (`clif_parse_buy_cash_emotion`), `src/map/clif_packetdb.hpp` (parseable_packet
  0x0BE9 pos=4 et 0x0BEC), `src/map/packets.hpp`
  (PACKET_CZ_REQ_BUY_CASH_EMOTION 7 o / PACKET_ZC_ACK_BUY_CASH_EMOTION 9 o),
  `src/map/clif.hpp` (ET_CLICK_ME..ET_CUSTOM_15, ET_MAX=108),
  `src/map/script_constants.hpp` (export_constant des memes ET_).
- **Types d'emote OBSERVES en log** dans 0x0BE9 (octet a l'offset 4) : 82-87 =
  ET_YUT2..ET_YUT7 (des animees coreennes), 4 = ET_SWEAT, 14 = ET_BIGTHROB
  (animations de base groupees), 96-97 = ET_CUSTOM_4..5 (vraies animations
  premium).
