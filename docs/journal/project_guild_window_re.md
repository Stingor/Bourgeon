# Fenêtre de guilde et onglet Guilde de la fiche de personnage

> Journal du chantier. La fiche de mémoire `project_guild_window_re` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

🔴 **NATIF MORT (01/08, commit 71fb67f)** — en interface moderne, la fenêtre de guilde
ne s'ouvre plus : raccourci ET bouton du menu d'icônes atterrissent sur l'**onglet
Guilde de la character sheet**.
⚠ **DEUX fenêtres**, pas une, choisies sur `g_Own_GuildId` **0x0159c230** :
`!= 0` → **0x3B**, `== 0` → **0xD4** (« pas de guilde » / création). Les deux chemins
font ce test au même endroit : raccourci `UIWindowMgr_DispatchHotkeyBehavior`
@0x00a455cf, bouton cmd 373 `UIMenuIconWnd_OnMsg` @0x00814c6d (`cmovl`).
⚠ **0x3B ≠ 0x3C** : 0x3B est le CONTENEUR qu'on bascule, 0x3C..0x42 sont ses
panneaux d'onglet — détruits avec lui (chacun est une native de plus, et une native
masquée garde le clavier). Aucun ne porte de donnée manquante : roster, relations et
bannis sont dans des globals, et les POSTES — seule donnée que le client ne gardait
QUE dans sa fenêtre — sont déjà parsés depuis les paquets par l'onglet.
Voir [[reference_native_window_toggle_router]], [[project_character_sheet]].

---

RE fait 2026-07-03 (2 agents + arbitrage live). Tout renommé/commenté Ghidra. Base 0x00400000.

**Famille de fenêtres** : toutes dérivent de `UIGuildWnd_ctor_Base` @0x00919eb0 (vtable base 0x0103afcc). Slots vtable : **DrawContent=+0x50, OnMsg=+0x94, GetActiveTabIndex=+0xd4, SaveGeom=+0x34**. Carte autoritative (id = 0x3c+rang, slot mgr = 0x320+rang*4) posée en commentaire sur le ctor de base.

**IDs (corrigés via FindWindow 0x00a47b90 + mapping onglets ; agent A avait mis 0x39/0x33 par erreur, 9↔c)** :
- **0x3c UIGuildTotalInfoWnd** = CONTENEUR à onglets (overview : nom/level/exp/emblème + listes alliés/hostiles). vtable 0103b0a8, DrawContent 0x00923a10, OnMsg 0x00927700, slot mgr +0x320, champ this+0xf4=index onglet actif.
- 0x3d MemberManage(0103b184/DC 009241b0/OnMsg 00928200/+0x324, CONF FindWindow) · 0x3e PositionManage(0103b260/00924840/00929c90/+0x328) · 0x3f Skill(0103b33c/00924fc0/0092a380/+0x32c, CONF FindWindow) · 0x40 AllyGuild(0103b418/009234d0/00927480/+0x330) · 0x41 InfoPopup-edit(0103b4f4/00924730/00929a70/+0x334) · 0x42 Banished(0103b5d0/009252f0/0092ab10/+0x338). Panneaux 0x3c-0x42 = par loi lineaire id=0x3c+rang, slot=0x320+rang*4 (calee sur 3 pts FindWindow).
- AUX non confirmes (via map ; NE PAS supposer 0x44/0x45 car **0x45=UIMessengerGroupWnd**) : CreateGuild 0103b6ac/00928190, AllyRequest 0103b784/009273c0, Notice 0103b85c/00927590, NoticeSub 0103b934/0092aae0, Leave/BanReason 0103ba0c/00927170. La base à onglets est apparentée au Messenger (ne pas confondre). Confirmer live avant câblage.

