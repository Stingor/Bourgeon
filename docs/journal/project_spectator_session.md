# Sessions spectateur : le décor vivant derrière l'écran de connexion

> Journal du chantier. La fiche de mémoire `project_spectator_session` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

Chantier ouvert le **2026-08-30**, branche **`map-login`** des deux dépôts
(Bourgeon ET moonlight). Fait suite au décor hors ligne
([[project_login_video_and_map_backdrop]]), dont la limite est nette : hors
ligne la ville est **DÉSERTE** (NPC et mobs viennent du serveur, aucun n'est
dans le `.rsw`). ✅ **Chaîne complète validée en jeu le 2026-08-30** : décor
peuplé → formulaire par-dessus → connexion réelle du joueur.

## L'idée (du joueur) : pas des comptes, des SESSIONS

Rien n'est créé, rien n'est détruit, rien à nettoyer. L'identifiant réservé
`SPECTATOR_USERID` = **`moonlight_spectator`** est authentifié SANS toucher la
base ; le personnage est fabriqué à la volée (slot 0), invisible, muet, immobile.

🔴🔴 **L'id NE DOIT PAS dériver du slot de connexion.** C'était ma V1, et elle
paraissait sûre (deux sockets vivantes ne partagent pas un slot). Elle est
fausse : **le client FERME sa socket de login dès qu'il a la liste des
char-servers**, donc l'OS rend le slot pendant que la session qu'il nommait joue
encore. Le client suivant reçoit le même slot, le même id, et se fait refuser
« déjà en ligne » (**code 8**) — par sa propre session précédente. ⇒
`login_spectator_pick_id()` balaie la plage et prend le premier id absent de
`login_get_online_user` ET de `login_get_auth_node` (les deux se relaient : un id
devient « en ligne » dès l'acceptation du login). Plage en HAUT des comptes
(2900000+, `SPECTATOR_ID_COUNT` 90000) : au-dessus c'est `FIXED_NPC_NUM`.
✅ Deux sessions spectateur simultanées mesurées.

## 🔴🔴 Le volet CLIENT : la déconnexion VOLONTAIRE est la clé

Idée du joueur, et c'est elle qui a débloqué tout le reste. **Au moment de
partir** — et pas avant : la session reste VIVANTE tant que le joueur remplit son
formulaire, donc la ville BOUGE — le décor **rend la connexion** :
`rag::DisconnectFromServer()` = `CRagConnection_GetInstance` 0x00c14d60 +
`OnDisconnect` 0x00c14320 — les deux gestes du [Apply] natif et du case 10011.
**Pas de boîte** : c'est le chemin VOLONTAIRE, pas celui de la DÉTECTION de perte.

Le monde reste à l'écran (le `CGameMode` tourne sans serveur, cf. décor hors
ligne) : la scène se fige juste le temps du basculement. **La seule chose qui
doit être vraie**, c'est qu'aucune session spectateur ne survive à l'instant où
la vraie connexion part ⇒ déconnexion PUIS bascule, dans cet ordre. La sortie
devient une **bascule LOCALE** (`RequestModeSwitch(0, "login")`), qui ne dépend
d'aucune réponse serveur.

⛔ **La voie « sortie propre » est MORTE, ne pas la reprendre** : CZ_RESTART
(cmd 25 type 1) puis retour au login (cmd 10011). Elle traverse le char-select
**DU SPECTATEUR**, et surtout elle ne marchait pas — cf. le piège suivant.

## 🔴🔴 Les quatre pièges, tous mesurés

**1. `ActiveModeSendMsgSafe` passe par le getter GATÉ** (`ActiveModeIfReady`,
0x00a75340 : rend 0 tant que mgr+0x58 != 1). Au char-select il rend 0 ⇒ **la
commande n'atteint jamais le mode**, en silence. `CharSelect::DriveModeCmd` lit
le pointeur BRUT (`ActiveMode()`), d'où son bouton qui marche. `globals.h`
l'écrit noir sur blanc, j'ai quand même pris la mauvaise. **Témoin** : `socket=`
inchangée dans le log alors que le case 10011 commence par déconnecter.

**2. Ne JAMAIS conclure sur l'ENVOI d'un message, seulement sur son EFFET.**
Règle écrite dans mon propre fichier, enfreinte dans la branche de sortie : elle
passait « terminée » dès la commande partie ⇒ le voile tombait, le char-select
ImGui reprenait la main sur un écran natif encore vivant et affichait la liste
qui s'y trouvait — **« Spectator » proposé au joueur**.

