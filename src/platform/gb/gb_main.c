/* gb_main.c -- note for the Game Boy.
 *
 * The design note behind this lives outside the repo; what follows is the
 * part of it that runs.
 *
 * The screen is the interesting half.  A Game Boy shows 20x18 tiles of 8x8,
 * which is 20 columns of text, which is not an editor.  So the tilemap here is
 * not a font: every cell of the text area owns a tile of its own and the tile's
 * pixels are rewritten as the text changes.  With a 4-pixel font two characters
 * share a tile and the screen becomes 40 columns wide.
 *
 * That costs one tile per cell out of the 256 the background can address, which
 * is what fixes the text area at 8 rows: 8 x 20 = 160 tiles, leaving 96 for the
 * keyboard's own alphabet.  Eight rows also divides 32, the height of the
 * hardware tilemap, so the map can be filled once with a repeating pattern and
 * scrolling becomes a single write to SCY -- no redraw, no tearing, and only
 * the one row that comes into view has to be painted.
 *
 * The keyboard sits on the window layer, which does not scroll, so it stays put
 * while the text moves underneath it.
 *
 * Owning every cell's tile pays for two more things than 40 columns.  A cell is
 * composed from the buffer each time it is drawn, so it can be composed in any
 * of the DMG's four shades -- which is a syntax highlighter -- and it can be
 * composed inverted, which is a block cursor of exactly one character.  Neither
 * costs a byte of VRAM or a sprite: the tile was going to be written anyway.
 */

#include <gb/gb.h>

/* The core's own gap buffer, unchanged: it allocates nothing, holds no OS
 * headers and never assumes an int is wider than 16 bits, which on this
 * machine stops being a style rule and starts being the specification. */
#define NOTE_ARENA_CHARS 16
#define NOTE_MAX_LANGS   1
#include "../../core/note_config.h"
#include "../../core/note_buffer.h"

#include "gb_font.h"

/* ==========================================================================
 * Layout
 * ========================================================================== */

#define TILE_COLS   20            /* tiles across the screen                */
#define TEXT_ROWS    8            /* rows of text, and a divisor of 32      */
#define TEXT_COLS   40            /* two characters to a tile               */

#define TILE_TEXT0   0            /* 160 tiles: row-major, 8 rows of 20     */
#define TILE_A      160           /* 26 letters                             */
#define TILE_DIGIT  186           /* 10 digits                              */
#define TILE_SYM    196           /* the symbols in kSymbols, in order      */
#define TILE_BLANK  213
/* The status band and the selected key are drawn in reverse video, and a
 * reversed glyph is a different tile from the upright one.  Rather than carry
 * a second alphabet -- 53 tiles for a bar that shows twenty characters and a
 * cap that shows eight -- these are rewritten as their content changes, the
 * same trick the text area is built on. */
#define TILE_STAT   214           /* 20: one per column of the status band  */
#define TILE_SEL    234           /* 8: the inverted cap under the cursor   */
#define TILE_HINT   242           /* 13: the legend on the bottom band      */
#define TILE_SOLID  255           /* a reversed space: the band's own fill  */

#define WIN_Y       64            /* the window starts below the 8 text rows */

/* Screen rows inside the window.  The keyboard is a panel: a band above it,
 * a band below it, and the keys spaced out in between.  Loose letters under
 * loose letters was the problem -- the eye had nothing to tell it where the
 * document stopped and the machinery started. */
#define WIN_STATUS   0
#define WIN_KEYS     2
#define WIN_HINT     9

#define HINT_X       3
#define HINT_N      13
static const char kHint[HINT_N + 1] = "A PUT  B DEL ";

static const char kSymbols[] = "=+-*/()<>,;:\".$#_";
#define NSYMBOLS (sizeof(kSymbols) - 1)

/* ==========================================================================
 * Shades
 *
 * The DMG has four, and one of them is the paper, so a highlighter here gets
 * three levels and no colours.  Three is enough for a hierarchy as long as it
 * is a hierarchy and not a palette: light grey recedes, black advances, and
 * ordinary text sits between them.  Comments and the line number are the parts
 * you skim past, keywords are the parts you look for.
 * ========================================================================== */

#define SH_DIM    1               /* light grey: comments, line numbers     */
#define SH_TEXT   2               /* dark grey: names, operators, numbers   */
#define SH_KEY    3               /* black: keywords and quoted text        */
#define SH_INV    4               /* a flag: swap ink and paper             */

/* ==========================================================================
 * The document
 * ========================================================================== */

#define TEXT_CAP   2048
#define UNDO_RECS    16
#define UNDO_TEXT   128
#define LINE_CAP    128

