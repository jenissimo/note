# Compresses the curated definition packs for embedding in note.exe.
#
# The output goes into build\ as a build artifact, not into assets\, and is
# read by src\note.rc.  Each blob is a 4-byte little-endian uncompressed size
# followed by LZMS data: the decompressor has to be told how large the result
# will be, and the size is not recoverable from the stream.
#
# LZMS comes from Cabinet.dll, which Windows has shipped since 8.  Using it
# means note carries no decompression code of its own -- the whole cost on the
# reading side is one API call.
#
#   powershell -ExecutionPolicy Bypass -File tools\compress_packs.ps1 <outdir>

param([string]$OutDir = "build")

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class NoteCompress {
    const uint COMPRESS_ALGORITHM_LZMS = 5;

    [DllImport("Cabinet.dll", SetLastError = true)]
    static extern bool CreateCompressor(uint alg, IntPtr allocRoutines, out IntPtr handle);
    [DllImport("Cabinet.dll", SetLastError = true)]
    static extern bool Compress(IntPtr handle, byte[] src, IntPtr srcSize,
                                byte[] dst, IntPtr dstSize, out IntPtr used);
    [DllImport("Cabinet.dll", SetLastError = true)]
    static extern bool CloseCompressor(IntPtr handle);

    public static byte[] Lzms(byte[] data) {
        IntPtr h;
        if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, IntPtr.Zero, out h))
            throw new Exception("CreateCompressor failed: " + Marshal.GetLastWin32Error());
        try {
            IntPtr used;
            // First call with a null buffer asks how much room the result needs.
            Compress(h, data, (IntPtr)data.Length, null, IntPtr.Zero, out used);
            byte[] buf = new byte[(int)used];
            if (!Compress(h, data, (IntPtr)data.Length, buf, (IntPtr)buf.Length, out used))
                throw new Exception("Compress failed: " + Marshal.GetLastWin32Error());
            Array.Resize(ref buf, (int)used);
            return buf;
        } finally {
            CloseCompressor(h);
        }
    }
}
'@

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$pairs = @(
    @{ src = "assets\core.syntax.pack"; dst = "core.syntax.lz" },
    @{ src = "assets\core.themes.pack"; dst = "core.themes.lz" }
)

foreach ($p in $pairs) {
    $inPath = Join-Path $root $p.src
    if (-not (Test-Path $inPath)) {
        Write-Host ("  skipped {0}: not generated yet" -f $p.src)
        continue
    }

    $raw = [IO.File]::ReadAllBytes($inPath)
    $packed = [NoteCompress]::Lzms($raw)

    $out = New-Object byte[] (4 + $packed.Length)
    [BitConverter]::GetBytes([uint32]$raw.Length).CopyTo($out, 0)
    $packed.CopyTo($out, 4)

    $outPath = Join-Path $OutDir $p.dst
    [IO.File]::WriteAllBytes($outPath, $out)
    Write-Host ("  {0,-22} {1,7:N0} -> {2,6:N0} bytes" -f $p.dst, $raw.Length, $out.Length)
}
