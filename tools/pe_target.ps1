<#
    pe_target.ps1 — says which Windows the executable is willing to run on.

        tools\pe_target.ps1 build\win32\note.exe 4 0

    Windows reads two version numbers out of the PE optional header before it
    resolves a single import, and refuses anything claiming to need a newer
    system than itself: "The file expects a newer version of Windows.  Upgrade
    your Windows version."  Nothing else about the binary gets a chance to be
    right or wrong.  Windows 95 wants 4.00.

    The linker will not write it.  MSVC dropped support for subsystem versions
    below 5.01 when it dropped Windows XP, and answers /SUBSYSTEM:WINDOWS,4.0
    with LNK4010 and its own default.  The fields themselves are ordinary,
    documented and unrelated to code generation, so they are set here instead.
    This does not make the program work on Windows 95 -- that is what the ANSI
    boundary and the import audit are for -- it only stops the loader refusing
    before any of that is reached.

    Offsets, from the PE signature at e_lfanew:
        +4          COFF header, 20 bytes
        +24         optional header
        +24+40      MajorOperatingSystemVersion, MinorOperatingSystemVersion
        +24+48      MajorSubsystemVersion, MinorSubsystemVersion

    The checksum a few fields along is left alone: it is zero for an ordinary
    executable, and Windows verifies it only for drivers and for DLLs loaded
    into a system process.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory, Position = 0)][string]$Exe,
    [Parameter(Position = 1)][int]$Major = 4,
    [Parameter(Position = 2)][int]$Minor = 0
)

$ErrorActionPreference = 'Stop'

# Read and write in the open, without a helper function between: PowerShell
# unrolls an array on the way out of one, and a byte[] that comes back as
# Object[] binds to nothing that wants bytes -- the failure surfaces later and
# elsewhere, as arithmetic on an array.
$bytes = [IO.File]::ReadAllBytes($Exe)

if ($bytes.Length -lt 0x40 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
    throw "$Exe does not begin with an MZ header."
}

$pe = [BitConverter]::ToInt32($bytes, 0x3C)
if ($pe -le 0 -or ($pe + 0x50) -ge $bytes.Length -or
    $bytes[$pe] -ne 0x50 -or $bytes[$pe + 1] -ne 0x45) {
    throw "$Exe has no PE signature where e_lfanew points."
}

$opt = $pe + 24
foreach ($at in ($opt + 40), ($opt + 48)) {
    [BitConverter]::GetBytes([uint16]$Major).CopyTo($bytes, $at)
    [BitConverter]::GetBytes([uint16]$Minor).CopyTo($bytes, $at + 2)
}

# A virus scanner opens an executable the moment the linker closes it, and this
# runs in that moment.  The catch is untyped on purpose: PowerShell wraps what
# a .NET method throws, so catching UnauthorizedAccessException by name catches
# nothing and the retry never happens.
for ($i = 0; ; $i++) {
    try { [IO.File]::WriteAllBytes($Exe, $bytes); break }
    catch {
        if ($i -ge 40) { throw }
        Start-Sleep -Milliseconds 250
    }
}

"  targets  Windows {0}.{1:D2} (subsystem and OS version)" -f $Major, $Minor
