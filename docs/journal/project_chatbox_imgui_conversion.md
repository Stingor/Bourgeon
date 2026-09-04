# Conversion de la chatbox en ImGui

> Journal du chantier. La fiche de mémoire `project_chatbox_imgui_conversion` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.

## 🔴🔴 CRASH BLOQUANT À L'ENTRÉE EN JEU — diagnostiqué 2026-08-04, NON CORRIGÉ

Mesuré au débogueur (x32dbg attaché sur le crash), pas déduit :
* **`0x008E17B3` → `mov edx,[eax+0xF4]` avec `eax = 0`.**
* `g_pNewChatWnd` (**0x0131F6B0**) vaut **NULL** — la native est morte — et part
  quand même comme `this` dans **`sub_8E1730` (0x008E1730)**, appelée par
  `sub_CB7510` à `0x00CB7875`.
* Déclencheur : `RecvLoop_DispatchPackets` → `sub_CB7510` = handler de
  **ZC_BROADCAST2 (0x01C3)**, le message de bienvenue du serveur. Entête 16
  octets (opcode, len, couleur RGB aux octets 4-6, type/size/align/y), message à
  +16. Il arrive à CHAQUE entrée en jeu ⇒ crash systématique.
* 🔴 **`sub_8E1730` a 15 appelants** (transformation des tags/liens d'un texte) :
  `sub_CAFD00`, `sub_CB7A40`, `Actor_OnMsg_AppearanceEffects`,
  `QuestTracker_DrawContent`, les cinq `UIRichTextBox_Layout*TagLinks`… Tous sont
  des crashs latents tant que la native est détruite. Le broadcast est seulement
  le premier rencontré.
* **Correctif retenu (non appliqué)** : détour `__fastcall` sur `0x008E1730` ; si
  `ecx == 0`, construire la sortie comme COPIE de l'entrée via le copy-ctor natif
  `0x004E52A0` (`void* __fastcall(void* dst /*ecx*/, void* edx, const void* src)`,
  vérifié au désassemblage), puis renvoyer ce pointeur ; sinon chaîner l'original.
  ⚠ La sortie arrive NON INITIALISÉE chez l'appelant : la laisser telle quelle
  fait planter le `std_string_dtor` suivant, et la rendre VIDE afficherait un
  message vide (l'appelant *move* le résultat dans son buffer). Une fois l'appel
  neutralisé, le natif poursuit jusqu'à `ChatAction(..., 1, msg, color, 0x19)` ⇒
  `ChatActionFilter` ingère la ligne normalement.

Cf. [[reference_native_window_toggle_router]] et
[[feedback_native_replacement]] : c'est le même piège, côté PAQUET
cette fois — la native morte est encore déréférencée par des handlers natifs.

## ÉTAT (2026-08-04) — PHASE 1 LIVRÉE, compile propre, PAS ENCORE testée en jeu
**`src/features/windows/chat_window.{h,cc}`** (plugin `ChatWindow`, enregistré
dans `LoadPlugins` juste après ChatTweaks ; clés yaml `chatwnd_*`, panneau
Interface → Chat). Détails dans **docs/chatbox_re.md §8.7bis**.
- 🔴 **Le détour de `ChatAction 0x00a4ad20` a DÉMÉNAGÉ** : il porte maintenant les
  DEUX besoins — filtre de messages système (`kBlockedMsgs`, autrefois
  `Bourgeon::InstallChatMessageFilter` dans bourgeon.cc, **supprimé de là**) ET
  l'ingestion. Il n'y a qu'un jeu d'octets à cette adresse : deux détours
  concurrents = le second posé gagne, le premier meurt en silence.
- Modèle : anneau de lignes {runs pré-analysés, texte nu, sender, couleur, type,
  heure}, cap 100..5000, mutex. Parse À L'INGESTION (jamais par frame) + hauteur
  de repli mémorisée par ligne ⇒ une ligne hors écran ne remesure rien (c'est le
  chat qui a offert au projet sa famille de freezes).
- Canaux/filtres lus dans les registres (std::map MSVC sous SEH) ; repli
  « Public » qui accepte tout. Liens `<ITEML>` → `[Nom]` + icône, clic DROIT =
  desc via `itemcell::DeferDescById`. Texte du fil converti par `ro::LocalToUtf8`
  (code-page du client, PAS CP949 en dur).
- Zone de log au fond SOMBRE dans une fenêtre RO claire : les couleurs envoyées
  par le serveur (blanc par défaut) sont pensées pour le fond noir du natif.
- **À vérifier en jeu** : noms d'onglets = vrais canaux du registre + respect des
  filtres de la 0x84 ; libellé des liens `<ITEML>`.
- ✅ **Vérifié EN JEU (capture 2026-08-04)** : les onglets portent bien les vrais
  canaux du registre (`Battle Log` / `Regular Chat`) — la lecture du std::map est
  BONNE.
- 🔴🔴 **Le « bug d'encodage » était un bug de PARSEUR** : le chat remplaçait
  l'octet 0xA0 (NBSP latin-1, hérité de npc_dialog) APRÈS conversion UTF-8, où
  c'est l'octet de continuation du « à » (C3 A0) ⇒ séquence invalide ⇒ `�`. Les
  « é » (C3 A9) passaient — c'est le signe qui a tranché. Neutraliser le NBSP
  AVANT la conversion, sur les octets du fil.
  Le texte du fil est converti par **`ro::WireToUtf8` = 1252 EN DUR** (ajouté au
  toolkit) : l'encodage du fil est une propriété du SERVEUR, pas de la machine —
  ni `LocalToUtf8` (servicetype client) ni `CP_ACP` (locale système, 949 chez qui
  l'a réglée en coréen) ne décrivent la donnée.
- 🔴 **La couleur d'une LIGNE est un COLORREF `0x00BBGGRR`** (ce que le client
  passe à GDI), pas du RGB : le rouge arrive en 0x0000FF et se rendait BLEU.
  Les codes `^RRGGBB` DANS le texte, eux, sont bien en RGB. Deux fonctions
  distinctes dans chat_window.cc : `LineColorToImU32` / `HexRgbToImU32`.
  `LocalToUtf8` reste bon pour ce que le CLIENT a chargé (DB d'items, msgstring,
  noms d'onglets relus de ChatWndInfo_U.lua).
  ⚠ `rodex_window.cc` et `npc_dialog_window.cc` ont chacun leur copie locale
  `AnsiToUtf8` en **CP_ACP** : même bug latent (accents mangés dans le courrier et
  les dialogues), à basculer sur `ro::WireToUtf8`.

## PHASE 2 PARTIELLE (2026-08-04) : ça ENVOIE, et le skin est fait
- **Envoi** = copie fidèle de `ChatMacro_SendEmotionHotkeySlot 0x00a47400` (le seul
  chemin d'envoi sans fenêtre). 🔴 **`g_ChatWordFilterEnabled 0x0131f6c4` n'est PAS
  un booléen** : c'est le POINTEUR du gestionnaire de filtre, et le `this` de
  `Chat_ContainsForbiddenWord`. Whisper = `SendMsg(11, char* nom)` (désassemblé,
  PAS une std::string). Commandes : `sub_D7F1A0(ctxKey, texte)` d'abord, puis
  `ChatCmd_LookupSlashCommandTable(texte, &id, std::string args[3])` +
  `SendMsg(0x2A, id, args)`. Joué par `FlushPending` depuis `OnProcessInput`,
  JAMAIS pendant la frame (modale bloquante possible).
- **Skin** = `ro::BeginRoChatWindow` (3e style de cadre après window/desc) : pas de
  barre de titre, fond translucide sombre réglable, onglets peints à la main,
  `btn_resize`. Les bitmaps du chat natif sont relevés dans docs/chatbox_re.md
  §8.7bis (battle_option, wnd_mini, dialog_btn0/1/2, sys_base, stickoff — le fond
  et les onglets, eux, n'ont AUCUNE image).
- 🔴🔴 **`ChatAction` N'EST PAS le chokepoint** (démenti EN JEU 2026-08-04) : la
  fenêtre ImGui n'y voyait qu'une PARTIE des lignes du natif (5 sur ~14 à
  l'entrée en jeu ; annonces et « Party Setup » manquantes, types pourtant
  cochés). Le vrai point de dépôt est le **`case 0x25` du WndProc**
  (`UISubChatWnd_AddLine` n'a que 3 appelants, tous des WndProc de chat).
  ⇒ **DEUX sources arbitrées par `g_pNewChatWnd 0x0131f6b0`** : native vivante =
  ingestion par le WndProc (hook déjà posé par ChatTweaks sur 0x008fc220, via
  `chatwnd::IngestNativeLine`) ; native absente = ChatAction seul. ⚠ Au WndProc
  l'ordre est `(texte, couleur, TYPE, sender)` — type et sender INTERVERTIS par
  rapport à ChatAction.
- Diagnostic intégré (case « Diagnostic » des réglages) : ignore les filtres,
  préfixe le type `tNN`, et affiche vues/retenues/en mémoire — c'est la mesure
  qui distingue « jamais ingérée » de « ingérée puis filtrée ».
- Reste pour la bascule totale : empêcher `MakeWindow(1)`/0x84 de naître, et
  reprendre l'ENTER-focus (aujourd'hui la native l'a encore) ; puis phase 3 =
  `chat.cc` → `chat_tweaks.cc`.

**Objectif** : convertir totalement la chatbox native en ImGui. **RE COMPLÈTE**
(passe finale 2026-07-30) consignée dans **`docs/chatbox_re.md`** — le livrable
de référence. Client 20250716. IDB renommé+commenté (WndProc, ChatAction,
Chat_HandleChatMessage, SendMsg cases, UIBattleMsgOptionWnd, loaders Lua…).
Complète [[project_chat_plugin]] / [[project_chat_item_icons]] /
[[reference_chat_clear_history]] / [[reference_ui_window_manager]].

## Résolutions clefs de la passe finale (tout est détaillé dans la doc)
- **msg 0x25** : `(this, p1, 0x25, texte=p3, COULEUR=p4, TYPE=p5, sender=p6)` ;
  `ChatAction(mgr, action, texte, couleur, sender, type)` (6 args). Sender extrait
  sur « ` : ` » (word 0x3A20) pour les types {1,3,4,0x15}. Broadcast = type 0x19
  ou (type 0 && couleur 0xFF).
- **Table de filtre** : octet **`node+0x2C+type`** du registre
  `g_ChatChannelRegistry 0x015faadc` (clé int +0x10, nom SSO +0x14, 25 octets
  +0x2C). **Enum complet des 25 types** dans la doc §3.1.1 (0 Public, 1 Public
  Chat, 2 Whisper, 3 Party, 4 Guild, 5 Alliance, 6 item, …, 0x10 Battle msg,
  0x11 Party battle, 0x15 Clan, 0x19 broadcast).
- **Envoi** : ENTER (WndProc case 6/0xB8) → whisper si box destinataire
  `main+0xC0` non vide (`SendMsg(11,nom)`) ; `/cmd` →
  `ChatCmd_LookupSlashCommandTable 0x00d5e590` → `CMode::SendMsg(0x2A, cmdId,
  args)` → **`Chat_HandleChatMessage 0x00c7a460` = l'exécuteur des 280 commandes**
  (case 0 = texte tapé, préfixes %/$/#, mode `g_ChatInputTargetMode 0x015ff838`).
  Paquets : public 0xF3, whisper 0x96, groupe 0x108, guilde 0x17E, clan 0x98D,
  bataille 0x2DB, alliés 0xBDD. Gate Berserk partout.
- **⚠ CORRECTIONS d'anciennes croyances** : `main+0xC0` n'est PAS un bouton envoi
  (= box destinataire whisper) ; `main+0x13C` n'est PAS un autocomplete
  (= UICashEmotionMiniWnd) ; fenêtre 0x33 n'est PAS un « setup chat »
  (= UICandidateWnd, candidats IME, ChatAction 8/9/10) ; **msg 0x73 / ChatAction
  0x13 = VOIE MORTE** (le WndProc l'ignore) ; un **log continu Chat.txt existe**
  (cmd 0x68 « Chatting Auto Save », `g_ChatAutoSaveOn`) en plus du /savechat
  ponctuel ; l'ancienne table de filtre « node+0xB+type » était fausse.
- **Fenêtres annexes** : 0x84 = `UIBattleMsgOptionWnd` (25 cases = 25 types,
  écrit node+0x2C+i) ; 0x1A = `UIComboBoxWnd` (dropdown générique : cibles
  whisper via bouton 0xE1, mode d'envoi via 0xEE → msg 0x27, l'input se recolore
  par mode) ; détachée `UIChatWnd` : resize snap 32px [280..512]×[74..384],
  renommage inline, msg 0x75 détacher / 0x76 recoller.
- **Persistance** : `SaveData\ChatWndInfo_U.lua` écrit par le **dtor**
  (`SaveChatWndInfo 0x008f9d00`) — CanDragWnd + ChatSubWnd_N {pos, TabState
  On/Off, TabName, option1..25} ; relu en EXÉCUTANT le .lua (C-funcs Lua
  `SetSubChatWndList` 0x00a9cf70 / `SetSubChatWndOption` 0x00a9d460). **Max 10
  canaux.** Macros EmotionHotkey (10, `g_EmotionHotkeyMacros 0x015fb398`,
  envoyées par `0x00a47400`) + WhisperBlockList en JSON séparé.
- **Globaux utiles** : `g_pNewChatWnd 0x0131f6b0` (ptr direct, plus besoin du
  cache ecx), `g_ChatSavedPosX/Y/TabRows 0x0131f518/51c/534`, `g_ChatCanDragWnd
  0x015faaec`, `g_BattleChatModeOn 0x015ff824`, `g_ChatWordFilterEnabled
  0x0131f6c4` (+ `Chat_ContainsForbiddenWord 0x00a23180`, refus modale 0xE53).
- **Confirmé statiquement** : une chat non peinte REÇOIT toujours msg 0x25
  (aucun test de visibilité) → la stratégie « natif vivant mais muet » tient.

## Stratégie conversion (doc §8, DÉCISION UTILISATEUR 2026-07-30) : REMPLACEMENT TOTAL
**Le natif devient CODE MORT quand l'interface moderne est active** — pas un
masquage : les fenêtres ne sont JAMAIS créées (bloquer MakeWindow(1) et 0x84).
**⚠ Piège central** : fenêtre absente ⇒ `ChatAction` action 1 empile dans la
file `mgr+0x4C4` SANS LIMITE ⇒ il FAUT hooker `UIWindowMgr_ChatAction
0x00a4ad20` (qui donne texte+couleur+sender+TYPE directement), pas seulement
supprimer la fenêtre. Modèle ImGui = ring buffer à nous ; canaux/filtres
amorcés depuis les registres (peuplés au boot par le .lua INDÉPENDAMMENT des
fenêtres, géométrie détachées dans la valeur à +0x34..+0x40) ; envoi = Option B
(pending text + SendMsg 0x2A/11, lookup commandes callable) ; ENTER-focus à
réimplémenter ; seule vraie perte = IME (UICandidateWnd) → ImGui IME si besoin.
Persistance : `SaveChatWndInfo 0x008f9d00` ne dépend PAS des fenêtres mais du
dtor → l'appeler NOUS-MÊMES (compat ChatWndInfo_U.lua conservée). Phase 3 :
`chat.cc` → **`chat_tweaks.cc`** — les retouches natives (largeur, icônes ^i,
timestamps, cache mesure GDI, fonds) NE SONT PAS retirées : elles restent les
OPTIONS des joueurs en chat natif (interface moderne OFF) ; deux modes
exclusifs. L'ancienne stratégie « natif muet » (early-return Paint/Draw +
lecture RAW tab+0x100/+0x10C/+0x118) reste utilisable comme phase de validation.
UISubChatWnd hérite de **UITextListView** (OnMsg 0x00860d50).

## ✅ Préfixes `%` / `$` / `#` repris (2026-08-15)

🔴 **Notre envoi avait copié le chemin des MACROS, pas celui de l'ENTRÉE.**
`ChatMacro_SendEmotionHotkeySlot` appelle directement la commande de canal ;
l'ENTRÉE, elle, passe par `SendMsg(0x2A, 0)` → **case 0**, et c'est LUI qui lit
les préfixes. Résultat : `%coucou` partait littéralement, en public.
`ResolveSendCommand` (chat_window.cc) rejoue le case 0 en C++ — détail dans
docs/chatbox_re.md §8.5. Ce qu'il ne faut pas re-découvrir :
* la bascule **QUITTE** le canal quand c'est déjà celui de la combo (« $ » en
  mode Guilde → public) ; elle ne DÉSIGNE un canal qu'au mode « Tous » ;
* Ctrl / Alt / Verr.Maj **enfoncées** valent `%` / `$` / `#` — relevées à la
  VALIDATION, pas au flush (une frame plus tard, la touche est relâchée) ;
* un préfixe seul n'envoie rien ; `g_BattleChatModeOn 0x015ff824` (commande
  slash 186) court-circuite tout ; mode 4 = alliés `SendMsg(0x14A)`, qui
  manquait aussi.

🔴🔴 **Les trois caractères se LISENT DANS L'EXE** (immédiats `0x00c7a7eb` `%`,
`0x00c7a822` `$`, `0x00c7a89a` alliance), JAMAIS en dur : le patch WARP
`AllianceChatHotkeySelector` réécrit celui de l'alliance et **Moonlight l'a posé
sur `^`** — c'est ce qui rend le `#` aux **charcommands rAthena**
(« #pseudo @cmd »). Les coder en dur d'après l'IDB (vanilla) les a cassés le
2026-08-16, le jour même du fix. C'est le piège de
[[reference_ida_is_vanilla_warp_patches]] payé une troisième fois : avant de
recopier un garde natif, regarder si un patch est posé DESSUS.

## ✅ `/savechat` repris (2026-08-15)

La commande était devenue MUETTE : `ChatLog_SaveAllToFiles` (0x00907030) parcourt
les sous-fenêtres natives (`mgr+0x1C8`), que l'interface moderne détruit — elle
sortait donc sans écrire ni se plaindre. L'**action 6** est interceptée dans
`ChatActionFilter`, au même endroit que 3 et 14, et l'export repart de notre
modèle : un fichier par onglet dans `<racine>\Chat\` (même dossier et même
rotation `_001` que le client), lignes filtrées par `ChannelAccepts` — ce qui est
enregistré est ce qui est affiché.
⚠ Différences assumées : horodatage par ligne (notre modèle le porte), BOM UTF-8
(sans lui le Bloc-notes lit les accents en ANSI), et un message qui NOMME le
dossier.
🔴 Écriture DIFFÉRÉE d'une frame (`pending_save_log_`, atomique) :
`ChatActionFilter` peut tourner sur le fil réseau, et `channels_` n'appartient
qu'au rendu.
## Membre du groupe « Interface moderne » (2026-08-18)

Plus de case « Chatbox ImGui » : `imgui_enabled_` est écrit par
`SetModernInterface` (avec dialogue NPC, descriptions item/skill et livre —
clés renommées `itemdesc_imgui`/`skilldesc_imgui`/`bookwnd_imgui`, défaut
false). `chatwnd_imgui` garde sa clé (défaut inchangé) et VOTE à la
réconciliation. Les réglages fins du panneau sont grisés hors groupe
(BeginDisabled fermé avant l'unique return). Le menu contextuel y était DÉJÀ.
