# Enveloppe de i18n::Tr() les libelles FRANCAIS passes aux appels d'interface.
#
# Ce fichier est volontairement en ASCII pur, regex Unicode comprises : lance par
# `powershell -File`, Windows PowerShell 5.1 lit un .ps1 sans BOM comme de l'ANSI
# et casse sur le premier accent d'une chaine. Les plages accentuees sont donc
# ecrites en \uXXXX, et les commentaires se passent d'accents.
#
# CE SCRIPT EST VOLONTAIREMENT TIMIDE. Il ne touche QUE le litteral en position
# connue d'un appel connu. Tout le reste -- tables statiques, cles YAML, chemins,
# identifiants ImGui, formats de log -- lui est invisible, et c'est le but :
#
#   - une table statique (const char* k[] = {"Tout", ...}) enveloppee ici
#     COMPILERAIT, mais serait evaluee une seule fois au chargement de la DLL,
#     donc figee en francais pour toujours. Panne muette, la pire du lot ;
#   - un identifiant ImGui traduit change l'identite du widget ;
#   - un message de log traduit rend les journaux incomparables entre joueurs.
#
# Ce qu'il rate se rattrape a la main, et l'export runtime du jeu (panneau
# Interface, bouton Exporter) dit exactement ce qui manque.
#
# Usage :
#   powershell -File tools\lang\migrate_tr.ps1 -Path src\features\windows\x.cc
#   powershell -File tools\lang\migrate_tr.ps1 -Path src -Recurse -WhatIf

param(
  [Parameter(Mandatory=$true)][string]$Path,
  [switch]$Recurse,
  [switch]$WhatIf
)

# Appels d'interface dont le libelle est le PREMIER argument.
# Les plus LONGS d'abord : « TextUnformatted » doit etre essaye avant « Text »,
# sans quoi le second capturerait le debut du premier.
# Les noms non qualifies (Text, Tooltip...) viennent du `using namespace mui` des
# panneaux ; le lookbehind pose plus bas empeche « BulletText » de matcher « Text ».
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
# Appels dont le libelle SUIT la premiere virgule (ImVec4 de couleur, ID de champ).
$callsArg1 = @('ImGui::TextColored', 'ImGui::InputTextWithHint', 'TextColored')

# 🔴 ro::BeginRoWindow N'EST PAS DE LA PARTIE, et ne doit pas y entrer : son titre
# EST l'identifiant de la fenetre (imgui.ini). Le traduire par Tr ferait perdre
# position et taille a chaque changement de langue. Ces titres-la se reprennent a
# la main, en TrId("Titre", "bourgeon_xxx"), un par un.
#
# ⚠ OpenPopup et BeginRoPopupModal y sont, EN COUPLE : c'est la meme chaine qui
# les apparie, donc les envelopper tous les deux les garde d'accord. En traduire
# un seul donnerait un popup qui ne s'ouvre plus.

# Un litteral est du TEXTE UTILISATEUR s'il porte un accent, ou s'il aligne des
# mots separes par des espaces. Identifiants, chemins et gabarits techniques sont
# ecartes.
$rxAccent = [regex]'[\u00C0-\u00FF\u0152\u0153\u00AB\u00BB]'
$rxWords  = [regex]'[A-Za-z\u00C0-\u00FF]{2,} +[A-Za-z\u00C0-\u00FF]'
$rxPath   = [regex]'(\\\\|/|\.(ttf|bmp|tga|png|jpg|yaml|yml|txt|lua|lub|dll|exe|gif|wav|spr|act|str|grf))'
$rxLetters= [regex]'[^A-Za-z\u00C0-\u00FF]'

function Test-IsUserText([string]$lit) {
  if ($lit.Length -lt 2) { return $false }
  if ($lit.StartsWith('##')) { return $false }
  if ($lit -match '###') { return $false }   # titre a ID stable : TrId, a la main
  # Le filtre chemin ne s'applique QU'AUX chaines sans structure de phrase : un
  # texte d'aide a parfaitement le droit de citer SaveData\lang\en.yaml, et le
  # rejeter en bloc laissait sans traduction toutes les explications qui nomment
  # un fichier.
  if ($rxPath.IsMatch($lit) -and -not $rxWords.IsMatch($lit)) { return $false }
  if (($rxLetters.Replace($lit, '')).Length -lt 2) { return $false }
  return ($rxAccent.IsMatch($lit) -or $rxWords.IsMatch($lit))
}

