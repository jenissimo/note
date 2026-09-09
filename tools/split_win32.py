#!/usr/bin/env python3
"""One-shot: cut note_win32.c into modules.

The backend had grown past three and a half thousand lines, which made it the
bottleneck for anyone working on the editor -- only one person or agent could
touch the Win32 side at a time.  This cuts it along the section boundaries
that were already in the file and lifts the shared state into a header.

The cut is by line range and deliberately dumb: it moves code, it does not
rewrite it.  Anything the compiler then reports as undeclared is a function
that is genuinely used across a boundary, and gets a prototype in the header.

Run once, from the repository root.  Keeps the original as note_win32.c.orig.
"""

import io
import os
import sys

SRC = "src/platform/win32/note_win32.c"
DIR = "src/platform/win32"

# (file, first line, last line, one-line description)  -- 1-based, inclusive.
PARTS = [
    ("win32_edit.c",    299,  745,
     "the RICHEDIT control, the line-number gutter and the highlighter"),
    ("win32_chrome.c",  747,  988,
     "tabs, the status bar, fonts and applying a theme"),
    ("win32_dialogs.c", 990, 1204,
     "the platform's own dialogs: open, save, font, find, print"),
    ("win32_host.c",   1205, 1379,
     "the plain services the core asks for: files, folders, the clock"),
    ("win32_menu.c",   1381, 2131,
     "menus and the tab strip, both drawn by hand from the theme"),
    ("win32_palette.c", 2133, 2923,
     "the command palette overlay and its list modes"),
]

# Everything left over -- the head, the message loop, the window procedure and
# startup -- stays in the module that owns the window.
MAIN = "win32_main.c"

HEADER = "note_win32.h"

BANNER = """/* %s -- %s
 *
 * Part of note's Win32 backend.  The shared state and the types every part of
 * it needs live in note_win32.h; see there for why the state is a header and
 * not a private static.
 */

#include "note_win32.h"

"""

HEADER_TOP = """/* note_win32.h -- state and types shared across the Win32 backend.
 *
 * The backend outgrew a single translation unit, so it is now several, and
 * they all work on one note_host.  That struct would rather have stayed
 * private to one file, but at this size the privacy was already nominal: the
 * gutter, the tab strip, the palette and the message loop all read and write
 * the same window handles, brushes and cached text.  Making the sharing
 * explicit in a header is honest about what was already true.
 *
 * Only declarations belong here.  Anything with a body lives in one of the
 * win32_*.c files.
 */
#ifndef NOTE_WIN32_H
#define NOTE_WIN32_H

"""

HEADER_BOTTOM = """
/* ---- defined in win32_main.c -------------------------------------------- */

extern struct note_host g;

/* Scaling and colour conversion, used by everything that draws. */
COLORREF cr(unsigned rgb);
int      px(int v);
void     read_dpi(note_host *h);
HWND     active_edit(void);

/* Suspends RichEdit's undo around programmatic formatting; see win32_main.c. */
ITextDocument *tom_open(HWND edit);

void relayout(note_host *h);
void update_status(note_host *h);
void measure_font(note_host *h);
void on_find_msg(note_host *h, FINDREPLACEW *fr);

#endif /* NOTE_WIN32_H */
"""


def main():
    if not os.path.exists(SRC):
        print("run me from the repository root")
        return 1

    lines = io.open(SRC, encoding="utf-8", newline="").read().split("\n")
    n = len(lines)

    def take(a, b):
        return "\n".join(lines[a - 1:b])

    # The head of the file, up to the first static definition, becomes the
    # header: includes, ids, the state struct, the undocumented UAH payloads
    # and the owner-draw item data.
    head = take(1, 226)
    head = head.replace(
        "/* note_win32.c — the Win32 backend for note.",
        "/* (moved into note_win32.h)")
    # Drop the original file comment; the header carries its own.
    head = head[head.index("#define WIN32_LEAN_AND_MEAN"):]

    io.open(os.path.join(DIR, HEADER), "w", encoding="utf-8", newline="").write(
        HEADER_TOP + head + "\n" + HEADER_BOTTOM)

    used = set()
    for name, first, last, what in PARTS:
        body = take(first, last)
        io.open(os.path.join(DIR, name), "w", encoding="utf-8", newline="").write(
            (BANNER % (name, what)) + body + "\n")
        used.update(range(first, last + 1))
        print("  %-18s lines %5d-%-5d  %5d lines" % (name, first, last, last - first + 1))

    # Whatever no part claimed, minus the head that became the header.
    rest = [lines[i - 1] for i in range(227, n + 1) if i not in used]
    io.open(os.path.join(DIR, MAIN), "w", encoding="utf-8", newline="").write(
        (BANNER % (MAIN, "the window, the message loop and startup")) +
        "\n".join(rest) + "\n")
    print("  %-18s the remainder      %5d lines" % (MAIN, len(rest)))

    os.rename(SRC, SRC + ".orig")
    print("\noriginal kept as %s.orig" % SRC)
    return 0


if __name__ == "__main__":
    sys.exit(main())
