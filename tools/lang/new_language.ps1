# Fabrique le SQUELETTE de catalogue d'une nouvelle langue a partir d'un
# catalogue deja rempli (en.yaml par defaut).
#
# Chaque entree sort avec sa valeur VIDE. Ce n'est pas un oubli : i18n::Tr traite
# une valeur vide comme absente et ressort le texte source. Un squelette a moitie
# rempli est donc utilisable tel quel -- les lignes traduites passent dans la
# langue cible, les autres restent en francais, et rien ne se vide a l'ecran.
#
# La traduction ANGLAISE de chaque cle est reportee en commentaire au-dessus :
# les cles sont francaises, et un traducteur hispanophone lit generalement mieux
# l'anglais. Les commentaires sont ignores par le jeu comme par check_catalog.
#
# Usage :
#   powershell -File tools\lang\new_language.ps1 -Code es
#   powershell -File tools\lang\new_language.ps1 -Code pt -From tools\lang\en.yaml
#
# Ensuite, pour suivre l'avancement :
#   powershell -File tools\lang\check_catalog.ps1 -Catalog tools\lang\es.yaml
# (« valeurs VIDES » = ce qu'il reste a traduire.)
#
# ⚠ Le fichier n'est PAS copie dans le jeu : tant qu'il n'est pas dans
# SaveData\lang, la langue reste grisee dans le combo -- ce qui evite de proposer
# une langue qui n'afficherait que du francais.

param(
  [Parameter(Mandatory = $true)][string]$Code,
  [string]$From = "",
  [string]$Out  = "",
  [switch]$Force
)

# Les defauts se calculent ICI et pas dans le bloc param : avec un parametre
# Mandatory, le script bascule en mode « avance » et $PSScriptRoot n'y vaut plus
# ce qu'on croit -- le chemin sortait ampute de son dossier.
if (-not $From) { $From = Join-Path $PSScriptRoot "en.yaml" }
if (-not $Out)  { $Out  = Join-Path $PSScriptRoot ($Code + ".yaml") }

if (-not (Test-Path $From)) {
  Write-Error ("catalogue source introuvable : " + $From)
  exit 1
}
if ((Test-Path $Out) -and -not $Force) {
  Write-Error ("existe deja : " + $Out + "  (utiliser -Force pour ecraser)")
  exit 1
}

# Les deux formes d'entree admises, cf. check_catalog.ps1.
$rxEntry   = [regex]'^"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s*$'
$rxExplKey = [regex]'^\?\s+"((?:[^"\\]|\\.)*)"\s*$'
$rxExplVal = [regex]'^:\s*"((?:[^"\\]|\\.)*)"\s*$'

# 🔴 Meme marge que i18n.cc : au-dela, la cle IMPLICITE depasse la limite YAML de
# 1024 caracteres et yaml-cpp rejette le fichier ENTIER.
$maxImplicitKeyBytes = 1000

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("# Bourgeon - catalogue de traduction : " + $Code + "`r`n")
[void]$sb.Append("#`r`n")
[void]$sb.Append("# Cle = le texte source francais, valeur = la traduction.`r`n")
[void]$sb.Append("# Key = the French source string, value = your translation.`r`n")
[void]$sb.Append("#`r`n")
[void]$sb.Append("# Une valeur laissee VIDE garde le francais : on peut livrer a tout moment.`r`n")
[void]$sb.Append("# An EMPTY value keeps the French text: the file is usable at any stage.`r`n")
[void]$sb.Append("#`r`n")
[void]$sb.Append("# Ne PAS toucher aux cles : elles doivent rester identiques, octet pour octet,`r`n")
[void]$sb.Append("# aux chaines du code -- un accent retouche et la ligne cesse de correspondre.`r`n")
[void]$sb.Append("# Do NOT edit the keys: a single changed byte silently breaks the match.`r`n")
[void]$sb.Append("#`r`n")
# Chaine SIMPLE quote : en double quote, PowerShell echappe au backtick et le
# antislash de \" ne proteger rien -- le guillemet fermerait la chaine ici meme.
[void]$sb.Append('# Les \n, \t et \" sont des ECHAPPEMENTS : les reporter tels quels.' + "`r`n")
[void]$sb.Append("# Les termes de jeu restent en anglais : storage, cart, shop, alootid.`r`n")
[void]$sb.Append("`r`n")

$count = 0
$pendingKey = $null
$emit = {
  param($k, $en)
  if ($en) { [void]$sb.Append("# en: " + $en + "`r`n") }
  if (([System.Text.Encoding]::UTF8.GetByteCount($k) + 2) -gt $maxImplicitKeyBytes) {
    [void]$sb.Append("? `"" + $k + "`"`r`n")
    [void]$sb.Append(": `"`"`r`n")
  } else {
    [void]$sb.Append("`"" + $k + "`": `"`"`r`n")
  }
}

foreach ($line in [System.IO.File]::ReadAllLines($From, [System.Text.Encoding]::UTF8)) {
  $t = $line.Trim()
  if (-not $t -or $t.StartsWith('#')) { continue }

  if ($null -ne $pendingKey) {
    $mv = $rxExplVal.Match($t)
    if ($mv.Success) { & $emit $pendingKey $mv.Groups[1].Value; $count++ }
    $pendingKey = $null
    continue
  }

  $mk = $rxExplKey.Match($t)
  if ($mk.Success) { $pendingKey = $mk.Groups[1].Value; continue }

  $m = $rxEntry.Match($t)
  if ($m.Success) { & $emit $m.Groups[1].Value $m.Groups[2].Value; $count++ }
}

# Un squelette VIDE serait pris pour un catalogue legitime et ne dirait rien de
# son echec : on refuse d'ecrire plutot que de livrer un fichier trompeur.
if ($count -eq 0) {
  Write-Error ("aucune entree lue dans " + $From + " -- rien n'a ete ecrit")
  exit 1
}

# UTF-8 SANS BOM : c'est ce que lit le jeu, et ce que produit deja en.yaml. Un
# BOM se retrouverait colle a la premiere cle et la ferait rater.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($Out, $sb.ToString(), $utf8NoBom)

Write-Output ("squelette ecrit : " + $Out)
Write-Output ("  entrees a traduire : " + $count)
Write-Output ("  source : " + (Split-Path $From -Leaf))
