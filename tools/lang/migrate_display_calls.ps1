# Enveloppe dans i18n::Tr() les libelles NUS des appels d'affichage releves par
# audit_display_calls.ps1.
#
# 🔴 Pourquoi un TROISIEME migrateur : migrate_tr.ps1 et migrate_rest.ps1
# decidaient « est-ce du texte utilisateur ? » sur une heuristique de LANGUE (un
# accent, ou deux mots separes par une espace). Tout libelle d'un seul mot non
# accentue -- « Fermer », « Objet », « Prix », « Oui », « Acheter » -- passait au
# travers, en silence, et il y en avait pres de cinq cents. Ici on ne devine plus
# la langue : on part de la LISTE D'APPELS qui dessinent du texte, et tout
# litteral qu'ils recoivent est enveloppe.
#
# 🔴 La transformation est LOCALE et equilibree PAR CONSTRUCTION : on insere
# « i18n::Tr( » devant le groupe de litteraux et « ) » derriere. Une parenthese
# ouverte, une fermee, rien d'autre n'est touche. C'est la lecon des deux
# corruptions de item_desc_window.cc : un migrateur qui balaie librement le
# fichier se desynchronise, un migrateur ancre sur un nom d'appel connu ne le
# peut pas.
#
# ASCII pur dans les chaines : cf. l'en-tete de migrate_tr.ps1.
#
# Usage :
#   powershell -File tools\lang\migrate_display_calls.ps1            # a blanc
#   powershell -File tools\lang\migrate_display_calls.ps1 -Apply
#   powershell -File tools\lang\migrate_display_calls.ps1 -Apply -Only chat_window.cc

param(
  [string]$Src = "$PSScriptRoot\..\..\src",
  [switch]$Apply,
  # 🔴 Pas « $File » : la boucle interne s'appelle « $file » et PowerShell ignore
  # la casse -- un parametre type [string] imposerait son type a l'objet fichier.
  [string]$Only = "",
  # Fichiers qu'on refuse de toucher par script, quoi qu'il arrive.
  [string[]]$Skip = @('item_desc_window.cc')
)

# Preparation du texte a analyser : commentaires blanchis SANS jamais
# confondre un delimiteur place a l interieur d une chaine. Cf. _scan.ps1.
. "$PSScriptRoot\_scan.ps1"

$callsArg0 = @(
  'ImGui::TextUnformatted', 'ImGui::TextDisabled', 'ImGui::TextWrapped',
  'ImGui::BulletText', 'ImGui::Text',
  'ImGui::SmallButton', 'ImGui::Button',
  'ImGui::MenuItem', 'ImGui::Selectable', 'ImGui::Checkbox', 'ImGui::RadioButton',
  'ImGui::CollapsingHeader', 'ImGui::SeparatorText', 'ImGui::SetTooltip',
  'ImGui::TableSetupColumn', 'ImGui::TreeNode', 'ImGui::BeginTabItem',
  'ImGui::BeginMenu', 'ImGui::CalcTextSize',
  # ⚠⚠ Les TROIS ensemble, jamais l'un sans les autres : c'est la meme chaine qui
  # apparie l'ouverture et le corps du popup. N'en envelopper qu'un donnerait un
  # popup qui ne s'ouvre plus -- en silence, sans erreur ni trace.
  'ImGui::OpenPopup', 'ImGui::BeginPopupModal', 'ImGui::BeginPopup',
  'ro::RoCheckbox', 'ro::RoButton', 'ro::RoSmallButton', 'ro::RoSeparatorText',
  'ro::SetNextWindowTitleBullet', 'ro::BeginRoPopupModal', 'ro::RoCombo',
  'BulletWrapped', 'WheelSliderInt', 'WheelSliderFloat',
  'ColorEdit4WithAlphaBar', 'ColorSwatch',
  'TextUnformatted', 'TextDisabled', 'TextWrapped', 'BulletText', 'Text',
  'CollapsingHeader', 'SeparatorText', 'HelpMarker', 'Tooltip',
  'DrawSpinner', 'DrawWaitCover'
)
$callsArg1 = @('ImGui::TextColored', 'ImGui::InputTextWithHint', 'TextColored')

# 🔴 ro::BeginRoWindow reste HORS de la liste : son titre EST l'identifiant de la
# fenetre dans imgui.ini. Le traduire ferait perdre position et taille a chaque
# bascule de langue. Ces titres-la se reprennent a la main, en TrId.



$rxFormatSpec = [regex]'%[-+ #0]*[0-9*]*(\.[0-9*]+)?(hh|h|ll|l|j|z|t|L|I64|I32)?[diouxXeEfgGaAcspn%]'
$rxJoin = [regex]'"((?:[^"\\]|\\.)*)"'

$grp = '("(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*)'
$probes = @()
foreach ($name in $callsArg0) {
  $probes += [pscustomobject]@{
    Name = $name
    Rx = [regex]('(?<![\w:])' + [regex]::Escape($name) + '\s*\(\s*' + $grp)
  }
}
foreach ($name in $callsArg1) {
  $probes += [pscustomobject]@{
    Name = $name
    Rx = [regex]('(?<![\w:])' + [regex]::Escape($name) + '\s*\(\s*[^,()"]+,\s*' + $grp)
  }
}

