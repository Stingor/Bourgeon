# Repere les libelles NUS que ni la liste d appels ni l heuristique de langue ne
# peuvent trouver : ceux qu on passe a une fonction MAISON.
#
# 🔴 Le principe, et il ne devine rien : si une meme fonction recoit, A LA MEME
# POSITION D ARGUMENT, des litteraux tantot enveloppes dans i18n::Tr et tantot
# nus, ce sont les nus qui sont suspects. Ses voisines prouvent que cette place
# porte du texte affiche.
#
# C est ce qui a rattrape « Expulser (kick) » dans entity_context_menu : passe a
# la lambda locale add_staff, que ni audit_display_calls (elle n est dans aucune
# liste) ni audit_untranslated (pas d accent, et « Expulser (kick) » echoue au
# test « deux mots » puisque la parenthese n est pas une lettre) ne pouvaient
# voir. Ses huit voisines, elles, etaient bien enveloppees.
#
# ASCII pur dans les chaines : cf. l en-tete de migrate_tr.ps1.
#
# Usage :
#   powershell -File tools\lang\audit_mixed_args.ps1
#   powershell -File tools\lang\audit_mixed_args.ps1 -Only entity_context_menu.cc

param(
  [string]$Src = "$PSScriptRoot\..\..\src",
  [string]$Only = "",
  # Enveloppe les libelles trouves. L analyse est EXACTEMENT la meme que celle du
  # rapport -- c est tout l interet de ne pas en faire un second script : deux
  # analyses separees finiraient par diverger, et l une envelopperait ce que
  # l autre n a jamais montre.
  [switch]$Apply,
  [string[]]$Skip = @('item_desc_window.cc')
)

# Preparation du texte a analyser : commentaires blanchis SANS jamais
# confondre un delimiteur place a l interieur d une chaine. Cf. _scan.ps1.
. "$PSScriptRoot\_scan.ps1"

# 🔴 Le titre d une fenetre EST son identifiant dans imgui.ini. On ne l enveloppe
# QUE s il porte deja un « ### » : ce qui suit est alors l identifiant, et la
# traduction voyage devant sans rien deplacer. Sans « ### », traduire perdrait
# position et taille a chaque bascule de langue -- ces titres-la se reprennent a
# la main, en TrId.
$windowTitleCalls = @('ImGui::Begin', 'ro::BeginRoWindow')


# Mots-cles qui portent une parenthese sans etre des appels.
# Mots-cles, et CONSTRUCTEURS trop generiques pour que le test des voisines ait
# un sens : std::string est appele partout, y compris pour des messages de log,
# et son « argument 0 » ne designe donc aucune place stable.
$keywords = @('if','while','for','switch','return','sizeof','catch','and','or','not',
              'std::string','std::wstring','std::string_view')

# \u26A0 Classe d IDENTIFIANT C++ : lettres, chiffres et souligne. Rien a voir avec la
# classe de LETTRES utilisee plus bas pour juger un libelle -- ne pas les
# confondre, ni les remplacer l une par l autre.
$rxCall = [regex]'(?<![\w:.>])([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)\s*\('
$rxWrapped = [regex]'^i18n::Tr(?:Id)?\s*\('
$rxLitGroup = [regex]'^"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*$'
$rxJoin = [regex]'"((?:[^"\\]|\\.)*)"'
$rxFormatSpec = [regex]'%[-+ #0]*[0-9*]*(\.[0-9*]+)?(hh|h|ll|l|j|z|t|L|I64|I32)?[diouxXeEfgGaAcspn%]'

