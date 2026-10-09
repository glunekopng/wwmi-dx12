# Run through wwmi_dx12_launch.bat, from the game's Client\Binaries\Win64 folder.
# Paths go through .NET: the PowerShell provider reads [ ] in a folder name as a wildcard.
$ErrorActionPreference = "Stop"
$dir    = $PSScriptRoot
$proc   = "Client-Win64-Shipping"
$proxy  = [IO.Path]::Combine($dir, "dxgi.dll")
$ini    = [IO.Path]::Combine($dir, "wwmi_dx12.ini")

function Wait-Key {
    Write-Host "Press any key to exit."
    try { [void][Console]::ReadKey($true) } catch { Start-Sleep -Seconds 15 }
}
function Fail([string]$msg) {
    Write-Host ""
    Write-Host $msg -ForegroundColor Red
    Wait-Key
    exit 1
}
trap {
    Write-Host ""
    Write-Host "The launcher stopped on an error: $_" -ForegroundColor Red
    Wait-Key
    exit 2
}

$wwmiKey = [regex]'(?m)^[ \t]*wwmi_dir[ \t]*=([^\r\n]*)'
function Get-WwmiSetting {
    if (-not [IO.File]::Exists($ini)) { return "" }
    $m = $wwmiKey.Match([IO.File]::ReadAllText($ini))
    if ($m.Success) { return $m.Groups[1].Value.Trim() }
    return ""
}
function Save-WwmiSetting([string]$value) {
    $text = if ([IO.File]::Exists($ini)) { [IO.File]::ReadAllText($ini) } else { "[wwmi-dx12]`r`n" }
    if (-not $wwmiKey.IsMatch($text)) { $text = ([regex]'(?m)^\[wwmi-dx12\][^\r\n]*').Replace($text, "`$0`r`nwwmi_dir =", 1) }
    $text = $wwmiKey.Replace($text, "wwmi_dir = $value".Replace('$', '$$'), 1)
    [IO.File]::WriteAllText($ini, $text, [Text.Encoding]::Unicode)
}

function Get-ReShadeAddonPath([string]$file) {
    if (-not [IO.File]::Exists($file)) { return "" }
    $inAddon = $false
    foreach ($line in [IO.File]::ReadAllLines($file, [Text.Encoding]::UTF8)) {
        $t = $line.Trim()
        if ($t.StartsWith("[")) { $inAddon = ($t -ieq "[ADDON]"); continue }
        if ($inAddon -and $t -match '^AddonPath\s*=(.*)$') { return $Matches[1].Trim() }
    }
    return ""
}

function Resolve-Dir([string]$p) {
    if (-not $p) { return "" }
    try {
        if ([IO.Path]::IsPathRooted($p)) { return $p }
        return [IO.Path]::GetFullPath([IO.Path]::Combine($dir, $p))
    } catch { return "" }
}
function Test-WwmiDir([string]$p) {
    if (-not $p) { return $false }
    try {
        return [IO.File]::Exists([IO.Path]::Combine($p, "d3dx.ini")) -and [IO.Directory]::Exists([IO.Path]::Combine($p, "Core\WWMI"))
    } catch { return $false }
}
function Find-WwmiNearGame([string]$from) {
    $found = @()
    $seen = @{}
    $p = $from
    for ($up = 0; $up -lt 3 -and $p; $up++) {
        $p = [IO.Path]::GetDirectoryName($p)
        if (-not $p -or $seen[$p]) { break }
        $seen[$p] = $true
        Write-Host "  looking in $p ..."
        $dirs = @(Get-ChildItem -LiteralPath $p -Directory -Recurse -Depth 2 -ErrorAction SilentlyContinue)
        foreach ($d in $dirs) {
            $cand = $d.FullName
            if ((Test-WwmiDir $cand) -and ($found -notcontains $cand)) { $found += $cand }
            $xxmi = [IO.Path]::Combine($cand, "WWMI")
            if ([IO.File]::Exists([IO.Path]::Combine($cand, "XXMI Launcher Config.json")) -and (Test-WwmiDir $xxmi) -and ($found -notcontains $xxmi)) { $found += $xxmi }
        }
        if ($found.Count -gt 0) { break }
    }
    return ,$found
}
function Select-Found($found) {
    if ($found.Count -eq 1) { return $found[0] }
    for ($i = 0; $i -lt $found.Count; $i++) { Write-Host "  [$($i + 1)] $($found[$i])" }
    $pick = Read-Host "Which one (1-$($found.Count))"
    $n = 0
    if (-not [int]::TryParse($pick, [ref]$n) -or $n -lt 1 -or $n -gt $found.Count) { Fail "No folder chosen." }
    return $found[$n - 1]
}
function Test-ReShade([string]$dll) {
    return [Diagnostics.FileVersionInfo]::GetVersionInfo($dll).ProductName -match "ReShade"
}
function Test-GameRunning { return [bool](Get-Process $proc -ErrorAction SilentlyContinue) }