static nchar     g_text[TEXT_CAP];
static note_edit g_undo[UNDO_RECS];
static nchar     g_utext[UNDO_TEXT];
static int       g_lines[LINE_CAP];
static note_buffer g_buf;

static int g_top;                 /* first visible line                     */

/* note_buffer asks the rest of the core for exactly one thing. */
int n_len(const nchar *s)
{
    int i = 0;
    if (!s) return 0;
    while (s[i]) i++;
    return i;
}

/* ==========================================================================
 * One line, and what colour each character of it is
 *
 * The lexer runs over a line at a time and writes a shade per column.  It is
 * not the core's note_syntax: that one wants a language registry, a span list
 * and a token enum, which is a kilobyte of ROM to say "REM starts a comment".
 * BASIC is small enough to lex in place.
 * ========================================================================== */

static char          g_lbuf[TEXT_COLS];
static unsigned char g_shade[TEXT_COLS];

/* Fourteen, not sixteen: two columns of seven leave the bottom row of the
 * panel for the legend band, and LET and STEP are the two a BASIC written on
 * a d-pad misses least. */
static const char *const kWords[] = {
    "PRINT", "INPUT", "IF", "THEN", "GOTO", "GOSUB",
    "RETURN", "FOR", "TO", "NEXT", "END",
    "REM", "CLS", "PAUSE"
};
#define NWORDS (sizeof(kWords) / sizeof(kWords[0]))

/* The rest of what the interpreter understands.  These have no key of their
 * own -- the keyboard has sixteen slots and these lost -- but they are still
 * keywords when they turn up in the text. */
static const char *const kMoreWords[] = {
    "LET", "STEP", "STOP", "AND", "OR", "NOT", "MOD", "RND", "ABS", "SGN", "JOY"
};
#define NMORE (sizeof(kMoreWords) / sizeof(kMoreWords[0]))