$litGroup = '"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
$rxJoin   = [regex]'"((?:[^"\\]|\\.)*)"'
# 🔴 Les litteraux de CARACTERE d'abord. `c == '"'` contient un guillemet ; sans
# ce nettoyage il ouvre une fausse chaine et TOUT le reste du fichier se decale --
# on s'est retrouve avec des morceaux de code C++ pris pour des libelles.
$rxCharLit = [regex]"'(?:[^'\\\\]|\\\\.)'"
function Remove-CharLiterals([string]$src) {
  return $rxCharLit.Replace($src, { param($m) "'" + ("x" * ($m.Value.Length - 2)) + "'" })
}

function Invoke-Wrap([string]$text, [string[]]$calls, [bool]$afterComma) {
  foreach ($call in $calls) {
    # Lookbehind : empeche un nom court de matcher la FIN d'un nom plus long
    # (« Text » dans « BulletText »). Le « : » n'est pas dans la classe, donc
    # « ImGui::Text » reste atteignable par l'entree non qualifiee « Text ».
    $guard = '(?<![A-Za-z0-9_])'
    $pat = if ($afterComma) {
      $guard + [regex]::Escape($call) + '\s*\((?s:.*?),\s*(' + $litGroup + ')'
    } else {
      $guard + [regex]::Escape($call) + '\s*\(\s*(' + $litGroup + ')'
    }
    $rx = [regex]$pat
    $text = $rx.Replace($text, {
      param($m)
      $lit = $m.Groups[1].Value
      $joined = ""
      foreach ($p in $rxJoin.Matches($lit)) { $joined += $p.Groups[1].Value }
      if (Test-IsUserText $joined) { return $m.Value.Replace($lit, "i18n::Tr($lit)") }
      return $m.Value
    })
  }
  return $text
}

