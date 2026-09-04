# Outillage de débogage de Bourgeon

> Journal du chantier. La fiche de mémoire `feedback_debug_tooling` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.


Comment instrumenter et observer le client. Deux outils complémentaires :
**LogDiag** pour une capture fiable dans la durée, **x32dbg** pour une inspection
immédiate sans build.

## 1. Instrumenter = `LogDiag()`, pas `LogInfo()`

Demandé par l'utilisateur le 2026-07-04, précisé le 2026-07-11 : pendant une
session de débogage/RE, **`LogDiag()` est un outil de première main**. C'est le
canal dédié au débogage — `LogInfo` reste pour l'information normale destinée à
l'utilisateur.

C'est propre, sans crash, ça survit d'une frame à l'autre, et Bourgeon possède
déjà les hooks utiles : l'instrumentation est une ligne.

- Journal écrit dans `E:\Nouveau dossier\Moonlight-Destiny\bourgeon.log` — le lire
  après que l'utilisateur ait reproduit l'action.
- Points de hook prêts pour du paquet :
  `RagConnection::SendPacketHook(int len, char* packet)` (tout paquet SORTANT) et
  `PacketBufReaderHook` (tout paquet ENTRANT), dans
  `src/ragnarok/rag_connection.cc`.

### 1 bis. 🔴 Une trace de RE ne survit PAS au chantier

Demandé le 2026-08-16 : **en fonctionnement normal, `LogDiag` ne doit rien dire.**
Une ligne écrite sur le chemin nominal (« pose », « auto », « envoyé ») est du
spam permanent dans le journal de TOUS les joueurs, au niveau warn de surcroît —
et un diagnostic qu'on ne peut pas éteindre finit par masquer ce qu'il devait
montrer. Le chantier fini, chaque trace doit être **retirée ou reconditionnée**.

Le patron retenu (déjà appliqué dans `discord_relay.cc`, puis `palette_inject` /
`style_sync`) : **une preuve UNE FOIS par session, puis seulement l'anormal.**
- garde `static bool` / `std::set` pour ne le dire qu'une fois (par session, par
  GID, ou par CORPS — jamais par joueur quand vingt joueurs partagent le défaut) ;
- **mettre le calcul de diagnostic DANS le `if`** (hachages, formatage) : il ne
  coûte rien seulement s'il n'est pas fait ;
- ce qui mérite vraiment une ligne, c'est le **refus muet** — un `return false`
  sans trace laisse une panne invisible (`ApplyRecipe` qui refuse) — ou une mesure
  hors plage (couverture quasi nulle), avec un seuil assez bas pour n'avoir aucun
  faux positif ;
- un geste explicite du joueur (clic « Appliquer ») est rare : `LogInfo`, pas
  `LogDiag`. Idem pour un apprentissage RÉUSSI (l'opcode de `NetPing`).

Mesurer plutôt que deviner : `sed -E 's/[0-9]+/N/g' bourgeon.log | sort | uniq -c
| sort -rn` classe les lignes par récurrence et désigne les vrais coupables.

## 2. `LogDiag` est du spdlog : `{}`, jamais `%s` / `%d`

🔴 `LogDiag`, `LogInfo`, `LogError`, `LogDebug` (`utils/log_console.h`) sont des
macros **spdlog**. Avec un gabarit printf, spdlog n'a aucun emplacement à
remplir : il imprime la chaîne **littérale** et jette silencieusement les
arguments. Ni avertissement de compilation, ni erreur à l'exécution — juste une
ligne inutile, découverte après un cycle de build complet :

```
[Bourgeon] [warning] [Avatar] job=%d classe=%d | arme='%s' ...
```

➡ `LogDiag("[X] a={} b={}", a, b)`.

Pour un chemin du GRF, passer par `ro::Cp949ToUtf8` (`ui/ro_imgui.h`, en-tête
léger, aucun include ImGui) : la console en jeu est de l'ImGui et n'affiche que de
l'UTF-8, la console texte est déjà en `SetConsoleOutputCP(CP_UTF8)`, et l'atlas
contient les 11 172 syllabes hangul. **Convertir à la source, jamais dans le puits
de journalisation** — une même ligne mélange nos libellés français, déjà UTF-8, et
les octets CP949 du chemin.

## 3. `LogDebug` doit rester compilé HORS de la Release

`LogDebug` est `((void)0)` en Release (actif seulement sous `BOURGEON_DEBUG`).
**Ne pas le transformer en appel toujours évalué.**

~35 sites d'appel (22 dans `d3d9_hook.cc`, 9 dans `proxy_idirectdraw.cc`) passent
des arguments qui **déréférencent des pointeurs** ou n'ont de sens que dans
certains états. Avec la macro no-op ils ne sont jamais évalués ; les rendre
toujours actifs a provoqué un `0xC0000005` pendant l'init/le rendu — l'utilisateur
l'a diagnostiqué comme « le define de debug ».

Pour de la verbosité à l'exécution, le niveau de journal (`log_level` dans
`bourgeon_settings.yaml` ou `--loglevel=`) n'agit que sur
`LogInfo`/`LogWarn`/`LogError`, toujours compilés. Toute demande de « rendre les
logs de debug basculables en Release » exige d'abord un audit de **chaque**
argument de `LogDebug`.

