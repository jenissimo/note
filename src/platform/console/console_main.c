/* console_main.c — note for machines with a screen and no window manager.
 *
 * One source, three targets: MS-DOS twice over — through DJGPP in 32-bit
 * protected mode and through Open Watcom in 16-bit real mode — and the
 * Commodore 64 through cc65.  They have almost nothing in common except that
 * all of them give you a fixed grid of characters, a keyboard, and a way to
 * read a file — and that all of them put a colour beside every character on
 * that grid, which is the whole reason an editor this small can still
 * highlight syntax.
 *
 * What it shares with the Windows build is note_buffer for the text and
 * note_syntax for the colours: the caret, the selection, undo, the line index,
 * the search and the lexer all come from the core and behave identically.
 * What it does not share is note_core.c, and that is deliberate rather than
 * lazy — that file reaches for a theme registry that parses #RRGGBB, a session
 * store and a host-ops table, none of which mean anything on a machine whose
 * palette is sixteen fixed colours burned into a chip.
 *
 * The core parts it does want are #included as source rather than added to the
 * link line, because the two build commands are fixed at console_main.c plus
 * note_buffer.c.  One translation unit also lets the profile below reshape the
 * core's size bounds for a 64 KB machine without touching note_config.h, which
 * belongs to someone else.
 *
 * Beyond conio.h, fcntl.h and unistd.h, the DJGPP half reaches for three of
 * its compiler's headers, none of them CRT: pc.h for the one call that writes
 * a cell without moving the cursor, and dpmi.h with go32.h to make an INT 10h
 * call, which is the only way to hand the VGA a font.  The C64 half reaches
 * for nothing at all — its screen is memory and it writes to it.  The real-mode
 * half is the C64's argument applied to a PC: the text screen is memory there
 * too, and the three BIOS calls it still wants are written as inline code
 * rather than reached through a library.
 *
 * Why a second MS-DOS target at all: a Win32 PE begins with an MZ header, and
 * the program that header describes — the DOS stub, normally the one that
 * prints "This program cannot be run in DOS mode" — can be any real-mode
 * executable the linker is handed.  A DJGPP build cannot be that program: it
 * is 32-bit, it needs a DPMI host beside it, and it is three hundred
 * kilobytes.  A 16-bit real-mode build can, so one file becomes an editor on
 * Windows 11, on Windows 95 and on DOS.  Everything the real-mode arm gives
 * up below, it gives up to fit in front of a PE.
 */

/* Open Watcom compiling for 16-bit real mode.  __WATCOMC__ names the compiler
 * and M_I86 the 8086 memory models, which is what tells this apart from the
 * 32-bit DOS target the same compiler can also build. */
#if defined(__WATCOMC__) && defined(M_I86)
  #define DOS16 1
#else
  #define DOS16 0
#endif

/* Whether this build reads the theme catalogue out of its own executable
 * without holding a registry.
 *
 * NOTE_THEME_CATALOGUE is the other way of having the catalogue: parse every
 * definition into note_theme.c's registry and reduce out of that.  It costs
 * NOTE_MAX_THEMES note_themes -- twenty-six kilobytes -- which is the whole
 * of a real-mode DGROUP and then some, and it is why the 16-bit build shipped
 * three themes reduced at build time.
 *
 * This is the same catalogue without the registry.  The definitions are
 * streamed once, each one reduced to sixteen colours as it goes, and what is
 * kept is the answer: a console_theme of sixteen bytes and a name, both in
 * memory the program asks DOS for and addresses __far.  Nothing between the
 * blob and the reduced row is ever resident, so what the near data segment
 * pays is a line buffer and the one theme on screen.
 *
 * DJGPP could use this too and does not, because it has flat memory and the
 * registry costs it nothing it misses.
 *
 * Guarded so a measurement build can turn it off and price it: -DCAT16=0
 * with note_pack.c off the command line is the same 16-bit editor with the
 * three build-time themes it had before the catalogue existed. */
#ifndef CAT16
  #if DOS16
    #define CAT16 1
  #else
    #define CAT16 0
  #endif
#endif

/* ==========================================================================
 * Profile.
 *
 * note_config.h is written for a desktop: a registry of 192 languages, a
 * 192 KB arena and a regex program of 256 instructions.  On a C64 the regex
 * program alone, times NOTE_MAX_RULES, is more memory than the machine has.
 * Its bounds are guarded, so setting them before the include is enough: the
 * numbers stay next to the machine they describe and the core's own file
 * stays untouched.
 * ========================================================================== */

/* The buffer note_syntax_widen copies a compiled-in definition through.
 *
 * The languages below never go near it -- nchar is a byte on every target
 * here, so a definition written as a C string literal is already an nchar
 * string, and syntax_setup hands them over as they are.  note_theme_init does
 * use it, for the core's built-in themes, so a target with the theme registry
 * has to size this for the longest of those; one that has no registry has
 * nothing to widen and keeps the buffer that costs nothing. */
#if defined(__CC65__) || (defined(__WATCOMC__) && defined(M_I86))
  #define NOTE_BUILTIN_MAX 4
#else
  #define NOTE_BUILTIN_MAX 1024
#endif

#if defined(__CC65__)
  #define NOTE_MAX_LANGS       3
  #define NOTE_ARENA_CHARS   768
  /* The 6502 has no room for the Pike VM, so the pattern rules are stubbed
   * out below and this only has to be legal, not useful. */
  #define NOTE_MAX_RULES       1
  #define NOTE_REGEX_PROG      4
  #define NOTE_REGEX_CLASSES   2
  #define NOTE_REGEX_RANGES    4
  /* The longest line either built-in definition carries is a keyword list of
   * a couple of hundred characters.  The desktop's 4 KB buffer is a stack
   * frame cc65 refuses outright, and this sits in BSS besides -- #pragma
   * static-locals moves it there -- so it is sized to the definitions rather
   * than to a round number. */
  #define NOTE_CONF_VALUE_MAX 320
#elif DOS16
  #define NOTE_MAX_LANGS       4
  #define NOTE_ARENA_CHARS  2048
  /* The pack reader is here for the theme catalogue and nothing else, and it
   * is driven directly -- see the streaming reader further down.  This takes
   * note_syntax.c's note_syntax_add_from_pack out with it, which is the only
   * thing in the core that calls note_pack_find, and note_pack_find is the
   * one entry point a caller-supplied window cannot have. */
  #define NOTE_EMBEDDED_PACKS  0
  /* No registry, and themes parsed all the same: this build streams the
   * catalogue and reduces as it goes, so it needs note_conf.c's #RRGGBB. */
  #define NOTE_THEME_PARSE     1
  /* The regex engine is kept, unlike on the C64, but not at desktop size.
   * Two arrays decide what it costs in a 64 KB data segment and both scale
   * with NOTE_REGEX_PROG: the compiled program itself, once per cached rule,
   * and the Pike VM's two thread lists, which carry NOTE_REGEX_SLOTS capture
   * offsets per thread and NOTE_REGEX_PROG threads per list.  At the
   * desktop's 256 instructions and ten groups that pair is over twenty
   * kilobytes; at these numbers it is under four.  Three rules and sixty-four
   * instructions is what the built-in C definition below actually uses --
   * its longest pattern compiles to about a dozen. */
  #define NOTE_MAX_RULES       3
  #define NOTE_REGEX_PROG     64
  #define NOTE_REGEX_CLASSES   4
  #define NOTE_REGEX_RANGES   32
  #define NOTE_REGEX_GROUPS    2
  /* A definition's longest line is a keyword list, and note_syntax_add parses
   * one through a buffer of this size on the stack.  Four kilobytes of stack
   * frame is most of a real-mode stack; the built-ins need a third of one. */
  #define NOTE_CONF_VALUE_MAX 512
#else
  #define NOTE_MAX_LANGS       8
  /* Two languages' worth of names and keywords, and then the theme
   * catalogue's: every definition in themes.pack leaves its name behind in
   * the arena, and there are three hundred and thirty-eight of them. */
  #define NOTE_ARENA_CHARS 16384
  #define NOTE_MAX_RULES       6
  #define NOTE_REGEX_PROG    192
#endif

#include "../../core/note_config.h"

#include <fcntl.h>
#if !DOS16
  #include <conio.h>              /* the real-mode arm draws its own screen */
#endif

#if defined(__CC65__)
  #include <cbm.h>
  #include <unistd.h>
  #define SCREEN_W       40
  #define SCREEN_H       25
  /* The whole machine is 64 KB, and the KERNAL, the screen and the C stack
   * all want a piece of it.  This is what is comfortably left. */
  /* Not a round eight kilobytes: the last quarter-kilobyte of it is what the
   * line-at-a-time repaint is standing on, and a document three per cent
   * shorter is a trade nobody will notice against a screen that redraws on
   * every keystroke. */
  #define TEXT_CAP       7936
  #define UNDO_RECS      16
  /* Characters of undo history, not steps: single-character edits are what
   * this machine's keyboard produces, so this is still deep. */
  #define UNDO_TEXT      128
  /* One entry per line here: note_config.h turns the sparse index off for
   * cc65, since this machine holds eight kilobytes of text and will never
   * hold the document that index exists to make cheap.  A file with more
   * lines than this scans on from the last of them, which is the price of
   * the kilobyte of RAM a longer table would cost. */
  #define LINE_CAP       128
  #define HL_BACK        192      /* lookback for an open block comment    */
  /* A screenful (40 x 24) plus the lookback, and nothing spare: this is the
   * largest single allocation after the document itself. */
  #define HL_CAP         1152     /* text handed to the lexer in one go    */
  #define MAX_SPANS      128
  #define DEFAULT_DEVICE 8
#elif DOS16
  #include <io.h>                 /* open, read, write, close — not stdio  */
  #define SCREEN_W       80
  #define SCREEN_H       25
  /* The small model gives one 64 KB data segment for everything at once: the
   * document, the undo ring, the line index, the lexer's scratch, the regex
   * engine's thread lists, the C library's own data and the eight kilobytes
   * of stack the build is linked with -- which is what a 512-nchar parse
   * buffer and a recursive-descent regex compiler want between them.
   *
   * Twenty-eight kilobytes of document is what was left after all of that,
   * rounded down to leave the segment a few kilobytes of slack.  It is a
   * seven-hundred-line source file, and a good deal more than a program that
   * exists to sit in front of a PE header is likely to be pointed at.
   *
   * It could not be much more in any case: every offset in note_buffer is an
   * int, so a document has to stay under 32767 characters however much room
   * the segment has. */
  #define TEXT_CAP       28672
  #define UNDO_RECS      128
  #define UNDO_TEXT      2048
  /* One entry per line here, as on the C64: note_config.h turns the sparse
   * index off for this compiler too, and for the same reason -- the index
   * exists to keep a ten-megabyte document cheap, and this machine cannot
   * hold one.  Two thousand entries covers the document above down to
   * fourteen-character lines; a file with more lines than this still opens and
   * still edits, it just scans on from the last entry. */
  #define LINE_CAP       2048
  #define HL_BACK        1024     /* lookback for an open block comment    */
  /* A screenful of 80 x 24 is 1920 characters, and the lookback is another
   * kilobyte; four thousand is that with room to land on a line boundary. */
  #define HL_CAP         4096
  /* A dense screen of C lexes to something under three hundred spans. */
  #define MAX_SPANS      320
#else
  #include <io.h>
  #include <unistd.h>
  #include <pc.h>                 /* ScreenPutChar, ScreenSetCursor        */
  #include <dpmi.h>               /* the INT 10h calls the font needs      */
  #include <go32.h>
  #include <sys/movedata.h>
  #include <stdlib.h>             /* system(), for Run                     */
  #define SCREEN_W       80
  #define SCREEN_H       25
  /* DJGPP runs in 32-bit protected mode with flat memory, so the only limit
   * here is taste. */
  #define TEXT_CAP       262144
  #define UNDO_RECS      256
  #define UNDO_TEXT      16384
  #define LINE_CAP       8192
  #define HL_BACK        1024
  #define HL_CAP         6144
  #define MAX_SPANS      768
#endif

#include "../../core/note_buffer.h"
#include "../../core/note_syntax.h"

/* The two helpers note_buffer and note_syntax ask the rest of the core for.
 * They are four lines each; linking note_core.c for them would drag in the
 * document model, the session and the menu tables. */
int n_len(const nchar *s)
{
    int i = 0;
    if (!s) return 0;
    while (s[i]) i++;
    return i;
}