# Decoupe les arguments de PREMIER niveau, en respectant parentheses, crochets,
# accolades et chaines. Rend $null si la parenthese ne se referme pas.
function Split-Args([string]$text, [int]$open) {
  # 🔴 PAS « $args » : c est une variable AUTOMATIQUE de PowerShell (les arguments
  # de la fonction). L ecraser donne des retours incoherents.
  $items = New-Object System.Collections.Generic.List[string]
  $depth = 1
  $start = $open + 1
  $i = $start
  while ($i -lt $text.Length) {
    $c = $text[$i]
    if ($c -eq '"') {
      $i++
      while ($i -lt $text.Length) {
        if ($text[$i] -eq '\') { $i += 2; continue }
        if ($text[$i] -eq '"') { break }
        $i++
      }
    } elseif ($c -eq '(' -or $c -eq '[' -or $c -eq '{') {
      $depth++
    } elseif ($c -eq ')' -or $c -eq ']' -or $c -eq '}') {
      $depth--
      if ($depth -eq 0) {
        $items.Add($text.Substring($start, $i - $start))
        # 🔴 La VIRGULE de tete est indispensable : sans elle PowerShell deroule
        # la liste dans le pipeline, et une liste d UN seul element revient en
        # CHAINE -- $parts[0] rendrait alors un CARACTERE.
        return ,$items
      }
    } elseif ($c -eq ',' -and $depth -eq 1) {
      $items.Add($text.Substring($start, $i - $start))
      $start = $i + 1
    }
    $i++
  }
  return $null  # parenthese non refermee : on ne devine pas
}

function Get-LineNo($starts, [int]$index) {
  $lo = 0; $hi = $starts.Count - 1; $res = 1
  while ($lo -le $hi) {
    $mid = [int](($lo + $hi) / 2)
    if ($starts[$mid] -le $index) { $res = $mid + 1; $lo = $mid + 1 } else { $hi = $mid - 1 }
  }
  return $res
}

# Un litteral qui ne se traduit pas : identifiant ImGui, identifiant nu,
# commande serveur, format sans texte. Memes regles que les autres outils.
# Un texte a des MOTS separes par des espaces, ou des accents. Sert uniquement a
# sauver un gabarit qui contient un chemin tout en etant une vraie phrase.
$rxSentence = [regex]'[A-Za-z\u00C0-\u00FF]{2,}[ ,:;.!?]+[A-Za-z\u00C0-\u00FF]|[\u00C0-\u00FF]'
# Chemin, nom de fichier, fragment de JSON : jamais du texte a traduire.
$rxTechnical = [regex]'\\\\|/|\.(bmp|png|tga|jpg|ttf|yaml|yml|txt|lua|lub|dll|exe|gif|wav|spr|act|str|grf)\b|\{\\"|\\"\}|^\w+:'

function Test-Translatable([string]$lit) {
  if ($lit.StartsWith('##')) { return $false }
  if ($lit.StartsWith('@'))  { return $false }
  if ($lit -cmatch '^[a-z0-9_]+$') { return $false }
  # 🔴 Une URL ne se traduit JAMAIS. Le test de phrase plus bas la sauverait a
  # tort : « moonlight-destiny.fr » y ressemble a « mot . mot ».
  if ($lit -match '^(https?|ftp)://') { return $false }
  # Ce qui reste APRES retrait des specificateurs doit ressembler a du texte. Un
  # identifiant tout en minuscules n en est pas : « tabh%s » construit un nom de
  # texture, « %s_db » un identifiant ImGui.
  if (($rxFormatSpec.Replace($lit, '')) -cmatch '^[a-z0-9_.-]*$') { return $false }
  # Un gabarit technique n est ecarte que s il n a PAS de structure de phrase : un
  # texte d aide a le droit de citer SaveData\lang\en.yaml.
  if ($rxTechnical.IsMatch($lit) -and -not $rxSentence.IsMatch($lit)) { return $false }
  $body = $rxFormatSpec.Replace($lit, '')
  return (($body -replace '[^A-Za-z\u00C0-\u00FF]', '').Length -ge 2)
}

$slots = @{}   # "nom#index" -> @{ Wrapped = n; Bare = liste }

foreach ($fileItem in Get-ChildItem -Path $Src -Recurse -Include *.cc,*.h) {
  $raw = [System.IO.File]::ReadAllText($fileItem.FullName, [System.Text.Encoding]::UTF8)
  $text = Get-ScannableText $raw
  $starts = New-Object System.Collections.Generic.List[int]
  $acc = 0
  foreach ($l in ($text -split "`n")) { $starts.Add($acc); $acc += $l.Length + 1 }

  foreach ($m in $rxCall.Matches($text)) {
    $name = $m.Groups[1].Value
    if ($keywords -contains $name) { continue }
    $open = $m.Index + $m.Length - 1
    $parts = Split-Args $text $open
    if ($null -eq $parts) { continue }

    for ($i = 0; $i -lt $parts.Count; $i++) {
      $arg = $parts[$i].Trim()
      if (-not $arg) { continue }
      $key = $name + '#' + $i
      if (-not $slots.ContainsKey($key)) {
        $slots[$key] = @{ Wrapped = 0; Bare = (New-Object System.Collections.Generic.List[object]) }
      }
      if ($rxWrapped.IsMatch($arg)) {
        $slots[$key].Wrapped++
      } elseif ($rxLitGroup.IsMatch($arg)) {
        $lit = ""
        foreach ($p in $rxJoin.Matches($arg)) { $lit += $p.Groups[1].Value }
        if (-not (Test-Translatable $lit)) { continue }
        # Position EXACTE du groupe de litteraux dans le texte, pour pouvoir
        # l envelopper : le decoupage rend l argument espaces compris.
        $argStart = $open + 1
        for ($k = 0; $k -lt $i; $k++) { $argStart += $parts[$k].Length + 1 }
        $lead = $parts[$i].Length - $parts[$i].TrimStart().Length
        $slots[$key].Bare.Add([pscustomobject]@{
          File = $fileItem.Name
          Path = $fileItem.FullName
          Line = (Get-LineNo $starts $m.Index)
          Start = $argStart + $lead
          Length = $arg.Length
          Call = $name
          Text = if ($lit.Length -gt 60) { $lit.Substring(0, 60) + '...' } else { $lit }
          HasStableId = $lit.Contains('###')
        })
      }
    }
  }
}

$rows = New-Object System.Collections.Generic.List[object]
foreach ($key in $slots.Keys) {
  $slot = $slots[$key]
  if ($slot.Wrapped -eq 0 -or $slot.Bare.Count -eq 0) { continue }
  $parts = $key -split '#'
  foreach ($b in $slot.Bare) {
    if ($Skip -contains $b.File) { continue }
    # Titre de fenetre sans identifiant stable : hors de portee du script.
    if (($windowTitleCalls -contains $b.Call) -and -not $b.HasStableId) { continue }
    $rows.Add([pscustomobject]@{
      File = $b.File; Path = $b.Path; Line = $b.Line
      Call = $parts[0]; Arg = [int]$parts[1]
      Start = $b.Start; Length = $b.Length
      Wrapped = $slot.Wrapped; Text = $b.Text
    })
  }
}
if ($Only) { $rows = @($rows | Where-Object { $_.File -eq $Only }) }

Write-Output ("litteraux NUS a une place ou leurs voisines sont traduites : " + $rows.Count)
Write-Output ""
$rows | Sort-Object File, Line | ForEach-Object {
  "{0,-28} {1,6}  {2}(arg {3}, {4} voisines traduites)  {5}" -f `
    $_.File, $_.Line, $_.Call, $_.Arg, $_.Wrapped, $_.Text
}

if (-not $Apply) { return }

Write-Output ""
# Par fichier, et de la FIN vers le DEBUT : chaque insertion decale ce qui suit.
$byFile = $rows | Group-Object Path
foreach ($group in $byFile) {
  $path = $group.Name
  $out = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
  $ordered = @($group.Group | Sort-Object Start -Descending)
  $done = 0
  $lastStart = [int]::MaxValue
  foreach ($r in $ordered) {
    # Deux motifs peuvent decrire la meme zone : on n enveloppe qu une fois.
    if ($r.Start -ge $lastStart) { continue }
    $literal = $out.Substring($r.Start, $r.Length)
    # Garde-fou : ce qu on remplace DOIT etre le groupe de litteraux qu on a
    # analyse. Si les positions ont glisse, on n ecrit rien.
    if (-not $rxLitGroup.IsMatch($literal)) {
      Write-Output ("  ! position suspecte, ignoree : " + $r.File + ":" + $r.Line)
      continue
    }
    $out = $out.Substring(0, $r.Start) + 'i18n::Tr(' + $literal + ')' +
           $out.Substring($r.Start + $r.Length)
    $lastStart = $r.Start
    $done++
  }
  if ($done -gt 0) {
    [System.IO.File]::WriteAllText($path, $out, (New-Object System.Text.UTF8Encoding($false)))
    Write-Output ("  {0,4}  {1}" -f $done, (Split-Path $path -Leaf))
  }
}
