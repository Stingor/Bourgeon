# Liste les libelles FRANCAIS encore nus, avec l'appel qui les porte.
#
# C'est le complement de migrate_tr.ps1 : celui-ci ne traite que les appels qu'il
# connait, celui-la dit ce qu'il a laisse et POURQUOI on ne l'a pas vu -- le nom
# de la fonction appelante suffit presque toujours a trancher entre « appel
# d'interface a ajouter au script » et « chaine technique a laisser nue ».
#
# ASCII pur : cf. l'en-tete de migrate_tr.ps1.
#
# Usage :
#   powershell -File tools\lang\audit_untranslated.ps1
#   powershell -File tools\lang\audit_untranslated.ps1 -ByCall

param(
  [string]$Src = "$PSScriptRoot\..\..\src",
  [switch]$ByCall
)

$rxAccent = [regex]'[\u00C0-\u00FF\u0152\u0153\u00AB\u00BB]'
$rxWords  = [regex]'[A-Za-z\u00C0-\u00FF]{2,} +[A-Za-z\u00C0-\u00FF]'
$rxLetters= [regex]'[^A-Za-z\u00C0-\u00FF]'
# Un GROUPE de litteraux adjacents = une seule chaine pour le compilateur.
$rxGroup  = [regex]'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
$rxJoin   = [regex]'"((?:[^"\\]|\\.)*)"'
# Ce qui precede immediatement le groupe : identifiant + parenthese eventuelle.
$rxCaller = [regex]'([A-Za-z_][A-Za-z0-9_:]*)\s*\(\s*$'

$rows = New-Object System.Collections.Generic.List[object]

foreach ($file in Get-ChildItem -Path $Src -Recurse -Include *.cc,*.h) {
  $text  = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
  $lines = $text -split "`n"
  # Offsets de debut de ligne, pour retrouver le numero depuis un index.
  $starts = New-Object System.Collections.Generic.List[int]
  $acc = 0
  foreach ($l in $lines) { $starts.Add($acc); $acc += $l.Length + 1 }

  foreach ($m in $rxGroup.Matches($text)) {
    $joined = ""
    foreach ($p in $rxJoin.Matches($m.Value)) { $joined += $p.Groups[1].Value }
    if ($joined.Length -lt 3) { continue }
    if ($joined.StartsWith('##')) { continue }
    if (($rxLetters.Replace($joined, '')).Length -lt 3) { continue }
    if (-not ($rxAccent.IsMatch($joined) -or $rxWords.IsMatch($joined))) { continue }

    # Contexte immediat : deja traduit ?
    $from = [Math]::Max(0, $m.Index - 40)
    $before = $text.Substring($from, $m.Index - $from)
    if ($before -match 'i18n::Tr(?:Id)?\(\s*$') { continue }

    # Numero de ligne + rejet des commentaires et des logs.
    $lineNo = 1
    for ($i = 0; $i -lt $starts.Count; $i++) { if ($starts[$i] -le $m.Index) { $lineNo = $i + 1 } else { break } }
    $lineTxt = $lines[$lineNo - 1]
    $trimmed = $lineTxt.TrimStart()
    if ($trimmed.StartsWith('//') -or $trimmed.StartsWith('*')) { continue }
    if ($lineTxt -match '\b(LogDiag|LogInfo|LogError|LogDebug|LogWarn|OutputDebugString)\b') { continue }

    $callerMatch = $rxCaller.Match($before)
    $caller = if ($callerMatch.Success) { $callerMatch.Groups[1].Value } else { '<non-appel>' }

    $rows.Add([pscustomobject]@{
      File = $file.Name; Line = $lineNo; Caller = $caller
      Text = if ($joined.Length -gt 60) { $joined.Substring(0,60) + '...' } else { $joined }
    })
  }
}

Write-Output ("Libelles francais encore nus : " + $rows.Count)
Write-Output ""
if ($ByCall) {
  Write-Output "--- par APPELANT (ce qui reste a couvrir) ---"
  $rows | Group-Object Caller | Sort-Object Count -Descending | ForEach-Object {
    "{0,5}  {1}" -f $_.Count, $_.Name
  }
} else {
  Write-Output "--- par FICHIER ---"
  $rows | Group-Object File | Sort-Object Count -Descending | Select-Object -First 30 | ForEach-Object {
    "{0,5}  {1}" -f $_.Count, $_.Name
  }
}
