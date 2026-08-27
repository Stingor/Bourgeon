# `clientinfo.xml` — comment le client 2026 le lit vraiment

Relevé du 2026-08-27 sur `2026-07-07_Ragexe.exe` (IDB 2026), à l'occasion d'un
écran de sélection de serveur vide puis d'un « 서버 연결 실패 ».

Tout est mesuré sur le binaire ; rien n'est repris de la documentation
communautaire, qui décrit `<subconnection>` de façon contradictoire.

## 🔴 Il y a DEUX listes, pas une

C'est la confusion qui a coûté le plus de temps : « la liste des serveurs » est
en réalité **deux vecteurs différents du même objet**, remplis par **deux
fonctions différentes**, depuis **deux niveaux XML différents**.

| fonction | vecteur | source XML | écran |
|---|---|---|---|
| `sub_C36480(this)` | `this[3908..3910]` | **toutes** les `<connection>` | Service Select, **avant** login |
| `sub_C361C0(this, idx)` | `this[3911..3913]` | les `<subconnection>` de la connection n° `idx` | choix du serveur, **après** |

Les entrées font **128 octets** dans les deux cas.

`sub_C36480` boucle sur toutes les connections et ne dépend pas des
subconnections :

```c
while (1) {
    ... display / desc / balloon ...
    v12 = sub_A95AC0("connection");
    ...
    if (!v12) return;
}
```

`sub_C361C0`, elle, **sélectionne** la connection n° `idx` (boucle qui
décrémente), puis liste ses subconnections — et si la connection n'en a
**aucune**, elle prend une branche qui n'ajoute rien :

```c
if (sub_A95A60("subconnection")) {
    do { ... } while (v17);   // le seul chemin qui remplit
} else {
    sub_C3EE80(v25);          // n'ajoute RIEN
}
```

➡ **Un `clientinfo` sans `<subconnection>` donne un écran de choix de serveur
vide** — alors que le Service Select, lui, fonctionne. J'ai d'abord attribué le
symptôme au mauvais écran.

⚠ Les deux remplissages sont gardés par « seulement si le vecteur est vide »
(`if (this[3908] != this[3909]) return;` et `if (this[3911] == this[3912])`).
Un retour arrière suivi d'un autre choix de service pourrait donc conserver la
liste précédente, sauf si un reset vide le vecteur entre-temps. **Non vérifié.**

## Les champs de connexion : `sub_A39660(connIndex, subIndex)`

C'est elle qui produit ce que le client va réellement composer. Elle gère
**les deux dispositions** :

```c
if (sub_A95A60("subconnection")) {
    ... avance subIndex fois ...
    sub_A39F20(noeud_subconnection);
    // address / port / version / langtype / registrationweb / aid / yellow
} else {
    sub_A39F20(noeud_connection);
    // exactement les mêmes champs, lus un cran plus haut
}
```

➡ `<address>`/`<port>` sont valides **dans la `<connection>` comme dans la
`<subconnection>`**. Mettre les deux est sans risque et rend le fichier
insensible à la branche empruntée.

Destinations des champs :

| champ XML | globale | traitement |
|---|---|---|
| `address` | `off_10DB16C` | pointeur brut dans le DOM |
| `port` | `off_10DB170` | pointeur brut (chaîne, pas un entier) |
| `version` | `dword_146DF88` | `atoi` |
| `langtype` | `dword_146DF9C` | `atoi` |
| `registrationweb` | `lpFile` | pointeur brut |
| `aid`/`admin` | `dword_146DFE0..E8` | vecteur d'entiers |
| `yellow`/`admin` | `dword_146DFEC..F4` | vecteur d'entiers |

🔴 **`langtype` n'est pas cosmétique** : `dword_146DF9C` indexe la table de
capacités de `sub_A84500` — `matrix[14 * dword_146DF9C + dword_146DFA0]`.
Changer le langtype change donc quelles fonctionnalités le client s'autorise,
GameGuard compris.

## 🔴 Les défauts EN DUR, quand le fichier n'est pas appliqué

| globale | défaut |
|---|---|
| `off_10DB16C` (adresse) | `112.175.128.137` — l'IP officielle kRO |
| `off_10DB170` (port) | `6900` |

➡ Un `clientinfo` non lu ne donne **pas** d'erreur : le client part
silencieusement vers kRO. Le symptôme est un « échec de connexion » et un
serveur local qui **ne voit jamais la connexion arriver**. À ne pas confondre
avec un rejet du serveur.

## Le nom du fichier dépend du `<servicetype>`

Table de paires (grf, clientinfo) à `0x00FB2490` :

| servicetype | grf | clientinfo |
|---|---|---|
| *(korea, nu)* | `data.grf` | **`clientinfo.xml`** |
| sakray | `sdata.grf` | `sclientinfo.xml` |
| — | `gdata.grf` | `gclientinfo.xml` |
| event | `eventdata.grf` | `eventclientinfo.xml` |
| pk | `pkdata.grf` | `pkclientinfo.xml` |
| — | `fdata.grf` / `f2data.grf` | `fclientinfo.xml` / `f2clientinfo.xml` |
| — | `tdata.grf` | `tclientinfo.xml` |
| china | `cdata.grf` | `cclientinfo.xml` |

Donc `<servicetype>korea</servicetype>` ⇒ le client cherche `clientinfo.xml`
nu. Un `servicetype` inattendu fait chercher un **autre nom de fichier**, qui
n'existe pas, et on retombe sur les défauts kRO ci-dessus.

## Structure retenue

Deux `<connection>` (pour que le Service Select liste les deux services),
chacune portant ses champs de connexion **et** une `<subconnection>` qui les
répète (pour que le second écran se remplisse, quelle que soit la branche) :

```xml
<connection>
    <display>Moonlight-Destiny</display>
    <address>moonlight-destiny.fr</address><port>16900</port>
    <version>55</version><langtype>1</langtype>
    <subconnection>… les mêmes …</subconnection>
</connection>
<connection>
    <display>TEST</display>
    <address>127.0.0.1</address><port>6900</port>
    <version>0</version><langtype>1</langtype>
    <subconnection>… les mêmes …</subconnection>
</connection>
```

⚠ **Conséquence à ne pas perdre de vue** : avec deux services listés, le
premier clic mène à la **production**. Un serveur local qui ne reçoit rien
n'est alors pas un bug, c'est le mauvais service sélectionné.

## Outillage

`stingor.grf` est un GRF v0x200 (`Master of Magic`) de deux fichiers, en
position **1** de `DATA.INI` — donc prioritaire sur `moonlight.grf` et
`data.grf`. Il se réécrit avec `grfwrite.py` (scratchpad), qui sauvegarde en
`.bak` et **relit après écriture pour contrôler**.

🔴 Le fichier est verrouillé tant que le client tourne : `os.replace` échoue en
`WinError 5`. Fermer le client avant d'écrire, et nettoyer le `.tmp` laissé
derrière.
