# Les adresses en dur de Bourgeon, et 17 de plus portées

Relevé du **2026-08-27**. Fait suite à [boot_addresses.md](boot_addresses.md) :
faire démarrer Bourgeon ne suffit pas, le projet appelle des centaines d'adresses
codées en dur, toutes propres au 20250716.

## L'état des lieux

| | |
|---|---|
| adresses de code 2025 en dur | **395** (hors `configuration.h`, gérée par YAML) |
| déjà portées | **210** (53,2 %) |
| restantes | **185** |

Ces 185 se regroupent en **155 fonctions contenantes** (plus 12 données) : une
adresse au milieu d'une fonction ne se porte pas seule, elle se porte avec sa
fonction. Six d'entre elles tombent dans la seule `sub_7F8810`.

## La méthode : intersection des appelants

Si la fonction 2025 `F` appelle `{a, b, c}` et que ces trois sont déjà portés
vers `{a', b', c'}`, alors la fonction 2026 cherchée appelle `{a', b', c'}`. Elle
est donc dans l'**intersection des appelants** de ces trois — un ensemble petit,
qu'on interroge sans balayer le binaire.

Deux garde-fous, parce qu'une intersection non vide ne prouve rien :

- le `retn N` doit être **identique** (convention d'appel) ;
- on n'accepte que si l'intersection filtrée laisse **un seul** candidat.

Sur les 149 fonctions à porter : 61 ont au moins deux ancres (signature
discriminante), 43 une seule (ambigu), 45 aucune prise.

## 🔴 Ce que vaut la méthode : mesuré, pas supposé

61 tentatives, 23 réponses uniques. Un chiffre plausible ne prouve rien — on
rejoue donc l'algorithme sur des paires **déjà connues**.

⚠ **Piège de circularité évité.** Les provenances du portage sont `vtable`
(4289), `tables` (1553), `appel-i1` (1083), `position` (789), `appel-i2` (476)…
Or `appel-i1` et `appel-i2` **sont** des propagations par appels : les inclure
aurait mesuré la méthode contre elle-même. Le témoin ne retient que
`vtable`/`tables`/`position`/`portage`/`accesseur`.

| témoin (120 cas) | |
|---|---|
| réponses uniques | 61 |
| correctes | **58** |
| fausses | **3** |
| **précision** | **95,1 %** |

## Le filtre de taille, calibré sur le témoin

Les ratios `taille2026 / taille2025` des bonnes réponses sont très concentrés —
médiane **1,00**, p10–p90 = 1,00–1,04 — tandis que deux des trois fausses sont
aberrantes (0,28 et 2,16). La troisième, elle, a un ratio de 1,00 : aucun filtre
de taille ne l'attrapera.

Filtre `[0,96 ; 1,13]` : garde 54/58 correctes, élimine 2/3 fausses.
⇒ **précision 98,2 %**, taux de réponse réduit d'autant.

⚠ Ce filtre est calibré sur le témoin lui-même, donc exposé au surapprentissage.
Il reste défendable parce que la concentration autour de 1,00 n'a rien d'un
artefact : deux builds successifs du même code produisent des fonctions de taille
voisine.

## Le résultat : 17 paires, 23 adresses

Sur les 23 résolutions uniques, **17 passent le filtre** et couvrent
**23 adresses en dur**.

**Contrôle externe.** La première du lot est
`RecvLoop_DispatchPackets 0x00c9df00 → 0x005095a0` — exactement la paire établie
le matin même par une voie entièrement différente (la table de sauts du switch,
relevée dans [opcode_dispatch_port.md](opcode_dispatch_port.md)). La méthode
retrouve donc, sans le savoir, un résultat connu par ailleurs.

Les 17 paires et les 6 écartées sont dans
[propagation_2026.json](propagation_2026.json), avec pour chacune le nombre
d'ancres et le ratio.

## 🔴 Ce que ça ne fait PAS

Ces paires enrichissent le **portage**, pas le code : Bourgeon continue d'appeler
des adresses 2025 en dur. Les substituer suppose un mécanisme d'adresses par
version — un refactoring d'une autre nature, qui touche 84 fichiers.

Et il reste **162 fonctions** non portées : 43 à une seule ancre (que la méthode
ne peut pas trancher seule), 45 sans aucune prise, et les 74 dont l'intersection
était vide ou ambiguë. Le taux de réponse de la méthode est de 38 % avant filtre,
28 % après — elle ne finira pas le travail seule.

Scripts rejouables : [scripts/](scripts/) (`hardcoded_census.py` pour l'état des
lieux ; la chaîne de propagation est décrite ici, ses scripts étant spécifiques à
la paire d'IDB ouverte).