**Onglets** : barre dessinée par `UIGuildWnd_DrawTabFrame` @0x009256f0 (6 onglets, largeur 0x40, y=0x12, **stride 0x42**, actif (2,2)/inactif (0xe,6)). Clic = msg 0x45 → `UIGuildWnd_OnMsg_Base` @0x0092abd0 ferme panneau courant puis `MakeWindow(0x3c+index)`. Masque privilèges onglets = `g_GuildWnd_TabPrivilegeMask` 0x0159c22c. Ouvrir sur un onglet = écrire `g_GuildWnd_ActiveTabIndex` 0x0159c230 avant création. Création via `UIWindowMgr_MakeWindow` 0x00a39340 (TotalInfo ctor appelé @0x00a3b9bc).

**Données de guilde** (PAS dans g_CGuildMgr) : globals scalaires `g_GuildInfo_*` @0x0159c1xx (GuildId/Level/MemberCount/AvgLevel/Exp/TaxPoint/IsMaster/GuildName/Notice/emblemVersion). Roster membres = liste chaînée `g_Guild_MemberRosterList` @0x0159c26c (node : +0x0c type, nom SSO à +0x10 ; enreg. membre stride 0x68 v104 avec **ContributionExp, OnlineFlag, PositionID, Class/Job, BaseLevel, Name[0x18]** — souvent NON affichés). Listes `g_Guild_AllyRelationList` 0x0159c274, `g_Guild_BanishedMemberList` 0x0159c27c. Postes : grille 6 col (largeurs DAT_01204e44/48/4c/50/54). Handlers réseau renommés `GuildNet_*` (ZC_GUILD_INFO 0x00cbfcd0, notice 0x00ce23b0, member list 4 variantes, skillinfo, emblem img 0x00d76b10).

**Emblème** : `g_CGuildMgr` @0x01254d70 = en fait **CGuildEmblemMgr** (map charId→version @+0x08, map fichiers .ebm @+0x14, download chunké, max 0xc800). `CGuildEmblemMgr_GetEmblemPath` 0x0061d370. Contrainte **24×24** de l'emblème imposée dans UIGuildTotalInfoWnd_OnMsg case 0x27/0x3d (tex[0x45]==0x18 && tex[0x46]==0x18) ; sélecteur scanne `<client>\emblem\*.bmp/*.gif`. Opcodes emblem-sync minimap custom 0x32c8-0x32cd.

**Cmds serveur** (g_UICommandDispatcher+0x18) : 0x67 expulser, 0x68 changer postes, 0x6a bannir, 0x6d/0x6e créer, 0x6f req alliance, 0x70 quitter, 0x76 maj info, 0x43 skill-up guilde.

**Leviers custo** (voir [[reference_window_position_persistence]] pour ajouter id 0x3c à window_pos_tweaks ; [[project_status_tweaks_plugin]] pour le pattern relayout DrawContent vtable-swap +0x50) : relayout onglet Info, colonnes membres (afficher contribution/online/grade), sélecteur d'emblème (assouplir 24×24), persistance position.

## ✅ VÉRIFIÉ LIVE x32dbg (2026-07-11) — intégration guilde dans character_sheet