static char upper_of(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static unsigned char word_char(char c)
{
    return (unsigned char)((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '$');
}

/* Reads one line of the document into g_lbuf, space-padded.  Lines wider than
 * the screen are simply cut: there is no horizontal scroll, and a Game Boy
 * BASIC line that needs one has a bigger problem than its colours. */
static void fetch_line(int line)
{
    int start = 0, len = 0;
    unsigned char i;

    if (line >= 0 && line < note_buffer_lines(&g_buf)) {
        start = note_buffer_line_start(&g_buf, line);
        len   = note_buffer_line_len(&g_buf, line);
    }
    if (len > TEXT_COLS) len = TEXT_COLS;

    for (i = 0; i < (unsigned char)len; i++)
        g_lbuf[i] = (char)note_buffer_at(&g_buf, start + i);
    for (; i < TEXT_COLS; i++) g_lbuf[i] = ' ';
}

/* Whole-word compare against a keyword, case-folded, anchored at `at`. */
static unsigned char word_is(const char *w, unsigned char at, unsigned char n)
{
    unsigned char i;
    for (i = 0; i < n; i++)
        if (upper_of(g_lbuf[at + i]) != w[i]) return 0;
    return (unsigned char)(w[n] == 0);
}

static unsigned char keyword_at(unsigned char at, unsigned char n)
{
    unsigned char i;
    for (i = 0; i < NWORDS; i++) if (word_is(kWords[i], at, n)) return 1;
    for (i = 0; i < NMORE;  i++) if (word_is(kMoreWords[i], at, n)) return 1;
    return 0;
}

static void classify(void)
{
    unsigned char i = 0, j;
    char c;

    for (j = 0; j < TEXT_COLS; j++) g_shade[j] = SH_TEXT;

    /* The line number is punctuation, not content: it is how you refer to the
     * line, and you already know which line you are on. */
    while (i < TEXT_COLS && g_lbuf[i] == ' ') i++;
    if (i < TEXT_COLS && g_lbuf[i] >= '0' && g_lbuf[i] <= '9')
        while (i < TEXT_COLS && g_lbuf[i] >= '0' && g_lbuf[i] <= '9')
            g_shade[i++] = SH_DIM;

    while (i < TEXT_COLS) {
        c = g_lbuf[i];

        if (c == '"') {
            g_shade[i++] = SH_KEY;
            while (i < TEXT_COLS) {
                g_shade[i] = SH_KEY;
                if (g_lbuf[i++] == '"') break;
            }
            continue;
        }

        if (word_char(c)) {
            j = i;
            while (j < TEXT_COLS && word_char(g_lbuf[j])) j++;

            if (word_is("REM", i, (unsigned char)(j - i))) {
                while (i < TEXT_COLS) g_shade[i++] = SH_DIM;
                return;
            }
            if (keyword_at(i, (unsigned char)(j - i)))
                while (i < j) g_shade[i++] = SH_KEY;
            else
                i = j;
            continue;
        }

        i++;
    }
}

/* ==========================================================================
 * Drawing the text
 *
 * A glyph is three pixels of ink in the high nibble of each of eight bytes.
 * Both bitplanes are written now rather than one: the shade of a pixel is the
 * pair of bits above it, so plane 0 alone can only ever say "colour 1".  The
 * write is the same sixteen bytes either way -- set_bkg_data moves a whole
 * tile -- so three shades and a reverse-video cell are free.
 * ========================================================================== */

static unsigned char g_cell[16];

static unsigned char glyph_index(char c)
{
    if (c >= 'a' && c <= 'z') c -= 32;          /* one case in the table */
    if (c < FONT_FIRST || c > FONT_LAST) c = '?';
    return (unsigned char)(c - FONT_FIRST);
}

/* Composes the tile for one cell and hands it to VRAM.  `row` is a buffer row
 * 0..7, not a line number: which line it is showing is the caller's business.
 * Each half carries its own shade, and SH_INV on a half fills it and punches
 * the glyph out in paper -- which is the cursor. */
static void draw_cell(unsigned char row, unsigned char col,
                      char l, unsigned char cl, char r, unsigned char cr)
{
    const unsigned char *gl = gb_font[glyph_index(l)];
    const unsigned char *gr = gb_font[glyph_index(r)];
    unsigned char i, a, b, p0, p1;

    for (i = 0; i < 8; i++) {
        a = (unsigned char)(gl[i] & 0xF0);
        b = (unsigned char)(gr[i] >> 4);

        if (cl & SH_INV) {
            p0 = (unsigned char)(~a & 0xF0);
            p1 = p0;
        } else {
            p0 = (unsigned char)((cl & 1) ? a : 0);
            p1 = (unsigned char)((cl & 2) ? a : 0);
        }

        if (cr & SH_INV) {
            p0 |= (unsigned char)(~b & 0x0F);
            p1 |= (unsigned char)(~b & 0x0F);
        } else {
            if (cr & 1) p0 |= b;
            if (cr & 2) p1 |= b;
        }

        g_cell[i * 2]     = p0;
        g_cell[i * 2 + 1] = p1;
    }
    set_bkg_data((unsigned char)(TILE_TEXT0 + row * TILE_COLS + col), 1, g_cell);
}

/* One line of the document into one buffer row, from `first` cell rightwards.
 *
 * Starting part-way along is not a micro-optimisation: typing at the end of a
 * line has to touch one tile, and touching one tile is what keeps a keystroke
 * inside a single VBlank.  Redrawing all twenty every time would not fit, and
 * the writes that did not fit would be dropped rather than delayed. */
static void draw_line_from(int line, unsigned char first)
{
    unsigned char row = (unsigned char)(line & (TEXT_ROWS - 1));
    unsigned char col, a, b;

    fetch_line(line);
    classify();

    for (col = first; col < TILE_COLS; col++) {
        a = (unsigned char)(col * 2);
        b = (unsigned char)(a + 1);
        draw_cell(row, col, g_lbuf[a], g_shade[a], g_lbuf[b], g_shade[b]);
    }
}

static void draw_line(int line)
{
    draw_line_from(line, 0);
}

static void draw_all_lines(void)
{
    unsigned char r;
    for (r = 0; r < TEXT_ROWS; r++) draw_line(g_top + r);
}

/* ==========================================================================
 * The cursor
 *
 * A bar between two characters is a mouse-and-proportional-font idea.  On a
 * fixed grid the honest cursor is the cell itself, turned inside out, and here
 * it costs nothing: the cell's tile is composed from the buffer on every
 * redraw anyway, so inverting one half of one tile is the same write it was
 * going to do.  The sprite the caret used to be is gone, and so is the
 * question of what happens when it lands on a character.
 * ========================================================================== */

static int           g_cur_line = -1;   /* where the inverted cell is drawn */
static unsigned char g_cur_col;         /* 0..TEXT_COLS-1                   */
static unsigned char g_cur_on;

/* Repaints one cell of a line, optionally inverting the half `col` names. */
static void paint_cell(int line, unsigned char col, unsigned char inv)
{
    unsigned char row = (unsigned char)(line & (TEXT_ROWS - 1));
    unsigned char cell = (unsigned char)(col >> 1);
    unsigned char a = (unsigned char)(cell * 2), b = (unsigned char)(a + 1);
    unsigned char ca, cb;

    fetch_line(line);
    classify();

    ca = g_shade[a];
    cb = g_shade[b];
    if (inv) {
        if (col & 1) cb = SH_INV;
        else         ca = SH_INV;
    }
    draw_cell(row, cell, g_lbuf[a], ca, g_lbuf[b], cb);
}

static void caret_hide(void)
{
    if (!g_cur_on) return;
    g_cur_on = 0;
    if (g_cur_line >= g_top && g_cur_line < g_top + TEXT_ROWS)
        paint_cell(g_cur_line, g_cur_col, 0);
}

static void caret_show(void)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int col  = g_buf.caret - note_buffer_line_start(&g_buf, line);

    if (col > TEXT_COLS - 1) col = TEXT_COLS - 1;
    if (line < g_top || line >= g_top + TEXT_ROWS) return;
    if (g_cur_on && g_cur_line == line && g_cur_col == (unsigned char)col) return;

    caret_hide();
    g_cur_line = line;
    g_cur_col  = (unsigned char)col;
    g_cur_on   = 1;
    paint_cell(line, g_cur_col, 1);
}

/* ==========================================================================
 * The shared alphabet the window is drawn with
 *
 * The same 3x5 glyphs, doubled sideways into 6x5, because on a keyboard the
 * thing that matters is being able to read a key from across the room rather
 * than fitting forty of them on a line.  Six pixels in an eight-pixel tile
 * leaves a two-pixel gutter, which is what makes a run of reversed tiles read
 * as a bar with letters knocked out of it rather than as a smear.
 * ========================================================================== */

/* Widens one glyph into g_cell's high bits and returns it row by row through
 * the caller's loop; both makers below share it. */
static unsigned char wide_row(const unsigned char *g, unsigned char i)
{
    unsigned char row = g[i], wide = 0, j;
    for (j = 0; j < 3; j++)
        if (row & (0x80 >> j)) wide |= (unsigned char)(0xC0 >> (j * 2));
    return wide;
}

static void make_wide_tile(char c)
{
    const unsigned char *g = gb_font[glyph_index(c)];
    unsigned char i, wide;

    for (i = 0; i < 8; i++) {
        wide = wide_row(g, i);
        /* Both planes, so the ink is colour 3 -- black under the palette
         * below.  Plane 0 alone would make the whole keyboard light grey. */
        g_cell[i * 2]     = wide;
        g_cell[i * 2 + 1] = wide;
    }
}

static void make_wide_tile_inv(char c)
{
    const unsigned char *g = gb_font[glyph_index(c)];
    unsigned char i, wide;

    for (i = 0; i < 8; i++) {
        wide = (unsigned char)~wide_row(g, i);
        g_cell[i * 2]     = wide;
        g_cell[i * 2 + 1] = wide;
    }
}

static unsigned char tile_for(char c)
{
    unsigned char i;
    if (c >= 'a' && c <= 'z') c -= 32;
    if (c >= 'A' && c <= 'Z') return (unsigned char)(TILE_A + (c - 'A'));
    if (c >= '0' && c <= '9') return (unsigned char)(TILE_DIGIT + (c - '0'));
    for (i = 0; i < NSYMBOLS; i++)
        if (kSymbols[i] == c) return (unsigned char)(TILE_SYM + i);
    return TILE_BLANK;
}

static void build_alphabet(void)
{
    unsigned char i;
    char c;

    for (c = 'A'; c <= 'Z'; c++) { make_wide_tile(c); set_bkg_data(tile_for(c), 1, g_cell); }
    for (c = '0'; c <= '9'; c++) { make_wide_tile(c); set_bkg_data(tile_for(c), 1, g_cell); }
    for (i = 0; i < NSYMBOLS; i++) {
        make_wide_tile(kSymbols[i]);
        set_bkg_data((unsigned char)(TILE_SYM + i), 1, g_cell);
    }
    make_wide_tile(' ');
    set_bkg_data(TILE_BLANK, 1, g_cell);

    /* A reversed space is a solid tile, so the bands need no artwork of their
     * own: the same maker that draws their letters draws their fill. */
    make_wide_tile_inv(' ');
    set_bkg_data(TILE_SOLID, 1, g_cell);

    for (i = 0; i < HINT_N; i++) {
        make_wide_tile_inv(kHint[i]);
        set_bkg_data((unsigned char)(TILE_HINT + i), 1, g_cell);
    }
}

/* Writes a string into the window, in shared tiles. */
static unsigned char g_row[TILE_COLS];

static void win_text(unsigned char x, unsigned char y, const char *s)
{
    unsigned char n = 0;
    while (s[n] && x + n < TILE_COLS) { g_row[n] = tile_for(s[n]); n++; }
    if (n) set_win_tiles(x, y, n, 1, g_row);
}

static void win_clear_row(unsigned char y)
{
    unsigned char i;
    for (i = 0; i < TILE_COLS; i++) g_row[i] = TILE_BLANK;
    set_win_tiles(0, y, TILE_COLS, 1, g_row);
}

/* ==========================================================================
 * The keyboard
 *
 * Three layers, because a flat grid of everything is four presses deep in
 * every direction.  The letter layer is ordered by how often a letter turns up
 * in BASIC rather than alphabetically: on a grid you navigate by distance, and
 * alphabetical order is only useful to someone who already knows where the
 * alphabet is going.
 * ========================================================================== */

#define KEY_COLS   10             /* keys across; each is two tiles wide     */
#define KEY_ROWS    4
#define SYM_COLS    5             /* symbols are fewer and want bigger gaps  */
#define SYM_ROWS    4

enum { LAYER_LETTERS = 0, LAYER_SYMBOLS, LAYER_WORDS, LAYER_COUNT };

/* 40 slots: the letters by frequency in BASIC source, then the digits. */
static const char kLetters[KEY_COLS * KEY_ROWS + 1] =
    "ETAOINSRHL"
    "DCUMFPGWYB"
    "VKXJQZ 0123456789";

#define WORD_COLS 2
#define WORD_ROWS (NWORDS / WORD_COLS)

static unsigned char g_layer;
static unsigned char g_sel;               /* index into the current layer   */
static unsigned char g_shown = 0xFF;      /* the slot the cap is drawn on   */

static unsigned char layer_cols(void)
{
    if (g_layer == LAYER_SYMBOLS) return SYM_COLS;
    if (g_layer == LAYER_WORDS)   return WORD_COLS;
    return KEY_COLS;
}

static unsigned char layer_rows(void)
{
    if (g_layer == LAYER_SYMBOLS) return SYM_ROWS;
    if (g_layer == LAYER_WORDS)   return WORD_ROWS;
    return KEY_ROWS;
}

/* Where a key lands on the window.  Letters and symbols are on a double-height
 * pitch: four rows of keys then fill the panel, and a key with air around it
 * is easier to aim a d-pad at than one packed against its neighbours.  Words
 * are already two to a row and wide, so they stay on a single pitch. */
static unsigned char key_y(unsigned char r)
{
    return (unsigned char)(WIN_KEYS + ((g_layer == LAYER_WORDS) ? r : r * 2));
}

static unsigned char key_x(unsigned char c)
{
    if (g_layer == LAYER_WORDS)   return (unsigned char)(c * 10);
    if (g_layer == LAYER_SYMBOLS) return (unsigned char)(c * 4 + 1);
    return (unsigned char)(c * 2);
}

static unsigned char layer_count(void)
{
    return (unsigned char)(layer_cols() * layer_rows());
}

static void draw_keys(void)
{
    unsigned char r, c, i;
    char one[2];

    one[1] = 0;
    g_shown = 0xFF;               /* nothing on this map is reversed now */

    for (r = WIN_KEYS; r < WIN_HINT; r++) win_clear_row(r);

    if (g_layer == LAYER_WORDS) {
        for (r = 0; r < WORD_ROWS; r++)
            for (c = 0; c < WORD_COLS; c++)
                win_text((unsigned char)(key_x(c) + 1), key_y(r),
                         kWords[r * WORD_COLS + c]);
        return;
    }

    for (r = 0; r < layer_rows(); r++) {
        for (c = 0; c < layer_cols(); c++) {
            i = (unsigned char)(r * layer_cols() + c);
            one[0] = (g_layer == LAYER_LETTERS) ? kLetters[i] : kSymbols[i];
            if (g_layer == LAYER_SYMBOLS && i >= NSYMBOLS) one[0] = ' ';
            win_text(key_x(c), key_y(r), one);
        }
    }
}

/* Where a slot sits and what it says.  A word gets a space on either side, so
 * the cap around it is a cap and not a tight box on the letters. */
static char          g_kstr[10];
static unsigned char g_kx, g_ky, g_kn;

static void key_slot(unsigned char sel)
{
    unsigned char cols = layer_cols();
    unsigned char r = (unsigned char)(sel / cols);
    unsigned char c = (unsigned char)(sel % cols);
    unsigned char i, n = 0;
    const char *w;
    char ch;

    if (g_layer == LAYER_WORDS) {
        w = kWords[sel];
        g_kstr[n++] = ' ';
        for (i = 0; w[i]; i++) g_kstr[n++] = w[i];
        g_kstr[n++] = ' ';
    } else {
        ch = (g_layer == LAYER_LETTERS) ? kLetters[sel] : kSymbols[sel];
        if (g_layer == LAYER_SYMBOLS && sel >= NSYMBOLS) ch = ' ';
        g_kstr[n++] = ch;
    }
    g_kstr[n] = 0;
    g_kn = n;
    g_kx = key_x(c);
    g_ky = key_y(r);
}

/* The selection is reverse video rather than an outline sprite.  An outline
 * has to be thin enough to leave the key readable, which makes it a hairline
 * on a screen this size; a filled cap is unmissable, and it is the same idea
 * as the cursor in the text -- one visual language for "here". */
static void sel_hide(void)
{
    unsigned char i;

    if (g_shown == 0xFF) return;
    key_slot(g_shown);
    for (i = 0; i < g_kn; i++) g_row[i] = tile_for(g_kstr[i]);
    set_win_tiles(g_kx, g_ky, g_kn, 1, g_row);
    g_shown = 0xFF;
}

static void sel_show(void)
{
    unsigned char i;

    if (g_shown == g_sel) return;
    sel_hide();
    key_slot(g_sel);
    for (i = 0; i < g_kn; i++) {
        make_wide_tile_inv(g_kstr[i]);
        set_bkg_data((unsigned char)(TILE_SEL + i), 1, g_cell);
        g_row[i] = (unsigned char)(TILE_SEL + i);
    }
    set_win_tiles(g_kx, g_ky, g_kn, 1, g_row);
    g_shown = g_sel;
}

/* ==========================================================================
 * Status
 *
 * A solid reversed band, which is the divider the keyboard needed: without one
 * the keys are loose letters floating under loose letters and the eye has to
 * work out where the document stops.  It is redrawn a tile at a time and only
 * where the text changed -- twenty tile writes a frame would not fit in a
 * VBlank, and one or two always do.
 * ========================================================================== */

static const char *const kLayerName[LAYER_COUNT] = { "ABC", "SYM", "BAS" };

static char g_stat[TILE_COLS];    /* what the band is showing now */

static void draw_status(void)
{
    char s[TILE_COLS];
    char num[8];
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    int col  = g_buf.caret - note_buffer_line_start(&g_buf, line);
    unsigned char n = 0, i;
    int v, d;

    line++;
    col++;

    s[n++] = ' ';
    s[n++] = 'L';
    v = line; d = 0;
    do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 7);
    while (d) s[n++] = num[--d];

    s[n++] = ' ';
    s[n++] = 'C';
    v = col; d = 0;
    do { num[d++] = (char)('0' + v % 10); v /= 10; } while (v && d < 7);
    while (d) s[n++] = num[--d];

    if (g_buf.dirty) { s[n++] = ' '; s[n++] = '.'; }

    while (n < TILE_COLS - 4) s[n++] = ' ';
    for (i = 0; kLayerName[g_layer][i]; i++) s[n++] = kLayerName[g_layer][i];
    while (n < TILE_COLS) s[n++] = ' ';

    for (i = 0; i < TILE_COLS; i++) {
        if (s[i] == g_stat[i]) continue;
        g_stat[i] = s[i];
        make_wide_tile_inv(s[i]);
        set_bkg_data((unsigned char)(TILE_STAT + i), 1, g_cell);
    }
}

