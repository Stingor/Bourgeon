# Poser et appeler un hook dans le client RO

> Journal du chantier. La fiche de mémoire `feedback_native_hooking` n'en garde que l'état et les règles.
> Extrait le 2026-09-04 de la mémoire ; contenu historique, non revérifié.


Tout ce qui concerne **poser un détour et l'appeler correctement**. Le fil rouge :
un hook mal posé ou mal appelé **ne plante pas** — il ne fait rien, et c'est
beaucoup plus long à trouver.

Pour *remplacer* une fenêtre ou un handler natif, voir
[[feedback_native_replacement]].

## 1. Native muette ? désassembler son premier octet AVANT tout

Quand une fonction native se comporte mal (retour bizarre, aucun effet), premier
réflexe : `disassemble_at` sur son entrée. Si c'est un `jmp` vers la plage de
`ddraw.dll` (base ~0x53C30000 = notre DLL en proxy), le code natif n'est jamais
exécuté et **tout breakpoint posé dedans restera muet**.

Vécu sur « bouton Apply du grimoire sans effet » : breakpoints dans
`UIWndMgr_ShowMessageBoxModal 0x00A31A30`, puis watchpoints matériels sur ses
sorties — rien ne tombait alors qu'elle rendait bien 0xB9. Le premier octet était
un `jmp` vers notre `Detour_ShowModal` (`char_select.cc`), dont un flag collant
(`g_cover_active`) supprimait TOUTES les modales natives. Cinq secondes de
désassemblage contre une demi-heure de RE.

Ensuite : `grep` de l'adresse dans `src/` pour localiser le hook, et **lire le
flag de garde en mémoire** (son adresse est dans le `cmp byte ptr [...]` du
prologue du détour) plutôt que le deviner. Corollaire : un flag « notre UI couvre
l'écran » doit être gardé par un état vérifiable, jamais par un booléen remis à
zéro dans un callback qui peut cesser d'être appelé.

## 2. Nos propres appels SAUTENT notre propre hook

Bourgeon garde un **trampoline** (`*Ref`) pour appeler l'originale, et les
wrappers maison l'utilisent :

```cpp
size_t UIWindowMgr::SendMsg(...)     { return SendMsgRef(...); }  // → l'ORIGINALE
size_t UIWindowMgr::SendMsgHook(...) { ... }                      // ← appels du JEU
```

🔴 Le hook n'intercepte donc **que les appels du jeu**. Une décision posée dans le
`*Hook` ne couvre pas Bourgeon lui-même. Vécu deux fois de suite : le relais
Discord et le DPS meter écrivaient dans le chat par le wrapper, et l'aiguillage
posé dans `SendMsgHook` n'a rien changé — alors que le diagnostic amont était
juste.

Avant d'aiguiller, se demander **qui appelle** : jeu → `*Hook`, nous → le
wrapper, ou les deux. ⚠ Un `LogDiag` côté émetteur ne prouve rien : il dit qu'on
a appelé, pas par où c'est passé. Tracer **dans le hook**.

## 3. Mauvaise convention d'appel = échec MUET

🔴 Avant de déclarer un pointeur vers une fonction du client, **lire son site
d'appel en désassemblé**. Un `mov ecx, <arg>` juste avant le `call` veut dire
`__thiscall` — le décompilateur écrit `__usercall ...@<ecx>`, qui se traduit en
`__thiscall`, jamais en `__cdecl`.

Vécu le 2026-08-15 sur `SpriteTexCache_ReloadAll` (`0x00568B30`), déclarée
`void(__cdecl*)(void*)` : elle lisait un ECX de passage, **sortait par son test de
liste vide, et ne se plaignait pas**. Le réglage de finesse des textures était
bien écrit en mémoire — donc actif au redémarrage — mais rien ne bougeait à
l'écran. **« Ça marche au reboot mais pas tout de suite » désigne exactement ce
genre d'appel qui n'a pas eu lieu.**

- ⚠ Le symptôme n'est presque jamais un plantage : un `__try` n'attrape rien,
  puisque rien ne fautte. Chercher plutôt : la fonction sort-elle par un test
  précoce sur son premier argument ?
- ➡ **Écrire la convention dans le commentaire de la constante d'adresse** et
  dans le doc de RE. Un paragraphe qui cite `sub_XXXX(&g_Truc)` sans dire par où
  passe l'argument invite l'erreur.
- Deux appels de suite peuvent avoir des conventions différentes. Les vérifier un
  par un.

## 3 bis. 🔴🔴 Le prototype d'Hex-Rays MENT sur le NOMBRE d'arguments — lire le `retn`

La convention (§3) ne suffit pas : il faut aussi le **compte**. Hex-Rays déduit
les arguments des accès qu'il a su rattacher au frame ; ceux qu'une fonction ne
lit jamais **n'apparaissent pas dans le prototype**, alors que l'épilogue les
dépile quand même.

🔴 **La preuve n'est pas le prototype, c'est `retn <n>`** : `n / 4` = le nombre
d'arguments PILE. Le stack frame le confirme (compter les `arg_*` après
`__return_address`). Et un **site d'appel natif** donne les VALEURS à recopier
plutôt qu'à deviner.