int n_eq(const nchar *a, const nchar *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const nchar *note_basename(const nchar *path)
{
    const nchar *p = path, *last = path;
    for (; *p; p++)
        if (*p == (nchar)'\\' || *p == (nchar)'/') last = p + 1;
    return last;
}

#include "../../core/note_conf.c"
#include "../../core/note_regex.h"

/* Word lists, comment delimiters and quotes cover every language this build
 * ships, and a Pike VM compiled for the 6502 is several kilobytes to answer
 * questions nobody here asks.  There used to be two stubs here for the linker's
 * sake, with note_syntax.c still carrying the machinery that would have called
 * them; NOTE_ENABLE_REGEX now takes both halves out together. */
#if NOTE_ENABLE_REGEX
  #include "../../core/note_regex.c"
#endif

#if defined(__CC65__)
/* NOTE_CONF_VALUE_MAX is down to 512 above, but cc65 caps a whole stack frame
 * at 256 bytes, so even a modest buffer is one local too many.  Static locals
 * move the frame into BSS: note_syntax_add runs twice, at startup, and nothing
 * here recurses, so the only thing given up is re-entrancy nobody wants. */
  #pragma static-locals (on)
#endif

#include "../../core/note_syntax.c"

#if defined(__CC65__)
  #pragma static-locals (off)
#endif

#if NOTE_THEME_CATALOGUE
/* The registry that parses a .theme document, and the assignment that maps one
 * onto sixteen fixed colours.  Both are core files this build had no use for
 * until it had more than three palettes to choose between; note_config.h says
 * which compilers can pay for them, and cc65 is not one of them -- note_theme.c
 * does not compile for the 6502 at all. */
#include "../../core/note_theme.c"
#include "../../core/note_reduce.c"
/* And the LZSS reader, because the catalogue arrives compressed and inside the
 * executable.  Here rather than on the build's source list, which the 16-bit
 * MS-DOS build already has its own copy of on. */
#include "../../core/note_pack.c"
#endif

/* ==========================================================================
 * Colour.
 *
 * A theme here is a table of palette indices, not of RGB triples: on both
 * machines the palette is the hardware's and cannot be changed, so a colour is
 * a number from 0 to 15 and nothing else.  Which number means which colour is
 * the chip's business — the VIC-II's order on one machine, CGA's on the other
 * — and nothing in this file needs to know, because nothing in this file
 * chooses a colour any more.  note_theme_reduce does, from RGB, against the
 * palette the display actually has; see the theme table below.
 *
 * There used to be an enum of sixteen colour names in the VIC-II's order and a
 * sixteen-byte table translating it to CGA at every cell drawn.  Both are gone
 * with the hand-written themes they existed for.
 * ========================================================================== */

typedef struct {
    const char   *name;
    unsigned char bg;
    unsigned char tok[TOK_COUNT];   /* tok[TOK_TEXT] is ordinary text */
    unsigned char gutter_fg, gutter_bg;
    unsigned char ui_fg, ui_bg;     /* status bar and the palette overlay */
    unsigned char caret;            /* the block the cursor fills a cell with */
} console_theme;

/* Where the themes come from, which is the same place on every target: the
 * note_theme definitions in the core, mapped onto sixteen colours by
 * note_theme_reduce.  What differs is when the mapping happens.
 *
 * With the registry (NOTE_THEME_CATALOGUE), the definitions arrive as text at
 * startup -- the built-ins, then the pack in the executable -- and are reduced
 * into this array as they are read.  Without it, the same reduction has
 * already been run by tools/reduce_themes.c and its answer is compiled in;
 * that target holds fewer themes, not different ones.
 *
 * There used to be three palettes written out here by hand as tables of
 * hardware indices, and a translation table beside them, because nothing was
 * reducing colours properly.  Something is now. */
static int g_theme;

#if NOTE_THEME_CATALOGUE

#define THEME_CAP NOTE_MAX_THEMES

/* Not named for the themes: note_theme.c is #included into this translation
 * unit further down, and two static definitions of the same name at file scope
 * are one object in C, not an error.  A counter called g_nthemes here would
 * silently be the registry's own. */
static console_theme g_cat[THEME_CAP];
static int           g_ncat;

#define THEME_COUNT   g_ncat
#define TH            (&g_cat[g_theme])
#define theme_name(i) (g_cat[i].name)

#else

#include "console_themes.h"

#define BUILTIN_COUNT ((int)(sizeof(kThemes) / sizeof(kThemes[0])))

#if CAT16

/* The three built-ins stay rows 0..2.  They are compiled in, they cost
 * nothing, and they are what the editor opens with before it has read a byte
 * of itself; the catalogue is rows 3 and up and arrives the first time
 * anybody asks to see the theme list.
 *
 * TH is one near struct rather than a subscript into a table, because the
 * table it would subscript is __far and the draw loop reads TH at every cell
 * of every repaint.  theme_sync() copies the chosen row into it and
 * theme_apply() is where that happens -- which is already the one call every
 * path that moves g_theme makes. */
static console_theme g_cur;
static int         cat_count(void);
static const char *cat_name(int i);

#define THEME_COUNT   (BUILTIN_COUNT + cat_count())
#define TH            (&g_cur)
#define theme_name(i) cat_name(i)

#else

#define THEME_COUNT   BUILTIN_COUNT
#define TH            (&kThemes[g_theme])
#define theme_name(i) (kThemes[i].name)

#endif
#endif

/* ==========================================================================
 * The screen.
 *
 * One primitive: put a character with a colour at a cell.  Everything draws
 * through it and nothing draws any other way, which is what killed the bug
 * this file used to have — putch on the bottom-right cell scrolls the whole
 * screen up by one, so the status line ate the top line of the file.  Neither
 * of these two writes can scroll, because neither moves a cursor.
 * ========================================================================== */

#if defined(__CC65__)

/* volatile because these are pins, not memory: a compiler that decides a
 * store nobody reads back is dead code would turn the whole screen off. */
#define C64_SCREEN ((volatile unsigned char *)0x0400)
#define C64_COLOR  ((volatile unsigned char *)0xD800)
#define VIC_D018   (*(volatile unsigned char *)0xD018)
#define VIC_BORDER (*(volatile unsigned char *)0xD020)
#define VIC_BG     (*(volatile unsigned char *)0xD021)

/* $D018 selects which 2 KB of the VIC's bank holds the character set.  21 is
 * the uppercase/graphics ROM font, 23 the lowercase one.  A text editor wants
 * the second, so that is the default. */
#define CHARSET_UPPER 21
#define CHARSET_LOWER 23

/* Which of the two, as an index rather than as the register value: the palette
 * picks a font the same way it picks a theme or a language, by moving a number
 * within a list, and font_apply is what that number means to the hardware. */
enum { FONT_LOWER = 0, FONT_UPPER, FONT_COUNT };

static int g_font = FONT_LOWER;

/* Text on this machine is PETSCII, not ASCII, and that is not a translation
 * layer bolted on here — it is what cc65 does to every literal in the file,
 * this one and note_syntax.c alike, so `'a'` is 0x41 and `c >= 'a' && c <= 'z'`
 * in the core's lexer is a test for the unshifted letters.  The keyboard hands
 * back PETSCII, a CBM file holds PETSCII, and a C '\n' assembles to the CR the
 * machine ends its lines with.  So PETSCII is simply the character set here,
 * and this is the one place it has to become something else.
 *
 * A screen code is not a character code: the VIC indexes the font directly.
 * The mapping is the classic one and is the same for both ROM fonts, which is
 * why switching fonts costs one register write and nothing else. */
static unsigned char screen_code(char ch)
{
    unsigned char u = (unsigned char)ch;

    if (u < 0x20) return 0x20;                       /* control: blank   */
    if (u < 0x40) return u;                          /* space .. '?'     */
    if (u < 0x60) return (unsigned char)(u - 0x40);  /* @, letters, [\]  */
    if (u < 0x80) return (unsigned char)(u - 0x20);
    if (u < 0xA0) return 0x20;                       /* control: blank   */
    if (u < 0xC0) return (unsigned char)(u - 0x40);
    if (u < 0xFF) return (unsigned char)(u - 0x80);
    return 0x5E;
}

/* Is this a character the document should hold at all?  PETSCII's two control
 * blocks are colours and cursor moves, not text. */
static int visible(char ch)
{
    unsigned char u = (unsigned char)ch;
    return u >= 0x20 && (u < 0x80 || u >= 0xA0);
}

/* The C64's background is one global colour, not one per cell: only the
 * foreground lives in colour RAM.  Where a cell wants a background of its own
 * — the status bar, the palette overlay — the reverse-video bit stands in for
 * it, filling the cell with the colour and punching the glyph out in the
 * screen background.  It is not the same thing, but it is the same picture.
 */
static void cell(int x, int y, char ch, unsigned char fg, unsigned char bg)
{
    int at = y * SCREEN_W + x;
    unsigned char sc = screen_code(ch);

    if (bg != TH->bg) {
        C64_SCREEN[at] = (unsigned char)(sc | 0x80);
        C64_COLOR[at]  = bg;
    } else {
        C64_SCREEN[at] = sc;
        C64_COLOR[at]  = fg;
    }
}

static void font_apply(void)
{
    VIC_D018 = (unsigned char)(g_font == FONT_UPPER ? CHARSET_UPPER
                                                    : CHARSET_LOWER);
}

static void screen_open(void)
{
    cursor(0);                  /* the cursor here is drawn, not blinked by ROM */
    font_apply();
    VIC_BG     = TH->bg;
    VIC_BORDER = TH->bg;
}

/* Put back the screen the KERNAL hands a BASIC program: the uppercase font,
 * blue on blue.  Written as the VIC-II's own numbers because that is what this
 * is -- the machine's power-on state, not a theme. */
#define C64_BLUE    6
#define C64_LTBLUE 14

static void screen_close(void)
{
    VIC_D018   = CHARSET_UPPER;
    VIC_BG     = C64_BLUE;
    VIC_BORDER = C64_LTBLUE;
    clrscr();
}

static const char *font_name_of(int i)
{
    return i ? "Upper Case / Graphics" : "Lower Case";
}


#define cursor_to(x, y) gotoxy((unsigned char)(x), (unsigned char)(y))
#define scr_key()       ((int)cgetc())

#else   /* MS-DOS, either compiler */

/* Code page 437 has a glyph for every byte, but the ones below a space are
 * control codes in a file and have no business in a document. */
static int visible(char ch)
{
    unsigned char u = (unsigned char)ch;
    return u >= 0x20 && u != 0x7F;
}

#if DOS16

/* Real mode is the machine with the lid off: the text screen is memory at a
 * fixed address, one word per cell with the character in the low byte and the
 * attribute in the high one, and a cell costs a single store.  (Monochrome
 * adapters answer at B000:0000 instead.  Nothing that can run Windows has
 * one, and this build exists to sit in front of a Windows executable.) */
static unsigned short far * const VIDEO = (unsigned short far *)0xB8000000UL;

/* Three BIOS services, written as inline code rather than reached through
 * int86: a register block and a call frame per keystroke is a poor trade for
 * three instructions, and dos.h's version of them would come with a library
 * this file is otherwise not linking.
 *
 * INT 16h/AH=00h blocks for a key and hands back the scan code in AH and the
 * ASCII code -- zero, for the keys that have none -- in AL. */
extern unsigned bios_key(void);
#pragma aux bios_key = "xor ah,ah" "int 16h" value [ax] modify [ax];

/* INT 10h/AH=01h sets the CRTC's cursor to scanlines CH..CL; bit 5 of CH
 * turns it off instead. */
extern void bios_cursor(unsigned shape);
#pragma aux bios_cursor = "mov ah,1" "int 10h" parm [cx] modify [ax];

/* INT 10h/AH=02h moves it, page 0, row in DH and column in DL. */
extern void bios_gotoxy(unsigned rowcol);
#pragma aux bios_gotoxy = "mov ah,2" "xor bh,bh" "int 10h" parm [dx] modify [ax bx];

/* INT 10h/AX=1003h decides what bit 7 of an attribute means. */
extern void bios_blink(unsigned on);
#pragma aux bios_blink = "mov ax,1003h" "int 10h" parm [bx] modify [ax];

static void cell(int x, int y, char ch, unsigned char fg, unsigned char bg)
{
    VIDEO[y * SCREEN_W + x] =
        (unsigned short)(((unsigned)((bg << 4) | fg) << 8) |
                         (unsigned char)ch);
}

static void screen_fill(unsigned char attr)
{
    unsigned short w = (unsigned short)(((unsigned)attr << 8) | ' ');
    int i;
    for (i = 0; i < SCREEN_W * SCREEN_H; i++) VIDEO[i] = w;
}

/* conio's, in name only: the rest of the file clears the screen and expects
 * the theme's own colours underneath. */
#define clrscr() \
    screen_fill((unsigned char)((TH->bg << 4) | TH->tok[TOK_TEXT]))

/* An extended key arrives from the BIOS as one event and reaches read_key as
 * two bytes, because that is the shape getch has and the DJGPP arm's key
 * table is written against it. */
static int g_pushback = -1;

static int scr_key(void)
{
    unsigned k;

    if (g_pushback >= 0) {
        int c = g_pushback;
        g_pushback = -1;
        return c;
    }
    k = bios_key();
    /* AL is 0 for the original extended keys and 0xE0 for the ones the
     * enhanced keyboard added; both mean "the scan code is the key". */
    if ((k & 0xFF) == 0x00 || (k & 0xFF) == 0xE0) {
        g_pushback = (int)(k >> 8);
        return (int)(k & 0xFF);
    }
    return (int)(k & 0xFF);
}

static void screen_open(void)
{
    /* Bit 7 of an attribute is either blink or the high bit of the background
     * colour, and the BIOS boots with it meaning blink.  A theme with a light
     * background needs the other reading. */
    bios_blink(0x0000);

    /* The block this file draws is the cursor; the CRTC's underline beside it
     * would be one cursor too many.  See draw_caret. */
    bios_cursor(0x2000);
}

static void screen_close(void)
{
    bios_cursor(0x0607);        /* the BIOS underline, back for the prompt */
    screen_fill(0x07);
    bios_gotoxy(0);
}

#define cursor_to(x, y) bios_gotoxy((unsigned)(((y) << 8) | (x)))

#else   /* DJGPP */

static void cell(int x, int y, char ch, unsigned char fg, unsigned char bg)
{
    ScreenPutChar((unsigned char)ch, (int)((bg << 4) | fg), x, y);
}

/* The VGA keeps its character generator in plane 2 of video memory rather than
 * in ROM, so a text mode font is not a property of the hardware: the whole set
 * can be replaced at runtime and the adapter never knows the difference.  Each
 * font here is 256 glyphs of 16 scanlines, one byte per row, high bit
 * leftmost, in code page 437 order -- exactly what the VGA holds and what
 * INT 10h/AX=1110h takes.  See assets/NOTICE.md for where Terminus came from.
 * The adapter's own set is not carried here: the BIOS can restore it. */
#include "font_terminus.h"

enum { FONT_ROM = 0, FONT_TERMINUS, FONT_COUNT };

static int g_font = FONT_TERMINUS;

static const char *kFontNames[FONT_COUNT] = {
    "Adapter ROM", "Terminus"
};

/* INT 10h/AX=1110h wants the glyphs in real-mode addressable memory, and a
 * protected-mode pointer is no use to the BIOS, so they go through DJGPP's
 * transfer buffer.  That buffer is 16 KB by default and a whole font is 4 KB,
 * so the set goes over in one call. */
static void font_upload(const unsigned char *bits, int first, int count)
{
    __dpmi_regs r;

    dosmemput(bits, count * 16, __tb);

    r.x.ax = 0x1110;
    r.h.bh = 16;            /* bytes per glyph */
    r.h.bl = 0;             /* block 0         */
    r.x.cx = (unsigned short)count;
    r.x.dx = (unsigned short)first;
    r.x.es = (unsigned short)(__tb >> 4);
    r.x.bp = (unsigned short)(__tb & 15);
    __dpmi_int(0x10, &r);
}

static void font_apply(void)
{
    __dpmi_regs r;

    switch (g_font) {
    case FONT_TERMINUS: font_upload(kFontTerminus[0], 0, 256); break;
    default:
        /* Reloading the ROM set is a BIOS service, so the original is always
         * one call away and note never has to carry a copy of it. */
        r.x.ax = 0x1114;
        r.h.bl = 0;
        __dpmi_int(0x10, &r);
        break;
    }
}

static const char *font_name_of(int i) { return kFontNames[i]; }

static void screen_open(void)
{
    __dpmi_regs r;

    /* Bit 7 of an attribute is either blink or the high bit of the background
     * colour, and the BIOS boots with it meaning blink.  A theme with a light
     * background needs the other reading. */
    r.x.ax = 0x1003;
    r.x.bx = 0x0000;
    __dpmi_int(0x10, &r);

    /* The chosen font has to be uploaded before anything is drawn; until this
     * runs the adapter is still showing whatever the BIOS left in plane 2. */
    font_apply();

    /* The BIOS cursor is a two-scanline underline, and it is drawn by the
     * CRTC in the cell's foreground colour, over whatever is there.  The one
     * this file draws is a cell in reverse video, which is both bigger and
     * still shows the character; two cursors would be one too many. */
    _setcursortype(_NOCURSOR);
}

static void screen_close(void)
{
    /* Leave the adapter as it was found: the replaced character generator
     * would otherwise outlive note and reshape the DOS prompt behind it. */
    _setcursortype(_NORMALCURSOR);
    g_font = FONT_ROM;
    font_apply();
    textattr(0x07);
    clrscr();
}

#define cursor_to(x, y) ScreenSetCursor((y), (x))
#define scr_key()       (getch())

#endif  /* DOS16 */
#endif  /* __CC65__ */

static void fill_row(int y, int from, int to, unsigned char fg, unsigned char bg)
{
    int x;
    for (x = from; x < to; x++) cell(x, y, ' ', fg, bg);
}

/* ==========================================================================
 * Storage.  All of it static: neither target has a heap worth the name, and
 * a fixed layout is one less thing to fail at three in the morning.
 * ========================================================================== */

static nchar     g_text[TEXT_CAP];
static note_edit g_undo[UNDO_RECS];
static nchar     g_utext[UNDO_TEXT];
static int       g_lines[LINE_CAP];
static note_buffer g_buf;

static char g_path[64];
static int  g_top;              /* first line on screen                    */
static int  g_left;             /* first column, for lines wider than the
                                 * screen: the C64's forty columns make
                                 * horizontal scrolling a necessity, not a
                                 * luxury                                  */
static int  g_goal = -1;        /* the column Up/Down is trying to keep     */
static int  g_msg_time;         /* frames the status message has left      */
static char g_msg[SCREEN_W];

static int  g_show_status = 1;
static int  g_show_lineno = 1;
static int  g_show_syntax = 1;
static int  g_lang;

/* The run of text handed to the lexer.  Loading and saving deliberately do
 * not use it: a second array the size of the document is two more kilobytes
 * the C64 does not have, and neither operation needs one. */
static nchar g_hl[HL_CAP];

static note_span g_spans[MAX_SPANS];
static int       g_nspans;

static note_arena g_arena;

#define TEXT_ROWS (SCREEN_H - (g_show_status ? 1 : 0))

/* ==========================================================================
 * Languages.
 *
 * Written in the format note_syntax_add already parses, so these say exactly
 * what a .syntax file on a desktop says and go through exactly the same code.
 * The C64 gets no `rule` lines because it has no regular-expression engine to
 * run them; word lists, comments, quotes and numbers are the lexer's own and
 * cover most of what either language looks like.
 * ========================================================================== */

static const char kLangC[] =
    "name = C\n"
    "extensions = c h cpp hpp cc\n"
    "line_comment = //\n"
    "block_comment = /* */\n"
    "quotes = \"'\n"
    "preproc = yes\n"
    "keywords = if else for while do return break continue switch case "
    "default goto sizeof static const extern typedef struct union enum "
    "register volatile auto\n"
    "types = int char long short unsigned signed float double void\n"
#if !defined(__CC65__)
    "rule = operator [-+*/%=<>.:;,~&|^!?]\n"
    "rule = number \\b0[Xx][0-9A-Fa-f]+\\b\n"
    "rule = type \\b[A-Z_][0-9A-Z_][0-9A-Z_]+\\b\n"
#endif
    ;

static const char kLangBasic[] =
    "name = BASIC\n"
    "extensions = bas prg lst\n"
    "line_comment = REM\n"
    "quotes = \"\n"
    "case_sensitive = no\n"
    "keywords = print input if then else for to step next goto gosub return "
    "poke peek data read let dim end run list open close get sys new\n"
    "types = str chr asc val len mid left right abs int rnd\n"
    ;

static void syntax_setup(void)
{
    note_syntax_init(&g_arena);
    note_syntax_add(&g_arena, kLangC);
    note_syntax_add(&g_arena, kLangBasic);
}

/* ==========================================================================
 * The screen
 * ========================================================================== */

static int digits(int v)
{
    int n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

static int gutter_w(void)
{
    int n;
    if (!g_show_lineno) return 0;
    n = digits(note_buffer_lines(&g_buf));
    if (n < 3) n = 3;
    if (n > 5) n = 5;
    return n + 1;               /* one column of air before the text */
}

/* Lexes the visible region once per redraw.
 *
 * The lexer wants a flat run of characters and the buffer is a gap buffer, so
 * the region is copied out first.  It starts a little above the view because a
 * block comment opened off the top of the screen still colours what is on it;
 * note_syntax_safe_start says how far back is far enough. */
static void highlight(void)
{
    int total = note_buffer_len(&g_buf);
    int nlines = note_buffer_lines(&g_buf);
    int last = g_top + TEXT_ROWS;
    int vis, end, from, got, begin;

    g_nspans = 0;
    if (!g_show_syntax || g_lang <= LANG_NONE) return;

    vis = note_buffer_line_start(&g_buf, g_top);
    if (last >= nlines) end = total;
    else                end = note_buffer_line_start(&g_buf, last);

    from = vis - HL_BACK;
    if (from < 0) from = 0;
    if (end - from > HL_CAP - 1) end = from + HL_CAP - 1;

    got = note_buffer_copy(&g_buf, from, end - from, g_hl, HL_CAP);

    begin = note_syntax_safe_start(g_lang, g_hl, got, vis - from, vis - from);
    g_nspans = note_tokenize(g_lang, g_hl + begin, got - begin,
                             from + begin, g_spans, MAX_SPANS);
}

/* Spans arrive sorted and the screen is painted in document order, so one
 * index walking forward answers every cell.  Anything else would be a linear
 * scan of the span list per character, which a 6502 notices. */
static int g_span_at;

/* How much of the screen the last key invalidated.
 *
 * A full repaint is a thousand cells, and on a 1 MHz 6502 a cell is not cheap:
 * a bounds-checked read out of the gap buffer, a screen code, two stores.  A
 * character typed into the middle of a line changes one line, so that is what
 * it should cost.  Anything that moves text between lines, scrolls, or could
 * change how the text below it lexes takes the whole screen. */
#define DMG_NONE 0
#define DMG_LINE 1
#define DMG_ALL  2

static int g_damage = DMG_ALL;

/* Where the cursor block is painted at the moment, so that moving it can put
 * back what it was covering instead of repainting the screen.  -1 means the
 * screen has been repainted under it and there is nothing to put back. */
static int g_cur_x = -1, g_cur_y;

/* colour_at walks forward and only forward, which is right for painting a
 * screen in document order and no use at all for asking about one cell.  This
 * is the same question asked out of order; the span list is short enough that
 * a scan of it costs less than the redraw it saves. */
static unsigned char colour_of(int pos)
{
    int i;
    for (i = 0; i < g_nspans; i++) {
        if (g_spans[i].start > pos) break;
        if (g_spans[i].start + g_spans[i].len > pos)
            return TH->tok[g_spans[i].kind];
    }
    return TH->tok[TOK_TEXT];
}

static unsigned char colour_at(int pos)
{
    while (g_span_at < g_nspans &&
           g_spans[g_span_at].start + g_spans[g_span_at].len <= pos)
        g_span_at++;

    if (g_span_at < g_nspans && g_spans[g_span_at].start <= pos)
        return TH->tok[g_spans[g_span_at].kind];

    return TH->tok[TOK_TEXT];
}

static void draw_status(void)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int col  = g_buf.caret - note_buffer_line_start(&g_buf, line);
    int i, n = 0, y = SCREEN_H - 1;
    char s[SCREEN_W + 1];

    if (!g_show_status) return;

    /* A message, when there is one, otherwise where the caret is. */
    if (g_msg_time > 0) {
        for (i = 0; g_msg[i] && n < SCREEN_W - 1; i++) s[n++] = g_msg[i];
        g_msg_time--;
    } else {
        const char *name = g_path[0] ? g_path : "untitled";
        if (g_buf.dirty && n < SCREEN_W - 1) s[n++] = '*';
        for (i = 0; name[i] && n < SCREEN_W - 14; i++) s[n++] = name[i];
        while (n < SCREEN_W - 13) s[n++] = ' ';
        /* "L" and "C" rather than "Line"/"Col": forty columns is forty. */
        s[n++] = 'L';
        {
            char num[8];
            int  v = line + 1, d = 0;
            do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 7);
            while (d) s[n++] = num[--d];
        }
        s[n++] = ' ';
        s[n++] = 'C';
        {
            char num[8];
            int  v = col + 1, d = 0;
            do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 7);
            while (d) s[n++] = num[--d];
        }
    }
    while (n < SCREEN_W) s[n++] = ' ';

    for (i = 0; i < SCREEN_W; i++) cell(i, y, s[i], TH->ui_fg, TH->ui_bg);
}