/* ==========================================================================
 * Scrolling
 * ========================================================================== */

static void place_caret(void)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);

    /* Follow the caret, a line at a time.  Scrolling is a write to SCY: the
     * tilemap is a repeating pattern of the eight buffer rows, so the hardware
     * does the moving and only the row coming into view is ever redrawn.  The
     * cursor comes off first: the row it is on may stay on screen without
     * being repainted, and a reversed cell left behind is a second cursor. */
    if (line < g_top || line >= g_top + TEXT_ROWS) {
        caret_hide();
        while (line < g_top)                  { g_top--; draw_line(g_top); }
        while (line >= g_top + TEXT_ROWS)     { g_top++; draw_line(g_top + TEXT_ROWS - 1); }
    }
    move_bkg(0, (unsigned char)((g_top & (TEXT_ROWS - 1)) * 8));
}

/* ==========================================================================
 * Input
 *
 * D-pad drives the keyboard; B held turns the same D-pad into the caret.  The
 * two most frequent things anyone does never contend for a button, and nothing
 * has to be toggled to get between them.
 * ========================================================================== */

#define REPEAT_FIRST 16           /* frames before a held direction repeats  */
#define REPEAT_THEN   6
#define REPEAT_FAST   2
#define ACCEL_AFTER  30           /* ...and when it gets impatient           */

