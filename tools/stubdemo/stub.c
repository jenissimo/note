/* THROWAWAY PROOF -- not part of note.  Delete tools\stubdemo\ at will.
 *
 * A 16-bit real-mode DOS program, built by Watcom, used as the /STUB: image
 * of a Win32 PE so we can see whether the stub survives the link and UPX.
 * It writes a line with int 21h ah=9 and exits with ah=4Ch, so nothing from
 * the C library is needed beyond whatever wcl's startup insists on.
 */
#include <dos.h>

/* The marker string is deliberately odd so a hexdump of the PE cannot
 * confuse it with the linker's own "This program cannot be run in DOS mode". */
static char msg[] = "NOTEPADTURBO-WATCOM-STUB: real mode, real bytes.\r\n$";

void main(void)
{
    union REGS r;
    struct SREGS s;

    segread(&s);
    s.ds = FP_SEG(msg);
    r.h.ah = 0x09;
    r.x.dx = FP_OFF(msg);
    int86x(0x21, &r, &r, &s);

    r.h.ah = 0x4C;
    r.h.al = 0;
    int86(0x21, &r, &r);
}