/* One row: the gutter, the text, and blank out to the right edge.  The caller
 * has lexed the region and reset g_span_at, because colour_at only ever walks
 * forward and a row painted out of order would read stale spans. */
static void draw_row(int row, int gw, int total)
{
    {
        int line = g_top + row;
        int x = gw;

        if (gw) {
            char num[8];
            int  d = 0, k;
            if (line < total) {
                int v = line + 1;
                do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 7);
            }
            for (k = 0; k < gw - 1; k++) {
                int at = gw - 1 - k;            /* right-aligned */
                cell(k, row, at <= d ? num[at - 1] : ' ',
                     TH->gutter_fg, TH->gutter_bg);
            }
            cell(gw - 1, row, ' ', TH->gutter_fg, TH->gutter_bg);
        }

        if (line < total) {
            int start = note_buffer_line_start(&g_buf, line);
            int len   = note_buffer_line_len(&g_buf, line);
            int i;
            for (i = g_left; i < len && x < SCREEN_W; i++, x++) {
                nchar c = note_buffer_at(&g_buf, start + i);
                cell(x, row, visible((char)c) ? (char)c : '.',
                     colour_at(start + i), TH->bg);
            }
            /* The span walk must see every character of the line, including
             * the ones scrolled off to the right, or the next line starts
             * from a stale span. */
            if (len > 0) colour_at(start + len - 1);
        }
        fill_row(row, x, SCREEN_W, TH->tok[TOK_TEXT], TH->bg);
    }
    if (g_cur_y == row) g_cur_x = -1;   /* the block was painted over */
}