static unsigned char g_held;      /* the direction bits held last frame      */
static unsigned char g_wait;      /* frames until the next repeat            */
static unsigned char g_reps;      /* how many have fired for this hold       */
static unsigned char g_bframes;   /* how long B has been down                */
static unsigned char g_bused;     /* ...and whether it did something already */

static unsigned char repeating(unsigned char dirs)
{
    if (dirs != g_held) {
        g_held = dirs;
        g_reps = 0;
        g_wait = REPEAT_FIRST;
        return dirs ? 1 : 0;       /* the first press always fires */
    }
    if (!dirs) return 0;
    if (g_wait) { g_wait--; return 0; }

    g_reps++;
    g_wait = (g_reps > ACCEL_AFTER) ? REPEAT_FAST : REPEAT_THEN;
    return 1;
}

static void move_sel(unsigned char dirs)
{
    unsigned char cols = layer_cols(), rows = layer_rows();
    unsigned char r = (unsigned char)(g_sel / cols);
    unsigned char c = (unsigned char)(g_sel % cols);

    /* Wrapping at every edge: on a grid this small, the far side is never
     * more than half a screen away in the other direction. */
    if (dirs & J_LEFT)  c = (unsigned char)((c + cols - 1) % cols);
    if (dirs & J_RIGHT) c = (unsigned char)((c + 1) % cols);
    if (dirs & J_UP)    r = (unsigned char)((r + rows - 1) % rows);
    if (dirs & J_DOWN)  r = (unsigned char)((r + 1) % rows);

    g_sel = (unsigned char)(r * cols + c);
    if (g_sel >= layer_count()) g_sel = 0;
    sel_show();
}

