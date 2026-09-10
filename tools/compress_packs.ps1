# Compresses the curated definition packs for embedding in note.exe.
#
# The output goes into build\ as a build artifact, not into assets\, and is
# read by src\note.rc.  Each blob is the eight-byte header src\core\note_pack.h
# describes -- 'NPK1' and the uncompressed length -- followed by LZSS data.
#
# This used to call LZMS in Cabinet.dll, which cost note no decompression code
# at all and cost it every machine older than Windows 8, where the API simply
# is not there.  Since no languages are compiled in any more, that was an
# editor with no highlighting whatsoever on Windows 95, carrying 19 KB of
# definitions it could not read.  So the format is note's own now; the
# decompressor is src\core\note_pack.c and the encoder is below.
#
# LZMS got these to 16,020 bytes and this gets them to about 20,300.  The
# missing 4 KB is the price of a format a 16-bit real-mode program can decode
# in a 4 KB window, and it is the right way round: a smaller pack no target
# can open is worth less than a larger one they all can.
#
#   powershell -ExecutionPolicy Bypass -File tools\compress_packs.ps1 <outdir>

# -Themes compresses a theme pack instead of the curated syntax pack, as
# themes.lz.  Every target that has themes inside its executable at all gets
# them this way: the blob is appended to the file and found again by scanning
# back from the end for the NPK1 magic, which is what note_pack.h put the
# magic there for.  build-retro.bat appends it to note-dos.exe; build.bat
# appends it to note.exe, where the Win32 half and the 16-bit MS-DOS half in
# the MZ stub read the same physical copy.
#
# -Catalogue picks the full 338-theme assets\themes.pack; without it -Themes
# takes the curated fourteen in assets\core.themes.pack, and that default is
# the one that matters.  note.exe is measured against 128 KB and the two
# packs are not close: the catalogue is 27,011 bytes compressed against the
# curated set's 1,700-odd, and the whole overage of a dual binary that spans
# MS-DOS to Windows 11 is that one substitution.  The curated bundle is the
# design the budget goes with -- everything you need in the file, nothing
# fetched at startup.  note-dos.exe is a standalone DJGPP build with no
# budget on it, so that is where -Catalogue is used.
param([string]$OutDir = "build", [switch]$Themes, [switch]$Catalogue)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;

// LZSS, 4 KB window, byte-aligned.  The format is documented once, in
// src/core/note_pack.h; this only has to agree with it.
public static class NotePack {
    const int WIN       = 4096;
    const int MINMATCH  = 3;
    const int MAXINLINE = 17;    // longest length a 4-bit code can carry
    const int MAXMATCH  = 273;   // longest with the escape code and a byte
    const int DEPTH     = 96;    // chain candidates examined per position

    static int Hash(byte[] d, int i) {
        return ((d[i] << 10) ^ (d[i + 1] << 5) ^ d[i + 2]) & 0xFFFF;
    }

    // The longest match for `pos` among the positions already chained.
    static void Find(byte[] d, int n, int pos, int[] head, int[] prev,
                     out int bestLen, out int bestDist) {
        bestLen = 0; bestDist = 0;
        if (pos + MINMATCH > n) return;

        int limit = pos - WIN; if (limit < 0) limit = -1;
        int max = n - pos; if (max > MAXMATCH) max = MAXMATCH;
        int j = head[Hash(d, pos)];

        for (int seen = 0; j > limit && seen < DEPTH; seen++) {
            if (d[j + bestLen] == d[pos + bestLen]) {
                int l = 0;
                while (l < max && d[j + l] == d[pos + l]) l++;
                if (l > bestLen) {
                    bestLen = l; bestDist = pos - j;
                    if (bestLen >= max) break;
                }
            }
            j = prev[j];
        }
    }