static void draw(void)
{
    int row, total = note_buffer_lines(&g_buf);
    int gw = gutter_w();

    highlight();
    g_span_at = 0;

    for (row = 0; row < TEXT_ROWS; row++) draw_row(row, gw, total);

    /* With the status bar hidden the loop above has already painted the last
     * row as text, so there is nothing left to clear. */
    draw_status();
    g_cur_x = -1;
}

/* Whether an edit of this character can be repainted as one line.
 *
 * A line break cannot: it moves every line below it.  Nor can a character that
 * changes how the text *after* it lexes -- a quote, a slash, a hash -- because
 * the colours further down the screen would go stale and stay that way until
 * something else forced a repaint.  Letters, digits, spaces and underscores
 * open no string and start no comment in any language this build ships, and
 * they are what typing mostly consists of.
 *
 * With the highlighter off the question does not arise and everything is
 * cheap, which is worth saying out loud: on this machine that is the
 * difference between one line and a thousand cells. */
static int plain_edit(char c)
{
    if (c == '\n' || c == '\r') return 0;
    if (!g_show_syntax || g_lang <= LANG_NONE) return 1;
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == ' ' || c == '_';
}

/* The one line an ordinary keystroke can have changed. */
static void draw_line_only(void)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int row  = line - g_top;

    if (row < 0 || row >= TEXT_ROWS) { draw(); return; }

    highlight();
    g_span_at = 0;
    draw_row(row, gutter_w(), note_buffer_lines(&g_buf));
    draw_status();
}

static void message(const char *s)
{
    int i;
    for (i = 0; s[i] && i < SCREEN_W - 1; i++) g_msg[i] = s[i];
    g_msg[i] = 0;
    g_msg_time = 1;
}

/* Keeps the caret on screen, scrolling as little as possible. */
static void follow_caret(void)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int col  = g_buf.caret - note_buffer_line_start(&g_buf, line);
    int w    = SCREEN_W - gutter_w();

    if (line < g_top) g_top = line;
    if (line >= g_top + TEXT_ROWS) g_top = line - TEXT_ROWS + 1;
    if (col < g_left) g_left = col;
    if (col >= g_left + w) g_left = col - w + 1;
    if (g_top < 0) g_top = 0;
    if (g_left < 0) g_left = 0;
}

/* The cursor is a cell in reverse video, not a hardware caret.
 *
 * Both machines have one in ROM and neither is worth using.  The VGA's is two
 * scanlines along the bottom of the cell, drawn by the CRTC in the cell's
 * foreground colour; the C64's is the KERNAL's, and the KERNAL is not editing
 * this document.  A filled cell is what every editor these machines ever ran
 * used, it says which character you are on rather than which gap you are in,
 * and on the C64 it costs exactly what the status bar already costs: the
 * reverse-video bit.
 *
 * It does not blink.  Blinking needs a clock in the input loop, the input loop
 * blocks on a key, and a cursor that is missing half the time is not obviously
 * better than one that is not. */
/* One cell of the text area, painted from the document, upright or reversed. */
static void paint_cell(int x, int y, int reversed)
{
    int line = g_top + y;
    int col  = g_left + x - gutter_w();
    int start, len;
    char ch = ' ';
    unsigned char fg = TH->tok[TOK_TEXT];

    if (y < 0 || y >= TEXT_ROWS || x < gutter_w() || x >= SCREEN_W) return;

    if (line < note_buffer_lines(&g_buf)) {
        start = note_buffer_line_start(&g_buf, line);
        len   = note_buffer_line_len(&g_buf, line);
        if (col >= 0 && col < len) {
            nchar c = note_buffer_at(&g_buf, start + col);
            ch = visible((char)c) ? (char)c : '.';
            fg = colour_of(start + col);
        }
    }

    if (reversed) cell(x, y, ch, TH->bg, TH->caret);
    else          cell(x, y, ch, fg, TH->bg);
}

/* Moves the block to where the caret is now: put back the cell it was
 * covering, reverse the new one.  Two cells rather than a screen, which on a
 * 6502 is the whole difference between an arrow key and a slideshow. */
static void draw_caret(void)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int col  = g_buf.caret - note_buffer_line_start(&g_buf, line);
    int x    = col - g_left + gutter_w();
    int y    = line - g_top;

    if (g_cur_x >= 0 && (g_cur_x != x || g_cur_y != y))
        paint_cell(g_cur_x, g_cur_y, 0);

    if (y < 0 || y >= TEXT_ROWS || x < gutter_w() || x >= SCREEN_W) {
        g_cur_x = -1;
        return;
    }
    g_cur_x = x;
    g_cur_y = y;
    paint_cell(x, y, 1);
}

/* ==========================================================================
 * Files
 * ========================================================================== */

static void set_lang(void)
{
    g_lang = note_lang_from_path(g_path);
}

/* The file is read straight into the buffer's own storage and then handed to
 * note_buffer_set, which copies text[i] to buf[i] — the same index, so with
 * the same array on both sides it is a no-op that rebuilds the gap and the
 * line index.  A separate load buffer would be a second copy of the document
 * in a machine that has room for one. */
static int load(const char *path)
{
    int fd, n, i;

#if defined(__CC65__)
    fd = open(path, O_RDONLY);
#else
    fd = open(path, O_RDONLY | O_BINARY);
#endif
    if (fd < 0) return 0;

    /* One byte per character on both machines, in the machine's own character
     * set: PETSCII on the C64, whatever code page DOS is running on the other.
     * nchar is a byte here, so what was read is already the text. */
    n = read(fd, (char *)g_text, TEXT_CAP - 1);
    close(fd);
    if (n < 0) return 0;
    g_text[n] = 0;

    if (!note_buffer_set(&g_buf, g_text)) return 0;

    for (i = 0; path[i] && i < (int)sizeof(g_path) - 1; i++) g_path[i] = path[i];
    g_path[i] = 0;
    set_lang();
    return 1;
}

static int save(const char *path)
{
    int fd, i = 0, n = note_buffer_len(&g_buf);
    char chunk[64];

    if (!path[0]) return 0;

#if defined(__CC65__)
    /* Commodore DOS will not overwrite; the @0: prefix says to replace. */
    {
        static char name[72];
        int j = 0;
        name[j++] = '@'; name[j++] = '0'; name[j++] = ':';
        for (i = 0; path[i] && j < (int)sizeof(name) - 1; i++) name[j++] = path[i];
        name[j] = 0;
        fd = open(name, O_WRONLY | O_CREAT | O_TRUNC);
    }
#else
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
#endif
    if (fd < 0) return 0;

    /* Written a chunk at a time straight out of the gap buffer, so saving
     * needs sixty-four bytes rather than a second copy of the document. */
    while (i < n) {
        int k = 0;
        while (k < (int)sizeof(chunk) && i < n)
            chunk[k++] = (char)note_buffer_at(&g_buf, i++);
        if (write(fd, chunk, k) != k) { close(fd); return 0; }
    }
    close(fd);

    g_buf.dirty = 0;
    return 1;
}

/* ==========================================================================
 * Keys
 *
 * The two keyboards agree on almost nothing, so everything above the editing
 * loop speaks in these instead.
 * ========================================================================== */

enum {
    K_NONE = 0, K_CHAR, K_LEFT, K_RIGHT, K_UP, K_DOWN,
    K_HOME, K_END, K_PGUP, K_PGDN, K_ENTER, K_BACK, K_DEL,
    K_WORDL, K_WORDR, K_DOCTOP, K_DOCEND, K_PALETTE, K_CMD
};

/* When read_key returns K_CMD it has already put the command in here. */
static int g_key_cmd;

enum {
    C_NONE = 0,
    C_SAVE, C_SAVEAS, C_OPEN, C_QUIT,
    C_UNDO, C_REDO, C_FIND, C_AGAIN, C_GOTO,
    C_LINENO, C_STATUS, C_SYNTAX, C_THEME, C_FONT, C_LANG,
    C_WORDL, C_WORDR, C_PGUP, C_PGDN, C_DOCTOP, C_DOCEND,
    C_LINESTART, C_LINEEND,
    C_RUN
};

static int read_key(int *ch)
{
    int c = scr_key();
    *ch = c;
    g_key_cmd = C_NONE;

#if defined(__CC65__)
    switch (c) {
    case CH_CURS_LEFT:  return K_LEFT;
    case CH_CURS_RIGHT: return K_RIGHT;
    case CH_CURS_UP:    return K_UP;
    case CH_CURS_DOWN:  return K_DOWN;
    case CH_ENTER:      return K_ENTER;
    case CH_DEL:        return K_BACK;
    case CH_HOME:       return K_HOME;
    case CH_STOP:       g_key_cmd = C_QUIT; return K_CMD;
    }
    /* The C64 has no Ctrl row worth hanging shortcuts on, so the function
     * keys carry the six commands that earn a key of their own and the
     * palette on f8 carries every other. */
    switch (c) {
    case 133: g_key_cmd = C_SAVE;  return K_CMD;   /* f1 */
    case 137: g_key_cmd = C_UNDO;  return K_CMD;   /* f2 */
    case 134: g_key_cmd = C_OPEN;  return K_CMD;   /* f3 */
    case 138: g_key_cmd = C_REDO;  return K_CMD;   /* f4 */
    case 135: g_key_cmd = C_FIND;  return K_CMD;   /* f5 */
    case 139: g_key_cmd = C_AGAIN; return K_CMD;   /* f6 */
    case 136: return K_PALETTE;                    /* f7 */
    case 140: g_key_cmd = C_THEME; return K_CMD;   /* f8 */
    }
    /* Ctrl+letter reaches the keyboard buffer as 1..26 on this machine, minus
     * the handful the KERNAL claims.  Where it does not, the palette has all
     * of these too, which is the point of having one. */
    switch (c) {
    case 1:  return K_DOCTOP;   /* Ctrl+A */
    case 24: return K_DOCEND;   /* Ctrl+X */
    case 15: return K_WORDL;    /* Ctrl+O */
    case 16: return K_WORDR;    /* Ctrl+P */
    case 21: return K_PGUP;     /* Ctrl+U */
    case 10: return K_PGDN;     /* Ctrl+J */
    case 5:  return K_END;      /* Ctrl+E */
    }
    if (visible((char)c)) return K_CHAR;
    return K_NONE;
#else
    if (c == 0 || c == 224) {           /* an extended key follows */
        c = scr_key();
        switch (c) {
        case 75:  return K_LEFT;
        case 77:  return K_RIGHT;
        case 72:  return K_UP;
        case 80:  return K_DOWN;
        case 71:  return K_HOME;
        case 79:  return K_END;
        case 73:  return K_PGUP;
        case 81:  return K_PGDN;
        case 83:  return K_DEL;
        case 115: return K_WORDL;    /* Ctrl+Left  */
        case 116: return K_WORDR;    /* Ctrl+Right */
        case 119: return K_DOCTOP;   /* Ctrl+Home  */
        case 117: return K_DOCEND;   /* Ctrl+End   */
        }
        return K_NONE;
    }
    switch (c) {
    case 13:  return K_ENTER;
    case 8:   return K_BACK;
    case 27:  return K_PALETTE;              /* Esc opens it, Esc closes it */
    case 16:  return K_PALETTE;              /* Ctrl+P */
    case 19:  g_key_cmd = C_SAVE;    return K_CMD;
    case 15:  g_key_cmd = C_OPEN;    return K_CMD;
    case 17:  g_key_cmd = C_QUIT;    return K_CMD;
    case 26:  g_key_cmd = C_UNDO;    return K_CMD;
    case 25:  g_key_cmd = C_REDO;    return K_CMD;
    case 6:   g_key_cmd = C_FIND;    return K_CMD;
    case 14:  g_key_cmd = C_AGAIN;   return K_CMD;
    case 7:   g_key_cmd = C_GOTO;    return K_CMD;
    case 12:  g_key_cmd = C_LINENO;  return K_CMD;
    case 20:  g_key_cmd = C_STATUS;  return K_CMD;
    case 5:   g_key_cmd = C_THEME;   return K_CMD;
    case 24:  g_key_cmd = C_SYNTAX;  return K_CMD;
    case 18:  g_key_cmd = C_RUN;     return K_CMD;   /* Ctrl+R */
#if !DOS16
    /* The real-mode build has no second font to switch to. */
    case 11:  g_key_cmd = C_FONT;    return K_CMD;
#endif
    }
    if (visible((char)c)) return K_CHAR;
    return K_NONE;
#endif
}

#define HIST_MAX  8         /* remembered command lines                    */
#define HIST_CELL 96        /* and how long one of them may be             */

static int str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return i;
}

/* Asks for a line of text on the status row.  The only prompt there is.
 *
 * `hist` is optional and holds the most recent entry first, so Up walks back
 * through it the way every shell has taught people to expect.  Passing none
 * gives the plain prompt, which is what everything but Run wants. */
