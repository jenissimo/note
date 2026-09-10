/* THROWAWAY PROOF -- not part of note.  Delete tools\stubdemo\ at will.
 *
 * A crude "editor-shaped" 16-bit real-mode program, written only to answer
 * the question "what does an editor cost in a small-model DOS build?".  It is
 * a line buffer, a file loader and saver, a text-mode screen painter that
 * writes straight into B800:0000, and an int 16h key loop -- the same four
 * pieces note has, at about a three-hundredth of the fidelity.  Nobody should
 * use this; it exists to be measured with a Watcom map file.
 *
 *   arrows / PgUp / PgDn  move        printable keys  insert
 *   Backspace  delete left           Enter  split line
 *   Ctrl+S  save                     Esc    quit
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <conio.h>
#include <dos.h>

#define MAXLINES 512
#define MAXCOL   200
#define ROWS     25
#define COLS     80

static char  *line[MAXLINES];
static int    len[MAXLINES];
static int    nlines;
static int    cy, cx, top;
static int    dirty;
static char   fname[80];
static char   status[COLS + 1];

static unsigned char far *vram = (unsigned char far *)0xB8000000L;

/* ---- screen ---------------------------------------------------------- */

static void put(int row, int col, char c, unsigned char attr)
{
    unsigned off = (unsigned)((row * COLS + col) << 1);
    vram[off] = (unsigned char)c;
    vram[off + 1] = attr;
}

static void clearrow(int row, unsigned char attr)
{
    int i;
    for (i = 0; i < COLS; i++) put(row, i, ' ', attr);
}

static void setcursor(int row, int col)
{
    union REGS r;
    r.h.ah = 0x02;
    r.h.bh = 0;
    r.h.dh = (unsigned char)row;
    r.h.dl = (unsigned char)col;
    int86(0x10, &r, &r);
}

static void setmode(void)
{
    union REGS r;
    r.h.ah = 0x00;
    r.h.al = 0x03;          /* 80x25 colour text */
    int86(0x10, &r, &r);
}

static void draw(void)
{
    int row, i, n;
    char *p;

    for (row = 0; row < ROWS - 1; row++) {
        n = top + row;
        clearrow(row, 0x07);
        if (n < nlines) {
            p = line[n];
            for (i = 0; i < len[n] && i < COLS; i++)
                put(row, i, p[i], 0x07);
        } else {
            put(row, 0, '~', 0x08);
        }
    }

    sprintf(status, " %s%s  %d:%d  %d lines ",
            fname[0] ? fname : "[no name]", dirty ? " *" : "",
            cy + 1, cx + 1, nlines);
    clearrow(ROWS - 1, 0x70);
    for (i = 0; status[i] && i < COLS; i++)
        put(ROWS - 1, i, status[i], 0x70);

    setcursor(cy - top, cx < COLS ? cx : COLS - 1);
}

/* ---- buffer ---------------------------------------------------------- */

static int newline_at(int at)
{
    int i;
    if (nlines >= MAXLINES) return 0;
    for (i = nlines; i > at; i--) {
        line[i] = line[i - 1];
        len[i] = len[i - 1];
    }
    line[at] = (char *)malloc(MAXCOL);
    if (!line[at]) return 0;
    len[at] = 0;
    nlines++;
    return 1;
}

static void killline(int at)
{
    int i;
    free(line[at]);
    for (i = at; i < nlines - 1; i++) {
        line[i] = line[i + 1];
        len[i] = len[i + 1];
    }
    nlines--;
}

static void insert(char c)
{
    int i;
    if (len[cy] >= MAXCOL - 1) return;
    for (i = len[cy]; i > cx; i--) line[cy][i] = line[cy][i - 1];
    line[cy][cx] = c;
    len[cy]++;
    cx++;
    dirty = 1;
}

static void backspace(void)
{
    int i, old;
    if (cx > 0) {
        for (i = cx - 1; i < len[cy] - 1; i++) line[cy][i] = line[cy][i + 1];
        len[cy]--;
        cx--;
    } else if (cy > 0) {
        old = len[cy - 1];
        if (old + len[cy] < MAXCOL - 1) {
            memcpy(line[cy - 1] + old, line[cy], (unsigned)len[cy]);
            len[cy - 1] += len[cy];
            killline(cy);
        }
        cy--;
        cx = old;
    }
    dirty = 1;
}

static void splitline(void)
{
    int rest;
    if (!newline_at(cy + 1)) return;
    rest = len[cy] - cx;
    memcpy(line[cy + 1], line[cy] + cx, (unsigned)rest);
    len[cy + 1] = rest;
    len[cy] = cx;
    cy++;
    cx = 0;
    dirty = 1;
}

/* ---- files ----------------------------------------------------------- */

static void load(const char *path)
{
    FILE *f;
    char buf[MAXCOL];
    int n;

    strcpy(fname, path);
    f = fopen(path, "r");
    if (!f) { newline_at(0); return; }
    while (nlines < MAXLINES && fgets(buf, MAXCOL, f)) {
        n = (int)strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
        if (!newline_at(nlines)) break;
        memcpy(line[nlines - 1], buf, (unsigned)n);
        len[nlines - 1] = n;
    }
    fclose(f);
    if (!nlines) newline_at(0);
}

static void save(void)
{
    FILE *f;
    int i;
    if (!fname[0]) return;
    f = fopen(fname, "w");
    if (!f) return;
    for (i = 0; i < nlines; i++) {
        fwrite(line[i], 1, (unsigned)len[i], f);
        fputc('\n', f);
    }
    fclose(f);
    dirty = 0;
}

/* ---- keys ------------------------------------------------------------ */

static void clampx(void)
{
    if (cx > len[cy]) cx = len[cy];
}

static void scroll(void)
{
    if (cy < top) top = cy;
    if (cy >= top + ROWS - 1) top = cy - (ROWS - 2);
    if (top < 0) top = 0;
}

int main(int argc, char **argv)
{
    int k;

    if (argc > 1) load(argv[1]);
    else newline_at(0);

    setmode();

    for (;;) {
        draw();
        k = getch();
        if (k == 0 || k == 0xE0) {
            switch (getch()) {
            case 72: if (cy > 0) { cy--; clampx(); } break;          /* up   */
            case 80: if (cy < nlines - 1) { cy++; clampx(); } break; /* down */
            case 75: if (cx > 0) cx--; break;                        /* left */
            case 77: if (cx < len[cy]) cx++; break;                  /* right*/
            case 71: cx = 0; break;                                  /* home */
            case 79: cx = len[cy]; break;                            /* end  */
            case 73: cy -= ROWS - 2; if (cy < 0) cy = 0; clampx(); break;
            case 81: cy += ROWS - 2; if (cy > nlines - 1) cy = nlines - 1;
                     clampx(); break;
            case 83:                                                 /* del  */
                if (cx < len[cy]) { cx++; backspace(); }
                else if (cy < nlines - 1) { cy++; cx = 0; backspace(); }
                break;
            }
        } else if (k == 27) {
            break;
        } else if (k == 19) {       /* Ctrl+S */
            save();
        } else if (k == 8) {
            backspace();
        } else if (k == 13) {
            splitline();
        } else if (k == 9) {
            insert(' '); insert(' '); insert(' '); insert(' ');
        } else if (k >= 32 && k < 127) {
            insert((char)k);
        }
        scroll();
    }

    setmode();
    printf("ted16: %d lines%s\n", nlines, dirty ? " (unsaved)" : "");
    return 0;
}
