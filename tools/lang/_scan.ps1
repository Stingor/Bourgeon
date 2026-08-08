# Prepare un fichier C++ pour l analyse : commentaires blanchis, litteraux de
# CARACTERE neutralises, litteraux de CHAINE intacts.
#
# 🔴 EN UNE SEULE PASSE, et c est tout l enjeu. Les versions precedentes
# blanchissaient les commentaires par une regex appliquee AVANT toute conscience
# des chaines. Un « /* » place a l interieur d un litteral -- ce qui arrive pour
# de vrai : le colorateur de script d item_desc_window.cc contient "/*", et
# image_preview.cc envoie un en-tete HTTP « Accept: */* » -- ouvrait alors un FAUX
# bloc de commentaire qui avalait tout le code jusqu au « */ » suivant. Des
# centaines de lignes disparaissaient de l analyse, sans erreur ni trace : c est
# ainsi que BeginTabItem("Monstres") est reste nu alors que l appel figurait
# pourtant dans la liste.
#
# Ici l alternation est lue de GAUCHE A DROITE et la branche « chaine » vient en
# premier : une chaine est consommee ENTIERE, donc un « /* » qu elle contient
# n est jamais vu comme un commentaire. Le probleme d ordre disparait.
#
# ⚠ Les longueurs sont CONSERVEES (commentaire remplace par autant d espaces,
# sauts de ligne gardes) : les positions relevees sur le texte prepare valent
# telles quelles sur le texte ORIGINAL, qui seul sera modifie.

$rxScanToken = [regex]"(?s)`"(?:[^`"\\]|\\.)*`"|'(?:[^'\\]|\\.)*'|//[^\r\n]*|/\*.*?\*/"

function Get-ScannableText([string]$src) {
  return $rxScanToken.Replace($src, {
    param($m)
    $v = $m.Value
    # Chaine : intacte, c est elle qu on cherche.
    if ($v.StartsWith('"')) { return $v }
    # Litteral de CARACTERE : neutralise a longueur egale. `c == '"'` contiendrait
    # sinon un guillemet qui ouvrirait une fausse chaine.
    if ($v.StartsWith("'")) { return "'" + ('x' * ($v.Length - 2)) + "'" }
    # Commentaire : blanchi, sauts de ligne preserves.
    return -join ($v.ToCharArray() | ForEach-Object {
      if ($_ -eq "`n" -or $_ -eq "`r") { $_ } else { ' ' }
    })
  })
}