static int prompt_hist(const char *label, char *out, int cap,
                       char (*hist)[HIST_CELL], int nhist)
{
    int n = 0, i, hsel = -1;

    for (;;) {
        int c, used = 0, y = SCREEN_H - 1;

        for (i = 0; label[i] && used < SCREEN_W; i++)
            cell(used++, y, label[i], TH->ui_fg, TH->ui_bg);
        for (i = 0; i < n && used < SCREEN_W; i++)
            cell(used++, y, out[i], TH->ui_fg, TH->ui_bg);
        fill_row(y, used, SCREEN_W, TH->ui_fg, TH->ui_bg);
        cursor_to(used < SCREEN_W ? used : SCREEN_W - 1, y);

        c = scr_key();
#if defined(__CC65__)
        if (c == CH_ENTER) break;
        if (c == CH_DEL) { if (n > 0) n--; continue; }
        if (c == CH_STOP) return 0;
        if (c == CH_CURS_UP)   { c = -1; }
        else if (c == CH_CURS_DOWN) { c = -2; }
#else
        if (c == '\r' || c == '\n') break;
        if (c == 8 || c == 127) { if (n > 0) n--; continue; }
        if (c == 27) return 0;
        if (c == 0 || c == 224) {
            /* An extended key arrives as a lead byte and a scan code. */
            int e = scr_key();
            if (e == 72)      c = -1;   /* Up   */
            else if (e == 80) c = -2;   /* Down */
            else continue;
        }
#endif
        if (c == -1) {                          /* older */
            if (hist && hsel + 1 < nhist) n = str_copy(out, hist[++hsel], cap);
            continue;
        }
        if (c == -2) {                          /* newer */
            if (hist && hsel > 0)      n = str_copy(out, hist[--hsel], cap);
            else if (hsel == 0)        { hsel = -1; n = 0; }
            continue;
        }
        if (visible((char)c) && n < cap - 1) out[n++] = (char)c;
    }
    out[n] = 0;
    return n > 0;
}

#define prompt(label, out, cap) prompt_hist((label), (out), (cap), 0, 0)

/* ==========================================================================
 * Motion
 * ========================================================================== */

static int is_word(nchar c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static void move_word(int dir)
{
    int at = g_buf.caret, n = note_buffer_len(&g_buf);

    if (dir < 0) {
        while (at > 0 && !is_word(note_buffer_at(&g_buf, at - 1))) at--;
        while (at > 0 &&  is_word(note_buffer_at(&g_buf, at - 1))) at--;
    } else {
        while (at < n &&  is_word(note_buffer_at(&g_buf, at))) at++;
        while (at < n && !is_word(note_buffer_at(&g_buf, at))) at++;
    }
    note_buffer_caret_set(&g_buf, at, 0);
}

/* Up and down keep the column they started from, even across lines too short
 * to hold it.  Collapsing to the end of every short line is the single thing
 * that makes a homemade editor feel homemade. */
static void move_line(int delta)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int col  = g_buf.caret - note_buffer_line_start(&g_buf, line);
    int want = line + delta, len, goal;

    goal = (g_goal >= 0) ? g_goal : col;

    if (want < 0) want = 0;
    if (want >= note_buffer_lines(&g_buf)) want = note_buffer_lines(&g_buf) - 1;

    len = note_buffer_line_len(&g_buf, want);
    col = goal > len ? len : goal;
    note_buffer_caret_set(&g_buf, note_buffer_line_start(&g_buf, want) + col, 0);
    g_goal = goal;
}

static void move_line_edge(int to_end)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int at = note_buffer_line_start(&g_buf, line);
    if (to_end) at += note_buffer_line_len(&g_buf, line);
    note_buffer_caret_set(&g_buf, at, 0);
}

/* ==========================================================================
 * Find
 * ========================================================================== */

static char g_find[32];

static void do_find(int again)
{
    int at;

    if (!again || !g_find[0]) {
        if (!prompt("find: ", g_find, sizeof(g_find))) return;
    }
    at = note_buffer_find(&g_buf, (const nchar *)g_find,
                          g_buf.caret + 1, FIND_DOWN);
    if (at < 0) { message("not found"); return; }
    note_buffer_caret_set(&g_buf, at, 0);
    follow_caret();
}

/* Go To Line used to be a prompt on the status row.  It is a palette mode now
 * -- see PM_GOTO -- because the answer is worth seeing before it is committed
 * and a one-line prompt has nowhere to show it. */

/* ==========================================================================
 * Run
 *
 * The smallest thing that turns an editor into somewhere you can work: type a
 * command, have it run beside the file rather than wherever note was started
 * from, and come back to the text.  On DOS that is a shell command.  The C64
 * has no shell and no second process, so there Run means what RUN has always
 * meant on that machine, and it lives further down with the BASIC code.
 *
 * Real mode has the same shell and the same system() -- four kilobytes of it,
 * measured, which this target can afford.  What it cannot afford is the
 * printf that prints the exit code, so that one line goes out through
 * INT 21h/AH=09h instead.
 * ========================================================================== */

#if !defined(__CC65__)

#if DOS16
  #include <stdlib.h>             /* system()                              */
  #include <direct.h>             /* chdir()                               */

/* AH=09h writes a string terminated by a '$' at the cursor, which is where
 * the child left it.  DS already addresses DGROUP in the small model, so a
 * near pointer in DX is the whole call. */
extern void dos_print(const char *dollar_terminated);
#pragma aux dos_print = "mov ah,9" "int 21h" parm [dx] modify [ax];
#endif

static int str_same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char g_runhist[HIST_MAX][HIST_CELL];
static int  g_nrunhist;

/* Most recent first, and a repeat of the last command does not push a second
 * copy -- re-running the same thing is the common case, not a new entry. */
static void hist_push(const char *cmd)
{
    int i;

    if (!cmd[0]) return;
    if (g_nrunhist && str_same(g_runhist[0], cmd)) return;

    for (i = (g_nrunhist < HIST_MAX ? g_nrunhist : HIST_MAX - 1); i > 0; i--)
        str_copy(g_runhist[i], g_runhist[i - 1], HIST_CELL);
    str_copy(g_runhist[0], cmd, HIST_CELL);
    if (g_nrunhist < HIST_MAX) g_nrunhist++;
}

/* The directory part of a path, or "." when it names a file in the current
 * one.  A bare drive letter keeps its slash: "C:" means something else. */
static void dir_of(const char *path, char *out, int cap)
{
    int i, cut = -1;

    for (i = 0; path[i]; i++)
        if (path[i] == '\\' || path[i] == '/') cut = i;

    if (cut < 0) { str_copy(out, ".", cap); return; }
    if (cut == 0) { str_copy(out, "\\", cap); return; }
    if (cut >= cap) cut = cap - 1;
    for (i = 0; i < cut; i++) out[i] = path[i];
    out[i] = 0;
    if (i == 2 && out[1] == ':') { out[i++] = '\\'; out[i] = 0; }
}

static void do_run(void)
{
    char cmd[HIST_CELL], dir[64];
    int rc;
#if !DOS16
    int saved_font;
#endif

    if (!prompt_hist("run: ", cmd, sizeof(cmd), g_runhist, g_nrunhist)) return;
    hist_push(cmd);

    /* Running the file as it was two edits ago is the one way this feature
     * can waste someone's afternoon, so an unsaved buffer with a name is
     * written out first. */
    if (g_buf.dirty && g_path[0]) save(g_path);

    dir_of(g_path, dir, sizeof(dir));
    if (dir[0]) chdir(dir);

    /* The child expects the adapter the BIOS gave it: its own font, its own
     * colours, its own cursor.  Hand all of them back and take them again
     * afterwards.  The real-mode build never replaced the font, so it has one
     * thing less to hand over. */
#if !DOS16
    saved_font = g_font;
    g_font = FONT_ROM;
    font_apply();
    textattr(0x07);
    clrscr();
    cursor_to(0, 0);
#else
    bios_cursor(0x0607);
    screen_fill(0x07);
    bios_gotoxy(0);
#endif

    rc = system(cmd);

#if !DOS16
    cprintf("\r\n-- exit %d, press a key --", rc);
#else
    {
        /* The same line, assembled by hand: the digits are two lines of code
         * and the printf that would format them is three kilobytes.  A '$'
         * ends it, because that is what AH=09h reads. */
        const char *head = "\r\n-- exit ";
        const char *tail = ", press a key --$";
        char line[40], num[6];
        int v = rc < 0 ? -rc : rc, d = 0, n = 0, i;

        for (i = 0; head[i]; i++) line[n++] = head[i];
        if (rc < 0) line[n++] = '-';
        do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 5);
        while (d) line[n++] = num[--d];
        for (i = 0; tail[i]; i++) line[n++] = tail[i];
        line[n] = 0;
        dos_print(line);
    }
#endif
    scr_key();

#if !DOS16
    g_font = saved_font;
#endif
    screen_open();
    clrscr();
}

#endif  /* !__CC65__ */

/* ==========================================================================
 * The theme catalogue.
 *
 * Three palettes are enough to cycle through and not enough to justify a list,
 * so where the machine can carry it the list is the shipped catalogue: the
 * same themes.pack the Windows build reads, three hundred and thirty-eight of
 * them, mapped onto the sixteen colours a CGA card has.
 *
 * The mapping is note_theme_reduce, which is why this is worth doing at all.
 * Nearest-match per colour turns a theme into mush -- a keyword and a comment
 * three shades apart both land on the same entry and the reader has lost the
 * distinction the colours existed to carry -- so the core solves it as an
 * assignment instead, and keeps kinds apart in a fixed, documented order when
 * sixteen entries will not stretch.
 *
 * The definitions live inside the executable, and are found by opening it and
 * looking.  build-retro.bat appends the LZSS blob note_pack.h describes to
 * note-dos.exe; this scans back from the end of its own file for the NPK1
 * magic, which is precisely what that magic is documented to be for -- an
 * MS-DOS program has no PE resource directory to reach into and can open
 * itself.  A loose file beside the executable would have worked and would have
 * been one more thing to lose; and 170 KB of text compresses to about a
 * seventh of that, which the format is already able to read a byte at a time.
 * ========================================================================== */

#if NOTE_THEME_CATALOGUE

/* Longer than any definition in the pack once its comments are gone: a theme
 * is a name, a flag and fifteen colours, which runs to about four hundred
 * characters. */
#define THEME_DEF_MAX 640

static char g_def[THEME_DEF_MAX];
static int  g_deflen;

static void theme_flush(void)
{
    if (g_deflen) {
        g_def[g_deflen] = 0;
        note_theme_add(&g_arena, g_def);
    }
    g_deflen = 0;
}

/* A line of three dashes separates definitions; comments and blank lines are
 * dropped here rather than passed on, which is most of the file's bulk and all
 * of the reason the buffer above can be this small. */
static void theme_line(const char *line, int len)
{
    int i;

    if (len == 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
        theme_flush();
        return;
    }
    if (len == 0 || line[0] == '#') return;
    if (g_deflen + len + 1 >= THEME_DEF_MAX) return;

    for (i = 0; i < len; i++) g_def[g_deflen++] = line[i];
    g_def[g_deflen++] = '\n';
}

/* The tail of the executable, which is where an appended blob is.  Sized so
 * that a compressed catalogue several times the shipped one still lands inside
 * it; the shipped one is about 24 KB. */
#define THEME_BLOB_MAX 65536

static unsigned char g_blob[THEME_BLOB_MAX];
static note_pack     g_pack;

/* Reads the tail of `path` and returns how many bytes of the blob start at
 * *off within it, or 0 when there is no pack in there.
 *
 * The last NPK1 in the tail is the one, not the first: the build appends this
 * one after everything else, and four bytes of magic can occur by chance in a
 * third of a megabyte of code. */
static int blob_from_self(const char *path, int *off)
{
    long size, start;
    int fd, n, i;

    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return 0;

    size  = lseek(fd, 0L, SEEK_END);
    start = size > (long)THEME_BLOB_MAX ? size - (long)THEME_BLOB_MAX : 0L;
    lseek(fd, start, SEEK_SET);
    n = (int)read(fd, g_blob, (unsigned)THEME_BLOB_MAX);
    close(fd);
    if (n <= 0) return 0;

    *off = -1;
    for (i = 0; i + NOTE_PACK_HEADER <= n; i++)
        if (g_blob[i] == 'N' && g_blob[i + 1] == 'P' &&
            g_blob[i + 2] == 'K' && g_blob[i + 3] == '1') *off = i;

    return (*off < 0) ? 0 : n - *off;
}

/* One definition at a time, out of the stream.  Nothing here ever holds the
 * pack: note_pack_get hands back a byte, a line accumulates, and a line of
 * three dashes is what turns the definition just read into a registry entry. */
static int themes_read(const char *path)
{
    static char line[128];
    int off = 0, len, c, nl = 0;

    len = blob_from_self(path, &off);
    if (!len) return 0;
    if (!note_pack_open(&g_pack, g_blob + off, (unsigned long)len)) return 0;

    g_deflen = 0;
    for (;;) {
        c = note_pack_get(&g_pack);
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[nl] = 0;
            theme_line(line, nl);
            nl = 0;
            continue;
        }
        /* A line longer than this is not a theme's; truncating it leaves
         * note_conf_next a key with a short value, which is what a
         * half-written definition already gets. */
        if (nl < (int)sizeof(line) - 1) line[nl++] = (char)c;
    }
    line[nl] = 0;
    theme_line(line, nl);
    theme_flush();
    return 1;
}