    public static byte[] Compress(byte[] d) {
        int n = d.Length;
        int[] head = new int[1 << 16];
        int[] prev = new int[n > 0 ? n : 1];
        for (int i = 0; i < head.Length; i++) head[i] = -1;

        List<byte> outp = new List<byte>();
        List<byte> items = new List<byte>();
        int flags = 0, nflags = 0;
        int chained = 0;   // positions [0, chained) are in the hash chains

        int pos = 0;
        while (pos < n) {
            // Chain every position before this one -- and not this one, which
            // would otherwise match itself at a distance of zero.
            while (chained < pos && chained + MINMATCH <= n) {
                int h = Hash(d, chained);
                prev[chained] = head[h];
                head[h] = chained;
                chained++;
            }

            int len, dist;
            Find(d, n, pos, head, prev, out len, out dist);

            // Lazy matching: if the next byte starts a longer match, this one
            // is better spent as a literal.  Worth about 1% and costs the
            // decoder nothing.
            if (len >= MINMATCH && pos + 1 < n) {
                while (chained < pos + 1 && chained + MINMATCH <= n) {
                    int h = Hash(d, chained);
                    prev[chained] = head[h];
                    head[h] = chained;
                    chained++;
                }
                int len2, dist2;
                Find(d, n, pos + 1, head, prev, out len2, out dist2);
                if (len2 > len) len = 0;
            }

            if (len >= MINMATCH) {
                int e = dist - 1;
                if (len <= MAXINLINE) {
                    items.Add((byte)(e & 0xFF));
                    items.Add((byte)(((e >> 8) << 4) | (len - MINMATCH)));
                } else {
                    items.Add((byte)(e & 0xFF));
                    items.Add((byte)(((e >> 8) << 4) | 15));
                    items.Add((byte)(len - 18));
                }
                pos += len;
            } else {
                flags |= 1 << nflags;          // a 1 bit means a literal
                items.Add(d[pos]);
                pos++;
            }

            if (++nflags == 8) {
                outp.Add((byte)flags); outp.AddRange(items);
                flags = 0; nflags = 0; items.Clear();
            }
        }
        if (nflags > 0) { outp.Add((byte)flags); outp.AddRange(items); }

        byte[] blob = new byte[8 + outp.Count];
        blob[0] = (byte)'N'; blob[1] = (byte)'P';
        blob[2] = (byte)'K'; blob[3] = (byte)'1';
        blob[4] = (byte)(n         & 0xFF);
        blob[5] = (byte)((n >>  8) & 0xFF);
        blob[6] = (byte)((n >> 16) & 0xFF);
        blob[7] = (byte)((n >> 24) & 0xFF);
        outp.CopyTo(blob, 8);
        return blob;
    }

    // Decodes a blob the way note_pack.c does, so the build can prove the two
    // agree rather than discover on Windows 95 that they do not.
    public static byte[] Expand(byte[] b) {
        int outlen = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
        byte[] o = new byte[outlen];
        byte[] w = new byte[WIN];
        int sp = 8, op = 0, wp = 0, flags = 0, nf = 0;

        while (op < outlen) {
            if (nf == 0) { flags = b[sp++]; nf = 8; }
            bool lit = (flags & 1) != 0;
            flags >>= 1; nf--;

            if (lit) {
                byte v = b[sp++];
                o[op++] = v; w[wp] = v; wp = (wp + 1) & (WIN - 1);
            } else {
                int b0 = b[sp++], b1 = b[sp++];
                int dist = 1 + (b0 | ((b1 >> 4) << 8));
                int code = b1 & 0x0F;
                int len  = (code == 15) ? 18 + b[sp++] : code + MINMATCH;
                int rp   = (wp - dist) & (WIN - 1);
                for (int k = 0; k < len; k++) {
                    byte v = w[rp]; rp = (rp + 1) & (WIN - 1);
                    o[op++] = v; w[wp] = v; wp = (wp + 1) & (WIN - 1);
                }
            }
        }
        return o;
    }
}
'@

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$pairs = if ($Themes) {
    $src = if ($Catalogue) { "assets\themes.pack" } else { "assets\core.themes.pack" }
    @( @{ src = $src; dst = "themes.lz" } )
} else {
    # Syntax only.  The curated theme pack used to be here beside it and is
    # not any more -- not because it left the executable, but because it
    # moved out of .rsrc: it is appended to the end of the file now, where
    # the 16-bit MS-DOS editor in note.exe's MZ stub can read the same bytes,
    # and a copy in .rsrc as well would be the same themes twice in a file
    # measured against 128 KB.
    @( @{ src = "assets\core.syntax.pack"; dst = "core.syntax.lz" } )
}

