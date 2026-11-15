# Replace the WSL (LX) symlinks in vend\ with native Windows equivalents: directory junctions for dirs, copies for files.
# Win32 can't open LX symlinks, so without this the game can't see studio\, lang\, workpics\, the theme TGAs, etc.
# usage: powershell -ExecutionPolicy Bypass -File fixlinks.ps1
$ErrorActionPreference = 'Stop'
$V = Join-Path $PSScriptRoot 'vend'
# link -> target, both relative to vend\ (order matters: pics\product first, others resolve through it)
$links = [ordered]@{
    'pics\product'        = 'pics\8024v1'
    'lang'                = 'pics\8024v1\lang'
    'studio'              = 'pics\8024v1\studio'
    'workpics'            = 'pics\8024v1\workpics'
    'pattr.ini'           = 'pics\8024v1\theme\pattr.ini'
    'ppaper.tga'          = 'pics\8024v1\theme\ppaper.tga'
    'wframe.tga'          = 'pics\8024v1\theme\wframe.tga'
    'wpaper.tga'          = 'pics\8024v1\theme\wpaper.tga'
    'pics\cat\8024v1.ini' = 'pics\8024v1\8024v1.ini'
    'hcs.dat'             = 'hcs\tvtuner.pos'
    'hcs.sav'             = 'hcs\tvtuner.pos'
    'hcs2.dat'            = 'hcs\tvtuner.scn'
    'hcs\hcs2.sav'        = 'hcs\tvtuner.scn'
}
foreach ($k in $links.Keys) {
    $l = Join-Path $V $k; $t = Join-Path $V $links[$k]
    if (-not (Test-Path -LiteralPath $t)) { "skip $k (no $($links[$k]))"; continue }
    $i = Get-Item -Force -LiteralPath $l -ErrorAction SilentlyContinue
    if ($i -and -not ($i.Attributes -band [IO.FileAttributes]::ReparsePoint)) { "ok   $k"; continue }
    if ($i -and $i.LinkType -eq 'Junction') { "ok   $k"; continue }
    if ($i) { cmd /c del /f /q "`"$l`"" 2>$null; if (Test-Path -LiteralPath $l) { cmd /c rmdir "`"$l`"" } }
    if ((Get-Item -LiteralPath $t).PSIsContainer) { New-Item -ItemType Junction -Path $l -Target $t | Out-Null; "junc $k -> $($links[$k])" }
    else { Copy-Item -LiteralPath $t -Destination $l; "copy $k <- $($links[$k])" }
}
