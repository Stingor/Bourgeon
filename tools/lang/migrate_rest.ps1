# Enveloppe de i18n::Tr() TOUT libelle francais situe dans un CORPS DE FONCTION.
#
# C'est le complement de migrate_tr.ps1, qui ne connait qu'une liste d'appels.
# Ici on ne reconnait plus l'appel : on reconnait le CONTEXTE. Le seul endroit
# dangereux est l'initialiseur statique, et ce script sait l'eviter.
#
# 🔴 POURQUOI CETTE DISTINCTION EST TOUT LE SUJET
# Un Tr() place dans un agregat de niveau fichier COMPILE parfaitement, mais il
# s'evalue UNE SEULE FOIS, au chargement de la DLL, avant meme que le catalogue
# existe : le libelle sort en francais et n'en sortira plus jamais. Aucune erreur,
# aucun avertissement, aucun symptome autre qu'un mot qui refuse de se traduire.
#
# On tient donc une PILE de blocs, et on distingue a l'ouverture de chaque « { » :
#   FUNC  -- precede de « ) », eventuellement suivi de const/noexcept/override :
#            c'est un corps de fonction ou de lambda. Tr y est evalue a CHAQUE
#            appel, donc dans la bonne langue ;
#   INIT  -- precede de « = », « , », « { » ou « ] » : c'est un initialiseur
#            (tableau, agregat, liste). Tr y serait fige. On n'y touche pas ;
#   OTHER -- classe, namespace, enum, bloc de controle.
# Un litteral n'est enveloppe que s'il y a au moins un FUNC dans la pile et
# AUCUN INIT au-dessus de lui.
#
# ASCII pur, regex Unicode en \uXXXX : cf. l'en-tete de migrate_tr.ps1.
#
# Usage :
#   powershell -File tools\lang\migrate_rest.ps1 -Path src -Recurse -WhatIf

param(
  [Parameter(Mandatory=$true)][string]$Path,
  [switch]$Recurse,
  [switch]$WhatIf
)

$rxAccent = [regex]'[\u00C0-\u00FF\u0152\u0153\u00AB\u00BB]'
$rxWords  = [regex]'[A-Za-z\u00C0-\u00FF]{2,} +[A-Za-z\u00C0-\u00FF]'
$rxPath   = [regex]'(\\\\|/|\.(ttf|bmp|tga|png|jpg|yaml|yml|txt|lua|lub|dll|exe|gif|wav|spr|act|str|grf))'
$rxLetters= [regex]'[^A-Za-z\u00C0-\u00FF]'
# Journalisation : un message de log traduit rend les journaux de deux joueurs
# incomparables. On regarde l'instruction entiere, pas seulement la ligne.
$rxLogCall= [regex]'\b(LogDiag|LogInfo|LogError|LogDebug|LogWarn|OutputDebugString|spdlog)\b'

function Test-IsUserText([string]$lit) {
  if ($lit.Length -lt 3) { return $false }
  if ($lit.StartsWith('##')) { return $false }
  if ($lit -match '###') { return $false }
  if ($rxPath.IsMatch($lit) -and -not $rxWords.IsMatch($lit)) { return $false }
  if (($rxLetters.Replace($lit, '')).Length -lt 3) { return $false }
  return ($rxAccent.IsMatch($lit) -or $rxWords.IsMatch($lit))
}

