# Builds note several ways and reports what each configuration costs.
#
# The 64 KB target is only worth chasing with numbers in front of it: some of
# the obvious levers turn out to be worth a few hundred bytes and some of the
# unobvious ones a dozen kilobytes.  Each variant builds into its own directory
# through NOTE_OUT, so none of them disturbs build\ or races another build.
#
#   powershell -ExecutionPolicy Bypass -File tools\measure_variants.ps1

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$bat  = Join-Path $root "build.bat"

# name, architecture, extra compiler flags, extra linker flags
$variants = @(
    @{ n = "x64 baseline";            a = "x64"; c = "";    l = "" },
    @{ n = "x64 + LTCG";              a = "x64"; c = "/GL"; l = "/LTCG" },
    @{ n = "x64 + LTCG + filealign";  a = "x64"; c = "/GL"; l = "/LTCG /FILEALIGN:16" },
    @{ n = "x86 baseline";            a = "x86"; c = "";    l = "" },
    @{ n = "x86 + LTCG";              a = "x86"; c = "/GL"; l = "/LTCG" },
    @{ n = "x86 + LTCG + filealign";  a = "x86"; c = "/GL"; l = "/LTCG /FILEALIGN:16" }
)

function Sections($path) {
    $b   = [IO.File]::ReadAllBytes($path)
    $pe  = [BitConverter]::ToInt32($b, 0x3C)
    $n   = [BitConverter]::ToUInt16($b, $pe + 6)
    $opt = [BitConverter]::ToUInt16($b, $pe + 20)
    $s   = $pe + 24 + $opt
    $r   = @{}
    for ($i = 0; $i -lt $n; $i++) {
        $o = $s + $i * 40
        $r[[Text.Encoding]::ASCII.GetString($b, $o, 8).Trim([char]0)] =
            [BitConverter]::ToUInt32($b, $o + 16)
    }
    $r
}

Write-Host ""
Write-Host ("{0,-26} {1,9} {2,9} {3,9} {4,9}" -f "variant", "total", ".text", ".rdata", ".rsrc")
Write-Host ("{0,-26} {1,9} {2,9} {3,9} {4,9}" -f "--------------------------", "---------", "---------", "---------", "---------")

$baseline = 0
foreach ($v in $variants) {
    $out = Join-Path $env:TEMP ("note_var_" + ($v.n -replace '[^A-Za-z0-9]', '_'))
    New-Item -ItemType Directory -Force -Path $out | Out-Null

    $env:NOTE_OUT     = $out
    $env:NOTE_CFLAGS  = $v.c
    $env:NOTE_LDFLAGS = $v.l
    $log = cmd /c "`"$bat`" $($v.a)" 2>&1

    $exe = Join-Path $out "note.exe"
    if (-not (Test-Path $exe)) {
        $err = ($log | Select-String "error" | Select-Object -First 1)
        Write-Host ("{0,-26} failed   {1}" -f $v.n, $err)
        continue
    }

    $len = (Get-Item $exe).Length
    $s   = Sections $exe
    if (-not $baseline) { $baseline = $len }
    $delta = if ($len -eq $baseline) { "" } else { "{0,7:N0}" -f ($len - $baseline) }

    Write-Host ("{0,-26} {1,9:N0} {2,9:N0} {3,9:N0} {4,9:N0}  {5}" -f `
        $v.n, $len, [int]$s['.text'], [int]$s['.rdata'], [int]$s['.rsrc'], $delta)
}

Remove-Item Env:NOTE_OUT, Env:NOTE_CFLAGS, Env:NOTE_LDFLAGS -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "budget 65 536 bytes"
Write-Host ""
