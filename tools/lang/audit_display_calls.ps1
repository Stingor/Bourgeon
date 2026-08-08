# Liste les appels d'AFFICHAGE dont le libelle est un litteral NU, c'est-a-dire
# non enveloppe dans i18n::Tr / i18n::TrId.
#
# 🔴 Pourquoi ce script existe alors qu'audit_untranslated.ps1 existe deja :
# celui-la DEVINE la langue (« un accent, ou deux mots separes par une espace »),
# et ce critere laisse passer tout libelle d'UN SEUL mot non accentue --
# « Enregistrer », « Fermer », « Annuler », « Filtrer », « Options »... Le
# migrateur partageant exactement le meme filtre, ces libelles-la n'ont JAMAIS
# ete migres, et rien ne le signalait.
#
# Ici on ne devine plus rien : si un appel qui DESSINE du texte recoit un
# litteral, c'est un defaut, quelle que soit la langue du litteral. Le test est
# verifiable au lieu d'etre heuristique.
#
# ASCII pur : cf. l'en-tete de migrate_tr.ps1.
#
# Usage :
#   powershell -File tools\lang\audit_display_calls.ps1
#   powershell -File tools\lang\audit_display_calls.ps1 -Detail
#   powershell -File tools\lang\audit_display_calls.ps1 -Detail -Only chat_window.cc

param(
  [string]$Src = "$PSScriptRoot\..\..\src",
  [switch]$Detail,
  # 🔴 Pas « $File » : la boucle interne s'appelle « $file » et PowerShell ignore
  # la casse -- un parametre type [string] imposerait son type a l'objet fichier.
  [string]$Only = ""
)

# Le libelle est le PREMIER argument.
$callsArg0 = @(
  'ImGui::TextUnformatted', 'ImGui::TextDisabled', 'ImGui::TextWrapped',
  'ImGui::BulletText', 'ImGui::Text',
  'ImGui::SmallButton', 'ImGui::Button',
  'ImGui::MenuItem', 'ImGui::Selectable', 'ImGui::Checkbox', 'ImGui::RadioButton',
  'ImGui::CollapsingHeader', 'ImGui::SeparatorText', 'ImGui::SetTooltip',
  'ImGui::TableSetupColumn', 'ImGui::TreeNode', 'ImGui::BeginTabItem',
  'ImGui::BeginMenu', 'ImGui::CalcTextSize', 'ImGui::OpenPopup',
  'ro::RoCheckbox', 'ro::RoButton', 'ro::RoSmallButton', 'ro::RoSeparatorText',
  'ro::SetNextWindowTitleBullet', 'ro::BeginRoPopupModal', 'ro::RoCombo',
  'BulletWrapped', 'WheelSliderInt', 'WheelSliderFloat',
  'ColorEdit4WithAlphaBar', 'ColorSwatch',
  'TextUnformatted', 'TextDisabled', 'TextWrapped', 'BulletText', 'Text',
  'CollapsingHeader', 'SeparatorText', 'HelpMarker', 'Tooltip',
  'DrawSpinner', 'DrawWaitCover'
)
# Le libelle SUIT la premiere virgule (couleur ImVec4, identifiant de champ).
$callsArg1 = @('ImGui::TextColored', 'ImGui::InputTextWithHint', 'TextColored')

# 🔴 Les litteraux de CARACTERE d'abord : `c == '"'` ouvrirait une fausse chaine
# et decalerait la lecture de tout le fichier. Longueur conservee pour que les
# numeros de ligne restent justes.
$rxCharLit = [regex]"'(?:[^'\\\\]|\\\\.)'"
function Remove-CharLiterals([string]$src) {
  return $rxCharLit.Replace($src, { param($m) "'" + ("x" * ($m.Value.Length - 2)) + "'" })
}

# Les COMMENTAIRES, blanchis a longueur EGALE : ce depot commente en francais et
# cite volontiers du code (« ImGui::Text("truc") »), qui serait sinon compte comme
# un vrai site d'appel. Conserver la longueur garde les numeros de ligne justes.
# Les sauts de ligne sont preserves pour la meme raison.
$rxComment = [regex]'(?s)//[^\r\n]*|/\*.*?\*/'
function Remove-Comments([string]$src) {
  return $rxComment.Replace($src, {
    param($m)
    -join ($m.Value.ToCharArray() | ForEach-Object { if ($_ -eq "`n" -or $_ -eq "`r") { $_ } else { ' ' } })
  })
}

# Ce qui ne reste QUE du format ne se traduit pas : « %s », « %s z », « %d/%d ».
# On retire les specificateurs, et on exige au moins deux lettres au reste.
$rxFormatSpec = [regex]'%[-+ #0]*[0-9*]*(\.[0-9*]+)?(hh|h|ll|l|j|z|t|L|I64|I32)?[diouxXeEfgGaAcspn%]'