# Analyse lexicale minimale : on doit distinguer une chaine d'un commentaire et
# d'un litteral de caractere, sinon on se decale (cf. `c == '"'`).
function Get-Wrappable([string]$src) {
  $n = $src.Length
  $stack = New-Object System.Collections.Generic.List[string]
  $spans = New-Object System.Collections.Generic.List[object]   # groupes enveloppables
  $i = 0
  $lastSignificant = ''      # dernier caractere de code vu (hors espaces/commentaires)
  while ($i -lt $n) {
    $c = $src[$i]
    # Commentaire de ligne
    if ($c -eq '/' -and $i + 1 -lt $n -and $src[$i+1] -eq '/') {
      while ($i -lt $n -and $src[$i] -ne "`n") { $i++ }
      continue
    }
    # Commentaire de bloc
    if ($c -eq '/' -and $i + 1 -lt $n -and $src[$i+1] -eq '*') {
      $i += 2
      while ($i + 1 -lt $n -and -not ($src[$i] -eq '*' -and $src[$i+1] -eq '/')) { $i++ }
      $i += 2
      continue
    }
    # Litteral de caractere : consomme sans rien interpreter
    if ($c -eq "'") {
      $i++
      while ($i -lt $n -and $src[$i] -ne "'") { if ($src[$i] -eq '\') { $i++ }; $i++ }
      $i++
      $lastSignificant = "'"
      continue
    }
    # Chaine, et le GROUPE de chaines adjacentes qui la suit
    if ($c -eq '"') {
      $start = $i
      $joined = ""
      while ($true) {
        $i++   # entre dans la chaine
        $sb = New-Object System.Text.StringBuilder
        while ($i -lt $n -and $src[$i] -ne '"') {
          if ($src[$i] -eq '\' -and $i + 1 -lt $n) { [void]$sb.Append($src[$i]); $i++ }
          [void]$sb.Append($src[$i]); $i++
        }
        $i++   # sort de la chaine
        $joined += $sb.ToString()
        # Chaine adjacente ? (espaces et commentaires autorises entre les deux)
        $j = $i
        while ($j -lt $n) {
          if ([char]::IsWhiteSpace($src[$j])) { $j++; continue }
          if ($src[$j] -eq '/' -and $j + 1 -lt $n -and $src[$j+1] -eq '/') { while ($j -lt $n -and $src[$j] -ne "`n") { $j++ }; continue }
          if ($src[$j] -eq '/' -and $j + 1 -lt $n -and $src[$j+1] -eq '*') { $j += 2; while ($j + 1 -lt $n -and -not ($src[$j] -eq '*' -and $src[$j+1] -eq '/')) { $j++ }; $j += 2; continue }
          break
        }
        if ($j -lt $n -and $src[$j] -eq '"') { $i = $j; continue }
        break
      }
      $inFunc = ($stack -contains 'FUNC')
      $topInit = $false
      for ($k = $stack.Count - 1; $k -ge 0; $k--) {
        if ($stack[$k] -eq 'INIT') { $topInit = $true; break }
        if ($stack[$k] -eq 'FUNC') { break }
      }
      if ($inFunc -and -not $topInit) {
        $spans.Add([pscustomobject]@{ Start = $start; End = $i; Text = $joined })
      }
      $lastSignificant = '"'
      continue
    }
    if ($c -eq '{') {
      # Nature du bloc, decidee par le dernier caractere de code.
      $kind = if ($lastSignificant -eq ')') { 'FUNC' }
              elseif ($lastSignificant -eq '=' -or $lastSignificant -eq ',' -or
                      $lastSignificant -eq '{' -or $lastSignificant -eq '[') { 'INIT' }
              else { 'OTHER' }
      $stack.Add($kind)
      $lastSignificant = '{'
      $i++
      continue
    }
    if ($c -eq '}') {
      if ($stack.Count -gt 0) { $stack.RemoveAt($stack.Count - 1) }
      $lastSignificant = '}'
      $i++
      continue
    }
    if (-not [char]::IsWhiteSpace($c)) { $lastSignificant = $c }
    $i++
  }
  return $spans
}

$files = if ($Recurse) { Get-ChildItem -Path $Path -Recurse -Include *.cc } else { Get-Item -Path $Path }
$total = 0
$enc = New-Object System.Text.UTF8Encoding($false)

foreach ($file in $files) {
  $src = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
  $spans = Get-Wrappable $src
  # De la FIN vers le DEBUT : sinon chaque insertion decale les positions suivantes.
  $keep = New-Object System.Collections.Generic.List[object]
  foreach ($s in $spans) { if (Test-IsUserText $s.Text) { $keep.Add($s) } }
  if ($keep.Count -eq 0) { continue }
  $out = $src
  $n = 0
  for ($k = $keep.Count - 1; $k -ge 0; $k--) {
    $s = $keep[$k]
    $before = $out.Substring([Math]::Max(0, $s.Start - 14), [Math]::Min(14, $s.Start))
    if ($before -match 'i18n::Tr(?:Id)?\(\s*$') { continue }
    # L'instruction qui porte le litteral : si c'est un log, on passe.
    $lineStart = $out.LastIndexOf("`n", [Math]::Max(0, $s.Start - 1)) + 1
    $stmtStart = $out.LastIndexOf(';', [Math]::Max(0, $s.Start - 1)) + 1
    $ctxStart = [Math]::Max(0, [Math]::Min($lineStart, $stmtStart))
    $ctx = $out.Substring($ctxStart, $s.Start - $ctxStart)
    if ($rxLogCall.IsMatch($ctx)) { continue }
    # 🔴 `static` LOCAL : meme piege que l'agregat de fichier. Une statique de
    # fonction s'initialise a la PREMIERE execution et ne se reevalue jamais --
    # le libelle serait fige dans la langue de ce moment-la. Et un
    # `static const char kX[] = ...` ne compilerait meme pas : on ne peut pas
    # initialiser un tableau de char avec un const char*.
    if ($ctx -match '\bstatic\b') { continue }
    if ($ctx -match '\bchar\s+\w+\s*\[\s*\]') { continue }
    # Comparaison de chaines : traduire l'operande CASSE la logique. Le texte
    # sert ici de valeur temoin, pas d'affichage.
    if ($ctx -match '(strcmp|strncmp|strstr|strcasecmp|_stricmp)\s*\(') { continue }
    if ($ctx -match '(==|!=)\s*$') { continue }
    $lit = $out.Substring($s.Start, $s.End - $s.Start)
    $out = $out.Substring(0, $s.Start) + "i18n::Tr(" + $lit + ")" + $out.Substring($s.End)
    $n++
  }
  if ($n -gt 0) {
    if ($out -notmatch '#include "utils/i18n\.h"') {
      $incs = [regex]::Matches($out, '(?m)^#include .*$')
      if ($incs.Count -gt 0) {
        $last = $incs[$incs.Count - 1]
        $out = $out.Substring(0, $last.Index + $last.Length) + "`n#include `"utils/i18n.h`"" + $out.Substring($last.Index + $last.Length)
      }
    }
    $total += $n
    Write-Output ("{0,5}  {1}" -f $n, $file.Name)
    if (-not $WhatIf) { [System.IO.File]::WriteAllText($file.FullName, $out, $enc) }
  }
}
Write-Output ("TOTAL enveloppe : " + $total)
