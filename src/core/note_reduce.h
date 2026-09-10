/* note_reduce.h — a theme on a display that has only so many colours.
 *
 * Some displays do not take a colour, they take a number.  A C64 and a CGA
 * card have sixteen entries burned into a chip; Windows on an adapter with no
 * driver has a sixteen-entry system palette and maps everything else onto it
 * behind the application's back.  Left to itself that mapping is per-colour
 * nearest-match, which is the wrong rule for an editor: a keyword and a
 * comment that were three shades apart both become black, and the reader has
 * lost the distinction the colours existed to carry.
 *
 * So the core does the reduction itself, as an assignment rather than as
 * eight independent lookups.  Distinguishability wins over fidelity: two
 * token kinds that are each a shade off are better than two that are exactly
 * right and identical.
 *
 * Separate from note_theme.c because the two halves are wanted separately: a
 * machine with sixteen colours is usually a machine that cannot afford the
 * registry, which parses note_conf documents into a 192 KB arena.
 */
#ifndef NOTE_REDUCE_H
#define NOTE_REDUCE_H

#include "note_theme.h"

/* Indices are unsigned char, so a palette longer than this could not be
 * addressed by the result.  Nothing note runs on has more entries than a
 * VGA's 256. */
#define NOTE_PAL_MAX 256

/* The same theme, addressed by palette index.  Every field is an index into
 * the `pal` array handed to note_theme_reduce, never an RGB value. */
typedef struct {
    unsigned char bg, fg;
    unsigned char gutter_bg, gutter_fg;
    /* The selection wash, and what a backend that paints a caret-line wash
     * uses for it: both are a band of colour laid behind ordinary text, and
     * both fail the same way — by matching the background. */
    unsigned char sel_bg;
    unsigned char caret;
    unsigned char ui_bg, ui_fg;
    unsigned char tok[TOK_COUNT];

    /* How many entries of `pal` held a colour no earlier entry held.  A
     * palette that repeats itself buys nothing, and this is what the
     * reduction actually had to work with. */
    unsigned char ncolors;
    /* How many distinct colours the eight token kinds ended up sharing
     * between them: 8 when the palette had room, fewer when it did not.
     * Which kinds merged is fixed and documented — see note_reduce.c. */
    unsigned char ntok;
} note_theme_map;

/* Maps `th` onto `pal`, which holds `npal` colours the display can actually
 * show (0xRRGGBB, in the order the hardware numbers them).  `npal` is clamped
 * to [1, NOTE_PAL_MAX].
 *
 * Guarantees, for a palette holding at least two distinct colours:
 *   - fg != bg, gutter_fg != gutter_bg, ui_fg != ui_bg, sel_bg != bg,
 *     caret != bg;
 *   - no token colour is bg;
 *   - two token kinds share an index only when the theme already painted them
 *     the same colour, or when the palette was too small to keep them apart
 *     and the fixed merge order in note_reduce.c gave one of them up.
 * gutter_bg and ui_bg may equal bg; a theme that wants them to differ says so
 * with its foregrounds, and spending a palette entry on a shade of the
 * background nobody can see costs a token kind its colour. */
void note_theme_reduce(note_theme_map *out, const note_theme *th,
                       const note_color *pal, int npal);

/* The two sixteen-colour palettes the ports meet, so that neither backend has
 * to write them down: a display's palette is a fact about the display. */
extern const note_color note_pal_ega16[16];  /* CGA/EGA/VGA index order, and
                                              * the Windows 4bpp system 16  */
extern const note_color note_pal_c64[16];    /* VIC-II, in the C64's order  */

#endif /* NOTE_REDUCE_H */