# Les motifs se construisent UNE fois, pas par fichier.
# (?<![\w:]) : « Text » ne doit mordre ni sur « BulletText » ni sur « ImGui::Text ».
# Un GROUPE de litteraux adjacents, guillemets compris : le compilateur les
# concatene, donc c'est une seule cle de catalogue.
$grp = '("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)'
$rxJoin = [regex]'"((?:[^"\\]|\\.)*)"'

$probes = @()
foreach ($name in $callsArg0) {
  $probes += [pscustomobject]@{
    Name = $name
    # Un litteral colle a la parenthese ouvrante = argument 0 nu.
    Rx = [regex]('(?<![\w:])' + [regex]::Escape($name) + '\s*\(\s*' + $grp)
  }
}
foreach ($name in $callsArg1) {
  $probes += [pscustomobject]@{
    Name = $name
    # Un seul argument avant la virgule, sans parenthese ni guillemet : cela
    # ecarte les appels imbriques que ce motif simple ne saurait pas suivre.
    Rx = [regex]('(?<![\w:])' + [regex]::Escape($name) + '\s*\(\s*[^,()"]+,\s*' + $grp)
  }
}

# Numero de ligne depuis un index, par dichotomie sur les debuts de ligne.
function Get-LineNo($starts, [int]$index) {
  $lo = 0; $hi = $starts.Count - 1; $res = 1
  while ($lo -le $hi) {
    $mid = [int](($lo + $hi) / 2)
    if ($starts[$mid] -le $index) { $res = $mid + 1; $lo = $mid + 1 } else { $hi = $mid - 1 }
  }
  return $res
}

# 🔴 Classe de lettres en \uXXXX et JAMAIS en caracteres accentues litteraux :
# powershell -File lit un .ps1 sans BOM en ANSI, et un accent dans une regex la
# casse silencieusement.
$rxHasLetter = [regex]'[A-Za-z\u00C0-\u00FF]'

$rows = New-Object System.Collections.Generic.List[object]

foreach ($fileItem in Get-ChildItem -Path $Src -Recurse -Include *.cc,*.h) {
  $raw = [System.IO.File]::ReadAllText($fileItem.FullName, [System.Text.Encoding]::UTF8)
  $text = Remove-Comments (Remove-CharLiterals $raw)
  $starts = New-Object System.Collections.Generic.List[int]
  $acc = 0
  foreach ($l in ($text -split "`n")) { $starts.Add($acc); $acc += $l.Length + 1 }

  foreach ($probe in $probes) {
    foreach ($m in $probe.Rx.Matches($text)) {
      # Les litteraux ADJACENTS sont concatenes par le compilateur : c'est UN
      # seul libelle, et c'est le groupe entier qu'il faudra envelopper.
      $lit = ""
      foreach ($part in $rxJoin.Matches($m.Groups[1].Value)) { $lit += $part.Groups[1].Value }

      # Identifiant ImGui pur : jamais dessine, jamais traduit.
      if ($lit.StartsWith('##')) { continue }
      # Ce qui ne reste que du format ne se traduit pas.
      $body = $rxFormatSpec.Replace($lit, '')
      if (($body -replace '[^A-Za-z\u00C0-\u00FF]', '').Length -lt 2) { continue }

      $rows.Add([pscustomobject]@{
        File = $fileItem.Name
        Line = (Get-LineNo $starts $m.Index)
        Call = $probe.Name
        Text = if ($lit.Length -gt 64) { $lit.Substring(0, 64) + '...' } else { $lit }
      })
    }
  }
}

if ($Only) { $rows = @($rows | Where-Object { $_.File -eq $Only }) }

Write-Output ("appels d'affichage a litteral NU : " + $rows.Count)
Write-Output ""
if ($Detail) {
  $rows | Group-Object File | Sort-Object Count -Descending | ForEach-Object {
    Write-Output ("=== " + $_.Name + "  (" + $_.Count + ") ===")
    $_.Group | Sort-Object Line | ForEach-Object {
      "{0,6}  {1,-28}  {2}" -f $_.Line, $_.Call, $_.Text
    }
  }
} else {
  Write-Output "--- par FICHIER ---"
  $rows | Group-Object File | Sort-Object Count -Descending | ForEach-Object {
    "{0,5}  {1}" -f $_.Count, $_.Name
  }
  Write-Output ""
  Write-Output "--- par APPEL ---"
  $rows | Group-Object Call | Sort-Object Count -Descending | ForEach-Object {
    "{0,5}  {1}" -f $_.Count, $_.Name
  }
}
