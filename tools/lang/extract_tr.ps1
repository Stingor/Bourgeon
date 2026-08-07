# Extrait les cles i18n::Tr()/TrId() d'un fichier source et emet un gabarit YAML
# a coller dans le catalogue, valeurs vides, pretes a traduire.
#
# Usage :
#   powershell -File tools\lang\extract_tr.ps1 -Path src\features\windows\cart_viewer.cc
#
# 🔴 POURQUOI UN SCRIPT ET PAS UN COPIER-COLLER : la cle doit correspondre au
# litteral du source OCTET POUR OCTET. Une espace insecable relevee comme une
# espace ordinaire, une apostrophe courbe prise pour une droite, et la cle ne
# matche plus -- sans la moindre erreur : Tr retombe simplement en francais, et on
# cherche longtemps pourquoi UNE ligne refuse de se traduire.
#
# ⚠ Extraction STATIQUE : une chaine passee a Tr par variable (Tr(kCats[c].label),
# les tables de libelles) n'y figure pas. Pour celles-la, lancer le jeu dans la
# langue cible, ouvrir les fenetres concernees, et utiliser l'export du panneau
# Interface (« Exporter »), qui ecrit SaveData\lang\<code>.missing.yaml.

param([Parameter(Mandatory=$true)][string]$Path)

$text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)

# Les litteraux ADJACENTS sont concatenes par le compilateur : un libelle coupe
# sur plusieurs lignes en C++ ne fait qu'UNE cle. Pour TrId, la repetition
# s'arrete a la virgule -> l'identifiant stable n'est pas capture.
$rx    = [regex]'i18n::Tr(?:Id)?\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)'
$rxLit = [regex]'"((?:[^"\\]|\\.)*)"'

$keys = New-Object System.Collections.Generic.List[string]
foreach ($m in $rx.Matches($text)) {
  $key = ""
  foreach ($lit in $rxLit.Matches($m.Groups[1].Value)) { $key += $lit.Groups[1].Value }
  if ($key -and -not $keys.Contains($key)) { $keys.Add($key) }
}

Write-Output ("# " + (Split-Path $Path -Leaf) + " : " + $keys.Count + " cles")
foreach ($k in $keys) {
  # Le litteral C est deja echappe (\n, \", \\) et YAML double-quoted emploie la
  # MEME notation : on le reemet tel quel, sans decoder ni reencoder. Les octets
  # UTF-8 des accents traversent intacts.
  Write-Output ('"' + $k + '": ""')
}