# ── Motifs hors appel ────────────────────────────────────────────────────────
# Trois formes portent l'essentiel de ce que la liste d'appels ne voit pas. Elles
# ont en commun de n'exister QUE dans un corps de fonction : une table statique
# aligne des litteraux separes par des virgules, sans « ? », sans « return » et
# sans snprintf. C'est ce qui les rend sures -- le piege du libelle fige en
# francais ne peut pas se produire ici.
function Invoke-WrapPatterns([string]$text) {
  # 1. Ternaire a DEUX litteraux : « cond ? "oui" : "non" ». Les deux branches se
  #    traduisent, jamais le ternaire entier. La forme complete est exigee, ce qui
  #    ecarte les « case X: » et les « std::string » d'un « : » isole.
  $rxTernary = [regex]('\?\s*(' + $litGroup + ')\s*:\s*(' + $litGroup + ')')
  $text = $rxTernary.Replace($text, {
    param($m)
    $a = $m.Groups[1].Value; $b = $m.Groups[2].Value
    $ja = ""; foreach ($p in $rxJoin.Matches($a)) { $ja += $p.Groups[1].Value }
    $jb = ""; foreach ($p in $rxJoin.Matches($b)) { $jb += $p.Groups[1].Value }
    $okA = Test-IsUserText $ja; $okB = Test-IsUserText $jb
    if (-not ($okA -or $okB)) { return $m.Value }
    $outA = if ($okA) { "i18n::Tr($a)" } else { $a }
    $outB = if ($okB) { "i18n::Tr($b)" } else { $b }
    return "? $outA : $outB"
  })

  # 2. Gabarit de snprintf : 3e argument. Les deux premiers (tampon, taille) ne
  #    contiennent pas de virgule de premier niveau -- « sizeof(buf) » en a une
  #    seulement dans ses parentheses, que [^,()]* n'atteint pas.
  $rxSnprintf = [regex]('snprintf\s*\(\s*[^,]+,\s*[^,]+,\s*(' + $litGroup + ')')
  $text = $rxSnprintf.Replace($text, {
    param($m)
    $lit = $m.Groups[1].Value
    $j = ""; foreach ($p in $rxJoin.Matches($lit)) { $j += $p.Groups[1].Value }
    if (Test-IsUserText $j) { return $m.Value.Replace($lit, "i18n::Tr($lit)") }
    return $m.Value
  })

  # 3bis. Helpers d'affichage du projet, qui prennent PLUSIEURS textes : un
  #       libelle ET sa description (pct("Regen PV", v, "Recuperation...")). On
  #       enveloppe donc TOUS les litteraux francais de l'appel, pas seulement le
  #       premier. Bornes a l'instruction courante pour ne pas deborder.
  foreach ($helper in @('pct','flat','add','bonusStat','row','line','slider','Label',
                        'Help','bw','preset','fail','add_staff','disable_last')) {
    $rxHelper = [regex]('(?<![A-Za-z0-9_])' + [regex]::Escape($helper) + '\s*\((?s:[^;]*?)\)\s*;')
    $text = $rxHelper.Replace($text, {
      param($m)
      $inner = $m.Value
      $rxG = [regex]$litGroup
      return $rxG.Replace($inner, {
        param($g)
        # Deja enveloppe ? La regex de groupe ne voit pas le Tr( qui precede,
        # on le teste sur le texte a gauche du litteral dans l'appel.
        $at = $g.Index
        $left = if ($at -ge 12) { $inner.Substring($at - 12, 12) } else { $inner.Substring(0, $at) }
        if ($left -match 'i18n::Tr(?:Id)?\(\s*$') { return $g.Value }
        $j = ""; foreach ($p in $rxJoin.Matches($g.Value)) { $j += $p.Groups[1].Value }
        if (Test-IsUserText $j) { return "i18n::Tr(" + $g.Value + ")" }
        return $g.Value
      })
    })
  }

  # 3ter. Affectation d'un libelle a une variable d'affichage : « tip = "..." ».
  #       🔴 La ligne DOIT etre indentee. Une declaration en colonne 0 est une
  #       globale ou une table de fichier, initialisee au chargement de la DLL :
  #       un Tr() pose la serait fige en francais pour toujours.
  $rxAssign = [regex]('(?m)^(\s{2,}[A-Za-z_][A-Za-z0-9_\.\->\[\]]*\s*=\s*)(' + $litGroup + ')(\s*;)')
  $text = $rxAssign.Replace($text, {
    param($m)
    $lit = $m.Groups[2].Value
    $j = ""; foreach ($p in $rxJoin.Matches($lit)) { $j += $p.Groups[1].Value }
    if (Test-IsUserText $j) { return $m.Groups[1].Value + "i18n::Tr($lit)" + $m.Groups[3].Value }
    return $m.Value
  })

  # 3. « return "libelle" » : une fonction qui rend un texte d'interface.
  $rxReturn = [regex]('return\s+(' + $litGroup + ')\s*;')
  $text = $rxReturn.Replace($text, {
    param($m)
    $lit = $m.Groups[1].Value
    $j = ""; foreach ($p in $rxJoin.Matches($lit)) { $j += $p.Groups[1].Value }
    if (Test-IsUserText $j) { return "return i18n::Tr($lit);" }
    return $m.Value
  })
  return $text
}

$files = if ($Recurse) { Get-ChildItem -Path $Path -Recurse -Include *.cc } else { Get-Item -Path $Path }
$totalWrapped = 0
$enc = New-Object System.Text.UTF8Encoding($false)   # UTF-8 SANS BOM, comme les sources

foreach ($file in $files) {
  $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
  $orig = $text

  $text = Invoke-Wrap $text $callsArg0 $false
  $text = Invoke-Wrap $text $callsArg1 $true
  $text = Invoke-WrapPatterns $text

  if ($text -ne $orig) {
    if ($text -notmatch '#include "utils/i18n\.h"') {
      $incs = [regex]::Matches($text, '(?m)^#include .*$')
      if ($incs.Count -gt 0) {
        $last = $incs[$incs.Count - 1]
        $text = $text.Substring(0, $last.Index + $last.Length) +
                "`n#include `"utils/i18n.h`"" +
                $text.Substring($last.Index + $last.Length)
      }
    }
    $n = ([regex]::Matches($text, 'i18n::Tr\(')).Count - ([regex]::Matches($orig, 'i18n::Tr\(')).Count
    $totalWrapped += $n
    Write-Output ("{0,5}  {1}" -f $n, $file.Name)
    if (-not $WhatIf) { [System.IO.File]::WriteAllText($file.FullName, $text, $enc) }
  }
}
Write-Output ("TOTAL enveloppe : " + $totalWrapped)