Vécu le 2026-09-01 (GreyWorld) sur `SpriteRes_GetOrLoadByName 0x00568760`,
déclarée `(cache, name, a, b)` par Hex-Rays donc appelée avec **3** arguments
pile — elle en dépile **5** (`retn 14h` @0x005688DF).

⚠ **LE SYMPTÔME NE RESSEMBLE EN RIEN À LA CAUSE.** Ce n'est pas un échec muet
(§3) ni un plantage dans la fonction appelée : ce sont **8 octets abandonnés sur
la pile à chaque appel**, donc c'est **L'APPELANT** qui meurt, plus tard, dans une
fonction qu'on n'a pas touchée. Ici : `0xC0000005` sur
`mov eax,[esi+1B0h]` à `CScene_RenderCellsAndCursor+0x17`, avec **esi = 0x8A —
une coordonnée de cellule passée à l'appel d'avant**. Un `this` qui vaut une
donnée métier récente = signature d'une pile décalée, jamais d'un pointeur mort.

➡ Recette, dans l'ordre, avant tout `reinterpret_cast` vers une native :
1. `retn <n>` de l'épilogue (get_bytes sur `addr + size - 3`) ;
2. les `arg_*` du stack frame après `__return_address` ;
3. un site d'appel natif, pour les valeurs.

➡ Et **corriger le prototype dans l'IDB** au passage : celui qui a menti une fois
mentira au suivant.

## 4. `Initialize()` tourne AVANT le client

🔴 Notre DLL est chargée en proxy de DirectDraw : à cet instant le jeu n'a monté
ni ses archives ni ses gestionnaires. **N'y appeler aucune fonction du client** —
ni le VFS, ni un manager, ni rien qui déréférence une globale du jeu. *Poser* un
détour est sûr ; l'*utiliser* ne l'est pas.

Différer jusqu'à une preuve que le client est prêt. La meilleure preuve est **une
demande venant de lui** : le premier appel de la fonction détournée. S'il réclame
un message, sa table est chargée, donc son VFS aussi.

Vécu le 2026-08-14 : le court-circuit de `msgstringtable` lisait le csv depuis
`Initialize()`, le client **ne démarrait plus du tout**.

⚠ **La pile de crash désignera le mauvais coupable.** Ici elle montrait
`ImGui_ImplWin32_Shutdown` → `~RagnarokClient` → `~Bourgeon`, avec `ESI = 0` :
un crash à la FERMETURE. Ce n'était que le démontage du processus après un
démarrage avorté. Le vrai témoin est **`bourgeon.log`**, arrêté après sa première
ligne. **Regarder le journal AVANT la pile.**

Pour situer une adresse de notre DLL : le `.map` du linker
(`build/src/Release/ddraw.map`) résout un RVA en symbole — x32dbg n'a pas nos
symboles. RVA = adresse − base chargée ; le `.map` indexe sur la base préférée,
donnée dans son en-tête.

## 5. Ne PAS gater l'installation d'un hook sur le timestamp du client

Un ctor de plugin qui fait `if (client().timestamp() != 20250716) return;` avant
d'installer son hook **ne l'installe jamais** : `timestamp_` n'est renseigné qu'à
`RagnarokClient::Initialize` (`ragnarok_client.cc`), donc APRÈS `LoadPlugins()`.
La garde est vraie, l'early-return est définitif (le ctor ne tourne qu'une fois).

