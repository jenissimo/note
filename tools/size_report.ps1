# Where note.exe's bytes go.
#
# Written for the 64 KB target: a scoreboard is the difference between cutting
# what is actually large and cutting what merely feels large.  Every number
# here comes from the file itself or from the linker's map, never from an
# estimate.
#
#   powershell -ExecutionPolicy Bypass -File tools\size_report.ps1 [build\win32\note.exe]

param(
    [string]$Exe = "build\win32\note.exe",
    [int]$Budget = 65536
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($Exe)) { $Exe = Join-Path $root $Exe }
if (-not (Test-Path $Exe)) { Write-Host "no such file: $Exe"; exit 1 }

$bytes = [IO.File]::ReadAllBytes($Exe)
$pe    = [BitConverter]::ToInt32($bytes, 0x3C)
$nsec  = [BitConverter]::ToUInt16($bytes, $pe + 6)
$optSz = [BitConverter]::ToUInt16($bytes, $pe + 20)
$magic = [BitConverter]::ToUInt16($bytes, $pe + 24)
$secs  = $pe + 24 + $optSz

Write-Host ""
Write-Host ("{0}  ({1})" -f (Split-Path -Leaf $Exe),
                            $(if ($magic -eq 0x20B) { "x64" } else { "x86" }))
Write-Host ("total {0:N0} bytes" -f $bytes.Length)
Write-Host ""
Write-Host "section        in file    in memory   what it is"
Write-Host "-------------  ---------  ---------   ------------------------------"

$what = @{
    ".text"  = "code";
    ".rdata" = "constants, imports, string literals";
    ".data"  = "writable statics; zeroes cost nothing on disk";
    ".pdata" = "unwind tables (x64 only)";
    ".rsrc"  = "icon, version, embedded packs";
    ".reloc" = "relocations";
}

for ($i = 0; $i -lt $nsec; $i++) {
    $o     = $secs + $i * 40
    $name  = [Text.Encoding]::ASCII.GetString($bytes, $o, 8).Trim([char]0)
    $vsize = [BitConverter]::ToUInt32($bytes, $o + 8)
    $rsize = [BitConverter]::ToUInt32($bytes, $o + 16)
    Write-Host ("{0,-13}  {1,9:N0}  {2,9:N0}   {3}" -f $name, $rsize, $vsize, $what[$name])
}

# Per-object code size, from the linker's map.  Symbols are listed by address;
# the gap to the next symbol is that symbol's size, which attributes to the
# object it came from.
$map = [IO.Path]::ChangeExtension($Exe, ".map")
if (Test-Path $map) {
    $syms = @()
    foreach ($line in Get-Content $map) {
        if ($line -match '^\s+0001:[0-9A-Fa-f]{8}\s+\S+\s+([0-9A-Fa-f]+)\s+\S*\s*(\S+\.obj)\s*$') {
            $syms += [pscustomobject]@{
                Rva = [Convert]::ToUInt64($matches[1], 16)
                Obj = Split-Path -Leaf $matches[2]
            }
        }
    }

    if ($syms.Count -gt 1) {
        $syms = $syms | Sort-Object Rva
        $tot = @{}
        for ($i = 0; $i -lt $syms.Count - 1; $i++) {
            $size = [int]($syms[$i + 1].Rva - $syms[$i].Rva)
            # A negative or absurd gap means the next symbol is in another
            # section; skip rather than poison the total.
            if ($size -lt 0 -or $size -gt 200000) { continue }
            $tot[$syms[$i].Obj] = [int]$tot[$syms[$i].Obj] + $size
        }

        Write-Host ""
        Write-Host "code per module"
        Write-Host "-------------------------  ---------"
        foreach ($e in $tot.GetEnumerator() | Sort-Object Value -Descending) {
            Write-Host ("{0,-25}  {1,9:N0}" -f $e.Key, $e.Value)
        }
    }
}

# What ships beside it, and whether the whole thing fits the target.
$dir   = Split-Path -Parent $Exe
$packs = Get-ChildItem (Join-Path $dir "*.pack") -ErrorAction SilentlyContinue
$side  = ($packs | Measure-Object Length -Sum).Sum
if (-not $side) { $side = 0 }

Write-Host ""
Write-Host ("executable          {0,9:N0}" -f $bytes.Length)
if ($side) {
    Write-Host ("packs beside it     {0,9:N0}   (optional; the curated set is embedded)" -f $side)
}
Write-Host ("budget              {0,9:N0}" -f $Budget)
$over = $bytes.Length - $Budget
if ($over -le 0) {
    Write-Host ("                    {0,9:N0}   to spare" -f (-$over))
} else {
    Write-Host ("                    {0,9:N0}   over" -f $over)
}
Write-Host ""