**3. La liste de personnages n'est PAS « héritée ».** J'ai perdu deux
allers-retours à corriger un bug inexistant (purge de mode+0x1CC, le nombre de
persos, borne du cmd 8). **Mesuré au désassemblage** : `Net_OnCharList_Parse006B`
pose `count = (len-27)/175` puis `memcpy` depuis le début — tous les chemins
ÉCRASENT (le seul `inc` est l'ajout d'un perso CRÉÉ). La liste survit bien à un
retour au login (`CLoginMode` ne la vide qu'en ENTRANT, 0x00d26bd3) mais ça ne
nuit à personne. Purger « pour être sûr » = un cran trop tard = **char-select
vide**.

**4. Le clavier et les écrans traversés.** Un voile plein écran pendant TOUTE la
séquence (aller et retour) : les écrans RO réagissent aux touches sans consulter
leur visibilité, une Entrée résiduelle sur le char-select du spectateur entre en
jeu sur lui. Gardes à étendre : `MoonlightAuth::WantsKeyboard` (+ InWorld) et
🔴 `CharSelect::NativeScreenHasKeyboard` — dont le filet « la salve du
char-server est passée » est vrai **pour la mauvaise raison** (c'est le
SPECTATEUR qui l'a fait passer).

## 🔴🔴 Faire TAIRE un module, c'est le priver de son TRAVAIL

Le piège le plus coûteux de l'armement automatique, et il se généralise. La
séquence fait taire `MoonlightAuth` pendant qu'elle tourne — légitime, il
dessinerait par-dessus des écrans pilotés. Mais c'est LUI qui franchit le
service-select, et ce franchissement **POSE L'ADRESSE DU SERVEUR**
(`Apply_ClientInfoConnection`), une fois, pour toute la session.

Armée « dès qu'on est dans le mode login », la séquence lui retirait donc son
seul moment utile : aucune adresse, **`socket_fd = -1`**, aucune connexion
ouverte — ni pour le décor, ni pour le login du JOUEUR ensuite. Symptômes
apparemment sans rapport (« Unregistered ID », puis « mot de passe incorrect »
sur un compte valide), et une réparation magique : décocher/recocher la case
rendait la main au formulaire, qui appliquait la connexion.

⇒ **N'armer qu'une fois la fenêtre de LOGIN présente** (pas seulement
`AtLoginScreen()`) : à ce moment le service-select est franchi. Et avant de
museler un module, se demander ce qu'il fait d'autre que dessiner.

⚠ Corollaire mesuré : la session spectateur n'a PAS d'adresse propre, elle
emprunte celle du client. Un décor pointant ailleurs (prod depuis un dev local)
demanderait un choix explicite.

## 🔴 Un réglage qui gouverne l'écran de connexion est LU TROP TARD

`bourgeon_settings.yaml` n'est relu qu'à l'ENTRÉE EN JEU (cf.
`utils/startup_settings.h`) — donc jamais, pour ce qui gouverne l'écran de
connexion. Deux fois le même piège dans ce chantier : la parade de Porings
s'affichait chez des joueurs qui l'avaient décochée, et l'écran de veille
pilotait la caméra du décor avec ses DÉFAUTS (62°) au lieu des valeurs réglées.
⇒ `startup::BoolKey` / `SaveBoolKey` (fichier de démarrage), et pour un réglage
qui vit déjà dans le yaml, en garder un **MIROIR** réécrit dès que la valeur
change — comparer la valeur suffit, personne n'a à prévenir personne.

⚠ Et pour le décor : ce n'est PAS la veille du joueur qui décide de son angle.
Valeurs EN DUR dans `spectator::Camera()` — un écran de connexion est une mise
en scène, la même pour tous.

## 🔴🔴 Les pièges de l'ARMEMENT AUTOMATIQUE (le décor par défaut)

**1. Ne JAMAIS relâcher l'état pendant que le client est DANS LE MONDE.** Le seul
défaut qui abîmait autre chose que l'esthétique : trois chemins d'abandon
(bascule refusée, sortie expirée, échec pendant le chargement suivi d'une entrée
en jeu tardive) passaient `InWorld()` à faux alors que la session tournait ⇒ HUD
complet, le joueur JOUE le spectateur, et plus rien ne ferme la session. Un
abandon en jeu FORCE désormais la sortie ; l'expiration REJOUE (bornée à 3).

**2. La BOUCLE décor ↔ `kDriveLogin`.** Au retour d'une partie, `MoonlightAuth`
garde son passthrough `kDriveLogin`, où il demande `spectator::Leave()` à chaque
frame. Le réarmement relançait en face : entrée/sortie toutes les 3 s, jusqu'au
CRASH du client dans `UINewSelectCharWnd_BuildPage`. ⇒ `BackdropPossible()`
refuse si `auth->IsDrivingLogin()`.

**3. 🔴 Le retour au login depuis le char-select NE CHANGE PAS DE MODE** (état
9/6 → 3). Aucune annonce n'est émise : tout réarmement branché dessus est aveugle
⇒ `spectator::Rearm()` appelée à la main dans le bouton, exactement là où
`MoonlightAuth::RearmWebLogin` l'est déjà, et pour la même raison.

**4. Couvrir l'arrivée plutôt que courir après les frames** (idée du joueur) :
le voile TIENT 500 ms après l'entrée en jeu. Il masque d'un coup le pantin que le
paquet SELF n'a pas encore caché, le HUD natif avant son veto, et la caméra qui
n'a pas pris sa pose. ⛔ La voie « classe invisible dans la liste de persos » n'a
PAS été retenue (cf. le ⚠ dans mmo.hpp — le crash qu'on lui a d'abord imputé
venait d'un char-server non rebuild).

**5. Le veto d'UI suit la SESSION, pas la veille.** `EndNow()` rend la caméra
avant la bascule (après, il n'y a plus de caméra) — ce qui termine la veille, et
l'interface native revenait le temps de la transition.

## 🔴 La méthode qui a fini par payer

**Instrumenter AVANT la troisième hypothèse.** Les logs vont dans
`E:\Nouveau dossier\Moonlight-Destiny\bourgeon.log` (tronqué à chaque
lancement, **lisible directement**) : une ligne par transition d'étape + un
compte rendu par seconde quand une étape s'éternise, avec l'état de TOUTES les
sondes d'écran. Ça a tranché en un essai ce que trois builds de suppositions
n'avaient pas trouvé. La question « uniquement Spectator, ou tes persos EN
PLUS ? » a éliminé une piste entière d'un coup.

## Ce qu'il a fallu, serveur par serveur

**login** — court-circuit dans `login_mmo_auth` (retour **-1**), option
`spectator_enabled` (défaut OFF), `login_spectator_pick_id()`.
**char** — `char_spectator_load()` fabrique le `mmo_charstatus` et l'enregistre
dans le `chardb` ⇒ la suite prend le chemin ORDINAIRE. Court-circuits :
`char_mmo_chars_fromsql`, `char_auth_ok` (+ filet qui LÂCHE une entrée online
périmée au lieu de refuser), `chclif_parse_charselect`. Lieu = `spectator_point`.
🔴🔴 **Pas de liste de persos sans l'état du PINCODE**, même désactivé.
**map** — `sd->state.spectator` dans `pc_authok` juste après le memcpy, avec
`status.option |= OPTION_INVISIBLE` 🔴🔴 **sur `status`, PAS `sc`** (resynchronisé
plus bas) + **exception au contrôle de permission** (le serveur retire l'option à
qui n'a pas droit à `@hide`).

🔴🔴 **Invisible ≠ absent** : ce qui empêche l'ENVOI est
`vd.look[LOOK_BASE] == JT_INVISIBLE` (testé par `clif_spawn` /
`clif_getareachar_unit`), à poser dans **`status_set_viewdata`** ; sinon un
fantôme réserve une cellule de picking. Et **le client ne se cache lui-même que
NOTIFIÉ** ⇒ `clif_changeoption_target(sd, sd)` en fin de `clif_parse_LoadEndAck`.

Fermé aussi : `chrif_save`, `intif_saveregistry`, `intif_achievement_save`,
`send_users_tochar` (elle écrivait en base), `npc_click`/`npc_scriptcont`/
`npc_touch_areanpc`/🔴`npc_script_event` (OnPCLoginEvent est lancé par le
SERVEUR), `clif_process_message`, `is_atcommand`, `clif_parse_WalkToXY`,
`clif_hpmeter`, `map_count_oncell`, `@who`/`@users`, `achievement_update_objective`.

## 🔴🔴 Un id spectateur se BRÛLE côté MAP-server (mesuré le 2026-09-02)

**Symptôme** : le décor cesse de marcher pour TOUT LE MONDE au bout de quelques
jours d'uptime, et **un redémarrage du map-server le répare**. Rien dans aucun
journal, des deux côtés — parce que le refus ne passe que par un `ShowInfo`, et
que les `*-msg_log.log` ne prennent que Warning/Error/Debug. 🔴 Sur ce genre de
panne muette, aller au **tcpdump** plutôt qu'aux hypothèses.

**Mesuré** (tcpdump sur le live, ports login/char/map) : la séquence va jusqu'au
bout côté char — `HC_NOTIFY_ZONESVR` **0x0AC5** avec la map et le port 15121 —
puis le client envoie `CZ_ENTER` **0x0436** et le map-server répond **`0x0081`
code 8** (« server still recognizes your last login ») avant de fermer.

**Cause** : `chrif_save` sort par la branche spectateur **sans rien envoyer à
sauvegarder**, mais il a posé un nœud `ST_LOGOUT` dans `auth_db`. Or ce nœud
n'est retiré QUE par `chrif_save_ack` — l'accusé de réception d'une sauvegarde
jamais demandée — et `auth_db_cleanup_sub` ne purge **jamais** un `ST_LOGOUT`
(il rejoue la sauvegarde, qui retombe dans la branche spectateur). Nœud
immortel ⇒ `clif_parse_WantToConnection` refuse toute session ultérieure sur cet
id, et le login-server redonne le MÊME id (il ne voit pas les nœuds du map).
**Un id brûlé par session, et la panne s'installe pour de bon.**

**Correctif** : rendre l'accusé nous-mêmes, au tick suivant
(`spectator_logout_ack` dans `chrif.cpp` : `chrif_char_offline_nsd` puis
`chrif_auth_delete`). 🔴 **Pas en place** : `chrif_auth_delete` libère le `sd`
que `map_quit` parcourt encore juste après (`unit_free_pc`).

## 🔴 Tout contrôle qui NOMME un joueur doit exclure le spectateur

Le contrôle d'intégrité (`clif_parse_bourgeon_integrity`) traitait la session
spectateur comme un joueur : dès que `min_patch_index` montait d'un cran, elle
se faisait kicker — le décor mourait pendant les heures qui séparent un patch de
son installation — et **chaque tentative écrivait « Spectator (AID 2900000) »
dans le journal**, au milieu des lignes qui nomment les vrais joueurs à
prévenir. Un avis qu'on ne peut plus lire ne sert plus à rien : c'est le dégât
le plus coûteux des deux.

⇒ exemption en tête du handler, et **rien d'autre** :
🔴🔴 `clif_bourgeon_grant_verified` **interroge la base** (`alootid` par
`char_id`) et pousse des données de personnage qu'un spectateur n'a pas. Le
MachineGuid n'est pas enregistré non plus — c'est celui du joueur, qui le
déclarera en entrant, et l'inscrire en ferait un multi-compte de son propre
décor.

⚠ L'exemption posait `has_bourgeon = true` pour esquiver le kick « sans DLL »
15 s plus tard. **Retiré le 2026-09-03** (cf. la revue ci-dessous) : le kick est
maintenant écarté à sa source — `clif_parse_LoadEndAck` n'ARME plus le contrôle
pour un spectateur. Même exemption ajoutée dans le handler de l'ancien opcode
(`clif_parse_bourgeon_integrity_legacy`).

## 🔴🔴 Revue de sécurité du 2026-09-03 — l'identifiant est PUBLIC

`spectator_enabled: yes` est actif en prod, et `moonlight_spectator` est en clair
dans la DLL distribuée à tous (`login_spectator.cc:28`). Le mot de passe est
ignoré. ⇒ **le modèle de menace n'est pas « un joueur curieux » mais « un inconnu
sans compte, avec un script et la doc rAthena »** : il obtient une session
authentifiée jusque sur le map-server, où TOUS les paquets CZ passent sauf les
quinze portes explicitement fermées. Rien à bannir derrière : pas de compte.

**Corrigé le 2026-09-03** (dépôt moonlight) :

1. **Création de personnage** (`chclif_parse_createnewchar`) : aucune garde. Un
   inconnu pouvait faire des INSERT dans `char` avec un `account_id` 2900000+ —
   des orphelins qu'aucune vue par compte ne montre, et surtout un **squat de
   noms** (ils sont uniques serveur-wide), sans plafond : MIN_CHARS par session,
   et une session est gratuite et renouvelable. ⇒ refus `-2` en tête.
   🔴 `char_delete` tient, LUI (`found_char[]` + `WHERE account_id AND char_id`) —
   mais `chclif_delchar_check` passe TOUJOURS pour un spectateur (email et
   birthdate vides des deux côtés) : c'est la seconde garde seule qui protège.
2. **`has_bourgeon` accordé sans contrepartie** : ce drapeau est la SEULE garde
   des 23 handlers `CZ_BOURGEON_*`. L'offrir ouvrait à un inconnu : `bug_report`
   (2 INSERT SQL **+ relais Discord** avec son texte ; le rate-limit de 30 s est
   **par account_id**, or il en obtient un neuf à chaque reconnexion),
   `preset_cmd` (DELETE/INSERT sur `alootid`), `reqtechdata`/`reqmobinfo`/
   `reqitemscript` (**scan complet du mob_db** et 32 Ko de réponse par paquet de
   quelques octets), `SETTING_REFRESH` (un `clif_refresh` complet, sans cooldown),
   `target_info`/`req_status_list` (sonder les joueurs à portée).
   ⇒ le drapeau n'est plus posé. **« Rien n'est jamais écrit pour un spectateur »
   était donc faux en quatre endroits** : `char`, `bug_reports`,
   `discord_outbound`, `alootid`.

3. **Aucune limite : ni par IP, ni de durée** (corrigé aussi le 2026-09-03).
   Les garde-fous rAthena ne servaient à rien : le ban dynamique se déclenche sur
   un ÉCHEC de mot de passe, et un spectateur n'en échoue jamais. Deux bornes, du
   côté qui sait les poser : `spectator_max_per_ip` (login, **1** par défaut : un
   seul décor VIVANT par adresse, décision du joueur du 2026-09-03 — le second
   client se rabat sur l'écran de connexion plat) et `spectator_session_ttl`
   (map, **600 s**, `battle_config`).
   ⚠ Le point à surveiller si le décor manquait chez un joueur SEUL : une session
   tout juste rendue peut se lire vivante encore un instant (le logout doit
   remonter du map). La course est gagnée d'avance — le client rend sa connexion
   AVANT de rebasculer, puis doit reconstruire son écran — mais le symptôme serait
   « décor absent après un retour au login ».
   🔴🔴 **Le refus serveur SE VOIT** : il arrive par le chemin d'erreur du login
   natif (`AC_REFUSE_LOGIN`, une boîte MSI) sur un écran où le joueur n'a rien
   tapé, et le module met ~2,5 s à renoncer (fenêtre de login reconstruite) ou 4 s
   (socket fermée). ⇒ le client tranche AVANT d'essayer, avec un **mutex nommé**
   `Bourgeon.LoginBackdrop.Live` (sans préfixe = local à la session Windows) :
   `LiveSlotTaken` (peek sans effet de bord, appelable par frame) / `ClaimLiveSlot`
   (dans `Begin` seul) / `ReleaseLiveSlot` (dans `FreezeDecor`, avec la connexion).
   🔴 `MaybeAutoStart` pose `g_auto_tried` quand le jeton est pris : sans ça, la
   séquence partirait au moment où la place se libère, **hors de la fenêtre du
   voile** — le joueur verrait son propre formulaire se remplir de
   « moonlight_spectator ». Deux machines derrière une même adresse restent
   l'affaire du serveur (et de sa boîte).
   🔴🔴 **Le client doit se couper AVANT le serveur** (`kDecorLifetimeMs`, 300 s) :
   une déconnexion SUBIE passe par la détection de perte de lien, qui affiche une
   boîte par-dessus l'écran de connexion ; la volontaire ne montre rien. ⇒ **une
   DLL en retard d'un patch verra la boîte** — déployer le client AVANT le serveur,
   ou desserrer le TTL le temps de la transition.
   🔴 Le minuteur du map porte `login_id1` en `data` : un identifiant spectateur
   est RECYCLÉ, sans ce témoin le minuteur d'une session partie couperait celle qui
   a hérité de son identifiant.
   ⚠ `conf/import/login_conf.txt` est **gitignoré** : la ligne
   `spectator_max_per_ip` posée en local n'est PAS dans le dépôt, et le serveur de
   prod garde son propre fichier — c'est le défaut du code qui s'y applique.

4. **Le mutisme ne couvrait que les MESSAGES** (corrigé aussi le 2026-09-03).
   `clif_process_message` ferme chat/whisper/groupe/guilde d'un coup, mais tout ce
   qui atteint un joueur SANS lui porter un message passait à côté. 🔴 Sept gardes
   `sd->state.spectator`, posées dans les fonctions MÉTIER (elles couvrent toutes
   les variantes de paquets, présentes et à venir) : `clif_parse_FriendsListAdd`
   (la pire — elle vise **par NOM**, donc ni distance ni carte ne protègent, et qui
   accepte inscrit un `char_id` fantôme dans sa liste SAUVEGARDÉE),
   `trade_traderequest`, `party_create` (elle **écrit en base**, `intif_create_party`)
   et `party_invite`, `chat_createpcchat` (titre = texte libre diffusé en AREA : un
   micro rendu à qui n'en a pas) et `chat_joinchat` (un salon **liste ses membres**),
   `channel_pcjoin`.
   🔴🔴 Le dernier est le plus important et le moins visible : `channel_autojoin`
   tourne pour TOUTE session, donc le décor entrait dans #main et en lisait chaque
   ligne — **une oreille anonyme sur tout le chat du serveur**, sans compte.
   ⚠ **Pas de garde sur la guilde, délibérément** : la créer demande un Emperium
   qu'un spectateur n'a pas, l'invitation demande d'y être déjà.
5. **Le spectateur AGISSAIT sur le monde** (corrigé le 2026-09-03).
   `clif_parse_TakeItem` : ce qu'il ramassait était **détruit** (inventaire d'une
   session jamais sauvegardée, objet d'un joueur). `clif_parse_ActionRequest` :
   🔴 l'invisibilité n'arrête pas un coup — le monstre répond, et le point de vue
   se battait, voire mourait en emportant le décor. Refusés sur l'ORDRE du client
   (doctrine de `WalkToXY`) : le serveur garde ses moyens de servir le point de vue.
6. **La plage débordait** (corrigé). `spectator_account_id()` bornait à
   `END_ACCOUNT_NUM` alors que `login_spectator_pick_id` ne distribue que
   `SPECTATOR_ID_COUNT` ids ⇒ 10 000 ids (2990000+) réputés spectateurs sans
   l'être. 🔴 Un compte réel qui y aurait atterri : invisible, exempté du contrôle
   d'intégrité, **jamais sauvegardé**. Les deux helpers bornent désormais à
   `BASE + SPECTATOR_ID_COUNT` (`spectator_char_id` n'a aucun appelant, corrigé
   quand même : un helper faux est un piège pour le prochain).
7. **Journal et protocole** (corrigé). `cheat_report` : une entrée par minute et
   par session, via `sd->bourgeon_cheatreport_tick` — 🔴 un champ de SESSION, pas
   une table statique `account_id -> tick` comme celle de `bug_report`, qui
   grandit sans que rien ne la vide. Rejet **silencieux** : journaliser le refus
   serait le flot qu'on coupe. Et `clif_parse_bourgeon_setting` exige enfin
   `has_bourgeon` (il était le SEUL des 23 à ne pas le demander).

**Reste ouvert, sans effet mesurable aujourd'hui** : les trois cas hors table de
`clif_parse_bourgeon_setting` (ALOOT_ID, AFK, REFRESH) ne consultent aucune
permission, là où la table applique `perm_atcmd`. Sans conséquence tant que
`autoloot*` est ouvert à tous dans `groups.yml` (vérifié) — à reprendre le jour
où l'une de ces commandes devient réservée. ⚠ Et la RÉCIPROQUE n'est pas fermée :
un joueur peut chuchoter à « Spectator » ou l'inviter, et la réponse du serveur
révèle qu'une session existe (fuite de présence, sans autre effet).

💡 Le MVP tracker a tenu par accident : sa garde `user_id == 0` bloque groupes,
invitations et favoris — un spectateur a `user_id` 0. Ne pas la retirer.

## 🔴🔴 Audit de sécurité n°2 du 2026-09-03 — six portes que la revue n°1 n'a pas vues

Même modèle de menace (un inconnu sans compte, un script, la doc rAthena).
La revue n°1 a fermé le chat, l'ami, l'échange, le groupe, le salon, le canal,
le ramassage et l'attaque. Elle a raisonné **par famille de fonctionnalité** ;
ce second passage a suivi les **effets** (écrit en base / part vers autrui /
coûte une requête / entre au journal), et c'est ce qui a fait tomber le reste.
✅ **Les six corrigés le 2026-09-03**, dépôt moonlight, 10 fichiers, 15 gardes,
5 commits — la 15ᵉ (`clif_parse_partybooking_join`) trouvée par une revue DES
CORRECTIFS : le booking fermé était le LEGACY (diffusion ALL_CLIENT), le MODERNE
(PACKETVER ≥ 20191204) vise `map_charid2sd` — ni distance ni carte —, inscrit un
char_id dans le vecteur du joueur VISÉ et lui ouvre une fenêtre. 🔴 Son garde-fou
« a-t-il déjà demandé » est indexé par char_id, **qui est recyclé**. Inerte
aujourd'hui (le moderne passe par le service web) : donc inerte PAR ACCIDENT.
⚠ `clif_parse_partybooking_reply` n'a PAS reçu de garde, délibérément — vecteur
vide et pas chef de groupe, deux verrous indépendants ; une garde y serait
décorative.

1. 🔴🔴 **RODEX = un canal de message vers un joueur NOMMÉ, écrit en base.**
   `clif_parse_Mail_beginwrite` (0x0A08) pose `state.mail_writing`, puis
   `clif_parse_Mail_send` (0x09EC) → `mail_send` → `intif_Mail_send` →
   `mapif_parse_Mail_send` → **INSERT dans `mail`**, titre et corps libres,
   `send_name` = « Spectator ». `mail_invalid_operation` ne teste QUE le mapflag
   NORODEX ; `mail_setattachment` passe à vide (0 item, 0 zeny). Freins :
   `mail_delay` 1 s et `mail_daily_count` 100 — **par session**, or une session
   est gratuite. Visé **par NOM** ⇒ ni distance ni carte ne protègent : c'est
   exactement le défaut de `FriendsListAdd`, qui LUI a été fermé.
   ⇒ **cinquième démenti** à « rien n'est jamais écrit pour un spectateur ».
2. **Trois oracles sur l'annuaire des personnages**, tous sans compte :
   `Mail_Receiver_Check` (0x0A13) → `intif_mail_checkreceiver` : nom →
   (char_id, classe, niveau), et réponse immédiate = « en ligne ici » ;
   `SolveCharName` (0x0193 + alias) → `map_reqnickdb` → `chrif_searchcharid` :
   **char_id → nom SANS contrôle de carte ni de distance** (là où
   `GetCharNameRequest`, lui, borne à AREA_SIZE) ⇒ balayage de l'annuaire ;
   et 🔴 côté CHAR-server `chclif_parse_reqrename` → `char_check_char_name` =
   un `SELECT ... WHERE name` **par paquet** — atteignable **dès le char-select**.
   La garde `found_char[i] == CID` ne protège pas : `char_mmo_chars_fromsql`
   pose `found_char[0]` = le char synthétique.
3. **Party booking = une diffusion À TOUT LE SERVEUR.**
   `clif_parse_PartyBookingRegisterReq` (0x0802 et 6 alias) →
   `party_booking_register` → `clif_PartyBookingInsertNotify` →
   `clif_send(..., **ALL_CLIENT**)`, et `party_booking_delete` (map_quit) rejoue
   la même à la sortie : **deux annonces serveur-wide par session**, portant
   « Spectator », plus une entrée dans une liste publique.
4. 🔴 **Aucune borne de durée AVANT l'entrée en jeu.** `spectator_session_ttl`
   n'est armé que par `clif_parse_LoadEndAck` sur `connect_new` — donc côté MAP.
   Une session qui s'arrête au char-select ne connaît que `stall_time`, et un
   `chclif_parse_keepalive` (0x0187) la maintient indéfiniment : elle garde son
   id ET **la place de son adresse** (`spectator_max_per_ip` = 1 ⇒ plus de décor
   pour un client légitime derrière la même IP), et de là elle martèle le 2c.
   ⚠ `conf/import/packet_conf.txt` (**gitignoré**) porte `stall_time: 3600`.
5. **Le journal serveur s'écrit sur commande du client.** Même dégât que celui
   corrigé pour `bourgeon_integrity` : `clif_parse_Adopt_request` (6 octets) sort
   toujours en `CHARACTER_NOT_FOUND` (partner_id = 0) mais **journalise à chaque
   paquet** ; `bg_queue_join_solo` passe le **nom fourni par le client** dans un
   `ShowWarning` (`feature.bgqueue: on`).
6. Mineurs : « Spectator » n'est pas un nom RÉSERVÉ (`char_check_char_name` ne
   réserve que `wisp_server_name`) ⇒ un joueur peut le créer et brouiller
   `map_nick2sd` · `spectator_session_ip` n'est purgée QUE par
   `login_spectator_count_for_ip`, jamais appelée si `spectator_max_per_ip` = 0 ·
   `clif_parse_Emotion` n'est fermé que **par accident** (`basic_skill_check: yes`
   et le Novice fabriqué n'a aucun skill) — le rendre `no` rouvrirait l'AREA.

## 🔴🔴 Ce que les correctifs de l'audit n°2 ont appris

**Une LISTE BLANCHE là où le besoin est minuscule.** Côté char-server le décor
n'envoie que trois paquets (`PING`, `CH_SELECT_CHAR`, `CH_CHARLIST_REQ`) ⇒
`char_spectator_packet_allowed` dans `chclif_parse`, et **tout le reste ferme la
session**. Ça vaut mieux qu'une garde par handler : ce qui sera ajouté demain est
fermé par défaut. 🔴 Refuser en IGNORANT était impossible — un paquet non
consommé reste dans le tampon et la boucle le relit ; le garde-fou du pincode,
juste au-dessus, répond déjà par `set_eof` pour la même raison.
✅ **Mesuré en SSH le 2026-09-03, la seule chose qui pouvait la casser** : le
`conf/import/char_conf.txt` de la PROD porte `pincode_enabled: no` (l.45) — et
l'import gagne sur `char_athena.conf`, qui dit `yes`. Le client ne demandera donc
jamais la fenêtre de PIN, et les paquets pincode n'ont pas à figurer dans la
liste. 🔴 Ce fichier est **gitignoré** : la valeur ne se lit QUE sur le serveur,
et une liste blanche se vérifie contre la conf de PROD, jamais celle du poste de
développement — les deux disaient ici la même chose, mais par hasard.

⚠ La liste blanche N'A PAS ÉTÉ transposée côté map : le décor y a besoin de trop
de choses, et une liste trop stricte casserait le décor de façon invisible.

🔴🔴 **`online_char_data::server` reste -1 pour un spectateur QUI JOUE.** Piège
coûteux, trouvé avant de livrer : `chrif_char_online` n'a **aucun appelant**, et
la seule chose qui met un joueur « en ligne » côté char est `send_users_tochar`
(0x2aff) — qui EXCLUT les spectateurs (revue n°1). Un minuteur de char-select qui
aurait conclu « server == -1 donc encore au char-select » aurait fermé la socket
d'une session en jeu ⇒ `char_set_char_offline` ⇒ **id rendu pendant qu'elle
joue**. ⇒ le minuteur est DÉSARMÉ explicitement (`spectator_charselect_timer`,
patron de `charblock_timer`) dans `chclif_parse_charselect`, et il vérifie
`!= tid` comme témoin. Le TTL de 60 s est une constante du code, pas une conf :
`conf/import/char_conf.txt` est gitignoré, une option n'atteindrait pas la prod.

**Un `sd` de handler `clif_parse_*` n'est JAMAIS nul** : `clif_parse` n'appelle
le handler que si `sd` existe (seule exception `clif_parse_WantToConnection`) et
que `sd->prev != nullptr` (seule exception `LoadEndAck`). Les `if (sd == nullptr)`
qu'on lit en tête de certains handlers sont défensifs, pas nécessaires — inutile
d'en ajouter autour d'une garde spectateur.

**Trouvé en chemin, hors périmètre** : `clif_parse_cash_emotion_use` →
`clif_play_cash_emotion` part en **AREA sans AUCUN cooldown ni contrôle**, là où
`clif_parse_Emotion` a son 1/s et son `basic_skill_check`. Fermé pour le
spectateur ; **reste ouvert pour un joueur ordinaire**.

✅ **Vérifiés et sains** (témoins négatifs, à ne pas re-parcourir) : `searchstore`
(`!sd.searchstore.open` — la fenêtre s'ouvre par NPC), `feature.auction: off`,
les 23 `CZ_BOURGEON_*` (`has_bourgeon` n'est plus posé, `bourgeon_setting`
compris), `is_atcommand`, `clif_process_message` (couvre BattleChat et clan_chat),
les 4 portes NPC, `moveCharSlot` / `delchar` / `char_delete2_req` (leurs SQL ne
touchent aucune ligne), les skills (aucun appris ⇒ `pc_checkskill` = 0),
`chrif_save` + `spectator_logout_ack`, le minuteur TTL et son témoin `login_id1`.

## 🔴🔴 Le CLIENT laissait la porte ouverte : « Save ID » (2026-09-03)

Trouvé par le joueur, en prod. `DriveLogin` écrit `moonlight_spectator` dans le
champ ID **natif** — et le client **MÉMORISE le dernier identifiant saisi**
(case « Save ID ») puis le **repose** dans le champ à chaque reconstruction de
sa fenêtre de login. Le même comportement écrasait déjà NOTRE saisie
(`kLoginSettleMs`) ; il revenait par l'autre bout. ⇒ le repli « Login
classique » s'ouvrait **pré-rempli**, mot de passe ignoré côté serveur, et le
joueur se retrouvait **EN JEU sur le compte du décor**, HUD complet, immobile et
muet, sans rien pour refermer avant la coupure serveur (qui, elle, montre sa
boîte de perte de lien).

⇒ trois gestes, complémentaires (commit `cb2c0bb`) :
· `native_login::ClearLoginIdIf(userid)` — efface le champ ID **seulement** s'il
  porte EXACTEMENT cet identifiant (celui du joueur est le sien). 🔴 Le texte
  d'un `CUIEdit` est la `std::string` à **+0xD8** (`CUIEdit_SetText` 0x008303F0
  assigne `this + 216`) ; comparer sur la **longueur annoncée**, la SSO ne
  garantit rien après.
· `spectator::ScrubNativePrefill()` à CHAQUE battement hors séquence, pas
  seulement au repli : la valeur mémorisée survit à la fenêtre, au retour au
  login et **au lancement suivant** — où le décor peut ne pas s'armer (case
  décochée) et où personne d'autre ne passerait derrière. 🔴 Jamais PENDANT la
  séquence : effacer entre l'écriture et le tir enverrait un id vide, refusé
  sans un mot.
· Éjection d'une session ouverte À LA MAIN : AID dans la plage ⇒ déconnexion +
  retour au login sous le voile. 🔴 **En jeu SEULEMENT** — `g_Account_Aid`
  (globals.h `OwnAccountIdSafe`) n'est JAMAIS effacé : à l'écran de connexion il
  porte encore celui du décor qui vient de fermer, s'y fier éjecterait le joueur
  de son propre login. Le char-select reste au filet serveur
  (`char_spectator_charselect_timeout`, 60 s).

⚠ **Aucune des trois options serveur n'est documentée dans les conf livrées** :
`spectator_enabled` / `spectator_max_per_ip` absents de `conf/login_athena.conf`,
`spectator_session_ttl` vit dans `src/custom/battle_config_init.inc` (défaut 600)
et nulle part dans `conf/battle/`. Un admin qui lit ses fichiers ne sait ni que
la porte existe, ni comment la fermer. À faire.

**La leçon** : un audit serveur ne couvre pas ce que le CLIENT sème. Les deux
revues de sécurité ont fermé ce qu'une session spectateur pouvait FAIRE ; aucune
n'a demandé **comment un joueur ordinaire pourrait en ouvrir une sans le
vouloir**.

## Reste à faire

Armement AUTOMATIQUE au démarrage (le but : le décor dès le lancement) — tout est
prêt, il ne manque que le réglage `login_spectator.enabled` dans
`startup::Section`. Vérifier en base que rien ne s'écrit (compter
`WHERE account_id >= 2900000`). Alimenter le catalogue i18n des nouveaux textes.

Lié : [[project_login_video_and_map_backdrop]], [[reference_moonlight_server]],
[[feedback_rathena_conf_import_overrides]], [[reference_moonlight_server_ssh]],
[[project_charselect_imgui]], [[reference_login_screen_re]], [[project_afk_screen]].