static void themes_setup(const char *argv0)
{
    note_theme_map m;
    char path[80];
    int i, n;

    g_ncat = 0;

    /* The core's built-ins register first, so the list has Light, Dark and
     * Commodore 64 in it whether or not a pack is found. */
    note_theme_init(&g_arena);

    /* The executable is the pack.  argv[0] is a full path on every DOS since
     * 3.0, which is every DOS that can run a DJGPP binary; if it somehow is
     * not, the three built-ins above are still a theme list. */
    str_copy(path, argv0, (int)sizeof(path));
    if (path[0]) themes_read(path);

    n = note_theme_count();
    for (i = 0; i < n && g_ncat < THEME_CAP; i++) {
        const note_theme *t = note_theme_get(i);
        console_theme    *c = &g_cat[g_ncat];
        int k;

        note_theme_reduce(&m, t, note_pal_ega16, 16);

        c->name      = t->name;
        c->bg        = m.bg;
        c->gutter_fg = m.gutter_fg;
        c->gutter_bg = m.gutter_bg;
        c->ui_fg     = m.ui_fg;
        c->ui_bg     = m.ui_bg;
        c->caret     = m.caret;
        for (k = 0; k < TOK_COUNT; k++) c->tok[k] = m.tok[k];
        g_ncat++;
    }

    /* By name, not by number: the pack decides how long the list is and the
     * registry decides the order, so the row this starts on is looked up the
     * same way tools/reduce_themes.c looks it up for the targets that have
     * their table compiled in.  A PC text screen is black when DOS hands it
     * over, so Dark is what note should look like when it takes it. */
    i = note_theme_find("Dark");
    g_theme = (i >= 0 && i < g_ncat) ? i : 0;
}

#endif  /* NOTE_THEME_CATALOGUE */

/* ==========================================================================
 * The theme pack on a real-mode machine.
 *
 * Same definitions as everywhere else, same reduction, and no registry.
 *
 * What note.exe actually carries is assets\core.themes.pack, the curated
 * fourteen, and not the 338 of assets\themes.pack that note-dos.exe gets:
 * the difference is 25,559 bytes compressed and note.exe is measured against
 * 128 KB.  Nothing below cares which -- it streams whatever blob is on the
 * end of the file, and a build made with compress_packs.ps1 -Catalogue puts
 * all 338 through this same code.  CAT_MAX is sized for that.
 *
 * The registry is what put this out of reach: NOTE_MAX_THEMES note_themes is
 * twenty-six kilobytes and the near data segment has about one to spare.  So
 * nothing here is resident that does not have to be.  The compressed blob and
 * the 4 KB LZSS window live in memory asked of DOS and are addressed __far --
 * see the note in note_pack.h -- and the stream is read once, each definition
 * reduced onto the sixteen colours a CGA card has the moment its separator
 * arrives.  What survives a definition is sixteen bytes of palette indices
 * and its name, both __far as well.  A theme costs this build about fifty
 * bytes of memory it did not have to find in DGROUP, and DGROUP pays for a
 * line buffer and the one theme on screen.
 *
 * Where the blob is: the end of this executable.  The Win32 half of the same
 * file reads the same bytes at the same offsets -- one physical copy,
 * appended once by build.bat after the link -- and finds it the same way,
 * because an MS-DOS program has no PE resource directory to reach into and
 * the two halves had to agree on something both could seek to.
 * ========================================================================== */

#if CAT16

#include <io.h>
#include <malloc.h>              /* _fmalloc: the far heap                 */
#include <i86.h>                 /* FP_SEG, FP_OFF, MK_FP                  */
#include <dos.h>                 /* _dos_read, which takes a far buffer    */

#include "../../core/note_theme.h"
#include "../../core/note_reduce.c"
#include "../../core/note_pack.h"

#ifndef SEEK_SET
  #define SEEK_SET 0
  #define SEEK_END 2
#endif

/* How many catalogue rows the far tables are sized for.  Not a limit anyone
 * is meant to reach: what decides how many definitions the blob holds is the
 * 128 KB the packed note.min.exe is measured against, and that runs out well
 * before this does.  A pack with more than this many is read up to here and
 * the rest ignored, which is a short list rather than a crash. */
#define CAT_MAX   384
/* One name slot, NUL included.  The longest in the shipped catalogue is
 * "Penumbra Light Contrast Plus Plus", at 33. */
#define CAT_NAME  36

static unsigned char __far *cat_blob;
static unsigned char __far *cat_win;
static char          __far *cat_names;
static console_theme __far *cat_rows;
static unsigned             cat_bloblen;
static int                  cat_n = -1;   /* -1: not read yet */
static const char          *cat_self;     /* argv[0] */

static int cat_count(void) { return cat_n > 0 ? cat_n : 0; }

/* A far block whose offset is small, so that indexing it cannot walk off the
 * end of its segment: _fmalloc hands back whatever the far heap has, and a
 * 27 KB blob placed at offset 0xF000 would wrap four kilobytes in.
 * Normalising once here is cheaper than normalising in the inner loop, which
 * is the only other place it could happen. */
static unsigned char __far *far_get(unsigned n)
{
    void __far *p = _fmalloc(n);
    unsigned long lin;

    if (!p) return (unsigned char __far *)0;
    lin = ((unsigned long)FP_SEG(p) << 4) + (unsigned long)FP_OFF(p);
    return (unsigned char __far *)MK_FP((unsigned)(lin >> 4),
                                        (unsigned)(lin & 0x000FU));
}

/* The last NPK1 in the last 64 KB of an open file, as a file offset, or -1.
 *
 * The last and not the first, for the reason the DJGPP arm scans backwards:
 * four bytes of magic turn up by chance in a couple of hundred kilobytes of
 * code, and the blob is appended after everything else.  The three-byte
 * overlap between reads is so that a magic split across two of them is still
 * seen. */
static long cat_locate(int fd, long size)
{
    char buf[512];
    long pos   = size > 65536L ? size - 65536L : 0L;
    long found = -1;
    int  n, i;

    if (lseek(fd, pos, SEEK_SET) < 0) return -1;
    for (;;) {
        n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (i = 0; i + 4 <= n; i++)
            if (buf[i] == 'N' && buf[i + 1] == 'P' &&
                buf[i + 2] == 'K' && buf[i + 3] == '1') found = pos + i;
        if (n < (int)sizeof(buf)) break;
        pos += n - 3;
        if (lseek(fd, pos, SEEK_SET) < 0) break;
    }
    return found;
}

/* Opens this executable and pulls the blob out of its tail into far memory.
 * Read with _dos_read rather than read(), because read()'s buffer is a near
 * pointer in this memory model and the whole point of the destination is
 * that it is not in DGROUP. */