static void move_caret(unsigned char dirs)
{
    int at = g_buf.caret;
    int line, col;

    if (dirs & J_LEFT)  at--;
    if (dirs & J_RIGHT) at++;

    if (dirs & (J_UP | J_DOWN)) {
        line = note_buffer_line_at(&g_buf, at);
        col  = at - note_buffer_line_start(&g_buf, line);
        line += (dirs & J_UP) ? -1 : 1;
        if (line < 0) line = 0;
        if (line >= note_buffer_lines(&g_buf)) line = note_buffer_lines(&g_buf) - 1;
        at = note_buffer_line_start(&g_buf, line);
        if (col > note_buffer_line_len(&g_buf, line))
            col = note_buffer_line_len(&g_buf, line);
        at += col;
    }

    if (at < 0) at = 0;
    if (at > note_buffer_len(&g_buf)) at = note_buffer_len(&g_buf);
    note_buffer_caret_set(&g_buf, at, 0);
}

/* A whole-screen repaint is 160 tiles, which is far more than a VBlank will
 * carry, so it is done with the LCD off -- one dark frame, and every write
 * lands.  Spreading it over frames instead would show the text tearing into
 * place, which is worse to look at and much easier to get wrong. */
static void redraw_visible(void)
{
    wait_vbl_done();
    DISPLAY_OFF;
    draw_all_lines();
    DISPLAY_ON;
}

