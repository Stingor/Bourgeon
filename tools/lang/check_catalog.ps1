# Confronte les cles i18n::Tr()/TrId() du code aux entrees d'un catalogue.
#
# Il attrape les pannes de traduction qui ne se voient PAS a l'ecran, parce que
# Tr retombe silencieusement en francais dans presque tous les cas :
#   - chaine migree SANS entree au catalogue -> elle sortira en francais ;
#   - entree ORPHELINE -> un texte source a ete retouche, sa traduction est morte ;
#   - valeur VIDE -> Tr la traite comme absente ;
#   - ESPACE DE BORD perdu ou ajoute -> le texte se decale d'un cran a l'ecran.
#
# 🔴 IL SORT EN CODE 1 QUAND IL TROUVE QUELQUE CHOSE : c'est ce qui le rend
# utilisable en integration continue (.github/workflows/i18n.yml). Le prix du
# choix « le francais EST la cle », c'est qu'une retouche de libelle tue sa
# traduction sans un mot ; ce script est le seul endroit ou ca fait du bruit.
#
# Usage :
#   pwsh -File tools/lang/check_catalog.ps1
#   pwsh -File tools/lang/check_catalog.ps1 -Catalog tools/lang/es.yaml
#
# ⚠ L'extraction est STATIQUE : elle ne voit que les litteraux passes a Tr. Une
# chaine venant d'une table (Tr(kCats[c].label)) n'y figure pas et ressortira en
# ORPHELINE -- c'est normal, elle est legitime. Seul l'export runtime du jeu
# (panneau Interface, bouton « Exporter ») les connait toutes.

