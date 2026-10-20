# One-click setup for the Theme Park static recompilation.
# Run it by double-clicking Setup.cmd in the repo folder; the README's "Step by
# step" is the same thing by hand, command for command.
#
# It checks the tools, copies your copy of the game into game\ (from a GOG
# install, GOG's offline installer, or the 1994 CD), lifts MAIN.EXE to C, builds
# themepark.exe and leaves a shortcut. Each step is skipped when its output
# already exists (-Force redoes them). Everything it does goes to setup.log.
# -Yes installs what is missing without asking; -Game (or -GameDir) is the
# game's folder, the CD's drive, GOG's offline installer or the folder it is
# in. RECOMP_LAUNCHER=1 (the recomp launcher) skips the "Press Enter".
param([switch]$Force, [switch]$Yes, [Alias('GameDir')][string]$Game = "")

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
# The toolkit: PCRECOMP, else ../pcrecomp (where the launcher puts it), else
# ../tools (older setups), else ../pcrecomp is cloned.
$Parent = Split-Path -Parent $Root
$Toolkit = if ($env:PCRECOMP) { $env:PCRECOMP }
  elseif (-not (Test-Path (Join-Path $Parent 'pcrecomp')) -and (Test-Path (Join-Path $Parent 'tools\runtime\dos32'))) { Join-Path $Parent 'tools' }
  else { Join-Path $Parent 'pcrecomp' }
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
$Steps = 5
function Log($t) { try { Add-Content -Path $Log -Value $t -Encoding UTF8 -ErrorAction Stop } catch {} }
Log "==== setup $(Get-Date -Format s)"