if (-not [IO.File]::Exists([IO.Path]::Combine($dir, "$proc.exe"))) {
    Fail "This launcher must be in the game's Client\Binaries\Win64 folder (next to $proc.exe)."
}
if (Test-GameRunning) { Fail "The game is already running." }
$mutex = New-Object Threading.Mutex($false, "wwmi_dx12_launch")
$owned = $false
try { $owned = $mutex.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owned = $true }
if (-not $owned) { Fail "The launcher is already running (look for its window)." }

$root = [IO.Path]::GetFullPath([IO.Path]::Combine($dir, "..\..\.."))
$exe = [IO.Path]::Combine($root, "Wuthering Waves.exe")
if (-not [IO.File]::Exists($exe)) { Fail "Wuthering Waves.exe not found in $root." }

$addonDir = $dir
$addonPath = Get-ReShadeAddonPath ([IO.Path]::Combine($dir, "ReShade.ini"))
if ($addonPath) { $addonDir = Resolve-Dir $addonPath }
$addonHere = [IO.Path]::Combine($dir, "wwmi_dx12.addon64")
if ($addonDir -and [IO.File]::Exists($addonHere)) {
    $addonThere = [IO.Path]::GetFullPath([IO.Path]::Combine($addonDir, "wwmi_dx12.addon64"))
    if (($addonThere -ine [IO.Path]::GetFullPath($addonHere)) -and
        (-not [IO.File]::Exists($addonThere) -or [IO.File]::GetLastWriteTimeUtc($addonHere) -gt [IO.File]::GetLastWriteTimeUtc($addonThere))) {
        [void][IO.Directory]::CreateDirectory($addonDir)
        [IO.File]::Copy($addonHere, $addonThere, $true)
        Write-Host "wwmi_dx12.addon64 copied to $addonDir (ReShade.ini's AddonPath)."
    }
}
if (-not $addonDir -or -not [IO.File]::Exists([IO.Path]::Combine($addonDir, "wwmi_dx12.addon64"))) {
    Fail "wwmi_dx12.addon64 is not in '$addonDir' (where ReShade loads add-ons from; ReShade.ini says AddonPath='$addonPath'). See README: Install."
}

$wwmi = Get-WwmiSetting
$resolved = Resolve-Dir $wwmi
$save = $false
if (-not (Test-WwmiDir $resolved)) {
    $save = $true
    if ($wwmi) { Write-Host "wwmi_dir in wwmi_dx12.ini is not a WWMI folder: $wwmi" -ForegroundColor Yellow }
    $xxmi = if ($env:APPDATA) { [IO.Path]::Combine($env:APPDATA, "XXMI Launcher\WWMI") } else { "" }
    $near = @()
    if (-not (Test-WwmiDir $xxmi)) {
        Write-Host "Looking for WWMI near the game..."
        $near = Find-WwmiNearGame ([IO.Path]::GetFullPath([IO.Path]::Combine($dir, "..\..\..")))
    }
    if (Test-WwmiDir $xxmi) {
        $resolved = $xxmi
        Write-Host "Found WWMI in XXMI Launcher's folder: $xxmi"
    } elseif ($near.Count -gt 0) {
        $resolved = Select-Found $near
        Write-Host "Found WWMI near the game: $resolved"
    } else {
        $answer = (Read-Host "Path to your WWMI folder (the one with d3dx.ini), or Enter to search all drives").Trim('" ')
        if ($answer) {
            if (-not (Test-WwmiDir $answer)) { Fail "Not a WWMI folder (needs d3dx.ini and Core\WWMI): $answer" }
            $resolved = $answer
        } else {
            Write-Host "Searching fixed drives for WWMI (this can take a few minutes)..."
            $found = @()
            foreach ($drv in [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady }) {
                Get-ChildItem -LiteralPath $drv.RootDirectory.FullName -Filter "d3dx.ini" -Recurse -File -ErrorAction SilentlyContinue |
                    ForEach-Object {
                        $cand = $_.Directory.FullName
                        if ((Test-WwmiDir $cand) -and ($found -notcontains $cand)) { $found += $cand; Write-Host "  found: $cand" }
                    }
            }
            if ($found.Count -eq 0) { Fail "No WWMI folder found. Install WWMI (XXMI Launcher) first, or enter its path." }
            $resolved = Select-Found $found
        }
    }
}
if ($resolved.Length -ge 248) { Fail "The WWMI folder's path is too long for the add-on ($($resolved.Length) characters; under 248 works): $resolved" }
if ($save) {
    try { Save-WwmiSetting $resolved } catch { Fail "Could not write wwmi_dx12.ini in $dir -- is the game folder read-only?" }
    Write-Host "WWMI folder saved to wwmi_dx12.ini: $resolved"
}

