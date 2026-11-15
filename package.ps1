# Package the Windows build into release\SketchBook-win32.7z:
#   SketchBook\v8mnu22b.exe, SketchBook\README.txt (from dist\), SketchBook\vend\ (game data, junctions resolved).
# Left out of vend: the Linux system leftovers, shell dotfiles, runtime logs/state, and the original i386 programs in bin\
# (only the recompiled *.exe tools and tgatools.ini are kept).
# usage: powershell -ExecutionPolicy Bypass -File package.ps1   (after build.ps1 and buildtool.ps1)
param([string] $Out = (Join-Path $PSScriptRoot 'release\SketchBook-win32.7z'))
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$Out = [IO.Path]::GetFullPath($Out)
foreach ($need in 'v8mnu22b.exe', 'vend', 'dist\README.txt') { if (-not (Test-Path $need)) { throw "missing $need" } }

$stage = Join-Path $env:TEMP "sketchbook-pkg-$PID"
$root = Join-Path $stage 'SketchBook'
$v = Join-Path $root 'vend'
$skipDirs = 'bin', 'tmp', 'lib', 'menu', 'share', 'sys', '.mc', '.mcop'
$skipFiles = '.bashrc', '.bash_logout', '.bash_profile', '.cshrc', '.cvspass', '.cvstag', '.esd_auth', '.gdbinit', '.history',
    '.lesshst', '.login', '.taglog', '.viminfo', 'devlog.txt', 'dongle.img', 'dteff.dat', 'errlog.txt', 'infohut.log',
    'mister.bak', 'mister.dat', 'nov.bak', 'nov.dat', 'picwhen.dat', 'syslog.txt', 'temp.log', 'timeout.dat', 'timeout.sav',
    'usage.log', 'vend.pid'
try {
    # robocopy follows the junctions (lang, studio, workpics, pics\product) and copies their contents
    $xd = $skipDirs | ForEach-Object { Join-Path (Resolve-Path vend) $_ }
    # stats.dat: play statistics the game writes; mkdict: an original i386 program in theme\support
    robocopy vend $v /E /NFL /NDL /NJH /NJS /NP /XD @xd /XF stats.dat mkdict | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    $skipFiles | ForEach-Object { Remove-Item -Force -LiteralPath (Join-Path $v $_) -ErrorAction SilentlyContinue }
    New-Item -ItemType Directory -Force (Join-Path $v 'bin'), (Join-Path $v 'tmp') | Out-Null
    Copy-Item vend\bin\*.exe, vend\bin\tgatools.ini (Join-Path $v 'bin')
    Copy-Item v8mnu22b.exe, dist\README.txt $root

    New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
    Remove-Item -Force $Out -ErrorAction SilentlyContinue
    Push-Location $stage
    & 7z a -t7z -mx=9 -bso0 -bsp0 $Out SketchBook
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc) { throw "7z failed ($rc)" }
    "packaged $Out ({0:N1} MB)" -f ((Get-Item $Out).Length / 1MB)
} finally {
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
}
