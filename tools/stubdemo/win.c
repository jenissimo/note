/* THROWAWAY PROOF -- not part of note.  Delete tools\stubdemo\ at will.
 *
 * The smallest thing that can be linked with note's real link line
 * (/SUBSYSTEM:WINDOWS /ENTRY:noteEntry /NODEFAULTLIB) and then given a custom
 * /STUB:.  It opens no window on purpose: it must not steal focus when run.
 *
 * The blob exists only so UPX has something to chew on.  A 4 KB executable
 * makes UPX throw NotCompressibleException and refuse the file, which tells
 * us nothing about what packing does to a DOS stub, so the test subject is
 * padded to roughly the size of a real program.
 */
#include <windows.h>

#define S64 "note is a small fast text editor and this line is here for bulk. "
#define X4(x)  x x x x
#define X16(x) X4(X4(x))
#define X256(x) X16(X16(x))

static const char blob[] = X256(X4(S64));   /* 64 KB of text */

void __stdcall noteEntry(void)
{
    ExitProcess((UINT)(blob[GetTickCount() % sizeof blob] * 0));
}
