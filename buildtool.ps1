# Recompile helper programs from vend\bin (i386 ELF) to vend\bin\<name>.exe, sharing the runtime objects.
# usage: powershell -ExecutionPolicy Bypass -File buildtool.ps1 tgascale voronoi ...
param([Parameter(ValueFromRemainingArguments)] [string[]] $Tools)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$env:PATH = "C:\MinGW\bin;$env:PATH"
$CF = @('-O1', '-fno-strict-aliasing', '-fwrapv', '-w', '-g0')
& "$PSScriptRoot\build.ps1" | Out-Null   # makes sure obj\rt_*.o are current
if (-not $Tools) { $Tools = 'tgafade', 'tgascale', 'tgaconv', 'tgaflip', 'tgapaste', 'tgawarp', 'voronoi', 'trilate', 'tgacreat' }
foreach ($t in $Tools) {
    $g = "tools\gen_$t"; $ob = "tools\obj_$t"
    New-Item -ItemType Directory -Force $g, $ob | Out-Null
    python recomp.py "vend\bin\$t" $g
    if ($LASTEXITCODE) { throw "recomp $t failed" }
    Get-ChildItem "$g\*.c", "$g\*.h" | ForEach-Object {
        (Get-Content $_.FullName -Raw).Replace('"../rt/cpu.h"', '"../../rt/cpu.h"') | Set-Content -NoNewline -Encoding ascii $_.FullName
    }
    $jobs = Get-ChildItem "$g\*.c" | ForEach-Object {
        Start-Process gcc -ArgumentList ($CF + @('-c', $_.FullName, '-o', "$ob\$($_.BaseName).o")) -NoNewWindow -PassThru
    }
    $jobs | ForEach-Object { $_.WaitForExit() }
    & gcc -o "vend\bin\$t.exe" (Get-ChildItem "$ob\*.o").FullName (Get-ChildItem obj\rt_*.o).FullName -lgdi32 -lwinmm -lole32
    if ($LASTEXITCODE) { throw "link $t failed" }
    "built vend\bin\$t.exe"
}