Corrige des LABELS Ghidra FAUX (le pkt ZC_GUILD_INFO avait nom↔maître inversés) :
- **Nom de guilde = `CGuild+0` @ `0x0159c188`** (std::string SSO/heap). ⚠ PAS 0x0159c1a0 (qui = « MasterName »/autre). Confirmé : 0x0159c188 = « Moonlight-Destiny ».
- **Niveau @ `0x0159c1e8`** · **est-maître @ `0x0159c23c`** (0/1) · **GuildId @ `0x0159c230`** (clé emblème ; label Ghidra « g_GuildWnd_ActiveTabIndex » FAUX = c'est le guildId).
- **Roster = liste chaînée dans `CGuild` (`g_CGuild_Roster` 0x0159c188)** : `_Myhead` (sentinelle) @ **CGuild+0xdc = 0x0159c264**, compteur @+0xe0. Node : **next@+0, prev@+4, payload@+8**. **Payload** (= node+8, cf. `FUN_009194d0`) : **AID @+0**, GID/CID @+4, **nom @+8** (std::string), ints @+0x20..0x30, **NOM DE POSTE (« rang ») @+0x34** (std::string, ex. « Handyman »), string @+0x50, ints @+0x68..0x74.
- **Position du joueur** = parcourir le roster, matcher `payload+0 == mon AID` (`session().aid()`), lire la string `@ node+0x3c` (payload+0x34). Alternative offsets packet (`GuildNet_OnMemberList_v104` 0x00ce3440) : PositionID @+0x16, mais le NOM du poste est déjà dans le node.
- **Emblème** : l'**acteur du joueur stocke le guildId @ `acteur+0x2e8`** (`Actor_SetGuildEmblemVersion` 0x00c58200). Acteur = `*(*(GameMode_GetActive(0x1213338)+0xcc)+0x2c)` (cf. [[project_character_sheet]]). Rendu natif (`UIGuildTotalInfoWnd_DrawContent`) : `GetEmblemPath(g_CGuildMgr=*0x01254d70, &path, (void*)guildId)` 0x0061d370 → `UITextureMgr` load → blit 24×24 ; l'appel déclenche le download si absent. Helper `GuildEmblem_LoadTextureOrRequest` 0x00714e20 (non encore décompilé).
## ✅ CARTE CORRIGÉE (RE IDA 2026-07-26) — onglet « Guilde » du character_sheet

⚠ Les labels IDA/Ghidra de `g_GuildInfo_*` sont **DÉCALÉS d'un champ** : ils viennent du parseur legacy `GuildNet_OnGuildInfo` 0x00cbfcd0 (qui lit `userNum` deux fois). Le serveur moonlight est en PACKETVER **20250716** → il envoie **ZC_GUILD_INFO 0x0b7b** et **ZC_MEMBERMGR_INFO 0x0b7d**, donc ce sont **`GuildNet_OnGuildInfoEx2` 0x00ce1f40** et **`GuildNet_OnMemberList_v58` 0x00ce37c0** qui tournent. Sémantique RÉELLE (recoupée avec `UIGuildTotalInfoWnd_DrawContent`) :
- `0x0159c188` nom de guilde (CGuild+0) · `0x0159c1a0` **nom du MAÎTRE** · `0x0159c258` masterGID · `0x0159c1e8` niveau
- `0x0159c1ec` nb de membres (posé par la liste des membres) · `0x0159c1f0` **connect_member (en ligne)** · `0x0159c1f4` **max_member** (label « AvgLevel » FAUX) · `0x0159c1f8` **niveau moyen** (label « Exp » FAUX)
- `0x0159c1fc` **manageLand** (territoire ; label « Notice » FAUX) · `0x0159c214` **exp** · `0x0159c218` **next_exp** · `0x0159c220/224/228` point/honor/virtue (0 en rAthena)
- Annonce (ZC 0x016f) : sujet `0x0159c1b8`, corps `0x0159c1d0` (`GuildNet_OnGuildNotice` 0x00ce23b0)
- Listes : roster `CGuild+0xdc` = 0x0159c264 (taille +0xe0) · **relations alliés/ennemis `0x0159c26c`** (node+8 guildId, +0xc relation 0=allié/1=ennemi, +0x10 nom) · bannis `0x0159c27c`

**Enregistrement de membre** (node = {next,prev,payload}, offsets NODE-relatifs) : +0x08 AID · +0x0c CID · +0x10 nom · +0x30 job · +0x34 sexe · +0x38 positionID · +0x3c nom de poste · +0x54 niveau · +0x58 mémo (vide en v58) · +0x74 contribution · +0x78 en ligne · +0x7c **lastLoginTime (timestamp Unix)**. Taille 0x78.
⚠ **La liste des membres RECRÉE chaque enregistrement avec un nom de poste VIDE** ; seul ZC_POSITION_ID_NAME_INFO (0x0166) le remplit → le plugin mémorise `positionId -> libellé` et s'en sert en repli.

**Écritures (paquets bruts via SendPacket, structures moonlight/src/map/packets.hpp)** : créer une guilde `CZ 0x0165` (30 o {cid.L, nom[24]} ; Emperium requis si `guild_emperium_check`, réponse `ZC 0x0167` : 0 créée / 1 déjà en guilde / 2 nom pris / 3 Emperium manquant) · droits d'expulsion/invitation = `guild_has_permission` = **masque du POSTE occupé**, PAS le drapeau maître (et le maître ne peut pas être expulsé) · rompre une alliance/hostilité `CZ 0x0183` (10 o {gid.L, relation.L} ; maître seul, refusé en WoE et sur carte guildlock ; le serveur répond ZC 0x0184 qui retire l'entrée) · rafraîchir `CZ 0x014f` {type.L} (0 = infos+alliances+noms de postes, 1 = noms de postes+membres) · quitter `0x0159` / expulser `0x015b` (54 o {gid, aid, cid, motif[40]}) · changer de poste `0x0155` {len, {aid,cid,pos}*} — **position 0 = `guild_gm_change` = TRANSFERT DE LA DIRECTION** (le sous-menu « Changer de poste » part donc de l'id **1** ; le transfert a sa propre entrée, cf. plus bas). Refus du transfert : WoE (`guild_leaderchange_woe: no`), `guild_leaderchange_delay` (1440, comparé à des SECONDES par `clif_parse_GuildChangeMemberPosition`) — les deux en `clif_msg` — et `instance_block_leaderchange`, **muet**. Seul le **CID** compte (l'AID est ignoré sur ce chemin) et la cible n'a pas besoin d'être connectée · inviter par nom `0x0916` {nom[24]} · annonce `0x016e` (186 o {gid, sujet[60], texte[120]}).

