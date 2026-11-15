# Build v8mnu22b.exe (recompiled guest code gen\ + Win32 runtime rt\) with MinGW gcc.
# usage: powershell -ExecutionPolicy Bypass -File build.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
$env:PATH = "C:\MinGW\bin;$env:PATH"
$CF = @('-O1', '-fno-strict-aliasing', '-fwrapv', '-w', '-g0')
New-Item -ItemType Directory -Force obj | Out-Null
$hdrTime = @((Get-Item rt\cpu.h).LastWriteTime, (Get-Item rt\rt.h).LastWriteTime) | Sort-Object | Select-Object -Last 1
$srcs = @(Get-ChildItem gen\*.c) + @(Get-ChildItem rt\*.c)
$jobs = @()
foreach ($s in $srcs) {
    $o = "obj\" + ($s.Directory.Name + '_' + $s.BaseName) + '.o'
    if ((Test-Path $o) -and (Get-Item $o).LastWriteTime -gt $s.LastWriteTime -and (Get-Item $o).LastWriteTime -gt $hdrTime) { continue }
    $p = Start-Process gcc -ArgumentList ($CF + @('-c', $s.FullName, '-o', $o)) -NoNewWindow -PassThru
    $null = $p.Handle   # keeps ExitCode readable
    $jobs += $p
    while (@($jobs | Where-Object { -not $_.HasExited }).Count -ge 12) { Start-Sleep -Milliseconds 100 }
}
$jobs | ForEach-Object { $_.WaitForExit() }
if (@($jobs | Where-Object { $_.ExitCode -ne 0 }).Count) { throw "compile failed" }
& gcc -o v8mnu22b.exe (Get-ChildItem obj\*.o).FullName -lgdi32 -lwinmm -lole32
if ($LASTEXITCODE) { throw "link failed" }
"built v8mnu22b.exe"