if (-not [IO.File]::Exists($proxy)) {
    Fail "ReShade not found. Install ReShade with full add-on support into this folder. See README."
}
if (-not (Test-ReShade $proxy)) {
    Fail "dxgi.dll in this folder is not ReShade. Remove or rename it, then install ReShade (with full add-on support) here."
}

# Read-only while the game starts: it rewrites Engine.ini and drops settings it does not keep.
$engineIni = [IO.Path]::Combine($root, "Client\Saved\Config\WindowsNoEditor\Engine.ini")
$cvars = [ordered]@{
    "r.TextureStreaming"                = "1"
    "r.Streaming.FullyLoadUsedTextures" = "1"
    "r.Streaming.Boost"                 = "30"
    "r.Streaming.MinBoost"              = "30"
    "r.Streaming.HLODStrategy"          = "2"
    "r.MipMapLODBias"                   = "-1"
}
function Set-EngineCvars {
    $lines = New-Object Collections.Generic.List[string]
    if ([IO.File]::Exists($engineIni)) {
        [IO.File]::SetAttributes($engineIni, [IO.File]::GetAttributes($engineIni) -band -bnot [IO.FileAttributes]::ReadOnly)
        foreach ($l in [IO.File]::ReadAllLines($engineIni)) { $lines.Add($l) }
    } else {
        [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($engineIni))
    }
    $out = New-Object Collections.Generic.List[string]
    $seen = @{}
    $inSys = $false
    $hasSys = $false
    foreach ($l in $lines) {
        $t = $l.Trim()
        if ($t.StartsWith("[") -and $t.EndsWith("]")) {
            if ($inSys) { foreach ($k in $cvars.Keys) { if (-not $seen[$k]) { $out.Add("$k=$($cvars[$k])"); $seen[$k] = $true } } }
            $inSys = ($t -ieq "[SystemSettings]")
            if ($inSys) { $hasSys = $true }
        } elseif ($inSys -and $t.Contains("=")) {
            $k = $t.Split("=", 2)[0].Trim()
            if ($cvars.Contains($k)) { $l = "$k=$($cvars[$k])"; $seen[$k] = $true }
        }
        $out.Add($l)
    }
    if ($inSys) { foreach ($k in $cvars.Keys) { if (-not $seen[$k]) { $out.Add("$k=$($cvars[$k])") } } }
    if (-not $hasSys) {
        $out.Add("")
        $out.Add("[SystemSettings]")
        foreach ($k in $cvars.Keys) { $out.Add("$k=$($cvars[$k])") }
    }
    [IO.File]::WriteAllLines($engineIni, $out)
    [IO.File]::SetAttributes($engineIni, [IO.File]::GetAttributes($engineIni) -bor [IO.FileAttributes]::ReadOnly)
}
function Clear-EngineReadOnly {
    if ([IO.File]::Exists($engineIni)) {
        [IO.File]::SetAttributes($engineIni, [IO.File]::GetAttributes($engineIni) -band -bnot [IO.FileAttributes]::ReadOnly)
    }
}

$tier = "hd"
foreach ($t in "uhd", "hd", "sd") {
    if ([IO.Directory]::Exists([IO.Path]::Combine($root, "Client", "Content", $t.ToUpper()))) { $tier = $t; break }
}

$problem = ""
try {
    Set-EngineCvars
    $start = New-Object Diagnostics.ProcessStartInfo
    $start.FileName = $exe
    $start.Arguments = "-krqlv=$tier -dx12 -SkipSplash"
    $start.WorkingDirectory = $root
    [void][Diagnostics.Process]::Start($start)
    $t0 = Get-Date
    while (-not (Test-GameRunning)) {
        if (((Get-Date) - $t0).TotalSeconds -gt 600) { $problem = "The game did not start within 10 minutes."; break }
        Start-Sleep -Seconds 1
    }
    if (-not $problem) {
        Write-Host "Game starting. This window closes by itself in a minute, once the game has read its settings."
        Start-Sleep -Seconds 60
    }
}
finally {
    Clear-EngineReadOnly
}
if ($problem) { Fail $problem }