foreach ($p in $pairs) {
    $inPath = Join-Path $root $p.src
    if (-not (Test-Path $inPath)) {
        Write-Host ("  skipped {0}: not generated yet" -f $p.src)
        continue
    }

    # The embedded copy is machine-read only: note_conf_next skips comments and
    # blank lines, and nothing ever shows this blob to anyone.  The pack that
    # ships beside the executable keeps its comments -- that one people read
    # and copy definitions out of.  Dropping them here, and the CR of every
    # CRLF with them, takes about 1.1 KB off the compressed result, which at
    # this size is worth more than symmetry between the two copies.
    $text = [IO.File]::ReadAllText($inPath)
    $lines = $text -split "`r?`n" | Where-Object {
        $t = $_.Trim(); $t -ne "" -and -not $t.StartsWith("#")
    }
    $joined = ($lines -join "`n") + "`n"

    # Five of the 338 theme names carry an accented letter -- Rosé Pine,
    # Pastelón de Amarillos and their variants -- and the check below would
    # refuse the whole pack over them.  The rule is real: note_pack widens a
    # byte to an nchar with a cast, so a byte over 0x7F means whatever the
    # reader's character set says it means, and a C64, a DOS code page and
    # UTF-16 do not agree.  Folding the accent off is the smallest loss
    # available -- it is still that theme's name to anyone looking for it, and
    # a fuzzy filter never matched the accent anyway.  Decomposing and dropping
    # the combining marks does it without a table of special cases.
    $joined = -join ($joined.Normalize([Text.NormalizationForm]::FormD).ToCharArray() |
        Where-Object {
            [Globalization.CharUnicodeInfo]::GetUnicodeCategory($_) -ne
                [Globalization.UnicodeCategory]::NonSpacingMark
        })

    $raw = [Text.Encoding]::UTF8.GetBytes($joined)

    # note_pack widens a byte to an nchar with a cast, which is only right
    # because of this check -- it is what lets the MS-DOS half read a pack
    # without carrying a UTF-8 decoder.  The comment lines that do hold
    # non-ASCII are the ones stripped above, so this has never yet fired; if a
    # definition ever brings some in, the build should stop rather than embed
    # a blob whose readers disagree about what it says.
    foreach ($b in $raw) {
        if ($b -gt 0x7F) {
            throw ("{0} holds a non-ASCII byte (0x{1:X2}) outside its comments; the embedded pack must be ASCII." -f $p.src, $b)
        }
    }

    $blob = [NotePack]::Compress($raw)

    # Prove the round trip before it is embedded.  A decompressor that is
    # wrong about its own format fails on the machine that has no other way to
    # read the file, which is the machine least able to report it.
    $back = [NotePack]::Expand($blob)
    if ($back.Length -ne $raw.Length) {
        throw ("{0}: round trip produced {1:N0} bytes, not {2:N0}." -f $p.dst, $back.Length, $raw.Length)
    }
    for ($i = 0; $i -lt $raw.Length; $i++) {
        if ($back[$i] -ne $raw[$i]) {
            throw ("{0}: round trip differs at byte {1:N0}." -f $p.dst, $i)
        }
    }

    $outPath = Join-Path $OutDir $p.dst
    [IO.File]::WriteAllBytes($outPath, $blob)
    Write-Host ("  {0,-22} {1,7:N0} -> {2,6:N0} bytes  ({3:N2}x)" -f `
        $p.dst, $raw.Length, $blob.Length, ($raw.Length / $blob.Length))
}