static int cat_read_blob(const char *path)
{
    int      fd;
    long     size, off;
    unsigned got;

    fd = open(path, O_RDONLY | O_BINARY);
    if (fd < 0) return 0;

    size = lseek(fd, 0L, SEEK_END);
    off  = (size > 0) ? cat_locate(fd, size) : -1;
    /* A blob this big is not one of ours and would not fit a segment either;
     * refusing here is how a stray magic in the code stops before the
     * decompressor rather than inside it. */
    if (off < 0 || size - off > 60000L) { close(fd); return 0; }

    cat_bloblen = (unsigned)(size - off);
    cat_blob    = far_get(cat_bloblen);
    if (!cat_blob || lseek(fd, off, SEEK_SET) < 0) { close(fd); return 0; }
    if (_dos_read(fd, cat_blob, cat_bloblen, &got) || got != cat_bloblen) {
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

/* One `key = value` line into the theme being built.  note_conf_next is the
 * parser note_theme_add uses, called on a single line rather than on a whole
 * document, so a key spelled correctly here is spelled correctly everywhere.
 * `name` is the one it does not hand on, because the name never goes into the
 * note_theme at all -- it goes straight to its far slot. */
static void cat_field(note_theme *T, char *nm, const char *line)
{
    nchar key[24], val[40];
    const nchar *p = (const nchar *)line;
    int i;

    if (!note_conf_next(&p, key, 24, val, 40)) return;

    if (n_eq(key, N("name"))) {
        for (i = 0; i < CAT_NAME - 1 && val[i]; i++) nm[i] = (char)val[i];
        nm[i] = 0;
    }
    else if (n_eq(key, N("dark")))       T->dark      = note_conf_bool(val) ? 1 : 0;
    else if (n_eq(key, N("background"))) T->bg        = note_conf_color(val);
    else if (n_eq(key, N("foreground"))) T->fg        = note_conf_color(val);
    else if (n_eq(key, N("gutter_bg")))  T->gutter_bg = note_conf_color(val);
    else if (n_eq(key, N("gutter_fg")))  T->gutter_fg = note_conf_color(val);
    else if (n_eq(key, N("selection")))  T->sel_bg    = note_conf_color(val);
    else if (n_eq(key, N("caret")))      T->caret     = note_conf_color(val);
    else if (n_eq(key, N("ui_bg")))      T->ui_bg     = note_conf_color(val);
    else if (n_eq(key, N("ui_fg")))      T->ui_fg     = note_conf_color(val);
    else if (n_eq(key, N("keyword")))    T->tok[TOK_KEYWORD]  = note_conf_color(val);
    else if (n_eq(key, N("type")))       T->tok[TOK_TYPE]     = note_conf_color(val);
    else if (n_eq(key, N("comment")))    T->tok[TOK_COMMENT]  = note_conf_color(val);
    else if (n_eq(key, N("string")))     T->tok[TOK_STRING]   = note_conf_color(val);
    else if (n_eq(key, N("number")))     T->tok[TOK_NUMBER]   = note_conf_color(val);
    else if (n_eq(key, N("preproc")))    T->tok[TOK_PREPROC]  = note_conf_color(val);
    else if (n_eq(key, N("operator")))   T->tok[TOK_OPERATOR] = note_conf_color(val);
}

static void cat_blank(note_theme *T)
{
    int i;
    for (i = 0; i < (int)sizeof(*T); i++) ((unsigned char *)T)[i] = 0;
}

/* The definition just read, reduced and kept.  The three defaults are
 * note_theme_add's, written out again rather than shared because the function
 * they live in is the registry this build does not have. */
static void cat_commit(note_theme *T, const char *nm)
{
    note_theme_map m;
    int i;

    if (!nm[0] || cat_n >= CAT_MAX) return;

    T->tok[TOK_TEXT] = T->fg;
    for (i = 1; i < TOK_COUNT; i++) if (!T->tok[i]) T->tok[i] = T->fg;
    if (!T->caret) T->caret = T->fg;

    note_theme_reduce(&m, T, note_pal_ega16, 16);

    cat_rows[cat_n].name      = 0;    /* the name is in cat_names */
    cat_rows[cat_n].bg        = m.bg;
    cat_rows[cat_n].gutter_fg = m.gutter_fg;
    cat_rows[cat_n].gutter_bg = m.gutter_bg;
    cat_rows[cat_n].ui_fg     = m.ui_fg;
    cat_rows[cat_n].ui_bg     = m.ui_bg;
    cat_rows[cat_n].caret     = m.caret;
    for (i = 0; i < TOK_COUNT; i++) cat_rows[cat_n].tok[i] = m.tok[i];

    for (i = 0; i < CAT_NAME - 1 && nm[i]; i++)
        cat_names[(long)cat_n * CAT_NAME + i] = nm[i];
    cat_names[(long)cat_n * CAT_NAME + i] = 0;
    cat_n++;
}

/* Reads the catalogue, once, and never holds more of it than a line.
 *
 * Deliberately not at startup.  A pass over the stream is a hundred and four
 * kilobytes through a far window plus three hundred reductions, which is a
 * second or two on the 8086 this build is compiled for; doing it when the
 * theme list is first opened puts that where somebody has already asked for
 * a list of themes, and leaves an editor that opens a file as fast as it did
 * before any of this existed. */
static void cat_load(void)
{
    note_pack  z;
    note_theme T;
    char line[96], nm[CAT_NAME];
    int  nl = 0, c;

    if (cat_n >= 0) return;
    cat_n = 0;
    if (!cat_self || !cat_self[0]) return;
    if (!cat_read_blob(cat_self)) return;

    cat_win   = far_get(NOTE_PACK_WINDOW);
    cat_names = (char __far *)far_get(CAT_MAX * CAT_NAME);
    cat_rows  = (console_theme __far *)far_get(CAT_MAX * sizeof(console_theme));
    if (!cat_win || !cat_names || !cat_rows) return;

    z.win = cat_win;
    if (!note_pack_open(&z, cat_blob, (unsigned long)cat_bloblen)) return;

    cat_blank(&T);
    nm[0] = 0;
    for (;;) {
        c = note_pack_get(&z);
        if (c == '\r') continue;
        if (c >= 0 && c != '\n') {
            /* Longer than any line a theme definition has; a keyword list
             * would not fit and no theme carries one. */
            if (nl < (int)sizeof(line) - 1) line[nl++] = (char)c;
            continue;
        }
        line[nl] = 0;
        if (nl >= 3 && line[0] == '-' && line[1] == '-' && line[2] == '-') {
            cat_commit(&T, nm);
            cat_blank(&T);
            nm[0] = 0;
        } else if (nl) {
            cat_field(&T, nm, line);
        }
        nl = 0;
        if (c < 0) break;
    }
    cat_commit(&T, nm);
}

/* The name of row i, for the list.  A far name has to be copied somewhere the
 * caller can read as an ordinary string; one buffer is enough because every
 * caller is done with the answer before it asks again. */
static const char *cat_name(int i)
{
    static char buf[CAT_NAME];
    int k = 0;

    if (i < BUILTIN_COUNT) return kThemes[i].name;
    i -= BUILTIN_COUNT;
    if (i < 0 || i >= cat_count()) return "";
    for (k = 0; k < CAT_NAME - 1; k++) {
        buf[k] = cat_names[(long)i * CAT_NAME + k];
        if (!buf[k]) break;
    }
    buf[k] = 0;
    return buf;
}

/* g_theme moved; put the row it now names where the draw loop reads it. */
static void theme_sync(void)
{
    int i = g_theme;

    if (i < 0) i = 0;
    if (i < BUILTIN_COUNT) { g_cur = kThemes[i]; return; }
    i -= BUILTIN_COUNT;
    if (i < cat_count()) g_cur = cat_rows[i];
}

#endif  /* CAT16 */

/* ==========================================================================
 * Commands
 * ========================================================================== */

static int g_running = 1;

/* Four commands are not commands at all but a way into one of the palette's
 * list modes, and the palette is written below because it is what calls this.
 * They are still reachable from a key, which is why this is a prototype rather
 * than a rearrangement. */
static void palette_run(int mode);
enum { PM_CMDS = 0, PM_THEME, PM_LANG, PM_GOTO, PM_FONT };

/* The one part of a theme that is not a per-cell attribute, and so the one
 * part changing theme has to do for itself: the VIC-II holds the background in
 * a register and the border in another.  A DOS text screen has neither -- every
 * cell carries its own two colours and draw() repaints all of them -- so there
 * is nothing to do on that side, which is what makes this cheap enough to run
 * on every arrow key of the theme list. */
static void theme_apply(void)
{
#if CAT16
    /* Where the rows are __far, TH is a near copy of one of them and this is
     * what keeps it the row g_theme names. */
    theme_sync();
#endif
#if defined(__CC65__)
    VIC_BG     = TH->bg;
    VIC_BORDER = TH->bg;
#endif
}

static void do_command(int cmd)
{
    switch (cmd) {
    case C_SAVE:
        if (!g_path[0] && !prompt("save as: ", g_path, sizeof(g_path))) break;
        set_lang();
        message(save(g_path) ? "saved" : "could not write that file");
        break;

    case C_SAVEAS: {
        char path[64];
        path[0] = 0;
        if (!prompt("save as: ", path, sizeof(path))) break;
        {
            int i;
            for (i = 0; path[i]; i++) g_path[i] = path[i];
            g_path[i] = 0;
        }
        set_lang();
        message(save(g_path) ? "saved" : "could not write that file");
        break;
    }

    case C_OPEN: {
        char path[64];
        path[0] = 0;
        if (!prompt("open: ", path, sizeof(path))) break;
        if (!load(path)) message("could not open that file");
        g_top = g_left = 0;
        break;
    }

    case C_QUIT:
        if (g_buf.dirty) {
            char yn[4];
            yn[0] = 0;
            if (!prompt("unsaved changes, quit? (y) ", yn, sizeof(yn)) ||
                (yn[0] != 'y' && yn[0] != 'Y')) break;
        }
        g_running = 0;
        break;

    case C_UNDO: if (!note_buffer_undo(&g_buf)) message("nothing to undo"); break;
    case C_REDO: if (!note_buffer_redo(&g_buf)) message("nothing to redo"); break;
    case C_FIND:  do_find(0); break;
    case C_AGAIN: do_find(1); break;
    case C_GOTO:  palette_run(PM_GOTO); break;

    case C_LINENO: g_show_lineno = !g_show_lineno; break;
    case C_STATUS: g_show_status = !g_show_status; break;
    case C_SYNTAX:
        g_show_syntax = !g_show_syntax;
        message(g_show_syntax ? "highlighting on" : "highlighting off");
        break;

    /* Three lists, not three "next"s.  Stepping blindly to the next item was
     * the whole of the choice this build offered; what it offers now is the
     * palette in the matching mode, which is the same list the Windows build
     * shows and behaves the same way -- filter as you type, preview as the
     * selection moves, Escape puts back what was there. */
    case C_THEME: palette_run(PM_THEME); break;
#if !DOS16
    case C_FONT:  palette_run(PM_FONT);  break;
#endif
    case C_LANG:  palette_run(PM_LANG);  break;

    case C_WORDL: move_word(-1); break;
    case C_WORDR: move_word(1);  break;
    case C_PGUP:  move_line(-TEXT_ROWS); break;
    case C_PGDN:  move_line(TEXT_ROWS);  break;
    case C_DOCTOP: note_buffer_caret_set(&g_buf, 0, 0); break;
    case C_DOCEND: note_buffer_caret_set(&g_buf, note_buffer_len(&g_buf), 0); break;
    case C_LINESTART: move_line_edge(0); break;
    case C_LINEEND:   move_line_edge(1); break;

#if !defined(__CC65__)
    case C_RUN: do_run(); break;
#endif
    }
}

/* ==========================================================================
 * The command palette.
 *
 * Everything the editor can do is in here, which is what lets the shortcuts
 * be the fast path rather than the only one.  On the C64 it is closer to the
 * only path: a machine with no Ctrl row cannot hang twenty commands off the
 * keyboard, but it can show them in a list and let you type three letters.
 *
 * MODES
 *
 * The list is not always the commands.  Choosing "View: Theme" does not step
 * to the next theme, it turns the same overlay into a list of themes with the
 * same filter over it, and the same for languages and fonts; Go To Line turns
 * it into a box that takes digits.  That is the shape the Windows build's
 * palette has -- see kPalModes in win32_palette.c -- and the reason for it is
 * the same on both: a choice you cannot see is not a choice, it is a guess
 * repeated until it comes out right.
 *
 * A mode is four questions rather than a table of function pointers: how many
 * rows, what is row i called, what does selecting row i do now, and what was
 * true before the overlay opened.  Four switches cost less on a 6502 than four
 * indirect calls per mode, and there are five modes, not fifty.
 *
 * Every list mode previews.  The overlay is drawn over a screen that draw()
 * has just repainted, so a mode that changes what draw() would produce -- a
 * theme, a language, a font, where the caret is -- shows its answer for free
 * as the selection moves.  Escape puts back what was current when the palette
 * opened; Enter keeps what is on screen.
 * ========================================================================== */

typedef struct { const char *label; unsigned char cmd; } palette_row;

static const palette_row kPalette[] = {
    { "File: Save",              C_SAVE },
    { "File: Save As",           C_SAVEAS },
    { "File: Open",              C_OPEN },
#if !defined(__CC65__)
    { "Run: Command",            C_RUN },
#endif
    { "File: Quit",              C_QUIT },
    { "Edit: Undo",              C_UNDO },
    { "Edit: Redo",              C_REDO },
    { "Edit: Find",              C_FIND },
    { "Edit: Find Next",         C_AGAIN },
    { "Edit: Go To Line...",     C_GOTO },
    { "View: Line Numbers",      C_LINENO },
    { "View: Status Bar",        C_STATUS },
    { "View: Highlighting",      C_SYNTAX },
    { "View: Theme...",          C_THEME },
#if !DOS16
    { "View: Font...",           C_FONT },
#endif
    { "View: Language...",       C_LANG },
    { "Go: Word Left",           C_WORDL },
    { "Go: Word Right",          C_WORDR },
    { "Go: Page Up",             C_PGUP },
    { "Go: Page Down",           C_PGDN },
    { "Go: Start Of Document",   C_DOCTOP },
    { "Go: End Of Document",     C_DOCEND },
    { "Go: Start Of Line",       C_LINESTART },
    { "Go: End Of Line",         C_LINEEND }
};
#define PALETTE_COUNT ((int)(sizeof(kPalette) / sizeof(kPalette[0])))

#define PAL_ROWS  10
/* Wide enough for the longest label and no wider: on eighty columns a panel
 * that reached both edges would stop reading as an overlay. */
#if SCREEN_W - 4 > 44
  #define PAL_W   44
#else
  #define PAL_W   (SCREEN_W - 4)
#endif
#define PAL_X     ((SCREEN_W - PAL_W) / 2)

/* The match list has to hold the longest list any mode can show, which is the
 * themes wherever there are more of them than there are commands.  A row index
 * is a byte where it can be: on a 6502 a sixteen-bit subscript is a shift and
 * two loads at every row of every redraw, and the longest list there is the
 * commands. */
#if NOTE_THEME_CATALOGUE
  #define PAL_MATCH_CAP THEME_CAP
  typedef short pal_index;
#elif CAT16
  /* The catalogue is a longer list than a byte can index, and CAT_MAX of
   * these is three quarters of a kilobyte -- most of what the near data
   * segment has left.  So the array is __far too, allocated beside the rows
   * it indexes; kMatchNear is what stands in until then and what the command
   * list, which is short and available before any of this has been read,
   * uses either way. */
  #define PAL_MATCH_CAP (BUILTIN_COUNT + CAT_MAX)
  typedef short pal_index;
#else
  #define PAL_MATCH_CAP PALETTE_COUNT
  typedef unsigned char pal_index;
#endif

static char      g_query[24];
#if CAT16
/* Long enough for the command list and the three built-in themes, which is
 * every list there is until the catalogue has been read. */
static pal_index kMatchNear[PALETTE_COUNT + BUILTIN_COUNT];
/* Both are set in main(): a near address is not a constant a far pointer can
 * be initialised with at compile time in this memory model. */
static pal_index __far *g_match;
static int              g_matchcap;
#else
static pal_index g_match[PAL_MATCH_CAP];
#define g_matchcap PAL_MATCH_CAP
#endif
static int       g_nmatch, g_sel;

/* Which list is on screen; see the modes note above. */
static int g_pal_mode = PM_CMDS;

/* Case folding written as a difference between two literals rather than as
 * the number 32: in PETSCII the capitals sit 0x80 above the lowercase letters,
 * not 0x20 below them, and the compiler already knows which set it is in. */
static char lower_c(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - ('A' - 'a')) : c;
}

/* Every query character has to appear in the label, in order: "vt" finds
 * "View: Next Theme" without anyone having to remember where it lives. */
static int fuzzy(const char *label, const char *q)
{
    int i = 0;
    while (*q) {
        while (label[i] && lower_c(label[i]) != lower_c(*q)) i++;
        if (!label[i]) return 0;
        i++; q++;
    }
    return 1;
}

/* ---- what the mode on screen is a list of ------------------------------- */

static int pal_count(void)
{
    switch (g_pal_mode) {
    case PM_THEME: return THEME_COUNT;
    case PM_LANG:  return note_lang_count();
#if !DOS16
    case PM_FONT:  return FONT_COUNT;
#endif
    /* The query is the answer here, so there is nothing to list. */
    case PM_GOTO:  return 0;
    }
    return PALETTE_COUNT;
}

static const char *pal_label(int i)
{
    switch (g_pal_mode) {
    case PM_THEME: return theme_name(i);
    case PM_LANG:  return note_lang_get(i)->name;
#if !DOS16
    case PM_FONT:  return font_name_of(i);
#endif
    }
    return kPalette[i].label;
}

/* The two strings a mode only has to name, indexed rather than switched on:
 * a jump table and five returns is more 6502 than a pointer lookup, and these
 * two answer no question the mode's own state could change.
 *
 * The first is drawn in front of the '>' so the overlay says which list it is;
 * the command list is the one that needs no telling.  The second is what
 * stands where the rows would be when there are none. */
static const char *const kPalPrompt[5] = { 0, "theme", "lang", "line", "font" };

static const char *const kPalEmpty[5] = {
    "no matching command", "no matching theme", "no matching language",
    "type a line number",  "no matching font"
};

#define pal_prompt() (kPalPrompt[g_pal_mode])
#define pal_empty()  (kPalEmpty[g_pal_mode])

/* Every list mode is a choice of one number -- which theme, which language,
 * which font -- so a pointer to that number is the whole of what the selection
 * has to reach.  One switch here spares three more in the preview, the entry
 * and the restore, which on a 6502 is worth writing down. */
static int *pal_target(void)
{
    switch (g_pal_mode) {
    case PM_THEME: return &g_theme;
    case PM_LANG:  return &g_lang;
#if !DOS16
    case PM_FONT:  return &g_font;
#endif
    }
    return 0;
}

/* Applying the selection, which is what makes this a preview rather than a
 * description.  draw() runs before every keystroke is read, so changing the
 * state it reads is the whole of showing the answer.
 *
 * theme_apply runs whichever number moved, because it is two register writes
 * on one machine and nothing at all on the other.  The font does not get the
 * same treatment: on the VGA applying one means pushing four kilobytes of
 * glyphs through the BIOS, which is not something to do on a keystroke that
 * was about a theme. */
static void pal_preview(void)
{
    if (g_pal_mode == PM_GOTO) {
        int v = 0, i, n = note_buffer_lines(&g_buf);
        for (i = 0; g_query[i]; i++) v = v * 10 + (g_query[i] - '0');
        if (v < 1) return;
        if (v > n) v = n;
        note_buffer_caret_set(&g_buf, note_buffer_line_start(&g_buf, v - 1), 0);
        follow_caret();
        return;
    }
    if (g_nmatch) {
        int *at = pal_target();
        if (!at) return;
        *at = g_match[g_sel];
        theme_apply();
#if !DOS16
        if (g_pal_mode == PM_FONT) font_apply();
#endif
    }
}

static void palette_filter(void)
{
    int i, n = pal_count();

    g_nmatch = 0;
    for (i = 0; i < n && g_nmatch < g_matchcap; i++)
        if (fuzzy(pal_label(i), g_query))
            g_match[g_nmatch++] = (pal_index)i;
    if (g_sel >= g_nmatch) g_sel = g_nmatch ? g_nmatch - 1 : 0;
}

static void palette_draw(void)
{
    int shown = g_nmatch > PAL_ROWS ? PAL_ROWS : g_nmatch;
    int y0 = (SCREEN_H - (shown + 2)) / 2;
    int first = 0, i, k, y;

    if (g_sel >= PAL_ROWS) first = g_sel - PAL_ROWS + 1;

    /* The query line: the mode's name if it has one, then the '>', then what
     * has been typed, with the caret sitting after it. */
    y = y0;
    {
        const char *p = pal_prompt();
        int at = PAL_X;

        for (i = 0; p && p[i] && at < PAL_X + PAL_W - 4; i++)
            cell(at++, y, p[i], TH->ui_fg, TH->ui_bg);
        cell(at++, y, '>', TH->ui_fg, TH->ui_bg);
        cell(at++, y, ' ', TH->ui_fg, TH->ui_bg);
        for (i = 0; g_query[i] && at < PAL_X + PAL_W - 1; i++)
            cell(at++, y, g_query[i], TH->ui_fg, TH->ui_bg);
        fill_row(y, at, PAL_X + PAL_W, TH->ui_fg, TH->ui_bg);
        cursor_to(at, y);
    }

    /* The selected row swaps the panel's two colours rather than taking a
     * third.  On the C64 that is the only way an overlay can have a body at
     * all: colour RAM holds a foreground and the background is one global
     * register, so a panel is a block of reverse-video cells and the
     * selection is a block of them the other way up. */
    for (k = 0; k < shown; k++) {
        const char *lab = pal_label(g_match[first + k]);
        int sel = (first + k) == g_sel;
        unsigned char fg = sel ? TH->ui_bg : TH->ui_fg;
        unsigned char bg = sel ? TH->ui_fg : TH->ui_bg;

        y = y0 + 1 + k;
        cell(PAL_X, y, ' ', fg, bg);
        for (i = 0; lab[i] && i < PAL_W - 2; i++)
            cell(PAL_X + 1 + i, y, lab[i], fg, bg);
        fill_row(y, PAL_X + 1 + i, PAL_X + PAL_W, fg, bg);
    }

    y = y0 + 1 + shown;
    fill_row(y, PAL_X, PAL_X + PAL_W, TH->ui_fg, TH->ui_bg);
    /* A mode whose query is the answer has no rows and so is always "empty";
     * what it puts here is an invitation, and an invitation to type a line
     * number outstays its welcome the moment one has been typed. */
    if (!g_nmatch && !(g_pal_mode == PM_GOTO && g_query[0])) {
        const char *s = pal_empty();
        for (i = 0; s[i] && i < PAL_W - 2; i++)
            cell(PAL_X + 1 + i, y, s[i], TH->ui_fg, TH->ui_bg);
    }
}

/* Which mode a command from the list opens instead of running, or -1 for the
 * commands that are simply commands. */
static int pal_mode_for(int cmd)
{
    switch (cmd) {
    case C_THEME: return PM_THEME;
    case C_LANG:  return PM_LANG;
    case C_GOTO:  return PM_GOTO;
#if !DOS16
    case C_FONT:  return PM_FONT;
#endif
    }
    return -1;
}

/* Starts the overlay in `mode` and does not come back until it is done.
 *
 * Switching mode happens in this loop rather than by calling in again, so a
 * palette that goes command list -> theme list is one activation with one set
 * of remembered state: what Escape puts back is what was true when the overlay
 * first opened, whichever list it is showing by then. */
/* Puts a mode on screen: empty query, the whole list, and the selection where
 * that list already stands, previewed. */
static void pal_enter(int mode)
{
    int *at;

#if CAT16
    /* The theme list is the first thing that needs the catalogue, and the
     * only thing, so this is where reading it belongs.  The match array goes
     * __far at the same moment and for the same reason the rows did: three
     * hundred and eighty-seven shorts is most of what DGROUP has left. */
    if (mode == PM_THEME && cat_n < 0) {
        cat_load();
        if (cat_n > 0) {
            pal_index __far *m = (pal_index __far *)
                far_get((unsigned)(PAL_MATCH_CAP * sizeof(pal_index)));
            if (m) { g_match = m; g_matchcap = PAL_MATCH_CAP; }
        }
    }
#endif

    g_pal_mode = mode;
    g_query[0] = 0;
    palette_filter();
    /* Opening a list lands on where that list already stands: with no filter
     * yet, row i is item i, so the number the mode chooses is the row. */
    at = pal_target();
    g_sel = at ? *at : 0;
    if (g_sel >= g_nmatch) g_sel = g_nmatch ? g_nmatch - 1 : 0;
    pal_preview();
}

static void palette_run(int mode)
{
    int n = 0, settled = 0;
    int was_theme = g_theme, was_lang = g_lang, was_at = g_buf.caret;
#if !DOS16
    int was_font = g_font;
#endif

    pal_enter(mode);

    /* settled: 0 still running, 1 Enter, -1 Escape. */
    while (!settled) {
        int c;

        draw();
        palette_draw();
        c = scr_key();

#if defined(__CC65__)
        if (c == CH_ENTER) { settled = 1; }
        else if (c == CH_STOP) { settled = -1; }
        else if (c == CH_DEL) {
            if (n > 0) g_query[--n] = 0;
            palette_filter();
            pal_preview();
        }
        else if (c == CH_CURS_UP)   { if (g_sel > 0) g_sel--; pal_preview(); }
        else if (c == CH_CURS_DOWN) {
            if (g_sel + 1 < g_nmatch) g_sel++;
            pal_preview();
        }
#else
        if (c == 13) { settled = 1; }
        else if (c == 27) { settled = -1; }
        else if (c == 8) {
            if (n > 0) g_query[--n] = 0;
            palette_filter();
            pal_preview();
        }
        else if (c == 0 || c == 224) {
            c = scr_key();
            if (c == 72 && g_sel > 0) g_sel--;
            if (c == 80 && g_sel + 1 < g_nmatch) g_sel++;
            pal_preview();
        }
#endif
        /* A mode that reads a number takes nothing else: a letter typed into
         * it would filter a list that is not there and leave the query saying
         * something no line number can be. */
        else if ((g_pal_mode != PM_GOTO || (c >= '0' && c <= '9')) &&
                 visible((char)c) && n < (int)sizeof(g_query) - 1) {
            g_query[n++] = (char)c;
            g_query[n] = 0;
            g_sel = 0;
            palette_filter();
            pal_preview();
        }

        /* Enter in the command list on a row that opens a mode is not a
         * command being run: it is this overlay becoming that list, with what
         * Escape has to put back still the state it was opened with. */
        if (settled > 0 && g_pal_mode == PM_CMDS) {
            int cmd  = g_nmatch ? (int)kPalette[g_match[g_sel]].cmd : C_NONE;
            int next = pal_mode_for(cmd);
            if (next >= 0) {
                pal_enter(next);
                n = 0;
                settled = 0;
            }
        }
    }

    if (settled < 0) {
        /* Every preview any mode can have made, put back.  Restoring what was
         * never changed costs nothing and is a branch fewer to get wrong. */
        g_theme = was_theme;
        g_lang  = was_lang;
#if !DOS16
        g_font  = was_font;
        font_apply();
#endif
        note_buffer_caret_set(&g_buf, was_at, 0);
        theme_apply();
        follow_caret();
        return;
    }

    if (g_pal_mode == PM_CMDS) {
        if (g_nmatch) do_command((int)kPalette[g_match[g_sel]].cmd);
    }
    /* A list mode has already applied itself, so committing is declining to
     * undo it -- and saying what was chosen, which is the row that was under
     * the selection and so needs no second name for it. */
    else if (g_nmatch && g_pal_mode != PM_GOTO)
        message(pal_label(g_match[g_sel]));
}

/* ==========================================================================
 * The editor
 * ========================================================================== */

int main(int argc, char **argv)
{
#if CAT16
    /* Before anything can filter a list.  cat_self is this executable, which
     * is also where the theme catalogue is: argv[0] is a full path on every
     * DOS since 3.0, and if it somehow is not, the three built-ins below are
     * still a theme list. */
    g_match    = (pal_index __far *)kMatchNear;
    g_matchcap = (int)(sizeof(kMatchNear) / sizeof(kMatchNear[0]));
    cat_self   = (argc > 0) ? argv[0] : 0;
#endif

    note_buffer_init(&g_buf, g_text, TEXT_CAP, g_undo, UNDO_RECS,
                     g_utext, UNDO_TEXT, g_lines, LINE_CAP);
    syntax_setup();
#if NOTE_THEME_CATALOGUE
    /* After syntax_setup, which resets the arena the theme names go into. */
    themes_setup(argc > 0 ? argv[0] : "");
#else
    g_theme = THEME_DEFAULT;
    theme_apply();
#endif
    screen_open();
    clrscr();

    if (argc > 1 && !load(argv[1])) message("could not open that file");

    while (g_running) {
        int ch, k, top0, left0, lines0;

        /* A screen is a thousand cells and a pass of the lexer; a caret move
         * is two cells.  Repainting the first when only the second happened
         * is what made the C64 build feel like a slideshow, so the screen is
         * repainted only when something asked for it. */
        if (g_damage == DMG_ALL)       draw();
        else if (g_damage == DMG_LINE) draw_line_only();
        else                           draw_status();
        g_damage = DMG_NONE;
        draw_caret();

        k = read_key(&ch);
        top0   = g_top;
        left0  = g_left;
        lines0 = note_buffer_lines(&g_buf);

        /* Anything but Up and Down abandons the column those two remember. */
        if (k != K_UP && k != K_DOWN &&
            !(k == K_CMD && (g_key_cmd == C_PGUP || g_key_cmd == C_PGDN)))
            g_goal = -1;

        switch (k) {
        case K_CHAR: {
            nchar c = (nchar)ch;
            note_buffer_insert(&g_buf, &c, 1);
            g_damage = plain_edit((char)ch) ? DMG_LINE : DMG_ALL;
            break;
        }
        case K_ENTER: {
            nchar c = (nchar)'\n';
            note_buffer_insert(&g_buf, &c, 1);
            g_damage = DMG_ALL;
            break;
        }
        case K_BACK: {
            /* What is about to go decides the cost, so it is read first. */
            int gone = g_buf.caret > 0
                     ? (int)note_buffer_at(&g_buf, g_buf.caret - 1) : '\n';
            note_buffer_delete(&g_buf, -1);
            g_damage = plain_edit((char)gone) ? DMG_LINE : DMG_ALL;
            break;
        }
        case K_DEL: {
            int gone = (int)note_buffer_at(&g_buf, g_buf.caret);
            note_buffer_delete(&g_buf, 1);
            g_damage = plain_edit((char)gone) ? DMG_LINE : DMG_ALL;
            break;
        }

        case K_LEFT:  note_buffer_caret_set(&g_buf, g_buf.caret - 1, 0); break;
        case K_RIGHT: note_buffer_caret_set(&g_buf, g_buf.caret + 1, 0); break;
        case K_UP:    move_line(-1); break;
        case K_DOWN:  move_line(1);  break;
        case K_PGUP:  move_line(-TEXT_ROWS); break;
        case K_PGDN:  move_line(TEXT_ROWS);  break;

        case K_HOME:  move_line_edge(0); break;
        case K_END:   move_line_edge(1); break;
        case K_WORDL: move_word(-1); break;
        case K_WORDR: move_word(1);  break;
        case K_DOCTOP: note_buffer_caret_set(&g_buf, 0, 0); break;
        case K_DOCEND: note_buffer_caret_set(&g_buf,
                                             note_buffer_len(&g_buf), 0); break;

        case K_PALETTE: palette_run(PM_CMDS);  g_damage = DMG_ALL; break;
        case K_CMD:     do_command(g_key_cmd); g_damage = DMG_ALL; break;
        }

        follow_caret();

        /* Scrolling moves every line under the cursor, and a line appearing or
         * going renumbers the gutter and shifts everything below it.  Either
         * way the cheap path no longer applies. */
        if (g_top != top0 || g_left != left0 ||
            note_buffer_lines(&g_buf) != lines0) g_damage = DMG_ALL;
    }

    screen_close();
    return 0;
}