static unsigned char has_newline(const char *s)
{
    while (*s) if (*s++ == '\n') return 1;
    return 0;
}

/* Everything that changes the text goes through here, so the choice between
 * the cheap repaint and the expensive one is made in one place.  Inserting a
 * character shifts the rest of its line and nothing else; inserting a line
 * break shifts every line below it, and that is the only case that costs a
 * full screen. */
static void edit_insert(const char *s)
{
    int line = note_buffer_line_at(&g_buf, g_buf.caret);
    unsigned char cell =
        (unsigned char)((g_buf.caret - note_buffer_line_start(&g_buf, line)) / 2);

    caret_hide();
    if (!note_buffer_insert(&g_buf, (const nchar *)s, -1)) return;

    if (has_newline(s)) redraw_visible();
    else if (line >= g_top && line < g_top + TEXT_ROWS) draw_line_from(line, cell);
}

static void edit_backspace(void)
{
    int at = g_buf.caret;
    int line, structural;
    unsigned char cell;

    if (at <= 0) return;
    structural = (note_buffer_at(&g_buf, at - 1) == '\n');

    line = note_buffer_line_at(&g_buf, at);
    cell = (unsigned char)((at - note_buffer_line_start(&g_buf, line)) / 2);
    if (cell) cell--;

    caret_hide();
    note_buffer_delete(&g_buf, -1);

    if (structural) redraw_visible();
    else if (line >= g_top && line < g_top + TEXT_ROWS) draw_line_from(line, cell);
}

static void do_key(void)
{
    char one[2];

    if (g_layer == LAYER_WORDS) {
        /* A keyword is one press, not five: the thing that made a rubber-keyed
         * Sinclair bearable, for the same reason it is bearable here. */
        edit_insert(kWords[g_sel]);
        edit_insert(" ");
        return;
    }
    one[0] = (g_layer == LAYER_LETTERS) ? kLetters[g_sel] : kSymbols[g_sel];
    one[1] = 0;
    if (g_layer == LAYER_SYMBOLS && g_sel >= NSYMBOLS) return;
    edit_insert(one);
}

