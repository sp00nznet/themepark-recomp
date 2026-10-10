# One-click setup for the Theme Park static recompilation.
# Run it by double-clicking Setup.cmd in the repo folder; the README's "Step by
# step" is the same thing by hand, command for command.
#
# It checks the tools, copies your copy of the game into game\ (from a GOG
# install, or by unpacking GOG's offline installer), lifts MAIN.EXE to C, builds
# themepark.exe and leaves a shortcut. Each step is skipped when its output
# already exists (-Force redoes them). Everything it does goes to setup.log.
param([switch]$Force, [string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP } else { Join-Path (Split-Path -Parent $Root) 'tools' }
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Steps = 5
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
# cmd.exe, which the build runs through, splits a path at '&'.
if ($Root -match '&') {
  Say "This folder's path has an '&' in it, which the build cannot handle:" 'Red'
  Say "  $Root" 'Red'
  Say "Move the folder somewhere else (for example C:\themepark) and run Setup.cmd from there." 'Yellow'
  Read-Host "Press Enter to close" | Out-Null; exit 1
}
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Read-Host "Press Enter to close" | Out-Null; exit 1
}
function Ask($q) { $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Winget-Install($cmd, $id, $size) {
  if (Get-Command $cmd -ErrorAction SilentlyContinue) { return }
  Say "  $cmd is not installed."
  if (-not (Ask "  Install it now (winget, about $size)?")) { Fail "$cmd is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install $cmd yourself." }
  Exec @('winget', 'install', '-e', '--id', $id, '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) { Fail "$cmd installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}

Clear-Host
Say "Theme Park - static recompilation setup" 'White'
Say "You need your own copy of Theme Park from GOG (installed, or GOG's offline installer) and about 2 GB free."
Say "The lift and the build take 10 to 30 minutes, once."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools the pipeline needs"
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  foreach ($c in @(@('py', '-3'), @('python'), @('python3'))) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but Windows has not picked it up yet: close this window and run Setup.cmd again." }
}
Say "  Python: $($pyargs -join ' ')"
if ((Exec ($pyargs + @('-c', 'import pefile, capstone'))) -ne 0) {
  Say "  The Python packages pefile and capstone are missing (about 15 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile and capstone are required." }
  Run "Installing pefile and capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone'))
}

if (-not (Test-Path (Join-Path $Toolkit 'tools\lift\lift32.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "git is needed to fetch pcrecomp. Install Git for Windows, then run Setup.cmd again." }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
if (-not (Select-String -Quiet -Path (Join-Path $Toolkit 'runtime\dos32\dos32.h') -Pattern 'overlay' -ErrorAction SilentlyContinue)) {
  Fail "your pcrecomp at $Toolkit predates the DOS host (runtime\dos32). Update it: git -C `"$Toolkit`" pull"
}
Say "  pcrecomp: $Toolkit"

# Visual Studio is several GB with an installer of its own, so this says what
# to install rather than installing it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
if (-not $vs) { Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again." }
Say "  Visual Studio: $vs"
if (-not (Test-Path "$env:ProgramFiles\LLVM\bin\clang-cl.exe") -and -not (Test-Path "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe")) {
  Winget-Install 'clang-cl' 'LLVM.LLVM' '350 MB'
}
Winget-Install 'cmake' 'Kitware.CMake' '30 MB'
Winget-Install 'ninja' 'Ninja-build.Ninja' '1 MB'

# ---------------------------------------------------------------- the game
Step 2 "Copying your copy of Theme Park into game\"
function Is-Install([string]$d) { return ($d -and (Test-Path (Join-Path $d 'GAME\MAIN.EXE')) -and (Test-Path (Join-Path $d 'THEME.CD\DATA'))) }
function Find-GOG {
  foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games\1207660423', 'HKLM:\SOFTWARE\GOG.com\Games\1207660423') {
    try { $d = (Get-ItemProperty $k -ErrorAction Stop).path; if (Is-Install $d) { return $d.TrimEnd('\') } } catch {}
  }
  return ""
}
if ((Is-Install 'game') -and -not $Force) {
  Say "  Already in game\ (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not $Game) { $Game = Find-GOG; if ($Game) { Say "  Found your GOG install: $Game" } }
  if (-not $Game) {
    $inst = Get-ChildItem -Path $Root, (Join-Path $env:USERPROFILE 'Downloads') -Filter 'setup_theme_park_*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inst) { $Game = $inst.FullName; Say "  Found GOG's installer: $Game" }
  }
  while (-not ((Is-Install $Game) -or ($Game -match '\.exe$' -and (Test-Path $Game)))) {
    $Game = (Read-Host "  Paste the Theme Park folder (with GAME\MAIN.EXE), or the path of GOG's setup_theme_park_*.exe").Trim('"', ' ')
  }
  if ($Game -match '\.exe$') {
    Winget-Install 'innoextract' 'dscharrer.innoextract' '5 MB'
    Run "Unpacking GOG's installer (about 200 MB)" @('innoextract', '-s', '-d', (Join-Path $Root 'game'), $Game)
  } else {
    Say "  Copying $Game (about 180 MB)..."
    $code = Exec @('robocopy', $Game, (Join-Path $Root 'game'), '/E', '/NFL', '/NDL', '/NJH', '/NP', '/XD', $Root)
    if ($code -ge 8) { Fail "copying the game failed (robocopy exit code $code)." }
  }
  if (-not (Is-Install 'game')) { Fail "game\ has no GAME\MAIN.EXE after copying. Is that the GOG version of Theme Park?" }
  # GOG keeps the sound setup beside its DOSBox config, not in THEME.CD.
  $snd = Get-ChildItem -Path 'game' -Recurse -Filter 'SNDSETUP.INF' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($snd -and -not (Test-Path 'game\THEME.CD\SNDSETUP.INF')) { Copy-Item $snd.FullName 'game\THEME.CD\SNDSETUP.INF' }
}

# ---------------------------------------------------------------- lift
Step 3 "Lifting MAIN.EXE to C (finding the functions takes a few minutes, once)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else {
  if ($Force) { Remove-Item -Force -ErrorAction SilentlyContinue 'analysis\d32_funcs.json' }
  $env:PCRECOMP = $Toolkit
  Run "Lifting" ($pyargs + @('run_lift.py'))
}

# ---------------------------------------------------------------- build
Step 4 "Building build\themepark.exe (5 to 20 minutes)"
if ((Test-Path 'build\themepark.exe') -and -not $Force) { Say "  Already done (skipping)." }
else {
  $env:PCRECOMP = $Toolkit
  Run "Compiling" @('cmd', '/c', ('call "' + (Join-Path $Root 'build.cmd') + '"'))
}

# ---------------------------------------------------------------- shortcut
Step 5 "Making the shortcut"
$lnk = Join-Path $Root 'Theme Park (recomp).cmd'
Set-Content -Path $lnk -Encoding ASCII -Value @(
  '@echo off',
  'rem Plays the recompiled Theme Park in its own window. F11 or Alt+Enter: fullscreen.',
  'cd /d "%~dp0"',
  'start "" build\themepark.exe')
Say "  $lnk"
Write-Host ""
Say "Done. Double-click 'Theme Park (recomp).cmd' to play." 'Green'
Read-Host "Press Enter to close" | Out-Null
