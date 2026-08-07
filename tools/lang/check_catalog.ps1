# Confronte les cles i18n::Tr()/TrId() du code aux entrees d'un catalogue.
#
# Il attrape les trois pannes de traduction qui ne se voient PAS a l'ecran, parce
# que Tr retombe silencieusement en francais dans les trois cas :
#   - chaine migree SANS entree au catalogue -> elle sortira en francais ;
#   - entree ORPHELINE -> un texte source a ete retouche, sa traduction est morte ;
#   - valeur VIDE -> Tr la traite comme absente.
#
# Usage :
#   powershell -File tools\lang\check_catalog.ps1
#   powershell -File tools\lang\check_catalog.ps1 -Catalog tools\lang\es.yaml
#
# ⚠ L'extraction est STATIQUE : elle ne voit que les litteraux passes a Tr. Une
# chaine venant d'une table (Tr(kCats[c].label)) n'y figure pas et ressortira en
# ORPHELINE -- c'est normal, elle est legitime. Seul l'export runtime du jeu
# (panneau Interface, bouton « Exporter ») les connait toutes.

param(
  [string]$Src     = "$PSScriptRoot\..\..\src",
  [string]$Catalog = "$PSScriptRoot\en.yaml"
)

# Pour TrId, la repetition de litteraux s'arrete a la virgule : seul le PREMIER
# argument est capture, l'identifiant stable n'etant pas une cle de traduction.
$rxTr    = [regex]'i18n::Tr(?:Id)?\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)'
$rxLit   = [regex]'"((?:[^"\\]|\\.)*)"'
# 🔴 Les litteraux de CARACTERE d'abord. `c == '"'` contient un guillemet ; sans
# ce nettoyage il ouvre une fausse chaine et TOUT le reste du fichier se decale --
# on s'est retrouve avec des morceaux de code C++ pris pour des libelles.
$rxCharLit = [regex]"'(?:[^'\\\\]|\\\\.)'"
function Remove-CharLiterals([string]$src) {
  return $rxCharLit.Replace($src, { param($m) "'" + ("x" * ($m.Value.Length - 2)) + "'" })
}
$rxEntry = [regex]'^"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s*$'

$srcKeys = New-Object System.Collections.Generic.HashSet[string]
$perFile = @{}
foreach ($f in Get-ChildItem -Path $Src -Recurse -Include *.cc,*.h) {
  $text = Remove-CharLiterals ([System.IO.File]::ReadAllText($f.FullName, [System.Text.Encoding]::UTF8))
  $n = 0
  foreach ($m in $rxTr.Matches($text)) {
    $key = ""
    foreach ($lit in $rxLit.Matches($m.Groups[1].Value)) { $key += $lit.Groups[1].Value }
    # Un vrai saut de ligne dans une cle = artefact d'analyse (raw string mal
    # decoupee). Un libelle C++ ecrit toujours \n en ECHAPPE, jamais en litteral.
    if ($key -and -not ($key.Contains("`n") -or $key.Contains("`r"))) { [void]$srcKeys.Add($key); $n++ }
  }
  if ($n -gt 0) { $perFile[$f.Name] = $n }
}

$catKeys = New-Object System.Collections.Generic.HashSet[string]
$empty  = New-Object System.Collections.Generic.List[string]
$dupes  = New-Object System.Collections.Generic.List[string]
$broken = New-Object System.Collections.Generic.List[string]
foreach ($line in [System.IO.File]::ReadAllLines($Catalog, [System.Text.Encoding]::UTF8)) {
  $t = $line.Trim()
  if (-not $t -or $t.StartsWith('#')) { continue }
  $m = $rxEntry.Match($t)
  if (-not $m.Success) { $broken.Add($t.Substring(0, [Math]::Min(70, $t.Length))); continue }
  $k = $m.Groups[1].Value
  if (-not $catKeys.Add($k)) { $dupes.Add($k) }
  if (-not $m.Groups[2].Value) { $empty.Add($k) }
}

$missing = @($srcKeys | Where-Object { -not $catKeys.Contains($_) })
$orphan  = @($catKeys | Where-Object { -not $srcKeys.Contains($_) })

Write-Output ("catalogue : " + (Split-Path $Catalog -Leaf))
Write-Output ("  cles dans le code : " + $srcKeys.Count + "  (fichiers migres : " + $perFile.Count + ")")
Write-Output ("  cles au catalogue : " + $catKeys.Count)
Write-Output ""
Write-Output ("SANS traduction : " + $missing.Count)
$missing | Sort-Object | ForEach-Object { "   - " + $_ }
Write-Output ("ORPHELINES (dont cles de tables statiques, legitimes) : " + $orphan.Count)
$orphan | Sort-Object | ForEach-Object { "   - " + $_ }
Write-Output ("valeurs VIDES : " + $empty.Count)
$empty | Sort-Object | ForEach-Object { "   - " + $_ }
Write-Output ("DOUBLONS : " + $dupes.Count)
$dupes | Sort-Object | ForEach-Object { "   - " + $_ }
if ($broken.Count) {
  Write-Output ("LIGNES NON PARSEES : " + $broken.Count)
  $broken | ForEach-Object { "   ! " + $_ }
}
Write-Output ""
Write-Output "Fichiers migres :"
$perFile.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { "{0,5}  {1}" -f $_.Value, $_.Key }

if ($missing.Count -or $empty.Count -or $dupes.Count -or $broken.Count) { exit 1 }