# ⚠ [IO.Path]::Combine et non une chaine a backslashes : ce script tourne aussi
# sur le runner LINUX de la CI, ou « \ » n'est pas un separateur de chemin mais un
# caractere ordinaire -- le defaut ne resolvait alors rien du tout. Combine rend
# le separateur natif de la plateforme, et existe aussi bien en 5.1 qu'en 7.
param(
  [string]$Src     = [IO.Path]::Combine($PSScriptRoot, '..', '..', 'src'),
  [string]$Catalog = [IO.Path]::Combine($PSScriptRoot, 'en.yaml')
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
# Les DEUX formes admises au catalogue. La seconde existe pour une raison
# precise : la spec YAML limite une cle IMPLICITE (`cle: valeur`) a 1024
# caracteres, et yaml-cpp l'applique. Au-dela il faut la forme EXPLICITE, sans
# limite -- sinon le jeu perd le fichier ENTIER, pas la seule ligne fautive :
#     ? "la tres longue cle"
#     : "sa traduction"
$rxEntry   = [regex]'^"((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)"\s*$'
$rxExplKey = [regex]'^\?\s+"((?:[^"\\]|\\.)*)"\s*$'
$rxExplVal = [regex]'^:\s*"((?:[^"\\]|\\.)*)"\s*$'

# Marge sous les 1024 octets, la meme que i18n.cc (kMaxImplicitKeyBytes).
$maxImplicitKeyBytes = 1000

# 🔴 L'ESPACE DE BORD FAIT PARTIE DE LA CLE, DONC DE LA TRADUCTION.
# Des cles commencent ou finissent par une espace, pour deux raisons legitimes :
# membre gauche d'une concatenation (« Conversation avec » + le nom du joueur) ou
# retrait d'affichage ("  Verrouille"). Une traduction qui perd cette espace -- ou
# qui en ajoute une -- ne fait broncher ni le compilateur ni les controles
# ci-dessus : la cle est bien traduite, l'entree n'est pas orpheline, la valeur
# n'est pas vide. A l'ecran, tout le texte se decale d'un cran, ou deux mots se
# collent. Vecu le 2026-08-08 : es.yaml avait 2 202 valeurs precedees d'un espace
# parasite, et les 16 cles a espace FINAL l'avaient perdu -- la meme espace,
# deplacee de la fin vers le debut. Un editeur qui rogne les fins de ligne le
# produit tout seul.
# La regle est mecanique et donc verifiable : meme prefixe, meme suffixe.
$rxLead  = [regex]'^[ \t]*'
$rxTrail = [regex]'[ \t]*$'
# Rend une espace visible dans le rapport : sans ca, « attendu ' ' trouve '' »
# s'imprime comme deux paires de quotes qu'on ne sait pas distinguer.
function Show-Edge([string]$s) {
  if (-not $s) { return "(rien)" }
  return ($s -replace ' ', '<esp>' -replace "`t", '<tab>')
}

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
$tooLong = New-Object System.Collections.Generic.List[string]
$edges  = New-Object System.Collections.Generic.List[string]

# Enregistre une entree, quelle que soit la forme dont elle vient.
$record = {
  param($k, $v)
  if (-not $catKeys.Add($k)) { $dupes.Add($k) }
  if (-not $v) { $empty.Add($k) }
  # ⚠ Seulement sur une valeur NON vide : une entree vide est deja signalee plus
  # haut, et un gabarit a moitie traduit sortirait sinon en centaines de faux
  # positifs.
  if ($v) {
    $kLead  = $rxLead.Match($k).Value
    $vLead  = $rxLead.Match($v).Value
    $kTrail = $rxTrail.Match($k).Value
    $vTrail = $rxTrail.Match($v).Value
    if ($kLead -ne $vLead -or $kTrail -ne $vTrail) {
      $edges.Add(("debut " + (Show-Edge $kLead) + " -> " + (Show-Edge $vLead) +
                  " | fin " + (Show-Edge $kTrail) + " -> " + (Show-Edge $vTrail) +
                  "   cle: " + $k.Substring(0, [Math]::Min(50, $k.Length))))
    }
  }
}

$pendingKey = $null
foreach ($line in [System.IO.File]::ReadAllLines($Catalog, [System.Text.Encoding]::UTF8)) {
  $t = $line.Trim()
  if (-not $t -or $t.StartsWith('#')) { continue }

  if ($null -ne $pendingKey) {
    # Une cle explicite a ete ouverte : la ligne suivante DOIT porter sa valeur.
    $mv = $rxExplVal.Match($t)
    if ($mv.Success) { & $record $pendingKey $mv.Groups[1].Value }
    else { $broken.Add($t.Substring(0, [Math]::Min(70, $t.Length))) }
    $pendingKey = $null
    continue
  }

  $mk = $rxExplKey.Match($t)
  if ($mk.Success) { $pendingKey = $mk.Groups[1].Value; continue }

  $m = $rxEntry.Match($t)
  if (-not $m.Success) { $broken.Add($t.Substring(0, [Math]::Min(70, $t.Length))); continue }
  $k = $m.Groups[1].Value
  # 🔴 La cle telle qu'ECRITE, guillemets compris : c'est ce que compte yaml-cpp.
  # Trop longue en forme implicite = fichier illisible au chargement, sans que
  # rien ici ne s'en apercoive -- d'ou ce controle.
  $bytes = [System.Text.Encoding]::UTF8.GetByteCount($k) + 2
  if ($bytes -gt $maxImplicitKeyBytes) {
    $tooLong.Add(("" + $bytes + " octets : " + $k.Substring(0, [Math]::Min(60, $k.Length))))
  }
  & $record $k $m.Groups[2].Value
}
if ($null -ne $pendingKey) { $broken.Add("cle explicite sans valeur : " + $pendingKey.Substring(0, [Math]::Min(60, $pendingKey.Length))) }

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
if ($tooLong.Count) {
  Write-Output ("CLES TROP LONGUES pour la forme implicite : " + $tooLong.Count)
  Write-Output "   (a reecrire en forme explicite : ligne '? \"cle\"' puis ligne ': \"valeur\"')"
  $tooLong | ForEach-Object { "   ! " + $_ }
}
if ($edges.Count) {
  Write-Output ("ESPACES DE BORD qui ne suivent pas la cle : " + $edges.Count)
  Write-Output "   (la valeur doit avoir EXACTEMENT le meme prefixe et le meme suffixe d'espaces que sa cle)"
  $edges | ForEach-Object { "   ! " + $_ }
}
if ($broken.Count) {
  Write-Output ("LIGNES NON PARSEES : " + $broken.Count)
  $broken | ForEach-Object { "   ! " + $_ }
}
Write-Output ""
Write-Output "Fichiers migres :"
$perFile.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { "{0,5}  {1}" -f $_.Value, $_.Key }

if ($missing.Count -or $empty.Count -or $dupes.Count -or $broken.Count -or $tooLong.Count -or $edges.Count) {
  Write-Output ""
  Write-Output "ECHEC : le catalogue et le code ont diverge (voir ci-dessus)."
  exit 1
}
Write-Output ""
Write-Output "OK : le catalogue suit le code."