## 4. Aucun anti-debug : attacher x32dbg SOI-MÊME

🔴 Correction utilisateur du 2026-07-04 : **il n'y a PAS d'anti-debug sur ce
client.** Attacher x32dbg ne ferme ni ne plante le jeu. Les épisodes anciens
(« le jeu s'est fermé », lectures aberrantes) venaient du pont x32dbgMCP
capricieux et/ou d'un breakpoint chaud par frame.

**Autorisation permanente** (reconfirmée le 2026-07-10, pour tout le projet) :
dès qu'une tâche a besoin de debug live et que `get_status` montre
`debugging:false` alors que le client tourne, **l'attacher moi-même — ne pas
demander.** L'utilisateur oublie parfois.

1. `Get-Process Moonlight-Destiny` → PID. Exe = `Moonlight-Destiny.exe`, base
   **0x400000**, dans `E:\Nouveau dossier\Moonlight-Destiny\`.
2. `attach <PID hex>` via `execute_command` du pont (il parle à la GUI x32dbg même
   quand `debugging:false`). PID décimal → hex.
3. Après attache, sonder `get_status` jusqu'à stabilisation ; les lectures
   statiques (mémoire/registres/désassemblage/modules) marchent en `running:true`.

**Précautions (bonnes pratiques, pas de l'anti-debug) :**
- Le pont est **mono-thread** et historiquement capricieux : **sérialiser** tous
  les appels x32dbg (jamais en agents parallèles) ; pour run/step, pousser via les
  outils dédiés puis sonder `get_status`, pas un `execute_command` bloquant.
- Préférer un breakpoint **FROID** (fonction atteinte seulement sur l'action du
  joueur) à un chaud (par frame/par paquet). Sur un chaud, armer depuis un froid
  et **supprimer** le chaud juste après la capture.
- Lire une struct d'UI exige que la fenêtre soit OUVERTE en jeu (le pointeur
  d'inventaire `0x0131f6bc` est nul fenêtre fermée) — c'est l'utilisateur qui
  ouvre.
- 🔴 **Une pause prolongée DÉCONNECTE le client** (payé le 2026-08-16). Le fil
  principal pausé n'envoie plus son keep-alive : au bout de quelques pauses
  enchaînées, le serveur coupe et le client bascule **in-game → login** sans
  rien dire. Le symptôme est traître pendant un diagnostic : un breakpoint sur
  une fonction in-game qui se déclenchait au début cesse de se déclencher, et on
  croit à une régression alors qu'on a soi-même changé l'état observé.
  ⇒ Sur un bug d'affichage/d'état, **capturer les mesures qui comptent en
  quelques pauses courtes**, et relire `[0x0121333c]` (mode courant) avant toute
  conclusion pour vérifier qu'on observe encore le même mode qu'au départ.
- 🔴 **Un watchpoint sur un objet du TAS est invalidé par toute recréation de
  l'objet** (payé le 2026-08-01). Un `bph <CGameMode+off>, w, 4` devient FAUX dès
  un changement de map ou une reconnexion : le `CGameMode` est détruit puis
  RÉALLOUÉ ailleurs (vu en live : `0x2262AF18` → `0x228F7A20`) et l'allocateur
  RECYCLE l'ancien bloc. Les déclenchements suivants sont des **faux positifs sur
  de la mémoire étrangère** — ici il a « attrapé » un buff self-cast sans rapport,
  et la conclusion tirée était fausse.
  ⇒ **Après chaque transition (warp, reconnexion, retour char-select)** : relire
  `[0x0121333c]` (`g_pCurrentMode`), comparer à la base connue, et si elle a bougé
  `bphwc <ancienne>` puis reposer. Valider la nouvelle base par
  `[[vtable]+0x18] == 0x00c86740` (`CMode::SendMsg`). Pour du durable, préférer un
  breakpoint sur le **CODE** qui écrit (adresse fixe dans l'exe) plutôt que sur la
  DONNÉE du tas.

## 5. 🔴 Jamais de `memory_search` sur une grande plage

**`mcp__x32dbg__memory_search` sur une grande plage TUE x32dbg.** Constaté le
2026-08-06 : `memory_search(start=0x47000000, size=0x2000000 /* 32 Mio */, ...)` a
fait crasher x32dbg sur « **49000000 bytes — Could not allocate memory** ». Le jeu
attaché est tombé avec lui, et **tout l'état live préparé par l'utilisateur**
(fenêtres ouvertes, menu déployé) a été perdu.

Le plugin lit la plage demandée d'un bloc dans un buffer côté debugger avant de
chercher : la taille est allouée telle quelle, en plus des tampons internes. Sur
un jeu 32 bits déjà gourmand, quelques dizaines de Mio suffisent, et l'échec n'est
pas rattrapé.

- Pour retrouver une structure, **chaîner les pointeurs** (parcourir la liste/map
  depuis son conteneur) plutôt que balayer le tas — c'est ce que fait le natif.
  Lire d'abord la fonction d'accès (`ActorList_FindByGID` : sentinelle
  `*(liste+0x10)`, nœuds `{next@0, prev@4, valeur@8}`, clé à `acteur+0x110`) et
  rejouer son parcours à coups de `read_memory` de 12 octets.
- Si un motif est vraiment nécessaire, le borner à **quelques centaines de Kio**
  sur une région déjà identifiée — jamais une plage « au cas où ».
- 🔴 **L'état live est un bien périssable préparé par l'utilisateur** : capturer
  d'abord toutes les lectures qui comptent, tenter le risqué après.

## 6. Écran noir : les signes vitaux, dans cet ordre

Établi le 2026-08-16 sur un « jeu bloqué sur écran noir, x32dbg ne signale rien ».
**Cause trouvée ce jour-là : `Present` renvoyait `D3DERR_DEVICEHUNG`
(`0x88760874`)** — l'intuition « perte de device » de l'utilisateur était BONNE.

🔴 **Ne pas écarter la perte de device sous prétexte que le client est en D3D9Ex.**
C'est le raisonnement qui a fait perdre une heure ici : un device Ex ne rend
jamais `D3DERR_DEVICELOST`, mais il rend `DEVICEHUNG` (0x88760874) et
`DEVICEREMOVED` (0x88760870). 🔴 Et **l'absence d'événement TDR au journal système
ne prouve RIEN** : le hang est signalé au device sans reset global journalisé —
recherche vide ≠ absence (cf. [[feedback_absence_needs_measurement]]).
⚠ `DEVICEHUNG` ne se répare PAS par `Reset` : sur Ex il faut DÉTRUIRE et recréer
le device entier. Donc **rien à restaurer en live** — ni x32dbg, ni Alt+Entrée,
ni la fenêtre. Seule issue : relancer le client.

1. **Gel ou pas ?** `bp win32u!NtUserPeekMessage`. S'il tombe, la boucle de
   message vit → ce n'est pas un deadlock. Compléter par `ThreadState` du fil GUI
   (`(Get-Process …).Threads`) : `Running` = pas bloqué.
2. **Quel mode tourne ?** `[0x0121333c]` = mode courant, et son nom de map est
   juste après à `0x1213340` (`"login.rsw"` = revenu au login, donc DÉCONNECTÉ).
   Identifier la classe par sa vtable : `vt+4` = la boucle du mode (`Run`),
   `vt+16` = **la frame**. 🔴 `vt+4` n'est appelée qu'UNE fois et *contient* la
   boucle — un breakpoint sur son entrée ne se déclenche jamais et ne prouve
   RIEN. C'est sur `vt+16` qu'il faut mesurer.
3. **Le client rend-il ?** bp sur le `vt+16` du mode. Pour CLoginMode
   (`0x00d272e0`), le rendu est gardé par `dword_122B468` (0) OU
   `!renderer->vt+16`, qui vaut `*(renderer[1]+0x500) == 0` — le rendu n'a lieu
   que si ce champ est **non nul**.
4. 🔴 **LA mesure qui tranche : le `HRESULT` de `Present`.** Y aller DIRECTEMENT,
   c'est 6 lectures et ça donne la cause exacte. Chemin vers le device :
   `[0x124F2C8]` (singleton SpriteTexFactory) → **le renderer** ; `renderer[1]` =
   l'objet porteur du device (se valide seul : son `[0]` **est le HWND** du jeu) ;
   `porteur[2]` = l'`IDirect3DDevice9Ex`. Sa **vtable est COPIÉE SUR LE TAS** (ne
   pas s'en alarmer, les entrées pointent bien dans d3d9.dll) ; les slots utiles :
   `[16]`=Reset, `[17]`=Present, `[42]`=EndScene — s'ils pointent dans `ddraw.dll`
   (0x57280000+), les hooks Bourgeon sont en place.
   Poser un bp sur `vtable[17]` (= `Hooked_Present`), lire `[esp]` = adresse de
   retour dans l'exe **et `[esp+4]` = le device** (confirme la chaîne), puis
   rebreakpointer sur l'adresse de retour et **lire EAX** = le `HRESULT`.
   ⚠ `bp win32u!NtGdiDdDDIPresent` est un mauvais test isolé : un `Present` qui
   échoue n'atteint JAMAIS le noyau, donc « aucun hit » ne veut pas dire « le
   client ne présente pas » — il présente et se fait refuser.
   ⚠ Indice gratuit et fiable : **le GIF recorder de Bourgeon ne produit rien**
   ⇒ `Hooked_Present` n'aboutit pas (le callback de capture y vit).
   ⚠ **OBS qui capture du noir prouve qu'une surface existe et qu'elle est vide** —
   ce n'est pas « rien n'est présenté ». Et `PrintWindow` rend du noir sur une
   surface DirectX même quand tout va bien : mesure INUTILISABLE ici, passer par
   `CopyFromScreen`.
5. **Ce que voit l'utilisateur** : capturer les écrans (`CopyFromScreen` par
   `Screen.AllScreens`) et les regarder. Ça vaut dix questions — c'est ce qui a
   montré que la fenêtre était visible et noire, bureau normal autour.

Discriminant gratuit à demander AVANT tout ça : **l'overlay ImGui est-il
visible ?** Overlay visible + jeu noir = rendu du jeu. Tout noir, overlay et
curseur compris = présentation.

🔴 Restaurer une fenêtre du jeu depuis l'extérieur : `ShowWindow(SW_RESTORE)`
échoue en silence ; `PostMessage(hwnd, WM_SYSCOMMAND, SC_RESTORE)` passe. Et
`IsIconic` **vrai en même temps que** `GetForegroundWindow() == hwnd` n'est pas
une contradiction à exploiter : c'est un état transitoire, pas une preuve que
quelqu'un re-minimise (vérifier par `bp user32!ShowWindow` avant d'y croire).

---

Voir aussi [[reference_x32dbgmcp_bridge]], [[reference_dll_watchpoint_veh_diag]],
[[feedback_re_method]], [[feedback_build_and_git]],
[[feedback_absence_needs_measurement]].

## 🔴🔴 Le client MASQUE ses propres crashes : exiger un dump COMPLET

Mesuré le 2026-08-26, plusieurs heures perdues dessus.

Le client installe son `TopLevelExceptionFilter` (`sub_6AC9B0` sur le 20260707).
Quand il attrape une exception, **il plante A SON TOUR** en formatant son rapport
(dans `ucrtbase`, sur une conversion numerique). Windows n'enregistre donc que ce
**SECOND** crash : l'Observateur d'evenements et le minidump accusent
`ucrtbase.dll`, ce qui n'a **aucun rapport** avec la vraie cause.

➡ **Symptome a reconnaitre : fenetre blanche puis fermeture, sans message.**

**La marche a suivre :**

1. `Get-WinEvent -FilterHashtable @{LogName='Application'; ProviderName='Application Error'}`
   donne le module + l'offset. ⚠ Si c'est `ucrtbase`, c'est probablement le
   gestionnaire, PAS la cause.
2. Activer les dumps COMPLETS (le minidump ne suffit pas : `dbghelp` ecrase le
   haut de la pile en l'ecrivant, les frames du client ne commencent qu'a
   `esp+0xE48`) :
   `reg add "HKLM\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" /v DumpType /t REG_DWORD /d 2 /f`
   ⚠ GLOBAL : toute app qui plante fera des centaines de Mo. Retirer apres.
   Dumps dans `%LOCALAPPDATA%\CrashDumps`.
3. Retrouver le PREMIER `EXCEPTION_RECORD` en balayant la memoire du dump :
   `scratchpad/findexc.py` (structure x86 : code(0), flags(4), record(8),
   address(0xC), nparams(0x10), info[](0x14) ; ne garder que les
   `ExceptionAddress` dans `.text`).

🔴 Le lecteur de minidump maison est dans le scratchpad (`dmp.py`) : exception,
contexte, modules, pile. ⚠ Offsets qui m'ont coute 3 essais :
`MINIDUMP_MODULE.ModuleNameRva` = **+20** ; `ThreadContext` de l'exception stream
= **+160** (`MINIDUMP_EXCEPTION` fait 152 octets) ; `CONTEXT` x86 : eip=+184,
esp=+196, eax=+176, ecx=+172. Ouvrir en **mmap** (un dump complet fait ~1 Go).