function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Pause-End { if (-not $env:RECOMP_LAUNCHER) { Read-Host "Press Enter to close" | Out-Null } }
# cmd.exe, which the build runs through, splits a path at '&'.
if ($Root -match '&') {
  Say "This folder's path has an '&' in it, which the build cannot handle:" 'Red'
  Say "  $Root" 'Red'
  Say "Move the folder somewhere else (for example C:\themepark) and run Setup.cmd from there." 'Yellow'
  Pause-End; exit 1
}
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Fail($t) {
  Say "" ; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }   # Enter = yes
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
Say "You need your own copy of Theme Park (GOG's, installed or its offline installer, or the 1994 CD) and about 2 GB free."
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
if (-not (Select-String -Quiet -Path (Join-Path $Toolkit 'runtime\dos32\dos32.h') -Pattern 'dos32_state_save' -ErrorAction SilentlyContinue)) {
  Fail "your pcrecomp at $Toolkit is older than this game needs. Update it: git -C `"$Toolkit`" pull"
}
Say "  pcrecomp: $Toolkit"

# Visual Studio is several GB with an installer of its own, so this says what
# to install rather than installing it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
if (-not $vs) {
  Say "  Visual Studio 2022 with the C++ tools is not installed."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB, asks for admin)?")) {
    Fail "Visual Studio 2022 (or its Build Tools) with 'Desktop development with C++' is needed. Install it, then run Setup.cmd again."
  }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or the Visual Studio Build Tools yourself." }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  $vs = if (Test-Path $vswhere) { (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath) } else { $null }
  if (-not $vs) { Fail "the Visual Studio Build Tools did not install. Install them with 'Desktop development with C++', then run Setup.cmd again." }
}
Say "  Visual Studio: $vs"
if (-not (Test-Path "$env:ProgramFiles\LLVM\bin\clang-cl.exe") -and -not (Test-Path "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe")) {
  Winget-Install 'clang-cl' 'LLVM.LLVM' '350 MB'
}
Winget-Install 'cmake' 'Kitware.CMake' '30 MB'
Winget-Install 'ninja' 'Ninja-build.Ninja' '1 MB'

# ---------------------------------------------------------------- the game
Step 2 "Copying your copy of Theme Park into game\"
# GOG's install, a 1994 install, or the 1994 CD itself (GAME\ and CONFIG\, no THEME.CD\).
function Is-Install([string]$d) { return ($d -and (Test-Path (Join-Path $d 'GAME\MAIN.EXE')) -and (Test-Path (Join-Path $d 'GAME\DATA'))) }
# The files the 1994 installer copies from the CD into C:\THEME.CD\DATA; the
# game reads these from there and nowhere else. GOG's THEME.CD\DATA is the same 32.
$HdFiles = 'HPANEL-0.DAT HPANEL-0.TAB MAWAR0-0.DAT MAWAR1-0.DAT MAWPAL-0.DAT MBLK-0.DAT MBLK-0.TAB MCUP-0.DAT MCUP-0.TAB MHAND-0.DAT MHAND-0.TAB MMAP-0.DAT MMENU-0.DAT MMENU-1.DAT MNEG-0.DAT MNGPAL-0.DAT MPALETTE.DAT MPANEL-0.DAT MPANEL-0.TAB MRES-0.DAT MRSPAL-0.DAT MRSSPR-0.DAT MRSSPR-0.TAB MSHARE-0.DAT MSPR-0.DAT MSPR-0.TAB MSTAP-0.DAT MSTATE-0.DAT MSTOCK-0.DAT MSTPAL-0.DAT MSTSPR-0.DAT MSTSPR-0.TAB' -split ' '
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
  # A folder holding GOG's offline installer (the launcher's "Choose folder" on Downloads, say).
  if ($Game -and -not (Is-Install $Game) -and (Test-Path $Game -PathType Container)) {
    $inst = Get-ChildItem -Path $Game -Filter 'setup_theme_park_*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inst) { $Game = $inst.FullName }
  }
  if (-not $Game) { $Game = Find-GOG; if ($Game) { Say "  Found your GOG install: $Game" } }
  if (-not $Game) {
    $inst = Get-ChildItem -Path $Root, (Join-Path $env:USERPROFILE 'Downloads') -Filter 'setup_theme_park_*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($inst) { $Game = $inst.FullName; Say "  Found GOG's installer: $Game" }
  }
  if ($Yes -and -not ((Is-Install $Game) -or ($Game -match '\.exe$' -and (Test-Path $Game)))) {
    Fail "Theme Park was not found. Give its folder (with GAME\MAIN.EXE), the CD's drive, or GOG's setup_theme_park_*.exe with -Game."
  }
  while (-not ((Is-Install $Game) -or ($Game -match '\.exe$' -and (Test-Path $Game)))) {
    $Game = (Read-Host "  Paste the Theme Park folder (with GAME\MAIN.EXE), the CD's drive (for example D:\; double-click an ISO to mount it), or the path of GOG's setup_theme_park_*.exe").Trim('"', ' ')
  }
  if ($Game -match '\.exe$') {
    Winget-Install 'innoextract' 'dscharrer.innoextract' '5 MB'
    Run "Unpacking GOG's installer (about 200 MB)" @('innoextract', '-s', '-d', (Join-Path $Root 'game'), $Game)
  } else {
    Say "  Copying $Game (about 180 MB)..."
    $code = Exec @('robocopy', $Game, (Join-Path $Root 'game'), '/E', '/NFL', '/NDL', '/NJH', '/NP', '/XD', $Root)
    if ($code -ge 8) { Fail "copying the game failed (robocopy exit code $code)." }
  }
  if (-not (Is-Install 'game')) { Fail "game\ has no GAME\MAIN.EXE after copying. Is that Theme Park (GOG, or the 1994 CD)?" }
  Exec @('attrib', '-R', (Join-Path $Root 'game\*'), '/S', '/D') | Out-Null   # files copied off a CD are read-only
  # From the CD: do what its installer did, minus the copy of GAME\.
  if (-not (Test-Path 'game\THEME.CD\DATA')) {
    Say "  That is the 1994 CD: setting up THEME.CD\ the way its installer did."
    New-Item -ItemType Directory -Force 'game\THEME.CD\DATA' | Out-Null
    foreach ($f in $HdFiles) { Copy-Item (Join-Path 'game\GAME\DATA' $f) 'game\THEME.CD\DATA\' }
    Copy-Item -Recurse 'game\GAME\SAVE' 'game\THEME.CD\SAVE'
  }
  # GOG keeps the sound setup beside its DOSBox config, not in THEME.CD.
  $snd = Get-ChildItem -Path 'game' -Recurse -Filter 'SNDSETUP.INF' -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($snd -and -not (Test-Path 'game\THEME.CD\SNDSETUP.INF')) { Copy-Item $snd.FullName 'game\THEME.CD\SNDSETUP.INF' }
  # The CD has none (its setup program writes it): GOG's settings, which the game's sound start-up needs.
  if (-not (Test-Path 'game\THEME.CD\SNDSETUP.INF')) {
    Set-Content -Path 'game\THEME.CD\SNDSETUP.INF' -Encoding ASCII -Value @('SOUNDFX = SB16 220 7 1', 'MUSIC = GENERAL 330 0 0')
  }
}

# ---------------------------------------------------------------- lift
Step 3 "Lifting MAIN.EXE and INTRO.EXE to C (finding the functions takes a few minutes, once)"
if ((Test-Path 'src\recomp\gen\recomp_dispatch.c') -and -not $Force) { Say "  Already done (skipping)." }
else {
  if ($Force) { Remove-Item -Force -ErrorAction SilentlyContinue 'analysis\d32_funcs.json' }
  $env:PCRECOMP = $Toolkit
  Run "Lifting" ($pyargs + @('run_lift.py'))
}
# The opening film: a second, smaller lift.
if ((Test-Path 'src\recomp\gen_intro\recomp_dispatch.c') -and -not $Force) { Say "  The intro: already done (skipping)." }
else {
  if ($Force) { Remove-Item -Force -ErrorAction SilentlyContinue 'analysis\intro_funcs.json' }
  $env:PCRECOMP = $Toolkit
  Run "Lifting the intro" ($pyargs + @('run_lift.py', '--intro'))
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
Pause-End
