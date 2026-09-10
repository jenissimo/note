/* note_theme.h — loadable colour themes.
 *
 * Themes are data on the same footing as languages: the built-in light and
 * dark palettes are note_conf documents parsed by note_theme_add(), exactly
 * like a .theme file a user drops into <exe>\themes or
 * %LOCALAPPDATA%\note\themes.
 *
 * Colours are 0xRRGGBB.  Backends convert to whatever their platform wants
 * (Win32 COLORREF is byte-swapped, for instance) — the core never encodes a
 * platform's pixel format.
 */
#ifndef NOTE_THEME_H
#define NOTE_THEME_H

#include "note_core.h"
#include "note_conf.h"
#include "note_syntax.h"

typedef struct {
    const nchar *name;
    int          dark;          /* 1 if this palette is a dark one */
    note_color   bg;            /* editor background   */
    note_color   fg;            /* default text        */
    note_color   gutter_bg;
    note_color   gutter_fg;
    note_color   sel_bg;
    /* The block the caret fills a cell with.  A backend that draws a thin
     * line uses it as the line's colour; the retro ports draw a reversed
     * cell, so the block is this colour and the character it swallows is
     * punched out of it in the background colour.  Defaults to `fg` when a
     * theme does not name one, which is what the console themes already do
     * in practice. */
    note_color   caret;
    note_color   ui_bg;         /* tab strip / status bar */
    note_color   ui_fg;
    note_color   tok[TOK_COUNT];
} note_theme;

void              note_theme_init(note_arena *ar);
int               note_theme_add (note_arena *ar, const nchar *text);
int               note_theme_count(void);
const note_theme *note_theme_get (int i);
/* Index of the first theme whose `dark` flag matches; never negative. */
int               note_theme_for (int dark);
/* Index of the theme with this name, or -1.  Used to restore the user's
 * choice by name, since indices shift as definitions are added. */
int               note_theme_find(const nchar *name);

#endif /* NOTE_THEME_H */