/* ==========================================================================
 * Setup
 * ========================================================================== */

/* The usual mapping: colour 0 is the paper and 1, 2, 3 are three darkening
 * inks.  The old build shipped 0xFC, which flattened all three onto black
 * because only one bitplane was ever written; now that both are, the greys are
 * the highlighter and the paper is what a reversed cell fills with. */
#define PAL_BG   0xE4            /* 0 white, 1 light, 2 dark, 3 black */

static const char kSeed[] =
    "10 REM NOTE/GB\n"
    "20 FOR I=1 TO 10\n"
    "30 PRINT I*I\n"
    "40 NEXT I\n"
    "50 END\n";

/* The background map is filled once and never touched again: 32 rows of the
 * eight buffer rows, repeating, which is what makes scrolling a register. */
static void build_map(void)
{
    unsigned char y, x;

    for (y = 0; y < 32; y++) {
        for (x = 0; x < TILE_COLS; x++)
            g_row[x] = (unsigned char)(TILE_TEXT0 + (y & (TEXT_ROWS - 1)) * TILE_COLS + x);
        set_bkg_tiles(0, y, TILE_COLS, 1, g_row);
    }

    /* The window has a tilemap of its own, and nothing has written to it yet:
     * every entry is tile 0, which in this layout is the first cell of the
     * text area.  Left alone, the rows the keyboard does not cover come up
     * showing whatever the top-left of the document says -- a row of "10" from
     * line 10, repeated across the screen.  Blank the whole map once. */
    for (x = 0; x < TILE_COLS; x++) g_row[x] = TILE_BLANK;
    for (y = 0; y < 18; y++) set_win_tiles(0, y, TILE_COLS, 1, g_row);

    /* The status band owns a tile per column for good; only their pixels
     * change after this. */
    for (x = 0; x < TILE_COLS; x++) g_row[x] = (unsigned char)(TILE_STAT + x);
    set_win_tiles(0, WIN_STATUS, TILE_COLS, 1, g_row);

    /* The legend never changes, so it is a map written once: solid fill with
     * thirteen tiles of text set into the middle of it. */
    for (x = 0; x < TILE_COLS; x++)
        g_row[x] = (x >= HINT_X && x < HINT_X + HINT_N)
                 ? (unsigned char)(TILE_HINT + (x - HINT_X)) : TILE_SOLID;
    set_win_tiles(0, WIN_HINT, TILE_COLS, 1, g_row);
}

void main(void)
{
    unsigned char keys, prev = 0, dirs;
    unsigned char blink = 0;

    DISPLAY_OFF;

    BGP_REG  = PAL_BG;

    note_buffer_init(&g_buf, g_text, TEXT_CAP, g_undo, UNDO_RECS,
                     g_utext, UNDO_TEXT, g_lines, LINE_CAP);
    note_buffer_set(&g_buf, (const nchar *)kSeed);
    note_buffer_caret_set(&g_buf, note_buffer_len(&g_buf), 0);

    build_alphabet();
    build_map();
    draw_all_lines();
    draw_keys();
    draw_status();

    move_win(7, WIN_Y);
    SHOW_WIN;
    HIDE_SPRITES;
    SHOW_BKG;
    DISPLAY_ON;

    sel_show();
    place_caret();
    caret_show();

    while (1) {
        keys = joypad();
        dirs = keys & (J_LEFT | J_RIGHT | J_UP | J_DOWN);

        if (keys & J_B) {
            g_bframes++;
            if (dirs && repeating(dirs)) { move_caret(dirs); g_bused = 1; }
        } else {
            /* B was a tap, not a modifier: that is Backspace.  Deciding on
             * release is what lets one button be both. */
            if (prev & J_B) {
                if (!g_bused) edit_backspace();
                g_bused = 0;
                g_bframes = 0;
                g_held = 0;
            } else if (repeating(dirs)) {
                move_sel(dirs);
            }
        }

        if ((keys & J_A) && !(prev & J_A)) do_key();
        if ((keys & J_SELECT) && !(prev & J_SELECT)) {
            sel_hide();
            g_layer = (unsigned char)((g_layer + 1) % LAYER_COUNT);
            g_sel = 0;
            draw_keys();
            sel_show();
        }
        if ((keys & J_START) && !(prev & J_START)) edit_insert("\n");

        draw_status();
        place_caret();

        /* The cursor blinks by being composed with and without its inversion.
         * Both calls are cheap when nothing has changed -- they compare the
         * position they last drew and return -- so this is one tile write
         * twice a second and nothing at all in between. */
        blink++;
        if ((blink & 31) < 22) caret_show();
        else                   caret_hide();

        prev = keys;
        wait_vbl_done();
    }
}