Diagnostiqué en live : hook `WeaponLayer` @0x006046e0 = `E9` (posé), hook
`FpsView` @0x00c82340 = `0x55` (octet d'origine → **jamais posé**), pour un
symptôme « la touche F9 ne fait rien ».

➡ **Installer inconditionnellement**, et garder la garde timestamp uniquement
dans les méthodes runtime (`OnTick`, `OnKeyDown`).

## 6. Un prologue SEH refuse le JMP-hook

Certaines fonctions ouvrent un cadre SEH dès leur prologue : le trampoline
JMP de Bourgeon les casse. Deux cas connus et vérifiés :
**`UIText_DrawColored` (0x00a26540)** et **`UISubChatWnd_WrapAndDispatch`**.

Un prologue sain ressemble à `PUSH EBP / MOV EBP,ESP / SUB ESP,0x18` avec un
`RET` simple — celui de `UIScrollText_PaintLines` (0x008539c0), qui se hooke sans
problème. **Regarder le prologue avant de choisir sa cible de hook**, et préférer
un appelant ou un appelé au cadre propre.

## 7. Une touche avalée affame NOTRE propre handler

`Plugin::OnKeyDown` n'écoute pas le WndProc : il vient de `Bourgeon::FireKeyDown`,
appelé depuis `UIWindowMgr::ProcessPushButtonHook` — c'est-à-dire **depuis le
jeu**. 🔴 Toute touche que le WndProc confisque (`return 0`) n'arrive donc ni au
client **ni à nous**. Confisquer, c'est s'affamer soi-même.

Vécu : la barre de chat ImGui confisque le clavier quand elle est ouverte (sinon
une lettre déplace le personnage). Entrée et Échap ont cessé de la refermer — non
parce que la logique était fausse, mais parce que `enter_pending_` n'était plus
jamais posé. Deux tours de débogage à relire une logique correcte.

➡ Quand le WndProc avale une touche **pour une de nos features**, la lui remettre
DIRECTEMENT par un point d'entrée dédié (`ChatWindow::OnRawKey(vkey)`), appelé aux
endroits mêmes qui retournent 0. ⚠ Ne la remettre que si c'est bien NOUS qui
motivons l'avalage : si un champ ImGui a le focus ou que DOOM tourne, elle leur
appartient.

## 8. Le fil recv COPIE, il ne décode JAMAIS

**Sans exception** : `OnRecvPacket` ne contient qu'une ligne —
`net_inbox_.Push(op, data, len)` (ou `PushAnnounced`). Tout le décodage va dans
`HandlePacket`, rejoué par `Bourgeon::OnProcessInput` sur le fil PRINCIPAL.
Brique : `src/features/net_inbox.h`, posée dans `Plugin` lui-même (commit
d82216e).

`OnRecvPacket` est le SEUL événement du `Plugin` qui ne soit pas sur le fil
principal — et `plugin.h` affirmait le contraire pendant des mois. Décoder
là-dedans, c'est écrire pendant que le rendu lit : `push_back` qui RÉALLOUE,
`clear()` en pleine boucle d'affichage, `unordered_map` qui REHASHE pendant un
`find`, `std::string` réassignée pendant qu'ImGui en lit le `c_str()`. Aucun de
ces bugs ne se voit à la lecture, aucun ne se reproduit en test : ils sortent en
jeu, sous charge, chez le joueur.

⚠ **`PushAnnounced`, et ce n'est pas un détail** : beaucoup de handlers lisent
AU-DELÀ de `len` (le dispatcher ne transmet parfois que 2 octets, le corps vit
dans le tampon de réception). Copier `len` octets casse ces listes EN SILENCE.
Concernés : boutique NPC (achat + vente), cash shop, fabrication (3 listes),
refine, guilde, dialogue NPC, 0x0A3B.

⚠ **Pas dans `OnTick`** (bridé à ~100 ms) : le drain est dans `OnProcessInput`,
chaque frame, hors frame ImGui. Les deux ne tournent QU'EN JEU (hooks CGameMode) —
un module qui reçoit au login ne peut pas s'en servir tel quel.

⚠ La file sérialise les données, pas les **décisions** : un handler qui décidait
au recv décide maintenant une frame plus tard. À vérifier quand un handler natif
encore vivant (opcode OBSERVÉ) doit passer avant ou après.

Sains, laissés tels quels : banque, inventaire, char-select, rapport de bug,
intégrité, relais Discord. Sur la latence de drain, voir
[[project_solved_archive]].

## 9. 🔴🔴 Un battement en tête de frame ne rattrape PAS la frame de NAISSANCE

Le patron « masquer une fenêtre native au battement par frame »
(`OnGameFramePulse`, appelé en tête de `CGameMode::OnUpdate`, AVANT le dessin)
laisse un trou **structurel** : la fenêtre, elle, naît **au MILIEU** de la frame
— son ctor part de la mise à jour du mode, donc après notre battement et avant
le dessin. **La frame de sa naissance la montre toujours.**

⚠ « Une seule frame » n'est PAS invisible : à l'entrée sur une carte, c'est
justement là que les frames sont les plus longues (textures à charger). Le
joueur a rapporté « on a le temps de les apercevoir » — c'était bien UNE frame.
➡ **Ne pas conclure « une frame, donc négligeable » et chercher ailleurs.**

➡ Le correctif est de la faire **NAÎTRE invisible** : détour sur son **ctor**,
appel du trampoline puis `uiwnd::SetVisible(wnd, false)` (après, jamais avant —
c'est `UIWindow_base_ctor` qui pose le drapeau à 1). Détourner la **fonction**
et non ses sites d'appel couvre d'un coup tous les créateurs (la jauge HP/SP en
a deux : msg 0x22 de l'acteur, et `CGameMode::SendMsg` case 41).

Vécu le 2026-08-29 sur `UIPlayerGage_ctor` (0x00836530) — barre HP/SP sous le
personnage, option « Masquer les barres HP/SP sous mon personnage ». Le
battement RESTE quand même : lui seul couvre la fenêtre DÉJÀ créée quand on
coche l'option en cours de partie. Même famille que le détour du site d'appel du
radar (`kDrawMiniMapCall`) et que le pré-rendu msg 0x22 de Basic Info.

⚠ Contrôler le motif d'octets du prologue avant de poser (l'exe livré porte des
patchs WARP que l'IDB ne montre pas) — et pas de garde timestamp à l'install
(cf. §5) : `LoadPlugins()` ne tourne déjà que sur le 20250716.

---

Voir aussi [[feedback_re_method]],
[[feedback_debug_tooling]], [[reference_dll_watchpoint_veh_diag]],
[[feedback_state_lifetime]].