$totalSites = 0
$totalFiles = 0
$needInclude = New-Object System.Collections.Generic.List[string]

foreach ($fileItem in Get-ChildItem -Path $Src -Recurse -Include *.cc,*.h) {
  if ($Skip -contains $fileItem.Name) { continue }
  if ($Only -and $fileItem.Name -ne $Only) { continue }

  $raw = [System.IO.File]::ReadAllText($fileItem.FullName, [System.Text.Encoding]::UTF8)
  $scan = Get-ScannableText $raw

  # Toutes les positions a envelopper, puis tri DECROISSANT : on remplace de la
  # fin vers le debut, pour qu'aucun index releve ne soit invalide par le
  # remplacement precedent.
  $spans = New-Object System.Collections.Generic.List[object]
  foreach ($probe in $probes) {
    foreach ($m in $probe.Rx.Matches($scan)) {
      $g = $m.Groups[1]
      $lit = ""
      foreach ($part in $rxJoin.Matches($g.Value)) { $lit += $part.Groups[1].Value }
      if ($lit.StartsWith('##')) { continue }
      # \uD83D\uDD34 Un IDENTIFIANT ImGui nu, pas un libelle : \u00AB picker \u00BB, \u00AB ctx \u00BB,
      # \u00AB skctx \u00BB. Tout en minuscules, d'un seul tenant. Ce depot capitalise
      # systematiquement ses libelles, donc la regle ne mange rien de visible --
      # et elle evite d'envelopper un cote d'une paire OpenPopup/BeginPopup dont
      # l'autre cote ne passe pas par la meme fonction.
      if ($lit -cmatch '^[a-z0-9_]+$') { continue }
      # 🔴 Un nom de commande SERVEUR (@autolootid, @mobinfo) n'est pas du texte :
      # c'est un identifiant, il doit s'ecrire pareil dans toutes les langues. Et
      # panel_commands en fait des TreeNode, dont l'etat ouvert/ferme est indexe
      # sur le libelle -- le traduire replierait l'arbre a chaque bascule.
      if ($lit.StartsWith('@')) { continue }
      # 🔴 Une URL ne se traduit JAMAIS, et un gabarit qui ne laisse qu un
      # identifiant minuscule apres retrait des specificateurs non plus :
      # « tabh%s » nomme une texture, « %s_db » un widget ImGui.
      if ($lit -match '^(https?|ftp)://') { continue }
      if (($rxFormatSpec.Replace($lit, '')) -cmatch '^[a-z0-9_.#-]*$') { continue }
      $body = $rxFormatSpec.Replace($lit, '')
      if (($body -replace '[^A-Za-z\u00C0-\u00FF]', '').Length -lt 2) { continue }
      $spans.Add([pscustomobject]@{ Start = $g.Index; Length = $g.Length; Call = $probe.Name; Text = $lit })
    }
  }
  if ($spans.Count -eq 0) { continue }

  # Deux motifs peuvent couvrir la meme zone : on garde le premier et on ecarte
  # tout chevauchement, sinon on insererait deux enveloppes imbriquees.
  $ordered = @($spans | Sort-Object Start)
  $kept = New-Object System.Collections.Generic.List[object]
  $lastEnd = -1
  foreach ($s in $ordered) {
    if ($s.Start -lt $lastEnd) { continue }
    $kept.Add($s)
    $lastEnd = $s.Start + $s.Length
  }

  $totalSites += $kept.Count
  $totalFiles++
  if ($raw -notmatch '#include\s+"utils/i18n\.h"') { $needInclude.Add($fileItem.Name) }

  if (-not $Apply) {
    Write-Output ("=== " + $fileItem.Name + "  (" + $kept.Count + ") ===")
    $kept | ForEach-Object {
      "        {0,-28}  {1}" -f $_.Call, $(if ($_.Text.Length -gt 60) { $_.Text.Substring(0,60) + '...' } else { $_.Text })
    }
    continue
  }

  $out = $raw
  for ($i = $kept.Count - 1; $i -ge 0; $i--) {
    $s = $kept[$i]
    $literal = $out.Substring($s.Start, $s.Length)
    $out = $out.Substring(0, $s.Start) + 'i18n::Tr(' + $literal + ')' + $out.Substring($s.Start + $s.Length)
  }
  [System.IO.File]::WriteAllText($fileItem.FullName, $out, (New-Object System.Text.UTF8Encoding($false)))
}

Write-Output ""
Write-Output ("sites : " + $totalSites + "  dans " + $totalFiles + " fichiers" + $(if ($Apply) { " -- APPLIQUE" } else { " -- a blanc" }))
if ($needInclude.Count) {
  Write-Output ""
  # 🔴 Chaines en SIMPLE quote et sans emoji : ce script est lu en ANSI par
  # powershell -File, et un guillemet precede d'un antislash n'y est pas echappe
  # -- il FERME la chaine.
  Write-Output ('include utils/i18n.h MANQUANT dans ' + $needInclude.Count + ' fichier(s) -- a ajouter A LA MAIN :')
  Write-Output '   (jamais par script : dans doom.cc l include s etait retrouve dans un bloc extern "C", ce qui ne compile pas)'
  $needInclude | Sort-Object -Unique | ForEach-Object { "   - " + $_ }
}