**Postes (grades)** : le client ne les stocke **QUE dans la fenêtre native** (liste interne à `UIGuildPositionManageWnd`, window+0x220) — **aucun global**. Solution retenue : `RegisterObserveOpcode` sur les 3 paquets ZC et parsing maison (comme la liste du cash shop) → `ZC_POSITION_ID_NAME_INFO 0x0166` (28 o : id.L + nom[24]), `ZC_POSITION_INFO 0x0160` (16 o : id, droits, rang, part d'exp), `ZC_ACK_CHANGE_GUILD_POSITIONINFO 0x0174` (40 o : + nom). ⚠ Pour un opcode *observé*, `data` pointe sur le champ **longueur** (paquet+2) → décalage paquet −2. Droits = `e_guild_permission` : inviter 0x001, expulser 0x010, entrepôt 0x100. Part d'exp plafonnée par `battle_config.guild_exp_limit` (50). MAX_GUILDPOSITION = 20 ; les listes envoient TOUJOURS les 20 postes.

**Ligne de membre native** (`sub_91C7E0` 0x0091c7e0, appelée par MemberManage_DrawContent) : fond VERT **et** miniature de tête **uniquement si `record+0x70 == 1`** (en ligne). La tête = `Job_GetHeadSpritePath` (__stdcall, rend le **.act**) + `Job_GetBodySpritePath_NoPal` (rend le **.spr** — noms inversés, même piège que les armes) → frame 0 / layer 1 → cellule → atlas. Côté Bourgeon on réutilise la recette éprouvée du char-select (snprintf sur les format-strings CP949 `인간족\머리통\<남|여>\%d_<남|여>.spr/.act` @0x0108F9A0/9BC/9D8/9F4, palette `palette\머리\head_<N>.pal` → **CPaletteRes+0x510** = palette CONVERTIE), extraite dans **`src/ui/head_icon.{h,cc}`** (`ro::DrawHeadIcon`) ; humains seulement (Doram → rien). ⚠ **L'id de coiffure n'est PAS le n° de fichier** pour les 12 premières coupes (le natif passe par une table de noms indexée par la coiffure, cf. `Job_BuildBodyOrHeadSpritePath_impl` + `g_JobName_Male/Female` 0x15fb30c/318) : appliquer le remap `{0,2,1,7,5,4,3,6,8,9,10,12,11}` (identique à `CharSelect::DrawHairIcon`), sinon coupe voisine affichée. Apparence dans l'enregistrement : coiffure node+0x28, couleur node+0x2c, genre node+0x34.

**Implémenté** : onglet « Guilde » de `character_sheet` (emblème, infos, jauge d'exp, annonce éditable par le maître, table des membres triable + menu contextuel copier/changer de poste/**transférer la direction** (2026-08-22, modal à nom retapé)/expulser, sous-onglet **Postes** éditable par le maître → envoi groupé CZ 0x0161, alliés/ennemis, inviter/quitter, **« Actualiser » ancré en haut à droite**). Écrit 2026-07-26, **pas encore testé live**.

## Changement d'emblème (RE IDA 2026-07-26) — le chemin moderne du client est INUTILISABLE

Le client 2025 a **deux** chemins, aucun compris par rAthena :
1. **Service web** : `CEmblemDataMgr_RequestUpload` 0x005c8950 (this = `g_EmblemDataMgr` 0x012517b8, args guildId + `std::string("emblem\\<fichier>")`) → `CEmblemDataUploadAsyncWork_DoWork` 0x005c9960 = POST multipart AID/AuthToken/WorldName/GDID/**ImgType** (3 derniers caractères du chemin en MAJUSCULES)/IMG. Réponse `{"Type":1,"version":N}`. Côté serveur c'est `src/web/emblem_controller.cpp` + `CZ_REQ_ADD_NEW_EMBLEM 0x0b46` — **or la constante 0x0b46 est introuvable dans l'exe** (aucun `mov/push` : le client ne l'envoie pas ici).
2. **Transfert en morceaux `CZ 0x0B36`** : `GuildEmblem_SendImageChunks_0B36` 0x005c8c00 {op.W, len.W, type.W, guildId.L, version.L, data} (type 2 début / 1 morceau ≤1010 o / 0 fin). Réception : `GuildNet_EmblemDownloadStream` 0x00cec810 → `CGuildEmblemMgr_SaveEbmFramesFromBmp`. **rAthena ne connaît pas 0x0B36.**

✅ **VALIDÉ EN JEU (2026-07-26)** : upload via le chemin natif + rafraîchissement par version = emblème dessiné dans ImGui puis publié, visible aussitôt. Le legacy, lui, reste inopérant (détail ci-dessous).

🔴 **VERDICT D'ESSAI (2026-07-26, en jeu)** : le chemin legacy **0x0153 NE FONCTIONNE PAS** sur moonlight — le paquet part bel et bien (`53 01 <len> 78 9C …` vérifié au journal, socket sain, joueur maître, WoE finie, 24 bits accepté) et **rien** ne revient (pas de ZC 0x0152), sans la moindre ligne côté serveur. La fenêtre NATIVE, elle, change l'emblème **aussitôt** — or elle n'envoie pas 0x0153 : elle passe par le **service web**. ⇒ **Bourgeon appelle désormais `CEmblemDataMgr_RequestUpload` 0x005c8950** (`__thiscall(this=*0x012517b8, guildId, std::string("emblem\\<fichier>"))`, string construite par `std_string_from_cstr` 0x00a94930 pour que l'allocateur soit celui du jeu — le callee la détruit) : le fichier doit exister dans `<jeu>\emblem\`, curl le lit lui-même. Renvoie false si un envoi est déjà en cours. Tout le code zlib/0x0153 a été RETIRÉ du plugin.
⚠ **Cache d'affichage** : ne PAS guetter un paquet pour invalider la texture (le chemin web n'en envoie aucun) — lire `CGuildEmblemMgr_GetEmblemVersion` **0x0061d560** (`__thiscall(this, guildId)`) à chaque frame et recharger quand la version change. Couvre les deux protocoles.

**Chemin legacy (documenté, mais mort ici)** : `CZ_REGISTER_GUILD_EMBLEM_IMG 0x0153` = `{op.W, len.W, zlib(BMP)}`. `clif_parse_GuildChangeEmblem` l'accepte (maître seul, refusé en WoE selon `inter_config.emblem_woe_change`), puis `guild_emblem_changed` rediffuse `clif_guild_emblem` **0x0152** à tous les membres — que le client sait toujours traiter : `GuildNet_OnEmblemImg` 0x00d76b10 écrit les octets BRUTS dans `_tmpEmblem\<gid>_<ver>.ebm`, que `GuildEmblem_LoadTextureOrRequest` 0x00714e20 **inflate** avant de le charger en BMP. Le `.ebm` EST donc du zlib(BMP) : même format à l'aller et au retour.
- Côté client 2025, `GuildEmblem_LoadAndCompressBmp` 0x00926e20 (l'ancien préparateur 0x0153) est **du code mort** — mais la compression réutilisable est là : `zlib_compress` **0x00dbd9c0** `(dst, &dstLen, src, srcLen)` et `zlib_compressBound` **0x00dbdb60** (zlib 1.2.13, cdecl).
- **Limites serveur** : `clif_validate_emblem` décompresse dans `buf[1800]` (BMP 24×24 24-bit = 1782 ✓, 8-bit = 1654 ✓, 32-bit = 2358 ✗) et compare **bfSize à la taille réelle** ; `intif_guild_emblem` jette silencieusement au-delà de **2000 octets compressés** ; `mmo_guild.emblem_data[2048]`.
- ⚠ **Transparence** : `conf/inter_athena.conf` de moonlight a `emblem_transparency_limit: 80` → un emblème trop vide est REFUSÉ (`transcount*300/taille_pixels > 80`, triplets de pixels magenta consécutifs). Le plugin reproduit la formule et avertit avant l'envoi. `emblem_woe_change: yes` (changement autorisé pendant la WoE).
- ⚠ **`0x0159c230` = guild id du personnage** (label Ghidra/IDA « g_GuildWnd_ActiveTabIndex » FAUX ; renommé `g_Own_GuildId` dans l'IDB le 2026-07-26).

**Implémenté (2026-07-26, non testé live)** : modal « Changer l'emblème » (clic sur l'emblème de l'en-tête ou bouton, maître seul) à deux onglets — **« Dessiner »** : canvas ImGui 24×24 (crayon/gomme/remplissage + **ligne/rectangle/ellipse** tirés avec aperçu et option « forme pleine », épaisseur 1-3, symétrie, palette + roue + sélecteur « Transparence » (le magenta), annulation par coup, damier = magenta transparent, aperçu 1:1), BMP 24-bit fabriqué en mémoire (`BuildEmblemBmp`), enregistrable dans `emblem\` — et **« Choisir un fichier »** : `<jeu>\emblem\*.bmp` avec vignettes et motif de refus explicite (le serveur, lui, refuse en silence). Décodage BMP 24 ET 8 bits factorisé dans `DecodeEmblemBmp`.

- ⚠ L'acteur du joueur (vtable 0x01094810) a `+0x1c` = **chemin .pal** (pas le nom), et les strings nom/guilde/poste ne sont PAS inline dans ses 1024 premiers octets → passer par le roster (fait) ou un cache nom (non trouvé).
- **FAIT** : `character_sheet` lit nom+position (roster) + niveau/maître ; affiche « GuildName [Poste] » dans l'en-tête gauche.
- **EMBLÈME FAIT (impl. 2026-07-11, à valider live)** : le `.ebm` (`<jeu>\_tmpEmblem\<nom>_<guildId>_<ver>.ebm`) est un **BMP 24×24 24-bit COMPRESSÉ ZLIB** (header `78 9C`) — le TexMgr générique NE le décompresse PAS (retourne texObj w=0). Solution : inflater maison **`src/utils/tinf_inflate.h`** (`tinf::zlib_uncompress`, style tinf domaine public, pas de dépendance) + `LoadEmblemFromFile()` (lit fichier → inflate → parse BMP bottom-up align-4 → couleur-clé magenta → `Overlay_CreateTextureARGB`). `ResolveEmblem` construit le chemin complet via `GetModuleFileNameA` + `\_tmpEmblem\` + nom relatif rendu par `GetEmblemPath`, avec **retry throttlé 3 s** (l'.ebm peut ne pas être encore téléchargé au 1er appel ; la fuite std::string de GetEmblemPath devient négligeable). Rendu = `AddImage 24×24` au coin haut-gauche en-tête. À vérifier live : orientation (sens géré via signe hauteur BMP) + ordre couleur.

## Création de guilde — RE IDA 2026-08-23

**UN SEUL producteur de `CZ 0x0165` dans tout l'exe** : le bloc **0x00c8ee00–0x00c8f05d**,
case **msg 110 (0x6E)** de `CMode::SendMsg`/ProcessInput. Et le **seul appelant de
`SendMsg(110)`** est `UICreateGuildWnd_OnMsg` **0x00928190** (scan `push 6Eh` sur .text) :
**aucune commande de chat n'y mène**. Structure composée : op@+0, `g_Own_CharId`@+2,
`strncpy(nom,23)`@+6, `0`@+29, longueur par `PacketLen_Get(0x165)` — **exactement** ce
qu'envoie `SendCreateGuild` de character_sheet ⇒ en cas d'échec, ne pas soupçonner le paquet.

🔴 **Gardes CLIENT de ce bloc** (elles n'existent PAS côté serveur, donc notre chemin ImGui
les ignore — et le serveur, lui, accepte l'espace) :
`strlen(nom) >= 24` → abandon muet · **espace dans le nom → REFUS** (msgstring 0x97B,
repli « Can NOT use space in Guild Name !! ») · liste de mots interdits `0x0159C2C8`
(msgstring 0xD07) · `[acteur+0x305] == 1` → msgstring 0xA2D, aucun envoi ·
`g_ServiceType == 0xC` → exige un '#' dans le nom (sinon msgstring 0x464).

⚠ **`/guild` N'EXISTE PAS dans ce Ragexe 2025-07-16** : la seule chaîne « /guild… » de
l'exe (vanilla ET Moonlight-Destiny.exe, vérifié au grep binaire hors IDA) est
**`/guildinvite`**. Les 197 commandes slash sont en clair autour de **0xC96400** (offset
fichier) ; ce client est coréen (variantes translittérées `/ruftjd`, `/rlfemcheo`), la
variante anglaise `/guild` documentée par iROwiki appartient aux exe iRO.
🔴 **`/organize` = créer une PARTY**, pas une guilde (cmdId 0x1A, avec `/결성` et `/ruftjd`) —
erreur commise le 2026-08-23, corrigée par l'utilisateur.
Côté serveur, trois refus sont **MUETS** (pas de ZC 0x0167) : carte `guildlock` (msg 228 au
chat), **appartenance à un clan** (`return` sec dans `clif_parse_CreateGuild`), nom vide
après `trim`. Et le flag **2 « nom déjà pris » est aussi rendu quand le nom sort de
`char_name_letters`** (`char_name_option: 1` chez moonlight, pas d'apostrophe dans la liste).
**`@guild` (atcommand) désactive `guild_emperium_check`** le temps de l'appel : sa réussite
ne prouve RIEN sur le chemin joueur.
